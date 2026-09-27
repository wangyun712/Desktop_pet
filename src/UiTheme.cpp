#include "UiTheme.h"

#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QVector>

// =============================================================================
//  预设主题
//
//  ★ 为什么 0 号必须是"现在这个样子" ★
//    QSS 字面量映射表（下面 kLiteralMap）是从**默认主题**的色值反推出来的：
//    页面 QSS 里写死的每个色值都对应一个角色。0 号主题原样填回这些色值，
//    用户不换主题时界面和改造前一模一样 —— 映射表错了也能从这里看出来。
//
//  ★ 带透明的三个角色（NavBg / SurfaceAlt / UserBubble）★
//    背景图画在面板最底层（MainPanel::paintEvent）。这三个角色的 alpha 略低于
//    255，叠在图上时图能隐约透出来，文字又不会被花纹搅得读不清。
//    ★ 没设背景图时它们也照常半透明 ★ —— 底下是同一主题的 PanelBg 实心底，
//    同色叠同色肉眼看不出差别，所以不需要"有图才透明"的两套值。
// =============================================================================
namespace {

// 强调色一族的取值（一套主题里"彩色"的那半边）
struct AccentSet
{
    QRgb accent, deep, hover, pressed, soft, softBg, softBg2, mid, mid2, text;
    QRgb chat, chatHover, onChat, tint;
};

// 中性色一族的取值（"灰白"的那半边）
struct NeutralSet
{
    QRgb panel, nav, surface, userBubble, hover, border, disabled;
    QRgb textStrong, textMid, textSub, textFaint, textDisabled;
};

// 导航/卡片底的透明度（255 制）。见文件头"带透明的三个角色"。
constexpr int NAV_ALPHA     = 242;
constexpr int SURFACE_ALPHA = 234;
constexpr int BUBBLE_ALPHA  = 244;

UiTheme::Preset makePreset(const char* id, const char* name, const AccentSet& a, const NeutralSet& n)
{
    UiTheme::Preset p;
    p.id   = id;
    p.name = name;

    p.colors[UiTheme::Accent]         = QColor(a.accent);
    p.colors[UiTheme::AccentDeep]     = QColor(a.deep);
    p.colors[UiTheme::AccentHover]    = QColor(a.hover);
    p.colors[UiTheme::AccentPressed]  = QColor(a.pressed);
    p.colors[UiTheme::AccentSoft]     = QColor(a.soft);
    p.colors[UiTheme::AccentSoftBg]   = QColor(a.softBg);
    p.colors[UiTheme::AccentSoftBg2]  = QColor(a.softBg2);
    p.colors[UiTheme::AccentMid]      = QColor(a.mid);
    p.colors[UiTheme::AccentMid2]     = QColor(a.mid2);
    p.colors[UiTheme::AccentText]     = QColor(a.text);
    p.colors[UiTheme::ChatAccent]     = QColor(a.chat);
    p.colors[UiTheme::ChatAccentHover]= QColor(a.chatHover);
    p.colors[UiTheme::OnAccent]       = QColor(a.onChat);
    p.colors[UiTheme::ChatAccentTint] = QColor(a.tint);

    p.colors[UiTheme::PanelBg]      = QColor(n.panel);
    p.colors[UiTheme::NavBg]        = QColor(n.nav);
    p.colors[UiTheme::SurfaceAlt]   = QColor(n.surface);
    p.colors[UiTheme::UserBubble]   = QColor(n.userBubble);
    p.colors[UiTheme::HoverBg]      = QColor(n.hover);
    p.colors[UiTheme::Border]       = QColor(n.border);
    p.colors[UiTheme::Disabled]     = QColor(n.disabled);
    p.colors[UiTheme::TextStrong]   = QColor(n.textStrong);
    p.colors[UiTheme::TextMid]      = QColor(n.textMid);
    p.colors[UiTheme::TextSub]      = QColor(n.textSub);
    p.colors[UiTheme::TextFaint]    = QColor(n.textFaint);
    p.colors[UiTheme::TextDisabled] = QColor(n.textDisabled);

    p.colors[UiTheme::NavBg].setAlpha(NAV_ALPHA);
    p.colors[UiTheme::SurfaceAlt].setAlpha(SURFACE_ALPHA);
    p.colors[UiTheme::UserBubble].setAlpha(BUBBLE_ALPHA);

    return p;
}

// 6 套预设。0 号 = 改造前的原样
//围绕某一个主色进行颜色深浅变化，第一个是主色颜色组，第二个是中性颜色组
const QVector<UiTheme::Preset>& presets()
{
    static const QVector<UiTheme::Preset> v = {
        makePreset("tianyi", u8"淡紫色",
            { 0x7F77DD, 0x534AB7, 0x6E65D6, 0x5F57C8, 0xAFA9EC, 0xEEEDFE, 0xE3E0FB,
              0x9C95E6, 0xB4B0E8, 0x26215C, 0x66CCFF, 0x8FDAFF, 0x042C53, 0xE6F1FB },
            { 0xFFFFFF, 0xF1EFE8, 0xF7F6F2, 0xEDEBE4, 0xE8E6DF, 0xE3E1D9, 0xD3D1C7,
              0x2C2C2A, 0x5F5E5A, 0x888780, 0x9A9890, 0xB4B2A9 }),

        makePreset("sakura", u8"樱花粉",
            { 0xE87EA6, 0xC05081, 0xEF90B4, 0xDA6F97, 0xF2B4CB, 0xFCEFF4, 0xF8DCE8,
              0xF3A8C4, 0xF7C0D5, 0x6B2444, 0xF088B0, 0xF7B1CB, 0x59182F, 0xFBE3ED },
            { 0xFFFDFD, 0xF8EFF2, 0xFBF5F7, 0xF4E8EC, 0xF0E4E9, 0xEBDCE2, 0xDECCD3,
              0x2C2C2A, 0x5F5E5A, 0x888780, 0x9A9890, 0xB4B2A9 }),

        makePreset("matcha", u8"抹茶绿",
            { 0x74AE68, 0x4C8446, 0x82BB75, 0x679F5C, 0xABD1A2, 0xF0F7ED, 0xDDEEDA,
              0x93C489, 0xAED4A6, 0x294D24, 0x7FBE73, 0xA6D59C, 0x173012, 0xE6F4E1 },
            { 0xFCFEFB, 0xF0F5EC, 0xF5F9F3, 0xEAF1E6, 0xE7EFE2, 0xDFE8D9, 0xCCD9C5,
              0x2C2C2A, 0x5F5E5A, 0x888780, 0x9A9890, 0xB4B2A9 }),

        makePreset("citrus", u8"蜜柑橙",
            { 0xEE9A3F, 0xC4711A, 0xF2A957, 0xE08C2F, 0xF5C48C, 0xFDF4E7, 0xFAE5CB,
              0xF4B169, 0xF8C795, 0x6B420F, 0xF4A94E, 0xF8C684, 0x59350A, 0xFCEBD2 },
            { 0xFFFEFB, 0xF8F3E9, 0xFBF7F0, 0xF3ECDF, 0xF1E9DB, 0xEAE2D2, 0xDCD2BE,
              0x2C2C2A, 0x5F5E5A, 0x888780, 0x9A9890, 0xB4B2A9 }),

        makePreset("lightblue", u8"天依蓝",
            { 0x66CCFF, 0x44AADD, 0x77D3FF, 0x55BBEE, 0x99DDFF, 0xE6F7FF, 0xD0EEFF,
              0x82D1FF, 0xA3DDFF, 0x1A4F6B, 0x70D4FF, 0xA0E0FF, 0x103447, 0xE0F5FF },
            { 0xFBFDFF, 0xE8F4FA, 0xF0F8FF, 0xE0EFF8, 0xDDECF6, 0xD0E3EF, 0xBFD8E8,
              0x282826, 0x585753, 0x82817A, 0x94928A, 0xAEACA3 }),

        makePreset("mint", u8"薄荷青",
            { 0x46B5A9, 0x2E8A80, 0x56C2B6, 0x3FA498, 0x9AD6CE, 0xEDF8F6, 0xD7EFEB,
              0x6FC4BA, 0x93D5CD, 0x14453F, 0x52BDB1, 0x85D2C8, 0x0B312C, 0xE0F3F0 },
            { 0xFBFDFD, 0xEDF5F4, 0xF2F8F7, 0xE7F0EF, 0xE4EEEC, 0xD8E6E4, 0xC3D6D3,
              0x2C2C2A, 0x5F5E5A, 0x888780, 0x9A9890, 0xB4B2A9 }),
    };
    return v;
}

// ---------------------------------------------------------------------------
//  QSS 字面量映射表：默认主题里写死的色值 → 角色。
//
//  ★ 这张表必须和各页 QSS 对着维护 ★ 新页面用了新的默认色值却不加进表里，
//  那个元素就不换肤（安全方向的失效 —— 不会崩、不会黑块，只是不跟主题）。
//  收录原则：跟着主题走的"品牌色 + 中性面"，不收"语义色"（危险红/成功绿
//  什么时候都不该被换成粉的）。
//  多个字面量可以指向同一个角色（三处差一像素的描边灰就是同一个 Border）。
// ---------------------------------------------------------------------------
struct LiteralRole
{
    const char*  literal;
    UiTheme::Role role;
};

const LiteralRole kLiteralMap[] = {
    { "#7F77DD", UiTheme::Accent          },
    { "#534AB7", UiTheme::AccentDeep      },
    { "#534AC0", UiTheme::AccentDeep      },   // PlayerPage 歌词高亮那档深紫
    { "#6E65D6", UiTheme::AccentHover     },
    { "#5F57C8", UiTheme::AccentPressed   },
    { "#9C95E6", UiTheme::AccentMid       },
    { "#B4B0E8", UiTheme::AccentMid2      },
    { "#AFA9EC", UiTheme::AccentSoft      },
    { "#EEEDFE", UiTheme::AccentSoftBg    },
    { "#E3E0FB", UiTheme::AccentSoftBg2   },
    { "#26215C", UiTheme::AccentText      },
    { "#66CCFF", UiTheme::ChatAccent      },
    { "#8FDAFF", UiTheme::ChatAccentHover },
    { "#042C53", UiTheme::OnAccent        },
    { "#E6F1FB", UiTheme::ChatAccentTint  },
    { "#F1EFE8", UiTheme::NavBg           },
    { "#F7F6F2", UiTheme::SurfaceAlt      },
    { "#EDEBE4", UiTheme::UserBubble      },
    { "#E8E6DF", UiTheme::HoverBg         },
    { "#E3E1D9", UiTheme::Border          },
    { "#E5E3DB", UiTheme::Border          },
    { "#E6E4DC", UiTheme::Border          },
    { "#D3D1C7", UiTheme::Disabled        },
    { "#2C2C2A", UiTheme::TextStrong      },
    { "#5F5E5A", UiTheme::TextMid         },
    { "#888780", UiTheme::TextSub         },
    { "#9A9890", UiTheme::TextFaint       },
    { "#8A8880", UiTheme::TextFaint       },
    { "#C9C7BF", UiTheme::TextDisabled    },
    { "#B4B2A9", UiTheme::TextDisabled    },
};

// ---------------------------------------------------------------------------
//  存档（和 UiFont 同一个 ui.ini，键前缀 theme/）
// ---------------------------------------------------------------------------
QSettings ini()
{
    // 返回纯右值靠 C++17 保证省略拷贝 —— 和 UiFont::ini() 同款写法。
    return QSettings(QSettings::IniFormat, QSettings::UserScope,
                     QStringLiteral("PetPal"), QStringLiteral("ui"));
}

// 内存态。-1 / 是否已加载 的套路和 UiFont 一样：第一次问的时候才读存档。
int     g_preset   = -1;
QString g_bgImage;
int     g_bgOpacity = -1;
bool    g_loaded    = false;

void ensureLoaded()
{
    if (g_loaded)
        return;
    g_loaded = true;

    const QSettings s = ini();

    // 主题：按 id 对号入座。存档里是老版本/手改坏的 id 时回落 0 号，
    // 绝不因为一个认不出的字符串就丢掉整套存档文件。
    const QString id = s.value(QStringLiteral("theme/id")).toString();
    g_preset = 0;
    if (!id.isEmpty())
    {
        for (int i = 0; i < presets().size(); ++i)
        {
            if (id == QLatin1String(presets().at(i).id))
            {
                g_preset = i;
                break;
            }
        }
    }

    g_bgImage   = s.value(QStringLiteral("theme/bgImage")).toString();
    g_bgOpacity = s.value(QStringLiteral("theme/bgOpacity"), 40).toInt();
    if (g_bgOpacity < 0 || g_bgOpacity > 100)
        g_bgOpacity = 40;
}

int clampIndex(int index)
{
    return (index >= 0 && index < presets().size()) ? index : 0;
}

// 角色颜色 → QSS 色值。不透明给 #RRGGBB（和页面里的写法一致，看着不突兀），
// 带透明的给 rgba() —— QSS 只认这两种写法。
QString colorString(const QColor& c)
{
    if (c.alpha() >= 255)
        return c.name(QColor::HexRgb);                 // #RRGGBB（大写）
    return QStringLiteral("rgba(%1,%2,%3,%4%)")
        .arg(c.red()).arg(c.green()).arg(c.blue())
        .arg(qRound(c.alphaF() * 100.0));
}

} // namespace

namespace UiTheme {

int presetCount()
{
    return presets().size();
}

const Preset& preset(int index)
{
    return presets().at(clampIndex(index));
}

int currentIndex()
{
    ensureLoaded();
    return g_preset;
}

QString currentName()
{
    return QString::fromUtf8(preset(currentIndex()).name);
}

QColor color(Role role)
{
    if (role < 0 || role >= RoleCount)
        return QColor(Qt::magenta);        // 编程错误要显眼，别悄悄给个白色
    return preset(currentIndex()).colors[role];
}

void setPreset(int index)
{
    ensureLoaded();
    g_preset = clampIndex(index);

    QSettings s = ini();
    s.setValue(QStringLiteral("theme/id"), QString::fromLatin1(presets().at(g_preset).id));
}

QString bgImagePath()
{
    ensureLoaded();
    return g_bgImage;
}

void setBgImagePath(const QString& absolutePath)
{
    ensureLoaded();
    g_bgImage = absolutePath;

    QSettings s = ini();
    s.setValue(QStringLiteral("theme/bgImage"), g_bgImage);
}

int bgOpacity()
{
    ensureLoaded();
    return g_bgOpacity;
}

void setBgOpacity(int percent)
{
    ensureLoaded();
    g_bgOpacity = qBound(0, percent, 100);

    QSettings s = ini();
    s.setValue(QStringLiteral("theme/bgOpacity"), g_bgOpacity);
}

qreal bgOpacityF()
{
    return qreal(bgOpacity()) / 100.0;
}

QString styleSheet(const QString& qss)
{
    QString out = qss;

    // 逐条字面量替换。数量少（30 条）× QSS 短（几 KB），只在套样式表的时候跑，
    // 一次几微秒 —— 不值得为此上正则合并。
    // Qt::CaseInsensitive：QSS 色值大小写都合法，别指望以后谁都写大写。
    for (const LiteralRole& lr : kLiteralMap)
        out.replace(QLatin1String(lr.literal), colorString(color(lr.role)),
                    Qt::CaseInsensitive);

    return out;
}

QString iniPath()
{
    const QSettings s = ini();
    return s.fileName();
}

QString describe()
{
    ensureLoaded();

    const QSettings s   = ini();
    const bool hasId      = s.contains(QStringLiteral("theme/id"));
    const bool hasImage   = s.contains(QStringLiteral("theme/bgImage"));
    const bool hasOpacity = s.contains(QStringLiteral("theme/bgOpacity"));
    const bool imgExists  = !g_bgImage.isEmpty() && QFile::exists(g_bgImage);

    QString out;
    out += QStringLiteral("当前主题 : %1（%2，共 %3 套预设）\r\n")
               .arg(QString::fromUtf8(presets().at(g_preset).name),
                    QString::fromLatin1(presets().at(g_preset).id))
               .arg(presets().size());
    out += hasId
               ? QStringLiteral("存档内容 : theme/id = %1\r\n").arg(s.value(QStringLiteral("theme/id")).toString())
               : QStringLiteral("存档内容 : 还没写过 theme/id —— 走默认 0 号，在设置页选一次就会存下来\r\n");

    out += g_bgImage.isEmpty()
               ? QStringLiteral("背景图片 : 未设置（纯主题色底）\r\n")
               : QStringLiteral("背景图片 : %1%2\r\n")
                     .arg(g_bgImage,
                          imgExists ? QString() : QStringLiteral("（文件已不在，显示时回落到纯色底）"));
    if (hasImage)
        out += QStringLiteral("存档内容 : theme/bgImage = %1\r\n").arg(s.value(QStringLiteral("theme/bgImage")).toString());

    out += QStringLiteral("图片不透明度 : %1%（存档 %2）\r\n")
               .arg(g_bgOpacity)
               .arg(hasOpacity ? QString::number(s.value(QStringLiteral("theme/bgOpacity")).toInt())
                               : QStringLiteral("未写"));
    out += QStringLiteral("生效方式 : 各页面 styleSheet() 换 QSS 色值 + MainPanel::paintEvent 画底色/背景图\r\n");
    out += QStringLiteral("存档文件 : %1\r\n").arg(iniPath());
    return out;
}

} // namespace UiTheme
