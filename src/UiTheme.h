#pragma once
// =============================================================================
//  UiTheme —— 面板主题（预设主题色 / 背景图片 / 图片不透明度）
//
//  ★ 它是个**首选项**，和 UiFont（界面字号）一个规矩 ★
//      · 调：设置页「主题外观」卡片 → setPreset()/setBgImagePath()/setBgOpacity()
//            （每个都"记内存 + 立刻落盘"，中途关程序也不丢）
//      · 启动：各页面套样式表时会经过 styleSheet()，它按当前主题换色；
//              面板底色/背景图由 MainPanel::paintEvent 直接问 color()/bgImage
//      · 存哪：和字号同一个文件（%APPDATA%/PetPal/ui.ini，见 UiFont.h），
//              键前缀 theme/ —— 外观类的首选项都搁一个 ini 里
//
//  ★ 换色是"字面量映射"，不是模板 ★
//    项目里 6 个页面 + 面板外壳的 QSS 都是手写的死色值（#7F77DD 这种）。
//    要让主题一换全套跟着换，不必把每处色值都改成变量 —— 这里维护一张
//    「默认主题字面量 → 颜色角色」的映射表（.cpp 里的 kLiteralMap），页面照旧
//    按默认主题写死色值，UiTheme::styleSheet() 在套上去**之前**把字面量换成
//    当前主题的对应色。和 UiFont 改写 font-size 是同一个思路：一处实现，全部生效。
//    后果（都是刻意的）：
//      · 新页面只要用默认主题的色值写 QSS，就自动支持换肤，不用接任何东西；
//      · 表里没有的色值（危险红、成功绿这类语义色）不会被换 —— 该红的还是红的。
//
//  ★ 谁来让界面真的变 ★
//    UiTheme 只管"记 + 存 + 提供颜色"，**不碰任何控件**。
//    换主题色后要重套各页样式表 —— 设置页发 themeChanged()，DesktopPet 接到后
//    逐页调 applyUiScale()（和字号档位变更同一条路）；
//    背景图/不透明度只影响面板自绘 —— 发 panelBackgroundChanged()，
//    DesktopPet 只要 m_panel->update() 重画一次就行，各页的样式表没变不用动。
// =============================================================================

#include <QColor>
#include <QString>

namespace UiTheme {

// 颜色角色。一套主题 = 每个角色一个值（见 .cpp 里的 6 套预设）。
// 角色 listing 的顺序只在 Preset 构造里用到，随意增删，但必须和那里对齐。
enum Role
{
    Accent,         // 主强调：导航选中条、进度条、滑条、实心按钮底
    AccentDeep,     // 深强调：hover 文字、按下底色、歌词/播放中标题
    AccentHover,    // 强调底色的 hover
    AccentPressed,  // 强调底色的按下
    AccentSoft,     // 浅强调描边（幽灵按钮的边）
    AccentSoftBg,   // 幽灵按钮 hover 底
    AccentSoftBg2,  // 幽灵按钮按下底
    AccentMid,      // 次强调（音量条这类"第二重"的强调）
    AccentMid2,     // 次强调的滑块
    AccentText,     // 强调色上/旁的深色文字（导航选中项、滑条百分比）
    ChatAccent,     // 聊天页强调（发送键、天依气泡描边、输入框聚焦）
    ChatAccentHover,// 聊天发送键 hover
    OnAccent,       // 聊天发送键上的文字（深色，压住亮底）
    ChatAccentTint, // 天依气泡的浅底
    PanelBg,        // 面板底色（MainPanel::paintEvent 画的那一层）
    NavBg,          // 导航栏/卡片底（带少量透明，让背景图能隐约透出来）
    SurfaceAlt,     // 次级表面（聊天消息区）
    UserBubble,     // 聊天里用户气泡的底
    HoverBg,        // 中性 hover 底（窗控按钮、播放器小按钮）
    Border,         // 分隔线 / 描边 / 滑条槽
    Disabled,       // 禁用底 / 面板外圈描边
    TextStrong,     // 主文字
    TextMid,        // 次文字（列表项、等级行）
    TextSub,        // 弱文字（说明、卡片标题行上的灰字）
    TextFaint,      // 更弱（时间戳、未唱歌词）
    TextDisabled,   // 禁用文字 / 占位灰
    RoleCount
};

struct Preset
{
    const char* id;                 // 存进 ini 的标识（换名不换 id，存档就不失效）
    const char* name;               // 设置页里显示的中文名
    QColor      colors[RoleCount];
};

int          presetCount();
const Preset& preset(int index);    // 越界时回落到 0（默认主题）
int          currentIndex();        // ini 里存的主题；没存过/存坏了 → 0
QString      currentName();         // 当前主题的中文名（设置页展示用）
QColor       color(Role role);      // 当前主题下某角色的颜色

// 记内存 + 立刻落盘。**都不会碰控件** —— 界面刷新走上面注释里那条信号路。
void setPreset(int index);

// 背景图：绝对路径。空串 = 清除（回到纯主题色底）。
// 存的文件后来被删/被移走时：bgImagePath() 原样返回路径，但画图的那边
// 加载不出来就自然回落到纯色底 —— 不弹错、不崩，设置页会提示"文件不在了"。
QString bgImagePath();
void    setBgImagePath(const QString& absolutePath);

// 背景图不透明度 0~100（0 = 看不见图，100 = 完全不透明）。
// 没设背景图时这个值没有意义，但照样存 —— 先调透明度再导图不丢。
int     bgOpacity();
void    setBgOpacity(int percent);
qreal   bgOpacityF();               // 0.0~1.0，给 QPainter::setOpacity 用

// 把一段 QSS 里"默认主题的字面量色值"换成当前主题的对应色（见文件头说明）。
// 表里没有的色值原样保留。幂等：换过色的 QSS 再过一遍不会再变。
QString styleSheet(const QString& qss);

// 存档文件路径（和 UiFont 同一个 ini）。给自检报告和排查用。
QString iniPath();

// 给 --selftest 用：当前主题、背景图、不透明度、实际读到的存档内容。
QString describe();

} // namespace UiTheme
