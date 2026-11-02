/*
 * test_loopback.c - 端到端回环测试（内核 UDP 后端，无需 root）
 *
 * 覆盖：
 *   - sx_create/start/add_unicast/send/update 全链路；
 *   - 客户端 -> 服务端 按序可靠投递（自动学习对端）；
 *   - 服务端 -> 客户端 回程按序投递；
 *   - 超限报文被拒绝。
 */
#include "../src/sx_internal.h"

#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "sx_test.h"

#define NMSG 200
#define CONV 0x0badf00dU

typedef struct ctx {
    uint32_t expect;
    uint32_t got;
    uint32_t ooo;
    sx_link_t *link;
} ctx_t;

static void on_recv(sx_endpoint_t *ep, sx_link_t *link, const void *data,
                    size_t len, void *user) {
    ctx_t *c = (ctx_t *)user;
    uint32_t v = 0;
    (void)ep;
    if (link != NULL) {
        c->link = link;
    }
    if (len != sizeof(v)) {
        c->ooo++;
        return;
    }
    memcpy(&v, data, sizeof(v));
    if (v != c->expect) {
        c->ooo++;
    }
    c->expect = v + 1;
    c->got++;
}

static void pump(sx_endpoint_t *a, sx_endpoint_t *b, int rounds) {
    int i;
    for (i = 0; i < rounds; i++) {
        sx_update(a, 0);
        sx_update(b, 0);
        if (a->io != NULL && a->io->poll != NULL) {
            (void)a->io->poll(a->io, 1);
        }
        if (b->io != NULL && b->io->poll != NULL) {
            (void)b->io->poll(b->io, 1);
        }
        usleep(500);
    }
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "test_loopback.log";
    sx_config_t scfg;
    sx_config_t ccfg;
    sx_endpoint_t *srv;
    sx_endpoint_t *cli;
    sx_link_t *cl;
    ctx_t sctx;
    ctx_t cctx;
    uint16_t sport;
    uint32_t i;
    uint8_t static_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x7f};

    T_BEGIN(path);
    memset(&sctx, 0, sizeof(sctx));
    memset(&cctx, 0, sizeof(cctx));

    sx_config_init(&scfg);
    scfg.backend = SX_BACKEND_UDP;
    scfg.local_ip = 0x7f000001u; /* 127.0.0.1 */
    scfg.local_port = 0;
    scfg.conv = CONV;
    scfg.on_recv = on_recv;
    scfg.user = &sctx;
    scfg.log_level = SX_LOG_ERROR;

    ccfg = scfg;
    ccfg.user = &cctx;
    ccfg.cc_mode = SX_CC_OFF; /* 覆盖专线模式下的 API/统计路径 */
    ccfg.udp_rcvbuf = 65536;
    ccfg.udp_sndbuf = 65536;

    srv = sx_create(&scfg);
    T_CHECK(srv != NULL, "server create");
    if (srv == NULL) {
        T_END();
    }
    T_CHECK(sx_start(srv) == SX_OK, "server start");
    sport = srv->cfg.local_port;
    T_CHECK(sport != 0, "server bound port");

    cli = sx_create(&ccfg);
    T_CHECK(cli != NULL, "client create");
    if (cli == NULL) {
        T_END();
    }
    /* 静态 ARP 允许在 start 前登记；UDP 后端启动时忽略，但 API 必须成功。 */
    T_CHECK(sx_add_static_arp(cli, 0x7f000001u, static_mac) == SX_OK,
            "static arp before start");
    T_CHECK(sx_start(cli) == SX_OK, "client start");
    {
        sx_stats_t st;
        int rcv = 0;
        socklen_t rlen = sizeof(rcv);
        T_CHECK(sx_get_stats(cli, &st) == SX_OK, "client stats");
        T_CHECK(st.cc_mode == (uint32_t)SX_CC_OFF, "cc off in stats");
        T_CHECK(st.rto_backoff == 1u, "rto backoff default in stats");
        T_CHECK(st.backend == (uint32_t)SX_BACKEND_UDP, "udp backend chosen");
        T_CHECK(st.backend_fallback == 0, "no fallback for forced udp");
        T_CHECK(getsockopt(cli->io->fd, SOL_SOCKET, SO_RCVBUF, &rcv, &rlen) == 0 &&
                    rcv > 0,
                "udp rcvbuf applied");
        T_LOG("udp actual rcvbuf=%d", rcv);
    }

    cl = sx_add_unicast_mac(cli, 0x7f000001u, sport, static_mac);
    T_CHECK(cl != NULL, "client add unicast with static mac");
    T_CHECK(sx_add_static_arp(cli, 0x7f000001u, static_mac) == SX_OK,
            "static arp after start");

    /* 客户端发送；窗口满时驱动双方（服务端回 ACK 打开窗口）。 */
    for (i = 0; i < NMSG; i++) {
        int guard = 0;
        while (sx_send(cli, cl, &i, sizeof(i)) < 0 && guard++ < 10000) {
            pump(srv, cli, 1);
        }
    }

    /* 等待服务端收齐。 */
    for (i = 0; i < 4000 && sctx.got < NMSG; i++) {
        pump(srv, cli, 1);
    }
    T_LOG("c->s got=%u ooo=%u", sctx.got, sctx.ooo);
    T_CHECK(sctx.got == NMSG, "server received all");
    T_CHECK(sctx.ooo == 0, "server in-order");
    T_CHECK(sctx.link != NULL, "server learned peer link");

    /* 服务端回程发送（客户端期望序列从 100000 开始）。 */
    cctx.expect = 100000u;
    if (sctx.link != NULL) {
        for (i = 0; i < NMSG; i++) {
            uint32_t v = 100000u + i;
            int guard = 0;
            while (sx_send(srv, sctx.link, &v, sizeof(v)) < 0 &&
                   guard++ < 10000) {
                pump(srv, cli, 1);
            }
        }
        for (i = 0; i < 4000 && cctx.got < NMSG; i++) {
            pump(srv, cli, 1);
        }
    }
    T_LOG("s->c got=%u ooo=%u", cctx.got, cctx.ooo);
    T_CHECK(cctx.got == NMSG, "client received all replies");
    T_CHECK(cctx.ooo == 0, "client in-order");

    /* 超限报文拒绝（净荷 > 上限）。 */
    {
        uint8_t big[SX_MAX_PAYLOAD_LIMIT + 8];
        memset(big, 0, sizeof(big));
        T_CHECK(sx_send(cli, cl, big, sizeof(big)) == SX_ERR_TOO_LARGE,
                "oversize send rejected");
    }

    sx_destroy(cli);
    sx_destroy(srv);
    T_END();
}
