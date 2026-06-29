# C++20 Coroutine + Asio HTTP(S) Server

基于 C++20 协程和 Asio 的高性能异步 HTTP/1.1 + HTTP/2 服务器，支持 TLS、实时指标看板和 SSE 推送。

## 特点

- **C++20 协程** — `co_await` 异步编程，无回调嵌套、无栈分配
- **HTTP/2 over TLS (h2)** — ALPN 协商，nghttp2 帧循环，多流多路复用
- **手写 H1Parser** — ~250 行有限状态机取代 llhttp（12K 行），keep-alive 零拷贝
- **TLS 1.3** — OpenSSL 3.0 + asio::ssl::stream，可选加密
- **多线程 + SO_REUSEPORT** — 4 个独立 io_context，各持一个 listener socket，内核做负载均衡
- **RegionPool** — 每线程 256MB mmap 区域，SessionRegion  bump allocator，近乎零开销
- **Metrics + Dashboard** — 实时 QPS / 延迟分位数 / 错误率，SSE 流式推送，Chart.js 前端
- **告警系统** — 可配置阈值规则，超过时 SSE `alert` 事件通知
- **文件缓存** — 启动时预加载到内存，零磁盘 I/O
- **协程 SQLite** — 带连接池的异步数据库封装
- **YAML 配置** — `config.yaml` 控制端口、TLS、线程数、CPU 亲和性等
- **CPU 亲和性** — 可选绑定 worker 线程到专用核心（默认开启），减少缓存抖动

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
  port: 8081           # HTTP（无 TLS，可选）
  tls_port: 8443       # HTTPS
  threads: 4
  doc_root: "./www"
  cpu_affinity: true   # 绑定 worker 到专用核心
  tls:
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

4 核 AMD EPYC 7K62, 4 线程/worker, OpenSSL 3.0 TLS 1.3, 静态文件（6 bytes body）。

**响应头：** 完整 HTTP/1.1 规范头（~244 bytes，与 nginx 一致），含 Date / Last-Modified / ETag / Accept-Ranges / Connection / CORS。

### HTTP/1.1 HTTPS（对比主流服务器）

每项均为同一台机器、干净环境、`wrk -t4 -cN -d30s`。

| 连接数 | **webcpp** | **nginx 1.24** | **Caddy 2.11** |
|:-----:|:----------:|:--------------:|:--------------:|
| 20 | **55,380** | 53,169 | 26,006 |
| 100 | **70,271** | 59,735 | 27,080 |
| 200 | **71,617** | 60,642 | 26,392 |
| 500 | **73,477** | 60,726 | 25,599 |
| 1000 | **65,742** | 58,856 | — |

**关键结论：**
- **全连接段领先**：webcpp 持续领先 nginx **10-25%**，领先 Caddy 约 **2.6 倍**
- 架构统一（H1/H2 共享 Region 结构化头存储）后零额外开销：`sizeof(Response)` 仅 112 字节

### HTTP/2 HTTPS（对比 nginx）

`h2load -cN -nN×1000 -m10`，多流复用压力测试。所有测试在相同机器、相同条件下运行，两台服务器均为 4 worker，TLS 1.3。

| 连接数 | **webcpp** | **nginx 1.24** | 对比 | webcpp 趋势 |
|:-----:|:----------:|:--------------:|:----:|:----------:|
| 100 | **66,183** | 41,948 | **webcpp +58%** | ↗ |
| 200 | **68,431** | 72,698 | nginx +6% | ↗ |
| 500 | **69,918** | 77,696 | nginx +10% | ↗ |
| 1000 | **88,020** | 78,975 | **webcpp +11%** | ↗ |
| 1500 | **91,094** | 78,572 | **webcpp +16%** | ↗ |
| 2000 | **93,151** | 77,893 | **webcpp +20%** | ↗ 峰值 |
| 2500 | **81,026** | 76,783 | webcpp +5% | ↘ |
| 3000 | **79,700** | 77,373 | webcpp +3% | ↘ |

**关键结论：**

- **c100 碾压 +58%**：紧凑响应 + 协程模型在低并发段优势明显
- **c200~c500**：nginx 多进程模型在此区间领先 6-10%
- **c1000~c2000 宽幅领先 11-20%**：协程 I/O 并发优势随连接数释放，达到峰值 **93,151 req/s**
- **c2500~c3000 持平 nginx**：webcpp 仍有 3-5% 优势，nginx 曲线从 c500 起 plateau 在 ~78K 不再增长
- **所有场景 100% 成功率**，零错误、零超时
- **HPACK 压缩率**：webcpp 达 87-88%（nginx 为 38%），头体积仅 nginx 的 1/5，带宽效率高 4 倍

**协程 vs 多进程调度：** 连接数从 100 增长到 2000，webcpp 吞吐量持续上升（66K → 93K，+41%），nginx 从 c500 起基本停滞（78K plateau）。协程在大量 I/O 并发下的零调度开销优势在高连接段充分释放。nginx 多进程模型在 c200-c500 有优势，但扩展上限明显。

### 优化亮点

| 优化 | 效果 |
|------|------|
| **Region 统一架构** | H1/H2 共享结构化头存储，`sizeof(Response)` 仅 112 字节，无回调 hack |
| **紧凑 Response 类** | 堆分配 HeaderStorage（仅 H2 使用），H1 零额外开销，QPS 最高 +4% |
| **accept/handshake 分离** | 2000 连接从落后 33% 到持平（+29%） |
| **CPU 亲和性** | 5000 连接提升 +17%（可通过 `config.yaml` 开关） |
| **完整响应头** | 与 nginx 头大小一致，QPS 仅下降 2-4%，换来 HTTP 规范合规 + 浏览器 304 缓存 |
| **顺序 H2 流处理** | 替代 co_spawn 并发，消除帧发送死锁 + 头回调竞争 |
| **HPACK 高压缩率** | 87-88% 头压缩（nginx 为 38%），单连接极限流场景领先 nginx 89% |
| **协程高并发扩展** | c100→c2000 吞吐量 +41%，远超 nginx 的 plateau 上限 |

### 延迟对比

| 连接数 | **webcpp** | **nginx** |
|:-----:|:----------:|:---------:|
| 20 | **0.45ms** | 0.46ms |
| 100 | **1.47ms** | 1.74ms |
| 200 | **2.81ms** | 3.28ms |
| 500 | **6.30ms** | 8.23ms |
| 1000 | **14.39ms** | 18.16ms |

webcpp 延迟整体更低且无错误；nginx 在高连接数下延迟抖动更剧烈。

## 项目结构

```
├── main.cpp              入口点
├── CMakeLists.txt        构建配置（C++20, Asio, OpenSSL, SQLite）
├── config.yaml           服务器配置
│
├── ssl/
│   └── tls_context.cpp   SSL_CTX 配置 + ALPN 协商
│
├── net/                  网络 I/O + 会话
│   ├── server.cpp        TCP 监听 + accept 循环
│   ├── session_base.cpp  Session 基类（协议无关）
│   ├── multi_server.cpp  多线程 SO_REUSEPORT 服务器 + ALPN 分发
│   ├── metrics.cpp       线程本地计数器 + 环形缓冲区 + SSE delta
│   ├── response.cpp      响应构建（inline / file / SSE）
│   ├── region_pool.cpp   256MB 每线程内存池
│   └── session_region.cpp bump allocator
│
├── http/                 HTTP 协议
│   ├── context.hpp       Context 基类（parser 接口）
│   ├── protocol.cpp      辅助函数
│   ├── http1.1/
│   │   ├── parser.cpp    手写 HTTP/1.1 状态机（~250 行）
│   │   ├── session.cpp   HTTP/1.1 会话
│   │   └── session_pool.cpp  对象池
│   └── http2/
│       ├── session.cpp   HTTP/2 会话（nghttp2 帧循环）
│       └── stream_context.cpp  请求上下文适配
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
