#include "httpcontext.hpp"
#include <algorithm>
#include <cctype>

static std::string_view trim(std::string_view sv) {
    while (!sv.empty() && sv.front() == ' ') sv.remove_prefix(1);
    while (!sv.empty() && sv.back() == ' ') sv.remove_suffix(1);
    return sv;
}

static std::string to_lower(std::string_view sv) {
    std::string s(sv);
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

ParseResult HttpContext::Feed(const char* data, size_t len)
{
    if (len < 4) return ParseResult::Incomplete;

    std::string_view s(data, len);
    headers_.clear();

    // ── 请求行 ──
    size_t pos = s.find("\r\n");
    if (pos == std::string_view::npos) return ParseResult::Incomplete;

    std::string_view req_line = s.substr(0, pos);

    size_t curr = req_line.find(' ');
    if (curr == std::string_view::npos) return ParseResult::Error;
    method_ = std::string(req_line.substr(0, curr));

    size_t prev = curr + 1;
    curr = req_line.find(' ', prev);
    if (curr == std::string_view::npos) return ParseResult::Error;
    path_ = std::string(req_line.substr(prev, curr - prev));
    version_ = std::string(req_line.substr(curr + 1));

    // ── 头部 ──
    prev = pos + 2;
    while (prev < len) {
        pos = s.find("\r\n", prev);
        if (pos == std::string_view::npos) break;
        if (pos == prev) { prev = pos + 2; break; }

        std::string_view h = s.substr(prev, pos - prev);
        size_t colon = h.find(':');
        if (colon != std::string_view::npos) {
            std::string key = to_lower(trim(h.substr(0, colon)));
            std::string value = std::string(trim(h.substr(colon + 1)));
            headers_[std::move(key)] = std::move(value);
        }
        prev = pos + 2;
    }

    // ── Body ──
    if (method_ == "POST") {
        auto it = headers_.find("content-length");
        if (it == headers_.end()) return ParseResult::Error;
        size_t body_len = static_cast<size_t>(std::stoi(it->second));
        std::string_view raw_body = s.substr(prev);
        if (raw_body.size() < body_len) return ParseResult::Incomplete;
        if (raw_body.size() > body_len) return ParseResult::Error;
        body_ = std::string(raw_body);
    } else {
        body_.clear();
    }

    return ParseResult::Complete;
}

std::string_view HttpContext::Header(const std::string_view key) const
{
    auto it = headers_.find(std::string(key));
    return it != headers_.end() ? it->second : std::string_view();
}

std::string HttpContext::MakeResponse(
    int code, const std::string& mime, const std::string& body) const
{
    std::string status =
        (code == 200) ? "200 OK" :
        (code == 404) ? "404 Not Found" :
        (code == 403) ? "403 Forbidden" :
        (code == 400) ? "400 Bad Request" :
                        "501 Not Implemented";

    return "HTTP/1.1 " + status + "\r\n"
           "Content-Type: " + mime + "\r\n"
           "Content-Length: " + std::to_string(body.size()) + "\r\n"
           "Connection: keep-alive\r\n"
           "\r\n" + body;
}

std::string HttpContext::MakeError(int code) const
{
    std::string msg =
        (code == 404) ? "Not Found" :
        (code == 403) ? "Forbidden" :
        (code == 400) ? "Bad Request" :
        (code == 501) ? "Not Implemented" :
                        "Error";
    std::string body = "<h1>" + std::to_string(code) + " " + msg + "</h1>";
    return MakeResponse(code, "text/html", body);
}
