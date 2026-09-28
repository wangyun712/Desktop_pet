#pragma once
// =============================================================================
//  PlayerPage —— 主面板里的「播放器」页
//
//  形态：上面一条工具行（选文件夹 / 搜索 / 计数），左边是曲目列表，
//        右边是歌词，底下是播放条。封面来源有两档：
//          ① 歌曲自带内嵌封面 → 照常读它（原有逻辑，一行没动）；
//          ② 没有内嵌封面 → 拿用户在主题设置里导入的那张背景图垫上
//            （UiTheme::bgImagePath，见 applyFallbackCover）；
//          ③ 连主题图都没设 → 干净的白色，什么都不垫。
//        不管哪档来的，封面都会**淡化**成整页的背景（透明度很低，
//        为的是"一眼看出是哪张专辑/什么氛围"，而不是抢文字的可读性）。
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
#include "TrackListModel.h"     // 联网行签名用到 TrackListModel::OnlineTrack（嵌套类型要完整定义）
#include "TrackMeta.h"

class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QPropertyAnimation;
class QSlider;
class QScrollArea;
class QVBoxLayout;
class QTimer;

class AudioPlayer;
class DesktopLyrics;
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

    // 主题背景图换了之后由 DesktopPet 喊一声：当前这首歌正垫着主题图的话，
    // 把垫的图换成新的。歌曲有自己的封面时这一声什么都不做（见 m_coverFromTheme）。
    void refreshFallbackCover();

    // 桌面歌词开关（设置页那个勾选框 → DesktopPet 接过来）。
    // 立刻生效并落盘；开着的时候悬浮窗跟 Playback 走 —— 有歌词且在唱就显示。
    void setDesktopLyricsEnabled(bool on);

    // 拖到桌宠身上的音乐文件 → 入列并从第一首开始播（DesktopPet::musicFilesDropped）。
    // 路径已在桌宠那边按扩展名过滤过；这边再挡一道文件不存在。
    void playDroppedFiles(const QStringList& paths);

    // ---- 联网搜索 / 收藏 / 临时播放 ----
    void doOnlineSearch(const QString& keyword);   // 网易云 → 空/全收费自动转 B 站
    void playOnlineRow(int row);                   // 双击联网行：下载→（转码）→播放
    void fetchAndPlay(int row, const TrackListModel::OnlineTrack& it,
                      const QString& dir, bool registerFav, bool play);
    void finishOnlineTrack(const TrackListModel::OnlineTrack& it, const QString& path,
                           const QString& dir, bool registerFav, bool play,
                           bool refreshList = true);
    void playLocalWav(const TrackListModel::OnlineTrack& it, const QString& wavPath,
                      bool registerFav, bool play);   // B站转好的 WAV：入库 + 播放
    // 网易搜不到 / 收费时的回退：B 站搜同关键词，结果替换联网区；
    // play = true 时（双击的那首收费）自动接档 B 站第一条。
    void fallbackBilibili(const QString& keyword, const QString& dir,
                          bool registerFav, bool play);

    // ---- 我的下载（储存盘整个目录扫描出来，可删除）----
    // ★ 不存元数据索引 ★ 每次打开都重新扫描储存盘：文件系统就是唯一的账本，
    // 人为改名/挪动/增删文件之后这里照样能找到（改的是哪份账就是哪份）。
    void showDownloads(bool on);                   // 我的下载视图
    void removeDownloaded(int libIndex);           // 删除：删文件（.lrc 侧车一起）+ 出曲库
    bool ensureSaveDir();                          // 没选过储存盘就弹目录框（记住）
    void showFavorites(bool on);                   // 「我的收藏」视图
    void addFavorite(const QString& title, const QString& artist, const QString& path);
    void onListMenu(const QPoint& pos);            // 列表右键：收藏 / 仅下载

    // 给 --selftest 用：直接塞几首假曲目进来，好让截图里有东西可看。
    // （走的是 MusicLibrary 的正式入口，不是另开一条后门。）
    // ★ 注意它必须在下面的 signals: 之前 ★ —— 放进 signals 段会被 moc
    //   当成信号生成实现，和 .cpp 里的定义撞成重定义（踩过一次）。
    void debugInjectTracks(const QVector<Track>& tracks);

signals:
    // 桌面歌词上三连击 → 唤出主面板。悬浮窗是 PlayerPage 的私有成员，
    // DesktopPet 接不到它，所以这里转一手（signal-to-signal）。
    void desktopLyricsPanelRequested();

    // 换歌了（%1 = 歌名）。只在 playLibraryIndex 这条"真的开始播一首新歌"
    // 的路上发 —— 暂停/恢复/seek 都不发。DesktopPet 拿它给桌面歌词报歌名。
    void currentTrackChanged(const QString& title);

    // 桌面歌词的开关状态变了（设置页勾选 / 悬浮窗上的 × 关闭都会发）。
    // DesktopPet 拿它去同步设置页的勾选框 —— × 关闭是反向路径，勾选框不能留旧状态。
    void desktopLyricsEnabledChanged(bool on);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;   // 歌词区的滚轮接管

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
    void applyFallbackCover();       // 这首没内嵌封面：拿主题背景图顶上（没设就空着）
    QPixmap themeFallbackCover();    // 读主题背景图（按路径缓存，换图才重读）
    void rebuildLyricLabels(const QVector<TrackMeta::LyricLine>& lines);
    void clearLyrics(const QString& message);
    void highlightLyric(int index);

    // ---- 歌词平滑滚动（学网易云的那套）----
    //   · 当前行变化 → 缓动动画滚到"行中心 = 可视区中心"，不再瞬间跳变；
    //   · 用户滚轮/拖滑条 → 自动跟随暂停，浮出「回到当前歌词」按钮；
    //   · 几秒没操作自动恢复跟随，点按钮立即归位；暂停期间高亮照常走，只是不抢滚动。
    //   · 双击某句歌词 → 进度跳到那一句（seekToLyric），跳完恢复自动跟随。
    int  lyricTargetValue(QLabel* line) const;   // 这一行滚到居中时滚动条该在的值
    void animateLyricTo(int target);             // 缓动滚过去（同方向连播也顺滑）
    void pauseLyricFollow();                     // 用户接管滚动
    void resumeLyricFollow();                    // 恢复自动跟随（按钮/超时共用）
    void placeLyricRecenterBtn();                // 悬浮按钮贴着歌词区右下摆位
    void seekToLyric(int index);                 // 双击歌词行：进度跳到这一句

    // 把"当前句/下一句"喂给桌面歌词悬浮窗。显隐原则：开关关了/没歌 →
    // 喂空 = 隐藏；有歌没歌词 → 喂"暂无歌词"占位（窗口不消失）；
    // 有歌词但没唱到第一句 → 先垫第一句；正常跟唱 → 当前行 + 下一行。
    // 在高亮行变化、清歌词、开关切换这三处都要喊一声。
    void updateDesktopLyrics();

    void updateNowPlaying();
    void updateCountLabel();

    static QString formatMs(qint64 ms);

    // ---- 数据 ----
    MusicLibrary*   m_lib    = nullptr;
    TrackListModel* m_model  = nullptr;
    AudioPlayer*    m_player = nullptr;

    // ---- 工具行 ----
    QPushButton* m_pickBtn    = nullptr;
    QPushButton* m_favBtn     = nullptr;   // 「我的收藏」切换（收藏视图 / 全部列表）
    QString      m_saveDir;              // 用户选的储存盘（ui.ini player/saveDir）
    QPushButton* m_dlBtn      = nullptr;   // 「我的下载」切换（下载视图 / 全部列表）
    bool         m_dlView     = false;    // 正在显示「我的下载」视图
    QPushButton* m_srcNetease = nullptr;   // 曲源选择：网易云
    QPushButton* m_srcBili    = nullptr;   // 曲源选择：B站
    int          m_onlineSource = 0;       // 联网搜索曲源（0 = 网易云，1 = B站）
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
    QPushButton* m_dtLyricsBtn = nullptr;    // 「词」：桌面歌词开关（在上一首左边，开/关同设置页联动）
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
    QTimer* m_onlineTimer = nullptr;       // 联网搜索防抖（600ms，独立于本地 250ms）
    QString m_onlineKeyword;               // 上次联网搜索词（过期回调丢弃）
    bool    m_favView     = false;         // 正在显示「我的收藏」视图

    // ---- 背景封面 ----
    QPixmap m_bgCover;         // 原始封面（已缩到一个合理尺寸）
    QPixmap m_bgCache;         // 铺满窗口那一版，缓存下来免得每次重绘都重缩
    QSize   m_bgCacheSize;

    // ---- 歌词平滑滚动 ----
    QPropertyAnimation* m_lyricAnim        = nullptr;  // 滚动条 value 的缓动动画
    bool     m_lyricFollow = true;         // false = 用户手动滚开了，暂停自动跟随
    QTimer*  m_lyricFollowTimer = nullptr; // 用户停止操作 N 秒后自动恢复跟随
    QPushButton* m_lyricRecenterBtn = nullptr; // 「回到当前歌词」悬浮按钮（暂停时出现）

    // ---- 桌面歌词（悬浮窗本体见 DesktopLyrics）----
    DesktopLyrics* m_desktopLyrics   = nullptr;  // 独立顶层窗口，随播放内容显隐
    bool           m_dtLyricsEnabled = false;    // 设置页那个开关的内存值

    // ---- 主题图兜底（这首歌没有内嵌封面时垫的那张）----
    //   ★ m_coverFromTheme 记的是"当前垫的这张是谁的" ★
    //     主题图换了要分辨"该不该跟着换"：垫的是主题图才跟着换，
    //     歌曲自己的封面不该被主题图顶掉。
    bool    m_coverFromTheme  = false;
    QPixmap m_themeCover;     // 主题图缩好的那份（不随换歌重来）
    QString m_themeCoverPath;  // m_themeCover 是从哪张图缩出来的（路径变了才重读）

    // ---- 状态 ----
    bool m_userSeeking = false;    // 正拖着进度条：期间不接受播放器回报的位置
    bool m_autoLoaded  = false;    // "上次那个文件夹"只自动加载一次
    int  m_playingLib  = -1;       // 正在播的那首在曲库里的下标
};
