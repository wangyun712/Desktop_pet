cmake_minimum_required(VERSION 3.24)

project(PetPal LANGUAGES C CXX)

# ---------------------------------------------------------------------------
#  ★ 头文件依赖的治本开关：不要让编译器去生成依赖 ★
#
#  默认（TRUE）时，Makefile / JOM 生成器靠 MSVC 的 /showIncludes 输出来算依赖。
#  本机这套环境里那个输出没被写进 *.cpp.obj.d（实测全是 0 字节），
#  于是 JOM 完全不知道 .obj 依赖哪些头文件 —— 改了头文件，只有"自身也一起改过"
#  的 .cpp 会重编，其余不重编。
#
#  这不是"改代码没生效"这么轻的事：改一个类的成员变量后新旧混编，
#  构造函数写越界堆块（new 的 sizeof 来自旧头文件），Debug 版直接报
#  "HEAP CORRUPTION DETECTED ... wrote to memory after end of heap buffer"。
#  代码一行没错，却能崩给你看。
#
#  设成 FALSE = 回到 CMake 3.20 之前的行为，用 CMake 自己的依赖扫描器，可靠。
#  见文件后半段那段"头文件依赖"注释，那里还有第二道保险。
# ---------------------------------------------------------------------------
set(CMAKE_DEPENDS_USE_COMPILER FALSE)

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
    src/AffectionSystem.h
    src/AffectionSystem.cpp
    src/MainPanel.h
    src/MainPanel.cpp
    src/AffectionPage.h
    src/AffectionPage.cpp
    src/SettingsPage.h
    src/SettingsPage.cpp
    src/DailyImagePage.h
    src/DailyImagePage.cpp
    src/ChatScript.h
    src/SongLibrary.h
    src/ChatPage.h
    src/ChatPage.cpp
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
#  ★ 头文件依赖（第二道保险）—— 这段别删 ★
#
#  症状一（轻）：改了头文件里的常量，exe 里还是旧值，"明明改了却没生效"。
#  症状二（重，2026-09-23 真实踩到）：改 MainPanel.h 加了成员变量后，
#    MainPanel.cpp 重编了、而 DesktopPet.cpp 没重编 —— 于是 DesktopPet.cpp 里
#    `new MainPanel(...)` 用的是**旧的 sizeof**（编译期常量），
#    分配出来的块比构造函数要写的小，构造函数往块尾之外写成员。
#    Debug 版 CRT 当场报：
#      HEAP CORRUPTION DETECTED: after Normal block (#xxxxx) ...
#      CRT detected that the application wrote to memory after end of heap buffer.
#    这种崩溃最难查 —— 代码一行都没写错，错的是"编译单元之间的版本不一致"。
#
#  根因：JOM（NMake Makefiles JOM，Qt Creator 用的生成器）下，
#  MSVC 的 /showIncludes 依赖没被写进 .d 文件（实测 *.cpp.obj.d 全是 0 字节）。
#  JOM 于是完全不知道 .obj 依赖哪些头文件，只有"那个 .cpp 自身也改过"时才重编。
#  Ninja 用二进制 depfile，不受影响 —— 所以同一份代码两条构建路径表现不同。
#
#  两道保险：
#    ① 文件开头的 `set(CMAKE_DEPENDS_USE_COMPILER FALSE)` —— 让 CMake 用它自己的
#       依赖扫描器，不再指望编译器的 /showIncludes 输出。这是治本。
#    ② 这段：显式给每个 .cpp 挂上 src/ 下所有头文件。不依赖任何自动扫描机制，
#       就算哪天开关又失效，JOM 也照样会重编。
#
#  为什么逐个文件列、而不用 target 级 OBJECT_DEPENDS？
#  因为 target 级会把自动生成的 13MB 的 qrc_resources.cpp 也算进来 ——
#  它不 include 任何项目头文件，却会因改一个常量就被重编十几秒。
#
#  ★ 维护规则：新增 .cpp 往下面列表补一行；新增 .h 往 PETPAL_HEADERS 补一行。★
#    漏了也不会报错，只会退化成"偶尔要手动全量重编"——所以宁可麻烦一点。
# ---------------------------------------------------------------------------
set(PETPAL_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/PetConfig.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/AnimationController.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/PetBehaviorController.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/DesktopPet.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/AffectionSystem.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/MainPanel.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/AffectionPage.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/SettingsPage.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/DailyImagePage.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ChatScript.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/SongLibrary.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ChatPage.h"
)

set_property(SOURCE
    src/main.cpp
    src/AnimationController.cpp
    src/PetBehaviorController.cpp
    src/DesktopPet.cpp
    src/AffectionSystem.cpp
    src/MainPanel.cpp
    src/AffectionPage.cpp
    src/SettingsPage.cpp
    src/DailyImagePage.cpp
    src/ChatPage.cpp
    APPEND PROPERTY OBJECT_DEPENDS ${PETPAL_HEADERS}
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
