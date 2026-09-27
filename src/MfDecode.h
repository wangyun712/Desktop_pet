#pragma once
// =============================================================================
//  MfDecode —— 用 Windows 自带的 Media Foundation 把 AAC 音频解码成 WAV
//
//  ★ 为什么需要它 ★ B 站的音频流是 AAC（M4S/M4A 容器），而播放器的
//  miniaudio 只认 MP3/FLAC/WAV。系统自带的 Media Foundation 能解 AAC ——
//  解成 PCM 后包一个 WAV 头落到缓存目录，播放器当普通本地文件播，
//  播放链路一行都不用改。
//
//  仅 Windows（本项目本来就只发 Windows）。个别 fragmented-mp4 可能
//  打不开 —— 调用方把失败显示在列表行上，不影响其它资源。
// =============================================================================

#include <QString>

namespace MfDecode {

// input: m4s/m4a/aac 等音频文件；output: 输出 WAV 路径（16bit PCM）。
// 成功返回 true；失败把原因写进 errorOut（可为空）。
bool decodeToWav(const QString& input, const QString& outputWav, QString* errorOut = nullptr);

} // namespace MfDecode
