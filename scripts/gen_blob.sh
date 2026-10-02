#!/bin/sh
# gen_blob.sh - 将 BPF 目标对象转为内嵌 C 字节数组
# 用法：gen_blob.sh <input.o> <output.c> [symbol]
set -euo pipefail
in="$1"
out="$2"
name="${3:-sx_xdp_kern_o}"
xxd -i -n "$name" "$in" \
    | sed -e 's/^unsigned char/const unsigned char/' \
          -e 's/^unsigned int/const unsigned int/' > "$out"
