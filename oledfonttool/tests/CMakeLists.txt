cmake_minimum_required(VERSION 3.16)

# 单独配置，不挂进主工程：主工程要 Qt Widgets（会拉进整套 GUI 依赖），
# 而探测逻辑只用到 Qt Core，拆开编译快得多，也方便在没装 Qt Widgets 的机器上跑。
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.27)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

project(oledfonttool_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8 /permissive- /Zc:__cplusplus)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui)

add_executable(test_probe
    test_probe.cpp
    ../src/screenprobe.cpp
    ../src/mergeproject.cpp
)
target_include_directories(test_probe PRIVATE ../src)
target_link_libraries(test_probe PRIVATE Qt6::Core)

# 生成内容的测试要真的渲染字体，所以多一条 Gui
add_executable(test_emit
    test_emit.cpp
    ../src/fontgen.cpp
    ../src/imagegen.cpp
    ../src/selftest.cpp
)
target_include_directories(test_emit PRIVATE ../src)
target_link_libraries(test_emit PRIVATE Qt6::Gui)

# 芯片容量读取：纯文件解析，只要 Core
add_executable(test_chip
    test_chip.cpp
    ../src/chipinfo.cpp
)
target_include_directories(test_chip PRIVATE ../src)
target_link_libraries(test_chip PRIVATE Qt6::Core)

# 一键接入：会真的写文件，重点测"改完文件还是不是好的"（BOM、插入位置、撤销）
add_executable(test_merge
    test_merge.cpp
    ../src/mergeproject.cpp
)
target_include_directories(test_merge PRIVATE ../src)
target_link_libraries(test_merge PRIVATE Qt6::Core)

# 把样例驱动复制到 exe 旁边，运行时不用带参数
add_custom_command(TARGET test_probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/fixture
            $<TARGET_FILE_DIR:test_probe>/fixture)
