#include "MainPanel.h"
#include "UiFont.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QStackedWidget>
#include <QPushButton>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QShortcut>
#include <QScreen>
#include <QGuiApplication>
#include <QWindow>          // startSystemMove()（见 mousePressEvent 的说明）

// =============================================================================
//  构造：搭出"顶部条 + (左侧导航 | 右侧内容)"
// =============================================================================
namespace {

// 100% 档位下的基准尺寸。applyUiScale() 会拿它们乘上字号档位 ——
// 详见 applyUiScale() 里的说明。
constexpr int BASE_NAV_W = 112;    // 左侧导航宽度
constexpr int BASE_WIN_W = 640;    // 面板默认宽
constexpr int BASE_WIN_H = 440;    // 面板默认高
constexpr int BASE_MIN_W = 560;    // 面板最小宽
constexpr int BASE_MIN_H = 400;    // 面板最小高

} // namespace

MainPanel::MainPanel(QWidget* parent) : QWidget(parent)
{
    // 无边框 + 独立窗口。
    // 用 Qt::Window（不是 Qt::Tool）是刻意的：它是"主面板"，在任务栏里占一格
    // 是合理的 —— 用户 Alt+Tab 能找回来。（要改成不占任务栏，换成 Qt::Tool 即可）
    //
    // ★ 这里**故意不加** Qt::WindowStaysOnTopHint ★（用户明确要求，2026-09-24）
    //   两个窗口的分工是：
    //     · 桌宠本体（DesktopPet）—— 置顶，永远压在所有窗口（含本面板）上面；
    //     · 主面板（这里）      —— 就是个普通窗口，该被别的窗口盖住就盖住。
    //   面板上原本也带着 WindowStaysOnTopHint，于是它跟桌宠一样永远在最前面：
    //   切到浏览器/编辑器之后它还浮在上面挡着，非常碍事。
    //   ★ 去掉之后仍要能"叫到前面来" ★ —— 由 showCenteredIn() 里的 raise() +
    //     activateWindow() 负责（从托盘/右键菜单打开面板时走那条路），
    //     所以是"用户一叫就上来、不叫就安分待着"，而不是"打开后永远压着别人"。
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);   // 圆角之外的地方要透出桌面
    setWindowTitle(QStringLiteral("PetPal 面板"));
    // 尺寸是给「每日图片」页留的：这一页要放一张插画，340 高的时候图片区只剩
    // 240px 左右，看着就是个缩略图。加高到 440 之后可用区域约 490x350。
    // 其它页都有 addStretch，跟着变高只是更透气，不会被拉变形。
    resize(BASE_WIN_W, BASE_WIN_H);
    setMinimumSize(BASE_MIN_W, BASE_MIN_H);

    // ---------------- 顶部条 ----------------
    m_topBar = new QWidget(this);
    m_topBar->setObjectName(QStringLiteral("topBar"));
    m_topBar->setFixedHeight(TOP_BAR_H);

    auto* topLay = new QHBoxLayout(m_topBar);
    topLay->setContentsMargins(16, 0, 8, 0);
    topLay->setSpacing(8);

    auto* topTitle = new QLabel(QStringLiteral("PetPal"), m_topBar);
    topTitle->setObjectName(QStringLiteral("topTitle"));
    topLay->addWidget(topTitle);
    topLay->addStretch();

    // 右侧三个窗控按钮，从右往左：关闭、放大/还原、最小化 —— 和 Windows 标题栏同序，
    // 用户不用重新学。都是 22x22 的圆形按钮，只是 hover 配色不同。
    m_minBtn = new QPushButton(QStringLiteral("\u2212"), m_topBar);   // − （减号 U+2212，比 ASCII 的 - 居中好看）
    m_minBtn->setObjectName(QStringLiteral("winBtn"));
    m_minBtn->setFixedSize(22, 22);
    m_minBtn->setCursor(Qt::ArrowCursor);
    m_minBtn->setToolTip(QStringLiteral("最小化到任务栏"));
    topLay->addWidget(m_minBtn);
    connect(m_minBtn, &QPushButton::clicked, this, &QWidget::showMinimized);

    m_maxBtn = new QPushButton(m_topBar);       // 文字在 updateMaxButtonIcon() 里设
    m_maxBtn->setObjectName(QStringLiteral("winBtn"));
    m_maxBtn->setFixedSize(22, 22);
    m_maxBtn->setCursor(Qt::ArrowCursor);
    topLay->addWidget(m_maxBtn);
    connect(m_maxBtn, &QPushButton::clicked, this, &MainPanel::toggleMaximized);

    m_closeBtn = new QPushButton(QStringLiteral("\u00d7"), m_topBar);   // ×
    m_closeBtn->setObjectName(QStringLiteral("closeBtn"));
    m_closeBtn->setFixedSize(22, 22);
    m_closeBtn->setCursor(Qt::ArrowCursor);
    m_closeBtn->setToolTip(QStringLiteral("关闭面板（Esc）"));
    topLay->addWidget(m_closeBtn);
    connect(m_closeBtn, &QPushButton::clicked, this, &QWidget::hide);

    updateMaxButtonIcon();

    // ---------------- 左侧导航 ----------------
    m_nav = new QListWidget(this);
    m_nav->setObjectName(QStringLiteral("nav"));
    m_nav->setFixedWidth(BASE_NAV_W);
    m_nav->setFrameShape(QFrame::NoFrame);
    m_nav->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_nav->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_nav->setFocusPolicy(Qt::NoFocus);            // 焦点别落在导航上，省得 Esc 被它吃掉

    // ---------------- 右侧内容 ----------------
    m_stack = new QStackedWidget(this);
    m_stack->setObjectName(QStringLiteral("stack"));

    // ---------------- 组装 ----------------
    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    body->addWidget(m_nav);
    body->addWidget(m_stack, 1);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(1, 1, 1, 1);          // 留 1px 给外圈描边
    root->setSpacing(0);
    root->addWidget(m_topBar);
    root->addLayout(body, 1);

    // 左侧选中哪一项，右侧就翻到哪一页
    connect(m_nav, &QListWidget::currentRowChanged, m_stack, &QStackedWidget::setCurrentIndex);

    // Esc 关面板。
    // 用 QShortcut 而不是 keyPressEvent：焦点可能落在按钮或列表上，
    // keyPressEvent 有收不到的风险，快捷方式是窗口级的，稳。
    auto* esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    esc->setContext(Qt::WindowShortcut);
    connect(esc, &QShortcut::activated, this, &QWidget::hide);

    applyUiScale();     // 套样式 + 按当前字号档位定尺寸（见下面的说明）
}

// =============================================================================
//  样式表 / 字号档位
//
//  这一节的样式全部包在 UiFont::styleSheet() 里 —— 它会把 `font-size: 12px`
//  按当前档位改写（设置页那个"界面字号"）。档位变了就调 applyUiScale() 重套一遍。
//
//  ★ 为什么尺寸也要跟着档位走，不只是字号 ★
//    只把字放大、框子不动的话，字会从右边顶出去。最先顶不住的是聊天页标题行
//    （"聊天 洛天依 · 心上华海 … 预设台词 · 不联网 · 今天加分还剩 N 次"）——
//    150% 时它比可用宽度长出一截，最后那几个字直接被裁掉，而裁掉的恰好是
//    "还剩几次"这个数字。所以导航宽度和窗口最小尺寸一起按同一个比例放大，
//    窗口会自动长到放得下 —— 这也是主流软件调字号时的行为。
// =============================================================================
void MainPanel::applyUiScale()
{
    applyStyle();

    m_nav->setFixedWidth(UiFont::px(BASE_NAV_W));

    // 最小尺寸按档位放大，但**不能超过屏幕** —— 屏幕不够大时宁可让内容挤一点，
    // 也不能把窗口撑到屏幕外面去（那样连标题栏都点不到了）。
    QSize minSz(UiFont::px(BASE_MIN_W), UiFont::px(BASE_MIN_H));
    QScreen* scr = screen() ? screen() : QGuiApplication::primaryScreen();
    if (scr)
    {
        const QSize avail = scr->availableGeometry().size();
        minSz.setWidth(qMin(minSz.width(),  avail.width()));
        minSz.setHeight(qMin(minSz.height(), avail.height()));
    }
    setMinimumSize(minSz);
}

void MainPanel::applyStyle()
{
    setStyleSheet(UiFont::styleSheet(QStringLiteral(R"(
        QWidget#topBar   { background: transparent; }
        QLabel#topTitle  { color: #888780; font-size: 12px; }
        QPushButton#closeBtn {
            border: none; border-radius: 11px;
            background: transparent; color: #888780; font-size: 15px;
        }
        QPushButton#closeBtn:hover { background: #FCEBEB; color: #A32D2D; }
        QPushButton#winBtn {
            border: none; border-radius: 11px;
            background: transparent; color: #888780; font-size: 13px;
        }
        QPushButton#winBtn:hover { background: #E8E6DF; color: #2C2C2A; }
        QListWidget#nav {
            background: #F1EFE8; border: none; outline: none;
            padding-top: 8px; border-bottom-left-radius: 12px;
        }
        QListWidget#nav::item {
            height: 34px; padding-left: 12px;
            color: #5F5E5A; font-size: 13px;
            border-left: 3px solid transparent;
        }
        QListWidget#nav::item:selected {
            background: #FFFFFF; color: #26215C;
            border-left: 3px solid #7F77DD;
        }
        QStackedWidget#stack { background: #FFFFFF; border-bottom-right-radius: 12px; }
    )")));
}

// =============================================================================
//  自绘圆角卡片
// =============================================================================
void MainPanel::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    QPainterPath path;
    // 铺满屏幕时用直角 + 满画布：还留 12px 圆角的话，四个屏幕角会各透出一块桌面，
    // 看着像"没铺满"。窗口化时让出 0.5px，否则描边会被窗口边界裁掉一半。
    const QRectF frame = m_maximized ? QRectF(rect())
                                     : QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal  radius = m_maximized ? 0.0 : 12.0;
    path.addRoundedRect(frame, radius, radius);

    p.fillPath(path, QColor(0xFF, 0xFF, 0xFF));            // 白底
    p.setPen(QPen(QColor(0xD3, 0xD1, 0xC7), 1.0));         // 一圈浅灰描边
    p.drawPath(path);
}

// =============================================================================
//  加一页
// =============================================================================
void MainPanel::addPage(const QString& title, QWidget* page)
{
    if (!page)
        return;

    m_nav->addItem(title);
    m_stack->addWidget(page);

    // 第一次加页时自动选中，否则右侧会空着
    if (m_nav->currentRow() < 0)
        m_nav->setCurrentRow(0);
}

int MainPanel::pageCount() const
{
    return m_stack ? m_stack->count() : 0;
}

void MainPanel::setCurrentPage(int index)
{
    if (m_nav && index >= 0 && index < m_nav->count())
        m_nav->setCurrentRow(index);   // 导航选中会联动右侧 stack
}

int MainPanel::indexOfPage(QWidget* page) const
{
    return m_stack ? m_stack->indexOf(page) : -1;   // 没加过这页时是 -1
}

// =============================================================================
//  居中显示
// =============================================================================
void MainPanel::showCenteredIn(const QRect& screenRect)
{
    // 从任务栏点回来的情况：先摘掉"最小化"状态，不然 show() 之后它还是缩着的
    if (isMinimized())
        setWindowState(windowState() & ~Qt::WindowMinimized);

    // 放大状态下不重新居中。此时几何是"铺满整屏"，再拿 size() 去算中心点会算到
    // 一个莫名其妙的位置（尺寸本身就已经是全屏了）。
    if (m_maximized)
    {
        show();
        raise();
        activateWindow();
        return;
    }

    const QSize sz = size();
    const QPoint topLeft(screenRect.center().x() - sz.width() / 2,
                         screenRect.center().y() - sz.height() / 2);

    // 屏幕太小就贴左上，别把窗口甩到屏幕外面去
    const QPoint clamped(qBound(screenRect.left(),  topLeft.x(), qMax(screenRect.left(), screenRect.right()  - sz.width())),
                         qBound(screenRect.top(),   topLeft.y(), qMax(screenRect.top(),  screenRect.bottom() - sz.height())));

    move(clamped);
    show();
    raise();
    activateWindow();
}

// =============================================================================
//  放大 / 还原
//
//  注意"铺满"用的是**当前屏幕的可用区**（availableGeometry 已经把任务栏扣掉），
//  不是 whole screen：面板不是播放器，压住任务栏只会让人没法切别的窗口。
//  另外按的是面板自己所在的屏幕 —— 面板被拖到副屏上放大时应该铺满副屏，不是主屏。
// =============================================================================
void MainPanel::toggleMaximized()
{
    if (m_maximized)
    {
        if (m_restoreGeometry.isValid())
            setGeometry(m_restoreGeometry);
        m_maximized = false;
    }
    else
    {
        m_restoreGeometry = geometry();

        QScreen* scr = screen();
        if (!scr)
            scr = QGuiApplication::primaryScreen();
        if (scr)
            setGeometry(scr->availableGeometry());

        m_maximized = true;
    }

    updateMaxButtonIcon();
    update();      // 圆角/直角要跟着重画
}

void MainPanel::updateMaxButtonIcon()
{
    if (!m_maxBtn)
        return;

    if (m_maximized)
    {
        m_maxBtn->setText(QStringLiteral("\u2750"));      // ❐ 两个叠起来的框 = 还原
        m_maxBtn->setToolTip(QStringLiteral("还原窗口大小"));
    }
    else
    {
        m_maxBtn->setText(QStringLiteral("\u25a1"));      // □
        m_maxBtn->setToolTip(QStringLiteral("铺满整个屏幕"));
    }
}

// =============================================================================
//  拖动：只有"顶部条那一条"是把手
//
//  顶部条里的 QLabel / QWidget 默认忽略鼠标事件，事件会冒泡到这里，
//  所以不用给它们装什么事件过滤器。左侧导航和右侧内容区会自己吃掉事件，
//  而且它们本来也不在 y < TOP_BAR_H 的范围里。
//
//  ★ 拖动交给系统去做（startSystemMove），不要自己跟鼠标 ★
//    这是这一页最容易写出"拖起来发涩"的地方，原因是三件事叠在一起：
//      · 这个窗口是 FramelessWindowHint + WA_TranslucentBackground，
//        Windows 上走的是分层窗口（layered window）那条路径 —— 每次 move()
//        都要把整块带 alpha 的内容重新合成一遍；
//      · 鼠标移动事件比合成快得多，一次拖动会来几十上百个 move，
//        每个都触发"重定位 + 整窗重绘 + 重合成"，于是画面开始落后于鼠标；
//      · 自己跟鼠标还丢掉了系统自带的拖动手感（贴边、跨屏、Aero Snap）。
//    startSystemMove() 把这一下交给窗口管理器：它自己进入模态移动循环、
//    只用移动窗口位置（不逐帧重绘内容），跟手度是另一回事。
//    ★ 返回值不看不行 ★ 平台不支持时要退回自己跟鼠标，否则窗口就完全拖不动了。
// =============================================================================
void MainPanel::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && event->position().y() < TOP_BAR_H)
    {
        // winId() 只是确保原生窗口已经建出来（面板是 show() 过的，一般早就有）
        QWindow* handle = windowHandle();
        if (!handle)
        {
            winId();
            handle = windowHandle();
        }

        if (handle && handle->startSystemMove())
        {
            event->accept();
            return;
        }

        // 兜底：系统不支持交互式移动，就还是自己跟鼠标
        m_dragging = true;
        m_dragOffset = event->globalPosition().toPoint() - frameGeometry().topLeft();
        event->accept();
        return;
    }

    QWidget::mousePressEvent(event);
}

void MainPanel::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragging && (event->buttons() & Qt::LeftButton))
    {
        move(event->globalPosition().toPoint() - m_dragOffset);
        event->accept();
        return;
    }

    QWidget::mouseMoveEvent(event);
}

void MainPanel::mouseReleaseEvent(QMouseEvent* event)
{
    m_dragging = false;
    QWidget::mouseReleaseEvent(event);
}
