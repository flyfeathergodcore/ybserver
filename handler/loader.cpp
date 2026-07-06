#include "handler/loader.hpp"
#include "handler/router.hpp"
#include <filesystem>
#include <iostream>

// ── register_routes 签名的函数指针类型 ──
using RegisterFunc = void (*)(Router&);

HandlerLoader::~HandlerLoader()
{
    UnloadAll();
}

size_t HandlerLoader::LoadAll(const std::string& dir, Router& router)
{
    namespace fs = std::filesystem;

    if (!fs::is_directory(dir)) {
        // 目录不存在不报错 —— 用户可能没有热插拔需求
        return 0;
    }

    size_t count = 0;
    for (auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".so")
            continue;

        auto path = entry.path().string();
        auto* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            std::cerr << "[loader] dlopen 失败: " << path
                      << " — " << ::dlerror() << std::endl;
            continue;
        }

        auto reg = reinterpret_cast<RegisterFunc>(
            ::dlsym(handle, "register_routes"));
        if (!reg) {
            std::cerr << "[loader] dlsym(register_routes) 失败: "
                      << path << " — " << ::dlerror() << std::endl;
            ::dlclose(handle);
            continue;
        }

        // ── 注册路由 —— .so 里调用 router.Add() ──
        try {
            reg(router);
        } catch (std::exception& e) {
            std::cerr << "[loader] register_routes 异常: "
                      << path << " — " << e.what() << std::endl;
            ::dlclose(handle);
            continue;
        }

        libs_.push_back({handle, path});
        std::cout << "[loader] 已加载: " << path << std::endl;
        ++count;
    }

    return count;
}

void HandlerLoader::UnloadAll()
{
    for (auto& lib : libs_) {
        if (lib.handle) {
            ::dlclose(lib.handle);
            std::cout << "[loader] 已卸载: " << lib.path << std::endl;
        }
    }
    libs_.clear();
}
