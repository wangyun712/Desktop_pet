#pragma once
// =============================================================================
//  TrackListModel —— 播放列表的模型（QListView 的数据源）
//
//  ★ 为什么用 model/view，而不是往 QListWidget 里 addItem ★
//    用户的文件夹里可能有几万首。QListWidget 是"每个条目一个真实的 QWidget"，
//    几万行就是几万个控件 —— 建列表能卡十几秒、滚动也发涩。
//    QListView + 自定义 model 是"只建看得见的那几十行"，几万行毫无压力。
//
//  ★ 它不持有数据，只是"一层视图" ★
//    真正的曲目在 MusicLibrary 里。这里保存的是 m_view —— 一串**曲库下标**，
//    也就是"筛完之后要显示哪些、按什么顺序显示"。
//    搜索就是把 m_view 换一批（setView），曲库本身完全不动 ——
//    所以清空搜索框能立刻恢复全部，不用重新扫描磁盘。
//
//    这一层间接还有个好处：列表行号 ≠ 曲库下标。用户筛过之后点第 3 行，
//    到底对应哪首歌由 m_view 说了算，界面那边不用自己换算。
//
//  ★ 两段式列表：本地在前、联网结果在后 ★
//    联网搜索的结果（TrackListModel::OnlineTrack）追加在本地行**之后**，
//    行号 >= 本地行数的就是联网行 —— 它们没有曲库下标、没有本地文件，
//    双击走"下载到临时目录再播"的联网路径（PlayerPage 负责下载和转码）。
//    TagRole 给出每行的来源徽标：本地 / 联网·网易 / 联网·B站 / 下载中 42%。
//    本地行的徽标可用 setLibraryTag 覆盖（联网下载的临时文件标"在线"）。
// =============================================================================

#include <QAbstractListModel>
#include <QHash>
#include <QVector>

class MusicLibrary;

class TrackListModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Role {
        TitleRole = Qt::UserRole + 1,   // 标题
        ArtistRole,                     // 艺术家（可能为空）
        DurationRole,                   // 时长（毫秒，0 = 未知）
        PathRole,                       // 绝对路径（联网行 = 来源 id）
        LibraryIndexRole,               // 它在 MusicLibrary::tracks() 里的下标（联网行 = -1）
        IsPlayingRole,                  // 是不是"正在播的那一首"（自绘 delegate 靠它画高亮）
        TagRole,                        // 来源徽标：本地 / 联网·网易 / 下载中 42% …
    };

    // 联网搜索结果的一行（来自 OnlineMusic，这里是它自己的轻量副本）
    struct OnlineTrack
    {
        QString id;             // 网易歌曲 id / B站 bvid
        int     source = 0;     // 0 = 网易，1 = B站
        QString title;
        QString artist;
        QString album;
        qint64  durationMs = 0;
        QString tag;            // 徽标文字（"联网·网易"），下载中会被改成"下载中 xx%"
    };

    explicit TrackListModel(MusicLibrary* lib, QObject* parent = nullptr);

    int      rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

    // 换一批要显示的本地行（传的是曲库下标）。搜索、清空搜索、扫描出新歌都走它。
    void setView(const QVector<int>& libraryIndexes);

    int  libraryIndexAt(int row) const;         // 行号 -> 曲库下标（联网行/越界给 -1）
    int  rowOfLibraryIndex(int libIndex) const; // 曲库下标 -> 行号（不在视图里给 -1）

    void setPlayingLibraryIndex(int libIndex);
    int  playingLibraryIndex() const { return m_playing; }

    // ---- 联网结果区（追加在本地行之后，PlayerPage 管理）----
    void setOnlineResults(const QVector<OnlineTrack>& items);   // 整体替换联网区
    void clearOnlineResults();
    void setOnlineTag(int row, const QString& tag);             // 下载进度 → 徽标文字
    bool isOnlineRow(int row) const;                            // 是不是联网行
    OnlineTrack onlineAt(int row) const;                        // 联网行内容（本地行给空）

    // 流式播放的联网行高亮（那行没有曲库下标，和本地行的高亮机制分开）。
    // setPlayingLibraryIndex 会自动清掉它 —— 本地歌开播时联网高亮让位。
    void setPlayingOnlineId(const QString& id);
    QString playingOnlineId() const { return m_playingOnlineId; }   // 正在流式播的 id
    int  onlineRowOfId(const QString& id) const;    // 联网行反查（按 id，没有给 -1）

    // 本地行的徽标覆盖（联网下载的临时文件标"在线"等）。
    // ★ 覆盖按**路径**记 ★ —— 删除某首歌后，后面歌曲的下标会整体前移，
    // 按下标记的标签会串位；按路径记则天然稳定。
    void setLibraryTag(int libIndex, const QString& tag);
    int  localRowCount() const { return m_view.size(); }

private:
    MusicLibrary* m_lib = nullptr;
    QVector<int>  m_view;              // 显示顺序：元素是曲库下标（本地区）
    QVector<OnlineTrack> m_online;     // 联网结果区（排在本地区之后）
    QString       m_playingOnlineId;   // 流式播放中那首联网歌的 id（高亮用）
    QHash<QString, QString> m_libTags; // 本地行的徽标覆盖（路径 → 徽标），缺省"本地"
    int           m_playing = -1;
};
