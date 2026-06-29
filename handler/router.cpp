#include "handler/router.hpp"
#include <cstring>
#include <iostream>

// ═══════════════════════════════════════════════════════════════════
// Node helpers
// ═══════════════════════════════════════════════════════════════════

void Router::Node::SetHandler(std::string_view method, RequestHandler* h)
{
    if (method == "GET")              handler_get    = h;
    else if (method == "POST")        handler_post   = h;
    else if (method == "PUT")         handler_put    = h;
    else if (method == "DELETE")      handler_delete = h;
    else if (method == "HEAD")        handler_head   = h;
    else {
        if (!extra)
            extra = std::make_unique<std::unordered_map<std::string, RequestHandler*>>();
        (*extra)[std::string(method)] = h;
    }
}

RequestHandler* Router::Node::GetHandler(std::string_view method) const
{
    if (method == "GET")              return handler_get;
    if (method == "POST")             return handler_post;
    if (method == "PUT")              return handler_put;
    if (method == "DELETE")           return handler_delete;
    if (method == "HEAD")             return handler_head;
    if (method == "HEAD")             return handler_get;   // HEAD → GET fallback
    if (extra) {
        auto it = extra->find(std::string(method));
        if (it != extra->end()) return it->second;
    }
    return nullptr;
}

bool Router::Node::HasHandler() const
{
    return handler_any || handler_get || handler_post
        || handler_put || handler_delete || handler_head
        || (extra && !extra->empty());
}

Router::Node* Router::Node::AddStaticChild(std::unique_ptr<Node> child, char childFirstByte)
{
    auto* raw = child.get();
    indices += childFirstByte;
    children.push_back(std::move(child));
    return raw;
}

Router::Node* Router::Node::AddParamChild(std::unique_ptr<Node> child)
{
    auto* raw = child.get();
    paramChild = raw;
    // Param 子节点不入 indices（匹配时特殊处理）
    children.push_back(std::move(child));
    return raw;
}

Router::Node* Router::Node::AddCatchAllChild(std::unique_ptr<Node> child)
{
    auto* raw = child.get();
    catchAllChild = raw;
    children.push_back(std::move(child));
    return raw;
}

// ═══════════════════════════════════════════════════════════════════
// Router 构造 / 析构
// ═══════════════════════════════════════════════════════════════════

Router::Router()
    : root_(std::make_unique<Node>()) {}

Router::~Router() = default;

// ═══════════════════════════════════════════════════════════════════
// 公开路由注册接口
// ═══════════════════════════════════════════════════════════════════

void Router::Add(std::string path, std::unique_ptr<RequestHandler> handler)
{
    bool prefix = false;
    // 尾部 "/" 表示前缀匹配，例如 "/api/" → "/api" + prefixMatch
    if (path.size() > 1 && path.back() == '/') {
        path.pop_back();
        prefix = true;
    } else if (path == "/") {
        // "/" 始终作为前缀匹配 —— 兜底所有子路径
        prefix = true;
        path = "";
    }

    auto* raw = handler.release();
    handlers_.emplace_back(raw);

    auto* node = Insert(root_.get(), path, raw, "");
    if (prefix)
        node->isPrefixMatch = true;
    node->handler_any = raw;
}

void Router::AddRoute(std::string method, std::string path,
                       std::unique_ptr<RequestHandler> handler)
{
    auto* raw = handler.release();
    handlers_.emplace_back(raw);

    auto* node = Insert(root_.get(), path, raw, method);
    node->SetHandler(method, raw);
}

// ═══════════════════════════════════════════════════════════════════
// 最长公共前缀
// ═══════════════════════════════════════════════════════════════════

size_t Router::LongestCommonPrefix(std::string_view a, std::string_view b)
{
    size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i])
        ++i;
    return i;
}

// ═══════════════════════════════════════════════════════════════════
// 查找通配符
// 返回第一个 :param 或 *catchAll（必须位于段首：i==0 || path[i-1]=='/'）
// 找不到时返回空 string_view
// ═══════════════════════════════════════════════════════════════════

std::string_view Router::FindWildcard(std::string_view path)
{
    for (size_t i = 0; i < path.size(); ++i) {
        if ((path[i] == ':' || path[i] == '*') &&
            (i == 0 || path[i - 1] == '/'))
        {
            // 找到段尾（下一个 '/' 或串尾）
            auto end = path.find('/', i);
            if (end == std::string_view::npos)
                return path.substr(i);
            return path.substr(i, end - i);
        }
    }
    return {};
}

// ═══════════════════════════════════════════════════════════════════
// InsertChild — 在无共享前缀的节点下创建完整子树
//
// 有两种情况走到这里：
//   1. 全新的路由（树中不存在任何共享前缀）
//   2. 已有共享前缀，分裂后剩余的 path 需要建新子树
//
// 处理 :param / *catchAll 通配符，递归创建中间节点。
// ═══════════════════════════════════════════════════════════════════

void Router::InsertChild(Node* node, std::string_view path,
                          RequestHandler* handler, std::string_view method)
{
    while (true) {
        // 扫描通配符
        auto wildcard = FindWildcard(path);
        if (wildcard.empty()) {
            // 无通配符 → 当前 node 即为叶节点
            node->path.assign(path.data(), path.size());
            if (method.empty())
                node->handler_any = handler;
            else
                node->SetHandler(method, handler);
            return;
        }

        // 通配符前的静态前缀（可能为空）
        auto wildcardPos = static_cast<size_t>(wildcard.data() - path.data());
        if (wildcardPos > 0) {
            node->path.assign(path.data(), wildcardPos);
            path = path.substr(wildcardPos);
        }

        if (wildcard[0] == ':') {
            // ── :param ──
            auto child = std::make_unique<Node>();
            child->nodeType  = Node::PARAM;
            child->path      = wildcard;
            child->paramName = std::string(wildcard.substr(1));

            node = node->AddParamChild(std::move(child));
            path = path.substr(wildcard.size());

            if (path.empty()) {
                // :param 是最后一段 → handler 挂于此节点
                if (method.empty())
                    node->handler_any = handler;
                else
                    node->SetHandler(method, handler);
                return;
            }

            // :param 之后还有路径 → 建静态子节点承载后续
            auto rest = std::make_unique<Node>();
            node       = node->AddStaticChild(std::move(rest), path[0]);
            // continue 继续处理后续 path（可能还有通配符）

        } else if (wildcard[0] == '*') {
            // ── *catchAll — 必须为最后一节 ──
            auto child = std::make_unique<Node>();
            child->nodeType  = Node::CATCH_ALL;
            child->path      = wildcard;
            child->paramName = std::string(wildcard.substr(1));

            if (method.empty())
                child->handler_any = handler;
            else
                child->SetHandler(method, handler);

            node->AddCatchAllChild(std::move(child));
            return;  // catchAll 后不能再有路径
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
// Insert — 向压缩前缀树插入一条路由
//
// 算法（gin/httprouter）：
//   1. 计算当前节点 path 与目标 path 的 LCP
//   2. 若 LCP < node.path → 分裂当前节点
//   3. 若 path 已被完全消费 → 设 handler 并返回
//   4. 若 path 尚有剩余 → 通过 indices 查找/创建子节点，递归
// ═══════════════════════════════════════════════════════════════════

Router::Node* Router::Insert(Node* node, std::string_view path,
                              RequestHandler* handler, std::string_view method)
{
    // 空树 / 根节点（path 为空）
    if (node->path.empty() && node->children.empty() && !node->HasHandler()) {
        InsertChild(node, path, handler, method);
        return node;
    }

    while (true) {
        auto i = LongestCommonPrefix(node->path, path);

        // ── 分裂当前节点 ──
        if (i < node->path.size()) {
            auto split = std::make_unique<Node>();
            split->path     = node->path.substr(i);
            split->indices  = std::move(node->indices);
            split->children = std::move(node->children);

            // 搬运动态子节点指针
            split->paramChild    = node->paramChild;
            split->catchAllChild = node->catchAllChild;

            // 搬运 handler
            split->handler_any    = node->handler_any;
            split->handler_get    = node->handler_get;
            split->handler_post   = node->handler_post;
            split->handler_put    = node->handler_put;
            split->handler_delete = node->handler_delete;
            split->handler_head   = node->handler_head;
            split->extra          = std::move(node->extra);
            split->isPrefixMatch  = node->isPrefixMatch;

            // 当前节点变为前缀
            node->path     = node->path.substr(0, i);
            node->indices  = split->path[0];  // 唯一子节点（分裂出来的）
            node->children.clear();
            node->children.push_back(std::move(split));

            // 当前节点不再持有 handler
            node->handler_any    = nullptr;
            node->handler_get    = nullptr;
            node->handler_post   = nullptr;
            node->handler_put    = nullptr;
            node->handler_delete = nullptr;
            node->handler_head   = nullptr;
            node->extra.reset();
            node->paramChild     = nullptr;
            node->catchAllChild  = nullptr;
            node->isPrefixMatch  = false;
        }

        // ── path 已被完全消费 ──
        if (i == path.size()) {
            // handler 设于此节点（路径已在 LCP 中被消费）
            // 注意：已有 handler 且不是覆盖则冲突——这里先简单覆盖
            if (!method.empty())
                node->SetHandler(method, handler);
            else
                node->handler_any = handler;
            return node;
        }

        // ── path 尚有剩余 ──
        path = path.substr(i);

        // 尝试通过 indices 匹配静态子节点
        char c = path[0];
        bool matched = false;
        for (size_t j = 0; j < node->indices.size(); ++j) {
            if (c == node->indices[j]) {
                node    = node->children[j].get();
                matched = true;
                break;
            }
        }
        if (matched)
            continue;  // 递归到子节点

        // 没有匹配的静态子节点 → 建新子树
        auto child = std::make_unique<Node>();
        auto* raw  = child.get();
        node->AddStaticChild(std::move(child), path[0]);
        InsertChild(raw, path, handler, method);
        return raw;
    }
}

// ═══════════════════════════════════════════════════════════════════
// Lookup — 在压缩前缀树中查找路径
//
// 匹配优先级（每层）：
//   1. 精确静态匹配（indices 首字节 + path 全量比对）
//   2. :param 通配（捕获到下一个 '/'）
//   3. *catchAll 兜底（捕获剩余所有路径）
//   4. 若当前节点标记为 isPrefixMatch，作为回退
//   5. 参数 prefixFallback 携带上级前缀匹配节点
// ═══════════════════════════════════════════════════════════════════

const Router::Node* Router::Lookup(
    const Node* node,
    std::string_view path,
    std::vector<std::pair<std::string_view, std::string_view>>* params,
    const Node* prefixFallback) const
{
    // 根节点 / root 直接命中
    if (path.empty() || path == "/") {
        if (node->HasHandler() ||
            node->handler_any)  // 根也可能挂 handler
            return node;
        if (node->isPrefixMatch)
            return node;
        return nullptr;
    }

    // 统一去掉尾部 '/' 再匹配
    if (path.size() > 1 && path.back() == '/')
        path.remove_suffix(1);

    while (true) {
        auto& prefix = node->path;

        // ── 路径必须以此节点前缀开头 ──
        if (path.size() < prefix.size() ||
            path.substr(0, prefix.size()) != prefix)
        {
            goto check_fallback;
        }
        path = path.substr(prefix.size());

        // ── 路径消耗完毕 ──
        if (path.empty()) {
            if (node->HasHandler() || node->handler_any)
                return node;
            // 当前节点无 handler → 尝试前缀回退
            if (node->isPrefixMatch)
                return node;
            return (prefixFallback && prefixFallback->HasHandler())
                       ? prefixFallback
                       : nullptr;
        }

        // 向下走之前更新 prefixFallback
        if (node->isPrefixMatch && node->HasHandler())
            prefixFallback = node;

        // ── 尝试静态子节点 ──
        {
            char c = path[0];
            for (size_t i = 0; i < node->indices.size(); ++i) {
                if (c == node->indices[i]) {
                    node = node->children[i].get();
                    goto match_child;
                }
            }
        }

        // ── 尝试 :param 子节点 ──
        if (node->paramChild) {
            auto slash = path.find('/');
            auto seg   = (slash == std::string_view::npos)
                             ? path
                             : path.substr(0, slash);

            if (!seg.empty()) {
                if (params)
                    params->emplace_back(node->paramChild->paramName, seg);
                if (slash == std::string_view::npos) {
                    // param 匹配到末尾 → 返回 param 子节点
                    return node->paramChild;
                }
                // param 之后还有路径 → 跟随 param 的静态子节点继续
                path = path.substr(slash);
                node = node->paramChild;

                // param 节点的子节点（通常只有一个静态节点）
                if (!node->indices.empty()) {
                    char c2 = path[0];
                    for (size_t i = 0; i < node->indices.size(); ++i) {
                        if (c2 == node->indices[i]) {
                            node = node->children[i].get();
                            goto match_child;
                        }
                    }
                }
                // param 下无匹配子节点 → 尝试 param 自身的 handler
                if (node->HasHandler())
                    return node;
                goto check_fallback;
            }
        }

    check_fallback:
        // ── 尝试 *catchAll 子节点 ──
        if (node->catchAllChild) {
            if (params)
                params->emplace_back(node->catchAllChild->paramName, path);
            return node->catchAllChild;
        }

        // ── 前缀匹配回退 ──
        return (prefixFallback && prefixFallback->HasHandler())
                   ? prefixFallback
                   : nullptr;

    match_child:
        // 递归匹配子节点（while 循环首部检查前缀）
        continue;
    }
}

// ═══════════════════════════════════════════════════════════════════
// Match（公开匹配接口）
// ═══════════════════════════════════════════════════════════════════

RequestHandler* Router::Match(std::string_view path) const
{
    auto* node = Lookup(root_.get(), path, nullptr);
    if (!node) return nullptr;
    if (node->handler_any) return node->handler_any;
    // 没 any 就试 method-specific 的任意一个
    return node->handler_get ? node->handler_get
         : node->handler_post ? node->handler_post
         : nullptr;
}

RequestHandler* Router::Match(
    std::string_view method,
    std::string_view path,
    std::vector<std::pair<std::string_view, std::string_view>>* params) const
{
    auto* node = Lookup(root_.get(), path, params);
    if (!node) return nullptr;

    auto* h = node->GetHandler(method);
    if (h) return h;
    return node->handler_any;  // fallback to any-method handler
}
