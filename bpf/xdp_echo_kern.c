// SPDX-License-Identifier: GPL-2.0
/*
 * xdp_echo_kern.c - 纯 XDP_TX 反射程序（微基准用，不经过用户态）
 *
 * 行为：
 *   - 匹配 IPv4/UDP 且目的端口等于 echo_config.port 的报文；
 *   - 交换以太源/目的 MAC、IPv4 源/目的地址、UDP 源/目的端口；
 *   - 重算 IPv4 头校验和，UDP 校验和置 0（IPv4 下合法）；
 *   - 直接 XDP_TX 原路返回。
 *
 * 用途：测量网卡/驱动/XDP_TX 的物理往返下限，剥离 KCP 与用户态协议栈开销。
 * 注意：仅用于测试，不参与生产数据面。
 */
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/in.h>

#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#define SX_IP_OFFMASK 0x1fff

struct echo_config {
    __u16 port;
    __u16 pad;
};

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct echo_config);
} echo_config SEC(".maps");

SEC("xdp")
int xdp_echo_prog(struct xdp_md *ctx) {
    void *data = (void *)(long)ctx->data;
    void *data_end = (void *)(long)ctx->data_end;
    struct ethhdr *eth = data;
    struct iphdr *ip;
    struct udphdr *udp;
    struct echo_config *cfg;
    __u32 key = 0;
    __u32 sum;
    __u32 ip_tmp;
    __u16 port_tmp;
    unsigned char mac_tmp;

    if ((void *)(eth + 1) > data_end) {
        return XDP_PASS;
    }
    if (eth->h_proto != bpf_htons(ETH_P_IP)) {
        return XDP_PASS;
    }
    ip = (struct iphdr *)(eth + 1);
    if ((void *)(ip + 1) > data_end) {
        return XDP_PASS;
    }
    if (ip->version != 4 || ip->ihl != 5 || ip->protocol != IPPROTO_UDP) {
        return XDP_PASS;
    }
    if (ip->frag_off & bpf_htons(SX_IP_OFFMASK)) {
        return XDP_PASS;
    }
    udp = (struct udphdr *)(ip + 1);
    if ((void *)(udp + 1) > data_end) {
        return XDP_PASS;
    }
    cfg = bpf_map_lookup_elem(&echo_config, &key);
    if (!cfg || udp->dest != bpf_htons(cfg->port)) {
        return XDP_PASS;
    }

    /* 交换 MAC（逐字节展开，避免可变边界循环在旧 verifier 上不可用）。 */
    mac_tmp = eth->h_dest[0]; eth->h_dest[0] = eth->h_source[0]; eth->h_source[0] = mac_tmp;
    mac_tmp = eth->h_dest[1]; eth->h_dest[1] = eth->h_source[1]; eth->h_source[1] = mac_tmp;
    mac_tmp = eth->h_dest[2]; eth->h_dest[2] = eth->h_source[2]; eth->h_source[2] = mac_tmp;
    mac_tmp = eth->h_dest[3]; eth->h_dest[3] = eth->h_source[3]; eth->h_source[3] = mac_tmp;
    mac_tmp = eth->h_dest[4]; eth->h_dest[4] = eth->h_source[4]; eth->h_source[4] = mac_tmp;
    mac_tmp = eth->h_dest[5]; eth->h_dest[5] = eth->h_source[5]; eth->h_source[5] = mac_tmp;

    ip_tmp = ip->saddr;
    ip->saddr = ip->daddr;
    ip->daddr = ip_tmp;
    port_tmp = udp->source;
    udp->source = udp->dest;
    udp->dest = port_tmp;

    /* IPv4 头校验和重算；UDP 校验和置 0（IPv4 允许）。 */
    ip->check = 0;
    sum = bpf_csum_diff(0, 0, (__be32 *)ip, sizeof(*ip), 0);
    sum = (sum & 0xffff) + (sum >> 16);
    sum = (sum & 0xffff) + (sum >> 16);
    ip->check = (__u16)~sum;
    udp->check = 0;

    return XDP_TX;
}

char _license[] SEC("license") = "GPL";
