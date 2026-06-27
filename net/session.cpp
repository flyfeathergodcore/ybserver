#include "net/session.hpp"
#include <iostream>
#include <sys/sendfile.h>
#include <unistd.h>

using asio::ip::tcp;

template<typename Stream>
Session<Stream>::Session(Stream stream,
                         std::unique_ptr<Context> parser,
                         RequestHandler& handler,
                         MiddlewareChain& middleware)
    : stream_(std::move(stream))
    , parser_(std::move(parser))
    , handler_(handler)
    , middleware_(middleware) {}

template<typename Stream>
asio::awaitable<void> Session<Stream>::Start()
{
    auto self = this->shared_from_this(); // 保持 Session 存活
    std::array<char, 4096> buf;
    try {
    for (;;)
    {
        pool_.Reset();  // 回收上个请求的池内存

        auto [ec, n] = co_await stream_.async_read_some(
            asio::buffer(buf), asio::as_tuple(asio::use_awaitable));
        if (ec) break;

        {   // 原始字节阶段：中间件可在此短路
            auto mw = middleware_.ProcessRaw(buf.data(), static_cast<size_t>(n));
            if (!mw.IsNone()) { co_await Send(std::move(mw)); break; }
        }

        auto ret = parser_->Feed(buf.data(), static_cast<size_t>(n));
        if (ret == ParseResult::Incomplete) continue;
        if (ret == ParseResult::Error) { co_await Send(Response::Error(400)); break; }

        // 注入每个请求的内存池
        parser_->SetPool(&pool_);

        // 标准洋葱链 → handler
        auto resp = middleware_.Execute(*parser_, handler_);
        co_await Send(std::move(resp));

        // keep-alive 检查
        auto conn = parser_->Header("connection");
        if (conn == "close") break;
    }
    } catch (std::exception& e) {
        std::cerr << "[session] " << e.what() << std::endl;
    }
}

template<typename Stream>
asio::awaitable<void> Session<Stream>::Send(Response response)
{
    if (response.IsFile())
    {
        // ── File: 写 Header，然后发送文件体 ──
        co_await async_write(stream_, asio::buffer(response.Headers()),
                             asio::use_awaitable);

        auto fd = response.Fd();
        ::lseek(fd, 0, SEEK_SET);
        auto remaining = response.FileSize();

        if constexpr (std::is_same_v<Stream, tcp::socket>)
        {
            // Plain TCP — 真正的零拷贝 sendfile
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
            // SSL — read + async_write
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
    else if (!response.Body().empty())
    {
        // ── String: 单次 gather-write 发送 headers + body（一个 SSL 记录） ──
        std::array<asio::const_buffer, 2> bufs = {{
            asio::buffer(response.Headers()),
            asio::buffer(response.Body())
        }};
        co_await async_write(stream_, bufs, asio::use_awaitable);
    }
    else
    {
        // ── Raw: 只有 headers（已包含完整响应） ──
        co_await async_write(stream_, asio::buffer(response.Headers()),
                             asio::use_awaitable);
    }
}

template class Session<tcp::socket>;
template class Session<asio::ssl::stream<tcp::socket>>;
