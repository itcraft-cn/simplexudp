/*
 * sx_group.c - 组播/广播传输引擎
 *
 * 语义：尽力可靠（best-effort reliable）+ 按序投递。
 *   - 发送端不做 ACK 驱动的 ARQ，而是保留历史窗口；收到 NACK 后按序号重发。
 *   - 接收端按来源维护期望序号，侦测空洞（丢包侦测）后回送 NACK。
 *   - 多来源各自独立序号空间，按 (ip, port) 分表。
 *
 * 注意：
 *   - 组播/广播无法汇聚多方 ACK，故不提供严格可靠保证，仅尽力恢复。
 *   - 序号回绕与窗口边界：历史窗口小于等于 hist_cap，超出范围不再重发。
 */
#include "sx_internal.h"

#include <stdlib.h>
#include <string.h>

#define SX_GROUP_NACK_INTERVAL_MS 20u
#define SX_GROUP_SRC_TIMEOUT_MS 120000u

typedef struct sx_grx {
    uint32_t ip;
    uint16_t port;
    int used;
    uint32_t rcv_nxt;
    uint32_t peer_max;  /* 对端高水位（下一待分配序号），用于尾部丢包侦测 */
    uint32_t count;
    sx_msg_t ring[SX_GROUP_REORDER];
    uint64_t last_recv_ms;
    uint64_t last_nack_ms;
} sx_grx_t;

struct sx_group {
    sx_endpoint_t *ep;
    sx_link_t *link;
    uint32_t conv;
    uint32_t local_id;

    /* 发送历史。 */
    uint32_t snd_nxt;
    uint32_t hist_cap;
    sx_msg_t *hist;

    /* 接收（按来源）。 */
    sx_grx_t src[SX_GROUP_MAX_SRC];

    uint64_t last_send_ms;
    int hello_sent;
};

void sx_group_send_raw(sx_group_t *g, uint8_t cmd, uint32_t seq, uint32_t una,
                       uint32_t wnd, const uint32_t *list, uint32_t list_count,
                       const void *payload, size_t payload_len,
                       uint32_t dst_ip, uint16_t dst_port) {
    uint8_t buf[SX_HDR_SIZE + SX_ACK_LIST_MAX * 4 + SX_MAX_PAYLOAD_LIMIT];
    sx_hdr_t h;
    size_t off;
    uint32_t i;

    if (g == NULL || g->ep == NULL || g->ep->io == NULL) {
        return;
    }
    if (list_count > SX_ACK_LIST_MAX) {
        list_count = SX_ACK_LIST_MAX;
    }
    if (payload_len > g->ep->cfg.max_payload) {
        return;
    }
    memset(&h, 0, sizeof(h));
    h.cmd = cmd;
    h.ver = SX_PROTO_VER;
    h.conv = g->conv;
    h.peer_id = g->local_id;
    h.seq = seq;
    h.una = una;
    h.ts = (uint32_t)(sx_now_ms() & 0xffffffffu);
    h.wnd = wnd;
    h.ack_count = (uint16_t)list_count;
    h.payload_len = (uint16_t)payload_len;

    off = sx_hdr_encode(&h, buf, sizeof(buf));
    for (i = 0; i < list_count; i++) {
        buf[off + 0] = (uint8_t)(list[i] >> 24);
        buf[off + 1] = (uint8_t)(list[i] >> 16);
        buf[off + 2] = (uint8_t)(list[i] >> 8);
        buf[off + 3] = (uint8_t)(list[i]);
        off += 4;
    }
    if (payload_len > 0 && payload != NULL) {
        memcpy(buf + off, payload, payload_len);
        off += payload_len;
    }
    (void)g->ep->io->sendto(g->ep->io, dst_ip, dst_port, buf, off);
    g->last_send_ms = sx_now_ms();
}

static uint32_t group_dst_ip(sx_group_t *g) { return g->link->addr.ip; }
static uint16_t group_dst_port(sx_group_t *g) { return g->link->addr.port; }

sx_group_t *sx_group_create(sx_endpoint_t *ep, sx_link_t *link, uint32_t conv,
                            uint32_t history, uint32_t reorder) {
    sx_group_t *g;
    uint32_t hcap;
    SX_UNUSED(reorder);

    if (ep == NULL || link == NULL) {
        return NULL;
    }
    g = (sx_group_t *)calloc(1, sizeof(*g));
    if (g == NULL) {
        return NULL;
    }
    hcap = sx_round_pow2(history > 0 ? history : 1024u);
    if (hcap == 0) {
        hcap = 1024u;
    }
    g->hist = (sx_msg_t *)calloc(hcap, sizeof(sx_msg_t));
    if (g->hist == NULL) {
        free(g);
        return NULL;
    }
    g->ep = ep;
    g->link = link;
    g->conv = conv;
    g->local_id = (uint32_t)(++ep->next_local_id);
    g->hist_cap = hcap;
    return g;
}

void sx_group_reset(sx_group_t *g) {
    uint32_t i;
    int s;
    if (g == NULL) {
        return;
    }
    for (i = 0; i < g->hist_cap; i++) {
        free(g->hist[i].data);
        g->hist[i].data = NULL;
    }
    memset(g->hist, 0, g->hist_cap * sizeof(sx_msg_t));
    for (s = 0; s < SX_GROUP_MAX_SRC; s++) {
        int r;
        for (r = 0; r < SX_GROUP_REORDER; r++) {
            free(g->src[s].ring[r].data);
            g->src[s].ring[r].data = NULL;
        }
        memset(&g->src[s], 0, sizeof(g->src[s]));
    }
    g->snd_nxt = 0;
    g->hello_sent = 0;
}

void sx_group_destroy(sx_group_t *g) {
    if (g == NULL) {
        return;
    }
    sx_group_reset(g);
    free(g->hist);
    free(g);
}

/* 发送历史中保留一条消息副本。 */
static void group_hist_store(sx_group_t *g, uint32_t seq, const void *data,
                             size_t len) {
    sx_msg_t *slot = &g->hist[seq & (g->hist_cap - 1u)];
    if (slot->data != NULL) {
        free(slot->data);
        slot->data = NULL;
    }
    slot->seq = seq;
    slot->len = (uint16_t)len;
    slot->flags = SX_MSG_USED;
    if (len > 0) {
        slot->data = (uint8_t *)malloc(len);
        if (slot->data != NULL) {
            memcpy(slot->data, data, len);
        } else {
            slot->len = 0;
        }
    }
}

int64_t sx_group_send(sx_group_t *g, const void *data, size_t len) {
    uint32_t seq;
    if (g == NULL || (data == NULL && len > 0)) {
        return SX_ERR_INVALID;
    }
    if (len > g->ep->cfg.max_payload) {
        return SX_ERR_TOO_LARGE;
    }
    seq = g->snd_nxt++;
    group_hist_store(g, seq, data, len);
    /* una 此处复用作“高水位/下一序号”，使接收端可侦测尾部丢包。 */
    sx_group_send_raw(g, SX_CMD_PUSH, seq, g->snd_nxt, 0, NULL, 0, data, len,
                      group_dst_ip(g), group_dst_port(g));
    g->ep->stats.tx_packets++;
    g->ep->stats.tx_bytes += len;
    return (int64_t)seq;
}

static sx_grx_t *group_src_find(sx_group_t *g, uint32_t ip, uint16_t port) {
    int i;
    for (i = 0; i < SX_GROUP_MAX_SRC; i++) {
        if (g->src[i].used && g->src[i].ip == ip && g->src[i].port == port) {
            return &g->src[i];
        }
    }
    for (i = 0; i < SX_GROUP_MAX_SRC; i++) {
        if (!g->src[i].used) {
            sx_grx_t *s = &g->src[i];
            memset(s, 0, sizeof(*s));
            s->used = 1;
            s->ip = ip;
            s->port = port;
            s->last_recv_ms = sx_now_ms();
            return s;
        }
    }
    return NULL;
}

static void group_deliver(sx_group_t *g, sx_grx_t *s, sx_msg_t *slot) {
    sx_ep_deliver(g->ep, g->link, s->ip, s->port, slot->data, slot->len);
    g->ep->stats.rx_packets++;
    g->ep->stats.rx_bytes += slot->len;
}

static void group_drain(sx_group_t *g, sx_grx_t *s) {
    uint32_t mask = SX_GROUP_REORDER - 1u;
    for (;;) {
        sx_msg_t *slot = &s->ring[s->rcv_nxt & mask];
        if (slot->seq != s->rcv_nxt || slot->flags != SX_MSG_USED) {
            break;
        }
        group_deliver(g, s, slot);
        free(slot->data);
        slot->data = NULL;
        slot->flags = SX_MSG_FREE;
        slot->seq = 0;
        s->rcv_nxt++;
        if (s->count > 0) {
            s->count--;
        }
    }
}

/* 向来源发送 NACK，请求缺失序号。 */
static void group_send_nack(sx_group_t *g, sx_grx_t *s) {
    uint32_t list[SX_ACK_LIST_MAX];
    uint32_t n = 0;
    uint32_t mask = SX_GROUP_REORDER - 1u;
    uint32_t seq = s->rcv_nxt;
    uint32_t limit = (s->peer_max != 0) ? s->peer_max : s->rcv_nxt;

    /* 从期望序号向上找空洞，直到遇到已缓存序号或达到对端高水位。 */
    while (n < SX_ACK_LIST_MAX && sx_seq_before_eq(seq, limit) && seq != limit) {
        sx_msg_t *slot = &s->ring[seq & mask];
        if (slot->seq == seq && slot->flags == SX_MSG_USED) {
            break; /* 空洞结束 */
        }
        list[n++] = seq;
        seq++;
        /* 最多看一个重排窗口。 */
        if (seq - s->rcv_nxt >= SX_GROUP_REORDER) {
            break;
        }
    }
    if (n == 0) {
        return;
    }
    sx_group_send_raw(g, SX_CMD_NACK, 0, 0, 0, list, n, NULL, 0, s->ip,
                      s->port);
    s->last_nack_ms = sx_now_ms();
    g->ep->stats.nack_sent++;
}

static void group_input_push(sx_group_t *g, sx_grx_t *s, const sx_hdr_t *h,
                             const uint8_t *payload) {
    uint32_t mask = SX_GROUP_REORDER - 1u;
    uint32_t seq = h->seq;

    if (seq == s->rcv_nxt) {
        /* 先推进期望序号，再投递（回调内可能立即发送）。 */
        s->rcv_nxt++;
        sx_ep_deliver(g->ep, g->link, s->ip, s->port, payload, h->payload_len);
        g->ep->stats.rx_packets++;
        g->ep->stats.rx_bytes += h->payload_len;
        group_drain(g, s);
    } else if (sx_seq_after(seq, s->rcv_nxt) &&
               (seq - s->rcv_nxt) < SX_GROUP_REORDER) {
        sx_msg_t *slot = &s->ring[seq & mask];
        if (slot->seq == seq && slot->flags == SX_MSG_USED) {
            g->ep->stats.dup_packets++;
        } else {
            if (slot->data != NULL) {
                free(slot->data);
                slot->data = NULL;
            }
            if (h->payload_len > 0) {
                slot->data = (uint8_t *)malloc(h->payload_len);
                if (slot->data == NULL) {
                    return;
                }
                memcpy(slot->data, payload, h->payload_len);
            }
            slot->seq = seq;
            slot->len = (uint16_t)h->payload_len;
            slot->flags = SX_MSG_USED;
            s->count++;
            g->ep->stats.reorder_pkts++;
        }
        /* 侦测到空洞，尽快请求重发。 */
        if (g->ep->stats.nack_sent == 0 ||
            sx_now_ms() - s->last_nack_ms >= SX_GROUP_NACK_INTERVAL_MS) {
            group_send_nack(g, s);
        }
    } else {
        g->ep->stats.dup_packets++;
    }
}

void sx_group_input(sx_group_t *g, const uint8_t *seg, size_t seg_len,
                    uint32_t src_ip, uint16_t src_port, uint64_t now_ms) {
    sx_hdr_t h;
    const uint8_t *list;
    const uint8_t *payload;
    sx_grx_t *s;

    if (g == NULL || seg == NULL) {
        return;
    }
    if (sx_hdr_decode(seg, seg_len, &h) != SX_OK || h.conv != g->conv) {
        return;
    }
    list = seg + SX_HDR_SIZE;
    payload = list + (size_t)h.ack_count * 4u;

    s = group_src_find(g, src_ip, src_port);
    if (s == NULL) {
        return;
    }
    s->last_recv_ms = now_ms;
    /* una 复用为发送端高水位，用于尾部空洞侦测。 */
    if (sx_seq_after(h.una, s->peer_max)) {
        s->peer_max = h.una;
    }

    switch (h.cmd) {
    case SX_CMD_PUSH:
        group_input_push(g, s, &h, payload);
        break;
    case SX_CMD_NACK: {
        /* 重发历史中仍保留的缺失序号。 */
        uint32_t i;
        const uint8_t *p = list;
        for (i = 0; i < h.ack_count; i++) {
            uint32_t m = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
            p += 4;
            if (sx_seq_before_eq(m, g->snd_nxt) &&
                (g->snd_nxt - m) <= g->hist_cap) {
                sx_msg_t *slot = &g->hist[m & (g->hist_cap - 1u)];
                if (slot->seq == m && slot->flags == SX_MSG_USED) {
                    /* 组播/广播：重发到组地址，使所有成员都能恢复；
                     * 已持有的成员按重复包丢弃。 */
                    sx_group_send_raw(g, SX_CMD_PUSH, m, g->snd_nxt, 0, NULL, 0,
                                      slot->data, slot->len, group_dst_ip(g),
                                      group_dst_port(g));
                    g->ep->stats.retransmits++;
                }
            }
        }
        g->ep->stats.nack_recv++;
        break;
    }
    case SX_CMD_PING:
        sx_group_send_raw(g, SX_CMD_PONG, 0, 0, 0, NULL, 0, NULL, 0, src_ip,
                          src_port);
        break;
    default:
        break;
    }
}

void sx_group_update(sx_group_t *g, uint64_t now_ms) {
    int i;
    uint32_t keepalive;

    if (g == NULL) {
        return;
    }
    /* 对存在空洞的来源周期性重发 NACK。 */
    for (i = 0; i < SX_GROUP_MAX_SRC; i++) {
        sx_grx_t *s = &g->src[i];
        if (!s->used) {
            continue;
        }
        if (now_ms - s->last_recv_ms > SX_GROUP_SRC_TIMEOUT_MS) {
            int r;
            for (r = 0; r < SX_GROUP_REORDER; r++) {
                free(s->ring[r].data);
                s->ring[r].data = NULL;
            }
            memset(s, 0, sizeof(*s));
            continue;
        }
        /* 只要期望序号落后于对端高水位（含尾部丢包），就请求重发。 */
        if (s->rcv_nxt != s->peer_max &&
            now_ms - s->last_nack_ms >= SX_GROUP_NACK_INTERVAL_MS) {
            group_send_nack(g, s);
        }
    }
    /* 周期性广播高水位，使接收端能侦测“尾部丢包”。 */
    if (g->snd_nxt > 0 && now_ms - g->last_send_ms >= SX_GROUP_NACK_INTERVAL_MS) {
        sx_group_send_raw(g, SX_CMD_WINS, 0, g->snd_nxt, 0, NULL, 0, NULL, 0,
                          group_dst_ip(g), group_dst_port(g));
    }
    keepalive = g->ep->cfg.keepalive_ms ? g->ep->cfg.keepalive_ms : 10000u;
    if (now_ms - g->last_send_ms >= keepalive) {
        sx_group_send_raw(g, SX_CMD_PING, 0, g->snd_nxt, 0, NULL, 0, NULL, 0,
                          group_dst_ip(g), group_dst_port(g));
    }
}
