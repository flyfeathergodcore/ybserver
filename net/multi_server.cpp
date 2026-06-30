#include "net/multi_server.hpp"
#include "http/http2/session.hpp"
#include "ssl/tls_context.hpp"
#include <iostream>
#include <memory>
#include <sys/socket.h>
#include <pthread.h>
#include <csignal>

MultiServer::MultiServer(const Config& cfg,
                         Router& router,
                         MiddlewareManager& middleware,
                         std::shared_ptr<TlsContext> tls,
                         std::shared_ptr<MetricsCollector> metrics)
    : ServerBase(cfg, router, middleware, std::move(tls))
    , metrics_(std::move(metrics)) {}

void MultiServer::EnableReusePort(tcp::acceptor& acceptor)
{
    int fd = acceptor.native_handle();
    if (fd != -1) {
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    }
}

void MultiServer::Start()
{
    auto port = endpoint_.port();
    std::cout << "[server] HTTPS 监听 " << cfg_.host << ":" << port
              << " (" << cfg_.threads << " workers, SO_REUSEPORT)" << std::endl;

    std::cout << "[server] 指标收集已启用, " << cfg_.threads << " workers" << std::endl;

    workers_.reserve(static_cast<size_t>(cfg_.threads));

    for (int i = 0; i < cfg_.threads; ++i)
    {
        auto w = std::make_unique<Worker>();

        w->acceptor = std::make_unique<tcp::acceptor>(w->ioctx);
        w->acceptor->open(endpoint_.protocol());
        EnableReusePort(*w->acceptor);
        w->acceptor->set_option(tcp::acceptor::reuse_address(true));
        w->acceptor->bind(endpoint_);
        w->acceptor->listen();

        w->pool = std::make_unique<H11SessionPool>();

        asio::co_spawn(w->ioctx, Listen(*w), asio::detached);

        asio::co_spawn(w->ioctx, FlushLoop(i), asio::detached);

        w->thread = std::jthread([w = w.get(), cpu_id = i,
                                   affinity = cfg_.cpu_affinity] {
            if (affinity) {
                cpu_set_t cpuset;
                CPU_ZERO(&cpuset);
                CPU_SET(cpu_id, &cpuset);
                int rc = ::pthread_setaffinity_np(::pthread_self(),
                                                   sizeof(cpuset), &cpuset);
                if (rc != 0 && cpu_id == 0)
                    std::cerr << "[server] 警告: pthread_setaffinity_np 失败 ("
                              << rc << "), 跳过 CPU 亲和性" << std::endl;
            }
            w->ioctx.run();
        });

        workers_.push_back(std::move(w));
    }

    // ── Signal handling for graceful shutdown ──
    ::signal(SIGPIPE, SIG_IGN);  // ignore SIGPIPE globally
    asio::signal_set sigset(workers_[0]->ioctx, SIGINT, SIGTERM);
    sigset.async_wait([this](std::error_code ec, int sig) {
        if (ec || shutdown_.exchange(true)) return;
        std::cout << "\n[server] 收到信号 " << sig
                  << "，开始优雅关闭（最多 " << kDrainTimeoutSec << " 秒）..."
                  << std::endl;

        // Step 1: close all acceptors — no new connections
        for (auto& w : workers_) {
            if (w->acceptor && w->acceptor->is_open())
                w->acceptor->close();
        }

        // Step 2: drain timer — force stop after timeout
        auto timer = std::make_shared<asio::steady_timer>(workers_[0]->ioctx);
        timer->expires_after(std::chrono::seconds(kDrainTimeoutSec));
        timer->async_wait([this, timer](std::error_code tec) {
            if (tec) return;
            std::cout << "[server] 排水超时，强制关闭所有连接..." << std::endl;
            for (auto& w : workers_)
                w->ioctx.stop();
        });
    });

    // ── Wait for all workers to exit ──
    for (auto& w : workers_) {
        w->thread.join();
    }
    std::cout << "[server] 已完全停止" << std::endl;
}

asio::awaitable<void> MultiServer::Listen(Worker& worker)
{
    int this_id = 0;
    for (size_t i = 0; i < workers_.size(); i++) {
        if (workers_[i].get() == &worker) { this_id = static_cast<int>(i); break; }
    }

    auto exec = co_await asio::this_coro::executor;
    while (!shutdown_)
    {
        try
        {
            auto socket = co_await worker.acceptor->async_accept(
                asio::use_awaitable);

            asio::co_spawn(exec,
                [this, socket = std::move(socket), &worker, this_id]() mutable
                    -> asio::awaitable<void>
                {
                    asio::ssl::stream<tcp::socket> ss(
                        std::move(socket), tls_->NativeContext());

                    auto [ec] = co_await ss.async_handshake(
                        asio::ssl::stream_base::server,
                        asio::as_tuple(asio::use_awaitable));
                    if (ec) co_return;

                    // ── ALPN dispatch ──
                    if (TlsContext::IsHttp2(ss.native_handle()))
                    {
                        auto session = std::make_shared<H2Session>(
                            std::move(ss), router_, middleware_,
                            &worker.region_pool);
                        session->SetMaxBodySize(cfg_.max_body_size);
                        session->SetMetrics(metrics_.get(), this_id);
                        co_await session->Start();
                    }
                    else
                    {
                        auto session = worker.pool->TryAcquireSession();
                        if (session) {
                            session->Reset(std::move(ss));
                            session->Region().Init(&worker.region_pool);
                            session->SetMaxBodySize(cfg_.max_body_size);
                            session->SetMetrics(metrics_.get(), this_id);
                        } else {
                            session = std::make_shared<
                                H11Session<asio::ssl::stream<tcp::socket>>>(
                                std::move(ss),
                                router_, middleware_, &worker.region_pool);
                            session->SetMaxBodySize(cfg_.max_body_size);
                            session->SetMetrics(metrics_.get(), this_id);
                        }

                        co_await session->Start();
                        worker.pool->ReleaseSession(std::move(session));
                    }
                },
                asio::detached);
        }
        catch (std::exception& e)
        {
            if (shutdown_) break;
            std::cerr << "[server] " << e.what() << std::endl;
        }
    }
    if (shutdown_)
        std::cout << "[server] 监听已停止" << std::endl;
}

asio::awaitable<void> MultiServer::FlushLoop(int worker_id)
{
    auto exec = co_await asio::this_coro::executor;
    auto timer = asio::steady_timer(exec);
    while (!shutdown_)
    {
        timer.expires_after(std::chrono::seconds(1));
        auto [ec] = co_await timer.async_wait(
            asio::as_tuple(asio::use_awaitable));
        if (ec || shutdown_) break;
        metrics_->Flush(worker_id);
    }
}
