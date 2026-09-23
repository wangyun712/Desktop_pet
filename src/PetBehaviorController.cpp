#include "PetBehaviorController.h"

#include <QTimer>
#include <QRandomGenerator>

// =============================================================================
//  构造函数
// =============================================================================
PetBehaviorController::PetBehaviorController(QObject* parent) : QObject(parent)
{
    // parent 传 this：两个定时器都会随本对象一起被 delete

    // 单次定时器：每次超时后做一次决定，然后按新的随机间隔重新排期
    m_decide = new QTimer(this);
    m_decide->setSingleShot(true);
    connect(m_decide, &QTimer::timeout, this, &PetBehaviorController::onDecide);

    // 每秒跑一次，只用来累加"多久没互动了"
    m_watch = new QTimer(this);
    m_watch->setInterval(1000);
    connect(m_watch, &QTimer::timeout, this, &PetBehaviorController::onIdleWatch);
}

// =============================================================================
//  开始自动行为
// =============================================================================
void PetBehaviorController::start()
{
    m_idleSec = 0;
    m_sleeping = false;

    m_watch->start();

    // 程序刚启动时先让它站一会儿再动。
    // 直接就走会显得很突兀，而且用户刚打开程序时还在看它。
    scheduleNext(randBetween(PetCfg::IDLE_MIN_MS, PetCfg::IDLE_MAX_MS));
}

// =============================================================================
//  暂停 / 恢复自动行为
// =============================================================================
void PetBehaviorController::setAutoEnabled(bool on)
{
    if (m_auto == on)
        return;

    m_auto = on;

    if (!on)
    {
        // 暂停时不只是"不再开始新动作"，也要把它从睡眠里拉出来，
        // 否则会出现"暂停了但一直躺着不动"的怪状态。
        m_sleeping = false;
        m_decide->stop();
    }
    else
    {
        m_idleSec = 0;
        scheduleNext(randBetween(PetCfg::IDLE_MIN_MS, PetCfg::IDLE_MAX_MS));
    }
}

// =============================================================================
//  用户互动
// =============================================================================
void PetBehaviorController::notifyUserInteraction()
{
    m_idleSec = 0;       // 无互动计时清零，重新开始数

    const bool wasSleeping = m_sleeping;
    m_sleeping = false;  // 被叫醒

    if (!m_auto)
        return;

    // 互动之后重新排一次决定。
    // 这样"刚玩完它又立刻自己跑掉"的情况不会发生。
    if (wasSleeping)
    {
        // 刚睡醒：让它清醒地站一会儿
        scheduleNext(PetCfg::IDLE_MIN_MS);
    }
    else
    {
        scheduleNext(randBetween(PetCfg::IDLE_MIN_MS, PetCfg::IDLE_MAX_MS));
    }
}

// =============================================================================
//  菜单里点了"睡觉"
// =============================================================================
void PetBehaviorController::sleepNow()
{
    if (!m_auto)
        return;
    m_sleeping = true;
    m_idleSec = PetCfg::SLEEP_AFTER_SEC;   // 直接算成"已经很久没互动"
    emit requestState(PetState::Sleep, PetCfg::MENU_SLEEP_MS);
    scheduleNext(randBetween(PetCfg::IDLE_MIN_MS, PetCfg::IDLE_MAX_MS));
}

// =============================================================================
//  每秒一次：统计无互动时长
// =============================================================================
void PetBehaviorController::onIdleWatch()
{
    if (!m_auto || m_sleeping)
        return;

    ++m_idleSec;

    if (m_idleSec >= PetCfg::SLEEP_AFTER_SEC)
        beginSleepSequence();
}

// =============================================================================
//  犯困 -> 睡着
//  按需求：IDLE -> DAZE -> SLEEP，所以先发呆一段时间，再真正闭眼。
// =============================================================================
void PetBehaviorController::beginSleepSequence()
{
    m_sleeping = true;   // 立刻置位，避免这一秒的 watch 又触发一次

    emit requestState(PetState::Daze, PetCfg::DAZE_BEFORE_SLEEP_MS);

    // singleShot 的第三个参数是"上下文对象"：万一本对象被销毁了，这个回调会自动取消，
    // 不会出现"对象没了回调还在跑"的崩溃。
    QTimer::singleShot(PetCfg::DAZE_BEFORE_SLEEP_MS, this, [this]()
    {
        // 万一在这 2.5 秒里用户点了它（m_sleeping 被清掉），就不要睡着了
        if (m_sleeping)
            emit requestState(PetState::Sleep, PetCfg::MENU_SLEEP_MS);
    });
}

// =============================================================================
//  排下一次决定
// =============================================================================
void PetBehaviorController::scheduleNext(int ms)
{
    m_decide->start(qMax(1, ms));
}

// =============================================================================
//  到点了：决定接下来干什么
// =============================================================================
void PetBehaviorController::onDecide()
{
    // 被暂停：什么也不做，但仍要按随机间隔排期，
    // 这样用户点"恢复"之后能马上继续（而不是等一个很长的旧周期）。
    if (!m_auto)
    {
        scheduleNext(randBetween(PetCfg::IDLE_MIN_MS, PetCfg::IDLE_MAX_MS));
        return;
    }

    // 睡觉期间不安排任何动作（特别是绝对不要自己走起来）
    if (m_sleeping)
    {
        scheduleNext(5000);
        return;
    }

    // ---- 掷骰子决定做什么 ----
    // 概率写在 PetConfig.h 里，方便调。
    const int roll = randBetween(0, 99);
    const int pWalk = PetCfg::PROB_WALK;
    const int pDaze = pWalk + PetCfg::PROB_DAZE;
    const int pSit = pDaze + PetCfg::PROB_SIT;

    if (roll < pWalk)
    {
        // 走路：方向和时长都是随机的
        const int dir = (randBetween(0, 1) == 0) ? +1 : -1;
        const int dur = randBetween(PetCfg::WALK_MIN_MS, PetCfg::WALK_MAX_MS);
        emit requestWalk(dir, dur);

        // 走完之后 DesktopPet 会自己回待机，这里只要按"走的时长 + 一段随机站立"排下一次决定
        scheduleNext(dur + randBetween(PetCfg::IDLE_MIN_MS, PetCfg::IDLE_MAX_MS));
        return;
    }

    if (roll < pDaze)
    {
        const int hold = randBetween(PetCfg::DAZE_MIN_MS, PetCfg::DAZE_MAX_MS);
        emit requestState(PetState::Daze, hold);
        scheduleNext(hold + randBetween(PetCfg::GAP_MIN_MS, PetCfg::GAP_MAX_MS));
        return;
    }

    if (roll < pSit)
    {
        const int hold = randBetween(PetCfg::SIT_MIN_MS, PetCfg::SIT_MAX_MS);
        emit requestState(PetState::Sit, hold);
        scheduleNext(hold + randBetween(PetCfg::GAP_MIN_MS, PetCfg::GAP_MAX_MS));
        return;
    }

    // 剩下的概率：就地站着待机一段时间
    const int hold = randBetween(PetCfg::IDLE_MIN_MS, PetCfg::IDLE_MAX_MS);
    emit requestState(PetState::Idle, hold);
    scheduleNext(hold);
}

// =============================================================================
//  取 [lo, hi] 之间的随机整数
//
//  用 QRandomGenerator::global()，它是 Qt 提供的全局随机数发生器，
//  不需要自己 srand，质量也够用（Mersenne Twister）。
// =============================================================================
int PetBehaviorController::randBetween(int lo, int hi)
{
    if (hi <= lo)
        return lo;

    // bounded(upper) 返回 [0, upper-1]，所以要 +1
    return lo + int(QRandomGenerator::global()->bounded(quint32(hi - lo + 1)));
}
