#pragma once
#include <string>
#include <string_view>

enum class ParseResult { Incomplete = 0, Complete = 1, Error = -1 };

class Context {
public:
    virtual ~Context() = default;

    // Feed raw data into the parser
    virtual ParseResult Feed(const char* data, size_t len) = 0;

    // Access parsed results
    virtual std::string_view Method()  const = 0;
    virtual std::string_view Path()    const = 0;
    virtual std::string_view Version() const = 0;
    virtual std::string_view Header(const std::string_view key) const = 0;
    virtual std::string_view Body()   const = 0;

    // Per-request memory pool (optional, set by Session before Execute).
    class MemPool* Pool() const { return pool_; }
    void SetPool(class MemPool* p) const { pool_ = p; }

private:
    mutable class MemPool* pool_ = nullptr;
};
