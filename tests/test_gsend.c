/*
 * test_gsend.c - 组播/广播发送端（跨主机测试用）
 *
 * 用法：test_gsend <mcast|bcast> <dst_ip> <port> <count> [interval_us]
 * 行为：以 UDP 后端建立组播/广播链路，发送 count 个带序号的小报文。
 * 日志写入文件（默认 test_gsend.log，可用 SX_LOG 覆盖），不使用标准输出。
 */
#include "../src/sx_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CONV 0x6770u

static FILE *g_log;

static void log_fn(sx_log_level_t level, const char *msg, void *user) {
    (void)user;
    if (g_log != NULL) {
        fprintf(g_log, "[level=%d] %s\n", (int)level, msg);
        fflush(g_log);
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
    const char *mode;
    sx_config_t cfg;
    sx_endpoint_t *ep;
    sx_link_t *link;
    uint32_t dst_ip;
    uint16_t port;
    unsigned long count;
    unsigned long interval_us = 0;
    unsigned long i;

    g_log = fopen(logpath != NULL ? logpath : "test_gsend.log", "w");
    if (g_log == NULL) {
        return 2;
    }
    if (argc < 5) {
        fprintf(g_log, "usage: %s <mcast|bcast> <dst_ip> <port> <count> "
                       "[interval_us]\n", argv[0]);
        fclose(g_log);
        return 2;
    }
    mode = argv[1];
    dst_ip = parse_ip(argv[2]);
    port = (uint16_t)strtoul(argv[3], NULL, 10);
    count = strtoul(argv[4], NULL, 10);
    if (argc > 5) {
        interval_us = strtoul(argv[5], NULL, 10);
    }

    sx_config_init(&cfg);
    cfg.backend = SX_BACKEND_UDP;
    cfg.local_port = 0;
    cfg.conv = CONV;
    cfg.on_log = log_fn;
    cfg.log_level = SX_LOG_WARN;
    cfg.send_window = 1024;

    ep = sx_create(&cfg);
    if (ep == NULL || sx_start(ep) != SX_OK) {
        fprintf(g_log, "create/start failed\n");
        fclose(g_log);
        return 1;
    }
    link = (strcmp(mode, "bcast") == 0)
               ? sx_add_broadcast(ep, dst_ip, port)
               : sx_add_multicast(ep, dst_ip, port);
    if (link == NULL) {
        fprintf(g_log, "add link failed\n");
        sx_destroy(ep);
        fclose(g_log);
        return 1;
    }
    fprintf(g_log, "send mode=%s dst=%s:%u count=%lu\n", mode, argv[2], port,
            count);

    for (i = 0; i < count; i++) {
        unsigned char buf[32];
        unsigned long seq = i;
        int guard = 0;
        memset(buf, 0, sizeof(buf));
        memcpy(buf, &seq, sizeof(seq));
        memcpy(buf + sizeof(seq), "SX-GRP", 6);
        while (sx_send(ep, link, buf, sizeof(buf)) == SX_ERR_WINDOW &&
               guard++ < 1000) {
            sx_update(ep, 0);
        }
        if ((i % 200) == 0) {
            sx_update(ep, 0);
        }
        if (interval_us > 0) {
            usleep((useconds_t)interval_us);
        }
    }
    /* 给底层一点时间发出。 */
    for (i = 0; i < 50; i++) {
        sx_update(ep, 0);
        usleep(1000);
    }
    fprintf(g_log, "send done sent=%lu\n", count);
    sx_destroy(ep);
    fclose(g_log);
    return 0;
}
