# ═══════════════════════════════════════════
# Stage 1 — Build http_server
# ═══════════════════════════════════════════
FROM alpine:3.21 AS build

RUN sed -i 's|dl-cdn.alpinelinux.org|mirrors.tuna.tsinghua.edu.cn|g' /etc/apk/repositories \
 && apk add --no-cache build-base cmake asio-dev openssl-dev \
                       yaml-cpp-dev sqlite-dev liburing-dev linux-headers \
                       grpc-dev protobuf-dev git

# 安装 agrpc (asio-grpc) 头文件库
RUN git clone --depth 1 https://github.com/Tradias/asio-grpc.git /tmp/asio-grpc \
 && cp -r /tmp/asio-grpc/src/agrpc /usr/local/include/ \
 && rm -rf /tmp/asio-grpc

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
