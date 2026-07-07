# rpc/CMakeLists.txt — gRPC infrastructure

# ── gRPC + Protobuf ──
find_package(gRPC REQUIRED)
find_package(Protobuf REQUIRED)

if(gRPC_FOUND AND Protobuf_FOUND)
    message(STATUS "Found gRPC: ${gRPC_VERSION}")
    message(STATUS "Found Protobuf: ${Protobuf_VERSION}")
endif()

# ── agrpc (header-only) ──
include(FetchContent)
FetchContent_Declare(
    agrpc_src
    GIT_REPOSITORY https://github.com/grpc/grpc.git
    GIT_TAG v1.62.0
    SOURCE_SUBDIR src/agrpc
)
FetchContent_MakeAvailable(agrpc_src)

message(STATUS "rpc: gRPC infrastructure configured")
