# ═══════════════════════════════════════════
# Stage 1 — Build http_server
# ═══════════════════════════════════════════
FROM alpine:3.21 AS build

RUN sed -i 's|dl-cdn.alpinelinux.org|mirrors.tuna.tsinghua.edu.cn|g' /etc/apk/repositories \
 && apk add --no-cache build-base cmake asio-dev openssl-dev \
                       yaml-cpp-dev sqlite-dev liburing-dev linux-headers

WORKDIR /src
COPY . .
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j$(nproc) --target http_server

# ═══════════════════════════════════════════
# Stage 2 — Runtime image (~25 MB)
# ═══════════════════════════════════════════
FROM alpine:3.21

RUN sed -i 's|dl-cdn.alpinelinux.org|mirrors.tuna.tsinghua.edu.cn|g' /etc/apk/repositories \
 && apk add --no-cache libstdc++ libgcc openssl yaml-cpp sqlite-libs

COPY --from=build /src/build/http_server /app/
COPY www /app/www

EXPOSE 8081 8443
WORKDIR /app
CMD ["./http_server"]
