#pragma once
// =============================================================================
//  DailyImagePage —— 主面板里的「每日图片」页
//
//  三个标签，各自独立：
//    · 本地  —— 原有功能：resources/daily_image 里"每日固定一张"，换一换不重复
//    · Pixiv —— 从 Pixiv 搜索取图（safe 分级 + 排除 AI，见 PixivFetcher）
//    · 堆糖  —— 从堆糖搜索取图（卡片即内容图，没有"封面 ≠ 内容"的问题）
//  每个联网标签有自己独立的池/缓存/当前图（m_online[0/1]），互不串。
//
//  ★ 本地标签：「换一换」优先给没看过的（2026-09-23 改）★
//    维护一个"**本次打开这一页**已经看过的图"集合（m_seenThisOpen）：
//    挑图时优先从没见过的那堆里抽，14 次之内不会重复。
//    ★ 这个集合**只在内存里、不落盘**，每次切到本地标签（showEvent）都重置。
//    ★ 重置时**不是清成空**，而是置成"只剩屏幕上那张"。
//
//  ★ 为什么图片放磁盘、不进 .qrc ★
//    双击要"用图片查看器看原图"，而系统看图工具只认**真实文件路径**；
//    qrc 里的虚拟路径外部程序打不开。resources/daily_image/ 保持成普通文件夹。
//    联网标签双击保存的图也落在这里，自动进入本地图库。
// =============================================================================

#include <QWidget>
#include <QString>
#include <QStringList>
#include <QPixmap>
#include <QSet>
#include <QSize>
#include <QByteArray>

#include "DuitangFetcher.h"
#include "PixivFetcher.h"

class QLabel;
class QPushButton;
class QTimer;
class QButtonGroup;

class DailyImagePage : public QWidget
{
    Q_OBJECT
public:
    // persistent = false 时**只读不写**存档（--selftest 用）。
    // 自检会离屏渲染这一页，如果照常写，"今天该显示哪张图"会被自检重新抽一次，
    // 等于替用户把当天的图定了 —— 和 AffectionSystem(false) 是同一个道理。
    explicit DailyImagePage(bool persistent = true, QWidget* parent = nullptr);

    // 界面字号档位变了之后重新套样式表（见 UiFont.h）
    void applyUiScale();

    // 检查"本地标签今天该显示哪张"。由 showEvent 调用（切到这一页都会走）。
    // 同一天里反复调用不会换图 —— 换不换由日期决定，不由调用次数决定。
    void refreshForToday();

    // 给 --selftest 用。纯只读：不建窗口、不解码图片、不碰存档、不触网。
    static QString describeDailyImage();

protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;   // 双击图片 -> 保存/打开

private slots:
    void onShuffle();               // 「换一换」：按当前标签各自的方式换一张

private:
    // ---- 联网标签（Pixiv / 堆糖）：每个源一份独立状态 ----
    enum { SourcePixiv = 0, SourceDuitang = 1 };

    struct OnlineState
    {
        QVector<PixivFetcher::Illust> pool;   // 一次搜索的结果（已乱序）
        int  poolPos    = 0;                  // 池消费到哪了
        struct OnlinePic
        {
            PixivFetcher::Illust info;
            QByteArray  bytes;                // 原始字节（双击保存用）
            QPixmap     pm;                   // 解码结果（显示用）
        };
        QVector<OnlinePic> cache;             // 预取好的图
        bool tried     = false;               // 本次运行是否已经搜过
        bool fetching  = false;               // 有一张图正在取（防重入）
        bool awaitShow = false;               // 用户点了换一换但缓存还空 → 到货自动上屏
        int  failRun   = 0;                   // 连续失败计数（≥3 才报错，偶发失败静默跳过）
        int  gen       = 0;                   // 回调流水号：重搜/切标签时 +1，过期回调按它丢弃
        int  duitangTotal = 0;                // 堆糖当前关键词命中总数（随机跳页用）

        QByteArray  bytes;                    // 当前显示这张的原始字节（双击保存用）
        QString     saveId;                   // 当前显示这张的保存名（pixiv_xxx / duitang_xxx）
        QString     title;                    // 底栏文字用
        QString     artist;
        QPixmap     source;                   // 显示缓存（切标签往返不丢）
        QSize       size;                     // 原图尺寸
    };

    void setTab(int id);                 // 0 = 本地,1 = Pixiv,2 = 堆糖
    void beginOnlineSearch(int duitangStart = -1);   // 按当前图源搜一批（duitangStart ≥0 = 指定堆糖偏移）
    void ingestOnline(int srcIdx, QVector<PixivFetcher::Illust> list);   // 乱序入池 + 上第一张
    void prefetchNext(int srcIdx);       // 从池里取下一个：补齐直链 → 字节 → 进缓存
    void handleOnlineBytes(int srcIdx, int gen, const PixivFetcher::Illust& info,
                           bool ok, const QByteArray& bytes, const QString& err);
    void showNextOnline(int srcIdx);     // 从缓存弹一张显示；缓存空则催补货
    void applyOnlineImage(int srcIdx);   // 把当前联网图放进显示管线（走 m_source 那套缩放）
    QString onlineCaption(const OnlineState& st) const;   // 联网图底栏文字

    void           applyStyle();            // 整页样式表（构造时和改字号时共用）
    QString        pickTodayImage();        // 今天的图（今天已经挑过就沿用，否则随机）
    QString        pickUnseen(const QStringList& files, const QString& currentName);
                                            // ★ 挑图唯一入口：优先给"本次还没看过"的
    QStringList    scanImages() const;      // 扫 daily_image 目录
    static QString findDailyImageDir();     // 找 daily_image 在哪（见 .cpp 里的说明）
    void           applyImage(const QString& path);
    // 按当前可用区域重新等比缩放。
    //   smooth=false 走最近邻（几毫秒，拖动窗口时用来保持跟手），
    //   smooth=true 走平滑（最终成品，由 m_rescaleTimer 在停手后触发）。
    void           rescaleImage(bool smooth = true);
    QString        captionText() const;     // 底部那行：文件名 / 原图尺寸 / 双击提示
    void           updateProgressLabel();   // 标题行：本次还剩 N 张没看过

    QLabel*      m_image      = nullptr;   // 图片本体
    QLabel*      m_caption    = nullptr;   // 文件名 · 原图尺寸 · 双击提示
    QLabel*      m_progress   = nullptr;   // 本次进度（本地标签专属）
    QPushButton* m_shuffleBtn = nullptr;   // 「换一换」

    QString m_dir;           // daily_image 的绝对路径（找不到时为空）
    QString m_currentPath;   // 当前这张图的绝对路径（联网标签下为空）
    // ★ m_source 是"够用"的那一版，不一定是原图 ★
    //   用户的插画动辄几千像素（本机实测有 7000px 的）。每换一次窗口大小就从原图
    //   平滑缩一次是"目标像素数 × 缩放倍数"级别的开销，而窗口一变就会连着来
    //   几十上百次 —— 面板就卡住了。所以载入后先按"屏幕的最大物理尺寸"降采样一次
    //   （面板最多也就铺满屏幕，再清晰也没地方显示），之后所有缩放都从这份出发。
    //   原图尺寸单独记在 m_sourceSize 里，给底部那行文字用。
    QPixmap m_source;        // 显示用的缓存（可能已降采样）
    QSize   m_sourceSize;    // 原图尺寸（只用于底栏显示）
    QTimer* m_rescaleTimer = nullptr;   // 缩放节流：停手后才做一次平滑缩放
    bool    m_persistent = true;   // false = 不写存档（自检用）

    // ★ 本地标签：本次打开期间"已经看过的图"（存放磁盘文件名，不是下标）★
    //   存文件名而不是下标：用户随时会往 daily_image 里加/删图，比名字天然适应。
    //   只在内存里，showEvent 里重置（见 .cpp）。
    QSet<QString> m_seenThisOpen;

    // ---- 三个标签的骨架与联网状态 ----
    //  每个联网源一份 OnlineState：池/缓存/当前图互不串，切标签就是切状态。
    //  ★ 联网态全部内存态、不落盘 ★ —— 联网是"看图"，没有"每日固定一张"的语义。
    QButtonGroup* m_tabGroup   = nullptr;   // 本地 / Pixiv / 堆糖 三选一
    QPushButton*  m_localTab   = nullptr;
    QPushButton*  m_pixivTab   = nullptr;
    QPushButton*  m_duitangTab = nullptr;
    int           m_tab        = 0;         // 当前标签（0 本地 / 1 Pixiv / 2 堆糖，
                                            //  ui.ini dailyImage/tab 记忆）
    OnlineState   m_online[2];               // [SourcePixiv] / [SourceDuitang]
};
