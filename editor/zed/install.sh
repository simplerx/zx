#!/bin/sh
# 把 editor/zed/ 完整安装到 Zed 的扩展目录（本地 dev 安装，不联网）。
#
# 用法：sh editor/zed/install.sh
#
# 前置：仓库根跑过 `sh build.sh`（生成 bin/zxls.exe——语言服务器本身）。
# 安装完在 Zed 里重开窗口，或执行 `editor: restart language server`。
#
# 注意：Zed 的卸载会删掉整个 installed/zx 目录，重装必须恢复全部 6 个文件：
#   extension.toml / extension.wasm / grammars/zx.wasm / languages/zx/{config,highlights,outline}
set -e
SRC="$(cd "$(dirname "$0")" && pwd)"
INST="${LOCALAPPDATA:-$HOME/.local/share}/Zed/extensions/installed/zx"

if [ ! -f "$SRC/zx.wasm" ]; then
    echo "缺 $SRC/zx.wasm（扩展宿主 wasm）。重建方法：" >&2
    echo "  cd editor/zed && cargo build --release --target wasm32-wasip2 && cp target/wasm32-wasip2/release/zx.wasm ." >&2
    exit 1
fi

mkdir -p "$INST/grammars" "$INST/languages/zx"
cp "$SRC/extension.toml" "$INST/"
cp "$SRC/zx.wasm" "$INST/extension.wasm"
cp "$SRC/grammars/zx.wasm" "$INST/grammars/"
cp "$SRC/languages/zx/config.toml" "$SRC/languages/zx/highlights.scm" \
   "$SRC/languages/zx/outline.scm" "$INST/languages/zx/"

echo "installed -> $INST"
find "$INST" -type f
