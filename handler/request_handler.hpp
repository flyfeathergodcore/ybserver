#pragma once
#include <string>
#include "http/context.hpp"
#include "cache/file_cache.hpp"
#include "net/response.hpp"

// Abstract interface for HTTP request handlers.
// Implementations produce a Response given a parsed request context.
class RequestHandler {
public:
    virtual ~RequestHandler() = default;
    virtual Response Handle(const Context& ctx) = 0;
};

// Serves static files from a FileCache.
// Only handles GET requests; returns 501 for other methods.
class StaticFileHandler : public RequestHandler {
public:
    explicit StaticFileHandler(const FileCache* cache);
    Response Handle(const Context& ctx) override;

private:
    const FileCache* cache_;
    std::string NormalizePath(std::string_view raw) const;
};
