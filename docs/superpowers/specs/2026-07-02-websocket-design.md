# WebSocket 支持设计

## 概述

为 webcpp 服务器添加 WebSocket (RFC 6455) 支持，包括：

1. **本地终结** — handler 可以注册为 WebSocket 端点，接收和发送帧
2. **反向代理透传** — ReverseProxy 检测 Upgrade: websocket 并双向转发帧

目标：H1 优先，H2 WebSocket (RFC 8441) 延后。

## 架构

WebSocket 在请求链路中的位置：

```
Client → TLS → H1Session::Start()
                    │
                    ├─ read → parse → middleware.Pre()
                    │
                    ├─ Upgrade: websocket 且 handler 支持 WS？
                    │     → Session 计算 Sec-WebSocket-Accept
                    │     → 发送 101 Switching Protocols
                    │     → WsConnection conn(stream_)
                    │     → co_await handler->HandleWebSocket(conn)
                    │     → conn 关闭后 break
                    │
                    └─ 普通请求
                          → handler.Handle()
                          → Send() → Post()
```

### 分层职责

| 层 | 文件 | 职责 |
|----|------|------|
| 帧编解码 | ws_frame | ReadFrame / WriteFrame 异步函数，mask/unmask |
| 连接状态 | ws_connection | WsConnection：Read/Send/Close + 自动 ping/pong/close 应答 |
| 帧中继 | ws_relay | 双向帧转发（反向代理透传用） |
| 握手 + 调度 | session.cpp | 101 握手计算 + 帧循环入口 |
| Handler 扩展 | request_handler.hpp | HandleWebSocket() 虚方法（默认空） |
| 代理扩展 | reverse_proxy.cpp | Upgrade 检测 + 帧 relay |

## Handler 接口

```cpp
class RequestHandler {
    virtual Response Handle(const Context& ctx) = 0;

    /// WebSocket 回调 — handler 覆写此方法接管帧循环
    /// session 在发送 101 后调用此方法，handler 在其中使用 conn 收发帧。
    /// 方法返回后 session 关闭连接。
    virtual asio::awaitable<void> HandleWebSocket(WsConnection& conn) {
        co_return;  // 默认空实现，不支持 WS 的 handler 无需改动
    }
};
```

Handler 示例：

```cpp
class EchoWsHandler : public RequestHandler {
    Response Handle(const Context&) override {
        return Response::Error(400, *ctx.Pool());  // 不会被调用
    }
    asio::awaitable<void> HandleWebSocket(WsConnection& conn) override {
        while (conn.IsOpen()) {
            auto frame = co_await conn.Read();
            if (frame.opcode == WsOpcode::Close) break;
            co_await conn.Send(frame.opcode, std::move(frame.payload));
        }
    }
};
```

## 帧层 (ws_frame)

WebSocket 帧格式 (RFC 6455 §5.2)：

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-------+-+-------------+-------------------------------+
|F|R|R|R| opcode|M| Payload len | Extended payload len (16/64) |
|I|S|S|S|       |A|     (7)     |  (if len=126 or 127)         |
+-+-+-+-+-+-+-+---------+-------------------+-------------------+
:                     Payload Data (unmasked for server→client) :
+---------------------------------------------------------------+
```

```cpp
// net/ws_frame.hpp

enum class WsOpcode : uint8_t {
    Continuation = 0x0,
    Text         = 0x1,
    Binary       = 0x2,
    Close        = 0x8,
    Ping         = 0x9,
    Pong         = 0xA,
};

struct WsFrame {
    bool fin = true;
    WsOpcode opcode = WsOpcode::Binary;
    std::string payload;
};

// 异步读一帧（自动 mask/unmask，client→server 的帧被 unmask）
template<typename Stream>
asio::awaitable<WsFrame> ReadFrame(Stream& stream);

// 异步写一帧（server→client 不 mask）
template<typename Stream>
asio::awaitable<void> WriteFrame(Stream& stream, WsOpcode opcode,
                                  std::string payload, bool fin = true);
```

### 实现要点

- 先读 2 字节首部，根据 length 字段决定是否有 extended length
- Client→Server 帧有 Masking-Key（4 字节），读完后 unmask payload
- 帧读写用 `co_await stream_.async_read_some()` / `async_write()` 实现
- payload 大小上限 64KB（超长帧可以考虑流式处理，初始版本直接读取完整 payload）

## 连接状态 (ws_connection)

```cpp
// net/ws_connection.hpp

class WsConnection {
public:
    /// 读一帧（ping/pong 自动应答，close 自动回复）
    asio::awaitable<WsFrame> Read();

    /// 发送一帧
    asio::awaitable<void> Send(WsOpcode opcode, std::string payload);

    /// 发起 close 握手
    asio::awaitable<void> Close(uint16_t code = 1000,
                                std::string_view reason = {});

    bool IsOpen() const { return !closed_; }

private:
    Stream& stream_;
    bool closed_ = false;
};
```

`Read()` 内部对 handler 透明的处理：

| 收到的 opcode | 行为 |
|---------------|------|
| Text / Binary | 返回给 handler |
| Ping | 自动回复 Pong，继续读下一帧 |
| Pong | 忽略，继续读下一帧（ping 的应答由框架内部匹配） |
| Close | 发送 Close 应答，标记 closed_ = true，不返回给 handler |

## Session 集成

### 101 握手

Session 在收到 Upgrade: websocket 请求后：

1. 路由匹配 handler
2. 如果 handler 没有覆写 `HandleWebSocket()`（基类默认空），走正常 HTTP 路径
3. 如果 handler 覆写了，session 计算 `Sec-WebSocket-Accept`：
   ```
   accept = Base64(SHA1(key + "258EAFA5-E914-47DA-95CA-5AB9DC11B85B"))
   ```
4. 构造 101 响应并发送
5. 创建 `WsConnection` 并调用 `handler->HandleWebSocket(conn)`
6. `HandleWebSocket` 返回后关闭连接

### H1 主循环改动

```cpp
// Before: session.cpp 中 Route → Handler 段
if (parser_.Header("upgrade") == "websocket") {
    auto* handler = router_.Match(parser_.Method(), parser_.Path());
    if (handler) {
        auto key = parser_.Header("sec-websocket-key");
        if (!key.empty()) {
            // 计算 accept
            auto accept = ComputeAccept(key);
            // 构造 101 响应
            std::string handshake = Build101Response(accept);
            co_await async_write(stream_, asio::buffer(handshake), use_awaitable);

            Region().Reset();  // 释放 HTTP 请求的内存
            WsConnection conn(stream_);
            co_await handler->HandleWebSocket(conn);
            break;  // WS 连接不再处理 HTTP 请求
        }
    }
    // 握手失败 → 400
    co_await Send(Response::Error(400, region_));
    break;
}
```

## 反向代理透传 (ws_relay)

反向代理 `ReverseProxy::HandleWebSocket()`：

```
1. 将客户端 Upgrade 请求转发到上游（保留 Upgrade/Connection/WS 头）
2. 读取上游的 101 响应 → 转发给客户端
3. 进入双向帧 relay：

   co_await asio::co_spawn(exec,
       WsRelayBidirectional(client_sock, upstream_sock), detached);
   co_await WsRelayBidirectional(upstream_sock, client_sock);
   // 任一方向关闭 → 取消另一方向
```

`WsRelayBidirectional` 实现：

```cpp
asio::awaitable<void> WsRelayBidirectional(
    Stream& from, Stream& to, bool mask_to)
{
    while (true) {
        auto frame = co_await ReadFrame(from);
        if (frame.opcode == WsOpcode::Close) {
            co_await WriteFrame(to, WsOpcode::Close, frame.payload);
            break;
        }
        co_await WriteFrame(to, frame.opcode, std::move(frame.payload),
                            true, mask_to);
    }
}
```

客户端→上游方向 mask_to=true（代理作为客户端向上游发帧），上游→客户端方向 mask_to=false。

## Response 扩展

```cpp
// net/response.hpp
class Response {
    static Response WebSocketUpgrade(SessionRegion& region,
                                      std::string accept);
    bool IsWebSocket() const { return ws_upgrade_; }
    std::string_view WsAccept() const { return ws_accept_; }
private:
    bool ws_upgrade_ = false;
    std::string ws_accept_;  // Sec-WebSocket-Accept 值
};
```

## 文件清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `net/ws_frame.hpp` | 新建 | WsFrame 结构体 + ReadFrame/WriteFrame 声明 |
| `net/ws_frame.cpp` | 新建 | 帧编解码实现 |
| `net/ws_connection.hpp` | 新建 | WsConnection 类 |
| `net/ws_connection.cpp` | 新建 | 连接状态 + 自动 ping/pong/close |
| `net/ws_relay.hpp` | 新建 | 双向帧 relay |
| `net/ws_relay.cpp` | 新建 | relay 实现 |
| `net/response.hpp` | 修改 | + WebSocketUpgrade() + IsWebSocket() |
| `net/response.cpp` | 修改 | + case 101 处理 |
| `http/http1.1/session.cpp` | 修改 | + Upgrade 检测 + 101 握手 + 帧循环 |
| `handler/request_handler.hpp` | 修改 | + HandleWebSocket() 虚方法 |
| `handler/reverse_proxy.hpp` | 修改 | + HandleWebSocket() 覆写 |
| `handler/reverse_proxy.cpp` | 修改 | + Upgrade 检测 + 帧 relay |
| `config/config.hpp` | 不修改 | WS 路由通过已有 Add/AddRoute 注册 |
| `CMakeLists.txt` | 修改 | + ws_frame/ws_connection/ws_relay |

## 范围边界

### 第一期（H1 only）

- H1 WebSocket 本地终结 ✓
- H1 WebSocket 反向代理透传 ✓
- 单帧 payload ≤ 64KB（初始版本全量读取，后续可流式化）

### 延后（不在本次范围）

- H2 WebSocket (RFC 8441 Extended CONNECT)
- H2 反向代理透传
- 分片帧重组（FIN=0 + Continuation）
- 超大帧流式处理（>64KB 分多次 read）
- Per-message deflate (RFC 7692)
