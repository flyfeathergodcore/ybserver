#!/usr/bin/env bash
# README 基准测试脚本
#
# 匹配 README.md 的 baseline 测试方法：
#   HTTP/1.1: wrk -t4 -cN -d30s
#   HTTP/2:   h2load -cN -nN×1000 -m10
#
# 用法:
#   bash test_bench.sh              # 完整测试（~15 分钟）
#   bash test_bench.sh --quick      # 快速版（10s/duration，~5 分钟）
#   bash test_bench.sh --h1-only    # 只测 HTTP/1.1
#   bash test_bench.sh --h2-only    # 只测 HTTP/2

set -euo pipefail

BOLD='\033[1m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m'

DURATION=30
H1_CONNS=(20 100 200 500 1000 1500 2000 2500 3000)
H2_CONNS=(100 200 500 1000 1500 2000 2500 3000)
TARGET="https://localhost:8443/"
QUICK=false
RUN_H1=true
RUN_H2=true

for arg in "$@"; do
    case "$arg" in
        --quick) QUICK=true ;;
        --h1-only) RUN_H2=false ;;
        --h2-only) RUN_H1=false ;;
    esac
done

if $QUICK; then
    DURATION=10
fi

cd "$(dirname "$0")"
SERVER="./build/http_server"
SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        timeout 5 wait "$SERVER_PID" 2>/dev/null || kill -9 "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# ── 编译 + 启动 ──
echo -e "${BOLD}[setup] 编译${NC}"
cmake --build build 2>/dev/null
echo -e "${BOLD}[setup] 启动服务器${NC}"
$SERVER config.yaml &
SERVER_PID=$!
sleep 2
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "服务器启动失败"
    exit 1
fi
echo "  PID=$SERVER_PID"

echo -e "\n${CYAN}══════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  基准测试 — $(date '+%Y-%m-%d %H:%M')${NC}"
echo -e "${CYAN}  $(uname -m) / $(nproc) cores / TLS 1.3${NC}"
echo -e "${CYAN}  target: $TARGET${NC}"
echo -e "${CYAN}══════════════════════════════════════════════════════${NC}"

# ═══════════════════════════
# HTTP/1.1
# ═══════════════════════════
if $RUN_H1; then
    echo -e "\n${BOLD}──────────────────────────────────────────────${NC}"
    echo -e "${BOLD}  HTTP/1.1 HTTPS  wrk -t4 -cN -d${DURATION}s${NC}"
    echo -e "${BOLD}──────────────────────────────────────────────${NC}"
    printf "  %-6s %12s %10s %10s\n" "连接数" "QPS" "p50延迟" "p99延迟"
    printf "  %-6s %12s %10s %10s\n" "------" "-----------" "--------" "--------"

    for c in "${H1_CONNS[@]}"; do
        result=$(wrk -t4 -c"$c" -d"${DURATION}s" --latency "$TARGET" 2>/dev/null)
        qps=$(echo "$result" | awk '/Requests\/sec:/ {print $2}')
        p50=$(echo "$result" | awk '$1 == "50%" {print $2}')
        p99=$(echo "$result" | awk '$1 == "99%" {print $2}')
        printf "  %-6s %12s %10s %10s\n" "$c" "$qps" "$p50" "$p99"
    done
fi

# ═══════════════════════════
# HTTP/2
# ═══════════════════════════
if $RUN_H2; then
    echo -e "\n${BOLD}──────────────────────────────────────────────${NC}"
    echo -e "${BOLD}  HTTP/2 HTTPS  h2load -cN -nN×1000 -m10${NC}"
    echo -e "${BOLD}──────────────────────────────────────────────${NC}"
    printf "  %-6s %12s\n" "连接数" "QPS"
    printf "  %-6s %12s\n" "------" "-----------"

    for c in "${H2_CONNS[@]}"; do
        n=$((c * 1000))
        result=$(h2load -c"$c" -n"$n" -m10 -t4 "$TARGET" 2>/dev/null)
        qps=$(echo "$result" | grep "finished in" | grep -oP '\d+\.\d+(?= req/s)')
        printf "  %-6s %12s\n" "$c" "${qps:-N/A}"
    done
fi

echo -e "\n${GREEN}测试完成${NC}"
