#include "handler/request_handler.hpp"
#include "net/response.hpp"
#include "net/session_region.hpp"
#include <unistd.h>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <algorithm>
#include <vector>

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

// Forward decl for GenerateDirectoryListing (defined below)
static Response GenerateDirectoryListing(
    const fs::path& dir, SessionRegion& pool,
    std::string_view virt_path);

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
        // ── Autoindex: if path maps to a directory, generate listing ──
        std::string dir_virt;  // virtual path for HTML display
        fs::path dir_on_disk;

        // Check if the path ends with "index.html" — directory autoindex candidate
        constexpr std::string_view kIdx("index.html");
        if (norm_path.size() > kIdx.size() &&
            norm_path.substr(norm_path.size() - kIdx.size()) == kIdx) {
            // /testdir/index.html → "testdir/" for disk, "/testdir/" for display
            size_t dir_end = norm_path.size() - kIdx.size();
            auto dir_rel = (dir_end > 1)
                ? norm_path.substr(1, dir_end - 1)
                : std::string();
            dir_on_disk = fs::path(cache_->DocRoot()) / dir_rel;
            dir_virt = "/" + dir_rel;  // e.g., "/testdir/" or "/" (when dir_rel is "")
        } else {
            // Bare path like /dashboard → maybe it's a directory
            dir_on_disk = fs::path(cache_->DocRoot()) / norm_path.substr(1);
            dir_virt = norm_path;
            if (dir_virt.back() != '/') dir_virt += '/';
        }
        // Ensure trailing slash for display
        if (!dir_virt.empty() && dir_virt.back() != '/')
            dir_virt += '/';

        if (fs::is_directory(dir_on_disk)) {
            return GenerateDirectoryListing(dir_on_disk, *pool, dir_virt);
        }
        return Response::Error(404, *pool);
    }

    // ── Conditional request (304 Not Modified) ──
    {
        bool not_modified = false;
        auto if_none_match = ctx.Header("if-none-match");
        if (!if_none_match.empty()) {
            auto etag_str = FormatEtag(file->mtime,
                !file->content.empty() ? file->content.size() : file->file_size);
            if (if_none_match == etag_str)
                not_modified = true;
        }
        if (!not_modified) {
            auto ims = ctx.Header("if-modified-since");
            if (!ims.empty()) {
                struct tm tm;
                if (::strptime(ims.data(), "%a, %d %b %Y %H:%M:%S GMT", &tm)) {
                    time_t ims_time = ::timegm(&tm);
                    if (ims_time >= file->mtime)
                        not_modified = true;
                }
            }
        }
        if (not_modified) {
            Response resp(304, *pool);
            resp.Header("Cache-Control", "no-cache");
            for (int i = 0; i < ctx.ResponseHeaderCount(); i++)
                resp.Header(ctx.ResponseHeaderKey(i), ctx.ResponseHeaderVal(i));
            resp.EndHeaders();
            return resp;
        }
    }

    // ── Range request helper ──
    struct ByteRange {
        size_t start, end, total;
        bool valid;
    };
    auto ParseRange = [](std::string_view hdr, size_t file_sz) -> ByteRange {
        if (file_sz == 0 || hdr.size() < 7 ||
            hdr.substr(0, 6) != "bytes=") return {};
        auto r = hdr.substr(6);
        auto dash = r.find('-');
        if (dash == std::string_view::npos || dash == 0) return {}; // no suffix
        auto s = r.substr(0, dash);
        auto e = r.substr(dash + 1);
        size_t start = 0;
        for (char c : s) { if (c < '0' || c > '9') return {}; start = start*10 + size_t(c-'0'); }
        if (start >= file_sz) return {};
        size_t end = file_sz - 1;
        if (!e.empty()) {
            end = 0;
            for (char c : e) { if (c < '0' || c > '9') return {}; end = end*10 + size_t(c-'0'); }
            if (end >= file_sz) end = file_sz - 1;
        }
        if (start > end) return {};
        return {start, end, file_sz, true};
    };

    // ── Normal response ──
    // Shared response headers
    auto fillCommonHeaders = [&](Response& resp, uint64_t content_len,
                                  size_t mtime, size_t fsize) {
        resp.Header("Content-Type", file->mime);
        resp.Header("Content-Length", content_len);
        resp.Header("Cache-Control", "no-cache");
        resp.Header("Last-Modified", FormatTime(static_cast<time_t>(mtime)));
        resp.Header("ETag", FormatEtag(static_cast<time_t>(mtime), fsize));
        resp.Header("Accept-Ranges", "bytes");
        for (int i = 0; i < ctx.ResponseHeaderCount(); i++)
            resp.Header(ctx.ResponseHeaderKey(i), ctx.ResponseHeaderVal(i));
    };

    // ── Check Range header → 206 Partial Content ──
    {
        auto range_hdr = ctx.Header("range");
        if (!range_hdr.empty()) {
            auto fsize = !file->content.empty() ? file->content.size() : file->file_size;
            auto br = ParseRange(range_hdr, fsize);
            if (br.valid) {
                auto range_len = br.end - br.start + 1;
                char cr_buf[64];
                int cr_len = std::snprintf(cr_buf, sizeof(cr_buf),
                                           "bytes %zu-%zu/%zu",
                                           br.start, br.end, br.total);

                if (!file->content.empty()) {
                    Response resp(206, *pool);
                    fillCommonHeaders(resp, range_len, file->mtime, fsize);
                    resp.Header("Content-Range", std::string_view(cr_buf,
                                   static_cast<size_t>(cr_len)));
                    resp.EndHeaders();
                    resp.Body(file->content.data() + br.start, range_len);
                    return resp;
                }
                if (file->fd >= 0) {
                    Response resp(206, *pool);
                    fillCommonHeaders(resp, range_len, file->mtime, fsize);
                    resp.Header("Content-Range", std::string_view(cr_buf,
                                   static_cast<size_t>(cr_len)));
                    resp.EndHeaders();
                    resp.BodyFile(file->fd, file->file_size, br.start, range_len);
                    return resp;
                }
            }
            // Fall through to 200 on invalid range (could return 416 instead)
        }
    }

    // ── Normal 200 response ──
    // In-memory content → gather-write (faster for SSL)
    if (!file->content.empty()) {
        Response resp(200, *pool);
        fillCommonHeaders(resp, file->content.size(), file->mtime, file->content.size());
        resp.EndHeaders();
        resp.Body(file->content);
        return resp;
    }

    // Large file: use sendfile path
    if (file->fd >= 0) {
        Response resp(200, *pool);
        fillCommonHeaders(resp, file->file_size, file->mtime, file->file_size);
        resp.EndHeaders();
        resp.BodyFile(file->fd, file->file_size);
        return resp;
    }

    return Response::Error(404, *pool);
}

// ── RedirectHandler ──

Response RedirectHandler::Handle(const Context& ctx)
{
    auto* pool = ctx.Pool();
    Response resp(code_, *pool);
    resp.Header("Location", target_);
    resp.Header("Cache-Control", "no-cache");
    for (int i = 0; i < ctx.ResponseHeaderCount(); i++)
        resp.Header(ctx.ResponseHeaderKey(i), ctx.ResponseHeaderVal(i));
    resp.EndHeaders();
    return resp;
}

// ── Directory listing (autoindex) ──

static Response GenerateDirectoryListing(
    const fs::path& dir, SessionRegion& pool,
    std::string_view virt_path)  // e.g. "/dashboard/" or "/"
{
    std::string html;
    html += "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">\n";
    html += "<title>Index of "; html += virt_path; html += "</title>\n";
    html += "<style>"
            "body{font-family:system-ui,sans-serif;margin:2em;max-width:800px}"
            "table{border-collapse:collapse;width:100%}"
            "th,td{padding:8px 12px;text-align:left;border-bottom:1px solid #eee}"
            "tr:hover{background:#f9f9f9}"
            "a{text-decoration:none}"
            "tr.dir a{font-weight:500}"
            ".size{text-align:right;font-variant-numeric:tabular-nums;color:#555}"
            ".date{white-space:nowrap;color:#888;font-size:90%}"
            "</style></head><body>\n";
    html += "<h1>Index of "; html += virt_path; html += "</h1>\n<hr><table>\n";
    html += "<tr><th>Name</th><th class=\"size\">Size</th><th class=\"date\">Last modified</th></tr>\n";

    // Collect directory entries, sorted: dirs first then files, alphabetical
    std::vector<fs::directory_entry> dirs, files;
    try {
        for (auto& e : fs::directory_iterator(dir)) {
            auto name = e.path().filename().string();
            if (name.empty() || name[0] == '.') continue;  // skip hidden
            if (e.is_directory())
                dirs.push_back(e);
            else if (e.is_regular_file())
                files.push_back(e);
        }
    } catch (...) {
        return Response::Error(404, pool);
    }

    auto by_name = [](const fs::directory_entry& a, const fs::directory_entry& b) {
        return a.path().filename() < b.path().filename();
    };
    std::sort(dirs.begin(),  dirs.end(),  by_name);
    std::sort(files.begin(), files.end(), by_name);

    // Parent dir link (skip for root)
    if (virt_path != "/") {
        html += "<tr class=\"dir\"><td><a href=\"../\">../</a></td>"
                "<td class=\"size\"></td><td class=\"date\"></td></tr>\n";
    }

    // Helpers
    auto fmt_time = [](const fs::directory_entry& e) -> std::string {
        auto ftime = e.last_write_time();
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ftime - fs::file_time_type::clock::now()
            + std::chrono::system_clock::now());
        auto t = std::chrono::system_clock::to_time_t(sctp);
        char buf[32];
        struct tm tm;
        ::gmtime_r(&t, &tm);
        ::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        return buf;
    };

    auto fmt_size = [](uintmax_t sz) -> std::string {
        if (sz < 1024) return std::to_string(sz) + " B";
        if (sz < 1024 * 1024) return std::to_string(sz / 1024) + " KB";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(sz) / (1024 * 1024));
        return buf;
    };

    // Directories
    for (auto& d : dirs) {
        auto name = d.path().filename().string();
        html += "<tr class=\"dir\"><td><a href=\"";
        html += name; html += "/\">"; html += name; html += "/</a></td>";
        html += "<td class=\"size\">-</td><td class=\"date\">";
        html += fmt_time(d); html += "</td></tr>\n";
    }

    // Files
    for (auto& f : files) {
        auto name = f.path().filename().string();
        html += "<tr><td><a href=\"";
        html += name; html += "\">"; html += name; html += "</a></td>";
        html += "<td class=\"size\">"; html += fmt_size(f.file_size()); html += "</td>";
        html += "<td class=\"date\">"; html += fmt_time(f); html += "</td></tr>\n";
    }

    html += "</table>\n<hr><address>webcpp</address>\n</body></html>\n";

    Response resp(200, pool);
    resp.Header("Content-Type", "text/html");
    resp.Header("Content-Length", static_cast<uint64_t>(html.size()));
    resp.Header("Cache-Control", "no-cache");
    resp.EndHeaders();
    pool.Write({html.data(), html.size()});
    return resp;
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
