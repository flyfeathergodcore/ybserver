#pragma once
#include "handler/request_handler.hpp"
#include "net/ws_connection.hpp"
#include <asio.hpp>

/// Echo WS handler — sends back whatever it receives.
class WsEchoHandler : public RequestHandler {
public:
    Response Handle(const Context& ctx) override {
        return Response::Error(404, *ctx.Pool());
    }

    asio::awaitable<void> HandleWebSocket(const Context& /*ctx*/,
                                           WsConnectionBase& conn) override
    {
        while (true) {
            auto frame = co_await conn.Read();
            if (frame.payload.empty()) break;
            // Keep stream open (fin=false) — we may get more messages.
            co_await conn.Send(frame.opcode, std::move(frame.payload), false);
        }
    }
};
