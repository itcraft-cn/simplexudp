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

static void link_ref(sx_link_t *link);
static void link_unref(sx_link_t *link);

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
    cfg->max_message = 64u * 1024u;
    cfg->send_window = 256;
    cfg->recv_window = 1024;
    cfg->group_history = 1024;
    cfg->group_reorder = 256;
    cfg->group_max_src = 32;
    cfg->max_peers = 1024;
    cfg->rx_batch = 4096;
    cfg->pool_blocks = 0;
    cfg->min_rto_ms = 100;
    cfg->max_rto_ms = 5000;
    cfg->rx_ts_mode = SX_RX_TS_OFF;
    cfg->cc_mode = SX_CC_ON;
    cfg->large_message_burst = 1;
    cfg->rto_backoff = 1;
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
    sx_event_item_t *it;
    if (ep == NULL || ep->cfg.on_event == NULL) {
        return;
    }
    it = (sx_event_item_t *)calloc(1, sizeof(*it));
    if (it == NULL) {
        return;
    }
    it->event = ev;
    it->link = link;
    pthread_mutex_lock(&ep->lock);
    if (link != NULL) {
        link_ref(link);
    }
    if (ep->evt_tail != NULL) {
        ep->evt_tail->next = it;
    } else {
        ep->evt_head = it;
    }
    ep->evt_tail = it;
    pthread_mutex_unlock(&ep->lock);
}

void sx_ep_flush_events(sx_endpoint_t *ep) {
    sx_event_item_t *batch;
    if (ep == NULL) {
        return;
    }
    pthread_mutex_lock(&ep->lock);
    batch = ep->evt_head;
    ep->evt_head = NULL;
    ep->evt_tail = NULL;
    pthread_mutex_unlock(&ep->lock);
    while (batch != NULL) {
        sx_event_item_t *next = batch->next;
        if (ep->cfg.on_event != NULL) {
            ep->cfg.on_event(ep, batch->event, batch->link, ep->cfg.user);
        }
        link_unref(batch->link);
        free(batch);
        batch = next;
    }
}

/* 把一条消息复制入队（数据来自内存池）。head/tail/count 由调用方指定，
 * 使拉取队列与回调队列共用同一套入队逻辑。 */
static void ep_enqueue(sx_endpoint_t *ep, sx_rx_item_t **head,
                       sx_rx_item_t **tail, size_t *count, sx_link_t *link,
                       uint32_t src_ip, uint16_t src_port, const void *data,
                       size_t len, const sx_pkt_info_t *info) {
    sx_rx_item_t *item = (sx_rx_item_t *)calloc(1, sizeof(*item));
    if (item == NULL) {
        return;
    }
    if (len > 0) {
        item->data = (uint8_t *)sx_pool_alloc(&ep->pool, len);
        if (item->data == NULL) {
            free(item);
            return;
        }
        memcpy(item->data, data, len);
    }
    item->len = len;
    item->link = link;
    item->addr.ip = link != NULL ? link->addr.ip : src_ip;
    item->addr.port = link != NULL ? link->addr.port : src_port;
    item->addr.scope = link != NULL ? link->scope : SX_SCOPE_UNICAST;
    if (link != NULL) {
        memcpy(item->addr.mac, link->addr.mac, 6);
    }
    if (info != NULL) {
        item->info = *info;
    }
    if (link != NULL) {
        link_ref(link);
    }
    item->next = NULL;
    if (*tail != NULL) {
        (*tail)->next = item;
    } else {
        *head = item;
    }
    *tail = item;
    (*count)++;
}

/* 取出回调队列，解锁后逐条调用 on_recv（数据来自内存池，回调后归还）。 */
void sx_ep_flush(sx_endpoint_t *ep) {
    sx_rx_item_t *batch;
    if (ep == NULL) {
        return;
    }
    pthread_mutex_lock(&ep->lock);
    batch = ep->cb_head;
    ep->cb_head = NULL;
    ep->cb_tail = NULL;
    ep->cb_count = 0;
    pthread_mutex_unlock(&ep->lock);

    while (batch != NULL) {
        sx_rx_item_t *next = batch->next;
        if (ep->cfg.on_recv_ex != NULL) {
            ep->cfg.on_recv_ex(ep, batch->link, batch->data, batch->len,
                               &batch->info, ep->cfg.user);
        } else if (ep->cfg.on_recv != NULL) {
            ep->cfg.on_recv(ep, batch->link, batch->data, batch->len,
                            ep->cfg.user);
        }
        sx_pool_free(&ep->pool, batch->data);
        link_unref(batch->link);
        free(batch);
        batch = next;
    }
}

void sx_ep_deliver_info(sx_endpoint_t *ep, sx_link_t *link, uint32_t src_ip,
                        uint16_t src_port, const void *data, size_t len,
                        const sx_pkt_info_t *info) {
    /* 回调模式：先入回调队列，由 sx_update 在解锁后统一调用，避免回调持锁。 */
    if (ep->cfg.on_recv_ex != NULL || ep->cfg.on_recv != NULL) {
        ep_enqueue(ep, &ep->cb_head, &ep->cb_tail, &ep->cb_count, link, src_ip,
                   src_port, data, len, info);
        return;
    }
    /* 拉取模式：入拉取队列。 */
    ep_enqueue(ep, &ep->rx_head, &ep->rx_tail, &ep->rx_count, link, src_ip,
               src_port, data, len, info);
}

void sx_ep_deliver(sx_endpoint_t *ep, sx_link_t *link, uint32_t src_ip,
                   uint16_t src_port, const void *data, size_t len) {
    sx_ep_deliver_info(ep, link, src_ip, src_port, data, len, NULL);
}

static void link_ref(sx_link_t *link) {
    if (link != NULL) {
        (void)__sync_add_and_fetch(&link->refcnt, 1);
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

static void link_unref(sx_link_t *link) {
    if (link == NULL) {
        return;
    }
    /* 引用计数可能被多个 sx_update/flush 线程并发修改。 */
    if (__sync_sub_and_fetch(&link->refcnt, 1) == 0 && link->heap) {
        link_destroy(link);
    }
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
    link->refcnt = 1;
    link->heap = 1;
    link->created_ms = sx_now_ms();
    return link;
}

static int ep_add_link(sx_endpoint_t *ep, sx_link_t *link) {
    uint32_t max_peers = ep->cfg.max_peers ? ep->cfg.max_peers : SX_MAX_PEERS;
    if (ep->nlinks >= max_peers) {
        sx_log(ep, SX_LOG_WARN,
               "peer limit %u reached, refuse %u.%u.%u.%u:%u", max_peers,
               (link->addr.ip >> 24) & 0xff, (link->addr.ip >> 16) & 0xff,
               (link->addr.ip >> 8) & 0xff, link->addr.ip & 0xff,
               link->addr.port);
        return SX_ERR_NOMEM;
    }
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
                             size_t seg_len, const sx_pkt_info_t *info) {
    sx_hdr_t h;
    sx_link_t *link;

    if (sx_hdr_decode(seg, seg_len, &h) != SX_OK) {
        ep->stats.dropped++;
        sx_log(ep, SX_LOG_DEBUG, "ep drop decode len=%zu from %u.%u.%u.%u:%u",
               seg_len, (src_ip >> 24) & 0xff, (src_ip >> 16) & 0xff,
               (src_ip >> 8) & 0xff, src_ip & 0xff, src_port);
        return 0;
    }

    /* 先匹配已建立的单播链路，避免组播/单播共用 conv 时把单播报文送进组引擎。 */
    link = find_unicast(ep, src_ip, src_port);
    if (link != NULL && link->kcp != NULL) {
        sx_kcp_input_info(link->kcp, seg, seg_len, sx_now_ms(), info);
        return 1;
    }
    /* 未命中单播时再按 conv 路由到组链路。 */
    if (ep->nmc > 0 || ep->bcast_link != NULL) {
        int is_group = 0;
        sx_link_t *g = find_group(ep, h.conv, &is_group);
        if (g != NULL) {
            sx_group_input_info(g->group, seg, seg_len, src_ip, src_port,
                                sx_now_ms(), info);
            return 1;
        }
        sx_log(ep, SX_LOG_DEBUG,
               "ep: group links=%zu but conv=%u no match (from %u.%u.%u.%u:%u)",
               ep->nmc, h.conv, (src_ip >> 24) & 0xff, (src_ip >> 16) & 0xff,
               (src_ip >> 8) & 0xff, src_ip & 0xff, src_port);
    }

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
        sx_log(ep, SX_LOG_DEBUG,
               "ep: no link for conv=%u from %u.%u.%u.%u:%u (links=%zu)",
               h.conv, (src_ip >> 24) & 0xff, (src_ip >> 16) & 0xff,
               (src_ip >> 8) & 0xff, src_ip & 0xff, src_port, ep->nlinks);
        return 0;
    }
    sx_kcp_input_info(link->kcp, seg, seg_len, sx_now_ms(), info);
    return 1;
}

/* 收空底层所有待处理报文。 */
static int ep_drain_rx(sx_endpoint_t *ep) {
    uint8_t buf[SX_HDR_SIZE + SX_ACK_LIST_MAX * 4 + SX_MAX_PAYLOAD_LIMIT];
    int count = 0;
    uint32_t batch = ep->cfg.rx_batch ? ep->cfg.rx_batch : SX_RX_BATCH_MAX;
    uint32_t guard;
    for (guard = 0; guard < batch; guard++) {
        uint32_t sip = 0;
        uint16_t sport = 0;
        size_t len = 0;
        sx_pkt_info_t info;
        int r;
        memset(&info, 0, sizeof(info));
        r = ep->io->recvfrom(ep->io, &sip, &sport, buf, sizeof(buf), &len,
                             &info);
        if (r <= 0) {
            if (r < 0) {
                sx_log(ep, SX_LOG_WARN, "recvfrom error %d", r);
            }
            break;
        }
        count += ep_process_packet(ep, sip, sport, buf, len, &info);
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
    /* 本端 id 种子随机化：进程重启后 peer_id 不与旧会话重复，
     * 对端才能据此识别会话重建。 */
    ep->next_local_id = (uint32_t)(sx_now_ms() ^ ((uint64_t)getpid() << 20));
    ep->cfg = *cfg;
    if (ep->cfg.max_payload == 0) {
        ep->cfg.max_payload = SX_MAX_PAYLOAD_DEFAULT;
    }
    if (ep->cfg.max_payload > SX_MAX_PAYLOAD_LIMIT) {
        ep->cfg.max_payload = SX_MAX_PAYLOAD_LIMIT;
    }
    if (ep->cfg.max_message == 0) {
        ep->cfg.max_message = 64u * 1024u;
    }
    if (ep->cfg.max_message > SX_MAX_MESSAGE_LIMIT) {
        ep->cfg.max_message = SX_MAX_MESSAGE_LIMIT;
    }
    if (ep->cfg.max_message < ep->cfg.max_payload) {
        ep->cfg.max_message = ep->cfg.max_payload;
    }
    /* 自动分片要求一条消息的所有分片能放入窗口；按 max_message 推导硬下限。
     * 用户显式设置更大的窗口时保持更大的值。 */
    {
        size_t frag_payload =
            ep->cfg.max_payload > SX_FRAG_HDR_SIZE
                ? ep->cfg.max_payload - SX_FRAG_HDR_SIZE
                : 1u;
        uint32_t need =
            (uint32_t)((ep->cfg.max_message + frag_payload - 1u) / frag_payload);
        if (need < 1u) {
            need = 1u;
        }
        if (need < SX_SEND_WINDOW_HARD_MIN) {
            need = SX_SEND_WINDOW_HARD_MIN;
        }
        if (need > SX_SEND_WINDOW_LIMIT) {
            need = SX_SEND_WINDOW_LIMIT;
        }
        if (ep->cfg.send_window < need) {
            ep->cfg.send_window = need;
        }
        if (ep->cfg.recv_window < need) {
            ep->cfg.recv_window = need;
        }
        if (ep->cfg.send_window > SX_SEND_WINDOW_LIMIT) {
            ep->cfg.send_window = SX_SEND_WINDOW_LIMIT;
        }
        if (ep->cfg.recv_window > SX_SEND_WINDOW_LIMIT) {
            ep->cfg.recv_window = SX_SEND_WINDOW_LIMIT;
        }
        /* 组播/广播的重排窗口与发送历史也要至少覆盖一条最大消息的分片数。 */
        if (ep->cfg.group_reorder < need) {
            ep->cfg.group_reorder = need;
        }
        if (ep->cfg.group_history < need) {
            ep->cfg.group_history = need;
        }
    }
    if (ep->cfg.conv == 0) {
        ep->cfg.conv = SX_DEFAULT_CONV;
    }
    {
        pthread_mutexattr_t attr;
        /* 递归锁：允许在投递回调内再次调用 sx_send。 */
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&ep->lock, &attr);
        pthread_mutexattr_destroy(&attr);
    }
    pthread_mutex_init(&ep->log_lock, NULL);

    /* 私有化 ifname，避免调用方释放原字符串导致悬垂。 */
    if (cfg->ifname != NULL && cfg->ifname[0] != '\0') {
        ep->ifname_owned = strdup(cfg->ifname);
        if (ep->ifname_owned == NULL) {
            pthread_mutex_destroy(&ep->log_lock);
            pthread_mutex_destroy(&ep->lock);
            free(ep);
            return NULL;
        }
        ep->cfg.ifname = ep->ifname_owned;
    } else {
        ep->cfg.ifname = NULL;
    }

    /* 热路径内存池：默认按发送/接收窗口推导块数。初始化失败不致命
     * （sx_pool_alloc 会回退 malloc）。 */
    {
        uint32_t bsz = (uint32_t)(ep->cfg.max_payload ? ep->cfg.max_payload : 1);
        uint32_t nblocks = ep->cfg.pool_blocks;
        if (nblocks == 0) {
            uint32_t sw = ep->cfg.send_window ? ep->cfg.send_window : 256;
            uint32_t rw = ep->cfg.recv_window ? ep->cfg.recv_window : 1024;
            nblocks = sw * 2 + rw + 128;
        }
        (void)sx_pool_init(&ep->pool, bsz, nblocks);
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
        /* 记录自动回退，便于现场区分“本来就在 UDP”与“XDP 失败降级”。 */
        ep->stats.backend_fallback = 1;
        sx_log(ep, SX_LOG_WARN, "xdp backend unavailable, fallback to udp");
    }
    io = sx_io_udp_create(ep);
    *chosen = SX_BACKEND_UDP;
    return io;
}

/* 把 start 前登记的静态 ARP 应用到后端。UDP 后端无二层发送路径，忽略并提示。 */
static void ep_apply_static_arps(sx_endpoint_t *ep) {
    size_t i;

    if (ep == NULL || ep->nstatic_arps == 0) {
        return;
    }
    if (ep->io == NULL || ep->io->set_static_arp == NULL) {
        sx_log(ep, SX_LOG_INFO,
               "static arp entries=%zu ignored by backend=%s",
               ep->nstatic_arps, ep->io != NULL ? ep->io->name : "none");
        return;
    }
    for (i = 0; i < ep->nstatic_arps; i++) {
        const sx_static_arp_t *a = &ep->static_arps[i];
        if (ep->io->set_static_arp(ep->io, a->ip, a->mac) != SX_OK) {
            sx_log(ep, SX_LOG_WARN, "static arp apply failed for %u.%u.%u.%u",
                   (a->ip >> 24) & 0xff, (a->ip >> 16) & 0xff,
                   (a->ip >> 8) & 0xff, a->ip & 0xff);
        }
    }
    sx_log(ep, SX_LOG_INFO, "static arp entries=%zu applied", ep->nstatic_arps);
}

static void *ep_thread_main(void *arg) {
    sx_endpoint_t *ep = (sx_endpoint_t *)arg;
    while (!ep->stop_flag) {
        /* 先用 poll 等待可读（有数据立即唤醒，否则最多 2ms），再驱动 update，
         * 避免纯轮询带来的固定延迟。sx_update 内部加锁，可与其他线程并发。 */
        (void)sx_wait(ep, 2);
        (void)sx_update(ep, 0);
    }
    return NULL;
}

int sx_start(sx_endpoint_t *ep) {
    int chosen = SX_BACKEND_UDP;
    size_t i;

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
    ep_apply_static_arps(ep);
    /* start 前添加的组播链路在此时统一入组。 */
    for (i = 0; i < ep->nmc; i++) {
        sx_link_t *l = ep->mc_links[i];
        if (l != NULL && l->scope == SX_SCOPE_MULTICAST &&
            ep->io->join_multicast != NULL) {
            (void)ep->io->join_multicast(ep->io, l->addr.ip,
                                         ep->cfg.multicast_if);
        }
    }
    sx_ep_emit(ep, SX_EVENT_STARTED, NULL);
    sx_ep_emit(ep, SX_EVENT_BACKEND_CHOSEN, NULL);
    sx_ep_flush_events(ep);
    sx_log(ep, SX_LOG_INFO,
           "started backend=%s local_port=%u rx_ts_mode=%u rx_ts_status=%d "
           "rx_ts_clock=%u",
           ep->io->name, ep->cfg.local_port, ep->stats.rx_ts_mode,
           (int)ep->stats.rx_ts_status, ep->stats.rx_ts_clock_id);

    if (ep->cfg.rx_thread) {
        if (pthread_create(&ep->thread, NULL, ep_thread_main, ep) == 0) {
            ep->thread_started = 1;
        }
    }
    return SX_OK;
}

int sx_stop(sx_endpoint_t *ep) {
    size_t i;
    size_t nlinks;
    sx_io_t *io;
    sx_link_t **links;
    sx_link_t **mc_links;
    sx_rx_item_t *rx;
    sx_rx_item_t *cb;
    sx_event_item_t *evt;

    if (ep == NULL) {
        return SX_ERR_INVALID;
    }
    /* 先通知线程退出，再在端点锁内摘除资源；避免 sx_update 正在使用 io/link。 */
    ep->stop_flag = 1;
    if (ep->thread_started) {
        pthread_join(ep->thread, NULL);
        ep->thread_started = 0;
    }

    pthread_mutex_lock(&ep->lock);
    io = ep->io;
    ep->io = NULL;
    links = ep->links;
    nlinks = ep->nlinks;
    ep->links = NULL;
    ep->nlinks = 0;
    ep->links_cap = 0;
    mc_links = ep->mc_links;
    ep->mc_links = NULL;
    ep->nmc = 0;
    ep->bcast_link = NULL;
    rx = ep->rx_head;
    ep->rx_head = ep->rx_tail = NULL;
    ep->rx_count = 0;
    cb = ep->cb_head;
    ep->cb_head = ep->cb_tail = NULL;
    ep->cb_count = 0;
    evt = ep->evt_head;
    ep->evt_head = ep->evt_tail = NULL;
    ep->running = 0;
    pthread_mutex_unlock(&ep->lock);

    if (io != NULL) {
        io->close(io);
    }
    for (i = 0; i < nlinks; i++) {
        link_unref(links[i]);
    }
    free(links);
    free(mc_links);
    while (rx != NULL) {
        sx_rx_item_t *n = rx->next;
        sx_pool_free(&ep->pool, rx->data);
        link_unref(rx->link);
        free(rx);
        rx = n;
    }
    while (cb != NULL) {
        sx_rx_item_t *n = cb->next;
        sx_pool_free(&ep->pool, cb->data);
        link_unref(cb->link);
        free(cb);
        cb = n;
    }
    while (evt != NULL) {
        sx_event_item_t *n = evt->next;
        link_unref(evt->link);
        free(evt);
        evt = n;
    }
    return SX_OK;
}

void sx_destroy(sx_endpoint_t *ep) {
    if (ep == NULL) {
        return;
    }
    (void)sx_stop(ep);
    sx_pool_destroy(&ep->pool);
    free(ep->static_arps);
    free(ep->ifname_owned);
    pthread_mutex_destroy(&ep->log_lock);
    pthread_mutex_destroy(&ep->lock);
    free(ep);
}

/* ------------------------------------------------------------------ */
/* 链路管理                                                            */
/* ------------------------------------------------------------------ */

/* 登记静态 ARP（调用方须持有 ep->lock）。若后端已就绪则立即应用。
 * 同一 IP 重复登记时覆盖旧 MAC，避免陈旧映射残留。 */
static int ep_static_arp_add_locked(sx_endpoint_t *ep, uint32_t ip,
                                    const uint8_t mac[6]) {
    size_t i;

    for (i = 0; i < ep->nstatic_arps; i++) {
        if (ep->static_arps[i].ip == ip) {
            memcpy(ep->static_arps[i].mac, mac, 6);
            if (ep->io != NULL && ep->io->set_static_arp != NULL) {
                return ep->io->set_static_arp(ep->io, ip, mac);
            }
            return SX_OK;
        }
    }
    if (ep->nstatic_arps == ep->static_arps_cap) {
        size_t cap = ep->static_arps_cap ? ep->static_arps_cap * 2u : 8u;
        sx_static_arp_t *p = (sx_static_arp_t *)realloc(
            ep->static_arps, cap * sizeof(*p));
        if (p == NULL) {
            return SX_ERR_NOMEM;
        }
        ep->static_arps = p;
        ep->static_arps_cap = cap;
    }
    ep->static_arps[ep->nstatic_arps].ip = ip;
    memcpy(ep->static_arps[ep->nstatic_arps].mac, mac, 6);
    ep->nstatic_arps++;
    if (ep->io != NULL && ep->io->set_static_arp != NULL) {
        return ep->io->set_static_arp(ep->io, ip, mac);
    }
    return SX_OK;
}

static sx_link_t *add_link_common(sx_endpoint_t *ep, uint32_t ip, uint16_t port,
                                  sx_scope_t scope, const uint8_t *mac) {
    sx_addr_t addr;
    sx_link_t *link = NULL;

    if (ep == NULL) {
        return NULL;
    }
    pthread_mutex_lock(&ep->lock);

    if (scope == SX_SCOPE_UNICAST) {
        link = find_unicast(ep, ip, port);
        if (link != NULL) {
            if (mac != NULL) {
                /* 已存在链路：更新目的 MAC，并登记为静态项。 */
                memcpy(link->addr.mac, mac, 6);
                (void)ep_static_arp_add_locked(ep, ip, mac);
            }
            pthread_mutex_unlock(&ep->lock);
            return link;
        }
    }
    memset(&addr, 0, sizeof(addr));
    addr.ip = ip;
    addr.port = port;
    addr.scope = scope;
    if (mac != NULL && scope == SX_SCOPE_UNICAST) {
        memcpy(addr.mac, mac, 6);
    }

    link = link_new(ep, &addr);
    if (link == NULL) {
        goto out;
    }
    if (scope == SX_SCOPE_UNICAST) {
        link->kcp = sx_kcp_create(ep, link, ep->cfg.conv, ep->cfg.send_window,
                                  ep->cfg.recv_window);
        if (link->kcp == NULL) {
            link_destroy(link);
            link = NULL;
            goto out;
        }
    } else {
        link->group = sx_group_create(ep, link, ep->cfg.conv,
                                      ep->cfg.group_history,
                                      ep->cfg.group_reorder,
                                      ep->cfg.group_max_src);
        if (link->group == NULL) {
            link_destroy(link);
            link = NULL;
            goto out;
        }
    }
    if (ep_add_link(ep, link) != SX_OK) {
        link_destroy(link);
        link = NULL;
        goto out;
    }
    if (scope == SX_SCOPE_UNICAST && mac != NULL) {
        (void)ep_static_arp_add_locked(ep, ip, mac);
    }
    if (scope != SX_SCOPE_UNICAST) {
        /* 组播与广播统一登记到 mc_links，供按 conv 的路由查找。 */
        sx_link_t **p = (sx_link_t **)realloc(ep->mc_links,
                                              (ep->nmc + 1) * sizeof(*p));
        if (p == NULL) {
            /* 回滚：撤销刚加入的链路，保持状态一致。 */
            ep->nlinks--;
            link_destroy(link);
            link = NULL;
            goto out;
        }
        ep->mc_links = p;
        ep->mc_links[ep->nmc++] = link;
        if (scope == SX_SCOPE_BROADCAST) {
            ep->bcast_link = link;
        }
        if (scope == SX_SCOPE_MULTICAST && ep->io != NULL &&
            ep->io->join_multicast != NULL) {
            (void)ep->io->join_multicast(ep->io, ip, ep->cfg.multicast_if);
        }
    }
    sx_ep_emit(ep, SX_EVENT_PEER_ADDED, link);
out:
    pthread_mutex_unlock(&ep->lock);
    sx_ep_flush_events(ep);
    return link;
}

sx_link_t *sx_add_unicast(sx_endpoint_t *ep, uint32_t ip, uint16_t port) {
    return add_link_common(ep, ip, port, SX_SCOPE_UNICAST, NULL);
}

sx_link_t *sx_add_unicast_mac(sx_endpoint_t *ep, uint32_t ip, uint16_t port,
                              const uint8_t mac[6]) {
    return add_link_common(ep, ip, port, SX_SCOPE_UNICAST, mac);
}

int sx_add_static_arp(sx_endpoint_t *ep, uint32_t ip, const uint8_t mac[6]) {
    int r;
    if (ep == NULL || mac == NULL) {
        return SX_ERR_INVALID;
    }
    pthread_mutex_lock(&ep->lock);
    r = ep_static_arp_add_locked(ep, ip, mac);
    pthread_mutex_unlock(&ep->lock);
    return r;
}

sx_link_t *sx_add_multicast(sx_endpoint_t *ep, uint32_t group_ip,
                            uint16_t port) {
    return add_link_common(ep, group_ip, port, SX_SCOPE_MULTICAST, NULL);
}

sx_link_t *sx_add_broadcast(sx_endpoint_t *ep, uint32_t bcast_ip,
                            uint16_t port) {
    return add_link_common(ep, bcast_ip, port, SX_SCOPE_BROADCAST, NULL);
}

int sx_link_remove(sx_endpoint_t *ep, sx_link_t *link) {
    size_t i;
    size_t j;
    if (ep == NULL || link == NULL) {
        return SX_ERR_INVALID;
    }
    pthread_mutex_lock(&ep->lock);
    for (i = 0; i < ep->nlinks; i++) {
        if (ep->links[i] == link) {
            ep->links[i] = ep->links[ep->nlinks - 1];
            ep->nlinks--;
            if (ep->bcast_link == link) {
                ep->bcast_link = NULL;
            }
            for (j = 0; j < ep->nmc; j++) {
                if (ep->mc_links[j] == link) {
                    ep->mc_links[j] = ep->mc_links[ep->nmc - 1];
                    ep->nmc--;
                    break;
                }
            }
            link_unref(link);
            pthread_mutex_unlock(&ep->lock);
            return SX_OK;
        }
    }
    pthread_mutex_unlock(&ep->lock);
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
    if (len > ep->cfg.max_message) {
        return SX_ERR_TOO_LARGE;
    }
    pthread_mutex_lock(&ep->lock);
    if (!ep->running) {
        pthread_mutex_unlock(&ep->lock);
        return SX_ERR_CLOSED;
    }
    if (link->scope == SX_SCOPE_UNICAST) {
        r = sx_kcp_send_message(link->kcp, data, len);
    } else {
        r = sx_group_send_message(link->group, data, len);
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
    pthread_mutex_lock(&ep->lock);
    link = find_unicast(ep, ip, port);
    pthread_mutex_unlock(&ep->lock);
    if (link == NULL) {
        link = sx_add_unicast(ep, ip, port);
        if (link == NULL) {
            return SX_ERR_NOMEM;
        }
    }
    return sx_send(ep, link, data, len);
}

static ssize_t sx_recv_common(sx_endpoint_t *ep, void *buf, size_t cap,
                             sx_addr_t *src_out, sx_link_t **link_out,
                             sx_pkt_info_t *info_out) {
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
    if (info_out != NULL) {
        *info_out = item->info;
    }
    {
        /* 返回完整消息长度：若大于 cap 说明本次被截断，调用方可据此察觉。 */
        ssize_t full = (ssize_t)item->len;
        sx_pool_free(&ep->pool, item->data);
        link_unref(item->link);
        free(item);
        return full;
    }
}

ssize_t sx_recv(sx_endpoint_t *ep, void *buf, size_t cap, sx_addr_t *src_out,
                sx_link_t **link_out) {
    return sx_recv_common(ep, buf, cap, src_out, link_out, NULL);
}

ssize_t sx_recv_ex(sx_endpoint_t *ep, void *buf, size_t cap,
                   sx_addr_t *src_out, sx_link_t **link_out,
                   sx_pkt_info_t *info_out) {
    return sx_recv_common(ep, buf, cap, src_out, link_out, info_out);
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

    /* 统一加锁：允许与应用线程的 sx_send/其它 sx_update 并发。 */
    pthread_mutex_lock(&ep->lock);
    if (!ep->running || ep->io == NULL) {
        pthread_mutex_unlock(&ep->lock);
        return SX_ERR_CLOSED;
    }

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

    /* 后端周期性维护（XDP 后端: ARP 老化/重试）。 */
    if (ep->io->update != NULL) {
        ep->io->update(ep->io, now);
    }

    ep_link_timeouts(ep, now);

    pthread_mutex_unlock(&ep->lock);

    /* 解锁后统一投递回调，避免用户回调在端点锁内执行。 */
    sx_ep_flush(ep);
    sx_ep_flush_events(ep);
    return processed;
}

int sx_wait(sx_endpoint_t *ep, int timeout_ms) {
    int r;
    if (ep == NULL) {
        return SX_ERR_INVALID;
    }
    pthread_mutex_lock(&ep->lock);
    if (!ep->running || ep->io == NULL) {
        pthread_mutex_unlock(&ep->lock);
        return SX_ERR_CLOSED;
    }
    if (ep->io->poll != NULL) {
        r = ep->io->poll(ep->io, timeout_ms);
    } else {
        if (timeout_ms > 0) {
            usleep((useconds_t)timeout_ms * 1000u);
        }
        r = 0;
    }
    pthread_mutex_unlock(&ep->lock);
    return r;
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
    pthread_mutex_lock(&ep->lock);
    *out = ep->stats;
    out->cc_mode = (uint32_t)ep->cfg.cc_mode;
    out->rto_backoff = ep->cfg.rto_backoff ? 1u : 0u;
    for (i = 0; i < ep->nlinks; i++) {
        sx_link_t *l = ep->links[i];
        if (l != NULL && l->kcp != NULL) {
            out->snd_wnd = SX_MIN(l->kcp->snd_wnd, l->kcp->cwnd);
            out->snd_outstanding = l->kcp->snd_count;
            out->per_peer_wnd = l->kcp->snd_wnd;
            out->cwnd = l->kcp->cwnd;
            out->rtt_ms = l->kcp->srtt;
            out->rto_ms = l->kcp->rto_ms;
            out->cc_mode = l->kcp->cc_enabled ? SX_CC_ON : SX_CC_OFF;
            out->rto_backoff = l->kcp->rto_backoff ? 1u : 0u;
            break;
        }
    }
    pthread_mutex_unlock(&ep->lock);
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

size_t sx_max_message(const sx_endpoint_t *ep) {
    if (ep == NULL) {
        return 0;
    }
    return ep->cfg.max_message;
}
