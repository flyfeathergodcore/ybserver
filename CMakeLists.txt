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
    target_compile_definitions(asio INTERFACE ASIO_STANDALONE
        ASIO_RECYCLING_ALLOCATOR_CACHE_SIZE=16)
else()
    message(FATAL_ERROR "Asio not found. Install: sudo apt install libasio-dev")
endif()

# ── Dependencies ──
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(NGHTTP2 REQUIRED libnghttp2 IMPORTED_TARGET)
find_package(OpenSSL REQUIRED)

# ── Project include root (so #include "module/file.hpp" works) ──
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# ── http_server (main executable) ──
add_executable(http_server
    main.cpp
    net/server.cpp
    http/http1.1/session_pool.cpp
    net/multi_server.cpp
    handler/metrics.cpp
    handler/router.cpp
    handler/reverse_proxy.cpp
    handler/upstream_pool.cpp
    http/http1.1/session.cpp
    net/response.cpp
    net/region_pool.cpp
    net/session_region.cpp
    ssl/tls_context.cpp
    http/http1.1/parser.cpp
    http/protocol.cpp
    http/http2/session.cpp
    http/http2/stream_context.cpp
    handler/request_handler.cpp
    middleware/middleware.cpp
    cache/file_cache.cpp
    config/config.cpp
    rpc/database.cpp
    rpc/connection_pool.cpp
)

target_link_libraries(http_server PRIVATE
    asio Threads::Threads yaml-cpp sqlite3
    OpenSSL::SSL OpenSSL::Crypto
    PkgConfig::NGHTTP2)

# ── db_test ──
add_executable(db_test
    rpc/database_test.cpp
    rpc/database.cpp
)

target_link_libraries(db_test PRIVATE
    asio Threads::Threads sqlite3)
