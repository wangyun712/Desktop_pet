#include "MfDecode.h"

#ifdef Q_OS_WIN

#include <QFile>
#include <QFileInfo>

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

#include <string>

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

void putU32(QFile& f, quint32 v)
{
    unsigned char b[4] = { quint8(v), quint8(v >> 8), quint8(v >> 16), quint8(v >> 24) };
    f.write(reinterpret_cast<const char*>(b), 4);
}

void putU16(QFile& f, quint16 v)
{
    unsigned char b[2] = { quint8(v), quint8(v >> 8) };
    f.write(reinterpret_cast<const char*>(b), 2);
}

} // namespace

namespace MfDecode {

bool decodeToWav(const QString& input, const QString& outputWav, QString* errorOut)
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
    MfPtr<IMFMediaType> native;
    reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &native);
    UINT32 channels = 2, sampleRate = 48000;
    native->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
    native->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate);

    // 输出类型：16bit PCM
    MfPtr<IMFMediaType> target;
    MFCreateMediaType(&target);
    target->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    target->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    target->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    target->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    target->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
    hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, target.get());
    if (FAILED(hr))
    {
        MFShutdown();
        return fail(QStringLiteral("AAC 解码器配置失败"));
    }

    // 解码循环：全部读到内存（一首歌 4 分钟 ≈ 40MB，量级可接受）
    QByteArray pcm;
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
            pcm.append(reinterpret_cast<const char*>(data), int(len));
            buf->Unlock();
        }
    }

    MFShutdown();

    if (pcm.isEmpty())
        return fail(QStringLiteral("没有解出音频数据"));

    // ---- 包 WAV 头（16bit PCM，小端）----
    QFile out(outputWav);
    if (!out.open(QIODevice::WriteOnly))
        return fail(QStringLiteral("无法写出 WAV 文件"));

    const quint32 dataLen  = quint32(pcm.size());
    const quint32 byteRate = channels * sampleRate * 2;

    out.write("RIFF", 4);
    putU32(out, 36 + dataLen);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    putU32(out, 16);              // fmt 块长
    putU16(out, 1);               // PCM
    putU16(out, quint16(channels));
    putU32(out, sampleRate);
    putU32(out, byteRate);
    putU16(out, quint16(channels * 2));   // 块对齐
    putU16(out, 16);              // 位深
    out.write("data", 4);
    putU32(out, dataLen);
    out.write(pcm.constData(), pcm.size());
    out.close();
    return true;
}

} // namespace MfDecode

#else // 非 Windows：编译占位（本项目只发 Windows）

namespace MfDecode {
bool decodeToWav(const QString&, const QString&, QString* errorOut)
{
    if (errorOut)
        *errorOut = QStringLiteral("仅 Windows 支持");
    return false;
}
} // namespace MfDecode

#endif
