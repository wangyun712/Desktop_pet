#pragma once
// =============================================================================
//  OnlineMusic —— 联网音乐搜索/下载引擎（网易云优先，B 站回退）
//
//  ★ 接口来源 ★ 两个平台的社区公开接口（非官方、无需登录、仅供个人本地
//  使用）。平台风控/改版可能导致失效 —— 失效时回调空结果，调用方在列表行
//  上提示"联网失败"，本地功能完全不受影响。
//
//  ★ 职责边界 ★ 这里只做"搜 → 拿直链 → 下载"，不知道播放器/列表的存在：
//    · 网易云：搜索 / 歌曲直链（MP3）/ 歌词（LRC）。免费歌直链可直接播；
//      VIP/无版权时 url 为空 —— 由调用方决定回退 B 站（这正是回退的触发点）；
//    · B 站：搜索（WBI 签名）→ 视频cid → playurl 的 **DASH 音频轨**（只要
//      声音不要画面）。音频是 AAC/M4S，播放器的 miniaudio 不认 —— 落地后
//      由 MfDecode（Windows 自带 Media Foundation）转成 WAV 再播；
//    · download：通用 HTTP 下载（带 Referer/UA，进度回调）。
//
//  ★ 全部异步 ★ 回调都在主线程触发（QNetworkAccessManager 的事件驱动），
//  界面可以放心在回调里改列表。
// =============================================================================

#include <QMap>
#include <QString>
#include <QVector>
#include <functional>

namespace OnlineMusic {

enum Source { NetEase = 0, Bilibili = 1 };

struct Item
{
    int     source     = NetEase;
    QString id;             // 网易歌曲 id / B站 bvid
    QString title;
    QString artist;
    QString album;
    qint64  durationMs = 0;
    bool    fee        = false;   // 网易：搜索结果里带付费/VIP 标记（仅供列表提示）
};

using ItemListCb = std::function<void(const QVector<Item>&)>;   // 空 = 搜索失败/无结果
using UrlCb      = std::function<void(const QString& url, const QString& suffix)>;
using TextCb     = std::function<void(const QString& text)>;
using ProgressCb = std::function<void(qint64 received, qint64 total)>;
using DoneCb     = std::function<void(bool ok, const QString& err)>;

// ---- 搜索（关键词 → 结果列表；失败/无结果给空表）----
void searchNetEase(const QString& keyword, ItemListCb cb);
void searchBilibili(const QString& keyword, ItemListCb cb);

// ---- 网易云：歌曲直链（url 为空 = VIP/无版权，调用方回退 B 站）+ 歌词 ----
void netEaseUrl(const QString& songId, UrlCb cb);
void netEaseLyric(const QString& songId, TextCb cb);

// ---- B 站：视频 bvid → 音频流直链（AAC/M4S，播放前需 MfDecode 转 WAV）----
void bilibiliAudio(const QString& bvid, UrlCb cb);

// ---- 通用下载（headers 里放 Referer 等；进度 + 完成，done.ok=false 时 err 带原因）----
void download(const QString& url, const QString& savePath,
              const QMap<QString, QString>& headers,
              ProgressCb progress, DoneCb done);

// ---- 临时缓存目录（%TEMP%/PetPalMusic，C 盘）：启动清残留、退出整删 ----
QString tempDir();
void    clearTempDir();

QString sanitizeFileName(const QString& name);   // 文件名净化（去非法字符）

// ---- 自检描述（纯只读，不发网络请求）----
QString describe();

} // namespace OnlineMusic
