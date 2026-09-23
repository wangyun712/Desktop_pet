#include "AffectionPage.h"
#include "AffectionSystem.h"
#include "PetConfig.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QPixmap>
#include <QTimer>

// =============================================================================
//  构造
// =============================================================================
AffectionPage::AffectionPage(AffectionSystem* sys, QWidget* parent)
    : QWidget(parent), m_sys(sys)
{
    applyStyle();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 16);
    root->setSpacing(0);

    // ---------------- 头像 + 等级 ----------------
    auto* head = new QHBoxLayout;
    head->setSpacing(12);

    m_avatar = new QLabel(this);
    m_avatar->setObjectName(QStringLiteral("avatar"));
    m_avatar->setFixedSize(44, 44);
    m_avatar->setAlignment(Qt::AlignCenter);
    // 头像直接用待机那张图（编号 8），和桌宠站着的时候一模一样。
    // 素材画布是 183x252，只按高度缩放，宽度自然出来，不会拉变形。
    m_avatar->setPixmap(QPixmap(petImagePath(8)).scaledToHeight(40, Qt::SmoothTransformation));
    head->addWidget(m_avatar);

    auto* nameBox = new QVBoxLayout;
    nameBox->setSpacing(2);
    auto* name = new QLabel(QStringLiteral("PetPal"), this);
    name->setObjectName(QStringLiteral("name"));
    m_levelText = new QLabel(QStringLiteral("-"), this);
    m_levelText->setObjectName(QStringLiteral("level"));
    nameBox->addWidget(name);
    nameBox->addWidget(m_levelText);
    head->addLayout(nameBox);
    head->addStretch();

    root->addLayout(head);
    root->addSpacing(18);

    // ---------------- 进度条 ----------------
    auto* barTop = new QHBoxLayout;
    auto* barCap = new QLabel(QStringLiteral("好感度"), this);
    barCap->setObjectName(QStringLiteral("cap"));
    m_progressText = new QLabel(QStringLiteral("- / -"), this);
    m_progressText->setObjectName(QStringLiteral("cap"));
    barTop->addWidget(barCap);
    barTop->addStretch();
    barTop->addWidget(m_progressText);
    root->addLayout(barTop);
    root->addSpacing(6);

    m_bar = new QProgressBar(this);
    m_bar->setObjectName(QStringLiteral("affBar"));
    m_bar->setFixedHeight(8);
    m_bar->setTextVisible(false);
    m_bar->setRange(0, 100);
    m_bar->setValue(0);
    root->addWidget(m_bar);
    root->addSpacing(16);

    // ---------------- 三个统计 ----------------
    auto* metrics = new QHBoxLayout;
    metrics->setSpacing(8);
    metrics->addWidget(buildMetric(QStringLiteral("今日好感"), &m_todayValue));
    metrics->addWidget(buildMetric(QStringLiteral("陪伴"),     &m_daysValue));
    metrics->addWidget(buildMetric(QStringLiteral("累计互动"), &m_totalValue));
    root->addLayout(metrics);
    root->addSpacing(16);

    // ---------------- 三个操作按钮 ----------------
    auto* btns = new QHBoxLayout;
    btns->setSpacing(8);

    m_btnPet = new QPushButton(QStringLiteral("摸摸"), this);
    m_btnPet->setObjectName(QStringLiteral("act"));
    m_btnPet->setCursor(Qt::PointingHandCursor);
    connect(m_btnPet, &QPushButton::clicked, this, &AffectionPage::petPetted);

    m_btnFeed = new QPushButton(QStringLiteral("喂食"), this);
    m_btnFeed->setObjectName(QStringLiteral("act"));
    m_btnFeed->setCursor(Qt::PointingHandCursor);
    connect(m_btnFeed, &QPushButton::clicked, this, &AffectionPage::feedRequested);

    m_btnChat = new QPushButton(QStringLiteral("聊聊天"), this);
    m_btnChat->setObjectName(QStringLiteral("actGhost"));
    m_btnChat->setCursor(Qt::PointingHandCursor);
    connect(m_btnChat, &QPushButton::clicked, this, &AffectionPage::chatRequested);

    btns->addWidget(m_btnPet);
    btns->addWidget(m_btnFeed);
    btns->addWidget(m_btnChat);
    btns->addStretch();
    root->addLayout(btns);
    root->addSpacing(8);

    // ---------------- 今日额度 ----------------
    auto* quota = new QHBoxLayout;
    m_petQuota = new QLabel(this);
    m_petQuota->setObjectName(QStringLiteral("hint"));
    m_feedQuota = new QLabel(this);
    m_feedQuota->setObjectName(QStringLiteral("hint"));
    quota->addWidget(m_petQuota);
    quota->addStretch();
    quota->addWidget(m_feedQuota);
    root->addLayout(quota);

    root->addStretch();

    // ---------------- 底部提示 ----------------
    m_hint = new QLabel(this);
    m_hint->setObjectName(QStringLiteral("hint"));
    m_hint->setWordWrap(true);
    root->addWidget(m_hint);

    // 数字一变就重画
    connect(m_sys, &AffectionSystem::changed, this, &AffectionPage::refresh);
    connect(m_sys, &AffectionSystem::leveledUp, this, &AffectionPage::onLeveledUp);

    refresh();
}

// =============================================================================
//  统计小卡片
// =============================================================================
QWidget* AffectionPage::buildMetric(const QString& caption, QLabel** valueOut)
{
    auto* card = new QWidget(this);
    card->setObjectName(QStringLiteral("metric"));

    auto* v = new QVBoxLayout(card);
    v->setContentsMargins(10, 8, 10, 8);
    v->setSpacing(2);

    auto* cap = new QLabel(caption, card);
    cap->setObjectName(QStringLiteral("metricCap"));

    auto* val = new QLabel(QStringLiteral("-"), card);
    val->setObjectName(QStringLiteral("metricVal"));

    v->addWidget(cap);
    v->addWidget(val);

    *valueOut = val;
    return card;
}

// =============================================================================
//  样式
// =============================================================================
void AffectionPage::applyStyle()
{
    setStyleSheet(QStringLiteral(R"(
        QLabel#avatar   { background: #F1EFE8; border-radius: 8px; }
        QLabel#name     { color: #2C2C2A; font-size: 15px; font-weight: 500; }
        QLabel#level    { color: #5F5E5A; font-size: 13px; }
        QLabel#cap      { color: #888780; font-size: 12px; }
        QLabel#hint     { color: #888780; font-size: 12px; }
        QWidget#metric  { background: #F1EFE8; border-radius: 8px; }
        QLabel#metricCap{ color: #888780; font-size: 12px; }
        QLabel#metricVal{ color: #2C2C2A; font-size: 15px; font-weight: 500; }
        QProgressBar#affBar {
            background: #F1EFE8; border: none; border-radius: 4px;
        }
        QProgressBar#affBar::chunk { background: #7F77DD; border-radius: 4px; }
        QPushButton#act {
            background: #7F77DD; color: #FFFFFF; border: none;
            border-radius: 8px; padding: 7px 16px; font-size: 13px;
        }
        QPushButton#act:hover { background: #534AB7; }
        QPushButton#act:disabled { background: #D3D1C7; color: #888780; }
        QPushButton#actGhost {
            background: transparent; color: #534AB7; border: 1px solid #AFA9EC;
            border-radius: 8px; padding: 7px 16px; font-size: 13px;
        }
        QPushButton#actGhost:hover { background: #EEEDFE; }
    )"));
}

// =============================================================================
//  刷新：所有数字都从 AffectionSystem 现取
// =============================================================================
void AffectionPage::refresh()
{
    if (!m_sys)
        return;

    const int    lv      = m_sys->level();
    const double gained  = m_sys->levelGained();
    const double need    = m_sys->levelNeed();
    const double toNext  = qMax(0.0, m_sys->levelCeil() - m_sys->point());

    m_levelText->setText(QStringLiteral("Lv.%1\u3000%2").arg(lv).arg(m_sys->stageName()));

    m_progressText->setText(QStringLiteral("%1 / %2")
                                .arg(qRound(gained))
                                .arg(qRound(need)));

    // 进度条按"本级需要的点数"当满格。need 理论上不会为 0，保险起见兜个底。
    m_bar->setRange(0, qMax(1, int(qRound(need))));
    m_bar->setValue(qBound(0, int(qRound(gained)), qMax(1, int(qRound(need)))));

    const double today = m_sys->todayGain();
    m_todayValue->setText(today > 0.0 ? QStringLiteral("+%1").arg(qRound(today))
                                      : QStringLiteral("0"));
    m_daysValue->setText(QStringLiteral("%1 天").arg(m_sys->companyDays()));
    m_totalValue->setText(QStringLiteral("%1 次").arg(m_sys->totalInteractions()));

    // ---- 每日额度：用完就把按钮灰掉，别让人以为还能点 ----
    const int petLeft  = m_sys->petLeftToday();
    const int feedLeft = m_sys->feedLeftToday();

    m_btnPet->setEnabled(petLeft > 0);
    m_btnPet->setText(petLeft > 0 ? QStringLiteral("摸摸 +%1").arg(int(PetCfg::AFF_PET_POINT))
                                  : QStringLiteral("摸摸"));
    m_btnFeed->setEnabled(feedLeft > 0);
    m_btnFeed->setText(feedLeft > 0 ? QStringLiteral("喂食 +%1").arg(int(PetCfg::AFF_FEED_POINT))
                                    : QStringLiteral("喂食"));

    m_petQuota->setText(petLeft > 0
                            ? QStringLiteral("摸摸今日还剩 %1 次").arg(petLeft)
                            : QStringLiteral("摸摸今日已用完，明天再来"));
    m_feedQuota->setText(feedLeft > 0
                             ? QStringLiteral("喂食今日还剩 %1 次").arg(feedLeft)
                             : QStringLiteral("喂食今日已用完"));

    // ---- 底部说明 ----
    if (m_sys->isMaxLevel())
    {
        m_hint->setText(QStringLiteral("已经是最高等级（%1）。满级后不再衰减，好感度只增不减。")
                            .arg(m_sys->stageName()));
    }
    else
    {
        m_hint->setText(QStringLiteral("距 Lv.%1 还差 %2 点\u3000·\u3000自然衰减 %3 点/小时\u3000·\u3000不会掉级")
                            .arg(lv + 1)
                            .arg(qRound(toNext))
                            .arg(PetCfg::AFF_DECAY_PER_HOUR));
    }
}

// =============================================================================
//  升级：先把提示改掉，两秒后再刷回正常内容
// =============================================================================
void AffectionPage::onLeveledUp(int level)
{
    m_hint->setText(QStringLiteral("升级了！现在是 Lv.%1 %2")
                        .arg(level)
                        .arg(m_sys ? m_sys->stageName() : QString()));
    QTimer::singleShot(2000, this, &AffectionPage::refresh);
}
