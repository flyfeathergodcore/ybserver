#pragma once
#include <asio.hpp>
#include <asio/ssl.hpp>
#include <string>

class TlsContext {
public:
    TlsContext();
    bool Load(const std::string& cert_file,
              const std::string& key_file,
              const std::string& dh_file = {});
    asio::ssl::context& NativeContext();
    const asio::ssl::context& NativeContext() const;
    explicit operator bool() const { return loaded_; }

private:
    asio::ssl::context ctx_;
    bool loaded_ = false;
};
