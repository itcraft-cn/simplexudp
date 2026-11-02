# simple-xudp (SX) Manual

Version: 0.1.0  Platforms: Linux x86_64 / aarch64  Language: ANSI C (C99)

This manual is the complete guide to `include/simplexudp.h`, covering the conceptual model, quick start, configuration,
API reference, protocol and reliability semantics, AF_XDP notes, tuning, and FAQ. See `docs/design.md` for design
rationale.

[README](README_en.md) | [Changelog](CHANGELOG_en.md) | [中文](MANUAL.md)

---

## Contents

1. Overview
2. Conceptual model
3. Build and install
4. Quick start
5. Event loop and threading model
6. Configuration (sx_config_t)
7. API reference
8. Error codes
9. Statistics and observability
10. Wire format and reliability semantics
11. AF_XDP backend notes and limitations
12. RX hardware timestamping
13. Tuning guide
14. Deployment and privileges
15. FAQ
16. Examples and tests

---

## 1. Overview

SX is a low-level communication library with a selectable data plane:

- **AF_XDP**: an XDP program redirects target UDP/ARP packets to a user-space xsk; zero-copy/copy mode is detected
  automatically;
- **Kernel UDP socket**: requires no root, convenient for development, unit tests, and CI; the wire format matches
  AF_XDP and interoperates with it.

It provides a message-oriented API (one packet, one message) with:

- a fixed per-packet payload limit (1400 by default, strictly below MTU) and no IP fragmentation; larger messages are
  split and reassembled automatically by the library;
- ordered delivery, loss detection, and retransmission (KCP-inspired);
- point-to-point (unicast, strictly reliable and ordered) and multicast/broadcast (best-effort reliable + ordered).

Only Linux x86_64 / aarch64 is supported. The library never writes to stdout/stderr; logs go through a callback.

---

## 2. Conceptual model

```
sx_endpoint_t  endpoint: one local ip:port + data-plane backend + several links
   └── sx_link_t  link: one logical channel
         ├── UNICAST   to a peer (ip,port), strictly reliable and ordered
         ├── MULTICAST to a group address, best-effort reliable + ordered
         └── BROADCAST to a broadcast address, best-effort reliable + ordered
```

- **conv**: session/group identifier. Both ends of the same session must use the same `conv`; the library demultiplexes
  packets by `conv` and source address.
- **Message**: one `sx_send` call corresponds to one message; if the length is <= `sx_max_payload()` it is sent as one
  packet, otherwise it is split into fragments (up to `sx_max_message()`).
- **Peer learning**: a unicast server does not need to register clients in advance; a packet with a matching `conv`
  automatically creates a link and triggers `SX_EVENT_PEER_ADDED`.
- **Callback vs pull**: set `on_recv` for callback mode; otherwise use `sx_recv()` in pull mode.

---

## 3. Build and install

```sh
make            # builds build/libsimplexudp.a and build/libsimplexudp.so
make examples   # examples
make tests      # tests
make check      # run unit/loopback tests (no root required)
```

Dependencies: `libbpf`, `libxdp`, `clang` (to compile BPF targets), `xxd` (to generate the embedded byte array).
Alternative build: `cmake -S . -B build-cmake && cmake --build build-cmake -j`.

Link in an application:

```sh
cc app.c -I<prefix>/include -L<prefix>/lib -lsimplexudp -lbpf -lxdp -lpthread
```

---

## 4. Quick start

Sender (client):

```c
sx_config_t cfg;
sx_config_init(&cfg);
cfg.ifname     = "eth0";          /* AF_XDP; empty means kernel UDP */
cfg.backend    = SX_BACKEND_AUTO; /* prefer AF_XDP, fall back to UDP */
cfg.local_port = 9000;
cfg.local_ip   = 0;               /* 0 = automatic */
cfg.conv       = 0x1234;
cfg.on_recv    = on_msg;          /* or NULL and use sx_recv */
cfg.user       = my_ctx;

sx_endpoint_t *ep = sx_create(&cfg);
sx_start(ep);

sx_link_t *peer = sx_add_unicast(ep, ipv4("10.0.0.2"), 9000);
sx_send(ep, peer, buf, len);      /* single packet if <= sx_max_payload; larger is fragmented */

while (running) {
    sx_wait(ep, 100);             /* block until readable (sx_run is also available) */
    sx_update(ep, 0);             /* RX/TX + timers */
}
sx_destroy(ep);
```

Receiver (server): only `sx_create` + `sx_start` + an event loop are needed; the first packet from a client creates a
link automatically, and the echo uses the `link` argument passed to the callback:

```c
static void on_msg(sx_endpoint_t *ep, sx_link_t *link,
                   const void *data, size_t len, void *user) {
    (void)user;
    sx_send(ep, link, data, len);   /* echo as-is */
}
```

---

## 5. Event loop and threading model

Two driving modes:

- **Single thread (default)**: the application calls `sx_update(ep, 0)` periodically; pair it with
  `sx_wait(ep, timeout)` to block until readable and avoid busy-waiting. Timers (retransmission/heartbeat) advance in
  `sx_update`, so the wait timeout should not be too large (20~200ms recommended).
- **Internal thread**: with `cfg.rx_thread = 1` the library starts RX/TX and timer threads, and callbacks fire on
  library threads. Internal locks are recursive, so callbacks may call `sx_send` directly.

Thread safety: `sx_send`/`sx_update`/`sx_recv`/link add-remove/`sx_get_stats` are all protected by the endpoint lock
(recursive), so multiple application threads may call them concurrently; callbacks may safely call `sx_send`.
`on_recv`/`on_event` are delivered outside the endpoint lock, so callbacks may safely call
`sx_link_remove`/`sx_stop`. `sx_stop` may be called from another thread: it sets the stop flag and reaps the internal
thread first, then removes io/links/queues under the lock. Note that a `sx_link_t*` handle becomes invalid after
`sx_link_remove`; do not remove the same handle concurrently with another thread using it. Queued messages hold a link
reference, so they do not become dangling pointers when the link is removed.

---

## 6. Configuration (sx_config_t)

Call `sx_config_init(&cfg)` for defaults, then override as needed.

### Data plane

| Field | Default | Description |
| --- | --- | --- |
| `ifname` | NULL | Interface name; required for AF_XDP, may be NULL for the UDP backend |
| `backend` | `SX_BACKEND_AUTO` | `AUTO` = prefer XDP, fall back to UDP on failure (including insufficient privileges); `XDP`/`UDP` force |
| `xdp_mode` | `SX_XDP_MODE_AUTO` | `AUTO` = native, fall back to generic; `NATIVE`/`GENERIC` |
| `xdp_bind` | `SX_XDP_BIND_AUTO` | `AUTO` = zero-copy, fall back to copy; `COPY`/`ZEROCOPY` |
| `xdp_queue` | 0 | RX queue to bind |

`AUTO` only tries AF_XDP when `ifname` is set; without `ifname` it uses kernel UDP directly. Without root and the
required capabilities, XDP initialization fails on privileges and falls back to UDP; confirm the downgrade with
`stats.backend_fallback=1`. Forcing `SX_BACKEND_XDP` does not fall back; `sx_start` returns `SX_ERR_BACKEND`. See
section 14 for privilege configuration.

### Local identity

| Field | Default | Description |
| --- | --- | --- |
| `local_ip` | 0 | Local IPv4 (host byte order); 0 = unspecified/derived by the backend |
| `gateway_ip` | 0 | Cross-subnet next hop IPv4 (host byte order); 0 = no gateway forwarding |
| `netmask` | 0 | Interface subnet mask (host byte order); 0 = auto-detected by the XDP backend |
| `udp_rcvbuf` | 0 | UDP receive buffer bytes; 0 = system default; 1~4MB recommended to avoid burst loss |
| `udp_sndbuf` | 0 | UDP send buffer bytes; 0 = system default |
| `local_port` | 0 | Local port (host byte order); 0 = kernel-assigned (UDP) |
| `conv` | `SX_DEFAULT_CONV` | Session/group identifier |

### Protocol parameters

| Field | Default | Description |
| --- | --- | --- |
| `rx_ts_mode` | `SX_RX_TS_OFF` | RX timestamp mode; off by default, must be enabled explicitly |
| `max_payload` | 1400 | Per-packet payload limit; 0→1400; values above 1426 are clamped |
| `max_message` | 65536 | Per-message limit, max 256KB; messages above `max_payload` are fragmented; set to `max_payload` to disable |
| `send_window` | 256 | Send window (messages); raised automatically to fit `max_message`, hard minimum 256, maximum 65536 |
| `recv_window` | 1024 | Receive window/reorder capacity; also raised automatically by `max_message`, hard minimum 256 |
| `group_history` | 1024 | Multicast/broadcast send history entries (for NACK recovery) |
| `group_reorder` | 256 | Multicast/broadcast receive reorder window (rounded up to a power of two) |
| `group_max_src` | 32 | Multicast/broadcast source table size |
| `max_peers` | 1024 | Maximum links per endpoint (including learned peers) |
| `rx_batch` | 4096 | Maximum packets received per `sx_update` |
| `pool_blocks` | 0 | Hot-path memory-pool blocks (0 = derived from send/receive windows) |
| `multicast_if` | 0 | Multicast egress/join interface IPv4 (required with multiple NICs); 0 = default interface |
| `min_rto_ms` | 100 | Minimum retransmission timeout |
| `max_rto_ms` | 5000 | Maximum retransmission timeout |
| `cc_mode` | `SX_CC_ON` | `SX_CC_OFF` disables congestion-window adjustment, keeping only RTO backoff/retransmission |
| `large_message_burst` | 1 | Whether to temporarily raise the window when the fragment count exceeds cwnd; set 0 for strict slow start |
| `rto_backoff` | 1 | 1 = exponential RTO backoff on timeout; 0 = fixed RTO (dedicated links, use with care) |
| `keepalive_ms` | 10000 | Idle heartbeat interval |
| `timeout_ms` | 30000 | Threshold for marking a peer DOWN after no response |

### Runtime/resources/callbacks

| Field | Default | Description |
| --- | --- | --- |
| `rx_thread` | 0 | 1 = the library starts internal threads |
| `umem_frames` | 4096 | AF_XDP UMEM frame count |
| `frame_size` | 4096 | AF_XDP frame size |
| `on_recv` | NULL | Receive callback; NULL means pull with `sx_recv` |
| `on_recv_ex` | NULL | Receive callback with timestamps; takes precedence over `on_recv` when set |
| `on_event` | NULL | Event callback |
| `on_log` | NULL | Log callback |
| `user` | NULL | Opaque pointer passed to callbacks |
| `log_level` | `SX_LOG_WARN` | Log level |

---

## 7. API reference

Convention: success returns 0 (or a non-negative handle/length/sequence); failure returns a negative error code,
convertible with `sx_strerror()`.

### 7.1 Lifecycle

#### `void sx_config_init(sx_config_t *cfg)`
Fills the configuration with defaults.

#### `sx_endpoint_t *sx_create(const sx_config_t *cfg)`
Creates an endpoint. Returns NULL on failure. `max_payload` is clamped to `SX_MAX_PAYLOAD_LIMIT`.

#### `int sx_start(sx_endpoint_t *ep)`
Initializes the backend, loads/attaches XDP (if applicable), and starts internal threads (if configured).
Returns `SX_OK` or a negative error code (e.g. `SX_ERR_BACKEND`). With the default AUTO backend, XDP failure falls back
to UDP.

#### `int sx_stop(sx_endpoint_t *ep)`
Stops threads, detaches XDP, closes the backend, and frees links. May be called repeatedly.

#### `void sx_destroy(sx_endpoint_t *ep)`
Destroys the endpoint (calls `sx_stop` internally). Passing NULL is safe.

### 7.2 Driving

#### `int sx_update(sx_endpoint_t *ep, uint64_t now_ms)`
Drives one iteration: receives and processes all pending packets and advances per-link timers
(retransmission/window probe/heartbeat/timeout). When `now_ms=0` the library reads the clock internally. Returns the
number of packets processed or a negative error code. In non-`rx_thread` mode this must be called periodically
(1~10ms recommended, or together with `sx_wait`).

#### `int sx_wait(sx_endpoint_t *ep, int timeout_ms)`
Blocks until a readable event (internally polls the backend fd). Returns >0 readable, 0 timeout, <0 error;
`timeout_ms<0` waits forever. Recommended loop: `sx_wait` + `sx_update` to avoid busy-waiting.

#### `int sx_run(sx_endpoint_t *ep, int timeout_ms)`
Runs an internal blocking loop (`sx_update` + `sx_wait`) until `sx_stop` or timeout; `timeout_ms<0` runs forever.

### 7.3 Link management

#### `sx_link_t *sx_add_unicast(sx_endpoint_t *ep, uint32_t ip, uint16_t port)`
Adds a unicast link (strictly reliable and ordered). Returns the same link if it already exists. NULL on failure.

#### `sx_link_t *sx_add_unicast_mac(sx_endpoint_t *ep, uint32_t ip, uint16_t port, const uint8_t mac[6])`
Adds a unicast link and statically binds the destination MAC (only effective on the AF_XDP send path). Equivalent to
`sx_add_unicast` + `sx_add_static_arp`; a NULL `mac` degrades to ordinary unicast.

#### `int sx_add_static_arp(sx_endpoint_t *ep, uint32_t ip, const uint8_t mac[6])`
Registers a static ARP mapping. May be called before `sx_start` (applied at startup) or at runtime.
Static entries cannot be overwritten by ARP replies and do not age out; the UDP backend ignores this mapping. For
cross-subnet traffic, set `gateway_ip` to the gateway address and register a static mapping for that address.

```c
uint8_t peer_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0xaa};
sx_add_unicast_mac(ep, ipv4("10.0.0.2"), 9000, peer_mac);

cfg.gateway_ip = ipv4("10.0.0.1");
cfg.netmask    = ipv4("255.255.255.0");
sx_add_static_arp(ep, cfg.gateway_ip, gw_mac);
```

#### `sx_link_t *sx_add_multicast(sx_endpoint_t *ep, uint32_t group_ip, uint16_t port)`
Adds a multicast link. The UDP backend performs `IP_ADD_MEMBERSHIP`; the XDP backend returns `SX_ERR_UNSUPPORTED`
(the NIC/interface is expected to accept the multicast traffic).

#### `sx_link_t *sx_add_broadcast(sx_endpoint_t *ep, uint32_t bcast_ip, uint16_t port)`
Adds a broadcast link (destination is usually `255.255.255.255` or a subnet broadcast address).

#### `int sx_link_remove(sx_endpoint_t *ep, sx_link_t *link)`
Removes a link and releases its state.

### 7.4 Send and receive

#### `int64_t sx_send(sx_endpoint_t *ep, sx_link_t *link, const void *data, size_t len)`
Sends one message. Returns the message sequence number (`>=0`) or a negative error code:

- `SX_ERR_TOO_LARGE`: `len > sx_max_message(ep)`;
- `SX_ERR_WINDOW`: the send window is full; retry later;
- `SX_ERR_CLOSED`: the endpoint is not started or has been stopped.

#### `int64_t sx_send_to(sx_endpoint_t *ep, uint32_t ip, uint16_t port, const void *data, size_t len)`
Convenience send: creates/reuses the corresponding unicast link and sends.

#### `ssize_t sx_recv(sx_endpoint_t *ep, void *buf, size_t cap, sx_addr_t *src_out, sx_link_t **link_out)`
**Note: always provide a buffer with `cap >= sx_max_payload(ep)`.** Semantics are "either receive completely or drop".
Receives one in-order message in pull mode. Returns the **full message length**: if the return value is greater than
`cap`, the message was truncated and the caller must handle it (enlarge the buffer or drop); returns `SX_ERR_AGAIN`
when there is no data; `src_out`/`link_out` may be NULL. Only use this in pull mode (`on_recv=NULL`).

#### `ssize_t sx_recv_ex(..., sx_pkt_info_t *info_out)`
Same as `sx_recv`, additionally returning the RX timestamp; `info_out` may be NULL.

```c
typedef struct sx_pkt_info {
    uint64_t rx_ns;      /* ns; 0 or valid=0 means no valid timestamp */
    uint32_t ts_mode;    /* SX_RX_TS_OFF/AUTO/HARDWARE */
    uint32_t ts_status;  /* SX_RX_TS_* status/reason */
    uint32_t clock_id;   /* SX_RX_TS_CLOCK_REALTIME / SX_RX_TS_CLOCK_PHC */
    uint32_t valid;
} sx_pkt_info_t;
```

### 7.5 Properties and statistics

#### `const char *sx_version(void)` / `const char *sx_strerror(int err)`
Version string / error-code description.

#### `int sx_get_stats(sx_endpoint_t *ep, sx_stats_t *out)`
Copies runtime statistics; also fills the window/RTT snapshot of the first unicast link.

#### `int sx_link_addr(const sx_link_t *link, sx_addr_t *out)`
Gets the link destination address.

#### `int sx_link_is_up(const sx_link_t *link)`
Whether a unicast link is usable (handshake/recent receive); multicast/broadcast always return 1.

#### `uint32_t sx_link_wnd(const sx_link_t *link)`
The link's available send window (messages = `min(peer-advertised window, congestion window)`).

#### `size_t sx_max_payload(const sx_endpoint_t *ep)`
The configured per-packet payload limit (fragment size).

#### `size_t sx_max_message(const sx_endpoint_t *ep)`
The per-message limit after automatic fragmentation/reassembly.

### 7.6 Callbacks

```c
void (*sx_recv_fn)(sx_endpoint_t *ep, sx_link_t *link,
                   const void *data, size_t len, void *user);
void (*sx_recv_ex_fn)(sx_endpoint_t *ep, sx_link_t *link,
                      const void *data, size_t len,
                      const sx_pkt_info_t *info, void *user);
void (*sx_event_fn)(sx_endpoint_t *ep, sx_event_t event, sx_link_t *link, void *user);
void (*sx_log_fn)(sx_log_level_t level, const char *msg, void *user);
```

- `data` is valid only during the callback; copy it if you need to keep it;
- callbacks may call `sx_send` (protected by the recursive lock);
- log callbacks must not block; the library never writes to stdout/stderr directly.

---

## 8. Error codes

| Value | Name | Meaning |
| --- | --- | --- |
| 0 | `SX_OK` | Success |
| -1 | `SX_ERR_INVALID` | Invalid argument |
| -2 | `SX_ERR_NOMEM` | Out of memory |
| -3 | `SX_ERR_TOO_LARGE` | Message exceeds `max_message` |
| -4 | `SX_ERR_NOT_FOUND` | Resource not found |
| -5 | `SX_ERR_EXISTS` | Already exists |
| -6 | `SX_ERR_AGAIN` | Temporarily unavailable/no data |
| -7 | `SX_ERR_IO` | Low-level IO error |
| -8 | `SX_ERR_BACKEND` | Backend initialization failed |
| -9 | `SX_ERR_STATE` | State does not allow the operation |
| -10 | `SX_ERR_TIMEOUT` | Timeout |
| -11 | `SX_ERR_UNSUPPORTED` | Unsupported |
| -12 | `SX_ERR_WINDOW` | Send window full |
| -13 | `SX_ERR_CLOSED` | Closed |

---

## 9. Statistics and observability

Key `sx_stats_t` fields:

- Traffic: `tx_packets/tx_bytes/rx_packets/rx_bytes`
- Reliability: `retransmits`, `fast_retrans`, `timeouts`, `dup_packets`, `reorder_pkts`, `lost_detected`,
  `nack_sent/nack_recv`, `dropped`, `xdp_rx_backlog` (XDP: UMEM frame exhaustion/RX backlog count)
- Window/latency: `snd_wnd`, `snd_outstanding`, `per_peer_wnd`, `cwnd`, `rtt_ms`, `rto_ms`
- RX timestamps: `rx_ts_mode`, `rx_ts_status`, `rx_ts_clock_id`
- Congestion control: `cc_mode` (the mode actually in effect)
- Backend: `backend`, `backend_fallback` (1 = AUTO tried XDP and fell back to UDP), `xdp_mode`, `xdp_bind`

For AF_XDP, `bpftool` can also be used: `bpftool net show dev <if>`, `bpftool prog show`,
`bpftool map dump name sx_stats_map` (key0 = redirected to xsk, key1 = PASS).

---

## 10. Wire format and reliability semantics

Frame format (AF_XDP backend):

```
Ethernet(14) | IPv4(20) | UDP(8) | SX header(32) | payload(<=1400)
total <= 1474 < 1500 (MTU)
```

SX header (network byte order, 32 bytes): `magic/ver/cmd | conv | peer_id | seq | una | ts | wnd | ack_count | payload_len`,
optionally followed by `ack_list[]`. Commands: `PUSH/ACK/WASK/WINS/HELLO/BYE/PING/PONG/NACK`.

- **Unicast**: strictly reliable and ordered. Cumulative ACK (`una`, i.e. the peer's `rcv_nxt`) + selective ACK
  (`ack_list`) + fast retransmit + RTO backoff + RTT estimation + flow control + congestion control
  (slow start/congestion avoidance/timeout window reduction). With `cc_mode=SX_CC_OFF` the window is fixed at
  `send_window`, while RTO backoff and retransmission remain unchanged.
- **Multicast/broadcast**: best-effort reliable + ordered. The sender keeps `group_history` entries and periodically
  broadcasts a high watermark; the receiver detects gaps per source and sends `NACK`, and the sender retransmits from
  history to the group address. Delivery is not guaranteed.
- **Session resynchronization**: each endpoint instance generates a random `peer_id`. After a linked peer restarts,
  its `HELLO` carries a `peer_id` different from the old value, so the local side automatically resets the send/receive
  windows and re-handshakes; for late-joining multicast receivers or timed-out sources, if the gap exceeds the entire
  reorder window the receiver fast-forwards to the current sequence number. Resynchronization discards sending data
  that was unacknowledged before the restart.
- **Reassembly failure**: if fragment reassembly fails (out of memory/malformed), `rcv_nxt` is not advanced and no ACK
  is sent, avoiding silent message loss.

---

## 11. AF_XDP backend notes and limitations

- Attach: native first, fall back to generic; bind: zero-copy first, fall back to copy (auto-detected, results in
  `stats.xdp_mode/xdp_bind`).
- Binds a single RX queue; target traffic must be steered to that queue (RSS/`ethtool -N` or a single-queue NIC).
- **Only one SX XDP endpoint per NIC at a time**: attach uses `XDP_FLAGS_UPDATE_IF_NOEXIST` and does not replace an
  existing XDP program. A second endpoint fails to attach; `AUTO` mode falls back to UDP and sets
  `stats.backend_fallback=1`. Use multiple NICs, multiple netns, or wait for a shared XDP program design for multiple
  endpoints.
- **ARP is handled in user space by this library** (the XDP program redirects ARP too), so only same-L2-subnet peers
  can be resolved directly. In production, use `sx_add_static_arp` to pin gateway/peer MACs (avoiding ARP queries, and
  static entries cannot be overwritten by forged replies); for cross-subnet traffic, set `gateway_ip`/`netmask` and
  register a static mapping for the gateway.
- Generic mode forces copy; zero-copy requires NIC driver support.
- For multicast on the XDP backend, the interface must accept the group (possibly `ip maddr add` or allmulti).
- Ensure a normal `sx_stop`/`sx_destroy` before exit to detach XDP; a SIGKILL leaves the XDP program behind
  (clean up with `sudo xdp-loader unload <if> --all`).

---

## 12. RX hardware timestamping

RX timestamping is off by default and must be enabled explicitly:

```c
cfg.rx_ts_mode = SX_RX_TS_HARDWARE; /* hardware only; if unavailable, disable and report the reason */
/* or cfg.rx_ts_mode = SX_RX_TS_AUTO; prefer hardware, fall back to software */
```

### 12.1 Backend differences

| Backend | Behavior |
| --- | --- |
| Kernel UDP | `SO_TIMESTAMPING` + `SOF_TIMESTAMPING_RX_HARDWARE | RAW_HARDWARE`, returns raw PHC nanoseconds |
| AF_XDP | Probes the kernel and driver at startup; Linux < 6.7 does not support XDP RX metadata timestamps; on >=6.7 with driver support it uses XDP metadata, and if the driver does not support it `HARDWARE` is disabled with a reason while `AUTO` falls back to software timestamps |

Startup logs and `sx_stats_t` report:

```text
rx_ts_mode    0=OFF, 1=AUTO(software fallback), 2=HARDWARE
rx_ts_status  0=disabled, 1=hardware OK, 2=software fallback,
              -1=unsupported, -2=kernel too old, -3=permission, -4=IO
rx_ts_clock_id 0=CLOCK_REALTIME, 1=PHC
```

Currently AF_XDP hardware timestamping on 10.45 (5.18) and 10.46 (4.18) is reported as `-2` (kernel < 6.7); on the
local 6.12 kernel the XDP metadata program compiles, but the `lo`/e1000e drivers do not support metadata timestamps,
so it falls back and reports `-1` (driver/interface unsupported). NICs with this capability enable `HARDWARE`.

On NICs that support hardware timestamping, the UDP backend obtains PHC time directly.

### 12.2 Usage

```c
static void on_msg(sx_endpoint_t *ep, sx_link_t *link,
                   const void *data, size_t len,
                   const sx_pkt_info_t *info, void *user) {
    if (info->valid && info->clock_id == SX_RX_TS_CLOCK_PHC) {
        /* info->rx_ns is the NIC's raw PHC time */
    }
}

cfg.rx_ts_mode = SX_RX_TS_HARDWARE;
cfg.on_recv_ex = on_msg;
```

In pull mode use `sx_recv_ex()`.

For automatically fragmented large messages, the reassembled message carries the RX timestamp of the **first fragment**;
the arrival times of subsequent fragments are not exposed individually.

Note: PHC time and `CLOCK_REALTIME` are two different clock domains and cannot be mixed directly; PTP synchronization
is required before cross-host comparison (this project has ptp4l deployed on 45/46, with `master_offset` about -8ns).
Plain NTP's sub-millisecond offset is not sufficient for microsecond-level timestamp comparison.

## 13. Tuning guide

- **MTU/payload**: a larger `max_payload` gives higher throughput but must stay < MTU; no IP fragmentation is used.
  Very large messages are controlled by `max_message` and fragmented at the application layer.
- **Window**: `send_window`/`recv_window` are raised automatically by `ceil(max_message/(max_payload-12))`, with a hard
  minimum of 256 and maximum of 65536. Thus a 256KB message fits in one window with the default 1400B fragments
  (189 fragments). If the fragment count exceeds the current `cwnd`, the first large message temporarily raises `cwnd`
  to `snd_wnd` to avoid `SX_ERR_WINDOW` from slow start; subsequent growth/backoff still follows the CC logic.
- **Window**: `send_window`/`recv_window` affect throughput and memory; larger values suit high-BDP links.
- **RTO**: on LANs, reduce `min_rto_ms` (e.g. 20~50); keep 100+ across WANs.
- **RTO backoff**: exponential backoff is the default; on dedicated links known to have no congestion and only random
  loss, `rto_backoff=0` avoids throughput collapse under high loss. This mode amplifies duplicate packets and must be
  combined with monitoring and confirmation of the loss cause; it is not recommended as a general default.
- **UDP buffers**: for high PPS or bursts, set `udp_rcvbuf`/`udp_sndbuf` (e.g. 1~4MB). The library prefers
  `SO_RCVBUFFORCE`/`SO_SNDBUFFORCE` (requires `CAP_NET_ADMIN`) and falls back to ordinary `setsockopt` without
  privileges, logging the actual values. Note that oversized buffers can increase queuing latency; for low-latency
  scenarios choose a compromise based on measurements rather than blindly using the system maximum.
- **CPU pinning**: for latency-sensitive scenarios, pin the RX/TX processes to different **performance cores** and
  avoid interrupt cores. Measurements show max jitter of hundreds of microseconds without pinning, dropping to about
  20~30us when pinned to two different performance cores:

  ```sh
  SX_CPU_SERVER=2 SX_CPU_CLIENT=3 sudo scripts/xdp_latency_2p.sh build/test_latency
  ```

- **Queues**: currently one endpoint binds one RX queue (`xdp_queue`), and only one SX XDP endpoint is allowed per NIC;
  multi-queue horizontal scaling is not implemented, so do not deploy "one endpoint/process per queue". Scale with
  multiple NICs/netns for now.
- **Logging**: use `SX_LOG_WARN` in production; temporarily enable `SX_LOG_DEBUG` when debugging reliability issues.

---

## 14. Deployment and privileges

### 14.1 Capabilities required by AF_XDP

AF_XDP needs to load and attach an XDP program and create AF_XDP sockets, so it requires privileges or equivalent
capabilities:

| Capability | Purpose | Applicable kernels |
| --- | --- | --- |
| `CAP_BPF` | Load BPF programs | >= 5.8 |
| `CAP_SYS_ADMIN` | Load BPF programs (legacy interface) | < 5.8 |
| `CAP_NET_ADMIN` | Attach/detach XDP, some NIC configuration | All |
| `CAP_NET_RAW` | Create AF_XDP sockets | All |
| `CAP_NET_BIND_SERVICE` | Bind ports below 1024 | Only for low ports |

A sufficient memlock limit is also required: AF_XDP UMEM memory is accounted against `RLIMIT_MEMLOCK`, so raise the
limit (`ulimit -l unlimited`, or grant the process `CAP_IPC_LOCK`). For least-privilege deployment, prefer `setcap` or
systemd rather than running the whole process as root:

```sh
# Kernel >= 5.8: grant the minimum capability set
sudo setcap cap_bpf,cap_net_admin,cap_net_raw+ep /path/to/app

# Add this when binding ports below 1024
sudo setcap cap_bpf,cap_net_admin,cap_net_raw,cap_net_bind_service+ep /path/to/app
```

```ini
# systemd unit fragment
[Service]
AmbientCapabilities=CAP_BPF CAP_NET_ADMIN CAP_NET_RAW
CapabilityBoundingSet=CAP_BPF CAP_NET_ADMIN CAP_NET_RAW
LimitMEMLOCK=infinity
```

`setcap` capabilities are attached to the executable file, so they must be re-applied after upgrading or replacing the
binary; also ensure that the file and its directory cannot be replaced by ordinary users, otherwise it becomes a
privilege-escalation path.

### 14.2 Kernel UDP backend

The kernel UDP backend runs as an ordinary user and requires no capabilities. The only exception is
`CAP_NET_BIND_SERVICE` when binding ports below 1024. When configuring `udp_rcvbuf`/`udp_sndbuf` without
`CAP_NET_ADMIN`, the library falls back to ordinary `setsockopt`, and the actual values are limited by
`net.core.rmem_max`/`net.core.wmem_max`.

### 14.3 Backend selection and fallback

- No `ifname`: `AUTO` uses kernel UDP directly and requires no privileges.
- `ifname` set and `AUTO`: prefer AF_XDP; on insufficient privileges, attach failure, or unsupported NIC, fall back to
  UDP and set `stats.backend_fallback=1`.
- Forced `SX_BACKEND_XDP`: no downgrade; `sx_start` returns `SX_ERR_BACKEND` on initialization failure.

### 14.4 XDP cleanup

A normal exit detaches XDP. If the process is SIGKILLed or exits abnormally, an XDP program may remain; clean it up
with `sudo xdp-loader unload <if> --all` or `bpftool net detach`.

---

## 15. FAQ

**Q: No data received?**
For AF_XDP check: (1) whether target traffic is steered to the bound RX queue; (2) whether both ends are on the same
L2 subnet (ARP), and for cross-subnet whether `gateway_ip`/`netmask` are configured and a static mapping is registered
for the gateway; (3) whether XDP attached successfully (`bpftool net show dev <if>`); (4) whether ports and `conv`
match. For the UDP backend, check firewalls and port binding.

**Q: `sx_send` returns `SX_ERR_WINDOW`?**
The send window is full; retry later (call `sx_update`/`sx_wait` first to process ACKs and open the window), or check
with `sx_link_wnd`.

**Q: How large can a single packet be?**
`sx_max_payload(ep)`, 1400 by default, absolute maximum 1426; the per-message limit is controlled by
`sx_max_message(ep)`, 64KB by default.

**Q: Are multicast/broadcast messages retransmitted?**
Yes, but only best-effort: the receiver detects gaps and sends NACK, and the sender retransmits from history to the
group address; delivery is not guaranteed.

**Q: Can I reply directly inside `on_recv`?**
Yes. The library uses a recursive lock, so calling `sx_send(ep, link, ...)` inside a callback is safe (`link` is the
source link).

**Q: How can I confirm traffic really goes through XDP?**
Use `bpftool net show dev <if>` to see the attached program and `bpftool map dump name sx_stats_map` for redirect
counts.

**Q: Can it run without root?**
Yes. The kernel UDP backend requires no privileges; AF_XDP requires root or capabilities such as `CAP_BPF`,
`CAP_NET_ADMIN`, and `CAP_NET_RAW`. `AUTO` falls back to UDP when privileges are insufficient and sets
`stats.backend_fallback=1`. See section 14 for details.

---

## 16. Examples and tests

- `examples/sx_p2p.c`: unicast request/echo (two-phase), `make examples && ./build/sx_p2p`.
- `examples/sx_group.c`: multicast demo with one sender and two receivers.
- `tests/test_frame.c`: frame encode/decode and checksum.
- `tests/test_proto.c`: protocol header boundaries.
- `tests/test_kcp.c`: injects loss + reordering, verifies ordering and retransmission.
- `tests/test_loopback.c`: UDP backend end-to-end bidirectional ordering.
- `tests/test_xdp.c` + `scripts/veth_test.sh`: AF_XDP end-to-end over veth.
- `tests/test_latency.c` + `scripts/xdp_latency.sh` / `xdp_latency_2p.sh`: latency and bpftool verification.
