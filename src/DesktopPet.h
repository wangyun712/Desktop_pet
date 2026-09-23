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

class QTimer;
class QMenu;
class QAction;
class AnimationController;
class PetBehaviorController;

class DesktopPet : public QWidget
{
    Q_OBJECT
public:
    explicit DesktopPet(QWidget* parent = nullptr);

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

signals:
    // 「和我聊天」的接口。
    // 第一阶段只把信号发出去，不接 AI。以后接本地 Qwen 时，
    // 在主程序里 connect 这个信号，拿到回答后再决定播 09 开心 / 10 委屈 / 11 惊讶 / 12 生气。
    void chatRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private slots:
    void onTick();                                       // 60Hz 心跳
    void onFrameChanged();                               // 动画换帧 -> 重绘
    void onAnimationStateChanged(PetState now, PetState before);  // 换状态 -> 调整窗口大小
    void onActionFinished(PetState finished);            // 一次性动作播完 -> 决定后续
    void onBehaviorState(PetState s, int holdMs);        // 行为控制器要求切换状态
    void onBehaviorWalk(int direction, int durationMs);  // 行为控制器要求走一段路

private:
    // ---------- 窗口 ----------
    void  setupWindow();
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

    // ---------- 数据 ----------
    AnimationController*   m_anim = nullptr;       // 动画控制器
    PetBehaviorController* m_behavior = nullptr;   // 自动行为控制器

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
};
