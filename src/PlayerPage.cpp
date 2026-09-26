#include "PlayerPage.h"

#include "AudioPlayer.h"
#include "TrackListModel.h"
#include "UiFont.h"

#include <QBuffer>
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
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
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
            return QColor(0xC9, 0xC7, 0xBF);
        if (isDown())
            return isMainKey() ? QColor(0xFF, 0xFF, 0xFF) : QColor(0x26, 0x21, 0x5C);
        if (underMouse())
            return isMainKey() ? QColor(0xFF, 0xFF, 0xFF) : QColor(0x26, 0x21, 0x5C);
        return isMainKey() ? QColor(0xFF, 0xFF, 0xFF) : QColor(0x5F, 0x5E, 0x5A);
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
            p->fillRect(r, QColor(0xF1, 0xEF, 0xE8));

        if (playing)
            p->fillRect(QRect(r.left() + 3, r.top() + 9, 3, r.height() - 18), QColor(0x7F, 0x77, 0xDD));

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

        p->setFont(smallFont);
        p->setPen(QColor(0x9A, 0x98, 0x90));
        p->drawText(QRect(r.right() - durWidth, r.top(), durWidth, r.height()),
                    Qt::AlignRight | Qt::AlignVCenter, dur);

        // ---- 标题 / 艺术家 ----
        const int avail = qMax(20, r.right() - durWidth - textLeft - 6);

        const QFontMetrics fm(opt.font);
        const int twoLine = fm.height() + smallFm.height();
        const int top = r.top() + qMax(0, (r.height() - twoLine) / 2);

        p->setFont(opt.font);
        p->setPen(playing ? QColor(0x53, 0x4A, 0xC0) : QColor(0x2C, 0x2C, 0x2A));
        const QString title = idx.data(TrackListModel::TitleRole).toString();
        p->drawText(QRect(textLeft, top, avail, fm.height()),
                    Qt::AlignLeft | Qt::AlignVCenter,
                    fm.elidedText(title, Qt::ElideRight, avail));

        QString artist = idx.data(TrackListModel::ArtistRole).toString();
        if (artist.isEmpty())
            artist = QStringLiteral("未知艺术家");

        p->setFont(smallFont);
        p->setPen(QColor(0x8A, 0x88, 0x80));
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

// 歌词高亮用的强调色（和样式表里那个 #7F77DD 是一套，深一档好压住灰底）
const QColor kAccentDeep(0x53, 0x4A, 0xC0);

// 歌词行距（100% 档位下的基准值）。实际用 UiFont::px() 按"界面字号"换算 ——
// 字放大行距不跟着走的话，几行大字会挤成一坨。
constexpr int kLyricSpacingBase = 10;

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

    // 搜索防抖：停手 250ms 才真去搜
    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(250);
    connect(m_searchTimer, &QTimer::timeout, this, &PlayerPage::doSearch);

    // 初始视图：空查询 = 全部
    m_model->setView(m_lib->search(QString()));
    updateCountLabel();      // 顺带把"上一首 / 下一首"的可按状态刷成最新
}

PlayerPage::~PlayerPage() = default;

// =============================================================================
//  搭界面
// =============================================================================
void PlayerPage::buildUi()
{
    // ---------------- 工具行 ----------------
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
    // 只替换 %1（歌词强调色），色值取自 kAccentDeep，不在这里再抄一遍
    const QString qss = QStringLiteral(R"(
        QPushButton#playerPick {
            border: none; border-radius: 6px; padding: 6px 14px;
            background: #7F77DD; color: #FFFFFF; font-size: 12px;
        }
        QPushButton#playerPick:hover   { background: #6E65D6; }
        QPushButton#playerPick:pressed { background: #5F57C8; }

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
    )").arg(kAccentDeep.name());

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
        // ★ 这里刻意用**最近邻**（FastTransformation），不是平滑 ★
        //   这张图只以 13% 的不透明度垫在文字后面（见下面 setOpacity），
        //   平滑与否肉眼完全看不出来；但它是在 resizeEvent 里算的 —— 拖窗口边缘时
        //   resizeEvent 会连着来上百次，每次都做平滑重缩就是"拉窗口一顿一顿"的来源。
        //   宁可糙一点也要跟手，反正最后看到的是那层淡到几乎看不见的底。
        m_bgCache = m_bgCover.scaled(size(), Qt::KeepAspectRatioByExpanding,
                                     Qt::FastTransformation);
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
    m_searchTimer->start();            // 防抖：连打十个字只搜一次
}

void PlayerPage::doSearch()
{
    m_model->setView(m_lib->search(m_searchEdit->text()));
    updateCountLabel();
}

// =============================================================================
//  播放
// =============================================================================
void PlayerPage::onRowActivated(const QModelIndex& index)
{
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
    if (!m_player->load(t->path, &err))
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

    // 封面：先缩到 640 以内再留着当背景，免得每次重绘都在缩一张两三千像素的原图
    if (meta.hasCover())
    {
        QImage img;
        img.loadFromData(meta.cover);
        if (!img.isNull())
        {
            if (img.width() > 640 || img.height() > 640)
                img = img.scaled(640, 640, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            m_bgCover = QPixmap::fromImage(img);
            m_coverLabel->setPixmap(m_bgCover.scaled(m_coverLabel->size(),
                                                     Qt::KeepAspectRatioByExpanding,
                                                     Qt::SmoothTransformation));
            m_bgCacheSize = QSize();       // 作废铺满缓存
        }
        else
        {
            m_bgCover = QPixmap();
            m_coverLabel->setText(QStringLiteral("\u266B"));
        }
    }
    else
    {
        m_bgCover = QPixmap();
        m_coverLabel->setText(QStringLiteral("\u266B"));   // ♫
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
    }

    m_lyricCurrent = -1;
    m_lyricHeader->setText(QStringLiteral("歌词 · %1 行").arg(lines.size()));
}

void PlayerPage::clearLyrics(const QString& message)
{
    m_lyricLines.clear();
    m_lyricCurrent = -1;

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

    // 滚到可视区中间 —— ensureWidgetVisible 的 yMargin 给一半高度就够了
    m_lyricScroll->ensureWidgetVisible(cur, 0, m_lyricScroll->viewport()->height() / 2);
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
