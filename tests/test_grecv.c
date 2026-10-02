/*
 * test_grecv.c - 组播/广播接收端（跨主机测试用）
 *
 * 用法：test_grecv <mcast|bcast> <mcast_if_ip|0> <group_ip> <port> <count> <timeout_ms>
 *   mcast_if_ip 为组播入组/出口接口地址（多网卡必需），0 表示默认。
 * 行为：绑定端口并加入组播（或加入广播链路的组引擎），按序接收 count 个报文。
 *       收满退出 0，超时未满退出 1。
 * 日志写入文件（默认 test_grecv.log，可用 SX_LOG 覆盖），不使用标准输出。
 */
#include "simplexudp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CONV 0x6770u

static FILE *g_log;

typedef struct ctx {
    unsigned long got;
    unsigned long ooo;
    unsigned long expect;
} ctx_t;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void log_fn(sx_log_level_t level, const char *msg, void *user) {
    (void)user;
    if (g_log != NULL) {
        fprintf(g_log, "[level=%d] %s\n", (int)level, msg);
        fflush(g_log);
    }
}

static void on_recv(sx_endpoint_t *ep, sx_link_t *link, const void *data,
                    size_t len, void *user) {
    ctx_t *c = (ctx_t *)user;
    unsigned long seq = 0;
    (void)ep;
    (void)link;
    if (len >= sizeof(seq)) {
        memcpy(&seq, data, sizeof(seq));
        if (seq != c->expect) {
            c->ooo++;
        }
        c->expect = seq + 1;
    }
    c->got++;
}

static uint32_t parse_ip(const char *s) {
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return 0;
    }
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d;
}

int main(int argc, char **argv) {
    const char *logpath = getenv("SX_LOG");
    const char *mode;
    sx_config_t cfg;
    sx_endpoint_t *ep;
    sx_link_t *link;
    ctx_t ctx;
    uint32_t grp_ip;
    uint32_t mcast_if;
    uint16_t port;
    unsigned long expect;
    unsigned long timeout_ms;
    uint64_t t0;

    memset(&ctx, 0, sizeof(ctx));
    g_log = fopen(logpath != NULL ? logpath : "test_grecv.log", "w");
    if (g_log == NULL) {
        return 2;
    }
    if (argc < 7) {
        fprintf(g_log, "usage: %s <mcast|bcast> <mcast_if_ip|0> <group_ip> "
                       "<port> <count> <timeout_ms>\n", argv[0]);
        fclose(g_log);
        return 2;
    }
    mode = argv[1];
    mcast_if = parse_ip(argv[2]);
    grp_ip = parse_ip(argv[3]);
    port = (uint16_t)strtoul(argv[4], NULL, 10);
    expect = strtoul(argv[5], NULL, 10);
    timeout_ms = strtoul(argv[6], NULL, 10);

    sx_config_init(&cfg);
    cfg.backend = SX_BACKEND_UDP;
    cfg.local_ip = 0;
    cfg.multicast_if = mcast_if;
    cfg.local_port = port;
    cfg.conv = CONV;
    cfg.on_recv = on_recv;
    cfg.user = &ctx;
    cfg.on_log = log_fn;
    cfg.log_level = SX_LOG_WARN;
    cfg.recv_window = 4096;

    ep = sx_create(&cfg);
    if (ep == NULL || sx_start(ep) != SX_OK) {
        fprintf(g_log, "create/start failed\n");
        fclose(g_log);
        return 1;
    }
    link = (strcmp(mode, "bcast") == 0)
               ? sx_add_broadcast(ep, grp_ip, port)
               : sx_add_multicast(ep, grp_ip, port);
    if (link == NULL) {
        fprintf(g_log, "join/add link failed\n");
        sx_destroy(ep);
        fclose(g_log);
        return 1;
    }
    fprintf(g_log, "recv mode=%s if=%s group=%s:%u expect=%lu\n", mode, argv[2],
            argv[3], port, expect);

    t0 = now_ms();
    while (ctx.got < expect && now_ms() - t0 < timeout_ms) {
        (void)sx_wait(ep, 100);
        (void)sx_update(ep, 0);
    }
    fprintf(g_log, "recv done got=%lu expect=%lu ooo=%lu\n", ctx.got, expect,
            ctx.ooo);
    sx_destroy(ep);
    fclose(g_log);
    return (ctx.got == expect) ? 0 : 1;
}
