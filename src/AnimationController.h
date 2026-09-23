#pragma once
// =============================================================================
//  AnimationController —— 动画控制器
//
//  它只干一件事：知道「当前该画哪一张图」。
//
//  它负责：
//    1. 从 qrc 资源里加载 PET_IMAGE_COUNT 张 PNG
//    2. 按状态组装帧序列，并按需生成水平翻转版本（左向行走、右向转身）
//    3. 用一个 QTimer 按固定帧率推进帧号
//    4. 一次性动作播完后发出 actionFinished 信号
//    5. 防止动作互相冲突（正在播一次性动作时，不接受别的动作抢占）
//
//  它【不】负责：窗口、位置、鼠标、什么时候该换状态。
//  那些是 DesktopPet（桌宠主控制器）和 PetBehaviorController（行为控制器）的事。
//
//  这样分层的好处：想换一套素材、换帧率，改这里或 PetConfig.h 就够了，
//  完全不用碰窗口和交互的代码。
// =============================================================================

#include <QObject>
#include <QPixmap>
#include <QHash>
#include <QVector>
#include <QSize>
#include <QStringList>

#include "PetConfig.h"

class QTimer;

class AnimationController : public QObject
{
    Q_OBJECT
public:
    explicit AnimationController(QObject* parent = nullptr);

    // 加载全部 PET_IMAGE_COUNT 张图片并组装帧序列。
    // 返回 false 表示有图片没找到（具体缺哪些可以用 missingResources() 查）。
    bool loadAll();

    // 加载失败的资源路径列表（排错用：说明 qrc 里没有这张图，或路径写错了）
    const QStringList& missingResources() const { return m_missing; }

    // 生成一份人能看懂的加载报告，--selftest 会把它写进文件，方便排查"图片显示不出来"
    QString describe() const;

    // ---------------- 状态 ----------------
    PetState state() const { return m_state; }

    // 切换状态。
    // force = false（默认）：如果当前正在播一次性动作，这次切换会被忽略——这就是"防止动作冲突"
    // force = true：强行打断当前动作。鼠标点击、右键菜单这类"用户明确要求"的操作用它
    void setState(PetState s, bool force = false);

    // ---------------- 画面 ----------------
    // 当前该画的那一帧（已经按 to-flip 标记做好翻转）。
    // 如果资源没加载成功会返回一张空图（isNull() == true），paintEvent 里要判空。
    const QPixmap& currentPixmap() const;

    // 某个状态下所有帧里最大的宽高。
    // DesktopPet 用它来决定窗口要开多大，保证任何一帧都放得下、且不会被拉伸。
    QSize frameBoxSize(PetState s) const;

    // 当前状态的所有帧里最大的宽高
    QSize currentFrameBoxSize() const { return frameBoxSize(m_state); }

    // 当前画面上这一帧对应的是「哪张图 + 要不要镜像」。
    // 自检和排错用：帧号是内部状态，外面看不到，想知道"现在到底在播哪张图"就用它。
    PetFrame currentFrameRef() const;

    // 是不是已经播到"循环体的最后一帧"了。
    // DesktopPet 用它来决定收步的时机：这一圈没走完就插收步的话，画面会从
    // 正迈着腿的中间帧直接跳到"双腿并拢"的收步帧，很生硬；
    // 等这一圈走完 (2)…(6) 再接 (7)(8)，才是自然的步态收尾。
    bool atLoopTail() const;

    // 手动推进一帧，效果等同于帧定时器超时一次。
    // 只给自检/排错用：那些场景不跑事件循环，定时器不会自己触发，
    // 手动调用就能确定性地复现播放顺序（--walktrace 就是这么验行走时序的）。
    void stepFrame();

signals:
    // 帧号变了 -> DesktopPet 收到后调 update() 请求重绘
    void frameChanged();

    // 状态变了 -> DesktopPet 收到后按新帧尺寸调整窗口大小
    void stateChanged(PetState now, PetState before);

    // 一次性动作播完了 -> DesktopPet 收到后决定"接下来做什么"
    //（注意：发这个信号之后，如果没人在槽函数里改状态，控制器会自动回到 Idle）
    void actionFinished(PetState finished);

private slots:
    void onFrameTimer();

private:
    void rebuildFor(PetState s);      // 组装某个状态的帧序列（含翻转）
    void finishOneShot();             // 一次性动作播完的收尾
    void restartTimer();              // 按当前状态重新设置并启动帧定时器
    const QPixmap& flippedOf(int img);   // 取某张图的水平翻转版（第一次用时才生成并缓存）

    QHash<int, QPixmap> m_raw;              // 图片编号(1~PET_IMAGE_COUNT) -> 原图
    QHash<int, QPixmap> m_flipped;          // 图片编号 -> 水平翻转图（惰性生成）
    QHash<PetState, QVector<QPixmap>> m_resolved;   // 状态 -> 已经按 flip 标记准备好的帧序列

    QTimer* m_timer = nullptr;
    PetState m_state = PetState::Idle;
    int m_index = 0;        // 当前帧号
    int m_elapsedMs = 0;    // 当前这个一次性动作已经播了多久
    QStringList m_missing;  // 加载失败的资源
};
