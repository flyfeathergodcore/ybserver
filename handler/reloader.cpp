#include "handler/reloader.hpp"
#include "handler/router.hpp"
#include <iostream>
#include <sys/inotify.h>
#include <unistd.h>
#include <poll.h>

static constexpr size_t kEventBufSize = sizeof(struct inotify_event) + NAME_MAX + 1;

using RegisterFunc = void (*)(Router&);

HotReloader::HotReloader(std::vector<std::string> watch_dirs, Router& router)
    : watch_dirs_(std::move(watch_dirs))
    , router_(router)
{}

HotReloader::~HotReloader()
{
    Stop();
    for (auto& h : stale_handles_) {
        if (h) ::dlclose(h);
    }
    for (auto& [_, so] : loaded_) {
        if (so.handle) ::dlclose(so.handle);
    }
}

void HotReloader::Start()
{
    if (running_.exchange(true)) return;
    thread_ = std::thread(&HotReloader::WatchLoop, this);
}

void HotReloader::Stop()
{
    if (!running_.exchange(false)) return;
    if (thread_.joinable())
        thread_.join();
}

void HotReloader::WatchLoop()
{
    int inotify_fd = ::inotify_init1(IN_NONBLOCK);
    if (inotify_fd < 0) {
        std::cerr << "[reloader] inotify_init1 失败" << std::endl;
        return;
    }

    // ── 首次加载所有已有的 .so ──
    namespace fs = std::filesystem;
    for (auto& dir : watch_dirs_) {
        if (!fs::is_directory(dir)) continue;

        // 监控目录
        int wd = ::inotify_add_watch(inotify_fd, dir.c_str(),
                                      IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
        if (wd < 0) {
            std::cerr << "[reloader] inotify_add_watch 失败: " << dir << std::endl;
            continue;
        }

        // 遍历已有 .so 文件
        for (auto& entry : fs::directory_iterator(dir)) {
            if (entry.path().extension() != ".so") continue;
            auto path = entry.path().string();
            auto mtime = fs::last_write_time(entry.path());
            // dlopen 先获取句柄（即使 HandlerLoader 已加载过也增 refcount）
            auto* h = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            loaded_[path] = {h, mtime};
        }
    }

    // ── 事件循环 ──
    char buf[kEventBufSize * 32];  // 放大缓冲区，一次读多个事件
    uint64_t tick = 0;
    auto check_dir = [&](const std::string& dir) {
        if (!fs::is_directory(dir)) return;
        for (auto& entry : fs::directory_iterator(dir)) {
            if (entry.path().extension() != ".so") continue;
            auto path = entry.path().string();
            auto mtime = fs::last_write_time(entry.path());
            auto it = loaded_.find(path);
            if (it != loaded_.end() && it->second.mtime == mtime)
                continue;
            std::cout << "[reloader] 定时检测到变化: " << path << std::endl;
            Reload(path);
        }
    };

    while (running_.load()) {
        struct pollfd pfd = {inotify_fd, POLLIN, 0};
        int ret = ::poll(&pfd, 1, 1000);  // 1秒超时，用于检查 running_
        ++tick;

        // 每 5 秒 stat 兜底（inotify 可能遗漏某些文件操作）
        if (ret < 0 && running_.load()) {
            ::close(inotify_fd);
            inotify_fd = ::inotify_init1(IN_NONBLOCK);
            if (inotify_fd < 0) break;
            for (auto& d : watch_dirs_) {
                if (fs::is_directory(d))
                    ::inotify_add_watch(inotify_fd, d.c_str(),
                        IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
            }
        }
        if (ret == 0 && tick % 5 == 0) {
            for (auto& dir : watch_dirs_) check_dir(dir);
            continue;
        }
        if (ret <= 0) continue;

        ssize_t len = ::read(inotify_fd, buf, sizeof(buf));
        if (len <= 0) continue;

        size_t off = 0;
        while (off < static_cast<size_t>(len)) {
            auto* ev = reinterpret_cast<struct inotify_event*>(buf + off);
            off += sizeof(struct inotify_event) + ev->len;

            if (ev->len == 0 || !(ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE)))
                continue;

            // 忽略 .so.tmp 等临时文件（只处理 .so 结尾）
            std::string name(ev->name, ev->len);
            if (name.size() < 3 ||
                name.substr(name.size() - 3) != ".so" ||
                (name.size() > 4 && name.substr(name.size() - 4) == ".tmp"))
                continue;

            // 找到完整路径
            for (auto& dir : watch_dirs_) {
                auto path = dir + "/" + name;
                if (!fs::exists(path)) continue;

                // 检查 mtime 是否真的变了
                auto mtime = fs::last_write_time(path);
                auto it = loaded_.find(path);
                if (it != loaded_.end() && it->second.mtime == mtime)
                    continue;

                Reload(path);
                break;
            }
        }
    }

    ::close(inotify_fd);
}

void HotReloader::Reload(const std::string& path)
{
    std::cout << "[reloader] 检测到变化: " << path << std::endl;

    // ── 复制到临时路径后 dlopen ──
    // dlopen 以路径名为缓存键；即使磁盘文件已更新，dlopen 同一路径仍返
    // 回旧句柄。方案：复制到临时文件，dlopen 临时路径 → 强制读新磁盘。
    // 加载后删除临时文件（Linux 保持已映射页面，不影响已加载的代码）。
    namespace fs = std::filesystem;
    auto tmp = path + ".reload." + std::to_string(version_++);
    std::error_code ec;
    fs::copy(path, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "[reloader] 复制到临时文件失败: " << ec.message() << std::endl;
        return;
    }

    auto* handle = ::dlopen(tmp.c_str(), RTLD_NOW | RTLD_LOCAL);
    fs::remove(tmp, ec);  // 清理临时文件（已加载的库不受影响）
    if (!handle) {
        std::cerr << "[reloader] dlopen 失败: " << ::dlerror() << std::endl;
        return;
    }

    auto reg = reinterpret_cast<RegisterFunc>(::dlsym(handle, "register_routes"));
    if (!reg) {
        std::cerr << "[reloader] dlsym(register_routes) 失败: " << ::dlerror() << std::endl;
        ::dlclose(handle);
        return;
    }

    // ── 注册新路由 ──
    try {
        reg(router_);
    } catch (std::exception& e) {
        std::cerr << "[reloader] register_routes 异常: " << e.what() << std::endl;
        ::dlclose(handle);
        return;
    }

    // ── 保留旧 .so 句柄不关闭（in-flight handler 对象仍需其代码） ──
    auto it = loaded_.find(path);
    if (it != loaded_.end() && it->second.handle)
        stale_handles_.push_back(it->second.handle);

    loaded_[path] = {handle, fs::last_write_time(path)};
    std::cout << "[reloader] 已热重载: " << path << std::endl;
}
