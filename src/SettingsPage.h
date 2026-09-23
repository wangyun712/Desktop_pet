#pragma once
// =============================================================================
//  SettingsPage —— 主面板里的「设置」页
//
//  现在只有一件事：重置好感度。做成"卡片"而不是一排按钮，是因为设置页
//  以后还会长别的东西（开机自启、音效、缩放……），每一条占一张卡片，
//  往后加只需再 new 一张，不用重排布局。
//
//  ★ 为什么确认弹窗写在这一页里，而不是 DesktopPet ★
//    ① 弹窗要居中在面板上，得有个父窗口才定位得对；
//    ② "这个动作要不要先问一句"属于界面自己的判断。
//    这页对外只发一个信号、而且只在用户真的按下"重置"之后才发。
//    真正清零的动作仍然由 DesktopPet 执行 —— 和好感度页的摸摸/喂食同一个规矩：
//    页面只喊意图，谁持有数据谁动手。
//
//  ★ 两个安全惯例（破坏性操作别省）★
//    · 默认按钮落在「取消」上 —— 手快敲回车不会把数据删掉；
//    · Esc 也退回到「取消」—— 顺手按 Esc 时不会误确认。
// =============================================================================

#include <QWidget>

class QLabel;
class QPushButton;
class QMessageBox;
class AffectionSystem;

class SettingsPage : public QWidget
{
    Q_OBJECT
public:
    explicit SettingsPage(AffectionSystem* sys, QWidget* parent = nullptr);

    // 造出那个"确定要清零吗"的确认框（返回值由调用方 deleteLater）。
    // 单独抽出来的原因：--selftest 要离屏渲染一张，验证按钮文字和默认按钮的位置。
    // 如果自检自己另写一个弹窗，那验的就不是真正弹出来的那个了。
    static QMessageBox* makeResetConfirmBox(QWidget* parent);

signals:
    void resetAffectionRequested();   // 用户已在弹窗里确认过，可以清零了

public slots:
    void refresh();                   // 数据一变就重读上面的摘要

private slots:
    void onResetClicked();

private:
    void applyStyle();

    AffectionSystem* m_sys = nullptr;

    QLabel*      m_summary  = nullptr;   // "当前 Lv.3 亲近 · 累计 120 点 · 陪伴 5 天"
    QLabel*      m_feedback = nullptr;   // 重置完成后的一句短确认
    QPushButton* m_btnReset = nullptr;
};
