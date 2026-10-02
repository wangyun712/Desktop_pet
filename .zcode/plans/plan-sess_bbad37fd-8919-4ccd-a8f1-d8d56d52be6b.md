## 实施计划:每日图片加"联网(Pixiv)"标签页

### 1. 新增 src/PixivFetcher.{h,cpp}(照 OnlineMusic 的命名空间+回调风格)
- search(keyword, page, ctx, cb):GET https://www.pixiv.net/ajax/search/artworks/{关键词URL编码}?s_mode=tag&type=illust&mode=safe&ai_type=1&wlt=3000&hlt=3000&p={page}(参数与你给的搜索头一一对应;浏览器 UA + Referer: https://www.pixiv.net/)。解析 body.illustManga[] → {illustId, title, userName} 列表(QJson 链式取值照 OnlineMusic 先例)。
- fetchIllust(id, ctx, cb):GET /ajax/illust/{id} → urls.regular(master1200 清晰度,面板显示与保存都够用)+ pageCount。
- fetchBytes(url, ctx, cb):GET 图片字节(i.pximg.net 必须带 Referer,不然 403)→ QByteArray。
- 三个函数都带 QObject* ctx 宿主守卫(照 hostAlive 先例,宿主析构丢弃回调)。

### 2. DailyImagePage 改造(本地标签完全不动)
- 标题行:「本地」/「联网」两个 checkable 按钮(QButtonGroup 互斥,照 PlayerPage"网易云/B站"先例),样式加进 DailyImagePage::applyStyle()(playerFav 同套色值,objectName=dailyTab 防串页);右侧"换一换"按钮两个标签共用。本地模式下原进度文案、"本次还剩 N 张"逻辑原样保留。
- 联网模式:
  - 首次切到联网 → 随机页码搜索(结果乱序)→ 依次取图:先 fetchIllust 拿大图地址,再 fetchBytes 拉字节;后台预取 5 张进内存缓存,显示时零等待;每消费一张自动补拉一张。
  - 取图中 caption 显示"正在从 Pixiv 取图…";失败(403/超时——需要能访问 pixiv,Qt 走系统代理)显示原因 + "点换一换重试"。
  - 底栏 caption:标题 · 画师 · 双击保存到本地图库并打开。
- 双击(联网模式):把缓存的图片字节存为 resources/daily_image/pixiv_{illustId}_p{页}.jpg(已存在则跳过下载直接开)→ QDesktopServices::openUrl 系统工具打开。落盘的图自动进入本地池(现有扫描机制),标题行进度随之变化。
- 自检(persistent=false)不触网,行为不变;describe 提示联网标签仅运行期有效。

### 3. 工程登记
- CMakeLists.txt 三处按维护规则登记新文件(PROJECT_SOURCES / PETPAL_HEADERS / OBJECT_DEPENDS)。
- pack.bat 无需改动(daily_image 目录本就随包发放,联网下载的图落这里)。

### 4. 验证
- 手动:切联网标签出图 → 换一换连续换 5+ 张 → 双击保存并打开 → 本地标签"换一换"能抽到刚存的图;断网/代理关掉时给明确失败提示且本地标签不受影响。
- --selftest 回归(每日图片节仍是本地只读断言)。

### 边界说明
搜索关键词先写死为"洛天依 -AI生成 -涩"(safe 分级 + 排除 AI);做成设置可改的关键词属于后续增强,这轮不做。Pixiv 从数据中心访问是 403(已实测),用户机器走浏览器/系统代理可达即可用;失败路径有明确提示。