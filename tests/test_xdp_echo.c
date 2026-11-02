/*
 * test_xdp_echo.c - 纯 XDP_TX 反射的往返时延测试（内核 UDP 发送端）
 *
 * 用法：
 *   test_xdp_echo <dst_ip> <dst_port> <rounds> [warmup] [log]
 *
 * 说明：对端运行 tools/xdp_echo 挂载纯 XDP_TX 反射程序；本程序用普通 UDP
 *       socket 逐包发送并等待反射，统计 RTT。用于测量“无 KCP、无用户态反射”
 *       的物理下限。
 */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x < y) ? -1 : (x > y);
}

int main(int argc, char **argv) {
    const char *logpath;
    FILE *log;
    struct sockaddr_in dst;
    struct timeval tv;
    uint64_t *samples;
    uint64_t t0;
    uint64_t sum = 0;
    uint64_t mn = ~0ull;
    uint64_t mx = 0;
    uint32_t rounds;
    uint32_t warmup;
    uint32_t recorded = 0;
    uint32_t i;
    int fd;

    if (argc < 4) {
        return 2;
    }
    rounds = (uint32_t)strtoul(argv[3], NULL, 10);
    warmup = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 10) : 200;
    logpath = argc > 5 ? argv[5] : "test_xdp_echo.log";
    log = fopen(logpath, "w");
    if (log == NULL) {
        return 2;
    }
    samples = (uint64_t *)calloc(rounds + warmup, sizeof(uint64_t));
    if (samples == NULL) {
        fclose(log);
        return 1;
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(log, "socket failed\n");
        fclose(log);
        free(samples);
        return 1;
    }
    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)atoi(argv[2]));
    if (inet_pton(AF_INET, argv[1], &dst.sin_addr) != 1) {
        fprintf(log, "bad dst ip\n");
        close(fd);
        fclose(log);
        free(samples);
        return 2;
    }

    fprintf(log, "xdp_echo client dst=%s:%s rounds=%u warmup=%u\n", argv[1],
            argv[2], rounds, warmup);
    for (i = 0; i < rounds + warmup; i++) {
        uint64_t seq = i;
        ssize_t n;
        t0 = now_ns();
        if (sendto(fd, &seq, sizeof(seq), 0, (struct sockaddr *)&dst,
                   sizeof(dst)) != (ssize_t)sizeof(seq)) {
            fprintf(log, "round %u send failed\n", i);
            break;
        }
        n = recv(fd, &seq, sizeof(seq), 0);
        if (n != (ssize_t)sizeof(seq)) {
            fprintf(log, "round %u recv timeout/failed\n", i);
            break;
        }
        {
            uint64_t us = (now_ns() - t0) / 1000ull;
            if (i >= warmup) {
                samples[recorded++] = us;
                sum += us;
                if (us < mn) mn = us;
                if (us > mx) mx = us;
            }
        }
    }

    if (recorded > 0) {
        qsort(samples, recorded, sizeof(uint64_t), cmp_u64);
        fprintf(log,
                "rtt(us): min=%llu avg=%llu p50=%llu p99=%llu max=%llu n=%u\n",
                (unsigned long long)mn,
                (unsigned long long)(sum / recorded),
                (unsigned long long)samples[recorded * 50 / 100],
                (unsigned long long)samples[recorded * 99 / 100],
                (unsigned long long)mx, recorded);
    } else {
        fprintf(log, "no samples\n");
    }

    close(fd);
    free(samples);
    fclose(log);
    return recorded > 0 ? 0 : 1;
}
