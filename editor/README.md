# 编辑器支持：语法高亮与自动补全

这个目录是 Zx 的**编辑器侧全部资产**，与编译器（`src/`、`bin/zxc.exe`）分开：
换编辑器不用动编译器，改语法也不用碰 C 代码。一套东西两个出口——

| 能力 | 提供者 | 依赖 |
| --- | --- | --- |
| 语法高亮、括号匹配、代码折叠、大纲 | tree-sitter 语法 + `highlights.scm` / `outline.scm` | 无（纯静态 wasm / 查询文件） |
| 补全、诊断、跳转、hover、签名、格式化、引用 | `zxls`（LSP over stdio） | `bin/zxls[.exe]` + `bin/zxc.exe` + `bin/zxfe.exe` |

**高亮不需要编译器**；补全和诊断是语言服务器转手问编译器，所以必须先
`sh build.sh`。两者可以独立工作——高亮正常但补全没有，问题一定在 LSP 侧。

```
editor/
├── zed/                       Zed 扩展（可直接安装）
│   ├── extension.toml         清单：语言 id、语法、语言服务器接线
│   ├── zx.wasm                扩展宿主 wasm（由 src/lib.rs 编出）
│   ├── grammars/zx.wasm       tree-sitter 语法 wasm（由 ../tree-sitter-zx 编出）
│   ├── languages/zx/
│   │   ├── config.toml        语言名、后缀、注释符、括号配对
│   │   ├── highlights.scm     ★ 高亮规则的唯一副本
│   │   └── outline.scm        大纲（符号树）
│   ├── src/lib.rs             把 <worktree>/bin/zxls[.exe] 注册为语言服务器
│   └── install.sh             一键装到 Zed 的扩展目录
└── tree-sitter-zx/            语法源码（可复用到任何 tree-sitter 宿主）
    ├── grammar.js             语法定义（手写）
    ├── src/parser.c           生成的解析器（tree-sitter generate）
    ├── src/node-types.json    节点类型（写查询时的字典）
    └── build-wasm.sh          重编 editor/zed/grammars/zx.wasm
```

`highlights.scm` 只有一份，放在 Zed 扩展里——那是**被消费的位置**。别的编辑器
要查询文件就从这里拷，别在 `tree-sitter-zx/` 再维护第二份（两份一定漂移）。

## Zed：安装与更新

```sh
sh build.sh                    # 1. 先有三件套：bin/zxc bin/zxfe bin/zxls
sh editor/zed/install.sh       # 2. 装扩展（拷 6 个文件到 installed/zx）
```

然后在 Zed 里重开窗口，或执行 `editor: restart language server`。

- 装到哪：`%LOCALAPPDATA%\Zed\extensions\installed\zx`（macOS/Linux 走
  `$HOME/.local/share/Zed/...`，`install.sh` 已按 `LOCALAPPDATA` 回退）。
- 六个文件缺一不可：`extension.toml`、`extension.wasm`、`grammars/zx.wasm`、
  `languages/zx/{config,highlights,outline}`。Zed 的"卸载扩展"会删掉整个目录。
- **不要在 `.zed/settings.json` 里写 `lsp.zx-lsp.binary.path`**。扩展已经返回
  `<worktree>/bin/zxls[.exe]`；写死绝对路径后换个 checkout 位置就指向不存在的
  文件，Zed 不会报错，只是补全和诊断静默消失（症状与"语言服务器没启动"完全
  一样）。仓库里的 `.zed/settings.json` 因此只留语言绑定。
- 扩展是"仓库内二进制"模型，不联网、不下载：升级 Zx 后重新 `build.sh` 即可，
  不用重装扩展（`install.sh` 只在 wasm 或查询文件改动时才需要重跑）。

## 其它编辑器

语法与查询是标准的 tree-sitter 资产，`editor/tree-sitter-zx/` 可以直接被
nvim-treesitter / Helix / Emacs 的 tree-sitter 集成引用。

**Neovim**

```lua
-- 语法：把 editor/tree-sitter-zx 加进 parser 搜索路径后 require("nvim-treesitter.parsers").zx
-- 查询：cp editor/zed/languages/zx/highlights.scm <rtp>/queries/zx/highlights.scm
--       （outline.scm 同理，给 aerial/outline 类插件用）
require("lspconfig").zxls.setup({
  cmd = { "/path/to/zx/bin/zxls" },   -- Windows: zxls.exe
  filetypes = { "zx" },
  root_dir = require("lspconfig.util").root_pattern("zx.json", ".git"),
})
```

**Helix**：`languages.toml` 里给 `zx` 配 `grammar = "zx"`、
`language-servers = ["zxls"]`，`[language-server.zxls]` 的 `command` 指向
`bin/zxls`。语法仓库指向本仓库的 `editor/tree-sitter-zx` 子目录。

**VS Code**：还没有官方 client。ZX 的补全/诊断只走 LSP，任何通用 stdio LSP
客户端把 `command` 指到 `bin/zxls[.exe]` 就能用；高亮需要一份 TextMate 语法
（尚未提供），在补上之前 VS Code 里只有纯文本。

## 重新生成

```sh
# 语法 wasm（改了 grammar.js 或 parser.c 之后）
sh editor/tree-sitter-zx/build-wasm.sh

# 扩展宿主 wasm（改了 src/lib.rs 之后）
cd editor/zed && cargo build --release --target wasm32-wasip2
cp target/wasm32-wasip2/release/zx.wasm . && rm -rf target
```

改了 `grammar.js` 要先重生成解析器（需要 Node + tree-sitter-cli）：

```sh
cd editor/tree-sitter-zx && npx tree-sitter generate
```

`build-wasm.sh` 用仓库自带的 `.toolchain/zig.exe` 编 wasm（`zig cc` 就是
clang + wasm lld），不需要装 wasi-sdk。产物必须保持"共享内存 + `_initialize`
入口初始化"这套 wasi reactor 模型——换成 freestanding 静态内存的编法，Zed
装载后一解析文件就崩（脚本头部有说明）。

## 排障

| 现象 | 先看这里 |
| --- | --- |
| 完全没高亮 | 扩展没装/没启用；`installed/zx` 下六个文件是否齐 |
| 高亮正常，补全空 | `bin/zxls` 是否存在、能否执行；`zxls` 同目录的 `zxls-lsp.log` 有没有 `boot` 行 |
| LSP 不启动、日志也没有 | `.zed/settings.json` 是否残留 `lsp.zx-lsp.binary.path` 绝对路径覆盖 |
| 补全能出、诊断说 `cannot open module` | 改过 `zx.json` 的 `deps`——搜索路径只在 initialize 时算一次，重启语言服务器 |
| 模块能编过但补不出 | 模块放在 `vendor/` 或 `deps` 里？解析链见 `spec/decisions.md` §80（编译器与 LSP 必须查同一批目录） |
| Windows 上 `build.sh` 说 zxls 被占用 | Zed 正跑着旧 exe。`mv bin/zxls.exe bin/zxls.old.exe` 再编，编完删掉 |
| 编辑器里诊断为空但命令行有 | Windows 上 zxls 拉起 zxc 必须经 `cmd /c` 中转，否则孙进程 stderr 句柄断链——已在 zxls 里处理，改 spawn 逻辑时别退回直连 |

语言服务器在 `bin/` 旁边写 `zxls-lsp.log`（boot / didOpen / publish 各一行）。
"没有反应"类问题先看它：有 boot 说明进程起来了，没有就是编辑器根本没拉起。
