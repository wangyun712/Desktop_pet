#pragma once
// =============================================================================
//  PlayerPage —— 主面板里的「播放器」页
//
//  形态：上面一条工具行（选文件夹 / 搜索 / 计数），左边是曲目列表，
//        右边是歌词，底下是播放条。没有封面时整页就是干净的白色；
//        有封面时封面会**淡化**成整页的背景（透明度很低，
//        为的是"一眼看出是哪张专辑"，而不是抢文字的可读性）。
//
//  ★ 这一页不碰桌宠、不碰好感度 ★（用户明确要求"不跟着"）
//    它就是一块普通的播放器界面：听歌归听歌，桌宠该干嘛干嘛。
//    所以这里既没有对 DesktopPet 的信号，也没有对 AffectionSystem 的调用。
//
//  ★ 界面 / 数据 / 播放三件事是分开的 ★
//      PlayerPage   —— 只管显示和交互
//      MusicLibrary —— 只管"有哪些歌"，跑在后台线程里扫
//      AudioPlayer  —— 只管"把声音放出来"
//    所以这一页里看不到任何解码、线程、标签解析的代码，全是调用。
//
//  ---------------------------------------------------------------------------
//  ▎界面上几个不那么显然的决定
//
//  · 进度条是**自绘的 SeekSlider**：Qt 默认的 QSlider 点一下只走一页
//    （pageStep），要跳到哪里得先拖那个小圆点，很别扭。播放器必须"点哪跳哪"。
//
//  · 拖动进度条时**不立刻 seek**，只在松手时 seek 一次：
//    拖动过程中每移动一个像素就 seek 会不断打断解码器，
//    表现为"拖的时候声音一顿一顿的"。拖的时候只改时间文字，松手才动播放器。
//
//  · 歌词跟着唱：找一个"时间 ≤ 当前播放位置"的最后一行（二分），
//    高亮它并滚动到可视区中间。用二分是因为一首歌几百行、每 200ms 找一次。
//
//  · 搜索有 250ms 防抖：每打一个字就全库过滤一次，几万首的库会让输入框发涩。
//    停手 250ms 再搜，配合 MusicLibrary 的"前缀收窄"，打字体验是跟手的。
// =============================================================================

#include <QVector>
#include <QWidget>

#include "MusicLibrary.h"
#include "TrackMeta.h"

class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QSlider;
class QScrollArea;
class QVBoxLayout;
class QTimer;

class AudioPlayer;
class TrackListModel;
class SeekSlider;
class PlayerIconButton;

class PlayerPage : public QWidget
{
    Q_OBJECT
public:
    explicit PlayerPage(QWidget* parent = nullptr);
    ~PlayerPage() override;

    // 给 --selftest 用：把标签解析、搜索、扫描各跑一遍并写成报告。
    // 纯只读（只在自己创建的临时目录里写样本文件，跑完就删）。
    static QString describePlayer();

    // 界面字号档位变了之后重新套样式表（见 UiFont.h）。
    // 歌词那边除了样式表还有布局间距，所以不能只靠 QSS —— 这条一起改掉。
    void applyUiScale();

    // 给 --selftest 用：直接塞几首假曲目进来，好让截图里有东西可看。
    // （走的是 MusicLibrary 的正式入口，不是另开一条后门。）
    void debugInjectTracks(const QVector<Track>& tracks);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private slots:
    void onPickFolder();
    void onSearchChanged();
    void doSearch();
    void onRowActivated(const QModelIndex& index);

    void onPositionChanged(qint64 ms);
    void onDurationChanged(qint64 ms);
    void onPlayerStateChanged();
    void onTrackFinished();

    void onModeClicked();            // 播放模式：点一下换一种

    void onScanStarted();
    void onScanBatch();
    void onScanFinished(int total, int skipped);

    void onSeekPressed();
    void onSeekMoved(int value);
    void onSeekReleased();

private:
    void buildUi();
    void applyStyle();
    void applyMode();                // 把 m_mode 刷到模式键的图标和 tooltip 上
    int  randomOtherRow(int curRow, int count) const;   // 随机挑一行（不会挑到 curRow）

    void playLibraryIndex(int libIndex);
    void playRow(int row);
    void playPrev();
    void playNext();
    void togglePlayPause();

    void loadLyricsAndCover(const QString& path);
    void rebuildLyricLabels(const QVector<TrackMeta::LyricLine>& lines);
    void clearLyrics(const QString& message);
    void highlightLyric(int index);

    void updateNowPlaying();
    void updateCountLabel();

    static QString formatMs(qint64 ms);

    // ---- 数据 ----
    MusicLibrary*   m_lib    = nullptr;
    TrackListModel* m_model  = nullptr;
    AudioPlayer*    m_player = nullptr;

    // ---- 工具行 ----
    QPushButton* m_pickBtn    = nullptr;
    QLineEdit*   m_searchEdit = nullptr;
    QLabel*      m_countLabel = nullptr;

    // ---- 列表 ----
    QListView* m_list = nullptr;

    // ---- 歌词 ----
    QLabel*          m_lyricHeader = nullptr;
    QScrollArea*     m_lyricScroll = nullptr;
    QWidget*         m_lyricHost   = nullptr;
    QVBoxLayout*     m_lyricLay    = nullptr;
    QVector<QLabel*> m_lyricLabels;
    QVector<TrackMeta::LyricLine> m_lyricLines;
    int              m_lyricCurrent = -1;

    // ---- 播放条 ----
    QLabel*      m_coverLabel = nullptr;
    QLabel*      m_nowLabel   = nullptr;
    PlayerIconButton* m_modeBtn = nullptr;   // 播放模式（随机 / 顺序 / 单曲循环）
    PlayerIconButton* m_prevBtn = nullptr;
    PlayerIconButton* m_playBtn = nullptr;
    PlayerIconButton* m_nextBtn = nullptr;
    SeekSlider*  m_slider     = nullptr;
    QLabel*      m_posLabel   = nullptr;
    QLabel*      m_durLabel   = nullptr;
    QSlider*     m_volume     = nullptr;

    // ---- 播放模式 ----
    //   ★ 这三个值要塞进 QSettings，**顺序就是存档含义** ★
    //     以后要加"列表循环"之类的新模式，只能往后追加，不能插在中间、也不能调顺序。
    //   枚举放在这一页里而不是 AudioPlayer 里：换哪种顺序属于"界面上的播放策略"，
    //   AudioPlayer 只管把一首歌放出来 —— 它连"还有没有下一首"都不知道。
    enum class PlayMode { Shuffle = 0, Order = 1, RepeatOne = 2 };
    PlayMode m_mode = PlayMode::Order;       // 默认顺序播放（和主流播放器一致）

    QTimer* m_searchTimer = nullptr;

    // ---- 背景封面 ----
    QPixmap m_bgCover;         // 原始封面（已缩到一个合理尺寸）
    QPixmap m_bgCache;         // 铺满窗口那一版，缓存下来免得每次重绘都重缩
    QSize   m_bgCacheSize;

    // ---- 状态 ----
    bool m_userSeeking = false;    // 正拖着进度条：期间不接受播放器回报的位置
    bool m_autoLoaded  = false;    // "上次那个文件夹"只自动加载一次
    int  m_playingLib  = -1;       // 正在播的那首在曲库里的下标
};
