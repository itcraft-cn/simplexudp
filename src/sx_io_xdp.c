/*
 * sx_io_xdp.c - AF_XDP 后端
 *
 * 数据面：
 *   - UMEM 预分配等长 chunk，FILL/COMPLETION/RX/TX 四个 SPSC 环；
 *   - 加载并 attach 自带 XDP 重定向程序（bpf/sx_xdp_kern.c），将本端口的
 *     IPv4/UDP 与 ARP 重定向到 xsk；
 *   - 用户态构造 Ethernet/IPv4/UDP 帧（sx_frame_build），校验和完整。
 *
 * 模式：
 *   - attach：native（DRV）优先，失败回退 generic（SKB）；
 *   - bind：zero-copy 优先，不支持时回退 copy（自动侦测）；
 *   - generic/skb 模式下强制 copy（内核不支持 skb+zero-copy）。
 *
 * BPF 目标对象：
 *   - 优先使用编译期内嵌的 sx_xdp_kern.o（SX_XDP_EMBEDDED）；
 *   - 可用环境变量 SX_XDP_OBJ 指定 .o 路径覆盖。
 *
 * 注意：
 *   - AF_XDP 绑定单个 RX 队列，接收路径流量需引导到该队列；
 *   - 本后端在同一二层网段内工作，跨网段需静态 ARP 或网关支持。
 */
#include "sx_internal.h"

#ifdef SX_NO_XDP
/*
 * UDP-only 构建（SX_NO_XDP=1）：不依赖 libbpf/libxdp/clang。
 * 提供同名接口的桩实现：XDP 后端不可用，AUTO 会自动回退到内核 UDP。
 */
sx_io_t *sx_io_xdp_create(sx_endpoint_t *ep) {
    (void)ep;
    return NULL;
}

int sx_io_xdp_send_frame(sx_io_t *io, const uint8_t *frame, size_t len) {
    (void)io;
    (void)frame;
    (void)len;
    return SX_ERR_UNSUPPORTED;
}
#else
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <xdp/xsk.h>

#ifdef SX_XDP_EMBEDDED
extern const unsigned char sx_xdp_kern_o[];
extern const unsigned int sx_xdp_kern_o_len;
#endif

#define SX_XDP_UMEM_FRAMES_DEFAULT 4096u
#define SX_XDP_FRAME_SIZE_DEFAULT 4096u
#define SX_XDP_RX_BATCH 1u
#define SX_XDP_RX_LOOP_MAX 1024u /* 单次 recvfrom 内最多处理的帧数 */
#define SX_XDP_INVALID_FRAME ((uint64_t)~0ull)

typedef struct sx_xdp_config_kern {
    uint16_t port;
    uint16_t flags;
} sx_xdp_config_kern_t;

typedef struct sx_xdp_priv {
    char ifname[IF_NAMESIZE];
    int ifindex;
    uint8_t local_mac[6];
    uint32_t local_ip;
    uint16_t local_port;

    void *umem_area;
    size_t umem_size;
    struct xsk_umem *umem;
    struct xsk_ring_prod fq;
    struct xsk_ring_cons cq;
    struct xsk_ring_cons rx;
    struct xsk_ring_prod tx;
    struct xsk_socket *xsk;
    int xsk_fd;

    uint32_t frame_size;
    uint32_t num_frames;
    uint64_t *frame_addr;
    uint32_t frame_free;

    sx_arp_t *arp;
    struct bpf_object *obj;
    int xsk_map_fd;
    int sx_config_map_fd;

    unsigned int attach_flags;
    uint16_t bind_flags;
    int attached;
    uint32_t outstanding_tx;
} sx_xdp_priv_t;

/* 解析接口 MAC 与 IPv4 地址。 */
static int xdp_query_iface(const char *ifname, uint8_t mac[6], uint32_t *ip) {
    int s;
    struct ifreq ifr;

    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        return SX_ERR_IO;
    }
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    if (ioctl(s, SIOCGIFHWADDR, &ifr) == 0) {
        memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    }
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    if (ioctl(s, SIOCGIFADDR, &ifr) == 0) {
        if (ip != NULL) {
            struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
            *ip = ntohl(sin->sin_addr.s_addr);
        }
    }
    close(s);
    return SX_OK;
}

static uint64_t xdp_frame_alloc(sx_xdp_priv_t *p) {
    if (p->frame_free == 0) {
        return SX_XDP_INVALID_FRAME;
    }
    return p->frame_addr[--p->frame_free];
}

static void xdp_frame_release(sx_xdp_priv_t *p, uint64_t addr) {
    if (p->frame_free < p->num_frames) {
        p->frame_addr[p->frame_free++] = addr;
    }
}

static void xdp_complete_tx(sx_xdp_priv_t *p) {
    unsigned int completed;
    uint32_t idx = 0;
    unsigned int i;

    if (p->outstanding_tx == 0) {
        return;
    }
    /* 触发内核完成发送。 */
    (void)sendto(p->xsk_fd, NULL, 0, MSG_DONTWAIT, NULL, 0);
    completed = xsk_ring_cons__peek(&p->cq, XSK_RING_CONS__DEFAULT_NUM_DESCS, &idx);
    if (completed > 0) {
        for (i = 0; i < completed; i++) {
            xdp_frame_release(p, *xsk_ring_cons__comp_addr(&p->cq, idx++));
        }
        xsk_ring_cons__release(&p->cq, completed);
        if (completed >= p->outstanding_tx) {
            p->outstanding_tx = 0;
        } else {
            p->outstanding_tx -= completed;
        }
    }
}

/* 用空闲帧填充 FILL 环。 */
static void xdp_fill_fq(sx_xdp_priv_t *p) {
    uint32_t stock;
    uint32_t idx = 0;
    uint32_t i;
    uint32_t reserved;

    stock = xsk_prod_nb_free(&p->fq, p->frame_free);
    if (stock == 0) {
        return;
    }
    reserved = xsk_ring_prod__reserve(&p->fq, stock, &idx);
    for (i = 0; i < reserved; i++) {
        uint64_t addr = xdp_frame_alloc(p);
        if (addr == SX_XDP_INVALID_FRAME) {
            break;
        }
        *xsk_ring_prod__fill_addr(&p->fq, idx++) = addr;
    }
    if (i > 0) {
        xsk_ring_prod__submit(&p->fq, i);
    }
}

/* 发送一帧原始以太帧（ARP 等）。 */
int sx_io_xdp_send_frame(sx_io_t *io, const uint8_t *frame, size_t len) {
    sx_xdp_priv_t *p;
    uint32_t idx = 0;
    uint64_t addr;
    void *dst;

    if (io == NULL || io->priv == NULL || frame == NULL) {
        return SX_ERR_INVALID;
    }
    p = (sx_xdp_priv_t *)io->priv;
    xdp_complete_tx(p);
    if (p->frame_free == 0) {
        return SX_ERR_AGAIN;
    }
    if (len > p->frame_size) {
        return SX_ERR_TOO_LARGE;
    }
    if (xsk_ring_prod__reserve(&p->tx, 1, &idx) != 1) {
        return SX_ERR_AGAIN;
    }
    addr = xdp_frame_alloc(p);
    if (addr == SX_XDP_INVALID_FRAME) {
        return SX_ERR_AGAIN;
    }
    dst = xsk_umem__get_data(p->umem_area, addr);
    memcpy(dst, frame, len);
    xsk_ring_prod__tx_desc(&p->tx, idx)->addr = addr;
    xsk_ring_prod__tx_desc(&p->tx, idx)->len = (uint32_t)len;
    xsk_ring_prod__submit(&p->tx, 1);
    p->outstanding_tx++;
    (void)sendto(p->xsk_fd, NULL, 0, MSG_DONTWAIT, NULL, 0);
    return SX_OK;
}

/* 组播 IP -> 组播 MAC（RFC 1112）。 */
static void xdp_mcast_mac(uint32_t ip, uint8_t mac[6]) {
    mac[0] = 0x01;
    mac[1] = 0x00;
    mac[2] = 0x5e;
    mac[3] = (uint8_t)((ip >> 16) & 0x7f);
    mac[4] = (uint8_t)((ip >> 8) & 0xff);
    mac[5] = (uint8_t)(ip & 0xff);
}

static int xdp_resolve_mac(sx_xdp_priv_t *p, uint32_t dst_ip, uint8_t mac[6]) {
    if (dst_ip == 0xffffffffu) {
        memset(mac, 0xff, 6);
        return SX_OK;
    }
    if ((dst_ip & SX_IP_CLASS_D_MASK) == SX_IP_CLASS_D_PREFIX) {
        xdp_mcast_mac(dst_ip, mac);
        return SX_OK;
    }
    return sx_arp_resolve(p->arp, dst_ip, mac);
}

static int xdp_sendto(sx_io_t *io, uint32_t dst_ip, uint16_t dst_port,
                      const void *buf, size_t len) {
    sx_xdp_priv_t *p;
    uint8_t dst_mac[6];
    uint32_t idx = 0;
    uint64_t addr;
    void *dst;
    ssize_t flen;

    if (io == NULL || io->priv == NULL) {
        return SX_ERR_INVALID;
    }
    p = (sx_xdp_priv_t *)io->priv;
    if (xdp_resolve_mac(p, dst_ip, dst_mac) != SX_OK) {
        return SX_ERR_AGAIN; /* ARP 未解析，等待下次 */
    }
    xdp_complete_tx(p);
    if (p->frame_free == 0) {
        return SX_ERR_AGAIN;
    }
    if (xsk_ring_prod__reserve(&p->tx, 1, &idx) != 1) {
        return SX_ERR_AGAIN;
    }
    addr = xdp_frame_alloc(p);
    if (addr == SX_XDP_INVALID_FRAME) {
        return SX_ERR_AGAIN;
    }
    dst = xsk_umem__get_data(p->umem_area, addr);
    flen = sx_frame_build((uint8_t *)dst, p->frame_size, p->local_mac, dst_mac,
                          p->local_ip, dst_ip, p->local_port, dst_port, buf,
                          len);
    if (flen < 0) {
        xdp_frame_release(p, addr);
        return (int)flen;
    }
    xsk_ring_prod__tx_desc(&p->tx, idx)->addr = addr;
    xsk_ring_prod__tx_desc(&p->tx, idx)->len = (uint32_t)flen;
    xsk_ring_prod__submit(&p->tx, 1);
    p->outstanding_tx++;
    (void)sendto(p->xsk_fd, NULL, 0, MSG_DONTWAIT, NULL, 0);
    return SX_OK;
}

static int xdp_recvfrom(sx_io_t *io, uint32_t *src_ip, uint16_t *src_port,
                        void *buf, size_t cap, size_t *len) {
    sx_xdp_priv_t *p;

    if (io == NULL || io->priv == NULL) {
        return SX_ERR_INVALID;
    }
    p = (sx_xdp_priv_t *)io->priv;

    {
        uint32_t guard;
        for (guard = 0; guard < SX_XDP_RX_LOOP_MAX; guard++) {
        uint32_t idx = 0;
        uint32_t n;
        uint64_t addr;
        uint32_t flen;
        uint8_t *frame;

        xdp_complete_tx(p);
        xdp_fill_fq(p);

        n = xsk_ring_cons__peek(&p->rx, SX_XDP_RX_BATCH, &idx);
        if (n == 0) {
            return 0;
        }
        addr = xsk_ring_cons__rx_desc(&p->rx, idx)->addr;
        flen = xsk_ring_cons__rx_desc(&p->rx, idx)->len;
        addr = xsk_umem__extract_addr(addr);
        frame = (uint8_t *)xsk_umem__get_data(p->umem_area, addr);

        if (flen > 0 && frame[12] == 0x08 && frame[13] == 0x06) {
            /* ARP。 */
            (void)sx_arp_input(p->arp, frame, flen);
            xdp_frame_release(p, addr);
            xsk_ring_cons__release(&p->rx, 1);
            continue;
        }
        {
            const uint8_t *payload = NULL;
            size_t plen = 0;
            uint32_t sip = 0, dip = 0;
            uint16_t sp = 0, dp = 0;
            if (sx_frame_parse(frame, flen, &sip, &dip, &sp, &dp, &payload,
                               &plen) == SX_OK) {
                size_t copy = plen < cap ? plen : cap;
                if (copy > 0) {
                    memcpy(buf, payload, copy);
                }
                if (src_ip) *src_ip = sip;
                if (src_port) *src_port = sp;
                if (len) *len = copy;
                xdp_frame_release(p, addr);
                xsk_ring_cons__release(&p->rx, 1);
                return 1;
            }
        }
        /* 非目标帧（理论上 XDP 程序已过滤），丢弃继续。 */
        xdp_frame_release(p, addr);
        xsk_ring_cons__release(&p->rx, 1);
        } /* guard loop */
    }
    return 0;
}

static void xdp_update(sx_io_t *io, uint64_t now_ms) {
    sx_xdp_priv_t *p;
    if (io == NULL || io->priv == NULL) {
        return;
    }
    p = (sx_xdp_priv_t *)io->priv;
    /* ARP 老化/重试（ARP 实例私有于 xdp 后端）。 */
    sx_arp_update(p->arp, now_ms);
}

static int xdp_poll(sx_io_t *io, int timeout_ms) {
    struct pollfd pfd;
    int r;
    if (io == NULL || io->fd < 0) {
        return 0;
    }
    pfd.fd = io->fd;
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

static int xdp_join_multicast(sx_io_t *io, uint32_t group_ip, uint32_t local_ip) {
    /* AF_XDP 组播由网卡/驱动与 XDP 程序决定，无 socket 级加入操作。 */
    SX_UNUSED(io);
    SX_UNUSED(group_ip);
    SX_UNUSED(local_ip);
    return SX_ERR_UNSUPPORTED;
}

/* 绑定 AF_XDP socket，按 xdp_bind 自动侦测零拷贝。 */
static int xdp_bind_socket(sx_xdp_priv_t *p, sx_endpoint_t *ep) {
    struct xsk_socket_config cfg;
    int ret;
    uint16_t preferred = XDP_COPY;

    memset(&cfg, 0, sizeof(cfg));
    cfg.rx_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    cfg.tx_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
    cfg.libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD;
    cfg.xdp_flags = p->attach_flags;

    if (ep->cfg.xdp_bind == SX_XDP_BIND_ZEROCOPY ||
        (ep->cfg.xdp_bind == SX_XDP_BIND_AUTO &&
         p->attach_flags != XDP_FLAGS_SKB_MODE)) {
        preferred = XDP_ZEROCOPY;
    }

    cfg.bind_flags = preferred;
    ret = xsk_socket__create(&p->xsk, p->ifname, (uint32_t)ep->cfg.xdp_queue,
                             p->umem, &p->rx, &p->tx, &cfg);
    if (ret != 0 && preferred == XDP_ZEROCOPY &&
        ep->cfg.xdp_bind == SX_XDP_BIND_AUTO) {
        /* 自动回退 copy 模式。 */
        cfg.bind_flags = XDP_COPY;
        ret = xsk_socket__create(&p->xsk, p->ifname,
                                 (uint32_t)ep->cfg.xdp_queue, p->umem, &p->rx,
                                 &p->tx, &cfg);
        if (ret == 0) {
            p->bind_flags = XDP_COPY;
        }
    } else if (ret == 0) {
        p->bind_flags = cfg.bind_flags;
    }
    if (ret != 0) {
        errno = -ret;
        return SX_ERR_BACKEND;
    }
    p->xsk_fd = xsk_socket__fd(p->xsk);
    return SX_OK;
}

/* 打开 XDP 目标对象：优先环境变量 SX_XDP_OBJ，其次编译期内嵌对象。 */
static struct bpf_object *xdp_open_object(sx_endpoint_t *ep) {
    struct bpf_object *obj;
    long err;
    const char *path = getenv("SX_XDP_OBJ");

    if (path != NULL && path[0] != '\0') {
        obj = bpf_object__open_file(path, NULL);
        err = libbpf_get_error(obj);
        if (err != 0) {
            sx_log(ep, SX_LOG_ERROR, "open xdp obj %s failed: %ld", path, err);
            return NULL;
        }
        return obj;
    }
#ifdef SX_XDP_EMBEDDED
    obj = bpf_object__open_mem(sx_xdp_kern_o, sx_xdp_kern_o_len, NULL);
    err = libbpf_get_error(obj);
    if (err != 0) {
        sx_log(ep, SX_LOG_ERROR, "open embedded xdp obj failed: %ld", err);
        return NULL;
    }
    return obj;
#else
    sx_log(ep, SX_LOG_ERROR, "no xdp object (set SX_XDP_OBJ)");
    return NULL;
#endif
}

/* 选择 attach 模式：native 优先，失败回退 generic。 */
static int xdp_attach_program(sx_xdp_priv_t *p, sx_endpoint_t *ep,
                              struct bpf_program *prog) {
    unsigned int modes[2];
    int nmodes = 0;
    int i;

    if (ep->cfg.xdp_mode == SX_XDP_MODE_NATIVE) {
        modes[nmodes++] = XDP_FLAGS_DRV_MODE;
    } else if (ep->cfg.xdp_mode == SX_XDP_MODE_GENERIC) {
        modes[nmodes++] = XDP_FLAGS_SKB_MODE;
    } else {
        modes[nmodes++] = XDP_FLAGS_DRV_MODE;
        modes[nmodes++] = XDP_FLAGS_SKB_MODE;
    }
    for (i = 0; i < nmodes; i++) {
        unsigned int flags = modes[i] | XDP_FLAGS_UPDATE_IF_NOEXIST;
        if (bpf_xdp_attach(p->ifindex, bpf_program__fd(prog), flags, NULL) == 0) {
            p->attach_flags = modes[i];
            p->attached = 1;
            ep->stats.xdp_mode = (modes[i] == XDP_FLAGS_SKB_MODE)
                                     ? SX_XDP_MODE_GENERIC
                                     : SX_XDP_MODE_NATIVE;
            return SX_OK;
        }
    }
    sx_log(ep, SX_LOG_ERROR, "bpf_xdp_attach failed on %s", p->ifname);
    return SX_ERR_BACKEND;
}

/* 加载并 attach XDP 重定向程序。 */
static int xdp_load_program(sx_xdp_priv_t *p, sx_endpoint_t *ep) {
    struct bpf_object *obj;
    struct bpf_program *prog;
    struct bpf_map *map;

    obj = xdp_open_object(ep);
    if (obj == NULL) {
        return SX_ERR_BACKEND;
    }
    if (bpf_object__load(obj) != 0) {
        sx_log(ep, SX_LOG_ERROR, "load xdp obj failed");
        bpf_object__close(obj);
        return SX_ERR_BACKEND;
    }
    prog = bpf_object__find_program_by_name(obj, "sx_xdp_redirect_prog");
    if (prog == NULL) {
        bpf_object__close(obj);
        return SX_ERR_BACKEND;
    }
    p->obj = obj;

    map = bpf_object__find_map_by_name(obj, "xsks_map");
    if (map == NULL) {
        return SX_ERR_BACKEND;
    }
    p->xsk_map_fd = bpf_map__fd(map);
    map = bpf_object__find_map_by_name(obj, "sx_config");
    if (map == NULL) {
        return SX_ERR_BACKEND;
    }
    p->sx_config_map_fd = bpf_map__fd(map);

    /* 选择 attach 模式：native 优先，失败回退 generic。 */
    if (xdp_attach_program(p, ep, prog) != SX_OK) {
        return SX_ERR_BACKEND;
    }

    /* 写入本端 UDP 端口供 XDP 程序匹配。 */
    {
        uint32_t key = 0;
        sx_xdp_config_kern_t val;
        val.port = p->local_port;
        val.flags = 0;
        (void)bpf_map_update_elem(p->sx_config_map_fd, &key, &val, BPF_ANY);
    }
    return SX_OK;
}

static void xdp_close(sx_io_t *io) {
    sx_xdp_priv_t *p;
    if (io == NULL) {
        return;
    }
    p = (sx_xdp_priv_t *)io->priv;
    if (p != NULL) {
        if (p->attached) {
            (void)bpf_xdp_detach(p->ifindex, p->attach_flags, NULL);
            p->attached = 0;
        }
        if (p->xsk != NULL) {
            xsk_socket__delete(p->xsk);
            p->xsk = NULL;
        }
        if (p->umem != NULL) {
            xsk_umem__delete(p->umem);
            p->umem = NULL;
        }
        if (p->umem_area != NULL) {
            free(p->umem_area);
            p->umem_area = NULL;
        }
        if (p->obj != NULL) {
            bpf_object__close(p->obj);
            p->obj = NULL;
        }
        if (p->arp != NULL) {
            sx_arp_destroy(p->arp);
            p->arp = NULL;
        }
        free(p->frame_addr);
        free(p);
    }
    free(io);
}

sx_io_t *sx_io_xdp_create(sx_endpoint_t *ep) {
    sx_io_t *io;
    sx_xdp_priv_t *p;
    struct xsk_umem_config ucfg;
    uint32_t i;
    int ret;

    if (ep == NULL || ep->cfg.ifname == NULL) {
        return NULL;
    }
    /* XDP 程序按目的端口过滤，local_port 必须为非 0，否则永不重定向。 */
    if (ep->cfg.local_port == 0) {
        sx_log(ep, SX_LOG_ERROR, "xdp backend requires local_port != 0");
        return NULL;
    }
    io = (sx_io_t *)calloc(1, sizeof(*io));
    p = (sx_xdp_priv_t *)calloc(1, sizeof(*p));
    if (io == NULL || p == NULL) {
        free(io);
        free(p);
        return NULL;
    }
    p->xsk_fd = -1;
    p->xsk_map_fd = -1;
    p->sx_config_map_fd = -1;
    snprintf(p->ifname, sizeof(p->ifname), "%s", ep->cfg.ifname);
    p->ifindex = (int)if_nametoindex(p->ifname);
    if (p->ifindex == 0) {
        free(io);
        free(p);
        return NULL;
    }
    p->frame_size = ep->cfg.frame_size ? ep->cfg.frame_size
                                       : SX_XDP_FRAME_SIZE_DEFAULT;
    p->num_frames = ep->cfg.umem_frames ? ep->cfg.umem_frames
                                        : SX_XDP_UMEM_FRAMES_DEFAULT;
    if (p->frame_size < 2048) {
        p->frame_size = 2048;
    }

    (void)xdp_query_iface(p->ifname, p->local_mac, &p->local_ip);
    if (ep->cfg.local_ip != 0) {
        p->local_ip = ep->cfg.local_ip;
    }
    p->local_port = ep->cfg.local_port;

    p->frame_addr = (uint64_t *)calloc(p->num_frames, sizeof(uint64_t));
    if (p->frame_addr == NULL) {
        free(io);
        free(p);
        return NULL;
    }
    for (i = 0; i < p->num_frames; i++) {
        p->frame_addr[i] = (uint64_t)i * p->frame_size;
    }
    p->frame_free = p->num_frames;

    p->umem_size = (size_t)p->num_frames * p->frame_size;
    if (posix_memalign(&p->umem_area, 4096, p->umem_size) != 0) {
        free(p->frame_addr);
        free(io);
        free(p);
        return NULL;
    }
    memset(&ucfg, 0, sizeof(ucfg));
    ucfg.fill_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
    ucfg.comp_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    ucfg.frame_size = p->frame_size;
    ucfg.frame_headroom = XSK_UMEM__DEFAULT_FRAME_HEADROOM;
    ucfg.flags = XSK_UMEM__DEFAULT_FLAGS;

    ret = xsk_umem__create(&p->umem, p->umem_area, p->umem_size, &p->fq, &p->cq,
                           &ucfg);
    if (ret != 0) {
        free(p->umem_area);
        free(p->frame_addr);
        free(io);
        free(p);
        return NULL;
    }

    io->name = "xdp";
    io->ep = ep;
    io->priv = p;

    if (xdp_load_program(p, ep) != SX_OK) {
        xdp_close(io);
        return NULL;
    }
    if (xdp_bind_socket(p, ep) != SX_OK) {
        xdp_close(io);
        return NULL;
    }
    io->fd = p->xsk_fd;
    io->sendto = xdp_sendto;
    io->recvfrom = xdp_recvfrom;
    io->poll = xdp_poll;
    io->join_multicast = xdp_join_multicast;
    io->update = xdp_update;
    io->close = xdp_close;

    /* 将 xsk 注册到 xsks_map 对应队列，并回填实际端口。 */
    if (xsk_socket__update_xskmap(p->xsk, p->xsk_map_fd) != 0) {
        xdp_close(io);
        return NULL;
    }
    if (ep->cfg.local_port == 0) {
        ep->cfg.local_port = p->local_port;
    }

    p->arp = sx_arp_create(io);
    if (p->arp == NULL) {
        xdp_close(io);
        return NULL;
    }
    sx_arp_set_local(p->arp, p->local_ip, p->local_mac);

    ep->stats.backend = SX_BACKEND_XDP;
    ep->stats.xdp_bind = (p->bind_flags == XDP_ZEROCOPY) ? SX_XDP_BIND_ZEROCOPY
                                                         : SX_XDP_BIND_COPY;
    sx_log(ep, SX_LOG_INFO, "xdp backend ready if=%s mac=%02x:%02x:%02x:%02x:%02x:%02x ip=%u.%u.%u.%u mode=%s bind=%s",
           p->ifname, p->local_mac[0], p->local_mac[1], p->local_mac[2],
           p->local_mac[3], p->local_mac[4], p->local_mac[5],
           (p->local_ip >> 24) & 0xff, (p->local_ip >> 16) & 0xff,
           (p->local_ip >> 8) & 0xff, p->local_ip & 0xff,
           (p->attach_flags == XDP_FLAGS_SKB_MODE) ? "generic" : "native",
           (p->bind_flags == XDP_ZEROCOPY) ? "zero-copy" : "copy");
    return io;
}
#endif /* !SX_NO_XDP */
