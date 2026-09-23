#include "AnimationController.h"

#include <QTimer>
#include <QTransform>
#include <QDebug>

// =============================================================================
//  构造函数：只建一个定时器，什么都不加载。
//  真正的加载放在 loadAll() 里，这样调用方能拿到成功/失败的结果。
// =============================================================================
AnimationController::AnimationController(QObject* parent) : QObject(parent)
{
    // parent 传 this：AnimationController 析构时定时器会被自动 delete，不用手动回收
    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);   // 精确计时，行走帧率更稳
    connect(m_timer, &QTimer::timeout, this, &AnimationController::onFrameTimer);
}

// =============================================================================
//  加载全部 PNG
//
//  为什么用 ":/pet/xxx.png" 这种路径而不是磁盘绝对路径？
//  因为 ":/" 是 Qt 资源系统（qrc）虚拟出来的路径，图片在编译时就被塞进了 exe。
//  好处：拷到任何电脑上都能显示，不会出现"换个目录就找不到图"的问题。
// =============================================================================
bool AnimationController::loadAll()
{
    m_raw.clear();
    m_flipped.clear();
    m_resolved.clear();
    m_missing.clear();

    // ---- 第一步：把 PET_IMAGE_COUNT 张原图逐张读进来 ----
    for (int no = 1; no <= PET_IMAGE_COUNT; ++no)
    {
        const QString path = petImagePath(no);

        // QPixmap 的构造函数会直接读文件；读不到时得到一张 isNull()==true 的空图。
        // 这里不要用 QPixmap::load() 的返回值做判断，直接判 isNull() 更直观。
        QPixmap pm(path);
        if (pm.isNull())
        {
            m_missing << path;
            qWarning() << "[AnimationController] 读不到图片:" << path;
            continue;
        }

        // 强制按 1:1 像素绘制。
        // 如果图片里自带 DPI 信息，Qt 会按 devicePixelRatio 缩放，桌宠大小就会和预期不符。
        pm.setDevicePixelRatio(1.5);

        m_raw.insert(no, pm);
    }

    // ---- 第二步：为每个状态组装帧序列（把需要翻转的帧提前翻好）----
    // 提前做的好处：绘制时不必每帧都翻转，省 CPU，画面也不会卡。
    const QVector<PetState> all = petAllStates();
    for (PetState s : all)
        rebuildFor(s);

    // ---- 第三步：把第一个状态设成 Idle（显示 08 站立），并把它自己的帧索引清零 ----
    m_state = PetState::Idle;
    m_index = 0;
    m_elapsedMs = 0;
    restartTimer();

    return m_missing.isEmpty();
}

// =============================================================================
//  为某个状态组装帧序列
// =============================================================================
void AnimationController::rebuildFor(PetState s)
{
    if (m_resolved.contains(s))
        return;   // 已经组装过就不重复做

    QVector<QPixmap> list;
    const QVector<PetFrame>& frames = petFrames(s);
    list.reserve(frames.size());

    for (const PetFrame& f : frames)
    {
        // 取原图或它的水平镜像版
        const auto rawIt = m_raw.constFind(f.img);
        if (rawIt == m_raw.constEnd())
            continue;   // 这张图当初没加载成功，跳过

        // 注意：flippedOf 可能会往 m_flipped 里插入新元素导致重哈希，
        // 所以这里必须马上 append 一份拷贝，不能把引用留到下一次循环。
        list.append(f.flip ? flippedOf(f.img) : rawIt.value());
    }

    m_resolved.insert(s, list);
}

// =============================================================================
//  取某张图的水平翻转版
//
//  QTransform().scale(-1, 1) 就是"x 方向取反"，也就是左右镜像。
//  QPixmap::transformed() 会自动把结果的包围盒平移到原点，
//  所以翻转后的图尺寸不变、不会歪，直接就能画在原地。
// =============================================================================
const QPixmap& AnimationController::flippedOf(int img)
{
    auto it = m_flipped.find(img);
    if (it == m_flipped.end())
    {
        const auto rawIt = m_raw.constFind(img);
        if (rawIt == m_raw.constEnd())
        {
            it = m_flipped.insert(img, QPixmap());
        }
        else
        {
            // 用 SmoothTransformation 让边缘不那么锯齿
            const QPixmap mirror =
                rawIt.value().transformed(QTransform().scale(-1, 1), Qt::SmoothTransformation);
            it = m_flipped.insert(img, mirror);
        }
    }
    return it.value();
}

// =============================================================================
//  切换状态
// =============================================================================
void AnimationController::setState(PetState s, bool force)
{
    if (s == m_state)
    {
        // 同一个循环状态重复设置毫无意义。
        // 这里必须拦掉，否则每帧都重启动画，帧号永远停在第一帧，看起来就像"卡住了"。
        if (petStateIsLooping(s))
            return;
        // 同一个一次性动作：默认不重播，force 时才从头再来
        if (!force)
            return;
    }

    // ★ 防止动作互相冲突的关键一行 ★
    // 正在播一次性动作（比如"思考 1.5 秒"）时，自动行为想插进来会被直接拒绝；
    // 只有用户明确的操作（force=true）才能打断。
    if (!force && !petStateIsLooping(m_state))
        return;

    const PetState before = m_state;

    m_state = s;
    m_index = 0;
    m_elapsedMs = 0;

    rebuildFor(s);     // 正常情况下已经组装好了，这里是兜底
    restartTimer();

    emit stateChanged(s, before);   // DesktopPet 收到后会按新帧尺寸调整窗口
    emit frameChanged();            // 请求重绘
}

// =============================================================================
//  按当前状态重新设置帧定时器
// =============================================================================
void AnimationController::restartTimer()
{
    m_timer->stop();

    const int count = m_resolved.value(m_state).size();
    if (count <= 0)
        return;   // 没有帧可播（资源缺失），不开定时器

    // 单帧的循环状态（站着 / 睡觉 / 坐下 / 发呆 / 飞行）是静止画面，
    // 根本不需要定时器，省得每秒白跑 10 次空回调。
    if (count == 1 && petStateIsLooping(m_state))
        return;

    m_timer->start(petStateFrameMs(m_state));
}

// =============================================================================
//  帧定时器超时：推进一帧
// =============================================================================
void AnimationController::onFrameTimer()
{
    const QVector<QPixmap>& frames = m_resolved.value(m_state);
    const int count = frames.size();
    if (count <= 0)
        return;

    // 用定时器的名义间隔累加。
    // 动画和"物理移动"不同：这里只是按固定节奏换图，用名义间隔完全够用，
    // 也不需要在 60Hz 心跳里额外判断（那种做法容易因为掉帧导致动画变慢或变快）。
    m_elapsedMs += m_timer->interval();

    if (count > 1)
    {
        if (petStateIsLooping(m_state))
        {
            // 循环状态：走到末尾就绕回去。
            // 绕回的落点不是第 0 帧，而是 petStateLoopBegin()：
            // 行走序列开头的「起步帧」只该出现一次，循环时要从循环体第一帧接上。
            const int begin = qBound(0, petStateLoopBegin(m_state), count - 1);
            m_index = (m_index + 1 >= count) ? begin : m_index + 1;
        }
        else if (m_index + 1 < count)
        {
            // 一次性动作：停在最后一帧，绝不绕回开头。
            //（否则收步的 28 -> 29 会在末尾闪一下 28，转身的 04 -> 03 也是同样的问题）
            m_index = m_index + 1;
        }

        emit frameChanged();
    }

    // 一次性动作：播够时间就收尾
    if (!petStateIsLooping(m_state) && m_elapsedMs >= petStateDurationMs(m_state))
        finishOneShot();
}

// =============================================================================
//  一次性动作播完
// =============================================================================
void AnimationController::finishOneShot()
{
    const PetState finished = m_state;

    m_timer->stop();

    // 先发信号，让 DesktopPet 有机会"接上后续动作"
    //（比如：挥手 -> 开心 -> 待机；再比如：转身结束 -> 继续朝新方向走）
    emit actionFinished(finished);

    // 槽函数里没有改变状态，说明外界不关心后续，那就自动回到待机。
    // 这一句保证了"任何特殊动作结束后都会回到 08 站立"。
    if (m_state == finished)
        setState(PetState::Idle, true);
}

// =============================================================================
//  当前该画的那一帧
// =============================================================================
const QPixmap& AnimationController::currentPixmap() const
{
    static const QPixmap kEmpty;

    const auto it = m_resolved.constFind(m_state);
    if (it == m_resolved.constEnd() || it.value().isEmpty())
        return kEmpty;

    const int i = qBound(0, m_index, int(it.value().size()) - 1);
    return it.value().at(i);
}

// =============================================================================
//  当前帧对应的是哪张图（图片编号 + 是否镜像）
// =============================================================================
PetFrame AnimationController::currentFrameRef() const
{
    const QVector<PetFrame>& refs = petFrames(m_state);
    if (m_index < 0 || m_index >= refs.size())
        return PetFrame{ 0, false };
    return refs.at(m_index);
}

// =============================================================================
//  是不是已经播到循环体的最后一帧
// =============================================================================
bool AnimationController::atLoopTail() const
{
    // 不在循环状态（比如正好被切成了表情），就别拦着，交给调用方决定
    if (!petStateIsLooping(m_state))
        return true;

    const int count = m_resolved.value(m_state).size();
    if (count <= 1)
        return true;

    // 注意判断的是"下一帧要绕回"，也就是当前正好停在最后一帧上
    return m_index + 1 >= count;
}

// =============================================================================
//  手动推进一帧（自检 / 排错专用，见头文件说明）
// =============================================================================
void AnimationController::stepFrame()
{
    onFrameTimer();
}

// =============================================================================
//  某个状态里所有帧的最大宽高
// =============================================================================
QSize AnimationController::frameBoxSize(PetState s) const
{
    QSize box;

    const auto it = m_resolved.constFind(s);
    if (it == m_resolved.constEnd())
        return box;

    for (const QPixmap& p : it.value())
    {
        if (p.isNull())
            continue;
        if (p.width() > box.width())
            box.setWidth(p.width());
        if (p.height() > box.height())
            box.setHeight(p.height());
    }
    return box;
}

// =============================================================================
//  生成自检报告（PetPal.exe --selftest 会把它写到文件里）
// =============================================================================
QString AnimationController::describe() const
{
    QString out;
    out += QStringLiteral("=== PetPal 资源自检报告 ===\r\n");
    out += QStringLiteral("应加载图片: %1 张   成功: %2 张   失败: %3 张\r\n")
               .arg(PET_IMAGE_COUNT)
               .arg(m_raw.size())
               .arg(m_missing.size());

    if (!m_missing.isEmpty())
    {
        out += QStringLiteral("\r\n[!!] 下面这些资源没读到，请检查 resources/resources.qrc 里有没有列出来：\r\n");
        for (const QString& p : m_missing)
            out += QStringLiteral("     ") + p + QStringLiteral("\r\n");
    }

    out += QStringLiteral("\r\n--- 每个状态的帧序列 ---\r\n");
    const QVector<PetState> all = petAllStates();
    for (PetState s : all)
    {
        const QVector<PetFrame>& refs = petFrames(s);
        const QVector<QPixmap> frames = m_resolved.value(s);
        const QSize box = frameBoxSize(s);

        out += QString::fromUtf8(petStateName(s))
             + QStringLiteral("  ")
             + (petStateIsLooping(s) ? QStringLiteral("[循环]") : QStringLiteral("[一次性 %1ms]").arg(petStateDurationMs(s)))
             + QStringLiteral("  帧数=%1  窗口=%2x%3   ")
                   .arg(frames.size())
                   .arg(box.width())
                   .arg(box.height());

        for (int i = 0; i < refs.size(); ++i)
        {
            out += QStringLiteral("#%1%2 ")
                       .arg(refs.at(i).img, 2, 10, QLatin1Char('0'))
                       .arg(refs.at(i).flip ? QStringLiteral("(镜像)") : QString());
        }

        if (frames.isEmpty())
            out += QStringLiteral(" <<< 没有任何可用帧！");

        out += QStringLiteral("\r\n");
    }

    // 行走时序单独写一段：它不是"22 一直循环到 29"，看报告时容易误会，这里写清楚
    out += QStringLiteral("\r\n--- 行走时序（右向；左向 = 整条水平镜像）---\r\n");
    out += QStringLiteral("  起步(播一次): ");
    for (int i = 0; i < PetCfg::WALK_START_COUNT; ++i)
        out += QStringLiteral("#%1 ").arg(PetCfg::WALK_START_IMGS[i], 2, 10, QLatin1Char('0'));
    out += QStringLiteral(" -> 循环(反复播): ");
    for (int i = 0; i < PetCfg::WALK_LOOP_COUNT; ++i)
        out += QStringLiteral("#%1 ").arg(PetCfg::WALK_LOOP_IMGS[i], 2, 10, QLatin1Char('0'));
    out += QStringLiteral(" -> 收步(播一次): ");
    for (int i = 0; i < PetCfg::WALK_STOP_COUNT; ++i)
        out += QStringLiteral("#%1 ").arg(PetCfg::WALK_STOP_IMGS[i], 2, 10, QLatin1Char('0'));
    out += QStringLiteral(" -> Idle(站立)\r\n");
    out += QStringLiteral("  一句话：起步 1 帧只出现一次，中间 5 帧反复播，停下前才连着播收步 2 帧"
                          "（收步占 %1ms）。\r\n")
               .arg(PetCfg::WALK_STOP_MS);

    return out;
}
