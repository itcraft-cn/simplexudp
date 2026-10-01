/*
 * sx_group.c - 组播演示（一个发送方，两个接收方）
 *
 * 演示内容：
 *   - 三个端点加入组播组 239.1.1.1:25000；
 *   - 发送方向组发送 N 条消息；
 *   - 每个接收方按序收到 N 条（顺序号校验），体现组播尽力可靠 + 有序。
 *
 * 用法：sx_group
 * 日志写入 ./sx_group.log（可用 SX_LOG 覆盖），不使用标准输出。
 *
 * 注意：组播依赖内核 UDP 后端的 IP_ADD_MEMBERSHIP 与回环（IP_MULTICAST_LOOP）。
 */
#include "simplexudp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NMSG 300
#define CONV 0x6770u
#define GROUP_IP 0xef010101u /* 239.1.1.1 */
#define GROUP_PORT 25000
#define NRCV 2

static FILE *g_log;

typedef struct ctx {
    uint32_t expect;
    uint32_t got;
    uint32_t ooo;
    uint32_t dup;
} ctx_t;

static void log_fn(sx_log_level_t level, const char *msg, void *user) {
    (void)user;
    if (g_log != NULL) {
        fprintf(g_log, "[level=%d] %s\n", (int)level, msg);
        fflush(g_log);
    }
}

static void rcv_cb(sx_endpoint_t *ep, sx_link_t *link, const void *data,
                   size_t len, void *user) {
    ctx_t *c = (ctx_t *)user;
    uint32_t v = 0;
    (void)ep;
    (void)link;
    if (len != sizeof(v)) {
        return;
    }
    memcpy(&v, data, sizeof(v));
    if (v == c->expect) {
        c->expect = v + 1;
        c->got++;
    } else if ((int32_t)(v - c->expect) > 0) {
        c->ooo++; /* 乱序/空洞（正常不应发生） */
        c->expect = v + 1;
        c->got++;
    } else {
        c->dup++; /* 重复包，忽略 */
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    sx_config_t cfg;
    sx_endpoint_t *tx;
    sx_endpoint_t *rx[NRCV];
    ctx_t rctx[NRCV];
    int i;
    uint32_t k;
    const char *logpath = getenv("SX_LOG");

    g_log = fopen(logpath != NULL ? logpath : "sx_group.log", "w");
    if (g_log == NULL) {
        return 1;
    }
    memset(rctx, 0, sizeof(rctx));

    sx_config_init(&cfg);
    cfg.backend = SX_BACKEND_UDP;
    cfg.local_ip = 0; /* 组播：由内核选择接口 */
    cfg.local_port = 0;
    cfg.conv = CONV;
    cfg.on_log = log_fn;
    cfg.log_level = SX_LOG_INFO;

    /* 发送方：不接收（不加入组），仅发送；这里用 broadcast 语义发送到组地址。
     * 若希望使用组播，可用 sx_add_multicast（会加入组，可能收到自身回环）。 */
    cfg.on_recv = NULL;
    tx = sx_create(&cfg);
    if (tx == NULL || sx_start(tx) != SX_OK) {
        fprintf(g_log, "tx create/start failed\n");
        fclose(g_log);
        return 1;
    }

    for (i = 0; i < NRCV; i++) {
        cfg.on_recv = rcv_cb;
        cfg.user = &rctx[i];
        cfg.local_port = GROUP_PORT; /* 组播成员绑定组端口 */
        rx[i] = sx_create(&cfg);
        if (rx[i] == NULL || sx_start(rx[i]) != SX_OK) {
            fprintf(g_log, "rx%d create/start failed\n", i);
            fclose(g_log);
            return 1;
        }
        if (sx_add_multicast(rx[i], GROUP_IP, GROUP_PORT) == NULL) {
            fprintf(g_log, "rx%d join multicast failed\n", i);
        }
    }

    /* 发送方用组播链路发送（加入组以便发送；自身回环消息忽略）。 */
    {
        sx_link_t *gl = sx_add_multicast(tx, GROUP_IP, GROUP_PORT);
        if (gl == NULL) {
            fprintf(g_log, "tx add group failed\n");
            fclose(g_log);
            return 1;
        }
        for (k = 0; k < NMSG; k++) {
            int guard = 0;
            while (sx_send(tx, gl, &k, sizeof(k)) < 0 && guard++ < 1000) {
                (void)sx_update(tx, 0);
            }
        }
    }

    /* 驱动接收方一段时间。 */
    for (i = 0; i < 20000; i++) {
        int j;
        (void)sx_update(tx, 0);
        for (j = 0; j < NRCV; j++) {
            (void)sx_update(rx[j], 0);
        }
        if (rctx[0].got >= NMSG && rctx[1].got >= NMSG) {
            break;
        }
        usleep(300);
    }

    for (i = 0; i < NRCV; i++) {
        fprintf(g_log, "rx%d got=%u ooo=%u dup=%u expect=%u\n", i,
                rctx[i].got, rctx[i].ooo, rctx[i].dup, rctx[i].expect);
    }
    fprintf(g_log, "result: %s\n",
            (rctx[0].got == NMSG && rctx[1].got == NMSG && rctx[0].ooo == 0 &&
             rctx[1].ooo == 0)
                ? "PASS"
                : "FAIL");

    for (i = 0; i < NRCV; i++) {
        sx_destroy(rx[i]);
    }
    sx_destroy(tx);
    fclose(g_log);
    return 0;
}
