#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <unordered_map>
#include <shared_mutex>
#include "handler/request_handler.hpp"
#include "config/config.hpp"

class FileCache;

// ═══════════════════════════════════════════════════════════════════
// Compressed Radix Tree Router  (gin/httprouter 风格)
//
// 核心算法：
//   - 路径压缩：连续不产生分支的路径段合并到一个节点
//   - LCP 分裂：插入时在最长公共前缀处分裂，保持树平衡
//   - 三级匹配优先级：精确静态 > :param > *
//
// 插入示例：
//   现有  "/api/v1/users" → 插入 "/api/v1/posts"
//     LCP＝"/api/v1/"，原节点分裂：
//       "/api/v1/" (prefix, no handler)
//         ├── "users" (handler1)
//         └── "posts" (handler2)
//
//   再插入 "/api/:id/profile"
//     LCP＝"/api/"，节点再次分裂：
//       "/api/" (prefix)
//         ├── "v1/" (prefix)
//         │   ├── "users" (handler1)
//         │   └── "posts" (handler2)
//         └── ":id" (param)
//             └── "/profile" (handler3)
//
// 前置约束（同 gin）：
//   - :param 必须出现在一个完整的段中，不能部分匹配
//   - *catchAll 必须在路由末尾
//   - 同一层不允许两个不同名的 :param（如 :id 与 :name 冲突）
// ═══════════════════════════════════════════════════════════════════

class Router {
public:
    Router();
    ~Router();

    // ── Route registration ──

    /// Register handler for any HTTP method.
    /// Path ends with "/" → prefix match (e.g. "/api/" matches "/api/foo").
    void Add(std::string path, std::unique_ptr<RequestHandler> handler);

    /// Method-specific routes.
    void Get   (std::string path, std::unique_ptr<RequestHandler> h) { AddRoute("GET",    std::move(path), std::move(h)); }
    void Post  (std::string path, std::unique_ptr<RequestHandler> h) { AddRoute("POST",   std::move(path), std::move(h)); }
    void Put   (std::string path, std::unique_ptr<RequestHandler> h) { AddRoute("PUT",    std::move(path), std::move(h)); }
    void Delete(std::string path, std::unique_ptr<RequestHandler> h) { AddRoute("DELETE", std::move(path), std::move(h)); }
    void Head  (std::string path, std::unique_ptr<RequestHandler> h) { AddRoute("HEAD",   std::move(path), std::move(h)); }

    /// Auto-register routes from Config (static files + proxy routes).
    /// Creates internal FileCache and ReverseProxy instances.
    void SetupFromConfig(const Config& cfg);

    // ── Matching ──

    /// Backward-compatible: match by path only (any-method).
    RequestHandler* Match(std::string_view path) const;

    /// Full match with HTTP method and path parameter capture.
    /// @param path   e.g. "/api/users/42"
    /// @param method e.g. "GET"
    /// @param params receives captured path parameters (views into @a path)
    RequestHandler* Match(std::string_view method,
                          std::string_view path,
                          std::vector<std::pair<
                              std::string_view,
                              std::string_view>>* params = nullptr) const;

private:
    struct Node {
        // ── Compressed path ──
        // 对于静态节点：连续的路径段（如 "/api/v1/")
        // 对于 param  节点：":paramName"
        // 对于 catchAll： "*paramName"
        std::string path;

        // ── Child routing ──
        std::string indices;                     // 静态子节点路径的首字节
        std::vector<std::unique_ptr<Node>> children;

        // 动态子节点的快速指针（O(1)，不重复索引 indices）
        Node* paramChild     = nullptr;          // :param（每层最多一个）
        Node* catchAllChild  = nullptr;          // *（每层最多一个，必须在最后）

        // ── Node type ──
        enum Type : uint8_t { STATIC, PARAM, CATCH_ALL };
        Type nodeType = STATIC;
        std::string paramName;                   // 无 :/ 前缀（仅 PARAM/CATCH_ALL）

        // ── Prefix match ──
        // Add("/api/") 会将 "/api" 标记为 prefixMatch
        // Lookup 无法精确匹配时，向上回溯到最近的 prefixMatch 节点
        bool isPrefixMatch = false;

        // ── Handlers ──
        // 每个节点可以同时持有 path 匹配的 handler 和 method-specific handler
        RequestHandler* handler_get    = nullptr;
        RequestHandler* handler_post   = nullptr;
        RequestHandler* handler_put    = nullptr;
        RequestHandler* handler_delete = nullptr;
        RequestHandler* handler_head   = nullptr;
        RequestHandler* handler_any    = nullptr; // 任意方法 / 回退

        std::unique_ptr<
            std::unordered_map<std::string, RequestHandler*>> extra;

        void SetHandler(std::string_view method, RequestHandler* h);
        RequestHandler* GetHandler(std::string_view method) const;
        bool HasHandler() const;

        // 子节点管理
        // @param childFirstByte 子节点 path 的首字节（用于 indices）
        // 由于 child->path 可能在后续才设置，调用者传入已知的首字节
        Node* AddStaticChild(std::unique_ptr<Node> child, char childFirstByte);
        Node* AddParamChild(std::unique_ptr<Node> child);
        Node* AddCatchAllChild(std::unique_ptr<Node> child);

        // 优先级：子树中的 leaf handler 数量（用于子节点排序）
        uint32_t priority = 0;
    };

    // FileCache 必须在 handlers_ 之前声明（C++ 反向析构顺序）
    std::unique_ptr<FileCache> file_cache_;

    // 所有 handler 的所有权（Node 内只存裸指针）
    std::vector<std::unique_ptr<RequestHandler>> handlers_;
    std::unique_ptr<Node> root_;

    // 读写锁 —— 热重载时保护路由树（写：Add；读：Match）
    mutable std::shared_mutex rw_mutex_;

    // ── 内部路由注册 ──
    void AddRoute(std::string method, std::string path,
                  std::unique_ptr<RequestHandler> handler);

    // ── 压缩前缀树核心操作 ──

    /// 将 (path, handler, method) 插入以 node 为根的子树。
    /// 通过 LCP 分裂 + 递归/添加子节点实现。
    Node* Insert(Node* node, std::string_view path,
                 RequestHandler* handler, std::string_view method);

    /// 在 *node 之下为剩余 path 创建完整的子树（处理 :param / * 通配符）。
    /// 仅在确定 node 处没有更多共享前缀后调用。
    void InsertChild(Node* node, std::string_view path,
                     RequestHandler* handler, std::string_view method);

    /// 在树中查找 path 对应的节点，沿途收集路径参数。
    const Node* Lookup(const Node* node, std::string_view path,
                       std::vector<std::pair<
                           std::string_view, std::string_view>>* params,
                       const Node* prefixFallback = nullptr) const;

    // ── 工具函数 ──
    static size_t LongestCommonPrefix(std::string_view a, std::string_view b);
    static std::string_view FindWildcard(std::string_view path);
};
