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
#include <stdlib.h>
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

/* ------------------------------------------------------------------ */
/* 定长块内存池                                                        */
/* ------------------------------------------------------------------ */

int sx_pool_init(sx_pool_t *p, uint32_t bsz, uint32_t nblocks) {
    uint32_t i;
    if (p == NULL || bsz == 0 || nblocks == 0) {
        return -1;
    }
    p->base = (uint8_t *)malloc((size_t)bsz * nblocks);
    p->free_idx = (uint32_t *)malloc((size_t)sizeof(uint32_t) * nblocks);
    if (p->base == NULL || p->free_idx == NULL) {
        free(p->base);
        free(p->free_idx);
        p->base = NULL;
        p->free_idx = NULL;
        p->bsz = 0;
        p->nblocks = 0;
        p->nfree = 0;
        return -1;
    }
    p->bsz = bsz;
    p->nblocks = nblocks;
    p->nfree = nblocks;
    for (i = 0; i < nblocks; i++) {
        p->free_idx[i] = i;
    }
    return 0;
}

void sx_pool_destroy(sx_pool_t *p) {
    if (p == NULL) {
        return;
    }
    free(p->base);
    free(p->free_idx);
    p->base = NULL;
    p->free_idx = NULL;
    p->bsz = 0;
    p->nblocks = 0;
    p->nfree = 0;
}

void *sx_pool_alloc(sx_pool_t *p, size_t len) {
    uint32_t idx;
    if (len == 0) {
        return NULL;
    }
    if (p != NULL && p->base != NULL && len <= p->bsz && p->nfree > 0) {
        idx = p->free_idx[--p->nfree];
        return p->base + (size_t)idx * p->bsz;
    }
    /* 池空或超大：回退系统分配，保证正确性。 */
    return malloc(len);
}

void sx_pool_free(sx_pool_t *p, void *ptr) {
    if (ptr == NULL) {
        return;
    }
    if (p != NULL && p->base != NULL) {
        uint8_t *b = (uint8_t *)ptr;
        size_t total = (size_t)p->bsz * p->nblocks;
        if (b >= p->base && b < p->base + total) {
            uint32_t idx = (uint32_t)((size_t)(b - p->base) / p->bsz);
            if (p->nfree < p->nblocks) {
                p->free_idx[p->nfree++] = idx;
            }
            return;
        }
    }
    free(ptr);
}
