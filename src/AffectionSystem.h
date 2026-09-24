#pragma once
// =============================================================================
//  AffectionSystem —— 好感度（只管数据，一行界面代码都没有）
//
//  为什么把它和界面分开？
//    好感度的数值会被四个地方碰到：点桌宠、拖桌宠、右键菜单、面板按钮；
//    还外加一个"时间自己在走"的衰减。如果这些逻辑写在面板类里，
//    面板一销毁数据就散了，而且桌宠不打开面板时也没法加分。
//    所以：数值和规则全在这里，面板只负责"把数字画出来 + 发按钮请求"。
//
//  ★ 三个必须理解的规则（都写在 PetConfig 的常量上）★
//    ① 等级只升不降 —— 衰减最多把本级进度吃光，不会把你打回上一级。
//    ② 每天有额度   —— 摸摸 10 次、喂食 3 次，用完当天不再给分（面板会写还剩几次）。
//    ③ 时间只有一个来源 —— 内部所有时间都走 nowMs()，它等于"单调时钟 + 一个可调偏移"。
//       正常运行偏移永远是 0；--affectiontrace 靠调这个偏移来模拟"过了一天"，
//       所以轨迹里跑的代码就是真正跑的代码，不是另写一套公式。
// =============================================================================

#include <QObject>
#include <QDate>
#include <QElapsedTimer>
#include <QString>

#include "PetConfig.h"   // 等级公式 affLevelNeed/affLevelFloor、各种上限常量

class QTimer;

class AffectionSystem : public QObject
{
    Q_OBJECT
public:
    // 加分的来源。每种来源加多少分、有没有冷却，都看 PetConfig 里的常量。
    enum class Source
    {
        Drag,        // 拎起来拖了一段（松手时结算）
        MenuAction,  // 右键菜单里的玩耍动作
        Chat,        // 聊天
        Company      // 纯陪伴：程序开着就慢慢加
    };

    // persistent = false 时完全不碰存档文件（--affectiontrace 用，
    // 免得自检把用户真正的数据改了）。
    explicit AffectionSystem(bool persistent = true, QObject* parent = nullptr);
    ~AffectionSystem() override;

    // ---------- 查询 ----------
    double  point() const { return m_point; }
    int     level() const;                 // 1 ~ AFF_MAX_LEVEL
    int     stageIndex() const;            // 0 ~ 4
    QString stageName() const;             // 初识 / 熟悉 / 亲近 / 心动 / 挚爱
    double  levelFloor() const;            // 本级起点（累计分）
    double  levelCeil() const;             // 下一级门槛
    double  levelGained() const;           // 本级已经攒了多少
    double  levelNeed() const;             // 本级一共要多少
    bool    isMaxLevel() const { return level() >= PetCfg::AFF_MAX_LEVEL; }

    int    petLeftToday() const;           // 今天还能摸几次
    int    feedLeftToday() const;          // 今天还能喂几次
    int    chatLeftToday() const;          // 今天还能加几次"聊天"的分（★★ 不是能聊几次话 ★★
                                           //   聊天本身不设次数限制，AFF_CHAT_PER_DAY 只卡加分）
    double todayGain() const { return m_todayGain; }
    double todayDecay() const { return m_todayDecay; }
    int    companyDays() const;            // 陪伴天数（含今天）
    int    totalInteractions() const { return m_totalInteractions; }

    // ---------- 加分 ----------
    // 返回值 = 这次真正加到的分。0 表示被额度/冷却挡掉了 —— 调用方靠它决定
    // "要不要给反应"（例如额度用完就不用再播开心了）。
    double addFromPet();                   // 摸摸：每天 AFF_PET_PER_DAY 次 + 3 秒冷却
    double addFromFeed();                  // 喂食：每天 AFF_FEED_PER_DAY 次
    double addSource(Source s, double amount, int cooldownMs = 0);

    void resetAll();                       // 清空重来（留给以后的重置按钮用）

    // ---------- 陪伴开关 ----------
    // 藏进状态栏时要关掉：人都看不见了，不该继续算"陪伴"。
    void setCompanyActive(bool on) { m_companyActive = on; }
    bool companyActive() const { return m_companyActive; }

    // ---------- 存档 ----------
    QString savePath() const;
    void    save() const;
    void    load();

    // ---------- 给 --selftest / --affectiontrace 用 ----------
    void   debugStopTimer();               // 停掉自动计时器，让轨迹完全由调用方控制
    // 时钟往前拨 ms（按小时切片推进），返回实际扣掉的点。
    // running = false 表示这段时间"程序是关着的" —— 时间照走，但什么都不结算，
    // 这正是运行时的真实行为（关掉程序就没有任何东西在算），轨迹里必须能表达出来。
    double debugAdvanceTime(qint64 ms, bool running = true);
    // 模拟"定时器晚到了 dtMs"：一步就是一步，走的是 onTimer() 那条真实路径
    // （含时间窗钳位）。用它才能验证"电脑睡了一觉回来不会被暴扣"。
    double debugLateTick(qint64 dtMs);
    QString describe() const;              // 多行中文报告

signals:
    void changed();                        // 数字有任何变化（面板据此刷新）
    void leveledUp(int newLevel);
    void stageChanged(int stageIndex);

private:
    // 内部时钟。正常运行 m_timeOffsetMs 恒为 0
    qint64 nowMs() const { return m_clock.elapsed() + m_timeOffsetMs; }
    QDate  today() const;

    void   checkNewDay();                            // 跨天就把今日额度清零
    double gain(double amount, bool countInteraction);
    double settle(qint64 dtMs, bool clampWindow);    // 结算一段时间的"陪伴 + 衰减"
    void   onTimer();

    // ---------- 数值 ----------
    double m_point      = 0.0;   // 好感度总点数（小数：衰减是连续的）
    double m_todayGain  = 0.0;   // 今日加分（不含衰减）
    double m_todayDecay = 0.0;   // 今日被衰减吃掉的
    int    m_totalInteractions = 0;

    // ---------- 每日额度 ----------
    QDate m_day;                 // 这些计数属于哪一天
    int   m_petUsed  = 0;        //摸摸
    int   m_feedUsed = 0;        //喂食
    int   m_chatUsed = 0;        //聊天
    double m_todayCompany = 0.0;

    // ---------- 冷却（只在本次运行内有效，不需要存档）----------
    // AFF_NO_COOLDOWN（-1）= 从没触发过。别用 0，理由见 PetConfig.h 里那段说明。
    qint64 m_lastPetMs  = PetCfg::AFF_NO_COOLDOWN;
    qint64 m_lastDragMs = PetCfg::AFF_NO_COOLDOWN;
    qint64 m_lastMenuMs = PetCfg::AFF_NO_COOLDOWN;

    // ---------- 陪伴累计 ----------
    bool   m_companyActive = true;   // 程序开着且桌宠没被收进状态栏
    qint64 m_companyAccMs  = 0;      // 凑够 AFF_COMPANY_EVERY_MS 就 +1

    // ---------- 首次使用 ----------
    QDate m_firstDay;

    // ---------- 时间 / 计时 ----------
    QElapsedTimer m_clock;
    qint64 m_timeOffsetMs = 0;       // 偏移（模拟时间用）
    qint64 m_lastSettleMs = 0;       // 上次结算到哪个时刻
    QTimer* m_timer = nullptr;
    bool    m_persistent = true;
};
