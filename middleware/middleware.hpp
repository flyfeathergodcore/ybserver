#pragma once
#include <memory>
#include <string>
#include <vector>
#include "http/context.hpp"
#include "handler/request_handler.hpp"
#include "net/response.hpp"

// ── Middleware 基类 ──
//
// 两个拦截点：
//
//   1. OnRawData(data, len)   ← 原始字节阶段，在 parser_->Feed() 之前
//      用途：协议检测（HTTP/2 preface、WebSocket upgrade）、
//            原始数据变换、对非 HTTP/1 流量提前处理
//      返回有效 Response = 短路，跳过 parser 和 handler
//
//   2. Handle(ctx, next)       ← 解析完成后的洋葱模型
//      默认行为：OnRequest(ctx) → 有效则短路 → next.Handle(ctx)
//      重写此方法实现洋葱效果（日志、CORS 响应注入等）
//
class Middleware {
public:
    virtual ~Middleware() = default;

    // ── 原始字节钩子（parser 之前） ──
    // data/len 是 socket 读到的原始数据。
    // 返回有效 Response = 直接作为 HTTP 响应返回，parser 和 handler 都不执行。
    virtual Response OnRawData(const char* data, size_t len) { return Response::None(); }

    // ── 请求检查钩子（parser 之后） ──
    // ctx 已经完成 HTTP/1 解析。
    // 返回有效 Response = 短路，跳过 handler。
    virtual Response OnRequest(const Context& ctx) { return Response::None(); }

    // ── 全控制洋葱模型 ──
    // 默认：OnRequest → 短路 → next.Handle
    virtual Response Handle(const Context& ctx, RequestHandler& next);
};

// ── MiddlewareChain ──
// 双阶段顺序执行：
//   1. ProcessRaw() — 原始字节阶段，短路则跳过 parser
//   2. Execute()    — 解析完成后的洋葱链 → handler
class MiddlewareChain {
public:
    void Add(std::unique_ptr<Middleware> mw);

    // 原始字节阶段。遍历各中间件的 OnRawData，任意有效即短路。
    Response ProcessRaw(const char* data, size_t len);

    // 解析完成阶段。遍历各中间件的 Handle → handler。
    Response Execute(const Context& ctx, RequestHandler& final_handler);

private:
    Response ExecuteFrom(size_t index, const Context& ctx,
                            RequestHandler& final);
    std::vector<std::unique_ptr<Middleware>> middlewares_;
};

// ════════════════════════════════════════════════════════════
// 内置中间件
// ════════════════════════════════════════════════════════════

// 请求日志 — 记录 Method、Path
class LoggingMiddleware : public Middleware {
public:
    Response Handle(const Context& ctx, RequestHandler& next) override;
};

// CORS — 跨域资源共享
class CORSMiddleware : public Middleware {
public:
    Response Handle(const Context& ctx, RequestHandler& next) override;
};

// HTTP/2 preface 检测 — 网关占位
// 检测到 HTTP/2 连接前言时返回 426 Upgrade Required
class Http2DetectMiddleware : public Middleware {
public:
    Response OnRawData(const char* data, size_t len) override;
};
