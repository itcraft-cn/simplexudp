/*
 * simplexudp.h - simple-xudp (SX) 公共 API
 *
 * 一个基于 XDP/AF_XDP 与内核 UDP socket 的底层通讯库，提供：
 *   - 消息导向的 UDP 语义（一包一消息，单包净荷 <= 上限，默认 1400 且 < MTU）
 *   - 借鉴 KCP 的可靠传输（时序有序、丢包侦测、选择性/快速重传、RTO、流控、拥塞控制）
 *   - 点对点（unicast）、组播（multicast）、广播（broadcast）三种语义
 *
 * 注意事项：
 *   - 本库仅面向 Linux x86_64 / aarch64。
 *   - 库内部禁止使用 stdout/stderr，日志统一通过 sx_log_handler 回调输出。
 *   - 所有接口除特别说明外，成功返回 0（或非负句柄），失败返回负的错误码，
 *     可用 sx_strerror() 获取可读信息。
 */
#ifndef SIMPLEXUDP_H
#define SIMPLEXUDP_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 常量与版本                                                          */
/* ------------------------------------------------------------------ */

#define SX_VERSION_MAJOR 0
#define SX_VERSION_MINOR 1
#define SX_VERSION_PATCH 0

/* 单包净荷上限：必须 < MTU。默认 1400。 */
#define SX_MAX_PAYLOAD_DEFAULT 1400
/* 协议允许的绝对上限：1500(MTU) - 14(Eth) - 20(IP) - 8(UDP) - 32(SX) = 1426。 */
#define SX_MAX_PAYLOAD_LIMIT 1426

/* 默认会话/组标识。 */
#define SX_DEFAULT_CONV 0x00000001u

/* ------------------------------------------------------------------ */
/* 错误码                                                              */
/* ------------------------------------------------------------------ */

typedef enum sx_error {
    SX_OK = 0,
    SX_ERR_INVALID = -1,        /* 参数非法 */
    SX_ERR_NOMEM = -2,          /* 内存不足 */
    SX_ERR_TOO_LARGE = -3,      /* 净荷超过上限 */
    SX_ERR_NOT_FOUND = -4,      /* peer/资源不存在 */
    SX_ERR_EXISTS = -5,         /* 已存在 */
    SX_ERR_AGAIN = -6,          /* 暂不可用（窗口满/无数据） */
    SX_ERR_IO = -7,             /* 底层 IO 错误 */
    SX_ERR_BACKEND = -8,        /* 后端初始化失败 */
    SX_ERR_STATE = -9,          /* 状态不允许 */
    SX_ERR_TIMEOUT = -10,       /* 超时 */
    SX_ERR_UNSUPPORTED = -11,   /* 不支持 */
    SX_ERR_WINDOW = -12,        /* 发送窗口已满（需重试） */
    SX_ERR_CLOSED = -13         /* 已关闭 */
} sx_error_t;

/* ------------------------------------------------------------------ */
/* 基础枚举与类型                                                      */
/* ------------------------------------------------------------------ */

/* 数据面后端。 */
typedef enum sx_backend {
    SX_BACKEND_AUTO = 0, /* 优先 AF_XDP，失败或不可用时回退内核 UDP */
    SX_BACKEND_XDP,      /* 强制 AF_XDP */
    SX_BACKEND_UDP       /* 强制内核 UDP socket */
} sx_backend_t;

/* XDP attach 模式。 */
typedef enum sx_xdp_mode {
    SX_XDP_MODE_AUTO = 0, /* 先 native，失败回退 generic */
    SX_XDP_MODE_NATIVE,   /* 驱动原生 */
    SX_XDP_MODE_GENERIC   /* 通用 skb 模式 */
} sx_xdp_mode_t;

/* AF_XDP 绑定模式。 */
typedef enum sx_xdp_bind {
    SX_XDP_BIND_AUTO = 0, /* 先 zero-copy，失败回退 copy */
    SX_XDP_BIND_COPY,     /* 强制 copy */
    SX_XDP_BIND_ZEROCOPY  /* 强制 zero-copy */
} sx_xdp_bind_t;

/* 通讯范围语义。 */
typedef enum sx_scope {
    SX_SCOPE_UNICAST = 0, /* 点对点，严格可靠有序 */
    SX_SCOPE_MULTICAST,   /* 组播，尽力可靠（NACK 恢复）+ 有序 */
    SX_SCOPE_BROADCAST    /* 广播，尽力可靠（NACK 恢复）+ 有序 */
} sx_scope_t;

/* 日志级别。 */
typedef enum sx_log_level {
    SX_LOG_ERROR = 0,
    SX_LOG_WARN,
    SX_LOG_INFO,
    SX_LOG_DEBUG,
    SX_LOG_TRACE
} sx_log_level_t;

/* 事件类型（回调通知）。 */
typedef enum sx_event {
    SX_EVENT_STARTED = 1,
    SX_EVENT_PEER_ADDED,
    SX_EVENT_PEER_REMOVED,
    SX_EVENT_PEER_TIMEOUT,   /* 对端长时间无响应 */
    SX_EVENT_PEER_UP,        /* 握手/首次收到对端数据 */
    SX_EVENT_PEER_DOWN,      /* 超时置为不可达 */
    SX_EVENT_BACKEND_CHOSEN, /* 后端/模式选择结果 */
    SX_EVENT_ERROR
} sx_event_t;

/* 地址描述（主机字节序）。 */
typedef struct sx_addr {
    uint32_t ip;    /* IPv4，主机字节序；0 表示自动/由后端推导 */
    uint16_t port;  /* 端口，主机字节序 */
    uint8_t mac[6]; /* 目的 MAC；XDP 后端可留 0，由 ARP 解析 */
    sx_scope_t scope;
} sx_addr_t;

/* 运行统计。 */
typedef struct sx_stats {
    uint64_t tx_packets;    /* 发送的 SX 报文数（不含重传） */
    uint64_t tx_bytes;
    uint64_t rx_packets;    /* 收到的有效 SX 报文数（去重后） */
    uint64_t rx_bytes;
    uint64_t retransmits;   /* 重传次数 */
    uint64_t fast_retrans;  /* 快速重传次数 */
    uint64_t timeouts;      /* RTO 超时次数 */
    uint64_t dup_packets;   /* 重复包 */
    uint64_t reorder_pkts;  /* 乱序包 */
    uint64_t lost_detected; /* 丢包侦测次数 */
    uint64_t nack_sent;
    uint64_t nack_recv;
    uint64_t dropped;       /* 因窗口满/校验失败等丢弃 */
    uint32_t snd_wnd;       /* 当前发送窗口可用 */
    uint32_t per_peer_wnd;  /* 对端通告窗口 */
    uint32_t cwnd;
    uint32_t rtt_ms;        /* 平滑 RTT */
    uint32_t rto_ms;        /* 当前 RTO */
    uint32_t backend;       /* 实际后端：sx_backend_t */
    uint32_t xdp_mode;      /* 实际 attach 模式：sx_xdp_mode_t */
    uint32_t xdp_bind;      /* 实际绑定模式：sx_xdp_bind_t */
} sx_stats_t;

/* 不透明类型。 */
typedef struct sx_endpoint sx_endpoint_t;
typedef struct sx_link sx_link_t;

/* ------------------------------------------------------------------ */
/* 回调                                                               */
/* ------------------------------------------------------------------ */

/* 收到一条按序消息。peer 为来源链路；data 仅在回调期间有效，如需保留请自行拷贝。 */
typedef void (*sx_recv_fn)(sx_endpoint_t *ep, sx_link_t *link,
                           const void *data, size_t len, void *user);

/* 事件通知。 */
typedef void (*sx_event_fn)(sx_endpoint_t *ep, sx_event_t event,
                            sx_link_t *link, void *user);

/* 日志回调。level 见 sx_log_level_t。禁止在库内直接 printf。 */
typedef void (*sx_log_fn)(sx_log_level_t level, const char *msg, void *user);

/* ------------------------------------------------------------------ */
/* 配置                                                               */
/* ------------------------------------------------------------------ */

typedef struct sx_config {
    /* ---- 数据面 ---- */
    const char *ifname;    /* 网卡名，如 "eth0"/"veth0"；UDP 后端可为 NULL */
    sx_backend_t backend;  /* 默认 SX_BACKEND_AUTO */
    sx_xdp_mode_t xdp_mode;
    sx_xdp_bind_t xdp_bind;
    int xdp_queue;         /* 绑定的 RX 队列，默认 0 */

    /* ---- 本地标识 ---- */
    uint32_t local_ip;     /* 本机 IP（主机序）；0 表示不指定 */
    uint16_t local_port;   /* 本地 UDP 端口（主机序） */
    uint32_t conv;         /* 会话/组标识，默认 SX_DEFAULT_CONV */

    /* ---- 协议参数 ---- */
    size_t max_payload;    /* 单包净荷上限，<= 1400 且 < MTU；0 -> 默认 1400 */
    uint32_t send_window;  /* 发送窗口（消息数），默认 256 */
    uint32_t recv_window;  /* 接收窗口/重排缓冲（消息数），默认 1024 */
    uint32_t group_history;/* 组播/广播历史条数，默认 1024 */
    uint32_t min_rto_ms;   /* 最小 RTO，默认 100 */
    uint32_t max_rto_ms;   /* 最大 RTO，默认 5000 */
    uint32_t keepalive_ms; /* 空闲心跳间隔，默认 10000 */
    uint32_t timeout_ms;   /* 对端超时判定，默认 30000 */

    /* ---- 运行模式 ---- */
    int rx_thread;         /* 1 = 库启动内部收发/定时线程 */

    /* ---- UMEM（XDP）---- */
    uint32_t umem_frames;  /* UMEM 帧数，默认 4096 */
    uint32_t frame_size;   /* 单帧大小，默认 4096 */

    /* ---- 回调 ---- */
    sx_recv_fn on_recv;
    sx_event_fn on_event;
    sx_log_fn on_log;
    void *user;

    sx_log_level_t log_level; /* 默认 SX_LOG_WARN */
} sx_config_t;

/* ------------------------------------------------------------------ */
/* 生命周期                                                            */
/* ------------------------------------------------------------------ */

/* 以默认值填充配置。 */
void sx_config_init(sx_config_t *cfg);

/* 创建端点。失败返回 NULL。 */
sx_endpoint_t *sx_create(const sx_config_t *cfg);

/* 销毁端点（自动 stop）。 */
void sx_destroy(sx_endpoint_t *ep);

/* 启动：初始化后端、加载/attach XDP（如适用）、启动线程（如配置）。 */
int sx_start(sx_endpoint_t *ep);

/* 停止：停止线程、detach XDP、关闭后端。 */
int sx_stop(sx_endpoint_t *ep);

/* 驱动一次：收发报文 + 处理定时器。now_ms 为单调毫秒；0 表示库内部取时。
 * 非 rx_thread 模式必须周期性调用（建议每 1~10ms）。返回处理的事件数或负错误码。 */
int sx_update(sx_endpoint_t *ep, uint64_t now_ms);

/* 阻塞运行直到 sx_stop 或收到信号（内部循环 sx_update）。timeout_ms<0 表示一直运行。 */
int sx_run(sx_endpoint_t *ep, int timeout_ms);

/* ------------------------------------------------------------------ */
/* 链路（peer/组）管理                                                 */
/* ------------------------------------------------------------------ */

/* 添加点对点链路。返回链路句柄，失败 NULL。 */
sx_link_t *sx_add_unicast(sx_endpoint_t *ep, uint32_t ip, uint16_t port);

/* 添加组播组（本地加入组、发送到 group_ip:port）。返回链路句柄，失败 NULL。 */
sx_link_t *sx_add_multicast(sx_endpoint_t *ep, uint32_t group_ip, uint16_t port);

/* 添加广播链路（发送到 bcast_ip:port，通常 255.255.255.255 或子网广播）。 */
sx_link_t *sx_add_broadcast(sx_endpoint_t *ep, uint32_t bcast_ip, uint16_t port);

/* 移除链路。 */
int sx_link_remove(sx_endpoint_t *ep, sx_link_t *link);

/* 通过链路发送一条消息（长度 <= max_payload）。
 * 返回分配的消息序号（>=0），或负错误码（SX_ERR_WINDOW 表示窗口满需重试）。 */
int64_t sx_send(sx_endpoint_t *ep, sx_link_t *link, const void *data, size_t len);

/* 便捷：对指定地址直接发送（自动创建/复用 unicast 链路）。 */
int64_t sx_send_to(sx_endpoint_t *ep, uint32_t ip, uint16_t port,
                   const void *data, size_t len);

/* 拉取模式接收：取出一条按序消息。
 * 返回长度（>=0），无数据返回 SX_ERR_AGAIN；link_out/src 可为 NULL。 */
ssize_t sx_recv(sx_endpoint_t *ep, void *buf, size_t cap,
                sx_addr_t *src_out, sx_link_t **link_out);

/* ------------------------------------------------------------------ */
/* 属性与统计                                                          */
/* ------------------------------------------------------------------ */

const char *sx_version(void);
const char *sx_strerror(int err);

/* 拷贝统计。 */
int sx_get_stats(sx_endpoint_t *ep, sx_stats_t *out);

/* 链路信息。 */
int sx_link_addr(const sx_link_t *link, sx_addr_t *out);
int sx_link_is_up(const sx_link_t *link);
/* 链路当前可用发送窗口（消息数）。 */
uint32_t sx_link_wnd(const sx_link_t *link);

/* 填充配置使用的单包净荷上限（便于应用切分）。 */
size_t sx_max_payload(const sx_endpoint_t *ep);

#ifdef __cplusplus
}
#endif

#endif /* SIMPLEXUDP_H */
