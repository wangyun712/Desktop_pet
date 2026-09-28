#pragma once
// =============================================================================
//  MfDecode —— 用 Windows 自带的 Media Foundation 把 AAC 音频解码成 FLAC
//
//  ★ 为什么需要它 ★ B 站的音频流是 AAC（M4S/M4A 容器），而播放器的
//  miniaudio 只认 MP3/FLAC/WAV。系统自带的 Media Foundation 能解 AAC ——
//  解成 16bit PCM 后交给 libFLAC（FlacEncode）编成 FLAC 落到缓存目录，
//  播放器当普通本地文件播，播放链路一行都不用改。
//
//  仅 Windows（本项目本来就只发 Windows）。个别 fragmented-mp4 可能
//  打不开 —— 调用方把失败显示在列表行上，不影响其它资源。
// =============================================================================

#include <QString>

#include <functional>

class QObject;

namespace MfDecode {

// 同步版：input 为 m4s/m4a/aac 等音频文件，解码后经 libFLAC 写出 outputFlac。
// 解码+编码是秒级操作，不要在主线程直接调（用下面的异步版）。
// title/artist 会写进 FLAC 的 VORBIS_COMMENT 标签，传空串则跳过。
// 成功返回 true；失败把原因写进 errorOut（可为空）。
bool decodeToFlac(const QString& input, const QString& outputFlac,
                  const QString& title, const QString& artist,
                  QString* errorOut = nullptr);

// 异步版：后台线程跑 decodeToFlac，完成后把回调派回 ctx 所在线程；
// ctx 已析构（窗口都关了）则结果直接丢弃。回调参数 (ok, err)。
void decodeToFlacAsync(QObject* ctx,
                       const QString& input, const QString& outputFlac,
                       const QString& title, const QString& artist,
                       std::function<void(bool ok, const QString& err)> cb);

} // namespace MfDecode
