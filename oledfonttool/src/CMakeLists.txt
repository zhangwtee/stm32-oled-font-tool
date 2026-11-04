cmake_minimum_required(VERSION 3.16)

# Qt Creator 20 自带的 CMake 是 4.x，对老写法会按策略报错并中止配置。
# 这一行让 4.x 按 3.5 时期的兼容策略处理。
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.27)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

project(oledmatrix LANGUAGES CXX)

# app.rc 要由 rc.exe 编译，得显式启用 RC 语言；project() 只启用了 CXX。
if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# 源码里有中文，MSVC 必须显式指定 UTF-8，否则中文字符串会变成乱码
if(MSVC)
    add_compile_options(/utf-8)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets)

# ---- 构建指纹 ----
# 把「版本号」和「配置日期」编进 exe。界面上不显示、也不写盘，只留在这份
# 二进制里，用来回答一个很实际的问题：手里这个 oledmatrix.exe 是哪个版本、
# 什么时候编出来的。
#
# 版本号从 app.rc 的 FileVersion "x.y.z" 读，跟资源里、installer 里同源，
# 不依赖 .git——课堂目录那份没有 .git 也能取到。想刷新日期，重新跑一次
# cmake 配置即可（版本号改了 .rc 也要重配置一次才会生效）。
set(OMX_VERSION "0.0.0")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/app.rc" _omx_rc)
if(_omx_rc MATCHES "FileVersion\"?[ \t]*,[ \t]*\"([0-9.]+)\"")
    set(OMX_VERSION "${CMAKE_MATCH_1}")
endif()
string(TIMESTAMP OMX_BUILD_DATE "%Y-%m-%d")
message(STATUS "build stamp: v${OMX_VERSION} ${OMX_BUILD_DATE}")
configure_file(omxbuild.h.in ${CMAKE_CURRENT_BINARY_DIR}/omxbuild.h @ONLY)

qt_add_executable(oledmatrix
    main.cpp
    fontgen.cpp
    fontpreview.cpp
    imagegen.cpp
    imagepreview.cpp
    chipinfo.cpp
    mergeproject.cpp
    screenprobe.cpp
    libprobe.cpp
    selftest.cpp
    app.rc          # 图标 + 版本信息（exe 属性里才有名字和版本号）
)

# app.rc 通过 ICON 语句引用这个文件，CMake 看不到这层依赖，单独列一下
set_source_files_properties(app.ico PROPERTIES HEADER_FILE_ONLY TRUE)
target_sources(oledmatrix PRIVATE app.ico)

target_link_libraries(oledmatrix PRIVATE Qt6::Widgets)

# omxbuild.h 是 configure_file 生成到构建目录的，不是源码目录
target_include_directories(oledmatrix PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# Windows 下不弹控制台窗口
set_target_properties(oledmatrix PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE OFF
)
