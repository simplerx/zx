# Zx

Zx 是一门**自举的系统级编程语言**：前端与标准库用 Zx 自身编写，编译器后端把 Zx
翻译成 C，再用项目内 vendored 的 zig `cc`（clang + 自带 libc）编出原生可执行文件。

设计追求简约，
错误用返回值类型`!T` / `?T` 显式表达，
并发只有一个 `Task` 实例抽象。

- **错误处理**：`!T`（错误联合）与 `?T`（可选）是值类型，`try` / 捕获 / `orelse` / `switch` 四种出口
- **泛型**：编译期单态化；常量与泛型函数在编译期求值
- **模块**：`module("std")` 显式导入，标准库是普通 Zx 代码，可读可改
- **FFI**：`extern "C"` 块直接声明 C 函数/变量/结构体，`@link` 挂系统库
- **内存**：手动管理 + 自建 arena；无 GC、无引用计数
- **诊断**：报错带源码行 + `^` 指位，带 `did you mean` 修法，错误码可查 `docs/errors.md`

手册：`docs/manual.md`　·　规格：`spec/zx-spec.md`　·　文法：`spec/zx.ebnf`　·
设计裁定：`spec/decisions.md`　·　仓库：<https://gitcode.com/simplex/zx>

---

## 构建与测试

```sh
sh build.sh          # → bin/zxc.exe + bin/zxfe.exe + bin/zxls.exe
sh test.sh           # 回归（语法 + 语义 + 反例 + 运行 + 多文件 + 增量 + CLI + LSP）
```

工具链是项目内 vendored 的 zig（`.toolchain/zig.exe`），不依赖系统 gcc/clang。
首次 clone 时 `.toolchain/` 为空（约 360 MB，不入库）：把 **zig 0.17.0** 解到
`.toolchain/zig.exe`，或用 `ZIG=zig sh build.sh` 走 PATH 上的 zig。

## 上手 demo

`hello.zx`：

```zx
const std = module("std");

const main = fn () !i32 {
    std.println("hello, {}", "zx");
    return 0;
};
```

```sh
bin/zxc.exe hello.zx -o hello.exe
./hello.exe
```

变量用 `let`（不可变）/ `var`（可变）；函数用 `const name = fn (args) ret { ... }`；
`!i32` 表示「返回 `i32` 或错误」。

语言速览：

```zx
const std = module("std");

// 错误联合：可能带回错误
const mayFail = fn (x: usize) !usize {
    if (x > 100) { return error.too_big; }
    return x * 2;
};

const main = fn () !i32 {
    let r = try mayFail(10);        // try：出错就沿 !T 传播链冒出去

    if (mayFail(200)) v {           // 捕获：成功臂拿值，错误臂拿错误码
        std.println("ok:{}", v);
    } else e {
        std.println("err:{}", std.debug.errName(e));
    }

    let o = mayFail(5).?;           // .? 解包，空/错则传播
    _ = o;
    return 0;
};
```

结构体、枚举、并发、FFI 的完整示例见 `docs/manual.md` 与 `demo/`。

## 常用命令

```sh
bin/zxc.exe file.zx               # 静态检查（默认）
bin/zxc.exe file.zx -o prog       # 编成可执行文件
bin/zxc.exe file.zx --emit-c      # 只吐生成的 C
bin/zxc.exe --check file.zx       # 只检查不编译
bin/zxc.exe -v                    # 版本

bin/zxc.exe init [name]           # 创建工程：src/、vendor/、zx.json
bin/zxc.exe build [--release]     # 工程构建 → out/<名>.exe
bin/zxc.exe run [-- args...]      # 构建后直接运行
bin/zxc.exe fmt [--check]         # 格式化
bin/zxc.exe include <name|header.h|dir> # 接入本地/工程外 C 库 → vendor/<name>.zx
bin/zxc.exe add <url>             # 从网站拉 C 库进 vendor/（写 zx.json + zx.lock）
bin/zxc.exe fetch [name]          # 按 zx.lock 校验 / 补齐 vendor/
bin/zxc.exe sync                  # 扫 vendor/ 补生成缺失的 C 绑定
bin/zxc.exe cbuild [group]        # 重编 vendor/ 下的 C 源码（一般不用手动跑）
```

## 调用 C 库

**最省事的用法：把库整个丢进 `vendor/`，然后照常 `zxc run`。** 不用手写 `extern`、
不用敲接入命令、也不用把 `include/` 和 `src/` 摊平复制。`zxc run` / `build` / `test`
会在解析源码之前扫一遍 `vendor/`，自动为新出现的 C 库生成 `vendor/<名>.zx` 绑定和
`vendor/<名>.clib` 清单，再用同一套编译器把 `.c` 编成静态库并链接好：

```sh
cp -r ../third_party/raylib vendor/   # 或者把 foo.h + foo.c 直接放在 vendor/ 一层
zxc run                               # 完成；源码里 const ray = module("raylib");
```

只有第一次会生成，之后这两个文件就是普通、可编辑的工程文件，建议进版本库。
已有同名 `.zx` / `.clib` 的库一律不动（可以手工裁剪清单、改绑定，也能用同名占位
文件关掉自动接管）。想在写第一行 `module("名字")` 之前就让编辑器有补全，先跑一次
`zxc sync`。

需要指向工程外的库、指定模块名或刷新清单时，用 `zxc include`：

```sh
zxc include ../third_party/raylib  # 推荐：库目录（include/、src/、任意多层，工程外也行）
zxc include ./vendor/raylib.h      # 单个头文件（小库简写）
zxc include m                      # 内置常见库配方：自动定位头文件并配好 @link
zxc include --list                 # 看有哪些配方
```

目录模式下 `zxc` 递归扫描整个库目录，写出一张 `vendor/<名>.clib` 清单，列明发现了哪些
头文件、源文件、include 路径，然后把清单里的 `.c` 编成 `vendor/lib<名>.a` 并写好 `@link`。
**库文件一个都不动**；多选了就在清单里注释掉那行，库里新增文件就 `zxc include <目录> --force`
刷新。之后改了任意 C 源码，直接 `zxc build` 会自动增量重编、重新链接，不用记额外命令。

```zx
const ray = module("raylib");
ray.InitWindow(800, 600, "hello");
```

生成物是普通 Zx 源码，`vendor/` 本来就是模块搜索根，所以补全、hover、行内类型提示
全部照常生效；要微调直接编辑，`--force` 可重新生成。整套本地流程不联网、不下载源码。
### 从网站拉库：`zxc add` / `zxc fetch`

库在远端时用 `zxc add`。拉完直接落到 `vendor/`，后续仍走上面那套自动接管：

```sh
zxc add https://raw.githubusercontent.com/nothings/stb/master/stb_image.h
zxc add https://gitcode.com/some/foo.git --ref v1.2.3 --name foo
zxc run                               # 之后完全离线
```

支持三种源：单个文件（单头库最常见）、`.tar.gz` / `.tgz` / `.zip` 归档、git 仓库。
`--subdir` 只取归档或仓库里的某一层，`--sha256` 可以在下载前就钉死期望的字节。

**两层文件都要提交**：

| 文件 | 谁写 | 内容 |
| --- | --- | --- |
| `zx.json` 的 `"vendor"` 段 | 人 | 意图：URL / git ref / 期望 sha256 / 子目录 |
| `zx.lock` | `zxc` | 结果：git 实际 commit、落地文件树 hash、落地路径、文件数 |

| 命令 | 联网 | 作用 |
| --- | --- | --- |
| `zxc add <url>` | 是 | 拉取 → 校验 → 落地 `vendor/` → 写声明与锁 |
| `zxc fetch [名]` | 仅缺库时 | 按锁校验 `vendor/`；缺失就重拉，内容对不上就报错 |
| `zxc run` / `build` / `test` | **否** | 只给本地缺库提示，绝不下载 |

安全边界：只接受 `https://`（本地镜像可用 loopback `http://`）；下载字节算 SHA-256，
git tag 会被解析成不可变 commit 写进锁。上游 tag 被改写、内容漂移、或 `zx.json`
与 `zx.lock` 不一致，一律拒绝而不是悄悄更新。不执行库里的任何构建脚本，不做依赖
求解、不做 registry；下载复用系统 `curl` / `git` / `tar`，不自己实现协议栈。

换源或升级用 `zxc add <url> --force` 显式重新锁定；本地已有的库用
`zxc include <path>`，`add` 只接 URL。

细节与边界见 `docs/manual.md` §3.17 / §3.19 / §3.20 / §3.21。

编辑器支持（Zed 扩展 + 通用 LSP 客户端，`bin/zxls.exe`）见 `editor/README.md`。

## 目录

| 目录 | 内容 |
| --- | --- |
| `src/` | 编译器 C 实现：`parse.c`（前端）、`sema.c`（类型/语义）、`backend.c`（C 发射）等 |
| `bootstrap/` | 自举前端（Zx 代码）：`parse.zx` / `lex.zx`，编译成 `bin/zxfe.exe`，zxc 默认走它 |
| `std/` | 标准库：`str/map/fs/json/time/pool/task.zx` + 聚合 `std.zx` |
| `docs/` | `manual.md` 语言手册、`guide.md` 教程、`errors.md` 错误码参考 |
| `spec/` | `zx-spec.md` 规格、`zx.ebnf` 文法、`decisions.md` 设计裁定记录 |
| `tests/` | `good/`（正例）+ `bad/`（反例）+ `multi/`（多文件） |
| `demo/` | `ffi/`：Windows API + OpenGL 例子（`extern "C"` 绑定的真实用户） |
| `editor/` | Zed 扩展、tree-sitter 语法、语言服务器 `tools/zxls.zig` |
