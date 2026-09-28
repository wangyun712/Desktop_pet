#include "OnlineMusic.h"

#include <QCryptographicHash>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUrlQuery>

namespace OnlineMusic {

namespace {

// 桌面客户端常见 UA：两个平台都按"浏览器"对待，裸 UA 会被直接拒
//伪造 HTTP 请求里的 User-Agent (UA) 请求头，伪装成 Windows 上的 Chrome 120 浏览器去访问网站接口
const char* const kUA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";

//Qt 框架里专门用来发送 HTTP/HTTPS 网络请求、接收网络响应的类，C++ Qt 桌面程序用它做网络通信
QNetworkAccessManager& nam()
{
    static QNetworkAccessManager m;
    return m;
}

QNetworkReply* get(const QUrl& url, const QMap<QString, QString>& headers = {},
                   int timeoutMs = 15000)
{
    QNetworkRequest req(url);
    // ★ 超时分两档 ★ 搜索/取直链是几 KB 的小请求，15 秒足够（挂太久会拖住
    // "搜不到 → B 站接档"的回退链）；下载整首歌单独放宽到 120 秒。
    // 到点 abort 的表现就是行上的"联网失败·超时"。
    req.setTransferTimeout(timeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setRawHeader("User-Agent", kUA);
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
        req.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
    return nam().get(req);
}

//封装一个简易工具函数，从 Qt 的 QJsonObject 里按 key 取出字符串类型的值，返回 QString
QString jsonStr(const QJsonObject& o, const char* key)
{
    return o.value(QLatin1String(key)).toString();
}

// B 站搜索结果标题里带搜索高亮 <em class="keyword">，剥掉所有标签
QString stripTags(const QString& s)
{
    static const QRegularExpression tagRe(QStringLiteral("<[^>]*>"));
    return QString(s).remove(tagRe).trimmed();
}

// ---------------------------------------------------------------------------
//  B 站会话：buvid3（本地造一个格式合法的）+ WBI 签名密钥（nav 接口，每天轮换）
// ---------------------------------------------------------------------------
//1. 伪造 UA，告诉服务器：我是 Chrome 浏览器
//2. `buvid3()` 生成固定字符串，放到 Cookie 头，模拟设备指纹标识
//3. 实现 WBI 签名（w_rid），满足 B 站网页接口另一个强制校验
//4. 用 QNetworkAccessManager 发送 HTTP 请求，带上 UA+Cookie (buvid3)+ 签名后的参数
QString buvid3()//标记 “是谁的设备”，风控限流、识别同一客户端
{
    static QString v;
    if (v.isEmpty())
    {
        auto* rng = QRandomGenerator::global();
        const quint32 a = rng->generate(), b = rng->generate(), c = rng->generate();
        v = QStringLiteral("%1-%2-%3-%4infoc")
                .arg(a & 0xFFFF, 4, 16, QLatin1Char('0'))
                .arg((a >> 16) & 0xFFFF, 4, 16, QLatin1Char('0'))
                .arg(b & 0xFFFF, 4, 16, QLatin1Char('0'))
                .arg(((b >> 16) ^ c) & 0xFFFF, 4, 16, QLatin1Char('0'));
    }
    return v;
}

// WBI 混淆表：imgKey+subKey 按这张表重排后取前 32 位就是签名密钥
//WBI作用：保证【本次请求参数没被篡改、没有过期重放】
const int kWbiTable[] = {
    46, 47, 18, 2, 53, 8, 23, 32, 15, 50, 10, 31, 58, 3, 45, 35, 27, 43, 5, 49,
    33, 9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13, 37, 48, 7, 16, 24, 55, 40,
    61, 26, 17, 0, 1, 60, 51, 30, 4, 22, 25, 54, 21, 56, 59, 6, 63, 57, 62,
    11, 36, 20, 34, 44, 52
};

void ensureWbiKey(std::function<void(const QString&)> cb)
{
    static QString cached;
    static QElapsedTimer timer;
    if (!cached.isEmpty() && timer.isValid() && timer.elapsed() < 3600 * 1000)
    {
        cb(cached);                        // 密钥一天轮换，缓存 1 小时足够
        return;
    }

    auto* reply = get(QUrl(QStringLiteral("https://api.bilibili.com/x/web-interface/nav")), {}, 15000);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, cb] {
        reply->deleteLater();
        const QJsonObject wbi = QJsonDocument::fromJson(reply->readAll())
                                    .object()
                                    .value(QLatin1String("data")).toObject()
                                    .value(QLatin1String("wbi_img")).toObject();
        const QString imgKey = QFileInfo(jsonStr(wbi, "img_url")).completeBaseName();
        const QString subKey = QFileInfo(jsonStr(wbi, "sub_url")).completeBaseName();
        if (imgKey.isEmpty() || subKey.isEmpty())
        {
            cb(QString());                 // 拿不到密钥：B 站搜索放弃
            return;
        }

        const QString raw = imgKey + subKey;
        QString mixin;
        for (const int idx : kWbiTable)
            if (idx < raw.size())
                mixin += raw.at(idx);
        cached = mixin.left(32);
        timer.restart();
        cb(cached);
    });
}

QString wbiQuery(const QMap<QString, QString>& params, const QString& mixinKey)
{
    QMap<QString, QString> withWts = params;
    withWts.insert(QStringLiteral("wts"),
                   QString::number(QDateTime::currentSecsSinceEpoch()));

    // 签名规矩：值里的 !'()* 去掉，按 key 排序，RFC3986 urlencode，末尾拼密钥取 md5
    QList<QPair<QString, QString>> items;
    for (auto it = withWts.constBegin(); it != withWts.constEnd(); ++it)
        items.append({ it.key(), it.value() });
    std::sort(items.begin(), items.end(),
              [](const QPair<QString, QString>& a, const QPair<QString, QString>& b) {
                  return a.first < b.first;
              });

    QString query;
    static const QRegularExpression special(QStringLiteral("[!'()*]"));
    for (const auto& kv : items)
    {
        const QString v = QString(kv.second).remove(special);
        query += QUrl::toPercentEncoding(kv.first) + QLatin1Char('=') +
                 QUrl::toPercentEncoding(v) + QLatin1Char('&');
    }
    query.chop(1);

    const QString wRid = QString::fromLatin1(
        QCryptographicHash::hash((query + mixinKey).toUtf8(),
                                 QCryptographicHash::Md5).toHex());
    return query + QStringLiteral("&w_rid=") + wRid;
}

} // namespace

// =============================================================================
//  网易云
// =============================================================================
void searchNetEase(const QString& keyword, ItemListCb cb)
{
    QUrl url(QStringLiteral("https://music.163.com/api/search/get/web"));
    //创建QUrl对象，目标是网易云音乐web版搜索API接口地址
    QUrlQuery q;//构造URL的查询参数
    q.addQueryItem(QStringLiteral("s"), keyword);//搜索关键词
    q.addQueryItem(QStringLiteral("type"), QStringLiteral("1"));//歌曲代码
    q.addQueryItem(QStringLiteral("offset"), QStringLiteral("0"));//分页偏移量
    q.addQueryItem(QStringLiteral("limit"), QStringLiteral("20"));//数量限制
    url.setQuery(q);//拼接搜索
    // 例如：https://music.163.com/api/search/get/web?s=关键词&type=1&offset=0&limit=20

    auto* reply = get(url, {{QStringLiteral("Referer"), QStringLiteral("https://music.163.com")}}, 15000);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, cb] {
        reply->deleteLater();
        QVector<Item> out;
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        const QJsonArray songs = doc.object()
                                     .value(QLatin1String("result")).toObject()
                                     .value(QLatin1String("songs")).toArray();
        for (const QJsonValue& sv : songs)
        {
            const QJsonObject s = sv.toObject();

            // ★ VIP/付费的直接过滤，列表里不显示播不了的歌（用户要求）★
            // fee 约定：0 = 免费，1 = VIP 专享，4 = 需购买专辑，8 = 非会员可放低音质。
            // 1 和 4 无登录播不了 → 不进列表；8 保留（能听）。
            const int fee = s.value(QLatin1String("fee")).toInt();
            if (fee == 1 || fee == 4)
                continue;

            Item it;
            it.source  = NetEase;
            it.id      = QString::number(
                s.value(QLatin1String("id")).toVariant().toLongLong());
            it.title   = jsonStr(s, "name");
            QString artists;
            const QJsonArray ar = s.value(QLatin1String("artists")).toArray();
            for (const QJsonValue& av : ar)
            {
                if (!artists.isEmpty())
                    artists += QStringLiteral(" / ");
                artists += av.toObject().value(QLatin1String("name")).toString();
            }
            it.artist     = artists;
            it.album      = s.value(QLatin1String("album")).toObject()
                                .value(QLatin1String("name")).toString();
            it.durationMs = s.value(QLatin1String("duration")).toVariant().toLongLong();
            it.fee        = fee != 0;
            if (!it.title.isEmpty() && !it.id.isEmpty())
                out.append(it);
        }
        cb(out);
    });
}

void netEaseUrl(const QString& songId, UrlCb cb)
{
    QUrl url(QStringLiteral("https://music.163.com/api/song/enhance/player/url"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("ids"), QStringLiteral("[%1]").arg(songId));
    q.addQueryItem(QStringLiteral("br"), QStringLiteral("320000"));
    url.setQuery(q);

    auto* reply = get(url, {{QStringLiteral("Referer"), QStringLiteral("https://music.163.com")}}, 15000);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, cb] {
        reply->deleteLater();
        QString link, suffix = QStringLiteral("mp3");
        const QJsonArray data = QJsonDocument::fromJson(reply->readAll())
                                    .object()
                                    .value(QLatin1String("data")).toArray();
        if (!data.isEmpty())
        {
            const QJsonObject d = data.at(0).toObject();
            link  = jsonStr(d, "url");
            suffix = jsonStr(d, "type");
            if (suffix.isEmpty())
                suffix = QStringLiteral("mp3");
        }
        // link 为空 = VIP/无版权：调用方转 B 站
        cb(link, suffix);
    });
}

void netEaseLyric(const QString& songId, TextCb cb)
{
    QUrl url(QStringLiteral("https://music.163.com/api/song/lyric"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("id"), songId);
    q.addQueryItem(QStringLiteral("lv"), QStringLiteral("1"));
    q.addQueryItem(QStringLiteral("kv"), QStringLiteral("1"));
    q.addQueryItem(QStringLiteral("tv"), QStringLiteral("-1"));
    url.setQuery(q);

    auto* reply = get(url, {{QStringLiteral("Referer"), QStringLiteral("https://music.163.com")}}, 15000);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, cb] {
        reply->deleteLater();
        cb(QJsonDocument::fromJson(reply->readAll())
               .object()
               .value(QLatin1String("lrc")).toObject()
               .value(QLatin1String("lyric")).toString());
    });
}

// =============================================================================
//  B 站
// =============================================================================
void searchBilibili(const QString& keyword, ItemListCb cb)
{
    ensureWbiKey([cb, keyword](const QString& mixin) {
        if (mixin.isEmpty())
        {
            cb({});                        // 拿不到 WBI 密钥：B 站搜索放弃
            return;
        }

        // ★ 综合排序（search/all/v2）★ 和 B 站网页搜索的"综合"页同源：
        // 结果按综合相关度混合排列。只挑其中**可播的视频条目**（直播/用户/
        // 合集不能当歌听），按综合顺序取前 10。
        QMap<QString, QString> params;
        params.insert(QStringLiteral("keyword"), keyword);
        const QString query = wbiQuery(params, mixin);
        const QUrl url(QStringLiteral("https://api.bilibili.com/x/web-interface/wbi/search/all/v2?")
                       + query);

        auto* reply = get(url, {
            {QStringLiteral("Referer"), QStringLiteral("https://www.bilibili.com")},
            {QStringLiteral("Cookie"), QStringLiteral("buvid3=") + buvid3()},
        }, 15000);
        QObject::connect(reply, &QNetworkReply::finished, reply, [reply, cb] {
            reply->deleteLater();
            QVector<Item> out;
            // result 是"分组"数组：每组 result_type 一类，具体条目在组内 data 里。
            // 按综合顺序遍历分组，只收视频条目，凑满 20 个为止。
            const QJsonArray groups = QJsonDocument::fromJson(reply->readAll())
                                          .object()
                                          .value(QLatin1String("data")).toObject()
                                          .value(QLatin1String("result")).toArray();
            for (const QJsonValue& gv : groups)
            {
                const QJsonObject g = gv.toObject();
                if (g.value(QLatin1String("result_type")).toString() != QLatin1String("video"))
                    continue;

                const QJsonArray data = g.value(QLatin1String("data")).toArray();
                for (const QJsonValue& rv : data)
                {
                    const QJsonObject r = rv.toObject();

                    Item it;
                    it.source  = Bilibili;
                    it.id      = jsonStr(r, "bvid");
                    it.title   = stripTags(jsonStr(r, "title"));
                    it.artist  = jsonStr(r, "author");

                    // 时长是 "mm:ss" / "h:mm:ss" 文本
                    qint64 sec = 0;
                    const QStringList parts = jsonStr(r, "duration").split(QLatin1Char(':'));
                    for (const QString& p : parts)
                        sec = sec * 60 + p.toLongLong();
                    it.durationMs = sec * 1000;

                    if (it.id.isEmpty() || it.title.isEmpty())
                        continue;
                    out.append(it);
                    if (out.size() >= 20)
                        break;
                }
                if (out.size() >= 20)
                    break;
            }
            cb(out);
        });
    });
}

void bilibiliAudio(const QString& bvid, UrlCb cb)
{
    // ① bvid → cid（一个视频一条 cid）
    const QUrl view(QStringLiteral("https://api.bilibili.com/x/web-interface/view?bvid=") + bvid);
    auto* r1 = get(view, {{QStringLiteral("Referer"), QStringLiteral("https://www.bilibili.com")}});
    QObject::connect(r1, &QNetworkReply::finished, r1, [r1, cb, bvid] {
        r1->deleteLater();
        const qint64 cid = QJsonDocument::fromJson(r1->readAll())
                               .object()
                               .value(QLatin1String("data")).toObject()
                               .value(QLatin1String("cid")).toVariant().toLongLong();
        if (cid <= 0)
        {
            cb(QString(), QString());
            return;
        }

        // ② playurl：fnval=16 → DASH（音视频分轨，只取 audio 轨 = 没有视频画面）
        QUrl play(QStringLiteral("https://api.bilibili.com/x/player/playurl"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("bvid"), bvid);
        q.addQueryItem(QStringLiteral("cid"), QString::number(cid));
        q.addQueryItem(QStringLiteral("fnval"), QStringLiteral("16"));
        q.addQueryItem(QStringLiteral("qn"), QStringLiteral("0"));
        play.setQuery(q);

        auto* r2 = get(play, {{QStringLiteral("Referer"), QStringLiteral("https://www.bilibili.com")}});
        QObject::connect(r2, &QNetworkReply::finished, r2, [r2, cb] {
            r2->deleteLater();
            const QJsonArray audio = QJsonDocument::fromJson(r2->readAll())
                                         .object()
                                         .value(QLatin1String("data")).toObject()
                                         .value(QLatin1String("dash")).toObject()
                                         .value(QLatin1String("audio")).toArray();
            QString best;
            qint64 bestBw = -1;
            for (const QJsonValue& av : audio)      // 挑码率最高的一条音频
            {
                const QJsonObject a = av.toObject();
                const qint64 bw = a.value(QLatin1String("bandwidth")).toVariant().toLongLong();
                const QString link = jsonStr(a, "baseUrl");
                if (!link.isEmpty() && bw > bestBw)
                {
                    bestBw = bw;
                    best   = link;
                }
            }
            cb(best, QStringLiteral("m4s"));   // m4s（AAC）→ 调用方用 MfDecode 转 WAV
        });
    });
}

// =============================================================================
//  下载
// =============================================================================
void download(const QString& url, const QString& savePath,
              const QMap<QString, QString>& headers,
              ProgressCb progress, DoneCb done)
{
    if (url.isEmpty())
    {
        if (done)
            done(false, QStringLiteral("没有可用的音频地址"));
        return;
    }

    auto* reply = get(QUrl(url), headers);
    if (progress)
    {
        QObject::connect(reply, &QNetworkReply::downloadProgress, reply,
                         [progress](qint64 got, qint64 total) { progress(got, total); });
    }
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, savePath, done] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            if (done)
                done(false, reply->errorString());
            return;
        }
        const QByteArray data = reply->readAll();
        if (data.isEmpty())
        {
            if (done)
                done(false, QStringLiteral("下载内容为空"));
            return;
        }
        const QFileInfo di(savePath);
        QDir().mkpath(di.absolutePath());
        QFile f(savePath);
        if (!f.open(QIODevice::WriteOnly))
        {
            if (done)
                done(false, QStringLiteral("无法写入文件"));
            return;
        }
        f.write(data);
        f.close();
        if (done)
            done(true, QString());
    });
}

// =============================================================================
//  临时缓存 / 工具
// =============================================================================
QString tempDir()
{
    return QDir::tempPath() + QStringLiteral("/PetPalMusic");
}

void clearTempDir()
{
    QDir d(tempDir());
    if (d.exists())
        d.removeRecursively();
    d.mkpath(tempDir());
}

QString sanitizeFileName(const QString& name)
{
    static const QRegularExpression bad(QStringLiteral("[\\\\/:*?\"<>|]"));
    QString s = QString(name).remove(bad).trimmed();
    while (s.endsWith(QLatin1Char('.')))
        s.chop(1);
    if (s.isEmpty())
        s = QStringLiteral("untitled");
    return s;
}

QString describe()
{
    QString out;
    out += QStringLiteral("临时缓存 : %1（启动清残留 / 退出整删）\r\n").arg(tempDir());
    out += QStringLiteral("接口     : 网易云搜索/直链/歌词 + B站搜索(WBI)/音频流 —— 非官方接口，可能随平台改版失效\r\n");
    out += QStringLiteral("B站音频  : AAC/M4S，经 Windows Media Foundation 转成 WAV 后播放\r\n");
    return out;
}

} // namespace OnlineMusic
