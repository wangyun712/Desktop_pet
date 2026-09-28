#pragma once
// =============================================================================
//  FlacEncode —— 把 16bit 交错 PCM 编码成 FLAC 文件（基于第三方 libFLAC）
//
//  ★ 为什么需要它 ★ B 站的音频经 Media Foundation 解出 16bit PCM 后，
//  原先直接包 WAV 头落盘（一首 4 分钟 ≈ 40MB）。现在改用 libFLAC 编成
//  FLAC：体积减半以上，miniaudio 原生播放，TITLE/ARTIST 还能写进
//  VORBIS_COMMENT 标签（TrackMeta::parseFlac 会读，曲库显示更准）。
//
//  libFLAC 源码在 third_party/libflac（BSD 许可，见同目录 LICENSE）。
//  只用它的编码能力，解码仍交给 Media Foundation。
// =============================================================================

#include <QByteArray>
#include <QString>

namespace FlacEncode {

// pcm16: 16bit 交错小端 PCM（尺寸必须是 channels*2 的整数倍）。
// title/artist 写进 VORBIS_COMMENT，传空串则跳过该标签。
// 先写 ".part" 临时文件、编完再改名：中途退出不会留下能被缓存命中的残件。
// 同步阻塞（一首 4 分钟的歌约 1~3 秒），调用方应放在后台线程。
// 成功返回 true；失败把原因写进 errorOut（可为空）。
bool encode(const QByteArray& pcm16, int channels, int sampleRate,
            const QString& title, const QString& artist,
            const QString& outputFlac, QString* errorOut = nullptr);

} // namespace FlacEncode
