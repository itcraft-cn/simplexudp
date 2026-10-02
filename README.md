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

### 依赖

- C 编译器：`gcc` 或 `clang`（C99）。
- 构建工具：`make`（或 CMake ≥ 3.16）。
- **AF_XDP 后端**额外需要：`libbpf`、`libxdp` 开发包，`clang`（支持 `-target bpf`，用于编译 XDP 程序），
  `xxd`（把 BPF 目标内嵌为字节数组），`pkg-config`（可选，用于探测 libbpf/libxdp）。
- 运行 AF_XDP 需要 root（或 `CAP_BPF` + `CAP_NET_ADMIN`）；**内核 UDP 后端无需特权**。

```sh
# Debian / Ubuntu
sudo apt-get install -y build-essential clang llvm libbpf-dev libxdp-dev pkg-config xxd

# RHEL / CentOS / Fedora
sudo dnf install -y gcc clang llvm libbpf-devel libxdp-devel pkgconf-pkg-config vim-common
```

### 常用目标

```sh
make                 # 构建 build/libsimplexudp.a 与 .so（默认含 XDP BPF 目标）
make bpf             # 仅编译 XDP BPF 目标
make examples        # 示例：build/sx_p2p、build/sx_group
make tests           # 单元/回环测试：build/test_{frame,proto,kcp,loopback,recv}
make tests-xdp       # AF_XDP 端到端测试：build/test_xdp
make tests-latency   # 时延测试：build/test_latency
make tests-throughput# 吞吐/长稳测试：build/test_throughput
make group-tools     # 组播/广播工具：build/test_gsend、build/test_grecv
make relay-tool      # 三跳接力：build/test_relay

make check           # 运行单元/回环测试（无需 root，UDP 后端）
make check-xdp       # 需要 root：创建 veth 跑 AF_XDP 端到端测试
make check-latency   # 需要 root：AF_XDP 时延测试 + bpftool 佐证流量经过 XDP
make check-latency-2p# 需要 root：双进程 AF_XDP 时延测试

make install         # 安装头文件与库（默认 PREFIX=/usr/local）
make uninstall
make clean
```

### 构建选项

| 变量 | 作用 |
| --- | --- |
| `SX_NO_XDP=1` | 仅内核 UDP 后端：不依赖 libbpf/libxdp/clang，用于无 XDP 环境或分发 |
| `SX_LOSSY=1` | 编译进丢包/乱序注入（运行期环境变量 `SX_LOSSY_DROP_PPM`/`SX_LOSSY_DUP_PPM`/`SX_LOSSY_REORDER_PPM` 开启），用于无 root 的可靠性测试 |
| `DEBUG=1` | `-O0 -g -DDEBUG` |
| `CC=clang` | 指定编译器 |
| `PREFIX` / `DESTDIR` | `install` 的安装前缀与打包根 |

```sh
# 无 XDP 环境（例如仅需 UDP 后端/容器/CI）
make SX_NO_XDP=1
make SX_NO_XDP=1 group-tools relay-tool

# 带丢包注入的调试构建
make SX_LOSSY=1 DEBUG=1

# 安装 / 打包
sudo make install PREFIX=/usr/local
make install DESTDIR=/tmp/pkg PREFIX=/usr
```

### 架构

Makefile 自动探测架构（`uname -m`，x86_64 / aarch64）：用 `$(CC) -print-multiarch`
定位 multiarch 头文件，XDP 程序用 `clang -target bpf` 编译；无需手工指定目标。

### CMake 备选

```sh
cmake -S . -B build-cmake && cmake --build build-cmake -j
cd build-cmake && ctest        # 运行单元/回环测试
```

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

详见 **使用手册 `docs/manual.md`**、设计文档 `docs/design.md`、示例 `examples/sx_p2p.c`、`examples/sx_group.c`。
多语言（Python/Java）绑定评估与计划见 `docs/ffi-evaluation.md`、`docs/ffi-todo.md`（当前聚焦 C 库，FFI 暂缓）。

## 测试

- `tests/test_frame.c`：帧构造/解析与校验和防御。
- `tests/test_proto.c`：协议头编解码边界。
- `tests/test_kcp.c`：注入 **丢包 + 相邻乱序**，验证 100/100 按序到达且确有重传。
- `tests/test_loopback.c`：UDP 后端端到端双向有序投递。
- `tests/test_xdp.c` + `scripts/veth_test.sh`：veth 上 AF_XDP native/copy 端到端。
- `tests/test_latency.c` + `scripts/xdp_latency.sh`：停等往返时延统计（min/avg/p50/p99/max），
  并在测试挂载期间用 `bpftool net show` / `bpftool prog show` / `bpftool map dump` 佐证
  流量确实经 XDP 处理（`sx_stats_map` 的 redirect 计数）。

### 实测参考（本机，停等 RTT，n=20000）

单进程（A/B 两端点在同一进程）：

| 路径 | min | avg | p50 | p99 | max |
| --- | --- | --- | --- | --- | --- |
| AF_XDP（veth，native+copy） | 1us | 1~2us | 2us | 2us | 11~17us |
| 内核 UDP（lo） | 4us | 4us | 4us | 6us | 55us |

双进程（server 与 client 各自独立进程，更接近真实）：

| 路径 | min | avg | p50 | p99 | max |
| --- | --- | --- | --- | --- | --- |
| AF_XDP（veth，native+copy） | 1us | 2us | 2us | 4us | ~425us |
| 内核 UDP（lo） | 0us | 3us | 3us | 5us | ~173us |

均为**往返时延**（含两端用户态处理），单向约为一半量级。双进程 `bpftool map dump`
显示两个进程各自的 XDP 程序重定向计数均约 4 万（2 万请求 + 2 万回显 + 控制帧），
证明双向报文都确实经过 XDP 重定向到 AF_XDP。

### 绑核（强烈建议）

不绑核时 max 抖动较大（UDP/lo 约 300~400us，AF_XDP/veth 可到数百 us），
主因是进程迁移与中断/软中断争抢；绑到两个不同的**性能核**后 max 降到约 20~30us：

| 路径 | 绑核 | p50 | p99 | max |
| --- | --- | --- | --- | --- |
| UDP/lo | 不绑 | 3us | 4us | ~408us |
| UDP/lo | s=4 c=5 | 2us | 3us | ~16us |
| AF_XDP/veth | 不绑 | 2us | 4us | 66~845us |
| AF_XDP/veth | s=1 c=2 | 2us | 4us | ~20us |
| AF_XDP/veth | s=2 c=3 | 2us | 5us | ~22us |

建议：server 与 client 各绑一个不同的性能核（本机 c0–c5，6.8GHz），避开中断核 c0；
iWLAN/WiFi 队列中断分布在 c0–c8，若追求极低抖动可把 IRQ 亲和性移出所用核
（`/proc/irq/*/smp_affinity_list`）。

```sh
# 双进程 + 绑核
SX_CPU_SERVER=2 SX_CPU_CLIENT=3 sudo scripts/xdp_latency_2p.sh build/test_latency
```

## AF_XDP 说明与限制

- attach 模式：native 优先，失败回退 generic；bind：zero-copy 优先，失败回退 copy（自动侦测）。
- AF_XDP 绑定单个 RX 队列；需将目标流量引导到该队列（RSS/ethtool 或单队列网卡）。
- ARP 由本库用户态处理（XDP 程序会重定向 ARP），因此**仅在同一二层网段内可直接解析**；跨网段需配置网关/静态映射。
- generic 模式强制 copy；zero-copy 需要网卡驱动支持。
- 组播在 XDP 后端需接口接受该组播（可能需要 `ip maddr add` 或 allmulti）。

## 可靠性语义

- **单播**：严格可靠有序。累积确认 `una` + 选择性确认 `ack_list` + 快速重传 + RTO 重传 + 拥塞/流控。
- **组播/广播**：尽力可靠 + 有序。发送端保留历史窗口并周期性广播高水位；接收端侦测空洞后回送 `NACK`，发送端从历史重发到组地址。不保证送达（无多方 ACK 汇聚）。
