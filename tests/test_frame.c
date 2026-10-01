/*
 * test_frame.c - Ethernet/IPv4/UDP 帧构造与解析、校验和测试
 *
 * 覆盖：
 *   - 构造/解析往返，字段与净荷一致；
 *   - 篡改 IP 校验和 / UDP 净荷后解析必须失败（校验和防御）。
 */
#include "../src/sx_internal.h"

#include <string.h>

#include "sx_test.h"

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "test_frame.log";
    uint8_t src_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    uint8_t dst_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
    uint8_t frame[1600];
    uint8_t payload[200];
    uint32_t sip = 0x0a000001u;
    uint32_t dip = 0x0a000002u;
    uint16_t sport = 1111;
    uint16_t dport = 2222;
    ssize_t flen;
    size_t i;

    T_BEGIN(path);

    for (i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i * 7 + 3);
    }

    flen = sx_frame_build(frame, sizeof(frame), src_mac, dst_mac, sip, dip,
                          sport, dport, payload, sizeof(payload));
    T_CHECK(flen == (ssize_t)(14 + 20 + 8 + sizeof(payload)),
            "frame length");
    T_CHECK(sizeof(payload) <= SX_MAX_PAYLOAD_LIMIT, "payload bound");

    {
        uint32_t osip = 0, odip = 0;
        uint16_t osp = 0, odp = 0;
        const uint8_t *op = NULL;
        size_t olen = 0;
        int r = sx_frame_parse(frame, (size_t)flen, &osip, &odip, &osp, &odp,
                               &op, &olen);
        T_CHECK(r == SX_OK, "parse ok");
        T_CHECK(osip == sip && odip == dip, "ip roundtrip");
        T_CHECK(osp == sport && odp == dport, "port roundtrip");
        T_CHECK(olen == sizeof(payload), "payload length");
        T_CHECK(op != NULL && memcmp(op, payload, sizeof(payload)) == 0,
                "payload bytes");

        /* 篡改 IP 校验和字段。 */
        {
            uint8_t save = frame[24];
            frame[24] ^= 0xff;
            r = sx_frame_parse(frame, (size_t)flen, NULL, NULL, NULL, NULL,
                               NULL, NULL);
            T_CHECK(r != SX_OK, "corrupt ip csum rejected");
            frame[24] = save;
        }
        /* 篡改净荷（UDP 校验和应失败）。 */
        {
            uint8_t save = frame[14 + 20 + 8];
            frame[14 + 20 + 8] ^= 0xff;
            r = sx_frame_parse(frame, (size_t)flen, NULL, NULL, NULL, NULL,
                               NULL, NULL);
            T_CHECK(r != SX_OK, "corrupt udp payload rejected");
            frame[14 + 20 + 8] = save;
        }
    }

    /* 超长净荷拒绝。 */
    {
        uint8_t big[SX_MAX_PAYLOAD_LIMIT + 1];
        memset(big, 0, sizeof(big));
        flen = sx_frame_build(frame, 200, src_mac, dst_mac, sip, dip, sport,
                              dport, big, sizeof(big));
        T_CHECK(flen < 0, "oversize frame rejected");
    }

    T_END();
}
