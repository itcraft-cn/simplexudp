/*
 * sx_frame.c - Ethernet + IPv4 + UDP 帧构造/解析与校验和
 *
 * 仅 XDP(AF_XDP) 后端使用：内核 UDP 后端由内核完成封装/解封。
 *
 * 设计约束：
 *   - 单帧不分片，整帧长度必须 <= 以太 MTU（默认 1500）。
 *   - UDP 校验和包含伪首部，接收端逐帧校验，防止脏数据进入可靠层。
 *
 * 注意事项：
 *   - 传入/返回的 IP、端口均为主机字节序；线格式为大端。
 *   - buf 必须有足够空间容纳 14+20+8+len。
 */
#include "sx_internal.h"

#include <string.h>

#define ETH_HLEN 14
#define IPV4_HLEN 20
#define UDP_HLEN 8
#define ETH_P_IP 0x0800
#define IPPROTO_UDP 17
#define IPV4_VERSION_IHL 0x45

uint16_t sx_inet_csum(const void *data, size_t len, uint32_t init) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = init;

    while (len > 1) {
        sum += ((uint32_t)p[0] << 8) | (uint32_t)p[1];
        p += 2;
        len -= 2;
    }
    if (len == 1) {
        sum += (uint32_t)p[0] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (uint16_t)sum;
}

uint16_t sx_ipv4_csum(const void *ip_hdr, size_t len) {
    return (uint16_t)(~sx_inet_csum(ip_hdr, len, 0) & 0xffffu);
}

/* UDP 伪首部（src/dst/zero/proto/len）的折叠和。 */
static uint16_t sx_udp_pseudo_sum(uint32_t src_ip, uint32_t dst_ip, size_t len) {
    uint8_t pseudo[12];

    pseudo[0] = (uint8_t)(src_ip >> 24);
    pseudo[1] = (uint8_t)(src_ip >> 16);
    pseudo[2] = (uint8_t)(src_ip >> 8);
    pseudo[3] = (uint8_t)(src_ip);
    pseudo[4] = (uint8_t)(dst_ip >> 24);
    pseudo[5] = (uint8_t)(dst_ip >> 16);
    pseudo[6] = (uint8_t)(dst_ip >> 8);
    pseudo[7] = (uint8_t)(dst_ip);
    pseudo[8] = 0;
    pseudo[9] = IPPROTO_UDP;
    pseudo[10] = (uint8_t)(len >> 8);
    pseudo[11] = (uint8_t)(len & 0xff);
    return sx_inet_csum(pseudo, sizeof(pseudo), 0);
}

uint16_t sx_udp_csum(uint32_t src_ip, uint32_t dst_ip,
                     const void *udp_seg, size_t len) {
    uint32_t sum;

    sum = sx_udp_pseudo_sum(src_ip, dst_ip, len);
    sum = sx_inet_csum(udp_seg, len, sum);
    sum = ~sum & 0xffffu;
    /* UDP 校验和为 0 表示未使用，发送端须置 0xffff。 */
    if (sum == 0) {
        sum = 0xffffu;
    }
    return (uint16_t)sum;
}

static inline void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}

static inline void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

static inline uint16_t get16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

ssize_t sx_frame_build(uint8_t *buf, size_t cap,
                       const uint8_t src_mac[6], const uint8_t dst_mac[6],
                       uint32_t src_ip, uint32_t dst_ip,
                       uint16_t src_port, uint16_t dst_port,
                       const void *payload, size_t len) {
    uint8_t *eth;
    uint8_t *ip;
    uint8_t *udp;
    size_t udp_len;
    size_t frame_len;

    if (buf == NULL || src_mac == NULL || dst_mac == NULL) {
        return SX_ERR_INVALID;
    }
    if (payload == NULL && len > 0) {
        return SX_ERR_INVALID;
    }
    udp_len = UDP_HLEN + len;
    frame_len = ETH_HLEN + IPV4_HLEN + udp_len;
    if (frame_len > cap) {
        return SX_ERR_TOO_LARGE;
    }
    if (udp_len > 0xffffu) {
        return SX_ERR_TOO_LARGE;
    }

    eth = buf;
    ip = buf + ETH_HLEN;
    udp = ip + IPV4_HLEN;

    /* Ethernet。 */
    memcpy(eth + 0, dst_mac, 6);
    memcpy(eth + 6, src_mac, 6);
    put16(eth + 12, ETH_P_IP);

    /* IPv4。 */
    ip[0] = IPV4_VERSION_IHL;
    ip[1] = 0;
    put16(ip + 2, (uint16_t)(IPV4_HLEN + udp_len));
    put16(ip + 4, 0); /* id */
    put16(ip + 6, 0); /* flags/frag: 不分片 */
    ip[8] = 64;       /* ttl */
    ip[9] = IPPROTO_UDP;
    put16(ip + 10, 0);
    put32(ip + 12, src_ip);
    put32(ip + 16, dst_ip);
    put16(ip + 10, sx_ipv4_csum(ip, IPV4_HLEN));

    /* UDP。 */
    put16(udp + 0, src_port);
    put16(udp + 2, dst_port);
    put16(udp + 4, (uint16_t)udp_len);
    put16(udp + 6, 0);
    if (len > 0) {
        memcpy(udp + UDP_HLEN, payload, len);
    }
    put16(udp + 6, sx_udp_csum(src_ip, dst_ip, udp, udp_len));

    return (ssize_t)frame_len;
}

int sx_frame_parse(const uint8_t *frame, size_t frame_len,
                   uint32_t *src_ip, uint32_t *dst_ip,
                   uint16_t *src_port, uint16_t *dst_port,
                   const uint8_t **payload, size_t *len) {
    const uint8_t *eth;
    const uint8_t *ip;
    const uint8_t *udp;
    size_t ihl;
    size_t ip_total;
    size_t udp_len;

    if (frame == NULL || frame_len < ETH_HLEN + IPV4_HLEN + UDP_HLEN) {
        return SX_ERR_INVALID;
    }
    eth = frame;
    if (get16(eth + 12) != ETH_P_IP) {
        return SX_ERR_UNSUPPORTED;
    }
    ip = frame + ETH_HLEN;
    if ((ip[0] >> 4) != 4) {
        return SX_ERR_UNSUPPORTED;
    }
    ihl = (size_t)(ip[0] & 0x0f) * 4u;
    if (ihl < IPV4_HLEN || frame_len < ETH_HLEN + ihl + UDP_HLEN) {
        return SX_ERR_INVALID;
    }
    if (ip[9] != IPPROTO_UDP) {
        return SX_ERR_UNSUPPORTED;
    }
    /* IP 头部校验和验证（含校验和字段的整段和为 0xffff）。 */
    if (sx_inet_csum(ip, ihl, 0) != 0xffffu) {
        return SX_ERR_INVALID;
    }
    ip_total = get16(ip + 2);
    if (ip_total < ihl + UDP_HLEN || ETH_HLEN + ip_total > frame_len) {
        return SX_ERR_INVALID;
    }
    udp = ip + ihl;
    udp_len = get16(udp + 4);
    if (udp_len < UDP_HLEN || udp_len > ip_total - ihl) {
        return SX_ERR_INVALID;
    }
    /* UDP 校验和验证（0 表示对端未计算，放行）。
     * 含已填校验和字段的整段折叠和应为 0xffff。 */
    {
        uint16_t uc = get16(udp + 6);
        if (uc != 0) {
            uint32_t sip = get32(ip + 12);
            uint32_t dip = get32(ip + 16);
            uint32_t sum = sx_udp_pseudo_sum(sip, dip, udp_len);
            sum = sx_inet_csum(udp, udp_len, sum);
            if (sum != 0xffffu) {
                return SX_ERR_INVALID;
            }
        }
    }
    if (src_ip) *src_ip = get32(ip + 12);
    if (dst_ip) *dst_ip = get32(ip + 16);
    if (src_port) *src_port = get16(udp + 0);
    if (dst_port) *dst_port = get16(udp + 2);
    if (payload) *payload = udp + UDP_HLEN;
    if (len) *len = udp_len - UDP_HLEN;
    return SX_OK;
}
