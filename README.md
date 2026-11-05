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
```

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
