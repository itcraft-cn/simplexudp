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
} sx_udp_priv_t;

static int udp_sendto(sx_io_t *io, uint32_t dst_ip, uint16_t dst_port,
                      const void *buf, size_t len) {
    sx_udp_priv_t *p = (sx_udp_priv_t *)io->priv;
    struct sockaddr_in sa;
    ssize_t n;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(dst_port);
    sa.sin_addr.s_addr = htonl(dst_ip);
    n = sendto(p->sock, buf, len, 0, (struct sockaddr *)&sa, sizeof(sa));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
            sx_log(io->ep, SX_LOG_DEBUG, "udp send EAGAIN len=%zu", len);
            return SX_ERR_AGAIN;
        }
        sx_log(io->ep, SX_LOG_WARN, "udp send error errno=%d len=%zu", errno,
               len);
        return SX_ERR_IO;
    }
    return 0;
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
