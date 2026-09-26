#pragma once
// =============================================================================
//  MusicLibrary —— 曲库：扫文件夹 → 建索引 → 搜索
//
//  职责边界（和 SongLibrary 不是一回事，别混）：
//    · SongLibrary（聊天用）—— resources/songs/*.txt，是"台词素材"
//    · MusicLibrary（这一份）—— 用户自己挑的文件夹里的**真音频文件**，
//      是"能放出声的东西"
//    名字像、用途完全不同，所以文件也分开。真要合并，代价是聊天页
//    要依赖整个音频扫描器，得不偿失。
//
//  ---------------------------------------------------------------------------
//  ▎为什么扫描要放后台线程
//
//  用户挑的可能是"我的音乐"这种几万首的大文件夹，也可能是挂着机械硬盘的
//  整个盘符。同步扫的话界面会白屏几秒到几十秒 —— 用户只会以为程序死了。
//  所以：QDirIterator 在工作线程里跑，主线程只负责"把结果追加进列表"。
//
//  数据流是单向的：
//      工作线程  ──(每 64 首一批)──▶  scanBatch  ──▶  主线程 append + 发信号
//      工作线程  ──(每 250ms 一次)──▶  scanProgress
//      工作线程  ────────────────▶  scanFinished
//  工作线程**从来不碰** m_tracks，那些信号在主线程被执行时才改数据。
//  这样"列表正在更新时用户点了播放"这类竞态根本不存在 —— 因为只有一个线程
//  在动它。
//
//  ---------------------------------------------------------------------------
//  ▎搜索为什么快（"高效"这个词的落点）
//
//  ① 预归一化索引：扫描时就把「标题+艺术家+专辑」拼成一个串、去标点、转小写，
//     一次性存好（m_normKeys）。搜索时不再对每首歌做字符串处理，
//     只剩一次纯扫描 —— 这是把 O(n·m) 里的 m 干掉。
//  ② 前缀收窄：新输入如果是上次输入往后多打了几个字（"周杰"→"周杰伦"），
//     只在上一次的结果里筛 —— 上一次已经很小了，这一步几乎是免费的。
//     这就是"边打字边过滤"能跟得上的原因。
//  ③ 前缀优先排序：命中位置在第 0 个字符的排前面。搜"晴天"时，
//     《晴天》排在《我在晴天想起你》前面 —— 不用额外的评分函数，看位置就够。
//
//  归一化规则直接复用聊天的 ChatScript::normalize（去空白、去标点、
//  全角转半角、转小写）—— 不另写一套，省得两处行为不一致。
// =============================================================================

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include "ChatScript.h"

class QThread;
class ScanThread;

// -----------------------------------------------------------------------------
//  列表里一行要用的全部信息
//
//  ★ 刻意**不放**封面和歌词正文 ★
//    封面动辄几百 KB、歌词几 KB，几万首全塞进内存就是几百 MB。
//    这里只留两个"有没有"的标记，真正的数据等用户点到那一首时再读一次。
// -----------------------------------------------------------------------------
struct Track
{
    QString path;              // 绝对路径（磁盘上真实的位置）
    QString title;             // 标签里的标题；没有就从文件名猜
    QString artist;            // 可为空
    QString album;             // 可为空
    int     durationMs = 0;    // 0 = 没算出来（界面显示 "--:--"）
    bool    hasCover   = false;
    bool    hasLyrics  = false;
};

Q_DECLARE_METATYPE(Track)

class MusicLibrary : public QObject
{
    Q_OBJECT
public:
    explicit MusicLibrary(QObject* parent = nullptr);
    ~MusicLibrary() override;

    // 换一个根目录开始扫。会先把上一次的扫描取消掉、清空旧索引。
    void scanAsync(const QString& rootDir);
    void cancelScan();
    bool isScanning() const { return m_scanning; }
    QString rootDir() const { return m_root; }

    void clear();

    const QVector<Track>& tracks() const { return m_tracks; }
    int  count() const { return m_tracks.size(); }
    const Track* trackAt(int index) const;

    // 返回的是**曲库下标**（不是列表行号）。空查询 = 全部（按扫描顺序）。
    QVector<int> search(const QString& query, int limit = 3000) const;

    // 自检用：索引占了多少内存、上次搜索用了多久
    QString describeIndex() const;

    static QStringList audioSuffixes();          // 认哪些后缀算音频
    static QString normalizeForSearch(const QString& in);

signals:
    void scanStarted();
    void scanProgress(int filesSeen, int found, const QString& currentDir);
    void scanBatch(const QVector<Track>& batch);
    void scanFinished(int total, int skipped);

public:
    // ★ 下面三个是给工作线程用的内部入口，界面别调 ★
    //   工作线程用 QMetaObject::invokeMethod(..., Qt::QueuedConnection) 把结果投递到
    //   主线程，实际执行发生在主线程的事件循环里 —— 所以这几个函数体里
    //   "只改数据、不发耗时操作"，永远不阻塞界面。
    //   之所以是 public 而不是 private + friend：lambda 闭包不继承 friend 权限，
    //   各家编译器表现不一致，走 public 最省心（函数名已经写明用途了）。
    void appendBatch(const QVector<Track>& batch);
    void finishScan(int total, int skipped);

private:
    static QString buildKey(const Track& t);

    QThread*    m_thread = nullptr;
    ScanThread* m_worker = nullptr;

    QVector<Track> m_tracks;
    QStringList    m_normKeys;    // 与 m_tracks 一一对应的归一化搜索串

    QString m_root;
    bool    m_scanning = false;

    // "前缀收窄"用的上一次结果。mutable 是为了让 search() 保持 const 语义 ——
    // 缓存对调用方是不可见的，改它不算改对象状态。
    mutable QString      m_lastQuery;
    mutable QVector<int> m_lastResult;
};
