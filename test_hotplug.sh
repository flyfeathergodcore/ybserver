#!/usr/bin/env bash
BOLD='\033[1m'
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'
PASS=0
FAIL=0

pass() { echo -e "  ${GREEN}✓${NC} $1"; PASS=$((PASS + 1)); }
fail() { echo -e "  ${RED}✗${NC} $1"; FAIL=$((FAIL + 1)); }

cd "$(dirname "$0")"

# ── 1. 编译 ──
echo -e "${BOLD}[1/6] 编译项目${NC}"
cmake --build build 2>/dev/null && pass "编译成功" || fail "编译失败"

# ── 2. 启动服务 ──
echo -e "${BOLD}[2/6] 启动服务${NC}"
kill http_server 2>/dev/null || true
./build/http_server config.yaml &
SERVER_PID=$!
sleep 2
if kill -0 $SERVER_PID 2>/dev/null; then
    pass "服务已启动 (PID=$SERVER_PID)"
else
    fail "服务启动失败"
    echo "  日志:"
    exit 1
fi

# ── 3. 测试路由 ──
echo -e "${BOLD}[3/6] 测试路由${NC}"

# healthz
code=$(curl -sk -o /dev/null -w "%{http_code}" https://localhost:8443/healthz 2>/dev/null)
[[ "$code" == "200" ]] && pass "/healthz → 200" || fail "/healthz → $code (期望 200)"

# echo (非 WS 请求应返回 404)
code=$(curl -sk -o /dev/null -w "%{http_code}" https://localhost:8443/echo 2>/dev/null)
[[ "$code" == "404" ]] && pass "/echo (GET) → 404" || fail "/echo → $code (期望 404)"

# hello (热插拔 .so handler)
body=$(curl -sk https://localhost:8443/hello 2>/dev/null)
[[ "$body" == *"hot-plug"* ]] && pass "/hello → 包含 hot-plug" || fail "/hello → 未包含预期内容"

# 静态文件
code=$(curl -sk -o /dev/null -w "%{http_code}" https://localhost:8443/ 2>/dev/null)
[[ "$code" == "200" ]] && pass "/ (静态文件) → 200" || fail "/ → $code (期望 200)"

# 404 未路由
code=$(curl -sk -o /dev/null -w "%{http_code}" https://localhost:8443/nonexistent 2>/dev/null)
[[ "$code" == "404" ]] && pass "/nonexistent → 404" || fail "/nonexistent → $code (期望 404)"

# ── 4. 测试 WebSocket 升级 ──
echo -e "${BOLD}[4/6] 测试 WebSocket 升级${NC}"
python3 test_ws.py 2>&1 && pass "WebSocket 连接测试通过" || fail "WebSocket 连接测试失败"

# ── 5. 测试热重载 ──
echo -e "${BOLD}[5/6] 测试热重载${NC}"

# 修改 .so 源码，加标记
sed -i 's/hot-plug handler/hot-plug handler (reloaded)/' handlers/example_handler.cpp
echo "  [test] 编译新版 .so ..."
cmake --build build --target example_handler 2>/dev/null
echo "  [test] 等待 6 秒让 inotify + stat 兜底检测到变化..."
sleep 6

body2=$(curl -sk https://localhost:8443/hello 2>/dev/null)
if [[ "$body2" == *"reloaded"* ]]; then
    pass "/hello 热重载成功 → 包含 reloaded"
else
    fail "/hello 热重载失败 → 仍为旧内容"
fi

# 恢复源码
sed -i 's/hot-plug handler (reloaded)/hot-plug handler/' handlers/example_handler.cpp
cmake --build build --target example_handler 2>/dev/null
sleep 3

# ── 6. 停止服务 ──
echo -e "${BOLD}[6/6] 停止服务${NC}"
kill $SERVER_PID 2>/dev/null; wait $SERVER_PID 2>/dev/null
pass "服务已停止"

# ── 汇总 ──
echo ""
echo -e "${BOLD}结果: ${PASS} 通过, ${FAIL} 失败${NC}"
[[ $FAIL -eq 0 ]]
