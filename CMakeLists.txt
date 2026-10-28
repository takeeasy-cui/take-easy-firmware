# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

# 版本：V1.0 于 2026-10-21 正式发布（用户 2026-10-17 钦定固定至定稿）。
# 定稿后升版：1.0.1 = 三 bug 修复 + SD 异步统计 + 目录标题；
# 1.0.2 = 分界线残影对症（窄带周期全驱）+ WiFi 密码键盘下移贴底；
# 1.0.3 = 阅读自动旋转 + 逆转裁判六项修复 + 锁屏提速 + EPUB 兼容 + 电量稳定；
# 1.0.4 = 习测题库应用 + 对话游戏/互动小说双引擎发布。
# 1.0.5 = 真机反馈四修（阅读目标 15 分钟步进/翻页柔和灰边无锯齿/时钟锁屏选项页与竖屏排版/画皮四连修）
#         + rc101 移植（ZIP64/字体目录 128）+ 防褪色重刷 + 运行期时钟校准 + 互动小说字号复用书阅读。
# ota_update.c 的 TF 升级校验接受 "1.0" 前缀，1.0.x 全系互通。
# 发布铁律（2026-10-25 教训）：站点 manifest.version 升版时必须同步本处
# PROJECT_VER——刷机页会校验固件 esp_app_desc 内部版本，漏升即被拦截。
if(DEFINED ENV{PICO_PROJECT_VER} AND NOT "$ENV{PICO_PROJECT_VER}" STREQUAL "")
    set(PROJECT_VER "$ENV{PICO_PROJECT_VER}")
else()
    set(PROJECT_VER "1.0.5")
endif()
project(Read_Pico)

# 固件描述时间统一为 UTC，与概览页标识一致；保留已有编译缓存启动器。
# Compile app-description timestamps in UTC to match Overview; preserve any compiler-cache launcher.
idf_component_get_property(app_desc_lib esp_app_format COMPONENT_LIB)
get_target_property(app_desc_launcher ${app_desc_lib} C_COMPILER_LAUNCHER)
if(NOT app_desc_launcher)
    set(app_desc_launcher "")
endif()
set(app_desc_utc_launcher "${CMAKE_COMMAND}" -E env TZ=UTC)
if(app_desc_launcher)
    list(APPEND app_desc_utc_launcher ${app_desc_launcher})
endif()
set_property(TARGET ${app_desc_lib} PROPERTY C_COMPILER_LAUNCHER "${app_desc_utc_launcher}")

# panic 包装器的链接选项在 main/CMakeLists.txt 里挂到 main 组件上：写在 project() 之后
# 用 idf_build_set_property 设全局链接选项已经太晚，链接器看不到 --wrap。
# The panic wrapper's link option hangs off the main component in main/CMakeLists.txt: setting a
# global link option after project() is too late for the linker to see --wrap.
