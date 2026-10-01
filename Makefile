# simple-xudp (SX) 构建脚本
#
# 目标：
#   make            构建静态库/动态库
#   make examples   构建示例
#   make tests      构建测试
#   make check      运行单元/回环测试（无需 root）
#   make bpf        仅编译 XDP BPF 目标
#   make clean
#
# 说明：
#   - 自动探测架构（x86_64 / aarch64），BPF 目标用 clang -target bpf 编译；
#   - XDP 目标对象内嵌进库（xxd 生成字节数组），运行时可被 SX_XDP_OBJ 覆盖。

CC      ?= gcc
AR      ?= ar
BPF_CLANG ?= clang

ARCH      := $(shell uname -m)
MULTIARCH := $(shell $(CC) -print-multiarch 2>/dev/null || echo $(ARCH)-linux-gnu)

CFLAGS    ?= -std=c99 -O2 -g
WARN      := -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-qual
CPPFLAGS  += -Iinclude -Isrc -D_GNU_SOURCE -DSX_XDP_EMBEDDED
LDLIBS    += -pthread

PKG_OK := $(shell pkg-config --exists libbpf libxdp 2>/dev/null && echo yes)
ifeq ($(PKG_OK),yes)
  CPPFLAGS += $(shell pkg-config --cflags libbpf libxdp)
  LDLIBS   += $(shell pkg-config --libs libbpf libxdp)
else
  LDLIBS   += -lbpf -lxdp -lelf -lz
endif

BUILD      := build
BPF_OBJ    := $(BUILD)/sx_xdp_kern.o
BLOB_C     := $(BUILD)/sx_xdp_kern_blob.c
BPF_CFLAGS := -target bpf -O2 -g -I/usr/include/$(MULTIARCH)

LIB_SRCS   := $(wildcard src/*.c)
LIB_OBJS   := $(patsubst src/%.c,$(BUILD)/%.o,$(LIB_SRCS)) $(BUILD)/sx_xdp_kern_blob.o

STATIC_LIB := $(BUILD)/libsimplexudp.a
SHARED_LIB := $(BUILD)/libsimplexudp.so

EXAMPLES   := $(BUILD)/sx_p2p $(BUILD)/sx_group
TESTS      := $(BUILD)/test_frame $(BUILD)/test_proto $(BUILD)/test_kcp $(BUILD)/test_loopback

.PHONY: all bpf examples tests tests-xdp tests-latency check check-xdp check-latency clean
all: $(STATIC_LIB) $(SHARED_LIB)

bpf: $(BPF_OBJ)

# ---- BPF 目标 ----
$(BPF_OBJ): bpf/sx_xdp_kern.c | $(BUILD)
	$(BPF_CLANG) $(BPF_CFLAGS) -c $< -o $@

# 生成内嵌字节数组（const）。
$(BLOB_C): $(BPF_OBJ)
	xxd -i -n sx_xdp_kern_o $< | sed -e 's/^unsigned char/const unsigned char/' -e 's/^unsigned int/const unsigned int/' > $@

$(BUILD)/sx_xdp_kern_blob.o: $(BLOB_C) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- 库 ----
$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(CPPFLAGS) -fPIC -c $< -o $@

$(STATIC_LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(SHARED_LIB): $(LIB_OBJS)
	$(CC) -shared -o $@ $^ $(LDLIBS)

# ---- 示例 ----
examples: $(EXAMPLES)

$(BUILD)/sx_p2p: examples/sx_p2p.c $(STATIC_LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(CPPFLAGS) $< $(STATIC_LIB) -o $@ $(LDLIBS)

$(BUILD)/sx_group: examples/sx_group.c $(STATIC_LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(CPPFLAGS) $< $(STATIC_LIB) -o $@ $(LDLIBS)

# ---- 测试 ----
tests: $(TESTS)

$(BUILD)/test_%: tests/test_%.c $(STATIC_LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(CPPFLAGS) $< $(STATIC_LIB) -o $@ $(LDLIBS)

tests-xdp: $(BUILD)/test_xdp

tests-latency: $(BUILD)/test_latency

check: tests
	@mkdir -p $(BUILD)
	@$(BUILD)/test_frame  $(BUILD)/test_frame.log;  echo "test_frame  exit=$$?"
	@$(BUILD)/test_proto  $(BUILD)/test_proto.log;  echo "test_proto  exit=$$?"
	@$(BUILD)/test_kcp    $(BUILD)/test_kcp.log;    echo "test_kcp    exit=$$?"
	@$(BUILD)/test_loopback $(BUILD)/test_loopback.log; echo "test_loopback exit=$$?"

# 需要 root：创建 veth 并跑 AF_XDP 端到端测试。
check-xdp: tests-xdp
	@chmod +x scripts/veth_test.sh
	@sudo scripts/veth_test.sh $(BUILD)/test_xdp

# 需要 root：AF_XDP 时延测试，并用 bpftool 佐证流量经过 XDP。
check-latency: tests-latency
	@chmod +x scripts/xdp_latency.sh
	@sudo scripts/xdp_latency.sh $(BUILD)/test_latency

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
