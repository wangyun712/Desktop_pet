#pragma once
// =============================================================================
//  ChatScript —— 「聊天」页的台词库 + 匹配引擎（纯数据 + 纯函数，不碰界面）
//
//  ★ 刻意不接大模型 ★
//    流程：输入 → 加权打分选出意图 → 从该意图的台词池里挑一句 → 打字机显示。
//    好处：离线、秒回、不花算力，而且**永远不会 OOC** —— 每句话都是手写的。
//    代价：听不懂没写进去的说法。所以两件事必须做好：
//      ① 兜底池要写得像"没听懂也很天依"，不能干巴巴一句"我不明白"；
//      ② 兜底台词里要主动把能聊的话题摊开 —— 这一页没有快捷气泡，
//         用户想知道"能聊什么"只能靠兜底那句（改这里的时候别忘了这件事）。
//
//  ★ 为什么台词写在头文件里、编译进 exe ★
//    用户明确选的。好处是 exe 拷到哪都自带台词，不可能丢；
//    代价是改一句台词要重编（ChatScript.h 在 CMakeLists 的 PETPAL_HEADERS 里，
//    动它会让所有 .cpp 重编，约一分钟）。
//
//  ★ 匹配用"加权打分"，不是 if-else 顺序匹配 ★
//    每条意图带若干「触发词 + 权重」，输入里命中就累加，取分最高的意图。
//    为什么不用 if-else 顺序匹配：多意图同时命中时，顺序就直接决定了结果，很脆；
//    而且"加一个说法"会被迫去调 if 的顺序。打分法下加说法就是加一行数据。
//    否定句也靠它自然解决 —— 见 negativeWords 那段的说明。
//
//  ★ 挑选台词的三条去重规则（都在 Picker::pick 里）★
//    ① 连续同一个话题 → 先给一句"你刚说过呀"，避免变成复读机；
//    ② 能抽到回显片段时用"回声式"台词（把用户自己的词嵌进回复），显得像听懂了；
//    ③ 好感度阶段 >= 亲近时优先用熟络版台词 —— 关系变好，说话方式跟着变。
//    ★ Picker 是独立的小结构体，不依赖界面 ★ 所以 --chattrace 能拿它跑一遍，
//      跑的就是运行时同一份代码（和 AffectionSystem 那套自检一个思路）。
// =============================================================================

#include <QString>
#include <QStringList>
#include <QVector>
#include <QRandomGenerator>

namespace ChatScript {

// 台词都是中文，写成 QStringLiteral 到处都是噪声。这个短别名只在本文件里用。
// 依赖编译器的 /utf-8（CMakeLists 里已经加了），窄字符串字面量就是 UTF-8。
inline QString S(const char* utf8) { return QString::fromUtf8(utf8); }

// 回复的情绪。ChatPage 负责把它翻成桌宠要播的动作（映射写在 DesktopPet 里）。
// 为什么不直接用 PetState：台词库不该依赖动画模块，保持这里只有数据和字符串。
enum class Mood
{
    Neutral,     // 普通闲聊，不切动作（切了反而打断它正在做的事）
    Happy,
    Shy,         // 害羞：没有专用素材，翻成 Surprise
    Sad,
    Thinking,
};

struct Trigger
{
    QString word;
    int     weight;
};

struct Intent
{
    QString          id;            // 内部标识（轨迹/自检打印用）
    QString          label;         // 中文说明
    QVector<Trigger> triggers;
    QStringList      replies;       // 通用台词池
    QStringList      warmReplies;   // 阶段 >= 亲近 时优先用；空则退回 replies
    QStringList      echoReplies;   // 含 %1 的"回声式"台词；抽得到回显片段时用
    Mood             mood     = Mood::Neutral;
    int              priority = 0;  // 同分时的决胜（越大越优先）
};

struct Hit
{
    int     intentIndex = -1;   // -1 = 没命中任何意图 → 走兜底
    int     score       = 0;
    QString id;                 // 命中意图的标识（兜底时为空）
    QString label;              // 命中意图的中文说明
    QString input;              // 归一化之后的输入
    QString echo;               // 可回显的片段（可能为空）
};

// =============================================================================
//  台词库
//
//  写台词的几条自我约束（对着角色设定来的，改的时候别丢掉）：
//    · 句子短，多用"唔…""呀"，不用网络梗；
//    · 不提自己超过 15 岁、不说成熟的话、不说脏话；
//    · 开心就提唱歌和包子；难过的话题就安静听着，先安慰再说话；
//    · 不懂就老实说不懂 —— 兜底池就是干这个的。
// =============================================================================
inline const QVector<Intent>& intents()
{
    static const QVector<Intent> v = {

        { S("greet"), S("打招呼"),
          { { S("你好"), 3 },   { S("您好"), 3 },   { S("哈喽"), 3 },   { S("嗨"), 2 },
            { S("hi"), 2 },     { S("hello"), 2 },  { S("在吗"), 2 },   { S("在不在"), 2 },
            { S("早上好"), 3 }, { S("中午好"), 3 }, { S("下午好"), 3 }, { S("晚上好"), 3 },
            { S("好久不见"), 3 }, { S("又见面"), 2 } },
          { S("唔…你好呀。（轻轻摆了摆手）"),
            S("呀，你来了。天钿刚才还在打转呢。"),
            S("你好呀。今天过得怎么样？"),
            S("唔…我在的哦。"),
            S("嗨呀。我刚刚在哼一段旋律，被你打断了呢。") },
          {}, {}, Mood::Happy, 0 },

        { S("whoareyou"), S("问身份"),
          { { S("你是谁"), 4 }, { S("你叫什么"), 4 }, { S("叫什么名字"), 4 },
            { S("你的名字"), 4 }, { S("自我介绍"), 4 }, { S("你是哪位"), 3 } },
          { S("我是洛天依呀。唔…从瓦纳海姆神州领来的，现在住在这里唱歌。"),
            S("洛天依。唔…名字有点长吗？你叫我天依也可以的。"),
            S("唔…我叫洛天依。旁边飘着的这个是音之精灵，叫天钿。"),
            S("洛天依，Vsinger 的歌手。唔…不过你不用记这么多，记得我会唱歌就好。") },
          {}, {}, Mood::Neutral, 2 },

        { S("ability"), S("问会什么"),
          { { S("你会什么"), 4 }, { S("你能做什么"), 4 }, { S("你能干什么"), 4 },
            { S("你会做什么"), 4 } },
          { S("唔…我会唱歌呀。还有，陪着你说话。"),
            S("唔…会唱一点歌，会发呆，会想吃包子。"),
            S("唔…我做的事情不多呀。不过你要是想聊天，我可以一直陪着。") },
          {}, {}, Mood::Happy, 2 },

        { S("praise"), S("夸她"),
          { { S("可爱"), 3 },   { S("好看"), 2 },   { S("漂亮"), 3 }, { S("萌"), 2 },
            { S("温柔"), 3 },   { S("好美"), 2 } },
          { S("唔…这样夸我的话，我会不好意思的呀。"),
            S("诶…？（把耳机往下压了压）谢谢、谢谢你。"),
            S("唔…真的吗？那我要把这句话记住。"),
            S("唔…我又没有做什么啦。") },
          { S("嘿嘿…那你要一直这么觉得哦。"),
            S("唔…你每次都这么说，我会当真的呀。") },
          {}, Mood::Shy, 1 },

        { S("touch"), S("抱抱摸摸"),
          { { S("抱抱"), 3 },   { S("抱一下"), 3 }, { S("摸摸"), 2 },   { S("摸摸头"), 3 },
            { S("摸摸你"), 3 }, { S("揉揉"), 2 },   { S("握手"), 2 },   { S("牵一下"), 2 } },
          { S("唔…！（愣了一下）可以、可以是可以啦。"),
            S("唔…有点痒呀。"),
            S("抱抱的话…那、那你要轻一点。"),
            S("唔…天钿好像也想被摸一下。") },
          {}, {}, Mood::Shy, 2 },

        { S("sing"), S("求唱歌"),
          { { S("唱歌"), 3 },   { S("唱一首"), 4 },   { S("唱首歌"), 4 },   { S("唱个歌"), 4 },
            { S("来一首"), 4 }, { S("为我唱"), 3 },   { S("给我唱"), 3 },   { S("唱给我听"), 4 },
            { S("会不会唱"), 3 }, { S("唱一下吧"), 3 }, { S("想听你唱"), 4 } },
          { S("唔…好啊。不过我先想一下，唱哪一首比较合适呢。"),
            S("会一点点呀。你想听快一点的，还是慢慢的？"),
            S("嗯…那我清一下嗓子。（认真地站好）"),
            S("唔…唱歌的话，我什么时候都可以的。") },
          { S("想听什么都可以哦，唱给你一个人听也行。") },
          {}, Mood::Happy, 2 },

        // ---- 点歌：快歌 / 慢歌 ----
        // ★ 这两条的 replies 是"曲库里没有这种歌"时的兜底台词 ★
        //   正常情况下 ChatPage 会拿 SongLibrary 里的歌词整段替换掉（见 songReplyFor），
        //   所以这里写的是"我还没学会"这类话，只在用户还没往 resources/songs 里填词时出现。
        //   priority = 5 是为了压过 sing（用户说"唱首快歌"时两条都能命中，得让点歌赢）。
        { S("fastsong"), S("点快歌"),
          { { S("快歌"), 5 },   { S("快一点"), 3 },  { S("快节奏"), 4 },   { S("来首快的"), 5 },
            { S("唱首快的"), 5 }, { S("嗨一点"), 3 },  { S("燃一点"), 3 },   { S("带劲"), 3 },
            { S("有活力"), 3 } },
          { S("唔…快的吗。我还得再练练气，先欠着好不好。"),
            S("快歌呀…那我先深吸一口气。唔…还差一点点。") },
          { S("今天想听快的呀。那我把最喜欢的这段唱给你。"),
            S("（跳了两下，把耳机扶正）好，我准备好了。") },
          {}, Mood::Happy, 5 },

        { S("slowsong"), S("点慢歌"),
          { { S("慢歌"), 5 },   { S("慢一点"), 3 },  { S("来首慢的"), 5 }, { S("唱首慢的"), 5 },
            { S("抒情的"), 4 },  { S("温柔的歌"), 4 }, { S("安静的歌"), 4 }, { S("舒缓"), 4 },
            { S("摇篮曲"), 4 },  { S("催眠"), 3 } },
          { S("唔…慢的歌呀。那我轻轻地唱，你听着就好。"),
            S("好呀。慢慢唱的话，每个字都放得下。") },
          { S("陪着你听慢的歌，最好了。"),
            S("那就慢慢的来。你要是困了，我就再轻一点。") },
          {}, Mood::Happy, 5 },

        { S("praiseSing"), S("夸唱功"),
          { { S("好听"), 3 },   { S("唱得好"), 3 }, { S("唱得真好"), 4 },
            { S("声音好听"), 4 }, { S("好厉害"), 2 }, { S("天籁"), 2 } },
          { S("唔…（脸有点红）谢谢你，我会继续唱下去的。"),
            S("真的吗…那我想把这首歌唱给更多人听。"),
            S("唔…被这样说，好像更有力气了。") },
          {}, {}, Mood::Shy, 1 },

        { S("food"), S("吃与包子"),
          { { S("包子"), 4 }, { S("吃"), 2 },   { S("饿"), 3 },   { S("好饿"), 4 },
            { S("吃饭"), 3 }, { S("好吃的"), 3 }, { S("零食"), 3 }, { S("甜品"), 2 },
            { S("馋"), 2 } },
          { S("包子！唔…你也是来说包子的吗？"),
            S("唔…说到吃的话，我最近很喜欢刚出笼的包子呀。"),
            S("有点饿了呀…不过唱歌之前不能吃太多，会打嗝的。"),
            S("唔…你要不要也去吃点东西？别饿着。"),
            S("唔…甜的也喜欢，咸的也喜欢。这个很难选呀。") },
          {}, {}, Mood::Happy, 1 },

        { S("partner"), S("聊伙伴与天钿"),
          { { S("乐正绫"), 4 }, { S("阿绫"), 4 },   { S("言和"), 4 },   { S("乐正龙牙"), 4 },
            { S("龙牙"), 4 },   { S("徵羽摩柯"), 4 }, { S("摩柯"), 4 },   { S("墨清弦"), 4 },
            { S("清弦"), 4 },   { S("天钿"), 5 },   { S("伙伴"), 2 },   { S("你的朋友"), 3 } },
          { S("唔…你说阿绫呀？她最近好像又在练新曲子。"),
            S("言和唱歌的时候很稳的，我一直很羡慕。"),
            S("龙牙哥话很少，不过他其实很照顾大家。"),
            S("摩柯…唔，他最近好像又长高了一点点哦。"),
            S("清弦练琴很认真的，我有时候会去旁边听。"),
            S("天钿呀，它就飘在我旁边。今天也乖乖的。") },
          {}, {}, Mood::Happy, 2 },

        { S("doing"), S("问在做什么"),
          { { S("在干嘛"), 3 },   { S("在干什么"), 3 }, { S("在做什么"), 3 },
            { S("干嘛呢"), 2 },   { S("在忙什么"), 3 }, { S("在想什么"), 3 },
            { S("做什么呢"), 2 } },
          { S("唔…刚刚在哼一段旋律，还没哼完。"),
            S("在想事情呀。有时候想着想着就变成歌了。"),
            S("天钿在打转，我在看它打转。"),
            S("唔…在等一个人来跟我说话。现在等到了。") },
          {}, {}, Mood::Thinking, 2 },

        { S("age"), S("问年龄身高"),
          { { S("多大"), 3 }, { S("几岁"), 3 }, { S("年龄"), 3 },
            { S("多高"), 3 }, { S("身高"), 3 } },
          { S("唔…十五岁呀。身高是…一百五十六厘米。"),
            S("唔，这个要说出来吗？…好吧，十五岁。"),
            S("唔…问这个做什么呀。（小声）十五岁。") },
          {}, {}, Mood::Shy, 2 },

        { S("color"), S("代表色"),
          { { S("什么颜色"), 3 }, { S("代表色"), 4 }, { S("天依蓝"), 5 },
            { S("喜欢的颜色"), 3 } },
          { S("天依蓝呀，就是这种颜色哦。"),
            S("唔…是天依蓝。有点像天空刚亮起来的时候。"),
            S("唔…你猜猜看？…是天依蓝啦。") },
          {}, {}, Mood::Happy, 2 },

        { S("thanks"), S("道谢"),
          { { S("谢谢"), 3 }, { S("感谢"), 3 }, { S("多谢"), 3 },
            { S("谢啦"), 2 }, { S("辛苦了"), 2 } },
          { S("唔…不用谢呀。"),
            S("嘿嘿，能帮上忙就好。"),
            S("唔…你这么客气，我反而不好意思了。") },
          {}, {}, Mood::Happy, 2 },

        { S("apology"), S("道歉"),
          { { S("对不起"), 3 }, { S("抱歉"), 3 }, { S("不好意思"), 3 }, { S("我错了"), 2 } },
          { S("唔…没事的呀，我没有生气。"),
            S("不用道歉的。你没事就好。"),
            S("唔…真的没关系的，别放在心上。") },
          {}, {}, Mood::Neutral, 2 },

        { S("happy"), S("用户开心"),
          { { S("开心"), 3 }, { S("高兴"), 3 }, { S("太好了"), 3 },
            { S("好耶"), 2 }, { S("今天很顺利"), 3 } },
          { S("唔…那太好了呀。"),
            S("诶，你遇到什么好事了？说来听听。"),
            S("唔…你开心的话，我也有点想唱歌了。") },
          {}, {}, Mood::Happy, 1 },

        { S("love"), S("表白"),
          { { S("喜欢你"), 4 }, { S("最喜欢你"), 6 }, { S("爱你"), 4 },
            { S("想你了"), 4 }, { S("我想你"), 3 } },
          { S("诶…！（耳机晃了一下）那个、那个…谢谢你。"),
            S("唔…我不太会说这种话，但是…我很开心。真的。"),
            S("你这样说，我会不知道要接什么了呀…") },
          { S("唔…我也是呀。一直、一直都是。") },
          {}, Mood::Shy, 3 },

        { S("sad"), S("心情不好"),
          { { S("不开心"), 6 }, { S("难过"), 5 }, { S("伤心"), 5 }, { S("好累"), 5 },
            { S("累"), 3 },     { S("疲惫"), 4 }, { S("郁闷"), 4 }, { S("难受"), 4 },
            { S("委屈"), 4 },   { S("想哭"), 5 }, { S("压力"), 4 }, { S("烦"), 4 },
            { S("讨厌"), 4 },   { S("被骂"), 4 }, { S("失败了"), 3 } },
          { S("唔…（安静地听着）你不用急着说清楚，我在这里。"),
            S("那…我先不唱歌了。你要是想说，我就一直听着。"),
            S("唔…辛苦了。要不要靠过来一点，我陪着你。"),
            S("没关系的呀。这种时候，也可以慢慢来的。") },
          { S("唔…今天也辛苦了。要不要说说看？") },
          { S("…「%1」呀。唔，那你今天一定很辛苦吧。"),
            S("唔…「%1」。我听见了，真的听见了。") },
          Mood::Sad, 3 },

        { S("bye"), S("告别"),
          { { S("再见"), 4 },   { S("拜拜"), 4 },   { S("我走了"), 4 },
            { S("先走了"), 4 }, { S("下次见"), 4 }, { S("晚安"), 5 },
            { S("bye"), 3 },    { S("睡觉去"), 3 } },
          { S("唔…要走了吗。那路上小心呀。"),
            S("再见啦。下次来的时候，我唱新歌给你听。"),
            S("晚安呀。做个有甜味的梦。"),
            S("唔…那我会在这里的。") },
          { S("嗯…那你要记得回来哦。我会等着的。") },
          {}, Mood::Neutral, 2 },
    };
    return v;
}

// 兜底：什么都没匹配上时用。★ 这里必须承担"告诉用户能聊什么"的责任 ★
// （这一页刻意没有快捷话题气泡，可发现性全靠这几句）
inline const QStringList& fallbackReplies()
{
    static const QStringList v = {
        S("唔…这个我有点听不明白呀。"),
        S("唔…（歪了歪头）我好像没听懂。你可以问我愿不愿意唱歌，或者…天钿今天乖不乖。"),
        S("诶…？我还在听呢，你再说说看。"),
        S("唔…我不太明白这个。不过你要是愿意讲，我会慢慢听的。"),
        S("唔…（认真地想了想）这个我答不上来呀。要不我们聊点别的？"),
    };
    return v;
}

// 兜底 + 回声式：把用户自己的词嵌回去，比干巴巴的"我不懂"像听懂了
inline const QStringList& fallbackEchoReplies()
{
    static const QStringList v = {
        S("「%1」呀…唔，这个我不太懂，但是我想听你多说一点。"),
        S("唔…「%1」？我不太明白呢。是不是很重要的事情？"),
        S("「%1」…这个词我记下了，不过我还不太懂它的意思。"),
    };
    return v;
}

// 连续同一个话题时用，避免变成复读机
inline const QStringList& repeatReplies()
{
    static const QStringList v = {
        S("唔…你刚才也说过这个呀。"),
        S("诶…又是这个吗？（轻轻笑了笑）"),
        S("唔…我们是不是刚聊过这个？不过我不介意的。"),
        S("呀，你很喜欢这个话题呢。"),
    };
    return v;
}

// 开场白
inline const QStringList& openingLines()
{
    static const QStringList v = {
        S("（轻轻歪头，耳机微微晃动，眼眸亮晶晶看着你）你好呀，我是洛天依。"
          "天钿就在旁边飘着哦，你是想来听歌，还是随便聊聊？"),
    };
    return v;
}

// 负向词：命中就说明用户在说"不"。
// ★ 为什么需要它 ★ 「我不喜欢你」里含「喜欢你」，光看触发词会把表白意图选中。
//   规则简单粗暴但够用：命中负向词时，**除 sad 以外的意图全部扣分**，sad 加分。
//   为什么不做精确的"否定域判定"：那需要分词和句法，对短句收益很低。
inline const QStringList& negativeWords()
{
    static const QStringList v = {
        S("不喜欢"), S("不爱"),   S("不好听"), S("难听"),  S("不想听"),
        S("别唱"),   S("太吵"),   S("好烦"),   S("烦死"),  S("讨厌"),
        S("走开"),   S("闭嘴"),
    };
    return v;
}

// =============================================================================
//  引擎
// =============================================================================

// 归一化：去空白、去标点符号、全角转半角、英文转小写。
// 为什么要它：用户会打「你好！」「你好。」「你好 ，」—— 不去标点就得为每种写法
// 各加一个触发词。全角转半角是为了让「ＨＩ」也能命中 hi。
inline QString normalize(const QString& in)
{
    QString s;
    s.reserve(in.size());
    for (const QChar c : in)
    {
        if (c.isSpace())
            continue;

        const ushort u = c.unicode();
        if (u == 0x3000)                    // 全角空格
            continue;
        if (u >= 0xFF01 && u <= 0xFF5E)     // 全角 ASCII → 半角
        {
            s.append(QChar(ushort(u - 0xFEE0)));
            continue;
        }
        if (c.isPunct() || c.isSymbol())
            continue;

        s.append(c);
    }
    return s.toLower();
}

// 从用户输入里抽一个"值得回显的片段"。
// 做法很朴素：去掉人称、时间、语气这些词之后剩下的拿去用。
// 抽不出来（太短/太长/只剩虚词）就返回空，调用方会自动退回普通台词。
inline QString echoFragment(const QString& raw)
{
    QString s = normalize(raw);
    if (s.size() < 2)
        return QString();

    static const QStringList noise = {
        S("我"),   S("你"),   S("他"),   S("她"),   S("的"),   S("了"),
        S("呀"),   S("啊"),   S("吧"),   S("呢"),   S("吗"),   S("嘛"),
        S("哦"),   S("哈"),   S("今天"), S("昨天"), S("明天"), S("现在"),
        S("然后"), S("其实"), S("就是"), S("感觉"), S("觉得"), S("有点"),
        S("一些"), S("这个"), S("那个"), S("真的"), S("好"),   S("很"),
    };
    for (const QString& w : noise)
        s.remove(w);

    if (s.isEmpty() || s.size() > 12)
        return QString();

    return s;
}

// 打分选出意图。全部函数式，不碰状态 —— 所以自检/轨迹可以直接调。
inline Hit matchIntent(const QString& raw)
{
    Hit h;
    h.input = normalize(raw);
    h.echo  = echoFragment(raw);

    if (h.input.isEmpty())
        return h;

    bool negative = false;
    for (const QString& w : negativeWords())
    {
        if (h.input.contains(w))
        {
            negative = true;
            break;
        }
    }

    const QVector<Intent>& all = intents();

    int best = -1;
    int bestScore = 0;
    int bestPrio = -1;

    for (int i = 0; i < all.size(); ++i)
    {
        const Intent& it = all.at(i);

        int score = 0;
        for (const Trigger& t : it.triggers)
        {
            if (h.input.contains(t.word))
                score += t.weight;
        }
        if (score <= 0)
            continue;

        if (negative)
            score += (it.id == S("sad")) ? 5 : -5;      // ★ 否定句：正向意图扣分、负面意图加分

        if (score <= 0)
            continue;

        if (score > bestScore || (score == bestScore && it.priority > bestPrio))
        {
            best = i;
            bestScore = score;
            bestPrio = it.priority;
        }
    }

    // 说了负面的话、但一条意图都没匹配上 —— 也该用温柔的方式接住，而不是回一句"我不懂"
    if (best < 0 && negative)
    {
        for (int i = 0; i < all.size(); ++i)
        {
            if (all.at(i).id == S("sad"))
            {
                best = i;
                bestScore = 1;
                break;
            }
        }
    }

    h.intentIndex = best;
    h.score = bestScore;
    if (best >= 0)
    {
        h.id    = all.at(best).id;
        h.label = all.at(best).label;
    }
    return h;
}

// -----------------------------------------------------------------------------
//  台词挑选器
//
//  为什么单独抽一个结构体、而不是塞进 ChatPage 里：ChatPage 是界面，
//  塞进去之后 --chattrace 就没法在不建窗口的情况下跑同一份挑词逻辑了，
//  于是"轨迹里测的"和"实际跑的"就成了两份代码 —— 这个项目已经吃过一次亏
//  （AffectionSystem 那边刻意把所有时间逻辑挤进纯函数，就是为了同一件事）。
// -----------------------------------------------------------------------------
struct Picker
{
    QStringList recent;        // 最近说过的话（存模板），挑的时候避开
    QString     lastIntentId;  // 上一句的意图，用来识别"同一个话题反复说"
    int         stage = 0;     // 好感度阶段 0~4；>= 2（亲近）时优先用熟络版台词

    QString pick(const Hit& h)
    {
        const QString r = pickInner(h);
        lastIntentId = (h.intentIndex >= 0) ? h.id : QStringLiteral("__fallback__");
        return r;
    }

private:
    QString pickInner(const Hit& h)
    {
        // ---- 兜底 ----
        if (h.intentIndex < 0)
        {
            if (!h.echo.isEmpty())
                return fill(choose(fallbackEchoReplies()), h.echo);
            return choose(fallbackReplies());
        }

        const Intent& it = intents().at(h.intentIndex);

        // ① 连续同一个话题 → 先说一句"你刚说过呀"
        if (!lastIntentId.isEmpty() && it.id == lastIntentId && !repeatReplies().isEmpty())
            return choose(repeatReplies());

        // ② 情绪类的话优先用回声式（能抽到片段时），显得在认真听
        if (!it.echoReplies.isEmpty() && !h.echo.isEmpty())
            return fill(choose(it.echoReplies), h.echo);

        // ③ 关系够近了、而且这个意图写了熟络版 → 用熟络版
        if (stage >= 2 && !it.warmReplies.isEmpty())
            return fill(choose(it.warmReplies), h.echo);

        return fill(choose(it.replies), h.echo);
    }

    // 随机挑一条，尽量避开最近说过的。
    // 池子比"最近记录"还小的时候一定避不开，那时就随机给一条，反正不能卡住。
    QString choose(const QStringList& pool)
    {
        if (pool.isEmpty())
            return QString();

        QRandomGenerator* rng = QRandomGenerator::global();

        QString pick;
        for (int i = 0; i < 12 && pick.isEmpty(); ++i)
        {
            const QString cand = pool.at(int(rng->bounded(pool.size())));
            if (!recent.contains(cand))
                pick = cand;
        }
        if (pick.isEmpty())
            pick = pool.at(int(rng->bounded(pool.size())));

        recent.append(pick);
        while (recent.size() > 5)
            recent.removeFirst();

        return pick;
    }

    // 把 %1 换成回显片段。没有占位符的模板原样返回 ——
    // 不给它硬塞 .arg()：模板里没有 %1 时那个调用没有意义，还容易看错。
    static QString fill(const QString& tpl, const QString& echo)
    {
        if (echo.isEmpty() || !tpl.contains(QLatin1Char('%')))
            return tpl;
        return tpl.arg(echo);
    }
};

} // namespace ChatScript
