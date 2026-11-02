/*
 * test_ts_status.c - 接收时间戳能力/状态探测
 *
 * 用法：
 *   test_ts_status <udp|xdp> [ifname] [local_ip] [port] [log]
 *
 * 环境变量 SX_RX_TS=hardware|auto 选择模式，默认 hardware。
 * 启动后输出 backend、rx_ts_mode、rx_ts_status、rx_ts_clock，以及库内探测日志。
 */
#include "simplexudp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *g_log;

static void log_fn(sx_log_level_t level, const char *msg, void *user) {
    (void)level;
    (void)user;
    if (g_log != NULL) {
        fprintf(g_log, "%s\n", msg);
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
    const char *logpath;
    const char *ts;
    sx_config_t cfg;
    sx_endpoint_t *ep;
    sx_stats_t st;

    if (argc < 2) {
        return 2;
    }
    logpath = argc > 5 ? argv[5] : "test_ts_status.log";
    g_log = fopen(logpath, "w");
    if (g_log == NULL) {
        return 2;
    }
    sx_config_init(&cfg);
    cfg.on_log = log_fn;
    cfg.log_level = SX_LOG_INFO;
    ts = getenv("SX_RX_TS");
    cfg.rx_ts_mode = (ts != NULL && strcmp(ts, "auto") == 0)
                         ? SX_RX_TS_AUTO
                         : SX_RX_TS_HARDWARE;
    if (strcmp(argv[1], "xdp") == 0) {
        cfg.backend = SX_BACKEND_XDP;
        cfg.ifname = argc > 2 ? argv[2] : NULL;
        cfg.local_ip = argc > 3 ? parse_ip(argv[3]) : 0;
        cfg.local_port = argc > 4 ? (uint16_t)atoi(argv[4]) : 49200;
    } else {
        cfg.backend = SX_BACKEND_UDP;
        cfg.local_ip = 0x7f000001u;
        cfg.local_port = 0;
    }
    ep = sx_create(&cfg);
    if (ep == NULL || sx_start(ep) != SX_OK) {
        fprintf(g_log, "create/start failed\n");
        fclose(g_log);
        return 1;
    }
    memset(&st, 0, sizeof(st));
    (void)sx_get_stats(ep, &st);
    fprintf(g_log,
            "backend=%u rx_ts_mode=%u rx_ts_status=%d rx_ts_clock=%u\n",
            st.backend, st.rx_ts_mode, (int)st.rx_ts_status,
            st.rx_ts_clock_id);
    sx_destroy(ep);
    fclose(g_log);
    return 0;
}
