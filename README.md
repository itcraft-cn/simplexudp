# simple-xudp (SX)

基于 **XDP + AF_XDP** 的 Linux 底层通讯库（ANSI C），同时提供**内核 UDP socket** 后端作为兼容/测试路径。向上提供消息导向、**有序可靠**、**丢包侦测重发**、支持**点播/组播/广播**的通讯能力。

- 目标平台：Linux x86_64 / aarch64，C99。
- 单包净荷上限固定（默认 1400），严格小于 MTU，禁止分片。
- 可靠性借鉴 **KCP**：滑动窗口、累积+选择性确认、快速重传、RTO 退避、RTT 估计、流控与拥塞控制。
- 组播/广播采用 **高水位 + NACK** 的尽力可靠（best-effort reliable）+ 按序投递。

## 架构

```
应用
 └─ sx_api (simplexudp.h)
     └─ sx_endpoint   端点/链路/报文集散、update 驱动
         └─ sx_kcp    单播可靠引擎（ARQ/窗口/重排/流控/拥塞）
         │  sx_group  组播/广播引擎（高水位/NACK/重排）
         └─ sx_io     后端抽象
             ├─ sx_io_udp   内核 UDP socket（无需 root）
             └─ sx_io_xdp   AF_XDP + sx_frame(以太/IP/UDP) + sx_arp + XDP BPF
```

线格式一致（SX 头位于 UDP 净荷内），因此 AF_XDP 与内核 UDP 后端可互操作。

## 数据面帧格式

```
Ethernet(14) | IPv4(20) | UDP(8) | SX header(32) | payload(<=1400)
总长 <= 1474 < 1500(MTU)
```

SX 头（网络序，32 字节）：`magic/ver/cmd | conv | peer_id | seq | una | ts | wnd | ack_count | payload_len`，其后可选 `ack_list[]`。

命令：`PUSH / ACK / WASK / WINS / HELLO / BYE / PING / PONG / NACK`。

## 构建

```sh
make            # 构建 build/libsimplexudp.a 与 .so（内含 XDP BPF 目标）
make examples   # 构建示例
make tests      # 构建测试
make check         # 运行单元/回环测试（无需 root，UDP 后端）
make check-xdp     # 需要 root：创建 veth 跑 AF_XDP 端到端测试
make check-latency # 需要 root：AF_XDP 时延测试 + bpftool 佐证流量经过 XDP
```

备选：`cmake -S . -B build-cmake && cmake --build build-cmake -j`。

依赖：`libbpf`、`libxdp`、`clang`（编译 BPF 目标；`xxd` 生成内嵌数组）。

> 日志：库内**不使用 stdout/stderr**，统一通过 `sx_config.on_log` 回调；测试/示例写入日志文件。

## 快速使用

```c
#include "simplexudp.h"

sx_config_t cfg;
sx_config_init(&cfg);
cfg.ifname    = "eth0";          /* 置空则走内核 UDP */
cfg.backend   = SX_BACKEND_AUTO; /* 优先 AF_XDP，失败回退 UDP */
cfg.local_port= 9000;
cfg.conv      = 0x1234;
cfg.on_recv   = my_recv;
cfg.user      = my_ctx;

sx_endpoint_t *ep = sx_create(&cfg);
sx_start(ep);

sx_link_t *peer = sx_add_unicast(ep, ipv4("10.0.0.2"), 9000);
sx_send(ep, peer, buf, len);            /* len <= sx_max_payload(ep) */

for (;;) sx_update(ep, 0);             /* 或 sx_run(ep, -1) */

sx_destroy(ep);
```

- 点播：`sx_add_unicast` / `sx_send_to`。
- 组播：`sx_add_multicast`（UDP 后端会 `IP_ADD_MEMBERSHIP`）。
- 广播：`sx_add_broadcast`（目的地址通常 `255.255.255.255` 或子网广播）。

详见 `docs/design.md`、`examples/sx_p2p.c`、`examples/sx_group.c`。

## 测试

- `tests/test_frame.c`：帧构造/解析与校验和防御。
- `tests/test_proto.c`：协议头编解码边界。
- `tests/test_kcp.c`：注入 **丢包 + 相邻乱序**，验证 100/100 按序到达且确有重传。
- `tests/test_loopback.c`：UDP 后端端到端双向有序投递。
- `tests/test_xdp.c` + `scripts/veth_test.sh`：veth 上 AF_XDP native/copy 端到端。
- `tests/test_latency.c` + `scripts/xdp_latency.sh`：停等往返时延统计（min/avg/p50/p99/max），
  并在测试挂载期间用 `bpftool net show` / `bpftool prog show` / `bpftool map dump` 佐证
  流量确实经 XDP 处理（`sx_stats_map` 的 redirect 计数）。

### 实测参考（本机 veth，两进程内端点）

| 路径 | RTT min | avg | p50 | p99 | max |
| --- | --- | --- | --- | --- | --- |
| AF_XDP（native + copy） | 1us | 1~2us | 2us | 2us | 11~17us |
| 内核 UDP（lo） | 4us | 4us | 4us | 6us | 55us |

上述为**同进程内两端点**的往返时延（含两端用户态处理），单向下行约为其一半量级。
`bpftool map dump` 显示 XDP 重定向计数约 4 万（2 万请求 + 2 万回显 + 控制帧），
证明报文确实经过 XDP 重定向到 AF_XDP。

## AF_XDP 说明与限制

- attach 模式：native 优先，失败回退 generic；bind：zero-copy 优先，失败回退 copy（自动侦测）。
- AF_XDP 绑定单个 RX 队列；需将目标流量引导到该队列（RSS/ethtool 或单队列网卡）。
- ARP 由本库用户态处理（XDP 程序会重定向 ARP），因此**仅在同一二层网段内可直接解析**；跨网段需配置网关/静态映射。
- generic 模式强制 copy；zero-copy 需要网卡驱动支持。
- 组播在 XDP 后端需接口接受该组播（可能需要 `ip maddr add` 或 allmulti）。

## 可靠性语义

- **单播**：严格可靠有序。累积确认 `una` + 选择性确认 `ack_list` + 快速重传 + RTO 重传 + 拥塞/流控。
- **组播/广播**：尽力可靠 + 有序。发送端保留历史窗口并周期性广播高水位；接收端侦测空洞后回送 `NACK`，发送端从历史重发到组地址。不保证送达（无多方 ACK 汇聚）。
