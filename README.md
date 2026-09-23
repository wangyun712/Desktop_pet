# PetPal —— Qt6 桌面宠物

一个用 **C++ / Qt 6 / Qt Widgets / CMake** 写的 Windows 桌面宠物。
不使用 QML，不使用任何第三方桌宠框架，只用 Qt 自带的那些东西。

角色是一名 Q 版紫发天使女孩，31 张透明 PNG 素材（21 张原始素材 + 8 张行走序列 + 1 张拖拽图 + 1 张下落图）。
走路动画按「**起步 (1) → 循环 (2)~(6) → 收步 (7)(8) → 站立**」三段播放，
不是把 8 张图简单循环 —— 原因见下文。

---

## 目录

1. [现在就能跑](#现在就能跑)
2. [项目结构](#项目结构)
3. [程序是怎么工作的](#程序是怎么工作的)
4. [状态机](#状态机)
5. [行走动画是怎么做的](#行走动画是怎么做的)
6. [关于素材的两条实测结论](#关于素材的两条实测结论)
7. [每个类负责什么](#每个类负责什么)
8. [启动流程](#启动流程)
9. [透明窗口是怎么实现的](#透明窗口是怎么实现的)
10. [鼠标拖动是怎么实现的](#鼠标拖动是怎么实现的)
11. [隐藏到系统托盘（右下角状态栏）](#隐藏到系统托盘右下角状态栏)
12. [从零在 Qt Creator 里建项目](#从零在-qt-creator-里建项目)
13. [文件都放在哪里](#文件都放在哪里)
14. [CMake 怎么配](#cmake-怎么配)
15. [怎么编译](#怎么编译)
16. [怎么运行](#怎么运行)
17. [排错：图片显示不出来](#排错图片显示不出来)
18. [排错：透明背景失效](#排错透明背景失效)
19. [排错：动画闪烁 / 抖动](#排错动画闪烁--抖动)
20. [打包成独立的 Windows exe](#打包成独立的-windows-exe)
21. [第二阶段：接入本地 Qwen](#第二阶段接入本地-qwen)

---

## 现在就能跑

已经编译通过并验证过：

```
[100%] Built target PetPal
BUILD_EXITCODE=0
```

自检结果（`PetPal.exe --selftest`）：

```
应加载图片: 31 张   成功: 31 张   失败: 0 张
Idle 待机            [循环]        帧数=1  窗口=183x252   #08
Drag 拖拽            [循环]        帧数=1  窗口=183x264   #30
Fall 下落            [循环]        帧数=1  窗口=183x264   #31
WalkRight 向右走     [循环]        帧数=6  窗口=215x259   #22 #23 #24 #25 #26 #27
WalkLeft 向左走      [循环]        帧数=6  窗口=215x259   #22(镜像) ... #27(镜像)
WalkStopRight 收步(右) [一次性 200ms] 帧数=2  窗口=215x259   #28 #29
WalkStopLeft  收步(左) [一次性 200ms] 帧数=2  窗口=215x259   #28(镜像) #29(镜像)
TurnLeft 向左转身    [一次性 440ms] 帧数=2  窗口=164x254   #04 #03
TurnRight 向右转身   [一次性 440ms] 帧数=2  窗口=162x254   #04(镜像) #05
...（其余状态略，完整报告见 petpal_selftest.txt）

系统托盘可用     : 是
托盘图标         : 已创建（由站立图缩出 16/20/24/32/48 五档）
托盘右键菜单     : 恢复 / 退出 —— 共 2 项

隐藏前 : 窗口可见=是  心跳=是  自动行为=是
隐藏后 : 窗口可见=否  心跳=否  自动行为=否  状态栏图标=是
恢复后 : 窗口可见=是  心跳=是  自动行为=是  状态栏图标=否
[OK] 隐藏/恢复往返正常，状态都收回来了
```

> 注意 `WalkRight` 只有 6 帧而不是 8 帧 —— 因为走路不是"22→29 一路循环"，
> 而是「起步 + 循环 + 收步」三段，收步是独立的一次性状态。
> 详见 [行走动画是怎么做的](#行走动画是怎么做的)。

光看帧表还看不出**播放顺序**对不对（比如"起步那一帧会不会在循环里又冒出来"），
所以还有一个专门的时序自检：

```
> run.bat walktrace
[report] build\ninja-Debug\petpal_walktrace.txt
```

它不跑事件循环，只手动按 100ms 的节奏推进帧，把每一步实际要画的那张图打出来：

```
【右向走】每 100ms 推进一步，共 20 步：
  1<起步> 2 3 4 5 6 2 3 4 5 6 2 3 4 5 6 2 3 4 5
【收步】走完了，切到收步：
  第1步  状态=WalkStopRight 收步(右)  画面=#28
  第2步  状态=WalkStopRight 收步(右)  画面=#29
  第3步  状态=Idle 待机  画面=#08
```

第 1 步是 `1`（起步），之后只在 `2~6` 之间打转，`1` 不会再次出现。

窗口实测（用 Win32 API 查的）：

| 检查项 | 结果 |
|---|---|
| 窗口尺寸 | 183×252，正好等于站立图 `p08.png` 的像素尺寸 |
| `IsWindowVisible` | True |
| `WS_EX_LAYERED` | **True** ← 透明窗口生效 |
| `WS_EX_TOPMOST` | **True** ← 始终置顶生效 |
| `WS_EX_TOOLWINDOW` | **True** ← 任务栏不显示按钮生效 |
| 窗口标题 | `PetPal 桌宠` |

另外把窗口尺寸和标题的原始数据贴一下，方便你对照：

- 窗口矩形 `(1363, 686) - (1546, 938)`，宽 183 高 252 —— 和 `p08.png` 的 `183x252` 完全一致，
  说明"窗口大小根据 PNG 自动适配、不拉伸"这一条是对上了的。
- 窗口外扩展样式 `0x08080088`，拆开是 `LAYERED(0x80000) | TOOLWINDOW(0x80) | TOPMOST(0x8) | 0x88`。

---

## 项目结构

```
DesktopPet/
│
├── CMakeLists.txt              构建脚本
├── README.md                   本文件
├── build.bat                   一键构建（Ninja/Debug，可选 jom / release / clean）
├── run.bat                     一键运行（run.bat [selftest] [walktrace] [release]）
│
├── src/                        源码（8 个文件）
│   ├── main.cpp                    程序入口 + --selftest / --walktrace
│   ├── PetConfig.h                 ★ 配置中心：状态枚举 + 全部可调参数 + 帧序列表
│   ├── AnimationController.h       动画控制器
│   ├── AnimationController.cpp
│   ├── PetBehaviorController.h     自动行为控制器
│   ├── PetBehaviorController.cpp
│   ├── DesktopPet.h                桌宠主控制器（同时也是那个透明窗口）
│   └── DesktopPet.cpp
│
├── docs/                       演示素材（可整个删掉，不影响程序）
│   ├── walk_preview.html           行走时序演示页（帧图内嵌，可交互播放，不是 gif）
│   ├── fall_preview.html           重力下落演示页（可拖动 g 滑块挑手感）
│   ├── drag_preview.png            待机 / 拖拽 两态按地面线对齐的对照图
│   ├── fall_preview.png            待机 / 拖拽 / 下落 三态对照图
│   └── walk_sequence_frames.png    (1)~(8) 八帧对照条
│
├── resources/                  资源
│   ├── resources.qrc               Qt 资源清单（把 PNG 打进 exe）
│   └── pet/                        31 张 PNG
│       ├── p01.png  ~  p31.png
│       └── ...
│
└── build/                      构建产物（可整个删掉，重新构建会再生成）
```

> **素材文件名说明**
>
> 你原本的文件名是 `01_行走_右脚前.png` 这种中文名。我把它改成了 `p01.png ~ p31.png`，
> 原因见 [排错：图片显示不出来](#排错图片显示不出来) 里的第 3 条 ——
> CMake 的 AUTORCC 按 GBK 解码 `.qrc`，`<file>` 标签里出现中文会导致编译失败。
>
> **中文名并没有丢**：`resources.qrc` 里用 `alias` 保留了它们，
> 程序里用的资源路径依然是 `:/pet/01_行走_右脚前.png`：
>
> ```xml
> <file alias="02_行走_右脚中.png">pet/p02.png</file>
> ```
>
> 代码、`PetConfig.h` 的注释、自检报告里看到的都是中文名，只有磁盘文件名是英文。

---

## 程序是怎么工作的

整个程序**没有一行 `while` 循环，也没有 `sleep`**。它完全靠事件驱动，
有两套互相独立的定时器在跑：

```
                       QApplication::exec()   ← 唯一的主循环，代码里看不见它
                              │
        ┌─────────────────────┴─────────────────────┐
        │                                           │
   60Hz 心跳定时器(m_tick)                 动画帧定时器(QTimer)
     DesktopPet::onTick()              AnimationController::onFrameTimer()
        │                                           │
   推进"位置"和"边界"                        推进"当前画哪一帧"
        │                                           │
   move(x,y) ──── 窗口真的在屏幕上移动 ────►  frameChanged 信号
                                                    │
                                              DesktopPet::update()
                                                    │
                                              paintEvent() 重绘
```

这样拆的好处：**"移动"和"动画"互不干扰**。
换动画帧的节奏（100ms 一次）和移动的节奏（16ms 一次）完全解耦，
所以行走时腿在动、人也在前进，不会互相拖累。

### 时间是怎么算的（为什么要那个 0.25 秒判断）

`QTimer` 的 16ms 只是"期望值"，系统调度会让它抖动，所以**不能假设每次间隔就是 16ms**。

代码的做法是：定时器只当"闹钟"，真正的时间用 `QElapsedTimer` 实测 ——
每次心跳读一次单调递增的高精度计数器，和上次相减，得到真正的帧间隔 `dt`（秒）。
这样逻辑写成 `位置 += 速度 × dt` 就**与帧率无关**：不管实际跑 60fps 还是 30fps，速度都一样。

而 `dt > 0.25` 那个分支处理的是异常情况：电脑睡眠、或者主线程被卡死几秒后，
定时器并不会补发遗漏的事件，醒来后第一次心跳测到的 `dt` 可能是几小时。
把这种"假时间"喂给移动逻辑，桌宠会瞬间飞出屏幕，所以这类帧直接丢弃。

---

## 状态机

状态枚举在 `src/PetConfig.h` 里，一共 22 个：

| 状态 | 素材 | 类型 | 说明 |
|---|---|---|---|
| `Idle` | 08 站立 | 循环 | 默认待机。**一切动作结束后都回到这里** |
| `Drag` | 30 拖拽 | 循环 | **鼠标左键按住拖动期间**显示：被拎起来、腿蜷着、头发上飘。悬空 12px 烘在图片里 |
| `Fall` | 31 下落 | 循环 | **松开鼠标后自由落体期间**显示：张开双臂、蜷着腿、带气流线。碰到屏幕底就回 `Idle` |
| `WalkRight` | **22（起步）+ 23~27（循环体）** | 循环 | 向右走，同时窗口 x 增大 |
| `WalkLeft` | 同上，全部水平翻转 | 循环 | 向左走，窗口 x 减小 |
| `WalkStopRight` | 28 → 29 | 一次性 200ms | 收步：停下前播一次，播完回待机 |
| `WalkStopLeft` | 28 → 29（镜像） | 一次性 200ms | 同上 |
| `TurnLeft` | 04 → 03 | 一次性 440ms | 向左转身 |
| `TurnRight` | 04(镜像) → 05 | 一次性 440ms | 向右转身 |
| `Happy` | 09 开心 | 一次性 900ms | |
| `Sad` | 10 委屈 | 一次性 900ms | |
| `Surprise` | 11 惊讶 | 一次性 900ms | |
| `Angry` | 12 生气 | 一次性 900ms | |
| `AngryFoot` | 19 生气跺脚 | 一次性 1100ms | |
| `Sleep` | 13 打瞌睡/睡觉 | 循环 | 一直睡到被点醒 |
| `Sit` | 14 坐下 | 循环 | |
| `Wave` | 15 挥手 | 一次性 1100ms | |
| `Think` | 16 思考 | 一次性 1500ms | |
| `Eat` | 17 吃东西 | 一次性 1100ms | |
| `Drink` | 18 喝水 | 一次性 1100ms | |
| `Daze` | 20 发呆 | 循环 | |
| `Fly` | 21 飞行 | 循环 | 边飘边水平移动，带正弦上下浮动 |

### 三类状态的行为完全不同

**循环状态**（Idle / Drag / Fall / Walk / Sleep / Sit / Daze / Fly）
永远不会自己结束，也不会发 `actionFinished` 信号。
什么时候离开由外面决定（走完一段路、发呆到点、用户点了它……）。

**一次性状态**（表情 / 挥手 / 思考 / 转身……）
播够 `petStateDurationMs()` 那么久，就发 `actionFinished(finished)` 信号，
然后**如果没人在槽函数里改状态，会自动回到 Idle**。
这一句保证了"任何特殊动作结束后都会回到 08 站立"。

**动作链**
有些动作要求连播，比如：

```
08 → 15 挥手 → 09 开心 → 08
12 生气 → 19 生气跺脚 → 10 委屈 → 08
```

用 `DesktopPet::runChain({Wave, Happy})` 表达。
`onActionFinished` 里看到链里还有东西就接着播下一个，链空了才回 Idle。

### 防止动作互相冲突

这是 `AnimationController::setState(s, force)` 里的三行判断：

```cpp
if (s == m_state) {
    if (petStateIsLooping(s)) return;   // 同一个循环状态重复设置 = 忽略
    if (!force) return;                 // 同一个一次性动作，默认不重播
}
if (!force && !petStateIsLooping(m_state)) return;   // ★ 正在播一次性动作，拒绝被抢占
```

含义：自动行为想插进来会被拒绝（比如"思考"播到一半，随机行为不能强行打断它）；
只有用户明确的操作（点击、右键菜单）才传 `force = true`，可以立刻打断。

### 状态是怎么切换的

```
用户点击 ──► DesktopPet::onPetClicked() ──► runChain({Wave,Happy})
                                              │
右键菜单 ──► 各个菜单动作 ────────────────────┤
                                              ▼
                        AnimationController::setState(force=true)
                                              │
                                      stateChanged 信号
                                              │
                              DesktopPet::resizeWindowForState()
                                  （按新状态最大的帧调整窗口大小）
                                              │
                                       frameChanged ──► update() ──► paintEvent()

自动行为 ──► PetBehaviorController::requestWalk / requestState
                        │
                        ▼
            DesktopPet::onBehaviorWalk / onBehaviorState
```

---

## 行走动画是怎么做的

### 0. 先记住一句话：走路不是"22 一路循环到 29"

走路是本项目里唯一**有结束姿态**的动作，它分三个阶段：

| 阶段 | 图片 | 播放方式 | 代码里的数组 |
|---|---|---|---|
| ① 起步 | **(1)** = #22 | **只播一次** | `WALK_START_IMGS` |
| ② 循环 | **(2)(3)(4)(5)(6)** = #23~#27 | **反复播** | `WALK_LOOP_IMGS` |
| ③ 收步 | **(7)(8)** = #28, #29 | **停下前播一次** | `WALK_STOP_IMGS` |

画面上看到的完整过程：

```
站立(08) → (1) → (2)(3)(4)(5)(6)(2)(3)(4)(5)(6)… → (7)(8) → 站立(08)
```

**为什么 (1) 不能放进循环？** 因为 (1) 和 (7) 是同一张图（MD5 完全相同，都是
`4FA86641D8`，双腿并拢的中性步态）。如果照 22→29 一路循环，每绕一圈都会在 (7)
那里重播一次 (1) 的姿势，看起来就是"走到一半卡一下"。所以让 (1) 只在开头出现一次。

（不只是逻辑推断 —— `docs/walk_sequence_frames.png` 里第 1 格和第 7 格能直接看出是同一张。）

### 1. 帧序列表（`PetConfig.h`）

```cpp
// 行走用哪几张、什么顺序 —— 改这三个数组就够了
constexpr int WALK_START_IMGS[] = { 22 };                   // (1)       起步，播一次
constexpr int WALK_LOOP_IMGS[]  = { 23, 24, 25, 26, 27 };   // (2)~(6)   循环体，反复播
constexpr int WALK_STOP_IMGS[]  = { 28, 29 };               // (7)(8)    收步，播一次
```

对应的两个状态：

```cpp
{ PetState::WalkRight,     petWalkFrames(false)     },  // 起步帧 + 循环体
{ PetState::WalkLeft,      petWalkFrames(true)      },  // 同上，全部水平翻转
{ PetState::WalkStopRight, petWalkStopFrames(false) },  // 收步两帧（一次性）
{ PetState::WalkStopLeft,  petWalkStopFrames(true)  },  // 同上，镜像
```

`petWalkFrames(false)` 展开后是 `{{22,false},{23,false},…,{27,false}}`；
`{22,false}` 的意思是"播第 22 张图，不翻转"，`{22,true}` 是"播第 22 张图的水平翻转版"。
左向行走完全靠翻转，**不需要额外准备左向图片**。

### 1.5 起步帧只播一次，是靠"循环回绕点"实现的

普通循环状态是 `m_index = (m_index + 1) % count`，末尾绕回第 0 帧。
但行走的序列里第 0 帧是起步帧，不能绕回去。所以多了一个回绕点：

```cpp
// PetConfig.h
inline int petStateLoopBegin(PetState s)
{
    // 行走序列 = 起步帧 + 循环体帧，绕回时跳过前面的起步帧
    if (s == PetState::WalkRight || s == PetState::WalkLeft)
        return PetCfg::WALK_START_COUNT;      // = 1
    return 0;
}

// AnimationController::onFrameTimer
const int begin = qBound(0, petStateLoopBegin(m_state), count - 1);
m_index = (m_index + 1 >= count) ? begin : m_index + 1;
```

同一处还顺手修了一个老问题：**一次性动作不再绕回开头**，而是停在最后一帧
（`else if (m_index + 1 < count)`）。否则收步的 `28 → 29` 会在末尾闪一下 28，
转身的 `04 → 03` 也是同样的毛病。

**收步是在什么时候切过去的？** `DesktopPet::tickMovement()` 里判断
"这段路只剩 `WALK_STOP_MS`（200ms）了"就切到收步，并停掉水平移动：

```cpp
if (m_moveMsLeft <= PetCfg::WALK_STOP_MS)
{
    beginWalkStop();     // 切到 WalkStopRight / WalkStopLeft
    return;
}
```

收步这两帧播完会发 `actionFinished`，`DesktopPet::onActionFinished()` 收到后才
`goIdle()` 回到站立。**不能提前回待机**，否则收步动画会被从中间掐断。

收步期间还会有大约 200ms 的**线性减速**（速度从 100% 衰减到 0，总共只挪几像素），
比"啪一下刹停"自然。

### 2. 水平翻转

```cpp
QPixmap mirror = src.transformed(QTransform().scale(-1, 1), Qt::SmoothTransformation);
```

`QTransform().scale(-1, 1)` 就是"x 方向取反"，也就是左右镜像。
`QPixmap::transformed()` 会自动把结果的包围盒平移回原点，
所以翻转后的图尺寸不变、不会歪，直接就能画在原地。

翻转版是**第一次用到时才生成并缓存**的，不会拖慢运行。

### 3. 真的在移动，不是原地播放

这是最关键的一点。每帧同时做两件事：

```cpp
// AnimationController：每 100ms 换一帧（循环状态才绕回，一次性动作停在末帧）
if (petStateIsLooping(m_state))
    m_index = (m_index + 1 >= count) ? petStateLoopBegin(m_state) : m_index + 1;

// DesktopPet::tickMovement：每 16ms 推进一次位置
m_pos.setX(m_pos.x() + m_moveDir * PetCfg::WALK_SPEED_PPS * dt);
move(qRound(m_pos.x()), qRound(m_pos.y()));
```

位置用 `double` 累加，只有真正调用 `move()` 时才取整。
如果直接用 `int` 累加，60px/s 在 60Hz 下每帧只有 1 像素，误差会被吃掉导致"走不动"。

### 4. 到屏幕边缘自动转向

```cpp
if (m_pos.x() < loX)      { m_pos.setX(loX); onHitEdge(); }   // 撞左边界
else if (m_pos.x() > hiX) { m_pos.setX(hiX); onHitEdge(); }   // 撞右边界

void DesktopPet::onHitEdge() {
    if (m_move == Move::None) return;                            // 没在移动，不管
    if (state == TurnLeft || state == TurnRight) return;         // 正在转身，别重复触发
    turnTo(-m_moveDir);                                          // 掉头
}
```

### 5. 转身

需求里提到用 04 背面图做转身。这里做得更完整一点：

```
TurnRight = 04 背面(镜像)  →  05 朝右侧身   →  继续向右走
TurnLeft  = 04 背面        →  03 朝左侧身   →  继续向左走
```

顺序是"先背对观众，再转向新方向"，看起来像真的转了个身，
而不是把一张背面图硬翻转了事。转身过程中**原地不动**，转完才继续走，
所以不会出现"侧着身子平移"的怪现象。

---

## 关于素材的两条实测结论

### 一、行走序列（22~29）是怎么入库的

新加入的 8 张行走图（原文件名 `image-1 (1).png` ~ `image-1 (8).png`）是 1254×1254 的
高分辨率画布，而原来的素材是紧贴角色裁剪的 ~180×252。**直接拿来用会有两个问题**：
窗口会变成 1254×1254（角色占半个屏幕）、而且每张的包围盒都不一样（走路会抖）。
所以入库前做了三步统一处理：

| 步骤 | 做法 | 目的 |
|---|---|---|
| 缩放 | 全部乘同一个比例 `0.21212`，让角色高度 ≈ 252px | 和 `p08.png`（站立 183×252）同一个尺寸档，不会突然变大 |
| 裁切 | 8 张用**同一个**裁切框，输出尺寸完全相同（215×259） | 同尺寸 ⇒ 走路时窗口不需要 resize，帧间不会跳 |
| 预乘 alpha | 先乘 alpha 再缩放，然后除回来 | 透明边缘不会因为黑底混色而发灰 |

处理完的对齐校验（角色在画布里的位置）：

```
角色中心 x 相对画布中心：-0.5 ~ +2.5 px
角色底边相对画布底边  ：  0 ~ -3 px
```

也就是说，8 帧之间角色左右摆动不超过 2.5px、上下不超过 3px —— 那是动画本身的
重心起伏，应该保留；相对站立图 `p08` 的落差也只有 3px（约 1.2% 身高），看不出跳动。

### 二、为什么转身还是用老素材的 03 / 04 / 05

老素材里我**逐帧做过数值分析**（alpha 轮廓重合度 + 眼睛像素数），结论是：

| 图片 | 实际姿态 | 依据 |
|---|---|---|
| 01 右脚前 | 正面视角（双眼可见） | 眼睛像素 120 个，重心几乎在头中部 |
| 02 右脚中 | 正面视角 | 眼睛像素 113 个 |
| **03 右脚后** | **朝左的侧身** | 只有 30 个眼睛像素（单眼），眼在头的左侧 |
| 04 背面 | 背面视角 | 眼睛像素 0 个 |
| **05 左脚前** | **朝右的侧身** | 只有 29 个眼睛像素（单眼），眼在头的右侧 |
| 06 左脚中 | 正面视角 | 眼睛像素 110 个 |
| 07 左脚后 | 正面视角 | 眼睛像素 158 个 |
| 08 站立 | 正面视角 | 眼睛像素 144 个 |

另外还测到：**05 正好是 03 的水平镜像**（把 03 翻转后和 05 做轮廓重合度，IoU = 0.968，几乎完全重合）。

所以老素材里 03/05 是左右两个方向的侧身、04 是背面 —— 它们串进行走循环会变成
"走两步突然把头扭到另一边"（就是需求里明确禁止的那种感觉）。但它们**恰好是转身动画
最需要的三个姿态**，所以现在这样分工：

| 素材 | 现在的职责 |
|---|---|
| **22** | 行走的**起步帧**（只在开头播一次） |
| **23~27** | 行走的**循环体**（反复播） |
| **28 / 29** | 行走的**收步帧**（停下前播一次） |
| 04 → 03 | `TurnLeft`：先背对观众，再转向左侧 |
| 04(镜像) → 05 | `TurnRight`：先背对观众，再转向右侧 |
| 08 | `Idle` 待机站立 |
| 01 / 02 / 06 / 07 | 已不参与任何状态，保留备用 |

> 顺带一提：**28 和 22 是同一张图**（MD5 都是 `4FA86641D8`）。这不是我搞错了 ——
> 原始素材 `image-1 (1).png` 和 `image-1 (7).png` 本来就是同一个文件。
> 正好一个当起步、一个当收步，所以看起来是"首尾呼应"。

> **别顺手删掉 `p01/p02/p06/p07`** —— 它们不参与动画了，但同组的 `p03/p04/p05`
> 还在给转身用，整套素材留在一起更不容易搞混。

### 想调整行走序列

只改 `src/PetConfig.h` 里的三个数组，**不用动任何逻辑代码**：

```cpp
// 现在：起步 (1)，循环 (2)~(6)，收步 (7)(8)
constexpr int WALK_START_IMGS[] = { 22 };
constexpr int WALK_LOOP_IMGS[]  = { 23, 24, 25, 26, 27 };
constexpr int WALK_STOP_IMGS[]  = { 28, 29 };
constexpr int WALK_START_COUNT  = 1;
constexpr int WALK_LOOP_COUNT   = 5;
constexpr int WALK_STOP_COUNT   = 2;

// 不要起步帧（走路一开始就直接迈步）：清空数组、计数写 0 即可
constexpr int WALK_START_IMGS[] = { };
constexpr int WALK_START_COUNT  = 0;

// 步子更大：把 6 也放进循环，或者调整循环体的顺序
constexpr int WALK_LOOP_IMGS[]  = { 23, 24, 25, 26, 27, 28 };
constexpr int WALK_LOOP_COUNT   = 6;   // 记得同步改元素个数
```

改完重新编译即可（计数要和数组元素个数一致）。

> 改的是 `PetConfig.h`（**头文件**）—— 编完记得确认跑的是新 exe。
> 自检报告头两行的"编译时间"就是干这个用的。

**怎么验证改对了？**

```
> run.bat walktrace
```

它会把真实的播放顺序逐帧打出来（见 [现在就能跑](#现在就能跑)），
顺序不对一眼就能发现，不用盯着屏幕猜。

**想看动画长什么样？** 打开 `docs/walk_preview.html` ——
它把 8 张帧图内嵌在文件里，按真实的 100ms 节奏播放一遍完整流程
（起步 → 循环 N 圈 → 收步 → 站立），可以自己选循环圈数。**不是 gif。**

---

## 每个类负责什么

### `AnimationController`（动画控制器）

**只干一件事：知道"当前该画哪一张图"。**

1. 从 qrc 资源加载全部 PNG（现在是 31 张）
2. 按状态组装帧序列，并按需生成水平翻转版本
3. 用一个 `QTimer` 按固定帧率推进帧号
4. 一次性动作播完后发 `actionFinished` 信号
5. 防止动作互相冲突

**它不管**：窗口、位置、鼠标、什么时候该换状态。

### `PetBehaviorController`（自动行为控制器）

**决定"什么时候自己干点什么"，但不亲自动手。**

- 什么时候站起来、发呆、坐下、走一段路（概率 + 随机时长）
- 多久没人互动了，该犯困、该睡觉

决定好之后只发信号（`requestState` / `requestWalk`），由 `DesktopPet` 执行。

计时用一个"单次定时器"，每次做完决定就按**随机**间隔重新排期 ——
所以动作之间不会像秒表一样规整，桌宠看起来才像活的。

### `DesktopPet`（桌宠主控制器）

它直接继承 `QWidget`，所以"窗口"就是"桌宠"。

1. 透明无边框窗口的创建
2. 把当前帧画出来（`paintEvent`）
3. 鼠标事件：区分「点击」和「拖动」、右键菜单
4. 位置、自动移动、屏幕边界、多显示器
5. 状态切换的调度中心

### `PetConfig.h`（配置中心）

只有枚举、常量和数据表，没有任何逻辑。
**想改动作快慢、走路速度、随机行为时间范围、行走用哪几张图，全在这里改。**

---

## 启动流程

```
main()
 │
 ├─① QApplication::setHighDpiScaleFactorRoundingPolicy(PassThrough)
 │      必须在 QApplication 构造之前。PassThrough = 不做取整，
 │      否则 125% 缩放会被四舍五入成 100%/200%，桌宠大小忽大忽小。
 │
 ├─② QApplication app(argc, argv)
 │      Qt 的地基：窗口系统、消息循环、字体。必须先于任何 QWidget。
 │
 ├─③ 若命令行带 --selftest → 跑自检、写报告、退出（不显示窗口）
 │
 ├─④ DesktopPet pet;          构造：设置窗口标志和属性、建两个控制器、连信号槽、建右键菜单
 │
 ├─⑤ pet.start()
 │     1) m_anim->loadAll()          加载全部图片
 │     2) resizeWindowForState(Idle)  按站立图尺寸定窗口大小
 │     3) 摆到屏幕右下角附近
 │     4) show()                      ← 这一句就是"启动程序后直接显示桌宠"
 │     5) 启动 60Hz 心跳
 │     6) m_behavior->start()        开始自动行为
 │
 └─⑥ return app.exec()
       阻塞在这里跑 Qt 事件循环，直到有人调用 quit()。
       程序"活"着的整个过程都发生在这行里面。
```

> 注意：`DesktopPet` 对象建在 `main` 的栈上。
> 它内部的窗口、定时器、两个控制器都以它为 parent，
> 所以退出时会被自动回收，不会内存泄漏。

---

## 透明窗口是怎么实现的

四个窗口标志 + 四个属性，缺一不可：

```cpp
setWindowFlags(Qt::FramelessWindowHint          // 无边框：去掉标题栏和边框，只剩客户区
             | Qt::WindowStaysOnTopHint         // 始终在其他普通窗口之上
             | Qt::Tool                        // 工具窗口：任务栏不显示按钮、不进 Alt+Tab
             | Qt::NoDropShadowWindowHint       // 关掉系统投影，否则透明窗口边缘会有一圈灰影
             | Qt::WindowDoesNotAcceptFocus);   // 不抢键盘焦点，不打断你打字

setAttribute(Qt::WA_TranslucentBackground, true);   // 画布带 alpha 通道
setAttribute(Qt::WA_NoSystemBackground,   true);    // 不让系统先刷一层不透明底色
setAttribute(Qt::WA_ShowWithoutActivating, true);   // show() 时不激活到前台
setAttribute(Qt::WA_OpaquePaintEvent,     false);   // 明确告诉 Qt：这里要画半透明
```

最后在 `paintEvent` 里把整块区域刷成完全透明：

```cpp
p.fillRect(rect(), Qt::transparent);   // 这一句不能省，否则移动/缩小后会留拖影
p.drawPixmap(frameTopLeft(pm), pm);
```

**画法：底边对齐 + 水平居中，按原始像素尺寸画，不做任何缩放。**

为什么不是"把图片拉伸铺满窗口"？因为所有图尺寸各不相同
（老素材 153×254 ~ 358×217，行走序列统一为 215×259），拉伸会让角色一会儿胖一会儿瘦 —— 那正是需求里禁止的。

**窗口大小怎么定？**
按"当前状态里最大的那一帧"来开窗口，而不是固定用全局最大的 358 宽：

- 站立 183×252、行走 215×259、睡姿那张宽达 358
- 如果一直用 358，走路时会有一大片看不见的窗口挡住桌面点击，撞到屏幕边也会提前停下

改尺寸时会让"角色脚底中心"在屏幕上保持不动，否则窗口一变尺寸角色就会跳一下。

---

## 鼠标拖动是怎么实现的

关键在**按下时不立刻开始拖，而是等移动超过阈值**：

```cpp
// 按下
m_pressed = true;
m_dragging = false;
m_pressGlobal  = event->globalPosition().toPoint();   // 鼠标在屏幕上的位置
m_pressTopLeft = pos();                               // 窗口左上角的位置

// 移动
const QPoint delta = event->globalPosition().toPoint() - m_pressGlobal;

if (!m_dragging) {
    if (delta.manhattanLength() < PetCfg::DRAG_THRESHOLD_PX)   // 默认 4 像素
        return;                       // 还没超过阈值 → 依然可能是"点击"，先不动
    m_dragging = true;                // 正式进入拖动
    stopMoving();                     // 拖起来就别走了
    m_anim->setState(PetState::Drag, true);   // 换成"被拎起来"的 30 号图
    m_pressTopLeft = pos();           // 切图后窗口尺寸变了，基准要重取（见下节）
}

m_pos = QPointF(m_pressTopLeft + delta);   // 桌宠跟随鼠标
clampToScreen(/*allowTurn=*/false);        // 钳到屏幕边上就停住，别顺手转身

// 松开
if (wasDragging) { startFall(); }           // 放下 -> 交给重力，自由落体回地面
else             { onPetClicked(); }        // ★ 没拖动过才算"点击"
```

用 `globalPosition()` 而不是 `position()`：后者是窗口内的相对坐标，
窗口自己在移动，用它算位移会变成"自己追自己"，拖起来会抽搐。

### 拖拽时换一张图（30 号素材）

按住拖动期间，角色不是干站着被拽，而是换成 `30_拖拽.png` —— 一张"被拎起来"的图
（双腿蜷着、头发往上飘、一脸慌张）。松手后不是直接站回去，而是**交给重力自由落体**，
掉到屏幕底边才站定（见下面的「松手之后会掉下去」）。

```cpp
// 越过阈值、正式进入拖动的那一刻
m_dragging = true;
stopMoving();
m_anim->setState(PetState::Drag, true);   // ← 换成拖拽图

m_pressTopLeft = pos();                   // ★ 见下面的"为什么必须重取基准"
```

松手时不是切回站立，而是切进「下落」：

```cpp
if (wasDragging)
    startFall();              // 放下 -> 从当前高度自由落体（见下一节）
else
    onPetClicked();           // 没拖动过才算"点击"
```

> 注意 `startFall()` 里已经处理了"松手时本来就贴着地面"的情况 ——
> 那种情况没得掉，直接站定，不会先闪一帧下落图。

#### 那 12 像素的悬空感是从哪来的

`p30.png` 的画布是 **183×264**，而站立图是 **183×252** —— 多出来的 12px 全留在画布底部：
**角色底边距画布底边正好 12px**。

窗口是按"当前状态的画布"开的，而改尺寸时 `resizeWindowForState()` 会
**锁住窗口底边中心不动**。所以切到拖拽图时，窗口只是往上长了 12px，
角色跟着被抬高 12px —— 而窗口底边（脚原来踩的那条线）纹丝不动。
看起来就是**被拎离地面**，松手后掉回地面（见下一节）。

> 想改悬空量：得重新处理 `resources/pet/p30.png`
>（画布高 = 252 + 悬空量，角色贴着画布顶部放），改代码是改不动的 —— 这个量烘在图片里。

#### 松手之后会掉下去（重力）

松手那一刻，角色不是"啪"地变回站立，而是**从当前高度自由落体**，越掉越快，
落到屏幕底部（可用区底边）才站定。物理公式就两步：

```cpp
v += g * dt;      // 速度累加重力
y += v * dt;      // 位置累加速度
```

`g` 默认 **2200 px/s²**，另有终端速度上限 1800 px/s
（兜底用：4K 竖屏从最顶上掉下来时，速度会大到像一道残影）。

**为什么不让它直接切回站立？**
拖到半空松手时，角色瞬间变回站立就等于"瞬移回地面"，是纯粹的 bug 观感。
有了重力，它才是"掉下来"。

**落地时那 12px 是怎么消失的** —— 这一段是这次设计的关键：

| 时刻 | 窗口高 | 窗口底边 | 角色脚离地 |
|---|---|---|---|
| 下落中（`Fall`，p31） | 264 | 钉在地面线上 | **12 px** |
| 落地切站立（`Idle`，p08） | 252 | **一动不动** | **0** |

`Fall` 和 `Drag` 的画布是**逐像素同规格**的（都是 183×264，角色底边都距画布底 12px），
所以**松手换图时窗口一个像素都不 resize，画面绝对不会抖**。
真正收高度只发生在落地那一瞬间：`resizeWindowForState()` 锁的是**窗口底边中心**，
底边不动、高度少 12px ⇒ 顶边往下 12px ⇒ 角色脚正好从"离地 12px"沉到地面线上。

**想调手感**：改 `PetCfg::GRAVITY_PPS2` 一个数就行。

| g (px/s²) | 从屏幕顶部掉到底大约 |
|---|---|
| 1200 | 1.3 秒，轻飘飘的，像片羽毛 |
| **2200** | **0.95 秒（当前默认）** |
| 4000 | 0.7 秒，砸得很实在 |

> 挑手感不用改代码重编译：`docs/fall_preview.html` 是可交互的演示页，
> 拖那个 g 滑块就能试（还能看实时速度和位移）。挑好了把数字抄进 `PetConfig.h` 即可。

**下落期间，点击和自动行为都会被挡掉**：

- **点击**：下落不到 1 秒，点它没意义；而如果真让它切了表情，
  状态会变成"悬在半空做表情"—— 因为位置已经掉到一半，移动逻辑却停了。
- **自动行为**：行为控制器 5~15 秒掷一次骰子。不挡这一下的话，
  随机动作会把下落图顶掉，而且它一 `stopMoving()` 就停在半空再也掉不下去
  （角色悬在空中，看着像卡死）。
- 唯一能打断下落的是**在空中再抓住它** —— 那时调的是
  `stopMoving(snapToGround = false)`，位置原样定住，
  不会出现"抓住它 → 它反而先掉到底 → 再弹回手里"。

#### 为什么切图之后要重取一次拖动基准

拖动是这样跟手的：`窗口左上角 = 按下时的左上角 + 鼠标位移`。

但切到拖拽图会让窗口高度从 252 变成 264，同一个"左上角"现在对应到的位置已经不同了。
基准不重取的话，按住那一瞬间角色会往下掉 12px（正好等于悬空量），手感上像"抓空了"。
切图后重新取一次 `pos()`，这一跳就完全消失。

#### 拖动期间自动行为会被屏蔽

行为控制器每隔 5~15 秒就掷一次骰子决定下一个动作。不挡这一下的话，
只要按住拖着超过十几秒，拖拽图就会被"开心一下""发呆""自己走两步"顶掉 ——
手里正拎着的角色当场就不听话了。所以：

```cpp
void DesktopPet::onBehaviorState(PetState s, int holdMs)
{
    if (m_dragging) return;   // 手里拎着的时候，什么自动行为都别插队
    ...
}
```

### 像素级命中判定

窗口是个矩形，但角色只占其中一部分。如果不做判定，点角色旁边的空白也会把桌宠拖走。
所以按下时会查这一帧图片在对应像素的 alpha：

```cpp
const QPoint p = localPos - frameTopLeft(pm);
return qAlpha(pm.toImage().pixel(p)) > 8;    // 只有摸到角色的实体像素才有反应
```

不想开就在 `PetConfig.h` 里把 `HIT_TEST_ALPHA` 改成 `false`，整个窗口矩形都能点。

---

## 隐藏到系统托盘（右下角状态栏）

桌宠不想要的时候不必退程序 —— 收进右下角状态栏就行。

### 怎么用

1. 在桌宠身上**右键**，菜单最下面两项是「隐藏到状态栏」和「退出」：

   ```
   …
   暂停自动行为
   恢复自动行为
   ─────────────────
   隐藏到状态栏      ← 收进状态栏，程序还活着
   退出             ← 真退出
   ```

2. 点「隐藏到状态栏」：桌宠窗口消失，右下角状态栏出现一个小图标
   （用站立图缩小的，和它站着时的样子一样，一眼能认出来）。
3. 想叫回来：**在那个图标上左键单击**；也可以**右键 → 恢复**。
   右键菜单里同时有「退出」，不想留了从那儿关。

> 这两项挨在一起最容易点错，区别就一句话：
> **隐藏 = 收起来，进程还在；退出 = 进程结束。**

### 实现要点

`DesktopPet::setupTray()` 建图标和托盘菜单，`hideToTray()` / `restoreFromTray()`
负责来回。托盘图标**不另做 `.ico`**，直接拿 `petImagePath(TRAY_ICON_IMG)` 那张站立图
按 16/20/24/32/48 五个尺寸各烘一份交给系统去挑（本机 150% 缩放，实际用的是 32 那份）。

托盘图标**平时不显示** —— 只在藏进状态栏之后才亮出来。平时桌宠本来就站在屏幕上，
再挂一个状态栏图标等于同一只宠物开了两个入口。

三个容易埋雷的地方，都处理了：

| 隐患 | 不处理的后果 | 怎么处理的 |
|---|---|---|
| 隐藏时 60Hz 心跳还在跑 | 窗口都看不见了，每秒白算 60 次位置 | `hideToTray()` 里 `m_tick->stop()` |
| `m_clock` 是单调时钟，藏起来这段时间它一直在走 | 恢复后第一次心跳算出的 dt 是"藏了多久"，画面顿一下 | `restoreFromTray()` 里重取时钟基准 |
| 用户可能本来就暂停了自动行为 | 恢复时擅自帮他打开 | 藏之前把 `autoEnabled()` 存进 `m_autoBeforeHide`，回来原样还回去 |

另外隐藏前会先 `stopMoving()` + `goIdle()`：不然藏起来的这段时间它还在"走"，
再放出来时位置已经溜到别处去了。

### 托盘图标长什么样

`--selftest` 会把程序真正用到的那 5 档尺寸各放大 4 倍并排存成 `petpal_tray_icons.png`。
状态栏里那个图标只有 16px 高，**缩到那么小还认不认得出是小人**是这个功能唯一的观感风险
（实测：紫发和蓝裙的轮廓还在，能认出来）。想换图标就改 `PetConfig.h` 里的 `TRAY_ICON_IMG`,
对照图名表填编号即可（13 是打瞌睡、21 是飞行）。

### 自检覆盖到哪

`--selftest` 的报告里有一节会真的跑一遍**隐藏 → 恢复往返**，走的就是菜单项和托盘图标
调的那两条路径（不是另写一套等价的代码），把每一步的状态都打出来：

```
隐藏前 : 窗口可见=是  心跳=是  自动行为=是
隐藏后 : 窗口可见=否  心跳=否  自动行为=否  状态栏图标=是
恢复后 : 窗口可见=是  心跳=是  自动行为=是  状态栏图标=否
[OK] 隐藏/恢复往返正常，状态都收回来了
```

为什么只能验到这一步：**点托盘图标没法自动模拟** —— `PostMessage` 伪造鼠标对 Qt6 不生效
（它用 `GetMessagePos()` 取全局坐标），`SendInput` 又会真抢用户的鼠标。
所以自动验证只覆盖"状态有没有收干净"，**托盘图标本身的点击手感得自己点一下**。

---

## 从零在 Qt Creator 里建项目

本项目已经建好了，你直接打开就行。如果要从零再造一个，步骤如下：

1. **打开 Qt Creator → 文件 → 新建项目**
2. 选择 **Application (Qt) → Qt Widgets Application**
3. 项目名填 `PetPal`，路径选 `E:\code\DesktopPet`
4. **构建系统选 CMake**（不要选 qmake）
5. "Class Information"（类信息）这一步：
   - **把 Base class 改成 `<Custom>`，ClassName 留空**
   - 项目里不需要 Qt Creator 自动生成的 `MainWindow`，
     桌宠窗口是我们自己写的 `DesktopPet`
   - 如果它已经生成了 `mainwindow.h/.cpp/.ui`，删掉这三个文件
6. 完成。然后把本项目 `src/` 和 `resources/` 里的文件按下面的结构放进去

> 用 Qt Creator 打开已有项目时：**文件 → 打开文件或项目 → 选 CMakeLists.txt**（不是选 .cpp）。

---

## 文件都放在哪里

```
E:\code\DesktopPet\
├── CMakeLists.txt
├── README.md
├── src\
│   ├── main.cpp
│   ├── PetConfig.h
│   ├── AnimationController.h
│   ├── AnimationController.cpp
│   ├── PetBehaviorController.h
│   ├── PetBehaviorController.cpp
│   ├── DesktopPet.h
│   └── DesktopPet.cpp
└── resources\
    ├── resources.qrc
    └── pet\
        ├── p01.png ~ p31.png        共 31 张
```

**31 张 PNG 放在 `resources\pet\` 里。**

对应关系（`resources.qrc` 里也写了注释）：

| 文件 | 含义 | 文件 | 含义 |
|---|---|---|---|
| p01.png | 01 行走_右脚前 | p12.png | 12 表情_生气 |
| p02.png | 02 行走_右脚中 | p13.png | 13 打瞌睡_睡觉 |
| p03.png | 03 行走_右脚后（朝左侧身） | p14.png | 14 坐下 |
| p04.png | 04 行走_背面 | p15.png | 15 挥手 |
| p05.png | 05 行走_左脚前（朝右侧身） | p16.png | 16 思考 |
| p06.png | 06 行走_左脚中 | p17.png | 17 吃东西 |
| p07.png | 07 行走_左脚后 | p18.png | 18 喝水 |
| p08.png | 08 站立 | p19.png | 19 生气跺脚 |
| p09.png | 09 表情_开心 | p20.png | 20 发呆 |
| p10.png | 10 表情_委屈 | p21.png | 21 飞行 |

**22~29 是本轮新加入的行走序列**（原文件名 `image-1 (1).png` ~ `image-1 (8).png`，
已统一缩放到 215×259）：

| 文件 | 含义 | 文件 | 含义 |
|---|---|---|---|
| p22.png | 22 行走_右01 | p26.png | 26 行走_右05 |
| p23.png | 23 行走_右02 | p27.png | 27 行走_右06 |
| p24.png | 24 行走_右03 | p28.png | 28 行走_右07 |
| p25.png | 25 行走_右04 | p29.png | 29 行走_右08 |

**30 / 31 是后加入的两张状态图**（原文件分别是 1069×1472 和 1068×1472，都已缩到 183×264）：

| 文件 | 含义 | 说明 |
|---|---|---|
| p30.png | 30 拖拽 | 鼠标左键按住拖动时显示。画布比站立图高 12px，那 12px 就是"被拎起来"的悬空量 |
| p31.png | 31 下落 | 松手后自由落体期间显示。**画布刻意和 p30 做到逐像素一致**，所以松手换图时窗口不 resize、画面不抖 |

（`p11.png` 对应 `11 表情_惊讶`，上面第一张表里已经列过。）

**新增/替换图片的流程**：把文件放进 `resources\pet\` → 在 `resources.qrc` 里加/改一行 →
在 `PetConfig.h` 的 `petImageNames()` 里对号 → **重新编译**（`.qrc` 是编译期处理的，不重编不生效）。

---

## CMake 怎么配

`CMakeLists.txt` 全文（关键处都有注释）：

```cmake
cmake_minimum_required(VERSION 3.24)
project(PetPal LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)   # 头文件里有 Q_OBJECT 就必须开，否则信号槽会链接失败
set(CMAKE_AUTORCC ON)   # 自动编译 resources.qrc，全部 PNG 靠它打包

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

set(PROJECT_SOURCES
    src/main.cpp
    src/PetConfig.h
    src/AnimationController.h      src/AnimationController.cpp
    src/PetBehaviorController.h    src/PetBehaviorController.cpp
    src/DesktopPet.h               src/DesktopPet.cpp
)
set(PROJECT_RESOURCES resources/resources.qrc)

# WIN32 = GUI 子系统：双击运行时不弹黑色控制台窗口
add_executable(PetPal WIN32 ${PROJECT_SOURCES} ${PROJECT_RESOURCES})

target_include_directories(PetPal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(PetPal PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)

if (MSVC)
    target_compile_options(PetPal PRIVATE /utf-8 /wd4828)
endif()
```

三个必须理解的开关：

| 开关 | 为什么必须 |
|---|---|
| `CMAKE_AUTOMOC` | 头文件里有 `Q_OBJECT`，需要 moc 生成元对象代码。不开会报 `undefined reference to vtable` |
| `CMAKE_AUTORCC` | 自动把 `.qrc` 编译进程序。不开图片就是"读不到" |
| `add_executable(... WIN32 ...)` | 用 GUI 子系统。不加的话程序启动会先弹一个黑色控制台 |

`/utf-8` 告诉编译器源码是 UTF-8（本项目所有源码都存成 UTF-8 无 BOM）。
`/wd4828` 关掉一个无害的警告 —— rcc 生成的 `.cpp` 里会把中文文件名写成注释，
而那一行是按 GBK 写的，在 `/utf-8` 下算"非法 UTF-8 字节"，每张图刷一条警告。它只影响注释。

### 在 Qt Creator 里配置 Kit

**工具 → 选项 → Kits**，确认：
- Qt Version：`Qt 6.11.2 MSVC2022 64bit`
- Compiler：`Microsoft Visual C++ Compiler 17.x (amd64)`
- CMake：`E:\QT\Tools\CMake_64\bin\cmake.exe`

然后 **构建 → 执行 CMake**（或直接点构建，Qt Creator 会自动跑）。

命令行手动配置：

```bat
E:\QT\Tools\CMake_64\bin\cmake.exe -S E:\code\DesktopPet ^
    -B E:\code\DesktopPet\build\Desktop_Qt_6_11_2_MSVC2022_64bit_Debug ^
    -G "NMake Makefiles JOM" ^
    -DCMAKE_BUILD_TYPE=Debug ^
    -DCMAKE_PREFIX_PATH=E:\QT\6.11.2\msvc2022_64
```

---

## 怎么编译

### ★ 先说清楚：本机有两个构建目录

| 目录 | 谁在用 | 生成器 |
|---|---|---|
| `build\ninja-Debug\` | **`build.bat` / `run.bat`（推荐）** | Ninja |
| `build\Desktop_Qt_6_11_2_MSVC2022_64bit_Debug\` | Qt Creator | NMake / JOM |

它们是**两份完全独立的产物**。在 Qt Creator 里按 Ctrl+B，更新的只是后者；
跑 `run.bat`，更新的只是前者。

**改了代码只构建了其中一个、却运行另一个，现象就是"改完完全没生效"** ——
新加的图片不显示、参数改了没反应，八成都是这个原因。

所以自检报告的**头两行**专门写了 `exe` 路径和 `编译时间`。
拿它和源文件时间戳一比，跑没跑对立刻见分晓。

### 为什么推荐 Ninja（`build.bat` / `run.bat`）

因为 JOM 在**本机的头文件依赖扫描是坏的**：实测这几个文件

```
CMakeFiles\PetPal.dir\src\DesktopPet.cpp.obj.d           0 字节
CMakeFiles\PetPal.dir\src\AnimationController.cpp.obj.d  0 字节
CMakeFiles\PetPal.dir\src\main.cpp.obj.d                 0 字节
```

全是空的 —— MSVC 的 `/showIncludes` 结果没被写进去。
后果是：**改了 `PetConfig.h`，除了自身也一起改过的那个 `.cpp`，
其余编译单元一律不重编**。链接出来的 exe 里内联的还是旧常量。

Ninja 把依赖存在二进制 depfile 数据库里，没有这个问题。

> 真实踩坑记录：`PetCfg::PET_IMAGE_COUNT` 从 29 改成 30（新增拖拽图 `p30`）后，
> Qt Creator 构建出的 exe 自检仍然报「应加载图片 29 张」，新图怎么拖都不显示；
> 同一份源码用 `run.bat` 构建却一切正常。差别就在这个依赖扫描上。

`CMakeLists.txt` 里已经用 `OBJECT_DEPENDS` 给 `PetConfig.h` 挂了一条**显式依赖**兜住这个坑
（见文件里那段注释），所以现在 JOM 也能正确重编。
但"改完头文件用 `run.bat` 跑一遍"仍然是更省心的习惯。

### 用 Qt Creator

1. 打开 `CMakeLists.txt`
2. 左下角选 Kit：`Desktop Qt 6.11.2 MSVC2022 64bit`
3. 按 **Ctrl + B** 构建，或点左下角的锤子图标

### 命令行

需要先有 MSVC 的编译环境，再跑 jom/nmake：

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set PATH=E:\QT\6.11.2\msvc2022_64\bin;E:\QT\Tools\QtCreator\bin\jom;%PATH%
cd /d E:\code\DesktopPet\build\Desktop_Qt_6_11_2_MSVC2022_64bit_Debug
jom.exe /NOLOGO PetPal
```

成功的输出长这样：

```
[ 22%] Building CXX object CMakeFiles/PetPal.dir/src/AnimationController.cpp.obj
[ 33%] Building CXX object CMakeFiles/PetPal.dir/PetPal_autogen/mocs_compilation.cpp.obj
[100%] Built target PetPal
```

---

## 怎么运行

### 在 Qt Creator 里

**Ctrl + R**（或左下角绿色三角）。Qt Creator 会自动把 Qt 的 `bin` 加进 `PATH`，所以能直接跑。

### 双击 exe

`PetPal.exe` 在
`build\Desktop_Qt_6_11_2_MSVC2022_64bit_Debug\PetPal.exe`

**直接双击可能会报错"找不到 Qt6Core.dll"** —— 因为它还不知道 Qt 装在哪。
两个办法：

1. 临时把 Qt 的 bin 加进 PATH：
   ```bat
   set PATH=E:\QT\6.11.2\msvc2022_64\bin;%PATH%
   PetPal.exe
   ```
2. 或者直接做一次[部署](#打包成独立的-windows-exe)，之后就能随便拷着跑

### 自检模式

```bat
PetPal.exe --selftest
```

不显示窗口，生成五个文件（都在 exe 同目录）：

| 文件 | 内容 |
|---|---|
| `petpal_selftest.txt` | **这份 exe 的路径和编译时间** + 全部图片的加载情况 + 每个状态的帧序列 + 每一帧的窗口尺寸 + 末尾的尺寸/DPI 小节 + **系统托盘小节（含隐藏/恢复往返测试）** |
| `petpal_selftest_preview.png` | **待机**状态离屏渲染出来的实际画面，用来确认透明和位置 |
| `petpal_selftest_drag.png` | **拖拽**状态离屏渲染出来的实际画面（被拎起来那张） |
| `petpal_selftest_fall.png` | **下落**状态离屏渲染出来的实际画面（张开双臂那张） |
| `petpal_tray_icons.png` | 托盘图标真正用到的 5 档尺寸，各放大 4 倍并排（看 16px 下还认不认得出） |

**桌宠显示不出来时，先跑这个。**

报告的头两行长这样：

```
exe      : E:/code/DesktopPet/build/ninja-Debug/PetPal.exe
编译时间 : 2026-09-22 23:03:26
```

`编译时间` 是**编译器编出这份 exe 的时刻**（来自 `__DATE__` / `__TIME__`），
不是运行时间。它存在的唯一理由，就是回答"我现在跑的到底是哪份 exe"——
跟 `src\PetConfig.h` 等源文件的时间戳一比就清楚了。

`petpal_selftest_drag.png` / `petpal_selftest_fall.png` 是拖动和下落时的图（30 / 31 号素材）。
单独拍它们是因为这两种画面平时没法用截图工具抓到
（本机对分层窗口的屏幕截取还会返回全黑），而它们又最容易出问题：
尺寸不对、透明没处理好、或者压根没进资源。

> 顺带一提：预览图的尺寸会比窗口的 `size()` 大 —— 本机是 275×378 而不是 183×252。
> 那是 **DPI 缩放**（本机 150%，DPR = 1.5），不是 bug。
> 查过 Win32 `GetWindowRect`：窗口物理尺寸和样式（`LAYERED|TOPMOST|TOOLWINDOW`）都正确。
> 报告末尾的「尺寸与 DPI」小节把这两个数都列出来了。

### 下落时序自检

```bat
PetPal.exe --falltrace
```

生成 `petpal_falltrace.txt`。它**不跑事件循环**，只是按 16ms 步进调用
`petFallAdvance()` —— 也就是运行时真正在用的那个函数，不是另写一套公式 ——
把每一步的垂直速度、位移、位置都打出来：

```
   步   时间(ms)   垂直速度(px/s)   本步位移(px)         y
     0        16          35.2          0.56          0.56
     5        96         211.2          3.38         11.83
    20       336         739.2         11.83        130.10
    45       736        1619.2         25.91        608.82
    49       800        1760.0         10.08        700.00

  落地：第 49 步（约 800 ms），落地速度 1760.0 px/s
  判读：垂直速度从 0 单调增大到落地值，单步位移同步变大
        （第 1 步只挪了 0.56 px，落地前一步已经挪 27.6 px）
```

速度从 0 单调增大、单步位移同步变大 —— 这就是"越掉越快"的直接证据，
说明重力真的在起作用，不是匀速平移。

报告最后还会列出三个状态的窗口尺寸并做两条断言：

```
  Idle  待机   183 x 252
  Drag  拖拽   183 x 264
  Fall  下落   183 x 264

  [OK] Drag 与 Fall 尺寸相同 -> 松手换图时窗口不做 resize，画面不抖。
  [OK] Fall 比 Idle 高 12px -> 落地收起这 12px，正好是「脚落回地面」。
```

### 行走时序自检

```bat
PetPal.exe --walktrace
```

生成 `petpal_walktrace.txt`。它**不跑事件循环**，只是手动按 100ms 的节奏推进帧
（`AnimationController::stepFrame()`），把每一步实际要画的那张图打出来。

为什么需要它？因为"起步 + 循环 + 收步"是**时序**逻辑，帧表看不出
"起步那一帧会不会在循环里又冒出来"。这个报告能直接给出答案：

```
【右向走】每 100ms 推进一步，共 20 步：
  1<起步> 2 3 4 5 6 2 3 4 5 6 2 3 4 5 6 2 3 4 5
```

---

## 排错：图片显示不出来

按下面的顺序排查：

**1. 先跑 `PetPal.exe --selftest`，看 `petpal_selftest.txt`**

- **先看头两行**：`exe` 路径 + `编译时间`。
  如果编译时间比你改的源文件还早，说明你跑的是**旧 exe** ——
  这是"新加的图片不显示 / 改了参数没反应"最常见的成因，
  详见上面「怎么编译」里的两个构建目录。
- 如果写的是"成功: 31 张，失败: 0 张"，那图片加载没问题，问题在别处
- 如果列出了缺失的路径，报告里会直接告诉你缺哪个

**2. 检查文件真的在 `resources\pet\` 里，而且文件名大小写完全一致**

```bat
dir E:\code\DesktopPet\resources\pet
```

应该是 31 个 `p01.png ~ p31.png`。

**3. 检查 `resources.qrc` 有没有列出来，`.qrc` 有没有被 CMake 编译**

**这是最容易踩的坑。** 我实测发现：

> CMake 的 AUTORCC 在扫描 `.qrc` 时是**按系统本地编码（中文 Windows 是 GBK）解码**的。
> 只要 `<file>` 标签里出现中文，路径就会被解码坏，编译时报：
> ```
> dependent 'E:\...\17_??png' does not exist.
> ```
> 而且是**编译期直接失败**，不是运行时读不到。

所以本项目的做法是：`<file>` 里写纯英文（`pet/p01.png`），中文名放进 `alias`：

```xml
<file alias="17_吃东西.png">pet/p17.png</file>
```

`alias` 是交给 rcc 处理的，rcc 对 UTF-8 中文名支持得很好，
所以程序里用 `:/pet/17_吃东西.png` 完全没问题。

**如果你自己新增图片**，也请照这个格式写。

**4. 确认 `CMakeLists.txt` 里有开 AUTORCC 和把 qrc 加进目标**

```cmake
set(CMAKE_AUTORCC ON)
set(PROJECT_RESOURCES resources/resources.qrc)
add_executable(PetPal WIN32 ${PROJECT_SOURCES} ${PROJECT_RESOURCES})
```

**5. 改完 `.qrc` 一定要重新编译**

`.qrc` 是**编译期**处理的，图片被打包进 exe。改了 qrc 或换了图片内容，
光"重新运行"是没用的，必须重新构建。Qt Creator 里：**构建 → 重新构建项目**。

**6. 检查资源路径写法**

代码里用的是 `:/pet/xxx.png`，开头那个 `:` 不能少。
少了就变成"相对磁盘路径"，自然找不到。也别写绝对路径 —— 一旦拷贝到别的电脑就废了。

**7. 图片格式问题**

- 必须是 **真 PNG**，不能是把 `.jpg` 改名成 `.png`
- 必须是 **带 alpha 通道的 RGBA**（否则透明区域会变黑或变白）
- 用画图/Photoshop 打开确认背景是透明的（有灰白棋盘格）

---

## 排错：透明背景失效

**症状**：图片之外是一片白色、黑色或灰色方块。

**1. 检查四条窗口标志 + 四条属性是否都在**

对照 `DesktopPet::setupWindow()`：`FramelessWindowHint`、`WindowStaysOnTopHint`、
`Tool`、`NoDropShadowWindowHint`，以及 `WA_TranslucentBackground`、
`WA_NoSystemBackground`、`WA_ShowWithoutActivating`、`WA_OpaquePaintEvent=false`。

**最常见的是漏了 `WA_TranslucentBackground`**：
只要少了它，窗口就变成不透明的，不管图片本身多透明都没用。

**2. 检查 `paintEvent` 第一句是不是 `p.fillRect(rect(), Qt::transparent)`**

少了这句，系统会先刷一层底色，虽然 Qt 通常还会补一次透明填充，
但一旦窗口被移动或缩小，旧像素就会留在原地变成拖影。

**3. 检查有没有设置 `setAutoFillBackground(true)` 或者用样式表设过背景色**

下面这些都**不能有**，它们会画上不透明背景：

```cpp
setAutoFillBackground(true);                        // ❌
setStyleSheet("background: white;");                // ❌
setPalette(/* 不透明的 palette */);                  // ❌
```

**4. 看是"图片本身"的问题还是"窗口"的问题**

跑 `--selftest` 看生成的 `petpal_selftest_preview.png`：

- **预览图里背景是透明的**（用图片查看器看，图片之外的区域应该是棋盘格或透明）
  → 那说明代码没问题，是**平台合成**的事，往下看第 5 条
- **预览图里背景就有白底**
  → 那说明是**图片本身带不透明背景**，回去检查 PNG

怎么自己验证预览图的透明度：用 Photoshop / GIMP / 画图 打开，看背景是不是棋盘格。

**5. 确认系统开启了桌面合成**

`WA_TranslucentBackground` 依赖 Windows 的 DWM 桌面合成。Win8 之后默认是开的。
如果被关了（老系统上有人会关掉"启用透明效果"），透明就会失效。

**6. 如果是显卡驱动相关的花屏**

在 `main.cpp` 里 `QApplication` 构造**之前**加一句：

```cpp
QApplication::setAttribute(Qt::AA_UseSoftwareOpenGL);   // 强制软件渲染，用来判断是不是驱动问题
```

能正常显示就说明是 OpenGL 驱动的问题，可以升级驱动，或者保留这行（代价是费一点 CPU）。

> **注意**：加了 `WS_EX_LAYERED` 的窗口，用 `BitBlt` 截图是**抓不到内容的**
> （Qt6 在 Windows 上用 DirectComposition 合成）。
> 如果你截图看不到桌宠，不代表它没显示 —— 用 Win10 自带的
> `Win + Shift + S` 截屏，或者用 `PrintWindow(hwnd, hdc, 2)` 才能抓到。

---

## 排错：动画闪烁 / 抖动

**症状**：走路时人物左右抖、上下跳，或者帧切换时闪。

**1. 先分清是哪一种"抖"**

| 现象 | 原因 | 怎么改 |
|---|---|---|
| 人物整体左右抖 | 每帧图片宽度不同，被居中放置 | 见下面第 2 条 |
| 人物上下跳 | 上下对齐方式不对 | 见下面第 3 条 |
| 画面闪白 / 闪黑 | 窗口重绘时序问题 | 见下面第 4 条 |
| 移动一顿一顿 | 位置用了 int 累加 | 见下面第 5 条 |
| 动画节奏不均匀 | 定时器类型是 CoarseTimer | 见下面第 6 条 |

**2. 左右抖动**

老素材是**各自紧贴角色裁剪**的，宽度都不一样
（站立 183、老行走帧 162~181、睡姿 358）。
新加入的行走序列（22~29）已经统一裁成同一尺寸 215×259，所以行走这一档本来就不会抖 ——
下面这套处理主要针对那些宽度不一致的状态。
如果每帧都简单地"水平居中"，角色就会因为裁剪宽度的差异左右晃。

本项目的做法是**统一按底边对齐 + 水平居中**。
实测过：这批图确实都是按"角色大致居中"裁的，
所以水平居中就够了（我试过用"躯体重心"对齐，差别只有约 5 像素，反而更复杂）。

如果换了新素材发现还是抖，可以改成按"角色的水平重心"对齐：
在 `AnimationController` 里为每张图算一次 alpha 加权重心，绘制时用它替代 `width()/2`。

**3. 上下跳动**

所有图的角色底边都贴着图片底边，所以**只要统一按下边对齐**，
脚就始终在同一条基准线上。`DesktopPet::frameTopLeft()` 就是这么算的：

```cpp
const int y = height() - pm.height() + PetCfg::WINDOW_V_OFFSET;
```

`WINDOW_V_OFFSET` 是留给你的微调旋钮（默认 0）。整组图整体偏上/偏下时改它。

**4. 画面闪烁**

- 确认 `paintEvent` 第一句是 `p.fillRect(rect(), Qt::transparent)`
- 确认没有 `setAttribute(Qt::WA_PaintOnScreen)`（它会绕开 Qt 的双缓冲，必然闪）
- 确认没有在 `paintEvent` 里做重活（比如加载图片）。
  图片加载只在 `loadAll()` 里做一次，`paintEvent` 只负责 `drawPixmap`

**5. 移动一顿一顿**

```cpp
// ❌ 错：每帧加不到 1 像素，取整后永远是 0，根本走不动
int m_x; m_x += int(speed * dt);
move(m_x, y());

// ✅ 对：用 double 累加，只在使用时取整
QPointF m_pos;
m_pos.setX(m_pos.x() + speed * dt);
move(qRound(m_pos.x()), qRound(m_pos.y()));
```

**6. 节奏不均匀**

`AnimationController` 和心跳定时器都设了 `Qt::PreciseTimer`。
默认的 `Qt::CoarseTimer` 有 5% 误差，加上原来定时器的余数，
帧间隔会在 95~105ms 之间飘，看起来就是"一卡一卡的"。

**7. 想整体调快/调慢**

不要改 `QTimer` 的间隔数字，改 `PetConfig.h` 里这两个值：

```cpp
constexpr int    WALK_FRAME_MS  = 100;   // 行走每帧停留多久（毫秒），100 = 10 FPS
constexpr double WALK_SPEED_PPS = 60.0;  // 走路速度（像素/秒）
```

想让走路更"细腻"就把 `WALK_FRAME_MS` 降到 80（12.5 FPS）；
想让它显得更悠闲就把 `WALK_SPEED_PPS` 降到 40。
**注意两者要一起调**，否则会出现"腿迈得很快但人走得很慢"的溜冰感 ——
大致保持"每帧前进 0.6~0.9 个像素"比较自然。

---

## 打包成独立的 Windows exe

编译出来的 `PetPal.exe` **不能直接拷给别人**：
它依赖 Qt6 的 DLL（`Qt6Core.dll`、`Qt6Gui.dll`、`Qt6Widgets.dll` 等）。

用 Qt 自带的 `windeployqt` 把依赖一起拷过来：

```bat
set PATH=E:\QT\6.11.2\msvc2022_64\bin;%PATH%

:: 建一个干净的发布目录
mkdir E:\code\DesktopPet\dist
copy /Y E:\code\DesktopPet\build\Desktop_Qt_6_11_2_MSVC2022_64bit_Debug\PetPal.exe E:\code\DesktopPet\dist\

:: 把 Qt 依赖拷进去（会自动带上需要的插件）
windeployqt.exe --release --no-translations --no-compiler-runtime E:\code\DesktopPet\dist\PetPal.exe
```

`dist` 目录会长成这样：

```
dist\
├── PetPal.exe
├── Qt6Core.dll
├── Qt6Gui.dll
├── Qt6Widgets.dll
├── platforms\qwindows.dll          ← 少这个目录会报"could not find or load the Qt platform plugin"
├── imageformats\qpng.dll           ← 少这个 PNG 就显示不出来
└── styles\...
```

然后 **把整个 `dist` 文件夹打包成 zip** 发给别人，对方解压后双击 `PetPal.exe` 就能跑。

### 几个注意点

| 事项 | 说明 |
|---|---|
| **图片不用额外拷** | 全部 PNG 已经被编译进 `PetPal.exe` 内部了（这就是用 qrc 的好处） |
| 要用 **Release** 版发布 | Debug 版依赖 `Qt6Cored.dll` 等调试 DLL，体积大、速度慢。Qt Creator 左下角把构建配置切成 `Release` 再构建 |
| `platforms\qwindows.dll` 必须有 | 这是最经典的部署错误 |
| `imageformats\qpng.dll` 建议保留 | 本项目的图片是打包进 exe 的，理论上不依赖它，但保留更保险 |
| 想做成单文件 | 可以用 `enigma virtual box` 这类工具把 `dist` 打成单个 exe，但没必要 —— 打包成 zip 已经够了 |
| 加图标 | 把 `.ico` 放到 `resources/`，在 `CMakeLists.txt` 里加一行：<br>`set_target_properties(PetPal PROPERTIES WIN32_EXECUTABLE TRUE)` 并在 `add_executable` 前写一个 `app.rc`，内容为 `IDI_ICON1 ICON DISCARDABLE "app.ico"` |

---

## 第二阶段：接入本地 Qwen

接口已经留好了。链路是这样的：

```
右键菜单「和我聊天」
   │
   ▼
DesktopPet::chatRequested()          ← 信号，在 DesktopPet.h 里声明
   │
   ▼
main.cpp 里 connect 的那段 lambda    ← 现在只打印一行日志，以后换成调模型
   │
   ▼
拿到模型的回答
   │
   ├─ 回答是好事  → DesktopPet 播 Happy
   ├─ 回答是坏事  → 播 Sad
   ├─ 回答很意外  → 播 Surprise
   └─ 回答冒犯了  → 播 Angry（12 → 19 → 10 → 08）
```

现在 `main.cpp` 里的样子：

```cpp
QObject::connect(&pet, &DesktopPet::chatRequested, &pet, []()
{
    qInfo() << "[PetPal] 收到聊天请求 —— 这里就是接入本地 Qwen 的位置。";
});
```

要接本地 Qwen 时，把这个 lambda 换成调用（比如通过 `QNetworkAccessManager` 请求
llama.cpp 的 `llama-server`，或者 Ollama 的 `/api/chat`），拿到回答后再让桌宠播对应表情。

> 小提示：`DesktopPet` 目前没有把"想让桌宠播某个动作"暴露出去。
> 接入时给 `DesktopPet` 加一个 public 槽即可，例如：
> ```cpp
> public slots:
>     void playEmotion(PetState s) { runChain(QVector<PetState>{ s }); }
>     void playAngry()            { runChain(QVector<PetState>{ PetState::Angry, PetState::AngryFoot, PetState::Sad }); }
> ```

---

## 快速调参速查

全都在 `src/PetConfig.h`：

```cpp
// 帧率 / 时长
WALK_FRAME_MS      = 100    // 行走每帧多少毫秒（100 = 10 FPS，建议 80~125）
TURN_FRAME_MS      = 220    // 转身每帧
ACTION_SHORT_MS    = 900    // 开心/委屈/惊讶/生气 显示多久
ACTION_MID_MS      = 1100   // 挥手/吃东西/喝水
ACTION_LONG_MS     = 1500   // 思考

// 速度
WALK_SPEED_PPS     = 60.0   // 走路像素/秒
FLY_SPEED_PPS      = 170.0  // 飞行像素/秒
FLY_BOB_PX         = 16     // 飞行上下浮动幅度
FLY_BOB_HZ         = 0.7    // 上下浮动频率

// 鼠标
DRAG_THRESHOLD_PX  = 4      // 移动超过几像素才算"拖动"
HIT_TEST_ALPHA     = true   // 是否只在角色实体像素上响应鼠标
// 拖动期间显示 30 号"被拎起来"的图，松手后转 31 号自由落体，落地回站立。
// 悬空量 12px 烘在 p30.png / p31.png 里（画布都是 183x264，角色底边距画布底边 12px），
// 代码里没有对应参数 —— 想改要重新处理图片。

// 重力（松手后的自由落体）
GRAVITY_PPS2       = 2200.0 // 重力加速度 px/s²。1200 飘 / 2200 默认 / 4000 砸
FALL_MAX_PPS       = 1800.0 // 终端速度上限，防止从超大屏幕顶部掉下来太快
FALL_LAND_EPS      = 0.5    // 落地判定容差（像素），不留会悬停 1px 下不来
// 挑手感：打开 docs/fall_preview.html 拖滑块试，挑好把数字抄过来

// 自动行为
IDLE_MIN_MS/MAX_MS = 5000/15000   // 两次动作之间站多久
WALK_MIN_MS/MAX_MS = 3000/8000    // 一次走多久
DAZE_MIN_MS/MAX_MS = 4000/9000    // 发呆多久
SIT_MIN_MS/MAX_MS  = 6000/12000   // 坐多久
PROB_WALK/DAZE/SIT = 45/20/15     // 各动作概率（剩下的概率 = 继续站着）

// 行走序列（改这几个就能换帧序，见「想调整行走序列」）
WALK_START_IMGS    = { 22 }              // 起步，只播一次      (1)
WALK_LOOP_IMGS     = { 23,24,25,26,27 }  // 循环体，反复播      (2)~(6)
WALK_STOP_IMGS     = { 28, 29 }          // 收步，停下前播一次  (7)(8)
WALK_STOP_MS       = 200    // 收步占多久（= 收步帧数 × WALK_FRAME_MS），由它决定何时开始收步

// 睡觉
SLEEP_AFTER_SEC      = 300  // 多少秒没互动后开始犯困（默认 5 分钟）
DAZE_BEFORE_SLEEP_MS = 2500 // 先发呆这么久再睡着

// 屏幕
EDGE_MARGIN_PX     = 0      // 离屏幕边缘留多少间距
WINDOW_V_OFFSET    = 0      // 整体上下微调（负数 = 上移）
```

改完重新编译即可，不需要动别的代码。
