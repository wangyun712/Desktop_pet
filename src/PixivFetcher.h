#pragma once
// =============================================================================
//  PixivFetcher —— 从 Pixiv 取图（每日图片「联网」标签用）
//
//  三个入口（全部异步、回调在主线程、宿主析构自动丢弃，照 OnlineMusic 的规矩）：
//    · search(keyword, page)      搜作品列表（参数与浏览器搜索头一一对应）
//    · fetchIllust(id)            单个作品的详情（标题/画师/大图地址/页数）
//    · fetchBytes(url)            图片字节（i.pximg.net 必须带 Referer，不然 403）
//
//  ★ 可达性须知 ★ Pixiv 对数据中心 IP 一律 403（实测），走家用宽带 +
//  系统代理没问题 —— QNetworkAccessManager 默认走系统代理，用户能开浏览器
//  看 pixiv 就能取到图。失败时错误文案会提示这一点。
//
//  接口形态：https://www.pixiv.net/ajax/... 是站点自己用的 JSON 接口，
//  匿名可读（浏览器无登录态也能打开搜索页），返回 body.illustManga[] /
//  body.urls.regular，QJson 链式取值照 OnlineMusic 的写法。
// =============================================================================

#include <QString>
#include <QVector>
#include <functional>

class QObject;

namespace PixivFetcher {

struct Illust
{
    QString id;          // 作品 ID（illustId，保存文件名/详情接口都用它）
    QString title;       // 作品标题
    QString userName;    // 画师
    int     pageCount = 1;
    QString imageUrl;    // 图片直链（堆糖源搜索直出；Pixiv 由详情步骤填，池内初始为空）
};

using SearchCb = std::function<void(bool ok, const QVector<Illust>& list,
                                    int lastPage, const QString& err)>;
// lastPage = 本次搜索的总页数（尺寸/分级过滤后可能只有几页，随机页码要按它来，
// 超出范围的页码会返回空列表）。拿不到时为 1。
using IllustCb = std::function<void(bool ok, const Illust& illust,
                                    const QString& regularUrl, const QString& err)>;
using BytesCb  = std::function<void(bool ok, const QByteArray& bytes, const QString& err)>;

// 搜索作品列表。keyword 形如「洛天依 -AI生成 -涩」（负号 = 排除）；
// 固定参数与产品搜索头一致：tag 匹配 / 只看插画 / 全年龄 / 排除 AI / 长≥3000。
// page 从 1 起。list 已按接口原序返回（调用方自行乱序）。
void search(const QString& keyword, int page, QObject* ctx, SearchCb cb);

// 单个作品的详情与大图地址（urls.regular ≈ 1200px，显示和保存都够用）。
// 给的是第 1 页（p0）；多页作品想翻页属于后续增强。
void fetchIllust(const QString& illustId, QObject* ctx, IllustCb cb);

// 图片字节。url 一般来自 fetchIllust 给的 regularUrl（i.pximg.net 域）。
void fetchBytes(const QString& url, QObject* ctx, BytesCb cb);

} // namespace PixivFetcher
