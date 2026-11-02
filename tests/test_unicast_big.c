/*
 * test_unicast_big.c - 单播大消息跨机测试
 *
 * 用法：
 *   test_unicast_big server <bind_ip> <port> <expect_size> <timeout_ms> [log]
 *   test_unicast_big client <dst_ip> <port> <size> <timeout_ms> [log]
 *
 * 依赖 UDP 后端，不需要 root；两端 max_message=size，验证 256KB 以内
 * 的单播消息自动分片、重组、长度与内容。
 */
#include "simplexudp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static FILE *g_log;

typedef struct ctx {
    unsigned long got;
    size_t len;
    int bad;
    size_t expect;
    sx_pkt_info_t info;
} ctx_t;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint32_t parse_ip(const char *s) {
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return 0;
    }
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d;
}

static void log_fn(sx_log_level_t level, const char *msg, void *user) {
    (void)level;
    (void)user;
    if (g_log != NULL) {
        fprintf(g_log, "%s\n", msg);
        fflush(g_log);
    }
}

static void handle_payload(ctx_t *c, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    size_t i;
    c->got++;
    c->len = len;
    if (len != c->expect) {
        c->bad = 1;
        return;
    }
    for (i = 0; i < len; i++) {
        if (p[i] != (unsigned char)(i * 31u + 7u)) {
            c->bad = 1;
            return;
        }
    }
}

static void on_recv(sx_endpoint_t *ep, sx_link_t *link, const void *data,
                    size_t len, void *user) {
    (void)ep;
    (void)link;
    handle_payload((ctx_t *)user, data, len);
}

static void on_recv_ex(sx_endpoint_t *ep, sx_link_t *link, const void *data,
                       size_t len, const sx_pkt_info_t *info, void *user) {
    ctx_t *c = (ctx_t *)user;
    (void)ep;
    (void)link;
    if (info != NULL) {
        c->info = *info;
    }
    handle_payload(c, data, len);
}

static void fill(unsigned char *buf, size_t len) {
    size_t i;
    for (i = 0; i < len; i++) {
        buf[i] = (unsigned char)(i * 31u + 7u);
    }
}

int main(int argc, char **argv) {
    const char *logpath;
    sx_config_t cfg;
    sx_endpoint_t *ep;
    uint32_t ip;
    uint16_t port;
    size_t size;
    unsigned long timeout_ms;
    uint64_t t0;
    int rc = 1;
    ctx_t ctx;

    if (argc < 6) {
        return 2;
    }
    logpath = argc > 6 ? argv[6] : "test_unicast_big.log";
    g_log = fopen(logpath, "w");
    if (g_log == NULL) {
        return 2;
    }
    ip = parse_ip(argv[2]);
    port = (uint16_t)atoi(argv[3]);
    size = (size_t)strtoul(argv[4], NULL, 10);
    timeout_ms = strtoul(argv[5], NULL, 10);

    memset(&ctx, 0, sizeof(ctx));
    ctx.expect = size;
    sx_config_init(&cfg);
    cfg.backend = SX_BACKEND_UDP;
    cfg.conv = 0x1b16u;
    cfg.max_payload = 1400;
    cfg.max_message = size;
    cfg.send_window = 64;   /* 应被自动提升 */
    cfg.recv_window = 64;
    cfg.local_ip = (strcmp(argv[1], "server") == 0) ? ip : 0;
    cfg.local_port = (strcmp(argv[1], "server") == 0) ? port : 0;
    cfg.on_log = log_fn;

    if (strcmp(argv[1], "server") == 0) {
        const char *ts = getenv("SX_RX_TS");
        cfg.on_recv = on_recv;
        if (ts != NULL) {
            cfg.rx_ts_mode = (strcmp(ts, "auto") == 0) ? SX_RX_TS_AUTO
                                                       : SX_RX_TS_HARDWARE;
            cfg.on_recv_ex = on_recv_ex;
        }
        cfg.user = &ctx;
    }

    ep = sx_create(&cfg);
    if (ep == NULL || sx_start(ep) != SX_OK) {
        fprintf(g_log, "create/start failed\n");
        fclose(g_log);
        return 1;
    }
    fprintf(g_log, "mode=%s ip=%s port=%u size=%zu max_payload=%zu "
                   "max_message=%zu\n",
            argv[1], argv[2], port, size, sx_max_payload(ep),
            sx_max_message(ep));
    t0 = now_ms();

    if (strcmp(argv[1], "server") == 0) {
        while (ctx.got == 0 && now_ms() - t0 < timeout_ms) {
            (void)sx_update(ep, 0);
            usleep(200);
        }
        if (ctx.got > 0) {
            fprintf(g_log,
                    "server got=%lu len=%zu bad=%d ts_mode=%u ts_status=%u "
                    "ts_valid=%u ts_clock=%u rx_ns=%llu\n",
                    ctx.got, ctx.len, ctx.bad, ctx.info.ts_mode,
                    ctx.info.ts_status, ctx.info.valid, ctx.info.clock_id,
                    (unsigned long long)ctx.info.rx_ns);
            rc = (ctx.got == 1 && ctx.len == size && ctx.bad == 0) ? 0 : 1;
        } else {
            fprintf(g_log, "server timeout\n");
        }
    } else {
        sx_link_t *link = sx_add_unicast(ep, ip, port);
        unsigned char *buf = (unsigned char *)malloc(size);
        int64_t seq;
        if (link == NULL || buf == NULL) {
            fprintf(g_log, "client setup failed\n");
        } else {
            fill(buf, size);
            seq = sx_send(ep, link, buf, size);
            fprintf(g_log, "client send seq=%lld\n", (long long)seq);
            if (seq >= 0) {
                while (now_ms() - t0 < timeout_ms) {
                    sx_stats_t st;
                    (void)sx_update(ep, 0);
                    (void)sx_get_stats(ep, &st);
                    if (st.snd_outstanding == 0) {
                        fprintf(g_log, "client acked outstanding=0\n");
                        rc = 0;
                        break;
                    }
                    usleep(200);
                }
            }
            free(buf);
        }
    }

    sx_destroy(ep);
    fclose(g_log);
    return rc;
}
