# ── 热插拔 .so handler 构建 ──
#
# 每个 .so 源文件编译成一个独立的动态库。
# 编译时必须使用与主程序相同的 C++ ABI（同一编译器 + 同一组头文件）。
# target_include_directories 和 target_link_libraries 需要与主程序一致。

# ── 工具函数：为 C++ 源文件自动添加 MODULE 目标 ──
# MODULE = 不产生 lib 前缀，不加 soname，适合 dlopen
include(GNUInstallDirs)

function(add_hotplug_handler src)
    get_filename_component(name ${src} NAME_WLE)
    add_library(${name} MODULE ${src})
    # 共享主项目的头文件路径（确保 ABI 一致）
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR})
    # 链接 asio（header-only，通过 INTERFACE 库传递编译定义）
    target_link_libraries(${name} PRIVATE asio)
    # 输出到统一目录，便于服务器扫描
    set_target_properties(${name} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/handlers"
    )
endfunction()

# ── 所有 .so handler ──
add_hotplug_handler(example_handler.cpp)
