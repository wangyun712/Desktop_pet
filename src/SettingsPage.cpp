#include "SettingsPage.h"
#include "AffectionSystem.h"
#include "DesktopLyrics.h"
#include "PetSay.h"
#include "PetConfig.h"
#include "UiFont.h"
#include "UiTheme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox>
#include <QMessageBox>
#include <QSlider>
#include <QTimer>
#include <QScrollArea>
#include <QButtonGroup>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QSettings>

namespace {

// 拖动字号滑条时，两次"真的让界面变"之间至少隔这么久（毫秒）。
//   ★ 为什么需要这个节流 ★
//     valueChanged 是每个像素来一次的。而"让界面变"这一步很贵：
//       · applyToApplication() 改 QApplication 默认字体 —— Qt 会给所有窗口发一次
//         字体变更事件，整棵控件树重新 polish + 重新布局；
//       · 再加上 DesktopPet 那边"六个页面一起重套样式表"。
//     一秒钟来几十次，界面当然跟不动鼠标。90ms ≈ 每秒最多 11 次，肉眼看还是连续变化的。
//   ★ 为什么是"限流"而不是纯防抖（只在停手后做一次）★
//     纯防抖在连续拖动期间会一直不刷新，屏幕上的字号整段路都冻着 —— 那更糟。
//     这里第一次改立刻见效，之后每 90ms 刷新一次，停手后还会补最后一次。
constexpr int kScaleApplyThrottleMs = 90;

} // namespace

// =============================================================================
//  构造
// =============================================================================
SettingsPage::SettingsPage(AffectionSystem* sys, QWidget* parent)
    : QWidget(parent), m_sys(sys)
{
    applyStyle();

    // ---- 整页包一个滚动区 ----
    // 三张卡片（重置 / 字号 / 主题外观）在面板默认高度下已经放不太下，字号调大
    // 之后更是必爆 —— 以前两张卡片时布局会悄悄把它们挤扁，现在有滚动区兜底。
    // 卡片一律挂在 m_host 上（滚动的是它），不是挂在这一页本身。
    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("setScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->viewport()->setAutoFillBackground(false);   // 透明滚到主题底色/背景图上

    m_host = new QWidget;
    m_host->setObjectName(QStringLiteral("setHost"));
    m_scroll->setWidget(m_host);          // ★ 别漏 ★ 漏了它 host 就是孤儿，整页空白

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(m_scroll);

    auto* root = new QVBoxLayout(m_host);
    root->setContentsMargins(20, 16, 20, 16);
    root->setSpacing(12);

    auto* title = new QLabel(QStringLiteral("好感度"), m_host);
    title->setObjectName(QStringLiteral("cap"));
    root->addWidget(title);

    // ---------------- 卡片：重置好感度 ----------------
    auto* card = new QWidget(m_host);
    card->setObjectName(QStringLiteral("card"));

    auto* cv = new QVBoxLayout(card);
    cv->setContentsMargins(14, 12, 14, 12);
    cv->setSpacing(6);

    auto* cardTitle = new QLabel(QStringLiteral("重置好感度"), card);
    cardTitle->setObjectName(QStringLiteral("cardTitle"));
    cv->addWidget(cardTitle);

    // 摘要：让用户知道"按下去会失去什么"，比只写一句"确定吗"有用
    m_summary = new QLabel(card);
    m_summary->setObjectName(QStringLiteral("hint"));
    m_summary->setWordWrap(true);
    cv->addWidget(m_summary);

    auto* note = new QLabel(QStringLiteral("清零后回到 Lv.1，累计互动次数归零，今日的摸摸 / 喂食次数也跟着重置。"
                                           "此操作无法撤销，存档会立刻被覆盖。"),
                            card);
    note->setObjectName(QStringLiteral("hint"));
    note->setWordWrap(true);
    cv->addWidget(note);

    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 4, 0, 0);
    row->setSpacing(10);

    m_btnReset = new QPushButton(QStringLiteral("重置…"), card);
    m_btnReset->setObjectName(QStringLiteral("danger"));
    m_btnReset->setCursor(Qt::PointingHandCursor);
    m_btnReset->setToolTip(QStringLiteral("先把好感度清零，之前会弹一个确认框"));
    connect(m_btnReset, &QPushButton::clicked, this, &SettingsPage::onResetClicked);
    row->addWidget(m_btnReset);

    m_feedback = new QLabel(card);          // 重置完在这儿闪一句"已重置"
    m_feedback->setObjectName(QStringLiteral("okHint"));
    row->addWidget(m_feedback);
    row->addStretch();
    cv->addLayout(row);

    root->addWidget(card);

    // ---------------- 卡片：界面字号 ----------------
    //  ★ 放在重置那张卡片下面，而且不挨着 ★
    //    上面那张是"删数据"，这张是"调外观"，两件事挨着放容易被顺手点到。
    auto* fontCard = new QWidget(m_host);
    fontCard->setObjectName(QStringLiteral("card"));

    auto* fv = new QVBoxLayout(fontCard);
    fv->setContentsMargins(14, 12, 14, 12);
    fv->setSpacing(6);

    auto* fontTitle = new QLabel(QStringLiteral("界面字号"), fontCard);
    fontTitle->setObjectName(QStringLiteral("cardTitle"));
    fv->addWidget(fontTitle);

    auto* fontHint = new QLabel(QStringLiteral("整个面板的字都跟着变，不用重启。"
                                              "调完会记住，下次打开还是这个大小。"), fontCard);
    fontHint->setObjectName(QStringLiteral("hint"));
    fontHint->setWordWrap(true);
    fv->addWidget(fontHint);

    auto* scaleRow = new QHBoxLayout;
    scaleRow->setContentsMargins(0, 4, 0, 0);
    scaleRow->setSpacing(12);

    m_fontScale = new QSlider(Qt::Horizontal, fontCard);
    m_fontScale->setObjectName(QStringLiteral("fontScale"));
    m_fontScale->setRange(UiFont::MIN_PERCENT, UiFont::MAX_PERCENT);
    m_fontScale->setSingleStep(UiFont::STEP_PERCENT);
    m_fontScale->setPageStep(UiFont::STEP_PERCENT * 2);
    m_fontScale->setCursor(Qt::PointingHandCursor);
    m_fontScale->setToolTip(QStringLiteral("拖动即可预览，松手后记住这个大小"));

    // 拖动时的节流定时器：见 onFontScaleChanged() 里的说明。
    // ★ 必须在 connect 之前建好 ★ —— onFontScaleChanged() 里要碰它。
    m_scaleApplyTimer = new QTimer(this);
    m_scaleApplyTimer->setSingleShot(true);
    m_scaleApplyTimer->setInterval(kScaleApplyThrottleMs);
    connect(m_scaleApplyTimer, &QTimer::timeout, this, &SettingsPage::onScaleApplyTick);

    // 构造时档位已经在 main 里生效过了，所以先记成"已经套上了"。
    m_scaleApplied = UiFont::scalePercent();

    m_fontScale->setValue(UiFont::scalePercent());   // 先设值再接信号，免得构造期间白发一次

    // 右边那行百分比。给个固定宽度，否则 "80%" 和 "150%" 宽度不同，
    // 拖动时滑条会跟着左右抽动。
    m_fontValue = new QLabel(fontCard);
    m_fontValue->setObjectName(QStringLiteral("scaleValue"));
    m_fontValue->setFixedWidth(46);
    m_fontValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    // ★ 值一变就记账 + 立刻落盘（只有一个信号）★
    //   这是"首选项"该有的样子：调完关掉程序，下次启动还是这个大小。
    //   落盘**必须**挂在 valueChanged 上，不能只挂 sliderReleased ——
    //   滚轮和方向键改值走的是 triggerAction()，没有按下/松手的过程，
    //   sliderReleased 永远不发，那条路上就成了"面板上改了、重启又变回去"。
    connect(m_fontScale, &QSlider::valueChanged, this, &SettingsPage::onFontScaleChanged);

    scaleRow->addWidget(m_fontScale, 1);
    scaleRow->addWidget(m_fontValue);
    fv->addLayout(scaleRow);

    refreshFontScaleLabel();

    root->addWidget(fontCard);

    // ---------------- 卡片：主题外观 ----------------
    //  ★ 放在最下面 ★ 它是"换个好看"的锦上添花，前两张（删数据 / 调字号）
    //    才是"出问题了要来找"的入口，顺序就照这个优先级排。
    auto* themeCard = new QWidget(m_host);
    themeCard->setObjectName(QStringLiteral("card"));

    auto* tv = new QVBoxLayout(themeCard);
    tv->setContentsMargins(14, 12, 14, 12);
    tv->setSpacing(6);

    auto* themeTitle = new QLabel(QStringLiteral("主题外观"), themeCard);
    themeTitle->setObjectName(QStringLiteral("cardTitle"));
    tv->addWidget(themeTitle);

    auto* themeHint = new QLabel(QStringLiteral("换一套配色，或者放一张喜欢的图当背景。"
                                                "设置会记住，下次打开软件还是这样。"), themeCard);
    themeHint->setObjectName(QStringLiteral("hint"));
    themeHint->setWordWrap(true);
    tv->addWidget(themeHint);

    // ---- 色板行：一块圆片一个预设主题 ----
    //  色片的底色直接用该主题的强调色 —— 摆在那里就是"选我之后大概长这样"。
    //  ★ 每块的样式单独 set，不走 UiFont::styleSheet() 网关 ★
    //    网关做的是"默认主题字面量 → 当前主题"的替换，色片要展示的是
    //    **各预设自己的**颜色，过一遍网关会被换成当前主题色，六块全变成一个色。
    auto* swatchRow = new QHBoxLayout;
    swatchRow->setContentsMargins(0, 4, 0, 0);
    swatchRow->setSpacing(8);

    m_swatchGroup = new QButtonGroup(this);   // 默认互斥：同一时间只有一块是选中的
    for (int i = 0; i < UiTheme::presetCount(); ++i)
    {
        const UiTheme::Preset& ps = UiTheme::preset(i);

        auto* swatch = new QPushButton(themeCard);
        swatch->setCheckable(true);
        swatch->setFixedSize(26, 26);
        swatch->setCursor(Qt::PointingHandCursor);
        swatch->setToolTip(QString::fromUtf8(ps.name));
        // 选中/悬停时的描边用该主题的"强调文字色"（深色），任何底色上都看得清
        swatch->setStyleSheet(QStringLiteral(
            "QPushButton { background:%1; border:2px solid transparent; border-radius:13px; padding:0; }"
            "QPushButton:hover   { border:2px solid %2; }"
            "QPushButton:checked { border:2px solid %2; }")
                .arg(ps.colors[UiTheme::Accent].name(QColor::HexRgb),
                     ps.colors[UiTheme::AccentText].name(QColor::HexRgb)));

        m_swatchGroup->addButton(swatch, i);
        m_swatches.append(swatch);
        swatchRow->addWidget(swatch);
    }
    connect(m_swatchGroup, &QButtonGroup::idClicked, this, &SettingsPage::onPresetSelected);

    swatchRow->addStretch();

    m_themeName = new QLabel(themeCard);
    m_themeName->setObjectName(QStringLiteral("scaleValue"));   // 和字号百分比同款强调色
    tv->addLayout(swatchRow);
    tv->addWidget(m_themeName);

    // ---- 背景图行 ----
    auto* bgRow = new QHBoxLayout;
    bgRow->setContentsMargins(0, 4, 0, 0);
    bgRow->setSpacing(8);

    m_btnImportBg = new QPushButton(QStringLiteral("导入背景图片…"), themeCard);
    m_btnImportBg->setObjectName(QStringLiteral("themeBtn"));
    m_btnImportBg->setCursor(Qt::PointingHandCursor);
    m_btnImportBg->setToolTip(QStringLiteral("选一张图铺在面板底层，卡片和文字不受影响"));
    connect(m_btnImportBg, &QPushButton::clicked, this, &SettingsPage::onImportBg);
    bgRow->addWidget(m_btnImportBg);

    m_btnClearBg = new QPushButton(QStringLiteral("清除图片"), themeCard);
    m_btnClearBg->setObjectName(QStringLiteral("themeBtn"));
    m_btnClearBg->setCursor(Qt::PointingHandCursor);
    m_btnClearBg->setToolTip(QStringLiteral("不想要背景图了就点这个，回到纯主题色底"));
    connect(m_btnClearBg, &QPushButton::clicked, this, &SettingsPage::onClearBg);
    bgRow->addWidget(m_btnClearBg);

    m_bgFile = new QLabel(themeCard);
    m_bgFile->setObjectName(QStringLiteral("hint"));
    bgRow->addWidget(m_bgFile, 1);
    tv->addLayout(bgRow);

    // ---- 不透明度行 ----
    //  0% = 完全看不见图（等于没设），100% = 原图本色。默认 40%：
    //  导入一张图直接是"隐约透出一层"的效果，不用人人再动手调。
    auto* opacityRow = new QHBoxLayout;
    opacityRow->setContentsMargins(0, 4, 0, 0);
    opacityRow->setSpacing(12);

    m_bgOpacity = new QSlider(Qt::Horizontal, themeCard);
    m_bgOpacity->setObjectName(QStringLiteral("themeSlider"));
    m_bgOpacity->setRange(0, 100);
    m_bgOpacity->setCursor(Qt::PointingHandCursor);
    m_bgOpacity->setToolTip(QStringLiteral("背景图有多明显。拖动实时预览，调完记住"));

    // 先设值再接信号 —— 构造期间不用发 panelBackgroundChanged。
    m_bgOpacity->setValue(UiTheme::bgOpacity());
    connect(m_bgOpacity, &QSlider::valueChanged, this, &SettingsPage::onBgOpacityChanged);

    m_opacityValue = new QLabel(themeCard);
    m_opacityValue->setObjectName(QStringLiteral("scaleValue"));
    m_opacityValue->setFixedWidth(46);
    m_opacityValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    opacityRow->addWidget(new QLabel(QStringLiteral("图片不透明度"), themeCard));
    opacityRow->addWidget(m_bgOpacity, 1);
    opacityRow->addWidget(m_opacityValue);
    tv->addLayout(opacityRow);

    root->addWidget(themeCard);

    // ---------------- 卡片：桌面歌词 ----------------
    //  开关只是"意图"：落盘（DesktopLyrics::saveEnabled）+ 发信号，真正
    //  显隐悬浮窗的是 PlayerPage（它才有歌词和进度数据）。
    auto* dtCard = new QWidget(m_host);
    dtCard->setObjectName(QStringLiteral("card"));

    auto* dv = new QVBoxLayout(dtCard);
    dv->setContentsMargins(14, 12, 14, 12);
    dv->setSpacing(6);

    auto* dtTitle = new QLabel(QStringLiteral("桌面歌词"), dtCard);
    dtTitle->setObjectName(QStringLiteral("cardTitle"));
    dv->addWidget(dtTitle);

    auto* dtHint = new QLabel(QStringLiteral("像网易云那样，把正在唱的歌词浮在桌面上。"
                                              "鼠标移到歌词条上会出现播放控制、锁定和关闭按钮，"
                                              "按住可拖到任意位置，位置会记住。"
                                              "锁定后双击歌词解锁，三连击唤出主面板；"
                                              "点 × 关闭后，回到这里重新勾选就能再开。"), dtCard);
    dtHint->setObjectName(QStringLiteral("hint"));
    dtHint->setWordWrap(true);
    dv->addWidget(dtHint);

    m_dtLyrics = new QCheckBox(QStringLiteral("在桌面上显示当前歌词"), dtCard);
    m_dtLyrics->setObjectName(QStringLiteral("dtLyricsToggle"));
    m_dtLyrics->setCursor(Qt::PointingHandCursor);
    // 先设状态再接信号 —— 构造期间不发 toggled
    m_dtLyrics->setChecked(DesktopLyrics::loadEnabled());
    connect(m_dtLyrics, &QCheckBox::toggled, this, &SettingsPage::onDesktopLyricsToggled);
    dv->addWidget(m_dtLyrics);

    root->addWidget(dtCard);

    // ---------------- 卡片：桌宠互动 ----------------
    //  开关只负责"落盘 + 喊一声"：气泡的显隐和台词挑选都在 DesktopPet
    //  （它持有 PetSay::Picker 和气泡窗）。
    auto* sayCard = new QWidget(m_host);
    sayCard->setObjectName(QStringLiteral("card"));

    auto* sv = new QVBoxLayout(sayCard);
    sv->setContentsMargins(14, 12, 14, 12);
    sv->setSpacing(6);

    auto* sayTitle = new QLabel(QStringLiteral("桌宠互动"), sayCard);
    sayTitle->setObjectName(QStringLiteral("cardTitle"));
    sv->addWidget(sayTitle);

    auto* sayHint = new QLabel(QStringLiteral("被点、被拎起来、落地、整点报时的时候，"
                                              "桌宠会在头顶冒一句台词，"
                                              "拖一首歌给它也会报歌名。"), sayCard);
    sayHint->setObjectName(QStringLiteral("hint"));
    sayHint->setWordWrap(true);
    sv->addWidget(sayHint);

    m_petSay = new QCheckBox(QStringLiteral("让桌宠在桌面上说话"), sayCard);
    m_petSay->setObjectName(QStringLiteral("dtLyricsToggle"));   // 复用同一套指示器样式
    m_petSay->setCursor(Qt::PointingHandCursor);
    // 先设状态再接信号 —— 构造期间不发 toggled
    m_petSay->setChecked(PetSay::loadEnabled());
    connect(m_petSay, &QCheckBox::toggled, this, &SettingsPage::onPetSayToggled);
    sv->addWidget(m_petSay);

    root->addWidget(sayCard);

    // ---------------- 卡片：桌宠大小 ----------------
    //  滑条只负责"喊一声"（petScaleChanged），落盘和缩放都在 DesktopPet
    //  （缩放牵连窗口尺寸/绘制/命中检测，全在它那边）。
    auto* scaleCard = new QWidget(m_host);
    scaleCard->setObjectName(QStringLiteral("card"));

    auto* sc = new QVBoxLayout(scaleCard);
    sc->setContentsMargins(14, 12, 14, 12);
    sc->setSpacing(6);

    auto* scaleTitle = new QLabel(QStringLiteral("桌宠大小"), scaleCard);
    scaleTitle->setObjectName(QStringLiteral("cardTitle"));
    sc->addWidget(scaleTitle);

    auto* scaleHint = new QLabel(QStringLiteral("把桌宠本体放大或缩小，走路、睡觉、"
                                                "被拎起来都跟着缩，点击照样精准。"
                                                "拖动立即生效，会记住。"), scaleCard);
    scaleHint->setObjectName(QStringLiteral("hint"));
    scaleHint->setWordWrap(true);
    sc->addWidget(scaleHint);

    auto* petScaleRow = new QHBoxLayout;
    petScaleRow->setContentsMargins(0, 4, 0, 0);
    petScaleRow->setSpacing(12);

    m_petScaleSlider = new QSlider(Qt::Horizontal, scaleCard);
    m_petScaleSlider->setObjectName(QStringLiteral("petScaleSlider"));
    m_petScaleSlider->setRange(50, 200);
    m_petScaleSlider->setSingleStep(10);
    m_petScaleSlider->setPageStep(20);
    m_petScaleSlider->setCursor(Qt::PointingHandCursor);
    m_petScaleSlider->setToolTip(QStringLiteral("拖动立即生效，松手就记住"));

    // 先设值再接信号 —— 构造期间不发 petScaleChanged
    m_petScaleSlider->setValue(QSettings(QSettings::IniFormat, QSettings::UserScope,
                                         QStringLiteral("PetPal"), QStringLiteral("ui"))
                                   .value(QStringLiteral("petScale/percent"), 100).toInt());
    connect(m_petScaleSlider, &QSlider::valueChanged, this, &SettingsPage::onPetScaleChanged);

    m_petScaleValue = new QLabel(scaleCard);
    m_petScaleValue->setObjectName(QStringLiteral("scaleValue"));
    m_petScaleValue->setFixedWidth(46);
    m_petScaleValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_petScaleValue->setText(QStringLiteral("%1%").arg(m_petScaleSlider->value()));

    petScaleRow->addWidget(new QLabel(QStringLiteral("缩放"), scaleCard));
    petScaleRow->addWidget(m_petScaleSlider, 1);
    petScaleRow->addWidget(m_petScaleValue);
    sc->addLayout(petScaleRow);

    root->addWidget(scaleCard);
    root->addStretch();

    // 色板/文件名/滑条可用态刷成 UiTheme 的当前值（也把选中块点亮）
    refreshThemeRow();

    if (m_sys)
        connect(m_sys, &AffectionSystem::changed, this, &SettingsPage::refresh);

    refresh();
}

// =============================================================================
//  样式（配色跟好感度页保持一致）
//
//  ★ 整段包在 UiFont::styleSheet() 里 ★
//    它按当前档位把每个 `font-size: Npx` 改写一遍（见 UiFont.h）——
//    所以这里照 100% 写死值就行，不用到处乘系数。
//    这也意味着"字号一变，这一页自己也得重套一次"，见 applyUiScale()。
// =============================================================================
void SettingsPage::applyStyle()
{
    setStyleSheet(UiFont::styleSheet(QStringLiteral(R"(
        QScrollArea#setScroll { background: transparent; border: none; }
        QWidget#setHost { background: transparent; }

        QLabel#cap       { color: #888780; font-size: 12px; }
        QWidget#card     { background: #F1EFE8; border-radius: 8px; }
        QLabel#cardTitle { color: #2C2C2A; font-size: 14px; font-weight: 500; }
        QLabel#hint      { color: #888780; font-size: 12px; }
        QLabel#okHint    { color: #3B6D11; font-size: 12px; }
        QPushButton#danger {
            background: transparent; color: #A32D2D;
            border: 1px solid #E0A8A8; border-radius: 8px;
            padding: 6px 16px; font-size: 13px;
        }
        QPushButton#danger:hover   { background: #FCEBEB; border-color: #C97B7B; }
        QPushButton#danger:pressed { background: #F7DADA; }

        QPushButton#themeBtn {
            background: transparent; color: #534AB7;
            border: 1px solid #AFA9EC; border-radius: 8px;
            padding: 5px 12px; font-size: 12px;
        }
        QPushButton#themeBtn:hover    { background: #EEEDFE; border-color: #534AB7; }
        QPushButton#themeBtn:disabled { color: #888780; border-color: #D3D1C7; }

        /* 字号滑条：和播放器那两条一个路子 —— 细轨道 + 圆滑块，
           默认那套 Windows 滑条有三层立体边框，跟这页的扁平卡片不搭。 */
        QSlider#fontScale::groove:horizontal, QSlider#themeSlider::groove:horizontal,
        QSlider#petScaleSlider::groove:horizontal {
            height: 4px; background: #E3E1D9; border-radius: 2px;
        }
        QSlider#fontScale::sub-page:horizontal, QSlider#themeSlider::sub-page:horizontal,
        QSlider#petScaleSlider::sub-page:horizontal {
            height: 4px; background: #7F77DD; border-radius: 2px;
        }
        QSlider#fontScale::handle:horizontal, QSlider#themeSlider::handle:horizontal,
        QSlider#petScaleSlider::handle:horizontal {
            width: 14px; height: 14px; margin: -5px 0;
            border-radius: 7px; background: #7F77DD;
        }
        QSlider#fontScale::handle:horizontal:hover,
        QSlider#themeSlider::handle:horizontal:hover,
        QSlider#petScaleSlider::handle:horizontal:hover { background: #6E65D6; }

        QLabel#scaleValue { color: #534AB7; font-size: 13px; font-weight: 500; }

        /* 滚动条收细，别让一根默认大灰条压在卡片上面 */
        QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }
        QScrollBar::handle:vertical {
            background: #D3D1C7; border-radius: 3px; min-height: 30px;
        }
        QScrollBar::handle:vertical:hover { background: #B9B7AE; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }

        /* 桌面歌词开关：方圆角指示器，选中填强调色 —— 不贴图，纯色块就很干净 */
        QCheckBox#dtLyricsToggle {
            color: #2C2C2A; font-size: 13px; spacing: 8px;
        }
        QCheckBox#dtLyricsToggle::indicator {
            width: 17px; height: 17px;
            border: 1px solid #D3D1C7; border-radius: 4px; background: #FFFFFF;
        }
        QCheckBox#dtLyricsToggle::indicator:hover   { border-color: #7F77DD; }
        QCheckBox#dtLyricsToggle::indicator:checked {
            background: #7F77DD; border-color: #7F77DD;
        }
    )")));
}

// =============================================================================
//  界面字号
// =============================================================================
void SettingsPage::refreshFontScaleLabel()
{
    if (m_fontValue)
        m_fontValue->setText(QStringLiteral("%1%").arg(UiFont::scalePercent()));
}

void SettingsPage::applyUiScale()
{
    // 档位已经改好了，这里只把"这一页的样子"刷新一遍。
    // 滑条值不用重设 —— 改档位的就是它自己。
    applyStyle();
    refreshFontScaleLabel();
}

void SettingsPage::onFontScaleChanged(int percent)
{
    // ① 记账：把档位记下来。这一步很便宜（内存里一个整数）。
    //    ★ 用 Quiet 版而不是 setScalePercent() ★ —— 后者会顺手改
    //      QApplication 默认字体，也就是把"最贵的那一步"做了，
    //      而拖动时它每秒会被要求做几十次（见 kScaleApplyThrottleMs 的说明）。
    UiFont::setScalePercentQuiet(percent);

    // ② 立刻落盘 —— "调一次，以后每次启动都是这个大小"就靠这一行。
    //    只做 ① 的话关掉程序就丢了，重启又是老大小（那种 bug 看着像"设置没保存"）。
    //    ★ 落盘不参与节流：万一用户拖完马上关程序，存下来的也一定是最后那个值。★
    UiFont::saveScalePercent();

    // ③ 右边那行百分比立刻跟着走 —— 不然拖动时数字是死的，看着像没反应。
    refreshFontScaleLabel();

    // ④ 让界面真的变（贵的那一步）—— 限流。
    //    为什么这里可以直接 return：节流窗口里攒下的档位已经记在 UiFont 里了，
    //    定时器到点时会读"当前档位"再补一次，所以最后一次改动永远不会被吞掉。
    if (m_scaleApplyTimer->isActive())
        return;

    applyScaleNow();
    m_scaleApplyTimer->start();
}

void SettingsPage::onScaleApplyTick()
{
    // 这一轮拖动攒下来的最后一个档位，在这里补上。
    // 档位没变的话 applyScaleNow() 自己会跳过，所以这里不用判断。
    applyScaleNow();
}

void SettingsPage::applyScaleNow()
{
    const int pct = UiFont::scalePercent();
    if (pct == m_scaleApplied)
        return;      // 这个档位已经套到界面上了，别白刷一遍（六页重套不便宜）

    m_scaleApplied = pct;

    // ① 默认字体（没写 font-size 的控件走这条）
    UiFont::applyToApplication();

    // ② 喊一声，让面板和各个页面重套自己的样式表。
    //    UiFont 只能改"以后套上去的样式表"，已经在屏幕上的控件不会自己变。
    //    接线在 DesktopPet::openPanel() 里（那边才拿得到所有页面的指针）。
    emit uiScaleChanged();
}

// =============================================================================
//  主题外观
//
//  四个动作的分工都是同一个模式：UiTheme 负责"记 + 落盘"，这一页负责
//  "刷新自己 + 发信号喊界面变"。UiTheme 不碰任何控件（见 UiTheme.h）。
// =============================================================================
void SettingsPage::onPresetSelected(int index)
{
    // UiTheme::setPreset 里没有"没变就跳过" —— 这里挡一道：
    // 重复点同一块色板时，六页重套样式表是白做的。
    if (index == UiTheme::currentIndex())
        return;

    UiTheme::setPreset(index);
    refreshThemeRow();
    emit themeChanged();      // DesktopPet：逐页 applyUiScale()（字号变更同一条路）
}

void SettingsPage::onImportBg()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择背景图片"),
        QString(),
        QStringLiteral("图片 (*.png *.jpg *.jpeg *.bmp *.webp)"));
    if (path.isEmpty())
        return;

    UiTheme::setBgImagePath(path);
    refreshThemeRow();
    emit panelBackgroundChanged();   // DesktopPet：m_panel->update()，只重画面板
}

void SettingsPage::onClearBg()
{
    UiTheme::setBgImagePath(QString());
    refreshThemeRow();
    emit panelBackgroundChanged();
}

void SettingsPage::onBgOpacityChanged(int percent)
{
    // 拖动中每个 tick 都会来一次：UiTheme 里只是"记一个整数 + 写几行 ini"，
    // 界面那边只是面板 update() 重画一次 —— 都便宜，不用节流。
    UiTheme::setBgOpacity(percent);
    m_opacityValue->setText(QStringLiteral("%1%").arg(percent));

    // ★ 不发这声，改动就只是"记下来了" ★ —— 面板不知道要重画，
    //   得等哪次切页触发整体重绘才"顺带"生效，看起来就像滑条坏了。
    emit panelBackgroundChanged();
}

void SettingsPage::onDesktopLyricsToggled(bool on)
{
    DesktopLyrics::saveEnabled(on);    // 开关自己的事自己落盘（键在 ui.ini）
    emit desktopLyricsToggled(on);     // 显隐悬浮窗是 PlayerPage 的事（它有歌词数据）
}

void SettingsPage::refreshDesktopLyricsToggle(bool on)
{
    // 反向同步（悬浮窗 × 关闭 → 勾选框取消）。值真的变了才 setChecked：
    // 触发的 toggled 链走一圈落盘/广播后，到这里发现值相同就停了，不成环。
    if (m_dtLyrics && m_dtLyrics->isChecked() != on)
        m_dtLyrics->setChecked(on);
}

void SettingsPage::onPetSayToggled(bool on)
{
    PetSay::saveEnabled(on);           // 键在 ui.ini（petSay/enabled），默认开
    emit petSayToggled(on);            // 气泡显隐和台词挑选是 DesktopPet 的事
}

void SettingsPage::onPetScaleChanged(int percent)
{
    // 滑条只改数字 + 喊一声：落盘和缩放都在 DesktopPet::setPetScale 里
    m_petScaleValue->setText(QStringLiteral("%1%").arg(percent));
    emit petScaleChanged(percent);
}

void SettingsPage::refreshThemeRow()
{
    const int idx = UiTheme::currentIndex();

    for (int i = 0; i < m_swatches.size(); ++i)
    {
        if (m_swatches.at(i))
            m_swatches.at(i)->setChecked(i == idx);
    }
    if (m_themeName)
        m_themeName->setText(UiTheme::currentName());

    // 背景图行的可用态：文件在才有得清、才有得调透明度。
    // 文件丢了不算错 —— 提示一句，画图那边自然会回落到纯色底。
    const QString img = UiTheme::bgImagePath();
    const bool has    = !img.isEmpty() && QFile::exists(img);

    if (m_btnClearBg)
        m_btnClearBg->setEnabled(has);
    if (m_bgOpacity)
        m_bgOpacity->setEnabled(has);
    if (m_opacityValue)
        m_opacityValue->setText(QStringLiteral("%1%").arg(UiTheme::bgOpacity()));

    if (m_bgFile)
    {
        if (img.isEmpty())
            m_bgFile->setText(QStringLiteral("未设置背景图"));
        else if (has)
        {
            m_bgFile->setText(QFileInfo(img).fileName());
            m_bgFile->setToolTip(img);      // 完整路径收进 tooltip，行宽留给文件名
        }
        else
        {
            m_bgFile->setText(QStringLiteral("图片文件不在了，请重新导入"));
            m_bgFile->setToolTip(img);
        }
    }
}

// =============================================================================
//  摘要
// =============================================================================
void SettingsPage::refresh()
{
    if (!m_sys)
        return;

    m_summary->setText(QStringLiteral("当前 Lv.%1 / %2 %3\u3000·\u3000累计 %4 点\u3000·\u3000陪伴 %5 天")
                           .arg(m_sys->level())
                           .arg(PetCfg::AFF_MAX_LEVEL)
                           .arg(m_sys->stageName())
                           .arg(qRound(m_sys->point()))
                           .arg(m_sys->companyDays()));
}

// =============================================================================
//  确认框
//
//  用 addButton 而不是 setStandardButtons：这样才能把按钮文字写成中文，
//  并且明确区分"哪个是危险按钮"。
// =============================================================================
QMessageBox* SettingsPage::makeResetConfirmBox(QWidget* parent)
{
    auto* box = new QMessageBox(parent);
    box->setIcon(QMessageBox::Warning);
    box->setWindowTitle(QStringLiteral("重置好感度"));
    box->setText(QStringLiteral("确定要把好感度清零吗？"));
    box->setInformativeText(QStringLiteral("等级、阶段和累计互动次数都会回到最初，且无法撤销。"));

    box->addButton(QStringLiteral("重置"), QMessageBox::DestructiveRole);
    QPushButton* cancelBtn = box->addButton(QStringLiteral("取消"), QMessageBox::RejectRole);

    // ★ 破坏性操作的两道保险：回车和 Esc 都落到「取消」★
    //   少了这两行，用户手快敲一下回车就把数据删了。
    box->setDefaultButton(cancelBtn);
    box->setEscapeButton(cancelBtn);

    return box;
}

// =============================================================================
//  按下「重置…」
// =============================================================================
void SettingsPage::onResetClicked()
{
    QMessageBox* box = makeResetConfirmBox(this);
    box->exec();

    // 用"按钮的角色"判断按了哪个，而不是存一个按钮指针 —— 指针要一路传出来，
    // 多一个容易出错的接线。点右上角关掉窗口时 clickedButton() 是 nullptr，
    // 所以先判空。
    QAbstractButton* clicked = box->clickedButton();
    const bool confirmed = clicked && box->buttonRole(clicked) == QMessageBox::DestructiveRole;
    box->deleteLater();

    if (!confirmed)
        return;                       // 取消 / 直接关掉：什么都不做

    emit resetAffectionRequested();

    // 面板本身不显示好感度数字，只在设置页里闪一句，让用户确认"确实按到了"
    m_feedback->setText(QStringLiteral("已重置"));
    QTimer::singleShot(2500, this, [this]() {
        if (m_feedback)
            m_feedback->clear();
    });
}
