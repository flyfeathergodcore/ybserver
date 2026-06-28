cmake_minimum_required(VERSION 3.20)
project(webcpp LANGUAGES CXX C)

# ── Release build with optimizations ──
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ── C++20 Standard + Coroutines ──
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fcoroutines")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Asio (standalone, header-only) ──
find_path(ASIO_INCLUDE_DIR asio.hpp
    PATHS /usr/include /usr/local/include
    NO_DEFAULT_PATH)
if(NOT ASIO_INCLUDE_DIR)
    find_path(ASIO_INCLUDE_DIR asio.hpp)
endif()

if(ASIO_INCLUDE_DIR)
    message(STATUS "Found Asio: ${ASIO_INCLUDE_DIR}")
    add_library(asio INTERFACE)
    target_include_directories(asio INTERFACE ${ASIO_INCLUDE_DIR})
    target_compile_definitions(asio INTERFACE ASIO_STANDALONE)
else()
    message(FATAL_ERROR "Asio not found. Install: sudo apt install libasio-dev")
endif()

# ── Dependencies ──
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(yaml-cpp REQUIRED)

# ── Project include root (so #include "module/file.hpp" works) ──
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# ── http_server (main executable) ──
add_executable(http_server
    main.cpp
    net/server.cpp
    net/session_pool.cpp
    net/multi_server.cpp
    net/metrics.cpp
    net/session.cpp
    net/response.cpp
    net/region_pool.cpp
    net/session_region.cpp
    net/tls_context.cpp
    http/h1_parser.cpp
    http/protocol.cpp
    handler/request_handler.cpp
    middleware/middleware.cpp
    cache/file_cache.cpp
    config/config.cpp
    rpc/database.cpp
    rpc/connection_pool.cpp
)

find_package(OpenSSL REQUIRED)

target_link_libraries(http_server PRIVATE
    asio Threads::Threads yaml-cpp sqlite3 OpenSSL::SSL OpenSSL::Crypto)

# ── db_test ──
add_executable(db_test
    rpc/database_test.cpp
    rpc/database.cpp
)

target_link_libraries(db_test PRIVATE
    asio Threads::Threads sqlite3)
