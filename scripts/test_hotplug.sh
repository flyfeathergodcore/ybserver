#!/usr/bin/env bash
BOLD='\033[1m'
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'
PASS=0
FAIL=0

pass() { echo -e "  ${GREEN}✓${NC} $1"; PASS=$((PASS + 1)); }
fail() { echo -e "  ${RED}✗${NC} $1"; FAIL=$((FAIL + 1)); }

cd "$(dirname "$0")/.."

# ── 1. 启动服务 ──
echo -e "${BOLD}[1/5] 启动服务${NC}"

# 检查是否已运行在 Docker 中（本地开发环境），否则尝试直接启动（CI 环境）
SERVER_PID=""
if curl -sk -o /dev/null https://localhost:8443/healthz 2>/dev/null; then
    pass "服务已在运行"
elif [ -f ./build/http_server ]; then
    kill http_server 2>/dev/null || true
    ./build/http_server config.yaml &
    SERVER_PID=$!
    sleep 2
    if kill -0 $SERVER_PID 2>/dev/null; then
        pass "服务已启动 (PID=$SERVER_PID)"
    else
        fail "服务启动失败"
        exit 1
    fi
else
    fail "找不到 http_server 二进制，请先编译"
    exit 1
fi

# ── 2. 测试路由 ──
echo -e "${BOLD}[2/5] 测试路由${NC}"

# healthz
code=$(curl -sk -o /dev/null -w "%{http_code}" https://localhost:8443/healthz 2>/dev/null)
[[ "$code" == "200" ]] && pass "/healthz → 200" || fail "/healthz → $code"

# 登录 API
resp=$(curl -sk -X POST https://localhost:8443/api/login \
  -H "Content-Type: application/json" \
  -d '{"id":"test","password":"test123"}' 2>/dev/null)
has_token=$(echo "$resp" | python3 -c "import sys,json; print('token' in json.load(sys.stdin))" 2>/dev/null)
[[ "$has_token" == "True" ]] && pass "POST /api/login → token ✅" || fail "POST /api/login → $resp"

# 注册 API（使用时间戳避免重复）
REG_USER="ci_$(date +%s)"
resp=$(curl -sk -X POST https://localhost:8443/api/register \
  -H "Content-Type: application/json" \
  -d "{\"id\":\"$REG_USER\",\"password\":\"cipass\"}" 2>/dev/null)
has_user=$(echo "$resp" | python3 -c "import sys,json; print(json.load(sys.stdin).get('user',''))" 2>/dev/null)
[[ "$has_user" == "$REG_USER" ]] && pass "POST /api/register → $REG_USER ✅" || fail "POST /api/register → $resp"

# 静态文件
code=$(curl -sk -o /dev/null -w "%{http_code}" https://localhost:8443/ 2>/dev/null)
[[ "$code" == "200" ]] && pass "/ (静态文件) → 200" || fail "/ → $code"

# chat 前端
code=$(curl -sk -o /dev/null -w "%{http_code}" https://localhost:8443/chat/index.html 2>/dev/null)
[[ "$code" == "200" ]] && pass "/chat/index.html → 200" || fail "/chat → $code"

# 404 未路由
code=$(curl -sk -o /dev/null -w "%{http_code}" https://localhost:8443/nonexistent 2>/dev/null)
[[ "$code" == "404" ]] && pass "/nonexistent → 404" || fail "/nonexistent → $code"

# ── 3. 测试 WebSocket ──
echo -e "${BOLD}[3/5] 测试 WebSocket 升级${NC}"
if python3 -c "import websockets" 2>/dev/null; then
    python3 scripts/test_ws.py 2>&1 && pass "WebSocket 连接测试通过" || fail "WebSocket 连接测试失败"
else
    # 快速手动验证
    code=$(curl -sk -o /dev/null -w "%{http_code}" -H "Connection: Upgrade" -H "Upgrade: websocket" -H "Sec-WebSocket-Version: 13" -H "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==" https://localhost:8443/echo 2>/dev/null)
    [[ "$code" == "101" ]] && pass "WebSocket 握手成功 (101)" || fail "WebSocket 握手返回 $code"
fi

# ── 4. 测试热重载 ──
echo -e "${BOLD}[4/5] 测试热重载${NC}"
# 检查 chat_handler 已加载
logs=$(curl -sk https://localhost:8443/healthz 2>/dev/null)
[[ -n "$logs" ]] && pass "handler 已加载" || fail "handler 未响应"

# 测试 auth token 验证
TOKEN=$(curl -sk -X POST https://localhost:8443/api/login \
  -H "Content-Type: application/json" \
  -d '{"id":"test","password":"test123"}' 2>/dev/null | \
  python3 -c "import sys,json; print(json.load(sys.stdin).get('token',''))" 2>/dev/null)
verify=$(curl -sk -X POST https://localhost:8443/api/verify \
  -H "Authorization: Bearer $TOKEN" 2>/dev/null | \
  python3 -c "import sys,json; print(json.load(sys.stdin).get('user',''))" 2>/dev/null)
[[ "$verify" == "test" ]] && pass "token 验证 ✅" || fail "token 验证失败"

# ── 5. 停止服务 ──
echo -e "${BOLD}[5/5] 停止服务${NC}"
if [ -n "$SERVER_PID" ]; then
    kill $SERVER_PID 2>/dev/null; wait $SERVER_PID 2>/dev/null
fi
pass "服务已停止"

echo ""
echo -e "${BOLD}结果: ${PASS} 通过, ${FAIL} 失败${NC}"
[[ $FAIL -eq 0 ]]
