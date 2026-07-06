cmake_minimum_required(VERSION 3.20)
project(webcpp-tests LANGUAGES CXX C)

# ── C++20 Standard + Coroutines ──
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fcoroutines")
endif()

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
find_package(OpenSSL REQUIRED)
find_package(PkgConfig QUIET)

# ── Project include root (to resolve #include "module/file.hpp") ──
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/..)

# ── db_test ──
add_executable(db_test
    ../rpc/database_test.cpp
    ../rpc/database.cpp
)

target_link_libraries(db_test PRIVATE
    asio Threads::Threads sqlite3)

# ── ws_test (H2 WebSocket client test) ──
if(PkgConfig_FOUND)
    pkg_check_modules(NGHTTP2 libnghttp2)
endif()

if(NGHTTP2_FOUND)
    add_executable(ws_test
        ../http/http2/ws_test.cpp
    )
    target_link_libraries(ws_test PRIVATE
        asio Threads::Threads OpenSSL::SSL OpenSSL::Crypto
        ${NGHTTP2_LIBRARIES})
    target_include_directories(ws_test PRIVATE
        ${NGHTTP2_INCLUDE_DIRS})
else()
    message(WARNING "libnghttp2 not found — ws_test will not be built")
endif()
