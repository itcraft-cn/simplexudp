/*
 * test_xdp.c - AF_XDP 端到端集成测试（需要 root 与 veth 环境）
 *
 * 用法：
 *   sudo ./test_xdp <if0> <if1> <ip0> <ip1> [port]
 * 例：
 *   sudo ./test_xdp sx0 sx1 10.10.0.1 10.10.0.2 30000
 *
 * 说明：
 *   - 两个端点分别在 if0/if1 上使用 AF_XDP（generic/native 自动侦测）；
 *   - 通过 ARP（本库用户态响应）解析对端 MAC；
 *   - 双向各发送 N 条消息，校验按序到达。
 *
 * 结果写入 ./test_xdp.log（可用 SX_LOG 覆盖）。
 */
#include "simplexudp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NMSG 1000
#define CONV 0x7d9u

static FILE *g_log;

typedef struct ctx {
    uint32_t expect;
    uint32_t got;
    uint32_t ooo;
} ctx_t;

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
    uint32_t v = 0;
    (void)ep;
    (void)link;
    if (len != sizeof(v)) {
        return;
    }
    memcpy(&v, data, sizeof(v));
    if (v != c->expect) {
        c->ooo++;
    }
    c->expect = v + 1;
    c->got++;
}

static uint32_t parse_ip(const char *s) {
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return 0;
    }
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d;
}

static void pump(sx_endpoint_t *a, sx_endpoint_t *b, int rounds) {
    int i;
    for (i = 0; i < rounds; i++) {
        (void)sx_update(a, 0);
        (void)sx_update(b, 0);
        usleep(300);
    }
}

int main(int argc, char **argv) {
    const char *if0;
    const char *if1;
    uint32_t ip0;
    uint32_t ip1;
    uint16_t port = 30000;
    sx_config_t cfg;
    sx_endpoint_t *ep0;
    sx_endpoint_t *ep1;
    sx_link_t *l0;
    sx_link_t *l1;
    ctx_t c1;
    ctx_t c0;
    uint32_t i;
    const char *logpath = getenv("SX_LOG");

    if (argc < 5) {
        return 2;
    }
    if0 = argv[1];
    if1 = argv[2];
    ip0 = parse_ip(argv[3]);
    ip1 = parse_ip(argv[4]);
    if (argc > 5) {
        port = (uint16_t)atoi(argv[5]);
    }
    g_log = fopen(logpath != NULL ? logpath : "test_xdp.log", "w");
    if (g_log == NULL) {
        return 2;
    }
    memset(&c0, 0, sizeof(c0));
    memset(&c1, 0, sizeof(c1));

    sx_config_init(&cfg);
    cfg.backend = SX_BACKEND_XDP;
    cfg.conv = CONV;
    cfg.on_log = log_fn;
    cfg.log_level = SX_LOG_INFO;
    cfg.local_port = port;
    cfg.max_payload = 1400;
    cfg.send_window = 256;
    cfg.recv_window = 1024;

    cfg.ifname = if0;
    cfg.local_ip = ip0;
    cfg.on_recv = on_recv;
    cfg.user = &c0;
    ep0 = sx_create(&cfg);
    cfg.ifname = if1;
    cfg.local_ip = ip1;
    cfg.user = &c1;
    ep1 = sx_create(&cfg);

    if (ep0 == NULL || ep1 == NULL) {
        fprintf(g_log, "create failed\n");
        fclose(g_log);
        return 1;
    }
    if (sx_start(ep0) != SX_OK || sx_start(ep1) != SX_OK) {
        fprintf(g_log, "start failed (need root / veth)\n");
        fclose(g_log);
        return 1;
    }

    l0 = sx_add_unicast(ep0, ip1, port);
    l1 = sx_add_unicast(ep1, ip0, port);
    if (l0 == NULL || l1 == NULL) {
        fprintf(g_log, "add_unicast failed\n");
        fclose(g_log);
        return 1;
    }

    /* 等待 ARP 解析。 */
    pump(ep0, ep1, 500);

    fprintf(g_log, "ep0 -> ep1 send %d\n", NMSG);
    for (i = 0; i < NMSG; i++) {
        int guard = 0;
        while (sx_send(ep0, l0, &i, sizeof(i)) < 0 && guard++ < 20000) {
            pump(ep0, ep1, 1);
        }
    }
    for (i = 0; i < 20000 && c1.got < NMSG; i++) {
        pump(ep0, ep1, 1);
    }
    fprintf(g_log, "ep1 got=%u ooo=%u\n", c1.got, c1.ooo);

    fprintf(g_log, "ep1 -> ep0 send %d\n", NMSG);
    for (i = 0; i < NMSG; i++) {
        int guard = 0;
        while (sx_send(ep1, l1, &i, sizeof(i)) < 0 && guard++ < 20000) {
            pump(ep0, ep1, 1);
        }
    }
    for (i = 0; i < 20000 && c0.got < NMSG; i++) {
        pump(ep0, ep1, 1);
    }
    fprintf(g_log, "ep0 got=%u ooo=%u\n", c0.got, c0.ooo);

    {
        sx_stats_t st;
        (void)sx_get_stats(ep0, &st);
        fprintf(g_log, "ep0 backend=%u xdp_mode=%u xdp_bind=%u rtt=%u rto=%u\n",
                st.backend, st.xdp_mode, st.xdp_bind, st.rtt_ms, st.rto_ms);
        (void)sx_get_stats(ep1, &st);
        fprintf(g_log, "ep1 backend=%u xdp_mode=%u xdp_bind=%u rtt=%u rto=%u\n",
                st.backend, st.xdp_mode, st.xdp_bind, st.rtt_ms, st.rto_ms);
    }
    fprintf(g_log, "result: %s\n",
            (c1.got == NMSG && c0.got == NMSG && c1.ooo == 0 && c0.ooo == 0)
                ? "PASS"
                : "FAIL");

    sx_destroy(ep0);
    sx_destroy(ep1);
    fclose(g_log);
    return 0;
}
