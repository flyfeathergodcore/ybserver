# C++20 Coroutine + Asio HTTP(S) Server

基于 C++20 协程和 Asio 的高性能异步 HTTP/1.1 + HTTP/2 服务器，支持 TLS、反向代理、WebSocket、实时指标看板和 SSE 推送。

> 📖 [开发接口文档](docs/api-interface.md) — 面向框架使用者的 Quick Start + API Reference
> 📊 [行业差距分析](docs/superpowers/specs/2026-06-27-industry-gap-analysis.md) — 与 nginx / BFE / Envoy 的维度对比

## 特点

- **C++20 协程** — `co_await` 异步编程，无回调嵌套、无栈分配
- **HTTP/2 over TLS (h2)** — ALPN 协商，nghttp2 帧循环，多流多路复用，HPACK 压缩率 **87-88%**
- **手写 H1Parser** — ~250 行有限状态机取代 llhttp（12K 行），keep-alive 零拷贝
- **TLS 1.3** — OpenSSL 3.0 + asio::ssl::stream，ALPN 自动分发 H1/H2
- **多线程 + SO_REUSEPORT** — 4 个独立 io_context，内核做负载均衡
- **RegionPool 内存架构** — 每线程 256MB mmap 区域，SessionRegion bump allocator，零 per-request 堆分配
- **压缩 Radix Tree 路由** — gin/httprouter 风格，支持 `:param` / `*catchAll` / 前缀/精确匹配
- **中间件链** — 双阶段设计（原始字节拦截 + PreRequest/PostResponse 洋葱模型）
- **反向代理 + 负载均衡** — 上游连接池，轮询选择，被动健康检查，WebSocket 透传
- **WebSocket (RFC 6455)** — H1 Upgrade 握手 + 帧中继，本地 Handler 终结 + 反向代理透传
- **健康检查** — `GET /healthz` 端点，返回服务器及上游状态
- **结构化日志** — JSON 格式，包含请求 ID、延迟、方法、路径、状态码
- **X-Request-Id** — 自动生成 / 透传上游，中间件注入
- **Metrics + Dashboard** — 实时 QPS / 延迟分位数 / 错误率，SSE 流式推送，Chart.js 前端
- **告警系统** — 可配置阈值规则，超过时 SSE `alert` 事件通知
- **文件缓存** — 启动时预加载到内存，零磁盘 I/O
- **协程 SQLite** — 带连接池的异步数据库封装
- **YAML 配置** — `config.yaml` 控制端口、TLS、线程数、CPU 亲和性、代理路由等
- **CPU 亲和性** — 可选绑定 worker 线程到专用核心（默认开启），减少缓存抖动
- **优雅关闭** — `SIGINT`/`SIGTERM` → 停 acceptor → 排空 session → 退出
- **配置校验** — `./http_server -t` dry-run 模式

## 快速开始

```bash
# 安装依赖
sudo apt install libasio-dev libyaml-cpp-dev libsqlite3-dev libssl-dev libnghttp2-dev

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
  port: 8081           # HTTP（明文，可选）
  tls_port: 8443       # HTTPS
  threads: 4
  doc_root: "./www"
  cpu_affinity: true   # 绑定 worker 到专用核心
  tls:
    cert: "./cert.pem"
    key:  "./key.pem"

# 反向代理路由
proxy:
  - prefix: "/api/"
    upstreams:
      - "127.0.0.1:3000"
      - "127.0.0.1:3001"    # 多上游自动轮询负载均衡
```

### 配置校验

```bash
./http_server -t              # 校验默认配置
./http_server -t /path/to/config.yaml   # 校验指定配置
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

## 健康检查

```
GET /healthz
```

返回 JSON：

```json
{
  "status": "ok",
  "uptime": 12345,
  "threads": 4,
  "active_connections": 12,
  "upstreams": [
    {"host": "127.0.0.1:3000", "state": "alive"},
    {"host": "127.0.0.1:3001", "state": "dead"}
  ]
}
```

## 基准测试

4 核 AMD EPYC 7K62, 4 线程/worker, OpenSSL 3.0 TLS 1.3, 静态文件（6 bytes body）。

**响应头：** 完整 HTTP/1.1 规范头（~244 bytes，与 nginx 一致），含 Date / Last-Modified / ETag / Accept-Ranges / Connection / CORS。

### HTTP/1.1 HTTPS（对比主流服务器）

每项均为同一台机器、干净环境、`wrk -t4 -cN -d30s`。
webcpp 数据更新于 2026-07-06，nginx/Caddy 为同期对比值。

| 连接数 | **webcpp** | **nginx 1.24** | **Caddy 2.11** |
|:-----:|:----------:|:--------------:|:--------------:|
| 20 | **59,704** | 53,169 | 26,006 |
| 100 | **70,212** | 59,735 | 27,080 |
| 200 | **73,469** | 60,642 | 26,392 |
| 500 | **70,632** | 60,726 | 25,599 |
| 1000 | **64,195** | 58,856 | — |
| 1500 | **60,759** | — | — |
| 2000 | **58,626** | — | — |

**关键结论：**
- **全连接段领先**：webcpp 持续领先 nginx **10-25%**，领先 Caddy 约 **2.6 倍**
- 架构统一（H1/H2 共享 Region 结构化头存储）后零额外开销：`sizeof(Response)` 仅 112 字节

### HTTP/2 HTTPS（对比 nginx）

`h2load -cN -nN×1000 -m10`，多流复用压力测试。webcpp 数据更新于 2026-07-06，nginx 为同期对比值。两台服务器均为 4 worker，TLS 1.3。

| 连接数 | **webcpp** | **nginx 1.24** | webcpp 优势 |
|:-----:|:----------:|:--------------:|:----------:|
| 100 | **132,012** | 41,948 | **+215%** |
| 200 | **137,416** | 72,698 | **+89%** |
| 500 | **131,558** | 77,696 | **+69%** |
| 1000 | **128,340** | 78,975 | **+63%** |
| 1500 | **128,985** | 78,572 | **+64%** |
| 2000 | **128,812** | 77,893 | **+65%** |
| 2500 | **134,402** | 76,783 | **+75%** |
| 3000 | **135,687** | 77,373 | **+75%** |

**关键结论：**

- **全线碾压 nginx 63-215%**：io_uring + 协程模型在所有并发段全面领先
- **c200 峰值 137K req/s**：io_uring 批量提交 + SessionRegion 零分配路径在高并发下优势完全释放
- **曲线持续上升**：从 c100（132K）到 c3000（136K）几乎无衰减，nignx 从 c500 起 plateau 在 ~78K
- **HPACK 压缩率**：webcpp 达 87-88%（nginx 为 38%），头体积仅 nginx 的 1/5，带宽效率高 4 倍

**协程 vs 多进程调度：** webcpp 的 H2 吞吐量在 100-3000 并发区间稳定在 128K-137K，几乎不受连接数影响。nginx 多进程模型在 c200-c500 达到峰值后即停滞。协程 I/O 的零调度开销 + io_uring 的零拷贝 DMA 共同消除了高并发下的系统调用瓶颈。

### 优化亮点

| 优化 | 效果 |
|------|------|
| **io_uring 后端** | ASIO io_uring backend，SQE 批量提交 + CQE 零中断通知，H2 QPS 翻倍至 137K |
| **Region 统一架构** | H1/H2 共享结构化头存储，`sizeof(Response)` 仅 112 字节，无回调 hack |
| **紧凑 Response 类** | 堆分配 HeaderStorage（仅 H2 使用），H1 零额外开销，QPS 最高 +4% |
| **零分配 Post 中间件** | HandlePostSync 消除零挂起协程帧，50K QPS 省 ~100K 帧 alloc/s |
| **accept/handshake 分离** | 2000 连接从落后 33% 到持平（+29%） |
| **CPU 亲和性** | 5000 连接提升 +17%（可通过 `config.yaml` 开关） |
| **完整响应头** | 与 nginx 头大小一致，QPS 仅下降 2-4%，换来 HTTP 规范合规 + 浏览器 304 缓存 |
| **顺序 H2 流处理** | 替代 co_spawn 并发，消除帧发送死锁 + 头回调竞争 |
| **HPACK 高压缩率** | 87-88% 头压缩（nginx 为 38%），单连接极限流场景领先 nginx 89% |
| **ASIO 回收分配器** | `ASIO_RECYCLING_ALLOCATOR_CACHE_SIZE=16` 减少协程帧堆分配波动 |
| **协程高并发扩展** | 100→3000 连接 H2 吞吐量稳定 128K-137K，无衰减 |

### 延迟对比

| 连接数 | **webcpp** | **nginx** |
|:-----:|:----------:|:---------:|
| 20 | **0.26ms** | 0.46ms |
| 100 | **1.19ms** | 1.74ms |
| 200 | **2.42ms** | 3.28ms |
| 500 | **6.08ms** | 8.23ms |
| 1000 | **13.66ms** | 18.16ms |

webcpp 延迟整体更低且无错误；nginx 在高连接数下延迟抖动更剧烈。

## 项目结构

```
├── main.cpp              入口点（配置校验、Metrics、Middleware 组装、启动）
├── CMakeLists.txt        构建配置（C++20, Asio, OpenSSL, SQLite, nghttp2）
├── config.yaml           服务器配置
│
├── ssl/
│   └── tls_context.cpp   SSL_CTX 配置 + ALPN 协商
│
├── net/                  网络 I/O + 会话
│   ├── server.cpp        TCP 监听 + accept 循环
│   ├── session_base.hpp  Session 基类（协议无关）
│   ├── multi_server.cpp  多线程 SO_REUSEPORT 服务器 + ALPN 分发
│   ├── response.cpp      响应构建（inline / file / SSE / WebSocket 101）
│   ├── region_pool.cpp   256MB 每线程内存池
│   ├── session_region.cpp bump allocator
│   ├── ws_frame.cpp      WebSocket 帧 I/O + Sec-WebSocket-Accept 计算
│   ├── ws_connection.hpp WebSocket 连接抽象（类型擦除 + 模板实现）
│   └── ws_relay.hpp      双向帧转发（用于反向代理透传）
│
├── http/                 HTTP 协议
│   ├── context.hpp       Context 基类（parser 接口）
│   ├── protocol.cpp      辅助函数
│   ├── http1.1/
│   │   ├── parser.cpp    手写 HTTP/1.1 状态机（~250 行）
│   │   ├── session.cpp   H1 会话（Upgrade: websocket 检测 + 101 握手）
│   │   └── session_pool.cpp  对象池
│   └── http2/
│       ├── session.cpp   H2 会话（nghttp2 帧循环 + 顺序流处理）
│       └── stream_context.cpp  请求上下文适配
│
├── handler/              路由 + 业务逻辑
│   ├── router.cpp        压缩 Radix Tree 路由（:param / *catchAll / 前缀）
│   ├── request_handler.cpp   Handler 基类 + StaticFileHandler
│   ├── reverse_proxy.cpp     反向代理（协程转发 + WebSocket 透传）
│   ├── upstream_pool.cpp     上游池（轮询 + 被动健康检查 + 冷却恢复）
│   ├── upstream_conn_pool.cpp 上游 TCP 连接池（每 worker 线程本地）
│   ├── health.cpp            健康检查端点 (/healthz)
│   ├── metrics.cpp           Metrics 采集 + SSE delta + 告警规则
│   └── proxy_handler.cpp     单上游代理（向后兼容）
│
├── middleware/           中间件链
│   ├── middleware.cpp    洋葱模型执行引擎
│   ├── cors              跨域 (Access-Control-Allow-Origin: *)
│   ├── metrics           每请求耗时记录
│   ├── logging           结构化 JSON 日志（method, path, status, latency, req_id）
│   └── request_id        X-Request-Id 自动生成 + 透传
│
├── rpc/                  数据库层
│   ├── database.cpp      协程版 SQLite 封装（线程池 offload）
│   └── connection_pool.cpp  等待队列连接池
│
├── cache/                缓存
│   └── file_cache.cpp    启动时预加载文件到内存
│
├── config/               配置
│   ├── config.cpp        YAML 配置加载 + 校验
│   └── config.hpp        配置结构体（server / proxy / redirect 三段）
│
├── docs/                 文档
│   ├── api-interface.md  开发接口文档（Quick Start + API Reference）
│   └── superpowers/
│       └── specs/        设计规格文档
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

  WebSocket 流（H1）:
  H1Session → Upgrade 检测 → ComputeWsAccept → 101 → HandleWebSocket(conn)
                                               │
                    ┌──────────────────────────┤
                    │                          │
              ┌─────▼─────┐            ┌───────▼──────┐
              │ Local      │            │ ReverseProxy │
              │ WsEcho     │            │ → 上游连接    │
              │ 等业务     │            │ → 双向 relay   │
              └───────────┘            └──────────────┘
```

## 路由

基于压缩 Radix Tree（gin/httprouter 风格）的 HTTP 路由：

```cpp
router.Add("/hello", handler);            // 精确匹配
router.Get("/users/:id", handler);        // 路径参数，匹配 /users/42
router.Post("/files/*path", handler);     // catchAll，匹配 /files/a/b/c
router.Add("/api/", handler);             // 前缀匹配
```

匹配优先级：`精确静态 > :param > *catchAll > 前缀匹配`

## Handler 编写

```cpp
class HelloHandler : public RequestHandler {
    Response Handle(const Context& ctx) override {
        Response resp(200, *ctx.Pool());
        resp.Header("Content-Type", "text/plain; charset=utf-8");
        resp.EndHeaders();
        resp.Body("Hello, World!");
        return resp;
    }
};
```

详细示例见 [开发接口文档 Quick Start](docs/api-interface.md#3-编写-handler同步)。

## WebSocket

H1 原生支持 Upgrade → 101 握手 → 帧循环。两种接入方式：

**本地 Handler 终结：**

```cpp
class WsEchoHandler : public RequestHandler {
    asio::awaitable<void> HandleWebSocket(const Context& ctx,
                                           WsConnectionBase& conn) override
    {
        while (conn.IsOpen()) {
            auto frame = co_await conn.Read();
            if (!conn.IsOpen()) break;
            co_await conn.Send(frame.opcode, std::move(frame.payload));
        }
    }
};

router.Add("/ws/echo", std::make_unique<WsEchoHandler>());
```

**反向代理透传：** `ReverseProxy` 自动将 WebSocket 连接转发到上游并双向 relay，无需额外代码。

## 许可

MIT
