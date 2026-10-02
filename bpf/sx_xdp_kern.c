// SPDX-License-Identifier: GPL-2.0
/*
 * sx_xdp_kern.c - simple-xudp 的 XDP 重定向程序
 *
 * 行为：
 *   - 将目的端口等于配置端口的 IPv4/UDP 报文重定向到对应队列的 AF_XDP socket；
 *   - 将 ARP 报文也重定向到用户态（由 sx_arp 处理请求/应答，实现自包含的二层解析）；
 *   - 其余报文 XDP_PASS 交回内核协议栈。
 *
 * 配置：
 *   sx_config 数组 map（key=0）保存本端 UDP 端口，用户态加载后写入。
 *   xsks_map 为 XSKMAP，key=rx_queue_index，用户态将 xsk 注册到对应队列。
 *
 * 注意：ARP 被重定向后，本接口的 ARP 由用户态库应答；库退出后需卸载 XDP。
 */
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/in.h>

#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#define SX_IP_OFFMASK 0x1fff

struct sx_xdp_config {
    __u16 port;
    __u16 flags;
};

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct sx_xdp_config);
} sx_config SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(max_entries, 64);
    __type(key, __u32);
    __type(value, __u32);
} xsks_map SEC(".maps");

/* 处理计数（per-CPU）：[0]=重定向到 xsk，[1]=PASS 回内核。可用 bpftool
 * `map dump` 观察，作为“流量确实经过 XDP 处理”的证据。 */
#define SX_STAT_REDIRECT 0
#define SX_STAT_PASS 1

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 2);
    __type(key, __u32);
    __type(value, __u64);
} sx_stats_map SEC(".maps");

static __always_inline void sx_stat_inc(__u32 key) {
    __u64 *v = bpf_map_lookup_elem(&sx_stats_map, &key);
    if (v) {
        /* per-CPU map：本 CPU 独占该 value，无需原子操作。 */
        (*v)++;
    }
}

/* 判断该帧是否应重定向到用户态。 */
static __always_inline int sx_should_redirect(struct xdp_md *ctx, void *data,
                                              void *data_end) {
    struct ethhdr *eth = data;
    struct iphdr *ip;
    struct udphdr *udp;
    struct sx_xdp_config *cfg;
    __u32 key = 0;

    if ((void *)(eth + 1) > data_end) {
        return 0;
    }
    /* ARP 交用户态处理。 */
    if (eth->h_proto == bpf_htons(ETH_P_ARP)) {
        return 1;
    }
    if (eth->h_proto != bpf_htons(ETH_P_IP)) {
        return 0;
    }
    ip = (struct iphdr *)(eth + 1);
    if ((void *)(ip + 1) > data_end) {
        return 0;
    }
    if (ip->protocol != IPPROTO_UDP) {
        return 0;
    }
    /* 分片报文不处理（本库不分片）。 */
    if (ip->frag_off & bpf_htons(SX_IP_OFFMASK)) {
        return 0;
    }
    udp = (struct udphdr *)(ip + 1);
    if ((void *)(udp + 1) > data_end) {
        return 0;
    }
    cfg = bpf_map_lookup_elem(&sx_config, &key);
    if (!cfg) {
        return 0;
    }
    if (udp->dest != bpf_htons(cfg->port)) {
        return 0;
    }
    return 1;
}

SEC("xdp")
int sx_xdp_redirect_prog(struct xdp_md *ctx) {
    void *data = (void *)(long)ctx->data;
    void *data_end = (void *)(long)ctx->data_end;
    __u32 idx = ctx->rx_queue_index;

    if (!sx_should_redirect(ctx, data, data_end)) {
        sx_stat_inc(SX_STAT_PASS);
        return XDP_PASS;
    }
    if (bpf_map_lookup_elem(&xsks_map, &idx)) {
        sx_stat_inc(SX_STAT_REDIRECT);
        return bpf_redirect_map(&xsks_map, idx, 0);
    }
    sx_stat_inc(SX_STAT_PASS);
    return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
