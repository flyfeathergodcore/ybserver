#pragma once
#include <string>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <nlohmann/json.hpp>
#include <mutex>

// ═══════════════════════════════════════════════════════════════════
// RSA 公钥验签工具（供 chat_handler 等业务 handler 使用）
//
// 在 register_routes 中调用 auth_init_verify() 加载公钥，
// 然后在 Handle/HandleStream 中调用 auth_verify_token(token) 验证。
// ═══════════════════════════════════════════════════════════════════

/// 初始化公钥（从 PEM 文件加载）
/// 线程安全，可重复调用
inline bool auth_init_verify()
{
    static EVP_PKEY* pubkey = nullptr;
    static std::once_flag flag;

    std::call_once(flag, [] {
        FILE* f = fopen("/app/auth_rsa_pub.pem", "re");
        if (!f) return;
        pubkey = PEM_read_PUBKEY(f, nullptr, nullptr, nullptr);
        fclose(f);
    });
    return pubkey != nullptr;
}

/// Base64url 解码
inline std::string auth_b64_decode(const std::string& s)
{
    std::string copy = s;
    for (auto& c : copy) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    while (copy.size() % 4) copy += '=';

    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* mem = BIO_new_mem_buf(copy.data(), static_cast<int>(copy.size()));
    BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);

    std::string out(copy.size(), '\0');
    int n = BIO_read(b64, &out[0], static_cast<int>(out.size()));
    BIO_free_all(b64);
    if (n > 0) out.resize(n); else out.clear();
    return out;
}

/// RSA-SHA256 验签
inline bool auth_rsa_verify(const std::string& data, const std::string& signature)
{
    static EVP_PKEY* pubkey = nullptr;
    {
        static std::once_flag flag;
        std::call_once(flag, [] {
            FILE* f = fopen("/app/auth_rsa_pub.pem", "re");
            if (f) { pubkey = PEM_read_PUBKEY(f, nullptr, nullptr, nullptr); fclose(f); }
        });
    }
    if (!pubkey) return false;

    auto sig_raw = auth_b64_decode(signature);

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return false;

    EVP_VerifyInit(ctx, EVP_sha256());
    EVP_VerifyUpdate(ctx, data.data(), data.size());
    int ok = EVP_VerifyFinal(ctx,
        reinterpret_cast<const unsigned char*>(sig_raw.data()),
        static_cast<unsigned int>(sig_raw.size()), pubkey);
    EVP_MD_CTX_free(ctx);
    return ok == 1;
}

/// 验证 JWT token，返回 user_id（空串=无效）
inline std::string auth_verify_token(const std::string& token)
{
    auto dot1 = token.find('.');
    if (dot1 == std::string::npos) return {};
    auto dot2 = token.find('.', dot1 + 1);
    if (dot2 == std::string::npos) return {};

    std::string content = token.substr(0, dot2);
    std::string sig     = token.substr(dot2 + 1);

    if (!auth_rsa_verify(content, sig)) return {};

    auto p_b64 = token.substr(dot1 + 1, dot2 - dot1 - 1);
    auto p_json = auth_b64_decode(p_b64);
    if (p_json.empty()) return {};

    try {
        auto body = nlohmann::json::parse(p_json);
        long exp = body.value("exp", 0L);
        if (time(nullptr) >= exp) return {};
        return body.value("user", "");
    } catch (...) {
        return {};
    }
}
