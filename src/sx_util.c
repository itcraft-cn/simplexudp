/*
 * sx_util.c - 时间、日志等通用工具
 *
 * 注意事项：
 *   - 本文件禁止直接使用 stdout/stderr，日志一律通过端点的 on_log 回调。
 *   - sx_now_ms 使用 CLOCK_MONOTONIC，保证单调性好，适合 RTO/心跳计算。
 */
#include "sx_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

uint64_t sx_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void sx_log(sx_endpoint_t *ep, sx_log_level_t level, const char *fmt, ...) {
    char buf[512];
    va_list ap;

    if (ep == NULL || ep->cfg.on_log == NULL) {
        return;
    }
    if (level > ep->cfg.log_level) {
        return;
    }

    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    /* 序列化日志回调：可能被网络线程与应用线程并发触发。 */
    pthread_mutex_lock(&ep->log_lock);
    ep->cfg.on_log(level, buf, ep->cfg.user);
    pthread_mutex_unlock(&ep->log_lock);
}
