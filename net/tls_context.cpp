#include "net/tls_context.hpp"
#include <cstring>
#include <iostream>
#include <random>

TlsContext::TlsContext()
    : ctx_(asio::ssl::context::tls_server)
{
    ctx_.set_options(
        asio::ssl::context::default_workarounds |
        asio::ssl::context::no_sslv2 |
        asio::ssl::context::no_sslv3 |
        asio::ssl::context::no_tlsv1 |
        asio::ssl::context::no_tlsv1_1 |
        asio::ssl::context::single_dh_use);

    // ── TLS 会话缓存 ──
    auto* ssl_ctx = ctx_.native_handle();

    // 服务端模式
    SSL_CTX_set_session_cache_mode(ssl_ctx, SSL_SESS_CACHE_SERVER);

    // 生成唯一的 session ID context（防止跨实例重用）
    unsigned char session_id[32];
    const char* prefix = "webcpp-srv-1";
    std::memcpy(session_id, prefix, std::strlen(prefix));

    // 随机填充剩余字节
    std::mt19937 rng(std::random_device{}());
    for (size_t i = std::strlen(prefix); i < sizeof(session_id); ++i)
        session_id[i] = static_cast<unsigned char>(rng() & 0xFF);

    SSL_CTX_set_session_id_context(ssl_ctx, session_id, sizeof(session_id));

    // TLS 1.3 每次握手只发 1 张票（默认 5）
    SSL_CTX_set_num_tickets(ssl_ctx, 1);

    // 会话超时 5 分钟
    SSL_CTX_set_timeout(ssl_ctx, 300);

    // 最多缓存 1024 个会话
    SSL_CTX_sess_set_cache_size(ssl_ctx, 1024);

    std::cout << "[tls] 会话缓存已启用 (max 1024, timeout 300s)" << std::endl;
}

bool TlsContext::Load(const std::string& cert_file,
                      const std::string& key_file,
                      const std::string& dh_file)
{
    try {
        ctx_.use_certificate_chain_file(cert_file);
        ctx_.use_private_key_file(key_file, asio::ssl::context::pem);
        if (!dh_file.empty())
            ctx_.use_tmp_dh_file(dh_file);
        loaded_ = true;
        std::cout << "[tls] 已加载证书: " << cert_file << std::endl;
        return true;
    } catch (asio::system_error& e) {
        std::cerr << "[tls] 证书加载失败: " << e.what() << std::endl;
        return false;
    }
}

asio::ssl::context& TlsContext::NativeContext() { return ctx_; }
const asio::ssl::context& TlsContext::NativeContext() const { return ctx_; }
