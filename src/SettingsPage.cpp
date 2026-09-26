#include "SettingsPage.h"
#include "AffectionSystem.h"
#include "PetConfig.h"
#include "UiFont.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QMessageBox>
#include <QSlider>
#include <QTimer>

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

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 16);
    root->setSpacing(12);

    auto* title = new QLabel(QStringLiteral("好感度"), this);
    title->setObjectName(QStringLiteral("cap"));
    root->addWidget(title);

    // ---------------- 卡片：重置好感度 ----------------
    auto* card = new QWidget(this);
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
    auto* fontCard = new QWidget(this);
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
    root->addStretch();

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

        /* 字号滑条：和播放器那两条一个路子 —— 细轨道 + 圆滑块，
           默认那套 Windows 滑条有三层立体边框，跟这页的扁平卡片不搭。 */
        QSlider#fontScale::groove:horizontal {
            height: 4px; background: #E3E1D9; border-radius: 2px;
        }
        QSlider#fontScale::sub-page:horizontal {
            height: 4px; background: #7F77DD; border-radius: 2px;
        }
        QSlider#fontScale::handle:horizontal {
            width: 14px; height: 14px; margin: -5px 0;
            border-radius: 7px; background: #7F77DD;
        }
        QSlider#fontScale::handle:horizontal:hover { background: #6E65D6; }

        QLabel#scaleValue { color: #534AB7; font-size: 13px; font-weight: 500; }
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
