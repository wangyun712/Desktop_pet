#pragma once
// =============================================================================
//  SongLibrary —— 曲库（给「唱快歌 / 唱慢歌 / 接歌词」三件事用）
//
//  ★ 曲库是磁盘上的纯文本，不在代码里、也不进 .qrc ★
//    两个原因：
//      1. 用户可以自己往里加歌 —— 丢一个 .txt 进 resources/songs/ 就行，
//         不用改代码、不用重编 exe（从别的页切回聊天页就重新扫一遍）；
//      2. 歌词是词作的版权，"编进 exe" 不合适 —— 放文本里，用户自己填、自己用。
//    这个头文件因此**一句歌词都没有**，只有格式、解析和匹配。
//
//  ---------------------------------------------------------------------------
//  ▎怎么加一首歌（30 秒）
//
//  在 `resources/songs/` 里新建一个 .txt，内容照这个格式：
//
//  ★ 文件名随你 —— 中文也行（`世末歌者.txt` 没问题）★
//    这些文件是直接读磁盘的，**不经过 .qrc / rcc**，所以不受"资源路径只能用
//    ASCII"那条限制（那条规矩是为 AUTORCC 立的，它按 GBK 解 .qrc 里的路径）。
//    实测：中文名文件照常被扫到、段落和 title 都解析正确。
//    文件名只是这首歌的 `id`（自检报告/日志里显示那个），给用户看的名字是 `title:`。
//    唯一要留心的是**打包分发**的时候：压缩工具在别的机器上解出来别把名字搞坏。
//
//        # 井号开头的行 = 注释，随你怎么写
//        title: 世末歌者          <- 歌名（聊天里就显示这个）
//        artist: COP / 洛天依      <- 可省
//        tempo: fast               <- fast = 快歌、slow = 慢歌（可省，默认 slow）
//        seg:                      <- 起一个新段落（"唱一次"唱的是这些段）
//        第一句歌词
//        第二句歌词
//        seg:
//        下一段第一句
//        ……
//
//    · 一句一行；空行会被忽略；
//    · **从歌词站复制一整段，直接粘在 seg: 下面就行** —— 不用整理。
//      标点带不带都没关系（接歌词匹配时会先去标点空格）；
//      "作词：/作曲：/编曲：/调校：/混音：/演唱：…" 这类元信息行会被自动跳过；
//    · 想要更多段就再写一行 `seg:`；
//    · 保存 → 回到桌宠面板，**从别的页切回「聊天」页即生效**，不用重编、不用重启。
//
//  ▎两首歌各自管什么
//    · 说"快歌" → 从 tempo=fast 的歌里随机挑一首，随机抽 SEGMENTS_PER_REPLY 段连着唱；
//    · 说"慢歌" → 同上，tempo=slow；
//    · 说上句歌词 → 天依接下句（见 findContinuation）。
//    某一边的歌一首都没有时，composeSongReply() 返回空串，
//    由 ChatPage 退回台词库里的兜底台词（"快歌我还在练呢"），不会哑掉。
//
//  ▎想加"另一种分类"（比如"来首摇滚"）
//    1) 这里给 Tempo 加一项；
//    2) 解析 tempo: 那个 if 里加一行；
//    3) ChatScript.h 里照 fastsong/slowsong 再加一条意图；
//    4) ChatPage::songReplyFor() 里加一行分支。
//    四处都是"照葫芦画瓢"，别的地方不用动。
// =============================================================================

#include <QVector>
#include <QStringList>
#include <QString>
#include <QRandomGenerator>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#include <utility>

// 复用台词库那套归一化（去空白/标点、全角转半角、转小写）。
// 接歌词和接话必须是同一套写法，不然"你好！"能匹配台词、却匹配不到歌词。
#include "ChatScript.h"

namespace SongLibrary {

// 说一次"快歌 / 慢歌"，唱几段。
// ★ 改了它就去 composeSongReply() 里把"这两段"那个说法一起改掉 ★
constexpr int SEGMENTS_PER_REPLY = 2;

// 接歌词的最小长度：4 个字以下一律不接 —— "你好""在吗"这种天天说的话
// 万一撞上某句歌词，就会出现"回一句没头没尾的歌词"，比接不上更奇怪。
constexpr int MIN_CONTINUE_CHARS = 3;

enum class Tempo
{
    Fast,   // 快歌
    Slow,   // 慢歌
};

struct Song
{
    QString id;                  // = 文件名（不含后缀）；日志、去重用
    QString title;               // title: 里的名字；没写就用 id 顶上
    QString artist;              // artist:（可空）
    Tempo   tempo = Tempo::Slow;
    QVector<QStringList> segments;   // 每段 = 一句一行的歌词
};

// 一句歌词的坐标
struct LineRef
{
    int song    = -1;
    int segment = -1;
    int line    = -1;
};

struct Continuation
{
    bool    found   = false;
    QString next;                // 天依要接的下一句
    QString songId;
    QString title;
    int     segment = -1;        // 命中的是哪一段哪一句（自检/日志用）
    int     line    = -1;
};

// -----------------------------------------------------------------------------
//  找曲库目录
//
//  为什么不能写相对路径：Qt Creator 跑的时候工作目录是构建目录，
//  双击 exe 的时候又是 exe 目录 —— 两条路的"当前目录"不一样。
//  所以从 exe 所在目录一层层往上爬去找 resources/songs（构建目录在第 2~3 层），
//  再试当前工作目录，最后兜底 exe 旁边的 songs/（有人只拷 exe 走的时候用）。
//  （和 DailyImagePage 找图库是同一个思路。）
// -----------------------------------------------------------------------------
inline QString songsDirPath()
{
    QDir dir(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 4; ++i)
    {
        const QString cand = dir.filePath(QStringLiteral("resources/songs"));
        if (QFileInfo::exists(cand))
            return QDir(cand).absolutePath();
        if (!dir.cdUp())
            break;
    }

    const QString cwd = QDir(QDir::currentPath()).filePath(QStringLiteral("resources/songs"));
    if (QFileInfo::exists(cwd))
        return QDir(cwd).absolutePath();

    const QString near = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("songs"));
    if (QFileInfo::exists(near))
        return QDir(near).absolutePath();

    return QString();
}

// 键值对的分隔符：半角冒号和全角"："都认（歌词站复制过来的多半是全角）
inline bool isKeySep(QChar c)
{
    return c == QLatin1Char(':') || c == QChar(0xFF1A);
}

// 歌词站复制过来的那些"作词：/作曲：…"行，别当成歌词唱出去。
// 判定很保守：行首是这些词、紧接着一个冒号（中英文都认）才算。
inline bool isMetaLine(const QString& line)
{
    static const char* kKeys[] = { "作词", "作曲", "编曲", "调校", "混音", "母带", "演唱",
                                   "出品", "制作人", "和声", "策划", "曲绘", "吉他", "贝斯",
                                   "打击乐", "混音师" };

    for (const char* k : kKeys)
    {
        const QLatin1String key(k);
        if (line.size() <= key.size() || !line.startsWith(key))
            continue;
        if (isKeySep(line.at(key.size())))
            return true;
    }
    return false;
}

inline QString tempoName(Tempo t)
{
    return t == Tempo::Fast ? QStringLiteral("快歌") : QStringLiteral("慢歌");
}

inline QString titleOrId(const Song& s)
{
    return s.title.isEmpty() ? s.id : s.title;
}

// -----------------------------------------------------------------------------
//  解析一个曲库文件
//
//  刻意写得"能吃脏数据"：空行忽略、注释忽略、不认识的键当歌词、
//  没写 title 就用文件名顶上 —— 因为用户是直接粘歌词进来的，不该让他先整理一遍。
// -----------------------------------------------------------------------------
inline Song parseSongFile(const QString& path, QStringList* notes)
{
    Song s;
    s.id = QFileInfo(path).completeBaseName();

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        if (notes)
            notes->append(QStringLiteral("读不了 %1").arg(QFileInfo(path).fileName()));
        return s;
    }

    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);

    QStringList cur;                 // 正在攒的那一段

    while (!ts.atEnd())
    {
        const QString line = ts.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;                // 空行 / 注释

        // ---- 元信息（键名大小写不敏感，冒号全角半角都认）----
        const auto valueOf = [&line](const char* key) -> QString {
            const QLatin1String k(key);
            if (line.size() <= k.size() || !line.startsWith(k, Qt::CaseInsensitive))
                return QString();
            if (!isKeySep(line.at(k.size())))
                return QString();
            return line.mid(k.size() + 1).trimmed();
        };

        if (const QString v = valueOf("title"); !v.isEmpty())  { s.title  = v; continue; }
        if (const QString v = valueOf("artist"); !v.isEmpty()) { s.artist = v; continue; }
        if (const QString v = valueOf("tempo"); !v.isEmpty())
        {
            // 认 fast/slow，也认中文的"快/快歌/慢/慢歌"
            const QString t = v.toLower();
            s.tempo = (t == QLatin1String("fast") || t.contains(QStringLiteral("快")))
                          ? Tempo::Fast : Tempo::Slow;
            continue;
        }
        if (line.size() > 3 && line.startsWith(QLatin1String("seg"), Qt::CaseInsensitive)
            && isKeySep(line.at(3)))
        {
            if (!cur.isEmpty())                 // 收上一段
                s.segments.append(cur);
            cur.clear();
            continue;
        }

        if (isMetaLine(line))
            continue;

        cur.append(line);            // 其余一律当歌词
    }

    if (!cur.isEmpty())
        s.segments.append(cur);

    if (s.title.isEmpty())
        s.title = s.id;

    return s;
}

// -----------------------------------------------------------------------------
//  曲库
// -----------------------------------------------------------------------------
struct Library
{
    QVector<Song> songs;
    QStringList   notes;    // 加载过程的说明（自检打印用）
    QString       dir;      // 实际用到的目录；空 = 没找到

    // 重新扫一遍磁盘。切回聊天页时调一次 —— 这就是"动态添加"的全部机关：
    // 用户丢个 .txt 进目录，切回来它就出现了。
    void reload()
    {
        songs.clear();
        notes.clear();
        dir = songsDirPath();

        if (dir.isEmpty())
        {
            notes.append(QStringLiteral("没找到 resources/songs 目录"));
            return;
        }

        const QStringList files = QDir(dir).entryList(QStringList() << QStringLiteral("*.txt"),
                                                     QDir::Files, QDir::Name);
        if (files.isEmpty())
            notes.append(QStringLiteral("目录里没有 .txt"));

        for (const QString& name : files)
        {
            const Song s = parseSongFile(QDir(dir).filePath(name), &notes);
            if (s.segments.isEmpty())
            {
                notes.append(QStringLiteral("%1 里没有歌词，跳过").arg(name));
                continue;            // 只有元信息、没词的文件不进库（不然会抽出空回复）
            }
            songs.append(s);
        }
    }

    // 这种节奏的歌里随机挑一首。没有就返回 nullptr（调用方退兜底台词）。
    const Song* pickByTempo(Tempo t) const
    {
        QVector<int> hit;
        for (int i = 0; i < songs.size(); ++i)
            if (songs.at(i).tempo == t)
                hit.append(i);

        if (hit.isEmpty())
            return nullptr;
        return &songs.at(hit.at(int(QRandomGenerator::global()->bounded(hit.size()))));
    }
};

// -----------------------------------------------------------------------------
//  点歌：抽 SEGMENTS_PER_REPLY 段拼成一次回复
//
//  抽的是"不重复的段"。段数不够就全唱（有 1 段就唱 1 段，不会重复同一段两遍）。
//  没有这种节奏的歌 → 返回空串，由调用方退回台词库的兜底台词。
// -----------------------------------------------------------------------------
inline QString composeSongReply(const Library& lib, Tempo tempo)
{
    const Song* s = lib.pickByTempo(tempo);
    if (!s || s->segments.isEmpty())
        return QString();

    // 洗牌（Fisher–Yates）后取前 N 个。比"随机抽 N 次再去重"稳：
    // 段数恰好等于 N 的时候后者要撞好几次才凑齐。
    QVector<int> order;
    order.reserve(s->segments.size());
    for (int i = 0; i < s->segments.size(); ++i)
        order.append(i);
    for (int i = order.size() - 1; i > 0; --i)
        std::swap(order[i], order[int(QRandomGenerator::global()->bounded(i + 1))]);

    QString out = QStringLiteral("《%1》—— 我唱这两段给你听哦。\n").arg(titleOrId(*s));

    const int n = qMin(SEGMENTS_PER_REPLY, order.size());
    for (int k = 0; k < n; ++k)
    {
        out += QLatin1Char('\n');                                // 段与段之间空一行
        out += s->segments.at(order.at(k)).join(QLatin1Char('\n'));
    }
    return out;
}

// 段末 / 整首唱完时接哪儿：
//   本段还有下一句 -> 就它；
//   到段尾了       -> 接下一段的第一句；
//   整首都唱完了   -> 绕回第一句（"唱到最后又从头开始"，比"没有下一句"更像在唱歌）。
inline QString nextLineOf(const Song& s, int segment, int line)
{
    if (segment >= 0 && segment < s.segments.size())
    {
        const QStringList& seg = s.segments.at(segment);
        if (line + 1 < seg.size())
            return seg.at(line + 1);
    }

    for (int gi = segment + 1; gi < s.segments.size(); ++gi)
        if (!s.segments.at(gi).isEmpty())
            return s.segments.at(gi).first();

    for (int gi = 0; gi < s.segments.size(); ++gi)
        if (!s.segments.at(gi).isEmpty())
            return s.segments.at(gi).first();

    return QString();
}

// -----------------------------------------------------------------------------
//  接歌词：用户说上句，天依接下句
//
//  命中分两档，精确的优先：
//    ① 整句一样（用户把这一句完整打出来了）
//    ② 库里某句包含用户的话（用户只打了半句）—— 反过来"用户的话包含库里某句"也算
//  一个都没命中就 found=false，照常走台词库。
// -----------------------------------------------------------------------------
inline Continuation findContinuation(const Library& lib, const QString& userInput)
{
    Continuation c;

    const QString q = ChatScript::normalize(userInput);
    if (q.size() < MIN_CONTINUE_CHARS)
        return c;                    // 太短，不接（见 MIN_CONTINUE_CHARS 的说明）

    QVector<LineRef> exact;
    QVector<LineRef> loose;

    for (int si = 0; si < lib.songs.size(); ++si)
    {
        const Song& s = lib.songs.at(si);
        for (int gi = 0; gi < s.segments.size(); ++gi)
        {
            const QStringList& seg = s.segments.at(gi);
            for (int li = 0; li < seg.size(); ++li)
            {
                const QString n = ChatScript::normalize(seg.at(li));
                if (n.isEmpty())
                    continue;

                if (n == q)
                    exact.append(LineRef{ si, gi, li });
                else if (n.contains(q) || q.contains(n))
                    loose.append(LineRef{ si, gi, li });
            }
        }
    }

    const QVector<LineRef>& pool = exact.isEmpty() ? loose : exact;
    if (pool.isEmpty())
        return c;

    const LineRef r = pool.at(int(QRandomGenerator::global()->bounded(pool.size())));
    const Song&   s = lib.songs.at(r.song);

    c.next    = nextLineOf(s, r.segment, r.line);
    c.found   = !c.next.isEmpty();
    c.songId  = s.id;
    c.title   = titleOrId(s);
    c.segment = r.segment;
    c.line    = r.line;
    return c;
}

// -----------------------------------------------------------------------------
//  自检描述：曲库有几首、每首几段几句，再拿真实数据各跑一遍点歌和接歌词
//  （纯只读，不写任何东西）
// -----------------------------------------------------------------------------
inline QString firstLine(const QString& text)
{
    const int nl = text.indexOf(QLatin1Char('\n'));
    return nl < 0 ? text : text.left(nl);
}

inline QString describeLibrary()
{
    Library lib;
    lib.reload();

    QString out;
    out += QStringLiteral("  曲库目录  : %1\r\n")
               .arg(lib.dir.isEmpty() ? QStringLiteral("(没找到)") : lib.dir);
    out += QStringLiteral("  歌曲数量  : %1 首\r\n").arg(lib.songs.size());

    for (const Song& s : std::as_const(lib.songs))
    {
        int lines = 0;
        for (const QStringList& g : s.segments)
            lines += g.size();

        out += QStringLiteral("    · %1（%2）%3 —— %4 段 / %5 句\r\n")
                   .arg(titleOrId(s), s.id, tempoName(s.tempo),
                        QString::number(s.segments.size()), QString::number(lines));
    }

    // 不走"另写一段样例代码"：直接调用运行时那两个入口，跑出来的就是真会唱的东西。
    const QString fast = firstLine(composeSongReply(lib, Tempo::Fast));
    const QString slow = firstLine(composeSongReply(lib, Tempo::Slow));
    out += QStringLiteral("  点歌试跑  : 快歌 → %1\r\n")
               .arg(fast.isEmpty() ? QStringLiteral("(曲库里没有快歌)") : fast);
    out += QStringLiteral("              慢歌 → %1\r\n")
               .arg(slow.isEmpty() ? QStringLiteral("(曲库里没有慢歌)") : slow);

    if (!lib.songs.isEmpty() && !lib.songs.first().segments.isEmpty()
        && !lib.songs.first().segments.first().isEmpty())
    {
        const QString probe = lib.songs.first().segments.first().first();
        const Continuation c = findContinuation(lib, probe);
        out += QStringLiteral("  接歌词试跑: 「%1」\r\n").arg(probe);
        out += QStringLiteral("              → %1\r\n")
                   .arg(c.found ? QStringLiteral("《%1》 接「%2」").arg(c.title, c.next)
                                : QStringLiteral("没接上（检查 findContinuation）"));
    }

    for (const QString& n : std::as_const(lib.notes))
        out += QStringLiteral("  [!] %1\r\n").arg(n);

    return out;
}

} // namespace SongLibrary
