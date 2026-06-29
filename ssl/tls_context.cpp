#include "ssl/tls_context.hpp"
#include <cstring>
#include <iostream>
#include <random>
#include <openssl/ssl.h>

static int alpn_select_cb(SSL* /*ssl*/,
                          const unsigned char** out, unsigned char* outlen,
                          const unsigned char* in, unsigned int inlen,
                          void* /*arg*/)
{
    // Wire format: "\x02h2\x08http/1.1"
    static const unsigned char h2_protos[] = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};

    if (SSL_select_next_proto((unsigned char**)out, outlen,
                              h2_protos, sizeof(h2_protos),
                              in, inlen) >= 0)
        return SSL_TLSEXT_ERR_OK;
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

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

    // ── ALPN ──
    SSL_CTX_set_alpn_select_cb(ctx_.native_handle(), alpn_select_cb, nullptr);

    // ── TLS 会话缓存 ──
    auto* ssl_ctx = ctx_.native_handle();

    SSL_CTX_set_session_cache_mode(ssl_ctx, SSL_SESS_CACHE_SERVER);

    unsigned char session_id[32];
    const char* prefix = "webcpp-srv-1";
    std::memcpy(session_id, prefix, std::strlen(prefix));

    std::mt19937 rng(std::random_device{}());
    for (size_t i = std::strlen(prefix); i < sizeof(session_id); ++i)
        session_id[i] = static_cast<unsigned char>(rng() & 0xFF);

    SSL_CTX_set_session_id_context(ssl_ctx, session_id, sizeof(session_id));

    SSL_CTX_set_num_tickets(ssl_ctx, 1);
    SSL_CTX_set_timeout(ssl_ctx, 300);
    SSL_CTX_sess_set_cache_size(ssl_ctx, 1024);

    std::cout << "[tls] 会话缓存已启用 (max 1024, timeout 300s)" << std::endl;
}

bool TlsContext::IsHttp2(SSL* ssl)
{
    if (!ssl) return false;
    const unsigned char* alpn = nullptr;
    unsigned int alpn_len = 0;
    SSL_get0_alpn_selected(ssl, &alpn, &alpn_len);
    return (alpn && alpn_len == 2 && std::memcmp(alpn, "h2", 2) == 0);
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
