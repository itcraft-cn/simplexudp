# 审查报告核实与高价值 TODO（2026-11-02）

## 1. 核实范围与方法

- 基线：`6bc4595`（`dev`）。
- 对照：`docs/review/code-review-20261102-001-ansi_c.md`。
- 方法：逐条阅读源码、调用路径与锁边界；不重复运行审查工具，只核实结论是否与当前代码一致。
- 结论标记：**成立**、**部分成立**、**成立但降级**、**需产品决策**。

## 2. 逐条核实

### P0

| 编号 | 结论 | 核实说明 |
| --- | --- | --- |
| P0-1 内存池锁外释放 | 成立 | `sx_ep_flush` 解锁后 `sx_pool_free`（`src/sx_endpoint.c:142-158`）；`sx_recv_common` 解锁后释放（`:899-916`）；`sx_stop` 无锁清队列（`:607-629`）。`sx_update` 在锁内 `sx_pool_alloc`，并发时确实存在空闲链表竞态。 |
| P0-2 `sx_stop` 并发释放 | 成立 | `sx_stop` 未取 `ep->lock`，关闭 io、销毁链路、释放队列（`:579-630`）；`sx_update` 可能在另一线程持锁遍历同一批链路。`sx_run` 的语义要求另一线程 stop，UAF 路径真实存在。 |
| P0-3 单行多变量声明 | 成立但降级 | `src/sx_io_xdp.c:395-396` 确为 `uint32_t sip = 0, dip = 0;` 与 `uint16_t sp = 0, dp = 0;`。这是规范强制项，但无运行时风险，建议降为 P2 格式项，不占 P0 修复窗口。 |

### P1

| 编号 | 结论 | 核实说明 |
| --- | --- | --- |
| P1-1 组播/单播路由歧义 | 成立 | `ep_process_packet` 先按 `conv` 查 group（`src/sx_endpoint.c:273-288`），`find_group` 只匹配组链路（`:241-256`）；单播链路使用同一个 `conv`，因此混用时单播报文会被送入 group。 |
| P1-2 无会话重同步 | 成立 | KCP 的 `HELLO` 只在 `!established` 时处理（`src/sx_kcp.c:689-698`），已建链对端重启后不会复位；组播来源超时把 `rcv_nxt` 置 0（`src/sx_group.c:567-578`），晚加入成员也从 0 开始，序号落后时无法快进。 |
| P1-3 链路移除后队列悬垂指针 | 成立 | `sx_link_remove` 持锁后立即 `link_destroy`（`src/sx_endpoint.c:803-830`）；`sx_rx_item_t` 保存裸 `sx_link_t *`（`:93-125`），已入队消息在回调/`sx_recv` 时仍会拿到已释放链路。 |
| P1-4 `on_event` 在锁内回调 | 成立 | `sx_ep_emit` 直接调用用户回调（`src/sx_endpoint.c:86-90`），调用点包括 `add_link_common`、`ep_link_timeouts`、自动建链（`:310`、`:363-364`、`:767`），均在端点锁内。 |
| P1-5 自动建链无认证/无限速 | 部分成立 | 自动学习确实只校验 `conv`（`src/sx_endpoint.c:289-312`），默认 `conv=1`，无认证；但链路数有 `max_peers` 上限，属于“可被耗尽/伪造”的风险，不是无限内存增长。严重度 P1（安全/可用性），不是 P0。 |
| P1-6 分片发送中途失败不回滚 | 部分成立 | `sx_kcp_send_message` 先检查窗口（`src/sx_kcp.c:302-312`），正常情况下不会半途窗口失败；但 `kcp_send_cmd` 仍可能因 `NOMEM` 失败，已发分片不会回滚（`:341`）。组播版同理（`src/sx_group.c:269`）。触发概率较低，但失败语义不干净。 |
| P1-7 重组失败仍 ACK | 成立 | `kcp_input_push` 先 `rcv_nxt++`（`src/sx_kcp.c:548-554`），再调用重组；`sx_frag_reasm_input` 返回 -1 时只增加 `dropped`，随后仍 `kcp_add_ack`。内存压力下会静默丢消息且不重传。 |
| P1-8 大消息强行拉满 cwnd | 成立，需产品决策 | `sx_kcp_send_message` 在 `cnt > cwnd` 时把 `cwnd = snd_wnd`（`src/sx_kcp.c:307-311`）。这是为支持 256KB 一次性入窗的刻意实现，但确实绕过慢启动；应按链路类型/配置决定，而不是默认强拉。 |
| P1-9 CMake 与测试门禁不对齐 | 成立，拆分处理 | CMake 只嵌入默认 BPF blob，没有 `SX_XDP_TS_EMBEDDED`，时间戳在 CMake 产物中不可用；`make check` 只跑 8 个单测，组播/XDP/时延/大消息未进门禁。建议拆成“构建一致性”和“测试门禁”两个 TODO。 |

### P2

| 编号 | 结论 | 核实说明 |
| --- | --- | --- |
| P2-2 start 前加组播不入组 | 成立 | `add_link_common` 仅在 `ep->io != NULL` 时调用 `join_multicast`（`src/sx_endpoint.c:762-765`），start 后不会补入组。 |
| P2-3 XDP TX 预留失败未 cancel | 成立 | `xdp_sendto` 先 `xsk_ring_prod__reserve`，再 `xdp_frame_alloc`；分配失败直接返回，未 `cancel`（`src/sx_io_xdp.c:244-250`、`:323-329`）。当前 `frame_free` 预检降低了触发概率。 |
| P2-4 `snd_wnd` 可超过配置 | 部分成立 | `kcp_input` 收到更大 `wnd` 时 `snd_wnd = min(h.wnd, send_cap)`（`src/sx_kcp.c:661-665`），`send_cap=2*send_window`，因此可能大于 `send_window`。是否错误取决于“send_window 是软目标还是硬上限”，需先明确语义。 |
| P2-5 死字段/周期广播 | 成立 | `adv_snd` 只写不读（`src/sx_group.c:55`、`:200`、`:591`）；`probe_wait/probe_cnt` 声明未使用（`src/sx_internal.h:336-337`）；`WINS` 每 200ms 无条件广播（`:588-593`）。 |
| P2-6 ARP 死分支 | 成立 | `sx_arp_input` 外层 `local_ip == 0 || tpa == local_ip` 与内层条件重复（`src/sx_arp.c:263-267`），`local_ip==0` 时内层永不成立。 |
| P2-7 `on_log` 持锁回调 | 成立 | `sx_log` 持 `log_lock` 调用户 `on_log`（`src/sx_util.c:25-50`），回调内再写日志会自锁。 |
| P2-8 统计口径 | 成立 | `rx_packets` 在分片路径按分片自增；`tx_packets` 在发送失败时也可能自增。与“消息数”注释不一致。 |
| P2-9 组播来源超时未重置 frag | 成立 | 超时分支只清 ring、置 `used=0`（`src/sx_group.c:567-578`），未 `sx_frag_reasm_reset`；半包内存留到下次复用。 |
| P2-10 测试 `-Wshadow` 告警 | 成立 | 测试构建有 shadow 告警；`src/` 未开 `-Werror`，CI 约束偏松。 |

## 3. 高价值 TODO 排序

排序依据：先修“会崩溃/数据错乱”，再修“协议正确性”，最后修生命周期、构建与风格。

| 顺序 | TODO | 价值 | 成本 | 依赖 |
| --- | --- | --- | --- | --- |
| 1 | 内存池并发保护（P0-1） | 极高：堆损坏/数据错乱 | S | 无 |
| 2 | `sx_stop` 状态机与加锁（P0-2） | 极高：UAF/崩溃 | M | 与 1 同批 |
| 3 | 组播/单播路由 demux（P1-1） | 高：单播被组引擎吞掉 | S | 无 |
| 4 | 分片重组失败不 ACK（P1-7） | 高：静默丢消息 | S/M | 无 |
| 5 | 会话重同步（P1-2） | 高：对端重启/晚加入永久失联 | L | 先定协议策略 |
| 6 | 分片发送失败回滚（P1-6） | 中高：半包/重组缓冲泄漏 | M | 与 4 同批 |
| 7 | 链路生命周期/引用计数（P1-3） | 高：回调悬垂指针 | M | 与 2 相关 |
| 8 | `on_event` 锁外投递（P1-4） | 高：回调内 stop/remove 崩溃 | M | 与 2 相关 |
| 9 | 大消息 cwnd 策略（P1-8） | 中：拥塞语义 | S | 需产品决策 |
| 10 | 构建一致性与测试门禁（P1-9） | 中：CMake 功能缺失、回归缺口 | S/M | 无 |
| 11 | 自动学习限速/认证边界（P1-5） | 中：DoS/注入风险 | M | 安全策略 |
| 12 | P2 清理项（P2-2..P2-10） | 低~中：防御性/可维护性 | S/M | 无 |
| 13 | P0-3 单行声明（降级） | 低：格式规范 | XS | 无 |

## 4. 建议实施批次

### 批次 A：并发与生命周期底座

- P0-1、P0-2 一起修：统一 `ep->lock` 保护 pool、io、links；增加 RUNNING/STOPPING/STOPPED 状态；
  `sx_stop` 先停线程/置标志，再持锁释放；`sx_ep_flush`/`sx_recv` 的 pool 归还放回锁内。
- 验收：TSan 下 `rx_thread=1 + 应用线程 sx_recv`、`sx_run + 另一线程 sx_stop` 各 1000 次；
  ASan 无 UAF/双重释放。

### 批次 B：协议正确性

- P1-1 路由 demux、P1-7 重组失败不 ACK、P1-6 分片回滚；
- P1-2 会话重同步：先定“对端重启/晚加入”的策略（重置、快进、拒绝），再实现；
- 验收：单端点混用单播+组播；对端重启；组播晚加入；内存注入失败。

### 批次 C：回调与构建

- P1-3 链路引用计数、P1-4 事件队列、P1-9 CMake/测试门禁；
- 验收：回调内 `sx_link_remove`/`sx_stop` 不崩溃；CMake 产物支持时间戳；默认 check 覆盖组播/大消息。

### 批次 D：策略与清理

- P1-8 cwnd 策略、P1-5 限速/认证边界、P2 清理项、P0-3 格式。

## 5. 需要先决策的问题

1. `send_window` 是硬上限还是软目标？决定 P2-4 是否修。
2. 大消息是否允许绕过慢启动？决定 P1-8 是“配置开关”还是“默认禁止”。
3. 会话重同步策略：对端重启时重置接收窗口，还是要求应用重建链路？
4. 自动学习默认是否关闭/仅白名单？决定 P1-5 的默认行为。

