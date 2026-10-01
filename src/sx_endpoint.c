/*
 * sx_endpoint.c - 端点与公共 API
 *
 * 职责：
 *   - 端点生命周期（create/start/stop/destroy）；
 *   - 后端选择（AF_XDP 优先，失败回退内核 UDP）；
 *   - 链路（peer/组）管理，点播/组播/广播三类语义；
 *   - 报文分用：按来源地址路由到 unicast 引擎或 group 引擎；
 *   - 周期驱动：收发、定时器、超时判定；
 *   - 按序投递（回调或拉取队列）与统计。
 *
 * 并发：
 *   - 默认单线程：应用周期性调用 sx_update。
 *   - rx_thread=1 时库启动内部线程调用 sx_update；send/update 用互斥锁保护。
 */
#include "sx_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* 配置与错误串                                                        */
/* ------------------------------------------------------------------ */

void sx_config_init(sx_config_t *cfg) {
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->backend = SX_BACKEND_AUTO;
    cfg->xdp_mode = SX_XDP_MODE_AUTO;
    cfg->xdp_bind = SX_XDP_BIND_AUTO;
    cfg->xdp_queue = 0;
    cfg->conv = SX_DEFAULT_CONV;
    cfg->max_payload = SX_MAX_PAYLOAD_DEFAULT;
    cfg->send_window = 256;
    cfg->recv_window = 1024;
    cfg->group_history = 1024;
    cfg->min_rto_ms = 100;
    cfg->max_rto_ms = 5000;
    cfg->keepalive_ms = 10000;
    cfg->timeout_ms = 30000;
    cfg->umem_frames = 4096;
    cfg->frame_size = 4096;
    cfg->log_level = SX_LOG_WARN;
}

const char *sx_version(void) { return "0.1.0"; }

const char *sx_strerror(int err) {
    switch (err) {
    case SX_OK: return "ok";
    case SX_ERR_INVALID: return "invalid argument";
    case SX_ERR_NOMEM: return "out of memory";
    case SX_ERR_TOO_LARGE: return "payload too large";
    case SX_ERR_NOT_FOUND: return "not found";
    case SX_ERR_EXISTS: return "already exists";
    case SX_ERR_AGAIN: return "try again";
    case SX_ERR_IO: return "io error";
    case SX_ERR_BACKEND: return "backend error";
    case SX_ERR_STATE: return "invalid state";
    case SX_ERR_TIMEOUT: return "timeout";
    case SX_ERR_UNSUPPORTED: return "unsupported";
    case SX_ERR_WINDOW: return "send window full";
    case SX_ERR_CLOSED: return "closed";
    default: return "unknown error";
    }
}

/* ------------------------------------------------------------------ */
/* 内部工具                                                            */
/* ------------------------------------------------------------------ */

void sx_ep_emit(sx_endpoint_t *ep, sx_event_t ev, sx_link_t *link) {
    if (ep != NULL && ep->cfg.on_event != NULL) {
        ep->cfg.on_event(ep, ev, link, ep->cfg.user);
    }
}

void sx_ep_deliver(sx_endpoint_t *ep, sx_link_t *link, uint32_t src_ip,
                   uint16_t src_port, const void *data, size_t len) {
    (void)src_ip;
    (void)src_port;
    if (ep->cfg.on_recv != NULL) {
        ep->cfg.on_recv(ep, link, data, len, ep->cfg.user);
        return;
    }
    /* 拉取模式：入队。 */
    {
        sx_rx_item_t *item = (sx_rx_item_t *)calloc(1, sizeof(*item));
        if (item == NULL) {
            return;
        }
        item->data = (uint8_t *)malloc(len > 0 ? len : 1);
        if (item->data == NULL) {
            free(item);
            return;
        }
        if (len > 0) {
            memcpy(item->data, data, len);
        }
        item->len = len;
        item->link = link;
        item->addr.ip = src_ip;
        item->addr.port = src_port;
        item->addr.scope = link != NULL ? link->scope : SX_SCOPE_UNICAST;
        if (link != NULL) {
            item->addr.ip = link->addr.ip;
            item->addr.port = link->addr.port;
            memcpy(item->addr.mac, link->addr.mac, 6);
        }
        item->next = NULL;
        if (ep->rx_tail != NULL) {
            ep->rx_tail->next = item;
        } else {
            ep->rx_head = item;
        }
        ep->rx_tail = item;
        ep->rx_count++;
    }
}

static void link_destroy(sx_link_t *link) {
    if (link == NULL) {
        return;
    }
    if (link->kcp != NULL) {
        sx_kcp_destroy(link->kcp);
    }
    if (link->group != NULL) {
        sx_group_destroy(link->group);
    }
    free(link);
}

static sx_link_t *link_new(sx_endpoint_t *ep, const sx_addr_t *addr) {
    sx_link_t *link = (sx_link_t *)calloc(1, sizeof(*link));
    if (link == NULL) {
        return NULL;
    }
    link->ep = ep;
    link->addr = *addr;
    link->scope = addr->scope;
    link->conv = ep->cfg.conv;
    link->used = 1;
    link->created_ms = sx_now_ms();
    return link;
}

static int ep_add_link(sx_endpoint_t *ep, sx_link_t *link) {
    if (ep->nlinks == ep->links_cap) {
        size_t cap = ep->links_cap == 0 ? 8 : ep->links_cap * 2;
        sx_link_t **p = (sx_link_t **)realloc(ep->links, cap * sizeof(*p));
        if (p == NULL) {
            return SX_ERR_NOMEM;
        }
        ep->links = p;
        ep->links_cap = cap;
    }
    link->index = (int)ep->nlinks;
    ep->links[ep->nlinks++] = link;
    return SX_OK;
}

static sx_link_t *find_unicast(sx_endpoint_t *ep, uint32_t ip, uint16_t port) {
    size_t i;
    for (i = 0; i < ep->nlinks; i++) {
        sx_link_t *l = ep->links[i];
        if (l != NULL && l->scope == SX_SCOPE_UNICAST && l->addr.ip == ip &&
            l->addr.port == port) {
            return l;
        }
    }
    return NULL;
}

static sx_link_t *find_group(sx_endpoint_t *ep, uint32_t conv, int *is_group) {
    size_t i;
    for (i = 0; i < ep->nlinks; i++) {
        sx_link_t *l = ep->links[i];
        if (l != NULL && l->group != NULL &&
            (l->scope == SX_SCOPE_MULTICAST || l->scope == SX_SCOPE_BROADCAST)) {
            if (l->conv == conv) {
                if (is_group) {
                    *is_group = 1;
                }
                return l;
            }
        }
    }
    return NULL;
}

/* 处理一条收到的 SX 报文。 */
static int ep_process_packet(sx_endpoint_t *ep, uint32_t src_ip,
                             uint16_t src_port, const uint8_t *seg,
                             size_t seg_len) {
    sx_hdr_t h;
    sx_link_t *link;

    if (sx_hdr_decode(seg, seg_len, &h) != SX_OK) {
        ep->stats.dropped++;
        return 0;
    }

    /* 组播/广播：按 conv 路由到组链路（先于单播查找）。 */
    if (ep->nmc > 0) {
        int is_group = 0;
        sx_link_t *g = find_group(ep, h.conv, &is_group);
        if (g != NULL) {
            sx_group_input(g->group, seg, seg_len, src_ip, src_port,
                           sx_now_ms());
            return 1;
        }
    }

    link = find_unicast(ep, src_ip, src_port);
    if (link == NULL) {
        /* 服务端自动学习：conv 匹配则建立 ephemeral unicast 链路。 */
        if (h.conv == ep->cfg.conv) {
            sx_addr_t a;
            memset(&a, 0, sizeof(a));
            a.ip = src_ip;
            a.port = src_port;
            a.scope = SX_SCOPE_UNICAST;
            link = link_new(ep, &a);
            if (link != NULL) {
                link->kcp = sx_kcp_create(ep, link, ep->cfg.conv,
                                          ep->cfg.send_window,
                                          ep->cfg.recv_window);
                if (link->kcp == NULL || ep_add_link(ep, link) != SX_OK) {
                    link_destroy(link);
                    link = NULL;
                } else {
                    sx_log(ep, SX_LOG_INFO,
                           "learned peer %u.%u.%u.%u:%u", (src_ip >> 24) & 0xff,
                           (src_ip >> 16) & 0xff, (src_ip >> 8) & 0xff,
                           src_ip & 0xff, src_port);
                    sx_ep_emit(ep, SX_EVENT_PEER_ADDED, link);
                }
            }
        }
    }
    if (link == NULL || link->kcp == NULL) {
        ep->stats.dropped++;
        return 0;
    }
    sx_kcp_input(link->kcp, seg, seg_len, sx_now_ms());
    return 1;
}

/* 收空底层所有待处理报文。 */
static int ep_drain_rx(sx_endpoint_t *ep) {
    uint8_t buf[SX_HDR_SIZE + SX_ACK_LIST_MAX * 4 + SX_MAX_PAYLOAD_LIMIT];
    int count = 0;
    for (;;) {
        uint32_t sip = 0;
        uint16_t sport = 0;
        size_t len = 0;
        int r = ep->io->recvfrom(ep->io, &sip, &sport, buf, sizeof(buf), &len);
        if (r <= 0) {
            if (r < 0) {
                sx_log(ep, SX_LOG_WARN, "recvfrom error %d", r);
            }
            break;
        }
        count += ep_process_packet(ep, sip, sport, buf, len);
    }
    return count;
}

static void ep_link_timeouts(sx_endpoint_t *ep, uint64_t now) {
    size_t i;
    uint32_t to = ep->cfg.timeout_ms ? ep->cfg.timeout_ms : 30000u;
    for (i = 0; i < ep->nlinks; i++) {
        sx_link_t *l = ep->links[i];
        if (l == NULL || l->kcp == NULL) {
            continue;
        }
        if (l->kcp->up && now - l->kcp->last_recv_ms > to) {
            l->kcp->up = 0;
            sx_ep_emit(ep, SX_EVENT_PEER_TIMEOUT, l);
            sx_ep_emit(ep, SX_EVENT_PEER_DOWN, l);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 生命周期                                                            */
/* ------------------------------------------------------------------ */

sx_endpoint_t *sx_create(const sx_config_t *cfg) {
    sx_endpoint_t *ep;
    if (cfg == NULL) {
        return NULL;
    }
    ep = (sx_endpoint_t *)calloc(1, sizeof(*ep));
    if (ep == NULL) {
        return NULL;
    }
    ep->cfg = *cfg;
    if (ep->cfg.max_payload == 0) {
        ep->cfg.max_payload = SX_MAX_PAYLOAD_DEFAULT;
    }
    if (ep->cfg.max_payload > SX_MAX_PAYLOAD_LIMIT) {
        ep->cfg.max_payload = SX_MAX_PAYLOAD_LIMIT;
    }
    if (ep->cfg.conv == 0) {
        ep->cfg.conv = SX_DEFAULT_CONV;
    }
    {
        pthread_mutexattr_t attr;
        /* 递归锁：允许在 rx_thread 模式下，投递回调内再次调用 sx_send。 */
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&ep->lock, &attr);
        pthread_mutexattr_destroy(&attr);
    }
    return ep;
}

static sx_io_t *create_backend(sx_endpoint_t *ep, int *chosen) {
    sx_io_t *io = NULL;

    if (ep->cfg.backend == SX_BACKEND_XDP) {
        if (ep->cfg.ifname == NULL) {
            return NULL;
        }
        io = sx_io_xdp_create(ep);
        *chosen = SX_BACKEND_XDP;
        return io;
    }
    if (ep->cfg.backend == SX_BACKEND_UDP) {
        io = sx_io_udp_create(ep);
        *chosen = SX_BACKEND_UDP;
        return io;
    }
    /* AUTO：优先 XDP。 */
    if (ep->cfg.ifname != NULL && ep->cfg.ifname[0] != '\0') {
        io = sx_io_xdp_create(ep);
        if (io != NULL) {
            *chosen = SX_BACKEND_XDP;
            return io;
        }
        sx_log(ep, SX_LOG_WARN, "xdp backend unavailable, fallback to udp");
    }
    io = sx_io_udp_create(ep);
    *chosen = SX_BACKEND_UDP;
    return io;
}

static void *ep_thread_main(void *arg) {
    sx_endpoint_t *ep = (sx_endpoint_t *)arg;
    while (!ep->stop_flag) {
        pthread_mutex_lock(&ep->lock);
        (void)sx_update(ep, 0);
        pthread_mutex_unlock(&ep->lock);
        usleep(2000);
    }
    return NULL;
}

int sx_start(sx_endpoint_t *ep) {
    int chosen = SX_BACKEND_UDP;

    if (ep == NULL) {
        return SX_ERR_INVALID;
    }
    if (ep->running) {
        return SX_ERR_STATE;
    }
    ep->io = create_backend(ep, &chosen);
    if (ep->io == NULL) {
        return SX_ERR_BACKEND;
    }
    ep->stats.backend = (uint32_t)chosen;
    ep->started_ms = sx_now_ms();
    ep->running = 1;
    ep->stop_flag = 0;
    sx_ep_emit(ep, SX_EVENT_STARTED, NULL);
    sx_ep_emit(ep, SX_EVENT_BACKEND_CHOSEN, NULL);
    sx_log(ep, SX_LOG_INFO, "started backend=%s local_port=%u",
           ep->io->name, ep->cfg.local_port);

    if (ep->cfg.rx_thread) {
        if (pthread_create(&ep->thread, NULL, ep_thread_main, ep) == 0) {
            ep->thread_started = 1;
        }
    }
    return SX_OK;
}

int sx_stop(sx_endpoint_t *ep) {
    size_t i;
    if (ep == NULL) {
        return SX_ERR_INVALID;
    }
    if (!ep->running) {
        return SX_OK;
    }
    ep->stop_flag = 1;
    if (ep->thread_started) {
        pthread_join(ep->thread, NULL);
        ep->thread_started = 0;
    }
    if (ep->io != NULL) {
        ep->io->close(ep->io);
        ep->io = NULL;
    }
    for (i = 0; i < ep->nlinks; i++) {
        link_destroy(ep->links[i]);
    }
    free(ep->links);
    ep->links = NULL;
    ep->nlinks = 0;
    ep->links_cap = 0;
    ep->nmc = 0;
    ep->bcast_link = NULL;
    /* 清空拉取队列。 */
    {
        sx_rx_item_t *it = ep->rx_head;
        while (it != NULL) {
            sx_rx_item_t *n = it->next;
            free(it->data);
            free(it);
            it = n;
        }
        ep->rx_head = ep->rx_tail = NULL;
        ep->rx_count = 0;
    }
    ep->running = 0;
    return SX_OK;
}

void sx_destroy(sx_endpoint_t *ep) {
    if (ep == NULL) {
        return;
    }
    (void)sx_stop(ep);
    pthread_mutex_destroy(&ep->lock);
    free(ep);
}

/* ------------------------------------------------------------------ */
/* 链路管理                                                            */
/* ------------------------------------------------------------------ */

static sx_link_t *add_link_common(sx_endpoint_t *ep, uint32_t ip, uint16_t port,
                                  sx_scope_t scope) {
    sx_addr_t addr;
    sx_link_t *link;

    if (ep == NULL) {
        return NULL;
    }
    if (scope == SX_SCOPE_UNICAST) {
        link = find_unicast(ep, ip, port);
        if (link != NULL) {
            return link;
        }
    }
    memset(&addr, 0, sizeof(addr));
    addr.ip = ip;
    addr.port = port;
    addr.scope = scope;

    link = link_new(ep, &addr);
    if (link == NULL) {
        return NULL;
    }
    if (scope == SX_SCOPE_UNICAST) {
        link->kcp = sx_kcp_create(ep, link, ep->cfg.conv, ep->cfg.send_window,
                                  ep->cfg.recv_window);
        if (link->kcp == NULL) {
            link_destroy(link);
            return NULL;
        }
    } else {
        link->group = sx_group_create(ep, link, ep->cfg.conv,
                                      ep->cfg.group_history,
                                      SX_GROUP_REORDER);
        if (link->group == NULL) {
            link_destroy(link);
            return NULL;
        }
    }
    if (ep_add_link(ep, link) != SX_OK) {
        link_destroy(link);
        return NULL;
    }
    if (scope == SX_SCOPE_MULTICAST) {
        sx_link_t **p = (sx_link_t **)realloc(
            ep->mc_links, (ep->nmc + 1) * sizeof(sx_link_t *));
        if (p != NULL) {
            ep->mc_links = p;
            ep->mc_links[ep->nmc++] = link;
        }
        if (ep->io != NULL && ep->io->join_multicast != NULL) {
            (void)ep->io->join_multicast(ep->io, ip, ep->cfg.local_ip);
        }
    } else if (scope == SX_SCOPE_BROADCAST) {
        ep->bcast_link = link;
    }
    sx_ep_emit(ep, SX_EVENT_PEER_ADDED, link);
    return link;
}

sx_link_t *sx_add_unicast(sx_endpoint_t *ep, uint32_t ip, uint16_t port) {
    return add_link_common(ep, ip, port, SX_SCOPE_UNICAST);
}

sx_link_t *sx_add_multicast(sx_endpoint_t *ep, uint32_t group_ip,
                            uint16_t port) {
    return add_link_common(ep, group_ip, port, SX_SCOPE_MULTICAST);
}

sx_link_t *sx_add_broadcast(sx_endpoint_t *ep, uint32_t bcast_ip,
                            uint16_t port) {
    return add_link_common(ep, bcast_ip, port, SX_SCOPE_BROADCAST);
}

int sx_link_remove(sx_endpoint_t *ep, sx_link_t *link) {
    size_t i;
    if (ep == NULL || link == NULL) {
        return SX_ERR_INVALID;
    }
    for (i = 0; i < ep->nlinks; i++) {
        if (ep->links[i] == link) {
            ep->links[i] = ep->links[ep->nlinks - 1];
            ep->nlinks--;
            if (ep->bcast_link == link) {
                ep->bcast_link = NULL;
            }
            for (i = 0; i < ep->nmc; i++) {
                if (ep->mc_links[i] == link) {
                    ep->mc_links[i] = ep->mc_links[ep->nmc - 1];
                    ep->nmc--;
                    break;
                }
            }
            link_destroy(link);
            return SX_OK;
        }
    }
    return SX_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* 发送与接收                                                          */
/* ------------------------------------------------------------------ */

int64_t sx_send(sx_endpoint_t *ep, sx_link_t *link, const void *data,
                size_t len) {
    int64_t r;
    if (ep == NULL || link == NULL) {
        return SX_ERR_INVALID;
    }
    if (len > ep->cfg.max_payload) {
        return SX_ERR_TOO_LARGE;
    }
    pthread_mutex_lock(&ep->lock);
    if (!ep->running) {
        pthread_mutex_unlock(&ep->lock);
        return SX_ERR_CLOSED;
    }
    if (link->scope == SX_SCOPE_UNICAST) {
        r = sx_kcp_send(link->kcp, data, len);
    } else {
        r = sx_group_send(link->group, data, len);
    }
    pthread_mutex_unlock(&ep->lock);
    return r;
}

int64_t sx_send_to(sx_endpoint_t *ep, uint32_t ip, uint16_t port,
                   const void *data, size_t len) {
    sx_link_t *link;
    if (ep == NULL) {
        return SX_ERR_INVALID;
    }
    link = find_unicast(ep, ip, port);
    if (link == NULL) {
        link = sx_add_unicast(ep, ip, port);
        if (link == NULL) {
            return SX_ERR_NOMEM;
        }
    }
    return sx_send(ep, link, data, len);
}

ssize_t sx_recv(sx_endpoint_t *ep, void *buf, size_t cap, sx_addr_t *src_out,
                sx_link_t **link_out) {
    sx_rx_item_t *item;
    size_t copy;
    if (ep == NULL || buf == NULL) {
        return SX_ERR_INVALID;
    }
    pthread_mutex_lock(&ep->lock);
    item = ep->rx_head;
    if (item == NULL) {
        pthread_mutex_unlock(&ep->lock);
        return SX_ERR_AGAIN;
    }
    ep->rx_head = item->next;
    if (ep->rx_head == NULL) {
        ep->rx_tail = NULL;
    }
    ep->rx_count--;
    pthread_mutex_unlock(&ep->lock);

    copy = item->len < cap ? item->len : cap;
    memcpy(buf, item->data, copy);
    if (src_out != NULL) {
        *src_out = item->addr;
    }
    if (link_out != NULL) {
        *link_out = item->link;
    }
    free(item->data);
    free(item);
    return (ssize_t)copy;
}

/* ------------------------------------------------------------------ */
/* 驱动                                                                */
/* ------------------------------------------------------------------ */

int sx_update(sx_endpoint_t *ep, uint64_t now_ms) {
    size_t i;
    uint64_t now;
    int processed;

    if (ep == NULL || ep->io == NULL) {
        return SX_ERR_INVALID;
    }
    now = now_ms != 0 ? now_ms : sx_now_ms();

    processed = ep_drain_rx(ep);

    for (i = 0; i < ep->nlinks; i++) {
        sx_link_t *l = ep->links[i];
        if (l == NULL) {
            continue;
        }
        if (l->kcp != NULL) {
            sx_kcp_update(l->kcp, now);
        }
        if (l->group != NULL) {
            sx_group_update(l->group, now);
        }
    }

    /* ARP 老化/重试。 */
    if (ep->io->name != NULL && strcmp(ep->io->name, "xdp") == 0) {
        /* ARP 实例私有于 xdp 后端，由后端在收发路径维护；
         * 此处仅驱动超时逻辑通过 sx_io_xdp_send_frame 的调用链完成。 */
    }

    ep_link_timeouts(ep, now);
    return processed;
}

int sx_run(sx_endpoint_t *ep, int timeout_ms) {
    uint64_t start;
    if (ep == NULL) {
        return SX_ERR_INVALID;
    }
    start = sx_now_ms();
    while (!ep->stop_flag) {
        int r = sx_update(ep, 0);
        if (r < 0) {
            return r;
        }
        if (ep->io != NULL && ep->io->poll != NULL) {
            (void)ep->io->poll(ep->io, 2);
        } else {
            usleep(2000);
        }
        if (timeout_ms >= 0 &&
            (int)(sx_now_ms() - start) >= timeout_ms) {
            break;
        }
    }
    return SX_OK;
}

/* ------------------------------------------------------------------ */
/* 属性与统计                                                          */
/* ------------------------------------------------------------------ */

int sx_get_stats(sx_endpoint_t *ep, sx_stats_t *out) {
    size_t i;
    if (ep == NULL || out == NULL) {
        return SX_ERR_INVALID;
    }
    *out = ep->stats;
    for (i = 0; i < ep->nlinks; i++) {
        sx_link_t *l = ep->links[i];
        if (l != NULL && l->kcp != NULL) {
            out->snd_wnd = SX_MIN(l->kcp->snd_wnd, l->kcp->cwnd);
            out->per_peer_wnd = l->kcp->snd_wnd;
            out->cwnd = l->kcp->cwnd;
            out->rtt_ms = l->kcp->srtt;
            out->rto_ms = l->kcp->rto_ms;
            break;
        }
    }
    return SX_OK;
}

int sx_link_addr(const sx_link_t *link, sx_addr_t *out) {
    if (link == NULL || out == NULL) {
        return SX_ERR_INVALID;
    }
    *out = link->addr;
    return SX_OK;
}

int sx_link_is_up(const sx_link_t *link) {
    if (link == NULL) {
        return 0;
    }
    if (link->kcp != NULL) {
        return link->kcp->up;
    }
    return 1; /* 组播/广播无连接状态，视为可用 */
}

uint32_t sx_link_wnd(const sx_link_t *link) {
    if (link == NULL || link->kcp == NULL) {
        return 0;
    }
    return SX_MIN(link->kcp->snd_wnd, link->kcp->cwnd);
}

size_t sx_max_payload(const sx_endpoint_t *ep) {
    if (ep == NULL) {
        return 0;
    }
    return ep->cfg.max_payload;
}
