#include "PlayerPage.h"

#include "AudioPlayer.h"
#include "DesktopLyrics.h"
#include "MfDecode.h"
#include "OnlineMusic.h"
#include "TrackListModel.h"
#include "UiFont.h"
#include "UiTheme.h"

#include <QBuffer>
#include <algorithm>
#include <QButtonGroup>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QPropertyAnimation>
#include <QEasingCurve>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSlider>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>

#include <utility>

// -----------------------------------------------------------------------------
//  歌词高亮用的强调色（主题的深强调角色，和样式表里那套 accent 是一组，
//  深一档好压住灰底）。★ 是函数不是常量 ★ —— 主题切换后颜色要跟着变，
//  常量只在进程启动时求一次值，换主题就成"样式表变了、歌词还是旧色"。
//  ★ 放在文件顶上 ★ 列表委托和歌词样式拼接两处都要用，得先于它们声明。
// -----------------------------------------------------------------------------
QColor accentDeepColor()
{
    return UiTheme::color(UiTheme::AccentDeep);
}

// -----------------------------------------------------------------------------
//  SeekSlider —— "点哪跳哪"的进度条
//
//  ★ 为什么要自己写 ★
//    Qt 默认的 QSlider 点一下轨道只走一页（pageStep），想跳到某处必须
//    精准按住那个小圆点拖 —— 对播放器来说这是反直觉的。
//    这里重写三个鼠标事件：按下就跳到点击位置，按住拖动就跟着走，
//    松手汇报"拖完了"。全程不走基类那套 pressedControl 逻辑，
//    所以不会出现"点了以后位置反而被基类改回去"。
//
//  ★ 必须定义在**全局作用域**，不能塞进下面的匿名 namespace ★
//    PlayerPage.h 里写的是 `class SeekSlider;`（全局的前向声明）。
//    如果这里把它定义在匿名 namespace 里，那就是**另一个类型**了 ——
//    `m_slider = new SeekSlider(...)` 会因为类型不兼容直接编译不过。
// -----------------------------------------------------------------------------
class SeekSlider : public QSlider
{
public:
    explicit SeekSlider(Qt::Orientation o, QWidget* parent = nullptr) : QSlider(o, parent) {}

protected:
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton)
        {
            QSlider::mousePressEvent(e);
            return;
        }
        m_dragging = true;
        emit sliderPressed();
        applyFromX(e->position().x());
        e->accept();
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!m_dragging)
        {
            QSlider::mouseMoveEvent(e);
            return;
        }
        applyFromX(e->position().x());
        e->accept();
    }

    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (!m_dragging)
        {
            QSlider::mouseReleaseEvent(e);
            return;
        }
        m_dragging = false;
        applyFromX(e->position().x());
        emit sliderReleased();
        e->accept();
    }

private:
    void applyFromX(qreal x)
    {
        const double ratio = qBound(0.0, x / double(qMax(1, width())), 1.0);
        setValue(minimum() + int(ratio * double(maximum() - minimum())));
        emit sliderMoved(value());
    }

    bool m_dragging = false;
};

// -----------------------------------------------------------------------------
//  PlayerIconButton —— 播放条上那三个按钮（上一首 / 播放-暂停 / 下一首）
//
//  ★ 为什么不用字符画 ★
//    原先是把 ▶ / ⏸ / ⏮ / ⏭ 这些字符写到按钮文字上，靠系统字体渲染。三个毛病：
//      · 字形随机器变 —— ⏸ 在有些字体里回落成豆腐块，有的干脆画成彩色 emoji；
//      · 光学中心偏 —— 三角字符左右留白不对称，摆正了也看着歪；
//      · 一放大就发虚 —— 字体位图放大，边缘糊。
//    主流播放器的这几个键都是矢量图形，这里也用 QPainter 现画：
//    坐标全按控件尺寸的比例算，所以窗口怎么缩放、DPI 是多少，边缘都是干净的。
//
//  ★ 画在基类之后，不是替代基类 ★
//    先调 QPushButton::paintEvent 让样式表把圆形底色 / hover 底色画出来，
//    再往上叠自己的图标。这样圆角和配色仍然由样式表统一管，
//    以后调配色只改 QSS，不用来这里动代码。
//
//  ★ 悬停 / 按下时的变色 ★
//    样式表能改底色，但管不到自绘图标的颜色，所以这里自己按状态取色
//    （取的就是样式表里那几个值，改配色要两处一起改）。
//
//  ★ 和 SeekSlider 一样，必须定义在**全局作用域** ★
//    PlayerPage.h 里写的是 `class PlayerIconButton;`（全局的前向声明）。
//    塞进匿名 namespace 就变成另一个类型了，成员指针赋值会直接编译不过。
// -----------------------------------------------------------------------------
enum class PlayIconKind { Prev, Play, Pause, Next, Shuffle, Order, RepeatOne };

class PlayerIconButton : public QPushButton
{
public:
    // iconFrac = 图标占按钮边长的比例。默认 0.44 是给 上一首/播放/下一首 的；
    // 播放模式那三个图标线条多，要给大一点才看得清那个 "1"。
    PlayerIconButton(PlayIconKind kind, int diameter, QWidget* parent = nullptr,
                     qreal iconFrac = 0.44)
        : QPushButton(parent), m_kind(kind), m_iconFrac(iconFrac)
    {
        setFixedSize(diameter, diameter);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);      // 焦点留给搜索框和列表，按钮别抢
    }

    void setKind(PlayIconKind kind)
    {
        if (m_kind == kind)
            return;
        m_kind = kind;
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        QPushButton::paintEvent(event);   // 圆底 / 悬停底色：交给样式表

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(iconColor());

        const QRectF box = iconBox();
        switch (m_kind)
        {
        case PlayIconKind::Play:      drawTriangle(p, box, true);  break;
        case PlayIconKind::Pause:     drawPause(p, box);           break;
        case PlayIconKind::Prev:      drawSkip(p, box, true);      break;
        case PlayIconKind::Next:      drawSkip(p, box, false);     break;
        case PlayIconKind::Shuffle:   drawShuffle(p, box);         break;
        case PlayIconKind::Order:     drawOrder(p, box);           break;
        case PlayIconKind::RepeatOne: drawRepeatOne(p, box);       break;
        }
    }

private:
    bool isMainKey() const
    {
        return m_kind == PlayIconKind::Play || m_kind == PlayIconKind::Pause;
    }

    QColor iconColor() const
    {
        if (!isEnabled())
            return UiTheme::color(UiTheme::TextDisabled);
        if (isDown())
            return isMainKey() ? QColor(0xFF, 0xFF, 0xFF) : UiTheme::color(UiTheme::AccentText);
        if (underMouse())
            return isMainKey() ? QColor(0xFF, 0xFF, 0xFF) : UiTheme::color(UiTheme::AccentText);
        return isMainKey() ? QColor(0xFF, 0xFF, 0xFF) : UiTheme::color(UiTheme::TextMid);
    }

    // 图标占控件正中约 44%（默认档）见方 —— 播放键那圈底色比较满，图形小一点才透气
    QRectF iconBox() const
    {
        const qreal side = qreal(qMin(width(), height())) * m_iconFrac;
        return QRectF((width() - side) / 2.0, (height() - side) / 2.0, side, side);
    }

    // 箭头尖：以 tip 为顶点、朝 dirDeg（0 = 向右，90 = 向下）张开的一个小三角。
    // 用 QTransform 转坐标系来画，省得自己算 sin/cos。
    static void drawArrowHead(QPainter& p, const QPointF& tip, qreal dirDeg, qreal size)
    {
        p.save();
        p.translate(tip);
        p.rotate(dirDeg);

        QPolygonF head;
        head << QPointF(0.0, 0.0)
             << QPointF(-size, -size * 0.60)
             << QPointF(-size,  size * 0.60);
        p.drawPolygon(head);          // 用当前 brush 填
        p.restore();
    }

    // 三角（圆角）：填一遍再拿粗圆头笔描一遍轮廓，就得到主流播放器那种钝角三角。
    // 纯 fillPolygon 画出来三个尖角很锋利，缩小之后看着像缺了一块。
    static void drawTriangle(QPainter& p, const QRectF& b, bool pointRight)
    {
        qreal xTip = b.right() - b.width() * 0.04;
        qreal xBase = b.left() + b.width() * 0.14;
        if (!pointRight)
            std::swap(xTip, xBase);

        QPainterPath path;
        path.moveTo(xBase, b.top());
        path.lineTo(xTip,  b.center().y());
        path.lineTo(xBase, b.bottom());
        path.closeSubpath();

        p.setPen(QPen(p.brush().color(), b.width() * 0.20,
                      Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(path);
    }

    static void drawPause(QPainter& p, const QRectF& b)
    {
        const qreal w   = b.width() * 0.32;
        const qreal gap = b.width() * 0.26;
        p.drawRoundedRect(QRectF(b.center().x() - gap / 2.0 - w, b.top(), w, b.height()),
                          w / 2.0, w / 2.0);
        p.drawRoundedRect(QRectF(b.center().x() + gap / 2.0, b.top(), w, b.height()),
                          w / 2.0, w / 2.0);
    }

    // 上一首 / 下一首 = 一根竖条 + 一个三角（主流播放器「跳曲」键的经典画法）
    static void drawSkip(QPainter& p, const QRectF& b, bool toLeft)
    {
        const qreal barW = b.width() * 0.20;
        const qreal barX = toLeft ? b.left() : b.right() - barW;
        p.drawRoundedRect(QRectF(barX, b.top(), barW, b.height()), barW / 2.0, barW / 2.0);

        const qreal inLeft  = toLeft ? b.left() + barW + b.width() * 0.10 : b.left();
        const qreal inRight = toLeft ? b.right() : b.right() - barW - b.width() * 0.10;
        drawTriangle(p, QRectF(inLeft, b.top(), inRight - inLeft, b.height()), !toLeft);
    }

    // -------------------------------------------------------------------------
    //  播放模式那三个图标（照网易云那套画的）
    //
    //  · 随机播放 —— 两根**交叉的箭头** + 左端两个小圆点（网易云那个"散点+交错箭头"）
    //  · 顺序播放 —— 三根横线（一份列表）+ 右边一个**向右的箭头**，读作"顺着往下放"
    //  · 单曲循环 —— 一个**带缺口的圆角回路** + 缺口处一个箭头 + 中间一个 "1"
    //
    //  ★ 为什么不用字体里的字符 ★
    //    和 ▶/⏸ 是同一个理由（见文件开头那段）：字形随机器变、放大发虚。
    //    这三个图形线条多，字形位图在 190% DPI 下糊得最明显。
    //  ★ "1" 是画出来的，不是打出来的 ★
    //    画一条竖线 + 左上一个小撇就是 1，省得去求"哪个字体一定装了这个字形"。
    // -------------------------------------------------------------------------
    static void drawShuffle(QPainter& p, const QRectF& b)
    {
        const QColor c = p.brush().color();
        const qreal w = b.width();
        const qreal stroke = w * 0.105;

        // 左端两个小点：交叉箭头的"起点"
        const qreal dotR = w * 0.070;
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(b.left() + w * 0.12, b.top()    + w * 0.28), dotR, dotR);
        p.drawEllipse(QPointF(b.left() + w * 0.12, b.bottom() - w * 0.28), dotR, dotR);

        // 两条交叉的线（对称 45°），线尾留一点空给箭头尖
        p.setPen(QPen(c, stroke, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(b.left() + w * 0.26, b.top()    + w * 0.28),
                   QPointF(b.left() + w * 0.68, b.bottom() - w * 0.28));
        p.drawLine(QPointF(b.left() + w * 0.26, b.bottom() - w * 0.28),
                   QPointF(b.left() + w * 0.68, b.top()    + w * 0.28));

        // 两个箭头尖，各朝自己那条线的方向
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        drawArrowHead(p, QPointF(b.left() + w * 0.78, b.bottom() - w * 0.18),  44, w * 0.24);
        drawArrowHead(p, QPointF(b.left() + w * 0.78, b.top()    + w * 0.18), -44, w * 0.24);
    }

    static void drawOrder(QPainter& p, const QRectF& b)
    {
        const QColor c = p.brush().color();
        const qreal w = b.width();
        const qreal stroke = w * 0.105;

        // 三根横线 = 一份列表
        p.setPen(QPen(c, stroke, Qt::SolidLine, Qt::RoundCap));
        const qreal xa = b.left() + w * 0.05;
        const qreal xb = b.left() + w * 0.50;
        p.drawLine(QPointF(xa, b.top()    + w * 0.18), QPointF(xb, b.top()    + w * 0.18));
        p.drawLine(QPointF(xa, b.center().y()),        QPointF(xb, b.center().y()));
        p.drawLine(QPointF(xa, b.bottom() - w * 0.18), QPointF(xb, b.bottom() - w * 0.18));

        // 右边一个向右的箭头 = 顺着列表往下放
        p.drawLine(QPointF(b.left() + w * 0.56, b.center().y()),
                   QPointF(b.right() - w * 0.18, b.center().y()));

        p.setPen(Qt::NoPen);
        p.setBrush(c);
        drawArrowHead(p, QPointF(b.right() - w * 0.03, b.center().y()), 0, w * 0.26);
    }

    static void drawRepeatOne(QPainter& p, const QRectF& b)
    {
        const QColor c = p.brush().color();
        const qreal w = b.width();
        const qreal stroke = w * 0.085;
        const qreal r  = w * 0.20;
        const qreal x0 = b.left() + w * 0.03, x1 = b.right() - w * 0.03;
        const qreal y0 = b.top()  + w * 0.13, y1 = b.bottom() - w * 0.13;
        const qreal gap = w * 0.30;               // 顶边留的缺口宽度（给箭头尖）

        // 圆角回路：从左上角出发，顺时针绕一圈，在顶边右侧留个缺口
        QPainterPath loop;
        loop.moveTo(x0 + r, y0);
        loop.lineTo(x1 - r - gap, y0);            // 顶边（走到缺口）
        loop.moveTo(x1 - r, y0);                  // 跳过缺口再接着画
        loop.quadTo(x1, y0, x1, y0 + r);          // 右上圆角
        loop.lineTo(x1, y1 - r);
        loop.quadTo(x1, y1, x1 - r, y1);          // 右下圆角
        loop.lineTo(x0 + r, y1);
        loop.quadTo(x0, y1, x0, y1 - r);          // 左下圆角
        loop.lineTo(x0, y0 + r);
        loop.quadTo(x0, y0, x0 + r, y0);          // 左上圆角

        p.setPen(QPen(c, stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(loop);

        // 缺口里的箭头尖 —— 这个"绕一圈"就是循环的意思
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        drawArrowHead(p, QPointF(x1 - r - gap + w * 0.06, y0), 0, w * 0.22);

        // 中间一个 "1"：竖线 + 左上角一个小撇（就是数字 1，只是不用字体打）
        p.setPen(QPen(c, stroke * 1.15, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const qreal cx  = b.center().x() + w * 0.02;
        const qreal oneTop = y0 + w * 0.16;
        const qreal oneBot = y1 - w * 0.10;
        p.drawLine(QPointF(cx, oneTop), QPointF(cx, oneBot));
        p.drawLine(QPointF(cx - w * 0.09, oneTop + w * 0.09), QPointF(cx, oneTop));
    }

    PlayIconKind m_kind;
    qreal        m_iconFrac;
};

namespace {

// -----------------------------------------------------------------------------
//  smallerFont —— "副标题"用的字号：比正文小一档
//
//  ★ 不能只减 pointSizeF ★
//    有的平台给的是像素字号，那时候 pointSizeF() 返回 -1，减出来变成 7pt ——
//    副标题反而比正文还大。两个字段都照顾到才稳。
//  （和 UiFont::applyToApplication() 里那段是同一个道理。）
// -----------------------------------------------------------------------------
QFont smallerFont(const QFont& base)
{
    QFont f = base;
    if (f.pixelSize() > 0)
        f.setPixelSize(qMax(8, f.pixelSize() - 2));
    else
        f.setPointSizeF(qMax(7.0, f.pointSizeF() - 1.0));
    return f;
}

// -----------------------------------------------------------------------------
//  TrackItemDelegate —— 列表每一行长什么样
//
//  三件事只有自己画才好控制：正在播的那一行左侧有一根紫色竖条、
//  标题和艺术家是**两行不同字号**、时长右对齐且不跟标题抢位置。
//  用 QListWidget + 自定义 widget 也能做，但几万行会建几万个控件（见 TrackListModel 的说明）。
//
//  ★ 行高必须由字体算出来，不能写死 42 ★
//    这一行里塞了两行文字（标题 + 艺术家）。设置页可以把界面字号调到 150%，
//    那时候两行文字加起来要 50 多像素 —— 写死 42 的话上一行的艺术家会和
//    下一行的标题**叠在一起**（看着像渲染坏了，其实是行高不够）。
//    所以 sizeHint 按当前字体量出来，字号档位变了自动跟着变。
// -----------------------------------------------------------------------------
class TrackItemDelegate : public QStyledItemDelegate
{
public:
    explicit TrackItemDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {}

    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex&) const override
    {
        const QFontMetrics fm(opt.font);
        const QFontMetrics smallFm(smallerFont(opt.font));
        // 两行字高 + 上下各留一点缝，就是这一行该有多高
        return QSize(160, fm.height() + smallFm.height() + 14);
    }

    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override
    {
        const bool selected = (opt.state & QStyle::State_Selected);
        const bool playing  = idx.data(TrackListModel::IsPlayingRole).toBool();

        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);

        const QRect r = opt.rect.adjusted(2, 1, -2, -2);

        if (selected)
            p->fillRect(r, UiTheme::color(UiTheme::NavBg));

        if (playing)
            p->fillRect(QRect(r.left() + 3, r.top() + 9, 3, r.height() - 18),
                        UiTheme::color(UiTheme::Accent));

        const int textLeft = r.left() + (playing ? 14 : 8);

        // ---- 时长（先算它，标题要在剩下的宽度里省略）----
        const qint64 ms = idx.data(TrackListModel::DurationRole).toLongLong();
        QString dur = QStringLiteral("--:--");
        if (ms > 0)
        {
            const qint64 sec = ms / 1000;
            dur = QStringLiteral("%1:%2").arg(sec / 60)
                      .arg(sec % 60, 2, 10, QLatin1Char('0'));
        }

        QFont smallFont = smallerFont(opt.font);
        const QFontMetrics smallFm(smallFont);
        const int durWidth = smallFm.horizontalAdvance(dur) + 8;

        // ---- 来源徽标（本地 / 联网·网易 / 下载中 42% …）：画在时长左边 ----
        const QString tag = idx.data(TrackListModel::TagRole).toString();
        int tagWidth = 0;
        if (!tag.isEmpty())
        {
            const QFont tagFont = smallerFont(opt.font);
            const QFontMetrics tagFm(tagFont);
            tagWidth = tagFm.horizontalAdvance(tag) + 10;
            p->setFont(tagFont);
            p->setPen(UiTheme::color(UiTheme::TextSub));
            p->drawText(QRect(r.right() - durWidth - tagWidth, r.top(), tagWidth, r.height()),
                        Qt::AlignRight | Qt::AlignVCenter, tag);
        }

        p->setFont(smallFont);
        p->setPen(UiTheme::color(UiTheme::TextFaint));
        p->drawText(QRect(r.right() - durWidth, r.top(), durWidth, r.height()),
                    Qt::AlignRight | Qt::AlignVCenter, dur);

        // ---- 标题 / 艺术家 ----
        const int avail = qMax(20, r.right() - durWidth - tagWidth - textLeft - 6);

        const QFontMetrics fm(opt.font);
        const int twoLine = fm.height() + smallFm.height();
        const int top = r.top() + qMax(0, (r.height() - twoLine) / 2);

        p->setFont(opt.font);
        p->setPen(playing ? accentDeepColor() : UiTheme::color(UiTheme::TextStrong));
        const QString title = idx.data(TrackListModel::TitleRole).toString();
        p->drawText(QRect(textLeft, top, avail, fm.height()),
                    Qt::AlignLeft | Qt::AlignVCenter,
                    fm.elidedText(title, Qt::ElideRight, avail));

        QString artist = idx.data(TrackListModel::ArtistRole).toString();
        if (artist.isEmpty())
            artist = QStringLiteral("未知艺术家");

        p->setFont(smallFont);
        p->setPen(UiTheme::color(UiTheme::TextFaint));
        p->drawText(QRect(textLeft, top + fm.height(), avail, smallFm.height()),
                    Qt::AlignLeft | Qt::AlignVCenter,
                    smallFm.elidedText(artist, Qt::ElideRight, avail));

        p->restore();
    }
};

// 找"时间 <= ms 的最后一行"—— 歌词高亮就是靠它。
// 二分：一首歌几百行、每 200ms 找一次，线性扫也能用，但二分更体面。
int lyricIndexAt(const QVector<TrackMeta::LyricLine>& lines, qint64 ms)
{
    int lo = 0, hi = lines.size() - 1, ans = -1;
    while (lo <= hi)
    {
        const int mid = (lo + hi) / 2;
        if (lines.at(mid).timeMs <= ms)
        {
            ans = mid;
            lo = mid + 1;
        }
        else
        {
            hi = mid - 1;
        }
    }
    return ans;
}

// 歌词行距（100% 档位下的基准值）。实际用 UiFont::px() 按"界面字号"换算 ——
// 字放大行距不跟着走的话，几行大字会挤成一坨。
constexpr int kLyricSpacingBase = 10;

// 歌词自动跟随的"暂停后恢复"时长。用户滚开歌词之后，闹钟清零重新计时 ——
// 网易云是这个路数：滚的时候绝不抢，但也不永远撒手不管。
constexpr int kLyricFollowResumeMs = 4000;

// 当前行切换时滚动动画的时长。300ms 上下是主流播放器的手感：
// 再快就看不出"滚"了（等于跳变），再慢就跟不上快节奏的歌。
constexpr int kLyricScrollAnimMs = 320;

// 歌词行的两种角色名（写在样式表里的，见 applyStyle）。
// 用 objectName 而不是逐个 setStyleSheet 的原因见 highlightLyric()。
constexpr const char* kLyricRoleNormal = "playerLyricLine";
constexpr const char* kLyricRoleActive = "playerLyricActive";

// 换 objectName 之后必须重新走一遍样式解析，否则新名字对应的规则不会生效 ——
// Qt 只在 polish 的时候按 objectName 匹配一次，光改名不会自动重算。
void applyLyricRole(QLabel* lab, const char* role)
{
    if (!lab)
        return;

    lab->setObjectName(QString::fromLatin1(role));
    if (QStyle* st = lab->style())
    {
        st->unpolish(lab);
        st->polish(lab);
    }
    lab->update();
}

} // namespace

// =============================================================================
//  构造
// =============================================================================
PlayerPage::PlayerPage(QWidget* parent) : QWidget(parent)
{
    m_lib    = new MusicLibrary(this);
    m_model  = new TrackListModel(m_lib, this);
    m_player = new AudioPlayer(this);

    buildUi();
    applyUiScale();          // = applyStyle() + 歌词行距，见函数里的说明

    // ---- 曲库 ----
    connect(m_lib, &MusicLibrary::scanStarted,  this, &PlayerPage::onScanStarted);
    connect(m_lib, &MusicLibrary::scanBatch,    this, &PlayerPage::onScanBatch);
    connect(m_lib, &MusicLibrary::scanFinished, this, &PlayerPage::onScanFinished);
    connect(m_lib, &MusicLibrary::scanProgress, this,
            [this](int seen, int found, const QString& dir) {
                Q_UNUSED(seen);
                Q_UNUSED(dir);
                m_countLabel->setText(QStringLiteral("正在扫描… 已找到 %1 首").arg(found));
            });

    // ---- 播放器 ----
    connect(m_player, &AudioPlayer::positionChanged, this, &PlayerPage::onPositionChanged);
    connect(m_player, &AudioPlayer::durationChanged, this, &PlayerPage::onDurationChanged);
    connect(m_player, &AudioPlayer::stateChanged,    this, &PlayerPage::onPlayerStateChanged);
    connect(m_player, &AudioPlayer::trackFinished,   this, &PlayerPage::onTrackFinished);
    connect(m_player, &AudioPlayer::errorOccurred,    this, [this](const QString& msg) {
        m_nowLabel->setText(msg);
    });

    // 音量：先读存档再设，免得用户每次打开都要重调
    {
        QSettings s;
        const int vol = s.value(QStringLiteral("player/volume"), 80).toInt();
        m_volume->setValue(qBound(0, vol, 100));
        m_player->setVolumePercent(m_volume->value());
    }

    // 播放模式：同样先读存档（缺省 = 顺序播放），再把图标/提示刷上去。
    //   ★ 存档里是枚举的整数值，读到不认识的数（老版本/手改过）就落回默认，
    //     不能直接把野值塞进 m_mode，否则 switch 走不到任何分支。★
    {
        QSettings s;
        const int mode = s.value(QStringLiteral("player/mode"), int(PlayMode::Order)).toInt();
        if (mode == int(PlayMode::Shuffle) || mode == int(PlayMode::Order)
            || mode == int(PlayMode::RepeatOne))
        {
            m_mode = PlayMode(mode);
        }
        applyMode();
    }

    // 搜索防抖：停手 250ms 才真去搜本地
    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(250);
    connect(m_searchTimer, &QTimer::timeout, this, &PlayerPage::doSearch);

    // 联网搜索防抖：独立 600ms（本地结果先出，联网结果随后追加，两不耽误）
    m_onlineTimer = new QTimer(this);
    m_onlineTimer->setSingleShot(true);
    m_onlineTimer->setInterval(600);
    connect(m_onlineTimer, &QTimer::timeout, this, [this] {
        doOnlineSearch(m_searchEdit->text());
    });

    // ---- 联网功能的存储 ----
    // 临时缓存（C 盘 %TEMP%/PetPalMusic）：启动先清上次残留（崩溃/强退漏下的），
    // 本会话的临时文件在程序退出时由 main() 统一整删。
    OnlineMusic::clearTempDir();
    m_saveDir = QSettings(QSettings::IniFormat, QSettings::UserScope,
                          QStringLiteral("PetPal"), QStringLiteral("ui"))
                    .value(QStringLiteral("player/saveDir")).toString();

    // 初始视图：空查询 = 全部
    m_model->setView(m_lib->search(QString()));
    updateCountLabel();      // 顺带把"上一首 / 下一首"的可按状态刷成最新

    // ---- 桌面歌词悬浮窗 ----
    // 独立顶层窗口（不给 parent —— parent 给了this会跟着面板的什么属性走，
    // 而它要的是"浮在桌面上"）。开不开由设置页的开关决定，那里也是存档所在。
    m_dtLyricsEnabled = DesktopLyrics::loadEnabled();
    m_desktopLyrics = new DesktopLyrics(nullptr);
    m_desktopLyrics->hide();
    m_desktopLyrics->setPlaying(m_player->isPlaying());
    // 悬浮窗只做显示 + 播放控制：按钮发意图，真正动手的是这一页
    //（它才有播放器和歌词数据）。三连击唤面板转给 DesktopPet。
    connect(m_desktopLyrics, &DesktopLyrics::playPauseRequested,
            this, &PlayerPage::togglePlayPause);
    connect(m_desktopLyrics, &DesktopLyrics::prevRequested,
            this, &PlayerPage::playPrev);
    connect(m_desktopLyrics, &DesktopLyrics::nextRequested,
            this, &PlayerPage::playNext);
    connect(m_desktopLyrics, &DesktopLyrics::panelRequested,
            this, &PlayerPage::desktopLyricsPanelRequested);
    // 悬浮窗上的 ×：整个功能关掉（落盘 + 收窗），重开回设置页勾选
    connect(m_desktopLyrics, &DesktopLyrics::closeRequested,
            this, [this]() { setDesktopLyricsEnabled(false); });
}

PlayerPage::~PlayerPage() = default;

// =============================================================================
//  搭界面
// =============================================================================
void PlayerPage::buildUi()
{
    // ---------------- 工具行 ----------------
    // 「我的收藏」切换按钮（收藏视图 / 全部列表）
    m_favBtn = new QPushButton(QStringLiteral("我的收藏"), this);
    m_favBtn->setObjectName(QStringLiteral("playerFav"));
    m_favBtn->setCheckable(true);
    m_favBtn->setCursor(Qt::PointingHandCursor);
    m_favBtn->setToolTip(QStringLiteral("只看收藏过的歌（联网搜索里点右键收藏）"));
    connect(m_favBtn, &QPushButton::toggled, this, &PlayerPage::showFavorites);

    // 「我的下载」切换按钮（已下载/已收藏落盘的歌，可删除）
    m_dlBtn = new QPushButton(QStringLiteral("我的下载"), this);
    m_dlBtn->setObjectName(QStringLiteral("playerFav"));   // 和我的收藏同款样式
    m_dlBtn->setCheckable(true);
    m_dlBtn->setCursor(Qt::PointingHandCursor);
    m_dlBtn->setToolTip(QStringLiteral("只看下载到本地的歌（列表里右键可删除）"));
    connect(m_dlBtn, &QPushButton::toggled, this, &PlayerPage::showDownloads);

    m_pickBtn = new QPushButton(QStringLiteral("选文件夹"), this);
    m_pickBtn->setObjectName(QStringLiteral("playerPick"));
    m_pickBtn->setCursor(Qt::PointingHandCursor);
    m_pickBtn->setToolTip(QStringLiteral("选一个文件夹，里面（包括子文件夹）的音频会自动列出来"));
    connect(m_pickBtn, &QPushButton::clicked, this, &PlayerPage::onPickFolder);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setObjectName(QStringLiteral("playerSearch"));
    m_searchEdit->setPlaceholderText(QStringLiteral("搜索歌名 / 歌手 / 专辑"));
    m_searchEdit->setClearButtonEnabled(true);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &PlayerPage::onSearchChanged);

    m_countLabel = new QLabel(this);
    m_countLabel->setObjectName(QStringLiteral("playerCount"));

    auto* tools = new QHBoxLayout;
    tools->setContentsMargins(0, 0, 0, 0);
    tools->setSpacing(8);
    tools->addWidget(m_favBtn);
    tools->addWidget(m_dlBtn);
    tools->addWidget(m_pickBtn);
    tools->addWidget(m_searchEdit, 1);
    tools->addWidget(m_countLabel);

    // ---------------- 左：曲目列表 ----------------
    m_list = new QListView(this);
    m_list->setObjectName(QStringLiteral("playerList"));
    m_list->setModel(m_model);
    m_list->setItemDelegate(new TrackItemDelegate(m_list));
    m_list->setUniformItemSizes(true);        // 行高一致 -> 几万行也只算一次布局
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setFrameShape(QFrame::NoFrame);
    connect(m_list, &QListView::activated, this, &PlayerPage::onRowActivated);

    // 右键行菜单：收藏 / 仅下载（联网行），收藏（本地行）
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QWidget::customContextMenuRequested,
            this, &PlayerPage::onListMenu);

    // ---------------- 右：歌词 ----------------
    m_lyricHeader = new QLabel(QStringLiteral("歌词"), this);
    m_lyricHeader->setObjectName(QStringLiteral("playerLyricHeader"));

    m_lyricHost = new QWidget;
    m_lyricLay  = new QVBoxLayout(m_lyricHost);
    m_lyricLay->setContentsMargins(6, 8, 6, 8);
    m_lyricLay->setSpacing(UiFont::px(kLyricSpacingBase));   // 行距跟着"界面字号"走，见 applyUiScale()
    m_lyricLay->addStretch();

    m_lyricScroll = new QScrollArea(this);
    m_lyricScroll->setObjectName(QStringLiteral("playerLyric"));
    m_lyricScroll->setWidget(m_lyricHost);
    m_lyricScroll->setWidgetResizable(true);
    m_lyricScroll->setFrameShape(QFrame::NoFrame);
    m_lyricScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    // ---- 歌词平滑滚动（网易云那套）----
    // 缓动动画骑在滚动条的 value 属性上；滚轮/拖滑条是"用户接管"信号。
    m_lyricAnim = new QPropertyAnimation(m_lyricScroll->verticalScrollBar(),
                                         "value", this);
    m_lyricAnim->setDuration(kLyricScrollAnimMs);
    m_lyricAnim->setEasingCurve(QEasingCurve::OutCubic);

    // ★ 用户接管的三个入口，一个都不能漏 ★
    //   滚轮（事件过滤器抓 viewport 和滚动条自己的滚轮）、拖滑条把手
    //   （sliderMoved）、点滑条凹槽翻页（actionTriggered）。
    //   刻意**不**连 valueChanged —— 动画 setValue 也会发它，那样自己会把自己
    //   判成"用户在滚"，永远暂停。
    m_lyricScroll->viewport()->installEventFilter(this);
    m_lyricScroll->verticalScrollBar()->installEventFilter(this);
    connect(m_lyricScroll->verticalScrollBar(), &QScrollBar::sliderMoved,
            this, [this]() { pauseLyricFollow(); });
    connect(m_lyricScroll->verticalScrollBar(), &QScrollBar::actionTriggered,
            this, [this]() { pauseLyricFollow(); });

    m_lyricFollowTimer = new QTimer(this);
    m_lyricFollowTimer->setSingleShot(true);
    m_lyricFollowTimer->setInterval(kLyricFollowResumeMs);
    connect(m_lyricFollowTimer, &QTimer::timeout, this, &PlayerPage::resumeLyricFollow);

    // 「回到当前歌词」：悬浮在歌词区上（子控件放在 QScrollArea 而不是 viewport 上，
    // 才不会被内容滚走）。平时藏着，用户滚开歌词时才浮现 —— 网易云同款。
    m_lyricRecenterBtn = new QPushButton(QStringLiteral("回到当前歌词"), m_lyricScroll);
    m_lyricRecenterBtn->setObjectName(QStringLiteral("lyricRecenter"));
    m_lyricRecenterBtn->setCursor(Qt::PointingHandCursor);
    m_lyricRecenterBtn->hide();
    m_lyricRecenterBtn->raise();
    connect(m_lyricRecenterBtn, &QPushButton::clicked,
            this, &PlayerPage::resumeLyricFollow);

    clearLyrics(QStringLiteral("还没有在放歌"));

    auto* lyricCol = new QVBoxLayout;
    lyricCol->setContentsMargins(0, 0, 0, 0);
    lyricCol->setSpacing(6);
    lyricCol->addWidget(m_lyricHeader);
    lyricCol->addWidget(m_lyricScroll, 1);

    auto* middle = new QHBoxLayout;
    middle->setContentsMargins(0, 0, 0, 0);
    middle->setSpacing(12);
    middle->addWidget(m_list, 3);
    middle->addLayout(lyricCol, 2);

    // ---------------- 底：播放条 ----------------
    // 播放模式键：放在**封面左边**（用户要求的位置，也是网易云 / QQ 音乐的位置）。
    // 点一下换一种：顺序 → 随机 → 单曲循环 → 顺序…（图标和提示由 applyMode() 刷）。
    // 30px 比上下首那对（32px）略小一点：它是个"设置"，不该和切歌键抢注意力。
    m_modeBtn = new PlayerIconButton(PlayIconKind::Order, 30, this, 0.52);
    m_modeBtn->setObjectName(QStringLiteral("playerMode"));
    connect(m_modeBtn, &QPushButton::clicked, this, &PlayerPage::onModeClicked);

    m_coverLabel = new QLabel(this);
    m_coverLabel->setObjectName(QStringLiteral("playerCover"));
    m_coverLabel->setFixedSize(44, 44);
    m_coverLabel->setAlignment(Qt::AlignCenter);

    m_nowLabel = new QLabel(QStringLiteral("没在放歌"), this);
    m_nowLabel->setObjectName(QStringLiteral("playerNow"));

    m_posLabel = new QLabel(QStringLiteral("0:00"), this);
    m_posLabel->setObjectName(QStringLiteral("playerTime"));
    m_durLabel = new QLabel(QStringLiteral("--:--"), this);
    m_durLabel->setObjectName(QStringLiteral("playerTime"));

    m_slider = new SeekSlider(Qt::Horizontal, this);
    m_slider->setObjectName(QStringLiteral("playerSeek"));
    m_slider->setRange(0, 0);
    m_slider->setCursor(Qt::PointingHandCursor);
    connect(m_slider, &QSlider::sliderPressed,  this, &PlayerPage::onSeekPressed);
    connect(m_slider, &QSlider::sliderMoved,    this, &PlayerPage::onSeekMoved);
    connect(m_slider, &QSlider::sliderReleased, this, &PlayerPage::onSeekReleased);

    auto* timeRow = new QHBoxLayout;
    timeRow->setContentsMargins(0, 0, 0, 0);
    timeRow->setSpacing(8);
    timeRow->addWidget(m_posLabel);
    timeRow->addWidget(m_slider, 1);
    timeRow->addWidget(m_durLabel);

    auto* infoCol = new QVBoxLayout;
    infoCol->setContentsMargins(0, 0, 0, 0);
    infoCol->setSpacing(4);
    infoCol->addWidget(m_nowLabel);
    infoCol->addLayout(timeRow);

    // ---- 播放控制：三个键都是自绘矢量图标，不是字体里的字符 ----
    //   · 播放/暂停 40px，实心紫圆底 + 白色三角/双竖条 —— 和主流播放器一样是视觉重心；
    //   · 上一首/下一首 32px，无底色，hover 才浮出一个浅灰圆；
    //   · 尺寸给的是像素而不是跟着字号缩放 —— 这几个键是"点得到"的靶子，
    //     缩太小会难点中；图标本身是矢量，不会因为按钮小而糊。
    m_prevBtn = new PlayerIconButton(PlayIconKind::Prev, 32, this);
    m_prevBtn->setObjectName(QStringLiteral("playerTransport"));
    m_prevBtn->setToolTip(QStringLiteral("上一首"));
    connect(m_prevBtn, &QPushButton::clicked, this, &PlayerPage::playPrev);

    m_playBtn = new PlayerIconButton(PlayIconKind::Play, 40, this);
    m_playBtn->setObjectName(QStringLiteral("playerPlay"));
    m_playBtn->setToolTip(QStringLiteral("播放 / 暂停"));
    connect(m_playBtn, &QPushButton::clicked, this, &PlayerPage::togglePlayPause);

    m_nextBtn = new PlayerIconButton(PlayIconKind::Next, 32, this);
    m_nextBtn->setObjectName(QStringLiteral("playerTransport"));
    m_nextBtn->setToolTip(QStringLiteral("下一首"));
    connect(m_nextBtn, &QPushButton::clicked, this, &PlayerPage::playNext);

    // 列表里一首歌都没有的时候灰掉 —— 主流播放器都这样。
    // 亮了却点了没反应，用户会以为程序卡了。具体状态由 updateCountLabel() 维护。
    m_prevBtn->setEnabled(false);
    m_nextBtn->setEnabled(false);

    m_volume = new QSlider(Qt::Horizontal, this);
    m_volume->setObjectName(QStringLiteral("playerVolume"));
    m_volume->setRange(0, 100);
    m_volume->setFixedWidth(72);
    m_volume->setCursor(Qt::PointingHandCursor);
    m_volume->setToolTip(QStringLiteral("音量"));
    connect(m_volume, &QSlider::valueChanged, this, [this](int v) {
        m_player->setVolumePercent(v);
        QSettings s;
        s.setValue(QStringLiteral("player/volume"), v);
    });

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(0, 6, 0, 0);
    bar->setSpacing(10);
    bar->addWidget(m_modeBtn);
    bar->addWidget(m_coverLabel);
    bar->addLayout(infoCol, 1);
    bar->addWidget(m_prevBtn);
    bar->addWidget(m_playBtn);
    bar->addWidget(m_nextBtn);
    bar->addWidget(m_volume);

    // ---------------- 总装 ----------------
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 10, 14, 12);
    root->setSpacing(10);
    root->addLayout(tools);

    // ---------------- 曲源选择行（在「我的收藏」正下方）----------------
    // 网易云 / B 站二选一：网易云模式搜不到会自动转 B 站接档；B 站模式只搜 B 站。
    // 选择记进 ui.ini，下次启动保持。
    auto* srcRow = new QHBoxLayout;
    srcRow->setContentsMargins(0, 2, 0, 0);
    srcRow->setSpacing(6);

    auto* srcCap = new QLabel(QStringLiteral("曲源"), this);
    srcCap->setObjectName(QStringLiteral("playerCount"));
    srcRow->addWidget(srcCap);

    m_srcNetease = new QPushButton(QStringLiteral("网易云"), this);
    m_srcNetease->setObjectName(QStringLiteral("playerFav"));
    m_srcNetease->setCheckable(true);
    m_srcNetease->setCursor(Qt::PointingHandCursor);

    m_srcBili = new QPushButton(QStringLiteral("B站"), this);
    m_srcBili->setObjectName(QStringLiteral("playerFav"));
    m_srcBili->setCheckable(true);
    m_srcBili->setCursor(Qt::PointingHandCursor);

    // 默认网易云（历史行为）；存档里有就按存档来
    m_onlineSource = QSettings(QSettings::IniFormat, QSettings::UserScope,
                               QStringLiteral("PetPal"), QStringLiteral("ui"))
                         .value(QStringLiteral("player/onlineSource"), 0).toInt() == 1
                         ? 1 : 0;
    m_srcNetease->setChecked(m_onlineSource == 0);
    m_srcBili->setChecked(m_onlineSource == 1);

    auto* srcGroup = new QButtonGroup(this);   // 默认互斥：同一时间只有一个曲源
    srcGroup->addButton(m_srcNetease, 0);
    srcGroup->addButton(m_srcBili, 1);
    connect(srcGroup, &QButtonGroup::idClicked, this, [this](int id) {
        if (id == m_onlineSource)
            return;                            // 重复点同一个：什么都不做
        m_onlineSource = id;
        QSettings(QSettings::IniFormat, QSettings::UserScope,
                  QStringLiteral("PetPal"), QStringLiteral("ui"))
            .setValue(QStringLiteral("player/onlineSource"), id);
        doOnlineSearch(m_searchEdit->text());  // 切曲源 = 立刻用当前关键词重搜
    });

    srcRow->addWidget(m_srcNetease);
    srcRow->addWidget(m_srcBili);
    srcRow->addStretch();

    root->addLayout(srcRow);

    root->addLayout(middle, 1);
    root->addLayout(bar);

    updateCountLabel();
}

// =============================================================================
//  样式表
//  全部用 objectName 限定 —— 这是主面板的第 4 页，写宽松了会串到别的页上。
//
//  ★ 整段包在 UiFont::styleSheet() 里 ★
//    它按"界面字号"档位把每个 `font-size: Npx` 改写一遍（见 UiFont.h），
//    所以这里照常按 100% 写死值就行，不用到处乘系数。
//
//  ★ 歌词三态走 objectName，不是逐个 setStyleSheet ★
//    「普通行 / 正在唱的那行 / 还没放歌的提示语」如果写在代码里逐行 setStyleSheet，
//    字号一变就得把每一行都改一遍（几百行歌词就是几百次），而且和这里的 QSS 成了
//    两份并存的字号定义，迟早对不上。改成这里定义三条规则、代码只切 objectName（
//    highlightLyric 里的 applyLyricRole），套一次样式全页跟着变。
// =============================================================================
void PlayerPage::applyStyle()
{
    // 只替换 %1（歌词强调色），色值取自 accentDeepColor()，不在这里再抄一遍
    const QString qss = QStringLiteral(R"(
        QPushButton#playerPick {
            border: none; border-radius: 6px; padding: 6px 14px;
            background: #7F77DD; color: #FFFFFF; font-size: 12px;
        }
        QPushButton#playerPick:hover   { background: #6E65D6; }
        QPushButton#playerPick:pressed { background: #5F57C8; }

        /* 我的收藏切换按钮：平时白底细边，选中（收藏视图）浅强调底 */
        QPushButton#playerFav {
            border: 1px solid #E3E1D9; border-radius: 6px;
            background: #FFFFFF; color: #2C2C2A;
            padding: 6px 10px; font-size: 12px;
        }
        QPushButton#playerFav:hover   { border-color: #7F77DD; color: #534AB7; }
        QPushButton#playerFav:checked { background: #EEEDFE; color: #534AB7; border-color: #7F77DD; }

        QLineEdit#playerSearch {
            border: 1px solid #E3E1D9; border-radius: 6px;
            padding: 5px 8px; font-size: 12px;
            color: #2C2C2A; background: #FFFFFF;
        }
        QLineEdit#playerSearch:focus { border-color: #7F77DD; }

        QLabel#playerCount { color: #888780; font-size: 11px; }

        QListView#playerList {
            background: transparent; border: none; outline: none;
        }
        QListView#playerList::item { border: none; }

        /* 曲目列表和歌词区的滚动条统一成细圆条 —— 原生那根大灰条又宽又方，
           在歌词这种窄栏里尤其压手。两处规则完全一致，看着才像一套东西。
           色值是默认主题的字面量，会被 UiTheme 网关按当前主题换色。 */
        QListView#playerList QScrollBar:vertical,
        QScrollArea#playerLyric QScrollBar:vertical {
            background: transparent; width: 8px; margin: 2px;
        }
        QListView#playerList QScrollBar::handle:vertical,
        QScrollArea#playerLyric QScrollBar::handle:vertical {
            background: #D3D1C7; border-radius: 3px; min-height: 30px;
        }
        QListView#playerList QScrollBar::handle:vertical:hover,
        QScrollArea#playerLyric QScrollBar::handle:vertical:hover {
            background: #B9B7AE;
        }
        QListView#playerList QScrollBar::add-line:vertical,
        QScrollArea#playerLyric QScrollBar::add-line:vertical,
        QListView#playerList QScrollBar::sub-line:vertical,
        QScrollArea#playerLyric QScrollBar::sub-line:vertical { height: 0; }
        QListView#playerList QScrollBar::add-page:vertical,
        QScrollArea#playerLyric QScrollBar::add-page:vertical,
        QListView#playerList QScrollBar::sub-page:vertical,
        QScrollArea#playerLyric QScrollBar::sub-page:vertical { background: transparent; }

        /* 「回到当前歌词」悬浮按钮：歌词被手动滚开时浮现（网易云那套）。
           底色比卡片实一点，别让底下的歌词字透上来搅成一团。 */
        QPushButton#lyricRecenter {
            background: #FFFFFF; color: #534AB7;
            border: 1px solid #AFA9EC; border-radius: 13px;
            padding: 5px 14px; font-size: 12px;
        }
        QPushButton#lyricRecenter:hover { background: #EEEDFE; }

        QLabel#playerLyricHeader {
            color: #888780; font-size: 12px; padding-left: 2px;
        }
        QScrollArea#playerLyric { background: transparent; border: none; }
        QScrollArea#playerLyric > QWidget > QWidget { background: transparent; }

        /* 歌词：普通行压灰、正在唱的那行放大加粗并染成强调色 —— 主流播放器都是这么分层的。
           padding 上下各 3px 是为了"稀疏一点"：光靠布局 spacing 的话，
           两行之间的视觉间距和字高不成比例，字一大就显挤。 */
        QLabel#playerLyricLine {
            color: #8A8880; font-size: 15px; padding: 3px 0;
            background: transparent;
        }
        QLabel#playerLyricActive {
            color: %1; font-size: 17px; font-weight: 600; padding: 3px 0;
            background: transparent;
        }
        QLabel#playerLyricHint {
            color: #B4B2A9; font-size: 14px; background: transparent;
        }

        QLabel#playerCover {
            border-radius: 8px; background: #F1EFE8;
            color: #B4B2A9; font-size: 16px;
        }
        QLabel#playerNow { color: #2C2C2A; font-size: 12px; }
        QLabel#playerTime { color: #9A9890; font-size: 11px; }

        /* 播放控制键：底色和圆角交给样式表，图标由 PlayerIconButton 自绘。
           这里不再写 color / font-size —— 图标不是字，写了对它没用。 */
        QPushButton#playerTransport {
            border: none; border-radius: 16px; background: transparent;
        }
        QPushButton#playerTransport:hover   { background: #F1EFE8; }
        QPushButton#playerTransport:pressed { background: #E8E6DF; }

        QPushButton#playerPlay {
            border: none; border-radius: 20px; background: #7F77DD;
        }
        QPushButton#playerPlay:hover   { background: #6E65D6; }
        QPushButton#playerPlay:pressed { background: #5F57C8; }

        /* 播放模式键（随机 / 顺序 / 单曲循环）：和上一首/下一首一样无底色，
           hover 才浮出一个浅灰圆。图标同样是 PlayerIconButton 自绘，
           所以这里只给底色和圆角，不写 color / font-size。 */
        QPushButton#playerMode {
            border: none; border-radius: 15px; background: transparent;
        }
        QPushButton#playerMode:hover   { background: #F1EFE8; }
        QPushButton#playerMode:pressed { background: #E8E6DF; }

        QSlider#playerSeek::groove:horizontal {
            height: 4px; background: #E6E4DC; border-radius: 2px;
        }
        QSlider#playerSeek::sub-page:horizontal {
            height: 4px; background: #7F77DD; border-radius: 2px;
        }
        QSlider#playerSeek::handle:horizontal {
            width: 11px; height: 11px; margin: -4px 0;
            border-radius: 5px; background: #7F77DD;
        }

        QSlider#playerVolume::groove:horizontal {
            height: 3px; background: #E6E4DC; border-radius: 2px;
        }
        QSlider#playerVolume::sub-page:horizontal {
            height: 3px; background: #B4B0E8; border-radius: 2px;
        }
        QSlider#playerVolume::handle:horizontal {
            width: 9px; height: 9px; margin: -3px 0;
            border-radius: 4px; background: #9C95E6;
        }
    )").arg(accentDeepColor().name());

    setStyleSheet(UiFont::styleSheet(qss));
}

// =============================================================================
//  界面字号档位变了
//
//  ★ 为什么不能只重套样式表 ★
//    这个控件树的字号确实全在样式表里（applyStyle 一次改完），
//    但歌词的**行距**是 QVBoxLayout 的属性，样式表管不到 ——
//    字放大了行距还是 10px，几行大字会挤在一起。
//    所以这一条要单独跟着档位重设一次。
// =============================================================================
void PlayerPage::applyUiScale()
{
    applyStyle();
    m_lyricLay->setSpacing(UiFont::px(kLyricSpacingBase));
}

// =============================================================================
//  背景：把封面淡化铺满整页
// =============================================================================
void PlayerPage::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter p(this);
    p.fillRect(rect(), QColor(0xFF, 0xFF, 0xFF));

    if (m_bgCover.isNull())
        return;                       // 没封面就是干净的白底，不留任何痕迹

    // ★ 缓存 ★ 每次重绘都 scaled() 一张几百像素的图太浪费，
    //   而窗口尺寸又不会老变 —— 只在"换了封面"或"换了尺寸"时重算。
    if (m_bgCache.isNull() || m_bgCacheSize != size())
    {
        // KeepAspectRatioByExpanding：等比放大到铺满，多出来的边裁掉。
        // 用 Stretch 会把封面拉变形，宁可裁。
        // ★ 平滑插值（SmoothTransformation），别用最近邻 ★
        //   曾经为了 resize 跟手刻意用最近邻（拖窗口边缘一次拖动会来上百次
        //   resize，当时怕平滑缩放拖慢）——但源图已经预先缩到 ≤1280（见
        //   loadLyricsAndCover / themeFallbackCover），从这里平滑缩到窗口尺寸
        //   只有一两毫秒，完全在每帧预算内；而最近邻扔像素带来的锯齿和马赛克
        //   是肉眼实打实看得见的。清晰度归清晰度，透明度归透明度 —— 这层图
        //   只该淡，不该糊。
        m_bgCache = m_bgCover.scaled(size(), Qt::KeepAspectRatioByExpanding,
                                     Qt::SmoothTransformation);
        m_bgCacheSize = size();
    }

    // 0.13 是调出来的：再高一点文字就开始发灰、看不清了。
    p.setOpacity(0.13);
    p.drawPixmap((width()  - m_bgCache.width())  / 2,
                 (height() - m_bgCache.height()) / 2,
                 m_bgCache);
    p.setOpacity(1.0);
}

void PlayerPage::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    m_bgCacheSize = QSize();          // 作废缓存，下次重绘按新尺寸重缩
}

void PlayerPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);

    // 切回播放器页时，把当前歌词行对到可视区中间。
    // ★ 瞬时定位，不走动画 ★ —— 页面刚出现就自己滚两下很晃眼。
    // ★ 用 singleShot(0) 推到布局落定之后 ★ —— showEvent 这会儿 viewport
    //   高度还是旧的，直接算会偏。
    if (m_lyricFollow && m_lyricCurrent >= 0 && m_lyricCurrent < m_lyricLabels.size())
    {
        QTimer::singleShot(0, this, [this]() {
            if (!m_lyricFollow || m_lyricCurrent < 0
                || m_lyricCurrent >= m_lyricLabels.size())
                return;
            if (QLayout* lay = m_lyricHost->layout())
                lay->activate();
            m_lyricScroll->verticalScrollBar()->setValue(
                lyricTargetValue(m_lyricLabels.at(m_lyricCurrent)));
        });
    }

    // "上次那个文件夹"只自动加载一次。
    // 为什么不是每次切到这一页都重扫：几万首的库扫一遍要好几秒，
    // 用户只是想切回来看一眼歌词，不该被拖着重扫。
    if (m_autoLoaded)
        return;
    m_autoLoaded = true;

    if (m_lib->count() > 0)
        return;

    QSettings s;
    const QString dir = s.value(QStringLiteral("player/lastDir")).toString();
    if (!dir.isEmpty() && QFileInfo::exists(dir))
        m_lib->scanAsync(dir);
}

// =============================================================================
//  选文件夹 / 扫描
// =============================================================================
void PlayerPage::onPickFolder()
{
    QSettings s;
    const QString start = s.value(QStringLiteral("player/lastDir"), QDir::homePath()).toString();

    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择音乐文件夹（会递归查找所有子文件夹）"), start);
    if (dir.isEmpty())
        return;

    s.setValue(QStringLiteral("player/lastDir"), dir);
    m_searchEdit->clear();            // 换了库，旧的搜索词留着只会让人困惑
    m_lib->scanAsync(dir);
}

void PlayerPage::onScanStarted()
{
    clearLyrics(QStringLiteral("还没有在放歌"));
    m_bgCover = QPixmap();
    m_bgCache = QPixmap();
    m_coverFromTheme = false;         // 兜底状态一并清掉，别让之后的 refresh 误以为还在垫图
    update();

    m_countLabel->setText(QStringLiteral("正在扫描…"));
    m_model->setView(m_lib->search(m_searchEdit->text()));
}

void PlayerPage::onScanBatch()
{
    // 每一批都刷新列表：让用户看到歌在往里冒，而不是"卡了很久然后一次全出来"。
    // 搜索框里有词的话就顺着当前关键词重筛 —— 扫到一半也不影响搜索。
    m_model->setView(m_lib->search(m_searchEdit->text()));
    updateCountLabel();
}

void PlayerPage::onScanFinished(int total, int skipped)
{
    updateCountLabel();
    m_model->setView(m_lib->search(m_searchEdit->text()));

    if (total == 0)
        m_countLabel->setText(QStringLiteral("这个文件夹里没找到音频文件"));
    else if (skipped > 0)
        m_countLabel->setText(QStringLiteral("共 %1 首（其中 %2 首读不到标签，用的是文件名）")
                                  .arg(total).arg(skipped));
}

void PlayerPage::updateCountLabel()
{
    // 上一首 / 下一首只在真的有歌可跳时才亮。
    // ★ 这两行必须放在下面那个"扫描中直接 return"之前 ★ ——
    //   扫描开始那一刻列表会先清空，要是被 return 挡住，
    //   按钮就会停在"亮着但点了没反应"的状态，直到扫完才恢复。
    const bool hasTracks = (m_model->rowCount() > 0);
    m_prevBtn->setEnabled(hasTracks);
    m_nextBtn->setEnabled(hasTracks);

    if (m_lib->isScanning())
        return;                        // 扫描中的文字由 scanProgress 那条路负责

    const int total = m_lib->count();
    const int shown = m_model->rowCount();

    if (total == 0 && m_lib->rootDir().isEmpty())
        m_countLabel->setText(QStringLiteral("点左边选个文件夹"));
    else if (shown == total)
        m_countLabel->setText(QStringLiteral("共 %1 首").arg(total));
    else
        m_countLabel->setText(QStringLiteral("筛出 %1 / %2 首").arg(shown).arg(total));
}

// =============================================================================
//  搜索
// =============================================================================
void PlayerPage::onSearchChanged()
{
    m_searchTimer->start();            // 防抖：连打十个字只搜一次本地
    m_onlineTimer->start();            // 联网搜索独立 600ms 防抖（结果追加在本地之后）
}

void PlayerPage::doSearch()
{
    // 打了新词 = 离开收藏/下载视图（这两个只是视图开关，不打断浏览）
    if (m_favView || m_dlView)
    {
        m_favView = false;
        m_dlView  = false;
        m_favBtn->blockSignals(true);
        m_favBtn->setChecked(false);
        m_favBtn->blockSignals(false);
        m_dlBtn->blockSignals(true);
        m_dlBtn->setChecked(false);
        m_dlBtn->blockSignals(false);
    }

    m_model->setView(m_lib->search(m_searchEdit->text()));
    updateCountLabel();
}

// =============================================================================
//  播放
// =============================================================================
void PlayerPage::onRowActivated(const QModelIndex& index)
{
    // 联网行：下载 → （B站还要 MF 转码）→ 播放；本地行：照旧
    if (m_model->isOnlineRow(index.row()))
    {
        playOnlineRow(index.row());
        return;
    }
    playRow(index.row());
}

void PlayerPage::playRow(int row)
{
    const int lib = m_model->libraryIndexAt(row);
    if (lib >= 0)
        playLibraryIndex(lib);
}

void PlayerPage::playLibraryIndex(int libIndex)
{
    const Track* t = m_lib->trackAt(libIndex);
    if (!t)
        return;

    QString err;
    bool loadOk = false;
    if (t->path.startsWith(QStringLiteral("http")))
    {
        // 联网行（流式播放，不落盘）：Referer 按域名给
        QUrl u(t->path);
        QMap<QString, QString> headers;
        headers.insert(QStringLiteral("Referer"),
                       u.host().contains(QStringLiteral("163"))
                           ? QStringLiteral("https://music.163.com")
                           : QStringLiteral("https://www.bilibili.com"));
        loadOk = m_player->loadOnline(u, t->durationMs, headers, &err);
    }
    else
    {
        loadOk = m_player->load(t->path, &err);
    }
    if (!loadOk)
    {
        // 播不了就明说（多半是格式不支持），别一声不吭 —— 用户会以为程序坏了
        m_nowLabel->setText(QStringLiteral("%1：%2").arg(t->title, err));
        return;
    }

    m_playingLib = libIndex;
    m_model->setPlayingLibraryIndex(libIndex);

    // 列表里的选中行跟着走，也顺便滚到可视区
    const int row = m_model->rowOfLibraryIndex(libIndex);
    if (row >= 0)
    {
        const QModelIndex idx = m_model->index(row, 0);
        m_list->setCurrentIndex(idx);
        m_list->scrollTo(idx, QAbstractItemView::EnsureVisible);
    }

    loadLyricsAndCover(t->path);
    updateNowPlaying();
    m_player->play();

    // 换歌事实成立：报出去（桌面歌词报歌名用）。暂停/恢复/seek 不走这条路。
    emit currentTrackChanged(t->title);
}

// =============================================================================
//  拖到桌宠身上的音乐文件：入列并立即播放
//
//  注入走 MusicLibrary::appendBatch —— 扫描线程的批次也从它过（ queued 回
//  主线程执行），这里复用同一条路，不另开后门；debugInjectTracks 是先例。
//  元数据用 TrackMeta::readMeta 现读（和"点了这首才读"是同一个原则），
//  durationMs 读不到就先 0（列表显示 "--:--"，播放时进度条自己会拿到真时长）。
// =============================================================================
void PlayerPage::playDroppedFiles(const QStringList& paths)
{
    const int firstLib = m_lib->count();

    QVector<Track> tracks;
    for (const QString& path : paths)
    {
        if (!QFileInfo::exists(path))
            continue;

        const TrackMeta::Meta meta = TrackMeta::readMeta(path);
        Track t;
        t.path       = path;
        t.title      = meta.title.isEmpty() ? QFileInfo(path).completeBaseName()
                                            : meta.title;
        t.artist     = meta.artist;
        t.durationMs = meta.durationMs;
        t.hasLyrics  = !meta.lyrics.isEmpty();
        t.hasCover   = meta.hasCover();
        tracks.append(t);
    }

    if (tracks.isEmpty())
        return;                            // 一个能用的都没有：什么都不发生

    m_lib->appendBatch(tracks);
    m_searchEdit->clear();                 // 清掉过滤词，新歌才在列表里看得见
    doSearch();
    updateCountLabel();

    playLibraryIndex(firstLib);            // 从拖进来的第一首开始放
}

void PlayerPage::playPrev()
{
    const int row = m_model->rowOfLibraryIndex(m_playingLib);
    const int count = m_model->rowCount();
    if (count == 0)
        return;

    // 到第一首再往前 -> 绕到最后一首（"上一首"永远按得动，不用用户先滚到底）
    // ★ 手动的上一首/下一首永远按列表顺序走，不受播放模式影响 ★
    //   和网易云一致：单曲循环下用户主动按"下一首"，意思就是"这首我不听了"。
    playRow(row <= 0 ? count - 1 : row - 1);
}

void PlayerPage::playNext()
{
    const int row = m_model->rowOfLibraryIndex(m_playingLib);
    const int count = m_model->rowCount();
    if (count == 0)
        return;

    // 随机：从**别的**行里随便挑一首（挑到自己会看着像"点了没反应"）
    if (m_mode == PlayMode::Shuffle)
    {
        playRow(randomOtherRow(row, count));
        return;
    }

    // 顺序 / 单曲循环：手动的下一首仍按顺序往下（理由同 playPrev）
    playRow(row < 0 ? 0 : (row + 1) % count);
}

// ---- 播放模式：随机 / 顺序 / 单曲循环 ----
//   点一下换下一种：顺序 → 随机 → 单曲循环 → 顺序…（这个环形顺序就是"点一下切换"）
//   换完立刻存盘 —— 和音量一样，下次打开还是这个模式。
void PlayerPage::onModeClicked()
{
    switch (m_mode)
    {
    case PlayMode::Order:     m_mode = PlayMode::Shuffle;   break;
    case PlayMode::Shuffle:   m_mode = PlayMode::RepeatOne; break;
    case PlayMode::RepeatOne: m_mode = PlayMode::Order;     break;
    }
    applyMode();

    QSettings s;
    s.setValue(QStringLiteral("player/mode"), int(m_mode));
}

// 把当前模式刷到模式键上：换个图标 + 更新提示语。
//   ★ 提示语里要写清楚"现在是什么、点一下会变成什么" ★
//     这三个图标很小，光看图形未必分得清"顺序"和"循环"，
//     而"点一下切换"这件事本身也得说一声，不然用户不知道能点。
void PlayerPage::applyMode()
{
    QString tip;
    switch (m_mode)
    {
    case PlayMode::Shuffle:
        m_modeBtn->setKind(PlayIconKind::Shuffle);
        tip = QStringLiteral("随机播放（点一下换成单曲循环）");
        break;
    case PlayMode::Order:
        m_modeBtn->setKind(PlayIconKind::Order);
        tip = QStringLiteral("顺序播放（点一下换成随机播放）");
        break;
    case PlayMode::RepeatOne:
        m_modeBtn->setKind(PlayIconKind::RepeatOne);
        tip = QStringLiteral("单曲循环（点一下换成顺序播放）");
        break;
    }
    m_modeBtn->setToolTip(tip);
}

// 随机挑一行，且不会挑到 curRow 那一行。
//   ★ 只有一行的时候没得挑，还回 0（此时 curRow 必然也是 0）★
//   ★ curRow 可能是 -1（还没在放歌），这时随便挑哪行都行，循环条件天然不成立 ★
int PlayerPage::randomOtherRow(int curRow, int count) const
{
    if (count <= 1)
        return 0;

    int r = curRow;
    while (r == curRow)
        r = int(QRandomGenerator::global()->bounded(quint32(count)));
    return r;
}

void PlayerPage::togglePlayPause()
{
    if (!m_player->hasTrack())
    {
        // 还没选歌：按播放键就放列表里的第一首 —— 比"什么都不发生"友好
        if (m_model->rowCount() > 0)
            playRow(0);
        return;
    }
    m_player->togglePlayPause();
}

// 一首放完了，接下来往哪走 —— 这里才是播放模式真正起作用的地方：
//   · 单曲循环 —— 原地重播这一首
//   · 随机     —— 随便挑另一首
//   · 顺序     —— 下一首（到头绕回第一首）
void PlayerPage::onTrackFinished()
{
    switch (m_mode)
    {
    case PlayMode::RepeatOne:
        if (m_playingLib >= 0)
        {
            playLibraryIndex(m_playingLib);   // 重播（重新 load + play）
            return;
        }
        break;

    case PlayMode::Shuffle:
        {
            const int row   = m_model->rowOfLibraryIndex(m_playingLib);
            const int count = m_model->rowCount();
            if (count > 0)
            {
                playRow(randomOtherRow(row, count));
                return;
            }
        }
        break;

    case PlayMode::Order:
        break;
    }

    playNext();                        // 顺序 / 上面两条没走成的兜底：放下一首
}

// =============================================================================
//  播放状态回报
// =============================================================================
void PlayerPage::onPositionChanged(qint64 ms)
{
    if (m_userSeeking)
        return;                        // 用户正拖着进度条，别跟他抢

    // 避免"进度条把自己设回去"引发递归：只在值真的变了时 setValue
    const int v = int(ms);
    if (m_slider->value() != v)
        m_slider->setValue(v);

    m_posLabel->setText(formatMs(ms));

    // 歌词跟着唱
    if (!m_lyricLines.isEmpty())
    {
        const int idx = lyricIndexAt(m_lyricLines, ms);
        if (idx != m_lyricCurrent)
            highlightLyric(idx);
    }
}

void PlayerPage::onDurationChanged(qint64 ms)
{
    m_slider->setRange(0, int(qMax<qint64>(0, ms)));
    m_durLabel->setText(ms > 0 ? formatMs(ms) : QStringLiteral("--:--"));
}

void PlayerPage::onPlayerStateChanged()
{
    // 播放键的图标自己切（三角 / 双竖条），不再靠换一个字符 ——
    // 换字符会让按钮内容重算尺寸，个别字体下还会左右跳一下。
    const bool playing = m_player->isPlaying();
    m_playBtn->setKind(playing ? PlayIconKind::Pause : PlayIconKind::Play);
    m_playBtn->setToolTip(playing ? QStringLiteral("暂停") : QStringLiteral("播放"));

    // 桌面歌词工具条上的播放键图标同步
    if (m_desktopLyrics)
        m_desktopLyrics->setPlaying(playing);

    updateNowPlaying();
}

void PlayerPage::onSeekPressed()
{
    m_userSeeking = true;
}

void PlayerPage::onSeekMoved(int value)
{
    // 拖动中只改时间文字，不 seek —— 每动一像素就 seek 会让声音一顿一顿的
    m_posLabel->setText(formatMs(value));
}

void PlayerPage::onSeekReleased()
{
    m_userSeeking = false;
    m_player->seekToMs(m_slider->value());
}

// =============================================================================
//  歌词 / 封面 / 当前曲目
// =============================================================================
void PlayerPage::loadLyricsAndCover(const QString& path)
{
    // ★ 只在这里读一次文件 ★
    //   列表里没有封面缩略图、索引里也没有歌词正文（见 Track/MusicLibrary 的说明），
    //   所有"重"的数据都是等用户真的点了这一首才读的 —— 这就是几万首也不卡的原因。
    const TrackMeta::Meta meta = TrackMeta::readMeta(path);

    // 封面：先缩到 1280 以内再留着当背景。★ 别贪小压到 640 ★ —— 铺满屏幕时
    // 窗口宽约 1920，源图只有 640 的话必然放大 3 倍，再平滑也会发虚；1280 的源
    // 配上 paintEvent 里的平滑缩放，最大化也够清晰。免得每次重绘都缩原图，所以
    // 保留预缩这一步（重绘用的是缓存，见 paintEvent）。1280² 的 pixmap 约 6.5MB，
    // 只留当前这一首，可以接受。
    if (meta.hasCover())
    {
        QImage img;
        img.loadFromData(meta.cover);
        if (!img.isNull())
        {
            m_coverFromTheme = false;      // 这首有自己的封面，主题图靠边站
            if (img.width() > 1280 || img.height() > 1280)
                img = img.scaled(1280, 1280, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            m_bgCover = QPixmap::fromImage(img);
            m_coverLabel->setPixmap(m_bgCover.scaled(m_coverLabel->size(),
                                                     Qt::KeepAspectRatioByExpanding,
                                                     Qt::SmoothTransformation));
            m_bgCacheSize = QSize();       // 作废铺满缓存
        }
        else
        {
            applyFallbackCover();          // 封面数据坏了 —— 当作没有，走兜底
        }
    }
    else
    {
        applyFallbackCover();              // 没有内嵌封面 —— 拿主题背景图顶上
    }
    update();                              // 背景要重画

    // 歌词
    if (meta.lyrics.isEmpty())
    {
        clearLyrics(QStringLiteral("这首歌没有歌词"));
    }
    else
    {
        const QVector<TrackMeta::LyricLine> lines = TrackMeta::parseLrc(meta.lyrics);
        if (lines.isEmpty())
        {
            // 有歌词、但没有时间标签（比如内嵌的纯文本）——
            // 逐行显示出来，只是不做高亮跟随。
            QVector<TrackMeta::LyricLine> plain;
            const QStringList rows = meta.lyrics.split(QRegularExpression(QStringLiteral("[\\r\\n]")),
                                                       Qt::SkipEmptyParts);
            for (const QString& r : rows)
                plain.append(TrackMeta::LyricLine{ -1, r.trimmed() });

            if (plain.isEmpty())
                clearLyrics(QStringLiteral("这首歌没有歌词"));
            else
                rebuildLyricLabels(plain);
        }
        else
        {
            rebuildLyricLabels(lines);
        }
    }
}

// =============================================================================
//  主题图兜底
//
//  这首歌没有内嵌封面（或者封面数据是坏的）时，拿用户在主题设置里导入的
//  背景图垫上 —— 44px 的小封面和整页的淡化背景都来自同一张（m_bgCover），
//  所以"换一张"只需要换 m_bgCover 一处。
//  有内嵌封面的歌照走原路（见 loadLyricsAndCover），主题图绝不掺和。
// =============================================================================
void PlayerPage::applyFallbackCover()
{
    m_coverFromTheme = true;           // 现在垫的是主题图 —— 它换了要跟着换

    m_bgCover = themeFallbackCover();
    if (m_bgCover.isNull())
    {
        m_coverLabel->setText(QStringLiteral("\u266B"));   // 连主题图都没设 → 回到 ♫ 占位
    }
    else
    {
        m_coverLabel->setPixmap(m_bgCover.scaled(m_coverLabel->size(),
                                                 Qt::KeepAspectRatioByExpanding,
                                                 Qt::SmoothTransformation));
    }
    m_bgCacheSize = QSize();           // 整页背景的铺满缓存一并作废
}

// 读主题背景图，缩到和内嵌封面同一个量级（1280 以内，理由见 loadLyricsAndCover）。
// ★ 按路径缓存 ★ 主题图不随换歌重来；路径变了（导入新图/清空）才重新读盘。
QPixmap PlayerPage::themeFallbackCover()
{
    const QString path = UiTheme::bgImagePath();
    if (path.isEmpty() || !QFile::exists(path))
        return QPixmap();

    if (path != m_themeCoverPath || m_themeCover.isNull())
    {
        QImage img(path);
        if (img.isNull())
            return QPixmap();          // 读不出来就空着，下次再试（不缓存失败结果）

        if (img.width() > 1280 || img.height() > 1280)
            img = img.scaled(1280, 1280, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        m_themeCover     = QPixmap::fromImage(img);
        m_themeCoverPath = path;
    }
    return m_themeCover;
}

void PlayerPage::refreshFallbackCover()
{
    // 只有正垫着主题图的歌才需要跟进。两个提前返回：
    //   · 这首有自己的封面（m_coverFromTheme == false）—— 主题图换谁的事都跟它无关；
    //   · 路径没变（比如只是拖了不透明度滑条）—— 图一模一样，别白刷一遍。
    if (!m_coverFromTheme)
        return;
    if (m_themeCoverPath == UiTheme::bgImagePath())
        return;

    applyFallbackCover();
    update();
}

void PlayerPage::rebuildLyricLabels(const QVector<TrackMeta::LyricLine>& lines)
{
    clearLyrics(QString());

    m_lyricLines = lines;
    m_lyricLabels.reserve(lines.size());

    for (const TrackMeta::LyricLine& line : std::as_const(lines))
    {
        auto* lab = new QLabel(line.text, m_lyricHost);
        lab->setWordWrap(true);
        lab->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        // 字号 / 颜色 / 内边距全在样式表的 QLabel#playerLyricLine 里 ——
        // 这里只贴个名字，改字号时不用回来动这一行（见 applyStyle 的说明）。
        lab->setObjectName(QString::fromLatin1(kLyricRoleNormal));
        // 插在最后的 stretch 之前
        m_lyricLay->insertWidget(m_lyricLay->count() - 1, lab);
        m_lyricLabels.append(lab);

        // 有时间戳的行可以双击跳进度（见 seekToLyric）—— 手型光标给个暗示。
        // ★ 过滤器必须装在每一行上 ★ eventFilter 里判的是"watched 是哪一行"，
        // 上一版就是漏了这行 installEventFilter，双击事件压根到不了处理逻辑。
        if (line.timeMs >= 0)
        {
            lab->setCursor(Qt::PointingHandCursor);
            lab->installEventFilter(this);
        }
    }

    m_lyricCurrent = -1;
    m_lyricHeader->setText(QStringLiteral("歌词 · %1 行").arg(lines.size()));

    // 新歌装好了但还没唱到第一句 —— 悬浮窗先收着，唱到第一行再出
    updateDesktopLyrics();
}

void PlayerPage::clearLyrics(const QString& message)
{
    m_lyricLines.clear();
    m_lyricCurrent = -1;

    // 换歌/清空 = 滚动状态一并归零：恢复自动跟随、停动画、收按钮、回到顶部。
    // （clearLyrics 在构造里也会被调一次，那时动画/按钮还没建，判空挡一下。）
    if (m_lyricAnim)
        m_lyricAnim->stop();
    if (m_lyricFollowTimer)
        m_lyricFollowTimer->stop();
    if (m_lyricRecenterBtn)
        m_lyricRecenterBtn->hide();
    m_lyricFollow = true;
    m_lyricScroll->verticalScrollBar()->setValue(0);

    // 删掉除"末尾那根 stretch"以外的全部控件
    while (m_lyricLay->count() > 1)
    {
        QLayoutItem* item = m_lyricLay->takeAt(0);
        if (QWidget* w = item->widget())
            w->deleteLater();
        delete item;
    }
    m_lyricLabels.clear();

    if (!message.isEmpty())
    {
        auto* lab = new QLabel(message, m_lyricHost);
        lab->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        lab->setObjectName(QStringLiteral("playerLyricHint"));   // 样式同样在 QSS 里
        m_lyricLay->insertWidget(0, lab);
        m_lyricHeader->setText(QStringLiteral("歌词"));
    }

    // 换歌/清空 = 桌面歌词一并收起来（updateDesktopLyrics 会喂空 → 隐藏）
    updateDesktopLyrics();
}

void PlayerPage::highlightLyric(int index)
{
    if (index < 0 || index >= m_lyricLabels.size())
        return;

    // 只切 objectName，剩下的交给样式表 —— 详见函数上方那段说明。
    if (m_lyricCurrent >= 0 && m_lyricCurrent < m_lyricLabels.size())
        applyLyricRole(m_lyricLabels.at(m_lyricCurrent), kLyricRoleNormal);

    m_lyricCurrent = index;

    QLabel* cur = m_lyricLabels.at(index);
    applyLyricRole(cur, kLyricRoleActive);

    // ★ 先把布局算完，再滚动 ★
    //   当前行会从 15px 变成 17px，上面所有行的位置都会往下挪一点点。
    //   布局重算默认是"排到事件循环下一轮"，这时候直接滚动会按**旧位置**定位，
    //   于是高亮那行总是偏上一点点。手动 activate() 一次就对齐了。
    if (QLayout* lay = m_lyricHost->layout())
        lay->activate();

    // ★ 跟随中才滚，而且滚是"缓动滚过去"，不是跳过去 ★
    //   用户正手动翻歌词时（m_lyricFollow == false）只高亮不抢滚动 ——
    //   高亮照样一格格走，视图停在用户放的地方（网易云同款行为）。
    if (m_lyricFollow)
        animateLyricTo(lyricTargetValue(cur));

    // 桌面歌词跟着换行（开着的话）；没开着时 setLine 是空操作
    updateDesktopLyrics();
}

// =============================================================================
//  歌词平滑滚动
// =============================================================================
int PlayerPage::lyricTargetValue(QLabel* line) const
{
    // 当前行中心对齐可视区中心，收在滚动条行程内。
    // 行的 y() 是相对 m_lyricHost 的 —— host 是 viewport 的 widget，
    // 坐标和滚动值同系，直接算就行。
    const QScrollBar* bar = m_lyricScroll->verticalScrollBar();
    const int target = line->y() + line->height() / 2
                       - m_lyricScroll->viewport()->height() / 2;
    return qBound(0, target, bar->maximum());
}

void PlayerPage::animateLyricTo(int target)
{
    QScrollBar* bar = m_lyricScroll->verticalScrollBar();
    if (bar->maximum() == 0)
        return;                        // 内容还没超出一屏，没什么可滚的

    if (qAbs(bar->value() - target) < 2)
        return;                        // 已经在位上了，别为 1px 抖一下

    // stop() 再 start()：行连续切换时从**当前值**接着滚，
    // 不是每次都从旧行起点重滚一遍 —— 快歌连续换行时才顺滑。
    m_lyricAnim->stop();
    m_lyricAnim->setStartValue(bar->value());
    m_lyricAnim->setEndValue(target);
    m_lyricAnim->start();
}

void PlayerPage::pauseLyricFollow()
{
    if (m_lyricFollow)
    {
        m_lyricFollow = false;
        m_lyricAnim->stop();           // 滚到一半被用户接手：动画立刻让位
    }

    // 已暂停时再滚：把"自动恢复"的闹钟往后推（从最后一次操作算起 4 秒）
    if (m_lyricCurrent >= 0 && m_lyricScroll->verticalScrollBar()->maximum() > 0)
        m_lyricRecenterBtn->setVisible(true);
    m_lyricFollowTimer->start();
}

void PlayerPage::resumeLyricFollow()
{
    m_lyricFollowTimer->stop();
    m_lyricRecenterBtn->hide();

    if (m_lyricFollow)
        return;                        // 本来就在跟着（比如 4 秒到了但用户已经点过按钮）

    m_lyricFollow = true;

    // 归位：滚回当前行。布局先 activate（高亮行字号变化的理由见 highlightLyric）
    if (m_lyricCurrent >= 0 && m_lyricCurrent < m_lyricLabels.size())
    {
        if (QLayout* lay = m_lyricHost->layout())
            lay->activate();
        animateLyricTo(lyricTargetValue(m_lyricLabels.at(m_lyricCurrent)));
    }
}

void PlayerPage::placeLyricRecenterBtn()
{
    if (!m_lyricRecenterBtn || !m_lyricScroll)
        return;

    const QSize sz = m_lyricRecenterBtn->sizeHint();
    m_lyricRecenterBtn->setGeometry((m_lyricScroll->width()  - sz.width())  / 2,
                                    m_lyricScroll->height() - sz.height() - 10,
                                    sz.width(), sz.height());
}

bool PlayerPage::eventFilter(QObject* watched, QEvent* event)
{
    // 双击歌词行 = 跳到这一句。m_lyricLabels 里只存活着的行（clearLyrics 会
    // 连人带名一起清），所以 indexOf 命中的必然是还活着的那个。
    if (event->type() == QEvent::MouseButtonDblClick)
    {
        if (QLabel* lab = qobject_cast<QLabel*>(watched))
        {
            const int idx = m_lyricLabels.indexOf(lab);
            if (idx >= 0)
                seekToLyric(idx);
        }
    }
    // 滚轮 = 用户要自己看歌词：暂停自动跟随。装在 viewport 和滚动条两处 ——
    // 鼠标悬在滚动条上滚的时候，事件走的是滚动条，不过 viewport。
    else if (event->type() == QEvent::Wheel
        && (watched == m_lyricScroll->viewport()
            || watched == m_lyricScroll->verticalScrollBar()))
    {
        pauseLyricFollow();
    }
    else if (watched == m_lyricScroll && event->type() == QEvent::Resize)
    {
        // 歌词区尺寸变了（拖面板/放大还原），悬浮按钮跟着挪
        placeLyricRecenterBtn();
    }
    return QWidget::eventFilter(watched, event);
}

// =============================================================================
//  桌面歌词
// =============================================================================
void PlayerPage::updateDesktopLyrics()
{
    if (!m_desktopLyrics || !m_player)
        return;                        // 构造早期（buildUi 里的 clearLyrics）还没建

    // ★ 显隐原则（对着网易云来的）★
    //   · 开关关了 / 连歌都没加载 → 整窗隐藏（确实没什么可显示的）；
    //   · 有歌但这首没有歌词 → 窗口留着，显示"暂无歌词"占位 —— 悬浮窗是
    //     用户摆在桌面上的，一句没词就整窗消失、下一首有词又冒出来，
    //     看着像坏了（这就是"切歌后桌面歌词没了"的原因）；
    //   · 有歌词但还没唱到第一句 → 先把第一句垫在窗口上，唱到哪儿从哪儿换；
    //   · 正常跟唱 → 当前行 + 下一行。
    if (!m_dtLyricsEnabled || !m_player->hasTrack())
    {
        m_desktopLyrics->setLine(QString(), QString());
        return;
    }

    if (m_lyricCurrent >= 0 && m_lyricCurrent < m_lyricLines.size())
    {
        QString next;
        if (m_lyricCurrent + 1 < m_lyricLines.size())
            next = m_lyricLines.at(m_lyricCurrent + 1).text;
        m_desktopLyrics->setLine(m_lyricLines.at(m_lyricCurrent).text, next);
        return;
    }

    if (!m_lyricLines.isEmpty())
    {
        QString next;
        if (m_lyricLines.size() > 1)
            next = m_lyricLines.at(1).text;
        m_desktopLyrics->setLine(m_lyricLines.at(0).text, next);
        return;
    }

    m_desktopLyrics->setLine(QStringLiteral("暂无歌词"), QString());
}

void PlayerPage::setDesktopLyricsEnabled(bool on)
{
    m_dtLyricsEnabled = on;
    DesktopLyrics::saveEnabled(on);    // 立刻落盘 —— 和字号/主题一个规矩
    updateDesktopLyrics();             // 开了就立刻按当前状态显示，不用等下一句

    // 广播出去：设置页的勾选框要跟着同步（悬浮窗 × 关闭是反向路径）
    emit desktopLyricsEnabledChanged(on);
}

// =============================================================================
//  双击歌词跳进度
// =============================================================================
void PlayerPage::seekToLyric(int index)
{
    if (index < 0 || index >= m_lyricLines.size())
        return;

    const qint64 t = m_lyricLines.at(index).timeMs;
    if (t < 0)
        return;                        // 内嵌纯文本行没有时间戳，无处可跳

    // 跳完这一句就是"当前句"—— 顺手把自动跟随恢复掉。
    // 用户多半是先滚开歌词找到这一句的（正处于暂停跟随），这里直接置回跟随
    // 而不是走 resumeLyricFollow()：后者会往**旧的**当前行滚，方向就反了。
    // 置回后下面 highlightLyric 的缓动会自己滚向新当前行。
    m_lyricFollow = true;
    m_lyricFollowTimer->stop();
    m_lyricRecenterBtn->hide();

    m_player->seekToMs(t);             // 超出范围会被夹到 [0, 时长]（AudioPlayer 管）

    // 界面立刻跟上，不等下一轮 positionChanged 轮询 ——
    // 暂停状态下等它回报要干等一小会儿，双击就该"啪"地到位。
    if (m_slider->value() != int(t))
        m_slider->setValue(int(t));
    m_posLabel->setText(formatMs(t));
    highlightLyric(lyricIndexAt(m_lyricLines, t));
}

// =============================================================================
//  杂项
// =============================================================================
void PlayerPage::updateNowPlaying()
{
    const Track* t = m_lib->trackAt(m_playingLib);
    if (!t)
    {
        m_nowLabel->setText(QStringLiteral("没在放歌"));
        return;
    }

    QString text = t->title;
    if (!t->artist.isEmpty())
        text += QStringLiteral("  ·  ") + t->artist;

    m_nowLabel->setText(text);
}

QString PlayerPage::formatMs(qint64 ms)
{
    if (ms < 0)
        ms = 0;

    const qint64 total = ms / 1000;
    const qint64 h = total / 3600;
    const qint64 m = (total % 3600) / 60;
    const qint64 s = total % 60;

    if (h > 0)
        return QStringLiteral("%1:%2:%3").arg(h)
                   .arg(m, 2, 10, QLatin1Char('0'))
                   .arg(s, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

// =============================================================================
//  给自检用
// =============================================================================
void PlayerPage::debugInjectTracks(const QVector<Track>& tracks)
{
    // 走 MusicLibrary 的正式入口（追加 + 建索引 + 发 scanBatch），
    // 不是另开一条后门 —— 所以截图里看到的就是真实那条路跑出来的界面。
    m_lib->appendBatch(tracks);
    doSearch();
    updateCountLabel();
}

// =============================================================================
//  --selftest：把播放器里那几条"光看代码看不出对错"的路径跑一遍
//
//  ★ 样本文件是现场拼出来的，不依赖这台机器上有没有歌 ★
//    音频格式就那么几种容器，头部结构是公开且固定的 —— 手写几十字节就能造出
//    一个"标签齐全、时长可验证"的样本。比起往仓库里塞几个真 mp3（版权 + 体积），
//    这条路既干净又能**精确断言**：我知道该解出什么，解出来不是那样就是有 bug。
//
//  ★ 时长也一起验了 ★
//    标签解析最容易被忽略的就是时长：WAV 要读 RIFF 里的字节率、
//    FLAC 要拆 STREAMINFO 那 8 个打包位、MP4 要认 mvhd、MP3 只能估算。
//    四个样本各自走一条不同的路，全都对上了才算这四段代码是活的。
// =============================================================================
namespace {

QByteArray be32(quint32 v)
{
    QByteArray b(4, '\0');
    b[0] = char((v >> 24) & 0xFF);
    b[1] = char((v >> 16) & 0xFF);
    b[2] = char((v >> 8) & 0xFF);
    b[3] = char(v & 0xFF);
    return b;
}

QByteArray be16(quint16 v)
{
    QByteArray b(2, '\0');
    b[0] = char((v >> 8) & 0xFF);
    b[1] = char(v & 0xFF);
    return b;
}

QByteArray le32(quint32 v)
{
    QByteArray b(4, '\0');
    b[0] = char(v & 0xFF);
    b[1] = char((v >> 8) & 0xFF);
    b[2] = char((v >> 16) & 0xFF);
    b[3] = char((v >> 24) & 0xFF);
    return b;
}

QByteArray le16(quint16 v)
{
    QByteArray b(2, '\0');
    b[0] = char(v & 0xFF);
    b[1] = char((v >> 8) & 0xFF);
    return b;
}

QByteArray syncSafe(quint32 v)
{
    QByteArray b(4, '\0');
    b[0] = char((v >> 21) & 0x7F);
    b[1] = char((v >> 14) & 0x7F);
    b[2] = char((v >> 7) & 0x7F);
    b[3] = char(v & 0x7F);
    return b;
}

bool writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    return f.write(data) == data.size();
}

// 一张 2x2 的真 PNG（拿 Qt 自己编，省得手算 CRC 写死字节）
QByteArray tinyPng()
{
    QImage img(2, 2, QImage::Format_RGB32);
    img.fill(QColor(0x7F, 0x77, 0xDD));

    QByteArray out;
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    buf.close();
    return out;
}

// ID3v2.4 的一个帧：id + syncsafe 大小 + flags + 数据
QByteArray id3Frame(const char* id, const QByteArray& payload)
{
    QByteArray f(id);
    f.append(syncSafe(quint32(payload.size())));
    f.append("\0\0", 2);                 // frame flags
    f.append(payload);
    return f;
}

// 带"编码字节"的文本帧（TIT2 / TPE1 / TALB）
QByteArray id3TextFrame(const char* id, quint8 enc, const QByteArray& text)
{
    QByteArray payload;
    payload.append(char(enc));
    payload.append(text);
    return id3Frame(id, payload);
}

// FLAC 的块头：1 字节（last + type）+ 3 字节长度
QByteArray flacBlockHeader(bool last, int type, int len)
{
    QByteArray b;
    b.append(char((last ? 0x80 : 0x00) | (type & 0x7F)));
    b.append(char((len >> 16) & 0xFF));
    b.append(char((len >> 8) & 0xFF));
    b.append(char(len & 0xFF));
    return b;
}

// 一个完整的 mp4 atom：大小 + 名字 + 内容
QByteArray atom(const char* name, const QByteArray& payload)
{
    QByteArray a;
    a.append(be32(quint32(8 + payload.size())));
    a.append(name, 4);
    a.append(payload);
    return a;
}

struct Sample
{
    QString fileName;
    QString title;
    QString artist;
    int     durationMs = 0;
    bool    cover  = false;
    bool    lyrics = false;
};

} // namespace

QString PlayerPage::describePlayer()
{
    QString out;

    // ---------------- ① 音频后端 ----------------
    out += QStringLiteral("--- 音频后端 ---\r\n");
    {
        AudioPlayer player;
        out += player.describe();
    }
    out += QStringLiteral("\r\n");

    // ---------------- ② 标签解析：现场拼样本 ----------------
    out += QStringLiteral("--- 标签解析（样本是现场拼的，不依赖机器上有没有歌）---\r\n");

    QTemporaryDir tmp;
    if (!tmp.isValid())
    {
        out += QStringLiteral("  [!!] 建不了临时目录，后面的扫描/搜索两节跳过\r\n");
        return out;
    }

    const QByteArray png = tinyPng();

    // 本地编码（中文 Windows = GBK）—— 专门用来测"标着 Latin-1 其实是 GBK"那条降级链
    //
    // ★ 返回类型必须写死成 QByteArray，不能用 auto ★
    //   QStringEncoder::operator() 返回的**不是** QByteArray，而是一个代理对象
    //   `DecodedData<const QString&>` —— 里面只存了 encoder 指针和字符串引用。
    //   写成 `return e(s);` 交给 auto 推导的话，lambda 返回的就是那个代理，
    //   而它指向的局部 e 在函数返回时已经析构了；等外面要 QByteArray 触发隐式
    //   转换时，就会去解引用一个死对象 —— 直接段错误（2026-09-24 实际踩到）。
    //   显式写 -> QByteArray 就没事：转换在 e 还活着的时候完成。
    //   TrackMeta.h 里 decodeSmart() 是同一个道理的安全写法（赋给显式 QString）。
    const auto localBytes = [](const QString& s) -> QByteArray {
        QStringEncoder e(QStringConverter::System);
        return e(s);
    };

    // ---- 样本 1：MP3，标签齐全（UTF-8 标题 / GBK 艺术家 / 封面 / 内嵌歌词）----
    constexpr int MP3_AUDIO_BYTES = 32000;      // 128 kbps 下约 2 秒
    {
        QByteArray body;
        body += id3TextFrame("TIT2", 3, QStringLiteral("测试歌曲").toUtf8());
        body += id3TextFrame("TPE1", 0, localBytes(QStringLiteral("测试歌手")));   // ★ 降级链
        body += id3TextFrame("TALB", 3, QStringLiteral("测试专辑").toUtf8());

        QByteArray apic;
        apic.append(char(0));                    // 编码字节：Latin-1（mime 和描述都是 ASCII）
        apic.append("image/png");
        apic.append(char(0));                    // mime 结束
        apic.append(char(3));                    // picture type = 封面
        apic.append(char(0));                    // 描述为空
        apic.append(png);
        body += id3Frame("APIC", apic);

        QByteArray uslt;
        uslt.append(char(3));                    // UTF-8
        uslt.append("chi");                      // 语言
        uslt.append(char(0));                    // 内容描述为空
        uslt.append(QStringLiteral("[00:01.00]内嵌歌词第一句\n[00:03.50]内嵌歌词第二句").toUtf8());
        body += id3Frame("USLT", uslt);

        QByteArray file;
        file.append("ID3");
        file.append(char(4));                    // v2.4（帧大小用 syncsafe）
        file.append(char(0));
        file.append(char(0));                    // flags
        file.append(syncSafe(quint32(body.size())));
        file.append(body);

        // 后面接一段"音频"：开头一个合法的 MPEG1 Layer3 帧头（128kbps / 44.1kHz），
        // 时长估算就是拿它算的 —— 整段填 0 也无所谓，解析器只看头。
        QByteArray audio;
        audio.append(char(0xFF));
        audio.append(char(0xFB));
        audio.append(char(0x90));
        audio.append(char(0x00));
        audio.resize(MP3_AUDIO_BYTES, '\0');
        file.append(audio);

        writeFile(tmp.filePath(QStringLiteral("01 - 测试歌手 - 测试歌曲.mp3")), file);
    }

    // ---- 样本 2：WAV，只有时长（8000 字节 8kHz 8bit = 1 秒）----
    constexpr int WAV_DATA_BYTES = 8000;
    {
        QByteArray f;
        f.append("RIFF");
        f.append(le32(quint32(36 + WAV_DATA_BYTES)));
        f.append("WAVE");
        f.append("fmt ");
        f.append(le32(16));
        f.append(le16(1));                        // PCM
        f.append(le16(1));                        // 单声道
        f.append(le32(8000));                     // 采样率
        f.append(le32(8000));                     // 字节率 = 8000 B/s
        f.append(le16(1));                        // block align
        f.append(le16(8));                        // 位深
        f.append("data");
        f.append(le32(quint32(WAV_DATA_BYTES)));
        f.append(QByteArray(WAV_DATA_BYTES, '\0'));

        writeFile(tmp.filePath(QStringLiteral("测试录音.wav")), f);
    }

    // ---- 样本 3：FLAC（44.1kHz / 132300 样本 = 3 秒 / 中文文件名）----
    {
        const quint32 sampleRate   = 44100;
        const quint64 totalSamples = 132300;

        QByteArray si;
        si.append(QByteArray(10, '\0'));          // 块长/帧长那 10 字节，解析器不看
        quint64 packed = (quint64(sampleRate) << 44)
                       | (quint64(0)  << 41)      // 声道数 - 1
                       | (quint64(15) << 36)      // 位深 - 1
                       | totalSamples;            // 低 36 位
        for (int i = 7; i >= 0; --i)
            si.append(char((packed >> (i * 8)) & 0xFF));
        si.append(QByteArray(16, '\0'));          // MD5
        // si.size() == 34，和 FLAC 规范一致

        QByteArray vc;
        const QByteArray vendor("petpal");
        vc.append(le32(quint32(vendor.size())));
        vc.append(vendor);
        const QByteArray c1 = QStringLiteral("TITLE=测试FLAC标题").toUtf8();
        const QByteArray c2 = QStringLiteral("ARTIST=测试艺术家").toUtf8();
        vc.append(le32(2));
        vc.append(le32(quint32(c1.size())));  vc.append(c1);
        vc.append(le32(quint32(c2.size())));  vc.append(c2);

        QByteArray pic;
        pic.append(be32(3));                      // picture type = 封面
        const QByteArray mime("image/png");
        pic.append(be32(quint32(mime.size())));
        pic.append(mime);
        pic.append(be32(0));                      // 描述为空
        pic.append(be32(1));                      // 宽
        pic.append(be32(1));                      // 高
        pic.append(be32(24));                     // 位深
        pic.append(be32(0));                      // 调色板数
        pic.append(be32(quint32(png.size())));
        pic.append(png);

        QByteArray f;
        f.append("fLaC");
        f.append(flacBlockHeader(false, 0, si.size()));  f.append(si);
        f.append(flacBlockHeader(false, 4, vc.size()));  f.append(vc);
        f.append(flacBlockHeader(true,  6, pic.size())); f.append(pic);

        writeFile(tmp.filePath(QStringLiteral("中文 歌名.flac")), f);
    }

    // ---- 样本 4：M4A（moov 里有 mvhd 时长 + ilst 里的标题/艺术家）----
    {
        // ilst 的一个条目长这样：©nam > data > 正文
        //   data 自己是**一个完整的 atom**（有它自己的长度字段）：
        //     size(4) 'data'(4) wellKnown(4) locale(4) payload
        //   ★ 别把 "data" 直接当 payload 拼进去 ★ —— 那样 data 的长度字段就丢了，
        //     解析器会拿 ASCII 的 "data"（0x64617461，十七亿）当长度，
        //     于是整个条目被当成坏数据跳过。2026-09-24 自检就栽在这上面：
        //     明明结构"看着像"，实际上少了 4 个字节，标签一个都读不出来。
        //   所以这里老老实实用 atom("data", ...) 再套一层。
        const auto textItem = [](const char* name, const QString& text) {
            QByteArray body;
            body.append(be32(1));                 // well-known type = 1（UTF-8 文本）
            body.append(be32(0));                 // locale
            body.append(text.toUtf8());
            return atom(name, atom("data", body));
        };

        QByteArray ilst;
        ilst.append(textItem("\xA9" "nam", QStringLiteral("测试MP4标题")));
        ilst.append(textItem("\xA9" "ART", QStringLiteral("测试MP4歌手")));

        // meta 是 fullbox：内容最前面多 4 字节 version/flags（解析器会跳过它）
        QByteArray meta;
        meta.append(be32(0));                     // version + flags
        meta.append(atom("ilst", ilst));

        // mvhd：version 0 / timescale 1000 / duration 5000 -> 5 秒
        QByteArray mvhd;
        mvhd.append(be32(0));                     // version + flags
        mvhd.append(be32(0));                     // 创建时间
        mvhd.append(be32(0));                     // 修改时间
        mvhd.append(be32(1000));                  // timescale
        mvhd.append(be32(5000));                  // duration
        mvhd.append(QByteArray(84, '\0'));        // 其余字段（速率/音量/矩阵…）

        // 真实文件就是 moov > udta > meta > ilst 这么套的，样本也照套，
        // 免得"只测了扁平结构"，一到真歌上就失灵。
        QByteArray moov;
        moov.append(atom("mvhd", mvhd));
        moov.append(atom("udta", atom("meta", meta)));

        QByteArray ftyp;
        ftyp.append("M4A ");
        ftyp.append(be32(0));
        ftyp.append("M4A mp42isom");

        QByteArray f;
        f.append(atom("ftyp", ftyp));
        f.append(atom("moov", moov));

        writeFile(tmp.filePath(QStringLiteral("测试音轨.m4a")), f);
    }

    // ---- 逐个解析并断言 ----
    const QVector<Sample> samples = {
        { QStringLiteral("01 - 测试歌手 - 测试歌曲.mp3"),
          QStringLiteral("测试歌曲"), QStringLiteral("测试歌手"),
          int(double(MP3_AUDIO_BYTES) * 8000.0 / 128000.0), true, true },
        { QStringLiteral("测试录音.wav"),
          QStringLiteral("测试录音"), QString(),
          int(double(WAV_DATA_BYTES) * 1000.0 / 8000.0), false, false },
        { QStringLiteral("中文 歌名.flac"),
          QStringLiteral("测试FLAC标题"), QStringLiteral("测试艺术家"),
          3000, true, false },
        { QStringLiteral("测试音轨.m4a"),
          QStringLiteral("测试MP4标题"), QStringLiteral("测试MP4歌手"),
          5000, false, false },
    };

    int passed = 0;
    for (const Sample& s : std::as_const(samples))
    {
        const TrackMeta::Meta m = TrackMeta::readMeta(tmp.filePath(s.fileName));

        const bool titleOk  = (m.title == s.title);
        const bool artistOk = s.artist.isEmpty() ? true : (m.artist == s.artist);
        const bool coverOk  = (m.hasCover() == s.cover);
        const bool lyricOk  = ((!m.lyrics.isEmpty()) == s.lyrics);
        // 时长给 5% 的宽容：MP3 那一条本来就是"估算"，不是帧级精确
        const bool durOk    = qAbs(m.durationMs - s.durationMs)
                              <= qMax(50, s.durationMs / 20);

        const bool all = titleOk && artistOk && coverOk && lyricOk && durOk;
        if (all)
            ++passed;

        out += QStringLiteral("  [%1] %2\r\n").arg(all ? QStringLiteral("OK")
                                                       : QStringLiteral("!!"), s.fileName);
        out += QStringLiteral("       标签来源 %1 ｜ 标题「%2」%3 ｜ 艺术家「%4」%5 ｜ "
                              "时长 %6 ms（期望 %7）%8 ｜ 封面 %9 ｜ 歌词 %10\r\n")
                   .arg(m.tagSource.isEmpty() ? QStringLiteral("(无)") : m.tagSource)
                   .arg(m.title)
                   .arg(titleOk ? QString() : QStringLiteral(" ← 不对"))
                   .arg(m.artist.isEmpty() ? QStringLiteral("(空)") : m.artist)
                   .arg(artistOk ? QString() : QStringLiteral(" ← 不对"))
                   .arg(m.durationMs)
                   .arg(s.durationMs)
                   .arg(durOk ? QString() : QStringLiteral(" ← 不对"))
                   .arg(m.hasCover() ? (coverOk ? QStringLiteral("有")
                                                : QStringLiteral("有(不该有)"))
                                     : (coverOk ? QStringLiteral("无")
                                                : QStringLiteral("无(应该有)")))
                   .arg(m.lyricSource.isEmpty()
                            ? (lyricOk ? QStringLiteral("无") : QStringLiteral("无(应该有)"))
                            : QStringLiteral("%1(%2)").arg(lyricOk ? QStringLiteral("有")
                                                                   : QStringLiteral("有(不该有)"),
                                                           m.lyricSource));
    }

    out += QStringLiteral("  小结：%1 / %2 个样本完全对上").arg(passed).arg(samples.size());
    out += (passed == samples.size())
               ? QStringLiteral("  [OK]\r\n\r\n")
               : QStringLiteral("  [!!] 有样本没对上，去查上面标「不对」的那一项\r\n\r\n");

    // ---------------- ③ 真扫一遍（把刚才那个临时目录当成用户的音乐文件夹）----------------
    out += QStringLiteral("--- 扫描与搜索（后台线程递归扫上面那个临时目录）---\r\n");
    {
        MusicLibrary lib;
        QEventLoop loop;
        QObject::connect(&lib, &MusicLibrary::scanFinished, &loop, &QEventLoop::quit);
        lib.scanAsync(tmp.path());
        loop.exec();                                   // 等后台线程发完 scanFinished

        out += lib.describeIndex();

        const auto probe = [&out, &lib](const QString& query, const QString& shown) {
            QElapsedTimer t;
            t.start();
            const QVector<int> hit = lib.search(query);
            const qint64 us = t.nsecsElapsed() / 1000;

            QStringList names;
            for (int i = 0; i < hit.size() && names.size() < 4; ++i)
                if (const Track* tr = lib.trackAt(hit.at(i)))
                    names << tr->title;

            out += QStringLiteral("  搜「%1」→ %2 首 / %3 µs%4\r\n")
                       .arg(shown)
                       .arg(hit.size())
                       .arg(us)
                       .arg(names.isEmpty() ? QString()
                                            : QStringLiteral("  →  ")
                                                  + names.join(QStringLiteral("、")));
        };

        probe(QStringLiteral("测试"), QStringLiteral("测试"));
        probe(QStringLiteral("歌手"), QStringLiteral("歌手"));       // 只在"艺术家"字段里，验证索引把艺术家也拼进去了
        probe(QStringLiteral("FLAC"), QStringLiteral("flac"));       // 验证大小写不敏感
        probe(QStringLiteral("不存在的东西"), QStringLiteral("不存在的东西"));

        const QVector<int> must = lib.search(QStringLiteral("测试"));
        out += (must.isEmpty() && lib.count() > 0)
                   ? QStringLiteral("  [!!] 库里明明有「测试*」的歌却一首都没搜到 —— "
                                    "检查 buildKey / search\r\n")
                   : QStringLiteral("  [OK] 搜索走的是预归一化索引，命中正常\r\n");
    }

    // ---------------- ④ 搜索性能 ----------------
    // 只有 4 首歌看不出快慢，所以造一批内存索引来量。
    // 重点比对"全表扫"和"前缀收窄"的差距 —— 那正是打字时一遍遍触发的那两条路。
    out += QStringLiteral("\r\n--- 搜索性能（2 万条内存索引上的实测，取 20 次平均）---\r\n");
    {
        constexpr int N    = 20000;
        constexpr int REPS = 20;

        static const char* kWords[] = { "周杰伦", "晴天", "七里香", "告白气球", "夜曲", "稻香",
                                        "青花瓷", "简单爱", "以父之名", "星晴",
                                        "洛天依", "霜雪千年", "世末歌者", "达拉崩吧", "权御天下",
                                        "海阔天空", "光辉岁月", "真的爱你", "情人", "喜欢你" };
        const int wordCount = int(sizeof(kWords) / sizeof(kWords[0]));

        QVector<QString> keys;
        keys.reserve(N);
        QRandomGenerator rng(20260923u);                 // 固定种子：每次跑的数字能对比
        for (int i = 0; i < N; ++i)
        {
            const QString raw = QString::fromUtf8(kWords[int(rng.bounded(wordCount))])
                              + QStringLiteral(" - ")
                              + QString::fromUtf8(kWords[int(rng.bounded(wordCount))])
                              + QLatin1Char(' ') + QString::number(i);
            keys.append(MusicLibrary::normalizeForSearch(raw));
        }

        const QString q1 = MusicLibrary::normalizeForSearch(QStringLiteral("晴"));
        const QString q2 = MusicLibrary::normalizeForSearch(QStringLiteral("晴天"));

        QElapsedTimer t;
        int hitFull = 0;
        t.start();
        for (int rep = 0; rep < REPS; ++rep)
        {
            hitFull = 0;
            for (const QString& k : std::as_const(keys))
                if (k.contains(q1))
                    ++hitFull;
        }
        const qint64 usFull = t.nsecsElapsed() / 1000 / REPS;

        // 收窄：只在"上一次的结果"里筛 —— 和 MusicLibrary::search 里那条捷径同一个算法
        QVector<int> prev;
        prev.reserve(hitFull);
        for (int i = 0; i < keys.size(); ++i)
            if (keys.at(i).contains(q1))
                prev.append(i);

        int hitNarrow = 0;
        t.restart();
        for (int rep = 0; rep < REPS; ++rep)
        {
            hitNarrow = 0;
            for (int idx : std::as_const(prev))
                if (keys.at(idx).contains(q2))
                    ++hitNarrow;
        }
        const qint64 usNarrow = t.nsecsElapsed() / 1000 / REPS;

        out += QStringLiteral("  索引规模    : %1 条\r\n").arg(N);
        out += QStringLiteral("  全表扫「晴」: %1 µs/次，命中 %2 条\r\n").arg(usFull).arg(hitFull);
        out += QStringLiteral("  收窄「晴天」: %1 µs/次，命中 %2 条\r\n").arg(usNarrow).arg(hitNarrow);

        if (usNarrow > 0 && usFull > 0)
            out += QStringLiteral("  收窄快 %1 倍 —— 这就是打字时一遍遍触发的那条捷径\r\n")
                       .arg(double(usFull) / double(qMax<qint64>(1, usNarrow)), 0, 'f', 1);

        out += QStringLiteral("  判读：全表扫 %1 µs 对 2 万条来说已经很轻（一次按键几十毫秒预算里"
                              "占不到 1%），所以搜索用的是 QString::contains —— 不引更重的索引结构，"
                              "换来的复杂度不划算。\r\n")
                   .arg(usFull);
    }

    return out;
}

// =============================================================================
//  联网搜索 / 收藏 / 我的收藏
//
//  两段式列表：本地行在前（标"本地"），联网结果追加在后（标"联网·网易 /
//  联网·B站"）。双击联网行 = 下载到临时目录即点即听；右键 = 收藏 / 仅下载
//  （存到用户选的储存盘，永久保留）。
// =============================================================================

// OnlineMusic::Item → 列表联网行的轻量副本（徽标带来源）
static TrackListModel::OnlineTrack toOnlineRow(const OnlineMusic::Item& it)
{
    TrackListModel::OnlineTrack row;
    row.id         = it.id;
    row.source     = it.source;
    row.title      = it.title;
    row.artist     = it.artist;
    row.album      = it.album;
    row.durationMs = it.durationMs;
    row.tag        = (it.source == OnlineMusic::Bilibili)
                         ? QStringLiteral("联网·B站")
                         : QStringLiteral("联网·网易");
    return row;
}

// 下载的 Referer：两个平台都校验（缺了直接 403）
static QMap<QString, QString> dlHeaders(int source)
{
    if (source == OnlineMusic::Bilibili)
        return { { QStringLiteral("Referer"), QStringLiteral("https://www.bilibili.com") } };
    return { { QStringLiteral("Referer"), QStringLiteral("https://music.163.com") } };
}

// 过滤掉"已经在库里的流式行"（同标题的直链条目）—— 同一首歌别显示两行。
// 场景：双击播放后那首歌以"在线"库行存在，联网结果里再出现一次就是重复。
static void dropAlreadyStreaming(QVector<TrackListModel::OnlineTrack>& rows,
                                 const MusicLibrary* lib)
{
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                   [lib](const TrackListModel::OnlineTrack& r) {
                       for (int i = 0; i < lib->count(); ++i)
                       {
                           const Track* t = lib->trackAt(i);
                           if (t && t->path.startsWith(QStringLiteral("http"))
                               && t->title == r.title)
                               return true;   // 这首正在流式播/已在线：别再列一遍
                       }
                       return false;
                   }),
               rows.end());
}

// 网络错误串 → 短中文（Qt 的 errorString 是英文长句，徽标上放不下）
static QString netErrText(const QString& raw)
{
    if (raw.contains(QLatin1String("timeout"), Qt::CaseInsensitive))
        return QStringLiteral("超时");
    if (raw.contains(QLatin1String("refused"), Qt::CaseInsensitive))
        return QStringLiteral("连接被拒");
    if (raw.contains(QLatin1String("host"), Qt::CaseInsensitive)
        || raw.contains(QLatin1String("DNS"), Qt::CaseInsensitive))
        return QStringLiteral("找不到服务器");
    if (raw.contains(QLatin1String("canceled"), Qt::CaseInsensitive)
        || raw.contains(QLatin1String("abort"), Qt::CaseInsensitive))
        return QStringLiteral("已取消");
    if (raw.contains(QLatin1String("SSL"), Qt::CaseInsensitive))
        return QStringLiteral("SSL 错误");
    if (raw.contains(QLatin1String("Temporary network failure"), Qt::CaseInsensitive)
        || raw.contains(QLatin1String("network"), Qt::CaseInsensitive))
        return QStringLiteral("网络不可用");
    return raw;                                // 没认出来的原样显示
}

void PlayerPage::doOnlineSearch(const QString& keyword)
{
    if (keyword.isEmpty())
    {
        m_model->clearOnlineResults();
        return;
    }
    m_onlineKeyword = keyword;         // 词改了之后的过期回调在这里丢弃

    // ---- B 站模式：只搜 B 站（综合排序前 10）----
    if (m_onlineSource == 1)
    {
        OnlineMusic::searchBilibili(keyword,
            [this, keyword](const QVector<OnlineMusic::Item>& items) {
                if (keyword != m_onlineKeyword)
                    return;
                QVector<TrackListModel::OnlineTrack> rows;
                for (const auto& it : items)
                    rows.append(toOnlineRow(it));
        dropAlreadyStreaming(rows, m_lib);
                m_model->setOnlineResults(rows);
            });
        return;
    }

    // ---- 网易云模式（默认）：网易云优先，搜不到自动 B 站接档 ----
    OnlineMusic::searchNetEase(keyword, [this, keyword](const QVector<OnlineMusic::Item>& items) {
        if (keyword != m_onlineKeyword)
            return;
        if (items.isEmpty())
        {
            fallbackBilibili(keyword, m_saveDir, /*registerFav=*/false, /*play=*/false);
            return;                    // 网易没搜到 → B 站搜同关键词
        }
        QVector<TrackListModel::OnlineTrack> rows;
        for (const auto& it : items)
            rows.append(toOnlineRow(it));
        dropAlreadyStreaming(rows, m_lib);
        m_model->setOnlineResults(rows);
    });
}

// 网易搜不到 / 收费时的回退：B 站搜同关键词，结果替换联网区；
// play = true 时（双击的那首收费）自动接档 B 站第一条。
void PlayerPage::fallbackBilibili(const QString& keyword, const QString& dir,
                                  bool registerFav, bool play)
{
    OnlineMusic::searchBilibili(keyword, [this, dir, registerFav, play](const QVector<OnlineMusic::Item>& items) {
        QVector<TrackListModel::OnlineTrack> rows;
        for (const auto& it : items)
            rows.append(toOnlineRow(it));
        dropAlreadyStreaming(rows, m_lib);
        m_model->setOnlineResults(rows);

        if (!rows.isEmpty() && play)
            fetchAndPlay(m_model->localRowCount(), rows.first(), dir, registerFav, play);
    });
}

void PlayerPage::playOnlineRow(int row)
{
    // 即点即听：下载到临时缓存（C 盘），退出时自动清除
    fetchAndPlay(row, m_model->onlineAt(row), OnlineMusic::tempDir(),
                 /*registerFav=*/false, /*play=*/true);
}

// =============================================================================
//  联网行落地：下载 → （B站经 MF 转 WAV）→ 入库 → （可选收藏/播放）
// =============================================================================
void PlayerPage::fetchAndPlay(int row, const TrackListModel::OnlineTrack& it,
                              const QString& dir, bool registerFav, bool play)
{
    // ★ 双击联网行（临时目录 + 播放）= 流式/就地播放，不刷新列表 ★
    // 收藏 / 仅下载（储存盘）= 完整落盘，完成后刷新列表让新歌出现。
    const bool refreshList = !(play && dir == OnlineMusic::tempDir());

    auto setTag = [this, row](const QString& t) {
        if (row >= 0)
            m_model->setOnlineTag(row, t);
    };

    auto startDl = [this, row, it, dir, registerFav, play, refreshList, setTag](const QString& url,
                                                                   const QString& suffix) {
        const QString base = OnlineMusic::sanitizeFileName(
            it.title + QStringLiteral(" - ") + it.artist);
        const QString savePath = dir + QLatin1Char('/') + base + QLatin1Char('.') + suffix;

        OnlineMusic::download(url, savePath, dlHeaders(it.source),
            [this, row](qint64 got, qint64 total) {
                if (total > 0 && row >= 0)
                    m_model->setOnlineTag(row,
                        QStringLiteral("下载中 %1%").arg(int(got * 100 / total)));
            },
            [this, it, row, savePath, dir, suffix, base, registerFav, play, refreshList, setTag]
            (bool ok, const QString& err) {
                if (!ok)
                {
                    setTag(QStringLiteral("联网失败·%1").arg(netErrText(err)));
                    return;
                }

                // B 站：M4S(AAC) → Media Foundation → WAV（miniaudio 只认 MP3/FLAC/WAV）
                if (suffix == QStringLiteral("m4s"))
                {
                    const QString wavPath = dir + QLatin1Char('/') + base + QStringLiteral(".wav");
                    QString decodeErr;
                    if (!MfDecode::decodeToWav(savePath, wavPath, &decodeErr))
                    {
                        setTag(QStringLiteral("该资源无法播放"));
                        QFile::remove(savePath);
                        return;
                    }
                    QFile::remove(savePath);           // 中间产物不留
                    finishOnlineTrack(it, wavPath, dir, registerFav, play, refreshList);
                    if (play && dir == OnlineMusic::tempDir())
                        m_model->setPlayingOnlineId(it.id);   // B站行原地高亮（列表不刷）
                    return;
                }

                // 网易：歌词顺路写成 .lrc 侧车（TrackMeta 读侧车，播放器零改动）
                if (it.source == OnlineMusic::NetEase)
                {
                    OnlineMusic::netEaseLyric(it.id,
                        [this, it, savePath, dir, registerFav, play, refreshList, setTag](const QString& lrc) {
                            if (!lrc.isEmpty())
                            {
                                QFile f(QFileInfo(savePath).absolutePath() + QLatin1Char('/')
                                        + QFileInfo(savePath).completeBaseName()
                                        + QStringLiteral(".lrc"));
                                if (f.open(QIODevice::WriteOnly))
                                {
                                    f.write(lrc.toUtf8());
                                    f.close();
                                }
                            }
                            finishOnlineTrack(it, savePath, dir, registerFav, play, refreshList);
                        });
                    return;
                }

                finishOnlineTrack(it, savePath, dir, registerFav, play, refreshList);
            });
    };

    if (it.source == OnlineMusic::NetEase)
    {
        OnlineMusic::netEaseUrl(it.id,
            [this, row, it, play, startDl](const QString& url, const QString& suffix) {
                if (url.isEmpty())
                {
                    // VIP/无版权 → B 站搜同名自动接档
                    fallbackBilibili(it.title + QStringLiteral(" ") + it.artist,
                                     m_saveDir, /*registerFav=*/false, /*play=*/false);
                    return;
                }

                // ★ 网易云：流式播放 —— 不落盘、不进曲库、列表原样 ★
                // 高亮联网行（没有库行，就不会出现"同歌两行"）；
                // 歌词异步拉 LRC 直接重建（流式没有本地文件可读侧车）。
                if (play)
                {
                    m_model->setPlayingLibraryIndex(-1);   // 本地行高亮让位
                    m_model->setPlayingOnlineId(it.id);    // 联网行点亮
                    m_nowLabel->setText(it.title + QStringLiteral(" · ") + it.artist);

                    QString err;
                    if (!m_player->loadOnline(QUrl(url), it.durationMs,
                                              dlHeaders(OnlineMusic::NetEase), &err))
                    {
                        m_nowLabel->setText(QStringLiteral("%1：%2").arg(it.title, err));
                        return;
                    }
                    m_player->play();
                    emit currentTrackChanged(it.title);

                    OnlineMusic::netEaseLyric(it.id,
                        [this, id = it.id](const QString& lrc) {
                            if (lrc.isEmpty() || m_model->playingOnlineId() != id)
                                return;          // 已经切歌：过期回调丢弃
                            rebuildLyricLabels(TrackMeta::parseLrc(lrc));
                        });
                    return;
                }

                // 收藏 / 仅下载：完整下载到储存盘（进"我的下载"）
                startDl(url, suffix);
            });
    }
    else
    {
        OnlineMusic::bilibiliAudio(it.id,
            [this, row, setTag, it, registerFav, play](const QString& url, const QString& suffix) {
                if (url.isEmpty())
                {
                    setTag(QStringLiteral("该资源无法播放"));
                    return;
                }

                // ★ B 站必须下载（AAC）→ 转好放进「我的下载」，下次直接读本地 ★
                if (!ensureSaveDir())
                {
                    setTag(QStringLiteral("未选择保存文件夹"));
                    return;
                }

                const QString base    = OnlineMusic::sanitizeFileName(
                    it.title + QStringLiteral(" - ") + it.artist);
                const QString wavPath = m_saveDir + QLatin1Char('/') + base + QStringLiteral(".wav");

                // 下次再点同一首：本地已有转好的 WAV → 直接读它，不再下载
                if (QFileInfo::exists(wavPath))
                {
                    playLocalWav(it, wavPath, registerFav, play);
                    return;
                }

                const QString m4aPath = m_saveDir + QLatin1Char('/') + base + QStringLiteral(".m4a");
                OnlineMusic::download(url, m4aPath, dlHeaders(OnlineMusic::Bilibili),
                    [this, row](qint64 got, qint64 total) {
                        if (total > 0 && row >= 0)
                            m_model->setOnlineTag(row,
                                QStringLiteral("下载中 %1%").arg(int(got * 100 / total)));
                    },
                    [this, it, row, m4aPath, wavPath, registerFav, play, setTag](bool ok, const QString& err) {
                        if (!ok)
                        {
                            setTag(QStringLiteral("联网失败·%1").arg(netErrText(err)));
                            return;
                        }
                        QString decodeErr;
                        if (!MfDecode::decodeToWav(m4aPath, wavPath, &decodeErr))
                        {
                            setTag(QStringLiteral("该资源无法播放"));
                            QFile::remove(m4aPath);
                            return;
                        }
                        QFile::remove(m4aPath);            // 中间产物不留
                        playLocalWav(it, wavPath, registerFav, play);
                    });
            });
    }
}

// =============================================================================
//  B 站落地后的播放：WAV 已在储存盘 → 入库（不广播）→ 播放 + 联网行高亮
//
//  ★ 下次再点同一首 B 站结果：wavPath 已存在 → 直接走这里读本地文件 ★
//  不再下载、不再转码。
// =============================================================================
void PlayerPage::playLocalWav(const TrackListModel::OnlineTrack& it, const QString& wavPath,
                              bool registerFav, bool play)
{
    int libIndex = m_lib->indexOfPath(wavPath);
    if (libIndex < 0)
    {
        Track t;
        t.path       = wavPath;
        t.title      = it.title;
        t.artist     = it.artist;
        t.durationMs = it.durationMs;
        libIndex = m_lib->count();
        m_lib->appendBatch(QVector<Track>{ t }, false);   // 不广播：列表保持 B 站结果
    }
    m_model->setLibraryTag(libIndex,
        registerFav ? QStringLiteral("收藏") : QStringLiteral("已下载"));
    if (registerFav)
        addFavorite(it.title, it.artist, wavPath);
    addDownloaded(it.title, it.artist, wavPath);

    if (play)
    {
        playLibraryIndex(libIndex);            // 本地 WAV 直接 load + play
        m_model->setPlayingOnlineId(it.id);    // 联网行高亮补回（playLibraryIndex 清过）
    }
}

void PlayerPage::finishOnlineTrack(const TrackListModel::OnlineTrack& it, const QString& path,
                                   const QString& dir, bool registerFav, bool play,
                                   bool refreshList)
{
    // ★ 先找"同一首歌"的现有库行 ★
    //   ① 路径相同（重复下载/重复双击）；
    //   ② 直链行且标题相同（流式播放中的那行 —— 下载完成后**就地转正**：
    //      直链换成本地文件，不再另起一行，列表不会多出重复的一首）。
    int libIndex = m_lib->indexOfPath(path);
    if (libIndex < 0)
    {
        for (int i = 0; i < m_lib->count(); ++i)
        {
            const Track* t = m_lib->trackAt(i);
            if (t && t->path.startsWith(QStringLiteral("http")) && t->title == it.title)
            {
                libIndex = i;
                break;
            }
        }
    }

    if (libIndex < 0)
    {
        Track t;
        t.path       = path;
        t.title      = it.title;
        t.artist     = it.artist;
        t.durationMs = it.durationMs;
        libIndex = m_lib->count();
        m_lib->appendBatch(QVector<Track>{ t });
    }
    else
    {
        m_lib->updateTrackPath(libIndex, path);   // 流式直链 → 本地文件（就地转正）
    }

    // 徽标：收藏的标"收藏"，仅下载的标"已下载"，临时播放的标"在线"
    const bool savedToDisk = (!m_saveDir.isEmpty() && dir == m_saveDir);
    m_model->setLibraryTag(libIndex,
        registerFav ? QStringLiteral("收藏")
                    : (savedToDisk  ? QStringLiteral("已下载")
                                    : QStringLiteral("在线")));

    if (registerFav)
        addFavorite(it.title, it.artist, path);
    if (savedToDisk)
        addDownloaded(it.title, it.artist, path);   // 收藏/仅下载都登记进"我的下载"

    // ★ 刷新列表只发生在"落盘"的场合 ★ 流式/临时播放不刷 —— 联网结果保持
    // 原样，正在播的那行高亮（setPlayingOnlineId 由调用方补一下）。
    if (refreshList)
    {
        m_model->clearOnlineResults();     // 联网区的那行使命完成（歌已是本地行）
        doSearch();
    }

    if (play)
        playLibraryIndex(libIndex);
}

// =============================================================================
//  收藏 / 我的收藏
// =============================================================================
void PlayerPage::addFavorite(const QString& title, const QString& artist, const QString& path)
{
    QSettings ini(QSettings::IniFormat, QSettings::UserScope,
                  QStringLiteral("PetPal"), QStringLiteral("favorites"));
    const int n = ini.beginReadArray(QStringLiteral("favorites"));
    for (int i = 0; i < n; ++i)
    {
        ini.setArrayIndex(i);
        if (ini.value(QStringLiteral("path")).toString() == path)
        {
            ini.endArray();
            return;                    // 已经收藏过：不重复登记
        }
    }
    ini.endArray();

    ini.beginWriteArray(QStringLiteral("favorites"), n + 1);
    ini.setArrayIndex(n);
    ini.setValue(QStringLiteral("title"), title);
    ini.setValue(QStringLiteral("artist"), artist);
    ini.setValue(QStringLiteral("path"), path);
    ini.endArray();
}

void PlayerPage::showFavorites(bool on)
{
    m_favView = on;
    if (m_favBtn->isChecked() != on)
    {
        m_favBtn->blockSignals(true);
        m_favBtn->setChecked(on);
        m_favBtn->blockSignals(false);
    }

    // 收藏视图打开时收起下载视图（两个视图互斥，按钮状态也跟着走）
    if (on && m_dlView)
    {
        m_dlView = false;
        m_dlBtn->blockSignals(true);
        m_dlBtn->setChecked(false);
        m_dlBtn->blockSignals(false);
    }

    if (!on)
    {
        doSearch();
        return;
    }

    // 读收藏索引：文件还在的进列表，丢了的自动剔除（循环后整组重写索引）
    QSettings ini(QSettings::IniFormat, QSettings::UserScope,
                  QStringLiteral("PetPal"), QStringLiteral("favorites"));
    const int n = ini.beginReadArray(QStringLiteral("favorites"));

    struct FavEntry
    {
        QString title, artist, path;
    };
    QVector<FavEntry> kept;
    QVector<Track>    inject;
    QVector<int>      view;
    const int         base = m_lib->count();

    for (int i = 0; i < n; ++i)
    {
        ini.setArrayIndex(i);
        const QString path = ini.value(QStringLiteral("path")).toString();
        if (!QFileInfo::exists(path))
            continue;                  // 文件没了：剔除

        int libIndex = m_lib->indexOfPath(path);
        if (libIndex < 0)
        {
            // 文件在、但还没进过曲库：现读元数据补进来
            const TrackMeta::Meta meta = TrackMeta::readMeta(path);
            Track t;
            t.path       = path;
            t.title      = meta.title.isEmpty() ? QFileInfo(path).completeBaseName()
                                                : meta.title;
            t.artist     = meta.artist;
            t.durationMs = meta.durationMs;
            inject.append(t);
            libIndex = base + inject.size() - 1;
        }
        view.append(libIndex);
        kept.append({ ini.value(QStringLiteral("title")).toString(),
                      ini.value(QStringLiteral("artist")).toString(), path });
    }
    ini.endArray();

    if (!inject.isEmpty())
        m_lib->appendBatch(inject);

    ini.beginWriteArray(QStringLiteral("favorites"), kept.size());
    for (int i = 0; i < kept.size(); ++i)
    {
        ini.setArrayIndex(i);
        ini.setValue(QStringLiteral("title"), kept.at(i).title);
        ini.setValue(QStringLiteral("artist"), kept.at(i).artist);
        ini.setValue(QStringLiteral("path"), kept.at(i).path);
    }
    ini.endArray();

    m_model->clearOnlineResults();
    m_model->setView(view);
    m_countLabel->setText(QStringLiteral("收藏 %1 首").arg(view.size()));
}

void PlayerPage::onListMenu(const QPoint& pos)
{
    const QModelIndex idx = m_list->indexAt(pos);
    if (!idx.isValid())
        return;
    const int row = idx.row();

    QMenu menu(this);
    if (m_model->isOnlineRow(row))
    {
        menu.addAction(QStringLiteral("收藏"), this, [this, row] {
            if (!ensureSaveDir())
                return;                // 没选储存盘（或取消了）：本次不动
            fetchAndPlay(row, m_model->onlineAt(row), m_saveDir,
                         /*registerFav=*/true, /*play=*/false);
        });
        menu.addAction(QStringLiteral("仅下载"), this, [this, row] {
            if (!ensureSaveDir())
                return;
            fetchAndPlay(row, m_model->onlineAt(row), m_saveDir,
                         /*registerFav=*/false, /*play=*/false);
        });
    }
    else if (m_dlView)
    {
        // 我的下载视图：右键删除（删文件 + 剔索引 + 出曲库）
        const int libIndex = idx.data(TrackListModel::LibraryIndexRole).toInt();
        menu.addAction(QStringLiteral("删除"), this, [this, libIndex] {
            removeDownloaded(libIndex);
        });
    }
    else
    {
        menu.addAction(QStringLiteral("收藏"), this, [this, idx] {
            const QString path = idx.data(TrackListModel::PathRole).toString();
            addFavorite(idx.data(TrackListModel::TitleRole).toString(),
                        idx.data(TrackListModel::ArtistRole).toString(), path);
        });
    }
    menu.exec(m_list->viewport()->mapToGlobal(pos));
}

bool PlayerPage::ensureSaveDir()
{
    if (!m_saveDir.isEmpty())
        return true;

    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择音乐保存文件夹（收藏和下载都存这里）"), QString());
    if (dir.isEmpty())
        return false;                  // 用户取消：本次不动

    m_saveDir = dir;
    QSettings(QSettings::IniFormat, QSettings::UserScope,
              QStringLiteral("PetPal"), QStringLiteral("ui"))
        .setValue(QStringLiteral("player/saveDir"), dir);
    return true;
}

// =============================================================================
//  我的下载（仅下载 / 收藏落盘的歌都在这里；列表里右键可删除）
// =============================================================================
void PlayerPage::addDownloaded(const QString& title, const QString& artist, const QString& path)
{
    QSettings ini(QSettings::IniFormat, QSettings::UserScope,
                  QStringLiteral("PetPal"), QStringLiteral("downloads"));
    const int n = ini.beginReadArray(QStringLiteral("downloads"));
    for (int i = 0; i < n; ++i)
    {
        ini.setArrayIndex(i);
        if (ini.value(QStringLiteral("path")).toString() == path)
        {
            ini.endArray();
            return;                    // 已经登记过：不重复
        }
    }
    ini.endArray();

    ini.beginWriteArray(QStringLiteral("downloads"), n + 1);
    ini.setArrayIndex(n);
    ini.setValue(QStringLiteral("title"), title);
    ini.setValue(QStringLiteral("artist"), artist);
    ini.setValue(QStringLiteral("path"), path);
    ini.endArray();
}

void PlayerPage::showDownloads(bool on)
{
    m_dlView = on;
    if (m_dlBtn->isChecked() != on)
    {
        m_dlBtn->blockSignals(true);
        m_dlBtn->setChecked(on);
        m_dlBtn->blockSignals(false);
    }

    // 下载视图打开时收起收藏视图（两个视图互斥）
    if (on && m_favView)
    {
        m_favView = false;
        m_favBtn->blockSignals(true);
        m_favBtn->setChecked(false);
        m_favBtn->blockSignals(false);
    }

    if (!on)
    {
        doSearch();
        return;
    }

    // 读下载索引：文件还在的进列表，丢了的自动剔除（循环后整组重写索引）
    QSettings ini(QSettings::IniFormat, QSettings::UserScope,
                  QStringLiteral("PetPal"), QStringLiteral("downloads"));
    const int n = ini.beginReadArray(QStringLiteral("downloads"));

    struct DlEntry
    {
        QString title, artist, path;
    };
    QVector<DlEntry> kept;
    QVector<Track>   inject;
    QVector<int>     view;
    const int        base = m_lib->count();

    for (int i = 0; i < n; ++i)
    {
        ini.setArrayIndex(i);
        const QString path = ini.value(QStringLiteral("path")).toString();
        if (!QFileInfo::exists(path))
            continue;                  // 文件没了：剔除

        int libIndex = m_lib->indexOfPath(path);
        if (libIndex < 0)
        {
            const TrackMeta::Meta meta = TrackMeta::readMeta(path);
            Track t;
            t.path       = path;
            t.title      = meta.title.isEmpty() ? QFileInfo(path).completeBaseName()
                                                : meta.title;
            t.artist     = meta.artist;
            t.durationMs = meta.durationMs;
            inject.append(t);
            libIndex = base + inject.size() - 1;
        }
        view.append(libIndex);
        kept.append({ ini.value(QStringLiteral("title")).toString(),
                      ini.value(QStringLiteral("artist")).toString(), path });
    }
    ini.endArray();

    if (!inject.isEmpty())
        m_lib->appendBatch(inject);

    ini.beginWriteArray(QStringLiteral("downloads"), kept.size());
    for (int i = 0; i < kept.size(); ++i)
    {
        ini.setArrayIndex(i);
        ini.setValue(QStringLiteral("title"), kept.at(i).title);
        ini.setValue(QStringLiteral("artist"), kept.at(i).artist);
        ini.setValue(QStringLiteral("path"), kept.at(i).path);
    }
    ini.endArray();

    m_model->clearOnlineResults();
    m_model->setView(view);
    m_countLabel->setText(QStringLiteral("下载 %1 首").arg(view.size()));
}

// 从索引文件里剔除某条路径（favorites / downloads 通用：数组名 = 文件名）
static void pruneIndexFile(const QString& appName, const QString& path)
{
    QSettings ini(QSettings::IniFormat, QSettings::UserScope,
                  QStringLiteral("PetPal"), appName);
    const int n = ini.beginReadArray(appName);

    struct Entry
    {
        QString title, artist, path;
    };
    QVector<Entry> kept;
    for (int i = 0; i < n; ++i)
    {
        ini.setArrayIndex(i);
        const QString p = ini.value(QStringLiteral("path")).toString();
        if (p == path)
            continue;
        kept.append({ ini.value(QStringLiteral("title")).toString(),
                      ini.value(QStringLiteral("artist")).toString(), p });
    }
    ini.endArray();

    ini.beginWriteArray(appName, kept.size());
    for (int i = 0; i < kept.size(); ++i)
    {
        ini.setArrayIndex(i);
        ini.setValue(QStringLiteral("title"), kept.at(i).title);
        ini.setValue(QStringLiteral("artist"), kept.at(i).artist);
        ini.setValue(QStringLiteral("path"), kept.at(i).path);
    }
    ini.endArray();
}

void PlayerPage::removeDownloaded(int libIndex)
{
    const Track* t = m_lib->trackAt(libIndex);
    if (!t)
        return;
    const QString path = t->path;

    // 正在播这首 → 先停（文件都要删了，继续播没有意义）
    if (m_playingLib == libIndex)
    {
        m_player->stop();
        m_playingLib = -1;
        m_model->setPlayingLibraryIndex(-1);
        m_nowLabel->setText(QStringLiteral("没在放歌"));
    }

    // 删文件（.lrc 侧车一并）
    QFile::remove(path);
    QFile::remove(QFileInfo(path).absolutePath() + QLatin1Char('/')
                  + QFileInfo(path).completeBaseName() + QStringLiteral(".lrc"));

    // 剔索引（下载 + 收藏里同路径的条目）
    pruneIndexFile(QStringLiteral("downloads"), path);
    pruneIndexFile(QStringLiteral("favorites"), path);

    // 出曲库（后面的下标整体前移，正在播的下标也要修）
    m_lib->removeAt(libIndex);
    if (m_playingLib > libIndex)
    {
        m_playingLib -= 1;
        m_model->setPlayingLibraryIndex(m_playingLib);
    }

    // 重新载入下载视图（列表重建）
    showDownloads(true);
}
