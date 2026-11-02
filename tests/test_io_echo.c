/*
 * test_io_echo.c - 统一 IO 抽象下的 RTT 微基准
 *
 * 用法：
 *   test_io_echo <udp|xdp|auto> <ifname> <local_ip> <local_port>
 *                <dst_ip> <dst_port> <rounds> [warmup] [log]
 *
 * 说明：
 *   - 上层使用同一套 RTT 循环；
 *   - 下层通过 sx_io 抽象选择内核 UDP 或 AF_XDP；
 *   - auto 模式在 root + 提供网卡时优先 AF_XDP，失败回退 UDP；
 *   - 对端运行纯 XDP_TX 反射程序（tools/xdp_echo），不进入用户态。
 *
 * 与 test_xdp_echo 的区别：发送端也走 AF_XDP，剥离内核 UDP syscall，
 * 用于测量更接近物理层的往返下限。
 */
#include "../src/sx_internal.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static FILE *g_log;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t now_ms(void) {
    return now_ns() / 1000000ull;
}

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

/* 解析 SX_STATIC_ARP=IP@MAC。 */
static int parse_static_arp(const char *s, uint32_t *ip, uint8_t mac[6]) {
    const char *at;
    char ipbuf[32];
    unsigned m[6];
    size_t n;

    if (s == NULL) {
        return -1;
    }
    at = strchr(s, '@');
    if (at == NULL) {
        return -1;
    }
    n = (size_t)(at - s);
    if (n == 0 || n >= sizeof(ipbuf)) {
        return -1;
    }
    memcpy(ipbuf, s, n);
    ipbuf[n] = '\0';
    *ip = parse_ip(ipbuf);
    if (*ip == 0) {
        return -1;
    }
    if (sscanf(at + 1, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3],
               &m[4], &m[5]) != 6) {
        return -1;
    }
    for (n = 0; n < 6; n++) {
        if (m[n] > 0xff) {
            return -1;
        }
        mac[n] = (uint8_t)m[n];
    }
    return 0;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x < y) ? -1 : (x > y);
}

/* 驱动一次收包；返回 1=收到目标 payload，0=无数据/其他帧，<0=错误。 */
static int recv_once(sx_io_t *io, void *buf, size_t cap, size_t *len) {
    uint32_t sip = 0;
    uint16_t sport = 0;
    return io->recvfrom(io, &sip, &sport, buf, cap, len);
}


static int do_mirror(int use_xdp, const char *ifname, uint32_t ip,
                     uint16_t port, uint32_t count, const char *logpath) {
    sx_endpoint_t ep;
    sx_io_t *io;
    uint32_t got = 0;
    unsigned char buf[2048];

    g_log = fopen(logpath, "w");
    if (g_log == NULL) {
        return 2;
    }
    memset(&ep, 0, sizeof(ep));
    ep.cfg.backend = use_xdp ? SX_BACKEND_XDP : SX_BACKEND_UDP;
    ep.cfg.ifname = ifname;
    ep.cfg.local_ip = ip;
    ep.cfg.local_port = port;
    ep.cfg.max_payload = 1400;
    ep.cfg.on_log = log_fn;
    {
        const char *q = getenv("SX_XDP_QUEUE");
        const char *b = getenv("SX_XDP_BIND");
        if (q != NULL) {
            ep.cfg.xdp_queue = atoi(q);
        }
        if (b != NULL) {
            if (strcmp(b, "zerocopy") == 0 || strcmp(b, "zc") == 0) {
                ep.cfg.xdp_bind = SX_XDP_BIND_ZEROCOPY;
            } else if (strcmp(b, "copy") == 0) {
                ep.cfg.xdp_bind = SX_XDP_BIND_COPY;
            }
        }
    }
    io = use_xdp ? sx_io_xdp_create(&ep) : sx_io_udp_create(&ep);
    if (io == NULL) {
        fprintf(g_log, "mirror backend create failed\n");
        fclose(g_log);
        return 1;
    }
    fprintf(g_log, "mirror io=%s local=%s:%u count=%u xdp_bind=%u\n",
            io->name, ifname != NULL ? ifname : "-", ep.cfg.local_port, count,
            ep.stats.xdp_bind);

    while (got < count) {
        uint32_t sip = 0;
        uint16_t sport = 0;
        size_t len = 0;
        int r = io->recvfrom(io, &sip, &sport, buf, sizeof(buf), &len);
        if (r > 0) {
            (void)io->sendto(io, sip, sport, buf, len);
            got++;
            if (got % 1000 == 0) {
                fprintf(g_log, "mirror %u/%u\n", got, count);
            }
        }
        if (io->update != NULL) {
            io->update(io, now_ms());
        }
    }
    fprintf(g_log, "mirror done=%u\n", got);
    if (io->close != NULL) {
        io->close(io);
    }
    fclose(g_log);
    return got == count ? 0 : 1;
}

int main(int argc, char **argv) {
    const char *mode;
    const char *ifname;
    const char *logpath;
    sx_endpoint_t ep;
    sx_io_t *io = NULL;
    uint32_t lip;
    uint32_t dip;
    uint16_t lport;
    uint16_t dport;
    uint32_t rounds;
    uint32_t warmup;
    uint32_t recorded = 0;
    uint64_t *samples;
    uint64_t sum = 0;
    uint64_t mn = ~0ull;
    uint64_t mx = 0;
    uint32_t i;
    uint8_t buf[256];

    if (argc < 2) {
        return 2;
    }
    if (strcmp(argv[1], "mirror") == 0) {
        int use_xdp;
        if (argc < 7) {
            return 2;
        }
        use_xdp = (strcmp(argv[2], "xdp") == 0);
        return do_mirror(use_xdp, argv[3], parse_ip(argv[4]),
                         (uint16_t)atoi(argv[5]),
                         (uint32_t)strtoul(argv[6], NULL, 10),
                         argc > 7 ? argv[7] : "test_io_echo_mirror.log");
    }
    if (argc < 8) {
        return 2;
    }
    mode = argv[1];
    ifname = argv[2];
    lip = parse_ip(argv[3]);
    lport = (uint16_t)atoi(argv[4]);
    dip = parse_ip(argv[5]);
    dport = (uint16_t)atoi(argv[6]);
    rounds = (uint32_t)strtoul(argv[7], NULL, 10);
    warmup = argc > 8 ? (uint32_t)strtoul(argv[8], NULL, 10) : 200;
    logpath = argc > 9 ? argv[9] : "test_io_echo.log";
    g_log = fopen(logpath, "w");
    if (g_log == NULL) {
        return 2;
    }
    samples = (uint64_t *)calloc(rounds + warmup, sizeof(uint64_t));
    if (samples == NULL) {
        fclose(g_log);
        return 1;
    }

    memset(&ep, 0, sizeof(ep));
    ep.cfg.backend = SX_BACKEND_XDP;
    ep.cfg.ifname = ifname;
    ep.cfg.local_ip = lip;
    ep.cfg.local_port = lport;
    ep.cfg.max_payload = 1400;
    ep.cfg.on_log = log_fn;
    {
        const char *q = getenv("SX_XDP_QUEUE");
        const char *b = getenv("SX_XDP_BIND");
        if (q != NULL) {
            ep.cfg.xdp_queue = atoi(q);
        }
        if (b != NULL) {
            if (strcmp(b, "zerocopy") == 0 || strcmp(b, "zc") == 0) {
                ep.cfg.xdp_bind = SX_XDP_BIND_ZEROCOPY;
            } else if (strcmp(b, "copy") == 0) {
                ep.cfg.xdp_bind = SX_XDP_BIND_COPY;
            } else {
                ep.cfg.xdp_bind = SX_XDP_BIND_AUTO;
            }
        }
    }

    if (strcmp(mode, "xdp") == 0 || strcmp(mode, "auto") == 0) {
        if (geteuid() == 0 && ifname != NULL && ifname[0] != '\0') {
            io = sx_io_xdp_create(&ep);
        }
        if (io == NULL && strcmp(mode, "xdp") == 0) {
            fprintf(g_log, "xdp backend create failed\n");
            fclose(g_log);
            free(samples);
            return 1;
        }
    }
    if (io == NULL) {
        ep.cfg.backend = SX_BACKEND_UDP;
        io = sx_io_udp_create(&ep);
        if (io == NULL) {
            fprintf(g_log, "udp backend create failed\n");
            fclose(g_log);
            free(samples);
            return 1;
        }
    }

    {
        const char *sa = getenv("SX_STATIC_ARP");
        uint32_t sip = 0;
        uint8_t smac[6];
        if (sa != NULL && io->set_static_arp != NULL &&
            parse_static_arp(sa, &sip, smac) == 0) {
            if (io->set_static_arp(io, sip, smac) == SX_OK) {
                fprintf(g_log, "static arp %u.%u.%u.%u applied\n",
                        (sip >> 24) & 0xff, (sip >> 16) & 0xff,
                        (sip >> 8) & 0xff, sip & 0xff);
            }
        }
    }

    fprintf(g_log,
            "io=%s local=%s:%u dst=%u.%u.%u.%u:%u rounds=%u warmup=%u "
            "xdp_bind=%u\n",
            io->name, ifname != NULL ? ifname : "-", ep.cfg.local_port,
            (dip >> 24) & 0xff, (dip >> 16) & 0xff, (dip >> 8) & 0xff,
            dip & 0xff, dport, rounds, warmup, ep.stats.xdp_bind);

    for (i = 0; i < rounds + warmup; i++) {
        uint64_t seq = i;
        uint64_t t0;
        uint64_t deadline;
        int sent = 0;
        int got = 0;

        t0 = now_ns();
        deadline = t0 + 200000000ull;
        while (!sent && now_ns() < deadline) {
            if (io->sendto(io, dip, dport, &seq, sizeof(seq)) == 0) {
                sent = 1;
                break;
            }
            {
                size_t dlen = 0;
                (void)recv_once(io, buf, sizeof(buf), &dlen);
            }
            if (io->update != NULL) {
                io->update(io, now_ms());
            }
        }
        if (!sent) {
            fprintf(g_log, "round %u send timeout\n", i);
            break;
        }
        while (!got && now_ns() < deadline) {
            size_t len = 0;
            int r = recv_once(io, buf, sizeof(buf), &len);
            if (r > 0 && len == sizeof(uint64_t)) {
                got = 1;
                break;
            }
            if (io->update != NULL) {
                io->update(io, now_ms());
            }
        }
        if (!got) {
            fprintf(g_log, "round %u recv timeout\n", i);
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
        fprintf(g_log,
                "rtt(us): min=%llu avg=%llu p50=%llu p99=%llu max=%llu n=%u\n",
                (unsigned long long)mn,
                (unsigned long long)(sum / recorded),
                (unsigned long long)samples[recorded * 50 / 100],
                (unsigned long long)samples[recorded * 99 / 100],
                (unsigned long long)mx, recorded);
    } else {
        fprintf(g_log, "no samples\n");
    }

    if (io->close != NULL) {
        io->close(io);
    }
    free(samples);
    fclose(g_log);
    return recorded > 0 ? 0 : 1;
}
