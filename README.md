# C++20 Coroutine + Asio HTTP(S) Server

基于 C++20 协程和 Asio 的高性能异步 HTTP/1.1 服务器，支持 TLS、实时指标看板和 SSE 推送。

## 特点

- **C++20 协程** — `co_await` 异步编程，无回调嵌套、无栈分配
- **手写 H1Parser** — ~250 行有限状态机取代 llhttp（12K 行），keep-alive 零拷贝
- **TLS 1.3** — OpenSSL 3.0 + asio::ssl::stream，可选加密
- **多线程 + SO_REUSEPORT** — 4 个独立 io_context，各持一个 listener socket，内核做负载均衡
- **RegionPool** — 每线程 256MB mmap 区域，SessionRegion  bump allocator，近乎零开销
- **Metrics + Dashboard** — 实时 QPS / 延迟分位数 / 错误率，SSE 流式推送，Chart.js 前端
- **告警系统** — 可配置阈值规则，超过时 SSE `alert` 事件通知
- **文件缓存** — 启动时预加载到内存，零磁盘 I/O
- **协程 SQLite** — 带连接池的异步数据库封装
- **YAML 配置** — `config.yaml` 控制端口、TLS、线程数等

## 快速开始

```bash
# 安装依赖
sudo apt install libasio-dev libyaml-cpp-dev libsqlite3-dev libssl-dev

# 构建
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make http_server -j$(nproc)

# 运行（默认 HTTPS 8443 端口，需配置 TLS 证书）
./http_server
```

## 配置

编辑 `config.yaml`：

```yaml
server:
  host: "0.0.0.0"
  port: 8443
  threads: 4
  doc_root: "./www"
  tls:
    enabled: true
    cert: "./cert.pem"
    key:  "./key.pem"
```

## 实时监控

启动服务器后打开浏览器：

```
https://127.0.0.1:8443/dashboard/
```

- **三个图表**：QPS & 错误数 / 延迟分位数 / 活跃连接数
- **顶部摘要**：当前吞吐量、p50/p90/p99、连接数、运行时间
- **告警面板**：高错误率、高延迟、QPS 骤降时自动展示
- **SSE 推送**：1 秒间隔推送增量，无需轮询

## 基准测试

4 线程、100 并发连接、TLS 1.3：

```
$ wrk -t4 -c100 -d30s https://127.0.0.1:8443/
Requests/sec:  70,000
Latency avg:   1.52ms
Transfer:      6.74 MB/s
```

仪表盘延迟分位数（同负载）：p50=269µs  p90=721µs  p99=1599µs

## 项目结构

```
├── main.cpp              入口点
├── CMakeLists.txt        构建配置（C++20, Asio, OpenSSL, SQLite）
├── config.yaml           服务器配置
│
├── net/                  网络 I/O + 会话
│   ├── server.cpp        TCP 监听 + accept 循环
│   ├── session.cpp       HTTP 会话（读/写，SSE 流推送）
│   ├── multi_server.cpp  多线程 SO_REUSEPORT 服务器
│   ├── metrics.cpp       线程本地计数器 + 环形缓冲区 + SSE delta
│   ├── response.cpp      响应构建（inline / file / SSE）
│   ├── region_pool.cpp   256MB 每线程内存池
│   ├── session_region.cpp bump allocator
│   ├── tls_context.cpp   SSL_CTX 配置
│   └── connection_pool.hpp/cpp  数据库连接池共享
│
├── http/                 HTTP 协议
│   ├── h1_parser.cpp     手写 HTTP/1.1 状态机（~250 行）
│   └── protocol.cpp      辅助函数
│
├── middleware/           中间件链
│   ├── middleware.cpp    CORS / Logging / Metrics / StaticFile
│   └── cors / logging / metrics 内建处理器
│
├── handler/              业务逻辑
│   ├── request_handler.cpp  请求路由 + 静态文件服务
│
├── rpc/                  数据库层
│   ├── database.cpp      协程版 SQLite 封装
│   └── connection_pool.cpp  连接池
│
├── cache/                缓存
│   └── file_cache.cpp    启动时预加载文件到内存
│
├── config/               配置
│   └── config.cpp        YAML 配置加载
│
├── www/                  静态文件
│   └── dashboard/        监控仪表盘（HTML/JS/CSS）
│
└── tools/
    └── hud.sh            终端 HUD（curl + jq）
```

## 架构要点

```
                           ┌─────────────┐
                           │  main.cpp    │
                           └──────┬──────┘
                                  │
                    ┌─────────────┼─────────────┐
                    │             │             │
              ┌─────▼─────┐ ┌────▼────┐ ┌─────▼─────┐
              │ Worker 0  │ │ Worker1 │ │ Worker 2  │  ← 各持独立 io_context
              │ epoll +   │ │ epoll   │ │ epoll     │
              │ SO_REUSEP│ │         │ │           │
              └─────┬─────┘ └────────┘ └───────────┘
                    │
        ┌───────────┼───────────┐
        │           │           │
  ┌─────▼────┐ ┌───▼────┐  ┌──▼──────┐
  │Session   │ │Session │  │Session  │  ← 每个连接一个协程
  │H1Parser  │ │  ...   │  │SSE Loop │
  │RegionPool│ │        │  │metrics  │
  └──────────┘ └────────┘  └─────────┘
```

## 学习路线

查看 [cpp-coroutine-network-learning-path.md](cpp-coroutine-network-learning-path.md) 了解完整学习路线。

详细技术总结见 [learn-summary.md](learn-summary.md)。

## 许可

MIT
