cmake_minimum_required(VERSION 3.16)

# Qt Creator 20 自带的 CMake 是 4.x，对老写法会按策略报错并中止配置。
# 这一行让 4.x 按 3.5 时期的兼容策略处理。
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.27)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

project(oledfonttool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# 源码里有中文，MSVC 必须显式指定 UTF-8，否则中文字符串会变成乱码
if(MSVC)
    add_compile_options(/utf-8)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_add_executable(oledfonttool
    main.cpp
    fontgen.cpp
    fontpreview.cpp
    imagegen.cpp
    imagepreview.cpp
    chipinfo.cpp
    mergeproject.cpp
    screenprobe.cpp
    selftest.cpp
)

target_link_libraries(oledfonttool PRIVATE Qt6::Widgets)

# Windows 下不弹控制台窗口
set_target_properties(oledfonttool PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE OFF
)
