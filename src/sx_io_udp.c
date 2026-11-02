/*
 * sx_io_udp.c - 内核 UDP socket 后端
 *
 * 用途：
 *   - 无需 root / 无需网卡特权，便于开发、单测与 CI；
 *   - 与 AF_XDP 后端线格式一致（SX 头位于 UDP 净荷），可互操作。
 *
 * 注意：
 *   - 非阻塞 socket + poll；epoll/select 由上层 poll 决定。
 *   - 组播需调用 join_multicast 加入组；广播由 sendto 目的地址决定。
 *   - 可选“丢包/乱序注入”（编译期 SX_ENABLE_LOSS_INJECT，运行期环境变量开启），
 *     用于在没有 root/netem 的环境下测试可靠层在真实网络上的恢复能力：
 *       SX_LOSSY_DROP_PPM    丢包概率（ppm，0..1000000）
 *       SX_LOSSY_DUP_PPM     重复概率（ppm）
 *       SX_LOSSY_REORDER_PPM 相邻乱序概率（ppm）
 */
#include "sx_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/net_tstamp.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef SOF_TIMESTAMPING_OPT_ID
#define SOF_TIMESTAMPING_OPT_ID (1 << 7)
#endif

/* 兼容旧内核头文件：部分 CentOS 7 头文件未定义 scm_timestamping。 */
struct sx_scm_timestamping {
    struct timespec ts[3];
};

typedef struct sx_udp_priv {
    int sock;
    int ts_mode;   /* 实际模式：SX_RX_TS_* */
    int ts_status; /* 状态/原因 */
    int ts_clock;  /* 时钟域 */
    int ts_hw;     /* 1=套接字已启用硬件时间戳 */
#ifdef SX_ENABLE_LOSS_INJECT
    uint32_t drop_ppm;
    uint32_t dup_ppm;
    uint32_t reorder_ppm;
    uint64_t rng;
    int has_pend;
    uint8_t pend[2048];
    size_t pend_len;
    uint32_t pend_ip;
    uint16_t pend_port;
#endif
} sx_udp_priv_t;

#ifdef SX_ENABLE_LOSS_INJECT
static uint32_t sx_xorshift64(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return (uint32_t)(x >> 16) & 0xffffffu;
}

static uint32_t env_ppm(const char *name) {
    const char *v = getenv(name);
    long n;
    if (v == NULL || v[0] == '\0') {
        return 0;
    }
    n = strtol(v, NULL, 10);
    if (n < 0) {
        n = 0;
    }
    if (n > 1000000) {
        n = 1000000;
    }
    return (uint32_t)n;
}
#endif

/* 真正的发送（无注入）。 */
static int udp_raw_send(sx_udp_priv_t *p, uint32_t dst_ip, uint16_t dst_port,
                        const void *buf, size_t len) {
    struct sockaddr_in sa;
    ssize_t n;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(dst_port);
    sa.sin_addr.s_addr = htonl(dst_ip);
    n = sendto(p->sock, buf, len, 0, (struct sockaddr *)&sa, sizeof(sa));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
            return SX_ERR_AGAIN;
        }
        return SX_ERR_IO;
    }
    return 0;
}

static int udp_sendto(sx_io_t *io, uint32_t dst_ip, uint16_t dst_port,
                      const void *buf, size_t len) {
    sx_udp_priv_t *p = (sx_udp_priv_t *)io->priv;
#ifdef SX_ENABLE_LOSS_INJECT
    /* 丢包：直接视为在网络上丢失。 */
    if (p->drop_ppm != 0 &&
        sx_xorshift64(&p->rng) % 1000000u < p->drop_ppm) {
        return SX_OK;
    }
    /* 重复。 */
    if (p->dup_ppm != 0 && sx_xorshift64(&p->rng) % 1000000u < p->dup_ppm) {
        (void)udp_raw_send(p, dst_ip, dst_port, buf, len);
        return udp_raw_send(p, dst_ip, dst_port, buf, len);
    }
    /* 相邻乱序：暂存一包，下一次发送时先发新包再发暂存包。 */
    if (p->reorder_ppm != 0) {
        if (p->has_pend && len <= sizeof(p->pend) &&
            sx_xorshift64(&p->rng) % 1000000u < p->reorder_ppm) {
            int r = udp_raw_send(p, dst_ip, dst_port, buf, len);
            (void)udp_raw_send(p, p->pend_ip, p->pend_port, p->pend,
                               p->pend_len);
            p->has_pend = 0;
            return r;
        }
        if (!p->has_pend && len <= sizeof(p->pend) &&
            sx_xorshift64(&p->rng) % 1000000u < p->reorder_ppm) {
            memcpy(p->pend, buf, len);
            p->pend_len = len;
            p->pend_ip = dst_ip;
            p->pend_port = dst_port;
            p->has_pend = 1;
            return SX_OK;
        }
        if (p->has_pend) {
            (void)udp_raw_send(p, p->pend_ip, p->pend_port, p->pend,
                               p->pend_len);
            p->has_pend = 0;
        }
    }
#endif
    return udp_raw_send(p, dst_ip, dst_port, buf, len);
}

static int udp_recvfrom(sx_io_t *io, uint32_t *src_ip, uint16_t *src_port,
                        void *buf, size_t cap, size_t *len,
                        sx_pkt_info_t *info) {
    sx_udp_priv_t *p = (sx_udp_priv_t *)io->priv;
    struct sockaddr_in sa;
    struct iovec iov;
    struct msghdr msg;
    char ctrl[512];
    ssize_t n;

    if (info != NULL) {
        memset(info, 0, sizeof(*info));
        info->ts_mode = (uint32_t)p->ts_mode;
        info->ts_status = (uint32_t)p->ts_status;
        info->clock_id = (uint32_t)p->ts_clock;
    }
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = buf;
    iov.iov_len = cap;
    msg.msg_name = &sa;
    msg.msg_namelen = sizeof(sa);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof(ctrl);

    n = recvmsg(p->sock, &msg, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return SX_ERR_IO;
    }
    if (src_ip) *src_ip = ntohl(sa.sin_addr.s_addr);
    if (src_port) *src_port = ntohs(sa.sin_port);
    if (len) *len = (size_t)n;
    if (info != NULL && p->ts_mode != SX_RX_TS_OFF) {
        int hw_found = 0;
        if (p->ts_hw) {
            struct cmsghdr *cmsg;
            for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
                 cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                if (cmsg->cmsg_level == SOL_SOCKET &&
                    cmsg->cmsg_type == SCM_TIMESTAMPING) {
                    struct sx_scm_timestamping *t =
                        (struct sx_scm_timestamping *)CMSG_DATA(cmsg);
                    if (t->ts[2].tv_sec != 0 || t->ts[2].tv_nsec != 0) {
                        info->rx_ns = (uint64_t)t->ts[2].tv_sec * 1000000000ull +
                                      (uint64_t)t->ts[2].tv_nsec;
                        info->clock_id = SX_RX_TS_CLOCK_PHC;
                        info->ts_mode = SX_RX_TS_HARDWARE;
                        info->ts_status = SX_RX_TS_HW_OK;
                        info->valid = 1;
                        hw_found = 1;
                    }
                }
            }
        }
        if (!hw_found) {
            struct timespec ts;
            if (p->ts_mode == SX_RX_TS_HARDWARE) {
                info->ts_status = SX_RX_TS_ERR_IO;
                info->valid = 0;
            } else {
                clock_gettime(CLOCK_REALTIME, &ts);
                info->rx_ns = (uint64_t)ts.tv_sec * 1000000000ull +
                              (uint64_t)ts.tv_nsec;
                info->clock_id = SX_RX_TS_CLOCK_REALTIME;
                info->ts_mode = SX_RX_TS_AUTO;
                info->ts_status = SX_RX_TS_SW_FALLBACK;
                info->valid = 1;
            }
        }
    }
    return 1;
}

static int udp_poll(sx_io_t *io, int timeout_ms) {
    sx_udp_priv_t *p = (sx_udp_priv_t *)io->priv;
    struct pollfd pfd;
    int r;

    pfd.fd = p->sock;
    pfd.events = POLLIN;
    pfd.revents = 0;
    r = poll(&pfd, 1, timeout_ms);
    if (r < 0) {
        if (errno == EINTR) {
            return 0;
        }
        return SX_ERR_IO;
    }
    return r;
}

static int udp_join_multicast(sx_io_t *io, uint32_t group_ip,
                              uint32_t iface_ip) {
    sx_udp_priv_t *p = (sx_udp_priv_t *)io->priv;
    struct ip_mreq mreq;
    struct in_addr ifa;

    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = htonl(group_ip);
    mreq.imr_interface.s_addr = iface_ip ? htonl(iface_ip) : htonl(INADDR_ANY);
    if (setsockopt(p->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,
                   sizeof(mreq)) < 0) {
        return SX_ERR_IO;
    }
    /* 指定组播发送/接收接口，避免走默认路由（多网卡场景必需）。 */
    ifa.s_addr = iface_ip ? htonl(iface_ip) : htonl(INADDR_ANY);
    (void)setsockopt(p->sock, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof(ifa));
    return SX_OK;
}

static void udp_close(sx_io_t *io) {
    if (io == NULL) {
        return;
    }
    if (io->priv != NULL) {
        sx_udp_priv_t *p = (sx_udp_priv_t *)io->priv;
        if (p->sock >= 0) {
            close(p->sock);
        }
        free(p);
    }
    free(io);
}

/* 应用 UDP 收发缓冲配置。优先使用 FORCE 变体（需 CAP_NET_ADMIN），
 * 失败则回退普通 setsockopt；最后读取实际值并记录，便于现场确认是否被内核钳制。 */
static void udp_apply_bufs(sx_endpoint_t *ep, int fd) {
    struct {
        int opt;
        int force_opt;
        uint32_t req;
        const char *name;
    } bufs[2];
    size_t i;

    bufs[0].opt = SO_RCVBUF;
#ifdef SO_RCVBUFFORCE
    bufs[0].force_opt = SO_RCVBUFFORCE;
#else
    bufs[0].force_opt = SO_RCVBUF;
#endif
    bufs[0].req = ep->cfg.udp_rcvbuf;
    bufs[0].name = "rcvbuf";
    bufs[1].opt = SO_SNDBUF;
#ifdef SO_SNDBUFFORCE
    bufs[1].force_opt = SO_SNDBUFFORCE;
#else
    bufs[1].force_opt = SO_SNDBUF;
#endif
    bufs[1].req = ep->cfg.udp_sndbuf;
    bufs[1].name = "sndbuf";

    for (i = 0; i < 2; i++) {
        int val;
        int actual = 0;
        socklen_t alen = sizeof(actual);
        if (bufs[i].req == 0) {
            continue;
        }
        val = (int)bufs[i].req;
        if (setsockopt(fd, SOL_SOCKET, bufs[i].force_opt, &val, sizeof(val)) != 0 &&
            setsockopt(fd, SOL_SOCKET, bufs[i].opt, &val, sizeof(val)) != 0) {
            sx_log(ep, SX_LOG_WARN, "udp %s=%u setsockopt failed",
                   bufs[i].name, bufs[i].req);
            continue;
        }
        if (getsockopt(fd, SOL_SOCKET, bufs[i].opt, &actual, &alen) == 0) {
            sx_log(ep, SX_LOG_INFO, "udp %s requested=%u actual=%d",
                   bufs[i].name, bufs[i].req, actual);
            if (actual < (int)bufs[i].req) {
                sx_log(ep, SX_LOG_WARN,
                       "udp %s clamped to %d (< requested %u); check "
                       "net.core.rmem_max/wmem_max or CAP_NET_ADMIN",
                       bufs[i].name, actual, bufs[i].req);
            }
        }
    }
}

sx_io_t *sx_io_udp_create(sx_endpoint_t *ep) {
    sx_io_t *io;
    sx_udp_priv_t *p;
    struct sockaddr_in sa;
    socklen_t slen;
    int on = 1;
    int flags;

    if (ep == NULL) {
        return NULL;
    }
    io = (sx_io_t *)calloc(1, sizeof(*io));
    p = (sx_udp_priv_t *)calloc(1, sizeof(*p));
    if (io == NULL || p == NULL) {
        free(io);
        free(p);
        return NULL;
    }
    p->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (p->sock < 0) {
        free(io);
        free(p);
        return NULL;
    }
    udp_apply_bufs(ep, p->sock);
    /* 仅 SO_REUSEADDR：允许多个组播成员绑定同一端口并各自收到副本。
     * 注意不要设置 SO_REUSEPORT，否则单播/组播可能被内核在多 socket 间负载均衡。 */
    (void)setsockopt(p->sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    /* 允许发送广播（如 255.255.255.255 / 子网广播）。 */
    (void)setsockopt(p->sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    if (ep->cfg.ifname != NULL && ep->cfg.ifname[0] != '\0') {
        (void)setsockopt(p->sock, SOL_SOCKET, SO_BINDTODEVICE, ep->cfg.ifname,
                         strlen(ep->cfg.ifname));
    }
    p->ts_mode = SX_RX_TS_OFF;
    p->ts_status = SX_RX_TS_DISABLED;
    p->ts_clock = SX_RX_TS_CLOCK_REALTIME;
    if (ep->cfg.rx_ts_mode != SX_RX_TS_OFF) {
        unsigned int ts_flags = SOF_TIMESTAMPING_RX_HARDWARE |
                                SOF_TIMESTAMPING_RAW_HARDWARE |
                                SOF_TIMESTAMPING_OPT_CMSG |
                                SOF_TIMESTAMPING_OPT_ID;
        int rc = setsockopt(p->sock, SOL_SOCKET, SO_TIMESTAMPING, &ts_flags,
                            sizeof(ts_flags));
        if (rc == 0) {
            p->ts_mode = SX_RX_TS_HARDWARE;
            p->ts_status = SX_RX_TS_HW_OK;
            p->ts_clock = SX_RX_TS_CLOCK_PHC;
            p->ts_hw = 1;
            sx_log(ep, SX_LOG_INFO, "udp rx hardware timestamp enabled");
        } else if (ep->cfg.rx_ts_mode == SX_RX_TS_AUTO) {
            p->ts_mode = SX_RX_TS_AUTO;
            p->ts_status = SX_RX_TS_SW_FALLBACK;
            p->ts_clock = SX_RX_TS_CLOCK_REALTIME;
            sx_log(ep, SX_LOG_WARN,
                   "udp rx hardware timestamp unavailable (errno=%d), "
                   "fallback to software",
                   errno);
        } else {
            p->ts_mode = SX_RX_TS_OFF;
            p->ts_status = SX_RX_TS_ERR_UNSUPPORTED;
            sx_log(ep, SX_LOG_WARN,
                   "udp rx hardware timestamp requested but unavailable "
                   "(errno=%d), disabled",
                   errno);
        }
    }
    ep->stats.rx_ts_mode = (uint32_t)p->ts_mode;
    ep->stats.rx_ts_status = (uint32_t)p->ts_status;
    ep->stats.rx_ts_clock_id = (uint32_t)p->ts_clock;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(ep->cfg.local_port);
    sa.sin_addr.s_addr =
        ep->cfg.local_ip ? htonl(ep->cfg.local_ip) : htonl(INADDR_ANY);
    if (bind(p->sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(p->sock);
        free(io);
        free(p);
        return NULL;
    }
    /* 回填实际端口（支持 0 端口自动分配）。 */
    slen = sizeof(sa);
    if (getsockname(p->sock, (struct sockaddr *)&sa, &slen) == 0) {
        ep->cfg.local_port = ntohs(sa.sin_port);
        if (ep->cfg.local_ip == 0) {
            ep->cfg.local_ip = ntohl(sa.sin_addr.s_addr);
        }
    }

    flags = fcntl(p->sock, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(p->sock, F_SETFL, flags | O_NONBLOCK);
    }

#ifdef SX_ENABLE_LOSS_INJECT
    p->drop_ppm = env_ppm("SX_LOSSY_DROP_PPM");
    p->dup_ppm = env_ppm("SX_LOSSY_DUP_PPM");
    p->reorder_ppm = env_ppm("SX_LOSSY_REORDER_PPM");
    p->rng = 0x9e3779b97f4a7c15ull ^ (uint64_t)(uintptr_t)p;
    if (p->drop_ppm || p->dup_ppm || p->reorder_ppm) {
        sx_log(ep, SX_LOG_WARN,
               "loss inject enabled: drop=%u dup=%u reorder=%u ppm",
               p->drop_ppm, p->dup_ppm, p->reorder_ppm);
    }
#endif

    io->name = "udp";
    io->ep = ep;
    io->fd = p->sock;
    io->priv = p;
    io->sendto = udp_sendto;
    io->recvfrom = udp_recvfrom;
    io->poll = udp_poll;
    io->join_multicast = udp_join_multicast;
    io->close = udp_close;
    return io;
}
