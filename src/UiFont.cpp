#include "UiFont.h"

#include <QApplication>
#include <QFont>
#include <QRegularExpression>
#include <QSettings>

namespace {

const char* const kKey = "fontScale";

// ★ 存到独立的 ui.ini，不走 QSettings 的默认位置（注册表）★
//   两个理由：
//     · 项目里另外两份存档（affection.ini / daily_image.ini）都是显式的 IniFormat，
//       沿用同一套 —— 路径确定、能直接打开看，排查"重启没生效"时一眼见分晓；
//     · 默认构造的 QSettings 在组织名为空时落点由 Qt 自己兜底（不同版本还不一样），
//       读写虽然一致，但只要哪次"先读后设"就劈叉了 —— 不值得赌这个运气。
QSettings ini()
{
    // QSettings 是 QObject（不可拷贝也不可移动），但这里是 C++17 ——
    // 返回纯右值时保证省略拷贝，赋值给调用方的变量也是直接初始化，所以这么写合法。
    return QSettings(QSettings::IniFormat, QSettings::UserScope,
                     QStringLiteral("PetPal"), QStringLiteral("ui"));
}

int clampPercent(int percent)
{
    return qBound(UiFont::MIN_PERCENT, percent, UiFont::MAX_PERCENT);
}

// 内存里的当前档位。-1 = 还没读过，第一次用时从 QSettings 初始化。
int g_percent = -1;

// 基准字体：Qt/系统给的原始默认字体，只在第一次取。
// ★ 必须是"最初的"那一份 ★ —— 拿当前字体去乘比例，拖几下就把字号乘上天了。
const QFont& baseFont()
{
    static const QFont f = QApplication::font();
    return f;
}

} // namespace

int UiFont::scalePercent()
{
    if (g_percent < 0)
    {
        // 懒加载：第一次问的时候才去读存档。读到多少就是多少 ——
        // 读不出来（文件不在 / 值被手改坏了）才回落到 DEFAULT_PERCENT。
        const QSettings s = ini();
        g_percent = clampPercent(s.value(QString::fromLatin1(kKey), DEFAULT_PERCENT).toInt());
    }
    return g_percent;
}

void UiFont::setScalePercent(int percent)
{
    g_percent = clampPercent(percent);
    applyToApplication();
}

void UiFont::setScalePercentQuiet(int percent)
{
    // 只记档位。为什么要有这一条：见头文件里的说明（拖动预览的节流）。
    // 这里刻意**不**调 applyToApplication() —— 那是整个流程里最贵的一步。
    g_percent = clampPercent(percent);
}

void UiFont::saveScalePercent()
{
    // 立刻落盘，不攒着。
    // ★ 别改回"只在松手时存一次" ★ —— 滚轮、方向键改值走的是 triggerAction()，
    //   没有按下/松手这个过程，sliderReleased 永远不会发，那条路上就白改了。
    //   一个几行的 ini，值一变就写，代价可以忽略（affection.ini 也是每次变动就写）。
    QSettings s = ini();
    s.setValue(QString::fromLatin1(kKey), scalePercent());
}

QString UiFont::savePath()
{
    const QSettings s = ini();
    return s.fileName();
}

QString UiFont::describe()
{
    const QSettings s = ini();
    const bool stored = s.contains(QString::fromLatin1(kKey));

    QString out;
    out += QStringLiteral("当前档位 : %1%（基准 %2%，范围 %3%~%4%，步长 %5%）\r\n")
               .arg(scalePercent()).arg(BASE_PERCENT)
               .arg(MIN_PERCENT).arg(MAX_PERCENT).arg(STEP_PERCENT);
    out += QStringLiteral("存档文件 : %1\r\n").arg(savePath());
    out += stored
               ? QStringLiteral("存档内容 : %1%（本次启动就是按它来的）\r\n")
                     .arg(s.value(QString::fromLatin1(kKey)).toInt())
               : QStringLiteral("存档内容 : 还没有写过 —— 走默认 %1%，用户在设置页调一次就会存下来\r\n")
                     .arg(DEFAULT_PERCENT);
    out += QStringLiteral("生效方式 : 启动时 applyToApplication() 缩默认字体 + 各页面 styleSheet() 改写 QSS 字号\r\n");
    return out;
}

int UiFont::px(int basePx)
{
    const int pct = scalePercent();
    if (pct == BASE_PERCENT)          // 基准档：字面值本来就是 1:1，不用算
        return basePx;

    return qMax(1, qRound(double(basePx) * double(pct) / 100.0));
}

QString UiFont::styleSheet(const QString& qss)
{
    const int pct = scalePercent();
    if (pct == BASE_PERCENT)
        return qss;                          // 基准档时原样返回，省一次正则

    // 注意大小写和空格：项目里都写 `font-size: 12px;`，但留一点余量
    static const QRegularExpression re(QStringLiteral("font-size\\s*:\\s*(\\d+)\\s*px"));

    QString out;
    out.reserve(qss.size() + qss.size() / 8);

    qsizetype last = 0;
    auto it = re.globalMatch(qss);
    while (it.hasNext())
    {
        const QRegularExpressionMatch m = it.next();

        out += qss.mid(last, m.capturedStart() - last);
        out += QStringLiteral("font-size:%1px")
                   .arg(qMax(1, qRound(double(m.captured(1).toInt()) * double(pct) / 100.0)));
        last = m.capturedEnd();
    }
    out += qss.mid(last);

    return out;
}

void UiFont::applyToApplication()
{
    if (!qApp)
        return;

    QFont f = baseFont();
    const double factor = double(scalePercent()) / 100.0;

    // 有的平台给的是像素字号、有的是磅值，两种都要照顾到，
    // 不然会出现"改了没反应"（动的是那个没用上的字段）。
    if (f.pixelSize() > 0)
        f.setPixelSize(qMax(1, qRound(double(f.pixelSize()) * factor)));
    else
        f.setPointSizeF(qMax(1.0, f.pointSizeF() * factor));

    QApplication::setFont(f);
}
