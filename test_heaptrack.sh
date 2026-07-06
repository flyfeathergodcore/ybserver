#!/usr/bin/env bash
# heaptrack 内存泄漏检测脚本
#
# 测试流程：启动服务器 → 施压 → 停止 → heaptrack 分析
# 用法:
#   bash test_heaptrack.sh              # 默认压力测试
#   bash test_heaptrack.sh --quick      # 轻量版（快速验证）
#   bash test_heaptrack.sh --view       # 完成后打开 GUI

set -euo pipefail

BOLD='\033[1m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
RED='\033[0;31m'
NC='\033[0m'

QUICK=false
VIEW=false
for arg in "$@"; do
    case "$arg" in
        --quick) QUICK=true ;;
        --view)  VIEW=true ;;
    esac
done

OUTDIR="./heaptrack_results"
mkdir -p "$OUTDIR"
TIMESTAMP=$(date '+%Y%m%d_%H%M%S')
OUTFILE="${OUTDIR}/heaptrack_${TIMESTAMP}"

cd "$(dirname "$0")"

# ── 编译 Release ──
echo -e "${BOLD}[1/4] 编译 Release${NC}"
cmake --build build 2>/dev/null || { echo "编译失败"; exit 1; }

# ── 启动 heaptrack ──
echo -e "${BOLD}[2/4] 启动 heaptrack + 服务器${NC}"
heaptrack -o "$OUTFILE" ./build/http_server config.yaml &
HEAPTRACK_PID=$!
sleep 3

if ! kill -0 "$HEAPTRACK_PID" 2>/dev/null; then
    echo -e "${RED}heaptrack 启动失败${NC}"
    exit 1
fi
echo "  heaptrack PID=$HEAPTRACK_PID"

# ── 施压 ──
echo -e "${BOLD}[3/4] 施压测试${NC}"

if $QUICK; then
    echo "  [quick] 200 conn × 30s + H2 多路复用 1000 req"
    wrk -t4 -c200 -d30s --latency https://localhost:8443/ > /dev/null 2>&1 &
    h2load -c100 -n100000 -m10 -t4 https://localhost:8443/ > /dev/null 2>&1
    wait
else
    echo "  H1 200 conn × 30s"
    timeout 40 wrk -t4 -c200 -d30s https://localhost:8443/ > /dev/null 2>&1 || echo "  [warn] wrk(200) 异常退出"

    echo "  H1 500 conn × 30s"
    timeout 40 wrk -t4 -c500 -d30s https://localhost:8443/ > /dev/null 2>&1 || echo "  [warn] wrk(500) 异常退出"

    echo "  H2 500 conn × 500000 req"
    timeout 30 h2load -c500 -n500000 -m10 -t4 https://localhost:8443/ > /dev/null 2>&1 || echo "  [warn] h2load(500) 异常退出"

    echo "  H1 1000 conn × 30s"
    timeout 40 wrk -t4 -c1000 -d30s https://localhost:8443/ > /dev/null 2>&1 || echo "  [warn] wrk(1000) 异常退出"

    echo "  H2 1000 conn × 1000000 req"
    timeout 40 h2load -c1000 -n1000000 -m10 -t4 https://localhost:8443/ > /dev/null 2>&1 || echo "  [warn] h2load(1000) 异常退出"

    echo "  混压 /healthz + /hello (各 15s)"
    timeout 25 wrk -t2 -c50 -d15s https://localhost:8443/healthz > /dev/null 2>&1 || true
    timeout 25 wrk -t2 -c50 -d15s https://localhost:8443/hello > /dev/null 2>&1 || true
fi

# 停掉可能残留的 wrk/h2load 进程
pkill -9 wrk 2>/dev/null || true
pkill -9 h2load 2>/dev/null || true

echo "  施压完成，等待服务器退出..."

# ── 停止服务器 ──
echo "  发送 SIGTERM..."
kill -TERM "$HEAPTRACK_PID" 2>/dev/null || true

echo "  等待 heaptrack 退出（最多 120s）..."
for i in $(seq 1 12); do
    sleep 10
    if ! kill -0 "$HEAPTRACK_PID" 2>/dev/null; then
        echo "  heaptrack 已退出 ($((i*10))s)"
        break
    fi
    echo "  等待中... $(($i*10))s"
done

if kill -0 "$HEAPTRACK_PID" 2>/dev/null; then
    echo "  超时，强制终止..."
    kill -9 "$HEAPTRACK_PID" 2>/dev/null || true
    sleep 2
fi

ZST_FILE="${OUTFILE}.zst"
if [ ! -f "$ZST_FILE" ]; then
    # 可能后处理中，等一会
    sleep 5
fi

echo -e "${BOLD}[4/4] 分析结果${NC}"

if [ ! -f "$ZST_FILE" ]; then
    echo -e "${RED}  heaptrack 输出未生成: $ZST_FILE${NC}"
    echo "  检查 /tmp 是否有 heaptrack 文件:"
    ls -lh /tmp/heaptrack* 2>/dev/null || true
    exit 1
fi

echo "  文件: $ZST_FILE"
echo ""

# 用 heaptrack_print 生成摘要
SUMMARY_FILE="${OUTFILE}_summary.txt"
heaptrack_print "$ZST_FILE" > "$SUMMARY_FILE" 2>&1 || true

# 提取关键信息
echo -e "${BOLD}── 峰值内存 ──${NC}"
grep -i "peak" "$SUMMARY_FILE" | head -5

echo -e "\n${BOLD}── 泄漏分析 ──${NC}"
LEAK_LINES=$(grep -i -A 20 "leak\|unfreed\|未释放\|still reachable\|definitely lost" "$SUMMARY_FILE" | head -40)
if [ -n "$LEAK_LINES" ]; then
    echo "$LEAK_LINES"
else
    echo "  无泄漏关键词命中，查看详细分配统计:"
    grep -i -A 10 "temporary allocation\|total memory\|allocations" "$SUMMARY_FILE" | head -20
fi

echo -e "\n${BOLD}── 摘要 ──${NC}"
head -30 "$SUMMARY_FILE"

echo ""
echo -e "${GREEN}结果文件:${NC}"
echo "  数据: $ZST_FILE"
echo "  摘要: $SUMMARY_FILE"

if $VIEW && command -v heaptrack_gui &>/dev/null; then
    echo -e "\n${BOLD}启动 heaptrack_gui ...${NC}"
    heaptrack_gui "$ZST_FILE" &>/dev/null &
fi

# 清理服务器进程
kill "$HEAPTRACK_PID" 2>/dev/null || true
timeout 5 wait "$HEAPTRACK_PID" 2>/dev/null || kill -9 "$HEAPTRACK_PID" 2>/dev/null || true

echo -e "\n${GREEN}完成${NC}"
