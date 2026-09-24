#pragma once
// =============================================================================
//  ChatPage —— 主面板里的「聊天」页（预设台词，不联网、不接大模型）
//
//  职责边界（和摸摸/喂食一个规矩）：
//    · 这一页只管"显示对话"和"从台词库里挑一句话"
//    · **不碰好感度数据** —— 切到这一页只发 chatEntered()，加分由 DesktopPet 去做
//    · **不直接切桌宠状态** —— 只发 moodChanged(情绪)，播什么动作由 DesktopPet 决定
//  为什么这么切：一句回复要同时发生两件事（加分、桌宠做表情）。页面自己动手的话，
//  迟早出现"加了分没播动画"或者反过来 —— 这一页插在中间只会把两边都搞乱。
//
//  界面形态：消息区（气泡，可滚动）+ 输入行。
//  ★ 刻意没有快捷话题气泡（用户明确不要）★ 话题的可发现性改由兜底台词承担：
//    兜底那几句会主动说"你可以问我愿不愿意唱歌"之类，见 ChatScript.h。
//
//  ★ 聊天次数不限，只有"好感度加分"有每日上限（AFF_CHAT_PER_DAY）★
//    所以标题行写的是"今天加分还剩 N 次"。哪天有人看到这个数字想加一道
//    "聊满 N 句就不让说了"的闸门 —— 不要加，用户要的是不限制。
//
//  气泡宽度上限跟着消息区走（bubbleMaxWidth）：面板放大后气泡会跟着变长，
//  不是固定 330px。站姿图上裁下来的圆形头像当说话人标识。
//
//  打字机：天依的话逐字显示（约 45ms/字）。不是为了炫技 —— 预设台词是瞬时的，
//  一口刷出来很像机器人；慢一点反而像在斟酌。打字期间发送键禁用，免得插话插乱。
// =============================================================================

#include <QWidget>
#include <QStringList>
#include <QPixmap>
#include <QVector>
#include "ChatScript.h"
#include "SongLibrary.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QVBoxLayout;
class QTimer;
class AffectionSystem;

class ChatPage : public QWidget
{
    Q_OBJECT
public:
    explicit ChatPage(AffectionSystem* sys, QWidget* parent = nullptr);

    // 给 --selftest 用：纯只读，不建窗口、不碰存档。
    static QString describeChat();

    // 从菜单「和我聊天」进来时，把光标直接放进输入框 —— 打开面板是为了说话，
    // 不该让人再点一下输入框（面板上点进这一页则不需要，那种情况下用户还在看界面）。
    void focusInput();

signals:
    void chatEntered();                      // 切到这一页 —— DesktopPet 据此记一次聊天
    void moodChanged(ChatScript::Mood mood); // 这句回复的情绪 —— DesktopPet 据此播动作

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;   // 面板放大/缩小时重算气泡宽度

private slots:
    void onSend();
    void onTypeTick();

private:
    QLabel*  makeBubble(const QString& text, bool fromTianyi);
    void     append(const QString& text, bool fromTianyi);
    void     startTyping(QLabel* label, const QString& text);
    void     scrollToBottom();
    void     refreshHeader();

    int      bubbleMaxWidth() const;    // 气泡宽度上限 —— 跟着消息区宽度变
    void     applyBubbleWidths();       // 把新上限刷到已经造出来的气泡上

    // 点歌：命中 fastsong/slowsong 时从曲库取词。返回空 = 不是点歌（或曲库里没有
    // 这种节奏的歌），那就照常用台词库挑好的那句。
    QString  songReplyFor(const QString& intentId);

    // 头像：从站姿图(08)上裁"脸"那一块，裁成圆形。裁切框见 .cpp 里的说明。
    static QPixmap makeAvatar(int size);

    AffectionSystem* m_sys = nullptr;

    QLabel*      m_subtitle = nullptr;   // 「洛天依 · 最初相逢」那行
    QLabel*      m_quota    = nullptr;   // 右上角：聊天不限次数，这里只报"今天还剩几次加分"
    QScrollArea* m_scroll   = nullptr;
    QWidget*     m_host     = nullptr;   // 气泡都挂在这上面
    QVBoxLayout* m_hostLay  = nullptr;
    QLineEdit*   m_input    = nullptr;
    QPushButton* m_sendBtn  = nullptr;

    // 造出来的气泡都记着 —— 面板一放大就得把它们一起放宽（见 applyBubbleWidths）。
    // 气泡只增不删，所以不用担心悬空指针。
    QVector<QLabel*> m_bubbles;

    // 头像（圆形，从站姿图上裁的脸）。构造时做一次，不给每个气泡重裁一遍
    // —— 也不做成函数里的 static：静态 QPixmap 要活到 QApplication 之后才析构，
    //    Qt 那边是不允许的。
    QPixmap m_avatar;

    QTimer* m_typeTimer = nullptr;
    QLabel* m_typeLabel = nullptr;       // 正在逐字显示的那个气泡
    QString m_typeText;
    int     m_typePos = 0;

    // 挑台词的三个状态（最近说过的话 / 上一句的意图 / 关系阶段）都收在这里，
    // 页面自己不解释它们 —— 全交给 ChatScript::Picker。
    ChatScript::Picker m_picker;

    // 曲库（磁盘上的 resources/songs/*.txt）。每次切到这一页重新扫一遍 ——
    // 用户往里丢个新 .txt 就会立刻出现，这就是"动态添加曲库"的全部机关。
    SongLibrary::Library m_songs;

    bool m_opened = false;               // 开场白只放一次
};
