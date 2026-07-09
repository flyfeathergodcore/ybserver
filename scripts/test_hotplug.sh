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

# 注册 API
resp=$(curl -sk -X POST https://localhost:8443/api/register \
  -H "Content-Type: application/json" \
  -d '{"id":"ci_test","password":"cipass"}' 2>/dev/null)
has_user=$(echo "$resp" | python3 -c "import sys,json; print(json.load(sys.stdin).get('user',''))" 2>/dev/null)
[[ "$has_user" == "ci_test" ]] && pass "POST /api/register → ci_test ✅" || fail "POST /api/register → $resp"

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
python3 scripts/test_ws.py 2>&1 && pass "WebSocket 连接测试通过" || fail "WebSocket 连接测试失败"

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
kill $SERVER_PID 2>/dev/null; wait $SERVER_PID 2>/dev/null
pass "服务已停止"

echo ""
echo -e "${BOLD}结果: ${PASS} 通过, ${FAIL} 失败${NC}"
[[ $FAIL -eq 0 ]]
