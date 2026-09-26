#include "DailyImagePage.h"
#include "UiFont.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QDate>
#include <QRandomGenerator>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QUrl>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QGuiApplication>
#include <QTimer>

#include <utility>      // std::as_const（见 findDailyImageDir 那里的说明）

// =============================================================================
//  缩放相关的两个常数（见 rescaleImage / applyImage 的说明）
// =============================================================================
namespace {

// 停手多久之后才做那次"平滑"缩放。120ms 比双击间隔短，用户感觉不到延迟，
// 又足够把一次连续拖动产生的一长串 resizeEvent 合并掉。
constexpr int kRescaleSettleMs = 120;

// 源图最多留多清晰。
//   ★ 判据是"面板最多能长多大"，不是"现在多大" ★
//     面板可以铺满屏幕，那时候图片区有上千像素宽。如果按当前控件尺寸去降采样，
//     一按"铺满"就会把这张小图拉大 —— 直接糊掉。所以取所有屏幕里最大的那块的
//     物理像素长边：再大在这个程序里也没地方显示。
//   ★ 为什么要有个上限 ★
//     平滑缩放的开销约等于「目标像素数 × 缩放倍数」。用户的插画可能有好几千像素
//     （本机实测有 7000px 的），每次窗口一变就从头缩一遍就是"这一页一拉就卡"的根。
//     有了上限，单次开销至少被压到一个可预期的范围内（配合下面的节流 → 一次拖动只做一次）。
//   ★ 为什么要留 1200 的下限 ★
//     屏幕很小（或者 DPI 报得很怪）时也留点余量，免得连正常窗口都喂不饱。
int sourceCapPx()
{
    int cap = 0;
    const QList<QScreen*> screens = QGuiApplication::screens();
    for (const QScreen* s : screens)
    {
        if (!s)
            continue;
        // geometry() 是逻辑像素，乘以 DPR 才是真正要画的物理像素
        const qreal dpr = s->devicePixelRatio();
        cap = qMax(cap, int(qMax(s->geometry().width(), s->geometry().height()) * dpr));
    }
    return qMax(1200, cap);
}

// 把原图降采样到"够用"的一版（比上限还小就原样返回）。
QPixmap downscaleSourceForDisplay(const QPixmap& src)
{
    const int longSide = qMax(src.width(), src.height());
    if (longSide <= 0)
        return src;

    const int cap = sourceCapPx();
    if (longSide <= cap)
        return src;               // 本来就不大，不动它

    return src.scaled(cap, cap, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

} // namespace

// =============================================================================
//  存档
//
//  用独立的 ini（%APPDATA%/PetPal/daily_image.ini），不跟好感度那份混在一起 ——
//  两个功能的键名各管各的，以后想单独删掉"每日图片"的记录也不用动好感度。
// =============================================================================
static QSettings dailySettings()
{
    return QSettings(QSettings::IniFormat, QSettings::UserScope,
                     QStringLiteral("PetPal"), QStringLiteral("daily_image"));
}

// =============================================================================
//  样式表 / 字号档位
//  包在 UiFont::styleSheet() 里 —— 它按"界面字号"档位改写 font-size（见 UiFont.h）
// =============================================================================
void DailyImagePage::applyStyle()
{
    setStyleSheet(UiFont::styleSheet(QStringLiteral(R"(
        QLabel#cap   { color: #888780; font-size: 12px; }
        QLabel#hint  { color: #888780; font-size: 12px; }
        QLabel#frame { background: #F1EFE8; border-radius: 8px;
                       color: #B4B2A9; font-size: 13px; }
        QPushButton#ghost {
            background: transparent; color: #534AB7; border: 1px solid #AFA9EC;
            border-radius: 7px; padding: 4px 12px; font-size: 12px;
        }
        QPushButton#ghost:hover   { background: #EEEDFE; }
        QPushButton#ghost:pressed { background: #E3E0FB; }
    )")));
}

void DailyImagePage::applyUiScale()
{
    applyStyle();
    updateProgressLabel();     // 字号一变行宽就变，那句进度文字要重排一次
}

// =============================================================================
//  找 daily_image 目录
//
//  为什么不能写死一个路径：这个程序有两种跑法 ——
//    · build.bat 产出      build/ninja-Debug/PetPal.exe
//    · Qt Creator 产出     build/Desktop_Qt_6_11_2_MSVC2022_64bit_Debug/PetPal.exe
//  两者都在 build 下面的**第二层**，而图片在项目根的 resources/daily_image。
//  所以做法是：从 exe 所在目录逐级往上找（找 4 层足够覆盖嵌套差异），
//  再兜底看当前工作目录（Qt Creator 的 CWD 有时就是构建目录或它的父目录）。
//
//  ★ 下面那个 std::as_const 别删 ★
//    roots 是"边建边填"的局部 QStringList，是非 const 的。范围 for 在非 const Qt 容器上
//    走的是非 const 的 begin()，会触发**写时复制分离（detach）** —— 这里白拷贝一份整个列表。
//    clazy 的 range-loop-detach 也正是指这个。套上 as_const 后走 const 版 begin()，
//    不分离、不拷贝，循环体里本来也只需要读。
//    （Qt 6.6 起 qAsConst 已弃用，统一用标准库的 std::as_const。）
// =============================================================================
QString DailyImagePage::findDailyImageDir()
{
    const QString rel  = QStringLiteral("resources/daily_image");
    const QString exeDir = QCoreApplication::applicationDirPath();

    QStringList roots;
    roots << exeDir;

    QDir up(exeDir);
    for (int i = 0; i < 4 && up.cdUp(); ++i)
        roots << up.absolutePath();

    const QString cwd = QDir::currentPath();
    if (!cwd.isEmpty() && !roots.contains(cwd))
        roots << cwd;

    for (const QString& root : std::as_const(roots))
    {
        const QDir d(root + QLatin1Char('/') + rel);
        if (d.exists())
            return d.absolutePath();
    }

    // 兜底：有人把 daily_image 整个拷到 exe 旁边了
    const QDir flat(exeDir + QStringLiteral("/daily_image"));
    if (flat.exists())
        return flat.absolutePath();

    return QString();
}

// =============================================================================
//  构造
// =============================================================================
DailyImagePage::DailyImagePage(bool persistent, QWidget* parent)
    : QWidget(parent), m_persistent(persistent)
{
    applyStyle();

    // 缩放的"停手"定时器：拖动窗口时每一次 resizeEvent 只做快速缩放，
    // 这个定时器被不断重启，等真正停手 120ms 后才补一次平滑缩放（见 resizeEvent）。
    m_rescaleTimer = new QTimer(this);
    m_rescaleTimer->setSingleShot(true);
    m_rescaleTimer->setInterval(kRescaleSettleMs);
    connect(m_rescaleTimer, &QTimer::timeout, this, [this]() { rescaleImage(/*smooth=*/true); });

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 16);
    root->setSpacing(10);

    // 标题行：左边是页名，右边是「换一换」
    auto* head = new QHBoxLayout;
    head->setSpacing(8);

    auto* title = new QLabel(QStringLiteral("每日图片"), this);
    title->setObjectName(QStringLiteral("cap"));
    head->addWidget(title);

    // 本次进度：「本次还剩 N 张没看过」。
    // ★ 刻意放标题右边（左边），不放「换一换」左边 ★ —— 这句长短会变
    //   （"19 张" → "1 张"），放在按钮旁边的话按钮每次都会左右挪一下；
    //   放在 stretch 左边，后面的伸展段把变化吃掉，按钮位置就是死的。
    m_progress = new QLabel(this);
    m_progress->setObjectName(QStringLiteral("cap"));
    head->addWidget(m_progress);

    head->addStretch();

    m_shuffleBtn = new QPushButton(QStringLiteral("换一换"), this);
    m_shuffleBtn->setObjectName(QStringLiteral("ghost"));
    m_shuffleBtn->setCursor(Qt::PointingHandCursor);
    m_shuffleBtn->setToolTip(QStringLiteral("随机换一张（今天剩下的时间就显示这张）"));
    connect(m_shuffleBtn, &QPushButton::clicked, this, &DailyImagePage::onShuffle);
    head->addWidget(m_shuffleBtn);

    root->addLayout(head);

    m_image = new QLabel(this);
    m_image->setObjectName(QStringLiteral("frame"));
    m_image->setAlignment(Qt::AlignCenter);
    // ★ 两个 Ignored 很关键 ★
    //   QLabel 默认会按 pixmap 大小改自己的 sizeHint，于是"缩放 -> 尺寸变 -> 再缩放"
    //   来回抖。设成 Ignored 后它只认布局给的那块区域，缩放是单向的。
    m_image->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_image->setCursor(Qt::PointingHandCursor);
    m_image->setToolTip(QStringLiteral("双击用系统看图工具打开原图"));
    m_image->installEventFilter(this);
    root->addWidget(m_image, 1);

    m_caption = new QLabel(this);
    m_caption->setObjectName(QStringLiteral("hint"));
    m_caption->setWordWrap(true);
    root->addWidget(m_caption);

    m_dir = findDailyImageDir();
    refreshForToday();
}

// =============================================================================
//  扫描目录里所有能当图片的文件
// =============================================================================
static QStringList listImagesIn(const QString& dir)
{
    if (dir.isEmpty())
        return QStringList();

    const QStringList filters{ QStringLiteral("*.jpg"),  QStringLiteral("*.jpeg"),
                              QStringLiteral("*.png"),  QStringLiteral("*.bmp"),
                              QStringLiteral("*.webp"), QStringLiteral("*.gif") };

    // 排序是刻意的：随机挑选得在"稳定的列表"上做，否则不同机器的目录顺序不一样，
    // 同一天在两台机器上挑出来的图会不同。
    return QDir(dir).entryList(filters, QDir::Files, QDir::Name);
}

QStringList DailyImagePage::scanImages() const
{
    return listImagesIn(m_dir);
}

// =============================================================================
//  自检描述（纯只读，不建窗口、不解码图、不写存档）
// =============================================================================
QString DailyImagePage::describeDailyImage()
{
    const QString     dir   = findDailyImageDir();
    const QStringList files = listImagesIn(dir);
    const QString     today = QDate::currentDate().toString(Qt::ISODate);

    QSettings st = dailySettings();
    const QString savedDate = st.value(QStringLiteral("dailyImage/date")).toString();
    const QString savedName = st.value(QStringLiteral("dailyImage/name")).toString();

    const QString none = QStringLiteral("（还没有）");

    QString out;
    out += QStringLiteral("  图片目录  : %1\r\n")
               .arg(dir.isEmpty() ? QStringLiteral("(没找到)") : dir);
    out += QStringLiteral("  目录图片数: %1 张\r\n").arg(files.size());
    out += QStringLiteral("  存档文件  : %1\r\n").arg(st.fileName());
    out += QStringLiteral("  存档记录  : %1\r\n")
               .arg(savedDate.isEmpty() ? none
                                        : QStringLiteral("%1  →  %2").arg(savedDate, savedName));
    out += QStringLiteral("  今天日期  : %1\r\n").arg(today);

    if (files.isEmpty())
        out += QStringLiteral("  结论      : 目录里没有图片，这一页会提示该往哪放\r\n");
    else if (savedDate == today && files.contains(savedName))
        out += QStringLiteral("  结论      : 今天沿用存档里那张 —— 同一天反复开关面板不会变  [OK]\r\n");
    else
        out += QStringLiteral("  结论      : 今天还没挑过，打开这一页会随机抽一张并记进存档\r\n");

    return out;
}

// =============================================================================
//  今天该显示哪张
// =============================================================================
QString DailyImagePage::pickTodayImage()
{
    const QStringList files = scanImages();
    if (files.isEmpty())
        return QString();

    QSettings s = dailySettings();

    const QString today     = QDate::currentDate().toString(Qt::ISODate);
    const QString savedDate = s.value(QStringLiteral("dailyImage/date")).toString();
    const QString savedName = s.value(QStringLiteral("dailyImage/name")).toString();

    // 同一天、而且那张图还在 -> 沿用。
    // 这一条是"每日图片"的关键：一天之内反复开关面板都该是同一张，
    // 否则每次打开都换一张，就不叫"每日"了。
    if (savedDate == today && !savedName.isEmpty() && files.contains(savedName))
        return m_dir + QLatin1Char('/') + savedName;

    // 跨天（或昨天那张被删了）：重新挑。
    // 「本次已看过」这会儿装的是昨天那张（showEvent 重置时留下的），
    // 所以 pickUnseen 天然会避开它 —— 原来那段"重试 16 次避开昨天"的代码可以退休了。
    const QString pick = pickUnseen(files, QString());

    // 只读模式（--selftest）不落盘：否则跑一次自检就替用户把当天的图定了
    if (m_persistent)
    {
        s.setValue(QStringLiteral("dailyImage/date"), today);
        s.setValue(QStringLiteral("dailyImage/name"), pick);
    }

    return m_dir + QLatin1Char('/') + pick;
}

// =============================================================================
//  ★ 挑图唯一入口：优先给"本次打开还没看过"的 ★
//
//  为什么只留一个函数：跨天首次挑图、点「换一换」，本质是同一件事 ——
//  "给我一张我这次还没看过的"。两处各写一套随机逻辑的话，迟早出现
//  "换一换会重复、首次挑图不会"这种不一致。
//
//  currentName 传"屏幕上正显示的那张"，会一并排除 —— 否则按「换一换」
//  有可能抽到当前这张，用户看着就是"按钮没反应"。
// =============================================================================
QString DailyImagePage::pickUnseen(const QStringList& files, const QString& currentName)
{
    if (files.isEmpty())
        return QString();

    auto collectUnseen = [&](QStringList& out)
    {
        out.clear();
        for (const QString& f : files)
        {
            if (f == currentName || m_seenThisOpen.contains(f))
                continue;
            out << f;
        }
    };

    QStringList unseen;
    collectUnseen(unseen);

    if (unseen.isEmpty())
    {
        // 目录里的图都看过了 → 开新一轮。
        // 只保留"屏幕上这张"，其余清掉：这样新一轮的第一张不会又是刚看过的那张。
        // （下一张抽出来后会由 applyImage 记进集合，所以集合不会一直是空的。）
        m_seenThisOpen.clear();
        if (!currentName.isEmpty())
            m_seenThisOpen.insert(currentName);
        collectUnseen(unseen);

        if (unseen.isEmpty())          // 目录里只有一张（连"除当前以外"都没得挑）
            return files.first();
    }

    return unseen.at(int(QRandomGenerator::global()->bounded(unseen.size())));
}

// =============================================================================
//  刷新（跨天在这里生效）
// =============================================================================
void DailyImagePage::refreshForToday()
{
    // 目录可能是在程序启动后才被创建/移走的，所以每次刷新都重找一遍
    if (m_dir.isEmpty())
        m_dir = findDailyImageDir();

    if (m_dir.isEmpty())
    {
        m_source = QPixmap();
        m_sourceSize = QSize();
        m_currentPath.clear();
        m_image->setPixmap(QPixmap());
        m_image->setText(QStringLiteral("没找到 daily_image 文件夹"));
        m_caption->setText(QStringLiteral(
            "应该在项目的 resources/daily_image/ 下面。"
            "程序会从 exe 所在目录往上找 4 层，再找当前工作目录。"));
        if (m_progress) m_progress->setText(QString());   // 没图就没进度可言
        return;
    }

    const QString path = pickTodayImage();

    if (path.isEmpty())
    {
        m_source = QPixmap();
        m_sourceSize = QSize();
        m_currentPath.clear();
        m_image->setPixmap(QPixmap());
        m_image->setText(QStringLiteral("daily_image 里还没有图片"));
        m_caption->setText(QStringLiteral("把 jpg / png 丢进 %1 就行，重启程序即可看到。").arg(m_dir));
        if (m_progress) m_progress->setText(QString());
        return;
    }

    // 同一张图就不重复解码（面板关掉又打开时会走到这里）。
    // 但底部那行进度要重算：showEvent 刚把"本次已看过"重置过，
    // 继续挂着上一次的数字会让人以为重置没生效。
    if (path == m_currentPath && !m_source.isNull())
    {
        m_caption->setText(captionText());
        updateProgressLabel();
        return;
    }

    applyImage(path);
}

// =============================================================================
//  载入并显示
// =============================================================================
void DailyImagePage::applyImage(const QString& path)
{
    QPixmap pm;
    if (!pm.load(path))
    {
        // ★ 读不出来的也记进"已看过" ★
        //   不记的话它会一直留在"没看过"那堆里，每按一次「换一换」都有概率又抽到它，
        //   用户的观感就是"按钮时灵时不灵"。代价是这张图这次不再被抽到 ——
        //   但它本来也显示不出来，跳过它才是对的。
        m_seenThisOpen.insert(QFileInfo(path).fileName());

        m_source = QPixmap();
        m_sourceSize = QSize();
        m_currentPath.clear();
        m_image->setPixmap(QPixmap());
        m_image->setText(QStringLiteral("这张图读不出来"));
        m_caption->setText(QFileInfo(path).fileName());
        updateProgressLabel();
        return;
    }

    m_currentPath = path;
    m_sourceSize  = pm.size();       // 原图尺寸：底栏那行要显示它（下面 m_source 可能被缩小）
    m_source      = downscaleSourceForDisplay(pm);

    // 真显示出来的也记一笔，理由同上：抽到过就别再抽第二次
    m_seenThisOpen.insert(QFileInfo(path).fileName());

    m_image->setText(QString());     // 清掉可能存在的提示文字
    m_rescaleTimer->stop();          // 刚换图，之前排队的那个平滑缩放不用再等了
    rescaleImage(/*smooth=*/true);

    m_caption->setText(captionText());
    updateProgressLabel();
}

// =============================================================================
//  底部那行说明：文件名 / 原图尺寸 / 双击提示
// =============================================================================
QString DailyImagePage::captionText() const
{
    const QString name = QFileInfo(m_currentPath).fileName();
    if (name.isEmpty())
        return QString();

    return QStringLiteral("%1\u3000·\u3000原图 %2 × %3\u3000·\u3000双击用系统看图工具打开")
        .arg(name)
        .arg(m_sourceSize.width())
        .arg(m_sourceSize.height());
}

// =============================================================================
//  标题行那个进度：「本次还剩 N 张没看过」
//
//  为什么要把这个数字显出来：这个功能（不重复）坏了的话，光看画面很难判断
//  是"真抽到了没看过的"还是"运气好没重复"。有计数就能当场对照 ——
//  点一次「换一换」，数字应该减 1；减到 0 之后再点，会重新开一轮、数字跳回 N-1。
// =============================================================================
void DailyImagePage::updateProgressLabel()
{
    if (!m_progress)
        return;

    const QStringList files = scanImages();

    int seen = 0;
    for (const QString& f : files)
        if (m_seenThisOpen.contains(f))
            ++seen;

    const int left = files.size() - seen;

    m_progress->setText(left > 0
        ? QStringLiteral("本次还剩 %1 张没看过").arg(left)
        : QStringLiteral("本次都看过了，再点开新一轮"));
}

// =============================================================================
//  「换一换」：换到下一张"本次还没看过"的
//
//  换完要写进存档 —— 用户主动换的，那今天就该是这张；不写的话下次打开面板
//  又弹回原来那张，像是没生效。
//  （"本次已看过"那个集合不写存档，见头文件说明。）
// =============================================================================
void DailyImagePage::onShuffle()
{
    const QStringList files = scanImages();

    if (files.isEmpty())
    {
        m_caption->setText(QStringLiteral("daily_image 里还没有图片，没得换"));
        return;
    }
    if (files.size() == 1)
    {
        m_caption->setText(QStringLiteral("目录里只有这一张图，没得换"));
        return;
    }

    const QString current = QFileInfo(m_currentPath).fileName();
    const QString pick    = pickUnseen(files, current);

    // 上面已经挡掉了空目录和单图目录，正常到不了这里；留个保护免得抽到空字符串去 load
    if (pick.isEmpty())
        return;

    if (m_persistent)
    {
        QSettings s = dailySettings();
        s.setValue(QStringLiteral("dailyImage/date"), QDate::currentDate().toString(Qt::ISODate));
        s.setValue(QStringLiteral("dailyImage/name"), pick);
    }

    applyImage(m_dir + QLatin1Char('/') + pick);
}

// =============================================================================
//  等比缩放填进可用区域
//
//  ★ 两条路径 ★（smooth 由调用方选）
//    · false —— 最近邻。几毫秒，给"正在拖窗口"这条高频路径用：画面立刻跟上，
//      虽然边缘糙一点，但拖动过程中看不出来；
//    · true  —— 平滑。停手之后补一次，最终看到的是干净的那张。
//  源图已经降过采样（见 downscaleSourceForDisplay），所以这两条都很便宜。
// =============================================================================
void DailyImagePage::rescaleImage(bool smooth)
{
    if (m_source.isNull() || !m_image)
        return;

    // 留一点内边距，别让图贴着圆角边框
    QSize avail = m_image->size() - QSize(12, 12);
    if (avail.width() < 16 || avail.height() < 16)
        return;                     // 布局还没算好，等下一次 resizeEvent

    m_image->setPixmap(m_source.scaled(avail, Qt::KeepAspectRatio,
                                       smooth ? Qt::SmoothTransformation
                                              : Qt::FastTransformation));
}

// =============================================================================
//  窗口尺寸变了
//
//  ★ 为什么不能在这里直接做平滑缩放 ★
//    拖动窗口边缘时 resizeEvent 会连着来几十上百次。每次都平滑重缩一遍
//    （原图几千像素时单次就是几百毫秒）→ 面板跟着一起卡，鼠标都已经松了画面还在追。
//    所以拆成两步：先做一次便宜的快速缩放保证跟手，再用一个"停手"定时器把
//    这串事件合并成一次平滑缩放。定时器每次都会被重启，所以只有真正停下来才触发。
// =============================================================================
void DailyImagePage::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);

    rescaleImage(/*smooth=*/false);     // 便宜的那一步：先让画面跟上
    m_rescaleTimer->start();            // 停手后再来一次平滑的
}

void DailyImagePage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);

    // ★ 每次切到这一页，就重置"本次已看过" ★
    //   用户要的是"打开这一页、看着的时候别重复"，不需要跨次记忆，
    //   所以这个集合是纯内存态，在这里归零。
    //   ★ 重置成"只剩屏幕上那张"，不是清成空：屏幕上明明还挂着这张图，
    //     清空的话一按「换一换」就可能又抽到它，看着像按钮没反应。
    m_seenThisOpen.clear();
    if (!m_currentPath.isEmpty())
        m_seenThisOpen.insert(QFileInfo(m_currentPath).fileName());

    // 再检查一次日期：程序跨 0 点一直开着的话，第二天切过来就是新的一张。
    // 同一天内则原样不动（pickTodayImage 会用存档里那张）。
    // 跨天换图时，上面留下的"昨天那张"正好被 pickUnseen 排除掉，一举两得。
    refreshForToday();
}

// =============================================================================
//  双击图片 -> 系统默认看图工具打开原图
// =============================================================================
bool DailyImagePage::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == m_image && event->type() == QEvent::MouseButtonDblClick)
    {
        if (m_currentPath.isEmpty())
            return true;

        if (!QFile::exists(m_currentPath))
        {
            // 图在程序运行期间被删了 —— 说清楚，别让人以为双击没反应
            m_caption->setText(QStringLiteral("文件已经不在了：%1").arg(m_currentPath));
            m_source = QPixmap();
            return true;
        }

        // fromLocalFile 会把中文名、括号、加号这些字符正确转义，
        // 所以 resources/daily_image 里的中文文件名（比如「四重奏.jpg」）不会有问题。
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(m_currentPath)))
            m_caption->setText(QStringLiteral("系统没有能打开这张图的程序：%1")
                                   .arg(QFileInfo(m_currentPath).fileName()));
        return true;
    }

    return QWidget::eventFilter(obj, event);
}
