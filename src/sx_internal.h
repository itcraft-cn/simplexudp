/*
 * sx_internal.h - simple-xudp 内部共享定义
 *
 * 说明：本头文件仅供库内部编译单元使用，不对外暴露。
 * 包含协议常量、线格式、后端抽象、KCP 可靠引擎、组播/广播引擎与端点结构。
 */
#ifndef SX_INTERNAL_H
#define SX_INTERNAL_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "simplexudp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 协议常量                                                            */
/* ------------------------------------------------------------------ */

#define SX_MAGIC 0x5358u /* 'S''X' */
#define SX_PROTO_VER 1u

/* 命令字。 */
enum sx_cmd {
    SX_CMD_PUSH = 0x51, /* 数据 */
    SX_CMD_ACK = 0x52,  /* 确认（una + 选择性确认列表） */
    SX_CMD_WASK = 0x53, /* 窗口探测请求 */
    SX_CMD_WINS = 0x54, /* 窗口更新 */
    SX_CMD_HELLO = 0x55,/* 建链问候 */
    SX_CMD_BYE = 0x56,  /* 关闭 */
    SX_CMD_PING = 0x57, /* 心跳 */
    SX_CMD_PONG = 0x58, /* 心跳应答 */
    SX_CMD_NACK = 0x59  /* 组播/广播缺失请求（丢包侦测） */
};

/* SX 头固定长度（字节）：magic2+ver1+cmd1+conv4+peer4+seq4+una4+ts4+wnd4+ackc2+len2 */
#define SX_HDR_SIZE 32

/* 头部之后可选的 ACK/NACK 序号列表条数上限（单包内）。 */
#define SX_ACK_LIST_MAX 64

/* 单端点最大链路数（含自动学习），防止无界增长/DoS。 */
#define SX_MAX_PEERS 1024
/* 单次 update 最多收取的报文数，避免长时间不收发导致定时器饥饿。 */
#define SX_RX_BATCH_MAX 4096

/* IPv4 组播地址判定（224.0.0.0/4）。 */
#define SX_IP_CLASS_D_MASK 0xf0000000u
#define SX_IP_CLASS_D_PREFIX 0xe0000000u

/* 组播/广播：每来源的乱序重排深度与发送端历史条数上限。 */
#define SX_GROUP_MAX_SRC 32
#define SX_GROUP_REORDER 256

/* KCP 参数。 */
#define SX_FASTACK_THRESH 2 /* 快速重传触发跨越次数 */
#define SX_PROBE_LIMIT 4    /* 连续窗口探测次数上限 */
#define SX_SND_CAP_MIN 64   /* 发送环最小容量（2 的幂） */
#define SX_RCV_CAP_MIN 64   /* 接收环最小容量（2 的幂） */

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

#define SX_MIN(a, b) ((a) < (b) ? (a) : (b))
#define SX_MAX(a, b) ((a) > (b) ? (a) : (b))
#define SX_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#ifndef SX_UNUSED
#define SX_UNUSED(x) ((void)(x))
#endif

/* 校验是否为 2 的幂。 */
static inline int sx_is_pow2(uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }

/* 向上取整到 2 的幂。 */
static inline uint32_t sx_round_pow2(uint32_t v) {
    uint32_t r = 1;
    while (r < v) {
        r <<= 1;
        if (r == 0) return 0; /* 溢出 */
    }
    return r;
}

/* 序列号前后比较（回绕安全）：a 在 b 之后返回 1。 */
static inline int sx_seq_after(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;
}
/* a 在 b 之前（或相等）返回 1。 */
static inline int sx_seq_before_eq(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) <= 0;
}

/* ------------------------------------------------------------------ */
/* 时间与日志                                                          */
/* ------------------------------------------------------------------ */

/* 单调毫秒时钟。 */
uint64_t sx_now_ms(void);

/* 库内日志。禁止 stdout/stderr。 */
void sx_log(sx_endpoint_t *ep, sx_log_level_t level, const char *fmt, ...);

/* ------------------------------------------------------------------ */
/* 网络校验和/字节序工具（sx_frame.c）                                 */
/* ------------------------------------------------------------------ */

uint16_t sx_inet_csum(const void *data, size_t len, uint32_t init);
uint16_t sx_ipv4_csum(const void *ip_hdr, size_t len);
uint16_t sx_udp_csum(uint32_t src_ip, uint32_t dst_ip,
                     const void *udp_seg, size_t len);

/* ------------------------------------------------------------------ */
/* 协议头编解码（sx_proto.c）                                          */
/* ------------------------------------------------------------------ */

typedef struct sx_hdr {
    uint8_t cmd;
    uint8_t ver;
    uint32_t conv;
    uint32_t peer_id;
    uint32_t seq;
    uint32_t una;
    uint32_t ts;
    uint32_t wnd;
    uint16_t ack_count;
    uint16_t payload_len;
} sx_hdr_t;

/* 编码头部到 buf（至少 SX_HDR_SIZE 字节）。返回写入长度。 */
size_t sx_hdr_encode(const sx_hdr_t *h, uint8_t *buf, size_t cap);

/* 解析头部。成功返回 0，失败返回负错误码。 */
int sx_hdr_decode(const uint8_t *buf, size_t len, sx_hdr_t *out);

/* ------------------------------------------------------------------ */
/* 可靠消息（发送队列/接收重排）                                       */
/* ------------------------------------------------------------------ */

typedef struct sx_msg {
    uint32_t seq;
    uint16_t len;
    uint16_t xmit;      /* 重传次数 */
    uint16_t fastack;   /* 被跨越（快速重传）计数 */
    uint16_t flags;     /* SX_MSG_* */
    uint64_t xmit_ts;   /* 最近一次发送时间 */
    uint8_t *data;      /* 拥有的净荷 */
} sx_msg_t;

enum {
    SX_MSG_FREE = 0,
    SX_MSG_USED = 1,
    SX_MSG_ACKED = 2
};

/* ------------------------------------------------------------------ */
/* 后端抽象（sx_io_udp.c / sx_io_xdp.c）                              */
/* ------------------------------------------------------------------ */

typedef struct sx_io sx_io_t;

struct sx_io {
    const char *name;
    sx_endpoint_t *ep;
    int fd;       /* 可 poll 的 fd，-1 表示无 */
    void *priv;   /* 后端私有数据 */
    /* 发送一条 UDP 净荷。返回 0 或负错误码。 */
    int (*sendto)(sx_io_t *io, uint32_t dst_ip, uint16_t dst_port,
                  const void *buf, size_t len);
    /* 非阻塞接收。返回 1=收到，0=无数据，<0=错误。 */
    int (*recvfrom)(sx_io_t *io, uint32_t *src_ip, uint16_t *src_port,
                    void *buf, size_t cap, size_t *len);
    /* 等待可读，超时毫秒；返回 >0 可读，0 超时，<0 错误。 */
    int (*poll)(sx_io_t *io, int timeout_ms);
    /* 加入组播组（仅 UDP 后端实现；XDP 后端返回 SX_ERR_UNSUPPORTED）。 */
    int (*join_multicast)(sx_io_t *io, uint32_t group_ip, uint32_t local_ip);
    /* 周期性维护（可选）：ARP 老化/重试等。可为 NULL。 */
    void (*update)(sx_io_t *io, uint64_t now_ms);
    void (*close)(sx_io_t *io);
};

/* 创建后端。成功返回 io（内部已 open），失败 NULL。 */
sx_io_t *sx_io_udp_create(sx_endpoint_t *ep);
sx_io_t *sx_io_xdp_create(sx_endpoint_t *ep);

/* 帧构造（供 XDP 后端使用）。 */
typedef struct sx_frame_meta {
    size_t l2_len;  /* 以太头长度 */
    size_t l3_off;  /* IP 头偏移 */
    size_t l4_off;  /* UDP 头偏移 */
} sx_frame_meta_t;

/* 构造 Ethernet+IPv4+UDP 帧，返回帧总长或负错误码。 */
ssize_t sx_frame_build(uint8_t *buf, size_t cap,
                       const uint8_t src_mac[6], const uint8_t dst_mac[6],
                       uint32_t src_ip, uint32_t dst_ip,
                       uint16_t src_port, uint16_t dst_port,
                       const void *payload, size_t len);

/* 解析并校验 Ethernet+IPv4+UDP 帧，取出净荷位置与源/目的地址。
 * 成功返回 0，并填充 src_ip/dst_ip/src_port/dst_port/payload/len。 */
int sx_frame_parse(const uint8_t *frame, size_t frame_len,
                   uint32_t *src_ip, uint32_t *dst_ip,
                   uint16_t *src_port, uint16_t *dst_port,
                   const uint8_t **payload, size_t *len);

/* ------------------------------------------------------------------ */
/* ARP 缓存（sx_arp.c），仅 XDP 后端使用                               */
/* ------------------------------------------------------------------ */

typedef struct sx_arp sx_arp_t;

sx_arp_t *sx_arp_create(sx_io_t *io);
void sx_arp_destroy(sx_arp_t *arp);

/* 查询 ip->mac。命中返回 0 并填充 mac；未命中返回 SX_ERR_AGAIN 并触发 ARP 请求。 */
int sx_arp_resolve(sx_arp_t *arp, uint32_t ip, uint8_t mac_out[6]);

/* 处理收到的 ARP 帧（XDP 后端调用）。返回 1 表示已消费，0 表示非 ARP。 */
int sx_arp_input(sx_arp_t *arp, const uint8_t *frame, size_t len);

/* 周期性维护（超时重发请求、老化）。 */
void sx_arp_update(sx_arp_t *arp, uint64_t now_ms);

/* 注册静态映射（覆盖自动解析）。 */
void sx_arp_set(sx_arp_t *arp, uint32_t ip, const uint8_t mac[6]);

/* 本地 IP/MAC 设置（用于发送 ARP 请求）。 */
void sx_arp_set_local(sx_arp_t *arp, uint32_t ip, const uint8_t mac[6]);

/* 直接发送以太帧（XDP 后端注入回调）。 */
int sx_io_xdp_send_frame(sx_io_t *io, const uint8_t *frame, size_t len);

/* ------------------------------------------------------------------ */
/* KCP 可靠引擎（sx_kcp.c）                                            */
/* ------------------------------------------------------------------ */

typedef struct sx_kcp {
    sx_endpoint_t *ep;
    sx_link_t *link;
    uint32_t conv;
    uint32_t local_id;   /* 本端 peer_id */
    uint32_t remote_id;  /* 对端 peer_id（学习得到） */

    /* 发送侧 */
    uint32_t snd_una;    /* 最老未确认 */
    uint32_t snd_nxt;    /* 下一个分配序号 */
    uint32_t snd_wnd;    /* 对端通告窗口（消息数） */
    uint32_t cwnd;       /* 拥塞窗口（消息数） */
    uint32_t cwnd_cnt;   /* 拥塞避免计数 */
    uint32_t ssthresh;
    uint32_t send_cap;   /* 发送环容量（2 的幂） */
    sx_msg_t *snd_buf;   /* 发送环 [send_cap] */
    uint32_t snd_count;  /* 在途消息数 */

    /* 接收侧 */
    uint32_t rcv_nxt;    /* 期望序号 */
    uint32_t rcv_wnd;    /* 本端通告窗口 */
    uint32_t recv_cap;   /* 接收重排环容量（2 的幂） */
    sx_msg_t *rcv_buf;   /* 接收环 [recv_cap] */
    uint32_t rcv_count;  /* 已缓存待投递数 */

    /* RTT/RTO */
    uint32_t srtt;
    uint32_t rttvar;
    uint32_t rto_ms;
    uint32_t min_rto;
    uint32_t max_rto;

    /* ACK 待发送 */
    uint32_t ack_list[SX_ACK_LIST_MAX];
    uint32_t ack_count;
    int ack_pending;

    /* 窗口探测 */
    uint32_t probe_wait;
    uint32_t probe_cnt;
    uint64_t last_ack_ts;

    /* 心跳/状态 */
    uint64_t last_send_ms;
    uint64_t last_recv_ms;
    uint64_t last_update_ms;
    int up;
    int hello_sent;
    int established;
} sx_kcp_t;

sx_kcp_t *sx_kcp_create(sx_endpoint_t *ep, sx_link_t *link, uint32_t conv,
                        uint32_t send_window, uint32_t recv_window);
void sx_kcp_destroy(sx_kcp_t *k);
void sx_kcp_reset(sx_kcp_t *k);

/* 入队并立即发送一条消息。返回 seq 或负错误码。 */
int64_t sx_kcp_send(sx_kcp_t *k, const void *data, size_t len);

/* 收到一个完整 SX 报文（seg 以 SX 头开始，seg_len 为整包长度）。 */
void sx_kcp_input(sx_kcp_t *k, const uint8_t *seg, size_t seg_len,
                  uint64_t now_ms);

/* 周期驱动：重传、窗口探测、心跳、超时判定。 */
void sx_kcp_update(sx_kcp_t *k, uint64_t now_ms);

/* 是否有数据需要底层发送。由 kcp_update/send 内部直接调用发送钩子。 */

/* 组播/广播引擎接口（sx_group.c）。 */
typedef struct sx_group sx_group_t;

sx_group_t *sx_group_create(sx_endpoint_t *ep, sx_link_t *link, uint32_t conv,
                            uint32_t history, uint32_t reorder);
void sx_group_destroy(sx_group_t *g);
void sx_group_reset(sx_group_t *g);
int64_t sx_group_send(sx_group_t *g, const void *data, size_t len);
void sx_group_input(sx_group_t *g, const uint8_t *seg, size_t seg_len,
                    uint32_t src_ip, uint16_t src_port, uint64_t now_ms);
void sx_group_update(sx_group_t *g, uint64_t now_ms);

/* ------------------------------------------------------------------ */
/* 链路                                                                */
/* ------------------------------------------------------------------ */

struct sx_link {
    sx_endpoint_t *ep;
    sx_addr_t addr;      /* 目的地址（unicast 对端 / group 地址） */
    sx_scope_t scope;
    uint32_t conv;       /* 会话/组标识 */
    int index;           /* 在端点链表中的下标 */
    int used;
    uint64_t created_ms;
    sx_kcp_t *kcp;       /* unicast 引擎 */
    sx_group_t *group;   /* multicast/broadcast 引擎 */
};

/* ------------------------------------------------------------------ */
/* 端点                                                                */
/* ------------------------------------------------------------------ */

typedef struct sx_rx_item {
    sx_link_t *link;
    sx_addr_t addr;
    size_t len;
    uint8_t *data;
    struct sx_rx_item *next;
} sx_rx_item_t;

struct sx_endpoint {
    sx_config_t cfg;
    sx_io_t *io;
    sx_stats_t stats;

    sx_link_t **links;
    size_t nlinks;
    size_t links_cap;

    /* 按地址快速查找 unicast 链路（多播/广播单独链表遍历）。 */
    sx_link_t *bcast_link;
    sx_link_t **mc_links;
    size_t nmc;

    uint64_t started_ms;
    uint32_t next_local_id;

    /* 拉取模式接收队列。 */
    sx_rx_item_t *rx_head;
    sx_rx_item_t *rx_tail;
    size_t rx_count;

    int running;
    int backend_chosen;

    /* 内部线程 */
    pthread_t thread;
    int thread_started;
    volatile int stop_flag;
    pthread_mutex_t lock;
    /* 日志回调序列化锁（避免多线程并发回调宿主）。 */
    pthread_mutex_t log_lock;
    /* cfg.ifname 的私有副本，避免调用方释放原字符串导致悬垂。 */
    char *ifname_owned;
};

/* 内部 API：__ 前缀。 */
void sx_ep_emit(sx_endpoint_t *ep, sx_event_t ev, sx_link_t *link);
void sx_ep_deliver(sx_endpoint_t *ep, sx_link_t *link, uint32_t src_ip,
                   uint16_t src_port, const void *data, size_t len);
void sx_kcp_send_raw(sx_kcp_t *k, uint8_t cmd, uint32_t seq, uint32_t una,
                     uint32_t wnd, const uint32_t *ack_list, uint32_t ack_count,
                     const void *payload, size_t payload_len);
void sx_group_send_raw(sx_group_t *g, uint8_t cmd, uint32_t seq, uint32_t una,
                       uint32_t wnd, const uint32_t *list, uint32_t list_count,
                       const void *payload, size_t payload_len,
                       uint32_t dst_ip, uint16_t dst_port);

#ifdef __cplusplus
}
#endif

#endif /* SX_INTERNAL_H */
