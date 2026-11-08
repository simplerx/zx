# zxc 使用说明

> 对应 **zxc 1.0.0**。本文只回答一个问题：**「我要做某件事，该敲哪条命令？」**
> 语法细节查 `docs/manual.md`，从零学语言看 `docs/guide.md`，想离线翻全文打开
> `docs/site/index.html`。命令行为以 `zxc -h` / `zxc help <命令>` 为准。

---

## 0. 一页速查

| 命令 | 联网 | 一句话 |
| --- | --- | --- |
| `zxc init [名]` | 否 | 建一个新工程 |
| `zxc run` | **否** | 编译 + 运行（日常就敲这一条） |
| `zxc build [选项]` | 否 | 只编译，产物在 `out/` |
| `zxc test [过滤]` | 否 | 跑 `src/test/` 里的用例 |
| `zxc check [文件...]` | 否 | 只做静态检查，不产出任何文件 |
| `zxc fmt [文件...]` | 否 | 格式化（纯空白改动） |
| `zxc sync` | 否 | 立刻扫 `vendor/`，补生成缺失的 C 绑定 |
| `zxc include <路径/配方>` | 否 | 接入**本机磁盘上**的 C 库 |
| `zxc add <url> [--force]` | **是** | 从网站拉 C 库进 `vendor/`，写一行 URL + 锁 |
| `zxc fetch [名\|URL]` | 仅缺库时 | 按 `zx.lock` 校验 / 补齐 `vendor/` |
| `zxc cbuild [组]` | 否 | 手动重编 `vendor/` 里的 C 源码（一般不用） |
| `zxc errors` / `zxc explain <码>` | 否 | 错误码列表 / 单个错误码的解释 |

**记住两条就够**：库已经在磁盘上 → 放进 `vendor/`，`zxc run`；库在网站上 →
`zxc add <url>` 一次，之后也永远是 `zxc run`。

---

## 1. 命令改名对照（最容易搞混的地方）

同一套能力在 1.0.0 里做了一次语义拆分，`add` 这个名字被"远端拉取"占用了：

| 以前敲的 | 现在敲 | 说明 |
| --- | --- | --- |
| `zxc add ./vendor/foo.h` | `zxc include ./vendor/foo.h` | 接入本地 C 库，能力**完全不变** |
| `zxc add ../third_party/foo` | `zxc include ../third_party/foo` | 目录模式、`--list`、`--name`、`--lib`、`--no-build`、`--force` 全保留 |
| `zxc add m`（内置配方） | `zxc include m` | 同上 |
| —— | `zxc add <url>` | **新命令**：从网站拉库；只接 URL，无可选参数 |
| —— | `zxc fetch` | **新命令**：换机器后按锁补齐 |
| `zxc sync` | 不变 | 扫 `vendor/` 补绑定 |
| `zxc cbuild` | 不变 | 显式重编 C 源码 |

一句话记忆：`include` = 把**不在 vendor 的 C 库包含进来**，`add` = 从**网站添加**。
`add` 只接 URL；给它本地路径会直接提示你去用 `zxc include`。

---

## 2. 五分钟：从零到跑起来

### 2.1 建工程

```sh
zxc init demo        # 建 demo/ 目录并铺好骨架
cd demo
# 或者在当前目录建：zxc init
```

生成：

```
demo/
  zx.json          工程清单：name / version / main / out / deps / vendor
  src/main.zx      入口（示例 greet + main）
  vendor/          外部模块根，存在即自动参与搜索；里面有 README.md
  README.md  .gitignore
```

`zx.json` 最小的样子：

```json
{
  "name": "demo",
  "version": "0.1.0",
  "main": "src/main.zx",
  "out": "demo"
}
```

`out` 决定产物名（`out/demo.exe`）。要额外搜索目录，加 `"deps": ["../shared"]`。

### 2.2 写第一个程序

```zx
const std = module("std");

const main = fn () i32 {
    std.println("hello, zx");
    return 0;
};
```

三个要点：函数是常量（`const main = fn () i32 { … };`）、语句末尾要分号、
`const` 只能绑定编译期常量，运行期值用 `let` / `var`。

### 2.3 构建运行

```sh
zxc run              # = build + 运行，日常用这条
zxc build            # 只编译 → out/demo.exe
zxc build --release  # -O2 发布档（默认 dev -O0，迭代快）
zxc check            # 只检查，适合做 pre-commit hook
```

不建工程也能编单个文件：

```sh
zxc hello.zx -o hello          # 生成 C 并编成可执行文件
zxc hello.zx --emit-c          # 只把生成的 C 打到 stdout
```

### 2.4 模块怎么找

搜索顺序：**导入文件所在目录 → 工程根的 `vendor/` → `zx.json` 的 `deps`**。

```zx
const m  = module("util");            // src/util.zx 或 vendor/util.zx
const s  = module("lib/util.zx");     // vendor/lib/util.zx
const st = module("std");             // 编译器自带标准库
```

`vendor/` 只要存在就自动生效，不用写进 `deps`。导出的东西要 `pub`。

---

## 3. 命令详解

### 3.1 工程与构建

| 命令 | 要点 |
| --- | --- |
| `zxc init [名]` | 建完会对 `src/main.zx` 自检一次，失败返回 1 |
| `zxc build` | 默认可执行文件；`--lib` 出 `.a`，`--lib --shared` 出 `.dll`/`.so`；`--release` 用 `-O2` |
| `zxc run [--release] [-- 参数...]` | `--` 之后的参数原样传给程序；退出码就是程序的退出码 |
| `zxc test [过滤] [--release]` | `src/test/*.zx` 里的**零参 `pub` 函数**即用例，每例独立进程；过滤是名字子串匹配 |
| `zxc check [文件...]` | 不给文件时先扫当前目录 `*.zx`，再回落 `src/`、`src/test/` |
| `zxc fmt [文件...] [--check]` | 4 空格缩进、去尾空格、合并空行、补末尾换行；CRLF 保持 CRLF |

中间文件在 `.zxc-cache/`，产物在 `out/`，两者都已在 `.gitignore` 里。
后端 C 的报错会被翻回 **zx 源码行列**；`ZX_VERBOSE=1 zxc build` 看完整 cc 命令行。

### 3.2 环境变量与退出码

| 变量 | 作用 |
| --- | --- |
| `ZX_CC` | 后端 C 编译器命令，缺省 `<zxc 目录>/../.toolchain/zig.exe cc` |
| `ZX_VERBOSE=1` | 打印构建细节、原始后端报错、缓存命中 |
| `ZX_ALLOW_LOCAL_FETCH=1` | 允许 `file://` 源（默认只允许 https，本地镜像可用 loopback http） |

退出码：`0` 成功，`1` 有诊断 / 构建失败，`2` 用法错误。脚本里判断"编得过吗"请只看退出码。

### 3.3 诊断

```sh
zxc errors                 # 按类别列出全部错误码
zxc explain E0301          # 单个错误码的含义与修法
zxc help add               # 任何子命令的详细帮助
```

---

## 4. C 库：三条路，怎么选

| 你的情况 | 用哪条 | 要不要敲命令 |
| --- | --- | --- |
| 库已经（或可以）放在 `vendor/` | **直接丢进去** | **不用敲，直接 `zxc run`** |
| 库在磁盘别处（工程外、系统里、不想复制） | `zxc include <路径>` | 一次，生成绑定和清单 |
| 库在网站上 / 要给别人复现 | `zxc add <url>` | 一次，写 `zx.json` + `zx.lock` |
| 换机器 / CI / 同事刚 clone | `zxc fetch` | 一次，按锁补齐 |

```
库在哪？
├─ 就在磁盘上 ──┬─ 能放进 vendor/ ──→ 拷进去，zxc run（最省事）
│               └─ 不想/不能搬 ────→ zxc include <路径>，之后 zxc run
└─ 在网站上 ─────────────────────→ zxc add <url>，之后 zxc run
                                    换机器：zxc fetch
```

三条路最终都汇到同一条链路：**生成普通 Zx 绑定 → 自动编译 C 源码 → 自动链接 →
`zxc run`**。生成物是可读可改的 `.zx` 文件，补全 / hover / 行内类型提示照常工作。

---

## 5. 路径一：直接丢进 `vendor/`（推荐）

```sh
cp -r ../third_party/foo vendor/foo   # include/ + 多层 src/ 原样搬入
# 或者更简单：把 foo.h / foo.c 直接放在 vendor/ 一层
zxc run                               # 完事
```

`run` / `build` / `test` 在解析源码**之前**扫一遍 `vendor/`，把还没接管的 C 库
自动补成"已接入"状态：

| 放法 | 自动生成 | 编译 |
| --- | --- | --- |
| `vendor/foo/`（含 `.h`，可带任意深度 `src/`） | `vendor/foo.clib` + `vendor/foo.zx` | 清单里的 `.c` → `vendor/libfoo.a` |
| `vendor/foo.h` + 可选 `vendor/foo.c` | `vendor/foo.zx` | 顶层 `.c` → `vendor/libfoo.a` |

三条保证：

- **不覆盖**：已有同名 `.clib` / `.zx` 一律不碰。手工裁剪清单、改函数签名都不会被冲掉；
  放一个同名占位 `.zx` 就是天然的 opt-out。
- **不阻断**：某个目录解析失败只打一行 `skipped` 警告并继续构建。
- **不半途落盘**：绑定文本生成成功后才一起写清单和绑定，不会留下"有清单没绑定"的残状态。

纯 Zx 模块目录（`vendor/mylib/*.zx`，没有 `.h`）不会被误判。

**编辑器里的先后问题**：绑定文件要先存在，LSP 才会在补全里列出 `module("foo")`。
写第一行代码前跑一次 `zxc sync`（只扫 `vendor/`、不编译、不联网），补全立刻可用。

**模块必须显式绑定**：`glfw3.glfwInit()` 这种裸模块名不是合法引用，会报
unknown identifier。在行首输入 `glfw` 会出「导入模块」补全项，Tab 展开成
`const glfw3 = module("glfw3");`；光标停在绑定名上，直接输入 `g` 就是缩写
`const g = module("glfw3");`，再导出则写 `pub const math = module("util");`。
名字只由源码里的绑定产生，不会因模块文件变多而撞名。

编辑器还有**骨架补全**：`if` / `fn` / `type` 等关键字按 Tab 展开整条语句；
表达式位置输入本文件声明的类型名出 `Name{ .字段 = … }` 实例骨架，输入
函数名出带形参占位的调用骨架，Tab 在占位符间依次跳转填空。

---

## 6. 路径二：`zxc include`（库在磁盘别处）

```sh
zxc include ../third_party/raylib          # 推荐：指目录，库文件原地不动
zxc include ./vendor/raylib.h              # 单个头文件（小库 / 单头库）
zxc include m                              # 内置常见库配方，自动定位头 + 配好 @link
zxc include --list                         # 看有哪些配方
```

内置配方：`c` / `libc` / `m` / `ws2_32` / `user32` / `gdi32` / `kernel32` /
`opengl32` / `sqlite3` / `z` / `zlib` / `raylib` / `sdl2`。

| 选项 | 作用 |
| --- | --- |
| `--lib NAME` | 覆盖 `@link` 用的库名 / 路径 |
| `--name NAME` | 目录模式下覆盖模块名 |
| `--no-build` | 只生成绑定，先不编译 C 源码 |
| `--force` | 覆盖已有的 `vendor/<名>.zx` |
| `--list` | 列出内置配方 |

**目录模式**会扫库自己的树（`include/`、`src/`、任意多层），写出
`vendor/<名>.clib` 清单（哪些头、哪些源文件），再把这些 `.c` 编成一个
`vendor/lib<名>.a`。**不复制任何库文件**，库可以放在任何地方。清单是可编辑的
编译输入——平台互斥的源文件、不想编的测试代码，注释掉对应 `source =` 行即可。

生成物示例：

```zx
// vendor/foo.zx —— 普通 Zx 源码
@link("vendor/libfoo.a");
extern "C" {
    pub fn foo_add(a: i32, b: i32) i32;
}
pub const FOO_MAGIC: i32 = 7;
```

应用侧：

```zx
const foo = module("foo");
std.println("{}", foo.foo_add(2, 3));
```

clang 翻不了的声明会列在文件末尾的注释里，按需手写补上。`include` **不联网**，
不下载任何东西。改了 C 源码后 `zxc build` 会自动增量重编并重链，不用手动 `cbuild`。

---

## 7. 路径三：`zxc add` / `zxc fetch`（库在网站上）

```sh
zxc add https://example.com/stb_image.h     # 单头库
zxc add https://example.com/foo.tar.gz      # 归档
zxc add https://gitcode.com/some/foo.git    # git 仓库
zxc run                                    # 之后完全离线
```

**没有可选参数**。`--name` / `--ref` / `--sha256` / `--subdir` / `--git` 这一组
已全部删掉，唯一还能带的是 `--force`（重新锁定同一个源）。源类型由 URL 自己
说明：`.git` 结尾是 git 仓库，`.tar` / `.tar.gz` / `.tgz` / `.tar.xz` / `.zip`
是归档（自动下钻唯一顶层目录），其余按单文件处理。

### 两层文件都要提交

| 文件 | 谁写 | 内容 |
| --- | --- | --- |
| `zx.json` 的 `"vendor"` | 人 / `zxc add` | 一行一个 URL 的字符串数组，和 `deps` 同构 |
| `zx.lock` | `zxc` | 全部安全锚点：下载字节 sha256、git commit、文件树 hash、落地路径、文件数 |

```json
{
  "name": "snake",
  "version": "0.1.0",
  "main": "src/main.zx",
  "out": "snake",
  "deps": [],
  "vendor": [
    "https://example.com/stb_image.h",
    "https://gitcode.com/some/foo.git"
  ]
}
```

清单允许的顶层字段就这六个，`vendor` 里没有 `ref` / `sha256` / `subdir`。原因是
tag 会移动、URL 指向的字节会变，所以**安全不能靠人填参数**：`zxc add` 首次拉取
时把每个校验值自动算出来钉进 `zx.lock`，`zxc fetch` 在另一台机器上逐项复现。
diff 里一眼能看出「想换哪个源」，锁里一眼能看出「实际锁了什么」。

```sh
zxc fetch                 # 检查全部（主路径，通常就敲这一条）
zxc fetch stb_image       # 只查一个：这里的 stb_image 是库名
zxc fetch "https://..."   # 也可以直接给 URL
zxc fetch --force         # 强制重拉 / 恢复被改过的本地副本
```

> **`zxc fetch <name>` 里的 name 是什么？** 是**库名，不是地址**。它由 URL 尾段
> 推出来（`.../stb_image.h` → `stb_image`、`.../foo.git` → `foo`），同时是
> `vendor/` 下的落地名和 `zx.lock` 里的 `[stb_image]`。地址只写在 `zx.json` 与
> `zx.lock` 里。拿不准就敲不带参数的 `zxc fetch`，或者直接把 URL 贴进去。

安全边界：只接受 `https://`（本地镜像可用 loopback `http://`，`file://` 需
`ZX_ALLOW_LOCAL_FETCH=1`）；重拉必须复现锁里的 sha256 / commit / tree hash；
`zx.json` 里的 URL 被改过就拒绝换源（要 `zxc add <url> --force` 重新锁定）；
`zxc fetch` 只复现 `zx.lock`：声明里出现未锁定的新 URL 一律拒绝并指路
`zxc add <url>`，不会替你下载。不执行库里的构建脚本，不做版本求解 /
registry / 传递依赖。

> **换机器流程**：`git clone` → `zxc fetch` → `zxc run`。`run` 从头到尾不联网。

---

## 8. 离线边界（重要）

| 命令 | 会不会联网 |
| --- | --- |
| `run` / `build` / `test` | **绝不**。只检查 `zx.lock` 登记的落地路径在不在，缺了打印提示让你跑 `zxc fetch` |
| `check` | 不会，而且保持纯只读，不产出任何文件 |
| `sync` / `include` / `cbuild` / `fmt` / `errors` / `explain` | 不会 |
| `add` | 会（这是它存在的意义） |
| `fetch` | **只在库缺失 / 被改且 `--force` 时**；完好的库纯本地校验 |

构建要快、要能离线、要可复现，所以网络行为只集中在两条显式命令里。

---

## 9. 常见故障排查

| 现象 | 原因 | 怎么办 |
| --- | --- | --- |
| `module 'xxx' failed to parse`、`LSP 里没有 module("foo")` | 绑定文件还没生成 | `zxc sync`（或直接 `zxc build`，它也会自动生成） |
| `E0101 lexical error`，生成物里有 `E:\...` | 旧版 `include` 把 Windows 反斜杠写进了 `.zx` 字符串 | 重新 `zxc include <路径> --force`（1.0.0 已修，路径统一正斜杠） |
| `E0401 unknown type 'GLFWwindow'` | C 头里的不完整句柄没有定义 | 新版 `include` 会自动补占位类型；旧的生成物可手工加 `pub const X = struct { _opaque: u8 };` |
| `E0302 integer literal ... does not fit in i32` | `0x80000000` 这类无符号宏 | 新版 `include` 自动改用 `u32`；手工改成 `u32` 即可 |
| `B2002 could not open 'libuuid.a' / 'libMSVCRT.a'` | 用了 **MSVC 工具链编的 `.lib`**，配 zig cc(mingw) 的 `/DEFAULTLIB` 对不上 | 换同 ABI 的 `lib<名>.a`；别混用 MSVC 与 mingw 静态库 |
| `B2001 undefined symbol: ... CreateDCW / ChoosePixelFormat` | C 库还依赖系统库 | 在源码里补 `@link("gdi32")` / `@link("user32")` / `@link("opengl32")` 等 |
| 链接期缺 `__security_cookie` / `_fltused` | 同上，MSVC CRT 专有符号 | 换 mingw/COFF 版库，不要试图补 stub |
| `zxc fetch: ... URL in zx.json changed` | 有人改了源地址但没重新锁定 | `zxc add <url> --force` 显式重新锁定 |
| `zx.json: "vendor" must be a URL list` | 还在用旧版「每库一个对象」的写法 | 改成 `"vendor": ["https://..."]` 数组 |
| `zxc fetch: ... not in zx.lock` | 清单里写了没锁过的新 URL | 先 `zxc add <url>` 显式钉住 |
| `zxc fetch: content differs from zx.lock` | 本地库被改过 | 确认后 `zxc fetch --force`（旧目录会备份） |
| `vendor/` 里某个库一直没被接管 | 存在同名 `.zx` / `.clib`（可能是占位 opt-out） | 检查同名文件，删掉或按需编辑 |
| `zxc add ./vendor/foo.h` 报"预期 https URL" | `add` 只接 URL | 本地路径用 `zxc include ./vendor/foo.h` |
| 生成物里少了某个函数 | clang 翻不了（回调 / 变参 / 复杂宏），列在文件末尾注释里 | 照末尾提示手写补声明；回调类可改用轮询式 API |

一个真实案例（glfw + OpenGL 贪吃蛇）见 `E:\code\zed\zx\snake\README.md`，
里面完整记录了选库、补系统库、以及上面几个错误的处理过程。

---

## 10. 速查卡

```sh
# 日常
zxc init demo && cd demo
zxc run                         # 编译 + 跑
zxc build --release             # 发布档
zxc test                        # 跑测试
zxc check                       # 静态检查

# C 库在磁盘上
cp -r ../third_party/foo vendor/foo   # 最省事：什么都不用敲
zxc run
zxc include ../third_party/foo        # 或者：库不搬，指过去
zxc include --list                    # 内置配方

# C 库在网站上
zxc add https://example.com/lib.h
zxc fetch                            # 换机器后
zxc run                              # 之后永远离线

# 编辑器 / 排查
zxc sync                            # 让补全立刻认识新库
zxc errors
zxc explain E0301
ZX_VERBOSE=1 zxc build              # 看完整编译链接命令
```
