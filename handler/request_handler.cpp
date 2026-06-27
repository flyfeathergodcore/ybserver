#include "handler/request_handler.hpp"
#include "net/response.hpp"
#include <unistd.h>

StaticFileHandler::StaticFileHandler(const FileCache* cache)
    : cache_(cache) {}

Response StaticFileHandler::Handle(const Context& ctx)
{
    if (ctx.Method() != "GET") {
        return Response::Error(501);
    }

    std::string norm_path = NormalizePath(ctx.Path());
    if (norm_path.empty()) {
        return Response::Error(403);
    }

    auto* file = cache_->Get(norm_path);
    if (!file) {
        return Response::Error(404);
    }

    auto* pool = ctx.Pool();

    // In-memory content available → use it (faster for SSL)
    if (!file->content.empty()) {
        if (pool) {
            return Response::Pooled(*pool, 200, file->mime, file->content);
        }
        return Response(200, file->mime, file->content);
    }

    // Large file: use sendfile path (zero-copy for TCP, read+write for SSL)
    if (file->fd >= 0) {
        return Response::File(200, file->mime, file->fd, file->file_size);
    }

    return Response::Error(404);
}

std::string StaticFileHandler::NormalizePath(std::string_view raw) const
{
    std::string p(raw);
    if (p.empty() || p[0] != '/') return {};
    if (p.find("..") != std::string::npos) return {};
    if (p.find("//") != std::string::npos) return {};

    if (p.back() == '/') p += "index.html";
    else if (p.size() == 1) p += "index.html";

    return p;
}
