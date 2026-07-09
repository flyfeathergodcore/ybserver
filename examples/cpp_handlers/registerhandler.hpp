#pragma once
#include "handler/request_handler.hpp"
#include "http/context.hpp"
#include "net/response.hpp"
#include <asio.hpp>
#include <mysql/mysql.h>
#include <memory>

// ═══════════════════════════════════════════════════════════════════
// AuthHandler — 用户注册 & 登录（MySQL 后端）
//
// POST /api/login     { "id":"user", "password":"pass" }
// POST /api/register  { "id":"user", "password":"pass" }
// ═══════════════════════════════════════════════════════════════════

class AuthHandler : public RequestHandler {
public:
    enum Route { LOGIN, REGISTER, VERIFY, LOGOUT };

    AuthHandler(MYSQL* conn, Route route);

    Response Handle(const Context& ctx) override;

private:
    Response do_login(const std::string& id, const std::string& password, SessionRegion* pool);
    Response do_register(const std::string& id, const std::string& password, SessionRegion* pool);
    Response do_verify(std::string_view auth, SessionRegion* pool);
    Response do_logout(std::string_view auth, SessionRegion* pool);

    MYSQL* conn_;
    Route route_;
};
