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
        ASIO_RECYCLING_ALLOCATOR_CACHE_SIZE=16
        ASIO_HAS_IO_URING)
else()
    message(FATAL_ERROR "Asio not found. Install: sudo apt install libasio-dev")
endif()

# ── Dependencies ──
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
find_library(LIBURING_LIB uring)

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
    net/sse_push.cpp
    net/region_pool.cpp
    net/session_region.cpp
    ssl/tls_context.cpp
    http/http1.1/parser.cpp
    http/protocol.cpp
    http/http2/session.cpp
    http/http2/stream_context.cpp
    http/http2/parser/HPACK.cpp
    http/http2/parser/stream_manager.cpp
    http/http2/parser/flow_control.cpp
    handler/request_handler.cpp
    handler/health.cpp
    handler/upstream_conn_pool.cpp
    middleware/middleware.cpp
    cache/file_cache.cpp
    config/config.cpp
    net/ws_frame.cpp
    db/database.cpp
    db/connection_pool.cpp
    rpc/grpc_bridge.cpp
    rpc/grpc_channel_pool.cpp
    rpc/types.hpp
    rpc/service_discovery.cpp
    rpc/load_balancer.cpp
    rpc/retry_policy.cpp
    rpc/rpc_module.cpp
    handler/loader.cpp
    handler/reloader.cpp
)

# ── gRPC RPC 基础设施 ──
add_subdirectory(rpc)

# ── 热插拔 .so handler 子项目 ──
# 所有 handler 源文件在 examples/cpp_handlers/ 下
include(GNUInstallDirs)

function(add_hotplug_handler src)
    get_filename_component(name ${src} NAME_WLE)
    add_library(${name} MODULE ${src})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(${name} PRIVATE asio)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        target_compile_options(${name} PRIVATE -fcoroutines -fpermissive)
    endif()
    set_target_properties(${name} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/handlers")
endfunction()

function(target_link_grpc name)
    pkg_check_modules(GRPC REQUIRED grpc++ grpc protobuf)
    target_include_directories(${name} PRIVATE ${GRPC_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/build/proto-gen)
    target_compile_definitions(${name} PRIVATE AGRPC_STANDALONE_ASIO)
    target_link_libraries(${name} PRIVATE ${GRPC_LDFLAGS})
endfunction()

# ── Auth Handler（MySQL）──
add_hotplug_handler(examples/cpp_handlers/registerhandler.cpp)
target_include_directories(registerhandler PRIVATE ${CMAKE_SOURCE_DIR})
find_path(MYSQL_INCLUDE_DIR mysql/mysql.h
    PATHS /usr/include /usr/local/include /usr/include/mysql)
target_include_directories(registerhandler PRIVATE ${MYSQL_INCLUDE_DIR})
find_library(MYSQL_LIB mysqlclient
    PATHS /usr/lib /usr/local/lib /usr/lib/aarch64-linux-gnu)
target_link_libraries(registerhandler PRIVATE ${MYSQL_LIB})

# OpenSSL（RSA 签名）
find_package(OpenSSL REQUIRED)
target_include_directories(registerhandler PRIVATE ${OPENSSL_INCLUDE_DIR})
target_link_libraries(registerhandler PRIVATE OpenSSL::SSL OpenSSL::Crypto)

# ── AI Chat Handler（gRPC + protobuf）──
set(CHAT_PROTO_SRC
    ${CMAKE_SOURCE_DIR}/build/proto-gen/examples/proto/chat.pb.cc
    ${CMAKE_SOURCE_DIR}/build/proto-gen/examples/proto/chat.grpc.pb.cc)
add_hotplug_handler(examples/cpp_handlers/chat_handler.cpp)
target_sources(chat_handler PRIVATE ${CHAT_PROTO_SRC})
target_include_directories(chat_handler PRIVATE ${CMAKE_SOURCE_DIR})
target_link_grpc(chat_handler)
find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp)
target_include_directories(chat_handler PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
# OpenSSL（RSA 验签）
target_include_directories(chat_handler PRIVATE ${OPENSSL_INCLUDE_DIR})
target_link_libraries(chat_handler PRIVATE OpenSSL::SSL OpenSSL::Crypto)

# ── gRPC link flags (via pkg-config for transitive deps) ──
pkg_check_modules(GRPC REQUIRED grpc++ grpc protobuf)

target_include_directories(http_server PRIVATE
    ${GRPC_INCLUDE_DIRS}
    /usr/local/include
    ${CMAKE_CURRENT_SOURCE_DIR}/build/proto-gen
    ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(http_server PRIVATE AGRPC_STANDALONE_ASIO)

target_link_libraries(http_server PRIVATE
    asio Threads::Threads yaml-cpp sqlite3
    OpenSSL::SSL OpenSSL::Crypto dl
    ${LIBURING_LIB}
    ${GRPC_LDFLAGS})

# ── 导出主程序符号，让 dlopen 的 .so 能找到 Router::Add() 等 ──
set_target_properties(http_server PROPERTIES
    LINK_FLAGS "-rdynamic")

# ── RPC 模块测试 ──
add_executable(rpc_test tests/rpc_test.cpp
    rpc/service_discovery.cpp
    rpc/load_balancer.cpp
    rpc/retry_policy.cpp
)
target_include_directories(rpc_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
enable_testing()
add_test(NAME RpcModuleTest COMMAND rpc_test)

# ── Tests are in tests/CMakeLists.txt (standalone) ──
