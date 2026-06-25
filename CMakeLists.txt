cmake_minimum_required(VERSION 3.20)
project(webcpp LANGUAGES CXX C)

# ── C++20 标准 + 协程 ──
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# GCC 需要显式开启 -fcoroutines
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fcoroutines")
endif()

# 生成 compile_commands.json（VS Code IntelliSense 用）
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── 查找 Asio（standalone 模式，仅头文件） ──
find_path(ASIO_INCLUDE_DIR asio.hpp
    PATHS /usr/include /usr/local/include
    NO_DEFAULT_PATH)
if(NOT ASIO_INCLUDE_DIR)
    find_path(ASIO_INCLUDE_DIR asio.hpp)  # 回退到默认路径
endif()

if(ASIO_INCLUDE_DIR)
    message(STATUS "Found Asio: ${ASIO_INCLUDE_DIR}")
    add_library(asio INTERFACE)
    target_include_directories(asio INTERFACE ${ASIO_INCLUDE_DIR})
    target_compile_definitions(asio INTERFACE ASIO_STANDALONE)
else()
    message(FATAL_ERROR "Asio not found. Install: sudo apt install libasio-dev")
endif()

# ── 线程库 ──
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)

# ── 辅助函数：添加一个可执行文件 ──
function(add_phase_exec PHASE_NAME SOURCE_FILE)
    get_filename_component(TARGET_NAME ${SOURCE_FILE} NAME_WE)
    add_executable(${TARGET_NAME} ${SOURCE_FILE})
    target_link_libraries(${TARGET_NAME} PRIVATE asio Threads::Threads)
    # 加个文件夹分组，VS Code 里好看
    set_target_properties(${TARGET_NAME} PROPERTIES FOLDER ${PHASE_NAME})
endfunction()

# ── Phase 1: 协程入门 + Echo Server ──
file(GLOB PHASE1_SOURCES phase1/*.cpp)
foreach(SRC ${PHASE1_SOURCES})
    add_phase_exec("phase1" ${SRC})
endforeach()

# ── Phase 2: HTTP 解析 + 静态文件服务器 ──
find_package(yaml-cpp REQUIRED)
add_executable(http_server
    phase2/main.cpp
    phase2/httpcontext/llhttp_context.cpp
    phase2/httpcontext/llhttp.c
    phase2/httpcontext/api.c
    phase2/httpcontext/http.c
    phase2/config.cpp
    phase2/file_cache.cpp)
target_link_libraries(http_server PRIVATE asio Threads::Threads yaml-cpp sqlite3)
set_target_properties(http_server PROPERTIES FOLDER phase2)

# ── Phase 3: 数据库 ──
add_executable(db_test phase3/database_test.cpp
    phase3/database.cpp)
target_link_libraries(db_test PRIVATE asio Threads::Threads sqlite3)
set_target_properties(db_test PROPERTIES FOLDER phase3)

# file(GLOB PHASE4_SOURCES phase4/*.cpp)
# foreach(SRC ${PHASE4_SOURCES})
#     add_phase_exec("phase4" ${SRC})
# endforeach()
