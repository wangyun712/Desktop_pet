#pragma once
// =============================================================================
//  SettingsPage —— 主面板里的「设置」页
//
//  两件事：重置好感度、调界面字号。做成"卡片"而不是一排控件，是因为设置页
//  以后还会长别的东西（开机自启、音效……），每一条占一张卡片，
//  往后加只需再 new 一张，不用重排布局。
//
//  ★ 为什么确认弹窗写在这一页里，而不是 DesktopPet ★
//    ① 弹窗要居中在面板上，得有个父窗口才定位得对；
//    ② "这个动作要不要先问一句"属于界面自己的判断。
//    这页对外的信号都只在用户真的操作之后才发。
//    真正清零的动作仍然由 DesktopPet 执行 —— 和好感度页的摸摸/喂食同一个规矩：
//    页面只喊意图，谁持有数据谁动手。
//
//  ★ 两个安全惯例（破坏性操作别省）★
//    · 默认按钮落在「取消」上 —— 手快敲回车不会把数据删掉；
//    · Esc 也退回到「取消」—— 顺手按 Esc 时不会误确认。
//
//  ★ 字号这一条是"设置完立刻生效、并且记住"的，不走 DesktopPet 执行 ★
//    它改的是 UiFont 里那个全局档位（设置本身就在那儿），页面这里
//    只负责"改档位 + 落盘 + 喊一声让所有页面重套样式"。
//    ★ 它是首选项：调一次，之后每次启动都按这个大小（存 %APPDATA%/PetPal/ui.ini）。
//    字号不属于任何一方的数据，所以不需要"谁持有数据谁动手"那套（重置好感度才需要）。
// =============================================================================

#include <QWidget>

class QLabel;
class QPushButton;
class QSlider;
class QMessageBox;
class QTimer;
class AffectionSystem;

class SettingsPage : public QWidget
{
    Q_OBJECT
public:
    explicit SettingsPage(AffectionSystem* sys, QWidget* parent = nullptr);

    // 界面字号档位变了之后重新套样式表（见 UiFont.h）。
    // 注意：调用它的时候档位**已经**改好了 —— 它只负责把这一页的样子刷新一遍。
    void applyUiScale();

    // 造出那个"确定要清零吗"的确认框（返回值由调用方 deleteLater）。
    // 单独抽出来的原因：--selftest 要离屏渲染一张，验证按钮文字和默认按钮的位置。
    // 如果自检自己另写一个弹窗，那验的就不是真正弹出来的那个了。
    static QMessageBox* makeResetConfirmBox(QWidget* parent);

signals:
    void resetAffectionRequested();   // 用户已在弹窗里确认过，可以清零了
    void uiScaleChanged();            // 字号档位变了，请所有页面重套一遍样式

public slots:
    void refresh();                   // 数据一变就重读上面的摘要

private slots:
    void onResetClicked();
    void onFontScaleChanged(int percent);   // 值一变：记账 + 落盘 + （节流地）让界面真的变
    void onScaleApplyTick();                // 节流定时器到点：把攒下的最后一次改动刷上去

private:
    void applyStyle();
    void refreshFontScaleLabel();
    // 真正"让界面变"的那一下（贵）：缩 QApplication 默认字体 + 喊各页重套样式表。
    // 里面挡了一道"档位没变就不重复做"，所以节流定时器可以放心地无脑调它。
    void applyScaleNow();

    AffectionSystem* m_sys = nullptr;

    QLabel*      m_summary  = nullptr;   // "当前 Lv.3 亲近 · 累计 120 点 · 陪伴 5 天"
    QLabel*      m_feedback = nullptr;   // 重置完成后的一句短确认
    QPushButton* m_btnReset = nullptr;

    QSlider*     m_fontScale = nullptr;  // 界面字号滑条（80% ~ 150%）
    QLabel*      m_fontValue = nullptr;  // 右边那行 "115%"
    QTimer*      m_scaleApplyTimer = nullptr;   // 拖动时的节流（见 .cpp 里的说明）
    int          m_scaleApplied = -1;           // 已经把哪个档位套到界面上了
};
