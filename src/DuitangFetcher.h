#pragma once
// =============================================================================
//  DuitangFetcher —— 从堆糖取图（每日图片「联网」标签的第二个图源）
//
//  堆糖是图片瀑布流：卡片本身就是图片本体，没有"封面 ≠ 内容"的问题；
//  搜索接口（网页内部接口，非官方）匿名可用，图片 CDN 无防盗链。
//  接口：GET www.duitang.com/napi/blog/list/by_search/?kw=...&start=...&limit=24
//  返回：{status:1, data:{total, next_start, object_list:[{photo:{id,path,width,height},msg}]}}
//  ★ 非官方接口，站点改版会失效 —— 失败时错误文案会明确，不影响 Pixiv 源 ★
//
//  回调全部在主线程、宿主析构自动丢弃（NetEnv 同一套纪律）。
// =============================================================================

#include <QString>
#include <QVector>
#include <functional>

class QObject;

namespace DuitangFetcher {

struct Post
{
    QString id;         // 照片 ID（数字，保存文件名用）
    QString title;      // 博主配文（msg，可能为空）
    QString imageUrl;   // 图片直链（a-ssl.dtstatic.com）
    int     width  = 0; // 原图宽（大图过滤用）
    int     height = 0;
};

// start 为偏移量（0,24,48...，应答里 data.next_start 给下一个）；limit 固定 24。
// total = 该关键词命中的总卡片数（随机跳页用它）。
using SearchCb = std::function<void(bool ok, const QVector<Post>& posts,
                                    int total, const QString& err)>;
using BytesCb  = std::function<void(bool ok, const QByteArray& bytes, const QString& err)>;

void search(const QString& keyword, int start, QObject* ctx, SearchCb cb);
void fetchBytes(const QString& url, QObject* ctx, BytesCb cb);   // 恒定直连的专用 NAM

} // namespace DuitangFetcher
