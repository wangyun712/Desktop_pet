#include "PixivFetcher.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
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
//  请求纪律（照 OnlineMusic 的既有模式）：
//    · QNetworkAccessManager 函数级单例，回调全部落回主线程（NAM 事件驱动）；
//    · 每个回调先用宿主守卫判活 —— ctx 析构（面板关了）就丢弃结果；
//    · 错误文案给"人能看懂"的原因，403/超时各自有针对性提示。
// =============================================================================

namespace {

constexpr char kUA[]      = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"
                            " (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
constexpr char kReferer[] = "https://www.pixiv.net/";
constexpr int  kTimeoutMs = 15000;

QNetworkAccessManager& nam()
{
    static QNetworkAccessManager n;

    // ★ 系统代理净化（只做一次）★ QNAM 默认跟随系统代理，但系统代理条目可能
    // 是"空 host 的坏条目"（PAC/自动配置残留，实测本机就是 host="" port=0）——
    // 请求全发给空代理只能超时，而浏览器/命令行工具不跟它、看起来一切正常，
    // 症状就是"功能里联网失败、别处都好好的"。所以：对 pixiv 查一次系统代理，
    // 查得到**可用的**（非直连且 host 非空）就照用（用户真挂了代理的场景），
    // 查不到就明确直连。
    static bool sanitized = false;
    if (!sanitized)
    {
        sanitized = true;
        const QNetworkProxyQuery q(QStringLiteral("www.pixiv.net"), 443);
        bool usable = false;
        const QList<QNetworkProxy> ps = QNetworkProxyFactory::systemProxyForQuery(q);
        for (const QNetworkProxy& p : ps)
            if (p.type() != QNetworkProxy::NoProxy && !p.hostName().isEmpty())
                usable = true;
        if (!usable)
        {
            // 没有可用的系统代理 → 看环境变量兜底（命令行代理客户端只导出
            // http_proxy/https_proxy 的用法），再不行就明确直连。
            QByteArray proxyEnv = qgetenv("https_proxy");
            if (proxyEnv.isEmpty())
                proxyEnv = qgetenv("HTTPS_PROXY");
            if (proxyEnv.isEmpty())
                proxyEnv = qgetenv("all_proxy");
            if (proxyEnv.isEmpty())
                proxyEnv = qgetenv("http_proxy");

            const QUrl pu = QUrl::fromUserInput(QString::fromUtf8(proxyEnv));
            if (!proxyEnv.isEmpty() && !pu.host().isEmpty() && pu.port() > 0)
                n.setProxy(QNetworkProxy(QNetworkProxy::HttpProxy, pu.host(),
                                         quint16(pu.port())));
            else
                n.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
        }
    }

    return n;
}

// 请求的接收上下文：调用方理论上必传（编译期不挡），空了兜底到 NAM 自己，
// 不然 connect 的空 ctx 会让信号连接直接失效、回调永远不来（排障极难）。
QObject* sinkContext(QObject* ctx)
{
    return ctx ? ctx : &nam();
}

QNetworkRequest makeRequest(const QUrl& url)
{
    QNetworkRequest req(url);
    req.setTransferTimeout(kTimeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setRawHeader("User-Agent", kUA);
    req.setRawHeader("Referer", kReferer);       // i.pximg.net 没有它就是 403
    return req;
}

// 人能看懂的网络错误：超时 / 403 各自点名（国内 DNS 污染、Pixiv 拦数据中心
// 是两种最常见的症状，给的建议不一样）
QString humanError(const QNetworkReply* reply)
{
    if (reply->error() == QNetworkReply::TimeoutError
        || reply->error() == QNetworkReply::OperationCanceledError)
        return QStringLiteral("请求超时：连不上 Pixiv（常见原因：DNS 被污染/代理没开）。"
                              "开着代理软件（系统代理或 TUN 模式）再试一次");
    if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 403)
        return QStringLiteral("被 Pixiv 拒绝（403）：先确认浏览器能打开 pixiv.net"
                              "（走系统代理也可以，本功能会自动跟随）");
    return reply->errorString();
}

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

    QNetworkReply* reply = nam().get(makeRequest(url));
    QObject::connect(reply, &QNetworkReply::finished, sinkContext(ctx),
                     [guard, reply, cb] {
        reply->deleteLater();
        if (!guard)
            return;

        QString err;
        QJsonObject body;
        if (reply->error() != QNetworkReply::NoError)
        {
            cb(false, {}, 1, humanError(reply));
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
    QNetworkReply* reply = nam().get(makeRequest(url));
    QObject::connect(reply, &QNetworkReply::finished, sinkContext(ctx), [guard, reply, illustId, cb] {
        reply->deleteLater();
        if (!guard)
            return;

        QString err;
        QJsonObject body;
        if (reply->error() != QNetworkReply::NoError)
        {
            cb(false, {}, {}, humanError(reply));
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
    QPointer<QObject> guard(ctx);

    QNetworkReply* reply = nam().get(makeRequest(QUrl(url)));
    QObject::connect(reply, &QNetworkReply::finished, sinkContext(ctx), [guard, reply, cb] {
        reply->deleteLater();
        if (!guard)
            return;

        if (reply->error() != QNetworkReply::NoError)
        {
            cb(false, {}, humanError(reply));
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

} // namespace PixivFetcher
