/*
 * test_throughput.c - 吞吐/长稳测试（内核 UDP 后端，跨主机可用）
 *
 * 用法：
 *   # 服务端（接收并统计，run_ms 到点或收到信号退出）
 *   ./test_throughput udp-server <bind_ip> <port> <run_ms>
 *
 *   # 客户端（突发发送 count 条，等待全部确认后统计吞吐）
 *   ./test_throughput udp-client <lip> <lport> <pip> <pport> <count> <msgsize> [timeout_ms]
 *
 * 说明：
 *   - 单条消息长度必须 <= 1400（默认上限）；
 *   - 客户端以“窗口满则 update”的方式尽快发送，末尾等待 snd_outstanding==0；
 *   - 输出含 MB/s、msg/s 与重传/超时/丢失等统计，便于在真实网络（含丢包注入）下评估。
 *
 * 结果写入 ./throughput_server.log / ./throughput_client.log（可用 SX_LOG 覆盖）。
 */
#include "simplexudp.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static FILE *g_log;
static volatile sig_atomic_t g_stop;
static volatile uint64_t g_rx_count;
static volatile uint64_t g_rx_bytes;

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

static void on_recv(sx_endpoint_t *ep, sx_link_t *link, const void *data,
                    size_t len, void *user) {
    (void)ep;
    (void)link;
    (void)data;
    (void)user;
    g_rx_count++;
    g_rx_bytes += len;
}

static uint32_t parse_ip(const char *s) {
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return 0;
    }
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d;
}

static void fill_common(sx_config_t *cfg, uint32_t ip, uint16_t port,
                        sx_recv_fn recv) {
    sx_config_init(cfg);
    cfg->backend = SX_BACKEND_UDP;
    cfg->conv = 0x7b0u;
    cfg->local_ip = ip;
    cfg->local_port = port;
    cfg->on_recv = recv;
    cfg->on_log = log_fn;
    cfg->log_level = SX_LOG_WARN;
    cfg->send_window = 4096;
    cfg->recv_window = 8192;
}

static int run_server(uint32_t ip, uint16_t port, uint32_t run_ms) {
    sx_config_t cfg;
    sx_endpoint_t *ep;
    uint64_t end = run_ms ? now_ns() + (uint64_t)run_ms * 1000000ull : 0;
    uint64_t t0 = now_ns();

    fill_common(&cfg, ip, port, on_recv);
    ep = sx_create(&cfg);
    if (ep == NULL || sx_start(ep) != SX_OK) {
        fprintf(g_log, "server create/start failed\n");
        fclose(g_log);
        return 1;
    }
    fprintf(g_log, "server up ip=%u.%u.%u.%u port=%u\n", (ip >> 24) & 0xff,
            (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff, port);

    while (!g_stop && (end == 0 || now_ns() < end)) {
        (void)sx_wait(ep, 100);
        (void)sx_update(ep, 0);
    }
    {
        double sec = (double)(now_ns() - t0) / 1e9;
        sx_stats_t st;
        (void)sx_get_stats(ep, &st);
        fprintf(g_log,
                "server rx_msgs=%llu rx_bytes=%llu sec=%.3f MBps=%.2f "
                "retrans=%llu dup=%llu reorder=%llu dropped=%llu\n",
                (unsigned long long)g_rx_count,
                (unsigned long long)g_rx_bytes, sec,
                sec > 0 ? (double)g_rx_bytes / 1e6 / sec : 0.0,
                (unsigned long long)st.retransmits,
                (unsigned long long)st.dup_packets,
                (unsigned long long)st.reorder_pkts,
                (unsigned long long)st.dropped);
    }
    sx_destroy(ep);
    fclose(g_log);
    return 0;
}

static int run_client(uint32_t lip, uint16_t lport, uint32_t pip,
                      uint16_t pport, uint64_t count, size_t msgsize,
                      uint32_t timeout_ms) {
    sx_config_t cfg;
    sx_endpoint_t *ep;
    sx_link_t *link;
    uint8_t *buf;
    uint64_t t0;
    uint64_t t1;
    uint64_t sent = 0;
    uint64_t start;

    fill_common(&cfg, lip, lport, NULL);
    ep = sx_create(&cfg);
    if (ep == NULL || sx_start(ep) != SX_OK) {
        fprintf(g_log, "client create/start failed\n");
        fclose(g_log);
        return 1;
    }
    if (msgsize > sx_max_payload(ep)) {
        fprintf(g_log, "msgsize %zu > max %zu\n", msgsize,
                sx_max_payload(ep));
        sx_destroy(ep);
        fclose(g_log);
        return 1;
    }
    link = sx_add_unicast(ep, pip, pport);
    if (link == NULL) {
        fprintf(g_log, "add_unicast failed\n");
        sx_destroy(ep);
        fclose(g_log);
        return 1;
    }
    buf = (uint8_t *)malloc(msgsize ? msgsize : 1);
    if (buf == NULL) {
        sx_destroy(ep);
        fclose(g_log);
        return 1;
    }
    memset(buf, 0x5a, msgsize);

    fprintf(g_log, "client -> %u.%u.%u.%u:%u count=%llu msgsize=%zu\n",
            (pip >> 24) & 0xff, (pip >> 16) & 0xff, (pip >> 8) & 0xff,
            pip & 0xff, pport, (unsigned long long)count, msgsize);

    start = now_ns();
    t0 = start;
    while (sent < count && !g_stop) {
        int64_t r = sx_send(ep, link, buf, msgsize);
        if (r >= 0) {
            sent++;
            if ((sent & 0xffu) == 0) {
                (void)sx_update(ep, 0);
            }
        } else {
            /* 窗口满：处理 ACK 打开窗口。 */
            (void)sx_update(ep, 0);
        }
    }
    /* 等待全部确认（或超时）。 */
    for (;;) {
        sx_stats_t st;
        (void)sx_get_stats(ep, &st);
        if (st.snd_outstanding == 0) {
            break;
        }
        if (timeout_ms > 0 &&
            (now_ns() - start) > (uint64_t)timeout_ms * 1000000ull) {
            break;
        }
        (void)sx_wait(ep, 1);
        (void)sx_update(ep, 0);
    }
    t1 = now_ns();

    {
        double sec = (double)(t1 - t0) / 1e9;
        uint64_t bytes = sent * (uint64_t)msgsize;
        sx_stats_t st;
        (void)sx_get_stats(ep, &st);
        fprintf(g_log,
                "client sent=%llu bytes=%llu sec=%.4f MBps=%.2f msgps=%.0f "
                "outstanding=%u retrans=%llu fast=%llu timeouts=%llu "
                "dropped=%llu rtt_ms=%u rto=%u\n",
                (unsigned long long)sent, (unsigned long long)bytes, sec,
                sec > 0 ? (double)bytes / 1e6 / sec : 0.0,
                sec > 0 ? (double)sent / sec : 0.0, st.snd_outstanding,
                (unsigned long long)st.retransmits,
                (unsigned long long)st.fast_retrans,
                (unsigned long long)st.timeouts,
                (unsigned long long)st.dropped, st.rtt_ms, st.rto_ms);
    }
    free(buf);
    sx_destroy(ep);
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
    g_log = fopen(logpath != NULL ? logpath : "throughput.log", "w");
    if (g_log == NULL) {
        return 2;
    }
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    if (strcmp(mode, "udp-server") == 0) {
        if (argc < 5) return 2;
        rc = run_server(parse_ip(argv[2]), (uint16_t)atoi(argv[3]),
                        (uint32_t)atoi(argv[4]));
    } else if (strcmp(mode, "udp-client") == 0) {
        if (argc < 8) return 2;
        rc = run_client(parse_ip(argv[2]), (uint16_t)atoi(argv[3]),
                        parse_ip(argv[4]), (uint16_t)atoi(argv[5]),
                        strtoull(argv[6], NULL, 10), (size_t)atoi(argv[7]),
                        argc > 8 ? (uint32_t)atoi(argv[8]) : 0);
    } else {
        rc = 2;
    }
    return rc;
}
