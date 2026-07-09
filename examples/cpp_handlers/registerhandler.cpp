#include "examples/cpp_handlers/registerhandler.hpp"
#include "handler/router.hpp"
#include "log/logger.hpp"
#include <nlohmann/json.hpp>
#include <mysql/mysql.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <iostream>
#include <memory>
#include <cstring>
#include <mutex>
#include <ctime>

// ═══════════════════════════════════════════════════════════════════
// RSA 签名工具（RS256 = RSA + SHA-256）
// ═══════════════════════════════════════════════════════════════════

static EVP_PKEY* g_rsa_key = nullptr;
static std::mutex g_rsa_mutex;

/// 加载或生成 RSA-2048 密钥对
static bool rsa_init()
{
    std::lock_guard<std::mutex> lock(g_rsa_mutex);
    if (g_rsa_key) return true;

    const char* key_path = "/app/auth_rsa_key.pem";
    FILE* f = fopen(key_path, "re");
    if (f) {
        g_rsa_key = PEM_read_PrivateKey(f, nullptr, nullptr, nullptr);
        fclose(f);
        if (g_rsa_key) return true;
    }

    // 生成 RSA-2048 密钥对
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!ctx) return false;
    EVP_PKEY_keygen_init(ctx);
    EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048);
    EVP_PKEY* key = nullptr;
    EVP_PKEY_keygen(ctx, &key);
    EVP_PKEY_CTX_free(ctx);
    if (!key) return false;

    // 保存私钥
    FILE* fout = fopen(key_path, "we");
    if (fout) {
        PEM_write_PrivateKey(fout, key, nullptr, nullptr, 0, nullptr, nullptr);
        fclose(fout);
    }
    // 保存公钥（供业务 handler 验签）
    fout = fopen("/app/auth_rsa_pub.pem", "we");
    if (fout) {
        PEM_write_PUBKEY(fout, key);
        fclose(fout);
    }
    g_rsa_key = key;
    std::cout << "[auth] RSA-2048 key generated" << std::endl;
    return true;
}

/// Base64url 编码（无补全 =）
static std::string base64url(const unsigned char* data, size_t len)
{
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* mem = BIO_new(BIO_s_mem());
    BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, data, static_cast<int>(len));
    (void)BIO_flush(b64);

    const char* encoded;
    long e_len = BIO_get_mem_data(mem, &encoded);
    std::string s(encoded, e_len);
    BIO_free_all(b64);

    // base64 → base64url（替换 + / 和去掉 =）
    for (auto& c : s) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    auto pos = s.find('=');
    if (pos != std::string::npos) s.resize(pos);
    return s;
}

/// Base64url 解码
static std::string unbase64url(const std::string& s)
{
    std::string copy = s;
    for (auto& c : copy) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    // 补全 =
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

/// RSA-SHA256 签名
static std::string rsa_sign(const std::string& data)
{
    std::lock_guard<std::mutex> lock(g_rsa_mutex);
    if (!g_rsa_key) return {};

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return {};

    EVP_SignInit(ctx, EVP_sha256());
    EVP_SignUpdate(ctx, data.data(), data.size());

    unsigned char sig[256];
    unsigned int sig_len = sizeof(sig);
    int ok = EVP_SignFinal(ctx, sig, &sig_len, g_rsa_key);
    EVP_MD_CTX_free(ctx);

    if (!ok) return {};
    return base64url(sig, sig_len);
}

/// RSA-SHA256 验签
static bool rsa_verify(const std::string& data, const std::string& signature)
{
    std::lock_guard<std::mutex> lock(g_rsa_mutex);
    if (!g_rsa_key) return false;

    auto sig_raw = unbase64url(signature);

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return false;

    EVP_VerifyInit(ctx, EVP_sha256());
    EVP_VerifyUpdate(ctx, data.data(), data.size());

    int ok = EVP_VerifyFinal(ctx, sig_raw.data(),
        static_cast<unsigned int>(sig_raw.size()), g_rsa_key);
    EVP_MD_CTX_free(ctx);
    return ok == 1;
}

// ── 创建 token（JWT 风格: header.payload.signature）──
static std::string create_token(const std::string& user_id)
{
    long now = static_cast<long>(time(nullptr));

    // header: {"alg":"RS256","typ":"JWT"}
    std::string header = R"({"alg":"RS256","typ":"JWT"})";

    // payload: {"user":"xxx","iat":123,"exp":123+86400}
    std::string payload = R"({"user":")" + user_id
        + R"(","iat":)" + std::to_string(now)
        + R"(,"exp":)" + std::to_string(now + 86400) + R"(})";

    auto h_b64 = base64url(
        reinterpret_cast<const unsigned char*>(header.data()), header.size());
    auto p_b64 = base64url(
        reinterpret_cast<const unsigned char*>(payload.data()), payload.size());

    std::string content = h_b64 + "." + p_b64;
    auto sig = rsa_sign(content);
    if (sig.empty()) return {};

    return content + "." + sig;
}

// ── 验证 token，返回 user_id（空串表示无效）──
static std::string verify_token(const std::string& token)
{
    // 解析 header.payload.signature
    auto dot1 = token.find('.');
    if (dot1 == std::string::npos) return {};
    auto dot2 = token.find('.', dot1 + 1);
    if (dot2 == std::string::npos) return {};

    std::string content = token.substr(0, dot2);       // header.payload
    std::string sig     = token.substr(dot2 + 1);      // signature

    if (!rsa_verify(content, sig)) return {};

    // 解析 payload 中的 user 和 exp
    auto p_b64 = token.substr(dot1 + 1, dot2 - dot1 - 1);
    auto p_json = unbase64url(p_b64);
    if (p_json.empty()) return {};

    try {
        auto body = nlohmann::json::parse(p_json);
        long exp = body.value("exp", 0L);
        if (time(nullptr) >= exp) return {};  // 过期
        return body.value("user", "");
    } catch (...) {
        return {};
    }
}

// ═══════════════════════════════════════════════════════════════════
// 辅助函数
// ═══════════════════════════════════════════════════════════════════

static Response json_resp(int code, const std::string& body, SessionRegion* pool)
{
    if (!pool) return Response::Error(500, *pool);
    Response r(code, *pool);
    r.Header("Content-Type", "application/json");
    r.Header("Content-Length", static_cast<uint64_t>(body.size()));
    r.EndHeaders();
    pool->Write(body);
    return r;
}

static bool parse_json(const Context& ctx,
                       std::string& id, std::string& password)
{
    nlohmann::json body;
    try { body = nlohmann::json::parse(ctx.Body()); }
    catch (...) { return false; }
    id = body.value("id", "");
    password = body.value("password", "");
    return !id.empty() && !password.empty();
}

static bool mysql_has_row(MYSQL* conn, const std::string& sql)
{
    if (mysql_query(conn, sql.c_str()) != 0) return false;
    MYSQL_RES* res = mysql_store_result(conn);
    if (!res) return false;
    bool has = mysql_num_rows(res) > 0;
    mysql_free_result(res);
    return has;
}

static bool mysql_exec(MYSQL* conn, const std::string& sql)
{
    return mysql_query(conn, sql.c_str()) == 0;
}

static std::string extract_bearer(std::string_view auth)
{
    if (auth.empty()) return {};
    static const std::string prefix = "Bearer ";
    if (auth.substr(0, prefix.size()) == prefix)
        return std::string(auth.substr(prefix.size()));
    return std::string(auth);
}

// ═══════════════════════════════════════════════════════════════════
// AuthHandler
// ═══════════════════════════════════════════════════════════════════

AuthHandler::AuthHandler(MYSQL* conn, Route route) : conn_(conn), route_(route) {}

Response AuthHandler::Handle(const Context& ctx)
{
    auto* pool = ctx.Pool();
    if (!pool) return Response::Error(500, *ctx.Pool());

    std::string id, password;
    auto auth = ctx.Header("authorization");

    switch (route_) {
    case LOGIN:
        if (!parse_json(ctx, id, password))
            return json_resp(400, R"({"error":"invalid input"})", pool);
        return do_login(id, password, pool);
    case REGISTER:
        if (!parse_json(ctx, id, password))
            return json_resp(400, R"({"error":"invalid input"})", pool);
        return do_register(id, password, pool);
    case VERIFY:
        return do_verify(auth, pool);
    case LOGOUT:
        return do_logout(auth, pool);
    }
    return json_resp(500, R"({"error":"unknown route"})", pool);
}

Response AuthHandler::do_login(const std::string& id,
                                const std::string& password,
                                SessionRegion* pool)
{
    std::string sql = "SELECT id FROM users WHERE id = '"
        + id + "' AND password = '" + password + "'";
    if (!mysql_has_row(conn_, sql))
        return json_resp(404, R"({"error":"user not found or password mismatch"})", pool);

    std::string token = create_token(id);
    if (token.empty())
        return json_resp(500, R"({"error":"token generation failed"})", pool);

    Logger::Instance().Business("AUTH", "login", "user=" + id);
    return json_resp(200, R"({"status":"ok","user":")" + id
        + R"(","token":")" + token + R"("})", pool);
}

Response AuthHandler::do_register(const std::string& id,
                                   const std::string& password,
                                   SessionRegion* pool)
{
    std::string sql = "SELECT id FROM users WHERE id = '" + id + "'";
    if (mysql_has_row(conn_, sql))
        return json_resp(409, R"({"error":"user already exists"})", pool);

    sql = "INSERT INTO users (id, password) VALUES ('"
        + id + "','" + password + "')";
    if (!mysql_exec(conn_, sql))
        return json_resp(500, R"({"error":"create user failed"})", pool);

    std::string token = create_token(id);
    Logger::Instance().Business("AUTH", "register", "user=" + id);
    return json_resp(200, R"({"status":"created","user":")" + id
        + R"(","token":")" + token + R"("})", pool);
}

Response AuthHandler::do_verify(std::string_view auth, SessionRegion* pool)
{
    std::string token = extract_bearer(auth);
    if (token.empty())
        return json_resp(401, R"({"error":"missing authorization"})", pool);

    std::string user = verify_token(token);
    if (user.empty())
        return json_resp(401, R"({"error":"invalid or expired token"})", pool);

    return json_resp(200, R"({"status":"ok","user":")" + user + R"("})", pool);
}

Response AuthHandler::do_logout(std::string_view auth, SessionRegion* pool)
{
    // RSA 签名 token 是无状态的，logout 只是客户端丢弃 token
    // 服务端可以维护一个黑名单（可选）
    return json_resp(200, R"({"status":"ok"})", pool);
}

// ── 热插拔入口 ──
extern "C" void register_routes(Router& router)
{
    // 初始化 RSA 密钥
    if (!rsa_init()) {
        std::cerr << "[auth] RSA init failed" << std::endl;
        return;
    }

    // 连接 MySQL
    static MYSQL* conn = nullptr;
    static std::once_flag init_flag;
    std::call_once(init_flag, [] {
        conn = mysql_init(nullptr);
        if (conn) {
            mysql_real_connect(conn, "mysql", "webcpp", "webcpp123",
                               "webcpp", 3306, nullptr, 0);
            std::cout << "[auth] MySQL connected" << std::endl;
        }
    });

    if (!conn) {
        std::cerr << "[auth] MySQL not available" << std::endl;
        return;
    }

    router.Post("/api/login",    std::make_unique<AuthHandler>(conn, AuthHandler::LOGIN));
    router.Post("/api/register", std::make_unique<AuthHandler>(conn, AuthHandler::REGISTER));
    router.Post("/api/verify",   std::make_unique<AuthHandler>(conn, AuthHandler::VERIFY));
    router.Post("/api/logout",   std::make_unique<AuthHandler>(conn, AuthHandler::LOGOUT));
    std::cout << "[auth] RSA + MySQL ready" << std::endl;
}
