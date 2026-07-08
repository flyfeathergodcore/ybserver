#include "handler/request_handler.hpp"
#include "handler/router.hpp"
#include "net/response.hpp"
#include "net/ws_frame.hpp"

// ═══════════════════════════════════════════════════════════════════
// ExampleHandler — 演示热插拔 .so 路由
//
// 编译成 .so 后，服务器运行中加载即可通过 /hello 访问。
// 修改此文件后重新编译 .so，再次 dlopen 即生效（无需重启服务器）。
// ═══════════════════════════════════════════════════════════════════

class ExampleHandler : public RequestHandler {
public:
    Response Handle(const Context& ctx) override {
        auto* pool = ctx.Pool();
        if (!pool) {
            // 内存池不可用，返回简单的 500 错误响应
            return Response::Raw(500, R"({"error":"Internal Server Error"})");
        }

        // 检查是否为 WebSocket 升级请求
        auto ws_key = ctx.Header("sec-websocket-key");
        if (!ws_key.empty()) {
            auto accept = ComputeWsAccept(ws_key);
            return Response::WebSocketUpgrade(*pool, std::move(accept));
        }

        // 正常 HTTP 响应
        std::string_view body =
            "<h1>Hello from hot-plug handler!</h1>\n"
            "<p>This handler was loaded via dlopen at runtime.</p>\n";

        Response resp(200, *pool);
        resp.Header("Content-Type", "text/html");
        resp.Header("Content-Length", body.size());
        resp.EndHeaders();
        pool->Write(body);
        return resp;
    }
};

// ── 导出符号 —— 服务器通过 dlsym 查找此函数 ──
extern "C" void register_routes(Router& router)
{
    router.Add("/hello", std::make_unique<ExampleHandler>());
}
