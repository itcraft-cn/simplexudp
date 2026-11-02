# 后续优化 TODO（2026-11-02）

## 已完成

- **P0 UDP 接收硬件时间戳**：`rx_ts_mode` 显式开启；UDP 后端返回 PHC 原始时间；
  `on_recv_ex` / `sx_recv_ex` 透出；45/46 已验证。
- **P2 AF_XDP 时间戳探测**：启动时检测内核版本与能力；当前 5.18/4.18 判定为
  `SX_RX_TS_ERR_KERNEL`，`AUTO` 回退软件时间戳；状态写入日志和 `sx_stats_t`。

## P1：单队列 PPS / CPU 基线

- 目标：在 45/46 单队列 AF_XDP copy 上测真实 PPS、msg/s、CPU 占用、软中断占比。
- 目的：为多队列设计提供依据，避免使用未经证实的“单核 1000 万~1400 万 PPS”。
- 验收：固定报文大小、绑核、IRQ 亲和性，输出可复现脚本与原始日志。

## P1：多队列 / 多 xsk

- 现状：一个端点一个 xsk，`cfg.xdp_queue` 单队列；BPF 已用 `rx_queue_index` 作 XSKMAP 键。
- 方案 A（推荐先做）：一个端点内创建多个 xsk，每队列一个，`sx_update` 轮询全部；
  TX 按流选择队列。
- 方案 B：多端点共享 XDP 程序，每端点绑定不同队列。
- 难点：共享 XDP 程序生命周期、ARP/组播归属、每队列统计、IRQ/线程绑核。
- 验收：两队列各收各的流；单队列关闭不影响其他队列；吞吐随队列数近线性。

## P2：共享 XDP 程序

- 现状：`XDP_FLAGS_UPDATE_IF_NOEXIST`，同一网卡只允许一个 SX XDP 程序。
- 目标：进程级 XDP 管理器，共享 BPF 对象、`xsks_map`、端口配置；引用计数，最后一个退出时 detach。
- 验收：同网卡两个 SX 端点同时工作；一个退出不影响另一个。

## P2：AF_XDP 硬件时间戳数据面

- 现状：仅探测并回退；Linux < 6.7 无 XDP RX metadata timestamp。
- 目标：内核 >= 6.7 且驱动支持时，通过 XDP metadata/hints 读取 RX PHC 时间，
  填充 `sx_pkt_info_t`。
- 依赖：内核升级、libbpf/headers 支持、驱动验证。
- 验收：AF_XDP `rx_ts_mode=HARDWARE`，与 UDP 硬件时间戳对照误差在 PTP 精度内。

## P3：发送硬件时间戳

- 目标：`sx_send_ex` 或发送完成回调，返回 TX PHC 时间。
- 路径：UDP error queue；AF_XDP completion 路径（需内核/驱动支持）。
- 验收：TX 时间与 `test_hwts` 结果一致。

## 其他

- zero-copy 的吞吐价值评估：单包 RTT 已证明 ZC 不快；应在批量/高 PPS 场景测。
- `chrt -f` 在当前环境被拒绝（Operation not permitted），需确认 CAP_SYS_NICE/SELinux。
- 组播/广播大消息仍是 best-effort，严重丢包下 NACK 恢复时间需评估。

