## 实施计划:B 站音频转存 FLAC(不抓歌词)

### 1. 引入 libFLAC 静态库
- 下载 xiph/flac 官方源码(1.5.x,BSD 协议)到 `third_party/libflac/`,只保留 `include/`、`src/libFLAC/` 和 `LICENSE`。
- `CMakeLists.txt` 新增 `add_library(flac STATIC ...)`:编译 `src/libFLAC/*.c`(约 28 个文件,不含 ogg 依赖,定义 `FLAC__NO_DLL`),头文件路径加 `include/`。不产新 DLL,pack.bat 无需改动。
- 新增的 `src/FlacEncode.h/.cpp` 按 CMakeLists 现有维护规则同步进源文件 / PETPAL_HEADERS 列表(169-220 行)。

### 2. 新增 FlacEncode 封装 — `src/FlacEncode.{h,cpp}`
- `bool encode(pcm16, channels, sampleRate, title, artist, outPath, err)`:用 libFLAC C API(stream_encoder,`init_file` 直接写文件,压缩级别 5),16bit 交错 PCM 分块 `process_interleaved`;同时把 TITLE/ARTIST 写进 VORBIS_COMMENT(`TrackMeta::parseFlac` 已支持读,曲库显示更准)。不写歌词标签。

### 3. 改造 MfDecode — `src/MfDecode.{h,cpp}`
- 现有解码循环(MfDecode.cpp:98-127)抽成同步内核 `decodePcm()`;删除 WAV 头打包段(134-156 行)和 putU32/putU16。
- 新增入口 `decodeToFlacAsync(QObject* ctx, input, outputFlac, title, artist, cb)`:在 `std::thread` 里跑「解码 → libFLAC 编码」,完成后用 `QMetaObject::invokeMethod(ctx, ...)`(AudioPlayer.cpp:808 已有同款写法)把回调派回主线程,ctx 失效自动丢弃——转码移出主线程,避免 UI 卡顿。
- 头文件注释同步更新(「解成 FLAC,miniaudio 原生播放」)。

### 4. PlayerPage 接线 — `src/PlayerPage.cpp`
B 站分支(2868-2919 行):
- 输出 `base + ".flac"`;缓存命中改为先查 `.flac`、再兼容查老 `.wav`(已收藏的歌不重新下载)。
- 下载完 m4a 后设行标签「转码中」→ `decodeToFlacAsync` → 回调里删中间产物、`playLocalWav(it, flacPath, ...)`。
- `startDl` 里遗留的 m4s 分支(2782-2797 行)同步改为 FLAC 输出,保持一致。
- `playLocalWav` 逻辑不动(miniaudio 直接播 FLAC;TrackMeta 自动读 FLAC 时长与 TITLE/ARTIST 标签)。

### 5. 验证
- CMake 构建(链接 flac 目标)通过。
- 运行检查:B 站搜歌双击 → 储存盘出现 `.flac`、正常播放、曲库显示标题/歌手;再点同首 → 缓存命中不重新下载;老 `.wav` 收藏仍可播。