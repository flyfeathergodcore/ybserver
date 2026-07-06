# CD: Docker 容器化部署

## 概述

为 webcpp 服务器实现完整的持续部署流水线：

- **Dockerfile** — 多阶段 Alpine 构建，~25MB 运行时镜像
- **docker-compose.yml** — 本地一键启动，挂载证书和配置
- **CI/CD** — GitHub Actions 自动构建镜像 → 推送到 GHCR → 自托管 runner 就地部署

## 目标

1. `git push` 后全自动完成编译、测试、构建镜像、部署重启
2. 多阶段构建确保镜像最小（~25MB vs ~600MB 构建环境）
3. 自托管 runner 直接在腾讯云服务器上执行部署，无需 SSH
4. 证书和配置通过 volume 挂载，不打包进镜像

## Dockerfile：多阶段构建

### Stage 1 — Build

```dockerfile
FROM alpine:3.21 AS build

RUN apk add --no-cache build-base cmake asio openssl-dev \
                       yaml-cpp-dev sqlite-dev liburing-dev linux-headers

WORKDIR /src
COPY . .
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j$(nproc) --target http_server
```

### Stage 2 — Run

```dockerfile
FROM alpine:3.21

RUN apk add --no-cache libstdc++ libgcc openssl yaml-cpp sqlite3-libs

COPY --from=build /src/build/http_server /app/
COPY www /app/www

EXPOSE 8081 8443
WORKDIR /app
CMD ["./http_server"]
```

运行时依赖（确认自 `ldd`）：libstdc++、libgcc、openssl (libssl.so.3 + libcrypto.so.3)、yaml-cpp、sqlite3-libs。无 nghttp2、无 liburing 运行时依赖。

## docker-compose.yml

```yaml
services:
  webcpp:
    build: .
    ports:
      - "8081:8081"
      - "8443:8443"
    volumes:
      - ./config.yaml:/app/config.yaml
      - ./ybuestc.art_nginx:/app/ybuestc.art_nginx
```

## GitHub Actions CD Pipeline

在现有 `ci.yml` 基础上增加 `deploy` job：

```yaml
  deploy:
    needs: build-and-test
    runs-on: self-hosted
    permissions:
      packages: write

    steps:
      - uses: actions/checkout@v4

      - name: Login to GHCR
        run: echo "${{ secrets.GITHUB_TOKEN }}" | \
             docker login ghcr.io -u ${{ github.actor }} --password-stdin

      - name: Build and push Docker image
        run: |
          IMAGE="ghcr.io/${{ github.repository }}"
          docker build -t $IMAGE:latest -t $IMAGE:${{ github.sha }} .
          docker push --all-tags $IMAGE

      - name: Deploy with docker compose
        run: |
          docker compose pull
          docker compose up -d
```

### 流程

```
push → CI build + test → docker build → push to GHCR → docker compose pull && up -d
```

### 镜像标签策略

- `:latest` — 最新构建
- `:${{ github.sha }}` — 按 commit 固定，支持回滚

## 自托管 Runner 设置

1. 在本机安装 GitHub Actions runner agent
2. 注册到仓库 `flyfeathergodcore/coroutine-asio-http-server`
3. runner 标签设为 `self-hosted`
4. runner 以 systemd 服务运行，开机自启

## 迁移注意事项

- **端口冲突**：当前本机可能在 8443 运行裸进程版 http_server，Docker 部署前需 `systemctl stop` 或 `kill` 旧进程
- **config.yaml**：Docker 容器路径为 `/app/config.yaml`，需确认 TLS cert/key 路径指向 `/app/ybuestc.art_nginx/...`
- **日志**：当前为 stdout JSON 日志，Docker 自动收集；如需持久化可加 volume

## 非目标

- 不做蓝绿部署 / 滚动更新（单机场景不需要）
- 不做 Kubernetes（只有一台机器）
- 不做多环境（dev/staging/prod），只有生产
