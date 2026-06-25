# C++20 协程 + Asio 网络编程学习路线

> 目标：从 C++14 基础 → C++20 协程 → 网络库 → 部署网站后端
> 
> 前置基础：C++14，使用过 muduo，理解 Reactor 模型
> 
> 路线：Asio + 协程（方案 A）

---

## 第一阶段：环境搭建 + 协程版 Echo Server（3 天）✅

> 已完成，详细总结见 [learn-summary.md](learn-summary.md)

### Day 1：环境搭建与 Asio 协程示例

**安装 Asio（standalone 模式，不需要 Boost）：**

```bash
# 方式一：包管理器安装（推荐）
sudo apt install libasio-dev

# 方式二：源码安装（获取最新版）
wget https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-30-2.tar.gz
tar xzf asio-1-30-2.tar.gz
sudo cp -r asio-1-30-2/include/* /usr/local/include/
```

**验证环境：**

```cpp
// 01_check_compiler.cpp
#include <iostream>
#include <coroutine>

int main() {
    std::cout << "__cplusplus: " << __cplusplus << std::endl;
#if __has_include(<coroutine>)
    std::cout << "<coroutine> is available" << std::endl;
#else
    std::cout << "<coroutine> is NOT available (need GCC>=11 / Clang>=14)" << std::endl;
#endif
    return 0;
}
```

```bash
g++ -std=c++20 -fcoroutines 01_check_compiler.cpp -o check
```

**要求：** GCC 11+ / Clang 14+，支持 `-std=c++20 -fcoroutines`（GCC）或 `-std=c++20 -stdlib=libc++`（Clang）。

**检查 Asio 头文件：**

```bash
ls /usr/include/asio.hpp || echo "Asio not found at /usr/include"
ls /usr/local/include/asio.hpp || echo "Asio not found at /usr/local/include"
```

---

### Day 2：跑通 Asio 官方协程示例

Asio 自带了协程示例，先跑通官方版本确保环境正确。

**编译 Asio 的协程 echo server：**

```bash
cd /workspace
cp /usr/share/doc/asio/examples/cpp20/echo/echo_server.cpp .
# 如果包管理器没复制示例，直接去 GitHub 下载：
# https://raw.githubusercontent.com/chriskohlhoff/asio/master/asio/src/examples/cpp20/echo/echo_server.cpp
```

```bash
g++ -std=c++20 -fcoroutines \
    -I/usr/include \
    echo_server.cpp \
    -lpthread \
    -o echo_server
```

**验证：**

```bash
./echo_server &
nc localhost 3000
# 输入任意内容，应该原样返回
```

**关键要搞懂的问题（读完代码后问自己）：**
1. `co_await` 挂起时，线程在做什么？（答：线程退回 event loop 处理其他 socket）
2. 对比 muduo 的回调版 echo server——两版的代码结构和执行流程有什么不同？
3. `use_awaitable` 是什么？它把 Asio 的异步操作变成了什么？

---

### Day 3：自己手写一个协程版 Echo Server

不要复制官方代码，自己从零写一遍。

```cpp
// 03_my_echo_server.cpp
#include <asio.hpp>
#include <iostream>

using asio::ip::tcp;

asio::awaitable<void> session(tcp::socket socket) {
    std::array<char, 1024> data;
    for (;;) {
        auto [ec, n] = co_await socket.async_read_some(
            asio::buffer(data), asio::as_tuple(asio::use_awaitable));
        if (ec == asio::error::eof) break;    // 客户端断开
        if (ec) co_return;                     // 其他错误
        co_await async_write(socket, 
            asio::buffer(data, n), asio::use_awaitable);
        std::cout << "Echoed " << n << " bytes" << std::endl;
    }
}

asio::awaitable<void> listener() {
    auto executor = co_await asio::this_coro::executor;
    tcp::acceptor acceptor(executor, {tcp::v4(), 3000});
    for (;;) {
        auto socket = co_await acceptor.async_accept(asio::use_awaitable);
        // 每个连接启动一个协程会话，不阻塞 acceptor
        asio::co_spawn(executor, session(std::move(socket)), asio::detached);
    }
}

int main() {
    asio::io_context ioctx(1);  // 单线程 event loop
    asio::co_spawn(ioctx, listener(), asio::detached);
    ioctx.run();
    return 0;
}
```

**编译运行：**

```bash
g++ -std=c++20 -fcoroutines -I/usr/include 03_my_echo_server.cpp -lpthread -o my_echo
./my_echo
```

**用 Wireshark / tcpdump 观察（可选但推荐）：**

```bash
# 本机回环抓包
sudo tcpdump -i lo port 3000 -X
# 另一个终端连上去
nc localhost 3000
# 观察三次握手、数据传输、四次挥手
```

**Day 3 结束时你能回答的问题：**
- 协程挂起时，栈上的局部变量去哪了？
- 如果 1000 个客户端同时连接，会有多少协程？多少线程？
- 一个协程阻塞在 `co_await` 上时，其他协程还能运行吗？

---

## 第一阶段验收标准

✅ Asio 安装正确，协程示例能编译运行
✅ 自己手写的 echo server 能跑通
✅ 用 `nc` 能连上并成功 echo
✅ 回答出上面三个关键问题

---

## 第二阶段：HTTP 解析 + 静态文件服务器（1 周）✅

> 已完成，手写解析器 + llhttp 替换 + FileCache + 多线程，详细总结见 [learn-summary.md](learn-summary.md)

### Day 4-5：集成 HTTP 解析器

**选择方案：**

| 方案 | 说明 | 难度 |
|---|---|---|
| 用 llhttp | Node.js 官方解析器，C 语言，高性能 | 低 |
| 手写简单解析器 | 只支持 HTTP/1.1 GET 请求，约 200 行 | 中 |
| 用 Boost.Beast | 功能全，但有 Boost 依赖 | 低 |

**推荐顺序：先手写一个极简解析器理解协议，再换成 llhttp 或 Beast。**

**极简 HTTP 请求解析（你可以在 200 行内实现）：**

```cpp
// 04_http_parser.h
struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::map<std::string, std::string> headers;
    std::string body;
};

// 解析结果
enum class ParseResult { Incomplete, Complete, Error };
ParseResult parse_http_request(HttpRequest& req, std::string_view data);
```

**不需要支持全部 HTTP 协议，只支持：**
- `GET /path HTTP/1.1`
- 几个基本头：Host、Connection、User-Agent
- 忽略请求体（GET 没有 body）

**集成到协程 session 中：**

```cpp
asio::awaitable<void> http_session(tcp::socket socket) {
    std::string buf;
    HttpRequest req;
    for (;;) {
        // 从 socket 读数据
        auto [ec, n] = co_await socket.async_read_some(
            asio::buffer(tmp), asio::as_tuple(asio::use_awaitable));
        if (ec) co_return;
        buf.append(tmp.data(), n);
        
        // 尝试解析 HTTP 请求
        auto result = parse_http_request(req, buf);
        if (result == ParseResult::Complete) {
            co_await handle_request(socket, req);
            buf.clear();
            // TODO: 支持 keep-alive
        } else if (result == ParseResult::Error) {
            co_await send_error(socket, 400);
            co_return;
        }
        // Incomplete: 继续读
    }
}
```

### Day 6-7：实现静态文件服务

**核心功能：**

```cpp
asio::awaitable<void> handle_get(tcp::socket& socket, 
                                  const std::string& doc_root,
                                  const std::string& path) {
    // 1. 安全检查——防止路径穿越（../etc/passwd）
    // 2. 拼接文件路径
    // 3. 打开文件
    // 4. 构造 HTTP 响应头
    // 5. co_await async_write 发送响应
}
```

**HTTP 响应格式：**

```http
HTTP/1.1 200 OK\r\n
Content-Type: text/html\r\n
Content-Length: 128\r\n
Connection: keep-alive\r\n
\r\n
<body content...>
```

**需要处理：**
- ✅ 200 OK + 文件内容
- ✅ 404 Not Found
- ✅ 403 Forbidden（文件名以 `.` 开头等）
- ✅ MIME 类型映射（`.html` → `text/html`, `.css` → `text/css` 等）
- ✅ 路径穿越防护

**数据结构：**

```cpp
std::unordered_map<std::string, std::string> mime_types = {
    {".html", "text/html"},
    {".css", "text/css"},
    {".js", "application/javascript"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    // ...
};
```

**调试技巧：**

```bash
# 启动服务器
./http_server /path/to/doc_root 8080

# 用 curl 测试
curl -v http://localhost:8080/index.html
curl -v http://localhost:8080/../etc/passwd  # 应该返回 403

# 用浏览器打开
firefox http://localhost:8080/index.html
```

### Day 7 晚：支持 Keep-Alive

这是你从 muduo 带过来的概念——HTTP 长连接。

在协程下实现 keep-alive 非常简单，因为不用注册回调：

```cpp
asio::awaitable<void> http_session(tcp::socket socket) {
    // 外层 while 循环处理 keep-alive
    while (socket.is_open()) {
        HttpRequest req;
        auto result = co_await read_http_request(socket, req);
        if (result != ParseResult::Complete) break;
        
        co_await handle_request(socket, req);
        
        // 检查 Connection 头
        if (req.headers["Connection"] == "close") break;
    }
}
```

对比 muduo 的 Keep-Alive 实现——回调版需要在 `messageCallback` 里判断是否继续注册回调，协程版就是一个 while 循环。

---

## 第二阶段验收标准

✅ 能解析 HTTP GET 请求
✅ 能返回 HTML/CSS/JS/图片等静态文件
✅ 正确处理 404、403
✅ 防止路径穿越攻击
✅ 支持 HTTP Keep-Alive
✅ 用浏览器能访问到你的页面

---

## 第三阶段：多线程 + 数据库（1 周）

### Day 8-9：多线程 io_context 池

**从单线程扩展到多线程：**

```cpp
int main(int argc, char* argv[]) {
    int thread_count = std::thread::hardware_concurrency();
    asio::io_context ioc(thread_count);
    
    // 工作线程池
    std::vector<std::jthread> workers;
    for (int i = 0; i < thread_count - 1; ++i) {
        workers.emplace_back([&ioc] { ioc.run(); });
    }
    
    asio::co_spawn(ioc, listener(), asio::detached);
    ioc.run();  // 主线程也参与
}
```

**⚠️ 协程线程安全要点：**

多个线程同时 `ioc.run()` 时，协程可能在哪个线程上恢复是不确定的。如果你在协程中操作了共享数据（比如计数器），需要保护。

**Asio 的解决方案：`strand`**

```cpp
asio::strand<asio::io_context::executor_type> strand(ioc.get_executor());

// 在 strand 上启动协程——保证同一时刻只有一个协程在这个 strand 上执行
asio::co_spawn(strand, my_coroutine(), asio::detached);
```

**对比 muduo：**
- muduo 的一个 EventLoop 绑定一个线程——每个线程上的操作自然安全
- Asio 的 io_context 可以被多线程 run——你需要自己决定用 strand 还是单线程

### Day 10-11：接入数据库

**方案选择：**

| 数据库 | C++ 客户端 | 说明 |
|---|---|---|
| SQLite | 自带 C API，或用 sqlpp11 | 文件数据库，零配置，适合学习 |
| Redis | redis-plus-plus | 内存 KV，适合做缓存/会话 |
| MySQL | mysql-connector-cpp | 需要 MySQL 服务 |

**推荐：SQLite 起步，简单直接。**

```bash
sudo apt install libsqlite3-dev
```

**协程版 SQLite 查询：**

```cpp
// 注意：SQLite 的 C API 是同步的
// 我们需要把它包装成协程版——丢到单独线程执行
asio::awaitable<std::vector<User>> query_users(asio::io_context& ioc, 
                                                sqlite3* db) {
    std::vector<User> result;
    
    // co_await on io_context to run in thread pool
    co_await asio::post(ioc, asio::use_awaitable);
    
    // 这里在某个工作线程上执行同步 SQLite 查询
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "SELECT id, name FROM users", -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        result.push_back({
            sqlite3_column_int(stmt, 0),
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1))
        });
    }
    sqlite3_finalize(stmt);
    
    co_return result;
}
```

### Day 11 晚：协程连接池

**实现一个简单的协程连接池：**

```cpp
template<typename T>
class ConnectionPool {
    std::vector<std::unique_ptr<T>> connections;
    std::queue<T*> available;
    asio::io_context& ioc;
    
public:
    // 协程版 acquire——如果没有可用连接就挂起等待
    asio::awaitable<T*> acquire() {
        while (available.empty()) {
            // 等待有连接归还
            // 可以用 asio::condition_variable 或简单的 channel
            co_await wait_for_connection();
        }
        auto* conn = available.front();
        available.pop();
        co_return conn;
    }
    
    void release(T* conn) {
        available.push(conn);
        // 通知等待者
        notify_one();
    }
};
```

**用法：**

```cpp
asio::awaitable<void> handle_request(tcp::socket& socket, HttpRequest& req) {
    auto conn = co_await pool.acquire();
    auto result = co_await conn->query("SELECT ...");
    pool.release(conn);
    co_await send_response(socket, result);
}
```

---

## 第三阶段验收标准

✅ 多线程 io_context 能处理更多并发连接
✅ 理解 strand 解决了什么问题
✅ SQLite 数据库查询能跑通
✅ 连接池正常工作，高并发下不崩溃

---

## 第四阶段：部署上线（3 天）

### Day 12：Nginx 反向代理

**为什么要 Nginx 反代？**

| 问题 | 解决方案 |
|---|---|
| C++ 服务不能直接暴露 80/443 端口（需要 root） | Nginx 监听 80，转发到你的端口 |
| 需要 TLS 终止 | Nginx 处理 HTTPS，后端只走 HTTP |
| 静态文件 | Nginx 直接服务静态文件，效率更高 |
| 负载均衡 | 未来扩展时 Nginx 做负载分发 |

**Nginx 配置：**

```nginx
# /etc/nginx/sites-available/cpp-server
upstream cpp_backend {
    server 127.0.0.1:8080;
    # future: server 127.0.0.1:8081;
}

server {
    listen 80;
    server_name your-domain.com;

    # 静态文件由 Nginx 直接处理
    location /static/ {
        alias /var/www/myapp/static/;
        expires 7d;
    }

    # 动态请求转发到 C++ 后端
    location / {
        proxy_pass http://cpp_backend;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    }
}
```

### Day 13：systemd 进程管理

```ini
# /etc/systemd/system/cpp-server.service
[Unit]
Description=C++ HTTP Server
After=network.target

[Service]
Type=simple
User=www-data
WorkingDirectory=/opt/cpp-server
ExecStart=/opt/cpp-server/bin/http_server /var/www/myapp 8080
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable cpp-server
sudo systemctl start cpp-server
sudo systemctl status cpp-server   # 查看状态
sudo journalctl -u cpp-server -f   # 实时查看日志
```

### Day 14：Docker 化

```dockerfile
# Dockerfile
# 多阶段构建
FROM gcc:13 AS builder
WORKDIR /src
COPY . .
RUN g++ -std=c++20 -fcoroutines -O2 -I/include \
    http_server.cpp -lpthread -lsqlite3 -o http_server

# 运行镜像——只有 几 MB 的二进制
FROM debian:stable-slim
RUN apt update && apt install -y libsqlite3-0 ca-certificates
COPY --from=builder /src/http_server /app/
COPY --from=builder /src/www /var/www/
EXPOSE 8080
CMD ["/app/http_server", "/var/www", "8080"]
```

```bash
docker build -t cpp-server .
docker run -d -p 8080:8080 --name myapp cpp-server
```

**Docker Compose（如果你要用 Redis/MySQL）：**

```yaml
# docker-compose.yml
version: '3'
services:
  app:
    build: .
    ports:
      - "8080:8080"
    depends_on:
      - redis
  redis:
    image: redis:7
    volumes:
      - redis_data:/data
  nginx:
    image: nginx:alpine
    ports:
      - "80:80"
    volumes:
      - ./nginx.conf:/etc/nginx/conf.d/default.conf
    depends_on:
      - app

volumes:
  redis_data:
```

---

## 第四阶段验收标准

✅ Nginx 反代正常工作，访问 80 端口能到你的 C++ 服务
✅ systemd 管理下，服务崩溃后自动重启
✅ Docker 镜像能构建并启动
✅ `curl http://localhost` 返回你的页面

---

## 附录

### 推荐阅读顺序

| 阶段 | 阅读材料 |
|---|---|
| 预备 | [C++ Coroutines: Understanding the promise model](https://www.scs.stanford.edu/~dm/blog/c++-coroutines.html) |
| 第一阶段 | [Asio 官方文档 - Coroutines](https://think-async.com/Asio/asio-1.30.2/doc/asio/overview/model/coroutines.html) |
| 第二阶段 | [HTTP/1.1 协议规范 (RFC 7230)](https://datatracker.ietf.org/doc/html/rfc7230) — 只读 Section 2-3 |
| 第三阶段 | [Asio 文档 - Strands](https://think-async.com/Asio/asio-1.30.2/doc/asio/overview/core/strands.html) |
| 第四阶段 | [Nginx 反向代理配置指南](https://nginx.org/en/docs/http/ngx_http_proxy_module.html) |

### 常见陷阱

1. **忘记 `asio::use_awaitable`**：编译不报错，但行为不对——每次 co_await 会阻塞线程而不是挂起
2. **协程中抛异常未捕获**：协程中的异常会通过 `promise.unhandled_exception()` 传播，如果你用 `asio::detached` 会丢到 `io_context` 的异常处理
3. **误用 `co_await` 在回调中**：普通函数里不能用 `co_await`，它只能在协程函数体中使用
4. **单线程 io_context 处理数据库查询**：SQLite 同步查询会阻塞整个 event loop——要么用 `asio::post` 丢到线程池，要么用 SQLite 的 WAL 模式 + 单独线程
5. **Docker 中端口绑定**：生产环境中注意用 `EXPOSE` 和 `-p` 的对应，以及 C++ 服务监听 `0.0.0.0` 而不是 `127.0.0.1`

### 推荐工具

| 工具 | 用途 |
|---|---|
| Wireshark | 观察 TCP/HTTP 协议细节 |
| curl -v | HTTP 调试（查看请求响应头） |
| telnet / nc | 原始 TCP 连接测试 |
| heaptrack | 检测协程帧泄漏 |
| perf top | 性能热点分析 |

---

> 文件位置：`/workspace/cpp-coroutine-network-learning-path.md`
>
> 每完成一个阶段，可以在旁边打勾记录进度。遇到具体问题随时问。
