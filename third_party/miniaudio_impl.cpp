// =============================================================================
//  miniaudio 的"实现单元"—— 这一个 .cpp 存在的唯一理由
//
//  ★ 为什么单独一个文件、不写在 AudioPlayer.cpp 里 ★
//    miniaudio 是单头文件库，但它把"声明"和"实现"分开了：
//      只写 #include "miniaudio.h"          -> 拿到全部函数声明，很便宜
//      #define MINIAUDIO_IMPLEMENTATION 后再 include -> 展开那 4MB 的实现代码
//    后者编一次要十几秒。如果写在 AudioPlayer.cpp 里，那么"改一行播放器逻辑"
//    就要重编这 4MB —— 开发时每改一次等十几秒，很快就没人愿意改了。
//    拆出来之后，miniaudio.h 没动，这个文件就永远不会重编。
//
//  ★ 为什么不用改 CMakeLists 去链 libwinmm / ole32 ★
//    miniaudio 在 Windows 上是**运行时**动态加载后端 API 的（LoadLibrary 那一套），
//    不需要在链接期挂任何额外的库。所以这里就只是一个纯粹的编译单元。
// =============================================================================

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

