#include "MfDecode.h"

#ifdef Q_OS_WIN

#include "FlacEncode.h"

#include <QCoreApplication>
#include <QFile>
#include <QMetaObject>
#include <QPointer>

#include <objbase.h>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

#include <string>
#include <thread>

namespace {

// 简易 RAII：MF 对象手动 Release 太容易漏，包一个只干这一件事的守护
template <typename T>
struct MfPtr
{
    T* p = nullptr;
    MfPtr() = default;
    ~MfPtr() { if (p) p->Release(); }
    T**       operator&()       { return &p; }
    T*        operator->() const { return p; }
    T*        get()       const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

struct DecodedPcm
{
    QByteArray pcm;
    UINT32 channels   = 2;
    UINT32 sampleRate = 48000;
};

// 解码内核：AAC/M4S → 16bit 交错小端 PCM（声道数/采样率随源走）。
// 只做解码，写文件交给 FlacEncode —— 职责分开，各自都能单测。
bool decodePcm(const QString& input, DecodedPcm& out, QString* errorOut)
{
    auto fail = [errorOut](const QString& msg) {
        if (errorOut)
            *errorOut = msg;
        return false;
    };

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr))
        return fail(QStringLiteral("Media Foundation 初始化失败"));

    MfPtr<IMFSourceReader> reader;
    const std::wstring wpath = input.toStdWString();
    hr = MFCreateSourceReaderFromURL(wpath.c_str(), nullptr, &reader);
    if (FAILED(hr))
    {
        MFShutdown();
        return fail(QStringLiteral("打不开音频文件（不支持该 AAC 容器）"));
    }

    // 只选第一条音频流，其它流（视频/字幕）全部关掉
    reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

    // 探一下源格式（声道数 / 采样率随源走，位深固定 16）
    // ★ 这一步必须判失败 ★ 没有音频轨的 m4a/m4s、或者容器认得出但流取不到时，
    //   GetNativeMediaType 会返回失败并把 native 留成空指针 —— 不判就
    //   native->GetUINT32() 是空指针调虚函数，直接崩。宁可在这里报错，
    //   让调用方把"该资源无法播放"显示到列表行上。
    MfPtr<IMFMediaType> native;
    hr = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &native);
    if (FAILED(hr) || !native)
    {
        MFShutdown();
        return fail(QStringLiteral("读不到音频流格式（这条资源可能没有音频轨）"));
    }
    // ★ 两个字段必须读到 ★ 读失败会静默落在默认值 2ch/48kHz 上,
    // 把非常规流编成变速变调的废文件 —— 不如明确报"无法播放"
    if (FAILED(native->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &out.channels))
        || FAILED(native->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &out.sampleRate)))
    {
        MFShutdown();
        return fail(QStringLiteral("音频流缺少声道/采样率信息"));
    }
    if (out.channels == 0 || out.sampleRate == 0)
    {
        MFShutdown();
        return fail(QStringLiteral("音频流格式不可用（声道数 / 采样率为 0）"));
    }

    // 输出类型：16bit PCM
    MfPtr<IMFMediaType> target;
    MFCreateMediaType(&target);
    target->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    target->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    target->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    target->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, out.channels);
    target->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, out.sampleRate);
    hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, target.get());
    if (FAILED(hr))
    {
        MFShutdown();
        return fail(QStringLiteral("AAC 解码器配置失败"));
    }

    // 解码循环：全部读到内存（一首歌 4 分钟 ≈ 40MB，量级可接受）
    for (;;)
    {
        DWORD flags = 0;
        MfPtr<IMFSample> sample;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
                                nullptr, &flags, nullptr, &sample);
        if (FAILED(hr))
        {
            MFShutdown();
            return fail(QStringLiteral("音频解码中断"));
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            break;
        if (!sample)
            continue;

        MfPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf)))
            continue;

        BYTE* data = nullptr;
        DWORD len = 0;
        if (SUCCEEDED(buf->Lock(&data, nullptr, &len)) && data)
        {
            out.pcm.append(reinterpret_cast<const char*>(data), int(len));
            buf->Unlock();
        }
    }

    MFShutdown();

    if (out.pcm.isEmpty())
        return fail(QStringLiteral("没有解出音频数据"));
    return true;
}

} // namespace

namespace MfDecode {

bool decodeToFlac(const QString& input, const QString& outputFlac,
                  const QString& title, const QString& artist,
                  QString* errorOut)
{
    QString err;
    DecodedPcm d;
    if (!decodePcm(input, d, &err))
    {
        if (errorOut)
            *errorOut = err;
        return false;
    }
    return FlacEncode::encode(d.pcm, int(d.channels), int(d.sampleRate),
                              title, artist, outputFlac, errorOut);
}

void decodeToFlacAsync(QObject* ctx, const QString& input, const QString& outputFlac,
                       const QString& title, const QString& artist,
                       std::function<void(bool ok, const QString& err)> cb)
{
    // QPointer 跨线程判断宿主是否还活着：窗口都关了就没必要再回头调 UI
    QPointer<QObject> guard(ctx);
    std::thread([guard, input, outputFlac, title, artist,
                 cb = std::move(cb)]() mutable {
        // MF 要求所在线程先初始化 COM；主线程 Qt 已经按 STA 起过，
        // 这里是新线程得自己来（MTA 即可）。RPC_E_CHANGED_MODE 说明
        // 已被别处按 STA 初始化，同样能跑，只是不能配对 Uninitialize。
        const HRESULT cohr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool needCoUninit = SUCCEEDED(cohr);

        QString err;
        const bool ok = decodeToFlac(input, outputFlac, title, artist, &err);

        if (needCoUninit)
            CoUninitialize();

        // 派回主线程。上下文用 qApp（活到进程结束）：invokeMethod 内部要
        // postEvent 解引用上下文,若直接用宿主指针,"判活通过之后、postEvent
        // 之前宿主恰好析构"的纳秒级 TOCTOU 就是崩溃;真宿主的判活放进
        // lambda 里,窗口没了就丢弃结果。
        QPointer<QObject> g = guard.data();
        QMetaObject::invokeMethod(QCoreApplication::instance(),
            [g, ok, err, cb = std::move(cb)] {
                if (g)
                    cb(ok, err);
            },
            Qt::QueuedConnection);
    }).detach();
}

} // namespace MfDecode

#else // 非 Windows：编译占位（本项目只发 Windows）

namespace MfDecode {

bool decodeToFlac(const QString&, const QString&, const QString&, const QString&,
                  QString* errorOut)
{
    if (errorOut)
        *errorOut = QStringLiteral("仅 Windows 支持");
    return false;
}

void decodeToFlacAsync(QObject*, const QString&, const QString&,
                       const QString&, const QString&,
                       std::function<void(bool ok, const QString& err)> cb)
{
    if (cb)
        cb(false, QStringLiteral("仅 Windows 支持"));
}

} // namespace MfDecode

#endif
