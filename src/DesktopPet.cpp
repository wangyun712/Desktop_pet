#include "DesktopPet.h"
#include "AnimationController.h"
#include "PetBehaviorController.h"
#include "AffectionSystem.h"
#include "MainPanel.h"
#include "AffectionPage.h"
#include "SettingsPage.h"
#include "DailyImagePage.h"
#include "ChatPage.h"

#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QTimer>
#include <QPainter>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QAction>
#include <QSystemTrayIcon>
#include <QIcon>
#include <QImage>
#include <QVBoxLayout>
#include <QLabel>
#include <QRandomGenerator>
#include <QtMath>
#include <QDebug>

// =============================================================================
//  构造：只做"搭建"，不做"启动"
// =============================================================================
DesktopPet::DesktopPet(QWidget* parent) : QWidget(parent)
{
    setupWindow();

    // 两个控制器都由本窗口持有。parent = this 的好处：
    // 窗口销毁时它们会被自动 delete，不用手写 delete，也不会内存泄漏。
    m_anim = new AnimationController(this);
    m_behavior = new PetBehaviorController(this);

    // ---- 动画控制器 -> 窗口 ----
    // 换帧了就重绘；换状态了要按新图片尺寸调整窗口大小和后续动作
    connect(m_anim, &AnimationController::frameChanged, this, &DesktopPet::onFrameChanged);
    connect(m_anim, &AnimationController::stateChanged, this, &DesktopPet::onAnimationStateChanged);
    connect(m_anim, &AnimationController::actionFinished, this, &DesktopPet::onActionFinished);

    // ---- 行为控制器 -> 窗口 ----
    connect(m_behavior, &PetBehaviorController::requestState, this, &DesktopPet::onBehaviorState);
    connect(m_behavior, &PetBehaviorController::requestWalk, this, &DesktopPet::onBehaviorWalk);

    // ---- 心跳定时器 ----
    m_tick = new QTimer(this);
    m_tick->setTimerType(Qt::PreciseTimer);   // 精确计时，移动更顺
    connect(m_tick, &QTimer::timeout, this, &DesktopPet::onTick);

    buildMenu();
    setupTray();   // 建好托盘图标和菜单，但先不显示 —— 藏进状态栏那一刻才亮出来

    // ---- 好感度 ----
    // 它是一个独立的数据对象（不带界面），到处都要用：点击、拖动、右键菜单、面板。
    // 挂在本窗口下面是为了让它随窗口一起销毁，不用手写 delete。
    m_affection = new AffectionSystem(/*persistent=*/true, this);

    // 升级时给个看得见的反馈：桌宠自己开心一下。
    // 两个"不行"要挡住 —— 正在被拖着、正在下落的时候切表情，
    // 会把它从"被拎着"的状态里拽出来，画面会跳。
    connect(m_affection, &AffectionSystem::leveledUp, this, [this](int) {
        if (m_dragging || isFalling())
            return;
        runChain(QVector<PetState>{ PetState::Happy });
    });

    // 菜单项的可用状态先刷一次。平时这一步由 contextMenuEvent 负责，
    // 但"隐藏到状态栏"要靠它根据托盘是否可用来禁用，自检报告也读这个结果。
    refreshMenuState();
}

DesktopPet::~DesktopPet()
{
    // 程序退出时把托盘图标撤掉。窗口销毁时 QSystemTrayIcon 虽然也会跟着销毁，
    // 但先 hide() 一次能让 Windows 立刻把状态栏上那个图标摘干净，不留"幽灵图标"。
    if (m_tray)
        m_tray->hide();

    // 主面板是顶层窗口（没有 parent），QWidget 的父子自动回收管不到它，必须手动收。
    // 好感度不在这里 delete —— 它的 parent 是 this，跟着一起走。
    delete m_panel;
    m_panel = nullptr;
}

// =============================================================================
//  窗口设置：透明、无边框、置顶、不进任务栏
//
//  「PNG 之外不能出现白底/黑底」全靠下面这几个标志 + 属性一起作用：
//    · FramelessWindowHint      去掉标题栏和边框，只剩客户区
//    · WA_TranslucentBackground 让窗口的画布带 alpha 通道，透明处透出桌面
//    · WA_NoSystemBackground    不让系统先刷一层不透明底色
//    · NoDropShadowWindowHint   关掉系统投影（否则透明窗口边缘会有一圈灰影）
//  最后 paintEvent 里把整块区域刷成 Qt::transparent，白色背景就彻底没有了。
// =============================================================================
void DesktopPet::setupWindow()
{
    setWindowFlags(Qt::FramelessWindowHint          // 无边框
                 | Qt::WindowStaysOnTopHint         // 始终保持在其他普通窗口之上
                 | Qt::Tool                        // 工具窗口：任务栏不显示按钮、不进 Alt+Tab
                 | Qt::NoDropShadowWindowHint       // 关掉系统投影
                 | Qt::WindowDoesNotAcceptFocus);   // 不抢键盘焦点，不打断用户打字

    setAttribute(Qt::WA_TranslucentBackground, true);   // 画布带 alpha 通道
    setAttribute(Qt::WA_NoSystemBackground, true);      // 不刷系统底色
    setAttribute(Qt::WA_ShowWithoutActivating, true);   // show() 时不激活到前台
    setAttribute(Qt::WA_OpaquePaintEvent, false);       // 明确告诉 Qt：这里要画半透明

    setWindowTitle(QStringLiteral("PetPal 桌宠"));

    // 不拖动时不需要跟踪鼠标移动，省一点开销
    setMouseTracking(false);

    // 先给一个占位尺寸，真正的尺寸在第一次拿到帧图后由 resizeWindowForState 算出来
    resize(1, 1);
}

// =============================================================================
//  自检专用：强制切到某个状态（详细说明见头文件）
//
//  为什么不直接调 applyState()？
//  因为 applyState() 会带"保持多久"、会参与动作链、还会判断当前是不是
//  一次性动作。自检只想"摆好姿势拍张照"，所以要一个不带任何副作用的入口：
//  清掉待播动作链和定时器，直接换图，再让窗口按新状态的尺寸重排一次。
// =============================================================================
void DesktopPet::debugSetState(PetState s)
{
    m_holdMsLeft   = 0;           // 不要"保持一会儿再回待机"那一套
    m_chain.clear();              // 不要动作链
    m_move         = Move::None;  // 不要移动
    m_walkStopping = false;
    m_dragging     = false;

    m_anim->setState(s, true);    // force = true：不管当前在播什么都换过去
    resizeWindowForState(s);      // 状态没变时 setState 不发信号，这里补一次保险
    update();
}

// =============================================================================
//  启动：加载图片 -> 定尺寸 -> 摆位置 -> 显示 -> 开心跳 -> 开自动行为
// =============================================================================
void DesktopPet::start()
{
    // 1) 加载全部 PNG（内部会为每个状态组装好帧序列，含水平翻转版本）
    if (!m_anim->loadAll())
    {
        const QStringList miss = m_anim->missingResources();
        qWarning() << "[PetPal] 有" << miss.size() << "张图片没加载成功:";
        for (const QString& p : miss)
            qWarning() << "        缺失 ->" << p;
        // 这里故意不退出：窗口会画一个提示框，
        // 让用户看到"是资源问题"而不是程序一闪就没了。
    }

    // 2) 按站立图的尺寸决定窗口大小。
    //    要放在 show() 之前，否则会先闪一下 1x1 的小窗。
    resizeWindowForState(PetState::Idle);

    // 3) 摆到屏幕右下角附近再显示
    const QRect r = availableScreenRect();
    const int marginX = qMin(160, r.width() / 4);
    const int marginY = qMin(80, r.height() / 6);
    m_pos = QPointF(r.right() - width() - marginX,
                    r.bottom() - height() - marginY);
    clampToScreen();

    // 4) 显示。程序没有主窗口，show() 就等于"启动后直接显示桌宠"
    show();

    // 5) 启动心跳。
    //    顺序很重要：先 start() 把计数器归零，紧接着读一次当基准，
    //    否则第一次心跳算出的 dt 会是"从开机到现在"的一大坨时间。
    m_clock.start();
    m_lastNs = m_clock.nsecsElapsed();
    m_tick->start(PetCfg::TICK_MS);

    // 6) 交给行为控制器，让它开始随机安排动作
    m_behavior->start();
}

// =============================================================================
//  绘制：唯一能往窗口上画东西的地方
//
//  画法：底边对齐 + 水平居中，按原始像素尺寸画（不缩放）——所以图片不会变形。
//  为什么不是"把图片拉伸铺满窗口"？因为所有图尺寸都不一样（153x254 到 358x217；
//  拉伸会让角色一会儿胖一会儿瘦，那正是需求里明确禁止的。
// =============================================================================
void DesktopPet::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter p(this);

    // 第一步：把整块区域刷成完全透明。
    // 这一句不能省。窗口被移动或缩小后，旧像素会留在原地变成"拖影"。
    p.fillRect(rect(), Qt::transparent);

    // 第二步：画当前帧
    const QPixmap& pm = m_anim->currentPixmap();
    if (pm.isNull())
    {
        // 资源没加载成功时的兜底提示，避免用户对着一个"什么都不显示"的窗口猜问题
        p.setPen(QPen(QColor(200, 60, 60), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(rect().adjusted(0, 0, -1, -1));
        p.drawText(rect(), Qt::AlignCenter,
                   QStringLiteral("图片未加载\n运行 PetPal.exe --selftest\n查看报告"));
        return;
    }

    p.drawPixmap(frameTopLeft(pm), pm);
}

// =============================================================================
//  当前帧在窗口里的左上角坐标
// =============================================================================
QPoint DesktopPet::frameTopLeft(const QPixmap& pm) const
{
    const int x = (width() - pm.width()) / 2;                       // 水平居中
    const int y = height() - pm.height() + PetCfg::WINDOW_V_OFFSET; // 底边对齐
    return QPoint(x, y);
}

// =============================================================================
//  角色脚底中心在屏幕上的坐标（用来在窗口改变尺寸时保持画面不跳）
// =============================================================================
QPoint DesktopPet::anchorScreenPos() const
{
    return QPoint(x() + width() / 2, y() + height());
}

// =============================================================================
//  按新状态的图片尺寸调整窗口
//
//  为什么窗口要跟着状态变？
//    · 站立/行走的图大概 183x252，睡姿那张宽达 358 —— 如果一直用最大的 358，
//      走路时会有一大片看不见的窗口挡住桌面点击，撞到屏幕边也会提前停下。
//    · 所以按"当前状态里最大的那一帧"来开窗口，既够用又紧凑。
//
//  关键：改尺寸前后要让"脚底中心"保持不动，否则角色会在屏幕上跳一下。
// =============================================================================
void DesktopPet::resizeWindowForState(PetState s)
{
    if (m_inResize)
        return;

    const QSize box = m_anim->frameBoxSize(s);
    if (box.isEmpty())
        return;              // 资源缺失，保持原尺寸
    if (box == size())
        return;              // 尺寸没变，不折腾

    m_inResize = true;

    const QPoint anchor = anchorScreenPos();
    resize(box);

    m_pos = QPointF(anchor.x() - box.width() / 2.0,
                    anchor.y() - box.height());
    // 这里禁掉"撞边掉头"：窗口尺寸变了导致被钳位，不代表桌宠该转身
    clampToScreen(/*allowTurn=*/false);

    m_inResize = false;
}

// =============================================================================
//  桌宠所在屏幕的可用区域（会避开任务栏）
// =============================================================================
QRect DesktopPet::availableScreenRect() const
{
    // 取桌宠中心点落在哪块屏幕上。多显示器时它会跟着桌宠走，
    // 所以拖到副屏之后，就在副屏范围内自动行走。
    QScreen* scr = QGuiApplication::screenAt(frameGeometry().center());
    if (!scr)
        scr = QGuiApplication::primaryScreen();
    if (!scr)
        return QRect(0, 0, 1280, 720);

    // availableGeometry() 比 geometry() 小，因为它扣掉了任务栏
    return scr->availableGeometry();
}

// =============================================================================
//  "地面线"：窗口底边能到的最低 y
//
//  和 clampPosition 里那条 hiY 是同一个式子 —— 因为"把桌宠钳到底"走到极致，
//  得到的就是角色站着的那条线。下落结束时要把窗口精确停在这条线上，所以抽出来，
//  免得两处各写一遍、以后改一处漏一处。
// =============================================================================
double DesktopPet::groundLineY() const
{
    const int m = PetCfg::EDGE_MARGIN_PX;
    const QRect r = availableScreenRect().adjusted(m, m, -m, -m);
    const double loY = r.top();
    // qMax 兜底：屏幕比桌宠还矮的时候，别让上下界反过来
    return qMax(loY, double(r.bottom() - height() + 1));
}

// =============================================================================
//  心跳：整个程序的"逻辑时钟"
// =============================================================================
void DesktopPet::onTick()
{
    // 用实测时间代替定时器的名义间隔。
    // QTimer 的 16ms 只是"期望值"，系统调度会让它抖动，所以必须以实测为准，
    // 这样 位置 += 速度 * dt 才与帧率无关：不管实际跑 60fps 还是 30fps，速度都一样。
    const qint64 now = m_clock.nsecsElapsed();
    const double dt = double(now - m_lastNs) / 1.0e9;
    m_lastNs = now;

    // 丢弃"休眠假时间"。
    // 电脑睡眠、或者主线程被卡死几秒后，定时器不会补发遗漏的事件，
    // 醒来后第一次心跳测到的 dt 可能是几小时。这种值喂给移动逻辑，桌宠会瞬间飞出屏幕。
    if (dt <= 0.0 || dt > PetCfg::MAX_TICK_DT_S)
        return;

    tickMovement(dt);

    // 保持型状态（坐下、发呆）的倒计时，到点了就回待机
    if (m_holdMsLeft > 0)
    {
        m_holdMsLeft -= int(dt * 1000.0);
        if (m_holdMsLeft <= 0)
        {
            m_holdMsLeft = 0;
            goIdle();
        }
    }
}

// =============================================================================
//  移动：走路和飞行
//
//  ★ 这是"桌宠真的在走"而不是"原地播放动画"的关键 ★
//    每帧同时做两件事：
//      ① 动画层面：AnimationController 自己按 100ms 换一帧
//      ② 位置层面：这里把窗口 x 增加 速度 * dt
//    两件事互不干扰，所以看起来就是"边迈腿边前进"。
// =============================================================================
void DesktopPet::tickMovement(double dt)
{
    if (m_move == Move::None)
        return;

    // ---- 收步阶段：不再"迈新步"，只以线性衰减的速度往前滑一点点 ----
    // 剩下的收尾交给 onActionFinished：28/29 两帧播完会发 actionFinished，
    // 那时才真正停下来回站立。这里绝不能再自己 goIdle()，
    // 否则会把收步动画从中间掐断（走过去会看到"顿一下"）。
    if (m_walkStopping)
    {
        if (m_stopMsLeft > 0)
        {
            m_stopMsLeft -= int(dt * 1000.0);
            if (m_stopMsLeft < 0)
                m_stopMsLeft = 0;

            // 速度系数 k 从 1 线性衰减到 0：200ms 里一共只挪几像素，
            // 但比"啪一下刹停"自然得多。
            const double k = double(m_stopMsLeft) / double(PetCfg::WALK_STOP_MS);
            m_pos.setX(m_pos.x() + m_moveDir * PetCfg::WALK_SPEED_PPS * k * dt);

            clampPosition(/*allowTurn=*/false);   // 收步时撞到边也不转身，直接站住
            move(qRound(m_pos.x()), qRound(m_pos.y()));
        }
        return;
    }

    // ---- 自由落体 ----
    // 和上面收步、下面走路/飞行是并列的"运动模式"。
    // 放在转身判断之前：下落途中没有"转身"这个概念（那是走路才有的）。
    if (m_move == Move::Fall)
    {
        const double groundY = groundLineY();
        const PetFallStep s = petFallAdvance(m_pos.y(), m_velY, groundY, dt);

        m_velY = s.velY;
        m_pos.setY(s.y);

        move(qRound(m_pos.x()), qRound(m_pos.y()));   // 水平方向一动不动，纯垂直下落

        if (s.landed)
            landFromFall();
        return;
    }

    // 转身过程中原地不动（先转过来，再继续走），这样不会出现"侧着身子平移"
    const PetState st = m_anim->state();
    if (st == PetState::TurnLeft || st == PetState::TurnRight)
        return;

    if (m_move == Move::Walk)
    {
        m_pos.setX(m_pos.x() + m_moveDir * PetCfg::WALK_SPEED_PPS * dt);

        m_moveMsLeft -= int(dt * 1000.0);

        // ★ 该收步了 ★
        // 时间条件：这段路只剩「收步」那点时间了。
        // 画面条件：这一圈 (2)…(6) 必须正好走完。
        //   少了后半个条件，收步就会从半圈当中插进来 —— 画面从"正迈着腿"的 (4)
        //   直接跳到"双腿并拢"的 (7)，很生硬。等走到 (6) 再收，就是原始 1→8 的顺序，
        //   腿先迈出去、再收拢、最后站定。
        //   代价是这次走路最多会多走半圈（400ms），肉眼看不出来。
        if (m_moveMsLeft <= PetCfg::WALK_STOP_MS && m_anim->atLoopTail())
        {
            beginWalkStop();
            return;
        }
    }
    else if (m_move == Move::Fly)
    {
        // 飞行：水平照常前进，垂直叠加一个正弦浮动，看起来像在飘
        m_flyPhase += dt * PetCfg::FLY_BOB_HZ * 2.0 * M_PI;
        m_pos.setX(m_pos.x() + m_moveDir * PetCfg::FLY_SPEED_PPS * dt);
        m_pos.setY(m_flyBaseY + qSin(m_flyPhase) * PetCfg::FLY_BOB_PX);

        // 飞行是一趟飞完直接落地回待机，没有"收步"这回事
        m_moveMsLeft -= int(dt * 1000.0);
        if (m_moveMsLeft <= 0)
        {
            stopMoving();
            goIdle();
            return;
        }
    }

    // 位置是 double，只有真正调用 move() 时才取整。
    // 如果直接用 int 累加，60px/s 在 60Hz 下每帧只有 1 像素，误差会被吃掉导致"走不动"。
    clampPosition();
    move(qRound(m_pos.x()), qRound(m_pos.y()));
}

// =============================================================================
//  开始收步：切到「收步」状态，播 28 -> 29 两帧
//
//  走路是唯一有"结束姿态"的动作。别的动作时间到了直接切回站立就行，
//  但走路如果也这么干，就会在画面正迈着腿的时候"啪"地变成站立，很生硬。
//  所以留最后 200ms 播收步：腿先收拢，再站定。
// =============================================================================
void DesktopPet::beginWalkStop()
{
    m_walkStopping = true;
    m_moveMsLeft = 0;
    m_stopMsLeft = PetCfg::WALK_STOP_MS;

    m_anim->setState(m_moveDir > 0 ? PetState::WalkStopRight : PetState::WalkStopLeft,
                     /*force=*/true);
}

// =============================================================================
//  屏幕边界：把 m_pos 修正到当前屏幕的可用区域内
// =============================================================================
void DesktopPet::clampPosition(bool allowTurn)
{
    const int m = PetCfg::EDGE_MARGIN_PX;
    const QRect r = availableScreenRect().adjusted(m, m, -m, -m);

    const double loX = r.left();
    const double loY = r.top();
    // qMax 兜底：万一把桌宠拖到一块比它本身还小的屏幕上，也不至于算出负数范围
    const double hiX = qMax(loX, double(r.right()  - width()  + 1));
    const double hiY = qMax(loY, double(r.bottom() - height() + 1));

    if (m_pos.x() < loX)      { m_pos.setX(loX); if (allowTurn) onHitEdge(); }
    else if (m_pos.x() > hiX) { m_pos.setX(hiX); if (allowTurn) onHitEdge(); }

    if (m_pos.y() < loY)      m_pos.setY(loY);
    else if (m_pos.y() > hiY) m_pos.setY(hiY);
}

void DesktopPet::clampToScreen(bool allowTurn)
{
    clampPosition(allowTurn);
    move(qRound(m_pos.x()), qRound(m_pos.y()));
}

void DesktopPet::syncPosFromWindow()
{
    m_pos = QPointF(x(), y());
}

// =============================================================================
//  撞到屏幕边缘：自动改变方向
// =============================================================================
void DesktopPet::onHitEdge()
{
    if (m_move == Move::None)
        return;

    // 已经在转身就不重复触发（否则会一直重启动画，卡在背面那一帧）
    const PetState st = m_anim->state();
    if (st == PetState::TurnLeft || st == PetState::TurnRight)
        return;

    turnTo(-m_moveDir);
}

// =============================================================================
//  转身
//
//  需求里提到用 04 背面图做转身。这里做得更完整一点：
//    TurnRight = 04 背面(镜像) -> 05 朝右侧身
//    TurnLeft  = 04 背面      -> 03 朝左侧身
//  顺序是"先背对观众，再转向新方向"，看起来就是真的转了个身，
//  而不是把一张背面图硬翻转了事。
// =============================================================================
void DesktopPet::turnTo(int newDir)
{
    m_moveDir = newDir;
    m_holdMsLeft = 0;
    m_anim->setState(newDir > 0 ? PetState::TurnRight : PetState::TurnLeft, true);
}

// =============================================================================
//  开始走路
//
//  一次走路的完整过程（也就是玩家看到的画面）：
//      （1）起步一帧  ->  (2)(3)(4)(5)(6) 反复播  ->  (7)(8) 收步  ->  站立
//  起步在哪播？由 AnimationController 的状态切换带出来 —— 切到 WalkRight 时
//  帧号从 0 开始，第 0 帧正好是起步帧；之后循环绕回时会跳过它（见 petStateLoopBegin）。
//  收步在哪播？由本文件的 beginWalkStop() 在这段路快走完时切过去。
// =============================================================================
void DesktopPet::startWalk(int direction, int durationMs)
{
    m_holdMsLeft = 0;
    m_chain.clear();
    syncPosFromWindow();

    const bool wasMoving = (m_move != Move::None);

    m_move = Move::Walk;
    // 至少走够「一个循环 + 收步」，否则会刚起步就进收步，看着像抽搐
    m_moveMsLeft = qMax(qMax(1, durationMs), PetCfg::WALK_SHORTEST_MS);
    m_walkStopping = false;
    m_stopMsLeft = 0;

    // 已经在走/飞，而且方向反了：先播一个转身动作（播完会自动继续走）
    if (wasMoving && direction != m_moveDir)
    {
        m_moveDir = direction;
        turnTo(direction);
        return;
    }

    m_moveDir = direction;
    m_anim->setState(direction > 0 ? PetState::WalkRight : PetState::WalkLeft, true);
}

// =============================================================================
//  开始飞行
// =============================================================================
void DesktopPet::startFly(int direction, int durationMs)
{
    m_holdMsLeft = 0;
    m_chain.clear();
    syncPosFromWindow();

    m_moveDir = direction;
    m_move = Move::Fly;
    m_moveMsLeft = qMax(1, durationMs);
    // 起飞时清掉走路留下的收步标记，否则会出现"一边飞一边收步"的怪状态
    m_walkStopping = false;
    m_stopMsLeft = 0;

    // 记下起飞时的高度，飞行结束要还原回去，否则会"停在半空"
    m_flyBaseY = qRound(m_pos.y());
    m_flyPhase = 0.0;

    m_anim->setState(PetState::Fly, true);
}

// =============================================================================
//  开始下落（松开鼠标之后）
//
//  松手那一刻垂直速度是 0，之后每帧 v += g*dt、y += v*dt —— 真正的自由落体，
//  越掉越快。水平位置完全不动，所以是"垂直掉下去"，不是"斜着飘出去"。
//
//  为什么不做成"松手直接切回站立"？因为拖到半空再松手时，角色会从半空瞬移回地面，
//  那是纯粹的 bug 观感。有了重力，它才是"掉下来"。
// =============================================================================
void DesktopPet::startFall()
{
    syncPosFromWindow();
    m_holdMsLeft = 0;
    m_chain.clear();
    m_walkStopping = false;
    m_stopMsLeft = 0;
    m_moveMsLeft = 0;

    // 已经在最底下了（比如贴着任务栏松手）就没得掉，直接站定。
    // 不判这一下的话会先闪一帧下落图再落地，虽然只有 16ms，但没必要。
    if (m_pos.y() >= groundLineY() - PetCfg::FALL_LAND_EPS)
    {
        goIdle();
        return;
    }

    m_move = Move::Fall;
    m_velY = 0.0;

    // p31 的画布和 p30 逐像素同规格，所以这一句不会触发窗口 resize —— 换图不抖。
    m_anim->setState(PetState::Fall, true);
}

bool DesktopPet::isFalling() const
{
    return m_move == Move::Fall;
}

// =============================================================================
//  落地
//
//  ★ 顺序很重要 ★
//    先把窗口底边压到地面，再切 Idle。
//    切 Idle 会把窗口从 264 高收到 252 高，而 resizeWindowForState 锁的是
//    "底边中心"锚点 —— 底边不动，于是角色脚正好落在地面线上，
//    既不会陷进任务栏，也不会悬空 12px。
// =============================================================================
void DesktopPet::landFromFall()
{
    m_velY = 0.0;
    m_move = Move::None;
    clampToScreen(/*allowTurn=*/false);
    m_anim->setState(PetState::Idle, true);
}

// =============================================================================
//  停止移动
// =============================================================================
void DesktopPet::stopMoving(bool snapToGround)
{
    if (m_move == Move::Fly)
    {
        // 落地：把高度还原
        m_pos.setY(m_flyBaseY);
        move(qRound(m_pos.x()), qRound(m_pos.y()));
    }

    // 下落途中被别的事情打断（点击、右键菜单里的动作）：绝不能让它悬在半空，
    // 先把窗口压到地面再停。正常路径下走不到这里 —— 下落期间的点击和自动行为
    // 都被挡掉了（见 onPetClicked / onBehaviorState），这条只是兜底。
    if (m_move == Move::Fall && snapToGround)
        clampToScreen(/*allowTurn=*/false);

    m_move = Move::None;
    m_moveMsLeft = 0;
    m_flyPhase = 0.0;
    m_velY = 0.0;

    // 收步标记必须跟着一起清掉。
    // 忘了清就会出现"卡在收步里出不来"的僵状态：m_walkStopping 永远是 true，
    // tickMovement 每帧都直接 return，桌宠再也走不动。
    m_walkStopping = false;
    m_stopMsLeft = 0;
}

// =============================================================================
//  统一的状态切换入口
//  holdMs > 0 表示"这个状态保持这么久，然后自动回待机"
// =============================================================================
void DesktopPet::applyState(PetState s, int holdMs)
{
    // 除了"走"和"飞"，其它状态都意味着先停下来
    if (s != PetState::WalkRight && s != PetState::WalkLeft && s != PetState::Fly)
        stopMoving();

    m_holdMsLeft = qMax(0, holdMs);
    m_anim->setState(s, true);
}

// =============================================================================
//  回到待机：所有动作的终点都是这里（显示 08 站立）
// =============================================================================
void DesktopPet::goIdle()
{
    stopMoving();
    m_chain.clear();
    m_holdMsLeft = 0;
    m_anim->setState(PetState::Idle, true);
}

// =============================================================================
//  动作链：把若干"一次性动作"排队依次播放
//  例如 挥手 -> 开心 -> 待机，就是 runChain({Wave, Happy})
// =============================================================================
void DesktopPet::runChain(const QVector<PetState>& chain)
{
    if (chain.isEmpty())
        return;

    stopMoving();
    m_holdMsLeft = 0;
    m_chain = chain;
    playNextInChain();
}

void DesktopPet::playNextInChain()
{
    if (m_chain.isEmpty())
    {
        goIdle();
        return;
    }

    const PetState s = m_chain.takeFirst();
    m_anim->setState(s, true);
}

// =============================================================================
//  左键点击（不是拖动）
// =============================================================================
void DesktopPet::onPetClicked()
{
    // 下落途中不响应点击。
    // 下落通常不到 1 秒，点它本来也没什么意义；但如果真让它在这里切了表情，
    // 状态会变成"悬在半空做表情"—— 因为位置已经掉到一半，而移动逻辑被停掉了。
    if (isFalling())
        return;

    const bool wasSleeping = m_behavior->isSleeping();

    // 提醒行为控制器"用户来过"，并把 5 分钟睡眠计时清零
    m_behavior->notifyUserInteraction();

    // 点它本身也算"摸了一下"：和面板上的「摸摸」共用每天的额度。
    // 这里不看返回值 —— 额度用完时它照样该有反应（只是不加分），
    // 不然点到第 11 下它就突然变成一块木头了。
    m_affection->addFromPet();

    if (wasSleeping)
    {
        // 睡觉时被点：SLEEP -> WAVE -> HAPPY -> IDLE
        runChain(QVector<PetState>{ PetState::Wave, PetState::Happy });
        return;
    }

    // 平时点击：随机给一个反应，不要每次都一样
    switch (int(QRandomGenerator::global()->bounded(quint32(3))))
    {
    case 0:
        runChain(QVector<PetState>{ PetState::Wave });
        break;
    case 1:
        runChain(QVector<PetState>{ PetState::Happy });
        break;
    default:
        runChain(QVector<PetState>{ PetState::Surprise });
        break;
    }
}

// =============================================================================
//  动画换帧 -> 请求重绘
// =============================================================================
void DesktopPet::onFrameChanged()
{
    // 注意：改状态不会自动重绘，必须调 update()。
    // Qt 会把这个请求合并，在下次空闲时统一调一次 paintEvent，不会一帧画很多次。
    update();
}

// =============================================================================
//  动画状态变了 -> 调整窗口尺寸
// =============================================================================
void DesktopPet::onAnimationStateChanged(PetState now, PetState before)
{
    Q_UNUSED(before);
    resizeWindowForState(now);
    update();
}

// =============================================================================
//  一次性动作播完了
// =============================================================================
void DesktopPet::onActionFinished(PetState finished)
{
    // 1) 收步播完 -> 这一次走路才算真正结束，回站立
    //    注意这里必须 goIdle()：收步的 28/29 是「停在最后一帧」的一次性动作，
    //    必须由这一句接手，否则会停在收步的最后一帧上不动。
    if (finished == PetState::WalkStopRight || finished == PetState::WalkStopLeft)
    {
        goIdle();
        return;
    }

    // 2) 转身播完 -> 继续朝新方向走 / 飞
    if (finished == PetState::TurnLeft || finished == PetState::TurnRight)
    {
        if (m_move == Move::None)
        {
            goIdle();
            return;
        }

        const PetState movingState = (m_move == Move::Fly)
                                         ? PetState::Fly
                                         : (m_moveDir > 0 ? PetState::WalkRight : PetState::WalkLeft);
        m_anim->setState(movingState, true);
        return;
    }

    // 3) 动作链还有下一环 -> 接着播
    if (!m_chain.isEmpty())
    {
        playNextInChain();
        return;
    }

    // 4) 普通一次性动作结束 -> 回待机
    stopMoving();
    m_anim->setState(PetState::Idle, true);
}

// =============================================================================
//  行为控制器要求切换状态
// =============================================================================
void DesktopPet::onBehaviorState(PetState s, int holdMs)
{
    // ★ 正在被拖动、或者正在下落时，自动行为一律不理会 ★
    //   行为控制器每隔 5~15 秒就会掷一次骰子。如果不挡这一下：
    //     · 拖动中 —— 拖过十几秒拖拽图就会被随机动作顶掉，手里的角色当场不听话；
    //     · 下落中 —— 随机动作会把下落图顶掉，而且它一 stopMoving 就停在半空，
    //                 再也掉不下去（站着悬在空中，看着就像卡死了）。
    if (m_dragging || isFalling())
        return;

    applyState(s, holdMs);
}

void DesktopPet::onBehaviorWalk(int direction, int durationMs)
{
    if (m_dragging || isFalling())
        return;

    startWalk(direction, durationMs);
}

// =============================================================================
//  鼠标按下
// =============================================================================
void DesktopPet::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
    {
        QWidget::mousePressEvent(event);
        return;
    }

    // 点在角色的透明区域时不理它（这样抓"空空气"不会把桌宠拖走）
    if (!hitCharacter(event->position().toPoint()))
    {
        event->ignore();
        return;
    }

    m_pressed = true;
    m_dragging = false;
    m_pressGlobal = event->globalPosition().toPoint();
    m_pressTopLeft = pos();
    syncPosFromWindow();

    event->accept();
}

// =============================================================================
//  鼠标移动
//
//  ★ 区分「点击」和「拖动」的关键 ★
//    按下时先不算拖动，只有当鼠标相对按下点移动超过 DRAG_THRESHOLD_PX 像素，
//    才把 m_dragging 置为 true，并开始跟随鼠标。
//    松开时看 m_dragging：true 说明是拖动（不触发点击动作）；
//    false 说明是点击（触发随机动作）。
// =============================================================================
void DesktopPet::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_pressed)
    {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const QPoint delta = event->globalPosition().toPoint() - m_pressGlobal;

    if (!m_dragging)
    {
        // 还没超过阈值 -> 依然可能是"点击"，先不动
        if (delta.manhattanLength() < PetCfg::DRAG_THRESHOLD_PX)
            return;

        // 正式进入拖动状态：停下来，换成"被拎起来"的 30 号图，避免"一边走一边被拖"
        m_dragging = true;
        // 如果它正在往下掉，这一下就是"空中把它接住"。
        // 传 false 是关键：默认的兜底会先把窗口压到地面再停，那就成了
        // "抓住它 -> 它反而先掉到底 -> 再弹回手里"，很跳。
        stopMoving(/*snapToGround=*/false);
        m_holdMsLeft = 0;
        m_chain.clear();
        m_anim->setState(PetState::Drag, true);

        // ★ 换图之后必须重新取一次拖动基准 ★
        //   拖拽图(p30)比站立图高一些（那是刻意留出的"悬空量"），
        //   切过去会触发一次窗口 resize，而下面跟随鼠标用的是
        //   "按下时的窗口左上角 + 鼠标位移"。基准不重取的话，
        //   窗口一变高，同一个左上角就对应到更低的位置，角色会往下掉一截。
        m_pressTopLeft = pos();

        m_behavior->notifyUserInteraction();
    }

    // 桌宠跟随鼠标：窗口左上角 = 按下时的左上角 + 鼠标位移
    m_pos = QPointF(m_pressTopLeft + delta);
    // 拖到屏幕边上要被钳住，但"钳住"绝不该顺手触发撞边转身 —— 手里正拎着它呢
    clampToScreen(/*allowTurn=*/false);

    event->accept();
}

// =============================================================================
//  鼠标松开
// =============================================================================
void DesktopPet::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !m_pressed)
    {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    const bool wasDragging = m_dragging;

    m_pressed = false;
    m_dragging = false;

    if (wasDragging)
    {
        // 松手：不再"啪"一下变回站立，而是交给重力 —— 从当前高度自由落体，
        // 掉到屏幕底边才切回站立。见 startFall() / tickMovement() 的 Fall 分支。
        // （拖动期间自动行为是被屏蔽的，没人会替我们把状态改回来，所以这里必须自己接手。）
        startFall();

        // 拖完就停在这儿，让行为控制器过一会儿再安排它自己走
        m_behavior->notifyUserInteraction();

        // 拎起来玩了一趟，算一次互动（有 10 秒冷却，免得反复拎着抖）
        m_affection->addSource(AffectionSystem::Source::Drag,
                               PetCfg::AFF_DRAG_POINT,
                               PetCfg::AFF_DRAG_COOLDOWN_MS);
    }
    else
    {
        // 没拖动过 -> 这是一次真正的"点击"
        onPetClicked();
    }

    event->accept();
}

// =============================================================================
//  右键菜单
// =============================================================================
void DesktopPet::contextMenuEvent(QContextMenuEvent* event)
{
    if (!hitCharacter(event->pos()))
    {
        event->ignore();
        return;
    }

    const bool wasSleeping = m_behavior->isSleeping();
    m_behavior->notifyUserInteraction();
    if (wasSleeping)
        runChain(QVector<PetState>{ PetState::Wave, PetState::Happy });

    refreshMenuState();                                     // 让"暂停/恢复"两项的可用状态正确
    m_menu->exec(event->globalPos());
    event->accept();
}

// =============================================================================
//  像素级命中判定
//
//  窗口是个矩形，但角色只占其中一部分。如果不做这个判定，
//  点角色旁边的空白区域也会把桌宠拖走，用起来很别扭。
//  这里直接查这一帧图片在对应像素的 alpha：只有不透明的地方才算"摸到了"。
// =============================================================================
bool DesktopPet::hitCharacter(const QPoint& localPos) const
{
    if (!PetCfg::HIT_TEST_ALPHA)
        return true;   // 关掉像素级判定：整个窗口矩形都能点

    const QPixmap& pm = m_anim->currentPixmap();
    if (pm.isNull())
        return true;   // 没有图（资源缺失）时宽松处理，否则用户什么都点不动

    const QPoint p = localPos - frameTopLeft(pm);
    if (p.x() < 0 || p.y() < 0 || p.x() >= pm.width() || p.y() >= pm.height())
        return false;

    // toImage() 只在"按下/右键"这一瞬间做一次（零点几毫秒），不影响使用
    const QImage img = pm.toImage();
    return qAlpha(img.pixel(p)) > 8;
}

// =============================================================================
//  右键菜单
// =============================================================================
void DesktopPet::buildMenu()
{
    // parent = this，随窗口一起销毁
    m_menu = new QMenu(this);

    QAction* title = m_menu->addAction(QStringLiteral("桌宠"));
    title->setEnabled(false);       // 只是个标题，不可点
    m_menu->addSeparator();

    // ---- 和我聊天：直接翻到聊天页 ----
    // 加分不在这里做 —— 切到聊天页本身就会触发一次 noteChat()（见 ChatPage::showEvent），
    // 这里再记一笔就成了"点一次加两次"。所以这一项在下面的 QMenu::triggered 里
    // 也是被排除掉的。
    m_menu->addAction(QStringLiteral("和我聊天"), this, [this]()
    {
        m_behavior->notifyUserInteraction();
        openPanel();
        if (m_panel && m_chatPage)
        {
            m_panel->setCurrentPage(m_panel->indexOfPage(m_chatPage));
            m_chatPage->focusInput();
        }
    });

    // ---- 打开面板：右侧弹出一个主面板（左侧是功能导航）----
    // 面板是惰性创建的，第一次点才建；之后每次点只是把它叫到最前面。
    m_menu->addAction(QStringLiteral("打开面板"), this, [this]()
    {
        m_behavior->notifyUserInteraction();
        openPanel();
    });

    m_menu->addAction(QStringLiteral("开心一下"), this, [this]()
    {
        m_behavior->notifyUserInteraction();
        runChain(QVector<PetState>{ PetState::Happy });
    });

    // 方便当场看一遍行走时序：起步 -> 循环 -> 收步
    m_menu->addAction(QStringLiteral("走两步"), this, [this]()
    {
        m_behavior->notifyUserInteraction();
        startWalk(m_moveDir, PetCfg::WALK_SHORTEST_MS * 2);
    });

    m_menu->addAction(QStringLiteral("坐下休息"), this, [this]()
    {
        m_behavior->notifyUserInteraction();
        // 坐下是循环状态，这里给它一个持续时长，到点自动回待机
        applyState(PetState::Sit, PetCfg::MENU_SIT_MS);
    });

    m_menu->addAction(QStringLiteral("睡觉"), this, [this]()
    {
        m_behavior->notifyUserInteraction();
        m_behavior->sleepNow();     // 交给行为控制器，它会负责"睡着后不再自动走动"
    });

    m_menu->addAction(QStringLiteral("挥手"), this, [this]()
    {
        m_behavior->notifyUserInteraction();
        runChain(QVector<PetState>{ PetState::Wave, PetState::Happy });
    });

    m_menu->addAction(QStringLiteral("思考"), this, [this]()
    {
        m_behavior->notifyUserInteraction();
        runChain(QVector<PetState>{ PetState::Think });
    });

    m_menu->addAction(QStringLiteral("吃东西"), this, [this]()
    {
        m_behavior->notifyUserInteraction();
        runChain(QVector<PetState>{ PetState::Eat, PetState::Happy });
    });

    m_menu->addAction(QStringLiteral("喝水"), this, [this]()
    {
        m_behavior->notifyUserInteraction();
        runChain(QVector<PetState>{ PetState::Drink, PetState::Happy });
    });

    m_menu->addAction(QStringLiteral("生气"), this, [this]()
    {
        // 12 生气 -> 19 生气跺脚 -> 10 委屈 -> 08 待机
        m_behavior->notifyUserInteraction();
        runChain(QVector<PetState>{ PetState::Angry, PetState::AngryFoot, PetState::Sad });
    });

    m_menu->addAction(QStringLiteral("飞行"), this, [this]()
    {
        m_behavior->notifyUserInteraction();
        startFly(m_moveDir, PetCfg::MENU_FLY_MS);
    });

    m_menu->addSeparator();

    m_actPause = m_menu->addAction(QStringLiteral("暂停自动行为"), this, [this]()
    {
        m_behavior->setAutoEnabled(false);
        goIdle();                   // 暂停时先回到站立，别让它停在半路
    });

    m_actResume = m_menu->addAction(QStringLiteral("恢复自动行为"), this, [this]()
    {
        m_behavior->setAutoEnabled(true);
    });

    m_menu->addSeparator();

    // ---- 隐藏到状态栏 ----
    // 注意它和"退出"的区别：隐藏只是把窗口收起来、程序还活着（右下角留一个图标），
    // 想回来就在那个图标上右键 -> 恢复。
    m_actHide = m_menu->addAction(QStringLiteral("隐藏到状态栏"), this, [this]()
    {
        hideToTray();
    });

    // 退出程序：qApp->quit() 会让 main() 里的 app.exec() 返回
    m_menu->addAction(QStringLiteral("退出"), this, []()
    {
        QApplication::quit();
    });

    // ---- 菜单里的"玩耍"动作统一记好感度 ----
    //
    // ★ 为什么用 QMenu::triggered 统一接，而不是在 10 个 lambda 里各写一行 ★
    //   以后往菜单里加新动作时，不用记得"还要顺手加一句加分"——
    //   这是最容易漏、漏了又不会报错的那类改动。
    //
    //   代价是得把"不算互动"的几项排除掉，分两类：
    //     · 系统项（暂停 / 恢复 / 隐藏）：它们只改程序状态，不是陪它玩
    //     · 入口项（和我聊天 / 打开面板）和退出："和我聊天"的加分走的是
    //       "切到聊天页 -> noteChat()"那条路（和面板里点进去同一个入口，只加一次），
    //       这里再记一笔会变成一次点击加两份；退出则根本不该算。
    connect(m_menu, &QMenu::triggered, this, [this](QAction* a) {
        if (!a || a == m_actPause || a == m_actResume || a == m_actHide)
            return;

        const QString t = a->text();
        if (t == QStringLiteral("和我聊天") || t == QStringLiteral("打开面板") || t == QStringLiteral("退出"))
            return;

        if (m_affection)
            m_affection->addSource(AffectionSystem::Source::MenuAction,
                                   PetCfg::AFF_MENU_POINT,
                                   PetCfg::AFF_MENU_COOLDOWN_MS);
    });
}

void DesktopPet::refreshMenuState()
{
    const bool autoOn = m_behavior->autoEnabled();
    if (m_actPause)
        m_actPause->setEnabled(autoOn);
    if (m_actResume)
        m_actResume->setEnabled(!autoOn);

    // 万一本机没有系统托盘（极罕见），点了"隐藏"就再也叫不回来了 —— 直接禁掉
    if (m_actHide)
        m_actHide->setEnabled(trayReady());
}

// =============================================================================
//  系统托盘：藏进右下角状态栏，以及从那里恢复
//
//  和"退出"的区别（菜单里这两项挨着，最容易混）：
//      隐藏  -> 窗口收起来、状态栏出现图标、程序继续活着、状态都还在
//      退出  -> 进程结束
// =============================================================================
void DesktopPet::setupTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
    {
        // 没有托盘的话，"隐藏"就是一张有去无回的单程票，
        // 所以这里干脆不建托盘，菜单里那一项也会被 refreshMenuState() 禁掉。
        qWarning() << "[PetPal] 本机没有可用的系统托盘，「隐藏到状态栏」已禁用";
        return;
    }

    m_tray = new QSystemTrayIcon(this);

    // ---- 图标：不另做 .ico，直接拿站立图缩出来 ----
    // 按几个常用尺寸各烘一份交给系统去挑，100% / 150% 缩放下都不糊（本机是 150%）。
    QIcon icon;
    const QPixmap src(petImagePath(PetCfg::TRAY_ICON_IMG));
    if (!src.isNull())
    {
        static const int sizes[] = { 16, 20, 24, 32, 48 };
        for (int px : sizes)
            icon.addPixmap(src.scaled(px, px, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    else
    {
        qWarning() << "[PetPal] 托盘图标用的站立图没加载出来，退回默认图标";
    }
    m_tray->setIcon(icon);
    m_tray->setToolTip(QStringLiteral("PetPal 桌宠 —— 左键单击回来，右键看菜单"));

    // ---- 状态栏图标上右键弹出的那个小面板：恢复 / 退出 ----
    m_trayMenu = new QMenu(this);
    m_trayMenu->addAction(QStringLiteral("恢复"), this, [this]()
    {
        restoreFromTray();
    });
    m_trayMenu->addSeparator();
    m_trayMenu->addAction(QStringLiteral("退出"), this, []()
    {
        QApplication::quit();
    });
    m_tray->setContextMenu(m_trayMenu);

    // 左键单击也直接恢复 —— 这是 Windows 上托盘图标最顺手的用法，
    // 只有想退出的人才去右键。
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason)
    {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
            restoreFromTray();
    });

    // ★ 这里故意不调 m_tray->show() ★
    // 托盘图标只在"藏进状态栏"之后才出现。平时桌宠本来就站在屏幕上，
    // 再挂一个状态栏图标等于同一只宠物开了两个入口，反而乱。
}

void DesktopPet::hideToTray()
{
    if (m_hiddenToTray || !trayReady())
        return;

    // 藏之前先收拾干净：
    //   · stopMoving() 让它落地停步 —— 否则藏起来的这段时间它还在"走"，
    //     再放出来时位置已经跑到别处去了；
    //   · goIdle() 回到站立，状态栏图标用的也是这张站姿图，视觉上对得上。
    stopMoving();
    goIdle();

    // 记住用户原本有没有暂停自动行为，回来时原样还回去（别擅自帮他打开）
    m_autoBeforeHide = m_behavior->autoEnabled();
    m_behavior->setAutoEnabled(false);

    // 60Hz 心跳也停掉：窗口都看不见了，没必要每秒还算 60 次位置
    m_tick->stop();

    // 好感度的"陪伴"加分也要停 —— 人都看不见了，不该继续算陪伴。
    // （不加这一句的话，把桌宠藏起来挂一整天也能刷满陪伴分，那就失去意义了）
    m_affection->setCompanyActive(false);

    // 面板一起收起来：桌宠都藏了，面板还杵在屏幕正中会很莫名其妙
    closePanel();

    m_hiddenToTray = true;
    hide();            // 窗口消失
    m_tray->show();    // 状态栏图标出现
}

void DesktopPet::restoreFromTray()
{
    if (!m_hiddenToTray)
        return;

    m_tray->hide();    // 图标撤掉
    m_hiddenToTray = false;

    show();            // 窗口原样回来（位置和尺寸都没动过）

    // ★ 时钟基准必须重取 ★
    // m_clock 是单调时钟，藏起来的整段时间它一直在走。不重取的话，
    // 恢复后第一次心跳算出的 dt 是"藏了多久"——虽然 MAX_TICK_DT_S 会兜住，
    // 但那是把它当垃圾丢掉，画面会顿一下。这里重新对一次基准最干净。
    m_clock.start();
    m_lastNs = m_clock.nsecsElapsed();
    m_tick->start(PetCfg::TICK_MS);

    // 按藏之前的开关恢复自动行为：藏之前是暂停的，回来还是暂停
    if (m_autoBeforeHide)
        m_behavior->setAutoEnabled(true);

    // 陪伴加分重新开始计
    m_affection->setCompanyActive(true);

    m_behavior->notifyUserInteraction();   // 清零"多久没互动"，别一放出来就犯困
}

// -----------------------------------------------------------------------------
//  托盘状态描述（--selftest 报告用，详细理由见头文件）
// -----------------------------------------------------------------------------
QString DesktopPet::describeTray() const
{
    QString s;
    s += QStringLiteral("系统托盘可用     : %1\r\n")
             .arg(QSystemTrayIcon::isSystemTrayAvailable() ? QStringLiteral("是")
                                                          : QStringLiteral("否（「隐藏到状态栏」会被禁用）"));
    s += QStringLiteral("托盘图标         : %1\r\n")
             .arg(m_tray ? QStringLiteral("已创建（由站立图缩出 16/20/24/32/48 五档）")
                         : QStringLiteral("未创建"));
    s += QStringLiteral("托盘右键菜单     : %1\r\n")
             .arg(m_trayMenu ? QStringLiteral("恢复 / 退出 —— 共 2 项")
                             : QStringLiteral("未创建"));
    s += QStringLiteral("「隐藏到状态栏」 : %1\r\n")
             .arg((m_actHide && m_actHide->isEnabled())
                      ? QStringLiteral("已启用（在「退出」上方）")
                      : QStringLiteral("已禁用"));
    s += QStringLiteral("当前是否已隐藏   : %1\r\n")
             .arg(m_hiddenToTray ? QStringLiteral("是（程序还活着，图标在状态栏）")
                                 : QStringLiteral("否（桌宠显示在桌面上）"));
    return s;
}

// -----------------------------------------------------------------------------
//  隐藏 -> 恢复 往返测试（--selftest 用，详细理由见头文件）
//
//  走的是和菜单项、托盘图标完全一样的两条路径（hideToTray / restoreFromTray），
//  不是另写一套等价的代码 —— 否则测过了也不代表真实操作没问题。
// -----------------------------------------------------------------------------
QString DesktopPet::debugTrayRoundTrip()
{
    if (!trayReady())
        return QStringLiteral("  [跳过] 本机没有可用的系统托盘\r\n");

    const auto yn = [](bool b) { return b ? QStringLiteral("是") : QStringLiteral("否"); };

    QString s;
    s += QStringLiteral("  隐藏前 : 窗口可见=%1  心跳=%2  自动行为=%3\r\n")
             .arg(yn(isVisible()), yn(m_tick->isActive()), yn(m_behavior->autoEnabled()));

    hideToTray();
    s += QStringLiteral("  隐藏后 : 窗口可见=%1  心跳=%2  自动行为=%3  状态栏图标=%4\r\n")
             .arg(yn(isVisible()), yn(m_tick->isActive()), yn(m_behavior->autoEnabled()),
                  yn(m_tray->isVisible()));

    restoreFromTray();
    s += QStringLiteral("  恢复后 : 窗口可见=%1  心跳=%2  自动行为=%3  状态栏图标=%4\r\n")
             .arg(yn(isVisible()), yn(m_tick->isActive()), yn(m_behavior->autoEnabled()),
                  yn(m_tray->isVisible()));

    // 往返一趟之后应该完全回到原样：窗口在、心跳在跑、隐藏标记清掉
    const bool ok = isVisible() && m_tick->isActive() && !m_hiddenToTray && !m_tray->isVisible();
    s += ok ? QStringLiteral("  [OK] 隐藏/恢复往返正常，状态都收回来了\r\n")
            : QStringLiteral("  [!!] 往返之后状态不对，检查 hideToTray / restoreFromTray\r\n");
    return s;
}

// =============================================================================
//  主面板
//
//  加新功能页只要写一个 QWidget，然后 m_panel->addPage("标题", page) ——
//  左侧导航会自动多一项、右侧自动多一页，这里的布局代码一行都不用动。
//  （原先这里有个 makePlaceholderPage() 给"聊天"占位，聊天页做出来之后就删了。
//    再加页不需要占位函数，MainPanel 的 addPage 本身就是那个接口。）
// =============================================================================

void DesktopPet::openPanel()
{
    if (!m_panel)
    {
        // 第一次点开才建。之后一直留着（关闭只是 hide），所以再点开是秒开、状态不丢。
        m_panel = new MainPanel(nullptr);        // 顶层窗口，没有 parent，析构时手动 delete

        // ---- 好感度页 ----
        m_affPage = new AffectionPage(m_affection, m_panel);
        m_panel->addPage(QStringLiteral("好感度"), m_affPage);

        // ★ 页面只喊"用户想做什么"，怎么响应由这里决定 ★
        //  加分和播动画必须成对发生，所以两件事写在同一个 lambda 里。
        //  如果让页面自己加分、这里只播动画，以后改规则要同时改两个文件，
        //  迟早出现"加了分没播动画"或者"额度用完了还在播"。
        connect(m_affPage, &AffectionPage::petPetted, this, [this]() {
            // 返回 0 说明额度用完或还在冷却 —— 那就什么也不做（面板按钮此时本来就是灰的）
            if (m_affection->addFromPet() > 0.0)
                runChain(QVector<PetState>{ PetState::Happy });          // 摸摸 -> 开心
        });

        connect(m_affPage, &AffectionPage::feedRequested, this, [this]() {
            if (m_affection->addFromFeed() > 0.0)
                runChain(QVector<PetState>{ PetState::Eat, PetState::Happy });  // 喂食 -> 吃东西
        });

        connect(m_affPage, &AffectionPage::chatRequested, this, &DesktopPet::noteChat);

        // ---- 每日图片页 ----
        // 没有信号要接：这页自己管"今天挑哪张图"和"双击开原图"，
        // 桌宠这边不需要为它做任何事（不涉及好感度，也不播动画）。
        m_dailyPage = new DailyImagePage(/*persistent=*/true, m_panel);
        m_panel->addPage(QStringLiteral("每日图片"), m_dailyPage);

        // ---- 聊天页（洛天依，预设台词，不联网）----
        // ★ 页面只发信号，不碰好感度、不碰桌宠状态 ★
        //  加分和播动作必须成对发生，所以两件事都写在这一个地方（和摸摸/喂食同一个规矩）。
        m_chatPage = new ChatPage(m_affection, m_panel);
        m_panel->addPage(QStringLiteral("聊天"), m_chatPage);

        // 每次切到聊天页 = 一次聊天。聊多少句都不再额外加分，
        // 所以反复说话刷不到分（额度是每日 AFF_CHAT_PER_DAY 次）。
        connect(m_chatPage, &ChatPage::chatEntered, this, &DesktopPet::noteChat);

        // 这句回复带着什么情绪，就播什么动作。
        // Neutral / Thinking 不切动作：闲聊时突然换个表情，反而像是被打断，
        // 而且大部分回复本来就该让它继续做自己的事。
        connect(m_chatPage, &ChatPage::moodChanged, this, [this](ChatScript::Mood mood) {
            if (m_dragging || isFalling())
                return;      // 正被拎着 / 正在下落，别打断画面（和升级那一条同一个理由）

            PetState s = PetState::Idle;
            switch (mood)
            {
            case ChatScript::Mood::Happy: s = PetState::Happy;    break;
            case ChatScript::Mood::Shy:   s = PetState::Surprise; break;  // 没有专用害羞帧，用惊讶顶一下
            case ChatScript::Mood::Sad:   s = PetState::Sad;      break;
            default:                      return;                          // Neutral / Thinking：不切
            }
            runChain(QVector<PetState>{ s });
        });

        // ---- 设置页 ----
        // 弹窗确认已经在页面里做完了（那是界面自己的事），这里收到的信号
        // 只代表"用户确实点过确认"。真正清零这一下仍然放在这里 ——
        // 和摸摸/喂食同一个规矩：页面只喊意图，谁持有数据谁动手。
        m_setPage = new SettingsPage(m_affection, m_panel);
        m_panel->addPage(QStringLiteral("设置"), m_setPage);

        connect(m_setPage, &SettingsPage::resetAffectionRequested, this, [this]() {
            // resetAll() 内部会 save() + emit changed()，
            // 所以两个页面的数字会自动刷新，不用在这里手动通知。
            m_affection->resetAll();
        });
    }

    // 居中在"桌宠所在的那块屏幕"，不是主屏。
    // 双屏时这一点很关键：桌宠在副屏上待着，面板却弹到主屏正中，会像是别的程序弹出来的。
    m_panel->showCenteredIn(availableScreenRect());
}

void DesktopPet::closePanel()
{
    if (m_panel && m_panel->isVisible())
        m_panel->hide();
}

void DesktopPet::noteChat()
{
    // 先记一笔好感度（每日上限 AFF_CHAT_PER_DAY 次），再交出去。
    // 谁调它：切到聊天页（ChatPage::showEvent）、好感度页那个"和我聊聊"按钮。
    // ★ 加分和"播什么动作"是两回事：这里只管加分。桌宠摆什么表情由
    //   ChatPage::moodChanged 那条连接决定 —— 用户点进来时它甚至可能正在睡觉。
    m_affection->addSource(AffectionSystem::Source::Chat, PetCfg::AFF_CHAT_POINT);
    emit chatRequested();
}

QString DesktopPet::describeAffection() const
{
    return m_affection ? m_affection->describe() : QStringLiteral("好感度对象不存在\r\n");
}
