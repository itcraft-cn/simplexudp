/*
 * sx_test.h - 极简测试框架
 *
 * 约定：
 *   - 测试结果写入文件（由 argv[1] 指定），不使用 stdout/stderr；
 *   - 进程退出码：0 全部通过，1 存在失败，2 初始化失败。
 */
#ifndef SX_TEST_H
#define SX_TEST_H

#include <stdio.h>
#include <stdlib.h>

static FILE *g_report;
static int g_checks;
static int g_fail;

#define T_BEGIN(path)                                    \
    do {                                                 \
        g_report = fopen((path), "w");                   \
        if (g_report == NULL) {                          \
            return 2;                                    \
        }                                                \
    } while (0)

#define T_END()                                                          \
    do {                                                                 \
        fprintf(g_report, "checks=%d fail=%d\n", g_checks, g_fail);      \
        fclose(g_report);                                                \
        return g_fail ? 1 : 0;                                           \
    } while (0)

#define T_CHECK(cond, msg)                                                \
    do {                                                                  \
        g_checks++;                                                       \
        if (!(cond)) {                                                    \
            g_fail++;                                                     \
            fprintf(g_report, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                    (msg));                                               \
        }                                                                 \
    } while (0)

#define T_LOG(...)                             \
    do {                                       \
        fprintf(g_report, __VA_ARGS__);        \
        fputc('\n', g_report);                 \
    } while (0)

#endif /* SX_TEST_H */
