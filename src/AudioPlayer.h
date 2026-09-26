#pragma once
// =============================================================================
//  AudioPlayer —— 播放器的"后端"：只干一件事，把文件放出来
//
//  ★ 为什么不直接用 QtMultimedia（QMediaPlayer）★
//    本机装的 Qt 没有 Multimedia 模块（find_package 找不到），而为了一个播放器
//    去装整个模块、还让每个用户都装，代价太大。
//    改用 miniaudio（third_party/miniaudio.h，单头文件、public domain / MIT-0）：
//      自带 MP3 / FLAC / WAV 解码，Windows 走 WASAPI，不需要任何额外依赖。
//
//  ★ 这一层刻意写得很薄 ★
//    外面（PlayerPage）不知道底下是 miniaudio 还是别的什么。
//    哪天要换回 QMediaPlayer，只需要重写这一个 .cpp —— 接口一行都不用动。
//    所以这里**没有任何界面**：不画进度条、不管歌词、不挑下一首。
//    它只回答四个问题：能播吗、在播吗、播到哪了、总共多长。
//
//  ★ 界面怎么知道"播到哪了"★
//    miniaudio 是回调式的（音频线程里推数据），但界面只能在自己的线程里更新。
//    所以这里不开回调，改用**轮询**：一个 200ms 的 QTimer 去问"现在几秒"，
//    问到了就 emit positionChanged。200ms 对进度条和歌词高亮都够用，
//    而且彻底避开了"音频线程里碰 QWidget"这类经典崩溃。
// =============================================================================

#include <QObject>
#include <QString>

class QTimer;

class AudioPlayer : public QObject
{
    Q_OBJECT
public:
    explicit AudioPlayer(QObject* parent = nullptr);
    ~AudioPlayer() override;

    // 换一首歌。成功返回 true；失败时把原因写进 errorOut（界面拿去显示）。
    // ★ 换歌会自动把上一首停掉并释放 ★ —— 不用调用方记得先 stop()。
    bool load(const QString& path, QString* errorOut = nullptr);

    bool hasTrack() const;

    void play();
    void pause();
    void stop();                 // 停 + 回到 0
    void togglePlayPause();

    // 拖进度条用。单位毫秒，超出范围会被夹到 [0, 时长]。
    void seekToMs(qint64 ms);

    qint64 positionMs() const;
    qint64 durationMs() const;
    bool   isPlaying() const;

    void setVolumePercent(int percent);   // 0~100
    int  volumePercent() const;

    // 自检用：引擎起没起来、走的哪个后端、设备叫什么。纯只读。
    QString describe() const;

signals:
    void positionChanged(qint64 ms);
    void durationChanged(qint64 ms);
    void stateChanged();                     // 播放/暂停/停止/换歌 —— 都会发
    void trackFinished();                    // 自然播完（seek 到结尾不算）
    void errorOccurred(const QString& message);

private:
    void onTick();

    // ★ pimpl：miniaudio.h 那 4MB 只允许出现在 .cpp 里 ★
    //   头文件里连 ma_engine 这个名字都不出现，所以谁 include 了 AudioPlayer.h
    //   谁就不会被拖慢 —— 这一点在这个项目里尤其重要，因为 PlayerPage、
    //   TrackListModel 全都要 include 它。
    struct Impl;
    Impl* m_impl = nullptr;

    QTimer* m_timer     = nullptr;
    qint64  m_durationMs = 0;
    int     m_volumePercent = 100;

    // "播完了"这件事只能报一次：轮询是 200ms 一次，不打个标记的话
    // 一首歌放完会连发五次 trackFinished，列表就会往下跳五首。
    bool m_finishedReported = false;
};
