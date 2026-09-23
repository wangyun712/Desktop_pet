cmake_minimum_required(VERSION 3.24)

project(PetPal LANGUAGES C CXX)

# ---------------------------------------------------------------------------
#  C++ 标准
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
#  Qt 自动化工具
#  AUTOMOC：只要头文件里写了 Q_OBJECT，就需要 moc 生成元对象代码（信号槽靠它）。
#          不开的话会报 "undefined reference to vtable" 之类的链接错误。
#  AUTORCC：自动把 resources.qrc 编译进目标，全部 PNG 就是靠它打包的。
# ---------------------------------------------------------------------------
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# ---------------------------------------------------------------------------
#  找 Qt6
#  Core / Gui / Widgets 三个模块就够：Widgets 提供 QWidget 那套传统桌面控件，
#  本项目不使用 QML。
# ---------------------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

# ---------------------------------------------------------------------------
#  源文件
#  这里故意一个个列出来，而不是用 file(GLOB ...)：
#  新增 .cpp 时你会立刻意识到"要在这里加一行"，也不会出现
#  "明明加了文件却编译不进去"这种让人抓狂的问题。
# ---------------------------------------------------------------------------
set(PROJECT_SOURCES
    src/main.cpp
    src/PetConfig.h
    src/AnimationController.h
    src/AnimationController.cpp
    src/PetBehaviorController.h
    src/PetBehaviorController.cpp
    src/DesktopPet.h
    src/DesktopPet.cpp
)

set(PROJECT_RESOURCES
    resources/resources.qrc
)

# ---------------------------------------------------------------------------
#  可执行文件
#  WIN32 关键字 = 使用 GUI 子系统：双击运行时不会先弹出一个黑色控制台窗口。
# ---------------------------------------------------------------------------
add_executable(PetPal WIN32
    ${PROJECT_SOURCES}
    ${PROJECT_RESOURCES}
)

# src 作为头文件搜索根目录，这样代码里就能写 #include "DesktopPet.h"
target_include_directories(PetPal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(PetPal PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

# ---------------------------------------------------------------------------
#  ★ 头文件依赖 —— 这一行是"踩坑补丁"，别删 ★
#
#  PetConfig.h 是本项目唯一的公共头文件（状态枚举 + 全部可调参数 + 帧序列表），
#  每个 .cpp 都 include 它。正常情况 CMake 会自动扫描 #include 生成依赖，
#  但在本机用 JOM（NMake Makefiles JOM，也正是 Qt Creator 用的那个生成器）时，
#  MSVC 的 /showIncludes 依赖没有被写进 .d 文件 ——
#  实测 DesktopPet.cpp.obj.d / AnimationController.cpp.obj.d / main.cpp.obj.d
#  全都是 0 字节。于是 JOM 完全不知道这些 .obj 依赖 PetConfig.h：
#  改了 PetConfig.h，除了自身也一起改过的那个 .cpp，其余编译单元一律不重编，
#  链接出来的 exe 里内联的还是旧常量，表现为"明明改了代码却一点没生效"。
#
#  真实踩坑记录：PetCfg::PET_IMAGE_COUNT 从 29 改成 30（新增拖拽图 p30）后，
#  Qt Creator 构建出的 exe 自检仍报"应加载图片 29 张"，新图怎么都不显示；
#  而 Ninja（用二进制 depfile 数据库）构建的 exe 一切正常 —— 差别就在这里。
#
#  下面这几行给这几个 .cpp 显式挂上"依赖 PetConfig.h"，不依赖任何自动扫描机制，
#  Ninja 和 JOM 都会老老实实重编。
#
#  为什么逐个文件列、而不用 target 级的 OBJECT_DEPENDS？
#  因为 target 级会把自动生成的 13MB 的 qrc_resources.cpp 也算进来 ——
#  它根本不 include PetConfig.h，却会因为改一个常量就被重编十几秒。
#  逐文件列之后，"改个参数"只需要重编这 4 个 .cpp。
#  以后新增 .cpp 且用到了 PetConfig.h，记得往这里补一行。
# ---------------------------------------------------------------------------
set_property(SOURCE
    src/main.cpp
    src/AnimationController.cpp
    src/PetBehaviorController.cpp
    src/DesktopPet.cpp
    APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/PetConfig.h"
)

# ---------------------------------------------------------------------------
#  MSVC 专属设置
#
#  /utf-8：告诉编译器"源文件是 UTF-8"。
#          本项目所有源码都保存为 UTF-8（无 BOM），中文注释才不会乱码。
#          Qt6 的 CMake 包通常会自动加上，这里显式写一遍更保险。
#
#  /wd4828：屏蔽 C4828 警告。
#          rcc（资源编译器）生成的 .cpp 里，会把中文文件名写成一行注释，
#          而这一行是按系统本地编码（中文 Windows 是 GBK）写的，
#          在 /utf-8 下就属于"非法 UTF-8 字节"，于是每张图刷一条警告。
#          它只影响注释，完全无害，所以关掉它让编译输出干净一些。
# ---------------------------------------------------------------------------
if (MSVC)
    target_compile_options(PetPal PRIVATE /utf-8 /wd4828)
endif()
