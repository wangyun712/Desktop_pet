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
// =============================================================================

#include <QAbstractListModel>
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
        PathRole,                       // 绝对路径
        LibraryIndexRole,               // 它在 MusicLibrary::tracks() 里的下标
        IsPlayingRole,                  // 是不是"正在播的那一首"（自绘 delegate 靠它画高亮）
    };

    explicit TrackListModel(MusicLibrary* lib, QObject* parent = nullptr);

    int      rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

    // 换一批要显示的行（传的是曲库下标）。搜索、清空搜索、扫描出新歌都走它。
    void setView(const QVector<int>& libraryIndexes);

    int  libraryIndexAt(int row) const;         // 行号 -> 曲库下标（越界给 -1）
    int  rowOfLibraryIndex(int libIndex) const; // 曲库下标 -> 行号（不在视图里给 -1）

    void setPlayingLibraryIndex(int libIndex);
    int  playingLibraryIndex() const { return m_playing; }

private:
    MusicLibrary* m_lib = nullptr;
    QVector<int>  m_view;      // 显示顺序：元素是曲库下标
    int           m_playing = -1;
};
