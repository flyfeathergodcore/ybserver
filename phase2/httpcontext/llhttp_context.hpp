#ifndef LLHTTP_CONTEXT_HPP
#define LLHTTP_CONTEXT_HPP

#include "context.hpp"
#include "llhttp.h"
#include <string>
#include <unordered_map>

class LlhttpContext : public Context {
public:
    LlhttpContext();
    ~LlhttpContext() override = default;

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
    void Reset();

    llhttp_t parser_;
    llhttp_settings_t settings_;

    // 当前解析中的临时数据
    std::string method_;
    std::string path_;
    std::string version_;
    std::unordered_map<std::string, std::string> headers_;
    std::string body_;

    std::string last_header_field_;
    bool headers_complete_ = false;
    bool message_complete_ = false;
    std::string error_reason_;

    // llhttp 回调函数
    static int OnUrl(llhttp_t* p, const char* at, size_t len);
    static int OnHeaderField(llhttp_t* p, const char* at, size_t len);
    static int OnHeaderValue(llhttp_t* p, const char* at, size_t len);
    static int OnHeadersComplete(llhttp_t* p);
    static int OnBody(llhttp_t* p, const char* at, size_t len);
    static int OnMessageComplete(llhttp_t* p);
};

#endif
