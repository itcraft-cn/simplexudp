# simplexudp 设计文档

## 1. 目标与范围

实现一个 Linux 用户态底层通讯库 `simple-xudp`（简称 SX），以 **XDP + AF_XDP** 为高速数据面，同时提供**内核 UDP socket** 作为兼容/测试后端，向上提供：

1. 类 UDP 的报文接口（一包一消息）；
2. 单包净荷上限固定（默认 1400 字节）且严格小于 MTU，禁止分片；
3. 时序有序 + 丢包侦测 + 重发（借鉴 KCP 的 ARQ、选择性重传、快速重传、RTO 退避、拥塞与流控）；
4. 点对点（unicast）、组播（multicast）、广播（broadcast）三种语义；
5. 仅 Linux x86_64 / aarch64，纯 ANSI C（C99）。

## 2. 总体分层

```
+-------------------------------------------------------------+
|  应用 (examples / 业务)                                      |
+-------------------------------------------------------------+
|  sx_api       公共 API：create/start/send/add_peer/update     |
+-------------------------------------------------------------+
|  sx_endpoint  端点、peer 表、报文分用/复用、update 驱动        |
+-------------------------------------------------------------+
|  sx_kcp       可靠传输引擎（ARQ/窗口/重排/流控/拥塞/NACK）    |
+-------------------------------------------------------------+
|  sx_proto     协议头编解码（含 ACK/选择性确认 bitmap）         |
+-------------------------------------------------------------+
|  sx_io        后端抽象 (vtable)                              |
|    +-- sx_io_udp   内核 UDP socket 后端（无需 root，易测试）   |
|    +-- sx_io_xdp   AF_XDP 后端                               |
|           +-- sx_frame  以太/IPv4/UDP 构造与解析、校验和      |
|           +-- sx_arp    ARP 缓存与请求/应答                   |
|           +-- BPF      XSKMAP 重定向程序 sx_xdp_kern.o        |
+-------------------------------------------------------------+
```

设计要点：**可靠传输与数据面解耦**。`sx_kcp` 只依赖 `sx_io` 的 `sendto/recvfrom`，因此同一套可靠逻辑既可跑在 AF_XDP 上，也可跑在内核 UDP 上。两者线格式完全一致（SX 头位于 UDP 净荷内），可互操作，便于单机/CI 联调。

## 3. 数据面（AF_XDP）

### 3.1 帧格式

```
Ethernet(14) | IPv4(20) | UDP(8) | SX header(32) | payload(<=1400)
帧总长 = 14+20+8+32+1400 = 1474 < 1500
```

- 不做 IPv4 分片/重组；`payload + SX头 <= MTU-42` 由库在 API 层硬性校验。
- IP 头校验和与 UDP 校验和（含伪首部）在用户态计算，零拷贝时需注意 `XDP_UMEM` 对齐。
- IPv4 TTL、DSCP、协议号可配置。

### 3.2 AF_XDP 资源模型（四环）

采用与参考项目一致的模型：UMEM 预分配等长 chunk，FILL/COMPLETION/RX/TX 四个 SPSC 环。`NUM_FRAMES`/`FRAME_SIZE` 可配置，默认 `FRAME_SIZE = 4096`（满足 XDP_UMEM 默认，允许 headroom）。

### 3.3 绑定模式与自动侦测

- XDP attach 模式：`native`（驱动原生）、`generic/skb`（通用，任意网卡/lo/veth）。
- 绑定标志：`copy`（`XDP_COPY`）、`zero-copy`（`XDP_ZEROCOPY`）。
- **自动侦测**：先尝试 `XDP_ZEROCOPY`，`xsk_socket__create` 返回 `ENOTSUP/EOPNOTSUPP` 或驱动无 zero-copy 能力时，回退到 `XDP_COPY`；XDP attach 先 native 失败再 generic。选择结果记入 `sx_stats_t.xdp_mode/xdp_bind`。
- 队列绑定：默认 queue 0；支持多队列时每队列一个 xsk，或使用 `XDP_SHARED_UMEM`（预留扩展）。

### 3.4 XDP 程序

`bpf/sx_xdp_kern.c`：查 `xsks_map`（`BPF_MAP_TYPE_XSKMAP`，key = rx_queue_index），命中则 `bpf_redirect_map`，否则 `XDP_PASS`；可选只处理目标 UDP 端口（避免劫持全部流量）。程序由库通过 libbpf 加载并 attach，BPF 目标对象在构建期用 `clang -target bpf` 编译并按架构生成 `__TARGET_ARCH_*`。

## 4. 可靠传输引擎（借鉴 KCP）

### 4.1 协议头（SX，网络字节序）

```
uint16 magic=0x5358   uint8 ver   uint8 cmd
uint32 conv        // 会话标识
uint32 peer_id     // 发送方本地 peer 标识
uint32 seq         // PUSH: 消息序号; ACK: 确认号
uint32 una         // 发送方最小未确认序号
uint32 ts          // 毫秒时间戳，用于 RTT
uint32 wnd         // 接收窗口（消息数）
uint16 ack_count   // 选择性确认条数
uint16 payload_len
uint32 ack_list[ack_count]   // 选择性确认（KCP ACK list 思想）
payload ...
```

命令：`PUSH/ACK/WASK/WINS/HELLO/BYE/PING/PONG/NACK`。

### 4.2 有序与重发

- **消息序号** `seq` 单调递增，一包一消息，无需分片/重组。
- 接收端维护 `rcv_nxt`（期望序号）与乱序重排缓冲；按序投递，去重。
- **ACK**：携带 `una`（累积确认）与 `ack_list`（选择性确认，覆盖乱序到达的序号），发送端据此推进 `snd_una`、清理发送队列。
- **重发**：
  - 超时：每消息记录 `xmit_ts`，`now - xmit_ts >= rto` 触发重传；
  - 快速重传：KCP 的 `fastack` 计数，同一序号被后续 ACK 跨越（skip）次数达到阈值即重传；
  - 重传时 `rto = rto * 2`（带上限），正常 ACK 时按 SRTT/RTTVAR 更新（Jacobson）。
- **RTT**：由 `ts` 回显或发送时间戳估算 SRTT/RTTVAR，得到自适应 RTO（min 100ms 默认，max 可配）。

### 4.3 流控与拥塞控制

- 接收窗口 `rcv_wnd`：剩余重排/投递容量，随 ACK 通告。
- 发送窗口 `snd_wnd = min(peer_wnd, cwnd)`。
- 拥塞：慢启动（`cwnd < ssthresh` 每 ACK +1）→ 拥塞避免（每 RTT +1）；超时 `ssthresh=cwnd/2, cwnd=1`；快速重传后进入快速恢复。
- 无数据可发且窗口探测：`WASK/WINS`，避免窗口零死锁（KCP 窗口探测）。

### 4.4 组播/广播的可靠性

组播/广播无法对单一接收方做 ACK 汇聚，采取 **NACK 驱动恢复 + 序号有序**：

- 发送端保留最近 `SX_GROUP_HISTORY`（默认 1024）条消息作为历史；
- 接收端按序号侦测空洞（丢包侦测），对空洞发送 `NACK`（带缺失序号列表，带节流/去重）；
- 发送端收到 `NACK` 从历史重发对应消息；
- 语义为**尽力可靠**（best-effort reliable），文档明确其不保证，但提供有序投递与丢包侦测重发能力；
- unicast 为**严格可靠有序**（KCP 语义）。

## 5. 会话与多播分发

- `conv` 为会话/组标识，由用户配置；同一组内 `conv` 一致。
- peer 表以 `(scope, ip, port)` 为键；unicast 每对端一个独立可靠控制块；multicast/broadcast 各一个组控制块（内部以发送方地址区分来源）。
- 地址解析：XDP 后端需目的 MAC，`sx_arp` 维护 `ip -> mac` 缓存，未命中时发 ARP 请求并缓存/超时；组播映射 `01:00:5e:...`，广播 `ff:ff:ff:ff:ff:ff`。

## 6. 并发与线程模型

- 单线程事件驱动：`sx_update(ep, now_ms)` 推进定时器（重传/心跳/ARP 超时）并收取报文；`sx_run` 提供阻塞循环。
- 可选内部线程：`cfg.rx_thread = 1` 时库启动收发/定时线程，回调在库线程触发；共享数据用互斥锁保护（见编码规范）。
- 所有日志走 `sx_log` 回调（禁止 stdout/stderr）。

## 7. 错误码与可观测性

- 枚举错误码 + `sx_strerror`。
- `sx_stats_t`：收发包、字节、重传、快速重传、丢包侦测、乱序、超时、窗口、RTT、后端模式等。

## 8. 构建与测试

- `Makefile`（主）+ `CMakeLists.txt`（可选），自动探测架构并生成 BPF 目标。
- 单测（无需 root）：`test_frame`（校验和/编解码）、`test_proto`（头/ACK 位图）、`test_kcp`（本地回环 ARQ：丢包/乱序/重传/有序性）。
- 集成测试（需 sudo）：`scripts/veth_test.sh` 创建 veth 对，XDP generic 模式跑 p2p/p2mp。
- UDP 后端使 CI 与开发机零依赖可跑。

## 9. 关键权衡

- **消息导向 vs 流导向**：需求为“包”，故选消息导向，简化重排与投递；代价是不支持超大消息（超出即报错）。
- **UDP 后端**：牺牲一点性能换取可测试性、可移植性与与内核栈互操作；生产可切 XDP。
- **组播/广播尽力可靠**：避免为多方维护 ACK 状态导致的状态爆炸；用 NACK + 历史窗口折中。
- **zero-copy 自动侦测**：优先性能，失败静默回退 copy-mode，保证兼容。
