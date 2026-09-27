#include "TrackListModel.h"

#include "MusicLibrary.h"

TrackListModel::TrackListModel(MusicLibrary* lib, QObject* parent)
    : QAbstractListModel(parent), m_lib(lib)
{
}

int TrackListModel::rowCount(const QModelIndex& parent) const
{
    // 列表模型：只有顶层有行，任何有效 parent 下的行数都是 0
    if (parent.isValid())
        return 0;
    return m_view.size() + m_online.size();
}

QVariant TrackListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0)
        return QVariant();

    const int row = index.row();

    // ---- 联网结果区（排在本地区之后）----
    if (row >= m_view.size())
    {
        const int on = row - m_view.size();
        if (on >= m_online.size())
            return QVariant();

        const OnlineTrack& t = m_online.at(on);
        switch (role)
        {
        case Qt::DisplayRole:
        case TitleRole:
            return t.title;
        case ArtistRole:
            return t.artist;
        case DurationRole:
            return t.durationMs;
        case PathRole:
            return t.id;               // 联网行没有本地路径：放来源 id（网易 id / bvid）
        case LibraryIndexRole:
            return -1;
        case IsPlayingRole:
            return t.id == m_playingOnlineId;   // 流式播放中：高亮那行联网结果
        case TagRole:
            return t.tag;
        default:
            return QVariant();
        }
    }

    // ---- 本地区 ----
    const int    libIndex = m_view.at(row);
    const Track* t = m_lib ? m_lib->trackAt(libIndex) : nullptr;
    if (!t)
        return QVariant();

    switch (role)
    {
    case Qt::DisplayRole:
    case TitleRole:
        return t->title;

    case ArtistRole:
        return t->artist;

    case DurationRole:
        return t->durationMs;

    case PathRole:
        return t->path;

    case LibraryIndexRole:
        return libIndex;

    case IsPlayingRole:
        return libIndex == m_playing;

    case TagRole:
        return m_libTags.value(t->path, QStringLiteral("本地"));

    default:
        return QVariant();
    }
}

void TrackListModel::setView(const QVector<int>& libraryIndexes)
{
    // 整体换一批：用 reset 最简单也最不容易出错（选择会被清掉，
    // 而"搜索结果变了"本来也就该清掉旧选择）。
    // ★ 联网区不受影响 ★ —— 本地搜索刷新不该把还没看完的联网结果抹掉
    //（联网结果的增删由 setOnlineResults/clearOnlineResults 负责）。
    beginResetModel();
    m_view = libraryIndexes;
    endResetModel();
}

int TrackListModel::libraryIndexAt(int row) const
{
    if (row < 0 || row >= m_view.size())
        return -1;
    return m_view.at(row);
}

int TrackListModel::rowOfLibraryIndex(int libIndex) const
{
    if (libIndex < 0)
        return -1;
    // 线性找。列表最多几万行、且只在切歌时调一次，不值得为它再维护一张反查表
    //（那张表在每次 setView 之后都要重建，反而更贵）。
    for (int row = 0; row < m_view.size(); ++row)
        if (m_view.at(row) == libIndex)
            return row;
    return -1;
}

void TrackListModel::setPlayingLibraryIndex(int libIndex)
{
    if (m_playing == libIndex)
        return;

    const int oldRow = rowOfLibraryIndex(m_playing);
    m_playing = libIndex;
    const int newRow = rowOfLibraryIndex(m_playing);

    // 本地歌开播：联网行的高亮让位（流式歌切到本地歌时清掉旧高亮）
    if (!m_playingOnlineId.isEmpty())
    {
        const int r = onlineRowOfId(m_playingOnlineId);
        m_playingOnlineId.clear();
        if (r >= 0)
            emit dataChanged(index(r), index(r), { IsPlayingRole });
    }

    // 只更新变化的那两行，别整个 model 重置 —— 否则列表会闪一下、
    // 滚动位置也会跳。
    const QVector<int> roles{ IsPlayingRole };
    if (oldRow >= 0)
        emit dataChanged(index(oldRow), index(oldRow), roles);
    if (newRow >= 0 && newRow != oldRow)
        emit dataChanged(index(newRow), index(newRow), roles);
}

// =============================================================================
//  联网结果区
// =============================================================================
void TrackListModel::setPlayingOnlineId(const QString& id)
{
    if (m_playingOnlineId == id)
        return;

    const int oldRow = onlineRowOfId(m_playingOnlineId);
    m_playingOnlineId = id;
    const int newRow = onlineRowOfId(m_playingOnlineId);

    const QVector<int> roles{ IsPlayingRole };
    if (oldRow >= 0)
        emit dataChanged(index(oldRow), index(oldRow), roles);
    if (newRow >= 0 && newRow != oldRow)
        emit dataChanged(index(newRow), index(newRow), roles);
}

int TrackListModel::onlineRowOfId(const QString& id) const
{
    if (id.isEmpty())
        return -1;
    for (int row = 0; row < m_online.size(); ++row)
        if (m_online.at(row).id == id)
            return row;
    return -1;
}
void TrackListModel::setOnlineResults(const QVector<OnlineTrack>& items)
{
    // 联网区整体替换：本地行数变了（新搜索），联网行的行号基线跟着移，
    // 所以这里也走一次整体 reset —— 联网搜索本来就是低频操作。
    beginResetModel();
    m_online = items;
    endResetModel();
}

void TrackListModel::clearOnlineResults()
{
    if (m_online.isEmpty())
        return;
    beginResetModel();
    m_online.clear();
    endResetModel();
}

void TrackListModel::setOnlineTag(int row, const QString& tag)
{
    if (row < m_view.size() || row - m_view.size() >= m_online.size())
        return;
    m_online[row - m_view.size()].tag = tag;
    emit dataChanged(index(row), index(row), { TagRole });
}

bool TrackListModel::isOnlineRow(int row) const
{
    return row >= m_view.size() && row - m_view.size() < m_online.size();
}

TrackListModel::OnlineTrack TrackListModel::onlineAt(int row) const
{
    OnlineTrack empty;
    if (!isOnlineRow(row))
        return empty;
    return m_online.at(row - m_view.size());
}

void TrackListModel::setLibraryTag(int libIndex, const QString& tag)
{
    const Track* t = m_lib ? m_lib->trackAt(libIndex) : nullptr;
    if (!t)
        return;
    m_libTags.insert(t->path, tag);
    const int row = rowOfLibraryIndex(libIndex);
    if (row >= 0)
        emit dataChanged(index(row), index(row), { TagRole });
}
