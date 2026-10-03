#include "PixivFetcher.h"

#include "NetEnv.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QUrlQuery>

// =============================================================================
//  PixivFetcher 实现
//
//  网络底座（NAM 单例/代理净化/UA/超时/错误文案/字节回调）全部在 NetEnv，
//  这里只做三件事：拼 URL、解析 JSON、把结果回调出去。
// =============================================================================

namespace {

constexpr char kReferer[] = "https://www.pixiv.net/";   // i.pximg.net 没有它就是 403

// ajax 接口的统一应答骨架：{error: bool, message: ..., body: {...}}
// error=true 时把 Pixiv 自己的话带回去
bool parseAjaxEnvelope(const QByteArray& data, QJsonObject& body, QString& err)
{
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject())
    {
        err = QStringLiteral("Pixiv 返回的不是 JSON（可能被拦截，检查网络/代理）");
        return false;
    }
    const QJsonObject root = doc.object();
    if (root.value(QLatin1String("error")).toBool())
    {
        err = QStringLiteral("Pixiv 接口报错：%1")
                  .arg(root.value(QLatin1String("message")).toString());
        return false;
    }
    body = root.value(QLatin1String("body")).toObject();
    return true;
}

} // namespace

namespace PixivFetcher {

void search(const QString& keyword, int page, QObject* ctx, SearchCb cb)
{
    QPointer<QObject> guard(ctx);

    QUrl url(QStringLiteral("https://www.pixiv.net/ajax/search/artworks/%1")
                 .arg(QString::fromUtf8(QUrl::toPercentEncoding(keyword))));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("s_mode"),   QStringLiteral("tag"));
    q.addQueryItem(QStringLiteral("type"),     QStringLiteral("illust"));
    q.addQueryItem(QStringLiteral("mode"),     QStringLiteral("safe"));     // 全年龄
    q.addQueryItem(QStringLiteral("ai_type"),  QStringLiteral("1"));     // 排除 AI 生成
    q.addQueryItem(QStringLiteral("wlt"),      QStringLiteral("3000"));   // 宽 ≥3000
    q.addQueryItem(QStringLiteral("hlt"),      QStringLiteral("3000"));   // 高 ≥3000
    q.addQueryItem(QStringLiteral("p"),        QString::number(qMax(1, page)));
    url.setQuery(q);

    QNetworkReply* reply = NetEnv::nam().get(NetEnv::makeRequest(url, kReferer));
    QObject::connect(reply, &QNetworkReply::finished, NetEnv::sinkContext(ctx),
                     [guard, reply, cb] {
        reply->deleteLater();
        if (!guard)
            return;

        QString err;
        QJsonObject body;
        if (reply->error() != QNetworkReply::NoError)
        {
            cb(false, {}, 1, NetEnv::humanError(reply));
            return;
        }
        if (!parseAjaxEnvelope(reply->readAll(), body, err))
        {
            cb(false, {}, 1, err);
            return;
        }

        // ★ illustManga 的两种形态都要兼容 ★ 搜索接口返回的是对象
        // {data:[...], total, lastPage}，别处/旧版可能直接是数组
        const QJsonValue imv = body.value(QLatin1String("illustManga"));
        QJsonArray items;
        int lastPage = 1;
        if (imv.isArray())
        {
            items = imv.toArray();
        }
        else if (imv.isObject())
        {
            const QJsonObject im = imv.toObject();
            items = im.value(QLatin1String("data")).toArray();
            lastPage = qMax(1, im.value(QLatin1String("lastPage")).toInt(1));
        }

        QVector<Illust> list;
        list.reserve(items.size());
        for (const QJsonValue& sv : items)
        {
            const QJsonObject o = sv.toObject();
            Illust it;
            it.id         = o.value(QLatin1String("id")).toString();
            it.title      = o.value(QLatin1String("title")).toString();
            it.userName   = o.value(QLatin1String("userName")).toString();
            it.pageCount  = qMax(1, o.value(QLatin1String("pageCount")).toInt());
            if (!it.id.isEmpty())
                list.append(it);
        }

        if (list.isEmpty())
            cb(false, {}, lastPage, QStringLiteral("这一页没有符合条件的结果"));
        else
            cb(true, list, lastPage, {});
    });
}

void fetchIllust(const QString& illustId, QObject* ctx, IllustCb cb)
{
    QPointer<QObject> guard(ctx);

    const QUrl url(QStringLiteral("https://www.pixiv.net/ajax/illust/%1").arg(illustId));
    QNetworkReply* reply = NetEnv::nam().get(NetEnv::makeRequest(url, kReferer));
    QObject::connect(reply, &QNetworkReply::finished, NetEnv::sinkContext(ctx),
                     [guard, reply, illustId, cb] {
        reply->deleteLater();
        if (!guard)
            return;

        QString err;
        QJsonObject body;
        if (reply->error() != QNetworkReply::NoError)
        {
            cb(false, {}, {}, NetEnv::humanError(reply));
            return;
        }
        if (!parseAjaxEnvelope(reply->readAll(), body, err))
        {
            cb(false, {}, {}, err);
            return;
        }

        Illust it;
        it.id        = body.value(QLatin1String("illustId")).toString(illustId);
        it.title     = body.value(QLatin1String("illustTitle")).toString();
        it.userName  = body.value(QLatin1String("userName")).toString();
        it.pageCount = qMax(1, body.value(QLatin1String("pageCount")).toInt());

        const QJsonObject urls = body.value(QLatin1String("urls")).toObject();
        const QString regular = urls.value(QLatin1String("regular")).toString();
        it.imageUrl  = regular;
        if (regular.isEmpty())
        {
            cb(false, it, {}, QStringLiteral("没拿到大图地址"));
            return;
        }
        cb(true, it, regular, {});
    });
}

void fetchBytes(const QString& url, QObject* ctx, BytesCb cb)
{
    NetEnv::fetchBytes(url, kReferer, ctx,
                       [cb](bool ok, const QByteArray& bytes, const QString& err) { cb(ok, bytes, err); });
}

} // namespace PixivFetcher
