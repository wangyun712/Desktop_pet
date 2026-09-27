#include "PetFx.h"
#include "UiFont.h"

#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRandomGenerator>
#include <QScreen>
#include <QTimer>

// =============================================================================
//  布局常量（100% 档位基准；特效是屏幕上的烟 花 糖，不随字号档位缩放）
// =============================================================================
namespace {

constexpr int kMarginX   = 50;    // 特效层比桌宠左右各宽出的余量
constexpr int kTopRoom   = 170;   // 头顶上方的活动空间（心能飘多高）
constexpr int kBottomPad = 14;    // 底部余量（心的出生点略进桌宠头顶）
constexpr int kTickMs    = 33;    // 粒子推进周期
constexpr int kFollowMs  = 30;    // 跟随轮询周期（和推进共用一拍）
constexpr int kZzzMs     = 1100;  // 睡觉时飘 Z 的间隔
constexpr int kShowMs    = 4000;  // 单颗心的寿命上限参考（实际 1.2~2.0s 随机）

// 经典"双弧 + 尖底"心形：两个三次贝塞尔拼出来，c 是心中心，s 是心宽
QPainterPath heartPath(const QPointF& c, qreal s)
{
    QPainterPath p;
    p.moveTo(c.x(), c.y() + 0.32 * s);
    p.cubicTo(c.x() + 0.55 * s, c.y() - 0.12 * s,
              c.x() + 0.32 * s, c.y() - 0.48 * s,
              c.x(),            c.y() - 0.18 * s);
    p.cubicTo(c.x() - 0.32 * s, c.y() - 0.48 * s,
              c.x() - 0.55 * s, c.y() - 0.12 * s,
              c.x(),            c.y() + 0.32 * s);
    return p;
}

} // namespace

PetFx::PetFx(QWidget* parent)
    : QWidget(parent)
{
    // 置顶 + 无边框 + 透明 + 绝不抢焦点（同 PetBubble/桌面歌词）
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    // ★ 特效层绝不挡鼠标 ★ 它盖在桌宠头顶上方，鼠标事件必须穿透下去，
    // 桌宠才能照常被点、被拎（穿透后点在桌宠身上的事件会落到桌宠窗口）。
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setWindowTitle(QStringLiteral("桌宠特效"));

    m_tickTimer = new QTimer(this);
    m_tickTimer->setInterval(kTickMs);
    connect(m_tickTimer, &QTimer::timeout, this, &PetFx::tick);

    m_zzzTimer = new QTimer(this);
    m_zzzTimer->setInterval(kZzzMs);
    connect(m_zzzTimer, &QTimer::timeout, this, [this] {
        if (m_sleeping && m_pet && m_pet->isVisible())
            spawnZzz();
    });
}

void PetFx::trackPet(QWidget* pet)
{
    m_pet = pet;
}

// =============================================================================
//  对外接口
// =============================================================================
void PetFx::burstHearts(int count)
{
    if (!m_pet || count <= 0)
        return;

    // ★ 先把覆盖层摆到位，再生成粒子 ★ spawnHeart 用的是特效窗内坐标
    // （桌面屏幕坐标 - fx 左上角），摆位之前 geometry() 还是旧值 ——
    // 首次爆发时窗还在默认位置，粒子全算到窗外，画出来就是一张空图。
    ensureRunning();

    const QRect petGeo = m_pet->frameGeometry();
    const QPointF head(petGeo.center().x(), petGeo.top() + 12);   // 头顶（屏幕坐标）
    for (int i = 0; i < count; ++i)
        spawnHeart(head);
}

void PetFx::setSleeping(bool on)
{
    if (m_sleeping == on)
        return;
    m_sleeping = on;
    if (on)
    {
        ensureRunning();               // 覆盖层先就位
        m_zzzTimer->start();           // Zzz 飘起来
    }
    else
    {
        m_zzzTimer->stop();            // 醒了：不再补货，存量 Z 飘完自然收
    }
}

void PetFx::petMoved()
{
    if (isVisible())
        placeOver();
}

// =============================================================================
//  推进 / 摆位 / 收工
// =============================================================================
void PetFx::tick()
{
    // 桌宠本体没了 / 藏进托盘了：特效跟着消失（下次 burst/setSleeping 会再起）
    if (!m_pet || !m_pet->isVisible())
    {
        m_tickTimer->stop();
        hide();
        return;
    }

    // 推进粒子：上飘 + 左右轻摆 + 掉寿命
    const double dt = kTickMs / 1000.0;
    for (auto it = m_particles.begin(); it != m_particles.end();)
    {
        it->life -= dt;
        if (it->life <= 0.0)
        {
            it = m_particles.erase(it);
            continue;
        }
        it->phase += dt * 3.0;
        it->pos.ry() += it->vy * dt;
        it->pos.rx() += std::sin(it->phase) * it->sway * dt;
        ++it;
    }

    // 没活了（粒子飘完、也没在睡觉）→ 收工：定时器停 + 窗隐藏，零开销
    if (m_particles.isEmpty() && !m_sleeping)
    {
        m_tickTimer->stop();
        hide();
        return;
    }

    // 睡觉但这一拍还没新 Z：不用重绘，摆位照常（桌宠可能在走路）
    if (!m_particles.isEmpty())
        update();
    placeOver();
}

void PetFx::placeOver()
{
    if (!m_pet)
        return;

    const QRect petGeo = m_pet->frameGeometry();
    QScreen* scr = m_pet->screen();
    const QRect avail = scr ? scr->availableGeometry()
                            : QGuiApplication::primaryScreen()->availableGeometry();

    // 覆盖区 = 桌宠横向加宽 + 头顶上方一大块（粒子的活动空间），夹回屏幕
    QRect target(petGeo.left() - kMarginX,
                 petGeo.top() - kTopRoom,
                 petGeo.width() + 2 * kMarginX,
                 petGeo.height() + kTopRoom + kBottomPad);
    target.moveLeft(qBound(avail.left(), target.left(),
                           qMax(avail.left(), avail.right() - target.width())));
    target.moveTop(qBound(avail.top(), target.top(),
                          qMax(avail.top(), avail.bottom() - target.height())));

    if (target == geometry() && petGeo == m_lastPetGeo)
        return;                            // 什么都没变（轮询路径的常态）

    setGeometry(target);
    if (petGeo != m_lastPetGeo)
        raise();                           // 桌宠动过：特效层压回它上面
    m_lastPetGeo = petGeo;
}

void PetFx::ensureRunning()
{
    placeOver();
    if (!isVisible())
    {
        show();
        raise();
    }
    if (!m_tickTimer->isActive())
        m_tickTimer->start();
}

// =============================================================================
//  生成粒子
// =============================================================================
void PetFx::spawnHeart(const QPointF& headScreen)
{
    const QRect fxGeo = geometry();
    // 桌宠头顶（屏幕坐标）→ 特效窗内坐标，再撒一点随机偏移
    const QPointF local = headScreen - QPointF(fxGeo.left(), fxGeo.top());
    QRandomGenerator* rng = QRandomGenerator::global();

    Particle pt;
    pt.pos     = QPointF(local.x() + rng->bounded(80) - 40,
                         local.y() + rng->bounded(20) - 10);
    pt.vy      = -(40 + rng->bounded(45));                      // 40~85 px/s 上飘
    pt.sway    = 6 + rng->bounded(10);
    pt.phase   = rng->bounded(628) / 100.0;
    pt.maxLife = pt.life = 1.2 + rng->bounded(80) / 100.0;      // 1.2~2.0s
    pt.size    = UiFont::px(10 + rng->bounded(8));              // 心宽 10~18
    pt.zzz     = false;
    m_particles.append(pt);
}

void PetFx::spawnZzz()
{
    const QRect petGeo = m_pet->frameGeometry();
    const QRect fxGeo = geometry();
    const QPointF local = QPointF(petGeo.center().x(), petGeo.top() + 6)
                          - QPointF(fxGeo.left(), fxGeo.top());
    QRandomGenerator* rng = QRandomGenerator::global();

    Particle pt;
    pt.pos     = QPointF(local.x() + rng->bounded(14) - 7, local.y());
    pt.vy      = -(18 + rng->bounded(14));                      // 慢慢飘
    pt.sway    = 10 + rng->bounded(8);                          // Z 飘得歪歪扭扭
    pt.phase   = rng->bounded(628) / 100.0;
    pt.maxLife = pt.life = 1.6 + rng->bounded(60) / 100.0;
    pt.size    = UiFont::px(12 + rng->bounded(10));             // Z 字号 12~22
    pt.zzz     = true;
    m_particles.append(pt);
}

// =============================================================================
//  绘制
// =============================================================================
void PetFx::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    for (const Particle& pt : m_particles)
    {
        const qreal alpha = qBound(0.0, pt.life / pt.maxLife, 1.0);

        if (pt.zzz)
        {
            // Z：灰白粗体斜排上飘
            QFont f;
            f.setPixelSize(qMax(10, qRound(pt.size)));
            f.setBold(true);
            p.setFont(f);
            p.setPen(QColor(158, 156, 150, int(alpha * 225)));
            p.drawText(pt.pos, QStringLiteral("Z"));
        }
        else
        {
            // 心：粉色，随寿命淡出
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 107, 138, int(alpha * 230)));
            p.drawPath(heartPath(pt.pos, pt.size));
        }
    }
}
