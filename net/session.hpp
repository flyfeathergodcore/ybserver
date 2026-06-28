#pragma once
#include <asio.hpp>
#include <asio/ssl.hpp>
#include <memory>
#include <array>
#include "http/h1_parser.hpp"
#include "net/response.hpp"
#include "net/session_region.hpp"
#include "handler/request_handler.hpp"
#include "middleware/middleware.hpp"

class RegionPool;
class MetricsCollector;

using asio::ip::tcp;

template<typename Stream>
class Session : public std::enable_shared_from_this<Session<Stream>> {
public:
    Session(Stream stream,
            RequestHandler& handler,
            MiddlewareChain& middleware,
            RegionPool* region_pool = nullptr);

    asio::awaitable<void> Start();

    // Reuse Session shell: replace stream, reuse existing parser.
    // Feed() already resets all parser state internally.
    // region_ retains its region (released by the next Start()).
    void Reset(Stream stream) {
        stream_ = std::move(stream);
    }

    /// Per-connection memory region (from Worker's RegionPool).
    SessionRegion& Region() { return region_; }

    /// Attach metrics collector (called by MultiServer after construction or pool reuse).
    void SetMetrics(MetricsCollector* mc, int wid) {
        metrics_ = mc;
        worker_id_ = wid;
    }

private:
    asio::awaitable<void> Send(Response response);

    Stream stream_;
    H1Parser parser_;
    RequestHandler& handler_;
    MiddlewareChain& middleware_;
    SessionRegion region_;
    MetricsCollector* metrics_ = nullptr;
    int worker_id_ = -1;
};
