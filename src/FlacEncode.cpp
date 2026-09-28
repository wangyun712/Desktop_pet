#include "FlacEncode.h"

#include <QFile>

#include <FLAC/metadata.h>
#include <FLAC/stream_encoder.h>

#include <cstdlib>
#include <vector>

namespace {

// libFLAC 的 init_file 走 fopen，MSVC 下按 ANSI 解释路径 —— 歌名全是中文
// 必然打不开。所以走 init_stream 回调，用 QFile 读写，彻底绕开路径编码问题。
FLAC__StreamEncoderWriteStatus writeCb(const FLAC__StreamEncoder*,
                                       const FLAC__byte buffer[], size_t bytes,
                                       uint32_t, uint32_t, void* clientData)
{
    auto* f = static_cast<QFile*>(clientData);
    if (bytes == 0)
        return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
    return f->write(reinterpret_cast<const char*>(buffer), qint64(bytes)) == qint64(bytes)
               ? FLAC__STREAM_ENCODER_WRITE_STATUS_OK
               : FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
}

FLAC__StreamEncoderSeekStatus seekCb(const FLAC__StreamEncoder*,
                                     FLAC__uint64 absoluteByteOffset, void* clientData)
{
    auto* f = static_cast<QFile*>(clientData);
    // 收尾时要回头改写 STREAMINFO（总帧数/MD5），seek 不可用就出不了完整文件
    return f->seek(qint64(absoluteByteOffset))
               ? FLAC__STREAM_ENCODER_SEEK_STATUS_OK
               : FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
}

FLAC__StreamEncoderTellStatus tellCb(const FLAC__StreamEncoder*,
                                     FLAC__uint64* absoluteByteOffset, void* clientData)
{
    auto* f = static_cast<QFile*>(clientData);
    *absoluteByteOffset = FLAC__uint64(f->pos());
    return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
}

// 往 VORBIS_COMMENT 追加一条标签；entry 的内存由本函数负责释放
void addVorbisComment(FLAC__StreamMetadata* block, const char* name, const char* value)
{
    FLAC__StreamMetadata_VorbisComment_Entry entry;
    if (!FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&entry, name, value))
        return;
    FLAC__metadata_object_vorbiscomment_append_comment(block, entry, /*copy=*/true);
    free(entry.entry);
}

} // namespace

namespace FlacEncode {

bool encode(const QByteArray& pcm16, int channels, int sampleRate,
            const QString& title, const QString& artist,
            const QString& outputFlac, QString* errorOut)
{
    auto fail = [errorOut](const QString& msg) {
        if (errorOut)
            *errorOut = msg;
        return false;
    };

    if (channels <= 0 || sampleRate <= 0)
        return fail(QStringLiteral("PCM 参数无效"));
    const int frameBytes = channels * 2;
    if (pcm16.size() < frameBytes || pcm16.size() % frameBytes != 0)
        return fail(QStringLiteral("PCM 数据不完整"));

    FLAC__StreamEncoder* enc = FLAC__stream_encoder_new();
    if (!enc)
        return fail(QStringLiteral("FLAC 编码器创建失败"));

    FLAC__stream_encoder_set_channels(enc, uint32_t(channels));
    FLAC__stream_encoder_set_bits_per_sample(enc, 16);
    FLAC__stream_encoder_set_sample_rate(enc, uint32_t(sampleRate));
    FLAC__stream_encoder_set_compression_level(enc, 5);

    // 标签块：set_metadata 只存指针，块要活到 finish() 之后才能删
    FLAC__StreamMetadata* tags = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
    if (!tags)
    {
        FLAC__stream_encoder_delete(enc);
        return fail(QStringLiteral("FLAC 标签块创建失败"));
    }
    if (!title.isEmpty())
        addVorbisComment(tags, "TITLE", title.toUtf8().constData());
    if (!artist.isEmpty())
        addVorbisComment(tags, "ARTIST", artist.toUtf8().constData());
    FLAC__StreamMetadata* metaArr[1] = { tags };
    FLAC__stream_encoder_set_metadata(enc, metaArr, 1);

    // 先写 .part 临时名，全部编完再改回正式名：进程中途退出留下的残缺
    // 文件叫 ".part"，缓存命中查的是正式名，残件不会被当成好文件反复播放
    QFile out(outputFlac + QStringLiteral(".part"));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        FLAC__metadata_object_delete(tags);
        FLAC__stream_encoder_delete(enc);
        return fail(QStringLiteral("无法写出 FLAC 文件"));
    }

    const FLAC__StreamEncoderInitStatus init =
        FLAC__stream_encoder_init_stream(enc, writeCb, seekCb, tellCb, nullptr, &out);
    if (init != FLAC__STREAM_ENCODER_INIT_STATUS_OK)
    {
        const QString msg = QStringLiteral("FLAC 编码器初始化失败：%1")
                                .arg(QLatin1String(FLAC__StreamEncoderInitStatusString[init]));
        FLAC__metadata_object_delete(tags);
        FLAC__stream_encoder_delete(enc);
        return fail(msg);
    }

    // int16 → FLAC__int32 交错分块喂入（libFLAC 统一按 int32 取样值）
    constexpr qint64 kChunkFrames = 11520;          // 约 0.25 秒 @44.1kHz
    std::vector<FLAC__int32> buf(size_t(kChunkFrames) * size_t(channels));
    const auto* src = reinterpret_cast<const int16_t*>(pcm16.constData());
    const qint64 totalFrames = pcm16.size() / frameBytes;
    qint64 done = 0;
    bool ok = true;
    while (ok && done < totalFrames)
    {
        const uint32_t frames = uint32_t(qMin(kChunkFrames, totalFrames - done));
        const size_t n = size_t(frames) * size_t(channels);
        for (size_t i = 0; i < n; ++i)
            buf[i] = src[done * channels + int(i)];
        ok = FLAC__stream_encoder_process_interleaved(enc, buf.data(), frames) != 0;
        done += frames;
    }
    // finish 会回头补写 STREAMINFO（总帧数/MD5），必须判断返回值
    if (ok && FLAC__stream_encoder_finish(enc) == 0)
        ok = false;

    QString err;
    if (!ok)
        err = QStringLiteral("FLAC 编码失败：%1")
                  .arg(QLatin1String(
                      FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(enc)]));

    out.close();
    FLAC__metadata_object_delete(tags);
    FLAC__stream_encoder_delete(enc);

    if (!ok)
    {
        out.remove();                    // 残缺文件不留
        return fail(err.isEmpty() ? QStringLiteral("FLAC 编码失败") : err);
    }

    out.rename(outputFlac);              // 同目录改名，不存在跨盘问题
    return true;
}

} // namespace FlacEncode
