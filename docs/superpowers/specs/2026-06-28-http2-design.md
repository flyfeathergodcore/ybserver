# HTTP/2 (h2) 支持 — 设计文档

## 概述

为 webcpp 服务器添加标准的 HTTP/2 over TLS（h2）支持，通过 ALPN 协商，与现有 HTTP/1.1 共存。

## 目录结构

```
http/
  http1.1/
    parser.hpp / .cpp          → H1Parser（原 http/h1_parser）
    session.hpp / .cpp         → H11Session（继承 SessionBase）
    session_pool.hpp / .cpp    → H11Session 对象池（从 net/ 移入）
  http2/
    session.hpp / .cpp         → H2Session（nghttp2，继承 SessionBase）

ssl/
  tls_context.hpp / .cpp       → SSL_CTX + ALPN 回调（从 net/ 移出）

net/
  session_base.hpp / .cpp      → SessionBase 基类（协议无关）
  response.hpp / .cpp          → Response 数据载体（协议无关）
  ...其余文件保持不变
```

## ALPN 协商

TLS 握手时通过 ALPN 宣告 `h2` 和 `http/1.1`，客户端协商结果决定 Session 类型：

```
TLS 握手完成
  → SSL_get0_alpn_selected()
  → "h2"        → new H2Session(stream)
  → "http/1.1"  → H11SessionPool::TryAcquire() 或 new H11Session(stream)
```

`ssl/tls_context.cpp` 添加 `SSL_CTX_set_alpn_select_cb()` 回调，首选 h2。

## SessionBase

协议无关的抽象基类：

```cpp
class SessionBase : public enable_shared_from_this<SessionBase> {
    virtual awaitable<void> Start() = 0;
    SessionRegion& Region();
    void SetMetrics(MetricsCollector*, int wid);
protected:
    SessionRegion region_;
    RequestHandler& handler_;
    MiddlewareChain& middleware_;
    MetricsCollector* metrics_ = nullptr;
    int worker_id_ = -1;
};
```

H11Session（模板 `Stream`）和 H2Session（固定 `ssl::stream`）分别继承实现。

## H2Session 架构

```
┌─────────────────────────────────────────┐
│ H2Session                               │
│                                         │
│ 读循环                                  │
│  async_read → nghttp2_session_mem_recv  │
│        │                                │
│  回调触发:                              │
│    on_begin_headers → 分配 H2StreamCtx  │
│    on_header        → region 存储头对   │
│    on_frame_recv    → co_spawn handler  │
│    on_data_chunk    → AppendBody        │
│    on_stream_close  → 清理 stream       │
│        │                                │
│ 写循环                                  │
│  nghttp2_session_mem_send → async_write │
│  (atomic flag 防止并发写)                │
│                                         │
│ streams_ map: stream_id → H2StreamCtx   │
│ region_: 所有 stream 共享 SessionRegion  │
└─────────────────────────────────────────┘
```

## H2StreamContext

每 stream 一个，使用 RegionOff 存储，与 H1Parser 内存模型一致：

```cpp
class H2StreamContext : public Context {
    void SetMethod(std::string_view);
    void SetPath(std::string_view);
    void AddHeader(std::string_view n, std::string_view v);  // → RegionOff
    
    // Context 接口
    ParseResult Feed() override { return Complete; }
    string_view Method() override;
    string_view Path() override;
    string_view Version() override { return "HTTP/2"; }
    string_view Header(key) override;
    string_view Body() override;

    RegionOff method_, path_, body_;
    struct Entry { RegionOff name, value; };
    Entry headers_[kMax];
    int header_count_ = 0;
};
```

## nghttp2 回调

| 回调 | 动作 |
|------|------|
| `on_begin_headers` | streams_[sid] = H2StreamContext{} |
| `on_header` | streams_[sid].AddHeader(name, value) |
| `on_frame_recv(HEADERS)` | co_spawn StreamHandler(stream_id) |
| `on_data_chunk_recv` | streams_[sid].AppendBody(data) |
| `on_stream_close` | streams_.erase(sid) |
| `cb_on_send` | 追加数据到 output_ 缓冲区 |

## StreamHandler 协程

```cpp
awaitable<void> HandleStream(int32_t stream_id, H2StreamContext& ctx) {
    auto resp = middleware_.Execute(ctx, handler_);
    
    // 构建响应头 (:status + 自定义头)
    vector<nghttp2_nv> headers = BuildResponseNv(resp);
    nghttp2_submit_headers(session_, ..., stream_id, headers, ...);
    nghttp2_submit_data(session_, ..., DATA);
    co_await FlushOutput();
}
```

## 构建计划

1. **ALPN 回调** — `ssl/tls_context.cpp`，~20 行
2. **H2StreamContext** — 新的 `http/http2/stream_context.hpp/cpp`
3. **H2Session 框架** — 构造、nghttp2 初始、读循环骨架
4. **nghttp2 回调实现** — 五个 C 回调
5. **Handler 协程 + 响应** — BuildResponseNv、FlushOutput
6. **MultiServer 分发** — ALPN 判断，创建 H2Session
7. **验证** — `curl --http2` 和 `wrk` with h2 测试
