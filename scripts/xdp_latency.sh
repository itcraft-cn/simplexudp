#!/usr/bin/env bash
# xdp_latency.sh - 在 veth 上测 AF_XDP 时延，并用 bpftool 佐证流量经过 XDP
#
# 用法：sudo scripts/xdp_latency.sh [build/test_latency]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$ROOT/build/test_latency}"
IF0=sx0
IF1=sx1
IP0=10.10.0.1
IP1=10.10.0.2
PORT=31000
ROUNDS=20000
HOLD_MS=6000

log() { echo "[xdp_latency] $*"; }

cleanup() {
    [ -n "${PID:-}" ] && kill "$PID" 2>/dev/null || true
    ip link del "$IF0" 2>/dev/null || true
}
trap cleanup EXIT

if [ "$(id -u)" != "0" ]; then
    log "需要 root"
    exit 2
fi
if [ ! -x "$BIN" ]; then
    log "未找到 $BIN（先 make tests-latency）"
    exit 2
fi

ip link del "$IF0" 2>/dev/null || true
log "创建 veth: $IF0 <-> $IF1"
ip link add "$IF0" type veth peer name "$IF1" || exit 1
ip addr add "$IP0/24" dev "$IF0"
ip addr add "$IP1/24" dev "$IF1"
ip link set "$IF0" up
ip link set "$IF1" up
sysctl -w net.ipv4.conf.all.rp_filter=0 >/dev/null 2>&1 || true
sysctl -w net.ipv4.conf."$IF0".rp_filter=0 >/dev/null 2>&1 || true
sysctl -w net.ipv4.conf."$IF1".rp_filter=0 >/dev/null 2>&1 || true

LOG="$ROOT/build/test_latency.log"
log "启动 test_latency (AF_XDP) ..."
SX_LOG="$LOG" "$BIN" xdp "$IF0" "$IF1" "$IP0" "$IP1" "$PORT" "$ROUNDS" 200 "$HOLD_MS" >/dev/null 2>&1 &
PID=$!

sleep 2

echo "================ BPFTOOL 证据 ================"
echo "--- XDP 程序挂载情况（bpftool net show）---"
bpftool net show dev "$IF0" 2>&1 || true
bpftool net show dev "$IF1" 2>&1 || true

echo "--- XDP 程序详情（bpftool prog show name sx_xdp_redirect_prog）---"
bpftool prog show name sx_xdp_redirect_prog 2>&1 || true

echo "--- 相关 BPF map ---"
bpftool map show 2>&1 | grep -E "sx_stats_map|xsks_map|sx_config" || true

echo "--- sx_stats_map 计数（[0]=重定向到xsk，[1]=PASS；per-CPU 已求和）---"
for id in $(bpftool map show 2>/dev/null | awk '/sx_stats_map/ {gsub(":","",$1); print $1}'); do
    echo "map id=$id:"
    bpftool map dump id "$id" 2>/dev/null | awk '
        /"key":/   { if (k != "") printf "  key=%s  total=%d\n", k, s;
                     k=$2; gsub(",", "", k); s=0 }
        /"value":/ { v=$2; gsub(",", "", v); s += v }
        END        { if (k != "") printf "  key=%s  total=%d\n", k, s }'
done

echo "--- sx_config（本端 UDP 端口）---"
for id in $(bpftool map show 2>/dev/null | awk '/sx_config/ {gsub(":","",$1); print $1}'); do
    echo "map id=$id:"
    bpftool map dump id "$id" 2>&1 || true
done

echo "--- xsks_map（已注册的 xsk 队列）---"
for id in $(bpftool map show 2>/dev/null | awk '/xsks_map/ {gsub(":","",$1); print $1}'); do
    echo "map id=$id:"
    bpftool map dump id "$id" 2>&1 || true
done
echo "=============================================="

log "等待测试完成 ..."
rc=0
wait "$PID" || rc=$?
PID=""
log "退出码=$rc"
echo "----- test_latency.log -----"
cat "$LOG" 2>/dev/null || true
exit $rc
