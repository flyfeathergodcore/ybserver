// Phase 1 - Day 2: 协程版 TCP Echo Server
// 编译: g++ -std=c++20 -fcoroutines 01_echo_server.cpp -lpthread -o echo_server

#include <asio.hpp>
#include <iostream>
#include <array>

using asio::ip::tcp;

// ── 一个连接的处理逻辑 ──
// 每个客户端连接进来，会 spawn 一个 session 协程
asio::awaitable<void> session(tcp::socket socket) {
    std::array<char, 1024> data;

    for (;;) {
        // 挂起：等待 socket 有数据可读
        // 数据到了，线程自动恢复这个协程
        auto [ec, n] = co_await socket.async_read_some(
            asio::buffer(data), asio::as_tuple(asio::use_awaitable));

        if (ec == asio::error::eof) {
            std::cout << "[session] 客户端断开连接" << std::endl;
            break;  // 正常断开
        }
        if (ec) {
            std::cerr << "[session] 读错误: " << ec.message() << std::endl;
            co_return;  // 异常断开
        }

        std::cout << "[session] 收到 " << n << " 字节，原样返回" << std::endl;

        // 挂起：等待数据写完
        co_await async_write(socket,
            asio::buffer(data, n), asio::use_awaitable);
    }
}

// ── 监听端口，接受连接 ──
// 这也是一个协程——永不结束
asio::awaitable<void> listener() {
    // 获取当前 io_context 的执行器
    auto executor = co_await asio::this_coro::executor;

    // 绑定端口 3000
    tcp::acceptor acceptor(executor, {tcp::v4(), 3000});
    std::cout << "[listener] Echo 服务器启动，监听端口 3000" << std::endl;

    for (;;) {
        // 挂起：等待新连接
        // 有客户端连上来时，自动恢复
        auto socket = co_await acceptor.async_accept(asio::use_awaitable);
        std::cout << "[listener] 新客户端连接" << std::endl;

        // 每个连接启动一个独立的协程会话
        // detached 表示"不关心返回值，让它自己跑"
        asio::co_spawn(executor, session(std::move(socket)), asio::detached);
    }
}

int main() {
    try {
        // 单线程事件循环
        asio::io_context ioctx(1);

        // 启动 listener 协程
        asio::co_spawn(ioctx, listener(), asio::detached);

        // 启动事件循环——阻塞在这里，直到 io_context 停止
        ioctx.run();
    } catch (std::exception& e) {
        std::cerr << "致命错误: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
