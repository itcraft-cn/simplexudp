/*
 * test_arp.c - 静态 ARP 映射单元测试
 *
 * 验证：
 *   1) 静态映射可解析；
 *   2) 伪造 ARP 应答不能覆盖静态项；
 *   3) 静态项不参与老化；
 *   4) 非静态项仍可正常学习，并可通过 sx_arp_set 覆盖。
 */
#include "../src/sx_internal.h"

#include <string.h>

#include "sx_test.h"

#define ETH_HLEN 14
#define ARP_LEN 28

static void put_ip(uint8_t *p, uint32_t ip) {
    p[0] = (uint8_t)(ip >> 24);
    p[1] = (uint8_t)(ip >> 16);
    p[2] = (uint8_t)(ip >> 8);
    p[3] = (uint8_t)ip;
}

/* 构造一个 ARP 应答帧：spa -> sha，目标为本地 IP。 */
static size_t build_arp_reply(uint8_t *frame, uint32_t spa, const uint8_t sha[6],
                              uint32_t local_ip, const uint8_t local_mac[6]) {
    uint8_t *a = frame + ETH_HLEN;

    memset(frame, 0, ETH_HLEN + ARP_LEN);
    memcpy(frame, local_mac, 6);
    memcpy(frame + 6, sha, 6);
    frame[12] = 0x08;
    frame[13] = 0x06;

    a[0] = 0;
    a[1] = 1; /* htype */
    a[2] = 0x08;
    a[3] = 0x00; /* ptype */
    a[4] = 6;
    a[5] = 4;
    a[6] = 0;
    a[7] = 2; /* reply */
    memcpy(a + 8, sha, 6);
    put_ip(a + 14, spa);
    memcpy(a + 18, local_mac, 6);
    put_ip(a + 24, local_ip);
    return ETH_HLEN + ARP_LEN;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "test_arp.log";
    sx_io_t io;
    sx_arp_t *arp;
    uint8_t local_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    uint8_t static_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0xaa};
    uint8_t forged_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0xbb};
    uint8_t learned_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0xcc};
    uint8_t new_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0xdd};
    uint8_t out[6];
    uint8_t frame[ETH_HLEN + ARP_LEN];
    uint32_t local_ip = 0x0a000001u;
    uint32_t peer_ip = 0x0a000002u;
    uint32_t other_ip = 0x0a000003u;

    T_BEGIN(path);
    memset(&io, 0, sizeof(io));
    arp = sx_arp_create(&io);
    T_CHECK(arp != NULL, "arp create");
    if (arp == NULL) {
        T_END();
    }
    sx_arp_set_local(arp, local_ip, local_mac);

    /* 1) 静态映射立即解析。 */
    T_CHECK(sx_arp_set(arp, peer_ip, static_mac) == SX_OK, "set static arp");
    T_CHECK(sx_arp_resolve(arp, peer_ip, out) == SX_OK, "resolve static");
    T_CHECK(memcmp(out, static_mac, 6) == 0, "static mac matched");

    /* 2) 伪造应答（声称同一 IP 换了 MAC）不得覆盖静态项。 */
    (void)build_arp_reply(frame, peer_ip, forged_mac, local_ip, local_mac);
    T_CHECK(sx_arp_input(arp, frame, sizeof(frame)) == 1, "arp reply accepted");
    T_CHECK(sx_arp_resolve(arp, peer_ip, out) == SX_OK, "resolve after forged");
    T_CHECK(memcmp(out, static_mac, 6) == 0, "static entry not overwritten");

    /* 3) 静态项不老化（推进远超老化周期的时钟）。 */
    sx_arp_update(arp, sx_now_ms() + 1000000000ull);
    T_CHECK(sx_arp_resolve(arp, peer_ip, out) == SX_OK,
            "static entry survives aging");
    T_CHECK(memcmp(out, static_mac, 6) == 0, "static mac after aging");

    /* 4) 非静态项可学习，且允许显式覆盖。 */
    (void)build_arp_reply(frame, other_ip, learned_mac, local_ip, local_mac);
    T_CHECK(sx_arp_input(arp, frame, sizeof(frame)) == 1, "learn reply");
    T_CHECK(sx_arp_resolve(arp, other_ip, out) == SX_OK, "resolve learned");
    T_CHECK(memcmp(out, learned_mac, 6) == 0, "learned mac matched");

    T_CHECK(sx_arp_set(arp, other_ip, new_mac) == SX_OK, "explicit set");
    T_CHECK(sx_arp_resolve(arp, other_ip, out) == SX_OK, "resolve updated");
    T_CHECK(memcmp(out, new_mac, 6) == 0, "explicit set overwrites");

    sx_arp_set_local(arp, 0, NULL);
    sx_arp_destroy(arp);
    T_END();
}
