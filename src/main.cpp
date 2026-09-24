// =============================================================================
//  main.cpp —— 程序入口
//
//  这个文件故意写得很短。它只做三件事：
//    1. 设置全局的 DPI 缩放策略
//    2. 创建 QApplication（Qt 的地基：窗口系统、消息循环、字体等）
//    3. 创建桌宠并交给事件循环
//
//  真正的逻辑分别在：
//    DesktopPet            —— 窗口、绘制、鼠标、移动
//    AnimationController   —— 图片、帧序列、帧率、翻转
//    PetBehaviorController —— 什么时候做什么（随机行为）
// =============================================================================

#include <QApplication>
#include <QCoreApplication>
#include <QColor>
#include <QDate>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QTextStream>
#include <QTime>

#include "DesktopPet.h"
#include "AnimationController.h"
#include "AffectionSystem.h"
#include "AffectionPage.h"
#include "SettingsPage.h"
#include "DailyImagePage.h"
#include "ChatPage.h"
#include "SongLibrary.h"
#include "MainPanel.h"

// -----------------------------------------------------------------------------
//  --walktrace：把行走的播放顺序逐帧打出来。
//
//  为什么需要它？
//    「走路 = 起步 + 循环 + 收步」是时序逻辑，光看帧表看不出"起步那一帧会不会
//    在循环里又冒出来"。这里不跑事件循环，只手动按 100ms 的节奏推进帧，
//    把每一步实际要画的那张图记下来，顺序对不对一眼就能看出来。
//
//  用法：PetPal.exe --walktrace
//  结果：petpal_walktrace.txt（和 exe 同目录）
//        注意 exe 是 WIN32 子系统（没有控制台），所以只能写文件，不能靠打印。
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
//  --selftest：自检模式。不接事件循环，跑完就退出。
//  会生成下面这些文件（都在 exe 同目录）：
//    petpal_selftest.txt          —— 这份 exe 的编译时间 + 全部图片的加载情况
//                                    + 每个状态的帧序列 + 系统托盘状态与隐藏/恢复往返测试
//                                    + 好感度的点数/等级/今日额度/存档路径
//    petpal_selftest_preview.png  —— 待机状态离屏渲染出来的实际画面
//    petpal_selftest_drag.png     —— 拖拽状态离屏渲染出来的实际画面
//    petpal_selftest_fall.png     —— 下落状态离屏渲染出来的实际画面
//    petpal_tray_icons.png        —— 托盘图标真正用到的 5 档尺寸（放大 4 倍并排）
//    petpal_selftest_panel.png    —— 主面板（左侧导航 + 好感度页）离屏渲染出来的样子
//    petpal_selftest_daily.png    —— 主面板的「每日图片」页（今天随机到的那张）
//    petpal_selftest_settings.png —— 主面板的「设置」页（重置好感度那张卡片）
//    petpal_selftest_settings.png —— 主面板的「设置」页（重置好感度那张卡片）
//    petpal_selftest_confirm.png  —— 重置前弹出的确认框（验证默认按钮落在「取消」上）
//
//  用法：PetPal.exe --selftest
//  排错用法：如果桌宠显示不出来，先跑它看报告。
//            预览图里角色以外的地方 alpha 应该是 0（完全透明），
//            如果那里是白色/黑色，说明透明窗口那几步设置没生效。
// -----------------------------------------------------------------------------

// 这份 exe 的"指纹"：编译时刻。
// 为什么要写进报告？因为本机有两个构建目录 ——
//    build/ninja-Debug/                              （build.bat 默认产物）
//    build/Desktop_Qt_6_11_2_MSVC2022_64bit_Debug/   （Qt Creator 用的产物）
// 改完代码只构建了其中一个、却运行了另一个，现象就是"改了完全没生效"。
// 报告里直接写上编译时间，跟源文件的时间戳一比，跑没跑对立刻见分晓。
static QString buildStamp()
{
    // __DATE__ / __TIME__ 是编译器在编译那一刻塞进来的常量，不是运行时间。
    // __DATE__ 固定是 "MMM dd yyyy"（如 "Sep 22 2026"，月份英文缩写，日期可能带前导空格）。
    // 这里转成 2026-09-22 23:10:15，看着顺眼些；转不出来就原样输出。
    const QDate d = QLocale::c().toDate(QString::fromLatin1(__DATE__).simplified(),
                                        QStringLiteral("MMM d yyyy"));
    const QTime t = QTime::fromString(QString::fromLatin1(__TIME__), QStringLiteral("hh:mm:ss"));
    if (!d.isValid() || !t.isValid())
        return QStringLiteral("%1 %2").arg(QString::fromLatin1(__DATE__),
                                           QString::fromLatin1(__TIME__));
    return d.toString(QStringLiteral("yyyy-MM-dd")) + QLatin1Char(' ')
         + t.toString(QStringLiteral("HH:mm:ss"));
}

// -----------------------------------------------------------------------------
//  --falltrace：把自由落体的过程逐步打出来
//
//  为什么需要它？
//    "松手 -> 掉下去 -> 落地站好"全程不到 1 秒，又同时有重力积分、终端速度、
//    落地判定这几件事，盯着屏幕根本看不出速度是不是真的在变快、
//    也看不出会不会落地时陷进任务栏一点点。
//    这里不跑事件循环，直接按 16ms 步进把轨迹打出来 —— 用的是运行时那一份
//    petFallAdvance()，不是另写一套公式。末尾再把三个相关状态的窗口尺寸并排列出：
//    换图抖不抖，看尺寸就知道。
//
//  用法：PetPal.exe --falltrace
//  结果：petpal_falltrace.txt（和 exe 同目录）
// -----------------------------------------------------------------------------
static QString runFallTrace()
{
    QString out;
    out += QStringLiteral("===== 自由落体时序自检 =====\r\n\r\n");

    // 自检用的假想场景：从 y=0 掉到 y=700 的"地面"。
    // 地面给个固定值只是为了让轨迹有确定的终点，公式和运行时完全一样。
    const double groundY = 700.0;
    const double dt = PetCfg::TICK_MS / 1000.0;

    out += QStringLiteral("重力加速度 g   = %1 px/s2\r\n").arg(PetCfg::GRAVITY_PPS2);
    out += QStringLiteral("终端速度上限   = %1 px/s\r\n").arg(PetCfg::FALL_MAX_PPS);
    out += QStringLiteral("心跳步长 dt    = %1 ms\r\n").arg(PetCfg::TICK_MS);
    out += QStringLiteral("起点 y = 0，地面 y = %1\r\n\r\n").arg(groundY, 0, 'f', 1);

    out += QStringLiteral("   步   时间(ms)   垂直速度(px/s)   本步位移(px)         y\r\n");
    out += QStringLiteral("  -----------------------------------------------------------\r\n");

    double y = 0.0, v = 0.0;
    double firstDisp = 0.0, maxDisp = 0.0;   // 第一步 / 落地前一步的位移，用来说明"越掉越快"
    int step = 0;
    bool landed = false;

    for (; step < 300; ++step)   // 300 步封顶，防止公式写错时死循环
    {
        const double prevY = y;
        const PetFallStep s = petFallAdvance(y, v, groundY, dt);
        y = s.y;
        v = s.velY;
        landed = s.landed;

        // 落地那一步的 y 被钉到地面上了，位移不准，所以统计时跳过它
        if (step == 0)
            firstDisp = y - prevY;
        if (!landed)
            maxDisp = qMax(maxDisp, y - prevY);

        // 只打前 6 步、每 5 步、以及落地那一步，避免刷屏
        if (step < 6 || landed || step % 5 == 0)
        {
            out += QStringLiteral("  %1  %2  %3  %4  %5\r\n")
                       .arg(step, 4)
                       .arg((step + 1) * PetCfg::TICK_MS, 8)
                       .arg(v, 12, 'f', 1)
                       .arg(y - prevY, 12, 'f', 2)
                       .arg(y, 12, 'f', 2);
        }
        if (landed)
            break;
    }

    out += QStringLiteral("\r\n");
    if (landed)
    {
        out += QStringLiteral("  落地：第 %1 步（约 %2 ms），落地速度 %3 px/s\r\n")
                   .arg(step).arg((step + 1) * PetCfg::TICK_MS).arg(v, 0, 'f', 1);
        out += QStringLiteral("  判读：垂直速度从 0 单调增大到落地值，单步位移同步变大\r\n"
                              "        （第 1 步只挪了 %1 px，落地前一步已经挪 %2 px）\r\n"
                              "        —— 这就是「越掉越快」，重力真的在起作用，不是匀速平移。\r\n")
                   .arg(firstDisp, 0, 'f', 2)
                   .arg(maxDisp, 0, 'f', 1);
    }
    else
    {
        out += QStringLiteral("  [!!] 300 步之内没落地，检查重力参数或落地判定。\r\n");
    }

    // ---------- 换图抖不抖，看窗口尺寸 ----------
    {
        AnimationController anim;
        anim.loadAll();
        const QSize si = anim.frameBoxSize(PetState::Idle);
        const QSize sd = anim.frameBoxSize(PetState::Drag);
        const QSize sf = anim.frameBoxSize(PetState::Fall);

        out += QStringLiteral("\r\n===== 换图时的窗口尺寸 =====\r\n\r\n");
        out += QStringLiteral("  Idle  待机   %1 x %2\r\n").arg(si.width()).arg(si.height());
        out += QStringLiteral("  Drag  拖拽   %1 x %2\r\n").arg(sd.width()).arg(sd.height());
        out += QStringLiteral("  Fall  下落   %1 x %2\r\n").arg(sf.width()).arg(sf.height());
        out += QStringLiteral("\r\n");
        out += (sd == sf)
                   ? QStringLiteral("  [OK] Drag 与 Fall 尺寸相同 -> 松手换图时窗口不做 resize，画面不抖。\r\n")
                   : QStringLiteral("  [!!] Drag 与 Fall 尺寸不同 -> 松手瞬间会 resize，可能看到跳动。\r\n");
        out += (sf.height() == si.height() + 12)
                   ? QStringLiteral("  [OK] Fall 比 Idle 高 12px -> 落地收起这 12px，正好是「脚落回地面」。\r\n")
                   : QStringLiteral("  [!!] Fall 与 Idle 的高度差不是 12px，去检查 p31 的画布。\r\n");
    }

    return out;
}

// -----------------------------------------------------------------------------
//  --affectiontrace：把好感度的时间轴打出来。
//
//  为什么需要它？
//    好感度里有两件事光看代码或者看面板都验证不了：
//      ① 跨天 —— 每日额度是不是真的在 0 点重置了
//      ② 衰减 —— 放着不管会不会掉级、掉到哪停
//    这两件事都要"过一段时间"才发生，总不能真的等一天再看。
//    所以这里给系统喂一条模拟时间轴：喂它就等于"过了这么久"，
//    而它内部跑的 settle()/checkNewDay() 和运行时完全是同一份代码。
//
//  参数 false 表示这次完全不读写用户真正的存档（免得自检把数据改了）。
//
//  用法：PetPal.exe --affectiontrace
//  结果：petpal_affectiontrace.txt（和 exe 同目录）
// -----------------------------------------------------------------------------
static QString runAffectionTrace()
{
    AffectionSystem sys(/*persistent=*/false);
    sys.debugStopTimer();     // 时间完全由这条轨迹控制，结果才可复现

    QString out;
    out += QStringLiteral("===== 好感度时序自检 =====\r\n\r\n");
    out += QStringLiteral("等级门槛 : 升到下一级 = %1 + %2 * 当前等级（点）\r\n")
               .arg(PetCfg::AFF_LEVEL_BASE, 0, 'f', 0)
               .arg(PetCfg::AFF_LEVEL_STEP, 0, 'f', 0);
    out += QStringLiteral("满级     : Lv.%1，累计需要 %2 点\r\n")
               .arg(PetCfg::AFF_MAX_LEVEL)
               .arg(affLevelFloor(PetCfg::AFF_MAX_LEVEL), 0, 'f', 0);
    out += QStringLiteral("衰减     : %1 点/小时（只在程序运行时算，掉到本级起点就停，不降级）\r\n")
               .arg(PetCfg::AFF_DECAY_PER_HOUR);
    out += QStringLiteral("每日额度 : 摸摸 %1 次（+%2/次，冷却 %3 秒），喂食 %4 次（+%5/次）\r\n")
               .arg(PetCfg::AFF_PET_PER_DAY).arg(PetCfg::AFF_PET_POINT, 0, 'f', 0)
               .arg(PetCfg::AFF_PET_COOLDOWN_MS / 1000)
               .arg(PetCfg::AFF_FEED_PER_DAY).arg(PetCfg::AFF_FEED_POINT, 0, 'f', 0);
    out += QStringLiteral("陪伴     : 每 %1 分钟 +%2（每天最多 +%3）\r\n\r\n")
               .arg(PetCfg::AFF_COMPANY_EVERY_MS / 60000)
               .arg(PetCfg::AFF_COMPANY_POINT, 0, 'f', 0)
               .arg(PetCfg::AFF_COMPANY_MAX_DAY, 0, 'f', 0);

    out += QStringLiteral("【1】初始：点数=%1，Lv.%2 %3\r\n\r\n")
               .arg(sys.point(), 0, 'f', 1).arg(sys.level()).arg(sys.stageName());

    // ---- 【2】放着不管会掉多少、掉到哪停 ----
    // 关键是要"有点数可掉"，而且已经离开本级起点一段距离 ——
    // 0 分的时候做这个测试，掉不掉都是 0，等于什么也没验证到。
    out += QStringLiteral("【2】放着不管：衰减会不会掉分、会不会掉级\r\n");
    sys.setCompanyActive(false);          // 只想看衰减，先把陪伴关掉
    sys.addSource(AffectionSystem::Source::Chat, 45.0);   // 聊天没有冷却，一次给 45 点
    out += QStringLiteral("     先玩一会儿攒到 %1 点：Lv.%2 %3，本级起点 %4，本级已攒 %5\r\n")
               .arg(sys.point(), 0, 'f', 1).arg(sys.level()).arg(sys.stageName())
               .arg(sys.levelFloor(), 0, 'f', 0).arg(sys.levelGained(), 0, 'f', 1);
    {
        double before = sys.point();
        double d1 = sys.debugAdvanceTime(24LL * 3600 * 1000);
        out += QStringLiteral("     24 小时不互动：%1 -> %2（掉 %3，正好是 %4 点/小时 x 24）\r\n")
                   .arg(before, 0, 'f', 2).arg(sys.point(), 0, 'f', 2)
                   .arg(d1, 0, 'f', 2).arg(PetCfg::AFF_DECAY_PER_HOUR);

        before = sys.point();
        const int lvBefore = sys.level();
        double d2 = sys.debugAdvanceTime(24LL * 3600 * 1000);
        out += QStringLiteral("     再来 24 小时：%1 -> %2（只掉了 %3 就停住了）\r\n")
                   .arg(before, 0, 'f', 2).arg(sys.point(), 0, 'f', 2).arg(d2, 0, 'f', 2);
        out += (sys.level() == lvBefore && d2 < PetCfg::AFF_DECAY_PER_HOUR * 24.0)
                   ? QStringLiteral("     [OK] 衰减生效、且掉到本级起点就停：等级没被拉下去（Lv.%1 稳住了）\r\n\r\n")
                         .arg(sys.level())
                   : QStringLiteral("     [!!] 和预期不符：检查 settle() 里的 levelFloor 钳位\r\n\r\n");
    }

    // ---- 【3】模拟正常使用：每天程序开 8 小时，做满额度 ----
    //      「程序关着」的 16 小时完全不算 —— 这是设计的一部分：
    //      关机期间不该掉好感度，也不该白送陪伴分。
    out += QStringLiteral("【3】每天：程序开 8 小时（其间摸满 %1 次 + 喂满 %2 次 + 5 次菜单动作），"
                          "其余 16 小时程序是关着的\r\n")
               .arg(PetCfg::AFF_PET_PER_DAY).arg(PetCfg::AFF_FEED_PER_DAY);
    out += QStringLiteral("     天   点数   等级   阶段   当日净增   衰减吃掉   摸摸额度\r\n");
    out += QStringLiteral("   ------------------------------------------------------------\r\n");

    sys.resetAll();
    sys.setCompanyActive(true);

    const qint64 dayMs   = 24LL * 3600 * 1000;
    const qint64 openMs  = 8LL * 3600 * 1000;
    const qint64 gapMs   = 5000;      // 两次操作之间隔 5 秒（顺便绕开摸摸的 3 秒冷却）
    const qint64 menuGap = 40000;     // 菜单动作之间隔 40 秒（绕开 30 秒冷却）

    int  reachedLv5Day  = 0;
    int  reachedMaxDay  = 0;
    bool quotaBlockedOk = false;

    for (int day = 1; day <= 30; ++day)
    {
        const double dayStart = sys.point();
        qint64 used = 0;

        // 摸摸：10 次
        for (int i = 0; i < PetCfg::AFF_PET_PER_DAY; ++i)
        {
            sys.addFromPet();
            sys.debugAdvanceTime(gapMs);
            used += gapMs;
        }
        // 再摸一次，验证"超额就被挡掉"（面板上那个按钮这时已经灰了）
        if (day == 1)
            quotaBlockedOk = (sys.addFromPet() == 0.0);

        // 喂食：3 次
        for (int i = 0; i < PetCfg::AFF_FEED_PER_DAY; ++i)
        {
            sys.addFromFeed();
            sys.debugAdvanceTime(gapMs);
            used += gapMs;
        }

        // 菜单动作：5 次
        for (int i = 0; i < 5; ++i)
        {
            sys.addSource(AffectionSystem::Source::MenuAction,
                          PetCfg::AFF_MENU_POINT, PetCfg::AFF_MENU_COOLDOWN_MS);
            sys.debugAdvanceTime(menuGap);
            used += menuGap;
        }

        // 剩下的开机时间什么都不做，光靠"陪伴"加分和自然衰减
        sys.debugAdvanceTime(qMax<qint64>(0, openMs - used));

        // ★ 这一天的两个数字必须在这里取 ★
        //   下面"程序关着的 16 小时"一推过去就跨过 0 点了，checkNewDay() 会把
        //   今日计数清零 —— 打印放在那之后就永远是 0，跟"衰减没生效"长得一模一样。
        const double net   = sys.point() - dayStart;
        const double decay = sys.todayDecay();

        // 程序关掉的 16 小时：时间照走，但什么都不结算
        sys.debugAdvanceTime(dayMs - openMs, /*running=*/false);

        out += QStringLiteral("   %1   %2   %3   %4   %5   %6   %7\r\n")
                   .arg(day, 3)
                   .arg(sys.point(), 7, 'f', 1)
                   .arg(sys.level(), 4)
                   .arg(sys.stageName())
                   .arg(QStringLiteral("%1%2").arg(net >= 0 ? "+" : "").arg(net, 0, 'f', 1), 8)
                   .arg(QStringLiteral("-%1").arg(decay, 0, 'f', 1), 8)
                   .arg(QStringLiteral("%1/%2").arg(PetCfg::AFF_PET_PER_DAY)
                            .arg(PetCfg::AFF_PET_PER_DAY), 6);

        if (!reachedLv5Day && sys.level() >= 5)
            reachedLv5Day = day;
        if (!reachedMaxDay && sys.isMaxLevel())
            reachedMaxDay = day;
    }

    out += QStringLiteral("\r\n     [%1] 第 %2 次摸摸被挡掉（每日额度是硬的）\r\n")
               .arg(quotaBlockedOk ? QStringLiteral("OK") : QStringLiteral("!!"))
               .arg(PetCfg::AFF_PET_PER_DAY + 1);
    out += QStringLiteral("     到 Lv.5 用了 %1 天；%2\r\n")
               .arg(reachedLv5Day > 0 ? QString::number(reachedLv5Day) : QStringLiteral("30 天还没到"),
                    reachedMaxDay > 0
                        ? QStringLiteral("满级（Lv.%1）用了 %2 天").arg(PetCfg::AFF_MAX_LEVEL).arg(reachedMaxDay)
                        : QStringLiteral("30 天内没满级"));
    out += QStringLiteral("     这 30 天平均每天净增 +%1 点（动作分 %2 + 陪伴 %3 - 衰减 %4）\r\n\r\n")
               .arg(sys.point() / 30.0, 0, 'f', 1)
               .arg(PetCfg::AFF_PET_PER_DAY * PetCfg::AFF_PET_POINT
                        + PetCfg::AFF_FEED_PER_DAY * PetCfg::AFF_FEED_POINT
                        + 5 * PetCfg::AFF_MENU_POINT, 0, 'f', 0)
               .arg(8.0 * 60.0 / (PetCfg::AFF_COMPANY_EVERY_MS / 60000.0), 0, 'f', 0)
               .arg(8.0 * PetCfg::AFF_DECAY_PER_HOUR, 0, 'f', 1);

    // ---- 【4】跨天：额度是不是真的在 0 点清零 ----
    out += QStringLiteral("【4】跨 0 点（关键：额度一定要自动重置，否则第二天就再也摸不到了）\r\n");
    out += QStringLiteral("     跨天前：摸摸还剩 %1 次，喂食还剩 %2 次\r\n")
               .arg(sys.petLeftToday()).arg(sys.feedLeftToday());
    {
        // 用掉一点额度，否则"还剩 10 次"这个数字本来就没变过，看不出重置
        sys.addFromPet();
        sys.addFromFeed();
        out += QStringLiteral("     用掉几次后：摸摸还剩 %1 次，喂食还剩 %2 次\r\n")
                   .arg(sys.petLeftToday()).arg(sys.feedLeftToday());

        // 程序关着的状态跨过 0 点：额度该重置还是要重置
        sys.debugAdvanceTime(24LL * 3600 * 1000, /*running=*/false);
        out += QStringLiteral("     跨天后：摸摸还剩 %1 次，喂食还剩 %2 次\r\n")
                   .arg(sys.petLeftToday()).arg(sys.feedLeftToday());
        out += (sys.petLeftToday() == PetCfg::AFF_PET_PER_DAY
                    && sys.feedLeftToday() == PetCfg::AFF_FEED_PER_DAY)
                   ? QStringLiteral("     [OK] 额度已经重置\r\n\r\n")
                   : QStringLiteral("     [!!] 额度没有重置，检查 checkNewDay()\r\n\r\n");
    }

    // ---- 【5】电脑睡了一觉：单次结算的时间窗上限有没有生效 ----
    // 用一个干净的系统来测，免得被上面那条轨迹的状态干扰。
    // 走的是 debugLateTick —— 也就是定时器那条真实路径（含钳位）。
    out += QStringLiteral("【5】电脑睡眠 6 小时后唤醒（定时器晚了 6 小时才响一次）\r\n");
    {
        AffectionSystem t(/*persistent=*/false);
        t.debugStopTimer();
        t.setCompanyActive(false);
        t.addSource(AffectionSystem::Source::Chat, 45.0);   // 攒到 Lv.2 的头一段，有 15 点可以被掉

        const double before = t.point();
        const double dec = t.debugLateTick(6LL * 3600 * 1000);
        out += QStringLiteral("     点数 %1 -> %2，实际只扣了 %3 点\r\n")
                   .arg(before, 0, 'f', 2).arg(t.point(), 0, 'f', 2).arg(dec, 0, 'f', 2);
        out += QStringLiteral("     （不加钳位的话这一下要扣 %1 点；钳位把一次结算的时间窗压到 %2 分钟 = %3 点）\r\n")
                   .arg(6.0 * PetCfg::AFF_DECAY_PER_HOUR, 0, 'f', 1)
                   .arg(PetCfg::AFF_DECAY_WINDOW_MS / 60000)
                   .arg(PetCfg::AFF_DECAY_PER_HOUR * PetCfg::AFF_DECAY_WINDOW_MS / 3600000.0, 0, 'f', 2);
        out += (dec <= PetCfg::AFF_DECAY_WINDOW_MS / 3600000.0 * PetCfg::AFF_DECAY_PER_HOUR + 0.0001)
                   ? QStringLiteral("     [OK] 钳位生效：睡一觉回来不会被一次性暴扣\r\n")
                   : QStringLiteral("     [!!] 钳位没生效，去检查 settle() 里的 clampWindow\r\n");
    }

    // ---- 【6】重置：设置页那个按钮最终走的就是这条路 ----
    out += QStringLiteral("【6】重置好感度（主面板「设置」页里那个按钮）\r\n");
    {
        AffectionSystem r(/*persistent=*/false);
        r.debugStopTimer();
        r.setCompanyActive(false);

        r.addSource(AffectionSystem::Source::Chat, 200.0);   // 先攒出等级，别在 0 分上重置
        r.addFromPet();                                     // 用掉一次摸摸额度，同时落下一段冷却
        r.addSource(AffectionSystem::Source::Chat, PetCfg::AFF_CHAT_POINT);  // 用掉一次聊天次数，让 chatUsed 非 0

        out += QStringLiteral("     重置前：点数=%1  Lv.%2 %3  累计互动 %4 次  "
                              "摸摸剩 %5  喂食剩 %6  聊天剩 %7\r\n")
                   .arg(r.point(), 0, 'f', 1).arg(r.level()).arg(r.stageName())
                   .arg(r.totalInteractions()).arg(r.petLeftToday())
                   .arg(r.feedLeftToday()).arg(r.chatLeftToday());

        r.resetAll();

        out += QStringLiteral("     重置后：点数=%1  Lv.%2 %3  累计互动 %4 次  "
                              "摸摸剩 %5  喂食剩 %6  聊天剩 %7\r\n")
                   .arg(r.point(), 0, 'f', 1).arg(r.level()).arg(r.stageName())
                   .arg(r.totalInteractions()).arg(r.petLeftToday())
                   .arg(r.feedLeftToday()).arg(r.chatLeftToday());

        // 重置完立刻再摸一次。要是只清了分数、没清那 3 秒冷却，
        // 这一次会被挡回 0 —— 用户看到的就是"额度明明还剩 10 次，可就是点不动"。
        const double again = r.addFromPet();
        out += QStringLiteral("     重置后紧接着再摸一次：加到 %1 点（说明冷却也一起清了）\r\n")
                   .arg(again, 0, 'f', 1);

        // 聊天次数也得回到满额（AFF_CHAT_PER_DAY）。之前漏清这一项，
        // 重置后 chatLeftToday 还是旧值 —— 这一节专门抓那个 bug。
        const bool cleaned = (r.level() == 1) && (r.totalInteractions() == 1)
                              && (again > 0.0) && (r.chatLeftToday() == PetCfg::AFF_CHAT_PER_DAY);
        out += cleaned
                   ? QStringLiteral("     [OK] 分数、等级、累计次数、每日额度（含聊天）、冷却全部归零\r\n")
                   : QStringLiteral("     [!!] 没清干净，去检查 AffectionSystem::resetAll()\r\n");
    }

    return out;
}

static int runSelfTest()
{
    const QDir exeDir(QCoreApplication::applicationDirPath());
    int exitCode = 0;

    // ---------- 第一步：检查全部图片能不能从 qrc 里读出来 ----------
    {
        AnimationController anim;
        const bool allOk = anim.loadAll();

        QFile file(exeDir.filePath(QStringLiteral("petpal_selftest.txt")));
        if (file.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            QTextStream ts(&file);
            ts.setEncoding(QStringConverter::Utf8);
            ts.setGenerateByteOrderMark(true);   // 加 UTF-8 BOM，避免记事本打开中文乱码

            // 头两行是"跑的是哪份 exe"的证据，排在报告正文之前。
            // 图数和编译时间对不上，就说明运行的不是刚编译的那份。
            ts << QStringLiteral("exe      : ") << QCoreApplication::applicationFilePath()
               << QStringLiteral("\r\n")
               << QStringLiteral("编译时间 : ") << buildStamp() << QStringLiteral("\r\n\r\n")
               << anim.describe();

            ts << (allOk ? QStringLiteral("\r\n[结果] 全部资源加载成功，可以正常运行。\r\n")
                         : QStringLiteral("\r\n[结果] 有资源缺失，请看上面的列表。\r\n"));
            ts.flush();
            file.close();
        }

        exitCode = allOk ? 0 : 2;
    }

    // ---------- 第二步：离屏渲染两张预览图 ----------
    // 这里真的建一个桌宠窗口并 grab()，所以看到的就是运行时的实际画面。
    // WindowDoesNotAcceptFocus + 马上退出，不会打扰用户。
    {
        DesktopPet pet;
        pet.start();
        QCoreApplication::processEvents();   // 让 show/resize 这些事件先处理掉

        // 待机：站立图。窗口尺寸应该和站立素材一致。
        pet.debugSetState(PetState::Idle);
        QCoreApplication::processEvents();
        pet.grab().save(exeDir.filePath(QStringLiteral("petpal_selftest_preview.png")));

        // 拖拽：按住鼠标左键拖动时显示的那张（被拎起来、脚离地）。
        // 单独给它拍一张，是因为"拖动中的画面"平时没法用截图抓到
        // （本机对分层窗口的屏幕截取还会返回全黑），
        // 而这张图又最三处容易出问题：尺寸不对、透明没处理好、压根没进资源。
        pet.debugSetState(PetState::Drag);
        QCoreApplication::processEvents();
        pet.grab().save(exeDir.filePath(QStringLiteral("petpal_selftest_drag.png")));

        // 下落：松手后自由落体期间显示的那张。它和拖拽图是"同一个动作的两拍"
        //（拎起来 -> 掉下去），所以尺寸必须一致，否则松手那一瞬间画面会跳。
        pet.debugSetState(PetState::Fall);
        QCoreApplication::processEvents();
        pet.grab().save(exeDir.filePath(QStringLiteral("petpal_selftest_fall.png")));

        // ---------- 顺带记一笔尺寸 / DPI ----------
        // 为什么要记？因为本机上 grab() 出来的图会比窗口的 size() 大 1.5 倍
        //（上面那三张预览图就是 275x378 而不是 183x252）。
        // 查过 Win32 GetWindowRect：窗口物理尺寸确实是 183x252，样式 LAYERED|TOPMOST|TOOLWINDOW
        // 也都对 —— 也就是说这只是 Qt 在高 DPI 感知上的一个怪癖，只影响自检产物，
        // 不影响屏幕上的实际显示。写进报告，省得下次又被这个数字吓一跳。
        {
            const QSize logical = pet.size();
            const qreal dpr = pet.devicePixelRatioF();
            const QPixmap g = pet.grab();

            QFile f(exeDir.filePath(QStringLiteral("petpal_selftest.txt")));
            if (f.open(QIODevice::Append | QIODevice::Text))
            {
                QTextStream ts(&f);
                ts.setEncoding(QStringConverter::Utf8);
                ts << QStringLiteral("\r\n--- 尺寸与 DPI（把这一节留着，以后查显示大小的问题先看它）---\r\n");
                ts << QStringLiteral("窗口逻辑尺寸 (Qt 的 size())  : %1 x %2\r\n")
                          .arg(logical.width()).arg(logical.height());
                ts << QStringLiteral("设备像素比 DPR               : %1\r\n").arg(dpr);
                ts << QStringLiteral("窗口物理尺寸 (逻辑 x DPR)    : %1 x %2\r\n")
                          .arg(qRound(logical.width() * dpr)).arg(qRound(logical.height() * dpr));
                ts << QStringLiteral("grab() 出来的图尺寸          : %1 x %2\r\n")
                          .arg(g.width()).arg(g.height());
                if (QScreen* sc = pet.screen())
                {
                    ts << QStringLiteral("所在屏幕逻辑分辨率           : %1 x %2\r\n")
                              .arg(sc->geometry().width()).arg(sc->geometry().height());
                    ts << QStringLiteral("所在屏幕可用区（已扣任务栏） : %1 x %2\r\n")
                              .arg(sc->availableGeometry().width()).arg(sc->availableGeometry().height());
                }
                ts.flush();
                f.close();
            }
        }

        // ---------- 系统托盘 ----------
        // 这一节是给"隐藏到状态栏"兜底的：那条路一旦断了（托盘不可用、图标没建起来），
        // 用户点了隐藏就再也叫不回来，只能去任务管理器杀进程。
        // 所以把托盘的实际状态写进报告，出问题时看一眼就知道断在哪。
        {
            QFile f(exeDir.filePath(QStringLiteral("petpal_selftest.txt")));
            if (f.open(QIODevice::Append | QIODevice::Text))
            {
                QTextStream ts(&f);
                ts.setEncoding(QStringConverter::Utf8);
                ts << QStringLiteral("\r\n--- 系统托盘（隐藏到右下角状态栏）---\r\n");
                ts << pet.describeTray();
                ts << QStringLiteral("\r\n隐藏/恢复往返测试（走的就是菜单项和托盘图标调的那两条路径）:\r\n");
                ts << pet.debugTrayRoundTrip();

                // 每日图片：图片在磁盘上（不在 .qrc 里），这一节说明"目录找没找到、
                // 今天该显示哪张、会不会一天之内变来变去"。
                ts << QStringLiteral("\r\n--- 每日图片（主面板那一页）---\r\n");
                ts << DailyImagePage::describeDailyImage();

                // 聊天：台词全部编译进 exe，所以"有多少句、什么时候用哪一句"
                // 是唯一值得看的东西。这一节顺带用真实输入跑一遍匹配 ——
                // 光看"意图 18 条"是看不出匹配逻辑对不对的。
                ts << QStringLiteral("\r\n--- 聊天（预设台词，不联网、不接大模型）---\r\n");
                ts << ChatPage::describeChat();

                // 曲库：歌和词都在磁盘上的 resources/songs/*.txt 里（不在 exe 里，
                // 也不在 .qrc 里）。这一节说明"目录找没找到、有几首歌、点歌会唱什么"。
                ts << QStringLiteral("\r\n--- 曲库（resources/songs/*.txt）---\r\n");
                ts << SongLibrary::describeLibrary();

                ts.flush();
                f.close();
            }
        }

        // ---------- 好感度 ----------
        // 面板上的数字对不对、存档写到哪、今天的额度还剩几次 ——
        // 这些都要么在面板里（自检时不会显示面板），要么跨天才会变，
        // 所以直接把状态打出来。真正的数值轨迹见 --affectiontrace。
        {
            QFile f(exeDir.filePath(QStringLiteral("petpal_selftest.txt")));
            if (f.open(QIODevice::Append | QIODevice::Text))
            {
                QTextStream ts(&f);
                ts.setEncoding(QStringConverter::Utf8);
                ts << QStringLiteral("\r\n--- 好感度 ---\r\n");
                ts << pet.describeAffection();
                ts.flush();
                f.close();
            }
        }

        // ---------- 托盘图标长得怎么样 ----------
        // 状态栏里那个图标只有 16px 高，站立图缩到那么小还认不认得出是只小人才是关键，
        // 光在报告里写"已创建"是看不出来的。所以把程序真正会用的那几档尺寸
        // 各放大 4 倍并排存一张图 —— 用的是和 setupTray() 完全相同的图和缩放参数。
        {
            const QPixmap src(petImagePath(PetCfg::TRAY_ICON_IMG));
            if (!src.isNull())
            {
                static const int sizes[] = { 16, 20, 24, 32, 48 };
                const int zoom = 4;      // 放大摆放，方便肉眼看清缩到 16px 后剩多少细节
                const int gap  = 14;
                const int maxH = 48 * zoom + gap * 2;

                int totalW = gap;
                for (int px : sizes)
                    totalW += px * zoom + gap;

                QPixmap sheet(totalW, maxH);
                sheet.fill(Qt::transparent);
                {
                    QPainter p(&sheet);
                    int x = gap;
                    for (int px : sizes)
                    {
                        const QPixmap one =
                            src.scaled(px, px, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                        const QPixmap big =
                            one.scaled(one.width() * zoom, one.height() * zoom,
                                       Qt::IgnoreAspectRatio, Qt::FastTransformation);
                        p.drawPixmap(x, (maxH - big.height()) / 2, big);
                        x += px * zoom + gap;
                    }
                }
                sheet.save(exeDir.filePath(QStringLiteral("petpal_tray_icons.png")));
            }
        }

        // ---------- 主面板长得怎么样 ----------
        // 面板是一个新的自绘窗口：圆角、左侧导航的选中样式、进度条、按钮样式，
        // 全部是手写样式表。这种"布局有没有排错行 / 样式表有没有生效"的问题，
        // 在报告里写一句"面板已创建"是看不出来的，所以离屏渲染。
        //
        // WA_DontShowOnScreen：布局、样式表、字体全按正常流程走，但窗口不会真的
        // 出现在屏幕上 —— 自检是个后台动作，不该在用户眼皮底下弹一个面板出来。
        {
            MainPanel panel;
            panel.setAttribute(Qt::WA_DontShowOnScreen, true);
            panel.addPage(QStringLiteral("好感度"),   new AffectionPage(pet.affection(), &panel));
            // 只读模式：自检不该把用户当天的图重新抽一遍
            panel.addPage(QStringLiteral("每日图片"), new DailyImagePage(/*persistent=*/false, &panel));
            // 聊天页也是只读用法：它的 showEvent 会发 chatEntered()，但自检这边
            // 没有把它接到 noteChat() 上，所以不会替用户加好感度、也不会写存档。
            auto* chatPage = new ChatPage(pet.affection(), &panel);
            panel.addPage(QStringLiteral("聊天"),     chatPage);
            panel.addPage(QStringLiteral("设置"),     new SettingsPage(pet.affection(), &panel));
            panel.show();
            QApplication::processEvents();

            // 面板本身是圆角透明的，直接存会是一张"四周全透明"的图，看不清楚边界，
            // 所以垫一层浅灰底，再把面板 1:1 画上去。
            //
            // ★ 用 render() 而不是 grab() ★
            //   grab() 返回的 pixmap 带着设备像素比（本机 150%），它的"逻辑尺寸"只有
            //   像素尺寸的 1/1.5，后面按逻辑坐标取源矩形时会再缩一次，图就糊了、还偏小。
            //   render() 是老老实实 1:1 往目标设备上画，没有这层换算。
            const auto shoot = [&](const QString& fileName) {
                QPixmap canvas(panel.size());
                canvas.fill(QColor(0xF0, 0xF0, 0xF0));
                panel.render(&canvas);
                canvas.save(exeDir.filePath(fileName));
            };

            shoot(QStringLiteral("petpal_selftest_panel.png"));     // 第 0 页：好感度

            // 「每日图片」：这一页的成败全看"图有没有等比放进框里、有没有被拉变形"，
            // 报告里写多少句都不如一张图。
            panel.setCurrentPage(1);
            QApplication::processEvents();
            shoot(QStringLiteral("petpal_selftest_daily.png"));

            // 「聊天」：这一页有两个只能看图看出来的东西 —— 左右两边的气泡样式
            //（天依在左、用户在右），以及输入行有没有排到窗口外面去。
            //
            // ★ 这里不是摆拍 ★：先等开场白打完（打字机 45ms/字，得给它真实时间跑），
            //   再往输入框里塞一句话、发一个真的回车事件。走的是和用户手打完全相同的
            //   那条路径（returnPressed -> onSend -> matchIntent -> Picker），
            //   所以截图里的那一问一答就是程序真跑出来的。
            panel.setCurrentPage(2);
            QApplication::processEvents();
            {
                const auto pump = [](int ms) {
                    QElapsedTimer t;
                    t.start();
                    while (t.elapsed() < ms)
                        QApplication::processEvents(QEventLoop::AllEvents, 20);
                };

                // 「说完了没有」不用猜秒数 —— 打字机放话期间发送键是禁用的，
                // 直接拿它当信号。开场白有长有短（最长那句要三秒多），
                // 猜一个固定值是猜不准的：改成固定等 900ms 时，截图里那句
                // 开场白正好被打断在半句话上。
                QPushButton* sendBtn = chatPage->findChild<QPushButton*>(QStringLiteral("send"));
                for (int i = 0; i < 100 && sendBtn && !sendBtn->isEnabled(); ++i)
                    pump(100);          // 上限 10 秒，纯属防呆

                if (QLineEdit* input = chatPage->findChild<QLineEdit*>())
                {
                    input->setText(QStringLiteral("你今天唱首歌给我听好不好"));
                    QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                    QCoreApplication::sendEvent(input, &press);
                }

                // 用户那句是立刻上屏的，天依的回复则在逐字放 —— 等它放完
                for (int i = 0; i < 100 && sendBtn && !sendBtn->isEnabled(); ++i)
                    pump(100);
            }
            shoot(QStringLiteral("petpal_selftest_chat.png"));

            // 「设置」页：重置按钮和那张卡片是纯样式表堆出来的，排错行只能看图。
            panel.setCurrentPage(3);
            QApplication::processEvents();
            shoot(QStringLiteral("petpal_selftest_settings.png"));

            // 放大状态：三处只有看图才知道对不对 —— 圆角有没有变成直角
            //（留着圆角的话铺满屏幕时四角会各透出一块桌面）、放大按钮有没有换成"还原"图标、
            // 顶部条那三个按钮在铺满之后是不是还老老实实靠右。
            panel.setCurrentPage(0);
            panel.toggleMaximized();
            QApplication::processEvents();
            shoot(QStringLiteral("petpal_selftest_panel_max.png"));
        }

        // ---------- 重置确认框 ----------
        // 这是整个"重置"功能里唯一会拦住用户的闸门：回车和 Esc 必须落在「取消」上，
        // 少了这一层，手快敲一下就把数据删了。这种事光看代码容易自欺，所以也渲染一张。
        // 用的是 SettingsPage::makeResetConfirmBox()，和真正点击时弹出的是同一段代码。
        {
            QMessageBox* box = SettingsPage::makeResetConfirmBox(nullptr);
            box->setAttribute(Qt::WA_DontShowOnScreen, true);
            box->show();
            QApplication::processEvents();

            QPixmap canvas(box->size());
            canvas.fill(QColor(0xF0, 0xF0, 0xF0));
            box->render(&canvas);
            canvas.save(exeDir.filePath(QStringLiteral("petpal_selftest_confirm.png")));

            box->deleteLater();
        }
    }

    return exitCode;
}

int main(int argc, char* argv[])
{
    // 必须在 QApplication 构造之前调用，因为它设置的是全局缩放策略。
    // PassThrough = 不做取整：125% 就按 1.25 倍算，而不是四舍五入成 1.0 / 2.0。
    // 对桌宠这种按像素绘图的程序，取整会让画面忽大忽小、位置抖动。
    QApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    // QApplication 必须先于任何 QWidget 构造
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("PetPal"));

    // 自检模式：跑完就退出，不进入事件循环
    if (QCoreApplication::arguments().contains(QStringLiteral("--selftest")))
        return runSelfTest();

    // 下落时序自检：把重力轨迹逐步打出来（同样是跑完就退出）
    if (QCoreApplication::arguments().contains(QStringLiteral("--falltrace")))
    {
        const QDir exeDir2(QCoreApplication::applicationDirPath());
        QFile f(exeDir2.filePath(QStringLiteral("petpal_falltrace.txt")));
        if (f.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            QTextStream ts(&f);
            ts.setEncoding(QStringConverter::Utf8);
            ts.setGenerateByteOrderMark(true);
            ts << runFallTrace();
            ts.flush();
            f.close();
        }
        return 0;
    }

    // 好感度时序自检：喂它一条模拟时间轴，看跨天重置和衰减对不对（跑完就退出）
    if (QCoreApplication::arguments().contains(QStringLiteral("--affectiontrace")))
    {
        const QDir exeDir3(QCoreApplication::applicationDirPath());
        QFile f(exeDir3.filePath(QStringLiteral("petpal_affectiontrace.txt")));
        if (f.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            QTextStream ts(&f);
            ts.setEncoding(QStringConverter::Utf8);
            ts.setGenerateByteOrderMark(true);
            ts << runAffectionTrace();
            ts.flush();
            f.close();
        }
        return 0;
    }

    // 桌宠对象建在栈上，生命周期就是 main 的作用域。
    // 它内部的窗口、定时器、两个控制器都会随它自动销毁，不会内存泄漏。
    DesktopPet pet;

    // 「和我聊天」的接口在这里接。
    // 第一阶段只打印一行日志；以后接本地 Qwen 时，把这里换成：
    //   1) 把用户的话发给模型
    //   2) 拿到回答
    //   3) 按回答内容让桌宠播 Think / Happy / Sad / Surprise / Angry
    QObject::connect(&pet, &DesktopPet::chatRequested, &pet, []()
    {
        qInfo() << "[PetPal] 收到聊天请求 —— 这里就是接入本地 Qwen 的位置。";
    });

    pet.start();

    // exec() 会阻塞在这里跑 Qt 事件循环，直到有人调用 quit()。
    // 程序"活"着的整个过程都发生在这行里面。
    return app.exec();
}
