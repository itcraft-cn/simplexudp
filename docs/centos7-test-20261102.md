# 10.45 本机测试记录（2026-11-02）

## 1. 环境

| 项 | 值 |
| --- | --- |
| 主机 | eraytlnx45 / 10.45 |
| OS | CentOS Linux 7.9.2009 |
| 内核 | 5.18.10-1.el7.elrepo.x86_64 |
| 编译器 | gcc 4.8.5、clang 11.0.0 |
| libbpf | 1.3.0（系统） |
| libxdp | 无系统包，临时构建 1.4.3 到 `/tmp/xdp-install` |
| BCM5720 | `em1`（管理口，192.168.10.45）、`em2`（无载波） |
| Mellanox | `p3p2`（25G，192.168.111.45，native XDP 可用） |

网络管理走 `em1`，数据测试网走 `p3p2`。为避免影响 SSH，未在 `em1` 上挂载 XDP 数据面。

## 2. 构建与单元测试

代码同步到 `~/sx-test` 后执行：

```sh
make SX_NO_XDP=1 check
```

结果：构建无警告，`test_frame`、`test_proto`、`test_kcp`、`test_loopback`、`test_recv`、`test_arp`、`test_cc` 七项全部通过。GCC 4.8.5 与 C99 构建可用。

## 3. 发现的移植性缺陷

CentOS 7 自带 `xxd V1.10` 不支持 `-n` 选项。原 Makefile 管道为：

```sh
xxd -i -n sx_xdp_kern_o ... | sed ... > sx_xdp_kern_blob.c
```

`xxd` 失败后 `sed` 仍以 0 退出，生成了 0 字节的 `sx_xdp_kern_blob.c`，最终链接报
`sx_xdp_kern_o` 未定义。该问题只在旧 `xxd` 上出现，本地新版 `xxd` 不会复现。

修复：

- `scripts/gen_blob.sh` 改为先执行可移植的 `xxd -i`，再从输出中提取默认符号并重命名；
- 增加输入存在性、符号提取、输出非空校验；
- Makefile 统一调用该脚本。

修复后在 10.45 上 XDP 构建成功，blob 约 95 KB。

## 4. AF_XDP 测试

libxdp 1.4.3 临时构建到 `/tmp/xdp-install`，未覆盖系统库。为满足编译，补充了 5.18 内核的
`linux/btf.h`、`linux/if_xdp.h`、`linux/if_link.h`、`linux/bpf.h` 到 `/usr/include/linux`。

### 4.1 veth 端到端

| 场景 | 结果 |
| --- | --- |
| 自动 ARP | 双向 1000/1000，`ooo=0`，PASS |
| `SX_STATIC_ARP=1` | 双向 1000/1000，`ooo=0`，PASS |
| `SX_STATIC_ARP=gateway`（/32 + 网关） | 双向 1000/1000，`ooo=0`，PASS |

veth 上 zero-copy 返回 `EOPNOTSUPP`，自动回退 copy，符合预期。

### 4.2 BCM5720（tg3）

- `em2` 无载波，无法做真实流量测试。
- 在 `em2` 上启动 AF_XDP，驱动报
  `Underlying driver does not support XDP in native mode`，自动回退 generic。
- 结论：该内核下的 `tg3` 不支持 native XDP；BCM5720 只能走 generic/copy，性能不适合作为低延迟数据面。
- `em1` 承载 SSH，未挂 XDP，避免 ARP 重定向影响管理面。

### 4.3 Mellanox p3p2（25G）

- native XDP attach 成功。
- zero-copy bind 返回 `EINVAL`，自动回退 copy。
- 用独立的最小 libxdp 探针复核：队列 0/1、frame size 2048/4096 均返回 `EINVAL`，排除本库参数问题。
- 结论：当前主机/驱动/固件组合下 zero-copy 不可用，需后续单独排查（网卡模式、固件、内核配置）。
- 已在库内补充 zero-copy 失败日志，现场可直接看到 errno 与回退行为。

## 5. 拥塞控制开关实测

### 5.1 回环时延

`test_latency udp-client/server`，127.0.0.1，20000 次停等：

| 模式 | min | avg | p50 | p99 | max | cwnd |
| --- | --- | --- | --- | --- | --- | --- |
| `SX_CC_ON` | 6us | 12us | 13us | 17us | 46us | 324 |
| `SX_CC_OFF` | 8us | 12us | 13us | 16us | 60us | 256 |

无丢包时两者时延一致；`SX_CC_OFF` 的窗口固定为 `send_window=256`。

### 5.2 回环吞吐

`test_throughput udp-client/server`，300000 条 × 1024 字节：

| 模式 | 客户端 MB/s | 重传 | 超时 |
| --- | --- | --- | --- |
| `SX_CC_ON` | 251.49 | 0 | 0 |
| `SX_CC_OFF` | 249.41 | 0 | 0 |

无丢包时吞吐一致。

### 5.3 注入 2% 丢包

`SX_LOSSY=1` 构建，发送端 `SX_LOSSY_DROP_PPM=20000`，100000 条：

| 模式 | 客户端 MB/s | 重传 | 快速重传 | 超时 |
| --- | --- | --- | --- | --- |
| `SX_CC_ON` | 112.40 | 74866 | 74858 | 8 |
| `SX_CC_OFF` | 118.07 | 112983 | 112958 | 25 |

结论：在 2% 丢包下，`SX_CC_OFF` 并未减少重传，反而因窗口更大产生更多重复重传。
“关闭拥塞控制一定更快”不成立；它只适用于无丢包或极低丢包的专线，且必须结合快速重传行为评估。

## 6. UDP 缓冲与 RTO 退避补充验证

新增配置：`udp_rcvbuf`/`udp_sndbuf`（0=系统默认，优先 `SO_*BUFFORCE` 再回退普通
`setsockopt`，日志打印实际值）与 `rto_backoff`（1=指数退避，0=固定 RTO）。

10.45 回环、发送端注入丢包、100000 条 × 1024B（2%）：

| 配置 | MB/s | 重传 | 超时 |
| --- | --- | --- | --- |
| `rto_backoff=1, cc=1` | 144.45 | 73188 | 10 |
| `rto_backoff=0, cc=1` | 119.35 | 108343 | 0 |
| `rto_backoff=0, cc=0` | 123.75 | 104430 | 12 |
| `rto_backoff=1, cc=0` | 120.49 | 105599 | 7 |

5% 丢包、50000 条：

| 配置 | MB/s | 重传 | 超时 |
| --- | --- | --- | --- |
| `rto_backoff=1` | 106.65 | 75019 | 26 |
| `rto_backoff=0` | 117.38 | 63029 | 11 |

结论：本组回环测试中，关闭 RTO 退避并非稳定收益；2% 丢包时默认退避反而最好，
5% 丢包时固定 RTO 略优。真正主导重传的是快速重传，而不是 RTO。该开关应作为现场可调项，
不能作为默认优化对外承诺。UDP 缓冲方面，`udp_rcvbuf=65536` 时内核实际返回 131072（内核翻倍）。

## 7. 结论

1. 代码在 CentOS 7 + GCC 4.8.5 + 内核 5.18 上可构建、单测全过。
2. 旧版 `xxd` 的 blob 生成缺陷已修复，这是本次现场测试最有价值的移植性修复。
3. BCM5720 在该内核下无 native XDP，只能 generic/copy；不适合低延迟数据面。
4. Mellanox p3p2 可 native XDP，但 zero-copy 当前不可用，已确认不是库参数问题。
5. 静态 ARP 与跨网段网关路径已在 veth XDP 上端到端通过。
6. `SX_CC_OFF` 的收益依赖链路质量，不能作为无条件优化项对外宣传。

## 8. 后续

- 在 10.45/10.46 之间用 `p3p2`（192.168.111.45）与 `ens3f1`（192.168.111.46）做真实跨机测试。
- 继续排查 p3p2 zero-copy `EINVAL` 的根因，确认是否为网卡模式/固件/内核配置限制。
- 如需在 BCM5720 上验证 generic XDP，可使用 `em2` 并接入对端链路，但不要占用管理口 `em1`。
