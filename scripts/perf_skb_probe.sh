#!/usr/bin/env bash
# perf_skb_probe.sh - 用 bpftrace 统计 skb 生命周期事件，验证“XDP 早丢包是否真的省了 skb 分配”
#
# 背景：XDP 与 TC 的 CPU 差异不取决于挂点早晚，而取决于 XDP 程序运行时
#       sk_buff 是否已经建好。veth 上接收侧先用 skb 转 xdp_buff，分配已经付过；
#       virtio-net / 物理网卡 native XDP 才可能在 skb 分配前返回 XDP_DROP。
#       本脚本计数 __alloc_skb / napi_alloc_skb / napi_build_skb / build_skb /
#       kfree_skbmem / __netif_receive_skb_core / ip_rcv，用可移植的“有没有建 skb”
#       代替依赖硬件 PMU、且禁止跨机比较的 cycles。TC ingress 在
#       __netif_receive_skb_core 内部执行，故用 ip_rcv（L3 入口）判断是否向上交付。
#
# 用法：
#   sudo scripts/perf_skb_probe.sh -d 10
#   sudo scripts/perf_skb_probe.sh -n 1000000 -d 20 -o build/skb.txt
#   sudo scripts/perf_skb_probe.sh --perf -n 1000000 -- ./build/test_throughput ...
#
# 选项：
#   -d SEC     采集时长，默认 10 秒（给了 -- CMD 时忽略，改为随命令生命周期）
#   -n PKTS    本次采集覆盖的报文数，用于计算每包均值（可选）
#   -o FILE    摘要写入 FILE（默认 stdout）
#   --perf     额外用 perf stat 采集 cycles/instructions（需要可用 PMU）
#   -h         显示帮助
#
# 注意：
#   - 计数是全局的，未按网卡/进程过滤；测试期间请保持环境安静（单流、无其他流量）。
#   - 高 pps 下 bpftrace 可能丢事件（计数偏小）；发包限速到几万 pps，或观察 stderr 的 Lost 提示。
#   - softirq/N-API 收包不在进程上下文，按 PID 过滤会漏计，因此本脚本不做进程过滤。
#   - __netif_receive_skb_core 在部分内核上被 constprop 改名，脚本会自动从 kallsyms 解析。
#   - 只做“同一次运行内”的相对比较；不要拿本机绝对数字与其它机器对比。
set -euo pipefail

DUR=10
PKTS=""
OUT=""
USE_PERF=0
CMD=()

log() { echo "[perf_skb_probe] $*" >&2; }
die() { log "$*"; exit 2; }

usage() { awk 'NR > 1 && /^set -euo pipefail/ { exit } NR > 1 { print }' "$0"; }

while [ $# -gt 0 ]; do
    case "$1" in
        -d) [ $# -ge 2 ] || die "-d 需要参数"; DUR="$2"; shift 2 ;;
        -n) [ $# -ge 2 ] || die "-n 需要参数"; PKTS="$2"; shift 2 ;;
        -o) [ $# -ge 2 ] || die "-o 需要参数"; OUT="$2"; shift 2 ;;
        --perf) USE_PERF=1; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; CMD=("$@"); break ;;
        *) die "未知参数：$1（用 -h 查看用法）" ;;
    esac
done

[ "$(id -u)" = "0" ] || die "需要 root（加载 kprobe）"
command -v bpftrace >/dev/null 2>&1 || die "未找到 bpftrace（apt install bpftrace）"
[ -r /proc/kallsyms ] || die "无法读取 /proc/kallsyms，无法解析内核符号"

case "$DUR" in *[!0-9]*|"") die "-d 必须是正整数" ;; esac
if [ -n "$PKTS" ]; then
    case "$PKTS" in *[!0-9]*|"") die "-n 必须是正整数" ;; esac
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/sx_skb_probe.XXXXXX")"
BT="$WORK/probe.bt"
RAW="$WORK/raw.txt"
ERR="$WORK/bpftrace.err"
PERFOUT="$WORK/perf.txt"
trap 'rm -rf "$WORK"' EXIT

: >"$BT"
MAPS=()

# 在 kallsyms 中查找 base 或 base.constprop.N；跳过 CFI 的 __pfx_ 前缀符号。
ksym_lookup() {
    awk -v b="$1" '
        $3 == b { exact = $3; next }
        index($3, b ".") == 1 && found == "" { found = $3 }
        END { if (exact != "") print exact; else if (found != "") print found }
    ' /proc/kallsyms
}

emit_probe() { # $1=symbol  $2=map name
    local sym="$1" name="$2"
    [ -n "$sym" ] || return 0
    printf 'kprobe:%s { @%s = count(); }\n' "$sym" "$name" >>"$BT"
    MAPS+=("$name")
    log "计数 $sym -> @$name"
}

emit_probe "$(ksym_lookup __alloc_skb)"        alloc_skb
emit_probe "$(ksym_lookup napi_alloc_skb)"     napi_alloc_skb
emit_probe "$(ksym_lookup napi_build_skb)"     napi_build_skb
emit_probe "$(ksym_lookup build_skb)"          build_skb
emit_probe "$(ksym_lookup kfree_skbmem)"       kfree_skbmem

STACK_SYM="$(ksym_lookup __netif_receive_skb_core)"
[ -n "$STACK_SYM" ] || STACK_SYM="$(ksym_lookup netif_receive_skb_core)"
[ -n "$STACK_SYM" ] || STACK_SYM="$(ksym_lookup __netif_receive_skb)"
emit_probe "$STACK_SYM" stack

# TC ingress 就在 __netif_receive_skb_core 内部执行，因此“进入 core”不能区分
# TC-DROP 与通过协议栈；用 L3 入口 ip_rcv 作为“是否继续向上交付”的判据。
IP_SYM="$(ksym_lookup ip_rcv)"
[ -n "$IP_SYM" ] || IP_SYM="$(ksym_lookup ip_rcv_finish)"
[ -n "$IP_SYM" ] || IP_SYM="$(ksym_lookup ip_rcv_core)"
emit_probe "$IP_SYM" ip_rcv

[ "${#MAPS[@]}" -gt 0 ] || die "没有找到任何可挂载的内核符号"

{
    echo 'END {'
    for m in "${MAPS[@]}"; do
        printf '  printf("%s=%%lld\\n", (int64)@%s); clear(@%s);\n' "$m" "$m" "$m"
    done
    echo '}'
} >>"$BT"

if [ "${#CMD[@]}" -eq 0 ]; then
    printf 'interval:s:%s { exit(); }\n' "$DUR" >>"$BT"
fi

bpftrace -q "$BT" >"$RAW" 2>"$ERR" &
BPID=$!

cleanup_bpftrace() {
    if kill -0 "$BPID" 2>/dev/null; then
        kill -INT "$BPID" 2>/dev/null || true
        wait "$BPID" 2>/dev/null || true
    fi
}
trap 'cleanup_bpftrace; rm -rf "$WORK"' EXIT

# -q 会抑制 "Attaching" 提示，因此用“进程是否存活”判断启动成功；
# kprobe 挂载失败会在启动阶段直接退出。
sleep 0.5
kill -0 "$BPID" 2>/dev/null || { cat "$ERR" >&2; die "bpftrace 启动失败（内核符号或权限问题）"; }

run_perf() { # $@=被测量的命令
    if [ "$USE_PERF" != "1" ]; then
        "$@"
        return
    fi
    if perf stat -a -e cycles,instructions true >/dev/null 2>&1; then
        perf stat -a -e cycles,instructions -o "$PERFOUT" -- "$@"
    else
        log "PMU 不可用（虚拟机/受限实例常见），跳过 perf stat，仅保留 skb 计数"
        USE_PERF=0
        "$@"
    fi
}

ELAPSED=""
if [ "${#CMD[@]}" -eq 0 ]; then
    log "采集 ${DUR}s ..."
    run_perf sleep "$DUR"
else
    log "运行命令并采集：${CMD[*]}"
    START=$(date +%s.%N 2>/dev/null || date +%s)
    run_perf "${CMD[@]}"
    END=$(date +%s.%N 2>/dev/null || date +%s)
    ELAPSED=$(awk -v a="$START" -v b="$END" 'BEGIN { printf "%.2f", b - a }')
    log "命令耗时 ${ELAPSED}s"
fi

cleanup_bpftrace

[ -s "$RAW" ] || { cat "$ERR" >&2; die "bpftrace 没有输出计数"; }

val() { # $1=key
    awk -F= -v k="$1" '$1 == k { print $2 }' "$RAW" | tail -1
}
get() { local v; v="$(val "$1")"; echo "${v:-0}"; }

A_SKB=$(get alloc_skb)
A_NAPI_ALLOC=$(get napi_alloc_skb)
A_NAPI_BUILD=$(get napi_build_skb)
A_BUILD=$(get build_skb)
A_FREE=$(get kfree_skbmem)
A_STACK=$(get stack)
A_IP=$(get ip_rcv)
BUILT=$((A_SKB + A_NAPI_ALLOC + A_NAPI_BUILD + A_BUILD))

verdict=$(
    awk -v built="$BUILT" -v ip="$A_IP" 'BEGIN {
        if (built == 0 && ip == 0)
            print "skb 未建、L3 未进：早丢包真正拿掉了分配与后续路径";
        else if (built == 0)
            print "skb 未建但 L3 有进入：检查是否有其它来源的 skb / 统计窗口偏差";
        else if (ip * 10 < built)
            print "skb 已建但多数未进 L3：只跳过了协议栈上层，分配已经付过（TC-DROP 或 skb 已建时的 XDP-DROP）";
        else
            print "skb 已建且多数进入 L3：过滤规则未命中，或流量未经过该挂载点";
    }'
)

report() {
    echo "# skb / 协议栈计数"
    echo
    if [ -n "$ELAPSED" ]; then
        echo "采集窗口：命令运行期间，约 ${ELAPSED}s。"
    else
        echo "采集窗口：固定 ${DUR}s。"
    fi
    echo
    echo "| 指标 | 计数 |"
    echo "| --- | --- |"
    echo "| \`__alloc_skb\` | $A_SKB |"
    echo "| \`napi_alloc_skb\` | $A_NAPI_ALLOC |"
    echo "| \`napi_build_skb\` | $A_NAPI_BUILD |"
    echo "| \`build_skb\` | $A_BUILD |"
    echo "| skb 构造合计 | $BUILT |"
    echo "| \`kfree_skbmem\` | $A_FREE |"
    echo "| \`$STACK_SYM\`（接收路径入口，TC ingress 在此内部执行） | $A_STACK |"
    echo "| \`$IP_SYM\`（L3 入口，TC ingress 之后） | $A_IP |"
    if [ -n "$PKTS" ] && [ "$PKTS" -gt 0 ]; then
        echo
        echo "按 $PKTS 包折算：每包 skb 构造 $(awk -v b="$BUILT" -v n="$PKTS" 'BEGIN{printf "%.3f", b/n}') 次，"
        echo "每包进入 L3 $(awk -v s="$A_IP" -v n="$PKTS" 'BEGIN{printf "%.3f", s/n}') 次。"
    fi
    echo
    echo "**判读**：$verdict"
    if [ -n "$PKTS" ] && [ "$PKTS" -gt 0 ]; then
        if awk -v b="$BUILT" -v n="$PKTS" 'BEGIN { exit !(b < n * 0.05) }'; then
            echo
            echo "**提示**：skb 构造数远低于 -n，采集窗口可能没有覆盖到目标流量（或事件被丢弃）。"
        fi
    fi
    if [ "$USE_PERF" = "1" ] && [ -s "$PERFOUT" ]; then
        echo
        echo "## perf stat（仅同一次运行内可比，禁止跨机比较）"
        echo
        echo '```'
        cat "$PERFOUT"
        echo '```'
        if [ -n "$PKTS" ] && [ "$PKTS" -gt 0 ]; then
            local cyc ins
            cyc=$(awk '/cycles/ && !/seconds/ { gsub(",", "", $1); print $1; exit }' "$PERFOUT")
            ins=$(awk '/instructions/ && !/seconds/ { gsub(",", "", $1); print $1; exit }' "$PERFOUT")
            if [ -n "$cyc" ] && [ -n "$ins" ]; then
                echo
                echo "每包：cycles $(awk -v c="$cyc" -v n="$PKTS" 'BEGIN{printf "%.0f", c/n}')，"
                echo "instructions $(awk -v i="$ins" -v n="$PKTS" 'BEGIN{printf "%.0f", i/n}')。"
            fi
        fi
    fi
}

if [ -n "$OUT" ]; then
    report >"$OUT"
    log "摘要已写入 $OUT"
else
    report
fi
