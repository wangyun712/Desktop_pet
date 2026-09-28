#pragma once
// =============================================================================
//  DesktopLyrics —— 桌面歌词悬浮窗（学网易云的桌面歌词）
//
//  机制（对着网易云桌面歌词来的）：
//    · 一个**置顶、无边框、透明背景**的独立小窗，浮在桌面所有窗口之上；
//    · ★ 宽度跟着歌词走 ★ —— 按当前句/下一句的文字宽度重算，窗口中心轴钉死
//      不动，只向两侧对称伸缩；没有歌词时收窄成一小条，长句时撑宽。
//      为把"改尺寸"的代价压到最小：
//        - 宽度向上量化到 16px 一档 —— 一档之内换行不重配窗口；
//        - 量化在拖动中 / 鼠标悬停（未锁）时**冻结** —— 挪+缩同时进行会跟手
//          打架，悬停中改宽可能把鼠标甩出窗外，等松手/移开再补一次；
//        - 超长行照样"…"截断（网易云同款），窗口不无限撑宽。
//    · 两行：当前句（大字、主题强调色）+ 下一句（小字、浅灰）—— 网易云的
//      经典布局，"正在唱什么、接下来是什么"一眼都有；
//    · 文字带一圈深色描边 —— 桌面歌词要压得住任何壁纸全靠这一圈；
//    · 按住可拖到屏幕任意位置，位置记进 ui.ini，下次启动回到原地；
//    · ★ 显隐原则 ★ 开关关了 / 没加载任何歌 → 整窗隐藏；有歌但这首没有
//      歌词 → 窗口**不**消失，显示"暂无歌词"占位（PlayerPage 喂的）——
//      悬浮窗一会儿有一会儿没有，看着像坏了；
//    · ★ 出现和更新都绝不抢焦点 ★（WA_ShowWithoutActivating）。
//
//  ★ 它只做显示 + 播放控制，不管歌词数据 ★
//    文本由 PlayerPage 喂（setLine，持有歌词和进度的是它）；这一层没有
//    时间戳，所以也不做"双击跳转"——进度的事都回面板里做。
//
//  工具条（悬浮在歌词条上方，鼠标移到窗上才出现，移开就收）：
//    [上一首] [播放/暂停] [下一首] [锁定] [关闭]
//    · 锁定后：工具条收起、拖拽和按钮全部失效 —— 只剩两条歌词条；
//      ★ 双击歌词条解锁，三连击唤出主面板 ★（锁定也要能被解开，
//      所以锁定不是输入穿透，只是不响应任何其它交互）；
//    · 锁定状态记进 ui.ini，下次启动保持；
//    · ★ 关闭（×）= 把桌面歌词整个关掉 ★（ui.ini 存成关，设置页的勾选框
//      会同步取消）；要再开，回设置页重新勾选。
// =============================================================================

#include <QWidget>
#include <QString>
#include <QPoint>
#include <QRect>
#include <QElapsedTimer>

class DesktopLyrics : public QWidget
{
    Q_OBJECT
public:
    explicit DesktopLyrics(QWidget* parent = nullptr);

    // 喂歌词。current 为空 = 没东西可显示（开关关了/没歌）→ 整窗隐藏；
    // 喂"暂无歌词"这类占位文本时窗口照常显示。
    void setLine(const QString& current, const QString& next);

    // 播放键图标状态（播放中显示暂停图标，否则显示播放图标）。
    void setPlaying(bool playing);

    // 开关的读写（设置页写、播放器页读，共用这一个键）。
    static bool loadEnabled();
    static void saveEnabled(bool on);

signals:
    void playPauseRequested();   // 工具条上的播放/暂停
    void prevRequested();        // 上一首
    void nextRequested();        // 下一首
    void panelRequested();       // 三连击歌词条 → 唤出主面板
    void closeRequested();       // 工具条上的 ×：把桌面歌词整个关掉（设置页重开）

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    enum Btn { BtnPrev, BtnPlay, BtnNext, BtnLock, BtnClose, BtnCount };

    void buildDefaultSize();   // 出场默认宽度 + 高度（启动 / 字号档位变化时各一次）
    void updateWidth();        // 按当前两行文字重算宽度（中心轴钉死，见 .h 顶部说明）
    void restorePosition();    // 启动时回到上次的位置（并夹回屏幕内）
    void savePosition();
    void countClick();         // 连击计数：双击解锁 / 三连击唤面板

    QRect  pillRect() const;   // 工具条底板
    QRect  btnRect(int which) const;
    bool   toolbarVisible() const;
    int    btnAt(const QPoint& pos) const;    // 点在哪个按钮上（-1 = 都不是）
    void   paintToolbar(QPainter& p);
    void   drawGlyph(QPainter& p, int which, const QRect& r, const QColor& color) const;

    QString m_cur;             // 当前行（空 = 隐藏）
    QString m_next;            // 下一行（可能为空：唱到最后一句了）
    bool    m_dragging = false;
    QPoint  m_dragOffset;

    bool    m_locked   = false;   // 锁定：无工具条、无拖拽，双击解锁
    bool    m_playing  = false;   // 播放键显示哪种图标
    bool    m_hovered  = false;   // 鼠标在窗上 → 显示工具条
    int     m_hoverBtn = -1;      // 悬停着哪个按钮（-1 = 无）

    int     m_clickCount = 0;     // 连击计数（600ms 窗口内有效）
    QElapsedTimer m_clickClock;   // 上一次点击的时间（超时就从头数）

    int     m_builtScale = 0;     // 固定尺寸是在哪个字号档位下定的（档位变了才重建）
};
