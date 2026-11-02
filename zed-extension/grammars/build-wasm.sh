#!/usr/bin/env bash
# 编译 tree-sitter-zx 语法为 Zed 可用的 wasm（wasi reactor / 动态链接模型）
#
# 要求：与官方 wasi-sdk 产物同构 ——
#   imports: env.memory / env.__indirect_function_table / env.__memory_base / env.__table_base
#   exports: tree_sitter_zx / _initialize / __wasm_apply_data_relocs
# tree-sitter 的 WasmStore 为所有语法共享一块内存，靠 __memory_base 偏移布局，
# 若用 freestanding/-no-entry 静态模型自建内存，Zed 装载后解析文件时会崩溃。
#
# 用法: ./build-wasm.sh [zig路径]
#   默认使用 zx 自带工具链 .toolchain/zig.exe（zig cc 即 clang + wasm lld）
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$(dirname "$0")/tree-sitter-zx"

ZIG="${1:-$REPO_ROOT/.toolchain/zig.exe}"

# 最小 libc stub（parser.c 只用 NULL/bool/stdint），避免拉入 wasi crt
STUB="$(mktemp -d)"
cat > "$STUB/stdbool.h" <<'EOF'
#ifndef STDBOOL_H
#define STDBOOL_H
#define bool _Bool
#define true 1
#define false 0
#endif
EOF
cat > "$STUB/stdint.h" <<'EOF'
#ifndef STDINT_H
#define STDINT_H
typedef signed char int8_t;
typedef short int16_t;
typedef int int32_t;
typedef long long int64_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
#endif
EOF
cat > "$STUB/stdlib.h" <<'EOF'
#ifndef STDLIB_H
#define STDLIB_H
typedef unsigned long size_t;
#define NULL ((void*)0)
#endif
EOF

# glue: reactor 入口。共享模块的被动数据段由 __wasm_call_ctors 初始化，
# tree-sitter 装载语法后会调用导出的 _initialize。
cat > "$STUB/glue.c" <<'EOF'
extern void __wasm_call_ctors(void);
__attribute__((export_name("_initialize")))
void _initialize(void) { __wasm_call_ctors(); }
EOF

"$ZIG" cc --target=wasm32-wasi -nostdlib -shared -fPIC \
    -I"$STUB" \
    -Wl,--no-entry -Wl,--export=tree_sitter_zx \
    -O2 -o ../zx.wasm src/parser.c "$STUB/glue.c"

rm -rf "$STUB" 2>/dev/null || true
echo "OK: ../zx.wasm"
