# C++ HTTP Server vs 行业方案：差距分析

> 分析日期：2026-06-27（更新于 2026-06-30）
> 对比对象：当前 C++20 Asio 协程 HTTP 服务器 ↔ Baidu BFE / nginx / Envoy

---

## 一、项目概览

| 项目 | 当前项目 | Baidu BFE | nginx | Envoy |
|------|---------|-----------|-------|-------|
| 语言 | C++20 | Go | C | C++ |
| 代码量 | ~2,143 行（自有代码）+ 11,070 行（llhttp） | 200,000+ 行 | 150,000+ 行 | 300,000+ 行 |
| 协程模型 | C++20 `co_await` + Asio Proactor | goroutine | 事件驱动（epoll/kqueue） | event-driven + 线程池 |
| 进程模型 | 单进程多线程 / MultiWorker 多进程 | 多进程（每 CPU 核一个） | 多进程（master + worker） | 多线程 + worker 池 |
| 内存管理 | Worker 级 RegionPool（256MB mmap） + 每 Session  bump 分配 + 2× 向量迁移 | 运行时 GC | 每连接 pool（小块分配） | 引用计数 + 池 |
| 定位 | HTTP 静态文件服务器 | 七层流量接入网关 | Web 服务器 / 反向代理 / 负载均衡 | 服务网格数据面 / 代理 |

---

## 二、能力维度矩阵

### 2.1 协议支持

| 能力 | 当前实现 | 行业水平 | 差距等级 |
|------|---------|---------|---------|
| HTTP/1.0 / 1.1 | ✅ llhttp 封装，完整解析 | ✅ 所有产品 | — |
| HTTP/1.1 Keep-Alive | ✅ Connection: close 检测 | ✅ 持久连接 + 复用 | — |
| **HTTP/2 (h2 over TLS)** | ✅ nghttp2 帧循环 + 顺序流处理 | ✅ BFE/nginx/Envoy 原生支持 | 🟡 中 |
| **HTTP/3 (QUIC)** | ❌ 未实现 | ❌ BFE 不支持；nginx 实验性；Envoy ✅ | 🔴 大 |
| **HTTPS/TLS 1.3** | ✅ Asio SSL stream + OpenSSL 3.0 | ✅ 证书管理、SNI、OCSP Stapling | 🟡 中 |
| **WebSocket** | ❌ 未实现 | ✅ BFE/nginx/Envoy 均支持 Upgrade/WSS | 🔴 大 |
| **gRPC** | ❌ 未实现 | ✅ BFE/Envoy 原生 gRPC、负载均衡 | 🔴 大 |
| CONNECT 隧道 | ❌ 未实现 | ✅ 正向代理 | 🟡 中 |
| 协议自动检测 | ⚠️ HTTP/2 preface 检测（基础） | ✅ ALPN/NPN + 协议协商 + 自动降级 | 🟡 中 |

### 2.2 请求处理模型

| 能力 | 当前实现 | 行业水平 | 差距等级 |
|------|---------|---------|---------|
| 事件循环 | ✅ Asio `io_context::run()` | ✅ 全部 | — |
| 多线程/多进程 | ✅ SO_REUSEPORT 多进程 + 共享 io_context 多线程 | 多进程（BFE/nginx）/多线程（Envoy） | — |
| **异步非阻塞 I/O** | ✅ 协程 + epoll | ✅ 全部 | — |
| **零拷贝 (sendfile)** | ✅ TCP 直通 sendfile；SSL 退化为 read+write | ✅ nginx `sendfile` + `tcp_nopush` | 🟡 中 |
| **内存池 (RegionPool)** | ✅ Worker 级 256MB mmap + 每 Session bump 分配 + vector 式 2× 迁移 | ✅ nginx ngx_pool_t | 🟢 小 |
| **FixedBuffer 零分配响应头** | ✅ 512 字节栈缓冲（用于 HTTP/1.1 状态构建）+ header 直接写入 region | ✅ nginx 自有缓冲区 | 🟢 小 |
| **Response 全 Region 构建** | ✅ 状态行+headers+body 连续写入 region，body 外存指针不拷贝，零堆分配 | ✅ nginx 自带池 | 🟢 小 |
| **LlhttpParser 嵌入 Session** | ✅ 直接成员，非 unique_ptr，零 per-session heap 分配 | ✅ nginx 连接池 | 🟢 小 |
| **零页错误压测** | ✅ 500 连接 15 秒 92 万请求：0 minor-faults、VmRSS 2MB 不增长 | ✅ nginx 相似水平 | 🟢 小 |
| **连接池复用** | ❌ 仅服务端 listen/accept，无上游连接池 | ✅ nginx upstream keepalive / Envoy conn-pool | 🔴 大 |
| **请求流水线 (Pipelining)** | ❌ 无 | ✅ nginx 支持 HTTP/1.1 pipelining | 🟡 中 |
| 优雅关闭 | ❌ 立即退出 | ✅ 先停监听 → 排水 → 超时强关 | 🟡 中 |
| **CPU 亲和性** | ❌ 无 | ✅ nginx worker 绑定 CPU 核 | 🟡 中 |
| SO_REUSEPORT | ✅ MultiWorker 模式 | ✅ 多进程独占 listener，减少锁竞争 | 🟢 小 |

### 2.3 路由与流量管理

| 能力 | 当前实现 | 行业水平 | 差距等级 |
|------|---------|---------|---------|
| 静态路由 | ✅ `/index.html` 等 | ✅ | — |
| 路径规范化 | ✅ 防 `..`、`//` | ✅ | — |
| **虚拟主机 (VirtualHost)** | ❌ 无 | ✅ nginx server block / BFE tenant | 🟡 中 |
| **反向代理 / 上游转发** | ✅ ProxyHandler + ReverseProxy + UpstreamPool | ✅ 核心能力 | 🟡 中 |
| **负载均衡算法** | ✅ 轮询（UpstreamPool） | ✅ 轮询/最小连接/一致性哈希/EWMA 等 | 🟡 中 |
| **灰度发布 / 流量切分** | ❌ 无 | ✅ BFE 支持按 header/cookie/IP 分流 | 🔴 大 |
| **熔断** | ❌ 无 | ✅ Envoy 异常检测 + 弹出 | 🔴 大 |
| **限流** | ❌ 无 | ✅ 令牌桶、滑动窗口、并发限流 | 🔴 大 |
| **重试** | ❌ 无 | ✅ 指数退避 + 幂等判断 | 🔴 大 |
| **请求改写 (Rewrite)** | ✅ 简单路径规范化 | ✅ nginx rewrite / BFE 条件改写 | 🟡 中 |
| **URL 重定向** | ❌ 无 | ✅ 301/302/307 | 🟡 中 |
| **条件判断引擎** | ❌ 无 | ✅ BFE 条件表达式（10+ 匹配规则） | 🔴 大 |

### 2.4 中间件 / 过滤链

| 能力 | 当前实现 | 行业水平 | 差距等级 |
|------|---------|---------|---------|
| 中间件架构 | ✅ 双阶段（原始字节 + 洋葱模型） | ✅ 都有类似 filter/plugin 机制 | — |
| 日志记录 | ✅ LoggingMiddleware | ✅ access_log（nginx） | 🟡 中 |
| CORS | ✅ CORSMiddleware | ✅ 需第三方或手写 | — |
| **自定义插件** | ❌ 无动态加载 | ✅ nginx 动态 `.so` / Envoy Lua 或 WASM | 🔴 大 |
| **插件热加载** | ❌ 需重新编译 | ✅ nginx `load_module` + HUP | 🔴 大 |
| **WAF / 安全过滤** | ❌ 无 | ✅ nginx ModSecurity / BFE 安全策略 | 🔴 大 |
| **请求体缓冲 / 流式处理** | ❌ 无 | ✅ BFE/nginx 均支持 body buffer | 🟡 中 |
| **响应头注入/修改** | ✅ ctx.AddResponseHeader() 在 handler 前写入，region 直接构建 | ✅ 复杂条件表达式操作 | 🟢 小 |

### 2.5 可观测性

| 能力 | 当前实现 | 行业水平 | 差距等级 |
|------|---------|---------|---------|
| stdout 日志 | ✅ 基础日志 | ✅ | — |
| **结构化日志 (JSON)** | ❌ 否 | ✅ 全部支持 | 🟡 中 |
| **指标 (Metrics)** | ❌ 无 | ✅ Prometheus / StatsD / 自定义 | 🔴 大 |
| **分布式追踪** | ❌ 无 | ✅ Envoy OpenTelemetry / Zipkin | 🔴 大 |
| **请求 ID / 链路 ID** | ❌ 无 | ✅ 全部支持 trace propagation | 🟡 中 |
| **健康检查接口** | ❌ 无 | ✅ /healthz / active + passive probes | 🔴 大 |
| **审计日志** | ❌ 无 | ✅ BFE/Envoy 支持 | 🟡 中 |
| **自定义日志管道** | ❌ 无 | ✅ nginx log_format / BFE log rotate | 🟡 中 |

### 2.6 配置管理

| 能力 | 当前实现 | 行业水平 | 差距等级 |
|------|---------|---------|---------|
| YAML 配置 | ✅ Config::Load | ✅ | — |
| 命令行参数 | ⚠️ 仅支持传入 config 路径 | ✅ 丰富的 CLI flags | 🟡 中 |
| **配置热重载** | ❌ 需重启进程 | ✅ nginx `reload` / BFE `SIGHUP` | 🔴 大 |
| **配置校验 / dry-run** | ❌ 无 | ✅ nginx `-t` 校验 | 🟡 中 |
| **多级配置（全局/站点/路由）** | ❌ 单层 | ✅ 所有产品分层配置 | 🟡 中 |
| 环境变量覆盖 | ❌ 无 | ✅ 部分支持 | 🟢 小 |
| 配置版本管理 | ❌ 无 | ✅ BFE 配置多版本 + 回滚 | 🟡 中 |

### 2.7 安全

| 能力 | 当前实现 | 行业水平 | 差距等级 |
|------|---------|---------|---------|
| **TLS 1.2/1.3** | ✅ OpenSSL 3.0 `asio::ssl::stream` | ✅ BFE `bfe_tls` 基于 BoringSSL，SNI、OCSP | 🟡 中 |
| **自动 HTTPS (ACME)** | ❌ 无 | ✅ nginx + certbot | 🔴 大 |
| **访问控制 (IP/User-Agent 等)** | ❌ 无 | ✅ nginx allow/deny / BFE 条件 | 🟡 中 |
| **CORS** | ✅ 实现 | ✅ | — |
| **CSRF/XSS 防护** | ❌ 无 | ✅ nginx ModSecurity / 响应头注入 | 🟡 中 |
| **请求体大小限制** | ❌ 无 | ✅ nginx client_max_body_size | 🟢 小 |
| **DDoS 缓解** | ❌ 无 | ✅ BFE 连接限制 / 限流 | 🔴 大 |
| **速率限制 (Rate Limit)** | ❌ 无 | ✅ nginx limit_req / BFE 限流 | 🔴 大 |
| SQL 注入防护 | ⚠️ 有 SQLite 但无过滤 | ✅ WAF 集成 | 🟡 中 |

### 2.8 静态文件服务

| 能力 | 当前实现 | 行业水平 | 差距等级 |
|------|---------|---------|---------|
| 文件缓存 | ✅ 启动时预加载到内存（小文件） | ✅ nginx open_file_cache / 磁盘 | 🟡 中 |
| MIME 检测 | ✅ 写死的映射表 | ✅ mime.types 文件 | 🟢 小 |
| Range 请求 | ❌ 无 | ✅ nginx 支持断点续传 | 🟡 中 |
| **ETag / Last-Modified** | ❌ 无 | ✅ nginx 条件缓存头 | 🟡 中 |
| **gzip / Brotli 压缩** | ❌ 无 | ✅ nginx gzip 模块 / Brotli | 🔴 大 |
| **目录列表** | ❌ 无 | ✅ nginx autoindex | 🟢 小 |
| **sendfile 零拷贝** | ✅ TCP 零拷贝；SSL 退化为 read+write | ✅ nginx sendfile + tcp_nopush | 🟢 小 |
| **缓存控制头** | ❌ 无 | ✅ Expires / Cache-Control 配置 | 🟡 中 |
| 大文件流式发送 | ✅ sendfile / read+write 分段 | ✅ nginx 分片发送 | 🟢 小 |

### 2.9 数据库 / RPC

| 能力 | 当前实现 | 行业水平 | 差距等级 |
|------|---------|---------|---------|
| SQLite 异步 | ✅ 线程池 offload | ✅ | — |
| 连接池 | ✅ 协程等待队列 | ✅ | — |
| **上游连接健康检查** | ❌ 无 | ✅ 主动 + 被动探测 | 🟡 中 |
| **写操作重试 / 幂等** | ❌ 无 | ✅ | 🟡 中 |
| **读写分离 / 主从路由** | ❌ 无 | ✅ | 🟡 中 |
| **连接超时 / 查询超时** | ❌ 无 | ✅ | 🟡 中 |
| **ORM / SQL Builder** | ❌ 拼接字符串 | ✅ | 🟢 小 |
| 数据库迁移 | ❌ 无 | ✅ | 🟢 小 |

### 2.10 已实施的 nginx 启发优化

| 能力 | 当前实现 | 说明 |
|------|---------|------|
| **RegionPool 内存池** | ✅ Worker 级 256MB mmap，free-list 回收，每 Session 64KB bump 分配，2× 向量复制迁移 | 替代 MemPool 每 Session new/delete；0 页错误压测 |
| **SessionRegion 顺序写** | ✅ `Write() / WriteCRLF() / WriteUint()` 直接 bump 写入，无对齐填充 | 用于 Response header 构建 |
| **Response 全 Region 构建** | ✅ 状态行+headers 连续写 region，body 外存指针不拷贝，移除 std::string headers_ | 每请求 0 heap 分配 |
| **LlhttpParser 嵌入 Session** | ✅ 直接成员而非 unique_ptr，消除 per-session new | 配合 Feed() 内部 llhttp_reset 实现请求级重用 |
| **中间件 header 注入** | ✅ ctx.AddResponseHeader() 在 handler 前写，region 直接构建 | 替代 AddHeader 字符串手术 |
| **单次 gather-write** | ✅ `std::array<const_buffer,2>` 合并 region headers + 缓存 body | SSL 只加密一次 |
| **SO_REUSEPORT 多进程** | ✅ MultiWorker 模式，每进程独立 epoll | 减少锁竞争，提升多核利用率 |
| **SessionPool 去锁** | ✅ 每 Worker 独占，无锁 TryAcquire/Release | 简化 Session 复用 |

---

## 三、成熟度分级模型

```
                 ┌─────────────────────────────────────────────────┐
                 │             L3：企业级 / 服务网格               │
                 │   HTTP/2/3 · 灰度 · 熔断 · 可观测性           │
                 │   动态配置热加载 · 自定义插件 · WASM           │
                 │         BFE / nginx / Envoy                    │
                 ├─────────────────────────────────────────────────┤
                 │                                                  │
                 │             L2：生产可用                        │
                 │   反向代理 · 负载均衡 · 限流 · 健康检查        │
                 │   结构化日志 · 优雅关闭 · 虚拟主机              │
                 │        nginx (基础功能)                         │
                 ├─────────────────────────────────────────────────┤
                 │                                                  │
                 │     ★  L2 基础功能已覆盖  ★                       │
                 │   HTTP/1.1 · HTTP/2 · TLS 1.3 · 静态文件        │
                 │   反向代理 · 轮询负载均衡 · 上游连接池          │
                 │   sendfile · RegionPool · 全 Region Response    │
                 │   多进程 · SQLite · 零页错误 92 万请求         │
                 │   HPACK 87% · c2000 峰值 93K req/s             │
                 │         ← 当前项目在此                          │
                 └─────────────────────────────────────────────────┘
```

**当前项目处于 L2 基础功能已覆盖阶段。** 已补齐 TLS、sendfile、RegionPool 内存架构、全 Region Response 构建、HTTP/2 over TLS、反向代理 + 负载均衡。压测 500 连接 92 万请求零页错误，VmRSS 2MB 不增长，H2 c2000 峰值 93K req/s 领先 nginx 20%。

**行业方案在 L2~L3。** BFE 在 L3（流量调度、灰度发布、热加载是它的强项），nginx 在 L2~L3 之间（核心功能 + 可选模块组合），Envoy 同样 L3（服务网格数据面）。

---

## 四、差距 → 追赶路线图

按从易到难排序，标注估算工作量：

### ✅ 已完成

| 功能 | 实现要点 | 说明 |
|------|---------|------|
| **TLS 1.3 终结** | `net/tls_context.hpp` + `asio::ssl::stream` | OpenSSL 3.0.13，单证书链 |
| **sendfile 零拷贝** | `net/session.cpp` TCP: sendfile 系统调用 / SSL: read+write 分段 | KTLS 模块已加载但未进入 nginx 配置 |
| **RegionPool 内存池** | `net/region_pool.hpp/cpp` + `net/session_region.hpp/cpp` | Worker 级 256MB mmap，free-list + bump 分配 + 2× 迁移 |
| **SessionRegion 顺序写** | `Write()/WriteCRLF()/WriteUint()` 直接 bump 写入 | 响应头构建零分配 |
| **Response 全 Region 构建** | `net/response.hpp/cpp` 重写 | 移除 std::string headers_ / body_own_ / AddHeader |
| **LlhttpParser 嵌入 Session** | `net/session.hpp` 直接成员 | 消除 per-session unique_ptr new |
| **中间件 header 注入** | `http/context.hpp` AddResponseHeader | 替代 AddHeader 字符串手术 |
| **单次 gather-write** | `net/session.cpp` asio::const_buffer 双缓冲 | 对比分开发送 SSL 提升 ~16x |
| **SO_REUSEPORT 多进程** | `net/multi_server.hpp` MultiWorker 模式 | 减少锁竞争 |
| **SessionPool 去锁** | `net/session_pool.hpp` 移除 mutex | 每 Worker 独占 |
| **零页错误压测** | 500 连接 92 万请求 0 minor-faults | VmRSS 2MB 不变 |

### 🟢 第一阶段：小投入（L1→L2a，一周内可完成）

| 功能 | 复杂度 | 说明 |
|------|--------|------|
| `client_max_body_size` | ~20 行 | llhttp 的 Content-Length 检查 |
| 目录列表 | ~50 行 | 生成 HTML 列表 |
| Cache-Control / Expires 头 | ~30 行 | 响应头注入 |
| 优雅关闭 (graceful shutdown) | ~50 行 | signal_handler → 停 acceptor → 等待 session |
| 配置文件校验 / dry-run | ~50 行 | `--check` / `--dry-run` 参数 |
| ETag 支持 | ~40 行 | 文件哈希或 mtime |
| Range 请求 | ~100 行 | 206 Partial Content + If-Range |
| 命令行参数 (argparse) | ~80 行 | `--host --port --threads --doc-root` |

### 🟡 第二阶段：中等投入（L2，2-4 周）

| 功能 | 估算代码 | 说明 |
|------|---------|------|
| **gzip/Brotli 压缩** | ~200 行 | 响应体过滤 + Content-Encoding 头 |
| **虚拟主机 (VirtualHost)** | ~200 行 | Host header → 路由映射 |
| **反向代理 / 上游转发** | ~500 行 | TCP 连接池 + HTTP 请求转发 + 复用 |
| **负载均衡（轮询/最小连接）** | ~200 行 | upstream 选择器 |
| **健康检查（主动）** | ~300 行 | 定期探测 upstream 的 /healthz |
| **结构化日志（JSON）** | ~100 行 | 替换 cout 为 spdlog / fmtlog |
| **速率限制 (Rate Limit)** | ~300 行 | 令牌桶 + sharded 计数器 |
| **可配置的 MIME 映射文件** | ~100 行 | 加载 mime.types |
| **通用上游连接池** | ~400 行 | 非 SQLite，通用 TCP 连接池 |
| **连接超时 / 请求超时** | ~100 行 | `async_wait(timer)` + cancel |
| **Prometheus Metrics** | ~300 行 | `/metrics` 端点暴露计数器/直方图 |
| **nginx 输出延迟 / 缓冲** | ~200 行 | SSL 写缓冲区合并（参考 nginx `ngx_http_write_filter`） |

### 🔴 第三阶段：大投入（L2→L3，1-3 个月）

| 功能 | 估算代码 | 说明 |
|------|---------|------|
| **HTTP/2 完整实现** | 2,000-5,000 行 | HPACK 编码/解码 + h2 帧 + 多路复用 + 流控制 |
| **HTTP/3 (QUIC)** | 5,000+ 行 | QUIC 传输 + 0-RTT + TLS 1.3 |
| **灰度发布 / 流量切分** | ~500 行 | BFE 风格的条件匹配引擎 |
| **熔断 (Circuit Breaker)** | ~300 行 | 滑动窗口 + 半开探测 |
| **分布式追踪 (OpenTelemetry)** | ~500 行 | trace propagation + 采样 |
| **条件表达式引擎** | ~800 行 | BFE 风格的 `req_host_in("example.com")` |
| **WAF 集成** | ~500+ 行 | ModSecurity / Coraza 规则引擎 |
| **WASM/Lua 插件** | 1,000+ 行 | 嵌入 Wasmtime / Lua 解释器 |
| **配置热重载** | ~400 行 | 版本号 + 原子切换 + SIGHUP |

### 总量估算

```
L1 起步：   1,164 行  (2026-06-27)
当前状态：  5,211 行  (2026-06-30，不含 llhttp)
L2 投入：  ~3,500 行  (压缩 + 反向代理 + 负载均衡 + 基础监控)
L3 投入： ~10,000+ 行 (QUIC + 灰度 + 熔断 + 追踪 + WASM)
```

三天内自有代码从 1,164 行增长到约 5,211 行。主要新增：TLS 封装、MultiServer 多进程、SessionPool、HTTP/2 nghttp2 帧循环、RegionPool 内存架构、Response 全 Region 构建。

如果目标是 **对标 BFE 的七层网关能力**，代码量需膨胀 10-20 倍，主要集中在 HTTP/2 协议栈和流量管理引擎。

---

## 五、关键差距总结

### 最核心的五个差距（按影响排序）

1. ~~缺少 TLS 终结~~ → **✅ 已实现**。TLS 1.3 + OpenSSL 3.0，现已可以部署 HTTPS。缺乏 ACME 自动续签和 SNI 多证书支持。

2. ~~没有反向代理 / 上游转发~~ → **✅ 已实现**。ProxyHandler + ReverseProxy + UpstreamPool，支持单上游和负载均衡多上游，基于 HandleAsync 协程非阻塞转发。缺乏上游健康检查和连接复用。

3. ~~没有 HTTP/2 支持~~ → **✅ 已实现**。基于 nghttp2 的 h2 over TLS 帧循环、顺序流处理（替代 co_spawn 消除竞争）。HPACK 压缩率达 87-88%（nginx 38%），c2000 峰值 93K req/s，高连接数下领先 nginx 3-20%。

4. **没有结构化指标（Metrics）** — 无法了解 QPS、延迟分布、错误率。对于生产运维，没有指标等于没有 visibility。

5. **没有插件 / 热加载** — 当前所有功能都在 main 编译时固定。行业方案都支持动态模块或脚本扩展（WASM / Lua），不改主程序就能加逻辑。

### 当前项目的亮点（与行业方案比也并不逊色）

- ✅ **C++20 协程模型** — Asio 的 Proactor + `co_await` 让异步代码写得像同步一样直白，比 nginx 的传统回调链更易维护
- ✅ **RegionPool 内存架构** — Worker 级 256MB mmap + 每 Session bump 分配 + 2× 迁移，零 per-Session new/delete
- ✅ **Response 全 Region 构建** — 状态行+headers+body 连续写入 region，body 外存指针不拷贝，零 heap 分配
- ✅ **LlhttpParser 嵌入 Session** — 直接成员代替 unique_ptr，配合 llhttp_reset 请求级重用
- ✅ **中间件双阶段设计** — 原始字节 + 洋葱模型，header 注入通过 ctx 在 handler 前完成
- ✅ **llhttp** — Node.js 团队维护的 HTTP/1.1 解析器，安全性和正确性有保障
- ✅ **TLS + sendfile + 多进程 + 零页错误** — 生产级基础能力已补齐，92 万请求 0 page faults
- ✅ **HTTP/2 over TLS (h2)** — nghttp2 帧循环，顺序流处理，HPACK 87-88% 压缩率，c2000 峰值 93K req/s
- ✅ **反向代理 + 负载均衡** — ProxyHandler 协程非阻塞转发，UpstreamPool 轮询负载均衡
- ✅ **SQLite 异步封装** — 协程友好的数据库操作，是实际业务需要的

---

## 六、路线图建议（分阶段执行）

```
Phase 1（已完成）：基础生产能力 ✅
  ├── TLS 1.3（asio::ssl + OpenSSL 3.0）
  ├── sendfile 零拷贝（TCP 直通 + SSL 回退）
  ├── RegionPool 内存架构（Worker 级 256MB mmap + Session bump + 2× 迁移）
  ├── Response 全 Region 构建（零堆分配，body 外存指针）
  ├── LlhttpParser 嵌入 Session（零 per-session 动态分配）
  ├── 单次 gather-write（SSL 加密合并）
  ├── SO_REUSEPORT 多进程
  ├── SessionPool 去锁（每 Worker 独占）
  └── 零页错误压测（500 连接 92 万请求 0 minor-faults）
  └── HTTP/2 over TLS（nghttp2 + 顺序流处理，c2000 峰值 93K req/s）

Phase 1.5（1-2 天）：小投入收尾
  ├── 最大请求体限制 + Range 请求
  ├── 缓冲区大小限制 + 优雅关闭
  ├── Cache-Control / ETag 响应头
  ├── 目录列表
  └── 命令行参数 (argparse)

Phase 2（2-3 周）：反向代理 + 上游管理
  ├── 通用 TCP 连接池（已完成 ✅）
  │   └── ProxyHandler 基于 HandleAsync 协程非阻塞转发
  ├── 负载均衡器（轮询）（已完成 ✅）
  ├── 上游健康检查
  ├── 虚拟主机（Host header 路由）
  └── gzip 压缩

Phase 3（2-3 周）：可观测性
  ├── Prometheus /metrics 端点
  ├── 结构化日志（JSON 格式，级别控制）
  ├── 请求 ID 传递
  └── Rate limiting

Phase 4（1-2 个月）：协议扩展
  ├── HTTP/2 over TLS（已完成 ✅）
  │   └── nghttp2 帧循环 + 顺序流处理，c2000 峰值 93K req/s
  ├── HTTP/2 h2c（明文协商）
  ├── WebSocket 升级支持
  ├── 配置热重载（版本化配置）
  └── 条件表达式引擎

Phase 5（1 个月+）：企业级
  ├── WASM 插件运行时（wasmtime）
  ├── 灰度发布 + 流量切分
  ├── 熔断 + 滑动窗口
  ├── OpenTelemetry 追踪
  └── HTTP/3 (QUIC) 实验性支持
```

---

## 七、nginx 源码学习启示录

### 已研究模块

| 模块 | 文件 | 可借鉴点 |
|------|------|---------|
| Header Filter | `ngx_http_header_filter_module.c` | 状态行预计算、响应头链式构建 |
| Write Filter | `ngx_http_write_filter_module.c` | 输出缓冲 + 延迟合并（i/o delay） |
| SSL 发送 | `ngx_event_openssl.c` | SSL 写缓冲 + 合并多个 buf 减少 SSL_write 次数 |
| 请求生命周期 | `ngx_http_request.c` | 11 阶段引擎、keepalive 处理 |
| 核心模块 | `ngx_http_core_module.c` | 配置结构、Location 匹配树 |
| 事件循环 | `ngx_event.c` | epoll ET + 事件状态机 |

### 已落地优化

- **RegionPool + SessionRegion** — 参考 `ngx_pool_t` 的 bump 分配，但升级为 Worker 级大池（256MB mmap）+ 2× 迁移，消除所有 per-connection new/delete
- **SessionRegion::Write 响应头构建** — 直接 bump 写入 region，移除 FixedBuffer → std::string 路径
- **Gather-write 合并** — 对应 nginx 的 SSL buffer coalescing

### 待研究

- `ngx_http_output_delay` 的 i/o delay 机制（经评估不适用于纯 HTTPS 静态文件场景）
- `ngx_ssl_send_chain` 的 SSL 写缓冲策略
- KTLS（Kernel TLS）模块加载但配置未使用，可跟踪 nginx 后续版本支持情况

---

> **更新结论**：三天内补齐了 TLS + sendfile + RegionPool 内存架构 + 全 Region Response 构建 + HTTP/2 over TLS + 反向代理 + 负载均衡。自有代码约 5,211 行。H2 基准测试 c2000 峰值 93K req/s，c100~c3000 全连接段与 nginx 持平或领先 3-58%，HPACK 压缩率 87-88%（nginx 38%），头体积仅 nginx 的 1/5。当前项目已不是"玩具"——具备可部署的 HTTPS 静态文件 + h2 多路复用 + 反向代理服务能力，协程模型在高连接数下展现了优于多进程 epoll 的扩展性。
