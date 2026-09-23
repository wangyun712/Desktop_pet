#include "AffectionSystem.h"

#include <QSettings>
#include <QTimer>

// =============================================================================
//  构造
// =============================================================================
AffectionSystem::AffectionSystem(bool persistent, QObject* parent)
    : QObject(parent), m_persistent(persistent)
{
    m_clock.start();
    m_day = QDate::currentDate();
    m_firstDay = m_day;

    if (m_persistent)
        load();

    // 每 30 秒结算一次"陪伴 + 衰减"。
    // 注意它结算的是"实际过了多久"（用 m_clock 量），不是"响了几次" ——
    // 所以系统卡顿、定时器被拖后腿都不会算错。
    m_timer = new QTimer(this);
    m_timer->setInterval(PetCfg::AFF_DECAY_TICK_MS);
    connect(m_timer, &QTimer::timeout, this, &AffectionSystem::onTimer);
    m_timer->start();
}

AffectionSystem::~AffectionSystem()
{
    // 退出前存一次。平时的每一次变动其实都已经即时存过了，这里是兜底。
    if (m_persistent)
        save();
}

// =============================================================================
//  等级 / 阶段
// =============================================================================
QDate AffectionSystem::today() const
{
    // 内部的"今天"。正常运行 m_timeOffsetMs 恒为 0，返回值就是真实日期；
    // 自检把时钟往前拨的时候，日期要跟着一起走 —— 否则"跨天重置"根本没法验证。
    if (m_timeOffsetMs <= 0)
        return QDate::currentDate();
    return QDate::currentDate().addDays(int(m_timeOffsetMs / (24LL * 3600 * 1000)));
}

int AffectionSystem::level() const
{
    // 从高往低找第一个"够得着"的等级。10 级而已，直接循环，不用查表。
    for (int L = PetCfg::AFF_MAX_LEVEL; L >= 1; --L)
    {
        if (m_point >= affLevelFloor(L))
            return L;
    }
    return 1;
}

int AffectionSystem::stageIndex() const
{
    // 10 级分 5 个阶段，每 2 级一个：1-2 / 3-4 / 5-6 / 7-8 / 9-10
    return qBound(0, (level() - 1) / 2, 4);
}

QString AffectionSystem::stageName() const
{
    return affStageName(stageIndex());
}

double AffectionSystem::levelFloor() const
{
    return affLevelFloor(level());
}

double AffectionSystem::levelCeil() const
{
    return affLevelFloor(level() + 1);
}

double AffectionSystem::levelGained() const
{
    return qMax(0.0, m_point - levelFloor());
}

double AffectionSystem::levelNeed() const
{
    return levelCeil() - levelFloor();
}

int AffectionSystem::petLeftToday() const
{
    return qMax(0, PetCfg::AFF_PET_PER_DAY - m_petUsed);
}

int AffectionSystem::feedLeftToday() const
{
    return qMax(0, PetCfg::AFF_FEED_PER_DAY - m_feedUsed);
}

int AffectionSystem::chatLeftToday() const
{
    return qMax(0, PetCfg::AFF_CHAT_PER_DAY - m_chatUsed);
}

int AffectionSystem::companyDays() const
{
    if (!m_firstDay.isValid())
        return 1;
    return qMax(1, int(m_firstDay.daysTo(today())) + 1);
}

// =============================================================================
//  加分
// =============================================================================
double AffectionSystem::addFromPet()
{
    checkNewDay();
    if (m_petUsed >= PetCfg::AFF_PET_PER_DAY)
        return 0.0;                                  // 今天的额度用完了

    const qint64 now = nowMs();
    // m_lastPetMs < 0 表示"从没摸过"，直接放行 —— 不能拿它跟 0 比，
    // 否则程序刚启动那几秒里的第一次摸摸会被当成连点。
    if (m_lastPetMs >= 0 && now - m_lastPetMs < PetCfg::AFF_PET_COOLDOWN_MS)
        return 0.0;                                  // 连点太快，这一下不算

    m_lastPetMs = now;
    ++m_petUsed;
    return gain(PetCfg::AFF_PET_POINT, /*countInteraction=*/true);
}

double AffectionSystem::addFromFeed()//喂食加分，限制加分次数
{
    checkNewDay();
    if (m_feedUsed >= PetCfg::AFF_FEED_PER_DAY)
        return 0.0;

    ++m_feedUsed;
    return gain(PetCfg::AFF_FEED_POINT, /*countInteraction=*/true);
}

double AffectionSystem::addSource(Source s, double amount, int cooldownMs)
{
    checkNewDay();

    // 冷却只对"需要限流"的来源生效（拖动、菜单动作）。聊天和陪伴不设冷却。
    checkNewDay();
    if (cooldownMs > 0)
    {
        const qint64 now = nowMs();
        qint64* slot = nullptr;
        if (s == Source::Drag)       slot = &m_lastDragMs;
        else if (s == Source::MenuAction) slot = &m_lastMenuMs;

        if (slot)
        {
            // 同上：负数 = 从没触发过，不算冷却
            if (*slot >= 0 && now - *slot < cooldownMs)
                return 0.0;
            *slot = now;
        }
    }
    if(m_chatUsed < PetCfg::AFF_CHAT_PER_DAY)
    {
        m_chatUsed++;
        return gain(amount, /*countInteraction=*/true);
    }
    else return 0.0;
}

double AffectionSystem::gain(double amount, bool countInteraction)
{
    if (amount <= 0.0)
        return 0.0;

    const int beforeLevel = level();
    const int beforeStage = stageIndex();

    m_point += amount;
    m_todayGain += amount;
    if (countInteraction)
        ++m_totalInteractions;

    save();          // 每次变动立刻落盘：QSettings 写 ini 很便宜，换来"随时拔电不丢数据"
    emit changed();

    // 升级 / 换阶段的信号。两个都发，因为调用方关心的事情不一样：
    //   升级   -> 播开心、面板闪一下
    //   换阶段 -> 以后接 Qwen 时用来换语气
    if (level() != beforeLevel)
        emit leveledUp(level());
    if (stageIndex() != beforeStage)
        emit stageChanged(stageIndex());

    return amount;
}

void AffectionSystem::resetAll()
{
    m_point      = 0.0;
    m_todayGain  = 0.0;
    m_todayDecay = 0.0;
    m_petUsed    = 0;
    m_feedUsed   = 0;
    m_chatUsed   = 0;        // ★ 之前漏了：聊天次数只增不清，重置后还顶着旧计数 ★
    m_todayCompany = 0.0;
    m_totalInteractions = 0;
    m_companyAccMs = 0;

    // 冷却时间戳也一起清掉，回到"从没触发过"。额度都归零了、
    // "还剩 10 次"却因为 3 秒冷却点不动，看起来就像按钮坏了。
    // （用 AFF_NO_COOLDOWN 而不是 0 —— 0 会被当成"刚刚才摸过"，见 PetConfig.h）
    m_lastPetMs  = PetCfg::AFF_NO_COOLDOWN;
    m_lastDragMs = PetCfg::AFF_NO_COOLDOWN;
    m_lastMenuMs = PetCfg::AFF_NO_COOLDOWN;

    m_day = today();          // 用内部时钟的"今天"，这样自检里拨过时间也不会错位
    m_firstDay = m_day;
    save();
    emit changed();

    // ★ 这里刻意不发 leveledUp ★
    //  重置后等级回到 1，那是"重来"不是"升级"。发了的话面板会弹一句
    //  "升级了！现在是 Lv.1"，看着像出了 bug。
    //  （m_lastSettleMs / m_timeOffsetMs 也不动：它们是时钟基准，
    //    清掉会让"上次结算到哪一刻"错位，下一次结算凭空多算一段时间。）
}

// =============================================================================
//  跨天：今日额度清零
// =============================================================================
void AffectionSystem::checkNewDay()
{
    const QDate t = today();
    if (t == m_day)
        return;

    m_day = t;
    m_petUsed  = 0;
    m_feedUsed = 0;
    m_chatUsed = 0;        // 聊天次数也按自然日重置（原来漏了，会一直累加）
    m_todayGain  = 0.0;
    m_todayDecay = 0.0;
    m_todayCompany = 0.0;
    m_companyAccMs = 0;
    save();
    emit changed();
}

// =============================================================================
//  结算一段时间：陪伴加分 + 自然衰减
//
//  两者共用同一个时间片，因为它们的依据是同一个"过了多久"。
//  clampWindow 只在自检里被关掉 —— 那是刻意要一次模拟一整天，
//  正常运行必须开着，否则电脑睡一觉醒来会被一次性扣掉一大截。
// =============================================================================
double AffectionSystem::settle(qint64 dtMs, bool clampWindow)
{
    if (dtMs <= 0)
        return 0.0;

    checkNewDay();

    qint64 eff = dtMs;
    if (clampWindow)
        eff = qMin(eff, qint64(PetCfg::AFF_DECAY_WINDOW_MS));

    // ---- ① 陪伴加分：每凑够 30 分钟 +1，每天最多 AFF_COMPANY_MAX_DAY 分 ----
    if (m_companyActive)
    {
        m_companyAccMs += eff;
        while (m_companyAccMs >= PetCfg::AFF_COMPANY_EVERY_MS)
        {
            m_companyAccMs -= PetCfg::AFF_COMPANY_EVERY_MS;
            if (m_todayCompany < PetCfg::AFF_COMPANY_MAX_DAY)
            {
                m_todayCompany += 1.0;
                gain(PetCfg::AFF_COMPANY_POINT, /*countInteraction=*/false);
            }
        }
    }

    // ---- ② 自然衰减 ----
    // 满级之后不再衰减：都到顶了还往回掉，只会让人莫名其妙。
    if (isMaxLevel())
        return 0.0;

    double d = PetCfg::AFF_DECAY_PER_HOUR * (double(eff) / 3600000.0);
    d = qMin(d, PetCfg::AFF_DECAY_MAX_TICK);
    if (d <= 0.0)
        return 0.0;

    const double floorPoint = levelFloor();          // 本级起点：衰减到此为止
    double np = m_point - d;
    if (np < floorPoint)
        np = floorPoint;

    const double realDecay = m_point - np;
    if (realDecay <= 0.0)
        return 0.0;                                  // 已经贴着本级起点，衰减暂时无效

    m_point = np;
    m_todayDecay += realDecay;
    save();
    emit changed();
    return realDecay;
}

void AffectionSystem::onTimer()
{
    // 结算"从上次结算到现在"这一段。
    // 用实测时间而不是定时器的名义间隔 —— 定时器被卡后腿时，间隔是 30 秒还是 3 分钟，
    // 结算出来的衰减量都应该一样。
    const qint64 now = nowMs();
    const qint64 dt  = now - m_lastSettleMs;
    m_lastSettleMs = now;
    settle(dt, /*clampWindow=*/true);
}

// =============================================================================
//  存档
// =============================================================================
QString AffectionSystem::savePath() const
{
    const QSettings s(QSettings::IniFormat, QSettings::UserScope,
                      QStringLiteral("PetPal"), QStringLiteral("affection"));
    return s.fileName();
}

void AffectionSystem::save() const
{
    if (!m_persistent)
        return;

    QSettings s(QSettings::IniFormat, QSettings::UserScope,
                QStringLiteral("PetPal"), QStringLiteral("affection"));
    s.setValue(QStringLiteral("point"),             m_point);
    s.setValue(QStringLiteral("day"),               m_day.toString(Qt::ISODate));
    s.setValue(QStringLiteral("petUsed"),           m_petUsed);
    s.setValue(QStringLiteral("feedUsed"),          m_feedUsed);
    s.setValue(QStringLiteral("todayGain"),         m_todayGain);
    s.setValue(QStringLiteral("todayDecay"),        m_todayDecay);
    s.setValue(QStringLiteral("todayCompany"),      m_todayCompany);
    s.setValue(QStringLiteral("totalInteractions"), m_totalInteractions);
    s.setValue(QStringLiteral("firstDay"),          m_firstDay.toString(Qt::ISODate));
    s.sync();
}

void AffectionSystem::load()
{
    if (!m_persistent)
        return;

    QSettings s(QSettings::IniFormat, QSettings::UserScope,
                QStringLiteral("PetPal"), QStringLiteral("affection"));

    m_point             = s.value(QStringLiteral("point"), 0.0).toDouble();
    m_petUsed           = s.value(QStringLiteral("petUsed"), 0).toInt();
    m_feedUsed          = s.value(QStringLiteral("feedUsed"), 0).toInt();
    m_todayGain         = s.value(QStringLiteral("todayGain"), 0.0).toDouble();
    m_todayDecay        = s.value(QStringLiteral("todayDecay"), 0.0).toDouble();
    m_todayCompany      = s.value(QStringLiteral("todayCompany"), 0.0).toDouble();
    m_totalInteractions = s.value(QStringLiteral("totalInteractions"), 0).toInt();

    const QDate d = QDate::fromString(s.value(QStringLiteral("day")).toString(), Qt::ISODate);
    m_day = d.isValid() ? d : QDate::currentDate();

    const QDate f = QDate::fromString(s.value(QStringLiteral("firstDay")).toString(), Qt::ISODate);
    m_firstDay = f.isValid() ? f : m_day;

    if (m_point < 0.0)
        m_point = 0.0;

    // 存档里的"今天"如果不是今天，说明关着程序跨天了 —— 直接走一次跨天逻辑把额度清掉。
    checkNewDay();
}

// =============================================================================
//  自检用
// =============================================================================
void AffectionSystem::debugStopTimer()
{
    if (m_timer)
        m_timer->stop();
}

double AffectionSystem::debugLateTick(qint64 dtMs)
{
    // ★ 和 debugAdvanceTime 的区别就在 clampWindow ★
    //   debugAdvanceTime 是"模拟过了很久"（自检要一天一天地推，不能被钳位拦住），
    //   这个是"模拟定时器这一响晚了很久"—— 走的是 onTimer() 真正的那条路径，
    //   所以时间窗钳位必须生效，否则测的就不是真东西。
    if (dtMs <= 0)
        return 0.0;

    m_timeOffsetMs += dtMs;
    const double d = settle(dtMs, /*clampWindow=*/true);
    m_lastSettleMs = nowMs();
    return d;
}

double AffectionSystem::debugAdvanceTime(qint64 ms, bool running)
{
    if (ms <= 0)
        return 0.0;

    // 按 1 小时切片推进。切片是为了让"跨 0 点"和"陪伴每 30 分钟 +1"
    // 都能在推进过程中自然发生，而不是等推完一整天再一次性结算。
    const qint64 slice = 3600000;
    double decayed = 0.0;

    qint64 left = ms;
    while (left > 0)
    {
        const qint64 step = qMin(left, slice);
        m_timeOffsetMs += step;
        left -= step;

        if (running)
            decayed += settle(step, /*clampWindow=*/false);
        else
            checkNewDay();     // 程序关着也会跨天：额度该重置还是要重置
    }
    // 时钟被我们拨过了，同步一下"上次结算时刻"，免得之后真实的定时器
    // 把这段被模拟掉的时间又算一遍。
    m_lastSettleMs = nowMs();
    return decayed;
}

QString AffectionSystem::describe() const
{
    QString s;
    s += QStringLiteral("好感度系统\r\n");
    s += QStringLiteral("  存档文件      : %1%2\r\n")
             .arg(savePath())
             .arg(m_persistent ? QString() : QStringLiteral("（本次不读写存档）"));
    s += QStringLiteral("  当前点数      : %1\r\n").arg(m_point, 0, 'f', 2);
    s += QStringLiteral("  等级 / 阶段   : Lv.%1 %2\r\n").arg(level()).arg(stageName());
    s += QStringLiteral("  本级进度      : %1 / %2（距 Lv.%3 还差 %4）\r\n")
             .arg(levelGained(), 0, 'f', 1)
             .arg(levelNeed(), 0, 'f', 0)
             .arg(qMin(level() + 1, PetCfg::AFF_MAX_LEVEL))
             .arg(qMax(0.0, levelCeil() - m_point), 0, 'f', 1);
    s += QStringLiteral("  今日          : 加 %1，衰减吃掉 %2\r\n")
             .arg(m_todayGain, 0, 'f', 1).arg(m_todayDecay, 0, 'f', 1);
    s += QStringLiteral("  今日额度      : 摸摸 %1/%2 已用，喂食 %3/%4 已用\r\n")
             .arg(m_petUsed).arg(PetCfg::AFF_PET_PER_DAY)
             .arg(m_feedUsed).arg(PetCfg::AFF_FEED_PER_DAY);
    s += QStringLiteral("  陪伴          : %1 天（首次使用 %2）\r\n")
             .arg(companyDays()).arg(m_firstDay.toString(QStringLiteral("yyyy-MM-dd")));
    s += QStringLiteral("  累计互动      : %1 次\r\n").arg(m_totalInteractions);
    s += QStringLiteral("  衰减速率      : %1 点/小时（只在程序运行时算；掉到本级起点就停，不降级）\r\n")
             .arg(PetCfg::AFF_DECAY_PER_HOUR);
    s += QStringLiteral("  等级门槛      : Lv.1->2 %1 点，Lv.9->10 %2 点，满级累计 %3 点\r\n")
             .arg(affLevelNeed(1), 0, 'f', 0)
             .arg(affLevelNeed(PetCfg::AFF_MAX_LEVEL - 1), 0, 'f', 0)
             .arg(affLevelFloor(PetCfg::AFF_MAX_LEVEL), 0, 'f', 0);
    return s;
}
