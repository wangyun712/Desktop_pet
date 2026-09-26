#pragma once
// =============================================================================
//  MainPanel —— 主面板（左侧功能导航 + 右侧内容区）
//
//  它不是"好感度面板"，而是"装各种面板的壳"。
//  以后要加新功能（装扮、设置、日记……），只需要：
//      写一个 QWidget 当页面  ->  panel->addPage("标题", page)
//  左侧导航会自动多出一项，右侧自动多出一页，不用动这里的布局代码。
//
//  ★ 为什么做成无边框自绘窗口 ★
//    桌宠本身是无边框透明窗口，面板如果顶着一条系统标题栏会很出戏。
//    代价是拖动、圆角、关闭按钮都得自己来，都写在下面这几个事件里。
// =============================================================================

#include <QWidget>
#include <QPoint>
#include <QRect>

class QListWidget;
class QStackedWidget;
class QPushButton;

class MainPanel : public QWidget
{
    Q_OBJECT
public:
    explicit MainPanel(QWidget* parent = nullptr);

    // 加一个功能页。isEnabled=false 的页面只是占位（灰色、点了显示"待接入"）
    void addPage(const QString& title, QWidget* page);
    int  pageCount() const;

    // 直接翻到第 index 页（左侧导航会跟着选中）。
    // 给 --selftest 用：它要把每一页都离屏渲染一张，得能一页页翻过去。
    void setCurrentPage(int index);

    // 某个页面排在第几页。加页顺序一变，硬编码的下标就会指错页，
    // 所以"跳到聊天页"这种需求一律走这个函数，不写数字。
    int  indexOfPage(QWidget* page) const;

    // 在指定屏幕可用区里居中显示。传进来而不是自己算，
    // 是为了让面板跟着"桌宠在哪块屏幕"走 —— 多显示器时这一点很重要。
    void showCenteredIn(const QRect& screenRect);

    // 放大/还原切换（顶部条那个 □ 按钮）。铺满当前屏幕的可用区，
    // 不盖任务栏 —— 面板不是播放器，压住任务栏只会让人没法切窗口。
    void toggleMaximized();
    bool isMaximizedPanel() const { return m_maximized; }

    // 界面字号档位变了之后重新套一遍样式表（外壳自己的那部分）。
    // 页面各自的字号由各自的 applyUiScale() 负责，这里只管导航和窗控按钮。
    void applyUiScale();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    static constexpr int TOP_BAR_H = 38;    // 顶部条高度：既是标题栏，也是拖动把手

    void updateMaxButtonIcon();             // □ / ❐ 跟着最大化状态切
    void applyStyle();                      // 外壳的样式表（构造时和改字号时都要套）

    QWidget*        m_topBar   = nullptr;
    QListWidget*    m_nav      = nullptr;
    QStackedWidget* m_stack    = nullptr;
    QPushButton*    m_minBtn   = nullptr;   // 最小化
    QPushButton*    m_maxBtn   = nullptr;   // 放大 / 还原
    QPushButton*    m_closeBtn = nullptr;   // 关闭

    QPoint m_dragOffset;                    // 拖动时鼠标相对窗口左上角的偏移
    bool   m_dragging = false;

    // 放大/还原用。放大前的几何要留着，还原时原样放回去。
    bool  m_maximized = false;
    QRect m_restoreGeometry;
};
