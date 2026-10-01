/*
 * test_proto.c - SX 协议头编解码测试
 *
 * 覆盖：字段往返、magic/版本校验、长度不足与越界列表拒绝。
 */
#include "../src/sx_internal.h"

#include <string.h>

#include "sx_test.h"

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "test_proto.log";
    uint8_t buf[SX_HDR_SIZE + 8 + 100];
    sx_hdr_t h;
    sx_hdr_t d;

    T_BEGIN(path);

    memset(&h, 0, sizeof(h));
    h.cmd = SX_CMD_PUSH;
    h.ver = SX_PROTO_VER;
    h.conv = 0xdeadbeefu;
    h.peer_id = 7;
    h.seq = 100;
    h.una = 50;
    h.ts = 12345;
    h.wnd = 256;
    h.ack_count = 2;
    h.payload_len = 10;

    T_CHECK(sx_hdr_encode(&h, buf, sizeof(buf)) == SX_HDR_SIZE, "encode len");
    put32(buf + SX_HDR_SIZE, 11);
    put32(buf + SX_HDR_SIZE + 4, 13);
    memset(buf + SX_HDR_SIZE + 8, 0xaa, 10);

    T_CHECK(sx_hdr_decode(buf, SX_HDR_SIZE + 8 + 10, &d) == SX_OK, "decode");
    T_CHECK(d.cmd == SX_CMD_PUSH, "cmd");
    T_CHECK(d.conv == 0xdeadbeefu, "conv");
    T_CHECK(d.peer_id == 7, "peer_id");
    T_CHECK(d.seq == 100, "seq");
    T_CHECK(d.una == 50, "una");
    T_CHECK(d.ts == 12345, "ts");
    T_CHECK(d.wnd == 256, "wnd");
    T_CHECK(d.ack_count == 2, "ack_count");
    T_CHECK(d.payload_len == 10, "payload_len");

    /* 长度不足。 */
    T_CHECK(sx_hdr_decode(buf, SX_HDR_SIZE - 1, &d) != SX_OK, "short rejected");

    /* magic 错误。 */
    buf[0] ^= 0xff;
    T_CHECK(sx_hdr_decode(buf, SX_HDR_SIZE, &d) != SX_OK, "bad magic");
    buf[0] ^= 0xff;

    /* 版本错误。 */
    buf[2] = 99;
    T_CHECK(sx_hdr_decode(buf, sizeof(buf), &d) != SX_OK, "bad version");
    buf[2] = SX_PROTO_VER;

    /* 声称的列表/净荷超出实际长度。 */
    {
        sx_hdr_t bad = h;
        bad.ack_count = 64;
        bad.payload_len = 1400;
        sx_hdr_encode(&bad, buf, sizeof(buf));
        T_CHECK(sx_hdr_decode(buf, SX_HDR_SIZE + 8 + 10, &d) != SX_OK,
                "oversize declared rejected");
    }

    /* 编码缓冲不足。 */
    T_CHECK(sx_hdr_encode(&h, buf, SX_HDR_SIZE - 1) == 0, "encode short buf");

    T_END();
}
