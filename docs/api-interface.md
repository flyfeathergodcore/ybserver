# HTTP Server — 开发接口文档

> 面向框架使用者的技术参考 | 2026-06-30

---

**上卷：Quick Start** — 按场景学习如何使用本框架
**下卷：API Reference** — 按类查阅完整接口签名

---

# 上卷：Quick Start

---

## 1. 最小服务器

从 `main()` 启动一个 HTTPS 服务器需要 4 步：

```cpp
#include "config/config.hpp"
#include "handler/router.hpp"
#include "handler/metrics.hpp"
#include "middleware/middleware.hpp"
#include "net/multi_server.hpp"
#include "ssl/tls_context.hpp"

int main() {
    // 1. 加载配置
    Config cfg = Config::Load("./config.yaml");

    // 2. 初始化路由（自动注册静态文件 + 代理路由）
    Router router;
    router.SetupFromConfig(cfg);

    // 3. 挂载中间件
    auto collector = std::make_shared<MetricsCollector>(cfg.threads);
    MiddlewareManager mw;
    mw.Add(std::make_unique<MetricsMiddleware>(collector.get()));
    mw.Add(std::make_unique<CORSMiddleware>());
    mw.Add(std::make_unique<LoggingMiddleware>());

    // 4. 启动
    auto tls = std::make_shared<TlsContext>();
    tls->Load(cfg.tls_cert, cfg.tls_key);
    MultiServer server(cfg, router, mw, tls, collector);
    server.Start();
}
```

### 组装顺序（不可调换）

```
Config → Router → MiddlewareManager → TlsContext → MultiServer
                              ↑
                     MetricsCollector
```

- `FileCache` 必须在 `router.Add()` 之前加载
- `MetricsCollector` 必须先构造，再传给 `MetricsMiddleware` 和 `MultiServer`
- `MiddlewareManager::Add()` 的顺序 = 执行顺序

---

## 2. 路由注册

### 路径语法

```cpp
// 精确匹配
router.Add("/hello", handler);           // 只匹配 /hello
router.Get("/users/:id", handler);       // 匹配 /users/42，捕获 id=42
router.Post("/files/*path", handler);    // 匹配 /files/a/b/c，捕获 path=a/b/c

// 前缀匹配（路径以 / 结尾）
router.Add("/api/", handler);            // 匹配 /api/、/api/v1/users 等所有子路径
router.Add("/", handler);                // 匹配所有路径（兜底路由）
```

### 匹配优先级

```
精确静态  >  :param  >  *catchAll  >  前缀匹配
```

同一前缀路径下，静态节点优先级高于参数节点。

### 快捷方法

```cpp
router.Get("/a", h1);       // 仅 GET
router.Post("/a", h2);      // 仅 POST
router.Put("/a", h3);       // 仅 PUT
router.Delete("/a", h4);    // 仅 DELETE
router.Head("/a", h5);      // 仅 HEAD
router.Add("/b", h6);       // 所有方法
```

`Add()` 注册的 handler 接收任意 HTTP 方法；`Get()`/`Post()` 等仅匹配对应方法。

---

## 3. 编写 Handler（同步）

### 完整示例

```cpp
#include "handler/request_handler.hpp"
#include "http/context.hpp"
#include "net/response.hpp"

class HelloHandler : public RequestHandler {
public:
    Response Handle(const Context& ctx) override {
        // 读取请求
        auto method = ctx.Method();       // "GET"
        auto path   = ctx.Path();         // "/hello"
        auto agent  = ctx.Header("User-Agent");
        auto body   = ctx.Body();

        // 构建响应
        Response resp(200, *ctx.Pool());
        resp.Header("Content-Type", "text/plain; charset=utf-8");
        resp.EndHeaders();
        resp.Body("Hello, World!");
        return resp;
    }
};
```

### 同步路径约定

| 方法 | 返回值 | 执行线程 | 适用场景 |
|------|--------|----------|----------|
| `Handle()` | `Response` | Worker 线程 | 静态文件、简单计算、内存查询 |

Handler 无需声明 `IsAsync()`——默认返回 `false`，Session 自动走同步路径。

### Context 读请求

参见 [API Reference: Context](#11-context)。

### Response 构建

参见 [API Reference: Response](#12-response)。

### 注册 Handler

```cpp
router.Add("/hello", std::make_unique<HelloHandler>());
```

---

## 4. 编写 Handler（异步）

适合反向代理、数据库查询等 I/O 密集型场景。

```cpp
class MyAsyncHandler : public RequestHandler {
public:
    // 声明异步路径
    bool IsAsync() const override { return true; }

    // 协程实现
    asio::awaitable<Response> HandleAsync(const Context& ctx) override {
        auto executor = co_await asio::this_coro::executor;

        // 模拟异步 I/O（如数据库查询、HTTP 调用）
        asio::steady_timer timer(executor);
        timer.expires_after(std::chrono::milliseconds(10));
        co_await timer.async_wait(asio::use_awaitable);

        Response resp(200, *ctx.Pool());
        resp.Header("Content-Type", "text/plain");
        resp.EndHeaders();
        resp.Body("async result");
        co_return resp;
    }

    // 同步路径也必须实现
    Response Handle(const Context& ctx) override {
        // 退化为同步实现，仅当 IsAsync()=false 时被调用
        return Response::Error(500, *ctx.Pool());
    }
};
```

### 异步路径约定

| 方法 | 返回值 | 执行线程 | 说明 |
|------|--------|----------|------|
| `IsAsync()` | `bool` | — | 返回 `true` 启用异步路径 |
| `HandleAsync()` | `asio::awaitable<Response>` | Worker 线程 | Session 会 `co_await` 此方法 |

### 注意事项

- `HandleAsync()` 使用 `co_await`，必须在 `main()` 所在 `.cpp` 文件**启用协程支持**
- `Handle()` 必须仍然实现（纯虚方法），但不会在异步路径下被调用
- Handler 生命周期由 `Router` 管理（通过 `unique_ptr`），Async handler 内部不能引用已释放对象

---

## 5. 反向代理

### 上游配置（config.yaml）

```yaml
proxy:
  - prefix: "/api/"
    upstreams:
      - "127.0.0.1:3000"
      - "127.0.0.1:3001"   # 多上游自动轮询负载均衡
```

### 代码注册

```cpp
// 直接从地址列表创建 ReverseProxy（推荐）
router.Add("/api/", std::make_unique<ReverseProxy>(
    std::vector<UpstreamAddr>{
        {"127.0.0.1", 3000},
        {"127.0.0.1", 3001}
    }
));
```

### 健康检查

| 机制 | 说明 |
|------|------|
| 被动检查 | 连续 3 次失败 → 标记 dead |
| 冷却恢复 | dead 10 秒后自动重试 |
| 全 dead 回退 | 尝试最后一次成功的上游 |
| 选择算法 | 加权 Round-Robin |

---

## 6. Middleware

### 内置 Middleware

| Middleware | 阶段 | 职责 |
|-----------|------|------|
| `CORSMiddleware` | Pre | 注入 `Access-Control-Allow-Origin: *`；OPTIONS 204 |
| `MetricsMiddleware` | Pre | 拦截 `/metrics.json`、`/metrics/stream`、`/dashboard` |
| `MetricsMiddleware` | Post | 记录每请求耗时、状态码、协议类型 |
| `LoggingMiddleware` | Post | 打印 `METHOD /path` 到终端 |

### 注册顺序

```cpp
MiddlewareManager mw;
mw.Add(std::make_unique<CORSMiddleware>());       // 1st Pre
mw.Add(std::make_unique<MetricsMiddleware>(...)); // 2nd Pre + Post
mw.Add(std::make_unique<LoggingMiddleware>());    // 3rd Post
```

PreRequest 按 Add 顺序执行；PostResponse 也按 Add 顺序执行。

### 自定义 Middleware

```cpp
class MyMiddleware : public Middleware {
public:
    Type GetType() const override { return Type::Both; }

    // PreRequest — 在 Handler 之前执行
    Response HandlePre(Context& ctx) override {
        auto ua = ctx.Header("User-Agent");
        if (ua.empty()) {
            // 返回有效 Response 会短路 Handler
            return Response::Raw(400, "HTTP/1.1 400 Bad Request\r\n..."
                                      "Content-Length: 0\r\n\r\n");
        }
        ctx.AddResponseHeader("X-My-Middleware", "hi");
        return Response::None();   // 继续执行 Handler
    }

    // PostResponse — 在响应发送后执行
    asio::awaitable<void> HandlePost(const Context& ctx,
                                     int status_code,
                                     size_t bytes_sent,
                                     uint64_t elapsed_us,
                                     int worker_id) override {
        // 记录日志、发送指标等
        co_return;
    }
};

// 注册
mw.Add(std::make_unique<MyMiddleware>());
```

### Middleware 短路规则

- `HandlePre()` 返回 `Response::None()` → 继续执行下一个 Middleware / Handler
- `HandlePre()` 返回有效 `Response` → **立即返回**，后续 Middleware 和 Handler 不再执行

---

## 7. Metrics 监控

### HTTP 端点

| URL | 返回格式 | 说明 |
|-----|----------|------|
| `GET /metrics.json` | JSON | 完整历史（60 秒窗口） |
| `GET /metrics/stream` | SSE | 实时推送（1 秒间隔） |
| `GET /dashboard/` | HTML | 可视化仪表盘 |

### Metrics JSON 字段

```json
{
  "qps": 51226,        // 总 QPS
  "qps_h1": 51226,     // H1 QPS
  "qps_h2": 0,         // H2 QPS
  "err": 0,            // 总错误/秒
  "err_h1": 0,         // H1 错误
  "err_h2": 0,         // H2 错误
  "p50": 145,          // p50 延迟 (µs)
  "p90": 430,          // p90 延迟 (µs)
  "p99": 911,          // p99 延迟 (µs)
  "act": 18,           // 活跃连接数
  "bytes": 12806500,   // 发送字节数
  "t": 14876           // 时间戳
}
```

### SSE 事件

| 事件 | 触发 | 数据格式 |
|------|------|----------|
| `full` | 首次连接 | 完整 JSON（同 /metrics.json） |
| `metrics` | 每秒 | 单条 JSON |
| `alert` | 状态变化 | `{"name":"...","state":"firing","value":...}` |

### Dashboard 图表

| 图表 | Canvas ID | 数据源 |
|------|-----------|--------|
| QPS & Errors | `chart-qps` | qps + err |
| H1 QPS & Errors | `chart-h1` | qps_h1 + err_h1 |
| H2 QPS & Errors | `chart-h2` | qps_h2 + err_h2 |
| Latency | `chart-latency` | p50 + p90 + p99 |
| Active Connections | `chart-connections` | act |

---

## 8. 配置详解

### config.yaml 完整字段

```yaml
server:
  host: "0.0.0.0"        # 监听地址
  port: 8081             # 明文 HTTP 端口（可选，0=不监听明文）
  tls_port: 8443         # TLS 端口（H1 + H2，必须配置证书）
  threads: 4             # Worker 线程数（建议 = CPU 核心数）
  doc_root: "./www"      # 静态文件根目录
  # cpu_affinity: false  # 取消注释可关闭 CPU 核心绑定（默认 true）
  tls:
    cert: "./cert.pem"   # PEM 证书路径（必需）
    key:  "./cert.key"   # PEM 私钥路径（必需）

proxy:
  - prefix: "/api/"      # 路由前缀（必须以 / 结尾 = 前缀匹配）
    upstreams:            # 上游列表（至少一个）
      - "127.0.0.1:3000"
      - "127.0.0.1:3001"
```

### 启动参数

```bash
./http_server              # 默认加载 ./config.yaml
./http_server /path/to/config.yaml
```

### Config 结构

```cpp
struct Config {
    std::string host = "0.0.0.0";
    unsigned short port = 8080;
    unsigned short tls_port = 0;    // 0 = TLS disabled
    int threads = 4;
    std::string doc_root = "./www";
    std::string tls_cert;
    std::string tls_key;
    bool cpu_affinity = true;

    // 代理路由（从 proxy 段解析）
    std::vector<ProxyRoute> proxy_routes;

    static Config Load(const std::string& path);
};
```

---

# 下卷：API Reference

---

## 9. Router

### `void Router::Add(std::string path, std::unique_ptr<RequestHandler> handler)`

**说明：** 注册一个接收任意 HTTP 方法的 handler。

**参数：**
| 参数 | 类型 | 说明 |
|------|------|------|
| path | `std::string` | 路由路径。以 `/` 结尾 = 前缀匹配（如 `/api/` 匹配 `/api/v1/users`） |
| handler | `std::unique_ptr<RequestHandler>` | Handler 所有权转移给 Router |

**路径语法：**
| 模式 | 示例 | 匹配 |
|------|------|------|
| 精确静态 | `/hello` | 仅 `/hello` |
| 前缀匹配 | `/api/` | `/api/`、`/api/v1`、`/api/v1/users` 等 |
| :param | `/users/:id` | `/users/42`，捕获 `id=42` |
| *catchAll | `/files/*path` | `/files/a/b/c`，捕获 `path=a/b/c` |

**注意：**
- `Add()` 注册的 handler 会匹配任何 HTTP 方法（GET/POST/PUT/DELETE/HEAD/OPTIONS）。
- Handler 所有权由 Router 管理，勿在外部释放。
- `:param` 必须占据完整路径段（不能 `/use:rs/`）；`*catchAll` 必须在路径末尾。

---

### `void Router::SetupFromConfig(const Config& cfg)`

**说明：** 从 `Config` 自动注册所有路由。等价于手动调用 `Add()` 注册静态文件 handler + 所有 proxy 路由。

**参数：**
| 参数 | 类型 | 说明 |
|------|------|------|
| cfg | `const Config&` | 配置对象（`Config::Load()` 加载） |

**内部行为：**
| 注册项 | 源 | 说明 |
|--------|-----|------|
| `"/"` → `StaticFileHandler` | `cfg.doc_root` | 前缀匹配，低优先级兜底 |
| `pr.prefix` → `ReverseProxy` | `cfg.proxy_routes` | 每个 proxy 路由注册一个 |

**典型用法：**
```cpp
Router router;
router.SetupFromConfig(cfg);  // 替代手动 Add + 循环
```

---

### `RequestHandler* Router::Match(std::string_view method, std::string_view path, std::vector<std::pair<std::string_view, std::string_view>>* params = nullptr)`

**说明：** 匹配路由并返回对应 handler。

**参数：**
| 参数 | 类型 | 说明 |
|------|------|------|
| method | `string_view` | HTTP 方法，如 `"GET"` |
| path | `string_view` | 请求路径，如 `"/api/users/42"` |
| params | `vector<pair<string_view,string_view>>*` | 可选，接收路径参数（:param / *catchAll 的值） |

**返回值：** `RequestHandler*` — 匹配到的 handler；未匹配返回 `nullptr`。

---

### 快捷方法

```cpp
void Router::Get(std::string path, std::unique_ptr<RequestHandler> h);
void Router::Post(std::string path, std::unique_ptr<RequestHandler> h);
void Router::Put(std::string path, std::unique_ptr<RequestHandler> h);
void Router::Delete(std::string path, std::unique_ptr<RequestHandler> h);
void Router::Head(std::string path, std::unique_ptr<RequestHandler> h);
```

**说明：** 仅匹配对应 HTTP 方法的路由注册。内部调用 `AddRoute()`。

**参数：** 同 `Add()`。

---

## 10. RequestHandler

### `virtual Response Handle(const Context& ctx) = 0`

**说明：** 纯虚方法。所有 Handler 必须实现。同步执行，在 Worker 线程上直接调用。

**参数：**
| 参数 | 类型 | 说明 |
|------|------|------|
| ctx | `const Context&` | 请求上下文（Method / Path / Header / Body） |

**返回值：** `Response` — HTTP 响应对象。必须通过 `ctx.Pool()` 传入内存池。

**注意：** 返回值不可为 `Response::None()`（None 仅用于 Middleware 短路）。

---

### `virtual asio::awaitable<Response> HandleAsync(const Context& ctx)`

**说明：** 异步协程路径。默认实现 `co_return Handle(ctx)`。当 `IsAsync()=true` 时，Session 会 `co_await` 此方法。

**参数：** 同 `Handle()`。

**返回值：** `asio::awaitable<Response>` — 协程风格的异步响应。

---

### `virtual bool IsAsync() const`

**说明：** 返回当前 Handler 是否使用异步路径。

**返回值：**
| 值 | 说明 |
|----|------|
| `false`（默认） | 使用同步路径，直接调用 `Handle()` |
| `true` | 使用异步路径，Session 会 `co_await HandleAsync()` |

---

## 11. Context

### `std::string_view Context::Method() const`

**说明：** 返回 HTTP 方法。

**返回值：** `string_view` — 如 `"GET"`、`"POST"`。生命周期：当前请求处理期间有效。

---

### `std::string_view Context::Path() const`

**说明：** 返回请求路径（不含 query string）。

**返回值：** `string_view` — 如 `"/hello"`、`"/api/users/42"`。

---

### `std::string_view Context::Version() const`

**说明：** 返回 HTTP 协议版本。

**返回值：** `string_view` — `"HTTP/1.1"`（H1）或 `"h2"`（H2）。

---

### `std::string_view Context::Header(std::string_view key) const`

**说明：** 按名称读取请求头（大小写不敏感）。

**参数：**
| 参数 | 类型 | 说明 |
|------|------|------|
| key | `string_view` | 请求头名称，如 `"content-type"` |

**返回值：** `string_view` — 请求头值；不存在返回空 `string_view`。

---

### `int Context::HeaderCount() const`

**说明：** 返回请求头总数。

**返回值：** `int` — 头部数量。

---

### `std::pair<std::string_view, std::string_view> Context::HeaderAt(int i) const`

**说明：** 按索引读取单个请求头。

**参数：**
| 参数 | 类型 | 说明 |
|------|------|------|
| i | `int` | 索引，范围 `[0, HeaderCount())` |

**返回值：** `pair<string_view, string_view>` — `.first` = 名称，`.second` = 值。

---

### `std::string_view Context::Body() const`

**说明：** 返回请求体。

**返回值：** `string_view` — 请求体内容；无 body 返回空 `string_view`。

---

### `bool Context::IsHttp2() const`

**说明：** 判断当前请求是否为 HTTP/2。用于协议差异化处理，如 Metrics 区分 H1/H2 计数。

**返回值：** `bool` — H2 返回 `true`，H1 返回 `false`。

---

### `void Context::AddResponseHeader(std::string_view key, std::string_view value)`

**说明：** 注入响应头（Middleware→Handler 通信）。Handler 通过 `ResponseHeaderCount()`/`ResponseHeaderKey()`/`ResponseHeaderVal()` 读取并写入 Response。

**参数：**
| 参数 | 类型 | 说明 |
|------|------|------|
| key | `string_view` | 头部名称 |
| value | `string_view` | 头部值 |

**限制：** 最多 8 个（`kMaxExtraHeaders`），超出忽略。

---

### `SessionRegion* Context::Pool() const`

**说明：** 返回当前请求的内存池指针。所有请求相关数据的生命周期与内存池绑定。

**返回值：** `SessionRegion*` — 内存池指针。传递给 `Response` 构造函数和 `Response::Error()`。

**注意：** 返回 `nullptr` 时表示内存池不可用（应避免此情况）。

---

## 12. Response

### `Response(int status_code, SessionRegion& region)`

**说明：** 标准响应构造。使用 Region 内存池构建响应头和体。

**参数：**
| 参数 | 类型 | 说明 |
|------|------|------|
| status_code | `int` | HTTP 状态码，如 200、404、500 |
| region | `SessionRegion&` | Context 返回的内存池。如 `*ctx.Pool()` |

**典型用法：**
```cpp
Response resp(200, *ctx.Pool());
resp.Header("Content-Type", "application/json");
resp.EndHeaders();
resp.Body(json_str);
```

---

### `static Response Response::None()`

**说明：** 返回空响应。用于 Middleware `HandlePre()` 表示"不短路，继续执行"。

**返回值：** `Response` — 空的 Response 对象，`IsNone()=true`。

---

### `static Response Response::Raw(int status_code, std::string body)`

**说明：** 直接以 wire 格式构建响应（完全控制输出内容）。不经过 Region 内存池。

**参数：**
| 参数 | 类型 | 说明 |
|------|------|------|
| status_code | `int` | HTTP 状态码 |
| body | `std::string` | 完整 HTTP 响应（含头部） |

```cpp
Response::Raw(204, "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n");
```

**注意：** 必须包含完整 HTTP 头部 + 空行分隔，Handler 不会自动添加 Content-Length 等。

---

### `static Response Response::Error(int status_code, SessionRegion& region)`

**说明：** 构建标准错误响应（含 HTML 错误体）。

**参数：**
| 参数 | 类型 | 说明 |
|------|------|------|
| status_code | `int` | HTTP 错误码 |
| region | `SessionRegion&` | 内存池 |

---

### `void Response::Header(std::string_view key, std::string_view value)`

**说明：** 添加响应头。可连续调用多次。

**参数：**
| 参数 | 类型 | 说明 |
|------|------|------|
| key | `string_view` | 头部名称，如 `"Content-Type"` |
| value | `string_view` | 头部值 |

**限制：** 最多 32 个（`kMaxHeaders`）。

---

### `void Response::Header(std::string_view key, uint64_t value)`

**说明：** 重载版本，value 为整数。自动转为字符串。

```cpp
resp.Header("Content-Length", json_str.size());
```

---

### `void Response::EndHeaders()`

**说明：** 标记头部结束。必须在 `Body()` / `BodyFile()` 之前调用。之后不可再调用 `Header()`。

---

### `void Response::Body(std::string_view data)`

**说明：** 设置响应体（内存数据）。region 中的引用拷贝，不额外分配。

**参数：**
| 参数 | 类型 | 说明 |
|------|------|------|
| data | `string_view` | 响应体内容 |

**前置条件：** `EndHeaders()` 已被调用。

---

### `void Response::BodyFile(int fd, size_t file_size)`

**说明：** 使用 `sendfile` 发送文件作为响应体。适用于大文件传输。

**参数：**
| 参数 | 类型 | 说明 |
|------|------|------|
| fd | `int` | 文件描述符 |
| file_size | `size_t` | 文件大小（字节） |

**前置条件：** `EndHeaders()` 已被调用。fd 必须为可读的已打开文件。

---

### `int Response::HeaderCount() const`

**说明：** 返回已设置的结构化响应头数量。

**返回值：** `int`

---

### `std::pair<std::string_view, std::string_view> Response::HeaderAt(int i) const`

**说明：** 按索引读取响应头。H2 通过此方法消费结构化头（而非 wire 格式）。

**参数：**
| 参数 | 类型 | 说明 |
|------|------|------|
| i | `int` | 索引，范围 `[0, HeaderCount())` |

**返回值：** `pair<string_view, string_view>` — `.first`=名称，`.second`=值。

---

## 13. Middleware

### `enum Middleware::Type`

| 值 | 说明 |
|----|------|
| `PreRequest` | 仅在 Handler 之前执行 |
| `PostResponse` | 仅在响应发送后执行 |
| `Both` | 两个阶段都执行 |

---

### `virtual Type Middleware::GetType() const = 0`

**说明：** 纯虚方法，标识此中间件在哪个阶段执行。

**返回值：** `Type` — `PreRequest` | `PostResponse` | `Both`。

---

### `virtual Response Middleware::HandlePre(Context& ctx)`

**说明：** PreRequest 钩子（同步）。在 Handler 之前执行。默认返回 `Response::None()`。

**参数：**
| 参数 | 类型 | 说明 |
|------|------|------|
| ctx | `Context&` | 请求上下文（可修改，支持 `AddResponseHeader`） |

**返回值：**
| 返回值 | 行为 |
|--------|------|
| `Response::None()` | 继续执行下一个 Middleware / Handler |
| 有效 `Response` | 短路，直接作为 HTTP 响应返回 |

---

### `virtual asio::awaitable<void> Middleware::HandlePost(const Context& ctx, int status_code, size_t bytes_sent, uint64_t elapsed_us, int worker_id)`

**说明：** PostResponse 钩子（异步协程）。响应发送后执行，不阻塞客户端。

**参数：**
| 参数 | 类型 | 说明 |
|------|------|------|
| ctx | `const Context&` | 请求上下文（只读） |
| status_code | `int` | 已发送的 HTTP 状态码 |
| bytes_sent | `size_t` | 已发送的响应体字节数 |
| elapsed_us | `uint64_t` | 请求总耗时（读→解析→handler→写完成），单位 µs |
| worker_id | `int` | Worker 线程 ID |

---

### `void MiddlewareManager::Add(std::unique_ptr<Middleware> mw)`

**说明：** 注册中间件。按 `GetType()` 分派到 pre / post 列表。所有权由 Manager 管理。

**参数：**
| 参数 | 类型 | 说明 |
|------|------|------|
| mw | `std::unique_ptr<Middleware>` | Middleware 实例 |

**注意：** PreRequest 按 Add 顺序执行；PostResponse 也按 Add 顺序执行。

---

## 14. MetricsCollector

### `MetricsCollector(int num_workers)`

**说明：** 构造采集器。`num_workers` 必须与服务器实际 Worker 数一致。

**参数：**
| 参数 | 类型 | 说明 |
|------|------|------|
| num_workers | `int` | Worker 线程数（≤ `kMaxWorkers`=64） |

---

### `void MetricsCollector::OnRequest(uint64_t latency_us, int status_code, size_t bytes, int wid, bool is_h2)`

**说明：** 热路径——记录一次请求。Worker 线程中直接调用。

**参数：**
| 参数 | 类型 | 说明 |
|------|------|------|
| latency_us | `uint64_t` | 请求延迟（微秒） |
| status_code | `int` | HTTP 状态码（非 2xx 计为错误） |
| bytes | `size_t` | 响应体字节数 |
| wid | `int` | Worker ID，范围 `[0, num_workers)` |
| is_h2 | `bool` | 是否为 H2 请求 |

---

### `void MetricsCollector::OnConnectionOpen(int wid)` / `OnConnectionClose(int wid)`

**说明：** 跟踪活跃连接数。`active_connections` 为 `atomic<uint64_t>`。

---

### `void MetricsCollector::Flush(int wid)`

**说明：** 每秒调用一次。将 Worker 的当前计数器快照写入 ring buffer 并清零。

**参数：**
| 参数 | 类型 | 说明 |
|------|------|------|
| wid | `int` | Worker ID |

---

### `std::string MetricsCollector::RenderMetricsJson() const`

**说明：** 渲染完整历史 JSON（同 `/metrics.json`）。

**返回值：** `std::string` — JSON 字符串。

---

### `std::string MetricsCollector::RenderLatestSnapshot(int64_t since_ts) const`

**说明：** 渲染最新的 ring 条目 JSON。SSE 使用。

**参数：**
| 参数 | 类型 | 说明 |
|------|------|------|
| since_ts | `int64_t` | 上次推送的时间戳；无新数据返回空字符串 |

**返回值：** `std::string` — 紧凑 JSON 行，或空字符串。

---

### `std::string MetricsCollector::RenderAlertDelta(const std::vector<AlertState>& prev) const`

**说明：** 渲染告警状态变化的 SSE 事件。

**参数：**
| 参数 | 类型 | 说明 |
|------|------|------|
| prev | `const vector<AlertState>&` | 上次推送的告警状态快照 |

**返回值：** `std::string` — SSE 事件文本（多个 `event: alert\ndata: ...`），无变化返回空。
