# examples/cpp_handlers/CMakeLists.txt

set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../proto")

# ── Proto 编译 ──
set(PROTO_FILES
    ${PROTO_DIR}/chat.proto
    ${PROTO_DIR}/rag.proto
    ${PROTO_DIR}/ppt.proto
)
set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto")
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

# 查找 grpc_cpp_plugin
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin)
if(NOT GRPC_CPP_PLUGIN)
    message(FATAL_ERROR "grpc_cpp_plugin not found")
endif()

foreach(PROTO ${PROTO_FILES})
    get_filename_component(PROTO_NAME ${PROTO} NAME_WE)
    set(PROTO_CC "${PROTO_GEN_DIR}/${PROTO_NAME}.pb.cc")
    set(PROTO_H "${PROTO_GEN_DIR}/${PROTO_NAME}.pb.h")
    set(GRPC_CC "${PROTO_GEN_DIR}/${PROTO_NAME}.grpc.pb.cc")
    set(GRPC_H "${PROTO_GEN_DIR}/${PROTO_NAME}.grpc.pb.h")
    add_custom_command(
        OUTPUT ${PROTO_CC} ${PROTO_H} ${GRPC_CC} ${GRPC_H}
        COMMAND protoc
            --proto_path=${PROTO_DIR}
            --cpp_out=${PROTO_GEN_DIR}
            --grpc_out=${PROTO_GEN_DIR}
            --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
            ${PROTO}
        DEPENDS ${PROTO}
        COMMENT "Generating C++ gRPC for ${PROTO_NAME}.proto"
    )
    list(APPEND PROTO_GEN_SRCS ${PROTO_CC} ${GRPC_CC})
endforeach()

# ── 示例 handler 库 ──
add_library(example_handlers STATIC
    chat_handler.cpp
    ${PROTO_GEN_SRCS}
)

target_include_directories(example_handlers PUBLIC
    ${PROTO_GEN_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(example_handlers PUBLIC
    grpc++ grpc protobuf
)

set_target_properties(example_handlers PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
