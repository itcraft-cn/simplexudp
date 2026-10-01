/*
 * sx_arp.c - 极简 ARP 缓存（仅 AF_XDP 后端需要）
 *
 * 功能：
 *   - 维护 ip -> mac 缓存，未命中时主动发送 ARP 请求；
 *   - 响应本机 IP 的 ARP 请求，学习请求方映射；
 *   - 支持注册静态映射（覆盖自动解析）。
 *
 * 注意：
 *   - AF_XDP 数据面在同一二层网段内工作，跨网段需上层配置网关或静态映射。
 *   - 组播/广播不经过 ARP，由调用方直接给出目的 MAC。
 */
#include "sx_internal.h"

#include <stdlib.h>
#include <string.h>

#define SX_ARP_MAX 64
#define SX_ARP_RETRY_MS 1000u
#define SX_ARP_MAX_TRIES 3u
#define SX_ARP_AGE_MS 300000u /* 5 分钟老化 */

#define ARP_ETH_HLEN 14
#define ARP_LEN 28
#define ARP_HTYPE_ETH 1
#define ARP_PTYPE_IP 0x0800
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY 2
#define ETH_P_ARP 0x0806

typedef struct sx_arp_entry {
    uint32_t ip;
    uint8_t mac[6];
    int valid;
    int probing;
    uint32_t tries;
    uint64_t ts;
} sx_arp_entry_t;

struct sx_arp {
    sx_io_t *io;
    uint32_t local_ip;
    uint8_t local_mac[6];
    sx_arp_entry_t entries[SX_ARP_MAX];
};

static sx_arp_entry_t *arp_lookup(sx_arp_t *arp, uint32_t ip) {
    int i;
    for (i = 0; i < SX_ARP_MAX; i++) {
        if (arp->entries[i].ip == ip && (arp->entries[i].valid || arp->entries[i].probing)) {
            return &arp->entries[i];
        }
    }
    return NULL;
}

sx_arp_t *sx_arp_create(sx_io_t *io) {
    sx_arp_t *arp = (sx_arp_t *)calloc(1, sizeof(*arp));
    if (arp == NULL) {
        return NULL;
    }
    arp->io = io;
    return arp;
}

void sx_arp_destroy(sx_arp_t *arp) {
    free(arp);
}

void sx_arp_set_local(sx_arp_t *arp, uint32_t ip, const uint8_t mac[6]) {
    if (arp == NULL) {
        return;
    }
    arp->local_ip = ip;
    if (mac != NULL) {
        memcpy(arp->local_mac, mac, 6);
    }
}

void sx_arp_set(sx_arp_t *arp, uint32_t ip, const uint8_t mac[6]) {
    sx_arp_entry_t *e;
    int i;
    if (arp == NULL || mac == NULL) {
        return;
    }
    e = arp_lookup(arp, ip);
    if (e == NULL) {
        for (i = 0; i < SX_ARP_MAX; i++) {
            if (!arp->entries[i].valid && !arp->entries[i].probing) {
                e = &arp->entries[i];
                break;
            }
        }
    }
    if (e == NULL) {
        return;
    }
    e->ip = ip;
    memcpy(e->mac, mac, 6);
    e->valid = 1;
    e->probing = 0;
    e->tries = 0;
    e->ts = sx_now_ms();
}

static void arp_send_request(sx_arp_t *arp, uint32_t target_ip) {
    uint8_t frame[ARP_ETH_HLEN + ARP_LEN];
    uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    uint8_t *eth = frame;
    uint8_t *a = frame + ARP_ETH_HLEN;

    memcpy(eth, bcast, 6);
    memcpy(eth + 6, arp->local_mac, 6);
    eth[12] = (uint8_t)(ETH_P_ARP >> 8);
    eth[13] = (uint8_t)(ETH_P_ARP & 0xff);

    a[0] = 0;
    a[1] = 1; /* htype */
    a[2] = 0x08;
    a[3] = 0x00; /* ptype */
    a[4] = 6;
    a[5] = 4;
    a[6] = 0;
    a[7] = ARP_OP_REQUEST;
    memcpy(a + 8, arp->local_mac, 6);
    a[14] = (uint8_t)(arp->local_ip >> 24);
    a[15] = (uint8_t)(arp->local_ip >> 16);
    a[16] = (uint8_t)(arp->local_ip >> 8);
    a[17] = (uint8_t)(arp->local_ip);
    memset(a + 18, 0, 6);
    a[24] = (uint8_t)(target_ip >> 24);
    a[25] = (uint8_t)(target_ip >> 16);
    a[26] = (uint8_t)(target_ip >> 8);
    a[27] = (uint8_t)(target_ip);

    (void)sx_io_xdp_send_frame(arp->io, frame, sizeof(frame));
}

static void arp_send_reply(sx_arp_t *arp, const uint8_t dst_mac[6], uint32_t dst_ip) {
    uint8_t frame[ARP_ETH_HLEN + ARP_LEN];
    uint8_t *eth = frame;
    uint8_t *a = frame + ARP_ETH_HLEN;

    memcpy(eth, dst_mac, 6);
    memcpy(eth + 6, arp->local_mac, 6);
    eth[12] = (uint8_t)(ETH_P_ARP >> 8);
    eth[13] = (uint8_t)(ETH_P_ARP & 0xff);

    a[0] = 0;
    a[1] = 1;
    a[2] = 0x08;
    a[3] = 0x00;
    a[4] = 6;
    a[5] = 4;
    a[6] = 0;
    a[7] = ARP_OP_REPLY;
    memcpy(a + 8, arp->local_mac, 6);
    a[14] = (uint8_t)(arp->local_ip >> 24);
    a[15] = (uint8_t)(arp->local_ip >> 16);
    a[16] = (uint8_t)(arp->local_ip >> 8);
    a[17] = (uint8_t)(arp->local_ip);
    memcpy(a + 18, dst_mac, 6);
    a[24] = (uint8_t)(dst_ip >> 24);
    a[25] = (uint8_t)(dst_ip >> 16);
    a[26] = (uint8_t)(dst_ip >> 8);
    a[27] = (uint8_t)(dst_ip);

    (void)sx_io_xdp_send_frame(arp->io, frame, sizeof(frame));
}

static void arp_learn(sx_arp_t *arp, uint32_t ip, const uint8_t mac[6]) {
    sx_arp_entry_t *e = arp_lookup(arp, ip);
    int i;
    if (e == NULL) {
        for (i = 0; i < SX_ARP_MAX; i++) {
            if (!arp->entries[i].valid && !arp->entries[i].probing) {
                e = &arp->entries[i];
                break;
            }
        }
    }
    if (e == NULL) {
        return;
    }
    e->ip = ip;
    memcpy(e->mac, mac, 6);
    e->valid = 1;
    e->probing = 0;
    e->tries = 0;
    e->ts = sx_now_ms();
}

int sx_arp_resolve(sx_arp_t *arp, uint32_t ip, uint8_t mac_out[6]) {
    sx_arp_entry_t *e;
    uint64_t now;

    if (arp == NULL || mac_out == NULL) {
        return SX_ERR_INVALID;
    }
    e = arp_lookup(arp, ip);
    if (e != NULL && e->valid) {
        memcpy(mac_out, e->mac, 6);
        return SX_OK;
    }
    /* 未命中：发起/继续探测。 */
    if (e == NULL) {
        int i;
        now = sx_now_ms();
        for (i = 0; i < SX_ARP_MAX; i++) {
            if (!arp->entries[i].valid && !arp->entries[i].probing) {
                e = &arp->entries[i];
                break;
            }
        }
        if (e == NULL) {
            return SX_ERR_AGAIN;
        }
        e->ip = ip;
        e->probing = 1;
        e->valid = 0;
        e->tries = 0;
        e->ts = now;
    }
    arp_send_request(arp, ip);
    e->tries++;
    e->ts = sx_now_ms();
    return SX_ERR_AGAIN;
}

int sx_arp_input(sx_arp_t *arp, const uint8_t *frame, size_t len) {
    const uint8_t *a;
    uint16_t op;
    uint32_t spa;
    uint32_t tpa;
    uint8_t sha[6];

    if (arp == NULL || frame == NULL || len < ARP_ETH_HLEN + ARP_LEN) {
        return 0;
    }
    if (frame[12] != 0x08 || frame[13] != 0x06) {
        return 0;
    }
    a = frame + ARP_ETH_HLEN;
    if (!(a[0] == 0 && a[1] == 1 && a[2] == 0x08 && a[3] == 0x00 &&
          a[4] == 6 && a[5] == 4)) {
        return 1;
    }
    op = (uint16_t)((a[6] << 8) | a[7]);
    memcpy(sha, a + 8, 6);
    spa = ((uint32_t)a[14] << 24) | ((uint32_t)a[15] << 16) |
          ((uint32_t)a[16] << 8) | a[17];
    tpa = ((uint32_t)a[24] << 24) | ((uint32_t)a[25] << 16) |
          ((uint32_t)a[26] << 8) | a[27];

    arp_learn(arp, spa, sha);

    if (op == ARP_OP_REQUEST && (arp->local_ip == 0 || tpa == arp->local_ip)) {
        if (arp->local_ip != 0 || tpa == arp->local_ip) {
            arp_send_reply(arp, sha, spa);
        }
    }
    return 1;
}

void sx_arp_update(sx_arp_t *arp, uint64_t now_ms) {
    int i;
    if (arp == NULL) {
        return;
    }
    for (i = 0; i < SX_ARP_MAX; i++) {
        sx_arp_entry_t *e = &arp->entries[i];
        if (!e->valid && e->probing) {
            if (now_ms - e->ts >= SX_ARP_RETRY_MS) {
                if (e->tries >= SX_ARP_MAX_TRIES) {
                    e->probing = 0; /* 放弃，等待下次解析重新探测 */
                    memset(e->mac, 0, 6);
                } else {
                    arp_send_request(arp, e->ip);
                    e->tries++;
                    e->ts = now_ms;
                }
            }
        } else if (e->valid && now_ms - e->ts >= SX_ARP_AGE_MS) {
            e->valid = 0;
        }
    }
}
