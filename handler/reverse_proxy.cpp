#include "handler/reverse_proxy.hpp"
#include "net/response.hpp"
#include "net/session_region.hpp"
#include "http/context.hpp"
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <iostream>

using asio::ip::tcp;

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

ReverseProxy::ReverseProxy(UpstreamPool& pool)
    : pool_(pool) {}

Response ReverseProxy::Handle(const Context& ctx)
{
    return Response::Error(502, *ctx.Pool());
}

asio::awaitable<Response> ReverseProxy::HandleAsync(const Context& ctx)
{
    auto* pool = ctx.Pool();
    if (!pool) co_return Response::Error(502, *pool);

    // ── Pick upstream ──
    const auto* upstream = pool_.Pick();
    if (!upstream) {
        std::cerr << "[proxy] 无可用上游" << std::endl;
        co_return Response::Error(502, *pool);
    }

    auto exec = co_await asio::this_coro::executor;

    // ── Forward and build response ──
    auto resp = co_await Forward(ctx, exec, upstream->host, upstream->port);

    if (resp.StatusCode() >= 500) {
        pool_.ReportFailure(upstream);
    } else {
        pool_.ReportSuccess(upstream);
    }

    co_return resp;
}

asio::awaitable<Response> ReverseProxy::Forward(
    const Context& ctx,
    asio::any_io_executor exec,
    std::string_view host,
    unsigned short port)
{
    auto* region = ctx.Pool();
    if (!region) co_return Response::Error(502, *region);

    tcp::resolver resolver(exec);
    tcp::socket sock(exec);
    asio::error_code ec;

    // ── Resolve ──
    tcp::resolver::results_type endpoints;
    std::tie(ec, endpoints) = co_await resolver.async_resolve(
        std::string(host), std::to_string(port),
        asio::as_tuple(asio::use_awaitable));
    if (ec || endpoints.empty())
        co_return Response::Error(502, *region);

    // ── Connect ──
    std::tie(ec) = co_await sock.async_connect(
        *endpoints.begin(), asio::as_tuple(asio::use_awaitable));
    if (ec) co_return Response::Error(502, *region);

    // ── Build upstream request ──
    std::string req;
    req.reserve(4096);

    req += ctx.Method();
    req += ' ';
    req += ctx.Path();
    req += " HTTP/1.1\r\n";

    req += "Host: ";
    req += host;
    req += ':';
    req += std::to_string(port);
    req += "\r\n";

    // Forward headers (skip hop-by-hop)
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
    if (ec) co_return Response::Error(502, *region);

    // ── Read response ──
    asio::streambuf streambuf(16384);
    std::string raw;

    // 1. Status line
    std::tie(ec, n) = co_await async_read_until(
        sock, streambuf, "\r\n", asio::as_tuple(asio::use_awaitable));
    if (ec) co_return Response::Error(502, *region);

    raw.assign(asio::buffers_begin(streambuf.data()),
               asio::buffers_end(streambuf.data()));
    streambuf.consume(n);

    size_t pos = 0;
    std::string status_line;
    if (!ReadLine(raw, pos, status_line))
        co_return Response::Error(502, *region);

    int status_code = 0;
    {
        auto sp1 = status_line.find(' ');
        if (sp1 == std::string::npos) co_return Response::Error(502, *region);
        auto sp2 = status_line.find(' ', sp1 + 1);
        if (sp2 == std::string::npos) co_return Response::Error(502, *region);
        status_code = std::atoi(status_line.c_str() + sp1 + 1);
    }

    // 2. Headers
    std::tie(ec, n) = co_await async_read_until(
        sock, streambuf, "\r\n\r\n", asio::as_tuple(asio::use_awaitable));
    if (ec) co_return Response::Error(502, *region);

    raw.assign(asio::buffers_begin(streambuf.data()),
               asio::buffers_end(streambuf.data()));
    streambuf.consume(n);

    auto hdr_end = raw.find("\r\n\r\n", pos);
    if (hdr_end == std::string::npos) co_return Response::Error(502, *region);

    Response resp(status_code, *region);

    // Forward upstream response headers (lowercased for H2)
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

        // Normalize to lowercase + skip hop-by-hop
        auto hnl = hname;
        for (auto& c : hnl)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (hnl == "transfer-encoding" || hnl == "connection" ||
            hnl == "keep-alive" || hnl == "proxy-connection" ||
            hnl == "upgrade")
            continue;

        resp.Header(hnl, hval);
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

    resp.Header("content-length", std::to_string(body_buf.size()));
    resp.EndHeaders();

    if (!body_buf.empty())
        region->Write(std::string_view{body_buf.data(), body_buf.size()});

    co_return resp;
}
