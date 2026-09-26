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
    return m_view.size();
}

QVariant TrackListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_view.size())
        return QVariant();

    const int    libIndex = m_view.at(index.row());
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

    default:
        return QVariant();
    }
}

void TrackListModel::setView(const QVector<int>& libraryIndexes)
{
    // 整体换一批：用 reset 最简单也最不容易出错（选择会被清掉，
    // 而"搜索结果变了"本来也就该清掉旧选择）。
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

    // 只更新变化的那两行，别整个 model 重置 —— 否则列表会闪一下、
    // 滚动位置也会跳。
    const QVector<int> roles{ IsPlayingRole };
    if (oldRow >= 0)
        emit dataChanged(index(oldRow), index(oldRow), roles);
    if (newRow >= 0 && newRow != oldRow)
        emit dataChanged(index(newRow), index(newRow), roles);
}
