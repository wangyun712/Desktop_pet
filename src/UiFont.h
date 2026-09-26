#pragma once
// =============================================================================
//  UiFont —— 全局界面字号档位（设置页里那个"界面字号"）
//
//  ★ 它是个**首选项**：用户调一次，之后每次启动都按这个大小 ★
//      · 调：滑条 valueChanged → setScalePercent()（立刻生效，实时预览）
//                            + saveScalePercent()（**每次都落盘**，见下）
//      · 启动：main.cpp 在建任何窗口之前调一次 applyToApplication()，
//              各页面构造时套样式表又会经过 styleSheet()，两处都把档位读出来
//      · 存哪：%APPDATA%/PetPal/ui.ini 的 fontScale（= savePath()，自检报告里也印一份）
//    ★ "改档位"有**两个**动作，缺一个就是"这次好用、重启打回原样"：
//      只 setScalePercent 不 save ⇒ 没落盘；只 save 不 apply ⇒ 屏幕上不变。
//
//  ★ 为什么要有这么个东西 ★
//    面板里所有字号都写在各自页的样式表里（`font-size: 12px` 这种），
//    一共几十处、散在 6 个文件里。要在设置里让用户整体调大调小，
//    逐个改成变量既啰嗦又容易漏。
//    所以这里不去动那些样式表本身，而是在**套上去之前**统一改写一遍：
//    页面照旧写 `font-size: 12px`，由 styleSheet() 按档位改成 `font-size: 15px`。
//    一处实现，六个页面一起生效，以后加新页也自动跟着走。
//
//  ★ 两件事要一起做，只做一件会"半大不小"★
//    ① 改写样式表里写死的 px 字号（styleSheet）
//    ② 把 QApplication 的默认字体也按同样比例放大（applyToApplication）
//    —— 没写 font-size 的控件走的是默认字体，只改①的话它们纹丝不动。
//
//  ★ 基准字体只取一次 ★
//    每次 setPointSizeF(当前字号 × 比例) 会乘法叠加，拖几下就撑爆了。
//    所以首次调用时把 Qt 给的原始字体存下来当基准，之后都从它算。
// =============================================================================

#include <QString>

namespace UiFont {

inline constexpr int MIN_PERCENT     = 80;
inline constexpr int MAX_PERCENT     = 150;
inline constexpr int STEP_PERCENT    = 5;

// ★ 别把 BASE_PERCENT 和 DEFAULT_PERCENT 合成一个 ★（2026-09-24 踩过）
//   BASE_PERCENT    = "基准档位"：QSS 里那些 font-size 的字面值就是这个档位，
//                     换算时按 1:1 原样返回（省一次正则）。它是**换算的零点**，
//                     必须是 100，改了它 100% 这一档就再也对不上。
//   DEFAULT_PERCENT = "默认值"：存档里还没有值时用哪个档位，是产品选择。
//   合成一个的后果：一旦把默认值调成 120，120% 这一档就走进"等于基准 → 原样返回"
//   的分支 —— QSS 里的字号**一点没放大**，只有 QApplication 默认字体被放大 1.2 倍。
//   屏幕上就变成"有的字变大了、有的字没变"，看着像功能坏了。
inline constexpr int BASE_PERCENT    = 100;
inline constexpr int DEFAULT_PERCENT = 100;

// 当前档位（百分比）。首次调用时从存档读，之后走内存里的值。
int scalePercent();

// 只改内存 + 立刻应用到 QApplication，**不落盘**。
void setScalePercent(int percent);

// 只记下档位，**不动界面**（不碰 QApplication 默认字体）。
// ★ 只给"拖动滑条时的高频预览"用 ★
//   真正的重排很贵：applyToApplication() 会让 Qt 给所有窗口发一次字体变更事件、
//   整棵控件树重新 polish + 重新布局，各页面还要再重套一次样式表。拖动时
//   valueChanged 是每个像素来一次，每次都做这个界面就发涩。
//   所以拖动期间用这个把档位记下来（并落盘），由调用方按节流自己决定什么时候
//   调 applyToApplication() + 重套样式表 —— 见 SettingsPage::onFontScaleChanged()。
//   ★ 用它就必须自己负责"最后一定会 apply 一次" ★，否则就成了
//     "值存进去了、屏幕上却没变"。不需要节流的地方一律用 setScalePercent()。
void setScalePercentQuiet(int percent);

// 落盘（存到 savePath() 那个文件）。
// ★ 调用方在**每一次**档位变化之后都要调它 ★，别只挂在"松开滑条"上 ——
//   滚轮和方向键改的是同一个值，但那两条路上没有"松手"这个动作（见 SettingsPage）。
void saveScalePercent();

// 存档文件路径（%APPDATA%/PetPal/ui.ini）。给自检报告和排查用。
QString savePath();

// 给 --selftest 用：把档位、存档路径、"这次到底读到了什么"写成一小段报告。
// 首选项这类东西最怕"改完以为存了" —— 所以印实际读到的东西，而不是只说"已加载"。
QString describe();

// 把"按 100% 设计的字号"换算成当前档位下的像素值。
// 给自绘代码和手拼的样式表用；整段 QSS 走下面的 styleSheet()。
int px(int basePx);

// 把一段 QSS 里所有的 `font-size: <数字>px` 按档位改写。
// 只认 px —— 项目里字号一律写 px，写 pt 的没有，所以不处理。
QString styleSheet(const QString& qss);

// 把 QApplication 的默认字体设为"基准字体 × 档位"。启动时和档位变化时都要调。
void applyToApplication();

} // namespace UiFont
