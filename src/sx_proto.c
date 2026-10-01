/*
 * sx_proto.c - SX 协议头编解码
 *
 * 线格式（网络字节序，固定 32 字节）：
 *   off  0  uint16 magic = 0x5358 ('S','X')
 *   off  2  uint8  ver
 *   off  3  uint8  cmd
 *   off  4  uint32 conv
 *   off  8  uint32 peer_id   发送方本地 peer 标识
 *   off 12  uint32 seq       PUSH: 消息序号; ACK: 确认号
 *   off 16  uint32 una       发送方最小未确认序号
 *   off 20  uint32 ts        发送毫秒时间戳
 *   off 24  uint32 wnd       接收窗口（消息数）
 *   off 28  uint16 ack_count 选择性确认/缺失序号条数
 *   off 30  uint16 payload_len
 *   off 32  uint32 list[ack_count]（可选）
 *
 * 注意：所有多字节字段均为大端（网络序）。
 */
#include "sx_internal.h"

#include <string.h>

static inline void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}

static inline void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xff);
}

static inline uint16_t get16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

size_t sx_hdr_encode(const sx_hdr_t *h, uint8_t *buf, size_t cap) {
    if (h == NULL || buf == NULL || cap < SX_HDR_SIZE) {
        return 0;
    }
    put16(buf + 0, SX_MAGIC);
    buf[2] = (uint8_t)(h->ver != 0 ? h->ver : SX_PROTO_VER);
    buf[3] = h->cmd;
    put32(buf + 4, h->conv);
    put32(buf + 8, h->peer_id);
    put32(buf + 12, h->seq);
    put32(buf + 16, h->una);
    put32(buf + 20, h->ts);
    put32(buf + 24, h->wnd);
    put16(buf + 28, h->ack_count);
    put16(buf + 30, h->payload_len);
    return SX_HDR_SIZE;
}

int sx_hdr_decode(const uint8_t *buf, size_t len, sx_hdr_t *out) {
    if (buf == NULL || out == NULL) {
        return SX_ERR_INVALID;
    }
    if (len < SX_HDR_SIZE) {
        return SX_ERR_INVALID;
    }
    if (get16(buf + 0) != SX_MAGIC) {
        return SX_ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));
    out->ver = buf[2];
    out->cmd = buf[3];
    out->conv = get32(buf + 4);
    out->peer_id = get32(buf + 8);
    out->seq = get32(buf + 12);
    out->una = get32(buf + 16);
    out->ts = get32(buf + 20);
    out->wnd = get32(buf + 24);
    out->ack_count = get16(buf + 28);
    out->payload_len = get16(buf + 30);

    if (out->ver != SX_PROTO_VER) {
        return SX_ERR_UNSUPPORTED;
    }
    /* 列表与净荷必须落在报文长度内。 */
    if ((size_t)SX_HDR_SIZE + (size_t)out->ack_count * 4u +
            (size_t)out->payload_len >
        len) {
        return SX_ERR_INVALID;
    }
    return SX_OK;
}
