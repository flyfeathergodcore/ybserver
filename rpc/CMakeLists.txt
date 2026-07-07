# rpc/CMakeLists.txt — gRPC infrastructure

# ── gRPC + Protobuf ──
find_package(gRPC REQUIRED)
find_package(Protobuf REQUIRED)

if(gRPC_FOUND AND Protobuf_FOUND)
    message(STATUS "Found gRPC: ${gRPC_VERSION}")
    message(STATUS "Found Protobuf: ${Protobuf_VERSION}")
endif()

# ── agrpc (header-only) — 在系统路径中查找 ──
find_path(AGRPC_INCLUDE_DIR agrpc/asio_grpc.hpp
    PATHS /usr/local/include /usr/include
    NO_DEFAULT_PATH)
if(NOT AGRPC_INCLUDE_DIR)
    find_path(AGRPC_INCLUDE_DIR agrpc/asio_grpc.hpp)
endif()

if(AGRPC_INCLUDE_DIR)
    message(STATUS "Found agrpc: ${AGRPC_INCLUDE_DIR}")
    add_library(agrpc INTERFACE)
    target_include_directories(agrpc INTERFACE ${AGRPC_INCLUDE_DIR})
else()
    message(FATAL_ERROR "agrpc not found. Install: sudo cp -r src/agrpc /usr/local/include/")
endif()

message(STATUS "rpc: gRPC infrastructure configured")
