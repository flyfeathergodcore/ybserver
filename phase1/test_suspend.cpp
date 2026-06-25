// 展示协程的挂起和恢复
// 编译: g++ -std=c++20 -fcoroutines test_suspend.cpp -o test_suspend

#include <coroutine>
#include <iostream>

struct Task {
    struct promise_type {
        Task get_return_object() {
            // 把 promise 自己的 coroutine_handle 返回给 Task
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() {  // ← 改为 always，协程创建后立即挂起
            std::cout << "[promise] 协程创建完毕，立即挂起" << std::endl;
            return {};
        }
        std::suspend_always final_suspend() noexcept {  // ← 改为 always，结束后也挂起
            std::cout << "[promise] 协程执行完毕，再次挂起" << std::endl;
            return {};
        }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };

    std::coroutine_handle<promise_type> handle;

    // 手动恢复协程
    void resume() {
        std::cout << "[main] 调用 resume() 恢复协程" << std::endl;
        handle.resume();
        std::cout << "[main] resume() 返回" << std::endl;
    }

    // 检查是否执行完毕
    bool done() const { return handle.done(); }

    // 析构时销毁协程帧
    ~Task() {
        if (handle) {
            handle.destroy();
            std::cout << "[main] 协程帧已销毁" << std::endl;
        }
    }
};

Task hello() {
    std::cout << "  >> 协程函数体开始执行" << std::endl;
    std::cout << "  >> hello from coroutine!" << std::endl;
    co_return;
    std::cout << "  >> 这一行永远不会执行" << std::endl;
}

int main() {
    std::cout << "=== 调用 hello() 创建协程 ===" << std::endl;
    Task t = hello();
    std::cout << "=== hello() 返回了，协程还未执行 ===" << std::endl;
    std::cout << "协程是否执行完? " << (t.done() ? "是" : "否") << std::endl;

    std::cout << "\n--- 第一次 resume ---" << std::endl;
    t.resume();

    std::cout << "\n协程是否执行完? " << (t.done() ? "是" : "否") << std::endl;

    std::cout << "\n--- 第二次 resume（对已结束的协程）---" << std::endl;
    t.resume();  // 再恢复一次会怎样？

    std::cout << "\n=== 程序结束，Task 析构会销毁协程帧 ===" << std::endl;
    return 0;
}
