#pragma once
#include "http/context.hpp"
#include "http/llhttp.h"
#include <string>
#include <unordered_map>

// llhttp-based HTTP/1.1 parser (wraps Node.js http-parser)
class LlhttpParser : public Context {
public:
    LlhttpParser();
    ~LlhttpParser() override;

    ParseResult Feed(const char* data, size_t len) override;

    std::string_view Method()  const override { return method_; }
    std::string_view Path()    const override { return path_; }
    std::string_view Version() const override { return version_; }
    std::string_view Header(const std::string_view key) const override;
    std::string_view Body()   const override { return body_; }

private:
    llhttp_t parser_;
    llhttp_settings_t settings_;

    std::string method_;
    std::string path_;
    std::string version_;
    std::unordered_map<std::string, std::string> headers_;
    std::string body_;

    std::string last_header_field_;
    std::string error_reason_;
    bool message_complete_ = false;

    // llhttp callbacks
    static int OnUrl(llhttp_t* p, const char* at, size_t len);
    static int OnHeaderField(llhttp_t* p, const char* at, size_t len);
    static int OnHeaderValue(llhttp_t* p, const char* at, size_t len);
    static int OnHeadersComplete(llhttp_t* p);
    static int OnBody(llhttp_t* p, const char* at, size_t len);
    static int OnMessageComplete(llhttp_t* p);
};
