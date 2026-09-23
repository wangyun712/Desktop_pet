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
#include <QDate>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QLocale>
#include <QPixmap>
#include <QScreen>
#include <QTextStream>
#include <QTime>

#include "DesktopPet.h"
#include "AnimationController.h"

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
//  会生成四个文件（都在 exe 同目录）：
//    petpal_selftest.txt          —— 这份 exe 的编译时间 + 全部图片的加载情况
//                                    + 每个状态的帧序列
//    petpal_selftest_preview.png  —— 待机状态离屏渲染出来的实际画面
//    petpal_selftest_drag.png     —— 拖拽状态离屏渲染出来的实际画面
//    petpal_selftest_fall.png     —— 下落状态离屏渲染出来的实际画面
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
