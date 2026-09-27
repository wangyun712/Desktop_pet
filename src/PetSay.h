#pragma once
// =============================================================================
//  PetSay —— 桌宠本体的独白台词池 + 挑句引擎（纯数据 + 纯函数，不碰界面）
//
//  ★ 它和 ChatScript 的分工 ★
//    ChatScript 是"你问我答"——用户先说话，它挑回答；
//    这里是"没人问它也说话"——被点、被拎、落地、犯困、切歌、整点、
//    随机闲聊……全是**主动事件**，没有"用户输入"可以匹配。
//    所以不复用意图匹配，只留两样最值钱的东西：
//      ① 分池的台词（按事件 + 时间段），洛天依口吻；
//      ② 节流 + 避重 —— 连续触发不能变成刷屏复读机。
//
//  ★ 谁来调 ★
//    DesktopPet 在现成的事件点上调 pick()（点击 / 拎起 / 落地 / 睡觉 / 整点），
//    挑到的句子交给 PetBubble 气泡显示。这里不建任何窗口、不碰任何控件。
//
//  ★ 节流为什么分两档 ★
//    主动事件（用户在逗它）默认 5 秒 —— 用户连着点也该有回应，只是别每点
//    一下都换一句；
//    被动事件（闲聊 / 整点）由调用方传 90 秒 —— 没人理它才自言自语，
//    一分钟冒一句刚好，多了就是噪音。
//
//  开关存哪：ui.ini 的 petSay/enabled（loadEnabled / saveEnabled），
//  和字号 / 主题 / 桌面歌词同一个文件 —— 外观与互动类首选项都搁一处。
// =============================================================================

#include <QDate>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTime>
#include <QVector>

namespace PetSay {

// 台词事件。加新事件 = 加一个枚举值 + pool() 里加一个池子。
enum class Event
{
    Clicked,      // 被点了一下
    Lifted,       // 被拎了起来
    Landed,       // 落地
    Sleepy,       // 犯困 / 睡觉
    Dream,        // 睡着后偶尔冒的梦话
    Hourly,       // 整点报时（%1 = 小时数）
    SongChanged,  // 切歌报歌名（%1 = 歌名）
    Dropped,      // 用户把歌拖到了桌宠身上
    DropHint,     // 拖着文件悬在桌宠上（引导提示）
    Petted,       // 面板上被摸摸
    Fed,          // 面板上被喂食
    Angry,        // 连点彩蛋：被戳急了（强制出句，无视节流）
    Idle,         // 随机闲聊
    Greet,        // 时间段问候（启动后第一次露面；greet() 还会先查节日/纪念日）
    EventCount
};

inline QString S(const char* utf8) { return QString::fromUtf8(utf8); }

// ---- 时间段（整点 / 问候按它分池）----
enum class Segment { Morning, Noon, Afternoon, Evening, Night };

inline Segment segmentOf(int hour)
{
    if (hour >= 5 && hour < 11)  return Segment::Morning;    // 早
    if (hour >= 11 && hour < 14) return Segment::Noon;       // 午
    if (hour >= 14 && hour < 18) return Segment::Afternoon;  // 午后
    if (hour >= 18 && hour < 23) return Segment::Evening;    // 晚
    return Segment::Night;                                    // 23~5 深夜
}

// ---- 台词池 ----
// 口吻约束（和 ChatScript.h 一致）：句子短、多用"唔…""呀"、不提自己超过
// 15 岁、不说脏话；没词就老实说没词，不硬凑。
inline const QStringList& pool(Event ev, Segment seg)
{
    static const QStringList clicked = {
        S("唔？叫我呀？"), S("嘿嘿，有什么吩咐？"), S("呀！好痒呀。"),
        S("在的在的～我一直在哦。"), S("唔…又是想让我唱歌？"),
    };
    static const QStringList lifted = {
        S("哇啊——！"), S("拎、拎我做什么呀！"), S("呜…空中好可怕…"),
        S("呀！要飞起来了！"),
    };
    static const QStringList landed = {
        S("唔…稳稳落地。"), S("摔、摔不疼的呀。"), S("落地成功！给自己鼓个掌。"),
        S("唔…下次轻一点放我嘛。"),
    };
    static const QStringList sleepy = {
        S("唔…眼皮好重…"), S("困了呀…那我眯一会儿…"), S("呼啊…（打了个哈欠）…"),
    };
    static const QStringList dream = {
        S("唔…包子…别跑…"), S("（说梦话）…下一首…唱给你听…"),
        S("呼…呼…（睡得香甜）…"), S("唔…天钿…别闹啦…"),
    };
    static const QStringList petted = {
        S("唔…好舒服呀…"), S("再、再摸头的话要长不高的！"),
        S("嘿嘿…摸摸也不白摸，回头的～"), S("唔…被摸得都要睡着啦。"),
    };
    static const QStringList fed = {
        S("好吃！谢谢款待～"), S("唔…吃多了会打嗝的…"),
        S("呀，我最喜欢的！"), S("嗷呜——（大口吃）…唔，饱了。"),
    };
    static const QStringList angry = {
        S("烦死了！别戳了！"), S("呜——再戳我真的要生气了！"),
        S("戳戳戳，就知道戳！哼！"), S("（气鼓鼓）我不理你了！…好吧还是理。"),
    };
    static const QStringList songChanged = {
        S("来听《%1》吧～"), S("《%1》，这首我也很喜欢哦。"),
        S("♪ 换歌啦：《%1》"), S("下一首是《%1》哦。"),
    };
    static const QStringList dropped = {
        S("收到！这就唱～"), S("呀，新歌！我来我来。"), S("唔…让我看看这首歌。"),
    };
    static const QStringList dropHint = {
        S("把歌拖到我身上就好啦～"), S("唔？要放歌吗？拖上来就行哦。"),
    };
    static const QStringList idle = {
        S("唔…今天也想听歌呢。"), S("天钿在旁边打转呀。"),
        S("你有没有好好吃饭呀？"), S("陪我聊聊天嘛～"),
        S("唔…坐久了要起来动一动哦。"), S("嘿嘿，我在这里哦。"),
    };
    // 整点：白天报时间，深夜换成劝睡
    static const QStringList hourlyDay = {
        S("呀，%1 点了。"), S("整点报时：%1 点整！"),
        S("%1 点啦。时间过得好快。"), S("唔…都 %1 点了呀。"),
    };
    static const QStringList hourlyNight = {
        S("呀…都 %1 点了。早点睡吧。"), S("唔…%1 点了哦。深夜了呢。"),
        S("%1 点了…你还不睡吗？"),
    };
    // 问候：按时间段各一小池
    static const QStringList greetMorning = {
        S("早上好呀。今天也从唱歌开始？"), S("早安。唔…我刚刚在哼歌哦。"),
        S("呀，早啊。包子要不要来一个？"),
    };
    static const QStringList greetNoon = {
        S("中午好呀。吃饭了吗？"), S("午安～唔…包子时间到啦。"),
    };
    static const QStringList greetAfternoon = {
        S("下午好呀。要不要来首歌？"), S("唔…下午了哦。来点轻快的？"),
    };
    static const QStringList greetEvening = {
        S("晚上好呀。今天过得怎么样？"), S("晚上了呢。想听慢歌还是快歌？"),
    };
    static const QStringList greetNight = {
        S("这么晚还不睡呀…唔，早点休息哦。"), S("深夜了呀…我唱轻一点的给你听吧。"),
        S("唔…夜深了。要注意身体哦。"),
    };

    switch (ev)
    {
    case Event::Clicked:     return clicked;
    case Event::Lifted:      return lifted;
    case Event::Landed:      return landed;
    case Event::Sleepy:      return sleepy;
    case Event::Dream:       return dream;
    case Event::Petted:      return petted;
    case Event::Fed:         return fed;
    case Event::Angry:       return angry;
    case Event::SongChanged: return songChanged;
    case Event::Dropped:     return dropped;
    case Event::DropHint:    return dropHint;
    case Event::Idle:        return idle;
    case Event::Hourly:      return (seg == Segment::Night) ? hourlyNight : hourlyDay;
    case Event::Greet:
        switch (seg)
        {
        case Segment::Morning:   return greetMorning;
        case Segment::Noon:      return greetNoon;
        case Segment::Afternoon: return greetAfternoon;
        case Segment::Evening:   return greetEvening;
        default:                 return greetNight;
        }
    default:                 return idle;
    }
}

// ---- 节日表（公历固定日期；农历无法离线换算，春节/中秋这类不做）----
// 每个节日一句专属问候，启动见面时优先于普通时间段问候。
struct Festival
{
    int     month;
    int     day;
    QString line;
};

inline const QVector<Festival>& festivals()
{
    static const QVector<Festival> v = {
        {  1,  1, S("元旦啦！新的一年，也请多多关照～") },
        {  2, 14, S("今天是情人节呀。唔…唱首情歌可以吗？") },
        {  5,  1, S("劳动节快乐！唔…放假也要好好休息哦。") },
        {  6,  1, S("儿童节呀！唔…我也是十五岁的小朋友嘛。") },
        { 10,  1, S("国庆节快乐！假期要开开心心的呀。") },
        { 12, 24, S("平安夜呢。愿你今晚做个有甜味的梦。") },
        { 12, 25, S("圣诞节快乐！唔…要一起听歌吗？") },
        { 12, 31, S("今天是今年的最后一天啦。谢谢你陪我到现在。") },
    };
    return v;
}

// ---- 挑句器（DesktopPet 持有一份，节流和避重都在这里）----
struct Picker
{
    QElapsedTimer clock;        // 首次挑句时启动
    qint64        lastMs = -1;  // 上次出句的时刻（-1 = 还没说过）
    QStringList   recent;       // 最近说过的（避重）

    // 挑一句。冷却中 / 池子为空返回空串，调用方直接不显示。
    //   minGapMs：两次出句的最小间隔。主动事件默认 5 秒；
    //   闲聊这类被动事件由 DesktopPet 传 90000。
    //   arg：%1 占位的内容（歌名 / 小时数），没有就原样返回。
    QString pick(Event ev, const QString& arg = QString(), qint64 minGapMs = 5000)
    {
        if (!throttled(minGapMs))
        {
            const Segment seg = segmentOf(QTime::currentTime().hour());
            const QString line = pickFrom(pool(ev, seg), arg);
            if (!line.isEmpty())
                lastMs = clock.elapsed();
            return line;
        }
        return QString();
    }

    // 整点报时：%1 = 小时数。深夜（23~5）自动换劝睡池。
    QString hourly(int hour, qint64 minGapMs = 5000)
    {
        if (!throttled(minGapMs))
        {
            const QString line = pickFrom(pool(Event::Hourly, segmentOf(hour)),
                                          QString::number(hour));
            if (!line.isEmpty())
                lastMs = clock.elapsed();
            return line;
        }
        return QString();
    }

    // 启动问候：节日 > 陪伴里程碑 > 普通时间段问候。
    //   daysTogether = 陪伴天数（AffectionSystem::companyDays()）。
    //   里程碑挑"整日子"：每满 100 天、每满一周年 —— 每天都报就腻了。
    QString greet(int hour, int daysTogether, qint64 minGapMs = 5000)
    {
        if (throttled(minGapMs))
            return QString();

        QString line;

        // ① 节日（公历固定日期，当天命中）
        const QDate today = QDate::currentDate();
        for (const Festival& f : festivals())
        {
            if (f.month == today.month() && f.day == today.day())
            {
                line = f.line;
                break;
            }
        }

        // ② 陪伴里程碑：满 100 天的整数倍、满一年的整数倍
        if (line.isEmpty() && daysTogether > 0
            && (daysTogether % 100 == 0 || daysTogether % 365 == 0))
        {
            line = S("今天是我们在一起的第 %1 天啦。唔…以后也要多多关照哦。");
        }

        // ③ 普通时间段问候
        if (line.isEmpty())
            line = pickFrom(pool(Event::Greet, segmentOf(hour)), QString());

        if (line.isEmpty())
            return QString();

        // 里程碑那句的 %1 = 陪伴天数
        if (line.contains(QStringLiteral("%1")))
            line = line.arg(QString::number(daysTogether));

        recent.append(line);
        while (recent.size() > 3)
            recent.removeFirst();
        lastMs = clock.elapsed();
        return line;
    }

private:
    bool throttled(qint64 minGapMs)
    {
        if (!clock.isValid())
            clock.start();
        return lastMs >= 0 && clock.elapsed() - lastMs < minGapMs;
    }

    // 从池子里挑一句，避开最近说过的 3 句；%1 有 arg 就填上。
    QString pickFrom(const QStringList& lines, const QString& arg)
    {
        if (lines.isEmpty())
            return QString();

        QRandomGenerator* rng = QRandomGenerator::global();
        QString line;
        for (int i = 0; i < 12 && line.isEmpty(); ++i)
        {
            const QString cand = lines.at(int(rng->bounded(lines.size())));
            if (!recent.contains(cand))
                line = cand;                       // 避开最近说过的
        }
        if (line.isEmpty())
            line = lines.at(int(rng->bounded(lines.size())));   // 池子太小避不开就算了

        recent.append(line);
        while (recent.size() > 3)
            recent.removeFirst();

        if (!arg.isEmpty() && line.contains(QStringLiteral("%1")))
            return line.arg(arg);
        return line;
    }
};

// ---- 开关存取（设置页写、DesktopPet 读）----
inline bool loadEnabled()
{
    return QSettings(QSettings::IniFormat, QSettings::UserScope,
                     QStringLiteral("PetPal"), QStringLiteral("ui"))
        .value(QStringLiteral("petSay/enabled"), true).toBool();   // 默认开：会说话才是活的
}

inline void saveEnabled(bool on)
{
    QSettings(QSettings::IniFormat, QSettings::UserScope,
              QStringLiteral("PetPal"), QStringLiteral("ui"))
        .setValue(QStringLiteral("petSay/enabled"), on);
}

// ---- 自检描述（纯只读）----
inline QString describe()
{
    const Segment seg = segmentOf(QTime::currentTime().hour());
    int total = 0;
    const Event all[] = { Event::Clicked, Event::Lifted, Event::Landed, Event::Sleepy,
                          Event::Dream, Event::Petted, Event::Fed, Event::Angry,
                          Event::SongChanged, Event::Dropped, Event::DropHint,
                          Event::Idle, Event::Greet, Event::Hourly };
    for (const Event ev : all)
        total += pool(ev, seg).size();

    // 样例走运行时同一条 pick 路径；把冷却清掉才能连挑两句
    Picker p;
    const QString s1 = p.pick(Event::Clicked);
    p.lastMs = -1;
    const QString s2 = p.pick(Event::SongChanged, QStringLiteral("世末歌者"));
    p.lastMs = -1;
    const QString s3 = p.hourly(21);

    QString out;
    out += QStringLiteral("台词事件 : %1 类\r\n").arg(int(Event::EventCount));
    out += QStringLiteral("台词总量 : %1 句\r\n").arg(total);
    out += QStringLiteral("节日表   : %1 个（公历固定日期，启动问候优先）\r\n").arg(festivals().size());
    out += QStringLiteral("样例     : 被点 →「%1」\r\n").arg(s1);
    out += QStringLiteral("           切歌 →「%1」\r\n").arg(s2);
    out += QStringLiteral("           整点 →「%1」\r\n").arg(s3);
    return out;
}

} // namespace PetSay
