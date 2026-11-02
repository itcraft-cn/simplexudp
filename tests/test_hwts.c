/*
 * test_hwts.c - UDP 硬件时间戳单向时延测试
 *
 * 用法：
 *   test_hwts rx <bind_ip> <port> <count> <out_csv> [ifname]
 *   test_hwts tx <dst_ip> <port> <count> <out_csv> [src_ip] [ifname]
 *
 * 说明：
 *   - rx 端开启 SOF_TIMESTAMPING_RX_HARDWARE，记录每个包的 RX PHC 时间；
 *   - tx 端开启 SOF_TIMESTAMPING_TX_HARDWARE，从 error queue 取 TX PHC 时间；
 *   - 两端 PHC 需先通过 PTP 同步；输出 CSV 供离线计算 rx-tx；
 *   - 时间戳使用 RAW_HARDWARE，即网卡 PHC 时间域，不是 CLOCK_REALTIME。
 */
#include <arpa/inet.h>
#include <errno.h>
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <time.h>
#include <unistd.h>

#ifndef SOF_TIMESTAMPING_OPT_TSONLY
#define SOF_TIMESTAMPING_OPT_TSONLY (1 << 11)
#endif
#ifndef SOF_TIMESTAMPING_OPT_ID
#define SOF_TIMESTAMPING_OPT_ID (1 << 7)
#endif

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static uint64_t ts_to_ns(const struct timespec *ts) {
    return (uint64_t)ts->tv_sec * 1000000000ull + (uint64_t)ts->tv_nsec;
}

static int enable_ts(int fd, unsigned int flags) {
    return setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags));
}

static int parse_ts(struct msghdr *msg, uint64_t *ns) {
    struct cmsghdr *cmsg;
    for (cmsg = CMSG_FIRSTHDR(msg); cmsg != NULL;
         cmsg = CMSG_NXTHDR(msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET &&
            cmsg->cmsg_type == SCM_TIMESTAMPING) {
            struct scm_timestamping *t =
                (struct scm_timestamping *)CMSG_DATA(cmsg);
            if (t->ts[2].tv_sec != 0 || t->ts[2].tv_nsec != 0) {
                *ns = ts_to_ns(&t->ts[2]);
                return 1;
            }
            if (t->ts[0].tv_sec != 0 || t->ts[0].tv_nsec != 0) {
                *ns = ts_to_ns(&t->ts[0]);
                return 1;
            }
        }
    }
    return 0;
}

static int do_rx(const char *ip, uint16_t port, uint32_t count,
                 const char *out, const char *ifname) {
    int fd;
    struct sockaddr_in sa;
    FILE *fp;
    uint32_t got = 0;
    uint64_t last = 0;
    unsigned int flags = SOF_TIMESTAMPING_RX_HARDWARE |
                         SOF_TIMESTAMPING_RAW_HARDWARE |
                         SOF_TIMESTAMPING_SOFTWARE |
                         SOF_TIMESTAMPING_OPT_CMSG |
                         SOF_TIMESTAMPING_OPT_ID;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return 1;
    }
    if (ifname != NULL && ifname[0] != '\0') {
        (void)setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname,
                         strlen(ifname));
    }
    if (enable_ts(fd, flags) != 0) {
        fprintf(stderr, "enable rx timestamp failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) {
        close(fd);
        return 1;
    }
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        fprintf(stderr, "bind failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    fp = fopen(out, "w");
    if (fp == NULL) {
        close(fd);
        return 1;
    }

    while (got < count) {
        uint8_t data[256];
        char ctrl[512];
        struct iovec iov = {data, sizeof(data)};
        struct msghdr msg;
        ssize_t n;
        uint64_t ts = 0;
        uint64_t seq = 0;

        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = ctrl;
        msg.msg_controllen = sizeof(ctrl);
        n = recvmsg(fd, &msg, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "recvmsg failed: %s\n", strerror(errno));
            break;
        }
        if (n >= (ssize_t)sizeof(seq)) {
            memcpy(&seq, data, sizeof(seq));
        }
        if (!parse_ts(&msg, &ts)) {
            continue;
        }
        if (got > 0 && got % 1000 == 0) {
            fprintf(stderr, "rx %u/%u\n", got, count);
        }
        fprintf(fp, "%llu,%llu\n", (unsigned long long)seq,
                (unsigned long long)ts);
        last = ts;
        got++;
    }
    fclose(fp);
    close(fd);
    fprintf(stderr, "rx done=%u last=%llu\n", got,
            (unsigned long long)last);
    return got == count ? 0 : 1;
}

static int do_tx(const char *ip, uint16_t port, uint32_t count,
                 const char *out, const char *src_ip, const char *ifname) {
    int fd;
    struct sockaddr_in sa;
    struct timeval tv;
    FILE *fp;
    uint32_t sent = 0;
    unsigned int flags = SOF_TIMESTAMPING_TX_HARDWARE |
                         SOF_TIMESTAMPING_TX_SOFTWARE |
                         SOF_TIMESTAMPING_RAW_HARDWARE |
                         SOF_TIMESTAMPING_SOFTWARE |
                         SOF_TIMESTAMPING_OPT_CMSG |
                         SOF_TIMESTAMPING_OPT_ID;
    const char *sw_only = getenv("HWTS_SW");
    if (sw_only != NULL) {
        flags = SOF_TIMESTAMPING_TX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE |
                SOF_TIMESTAMPING_RAW_HARDWARE |
                SOF_TIMESTAMPING_OPT_CMSG | SOF_TIMESTAMPING_OPT_ID;
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return 1;
    }
    if (ifname != NULL && ifname[0] != '\0') {
        (void)setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname,
                         strlen(ifname));
    }
    if (src_ip != NULL && src_ip[0] != '\0') {
        struct sockaddr_in src;
        memset(&src, 0, sizeof(src));
        src.sin_family = AF_INET;
        src.sin_port = 0;
        if (inet_pton(AF_INET, src_ip, &src.sin_addr) != 1 ||
            bind(fd, (struct sockaddr *)&src, sizeof(src)) != 0) {
            fprintf(stderr, "bind src failed: %s\n", strerror(errno));
            close(fd);
            return 1;
        }
    }
    if (enable_ts(fd, flags) != 0) {
        fprintf(stderr, "enable tx timestamp failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    {
        unsigned int got_flags = 0;
        socklen_t flen = sizeof(got_flags);
        (void)getsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &got_flags, &flen);
        fprintf(stderr, "timestamp flags=0x%x\n", got_flags);
    }
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) {
        close(fd);
        return 1;
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        fprintf(stderr, "connect failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    fp = fopen(out, "w");
    if (fp == NULL) {
        close(fd);
        return 1;
    }

    while (sent < count) {
        uint64_t seq = sent;
        uint64_t ts = 0;
        int got = 0;
        ssize_t n = send(fd, &seq, sizeof(seq), 0);
        if (n != (ssize_t)sizeof(seq)) {
            fprintf(stderr, "send failed: %s\n", strerror(errno));
            break;
        }
        /* 从 error queue 取 TX 硬件时间戳；硬件时间戳是异步的，不能遇 EAGAIN 就退出。 */
        {
            uint64_t deadline = now_ms() + 1000;
            while (!got && now_ms() < deadline) {
            uint8_t data[256];
            char ctrl[512];
            struct iovec iov = {data, sizeof(data)};
            struct msghdr msg;
            memset(&msg, 0, sizeof(msg));
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = ctrl;
            msg.msg_controllen = sizeof(ctrl);
            {
                struct pollfd pfd;
                pfd.fd = fd;
                pfd.events = POLLERR;
                pfd.revents = 0;
                if (poll(&pfd, 1, 100) <= 0) {
                    usleep(50);
                    continue;
                }
            }
            n = recvmsg(fd, &msg, MSG_ERRQUEUE);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    usleep(50);
                    continue;
                }
                if (errno == EINTR) {
                    continue;
                }
                fprintf(stderr, "errqueue recvmsg failed: %s\n",
                        strerror(errno));
                break;
            }
            if (sent == 0) {
                struct cmsghdr *cmsg;
                for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
                     cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                    fprintf(stderr, "  cmsg level=%d type=%d len=%zu\n",
                            cmsg->cmsg_level, cmsg->cmsg_type,
                            (size_t)cmsg->cmsg_len);
                }
                fprintf(stderr, "  errqueue n=%zd\n", n);
            }
            if (parse_ts(&msg, &ts)) {
                got = 1;
            }
            }
        }
        if (!got) {
            fprintf(stderr, "tx timestamp timeout at %u\n", sent);
            break;
        }
        if (sent > 0 && sent % 1000 == 0) {
            fprintf(stderr, "tx %u/%u\n", sent, count);
        }
        fprintf(fp, "%llu,%llu\n", (unsigned long long)seq,
                (unsigned long long)ts);
        sent++;
    }
    fclose(fp);
    close(fd);
    fprintf(stderr, "tx done=%u\n", sent);
    return sent == count ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 6) {
        return 2;
    }
    if (strcmp(argv[1], "rx") == 0) {
        return do_rx(argv[2], (uint16_t)atoi(argv[3]),
                     (uint32_t)strtoul(argv[4], NULL, 10), argv[5],
                     argc > 6 ? argv[6] : NULL);
    }
    if (strcmp(argv[1], "tx") == 0) {
        return do_tx(argv[2], (uint16_t)atoi(argv[3]),
                     (uint32_t)strtoul(argv[4], NULL, 10), argv[5],
                     argc > 6 ? argv[6] : NULL,
                     argc > 7 ? argv[7] : NULL);
    }
    return 2;
}
