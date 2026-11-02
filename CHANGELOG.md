# 变更记录

本文件记录 simple-xudp (SX) 的功能变更、修复与测试文档更新。
格式参考 Keep a Changelog，版本遵循语义化版本。

## [Unreleased] - 2026-11-02

### Added

- 接收硬件时间戳：默认关闭，`rx_ts_mode` 显式开启；UDP 后端通过 `SO_TIMESTAMPING`
  返回 PHC 原始时间，`on_recv_ex`/`sx_recv_ex` 透出 `sx_pkt_info_t`。
- AF_XDP 时间戳：启动时检测内核；>=6.7 时加载设备绑定的 XDP metadata 程序，
  通过 `bpf_xdp_adjust_meta` + `bpf_xdp_metadata_rx_timestamp` 读取 RX PHC 时间；
  内核 <6.7 或驱动不支持时给出原因，`AUTO` 回退软件时间戳。
- 单播消息自动拆包/重组：新增 `SX_CMD_PUSH_FRAG`、分片头 `msg_id/frag_idx/frag_cnt/total_len`、
  按链路重组缓冲；`max_message` 默认 64KB，硬上限 256KB（`f3f11a2`）。
- 组播/广播大消息分片：按来源重组，NACK 重传保留 `PUSH_FRAG`；`group_reorder`/`group_history`
  按最大消息分片数自动提升（`48a72d1`）。
- 发送/接收窗口自动提升：按 `ceil(max_message/(max_payload-12))` 计算，硬下限 256、上限 65536；
  大消息分片数超过 `cwnd` 时临时提升 `cwnd` 到 `snd_wnd`（`c26e91a`、`8deeafd`、`6fbb748`）。
- 静态 ARP/MAC API：`sx_add_static_arp`、`sx_add_unicast_mac`；静态项不可被 ARP 应答覆盖、不老化；
  支持跨网段 `gateway_ip`/`netmask`（`bc8cd2d`）。
- 拥塞控制开关 `SX_CC_OFF`：窗口固定为 `send_window`，超时只做 RTO 退避（`bc8cd2d`）。
- UDP 缓冲配置 `udp_rcvbuf`/`udp_sndbuf`，优先 `SO_*BUFFORCE`，并记录实际值（`d7c3186`）。
- RTO 退避开关 `rto_backoff`：可关闭指数退避，仅保留重传（`d7c3186`）。
- 统一 IO 微基准：
  - 纯 XDP_TX 反射与加载器（`ebcec3d`）；
  - UDP/AF_XDP 统一发送端 RTT 对比（`846946b`、`1b75970`）；
  - 纯 AF_XDP mirror（`ee75afb`）；
  - zero-copy mirror 对比（`bc94793`）；
  - busy-poll 模式（`0c7bab7`）。
- 硬件时间戳与 PTP：
  - `test_hwts` TX/RX PHC 时间戳单向测量（`ee3b0ba`）；
  - 10.45 PTP master、10.46 PTP slave，`master_offset` 约 -8ns（`f933734`）；
  - 10.45 NTP 改为与 10.46 同源 `192.168.12.1`（`f28c7c0`）；
  - 时间同步与 PHC 现状记录（`dfb0fd8`）。

### Fixed

- 旧版 `xxd` 不支持 `-n` 导致 BPF blob 为空；`scripts/gen_blob.sh` 改为可移植生成并校验非空（`27f73f8`）。
- 残留 XDP 程序导致 `bpf_xdp_attach` 静默 no-op；attach 前查询、attach 后校验 program id，
  失败时明确报错（`b1479cd`）。
- 大消息在慢启动下因 `cwnd < 分片数` 返回 `SX_ERR_WINDOW`；首条大消息临时提升 `cwnd`（`8deeafd`）。
- 显式配置的 `send_window`/`recv_window` 超过硬上限时未钳制；增加 65536 上限（`6fbb748`）。

### Changed

- `max_message` 默认 64KB、硬上限 256KB；`send_window`/`recv_window` 自动下限 256（`c26e91a`）。
- 单播、组播、广播均支持 256KB 自动拆包/重组；组播/广播仍为 best-effort 语义。
- 文档增加静态 ARP、CC、UDP 缓冲、RTO、PTP、硬件时间戳、AF_XDP 基准与 45/46 测试记录
  （`e17b283`、`5c612f8`、`e0b6c74`、`740dce1`、`a8d48e6`、`b40d31e`）。

### Tests

- 新增 `test_frag`（200KB KCP 分片）、`test_arp`、`test_cc`，扩展 `test_loopback`（200KB 分片）。
- 45/46 实测：
  - 单播 256KB：发送成功、ACK、重组、内容校验通过（`b40d31e`）；
  - 组播/广播 200KB、256KB：全部通过（`48a72d1`）；
  - AF_XDP native+copy 跨机 p50 16~21us、p99 31~38us；
  - 纯 XDP_TX 硬件时间戳单向 0.38~0.41us；
  - AF_XDP copy mirror p50 10~12us；zero-copy mirror p50 18us（未体现收益）。

## [0.1.0] - 2026-10-02

### Added

- AF_XDP 与内核 UDP 双后端、线格式互操作（`8fe44ab`）。
- KCP 风格可靠传输：滑动窗口、累积/选择性确认、快速重传、RTO 退避、RTT 估计、流控/拥塞。
- 点播/组播/广播语义；组播/广播高水位 + NACK 尽力可靠（`8fe44ab`、`828688d`、`53b3543`）。
- UDP-only 构建 `SX_NO_XDP`、丢包/乱序注入 `SX_LOSSY`、吞吐测试（`c0ba660`）。
- 时延/吞吐测试与绑核脚本（`744b798`、`231aa7c`、`3c98adb`）。
- 三跳 XDP 接力环测试（`4d463e5`）。
- 三节点组播/广播与单播 mesh 测试（`3307181`）。
- CMake 备选构建、安装目标与完整构建文档（`9a451b7`、`7cc0736`）。
- 应用层接入代价评估、FFI 评估与待办（`1f4f484`、`1eb1694`、`7e30736`）。

### Fixed

- 滞后 ACK 导致序号扫描死循环（`3c98adb`）。
- 代码审查 H/M/L 问题：广播路由、资源泄漏、XDP 端口校验、`sx_recv` 截断、线程契约等（`9a451b7`）。
- 点评 7 项改进：回调锁外投递、MTU 联合预算、内存池、XDP 可观测性、常量可配置等（`fb7ff10`）。

### Changed

- 组播支持指定接口 `multicast_if`；UDP 后端支持广播（`53b3543`、`828688d`）。
- 文档补充 VirtualBox、跨网、组播/广播、代码审查与三节点测试记录
  （`1cec728`、`e466fa7`、`63ad9a9`、`dadcdfd`、`404d3f9`、`8859177`）。

### Tests

- 单元/回环测试、AF_XDP veth 端到端、双进程时延、吞吐与长稳。
- 25G 直连 UDP：2%/5% 丢包零丢失；2000 万条长稳 RSS 稳定。

## [0.1.0] 之前

- `8fe44ab` 初始实现：XDP/AF_XDP 可靠通讯库、UDP 后端、基础协议与测试。

