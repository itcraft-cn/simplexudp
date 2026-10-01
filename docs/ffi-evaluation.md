# SX FFI 可行性评估（Python / Java）

范围：评估将 `simple-xudp`(SX) 暴露给 Python（Cython）与 Java（JNI / Unsafe+mmap+线程唤醒）
两条路线的可行性、性能上限、复杂度与风险。本文不含实现代码，只做评估与建议。

---

## 0. 结论摘要

| 方案 | 可行性 | 性能上限 | 复杂度 | 结论 |
| --- | --- | --- | --- | --- |
| Python + Cython + Pull 模式 | 高 | 中（几十万~百万 msg/s） | 低 | 可行，推荐作为首选 |
| Python + Cython + 回调模式 | 高 | 低（GIL + 逐消息回调） | 中 | 可行但仅适合低频 |
| Python + 共享内存 ring | 高 | 高 | 高 | 需要 SX 先提供 ring，二期 |
| Java + JNI + 直接 API | 中 | 低（逐消息 JNI 失速） | 中 | 仅控制面可用，数据面不推荐 |
| Java + JNI + Unsafe + mmap + 唤醒 | 中 | 高 | 很高 | 思路正确但 `Unsafe` 路线将失效 |
| Java + FFM(java.lang.foreign) + mmap ring | 高 | 高 | 高 | 推荐的现代路线（JDK 22+） |

核心判断：

1. **两条路线都可行，但前提是数据面不要逐消息跨语言调用**。逐消息的 C→Python / C→Java
   回调是性能杀手，尤其 Java 的 native→JVM 回调需要 `AttachCurrentThread` 且代价高。
2. SX 当前已具备对 FFI 友好的关键形态：**不透明句柄 + 拉取式 `sx_recv` + `sx_wait`**。
   只要能避免回调，Python/Java 都可以直接跑通。
3. 高吞吐/低延迟目标下，正解是 **共享内存环形队列(SPSC) + 事件 fd 唤醒**，语言侧用
   `memoryview`/`MemorySegment` 读写，跨语言只传“通知”不传“每条消息”。
4. Java 侧的 `sun.misc.Unsafe` + mmap 是上一代做法；JDK 23 起内存访问方法被弃用/封装
   （JEP 471/498），应改用 **Java 22+ 的 Foreign Function & Memory API(FFM)**，其
   `Linker` 可免 JNI 直接 downcall，`upcallStub` 可注册回调，且 `MemorySegment` 官方支持 mmap。
5. 为支撑 FFI，SX 需要少量增强：暴露后端 fd、可选的零拷贝收包、共享内存 ring + 通知、
   以及固定的 ABI/布局版本号。否则各语言只能逐消息调用，性能受限。

---

## 1. SX 现状对 FFI 的适配性

有利点：

- 句柄不透明（`sx_endpoint_t*`/`sx_link_t*`），适合跨语言持有裸指针。
- 提供 **拉取模式**（`on_recv=NULL` + `sx_recv`），可完全避免 C→宿主语言回调。
- `sx_wait(ep, timeout)` 暴露了阻塞等待，便于宿主语言在自己的事件循环里配合。
- `rx_thread` 可选，允许把网络线程留在 C 侧。
- 参数/返回均为固定宽度标量或指针，ABI 简单。

不利点：

- `on_recv/on_event/on_log` 是 C 函数指针，跨语言回调成本高（Python 需 GIL，Java 需 attach）。
- `sx_recv` 是**拷贝式**（写入调用者缓冲），无零拷贝视图；高吞吐会多一次内存拷贝。
- 未暴露后端 fd；宿主语言无法用 `select/epoll/kqueue` 直接等待，只能靠 `sx_wait` 或轮询。
- 逐消息 API 粒度：每条消息一次跨语言调用，天然限制吞吐。
- `sx_send` 内部有端点级互斥锁，多宿主线程并发发送会串行化。
- 依赖 `libbpf/libxdp` 与内嵌 BPF 目标，增加宿主分发包体积与平台耦合。

结论：**当前形态足以支撑“可用的 FFI 封装”，但要达到“高性能 FFI”，需补充 ring/fd/零拷贝。**

---

## 2. Python 方案评估

### 2.1 绑定技术选型

| 技术 | 开发成本 | 运行性能 | 回调支持 | 适用 |
| --- | --- | --- | --- | --- |
| `ctypes` | 最低 | 低 | CFUNCTYPE，慢 | 原型/脚本 |
| `cffi`(ABI/API 模式) | 低 | 中 | `@ffi.callback`，较慢 | 通用 |
| **Cython** | 中 | 中高 | `cdef` + `with gil` | 推荐 |
| `pybind11`(C++ 编译器) | 中 | 中高 | 好 | 已有 C++ 工具链时 |

推荐 Cython：可直接 `cdef extern from "simplexudp.h"`，对裸指针、结构体、函数指针都有原生支持，
且能在热点路径用 `nogil` 释放 GIL。

### 2.2 Python 侧关键问题

1. **GIL 与阻塞**：`sx_run`/`sx_wait` 会阻塞，必须在 `nogil` 段调用，否则冻结解释器。
   `sx_wait` 返回后再在持有 GIL 的情况下 `sx_update`/`sx_recv`。
2. **回调（最需要规避）**：C 回调进 Python 必须 `PyGILState_Ensure`，
   若回调发生在 C 的内部线程（`rx_thread=1`），还要处理线程态与解释器状态。
   数据面强烈建议**用拉取模式替代 `on_recv`**；`on_event`/`on_log` 低频，可保留回调。
3. **数据交付的拷贝**：`sx_recv` 先写入预分配 C buffer（可在 `nogil` 下），再构造 `bytes`
   （需 GIL）。每条一次 `bytes` 分配是主要开销之一。可选优化：提供零拷贝收包，
   Cython 用 `memoryview` 包住内部指针。
4. **asyncio 集成**：若 SX 暴露后端 fd，可用 `loop.add_reader(fd, ...)`，把网络事件并入
   asyncio 事件循环，无需额外线程；当前需自行在 C 侧线程或 Python 线程里跑 `sx_wait`。
5. **分发**：Cython 需编译扩展，产物为平台相关 wheel（manylinux+aarch64）。若含 AF_XDP，
   需一并打包 `libbpf/libxdp` 与 BPF 目标，wheel 复杂；建议提供 **UDP-only 编译开关**，
   AF_XDP 作为可选系统库依赖。

### 2.3 Python 性能预期

- 拉取模式、`nogil` 等待、单线程：瓶颈在 Python 对象分配与循环，量级约几十万~百万 msg/s。
- 逐消息回调模式：明显更低（GIL 切换 + 回调封送），仅适合低频控制/监控。
- 要突破到百万级以上并有稳定尾延迟，需要共享内存 ring（见第 5 节）。

**结论：Python + Cython 完全可行且推荐，优先拉取模式 + nogil；高性能再叠加 ring。**

---

## 3. Java 方案评估

### 3.1 方案 A：Java + JNI（直接调用 SX API）

- 每条消息一次 JNI 进出：参数封送、异常检查、`GetByteArrayRegion/SetByteArrayRegion` 拷贝，
  JNI 单次开销约数百 ns 量级；高 pps 下**会成为主瓶颈**，即用户所说的“失速”。
- C→Java 回调更糟：native 线程必须 `AttachCurrentThread` 才能回调 Java，attach/detach
  与 JNI 回调代价高，且线程管理复杂。
- 因此：**仅适合控制面/低频**（建链、配置、统计），不适合数据面。

### 3.2 方案 B：JNI load + Unsafe + mmap + C 线程唤醒

思路正确（Aeron/Agrona 风格），但有关键难点与时代性问题：

1. **C 如何唤醒 Java 线程**：这是最容易踩坑的点。
   - `LockSupport.unpark` 依赖 JVM 内部对象/futex，C 侧无法直接触发；想用就得经 JNI 调 Java
     方法，又回到昂贵的回调。
   - 稳妥做法一：native 写 **eventfd/pipe**（其 fd 可由 C 通过 JNI 暴露给 Java），
     Java 用 NIO `Selector`/Netty `EpollEventLoop` 阻塞等待，被唤醒后读 mmap ring。
   - 稳妥做法二：Java 侧对共享 volatile 序号做 **自旋 + park 退避** 轮询，native 只写内存 +
     内存屏障；实现简单但占一个核，且退避策略影响延迟。
2. **`sun.misc.Unsafe` 的可持续性**：JDK 23 起内存访问方法被弃用并逐步封装（JEP 471/498），
   JDK 26 起将直接抛异常。以 `Unsafe` 做 mmap 读写属于**技术债**。
3. **ABI 脆弱**：Java 直接按字节偏移读写 C 结构体，一旦 SX 结构体/布局变化即静默损坏；
   需要 SX 提供**版本化、固定宽度、显式 padding** 的共享内存布局。

### 3.3 方案 C（推荐）：Java 22+ FFM + 共享内存 ring

- `java.lang.foreign` 已稳定（JEP 454，JDK 22）：`Linker` 免 JNI 直接 downcall，
  `upcallStub` 注册回调，`Arena`/`MemorySegment` 官方支持 mmap 与堆外内存。
- 数据面：SX 写入 mmap 的 SPSC ring，Java 用 `MemorySegment` 直接读；
  唤醒用 eventfd/pipe + NIO Selector，或自旋轮询。
- 控制面：用 FFM downcall 调 `sx_add_unicast/sx_start/sx_get_stats` 等，低频调用开销可接受。
- 相比 `Unsafe`：官方支持、跨版本稳定、无弃用风险；相比 JNI：downcall 更快、无 `AttachCurrentThread`。

**结论：Java 侧“JNI+Unsafe+mmap+线程唤醒”思路可行，但应升级为 FFM；数据面务必走共享内存
ring + fd 唤醒，避免逐消息 JNI/回调。纯 JNI 直调会失速，仅用于控制面。**

---

## 4. 共享内存 ring + 唤醒（两个语言共同的正解）

要达到高吞吐/低尾延迟，核心是**把“每条消息一次跨语言调用”变成“一次内存写入 + 一次通知”**。

设计要点（对 SX 的通用要求）：

- **SPSC 环形队列**：单生产者/单消费者，固定长度记录（净荷 <= `max_payload`），
  原子读写 head/tail 序号；读写两侧用内存屏障（C: `atomic`，Java: `VarHandle` acquire/release）。
- **双向**：
  - 收：C 网络线程写 ring，通知宿主读取；
  - 发：宿主写 ring，通知 C 发送线程（或宿主直接 FFM downcall `sx_send`，视批量化程度）。
- **唤醒机制**：
  - 首选 eventfd（Linux）或 pipe，fd 交给宿主用 epoll/Selector 等待；
  - 备选自旋 + 退避（牺牲一个核换最低延迟）。
- **无锁与批处理**：批量生产/消费，减少通知次数；通知可合并（只有从空到非空时触发）。
- **生命周期与背压**：ring 满/空的显式语义；关闭顺序（先停通知、再停线程、最后 unmap）。
- **ABI 稳定**：ring 头部包含 magic + 版本 + 记录布局，宿主启动时校验版本，不匹配即拒绝。

---

## 5. 建议的 SX 扩展点（为 FFI 铺路）

按性价比排序：

1. `int sx_fd(sx_endpoint_t*)`：暴露后端可 poll 的 fd（UDP socket / xsk fd）
   —— 让 Python/Java 能用各自的事件循环直接等待，无需额外线程或忙等。
2. `ssize_t sx_recv_ref(ep, const void **data, size_t *len, ...)`：零拷贝收包，
   指针有效期至下次 `sx_update`/`sx_recv*`；Cython 用 `memoryview`、Java 用 `MemorySegment`
   直接引用，省一次拷贝（并支持 `nogil`）。
3. **共享内存 ring 接口**：`sx_ring_t`（mmap 创建/打开 + 通知 fd + 批量收发），
   作为“高性能 FFI 通道”；C 内部仍复用现有 KCP 可靠层，只是在 ring 与网络之间搬运。
4. 批量 API：`sx_recv_batch` / `sx_send_batch`，摊薄调用与通知次数。
5. ABI/版本：`sx_abi_version()`、固定宽度类型、显式 padding、ring 头版本校验。
6. 分发：提供 `UDP-only` 编译开关（不依赖 libbpf/libxdp），降低 Python wheel/Java 原生库
   的打包与平台耦合；AF_XDP 作为可选后端。

---

## 6. 方案对比总表

| 维度 | Python+Cython | Java+JNI 直调 | Java+Unsafe+mmap | Java+FFM+ring |
| --- | --- | --- | --- | --- |
| 可行性 | 高 | 中 | 中（将失效） | 高 |
| 数据面吞吐 | 中 | 低 | 高 | 高 |
| 尾延迟 | 中 | 差 | 良 | 良 |
| 开发复杂度 | 低 | 中 | 很高 | 高 |
| 版本可持续性 | 好 | 好 | 差（JEP 471/498/…) | 好（JDK 22+） |
| 对 SX 改动 | 小（fd/零拷贝更佳） | 小 | 大（需 ring+ABI） | 大（需 ring+ABI） |
| 回调成本 | 中（GIL） | 高（attach） | 规避 | 规避 |

---

## 7. 推荐路线与里程碑

Python：

- 一期：Cython 封装，**拉取模式 + `nogil` 等待**；`on_event/on_log` 保留回调。
  目标：功能可用，吞吐几十万 msg/s。
- 二期：`sx_fd` + `asyncio.add_reader` 集成，`sx_recv_ref` 零拷贝 `memoryview`。

Java：

- 一期：**FFM（JDK 22+）downcall 做控制面**，数据面先用 FFM 逐消息 downcall（中等速率）。
- 二期：**共享内存 ring + eventfd + NIO Selector/Netty** 数据面；C 提供 `sx_ring`，Java 用
  `MemorySegment` 读写。彻底避免 per-message JNI/回调。
- 不建议：`sun.misc.Unsafe` 新代码；纯 JNI 逐消息数据面。

共同前置：SX 增加 `sx_fd`、`sx_recv_ref`、`sx_ring`（含 ABI 版本），并提供 UDP-only 构建开关。

---

## 8. 风险

- **跨语言回调线程态**：C 内部线程回调宿主（Python GIL / Java attach）易错且慢；用拉取/ring 规避。
- **ABI 漂移**：Java/Python 直接按偏移访问结构体，必须版本化并校验。
- **依赖与分发**：libbpf/libxdp 与 BPF 目标增加打包复杂度；建议 UDP-only 变体。
- **`Unsafe` 淘汰**：新代码不应依赖；改用 FFM。
- **锁竞争**：`sx_send` 端点锁在多宿主线程下串行；ring/批量可缓解。
- **通知丢失/惊群**：eventfd 需处理 EAGAIN 与合并通知，避免丢唤醒或空转。

---

## 9. 最终结论

- **Python + Cython：可行且推荐**；以拉取模式 + `nogil` 起步，高性能再加 ring/零拷贝。
- **Java + JNI 直调：可行但会失速**，只适合控制面。
- **Java + JNI + Unsafe + mmap + 线程唤醒：思路可行**，但 `Unsafe` 路线将失效，
  应升级为 **FFM(JDK 22+) + 共享内存 ring + eventfd/Selector**；C 侧需提供 ring 与 fd。
- 无论哪种语言，**“共享内存 ring + 事件 fd 唤醒”是高性能 FFI 的公共正解**；
  SX 需按第 5 节补充少量扩展点即可同时服务 Python 与 Java。
