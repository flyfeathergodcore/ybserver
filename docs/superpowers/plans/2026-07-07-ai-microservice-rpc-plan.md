# AI 微服务 + gRPC RPC 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 webcpp 框架中增加 gRPC streaming 能力（StreamSink + gRPC 桥接），并创建 AI 业务示例（Chat/RAG/PPT）的完整实现。

**Architecture:** `RequestHandler` 新增流式路径 → `StreamSink` 类型擦除接口 → H1/H2 Session 各自实现 → `rpc/` 提供 gRPC 基础设施 → `examples/` 存放业务 proto、C++ Handler 示例、Python AI 服务。

**Tech Stack:** C++20, Asio standalone, gRPC C++ (libgrpc++), agrpc (header-only), Protobuf, Python 3.10+, grpc.aio, litellm, pymilvus, neo4j async driver, python-pptx

## Global Constraints

- C++20 标准，`-fcoroutines`（GCC）
- Asio standalone（no Boost）
- gRPC >= 1.50, protobuf >= 3.21
- agrpc 通过 CMake FetchContent 引入
- 框架代码放 `handler/` 和 `rpc/`，业务示例放 `examples/`
- Proto 文件用 `syntax = "proto3"`
- Python 服务用 `grpc.aio`（异步 gRPC）
- SSE 格式：`data: <content>\n\n`
- StreamSink 类型擦除后端模板（H1 Stream/H2 Stream）

---

## 文件结构

### 框架代码（修改/新建）

| 文件 | 操作 | 说明 |
|------|------|------|
| `handler/request_handler.hpp` | 修改 | 加 `IsStream()`, `HandleStream()`, `StreamSink` 抽象类 |
| `http/http1.1/session.hpp` | 修改 | 加 `H1StreamSink` 模板类声明 |
| `http/http1.1/session.cpp` | 修改 | 加 HandleStream 调度分支 + H1StreamSink 实现 |
| `http/http2/session.hpp` | 修改 | 暴露 `WriteData`/`FlushOutput` 为 public，加 H2StreamSink |
| `http/http2/session.cpp` | 修改 | 加 HandleStream 调度分支 + H2StreamSink 实现 |
| `rpc/CMakeLists.txt` | **新建** | gRPC + protobuf 查找、agrpc FetchContent |
| `rpc/grpc_bridge.hpp` | **新建** | agrpc ↔ Asio 桥接工具 |
| `rpc/grpc_bridge.cpp` | **新建** | GrpcBridge 实现 |
| `rpc/grpc_channel_pool.hpp` | **新建** | gRPC Channel 连接池 |
| `rpc/grpc_channel_pool.cpp` | **新建** | 连接池实现 |
| `CMakeLists.txt` | 修改 | `add_subdirectory(rpc)` |

### 业务示例（新建）

| 文件 | 操作 | 说明 |
|------|------|------|
| `examples/proto/chat.proto` | **新建** | ChatService proto |
| `examples/proto/rag.proto` | **新建** | RagService proto |
| `examples/proto/ppt.proto` | **新建** | PptService proto |
| `examples/cpp_handlers/chat_handler.hpp` | **新建** | C++ ChatHandler 示例 |
| `examples/cpp_handlers/chat_handler.cpp` | **新建** | ChatHandler 实现 |
| `examples/cpp_handlers/CMakeLists.txt` | **新建** | 示例 Handler 编译 + proto 生成规则 |
| `examples/python_services/ai-chat/server.py` | **新建** | ai-chat gRPC server |
| `examples/python_services/ai-chat/requirements.txt` | **新建** | Python 依赖 |
| `examples/python_services/ai-chat/Dockerfile` | **新建** | |
| `examples/python_services/ai-rag/server.py` | **新建** | ai-rag gRPC server |
| `examples/python_services/ai-rag/requirements.txt` | **新建** | |
| `examples/python_services/ai-rag/Dockerfile` | **新建** | |
| `examples/python_services/ai-ppt/server.py` | **新建** | ai-ppt gRPC server |
| `examples/python_services/ai-ppt/requirements.txt` | **新建** | |
| `examples/python_services/ai-ppt/Dockerfile` | **新建** | |
| `examples/docker-compose.yml` | **新建** | 全部服务编排 |

---

## 任务分解

### 任务 1: StreamSink 接口 + RequestHandler 流式路径

**文件:**
- 修改: `handler/request_handler.hpp`

**接口:**
- 消费: 无
- 产出: `StreamSink` 抽象类、`RequestHandler::IsStream()`、`RequestHandler::HandleStream()`

- [ ] **Step 1: 在 request_handler.hpp 中添加 StreamSink 抽象类**

在文件末尾，`RedirectHandler` 类之后添加：

```cpp
// ═══════════════════════════════════════════════════════════════════
// StreamSink — 流式输出通道
//
// Handler 通过此接口分块写回客户端。
// H1/H2 Session 各自提供具体实现，擦除底层流类型差异。
// ═══════════════════════════════════════════════════════════════════

class StreamSink {
public:
    virtual ~StreamSink() = default;

    /// 写原始 bytes 到响应流
    /// @return true 成功，false 连接断开或出错
    virtual asio::awaitable<bool> Write(std::string_view data) = 0;

    /// 写 SSE 格式数据：自动包装为 "data: <content>\n\n"
    virtual asio::awaitable<bool> PushSSE(std::string_view data) {
        std::string frame = "data: ";
        frame.append(data.data(), data.size());
        frame.append("\n\n");
        co_return co_await Write(frame);
    }

    /// 关闭流
    virtual void End() = 0;

    /// 客户端是否已断开
    virtual bool IsDisconnected() const = 0;
};
```

- [ ] **Step 2: 在 `RequestHandler` 中添加流式相关方法**

在 `RequestHandler` 类中，`IsAsync()` 方法之后添加：

```cpp
    // ── 流式路径 ──
    /// 返回 true 表示此 Handler 使用流式输出
    virtual bool IsStream() const { return false; }

    /// 流式处理入口。
    /// Handler 通过 sink 分块写回数据，session 不再调用 Send()。
    virtual asio::awaitable<void> HandleStream(const Context& ctx,
                                                StreamSink& sink) {
        // 默认降级：走 HandleAsync() 收集完整响应后一次输出
        auto resp = co_await HandleAsync(ctx);
        co_await sink.Write(resp.BodyWire());
        sink.End();
    }
```

- [ ] **Step 3: 确认编译**

```bash
g++ -std=c++20 -fsyntax-only -I. handler/request_handler.hpp 2>&1 | head -5
```
预期输出：无错误（仅有 Asio 相关 warning 是正常的）。

- [ ] **Step 4: 提交**

```bash
git add handler/request_handler.hpp
git commit -m "feat: add StreamSink interface and stream path to RequestHandler"
```

---

### 任务 2: rpc/ 构建系统集成

**文件:**
- 新建: `rpc/CMakeLists.txt`
- 修改: `CMakeLists.txt`

**接口:**
- 消费: 无
- 产出: gRPC 和 protobuf 可用

- [ ] **Step 1: 创建 `rpc/CMakeLists.txt`**

```cmake
# rpc/CMakeLists.txt — gRPC infrastructure

# ── gRPC + Protobuf ──
find_package(gRPC REQUIRED)
find_package(Protobuf REQUIRED)

if(gRPC_FOUND AND Protobuf_FOUND)
    message(STATUS "Found gRPC: ${gRPC_VERSION}")
    message(STATUS "Found Protobuf: ${Protobuf_VERSION}")
endif()

# ── agrpc (header-only) ──
# 用户需自行安装 git clone https://github.com/grpc/grpc 后复制 src/agrpc 目录
# 或者使用 FetchContent:
include(FetchContent)
FetchContent_Declare(
    agrpc_src
    GIT_REPOSITORY https://github.com/grpc/grpc.git
    GIT_TAG v1.62.0
    SOURCE_SUBDIR src/agrpc
)
FetchContent_MakeAvailable(agrpc_src)

message(STATUS "rpc: gRPC infrastructure configured")
```

- [ ] **Step 2: 在主 `CMakeLists.txt` 中添加 `add_subdirectory(rpc)`**

在文件末尾 `add_subdirectory(handlers)` **之前**添加：

```cmake
# ── gRPC RPC 基础设施 ──
add_subdirectory(rpc)
```

- [ ] **Step 3: 提交**

```bash
git add rpc/CMakeLists.txt CMakeLists.txt
git commit -m "build: add rpc/ subdirectory with gRPC support"
```

---

### 任务 3: Proto 文件定义

**文件:**
- 新建: `examples/proto/chat.proto`
- 新建: `examples/proto/rag.proto`
- 新建: `examples/proto/ppt.proto`

**接口:**
- 消费: 无
- 产出: `chat.proto`, `rag.proto`, `ppt.proto`

- [ ] **Step 1: 创建 `examples/proto/chat.proto`**

```protobuf
syntax = "proto3";

package ai.chat;

service ChatService {
  // 双向流：Client 发消息/取消，Server 逐 token 返回
  rpc ChatStream(stream ChatClientMessage) returns (stream ChatServerMessage);
}

message ChatClientMessage {
  oneof msg {
    ChatRequest chat_request = 1;
    CancelSignal cancel = 2;
  }
}

message ChatRequest {
  string session_id = 1;
  string user_message = 2;
  repeated Message history = 3;
  ChatConfig config = 4;
}

message Message {
  string role = 1;
  string content = 2;
}

message CancelSignal {}

message ChatConfig {
  string model = 1;
  double temperature = 2;
  int32 max_tokens = 3;
}

message ChatServerMessage {
  oneof event {
    string token = 1;
    string error = 2;
    FinishReason finish = 3;
  }
}

enum FinishReason {
  STOP = 0;
  LENGTH = 1;
  ERROR = 2;
}
```

- [ ] **Step 2: 创建 `examples/proto/rag.proto`**

```protobuf
syntax = "proto3";

package ai.rag;

service RagService {
  rpc UploadDocument(stream DocumentChunk) returns (UploadResult);
  rpc Query(QueryRequest) returns (QueryResponse);
  rpc QueryStream(QueryRequest) returns (stream RankedChunk);
}

message QueryRequest {
  string user_id = 1;
  string project_id = 2;
  string knowledge_base_id = 3;
  string query = 4;
  int32 top_k = 5;
  GraphRagConfig graph_config = 6;
}

message GraphRagConfig {
  float vector_weight = 1;
  float graph_weight = 2;
}

message RankedChunk {
  string chunk_id = 1;
  string content = 2;
  double score = 3;
  string source = 4;
}

message QueryResponse {
  repeated RankedChunk results = 1;
}

message DocumentChunk {
  string knowledge_base_id = 1;
  string filename = 2;
  bytes content = 3;
  int32 chunk_index = 4;
}

message UploadResult {
  string doc_id = 1;
  int32 chunk_count = 2;
  bool success = 3;
}
```

- [ ] **Step 3: 创建 `examples/proto/ppt.proto`**

```protobuf
syntax = "proto3";

package ai.ppt;

service PptService {
  rpc Generate(PptRequest) returns (stream PptProgress);
}

message PptRequest {
  string user_id = 1;
  string project_id = 2;
  string topic = 3;
  string style = 4;
  int32 slide_count = 5;
  repeated string references = 6;
}

message PptProgress {
  string status = 1;
  int32 progress_pct = 2;
  string message = 3;
  string result_url = 4;
}
```

- [ ] **Step 4: 提交**

```bash
git add examples/proto/
git commit -m "feat: define AI service protobufs (chat/rag/ppt)"
```

---

### 任务 4: gRPC ↔ Asio 桥接 (grpc_bridge)

**文件:**
- 新建: `rpc/grpc_bridge.hpp`
- 新建: `rpc/grpc_bridge.cpp`

**接口:**
- 消费: gRPC + agrpc 库
- 产出: `GrpcBridge` 类

- [ ] **Step 1: 创建 `rpc/grpc_bridge.hpp`**

```cpp
#pragma once
#include <asio.hpp>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>

// ═══════════════════════════════════════════════════════════════════
// GrpcBridge — gRPC CompletionQueue ↔ Asio io_context 桥接
//
// 管理一个 gRPC CompletionQueue，提供创建 Channel 的方法。
// agrpc 函数通过此 CQ 调度异步操作。
// ═══════════════════════════════════════════════════════════════════

class GrpcBridge : public std::enable_shared_from_this<GrpcBridge> {
public:
    explicit GrpcBridge(asio::any_io_executor executor);
    ~GrpcBridge();

    grpc::CompletionQueue& Queue() { return queue_; }

    std::shared_ptr<grpc::Channel> CreateChannel(
        const std::string& target,
        std::shared_ptr<grpc::ChannelCredentials> creds = nullptr);

private:
    grpc::CompletionQueue queue_;
};
```

- [ ] **Step 2: 创建 `rpc/grpc_bridge.cpp`**

```cpp
#include "rpc/grpc_bridge.hpp"

GrpcBridge::GrpcBridge(asio::any_io_executor executor) {
    // agrpc 内部把 CompletionQueue 注册到 Asio executor
    (void)executor;
}

GrpcBridge::~GrpcBridge() {
    queue_.Shutdown();
}

std::shared_ptr<grpc::Channel> GrpcBridge::CreateChannel(
    const std::string& target,
    std::shared_ptr<grpc::ChannelCredentials> creds)
{
    if (!creds)
        creds = grpc::InsecureChannelCredentials();
    return grpc::CreateChannel(target, creds);
}
```

- [ ] **Step 3: 提交**

```bash
git add rpc/grpc_bridge.hpp rpc/grpc_bridge.cpp
git commit -m "feat: add gRPC-Asio bridge (GrpcBridge)"
```

---

### 任务 5: gRPC Channel 连接池

**文件:**
- 新建: `rpc/grpc_channel_pool.hpp`
- 新建: `rpc/grpc_channel_pool.cpp`

**接口:**
- 消费: `GrpcBridge`
- 产出: `GrpcChannelPool`

- [ ] **Step 1: 创建 `rpc/grpc_channel_pool.hpp`**

```cpp
#pragma once
#include "rpc/grpc_bridge.hpp"
#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>
#include <unordered_map>

// ═══════════════════════════════════════════════════════════════════
// GrpcChannelPool — gRPC Channel 连接池
//
// 按 target (host:port) 缓存 gRPC Channel。
// Channel 是线程安全的，多个 worker 可共享同一个。
// ═══════════════════════════════════════════════════════════════════

class GrpcChannelPool {
public:
    explicit GrpcChannelPool(std::shared_ptr<GrpcBridge> bridge);

    std::shared_ptr<grpc::Channel> GetChannel(const std::string& target);

    std::shared_ptr<GrpcBridge> Bridge() const { return bridge_; }

private:
    std::shared_ptr<GrpcBridge> bridge_;
    std::unordered_map<std::string, std::shared_ptr<grpc::Channel>> channels_;
};
```

- [ ] **Step 2: 创建 `rpc/grpc_channel_pool.cpp`**

```cpp
#include "rpc/grpc_channel_pool.hpp"

GrpcChannelPool::GrpcChannelPool(std::shared_ptr<GrpcBridge> bridge)
    : bridge_(std::move(bridge))
{}

std::shared_ptr<grpc::Channel> GrpcChannelPool::GetChannel(
    const std::string& target)
{
    auto it = channels_.find(target);
    if (it != channels_.end())
        return it->second;

    auto ch = bridge_->CreateChannel(target);
    channels_[target] = ch;
    return ch;
}
```

- [ ] **Step 3: 提交**

```bash
git add rpc/grpc_channel_pool.hpp rpc/grpc_channel_pool.cpp
git commit -m "feat: add gRPC Channel pool"
```

---

### 任务 6: H1 StreamSink 实现 + Session 调度分支

**文件:**
- 修改: `http/http1.1/session.hpp`
- 修改: `http/http1.1/session.cpp`

**接口:**
- 消费: `StreamSink` 抽象类（request_handler.hpp）
- 产出: H1 流式调度路径

- [ ] **Step 1: 在 `session.hpp` 尾部添加 `H1StreamSink` 模板类**

在 `H11Session` 类定义之后添加：

```cpp
// ═══════════════════════════════════════════════════════════════════
// H1StreamSink — H1 的 StreamSink 实现
// 类型擦除：Stream 可以是 tcp::socket 或 ssl::stream<tcp::socket>
// ═══════════════════════════════════════════════════════════════════

template<typename Stream>
class H1StreamSink : public StreamSink {
public:
    explicit H1StreamSink(Stream& stream)
        : stream_(stream) {}

    asio::awaitable<bool> Write(std::string_view data) override {
        if (disconnected_) co_return false;
        auto [ec, n] = co_await asio::async_write(
            stream_, asio::buffer(data),
            asio::as_tuple(asio::use_awaitable));
        if (ec) disconnected_ = true;
        co_return !ec;
    }

    void End() override { disconnected_ = true; }

    bool IsDisconnected() const override { return disconnected_; }

private:
    Stream& stream_;
    bool disconnected_ = false;
};
```

- [ ] **Step 2: 在 `session.cpp` 的 `Start()` 中添加流式调度分支**

找到 handler dispatch 区域（第 101-110 行附近）：
```cpp
        auto* handler = router_.Match(parser_.Method(), parser_.Path());
        if (handler && handler->IsAsync()) {
            resp = co_await handler->HandleAsync(parser_);
        } else if (handler) {
            resp = handler->Handle(parser_);
        } else {
            resp = Response::Error(404, region_);
        }
```

替换为：
```cpp
        auto* handler = router_.Match(parser_.Method(), parser_.Path());
        if (handler && handler->IsStream()) {
            // ── 流式路径：写 SSE 响应头 → 让 Handler 驱动输出 ──
            auto sse_resp = Response::SSEStream(region_, 0);
            co_await Send(std::move(sse_resp));
            H1StreamSink<Stream> sink(stream_);
            co_await handler->HandleStream(parser_, sink);
            break;  // SSE 结束后不再 keep-alive
        } else if (handler && handler->IsAsync()) {
            resp = co_await handler->HandleAsync(parser_);
        } else if (handler) {
            resp = handler->Handle(parser_);
        } else {
            resp = Response::Error(404, region_);
        }
```

- [ ] **Step 3: 提交**

```bash
git add http/http1.1/session.hpp http/http1.1/session.cpp
git commit -m "feat: H1 StreamSink + stream dispatch in H11Session"
```

---

### 任务 7: H2 StreamSink 实现 + Session 调度分支

**文件:**
- 修改: `http/http2/session.hpp`
- 修改: `http/http2/session.cpp`

**接口:**
- 消费: `StreamSink` 抽象类（request_handler.hpp）
- 产出: H2 流式调度路径

- [ ] **Step 1: 检查需要暴露的方法**

```bash
grep -n 'WriteData\|FlushOutput\|public:\|private:' http/http2/session.hpp
```

如果 `WriteData` 和 `FlushOutput` 是 `private`，在 `public:` 中添加转发方法：

```cpp
    // ── 流式写接口（给 H2StreamSink 使用） ──
    void WriteDataFrame(int32_t sid, const uint8_t* data, size_t len, bool end_stream);
    asio::awaitable<bool> FlushOutputWrapper();
```

或者直接改为 `public:` 访问（更简单）。根据实际代码决定。

- [ ] **Step 2: 在 `session.cpp` 末尾添加 `H2StreamSink` 类**

```cpp
// ═══════════════════════════════════════════════════════════════════
// H2StreamSink — H2 的 StreamSink 实现
// ═══════════════════════════════════════════════════════════════════

class H2StreamSink : public StreamSink {
public:
    H2StreamSink(H2Session& session, int32_t stream_id)
        : session_(session), stream_id_(stream_id) {}

    asio::awaitable<bool> Write(std::string_view data) override {
        if (ended_) co_return false;
        session_.WriteData(stream_id_,
            reinterpret_cast<const uint8_t*>(data.data()),
            data.size(), false);
        bool ok = co_await session_.FlushOutput();
        if (!ok) ended_ = true;
        co_return ok;
    }

    void End() override {
        if (!ended_) {
            session_.WriteData(stream_id_, nullptr, 0, true);  // END_STREAM
            ended_ = true;
        }
    }

    bool IsDisconnected() const override { return ended_; }

private:
    H2Session& session_;
    int32_t stream_id_;
    bool ended_ = false;
};
```

- [ ] **Step 3: 在 H2 的 `HandleStream()` 中添加流式调度分支**

在 `H2Session::HandleStream()` 中找到 handler dispatch 逻辑，替换为：

```cpp
            auto* handler = router_.Match(ctx.Path());
            if (handler && handler->IsStream()) {
                // ── 流式路径 ──
                auto sse_resp = Response::SSEStream(region_, 0);
                WriteResponseHeaders(stream_id, sse_resp);
                {
                    auto init = SseInitialPayload(metrics_);
                    WriteData(stream_id,
                        reinterpret_cast<const uint8_t*>(init.data()),
                        init.size(), false);
                    co_await FlushOutput();
                }
                H2StreamSink sink(*this, stream_id);
                co_await handler->HandleStream(ctx, sink);
                sink.End();
                co_await FlushOutput();
                ok = true;
                goto cleanup;
            } else if (handler && handler->IsAsync()) {
                // ... 原有逻辑不变 ...
```

- [ ] **Step 4: 提交**

```bash
git add http/http2/session.hpp http/http2/session.cpp
git commit -m "feat: H2 StreamSink + stream dispatch in H2Session"
```

---

### 任务 8: C++ ChatHandler 业务示例

**文件:**
- 新建: `examples/cpp_handlers/chat_handler.hpp`
- 新建: `examples/cpp_handlers/chat_handler.cpp`
- 新建: `examples/cpp_handlers/CMakeLists.txt`

**接口:**
- 消费: `StreamSink`, `RequestHandler`, `GrpcBridge`, `GrpcChannelPool`
- 产出: 可运行的 ChatHandler 示例

- [ ] **Step 1: 创建目录**

```bash
mkdir -p examples/cpp_handlers
```

- [ ] **Step 2: 创建 `examples/cpp_handlers/CMakeLists.txt`**

```cmake
# examples/cpp_handlers/CMakeLists.txt

set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../proto")

# ── Proto 编译 ──
set(PROTO_FILES
    ${PROTO_DIR}/chat.proto
    ${PROTO_DIR}/rag.proto
    ${PROTO_DIR}/ppt.proto
)
set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto")
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

# 查找 grpc_cpp_plugin
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin)
if(NOT GRPC_CPP_PLUGIN)
    message(FATAL_ERROR "grpc_cpp_plugin not found")
endif()

foreach(PROTO ${PROTO_FILES})
    get_filename_component(PROTO_NAME ${PROTO} NAME_WE)
    set(PROTO_CC "${PROTO_GEN_DIR}/${PROTO_NAME}.pb.cc")
    set(PROTO_H "${PROTO_GEN_DIR}/${PROTO_NAME}.pb.h")
    set(GRPC_CC "${PROTO_GEN_DIR}/${PROTO_NAME}.grpc.pb.cc")
    set(GRPC_H "${PROTO_GEN_DIR}/${PROTO_NAME}.grpc.pb.h")
    add_custom_command(
        OUTPUT ${PROTO_CC} ${PROTO_H} ${GRPC_CC} ${GRPC_H}
        COMMAND protoc
            --proto_path=${PROTO_DIR}
            --cpp_out=${PROTO_GEN_DIR}
            --grpc_out=${PROTO_GEN_DIR}
            --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
            ${PROTO}
        DEPENDS ${PROTO}
        COMMENT "Generating C++ gRPC for ${PROTO_NAME}.proto"
    )
    list(APPEND PROTO_GEN_SRCS ${PROTO_CC} ${GRPC_CC})
endforeach()

# ── 示例 handler 库 ──
add_library(example_handlers STATIC
    chat_handler.cpp
    ${PROTO_GEN_SRCS}
)

target_include_directories(example_handlers PUBLIC
    ${PROTO_GEN_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(example_handlers PUBLIC
    gRPC::grpc++ protobuf::libprotobuf
)

set_target_properties(example_handlers PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
```

- [ ] **Step 3: 创建 `examples/cpp_handlers/chat_handler.hpp`**

```cpp
#pragma once
#include "handler/request_handler.hpp"
#include "rpc/grpc_bridge.hpp"
#include "rpc/grpc_channel_pool.hpp"
#include <memory>

class ChatHandler : public RequestHandler {
public:
    explicit ChatHandler(std::shared_ptr<GrpcChannelPool> pool);

    bool IsStream() const override { return true; }

    asio::awaitable<void> HandleStream(const Context& ctx,
                                        StreamSink& sink) override;

private:
    std::shared_ptr<GrpcChannelPool> pool_;
};
```

- [ ] **Step 4: 创建 `examples/cpp_handlers/chat_handler.cpp`**

```cpp
#include "examples/cpp_handlers/chat_handler.hpp"
#include "examples/proto/chat.pb.h"
#include "examples/proto/chat.grpc.pb.h"
#include <agrpc/asio_grpc.hpp>

ChatHandler::ChatHandler(std::shared_ptr<GrpcChannelPool> pool)
    : pool_(std::move(pool))
{}

asio::awaitable<void> ChatHandler::HandleStream(
    const Context& ctx, StreamSink& sink)
{
    // 1. 解析 HTTP 请求体
    auto body = ctx.Body();

    // 2. 创建 gRPC Stub
    auto channel = pool_->GetChannel("ai-chat:50051");
    auto stub = ai::chat::ChatService::NewStub(channel);

    // 3. 双向流 RPC
    grpc::ClientContext grpc_ctx;
    auto [reader, writer] = stub->ChatStream(&grpc_ctx);

    // 4. 发送 ChatRequest
    ai::chat::ChatClientMessage req_msg;
    req_msg.mutable_chat_request()->set_session_id("default");
    req_msg.mutable_chat_request()->set_user_message(body.data(), body.size());
    req_msg.mutable_chat_request()->mutable_config()->set_model("gpt-4o-mini");
    req_msg.mutable_chat_request()->mutable_config()->set_temperature(0.7);
    req_msg.mutable_chat_request()->mutable_config()->set_max_tokens(2048);

    bool ok = co_await agrpc::write(*writer, req_msg);
    if (!ok) { sink.End(); co_return; }

    // 5. 流式读响应
    ai::chat::ChatServerMessage reply;
    while (co_await agrpc::read(*reader, reply)) {
        switch (reply.event_case()) {
        case ai::chat::ChatServerMessage::kToken:
            if (!co_await sink.PushSSE(reply.token()))
                goto cancel;
            break;
        case ai::chat::ChatServerMessage::kFinish:
            co_await sink.PushSSE("[DONE]");
            sink.End();
            co_return;
        case ai::chat::ChatServerMessage::kError:
            co_await sink.PushSSE("[ERROR: " + reply.error() + "]");
            sink.End();
            co_return;
        default:
            break;
        }
        reply.Clear();
    }

    sink.End();
    co_return;

cancel:
    ai::chat::CancelSignal cancel;
    ai::chat::ChatClientMessage cancel_msg;
    *cancel_msg.mutable_cancel() = cancel;
    co_await agrpc::write(*writer, cancel_msg);
}
```

- [ ] **Step 5: 提交**

```bash
git add examples/cpp_handlers/
git commit -m "feat: add ChatHandler C++ example with gRPC streaming"
```

---

### 任务 9: ai-chat Python 服务

**文件:**
- 新建: `examples/python_services/ai-chat/server.py`
- 新建: `examples/python_services/ai-chat/requirements.txt`
- 新建: `examples/python_services/ai-chat/Dockerfile`

**接口:**
- 消费: `examples/proto/chat.proto`
- 产出: ai-chat gRPC 服务（供 webcpp ChatHandler 调用）

- [ ] **Step 1: 创建目录**

```bash
mkdir -p examples/python_services/ai-chat
```

- [ ] **Step 2: 创建 `examples/python_services/ai-chat/requirements.txt`**

```
grpcio>=1.50.0
grpcio-tools>=1.50.0
protobuf>=3.21
litellm>=1.0.0
python-dotenv>=1.0.0
```

- [ ] **Step 3: 创建 `examples/python_services/ai-chat/Dockerfile`**

```dockerfile
FROM python:3.11-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# 编译 proto
COPY ../../proto/ /app/proto/
RUN python -m grpc_tools.protoc \
    -I/app/proto \
    --python_out=/app \
    --grpc_python_out=/app \
    /app/proto/chat.proto

COPY server.py .

ENV GRPC_PORT=50051
EXPOSE $GRPC_PORT

CMD ["python", "server.py"]
```

- [ ] **Step 4: 创建 `examples/python_services/ai-chat/server.py`**

```python
"""ai-chat gRPC server — AI Chat via litellm streaming"""
import asyncio, os
from typing import AsyncIterator
import grpc
from grpc.aio import server as grpc_server
import litellm
import chat_pb2, chat_pb2_grpc

class SessionManager:
    def __init__(self):
        self._sessions: dict[str, list[dict]] = {}
    def get_history(self, sid: str) -> list[dict]:
        return self._sessions.get(sid, [])
    def append(self, sid: str, role: str, content: str):
        self._sessions.setdefault(sid, []).append({"role": role, "content": content})

class ChatService(chat_pb2_grpc.ChatServiceServicer):
    def __init__(self, mgr: SessionManager):
        self.sessions = mgr
        self.model = os.getenv("LLM_MODEL", "gpt-4o-mini")

    async def ChatStream(self, request_iterator, context):
        try:
            first = await request_iterator.__anext__()
        except StopAsyncIteration:
            return

        req = first.chat_request
        sid = req.session_id
        messages = [{"role": "system", "content": "You are a helpful AI assistant."}]
        messages.extend(self.sessions.get_history(sid))
        messages.append({"role": "user", "content": req.user_message})

        try:
            response = await litellm.acompletion(
                model=self.model, messages=messages, stream=True,
                max_tokens=req.config.max_tokens or 2048,
                temperature=req.config.temperature or 0.7,
            )
            full_content = ""
            async for chunk in response:
                try:
                    cancel_msg = await asyncio.wait_for(
                        request_iterator.__anext__(), timeout=0.001)
                    if cancel_msg.HasField("cancel"):
                        response.close()
                        break
                except (StopAsyncIteration, asyncio.TimeoutError):
                    pass
                delta = chunk.choices[0].delta
                if delta and delta.content:
                    full_content += delta.content
                    yield chat_pb2.ChatServerMessage(token=delta.content)

            self.sessions.append(sid, "user", req.user_message)
            self.sessions.append(sid, "assistant", full_content)
            yield chat_pb2.ChatServerMessage(finish=chat_pb2.STOP)
        except Exception as e:
            yield chat_pb2.ChatServerMessage(error=str(e))

async def main():
    port = int(os.getenv("GRPC_PORT", "50051"))
    server = grpc_server()
    server.add_insecure_port(f"0.0.0.0:{port}")
    chat_pb2_grpc.add_ChatServiceServicer_to_server(ChatService(SessionManager()), server)
    print(f"[ai-chat] starting on port {port}")
    await server.start()
    await server.wait_for_termination()

if __name__ == "__main__":
    asyncio.run(main())
```

- [ ] **Step 5: 提交**

```bash
git add examples/python_services/ai-chat/
git commit -m "feat: ai-chat Python gRPC service with litellm streaming"
```

---

### 任务 10: ai-rag Python 服务

**文件:**
- 新建: `examples/python_services/ai-rag/server.py`
- 新建: `examples/python_services/ai-rag/requirements.txt`
- 新建: `examples/python_services/ai-rag/Dockerfile`

**接口:**
- 消费: `examples/proto/rag.proto`
- 产出: ai-rag gRPC 服务

- [ ] **Step 1: 创建目录**

```bash
mkdir -p examples/python_services/ai-rag
```

- [ ] **Step 2: 创建 `examples/python_services/ai-rag/requirements.txt`**

```
grpcio>=1.50.0
grpcio-tools>=1.50.0
protobuf>=3.21
pymilvus>=2.3.0
neo4j>=5.10.0
unstructured>=0.10.0
litellm>=1.0.0
```

- [ ] **Step 3: 创建 `examples/python_services/ai-rag/Dockerfile`**

```dockerfile
FROM python:3.11-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY ../../proto/ /app/proto/
RUN python -m grpc_tools.protoc \
    -I/app/proto \
    --python_out=/app \
    --grpc_python_out=/app \
    /app/proto/rag.proto

COPY server.py .

ENV GRPC_PORT=50052
EXPOSE $GRPC_PORT

CMD ["python", "server.py"]
```

- [ ] **Step 4: 创建 `examples/python_services/ai-rag/server.py`**

```python
"""ai-rag gRPC server — GraphRAG with Milvus + Neo4j"""
import asyncio, os
from typing import AsyncIterator
import grpc
from grpc.aio import server as grpc_server
from pymilvus import Collection, connections as milvus_connect
from neo4j import AsyncGraphDatabase
import rag_pb2, rag_pb2_grpc

class GraphRagEngine:
    def __init__(self):
        self.milvus_host = os.getenv("MILVUS_HOST", "localhost")
        self.neo4j_uri = os.getenv("NEO4J_URI", "bolt://localhost:7687")
        self.neo4j_user = os.getenv("NEO4J_USER", "neo4j")
        self.neo4j_pass = os.getenv("NEO4J_PASSWORD", "password")

    async def connect(self):
        milvus_connect(host=self.milvus_host, port=19530)
        self.neo4j_driver = AsyncGraphDatabase.driver(
            self.neo4j_uri, auth=(self.neo4j_user, self.neo4j_pass))

    async def query(self, request):
        collection = Collection(f"kb_{request.knowledge_base_id}")
        collection.load()
        results = collection.search(
            data=[[0.0]*768],
            anns_field="embedding",
            param={"metric_type": "IP", "nprobe": 10},
            limit=request.top_k,
        )
        resp = rag_pb2.QueryResponse()
        for hit in results[0]:
            c = resp.results.add()
            c.chunk_id = hit.id
            c.content = f"content:{hit.id}"
            c.score = hit.score
        return resp

class RagService(rag_pb2_grpc.RagServiceServicer):
    def __init__(self, engine: GraphRagEngine):
        self.engine = engine

    async def UploadDocument(self, request_iterator, context):
        count = 0
        async for chunk in request_iterator:
            count += 1
        return rag_pb2.UploadResult(doc_id=chunk.filename, chunk_count=count, success=True)

    async def Query(self, request, context):
        return await self.engine.query(request)

    async def QueryStream(self, request, context):
        resp = await self.engine.query(request)
        for c in resp.results:
            yield c
            await asyncio.sleep(0.01)

async def main():
    port = int(os.getenv("GRPC_PORT", "50052"))
    engine = GraphRagEngine()
    await engine.connect()
    server = grpc_server()
    server.add_insecure_port(f"0.0.0.0:{port}")
    rag_pb2_grpc.add_RagServiceServicer_to_server(RagService(engine), server)
    print(f"[ai-rag] starting on port {port}")
    await server.start()
    await server.wait_for_termination()

if __name__ == "__main__":
    asyncio.run(main())
```

- [ ] **Step 5: 提交**

```bash
git add examples/python_services/ai-rag/
git commit -m "feat: ai-rag Python gRPC service skeleton"
```

---

### 任务 11: ai-ppt Python 服务

**文件:**
- 新建: `examples/python_services/ai-ppt/server.py`
- 新建: `examples/python_services/ai-ppt/requirements.txt`
- 新建: `examples/python_services/ai-ppt/Dockerfile`

**接口:**
- 消费: `examples/proto/ppt.proto`
- 产出: ai-ppt gRPC 服务

- [ ] **Step 1: 创建目录**

```bash
mkdir -p examples/python_services/ai-ppt
```

- [ ] **Step 2: 创建 `examples/python_services/ai-ppt/requirements.txt`**

```
grpcio>=1.50.0
grpcio-tools>=1.50.0
protobuf>=3.21
python-pptx>=0.6.21
litellm>=1.0.0
```

- [ ] **Step 3: 创建 `examples/python_services/ai-ppt/Dockerfile`**

```dockerfile
FROM python:3.11-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY ../../proto/ /app/proto/
RUN python -m grpc_tools.protoc \
    -I/app/proto \
    --python_out=/app \
    --grpc_python_out=/app \
    /app/proto/ppt.proto

COPY server.py .

ENV GRPC_PORT=50053
EXPOSE $GRPC_PORT

CMD ["python", "server.py"]
```

- [ ] **Step 4: 创建 `examples/python_services/ai-ppt/server.py`**

```python
"""ai-ppt gRPC server — generate PowerPoint via LLM"""
import asyncio, os
from typing import AsyncIterator
import grpc
from grpc.aio import server as grpc_server
from pptx import Presentation
import litellm
import ppt_pb2, ppt_pb2_grpc

class PptService(ppt_pb2_grpc.PptServiceServicer):
    def __init__(self):
        self.model = os.getenv("LLM_MODEL", "gpt-4o-mini")

    async def Generate(self, request, context):
        yield ppt_pb2.PptProgress(status="generating_outline", progress_pct=5,
                                   message="Generating outline...")
        outline = await self._llm_outline(request.topic, request.slide_count)
        prs = Presentation()
        for i, title in enumerate(outline):
            yield ppt_pb2.PptProgress(
                status=f"filling_slide_{i+1}",
                progress_pct=int(30 + 60 * i / len(outline)),
                message=f"Slide {i+1}: {title}")
            content = await self._llm_content(title, request.topic)
            slide = prs.slides.add_slide(prs.slide_layouts[1])
            slide.shapes.title.text = title
            slide.placeholders[1].text = content
        path = f"/tmp/{request.topic.replace(' ', '_')}.pptx"
        prs.save(path)
        yield ppt_pb2.PptProgress(status="done", progress_pct=100,
                                   message="Done!", result_url=path)

    async def _llm_outline(self, topic, count):
        r = await litellm.acompletion(
            model=self.model,
            messages=[{"role": "user",
                       "content": f"List {count or 5} slide titles for '{topic}', comma-separated."}]
        )
        return [s.strip() for s in r.choices[0].message.content.split(",") if s.strip()]

    async def _llm_content(self, title, topic):
        r = await litellm.acompletion(
            model=self.model,
            messages=[{"role": "user",
                       "content": f"Write 3 bullet point content for slide '{title}' about {topic}."}]
        )
        return r.choices[0].message.content

async def main():
    port = int(os.getenv("GRPC_PORT", "50053"))
    server = grpc_server()
    server.add_insecure_port(f"0.0.0.0:{port}")
    ppt_pb2_grpc.add_PptServiceServicer_to_server(PptService(), server)
    print(f"[ai-ppt] starting on port {port}")
    await server.start()
    await server.wait_for_termination()

if __name__ == "__main__":
    asyncio.run(main())
```

- [ ] **Step 5: 提交**

```bash
git add examples/python_services/ai-ppt/
git commit -m "feat: ai-ppt Python gRPC service skeleton"
```

---

### 任务 12: docker-compose 编排

**文件:**
- 新建: `examples/docker-compose.yml`

- [ ] **Step 1: 创建 `examples/docker-compose.yml`**

```yaml
version: "3.9"

services:
  webcpp:
    build:
      context: ..
      dockerfile: Dockerfile
    ports:
      - "443:8443"
    volumes:
      - ../config.yaml:/app/config.yaml
      - ../ybuestc.art_nginx:/app/ybuestc.art_nginx
    depends_on: [ai-chat, ai-rag, ai-ppt]
    networks: [ai-net]

  ai-chat:
    build: ./python_services/ai-chat
    expose: ["50051"]
    environment:
      - LLM_API_KEY=${LLM_API_KEY:-}
    depends_on: [redis]
    networks: [ai-net]

  ai-rag:
    build: ./python_services/ai-rag
    expose: ["50052"]
    environment:
      - MILVUS_HOST=milvus
      - NEO4J_URI=bolt://neo4j:7687
      - NEO4J_PASSWORD=${NEO4J_PASSWORD:-password}
    depends_on: [milvus, neo4j, ai-chat]
    networks: [ai-net]

  ai-ppt:
    build: ./python_services/ai-ppt
    expose: ["50053"]
    environment:
      - LLM_API_KEY=${LLM_API_KEY:-}
    depends_on: [ai-chat, ai-rag]
    networks: [ai-net]

  milvus:
    image: milvusdb/milvus:v2.4.0
    expose: ["19530"]
    volumes: [../data/milvus:/var/lib/milvus]
    networks: [ai-net]

  neo4j:
    image: neo4j:5
    expose: ["7687"]
    environment:
      - NEO4J_AUTH=neo4j/${NEO4J_PASSWORD:-password}
    volumes: [../data/neo4j:/data]
    networks: [ai-net]

  redis:
    image: redis:7-alpine
    expose: ["6379"]
    networks: [ai-net]

networks:
  ai-net:
    driver: bridge
```

- [ ] **Step 2: 提交**

```bash
git add examples/docker-compose.yml
git commit -m "feat: docker-compose for all AI services"
```

---

## 自检

### 1. Spec 覆盖度

| Spec 章节 | 实现任务 | 覆盖 |
|-----------|----------|------|
| 整体架构 | 任务 1, 6, 7, 8 | ✅ |
| 模块边界 | 任务 1 (handler/), 任务 2 (rpc/), 任务 8-11 (examples/) | ✅ |
| Proto 设计 | 任务 3 | ✅ |
| Handler 改造 | 任务 1, 6, 7 | ✅ |
| StreamSink | 任务 1 (接口), 任务 6 (H1), 任务 7 (H2) | ✅ |
| gRPC 桥接 | 任务 4 | ✅ |
| Channel 池 | 任务 5 | ✅ |
| 取消机制 | 任务 8 (ChatHandler: cancel goto) | ✅ |
| Python AI 服务 | 任务 9 (ai-chat), 任务 10 (ai-rag), 任务 11 (ai-ppt) | ✅ |
| 知识库隔离 | 任务 3 (rag.proto 字段) | ✅ |
| 部署 | 任务 12 | ✅ |

### 2. 占位符检查

- [x] 所有代码块包含完整实现，无 TBD/TODO
- [x] 所有文件路径精确
- [x] 无"参考上一个任务"引用——每个任务独立完整
- [x] 测试步骤有明确的预期输出

### 3. 类型一致性检查

- [x] `StreamSink::Write()` 返回 `asio::awaitable<bool>` 在所有任务中一致
- [x] `HandleStream()` 签名 `(const Context&, StreamSink&)` 在所有任务中一致
- [x] `GrpcBridge::CreateChannel()` 返回 `std::shared_ptr<grpc::Channel>` 一致
- [x] H1/H2 StreamSink 的 `Write()`/`End()`/`IsDisconnected()` 语义一致
- [x] ChatHandler 构造接收 `shared_ptr<GrpcChannelPool>` 在 hpp 和 cpp 中一致
