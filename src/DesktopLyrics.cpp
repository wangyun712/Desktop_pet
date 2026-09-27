#include "DesktopLyrics.h"
#include "UiFont.h"
#include "UiTheme.h"

#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QScreen>
#include <QSettings>

// =============================================================================
//  布局参数（100% 字号档位下的基准值，实际用 UiFont::px() 换算）
//
//  窗口从上到下：[工具条保留区] [当前句] [下一句]。
//  ★ 工具条区域是**常驻保留**的，按钮藏起来时它是透明的 ★ ——
//  这样歌词条的位置永远不动（不会"按钮一出、歌词往下跳一截"），
//  而且鼠标从歌词条往上一挪就能碰到按钮 —— 悬停判定用的也是整窗。
// =============================================================================
namespace {

constexpr int kToolbarH  = 40;   // 工具条保留区高度
constexpr int kPad       = 14;   // 文字四周留白（描边有 4px 宽，留少了会被裁）
constexpr int kRowGap    = 6;    // 当前行与下一行之间
constexpr int kCurFontPx = 26;   // 当前行字号
constexpr int kNextFontPx = 19;  // 下一行字号
constexpr int kRowLead   = 6;    // 一行字的上/下呼吸空隙

constexpr int kPillH     = 32;   // 工具条底板高
constexpr int kBtnSize   = 24;   // 按钮边长
constexpr int kBtnGap    = 6;    // 按钮间距 / 底板内边距
constexpr int kClickMs   = 600;  // 连击窗口（双击/三连击的判定时限）

QSettings ini()
{
    // 和 UiFont / UiTheme 同一个 ui.ini（返回纯右值靠 C++17 保证省略拷贝）
    return QSettings(QSettings::IniFormat, QSettings::UserScope,
                     QStringLiteral("PetPal"), QStringLiteral("ui"));
}

QFont lineFont(int basePx)
{
    QFont f;
    f.setPixelSize(qMax(12, UiFont::px(basePx)));
    f.setBold(true);
    return f;
}

int rowHeight(int basePx)
{
    return QFontMetrics(lineFont(basePx)).height() + kRowLead;
}

} // namespace

DesktopLyrics::DesktopLyrics(QWidget* parent)
    : QWidget(parent)
{
    // 置顶 + 无边框 + 透明：桌面歌词的三个标准件。
    // Qt::Tool = 不在任务栏占格（它不是"程序"，是桌面上的一层字）。
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    // ★ 绝不抢焦点 ★ —— 用户多半在别的窗口里打字做事，
    // 歌词窗一出现就把前台抢走等于替用户按了 Alt。
    setAttribute(Qt::WA_ShowWithoutActivating);
    setWindowTitle(QStringLiteral("桌面歌词"));
    // 鼠标移进移出要能收到（工具条的显隐靠它），别让 Qt 省略 enter/leave
    setAttribute(Qt::WA_Hover);

    // 锁定状态上次开着的就保持 —— 和网易云一样，锁了不会自己弹开
    m_locked = ini().value(QStringLiteral("desktopLyrics/locked"), false).toBool();

    // 先定尺寸、再回位置 —— 位置夹取要用到窗口宽高
    buildFixedSize();
    restorePosition();
}

// =============================================================================
//  固定条带尺寸
//
//  ★ 窗口尺寸不跟歌词走，这是网易云桌面歌词的核心机制 ★
//    宽度 = 所在屏幕可用宽的 60%（收口在 [420, 1100] 基准像素，随字号档位
//    缩放）。定了就不变：换行只是原地换字（超长用"…"截断），没有任何窗口
//    重配置 —— 之前"按文本改尺寸"的路线，哪怕合成一次 setGeometry，分层
//    窗口重配置时旧内容仍会被系统拉伸一帧，肉眼看就是闪。固定尺寸从根上
//    消掉了这个过程，中心轴也天然钉死。
//    整个会话只有两处会重建尺寸：启动，和字号档位变化（罕见）。
// =============================================================================
void DesktopLyrics::buildFixedSize()
{
    // 认一下自己该在哪块屏上：优先按存档位置找屏，找不到用主屏
    const QSettings s = ini();
    const QPoint savedPos(s.value(QStringLiteral("desktopLyrics/x"), 120).toInt(),
                          s.value(QStringLiteral("desktopLyrics/y"), 120).toInt());
    QScreen* scr = QGuiApplication::screenAt(savedPos);
    if (!scr)
        scr = QGuiApplication::primaryScreen();

    const int availW = scr ? scr->availableGeometry().width() : 1200;
    const int w = qBound(UiFont::px(420), int(availW * 0.6), UiFont::px(1100));
    const int h = kToolbarH + kPad
                  + rowHeight(kCurFontPx) + kRowGap + rowHeight(kNextFontPx)
                  + kPad;

    m_builtScale = UiFont::scalePercent();
    resize(w, h);
}

// =============================================================================
//  喂歌词 / 播放状态
// =============================================================================
void DesktopLyrics::setLine(const QString& current, const QString& next)
{
    // 文本没变就是空操作 —— PlayerPage 那边 200ms 轮询一次，
    // 每帧都重绘一遍是白白耗电。
    if (m_cur == current && m_next == next)
        return;

    m_cur  = current;
    m_next = next;

    if (m_cur.isEmpty())
    {
        // 没在唱（没歌/没歌词/开关关了）→ 整窗收起来，不在桌面上留一块空壳
        hide();
        return;
    }

    // ★ 不改窗口几何 ★ —— 条带是固定宽的，换行只是原地换字（超长截断）。
    // 唯一的例外：字号档位变了，重建一次尺寸（整个会话难得一遇）。
    if (m_builtScale != UiFont::scalePercent())
        buildFixedSize();

    if (!isVisible())
        show();
    update();
}

void DesktopLyrics::setPlaying(bool playing)
{
    if (m_playing == playing)
        return;
    m_playing = playing;
    update();                          // 播放键图标要换（三角 / 双竖条）
}

// =============================================================================
//  位置记忆
// =============================================================================
void DesktopLyrics::restorePosition()
{
    const QSettings s = ini();
    QPoint pos(s.value(QStringLiteral("desktopLyrics/x"), 120).toInt(),
               s.value(QStringLiteral("desktopLyrics/y"), 120).toInt());

    // 夹回屏幕里：换过显示器、改过分辨率、或者存档被手改坏，
    // 都不能让歌词窗找不回来。先认"点落在哪块屏"，认不出就用主屏。
    QScreen* scr = QGuiApplication::screenAt(pos);
    if (!scr)
        scr = QGuiApplication::primaryScreen();
    if (scr)
    {
        const QRect avail = scr->availableGeometry();
        pos.setX(qBound(avail.left(), pos.x(), qMax(avail.left(), avail.right()  - width())));
        pos.setY(qBound(avail.top(),  pos.y(), qMax(avail.top(),  avail.bottom() - height())));
    }
    move(pos);
}

void DesktopLyrics::savePosition()
{
    QSettings s = ini();
    s.setValue(QStringLiteral("desktopLyrics/x"), x());
    s.setValue(QStringLiteral("desktopLyrics/y"), y());
}

// =============================================================================
//  绘制
// =============================================================================
void DesktopLyrics::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    // ★ 整窗铺一层 alpha=1 的底 —— 这就是那个"大判定框" ★
    //   没有它，工具条藏起来时那块区域是全透明像素，而 Windows 对透明
    //   分层窗口的命中测试按**像素 alpha** 走：alpha=0 的地方鼠标事件直接
    //   穿到下层窗口。表现为"鼠标从歌词挪向按钮，刚出文字区就触发 leave、
    //   工具条收起"—— 按钮永远悬不上、点不到。
    //   铺上这层（1/255 透明度，肉眼不可见）后，整个窗口矩形就是一个实心
    //   判定框：歌词、工具条、中间的空隙全在里面，鼠标随便走都不会掉出去。
    //
    //   ★ 但锁定时要**反过来**：不铺这层 ★
    //   锁定 = 鼠标穿透 —— 按像素 alpha 命中的规则下，撤掉这层后只剩
    //   歌词笔画像素（alpha>0）能接住鼠标：双击笔画解锁仍然可行，
    //   笔画之间的空白则全部穿透到下层窗口（用户的要求）。
    if (!m_locked)
        p.fillRect(rect(), QColor(0, 0, 0, 1));

    if (toolbarVisible())
        paintToolbar(p);

    // ---- 歌词两行（描边字，压得住任何壁纸）----
    // 条带是固定宽的：放得下就整句居中，放不下就"…"截断（网易云同款）。
    // 文字永远以条带中心轴居中 —— 轴是窗口的，窗口又从不移动。
    const int maxTextW = width() - 2 * kPad;

    int y = kToolbarH + kPad;
    if (!m_cur.isEmpty())
    {
        const QFont f = lineFont(kCurFontPx);
        const QFontMetrics fm(f);
        const QString text = fm.elidedText(m_cur, Qt::ElideRight, maxTextW);
        QPainterPath path;
        path.addText((width() - fm.horizontalAdvance(text)) / 2,
                     y + fm.ascent(), f, text);
        // 深色描边垫底、主题强调色填充盖面 —— 先描后填，描边只露外面半圈
        p.strokePath(path, QPen(QColor(0, 0, 0, 170), 4.0,
                                Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.fillPath(path, UiTheme::color(UiTheme::Accent));
    }

    y += rowHeight(kCurFontPx) + kRowGap;
    if (!m_next.isEmpty())
    {
        const QFont f = lineFont(kNextFontPx);
        const QFontMetrics fm(f);
        const QString text = fm.elidedText(m_next, Qt::ElideRight, maxTextW);
        QPainterPath path;
        path.addText((width() - fm.horizontalAdvance(text)) / 2,
                     y + fm.ascent(), f, text);
        p.strokePath(path, QPen(QColor(0, 0, 0, 170), 4.0,
                                Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.fillPath(path, QColor(0xE6, 0xE4, 0xDC));
    }
}

// =============================================================================
//  工具条
// =============================================================================
bool DesktopLyrics::toolbarVisible() const
{
    // 锁定时永远不出现 —— 锁的就是"别再跟我互动"
    return m_hovered && !m_locked;
}

QRect DesktopLyrics::pillRect() const
{
    const int w = BtnCount * kBtnSize + (BtnCount + 1) * kBtnGap;
    return QRect((width() - w) / 2, (kToolbarH - kPillH) / 2, w, kPillH);
}

QRect DesktopLyrics::btnRect(int which) const
{
    const QRect pr = pillRect();
    return QRect(pr.left() + kBtnGap + which * (kBtnSize + kBtnGap),
                 pr.top() + (kPillH - kBtnSize) / 2, kBtnSize, kBtnSize);
}

int DesktopLyrics::btnAt(const QPoint& pos) const
{
    for (int i = 0; i < BtnCount; ++i)
    {
        if (btnRect(i).contains(pos))
            return i;
    }
    return -1;
}

void DesktopLyrics::paintToolbar(QPainter& p)
{
    const QRect pr = pillRect();

    // 底板：白色小圆条 —— 无论底下壁纸什么颜色，按钮都可读
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 235));
    p.drawRoundedRect(pr, 16, 16);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 45), 1));
    p.drawRoundedRect(QRectF(pr).adjusted(0.5, 0.5, -0.5, -0.5), 16, 16);

    const QColor glyph = UiTheme::color(UiTheme::AccentDeep);
    for (int i = 0; i < BtnCount; ++i)
    {
        const QRect r = btnRect(i);
        if (i == m_hoverBtn)
        {
            // 悬停的按钮垫一个强调色圆底 —— 和面板里按钮 hover 一个思路
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(UiTheme::color(UiTheme::Accent).red(),
                              UiTheme::color(UiTheme::Accent).green(),
                              UiTheme::color(UiTheme::Accent).blue(), 40));
            p.drawEllipse(r.adjusted(1, 1, -1, -1));
        }
        drawGlyph(p, i, r, glyph);
    }
}

void DesktopLyrics::drawGlyph(QPainter& p, int which, const QRect& r,
                              const QColor& color) const
{
    // 图标和播放器页那套是一个语言：矢量几何形，不依赖字体字符
    QPainterPath path;
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.setRenderHint(QPainter::Antialiasing, true);

    switch (which)
    {
    case BtnPrev:
    {
        // |◀：左边的竖条 + 向左的三角
        path.addRoundedRect(QRectF(r.left() + 5, r.top() + 6.5, 2.5, 11), 1, 1);
        QPolygonF tri;
        tri << QPointF(r.right() - 6.5, r.top() + 5.5)
            << QPointF(r.right() - 6.5, r.bottom() - 5.5)
            << QPointF(r.left() + 8.5,  r.center().y());
        path.addPolygon(tri);
        break;
    }
    case BtnPlay:
        if (m_playing)
        {
            // 播放中 → 显示"暂停"（两根竖条）
            path.addRoundedRect(QRectF(r.left() + 6, r.top() + 6, 3.5, 12), 1.5, 1.5);
            path.addRoundedRect(QRectF(r.right() - 9.5, r.top() + 6, 3.5, 12), 1.5, 1.5);
        }
        else
        {
            // 暂停中 → 显示"播放"（向右三角）
            QPolygonF tri;
            tri << QPointF(r.left() + 7,  r.top() + 5.5)
                << QPointF(r.left() + 7,  r.bottom() - 5.5)
                << QPointF(r.right() - 6, r.center().y());
            path.addPolygon(tri);
        }
        break;
    case BtnNext:
    {
        // ▶|：向右的三角 + 右边的竖条
        QPolygonF tri;
        tri << QPointF(r.left() + 6.5, r.top() + 5.5)
            << QPointF(r.left() + 6.5, r.bottom() - 5.5)
            << QPointF(r.right() - 8.5, r.center().y());
        path.addPolygon(tri);
        path.addRoundedRect(QRectF(r.right() - 7.5, r.top() + 6.5, 2.5, 11), 1, 1);
        break;
    }
    case BtnLock:
    {
        // 挂锁：锁梁（弧线描）+ 锁体（圆角方块填）
        QPainterPath shackle;
        shackle.moveTo(r.left() + 7.5, r.top() + 11);
        shackle.lineTo(r.left() + 7.5, r.top() + 8);
        shackle.arcTo(QRectF(r.left() + 7.5, r.top() + 4, 9, 8), 180, -180);
        shackle.lineTo(r.right() - 7.5, r.top() + 11);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(color, 2.0, Qt::SolidLine, Qt::RoundCap));
        p.drawPath(shackle);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        path.addRoundedRect(QRectF(r.left() + 5.5, r.top() + 11, 13, 8.5), 2, 2);
        break;
    }
    case BtnClose:
    {
        // ×：两条对角线（描线，不参与下面的 path 填充）
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(color, 2.2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(r.left() + 7, r.top() + 7), QPointF(r.right() - 7, r.bottom() - 7));
        p.drawLine(QPointF(r.right() - 7, r.top() + 7), QPointF(r.left() + 7, r.bottom() - 7));
        break;
    }
    default:
        break;
    }

    if (!path.isEmpty())
        p.drawPath(path);
}

// =============================================================================
//  悬停 / 拖动 / 点击
// =============================================================================
void DesktopLyrics::enterEvent(QEnterEvent* event)
{
    QWidget::enterEvent(event);
    m_hovered = true;
    update();                          // 工具条浮现
}

void DesktopLyrics::leaveEvent(QEvent* event)
{
    QWidget::leaveEvent(event);
    m_hovered  = false;
    m_hoverBtn = -1;
    update();                          // 工具条收起
}

void DesktopLyrics::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton)
    {
        QWidget::mousePressEvent(event);
        return;
    }

    // 工具条按钮：按下即生效（小窗没有"按下/松开"的仪式感必要）
    if (toolbarVisible())
    {
        const int btn = btnAt(event->pos());
        switch (btn)
        {
        case BtnPrev:  emit prevRequested();        return;
        case BtnPlay:  emit playPauseRequested();   return;
        case BtnNext:  emit nextRequested();        return;
        case BtnLock:
            m_locked = true;
            ini().setValue(QStringLiteral("desktopLyrics/locked"), true);
            m_hoverBtn = -1;
            update();                   // 工具条立刻收起
            return;
        case BtnClose:
            emit closeRequested();      // 整个功能关掉（PlayerPage 落盘 + 收窗，
            return;                     // 设置页勾选框同步取消；重开回设置页勾选）
        default:
            break;                      // 点在按钮外面 → 掉到下面的拖拽/连击逻辑
        }
    }

    // 连击计数（双击解锁 / 三连击唤面板）—— 锁定与否都要数
    countClick();

    if (!m_locked)
    {
        m_dragging   = true;
        m_dragOffset = event->globalPosition().toPoint() - frameGeometry().topLeft();
    }
    event->accept();
}

void DesktopLyrics::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragging && (event->buttons() & Qt::LeftButton))
    {
        move(event->globalPosition().toPoint() - m_dragOffset);
        event->accept();
        return;
    }

    // 悬停高亮跟手：指到哪个按钮哪个亮，顺便把手型光标挂上
    const int btn = toolbarVisible() ? btnAt(event->pos()) : -1;
    if (btn != m_hoverBtn)
    {
        m_hoverBtn = btn;
        update();
    }
    setCursor(btn >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    event->accept();
}

void DesktopLyrics::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_dragging)
    {
        m_dragging = false;
        savePosition();                // 松手就记，中途关程序位置也不丢
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void DesktopLyrics::mouseDoubleClickEvent(QMouseEvent* event)
{
    // 双击不再做"跳转"（这一层只显示）——它只参与连击计数：
    // 锁定状态下双击 = 解锁。不该与拖拽冲突：双击本身没有位移。
    if (!toolbarVisible() || btnAt(event->pos()) < 0)
        countClick();
    event->accept();
}

// =============================================================================
//  连击计数：双击解锁 / 三连击唤出主面板
// =============================================================================
void DesktopLyrics::countClick()
{
    // 超出连击窗口就从头数（QElapsedTimer 第一次用时 isValid 为假，正好当作"第一次点"）
    if (!m_clickClock.isValid() || m_clickClock.elapsed() > kClickMs)
        m_clickCount = 0;
    m_clickClock.restart();
    ++m_clickCount;

    if (m_clickCount == 2 && m_locked)
    {
        // 双击解锁：锁定状态记回存档，工具条立刻可用（鼠标就在窗上）
        m_locked = false;
        ini().setValue(QStringLiteral("desktopLyrics/locked"), false);
        update();
    }
    else if (m_clickCount == 3)
    {
        // 三连击：把主面板叫出来（DesktopPet 接了这个信号）
        m_clickCount = 0;
        emit panelRequested();
    }
}

// =============================================================================
//  开关存取（设置页写、播放器页读）
// =============================================================================
bool DesktopLyrics::loadEnabled()
{
    const QSettings s = ini();
    return s.value(QStringLiteral("desktopLyrics/enabled"), false).toBool();
}

void DesktopLyrics::saveEnabled(bool on)
{
    QSettings s = ini();
    s.setValue(QStringLiteral("desktopLyrics/enabled"), on);
}
