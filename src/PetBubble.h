#pragma once
// =============================================================================
//  PetBubble —— 桌宠头顶的台词气泡
//
//  独立置顶小窗，机制和 DesktopLyrics 一个路数：
//    Qt::Tool + 无边框 + 透明背景 + 绝不抢焦点（WA_ShowWithoutActivating）。
//    桌宠本体窗口只有角色那么大，气泡画在里面会被裁掉，所以单独开窗。
//
//  跟随方式：显示期间用一个 30ms 的轮询定时器读桌宠窗口的位置再摆到自己
//  该在的地方 —— 桌宠怎么动（走路 / 被拖 / 下落）它都不用关心，
//  隐藏时轮询停掉，零开销。（不去钩 DesktopPet 的 applyWindowPos ——
//  那样得在每条移动路径上都插一脚，还得担心拖动路径的例外。）
//
//  生命周期：say() 显示 4 秒自动收起。isBusy() 给 DesktopPet 做节流
//  （气泡还在显示时不打断、不叠句）。
// =============================================================================

#include <QPointer>
#include <QWidget>

class QTimer;

class PetBubble : public QWidget
{
    Q_OBJECT
public:
    explicit PetBubble(QWidget* parent = nullptr);

    void say(const QString& text);     // 冒一句，4 秒后自动收起
    void trackPet(QWidget* pet);       // 跟随哪个窗口（桌宠本体）
    void petMoved();                   // 桌宠动过/换状态了：立刻重新摆位
    bool isBusy() const { return isVisible(); }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void relayout();                   // 按文本重算尺寸（最大宽内自动换行）
    void placeAbove();                 // 摆到桌宠头顶（内部有"没变就不动"的短路）

    QString           m_text;
    QPointer<QWidget> m_pet;           // 跟随的桌宠窗（QPointer：桌宠没了就不再跟随）
    QRect             m_lastPetGeo;    // 上次摆位时桌宠的几何（变了才重摆 + raise）
    QTimer*           m_hideTimer   = nullptr;   // 4 秒自动收
    QTimer*           m_followTimer = nullptr;   // 显示期间 30ms 兜底轮询
};
