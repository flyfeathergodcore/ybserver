// ── Context 抽象基类 ──
// 定义协议解析的统一接口，Sessionmanage 不感知具体协议

#ifndef CONTEXT_HPP
#define CONTEXT_HPP

#include <string>
#include <string_view>

enum class ParseResult { Incomplete = 0, Complete = 1, Error = -1 };

class Context {
public:
    virtual ~Context() = default;

    // 喂入数据，返回解析状态
    virtual ParseResult Feed(const char* data, size_t len) = 0;

    // 访问解析结果
    virtual std::string_view Method()  const = 0;
    virtual std::string_view Path()    const = 0;
    virtual std::string_view Version() const = 0;
    virtual std::string_view Header(const std::string_view key) const = 0;
    virtual std::string_view Body()   const = 0;

    // 构建响应
    virtual std::string MakeResponse(int code,
        const std::string& mime, const std::string& body) const = 0;
    virtual std::string MakeError(int code) const = 0;
};

#endif // CONTEXT_HPP
