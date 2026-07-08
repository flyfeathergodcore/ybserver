#!/bin/bash
set -e

CONTAINER_NAME="web"
PROJECT_PATH="/web"
BUILD_DIR="$PROJECT_PATH/build"

echo "[build] 检查 Docker 容器状态..."
if ! docker ps --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
    echo "[error] 容器 '$CONTAINER_NAME' 未运行"
    echo "请先启动: docker run -d --name $CONTAINER_NAME -v \$(pwd):$PROJECT_PATH alpine:3.21"
    exit 1
fi

echo "[build] 清理旧编译..."
docker exec $CONTAINER_NAME sh -c "rm -rf $BUILD_DIR"

echo "[build] 生成 protobuf 文件..."
docker exec $CONTAINER_NAME sh -c "cd $PROJECT_PATH && mkdir -p $BUILD_DIR/proto-gen/examples/proto && \
    protoc --proto_path=examples/proto \
           --cpp_out=$BUILD_DIR/proto-gen/examples/proto \
           --grpc_out=$BUILD_DIR/proto-gen/examples/proto \
           --plugin=protoc-gen-grpc=/usr/bin/grpc_cpp_plugin \
           examples/proto/*.proto"

echo "[build] CMake 配置..."
docker exec $CONTAINER_NAME sh -c "cd $BUILD_DIR && cmake -DCMAKE_BUILD_TYPE=Release .."

echo "[build] 编译..."
docker exec $CONTAINER_NAME sh -c "cd $BUILD_DIR && cmake --build . -j4"

echo "[build] 验证二进制..."
docker exec $CONTAINER_NAME ls -lh $BUILD_DIR/http_server

echo "[✓] 编译成功: $BUILD_DIR/http_server"
