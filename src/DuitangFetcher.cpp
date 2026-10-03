#include "DuitangFetcher.h"

#include "NetEnv.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QUrlQuery>

namespace {

constexpr char kReferer[] = "https://www.duitang.com/";

// ★ 堆糖专用 NAM:恒定直连 ★ 堆糖是国内站点(a-ssl.dtstatic.com 直连实测可用),
// 塞进系统代理反而可能超时(实测);TUN 模式的代理对"直连"同样透明,不受影响。
// Pixiv 源才需要跟随代理(见 NetEnv::nam 的净化逻辑)。
QNetworkAccessManager& nam()
{
    static QNetworkAccessManager n;
    static bool directForced = false;
    if (!directForced)
    {
        directForced = true;
        n.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    }
    return n;
}

} // namespace

namespace DuitangFetcher {

void search(const QString& keyword, int start, QObject* ctx, SearchCb cb)
{
    QPointer<QObject> guard(ctx);

    QUrl url(QStringLiteral("https://www.duitang.com/napi/blog/list/by_search/"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("kw"),    keyword);
    q.addQueryItem(QStringLiteral("start"), QString::number(qMax(0, start)));
    q.addQueryItem(QStringLiteral("limit"), QStringLiteral("24"));
    url.setQuery(q);

    QNetworkReply* reply = nam().get(NetEnv::makeRequest(url, kReferer));
    QObject::connect(reply, &QNetworkReply::finished, NetEnv::sinkContext(ctx),
                     [guard, reply, cb] {
        reply->deleteLater();
        if (!guard)
            return;

        if (reply->error() != QNetworkReply::NoError)
        {
            cb(false, {}, 0, NetEnv::humanError(reply));
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        const QJsonObject root = doc.object();
        if (root.value(QLatin1String("status")).toInt() != 1)
        {
            cb(false, {}, 0, QStringLiteral("堆糖接口返回异常（可能改版了）"));
            return;
        }

        const QJsonObject data = root.value(QLatin1String("data")).toObject();
        const QJsonArray items = data.value(QLatin1String("object_list")).toArray();

        QVector<Post> posts;
        posts.reserve(items.size());
        for (const QJsonValue& sv : items)
        {
            const QJsonObject o = sv.toObject();
            const QJsonObject photo = o.value(QLatin1String("photo")).toObject();

            Post p;
            p.id       = QString::number(photo.value(QLatin1String("id")).toVariant().toLongLong());
            p.title    = o.value(QLatin1String("msg")).toString();
            p.imageUrl = photo.value(QLatin1String("path")).toString();
            p.width    = photo.value(QLatin1String("width")).toInt();
            p.height   = photo.value(QLatin1String("height")).toInt();
            if (!p.id.isEmpty() && !p.imageUrl.isEmpty())
                posts.append(p);
        }

        const int total = data.value(QLatin1String("total")).toInt();
        if (posts.isEmpty())
            cb(false, {}, total, QStringLiteral("这一页没有符合条件的结果"));
        else
            cb(true, posts, total, {});
    });
}

void fetchBytes(const QString& url, QObject* ctx, BytesCb cb)
{
    QPointer<QObject> guard(ctx);

    QNetworkReply* reply = nam().get(NetEnv::makeRequest(QUrl(url), QByteArray()));
    QObject::connect(reply, &QNetworkReply::finished, NetEnv::sinkContext(ctx),
                     [guard, reply, cb] {
        reply->deleteLater();
        if (!guard)
            return;

        if (reply->error() != QNetworkReply::NoError)
        {
            cb(false, {}, NetEnv::humanError(reply));
            return;
        }
        const QByteArray bytes = reply->readAll();
        if (bytes.isEmpty())
        {
            cb(false, {}, QStringLiteral("图片内容为空"));
            return;
        }
        cb(true, bytes, {});
    });
}

} // namespace DuitangFetcher
