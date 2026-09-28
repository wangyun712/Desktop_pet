# PetPal —— Qt6 桌面宠物（洛天依）

C++ / Qt 6 Widgets / CMake / miniaudio。Windows 桌宠 + 主面板（好感度、每日图片、聊天、音乐播放器、设置），离线功能为主，联网音乐搜索为可选增强。

---

## 功能一览

| 模块 | 内容 |
|---|---|
| 桌宠本体 | 31 张素材、行走三段式动画、拖拽/下落/睡觉、点击反应、头顶台词气泡（PetSay，13 类事件）、连点 10 次的生气彩蛋 |
| 好感度 | 摸摸 +1（10 次/天）、喂食 +5（3 次/天）、聊天 +3（5 次/天），自然衰减，Lv.1~10 × 5 个阶段称号 |
| 每日图片 | 从 `resources/daily_image/` 按日期种子固定抽一张，可开原图 |
| 聊天 | 预设台词引擎（`ChatScript.h`，35 类意图、164 句），离线秒回，永不 OOC；支持歌词接唱 |
| 播放器 | 本地曲库 + 联网搜索（网易云/B站）、流式播放、下载/收藏、逐句歌词 |
| 桌面歌词 | 置顶条带、固定宽度、工具条（播放控制/锁定/关闭）、锁定后鼠标穿透 |
| 外观 | 6 套主题预设、背景图、界面字号、桌宠大小缩放，全部即时生效并记忆 |

---

## 快速开始

```bat
:: 构建（Ninja/Debug；jom / release / clean 见 build.bat 参数）
run.bat

:: 运行 + 自检（产物在 build\Desktop_Qt_6_11_2_MSVC2022_64bit_Debug\）
PetPal.exe --selftest
```

环境：Qt 6（Core/Gui/Widgets/Network，本机 6.11 MSVC2022）+ CMake 3.24+。Qt Creator 打开 `CMakeLists.txt` 选 Kit 即可；命令行构建细节见 `build.bat` 注释。

---

## 项目结构

```
src/
├── main.cpp                 入口 + --selftest / --walktrace / --falltrace
├── PetConfig.h              ★ 全局配置：状态机参数 + 好感度数值 + 帧序列表
├── AnimationController/     动画帧序列（.h/.cpp 成对，下同）
├── PetBehaviorController/   自动行为（闲逛/发呆/睡觉的调度）
├── DesktopPet/              桌宠主控制器（透明窗口本体）
├── AffectionSystem/         好感度数据（唯一持有者，纯函数时间逻辑）
├── AffectionPage/           面板「好感度」页
├── DailyImagePage/          面板「每日图片」页
├── ChatPage/ + ChatScript.h 「聊天」页 + 预设台词引擎（纯数据）
├── SongLibrary.h            歌词库（resources/songs/*.txt，运行时读盘）
├── MusicLibrary/            本地曲库（后台扫描 + 模糊搜索）
├── TrackMeta.h              音频标签解析（MP3/FLAC/WAV/M4A）
├── TrackListModel/          播放列表模型（本地行 + 联网行两段式）
├── AudioPlayer/             播放后端（miniaudio；本地文件 + HTTP 流式）
├── PlayerPage/              「播放器」页（列表/歌词/联网搜索/收藏下载）
├── OnlineMusic.h/.cpp       联网引擎：网易云 + B站（WBI 签名）+ 下载器
├── MfDecode.h/.cpp          Media Foundation 解码（B站 AAC → WAV）
├── DesktopLyrics/           桌面歌词（固定宽度条带 + 工具条 + 锁定）
├── PetBubble.h/.cpp         桌宠头顶台词气泡
├── PetFx.h/.cpp             粒子特效层（爱心 / 睡觉 Zzz）
├── PetSay.h                 桌宠独白台词池（13 类事件，纯数据）
├── MainPanel/               主面板外壳（无边框圆角窗 + 导航 + 页面栈）
├── SettingsPage/            「设置」页（重置/字号/主题/背景图/大小/开关）
├── UiFont/ + UiTheme/       全局外观首选项（字号档位 / 6 套主题调色板）
resources/
├── pet/                     31 张桌宠 PNG（p01~p31）
├── songs/                   ★ 歌词库（纯文本 .txt，见下）★
└── daily_image/             每日图片插画池（随手增删）
third_party/miniaudio.h      音频后端（单头文件）
```

运行时用户数据在 `%APPDATA%/PetPal/`（好感度、收藏/下载索引、外观首选项）；
联网临时缓存在 `%TEMP%/PetPalMusic/`（退出自动清除）。

---

## 架构要点

**桌宠本体**：60Hz 心跳推位置 + 独立帧定时器推动画，互不干扰。状态机（Idle/Walk/Drag/Fall/Sleep/Eat…）见 `PetConfig.h`；所有窗口移动收敛在 `applyWindowPos()` 一处。桌面台词、粒子特效是独立置顶小窗，绝不抢焦点。

**音频管线**：`AudioPlayer` 用 miniaudio（MP3/FLAC/WAV，WASAPI 输出），200ms 轮询报进度，界面永不碰音频线程。本地文件走 `load(path)`；网易云直链走 `loadOnline(url)`——`StreamFeed` 后台拉 HTTP 流进带锁缓冲，解码器通过自定义 read/seek 回调边下边播（拖到未缓冲区自动 Range 续传）。

**面板**：懒加载（首次「打开面板」才创建），五个页面 + 互斥的收藏/下载视图。所有外观设置（字号/主题/曲源/储存盘）存 `%APPDATA%/PetPal/ui.ini`，即时生效。

**主线程纪律**：音频回调内不碰 Qt、不加锁、不分配；联网回调全部异步回主线程。

---

## 在线音乐（网易云 / B站）

搜索框输入 → 本地结果先行（标"本地"），联网结果追加在后（标"联网·网易 / 联网·B站"）。曲源选择行可切换**网易云 / B站**：

| 平台 | 能拿到什么 | 播放方式 |
|---|---|---|
| 网易云 | 搜索（VIP/付费已过滤）、MP3 直链、LRC 歌词 | **流式播放**（不落盘、1~2 秒出声、可拖进度条） |
| B站 | 综合排序前 10 的视频条目 | AAC/M4S → **Media Foundation 转成 WAV** 存进储存盘 → 播放 |

网易云 VIP（fee=1）/ 需购买（fee=4）的歌**不进列表**；网易云搜不到 → 自动转 B 站。

**存储规则**：

| 动作 | 去向 |
|---|---|
| 双击联网行 | 临时播放：网易云纯流式不落盘；B站转出 WAV 存进储存盘并登记「我的下载」 |
| 右键收藏 | 完整下载到储存盘 + 登记「我的收藏」 |
| 右键仅下载 | 完整下载到储存盘 + 登记「我的下载」 |
| 退出程序 | 临时目录整删；储存盘和两个索引不受影响 |

「我的下载 / 我的收藏」两个视图按索引列出文件还在的歌；下载视图右键可**删除**（文件+记录一起清）。

> ★ 接口为社区逆向的非官方 API（网易云旧版 api / B站 WBI 签名），平台风控或改版会导致联网功能失效——失败原因会显示在列表行上（联网失败·超时 / 联网失败·找不到服务器…），本地功能完全不受影响。仅供个人本地使用。★

---

## 歌词库格式（resources/songs/*.txt）

纯文本、直接读磁盘（不进 qrc、不改代码），丢一个 `.txt` 进目录就生效。完整规范见 `src/SongLibrary.h` 开头，速记：

```text
# 注释行（# 开头被忽略）
title: 达拉崩吧                ← 歌名
artist: 洛天依                 ← 歌手
tempo: fast                    ← fast(快歌) / slow(慢歌)，二选一

seg:                           ← 新起一段（说"快歌/慢歌"时随机抽一段唱）
第一句歌词
第二句歌词
seg:
下一段的第一句
……
```

- 整段从歌词站复制直接粘即可；`作词：/作曲：` 这类行自动跳过
- 歌词是**词作的版权**：仓库里各文件先放了占位句，粘贴真歌词替换即可
- 联网搜到的网易云歌下载时自动带 `.lrc`，无需手工整理

## 聊天台词（src/ChatScript.h）

台词是编译进程序的纯数据表：每条意图 = 触发词 + 权重 + 回复池。加新话题 = 加一条意图（一个结构体）；加新说法 = 加一行触发词。改完重编译生效。桌宠本体的独白（被点/拎/落地/整点/节日/梦话…）在 `src/PetSay.h`，同一种写法。

---

## 自检模式

```bat
PetPal.exe --selftest
```

在 exe 同目录生成 `petpal_selftest.txt`（资源加载 / 帧序列 / 托盘往返 / 好感度 / 曲库 / 聊天匹配样例 / 播放器解析 / 联网模块 / 字号 / 主题各一节）和若干离屏渲染 PNG（桌宠三态、主面板各页、聊天页、播放器页、设置页、气泡、特效层）。**任何显示问题先看这批图。**

---

## 调参速查

**桌宠本体**（`src/PetConfig.h`）：

| 参数 | 默认 | 说明 |
|---|---|---|
| TICK_MS | 16 | 心跳周期 |
| WALK_SPEED_PPS | 60 | 走路速度 |
| DRAG_THRESHOLD_PX | 4 | 拖动判定阈值 |
| GRAVITY_PPS2 / FALL_MAX_PPS | 2200 / 1800 | 下落重力 / 终端速度 |
| SLEEP_AFTER_SEC | 300 | 无互动几分钟开始犯困 |
| AFF_PET_PER_DAY / AFF_FEED_PER_DAY / AFF_CHAT_PER_DAY | 10 / 3 / 5 | 每日互动次数上限 |
| AFF_PET_POINT / AFF_FEED_POINT / AFF_CHAT_POINT | 1 / 5 / 3 | 单次加分 |
| AFF_DECAY_PER_HOUR | 0.5 | 自然衰减 |

**其余模块各归各家**：

| 位置 | 管什么 |
|---|---|
| `src/UiTheme.cpp` 的 `presets()` | 6 套主题的 26 个颜色角色 |
| `src/PetSay.h` | 桌宠独白台词池（13 类事件） |
| `src/ChatScript.h` | 聊天意图与台词 |
| `src/OnlineMusic.cpp` | 联网接口地址、超时分档（小请求 15s / 下载 120s） |
| `src/PetBubble.cpp`、`src/PetFx.cpp` 顶部常量 | 气泡与粒子的尺寸/节奏 |
| `src/DesktopLyrics.cpp` 顶部常量 | 桌面歌词条带宽度、工具条布局 |

> 维护规则：新增 `.cpp` 记得同时加进 `CMakeLists.txt` 的 `PROJECT_SOURCES`、`PETPAL_HEADERS` 和 `set_property(SOURCE ...)` 三处；每个源文件开头的大注释块就是它的说明书。

---

## 已知限制

- 联网接口为社区逆向的非官方 API，平台改版会导致联网功能失效（本地功能不受影响）；仅供个人本地使用
- B 站音频是 AAC，miniaudio 不支持——依赖 Windows Media Foundation 转成 WAV，个别资源可能解码失败（行上会提示）
- 网易云 VIP 歌曲不提供播放直链，搜索结果已直接过滤（对未登录用户本就不可播）
