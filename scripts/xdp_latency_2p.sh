#!/usr/bin/env bash
# xdp_latency_2p.sh - 双进程 AF_XDP 时延测试（server / client 两个独立进程）
#
# 用法：sudo scripts/xdp_latency_2p.sh [build/test_latency]
#
# 拓扑：veth sx0<->sx1；server 进程独占 sx1，client 进程独占 sx0。
# 期间用 bpftool 展示两个进程各自的 XDP 程序与 sx_stats_map 计数。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$ROOT/build/test_latency}"
IF0=sx0
IF1=sx1
IP0=10.10.0.1
IP1=10.10.0.2
SPORT=31000
CPORT=31001
ROUNDS=20000
WARMUP=200
CHOLD=4000
SHOLD=15000

# 可选绑核：把 server/client 分别绑定到不同 CPU（建议两个不同的性能核，避开中断核）。
# 例：SX_CPU_SERVER=2 SX_CPU_CLIENT=3 sudo scripts/xdp_latency_2p.sh
TASKSET_S=""
TASKSET_C=""
if [ -n "${SX_CPU_SERVER:-}" ]; then
    TASKSET_S="taskset -c ${SX_CPU_SERVER}"
fi
if [ -n "${SX_CPU_CLIENT:-}" ]; then
    TASKSET_C="taskset -c ${SX_CPU_CLIENT}"
fi

log() { echo "[xdp_latency_2p] $*"; }

cleanup() {
    [ -n "${CPID:-}" ] && kill "$CPID" 2>/dev/null || true
    [ -n "${SPID:-}" ] && kill "$SPID" 2>/dev/null || true
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

SLOG="$ROOT/build/test_latency_server.log"
CLOG="$ROOT/build/test_latency_client.log"

log "启动 server 进程 (xdp-server $IF1 $IP1 $SPORT) ${TASKSET_S:+[$TASKSET_S]} ..."
SX_LOG="$SLOG" $TASKSET_S "$BIN" xdp-server "$IF1" "$IP1" "$SPORT" "$SHOLD" \
    >/dev/null 2>&1 &
SPID=$!
sleep 1

log "启动 client 进程 (xdp-client $IF0 $IP0 $CPORT -> $IP1:$SPORT) ${TASKSET_C:+[$TASKSET_C]} ..."
SX_LOG="$CLOG" $TASKSET_C "$BIN" xdp-client "$IF0" "$IP0" "$CPORT" "$IP1" \
    "$SPORT" "$ROUNDS" "$WARMUP" "$CHOLD" >/dev/null 2>&1 &
CPID=$!

sleep 1

echo "================ 进程 ================"
ps -o pid,comm,args -p "$SPID" -p "$CPID" 2>/dev/null || true

echo "================ BPFTOOL 证据 ================"
echo "--- XDP 程序挂载（bpftool net show）---"
bpftool net show dev "$IF0" 2>&1 || true
bpftool net show dev "$IF1" 2>&1 || true

echo "--- XDP 程序详情 ---"
bpftool prog show name sx_xdp_redirect_prog 2>&1 || true

echo "--- 相关 BPF map ---"
bpftool map show 2>&1 | grep -E "sx_stats_map|xsks_map|sx_config" || true

echo "--- sx_stats_map 计数（[0]=重定向到xsk，[1]=PASS；per-CPU 求和）---"
for id in $(bpftool map show 2>/dev/null | awk '/sx_stats_map/ {gsub(":","",$1); print $1}'); do
    echo "map id=$id:"
    bpftool map dump id "$id" 2>/dev/null | awk '
        /"key":/   { if (k != "") printf "  key=%s  total=%d\n", k, s;
                     k=$2; gsub(",", "", k); s=0 }
        /"value":/ { v=$2; gsub(",", "", v); s += v }
        END        { if (k != "") printf "  key=%s  total=%d\n", k, s }'
done
echo "=============================================="

log "等待 client 完成 ..."
CRC=0
wait "$CPID" || CRC=$?
CPID=""
log "client 退出码=$CRC"
echo "----- client.log -----"
cat "$CLOG" 2>/dev/null || true

kill "$SPID" 2>/dev/null || true
wait "$SPID" 2>/dev/null || true
SPID=""
echo "----- server.log -----"
cat "$SLOG" 2>/dev/null || true
exit $CRC
