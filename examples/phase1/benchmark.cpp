// Echo Server 压测客户端
// 编译: g++ -std=c++20 -fcoroutines benchmark.cpp -lpthread -o bench

#include <asio.hpp>
#include <iostream>
#include <atomic>
#include <chrono>
#include <vector>
#include <thread>

using asio::ip::tcp;
using namespace std::chrono;

std::atomic<uint64_t> total_requests{0};
std::atomic<uint64_t> total_errors{0};

asio::awaitable<void> client(int id, int port,
                              int requests, int payload_size) {
    auto executor = co_await asio::this_coro::executor;

    tcp::socket socket(executor);
    try {
        co_await socket.async_connect(
            {asio::ip::make_address("127.0.0.1"), (unsigned short)port},
            asio::use_awaitable);
    } catch (std::exception& e) {
        std::cerr << "[client " << id << "] 连接失败: " << e.what() << std::endl;
        total_errors += requests;
        co_return;
    }

    std::string send_buf(payload_size, 'x');
    std::string recv_buf(payload_size, '\0');

    for (int i = 0; i < requests; ++i) {
        try {
            co_await async_write(socket, asio::buffer(send_buf), asio::use_awaitable);

            size_t total_recvd = 0;
            while (total_recvd < payload_size) {
                auto [ec, n] = co_await socket.async_read_some(
                    asio::buffer(recv_buf.data() + total_recvd,
                                 payload_size - total_recvd),
                    asio::as_tuple(asio::use_awaitable));
                if (ec) throw std::system_error(ec);
                total_recvd += n;
            }

            total_requests++;
        } catch (...) {
            total_errors++;
        }
    }
}

int main(int argc, char** argv) {
    int conns = 10;
    int requests = 1000;
    int payload = 64;
    int port = 3000;

    if (argc >= 2) conns = atoi(argv[1]);
    if (argc >= 3) requests = atoi(argv[2]);
    if (argc >= 4) payload = atoi(argv[3]);
    if (argc >= 5) port = atoi(argv[4]);

    std::cout << "Echo Server 压测" << std::endl;
    std::cout << "  并发连接: " << conns << std::endl;
    std::cout << "  每连接请求: " << requests << std::endl;
    std::cout << "  消息大小: " << payload << " 字节" << std::endl;
    std::cout << "  总请求: " << (conns * requests) << std::endl;
    std::cout << "------------------------" << std::endl;

    asio::io_context ioc(conns);
    auto start = high_resolution_clock::now();

    for (int i = 0; i < conns; ++i) {
        asio::co_spawn(ioc,
            client(i, port, requests, payload),
            asio::detached);
    }

    std::vector<std::jthread> workers;
    for (int i = 0; i < conns - 1; ++i) {
        workers.emplace_back([&ioc] { ioc.run(); });
    }
    ioc.run();

    auto end = high_resolution_clock::now();
    auto ms = duration_cast<milliseconds>(end - start).count();
    auto total = total_requests.load();
    auto errs = total_errors.load();

    std::cout << "------------------------" << std::endl;
    std::cout << "完成: " << total << " 请求" << std::endl;
    std::cout << "错误: " << errs << std::endl;
    std::cout << "耗时: " << ms << " 毫秒" << std::endl;
    std::cout << "QPS: " << (total * 1000 / ms) << " req/s" << std::endl;
    std::cout << "延迟(平均): " << (ms * 1000 / total) << " μs" << std::endl;

    return 0;
}
