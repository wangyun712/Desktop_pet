#include "AudioPlayer.h"

#include <QFileInfo>
#include <QStringList>
#include <QTimer>

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

    sh->playedFrames.fetch_add(got);
}

} // namespace

// =============================================================================
//  Impl
// =============================================================================
struct AudioPlayer::Impl
{
    AudioShared shared;

    ma_log*     log     = nullptr;
    ma_context* context = nullptr;
    ma_device*  device  = nullptr;

    QString backendName;
    QString deviceName;

    // 后端探测过程中"哪些后端没走通、为什么"，给自检报告看
    QStringList probeNotes;

    bool logReady     = false;
    bool contextReady = false;
    bool deviceReady  = false;
};

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

    // 顺序不能乱：先停设备（等音频线程退出），再放 decoder，最后才是 context / log
    if (m_impl->deviceReady)
    {
        ma_device_uninit(m_impl->device);      // 内部会 stop + join 线程
        m_impl->deviceReady = false;
    }
    if (m_impl->shared.decoderReady.load())
    {
        ma_decoder_uninit(&m_impl->shared.decoder);
        m_impl->shared.decoderReady.store(false);
    }
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
