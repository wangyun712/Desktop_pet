#include "SettingsPage.h"
#include "AffectionSystem.h"
#include "PetConfig.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QMessageBox>
#include <QTimer>

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
    root->addStretch();

    if (m_sys)
        connect(m_sys, &AffectionSystem::changed, this, &SettingsPage::refresh);

    refresh();
}

// =============================================================================
//  样式（配色跟好感度页保持一致）
// =============================================================================
void SettingsPage::applyStyle()
{
    setStyleSheet(QStringLiteral(R"(
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
    )"));
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
