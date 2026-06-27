#pragma once
#include "http/context.hpp"
#include <string>
#include <unordered_map>

// Hand-written HTTP/1.1 parser (no external dependencies)
class HttpParser : public Context {
public:
    ParseResult Feed(const char* data, size_t len) override;

    std::string_view Method()  const override { return method_; }
    std::string_view Path()    const override { return path_; }
    std::string_view Version() const override { return version_; }
    std::string_view Header(const std::string_view key) const override;
    std::string_view Body()   const override { return body_; }

private:
    std::string method_;
    std::string path_;
    std::string version_;
    std::unordered_map<std::string, std::string> headers_;
    std::string body_;
};
