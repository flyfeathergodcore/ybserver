#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <thread>
#include <filesystem>
#include <dlfcn.h>

class Router;

// ═══════════════════════════════════════════════════════════════════
// HotReloader — inotify 监控 .so 目录，文件变化时自动热重载
//
// 在后台线程运行，检测到 .so 发生变化后：
//   1. dlopen 新版本
//   2. dlsym("register_routes") 获取注册函数
//   3. 调用注册函数更新路由
//   4. 旧 .so 保持打开（避免正在执行的 handler 崩溃）
//
// 注意：Router::Add 和 Router::Match 在多线程时需要外部同步。
// 此实现假设在开发环境下，不存在大量的并发请求。
// ═══════════════════════════════════════════════════════════════════

class HotReloader {
public:
    HotReloader(std::vector<std::string> watch_dirs, Router& router);
    ~HotReloader();

    HotReloader(const HotReloader&) = delete;
    HotReloader& operator=(const HotReloader&) = delete;

    void Start();
    void Stop();

private:
    void WatchLoop();

    struct LoadedSo {
        void* handle = nullptr;
        std::filesystem::file_time_type mtime;
    };

    std::vector<std::string> watch_dirs_;
    Router& router_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    uint64_t version_ = 0;  // 临时文件版本后缀

    std::unordered_map<std::string, LoadedSo> loaded_;
    std::vector<void*> stale_handles_;  // 旧 .so，保持打开直到进程退出

    void Reload(const std::string& path);
};
