#!/usr/bin/env bash
# valgrind 内存分析
# 用法:
#   bash test_valgrind.sh              # 泄漏检测 (memcheck)
#   bash test_valgrind.sh --massif     # 堆分配速率 (massif)

set -euo pipefail

BOLD='\033[1m'
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

TOOL="memcheck"
MASSIF=false
for arg in "$@"; do
    case "$arg" in
        --massif) TOOL="massif"; MASSIF=true ;;
    esac
done

cd "$(dirname "$0")"

echo -e "${BOLD}[1/3] 编译 Release${NC}"
cmake --build build 2>/dev/null

OUTDIR="./valgrind_results"
mkdir -p "$OUTDIR"
TIMESTAMP=$(date '+%Y%m%d_%H%M%S')

echo -e "${BOLD}[2/3] valgrind --tool=$TOOL + 服务器${NC}"

if $MASSIF; then
    OUTFILE="${OUTDIR}/massif_${TIMESTAMP}.txt"
    VAL_OUTFILE="${OUTDIR}/massif_${TIMESTAMP}.out"
    valgrind --tool=massif \
        --massif-out-file="$VAL_OUTFILE" \
        --time-unit=ms \
        ./build/http_server config.yaml &
else
    OUTFILE="${OUTDIR}/valgrind_${TIMESTAMP}.txt"
    valgrind \
        --tool=memcheck \
        --leak-check=full \
        --show-leak-kinds=all \
        --track-origins=yes \
        --log-file="$OUTFILE" \
        ./build/http_server config.yaml &
fi

VAL_PID=$!
sleep 5

if ! kill -0 "$VAL_PID" 2>/dev/null; then
    echo -e "${RED}启动失败${NC}"
    exit 1
fi
echo "  valgrind PID=$VAL_PID"

echo -e "${BOLD}[3/3] 施压 + 退出${NC}"

# 简单请求（valgrind 下重量级测试太慢）
echo "  发送请求..."
curl -sk --connect-timeout 10 --max-time 30 -o /dev/null https://localhost:8443/healthz 2>/dev/null || echo "  curl healthz 失败"

if $MASSIF; then
    # massif 需要更多压力来产生有意义的分配图
    curl -sk --connect-timeout 10 --max-time 30 -o /dev/null https://localhost:8443/ 2>/dev/null || true
    curl -sk --connect-timeout 10 --max-time 30 -o /dev/null https://localhost:8443/hello 2>/dev/null || true
    echo "  wrk 20 conn × 10s（观察分配模式）..."
    wrk -t2 -c20 -d10s https://localhost:8443/ > /dev/null 2>&1 || true
fi

echo "  发送 SIGTERM..."
kill -TERM "$VAL_PID"

echo "  等待 valgrind 退出（最多 300s）..."
for i in $(seq 1 30); do
    sleep 10
    if ! kill -0 "$VAL_PID" 2>/dev/null; then
        echo "  valgrind 已退出 ($((i*10))s)"
        break
    fi
    echo "  等待中... $(($i*10))s"
done

if kill -0 "$VAL_PID" 2>/dev/null; then
    echo "  超时，强制终止..."
    kill -9 "$VAL_PID" 2>/dev/null || true
    sleep 1
fi

echo ""
echo -e "${BOLD}═════════════════════════════════════════${NC}"

if $MASSIF; then
    echo -e "${BOLD}  heap 分配分析 (massif)${NC}"
    echo -e "${BOLD}═════════════════════════════════════════${NC}"
    if [ -f "$VAL_OUTFILE" ]; then
        # 提取关键信息：总堆大小、分配次数
        echo ""
        echo -e "${BOLD}── 堆分配时间线 ──${NC}"
        ms_print "$VAL_OUTFILE" 2>&1 | head -50

        echo ""
        echo -e "${BOLD}── 摘要 ──${NC}"
        ms_print "$VAL_OUTFILE" 2>&1 | tail -20
    else
        echo -e "${RED}  massif 输出未生成${NC}"
        ls -la "$VAL_OUTFILE" 2>/dev/null || true
    fi
else
    echo -e "${BOLD}  valgrind 泄漏报告${NC}"
    echo -e "${BOLD}═════════════════════════════════════════${NC}"
    if [ -f "$OUTFILE" ]; then
        grep -E "definitely lost|indirectly lost|possibly lost|still reachable|suppressed|ERROR SUMMARY|LEAK SUMMARY" "$OUTFILE"
    fi
fi

echo ""
echo -e "${GREEN}日志: $OUTFILE${NC}"
