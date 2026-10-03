#pragma once
// =============================================================================
//  NetEnv —— 联网取图共用的小底座（PixivFetcher / DuitangFetcher 共用）
//
//  · nam()          ：全项目共用的 QNetworkAccessManager。
//    ★ 带系统代理净化 ★ QNAM 默认跟随系统代理，但系统代理条目可能是
//    "空 host 的坏条目"（PAC/自动配置残留，实测本机就是 host="" port=0）——
//    请求全发给空代理只能超时，而浏览器/命令行工具不跟它，症状就是
//    "功能里联网失败、别处都好好的"。净化规则：对目标域查一次系统代理，
//    查到**可用的**（非直连且 host 非空）就照用（用户真挂了代理的场景）；
//    查不到再看 http_proxy/https_proxy 环境变量兜底；都没有就明确直连。
//  · makeRequest()  ：统一 UA / 超时 / 重定向策略 / Referer。
//  · sinkContext()  ：connect 的空 ctx 兜底（空 ctx 会让信号连接失效）。
//  · humanError()   ：人能看懂的网络错误（超时=DNS 污染/代理没开、403 各有建议）。
//  · fetchBytes()   ：通用"GET → 字节回调"，两个图源共用。
// =============================================================================

#include <QByteArray>
#include <QString>
#include <functional>

class QObject;
class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;
class QUrl;

namespace NetEnv {

QNetworkAccessManager& nam();

QNetworkRequest makeRequest(const QUrl& url, const QByteArray& referer);

QObject* sinkContext(QObject* ctx);

QString humanError(const QNetworkReply* reply);

using BytesCb = std::function<void(bool ok, const QByteArray& bytes, const QString& err)>;
void fetchBytes(const QString& url, const QByteArray& referer, QObject* ctx, BytesCb cb);

} // namespace NetEnv
