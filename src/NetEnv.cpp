#include "NetEnv.h"

#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>
#include <QUrl>

namespace {

constexpr char kUA[]      = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"
                            " (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
constexpr int  kTimeoutMs = 15000;

// 目标域的系统代理里有没有"可用的"条目（非直连且 host 非空）。
// 空条目 = 系统代理配置里的 PAC/自动配置残留，发给它只会超时。
bool usableSystemProxyFor(const QString& host)
{
    const QNetworkProxyQuery q(host, 443);
    const QList<QNetworkProxy> ps = QNetworkProxyFactory::systemProxyForQuery(q);
    for (const QNetworkProxy& p : ps)
        if (p.type() != QNetworkProxy::NoProxy && !p.hostName().isEmpty())
            return true;
    return false;
}

// 环境变量代理兜底：命令行代理客户端只导出 http_proxy/https_proxy 的用法。
// 返回是否设置成功。
bool applyEnvProxy(QNetworkAccessManager& n)
{
    QByteArray proxyEnv = qgetenv("https_proxy");
    if (proxyEnv.isEmpty())
        proxyEnv = qgetenv("HTTPS_PROXY");
    if (proxyEnv.isEmpty())
        proxyEnv = qgetenv("all_proxy");
    if (proxyEnv.isEmpty())
        proxyEnv = qgetenv("http_proxy");

    const QUrl pu = QUrl::fromUserInput(QString::fromUtf8(proxyEnv));
    if (!proxyEnv.isEmpty() && !pu.host().isEmpty() && pu.port() > 0)
    {
        n.setProxy(QNetworkProxy(QNetworkProxy::HttpProxy, pu.host(), quint16(pu.port())));
        return true;
    }
    return false;
}

} // namespace

namespace NetEnv {

QNetworkAccessManager& nam()
{
    static QNetworkAccessManager n;

    // ★ 代理净化只做一次 ★（判定结果进程内不变）
    static bool sanitized = false;
    if (!sanitized)
    {
        sanitized = true;
        // 注意：净化与否按"最近要访问的域"判定即可 —— 各图源走同一策略，
        // 家用宽带 + 代理客户端的组合下结论一致。
        if (!usableSystemProxyFor(QStringLiteral("www.pixiv.net")))
        {
            if (!applyEnvProxy(n))
                n.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
        }
    }

    return n;
}

QNetworkRequest makeRequest(const QUrl& url, const QByteArray& referer)
{
    QNetworkRequest req(url);
    req.setTransferTimeout(kTimeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setRawHeader("User-Agent", kUA);
    if (!referer.isEmpty())
        req.setRawHeader("Referer", referer);
    return req;
}

QObject* sinkContext(QObject* ctx)
{
    // 调用方理论上必传 ctx（编译期不挡）：空了兜底到 NAM 自己 —— 不然
    // connect 的空 ctx 会让信号连接直接失效、回调永远不来（排障极难）。
    return ctx ? ctx : &nam();
}

QString humanError(const QNetworkReply* reply)
{
    if (reply->error() == QNetworkReply::TimeoutError
        || reply->error() == QNetworkReply::OperationCanceledError)
        return QStringLiteral("请求超时：连不上图片站（常见原因：DNS 被污染/代理没开）。"
                              "开着代理软件（系统代理或 TUN 模式）再试一次");
    if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 403)
        return QStringLiteral("被站点拒绝（403）：先确认浏览器能打开对应网站"
                              "（走系统代理也可以，本功能会自动跟随）");
    return reply->errorString();
}

void fetchBytes(const QString& url, const QByteArray& referer, QObject* ctx, BytesCb cb)
{
    QPointer<QObject> guard(ctx);

    QNetworkReply* reply = nam().get(makeRequest(QUrl(url), referer));
    QObject::connect(reply, &QNetworkReply::finished, sinkContext(ctx),
                     [guard, reply, cb] {
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

} // namespace NetEnv
