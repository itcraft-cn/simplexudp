# 基准与归因方法论：XDP / TC 的 CPU 收益到底从哪来

本文档固定两件事：

1. 本项目所有性能数字的**适用边界**（拓扑、驱动、内核、测量方式）；
2. 出现“XDP 比 UDP/TC 快多少”这类结论时，**必须做的归因步骤**与可复现配方。

配套脚本：`scripts/perf_skb_probe.sh`（bpftrace 计数 skb 生命周期，不依赖硬件 PMU）。

---

## 1. 被纠正的误区

> “XDP 比 TC 更早丢包，所以一定更省 CPU。”

挂点更早**不等于**工作量更少。真正决定成本的是：**XDP 程序执行时，内核有没有已经为这个包
构造 `sk_buff`。** 构造过，分配与初始化已经付账，XDP 只是把包拦在协议栈之外；没构造过，
`XDP_DROP` 才真正拿掉整段收包路径。

| 场景 | XDP 运行时的状态 | 真实节省 |
| --- | --- | --- |
| veth（容器常见软件网卡） | 包在到达 XDP 前已是 skb（发送侧构造），接收侧只做 `skb -> xdp_buff/xdp_frame` 转换 | **不省 skb 构造**，只省后续协议栈 |
| virtio-net / 物理网卡 native XDP | 驱动拿到原始帧，`xdp_buff` 直接建立在 page 上 | **省掉 skb 构造**与整条接收路径 |
| generic / skb 模式 XDP | skb 已存在，走 `do_xdp_generic` | 与 TC 同构，几乎无“早丢包”收益 |

AF_XDP 是另一条路径：它要的是“把包取到用户态”，收益来自绕开内核 UDP socket 的排队/唤醒/
系统调用，**不能归因为“省掉了 skb 分配”**。在 veth 上 AF_XDP 必然 copy，更不能套用 native
dropping 的结论。

---

## 2. 测量纪律（硬性）

1. **只做同一 Run 内的 A/B。** 两个方案的包数、过滤规则、发包端、绑核、队列配置必须一致。
2. **禁止跨机/跨 Run 比较绝对 cycles。** 频率、PMU、虚拟化、中断分布全在变，跨机差异没有解释力。
   允许搬运的是“同 Run 内 TC 与 XDP 的相对差”。
3. **每次记录环境**：内核版本、驱动与固件、是否 native/vPMU、RX 队列数与 IRQ 亲和、绑核方案、
   是否 zero-copy、MTU、报文大小与包数。
4. **小差异要重复。** 1%~3% 的差异在单次运行里可能是噪声，至少 3 次取中位数，或直接报告
   “在噪声内，无显著差异”。
5. **无 PMU 时不要编数。** 用 skb/协议栈计数给“半边故事”，足以否定或确认“省了没有”，
   但不要用软件事件冒充 cycles。

---

## 3. 工具分工

| 工具 | 看什么 | 前提 |
| --- | --- | --- |
| `perf stat -e cycles,instructions` | 每包指令/周期成本 | 硬件 PMU 或 vPMU；公有云标准实例、Lima/UTM 等常不可用 |
| `bpftool prog show` | BPF 程序自身耗时（`run_time_ns`/`run_cnt`） | 已 attach 的程序 |
| `bpftool net show dev IF` | 挂载点与模式（driver/generic/skb） | root |
| `bpftrace` kprobe | `__alloc_skb` / `kfree_skbmem` / `__netif_receive_skb_core` / `ip_rcv` 次数 | root；符号名随内核变化 |
| `scripts/perf_skb_probe.sh` | 上述计数的封装 + 判读 + 可选 perf | root |

### 3.1 用配套脚本

```sh
# 固定窗口采集（测试期间保持环境安静）
sudo scripts/perf_skb_probe.sh -d 10

# 随一条命令的生命周期采集，并按包数折算
sudo scripts/perf_skb_probe.sh -n 1000000 --perf -- ./build/test_throughput ...

# 结果写入文件，便于贴进测试记录
sudo scripts/perf_skb_probe.sh -n 1000000 -o build/skb-ab-xdp.txt -- ...
```

输出是一张计数表加一行判读，例如：

```text
| skb 构造合计 | 0 |
| __netif_receive_skb_core（进入协议栈） | 0 |
判读：skb 未建、协议栈未进：早丢包真正拿掉了分配与后续路径
```

反之若“skb 构造合计 ≈ 包数，进入协议栈 ≈ 0”，说明 XDP 只跳过了协议栈，
**分配已经付过**——这时声称“XDP 省 CPU”就是错的。

### 3.2 符号名会变

`__netif_receive_skb_core` 在部分内核上被编译成 `__netif_receive_skb_core.constprop.0`，
直接写死符号名会 attach 失败。脚本从 `/proc/kallsyms` 解析 `base` 或 `base.constprop.N`，
并跳过 CFI 的 `__pfx_` 前缀符号。手工写 bpftrace 时同理：

```sh
grep -E ' _(__netif_receive_skb_core|netif_receive_skb_core)' /proc/kallsyms
```

---

## 4. skb 计数的判读矩阵

| skb 构造合计 | 进入协议栈 | 含义 |
| --- | --- | --- |
| ≈ 包数 | ≈ 0 | 分配已发生，XDP 只省协议栈；**不能说省了包处理** |
| ≈ 0 | ≈ 0 | 分配与协议栈都没发生；这才是“决定不建” |
| ≈ 包数 | ≈ 包数 | XDP 未生效、未命中规则，或流量没经过该挂载点 |
| 明显小于包数但非 0 | 介于两者 | 多队列/多路径混杂，需拆分环境后重测 |

注意：不同内核与驱动走不同构造路径，`__alloc_skb`、`napi_alloc_skb`、`napi_build_skb`、
`build_skb` 不必然同时为 0。判断标准是**“这几项是否整体接近 0”**，而不是某一项。

---

## 5. TC ingress 与 XDP 的 A/B 复现配方

目标：同一个过滤条件（丢弃 UDP 目的端口 9999），只换挂载点。

### 5.1 两个 BPF 程序

`/tmp/opencode/xdp_drop_9999.c`：

```c
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/in.h>
#include <bpf/bpf_helpers.h>

SEC("xdp")
int xdp_drop_9999(struct xdp_md *ctx)
{
    void *data = (void *)(long)ctx->data;
    void *end  = (void *)(long)ctx->data_end;
    struct ethhdr *eth = data;
    struct iphdr *ip;
    struct udphdr *udp;

    if ((void *)(eth + 1) > end)
        return XDP_PASS;
    if (eth->h_proto != __builtin_bswap16(ETH_P_IP))
        return XDP_PASS;
    ip = (void *)(eth + 1);
    if ((void *)(ip + 1) > end || ip->protocol != IPPROTO_UDP)
        return XDP_PASS;
    udp = (void *)ip + ip->ihl * 4;
    if ((void *)(udp + 1) > end)
        return XDP_PASS;
    if (__builtin_bswap16(udp->dest) == 9999)
        return XDP_DROP;
    return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
```

`/tmp/opencode/tc_drop_9999.c`（`__sk_buff` 上下文，用 `bpf_skb_load_bytes` 避免直接指针的
边界检查负担）：

```c
#include <linux/bpf.h>
#include <linux/pkt_cls.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/in.h>
#include <bpf/bpf_helpers.h>

SEC("classifier")
int tc_drop_9999(struct __sk_buff *skb)
{
    struct ethhdr eth;
    struct iphdr ip;
    struct udphdr udp;

    if (bpf_skb_load_bytes(skb, 0, &eth, sizeof(eth)) < 0)
        return TC_ACT_OK;
    if (eth.h_proto != __builtin_bswap16(ETH_P_IP))
        return TC_ACT_OK;
    if (bpf_skb_load_bytes(skb, sizeof(eth), &ip, sizeof(ip)) < 0)
        return TC_ACT_OK;
    if (ip.protocol != IPPROTO_UDP)
        return TC_ACT_OK;
    if (bpf_skb_load_bytes(skb, sizeof(eth) + ip.ihl * 4, &udp, sizeof(udp)) < 0)
        return TC_ACT_OK;
    if (__builtin_bswap16(udp.dest) == 9999)
        return TC_ACT_SHOT;
    return TC_ACT_OK;
}

char _license[] SEC("license") = "GPL";
```

```sh
MULTIARCH=$(gcc -print-multiarch)
for p in xdp_drop_9999 tc_drop_9999; do
    clang -O2 -g -target bpf -I/usr/include/"$MULTIARCH" \
        -c /tmp/opencode/$p.c -o /tmp/opencode/$p.o
done
```

### 5.2 分别挂载

```sh
# 建议用两个 netns 各放一端，避免发往本机地址时走 lo（见 5.5）
IF=sxb

# A：TC ingress
tc qdisc add dev "$IF" clsact
tc filter add dev "$IF" ingress bpf da obj /tmp/opencode/tc_drop_9999.o sec classifier
sudo scripts/perf_skb_probe.sh -n 1000000 -o build/skb-tc.txt -- <发 100 万包的命令>

# B：native XDP（veth 上会落到 driver 模式的转化路径；物理口/virtio 才是真正的驱动层）
ip link set dev "$IF" xdpdrv obj /tmp/opencode/xdp_drop_9999.o sec xdp
sudo scripts/perf_skb_probe.sh -n 1000000 -o build/skb-xdp.txt -- <同一条发包命令>
```

清理：

```sh
ip link set dev "$IF" xdp off
tc filter del dev "$IF" ingress
tc qdisc del dev "$IF" clsact
```

### 5.3 预期与判读

- **veth**：TC 与 XDP 两边 `skb 构造合计` 都 ≈ 包数；XDP 的“进入协议栈”≈ 0。
  相对 cycles 差异应在噪声内（量级 1%~3%），**不应宣称 XDP 更省 CPU**。
- **virtio-net / 物理口**：XDP 侧 `skb 构造合计` ≈ 0，cycles 显著下降（数十个百分点）。
- 若两块网卡结果相反，先检查 `bpftool net show dev "$IF"` 的挂载模式，再检查内核版本。

### 5.4 本机复现记录（2026-11-03，内核 6.12，veth）

拓扑：两个 netns 各放一端 veth，包从 `ns1` 发往 `ns2`（目的 `10.99.0.2:9999`，1 万～2 万包，
限速约 30k pps），过滤程序只丢弃目的端口 9999 的 UDP。

| 场景 | skb 构造合计 | `ip_rcv`（L3） | 判读 |
| --- | --- | --- | --- |
| 无过滤 | 20224（≈ 包数） | 20009 | 正常进入协议栈 |
| TC ingress drop | 19694（≈ 包数） | 3 | 分配已付，只省掉上层 |
| native XDP drop（veth） | 15504（≈ 0.78×包数） | 6 | 接收侧仍发生 skb 构造 |

结论与文章一致：**veth 上 XDP_DROP 并没有做到“不建 skb”**，只是拦在协议栈之外；
TC 与 XDP 在这里的差别是“在 core 内部还是 core 之前执行”，不是“建没建 skb”。

XDP 场景计数略低于包数（0.78×），是因为本机测试中发送端与接收端同在一台机器：发送侧
`udp_sendmsg` 路径已经为每个包构造了 skb，veth 的 XDP 快速路径随后把 skb 转成 `xdp_frame`
交给对端，接收侧不再新建。**这笔分配成本仍然记在本机账上**，容器场景（发送/接收在同一
宿主机、不同 netns）同样如此。因此不能用“接收侧没再建 skb”推断“XDP 省了分配”。

同机同配置连跑 3 轮的 `cycles/包` 分别为 TC 22k / 54k / 99k、XDP 46k / 48k / 34k——
在未隔离的桌面上，cycles 的 Run 间波动远大于方案差异，**这些数字不能用于比较**。
这正是“只比同 Run 内相对差、且必须隔离环境”的现实依据。

### 5.5 三个容易踩的坑

1. **同 netns 下把包发往本机 IP 会走 lo。** 两个 veth 端点都在同一 netns 且目标地址是本机
   地址时，`ip route` 会命中 `local` 路由，包根本不经过 veth，TC/XDP 全部“未生效”。
   必须把两端放进不同 netns（如上），或用 AF_PACKET 直接构造以太帧。
2. **bpftrace 在高 pps 下会丢事件。** 直接在 100 万包/秒的流上计数，`__alloc_skb` 可能只记到
   几千次，结论完全失真。把发包限速到几万 pps，或分批采集后相加。
3. **TC ingress 在 `__netif_receive_skb_core` 内部执行。** 用该函数计数无法区分“TC 丢弃”与
   “继续走协议栈”。要判断是否向上交付，用 `ip_rcv`（L3 入口）作判据；`perf_skb_probe.sh`
   已同时输出两者。

---

## 6. 本项目性能数字的适用边界

| 结论来源 | 可外推到 | 不可外推到 |
| --- | --- | --- |
| veth + AF_XDP `native+copy` RTT（`README` 本机小节） | 同机、同内核、同 veth 配置的容器内通信 | 云主机 virtio、物理网卡、zero-copy 场景 |
| 10.45/10.46 mlx5 25G 跨机数据 | 同为 mlx5 + native XDP 的部署 | veth/e1000 等无 native XDP 的环境 |
| e1000 VM（generic+copy，`docs/vm-udp-xdp-test.md`） | 同类无 native XDP 的虚拟网卡 | 具备 native XDP 的网卡 |
| 纯 XDP_TX 微基准 | 反射路径自身的开销 | 带 KCP/用户态处理的端到端时延 |

写结论时必须带上：**拓扑 + 驱动 + XDP 模式 + 内核版本 + 是否 zero-copy**。
缺任何一项，数字就不可复用。

### 6.1 选型速查

| 场景 | 建议 |
| --- | --- |
| 容器 veth，早期丢包/限速 | 优先 TC ingress；或评估 netkit（内核 ≥ 6.7） |
| 容器 veth，需要把包取到用户态 | AF_XDP 仍可用，但要明确收益来自绕开内核栈，且必然是 copy |
| virtio-net / 物理口，早期丢包/转发 | native XDP 收益最大，可在驱动层决定不建 skb |
| 无 vPMU 的云实例 | 用 `perf_skb_probe.sh` 的计数代替 cycles，结论只谈“有没有建 skb” |

---

## 7. 测试记录模板

```text
日期/主机：
内核：            驱动与固件：
网卡类型：        XDP 模式（driver/generic/skb）：
RX 队列 / IRQ 亲和：      绑核：
zero-copy：        MTU / 报文大小 / 包数：
A 方案（挂载点/程序）：
B 方案（挂载点/程序）：
同 Run 内 cycles/包：A=      B=      差=
skb 构造合计：A=      B=
进入协议栈：A=      B=
BPF 程序 run_time_ns：A=      B=
判定：省掉了分配 / 只跳过了协议栈 / 无显著差异
```
