#pragma once
// =============================================================================
//  TrackMeta —— 从音频文件里读出「标题 / 艺术家 / 专辑 / 封面 / 歌词 / 时长」
//
//  ★ 为什么自己写，不用 TagLib ★
//    TagLib 要拉源码、编成库、再挂进 CMake；而播放器要的只是"读"，
//    不写标签、不重编码。所以这里只做读取，纯头文件、纯函数，
//    不引第三方运行时、不联网。
//
//  支持三种最主流的容器（本地曲库的 99%）：
//    · MP3     —— ID3v2.3 / ID3v2.4，读不到再退回文件末尾的 ID3v1
//    · FLAC    —— STREAMINFO 拿精确时长，VORBIS_COMMENT 拿文本，PICTURE 拿封面
//    · M4A/MP4 —— moov.udta.meta.ilst 里的 ©nam / ©ART / ©alb / ©lyr / covr
//
//  ---------------------------------------------------------------------------
//  ▎编码降级链 —— 国内老 MP3 的重灾区
//
//  ID3v2 头部理论上有一个"编码字节"告诉你怎么解那串字节，但实际上
//  "标着 Latin-1、内容其实是 GBK"极其常见（2000 年代那批 Windows 工具干的）。
//  所以除 UTF-16 那两种（有明确 BOM / 字节序，能解准）之外，文本一律走：
//
//        严格 UTF-8  ──失败──▶  本地编码 System（中文 Windows = GBK）  ──失败──▶  Latin-1
//
//  最后那一步是"绝不返回空串"的保险：宁愿显示一两个乱码字，
//  也好过整首歌的标题凭空消失。
//  （反过来，"GBK 字节恰好是合法 UTF-8"的串会解错 —— 这是无解的，
//    代码里没有 hook 能救，只能承认。）
//
//  ---------------------------------------------------------------------------
//  ▎只有"必须读"的地方才去读文件
//
//  这个头文件会被列表模型逐行调用，几万首的库如果每行都读整文件会卡到不能用。
//  所以每个解析器都只读它在结构上"必须读到"的那一段：
//    · MP3 的 ID3v2 一定在文件最前 → 读头部（读到哪里由标签自己声明的大小决定）
//    · ID3v1 一定在文件最后 128 字节 → 只 seek 到末尾读 128 字节
//    · MP3 时长用"首个有效帧头的比特率"估算 → 只扫前 64KB
//    · FLAC / MP4 的结构块也在前部（moov 在末尾的 m4a 会走一次顶层遍历）
//  这就是"扫一个几万首的大文件夹也只要几秒"的全部秘密。
//
//  ---------------------------------------------------------------------------
//  ▎歌词从哪来（自上而下，先命中先用）
//    ① 同目录同名的 .lrc 文件   —— 用户可以自己改词，最灵活，所以放最前
//    ② 内嵌歌词（USLT / ©lyr / VORBIS LYRICS）
//    ③ 都没有 → 界面显示"没有歌词"，不是错误
// =============================================================================

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QString>
#include <QStringConverter>
#include <QVector>

#include <algorithm>

namespace TrackMeta {

// -----------------------------------------------------------------------------
//  读文件的一小段
// -----------------------------------------------------------------------------
inline QByteArray readRange(const QString& path, qint64 offset, qint64 length)
{
    if (length <= 0)
        return QByteArray();

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    if (offset > 0 && !f.seek(offset))
        return QByteArray();

    return f.read(length);
}

inline qint64 fileSize(const QString& path)
{
    return QFileInfo(path).size();
}

// -----------------------------------------------------------------------------
//  大端 / 小端整数（标签格式里两种都有，硬编不能混）
// -----------------------------------------------------------------------------
inline quint32 beU32(const QByteArray& b, int off)
{
    if (off < 0 || off + 4 > b.size())
        return 0;
    return (quint32(quint8(b.at(off))) << 24) | (quint32(quint8(b.at(off + 1))) << 16)
         | (quint32(quint8(b.at(off + 2))) << 8) | quint32(quint8(b.at(off + 3)));
}

inline quint32 leU32(const QByteArray& b, int off)
{
    if (off < 0 || off + 4 > b.size())
        return 0;
    return quint32(quint8(b.at(off))) | (quint32(quint8(b.at(off + 1))) << 8)
         | (quint32(quint8(b.at(off + 2))) << 16) | (quint32(quint8(b.at(off + 3))) << 24);
}

inline quint16 beU16(const QByteArray& b, int off)
{
    if (off < 0 || off + 2 > b.size())
        return 0;
    return quint16((quint16(quint8(b.at(off))) << 8) | quint16(quint8(b.at(off + 1))));
}

// ID3v2 的"syncsafe"整数：每字节只用低 7 位，最高位永远是 0（防止被当成帧同步头）。
inline quint32 syncSafeU32(const QByteArray& b, int off)
{
    if (off < 0 || off + 4 > b.size())
        return 0;
    return (quint32(quint8(b.at(off)) & 0x7F) << 21)
         | (quint32(quint8(b.at(off + 1)) & 0x7F) << 14)
         | (quint32(quint8(b.at(off + 2)) & 0x7F) << 7)
         | (quint32(quint8(b.at(off + 3)) & 0x7F));
}

// 去掉结尾的 NUL 填充（标签里的字符串常常拿 0 补满固定长度）
inline QByteArray chopZeros(QByteArray b)
{
    while (!b.isEmpty() && b.endsWith('\0'))
        b.chop(1);
    return b;
}

// -----------------------------------------------------------------------------
//  解码一条文本 —— 降级链见文件开头
// -----------------------------------------------------------------------------
inline QString decodeSmart(const QByteArray& raw)
{
    const QByteArray b = chopZeros(raw);
    if (b.isEmpty())
        return QString();

    // ① 严格 UTF-8。只要有一个非法序列就整体放弃 —— 半对半错比全错更难查。
    {
        QStringDecoder d(QStringConverter::Utf8);
        const QString s = d(b);
        if (!d.hasError())
            return s;
    }

    // ② 本地编码（中文 Windows 上是 CP936/GBK）
    {
        QStringDecoder d(QStringConverter::System);
        const QString s = d(b);
        if (!d.hasError() && !s.isEmpty())
            return s;
    }

    // ③ 保底：Latin-1 对任意字节都有定义，绝不返回空
    return QString::fromLatin1(b);
}

// UTF-16 大端。QString::fromUtf16 只认"本机字节序"（x86 上是小端），
// 碰到 BE 的内容会解出一串错位的汉字 —— 2026-09-23 就先栽在这上面，
// 所以这里手工按大端拼一遍，不赌平台。
inline QString fromUtf16BigEndian(const QByteArray& b)
{
    const int n = b.size() / 2;
    QString s;
    s.reserve(n);
    for (int i = 0; i < n; ++i)
        s.append(QChar(quint16((quint16(quint8(b.at(2 * i))) << 8)
                               |  quint16(quint8(b.at(2 * i + 1))))));
    return s;
}

inline QString fromUtf16LittleEndian(const QByteArray& b)
{
    const int n = b.size() / 2;
    QString s;
    s.reserve(n);
    for (int i = 0; i < n; ++i)
        s.append(QChar(quint16(quint16(quint8(b.at(2 * i)))
                               | (quint16(quint8(b.at(2 * i + 1))) << 8))));
    return s;
}

// 按 ID3v2 头部那个"编码字节"解码
inline QString decodeId3Text(quint8 enc, const QByteArray& raw)
{
    QByteArray b = raw;

    switch (enc)
    {
    case 0:      // ISO-8859-1 —— 名义上是它，实际上十有八九是 GBK
        return decodeSmart(b);

    case 1:      // UTF-16，带 BOM
    {
        if (b.size() >= 2 && quint8(b.at(0)) == 0xFF && quint8(b.at(1)) == 0xFE)
            return fromUtf16LittleEndian(b.mid(2));
        if (b.size() >= 2 && quint8(b.at(0)) == 0xFE && quint8(b.at(1)) == 0xFF)
            return fromUtf16BigEndian(b.mid(2));
        return fromUtf16LittleEndian(b);       // 没 BOM 就按小端（Windows 上的主流写法）
    }

    case 2:      // UTF-16BE（无 BOM）
        return fromUtf16BigEndian(b);

    case 3:      // UTF-8
        return decodeSmart(b);

    default:
        return decodeSmart(b);
    }
}

// 跳过一段"以编码字节描述的"字符串（APIC 的 description、USLT 的 content descriptor），
// 返回跳过之后的下标。UTF-16 的结束符是两个 0 字节，所以要按 2 步进。
inline int skipEncodedString(const QByteArray& b, int pos, quint8 enc)
{
    if (enc == 1 || enc == 2)
    {
        while (pos + 1 < b.size())
        {
            if (b.at(pos) == 0 && b.at(pos + 1) == 0)
                return pos + 2;
            pos += 2;
        }
        return b.size();
    }

    while (pos < b.size())
    {
        if (b.at(pos) == 0)
            return pos + 1;
        ++pos;
    }
    return b.size();
}

// -----------------------------------------------------------------------------
//  Meta —— 从文件里读到的一切
// -----------------------------------------------------------------------------
struct Meta
{
    QString    title;
    QString    artist;
    QString    album;
    int        durationMs = 0;
    QByteArray cover;          // 原始 JPEG/PNG 字节；空 = 没有封面
    QString    lyrics;         // 歌词正文（可能带 [mm:ss] 时间标签）
    QString    lyricSource;    // "lrc" / "内嵌" / "" —— 自检和界面提示用
    QString    tagSource;      // "ID3v2" / "ID3v1" / "FLAC" / "MP4" / "" —— 自检用
    bool       guessedFromFileName = false;

    bool hasCover() const { return !cover.isEmpty(); }
};

// -----------------------------------------------------------------------------
//  单元 ①：ID3v1 —— 文件末尾 128 字节
//  很土，但胜在"永远在那儿"：ID3v2 被别的工具写坏时，它常常还活着。
// -----------------------------------------------------------------------------
inline int estimateMp3DurationMs(const QString& path, qint64 id3v2Size);   // 定义见单元 ③

inline void parseId3v1(const QString& path, Meta& m)
{
    const qint64 sz = fileSize(path);
    if (sz < 128)
        return;

    const QByteArray tail = readRange(path, sz - 128, 128);
    if (tail.size() != 128 || !tail.startsWith("TAG"))
        return;

    const auto field = [&tail](int off, int len) {
        return decodeSmart(tail.mid(off, len)).trimmed();
    };

    if (m.title.isEmpty())
        m.title = field(3, 30);
    if (m.artist.isEmpty())
        m.artist = field(33, 30);
    if (m.album.isEmpty())
        m.album = field(63, 30);

    if (!m.title.isEmpty() || !m.artist.isEmpty())
        m.tagSource = QStringLiteral("ID3v1");
}

// -----------------------------------------------------------------------------
//  单元 ②：ID3v2.3 / ID3v2.4
// -----------------------------------------------------------------------------
inline void parseId3v2(const QString& path, Meta& m)
{
    const QByteArray head = readRange(path, 0, 10);
    if (head.size() < 10 || !head.startsWith("ID3"))
        return;

    const int major = quint8(head.at(3));          // 3 = v2.3，4 = v2.4
    if (major != 3 && major != 4)
        return;                                    // v2.2（3 字节帧 id）不伺候，太老了

    const quint32 bodySize = syncSafeU32(head, 6);
    if (bodySize == 0 || bodySize > 64u * 1024u * 1024u)   // 明显不正常的直接放弃
        return;

    const QByteArray body = readRange(path, 10, qint64(bodySize));
    if (body.isEmpty())
        return;

    const quint8 flags = quint8(head.at(5));
    int pos = 0;

    // 扩展头（很少见，但格式上必须跳过，否则第一个帧 id 会对不上）
    if (flags & 0x40)
    {
        if (major == 4)
            pos += int(syncSafeU32(body, 0));                // v2.4：整个扩展头的大小（含自身）
        else
            pos += 4 + int(beU32(body, 0));                  // v2.3：前 4 字节是大小（不含自身）
    }

    while (pos + 10 <= body.size())
    {
        if (quint8(body.at(pos)) == 0)                       // 到底了（padding 全 0）
            break;

        const QByteArray idRaw = body.mid(pos, 4);
        if (idRaw.size() < 4)
            break;

        const quint32 frameSize = (major == 4) ? syncSafeU32(body, pos + 4)
                                               : beU32(body, pos + 4);
        if (frameSize == 0 || frameSize > quint32(body.size()))
            break;                                           // 帧大小离谱 -> 结构已经坏了

        const int dataOff = pos + 10;
        const QByteArray fdata = body.mid(dataOff, int(frameSize));
        const QString id = QString::fromLatin1(idRaw);

        if (id == QLatin1String("TIT2"))
        {
            if (!fdata.isEmpty())
                m.title = decodeId3Text(quint8(fdata.at(0)), fdata.mid(1)).trimmed();
        }
        else if (id == QLatin1String("TPE1"))
        {
            if (!fdata.isEmpty())
                m.artist = decodeId3Text(quint8(fdata.at(0)), fdata.mid(1)).trimmed();
        }
        else if (id == QLatin1String("TALB"))
        {
            if (!fdata.isEmpty())
                m.album = decodeId3Text(quint8(fdata.at(0)), fdata.mid(1)).trimmed();
        }
        else if (id == QLatin1String("APIC"))
        {
            // enc(1) mime\0 picType(1) description(按 enc) 图片数据
            if (fdata.size() > 4 && m.cover.isEmpty())
            {
                const quint8 enc = quint8(fdata.at(0));
                int p = 1;
                while (p < fdata.size() && fdata.at(p) != 0)   // mime 一定是 ASCII，找 \0
                    ++p;
                ++p;                                            // 跳过 \0
                ++p;                                            // 跳过 picture type
                p = skipEncodedString(fdata, p, enc);           // 跳过 description

                if (p < fdata.size())
                    m.cover = fdata.mid(p);                     // 剩下的就是图片字节
            }
        }
        else if (id == QLatin1String("USLT"))
        {
            // enc(1) language(3) contentDescriptor(按 enc) 歌词正文(按 enc)
            if (fdata.size() > 5 && m.lyrics.isEmpty())
            {
                const quint8 enc = quint8(fdata.at(0));
                int p = skipEncodedString(fdata, 4, enc);
                if (p < fdata.size())
                {
                    // ★ 这里必须是 QByteArray，不能是 QString ★
                    //   写成 QString 会走"QByteArray 隐式转 QString"（按 Latin-1 解），
                    //   中文歌词在进 decodeId3Text 之前就已经被解坏一次了。
                    const QByteArray text = chopZeros(fdata.mid(p));
                    m.lyrics = decodeId3Text(enc, text);
                }
            }
        }
        else if (id == QLatin1String("SYLT"))
        {
            // 同步歌词（带时间戳的结构化格式）。极少数文件才有，
            // 而 .lrc / USLT 已经覆盖了绝大多数场景 —— 这里不解析，
            // 免得为一首歌写两百行。真有需要时看这里就知道该从哪下手。
        }

        pos = dataOff + int(frameSize);
    }

    if (!m.title.isEmpty() || !m.artist.isEmpty() || !m.album.isEmpty() || m.hasCover())
        m.tagSource = QStringLiteral("ID3v2.%1").arg(major);

    // 时长：ID3v2 这个块占了文件最前面多少字节，到这里才确定。
    // 不把它扣掉的话，一个带封面的标签能有几 MB，估算出来的时间会凭空多出几十秒。
    if (m.durationMs == 0)
        m.durationMs = estimateMp3DurationMs(path, 10 + qint64(bodySize));
}

// -----------------------------------------------------------------------------
//  单元 ③：MP3 时长估算
//
//  找第一个看起来合法的帧头，拿它的比特率当整首的平均比特率来估。
//  CBR 文件是准的；VBR 文件会偏（有 Xing 头才能算准，这里不做）——
//  列表上显示个大概就够，播放时进度条走的是解码器给的真实位置，不受影响。
// -----------------------------------------------------------------------------
inline int estimateMp3DurationMs(const QString& path, qint64 id3v2Size)
{
    const QByteArray probe = readRange(path, qMax<qint64>(0, id3v2Size), 64 * 1024);
    if (probe.size() < 4)
        return 0;

    static const int brV1L1[]  = { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448 };
    static const int brV1L2[]  = { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 };
    static const int brV1L3[]  = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 };
    static const int brV2L23[] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 };

    for (int i = 0; i + 4 <= probe.size(); ++i)
    {
        const int h0 = quint8(probe.at(i));
        const int h1 = quint8(probe.at(i + 1));
        const int h2 = quint8(probe.at(i + 2));

        if (h0 != 0xFF || (h1 & 0xE0) != 0xE0)
            continue;

        const int verBits   = (h1 >> 3) & 0x03;      // 3=MPEG1  2=MPEG2  0=MPEG2.5  1=保留
        const int layerBits = (h1 >> 1) & 0x03;      // 3=Layer1 2=Layer2 1=Layer3
        const int brIdx     = (h2 >> 4) & 0x0F;
        const int srIdx     = (h2 >> 2) & 0x03;

        if (verBits == 1 || layerBits == 0 || brIdx == 0 || brIdx == 15 || srIdx == 3)
            continue;                                 // 这个"同步头"是假的，继续往后找

        int kbps = 0;
        if (verBits == 3)
            kbps = (layerBits == 3) ? brV1L1[brIdx] : (layerBits == 2 ? brV1L2[brIdx] : brV1L3[brIdx]);
        else
            kbps = brV2L23[brIdx];                   // MPEG2 / 2.5：Layer2 和 Layer3 共用一张表

        if (kbps <= 0)
            continue;

        // ID3v1 那 128 字节不算音频
        qint64 audioBytes = fileSize(path) - id3v2Size;
        if (audioBytes > 128)
        {
            const QByteArray tail = readRange(path, fileSize(path) - 128, 3);
            if (tail == QByteArray("TAG"))
                audioBytes -= 128;
        }
        if (audioBytes <= 0)
            return 0;

        return int(double(audioBytes) * 8.0 * 1000.0 / (double(kbps) * 1000.0));
    }

    return 0;
}

// -----------------------------------------------------------------------------
//  单元 ④：FLAC
// -----------------------------------------------------------------------------
inline void parseFlac(const QString& path, Meta& m)
{
    const qint64 sz = fileSize(path);
    if (sz < 8)
        return;

    // 一次性读前 4MB：STREAMINFO 一定在最前，VORBIS_COMMENT / PICTURE 也几乎都在前部。
    // （封面特别大的少数文件会读漏，那种情况退化成"没封面"，不影响播放。）
    const QByteArray buf = readRange(path, 0, 4 * 1024 * 1024);
    if (buf.size() < 8 || !buf.startsWith("fLaC"))
        return;

    m.tagSource = QStringLiteral("FLAC");

    int pos = 4;
    bool gotStreamInfo = false;

    while (pos + 4 <= buf.size())
    {
        const quint8 header      = quint8(buf.at(pos));
        const bool   isLast      = (header & 0x80) != 0;
        const int    blockType   = header & 0x7F;
        const int    blockLen    = int((quint32(quint8(buf.at(pos + 1))) << 16)
                                     | (quint32(quint8(buf.at(pos + 2))) << 8)
                                     |  quint32(quint8(buf.at(pos + 3))));
        const int    dataOff     = pos + 4;

        if (blockLen < 0 || dataOff + blockLen > buf.size())
            break;                                   // 超出去的那块没读到，作罢

        const QByteArray blk = buf.mid(dataOff, blockLen);

        if (blockType == 0 && blk.size() >= 34 && !gotStreamInfo)
        {
            // STREAMINFO 的最后 8 个"有效位"里塞了采样率/声道/位深/总样本数
            quint64 v = 0;
            for (int i = 10; i < 18; ++i)
                v = (v << 8) | quint64(quint8(blk.at(i)));

            const quint32 sampleRate    = quint32(v >> 44);           // 高 20 位
            const quint64 totalSamples  = v & 0xFFFFFFFFFull;         // 低 36 位

            if (sampleRate > 0 && totalSamples > 0)
                m.durationMs = int(double(totalSamples) * 1000.0 / double(sampleRate));

            gotStreamInfo = true;
        }
        else if (blockType == 4)
        {
            // VORBIS_COMMENT：全小端
            int p = 0;
            const quint32 vendorLen = leU32(blk, p);
            p += 4 + int(vendorLen);

            const quint32 count = leU32(blk, p);
            p += 4;

            for (quint32 i = 0; i < count && p + 4 <= blk.size(); ++i)
            {
                const quint32 len = leU32(blk, p);
                p += 4;
                if (len == 0 || p + int(len) > blk.size())
                    break;

                const QString entry = QString::fromUtf8(blk.mid(p, int(len)));
                p += int(len);

                const int eq = entry.indexOf(QLatin1Char('='));
                if (eq <= 0)
                    continue;

                const QString key = entry.left(eq).toUpper();
                const QString val = entry.mid(eq + 1);

                if (key == QLatin1String("TITLE") && m.title.isEmpty())
                    m.title = val;
                else if (key == QLatin1String("ARTIST") && m.artist.isEmpty())
                    m.artist = val;
                else if (key == QLatin1String("ALBUM") && m.album.isEmpty())
                    m.album = val;
                else if ((key == QLatin1String("LYRICS") || key == QLatin1String("UNSYNCEDLYRICS"))
                         && m.lyrics.isEmpty())
                {
                    m.lyrics = val;
                }
                // METADATA_BLOCK_PICTURE（base64 的图片块）这里不展开 ——
                // FLAC 的封面绝大多数直接放在 PICTURE 块里，下面那支就够了。
            }
        }
        else if (blockType == 6)
        {
            // PICTURE：type(4) mimeLen(4) mime descLen(4) desc w(4) h(4) depth(4) colors(4) dataLen(4) data
            int p = 4;                                   // 跳过 picture type
            const quint32 mimeLen = beU32(blk, p);
            p += 4 + int(mimeLen);
            const quint32 descLen = beU32(blk, p);
            p += 4 + int(descLen);
            p += 16;                                     // 宽高深色 4 个 4 字节
            const quint32 dataLen = beU32(blk, p);
            p += 4;

            if (m.cover.isEmpty() && dataLen > 0 && p + int(dataLen) <= blk.size())
                m.cover = blk.mid(p, int(dataLen));
        }

        pos = dataOff + blockLen;
        if (isLast || blockType == 127)
            break;
    }
}

// -----------------------------------------------------------------------------
//  单元 ⑤：M4A / MP4
//
//  atom 结构是嵌套的：moov → udta → meta → ilst → (©nam … ) → data。
//  ★ 注意 moov 不一定在文件开头 ★：没做 faststart 的 m4a 会把 moov 放在**末尾**，
//    所以这里老老实实从第 0 字节开始遍历顶层 atom，不能只读前几 KB。
// -----------------------------------------------------------------------------
namespace Mp4 {

// 在 [begin,end) 里遍历子 atom
inline void walkAtoms(const QByteArray& d, int begin, int end, Meta& m, int depth)
{
    if (depth > 6)
        return;

    int pos = begin;
    while (pos + 8 <= end)
    {
        quint32 size = beU32(d, pos);
        const QByteArray typeRaw = d.mid(pos + 4, 4);
        const QString type = QString::fromLatin1(typeRaw);

        int headerLen = 8;
        if (size == 1)                                   // 64 位大小
        {
            const quint32 hi = beU32(d, pos + 8);
            const quint32 lo = beU32(d, pos + 12);
            size = (hi == 0) ? lo : quint32(-1);
            headerLen = 16;
        }
        else if (size == 0)
        {
            size = quint32(end - pos);                   // 0 = 一直到容器末尾
        }

        if (size < quint32(headerLen) || pos + qint64(size) > end)
            break;

        const int cBegin = pos + headerLen;
        const int cEnd   = pos + int(size);

        // meta 是个 fullbox：内容最前面多 4 个字节的 version/flags
        const int childBegin = (type == QLatin1String("meta")) ? cBegin + 4 : cBegin;

        if (type == QLatin1String("mvhd") && m.durationMs == 0)
        {
            const int ver = (cBegin < cEnd) ? quint8(d.at(cBegin)) : 0;
            if (ver == 1 && cBegin + 32 <= cEnd)
            {
                const quint32 timescale = beU32(d, cBegin + 20);
                const quint32 hi = beU32(d, cBegin + 24);
                const quint32 lo = beU32(d, cBegin + 28);
                const quint64 dur = (quint64(hi) << 32) | quint64(lo);
                if (timescale > 0)
                    m.durationMs = int(double(dur) * 1000.0 / double(timescale));
            }
            else if (cBegin + 20 <= cEnd)
            {
                const quint32 timescale = beU32(d, cBegin + 12);
                const quint32 dur       = beU32(d, cBegin + 16);
                if (timescale > 0)
                    m.durationMs = int(double(dur) * 1000.0 / double(timescale));
            }
        }
        else if (type == QLatin1String("moov") || type == QLatin1String("udta")
                 || type == QLatin1String("meta") || type == QLatin1String("ilst"))
        {
            walkAtoms(d, childBegin, cEnd, m, depth + 1);
        }
        else if (typeRaw.startsWith("\xA9") || typeRaw == QByteArray("covr"))
        {
            // ilst 的一个条目：里面通常就一个 data 子 atom
            //   data: size(4) 'data' type(4) locale(4) payload
            int p = cBegin;
            while (p + 8 <= cEnd)
            {
                const quint32 dsize = beU32(d, p);
                const QByteArray dtype = d.mid(p + 4, 4);
                if (dsize < 16 || p + qint64(dsize) > cEnd)
                    break;

                if (dtype == QByteArray("data"))
                {
                    const quint32 wellKnown = beU32(d, p + 8) & 0x00FFFFFF;
                    const QByteArray payload = d.mid(p + 16, int(dsize) - 16);

                    if (typeRaw == QByteArray("covr"))
                    {
                        // 13 = JPEG，14 = PNG
                        if (m.cover.isEmpty() && (wellKnown == 13 || wellKnown == 14))
                            m.cover = payload;
                    }
                    else if (wellKnown == 1)           // UTF-8 文本
                    {
                        const QString text = QString::fromUtf8(payload).trimmed();
                        // ★ 这里一律写 "\xA9" "nam" 而不是 "\xA9nam" ★
                        //   C++ 的十六进制转义会一直吃到第一个"非十六进制字符"为止，
                        //   而 A / a 恰好都是十六进制数字 —— 写 "\xA9ART" 会被解析成
                        //   \xA9A + "RT"，直接编译不过（C7744 转义超出范围）。
                        //   拆成两个相邻字面量最省事，也最不容易看错。
                        if (typeRaw == QByteArray("\xA9" "nam") && m.title.isEmpty())
                            m.title = text;
                        else if (typeRaw == QByteArray("\xA9" "ART") && m.artist.isEmpty())
                            m.artist = text;
                        else if (typeRaw == QByteArray("\xA9" "alb") && m.album.isEmpty())
                            m.album = text;
                        else if (typeRaw == QByteArray("\xA9" "lyr") && m.lyrics.isEmpty())
                            m.lyrics = text;
                    }
                }
                p += int(dsize);
            }
        }

        pos = cEnd;
    }
}

} // namespace Mp4

inline void parseMp4(const QString& path, Meta& m)
{
    const qint64 sz = fileSize(path);
    if (sz < 16)
        return;

    const QByteArray head = readRange(path, 0, 16);
    if (head.size() < 12)
        return;

    int pos = 0;
    int guard = 0;                                        // 防呆：atom 表坏掉时别转死循环
    while (pos + 8 <= sz && guard++ < 256)
    {
        const QByteArray h = readRange(path, pos, 16);
        if (h.size() < 8)
            break;

        quint32 size = beU32(h, 0);
        const QByteArray type = h.mid(4, 4);

        int headerLen = 8;
        if (size == 1 && h.size() >= 16)
        {
            const quint32 hi = beU32(h, 8);
            const quint32 lo = beU32(h, 12);
            size = (hi == 0) ? lo : 0;
            headerLen = 16;
        }
        else if (size == 0)
        {
            size = quint32(sz - pos);
        }

        if (size < quint32(headerLen))
            break;

        if (type == QByteArray("moov"))
        {
            // moov 里含封面，可能一两 MB；读它，然后停止遍历
            const QByteArray moov = readRange(path, pos + headerLen, qint64(size) - headerLen);
            if (!moov.isEmpty())
            {
                Meta tmp;
                Mp4::walkAtoms(moov, 0, moov.size(), tmp, 0);
                tmp.tagSource = QStringLiteral("MP4");
                m.title  = m.title.isEmpty()  ? tmp.title  : m.title;
                m.artist = m.artist.isEmpty() ? tmp.artist : m.artist;
                m.album  = m.album.isEmpty()  ? tmp.album  : m.album;
                m.lyrics = m.lyrics.isEmpty() ? tmp.lyrics : m.lyrics;
                if (m.cover.isEmpty())
                    m.cover = tmp.cover;
                if (m.durationMs == 0)
                    m.durationMs = tmp.durationMs;
            }
            break;
        }

        pos += int(size);
    }

    if (!m.title.isEmpty() || !m.artist.isEmpty() || m.hasCover())
        m.tagSource = QStringLiteral("MP4");
}

// -----------------------------------------------------------------------------
//  单元 ⑥：WAV
//  最土的容器：没有标签，但时长是精确的（RIFF 头里就写着字节率和数据长度）。
//  自检要拿它造样本 —— 一个只有头 + 静音数据的 wav 就能手工拼出来，
//  不需要任何编码器，正好用来验证"时长算得对不对"。
// -----------------------------------------------------------------------------
inline void parseWav(const QString& path, Meta& m)
{
    const QByteArray buf = readRange(path, 0, 4 * 1024 * 1024);
    if (buf.size() < 12 || !buf.startsWith("RIFF") || buf.mid(8, 4) != QByteArray("WAVE"))
        return;

    m.tagSource = QStringLiteral("WAV");

    quint32 byteRate = 0;
    int pos = 12;
    while (pos + 8 <= buf.size())
    {
        const QByteArray id = buf.mid(pos, 4);
        const quint32    size = leU32(buf, pos + 4);
        const int        dataOff = pos + 8;

        if (id == QByteArray("fmt ") && size >= 16 && dataOff + 16 <= buf.size())
        {
            byteRate = leU32(buf, dataOff + 8);
        }
        else if (id == QByteArray("data"))
        {
            // 流式写出来的 wav 会把 size 写成 0 或 0xFFFFFFFF（"我也不知道多长"），
            // 这种情况按"文件剩余长度"来算，比直接放弃强。
            qint64 bytes = qint64(size);
            const qint64 remain = fileSize(path) - dataOff;
            if (bytes <= 0 || bytes > remain)
                bytes = remain;

            if (byteRate > 0 && bytes > 0)
                m.durationMs = int(double(bytes) * 1000.0 / double(byteRate));
            break;
        }

        if (size == 0)
            break;
        pos = dataOff + int(size);
    }
}

// -----------------------------------------------------------------------------
//  单元 ⑦：文件名兜底
//
//  标签读不到（或者只有一半）时，从文件名里猜。认这三种写法：
//    「艺术家 - 标题」 / 「01. 标题」 / 「01 - 艺术家 - 标题」
// -----------------------------------------------------------------------------
inline void applyFileNameGuess(const QString& path, Meta& m)
{
    const QFileInfo fi(path);
    QString base = fi.completeBaseName();
    if (base.isEmpty())
        return;

    QString rest = base;
    rest.remove(QRegularExpression(QStringLiteral("^\\s*\\d{1,3}\\s*[\\.\\-、_]\\s*")));   // 去曲号
    rest = rest.trimmed();
    if (rest.isEmpty())
        rest = base;

    if (m.artist.isEmpty() || m.title.isEmpty())
    {
        const int sep = rest.indexOf(QStringLiteral(" - "));
        if (sep > 0)
        {
            if (m.artist.isEmpty())
                m.artist = rest.left(sep).trimmed();
            if (m.title.isEmpty())
                m.title = rest.mid(sep + 3).trimmed();
        }
    }

    if (m.title.isEmpty())
    {
        m.title = rest;
        m.guessedFromFileName = true;
    }
    if (m.title.isEmpty())
        m.title = base;
}

// -----------------------------------------------------------------------------
//  单元 ⑧：歌词 —— .lrc 文件
// -----------------------------------------------------------------------------
struct LyricLine
{
    qint64  timeMs = 0;
    QString text;
};

// 解析 .lrc 正文。支持一行挂多个时间标签（[00:12.00][01:20.30]词），
// 毫秒两位按"百分秒"算（这是 .lrc 的老规矩：两位是 1/100 秒，三位才是毫秒）。
// 没有时间标签的行直接丢掉 —— 那种"纯文本歌词"没法跟着唱，显示出来只会误导。
inline QVector<LyricLine> parseLrc(const QString& content)
{
    QVector<LyricLine> out;
    if (content.isEmpty())
        return out;

    static const QRegularExpression tag(QStringLiteral("\\[(\\d{1,3}):(\\d{1,2})(?:[\\.:](\\d{1,3}))?\\]"));

    const QStringList rows = content.split(QRegularExpression(QStringLiteral("[\\r\\n]")),
                                           Qt::SkipEmptyParts);
    out.reserve(rows.size());

    for (const QString& row : rows)
    {
        QVector<qint64> times;
        auto it = tag.globalMatch(row);
        while (it.hasNext())
        {
            const QRegularExpressionMatch mt = it.next();
            const qint64 mm   = mt.captured(1).toLongLong();
            const qint64 ss   = mt.captured(2).toLongLong();
            const QString frac = mt.captured(3);

            qint64 ms = 0;
            if (frac.size() == 1)      ms = frac.toLongLong() * 100;
            else if (frac.size() == 2) ms = frac.toLongLong() * 10;
            else if (frac.size() == 3) ms = frac.toLongLong();

            times.append((mm * 60 + ss) * 1000 + ms);
        }

        if (times.isEmpty())
            continue;                                    // [ti:] [ar:] 这类元信息行也走这里被丢掉

        QString text = row;
        text.remove(tag);
        text = text.trimmed();
        if (text.isEmpty())
            continue;                                    // 纯时间戳的间奏行，不要

        for (qint64 t : times)
            out.append(LyricLine{ t, text });
    }

    std::sort(out.begin(), out.end(),
              [](const LyricLine& a, const LyricLine& b) { return a.timeMs < b.timeMs; });
    return out;
}

// 找同名 .lrc：先试完全同名（song.mp3 -> song.lrc），
// 再试"忽略大小写"的一遍（有的曲子是 Song.LRC）。
inline QString findLrcPath(const QString& audioPath)
{
    const QFileInfo fi(audioPath);
    const QString dir = fi.absolutePath();
    const QString base = fi.completeBaseName();

    const QString exact = QDir(dir).filePath(base + QStringLiteral(".lrc"));
    if (QFileInfo::exists(exact))
        return exact;

    const QStringList cands = QDir(dir).entryList(QStringList() << (base + QStringLiteral(".*")),
                                                  QDir::Files, QDir::Name);
    for (const QString& name : cands)
    {
        if (name.endsWith(QStringLiteral(".lrc"), Qt::CaseInsensitive))
            return QDir(dir).filePath(name);
    }
    return QString();
}

inline QString readLrcFile(const QString& lrcPath)
{
    QFile f(lrcPath);
    if (!f.open(QIODevice::ReadOnly))
        return QString();

    const QByteArray raw = f.readAll();
    const QByteArray clean = chopZeros(raw);

    // .lrc 的编码在国内同样很乱：UTF-8 和 GBK 五五开，还有带 BOM 的
    if (clean.startsWith("\xEF\xBB\xBF"))
        return QString::fromUtf8(clean.mid(3));

    {
        QStringDecoder d(QStringConverter::Utf8);
        const QString s = d(clean);
        if (!d.hasError())
            return s;
    }
    {
        QStringDecoder d(QStringConverter::System);
        const QString s = d(clean);
        if (!d.hasError())
            return s;
    }
    return QString::fromLatin1(clean);
}

// -----------------------------------------------------------------------------
//  顶层入口
// -----------------------------------------------------------------------------
inline Meta readMeta(const QString& path)
{
    Meta m;
    const QString suffix = QFileInfo(path).suffix().toLower();

    if (suffix == QLatin1String("flac"))
    {
        parseFlac(path, m);
    }
    else if (suffix == QLatin1String("m4a") || suffix == QLatin1String("mp4")
             || suffix == QLatin1String("aac"))
    {
        parseMp4(path, m);
    }
    else if (suffix == QLatin1String("wav"))
    {
        parseWav(path, m);
    }
    else
    {
        // mp3、以及一切不确定的 —— ID3v2 优先，读不到再退 ID3v1。
        // 时长也在 parseId3v2 里顺手估了（那里才知道标签块有多大）。
        parseId3v2(path, m);
        if (m.title.isEmpty() || m.artist.isEmpty())
            parseId3v1(path, m);
        if (m.durationMs == 0)
            m.durationMs = estimateMp3DurationMs(path, 0);   // 连 ID3v2 都没有的文件
    }

    applyFileNameGuess(path, m);

    // 歌词：.lrc 优先（用户能自己改），没有才用内嵌的
    const QString lrc = findLrcPath(path);
    if (!lrc.isEmpty())
    {
        const QString text = readLrcFile(lrc);
        if (!text.isEmpty())
        {
            m.lyrics      = text;
            m.lyricSource = QStringLiteral("lrc");
        }
    }
    else if (!m.lyrics.isEmpty())
    {
        m.lyricSource = QStringLiteral("内嵌");
    }

    return m;
}

} // namespace TrackMeta
