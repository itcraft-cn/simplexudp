#!/usr/bin/env bash
# veth_test.sh - 基于 veth 的 AF_XDP 集成测试
#
# 用法：sudo scripts/veth_test.sh [build/test_xdp]
#
# 步骤：
#   1. 创建 veth 对 sx0<->sx1，配置 IP 并 UP；
#   2. 运行 test_xdp（AF_XDP 后端，generic/native 自动侦测）；
#   3. 清理 veth 与残留 XDP 程序。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$ROOT/build/test_xdp}"
IF0=sx0
IF1=sx1
IP0=10.10.0.1
IP1=10.10.0.2

log() { echo "[veth_test] $*"; }

cleanup() {
    ip link del "$IF0" 2>/dev/null || true
}
trap cleanup EXIT

if [ "$(id -u)" != "0" ]; then
    log "需要 root 运行"
    exit 2
fi

if [ ! -x "$BIN" ]; then
    log "未找到测试程序: $BIN（先执行 make tests-xdp）"
    exit 2
fi

cleanup
log "创建 veth: $IF0 <-> $IF1"
ip link add "$IF0" type veth peer name "$IF1" || exit 1
ip addr add "$IP0/24" dev "$IF0"
ip addr add "$IP1/24" dev "$IF1"
ip link set "$IF0" up
ip link set "$IF1" up

# 关闭反向路径过滤与 IPv6（避免干扰）。
sysctl -w net.ipv4.conf.all.rp_filter=0 >/dev/null 2>&1 || true
sysctl -w net.ipv4.conf."$IF0".rp_filter=0 >/dev/null 2>&1 || true
sysctl -w net.ipv4.conf."$IF1".rp_filter=0 >/dev/null 2>&1 || true

log "运行 test_xdp ..."
rc=0
SX_LOG="$ROOT/build/test_xdp.log" "$BIN" "$IF0" "$IF1" "$IP0" "$IP1" 30000 \
    || rc=$?
log "退出码=$rc"
echo "----- test_xdp.log -----"
cat "$ROOT/build/test_xdp.log" 2>/dev/null || true
exit $rc
