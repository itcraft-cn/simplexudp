/*
 * test_gsend.c - 组播/广播发送端（跨主机测试用）
 *
 * 用法：test_gsend <mcast|bcast> <mcast_if_ip|0> <dst_ip> <port> <count>
 *                  [interval_us] [payload_size] [linger_ms]
 *   mcast_if_ip 为组播出口接口地址（多网卡必需），0 表示默认。
 *   linger_ms 为发送后继续处理 NACK/重传的时间，大消息组播测试需 > 0。
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
    uint32_t mcast_if;
    unsigned long count;
    unsigned long interval_us = 0;
    unsigned long payload_size = 32;
    unsigned long linger_ms = 200;
    unsigned long i;
    unsigned char *buf;

    g_log = fopen(logpath != NULL ? logpath : "test_gsend.log", "w");
    if (g_log == NULL) {
        return 2;
    }
    if (argc < 6) {
        fprintf(g_log, "usage: %s <mcast|bcast> <mcast_if_ip|0> <dst_ip> "
                       "<port> <count> [interval_us] [payload_size] "
                       "[linger_ms]\n", argv[0]);
        fclose(g_log);
        return 2;
    }
    mode = argv[1];
    mcast_if = parse_ip(argv[2]);
    dst_ip = parse_ip(argv[3]);
    port = (uint16_t)strtoul(argv[4], NULL, 10);
    count = strtoul(argv[5], NULL, 10);
    if (argc > 6) {
        interval_us = strtoul(argv[6], NULL, 10);
    }
    if (argc > 7) {
        payload_size = strtoul(argv[7], NULL, 10);
    }
    if (argc > 8) {
        linger_ms = strtoul(argv[8], NULL, 10);
    }
    if (payload_size < 8 || payload_size > SX_MAX_MESSAGE_LIMIT) {
        fprintf(g_log, "invalid payload size %lu\n", payload_size);
        fclose(g_log);
        return 2;
    }

    sx_config_init(&cfg);
    cfg.backend = SX_BACKEND_UDP;
    cfg.local_port = 0;
    cfg.multicast_if = mcast_if;
    cfg.conv = CONV;
    cfg.on_log = log_fn;
    cfg.log_level = SX_LOG_WARN;
    cfg.send_window = 1024;
    cfg.max_message = 256u * 1024u;
    cfg.group_history = 2048;

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
    fprintf(g_log, "send mode=%s if=%s dst=%s:%u count=%lu size=%lu\n", mode,
            argv[2], argv[3], port, count, payload_size);
    buf = (unsigned char *)malloc(payload_size);
    if (buf == NULL) {
        sx_destroy(ep);
        fclose(g_log);
        return 1;
    }

    for (i = 0; i < count; i++) {
        unsigned long seq = i;
        size_t j;
        int guard = 0;
        memcpy(buf, &seq, sizeof(seq));
        for (j = sizeof(seq); j < payload_size; j++) {
            buf[j] = (unsigned char)(j * 17u + 3u);
        }
        while (sx_send(ep, link, buf, payload_size) == SX_ERR_WINDOW &&
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
    /* 发送后继续处理一段时间，使对端 NACK 能触发重传（大消息组播必需）。 */
    for (i = 0; i < linger_ms; i++) {
        sx_update(ep, 0);
        usleep(1000);
    }
    fprintf(g_log, "send done sent=%lu size=%lu linger=%lu\n", count,
            payload_size, linger_ms);
    free(buf);
    sx_destroy(ep);
    fclose(g_log);
    return 0;
}
