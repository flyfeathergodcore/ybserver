#include "handler/proxy_handler.hpp"
#include "net/response.hpp"
#include "net/session_region.hpp"
#include "http/context.hpp"
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <cctype>

using asio::ip::tcp;

ProxyHandler::ProxyHandler(UpstreamConfig upstream)
    : upstream_(std::move(upstream)) {}

Response ProxyHandler::Handle(const Context& ctx)
{
    // Sync path not supported — needs I/O.
    return Response::Error(502, *ctx.Pool());
}

// ── Helpers ──

static bool ReadLine(const std::string& buf, size_t& pos, std::string& line)
{
    auto cr = buf.find('\r', pos);
    if (cr == std::string::npos || cr + 1 >= buf.size() || buf[cr + 1] != '\n')
        return false;
    line = buf.substr(pos, cr - pos);
    pos = cr + 2;
    return true;
}

asio::awaitable<Response> ProxyHandler::HandleAsync(const Context& ctx)
{
    auto* pool = ctx.Pool();
    if (!pool) co_return Response::Error(502, *pool);

    // Create resolver + socket from the current coroutine's executor
    auto exec = co_await asio::this_coro::executor;
    tcp::resolver resolver(exec);
    tcp::socket sock(exec);

    // ── Resolve upstream ──
    asio::error_code ec;
    tcp::resolver::results_type endpoints;
    std::tie(ec, endpoints) = co_await resolver.async_resolve(
        upstream_.host, std::to_string(upstream_.port),
        asio::as_tuple(asio::use_awaitable));
    if (ec || endpoints.empty())
        co_return Response::Error(502, *pool);

    // ── Connect ──
    std::tie(ec) = co_await sock.async_connect(*endpoints.begin(),
                                asio::as_tuple(asio::use_awaitable));
    if (ec) co_return Response::Error(502, *pool);

    // ── Build upstream request ──
    std::string req;
    req.reserve(4096);

    req += ctx.Method();
    req += ' ';
    req += ctx.Path();
    req += " HTTP/1.1\r\n";

    req += "Host: ";
    req += upstream_.host;
    req += ':';
    req += std::to_string(upstream_.port);
    req += "\r\n";

    // Forward incoming headers (skip hop-by-hop)
    auto hop_by_hop = [](std::string_view name) -> bool {
        return name == "host" || name == "connection"
            || name == "transfer-encoding" || name == "proxy-connection"
            || name == "keep-alive" || name == "upgrade";
    };

    for (int i = 0; i < ctx.HeaderCount(); i++) {
        auto [name, value] = ctx.HeaderAt(i);
        if (hop_by_hop(name)) continue;
        req += name;
        req += ": ";
        req += value;
        req += "\r\n";
    }

    // Content-Length
    auto body = ctx.Body();
    if (!body.empty()) {
        req += "Content-Length: ";
        req += std::to_string(body.size());
        req += "\r\n";
    }

    req += "Connection: close\r\n";
    req += "\r\n";

    // ── Send ──
    size_t n;
    if (body.empty()) {
        std::tie(ec, n) = co_await async_write(
            sock, asio::buffer(req),
            asio::as_tuple(asio::use_awaitable));
    } else {
        std::array<asio::const_buffer, 2> bufs = {{
            asio::buffer(req), asio::buffer(body)
        }};
        std::tie(ec, n) = co_await async_write(
            sock, bufs,
            asio::as_tuple(asio::use_awaitable));
    }
    (void)n;
    if (ec) co_return Response::Error(502, *pool);

    // ── Read response ──
    asio::streambuf streambuf(16384);
    std::string raw;

    // 1. Status line
    std::tie(ec, n) = co_await async_read_until(
        sock, streambuf, "\r\n", asio::as_tuple(asio::use_awaitable));
    if (ec) co_return Response::Error(502, *pool);

    raw.assign(asio::buffers_begin(streambuf.data()),
               asio::buffers_end(streambuf.data()));
    streambuf.consume(n);

    size_t pos = 0;
    std::string status_line;
    if (!ReadLine(raw, pos, status_line))
        co_return Response::Error(502, *pool);

    int status_code = 0;
    {
        auto sp1 = status_line.find(' ');
        if (sp1 == std::string::npos) co_return Response::Error(502, *pool);
        auto sp2 = status_line.find(' ', sp1 + 1);
        if (sp2 == std::string::npos) co_return Response::Error(502, *pool);
        status_code = std::atoi(status_line.c_str() + sp1 + 1);
    }

    // 2. Headers
    std::tie(ec, n) = co_await async_read_until(
        sock, streambuf, "\r\n\r\n", asio::as_tuple(asio::use_awaitable));
    if (ec) co_return Response::Error(502, *pool);

    raw.assign(asio::buffers_begin(streambuf.data()),
               asio::buffers_end(streambuf.data()));
    streambuf.consume(n);

    auto hdr_end = raw.find("\r\n\r\n", pos);
    if (hdr_end == std::string::npos) co_return Response::Error(502, *pool);

    // Build response
    Response resp(status_code, *pool);

    // Forward upstream response headers
    size_t hp = pos;
    while (hp < hdr_end) {
        std::string hdr_line;
        if (!ReadLine(raw, hp, hdr_line)) break;
        auto colon = hdr_line.find(':');
        if (colon == std::string::npos) continue;
        auto hname = hdr_line.substr(0, colon);
        auto hval  = hdr_line.substr(colon + 1);
        while (!hval.empty() && hval[0] == ' ')
            hval.erase(0, 1);

        auto hname_lower = hname;
        for (auto& c : hname_lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (hname_lower == "transfer-encoding" || hname_lower == "connection" ||
            hname_lower == "keep-alive" || hname_lower == "proxy-connection" ||
            hname_lower == "upgrade")
            continue;

        // Normalize to lowercase (required by HTTP/2, harmless for HTTP/1.1)
        resp.Header(hname_lower, hval);
    }

    // 3. Body (read until EOF — Connection: close)
    std::string body_buf;
    body_buf.reserve(65536);

    size_t body_start = hdr_end + 4;
    if (body_start < raw.size())
        body_buf.append(raw.data() + body_start, raw.size() - body_start);

    {
        std::array<char, 65536> read_buf;
        for (;;) {
            std::tie(ec, n) = co_await sock.async_read_some(
                asio::buffer(read_buf), asio::as_tuple(asio::use_awaitable));
            if (ec == asio::error::eof) break;
            if (ec) break;
            body_buf.append(read_buf.data(), n);
        }
    }

    resp.Header("Content-Length", std::to_string(body_buf.size()));
    resp.EndHeaders();

    // Write body to region (not resp.Body() — that stores a dangling pointer
    // since body_buf is local to this coroutine frame).
    if (!body_buf.empty())
        pool->Write(std::string_view{body_buf.data(), body_buf.size()});

    co_return resp;
}
