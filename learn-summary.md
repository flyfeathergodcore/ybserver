# C++20 协程 + Asio 网络编程 — 深入理解总结

> 学习阶段：Phase 1-2 (协程原理 → Asio 架构 → HTTP 服务器)
> 前置知识：C++14，muduo，Reactor 模型

---

## 一、C++20 协程核心概念

### 1.1 协程函数 vs Awaitable 对象

```
协程函数（coroutine function）：              被 co_await 的对象（Awaitable）：
    Task hello() {                                 co_await some_object
        co_return;          ← 函数体里有 co_xxx         ↑
        }                                           这个类型需要
    编译器特殊处理它                                 await_ready/suspend/resume
    编译器看返回类型，找 promise_type
```

- **协程函数** = 函数体内有 `co_await` / `co_return` / `co_yield` → 需要 `promise_type`
- **Awaitable** = 被 `co_await` 的对象 → 需要三个方法：`await_ready` / `await_suspend` / `await_resume`
- 一个类型可以同时扮演两个角色（如 `asio::awaitable<T>`）

### 1.2 `co_await` 展开流程

```cpp
auto result = co_await expr;

// 编译器展开为：
if (!expr.await_ready()) {         // ① 准备好没有？
    expr.await_suspend(handle);    // ② 没准备好，挂起
    // ⑦ 将来被恢复
}
auto result = expr.await_resume(); // ③ 恢复后拿到返回值
```

### 1.3 `promise_type` 的职责

| 方法 | 作用 |
|------|------|
| `get_return_object()` | 协程的返回值（构造返回给调用者的对象） |
| `initial_suspend()` | 创建后立即挂起（suspend_always/never） |
| `final_suspend()` | 结束后的行为 |
| `return_void/value()` | `co_return` 时存值 |
| `unhandled_exception()` | 捕获协程内异常 |
| `await_transform()` | 拦截 `co_await`，支持特殊操作 |

---

## 二、Asio 协程架构（核心设计）

### 2.1 三层封装

```
你的代码:  co_await socket.async_read_some(...)
               │
               ▼
┌─────────────────────────────────┐
│ asio::awaitable<T>              │ ← 外壳（8 字节，一个指针 frame_）
│  await_ready()    = false       │
│  await_suspend()  = push_frame  │
│  await_resume()   = frame->get  │
└─────────────┬───────────────────┘
              │
┌─────────────▼───────────────────┐
│ awaitable_frame<T> (promise_type)│ ← 真正的协程帧（堆上）
│  coro_          → 标准库 handle  │
│  caller_        → 链表前驱      │
│  attached_thread_ → 执行令牌    │
│  push_frame() / pop_frame()    │
└─────────────┬───────────────────┘
              │
┌─────────────▼───────────────────┐
│ awaitable_thread                │ ← 虚拟线程引擎
│  pump() { do resume() while }   │
│  bottom_of_stack_ (入口帧)      │
└─────────────────────────────────┘
```

### 2.2 入口帧（Entry Frame）

- 每次 `co_spawn` 时 Asio **偷偷创建**的一个额外协程帧
- 类型：`awaitable_frame<awaitable_thread_entry_point, Executor>`
- 作用是**虚拟线程的根锚点**，持有全局状态：
  - `top_of_stack_`：当前栈顶指针
  - `u_.executor_`：执行器
  - `cancellation_state_`：取消状态
- 逻辑上 = 链表栈的哑头节点（dummy head），永不出栈

### 2.3 帧链表栈（push_frame / pop_frame）

Asio 用 `caller_` 指针把多个协程帧串成链表，模拟函数调用栈：

```
入栈 (co_await)：
void push_frame(caller) {
    caller_ = caller;                          // 记下调用者
    attached_thread_ = caller->attached_thread_; // 继承令牌
    entry_point->top_of_stack_ = this;          // 我成栈顶
    caller->attached_thread_ = nullptr;         // 调用者让出令牌
}

出栈 (co_return)：
void pop_frame() {
    caller_->attached_thread_ = attached_thread_; // 归还令牌
    entry_point->top_of_stack_ = caller_;         // 调用者恢复栈顶
    attached_thread_ = nullptr;
    caller_ = nullptr;
}
```

**`attached_thread_` 是执行令牌**——帧链里只有一个帧持有它（正在被 pump 执行的帧）。`push_frame` 转移，`pop_frame` 归还。

### 2.4 `awaitable_thread::pump()` 引擎

```cpp
void pump() {
    do
        bottom_of_stack_.frame_->top_of_stack_->resume();
    while (bottom_of_stack_.frame_ && bottom_of_stack_.frame_->top_of_stack_);
}
```

本质就是：**不断恢复栈顶帧，直到栈空**。协程挂起时 `resume()` 返回，`pump()` 检查 `top_of_stack_` 是否变化，变了就继续 pump 新栈顶。

### 2.5 `coroutine_traits` 特化

Asio 不在 `awaitable` 类里直接定义 `promise_type`，而是通过 `std::coroutine_traits` 特化来关联：

```cpp
template <typename T, typename Executor, typename... Args>
struct coroutine_traits<asio::awaitable<T, Executor>, Args...> {
    using promise_type = detail::awaitable_frame<T, Executor>;
};
```

这是"方式 2"，优点是把公开 API 和内部实现分离。

### 2.6 线程局部分配器

`awaitable_frame_base` 重写了 `operator new/delete`，使用线程本地缓存池：

- 帧销毁时不 `free` 回 OS，放入线程本地空闲链表
- 新帧分配时从缓存取，零分配
- 线程退出时才统一释放
- 避免了多线程 `malloc` 锁竞争

---

## 三、Asio 异步模型

### 3.1 Proactor 模式

| | Reactor（muduo） | Proactor（Asio 接口） |
|--|---------|----------|
| 通知 | "可以读了" | "读完了，数据在 buf 里" |
| 你做的事 | 收到通知后 `read(fd, buf)` | 直接处理 buf |
| 谁做 I/O | 你 | Asio（底层模拟）|

**Linux 上是模拟 Proactor**：`epoll` 本质是 Reactor，Asio 在内部手动 `::read()` 完成后才调你的 handler，让你感觉像 Proactor。

**Windows 上是真 Proactor**：IOCP 原生支持"投递 buf，内核完成后再通知"。

### 3.2 Completion Token 机制

Asio 所有异步操作参数化最后一个参数——Completion Token：

```cpp
socket.async_read_some(buf, callback);          // 回调版
socket.async_read_some(buf, use_awaitable);       // 返回 awaitable
socket.async_read_some(buf, detached);            // 放弃返回值
socket.async_read_some(buf, yield_context);       // 老式协程版
```

通过 `async_result<Token>::initiate` 在编译期分派到不同的实现路径。

### 3.3 Executor（执行器）

执行器 = 一个 `uintptr_t`（指针 + 标志位）：

```
63                      3 2 1 0
┌─────────────────────────┬───────┐
│  io_context 指针        │ 标志位 │
│  (61 位)                │ (3位)  │
└─────────────────────────┴───────┘
```

- `execute(f)`：直接跑（如果在 io_context 线程中）或塞入 op_queue_
- `dispatch` / `post` / `defer`：只是 `execute` 的不同标志位组合
- `any_io_executor`：类型擦除包装，可持有任意具体执行器

### 3.4 写操作的内部管理

Reactor 下要自己管理写队列、注册/注销 EPOLLOUT。Asio Proactor 内部自动管理：

```
async_write → write_op 状态机
  → try ::write（投机写）
  → 没写完 → epoll_ctl(ADD, EPOLLOUT) → 等可写
  → EPOLLOUT → 继续写剩下的
  → 写完了 → 去掉 EPOLLOUT → 调 handler
```

---

## 四、Asio 协程全流程（co_spawn 到 I/O 完成）

```
co_spawn(ioctx, listener(), detached)
  │
  ├→ 创建入口帧（co_spawn_entry_point 的协程帧）
  │  类型：awaitable_frame<entry_point, Executor>
  │
  ├→ 创建 awaitable_handler（继承 awaitable_thread）
  │  持有入口帧在 bottom_of_stack_
  │
  ├→ launch() → pump()
  │   第一次 resume → initial_suspend 挂起
  │   第二次 resume → 进入入口协程体
  │   co_await dispatch → 切到目标 executor
  │   co_await s.function() → 即 listener()
  │     → 创建 acceptor → 注册到 epoll
  │     → co_await accept → 挂起
  │
  ├→ (epoll 返回新连接) → 恢复 listener
  │   co_spawn(session) → 新协程链
  │   回到 accept
  │
  └→ (epoll 返回数据) → pump→resume(session)
     → async_read_some → await_resume 返回 (ec, n)
     → 继续执行...
```

---

## 五、Phase 2：HTTP 服务器设计

### 5.1 架构分层

```
main.cpp
  ├─ Config (YAML)           ← 配置加载
  ├─ FileCache               ← 预加载文件到内存
  └─ listener() 协程
       ├─ LlhttpContext       ← llhttp 解析器（Context 基类）
       └─ Sessionmanage       ← 连接管理 + 文件服务
```

### 5.2 Context 抽象基类

```cpp
class Context {
    virtual ParseResult Feed(const char* data, size_t len) = 0;
    virtual std::string_view Method() const = 0;
    virtual std::string_view Path() const = 0;
    virtual std::string_view Header(std::string_view key) const = 0;
    virtual std::string_view Body() const = 0;
    virtual std::string MakeResponse(int code, const std::string& mime,
                                     const std::string& body) const = 0;
    virtual std::string MakeError(int code) const = 0;
};
```

- `Sessionmanage` 只依赖 `Context*`，不感知 HTTP 具体协议
- 替换解析器只需更换 `Context` 实现，`Sessionmanage` 零改动

### 5.3 文件缓存的作用

用 `FileCache` 代替 `ifstream` 后：

| 指标 | ifstream | FileCache |
|------|----------|-----------|
| QPS (4线程) | 54,283 | **122,307** |
| 延迟 avg | 8.25ms | **0.78ms** |

原因：`ifstream::read` 是同步 I/O，阻塞 io_context 的工作线程。FileCache 在启动时一次性读入内存，请求时零文件 I/O。

### 5.4 多线程 io_context

```cpp
asio::io_context ioctx;
for (int i = 0; i < thread_count - 1; ++i)
    workers.emplace_back([&ioctx] { ioctx.run(); });
ioctx.run();  // 主线程也参与
```

- 所有线程共享同一个 `io_context` 的 epoll fd
- 内核随机唤醒一个线程处理就绪事件
- 同一个连接可能在不同线程间切换（无状态则安全）
- 多线程需要 `executor_work_guard` 防止 `run()` 提前退出

### 5.5 路径穿越防护

```cpp
std::string NormalizePath(std::string_view raw) {
    if (raw.find("..") != std::string::npos) return {};
    if (raw[0] != '/') return {};
    if (p.back() == '/') p += "index.html";
    return p;
}
```

---

## 六、llhttp 解析器设计

### 6.1 核心设计

- **代码生成**：TypeScript → 中间表示 → C（非手写，236 个状态）
- **显式状态机**：逐字符 goto 跳转，比手写 if-else 分支预测友好
- **零拷贝回调**：`on_url(parser, at, len)` 的 `at` 直接指向输入 buffer
- `llhttp_t`（可变状态）与 `llhttp_settings_t`（回调表）分离

### 6.2 手写 vs llhttp

| 维度 | 手动解析 | llhttp |
|------|---------|--------|
| 状态管理 | 隐式 if-else | 显式 236 状态跳转表 |
| 内存分配 | 每次构造 string | 回调直接传位置 |
| Chrome | 不认识 | 内置 |
| WebSocket Upgrade | 不认识 | `on_headers_complete` 返回 2 |
| Pipelining | 会丢请求 | `on_message_complete` 可续解析 |
| 错误处理 | 自己写 | 30+ 错误码 |

### 6.3 llhttp 回调返回值

```
0          → 继续解析
1          → 认为无 body，跳过
2          → 无 body + 停止解析（用于 Upgrade）
-1         → 错误
HPE_PAUSED → 暂停（后续 resume_after_upgrade）
```

---

## 七、HTTP 协议概念要点

| 概念 | 说明 | 需要解析器处理 |
|------|------|--------------|
| Keep-Alive | 复用 TCP 连接处理多个请求 | 多请求解析循环 |
| Content-Length | body 长度 | 精确匹配 |
| Chunked | 分块传输，未知总长度 | 16 进制长度行 |
| Pipelining | 不等待响应连续发多个请求 | 一个 buffer 里多个请求 |
| Upgrade | 协议升级（如 WebSocket） | 握手后换协议 |
| HTTP/2 | 二进制帧，多路复用，HPACK | nghttp2 库，不能手写 |

---

## 八、项目结构

```
webcpp/
├── CMakeLists.txt
├── cpp-coroutine-network-learning-path.md   ← 原始学习路线
├── learn-summary.md                         ← 本文（深入理解总结）
├── config.yaml                              ← 服务器配置
├── www/
│   └── index.html                           ← 测试用静态文件
├── phase1/                                  ← 协程基础 + Echo Server
│   ├── test_coro.cpp                        ← 最简单的协程（suspend_never）
│   ├── test_suspend.cpp                     ← 手动控制挂起恢复
│   ├── 01_echo_server.cpp                   ← Asio 协程版 TCP Echo Server
│   └── benchmark.cpp                        ← Echo 压测客户端
├── phase2/                                  ← HTTP 解析 + 静态文件服务器
│   ├── config.yaml                          ← YAML 配置
│   ├── config.hpp / config.cpp              ← 配置加载
│   ├── file_cache.hpp / file_cache.cpp      ← 文件缓存（替代 ifstream）
│   ├── main.cpp                             ← 入口 + listener + 多线程
│   ├── session.cpp                          ← Sessionmanage（Context + FileCache）
│   └── httpcontext/
│       ├── context.hpp                      ← Context 抽象基类
│       ├── httpcontext.hpp / .cpp           ← 手写 HTTP 解析器（参考）
│       ├── llhttp_context.hpp / .cpp        ← llhttp 版 HTTP 解析器
│       ├── llhttp.h / .c / api.c / http.c   ← llhttp 源码
├── phase3/                                  ← 预留：数据库
├── phase4/                                  ← 预留：部署
└── build/
    ├── http_server                          ← 编译产物
    └── ...
```

---

## 九、推荐进阶阅读

| 主题 | 阅读材料 |
|------|---------|
| C++ 协程原理 | [C++ Coroutines: Understanding the promise model](https://www.scs.stanford.edu/~dm/blog/c++-coroutines.html) |
| Asio 协程 | [Asio 官方文档 - Coroutines](https://think-async.com/Asio/asio-1.30.2/doc/asio/overview/model/coroutines.html) |
| Asio 源码 | `impl/awaitable.hpp`（awaitable_frame_base, awaitable_thread, pump） |
| llhttp 设计 | llhttp 源码 + Chris Dickinson 的关于 FSM 生成器的演讲 |
| HTTP/2 | [RFC 7540](https://datatracker.ietf.org/doc/html/rfc7540) + nghttp2 示例 |
| WebSocket | [RFC 6455](https://datatracker.ietf.org/doc/html/rfc6455) |
| 零拷贝网络 | Linux `sendfile()` / `io_uring` |

---

> 完成日期：2026/06/25
> 环境：GCC 13.3.0, Asio 1.28.1, C++20
