#pragma once
// =============================================================================
//  AffectionPage —— 主面板里的「好感度」页（右侧内容）
//
//  它的职责被刻意压到最小：
//    · 把 AffectionSystem 里的数字画出来
//    · 三个按钮点了之后，只发信号，不自己加分、不自己放动画
//
//  为什么不自己加分？因为"摸摸"这个动作有两件事要同时发生：
//    ① 好感度 +1   ② 桌宠播一个开心的表情
//  加分归 AffectionSystem，动画归 DesktopPet。页面插在中间只会把两边都搞乱，
//  所以它只管喊一声"用户摸了一下"，剩下由 DesktopPet 决定怎么响应。
// =============================================================================

#include <QWidget>

class QLabel;
class QProgressBar;
class QPushButton;
class AffectionSystem;

class AffectionPage : public QWidget
{
    Q_OBJECT
public:
    explicit AffectionPage(AffectionSystem* sys, QWidget* parent = nullptr);

signals:
    void petPetted();        // 「摸摸」
    void feedRequested();    // 「喂食」
    void chatRequested();    // 「聊聊天」

public slots:
    void refresh();          // 好感度有任何变化时重画

private slots:
    void onLeveledUp(int level);

private:
    QWidget* buildMetric(const QString& caption, QLabel** valueOut);
    void     applyStyle();

    AffectionSystem* m_sys = nullptr;

    QLabel*       m_avatar       = nullptr;
    QLabel*       m_levelText    = nullptr;
    QLabel*       m_progressText = nullptr;
    QProgressBar* m_bar          = nullptr;

    QLabel* m_todayValue = nullptr;
    QLabel* m_daysValue  = nullptr;
    QLabel* m_totalValue = nullptr;

    QPushButton* m_btnPet  = nullptr;
    QPushButton* m_btnFeed = nullptr;
    QPushButton* m_btnChat = nullptr;

    QLabel* m_petQuota  = nullptr;
    QLabel* m_feedQuota = nullptr;
    QLabel* m_hint      = nullptr;
};
