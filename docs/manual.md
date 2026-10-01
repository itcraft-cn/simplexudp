# simple-xudp (SX) 使用手册

版本：0.1.0　适用平台：Linux x86_64 / aarch64　语言：ANSI C（C99）

本手册是 `include/simplexudp.h` 的完整使用说明，包含概念模型、快速开始、配置详解、
API 参考、协议与可靠性语义、AF_XDP 说明、调优与常见问题。设计原理见 `docs/design.md`。

---

## 目录

1. 概述
2. 概念模型
3. 构建与安装
4. 快速开始
5. 事件循环与线程模型
6. 配置详解（sx_config_t）
7. API 参考
8. 错误码
9. 统计与可观测性
10. 线格式与可靠性语义
11. AF_XDP 后端说明与限制
12. 调优指南
13. 部署与权限
14. 常见问题（FAQ）
15. 示例与测试

---

## 1. 概述

SX 是一个底层通讯库，数据面可选：

- **AF_XDP**：XDP 程序将目标 UDP/ARP 报文重定向到用户态 xsk，零拷贝/拷贝模式自动侦测；
- **内核 UDP socket**：无需 root，便于开发、单测与 CI，与 AF_XDP 线格式一致、可互操作。

向上提供消息导向（一包一消息）的接口，具备：

- 单包净荷上限固定（默认 1400，严格小于 MTU），不分片；
- 时序有序、丢包侦测与重发（借鉴 KCP）；
- 点对点（unicast，严格可靠有序）、组播/广播（尽力可靠 + 有序）。

平台仅限 Linux x86_64 / aarch64。库内部不使用 stdout/stderr，日志通过回调输出。

---

## 2. 概念模型

```
sx_endpoint_t  端点：一个本地 ip:port + 数据面后端 + 若干链路
   └── sx_link_t  链路：一条逻辑通道
         ├── UNICAST   到某个对端 (ip,port)，严格可靠有序
         ├── MULTICAST 到组地址，尽力可靠 + 有序
         └── BROADCAST 到广播地址，尽力可靠 + 有序
```

- **conv**：会话/组标识。同一会话两端 `conv` 必须一致；库按 `conv` 与来源地址分用报文。
- **消息**：一次 `sx_send` 对应一个序号、一个 UDP 报文，长度不得超过 `sx_max_payload()`。
- **对端学习**：点播服务端无需预先登记客户端；收到 `conv` 匹配的报文会自动建立链路并
  触发 `SX_EVENT_PEER_ADDED`。
- **回调 vs 拉取**：设置 `on_recv` 为回调模式；未设置则用 `sx_recv()` 拉取。

---

## 3. 构建与安装

```sh
make            # 生成 build/libsimplexudp.a 与 build/libsimplexudp.so
make examples   # 示例
make tests      # 测试
make check      # 运行单元/回环测试（无需 root）
```

依赖：`libbpf`、`libxdp`、`clang`（编译 BPF 目标）、`xxd`（生成内嵌字节数组）。
备选构建：`cmake -S . -B build-cmake && cmake --build build-cmake -j`。

在应用中链接：

```sh
cc app.c -I<prefix>/include -L<prefix>/lib -lsimplexudp -lbpf -lxdp -lpthread
```

---

## 4. 快速开始

发送端（客户端）：

```c
sx_config_t cfg;
sx_config_init(&cfg);
cfg.ifname     = "eth0";          /* AF_XDP；置空则走内核 UDP */
cfg.backend    = SX_BACKEND_AUTO; /* 优先 AF_XDP，失败回退 UDP */
cfg.local_port = 9000;
cfg.local_ip   = 0;               /* 0=自动 */
cfg.conv       = 0x1234;
cfg.on_recv    = on_msg;          /* 或 NULL 用 sx_recv 拉取 */
cfg.user       = my_ctx;

sx_endpoint_t *ep = sx_create(&cfg);
sx_start(ep);

sx_link_t *peer = sx_add_unicast(ep, ipv4("10.0.0.2"), 9000);
sx_send(ep, peer, buf, len);      /* len <= sx_max_payload(ep) */

while (running) {
    sx_wait(ep, 100);             /* 阻塞等待可读（也可用 sx_run） */
    sx_update(ep, 0);             /* 收发 + 定时器 */
}
sx_destroy(ep);
```

接收端（服务端）：只需 `sx_create` + `sx_start` + 事件循环；来自客户端的第一条报文会
自动建立链路，回显时用回调参数里的 `link`：

```c
static void on_msg(sx_endpoint_t *ep, sx_link_t *link,
                   const void *data, size_t len, void *user) {
    (void)user;
    sx_send(ep, link, data, len);   /* 原样回显 */
}
```

---

## 5. 事件循环与线程模型

两种驱动方式：

- **单线程（默认）**：应用周期性调用 `sx_update(ep, 0)`；建议配合 `sx_wait(ep, timeout)`
  阻塞等待可读，避免忙等。定时器（重传/心跳）由 `sx_update` 推进，等待超时不宜过大
  （推荐 20~200ms）。
- **内部线程**：`cfg.rx_thread = 1` 时库启动收发/定时线程，回调在库线程触发。
  库内部锁为递归锁，允许在回调中直接调用 `sx_send`。

线程安全：`sx_send`/`sx_update`/链路增删受端点锁保护；多个应用线程可并发 `sx_send`。

---

## 6. 配置详解（sx_config_t）

先用 `sx_config_init(&cfg)` 填默认值，再按需覆盖。

### 数据面

| 字段 | 默认 | 说明 |
| --- | --- | --- |
| `ifname` | NULL | 网卡名；AF_XDP 必填，UDP 后端可为 NULL |
| `backend` | `SX_BACKEND_AUTO` | `AUTO`=优先 XDP 失败回退 UDP；`XDP`/`UDP` 强制 |
| `xdp_mode` | `SX_XDP_MODE_AUTO` | `AUTO`=native 失败回退 generic；`NATIVE`/`GENERIC` |
| `xdp_bind` | `SX_XDP_BIND_AUTO` | `AUTO`=zero-copy 失败回退 copy；`COPY`/`ZEROCOPY` |
| `xdp_queue` | 0 | 绑定的 RX 队列 |

### 本地标识

| 字段 | 默认 | 说明 |
| --- | --- | --- |
| `local_ip` | 0 | 本机 IPv4（主机序）；0=不指定/由后端推导 |
| `local_port` | 0 | 本地端口（主机序）；0=内核分配（UDP） |
| `conv` | `SX_DEFAULT_CONV` | 会话/组标识 |

### 协议参数

| 字段 | 默认 | 说明 |
| --- | --- | --- |
| `max_payload` | 1400 | 单包净荷上限；0→1400；超过 1426 会被钳制 |
| `send_window` | 256 | 发送窗口（消息数），同时决定发送环容量 |
| `recv_window` | 1024 | 接收窗口/乱序重排容量 |
| `group_history` | 1024 | 组播/广播发送历史条数（NACK 恢复用） |
| `min_rto_ms` | 100 | 最小重传超时 |
| `max_rto_ms` | 5000 | 最大重传超时 |
| `keepalive_ms` | 10000 | 空闲心跳间隔 |
| `timeout_ms` | 30000 | 对端无响应判定为 DOWN 的阈值 |

### 运行/资源/回调

| 字段 | 默认 | 说明 |
| --- | --- | --- |
| `rx_thread` | 0 | 1=库启动内部线程 |
| `umem_frames` | 4096 | AF_XDP UMEM 帧数 |
| `frame_size` | 4096 | AF_XDP 单帧大小 |
| `on_recv` | NULL | 收包回调；NULL 用 `sx_recv` 拉取 |
| `on_event` | NULL | 事件回调 |
| `on_log` | NULL | 日志回调 |
| `user` | NULL | 回调透传指针 |
| `log_level` | `SX_LOG_WARN` | 日志级别 |

---

## 7. API 参考

约定：成功返回 0（或非负句柄/长度/序号），失败返回负的错误码，可用 `sx_strerror()` 转换。

### 7.1 生命周期

#### `void sx_config_init(sx_config_t *cfg)`
以默认值填充配置。

#### `sx_endpoint_t *sx_create(const sx_config_t *cfg)`
创建端点。失败返回 NULL。`max_payload` 会被钳制到 `SX_MAX_PAYLOAD_LIMIT`。

#### `int sx_start(sx_endpoint_t *ep)`
初始化后端、加载/attach XDP（如适用）、启动内部线程（如配置）。
返回 `SX_OK` 或负错误码（如 `SX_ERR_BACKEND`）。默认后端为 AUTO 时 XDP 失败会回退 UDP。

#### `int sx_stop(sx_endpoint_t *ep)`
停止线程、detach XDP、关闭后端、释放链路。可重复调用。

#### `void sx_destroy(sx_endpoint_t *ep)`
析构端点（内部自动 `sx_stop`）。传 NULL 安全。

### 7.2 驱动

#### `int sx_update(sx_endpoint_t *ep, uint64_t now_ms)`
驱动一次：收取并处理所有待处理报文、推进各链路定时器（重传/窗口探测/心跳/超时）。
`now_ms=0` 时库内部取时。返回本次处理的报文数或负错误码。
非 `rx_thread` 模式必须周期性调用（建议 1~10ms，或配合 `sx_wait`）。

#### `int sx_wait(sx_endpoint_t *ep, int timeout_ms)`
阻塞等待可读事件（内部 `poll` 后端 fd）。返回 >0 可读、0 超时、<0 错误；
`timeout_ms<0` 无限等待。推荐事件循环：`sx_wait` + `sx_update`，避免忙等。

#### `int sx_run(sx_endpoint_t *ep, int timeout_ms)`
阻塞运行内部循环（`sx_update` + `sx_wait`）直到 `sx_stop` 或超时；`timeout_ms<0` 一直运行。

### 7.3 链路管理

#### `sx_link_t *sx_add_unicast(sx_endpoint_t *ep, uint32_t ip, uint16_t port)`
添加点播链路（严格可靠有序）。已存在则返回同一条。失败 NULL。

#### `sx_link_t *sx_add_multicast(sx_endpoint_t *ep, uint32_t group_ip, uint16_t port)`
添加组播链路。UDP 后端会执行 `IP_ADD_MEMBERSHIP`；XDP 后端返回
`SX_ERR_UNSUPPORTED`（由网卡/接口接受组播）。

#### `sx_link_t *sx_add_broadcast(sx_endpoint_t *ep, uint32_t bcast_ip, uint16_t port)`
添加广播链路（目的通常 `255.255.255.255` 或子网广播地址）。

#### `int sx_link_remove(sx_endpoint_t *ep, sx_link_t *link)`
移除链路并释放其状态。

### 7.4 收发

#### `int64_t sx_send(sx_endpoint_t *ep, sx_link_t *link, const void *data, size_t len)`
发送一条消息。返回消息序号（`>=0`），或负错误码：

- `SX_ERR_TOO_LARGE`：`len > sx_max_payload(ep)`；
- `SX_ERR_WINDOW`：发送窗口已满，需稍后重试；
- `SX_ERR_CLOSED`：端点未启动或已停止。

#### `int64_t sx_send_to(sx_endpoint_t *ep, uint32_t ip, uint16_t port, const void *data, size_t len)`
便捷发送：自动创建/复用对应点播链路后发送。

#### `ssize_t sx_recv(sx_endpoint_t *ep, void *buf, size_t cap, sx_addr_t *src_out, sx_link_t **link_out)`
拉取模式接收一条按序消息。返回实际长度；无数据返回 `SX_ERR_AGAIN`；
`src_out`/`link_out` 可为 NULL。仅在使用拉取模式（`on_recv=NULL`）时使用。

### 7.5 属性与统计

#### `const char *sx_version(void)` / `const char *sx_strerror(int err)`
版本字符串 / 错误码描述。

#### `int sx_get_stats(sx_endpoint_t *ep, sx_stats_t *out)`
拷贝运行统计；同时填充首个点播链路的窗口/RTT 快照。

#### `int sx_link_addr(const sx_link_t *link, sx_addr_t *out)`
取链路目的地址。

#### `int sx_link_is_up(const sx_link_t *link)`
点播链路是否可用（握手/近期有收包）；组播/广播恒返回 1。

#### `uint32_t sx_link_wnd(const sx_link_t *link)`
链路的可用发送窗口（消息数 = `min(对端通告窗口, 拥塞窗口)`）。

#### `size_t sx_max_payload(const sx_endpoint_t *ep)`
配置的单包净荷上限，便于应用切分消息。

### 7.6 回调

```c
void (*sx_recv_fn)(sx_endpoint_t *ep, sx_link_t *link,
                   const void *data, size_t len, void *user);
void (*sx_event_fn)(sx_endpoint_t *ep, sx_event_t event, sx_link_t *link, void *user);
void (*sx_log_fn)(sx_log_level_t level, const char *msg, void *user);
```

- `data` 仅在回调期间有效，需保留请自行拷贝；
- 回调内可调用 `sx_send`（递归锁保护）；
- 日志回调不得阻塞，库内不直接写 stdout/stderr。

---

## 8. 错误码

| 值 | 名称 | 含义 |
| --- | --- | --- |
| 0 | `SX_OK` | 成功 |
| -1 | `SX_ERR_INVALID` | 参数非法 |
| -2 | `SX_ERR_NOMEM` | 内存不足 |
| -3 | `SX_ERR_TOO_LARGE` | 净荷超过上限 |
| -4 | `SX_ERR_NOT_FOUND` | 资源不存在 |
| -5 | `SX_ERR_EXISTS` | 已存在 |
| -6 | `SX_ERR_AGAIN` | 暂不可用/无数据 |
| -7 | `SX_ERR_IO` | 底层 IO 错误 |
| -8 | `SX_ERR_BACKEND` | 后端初始化失败 |
| -9 | `SX_ERR_STATE` | 状态不允许 |
| -10 | `SX_ERR_TIMEOUT` | 超时 |
| -11 | `SX_ERR_UNSUPPORTED` | 不支持 |
| -12 | `SX_ERR_WINDOW` | 发送窗口已满 |
| -13 | `SX_ERR_CLOSED` | 已关闭 |

---

## 9. 统计与可观测性

`sx_stats_t` 关键字段：

- 流量：`tx_packets/tx_bytes/rx_packets/rx_bytes`
- 可靠性：`retransmits`、`fast_retrans`、`timeouts`、`dup_packets`、`reorder_pkts`、
  `lost_detected`、`nack_sent/nack_recv`、`dropped`
- 窗口/时延：`snd_wnd`、`snd_outstanding`、`per_peer_wnd`、`cwnd`、`rtt_ms`、`rto_ms`
- 后端：`backend`、`xdp_mode`、`xdp_bind`

AF_XDP 还可用 `bpftool` 观察：`bpftool net show dev <if>`、`bpftool prog show`、
`bpftool map dump name sx_stats_map`（key0=重定向到 xsk，key1=PASS）。

---

## 10. 线格式与可靠性语义

帧格式（AF_XDP 后端）：

```
Ethernet(14) | IPv4(20) | UDP(8) | SX header(32) | payload(<=1400)
总长 <= 1474 < 1500(MTU)
```

SX 头（网络序，32 字节）：`magic/ver/cmd | conv | peer_id | seq | una | ts | wnd | ack_count | payload_len`，
其后可选 `ack_list[]`。命令：`PUSH/ACK/WASK/WINS/HELLO/BYE/PING/PONG/NACK`。

- **单播**：严格可靠有序。累积确认（`una`，即对端 `rcv_nxt`）+ 选择性确认（`ack_list`）+
  快速重传 + RTO 退避 + RTT 估计 + 流控 + 拥塞控制（慢启动/拥塞避免/超时降窗）。
- **组播/广播**：尽力可靠 + 有序。发送端保留 `group_history` 条历史并周期广播高水位；
  接收端按来源侦测空洞后回送 `NACK`，发送端从历史重发到组地址。不保证送达。

---

## 11. AF_XDP 后端说明与限制

- attach：native 优先，失败回退 generic；bind：zero-copy 优先，失败回退 copy（自动侦测，
  结果见 `stats.xdp_mode/xdp_bind`）。
- 绑定单个 RX 队列；需把目标流量引导到该队列（RSS/`ethtool -N` 或单队列网卡）。
- **ARP 由本库用户态处理**（XDP 程序把 ARP 也重定向到用户态），因此仅同一二层网段内可
  直接解析；跨网段需网关或静态映射。
- generic 模式强制 copy；zero-copy 需网卡驱动支持。
- 组播在 XDP 后端需接口接受该组播（可能需要 `ip maddr add` 或 allmulti）。
- 退出前请确保正常 `sx_stop`/`sx_destroy` 以 detach XDP；进程被 SIGKILL 会残留 XDP 程序
  （可用 `sudo xdp-loader unload <if> --all` 清理）。

---

## 12. 调优指南

- **MTU/净荷**：`max_payload` 越大吞吐越高，但必须 < MTU；本库不分片。
- **窗口**：`send_window`/`recv_window` 影响吞吐与内存；高 BDP 链路可增大。
- **RTO**：局域网可减小 `min_rto_ms`（如 20~50）；跨广域网保持 100+。
- **绑核**：时延敏感场景把收发进程分别绑定到不同**性能核**，并避开中断核。实测不绑核
  max 抖动可达数百 us，绑到两个不同性能核后降到约 20~30us：

  ```sh
  SX_CPU_SERVER=2 SX_CPU_CLIENT=3 sudo scripts/xdp_latency_2p.sh build/test_latency
  ```

- **队列**：多队列网卡可将不同流 RSS 到不同队列，每队列一个端点/进程。
- **日志**：生产建议 `SX_LOG_WARN`；调试可靠性问题时临时开 `SX_LOG_DEBUG`。

---

## 13. 部署与权限

- **AF_XDP**：需要 `CAP_BPF`、`CAP_NET_ADMIN`（加载/attach XDP）与足够的内存锁上限；
  传统内核还需 `RLIMIT_MEMLOCK`（`ulimit -l unlimited`）。可用 `setcap` 授予最小能力，
  或由 systemd 指定 `AmbientCapabilities`。
- **内核 UDP**：普通用户即可。
- 程序正常退出会 detach XDP；异常退出需手动清理。

---

## 14. 常见问题（FAQ）

**Q：收不到数据？**
AF_XDP 下检查：(1) 目标流量是否被引导到绑定的 RX 队列；(2) 是否同一二层网段（ARP）；
(3) XDP 是否成功 attach（`bpftool net show dev <if>`）；(4) 端口与 `conv` 是否一致。
UDP 后端下检查防火墙与端口绑定。

**Q：`sx_send` 返回 `SX_ERR_WINDOW`？**
发送窗口已满，稍后重试（先 `sx_update`/`sx_wait` 处理 ACK 打开窗口），或用 `sx_link_wnd` 判断。

**Q：单包最大能多大？**
`sx_max_payload(ep)`，默认 1400，绝对上限 1426。超出返回 `SX_ERR_TOO_LARGE`。

**Q：组播/广播会重传吗？**
会，但为“尽力可靠”：接收端侦测空洞后回送 NACK，发送端从历史重发到组地址；不保证送达。

**Q：`on_recv` 里能直接回复吗？**
可以。库使用递归锁，回调内调用 `sx_send(ep, link, ...)` 安全（`link` 即来源链路）。

**Q：如何确认真的走了 XDP？**
`bpftool net show dev <if>` 看程序挂载；`bpftool map dump name sx_stats_map` 看重定向计数。

---

## 15. 示例与测试

- `examples/sx_p2p.c`：点播请求/回显（两阶段），`make examples && ./build/sx_p2p`。
- `examples/sx_group.c`：一个发送方 + 两个接收方的组播演示。
- `tests/test_frame.c`：帧编解码与校验和。
- `tests/test_proto.c`：协议头边界。
- `tests/test_kcp.c`：注入丢包 + 乱序，验证有序与重传。
- `tests/test_loopback.c`：UDP 后端端到端双向有序。
- `tests/test_xdp.c` + `scripts/veth_test.sh`：veth 上 AF_XDP 端到端。
- `tests/test_latency.c` + `scripts/xdp_latency.sh` / `xdp_latency_2p.sh`：时延与 bpftool 佐证。
