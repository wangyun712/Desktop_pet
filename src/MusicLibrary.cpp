#include "MusicLibrary.h"

#include "TrackMeta.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMetaObject>
#include <QSet>
#include <QThread>

#include <atomic>
#include <utility>

namespace {

constexpr int  BATCH_SIZE      = 64;     // 攒够多少首往主线程送一批
constexpr int  PROGRESS_EVERY  = 250;    // 进度信号的最小间隔（毫秒）
constexpr int  SEARCH_DEFAULT_LIMIT = 3000;

} // namespace

// =============================================================================
//  ScanThread —— 真正在后台跑的那个循环
//
//  ★ 它从来不碰 MusicLibrary 的成员数据 ★
//    扫到东西就攒进本地 batch，攒够一批用 invokeMethod 投递到主线程，
//    由主线程去 append。所以"追加列表"这件事永远只有一个线程在做。
// =============================================================================
class ScanThread : public QThread
{
public:
    ScanThread(MusicLibrary* owner, QString root)
        : m_owner(owner), m_root(std::move(root)) {}

    // 取消：只置一个原子标志。run() 在每一轮循环开头检查它 ——
    // QDirIterator 取下一个文件名很快，所以最多等一轮（毫秒级）就退出了。
    void cancel() { m_cancel.store(true, std::memory_order_relaxed); }

protected:
    void run() override;

private:
    MusicLibrary* m_owner = nullptr;
    QString       m_root;
    std::atomic<bool> m_cancel{ false };
};

// 往主线程投递一批结果
static void postBatch(MusicLibrary* owner, const QVector<Track>& batch)
{
    const QVector<Track> payload = batch;
    QMetaObject::invokeMethod(owner, [owner, payload]() {
        owner->appendBatch(payload);
    }, Qt::QueuedConnection);
}

void ScanThread::run()
{
    // 这个目录可能已经被用户删了/改名了（上次选的路径被记住了）
    if (!QFileInfo::exists(m_root))
    {
        QMetaObject::invokeMethod(m_owner, [o = m_owner]() { o->finishScan(0, 0); },
                                  Qt::QueuedConnection);
        return;
    }

    QSet<QString> exts;
    for (const QString& e : MusicLibrary::audioSuffixes())
        exts.insert(e);

    // Subdirectories = 一路递归到底，这正是"文件夹里还有文件夹也要翻到"那一条。
    // 刻意不加 QDir::Hidden —— 隐藏目录里基本只有回收站和系统缓存，
    // 翻进去只会拖慢扫描、还可能读到一堆无关文件。
    QDirIterator it(m_root, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);

    QVector<Track> batch;
    batch.reserve(BATCH_SIZE);

    int seen = 0;
    int found = 0;
    int skipped = 0;

    QElapsedTimer progressClock;
    progressClock.start();

    while (it.hasNext())
    {
        if (m_cancel.load(std::memory_order_relaxed))
            return;                          // 取消：静默退出，什么都不发

        it.next();
        ++seen;

        const QFileInfo fi = it.fileInfo();
        if (!exts.contains(fi.suffix().toLower()))
            continue;                        // 不是音乐文件，只算了 seen，不进列表

        Track tr;
        tr.path = fi.absoluteFilePath();

        const TrackMeta::Meta meta = TrackMeta::readMeta(tr.path);

        tr.title      = meta.title;
        tr.artist     = meta.artist;
        tr.album      = meta.album;
        tr.durationMs = meta.durationMs;
        tr.hasCover   = meta.hasCover();
        tr.hasLyrics  = !meta.lyrics.isEmpty();

        if (tr.title.isEmpty())
        {
            tr.title = fi.completeBaseName();     // 最后一层保险：文件名就是标题
            ++skipped;                            // 顺便记一笔"标签没读全的"
        }

        batch.append(tr);
        ++found;

        if (batch.size() >= BATCH_SIZE)
        {
            postBatch(m_owner, batch);
            batch.clear();
        }

        if (progressClock.elapsed() >= PROGRESS_EVERY)
        {
            progressClock.restart();
            const QString dir = fi.absolutePath();
            QMetaObject::invokeMethod(m_owner, [o = m_owner, seen, found, dir]() {
                emit o->scanProgress(seen, found, dir);
            }, Qt::QueuedConnection);
        }
    }

    if (!batch.isEmpty())
        postBatch(m_owner, batch);

    QMetaObject::invokeMethod(m_owner, [o = m_owner, found, skipped]() {
        o->finishScan(found, skipped);
    }, Qt::QueuedConnection);
}

// =============================================================================
//  MusicLibrary
// =============================================================================
MusicLibrary::MusicLibrary(QObject* parent) : QObject(parent)
{
    // 跨线程传 QVector<Track> 必须先登记类型，否则 queued connection 会在
    // 运行时警告 "Cannot queue arguments of type 'QVector<Track>'" 并把参数丢掉。
    qRegisterMetaType<QVector<Track>>("QVector<Track>");
}

MusicLibrary::~MusicLibrary()
{
    cancelScan();
}

QStringList MusicLibrary::audioSuffixes()
{
    // 只列"可能会是音乐"的。miniaudio 自己能解码的只有 MP3 / FLAC / WAV，
    // 其它格式（m4a / ogg / ape …）先扫进来、让用户能看到，
    // 真的点下去播不了时 AudioPlayer 会明确报错，而不是假装播了。
    return QStringList{
        QStringLiteral("mp3"),  QStringLiteral("flac"), QStringLiteral("wav"),
        QStringLiteral("m4a"),  QStringLiteral("aac"),  QStringLiteral("ogg"),
        QStringLiteral("opus"), QStringLiteral("wma"),  QStringLiteral("aiff"),
        QStringLiteral("aif"),  QStringLiteral("ape"),
    };
}

QString MusicLibrary::normalizeForSearch(const QString& in)
{
    // 和聊天共用同一套归一化（去空白/标点、全角转半角、转小写）。
    // 不另写一套的理由很实在：哪天要改规则，改一处两个功能一起对。
    return ChatScript::normalize(in);
}

QString MusicLibrary::buildKey(const Track& t)
{
    // 分隔符用 0x1F（ASCII 的"单元分隔符"）：它不可能出现在用户输入里，
    // 所以不会出现"标题的尾巴 + 艺术家的开头"被当成一个连续词条的假命中。
    return normalizeForSearch(t.title) + QChar(0x1F)
         + normalizeForSearch(t.artist) + QChar(0x1F)
         + normalizeForSearch(t.album);
}

void MusicLibrary::scanAsync(const QString& rootDir)
{
    cancelScan();       // 上一次没扫完就被换了目录 —— 先让它体面地停下来

    m_tracks.clear();
    m_normKeys.clear();
    m_lastQuery.clear();
    m_lastResult.clear();

    m_root     = rootDir;
    m_scanning = true;

    emit scanStarted();

    auto* w = new ScanThread(this, rootDir);
    m_worker = w;
    m_thread = w;

    // 线程对象自己删自己：扫描结束 -> deleteLater -> 主线程的事件循环里销毁。
    // ★ lambda 里要比一次指针 ★ —— 用户连着换了两个目录时，旧的 finished
    //   可能在新线程起来之后才被执行，那时候 m_worker 已经指向新线程了，
    //   不比较就会把新线程的指针误清掉。
    connect(w, &QThread::finished, this, [this, w]() {
        if (m_worker == w)
        {
            m_worker = nullptr;
            m_thread = nullptr;
            m_scanning = false;
        }
        w->deleteLater();
    });

    w->start();
}

void MusicLibrary::cancelScan()
{
    if (!m_worker)
        return;

    m_worker->cancel();
    m_worker->wait(5000);           // 一轮 QDirIterator 就是毫秒级，5 秒纯属防呆

    if (m_worker)
    {
        m_worker->deleteLater();    // finished 的 lambda 可能还没轮到执行
        m_worker = nullptr;
        m_thread = nullptr;
    }
}

void MusicLibrary::clear()
{
    cancelScan();

    m_tracks.clear();
    m_normKeys.clear();
    m_lastQuery.clear();
    m_lastResult.clear();
    m_root.clear();
    m_scanning = false;
}

const Track* MusicLibrary::trackAt(int index) const
{
    if (index < 0 || index >= m_tracks.size())
        return nullptr;
    return &m_tracks.at(index);
}

void MusicLibrary::appendBatch(const QVector<Track>& batch)
{
    m_tracks.reserve(m_tracks.size() + batch.size());
    m_normKeys.reserve(m_normKeys.size() + batch.size());

    for (const Track& t : batch)
    {
        m_tracks.append(t);
        m_normKeys.append(buildKey(t));
    }

    // 索引变了，上次的收窄缓存作废
    m_lastQuery.clear();
    m_lastResult.clear();

    emit scanBatch(batch);
}

void MusicLibrary::finishScan(int total, int skipped)
{
    m_scanning = false;
    m_lastQuery.clear();
    m_lastResult.clear();

    emit scanFinished(total, skipped);
}

// =============================================================================
//  搜索
// =============================================================================
QVector<int> MusicLibrary::search(const QString& query, int limit) const
{
    if (limit <= 0)
        limit = SEARCH_DEFAULT_LIMIT;

    const QString q = normalizeForSearch(query);

    // 空查询 = 不筛选。直接把前 limit 首按扫描顺序给出去。
    if (q.isEmpty())
    {
        const int n = qMin(limit, m_tracks.size());
        QVector<int> all;
        all.reserve(n);
        for (int i = 0; i < n; ++i)
            all.append(i);

        m_lastQuery.clear();
        m_lastResult.clear();
        return all;
    }

    // ---- ② 前缀收窄 ----
    // 条件很严格，缺一不可：上次有结果、这次是在上次的基础上"又往后打了字"、
    // 而且上次的结果确实比全库小。满足的话只在上次结果里筛 —— 这一步是
    // "边打字边过滤"跟得上的原因（后面几万首根本不用看）。
    if (!m_lastQuery.isEmpty() && m_lastResult.size() < m_tracks.size()
        && q.size() > m_lastQuery.size() && q.startsWith(m_lastQuery))
    {
        QVector<int> narrowed;
        narrowed.reserve(qMin(limit, m_lastResult.size()));

        for (int idx : std::as_const(m_lastResult))
        {
            if (idx < 0 || idx >= m_normKeys.size())
                continue;
            if (m_normKeys.at(idx).contains(q))
            {
                narrowed.append(idx);
                if (narrowed.size() >= limit)
                    break;
            }
        }

        if (!narrowed.isEmpty())
        {
            m_lastQuery  = q;
            m_lastResult = narrowed;
            return narrowed;
        }
        // 收窄到空说明用户其实在改别的词（不是往后打字），落到全表搜
    }

    // ---- ① 全表扫 + ③ 前缀优先 ----
    QVector<int> heads;      // 命中位置在第 0 个字符 —— 这些更接近用户想找的
    QVector<int> subs;       // 命中在中间
    heads.reserve(qMin(limit, 256));
    subs.reserve(qMin(limit, 1024));

    for (int i = 0; i < m_normKeys.size(); ++i)
    {
        const int p = m_normKeys.at(i).indexOf(q);
        if (p < 0)
            continue;

        if (p == 0)
            heads.append(i);
        else
            subs.append(i);

        if (heads.size() >= limit && subs.size() >= limit)
            break;                       // 两边都满了，后面不用再看
    }

    QVector<int> out;
    out.reserve(qMin(limit, heads.size() + subs.size()));
    for (int i = 0; i < heads.size() && out.size() < limit; ++i)
        out.append(heads.at(i));
    for (int i = 0; i < subs.size() && out.size() < limit; ++i)
        out.append(subs.at(i));

    m_lastQuery  = q;
    m_lastResult = out;
    return out;
}

// =============================================================================
//  自检描述
// =============================================================================
QString MusicLibrary::describeIndex() const
{
    qint64 chars = 0;
    for (const QString& k : std::as_const(m_normKeys))
        chars += k.size();

    QString out;
    out += QStringLiteral("  曲库目录  : %1\r\n")
               .arg(m_root.isEmpty() ? QStringLiteral("(还没选文件夹)") : m_root);
    out += QStringLiteral("  曲目数量  : %1 首\r\n").arg(m_tracks.size());
    out += QStringLiteral("  搜索索引  : %1 个归一化串 / 约 %2 KB"
                          "（每首一个，串里含标题+艺术家+专辑）\r\n")
               .arg(m_normKeys.size())
               .arg(chars * 2 / 1024);
    out += QStringLiteral("  已知格式  : %1\r\n").arg(audioSuffixes().join(QLatin1Char(' ')));
    if (!m_tracks.isEmpty())
        out += QStringLiteral("  示例曲目  : %1\r\n").arg(m_tracks.first().path);
    return out;
}
