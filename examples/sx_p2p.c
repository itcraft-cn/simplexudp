/*
 * sx_p2p.c - 点对点可靠传输示例（单进程回环演示）
 *
 * 演示内容：
 *   - 两个端点：服务端(24000) 与 客户端(24001)；
 *   - 客户端向服务端可靠发送 N 条消息，服务端按序回调并回显；
 *   - 客户端校验回显按序到达。
 *
 * 用法：
 *   sx_p2p [udp|xdp <ifname>]
 *   - 默认 udp 后端（无需 root）；
 *   - 指定 xdp <ifname> 时使用 AF_XDP（需要 root/网卡支持）。
 *
 * 日志写入文件（默认 ./sx_p2p.log，可用环境变量 SX_LOG 指定），不使用标准输出。
 */
#include "simplexudp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NMSG 500
#define CONV 0x5150u
#define SRV_PORT 24000
#define CLI_PORT 24001

static FILE *g_log;

typedef struct ctx {
    uint32_t expect;
    uint32_t got;
    uint32_t ooo;
    sx_link_t *link;
} ctx_t;

static void log_fn(sx_log_level_t level, const char *msg, void *user) {
    (void)user;
    if (g_log != NULL) {
        fprintf(g_log, "[level=%d] %s\n", (int)level, msg);
        fflush(g_log);
    }
}

static void srv_recv(sx_endpoint_t *ep, sx_link_t *link, const void *data,
                     size_t len, void *user) {
    ctx_t *c = (ctx_t *)user;
    uint32_t v = 0;
    (void)ep;
    c->link = link;
    if (len == sizeof(v)) {
        memcpy(&v, data, sizeof(v));
        if (v != c->expect) {
            c->ooo++;
        }
        c->expect = v + 1;
    }
    c->got++;
}

static void cli_recv(sx_endpoint_t *ep, sx_link_t *link, const void *data,
                     size_t len, void *user) {
    ctx_t *c = (ctx_t *)user;
    uint32_t v = 0;
    (void)ep;
    (void)link;
    if (len == sizeof(v)) {
        memcpy(&v, data, sizeof(v));
        if (v != c->expect) {
            c->ooo++;
        }
        c->expect = v + 1;
    }
    c->got++;
}

static void pump(sx_endpoint_t *a, sx_endpoint_t *b, int rounds) {
    int i;
    for (i = 0; i < rounds; i++) {
        (void)sx_update(a, 0);
        (void)sx_update(b, 0);
        usleep(500);
    }
}

int main(int argc, char **argv) {
    sx_config_t scfg;
    sx_config_t ccfg;
    sx_endpoint_t *srv;
    sx_endpoint_t *cli;
    sx_link_t *link;
    ctx_t sctx, cctx;
    uint32_t i;
    const char *logpath = getenv("SX_LOG");
    int use_xdp = 0;
    const char *ifname = NULL;

    if (argc >= 2 && strcmp(argv[1], "xdp") == 0) {
        use_xdp = 1;
        ifname = (argc >= 3) ? argv[2] : NULL;
    }
    g_log = fopen(logpath != NULL ? logpath : "sx_p2p.log", "w");
    if (g_log == NULL) {
        return 1;
    }

    memset(&sctx, 0, sizeof(sctx));
    memset(&cctx, 0, sizeof(cctx));

    sx_config_init(&scfg);
    scfg.conv = CONV;
    scfg.on_recv = srv_recv;
    scfg.on_log = log_fn;
    scfg.user = &sctx;
    scfg.log_level = SX_LOG_INFO;
    scfg.local_port = SRV_PORT;
    if (use_xdp) {
        scfg.backend = SX_BACKEND_XDP;
        scfg.ifname = ifname;
    } else {
        scfg.backend = SX_BACKEND_UDP;
        scfg.local_ip = 0x7f000001u;
    }

    ccfg = scfg;
    ccfg.on_recv = cli_recv;
    ccfg.user = &cctx;
    ccfg.local_port = CLI_PORT;

    srv = sx_create(&scfg);
    cli = sx_create(&ccfg);
    if (srv == NULL || cli == NULL) {
        fprintf(g_log, "create failed\n");
        fclose(g_log);
        return 1;
    }
    if (sx_start(srv) != SX_OK || sx_start(cli) != SX_OK) {
        fprintf(g_log, "start failed\n");
        fclose(g_log);
        return 1;
    }

    link = sx_add_unicast(cli, use_xdp ? 0u : 0x7f000001u, SRV_PORT);
    if (use_xdp) {
        fprintf(g_log, "xdp mode requires explicit peer IP; demo uses UDP loopback\n");
    }
    if (link == NULL) {
        fprintf(g_log, "add_unicast failed\n");
        fclose(g_log);
        return 1;
    }

    /* 第一阶段：客户端 -> 服务端。 */
    fprintf(g_log, "phase1: client -> server, %d messages...\n", NMSG);
    for (i = 0; i < NMSG; i++) {
        int guard = 0;
        while (sx_send(cli, link, &i, sizeof(i)) < 0 && guard++ < 20000) {
            pump(srv, cli, 1);
        }
    }
    for (i = 0; i < 8000 && sctx.got < NMSG; i++) {
        pump(srv, cli, 1);
    }

    /* 第二阶段：服务端 -> 客户端（使用自动学习的链路）。 */
    fprintf(g_log, "phase2: server -> client, %d messages...\n", NMSG);
    if (sctx.link != NULL) {
        for (i = 0; i < NMSG; i++) {
            int guard = 0;
            while (sx_send(srv, sctx.link, &i, sizeof(i)) < 0 &&
                   guard++ < 20000) {
                pump(srv, cli, 1);
            }
        }
        for (i = 0; i < 8000 && cctx.got < NMSG; i++) {
            pump(srv, cli, 1);
        }
    }

    fprintf(g_log, "server got=%u ooo=%u, client got=%u ooo=%u\n", sctx.got,
            sctx.ooo, cctx.got, cctx.ooo);
    fprintf(g_log, "result: %s\n",
            (cctx.got == NMSG && cctx.ooo == 0 && sctx.ooo == 0) ? "PASS"
                                                                 : "FAIL");

    sx_destroy(cli);
    sx_destroy(srv);
    fclose(g_log);
    return 0;
}
