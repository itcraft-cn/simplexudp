/*
 * test_recv.c - 拉取式接收与广播登记回归测试
 *
 * 覆盖：
 *   - sx_recv 在缓冲区不足时返回“完整消息长度”（调用方可感知截断）；
 *   - 空队列返回 SX_ERR_AGAIN；
 *   - 广播链路会登记到 mc_links/nmc（H1 回归）。
 */
#include "../src/sx_internal.h"

#include <string.h>

#include "sx_test.h"

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "test_recv.log";
    sx_config_t cfg;
    sx_endpoint_t *ep;
    uint8_t msg[100];
    uint8_t out[8];
    ssize_t n;
    sx_link_t *bc;
    int i;

    T_BEGIN(path);

    for (i = 0; i < (int)sizeof(msg); i++) {
        msg[i] = (uint8_t)i;
    }

    sx_config_init(&cfg);
    cfg.backend = SX_BACKEND_UDP;
    cfg.local_port = 0;
    cfg.on_recv = NULL; /* 拉取模式 */

    ep = sx_create(&cfg);
    T_CHECK(ep != NULL, "create endpoint");
    if (ep == NULL) {
        T_END();
    }

    /* 直接投递一条 100 字节消息。 */
    sx_ep_deliver(ep, NULL, 0x0a000001u, 1234, msg, sizeof(msg));

    memset(out, 0, sizeof(out));
    n = sx_recv(ep, out, sizeof(out), NULL, NULL);
    T_CHECK(n == (ssize_t)sizeof(msg), "sx_recv returns full length");
    T_CHECK(memcmp(out, msg, sizeof(out)) == 0, "prefix copied correctly");

    n = sx_recv(ep, out, sizeof(out), NULL, NULL);
    T_CHECK(n == SX_ERR_AGAIN, "empty queue returns AGAIN");

    /* H1 回归：广播链路须登记到 mc_links/nmc。 */
    bc = sx_add_broadcast(ep, 0xffffffffu, 12345);
    T_CHECK(bc != NULL, "add broadcast link");
    T_CHECK(ep->nmc > 0, "broadcast registered in mc_links");
    T_CHECK(ep->bcast_link == bc, "bcast_link set");

    sx_destroy(ep);
    T_END();
}
