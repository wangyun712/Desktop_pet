#pragma once
// =============================================================================
//  PetConfig.h —— 桌宠的「配置中心」
//
//  这个文件里只有枚举、常量和数据表，没有任何逻辑。
//  想改动作快慢、走路速度、随机行为的时间范围、行走用哪几张图，
//  全部在这里改，不需要动别的代码。
// =============================================================================

#include <QString>
#include <QStringList>
#include <QVector>
#include <QHash>
#include <QtGlobal>   // qMin / qMax

// -----------------------------------------------------------------------------
//  一、状态枚举
//      桌宠任何时刻都只处于一个状态。所有状态之间的跳转都写在 DesktopPet 里。
// -----------------------------------------------------------------------------
enum class PetState //强类型枚举，限定枚举变量的作用域
{
    Idle,        // 待机：显示 08 站立。一切动作结束后都回到这里
    WalkRight,   // 向右走：起步帧播一次 -> 循环体反复播，同时窗口 x 增大
    WalkLeft,    // 向左走：同一组行走帧水平翻转，同时窗口 x 减小
    WalkStopRight,  // 收步(右)：停下前播一次 28 -> 29（一次性），播完回待机
    WalkStopLeft,   // 收步(左)：同上，水平翻转
    TurnLeft,    // 向左转身：04 背面 -> 03 朝左侧身（一次性，播完继续走）
    TurnRight,   // 向右转身：04 背面(镜像) -> 05 朝右侧身
    Happy,       // 09 开心
    Sad,         // 10 委屈
    Surprise,    // 11 惊讶
    Angry,       // 12 生气
    AngryFoot,   // 19 生气跺脚
    Sleep,       // 13 打瞌睡/睡觉（循环，直到被叫醒）
    Sit,         // 14 坐下
    Wave,        // 15 挥手
    Think,       // 16 思考
    Eat,         // 17 吃东西
    Drink,       // 18 喝水
    Daze,        // 20 发呆
    Fly,         // 21 飞行
    Drag,        // 30 被拖拽：鼠标左键按住拖动时显示（"被拎起来"的样子）
    Fall         // 31 下落：松开鼠标后受重力自由落体，落到地面切回待机
};

// 状态的中文名，只用于日志和自检报告，方便排查问题
inline const char* petStateName(PetState s)
{
    switch (s)
    {
    case PetState::Idle:      return "Idle 待机";
    case PetState::WalkRight: return "WalkRight 向右走";
    case PetState::WalkLeft:  return "WalkLeft 向左走";
    case PetState::WalkStopRight: return "WalkStopRight 收步(右)";
    case PetState::WalkStopLeft:  return "WalkStopLeft 收步(左)";
    case PetState::TurnLeft:  return "TurnLeft 向左转身";
    case PetState::TurnRight: return "TurnRight 向右转身";
    case PetState::Happy:     return "Happy 开心";
    case PetState::Sad:       return "Sad 委屈";
    case PetState::Surprise:  return "Surprise 惊讶";
    case PetState::Angry:     return "Angry 生气";
    case PetState::AngryFoot: return "AngryFoot 生气跺脚";
    case PetState::Sleep:     return "Sleep 睡觉";
    case PetState::Sit:       return "Sit 坐下";
    case PetState::Wave:      return "Wave 挥手";
    case PetState::Think:     return "Think 思考";
    case PetState::Eat:       return "Eat 吃东西";
    case PetState::Drink:     return "Drink 喝水";
    case PetState::Daze:      return "Daze 发呆";
    case PetState::Fly:       return "Fly 飞行";
    case PetState::Drag:      return "Drag 拖拽";
    case PetState::Fall:      return "Fall 下落";
    }
    return "未知状态";
}

// -----------------------------------------------------------------------------
//  二、全部可调参数
// -----------------------------------------------------------------------------
namespace PetCfg
{
// ---------- 动画帧率 / 时长（单位：毫秒）----------
// 行走每帧停留时间。100ms = 10 FPS。建议范围 80~125（也就是 8~12 FPS）
constexpr int WALK_FRAME_MS      = 100;
// 转身每帧停留时间（转身只有 2 帧）
constexpr int TURN_FRAME_MS      = 220;
// 表情类一次性动作（开心/委屈/惊讶/生气）的显示时长
constexpr int ACTION_SHORT_MS    = 900;
// 挥手 / 吃东西 / 喝水
constexpr int ACTION_MID_MS      = 1100;
// 思考
constexpr int ACTION_LONG_MS     = 1500;

// ---------- 移动速度 ----------
constexpr double WALK_SPEED_PPS  = 60.0;   // 走路速度：像素 / 秒
constexpr double FLY_SPEED_PPS   = 170.0;  // 飞行速度：像素 / 秒
constexpr int    FLY_BOB_PX      = 16;     // 飞行时上下浮动幅度（像素）
constexpr double FLY_BOB_HZ      = 0.7;    // 飞行浮动频率（次 / 秒）

// ---------- 重力：松开鼠标之后的自由落体 ----------
// 松手那一刻垂直速度从 0 开始，之后每帧 v += g*dt、y += v*dt —— 就是中学物理的自由落体。
// 手感参考（屏幕高按 1000px 估算，实际从拖到的高度开始掉）：
//   g = 1200  -> 掉到底约 1.3 秒，轻飘飘的，像片羽毛
//   g = 2200  -> 掉到底约 0.95 秒，目前用的值，像"啪嗒"掉下来
//   g = 4000  -> 掉到底约 0.7 秒，砸得很实在
constexpr double GRAVITY_PPS2    = 2200.0;
// 终端速度上限（像素/秒）：不管掉多久，垂直速度都不会超过它。
// 主要是兜底 —— 4K 竖屏从最顶上掉下来时，速度会大到像一道残影。
constexpr double FALL_MAX_PPS    = 1800.0;
// 落地判定容差（像素）：离地面这么近就算已经落地。
// 不留这个容差的话，最后一小段可能因为取整误差永远差零点几像素，悬在半空下不来。
constexpr double FALL_LAND_EPS   = 0.5;

// ---------- 主心跳 ----------
constexpr int    TICK_MS         = 16;     // 心跳间隔，16ms ≈ 60Hz
constexpr double MAX_TICK_DT_S   = 0.25;   // 一帧间隔超过 0.25 秒就当"休眠假时间"丢弃，防止瞬移

// ---------- 鼠标 ----------
constexpr int  DRAG_THRESHOLD_PX = 4;      // 鼠标移动超过这么多像素才算"拖动"，否则算"点击"
constexpr bool HIT_TEST_ALPHA    = true;   // true=只有点在角色的实体像素上才有反应（透明处不响应）

// ---------- 自动行为的随机时间区间 ----------
constexpr int IDLE_MIN_MS        = 5000;   // 两次动作之间：最短站立时间
constexpr int IDLE_MAX_MS        = 15000;  // 最长站立时间
constexpr int WALK_MIN_MS        = 3000;   // 一次走路最短持续
constexpr int WALK_MAX_MS        = 8000;   // 一次走路最长持续
constexpr int DAZE_MIN_MS        = 4000;   // 发呆时长
constexpr int DAZE_MAX_MS        = 9000;
constexpr int SIT_MIN_MS         = 6000;   // 坐下时长
constexpr int SIT_MAX_MS         = 12000;
constexpr int GAP_MIN_MS         = 600;    // 动作刚结束时额外多站一会儿
constexpr int GAP_MAX_MS         = 2500;

// ---------- 自动行为的概率（0~100，三者之和不要超过 100，剩下的是"继续站着"）----------
constexpr int PROB_WALK          = 45;
constexpr int PROB_DAZE          = 20;
constexpr int PROB_SIT           = 15;

// ---------- 睡觉 ----------
constexpr int SLEEP_AFTER_SEC      = 300;   // 多少秒没有互动后开始犯困（默认 5 分钟）
constexpr int DAZE_BEFORE_SLEEP_MS = 2500;  // 先发呆这么久，再真正睡着

// ---------- 窗口 / 屏幕边界 ----------
constexpr int EDGE_MARGIN_PX     = 0;      // 桌宠离屏幕可用区边缘保留的间距
constexpr int WINDOW_V_OFFSET    = 0;      // 整个角色上下微调（负数 = 上移）

// ---------- 右键菜单里几个动作的持续时长 ----------
constexpr int MENU_SIT_MS        = 10000;  // 坐下休息 10 秒后回待机
constexpr int MENU_FLY_MS        = 4000;   // 飞行 4 秒
constexpr int MENU_SLEEP_MS      = 0;      // 0 = 一直睡，直到用户点它
constexpr int MENU_IDLE_MS       = 0;      // 0 = 一直待机（不定时）

// ---------- 行走序列（22~29 这 8 张图）----------
//
//  ★ 一次完整的走路不是「22→29 顺序循环」，而是三个阶段：★
//
//      ① 起步    WALK_START_IMGS   刚迈出第一步时播一次
//      ② 循环    WALK_LOOP_IMGS    一直反复播，直到该停下来了
//      ③ 收步    WALK_STOP_IMGS    停下之前连着播一次，播完回站立
//
//    画面上看到的就是：  (1)  (2)(3)(4)(5)(6)(2)(3)(4)(5)(6)…  (7)(8)  →  站立
//
//  想调顺序 / 取舍，只改下面三个数组；某个阶段不想要，就把它清空并把计数改成 0
//（例如把 WALK_START_IMGS 清空、WALK_START_COUNT 改成 0，就是"直接进循环"）。
constexpr int WALK_START_IMGS[] = { 22 };                   // (1)       起步，播一次
constexpr int WALK_LOOP_IMGS[]  = { 23, 24, 25, 26, 27 };   // (2)~(6)   循环体，反复播
constexpr int WALK_STOP_IMGS[]  = { 28, 29 };               // (7)(8)    收步，播一次
constexpr int WALK_START_COUNT  = 1;
constexpr int WALK_LOOP_COUNT   = 5;
constexpr int WALK_STOP_COUNT   = 2;

// 收步阶段总共占多长时间（毫秒）。DesktopPet 用它来判断"还剩这么久的时候该收步了"
constexpr int WALK_STOP_MS      = WALK_STOP_COUNT * WALK_FRAME_MS;
// 一次走路至少要够播完「一个完整循环 + 收步」，否则会刚起步就开始收步，像抽搐。
// 注意别和上面自动行为区的 WALK_MIN_MS（随机时长下限）搞混，这个是"技术的下限"。
constexpr int WALK_SHORTEST_MS  = WALK_STOP_MS + WALK_FRAME_MS * WALK_LOOP_COUNT;

// ---------- 点击时的随机动作 ----------
constexpr int SLEEP_WAKE_WAVE_MS = ACTION_MID_MS;
}   // namespace PetCfg

// -----------------------------------------------------------------------------
//  二·五、自由落体的一步积分
//
//  ★ 为什么把它单独抽出来 ★
//    因为 --falltrace 自检会直接调这个函数按 16ms 步进模拟下落。
//    也就是说，自检打出来的轨迹和运行时真正跑的轨迹，是同一份代码算出来的，
//    而不是"另写一套公式凑一个好看的报告"。改重力参数，两边同时生效。
// -----------------------------------------------------------------------------
struct PetFallStep
{
    double y;        // 这一步之后的位置（窗口左上角 y）
    double velY;     // 这一步之后的垂直速度（px/s，向下为正）
    bool   landed;   // 这一步之后是否已经落到地面
};

inline PetFallStep petFallAdvance(double y, double velY, double groundY, double dt)
{
    PetFallStep r;
    r.velY = qMin(velY + PetCfg::GRAVITY_PPS2 * dt, PetCfg::FALL_MAX_PPS);  // v += g*dt
    r.y    = y + r.velY * dt;                                               // y += v*dt

    r.landed = (r.y >= groundY - PetCfg::FALL_LAND_EPS);
    if (r.landed)
        r.y = groundY;   // 钉在地面上，免得陷进任务栏一点点
    return r;
}

// -----------------------------------------------------------------------------
//  三、资源：图片编号 -> qrc 里的路径
//
//  resources.qrc 里写的是 <qresource prefix="/pet">，所以运行时路径是 :/pet/xxx.png
//  ":/" 开头说明它来自 exe 内部，不是磁盘文件，所以打包后也能正常显示。
// -----------------------------------------------------------------------------
inline const QStringList& petImageNames()
{
    // 下标 0 是占位（没用），1~31 对应 31 张图
    static const QStringList names = {
        QString(),
        QStringLiteral("01_行走_右脚前.png"),
        QStringLiteral("02_行走_右脚中.png"),
        QStringLiteral("03_行走_右脚后.png"),
        QStringLiteral("04_行走_背面.png"),
        QStringLiteral("05_行走_左脚前.png"),
        QStringLiteral("06_行走_左脚中.png"),
        QStringLiteral("07_行走_左脚后.png"),
        QStringLiteral("08_站立.png"),
        QStringLiteral("09_表情_开心.png"),
        QStringLiteral("10_表情_委屈.png"),
        QStringLiteral("11_表情_惊讶.png"),
        QStringLiteral("12_表情_生气.png"),
        QStringLiteral("13_打瞌睡_睡觉.png"),
        QStringLiteral("14_坐下.png"),
        QStringLiteral("15_挥手.png"),
        QStringLiteral("16_思考.png"),
        QStringLiteral("17_吃东西.png"),
        QStringLiteral("18_喝水.png"),
        QStringLiteral("19_生气跺脚.png"),
        QStringLiteral("20_发呆.png"),
        QStringLiteral("21_飞行.png"),
        // ---- 22~29：新的 8 帧向右行走序列（左向由水平镜像得到）----
        QStringLiteral("22_行走_右01.png"),
        QStringLiteral("23_行走_右02.png"),
        QStringLiteral("24_行走_右03.png"),
        QStringLiteral("25_行走_右04.png"),
        QStringLiteral("26_行走_右05.png"),
        QStringLiteral("27_行走_右06.png"),
        QStringLiteral("28_行走_右07.png"),
        QStringLiteral("29_行走_右08.png"),
        // ---- 30：拖拽图（鼠标左键按住拖动时显示）----
        QStringLiteral("30_拖拽.png"),
        // ---- 31：下落图（松手后自由落体期间显示）----
        QStringLiteral("31_下落.png"),
    };
    return names;
}

constexpr int PET_IMAGE_COUNT = 31;   // 一共 31 张图（21 原始素材 + 8 行走序列 + 拖拽 + 下落）

// 由图片编号得到资源路径，例如 8 -> ":/pet/08_站立.png"
inline QString petImagePath(int img)
{
    if (img < 1 || img > PET_IMAGE_COUNT)
        return QString();
    return QStringLiteral(":/pet/") + petImageNames().at(img);
}

// -----------------------------------------------------------------------------
//  四、帧序列表：每个状态播放哪几张图、要不要左右翻转
//
//  flip = true 表示这张图在播放前先做一次水平镜像
//（用 QPixmap::transformed / QTransform 实现，不需要额外准备左向图片）
//
//  ★ 关于行走序列的设计依据（重要的美术结论，别乱改）★
//
//  【现在的行走】用 22~29 这 8 张（磁盘上 p22.png ~ p29.png），分三个阶段播：
//      起步 {22} → 循环 {23,24,25,26,27} → 收步 {28,29}
//    这 8 张是同一角色、同一视角（正面，双眼可见）的一组连续行走帧，
//    入库前已经统一处理过：
//      · 全部按 0.21212 等比缩放到「角色高度 ≈ 252px」，与 p08 站立图同一尺寸档；
//      · 全部用同一个裁切框裁剪，输出尺寸完全相同（215x261）。
//        同尺寸 = 走路时窗口不需要 resize，帧间不会左右/上下跳。
//    左向行走不另出图，由代码在运行时把同一组帧水平镜像（flip = true）得到。
//    三个数组的定义和原因见文件上面的「行走序列」注释。
//
//  【原来的 01~08】仍保留在资源里，其中 01/02/06/07 这四帧正面走路图现在已经不参与
//    任何状态（被 22~29 取代）。不要顺手删掉它们，因为：
//      · 03（朝左侧身）、05（朝右侧身，正好是 03 的镜像，轮廓重合度 0.97）
//        —— 这两个侧身帧仍然是「转身」动画用的；
//      · 04 是背面视角 —— TurnLeft / TurnRight 的第一帧用的就是它；
//      · 08 是站立图 —— Idle 用的还是它。
//    所以这套素材现在的分工是：01/02/06/07 备用，03/04/05 管转身，08 管待机。
// -----------------------------------------------------------------------------
struct PetFrame
{
    int  img;    // 图片编号 1~31
    bool flip;   // 是否水平翻转
};

// 拼接两个数组的公共模板：把 imgs 里的编号原样转成帧（统一决定要不要镜像）
inline void petAppendImgs(QVector<PetFrame>& v, const int* imgs, int count, bool flip)
{
    for (int i = 0; i < count; ++i)
        v.append({ imgs[i], flip });
}

// 「起步 + 循环体」这条序列是 WalkRight / WalkLeft 两个循环状态用的。
// 注意它把起步帧也放在开头 —— 循环回绕时不会回到起步帧，见 petStateLoopBegin()。
inline QVector<PetFrame> petWalkFrames(bool flip)
{
    QVector<PetFrame> v;
    v.reserve(PetCfg::WALK_START_COUNT + PetCfg::WALK_LOOP_COUNT);
    petAppendImgs(v, PetCfg::WALK_START_IMGS, PetCfg::WALK_START_COUNT, flip);
    petAppendImgs(v, PetCfg::WALK_LOOP_IMGS,  PetCfg::WALK_LOOP_COUNT,  flip);
    return v;
}

// 「收步」这条序列是 WalkStopRight / WalkStopLeft 两个一次性状态用的
inline QVector<PetFrame> petWalkStopFrames(bool flip)
{
    QVector<PetFrame> v;
    v.reserve(PetCfg::WALK_STOP_COUNT);
    petAppendImgs(v, PetCfg::WALK_STOP_IMGS, PetCfg::WALK_STOP_COUNT, flip);
    return v;
}

inline const QVector<PetFrame>& petFrames(PetState s)
{
    static const QHash<PetState, QVector<PetFrame>> table = {
        // 待机：只有站立图。走路结束后回落到这里
        { PetState::Idle,      { {8, false} } },

        // 拖拽：鼠标左键按住拖动期间显示这一张（角色被拎起来、双腿蜷着、头发往上飘）。
        //   单帧循环态，只由鼠标事件进入/离开，不参与随机行为。
        //   p30 画布是 183x264，角色底边距画布底边 12px —— 那 12px 就是"悬空量"。
        //   窗口是按画布开的、底边锚点固定，所以角色在屏幕上会比站立时整体高 12px，
        //   看起来就是被拎离地面。悬空量已经烘进图片里了，想调要重新处理
        //   resources/pet/p30.png，改代码是改不动的。
        { PetState::Drag,      { {30, false} } },

        // 下落：松开鼠标后自由落体期间显示这一张（张开双臂、蜷着腿、带汗滴和气流线）。
        //   单帧循环态 —— 进入和离开都由 DesktopPet 的重力逻辑负责（startFall / landFromFall），
        //   它自己不会结束，也不参与随机行为。
        //   ★ 画布刻意和 p30 做到逐像素同规格（183x264，角色底边距画布底边 12px）★
        //     所以 Drag -> Fall 换图时窗口尺寸一个像素都不变，画面绝对不会抖；
        //     而落地时 Fall(264) -> Idle(252) 收掉那 12px，正好就是"脚落回地面站定"。
        { PetState::Fall,      { {31, false} } },

        // 右向行走：【起步 22】+【循环体 23~27】，循环回绕时只回到 23，不会再播 22
        { PetState::WalkRight, petWalkFrames(false) },

        // 左向行走：同一组帧全部水平翻转（不额外准备左向图片）
        { PetState::WalkLeft,  petWalkFrames(true)  },

        // 收步：停下之前连着播一次 28 -> 29，播完回待机（一次性，不循环）
        { PetState::WalkStopRight, petWalkStopFrames(false) },
        { PetState::WalkStopLeft,  petWalkStopFrames(true)  },

        // 转身：先给一个背面，再给一张朝新方向的侧身，看起来像真的转过去了
        { PetState::TurnLeft,  { {4, false}, {3, false} } },
        { PetState::TurnRight, { {4, true }, {5, false} } },

        // 表情类（一次性）
        { PetState::Happy,     { {9, false} } },
        { PetState::Sad,       { {10, false} } },
        { PetState::Surprise,  { {11, false} } },
        { PetState::Angry,     { {12, false} } },
        { PetState::AngryFoot, { {19, false} } },

        // 循环类（不会自动结束，由外面决定什么时候离开）
        { PetState::Sleep,     { {13, false} } },
        { PetState::Sit,       { {14, false} } },
        { PetState::Daze,      { {20, false} } },
        { PetState::Fly,       { {21, false} } },

        // 其他一次性动作
        { PetState::Wave,      { {15, false} } },
        { PetState::Think,     { {16, false} } },
        { PetState::Eat,       { {17, false} } },
        { PetState::Drink,     { {18, false} } },
    };

    static const QVector<PetFrame> empty;
    const auto it = table.constFind(s);
    return (it == table.constEnd()) ? empty : it.value();
}

// 全部状态的固定顺序列表。
// 有了它，遍历状态时的顺序就是稳定的（自检报告读起来更清楚）。
inline const QVector<PetState>& petAllStates()
{
    static const QVector<PetState> all = {
        PetState::Idle,
        PetState::Drag,
        PetState::Fall,
        PetState::WalkRight, PetState::WalkLeft,
        PetState::WalkStopRight, PetState::WalkStopLeft,
        PetState::TurnLeft,  PetState::TurnRight,
        PetState::Happy,     PetState::Sad,
        PetState::Surprise,  PetState::Angry,
        PetState::AngryFoot, PetState::Sleep,
        PetState::Sit,       PetState::Wave,
        PetState::Think,     PetState::Eat,
        PetState::Drink,     PetState::Daze,
        PetState::Fly
    };
    return all;
}

// 是否是循环状态。循环状态永远不会自己结束，也不会发出 actionFinished
inline bool petStateIsLooping(PetState s)
{
    switch (s)
    {
    case PetState::Idle:
    case PetState::Drag:
    case PetState::Fall:
    case PetState::WalkRight:
    case PetState::WalkLeft:
    case PetState::Sleep:
    case PetState::Sit:
    case PetState::Daze:
    case PetState::Fly:
        return true;
    default:
        return false;
    }
}

// 循环状态下，帧号走到末尾时要绕回第几帧。
// 行走的序列是「起步帧 + 循环体帧」，起步帧（前 WALK_START_COUNT 帧）只该出现一次，
// 所以循环绕回时要跳过它们，回到循环体的第一帧。
inline int petStateLoopBegin(PetState s)
{
    switch (s)
    {
    case PetState::WalkRight:
    case PetState::WalkLeft:
        return PetCfg::WALK_START_COUNT;
    default:
        return 0;   // 其它循环状态老老实实从头绕回
    }
}

// 循环状态下每帧停留多久（毫秒）
inline int petStateFrameMs(PetState s)
{
    switch (s)
    {
    case PetState::WalkRight:
    case PetState::WalkLeft:
    case PetState::WalkStopRight:
    case PetState::WalkStopLeft:
        return PetCfg::WALK_FRAME_MS;    // 行走 100ms/帧 = 10 FPS
    case PetState::TurnLeft:
    case PetState::TurnRight:
        return PetCfg::TURN_FRAME_MS;
    default:
        return PetCfg::ACTION_SHORT_MS;  // 单帧循环状态用不到这个值
    }
}

// 一次性状态总共播多久（毫秒），播完会发出 actionFinished 信号
inline int petStateDurationMs(PetState s)
{
    switch (s)
    {
    case PetState::WalkStopRight:
    case PetState::WalkStopLeft:
        return PetCfg::WALK_STOP_MS;        // 28 -> 29 两帧，加起来 200ms
    case PetState::TurnLeft:
    case PetState::TurnRight:
        return PetCfg::TURN_FRAME_MS * 2;   // 2 帧
    case PetState::Happy:
    case PetState::Sad:
    case PetState::Surprise:
    case PetState::Angry:
        return PetCfg::ACTION_SHORT_MS;
    case PetState::Wave:
    case PetState::AngryFoot:
        return PetCfg::ACTION_MID_MS;
    case PetState::Eat:
    case PetState::Drink:
        return PetCfg::ACTION_MID_MS;
    case PetState::Think:
        return PetCfg::ACTION_LONG_MS;
    default:
        return PetCfg::ACTION_SHORT_MS;
    }
}
