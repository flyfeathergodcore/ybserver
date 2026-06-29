#include "handler/request_handler.hpp"
#include "net/response.hpp"
#include "net/session_region.hpp"
#include <unistd.h>
#include <cstdio>
#include <ctime>

// ── Helpers for HTTP-date format (RFC 7231) and ETag ──

static std::string FormatTime(time_t t)
{
    char buf[64];
    struct tm tm;
    ::gmtime_r(&t, &tm);
    ::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return buf;
}

static std::string FormatEtag(time_t mtime, size_t size)
{
    char buf[48];
    int n = std::snprintf(buf, sizeof(buf), "\"%lx-%zx\"",
                          static_cast<unsigned long>(mtime), size);
    return std::string(buf, static_cast<size_t>(n));
}

StaticFileHandler::StaticFileHandler(const FileCache* cache)
    : cache_(cache) {}

Response StaticFileHandler::Handle(const Context& ctx)
{
    auto* pool = ctx.Pool();

    if (ctx.Method() != "GET") {
        return Response::Error(501, *pool);
    }

    std::string norm_path = NormalizePath(ctx.Path());
    if (norm_path.empty()) {
        return Response::Error(403, *pool);
    }

    auto* file = cache_->Get(norm_path);
    if (!file) {
        return Response::Error(404, *pool);
    }

    // In-memory content available → gather-write (faster for SSL)
    if (!file->content.empty()) {
        Response resp(200, *pool);
        resp.Header("Content-Type", file->mime);
        resp.Header("Content-Length", file->content.size());
        resp.Header("Cache-Control", "no-cache");
        resp.Header("Last-Modified", FormatTime(file->mtime));
        resp.Header("ETag", FormatEtag(file->mtime, file->content.size()));
        resp.Header("Accept-Ranges", "bytes");
        // Inject any headers from middleware (CORS etc.)
        for (int i = 0; i < ctx.ResponseHeaderCount(); i++)
            resp.Header(ctx.ResponseHeaderKey(i), ctx.ResponseHeaderVal(i));
        resp.EndHeaders();
        resp.Body(file->content);
        return resp;
    }

    // Large file: use sendfile path
    if (file->fd >= 0) {
        Response resp(200, *pool);
        resp.Header("Content-Type", file->mime);
        resp.Header("Content-Length", file->file_size);
        resp.Header("Cache-Control", "no-cache");
        resp.Header("Last-Modified", FormatTime(file->mtime));
        resp.Header("ETag", FormatEtag(file->mtime, file->file_size));
        resp.Header("Accept-Ranges", "bytes");
        for (int i = 0; i < ctx.ResponseHeaderCount(); i++)
            resp.Header(ctx.ResponseHeaderKey(i), ctx.ResponseHeaderVal(i));
        resp.EndHeaders();
        resp.BodyFile(file->fd, file->file_size);
        return resp;
    }

    return Response::Error(404, *pool);
}

std::string StaticFileHandler::NormalizePath(std::string_view raw) const
{
    std::string p(raw);
    // Strip query string for cache busting (?v=...)
    auto qpos = p.find('?');
    if (qpos != std::string::npos)
        p.resize(qpos);

    if (p.empty() || p[0] != '/') return {};
    if (p.find("..") != std::string::npos) return {};
    if (p.find("//") != std::string::npos) return {};

    if (p.back() == '/') p += "index.html";
    else if (p.size() == 1) p += "index.html";

    return p;
}
