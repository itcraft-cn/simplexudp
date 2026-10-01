# SX FFI 待办（TODO / Backlog）

本文件记录 `simple-xudp`(SX) 面向外部语言（Python / Java 等）的 FFI 支持计划与待办项，
作为长期记录。评估结论见 `docs/ffi-evaluation.md`。

## 当前聚焦（Focus）

> 现阶段**聚焦 C 库本身的实现与打磨**，FFI 相关工作整体**暂缓**，仅在此登记，不排期。

原因：

- 可靠层（KCP）、AF_XDP/UDP 双后端、组播/广播、构建与测试仍需持续完善；
- FFI 的高性能形态依赖 C 侧新增原语（fd / 零拷贝 / 共享内存 ring / ABI 版本），
  这些属于 C 库能力，应先在 C 侧稳定，再对外封装；
- 避免过早为多语言封装引入新的抽象负担。

## 已具备的 C 侧基础（对 FFI 友好）

- [x] 不透明句柄（`sx_endpoint_t*` / `sx_link_t*`）
- [x] 拉取式收包（`on_recv=NULL` + `sx_recv`），可规避跨语言回调
- [x] 阻塞等待 `sx_wait`（配合宿主事件循环）
- [x] 可选 `rx_thread` 把网络线程留在 C 侧
- [x] 分层清晰（`sx_kcp` 只依赖 `sx_io`），便于插入 ring 通道

## 待办清单

状态：`TODO` / `DOING` / `DONE` / `DROPPED`　优先级：P0 > P1 > P2

### A. C 侧 FFI 原语（前置，优先级最高）

- [ ] **FFI-001（P0）暴露后端 fd** `int sx_fd(sx_endpoint_t *ep)`
  - 动机：让 Python/Java 用自身事件循环（asyncio / NIO Selector / Netty）直接等待，免额外线程或忙等。
  - 依赖：无。
  - 验收：UDP 返回 socket fd；XDP 返回 xsk fd；未启动返回负错误码。

- [ ] **FFI-002（P0）零拷贝收包** `ssize_t sx_recv_ref(ep, const void **data, size_t *len, sx_addr_t *src, sx_link_t **link)`
  - 动机：省去 `sx_recv` 的第二次拷贝；Cython 用 `memoryview`、Java 用 `MemorySegment` 直接引用。
  - 约束：返回指针有效期至下次 `sx_update`/`sx_recv*`；文档需显著标注。
  - 验收：与 `sx_recv` 语义一致，含有效期与失效条件说明。

- [ ] **FFI-003（P1）批量收发** `sx_recv_batch` / `sx_send_batch`
  - 动机：摊薄跨语言调用与通知次数。
  - 依赖：FFI-002 更佳。
  - 验收：批量接口有明确的返回计数与错误语义。

- [ ] **FFI-004（P0）ABI/布局版本化** `uint32_t sx_abi_version(void)`
  - 动机：宿主直接按偏移访问结构体/共享内存时必须校验版本，避免静默损坏。
  - 内容：固定宽度类型、显式 padding、结构体尺寸；ring 头含 magic+version。
  - 验收：版本不匹配时宿主可检测并拒绝。

- [ ] **FFI-005（P1）事件/日志的可轮询接口**（回调之外）
  - 动机：避免跨语言回调；让宿主拉取事件与日志。
  - 方案：`sx_poll_event(ep, sx_event_t*, sx_link_t**)` / 日志环形缓冲。
  - 验收：事件不丢、不过量占用。

- [ ] **FFI-006（P2）UDP-only 编译开关**
  - 动机：打包 wheel / Java 原生库时避免强依赖 libbpf/libxdp 与 BPF 目标。
  - 验收：`SX_NO_XDP` 构建不链接 libbpf/libxdp，功能仅 UDP 后端。

### B. 共享内存通道（高性能 FFI 的公共正解）

- [ ] **FFI-011（P1）SPSC ring 收发通道** `sx_ring_t`
  - 动机：把“每条消息一次跨语言调用”变成“一次内存写 + 一次通知”。
  - 设计：mmap + 固定长度记录（净荷 <= max_payload）+ 原子 head/tail + 内存屏障；
    双向（收/发）；订阅通知 fd（eventfd）。
  - 依赖：FFI-004（布局版本）。
  - 验收：C 侧单测 + 与现有可靠层对接；背压（满/空）语义明确。

- [ ] **FFI-012（P1）通知 fd 与合并唤醒**
  - 动机：ring 从空到非空时通知；避免惊群与空转。
  - 验收：EAGAIN/合并通知处理正确，不丢唤醒。

- [ ] **FFI-013（P2）ring 使用文档与示例（纯 C）**
  - 验收：`docs/manual.md` 增补 ring 章节；提供一个 C 示例。

### C. Python 封装（Cython）

- [ ] **FFI-101（P1）Cython 绑定（拉取模式 + `nogil`）**
  - 内容：`cdef extern` 包装 API；阻塞调用在 `nogil`；数据走拉取，事件/日志可回调。
  - 依赖：FFI-001/002 更佳。
  - 验收：示例可收发；无 GIL 卡死；异常/错误码映射。

- [ ] **FFI-102（P2）asyncio 集成**（基于 FFI-001 `loop.add_reader`）
- [ ] **FFI-103（P2）零拷贝 `memoryview`**（基于 FFI-002）
- [ ] **FFI-104（P2）打包**：manylinux/aarch64 wheel；UDP-only 可选（FFI-006）
- [ ] **FFI-105（P2）文档与示例**（p2p / 组播 / asyncio）

### D. Java 封装

- [ ] **FFI-201（P1）FFM downcall（JDK 22+）控制面**
  - 内容：`Linker` 调用 `sx_create/start/add_unicast/send/get_stats` 等；避免 JNI。
  - 依赖：无（控制面可先行）。
  - 验收：Java 示例可建链/发送/查询统计。
- [ ] **FFI-202（P1）数据面：mmap ring + eventfd + NIO Selector/Netty**
  - 依赖：FFI-011/012。
  - 明确：**不采用逐消息 JNI/回调**；不使用 `sun.misc.Unsafe` 做内存访问。
- [ ] **FFI-203（P2）FFM `upcallStub` 注册事件回调**（可选，低频事件）
- [ ] **FFI-204（P2）打包与示例**（p2p / 组播 / 背压）
- [ ] **FFI-205（P2）JDK 版本与降级策略说明**（FFM 需 JDK 22+）

### E. 通用

- [ ] **FFI-301（P2）线程模型文档**：回调线程态（C 线程 vs 应用线程）、`rx_thread` 影响
- [ ] **FFI-302（P2）错误/异常映射规范**：`sx_error_t` → Python 异常 / Java 异常
- [ ] **FFI-303（P2）内存与生命周期规范**：句柄所有权、关闭顺序、失效指针
- [ ] **FFI-304（P2）ABI 兼容性测试**：host 侧结构体尺寸/偏移断言

## 里程碑（草案，未排期）

- M1：C 侧 `sx_fd` + `sx_recv_ref` + `sx_abi_version`（服务普通 FFI）。
- M2：C 侧共享内存 ring + 通知 fd（服务高性能 FFI）。
- M3：Python Cython（拉取/`nogil`），可选 asyncio/零拷贝。
- M4：Java FFM 控制面 + ring 数据面。

## 非目标（暂不考虑）

- Windows 平台 FFI（SX 目前仅 Linux）。
- `sun.misc.Unsafe` 路线的正式支持（仅作为历史评估记录）。
- 逐消息 JNI 回调的数据面（明确排除为性能不可接受）。

## 启动 FFI 的触发条件

满足任一即可把 FFI 从“暂缓”升为“进行”：

1. C 库 API/ABI 进入相对稳定期（可靠层、后端、错误模型不再频繁变动）；
2. 出现明确的 Python/Java 集成需求与验收场景；
3. FFI-001/002/004 已在 C 侧落地。

## 变更记录

- 2026-10-01：创建本文件；确立“聚焦 C 库、FFI 暂缓”的方针与上述 backlog。
