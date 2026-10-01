/*
 * test_latency.c - 端到端往返时延(RTT)测试
 *
 * 方法：A 发一条消息，B 在回调中原样回显，A 测量往返时间；采用停等(stop-and-wait)
 *       方式，逐条测量并统计 min/avg/max/p50/p99，用于粗略评估时延。
 *
 * 用法：
 *   # AF_XDP（veth 两接口，同一端口）
 *   sudo ./test_latency xdp <if0> <if1> <ip0> <ip1> <port> [rounds] [warmup]
 *   # 内核 UDP（回环两端口）
 *   ./test_latency udp <ip0> <ip1> <port0> <port1> [rounds] [warmup]
 *
 * 结果写入 ./test_latency.log（可用 SX_LOG 覆盖），不使用标准输出。
 */
#include "simplexudp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_ROUNDS 100000
#define DEFAULT_WARMUP 200

static FILE *g_log;
static volatile int g_reply;
static uint64_t g_t0_ns;
static uint64_t g_rtt_ns;
static sx_endpoint_t *g_epb;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void log_fn(sx_log_level_t level, const char *msg, void *user) {
    (void)user;
    if (g_log != NULL) {
        fprintf(g_log, "[level=%d] %s\n", (int)level, msg);
        fflush(g_log);
    }
}

static void b_recv(sx_endpoint_t *ep, sx_link_t *link, const void *data,
                   size_t len, void *user) {
    (void)user;
    (void)link;
    /* 原样回显。 */
    (void)sx_send(ep, link, data, len);
}

static void a_recv(sx_endpoint_t *ep, sx_link_t *link, const void *data,
                   size_t len, void *user) {
    (void)ep;
    (void)link;
    (void)data;
    (void)user;
    if (len == sizeof(uint64_t) && g_reply == 0) {
        g_rtt_ns = now_ns() - g_t0_ns;
        g_reply = 1;
    }
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x < y) ? -1 : (x > y);
}

static uint32_t parse_ip(const char *s) {
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return 0;
    }
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d;
}

int main(int argc, char **argv) {
    const char *mode;
    sx_config_t ca;
    sx_config_t cb;
    sx_endpoint_t *epa;
    sx_link_t *la;
    sx_link_t *lb;
    uint64_t *samples;
    uint32_t rounds = DEFAULT_ROUNDS;
    uint32_t warmup = DEFAULT_WARMUP;
    uint32_t hold_ms = 0; /* 测试完成后保持挂载的时间，便于 bpftool 观察 */
    uint32_t i;
    uint32_t recorded;
    uint64_t sum = 0;
    uint64_t mn = ~0ull;
    uint64_t mx = 0;
    const char *logpath = getenv("SX_LOG");
    int use_xdp;

    if (argc < 6) {
        return 2;
    }
    mode = argv[1];
    use_xdp = (strcmp(mode, "xdp") == 0);

    g_log = fopen(logpath != NULL ? logpath : "test_latency.log", "w");
    if (g_log == NULL) {
        return 2;
    }

    sx_config_init(&ca);
    ca.conv = 0x1a7eu;
    ca.on_log = log_fn;
    ca.log_level = SX_LOG_INFO;
    ca.on_recv = a_recv;

    if (use_xdp) {
        if (argc < 7) {
            return 2;
        }
        ca.backend = SX_BACKEND_XDP;
        ca.ifname = argv[2];
        ca.local_ip = parse_ip(argv[4]);
        ca.local_port = (uint16_t)atoi(argv[6]);
        cb = ca;
        cb.ifname = argv[3];
        cb.local_ip = parse_ip(argv[5]);
        cb.on_recv = b_recv;
        if (argc > 7) rounds = (uint32_t)atoi(argv[7]);
        if (argc > 8) warmup = (uint32_t)atoi(argv[8]);
        if (argc > 9) hold_ms = (uint32_t)atoi(argv[9]);
    } else {
        ca.backend = SX_BACKEND_UDP;
        ca.local_ip = parse_ip(argv[2]);
        ca.local_port = (uint16_t)atoi(argv[4]);
        cb = ca;
        cb.local_ip = parse_ip(argv[3]);
        cb.local_port = (uint16_t)atoi(argv[5]);
        cb.on_recv = b_recv;
        if (argc > 6) rounds = (uint32_t)atoi(argv[6]);
        if (argc > 7) warmup = (uint32_t)atoi(argv[7]);
        if (argc > 8) hold_ms = (uint32_t)atoi(argv[8]);
    }
    if (rounds == 0) {
        rounds = DEFAULT_ROUNDS;
    }

    epa = sx_create(&ca);
    g_epb = sx_create(&cb);
    if (epa == NULL || g_epb == NULL) {
        fprintf(g_log, "create failed\n");
        fclose(g_log);
        return 1;
    }
    if (sx_start(epa) != SX_OK || sx_start(g_epb) != SX_OK) {
        fprintf(g_log, "start failed\n");
        fclose(g_log);
        return 1;
    }

    if (use_xdp) {
        la = sx_add_unicast(epa, cb.local_ip, cb.local_port);
        lb = sx_add_unicast(g_epb, ca.local_ip, ca.local_port);
    } else {
        la = sx_add_unicast(epa, cb.local_ip, cb.local_port);
        lb = sx_add_unicast(g_epb, ca.local_ip, ca.local_port);
    }
    if (la == NULL || lb == NULL) {
        fprintf(g_log, "add_unicast failed\n");
        fclose(g_log);
        return 1;
    }

    samples = (uint64_t *)calloc(rounds, sizeof(uint64_t));
    if (samples == NULL) {
        fclose(g_log);
        return 1;
    }

    fprintf(g_log, "mode=%s rounds=%u warmup=%u\n", mode, rounds, warmup);
    recorded = 0;
    for (i = 0; i < rounds + warmup; i++) {
        uint64_t seq = i;
        int guard;

        g_reply = 0;
        g_t0_ns = now_ns();
        for (guard = 0; sx_send(epa, la, &seq, sizeof(seq)) < 0 && guard < 10000;
             guard++) {
            (void)sx_update(g_epb, 0);
        }
        /* 忙等回显。 */
        for (guard = 0; guard < 20000000 && !g_reply; guard++) {
            (void)sx_update(epa, 0);
            (void)sx_update(g_epb, 0);
        }
        if (!g_reply) {
            fprintf(g_log, "round %llu timeout\n",
                    (unsigned long long)i);
            break;
        }
        if (i >= warmup) {
            uint64_t us = g_rtt_ns / 1000ull;
            samples[recorded++] = us;
            sum += us;
            if (us < mn) mn = us;
            if (us > mx) mx = us;
        }
    }

    if (recorded > 0) {
        uint64_t avg = sum / recorded;
        uint64_t p50;
        uint64_t p99;
        qsort(samples, recorded, sizeof(uint64_t), cmp_u64);
        p50 = samples[recorded * 50 / 100];
        p99 = samples[recorded * 99 / 100];
        fprintf(g_log,
                "rtt(us): min=%llu avg=%llu p50=%llu p99=%llu max=%llu n=%u\n",
                (unsigned long long)mn, (unsigned long long)avg,
                (unsigned long long)p50, (unsigned long long)p99,
                (unsigned long long)mx, recorded);
    } else {
        fprintf(g_log, "no samples\n");
    }

    {
        sx_stats_t st;
        (void)sx_get_stats(epa, &st);
        fprintf(g_log, "backend=%u xdp_mode=%u xdp_bind=%u kcp_rtt_ms=%u rto=%u\n",
                st.backend, st.xdp_mode, st.xdp_bind, st.rtt_ms, st.rto_ms);
    }

    /* 保持挂载一段时间，便于外部（bpftool）观察 XDP 程序与 map 计数。 */
    if (hold_ms > 0) {
        uint64_t end = now_ns() + (uint64_t)hold_ms * 1000000ull;
        fprintf(g_log, "hold %u ms (xdp attached) ...\n", hold_ms);
        fflush(g_log);
        while (now_ns() < end) {
            (void)sx_update(epa, 0);
            (void)sx_update(g_epb, 0);
            usleep(200);
        }
        fprintf(g_log, "hold done\n");
    }

    free(samples);
    sx_destroy(epa);
    sx_destroy(g_epb);
    fclose(g_log);
    return 0;
}
