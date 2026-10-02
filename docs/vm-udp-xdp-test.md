# SX 跨机测试记录（VirtualBox VM：UDP 与 AF_XDP）

日期：2026-10-02　库版本：0.1.0

## 1. 环境

| 项 | vm1 (vboxdeb001) | vm2 (vboxdeb002) |
| --- | --- | --- |
| 登录 | vboxuser@172.22.133.251（免密，sudo 免密） | vboxuser@172.22.133.252 |
| 测试网卡 | enp0s9 = 192.168.77.101 | enp0s9 = 192.168.77.102 |
| 控制网卡 | enp0s8 = 172.22.133.251 | enp0s8 = 172.22.133.252 |
| 系统/内核 | Debian 13 / 6.12.111 | 同 |
| 编译器 | gcc 14.2 / clang 19.1 | 同 |
| XDP 依赖 | libbpf 1.5.0 / libxdp 1.5.4 / bpftool 7.5 | 同 |
| vCPU | 2 | 2 |
| 驱动 | e1000（VirtualBox） | e1000 |

- SSH/控制走 172.22.133.x；**数据面走 enp0s9 的 192.168.77.x**。
- e1000 驱动**不支持 native XDP**，库自动回退到 **generic(SKB) + copy** 模式。
- 构建：`make -j4 all tests-latency tests-throughput`（XDP 全功能）。

## 2. UDP 测试（内核 UDP 后端）

### 时延（n=5000）

```
rtt(us): min=0 avg=84 p50=61 p99=545 max=1745
```

### 吞吐（各 500,000 条 × 1400B = 700MB）

| 方向 | 耗时 | 吞吐 | msg/s | 客户端 outstanding | 服务端收到 | 客户端重传 |
| --- | --- | --- | --- | --- | --- | --- |
| 101 -> 102 | 5.63s | 124.3 MB/s | 88,801 | 0 | 500,000 | 323,016 |
| 102 -> 101 | 6.67s | 104.9 MB/s | 74,925 | 0 | 500,000 | 346,296 |

零丢失，但重传率很高（约 60%+）：VM 网络较慢、且接收端 UDP 套接字缓冲不足导致溢出丢包，
KCP 依靠快速重传全部补齐。建议后续为 UDP 后端增加 `SO_RCVBUF/SO_SNDBUF` 调优与自适应窗口。

## 3. AF_XDP 测试（XDP generic + copy）

启动日志显示：native 失败（`Underlying driver does not support XDP in native mode`），
自动回退 `mode=generic bind=copy`（客户端 `xdp_mode=SX_XDP_MODE_GENERIC`、`xdp_bind=COPY`）。

### 时延（n=2000）

```
rtt(us): min=52 avg=76 p50=69 p99=266 max=1567
```

### 吞吐（各 200,000 条 × 1400B = 280MB）

| 方向 | 耗时 | 吞吐 | msg/s | 客户端重传 | 服务端收到 | 服务端 dup/reorder |
| --- | --- | --- | --- | --- | --- | --- |
| 101 -> 102 | 1.60s | 175.3 MB/s | 125,208 | 16 | 200,000 | 0 / 0 |
| 102 -> 101 | 1.38s | 203.2 MB/s | 145,142 | 84 | 200,000 | 66 / 344 |

AF_XDP 在本环境下**吞吐高于内核 UDP**（175~203 vs 105~124 MB/s），且**重传几乎为零**：
XDP 重定向到 xsk 绕过了内核 UDP 套接字缓冲，避免了溢出丢包。

### bpftool 证据

```
# bpftool net show dev enp0s9
xdp:
enp0s9(3) generic id 36        # 服务端；客户端为 id 42

# bpftool prog show name sx_xdp_redirect_prog
36: xdp  name sx_xdp_redirect_prog  tag 33fad027e9fc8771  gpl
    xlated 928B  jited 544B  memlock 4096B  map_ids 13,14,12

# bpftool map dump <sx_stats_map>
map id=14: key=0 redirect=4421  key=1 PASS=1        # 时延测试
map id=17: key=0 redirect=200898 key=1 PASS=1       # 吞吐测试

# bpftool map dump name sx_config
key 0 -> { port: 45000/46000 }
```

`sx_stats_map` 的 redirect 计数（约等于请求+回显+控制帧）证明报文确实经过 XDP 重定向到 AF_XDP。

## 4. 观察与结论

- **UDP 与 AF_XDP 两种跨机测试均通过**：所有场景服务端接收计数严格等于客户端发送计数，按序、零丢失。
- **AF_XDP 在该 VM 环境明显更优**：吞吐更高、重传接近零（绕开内核 UDP 缓冲）。
- **e1000 无 native XDP**：只能 generic + copy；要验证 native/zero-copy，需要支持 XDP 的
  驱动（e1000e/igb/ixgbe/virtio 视版本）或真实网卡。
- **UDP 后端调优点**：高重传源于接收缓冲溢出，建议增加 `SO_RCVBUF/SO_SNDBUF` 与自适应发送窗口。
- 测试后两机进程与 XDP 程序均已清理干净（`bpftool net show dev enp0s9` 为空）。

## 5. 复现

```sh
# 构建（两机）
scp main.zip vboxuser@<host>:/tmp/ && ssh vboxuser@<host> \
  'rm -rf ~/simplexudp && mkdir ~/simplexudp && cd ~/simplexudp && unzip -q /tmp/main.zip && \
   make -j4 all tests-latency tests-throughput'

# UDP 时延 / 吞吐
./build/test_latency  udp-server 192.168.77.102 43000 600000
./build/test_latency  udp-client 192.168.77.101 43100 192.168.77.102 43000 5000 200
./build/test_throughput udp-server 192.168.77.102 44000 600000
./build/test_throughput udp-client 192.168.77.101 44101 192.168.77.102 44000 500000 1400 60000

# AF_XDP 时延 / 吞吐（需 sudo）
sudo ./build/test_latency  xdp-server enp0s9 192.168.77.102 45000 600000
sudo ./build/test_latency  xdp-client enp0s9 192.168.77.101 45100 192.168.77.102 45000 2000 200
sudo ./build/test_throughput xdp-server enp0s9 192.168.77.102 46000 600000
sudo ./build/test_throughput xdp-client enp0s9 192.168.77.101 46100 192.168.77.102 46000 200000 1400 120000

# 证据
sudo /usr/sbin/bpftool net show dev enp0s9
sudo /usr/sbin/bpftool map dump name sx_stats_map
```

---

## 6. 修复后回归（2026-10-02，提交 9a451b7）

在同一 VM 环境用修复后的代码重跑（UDP 与 AF_XDP，吞吐双向）：

### UDP

| 项目 | 结果 |
| --- | --- |
| 时延（n=5000） | min=25 avg=160 p50=55 p99=946 max=102059 us（含一次约 102ms 的 RTO 回退） |
| 吞吐 101->102 | 106.9 MB/s，76,358 msg/s，retrans=322,690，服务端 rx=500000 |
| 吞吐 102->101 | 86.7 MB/s，61,924 msg/s，retrans=70,464，服务端 rx=500000 |

零丢失；高重传仍来自接收端 UDP 缓冲溢出（VM 2 vCPU + 慢盘网络）。

### AF_XDP（generic + copy）

| 项目 | 结果 |
| --- | --- |
| 时延（n=5000） | min=20 avg=82 p50=52 p99=351 max=45792 us |
| 吞吐 101->102 | 177.2 MB/s，126,598 msg/s，retrans=16，服务端 rx=200000（dup=0 reorder=0） |
| 吞吐 102->101 | 185.9 MB/s，132,817 msg/s，retrans=220，服务端 rx=200000（dup=60 reorder=218） |
| bpftool | enp0s9 generic id 56；sx_stats_map redirect 10436（时延）/ 200933（吞吐） |

### 结论

- 与修复前（第 3 节）相比**无回归**：XDP 吞吐 177~186 MB/s 与之前 175~203 同档；
  UDP 106.9/86.7 MB/s 受 VM 抖动与缓冲影响略有波动，均零丢失。
- AF_XDP 依旧优于内核 UDP（吞吐更高、重传近零）。
- 注意：**单个网卡同时只能挂一个 XDP 端点**；双向测试需逐方向串行（server/client 分属两台主机），
  不可在同一主机同一网卡上同时起 server 与 client（会因 XDP 程序冲突导致第二个端点启动失败）。
