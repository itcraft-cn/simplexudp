# simple-xudp (SX)

[Changelog](CHANGELOG_en.md) | [Manual](MANUAL_en.md) | [中文](README.md)

A low-level Linux communication library built on **XDP + AF_XDP** (ANSI C), with a **kernel UDP socket** backend as a compatibility/testing path. It provides message-oriented, **ordered, reliable** communication with **loss detection and retransmission**, supporting **unicast, multicast, and broadcast**.

- Target platforms: Linux x86_64 / aarch64, C99.
- The per-packet payload limit is fixed (1400 by default, strictly below MTU, no IP fragmentation); larger messages are automatically split and reassembled, up to 256KB per message. Unicast, multicast, and broadcast are all supported.
- `send_window` is configurable and is raised automatically according to `max_message`, with a hard minimum of 256 and a hard maximum of 65536, so a 256KB message fits in one window; `large_message_burst` controls whether large messages temporarily raise `cwnd` (default 1; set 0 for strict slow start).
- Reliability is KCP-inspired: sliding window, cumulative + selective ACK, fast retransmit, RTO backoff, RTT estimation, flow control, and congestion control. Dedicated links can set `SX_CC_OFF` to disable window adjustment.
- A restarted peer is detected automatically through a random `peer_id` and the session is resynchronized; late-joining multicast receivers or timed-out sources fast-forward according to the reorder window.
- Multicast/broadcast use a **high-watermark + NACK** best-effort reliable scheme with in-order delivery.
- AF_XDP supports static ARP (which cannot be overwritten by replies) and cross-subnet gateway configuration.
- The UDP backend supports `udp_rcvbuf`/`udp_sndbuf`; dedicated links can disable RTO exponential backoff (`rto_backoff=0`).
- RX hardware timestamping is off by default; enable it explicitly with `rx_ts_mode`. `on_recv_ex`/`sx_recv_ex` expose PHC timestamps.

## Architecture

```
Application
 └─ sx_api (simplexudp.h)
     └─ sx_endpoint   endpoint/link/packet demux, update-driven
         └─ sx_kcp    unicast reliable engine (ARQ/window/reorder/flow/congestion)
         │  sx_group  multicast/broadcast engine (high-watermark/NACK/reorder)
         └─ sx_io     backend abstraction
             ├─ sx_io_udp   kernel UDP socket (no root required)
             └─ sx_io_xdp   AF_XDP + sx_frame(Ethernet/IP/UDP) + sx_arp + XDP BPF
```

The wire format is identical (the SX header sits inside the UDP payload), so AF_XDP and the kernel UDP backend interoperate.

## Data-plane frame format

```
Ethernet(14) | IPv4(20) | UDP(8) | SX header(32) | payload(<=1400)
total <= 1474 < 1500 (MTU)
```

SX header (network byte order, 32 bytes): `magic/ver/cmd | conv | peer_id | seq | una | ts | wnd | ack_count | payload_len`, optionally followed by `ack_list[]`.

Commands: `PUSH / ACK / WASK / WINS / HELLO / BYE / PING / PONG / NACK`.

## Build

### Dependencies

- C compiler: `gcc` or `clang` (C99).
- Build tool: `make` (or CMake ≥ 3.16).
- **AF_XDP backend** additionally requires: `libbpf`, `libxdp` development packages, `clang` (with `-target bpf` support, to compile the XDP program),
  `xxd` (to embed the BPF object as a byte array), and `pkg-config` (optional, for detecting libbpf/libxdp).
- Running AF_XDP requires root, or `CAP_BPF`, `CAP_NET_ADMIN`, `CAP_NET_RAW` (plus `CAP_SYS_ADMIN` on kernels < 5.8) and a raised memlock limit; the **kernel UDP backend requires no privileges**, except `CAP_NET_BIND_SERVICE` when binding ports below 1024. See [MANUAL_en.md](MANUAL_en.md) section 14 for the minimum capability set, `setcap`, and systemd examples.

```sh
# Debian / Ubuntu
sudo apt-get install -y build-essential clang llvm libbpf-dev libxdp-dev pkg-config xxd

# RHEL / CentOS / Fedora
sudo dnf install -y gcc clang llvm libbpf-devel libxdp-devel pkgconf-pkg-config vim-common
```

### Common targets

```sh
make                 # build build/libsimplexudp.a and .so (includes XDP BPF targets by default)
make bpf             # compile XDP BPF targets only
make examples        # examples: build/sx_p2p, build/sx_group
make tests           # unit/loopback tests: build/test_{frame,proto,kcp,loopback,recv,arp,cc,frag}
make tests-xdp       # AF_XDP end-to-end test: build/test_xdp
make tests-latency   # latency test: build/test_latency
make tests-throughput# throughput/soak test: build/test_throughput
make group-tools     # multicast/broadcast tools: build/test_gsend, build/test_grecv
make relay-tool      # three-hop relay: build/test_relay
make xdp-echo        # pure XDP_TX reflector and loader (microbenchmark)
make test-xdp-echo   # RTT sender for the pure XDP_TX reflector
make test-io-echo    # unified IO abstraction: UDP/AF_XDP sender RTT comparison
make test-hwts       # UDP hardware timestamp one-way measurement (requires PTP sync)
make unicast-big     # cross-host unicast large-message test tool (server/client)
make ts-status       # RX timestamp capability probe (UDP/AF_XDP)

make check           # run unit/loopback tests (no root, UDP backend)
make check-xdp       # requires root: create a veth pair and run the AF_XDP end-to-end test
make check-latency   # requires root: AF_XDP latency test + bpftool proof that traffic goes through XDP
make check-latency-2p# requires root: two-process AF_XDP latency test

make install         # install headers and libraries (default PREFIX=/usr/local)
make uninstall
make clean
```

### Build options

| Variable | Effect |
| --- | --- |
| `SX_NO_XDP=1` | Kernel UDP backend only: no libbpf/libxdp/clang dependency, for environments without XDP or for distribution |
| `SX_LOSSY=1` | Compile in loss/reorder injection (enabled at runtime by `SX_LOSSY_DROP_PPM`/`SX_LOSSY_DUP_PPM`/`SX_LOSSY_REORDER_PPM`), for reliability tests without root |
| `DEBUG=1` | `-O0 -g -DDEBUG` |
| `CC=clang` | Select the compiler |
| `PREFIX` / `DESTDIR` | Install prefix and packaging root for `install` |

```sh
# Environment without XDP (e.g. UDP backend only, containers, CI)
make SX_NO_XDP=1
make SX_NO_XDP=1 group-tools relay-tool

# Debug build with loss injection
make SX_LOSSY=1 DEBUG=1

# Install / package
sudo make install PREFIX=/usr/local
make install DESTDIR=/tmp/pkg PREFIX=/usr
```

### Architecture

The Makefile detects the architecture automatically (`uname -m`, x86_64 / aarch64): it uses `$(CC) -print-multiarch`
to locate multiarch headers and compiles the XDP program with `clang -target bpf`; no manual target selection is needed.

### CMake alternative

```sh
cmake -S . -B build-cmake && cmake --build build-cmake -j
cd build-cmake && ctest        # run unit/loopback tests
```

> Logging: the library **never writes to stdout/stderr**; all logs go through the `sx_config.on_log` callback. Tests/examples write to log files.

## Quick start

```c
#include "simplexudp.h"

sx_config_t cfg;
sx_config_init(&cfg);
cfg.ifname    = "eth0";          /* leave empty to use kernel UDP */
cfg.backend   = SX_BACKEND_AUTO; /* prefer AF_XDP, fall back to UDP */
cfg.local_port= 9000;
cfg.conv      = 0x1234;
cfg.on_recv   = my_recv;
cfg.user      = my_ctx;

sx_endpoint_t *ep = sx_create(&cfg);
sx_start(ep);

sx_link_t *peer = sx_add_unicast(ep, ipv4("10.0.0.2"), 9000);
sx_send(ep, peer, buf, len);            /* automatically splits above max_payload */

for (;;) sx_update(ep, 0);             /* or sx_run(ep, -1) */

sx_destroy(ep);
```

- Unicast: `sx_add_unicast` / `sx_send_to`.
- Multicast: `sx_add_multicast` (the UDP backend performs `IP_ADD_MEMBERSHIP`).
- Broadcast: `sx_add_broadcast` (destination is usually `255.255.255.255` or a subnet broadcast address).

See the **[Manual](MANUAL_en.md)**, the design document `docs/design.md`, and the examples `examples/sx_p2p.c` and `examples/sx_group.c`.
The Python/Java binding evaluation and plan are in `docs/ffi-evaluation.md` and `docs/ffi-todo.md` (the current focus is the C library; FFI is deferred).

## Tests

- `tests/test_frame.c`: frame construction/parsing and checksum defenses.
- `tests/test_proto.c`: protocol header encoding/decoding boundaries.
- `tests/test_kcp.c`: injects **loss + adjacent reordering**, verifies 100/100 in-order delivery and that retransmissions actually happen.
- `tests/test_loopback.c`: UDP backend end-to-end bidirectional in-order delivery, including 4KB and 200KB fragmentation/reassembly.
- `tests/test_arp.c`: static ARP entries are not overwritten by forged replies and do not age out.
- `tests/test_cc.c`: `SX_CC_OFF` does not reduce the window; `SX_CC_ON` reduces it on timeout.
- `tests/test_frag.c`: fragmented send/reassembly and the 256KB large-message window.
- `tests/test_xdp.c` + `scripts/veth_test.sh`: AF_XDP native/copy end-to-end over veth.
- `tests/test_latency.c` + `scripts/xdp_latency.sh`: stop-and-wait round-trip latency statistics (min/avg/p50/p99/max),
  and while the test is attached, `bpftool net show` / `bpftool prog show` / `bpftool map dump` prove that traffic
  really goes through XDP (redirect counts in `sx_stats_map`).

CentOS 7 / kernel 5.18 on real hardware is recorded in `docs/centos7-test-20261102.md`; the 10.45/10.46 25G cross-host
AF_XDP latency/throughput results and residual XDP issues are in `docs/cross-host-45-46-20261102.md`.

### Measured reference (local machine, stop-and-wait RTT, n=20000), single process (both endpoints in one process):

| Path | min | avg | p50 | p99 | max |
| --- | --- | --- | --- | --- | --- |
| AF_XDP (veth, native+copy) | 1us | 1~2us | 2us | 2us | 11~17us |
| Kernel UDP (lo) | 4us | 4us | 4us | 6us | 55us |

Two processes (server and client in separate processes, closer to reality):

| Path | min | avg | p50 | p99 | max |
| --- | --- | --- | --- | --- | --- |
| AF_XDP (veth, native+copy) | 1us | 2us | 2us | 4us | ~425us |
| Kernel UDP (lo) | 0us | 3us | 3us | 5us | ~173us |

All values are **round-trip latency** (including user-space processing at both ends); one-way latency is roughly half.
For the two-process run, `bpftool map dump` shows that each process's XDP program redirected about 40k packets
(20k requests + 20k replies + control frames), proving that traffic in both directions really goes through XDP.

### CPU pinning (strongly recommended)

Without pinning, max latency jitters significantly (UDP/lo about 300~400us, AF_XDP/veth up to several hundred us),
mainly due to process migration and IRQ/softirq contention; binding the two sides to different **performance cores**
reduces max latency to about 20~30us:

| Path | Pinning | p50 | p99 | max |
| --- | --- | --- | --- | --- |
| UDP/lo | none | 3us | 4us | ~408us |
| UDP/lo | s=4 c=5 | 2us | 3us | ~16us |
| AF_XDP/veth | none | 2us | 4us | 66~845us |
| AF_XDP/veth | s=1 c=2 | 2us | 4us | ~20us |
| AF_XDP/veth | s=2 c=3 | 2us | 5us | ~22us |

Recommendation: pin the server and client to different performance cores (cores c0–c5 on the local machine, 6.8GHz) and
avoid the interrupt core c0; iWLAN/WiFi queue interrupts are spread over c0–c8, so for very low jitter move IRQ affinity
away from the cores in use (`/proc/irq/*/smp_affinity_list`).

```sh
# Two processes + CPU pinning
SX_CPU_SERVER=2 SX_CPU_CLIENT=3 sudo scripts/xdp_latency_2p.sh build/test_latency
```

### Cross-host measurements (10.45 / 10.46, 25G direct link)

Topology: mlx5_core 25G, 10.45 `p3p2` 192.168.111.45 / 10.46 `ens3f1` 192.168.111.46;
the data interfaces use `combined 1` during tests and are restored to `combined 40` afterwards. AF_XDP is
native + copy, stop-and-wait RTT, n=20000:

| Path | min | avg | p50 | p99 | max |
| --- | --- | --- | --- | --- | --- |
| AF_XDP 10.45 -> 10.46 | 10us | 16us | 16us | 31us | 94us |
| AF_XDP 10.46 -> 10.45 | 7us | 23us | 21us | 38us | 91us |
| Kernel UDP 10.45 -> 10.46 | 15us | 20us | 19us | 37us | 166us |

Throughput (500000 × 1024B):

| Backend | Client MB/s | msg/s | Retransmits | Timeouts | Server dup | Server reorder |
| --- | --- | --- | --- | --- | --- | --- |
| Kernel UDP | 312.60 | 305276 | 3333 | 0 | 3167 | 1545 |
| AF_XDP native+copy | 251.20 | 245308 | 16 | 13 | 0 | 5 |

UDP has higher peak throughput but noticeable cross-host loss/retransmission; AF_XDP is slightly slower but has
near-zero retransmission and reordering, giving better stability.

zero-copy and CPU pinning (`combined 2` + Flow Director steering to queue 2 + static ARP):

| Mode | Run | p50 | p99 | max |
| --- | --- | --- | --- | --- |
| copy, unpinned | 1 | 16us | 31us | 94us |
| copy, pinned | 1~2 | 15us | 30us | 43~106us |
| zero-copy, unpinned | 1 | 19us | 31us | 1065us |
| zero-copy, pinned | 1~2 | 16us | 23~24us | 71~358us |

A pure XDP_TX benchmark with KCP/user-space reflection removed gives p50 10~11us and p99 13~15us; switching the sender
to AF_XDP gives p50 8~10us. With `SX_BUSY_POLL=1` skipping the per-packet `poll()`, the KCP path improves from p50
15us to 11us and p99 from 31us to 26us.

Hardware-timestamp one-way measurements (after PTP sync, n=10000):

| Direction | min | avg | p50 | p99 | max |
| --- | --- | --- | --- | --- | --- |
| 10.45 -> 10.46 | 0.363us | 0.384us | 0.384us | 0.405us | 2.524us |
| 10.46 -> 10.45 | 0.385us | 0.405us | 0.405us | 0.425us | 0.436us |

The NIC-to-NIC physical one-way latency is about 0.4us; the 4~5us obtained from RTT/2 is dominated by software-path
overhead.

Large-message verification: unicast 256KB completes automatic fragmentation, reliable transmission, reassembly, and
content verification on 45/46; multicast/broadcast 256KB both give got=1/1 and bad=0 (multicast/broadcast rely on NACK
recovery, so the sender must linger to answer retransmissions).

See `docs/cross-host-45-46-20261102.md` for the full topology, field issues (residual XDP, zero-copy queue offset),
and per-item data.

## AF_XDP notes and limitations

- Attach mode: native first, fall back to generic; bind: zero-copy first, fall back to copy (auto-detected).
- AF_XDP binds a single RX queue; target traffic must be steered to that queue (RSS/ethtool or a single-queue NIC).
- **Only one SX XDP endpoint per NIC at a time**: existing XDP programs are not replaced; a second endpoint fails to attach,
  and `SX_BACKEND_AUTO` falls back to UDP with `stats.backend_fallback=1`. Use multiple NICs/netns for multiple endpoints.
- ARP is handled in user space by this library (the XDP program redirects ARP too). In production, use `sx_add_static_arp`
  to pin peer/gateway MACs; static entries cannot be overwritten by ARP replies and do not age out. For cross-subnet
  traffic, set `gateway_ip`/`netmask` and register a static mapping for the gateway.
- Generic mode forces copy; zero-copy requires NIC driver support.
- For multicast on the XDP backend, the interface must accept the group (possibly `ip maddr add` or allmulti).
- Without root and the required capabilities: `SX_BACKEND_AUTO` falls back to kernel UDP and sets `stats.backend_fallback=1`;
  forcing `SX_BACKEND_XDP` does not fall back, and `sx_start` returns `SX_ERR_BACKEND`.

## Reliability semantics

- **Unicast**: strictly reliable and ordered. Cumulative ACK `una` + selective ACK `ack_list` + fast retransmit + RTO retransmit + congestion/flow control.
- **Multicast/broadcast**: best-effort reliable + ordered. The sender keeps a history window and periodically broadcasts a high watermark; the receiver detects gaps and sends `NACK`, and the sender retransmits from history to the group address. Delivery is not guaranteed (no multi-party ACK aggregation).
- **Session resynchronization**: after a peer restarts, `HELLO` carries a new random `peer_id`; the local side automatically resets its windows and re-handshakes. Resynchronization discards sending data that was unacknowledged before the restart.
- **Reassembly failure**: if fragment reassembly fails, the receive sequence number is not advanced and no ACK is sent; recovery is left to retransmission.
