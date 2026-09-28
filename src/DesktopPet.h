#pragma once
// =============================================================================
//  DesktopPet —— 桌宠主控制器（同时也是那个透明窗口本身）
//
//  它直接继承 QWidget，所以"窗口"就是"桌宠"。职责：
//    1. 透明无边框窗口的创建（无边框 / 背景全透明 / 置顶 / 不进任务栏）
//    2. 把 AnimationController 给的当前帧画出来
//    3. 鼠标事件：区分「点击」和「拖动」、右键菜单
//    4. 桌宠在屏幕上的位置、自动移动、屏幕边界、多显示器
//    5. 状态切换的调度中心：把"动画帧"和"位置移动"协调起来
//
//  它不负责：决定什么时候该做什么（那是 PetBehaviorController）、
//            也不知道某一张图叫什么（那是 AnimationController）。
//
//  ★ 整个程序没有一行 while 循环 ★
//    一切靠两类事件驱动：
//      · m_tick 这个 60Hz 心跳定时器  -> 推进位置、边界、保持时长
//      · AnimationController 的帧定时器 -> 推进动画帧
//    两者互相独立，各管各的，这样"移动"和"动画"不会互相拖累。
// =============================================================================

#include <QWidget>
#include <QPixmap>
#include <QPointF>
#include <QElapsedTimer>
#include <QVector>

#include "PetConfig.h"
#include "PetSay.h"            // 桌宠独白台词池（Picker 以值成员持有，要完整类型）

class QTimer;
class QMenu;
class QAction;
class QSystemTrayIcon;
class AnimationController;
class PetBehaviorController;
class AffectionSystem;
class MainPanel;
class ChatPage;
class PlayerPage;
class AffectionPage;
class SettingsPage;
class DailyImagePage;
class PetBubble;               // 头顶台词气泡（独立置顶小窗，指针持有）
class PetFx;                   // 粒子特效层（爱心/Zzz，独立置顶小窗，指针持有）

class DesktopPet : public QWidget
{
    Q_OBJECT
public:
    explicit DesktopPet(QWidget* parent = nullptr);

    // 析构时先把托盘图标撤掉。Windows 上如果程序退出时图标还挂在状态栏里，
    // 那个图标会变成"幽灵"——位置留着、鼠标划过去才消失。手动收一下最稳。
    ~DesktopPet() override;

    // 显示桌宠 + 启动心跳 + 启动自动行为。
    // 注意：构造函数里不做这些，因为要先把图片加载完才知道窗口该多大。
    void start();

    // -----------------------------------------------------------------------
    //  【只给 --selftest 用】强制切到某个状态并把窗口摆好，方便截图。
    //
    //  正式运行路径永远不会调它：它绕过自动行为控制器和动作链，
    //  不改变位置、不启动定时器，纯粹是"让桌宠摆出某个姿势让我拍一张"。
    //
    //  加它的唯一目的，是让自检能把"拖动时那张图到底长什么样"也拍下来。
    //  拖拽图只在按住鼠标拖动的那一瞬间出现，用普通截图工具根本抓不到
    //  （本机对 WS_EX_LAYERED 窗口的屏幕截取还会返回全黑），
    //  所以干脆从内部直接切过去抓一张。
    // -----------------------------------------------------------------------
    void debugSetState(PetState s);

    // 【只给 --selftest 用】让气泡真的冒一句话，再把气泡窗离屏抓成图。
    // 气泡是独立小窗，桌宠自己的 grab() 拍不到它；拍它自己才能一眼看出
    // "文字有没有被截、尺寸算得对不对、主题色上没上"。
    QPixmap debugGrabBubble(const QString& text);

    // 【只给 --selftest 用】让特效层撒满爱心粒子，再把特效窗离屏抓成图。
    // 心形形状/颜色/淡出对不对，看图就知道。
    QPixmap debugGrabFx();

    // 应用头像图标：站立图(08)裁出脸部，多尺寸生成（任务栏/Alt-Tab/窗口通用）。
    // main() 在建任何窗口前把它设成全局图标；本窗口自己也设一份（Alt-Tab）。
    static QIcon appAvatarIcon();

    // 托盘状态描述，给 --selftest 用。
    // 托盘是个"看不见的功能"——出问题时用户只会觉得"点了隐藏就再也叫不回来"，
    // 所以把可用性、图标有没有建起来、菜单项是否启用都写进自检报告，排错第一步就能看到。
    QString describeTray() const;

    // 好感度的状态描述，给 --selftest 用（点数、等级、今日额度、存档路径）。
    QString describeAffection() const;

    // 好感度数据对象。面板页拿它读数字，--affectiontrace 之外没人会改它。
    AffectionSystem* affection() const { return m_affection; }

    // 「桌宠互动」开关（设置页 → 这里）。改内存 + 落盘（键在 ui.ini），
    // 关掉时正在显示的气泡也会收起来。
    void setPetSayEnabled(bool on);

    // 「桌宠大小」滑条（设置页 → 这里）：50%~200%，按脚底锚点整体缩放。
    // 改内存 + 落盘（ui.ini petScale/percent），立即 resize 生效。
    void setPetScale(int percent);

    // 【只给 --selftest 用】真的跑一遍"隐藏 -> 恢复"往返，把每一步的窗口可见性、
    // 心跳开关、自动行为开关都记下来。
    //
    // 为什么需要它：点托盘图标这件事没法自动模拟（PostMessage 对 Qt6 不生效，
    // SendInput 又会真抢用户的鼠标），但"隐藏/恢复有没有把状态收干净"是能直接查的 ——
    // 时钟基准、心跳定时器、自动行为开关这三样只要漏一个，就会出现
    // "回来以后顿一下""回来不走路了""明明暂停了却自己动起来"这类怪现象。
    QString debugTrayRoundTrip();

    // 【只给 --selftest 用】真实投递 SC_MINIMIZE / SC_RESTORE（Shell 点任务栏图标
    // 发的就是这两条消息），验证面板补的 WS_MINIMIZEBOX 在 show() 之后仍然在位、
    // 任务栏"点一下最小化、再点一下复原"的原生切换对无边框面板真的生效。
    // ★ 只管面板自己 ★ 桌宠是独立窗口，不参与这条链（用户明确要求，2026-09-28）。
    QString debugMinimizeRoundTrip();

    // 【只给 --selftest 用】读 Windows **实际**的 WS_EX_TOPMOST 位。
    //
    // 为什么不能拿 windowFlags().testFlag(Qt::WindowStaysOnTopHint) 代替：
    // 那个读的是 Qt 自己记着的"意图"，和系统窗口上真实的那一位**可以不一致** ——
    // 这个 bug 的症状恰好就是"Qt 还说自己是置顶，系统那一位早被摘了"。
    // 非 Windows 平台上恒返回 true（那边没有这种问题）。
    bool reallyOnTop() const;

signals:
    // 「和我聊天」的接口。
    // 第一阶段只把信号发出去，不接 AI。以后接本地 Qwen 时，
    // 在主程序里 connect 这个信号，拿到回答后再决定播 09 开心 / 10 委屈 / 11 惊讶 / 12 生气。
    void chatRequested();

    // 用户把音乐文件拖到了桌宠身上（路径已按扩展名过滤）。
    // 播放器页接它：入列并从第一首开始播。
    void musicFilesDropped(const QStringList& paths);

protected:
    // 每次窗口重新变得可见（启动、从状态栏恢复、被系统重新显示）都会走这里。
    // 它只做一件事：把"始终置顶"重新钉一遍（原因见 .cpp 里 ensureOnTop 的说明）。
    void showEvent(QShowEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    // 外部文件拖入（资源管理器拖歌到桌宠身上）。
    // 和"鼠标拖自己"互不打扰：拖放期间鼠标事件根本不会发给本窗口。
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    void onTick();                                       // 60Hz 心跳
    void onFrameChanged();                               // 动画换帧 -> 重绘
    void onAnimationStateChanged(PetState now, PetState before);  // 换状态 -> 调整窗口大小
    void onActionFinished(PetState finished);            // 一次性动作播完 -> 决定后续
    void onBehaviorState(PetState s, int holdMs);        // 行为控制器要求切换状态
    void onBehaviorWalk(int direction, int durationMs);  // 行为控制器要求走一段路

    // 「和我聊天」的入口：菜单项和面板按钮都走这里 ——
    // 先记一次好感度，再把信号发出去（发出去之后就不归它管了，接 AI 在主程序里）。
    void noteChat();

private:
    // ---------- 窗口 ----------
    void  setupWindow();
    // 把 Qt::WindowStaysOnTopHint 对应到系统上的 WS_EX_TOPMOST 位**重新钉一遍**。
    // 为什么需要它：Windows 会在 Qt 不知情的时候把这一位摘掉（隐藏后再显示、
    // 别的顶层窗口开开关关），摘掉之后桌宠就会被普通窗口盖住。详见 .cpp。
    void  ensureOnTop();
    void  resizeWindowForState(PetState s);
    QPoint anchorScreenPos() const;                      // 角色脚底中心在屏幕上的位置
    QPoint frameTopLeft(const QPixmap& pm) const;        // 当前帧在窗口里的左上角
    QRect  availableScreenRect() const;                  // 桌宠所在屏幕的可用区域
    double groundLineY() const;                          // 窗口底边能到的最低 y（"地面线"）

    // ---------- 状态 ----------
    void applyState(PetState s, int holdMs);
    void goIdle();
    void runChain(const QVector<PetState>& chain);
    void playNextInChain();
    void onPetClicked();

    // ---------- 桌宠说话 ----------
    // 全部的门禁都在这一道：开关 / 藏进托盘 / 气泡正忙 / PetSay 内部节流。
    // 挑不到句子就安静 —— 宁可少说，不能刷屏。arg 填 %1（歌名等）。
    // force = true：无视"气泡正忙"和节流（连点彩蛋的爆发台词用）。
    void petSay(PetSay::Event ev, const QString& arg = QString(),
                qint64 minGapMs = 5000, bool force = false);
    void petSayHourly();           // 整点报时（%1 = 小时数，深夜自动换劝睡池）

    // ---------- 移动 ----------
    void startWalk(int direction, int durationMs);
    void startFly(int direction, int durationMs);
    void startFall();              // 松手：开始自由落体（切到下落实，垂直速度从 0 起）
    void landFromFall();           // 落地：垂直速度归零、位置钉在地面、切回站立
    bool isFalling() const;        // 是否正在下落（点击和自动行为都要给它让路）
    // snapToGround = true（默认）：如果正在下落，先把窗口压到地面再停。
    // 这是为"点击/右键菜单"这类打断兜底，免得它悬在半空。
    // 空中用鼠标抓住它时要传 false —— 那种场景希望位置原样定住，不要掉下去再弹回来。
    void stopMoving(bool snapToGround = true);
    void beginWalkStop();          // 走路收尾：停步前播 28 -> 29 两帧
    void turnTo(int newDir);
    void onHitEdge();
    void tickMovement(double dt);
    // 把窗口挪到 m_pos 对应的整数位置（位置取整后没变就什么都不做）。
    // 为什么要有它：见 .cpp 里的说明 —— 分层窗口的每一次 move() 都要整块重合成。
    void applyWindowPos();
    // allowTurn = false 时不触发"撞边掉头"。
    // 调整窗口大小、拖动这类操作也会调用钳位，但它们不该让桌宠转身，
    // 而且禁掉转身还能避免"尺寸变化 -> 钳位 -> 转身 -> 状态变化 -> 再改尺寸"这种重入。
    void clampPosition(bool allowTurn = true);
    void clampToScreen(bool allowTurn = true);
    void syncPosFromWindow();

    // ---------- 输入 ----------
    bool hitCharacter(const QPoint& localPos) const;
    void buildMenu();
    void refreshMenuState();

    // ---------- 系统托盘（隐藏到右下角状态栏）----------
    void setupTray();          // 建托盘图标 + 托盘菜单（构造时调一次）
    void hideToTray();         // 藏起来：先回站立 -> 停心跳和自动行为 -> hide -> 亮出托盘图标
    void restoreFromTray();    // 恢复：撤托盘图标 -> show -> 重启心跳 -> 按原来的开关恢复自动行为
    bool trayReady() const { return m_tray != nullptr; }   // 本机有没有可用的系统托盘

    // ---------- 主面板 ----------
    // 面板是"第一次点开才建"的（惰性）。理由有两个：
    //   · 不用面板的用户，程序启动时不该多花时间搭一堆看不见的控件；
    //   · 藏进状态栏的时候，面板如果早就存在，还得额外记得把它藏起来。
    // 建好之后就一直留着（关闭只是 hide，不销毁），所以再点开是"秒开"、状态不丢。
    // 打开主面板。showPanel = false 时只**创建**不显示 —— 拖放音乐到桌宠
    // 身上走这条路：播放器在面板里，建好了才能播，但面板不该弹出来。
    void openPanel(bool showPanel = true);
    void closePanel();         // 藏进状态栏时要顺手把面板收起来

    // ---------- 数据 ----------
    AnimationController*   m_anim = nullptr;       // 动画控制器
    PetBehaviorController* m_behavior = nullptr;   // 自动行为控制器
    AffectionSystem*       m_affection = nullptr;  // 好感度（跨面板/点击/菜单共用同一份）

    QTimer*       m_tick = nullptr;                // 60Hz 心跳
    QElapsedTimer m_clock;                         // 高精度单调时钟
    qint64        m_lastNs = 0;                    // 上次心跳的纳秒读数

    // 拖动状态
    bool   m_pressed = false;
    bool   m_dragging = false;
    QPoint m_pressGlobal;      // 按下时鼠标在屏幕上的位置
    QPoint m_pressTopLeft;     // 按下时窗口左上角的位置

    // 移动状态
    //   Fall 和另外三个不太一样：它不由"时长"结束，而是由"碰到地面"结束。
    enum class Move { None, Walk, Fly, Fall };
    Move    m_move = Move::None;
    int     m_moveDir = 1;         // +1 向右 / -1 向左
    int     m_moveMsLeft = 0;      // 本次移动还剩多少毫秒
    double  m_velY = 0.0;          // 垂直速度（px/s，向下为正），只有下落时用得到
    // 收步阶段：走路时间快用完时不再前进，改成播「收步」两帧，等它播完才回待机。
    // 收步期间还会以线性衰减的速度往前滑几像素，避免"啪"一下刹停。
    bool    m_walkStopping = false;
    int     m_stopMsLeft = 0;      // 收步还剩多少毫秒（同时也是速度衰减系数）
    QPointF m_pos;                 // 精确位置（保留小数，避免慢速移动被取整吃掉）
    int     m_flyBaseY = 0;        // 飞行时的基准高度
    double  m_flyPhase = 0.0;      // 飞行上下浮动的相位

    int     m_holdMsLeft = 0;            // 当前状态还要保持多久（0 = 不定时）
    QVector<PetState> m_chain;           // 待播放的动作链

    bool    m_inResize = false;          // 防止 resize 过程里递归

    QMenu*   m_menu = nullptr;
    QAction* m_actPause = nullptr;
    QAction* m_actResume = nullptr;
    QAction* m_actHide = nullptr;

    // ---------- 系统托盘 ----------
    QSystemTrayIcon* m_tray = nullptr;         // nullptr = 本机没有可用托盘，"隐藏到状态栏"会被禁用
    QMenu*           m_trayMenu = nullptr;     // 在状态栏图标上右键弹出的那个小面板
    bool             m_hiddenToTray = false;   // 现在是不是"已经藏进状态栏"的状态
    bool             m_autoBeforeHide = true;  // 藏起来之前，自动行为是开着还是关着

    // ---------- 主面板 ----------
    // 注意 m_panel 是顶层窗口（没有 parent），所以必须在本类析构时手动 delete，
    // 否则它会活到进程结束才被系统回收。
    MainPanel*     m_panel     = nullptr;
    AffectionPage* m_affPage   = nullptr;
    DailyImagePage* m_dailyPage = nullptr;
    ChatPage*      m_chatPage  = nullptr;
    PlayerPage*    m_playerPage = nullptr;
    SettingsPage*  m_setPage   = nullptr;

    // ---------- 桌宠说话 ----------
    PetBubble*     m_bubble        = nullptr;   // 头顶气泡（独立置顶小窗，常驻隐藏）
    PetSay::Picker m_sayPicker;                 // 节流 + 避重都在它里面
    bool           m_petSayEnabled = false;     // 设置页开关（ui.ini petSay/enabled）
    int            m_lastHour      = -1;        // 整点报时用（ctor 里初始化为当前小时）
    QTimer*        m_chatterTimer  = nullptr;   // 随机闲聊的节拍器（60s 一看，看缘分开口）

    // ---------- 粒子特效 / 连点彩蛋 ----------
    PetFx*        m_fx        = nullptr;       // 爱心/Zzz 特效层（独立置顶小窗）
    int           m_pokeCount = 0;             // 连点计数（1.5s 滚动窗口内有效）
    QElapsedTimer m_pokeClock;                 // 距上次点击的计时

    // ---------- 桌宠大小 ----------
    // 缩放系数（0.5~2.0，1.0 = 原始像素）。吃它的只有四个函数：
    // resizeWindowForState（窗口尺寸）/ paintEvent（画大画小）/
    // frameTopLeft（底边对齐按缩放后尺寸算）/ hitCharacter（采样缩放后的图）——
    // 地面线、锚点、拖拽、气泡跟随全部经窗口几何自动成立，不用各自适配。
    double m_petScale = 1.0;

    // 返回"按当前系数缩放后的当前帧"。scale == 1 时原样返回，零开销。
    QPixmap scaledFrame(const QPixmap& pm) const;
};
