# C++20 Coroutine + Asio HTTP Server

基于 C++20 协程和 Asio 库的异步 HTTP 静态文件服务器。

## 特点

- **C++20 协程** — 使用 `co_await` 编写异步代码，无需回调嵌套
- **Asio Proactor** — 基于 epoll 的高性能事件循环
- **llhttp 解析** — Node.js 官方 HTTP 解析器，安全可靠
- **文件缓存** — 启动时预加载文件到内存，零磁盘 I/O
- **YAML 配置** — 通过 `config.yaml` 配置端口、线程数等
- **多线程** — 共享 io_context 的多线程事件循环
- **SQLite 集成**（Phase 3） — 协程友好的异步数据库封装

## 快速开始

```bash
# 安装依赖
sudo apt install libasio-dev libyaml-cpp-dev libsqlite3-dev

# 构建
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make http_server -j$(nproc)

# 运行
./http_server

# 用默认参数运行（www 目录，8081 端口，4 线程）
```

## 配置

编辑 `config.yaml`：

```yaml
server:
  host: "0.0.0.0"
  port: 8081
  threads: 4
  doc_root: "./www"
```

## 基准测试

4 线程、100 并发连接、164B 静态文件：

```
Requests/sec:  122,307
Latency avg:   0.78ms
Transfer:      29.51 MB/s
```

## 项目结构

```
├── phase1/          协程入门 + Echo Server
├── phase2/          HTTP 静态文件服务器（当前）
│   ├── httpcontext/ HTTP 解析器（llhttp + 手写版）
│   ├── config.yaml  服务器配置
│   ├── file_cache   文件缓存
│   └── session.cpp  连接管理
├── phase3/          数据库封装（SQLite + 协程）
│   ├── database     异步数据库封装
│   └── connection_pool 协程连接池
└── www/             测试用静态文件
```

## 学习路线

查看 [cpp-coroutine-network-learning-path.md](cpp-coroutine-network-learning-path.md) 了解完整学习路线。

详细技术总结见 [learn-summary.md](learn-summary.md)。

## 许可

MIT
