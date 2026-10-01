/*
 * test_latency.c - 端到端往返时延(RTT)测试
 *
 * 支持两种拓扑：
 *   1) 单进程：A/B 两端点在同一进程（忙等测量，量级最小）。
 *   2) 双进程：server 进程回显，client 进程测量（更接近真实，含进程调度）。
 *
 * 方法：停等(stop-and-wait)，逐条测量往返时间，统计 min/avg/p50/p99/max。
 *
 * 用法：
 *   # --- 单进程 ---
 *   sudo ./test_latency xdp <if0> <if1> <ip0> <ip1> <port> [rounds] [warmup] [hold_ms]
 *        ./test_latency udp <ip0> <ip1> <port0> <port1> [rounds] [warmup] [hold_ms]
 *
 *   # --- 双进程（server 先起） ---
 *   sudo ./test_latency xdp-server <if> <ip> <port> [run_ms]
 *        ./test_latency udp-server <ip> <port> [run_ms]
 *   sudo ./test_latency xdp-client <if> <lip> <lport> <pip> <pport> [rounds] [warmup] [hold_ms]
 *        ./test_latency udp-client <lip> <lport> <pip> <pport> [rounds] [warmup] [hold_ms]
 *
 * 结果写入 ./test_latency.log（可用 SX_LOG 覆盖），不使用标准输出。
 */
#include "simplexudp.h"
#include "../src/sx_internal.h"

#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_ROUNDS 100000
#define DEFAULT_WARMUP 200

static FILE *g_log;
static volatile sig_atomic_t g_stop;
static volatile int g_reply;
static uint64_t g_t0_ns;
static uint64_t g_rtt_ns;
static volatile uint64_t g_echo_count;
static volatile uint64_t g_echo_fail;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void on_sig(int s) {
    (void)s;
    g_stop = 1;
}

static void log_fn(sx_log_level_t level, const char *msg, void *user) {
    (void)user;
    if (g_log != NULL) {
        fprintf(g_log, "[level=%d] %s\n", (int)level, msg);
        fflush(g_log);
    }
}

/* 服务端：原样回显。 */
static void b_recv(sx_endpoint_t *ep, sx_link_t *link, const void *data,
                   size_t len, void *user) {
    (void)user;
    if (sx_send(ep, link, data, len) < 0) {
        if (g_echo_fail < 10) {
            fprintf(g_log, "echo send fail\n");
            fflush(g_log);
        }
        g_echo_fail++;
    }
    g_echo_count++;
}

/* 客户端：记录往返。 */
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

static void fill_common(sx_config_t *cfg, int use_xdp, const char *ifname,
                        uint32_t ip, uint16_t port, sx_recv_fn recv) {
    sx_config_init(cfg);
    cfg->conv = 0x1a7eu;
    cfg->on_log = log_fn;
    cfg->log_level = SX_LOG_INFO;
    cfg->on_recv = recv;
    cfg->local_ip = ip;
    cfg->local_port = port;
    if (use_xdp) {
        cfg->backend = SX_BACKEND_XDP;
        cfg->ifname = ifname;
    } else {
        cfg->backend = SX_BACKEND_UDP;
    }
}

/* 服务端：创建端点并持续回显，直到收到信号或 run_ms 到期。 */
static int run_server(int use_xdp, const char *ifname, uint32_t ip,
                      uint16_t port, uint32_t run_ms) {
    sx_config_t cfg;
    sx_endpoint_t *ep;
    uint64_t end = 0;
    uint64_t total = 0;

    fill_common(&cfg, use_xdp, ifname, ip, port, b_recv);
    ep = sx_create(&cfg);
    if (ep == NULL || sx_start(ep) != SX_OK) {
        fprintf(g_log, "server create/start failed\n");
        fclose(g_log);
        return 1;
    }
    fprintf(g_log, "server up ip=%s port=%u backend=%s\n",
            use_xdp ? (ifname ? ifname : "?") : "udp", port,
            use_xdp ? "xdp" : "udp");
    if (run_ms > 0) {
        end = now_ns() + (uint64_t)run_ms * 1000000ull;
    }
    while (!g_stop && (end == 0 || now_ns() < end)) {
        (void)sx_wait(ep, 100);
        (void)sx_update(ep, 0);
        total++;
    }
    fprintf(g_log, "server exit (loops=%llu echoed=%llu)\n",
            (unsigned long long)total, (unsigned long long)g_echo_count);
    sx_destroy(ep);
    fclose(g_log);
    return 0;
}

/* 客户端：发一条等一条，统计 RTT。 */
static int run_client(int use_xdp, const char *ifname, uint32_t lip,
                      uint16_t lport, uint32_t pip, uint16_t pport,
                      uint32_t rounds, uint32_t warmup, uint32_t hold_ms) {
    sx_config_t cfg;
    sx_endpoint_t *ep;
    sx_link_t *link;
    uint64_t *samples;
    uint32_t i;
    uint32_t recorded = 0;
    uint64_t sum = 0;
    uint64_t mn = ~0ull;
    uint64_t mx = 0;

    fill_common(&cfg, use_xdp, ifname, lip, lport, a_recv);
    ep = sx_create(&cfg);
    if (ep == NULL || sx_start(ep) != SX_OK) {
        fprintf(g_log, "client create/start failed\n");
        fclose(g_log);
        return 1;
    }
    link = sx_add_unicast(ep, pip, pport);
    if (link == NULL) {
        fprintf(g_log, "client add_unicast failed\n");
        fclose(g_log);
        return 1;
    }
    samples = (uint64_t *)calloc(rounds, sizeof(uint64_t));
    if (samples == NULL) {
        fclose(g_log);
        return 1;
    }

    fprintf(g_log, "client rounds=%u warmup=%u peer=%u.%u.%u.%u:%u\n", rounds,
            warmup, (pip >> 24) & 0xff, (pip >> 16) & 0xff, (pip >> 8) & 0xff,
            pip & 0xff, pport);

    for (i = 0; i < rounds + warmup && !g_stop; i++) {
        uint64_t seq = i;
        int guard;

        g_reply = 0;
        for (guard = 0; sx_send(ep, link, &seq, sizeof(seq)) < 0 &&
                       guard < 100000 && !g_stop;
             guard++) {
            (void)sx_wait(ep, 20);
            (void)sx_update(ep, 0);
        }
        g_t0_ns = now_ns(); /* 仅在真正发送成功后开始计往返 */
        for (guard = 0; guard < 100000000 && !g_reply && !g_stop; guard++) {
            (void)sx_wait(ep, 200);
            (void)sx_update(ep, 0);
        }
        if (!g_reply && !g_stop) {
            fprintf(g_log, "round %llu timeout\n", (unsigned long long)i);
            if (link->kcp != NULL) {
                fprintf(g_log,
                        "  kcp: una=%u nxt=%u cnt=%u rcv_nxt=%u rcv_cnt=%u "
                        "cwnd=%u ssthresh=%u snd_wnd=%u rto=%u srtt=%u\n",
                        link->kcp->snd_una, link->kcp->snd_nxt,
                        link->kcp->snd_count, link->kcp->rcv_nxt,
                        link->kcp->rcv_count, link->kcp->cwnd,
                        link->kcp->ssthresh, link->kcp->snd_wnd,
                        link->kcp->rto_ms, link->kcp->srtt);
            }
            break;
        }
        if (i < 40) {
            fprintf(g_log, "round %u rtt=%llu us\n", i,
                    (unsigned long long)(g_rtt_ns / 1000ull));
            fflush(g_log);
        }
        if (i >= warmup) {
            uint64_t us = g_rtt_ns / 1000ull;
            samples[recorded++] = us;
            sum += us;
            if (us < mn) mn = us;
            if (us > mx) mx = us;
            if (us > 2000) {
                fprintf(g_log, "slow round %u rtt=%llu us\n", i,
                        (unsigned long long)us);
            }
        }
    }

    if (recorded > 0) {
        qsort(samples, recorded, sizeof(uint64_t), cmp_u64);
        fprintf(g_log,
                "rtt(us): min=%llu avg=%llu p50=%llu p99=%llu max=%llu n=%u\n",
                (unsigned long long)mn, (unsigned long long)(sum / recorded),
                (unsigned long long)samples[recorded * 50 / 100],
                (unsigned long long)samples[recorded * 99 / 100],
                (unsigned long long)mx, recorded);
    } else {
        fprintf(g_log, "no samples\n");
    }
    {
        sx_stats_t st;
        (void)sx_get_stats(ep, &st);
        fprintf(g_log,
                "backend=%u xdp_mode=%u xdp_bind=%u kcp_rtt_ms=%u rto=%u "
                "snd_out=%u cwnd=%u sndwnd=%u\n",
                st.backend, st.xdp_mode, st.xdp_bind, st.rtt_ms, st.rto_ms,
                st.snd_outstanding, st.cwnd, st.snd_wnd);
    }

    if (hold_ms > 0) {
        uint64_t end = now_ns() + (uint64_t)hold_ms * 1000000ull;
        fprintf(g_log, "hold %u ms (xdp attached) ...\n", hold_ms);
        fflush(g_log);
        while (now_ns() < end) {
            (void)sx_update(ep, 0);
            usleep(200);
        }
        fprintf(g_log, "hold done\n");
    }

    free(samples);
    sx_destroy(ep);
    fclose(g_log);
    return 0;
}

/* 单进程模式：A 发 B 回，忙等测量。 */
static int run_single(int use_xdp, char **argv, int argc) {
    sx_config_t ca;
    sx_config_t cb;
    sx_endpoint_t *epa;
    sx_endpoint_t *epb;
    sx_link_t *la;
    sx_link_t *lb;
    uint64_t *samples;
    uint32_t rounds = DEFAULT_ROUNDS;
    uint32_t warmup = DEFAULT_WARMUP;
    uint32_t hold_ms = 0;
    uint32_t i;
    uint32_t recorded = 0;
    uint64_t sum = 0;
    uint64_t mn = ~0ull;
    uint64_t mx = 0;
    const char *if0 = NULL;
    const char *if1 = NULL;
    uint32_t ip0;
    uint32_t ip1;
    uint16_t port0;
    uint16_t port1;

    if (use_xdp) {
        if (argc < 7) return 2;
        if0 = argv[2];
        if1 = argv[3];
        ip0 = parse_ip(argv[4]);
        ip1 = parse_ip(argv[5]);
        port0 = (uint16_t)atoi(argv[6]);
        port1 = port0;
        if (argc > 7) rounds = (uint32_t)atoi(argv[7]);
        if (argc > 8) warmup = (uint32_t)atoi(argv[8]);
        if (argc > 9) hold_ms = (uint32_t)atoi(argv[9]);
    } else {
        if (argc < 6) return 2;
        ip0 = parse_ip(argv[2]);
        ip1 = parse_ip(argv[3]);
        port0 = (uint16_t)atoi(argv[4]);
        port1 = (uint16_t)atoi(argv[5]);
        if (argc > 6) rounds = (uint32_t)atoi(argv[6]);
        if (argc > 7) warmup = (uint32_t)atoi(argv[7]);
        if (argc > 8) hold_ms = (uint32_t)atoi(argv[8]);
    }
    if (rounds == 0) rounds = DEFAULT_ROUNDS;

    fill_common(&ca, use_xdp, if0, ip0, port0, a_recv);
    cb = ca;
    cb.local_ip = ip1;
    cb.local_port = port1;
    cb.ifname = if1;
    cb.on_recv = b_recv;

    epa = sx_create(&ca);
    epb = sx_create(&cb);
    if (epa == NULL || epb == NULL) {
        fprintf(g_log, "create failed\n");
        fclose(g_log);
        return 1;
    }
    if (sx_start(epa) != SX_OK || sx_start(epb) != SX_OK) {
        fprintf(g_log, "start failed\n");
        fclose(g_log);
        return 1;
    }
    la = sx_add_unicast(epa, ip1, port1);
    lb = sx_add_unicast(epb, ip0, port0);
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
    fprintf(g_log, "single-process mode=%s rounds=%u warmup=%u\n",
            use_xdp ? "xdp" : "udp", rounds, warmup);
    for (i = 0; i < rounds + warmup; i++) {
        uint64_t seq = i;
        int guard;
        g_reply = 0;
        g_t0_ns = now_ns();
        for (guard = 0; sx_send(epa, la, &seq, sizeof(seq)) < 0 && guard < 10000;
             guard++) {
            (void)sx_update(epb, 0);
        }
        for (guard = 0; guard < 20000000 && !g_reply; guard++) {
            (void)sx_update(epa, 0);
            (void)sx_update(epb, 0);
        }
        if (!g_reply) {
            fprintf(g_log, "round %llu timeout\n", (unsigned long long)i);
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
        qsort(samples, recorded, sizeof(uint64_t), cmp_u64);
        fprintf(g_log,
                "rtt(us): min=%llu avg=%llu p50=%llu p99=%llu max=%llu n=%u\n",
                (unsigned long long)mn, (unsigned long long)(sum / recorded),
                (unsigned long long)samples[recorded * 50 / 100],
                (unsigned long long)samples[recorded * 99 / 100],
                (unsigned long long)mx, recorded);
    }
    {
        sx_stats_t st;
        (void)sx_get_stats(epa, &st);
        fprintf(g_log, "backend=%u xdp_mode=%u xdp_bind=%u kcp_rtt_ms=%u rto=%u\n",
                st.backend, st.xdp_mode, st.xdp_bind, st.rtt_ms, st.rto_ms);
    }
    fprintf(g_log, "echoed=%llu\n", (unsigned long long)g_echo_count);
    if (hold_ms > 0) {
        uint64_t end = now_ns() + (uint64_t)hold_ms * 1000000ull;
        while (now_ns() < end) {
            (void)sx_update(epa, 0);
            (void)sx_update(epb, 0);
            usleep(200);
        }
    }
    free(samples);
    sx_destroy(epa);
    sx_destroy(epb);
    fclose(g_log);
    return 0;
}

int main(int argc, char **argv) {
    const char *mode;
    const char *logpath = getenv("SX_LOG");
    int rc;

    if (argc < 2) {
        return 2;
    }
    mode = argv[1];
    g_log = fopen(logpath != NULL ? logpath : "test_latency.log", "w");
    if (g_log == NULL) {
        return 2;
    }
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    if (strcmp(mode, "xdp-server") == 0) {
        /* xdp-server <if> <ip> <port> [run_ms] */
        if (argc < 5) return 2;
        rc = run_server(1, argv[2], parse_ip(argv[3]),
                        (uint16_t)atoi(argv[4]),
                        argc > 5 ? (uint32_t)atoi(argv[5]) : 0);
    } else if (strcmp(mode, "udp-server") == 0) {
        /* udp-server <ip> <port> [run_ms] */
        if (argc < 4) return 2;
        rc = run_server(0, NULL, parse_ip(argv[2]),
                        (uint16_t)atoi(argv[3]),
                        argc > 4 ? (uint32_t)atoi(argv[4]) : 0);
    } else if (strcmp(mode, "xdp-client") == 0) {
        /* xdp-client <if> <lip> <lport> <pip> <pport> [rounds] [warmup] [hold_ms] */
        if (argc < 7) return 2;
        rc = run_client(1, argv[2], parse_ip(argv[3]),
                        (uint16_t)atoi(argv[4]), parse_ip(argv[5]),
                        (uint16_t)atoi(argv[6]),
                        argc > 7 ? (uint32_t)atoi(argv[7]) : DEFAULT_ROUNDS,
                        argc > 8 ? (uint32_t)atoi(argv[8]) : DEFAULT_WARMUP,
                        argc > 9 ? (uint32_t)atoi(argv[9]) : 0);
    } else if (strcmp(mode, "udp-client") == 0) {
        /* udp-client <lip> <lport> <pip> <pport> [rounds] [warmup] [hold_ms] */
        if (argc < 6) return 2;
        rc = run_client(0, NULL, parse_ip(argv[2]),
                        (uint16_t)atoi(argv[3]), parse_ip(argv[4]),
                        (uint16_t)atoi(argv[5]),
                        argc > 6 ? (uint32_t)atoi(argv[6]) : DEFAULT_ROUNDS,
                        argc > 7 ? (uint32_t)atoi(argv[7]) : DEFAULT_WARMUP,
                        argc > 8 ? (uint32_t)atoi(argv[8]) : 0);
    } else {
        rc = run_single(strcmp(mode, "xdp") == 0, argv, argc);
    }
    return rc;
}
