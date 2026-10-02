#include "AudioPlayer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QStringList>
#include <QTimer>

#include <QMutex>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QWaitCondition>

#include <atomic>
#include <cstring>
#include <string>

// 结构化异常（SEH）：用来做下面那个"后端崩了就跳过"的兜底。
// 只为了拿 __try / __except / _exception_code，不拉整个 windows.h 进来。
#include <excpt.h>

// ★ 只在 .cpp 里吃 miniaudio 的声明（那 4MB 的实现见 third_party/miniaudio_impl.cpp）★
#include "miniaudio.h"

// =============================================================================
//  这一层为什么不用 miniaudio 的 ma_engine（那是它最上层的便利封装）
//
//  2026-09-24 踩到的坑，写下来省得有人再踩：
//    ma_engine_init() 会在内部自己造一个播放设备，而它**不指定设备格式** ——
//    它让 miniaudio 去问设备"你的原生格式是多少"。本机走到那一步直接段错误
//    （SIGSEGV，整个程序挂掉，不是返回错误码）。
//    而 ma_engine 没有提供任何入口让我们插手那个 deviceConfig，
//    所以只要用 engine，就绕不开那条会崩的路。
//
//    换成 ma_device + ma_decoder 自己搭之后，deviceConfig 完全由我们决定 ——
//    **把 format / channels / sampleRate 三个字段显式写死**，
//    miniaudio 就不再去查询设备原生格式。
//
//  ---------------------------------------------------------------------------
//  ▎但光"显式格式"还不够 —— WASAPI 那条路本身在本机就是坏的
//
//    实测（自检报告里能看到）：显式格式之后，WASAPI 的 **context 建得起来**
//    （ma_context_init 返回 0），可紧接着的 **ma_device_init 直接访问违例**，
//    而且崩之前一条日志都不打 —— 崩点在 ma_device_init__wasapi 开头给
//    pDevice->wasapi 清零那一带，还没轮到任何 ma_log_post。
//
//    DirectSound 则是干干净净地成功（r=0，48kHz/2ch），WinMM 也可以。
//
//  ▎所以这里的做法是"每个后端都单独试，崩了就当它不可用"
//
//    这也是这个文件里最不常规的一处：后端探测包在 **SEH（__try/__except）** 里。
//    理由很实在 —— 音频后端是"跟别人家驱动打交道"的代码，在个别机器上抽风
//    是常态；我们不可能因为它就把整个程序放弃掉。崩了记一笔、换个后端继续，
//    最坏也就是退回到 WinMM，而不是用户双击 exe 就闪退。
//    崩过的后端会在本进程内拉黑，免得每次开播放器都把线程再崩一遍。
//
//    注意：__try 里只允许放 C 调用（MSVC 不允许"需要对象析构"的函数用 __try），
//    所以每个后端调用各自封成一个只有 POD 的小函数。
//
//  ▎后端顺序是实测定的：DirectSound → WinMM → WASAPI
//    不是按"谁更现代"排的，理由写在 kBackends 上面那段注释里（含对照实验）。
//
//  ▎还有第二个原本要解决的问题：后端降级
//    ma_context_init(nullptr, ...) 只挑"第一个能初始化的后端"，可如果这个后端
//    在**建播放设备**这一步失败，它不会再回头试别的 —— 播放就整个没了。
//    所以这里自己把几个后端挨个排一遍，第一个"context 和 device 都成功"的胜出。
//
//  ---------------------------------------------------------------------------
//  ▎两个线程的分工（这是整个文件里最容易写错的地方）
//
//    主线程：把 decoder 换掉、启停设备、读"播到哪了"
//    音频线程（miniaudio 的回调）：唯一有权碰 decoder 读帧的人
//
//    decoder 不是线程安全的，所以：
//      · seek 不直接调，而是往 shared.seekPending 里放个请求，让音频线程去执行；
//      · 换歌之前必须 ma_device_stop() —— 它会等音频线程退出，
//        之后才敢 uninit 旧的 decoder。
//    音量、位置、结束标记这些跨线程的东西一律用 std::atomic。
// =============================================================================

namespace {

constexpr int TICK_MS = 200;      // 界面轮询周期：进度条和歌词高亮都够用

// 统一的解码/输出格式。
// ★ 必须显式给出 ★ —— 这是绕开上面那个崩溃的关键（见文件开头的说明）。
// 48kHz 立体声浮点是 Windows 共享模式的通用格式，解码器会自动重采样到这个格式。
constexpr ma_format kFormat     = ma_format_f32;
constexpr ma_uint32 kChannels   = 2;
constexpr ma_uint32 kSampleRate = 48000;

// 在线流式播放（StreamFeed）用的 HTTP 客户端。QNetworkAccessManager 全程
// 在主线程跑，音频线程只通过 StreamFeed 的锁缓冲拿数据。
QNetworkAccessManager& nam()
{
    static QNetworkAccessManager m;
    return m;
}

const char* const kStreamUA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";

struct BackendChoice
{
    ma_backend  backend;
    const char* name;
};

// 后端优先级：现代低延迟路径优先，兼容性最好的垫底。
// 本机 WASAPI 的设备初始化会崩（见文件开头），但那是**这台机器**的事 ——
// 别的机器上它是首选路径，所以顺序不动，靠上面的 SEH 兜底往下走。
//  ---------------------------------------------------------------------------
//  ▎后端顺序：DirectSound 优先，WASAPI 垫底（本机实测结论，2026-09-24）
//
//    直觉上 WASAPI 该排第一（现代、低延迟），但本机实测：
//      · WASAPI 的 ma_device_init 直接访问违例（SEH 0xC0000005，见上）；
//      · 更麻烦的是 —— 就算用它自己的 SEH 兜住、并且老老实实调了
//        ma_context_uninit() 收尾，这次"没走完的"设备初始化还是会留下收不干净的
//        状态：进程**正常退出时**在 Qt 收尾阶段段错误（退出码 139）。
//        对照实验：把 WASAPI 从这张表里删掉，同一个自检立刻变回退出码 0。
//        也就是说，这个后端在本机是"碰一下就脏"，不是"失败了就没事"。
//
//    所以顺序改成"先后端能跑通的先来"：
//      DirectSound —— 实测 r=0 一把过，从播放到退出全程干净；
//      WinMM       —— 更老的垫底，兼容性最好（延迟高，但放歌无所谓）；
//      WASAPI      —— 排最后：只有当上面两个都建不起设备时才去碰它。
//                     在 DSound 正常的机器上它根本不会被尝试，也就不会脏。
//
//    DirectSound 在 Windows 10/11 上是系统自带的（走音频引擎的兼容层），
//    延迟比 WASAPI 高一些 —— 但这是听歌，不是打音游，这点延迟完全无所谓。
//    稳定、能出声、退出不崩，比"用上更现代的后端"重要。
//  ---------------------------------------------------------------------------
const BackendChoice kBackends[] = {
    { ma_backend_dsound, "DirectSound" },
    { ma_backend_winmm,  "WinMM"       },
    { ma_backend_wasapi, "WASAPI"      },
};
constexpr size_t kBackendCount = sizeof(kBackends) / sizeof(kBackends[0]);

// 崩过的后端（本进程内拉黑，见文件开头）。它只被主线程碰，所以不用原子。
// 全 false 初始化 —— 这里**别写死元素个数**，kBackends 增删时容易忘。
bool g_backendFaulted[kBackendCount] = {};

// -----------------------------------------------------------------------------
//  后端探测 —— 全部包在 SEH 里
//
//  这三个函数存在的唯一理由是 __try 不能和"需要析构的 C++ 对象"共存，
//  所以把 C 调用单独拎出来。每个函数里只有 POD，没有 Qt 对象，没有 string。
// -----------------------------------------------------------------------------

// 返回值的约定：*faulted 为 true 表示"崩了"，此时返回值无意义。
ma_result probeContextInit(ma_backend backend, const ma_context_config* cfg,
                           ma_context* ctx, bool* faulted)
{
    __try
    {
        ma_backend list[1] = { backend };
        return ma_context_init(list, 1, cfg, ctx);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *faulted = true;
        return MA_ERROR;
    }
}

ma_result probeDeviceInit(ma_context* ctx, const ma_device_config* cfg,
                          ma_device* dev, unsigned long* faultCode)
{
    __try
    {
        return ma_device_init(ctx, cfg, dev);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *faultCode = _exception_code();
        return MA_ERROR;
    }
}

void probeContextUninit(ma_context* ctx, bool* faulted)
{
    __try
    {
        ma_context_uninit(ctx);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *faulted = true;
    }
}

// -----------------------------------------------------------------------------
//  AudioShared —— 主线程和音频线程之间共享的那点状态
// -----------------------------------------------------------------------------
struct AudioShared
{
    ma_decoder decoder{};
    std::atomic<bool> decoderReady{ false };    // 只有"设备已停"时才改

    std::atomic<ma_uint64> playedFrames{ 0 };   // 已经放出去多少帧
    std::atomic<ma_uint64> seekTo{ 0 };         // 待执行的跳转目标
    std::atomic<bool>      seekPending{ false };
    std::atomic<bool>      reachedEnd{ false };
    std::atomic<float>     volume{ 1.0f };      // 线性 0~1

    // ---- 频谱取样环形缓冲（音频线程只写、主线程只读，无锁）----
    // 单声道 f32。游标是"累计写入帧数"（单调递增，取模定位）：写端 release
    // 递增，读端 acquire 取快照 —— 经典 SPSC，写读各自单线程，天然安全。
    // ★ 不播放时回调停发，缓冲里是旧数据：读取方（PlayerPage）按 isPlaying
    //   自行衰减到零，这里不管生命周期 ★
    static constexpr int kSpecFrames = 2048;    // ≈42ms @48kHz，够算 1024 点 FFT
    float                  specMono[kSpecFrames]{};
    std::atomic<ma_uint64> specWritten{ 0 };
};

// -----------------------------------------------------------------------------
//  音频线程：往输出缓冲区里填声音
//
//  ★ 这里面**绝对不能**碰 Qt 对象、不能加锁、不能分配内存 ★
//    这条线程是音频后端按固定周期回调的，稍微慢一点就是"咔咔"的爆音。
// -----------------------------------------------------------------------------
void onDeviceData(ma_device* pDevice, void* pOutput, const void* /*pInput*/, ma_uint32 frameCount)
{
    auto* sh = static_cast<AudioShared*>(pDevice->pUserData);
    if (!sh)
        return;

    const ma_uint32 bytesPerFrame = ma_get_bytes_per_frame(kFormat, kChannels);

    // ---- ① 有 seek 请求就在这里执行 ----
    // decoder 不是线程安全的，只有这条线程能动它。
    if (sh->seekPending.exchange(false))
    {
        const ma_uint64 target = sh->seekTo.load();
        ma_decoder_seek_to_pcm_frame(&sh->decoder, target);
        sh->playedFrames.store(target);
        sh->reachedEnd.store(false);
    }

    // ---- ② 读一帧的数据 ----
    ma_uint64 got = 0;
    if (sh->decoderReady.load())
        ma_decoder_read_pcm_frames(&sh->decoder, pOutput, frameCount, &got);

    if (got < frameCount)
    {
        // 不够就补静音。宁可安静，也不要让缓冲区里的残留数据被重复播出来。
        std::memset(static_cast<ma_uint8*>(pOutput) + got * bytesPerFrame,
                    0, size_t(frameCount - got) * bytesPerFrame);
        sh->reachedEnd.store(true);
    }
    else
    {
        sh->reachedEnd.store(false);
    }

    // ---- ③ 音量（自己乘，比让引擎去算一层增益省事）----
    const float v = sh->volume.load();
    if (v < 0.999f)
    {
        float* f = static_cast<float*>(pOutput);
        const size_t n = size_t(frameCount) * kChannels;
        for (size_t i = 0; i < n; ++i)
            f[i] *= v;
    }

    // ---- ④ 频谱取样：降混单声道塞进环形缓冲（纯拷贝 + 原子递增，无锁无分配）----
    // 放的是音量乘完之后的样本 —— 柱子高度跟用户耳朵听到的响度一致。
    // 不足帧数的静音填充也要进缓冲：不播/播完时频谱才能"看见"安静。
    // 万一某后端的周期超过缓冲容量（当前 10ms≈480 帧，远小于 2048，到不了），
    // 只保留**最新的** cap 帧 —— 游标照样前进 frameCount，语义才对得上。
    {
        const float* f = static_cast<const float*>(pOutput);
        const int cap    = AudioShared::kSpecFrames;
        const int n      = int(qMin<ma_uint64>(frameCount, ma_uint64(cap)));
        const int first  = int(frameCount - ma_uint64(n));   // 超容量时跳过最旧的
        const int start  = int(sh->specWritten.load(std::memory_order_relaxed) % ma_uint64(cap));
        for (int i = 0; i < n; ++i)
            sh->specMono[(start + i) % cap] =
                0.5f * (f[size_t(first + i) * kChannels] + f[size_t(first + i) * kChannels + 1]);
        // release：主线程读到新游标时，这批样本必然已经可见
        sh->specWritten.fetch_add(ma_uint64(frameCount), std::memory_order_release);
    }

    sh->playedFrames.fetch_add(got);
}

} // namespace

// 流式喂数据器（定义在 Impl 之后）：在线播放时给解码器当"数据源"
struct StreamFeed;

// =============================================================================
//  Impl
// =============================================================================
struct AudioPlayer::Impl
{
    AudioShared shared;

    ma_log*     log     = nullptr;
    ma_context* context = nullptr;
    ma_device*  device  = nullptr;

    StreamFeed* stream  = nullptr;    // 在线播放时的流式喂数据器（本地播放为空）

    QString backendName;
    QString deviceName;

    // 后端探测过程中"哪些后端没走通、为什么"，给自检报告看
    QStringList probeNotes;

    bool logReady     = false;
    bool contextReady = false;
    bool deviceReady  = false;
};

// =============================================================================
//  StreamFeed —— 在线播放的流式喂数据器
//
//  ★ 给谁用 ★ ma_decoder 的自定义 read/seek 回调（ma_decoder_init 的回调版）。
//    解码器住在音频线程里，read/seek 就发生在音频线程；下载在主线程
//   （QNetworkAccessManager 的 readyRead 增量追加）。
//
//  ★ 线程模型 ★
//    · m_buf 是"文件字节区间 [m_base, m_base+size)" 的顺序缓冲，只在主线程追加；
//    · 解码线程 read/seek 时拿 QMutex 短暂加锁 —— 音频回调里加锁是本项目
//      的破例：等补货时音频线程会停，表现是"缓冲期的一小段静音"。概率靠
//      预缓冲压到最低：网易云的下载速度通常是播放速度的几十倍；
//    · seek 到未缓冲区 → 清缓冲 + 发 queued 回主线程用 Range 续传
//     （m_restart 由 AudioPlayer 设置，内部 invokeMethod 排回主线程）。
//
//  ★ 刻意不是 QObject ★ 没有信号槽需求；重启回调用 std::function +
//  invokeMethod(ctx) 排线程，省一个 moc 注册。
// =============================================================================
class StreamFeed
{
public:
    StreamFeed(const QUrl& url, QMap<QString, QString> headers)
        : m_url(url), m_headers(std::move(headers))
    {
    }

    ~StreamFeed()
    {
        stop();
        if (m_reply)
            m_reply->deleteLater();
    }

    void setRestartHandler(std::function<void(qint64)> h) { m_restart = std::move(h); }

    void start() { issueGet(0); }
    void restartAt(qint64 offset) { issueGet(offset); }   // 主线程：Range 续传

    void stop()
    {
        {
            QMutexLocker l(&m_mtx);
            m_stopped = true;
        }
        m_cv.wakeAll();
        if (m_reply)
            m_reply->abort();              // 触发 finished → 主线程清理
    }

    // ---- 解码器回调（音频线程）----
    size_t read(void* dst, size_t len)
    {
        QMutexLocker l(&m_mtx);
        if (m_read < m_base)
            m_read = m_base;               // 异常兜底

    qint64 waitedMs = 0;
    for (;;)
    {
        const qint64 inBuf = m_base + qint64(m_buf.size()) - m_read;
        const bool   atEnd = (m_total >= 0 && m_read >= m_total);
        if (atEnd || inBuf >= qint64(len))
            break;                                 // 够了 / 到头了
        if (m_stopped || (m_finished && inBuf <= 0))
            break;                                 // 没有更多数据了
        if (m_finished && inBuf > 0)
            break;                                 // 下载完：给剩余的短读
            if (m_failed)
                break;                                 // 网络出错：交给调用方提示
            if (waitedMs >= 10000)
                break;                                 // 弱网兜底：10 秒等不到给短读
            m_cv.wait(&m_mtx, 50);
            waitedMs += 50;
    }

        const qint64 inBuf  = m_base + qint64(m_buf.size()) - m_read;
        const qint64 remain = (m_total >= 0) ? (m_total - m_read) : qint64(len);
        const qint64 n = qMin<qint64>(qMin<qint64>(qint64(len), inBuf),
                                      qMax<qint64>(remain, 0));
        if (n <= 0)
            return 0;                      // EOF：解码器走 reachedEnd → trackFinished

        std::memcpy(dst, m_buf.constData() + (m_read - m_base), size_t(n));
        m_read += n;
        return size_t(n);
    }

    bool seek(qint64 off, int origin)
    {
        QMutexLocker l(&m_mtx);
        qint64 target = off;
        if (origin == ma_seek_origin_current)
            target += m_read;
        else if (origin == ma_seek_origin_end)
            target = (m_total >= 0 ? m_total : m_base + qint64(m_buf.size())) + off;
        if (target < 0)
            target = 0;

        if (target >= m_base && target <= m_base + qint64(m_buf.size()))
        {
            m_read = target;               // 缓冲内：直接跳
        }
        else
        {
            // 跳到未缓冲区：清缓冲、从目标重新拉流（主线程 Range 续传）
            m_base     = target;
            m_buf.clear();
            m_read     = target;
            m_finished = false;
            m_failed   = false;
            m_bufGen.fetch_add(1, std::memory_order_release);   // 旧偏移的迟到数据作废
            if (m_restart)
                m_restart(target);         // handler 内部排回主线程（见上）
        }
        m_cv.wakeAll();
        return true;
    }

private:
    void issueGet(qint64 fromOffset)
    {
    if (m_reply)
    {
        m_reply->disconnect();             // 断开本 reply 的所有信号连接
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }

        {
            QMutexLocker l(&m_mtx);
            m_finished = false;
            m_failed   = false;
        }

        QUrl url = m_url;
        QMap<QString, QString> headers = m_headers;
        if (fromOffset > 0)
            headers.insert(QStringLiteral("Range"),
                           QStringLiteral("bytes=%1-").arg(fromOffset));

        QNetworkRequest req(url);
        // ★ 流式回放不能设总时长上限 ★ 这条连接要活完整首歌 —— 15 秒的话
        // 每首歌播到一分钟出头就会被 abort。卡死检测交给读侧的 10 秒等待
        // 兜底：真断了 read 会给短读 → 解码器 EOF → 自然切下一首。
        req.setTransferTimeout(0);
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
        req.setRawHeader("User-Agent", kStreamUA);
        for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
            req.setRawHeader(it.key().toUtf8(), it.value().toUtf8());

        m_reply = nam().get(req);
        m_replyGen = m_bufGen.load(std::memory_order_acquire);   // 本批数据的代数
        QObject::connect(m_reply, &QNetworkReply::readyRead, m_reply, [this] {
            QMutexLocker l(&m_mtx);
            if (m_replyGen != m_bufGen.load(std::memory_order_acquire))
                return;                        // ★ seek 已重置缓冲:旧偏移的迟到数据整段丢弃 ★
            const QVariant cl = m_reply->header(QNetworkRequest::ContentLengthHeader);
            if (m_total < 0 && cl.isValid())
                m_total = m_base + cl.toLongLong();
            m_buf += m_reply->readAll();
            m_cv.wakeAll();
        });
        QObject::connect(m_reply, &QNetworkReply::finished, m_reply, [this] {
            QMutexLocker l(&m_mtx);
            if (m_replyGen != m_bufGen.load(std::memory_order_acquire))
                return;                        // 旧 reply 的收尾不碰新缓冲的账本
            const QVariant cl = m_reply->header(QNetworkRequest::ContentLengthHeader);
            if (m_total < 0 && cl.isValid())
                m_total = m_base + cl.toLongLong();
            m_finished = true;
            m_cv.wakeAll();
        });
        QObject::connect(m_reply, &QNetworkReply::errorOccurred, m_reply, [this] {
            QMutexLocker l(&m_mtx);
            if (!m_stopped)                    // 主动 abort 的不记错误
                m_errorText = m_reply->errorString();
            m_failed = true;               // abort() 也会到这 —— 靠 stopped 区分主动/异常
            m_cv.wakeAll();
        });
    }

    QUrl                   m_url;
    QMap<QString, QString> m_headers;
    QNetworkReply*         m_reply = nullptr;

    QMutex         m_mtx;
    QWaitCondition m_cv;
    QByteArray     m_buf;          // 文件字节区间 [m_base, m_base+size) 的数据
    qint64         m_base    = 0;  // m_buf[0] 对应的文件偏移（seek 重拉后 >0）
    qint64         m_read    = 0;  // 解码器读位置（文件字节偏移）
    qint64         m_total   = -1; // Content-Length（-1 = 未知）
    QString        m_errorText;    // 最近一次网络错误（诊断用）
    bool           m_finished = false;
    bool           m_failed   = false;
    bool           m_stopped  = false;

    // ★ 缓冲"代数"★ seek 出缓冲区时（音频线程）会清空 m_buf、改 m_base ——
    // 旧 reply 已经排到主线程队列里的 readyRead 若晚于这次清空才执行，
    // 会把旧文件偏移的字节追加进以新 base 起算的缓冲（解码器瞬间失步出噪声）。
    // seek 时代数 +1；readyRead 持锁核对代数，不一致就整段丢弃。
    // m_bufGen 跨线程（音频 +1 / 主线程读），m_replyGen 只在主线程。
    std::atomic<quint64> m_bufGen{ 0 };
    quint64              m_replyGen = 0;

    std::function<void(qint64)> m_restart; // seek 出缓冲区 → 主线程 Range 续传

public:
    // ---- 诊断 / 预缓冲（主线程）----
    // ★ 这三个诊断方法刻意不写 const ★ —— 要拿锁，而 m_mtx 不是 mutable。
    qint64 bufferedBytes()
    {
        QMutexLocker l(&m_mtx);
        return m_base + qint64(m_buf.size()) - m_read;
    }
    bool isFailed()
    {
        QMutexLocker l(&m_mtx);
        return m_failed;
    }
    QString errorText()
    {
        QMutexLocker l(&m_mtx);
        return m_errorText;
    }

    // ★ 主线程等首块数据 ★ ma_decoder_init 的探测读发生在主线程，而数据
    // 靠主线程的事件循环送达 —— 不先缓冲的话，探测读会自锁到超时
    // （本地事件收不到）。processEvents 转起来，网络事件就能进来。
    void waitForFirstBytes(int timeoutMs)
    {
        QElapsedTimer t;
        t.start();
        while (m_buf.isEmpty() && !m_failed && !m_finished && !m_stopped
               && t.elapsed() < timeoutMs)
        {
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
        }
    }
};

// ---- miniaudio 解码回调：把 StreamFeed 适配成 ma_decoder 的数据源 ----
ma_result onStreamRead(ma_decoder* pDecoder, void* pBufferOut, size_t bytesToRead,
                       size_t* pBytesRead)
{
    auto* feed = static_cast<StreamFeed*>(pDecoder->pUserData);
    *pBytesRead = feed ? feed->read(pBufferOut, bytesToRead) : 0;
    return MA_SUCCESS;                     // 读多少算多少；0 = EOF（解码器自行判定）
}

ma_result onStreamSeek(ma_decoder* pDecoder, ma_int64 byteOffset, ma_seek_origin origin)
{
    auto* feed = static_cast<StreamFeed*>(pDecoder->pUserData);
    return feed ? (feed->seek(byteOffset, int(origin)) ? MA_SUCCESS : MA_ERROR)
                : MA_ERROR;
}

// =============================================================================
//  构造 / 析构
// =============================================================================

AudioPlayer::AudioPlayer(QObject* parent) : QObject(parent), m_impl(new Impl)
{

    // ---------------- 日志 ----------------
    // 把 miniaudio 的内部日志接到 qDebug 之外的地方（它默认往 OutputDebugString 写，
    // 排查时在自检报告里看不到）。这里攒着也没用，所以只注册、不保存内容 ——
    // 真正的价值是"后端挑错了/设备建不起来"这类信息能被我们自己的调试输出看到。
    m_impl->log = new ma_log;
    if (ma_log_init(nullptr, m_impl->log) == MA_SUCCESS)
    {
        m_impl->logReady = true;

        // 回调要**在这里**就挂上，不能等后端挑完 —— 后端探测阶段正是最需要
        // 它说话的时候（"设备建不起来"的原因往往只在 miniaudio 自己的日志里）。
        ma_log_register_callback(
            m_impl->log,
            ma_log_callback_init(
                [](void* /*pUserData*/, ma_uint32 /*level*/, const char* pMessage) {
                    qWarning("[miniaudio] %s", pMessage);
                }, nullptr));
    }
    else
    {
        delete m_impl->log;
        m_impl->log = nullptr;
    }

    // ---------------- 挑一个能用的后端 ----------------
    for (size_t bi = 0; bi < kBackendCount; ++bi)
    {
        const BackendChoice& bc = kBackends[bi];

        if (g_backendFaulted[bi])
        {
            m_impl->probeNotes += QStringLiteral("%1：本进程内已崩过，跳过\r\n")
                                      .arg(QString::fromLatin1(bc.name));
            continue;
        }

        ma_backend backendList[1] = { bc.backend };

        auto* ctx = new ma_context;
        ma_context_config ctxCfg = ma_context_config_init();
        if (m_impl->logReady)
            ctxCfg.pLog = m_impl->log;

        bool ctxFaulted = false;
        const ma_result ctxRes = probeContextInit(bc.backend, &ctxCfg, ctx, &ctxFaulted);

        if (ctxFaulted)
        {
            // 崩在这儿的话连内存都不敢碰 —— 它可能只初始化了一半。
            // 直接漏掉这一小块内存，换个后端继续：为几十字节把进程搭进去不值。
            g_backendFaulted[bi] = true;
            m_impl->probeNotes += QStringLiteral("%1：初始化后端时崩了（SEH），已跳过\r\n")
                                      .arg(QString::fromLatin1(bc.name));
            continue;
        }

        if (ctxRes != MA_SUCCESS)
        {
            delete ctx;
            continue;                       // 这个后端在这台机器上不存在，下一个
        }

        // ★★★ 三个格式字段一个都不能少 ★★★ —— 见文件开头的说明：
        //   不写的话 miniaudio 会去查询设备原生格式，那条路本机是崩的。
        ma_device_config dcfg  = ma_device_config_init(ma_device_type_playback);
        dcfg.playback.format   = kFormat;
        dcfg.playback.channels = kChannels;
        dcfg.sampleRate        = kSampleRate;
        dcfg.dataCallback      = onDeviceData;
        dcfg.pUserData         = &m_impl->shared;

        auto* dev = new ma_device;
        unsigned long faultCode = 0;
        const ma_result devRes = probeDeviceInit(ctx, &dcfg, dev, &faultCode);

        if (faultCode != 0)
        {
            // 这就是本机 WASAPI 的结局：整条线程访问违例。
            // context 是建成功的，所以还它回去（顺手也防它自己崩）。
            bool uninitFaulted = false;
            probeContextUninit(ctx, &uninitFaulted);
            delete ctx;
            delete dev;

            g_backendFaulted[bi] = true;
            m_impl->probeNotes +=
                QStringLiteral("%1：建播放设备时崩了（SEH 0x%2），已跳过\r\n")
                    .arg(QString::fromLatin1(bc.name))
                    .arg(faultCode, 8, 16, QLatin1Char('0'));
            continue;
        }

        if (devRes != MA_SUCCESS)
        {
            // 失败时 miniaudio 已经自己清理过了，这里只需要把内存还回去
            ma_context_uninit(ctx);
            delete ctx;
            delete dev;
            continue;                       // 这个后端建不起设备，下一个
        }

        m_impl->context      = ctx;
        m_impl->device       = dev;
        m_impl->backendName  = QString::fromLatin1(bc.name);
        m_impl->deviceName   = QString::fromUtf8(dev->playback.name);
        m_impl->contextReady = true;
        m_impl->deviceReady  = true;
        break;
    }

    m_timer = new QTimer(this);
    m_timer->setInterval(TICK_MS);
    connect(m_timer, &QTimer::timeout, this, &AudioPlayer::onTick);
    m_timer->start();
}

AudioPlayer::~AudioPlayer()
{
    if (!m_impl)
        return;

    // 收尾各阶段的耗时打点：退出卡顿（"按退出好几秒才结束"）排查看这一行
    QElapsedTimer dtorClock;
    dtorClock.start();

    // 顺序不能乱：★先把在线流的喂数据器叫停★ 音频回调可能正卡在"等网络
    // 补数据"的条件变量上（最长睡 10 秒）—— 不先唤醒它，下面 ma_device_uninit
    // 等回调返回就会跟着吊住，用户按退出后进程在任务管理器里赖着不走的就是它。
    // 然后停设备（等音频线程退出），再放 decoder，最后才是 context / log
    if (m_impl->stream)
    {
        m_impl->stream->stop();
        qDebug() << "[PetPal] 音频收尾·流停止" << dtorClock.restart() << "ms";
    }
    if (m_impl->deviceReady)
    {
        ma_device_uninit(m_impl->device);      // 内部会 stop + join 线程
        m_impl->deviceReady = false;
        qDebug() << "[PetPal] 音频收尾·设备释放" << dtorClock.restart() << "ms";
    }
    if (m_impl->shared.decoderReady.load())
    {
        ma_decoder_uninit(&m_impl->shared.decoder);
        m_impl->shared.decoderReady.store(false);
    }
    delete m_impl->stream;                 // 在线流的喂数据器一并回收
    m_impl->stream = nullptr;
    if (m_impl->contextReady)
    {
        ma_context_uninit(m_impl->context);
        m_impl->contextReady = false;
    }
    if (m_impl->logReady)
    {
        ma_log_uninit(m_impl->log);
        m_impl->logReady = false;
    }

    delete m_impl->device;
    delete m_impl->context;
    delete m_impl->log;
    delete m_impl;

    m_impl = nullptr;
}

// =============================================================================
//  换歌
// =============================================================================
bool AudioPlayer::load(const QString& path, QString* errorOut)
{
    const auto fail = [this, errorOut](const QString& msg) -> bool {
        if (errorOut)
            *errorOut = msg;
        emit errorOccurred(msg);
        return false;
    };

    if (!m_impl->deviceReady)
        return fail(QStringLiteral("没找到可用的音频输出设备"));

    // ★★ 换歌之前必须先让音频线程停下来 ★★
    //   ma_device_stop() 会等那条线程退出，之后才敢动 decoder ——
    //   否则就是"一边读一边换"，早晚读出乱码或者直接崩。
    if (ma_device_is_started(m_impl->device))
        ma_device_stop(m_impl->device);

    if (m_impl->shared.decoderReady.load())
    {
        ma_decoder_uninit(&m_impl->shared.decoder);
        m_impl->shared.decoderReady.store(false);
    }
    delete m_impl->stream;                 // 换歌：在线流的喂数据器一并回收
    m_impl->stream = nullptr;

    m_durationMs       = 0;
    m_finishedReported = false;

    if (path.isEmpty() || !QFileInfo::exists(path))
        return fail(QStringLiteral("文件不存在：%1").arg(path));

    // ★ 中文路径必须走 _w 版本 ★
    //   ma_decoder_init_file() 收 char*，miniaudio 内部拿它去 fopen ——
    //   中文 Windows 上就是按 GBK 找文件。转错一个字节的后果是
    //   "文件明明在，就是打不开"，而且报错还看不出来。
    //   QString::toStdWString() 给的 UTF-16 正好是 _w 版本要的格式。
    const std::wstring wpath = path.toStdWString();
    const ma_decoder_config dcfg = ma_decoder_config_init(kFormat, kChannels, kSampleRate);

    if (ma_decoder_init_file_w(wpath.c_str(), &dcfg, &m_impl->shared.decoder) != MA_SUCCESS)
        return fail(QStringLiteral("打不开「%1」—— 内置解码器只认 MP3 / FLAC / WAV，"
                                   "其它格式得先转码")
                        .arg(QFileInfo(path).fileName()));

    m_impl->shared.decoderReady.store(true);
    m_impl->shared.playedFrames.store(0);
    m_impl->shared.seekPending.store(false);
    m_impl->shared.reachedEnd.store(false);

    ma_uint64 frames = 0;
    if (ma_decoder_get_length_in_pcm_frames(&m_impl->shared.decoder, &frames) == MA_SUCCESS
        && frames > 0)
    {
        m_durationMs = qint64(double(frames) * 1000.0 / double(kSampleRate) + 0.5);
    }

    emit durationChanged(m_durationMs);
    emit positionChanged(0);
    emit stateChanged();
    return true;
}

// =============================================================================
//  在线流式播放（网易云 MP3 直链）：边下边播，不落盘
//
//  解码器从"文件"换成 StreamFeed 自定义数据源（ma_decoder_init 的回调版，
//  dr_mp3 默认启用，无需任何后端选择代码）。
//  时长直接用搜索结果带的毫秒数 —— 流式下 ma_decoder 拿不到总长，不能靠它。
//  play/pause/seek/进度轮询/reachedEnd/trackFinished 全部复用现有机制：
//    · seek → ma_decoder_seek_to_pcm_frame → StreamFeed::seek（缓冲内直接跳，
//      跳出缓冲区由主线程 Range 续传）；
//    · 播放位置追平下载进度 → StreamFeed::read 短暂等待补货（弱网 = 一小段
//      静音，不是错误）。
// =============================================================================
bool AudioPlayer::loadOnline(const QUrl& url, qint64 durationMs,
                             const QMap<QString, QString>& headers, QString* errorOut)
{
    const auto fail = [this, errorOut](const QString& msg) -> bool {
        if (errorOut)
            *errorOut = msg;
        emit errorOccurred(msg);
        return false;
    };

    if (!m_impl->deviceReady)
        return fail(QStringLiteral("没找到可用的音频输出设备"));

    // 和 load() 同一条纪律：先停音频线程，再动 decoder / stream
    if (ma_device_is_started(m_impl->device))
        ma_device_stop(m_impl->device);

    if (m_impl->shared.decoderReady.load())
    {
        ma_decoder_uninit(&m_impl->shared.decoder);
        m_impl->shared.decoderReady.store(false);
    }
    delete m_impl->stream;
    m_impl->stream = nullptr;

    m_durationMs       = durationMs;
    m_finishedReported = false;

    m_impl->stream = new StreamFeed(url, headers);
    m_impl->stream->setRestartHandler([this](qint64 off) {
        // seek 可能从音频线程发起 —— 排到主线程再动网络对象
        QMetaObject::invokeMethod(this, [this, off] { restartStreamAt(off); },
                                  Qt::QueuedConnection);
    });
    m_impl->stream->start();

    // ★ 等首块数据再初始化解码器 ★ ma_decoder_init 的探测读发生在**主线程**，
    // 而数据靠主线程的事件循环送达 —— 不先缓冲的话，探测读在这里等网络
    // = 自己锁死自己（本地事件收不到），5 秒超时后报"初始化失败"。
    // processEvents（排除用户输入）转起来，网络事件就能进来；正常 100~300ms
    // 就能拿到首块，UI 只是极短暂的无响应。
    m_impl->stream->waitForFirstBytes(8000);

    const ma_decoder_config dcfg = ma_decoder_config_init(kFormat, kChannels, kSampleRate);
    if (ma_decoder_init(onStreamRead, onStreamSeek, m_impl->stream, &dcfg,
                        &m_impl->shared.decoder) != MA_SUCCESS)
    {
        // 带上诊断：缓冲了 0 字节 = 下载就没成（网络/地址/风控）；
        // 缓冲了 N 字节还失败 = 数据格式不被识别。
        const QString diag = QStringLiteral("（已缓冲 %1 字节%2）")
                                 .arg(m_impl->stream->bufferedBytes())
                                 .arg(m_impl->stream->isFailed()
                                          ? QStringLiteral("，下载出错：")
                                                + m_impl->stream->errorText()
                                          : QString());
        delete m_impl->stream;
        m_impl->stream = nullptr;
        return fail(QStringLiteral("流式解码器初始化失败%1").arg(diag));
    }

    m_impl->shared.decoderReady.store(true);
    m_impl->shared.playedFrames.store(0);
    m_impl->shared.seekPending.store(false);
    m_impl->shared.reachedEnd.store(false);

    emit durationChanged(m_durationMs);
    emit positionChanged(0);
    emit stateChanged();
    return true;
}

// 音频线程 seek 出缓冲区后，由这里重启 Range 续传（主线程）
void AudioPlayer::restartStreamAt(qint64 byteOffset)
{
    if (m_impl && m_impl->stream)
        m_impl->stream->restartAt(byteOffset);
}

bool AudioPlayer::hasTrack() const
{
    return m_impl && m_impl->shared.decoderReady.load();
}

// =============================================================================
//  传输控制
// =============================================================================
void AudioPlayer::play()
{
    if (!m_impl->deviceReady || !m_impl->shared.decoderReady.load())
        return;

    // 上一首已经放到底了，再按播放应该从头来（不然按下去毫无反应）
    if (m_impl->shared.reachedEnd.load())
    {
        m_impl->shared.seekTo.store(0);
        m_impl->shared.seekPending.store(true);
        m_finishedReported = false;
    }

    ma_device_start(m_impl->device);
    emit stateChanged();
}

void AudioPlayer::pause()
{
    if (!m_impl->deviceReady)
        return;

    ma_device_stop(m_impl->device);        // 暂停：位置自然冻住
    emit stateChanged();
}

void AudioPlayer::stop()
{
    if (!m_impl->deviceReady)
        return;

    ma_device_stop(m_impl->device);

    // 设备已经停了，音频线程不在了 —— 这时候动 decoder 是安全的
    if (m_impl->shared.decoderReady.load())
    {
        ma_decoder_seek_to_pcm_frame(&m_impl->shared.decoder, 0);
        m_impl->shared.playedFrames.store(0);
        m_impl->shared.reachedEnd.store(false);
    }

    m_finishedReported = false;

    emit positionChanged(0);
    emit stateChanged();
}

void AudioPlayer::togglePlayPause()
{
    if (isPlaying())
        pause();
    else
        play();
}

void AudioPlayer::seekToMs(qint64 ms)
{
    if (!m_impl->shared.decoderReady.load())
        return;

    if (ms < 0)
        ms = 0;
    if (m_durationMs > 0 && ms > m_durationMs)
        ms = m_durationMs;

    const ma_uint64 frame = ma_uint64(double(ms) * double(kSampleRate) / 1000.0);

    // 交给音频线程去执行。设备没在跑的时候请求就先挂着，
    // 等用户按播放、回调第一次进来时再落实 —— 这样"暂停中拖进度条"
    // 和"播放中拖进度条"走的是同一条路，不用写两套。
    m_impl->shared.seekTo.store(frame);
    m_impl->shared.seekPending.store(true);

    // 界面上立刻反映新位置，不用等下一次回调
    m_impl->shared.playedFrames.store(frame);
    m_impl->shared.reachedEnd.store(false);
    m_finishedReported = false;

    emit positionChanged(ms);
}

// =============================================================================
//  查询
// =============================================================================
qint64 AudioPlayer::positionMs() const
{
    if (!m_impl || !m_impl->shared.decoderReady.load())
        return 0;

    return qint64(double(m_impl->shared.playedFrames.load()) * 1000.0 / double(kSampleRate));
}

qint64 AudioPlayer::durationMs() const
{
    return m_durationMs;
}

bool AudioPlayer::isPlaying() const
{
    return m_impl && m_impl->deviceReady && ma_device_is_started(m_impl->device) != 0;
}

void AudioPlayer::setVolumePercent(int percent)
{
    m_volumePercent = qBound(0, percent, 100);

    if (m_impl)
        m_impl->shared.volume.store(float(m_volumePercent) / 100.0f);
}

int AudioPlayer::volumePercent() const
{
    return m_volumePercent;
}

// =============================================================================
//  频谱取样：拉最近一段单声道样本（音频回调里写进环形缓冲的那份）
//
//  ★ 只能在主线程调（和 positionMs 同一条纪律）★ 读端单线程 + acquire
//  游标，和写端 SPSC 配对，无锁。没在播时缓冲里是旧数据 —— 调用方按
//  isPlaying 自行衰减，这里不管生命周期。
// =============================================================================
int AudioPlayer::readSpectrum(float* dst, int maxFrames)
{
    if (!m_impl || !dst || maxFrames <= 0)
        return 0;

    const AudioShared* sh = &m_impl->shared;
    const int cap     = AudioShared::kSpecFrames;
    const qint64 written = qint64(sh->specWritten.load(std::memory_order_acquire));
    const int have = int(qMin<qint64>(written, qint64(cap)));
    const int n    = qMin(have, maxFrames);
    for (int i = 0; i < n; ++i)
        dst[i] = sh->specMono[int((written - n + i) % cap)];
    return n;
}

// =============================================================================
//  轮询：报位置 + 发现"放完了"
// =============================================================================
void AudioPlayer::onTick()
{
    if (!m_impl->deviceReady || !m_impl->shared.decoderReady.load())
        return;

    emit positionChanged(positionMs());

    if (!m_impl->shared.reachedEnd.load())
        return;

    // ★ 只报一次 ★
    //   轮询是 200ms 一次，不记标记的话一首歌放完会连发五次 trackFinished，
    //   列表就会自己往下跳五首。
    if (m_finishedReported)
        return;

    m_finishedReported = true;

    // 停在末尾：别让空回调一直跑着输出静音（白耗一点 CPU，也让"是否在播放"变得含糊）
    ma_device_stop(m_impl->device);

    if (m_durationMs > 0)
        emit positionChanged(m_durationMs);

    emit trackFinished();
    emit stateChanged();
}

// =============================================================================
//  自检描述
// =============================================================================
QString AudioPlayer::describe() const
{
    QString out;
    out += QStringLiteral("  解码后端  : miniaudio %1（public domain / MIT-0，单头文件）\r\n")
               .arg(QString::fromLatin1(ma_version_string()));

    // 探测过程里被跳过的后端：这是"为什么用的不是 WASAPI"的唯一答案来源
    if (m_impl && !m_impl->probeNotes.isEmpty())
        out += QStringLiteral("  探测记录  : %1").arg(m_impl->probeNotes.join(QString()));

    if (!m_impl || !m_impl->deviceReady)
    {
        out += QStringLiteral("  引擎状态  : [!!] 没起来 —— 这台机器上没有可用的音频输出设备，"
                              "播放会静默失败（界面本身不受影响）\r\n");
        return out;
    }

    out += QStringLiteral("  引擎状态  : 已启动\r\n");
    out += QStringLiteral("  输出后端  : %1\r\n").arg(m_impl->backendName);
    out += QStringLiteral("  输出设备  : %1\r\n").arg(m_impl->deviceName);
    out += QStringLiteral("  采样格式  : 32 位浮点 / 双声道 / 48 kHz（解码器自动重采样）\r\n");
    out += QStringLiteral("  可解码    : MP3 / FLAC / WAV（其它格式会明确报错，不会静默哑掉）\r\n");
    out += QStringLiteral("  音量      : %1%\r\n").arg(m_volumePercent);
    return out;
}
