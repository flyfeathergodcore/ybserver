#pragma once
#include <string>
#include <vector>
#include <memory>
#include <dlfcn.h>

class Router;

// ═══════════════════════════════════════════════════════════════════
// HandlerLoader — 运行时 .so 热加载
//
// 扫描指定目录中的所有 .so 文件，每个 .so 需导出一个 C 符号：
//   extern "C" void register_routes(Router& router);
//
// 在函数内通过 router.Add() 注册任意数量的路由和 handler。
// ═══════════════════════════════════════════════════════════════════
class HandlerLoader {
public:
    HandlerLoader() = default;
    ~HandlerLoader();

    HandlerLoader(const HandlerLoader&) = delete;
    HandlerLoader& operator=(const HandlerLoader&) = delete;

    /// 扫描 dir 下所有 .so，dlopen + register_routes。
    /// 返回成功加载的个数。dir 不存在则静默返回 0。
    size_t LoadAll(const std::string& dir, Router& router);

    /// 卸载所有已加载的 .so（进程退出时清理）。
    void UnloadAll();

    /// 释放所有已加载 .so 句柄的所有权（销毁时不做 dlclose）。
    /// 用于短期 loader 对象：加载后转移所有权，.so 保持打开。
    void ReleaseHandles() { libs_.clear(); }

private:
    struct LoadedLib {
        void*       handle = nullptr;
        std::string path;
    };
    std::vector<LoadedLib> libs_;
};
