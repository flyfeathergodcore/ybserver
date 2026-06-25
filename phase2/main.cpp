// ─────────────────────────────────────────────────────────────
// Phase 2 — HTTP 静态文件服务器
//
// 从 config.yaml 读取配置，预加载文件到缓存，
// 多线程事件循环处理请求。
// ─────────────────────────────────────────────────────────────

#include <asio.hpp>
#include <iostream>
#include <memory>
#include <vector>
#include <thread>

#include "config.hpp"
#include "file_cache.hpp"
#include "httpcontext/llhttp_context.hpp"
#include "session.cpp"

using asio::ip::tcp;

asio::awaitable<void> listener(const FileCache* cache, const Config& cfg)
{
    auto executor = co_await asio::this_coro::executor;
    tcp::acceptor acceptor(executor, {asio::ip::make_address(cfg.host), cfg.port});
    std::cout << "[server] 监听 " << cfg.host << ":" << cfg.port << std::endl;

    for (;;)
    {
        auto socket = co_await acceptor.async_accept(asio::use_awaitable);

        auto ctx = std::make_unique<LlhttpContext>();
        auto sess = std::make_shared<Sessionmanage>(
            std::move(socket), std::move(ctx), cache);

        asio::co_spawn(executor, sess->Start(), asio::detached);
    }
}

int main(int argc, char* argv[])
{
    try
    {
        std::string cfg_path = argc > 1 ? argv[1] : "./config.yaml";
        Config cfg = Config::Load(cfg_path);

        FileCache cache;
        cache.LoadDirectory(cfg.doc_root);

        asio::io_context ioctx;
        asio::executor_work_guard<asio::io_context::executor_type>
            work_guard(ioctx.get_executor());

        asio::co_spawn(ioctx, listener(&cache, cfg), asio::detached);

        std::vector<std::jthread> workers;
        for (int i = 0; i < cfg.threads - 1; ++i)
            workers.emplace_back([&ioctx] { ioctx.run(); });

        std::cout << "[server] " << cfg.threads << " 个工作线程启动"
                  << std::endl;
        ioctx.run();
    }
    catch (std::exception& e)
    {
        std::cerr << "致命错误: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
