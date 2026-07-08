# ═══════════════════════════════════════════
# Stage 1 — Build http_server
# ═══════════════════════════════════════════
FROM alpine:3.21 AS build

RUN sed -i 's|dl-cdn.alpinelinux.org|mirrors.tuna.tsinghua.edu.cn|g' /etc/apk/repositories \
 && apk add --no-cache build-base cmake asio-dev openssl-dev \
                       yaml-cpp-dev sqlite-dev liburing-dev linux-headers \
                       grpc-dev protobuf-dev nlohmann-json wget tar

# 安装 agrpc (asio-grpc) 头文件库 - 使用 wget 下载 tarball
RUN mkdir -p /usr/local/include && \
    wget -q https://github.com/Tradias/asio-grpc/archive/refs/tags/v3.2.0.tar.gz -O /tmp/agrpc.tar.gz && \
    tar -xzf /tmp/agrpc.tar.gz -C /tmp && \
    cp -r /tmp/asio-grpc-3.2.0/src/agrpc /usr/local/include/ && \
    rm -rf /tmp/agrpc.tar.gz /tmp/asio-grpc-3.2.0

WORKDIR /src
COPY . .
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j$(nproc) --target http_server

# ═══════════════════════════════════════════
# Stage 2 — Runtime image (~25 MB)
# ═══════════════════════════════════════════
FROM alpine:3.21

RUN sed -i 's|dl-cdn.alpinelinux.org|mirrors.tuna.tsinghua.edu.cn|g' /etc/apk/repositories \
 && apk add --no-cache libstdc++ libgcc openssl yaml-cpp sqlite-libs grpc-cpp protobuf

COPY --from=build /src/build/http_server /app/
COPY www /app/www

EXPOSE 8081 8443
WORKDIR /app
CMD ["./http_server"]
