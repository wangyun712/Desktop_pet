#include "PetBubble.h"
#include "UiFont.h"
#include "UiTheme.h"

#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QScreen>
#include <QTimer>

// =============================================================================
//  布局常量（100% 字号档位基准，实际用 UiFont::px() 换算）
// =============================================================================
namespace {

constexpr int kPad      = 10;    // 文字到气泡边的留白
constexpr int kTailH    = 9;     // 尾巴高度（指向桌宠头顶）
constexpr int kMaxTextW = 260;   // 文字最大宽：超了就换行，气泡不无限变宽
constexpr int kShowMs   = 3000;  // 一句话挂多久
constexpr int kFollowMs = 30;    // 跟随轮询周期（桌宠 60Hz 动，气泡 33Hz 跟，够顺）

QFont bubbleFont()
{
    QFont f;
    f.setPixelSize(qMax(11, UiFont::px(13)));
    return f;
}

} // namespace

PetBubble::PetBubble(QWidget* parent)
    : QWidget(parent)
{
    // 和桌宠本体一个路数的置顶小窗：Qt::Tool（不占任务栏）+ 无边框 + 透明 +
    // 绝不抢焦点 —— 气泡一冒出来就把用户正在打的字顶掉，那就本末倒置了。
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setWindowTitle(QStringLiteral("桌宠台词"));

    m_hideTimer = new QTimer(this);
    m_hideTimer->setSingleShot(true);
    // ★ interval 必须设 ★ QTimer 默认间隔是 0 —— 不设的话 start() 就是
    // "0 毫秒后隐藏"，气泡 show 出来的下一圈事件循环就被自己收走，
    // 肉眼看正好是"闪了一下就没了"。（这个 bug 自检的 grab() 抓不出来：
    // grab 对隐藏的窗口照样能离屏渲染。）
    m_hideTimer->setInterval(kShowMs);
    connect(m_hideTimer, &QTimer::timeout, this, [this] {
        m_followTimer->stop();
        hide();
    });

    // 显示期间 30ms 读一次桌宠窗口的位置再摆到自己该在的地方 ——
    // 桌宠怎么动（走路 / 被拎 / 下落）气泡都不用关心，读 pos() 就行。
    // 隐藏时轮询停掉，零开销。（不去钩 applyWindowPos —— 那样得在每条
    // 移动路径上都插一脚，还得担心拖动路径的例外。）
    m_followTimer = new QTimer(this);
    m_followTimer->setInterval(kFollowMs);
    connect(m_followTimer, &QTimer::timeout, this, [this] {
        if (m_pet && isVisible())
            placeAbove();
    });
}

void PetBubble::trackPet(QWidget* pet)
{
    m_pet = pet;
}

// =============================================================================
//  冒一句
// =============================================================================
void PetBubble::say(const QString& text)
{
    if (text.isEmpty() || !m_pet)
        return;

    // 同一句话还在显示中：只把 4 秒的闹钟续上，不重摆不重绘
    if (text == m_text && isVisible())
    {
        m_hideTimer->start();
        return;
    }

    m_text = text;
    relayout();        // 按新文本定尺寸（此时气泡必然是隐藏的 —— 调用方 petSay
                       // 会挡掉"气泡正忙"，所以不存在"看得见的窗口先 resize
                       // 再 move"的中间态闪烁）
    placeAbove();      // 摆位（一次 setGeometry + 压回桌宠上面）
    show();
    raise();
    m_hideTimer->start();
    m_followTimer->start();
}

// =============================================================================
//  桌宠动过 / 换过状态：立刻重新摆位
//
//  ★ 为什么 30ms 轮询之外还要这个入口 ★ 桌宠切状态时窗口会**变高**
//  （拎起图比站立图高 12px，底边贴地、顶边向上长）——顶边一抬就会盖进
//  气泡里，而 30ms 的轮询盖不住 resize 后的头两帧，肉眼看就是
//  "气泡闪一下、缺一块"。DesktopPet 在窗口真正动过的地方（applyWindowPos /
//  onAnimationStateChanged）同步喊这里，一帧都不漏。
// =============================================================================
void PetBubble::petMoved()
{
    if (isVisible())
        placeAbove();
}

// =============================================================================
//  尺寸：按文本换行重算（最大宽内自动折行，气泡不无限变宽）
// =============================================================================
void PetBubble::relayout()
{
    const QFont f = bubbleFont();
    const QFontMetrics fm(f);
    const QRect textRect = fm.boundingRect(0, 0, qMax(120, UiFont::px(kMaxTextW)), 0,
                                           Qt::TextWordWrap, m_text);
    const int w = textRect.width() + 2 * kPad;
    const int h = textRect.height() + 2 * kPad + kTailH;
    resize(qMax(UiFont::px(72), w), h);
}

// =============================================================================
//  摆位：气泡悬在桌宠头顶，尾巴尖指向它
// =============================================================================
void PetBubble::placeAbove()
{
    if (!m_pet)
        return;

    const QRect petGeo = m_pet->frameGeometry();
    QScreen* scr = m_pet->screen();
    const QRect avail = scr ? scr->availableGeometry()
                            : QGuiApplication::primaryScreen()->availableGeometry();

    // 尾巴尖对准桌宠头顶中心；整窗夹回屏幕里
    //（桌宠贴着屏幕顶端时气泡会压到它头顶一点 —— 可接受，比飞出屏幕强）
    int x = petGeo.center().x() - width() / 2;
    int y = petGeo.top() - height() - 2;
    x = qBound(avail.left(), x, qMax(avail.left(), avail.right() - width()));
    y = qBound(avail.top(),  y, qMax(avail.top(),  avail.bottom() - height()));

    // 桌宠没动、位置也没变 → 什么都不做（轮询路径的绝大多数情况）
    const QRect target(x, y, width(), height());
    if (petGeo == m_lastPetGeo && target == geometry())
        return;

    // ★ 位置和尺寸合成一次 setGeometry ★ 先 resize 再 move 是两次窗口重配置，
    // 透明小窗会闪一下"旧位置新尺寸"的中间态。
    setGeometry(target);

    // ★ 压回桌宠上面 ★ 桌宠切状态窗口变高会顶进气泡，而且它偶尔会重钉
    // 置顶（SetWindowPos 把它插到置顶层最上面）——跟着 raise 一下，
    // 气泡永远在桌宠上面。只有桌宠几何真变了才 raise，不做无谓的抖动。
    if (petGeo != m_lastPetGeo)
        raise();
    m_lastPetGeo = petGeo;
}

// =============================================================================
//  绘制：圆角气泡 + 朝下的尾巴 + 台词
// =============================================================================
void PetBubble::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const int bodyH = height() - kTailH;

    // 主体 + 尾巴合成**一个**路径再填再描 —— 分开画的话接缝处会有一条线
    QPainterPath body;
    body.addRoundedRect(QRectF(0.5, 0.5, width() - 1.0, bodyH - 1.0), 10, 10);
    QPainterPath tail;
    QPolygonF tri;
    tri << QPointF(width() / 2.0 - 7.0, bodyH - 1.0)
        << QPointF(width() / 2.0 + 7.0, bodyH - 1.0)
        << QPointF(width() / 2.0,       height() - 1.0);
    tail.addPolygon(tri);
    tail.closeSubpath();
    const QPainterPath bubble = body.united(tail);   // united：轮廓线没有内缝

    p.fillPath(bubble, QColor(255, 255, 255, 242));
    p.strokePath(bubble, QPen(UiTheme::color(UiTheme::Accent), 1.2));

    // 台词：气泡底是实色，普通深色字就够清楚，不需要描边
    p.setFont(bubbleFont());
    p.setPen(UiTheme::color(UiTheme::TextStrong));
    p.drawText(QRect(kPad, kPad, width() - 2 * kPad, bodyH - 2 * kPad),
               Qt::AlignHCenter | Qt::AlignVCenter | Qt::TextWordWrap, m_text);
}
