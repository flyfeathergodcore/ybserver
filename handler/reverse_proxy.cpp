#include "handler/reverse_proxy.hpp"
#include "handler/upstream_conn_pool.hpp"
#include "net/response.hpp"
#include "net/session_region.hpp"
#include "net/ws_relay.hpp"
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

/// Copy already-buffered body bytes from streambuf into body_buf,
/// consuming them from the streambuf.  Returns number of bytes copied.
static size_t DrainStreambuf(asio::streambuf& sb, std::string& body_buf,
                              size_t max_bytes)
{
    auto avail = sb.size();
    if (avail == 0 || max_bytes == 0) return 0;

    size_t take = std::min(avail, max_bytes);
    // Use the streambuf's sgetn for efficient contiguous read
    // but fall back to iterative copy if it's non-contiguous
    auto* base = static_cast<const char*>(sb.data().data());
    body_buf.append(base, take);
    sb.consume(take);
    return take;
}

/// Try to read exactly `needed` bytes from socket into body_buf.
/// Returns the number of bytes actually read (may be < needed on EOF/error).
static asio::awaitable<size_t> ReadExact(
    tcp::socket& sock, std::string& body_buf, size_t needed)
{
    if (needed == 0) co_return 0;

    size_t total = 0;
    std::array<char, 65536> buf;

    while (total < needed)
    {
        auto to_read = std::min(buf.size(), needed - total);
        asio::error_code ec;
        size_t n;

        std::tie(ec, n) = co_await sock.async_read_some(
            asio::buffer(buf.data(), to_read),
            asio::as_tuple(asio::use_awaitable));

        if (ec) break;
        body_buf.append(buf.data(), n);
        total += n;
    }

    co_return total;
}

ReverseProxy::ReverseProxy(std::vector<UpstreamAddr> upstreams)
{
    std::vector<UpstreamServer> servers;
    servers.reserve(upstreams.size());
    for (auto& a : upstreams)
        servers.push_back({a.host, a.port});
    owned_pool_ = std::make_unique<UpstreamPool>(std::move(servers));
    pool_ = owned_pool_.get();
}

ReverseProxy::ReverseProxy(UpstreamPool& pool)
    : pool_(&pool) {}

Response ReverseProxy::Handle(const Context& ctx)
{
    return Response::Error(502, *ctx.Pool());
}

asio::awaitable<Response> ReverseProxy::HandleAsync(const Context& ctx)
{
    auto* pool = ctx.Pool();
    if (!pool) co_return Response::Error(502, *pool);

    // ── Pick upstream ──
    const auto* upstream = pool_->Pick();
    if (!upstream) {
        std::cerr << "[proxy] 无可用上游" << std::endl;
        co_return Response::Error(502, *pool);
    }

    auto exec = co_await asio::this_coro::executor;

    // ── Forward and build response ──
    auto resp = co_await Forward(ctx, exec, upstream->host, upstream->port);

    if (resp.StatusCode() >= 500) {
        pool_->ReportFailure(upstream);
    } else {
        pool_->ReportSuccess(upstream);
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

    auto& conn_pool = UpstreamConnPool::Instance();
    auto host_str   = std::string(host);
    auto port_str   = std::to_string(port);

    // ── Helper: create + connect a new socket ──
    auto make_conn = [&]() -> asio::awaitable<
                           std::unique_ptr<asio::ip::tcp::socket>>
    {
        auto sock = std::make_unique<tcp::socket>(exec);
        tcp::resolver resolver(exec);

        asio::error_code ec;
        tcp::resolver::results_type endpoints;
        std::tie(ec, endpoints) = co_await resolver.async_resolve(
            host_str, port_str, asio::as_tuple(asio::use_awaitable));
        if (ec || endpoints.empty()) co_return nullptr;

        std::tie(ec) = co_await sock->async_connect(
            *endpoints.begin(), asio::as_tuple(asio::use_awaitable));
        if (ec) co_return nullptr;

        co_return std::move(sock);
    };

    // ── Try to get a pooled connection ──
    auto pooled = conn_pool.Acquire(host_str, port);
    std::unique_ptr<tcp::socket> sock;
    bool from_pool = false;

    if (pooled) {
        sock = std::make_unique<tcp::socket>(std::move(pooled->socket));
        from_pool = true;
    }

    // Create fresh if nothing pooled
    if (!sock) {
        sock = co_await make_conn();
        if (!sock) co_return Response::Error(502, *region);
    }

    // ── Build upstream request (no Connection: close) ──
    std::string req;
    req.reserve(4096);

    req += ctx.Method();
    req += ' ';
    req += ctx.Path();
    req += " HTTP/1.1\r\n";

    req += "Host: ";
    req += host_str;
    if (port != 80 && port != 443) {
        req += ':';
        req += port_str;
    }
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

    // Tell upstream we want keep-alive
    req += "Connection: keep-alive\r\n";

    auto body = ctx.Body();
    if (!body.empty()) {
        req += "Content-Length: ";
        req += std::to_string(body.size());
        req += "\r\n";
    }

    req += "\r\n";

    // ── Send request (with retry if pooled connection was stale) ──
    asio::error_code ec;
    size_t n;

    {
        std::tie(ec, n) = co_await async_write(
            *sock, asio::buffer(req), asio::as_tuple(asio::use_awaitable));

        // If the pooled connection was already closed by upstream, retry once
        if (ec && from_pool) {
            sock = co_await make_conn();
            if (sock) {
                from_pool = false;
                ec.clear();
                std::tie(ec, n) = co_await async_write(
                    *sock, asio::buffer(req), asio::as_tuple(asio::use_awaitable));
            }
        }

        // Send body if any (only on success)
        if (!ec && !body.empty()) {
            std::tie(ec, n) = co_await async_write(
                *sock, asio::buffer(body), asio::as_tuple(asio::use_awaitable));
        }
    }
    (void)n;
    if (ec) co_return Response::Error(502, *region);

    // ── Read response ──
    asio::streambuf streambuf(16384);
    std::string raw;

    // 1. Status line
    std::tie(ec, n) = co_await async_read_until(
        *sock, streambuf, "\r\n", asio::as_tuple(asio::use_awaitable));
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
        *sock, streambuf, "\r\n\r\n", asio::as_tuple(asio::use_awaitable));
    if (ec) co_return Response::Error(502, *region);

    raw.assign(asio::buffers_begin(streambuf.data()),
               asio::buffers_end(streambuf.data()));
    streambuf.consume(n);

    auto hdr_end = raw.find("\r\n\r\n", pos);
    if (hdr_end == std::string::npos) co_return Response::Error(502, *region);

    // ── Parse headers AND extract Content-Length ──
    size_t content_length = 0;
    bool upstream_close = false;   // upstream sent Connection: close

    {
        size_t hp = pos;
        while (hp < hdr_end) {
            std::string hdr_line;
            if (!ReadLine(raw, hp, hdr_line)) break;
            auto colon = hdr_line.find(':');
            if (colon == std::string::npos) continue;
            auto hname = hdr_line.substr(0, colon);
            auto hval  = hdr_line.substr(colon + 1);

            // Trim leading whitespace from value
            while (!hval.empty() && hval[0] == ' ') {
                hval.erase(0, 1);
            }

            // Normalize to lowercase
            auto hnl = hname;
            for (auto& c : hnl)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            // Track Content-Length
            if (hnl == "content-length") {
                content_length = 0;
                for (char c : hval) {
                    if (c >= '0' && c <= '9')
                        content_length = content_length * 10 + size_t(c - '0');
                }
            }

            // Track Connection: close from upstream
            if (hnl == "connection") {
                auto hvl = hval;
                for (auto& c : hvl)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (hvl.find("close") != std::string::npos)
                    upstream_close = true;
            }

        }
    }

    // 3. Body
    std::string body_buf;
    bool can_persist = false;   // whether connection is safe to reuse

    {
        // Already-buffered data after the header terminator
        size_t body_start = hdr_end + 4;
        if (body_start < raw.size())
            body_buf.append(raw.data() + body_start, raw.size() - body_start);
    }

    if (content_length > 0) {
        // Read exactly content_length bytes
        if (body_buf.size() < content_length) {
            auto needed = content_length - body_buf.size();
            auto got   = co_await ReadExact(*sock, body_buf, needed);
            can_persist = (got == needed);
        } else {
            can_persist = true;  // already have enough from streambuf
        }
    } else {
        // No Content-Length — read until EOF (Connection: close fallback)
        std::array<char, 65536> read_buf;
        for (;;) {
            std::tie(ec, n) = co_await sock->async_read_some(
                asio::buffer(read_buf), asio::as_tuple(asio::use_awaitable));
            if (ec == asio::error::eof) break;
            if (ec) break;
            body_buf.append(read_buf.data(), n);
        }
        can_persist = false;
    }

    // Don't persist if upstream itself wants to close
    if (upstream_close)
        can_persist = false;

    // ── Build response ──
    Response resp(status_code, *region);

    // Write headers (re-parse, same pass as above but we skip the second
    // loop by storing header pairs earlier — refactored below for clarity)
    // Instead of re-parsing, we set our known headers explicitly.
    {
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

            auto hnl = hname;
            for (auto& c : hnl)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            if (hnl == "transfer-encoding" || hnl == "connection" ||
                hnl == "keep-alive" || hnl == "proxy-connection" ||
                hnl == "upgrade")
                continue;

            resp.Header(hnl, hval);
        }
    }

    // Override Content-Length with actual bytes read
    resp.Header("content-length", std::to_string(body_buf.size()));
    resp.EndHeaders();

    if (!body_buf.empty())
        region->Write(std::string_view{body_buf.data(), body_buf.size()});

    // ── Return connection to pool if healthy and reusable ──
    if (can_persist && sock->is_open() && status_code < 500)
    {
        auto returned = std::make_unique<UpstreamConnPool::Conn>(
            sock->get_executor());
        returned->socket = std::move(*sock);
        conn_pool.Release(std::move(returned), host_str, port);
    }

    co_return resp;
}

// ═══════════════════════════════════════════════════════════════════
// HandleWebSocket — WebSocket passthrough to upstream
// ═══════════════════════════════════════════════════════════════════

asio::awaitable<void> ReverseProxy::HandleWebSocket(
    const Context& ctx, WsConnectionBase& client_conn)
{
    auto* upstream = pool_->Pick();
    if (!upstream) {
        std::cerr << "[proxy] WS 无可用上游" << std::endl;
        co_return;
    }

    auto exec = co_await asio::this_coro::executor;
    auto host_str = std::string(upstream->host);
    auto port_str = std::to_string(upstream->port);

    // ── Connect to upstream ──
    tcp::resolver resolver(exec);
    asio::error_code ec;
    tcp::resolver::results_type endpoints;
    std::tie(ec, endpoints) = co_await resolver.async_resolve(
        host_str, port_str, asio::as_tuple(asio::use_awaitable));
    if (ec || endpoints.empty()) { pool_->ReportFailure(upstream); co_return; }

    tcp::socket upstream_sock(exec);
    std::tie(ec) = co_await upstream_sock.async_connect(
        *endpoints.begin(), asio::as_tuple(asio::use_awaitable));
    if (ec) { pool_->ReportFailure(upstream); co_return; }

    // ── Forward the upgrade request ──
    std::string req;
    req.reserve(4096);
    req += ctx.Method(); req += ' '; req += ctx.Path(); req += " HTTP/1.1\r\n";
    req += "Host: ";
    req += host_str;
    if (upstream->port != 80 && upstream->port != 443) {
        req += ':'; req += port_str;
    }
    req += "\r\n";

    for (int i = 0; i < ctx.HeaderCount(); i++) {
        auto [name, value] = ctx.HeaderAt(i);
        // Forward ALL headers (including Upgrade/Connection/Sec-WebSocket-*)
        req += name; req += ": "; req += value; req += "\r\n";
    }
    req += "\r\n";

    std::tie(ec, std::ignore) = co_await async_write(
        upstream_sock, asio::buffer(req), asio::as_tuple(asio::use_awaitable));
    if (ec) { pool_->ReportFailure(upstream); co_return; }

    // ── Read upstream's 101 response ──
    asio::streambuf sb(1024);
    std::tie(ec, std::ignore) = co_await async_read_until(
        upstream_sock, sb, "\r\n", asio::as_tuple(asio::use_awaitable));
    if (ec) { pool_->ReportFailure(upstream); co_return; }

    {
        std::string raw(asio::buffers_begin(sb.data()),
                        asio::buffers_end(sb.data()));
        if (raw.find("101") == std::string::npos) {
            pool_->ReportFailure(upstream);
            co_return;
        }
    }
    sb.consume(sb.size());
    pool_->ReportSuccess(upstream);

    // ── Bidirectional frame relay ──
    std::atomic<bool> relay_done{false};

    auto relay_c2u = [&]() -> asio::awaitable<void> {
        while (!relay_done.load(std::memory_order_relaxed)) {
            auto frame = co_await client_conn.Read();
            if (!client_conn.IsOpen()) break;
            if (frame.opcode == WsOpcode::Close) {
                co_await WriteFrame(upstream_sock, WsOpcode::Close,
                                    std::move(frame.payload), true, true);
                relay_done.store(true, std::memory_order_relaxed);
                break;
            }
            co_await WriteFrame(upstream_sock, frame.opcode,
                                std::move(frame.payload), frame.fin, true);
        }
    };

    auto relay_u2c = [&]() -> asio::awaitable<void> {
        while (!relay_done.load(std::memory_order_relaxed)) {
            auto frame = co_await ReadFrame(upstream_sock);
            if (frame.payload.empty() && !frame.fin) break;
            if (frame.opcode == WsOpcode::Close) {
                co_await client_conn.Close(
                    frame.payload.size() >= 2
                        ? static_cast<uint16_t>(
                              (static_cast<uint8_t>(frame.payload[0]) << 8) |
                              static_cast<uint8_t>(frame.payload[1]))
                        : 1000);
                relay_done.store(true, std::memory_order_relaxed);
                break;
            }
            co_await client_conn.Send(frame.opcode, std::move(frame.payload),
                                       frame.fin);
        }
    };

    co_await asio::co_spawn(exec, relay_c2u(), asio::use_awaitable);
    co_await relay_u2c();
    relay_done.store(true, std::memory_order_relaxed);

    if (upstream_sock.is_open()) {
        asio::error_code sec;
        upstream_sock.shutdown(tcp::socket::shutdown_both, sec);
        upstream_sock.close(sec);
    }
    co_return;
}
