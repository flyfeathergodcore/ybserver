#pragma once
#include <memory>
#include <string>
#include <vector>
#include <asio.hpp>
#include "http/context.hpp"
#include "handler/request_handler.hpp"
#include "net/response.hpp"

// ── Middleware 基类 ──
//
// 双阶段模型：
//   PreRequest  — handler 之前执行（串行），可修改 ctx、可短路
//   PostResponse — response 发送之后执行（异步），只记录不修改
//
// PreRequest 是同步的（零协程开销），PostResponse 是异步的（支持 I/O 等待）。
//
// 一个 Middleware 可同时充当 Pre + Post（设置 Type=Both）。
//
class Middleware {
public:
    enum class Type { PreRequest, PostResponse, Both };
    virtual ~Middleware() = default;

    /// 标识此中间件在哪一个（或两个）阶段运行。
    virtual Type GetType() const = 0;

    // ── PreRequest 钩子（同步） ──
    // 返回非 None Response = 短路 handler，直接返回。
    // 可在 ctx 上调用 AddResponseHeader() 注入响应头，handler 会读取。
    /// 原始字节钩子（parser 之前），保留供 H1 的 H2 preface 检测等场景。
    virtual Response OnRawData(const char* /*data*/, size_t /*len*/) { return Response::None(); }

    /// PreRequest 钩子（同步）。
    /// 返回非 None Response = 短路 handler，直接返回。
    virtual Response HandlePre(Context& ctx) { return Response::None(); }

    // ── PostResponse 钩子（异步） ──
    // Response 已发送至客户端，此方法不阻塞客户端。
    // elapsed_us 是请求总耗时（读→解析→handler→send）。
    virtual asio::awaitable<void> HandlePost(const Context& ctx,
                                              int status_code,
                                              size_t bytes_sent,
                                              uint64_t elapsed_us,
                                              int worker_id) {
        co_return;
    }
};

// ── MiddlewareManager ──
//
// 管理所有 Middleware 的生命周期和执行顺序：
//   1. ProcessRaw — 原始字节阶段（parser 之前）
//   2. ExecutePre — 串行运行 PreRequest（同步）
//   3. ExecutePost — 串行运行 PostResponse（异步，允许 I/O）
//
class MiddlewareManager {
public:
    /// 添加中间件（生命周期由 Manager 管理）。
    void Add(std::unique_ptr<Middleware> mw);

    /// 原始字节阶段。遍历各中间件的 OnRawData，任意有效即短路。
    /// 此方法在 parser_->Feed() 之前调用。
    Response ProcessRaw(const char* data, size_t len);

    /// 串行运行所有 PreRequest 中间件（同步）。
    /// 返回 None() 表示继续执行 handler；
    /// 返回有效 Response 表示短路（直接作为 HTTP 响应返回）。
    Response ExecutePre(Context& ctx);

    /// 串行运行所有 PostResponse 中间件（异步）。
    /// 在 response 发送至客户端后调用，不阻塞客户端。
    asio::awaitable<void> ExecutePost(const Context& ctx,
                                       int status_code,
                                       size_t bytes_sent,
                                       uint64_t elapsed_us,
                                       int worker_id);

private:
    std::vector<std::unique_ptr<Middleware>> owned_;  // 拥有所有对象
    std::vector<Middleware*> pre_;   // PreRequest / Both
    std::vector<Middleware*> post_;  // PostResponse / Both
};

// ════════════════════════════════════════════════════════════
// 内置中间件
// ════════════════════════════════════════════════════════════

// ── CORS（PreRequest） ──
// 注入跨域头，OPTIONS 请求直接返回 204。
class CORSMiddleware : public Middleware {
public:
    Type GetType() const override { return Type::PreRequest; }
    Response HandlePre(Context& ctx) override;
};

// ── X-Request-Id（PreRequest） ──
// 转发或生成请求 ID，注入响应头，日志中使用。
class RequestIdMiddleware : public Middleware {
public:
    Type GetType() const override { return Type::PreRequest; }
    Response HandlePre(Context& ctx) override;
    /// Generate a short unique ID into pool (zero heap alloc)
    static std::string_view GenerateId(SessionRegion& pool);
};

// ── 请求日志（PostResponse） ──
// handler 完成后记录 Method + Path。
class LoggingMiddleware : public Middleware {
public:
    Type GetType() const override { return Type::PostResponse; }
    asio::awaitable<void> HandlePost(const Context& ctx,
                                      int status_code,
                                      size_t bytes_sent,
                                      uint64_t elapsed_us,
                                      int worker_id) override;
};

class MetricsCollector;

// ── 指标（Both） ──
// Pre: 拦截 /metrics.json、/dashboard、/metrics/stream
// Post: 记录每请求指标（耗时、状态码、字节数）
class MetricsMiddleware : public Middleware {
public:
    explicit MetricsMiddleware(MetricsCollector* collector)
        : collector_(collector) {}
    Type GetType() const override { return Type::Both; }
    Response HandlePre(Context& ctx) override;
    asio::awaitable<void> HandlePost(const Context& ctx,
                                      int status_code,
                                      size_t bytes_sent,
                                      uint64_t elapsed_us,
                                      int worker_id) override;

    /// 由 session 在启动时设置 worker ID。
    void SetWorkerId(int wid) { worker_id_ = wid; }

private:
    MetricsCollector* collector_;
    int worker_id_ = -1;
};
