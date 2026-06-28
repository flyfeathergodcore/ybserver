#include "net/session.hpp"
#include "net/region_pool.hpp"
#include "net/metrics.hpp"
#include <iostream>
#include <sys/sendfile.h>
#include <unistd.h>

using asio::ip::tcp;

// ── Helper: microseconds since start ──
static uint64_t dur_us(std::chrono::steady_clock::time_point start)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count());
}

template<typename Stream>
Session<Stream>::Session(Stream stream,
                         RequestHandler& handler,
                         MiddlewareChain& middleware,
                         RegionPool* region_pool)
    : stream_(std::move(stream))
    , handler_(handler)
    , middleware_(middleware)
{
    if (region_pool)
        region_.Init(region_pool);
}

template<typename Stream>
asio::awaitable<void> Session<Stream>::Start()
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

        {   // Raw byte phase
            auto mw = middleware_.ProcessRaw(buf.data(), static_cast<size_t>(n));
            if (!mw.IsNone()) {
                int code = mw.StatusCode();
                size_t bytes = mw.HeaderWire().size() + mw.BodyWire().size();
                co_await Send(std::move(mw));
                auto elapsed = dur_us(read_start);
                if (metrics_) metrics_->OnRequest(elapsed, code, bytes, worker_id_);
                break;
            }
        }

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
                auto elapsed = dur_us(read_start);
                if (metrics_) metrics_->OnRequest(elapsed, code, bytes, worker_id_);
            } else {
                auto resp = Response::Error(400, region_);
                int code = resp.StatusCode();
                size_t bytes = resp.HeaderWire().size() + resp.BodyWire().size();
                co_await Send(std::move(resp));
                auto elapsed = dur_us(read_start);
                if (metrics_) metrics_->OnRequest(elapsed, code, bytes, worker_id_);
            }
            break;
        }

        auto resp = middleware_.Execute(parser_, handler_);
        int code = resp.StatusCode();
        size_t bytes = resp.HeaderWire().size() + resp.BodyWire().size()
                     + (resp.IsFile() ? resp.FileSize() : 0);
        bool is_stream = resp.IsStream();
        co_await Send(std::move(resp));

        if (is_stream) break;  // SSE consumed the connection

        auto elapsed = dur_us(read_start);
        if (metrics_) metrics_->OnRequest(elapsed, code, bytes, worker_id_);

        auto conn = parser_.Header("connection");
        if (conn == "close") break;
    }
    } catch (std::exception& e) {
        std::cerr << "[session] " << e.what() << std::endl;
    }

    if (metrics_) metrics_->OnConnectionClose(worker_id_);
}

template<typename Stream>
asio::awaitable<void> Session<Stream>::Send(Response response)
{
    if (response.IsFile())
    {
        co_await async_write(stream_,
            asio::buffer(response.HeaderWire()),
            asio::use_awaitable);

        auto fd = response.Fd();
        ::lseek(fd, 0, SEEK_SET);
        auto remaining = response.FileSize();

        if constexpr (std::is_same_v<Stream, tcp::socket>)
        {
            off_t offset = 0;
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
        // ── SSE streaming loop ──
        // First, send HTTP response headers (status line, Content-Type, etc.)
        {
            auto [hec, _h] = co_await async_write(stream_,
                asio::buffer(response.HeaderWire()),
                asio::as_tuple(asio::use_awaitable));
            (void)_h;
            if (hec) co_return;
        }

        auto exec = co_await asio::this_coro::executor;
        auto timer = asio::steady_timer(exec);

        // Send initial SSE payload (retry directive + full state in one write)
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
            prev_alerts = metrics_->AlertStates();  // sync with initial push
            last_ts = metrics_->LastFlushTimestamp(); // avoid re-sending full data
        }
        int push_ms = response.PushIntervalMs();

        for (;;)
        {
            timer.expires_after(std::chrono::milliseconds(push_ms));
            auto [tec] = co_await timer.async_wait(
                asio::as_tuple(asio::use_awaitable));
            if (tec) break;

            if (!metrics_) break;

            // Metrics delta
            auto delta = metrics_->RenderLatestSnapshot(last_ts);
            if (!delta.empty())
            {
                last_ts = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();

                std::string ev = "event: metrics\ndata: ";
                ev += delta;
                ev += "\n\n";

                // Alert delta
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

template class Session<tcp::socket>;
template class Session<asio::ssl::stream<tcp::socket>>;
