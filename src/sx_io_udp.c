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
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct sx_udp_priv {
    int sock;
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
                        void *buf, size_t cap, size_t *len) {
    sx_udp_priv_t *p = (sx_udp_priv_t *)io->priv;
    struct sockaddr_in sa;
    socklen_t slen = sizeof(sa);
    ssize_t n;

    n = recvfrom(p->sock, buf, cap, 0, (struct sockaddr *)&sa, &slen);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return SX_ERR_IO;
    }
    if (src_ip) *src_ip = ntohl(sa.sin_addr.s_addr);
    if (src_port) *src_port = ntohs(sa.sin_port);
    if (len) *len = (size_t)n;
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

static int udp_join_multicast(sx_io_t *io, uint32_t group_ip, uint32_t local_ip) {
    sx_udp_priv_t *p = (sx_udp_priv_t *)io->priv;
    struct ip_mreq mreq;

    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = htonl(group_ip);
    mreq.imr_interface.s_addr = local_ip ? htonl(local_ip) : htonl(INADDR_ANY);
    if (setsockopt(p->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,
                   sizeof(mreq)) < 0) {
        return SX_ERR_IO;
    }
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
    /* 仅 SO_REUSEADDR：允许多个组播成员绑定同一端口并各自收到副本。
     * 注意不要设置 SO_REUSEPORT，否则单播/组播可能被内核在多 socket 间负载均衡。 */
    (void)setsockopt(p->sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    /* 允许发送广播（如 255.255.255.255 / 子网广播）。 */
    (void)setsockopt(p->sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    if (ep->cfg.ifname != NULL && ep->cfg.ifname[0] != '\0') {
        (void)setsockopt(p->sock, SOL_SOCKET, SO_BINDTODEVICE, ep->cfg.ifname,
                         strlen(ep->cfg.ifname));
    }

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
