#include "ChatPage.h"

#include "AffectionSystem.h"
#include "PetConfig.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QFrame>
#include <QTimer>
#include <QRandomGenerator>
#include <QShowEvent>
#include <QPainter>
#include <QPainterPath>

#include <utility>      // std::as_const：只读遍历 Qt 容器时别让它 detach

// 圆形头像的直径。26 那张脸太小了看不清，30 出得来眼睛
static constexpr int AVATAR_PX = 30;

// =============================================================================
//  构造
// =============================================================================
ChatPage::ChatPage(AffectionSystem* sys, QWidget* parent)
    : QWidget(parent), m_sys(sys)
{
    // 头像只裁这一次（每个气泡都用它），也避免做成函数里的 static QPixmap
    m_avatar = makeAvatar(AVATAR_PX);

    // 曲库扫盘。构造时先扫一次，之后每次切到这一页再扫一遍（见 showEvent）——
    // 用户往里丢歌词文件，切回来就生效，这就是"动态加歌"的全部机关。
    m_songs.reload();

    setStyleSheet(QStringLiteral(R"(
        QLabel#cap { color: #888780; font-size: 12px; }

        QScrollArea#msgArea { background: #F7F6F2; border: 1px solid #E5E3DB;
                              border-radius: 10px; }
        QWidget#msgHost { background: transparent; }

        QLabel#bubbleT { background: #E6F1FB; border-left: 2px solid #66CCFF;
                         border-radius: 10px; padding: 8px 11px;
                         font-size: 13px; color: #2C2C2A; }
        QLabel#bubbleU { background: #EDEBE4; border-radius: 10px;
                         padding: 8px 11px; font-size: 13px; color: #2C2C2A; }

        QLineEdit { background: #FFFFFF; border: 1px solid #D3D1C7;
                    border-radius: 8px; padding: 6px 10px; font-size: 12px; }
        QLineEdit:focus { border: 1px solid #66CCFF; }

        QPushButton#send { background: #66CCFF; color: #042C53; border: none;
                           border-radius: 8px; padding: 7px 14px; font-size: 12px; }
        QPushButton#send:hover    { background: #8FDAFF; }
        QPushButton#send:disabled { background: #D3D1C7; color: #888780; }
    )"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 16);
    root->setSpacing(10);

    // ---- 标题行：页名 · 关系阶段 ……… 今天还能聊几次 ----
    auto* head = new QHBoxLayout;
    head->setSpacing(8);

    auto* title = new QLabel(QStringLiteral("聊天"), this);
    title->setObjectName(QStringLiteral("cap"));
    head->addWidget(title);

    m_subtitle = new QLabel(this);
    m_subtitle->setObjectName(QStringLiteral("cap"));
    head->addWidget(m_subtitle);

    head->addStretch();

    m_quota = new QLabel(this);
    m_quota->setObjectName(QStringLiteral("cap"));
    head->addWidget(m_quota);

    root->addLayout(head);

    // ---- 消息区 ----
    // 气泡挂在一个普通 QWidget 上，整个塞进 QScrollArea（setWidgetResizable）。
    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("msgArea"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 视口默认会自己填一层底色，那样圆角就白画了 —— 关掉，让它露出上面样式表里的米色
    m_scroll->viewport()->setAutoFillBackground(false);

    m_host = new QWidget;
    m_host->setObjectName(QStringLiteral("msgHost"));
    m_hostLay = new QVBoxLayout(m_host);
    m_hostLay->setContentsMargins(12, 12, 12, 12);
    m_hostLay->setSpacing(10);
    // ★ 末尾挂一个伸展项 ★ 气泡一律插在它**前面**，这样从上面往下排；
    //   少了它，气泡会被垂直居中，一眼就像排错了。
    m_hostLay->addStretch(1);
    m_scroll->setWidget(m_host);

    root->addWidget(m_scroll, 1);

    // ---- 输入行 ----
    auto* bottom = new QHBoxLayout;
    bottom->setSpacing(8);

    m_input = new QLineEdit(this);
    m_input->setPlaceholderText(QStringLiteral("想对天依说点什么…（回车发送）"));
    connect(m_input, &QLineEdit::returnPressed, this, &ChatPage::onSend);
    bottom->addWidget(m_input, 1);

    m_sendBtn = new QPushButton(QStringLiteral("发送"), this);
    m_sendBtn->setObjectName(QStringLiteral("send"));
    m_sendBtn->setCursor(Qt::PointingHandCursor);
    connect(m_sendBtn, &QPushButton::clicked, this, &ChatPage::onSend);
    bottom->addWidget(m_sendBtn);

    root->addLayout(bottom);

    // ---- 打字机 ----
    // 45ms/字：短句大约一秒。太长会等得不耐烦，太短就等于没有这个效果。
    m_typeTimer = new QTimer(this);
    m_typeTimer->setInterval(45);
    connect(m_typeTimer, &QTimer::timeout, this, &ChatPage::onTypeTick);

    refreshHeader();
}

// =============================================================================
//  造一个气泡，挂到消息区末尾（伸展项之前）
//  返回气泡本身给打字机用
// =============================================================================
QLabel* ChatPage::makeBubble(const QString& text, bool fromTianyi)
{
    auto* row = new QWidget(m_host);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(8);

    auto* bubble = new QLabel(text, row);
    bubble->setObjectName(fromTianyi ? QStringLiteral("bubbleT") : QStringLiteral("bubbleU"));
    bubble->setWordWrap(true);
    // 宽度上限跟着消息区走（见 bubbleMaxWidth）：面板越大气泡越长，
    // 但永远留出一截空白，免得一句话铺满整行、看着分不出是谁在说。
    bubble->setMaximumWidth(bubbleMaxWidth());
    bubble->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_bubbles.append(bubble);        // 面板放大时要回头改它的上限

    if (fromTianyi)
    {
        // 头像是从站姿图上裁下来的那张脸，见 makeAvatar()
        auto* avatar = new QLabel(row);
        avatar->setPixmap(m_avatar);
        avatar->setFixedSize(AVATAR_PX, AVATAR_PX);
        h->addWidget(avatar, 0, Qt::AlignTop);
        h->addWidget(bubble, 0, Qt::AlignTop);
        h->addStretch(1);
    }
    else
    {
        h->addStretch(1);
        h->addWidget(bubble, 0, Qt::AlignTop);
    }

    m_hostLay->insertWidget(m_hostLay->count() - 1, row);
    return bubble;
}

void ChatPage::append(const QString& text, bool fromTianyi)
{
    QLabel* bubble = makeBubble(text, fromTianyi);

    if (fromTianyi)
        startTyping(bubble, text);

    scrollToBottom();
    // 布局是异步跟上的，直接滚有时会停在旧的 maximum() 上，再补一次
    QTimer::singleShot(0, this, &ChatPage::scrollToBottom);
}

// =============================================================================
//  打字机
// =============================================================================
void ChatPage::startTyping(QLabel* label, const QString& text)
{
    if (!label)
        return;

    m_typeLabel = label;
    m_typeText  = text;
    m_typePos   = 0;

    // 打字机放话期间不许再发 —— 这一页是"一个人说完，另一个人再说"。
    // ★ 这个禁用放在这里而不是 onSend() 里 ★
    //   开场白（showEvent）和回复（onSend）都走这个函数，只管 onSend 的话，
    //   开场白还在逐字显示时发送键是可点的 —— 点了什么也不会发生（被 onSend
    //   里那句 isActive() 拦掉），用户只会觉得按钮坏了。
    if (m_sendBtn)
        m_sendBtn->setEnabled(false);

    label->setText(QString());      // 先清空，再一个字一个字往外放
    m_typeTimer->start();
}

void ChatPage::onTypeTick()
{
    if (!m_typeLabel)
    {
        m_typeTimer->stop();
        return;
    }

    if (m_typePos >= m_typeText.size())
    {
        m_typeTimer->stop();
        m_typeLabel = nullptr;

        m_sendBtn->setEnabled(true);
        if (isVisible())
            m_input->setFocus();        // 说完一句就把光标还回去，方便接着打

        QTimer::singleShot(0, this, &ChatPage::scrollToBottom);
        return;
    }

    ++m_typePos;
    m_typeLabel->setText(m_typeText.left(m_typePos));
    scrollToBottom();
}

// =============================================================================
//  发送
// =============================================================================
void ChatPage::onSend()
{
    const QString text = m_input->text().trimmed();
    if (text.isEmpty())
        return;

    // 上一句还没放完就先别插话 —— 否则两条回复会叠在一起，
    // 而且挑词器会连按两下把最近说过的话记乱。（回车和按钮都会走到这里）
    if (m_typeTimer->isActive())
        return;

    append(text, false);
    m_input->clear();

    const ChatScript::Hit hit = ChatScript::matchIntent(text);

    // 先把挑词器跑一遍（它会推进"最近说过什么"的状态），
    // 下面两种情况之一命中时再把这句话整个换掉。
    QString          reply = m_picker.pick(hit);
    ChatScript::Mood mood  = (hit.intentIndex >= 0)
                                 ? ChatScript::intents().at(hit.intentIndex).mood
                                 : ChatScript::Mood::Neutral;

    // ★ ① 接歌词优先于一切 ★
    //   用户明明在唱某首歌的上一句，回一句"唔…你在说什么呀"就太扫兴了。
    const SongLibrary::Continuation next = SongLibrary::findContinuation(m_songs, text);
    if (next.found)
    {
        // ♪ 是给用户看的：一眼就知道这是在接着唱，不是随手回了句话
        reply = QStringLiteral("♪ ") + next.next;
        mood  = ChatScript::Mood::Happy;
    }
    else if (const QString song = songReplyFor(hit.id); !song.isEmpty())
    {
        // ★ ② 点歌：整段替换成曲库里的词 ★
        reply = song;
    }

    append(reply, true);

    // 情绪交给 DesktopPet 决定播什么动作 —— 这一页不碰桌宠状态。
    // 兜底（没命中意图）用 Neutral：本来就是"没听懂"，突然换个表情反而怪。
    emit moodChanged(mood);

    // 这里不用再禁用发送键了：上面的 append(reply, true) 会走 startTyping()，
    // 禁用和"放完再开"都由它管，放完那一半在 onTypeTick() 里。
}

// =============================================================================
//  点歌：从曲库里取词
//
//  返回空串的含义是"这个意图不归我管 / 曲库里没有这种节奏的歌"——
//  调用方会用台词库挑好的那句顶上（ChatScript 里那两条意图的 replies）。
//  这样即使用户把 resources/songs 清空了，也只会"唱不出来"，不会哑掉。
// =============================================================================
QString ChatPage::songReplyFor(const QString& intentId)
{
    if (intentId == QLatin1String("fastsong"))
        return SongLibrary::composeSongReply(m_songs, SongLibrary::Tempo::Fast);
    if (intentId == QLatin1String("slowsong"))
        return SongLibrary::composeSongReply(m_songs, SongLibrary::Tempo::Slow);
    return QString();
}

// =============================================================================
//  头像：从站姿图(08)上裁出"脸那一块"，再裁成圆形
//
//  ★ 裁切框 (38, 81, 90, 90) 是量出来的，不是试出来的 ★
//    站姿图是 183x252，角色整张脸偏上：眼睛（碧绿色）像素的重心在 (83, 116)。
//    以 (83, 126) 为中心、边长 90 的正方形 —— 上面留出头发，下面到下巴再带一点肩膀。
//    边长比"脸"本身大是因为头像最终只有 30px：裁太紧的话，缩下去只剩两只眼睛，
//    看不出是张脸；带上一点头发和肩膀才认得出人。
//    （量法：把"绿明显压过红和蓝"的像素重心算出来，就是眼睛的位置。）
//    换成别的姿势图或换了素材，这个框就得重量一遍。
// =============================================================================
QPixmap ChatPage::makeAvatar(int size)
{
    const QPixmap src(petImagePath(PetCfg::TRAY_ICON_IMG));   // 8 = 08_站立.png
    if (src.isNull() || size <= 0)
        return QPixmap();

    const QRect face(38, 81, 90, 90);

    QPixmap out(size, size);
    out.fill(Qt::transparent);

    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // 先按圆形裁，再画 —— 反过来（画完再擦四角）边缘会有一圈毛刺
    QPainterPath circle;
    circle.addEllipse(QRectF(0.0, 0.0, size, size));
    p.setClipPath(circle);
    p.drawPixmap(QRect(0, 0, size, size), src, face);
    p.end();

    return out;
}

// =============================================================================
//  气泡宽度上限 —— 跟着消息区宽度走
//
//  面板从 640x440 放大到铺满屏幕时，消息区会宽出好几倍。上限写死成一个像素值
//  （原来是 330）的话，气泡会缩在左边一小条里，右边一大片空白。
//
//  取 78% 并在 [240, 660] 之间收口：
//    · 留 22% 空白，让人一眼看出"这句到哪结束"，而不是铺满整行；
//    · 下界 240 保证窄窗口下还能放下一整句；
//    · 上界 660 是"再宽就不好读了"那条线（一行六十多个字已经是极限）。
// =============================================================================
int ChatPage::bubbleMaxWidth() const
{
    const int avail = m_scroll ? m_scroll->viewport()->width() : 0;
    return qBound(240, int(avail * 0.78), 660);
}

void ChatPage::applyBubbleWidths()
{
    if (m_bubbles.isEmpty())
        return;

    const int maxW = bubbleMaxWidth();
    for (QLabel* b : std::as_const(m_bubbles))    // as_const：别让这个只读遍历把容器 detach 一份
    {
        if (b && b->maximumWidth() != maxW)
            b->setMaximumWidth(maxW);   // 改上限会自己触发重新布局
    }
}

void ChatPage::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    applyBubbleWidths();
}

// =============================================================================
//  滚动到底 / 刷新标题
// =============================================================================
void ChatPage::scrollToBottom()
{
    if (!m_scroll)
        return;

    QScrollBar* bar = m_scroll->verticalScrollBar();
    bar->setValue(bar->maximum());
}

void ChatPage::refreshHeader()
{
    if (!m_subtitle || !m_quota)
        return;

    // 阶段称号本身就是四个字（最初相逢 / 心上华海 / 浅梦低语 / 初心华梦 / 依为心生），
    // 后面再缀一个"阶段"会读成"最初相逢阶段"——所以这里只显示称号。
    const QString stage = m_sys ? m_sys->stageName() : QStringLiteral("最初相逢");
    m_subtitle->setText(QStringLiteral("洛天依 · %1").arg(stage));

    // ★ 这里是"今天还能加几次分"，不是"还能聊几次" ★
    //   聊天本身不设次数限制 —— 写成"还能聊 5 次"会让人以为聊满 5 句就不能说话了。
    //   上限（AFF_CHAT_PER_DAY）限制的是好感度加分，所以标题行写清楚"加分"，
    //   完整解释塞进 tooltip（标题行只有一格宽，写不下）。
    const int left = m_sys ? m_sys->chatLeftToday() : 0;
    m_quota->setText(left > 0
                         ? QStringLiteral("预设台词 · 不联网 · 今天加分还剩 %1 次").arg(left)
                         : QStringLiteral("预设台词 · 不联网 · 今天的聊天加分已用完"));
    m_quota->setToolTip(QStringLiteral("聊天不限次数，想聊多久都行 —— 台词是本地预设的，不用联网。\n"
                                       "只有好感度加分每天最多 %1 次，每次进入聊天页记一次。")
                            .arg(PetCfg::AFF_CHAT_PER_DAY));
}

// =============================================================================
//  每次切到这一页
// =============================================================================
void ChatPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);

    // 每次切到这一页都重新扫一遍曲库 —— 用户往 resources/songs/ 里丢了个新的
    // .txt、或者在旧文件里改了几句词，切回来就生效，不用重编也不用重启。
    // 只读几个小文本文件，代价可以忽略。
    m_songs.reload();

    // 关系阶段每次进来都重取：中途可能摸过喂过、升过级，说话方式该跟着变
    m_picker.stage = m_sys ? m_sys->stageIndex() : 0;

    // 每次切到这一页 = 一次"聊天"。
    // 记分的活交给 DesktopPet（这一页不碰好感度数据，见头文件的说明）；
    // 聊多少句都不再额外加分，所以反复说话刷不到分。
    // ★ 顺序是"先记分再刷新标题" ★：反过来的话，右上角写的是"用过之前"的次数，
    //   进来一次却已经扣掉了一次，看起来像少算了一回。
    emit chatEntered();
    refreshHeader();

    if (!m_opened)
    {
        m_opened = true;

        const QStringList& opening = ChatScript::openingLines();
        if (!opening.isEmpty())
            append(opening.at(int(QRandomGenerator::global()->bounded(opening.size()))), true);
    }
}

// =============================================================================
//  把光标放进输入框（菜单「和我聊天」进来时用）
// =============================================================================
void ChatPage::focusInput()
{
    if (m_input)
        m_input->setFocus();
}

// =============================================================================
//  自检描述（纯只读：不建窗口、不碰存档）
// =============================================================================
QString ChatPage::describeChat()
{
    const QVector<ChatScript::Intent>& all = ChatScript::intents();

    int replyCount = 0;
    for (const ChatScript::Intent& it : all)
        replyCount += it.replies.size() + it.warmReplies.size() + it.echoReplies.size();
    replyCount += ChatScript::fallbackReplies().size()
                + ChatScript::fallbackEchoReplies().size()
                + ChatScript::repeatReplies().size();

    QString out;
    out += QStringLiteral("  台词来源  : 编译进 exe（ChatScript.h），不联网、不接大模型\r\n");
    out += QStringLiteral("  意图数量  : %1 条\r\n").arg(all.size());
    out += QStringLiteral("  台词总量  : %1 句（含兜底 / 回声式 / 重复应对）\r\n").arg(replyCount);

    // 光有数字看不出逻辑对不对，所以拿真实输入跑一条完整的样例出来。
    // 跑的是运行时同一份 matchIntent + Picker —— 不是另写一段样例代码。
    const QString        sample = QStringLiteral("你今天唱首歌给我听好不好");
    const ChatScript::Hit hit    = ChatScript::matchIntent(sample);

    ChatScript::Picker picker;
    picker.stage = 2;                       // 按"亲近"阶段跑，顺便验证熟络版台词
    const QString reply = picker.pick(hit);

    out += QStringLiteral("  样例      : 「%1」\r\n").arg(sample);
    out += QStringLiteral("               → 意图 %1（%2），得分 %3\r\n")
               .arg(hit.id.isEmpty() ? QStringLiteral("(兜底)") : hit.id, hit.label)
               .arg(hit.score);
    out += QStringLiteral("               → 台词「%1」\r\n").arg(reply);
    out += QStringLiteral("  挑词规则  : 同一话题连说 → \"你刚说过呀\"；"
                          "抽到回显片段 → 回声式；关系到亲近 → 熟络版\r\n");
    return out;
}
