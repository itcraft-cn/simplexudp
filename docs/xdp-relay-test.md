# XDP 三跳接力环测试（101 -> 102 -> 103 -> 101）

日期：2026-10-02　后端：AF_XDP（generic 模式，e1000 无 native）

## 1. 目标与拓扑

101 发送，102 转发给 103，103 转发回 101，101 统计（环回）。

```
 101(origin) ──10200──► 102(relay) ──10300──► 103(relay) ──10100──► 101(origin)
   listen:10100            listen:10200          listen:10300         统计
```

端口约定（各节点监听自己的 local_port，下一跳目的端口如下）：
- 101 监听 10100，发往 102:10200
- 102 监听 10200，发往 103:10300
- 103 监听 10300，发往 101:10100

## 2. 实现

- `tests/test_relay.c`：AF_XDP 数据面 + **应用层逐跳转发**。
  - origin：延迟后发送 count 条带序号报文，随后统计回收（校验序号连续）。
  - relay：收到后入**发送队列**（解耦收/发），主循环 `flush_queue` 转发给下一跳，
    避免发送窗口满导致丢包。
- 每节点各自在 enp0s9 挂自己的 XDP 程序（按本地端口重定向），互不冲突。
- 需要 root（AF_XDP）；`make relay-tool`。

## 3. 运行

```sh
# 103 relay
sudo ./build/test_relay relay  enp0s9 192.168.77.103 10300 192.168.77.101 10100 2000 0 60000
# 102 relay
sudo ./build/test_relay relay  enp0s9 192.168.77.102 10200 192.168.77.103 10300 2000 0 60000
# 101 origin（延迟 6s 等其它节点就绪）
sudo ./build/test_relay origin enp0s9 192.168.77.101 10100 192.168.77.102 10200 2000 6000 40000
```

## 4. 结果

```
101 origin: sent=2000 recv=2000 ooo=0 retrans=0 fast=0 timeouts=0
102 relay : fwd=2000 drop=0 pending=0
103 relay : fwd=2000 drop=0 pending=0
```

三跳环 2000 条全部回流，**零丢失、零乱序、零重传**。每跳各自可靠（KCP），
端到端因逐跳可靠而完整。

## 5. 说明与限制

- e1000 无 native XDP，各节点为 generic（SKB）模式，非线速。
- 每节点一个 AF_XDP 端点（各自网卡），互不冲突；同一网卡同时挂两个端点需共享 UMEM。
- 这是**应用层接力**（收后转发），非内核内纯 XDP redirect 链；每跳终结并重建。
- 若需“端到端一致语义”（如原始发送者身份），需在 payload 或协议头携带源标识。
