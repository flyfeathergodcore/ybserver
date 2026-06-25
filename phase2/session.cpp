// ─────────────────────────────────────────────────────────────
// Phase 2 — Session 管理（协议无关层）
//
// Sessionmanage 通过 Context 接口解析协议，FileCache 获取静态文件，
// 自身只负责 socket I/O。
// ─────────────────────────────────────────────────────────────

#include <asio.hpp>
#include <iostream>
#include <memory>
#include <array>

#include "httpcontext/context.hpp"
#include "file_cache.hpp"

using asio::ip::tcp;

class Sessionmanage : public std::enable_shared_from_this<Sessionmanage>
{
public:
    Sessionmanage(tcp::socket socket,
                  std::unique_ptr<Context> ctx,
                  const FileCache* cache)
        : socket_(std::move(socket))
        , ctx_(std::move(ctx))
        , cache_(cache) {}

    asio::awaitable<void> Start()
    {
        auto self = shared_from_this();
        std::array<char, 4096> data;

        for (;;)
        {
            auto [ec, n] = co_await socket_.async_read_some(
                asio::buffer(data), asio::as_tuple(asio::use_awaitable));
            if (ec) break;

            auto ret = ctx_->Feed(data.data(), static_cast<size_t>(n));
            if (ret == ParseResult::Incomplete) continue;
            if (ret == ParseResult::Error) {
                co_await Send(ctx_->MakeError(400));
                break;
            }

            co_await HandleRequest();
            auto conn = ctx_->Header("connection");
            if (conn == "close") break;
        }
    }

private:
    asio::awaitable<void> HandleRequest()
    {
        if (ctx_->Method() != "GET") {
            co_await Send(ctx_->MakeError(501));
            co_return;
        }

        std::string norm_path = NormalizePath(ctx_->Path());
        if (norm_path.empty()) {
            co_await Send(ctx_->MakeError(403));
            co_return;
        }

        auto* file = cache_->Get(norm_path);
        if (!file) {
            co_await Send(ctx_->MakeError(404));
            co_return;
        }

        co_await Send(ctx_->MakeResponse(200, file->mime, file->content));
    }

    std::string NormalizePath(std::string_view raw) const
    {
        std::string p(raw);
        if (p.empty() || p[0] != '/') return {};
        if (p.find("..") != std::string::npos) return {};
        if (p.find("//") != std::string::npos) return {};

        if (p.back() == '/') p += "index.html";
        else if (p.size() == 1) p += "index.html";

        return p;
    }

    asio::awaitable<void> Send(const std::string& response)
    {
        co_await async_write(socket_,
            asio::buffer(response), asio::use_awaitable);
    }

    tcp::socket socket_;
    std::unique_ptr<Context> ctx_;
    const FileCache* cache_;
};
