#include "http/http1.1/session.hpp"
#include "net/region_pool.hpp"
#include "handler/metrics.hpp"
#include <iostream>
#include <sys/sendfile.h>
#include <unistd.h>

using asio::ip::tcp;

static uint64_t dur_us(std::chrono::steady_clock::time_point start)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count());
}

template<typename Stream>
H11Session<Stream>::H11Session(Stream stream,
                                Router& router,
                                MiddlewareManager& middleware,
                                RegionPool* region_pool)
    : SessionBase(router, middleware)
    , stream_(std::move(stream))
{
    if (region_pool)
        region_.Init(region_pool);
}

template<typename Stream>
asio::awaitable<void> H11Session<Stream>::Start()
{
    auto self = this->shared_from_this();
    std::array<char, 4096> buf;

    if (metrics_) metrics_->OnConnectionOpen(worker_id_);

    try {
    for (;;)
    {
        region_.Reset();
        parser_.SetPool(&region_);

        auto read_start = std::chrono::steady_clock::now();

        auto [ec, n] = co_await stream_.async_read_some(
            asio::buffer(buf), asio::as_tuple(asio::use_awaitable));
        if (ec) break;

        // ── Raw byte phase ──
        {
            auto mw = middleware_.ProcessRaw(buf.data(), static_cast<size_t>(n));
            if (!mw.IsNone()) {
                int code = mw.StatusCode();
                size_t bytes = mw.HeaderWire().size() + mw.BodyWire().size();
                co_await Send(std::move(mw));
                co_await middleware_.ExecutePost(parser_, code, bytes,
                    dur_us(read_start), worker_id_);
                break;
            }
        }

        // ── Parse ──
        auto ret = parser_.Feed(buf.data(), static_cast<size_t>(n));
        if (ret == ParseResult::Incomplete) continue;

        if (ret == ParseResult::Error) {
            if (parser_.IsH2()) {
                auto resp = Response::Raw(426,
                    "HTTP/1.1 426 Upgrade Required\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: 48\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                    "HTTP/2 is not supported yet. Use HTTP/1.1.\r\n");
                int code = resp.StatusCode();
                size_t bytes = resp.HeaderWire().size() + resp.BodyWire().size();
                co_await Send(std::move(resp));
                co_await middleware_.ExecutePost(parser_, code, bytes,
                    dur_us(read_start), worker_id_);
            } else {
                auto resp = Response::Error(400, region_);
                int code = resp.StatusCode();
                size_t bytes = resp.HeaderWire().size() + resp.BodyWire().size();
                co_await Send(std::move(resp));
                co_await middleware_.ExecutePost(parser_, code, bytes,
                    dur_us(read_start), worker_id_);
            }
            break;
        }

        // ── Body size check ──
        if (max_body_size_ > 0 && parser_.ContentLength() > max_body_size_) {
            {
                auto resp = Response::Raw(413,
                    "HTTP/1.1 413 Payload Too Large\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: 21\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                    "Payload Too Large\r\n");
                int code = resp.StatusCode();
                size_t bytes = resp.HeaderWire().size() + resp.BodyWire().size();
                co_await Send(std::move(resp));
                co_await middleware_.ExecutePost(parser_, code, bytes,
                    dur_us(read_start), worker_id_);
            }
            break;
        }

        // ── PreRequest phase (sync) ──
        {
            auto pre = middleware_.ExecutePre(parser_);
            if (!pre.IsNone()) {
                int code = pre.StatusCode();
                size_t bytes = pre.HeaderWire().size() + pre.BodyWire().size();
                bool is_stream = pre.IsStream();
                co_await Send(std::move(pre));
                if (is_stream) break;
                co_await middleware_.ExecutePost(parser_, code, bytes,
                    dur_us(read_start), worker_id_);

                auto conn = parser_.Header("connection");
                if (conn == "close") break;
                continue;
            }
        }

        // ── Route → Handler ──
        auto resp = Response::None();
        auto* handler = router_.Match(parser_.Method(), parser_.Path());
        if (handler && handler->IsAsync()) {
            resp = co_await handler->HandleAsync(parser_);
        } else if (handler) {
            resp = handler->Handle(parser_);
        } else {
            resp = Response::Error(404, region_);
        }

        int code = resp.StatusCode();
        size_t bytes = resp.HeaderWire().size() + resp.BodyWire().size()
                     + (resp.IsFile() ? resp.FileSize() : 0);
        bool is_stream = resp.IsStream();
        co_await Send(std::move(resp));

        if (is_stream) break;

        // ── PostResponse phase ──
        co_await middleware_.ExecutePost(parser_, code, bytes,
            dur_us(read_start), worker_id_);

        auto conn = parser_.Header("connection");
        if (conn == "close") break;
    }
    } catch (std::exception& e) {
        std::cerr << "[session] " << e.what() << std::endl;
    }

    if (metrics_) metrics_->OnConnectionClose(worker_id_);
}

template<typename Stream>
asio::awaitable<void> H11Session<Stream>::Send(Response response)
{
    if (response.IsFile())
    {
        co_await async_write(stream_,
            asio::buffer(response.HeaderWire()),
            asio::use_awaitable);

        auto fd = response.Fd();
        auto range_off = response.FileRangeOffset();
        auto remaining = (response.FileRangeLen() > 0)
                       ? response.FileRangeLen()
                       : response.FileSize();

        if constexpr (std::is_same_v<Stream, tcp::socket>)
        {
            off_t offset = static_cast<off_t>(range_off);
            while (remaining > 0) {
                ssize_t n = ::sendfile(stream_.native_handle(), fd,
                                       &offset, remaining);
                if (n <= 0) {
                    if (errno == EAGAIN) continue;
                    break;
                }
                remaining -= static_cast<size_t>(n);
            }
        }
        else
        {
            ::lseek(fd, static_cast<off_t>(range_off), SEEK_SET);
            std::array<char, 65536> readbuf;
            while (remaining > 0) {
                auto to_read = std::min(remaining, readbuf.size());
                ssize_t n = ::read(fd, readbuf.data(), to_read);
                if (n <= 0) break;
                co_await async_write(stream_,
                    asio::buffer(readbuf.data(), static_cast<size_t>(n)),
                    asio::use_awaitable);
                remaining -= static_cast<size_t>(n);
            }
        }
    }
    else if (response.IsStream())
    {
        {
            auto [hec, _h] = co_await async_write(stream_,
                asio::buffer(response.HeaderWire()),
                asio::as_tuple(asio::use_awaitable));
            (void)_h;
            if (hec) co_return;
        }

        auto exec = co_await asio::this_coro::executor;
        auto timer = asio::steady_timer(exec);

        {
            std::string payload = "retry: 2000\n\n";
            if (metrics_)
            {
                auto full_json = metrics_->RenderMetricsJson();
                payload += "event: full\ndata: ";
                payload += full_json;
                payload += "\n\n";
            }
            auto [ec, _] = co_await async_write(stream_,
                asio::buffer(payload),
                asio::as_tuple(asio::use_awaitable));
            (void)_;
            if (ec) co_return;
        }

        int64_t last_ts = 0;
        std::vector<AlertState> prev_alerts;
        if (metrics_) {
            prev_alerts = metrics_->AlertStates();
            last_ts = metrics_->LastFlushTimestamp();
        }
        int push_ms = response.PushIntervalMs();

        for (;;)
        {
            timer.expires_after(std::chrono::milliseconds(push_ms));
            auto [tec] = co_await timer.async_wait(
                asio::as_tuple(asio::use_awaitable));
            if (tec) break;

            if (!metrics_) break;

            auto delta = metrics_->RenderLatestSnapshot(last_ts);
            if (!delta.empty())
            {
                last_ts = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();

                std::string ev = "event: metrics\ndata: ";
                ev += delta;
                ev += "\n\n";

                auto alert_delta = metrics_->RenderAlertDelta(prev_alerts);
                prev_alerts = metrics_->AlertStates();
                ev += alert_delta;
                if (!alert_delta.empty()) ev += "\n";

                auto [wec, _2] = co_await async_write(stream_,
                    asio::buffer(ev),
                    asio::as_tuple(asio::use_awaitable));
                (void)_2;
                if (wec) break;
            }
        }
    }
    else
    {
        auto body = response.BodyWire();
        if (!body.empty())
        {
            std::array<asio::const_buffer, 2> bufs = {{
                asio::buffer(response.HeaderWire()),
                asio::buffer(body)
            }};
            co_await async_write(stream_, bufs, asio::use_awaitable);
        }
        else
        {
            co_await async_write(stream_,
                asio::buffer(response.HeaderWire()),
                asio::use_awaitable);
        }
    }
}

template class H11Session<tcp::socket>;
template class H11Session<asio::ssl::stream<tcp::socket>>;
