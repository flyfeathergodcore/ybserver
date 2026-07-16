# ═══════════════════════════════════════════
# Stage 1 — Build http_server & handlers
# ═══════════════════════════════════════════
FROM alpine:3.21 AS build

# 清空宿主机注入的代理
ENV HTTP_PROXY=""
ENV HTTPS_PROXY=""
ENV http_proxy=""
ENV https_proxy=""
ENV NO_PROXY="*"
ENV no_proxy="*"

RUN sed -i 's|dl-cdn.alpinelinux.org|mirrors.tuna.tsinghua.edu.cn|g' /etc/apk/repositories \
 && apk add --no-cache build-base cmake asio-dev openssl-dev \
                       yaml-cpp-dev sqlite-dev liburing-dev linux-headers \
                       grpc-dev protobuf-dev nlohmann-json mariadb-dev \
                       wget tar

# 安装 agrpc (asio-grpc) 头文件库
RUN mkdir -p /usr/local/include && \
    wget -q https://github.com/Tradias/asio-grpc/archive/refs/tags/v3.2.0.tar.gz -O /tmp/agrpc.tar.gz && \
    tar -xzf /tmp/agrpc.tar.gz -C /tmp && \
    cp -r /tmp/asio-grpc-3.2.0/src/agrpc /usr/local/include/ && \
    rm -rf /tmp/agrpc.tar.gz /tmp/asio-grpc-3.2.0

WORKDIR /src
COPY . .
RUN mkdir -p build/proto-gen/examples/proto && \
    protoc --proto_path=examples/proto \
           --cpp_out=build/proto-gen/examples/proto \
           --grpc_out=build/proto-gen/examples/proto \
           --plugin=protoc-gen-grpc=/usr/bin/grpc_cpp_plugin \
           examples/proto/*.proto && \
    cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j$(nproc) --target http_server \
    && cmake --build build -j$(nproc) --target chat_handler \
    && cmake --build build -j$(nproc) --target registerhandler \
    && cmake --build build -j$(nproc) --target shopping_handler

# ═══════════════════════════════════════════
# Stage 2 — Runtime image
# ═══════════════════════════════════════════
FROM alpine:3.21

# 清空宿主机注入的代理
ENV HTTP_PROXY=""
ENV HTTPS_PROXY=""
ENV http_proxy=""
ENV https_proxy=""
ENV NO_PROXY="*"
ENV no_proxy="*"

RUN sed -i 's|dl-cdn.alpinelinux.org|mirrors.tuna.tsinghua.edu.cn|g' /etc/apk/repositories \
 && apk add --no-cache libstdc++ libgcc openssl yaml-cpp sqlite-libs \
                       grpc-cpp protobuf mariadb-connector-c

COPY --from=build /src/build/http_server /app/
COPY --from=build /src/build/handlers/ /app/build/handlers/
COPY www /app/www

EXPOSE 8081 8443
WORKDIR /app
CMD ["./http_server"]
