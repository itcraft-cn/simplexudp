# SX 跨网测试记录

日期：2026-10-01　后端：内核 UDP（`SX_NO_XDP=1` 构建）　库版本：0.1.0

## 1. 测试环境

| 项 | host45 (eraytlnx45) | host46 (eraytlnx46) |
| --- | --- | --- |
| 登录 | tomcat@192.168.10.45（SSH 免密） | tomcat@192.168.10.46 |
| 内核 | 5.18.10 (elrepo) | 4.18.0-193.el8 |
| gcc | 4.8.5 | 8.5.0 |
| CPU | 40 | 40 |
| 管理网 | em1 = 192.168.10.45 | eno1 = 192.168.10.46 |
| 互联网卡（数据面） | p3p2 = 192.168.111.45（25G） | ens3f1 = 192.168.111.46（25G） |

- SSH/控制走 10.x；**数据面走 111.x 的 25G 直连**（MTU 1500）。
- 两机均可 ping 通对方 10.x 与 111.x。
- 构建：`make SX_NO_XDP=1 SX_LOSSY=1 -j8 all tests-latency tests-throughput`
  （SX_NO_XDP：不依赖 libbpf/libxdp/clang；SX_LOSSY：编译进丢包/乱序注入）。

## 2. 时延（停等 RTT，2000 次）

服务端 46:40000，客户端 45 -> 111.46:40000。

```
rtt(us): min=10 avg=15 p50=14 p99=36 max=46 n=2000
```

25G 直连、内核 UDP 下 RTT 约 14~15us（p99 36us）。

## 3. 吞吐（双向分别测，各 2,000,000 条 x 1400B = 2.8GB）

| 方向 | 客户端耗时 | 吞吐 | msg/s | 客户端 outstanding | 服务端收到 |
| --- | --- | --- | --- | --- | --- |
| 45 -> 46 | 5.78 s | 484 MB/s | 345,810 | 0 | 2,000,000 |
| 46 -> 45 | 5.07 s | 552 MB/s | 394,661 | 0 | 2,000,000 |

两方向均零丢失（服务端计数等于客户端发送数），重传约 0.3%~0.5%。

## 4. 丢包/乱序下的可靠重传

两端均注入：`SX_LOSSY_DROP_PPM=20000`（2%）+ `SX_LOSSY_REORDER_PPM=10000`（1%），
方向 45 -> 46，200,000 条 x 1400B。

| 指标 | 客户端 | 服务端 |
| --- | --- | --- |
| 发送/接收 | sent=200000 | rx_msgs=200000 |
| 未确认在途 | outstanding=0 | — |
| 重传 | retrans=11997, fast=8421, timeouts=3576 | retrans=0 |
| 重复/乱序 | — | dup=7694, reorder=88461 |
| 吞吐 | 24.9 MB/s | — |

结论：注入 2% 丢包 + 1% 乱序后，**全部 200000 条按序可靠恢复，零丢失**。

更严苛的 5% 丢包（`SX_LOSSY_DROP_PPM=50000`，单向 200,000 条）：

```
client: sent=200000 outstanding=0 retrans=26848 fast=18281 timeouts=8567, 6.5 MB/s
server: rx_msgs=200000, dup=15365, reorder=104996
```

同样零丢失，但吞吐显著下降（RTO 退避主导）。

## 5. 长时间稳定性

持续 56.6s，20,000,000 条 x 1400B = 28 GB，方向 45 -> 46，无注入。

```
client: sent=20000000 bytes=28000000000 sec=56.57 MBps=494.92 msgps=353516
        outstanding=0 retrans=28147 fast=27298 timeouts=849 rtt_ms=1 rto=100
server: rx_msgs=20000000 rx_bytes=28000000000 retrans=0 dup=21143 reorder=36716 dropped=0
```

期间采样进程 RSS：

```
20:33:57  45[2284 KB]  46[2080 KB]
20:34:13  45[2328 KB]  46[2080 KB]
20:34:30  45[1984 KB]  46[2080 KB]
```

RSS 稳定在约 2 MB，无增长；无崩溃、无死锁；服务端精确收到 2000 万条。

## 6. 观察与结论

- **可靠性达标**：所有场景下服务端接收计数严格等于客户端发送计数；丢包/乱序/5% 丢包均完整恢复且按序。
- **时延优秀**：25G 直连 p50 14us、p99 36us。
- **吞吐**：单进程内核 UDP 约 0.5 GB/s（34~39 万 msg/s，1400B）；受单线程 syscall/拷贝限制，未打满 25G。
- **丢包代价**：KCP 的 RTO 退避在高丢包下吞吐下降明显（5% 丢包降至 6.5 MB/s）；后续可调优
  （更激进的快速重传、减小 RTO、增大窗口/批量、或启用 AF_XDP）。
- **无内存问题**：28 GB 流量、56s 长跑，RSS 平稳。

## 7. 复现

```sh
# 部署与构建（两机）
scp main.zip tomcat@<host>:/tmp/ && ssh tomcat@<host> \
  'rm -rf ~/simplexudp && mkdir ~/simplexudp && cd ~/simplexudp && unzip -q /tmp/main.zip && \
   make SX_NO_XDP=1 SX_LOSSY=1 -j8 all tests-latency tests-throughput'

# 时延：46 服务端，45 客户端
ssh tomcat@192.168.10.46 'cd ~/simplexudp && SX_LOG=/tmp/s.log ./build/test_latency udp-server 192.168.111.46 40000 300000'
ssh tomcat@192.168.10.45 'cd ~/simplexudp && SX_LOG=/tmp/c.log ./build/test_latency udp-client 192.168.111.45 40001 192.168.111.46 40000 2000 200'

# 吞吐（45 -> 46）
ssh tomcat@192.168.10.46 'cd ~/simplexudp && SX_LOG=/tmp/s.log ./build/test_throughput udp-server 192.168.111.46 41000 600000'
ssh tomcat@192.168.10.45 'cd ~/simplexudp && SX_LOG=/tmp/c.log ./build/test_throughput udp-client 192.168.111.45 41101 192.168.111.46 41000 2000000 1400 60000'

# 丢包注入（两端都加环境变量）
SX_LOSSY_DROP_PPM=20000 SX_LOSSY_REORDER_PPM=10000 ...
```

代码改动（提交 `c0ba660`）：新增 `SX_NO_XDP` / `SX_LOSSY` 构建选项与 `tests/test_throughput.c`。
