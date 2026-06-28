#!/bin/bash
# ═══════════════════════════════════════════════════════════════
# HTTP Server Terminal HUD
# Polls /metrics.json every second and renders ANSI output.
# Requires: curl, jq
# Usage:   ./tools/hud.sh [url]
# Default: https://127.0.0.1:8443/metrics.json
# ═══════════════════════════════════════════════════════════════

URL="${1:-https://127.0.0.1:8443/metrics.json}"
CURL_OPTS="-sk"

BAR_WIDTH=40

# ANSI setup
tput civis 2>/dev/null           # hide cursor
tput smcup 2>/dev/null           # save screen
trap 'tput cnorm 2>/dev/null; tput rmcup 2>/dev/null; tput sgr0 2>/dev/null; clear; exit' INT TERM

draw_bar() {
    local val=$1 max=$2 label=$3 char=$4
    local pct=0
    (( max > 0 )) && pct=$(( val * BAR_WIDTH / max ))
    (( pct > BAR_WIDTH )) && pct=$BAR_WIDTH
    printf "\e[1m%s\e[0m %s" "$label" "$char"
    for ((i=0; i<pct; i++)); do echo -n "$char"; done
}

echo -e "\e[1;36m    HTTP Server Dashboard (HUD)\e[0m"
echo -e "\e[2m    Polling: $URL\e[0m"
echo

while true; do
    data=$(curl $CURL_OPTS "$URL" 2>/dev/null)
    if [ -z "$data" ]; then
        echo -ne "\e[1;31m  ✗ Connection failed — retrying...\e[0m\033[K\n"
        sleep 1
        continue
    fi

    active=$(echo "$data" | jq '.active_connections // 0')
    uptime=$(echo "$data" | jq '.uptime_seconds // 0')
    qps=$(echo "$data" | jq '.history[-1].qps // 0')
    err=$(echo "$data" | jq '.history[-1].err // 0')
    p50=$(echo "$data" | jq '.history[-1].p50 // 0')
    p90=$(echo "$data" | jq '.history[-1].p90 // 0')
    p99=$(echo "$data" | jq '.history[-1].p99 // 0')
    bytes=$(echo "$data" | jq '.history[-1].bytes // 0')

    now=$(date '+%H:%M:%S')

    # Determine bar max for QPS scaling
    max_qps=$(( qps > 10000 ? qps : 50000 ))

    # ── Render ──
    echo -ne "\033[H"  # cursor home

    echo -e "\e[1;36m  ┌──────────────────────────────────────┐\e[0m"
    echo -e "\e[1;36m  │     HTTP Server Dashboard (HUD)      │\e[0m"
    echo -e "\e[1;36m  └──────────────────────────────────────┘\e[0m"
    echo

    # QPS bar
    printf "  \e[1;33mQPS\e[0m     \e[1;32m%'8d\e[0m  │" "$qps"
    local pct=$(( qps * BAR_WIDTH / max_qps ))
    (( pct > BAR_WIDTH )) && pct=$BAR_WIDTH
    for ((i=0; i<pct; i++)); do echo -n "█"; done
    echo -e "\e[0m"

    # Error rate
    local err_pct=0
    (( qps > 0 )) && err_pct=$(( err * 100 / qps ))
    if (( err > 0 )); then
        printf "  \e[1;33mErrors\e[0m  \e[1;31m%'8d\e[0m  (%d%%)\n" "$err" "$err_pct"
    else
        printf "  \e[1;33mErrors\e[0m  \e[1;32m%'8d\e[0m  ✓\n" "$err"
    fi

    # Bytes/s
    local mb=0
    (( bytes > 0 )) && mb=$(( bytes / 1048576 ))
    printf "  \e[1;33mThroughput\e[0m  \e[1m%'8d\e[0m B/s  (%d MB/s)\n" "$bytes" "$mb"

    echo
    printf "  \e[1;33mLatency\e[0m\n"
    printf "    p50  \e[1;32m%'6d\e[0m µs\n" "$p50"
    printf "    p90  \e[1;36m%'6d\e[0m µs\n" "$p90"
    printf "    p99  \e[1;31m%'6d\e[0m µs\n" "$p99"

    echo
    printf "  \e[1;33mActive\e[0m  %'d\n" "$active"
    printf "  \e[1;33mUptime\e[0m %dm %ds\n" $(( uptime / 60 )) $(( uptime % 60 ))
    echo
    echo -e "  \e[2m$now — Ctrl+C to exit\e[0m\033[K"

    sleep 0.9  # slightly less than 1s to avoid drift
done
