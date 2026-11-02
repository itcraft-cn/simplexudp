/*
 * sx_frag.c - 消息分片重组
 *
 * 分片头（网络序）：
 *   msg_id(4) | frag_idx(2) | frag_cnt(2) | total_len(4)
 *
 * 约束：
 *   - 分片由 KCP 按消息序号可靠、按序交付，因此重组只处理顺序到达；
 *   - 如果首片丢失，后续片会被丢弃，等待 KCP 重传首片；
 *   - total_len 受配置 max_message 限制，避免恶意长度导致大内存分配。
 */
#include "sx_internal.h"

#include <stdlib.h>
#include <string.h>

void sx_frag_reasm_reset(sx_frag_reasm_t *r) {
    if (r == NULL) {
        return;
    }
    free(r->buf);
    memset(r, 0, sizeof(*r));
}

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t get_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int sx_frag_reasm_input(sx_frag_reasm_t *r, const uint8_t *payload, size_t len,
                        uint32_t max_total, const sx_pkt_info_t *info,
                        const uint8_t **out, size_t *out_len,
                        sx_pkt_info_t *out_info) {
    uint32_t msg_id;
    uint32_t total;
    uint16_t idx;
    uint16_t cnt;
    size_t data_len;

    if (r == NULL || payload == NULL || len < SX_FRAG_HDR_SIZE) {
        return -1;
    }
    msg_id = get_u32(payload);
    idx = get_u16(payload + 4);
    cnt = get_u16(payload + 6);
    total = get_u32(payload + 8);
    data_len = len - SX_FRAG_HDR_SIZE;

    if (cnt == 0 || total == 0 || total > max_total || idx >= cnt) {
        return -1;
    }
    if (idx == 0) {
        /* 新消息开始：丢弃旧的未完成状态。 */
        sx_frag_reasm_reset(r);
    }
    if (r->active &&
        (r->msg_id != msg_id || r->frag_cnt != cnt || r->total_len != total)) {
        sx_frag_reasm_reset(r);
    }
    if (!r->active) {
        if (idx != 0) {
            return 0; /* 首片未到，暂不缓存后续片 */
        }
        r->buf = (uint8_t *)malloc(total);
        if (r->buf == NULL) {
            return -1;
        }
        r->msg_id = msg_id;
        r->total_len = total;
        r->frag_cnt = cnt;
        r->next_idx = 0;
        r->received = 0;
        r->active = 1;
        if (info != NULL) {
            r->info = *info;
        } else {
            memset(&r->info, 0, sizeof(r->info));
        }
    }
    if (idx != r->next_idx || r->received + data_len > r->total_len) {
        sx_frag_reasm_reset(r);
        return -1;
    }
    memcpy(r->buf + r->received, payload + SX_FRAG_HDR_SIZE, data_len);
    r->received += (uint32_t)data_len;
    r->next_idx++;
    if (r->next_idx == r->frag_cnt && r->received == r->total_len) {
        *out = r->buf;
        *out_len = r->total_len;
        if (out_info != NULL) {
            *out_info = r->info;
        }
        return 1;
    }
    return 0;
}
