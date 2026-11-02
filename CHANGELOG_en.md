# Changelog

This file records functional changes, fixes, and test/documentation updates for simple-xudp (SX).
The format follows Keep a Changelog, and versions follow semantic versioning.

[README](README_en.md) | [Manual](MANUAL_en.md) | [中文](CHANGELOG.md)

## [Unreleased] - 2026-11-02

### Added

- RX hardware timestamping: off by default, enabled explicitly with `rx_ts_mode`; the UDP backend returns the raw PHC
  time via `SO_TIMESTAMPING`, and `on_recv_ex`/`sx_recv_ex` expose `sx_pkt_info_t`.
- AF_XDP timestamping: the kernel is probed at startup; on >=6.7 a device-bound XDP metadata program is loaded and the
  RX PHC time is read through `bpf_xdp_adjust_meta` + `bpf_xdp_metadata_rx_timestamp`; on kernels <6.7 or drivers without
  support the reason is reported, and `AUTO` falls back to software timestamps.
- Automatic unicast message fragmentation/reassembly: new `SX_CMD_PUSH_FRAG`, fragment header
  `msg_id/frag_idx/frag_cnt/total_len`, and per-link reassembly buffers; `max_message` defaults to 64KB with a 256KB hard
  limit (`f3f11a2`).
- Multicast/broadcast large-message fragmentation: per-source reassembly, `PUSH_FRAG` preserved across NACK
  retransmissions; `group_reorder`/`group_history` are raised automatically according to the maximum fragment count
  (`48a72d1`).
- Automatic send/receive window raising: computed as `ceil(max_message/(max_payload-12))`, hard minimum 256, maximum
  65536; when the fragment count exceeds `cwnd`, `cwnd` is temporarily raised to `snd_wnd` (`c26e91a`, `8deeafd`,
  `6fbb748`).
- Static ARP/MAC API: `sx_add_static_arp`, `sx_add_unicast_mac`; static entries cannot be overwritten by ARP replies and
  do not age out; cross-subnet `gateway_ip`/`netmask` support (`bc8cd2d`).
- Congestion-control switch `SX_CC_OFF`: the window is fixed at `send_window`, and timeouts only perform RTO backoff
  (`bc8cd2d`).
- UDP buffer configuration `udp_rcvbuf`/`udp_sndbuf`, preferring `SO_*BUFFORCE`, with the actual values recorded
  (`d7c3186`).
- RTO backoff switch `rto_backoff`: exponential backoff can be disabled, keeping retransmission only (`d7c3186`).
- Unified IO microbenchmarks:
  - pure XDP_TX reflector and loader (`ebcec3d`);
  - UDP/AF_XDP unified sender RTT comparison (`846946b`, `1b75970`);
  - pure AF_XDP mirror (`ee75afb`);
  - zero-copy mirror comparison (`bc94793`);
  - busy-poll mode (`0c7bab7`).
- Hardware timestamping and PTP:
  - `test_hwts` one-way TX/RX PHC timestamp measurement (`ee3b0ba`);
  - 10.45 PTP master, 10.46 PTP slave, `master_offset` about -8ns (`f933734`);
  - 10.45 NTP changed to the same source as 10.46, `192.168.12.1` (`f28c7c0`);
  - time-sync and PHC status recorded (`dfb0fd8`).

### Fixed

- Concurrency and lifecycle: the memory pool's `alloc/free` are now locked; `sx_stop` stops threads first and then
  removes io/links/queues under the lock; `sx_update`/`sx_wait` check the running state; link reference counting
  protects queued messages; `on_event` is delivered through an out-of-lock queue; the `sx_log` callback is moved outside
  the lock to avoid self-deadlock.
- Protocol correctness: unicast is routed before multicast, fixing unicast data being fed into the group engine when one
  endpoint mixes unicast and multicast; failed fragment reassembly no longer advances `rcv_nxt` or sends ACK; a
  mid-send fragment failure rolls back fragments already placed in the window; a peer restart is detected through a
  random `peer_id` and resynchronized; late-joining or timed-out multicast sources fast-forward according to the reorder
  window.
- Multicast: `WINS` is broadcast only when the high watermark changes or after a long period, reducing idle chatter;
  source timeouts reset the reassembly buffer; `test_gsend` supports post-send lingering to allow large-message NACK
  retransmission.
- Build: `Makefile` now sets `.DEFAULT_GOAL := all` explicitly, fixing `-include *.d` turning the default goal into a
  single `.o` and preventing incremental builds from updating the library; CMake embeds the hardware-timestamp BPF blob
  and enables `SX_XDP_TS_EMBEDDED`.
- XDP: TX allocates the frame before reserving and releases it on failure; ARP reply conditions deduplicated.
- Tests: cleaned up the `-Wshadow` warning in `test_loopback`; `test_frag` explicitly enables `large_message_burst`.
- Configuration: new large-message congestion policy `large_message_burst` (default 1, preserving 256KB compatibility;
  set 0 for strict slow start).
- Old `xxd` versions lack `-n`, producing an empty BPF blob; `scripts/gen_blob.sh` now generates portably and validates
  that the output is non-empty (`27f73f8`).
- Residual XDP programs caused `bpf_xdp_attach` to silently no-op; the program id is queried before attach and verified
  after attach, with an explicit error on failure (`b1479cd`).
- Large messages under slow start returned `SX_ERR_WINDOW` when `cwnd < fragment count`; the first large message now
  temporarily raises `cwnd` (`8deeafd`).
- Explicitly configured `send_window`/`recv_window` values above the hard limit were not clamped; a 65536 cap was added
  (`6fbb748`).

### Changed

- `max_message` defaults to 64KB with a 256KB hard limit; `send_window`/`recv_window` have an automatic minimum of 256
  (`c26e91a`).
- Unicast, multicast, and broadcast all support automatic 256KB fragmentation/reassembly; multicast/broadcast keep
  best-effort semantics.
- Documentation adds static ARP, CC, UDP buffers, RTO, PTP, hardware timestamping, AF_XDP benchmarks, and 45/46 test
  records (`e17b283`, `5c612f8`, `e0b6c74`, `740dce1`, `a8d48e6`, `b40d31e`).
- Deployment privilege documentation completed: AF_XDP adds `CAP_NET_RAW`, distinguishes `CAP_SYS_ADMIN` for kernels
  < 5.8, and adds `setcap`/systemd examples, privileged-port requirements, and the non-root `AUTO` fallback to UDP
  (`README.md`, `MANUAL.md` sections 6/13/14/15).

### Tests

- Added `test_frag` (200KB KCP fragmentation), `test_arp`, and `test_cc`; extended `test_loopback` (200KB
  fragmentation).
- 45/46 measurements:
  - unicast 256KB: send, ACK, reassembly, and content verification passed (`b40d31e`);
  - multicast/broadcast 200KB and 256KB: all passed (`48a72d1`);
  - AF_XDP native+copy cross-host p50 16~21us, p99 31~38us;
  - pure XDP_TX hardware-timestamp one-way 0.38~0.41us;
  - AF_XDP copy mirror p50 10~12us; zero-copy mirror p50 18us (no benefit observed).

## [0.1.0] - 2026-10-02

### Added

- AF_XDP and kernel UDP dual backends with interoperable wire format (`8fe44ab`).
- KCP-style reliable transport: sliding window, cumulative/selective ACK, fast retransmit, RTO backoff, RTT estimation,
  flow control/congestion.
- Unicast/multicast/broadcast semantics; multicast/broadcast high-watermark + NACK best-effort reliability (`8fe44ab`,
  `828688d`, `53b3543`).
- UDP-only build `SX_NO_XDP`, loss/reorder injection `SX_LOSSY`, and throughput tests (`c0ba660`).
- Latency/throughput tests and CPU-pinning scripts (`744b798`, `231aa7c`, `3c98adb`).
- Three-hop XDP relay ring test (`4d463e5`).
- Three-node multicast/broadcast and unicast mesh tests (`3307181`).
- CMake alternative build, install target, and complete build documentation (`9a451b7`, `7cc0736`).
- Application integration cost evaluation, FFI evaluation, and TODO (`1f4f484`, `1eb1694`, `7e30736`).

### Fixed

- Sequence-scan infinite loop caused by delayed ACKs (`3c98adb`).
- Code-review H/M/L issues: broadcast routing, resource leaks, XDP port validation, `sx_recv` truncation, thread
  contract, and others (`9a451b7`).
- Seven review improvements: out-of-lock callback delivery, MTU joint budget, memory pool, XDP observability,
  configurable constants, and others (`fb7ff10`).

### Changed

- Multicast supports an explicit interface `multicast_if`; the UDP backend supports broadcast (`53b3543`, `828688d`).
- Documentation adds VirtualBox, cross-subnet, multicast/broadcast, code-review, and three-node test records
  (`1cec728`, `e466fa7`, `63ad9a9`, `dadcdfd`, `404d3f9`, `8859177`).

### Tests

- Unit/loopback tests, AF_XDP veth end-to-end, two-process latency, throughput, and soak.
- 25G direct-connect UDP: zero loss at 2%/5% packet loss; 20 million messages with stable RSS.

## Before [0.1.0]

- `8fe44ab` initial implementation: XDP/AF_XDP reliable communication library, UDP backend, basic protocol, and tests.
