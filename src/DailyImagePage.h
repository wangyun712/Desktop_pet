#pragma once
// =============================================================================
//  DailyImagePage —— 主面板里的「每日图片」页
//
//  做三件事：
//    ① 挑图：同一天内固定同一张，跨天重新挑（不会跟昨天重复）
//    ② 显示：等比缩放到页面里，不裁不拉变形
//    ③ 双击：交给系统看图工具打开原图
//
//  ★ 「换一换」优先给没看过的（2026-09-23 改）★
//    原先是纯随机，于是连点几次很可能又抽到看过的那张，观感就是"没换"。
//    现在维护一个"**本次打开这一页**已经看过的图"集合（m_seenThisOpen）：
//    挑图时优先从没见过的那堆里抽，14 次之内不会重复。
//    ★ 这个集合**只在内存里、不落盘**，每次切到这一页（showEvent）都重置。
//      用户要的就是"打开看着的时候不重复"，不需要跨次记忆 —— 落盘反而会
//      引入存档格式变更（新版本写进去的键老版本读不懂）这类麻烦。
//    ★ 重置时**不是清成空**，而是置成"只剩屏幕上那张"。清空的话，屏幕上明明
//      还挂着这张图，一按「换一换」就可能又抽到它，看着像按钮失效。
//
//  ★ 为什么这些图放磁盘、不进 .qrc ★
//    双击要"用图片查看器看原图"，而系统看图工具只认**真实文件路径**；
//    qrc 里的 `:/daily_image/xxx.jpg` 是虚拟路径，扔给外部程序是打不开的。
//    所以 resources/daily_image/ 保持成普通文件夹，运行时去磁盘上找。
//    附带好处：往里丢新图，重启程序就能看见 —— 不用改 .qrc、不用重新编译。
//
//  ★ 代价（得知道）★
//    图片不再编进 exe，所以把 PetPal.exe 单独拷走时这一页会是空的。
//    程序里的查找顺序见 findDailyImageDir()，找不到会在页面上写清楚找过哪些位置。
// =============================================================================

#include <QWidget>
#include <QString>
#include <QStringList>
#include <QPixmap>
#include <QSet>
#include <QSize>

class QLabel;
class QPushButton;

class DailyImagePage : public QWidget
{
    Q_OBJECT
public:
    // persistent = false 时**只读不写**存档（--selftest 用）。
    // 自检会离屏渲染这一页，如果照常写，"今天该显示哪张图"会被自检重新抽一次，
    // 等于替用户把当天的图定了 —— 和 AffectionSystem(false) 是同一个道理。
    explicit DailyImagePage(bool persistent = true, QWidget* parent = nullptr);

    // 检查"今天该显示哪张"。由 showEvent 调用（切到这一页 / 重新打开面板时都会走）。
    // 同一天里反复调用不会换图 —— 换不换由日期决定，不由调用次数决定。
    void refreshForToday();

    // 给 --selftest 用。这一页是"一天才变一次"的功能，坏了也看不出来（顶多觉得
    // "怎么每次打开都不一样"），所以把目录、图片数、存档里记的日期写进报告。
    // static 且纯只读：不建窗口、不解码图片、不碰存档 —— 自检不该为了描述它
    // 就把一张 7000px 的插画读进内存。
    static QString describeDailyImage();

protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;   // 双击图片 -> 系统看图工具

private slots:
    void onShuffle();               // 「换一换」：随机换一张，并把结果记进存档

private:
    QString        pickTodayImage();        // 今天的图（今天已经挑过就沿用，否则随机）
    QString        pickUnseen(const QStringList& files, const QString& currentName);
                                            // ★ 挑图唯一入口：优先给"本次还没看过"的
    QStringList    scanImages() const;      // 扫 daily_image 目录
    static QString findDailyImageDir();     // 找 daily_image 在哪（见 .cpp 里的说明）
    void           applyImage(const QString& path);
    void           rescaleImage();          // 按当前可用区域重新等比缩放
    QString        captionText() const;     // 底部那行：文件名 / 原图尺寸 / 双击提示
    void           updateProgressLabel();   // 标题行：本次还剩 N 张没看过

    QLabel*      m_image      = nullptr;   // 图片本体
    QLabel*      m_caption    = nullptr;   // 文件名 · 原图尺寸 · 双击提示
    QLabel*      m_progress   = nullptr;   // 本次进度（放标题行左侧，见构造函数里的说明）
    QPushButton* m_shuffleBtn = nullptr;   // 「换一换」

    QString m_dir;           // daily_image 的绝对路径（找不到时为空）
    QString m_currentPath;   // 当前这张图的绝对路径
    QPixmap m_source;        // 原图缓存（resize 时重新缩放用它，避免反复解码）
    bool    m_persistent = true;   // false = 不写存档（自检用）

    // ★ 本次打开这一页期间"已经看过的图"（存放磁盘文件名，不是下标）★
    //   存文件名而不是下标：用户随时会往 daily_image 里加/删图，
    //   下标一变标记全部错位；比名字则天然适应 ——
    //   删掉的图自然消失，**新加的图因为不在集合里，会被优先抽中**。
    //   只在内存里，showEvent 里重置（见 .cpp）。
    QSet<QString> m_seenThisOpen;
};
