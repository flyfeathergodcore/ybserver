// ── HTTP/1.1 协议解析器 ──
// 继承 Context 基类，实现 HTTP 请求解析和响应构建

#ifndef HTTPCONTEXT_HPP
#define HTTPCONTEXT_HPP

#include "context.hpp"
#include <unordered_map>
#include <vector>

class HttpContext : public Context {
public:
    ParseResult Feed(const char* data, size_t len) override;

    std::string_view Method()  const override { return method_; }
    std::string_view Path()    const override { return path_; }
    std::string_view Version() const override { return version_; }
    std::string_view Header(const std::string_view key) const override;
    std::string_view Body()   const override { return body_; }

    std::string MakeResponse(int code,
        const std::string& mime, const std::string& body) const override;
    std::string MakeError(int code) const override;

private:
    std::string method_;
    std::string path_;
    std::string version_;
    std::unordered_map<std::string, std::string> headers_;
    std::string body_;
};

#endif // HTTPCONTEXT_HPP
