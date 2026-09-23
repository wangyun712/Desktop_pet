#pragma once
// =============================================================================
//  PetBehaviorController —— 自动行为控制器
//
//  它决定「桌宠什么时候自己干点什么」，但不亲自动手：
//    · 什么时候站起来、什么时候发呆、什么时候坐下、什么时候走一段路
//    · 多久没人和它互动了，该开始犯困、该睡觉了
//
//  决定好之后，它只是发信号（requestState / requestWalk），
//  真正的状态切换和移动由 DesktopPet 执行。
//
//  它【不】碰窗口、不碰图片、不碰鼠标 —— 那些都不是"决策"。
//
//  计时设计：
//    用一个"单次定时器" m_decide，每次做完决定就按【随机】间隔重新排期。
//    这样动作之间不会像秒表一样规整，桌宠看起来才像活的。
// =============================================================================

#include <QObject>

#include "PetConfig.h"

class QTimer;

class PetBehaviorController : public QObject
{
    Q_OBJECT
public:
    explicit PetBehaviorController(QObject* parent = nullptr);

    void start();                       // 开始自动行为（程序启动时调用一次）
    void setAutoEnabled(bool on);       // 右键菜单的"暂停 / 恢复自动行为"
    bool autoEnabled() const { return m_auto; }
    bool isSleeping() const { return m_sleeping; }

    // 用户和桌宠互动了（点击 / 拖动 / 右键菜单）。
    // 作用：把"无互动计时"清零；如果正在睡觉就把睡眠标记清掉（叫醒）。
    void notifyUserInteraction();

    // 用户通过菜单要求它立刻睡觉
    void sleepNow();

signals:
    // 请 DesktopPet 切换到某个状态并保持 holdMs 毫秒（holdMs = 0 表示不定时）
    void requestState(PetState s, int holdMs);

    // 请 DesktopPet 走一段路：direction = +1 向右 / -1 向左
    void requestWalk(int direction, int durationMs);

private slots:
    void onDecide();      // 到点了，决定接下来做什么
    void onIdleWatch();   // 每秒跑一次，统计"多久没互动了"

private:
    void scheduleNext(int ms);   // 排下一次决定的时间
    void beginSleepSequence();   // 发呆 -> 睡着
    static int randBetween(int lo, int hi);   // 取 [lo, hi] 内的随机毫秒数

    QTimer* m_decide = nullptr;   // 单次定时器：下一次"做决定"的时刻
    QTimer* m_watch = nullptr;    // 每秒一次：无互动计时

    bool m_auto = true;           // 自动行为总开关（false = 被用户暂停）
    bool m_sleeping = false;      // 是不是正在睡
    int m_idleSec = 0;            // 已经多少秒没有互动了
};
