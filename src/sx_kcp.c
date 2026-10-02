/*
 * sx_kcp.c - 可靠传输引擎（借鉴 KCP）
 *
 * 职责：
 *   - 消息序号分配与滑动窗口发送；
 *   - 累积确认(una) + 选择性确认(ack_list) + 快速重传(fastack) + RTO 退避；
 *   - RTT 估计与自适应 RTO；
 *   - 接收端乱序重排、按序投递、去重；
 *   - 流控（对端窗口）与拥塞控制（慢启动/拥塞避免/超时降窗）；
 *   - 建链问候(HELLO)、窗口探测(WASK/WINS)、心跳(PING/PONG)。
 *
 * 设计要点：
 *   - 一包一消息，序号即消息序号，无需分片/重组。
 *   - 发送环/接收环容量取 2 的幂，按 seq & (cap-1) 索引；窗口不超过容量，
 *     保证环内不产生别名冲突。
 *   - 本文件不直接操作 socket，统一经 sx_kcp_send_raw -> ep->io->sendto。
 */
#include "sx_internal.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 发送底层                                                            */
/* ------------------------------------------------------------------ */

void sx_kcp_send_raw(sx_kcp_t *k, uint8_t cmd, uint32_t seq, uint32_t una,
                     uint32_t wnd, const uint32_t *ack_list, uint32_t ack_count,
                     const void *payload, size_t payload_len) {
    uint8_t buf[SX_HDR_SIZE + SX_ACK_LIST_MAX * 4 + SX_MAX_PAYLOAD_LIMIT];
    sx_hdr_t h;
    size_t off;
    uint32_t i;

    if (k == NULL || k->ep == NULL || k->ep->io == NULL) {
        return;
    }
    if (ack_count > SX_ACK_LIST_MAX) {
        ack_count = SX_ACK_LIST_MAX;
    }
    if (payload_len > k->ep->cfg.max_payload) {
        return;
    }

    memset(&h, 0, sizeof(h));
    h.cmd = cmd;
    h.ver = SX_PROTO_VER;
    h.conv = k->conv;
    h.peer_id = k->local_id;
    h.seq = seq;
    h.una = una;
    h.ts = (uint32_t)(sx_now_ms() & 0xffffffffu);
    h.wnd = wnd;
    h.ack_count = (uint16_t)ack_count;
    h.payload_len = (uint16_t)payload_len;

    off = sx_hdr_encode(&h, buf, sizeof(buf));
    for (i = 0; i < ack_count; i++) {
        buf[off + 0] = (uint8_t)(ack_list[i] >> 24);
        buf[off + 1] = (uint8_t)(ack_list[i] >> 16);
        buf[off + 2] = (uint8_t)(ack_list[i] >> 8);
        buf[off + 3] = (uint8_t)(ack_list[i]);
        off += 4;
    }
    if (payload_len > 0 && payload != NULL) {
        memcpy(buf + off, payload, payload_len);
        off += payload_len;
    }
    (void)k->ep->io->sendto(k->ep->io, k->link->addr.ip, k->link->addr.port,
                            buf, off);
    k->last_send_ms = sx_now_ms();
}

/* 发送一个控制/数据段，自动捎带待发送的 ACK 列表并清除 pending。 */
static void kcp_emit(sx_kcp_t *k, uint8_t cmd, uint32_t seq, uint32_t payload_len) {
    uint32_t list[SX_ACK_LIST_MAX] = {0};
    uint32_t n = 0;
    uint32_t i;

    if (k->ack_pending) {
        n = k->ack_count;
        for (i = 0; i < n && i < SX_ACK_LIST_MAX; i++) {
            list[i] = k->ack_list[i];
        }
        k->ack_pending = 0;
        k->ack_count = 0;
    }
    /* una 为累积确认：本端已收到的连续序号（对端发送流）。 */
    sx_kcp_send_raw(k, cmd, seq, k->rcv_nxt, k->rcv_wnd, list, n, NULL,
                    payload_len);
}

/* 发送时捎带 ack 的变体（带净荷）。 */
static void kcp_emit_data(sx_kcp_t *k, uint32_t seq, const void *data,
                          size_t len) {
    uint32_t list[SX_ACK_LIST_MAX] = {0};
    uint32_t n = 0;
    uint32_t i;

    if (k->ack_pending) {
        n = k->ack_count;
        for (i = 0; i < n && i < SX_ACK_LIST_MAX; i++) {
            list[i] = k->ack_list[i];
        }
        k->ack_pending = 0;
        k->ack_count = 0;
    }
    sx_kcp_send_raw(k, SX_CMD_PUSH, seq, k->rcv_nxt, k->rcv_wnd, list, n, data,
                    len);
}

/* ------------------------------------------------------------------ */
/* 生命周期                                                            */
/* ------------------------------------------------------------------ */

sx_kcp_t *sx_kcp_create(sx_endpoint_t *ep, sx_link_t *link, uint32_t conv,
                        uint32_t send_window, uint32_t recv_window) {
    sx_kcp_t *k;
    uint32_t scap;
    uint32_t rcap;

    if (ep == NULL || link == NULL) {
        return NULL;
    }
    k = (sx_kcp_t *)calloc(1, sizeof(*k));
    if (k == NULL) {
        return NULL;
    }
    scap = sx_round_pow2(send_window > 0 ? send_window * 2u : 512u);
    rcap = sx_round_pow2(recv_window > 0 ? recv_window : 1024u);
    if (scap < SX_SND_CAP_MIN) scap = SX_SND_CAP_MIN;
    if (rcap < SX_RCV_CAP_MIN) rcap = SX_RCV_CAP_MIN;

    k->snd_buf = (sx_msg_t *)calloc(scap, sizeof(sx_msg_t));
    k->rcv_buf = (sx_msg_t *)calloc(rcap, sizeof(sx_msg_t));
    if (k->snd_buf == NULL || k->rcv_buf == NULL) {
        free(k->snd_buf);
        free(k->rcv_buf);
        free(k);
        return NULL;
    }

    k->ep = ep;
    k->link = link;
    k->conv = conv;
    k->local_id = (uint32_t)(++ep->next_local_id);
    k->send_cap = scap;
    k->recv_cap = rcap;
    k->snd_wnd = send_window > 0 ? send_window : 256u;
    k->cwnd = SX_MIN(k->snd_wnd, 16u);
    k->cwnd_cnt = 0;
    k->ssthresh = k->snd_wnd;
    k->rcv_wnd = rcap - 1;
    k->min_rto = ep->cfg.min_rto_ms ? ep->cfg.min_rto_ms : 100u;
    k->max_rto = ep->cfg.max_rto_ms ? ep->cfg.max_rto_ms : 5000u;
    k->rto_ms = k->min_rto;
    k->last_recv_ms = sx_now_ms();
    k->last_update_ms = k->last_recv_ms;
    return k;
}

void sx_kcp_reset(sx_kcp_t *k) {
    uint32_t i;
    if (k == NULL) {
        return;
    }
    for (i = 0; i < k->send_cap; i++) {
        free(k->snd_buf[i].data);
        k->snd_buf[i].data = NULL;
    }
    for (i = 0; i < k->recv_cap; i++) {
        free(k->rcv_buf[i].data);
        k->rcv_buf[i].data = NULL;
    }
    memset(k->snd_buf, 0, k->send_cap * sizeof(sx_msg_t));
    memset(k->rcv_buf, 0, k->recv_cap * sizeof(sx_msg_t));
    k->snd_una = 0;
    k->snd_nxt = 0;
    k->snd_count = 0;
    k->rcv_nxt = 0;
    k->rcv_count = 0;
    k->cwnd = SX_MIN(k->snd_wnd, 16u);
    k->ssthresh = k->snd_wnd;
    k->ack_pending = 0;
    k->ack_count = 0;
    k->established = 0;
    k->hello_sent = 0;
    k->up = 0;
    k->srtt = 0;
    k->rttvar = 0;
    k->rto_ms = k->min_rto;
}

void sx_kcp_destroy(sx_kcp_t *k) {
    if (k == NULL) {
        return;
    }
    sx_kcp_reset(k);
    free(k->snd_buf);
    free(k->rcv_buf);
    free(k);
}

/* ------------------------------------------------------------------ */
/* 发送                                                                */
/* ------------------------------------------------------------------ */

int64_t sx_kcp_send(sx_kcp_t *k, const void *data, size_t len) {
    uint32_t window;
    uint32_t seq;
    sx_msg_t *slot;
    uint8_t *copy;

    if (k == NULL || (data == NULL && len > 0)) {
        return SX_ERR_INVALID;
    }
    if (len > k->ep->cfg.max_payload) {
        return SX_ERR_TOO_LARGE;
    }
    window = SX_MIN(k->snd_wnd, k->cwnd);
    if (k->snd_count >= window) {
        return SX_ERR_WINDOW;
    }
    seq = k->snd_nxt;
    slot = &k->snd_buf[seq & (k->send_cap - 1u)];
    if (slot->data != NULL) {
        /* 环回绕前的残留（理论上不应发生，防御性释放）。 */
        free(slot->data);
        slot->data = NULL;
    }
    copy = NULL;
    if (len > 0) {
        copy = (uint8_t *)malloc(len);
        if (copy == NULL) {
            /* 复位槽位，避免留下 data=NULL 但 seq/flags 陈旧的半初始化状态。 */
            slot->seq = 0;
            slot->len = 0;
            slot->xmit = 0;
            slot->fastack = 0;
            slot->xmit_ts = 0;
            slot->flags = SX_MSG_FREE;
            return SX_ERR_NOMEM;
        }
        memcpy(copy, data, len);
    }
    slot->seq = seq;
    slot->len = (uint16_t)len;
    slot->xmit = 0;
    slot->fastack = 0;
    slot->flags = SX_MSG_USED;
    slot->data = copy;
    slot->xmit_ts = sx_now_ms();

    k->snd_nxt++;
    k->snd_count++;

    /* 立即发送（捎带 ACK）。 */
    kcp_emit_data(k, seq, copy, len);
    slot->xmit = 1;
    k->ep->stats.tx_packets++;
    k->ep->stats.tx_bytes += len;
    return (int64_t)seq;
}

/* ------------------------------------------------------------------ */
/* RTT 与确认处理                                                      */
/* ------------------------------------------------------------------ */

static void kcp_update_rtt(sx_kcp_t *k, uint32_t r) {
    if (r == 0) {
        r = 1;
    }
    if (k->srtt == 0) {
        k->srtt = r;
        k->rttvar = r / 2;
    } else {
        uint32_t diff = (k->srtt > r) ? (k->srtt - r) : (r - k->srtt);
        k->rttvar = (3u * k->rttvar + diff) / 4u;
        k->srtt = (7u * k->srtt + r) / 8u;
    }
    k->rto_ms = k->srtt + 4u * k->rttvar;
    if (k->rto_ms < k->min_rto) k->rto_ms = k->min_rto;
    if (k->rto_ms > k->max_rto) k->rto_ms = k->max_rto;
}

/* 确认单个序号。返回 1 表示新确认。 */
static int kcp_parse_ack(sx_kcp_t *k, uint32_t sn, uint64_t now) {
    uint32_t mask = k->send_cap - 1u;
    sx_msg_t *slot;

    /* sn 必须落在 [snd_una, snd_nxt)（允许等于 snd_una）。 */
    if (sx_seq_after(k->snd_una, sn) || sx_seq_after(sn, k->snd_nxt)) {
        return 0;
    }
    slot = &k->snd_buf[sn & mask];
    if (slot->seq != sn || slot->flags != SX_MSG_USED) {
        return 0;
    }
    if (slot->xmit == 1) {
        kcp_update_rtt(k, (uint32_t)(now - slot->xmit_ts));
    }
    slot->flags = SX_MSG_ACKED;
    return 1;
}

/* 对序号 < sn 的在途消息累加 fastack（KCP 快速重传思想）。
 * 注意：sn 可能落后于 snd_una（陈旧/重复 ACK），必须直接返回，否则
 * 以 != 为终止条件的循环会遍历整个 32 位序号空间导致长时间卡死。 */
static void kcp_skip_acks(sx_kcp_t *k, uint32_t sn) {
    uint32_t s;
    uint32_t stop;
    uint32_t mask = k->send_cap - 1u;

    if (!sx_seq_after(sn, k->snd_una)) {
        return; /* sn <= snd_una：无需要标记的跨越 */
    }
    stop = sx_seq_after(sn, k->snd_nxt) ? k->snd_nxt : sn;
    for (s = k->snd_una; s != stop; s++) {
        sx_msg_t *slot = &k->snd_buf[s & mask];
        if (slot->seq == s && slot->flags == SX_MSG_USED) {
            if (slot->fastack < 0xffffu) {
                slot->fastack++;
            }
        }
    }
}

/* 推进 snd_una，释放已确认槽位。 */
static void kcp_advance_una(sx_kcp_t *k) {
    uint32_t mask = k->send_cap - 1u;
    while (k->snd_una != k->snd_nxt) {
        sx_msg_t *slot = &k->snd_buf[k->snd_una & mask];
        if (slot->flags != SX_MSG_ACKED) {
            break;
        }
        free(slot->data);
        slot->data = NULL;
        slot->flags = SX_MSG_FREE;
        slot->seq = 0;
        slot->len = 0;
        k->snd_una++;
        if (k->snd_count > 0) {
            k->snd_count--;
        }
    }
}

/* 拥塞窗口增长（收到新确认）。 */
static void kcp_on_ack(sx_kcp_t *k) {
    if (k->cwnd < k->ssthresh) {
        k->cwnd++;
    } else {
        k->cwnd_cnt++;
        if (k->cwnd_cnt >= k->cwnd) {
            k->cwnd_cnt = 0;
            k->cwnd++;
        }
    }
    if (k->cwnd > k->send_cap) {
        k->cwnd = k->send_cap;
    }
}

static void kcp_retransmit(sx_kcp_t *k, sx_msg_t *slot) {
    kcp_emit_data(k, slot->seq, slot->data, slot->len);
    slot->xmit++;
    slot->xmit_ts = sx_now_ms();
    slot->fastack = 0;
    k->ep->stats.retransmits++;
}

/* 快速重传扫描。 */
static void kcp_fast_retrans(sx_kcp_t *k) {
    uint32_t s;
    uint32_t mask = k->send_cap - 1u;
    for (s = k->snd_una; s != k->snd_nxt; s++) {
        sx_msg_t *slot = &k->snd_buf[s & mask];
        if (slot->seq == s && slot->flags == SX_MSG_USED &&
            slot->fastack >= SX_FASTACK_THRESH) {
            kcp_retransmit(k, slot);
            k->ep->stats.fast_retrans++;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 接收处理                                                            */
/* ------------------------------------------------------------------ */

static void kcp_add_ack(sx_kcp_t *k, uint32_t seq) {
    uint32_t i;
    for (i = 0; i < k->ack_count; i++) {
        if (k->ack_list[i] == seq) {
            return; /* 去重 */
        }
    }
    if (k->ack_count < SX_ACK_LIST_MAX) {
        k->ack_list[k->ack_count++] = seq;
    }
    k->ack_pending = 1;
}

static void kcp_deliver(sx_kcp_t *k, sx_msg_t *slot) {
    sx_ep_deliver(k->ep, k->link, k->link->addr.ip, k->link->addr.port,
                  slot->data, slot->len);
    k->ep->stats.rx_packets++;
    k->ep->stats.rx_bytes += slot->len;
}

/* 投递按序到达的消息直到空洞。 */
static void kcp_drain(sx_kcp_t *k) {
    uint32_t mask = k->recv_cap - 1u;
    uint32_t guard;
    for (guard = 0; guard < k->recv_cap; guard++) {
        sx_msg_t *slot = &k->rcv_buf[k->rcv_nxt & mask];
        if (slot->seq != k->rcv_nxt || slot->flags != SX_MSG_USED) {
            break;
        }
        kcp_deliver(k, slot);
        free(slot->data);
        slot->data = NULL;
        slot->flags = SX_MSG_FREE;
        slot->seq = 0;
        k->rcv_nxt++;
        if (k->rcv_count > 0) {
            k->rcv_count--;
        }
    }
    k->rcv_wnd = k->recv_cap - k->rcv_count - 1u;
}

/* 处理 PUSH 数据。 */
static void kcp_input_push(sx_kcp_t *k, const sx_hdr_t *h, const uint8_t *payload,
                           uint64_t now) {
    uint32_t mask = k->recv_cap - 1u;
    uint32_t seq = h->seq;

    if (seq == k->rcv_nxt) {
        /* 先推进 rcv_nxt，再投递：这样回调内若立即发送（如应用回显），
         * 其携带的累积确认 una 才是最新的，不会漏确认当前这条消息。 */
        k->rcv_nxt++;
        sx_ep_deliver(k->ep, k->link, k->link->addr.ip, k->link->addr.port,
                      payload, h->payload_len);
        k->ep->stats.rx_packets++;
        k->ep->stats.rx_bytes += h->payload_len;
        kcp_drain(k);
        kcp_add_ack(k, seq);
    } else if (sx_seq_after(seq, k->rcv_nxt) &&
               (seq - k->rcv_nxt) < k->recv_cap) {
        sx_msg_t *slot = &k->rcv_buf[seq & mask];
        if (slot->seq == seq && slot->flags == SX_MSG_USED) {
            k->ep->stats.dup_packets++;
            kcp_add_ack(k, seq);
            return;
        }
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
        slot->xmit = 0;
        slot->fastack = 0;
        k->rcv_count++;
        k->ep->stats.reorder_pkts++;
        sx_log(k->ep, SX_LOG_DEBUG, "kcp ooo seq=%u rcv_nxt=%u", seq,
               k->rcv_nxt);
        kcp_add_ack(k, seq);
    } else {
        /* 老包/重复包：仅补 ACK。 */
        k->ep->stats.dup_packets++;
        sx_log(k->ep, SX_LOG_DEBUG, "kcp dup seq=%u rcv_nxt=%u", seq,
               k->rcv_nxt);
        kcp_add_ack(k, seq);
    }
    (void)now;
}

/* 处理一条段携带的确认信息：累积确认(una) + 选择性确认(ack_list)，
 * 并标记需要快速重传的序号。返回是否产生了新确认。 */
static int kcp_process_acks(sx_kcp_t *k, const sx_hdr_t *h,
                            const uint8_t *list, uint64_t now_ms) {
    uint32_t i;
    int new_ack = 0;

    /* 累积确认：对端 una 之前的数据均已被接收（上限钳制到 snd_nxt，
     * 避免异常/陈旧 una 导致长时间循环）。 */
    if (sx_seq_after(h->una, k->snd_una)) {
        uint32_t s;
        uint32_t stop = sx_seq_after(h->una, k->snd_nxt) ? k->snd_nxt : h->una;
        for (s = k->snd_una; s != stop; s++) {
            if (kcp_parse_ack(k, s, now_ms)) {
                new_ack = 1;
                kcp_on_ack(k);
            }
        }
        kcp_skip_acks(k, stop);
    }
    /* 选择性确认列表（对任意命令都处理）。 */
    for (i = 0; i < h->ack_count; i++) {
        uint32_t a = ((uint32_t)list[0] << 24) | ((uint32_t)list[1] << 16) |
                     ((uint32_t)list[2] << 8) | (uint32_t)list[3];
        list += 4;
        if (kcp_parse_ack(k, a, now_ms)) {
            new_ack = 1;
            kcp_on_ack(k);
        }
        kcp_skip_acks(k, a);
    }
    return new_ack;
}

void sx_kcp_input(sx_kcp_t *k, const uint8_t *seg, size_t seg_len,
                  uint64_t now_ms) {
    sx_hdr_t h;
    const uint8_t *list;
    const uint8_t *payload;
    int new_ack = 0;

    if (k == NULL || seg == NULL) {
        return;
    }
    if (sx_hdr_decode(seg, seg_len, &h) != SX_OK) {
        k->ep->stats.dropped++;
        sx_log(k->ep, SX_LOG_DEBUG, "kcp drop decode len=%zu", seg_len);
        return;
    }
    if (h.conv != k->conv) {
        return; /* 非本会话 */
    }
    if (h.peer_id != 0 && h.peer_id != k->remote_id) {
        k->remote_id = h.peer_id;
    }
    k->last_recv_ms = now_ms;

    list = seg + SX_HDR_SIZE;
    payload = list + (size_t)h.ack_count * 4u;

    /* 对端通告窗口。 */
    if (h.wnd < k->snd_wnd) {
        k->snd_wnd = h.wnd;
    } else if (h.wnd > k->snd_wnd) {
        k->snd_wnd = SX_MIN(h.wnd, k->send_cap);
    }

    /* 累积确认 + 选择性确认 + 快速重传标记。
     * 说明：累积确认统一由 una 表达（对端 rcv_nxt），选择性确认由 ack_list
     * 表达；ACK 命令的 seq 字段不用于逐序号确认，避免把“期望序号”误认成
     * “已收序号”导致确认空洞。 */
    new_ack = kcp_process_acks(k, &h, list, now_ms);

    kcp_advance_una(k);
    if (new_ack) {
        kcp_fast_retrans(k);
    }

    switch (h.cmd) {
    case SX_CMD_PUSH:
        kcp_input_push(k, &h, payload, now_ms);
        if (!k->established && h.peer_id != 0) {
            k->established = 1;
            k->up = 1;
            sx_ep_emit(k->ep, SX_EVENT_PEER_UP, k->link);
        }
        break;
    case SX_CMD_HELLO:
        /* 仅在未建链时回一次 HELLO，避免双方互答造成风暴。 */
        if (!k->established) {
            k->established = 1;
            k->up = 1;
            kcp_emit(k, SX_CMD_HELLO, 0, 0);
            sx_ep_emit(k->ep, SX_EVENT_PEER_UP, k->link);
        }
        break;
    case SX_CMD_ACK:
    case SX_CMD_WINS:
    case SX_CMD_PING:
        if (h.cmd == SX_CMD_PING) {
            kcp_emit(k, SX_CMD_PONG, 0, 0);
        }
        break;
    case SX_CMD_WASK:
        kcp_emit(k, SX_CMD_WINS, 0, 0);
        break;
    case SX_CMD_PONG:
        break;
    case SX_CMD_BYE:
        k->up = 0;
        k->established = 0;
        sx_ep_emit(k->ep, SX_EVENT_PEER_DOWN, k->link);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* 周期驱动                                                            */
/* ------------------------------------------------------------------ */

static void kcp_check_retrans(sx_kcp_t *k, uint64_t now) {
    uint32_t s;
    uint32_t mask = k->send_cap - 1u;
    int timeout = 0;

    for (s = k->snd_una; s != k->snd_nxt; s++) {
        sx_msg_t *slot = &k->snd_buf[s & mask];
        if (slot->seq != s || slot->flags != SX_MSG_USED) {
            continue;
        }
        if (now - slot->xmit_ts >= k->rto_ms) {
            kcp_retransmit(k, slot);
            timeout = 1;
            k->ep->stats.timeouts++;
        }
    }
    if (timeout) {
        /* RTO 退避 + 超时降窗。 */
        k->rto_ms = SX_MIN(k->rto_ms * 2u, k->max_rto);
        k->ssthresh = SX_MAX(k->cwnd / 2u, 2u);
        k->cwnd = 1;
        k->cwnd_cnt = 0;
    }
}

void sx_kcp_update(sx_kcp_t *k, uint64_t now_ms) {
    uint32_t keepalive;
    if (k == NULL) {
        return;
    }
    k->last_update_ms = now_ms;

    /* 建链问候。 */
    if (!k->hello_sent) {
        kcp_emit(k, SX_CMD_HELLO, 0, 0);
        k->hello_sent = 1;
    }

    /* 待发送 ACK。 */
    if (k->ack_pending) {
        kcp_emit(k, SX_CMD_ACK, k->rcv_nxt, 0);
    }

    /* 重传检查。 */
    kcp_check_retrans(k, now_ms);

    /* 零窗口探测。 */
    if (k->snd_wnd == 0 && k->snd_count > 0) {
        if (now_ms - k->last_ack_ts >= k->rto_ms) {
            kcp_emit(k, SX_CMD_WASK, 0, 0);
            k->last_ack_ts = now_ms;
        }
    }

    /* 心跳。 */
    keepalive = k->ep->cfg.keepalive_ms ? k->ep->cfg.keepalive_ms : 10000u;
    if (k->snd_count == 0 && now_ms - k->last_send_ms >= keepalive) {
        kcp_emit(k, SX_CMD_PING, 0, 0);
    }
}
