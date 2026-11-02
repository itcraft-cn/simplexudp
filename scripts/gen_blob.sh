#!/bin/sh
# gen_blob.sh - 将 BPF 目标对象转为内嵌 C 字节数组
# 用法：gen_blob.sh <input.o> <output.c> [symbol]
#
# 兼容性说明：
#   - 旧版 xxd（如 CentOS 7 的 V1.10）不支持 -n 指定符号名，直接使用会失败，
#     而管道中的失败可能被 sed 吞掉并生成空文件。
#   - 因此先用可移植的 `xxd -i` 生成默认符号，再统一重命名为目标符号，
#     并在最后校验输出非空。
set -eu

in="$1"
out="$2"
name="${3:-sx_xdp_kern_o}"

if [ ! -f "$in" ]; then
    echo "gen_blob: input not found: $in" >&2
    exit 1
fi

tmp="${out}.tmp.$$"
trap 'rm -f "$tmp"' EXIT HUP INT TERM

xxd -i "$in" > "$tmp"
sym=$(sed -n 's/^unsigned char \(.*\)\[\] =.*/\1/p' "$tmp")
if [ -z "$sym" ]; then
    echo "gen_blob: cannot determine xxd symbol in $tmp" >&2
    exit 1
fi

sed -e "s/${sym}/${name}/g" \
    -e 's/^unsigned char/const unsigned char/' \
    -e 's/^unsigned int/const unsigned int/' "$tmp" > "$out"

if [ ! -s "$out" ]; then
    echo "gen_blob: empty output: $out" >&2
    exit 1
fi
