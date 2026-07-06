#pragma once
#include "handler/request_handler.hpp"
#include "net/ws_connection.hpp"
#include "net/ws_frame.hpp"
#include <asio.hpp>

/// Echo WS handler — sends back whatever it receives.
class WsEchoHandler : public RequestHandler {
public:
    Response Handle(const Context& ctx) override {
        auto ws_key = ctx.Header("sec-websocket-key");
        if (!ws_key.empty()) {
            auto accept = ComputeWsAccept(ws_key);
            return Response::WebSocketUpgrade(*ctx.Pool(), std::move(accept));
        }
        return Response::Error(404, *ctx.Pool());
    }

    asio::awaitable<void> HandleWebSocket(const Context& /*ctx*/,
                                           WsConnectionBase& conn) override
    {
        while (true) {
            auto frame = co_await conn.Read();
            if (frame.payload.empty()) break;
            co_await conn.Send(frame.opcode, std::move(frame.payload), true);
        }
    }
};
