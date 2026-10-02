#include "MainPanel.h"
#include "UiFont.h"
#include "UiTheme.h"

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
#include <QCloseEvent>
#include <QShortcut>
#include <QFile>
#include <QScreen>
#include <QGuiApplication>
#include <QWindow>          // startSystemMove()（见 mousePressEvent 的说明）
#include <QTimer>
#include <QVariantAnimation>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <functional>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

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

// =============================================================================
//  网络状态徽章 —— 顶部条 "PetPal" 右边的那颗小胶囊
//
//  长相：圆角胶囊底 + 一颗状态点 + 两个字（在线 / 离线 / 检测中）。
//    · 在线   —— 绿点带一圈向外扩散的"呼吸环"，绿底绿字；
//    · 离线   —— 静止的红点（断网没什么可"活"的，动效留给恢复），红底红字；
//    · 检测中 —— 灰点带呼吸环，程序刚启动、第一次探测还没回来时的过渡态。
//  呼吸环的用意：绿色说明"现在是好的"，但它是不是"刚刚才变好/正在波动"，
//  静态图看不出来 —— 让环按探测节奏轻轻扩散，状态一眼就是"活的"。
//
//  实现是自绘而不是 QSS + 两个 QLabel：
//    · 点、环、胶囊的几何要互相咬合（环从点的边缘长出去），QSS 拼不出这种细节；
//    · 尺寸要跟字号档位走（UiFont::px），自绘时一处 sizeHint 就算清了。
//  不吃鼠标事件：默认 QWidget 会忽略鼠标按下并冒泡给顶部条（见 mousePressEvent
//  的注释），拖动不受影响；只挂了 tooltip。
//
//  ★ 这两个类放在全局作用域而不是匿名命名空间 ★
//    MainPanel.h 里前置声明了 NetBadge（成员指针得有类型名），匿名命名空间里的
//    同名类会和它撞成"不明确的符号"（MSVC C2872）。反正只在本文件里定义和使用，
//    不会污染别的编译单元。
// =============================================================================
class NetBadge : public QWidget
{
public:
    enum class State { Checking, Online, Offline };

    explicit NetBadge(QWidget* parent) : QWidget(parent)
    {
        // 呼吸动画：0→1 线性循环，paintEvent 里换算成扩散环的半径和透明度。
        // 只有几十像素宽的控件自己重画自己，开销可以忽略。
        m_pulse = new QVariantAnimation(this);
        m_pulse->setStartValue(0.0);
        m_pulse->setEndValue(1.0);
        m_pulse->setDuration(2200);
        m_pulse->setLoopCount(-1);
        connect(m_pulse, &QVariantAnimation::valueChanged, this, [this] { update(); });

        setState(State::Checking);
        refreshScale();
    }

    void setState(State s)
    {
        if (m_state == s)
            return;
        m_state = s;

        setToolTip(stateTip(s));

        // 离线时把动画停掉 —— 红点是静止的；在线/检测中都让它呼吸。
        if (s == State::Offline)
            m_pulse->stop();
        else
            m_pulse->start();

        refreshScale();     // 文案宽度变了（"检测中…" vs "在线"），胶囊要跟着伸缩
    }

    // 字号档位变了（或状态文案变了）之后重算胶囊尺寸。applyUiScale() 会来调。
    void refreshScale()
    {
        setFixedSize(sizeHint());
        update();
    }

protected:
    QSize sizeHint() const override
    {
        QFont f = badgeFont();
        const int textW = QFontMetrics(f).horizontalAdvance(stateText(m_state));
        const int w = UiFont::px(9) * 2   // 胶囊左右内边距
                    + UiFont::px(6)       // 状态点直径
                    + UiFont::px(5)       // 点和文字的间距
                    + textW + 2;          // +2 给一圈 1px 描边
        const int h = UiFont::px(20);
        return QSize(w, h);
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::TextAntialiasing, true);

        p.setFont(badgeFont());

        const int  padH = UiFont::px(9);
        const int  dotD = UiFont::px(6);
        const int  gap  = UiFont::px(5);
        const QRectF pill(0.5, 0.5, width() - 1.0, height() - 1.0);

        // ---- 胶囊底 + 描边（浅色实底压得住背景图，深色字在图上也不花）----
        p.setPen(QPen(skinBorder(m_state), 1.0));
        p.setBrush(skinFill(m_state));
        p.drawRoundedRect(pill, pill.height() / 2.0, pill.height() / 2.0);

        // ---- 状态点 ----
        const QPointF dotC(padH + dotD / 2.0, height() / 2.0);

        // 呼吸环：从点的边缘向外扩散、越扩越淡，到头重来。离线不画（静止）。
        if (m_state != State::Offline)
        {
            const qreal ph  = m_pulse->currentValue().toReal();
            const qreal rr  = dotD / 2.0 + 1.0 + ph * UiFont::px(4);
            QColor halo = skinDot(m_state);
            halo.setAlpha(int((1.0 - ph) * 140.0));
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(halo, UiFont::px(2)));
            p.drawEllipse(dotC, rr, rr);
        }

        p.setPen(Qt::NoPen);
        p.setBrush(skinDot(m_state));
        p.drawEllipse(dotC, dotD / 2.0, dotD / 2.0);

        // ---- 文字 ----
        p.setPen(skinText(m_state));
        p.drawText(QRect(padH + dotD + gap, 0, width() - padH - dotD - gap, height()),
                   Qt::AlignVCenter | Qt::AlignLeft, stateText(m_state));
    }

private:
    static QFont badgeFont()
    {
        // 自绘控件不走 QSS，字号档位得自己套：11px 基准 × 当前档位。
        QFont f;
        f.setPixelSize(UiFont::px(11));
        f.setWeight(QFont::DemiBold);   // 小字号加半粗，两个字才立得住
        return f;
    }

    static QString stateText(State s)
    {
        switch (s)
        {
        case State::Online:  return QStringLiteral("在线");
        case State::Offline: return QStringLiteral("离线");
        case State::Checking: break;
        }
        return QStringLiteral("检测中…");
    }

    static QString stateTip(State s)
    {
        switch (s)
        {
        case State::Online:  return QStringLiteral("网络连接正常");
        case State::Offline: return QStringLiteral("无网络连接，正在自动重试…");
        case State::Checking: break;
        }
        return QStringLiteral("正在检测网络…");
    }

    // 配色跟面板现有元素对齐：离线红取 × 按钮 hover 的 #A32D2D，
    // 灰取标题的 #888780 一族；胶囊用浅色实底，压得住背景图。
    static QColor skinFill(State s)
    {
        switch (s)
        {
        case State::Online:  return QColor(226, 244, 232, 215);
        case State::Offline: return QColor(252, 235, 235, 215);   // closeBtn hover 同色 #FCEBEB
        case State::Checking: break;
        }
        return QColor(238, 236, 230, 215);
    }

    static QColor skinBorder(State s)
    {
        switch (s)
        {
        case State::Online:  return QColor(96, 190, 134, 210);
        case State::Offline: return QColor(224, 106, 106, 210);
        case State::Checking: break;
        }
        return QColor(170, 168, 158, 210);
    }

    static QColor skinDot(State s)
    {
        switch (s)
        {
        case State::Online:  return QColor(0x2E, 0xA4, 0x5C);
        case State::Offline: return QColor(0xD6, 0x45, 0x45);
        case State::Checking: break;
        }
        return QColor(0x8F, 0x8E, 0x86);
    }

    static QColor skinText(State s)
    {
        switch (s)
        {
        case State::Online:  return QColor(0x1D, 0x7A, 0x44);
        case State::Offline: return QColor(0xA3, 0x2D, 0x2D);     // closeBtn hover 同色
        case State::Checking: break;
        }
        return QColor(0x6B, 0x6A, 0x64);
    }

    QVariantAnimation* m_pulse = nullptr;
    State              m_state = State::Checking;
};

// =============================================================================
//  网络探测 —— 徽章的数据源
//
//  ★ 为什么自己发请求，不用 QNetworkInformation ★
//    那套 API 只反映"操作系统觉得有没有网"，还得带上对应的平台插件 DLL 一起
//    部署；而且测不出"Wi-Fi 连着但路由器没外网"这种最常见的假在线。
//    本程序里真正吃网络的就是音乐一条线（联网搜索、在线播放、下载，
//    见 OnlineMusic / AudioPlayer）；聊天（预设台词）和每日图片（本地
//    resources/daily_image）都是离线功能，不依赖这颗徽章。
//    "能不能上网"就用"真的去连一次"来回答 —— 徽章说在线，
//    至少音乐搜索/在线播放此刻是能用的。
//
//  节奏：在线时 10s 一轮；一旦判离线，收紧到 5s 一轮，恢复能快点被看到。
//  一轮按顺序试下面的端点，任一成功即在线（都是连通性检查专用地址，响应只有
//  几十字节，不带任何内容，流量可忽略）。单次请求 5s 超时，全失败判离线。
//  探测全程异步：构造里发出第一发就返回，不拖面板的启动。
// =============================================================================
class NetWatcher : public QObject
{
public:
    explicit NetWatcher(std::function<void(bool)> onResult, QObject* parent = nullptr)
        : QObject(parent), m_onResult(std::move(onResult))
    {
        m_nam = new QNetworkAccessManager(this);
        m_retry.setSingleShot(true);
        connect(&m_retry, &QTimer::timeout, this, [this] { probe(); });
        QTimer::singleShot(250, this, [this] { probe(); });   // 等事件循环转起来再发第一轮
    }

private:
    void probe()
    {
        m_next = 0;
        sendOne();
    }

    void sendOne()
    {
        if (m_next >= m_urls.count())
        {
            report(false);      // 几个端点都不通，真离线
            return;
        }

        QNetworkRequest req(QUrl(m_urls.at(m_next++)));
        req.setTransferTimeout(5000);   // 卡住的连接 5s 掐掉，别拖住下一轮
        req.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);   // 探测就别吃缓存了
        QNetworkReply* reply = m_nam->get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply] { onOneDone(reply); });
    }

    void onOneDone(QNetworkReply* reply)
    {
        reply->deleteLater();
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() == QNetworkReply::NoError && code >= 200 && code < 400)
        {
            report(true);
            return;
        }
        sendOne();      // 这个端点不行，换下一个
    }

    void report(bool online)
    {
        if (m_onResult)
            m_onResult(online);
        m_retry.start(online ? 10000 : 5000);
    }

    QNetworkAccessManager*    m_nam      = nullptr;
    QTimer                    m_retry;
    std::function<void(bool)> m_onResult;
    int                       m_next     = 0;

    // 主用 MIUI 的 generate_204（国内快、响应 204 无正文），
    // 备用 Windows 自带的 NCSI 探测地址（微软自家连网检测用的那条）。
    const QStringList m_urls = {
        QStringLiteral("https://connect.rom.miui.com/generate_204"),
        QStringLiteral("http://www.msftconnecttest.com/connecttest.txt"),
    };
};

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
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint
                 | Qt::WindowMinimizeButtonHint);   // 声明"支持最小化"（真正的开关在下面补的原生位）
    setAttribute(Qt::WA_TranslucentBackground);   // 圆角之外的地方要透出桌面
    setWindowTitle(QStringLiteral("PetPal 面板"));

#ifdef Q_OS_WIN
    // ★ 无边框窗口要自己补 WS_MINIMIZEBOX，任务栏的"点一下最小化 / 再点一下复原"才生效 ★
    //   无边框窗口在系统里是 WS_POPUP，没有 WS_MINIMIZEBOX 这一位时，Shell 对任务栏
    //   图标的点击只会"激活窗口"，不发 SC_MINIMIZE/SC_RESTORE —— 用户点第二下什么
    //   都不会发生。这里在 winId() 建好原生窗口之后把这一位 OR 上去，之后系统按
    //   标准窗口行为原生完成切换：任务栏图标只管面板自己（最小化/复原面板），
    //   桌宠是另一个独立窗口，不受它影响（用户明确要求，2026-09-28）。
    //   只补 MINIMIZEBOX 不补 CAPTION，系统不会因此画出标题栏，面板还是无边框的样子。
    {
        const HWND hwnd = reinterpret_cast<HWND>(winId());
        if (hwnd)
            ::SetWindowLongPtrW(hwnd, GWL_STYLE,
                                ::GetWindowLongPtrW(hwnd, GWL_STYLE) | WS_MINIMIZEBOX);
    }
#endif
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

    // 网络状态徽章：标题右边的绿/红小胶囊（见上面 NetBadge 的说明）。
    // 数据来自下面的 NetWatcher —— 真·发一次请求来判定在线/离线。
    m_netBadge = new NetBadge(m_topBar);
    topLay->addWidget(m_netBadge);

    topLay->addStretch();

    new NetWatcher([this](bool online)
    {
        if (m_netBadge)
            m_netBadge->setState(online ? NetBadge::State::Online
                                        : NetBadge::State::Offline);
    }, this);

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

    // 徽章是自绘的，不吃 QSS 的字号改写，尺寸得自己跟着档位走
    if (m_netBadge)
        m_netBadge->refreshScale();

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
        QStackedWidget#stack { background: transparent; border-bottom-right-radius: 12px; }
    )")));
}

// =============================================================================
//  自绘圆角卡片
//
//  三层，从下往上：
//    ① 主题底色（UiTheme::PanelBg）—— 没设背景图时看到的就是它；
//    ② 背景图（设置页导入的那张）：按"盖满"缩放铺住圆角框，多出部分裁掉，
//       以用户调的不透明度叠在底色上 —— 0% 等于没有图，100% 是原图本色；
//    ③ 一圈描边（主题 Disabled 色，就是改造前那圈浅灰）。
//  卡片/导航这些角色带一点透明（见 UiTheme.cpp 顶上的说明），图会从它们
//  底下隐约透出来；页面 QSS 不用关心这件事。
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

    // ---- ① 主题底色 ----
    p.fillPath(path, UiTheme::color(UiTheme::PanelBg));

    // ---- ② 背景图 ----
    // 路径变了才重新读盘（切图/清图的时候），平时重画只动用缓存 ——
    // 不透明度滑条拖动时每 tick 都会走到这里，不能每次都去磁盘。
    const QString bgPath = UiTheme::bgImagePath();
    if (m_bgPath != bgPath)
    {
        m_bgPath = bgPath;
        m_bgImg  = (bgPath.isEmpty() || !QFile::exists(bgPath)) ? QPixmap() : QPixmap(bgPath);
    }

    if (!m_bgImg.isNull() && UiTheme::bgOpacityF() > 0.0)
    {
        const QRectF box     = path.boundingRect();
        const QSizeF imgSize(m_bgImg.size());
        // 盖满（cover）：取两个方向里更放大的那个比例。图一定铺满框，
        // 既不留白边也不拉变形，多出来的部分被圆角框裁掉。
        const qreal  scale   = qMax(box.width() / imgSize.width(),
                                    box.height() / imgSize.height());
        const QRectF target(box.left()   + (box.width()  - imgSize.width()  * scale) / 2.0,
                            box.top()    + (box.height() - imgSize.height() * scale) / 2.0,
                            imgSize.width()  * scale,
                            imgSize.height() * scale);

        p.save();
        p.setClipPath(path);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.setOpacity(UiTheme::bgOpacityF());
        p.drawPixmap(target, m_bgImg, QRectF(m_bgImg.rect()));
        p.restore();
    }

    // ---- ③ 描边 ----
    p.setPen(QPen(UiTheme::color(UiTheme::Disabled), 1.0));   // 一圈浅灰描边
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
//  关闭 = 藏起来，绝不退出程序
//
//  ★ 为什么必须拦 ★ 无边框窗口补了 WS_MINIMIZEBOX 之后，任务栏图标右键
//  菜单里的「关闭窗口」、Alt+F4 都会把真正的关闭命令（WM_CLOSE / SC_CLOSE）
//  送进面板。放任默认处理的话面板会被 close() —— 而桌宠是 Qt::Tool 小窗，
//  拦不住"最后一个主窗口关闭"的退出逻辑，整个程序跟着退出：用户只是想关个
//  面板，桌宠也一起没了（用户明确要求两者互不相干，2026-10-01）。
//  这里一律 ignore + hide()，和右上角 × 完全同路。真正的退出走托盘/右键
//  菜单的「退出」（QApplication::quit，不经过 closeEvent，不受影响）。
// =============================================================================
void MainPanel::closeEvent(QCloseEvent* event)
{
    event->ignore();
    hide();
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
