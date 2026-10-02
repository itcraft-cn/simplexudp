/*
 * test_relay.c - XDP(AF_XDP) 三跳接力环：101 -> 102 -> 103 -> 101，101 统计
 *
 * 用法：
 *   test_relay origin <if> <local_ip> <local_port> <next_ip> <next_port> \
 *              <count> <delay_ms> <timeout_ms>
 *   test_relay relay  <if> <local_ip> <local_port> <next_ip> <next_port> \
 *              <count> <delay_ms> <timeout_ms>
 *
 * 行为：
 *   - origin：delay 后向 next 发送 count 条带序号报文；随后等待回收（来自上一跳的
 *     回环），统计收到条数与序号是否连续。
 *   - relay ：收到报文后转发给 next；用发送队列解耦收/发，避免窗口满导致丢包。
 *
 * 三节点端口约定（各节点监听自己的 local_port，下一跳的目的端口见脚本）：
 *   101(listen 10100) -> 102:10200 ; 102(listen 10200) -> 103:10300 ;
 *   103(listen 10300) -> 101:10100
 *
 * 需要 root（AF_XDP）。日志写入文件（默认 test_relay.log，可用 SX_LOG 覆盖）。
 */
#include "simplexudp.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CONV 0x9e11u
#define QCAP 16384
#define MAX_PAY 1400

typedef struct relay_ctx {
    sx_endpoint_t *ep;
    sx_link_t *next;
    int is_origin;
    unsigned long expect;
    unsigned long sent;
    unsigned long recv;
    unsigned long fwd;
    unsigned long drop;
    unsigned long ooo;
    /* 发送队列（relay） */
    size_t qlen[QCAP];
    uint8_t *qdata[QCAP];
    unsigned long qh;
    unsigned long qt;
    FILE *log;
} relay_ctx_t;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void log_fn(sx_log_level_t level, const char *msg, void *user) {
    relay_ctx_t *c = (relay_ctx_t *)user;
    if (c != NULL && c->log != NULL) {
        fprintf(c->log, "[level=%d] %s\n", (int)level, msg);
        fflush(c->log);
    }
}

static void on_recv(sx_endpoint_t *ep, sx_link_t *link, const void *data,
                    size_t len, void *user) {
    relay_ctx_t *c = (relay_ctx_t *)user;
    (void)link;
    if (c->is_origin) {
        unsigned long seq = 0;
        if (len >= sizeof(unsigned long)) {
            memcpy(&seq, data, sizeof(seq));
            if (seq != c->recv) {
                c->ooo++;
            }
        }
        c->recv++;
        return;
    }
    /* relay：入发送队列（拷贝），由主循环转发。 */
    if ((c->qt + 1) % QCAP == c->qh) {
        c->drop++;
        return;
    }
    c->qdata[c->qt] = (uint8_t *)malloc(len > 0 ? len : 1);
    if (c->qdata[c->qt] == NULL) {
        c->drop++;
        return;
    }
    if (len > 0) {
        memcpy(c->qdata[c->qt], data, len);
    }
    c->qlen[c->qt] = len;
    c->qt = (c->qt + 1) % QCAP;
    (void)ep;
}

/* relay：尽量把队列中的报文发完。 */
static void flush_queue(relay_ctx_t *c) {
    while (c->qh != c->qt) {
        int64_t r = sx_send(c->ep, c->next, c->qdata[c->qh], c->qlen[c->qh]);
        if (r < 0) {
            break; /* 窗口满，稍后重试 */
        }
        free(c->qdata[c->qh]);
        c->qdata[c->qh] = NULL;
        c->qh = (c->qh + 1) % QCAP;
        c->fwd++;
    }
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
    const char *role;
    sx_config_t cfg;
    relay_ctx_t ctx;
    uint32_t lip, nip;
    uint16_t lport, nport;
    unsigned long count, delay_ms, timeout_ms;
    unsigned long i;
    uint64_t t0;

    memset(&ctx, 0, sizeof(ctx));
    if (argc < 10) {
        return 2;
    }
    role = argv[1];
    ctx.is_origin = (strcmp(role, "origin") == 0);
    lip = parse_ip(argv[3]);
    lport = (uint16_t)strtoul(argv[4], NULL, 10);
    nip = parse_ip(argv[5]);
    nport = (uint16_t)strtoul(argv[6], NULL, 10);
    count = strtoul(argv[7], NULL, 10);
    delay_ms = strtoul(argv[8], NULL, 10);
    timeout_ms = strtoul(argv[9], NULL, 10);
    ctx.expect = count;

    ctx.log = fopen(logpath != NULL ? logpath : "test_relay.log", "w");
    if (ctx.log == NULL) {
        return 2;
    }

    sx_config_init(&cfg);
    cfg.backend = SX_BACKEND_XDP;
    cfg.ifname = argv[2];
    cfg.local_ip = lip;
    cfg.local_port = lport;
    cfg.conv = CONV;
    cfg.max_payload = MAX_PAY;
    cfg.send_window = 4096;
    cfg.recv_window = 8192;
    cfg.on_recv = on_recv;
    cfg.user = &ctx;
    cfg.on_log = log_fn;
    cfg.log_level = SX_LOG_WARN;

    ctx.ep = sx_create(&cfg);
    if (ctx.ep == NULL || sx_start(ctx.ep) != SX_OK) {
        fprintf(ctx.log, "create/start failed\n");
        fclose(ctx.log);
        return 1;
    }
    ctx.next = sx_add_unicast(ctx.ep, nip, nport);
    if (ctx.next == NULL) {
        fprintf(ctx.log, "add next failed\n");
        sx_destroy(ctx.ep);
        fclose(ctx.log);
        return 1;
    }
    fprintf(ctx.log, "role=%s if=%s listen=%s:%u next=%s:%u count=%lu\n", role,
            argv[2], argv[3], lport, argv[5], nport, count);
    fflush(ctx.log);

    if (!ctx.is_origin) {
        /* relay：等到超时或（可选）没有更多数据。 */
        t0 = now_ms();
        while (now_ms() - t0 < timeout_ms) {
            (void)sx_wait(ctx.ep, 2);
            (void)sx_update(ctx.ep, 0);
            flush_queue(&ctx);
        }
        fprintf(ctx.log, "relay done fwd=%lu drop=%lu pending=%lu\n", ctx.fwd,
                ctx.drop, (ctx.qt + QCAP - ctx.qh) % QCAP);
        sx_destroy(ctx.ep);
        fclose(ctx.log);
        return 0;
    }

    /* origin：等待其它节点就绪，再发 count 条。 */
    for (i = 0; i < delay_ms; i += 100) {
        (void)sx_update(ctx.ep, 0);
        usleep(100000);
    }
    for (i = 0; i < count; i++) {
        unsigned char buf[16];
        unsigned long seq = i;
        int guard = 0;
        memset(buf, 0, sizeof(buf));
        memcpy(buf, &seq, sizeof(seq));
        memcpy(buf + sizeof(seq), "RELAY", 5);
        while (sx_send(ctx.ep, ctx.next, buf, sizeof(buf)) == SX_ERR_WINDOW &&
               guard++ < 100000) {
            (void)sx_update(ctx.ep, 0);
            usleep(50);
        }
        ctx.sent++;
        if ((i % 200) == 0) {
            (void)sx_update(ctx.ep, 0);
        }
    }
    fprintf(ctx.log, "origin sent=%lu, waiting for return...\n", ctx.sent);
    fflush(ctx.log);

    t0 = now_ms();
    while (ctx.recv < count && now_ms() - t0 < timeout_ms) {
        (void)sx_wait(ctx.ep, 2);
        (void)sx_update(ctx.ep, 0);
    }
    fprintf(ctx.log, "origin done sent=%lu recv=%lu ooo=%lu\n", ctx.sent,
            ctx.recv, ctx.ooo);
    {
        sx_stats_t st;
        (void)sx_get_stats(ctx.ep, &st);
        fprintf(ctx.log, "origin stats retrans=%llu fast=%llu timeouts=%llu\n",
                (unsigned long long)st.retransmits,
                (unsigned long long)st.fast_retrans,
                (unsigned long long)st.timeouts);
    }
    sx_destroy(ctx.ep);
    fclose(ctx.log);
    return (ctx.recv == count) ? 0 : 1;
}
