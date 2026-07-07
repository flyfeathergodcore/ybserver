---
title: AI 业务微服务 + gRPC RPC 设计
date: 2026-07-07
status: draft
---

# AI 业务微服务 + gRPC RPC 设计

## 1. 背景与目标

webcpp 是一个基于 C++20 协程 + Asio 的高性能 HTTP(S) 服务器框架，已完成网络层骨架（H1/H2、TLS、反向代理、WebSocket、中间件、指标看板）。需要在框架之上承载 AI 业务（AI 对话、RAG 知识库、AI PPT 生成），并采用**微服务 + gRPC RPC** 的方式实现业务逻辑隔离。

### 核心设计目标

- **隔离**：AI 业务逻辑与网络框架解耦，独立开发、独立部署、独立扩缩容
- **流式**：AI 对话需要逐 token 流式返回，全链路异步非阻塞
- **统一入口**：webcpp 是唯一的对外接口，所有客户端请求经过 webcpp
- **生态优先**：AI 服务使用 Python（LLM/向量/图数据库生态最成熟）

## 2. 整体架构

```
┌──────────────────────────────────────────────────────────────────┐
│                        Client (浏览器/移动端)                     │
│                    HTTPS / SSE (流式对话)                        │
└────────────────────────────┬─────────────────────────────────────┘
                             │
                             ▼
┌──────────────────────────────────────────────────────────────────┐
│                   webcpp (C++20, Asio 协程)                      │
│                                                                  │
│  中间件链 (鉴权/限流/日志) → Router → RequestHandler              │
│                                          │                      │
│                                   流式 Handler                   │
│                                   (co_await gRPC)               │
│                                          │                      │
│   rpc/grpc_bridge  (agrpc ↔ Asio 桥接)   │                      │
│   rpc/grpc_channel_pool                  │                      │
└────────────────────────────┬─────────────────────────────────────┘
                             │ gRPC (Bidirectional / Server Streaming)
                             ▼
┌──────────────────────────────────────────────────────────────────┐
│                Python AI Services (独立部署)                      │
│                                                                  │
│  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────┐   │
│  │   ai-chat        │  │   ai-rag         │  │   ai-ppt     │   │
│  │   grpc.aio       │◄─│   grpc.aio       │◄─│  grpc.aio    │   │
│  │   litellm        │  │   pymilvus       │  │  python-pptx │   │
│  │   (LLM 路由)     │  │   neo4j driver   │  │  litellm     │   │
│  └──────────────────┘  └──────────────────┘  └──────────────┘   │
│         │                      │                      │          │
│         └──────────┬───────────┴──────────┬───────────┘          │
│                    ▼                      ▼                      │
│              ai-chat gRPC            ai-rag gRPC                 │
│              (被 RAG/PPT 调用)        (被 PPT 调用)              │
└──────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────┐
│                    基础设施                                       │
│  ┌─────────┐  ┌──────────┐  ┌──────────┐  ┌─────────────────┐   │
│  │ Neo4j   │  │ Milvus   │  │ Redis    │  │ (未来: 对象存储)  │   │
│  │ 图数据库 │  │ 向量数据库 │  │ 会话缓存  │  │ PPT/文档存储    │   │
│  └─────────┘  └──────────┘  └──────────┘  └─────────────────┘   │
└──────────────────────────────────────────────────────────────────┘
```

### 服务间调用关系

| 调用方 | 被调用方 | RPC 类型 | 用途 |
|--------|----------|----------|------|
| webcpp Handler | ai-chat | Bidirectional Streaming | AI 对话（发消息 + 流式接收 + 取消） |
| webcpp Handler | ai-rag | Unary / Server Streaming | 知识库检索（可流式返回分块排序结果） |
| webcpp Handler | ai-ppt | Server Streaming | PPT 生成（流式返回进度） |
| ai-rag | ai-chat | Bidirectional Streaming | RAG 检索后增强 prompt，再调 LLM |
| ai-ppt | ai-chat | Bidirectional Streaming | 内容生成（大纲、章节） |
| ai-ppt | ai-rag | Unary | PPT 素材检索 |

## 3. 模块边界

### webcpp 框架侧（不可改的业务无关部分）

| 模块 | 路径 | 说明 |
|------|------|------|
| RequestHandler | `handler/request_handler.hpp` | 新增 `IsStream()` + `HandleStream()` + `StreamSink` |
| StreamSink | `handler/request_handler.hpp` | 流式输出接口（SSE chunk write） |
| gRPC Bridge | `rpc/grpc_bridge.hpp` | agrpc + Asio 桥接工具类 |
| gRPC Channel Pool | `rpc/grpc_channel_pool.hpp` | 连接池管理 |
| CMake gRPC 集成 | `rpc/CMakeLists.txt` | protobuf + gRPC 查找、proto 编译规则 |

### 业务示例侧（由实际业务填充）

| 模块 | 路径 | 说明 |
|------|------|------|
| Handler 示例 | `examples/cpp_handlers/` | ChatHandler / RagHandler / PptHandler |
| Proto 文件 | `examples/proto/` | chat.proto / rag.proto / ppt.proto |
| Python 服务 | `examples/python_services/` | ai-chat / ai-rag / ai-ppt 实现 |
| 部署配置 | `examples/docker-compose.yml` | 编排所有服务 |

## 4. gRPC Proto 设计

### 4.1 ai-chat (ChatService)

```protobuf
syntax = "proto3";

package ai.chat;

service ChatService {
  // 双向流：Client 发消息/取消，Server 逐 token 回
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

### 4.2 ai-rag (RagService)

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

message DocumentChunk {
  string knowledge_base_id = 1;
  string filename = 2;
  bytes content = 3;
  int32 chunk_index = 4;
}
```

### 4.3 ai-ppt (PptService)

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
  string status = 1;       // "generating_outline" | "filling_slide_X" | "rendering" | "done"
  int32 progress_pct = 2;  // 0-100
  string result_url = 3;   // done 时
}
```

## 5. webcpp Handler 改造

### 5.1 RequestHandler 新增流式接口

```cpp
class RequestHandler {
public:
    virtual ~RequestHandler() = default;

    // ── 保留的同步/异步路径（向后兼容） ──
    virtual Response Handle(const Context& ctx) = 0;
    virtual asio::awaitable<Response> HandleAsync(const Context& ctx) {
        co_return Handle(ctx);
    }
    virtual bool IsAsync() const { return false; }

    // ── 新增流式路径 ──
    virtual bool IsStream() const { return false; }

    virtual asio::awaitable<void> HandleStream(const Context& ctx,
                                                StreamSink& sink) {
        // 默认降级：收集完整响应后一次写入
        auto resp = co_await HandleAsync(ctx);
        co_await sink.Write(resp.Body());
        sink.End();
    }

    // ── WebSocket ──
    virtual asio::awaitable<void> HandleWebSocket(const Context&,
                                                  WsConnectionBase&) {
        co_return;
    }
};
```

### 5.2 StreamSink 接口

```cpp
/// 流式输出通道 —— Handler 通过此接口分块写回客户端
class StreamSink {
public:
    virtual ~StreamSink() = default;

    /// 初始化 SSE 响应（写 Content-Type: text/event-stream 头）
    virtual void BeginSSE() = 0;

    /// 写一个 SSE data 块，co_await 等待实际写入完成
    virtual asio::awaitable<void> PushSSE(std::string_view data) = 0;

    /// 写原始 bytes（非 SSE 场景）
    virtual asio::awaitable<void> Write(std::string_view data) = 0;

    /// 结束流
    virtual void End() = 0;

    /// 客户端是否已断开
    virtual bool IsDisconnected() const = 0;
};
```

### 5.3 Session 调度逻辑

```cpp
if (handler->IsStream()) {
    StreamSink sink(response);
    co_await handler->HandleStream(ctx, sink);
} else if (handler->IsAsync()) {
    auto resp = co_await handler->HandleAsync(ctx);
    co_await resp.Send();
} else {
    auto resp = handler->Handle(ctx);
    resp.Send();
}
```

### 5.4 gRPC ↔ Asio 桥接

使用 agrpc 库将 gRPC `CompletionQueue` 包装为 Asio executor：

```cpp
// rpc/grpc_bridge.hpp
#include <agrpc/asio_grpc.hpp>

// 创建 gRPC CompletionQueue 绑定到 Asio io_context
// Handler 可以直接 co_await gRPC async 操作
class GrpcBridge {
public:
    GrpcBridge(asio::any_io_executor executor);
    
    // 获取绑定后的 CompletionQueue
    grpc::CompletionQueue& Queue();
    
    // 创建 gRPC Channel（共享连接池可选）
    std::shared_ptr<grpc::Channel> CreateChannel(
        const std::string& target,
        const std::shared_ptr<grpc::ChannelCredentials>& creds);
};
```

## 6. Python AI 服务设计

### 6.1 技术栈

| 组件 | 技术选型 | 理由 |
|------|----------|------|
| gRPC Server | `grpc.aio` | Python 原生异步 gRPC，Streaming 天然支持 |
| LLM 调用 | `litellm` | 统一接口支持 100+ 模型，一行切换 |
| 向量检索 | `pymilvus` | Milvus 官方 Python SDK |
| 图数据库 | `neo4j` async driver | 官方异步驱动 |
| 文档解析 | `unstructured` | PDF/Word/HTML 全格式 |
| PPT 生成 | `python-pptx` | Python 最新熟的 PPT 库 |
| 配置 | `python-dotenv` | 环境变量管理 |

### 6.2 ai-chat 内部逻辑

```
ChatStream(gRPC Bidirectional Stream) 进入
  → SessionManager 加载上下文 (Redis/DB)
  → LLMRouter 选择模型 (根据 ChatRequest.config.model)
  → 调 LLM API (litellm.acompletion, stream=True)
  → 逐 token → gRPC write → webcpp → SSE
  → 如果收到 CancelSignal → 中断 LLM → 清理
  → 完成后写回会话历史
```

### 6.3 ai-rag 知识库层级

```
User (user_id)
 └── Project (project_id)
      └── KnowledgeBase (knowledge_base_id)
           ├── Milvus: collection = "kb_{kb_id}"
           └── Neo4j:  subgraph = 按 user_id + project_id + kb_id 标签隔离
```

上传文档流程：

```
UploadDocument(stream) 进入
  → DocumentChunk 流式接收
  → 文本提取 + 分句
  → embedding 生成 → 写入 Milvus
  → 实体提取 → 写入 Neo4j
  → 返回 UploadResult
```

### 6.4 ai-ppt 内部逻辑

```
Generate(PptRequest) 进入
  → 进度: "generating_outline" (0%)
  → gRPC → ai-chat: 生成大纲
  → 进度: "filling_slide_1" (25%)
  → 逐页调用 ai-chat 生成内容
  → 进度: "filling_slide_N" (75%)
  → python-pptx 渲染
  → 进度: "done" (100%), 返回 result_url
```

## 7. 部署拓扑

```yaml
# docker-compose.yml
version: "3.9"

services:
  webcpp:
    build: .
    ports: ["443:8443"]
    volumes:
      - ./config.yaml:/app/config.yaml
      - ./cert.pem:/app/cert.pem
    depends_on: [ai-chat, ai-rag, ai-ppt]

  ai-chat:
    build: ./examples/python_services/ai-chat
    expose: ["50051"]
    environment:
      - LLM_API_KEY=${LLM_API_KEY}
      - REDIS_URL=redis://redis:6379
    depends_on: [redis]

  ai-rag:
    build: ./examples/python_services/ai-rag
    expose: ["50052"]
    environment:
      - MILVUS_HOST=milvus
      - NEO4J_URI=bolt://neo4j:7687
    depends_on: [milvus, neo4j, ai-chat]

  ai-ppt:
    build: ./examples/python_services/ai-ppt
    expose: ["50053"]
    depends_on: [ai-chat, ai-rag]

  milvus:
    image: milvusdb/milvus:latest
    volumes: ["./data/milvus:/var/lib/milvus"]

  neo4j:
    image: neo4j:latest
    environment:
      - NEO4J_AUTH=neo4j/${NEO4J_PASSWORD}
    volumes: ["./data/neo4j:/data"]

  redis:
    image: redis:7-alpine
```

扩缩容：
- `docker-compose up -d --scale ai-chat=3` 独立扩缩 ai-chat
- ai-chat 是算力密集型（LLM 调用），优先扩容

## 8. 流式全链路时序

```
用户浏览器                  webcpp Handler                     ai-chat(Python)
    │                            │                                  │
    │── POST /v1/chat             │                                  │
    │   {msg, session_id}         │                                  │
    │                            │── ChatStream(ChatRequest) ──────→│
    │                            │                                  │── litellm.acompletion()
    │                            │   ←── ChatServerMessage{token}───│    stream=True
    │── SSE: data: <token>──────│                                  │
    │                            │   ←── ChatServerMessage{token}───│
    │── SSE: data: <token>──────│                                  │
    │                            │   ...                            │
    │                            │   ←── ChatServerMessage{finish}─ │
    │── SSE: data: [DONE] ─────│                                  │
    │                            │                                  │
    │                            │                                  │
    │── (用户关闭页面)            │                                  │
    │                            │── CancelSignal ────────────────→│── 中断 LLM
    │                            │                                  │── 清理资源
```

## 9. 取消机制

当客户端断开 SSE 连接或发起取消：

```cpp
// Handler 侧
co_await agrpc::read(*reader, reply);
bool ok = co_await sink.PushSSE(reply.token());
if (!ok || sink.IsDisconnected()) {
    // 客户端断连 → 通知 AI 服务取消
    ChatClientMessage cancel;
    cancel.mutable_cancel();
    co_await agrpc::write(*writer, cancel);  // 通知 ai-chat 中断
    co_return;
}
```

## 10. 设计决策记录

| 决策 | 选项 | 结论 | 理由 |
|------|------|------|------|
| 服务间通信 | HTTP / gRPC / MQ | **gRPC** | 流式支持好、强类型、双向流 |
| Chat 流式模式 | Server Streaming / Bidirectional | **Bidirectional** | 支持打断 |
| RAG 存储 | 纯向量 / 向量+图 | **Milvus + Neo4j** | GraphRAG 准确率更高 |
| Python gRPC | sync / async | **grpc.aio** | 原生 asyncio，适合协程 |
| Handler 改造 | 新基类 / 合并 | **合并到 RequestHandler** | 最小侵入，向后兼容 |
| 部署 | 单进程 / 容器 | **Docker Compose** | 与现有运维一致 |

## 11. 开发路线图

1. **Phase 0: 基础设施**
   - `rpc/CMakeLists.txt` — gRPC + protobuf 查找、proto 编译
   - `handler/` — RequestHandler + StreamSink 改造
   - `rpc/grpc_bridge` — agrpc 集成
   - 验证：写一个 echo chat handler，co_await gRPC 调 Python echo 服务

2. **Phase 1: Python AI 服务骨架**
   - ai-chat: gRPC server + litellm 集成（流式对话）
   - ai-rag: Milvus + Neo4j 集成
   - ai-ppt: python-pptx 生成
   - docker-compose 编排

3. **Phase 2: webcpp Handler 示例**
   - ChatHandler（C++ gRPC Client → ai-chat）
   - RagHandler
   - PptHandler

4. **Phase 3: 集成测试与生产化**
   - 取消机制
   - 错误重试
   - 负载均衡
   - 监控对接
