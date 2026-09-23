#include "MainPanel.h"

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

// =============================================================================
//  构造：搭出"顶部条 + (左侧导航 | 右侧内容)"
// =============================================================================
MainPanel::MainPanel(QWidget* parent) : QWidget(parent)
{
    // 无边框 + 置顶 + 独立窗口。
    // 用 Qt::Window（不是 Qt::Tool）是刻意的：它是"主面板"，在任务栏里占一格
    // 是合理的 —— 用户 Alt+Tab 能找回来。（要改成不占任务栏，换成 Qt::Tool 即可）
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);   // 圆角之外的地方要透出桌面
    setWindowTitle(QStringLiteral("PetPal 面板"));
    // 尺寸是给「每日图片」页留的：这一页要放一张插画，340 高的时候图片区只剩
    // 240px 左右，看着就是个缩略图。加高到 440 之后可用区域约 490x350。
    // 其它页都有 addStretch，跟着变高只是更透气，不会被拉变形。
    resize(640, 440);
    setMinimumSize(560, 400);

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
    m_nav->setFixedWidth(112);
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

    setStyleSheet(QStringLiteral(R"(
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
    )"));
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
// =============================================================================
void MainPanel::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && event->position().y() < TOP_BAR_H)
    {
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
