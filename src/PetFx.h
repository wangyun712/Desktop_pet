#pragma once
// =============================================================================
//  PetFx —— 桌宠的粒子特效窗（爱心 / 睡觉 Zzz）
//
//  为什么是独立小窗：桌宠本体窗口只有精灵那么大，"从头顶飘起来的爱心"
//  一半时间在窗口外面 —— 画在桌宠里会被整个裁掉。所以盖一块**透明的
//  特效层**在桌宠上方，粒子在这层里自由飞。机制和 PetBubble/桌面歌词
//  同源：Qt::Tool + 无边框 + 透明 + 绝不抢焦点，**无 parent**（带 parent
//  的 Qt::Tool 透明小窗在 Windows 上显示会出问题，见 PetBubble 的教训），
//  析构由 DesktopPet 手动回收。
//
//  ★ 特效层绝不挡鼠标 ★（WA_TransparentForMouseEvents）—— 它盖在桌宠
//  头顶上方，鼠标事件必须穿透下去，桌宠才能照常被点、被拎。
//
//  粒子：QList<Particle>，33ms 一拍推进（上飘 + 左右轻摆 + 随寿命淡出）。
//  心形是 QPainterPath 贝塞尔画的（纯矢量，不需要新素材）。
//  没粒子且不在睡觉 → 定时器停 + 窗隐藏，零开销。
// =============================================================================

#include <QPointer>
#include <QVector>
#include <QWidget>
#include <QList>
#include <QPointF>

class QTimer;

class PetFx : public QWidget
{
    Q_OBJECT
public:
    explicit PetFx(QWidget* parent = nullptr);

    void trackPet(QWidget* pet);       // 覆盖谁（桌宠本体）
    void burstHearts(int count);       // 从桌宠头顶散开 n 颗小心心
    void setSleeping(bool on);         // 睡觉期间周期性飘 Zzz（醒来自动停）
    void petMoved();                   // 桌宠动过：重新覆盖（内部有短路）

    // 音频频谱（桌宠脚下）：PlayerPage 30fps 喂 16 个频段能量 [0,1]。
    // 全零时自然收工；桌宠不可见时这里直接不画（柱子不会飘在桌面上）。
    void setSpectrum(const QVector<float>& bands);
    void stopSpectrum();               // 桌宠藏进托盘时叫一声，柱子立刻归零

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    struct Particle
    {
        QPointF pos;        // 窗口内坐标
        double  vy;         // 上飘速度（负 = 向上）
        double  sway;       // 左右摆幅度
        double  phase;      // 摆动相位
        double  life;       // 剩余寿命（秒），随寿命淡出
        double  maxLife;
        qreal   size;       // 心宽 / Z 字号
        bool    zzz;        // true = 画 "Z"，false = 画心
    };

    void ensureRunning();              // 有活干：摆位 + 显示 + 启动推进定时器
    void tick();                       // 33ms：推进粒子 + 跟随 + 重绘
    void placeOver();                  // 覆盖到桌宠上方（内部有短路）
    void spawnHeart(const QPointF& headScreen);
    void spawnZzz();

    QList<Particle>   m_particles;
    QPointer<QWidget> m_pet;           // 跟随的桌宠窗
    QRect             m_lastPetGeo;    // 上次覆盖时桌宠的几何（变了才重摆）
    QTimer*           m_tickTimer = nullptr;   // 33ms 推进
    QTimer*           m_zzzTimer  = nullptr;   // 睡觉时 ~1.1s 飘一个 Z
    bool              m_sleeping  = false;
    QVector<float>    m_spec;          // 频段能量（空 = 频谱没开）
    bool              m_specAlive = false; // 这一拍频谱有活（有柱子要画）
};
