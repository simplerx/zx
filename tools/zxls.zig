// zxls.zig — Zx 语言服务器（LSP over stdio）。
//
// 设计：
//   - 诊断：didOpen/didChange 后防抖，spawn 兄弟目录的 `zxc <file> --check`，
//     解析 stderr 的 `path:line:col: error[CODE]: msg` 行 → publishDiagnostics。
//     直接复用自举前端（zxfe），诊断与编译器 100% 同源。
//   - 补全：
//       `@` 前缀      → 顶层内建（@type/@memcpy/@trap/… 共七个）
//       `X.`          → std 内建命名空间 / module 绑定的顶层 pub 声明 /
//                       容器（struct/union/enum）的成员与方法（含实例推断：
//                       `let v = Obj.init(...)` 后 `v.` 出 Obj 的成员）
//       模块文件按**编译器的解析链**找：文件所在目录 → 工程根 → 工程根的
//       vendor/ → zx.json 的 deps（顺序同 setup_search_paths，见 modDirsInto）
//       其余          → 关键字 + 当前文件标识符 + 工作区标识符（TTL 缓存）
//   - 定义跳转 / hover / 签名提示：声明语句文本扫描 + 类型初值推断。
//   - 格式化：影子文件 → `zxc fmt` 原地格式化 → 读回 → 全文 TextEdit。
//   - 大纲：第 0 列顶层声明的 DocumentSymbol。
//   - 引用：当前文档 + 工作区文件词边界扫描（文本级，截 200 条）。
//   - 工作区标识符从 root 目录（含子目录、≤2 层）的 .zx 文件扫描。
//
// 构建：zig 0.17.0 build-exe tools/zxls.zig -O ReleaseSafe -femit-bin=bin/zxls.exe
const std = @import("std");
const Io = std.Io;

const gpa = std.heap.smp_allocator;

var g_threaded: Io.Threaded = undefined;
var g_io: Io = undefined;

// ---------------------------------------------------------------- json write

const Json = struct {
    buf: std.ArrayList(u8) = .empty,

    fn raw(j: *Json, comptime fmt: []const u8, args: anytype) void {
        j.buf.print(gpa, fmt, args) catch @panic("oom");
    }
    fn str(j: *Json, s_in: []const u8) void {
        // 出站清洗：宿主（Zed）读到非法 UTF-8 会直接杀掉 server——
        // 无论来源编码（GBK 诊断文本、磁盘旧文件），一律替换为 U+FFFD。
        const s = sanitizeUtf8(s_in);
        j.buf.append(gpa, '"') catch @panic("oom");
        for (s) |c| switch (c) {
            '"' => j.buf.appendSlice(gpa, "\\\"") catch @panic("oom"),
            '\\' => j.buf.appendSlice(gpa, "\\\\") catch @panic("oom"),
            '\n' => j.buf.appendSlice(gpa, "\\n") catch @panic("oom"),
            '\r' => j.buf.appendSlice(gpa, "\\r") catch @panic("oom"),
            '\t' => j.buf.appendSlice(gpa, "\\t") catch @panic("oom"),
            else => {
                if (c < 0x20) {
                    j.raw("\\u{x:0>4}", .{c});
                } else j.buf.append(gpa, c) catch @panic("oom");
            },
        };
        j.buf.append(gpa, '"') catch @panic("oom");
    }
};

// ---------------------------------------------------------------- state

const Document = struct {
    uri: []const u8,
    path: []const u8,
    text: []const u8,
    version: i64,
};

var docs_mu: Io.Mutex = .init;
var docs_cond: Io.Condition = .init;
var docs: std.StringHashMapUnmanaged(Document) = .empty;
var dirty: std.StringHashMapUnmanaged(void) = .empty;
var quitting = false;

var root_path: ?[]const u8 = null;
var zxc_path: []const u8 = "zxc.exe";

// std 目录：与编译器 find_std_path（src/sema.c）同一条解析链——
// <exe>/std → <exe>/../std（编译器还认 ZX_STD 与 cwd/std，LSP 这里
// 只实现 exe 相对两档：zxls 与 zxc 同源同目录，真实部署必中第二档）。
// 仓库外工程（如 hello-zx）的文档目录里没有 std.zx，缺了它
// std. / std.Task. 的补全整个断掉。
var g_std_dir: ?[]const u8 = null;

/// <dir> 下存在 std.zx 或 std/std.zx 即算 std 目录。
fn stdDirProbe(dir: []const u8) bool {
    var p: std.ArrayList(u8) = .empty;
    p.appendSlice(gpa, dir) catch return false;
    p.appendSlice(gpa, "/std.zx") catch return false;
    if (readFileSmall(p.items)) |_| return true;
    p.items.len = dir.len;
    p.appendSlice(gpa, "/std/std.zx") catch return false;
    if (readFileSmall(p.items)) |_| return true;
    p.deinit(gpa);
    return false;
}

fn findStdDir() ?[]const u8 {
    const ed = exeDir();
    {
        var p: std.ArrayList(u8) = .empty;
        p.appendSlice(gpa, ed) catch return null;
        p.appendSlice(gpa, "/std") catch return null;
        if (stdDirProbe(p.items)) return p.items;
    }
    {
        var p: std.ArrayList(u8) = .empty;
        p.appendSlice(gpa, ed) catch return null;
        p.appendSlice(gpa, "/../std") catch return null;
        if (stdDirProbe(p.items)) return p.items;
    }
    return null;
}

/// 错误码说明文档的地址（`<base>#e0304` 直达一条）。与 src/errors.c 的
/// ZX_DOCS_URL 是同一个字面量——C 常量传不到 Zig 这边，只能各存一份，
/// test.sh 会校验两处一致，改地址记得两边一起改。
///
/// 只指向错误码参考；手册与离线文档站已删（std 还没定型）。
const docs_base = "https://gitcode.com/simplex/zx/blob/main/docs/errors.md";

/// 客户端有没有在 initialize 里声明 codeDescriptionSupport。
/// 没有就别发 codeDescription（见 publishDiags 里的注释）。
var cap_code_desc = false;

var out_mu: Io.Mutex = .init;
var g_out_buf: [8192]u8 = undefined;
var g_out_writer: Io.File.Writer = undefined;
var g_in_buf: [16384]u8 = undefined;
var g_in_reader: Io.File.Reader = undefined;

// ---------------------------------------------------------------- tables

const keywords = [_][]const u8{
    "const",  "let",    "var",  "fn",     "module", "type",   "null",     "undefined",
    "if",     "else",   "while", "for",  "inline", "switch", "continue", "return",
    "defer",  "errdefer", "struct", "union", "enum", "error",  "pub",    "extern",   "orelse",
    "try",
};

const Snip = struct { label: []const u8, body: []const u8, detail: []const u8 };

/// KEEP — 常用代码片段（LSP snippet，insertTextFormat=2）。用户 2026-11-08
/// 要求：写 if / fn 按 Tab 展开固定骨架，块尾带 `;`，块内换行缩进 4 空格。
/// body 里的 $1/${1:默认值}/$0 是 LSP tabstop：确认后光标落在 $1，
/// 再按 Tab 依次跳到 $2…$0。缩进写死 4 空格（Zed tab_size=4，且
/// 不设 insertTextMode 以免客户端二次加缩进变成 8 空格）。
/// 每条模板展开后的形态都过一遍 zxc 编译；lsptest.py 的
/// completion.snippet.* 用例把 body 内容钉住，改坏会连挂。
/// 关键字同名项由这段替换普通关键字补全；其余是额外的常用模板。
const snippets = [_]Snip{
    .{ .label = "if", .body = "if (${1:cond}) {\n    $0\n};", .detail = "代码片段 if" },
    .{ .label = "ifelse", .body = "if (${1:cond}) {\n    $2\n} else {\n    $0\n};", .detail = "代码片段 if/else" },
    .{ .label = "else", .body = "else {\n    $0\n};", .detail = "代码片段 else" },
    .{ .label = "while", .body = "while (${1:cond}) {\n    $0\n};", .detail = "代码片段 while" },
    .{ .label = "for", .body = "for (${1:xs}) ${2:x} {\n    $0\n};", .detail = "代码片段 for" },
    .{ .label = "inline", .body = "inline for (${1:0..n}) ${2:i} {\n    $0\n};", .detail = "代码片段 inline for" },
    .{ .label = "switch", .body = "switch (${1:x}) {\n    ${2:0}: $3;\n    else: $0;\n};", .detail = "代码片段 switch" },
    .{ .label = "fn", .body = "fn ($1) ${2:i32} {\n    $0\n};", .detail = "代码片段 fn" },
    .{ .label = "struct", .body = "struct {\n    ${1:name}: ${2:i32},\n};", .detail = "代码片段 struct" },
    .{ .label = "union", .body = "union {\n    ${1:name}: ${2:i32},\n};", .detail = "代码片段 union" },
    .{ .label = "enum", .body = "enum { ${1:a}, ${2:b} };", .detail = "代码片段 enum" },
    .{ .label = "type", .body = "const ${1:Name} = struct {\n    ${2:field}: ${3:i32},\n};", .detail = "自定义类型 struct" },
    .{ .label = "uniontag", .body = "const ${1:Name} = union(enum) {\n    ${2:a}: ${3:i32},\n};", .detail = "自定义类型 union(enum)" },
    .{ .label = "enumtag", .body = "const ${1:Name} = enum(u8) { ${2:a}, ${3:b} };", .detail = "自定义类型 enum(u8)" },
    .{ .label = "new", .body = "const ${1:v} = ${2:Name}{ .${3:field} = $0 };", .detail = "按类型构造实例" },
    .{ .label = "anon", .body = ".{ .${1:field} = ${2:0} }$0", .detail = "匿名 struct 字面量" },
    .{ .label = "error", .body = "error { ${1:open}, ${2:read} };", .detail = "代码片段 error" },
    .{ .label = "const", .body = "const ${1:name} = $0;", .detail = "代码片段 const" },
    .{ .label = "let", .body = "let ${1:name} = $0;", .detail = "代码片段 let" },
    .{ .label = "var", .body = "var ${1:name} = $0;", .detail = "代码片段 var" },
    .{ .label = "module", .body = "module(\"${1:std}\")", .detail = "代码片段 module" },
    .{ .label = "defer", .body = "defer $0;", .detail = "代码片段 defer" },
    .{ .label = "errdefer", .body = "errdefer $0;", .detail = "代码片段 errdefer" },
    .{ .label = "try", .body = "try $0", .detail = "代码片段 try" },
    .{ .label = "return", .body = "return $0;", .detail = "代码片段 return" },
    .{ .label = "continue", .body = "continue;", .detail = "代码片段 continue" },
    .{ .label = "orelse", .body = "orelse $0", .detail = "代码片段 orelse" },
    .{ .label = "main", .body = "pub const main = fn () !i32 {\n    $0\n    return 0;\n};", .detail = "代码片段 main" },
    .{ .label = "println", .body = "std.println(\"{}\", $0);", .detail = "代码片段 println" },
    .{ .label = "test", .body = "pub const ${1:t_name} = fn () void {\n    $0\n};", .detail = "代码片段 test" },
};

fn snippetFor(label: []const u8) ?Snip {
    for (snippets) |s| {
        if (std.mem.eql(u8, s.label, label)) return s;
    }
    return null;
}

const BuiltinNs = struct { name: []const u8, members: []const []const u8, detail: []const u8 };

// std.<ns> 编译器内建命名空间成员（与 src/sema.c std_builtin()/ns_member() 逐字对齐）
const std_ns_table = [_]BuiltinNs{
    .{ .name = "heap", .members = &.{"page"}, .detail = "std.heap：向系统要页的分配器" },
    .{ .name = "file", .members = &.{ "read", "write", "append", "exists", "remove" }, .detail = "std.file：整块读/写文件" },
    .{ .name = "io", .members = &.{ "openRead", "openWrite", "openAppend", "read", "write", "seek", "tell", "close", "flush", "stdin", "stdout", "stderr", "print", "println" }, .detail = "std.io：句柄流 I/O" },
    .{ .name = "process", .members = &.{ "args", "getEnv", "setEnv", "exit" }, .detail = "std.process：进程与环境变量" },
    .{ .name = "debug", .members = &.{ "panic", "assert", "errName", "printTrace" }, .detail = "std.debug：调试内建（panic/assert 位置自动注入）" },
    .{ .name = "atomic", .members = &.{ "atomicAdd", "atomicSub", "atomicLoad", "atomicStore", "atomicSwap", "atomicCAS" }, .detail = "std.atomic：原子操作（SEQ_CST）" },
};

// KEEP — std 内建的签名与调用骨架（用户 2026-11-09 第 2 条：std 空间里的
// 函数没有类型提示、snippet 也要做）。这些成员实现在编译器 C 侧（sema.c
// std_builtin()），std.zx 里没有声明文本可查，hover/补全 detail 只能从
// 这张手工对齐表出；签名与 src/sema.c 的 ty_fn 注册逐条对照，漂移只影响
// 显示不会出错。snip 非空时点补全出 LSP 骨架（label 后接的部分，光标
// tabstop 与 snippets 同约定）。
const StdBuiltin = struct {
    path: []const u8, // 完整路径："std.println" / "std.io.openRead"
    sig: []const u8, // hover / 补全 detail 的签名
    snip: []const u8 = "", // 非空 → 骨架插 name+snip
    // 命名空间（heap/io/debug/…）：不是函数，补全按 Module 出（kind=9），
    // 免得用户把它们当函数调用（用户 2026-11-09：`std.debug(` 报
    // "called value has type `type`"）。
    ns: bool = false,
};

const std_builtin_docs = [_]StdBuiltin{
    .{ .path = "std.print", .sig = "print(fmt: []const u8, args: anytype) void", .snip = "(\"${1:fmt}\", $0)" },
    .{ .path = "std.println", .sig = "println(fmt: []const u8, args: anytype) void", .snip = "(\"${1:fmt}\", $0)" },
    .{ .path = "std.format", .sig = "format(alloc: Allocator, fmt: []const u8, args: anytype) []u8", .snip = "(${1:alloc}, \"${2:fmt}\", $0)" },
    .{ .path = "std.heap", .sig = "heap — 命名空间（不能直接调用）：std.heap.page 是现成的页分配器", .ns = true },
    .{ .path = "std.heap.page", .sig = "page: Allocator" },
    .{ .path = "std.file", .sig = "file — 命名空间（不能直接调用）：read/write/append/exists/remove", .ns = true },
    .{ .path = "std.file.read", .sig = "read(alloc: Allocator, path: []const u8) ?[]const u8", .snip = "(${1:alloc}, ${2:path})$0" },
    .{ .path = "std.file.write", .sig = "write(path: []const u8, data: []const u8) bool", .snip = "(${1:path}, ${2:data})$0" },
    .{ .path = "std.file.append", .sig = "append(path: []const u8, data: []const u8) bool", .snip = "(${1:path}, ${2:data})$0" },
    .{ .path = "std.file.exists", .sig = "exists(path: []const u8) bool", .snip = "(${1:path})$0" },
    .{ .path = "std.file.remove", .sig = "remove(path: []const u8) bool", .snip = "(${1:path})$0" },
    .{ .path = "std.io", .sig = "io — 命名空间（不能直接调用）：openRead/openWrite/read/write/close/…", .ns = true },
    .{ .path = "std.io.openRead", .sig = "openRead(path: []const u8) ?usize", .snip = "(${1:path})$0" },
    .{ .path = "std.io.openWrite", .sig = "openWrite(path: []const u8) ?usize", .snip = "(${1:path})$0" },
    .{ .path = "std.io.openAppend", .sig = "openAppend(path: []const u8) ?usize", .snip = "(${1:path})$0" },
    .{ .path = "std.io.read", .sig = "read(h: usize, buf: []u8) usize", .snip = "(${1:h}, ${2:buf})$0" },
    .{ .path = "std.io.write", .sig = "write(h: usize, data: []const u8) usize", .snip = "(${1:h}, ${2:data})$0" },
    .{ .path = "std.io.seek", .sig = "seek(h: usize, off: usize) bool", .snip = "(${1:h}, ${2:off})$0" },
    .{ .path = "std.io.tell", .sig = "tell(h: usize) usize", .snip = "(${1:h})$0" },
    .{ .path = "std.io.close", .sig = "close(h: usize) void", .snip = "(${1:h})$0" },
    .{ .path = "std.io.flush", .sig = "flush(h: usize) void", .snip = "(${1:h})$0" },
    .{ .path = "std.io.stdin", .sig = "stdin() usize", .snip = "()$0" },
    .{ .path = "std.io.stdout", .sig = "stdout() usize", .snip = "()$0" },
    .{ .path = "std.io.stderr", .sig = "stderr() usize", .snip = "()$0" },
    .{ .path = "std.io.print", .sig = "print(h: usize, fmt: []const u8, args: anytype) void", .snip = "(${1:h}, \"${2:fmt}\", $0)" },
    .{ .path = "std.io.println", .sig = "println(h: usize, fmt: []const u8, args: anytype) void", .snip = "(${1:h}, \"${2:fmt}\", $0)" },
    .{ .path = "std.process", .sig = "process — 命名空间（不能直接调用）：args/getEnv/setEnv/exit", .ns = true },
    .{ .path = "std.process.args", .sig = "args() [][]const u8", .snip = "()$0" },
    .{ .path = "std.process.getEnv", .sig = "getEnv(name: []const u8) ?[]const u8", .snip = "(${1:name})$0" },
    .{ .path = "std.process.setEnv", .sig = "setEnv(name: []const u8, value: []const u8) bool", .snip = "(${1:name}, ${2:value})$0" },
    .{ .path = "std.process.exit", .sig = "exit(code: u8) noreturn", .snip = "(${1:code})$0" },
    .{ .path = "std.debug", .sig = "debug — 命名空间（不能直接调用）：panic/assert/errName/printTrace", .ns = true },
    .{ .path = "std.debug.panic", .sig = "panic(msg: []const u8, args: anytype) noreturn", .snip = "(\"${1:msg}\", $0)" },
    .{ .path = "std.debug.assert", .sig = "assert(cond: bool, msg: []const u8) void", .snip = "(${1:cond}, \"${2:msg}\")$0" },
    .{ .path = "std.debug.errName", .sig = "errName(e: error) []const u8", .snip = "(${1:e})$0" },
    .{ .path = "std.debug.printTrace", .sig = "printTrace() void", .snip = "()$0" },
    .{ .path = "std.atomic", .sig = "atomic — 命名空间（不能直接调用）：atomicAdd/atomicLoad/atomicCAS/…", .ns = true },
    .{ .path = "std.atomic.atomicAdd", .sig = "atomicAdd(p: *u64, n: usize) usize", .snip = "(${1:p}, ${2:n})$0" },
    .{ .path = "std.atomic.atomicSub", .sig = "atomicSub(p: *u64, n: usize) usize", .snip = "(${1:p}, ${2:n})$0" },
    .{ .path = "std.atomic.atomicLoad", .sig = "atomicLoad(p: *u64) usize", .snip = "(${1:p})$0" },
    .{ .path = "std.atomic.atomicStore", .sig = "atomicStore(p: *u64, n: usize) void", .snip = "(${1:p}, ${2:n})$0" },
    .{ .path = "std.atomic.atomicSwap", .sig = "atomicSwap(p: *u64, n: usize) usize", .snip = "(${1:p}, ${2:n})$0" },
    .{ .path = "std.atomic.atomicCAS", .sig = "atomicCAS(p: *u64, old: usize, new: usize) bool", .snip = "(${1:p}, ${2:old}, ${3:new})$0" },
    // 类型内建（纯编译期反射）：不进 std. 补全（isCtBuiltin），hover 照常可查。
    // Allocator/stackAlloc/heapAlloc 不在过滤之列——用户 2026-11-09 第 4 条
    // （下午）：这三个要暴露出来，还要带 snippet。
    .{ .path = "std.Allocator", .sig = "Allocator: type — 分配器接口 { ctx: ?*u8, alloc: fn(?*u8, usize) ?*u8, free: fn(?*u8, *u8, usize) void }",
       .snip = "{ .ctx = ${1:null}, .alloc = ${2:allocFn}, .free = ${3:freeFn} }$0" },

    .{ .path = "std.stackAlloc", .sig = "stackAlloc(T: type, n: usize) *[n]T — 栈上分配（编译期 n）",
       .snip = "(${1:T}, ${2:n})$0" },
    .{ .path = "std.heapAlloc", .sig = "heapAlloc(alloc: Allocator, T: type, n: usize) *[n]T — 堆分配，OOM abort",
       .snip = "(${1:alloc}, ${2:T}, ${3:n})$0" },
};

/// std 内建条目的 CompletionItemKind：签名里带 `type` 的是类型（22），
/// 其余当函数（3）；命名空间由调用方单独给 9。
fn stdBuiltinKind(sig: []const u8) u32 {
    // `Name: type — …`：名字后面直接跟类型说明，是类型条目。
    // `stackAlloc(T: type, n: usize) …` 里的 `T: type` 是形参，不是类型条目。
    if (std.mem.indexOf(u8, sig, ": type")) |p| {
        const lp = std.mem.indexOfScalar(u8, sig, '(');
        if (lp == null or p < lp.?) return 22;
    }
    if (std.mem.endsWith(u8, sig, " type")) return 22;
    return 3;
}

fn stdBuiltinDoc(path: []const u8) ?StdBuiltin {
    for (std_builtin_docs) |b| {
        if (std.mem.eql(u8, b.path, path)) return b;
    }
    return null;
}

/// 1.2.2 反射只剩一个 @type：Type/Field 描述符仍只在编译期存在，
/// 不许进 std. 补全列表（去污染）。
fn isCtBuiltin(name: []const u8) bool {
    const ct = [_][]const u8{ "Type", "Field" };
    for (ct) |c| {
        if (std.mem.eql(u8, name, c)) return true;
    }
    return false;
}

// std 顶层由编译器 seed 的名字（src/sema.c g_std_seeded；std.zx 自己的声明另行扫描）
const std_seeded = [_][]const u8{
    "print",     "println", "format",   "heap",     "file",
    "io",        "Allocator", "stackAlloc", "heapAlloc", "process",
};

const BuiltinDoc = struct { name: []const u8, detail: []const u8 };

// @ 内建（与 src/sema.c check_builtin() 逐字对齐，1.2.2 反射只剩 @type）：
// 类型转换统一写 `(T)value`；描述符全在编译期。
const builtin_docs = [_]BuiltinDoc{
    .{ .name = "type", .detail = "@type(x) 编译期描述符：.name/.size/.align/.ty/.fieldCount/.fields[i]{.name,.ty,.offset,.ptr}/.statics[i]；x 是左值时 ptr 指向该实例的字段，f.ptr.0 取值" },
    .{ .name = "memcpy", .detail = "@memcpy(dest, src, count) 内存复制" },
    .{ .name = "memmove", .detail = "@memmove(dest, src, count) 内存移动" },
    .{ .name = "memset", .detail = "@memset(dest, fill, count) 内存填充" },
    .{ .name = "link", .detail = "@link(\"lib\") 链接外部库" },
    .{ .name = "trap", .detail = "@trap() 触发断点/中止" },
    .{ .name = "compileError", .detail = "@compileError(msg) 编译期报错" },
};

const toplevel_builtins = [_][]const u8{
    "type",   "memcpy",  "memmove", "memset",
    "link",   "trap",    "compileError",
};

// ---------------------------------------------------------------- utils

/// 探针日志：追加一行到 cwd/bin/zxls-lsp.log，排查宿主（Zed）是否真的
/// 走到 didOpen/诊断发布。开销可忽略，留作长期排查手段。
fn logLine(msg: []const u8) void {
    // 写到 exe 同目录（绝对路径），不依赖宿主给的 cwd
    const path = std.fmt.allocPrint(gpa, "{s}/zxls-lsp.log", .{exeDir()}) catch return;
    defer gpa.free(path);
    if (Io.Dir.cwd().openFile(g_io, path, .{ .mode = .read_write })) |f| {
        defer f.close(g_io);
        const end = f.length(g_io) catch return;
        f.writePositionalAll(g_io, msg, end) catch return;
        f.writePositionalAll(g_io, "\n", end + msg.len) catch return;
    } else |_| {
        var buf: std.ArrayList(u8) = .empty;
        defer buf.deinit(gpa);
        buf.appendSlice(gpa, msg) catch return;
        buf.append(gpa, '\n') catch return;
        Io.Dir.cwd().writeFile(g_io, .{ .sub_path = path, .data = buf.items }) catch {};
    }
}

/// 非法 UTF-8 序列替换为 U+FFFD；合法输入原样返回（零拷贝）。
fn sanitizeUtf8(s: []const u8) []const u8 {
    if (std.unicode.utf8ValidateSlice(s)) return s;
    var out: std.ArrayList(u8) = .empty;
    var i: usize = 0;
    while (i < s.len) {
        const len = std.unicode.utf8ByteSequenceLength(s[i]) catch {
            out.appendSlice(gpa, "\u{FFFD}") catch {};
            i += 1;
            continue;
        };
        if (i + len <= s.len and std.unicode.utf8ValidateSlice(s[i .. i + len])) {
            out.appendSlice(gpa, s[i .. i + len]) catch {};
            i += len;
        } else {
            out.appendSlice(gpa, "\u{FFFD}") catch {};
            i += 1;
        }
    }
    logLine("sanitized invalid utf-8 in outgoing string");
    return out.items;
}

fn dupe(s: []const u8) []const u8 {
    return gpa.dupe(u8, s) catch @panic("oom");
}

/// file:/// URI → 本地路径（处理 %XX 与 Windows 盘符）
fn uriToPath(uri: []const u8) []const u8 {
    if (!std.mem.startsWith(u8, uri, "file://")) return dupe(uri);
    var p = uri["file://".len..];
    if (p.len > 0 and p[0] == '/') p = p[1..]; // file:///C:/... → C:/...
    var out: std.ArrayList(u8) = .empty;
    var i: usize = 0;
    while (i < p.len) : (i += 1) {
        if (p[i] == '%' and i + 2 < p.len) {
            const hi = std.fmt.charToDigit(p[i + 1], 16) catch {
                out.append(gpa, p[i]) catch @panic("oom");
                continue;
            };
            const lo = std.fmt.charToDigit(p[i + 2], 16) catch {
                out.append(gpa, p[i]) catch @panic("oom");
                continue;
            };
            out.append(gpa, hi * 16 + lo) catch @panic("oom");
            i += 2;
        } else out.append(gpa, p[i]) catch @panic("oom");
    }
    return out.items;
}

/// LSP position（UTF-16 计量）→ 文档字节偏移
fn posToOffset(text: []const u8, line: u32, character: u32) usize {
    var ln: u32 = 0;
    var i: usize = 0;
    while (ln < line and i < text.len) : (i += 1) {
        if (text[i] == '\n') ln += 1;
    }
    var units: u32 = 0;
    // '\r' 视为行终止符的一部分（CRLF 文档）：客户端位置不含行尾，
    // 越界 clamp 时不能把 '\r' 吃进来，否则 off-1 落在 '\r' 上破坏点补全判定。
    while (i < text.len and text[i] != '\n' and text[i] != '\r' and units < character) {
        const n = std.unicode.utf8ByteSequenceLength(text[i]) catch 1;
        const end = @min(i + n, text.len);
        const cp = std.unicode.utf8Decode(text[i..end]) catch {
            units += 1;
            i += 1;
            continue;
        };
        units += if (cp < 0x10000) 1 else 2;
        i = end;
    }
    return i;
}

fn isIdentChar(c: u8) bool {
    return (c >= 'a' and c <= 'z') or (c >= 'A' and c <= 'Z') or c == '_' or (c >= '0' and c <= '9');
}

/// offset 前的标识符（光标贴在词尾/词中）
fn identBefore(text: []const u8, offset: usize) []const u8 {
    const e = @min(offset, text.len);
    var s = e;
    while (s > 0 and isIdentChar(text[s - 1])) s -= 1;
    return text[s..e];
}

fn baseName(path: []const u8) []const u8 {
    if (std.mem.lastIndexOfScalar(u8, path, '/')) |s| return path[s + 1 ..];
    if (std.mem.lastIndexOfScalar(u8, path, '\\')) |s| return path[s + 1 ..];
    return path;
}

fn dirName(path: []const u8) []const u8 {
    if (std.mem.lastIndexOfScalar(u8, path, '/')) |s| return path[0..s];
    if (std.mem.lastIndexOfScalar(u8, path, '\\')) |s| return path[0..s];
    return ".";
}

// ---------------------------------------------------------------- scanning

const DeclKind = enum { value, field, variant };

// KEEP — 补全弹窗里字段候选的 detail 必须是字段声明类型，不是容器名
//（用户 2026-11-08 02:15 拍板：字段类型不做行内 inlay，太吵；写的时候给提示）。
// 配套四处：Decl.ty、extractBraceMembers / anonFieldOne 填类型、readFieldTy、
// completeAt 的 addItem。lsptest.py 的 completion.*.fieldType 4 条是守门断言。
const Decl = struct {
    name: []const u8,
    kind: DeclKind,
    ty: []const u8 = "",
    is_fn: bool = false,   // KEEP: extern "C" { pub fn ... } 的模块成员
};

/// 从 ':' 之后读字段类型，直到顶层分隔符（, } ; 换行）。
/// 括号/方括号/花括号内部的分隔符不算——`rect: {f64, f64}`、`[]const u8`、
/// `struct{a:i32,b:u32}` 都要完整保留。返回去掉首尾空白的切片。
fn readFieldTy(src: []const u8, from: usize) []const u8 {
    var i = from;
    while (i < src.len and (src[i] == ' ' or src[i] == '\t' or src[i] == '\r' or src[i] == '\n')) i += 1;
    const s = i;
    var d_par: usize = 0;
    var d_br: usize = 0;
    var d_sq: usize = 0;
    while (i < src.len) : (i += 1) {
        const c = src[i];
        if (c == '"') {
            i += 1;
            while (i < src.len and src[i] != '"' and src[i] != '\n') {
                if (src[i] == '\\' and i + 1 < src.len) i += 1;
                i += 1;
            }
            continue;
        }
        switch (c) {
            '(' => d_par += 1,
            ')' => if (d_par > 0) { d_par -= 1; },
            '{' => d_br += 1,
            '}' => {
                if (d_br > 0) {
                    d_br -= 1;
                } else if (d_par == 0 and d_sq == 0) break; // 容器体结束
            },
            '[' => d_sq += 1,
            ']' => if (d_sq > 0) { d_sq -= 1; },
            ',', ';', '\n' => if (d_par == 0 and d_br == 0 and d_sq == 0) break,
            else => {},
        }
    }
    return trimSl(src[s..i]);
}

/// 顶层声明：`[pub] (const|var|type) NAME =` 与 `[pub] [extern] fn NAME(`。
/// KEEP: 第二条是 C 绑定的入口——`extern "C" { pub fn glClear(...); }` 里的
/// 声明带缩进、被花括号包着，但语义上仍是模块的 pub 成员。以前只认 const/
/// var/type，于是 `zxc include` 生成的 vendor 绑定和手写的 gl 模块都补不出
/// 成员、hover 也没签名。块本身 `extern "C" {` 剩 `"C" {` 不命中任何前缀；
/// 文件根的 `pub extern var X: T;`（cbind 生成器写法）也一起收下。
/// 一行代码对花括号深度的净影响：跳过 `//` 行注释、`/* */` 块注释、
/// `"…"` 与 `'…'` 字面量里的括号。
fn lineBraceDelta(line: []const u8, in_block: *bool) i32 {
    var d: i32 = 0;
    var i: usize = 0;
    while (i < line.len) : (i += 1) {
        const c = line[i];
        if (in_block.*) {
            if (c == '*' and i + 1 < line.len and line[i + 1] == '/') {
                in_block.* = false;
                i += 1;
            }
            continue;
        }
        if (c == '/' and i + 1 < line.len) {
            if (line[i + 1] == '/') return d;
            if (line[i + 1] == '*') {
                in_block.* = true;
                i += 1;
                continue;
            }
        }
        if (c == '"' or c == '\'') {
            const q = c;
            i += 1;
            while (i < line.len) : (i += 1) {
                if (line[i] == '\\') {
                    i += 1;
                    continue;
                }
                if (line[i] == q) break;
            }
            continue;
        }
        if (c == '{') d += 1;
        if (c == '}') d -= 1;
    }
    return d;
}

fn topLevelDecls(text: []const u8, out: *std.ArrayList(Decl), pub_only: bool) void {
    // KEEP — 必须跟花括号深度：容器体里的成员不是模块成员。2026-11-10 给
    // 容器成员加 `pub` 之后，纯行扫描把 `pub const push = fn…` 当成 std 顶层
    // 声明，`std.` 一弹就混进 ArrayList/BufReader 的整屏方法（用户截图）。
    var depth: i32 = 0;
    // 顶层 `extern "C" { … }` 块是**透明作用域**：块里的 `pub fn glClear(…)`
    // 仍是模块成员（cbind 生成物/手写绑定都长这样）。记下块内所在的深度。
    var top_depth: i32 = 0;
    var in_block = false;
    var it = std.mem.splitScalar(u8, text, '\n');
    while (it.next()) |line_raw| {
        var rest = std.mem.trim(u8, line_raw, " \t\r");
        const at_top = depth == top_depth and !in_block;
        depth += lineBraceDelta(line_raw, &in_block);
        if (at_top and std.mem.startsWith(u8, rest, "extern") and
            std.mem.indexOfScalar(u8, rest, '{') != null) top_depth = depth;
        if (depth <= 0) top_depth = 0;
        if (!at_top) continue;
        if (rest.len == 0 or rest[0] == '/' or rest[0] == '@' or rest[0] == '}') continue;
        var is_pub = false;
        if (std.mem.startsWith(u8, rest, "pub ")) {
            is_pub = true;
            rest = std.mem.trimStart(u8, rest[4..], " \t");
        }
        if (pub_only and !is_pub) continue;
        if (std.mem.startsWith(u8, rest, "extern "))
            rest = std.mem.trimStart(u8, rest[7..], " \t");
        if (std.mem.startsWith(u8, rest, "fn ")) {
            const tail = rest[3..];
            var n: usize = 0;
            while (n < tail.len and isIdentChar(tail[n])) n += 1;
            if (n > 0 and n < tail.len and tail[n] == '(')
                out.append(gpa, .{ .name = tail[0..n], .kind = .value, .is_fn = true }) catch {};
            continue;
        }
        const prefixes = [_][]const u8{ "const ", "var ", "type " };
        for (prefixes) |p| {
            if (std.mem.startsWith(u8, rest, p)) {
                const tail = rest[p.len..];
                var n: usize = 0;
                while (n < tail.len and isIdentChar(tail[n])) n += 1;
                if (n > 0) {
                    // KEEP: 带显式类型注解的常量把类型记进 ty（detail 显示
                    // `i32` 而不是绑定别名）；`const NAME = fn (...)` 标
                    // is_fn——模块里 `pub const hello = fn …` 这种 const 包着
                    // 的函数也要按函数出补全（kind=3 + 签名 detail），
                    // 用户 2026-11-08 截图反馈：弹窗里全是别名看不出函数。
                    var ty: []const u8 = "";
                    var is_fn = false;
                    var k = n;
                    while (k < tail.len and (tail[k] == ' ' or tail[k] == '\t')) k += 1;
                    if (k < tail.len and tail[k] == ':') {
                        // 类型段只到 '=' 为止：readFieldTy 的分隔符集合里没有
                        // '='，直接喂全文会把 `i32 = 1` 整段当类型。
                        const ty_from = if (std.mem.indexOfScalarPos(u8, tail, k, '=')) |eq|
                            tail[k + 1 .. eq]
                        else
                            tail[k + 1 ..];
                        ty = readFieldTy(ty_from, 0);
                    } else if (k < tail.len and tail[k] == '=') {
                        var e = k + 1;
                        while (e < tail.len and (tail[e] == ' ' or tail[e] == '\t')) e += 1;
                        is_fn = std.mem.startsWith(u8, tail[e..], "fn ") or
                            std.mem.startsWith(u8, tail[e..], "fn(");
                    }
                    out.append(gpa, .{ .name = tail[0..n], .kind = .value, .ty = ty, .is_fn = is_fn }) catch {};
                }
                break;
            }
        }
    }
}

/// pos 是否落在 `//` 行注释里（找声明时别把注释里的同名 fn 当真声明）。
fn inLineComment(text: []const u8, pos: usize) bool {
    var p = pos;
    while (p > 0 and text[p - 1] != '\n') {
        p -= 1;
        if (p + 1 < text.len and text[p] == '/' and text[p + 1] == '/') return true;
    }
    return false;
}

/// 当前文件里"能单独写出来的名字"。以前是纯词法扫词，把 `g.glfwInit(...)`、
/// 注释和字符串里的词都收进来，结果输入 glfw 弹一屏不可用的 C 函数名，
/// 标着"当前文件"（用户 2026-11-08 反馈）。点成员名不是本作用域的绑定，
/// 成员补全走 `绑定名.` 那条链；真有同名局部变量时声明处仍会收到。
fn collectIdents(text: []const u8, set: *std.StringHashMapUnmanaged(void)) void {
    var i: usize = 0;
    while (i < text.len) {
        const c = text[i];
        if (c == '/' and i + 1 < text.len and text[i + 1] == '/') {
            while (i < text.len and text[i] != '\n') i += 1;
            continue;
        }
        if (c == '"') {
            i += 1;
            while (i < text.len and text[i] != '"' and text[i] != '\n') i += 1;
            if (i < text.len) i += 1;
            continue;
        }
        if (isIdentChar(c) and !(c >= '0' and c <= '9')) {
            var j = i + 1;
            while (j < text.len and isIdentChar(text[j])) j += 1;
            var p = i;
            while (p > 0 and (text[p - 1] == ' ' or text[p - 1] == '\t')) p -= 1;
            const is_member = p > 0 and text[p - 1] == '.';
            if (!is_member and j - i >= 2 and j - i <= 64)
                set.put(gpa, text[i..j], {}) catch {};
            i = j;
        } else i += 1;
    }
}

/// `error{` 的 '{' 位置。跳过 // 注释与字符串；`myerror{` 这种长名不认
/// （前一个字符是标识符字节就继续扫）。
fn findErrBrace(text: []const u8, from: usize) ?usize {
    var i = from;
    while (i + 5 <= text.len) : (i += 1) {
        const c = text[i];
        if (c == '/' and i + 1 < text.len and text[i + 1] == '/') {
            while (i < text.len and text[i] != '\n') i += 1;
            continue;
        }
        if (c == '"') {
            i += 1;
            while (i < text.len and text[i] != '"' and text[i] != '\n') i += 1;
            continue;
        }
        if (!std.mem.startsWith(u8, text[i..], "error")) continue;
        if (i > 0 and isIdentChar(text[i - 1])) continue;
        var p = i + 5;
        while (p < text.len and (text[p] == ' ' or text[p] == '\t')) p += 1;
        if (p < text.len and text[p] == '{') return p;
    }
    return null;
}

/// 从 '{' 起收割错误集成员（逗号分隔的名字，可带 `= 值`）。返回 '}' 之后的位置。
fn harvestErrBlock(text: []const u8, brace: usize, out: *std.ArrayList(Decl)) usize {
    var depth: usize = 0;
    var i = brace + 1;
    while (i < text.len) : (i += 1) {
        const c = text[i];
        if (c == '/' and i + 1 < text.len and text[i + 1] == '/') {
            while (i < text.len and text[i] != '\n') i += 1;
            continue;
        }
        if (c == '"') {
            i += 1;
            while (i < text.len and text[i] != '"' and text[i] != '\n') i += 1;
            continue;
        }
        if (c == '{') depth += 1;
        if (c == '}') {
            if (depth == 0) return i + 1;
            depth -= 1;
        }
        if (depth == 0 and isIdentChar(c) and !(c >= '0' and c <= '9')) {
            var j = i + 1;
            while (j < text.len and isIdentChar(text[j])) j += 1;
            out.append(gpa, .{ .name = text[i..j], .kind = .variant }) catch {};
            i = j - 1; // 循环末尾 +1，正好落在名字后
        }
    }
    return i;
}

/// `error.` 的补全：所有 `error{ … }` 集合 + 所有 `error.x` 用法里的码名。
/// 错误码名在编译器侧是全局 intern 的（没有作用域），所以工作区一起扫，
/// 见过的都端出来——这与容器分支的"宁缺毋滥"不同：码名来源可靠，
/// 只认 error{ } 块与 error.x 用法，混不进局部变量。
fn collectAllErrMembers(files: []const []const u8, out: *std.ArrayList(Decl)) bool {
    for (files) |ftext| {
        var idx: usize = 0;
        while (findErrBrace(ftext, idx)) |brace| idx = harvestErrBlock(ftext, brace, out);
        var q: usize = 0;
        while (std.mem.indexOfPos(u8, ftext, q, "error.")) |hit| {
            q = hit + 6;
            if (q >= ftext.len) break;
            if (!isIdentChar(ftext[q]) or (ftext[q] >= '0' and ftext[q] <= '9')) continue;
            var j = q + 1;
            while (j < ftext.len and isIdentChar(ftext[j])) j += 1;
            out.append(gpa, .{ .name = ftext[q..j], .kind = .variant }) catch {};
            q = j;
        }
    }
    return out.items.len > 0;
}

/// `const <name> = error{` 的块起点（文件文本 + '{' 位置）。找不到返回 null。
fn findErrsetBlock(files: []const []const u8, name: []const u8) ?struct { text: []const u8, brace: usize } {
    var pat: std.ArrayList(u8) = .empty;
    defer pat.deinit(gpa);
    pat.appendSlice(gpa, "const ") catch return null;
    pat.appendSlice(gpa, name) catch return null;
    pat.appendSlice(gpa, " =") catch return null;
    for (files) |ftext| {
        var idx: usize = 0;
        while (std.mem.indexOfPos(u8, ftext, idx, pat.items)) |hit| {
            if (hit > 0 and isIdentChar(ftext[hit - 1])) {
                idx = hit + 1;
                continue;
            }
            var p = hit + pat.items.len;
            while (p < ftext.len and (ftext[p] == ' ' or ftext[p] == '\t')) p += 1;
            if (!std.mem.startsWith(u8, ftext[p..], "error")) {
                idx = hit + 1;
                continue;
            }
            p += 5;
            while (p < ftext.len and (ftext[p] == ' ' or ftext[p] == '\t')) p += 1;
            if (p >= ftext.len or ftext[p] != '{') {
                idx = hit + 1;
                continue;
            }
            return .{ .text = ftext, .brace = p };
        }
    }
    return null;
}

/// `Err.` 的补全：`const Err = error{ … }` 那一块的成员。
/// 块里引用别的集合（`error{ IoErr, extra }`）时递归展开——与 sema 的
/// 合并语义一致（NK_ERR_SET 的 refs），引用到的名字必须是声明的集合，
/// 否则当普通成员。depth 限 4 层，互相引用不会挂死。
fn collectNamedErrMembers(files: []const []const u8, name: []const u8, out: *std.ArrayList(Decl)) bool {
    return namedErrInner(files, name, out, 0);
}

fn namedErrInner(files: []const []const u8, name: []const u8, out: *std.ArrayList(Decl), depth: u8) bool {
    const hit = findErrsetBlock(files, name) orelse return false;
    var raw: std.ArrayList(Decl) = .empty;
    defer raw.deinit(gpa);
    _ = harvestErrBlock(hit.text, hit.brace, &raw);
    for (raw.items) |d| {
        if (depth < 4 and findErrsetBlock(files, d.name) != null) {
            _ = namedErrInner(files, d.name, out, depth + 1);
        } else {
            out.append(gpa, d) catch {};
        }
    }
    return out.items.len > 0;
}

/// 在 files 里找 `const BASE = struct/union/enum …{`，提取成员
const ContainerHit = struct { body: []const u8, is_enum: bool };

/// 在 files 里找 `const <base> = struct/union/enum {`，返回从 '{' 起的体
/// 与是否 enum。containerMembers 与方法返回类型解析共用这条匹配。
fn findContainerBody(files: []const []const u8, base: []const u8) ?ContainerHit {
    var pat: std.ArrayList(u8) = .empty;
    defer pat.deinit(gpa);
    pat.appendSlice(gpa, "const ") catch return null;
    pat.appendSlice(gpa, base) catch return null;
    for (files) |ftext| {
        var idx: usize = 0;
        while (std.mem.indexOfPos(u8, ftext, idx, pat.items)) |hit| {
            if (hit > 0 and isIdentChar(ftext[hit - 1])) {
                idx = hit + 1;
                continue;
            }
            var p = hit + pat.items.len;
            while (p < ftext.len and (ftext[p] == ' ' or ftext[p] == '\t')) p += 1;
            if (p >= ftext.len or ftext[p] != '=') {
                idx = hit + 1;
                continue;
            }
            p += 1;
            while (p < ftext.len and ftext[p] != '{' and ftext[p] != '\n') p += 1;
            if (p >= ftext.len or ftext[p] != '{') {
                idx = hit + 1;
                continue;
            }
            const head = ftext[hit + pat.items.len .. p];
            const is_enum = std.mem.indexOf(u8, head, "enum") != null;
            const is_struct = std.mem.indexOf(u8, head, "struct") != null or
                std.mem.indexOf(u8, head, "union") != null;
            if (!is_enum and !is_struct) {
                idx = hit + 1;
                continue;
            }
            // 容器体必须止于匹配的 '}'：旧版 ftext[p..] 一路到文件尾，
            // 后面的 Arr/Obj 方法会被当成本容器的方法（Value 因此误出
            // `Value.xxx` 工厂项、构造骨架反而被挤掉）。
            const close = matchBrace(ftext, p) orelse {
                idx = hit + 1;
                continue;
            };
            return .{ .body = ftext[p .. close + 1], .is_enum = is_enum };
        }
    }
    return null;
}

fn containerMembers(files: []const []const u8, base: []const u8, fields: *std.ArrayList(Decl)) bool {
    const hit = findContainerBody(files, base) orelse return false;
    extractBraceMembers(hit.body, hit.is_enum, fields);
    return true;
}

/// 容器成员的统一装载入口（用户 2026-11-09 第 7 条：不要一类一类地补，
/// 递归地找类型实现）。两种形态都认：
///   1 `const X = struct/union/enum { … }`        —— findContainerBody
///   2 `const X = fn (T: type) type { return struct { … } }` —— genericBodyOf
/// 返回成员声明所在的文件文本（拼签名 / 判定容器 / 出骨架都要它），
/// 找不到返回 null。`std.map.HashMap.` 这类泛型容器的三段路径此前
/// 0 成员，就是因为只走了形态 1。
fn containerMembersAny(files: []const []const u8, base: []const u8,
                       fields: *std.ArrayList(Decl)) ?[]const u8 {
    if (containerMembers(files, base, fields)) return containerSrcOf(files, base);
    for (files) |ftext| {
        if (genericBodyOf(&[_][]const u8{ftext}, base)) |gb| {
            extractBraceMembers(gb, false, fields);
            return ftext;
        }
    }
    return null;
}

/// 容器内方法（`const <m> = fn (…) Ret {`）的返回类型。
/// std.Task.group() → Group 这类推断靠它；解析不出返回 null。
fn methodRetIn(files: []const []const u8, base: []const u8, method: []const u8) ?[]const u8 {
    const body = (findContainerBody(files, base) orelse return null).body;
    var pat: std.ArrayList(u8) = .empty;
    defer pat.deinit(gpa);
    pat.appendSlice(gpa, "const ") catch return null;
    pat.appendSlice(gpa, method) catch return null;
    var idx: usize = 0;
    while (std.mem.indexOfPos(u8, body, idx, pat.items)) |hit| {
        idx = hit + 1;
        if (hit > 0 and isIdentChar(body[hit - 1])) continue;
        var p = hit + pat.items.len;
        while (p < body.len and (body[p] == ' ' or body[p] == '\t')) p += 1;
        if (p >= body.len or body[p] != '=') continue;
        p += 1;
        while (p < body.len and (body[p] == ' ' or body[p] == '\t')) p += 1;
        if (p >= body.len or body[p] != 'f') continue; // 只要方法（fn …）
        // 参数列表的匹配 ')'，之后到 '{' 之间是返回类型
        var d: usize = 0;
        var q = p;
        while (q < body.len) : (q += 1) {
            if (body[q] == '(') d += 1;
            if (body[q] == ')') {
                d -= 1;
                if (d == 0) break;
            }
        }
        if (q >= body.len or d != 0) continue;
        q += 1;
        while (q < body.len and (body[q] == ' ' or body[q] == '\t')) q += 1;
        // 返回类型前的包装先剥掉：`?Pool` / `!Value` / `*Self` / `&T`
        //（`Pool.init` 返回 `?Pool`、`json.parse` 返回 `!Value`，旧版
        // 只看首字符是不是标识符，这些方法一律解析不出返回类型）
        while (q < body.len and (body[q] == '?' or body[q] == '!' or
            body[q] == '*' or body[q] == '&' or body[q] == ' ' or body[q] == '\t')) q += 1;
        if (q < body.len and std.mem.startsWith(u8, body[q..], "const ")) q += 6;
        var n = q;
        while (n < body.len and isIdentChar(body[n])) n += 1;
        if (n == q) continue;
        return dupe(body[q..n]);
    }
    return null;
}

/// `pub const NAME = module("X.zx")` 门面别名解析：返回 "X.zx"。
/// std.zx 的子模块（str/map/pool/…）都长这样，模块绑定查找
///（moduleOfBinding）只认单段名字，二级路径 std.str. 靠这里接住。
fn moduleAliasIn(text: []const u8, name: []const u8) ?[]const u8 {
    var it = std.mem.splitScalar(u8, text, '\n');
    while (it.next()) |line_raw| {
        var rest = std.mem.trim(u8, line_raw, " \t\r");
        if (std.mem.startsWith(u8, rest, "pub ")) rest = rest[4..];
        if (!std.mem.startsWith(u8, rest, "const ")) continue;
        rest = rest[6..];
        if (!std.mem.startsWith(u8, rest, name)) continue;
        rest = rest[name.len..];
        var k: usize = 0;
        while (k < rest.len and (rest[k] == ' ' or rest[k] == '\t')) k += 1;
        if (k >= rest.len or rest[k] != '=') continue;
        k += 1;
        while (k < rest.len and (rest[k] == ' ' or rest[k] == '\t')) k += 1;
        if (!std.mem.startsWith(u8, rest[k..], "module(")) continue;
        k += 7;
        if (k >= rest.len or rest[k] != '"') continue;
        k += 1;
        const e = std.mem.indexOfScalarPos(u8, rest, k, '"') orelse continue;
        return rest[k..e];
    }
    return null;
}

/// 容器/泛型容器声明所在的文件文本。容器实例的成员 detail 要拼签名、
/// 方法要出调用骨架，光有成员名不够——声明在 std/模块文件里时
/// findDeclStmt(当前文档) 落空，必须把成员声明所在的源文本带上。
fn containerSrcOf(files: []const []const u8, base: []const u8) ?[]const u8 {
    for (files) |ftext| {
        if (findContainerBody(&[_][]const u8{ftext}, base) != null) return ftext;
        if (genericBodyOf(&[_][]const u8{ftext}, base) != null) return ftext;
    }
    return null;
}

/// 泛型容器的实例体：`pub const ArrayList = fn (T: type) type {
/// return struct { … } }` 的 struct 体（'{' 起）。findContainerBody 只认
/// `const X = struct`，泛型 fn 形态落空——`let l = std.ArrayList(i32).init(…)`
/// 之后 `l.` 一个成员都不出（用户 2026-11-09 第 3 条）。编译期实参不做
/// 替换：字段类型里的 T 原样显示，成员名与 Self 方法照常出。
fn genericBodyOf(files: []const []const u8, base: []const u8) ?[]const u8 {
    var pat: std.ArrayList(u8) = .empty;
    defer pat.deinit(gpa);
    pat.appendSlice(gpa, "const ") catch return null;
    pat.appendSlice(gpa, base) catch return null;
    pat.appendSlice(gpa, " = fn") catch return null;
    for (files) |ftext| {
        var idx: usize = 0;
        while (std.mem.indexOfPos(u8, ftext, idx, pat.items)) |hit| {
            idx = hit + 1;
            if (hit > 0 and isIdentChar(ftext[hit - 1])) continue;
            // fn 体里第一个 struct/union 字面体：进 '{' 后跟踪深度，
            // 关键字后跟 '{' 且深度 ≥1 即返回该体（matchBrace 校验配对）
            var i = hit + pat.items.len;
            var depth: usize = 0;
            var started = false;
            while (i < ftext.len) : (i += 1) {
                const c = ftext[i];
                if (c == '{') {
                    depth += 1;
                    started = true;
                    continue;
                }
                if (c == '}') {
                    if (!started) break; // fn 头就断了，不是泛型容器
                    if (depth > 0) {
                        depth -= 1;
                        if (depth == 0) break; // fn 体结束也没遇到 struct
                    }
                    continue;
                }
                if (!started or depth < 1) continue;
                const is_struct = c == 's' and std.mem.startsWith(u8, ftext[i..], "struct");
                const is_union = c == 'u' and std.mem.startsWith(u8, ftext[i..], "union");
                if (!is_struct and !is_union) continue;
                if (i > 0 and isIdentChar(ftext[i - 1])) continue;
                var p = i;
                while (p < ftext.len and ftext[p] != '{' and ftext[p] != '\n') p += 1;
                if (p < ftext.len and ftext[p] == '{') {
                    // 同样止于匹配的 '}'：后面别的容器的方法不能算进来。
                    if (matchBrace(ftext, p)) |close| return ftext[p .. close + 1];
                }
            }
        }
    }
    return null;
}

/// 容器实例成员（extractBraceMembers 产出）的方法标记：`.value` 成员在
/// 声明文件里是 `const NAME = fn …` → is_fn=true。不标的话 detail 落到
/// "初始化表达式 ≤64 字符否则空"，方法永远没有签名、不出调用骨架
///（用户 2026-11-09 第 2/3 条：std 类型实体没有提示）。
fn markContainerFns(src: []const u8, decls: *std.ArrayList(Decl)) void {
    for (decls.items) |*d| {
        if (d.is_fn or d.kind != .value) continue;
        const stmt = findDeclStmt(src, d.name) orelse continue;
        if (sigOfStmt(stmt).is_fn) d.is_fn = true;
    }
}

/// 同上，但声明查找锁在容器体 `body` 里：同名成员（`A.B.Cfg.mk` 与顶层
/// `Cfg.mk`）在整份 src 里 findDeclStmt 会拿到别人的签名。
fn markContainerFnsIn(body: []const u8, src: []const u8, decls: *std.ArrayList(Decl)) void {
    for (decls.items) |*d| {
        if (d.is_fn or d.kind != .value) continue;
        const stmt = findMemberStmt(body, d.name) orelse findDeclStmt(src, d.name) orelse continue;
        if (sigOfStmt(stmt).is_fn) d.is_fn = true;
    }
}

/// 补全/hover/签名提示共用的扫描文件集：当前文档 + 文档目录 .zx + std 目录 .zx。
/// 自身文件跳过（text 已含）。调用方 defer deinit。
fn scanFiles(doc: *const Document, text: []const u8) std.ArrayList([]const u8) {
    var files: std.ArrayList([]const u8) = .empty;
    files.append(gpa, text) catch {};
    const doc_dir = dirName(doc.path);
    const self_name = baseName(doc.path);
    var d = Io.Dir.cwd().openDir(g_io, doc_dir, .{ .iterate = true }) catch return files;
    defer d.close(g_io);
    var it = d.iterate();
    while (it.next(g_io) catch null) |entry| {
        if (entry.kind != .file or !std.mem.endsWith(u8, entry.name, ".zx")) continue;
        if (std.mem.eql(u8, entry.name, self_name)) continue;
        var fp: std.ArrayList(u8) = .empty;
        defer fp.deinit(gpa);
        fp.appendSlice(gpa, doc_dir) catch continue;
        fp.append(gpa, '/') catch continue;
        fp.appendSlice(gpa, entry.name) catch continue;
        if (readFileSmall(fp.items)) |txt| files.append(gpa, txt) catch {};
    }
    // std 目录：仓库外工程里 std.Task 的成员定义在 <exe>/../std/task.zx，
    // 文档目录永远扫不到（findStdDir 对齐编译器 find_std_path）。
    if (g_std_dir) |sd| {
        var sd_d = Io.Dir.cwd().openDir(g_io, sd, .{ .iterate = true }) catch return files;
        defer sd_d.close(g_io);
        var sit = sd_d.iterate();
        while (sit.next(g_io) catch null) |entry| {
            if (entry.kind != .file or !std.mem.endsWith(u8, entry.name, ".zx")) continue;
            var fp: std.ArrayList(u8) = .empty;
            defer fp.deinit(gpa);
            fp.appendSlice(gpa, sd) catch continue;
            fp.append(gpa, '/') catch continue;
            fp.appendSlice(gpa, entry.name) catch continue;
            if (readFileSmall(fp.items)) |txt| files.append(gpa, txt) catch {};
        }
    }
    return files;
}

/// 实例变量 → 容器类型名：`let g = std.Task.group()` → "Group"。
/// 推断交给 inferContainer（files 需含 std 目录）。
fn instanceContainerTy(files: []const []const u8, text: []const u8, name: []const u8) ?[]const u8 {
    const s = findDeclStmt(text, name) orelse return null;
    const init = declInitOf(s) orelse return null;
    return inferContainer(files, text, init, 0);
}

/// KEEP — 模块容器实例的点路径类型（用户 2026-11-08 第 3 条）：
/// `let v = a.Outer{ … }; v.inner` → `inner` 的声明类型，
/// `v.inner.a` → `a` 的类型。首段按 `绑定.容器` 定位模块文件的容器体
/// （modContainerOf），后续段逐层 fieldTypeIn；内联 struct 用 structTyOf
/// 走 parseAnonTy 继续下钻。out_mtxt 出模块文件文本，供命名容器再查成员。
fn modInstanceTyOfPath(doc: *const Document, text: []const u8, path: []const u8,
                       out_mtxt: *?[]const u8) ?[]const u8 {
    out_mtxt.* = null;
    var it = std.mem.splitScalar(u8, path, '.');
    const first = it.next() orelse return null;
    if (first.len == 0) return null;
    const s = findDeclStmt(text, first) orelse return null;
    const init0 = trimSl(declInitOf(s) orelse return null);
    var j: usize = 0;
    while (j < init0.len and (isIdentChar(init0[j]) or init0[j] == '.')) j += 1;
    if (j == 0) return null;
    const q = init0[0..j];
    if (std.mem.indexOfScalar(u8, q, '.') == null) return null; // 本文件容器走别的分支
    const mc = modContainerOf(doc, text, q) orelse return null;
    out_mtxt.* = mc.txt;
    const mfiles = [_][]const u8{mc.txt};
    var ty: []const u8 = mc.member;
    var n: usize = 0;
    while (it.next()) |seg| {
        if (seg.len == 0) return null;
        var cbody: []const u8 = "";
        if (n == 0) {
            if (findContainerBody(&mfiles, ty)) |h| cbody = h.body else return null;
        } else if (structTyOf(ty)) |st| {
            // fieldTypeIn 要求从 '{' 起（旧的 body 切片约定）
            const bi = std.mem.indexOfScalar(u8, st, '{') orelse return null;
            cbody = st[bi..];
        } else if (findContainerBody(&mfiles, ty)) |h| {
            cbody = h.body;
        } else return null;
        ty = fieldTypeIn(cbody, seg) orelse return null;
        n += 1;
    }
    if (n == 0) return null;
    return ty;
}

/// 点路径切段：按 '.' 切，括号里的点不切（泛型实例化 `HashMap(i32)`），
/// 每段去掉泛型实例化尾缀：`HashMap(i32)` → `HashMap`。
fn splitPathSegs(path: []const u8, out: *[10][]const u8) usize {
    var n: usize = 0;
    var start: usize = 0;
    var depth: usize = 0;
    var i: usize = 0;
    while (i <= path.len) : (i += 1) {
        if (i < path.len) {
            const ch = path[i];
            if (ch == '(' or ch == '[' or ch == '{') {
                depth += 1;
                continue;
            }
            if (ch == ')' or ch == ']' or ch == '}') {
                if (depth > 0) depth -= 1;
                continue;
            }
            if (ch != '.' or depth > 0) continue;
        }
        var seg = trimSl(path[start..i]);
        start = i + 1;
        if (std.mem.indexOfScalar(u8, seg, '(')) |lp| seg = trimSl(seg[0..lp]);
        if (seg.len == 0) return 0;
        if (n >= out.len) return 0;
        out[n] = seg;
        n += 1;
    }
    return n;
}

const MemberHit = struct {
    stmt: []const u8, // 声明语句（sigOfStmt / declInitOf 可直接消费）
    owner: []const u8, // 所属容器名；模块函数、别名链上为空串
};

/// 点路径 → 成员声明。逐段下钻：
///   起点：模块绑定（`std` / `const m = module(…)`）或扫描集里的容器
///   中间：容器体 / 模块别名（`pub const map = module("map.zx")`）/
///         类型别名（`pub const Task = task.Task`）
///   末段：声明语句本身
/// KEEP — 用户 2026-11-09 第 14 条：`std.arena.ArenaAllocator.init` 没有
/// 参数名提示、`std.map.HashMap(i32).init` 整条哑掉。旧实现只认
/// `模块.成员`（n ≤ 3、且段里不能带泛型括号），四段以上一律 null。
fn resolveMemberPath(files: []const []const u8, doc: *const Document,
                     text: []const u8, path: []const u8, depth: u8) ?MemberHit {
    if (depth > 4) return null;
    var segs: [10][]const u8 = undefined;
    const n = splitPathSegs(path, &segs);
    if (n < 2) return null;
    var cur: []const u8 = undefined;
    var owner: []const u8 = "";
    if (moduleOfBinding(text, segs[0])) |mod| {
        const p = resolveModuleFile(doc, mod) orelse return null;
        cur = readFileSmall(p) orelse return null;
    } else if (std.mem.eql(u8, segs[0], "std")) {
        const p = resolveModuleFile(doc, "std") orelse return null;
        cur = readFileSmall(p) orelse return null;
    } else if (findContainerBody(files, segs[0])) |ch| {
        cur = ch.body;
        owner = segs[0];
    } else if (genericBodyOf(files, segs[0])) |gb| {
        cur = gb;
        owner = segs[0];
    } else if (isTypeValuedRecv(files, doc, text, segs[0], 0)) {
        // 类型值绑定的首段（`const P: type = t.P1.P2;` 的 P）：P 不是容器
        // 声明，findContainerBody 按名字找不到，`P.P3.f3(…)` 的返回类型也就
        // 推不出（`var f = P.P3.f3(…);` 没有行内提示）。容器沿初值路径取，
        // 体可以在 vendor / 模块文件里。只对类型值开放——实例变量仍走
        // instanceContainerTy，避免把实例当类型。
        const pc = containerOfBinding(files, doc, text, segs[0], 0) orelse return null;
        cur = pc.body;
        owner = pc.name;
    } else return null;

    var at: usize = 1;
    while (at < n) : (at += 1) {
        const seg = segs[at];
        if (findDeclStmt(cur, seg)) |st| {
            if (at + 1 == n) return .{ .stmt = st, .owner = owner };
            if (findContainerBody(&[_][]const u8{cur}, seg)) |ch| {
                cur = ch.body;
                owner = seg;
                continue;
            }
            if (genericBodyOf(&[_][]const u8{cur}, seg)) |gb| {
                cur = gb;
                owner = seg;
                continue;
            }
            if (moduleAliasIn(cur, seg)) |al| {
                const p = resolveModuleFile(doc, al) orelse return null;
                cur = readFileSmall(p) orelse return null;
                owner = "";
                continue;
            }
            // 类型别名：`pub const Task = task.Task` → 沿别名继续下钻
            if (declInitOf(st)) |init0| {
                const t = trimSl(init0);
                if (t.len > 0 and t.len < 100 and
                    std.mem.indexOfScalar(u8, t, '{') == null and
                    std.mem.indexOfScalar(u8, t, '(') == null and
                    std.mem.indexOfScalar(u8, t, '"') == null)
                {
                    var np: std.ArrayList(u8) = .empty;
                    np.appendSlice(gpa, t) catch return null;
                    var rest = at + 1;
                    while (rest < n) : (rest += 1) {
                        np.append(gpa, '.') catch return null;
                        np.appendSlice(gpa, segs[rest]) catch return null;
                    }
                    return resolveMemberPath(files, doc, cur, np.items, depth + 1);
                }
            }
            return null;
        }
        // 当前文件里没有：std 门面文件的成员在模块目录的其它 .zx 里
        if (findContainerBody(files, seg)) |ch| {
            cur = ch.body;
            owner = seg;
            continue;
        }
        if (genericBodyOf(files, seg)) |gb| {
            cur = gb;
            owner = seg;
            continue;
        }
        return null;
    }
    return null;
}

/// 兼容旧调用点：只要声明语句。
fn resolvePathStmt(files: []const []const u8, doc: *const Document, text: []const u8, path: []const u8) ?[]const u8 {
    const hit = resolveMemberPath(files, doc, text, path, 0) orelse return null;
    return hit.stmt;
}

/// 纯点路径（`A.B.C` / `m.A.B`）：每段都是标识符，中间只有 '.'。
/// 只给"逐段下钻容器体"用；带调用/字面量/包装的表达式一律 false。
fn isPurePath(e0: []const u8) bool {
    const e = trimSl(e0);
    if (e.len == 0 or e.len > 120) return false;
    var seg_len: usize = 0;
    for (e) |c| {
        if (isIdentChar(c)) {
            seg_len += 1;
            continue;
        }
        if (c == '.' and seg_len > 0) {
            seg_len = 0;
            continue;
        }
        return false;
    }
    return seg_len > 0;
}

/// 成员访问的接收者起点：从 `.` 往回扫，标识符字符和 `.` 都算，成对的
/// `( … )` 整段也算——`HashMap(i32).init` 的接收者是 `HashMap(i32)`，
/// 泛型实例上的方法/关联常量也该能跳（用户 2026-11-11：类型和函数跳不过去）。
/// 扫到别的字符就停。
fn memberBaseStart(text: []const u8, dot: usize) usize {
    var i = dot;
    while (i > 0) {
        const c = text[i - 1];
        if (isIdentChar(c) or c == '.') {
            i -= 1;
            continue;
        }
        if (c == ')') {
            var d: usize = 0;
            var j = i - 1;
            var found = false;
            while (true) {
                if (text[j] == ')') {
                    d += 1;
                } else if (text[j] == '(') {
                    d -= 1;
                    if (d == 0) {
                        found = true;
                        break;
                    }
                }
                if (j == 0) break;
                j -= 1;
            }
            if (!found) return i;
            i = j;
            continue;
        }
        break;
    }
    return i;
}

/// 去掉尾部的泛型实参：`std.map.HashMap(i32)` → `std.map.HashMap`。
/// 容器名后的括号只可能是实例化实参（Zx 里函数调用不会出现在类型路径上），
/// 所以从尾部一段段剥干净就行，多段嵌套 `A.B(C)(D)` 也覆盖。
fn stripGenericSuffix(e0: []const u8) []const u8 {
    var e = trimSl(e0);
    while (e.len > 1 and e[e.len - 1] == ')') {
        var d: usize = 0;
        var j = e.len;
        var found = false;
        while (j > 0) {
            j -= 1;
            if (e[j] == ')') {
                d += 1;
            } else if (e[j] == '(') {
                d -= 1;
                if (d == 0) {
                    found = true;
                    break;
                }
            }
        }
        if (!found or j == 0) break;
        e = trimSl(e[0..j]);
    }
    return e;
}

/// `A.B.Cfg{ .x = 1 }` → "A.B.Cfg"（'{' 前的纯点路径）；不是具名字面量返回 null。
fn literalPathOf(e0: []const u8) ?[]const u8 {
    const e = trimSl(e0);
    const lb = std.mem.indexOfScalar(u8, e, '{') orelse return null;
    const pre = trimSl(e[0..lb]);
    if (pre.len == 0 or !isPurePath(pre)) return null;
    return pre;
}

/// 实例绑定 → 逐段解析出来的容器：`var z: A.B.C`（声明类型）或
/// `let x = A.B.Cfg{…}`（具名字面量）里的限定路径。单段名走老路（null）。
fn instancePathContainer(files: []const []const u8, doc: *const Document,
                         text: []const u8, name: []const u8) ?PathContainer {
    const st = findDeclStmt(text, name) orelse return null;
    var te: ?[]const u8 = null;
    if (declTyOf(st)) |dt| {
        const t = trimSl(dt);
        // 显式 `: type` 标注只说明"这是类型值"，容器本体在初始式里
        // （`const P: type = t.Root.P1.P2;`）。照抄标注拿到的是 "type"，
        // 单段路径立刻落空 → `P.` 一个成员都补不出来（用户 2026-11-11
        // 截图：第一行 P 有 `: type`，紧跟的 `P.P3` 就断了）。
        if (isPurePath(t) and !std.mem.eql(u8, t, "type")) te = t;
    }
    if (te == null) {
        if (declInitOf(st)) |ini| {
            const t = trimSl(ini);
            if (isPurePath(t) and !std.mem.eql(u8, t, "type")) te = t else if (literalPathOf(ini)) |lp| te = lp;
        }
    }
    const path = te orelse return null;
    if (std.mem.indexOfScalar(u8, path, '.') == null) return null;
    return resolveContainerPath(files, doc, text, path);
}

/// 逐段解析出来的容器：体、声明所在文件文本、容器名、是否 enum。
const PathContainer = struct {
    body: []const u8,
    src: []const u8,
    name: []const u8,
    is_enum: bool,
};

/// 在 `cur` 容器体里找成员 `seg` 的体；`cur` 为空时在整份 src 里按名字找。
/// 关键：有 cur 时必须**只在这个体的顶层**找声明（findMemberStmt 是深度
/// 感知的），否则 `A.B.Cfg` 会先撞上文件里顶层的同名 Cfg。
fn bodyOfMemberDecl(src: []const u8, seg: []const u8, cur: ?[]const u8) ?ContainerHit {
    if (cur) |body| {
        const st = findMemberStmt(body, seg) orelse return null;
        if (findContainerBody(&[_][]const u8{st}, seg)) |h| return h;
        if (genericBodyOf(&[_][]const u8{st}, seg)) |gb| return .{ .body = gb, .is_enum = false };
        return null;
    }
    if (findContainerBody(&[_][]const u8{src}, seg)) |h| return h;
    if (genericBodyOf(&[_][]const u8{src}, seg)) |gb| return .{ .body = gb, .is_enum = false };
    return null;
}

/// 限定路径 → 那一层容器的体。`A.B.C` 的每一段都只在上一层的体里找，
/// 绝不用末段名字在全文件里瞎配：同名嵌套类型（`A.B.Cfg` 与顶层 `Cfg`）
/// 过去会被外层同名的抢走——第二层丢类型、第三层直接空列表
///（用户 2026-11-11："嵌套的 pub 类型导出不全"）。
fn resolveContainerPath(files: []const []const u8, doc: *const Document,
                        text: []const u8, path: []const u8) ?PathContainer {
    return resolveContainerPathD(files, doc, text, path, 0);
}

/// 绑定的声明（类型标注 / 初值）指向哪一层容器。三条路：
///   `var z: A.B.C`          声明类型就是限定路径
///   `const P = t.p1.P2T;`   初值是限定路径（类型值或实例链）
///   `let x = A.B.Cfg{ … }`  具名字面量
/// 都落空再退回单名字推断（`let t = l.Root.make()`）。
/// 这是实例链（`t.p1.Inner.Sub`）能当接收者的入口——此前只认"第一段就是
/// 顶层容器名"，`const P = t.p1.P2T;` 之后的 `P.` 被当成实例，静态成员
/// 与嵌套类型全丢（用户 2026-11-11："变量接收类型时不提示类型"）。
fn containerOfBinding(files: []const []const u8, doc: *const Document,
                      text: []const u8, name: []const u8, depth: u8) ?PathContainer {
    if (depth > 6 or name.len == 0) return null;
    const st = findDeclStmt(text, name) orelse {
        // 形参 / 字段不是 const 声明（`t: *T` 里的 `t`），findDeclStmt 够不着，
        // 靠 `name: Type` 形状扫兜住。少了这条，函数体里
        // `const P = t.P1.P2;` 的 P 就成不了类型值：`Pp = P.P3` 不提示
        // `: type`，`P.` 补出来的是全文标识符（用户 2026-11-11 截图 1/2）。
        const pt = namedTypeNear(text, name, text.len) orelse return null;
        const t = stripPtrPrefix(trimSl(pt));
        if (!isPurePath(t)) return null;
        return resolveContainerPathD(files, doc, text, t, depth + 1);
    };
    if (declTyOf(st)) |dt| {
        const t = trimSl(dt);
        if (!std.mem.eql(u8, t, "type") and isPurePath(t) and
            std.mem.indexOfScalar(u8, t, '.') != null)
        {
            if (resolveContainerPathD(files, doc, text, t, depth + 1)) |pc| return pc;
        }
    }
    const init = trimSl(declInitOf(st) orelse "");
    if (init.len > 0) {
        if (isPurePath(init) and std.mem.indexOfScalar(u8, init, '.') != null) {
            if (resolveContainerPathD(files, doc, text, init, depth + 1)) |pc| return pc;
            // 退路：链的源头解析不到（`t` 的模块文件不在扫描集里）时按末段
            // 名字找容器——否则 `const P: type = t.P1.P2;` 的 P 连容器都没有。
            if (resolveContainerPathLoose(files, doc, text, init)) |pc| return pc;
        }
        if (literalPathOf(init)) |lp| {
            if (resolveContainerPathD(files, doc, text, lp, depth + 1)) |pc| return pc;
        }
        if (inferExprType(text, doc, init, 1)) |ty0| {
            const ty = containerNameOf(ty0);
            if (ty.len > 0) {
                if (findContainerBody(files, ty)) |h| return .{
                    .body = h.body,
                    .src = containerSrcOf(files, ty) orelse text,
                    .name = ty,
                    .is_enum = h.is_enum,
                };
                if (genericBodyOf(files, ty)) |gb| return .{
                    .body = gb,
                    .src = containerSrcOf(files, ty) orelse text,
                    .name = ty,
                    .is_enum = false,
                };
            }
        }
    }
    return null;
}

/// [start, end) 里的顶层逗号段（字符串感知、括号配平），最多 16 段。
fn splitTopSegs(txt: []const u8, start: usize, end: usize, out: *[16][]const u8) usize {
    var n: usize = 0;
    var d: usize = 0;
    var in_str = false;
    var seg = start;
    var i = start;
    while (i < end) : (i += 1) {
        const ch = txt[i];
        if (in_str) {
            if (ch == '\\') { i += 1; continue; }
            if (ch == '"') in_str = false;
            continue;
        }
        if (ch == '"') { in_str = true; continue; }
        if (ch == '(' or ch == '[' or ch == '{') { d += 1; continue; }
        if (ch == ')' or ch == ']' or ch == '}') { if (d > 0) d -= 1; continue; }
        if (ch == ',' and d == 0) {
            if (n < 16) out[n] = trimSl(txt[seg..i]);
            n += 1;
            seg = i + 1;
        }
    }
    if (end > seg) {
        if (n < 16) out[n] = trimSl(txt[seg..end]);
        n += 1;
    }
    return n;
}

/// '(' 在 lp 的配平 ')'
fn matchParen(txt: []const u8, lp: usize) ?usize {
    var d: usize = 0;
    var i = lp;
    while (i < txt.len) : (i += 1) {
        if (txt[i] == '(') d += 1
        else if (txt[i] == ')') {
            d -= 1;
            if (d == 0) return i;
        }
    }
    return null;
}

/// 泛型形参接收者（§117 的 LSP 半边，用户 2026-11-11 第 9 张截图）：
///
///   const fill = fn (T: type, val: T) void {
///       let v = val.a;      // ← 定义处根本不知道 T 是什么
///   };
///   fill(P3, p);            // ← 调用点才知道：T = P3
///
/// `val.` / `val.a` 过去一个成员都推不出。这里从扫描集里的**调用点**反推：
/// 找到形参 `val: T`（T 是同一张形参表里的 `T: type`），再扫 `fill(…)` 的
/// 调用点，取 **T 那一位**的实参（不是 val 那位——那是值）解析成容器。
/// `off` 是光标偏移（补全用：确认 recv 在这个函数体里）；0 表示没有位置
/// 信息（inferExprType 用），取第一份匹配的声明。
/// 找不到调用点返回 null——列表保持空，绝不回落全局标识符噪音。
fn genericParamRecvTy(files: []const []const u8, doc: *const Document,
                      text: []const u8, recv: []const u8, off: usize) ?PathContainer {
    if (recv.len == 0) return null;
    var from: usize = 0;
    while (std.mem.indexOfPos(u8, text, from, recv)) |c| {
        from = c + recv.len;
        // 词边界：排除 self.val / valX
        if (c > 0 and (isIdentChar(text[c - 1]) or text[c - 1] == '.')) continue;
        var j = c + recv.len;
        if (j < text.len and isIdentChar(text[j])) continue;
        while (j < text.len and (text[j] == ' ' or text[j] == '\t')) j += 1;
        if (j >= text.len or text[j] != ':') continue;   // 只要 `val:` 形参位
        j += 1;
        while (j < text.len and (text[j] == ' ' or text[j] == '\t')) j += 1;
        // 形参类型读到顶层分隔符（同 namedTypeNear 的截法）
        var depth: usize = 0;
        var e = j;
        while (e < text.len) : (e += 1) {
            const ch = text[e];
            if (ch == '(' or ch == '[' or ch == '{') { depth += 1; continue; }
            if (ch == ')' or ch == ']' or ch == '}') {
                if (depth == 0) break;
                depth -= 1;
                continue;
            }
            if (depth == 0 and (ch == ',' or ch == ';' or ch == '=')) break;
        }
        const pty = trimSl(text[j..e]);
        if (!isPurePath(pty)) continue;                  // T 是裸名字
        if (std.mem.eql(u8, pty, "type")) continue;      // `val: type` 本身
        // 形参表：向左最近未配平的 '('，且 '(' 前紧贴 `fn`
        var d2: usize = 0;
        var lp: ?usize = null;
        var w = j;
        while (w > 0) {
            w -= 1;
            const ch = text[w];
            if (ch == ')' or ch == ']' or ch == '}') d2 += 1
            else if (ch == '(') {
                if (d2 == 0) { lp = w; break; }
                d2 -= 1;
            }
        }
        const lpx = lp orelse continue;
        var e2 = lpx;
        while (e2 > 0 and (text[e2 - 1] == ' ' or text[e2 - 1] == '\t')) e2 -= 1;
        if (e2 < 2 or !std.mem.eql(u8, text[e2 - 2 .. e2], "fn")) continue;
        const rpx = matchParen(text, lpx) orelse continue;
        // T 是这张表里第几个形参（`T: type` 的位置）
        var psegs: [16][]const u8 = undefined;
        const np = splitTopSegs(text, lpx + 1, rpx, &psegs);
        var targi: ?usize = null;
        for (psegs[0..np], 0..) |sg, si| {
            if (sg.len > pty.len and std.mem.startsWith(u8, sg, pty)) {
                var q = pty.len;
                while (q < sg.len and (sg[q] == ' ' or sg[q] == '\t')) q += 1;
                if (q < sg.len and sg[q] == ':' and
                    std.mem.startsWith(u8, trimSl(sg[q + 1 ..]), "type"))
                    targi = si;
            }
        }
        const tpos = targi orelse continue;              // 不是类型形参的值
        // off 有值时：光标必须在这个 fn 的体里（体从签名 ')' 后第一个 '{' 起）
        if (off > 0) {
            const br = fnBodyRange(text, rpx) orelse continue;
            if (off < br.start or off > br.end) continue;
        }
        if (callSiteArgTy(files, doc, text, fnameOfFnHead(text, e2), tpos, lpx)) |pc| return pc;
    }
    return null;
}

/// fn 头的名字：`fn name(` 具名取 name；`fn (` 匿名（`const fill = fn (`）
/// 往前找 `const`/`let`/`var` 取绑定名。找不到返回空。
fn fnameOfFnHead(text: []const u8, fn_end: usize) []const u8 {
    var q = fn_end;
    while (q < text.len and (text[q] == ' ' or text[q] == '\t')) q += 1;
    if (q < text.len and isIdentChar(text[q])) {
        var fe = q;
        while (fe < text.len and isIdentChar(text[fe])) fe += 1;
        if (fe > q) return text[q..fe];
    }
    var b = fn_end - 2;
    var guard: usize = 0;
    while (b > 0 and guard < 400) : (guard += 1) {
        b -= 1;
        if (std.mem.startsWith(u8, text[b..], "const ") or
            std.mem.startsWith(u8, text[b..], "let ") or
            std.mem.startsWith(u8, text[b..], "var ")) break;
    }
    if (guard < 400 and b > 0) {
        var ns = b;
        while (ns < text.len and text[ns] != ' ') ns += 1;
        while (ns < text.len and text[ns] == ' ') ns += 1;
        var ne = ns;
        while (ne < text.len and isIdentChar(text[ne])) ne += 1;
        if (ne > ns) return text[ns..ne];
    }
    return "";
}

/// 签名 ')' 在 rpx 的 fn：')' 之后（跳过返回类型）第一个 '{' 起的平衡体
fn fnBodyRange(text: []const u8, rpx: usize) ?struct { start: usize, end: usize } {
    var lb: ?usize = null;
    var i = rpx + 1;
    while (i < text.len) : (i += 1) {
        if (text[i] == '{') { lb = i; break; }
        if (text[i] == ';') return null;   // 还没 '{' 就出语句了：不是 fn 头
    }
    const lbx = lb orelse return null;
    var depth: usize = 0;
    var in_str = false;
    var k = lbx;
    while (k < text.len) : (k += 1) {
        const ch = text[k];
        if (in_str) {
            if (ch == '\\') { k += 1; continue; }
            if (ch == '"') in_str = false;
            continue;
        }
        if (ch == '"') { in_str = true; continue; }
        if (ch == '{') depth += 1;
        if (ch == '}') {
            depth -= 1;
            if (depth == 0) return .{ .start = lbx, .end = k };
        }
    }
    return .{ .start = lbx, .end = text.len };  // 未闭合（正在打字）
}

/// `fname(…)` 的调用点 → 同位实参的类型路径解析成容器。
/// decl_lp 是声明自己的签名 '('（跳过它）。先扫 files（文档+同目录+std），
/// 落空再扫模块目录（vendor / zx.json deps / 工程根，跳 std，≤240 文件），
/// 与编译器 module_load 同一条链。
fn callSiteArgTy(files: []const []const u8, doc: *const Document,
                 text: []const u8, fname: []const u8, targi: usize, decl_lp: usize) ?PathContainer {
    if (fname.len == 0) return null;
    for (files) |ftxt| {
        var from: usize = 0;
        while (std.mem.indexOfPos(u8, ftxt, from, fname)) |c| {
            from = c + fname.len;
            if (c > 0 and isIdentChar(ftxt[c - 1])) continue;
            var j = c + fname.len;
            while (j < ftxt.len and (ftxt[j] == ' ' or ftxt[j] == '\t')) j += 1;
            if (j >= ftxt.len or ftxt[j] != '(') continue;
            if (ftxt.ptr == text.ptr and j == decl_lp) continue;   // 自己的签名
            if (callSiteArgOnce(files, doc, text, ftxt, j, targi)) |pc| return pc;
        }
    }
    // 模块目录兜底：vendor / deps / 工程根里的调用点
    const doc_dir = dirName(doc.path);
    var mdirs: std.ArrayList([]const u8) = .empty;
    defer mdirs.deinit(gpa);
    modDirsInto(doc_dir, &mdirs);
    var nfiles: usize = 0;
    for (mdirs.items) |md2| {
        if (g_std_dir) |sd2| {
            if (std.mem.eql(u8, md2, sd2)) continue;
        }
        if (std.mem.eql(u8, md2, doc_dir)) continue;
        var d9 = Io.Dir.cwd().openDir(g_io, md2, .{ .iterate = true }) catch continue;
        defer d9.close(g_io);
        var it9 = d9.iterate();
        while (it9.next(g_io) catch null) |e9| {
            if (e9.kind != .file or !std.mem.endsWith(u8, e9.name, ".zx")) continue;
            if (nfiles >= 240) return null;
            nfiles += 1;
            var fp9: std.ArrayList(u8) = .empty;
            defer fp9.deinit(gpa);
            fp9.appendSlice(gpa, md2) catch continue;
            fp9.append(gpa, '/') catch continue;
            fp9.appendSlice(gpa, e9.name) catch continue;
            const t9 = readFileSmall(fp9.items) orelse continue;
            var from: usize = 0;
            while (std.mem.indexOfPos(u8, t9, from, fname)) |c| {
                from = c + fname.len;
                if (c > 0 and isIdentChar(t9[c - 1])) continue;
                var j = c + fname.len;
                while (j < t9.len and (t9[j] == ' ' or t9[j] == '\t')) j += 1;
                if (j >= t9.len or t9[j] != '(') continue;
                if (callSiteArgOnce(files, doc, text, t9, j, targi)) |pc| return pc;
            }
        }
    }
    return null;
}

fn callSiteArgOnce(files: []const []const u8, doc: *const Document,
                   text: []const u8, ftxt: []const u8, lp: usize, targi: usize) ?PathContainer {
    const rpx = matchParen(ftxt, lp) orelse return null;
    var asegs: [16][]const u8 = undefined;
    const na = splitTopSegs(ftxt, lp + 1, rpx, &asegs);
    if (targi >= na) return null;
    const arg = asegs[targi];
    // 只要纯类型路径：`P3` / `vp.Cfg` / `m.A.B`。表达式（`T` 之外的变量、
    // 字面量、调用）不认识就放弃——宁缺毋滥。
    if (!isPurePath(arg)) return null;
    if (resolveContainerPathD(files, doc, text, arg, 0)) |pc| return pc;
    // 退路：调用点在别的文件、类型在那里定义。按末段名字找容器。
    var last = arg;
    if (std.mem.lastIndexOfScalar(u8, arg, '.')) |d3| last = arg[d3 + 1 ..];
    if (findContainerBody(files, last)) |h| return .{
        .body = h.body,
        .src = containerSrcOf(files, last) orelse text,
        .name = last,
        .is_enum = h.is_enum,
    };
    if (genericBodyOf(files, last)) |gb| return .{
        .body = gb,
        .src = containerSrcOf(files, last) orelse text,
        .name = last,
        .is_enum = false,
    };
    return null;
}

fn resolveContainerPathD(files: []const []const u8, doc: *const Document,
                         text: []const u8, path: []const u8, depth: u8) ?PathContainer {
    if (depth > 6) return null;
    if (!isPurePath(path)) return null;
    var segs: [10][]const u8 = undefined;
    const n = splitPathSegs(path, &segs);
    if (n == 0) return null;
    var src: []const u8 = text;
    var at: usize = 0;
    if (n > 1) {
        if (moduleOfBinding(text, segs[0])) |mod| {
            const p = resolveModuleFile(doc, mod) orelse return null;
            src = readFileSmall(p) orelse return null;
            at = 1;
        } else if (std.mem.eql(u8, segs[0], "std")) {
            const p = resolveModuleFile(doc, "std") orelse return null;
            src = readFileSmall(p) orelse return null;
            at = 1;
        }
    }
    var hit: ?ContainerHit = null;
    var name: []const u8 = "";
    if (at == 0) {
        // 起点：**先看本文档里的绑定**（`const P = t.P1.P2;` 的 P），再看容器声明。
        // 顺序不能反：先按名字找容器，同目录里恰好有个同名容器（测试 fixture
        // 尤其容易撞）就会把用户的绑定顶掉，`P.` 直接补不出成员。
        // 只对多段路径开放——单段名字的实例补全走 instanceContainerTy，
        // 这里放进来会让"按名字在全扫描集里找容器"的老毛病回来。
        if (n > 1) {
            if (containerOfBinding(files, doc, text, segs[0], depth + 1)) |pc| {
                hit = .{ .body = pc.body, .is_enum = pc.is_enum };
                src = pc.src;
                name = pc.name;
                at = 1;
            }
        }
        if (hit == null) {
            if (findContainerBody(files, segs[0])) |h| {
                hit = h;
                src = containerSrcOf(files, segs[0]) orelse text;
            } else if (genericBodyOf(files, segs[0])) |gb| {
                hit = .{ .body = gb, .is_enum = false };
                src = containerSrcOf(files, segs[0]) orelse text;
            }
        }
        if (name.len == 0) name = segs[0];
        if (at == 0) at = 1;
    } else {
        // 起始模块之后的第一段才是容器名（segs[0] 是绑定名 m / std）。
        // 那一段也可能是个**再导出的子模块**（std.zx 的
        // `pub const json = module("json.zx")`）：这时先把它当模块下钻一层，
        // 再在子模块文件里找容器。
        //
        // 必须这样走——以前退化成"拿末段名字在全扫描集里找 `const X = struct`"，
        // 只要同目录的 fixture 里有同名容器（lsptest 的 snipprobe.zx 就定义了一个
        // 私有的 `Obj`），`std.json.Obj.` 就弹那一个的成员（用户 2026-11-11 看到
        // 的"补全忽然空了"就是这么来的）。
        while (at < n) {
            if (bodyOfMemberDecl(src, segs[at], null)) |h| {
                hit = h;
                name = segs[at];
                at += 1;
                break;
            }
            var pbuf: std.ArrayList(u8) = .empty;
            defer pbuf.deinit(gpa);
            for (segs[0 .. at + 1], 0..) |sg, si| {
                if (si > 0) pbuf.append(gpa, '.') catch break;
                pbuf.appendSlice(gpa, sg) catch break;
            }
            const mp = qualModulePath(doc, text, pbuf.items) orelse break;
            src = readFileSmall(mp) orelse break;
            at += 1;
        }
        if (hit == null) return null;
    }
    var cur: []const u8 = (hit orelse return null).body;
    var is_enum = (hit orelse return null).is_enum;
    while (at < n) : (at += 1) {
        const seg = segs[at];
        var st: ?[]const u8 = null;
        var h2: ?ContainerHit = null;
        if (findMemberStmt(cur, seg)) |s| {
            st = s;
            if (findContainerBody(&[_][]const u8{s}, seg)) |h| h2 = h
            else if (genericBodyOf(&[_][]const u8{s}, seg)) |gb| h2 = .{ .body = gb, .is_enum = false };
        }
        if (h2 == null) {
            // 类型别名：`pub const Entry = Base;`（同层优先）或限定路径
            if (st) |s| if (declInitOf(s)) |ini| {
                const t = trimSl(ini);
                if (isPurePath(t)) {
                    if (std.mem.indexOfScalar(u8, t, '.') == null) {
                        if (findMemberStmt(cur, t)) |s2| {
                            if (findContainerBody(&[_][]const u8{s2}, t)) |h| h2 = h
                            else if (genericBodyOf(&[_][]const u8{s2}, t)) |gb| h2 = .{ .body = gb, .is_enum = false };
                        }
                    }
                    if (h2 == null) {
                        if (resolveContainerPathD(files, doc, src, t, depth + 1)) |pc| {
                            cur = pc.body;
                            src = pc.src;
                            name = pc.name;
                            is_enum = pc.is_enum;
                            continue;
                        }
                    }
                }
            };
        }
        if (h2 == null) {
            // 字段段：`t.p1.P2T` 的 `p1` 是字段（不是容器），类型 `P1T`
            // 才是下一层容器。少了这条，实例链在第一个字段处就断了。
            if (fieldTypeIn(cur, seg)) |ft0| {
                const ft = trimSl(ft0);
                if (isPurePath(ft)) {
                    if (resolveContainerPathD(files, doc, src, ft, depth + 1)) |pc| {
                        cur = pc.body;
                        src = pc.src;
                        name = pc.name;
                        is_enum = pc.is_enum;
                        continue;
                    }
                }
            }
        }
        const h3 = h2 orelse return null;
        cur = h3.body;
        is_enum = h3.is_enum;
        name = seg;
    }
    return .{ .body = cur, .src = src, .name = name, .is_enum = is_enum };
}


/// 严格路径解析失败时的退路：把开头的段一段段丢掉再试，最后按**末段名字**
/// 在全部文件里找容器（`t.P1.P2` → `P1.P2` → `P2`）。
///
/// 只在"接收者已确定是类型值"时用（标注写了 `type`、初值是容器路径）：那时
/// 末段名字必定是容器名，解析不到多半是链的**源头**断了——模块文件没进扫描
/// 集、`t` 是别处模块里的形参等。用户 2026-11-11：`const P: type = t.P1.P2;`
/// 之后 `const pp = P.P3;` 既不提示 `: type`、`pp.` 没有成员、`P.P3` 也跳不
/// 过去（探针里同一段代码能过，说明差在链源头）。
/// 代价：极少数同名嵌套容器会被第一个同名的抢走——所以只做最后一步，严格
/// 路径能解析时绝不走这里。
fn resolveContainerPathLoose(files: []const []const u8, doc: *const Document,
                             text: []const u8, path: []const u8) ?PathContainer {
    if (resolveContainerPath(files, doc, text, path)) |pc| return pc;
    if (!isPurePath(path)) return null;
    var segs: [10][]const u8 = undefined;
    const n = splitPathSegs(path, &segs);
    if (n < 2) return null;
    var k: usize = 1;
    while (k < n) : (k += 1) {
        var buf: std.ArrayList(u8) = .empty;
        var si = k;
        while (si < n) : (si += 1) {
            if (si > k) buf.append(gpa, '.') catch break;
            buf.appendSlice(gpa, segs[si]) catch break;
        }
        const suffix = buf.items;
        if (suffix.len > 0) {
            if (resolveContainerPath(files, doc, text, suffix)) |pc| {
                // pc.name 指向上面的 suffix 缓冲，随 buf 一起释放——不能直接
                // 返回，否则调用方拿到的容器名是回收后的字节，
                // `findContainerBody` 必然落空（这里踩过一次：打印出乱码、
                // `pp` 就是没有 `: type`）。容器名本来就是**末段**，而 segs
                // 指向调用方的 path，直接用它，不做分配。
                const last_seg = segs[n - 1];
                buf.deinit(gpa);
                return .{ .body = pc.body, .src = pc.src, .name = last_seg, .is_enum = pc.is_enum };
            }
        }
        if (n - k == 1) {
            const nm = segs[n - 1];
            const src = containerSrcOf(files, nm) orelse text;
            if (findContainerBody(files, nm)) |h| {
                buf.deinit(gpa);
                return .{ .body = h.body, .src = src, .name = nm, .is_enum = h.is_enum };
            }
            if (genericBodyOf(files, nm)) |gb| {
                buf.deinit(gpa);
                return .{ .body = gb, .src = src, .name = nm, .is_enum = false };
            }
        }
        buf.deinit(gpa);
    }
    return null;
}


/// 容器体里的字段类型：`age:i32,` → "i32"（容器内深度 0，名字后跟冒号）。
/// body 以 '{' 开头，与 extractBraceMembers 同约定：首字符不入深度。
fn fieldTypeIn(body: []const u8, name: []const u8) ?[]const u8 {
    var depth: usize = 0;
    var i: usize = 1; // body[0] == '{'
    while (i < body.len) : (i += 1) {
        const c = body[i];
        if (c == '"') {
            i += 1;
            while (i < body.len and body[i] != '"' and body[i] != '\n') i += 1;
            continue;
        }
        if (c == '/' and i + 1 < body.len and body[i + 1] == '/') {
            while (i < body.len and body[i] != '\n') i += 1;
            continue;
        }
        if (c == '(' or c == '{' or c == '[') {
            depth += 1;
            continue;
        }
        if (c == ')') {
            depth -= 1;
            continue;
        }
        if (c == ']' or c == '}') {
            if (depth == 0) return null; // 容器体结束
            depth -= 1;
            continue;
        }
        if (depth == 0 and isIdentChar(c) and !(c >= '0' and c <= '9')) {
            var j = i + 1;
            while (j < body.len and isIdentChar(body[j])) j += 1;
            if (std.mem.eql(u8, body[i..j], name)) {
                var k = j;
                while (k < body.len and (body[k] == ' ' or body[k] == '\t')) k += 1;
                if (k < body.len and body[k] == ':') {
                    k += 1;
                    // 嵌套内联 struct 的逗号不能截断类型（`inner: struct { a: i32, b: u8 }`）
                    const ty = readFieldTy(body, k);
                    if (ty.len > 0) return ty;
                }
            }
            i = j - 1;
        }
    }
    return null;
}

/// 裸标识符的 `name: Type` 形状扫描：函数形参（含 self）、struct/union 字段、
/// 匿名 struct 字段。findDeclStmt 够不着的绑定（形参/字段不是 const 声明）归这里。
/// 同名多处时取 off 之前最近的一份（作用域遮蔽近似），前面没有就取全文第一份。
/// 返回类型切片；识别不了返回 null。
fn namedTypeNear(text: []const u8, name: []const u8, off: usize) ?[]const u8 {
    var prev: ?[]const u8 = null;
    var first: ?[]const u8 = null;
    var from: usize = 0;
    while (std.mem.indexOfPos(u8, text, from, name)) |c| {
        from = c + 1;
        // 词边界：排除 self.age 里的 age、abc 里的 ab
        if (c > 0 and (isIdentChar(text[c - 1]) or text[c - 1] == '.')) continue;
        var j = c + name.len;
        if (j < text.len and isIdentChar(text[j])) continue;
        while (j < text.len and (text[j] == ' ' or text[j] == '\t')) j += 1;
        if (j >= text.len or text[j] != ':') continue;
        j += 1;
        while (j < text.len and (text[j] == ' ' or text[j] == '\t')) j += 1;
        // 类型读到顶层分隔符：, ) ] } ; =（括号/方括号计入深度，覆盖
        // [n]i32、{f64, f64} 这类自带括号的类型；'=' 截掉字段默认值）
        var depth: usize = 0;
        var in_str = false;
        var e = j;
        while (e < text.len) : (e += 1) {
            const ch = text[e];
            if (in_str) {
                if (ch == '\\') {
                    e += 1;
                    continue;
                }
                if (ch == '"') in_str = false;
                continue;
            }
            if (ch == '"') {
                in_str = true;
                continue;
            }
            if (ch == '(' or ch == '[' or ch == '{') {
                depth += 1;
                continue;
            }
            if (ch == ')' or ch == ']' or ch == '}') {
                if (depth == 0) break;
                depth -= 1;
                continue;
            }
            if (depth == 0 and (ch == ',' or ch == ';' or ch == '=')) break;
        }
        const ty = trimSl(text[j..e]);
        if (ty.len == 0) continue;
        if (first == null) first = ty;
        if (c < off) prev = ty;
    }
    return prev orelse first;
}

/// 去掉类型前的指针与修饰：`*T` / `*const T` / `*var T` / `**T` → `T`。
/// 形参几乎都写成 `t: *T`（实例方法还有 `*const Self`），解析容器前必须剥掉，
/// 否则 `isPurePath` 直接判否、整条链断在第一个标识符上。
fn stripPtrPrefix(e0: []const u8) []const u8 {
    var e = trimSl(e0);
    var guard: usize = 0;
    while (guard < 4) : (guard += 1) {
        if (e.len == 0 or e[0] != '*') break;
        e = trimSl(e[1..]);
        if (std.mem.startsWith(u8, e, "const ")) {
            e = trimSl(e["const ".len..]);
        } else if (std.mem.startsWith(u8, e, "var ")) {
            e = trimSl(e["var ".len..]);
        }
    }
    return e;
}

/// 方法体内的 self → 容器体：扫全文的容器声明（const X = struct/union {…}），
/// 取声明了 `self:` 形参的那个（zx 的 Self 别名指外围容器，文本扫描拿不到
/// 词法作用域，多容器都带 self 方法时取第一份的近似）。
fn selfContainerBody(text: []const u8) ?[]const u8 {
    var from: usize = 0;
    while (std.mem.indexOfPos(u8, text, from, "const ")) |c| {
        from = c + 1;
        var j = c + 6;
        while (j < text.len and text[j] == ' ') j += 1;
        const ns = j;
        while (j < text.len and isIdentChar(text[j])) j += 1;
        if (j == ns) continue;
        while (j < text.len and (text[j] == ' ' or text[j] == '\t')) j += 1;
        if (j >= text.len or text[j] != '=') continue; // const X = struct/union …
        j += 1;
        while (j < text.len and (text[j] == ' ' or text[j] == '\t')) j += 1;
        // 容器关键字：struct / union（enum 没有 self 方法，略过）
        var is_container = false;
        if (std.mem.startsWith(u8, text[j..], "struct") or std.mem.startsWith(u8, text[j..], "union")) {
            is_container = true;
            while (j < text.len and isIdentChar(text[j])) j += 1;
        }
        if (!is_container) continue;
        while (j < text.len and (text[j] == ' ' or text[j] == '\t')) j += 1;
        if (j >= text.len or text[j] != '{') continue;
        const body_start = j;
        // 平衡大括号找容器体结束
        var depth: usize = 0;
        var in_str = false;
        var k = j;
        while (k < text.len) : (k += 1) {
            const ch = text[k];
            if (in_str) {
                if (ch == '\\') {
                    k += 1;
                    continue;
                }
                if (ch == '"') in_str = false;
                continue;
            }
            if (ch == '"') {
                in_str = true;
                continue;
            }
            if (ch == '{') depth += 1;
            if (ch == '}') {
                depth -= 1;
                if (depth == 0) break;
            }
        }
        if (k >= text.len) continue;
        const body = text[body_start .. k + 1];
        // `self:` 形参（fn (self: *Self …)）才算有方法
        var m: usize = 1;
        while (m + 5 < body.len) : (m += 1) {
            if (std.mem.eql(u8, body[m .. m + 5], "self:") and
                !isIdentChar(body[m - 1]) and body[m - 1] != '.') return body;
        }
    }
    return null;
}

fn extractBraceMembers(text: []const u8, is_enum: bool, out: *std.ArrayList(Decl)) void {
    var depth: usize = 0;
    var d_par: usize = 0; // 圆括号深度：方法签名 fn (f: …, ctx: …) 里的参数名
    // 形状与字段一致，括号内一律不收，否则混进成员列表。
    var i: usize = 1; // text[0] == '{'
    while (i < text.len) : (i += 1) {
        const c = text[i];
        // 正向跳过 // 注释行与字符串——注释里的同名标识符绝不能当成员
        if (c == '/' and i + 1 < text.len and text[i + 1] == '/') {
            while (i < text.len and text[i] != '\n') i += 1;
            continue;
        }
        if (c == '"') {
            i += 1;
            while (i < text.len and text[i] != '"' and text[i] != '\n') i += 1;
            continue;
        }
        if (c == '(') d_par += 1;
        if (c == ')' and d_par > 0) d_par -= 1;
        if (c == '{') depth += 1;
        if (c == '}') {
            if (depth == 0) return;
            depth -= 1;
        }
        if (depth == 0 and d_par == 0 and isIdentChar(c) and !(c >= '0' and c <= '9')) {
            var j = i + 1;
            while (j < text.len and isIdentChar(text[j])) j += 1;
            var k = j;
            while (k < text.len and (text[k] == ' ' or text[k] == '\t')) k += 1;
            var kind: ?DeclKind = null;
            if (!is_enum and k < text.len and text[k] == ':') kind = .field;
            // `name = fn/struct/...`：方法与关联常量（Zx 的 struct 方法都这么写）
            if (!is_enum and k < text.len and text[k] == '=') kind = .value;
            if (is_enum and k < text.len and (text[k] == ',' or text[k] == '}' or text[k] == '=' or text[k] == ':'))
                kind = .variant;
            if (kind) |kd| {
                // 前一个有效字符须是块起点/逗号/换行，避免收进类型里的 ident；
                // 回扫跳空白、整行 // 注释，以及 const / pub const 前缀（方法声明）。
                var b = i;
                var skipped = true;
                while (skipped) {
                    skipped = false;
                    while (b > 1) {
                        const c2 = text[b - 1];
                        if (c2 == ' ' or c2 == '\t' or c2 == '\n' or c2 == '\r') {
                            b -= 1;
                            continue;
                        }
                        // 落点所在行若是 // 注释行，整行跳过
                        var ls = b - 1;
                        while (ls > 0 and text[ls - 1] != '\n') ls -= 1;
                        var t = ls;
                        while (t < text.len and (text[t] == ' ' or text[t] == '\t')) t += 1;
                        if (t + 1 < b and text[t] == '/' and text[t + 1] == '/') {
                            b = ls;
                            continue;
                        }
                        // 行尾注释（`mtime: u64,   // 注释`）同理：注释文字会被
                        // 当成"前一个有效字符"，紧随其后的字段被判成
                        // "前面不是分隔符"而丢掉——Stat.isDir / DateTime.hour
                        // 就这么消失过。整行当作换行继续回扫上一行。
                        if (std.mem.indexOf(u8, text[ls..b], "//") != null) {
                            b = ls;
                            continue;
                        }
                        break;
                    }
                    if (b >= 5 and std.mem.eql(u8, text[b - 5 .. b], "const")) {
                        b -= 5;
                        skipped = true;
                    } else if (b >= 3 and std.mem.eql(u8, text[b - 3 .. b], "pub")) {
                        b -= 3;
                        skipped = true;
                    }
                }
                const prev: u8 = if (b >= 2) text[b - 1] else '{';
                if (prev == '{' or prev == ',' or prev == '\n' or prev == ';') {
                    // 字段顺手记下声明类型：补全时 detail 显示 `i32` 而不是容器名
                    var ty: []const u8 = "";
                    if (kd == .field and k < text.len and text[k] == ':') ty = readFieldTy(text, k + 1);
                    out.append(gpa, .{ .name = text[i..j], .kind = kd, .ty = ty }) catch {};
                }
            }
            i = j - 1;
        }
    }
}

// ---------------------------------------------------------------- 匿名类型线
//
// KEEP — 用户 2026-11-08：提示/补全/snippet 要覆盖匿名类型、匿名参数、嵌套
// 匿名类型。inferExprType 把匿名字面量拼成类型串（struct{x:i32} / {i32,f64}），
// 下面一组函数负责把类型串拆回字段、沿点路径下钻、在调用点按**实参位置**取
// 匿名 struct 形参——匿名绑定 `anon.`、嵌套 `nv.pos.`、形参 `info.`、嵌套实参
// `deep(.{ .opts = .{ .` 全走这一条链。lsptest.py 3v/3w/3x/3y/3z 是守门。
// （旧实现 anonFieldOne/anonFieldsOf/anonStructFieldsOf 只认第一个 struct 形参、
// 不能下钻，已被这组替代。）

/// '{' 的配对 '}'（跳过字符串）。找不到返回 null。
fn matchBrace(text: []const u8, lb: usize) ?usize {
    var d: usize = 0;
    var in_str = false;
    var i = lb;
    while (i < text.len) : (i += 1) {
        const c = text[i];
        if (in_str) {
            if (c == '\\') { i += 1; continue; }
            if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; continue; }
        if (c == '{') d += 1;
        if (c == '}') {
            d -= 1;
            if (d == 0) return i;
        }
    }
    return null;
}

const AnonField = struct { name: []const u8, ty: []const u8 };

/// inferExprType 拼出的匿名类型串 → 字段表：
///   `struct{x:i32,y:struct{a:u8}}` 匿名结构体（嵌套类型完整保留）
///   `{i32,f64}`                    元组（字段名为空）
/// 切片指向 ty（最终指向文档文本），随请求生命周期有效。
fn parseAnonTy(ty: []const u8) ?std.ArrayList(AnonField) {
    const s = trimSl(ty);
    var inner: []const u8 = "";
    var tuple = false;
    // `struct{x:i32}`（inferExprType 拼的）与 `struct { x: i32 }`（声明原文）
    // 都要认——模块容器字段类型来自源码，带空格（用户 2026-11-08 第 3 条）。
    if (std.mem.startsWith(u8, s, "struct")) {
        var j: usize = "struct".len;
        while (j < s.len and (s[j] == ' ' or s[j] == '\t' or s[j] == '\n' or s[j] == '\r')) j += 1;
        if (j >= s.len or s[j] != '{' or s[s.len - 1] != '}') return null;
        inner = s[j + 1 .. s.len - 1];
    } else if (s.len >= 2 and s[0] == '{' and s[s.len - 1] == '}') {
        inner = s[1 .. s.len - 1];
        tuple = true;
    } else return null;
    var out: std.ArrayList(AnonField) = .empty;
    var d: usize = 0;
    var in_str = false;
    var start: usize = 0;
    var i: usize = 0;
    while (i <= inner.len) : (i += 1) {
        const at_end = i == inner.len;
        if (!at_end) {
            const c = inner[i];
            if (in_str) {
                if (c == '\\') { i += 1; continue; }
                if (c == '"') in_str = false;
                continue;
            }
            if (c == '"') { in_str = true; continue; }
            if (c == '{' or c == '(' or c == '[') { d += 1; continue; }
            if (c == '}' or c == ')' or c == ']') { if (d > 0) d -= 1; continue; }
            if (c != ',' or d != 0) continue;
        }
        const seg = trimSl(inner[start..i]);
        start = i + 1;
        if (seg.len == 0) continue;
        if (tuple) {
            out.append(gpa, .{ .name = "", .ty = seg }) catch {};
            continue;
        }
        // name: ty——冒号取深度 0 的第一个（嵌套 struct{...} 里的冒号不算）
        var d2: usize = 0;
        var colon: ?usize = null;
        var s2 = false;
        for (seg, 0..) |c2, k2| {
            if (s2) {
                if (c2 == '\\') continue;
                if (c2 == '"') s2 = false;
                continue;
            }
            if (c2 == '"') { s2 = true; continue; }
            if (c2 == '{' or c2 == '(' or c2 == '[') {
                d2 += 1;
            } else if (c2 == '}' or c2 == ')' or c2 == ']') {
                if (d2 > 0) d2 -= 1;
            } else if (c2 == ':' and d2 == 0) {
                colon = k2;
                break;
            }
        }
        const cl = colon orelse continue;
        // `struct{pub a: u32}`（跨文件匿名形参常这么写）：补全的 label
        // 只能是字段名，`pub ` 是修饰符不是名字。
        var nm0 = trimSl(seg[0..cl]);
        if (std.mem.startsWith(u8, nm0, "pub ") or std.mem.startsWith(u8, nm0, "pub\t"))
            nm0 = trimSl(nm0[3..]);
        const nm = nm0;
        const t2 = trimSl(seg[cl + 1 ..]);
        if (nm.len == 0 or t2.len == 0) continue;
        out.append(gpa, .{ .name = nm, .ty = t2 }) catch {};
    }
    if (out.items.len == 0) return null;
    return out;
}

/// 类型串里第一个 `struct{…}`（完整切片，含关键字与配对 '}'）。
fn structTyOf(ty: []const u8) ?[]const u8 {
    var i: usize = 0;
    while (std.mem.indexOfPos(u8, ty, i, "struct")) |c| {
        i = c + 6;
        if (c > 0 and isIdentChar(ty[c - 1])) continue;
        var j = i;
        while (j < ty.len and (ty[j] == ' ' or ty[j] == '\t' or ty[j] == '\n' or ty[j] == '\r')) j += 1;
        if (j >= ty.len or ty[j] != '{') continue;
        const rb = matchBrace(ty, j) orelse continue;
        return ty[c .. rb + 1];
    }
    return null;
}

/// `const <fname> = fn (…)` 第 argi 个形参的匿名 struct 类型串。
/// 该位形参不是 struct 时退回第一个 struct 形参（旧口径，3d 守门依赖）。
fn anonParamTy(files: []const []const u8, fname: []const u8, argi: usize) ?[]const u8 {
    var pat: std.ArrayList(u8) = .empty;
    defer pat.deinit(gpa);
    pat.appendSlice(gpa, "const ") catch return null;
    pat.appendSlice(gpa, fname) catch return null;
    pat.appendSlice(gpa, " = fn (") catch return null;
    for (files) |ftext| {
        var idx: usize = 0;
        while (std.mem.indexOfPos(u8, ftext, idx, pat.items)) |hit| {
            if (hit > 0 and isIdentChar(ftext[hit - 1])) {
                idx = hit + 1;
                continue;
            }
            var i = hit + pat.items.len; // 参数列表第一个字符
            var depth: usize = 0;
            while (i < ftext.len) : (i += 1) {
                if (ftext[i] == '(') depth += 1;
                if (ftext[i] == ')') {
                    if (depth == 0) break;
                    depth -= 1;
                }
            }
            if (i >= ftext.len) {
                idx = hit + 1;
                continue;
            }
            // 顶层逗号切参数
            var segs: std.ArrayList([]const u8) = .empty;
            defer segs.deinit(gpa);
            var d2: usize = 0;
            var in_str = false;
            var start = hit + pat.items.len;
            var q = start;
            while (q < i) : (q += 1) {
                const c = ftext[q];
                if (in_str) {
                    if (c == '\\') { q += 1; continue; }
                    if (c == '"') in_str = false;
                    continue;
                }
                if (c == '"') { in_str = true; continue; }
                if (c == '{' or c == '(' or c == '[') d2 += 1;
                if (c == '}' or c == ')' or c == ']') { if (d2 > 0) d2 -= 1; }
                if (c == ',' and d2 == 0) {
                    const t = trimSl(ftext[start..q]);
                    if (t.len > 0) segs.append(gpa, t) catch {};
                    start = q + 1;
                }
            }
            {
                const t = trimSl(ftext[start..i]);
                if (t.len > 0) segs.append(gpa, t) catch {};
            }
            // 目标位优先，该位不是 struct 时退回第一个 struct 形参
            var pick: ?[]const u8 = null;
            if (argi < segs.items.len and structTyOf(segs.items[argi]) != null)
                pick = segs.items[argi];
            if (pick == null) {
                for (segs.items) |sg| {
                    if (structTyOf(sg) != null) {
                        pick = sg;
                        break;
                    }
                }
            }
            if (pick) |sg| {
                if (structTyOf(sg)) |sty| return sty;
            }
            idx = hit + 1; // 本命中无 struct 形参，继续找下一处声明
        }
    }
    return null;
}

/// 调用 '(' 与目标 '{' 之间的嵌套字段链：`("n", .{ .opts = .{ ` → ["opts"]。
/// 统一规则：遇到 '{' 看它前面是不是 `.name =`（匿名字面量）或裸类型名
/// （struct{ / Obj{）——配对 '}' 在目标之外（目标是其后代）则记名下钻，
/// 在目标之前已闭合（兄弟字面量）则整体跳过；目标自身（lb == to）记名即停。
fn anonFieldChain(text: []const u8, from: usize, to: usize) std.ArrayList([]const u8) {
    var chain: std.ArrayList([]const u8) = .empty;
    var i = from;
    while (i < to) {
        const c = text[i];
        if (c == '"') {
            i += 1;
            while (i < to and text[i] != '"') {
                if (text[i] == '\\' and i + 1 < to) i += 1;
                i += 1;
            }
            i += 1;
            continue;
        }
        if (c == '/' and i + 1 < to and text[i + 1] == '/') {
            while (i < to and text[i] != '\n') i += 1;
            continue;
        }
        if (c != '{') { i += 1; continue; }
        var name: []const u8 = "";
        var k = i;
        while (k > from and (text[k - 1] == ' ' or text[k - 1] == '\t' or
            text[k - 1] == '\n' or text[k - 1] == '\r')) k -= 1;
        if (k > from and text[k - 1] == '.') {
            // `.{`：看 '.' 前是不是 `name =`
            var m = k - 1;
            while (m > from and (text[m - 1] == ' ' or text[m - 1] == '\t' or
                text[m - 1] == '\n' or text[m - 1] == '\r')) m -= 1;
            if (m > from and text[m - 1] == '=') {
                var e2 = m - 1;
                while (e2 > from and (text[e2 - 1] == ' ' or text[e2 - 1] == '\t' or
                    text[e2 - 1] == '\n' or text[e2 - 1] == '\r')) e2 -= 1;
                var s3 = e2;
                while (s3 > from and isIdentChar(text[s3 - 1])) s3 -= 1;
                if (e2 > s3) name = text[s3..e2];
            }
        } else if (k > from and isIdentChar(text[k - 1])) {
            // 裸类型字面量 Obj{ / struct{：类型名前是不是 `name =`
            var e2 = k - 1;
            while (e2 > from and isIdentChar(text[e2 - 1])) e2 -= 1;
            var m = e2;
            while (m > from and (text[m - 1] == ' ' or text[m - 1] == '\t' or
                text[m - 1] == '\n' or text[m - 1] == '\r')) m -= 1;
            if (m > from and text[m - 1] == '=') {
                var e3 = m - 1;
                while (e3 > from and (text[e3 - 1] == ' ' or text[e3 - 1] == '\t' or
                    text[e3 - 1] == '\n' or text[e3 - 1] == '\r')) e3 -= 1;
                var s3 = e3;
                while (s3 > from and isIdentChar(text[s3 - 1])) s3 -= 1;
                if (e3 > s3) name = text[s3..e3];
            }
        }
        const rb = matchBrace(text, i) orelse {
            // 未闭合的祖先字面量（编辑中的调用实参），目标在其内部：
            // 有名则记名下钻，匿名也继续往里走
            if (name.len > 0) chain.append(gpa, name) catch {};
            i += 1;
            continue;
        };
        if (rb > to) {
            // 祖先字面量：目标在其内部。有名则记名下钻；匿名（调用最外层
            // `.{`，前面是 '(' 没有 .name =）无名也必须继续往里走，
            // 绝不能 i = rb + 1 整体跳过——那会把目标自身一并跳掉，
            // chain 变空、补全退回外层字段（anonDeep 当初的假绿根源）。
            if (name.len > 0) chain.append(gpa, name) catch {};
            i += 1;
            continue;
        }
        i = rb + 1; // 目标之外的兄弟字面量（已闭合），整体跳过
    }
    return chain;
}

/// 点路径 → 匿名类型串。首段：本文件绑定（init 是 .{…} 或其别名链），
/// 找不到声明再按 `name: struct{…}` 形状试形参/字段；后续段沿匿名字段下钻。
/// `anon.pos` / `info.a` / `nv.pos.x` 都走这里；识别不了返回 null。
fn anonTyOfPath(text: []const u8, doc: *const Document, path: []const u8, off: usize) ?[]const u8 {
    var ty: []const u8 = "";
    var idx: usize = 0;
    var it = std.mem.splitScalar(u8, path, '.');
    while (it.next()) |seg| {
        if (seg.len == 0) return null;
        if (idx == 0) {
            if (findDeclStmt(text, seg)) |stmt| {
                const init = declInitOf(stmt) orelse return null;
                ty = inferExprType(text, doc, init, 0) orelse return null;
            } else if (namedTypeNear(text, seg, off)) |t| {
                ty = t;
            } else return null;
        } else {
            const flds = parseAnonTy(ty) orelse return null;
            var hit: ?[]const u8 = null;
            for (flds.items) |f| {
                if (std.mem.eql(u8, f.name, seg)) {
                    hit = f.ty;
                    break;
                }
            }
            ty = hit orelse return null;
        }
        idx += 1;
    }
    if (idx == 0) return null;
    return ty;
}

// ------------------------------------------------------------- 模块搜索目录
//
// 编译器的 module_load 只有一条解析链：**相对导入文件所在目录** → 打不开时
// 依次退 <搜索路径>/<模块名>，搜索路径由 setup_search_paths 填成
// 「vendor/ → zx.json 的 deps」。语言服务器以前只认前两个目录（文档目录、
// 工程根），vendor/ 里的模块于是能编过、却补不出成员——同一条解析链在两侧
// 各写一遍是这次 bug 的根源，现在 vendor/ 与 deps 在这里补齐。
//
// deps 是相对工程根写的，而 LSP 进程的 cwd 由宿主决定（Zed 里未必是工程根），
// 所以这里一律拼成**绝对路径**。目录只在 initialize 后算一次——每次补全
// 都重扫 zx.json 不划算；工程根的 zx.json 改了要重启语言服务器（与
// 诊断用的 zx.json 读取口径一致）。

var g_mod_dirs: [48][]const u8 = undefined;
var g_mod_dir_n: usize = 0;

fn joinPath(a: []const u8, b: []const u8) []const u8 {
    var out: std.ArrayList(u8) = .empty;
    out.appendSlice(gpa, a) catch return "";
    if (out.items.len > 0 and out.items[out.items.len - 1] != '/' and
        out.items[out.items.len - 1] != '\\') out.append(gpa, '/') catch {};
    out.appendSlice(gpa, b) catch {};
    return out.items;
}

fn isAbsPath(p: []const u8) bool {
    if (p.len == 0) return false;
    if (p[0] == '/' or p[0] == '\\') return true;
    return p.len >= 2 and p[1] == ':'; // C: 盘符
}

fn isDirPath(p: []const u8) bool {
    if (p.len == 0) return false;
    var d = Io.Dir.cwd().openDir(g_io, p, .{}) catch return false;
    d.close(g_io);
    return true;
}

/// 收一个搜索目录（接管 p 的所有权：没被收下就释放）
fn addModDir(p: []const u8) void {
    var keep = false;
    defer if (!keep) gpa.free(p);
    if (p.len == 0 or g_mod_dir_n >= g_mod_dirs.len) return;
    for (g_mod_dirs[0..g_mod_dir_n]) |d| {
        if (std.mem.eql(u8, d, p)) return;
    }
    if (!isDirPath(p)) return;
    g_mod_dirs[g_mod_dir_n] = p;
    g_mod_dir_n += 1;
    keep = true;
}

/// zx.json 的 `"deps"`：模块搜索目录。相对路径按**工程根**拼成绝对路径
/// （LSP 进程的 cwd 由宿主决定，不能靠相对路径）。`vendor/` 不写也在，
/// 那是 zx 的规矩（编译器 setup_search_paths 同款）。
fn appendDepsDirs(root: []const u8, txt: []const u8) void {
    const parsed = std.json.parseFromSlice(std.json.Value, gpa, txt, .{}) catch return;
    defer parsed.deinit();
    if (parsed.value != .object) return;
    const deps = parsed.value.object.get("deps") orelse return;
    if (deps != .array) return;
    for (deps.array.items) |it| {
        if (it != .string or it.string.len == 0) continue;
        addModDir(if (isAbsPath(it.string)) dupe(it.string) else joinPath(root, it.string));
    }
}

/// initialize 之后调用一次：工程根的 vendor/ 与 zx.json 的 deps
fn initModuleDirs() void {
    g_mod_dir_n = 0;
    const root = root_path orelse return;
    addModDir(joinPath(root, "vendor"));
    const cfg = joinPath(root, "zx.json");
    defer gpa.free(cfg);
    const txt = readFileSmall(cfg) orelse return;
    defer gpa.free(txt);
    appendDepsDirs(root, txt);
}

/// 文档所在目录往上的工程根：就近的 zx.json + 它的 vendor/ + 它的 deps。
/// 只在 initialize 时按 rootUri 算一次是不够的——宿主可能只发单个文件
/// （rootUri 为 null），或者把工作区开在 src/ 而不是工程根，那时
/// `vendor/` 与 deps 全都漏掉：模块能编过，补全/跳转却一律落空。
/// addModDir 自带去重，重复调用不会把表撑大。
fn ensureDocDirs(doc_dir: []const u8) void {
    var dir = doc_dir;
    var depth: u8 = 0;
    while (depth < 8) : (depth += 1) {
        if (dir.len == 0) return;
        // vendor 不看 zx.json 是否存在：编译器也是"目录在就参与搜索"
        addModDir(joinPath(dir, "vendor"));
        const cfg = joinPath(dir, "zx.json");
        const txt = readFileSmall(cfg);
        gpa.free(cfg);
        if (txt) |t| {
            defer gpa.free(t);
            addModDir(dupe(dir));
            appendDepsDirs(dir, t);
            return;
        }
        const up = dirName(dir);
        if (up.len == 0 or up.len >= dir.len) return;
        dir = up;
    }
}

/// 模块解析目录，顺序：文件所在目录 → 工程根 → vendor/ → deps。
/// 前两个每次现算（跟着文档走），后两个 initialize 时已定。
fn modDirsInto(doc_dir: []const u8, out: *std.ArrayList([]const u8)) void {
    // 文档目录附近的工程根 / vendor / deps：跟着文档补齐（见 ensureDocDirs）
    ensureDocDirs(doc_dir);
    out.append(gpa, doc_dir) catch return;
    if (root_path) |r| {
        if (!std.mem.eql(u8, r, doc_dir)) out.append(gpa, r) catch return;
    }
    for (g_mod_dirs[0..g_mod_dir_n]) |d| out.append(gpa, d) catch return;
    // std 兜底目录放最后：工程与 vendor/deps 都没命中才生效，
    // 对齐编译器「module("std") 走 find_std_path 特例」的语义。
    if (g_std_dir) |sd| out.append(gpa, sd) catch return;
}

/// module("…") 字符串 → 模块基名（去 .zx 后缀）。编译器两种形式都收：
/// 裸名 "std"（解析 <d>/std.zx 或 <d>/std/std.zx）与路径 "std/str.zx"。
fn moduleBase(m: []const u8) []const u8 {
    if (std.mem.endsWith(u8, m, ".zx")) return m[0 .. m.len - 3];
    return m;
}

/// mod 名 → 各搜索目录下的 `<mod>.zx` 或 `<mod>/<mod>.zx` 顶层声明
/// （只收 pub；目录顺序见 modDirsInto，与编译器一致）
fn moduleDecls(doc_dir: []const u8, mod: []const u8, out: *std.ArrayList(Decl)) void {
    const m = moduleBase(mod);
    var dirs: std.ArrayList([]const u8) = .empty;
    defer dirs.deinit(gpa);
    modDirsInto(doc_dir, &dirs);
    for (dirs.items) |d| {
        var path: std.ArrayList(u8) = .empty;
        defer path.deinit(gpa);
        path.appendSlice(gpa, d) catch continue;
        path.append(gpa, '/') catch continue;
        const dirlen = path.items.len;
        path.appendSlice(gpa, m) catch continue;
        path.appendSlice(gpa, ".zx") catch continue;
        if (readFileSmall(path.items)) |txt| {
            // 跨模块引用只收 pub 声明（非 pub 是模块内部实现细节）
            topLevelDecls(txt, out, true);
            continue;
        }
        // 目录型模块（如 std → std/std.zx）：编译器只读 <mod>/<mod>.zx，
        // 补全也只对齐这个语义，绝不扫目录里其他文件的内部声明。
        path.items.len = dirlen + m.len;
        path.append(gpa, '/') catch continue;
        path.appendSlice(gpa, m) catch continue;
        path.appendSlice(gpa, ".zx") catch continue;
        if (readFileSmall(path.items)) |txt| {
            topLevelDecls(txt, out, true);
            continue;
        }
        // 目录里没有 <mod>/<mod>.zx 才退回全目录扫描（同样只收 pub）
        path.items.len = dirlen + m.len;
        var dir = Io.Dir.cwd().openDir(g_io, path.items, .{ .iterate = true }) catch continue;
        defer dir.close(g_io);
        var it = dir.iterate();
        while (it.next(g_io) catch null) |entry| {
            if (entry.kind != .file) continue;
            if (!std.mem.endsWith(u8, entry.name, ".zx")) continue;
            var fp: std.ArrayList(u8) = .empty;
            defer fp.deinit(gpa);
            fp.appendSlice(gpa, path.items) catch continue;
            fp.append(gpa, '/') catch continue;
            fp.appendSlice(gpa, entry.name) catch continue;
            if (readFileSmall(fp.items)) |txt| topLevelDecls(txt, out, true);
        }
    }
}

// Zx 没有隐式跨文件名字：其它 .zx 里的标识符必须先 module("…") 绑定。
// 早期把它们当裸补全列出来，输入 glfw 弹一堆不可用的 glfwInit，
// 真正的模块名反而被淹没（用户 2026-11-08 反馈）。模块名改由
// addImportModules 在声明位置给「插入绑定」项，成员走 `绑定名.` 补全。

fn readFileSmall(path: []const u8) ?[]u8 {
    return Io.Dir.cwd().readFileAlloc(g_io, path, gpa, Io.Limit.limited(4 * 1024 * 1024)) catch null;
}

// ---------------------------------------------------------------- definition

fn pathToUri(path: []const u8) []const u8 {
    var out: std.ArrayList(u8) = .empty;
    out.appendSlice(gpa, "file:///") catch {};
    for (path) |c| out.append(gpa, if (c == '\\') '/' else c) catch {};
    return out.items;
}

/// Location JSON（appendSlice 拼字面括号，绕开 fmt 转义）
fn makeLocation(uri: []const u8, l1: u32, c1: u32, c2: u32) []const u8 {
    var j = Json{};
    j.buf.appendSlice(gpa, "{\"uri\":") catch {};
    j.str(uri);
    j.buf.appendSlice(gpa, ",\"range\":{\"start\":{\"line\":") catch {};
    j.raw("{d}", .{l1});
    j.buf.appendSlice(gpa, ",\"character\":") catch {};
    j.raw("{d}", .{c1});
    j.buf.appendSlice(gpa, "},\"end\":{\"line\":") catch {};
    j.raw("{d}", .{l1});
    j.buf.appendSlice(gpa, ",\"character\":") catch {};
    j.raw("{d}", .{c2});
    j.buf.appendSlice(gpa, "}}}") catch {};
    return j.buf.items;
}

/// 行内找 `[pub ][extern ](const|let|var|type|fn) word` 声明；返回 0-based 行/列
fn findDeclLine(text: []const u8, word: []const u8) ?struct { line: u32, col: u32 } {
    var ln: u32 = 0;
    var it = std.mem.splitScalar(u8, text, '\n');
    while (it.next()) |line_raw| : (ln += 1) {
        var ci: usize = 0;
        while (ci < line_raw.len and (line_raw[ci] == ' ' or line_raw[ci] == '\t')) ci += 1;
        if (line_raw.len - ci >= 4 and std.mem.eql(u8, line_raw[ci .. ci + 4], "pub ")) ci += 4;
        while (ci < line_raw.len and (line_raw[ci] == ' ' or line_raw[ci] == '\t')) ci += 1;
        if (line_raw.len - ci >= 7 and std.mem.eql(u8, line_raw[ci .. ci + 7], "extern ")) ci += 7;
        while (ci < line_raw.len and (line_raw[ci] == ' ' or line_raw[ci] == '\t')) ci += 1;
        const rest = line_raw[ci..];
        const kws = [_][]const u8{ "const", "let", "var", "type", "fn" };
        for (kws) |kw| {
            if (!(rest.len > kw.len and std.mem.startsWith(u8, rest, kw))) continue;
            if (rest[kw.len] != ' ' and rest[kw.len] != '\t') continue;
            var t = kw.len + 1;
            while (t < rest.len and (rest[t] == ' ' or rest[t] == '\t')) t += 1;
            if (std.mem.startsWith(u8, rest[t..], word) and
                (rest.len == t + word.len or !isIdentChar(rest[t + word.len])))
            {
                return .{ .line = ln, .col = @intCast(ci + t) };
            }
        }
    }
    return null;
}

/// `const word = module("X")` → X。KEEP: 等号两侧的空白是任意的——
/// 真实工程里为了对齐常写 `const g    = module("glfw3");`，旧版按单空格
/// 拼字符串硬匹配，于是 `g.` 落回工作区标识符兜底：补全一堆噪音、hover
/// 和签名全空。用户 2026-11-08 反馈的 vendor / 手写 extern 模块没提示，
/// 除了 extern fn 没进索引，这一处是第二个根因。
fn moduleOfBinding(text: []const u8, word: []const u8) ?[]const u8 {
    if (word.len == 0) return null;
    var from: usize = 0;
    while (std.mem.indexOfPos(u8, text, from, "const ")) |c| {
        from = c + 6;
        if (c > 0 and isIdentChar(text[c - 1])) continue;
        if (inLineComment(text, c)) continue;
        var j = c + 6;
        while (j < text.len and (text[j] == ' ' or text[j] == '\t')) j += 1;
        if (!std.mem.startsWith(u8, text[j..], word)) continue;
        var e = j + word.len;
        if (e < text.len and isIdentChar(text[e])) continue;
        while (e < text.len and (text[e] == ' ' or text[e] == '\t' or
            text[e] == '\r' or text[e] == '\n')) e += 1;
        if (e >= text.len or text[e] != '=') continue;
        e += 1;
        while (e < text.len and (text[e] == ' ' or text[e] == '\t' or
            text[e] == '\r' or text[e] == '\n')) e += 1;
        if (!std.mem.startsWith(u8, text[e..], "module(\"")) continue;
        const s = e + 8; // 跳过 `module("` —— 8 个字符
        const q = std.mem.indexOfScalarPos(u8, text, s, '"') orelse continue;
        if (q == s) continue;
        return text[s..q];
    }
    return null;
}

/// off 在同行 module("…" 字符串内（含停在内容上）→ 完整模块名
fn moduleStringAt(text: []const u8, off: usize) ?[]const u8 {
    var ls = off;
    while (ls > 0 and text[ls - 1] != '\n') ls -= 1;
    const seg = text[ls..off];
    const hit = std.mem.lastIndexOf(u8, seg, "module(\"") orelse return null;
    const start = hit + 8; // 跳过 module("
    if (std.mem.indexOfScalar(u8, seg[start..], '"') != null) return null; // 已闭合
    // 模块名延伸到闭合引号
    var e = off;
    while (e < text.len and text[e] != '"' and text[e] != '\n') e += 1;
    return text[ls + start .. e];
}

/// 声明初始化是不是容器字面量开头（struct/union/enum）。
/// hoverMarkdownForDecl 的 is_container 判定同款。
fn containerInitOf(stmt: []const u8) ?[]const u8 {
    const init0 = trimSl(declInitOf(stmt) orelse return null);
    if (std.mem.startsWith(u8, init0, "struct") or
        std.mem.startsWith(u8, init0, "union") or
        std.mem.startsWith(u8, init0, "enum")) return init0;
    return null;
}

const ModContainer = struct { txt: []const u8, member: []const u8 };

/// KEEP — vendor/deps 导入的容器 `vp.Cfg` / `std.Widget`：
/// 本文件与 std 目录扫描（scanFiles）都够不到模块文件，容器体必须按
/// 绑定解析。用户 2026-11-08：从 vendor 导入的 struct、fn 和原生不一样，
/// 字段/成员也要查得到。限定名（恰好一段 `绑定.容器`）、绑定存在、
/// 模块文件里该成员确实是容器，三条都成立才命中。
fn modContainerOf(doc: *const Document, text: []const u8, qualified: []const u8) ?ModContainer {
    const d = std.mem.lastIndexOfScalar(u8, qualified, '.') orelse return null;
    if (d == 0 or d + 1 >= qualified.len) return null;
    const bind = qualified[0..d];
    const member = qualified[d + 1 ..];
    if (std.mem.indexOfScalar(u8, member, '.') != null) return null;
    var mm: ?[]const u8 = moduleOfBinding(text, bind);
    if (mm == null and std.mem.eql(u8, bind, "std")) mm = "std";
    const mod = mm orelse return null;
    if (resolveModuleFile(doc, mod)) |mp| {
        if (readFileSmall(mp)) |mt| {
            if (findDeclStmt(mt, member)) |st| {
                if (containerInitOf(st) != null) return .{ .txt = mt, .member = member };
            }
        }
    }
    return null;
}

/// 容器体里成员声明的字节偏移（相对 body 起点）。字段（`pub c: i32,`）、
/// 方法/嵌套类型（`pub const X = …`）、枚举变体都能定到。
/// definition 用它把「路径成员」落到真实声明上——成员常在别的文件里，
/// 光有体文本不够，还要偏移。
fn memberDeclOffset(body: []const u8, word: []const u8) ?usize {
    const base = @intFromPtr(body.ptr);
    if (findMemberStmt(body, word)) |st| {
        const p = @intFromPtr(st.ptr);
        if (p >= base and p - base + st.len <= body.len) {
            var k: usize = 0;
            while (k + word.len <= st.len) : (k += 1) {
                if (!std.mem.eql(u8, st[k .. k + word.len], word)) continue;
                if (k > 0 and isIdentChar(st[k - 1])) continue;
                if (k + word.len < st.len and isIdentChar(st[k + word.len])) continue;
                return p - base + k;
            }
            return p - base;
        }
    }
    // 字段与枚举变体：体里深度 1 的 `word:` 或 `word,`
    var depth: usize = 0;
    var in_str = false;
    var i: usize = 0;
    while (i < body.len) : (i += 1) {
        const c = body[i];
        if (in_str) {
            if (c == '\\') { i += 1; continue; }
            if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; continue; }
        if (c == '/' and i + 1 < body.len and body[i + 1] == '/') {
            while (i < body.len and body[i] != '\n') i += 1;
            continue;
        }
        if (c == '{' or c == '(' or c == '[') { depth += 1; continue; }
        if (c == '}' or c == ')' or c == ']') { if (depth > 0) depth -= 1; continue; }
        if (depth != 1 or !isIdentChar(c)) continue;
        if (i > 0 and isIdentChar(body[i - 1])) continue;
        var j = i;
        while (j < body.len and isIdentChar(body[j])) j += 1;
        if (!std.mem.eql(u8, body[i..j], word)) continue;
        var k = j;
        while (k < body.len and (body[k] == ' ' or body[k] == '\t')) k += 1;
        if (k < body.len and (body[k] == ':' or body[k] == ',')) return i;
    }
    return null;
}

/// 体文本属于哪个文件：先看是不是当前文档，再按**内容**在文档目录与 std
/// 目录里比对（readFileSmall 每次都新分配，指针没法用）。返回路径。
fn pathOfSrcText(doc: *const Document, src: []const u8) ?[]const u8 {
    if (std.mem.eql(u8, src, doc.text)) return doc.path;
    const doc_dir = dirName(doc.path);
    if (scanDirForText(doc_dir, src)) |p| return p;
    // 模块目录（vendor / zx.json deps / 工程根）：`t.P1.P2` 这种类型的声明
    // 在 vendor/test.zx 里，只扫文档目录永远定位不到 → 跳转落到 doc.path
    // 配一个乱算出来的行号（用户 2026-11-11：`P.P3` 跳到自己的 `const pp`
    // 那一行）。目录顺序与 modDirsInto 一致。
    if (root_path) |r| {
        if (!std.mem.eql(u8, r, doc_dir)) {
            if (scanDirForText(r, src)) |p| return p;
        }
    }
    for (g_mod_dirs[0..g_mod_dir_n]) |md| {
        if (std.mem.eql(u8, md, doc_dir)) continue;
        if (scanDirForText(md, src)) |p| return p;
    }
    if (g_std_dir) |sd| {
        if (scanDirForText(sd, src)) |p| return p;
    }
    return null;
}

fn scanDirForText(dir_path: []const u8, src: []const u8) ?[]const u8 {
    var d = Io.Dir.cwd().openDir(g_io, dir_path, .{ .iterate = true }) catch return null;
    defer d.close(g_io);
    var it = d.iterate();
    while (it.next(g_io) catch null) |entry| {
        if (entry.kind != .file or !std.mem.endsWith(u8, entry.name, ".zx")) continue;
        var fp: std.ArrayList(u8) = .empty;
        defer fp.deinit(gpa);
        fp.appendSlice(gpa, dir_path) catch continue;
        fp.append(gpa, '/') catch continue;
        fp.appendSlice(gpa, entry.name) catch continue;
        const txt = readFileSmall(fp.items) orelse continue;
        // dupe：fp 的 defer deinit 在 return 时就会释放缓冲区
        if (std.mem.eql(u8, txt, src)) return dupe(fp.items);
    }
    return null;
}

/// `var c = vp.Cfg{…}`：vendor/deps 容器实例 → 把容器字段收进 out。
/// 原生实例（`var o = Obj{…}`）走 inferContainer（本文件/std 扫得到），
/// 模块容器这里补齐——inferContainer 对绑定名打头的结果一律 null。
fn vendorInstanceFields(doc: *const Document, text: []const u8, name: []const u8,
                        out: *std.ArrayList(Decl)) bool {
    const s = findDeclStmt(text, name) orelse return false;
    const init0 = trimSl(declInitOf(s) orelse return false);
    var j2: usize = 0;
    while (j2 < init0.len and (isIdentChar(init0[j2]) or init0[j2] == '.')) j2 += 1;
    if (j2 == 0 or j2 >= init0.len or init0[j2] != '{') return false;
    const mc = modContainerOf(doc, text, init0[0..j2]) orelse return false;
    return containerMembers(&[_][]const u8{mc.txt}, mc.member, out);
}

/// 模块名 → 真实文件路径（<dir>/<m>.zx 或 <dir>/<m>/<m>.zx）
fn resolveModuleFile(doc: *const Document, m_raw: []const u8) ?[]const u8 {
    const m = moduleBase(m_raw);
    const doc_dir = dirName(doc.path);
    var dirs: std.ArrayList([]const u8) = .empty;
    defer dirs.deinit(gpa);
    modDirsInto(doc_dir, &dirs);
    for (dirs.items) |d| {
        var path: std.ArrayList(u8) = .empty;
        defer path.deinit(gpa);
        path.appendSlice(gpa, d) catch continue;
        path.append(gpa, '/') catch continue;
        const base_len = path.items.len;
        path.appendSlice(gpa, m) catch continue;
        path.appendSlice(gpa, ".zx") catch continue;
        if (readFileSmall(path.items) != null) return dupe(path.items);
        // <m>/<m>.zx
        path.items.len = base_len + m.len;
        path.append(gpa, '/') catch continue;
        path.appendSlice(gpa, m) catch continue;
        path.appendSlice(gpa, ".zx") catch continue;
        if (readFileSmall(path.items) != null) return dupe(path.items);
    }
    return null;
}

/// 模块文件里 `pub const seg = module("x.zx")` 再导出的真实文件
///（相对该模块文件所在目录解析）。
fn aliasModuleFile(doc: *const Document, mod_path: []const u8, seg: []const u8) ?[]const u8 {
    const txt = readFileSmall(mod_path) orelse return null;
    const al = moduleAliasIn(txt, seg) orelse return null;
    var ap: std.ArrayList(u8) = .empty;
    ap.appendSlice(gpa, dirName(mod_path)) catch return null;
    ap.append(gpa, '/') catch return null;
    ap.appendSlice(gpa, al) catch return null;
    if (readFileSmall(ap.items) != null) return dupe(ap.items);
    // 别名写的是模块名而不是文件名：退回常规解析
    return resolveModuleFile(doc, al);
}

/// 限定路径（可能多段）→ 真实模块文件。
///   "std"           → std/std.zx
///   "glfw3"         → 绑定名 moduleOfBinding
///   "std.task"      → std.zx 里 pub const task = module("task.zx") → std/task.zx
///   "vp.util.walk"  → vendor 模块的二级再导出，同样逐段走
fn qualModulePath(doc: *const Document, text: []const u8, path: []const u8) ?[]const u8 {
    if (path.len == 0) return null;
    const d0 = std.mem.indexOfScalar(u8, path, '.');
    if (d0 == null) {
        if (std.mem.eql(u8, path, "std")) return resolveModuleFile(doc, "std");
        const m = moduleOfBinding(text, path) orelse return null;
        return resolveModuleFile(doc, m);
    }
    const seg0 = path[0..d0.?];
    const first: []const u8 = if (std.mem.eql(u8, seg0, "std"))
        "std"
    else
        moduleOfBinding(text, seg0) orelse return null;
    var mp = resolveModuleFile(doc, first) orelse return null;
    var rest = path[d0.? + 1 ..];
    while (rest.len > 0) {
        var seg = rest;
        var more = false;
        if (std.mem.indexOfScalar(u8, rest, '.')) |d| {
            seg = rest[0..d];
            more = true;
        }
        mp = aliasModuleFile(doc, mp, seg) orelse return null;
        rest = if (more) rest[seg.len + 1 ..] else "";
    }
    return mp;
}

/// `base` 是模块（`std` / `const t = module(…)` / 再导出子模块 `std.json`）时
/// 返回该模块文件的文本；不是模块返回 null。挂在 qualModulePath 上，因此
/// 再导出的子模块（std.zx 里 `pub const json = module("json.zx")`）也认。
fn moduleTextOf(doc: *const Document, text: []const u8, base: []const u8) ?[]const u8 {
    const mp = qualModulePath(doc, text, base) orelse return null;
    return readFileSmall(mp);
}

/// 模块文件里的成员是什么：容器/类型声明 → "type"；指向容器/类型的别名常量
/// （`pub const Entry = Base;`）→ "type"；写了标注的常量 → 标注类型；
/// 函数 → null（函数值的类型没有可读串，不能拿返回类型冒充）。
/// 用于 `const P = t.Inner;` / `const X = t.Inner.Sub;` 的类型推断
///（用户 2026-11-11 第 4 张截图：`t` 是 module 时没有 `: type`）。
fn moduleMemberTy(files: []const []const u8, doc: *const Document,
                  mt: []const u8, member: []const u8) ?[]const u8 {
    if (isContainerIn(&[_][]const u8{mt}, member)) return "type";
    const st = findDeclStmt(mt, member) orelse return null;
    const sig = sigOfStmt(st);
    if (sig.is_fn) return null;
    if (declTyOf(st)) |dt| {
        const t = trimSl(dt);
        if (t.len > 0 and !std.mem.eql(u8, t, "type")) return t;
    }
    const ini = trimSl(declInitOf(st) orelse return null);
    if (ini.len == 0 or !isPurePath(ini)) return null;
    // 别名指向的容器：先在本模块文件里逐段找，再借扫描集
    //（模块可能在 std/ 或 vendor 里，扫描集里没有它的文本）。
    var afiles: std.ArrayList([]const u8) = .empty;
    defer afiles.deinit(gpa);
    afiles.append(gpa, mt) catch {};
    for (files) |f| afiles.append(gpa, f) catch {};
    if (resolveContainerPathD(afiles.items, doc, mt, ini, 1) != null) return "type";
    return null;
}

/// `base.member` 的跳转：base 先按模块路径 / 容器路径逐段解析，再在目标文件
/// 或容器体里找成员声明（方法、嵌套类型、字段、枚举变体）。
///
/// 必须能**独立调用**：调用点在"本文件同名声明"之前。`P.P3.f3` 里的 `f3`
/// 会和 `const f3 = …` 同名，先查本文件就跳到自己的 const 行，而不是模块里的
/// 声明（用户 2026-11-11：`t` 是 module 时"跳转还是没有"）。
fn definitionOfMember(doc: *const Document, text: []const u8,
                      word: []const u8, ws: usize) ?[]const u8 {
    if (ws == 0 or text[ws - 1] != '.') return null;
    const bs = memberBaseStart(text, ws - 1);
    // 泛型实例上的成员：`HashMap(i32).init` 的接收者是 `HashMap(i32)`，
    // 剥掉实例化实参才认得出容器（`std.map.HashMap(i32).init` 以前一律 MISS）。
    const base = stripGenericSuffix(text[bs .. ws - 1]);
    if (base.len == 0) return null;

    // 模块路径（`std.task.Task`、`m.Root.P1`）：沿 std 门面 / 模块绑定的
    // 再导出逐段落到真实源文件，再找顶层声明。
    if (qualModulePath(doc, text, base)) |p| {
        if (aliasModuleFile(doc, p, word)) |ap2| return makeLocation(pathToUri(ap2), 0, 0, 0);
        if (readFileSmall(p)) |t| {
            if (findDeclLine(t, word)) |loc|
                return makeLocation(pathToUri(p), loc.line, loc.col, loc.col + @as(u32, @intCast(word.len)));
        }
    }
    if (!isPurePath(base)) return null;

    // 容器路径（实例链 / 限定类型路径 / 类型值别名 / 模块里的容器）：逐段
    // 解析拿到容器体，再在体里找成员声明。
    var sf = scanFiles(doc, text);
    defer sf.deinit(gpa);
    const pc = resolveContainerPath(sf.items, doc, text, base) orelse
        // `const P: type = t.P1.P2;` 之后的 `P.P3`：base 就是绑定名 P，
        // 容器在它的初值路径上（containerOfBinding 内部还有末段退路）。
        containerOfBinding(sf.items, doc, text, base, 0) orelse
        // base 本身是链（`t.P1.P2.P3`）而源头解析不到：末段退路。
        resolveContainerPathLoose(sf.items, doc, text, base) orelse return null;
    const boff = memberDeclOffset(pc.body, word) orelse return null;
    const base_off = @intFromPtr(pc.body.ptr) - @intFromPtr(pc.src.ptr);
    const lc = offsetToLineCol(pc.src, base_off + boff);
    const p2 = pathOfSrcText(doc, pc.src) orelse doc.path;
    return makeLocation(pathToUri(p2), lc.line, lc.character,
                        lc.character + @as(u32, @intCast(word.len)));
}

fn definitionAt(doc: *const Document, line: u32, character: u32) ?[]const u8 {
    const text = doc.text;
    const off = posToOffset(text, line, character);

    // module("…") 字符串内（含停在名字上）：跳模块文件
    if (moduleStringAt(text, off)) |m| {
        if (resolveModuleFile(doc, m)) |p| return makeLocation(pathToUri(p), 0, 0, 0);
        return null;
    }

    var ws = off;
    while (ws > 0 and isIdentChar(text[ws - 1])) ws -= 1;
    var we = off;
    while (we < text.len and isIdentChar(text[we])) we += 1;
    const word = text[ws..we];
    if (word.len == 0) return null;

    // 0.5) 成员访问 `base.member`：**先**按成员解析，本文件里的同名声明不抢。
    //      `P.P3.f3` 的 `f3` 与 `const f3 = …` 同名，先查本文件就会跳到自己的
    //      const 行（用户 2026-11-11：`t` 是 module，函数跳不过去）。
    if (definitionOfMember(doc, text, word, ws)) |loc| return loc;

    // 1) 当前文档声明
    if (findDeclLine(text, word)) |loc|
        return makeLocation(doc.uri, loc.line, loc.col, loc.col + @as(u32, @intCast(word.len)));

    // 2) 模块绑定名 / std → 模块文件
    var m = moduleOfBinding(text, word);
    if (m == null and std.mem.eql(u8, word, "std")) m = "std";
    if (m) |mm| {
        if (resolveModuleFile(doc, mm)) |p| return makeLocation(pathToUri(p), 0, 0, 0);
    }

    // 3) 兄弟文件的顶层声明（同目录 + worktree 根）
    const doc_dir = dirName(doc.path);
    var dirs: [2][]const u8 = undefined;
    var nd: usize = 0;
    dirs[nd] = doc_dir;
    nd += 1;
    if (root_path) |r| {
        if (!std.mem.eql(u8, r, doc_dir)) {
            dirs[nd] = r;
            nd += 1;
        }
    }
    const self_name = baseName(doc.path);
    for (dirs[0..nd]) |d| {
        var dir = Io.Dir.cwd().openDir(g_io, d, .{ .iterate = true }) catch continue;
        defer dir.close(g_io);
        var it = dir.iterate();
        while (it.next(g_io) catch null) |entry| {
            if (entry.kind != .file or !std.mem.endsWith(u8, entry.name, ".zx")) continue;
            if (std.mem.eql(u8, entry.name, self_name)) continue;
            var fp: std.ArrayList(u8) = .empty;
            defer fp.deinit(gpa);
            fp.appendSlice(gpa, d) catch continue;
            fp.append(gpa, '/') catch continue;
            fp.appendSlice(gpa, entry.name) catch continue;
            const txt = readFileSmall(fp.items) orelse continue;
            if (findDeclLine(txt, word)) |loc|
                return makeLocation(pathToUri(fp.items), loc.line, loc.col, loc.col + @as(u32, @intCast(word.len)));
        }
    }
    return null;
}

// ---------------------------------------------------------------- symbols / references

/// 大纲：第 0 列起的 `[pub] (const|var|type) NAME =`（缩进的 struct 方法不算，
/// 那是成员不是顶层符号）。kind 按 '=' 右侧初值判。
fn documentSymbols(doc: *const Document) []const u8 {
    const text = doc.text;
    var j = Json{};
    j.buf.append(gpa, '[') catch {};
    var first = true;
    var ln: u32 = 0;
    var it = std.mem.splitScalar(u8, text, '\n');
    while (it.next()) |line_raw| : (ln += 1) {
        const line = std.mem.trimEnd(u8, line_raw, " \t\r");
        // 只收第 0 列的声明
        var ci: usize = 0;
        while (ci < line.len and (line[ci] == ' ' or line[ci] == '\t')) ci += 1;
        if (ci != 0) continue;
        var rest = line;
        if (std.mem.startsWith(u8, rest, "pub ")) {
            rest = std.mem.trimStart(u8, rest[4..], " \t");
        }
        // fn 直接声明（pub fn main(...)）：先于 const/var/type 检查
        if (std.mem.startsWith(u8, rest, "fn ")) {
            var t2: usize = 3;
            while (t2 < rest.len and (rest[t2] == ' ' or rest[t2] == '\t')) t2 += 1;
            var n2 = t2;
            while (n2 < rest.len and isIdentChar(rest[n2])) n2 += 1;
            if (n2 > t2 and n2 < rest.len and rest[n2] == '(') {
                if (!first) j.buf.append(gpa, ',') catch {};
                first = false;
                j.buf.appendSlice(gpa, "{\"name\":") catch {};
                j.str(rest[t2..n2]);
                j.buf.appendSlice(gpa, ",\"kind\":12,\"range\":{\"start\":{\"line\":") catch {};
                j.raw("{d}", .{ln});
                j.buf.appendSlice(gpa, ",\"character\":0},\"end\":{\"line\":") catch {};
                j.raw("{d}", .{ln});
                j.buf.appendSlice(gpa, ",\"character\":") catch {};
                j.raw("{d}", .{line.len});
                j.buf.appendSlice(gpa, "}},\"selectionRange\":{\"start\":{\"line\":") catch {};
                j.raw("{d}", .{ln});
                j.buf.appendSlice(gpa, ",\"character\":") catch {};
                j.raw("{d}", .{t2});
                j.buf.appendSlice(gpa, "},\"end\":{\"line\":") catch {};
                j.raw("{d}", .{ln});
                j.buf.appendSlice(gpa, ",\"character\":") catch {};
                j.raw("{d}", .{n2});
                j.buf.appendSlice(gpa, "}}}") catch {};
            }
            continue;
        }
        const kws = [_][]const u8{ "const", "var", "type" };
        var kwlen: usize = 0;
        var is_var = false;
        for (kws) |kw| {
            if (rest.len > kw.len and std.mem.startsWith(u8, rest, kw) and
                (rest[kw.len] == ' ' or rest[kw.len] == '\t'))
            {
                kwlen = kw.len + 1;
                is_var = kw[0] == 'v';
                break;
            }
        }
        if (kwlen == 0) continue;
        var t = kwlen;
        while (t < rest.len and (rest[t] == ' ' or rest[t] == '\t')) t += 1;
        var n = t;
        while (n < rest.len and isIdentChar(rest[n])) n += 1;
        if (n == t) continue;
        const name = rest[t..n];
        // 名字后（可隔类型注解）要接 '='
        var q = n;
        while (q < rest.len and (rest[q] == ' ' or rest[q] == '\t' or isIdentChar(rest[q]) or
            rest[q] == ':' or rest[q] == '*' or rest[q] == '[' or rest[q] == ']')) q += 1;
        if (q >= rest.len or rest[q] != '=') continue;
        const after = rest[q..];
        const kind: u32 = blk: {
            if (std.mem.indexOf(u8, after, "fn ") != null or std.mem.indexOf(u8, after, "fn(") != null) break :blk 12; // Function
            if (std.mem.indexOf(u8, after, "struct") != null or std.mem.indexOf(u8, after, "union") != null) break :blk 23; // Struct
            if (std.mem.indexOf(u8, after, "enum") != null) break :blk 10; // Enum
            if (std.mem.indexOf(u8, after, "module(") != null) break :blk 2; // Module
            break :blk if (is_var) 13 else 21; // Variable / Constant
        };
        if (!first) j.buf.append(gpa, ',') catch {};
        first = false;
        j.buf.appendSlice(gpa, "{\"name\":") catch {};
        j.str(name);
        j.buf.appendSlice(gpa, ",\"kind\":") catch {};
        j.raw("{d}", .{kind});
        j.buf.appendSlice(gpa, ",\"range\":{\"start\":{\"line\":") catch {};
        j.raw("{d}", .{ln});
        j.buf.appendSlice(gpa, ",\"character\":0},\"end\":{\"line\":") catch {};
        j.raw("{d}", .{ln});
        j.buf.appendSlice(gpa, ",\"character\":") catch {};
        j.raw("{d}", .{line.len});
        j.buf.appendSlice(gpa, "}},\"selectionRange\":{\"start\":{\"line\":") catch {};
        j.raw("{d}", .{ln});
        j.buf.appendSlice(gpa, ",\"character\":") catch {};
        j.raw("{d}", .{t});
        j.buf.appendSlice(gpa, "},\"end\":{\"line\":") catch {};
        j.raw("{d}", .{ln});
        j.buf.appendSlice(gpa, ",\"character\":") catch {};
        j.raw("{d}", .{n});
        j.buf.appendSlice(gpa, "}}}") catch {};
    }
    j.buf.append(gpa, ']') catch {};
    return j.buf.items;
}

/// 文本里 word 的词边界出现位置回调（行、列 0-based）。跳过 skip_line/skip_col
/// 处的那一次（includeDeclaration=false 时用来剔声明本身）。
fn eachWordUse(text: []const u8, word: []const u8, skip_line: ?u32, skip_col: ?usize, ctx: anytype, comptime cb: fn (@TypeOf(ctx), u32, usize) void) void {
    var ln: u32 = 0;
    var ls: usize = 0;
    var i: usize = 0;
    while (i <= text.len) : (i += 1) {
        if (i == text.len or text[i] == '\n') {
            const line = text[ls..i];
            var p: usize = 0;
            while (std.mem.indexOfPos(u8, line, p, word)) |hit| {
                p = hit + 1;
                if (hit > 0 and isIdentChar(line[hit - 1])) continue;
                const end = hit + word.len;
                if (end < line.len and isIdentChar(line[end])) continue;
                if (skip_line != null and skip_line.? == ln and skip_col != null and skip_col.? == hit) continue;
                cb(ctx, ln, hit);
            }
            ln += 1;
            ls = i + 1;
        }
    }
}

const RefSink = struct {
    j: *Json,
    first: *bool,
    uri: []const u8,
    count: *usize,

    fn add(self: RefSink, ln: u32, col: usize) void {
        if (self.count.* >= 200) return;
        self.count.* += 1;
        if (!self.first.*) self.j.buf.append(gpa, ',') catch {};
        self.first.* = false;
        self.j.buf.appendSlice(gpa, "{\"uri\":") catch {};
        self.j.str(self.uri);
        self.j.buf.appendSlice(gpa, ",\"range\":{\"start\":{\"line\":") catch {};
        self.j.raw("{d}", .{ln});
        self.j.buf.appendSlice(gpa, ",\"character\":") catch {};
        self.j.raw("{d}", .{col});
        self.j.buf.appendSlice(gpa, "},\"end\":{\"line\":") catch {};
        self.j.raw("{d}", .{ln});
        self.j.buf.appendSlice(gpa, ",\"character\":") catch {};
        self.j.raw("{d}", .{col + 1});
        self.j.buf.appendSlice(gpa, "}}}") catch {};
    }
};

/// 引用查找：当前文档 + 工作区 .zx 文件的词边界扫描。文本级（不做语义判别，
/// 同名局部变量会混入），引用量大时截 200 条。
fn referencesAt(doc: *const Document, line: u32, character: u32, include_decl: bool) ?[]const u8 {
    const text = doc.text;
    const off = posToOffset(text, line, character);
    var ws = off;
    while (ws > 0 and isIdentChar(text[ws - 1])) ws -= 1;
    var we = off;
    while (we < text.len and isIdentChar(text[we])) we += 1;
    const word = text[ws..we];
    if (word.len == 0) return null;

    var j = Json{};
    j.buf.append(gpa, '[') catch {};
    var first = true;
    var count: usize = 0;

    // 当前文档；includeDeclaration=false 时剔掉声明位置那一处
    var skip_line: ?u32 = null;
    var skip_col: ?usize = null;
    if (!include_decl) {
        if (findDeclLine(text, word)) |dl| {
            skip_line = dl.line;
            skip_col = dl.col;
        }
    }
    const sink = RefSink{ .j = &j, .first = &first, .uri = doc.uri, .count = &count };
    eachWordUse(text, word, skip_line, skip_col, sink, RefSink.add);

    // 工作区其他文件（同目录 + root，≤2 层，与补全同一套扫描）
    const doc_dir = dirName(doc.path);
    var searched: std.StringHashMapUnmanaged(void) = .empty;
    defer searched.deinit(gpa);
    var dirs: [2][]const u8 = undefined;
    var nd: usize = 0;
    dirs[nd] = doc_dir;
    nd += 1;
    if (root_path) |r| {
        if (!std.mem.eql(u8, r, doc_dir)) {
            dirs[nd] = r;
            nd += 1;
        }
    }
    const self_name = baseName(doc.path);
    for (dirs[0..nd]) |d| {
        if (searched.contains(d)) continue;
        searched.put(gpa, d, {}) catch {};
        var dir = Io.Dir.cwd().openDir(g_io, d, .{ .iterate = true }) catch continue;
        defer dir.close(g_io);
        var it = dir.iterate();
        while (it.next(g_io) catch null) |entry| {
            if (entry.kind != .file or !std.mem.endsWith(u8, entry.name, ".zx")) continue;
            if (std.mem.eql(u8, entry.name, self_name)) continue;
            var fp: std.ArrayList(u8) = .empty;
            defer fp.deinit(gpa);
            fp.appendSlice(gpa, d) catch continue;
            fp.append(gpa, '/') catch continue;
            fp.appendSlice(gpa, entry.name) catch continue;
            const txt = readFileSmall(fp.items) orelse continue;
            const sink2 = RefSink{ .j = &j, .first = &first, .uri = pathToUri(fp.items), .count = &count };
            eachWordUse(txt, word, null, null, sink2, RefSink.add);
        }
    }
    j.buf.append(gpa, ']') catch {};
    return j.buf.items;
}

// ---------------------------------------------------------------- hover / signature

fn trimSl(s: []const u8) []const u8 {
    var a: usize = 0;
    var b: usize = s.len;
    while (a < b and (s[a] == ' ' or s[a] == '\t' or s[a] == '\r' or s[a] == '\n')) a += 1;
    while (b > a and (s[b - 1] == ' ' or s[b - 1] == '\t' or s[b - 1] == '\r' or s[b - 1] == '\n')) b -= 1;
    return s[a..b];
}

/// 找声明语句，返回从关键字到顶层 ';' 的整段。
/// const / let / var 的 `<name> = …`、extern var 的 `<name>: T;` 都认；
/// KEEP: `pub fn NAME(...)` 形式也认——extern "C" 块里没有 `=`，以前这里
/// 一律返回 null，模块成员的 hover / 签名帮助 / 参数名 inlay 全跟着哑掉。
fn findDeclStmt(text: []const u8, name: []const u8) ?[]const u8 {
    const kws = [_][]const u8{ "const ", "let ", "var ", "fn " };
    for (kws) |kw| {
        var from: usize = 0;
        while (std.mem.indexOfPos(u8, text, from, kw)) |c| {
            from = c + 1;
            if (c > 0 and isIdentChar(text[c - 1])) continue; // obsolete 之类
            if (inLineComment(text, c)) continue;
            if (declStmtAt(text, c, kw, name)) |st| return st;
        }
    }
    return null;
}

/// c 指向声明关键字（kw）的起点。校验名字、声明形态（`=` / `:` / `(`），
/// 往前吞 `pub ` / `extern `，往后到顶层 `;`，返回声明语句切片。
/// findDeclStmt 与 DeclIndex 共用这一段，保证两种查找方式结果一致。
fn declStmtAt(text: []const u8, c: usize, kw: []const u8, name: []const u8) ?[]const u8 {
    {
            var j = c + kw.len;
            while (j < text.len and (text[j] == ' ' or text[j] == '\t')) j += 1;
            const ns = j;
            while (j < text.len and isIdentChar(text[j])) j += 1;
            if (j == ns or !std.mem.eql(u8, text[ns..j], name)) return null;
            while (j < text.len and (text[j] == ' ' or text[j] == '\t')) j += 1;
            if (j >= text.len) return null;
            if (std.mem.eql(u8, kw, "fn ")) {
                if (text[j] != '(') return null;
            } else if (text[j] != '=' and text[j] != ':') return null;
            // 往前吞掉 `pub ` / `extern ` 修饰，返回片段从声明开头起
            var start = c;
            var guard: usize = 0;
            while (guard < 3) : (guard += 1) {
                if (start >= 4 and std.mem.eql(u8, text[start - 4 .. start], "pub ")) {
                    start -= 4;
                    continue;
                }
                if (start >= 7 and std.mem.eql(u8, text[start - 7 .. start], "extern ")) {
                    start -= 7;
                    continue;
                }
                break;
            }
            // 到顶层 ';'（跳过字符串、跟踪嵌套深度）
            var depth: usize = 0;
            var in_str = false;
            var k = j;
            while (k < text.len) : (k += 1) {
                const ch = text[k];
                if (in_str) {
                    if (ch == '\\') {
                        k += 1;
                        continue;
                    }
                    if (ch == '"') in_str = false;
                    continue;
                }
                switch (ch) {
                    '"' => in_str = true,
                    '{', '(', '[' => depth += 1,
                    '}', ')', ']' => {
                        if (depth > 0) depth -= 1;
                    },
                    ';' => {
                        if (depth == 0) return text[start .. k + 1];
                    },
                    else => {},
                }
            }
            return text[start..];
    }
    return null;
}

/// 容器体**内部**的成员声明：`const spawn = fn (self: *const Self, …) …;`。
///
/// 拼签名 / 判 self / 出调用骨架都必须用它，不能拿整个声明文件去
/// findDeclStmt(名字)：两个结构体有同名成员时（`Task.spawn` 与
/// `Group.spawn`），全文查找拿到的是文件里第一个，签名和 self 判定都会串
/// ——`t.spawn` 的 detail 曾显示成 Group 的 `spawn(…) bool`，看上去就像
/// 实例上冒出个关联函数（用户 2026-11-10 报的正是这个）。
///
/// 只认容器体**顶层**（相对深度 1）的声明：嵌套 struct 的同名成员不算。
/// 容器体里这个名字的声明带没带 `pub`。补全要跟编译器一样严：成员默认
/// 私有（只在声明它的文件里可见），别的文件里不该把它们摆出来。
/// 找不到声明就返回 true（宁可多提示，也不要凭猜测把成员藏掉）。
fn memberPubIn(body: []const u8, name: []const u8) bool {
    var it = std.mem.splitScalar(u8, body, '\n');
    while (it.next()) |raw| {
        var line = std.mem.trimStart(u8, raw, " \t");
        const is_pub = std.mem.startsWith(u8, line, "pub ");
        if (is_pub) line = std.mem.trimStart(u8, line[4..], " \t");
        if (std.mem.startsWith(u8, line, "const ")) line = std.mem.trimStart(u8, line[6..], " \t");
        if (std.mem.startsWith(u8, line, "let ")) line = std.mem.trimStart(u8, line[4..], " \t");
        if (std.mem.startsWith(u8, line, "var ")) line = std.mem.trimStart(u8, line[4..], " \t");
        if (!std.mem.startsWith(u8, line, name)) continue;
        var k = name.len;
        while (k < line.len and (line[k] == ' ' or line[k] == '\t')) k += 1;
        if (k < line.len and (line[k] == ':' or line[k] == '='))
            return is_pub;
    }
    return true;
}

fn findMemberStmt(body: []const u8, name: []const u8) ?[]const u8 {
    const kws = [_][]const u8{ "const ", "let ", "var ", "fn " };
    var depth: usize = 0;
    var in_str = false;
    var i: usize = 0;
    while (i < body.len) : (i += 1) {
        const c = body[i];
        if (in_str) {
            if (c == '\\') { i += 1; continue; }
            if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; continue; }
        if (c == '/' and i + 1 < body.len and body[i + 1] == '/') {
            while (i < body.len and body[i] != '\n') i += 1;
            continue;
        }
        if (c == '{') { depth += 1; continue; }
        if (c == '}') { if (depth > 0) depth -= 1; continue; }
        if (depth != 1) continue;                    // 只看容器体顶层
        if (i > 0 and isIdentChar(body[i - 1])) continue;
        for (kws) |kw| {
            if (!std.mem.startsWith(u8, body[i..], kw)) continue;
            if (declStmtAt(body, i, kw, name)) |st| return st;
        }
    }
    return null;
}

/// 本文件声明索引：单趟扫描，把每个「语句开头的 const/let/var/fn + 名字」
/// 记成 name → 关键字偏移。此前无点号的被调方每查一个名字就 findDeclStmt
/// 全文扫 4 遍（4 个关键字 × indexOfPos 到文件尾），名字数 × 文件长度是
/// 平方——12000 个不同名函数实测 11.5s。建一次索引后每个名字只验证它自己
/// 的候选位置。索引只活在一次 inlay 请求里（InlayCtx），键指向 text。
const DeclIndex = struct {
    map: std.StringHashMapUnmanaged(std.ArrayListUnmanaged(usize)) = .empty,

    fn build(text: []const u8) DeclIndex {
        var self = DeclIndex{};
        var i: usize = 0;
        while (i < text.len) : (i += 1) {
            // 关键字首字符过滤：绝大多数位置这里一步跳过
            const kw: []const u8 = switch (text[i]) {
                'c' => if (std.mem.startsWith(u8, text[i..], "const ")) "const " else continue,
                'l' => if (std.mem.startsWith(u8, text[i..], "let ")) "let " else continue,
                'v' => if (std.mem.startsWith(u8, text[i..], "var ")) "var " else continue,
                'f' => if (std.mem.startsWith(u8, text[i..], "fn ")) "fn " else continue,
                else => continue,
            };
            if (i > 0 and isIdentChar(text[i - 1])) continue; // obsolete 之类
            if (inLineComment(text, i)) continue;
            var j = i + kw.len;
            while (j < text.len and (text[j] == ' ' or text[j] == '\t')) j += 1;
            const ns = j;
            while (j < text.len and isIdentChar(text[j])) j += 1;
            if (j == ns) continue;
            const gop = self.map.getOrPut(gpa, text[ns..j]) catch continue;
            if (!gop.found_existing) gop.value_ptr.* = .empty;
            gop.value_ptr.append(gpa, i) catch {};
        }
        return self;
    }
};

/// fn 签名解析：`pub const Add = fn(a:i32,b:i32) i32{…};` →
/// name="Add"、params=["a:i32","b:i32"]、ret="i32"（切片都指向 stmt）
const FnSig = struct {
    name: []const u8,
    params: []const []const u8,
    ret: []const u8,
    is_fn: bool,
};

fn sigOfStmt(stmt: []const u8) FnSig {
    var r = FnSig{ .name = "", .params = &.{}, .ret = "", .is_fn = false };
    // name：`const NAME = fn(...)` 或 extern 块的 `pub fn NAME(...)`。
    // KEEP: 只在**声明头部**找，不能全文搜 "const "——C 绑定里到处是
    // `*const u8` 形参，旧版会把这类函数 hover 成 `u8(title: *const u8)`。
    var head = std.mem.trimStart(u8, stmt, " \t\r\n");
    if (std.mem.startsWith(u8, head, "pub ")) head = std.mem.trimStart(u8, head[4..], " \t");
    if (std.mem.startsWith(u8, head, "extern ")) head = std.mem.trimStart(u8, head[7..], " \t");
    const npre: usize = if (std.mem.startsWith(u8, head, "const ")) 6
        else if (std.mem.startsWith(u8, head, "fn ")) 3
        else 0;
    if (npre > 0) {
        var j = npre;
        while (j < head.len and (head[j] == ' ' or head[j] == '\t')) j += 1;
        const ns = j;
        while (j < head.len and isIdentChar(head[j])) j += 1;
        r.name = head[ns..j];
    }
    // 容器声明（`const X = struct { … }` / union / enum）：体内一堆方法都带
    // `fn `，下面的搜索会把整条声明认成函数，hover 于是吐第一个方法的签名
    //（`std.Task` 显示成 `single() Self`、`std.task.Task` 显示成 `Task() Self`）。
    // 容器不是函数：值不是 fn 就直接收手，交给 hover 的容器分支贴完整定义。
    if (std.mem.indexOfScalar(u8, head, '=')) |eq| {
        var vp = eq + 1;
        while (vp < head.len and (head[vp] == ' ' or head[vp] == '\t')) vp += 1;
        const vs = head[vp..];
        if (std.mem.startsWith(u8, vs, "struct") or
            std.mem.startsWith(u8, vs, "union") or
            std.mem.startsWith(u8, vs, "enum")) return r;
    }
    // fn?
    const fp = std.mem.indexOf(u8, stmt, "fn ") orelse std.mem.indexOf(u8, stmt, "fn(");
    if (fp == null) return r;
    const lp0 = std.mem.indexOfScalarPos(u8, stmt, fp.?, '(') orelse return r;
    // 匹配 ')'
    var depth: usize = 0;
    var rp = lp0;
    var in_str = false;
    while (rp < stmt.len) : (rp += 1) {
        const ch = stmt[rp];
        if (in_str) {
            if (ch == '\\') {
                rp += 1;
                continue;
            }
            if (ch == '"') in_str = false;
            continue;
        }
        if (ch == '"') {
            in_str = true;
            continue;
        }
        if (ch == '(') depth += 1;
        if (ch == ')') {
            depth -= 1;
            if (depth == 0) break;
        }
    }
    if (rp >= stmt.len) return r;
    r.is_fn = true;
    // 参数：顶层逗号切分
    var plist: std.ArrayList([]const u8) = .empty;
    var seg: usize = lp0 + 1;
    depth = 0;
    in_str = false;
    var q = lp0 + 1;
    while (q < rp) : (q += 1) {
        const ch = stmt[q];
        if (in_str) {
            if (ch == '\\') {
                q += 1;
                continue;
            }
            if (ch == '"') in_str = false;
            continue;
        }
        if (ch == '"') {
            in_str = true;
            continue;
        }
        if (ch == '(' or ch == '[' or ch == '{') depth += 1;
        if (ch == ')' or ch == ']' or ch == '}') {
            if (depth > 0) depth -= 1;
        }
        if (ch == ',' and depth == 0) {
            const t = trimSl(stmt[seg..q]);
            if (t.len > 0) plist.append(gpa, t) catch {};
            seg = q + 1;
        }
    }
    {
        const t = trimSl(stmt[seg..rp]);
        if (t.len > 0) plist.append(gpa, t) catch {};
    }
    r.params = plist.items;
    // 返回类型：')' 之后到 '{' 或 ';' 或行尾
    var e = rp + 1;
    while (e < stmt.len and (stmt[e] == ' ' or stmt[e] == '\t')) e += 1;
    const rs = e;
    while (e < stmt.len and stmt[e] != '{' and stmt[e] != ';' and stmt[e] != '\n') e += 1;
    r.ret = trimSl(stmt[rs..e]);
    return r;
}

/// 字面量/表达式 → 类型名。text 用于解析调用目标的声明；doc 用于模块解析。
/// 内联 struct 形参的字段类型：`info: struct{a:i32,b:u32}` → `info.a` 是 i32。
/// 这类类型没有容器声明可查，只能按 `name: struct{…}` 形状现场解析。
fn inlineStructFieldTy(text: []const u8, base: []const u8, member: []const u8) ?[]const u8 {
    var from: usize = 0;
    while (std.mem.indexOfPos(u8, text, from, base)) |c| {
        from = c + 1;
        if (c > 0 and isIdentChar(text[c - 1])) continue;
        var j = c + base.len;
        while (j < text.len and (text[j] == ' ' or text[j] == '\t')) j += 1;
        if (j >= text.len or text[j] != ':') continue;
        j += 1;
        while (j < text.len and (text[j] == ' ' or text[j] == '\t')) j += 1;
        if (j >= text.len or !std.mem.startsWith(u8, text[j..], "struct")) continue;
        const lb = std.mem.indexOfScalarPos(u8, text, j, '{') orelse continue;
        var depth: usize = 0;
        var rb = text.len;
        var k = lb;
        while (k < text.len) : (k += 1) {
            if (text[k] == '{') depth += 1;
            if (text[k] == '}') {
                depth -= 1;
                if (depth == 0) {
                    rb = k;
                    break;
                }
            }
        }
        if (rb >= text.len) continue;
        // 顶层逗号切分，每项形如 `a: i32`
        var seg = lb + 1;
        var d2: usize = 0;
        var q = lb + 1;
        while (q <= rb) : (q += 1) {
            const at_end = q == rb;
            if (!at_end) {
                const ch = text[q];
                if (ch == '{' or ch == '(' or ch == '[') {
                    d2 += 1;
                    continue;
                }
                if (ch == '}' or ch == ')' or ch == ']') {
                    if (d2 > 0) d2 -= 1;
                    continue;
                }
                if (ch != ',' or d2 != 0) continue;
            }
            const item = trimSl(text[seg..q]);
            if (std.mem.indexOfScalar(u8, item, ':')) |colon| {
                if (std.mem.eql(u8, trimSl(item[0..colon]), member)) {
                    const t = trimSl(item[colon + 1 ..]);
                    if (t.len > 0) return t;
                }
            }
            seg = q + 1;
        }
    }
    return null;
}

/// 匿名 struct 字段初值的类型。cast 前缀 `(u32)expr` 直接取目标类型；
/// 其余交给 inferExprType（字面量、成员访问、调用返回值等）。
fn inferAnonFieldTy(text: []const u8, doc: *const Document, val: []const u8, depth: u8) ?[]const u8 {
    const v = trimSl(val);
    if (v.len > 2 and v[0] == '(') {
        if (std.mem.indexOfScalar(u8, v, ')')) |rp| {
            const t = trimSl(v[1..rp]);
            var ok = t.len > 0 and t.len <= 24 and isIdentChar(t[0]);
            for (t) |ch| {
                if (!isIdentChar(ch)) { ok = false; break; }
            }
            if (ok) return t;
        }
    }
    return inferExprType(text, doc, v, depth + 1);
}

/// 容器体里的成员是不是**类型成员**：嵌套 `const Inner = struct {…}`、
/// `const Inner = Other;`、`const Inner = m.A.B;`。
/// 是的话这个表达式的值就是一个类型——行内提示该写 `: type`
///（用户 2026-11-11："变量接收类型时不提示类型"）。
fn memberIsTypeDecl(files: []const []const u8, body: []const u8, member: []const u8) bool {
    const st = findMemberStmt(body, member) orelse return false;
    if (declTyOf(st)) |dt| {
        if (std.mem.eql(u8, trimSl(dt), "type")) return true;
    }
    if (containerInitOf(st) != null) return true;
    const ini0 = trimSl(declInitOf(st) orelse return false);
    if (ini0.len == 0) return false;
    if (isTypePathExpr(ini0)) return true;
    if (std.mem.indexOfScalar(u8, ini0, '.') == null) {
        var j: usize = 0;
        while (j < ini0.len and isIdentChar(ini0[j])) j += 1;
        if (j == ini0.len) {
            return findContainerBody(files, ini0) != null or genericBodyOf(files, ini0) != null;
        }
    }
    return false;
}

fn inferExprType(text: []const u8, doc: *const Document, expr: []const u8, depth: u8) ?[]const u8 {
    const e = trimSl(expr);
    if (e.len == 0) return null;
    if (std.mem.startsWith(u8, e, "module(")) return null;
    // `@type(x)` 是编译器内建描述符（src/sema.c 的 sema_type_type），值是
    // `Type`；`@type(x).fields` 之类按内建字段表取。没有声明文本可查，所以
    // 必须在下面"调用"分支之前拦住——那边会把 @type 当普通函数、查不到
    // 返回类型就返回 null，于是 `const info = …` 没有 `: Type`、`info.` 空列表
    //（用户 2026-11-11 第 2 张截图）。
    if (std.mem.startsWith(u8, e, "@type(")) {
        var i: usize = 6;
        var d: usize = 0;
        var rb: ?usize = null;
        while (i < e.len) : (i += 1) {
            if (e[i] == '(') d += 1;
            if (e[i] == ')') {
                if (d == 0) {
                    rb = i;
                    break;
                }
                d -= 1;
            }
        }
        const close = rb orelse return null;
        const rest = trimSl(e[close + 1 ..]);
        if (rest.len == 0) return "Type";
        if (rest[0] != '.') return null;
        return builtinFieldTy("Type", trimSl(rest[1..]));
    }
    // 编译器 seed 的分配器值：sema 里 page_s.ty = sema_allocator_type()。
    // 不特判的话 `var b = std.heap.page; b.` 会被当成类型名 page 而落空。
    if (std.mem.eql(u8, e, "std.heap.page")) return "Allocator";
    if (std.mem.startsWith(u8, e, "\"")) return "str";
    if (std.mem.eql(u8, e, "true") or std.mem.eql(u8, e, "false")) return "bool";
    // 数字
    {
        var all_digit = e.len > 0;
        var has_dot = false;
        for (e, 0..) |ch, idx| {
            if (idx == 0 and ch == '-') continue;
            if (ch == '.') {
                has_dot = true;
                continue;
            }
            if (ch < '0' or ch > '9') {
                all_digit = false;
                break;
            }
        }
        if (all_digit) return if (has_dot) "f64" else "i32";
    }
    // 可选解包：`buf.?` → 内层类型去掉一层 `?` / `!`
    //（用户 2026-11-09 第 14 条：`var b = buf.?` 没有类型提示）
    if (std.mem.endsWith(u8, e, ".?")) {
        if (depth < 4) {
            if (inferExprType(text, doc, e[0 .. e.len - 2], depth + 1)) |t0| {
                var t = trimSl(t0);
                var g: usize = 0;
                while (t.len > 1 and (t[0] == '?' or t[0] == '!') and g < 4) : (g += 1)
                    t = trimSl(t[1..]);
                if (t.len > 0) return t;
            }
        }
        return null;
    }
    // 取地址的数组字面量：`&.{ "a", "b" }` → `*[N]T`（元素类型按第一项推）。
    // 用户 2026-11-09 第 16 条：`const s = strs.0` 推不出类型。
    if (e.len > 1 and e[0] == '&' and depth < 4) {
        const inner = trimSl(e[1..]);
        if (std.mem.startsWith(u8, inner, ".{")) {
            if (inferExprType(text, doc, inner, depth + 1)) |tt| {
                const t = trimSl(tt);
                if (t.len > 2 and t[0] == '{' and t[t.len - 1] == '}') {
                    const body = t[1 .. t.len - 1];
                    var n: usize = 0;
                    var elem: []const u8 = "";
                    var d: usize = 0;
                    var seg: usize = 0;
                    var i: usize = 0;
                    while (i <= body.len) : (i += 1) {
                        const ch: u8 = if (i < body.len) body[i] else ',';
                        if (ch == '{' or ch == '(' or ch == '[') d += 1;
                        if ((ch == '}' or ch == ')' or ch == ']') and d > 0) d -= 1;
                        if (ch != ',' or d != 0) continue;
                        const it2 = trimSl(body[seg..i]);
                        seg = i + 1;
                        if (it2.len == 0) continue;
                        n += 1;
                        if (elem.len == 0) elem = it2;
                    }
                    if (n > 0 and elem.len > 0 and elem.len <= 48)
                        return std.fmt.allocPrint(gpa, "*[{d}]{s}", .{ n, elem }) catch null;
                }
            }
        } else {
            // 取地址：`&p` → `*Person`（用户 2026-11-09 第 17 条：`let pp = &p;`
            // 没有类型提示，连带 `pp.` 之外的整条链都断）。指针再取地址就先
            // 折掉一层，避免 `* *T` 叠出来。
            if (inferExprType(text, doc, inner, depth + 1)) |tt| {
                const t = trimSl(tt);
                if (t.len == 0) return null;
                if (t[0] == '*') return t;
                if (t.len > 60) return null;
                return std.fmt.allocPrint(gpa, "*{s}", .{t}) catch null;
            }
        }
        return null;
    }
    // 下标：`args[0]`、`args[0][0]`、`m[k]` —— 拆一层容器拿元素类型
    //（用户 2026-11-09 第 16 条：`let slice = args[0][0]` 类型丢失）
    if (e[e.len - 1] == ']' and depth < 4) {
        var d: usize = 0;
        var lb: ?usize = null;
        var q: usize = e.len;
        while (q > 0) {
            q -= 1;
            if (e[q] == ']') {
                d += 1;
                continue;
            }
            if (e[q] == '[') {
                d -= 1;
                if (d == 0) {
                    lb = q;
                    break;
                }
            }
        }
        if (lb) |l| {
            if (inferExprType(text, doc, e[0..l], depth + 1)) |bt| {
                if (elemTyOf(bt)) |et| return et;
            }
        }
        return null;
    }
    // 位置访问：`t.0`、`strs.0` —— 元组取第 N 项，定长数组（含数组指针）取元素。
    // 必须在下面的"点路径"分支之前：那边把 `.0` 当成员名（标识符字符含数字）。
    if (std.mem.lastIndexOfScalar(u8, e, '.')) |dot| {
        const tail = e[dot + 1 ..];
        if (tail.len > 0 and tail.len <= 3 and allDigits(tail) and depth < 4) {
            if (inferExprType(text, doc, e[0..dot], depth + 1)) |bt| {
                const idx = std.fmt.parseInt(usize, tail, 10) catch return null;
                if (tupleElemTy(bt, idx)) |t| return t;
                if (elemTyOf(bt)) |t| return t;
            }
            return null;
        }
    }
    // 调用：path(args)。取**末位**那个 '('（`std.map.HashMap(i32).init(x)`
    // 的开括号在 init 前，不是 HashMap 后），callee 用 recvExprStart 跨过
    // 泛型实例化的括号取整条路径（第 14 条：整条没有类型提示）。
    if (finalCallOpen(e)) |lp| {
        const cs = recvExprStart(e, lp);
        const callee = trimSl(e[cs..lp]);
        if (callee.len > 0) {
            if (resolveCalleeRet(text, doc, callee)) |rt| return rt;
        }
        return null;
    }
    // 匿名 struct 字面量 `.{ .a = 1, .b = "x" }`：没有期望类型时自成
    // 一个匿名结构体，字段类型由初值推（spec/decisions.md §33）。
    // 元素式 `.{1, 2.5}` 是元组 → `{i32,f64}`（§4.1，同层不可混用）。
    // 字段值递归推（depth < 4），嵌套拼成
    // `struct{pos:struct{x:i32,y:i32},tag:str}` 给行内类型提示用。
    if (std.mem.startsWith(u8, e, ".{") and depth < 4) {
        var d: usize = 0;
        var in_str = false;
        var rb: ?usize = null;
        var q: usize = 1;
        while (q < e.len) : (q += 1) {
            const ch = e[q];
            if (in_str) {
                if (ch == '\\') { q += 1; continue; }
                if (ch == '"') in_str = false;
                continue;
            }
            if (ch == '"') { in_str = true; continue; }
            if (ch == '{') { d += 1; continue; }
            if (ch == '}') {
                if (d > 0) d -= 1;
                if (d == 0) { rb = q; break; }
                continue;
            }
        }
        const end = rb orelse return null;
        const body = e[2..end];
        const Item = struct { fname: []const u8, val: []const u8, field: bool };
        var items: std.ArrayList(Item) = .empty;
        var seg: usize = 0;
        d = 0;
        in_str = false;
        var i: usize = 0;
        while (i <= body.len) : (i += 1) {
            const at_end = i == body.len;
            if (!at_end) {
                const ch = body[i];
                if (in_str) {
                    if (ch == '\\') { i += 1; continue; }
                    if (ch == '"') in_str = false;
                    continue;
                }
                if (ch == '"') { in_str = true; continue; }
                if (ch == '{' or ch == '(' or ch == '[') { d += 1; continue; }
                if (ch == '}' or ch == ')' or ch == ']') { if (d > 0) d -= 1; continue; }
                if (ch != ',' or d != 0) continue;
            }
            const item = trimSl(body[seg..i]);
            seg = i + 1;
            if (item.len == 0) continue;
            var fname: []const u8 = "";
            var val = item;
            var field = false;
            if (item[0] == '.') {
                var ni: usize = 1;
                while (ni < item.len and isIdentChar(item[ni])) ni += 1;
                if (ni > 1) {
                    const eqp = std.mem.indexOfScalarPos(u8, item, ni, '=') orelse continue;
                    fname = item[1..ni];
                    val = trimSl(item[eqp + 1 ..]);
                    field = true;
                }
            }
            items.append(gpa, .{ .fname = fname, .val = val, .field = field }) catch {};
        }
        if (items.items.len == 0) return null;
        var any_field = false;
        for (items.items) |it2| {
            if (it2.field) { any_field = true; break; }
        }
        var out: std.ArrayList(u8) = .empty;
        out.appendSlice(gpa, if (any_field) "struct{" else "{") catch return null;
        var count: usize = 0;
        for (items.items) |it2| {
            if (any_field and !it2.field) return null; // 字段式/元素式混用不猜
            const t = inferAnonFieldTy(text, doc, it2.val, depth) orelse return null;
            if (t.len == 0 or t.len > 48) return null;
            if (count > 0) out.append(gpa, ',') catch return null;
            if (any_field) {
                out.print(gpa, "{s}:{s}", .{ it2.fname, t }) catch return null;
            } else {
                out.appendSlice(gpa, t) catch return null;
            }
            count += 1;
            if (count >= 8) break;
        }
        if (count == 0) return null;
        out.append(gpa, '}') catch return null;
        if (out.items.len > 200) return null;
        return out.items;
    }
    // 结构体字面量：X{…} → X（X 是扫描集里的容器）
    // var p = Person{.name = …} 的类型就是 Person。
    if (std.mem.indexOfScalar(u8, e, '{')) |lb| {
        var ok = lb > 0;
        var k: usize = 0;
        while (k < lb) : (k += 1) {
            if (e[k] == '.') continue;
            if (!isIdentChar(e[k]) or (e[k] >= '0' and e[k] <= '9' and k == 0)) {
                ok = false;
                break;
            }
        }
        if (ok) {
            const nm = e[0..lb];
            var sf = scanFiles(doc, text);
            defer sf.deinit(gpa);
            if (isContainerIn(sf.items, nm)) return nm;
            // 点路径容器：`std.fs.Stat{ … }` → Stat
            if (std.mem.indexOfScalar(u8, nm, '.') != null) {
                if (resolveMemberPath(sf.items, doc, text, nm, 0)) |hit| {
                    if (containerInitOf(hit.stmt) != null) {
                        const dot = std.mem.lastIndexOfScalar(u8, nm, '.') orelse return null;
                        return nm[dot + 1 ..];
                    }
                }
            }
        }
    }
    // 点路径分两类：类型名（std.Buffer、task.Obj）原样返回；
    // 实例成员访问（p.age / g.spawn）解析成字段类型或方法返回类型。
    // 此前一律原样返回，const n = p.age 的 hover 会把 "p.age" 当类型名。
    if (std.mem.indexOfScalar(u8, e, '.') != null) {
        var ok = true;
        for (e) |ch| {
            // 接收者也可以是表达式：`arrs[0].name`、`f(x).name`、`buf.?.len`。
            // 只放行下标/调用/解包这三种"后缀形状"的字符，运算符仍挡在外面。
            if (!isIdentChar(ch) and ch != '.' and ch != '[' and ch != ']' and
                ch != '(' and ch != ')' and ch != '?') {
                ok = false;
                break;
            }
        }
        if (ok) {
            const dot = std.mem.lastIndexOfScalar(u8, e, '.') orelse return null;
            const base = e[0..dot];
            const member = e[dot + 1 ..];
            if (base.len == 0 or member.len == 0) return null;
            var sf = scanFiles(doc, text);
            defer sf.deinit(gpa);
            // base 是模块绑定（std / const t = module(…)）：成员是容器/类型
            // 就写 `type`，函数值/普通常量就不再往下猜。以前一律 `return e`——
            // 类型路径原样返回，于是 `const P = t.Inner;` 推出来的是 "t.Inner"
            // 这串**文本**，行内提示要么不显示（和初值同名被当成冗余），要么
            // 显示一长串模块路径（用户 2026-11-11 第 4 张截图）。
            // 子模块别名（std.json / std.time）落不下去，仍按原样返回。
            if (std.mem.eql(u8, base, "std") or moduleOfBinding(text, base) != null) {
                if (moduleTextOf(doc, text, base)) |mt| {
                    // 模块成员：类型 → `type`；函数值/普通常量 → 推不出（返回
                    // null，别再退回整条路径文本 `t.cnt` 当类型）。
                    if (moduleMemberTy(sf.items, doc, mt, member)) |t| return t;
                    return null;
                }
                return e;
            }
            // 类型值接收者（`const P = t.p1.P2T;` 之后的 `P.P3T`）：容器是
            // 它背后的那个类型；成员本身还是类型就写 `type`，是字段出字段类型。
            if (isTypeValuedRecv(sf.items, doc, text, base, 0)) {
                // 容器体优先走 containerOfBinding：它沿绑定的**初值路径**逐段
                // 解析，体可以在扫描集之外的文件里（`const P: type = t.P1.P2;`
                // 的 P2 在 vendor/test.zx，scanFiles 只扫文档目录 + std，
                // findContainerBody 必然落空 → `const pp = P.P3;` 没有 `: type`、
                // `P.P3.f3(…)` 也推不出返回类型）。用户 2026-11-11 第 7 张截图。
                if (containerOfBinding(sf.items, doc, text, base, 0)) |pc| {
                    if (memberIsTypeDecl(sf.items, pc.body, member)) return "type";
                    if (fieldTypeIn(pc.body, member)) |t| return t;
                    if (findDeclStmt(pc.body, member)) |st3| {
                        const sg3 = sigOfStmt(st3);
                        if (sg3.is_fn and sg3.ret.len > 0) return sg3.ret;
                    }
                    return null;
                }
                if (typeValuedContainer(sf.items, doc, text, base, 0)) |tc| {
                    if (findContainerBody(sf.items, tc)) |ch| {
                        if (memberIsTypeDecl(sf.items, ch.body, member)) return "type";
                        if (fieldTypeIn(ch.body, member)) |t| return t;
                    } else if (genericBodyOf(sf.items, tc)) |gb| {
                        if (memberIsTypeDecl(sf.items, gb, member)) return "type";
                    }
                }
                return null;
            }
            // 接收者是**链式子表达式**时也落到容器：`t.P1`（实例上取类型成员）、
            // `t.P1.P2`（再取一层）。isTypeValuedRecv 只覆盖"绑定名 → 类型值"，
            // base 是 `t.P1` 这种链就认不出，`const P = t.P1.P2;` 整条推不出类型、
            // 也就没有 `: type`（用户 2026-11-11 截图 1）。
            // `t` 是 module 时走的是同一条链的**另一头**：`t.P1` 的 base 是
            // "模块名.容器名"，既不是模块绑定也不是本地容器，必须让模块文件
            // 也进解析（moduleTextOf），否则 `const P = t.P1.P2;` 只有模块那条
            // 分支能返回、落到这里就被跳过（用户 2026-11-11 第 4 张截图）。
            if (isPurePath(base)) {
                if (moduleTextOf(doc, text, base)) |mt| {
                    if (moduleMemberTy(sf.items, doc, mt, member)) |t| return t;
                }
                if (resolveContainerPath(sf.items, doc, text, base) orelse
                    resolveContainerPathLoose(sf.items, doc, text, base)) |pc| {
                    if (memberIsTypeDecl(sf.items, pc.body, member)) return "type";
                    if (fieldTypeIn(pc.body, member)) |t| return t;
                    if (findDeclStmt(pc.body, member)) |stmt| {
                        const sig = sigOfStmt(stmt);
                        if (sig.is_fn and sig.ret.len > 0) return sig.ret;
                    }
                }
            }
            // 泛型形参接收者（§117）：`val.a` 的 base 是 `T: type` 的形参，
            // 从调用点反推容器再读字段类型 / 方法返回类型。没有光标位置，
            // 取第一份匹配的形参声明（同名形参的歧义在实践中罕见）。
            if (depth < 4 and isPurePath(base)) {
                if (genericParamRecvTy(sf.items, doc, text, base, 0)) |pc| {
                    if (memberIsTypeDecl(sf.items, pc.body, member)) return "type";
                    if (fieldTypeIn(pc.body, member)) |t| return t;
                    if (findDeclStmt(pc.body, member)) |stmt| {
                        const sig = sigOfStmt(stmt);
                        if (sig.is_fn and sig.ret.len > 0) return sig.ret;
                    }
                }
            }
            // 实例成员 → 容器体里的字段类型 / 方法返回类型
            if (instanceContainerTy(sf.items, text, base)) |ty| {
                if (findContainerBody(sf.items, ty)) |ch| {
                    if (fieldTypeIn(ch.body, member)) |t| return t;
                    if (memberIsTypeDecl(sf.items, ch.body, member)) return "type";
                    if (findDeclStmt(ch.body, member)) |stmt| {
                        const sig = sigOfStmt(stmt);
                        if (sig.is_fn and sig.ret.len > 0) return sig.ret;
                    }
                }
            }
            // 链式接收者（用户 2026-11-09 第 17 条）：`arrs.0.name`、
            // `pa.name`（pa 的类型本身是推出来的）、`pp.0.name`。base 不是
            // 一个"容器名"，但能当表达式推出类型——去指针/可选后取容器名，
            // 再查字段。instanceContainerTy 只认"变量名 → 声明类型"这一跳，
            // 遇到下标/位置访问/推导出来的绑定就断，这里补上。
            if (depth < 4) {
                if (inferExprType(text, doc, base, depth + 1)) |bt| {
                    const cn = containerNameOf(bt);
                    if (cn.len > 0) {
                        if (findContainerBody(sf.items, cn)) |ch2| {
                            if (fieldTypeIn(ch2.body, member)) |t| return t;
                            if (memberIsTypeDecl(sf.items, ch2.body, member)) return "type";
                            if (findDeclStmt(ch2.body, member)) |st2| {
                                const sg2 = sigOfStmt(st2);
                                if (sg2.is_fn and sg2.ret.len > 0) return sg2.ret;
                            }
                        }
                    }
                    // 内建类型（`@type(x)` 描述符 / Allocator）：`info.fields`、
                    // `arena.ctx` 没有声明文本可查，字段表在编译器里
                    //（用户 2026-11-11 第 2 张截图：`info.` 空列表）。
                    // 放在用户容器之后：真有个叫 Type 的容器时以声明为准。
                    if (builtinFieldTy(bt, member)) |t| return t;
                }
            }
            // 形参是内联 struct（info: struct{a:i32,b:u32}）：现场解析字段
            if (inlineStructFieldTy(text, base, member)) |t| return t;
            // self.age / self.count（方法体内）：容器取带 `self:` 形参的容器体
            if (std.mem.eql(u8, base, "self")) {
                if (selfContainerBody(text)) |body| {
                    if (fieldTypeIn(body, member)) |t| return t;
                    if (findDeclStmt(body, member)) |stmt| {
                        const sig = sigOfStmt(stmt);
                        if (sig.is_fn and sig.ret.len > 0) return sig.ret;
                    }
                }
            }
            return null;
        }
    }
    // 裸标识符：显式类型标注优先（`var buf: ?std.BufReader = …` 的 buf），
    // 其次追一层别名（const b = a）。不先看标注的话，`buf.?` 只能拿到
    // init 的 Return 类型里的 `Self`，解包后又丢掉（第 14 条）。
    // 6 跳：`pa` → `arrs.0` → `arrs` → 标注，用户 2026-11-09 第 17 条的
    // `let pa = arrs.0; pa.name` 要跨三跳才拿到 Person。各表达式分支自带
    // depth<4 的帽子，真正的递归深度仍被封住。
    if (depth < 6 and isIdentChar(e[0]) and std.mem.indexOfScalar(u8, e, '.') == null) {
        if (findDeclStmt(text, e)) |stmt2| {
            const init2 = declInitOf(stmt2);
            if (init2) |ival| {
                const iv = trimSl(ival);
                const not_val = std.mem.startsWith(u8, iv, "fn") or
                    std.mem.startsWith(u8, iv, "struct") or
                    std.mem.startsWith(u8, iv, "union") or
                    std.mem.startsWith(u8, iv, "enum") or
                    std.mem.startsWith(u8, iv, "module(");
                if (!not_val) {
                    if (declTyOf(stmt2)) |dt| {
                        const t = trimSl(dt);
                        if (t.len > 0 and t.len <= 120) return t;
                    }
                    return inferExprType(text, doc, iv, depth + 1);
                }
            }
            return null;
        }
        // 捕获绑定（`if (arena) a {` 的 a）：没有声明语句，类型来自条件解包
        if (depth < 4) {
            if (captureBindTy(text, doc, e)) |t| return t;
        }
    }
    return null;
}

/// 末位调用的左括号：`f(a)` → f 后的 '('；`X(T).m(v)` → m 后的 '('。
/// 从尾部配平回扫（跳过字符串），返回与最后一个 ')' 配对的那个 '('。
fn finalCallOpen(e: []const u8) ?usize {
    var stack: [32]usize = undefined;
    var sp: usize = 0;
    var in_str = false;
    var i: usize = 0;
    while (i < e.len) : (i += 1) {
        const ch = e[i];
        if (in_str) {
            if (ch == '\\') {
                i += 1;
                continue;
            }
            if (ch == '"') in_str = false;
            continue;
        }
        if (ch == '"') {
            in_str = true;
            continue;
        }
        if (ch == '(') {
            if (sp < stack.len) {
                stack[sp] = i;
                sp += 1;
            }
            continue;
        }
        if (ch == ')') {
            if (sp == 0) continue;
            sp -= 1;
            if (i == e.len - 1) return stack[sp];
        }
    }
    return null;
}

/// 声明语句 '=' 右侧（不含 ';'）
fn declInitOf(stmt: []const u8) ?[]const u8 {
    const eq = std.mem.indexOfScalar(u8, stmt, '=') orelse return null;
    var body = stmt[eq + 1 ..];
    if (body.len > 0 and body[body.len - 1] == ';') body = body[0 .. body.len - 1];
    return trimSl(body);
}

/// 声明语句上的类型标注（`var b: []const u8 = …` → `[]const u8`）。
/// 只在冒号与（顶层的第一个）`=` 之间取；没标注返回 null。
fn declTyOf(stmt: []const u8) ?[]const u8 {
    const colon = std.mem.indexOfScalar(u8, stmt, ':') orelse return null;
    var end = stmt.len;
    if (std.mem.indexOfScalarPos(u8, stmt, colon + 1, '=')) |eq| end = eq;
    const ty = trimSl(stmt[colon + 1 .. end]);
    if (ty.len == 0) return null;
    return ty;
}

const SliceTy = struct { elem: []const u8, is_const: bool };

/// `[]T` / `[]const T` / `?[]T` / `![]T` → 元素类型与常量性。
/// 元素只认单个标识符（u8 / i32 …），嵌套切片/函数指针返回 null。
fn parseSliceTy(ty0: []const u8) ?SliceTy {
    var t = trimSl(ty0);
    var guard: usize = 0;
    while (t.len > 0 and (t[0] == '?' or t[0] == '!') and guard < 4) : (guard += 1)
        t = trimSl(t[1..]);
    if (!std.mem.startsWith(u8, t, "[]")) return null;
    t = trimSl(t[2..]);
    var is_const = false;
    if (std.mem.startsWith(u8, t, "const ")) {
        is_const = true;
        t = trimSl(t[6..]);
    }
    if (t.len == 0) return null;
    for (t) |c| {
        if (!isIdentChar(c)) return null;
    }
    return .{ .elem = t, .is_const = is_const };
}

fn allDigits(t: []const u8) bool {
    if (t.len == 0) return false;
    for (t) |c| {
        if (c < '0' or c > '9') return false;
    }
    return true;
}

/// 拆一层容器拿元素类型：`[]T` → T、`*[N]T` / `[N]T` → T、`str` → u8。
/// 用户 2026-11-09 第 16 条：`args[0][0]`、`strs.0` 的下标/位置访问要能一路拆到底。
fn elemTyOf(ty0: []const u8) ?[]const u8 {
    var t = trimSl(ty0);
    var g: usize = 0;
    while (t.len > 0 and (t[0] == '?' or t[0] == '!') and g < 4) : (g += 1)
        t = trimSl(t[1..]);
    while (t.len > 0 and t[t.len - 1] == '?') t = trimSl(t[0 .. t.len - 1]);
    if (t.len == 0) return null;
    if (std.mem.eql(u8, t, "str")) return "u8";
    if (std.mem.startsWith(u8, t, "[]")) return trimSl(t[2..]);
    if (t[0] == '*') {
        // `.0` 对指针是解引用（`pp.0` → Person），对数组指针是取元素
        //（`*[2]Person` 的 `.0` → Person）。两种都靠这里分辨。
        const rest = trimSl(t[1..]);
        if (rest.len == 0) return null;
        if (rest[0] == '[') return elemTyOf(rest);
        return rest;
    }
    if (t[0] == '[') {
        const rb = std.mem.indexOfScalar(u8, t, ']') orelse return null;
        const et = trimSl(t[rb + 1 ..]);
        return if (et.len > 0) et else null;
    }
    return null;
}

/// 元组（`{i32,f64}`）的第 N 项；不是元组返回 null。
fn tupleElemTy(ty0: []const u8, idx0: usize) ?[]const u8 {
    var t = trimSl(ty0);
    var g: usize = 0;
    while (t.len > 0 and (t[0] == '?' or t[0] == '!') and g < 4) : (g += 1)
        t = trimSl(t[1..]);
    if (t.len < 3 or t[0] != '{' or t[t.len - 1] != '}') return null;
    const body = t[1 .. t.len - 1];
    var i: usize = 0;
    var d: usize = 0;
    var seg: usize = 0;
    var idx = idx0;
    while (i <= body.len) : (i += 1) {
        const ch: u8 = if (i < body.len) body[i] else ',';
        if (ch == '{' or ch == '(' or ch == '[') d += 1;
        if ((ch == '}' or ch == ')' or ch == ']') and d > 0) d -= 1;
        if (ch != ',' or d != 0) continue;
        const it2 = trimSl(body[seg..i]);
        seg = i + 1;
        if (it2.len == 0) continue;
        if (idx == 0) return it2;
        idx -= 1;
    }
    return null;
}

/// 接收者类型 → 切片形状（`.len` / `.ptr` 的成员补全用）。
/// `[]T` 之外还认 `*[N]T` / `[N]T` / `str`——定长数组同样有 `.len`（编译期常量）
/// 与 `.ptr`，只是语义上不叫切片（用户 2026-11-09 第 16 条：`strs.0` 之后没有提示）。
fn sliceOrArrayRecv(ty0: []const u8) ?SliceTy {
    if (parseSliceTy(ty0)) |s| return s;
    var t = trimSl(ty0);
    var g: usize = 0;
    while (t.len > 0 and (t[0] == '?' or t[0] == '!') and g < 4) : (g += 1)
        t = trimSl(t[1..]);
    while (t.len > 0 and t[t.len - 1] == '?') t = trimSl(t[0 .. t.len - 1]);
    if (std.mem.eql(u8, t, "str")) return .{ .elem = "u8", .is_const = true };
    if (t.len > 0 and t[0] == '*') t = trimSl(t[1..]);
    if (!std.mem.startsWith(u8, t, "[")) return null;
    const rb = std.mem.indexOfScalar(u8, t, ']') orelse return null;
    var et = trimSl(t[rb + 1 ..]);
    if (et.len == 0) return null;
    var is_const = false;
    if (std.mem.startsWith(u8, et, "const ")) {
        is_const = true;
        et = trimSl(et[6..]);
    }
    return .{ .elem = et, .is_const = is_const };
}

/// 推断出的类型串 → 容器名（去 `?`/`!`、去泛型实参、去模块前缀）。
/// 不是单个标识符形状时返回空串（切片、元组、函数类型都不当容器）。
fn containerNameOf(ty0: []const u8) []const u8 {
    var t = trimSl(ty0);
    var g: usize = 0;
    while (t.len > 0 and (t[0] == '?' or t[0] == '!') and g < 4) : (g += 1)
        t = trimSl(t[1..]);
    while (t.len > 0 and t[t.len - 1] == '?') t = trimSl(t[0 .. t.len - 1]);
    // 指针自动穿透（spec §3.1：`.` 穿透一层指针）：`*Person` 的成员就是
    // `Person` 的成员——`let pp = &p; pp.` 与 `const pa = arrs.0` 都要用。
    var pg: usize = 0;
    while (t.len > 0 and t[0] == '*' and pg < 4) : (pg += 1) t = trimSl(t[1..]);
    if (std.mem.indexOfScalar(u8, t, '(')) |lp| t = trimSl(t[0..lp]);
    if (std.mem.lastIndexOfScalar(u8, t, '.')) |d| t = t[d + 1 ..];
    if (t.len == 0 or t.len > 64) return "";
    for (t) |c| {
        if (!isIdentChar(c)) return "";
    }
    return t;
}

// ---------------------------------------------------------------- 内建类型
//
// KEEP —— 编译器内建类型在源文件里没有声明，std/ 目录里也查不到 struct 体，
// 点补全/推断只能按编译器定义硬编码。三个都是：
//   Allocator  sema_allocator_type()    合成
//   Type       sema_type_type()         `@type(x)` 的描述符
//   Field      sema_type_field_type()   Type.fields 的元素
// 用户 2026-11-10：`var arena: ?Allocator` 解包后 `arena.?.` 一个成员都不出。
// 用户 2026-11-11：`const info = @type(T); info.` 空列表——`@type` 的返回值
// 没有声明文本，全靠这张表。改 src/sema.c 的字段要同步改这里。

const BuiltinFieldDef = struct { name: []const u8, ty: []const u8 };
const BuiltinTyDef = struct { name: []const u8, fields: []const BuiltinFieldDef };

const builtin_ty_table = [_]BuiltinTyDef{
    // 顺序同 sema_allocator_type：ctx / alloc / free
    .{ .name = "Allocator", .fields = &.{
        .{ .name = "ctx", .ty = "?*u8" },
        .{ .name = "alloc", .ty = "fn (?*u8, usize) ?*u8" },
        .{ .name = "free", .ty = "fn (?*u8, *u8, usize) void" },
    } },
    // Type  = { name, size, align, ty, fieldCount, fields: []Field, statics: []Field }
    .{ .name = "Type", .fields = &.{
        .{ .name = "name", .ty = "[]const u8" },
        .{ .name = "size", .ty = "usize" },
        .{ .name = "align", .ty = "usize" },
        .{ .name = "ty", .ty = "type" },
        .{ .name = "fieldCount", .ty = "usize" },
        .{ .name = "fields", .ty = "[]Field" },
        .{ .name = "statics", .ty = "[]Field" },
    } },
    // Field = { name, ty, offset, ptr }（字段叫 ty，type 是关键字）。
    // ptr 是字段值指针（§118）：@type(左值) 的描述符才有，`f.ptr.0` 取值 /
    // 写值。具体类型 *(f.ty) 只在编译器里知道，ty 留空不给误导提示。
    .{ .name = "Field", .fields = &.{
        .{ .name = "name", .ty = "[]const u8" },
        .{ .name = "ty", .ty = "type" },
        .{ .name = "offset", .ty = "usize" },
        .{ .name = "ptr", .ty = "字段值指针：f.ptr.0 取值，@type(左值) 才有" },
    } },
};

fn builtinTyDefOf(ty0: []const u8) ?BuiltinTyDef {
    const cn = containerNameOf(ty0);
    if (cn.len == 0) return null;
    for (builtin_ty_table) |b| {
        if (std.mem.eql(u8, b.name, cn)) return b;
    }
    return null;
}

fn isBuiltinTy(ty0: []const u8) bool {
    return builtinTyDefOf(ty0) != null;
}

/// 内建类型的字段类型：`Type` + `fields` → "[]Field"。推断
/// `const n = info.fieldCount` / `info.fields[0].offset` 用它。
fn builtinFieldTy(ty0: []const u8, member: []const u8) ?[]const u8 {
    const b = builtinTyDefOf(ty0) orelse return null;
    // KEEP — §118：f.ptr 的具体类型 *(f.ty) 只有编译器知道（逐字段不同），
    // 推断返回 null（不出 inlay/hover 类型）；表里的文案只给补全 detail。
    if (std.mem.eql(u8, b.name, "Field") and std.mem.eql(u8, member, "ptr"))
        return null;
    for (b.fields) |f| {
        if (std.mem.eql(u8, f.name, member)) return f.ty;
    }
    return null;
}

fn builtinTyMembers(ty0: []const u8, out: *std.ArrayList(Decl)) bool {
    const b = builtinTyDefOf(ty0) orelse return false;
    for (b.fields) |f| {
        out.append(gpa, .{ .name = f.name, .kind = .field, .ty = f.ty }) catch {};
    }
    return true;
}

/// 切片 / 定长数组接收者的成员：`.len` 与 `.ptr`。源文件里没有声明文本，
/// 只能按语义规则直接出（sema 的 TY_SLICE 分支也只认这两个）。
fn addSliceMembers(list: *Json, seen: *std.StringHashMapUnmanaged(void),
                   prefix: []const u8, si: SliceTy) void {
    if (prefix.len == 0 or std.mem.startsWith(u8, "len", prefix))
        addItem(list, seen, "len", 6, "usize", 0);
    if (prefix.len == 0 or std.mem.startsWith(u8, "ptr", prefix)) {
        var pb: std.ArrayList(u8) = .empty;
        defer pb.deinit(gpa);
        pb.appendSlice(gpa, "ptr: *") catch {};
        if (si.is_const) pb.appendSlice(gpa, "const ") catch {};
        pb.appendSlice(gpa, si.elem) catch {};
        addItem(list, seen, "ptr", 6, pb.items, 0);
    }
}

/// 接收者表达式 → 切片类型。覆盖 `"字面量"`、`var b: []T`、`let b = 切片`、
/// `f(…)` 返回 `?[]T`（如 std.file.read）。识别不了返回 null。
/// KEEP — 用户 2026-11-09：`"hello".` 与切片变量的 `.` 一个成员都不出。
/// sema 的 TY_SLICE 分支只有 .ptr / .len / .0，源文件里没有声明文本可查。
fn sliceRecvTy(text: []const u8, doc: *const Document, expr0: []const u8, depth: u8) ?SliceTy {
    if (depth > 4) return null;
    const e = trimSl(expr0);
    if (e.len == 0) return null;
    if (e[0] == '"') return .{ .elem = "u8", .is_const = true };
    if (std.mem.indexOfScalar(u8, e, '(')) |lp| {
        if (e[e.len - 1] == ')') {
            var ps = lp;
            while (ps > 0 and (isIdentChar(e[ps - 1]) or e[ps - 1] == '.')) ps -= 1;
            const callee = trimSl(e[ps..lp]);
            if (callee.len > 0) {
                if (resolveCalleeRet(text, doc, callee)) |rt| {
                    if (parseSliceTy(rt)) |s| return s;
                }
            }
        }
        return null;
    }
    for (e, 0..) |c, i| {
        if (!isIdentChar(c) or (i == 0 and c >= '0' and c <= '9')) return null;
    }
    const st = findDeclStmt(text, e) orelse return null;
    if (declTyOf(st)) |dt| {
        if (parseSliceTy(dt)) |s| return s;
    }
    if (declInitOf(st)) |ini| {
        if (!std.mem.eql(u8, ini, e)) return sliceRecvTy(text, doc, ini, depth + 1);
    }
    return null;
}

/// 光标落在字符串字面量内时返回整条字面量（含引号）。字符串不跨行，
/// 从行首扫到光标即可。
fn strLitSpanAt(text: []const u8, off: usize) ?[]const u8 {
    var ls = off;
    while (ls > 0 and text[ls - 1] != '\n') ls -= 1;
    var i = ls;
    var start: ?usize = null;
    while (i < off) {
        if (text[i] == '\\') { i += 2; continue; }
        if (text[i] == '"') start = if (start == null) i else null;
        i += 1;
    }
    const s = start orelse return null;
    var j = s + 1;
    while (j < text.len) {
        if (text[j] == '\\') { j += 2; continue; }
        if (text[j] == '"') return text[s .. j + 1];
        j += 1;
    }
    return null;
}

/// 调用目标 → 返回类型。callee 可为 `Add`（本文件）、`other.Add`（模块成员）、
/// `std.print`（编译器内建）。
fn resolveCalleeRet(text: []const u8, doc: *const Document, callee0: []const u8) ?[]const u8 {
    const callee = trimSl(callee0);
    // std 内建（编译器 seed，不在 std.zx 文本里）。format 的 []u8 统一按
    // 展示用的 `str` 出，和字符串字面量的推断保持一致。
    if (std.mem.eql(u8, callee, "std.print") or std.mem.eql(u8, callee, "std.println")) return "void";
    if (std.mem.eql(u8, callee, "std.format")) return "str";

    if (std.mem.lastIndexOfScalar(u8, callee, '.')) |dot| {
        const bind = callee[0..dot];
        const member = callee[dot + 1 ..];
        if (member.len == 0) return null;
        var sf = scanFiles(doc, text);
        defer sf.deinit(gpa);
        // 逐段下钻：模块函数 / 容器方法 / 泛型实例化段（第 14 条）
        if (resolveMemberPath(sf.items, doc, text, callee, 0)) |hit| {
            const sig = sigOfStmt(hit.stmt);
            if (sig.is_fn and sig.ret.len > 0) return selfToOwner(sig.ret, callee);
        }
        // 实例方法（`r.next()` / `g.await(…)`）：接收者是变量
        if (instanceContainerTy(sf.items, text, bind)) |ty| {
            if (findContainerBody(sf.items, ty)) |ch| {
                if (findDeclStmt(ch.body, member)) |stmt| {
                    const sig = sigOfStmt(stmt);
                    if (sig.is_fn and sig.ret.len > 0) return selfToOwner(sig.ret, callee);
                }
            }
        }
        // std 编译期内建（std.process.args → [][]const u8）
        if (builtinDeclStmt(callee)) |st| {
            const sig = sigOfStmt(st);
            if (sig.is_fn and sig.ret.len > 0) return sig.ret;
        }
        return null;
    }
    if (findDeclStmt(text, callee)) |stmt| {
        const sig = sigOfStmt(stmt);
        if (sig.is_fn and sig.ret.len > 0) return sig.ret;
    }
    return null;
}

/// 点补全的接收者起点：从 `.` 往前取**整条接收者表达式**，含调用括号。
/// 旧实现只认 `标识符` 与 `.`，遇到 `)` 就停——`std.map.HashMap(i32).`、
/// `std.time.fromUnixMs(0).`、`f().` 这类链式接收者路径直接变成空串，掉进
/// "未知容器 → 空列表"（用户 2026-11-09 第 8 条：连写的三段没提示）。
/// 现在 `)` `]` 配平回开括号后继续往前取被调名，返回表达式起点。
fn recvExprStart(text: []const u8, at: usize) usize {
    var s = at;
    var guard: usize = 0;
    while (s > 0 and guard < 4096) {
        guard += 1;
        const ch = text[s - 1];
        if (isIdentChar(ch) or ch == '.') {
            s -= 1;
            continue;
        }
        // `x.?` 可选解包：`?` 不是标识符字符，但它是接收者的一部分
        //（`arena.?.` 的 path 必须是 `arena.?`，否则推断不出内层类型）。
        if (ch == '?' and s >= 2 and text[s - 2] == '.') {
            s -= 2;
            continue;
        }
        if (ch != ')' and ch != ']') break;
        // 配平回对应的开括号：跳过去继续取 `(` 前面的被调名/限定链
        var depth: usize = 0;
        var k = s - 1;
        var matched: ?usize = null;
        while (k > 0) {
            k -= 1;
            const c2 = text[k];
            if (c2 == ')' or c2 == ']') {
                depth += 1;
            } else if (c2 == '(' or c2 == '[') {
                if (depth == 0) {
                    matched = k;
                    break;
                }
                depth -= 1;
            }
        }
        s = matched orelse break;
    }
    return s;
}

/// 成员路径（a、other.Add、std.heap）全文扫描
fn identPathBefore(text: []const u8, off: usize) []const u8 {
    var s = off;
    while (s > 0 and (isIdentChar(text[s - 1]) or text[s - 1] == '.')) s -= 1;
    // 向右只吞标识符，不吞下一个点：光标停在 `std.Task` 中间/末尾时路径就是
    // `std.Task`，不会把后面的 `.single` 也串进来（那会把类型 hover 成方法签名）。
    var e = off;
    while (e < text.len and isIdentChar(text[e])) e += 1;
    return text[s..e];
}

// ---- 实例变量点补全：变量 → 容器类型 ---------------------------------------

/// base 是否是某个文件里的 struct/union/enum 容器（containerMembers 的存在性探针）
fn isContainerIn(files: []const []const u8, base: []const u8) bool {
    var tmp: std.ArrayList(Decl) = .empty;
    defer tmp.deinit(gpa);
    return containerMembers(files, base, &tmp);
}

/// KEEP — 表达式 → 容器类型名（唯一的类型解析入口，递归）。用户 2026-11-09
/// 第 7 条："递归地去找这些函数、类型实现，不能一个一个地找"。覆盖：
///   `X{…}` 字面量 / `X.m(…)` 静态调用 / 泛型实例 `X(T)`
///   `a.b.C` / `std.mod.Type`（任意深度：末段既可能是容器，也可能是函数）
///   `V.init(…)`，其中 `var V: type = std.ArrayList(i32)`（类型变量绑定）
///   `let x = y` / `let x = Obj.init(…)` / `let x = f(…)`（别名链递归）
/// 包装（`?` `!` `&` `*` `[]T` `[n]T`）先剥掉；方法的 `Self` 返回映射回
/// 接收者容器。识别不了返回 null。
fn inferContainer(files: []const []const u8, text: []const u8, expr: []const u8, depth: u8) ?[]const u8 {
    if (depth > 6) return null;
    var e = trimSl(expr);
    // `x.?` 解包不改变容器身份：`arena.?.` 的接收者是 arena 的内层值
    if (e.len > 2 and std.mem.endsWith(u8, e, ".?")) e = trimSl(e[0 .. e.len - 2]);
    // 剥包装：?T / !T / &x / *T / *const T / []T / [n]T
    var guard: usize = 0;
    while (guard < 8) : (guard += 1) {
        if (e.len == 0) return null;
        if (e[0] == '?' or e[0] == '!' or e[0] == '&' or e[0] == '*') {
            e = trimSl(e[1..]);
            if (std.mem.startsWith(u8, e, "const ")) e = trimSl(e[6..]);
            continue;
        }
        if (std.mem.startsWith(u8, e, "[]")) {
            e = trimSl(e[2..]);
            continue;
        }
        if (e[0] == '[') {
            const rb = std.mem.indexOfScalar(u8, e, ']') orelse break;
            e = trimSl(e[rb + 1 ..]);
            continue;
        }
        break;
    }
    if (e.len == 0) return null;
    if (!isIdentChar(e[0]) or (e[0] >= '0' and e[0] <= '9')) return null;
    var j: usize = 0;
    while (j < e.len and isIdentChar(e[j])) j += 1;
    if (j == 0) return null;
    const head = e[0..j];
    var k = j;
    while (k < e.len and (e[k] == ' ' or e[k] == '\t')) k += 1;
    if (k >= e.len) {
        // 裸名字：扫描集里的容器 → 自己；否则按别名/类型变量追初始化
        // （`var V: type = std.ArrayList(i32)` 的 V 在这里接住）
        if (isContainerIn(files, head) or genericBodyOf(files, head) != null) return head;
        // 内建类型（Allocator）：没有声明文本，但成员表在
        if (isBuiltinTy(head)) return containerNameOf(head);
        if (findDeclStmt(text, head)) |s2| {
            if (declInitOf(s2)) |init2| return inferContainer(files, text, init2, depth + 1);
        }
        return null;
    }
    if (e[k] == '{') {
        // X{…} 字面量（`std.task.Task{ … }` 走下面的路径分支）
        return if (isContainerIn(files, head) or genericBodyOf(files, head) != null) head else null;
    }
    if (e[k] == '(') {
        // 裸调用 f(…)：同文件函数的返回类型
        if (findDeclStmt(text, head)) |s2| {
            const sig = sigOfStmt(s2);
            if (sig.is_fn and sig.ret.len > 0) return inferContainer(files, text, sig.ret, depth + 1);
        }
        return null;
    }
    if (e[k] != '.') return null;
    // 点路径：逐段收集，末段决定结果
    var segs: [10][]const u8 = undefined;
    segs[0] = head;
    var nsegs: usize = 1;
    var tail_end: usize = j;
    var last_dot: usize = k; // 末段前的那个 '.'（用来切接收者表达式）
    var q = k;
    while (q < e.len and e[q] == '.') {
        var m = q + 1;
        while (m < e.len and (e[m] == ' ' or e[m] == '\t')) m += 1;
        var n = m;
        while (n < e.len and isIdentChar(e[n])) n += 1;
        if (n == m or nsegs >= segs.len) break;
        last_dot = q;
        segs[nsegs] = e[m..n];
        nsegs += 1;
        tail_end = n;
        q = n;
    }
    if (nsegs == 1) return null;
    const last = segs[nsegs - 1];
    if (tail_end >= e.len or e[tail_end] != '(') {
        // `std.mod.Type` / `Type` 裸引用：末段是容器就是它
        if (isContainerIn(files, last) or genericBodyOf(files, last) != null) return last;
        return null;
    }
    // 末段后跟 '('：先按方法调用（接收者 = 去掉末段的路径），再按泛型
    // 实例化（末段是容器），最后按模块函数（std.time.fromUnixMs → DateTime）。
    if (inferContainer(files, text, e[0..last_dot], depth + 1)) |recv| {
        if (methodRetIn(files, recv, last)) |ret| {
            const rt = trimSl(ret);
            if (std.mem.eql(u8, rt, "Self")) return recv;
            // 内建类型先判：`init(...) ?Allocator` 的 Allocator 不在源文件里，
            // 落到下面"方法没写返回类型就按接收者算"会错成 ArenaAllocator，
            // 于是 `arena.` 弹 init/reset/deinit（全编译不过）。
            if (isBuiltinTy(rt)) return containerNameOf(rt);
            if (inferContainer(files, text, rt, depth + 1)) |r2| return r2;
        }
        // 方法没写返回类型 / 解析不出：按接收者容器算（保底）
        if (isContainerIn(files, recv) or genericBodyOf(files, recv) != null) return recv;
    }
    if (isContainerIn(files, last) or genericBodyOf(files, last) != null) return last;
    if (moduleFnContainerRet(files, last)) |rt| return rt;
    return null;
}

/// 模块函数返回的容器类型名：`std.time.fromUnixMs(...)` → DateTime、
/// `std.fs.stat(...)` → Stat。返回类型开头的 `?`/`!` 去掉后再判定，且要求
/// 该容器就定义在函数所在的同一个模块文件里（避免同名函数跨模块误命中）。
fn moduleFnContainerRet(files: []const []const u8, fn_name: []const u8) ?[]const u8 {
    for (files) |ftext| {
        const st = findDeclStmt(ftext, fn_name) orelse continue;
        const sig = sigOfStmt(st);
        if (!sig.is_fn or sig.ret.len == 0) continue;
        var rt = trimSl(sig.ret);
        while (rt.len > 0 and (rt[0] == '?' or rt[0] == '!')) rt = trimSl(rt[1..]);
        if (rt.len == 0 or !isIdentChar(rt[0])) continue;
        if (isContainerIn(&[_][]const u8{ftext}, rt) or
            genericBodyOf(&[_][]const u8{ftext}, rt) != null) return rt;
    }
    return null;
}

/// 实例名 → 容器类型名：找 `const/let/var NAME =` 的初始化交给 inferContainer。
fn instanceContainerName(text: []const u8, name: []const u8) ?[]const u8 {
    const s = findDeclStmt(text, name) orelse return null;
    const init = declInitOf(s) orelse return null;
    return inferContainer(&.{text}, text, init, 0);
}

fn hoverAt(doc: *const Document, line: u32, character: u32) ?[]const u8 {
    const text = doc.text;
    const off = posToOffset(text, line, character);

    // module("…") 字符串内
    if (moduleStringAt(text, off)) |m| {
        var b: std.ArrayList(u8) = .empty;
        b.print(gpa, "```zx\nmodule `{s}`\n```", .{m}) catch return null;
        return hoverPayload(b.items);
    }

    // 字符串字面量本身（用户 2026-11-09：`.ptr` / `.len` 没提示）
    if (strLitSpanAt(text, off)) |lit| {
        // 字节数 = 字面量长度 - 2 个引号（转义序列在这里不精确，够用——
        // 精确的 N 由编译器按解码后的字节数定）
        const nbytes = if (lit.len >= 2) lit.len - 2 else 0;
        var b: std.ArrayList(u8) = .empty;
        b.print(gpa, "```zx\n" ++
                     "\"…\": *[{d}]const u8\n```\n" ++
                     "- 定长数组指针（用户 2026-11-09：字符串等于 zx 的数组 `*[N]const u8`），N 是字节数、不含结尾 0\n" ++
                     "- `.len: usize`（编译期常量）、`.ptr: *const u8`\n" ++
                     "- 传给 `[]const u8` 形参自动降级为切片；给 C 的 `*const u8` 用 `.ptr`", .{nbytes}) catch return null;
        return hoverPayload(b.items);
    }

    const path = trimSl(identPathBefore(text, off));
    if (path.len == 0) return null;

    // KEEP — std 内建（seeded 名 / 命名空间成员）：实现在编译器 C 侧，
    // 源文件里没有声明文本，hover 从对齐表出签名（用户 2026-11-09 第 2 条）。
    if (std.mem.startsWith(u8, path, "std.")) {
        if (stdBuiltinDoc(path)) |bd| {
            var b: std.ArrayList(u8) = .empty;
            b.print(gpa, "```zx\n{s}\n```", .{bd.sig}) catch return null;
            return hoverPayload(b.items);
        }
    }

    // KEEP — 链式接收者的成员 hover（用户 2026-11-09 第 8 条：连写三段没提示）。
    // `std.map.HashMap(i32).init`、`std.time.fromUnixMs(0).year` 的成员名前是
    // ')', identPathBefore 在那里截止，path 只剩成员名，所有名字路径落空。
    // 这里把 `.` 之前的整条接收者表达式取出来交给 inferContainer 定容器，
    // 再在容器体里找成员：方法出签名、字段出 `name: Type`。
    if (path.len > 0) {
        // 成员名（光标所在标识符整体，可能只打了前几个字母）
        var mstart = off;
        while (mstart > 0 and isIdentChar(text[mstart - 1])) mstart -= 1;
        var mend = off;
        while (mend < text.len and isIdentChar(text[mend])) mend += 1;
        const mname = text[mstart..mend];
        if (mstart > 0 and text[mstart - 1] == '.' and mname.len > 0) {
            const rstart = recvExprStart(text, mstart - 1);
            const recv = trimSl(text[rstart .. mstart - 1]);
            if (recv.len > 0 and std.mem.indexOfScalar(u8, recv, '(') != null) {
                var sfr = scanFiles(doc, text);
                defer sfr.deinit(gpa);
                if (inferContainer(sfr.items, text, recv, 0)) |rty| {
                    const rbody: ?[]const u8 = if (findContainerBody(sfr.items, rty)) |ch|
                        ch.body
                    else
                        genericBodyOf(sfr.items, rty);
                    if (rbody) |rb| {
                        if (hoverMarkdownForDecl(rb, mname, doc)) |md| return hoverPayload(md);
                        if (fieldTypeIn(rb, mname)) |t| {
                            var b: std.ArrayList(u8) = .empty;
                            b.print(gpa, "```zx\n{s}: {s}\n```", .{ mname, t }) catch return null;
                            return hoverPayload(b.items);
                        }
                    }
                }
            }
        }
    }

    // KEEP — 切片/字符串成员 hover：`"hello".len` / `b.ptr`。接收者以引号
    // 收尾时 recvExprStart 停在点上，往回找配对引号取整条字面量。
    {
        var ms = off;
        while (ms > 0 and isIdentChar(text[ms - 1])) ms -= 1;
        var me = off;
        while (me < text.len and isIdentChar(text[me])) me += 1;
        const mn = text[ms..me];
        if (ms > 0 and text[ms - 1] == '.' and mn.len > 0) {
            var rs = recvExprStart(text, ms - 1);
            if (rs == ms - 1 and ms - 1 > 0 and text[ms - 2] == '"') {
                var q = ms - 2;
                var okq = false;
                while (q > 0) {
                    q -= 1;
                    if (text[q] == '\\') {
                        if (q > 0) q -= 1;
                        continue;
                    }
                    if (text[q] == '"') { okq = true; break; }
                }
                if (okq) rs = q;
            }
            const recv = trimSl(text[rs .. ms - 1]);
            if (recv.len > 0) {
                if (sliceRecvTy(text, doc, recv, 0)) |si| {
                    if (std.mem.eql(u8, mn, "len")) {
                        var b: std.ArrayList(u8) = .empty;
                        b.print(gpa, "```zx\nlen: usize\n```", .{}) catch return null;
                        return hoverPayload(b.items);
                    }
                    if (std.mem.eql(u8, mn, "ptr")) {
                        var ty: std.ArrayList(u8) = .empty;
                        defer ty.deinit(gpa);
                        ty.appendSlice(gpa, "*") catch {};
                        if (si.is_const) ty.appendSlice(gpa, "const ") catch {};
                        ty.appendSlice(gpa, si.elem) catch {};
                        var b: std.ArrayList(u8) = .empty;
                        b.print(gpa, "```zx\nptr: {s}\n```", .{ty.items}) catch return null;
                        return hoverPayload(b.items);
                    }
                }
            }
        }
    }

    // 模块绑定名本身
    var mb: ?[]const u8 = moduleOfBinding(text, path);
    if (mb == null and std.mem.eql(u8, path, "std")) mb = "std";
    if (mb) |mm| {
        var b: std.ArrayList(u8) = .empty;
        b.print(gpa, "```zx\nmodule `{s}`\n```", .{mm}) catch return null;
        return hoverPayload(b.items);
    }

    // <module>.<member>
    if (std.mem.lastIndexOfScalar(u8, path, '.')) |dot| {
        const bind = path[0..dot];
        const member = path[dot + 1 ..];
        if (member.len > 0) {
            var mm: ?[]const u8 = moduleOfBinding(text, bind);
            if (mm == null and std.mem.eql(u8, bind, "std")) mm = "std";
            if (mm) |mod| {
                if (resolveModuleFile(doc, mod)) |p| {
                    if (readFileSmall(p)) |t| {
                        // 类型别名（`pub const Task = task.Task;`）：往目标容器
                        // 追一层，贴真实 struct（字段+方法）；追不到才报别名行。
                        if (findDeclStmt(t, member)) |st0| {
                            const ini0 = trimSl(declInitOf(st0) orelse "");
                            if (isTypePathExpr(ini0)) {
                                var sf0 = scanFiles(doc, text);
                                defer sf0.deinit(gpa);
                                if (aliasContainerHover(sf0.items, doc, member, t)) |md2|
                                    return hoverPayload(md2);
                            }
                        }
                        if (hoverMarkdownForDecl(t, member, doc)) |md|
                            return hoverPayload(md);
                    }
                }
            }
        }
        // 实例方法（g.spawn → Group 体里的 spawn）与容器限定成员
        //（std.Task.coro / task.Obj.make / Obj.make）
        if (member.len > 0) {
            var sf = scanFiles(doc, text);
            defer sf.deinit(gpa);
            // 绑定是限定类型路径（`let x = A.B.Cfg{…}` / `var z: A.B.C`）：
            // 逐段解析拿**那一层**的容器体，和补全用同一套判定——按末段
            // 名字找会撞上外层同名容器，字段 hover 直接空掉。
            var ipc = instancePathContainer(sf.items, doc, text, bind);
            // 接收者本身是多段表达式（`t.p1.P2T` 的 bind 是 `t.p1`）：
            // 绑定名查不到，走逐段路径解析（实例起点 / 类型值都会跟）。
            if (ipc == null and std.mem.indexOfScalar(u8, bind, '.') != null)
                ipc = resolveContainerPath(sf.items, doc, text, bind);
            const ity: ?[]const u8 = if (ipc) |pc| pc.name else instanceContainerTy(sf.items, text, bind);
            if (ity) |ty| {
                // 泛型容器（std.ArrayList(i32) 实例）：findContainerBody 只认
                // `const X = struct`，泛型 fn 里的 `return struct {…}` 落空，
                // genericBodyOf 兜住（用户 2026-11-09 第 3 条）。
                const body: ?[]const u8 = if (ipc) |pc| pc.body else if (findContainerBody(sf.items, ty)) |ch|
                    ch.body
                else
                    genericBodyOf(sf.items, ty);
                if (body) |b| {
                    if (hoverMarkdownForDecl(b, member, doc)) |md|
                        return hoverPayload(md);
                    // 字段成员（p.age 的 age）：出 `age: i32`
                    if (fieldTypeIn(b, member)) |t| {
                        var b2: std.ArrayList(u8) = .empty;
                        b2.print(gpa, "```zx\n{s}: {s}\n```", .{ member, t }) catch return null;
                        return hoverPayload(b2.items);
                    }
                }
            }
            // 方法体内的 self.age / self.count：self 没有声明语句，
            // 容器取声明了 `self:` 形参的容器体
            if (std.mem.eql(u8, bind, "self")) {
                if (selfContainerBody(text)) |body| {
                    if (hoverMarkdownForDecl(body, member, doc)) |md|
                        return hoverPayload(md);
                    if (fieldTypeIn(body, member)) |t| {
                        var b: std.ArrayList(u8) = .empty;
                        b.print(gpa, "```zx\n{s}: {s}\n```", .{ member, t }) catch return null;
                        return hoverPayload(b.items);
                    }
                }
            }
            if (resolvePathStmt(sf.items, doc, text, path)) |stmt| {
                const sig = sigOfStmt(stmt);
                if (sig.is_fn) {
                    // 泛型容器（std.map.HashMap / std.ArrayList）：贴真实
                    // struct 体，不是一行签名（用户 2026-11-09）。
                    if (genericContainerHover(member, stmt)) |md|
                        return hoverPayload(md);
                    var b: std.ArrayList(u8) = .empty;
                    b.appendSlice(gpa, "```zx\n") catch return null;
                    b.appendSlice(gpa, member) catch {};
                    appendSigTail(&b, sig);
                    b.appendSlice(gpa, "\n```") catch {};
                    return hoverPayload(b.items);
                }
                // 三段点路径落在容器上（`std.task.Task`）：贴真实定义。
                if (containerHoverFromStmt(member, stmt)) |md|
                    return hoverPayload(md);
            }
            // KEEP - 匿名类型线：`anon.pos` / `info.a` / `nv.pos.x` 的成员
            // hover。命名容器路径全部落空才到这里；3z 守门。
            if (anonTyOfPath(text, doc, bind, off)) |aty| {
                if (parseAnonTy(aty)) |flds| {
                    for (flds.items) |f| {
                        if (std.mem.eql(u8, f.name, member)) {
                            var b: std.ArrayList(u8) = .empty;
                            b.print(gpa, "```zx\n{s}: {s}\n```", .{ member, f.ty }) catch return null;
                            return hoverPayload(b.items);
                        }
                    }
                }
            }
            // KEEP — 模块容器实例嵌套（用户 2026-11-08 第 3 条）：
            // `v.inner.a` 的 a 类型 = 模块 Outer 里 inner 的内联 struct 字段。
            {
                var mitxt: ?[]const u8 = null;
                if (modInstanceTyOfPath(doc, text, path, &mitxt)) |mty| {
                    var b: std.ArrayList(u8) = .empty;
                    b.print(gpa, "```zx\n{s}: {s}\n```", .{ member, mty }) catch return null;
                    return hoverPayload(b.items);
                }
            }
        }
        return null;
    }

    // 当前文件声明
    if (hoverMarkdownForDecl(text, path, doc)) |md| return hoverPayload(md);
    // 函数形参 / struct 字段 / 方法 self：`name: Type` 形状
    //（形参和字段不是 const 声明，findDeclStmt 落空，这里兜住）
    if (namedTypeNear(text, path, off)) |t| {
        var b: std.ArrayList(u8) = .empty;
        b.print(gpa, "```zx\n{s}: {s}\n```", .{ path, t }) catch return null;
        return hoverPayload(b.items);
    }
    return null;
}

fn hoverPayload(md: []const u8) []const u8 {
    var j = Json{};
    j.buf.appendSlice(gpa, "{\"contents\":{\"kind\":\"markdown\",\"value\":") catch return "";
    j.str(md);
    j.buf.appendSlice(gpa, "}}") catch return "";
    return j.buf.items;
}

/// 声明的真实关键字（hover 文案照抄源码，不再一律写 const）。
fn declKwOf(stmt: []const u8) []const u8 {
    var s = trimSl(stmt);
    if (std.mem.startsWith(u8, s, "pub ")) {
        s = trimSl(s[4..]);
        if (std.mem.startsWith(u8, s, "const")) return "pub const";
        if (std.mem.startsWith(u8, s, "let")) return "pub let";
        if (std.mem.startsWith(u8, s, "var")) return "pub var";
        return "pub const";
    }
    if (std.mem.startsWith(u8, s, "const")) return "const";
    if (std.mem.startsWith(u8, s, "let")) return "let";
    if (std.mem.startsWith(u8, s, "var")) return "var";
    return "const";
}

/// 名字 → markdown 悬停文本（```zx 代码块）。
/// 声明是容器（struct/union/enum）时贴完整定义；否则 null。
/// 供「类型别名追一层」和「三段点路径」两条 hover 路径共用：用户 2026-11-09
/// 第 3 条要的是类型实体的真实形状（字段 + 方法），不是一行别名。
fn containerHoverFromStmt(name: []const u8, stmt: []const u8) ?[]const u8 {
    const it0 = trimSl(declInitOf(stmt) orelse return null);
    if (!(std.mem.startsWith(u8, it0, "struct") or
        std.mem.startsWith(u8, it0, "union") or
        std.mem.startsWith(u8, it0, "enum"))) return null;
    var b: std.ArrayList(u8) = .empty;
    b.print(gpa, "```zx\npub const {s}: type = ", .{name}) catch return null;
    b.appendSlice(gpa, if (it0.len > 600) it0[0..600] else it0) catch {};
    if (it0.len > 600) b.appendSlice(gpa, "...") catch {};
    b.appendSlice(gpa, ";\n```") catch {};
    return b.items;
}

/// 泛型容器的定义体：`pub const X = fn (T: type) type { return struct {…} }`
/// 从 struct/union 关键字起取（matchBrace 校验配对）。不是泛型容器返回
/// null。用户 2026-11-09：泛型类型（std.ArrayList / std.map.HashMap）hover
/// 只有一行签名 `X(T: type) type`，看不到真实形状——与非泛型容器对齐。
fn genericContainerDef(stmt: []const u8) ?[]const u8 {
    const it0 = trimSl(declInitOf(stmt) orelse return null);
    if (!std.mem.startsWith(u8, it0, "fn")) return null;
    var i: usize = 0;
    while (i < it0.len) : (i += 1) {
        const is_struct = it0[i] == 's' and std.mem.startsWith(u8, it0[i..], "struct");
        const is_union = it0[i] == 'u' and std.mem.startsWith(u8, it0[i..], "union");
        if (!is_struct and !is_union) continue;
        if (i > 0 and isIdentChar(it0[i - 1])) continue;
        var p = i;
        while (p < it0.len and it0[p] != '{') p += 1;
        if (p >= it0.len or matchBrace(it0, p) == null) return null;
        return it0[i..];
    }
    return null;
}

/// 泛型容器定义的 hover 文案（与 containerHoverFromStmt 同款格式）。
fn genericContainerHover(name: []const u8, stmt: []const u8) ?[]const u8 {
    const gdef = genericContainerDef(stmt) orelse return null;
    var b: std.ArrayList(u8) = .empty;
    b.print(gpa, "```zx\npub const {s}: type = ", .{name}) catch return null;
    b.appendSlice(gpa, if (gdef.len > 600) gdef[0..600] else gdef) catch {};
    if (gdef.len > 600) b.appendSlice(gpa, "...") catch {};
    b.appendSlice(gpa, ";\n```") catch {};
    return b.items;
}

/// `task.Task` / `std.task.Task` 形状（首段纯标识符，点之前没有空格/括号）。
/// `fn (…)`、`module("x")`、`X{ .a = 1 }` 都不算。
fn isTypePathExpr(e: []const u8) bool {
    if (e.len == 0 or !isIdentChar(e[0]) or (e[0] >= '0' and e[0] <= '9')) return false;
    const dot = std.mem.indexOfScalar(u8, e, '.') orelse return false;
    if (dot == 0) return false;
    for (e[0..dot]) |c| {
        if (!isIdentChar(c)) return false;
    }
    return true;
}

/// `pub const Task = task.Task;` 这种类型别名：把别名目标解析成容器定义。
/// 拿不到返回 null（调用方照旧报别名行）。
fn aliasContainerHover(files: []const []const u8, doc: *const Document,
                       alias_name: []const u8, alias_text: []const u8) ?[]const u8 {
    const st = findDeclStmt(alias_text, alias_name) orelse return null;
    const init = trimSl(declInitOf(st) orelse return null);
    if (init.len == 0 or !isIdentChar(init[0])) return null;
    if (std.mem.indexOfScalar(u8, init, '.') == null) return null;
    const target = resolvePathStmt(files, doc, alias_text, init) orelse return null;
    return containerHoverFromStmt(alias_name, target);
}

fn hoverMarkdownForDecl(text: []const u8, name: []const u8, doc: ?*const Document) ?[]const u8 {
    const stmt = findDeclStmt(text, name) orelse return null;
    const sig = sigOfStmt(stmt);
    var b: std.ArrayList(u8) = .empty;
    b.appendSlice(gpa, "```zx\n") catch return null;
    if (sig.is_fn) {
        // 泛型容器（`const X = fn (T: type) type { return struct {…} }`）：
        // 贴真实 struct 体，而不是一行 `X(T: type) type` 签名（用户
        // 2026-11-09：std 泛型类型 hover 要与非泛型容器对齐，看得到形状）。
        if (genericContainerHover(name, stmt)) |md| return md;
        // fn：显示完整签名
        b.appendSlice(gpa, sig.name) catch {};
        appendSigTail(&b, sig);
    } else {
        // 值声明：推断类型
        const init = declInitOf(stmt) orelse return null;
        const kw = declKwOf(stmt);
        const it0 = trimSl(init);
        // 容器声明：贴完整原始定义（含字段与收尾括号），
        // 旧实现只吐 `const X: type = struct {` 半句，看着像被截断。
        const is_container = std.mem.startsWith(u8, it0, "struct") or
            std.mem.startsWith(u8, it0, "union") or
            std.mem.startsWith(u8, it0, "enum");
        if (is_container) {
            b.print(gpa, "{s} {s}: type = ", .{ kw, name }) catch {};
            b.appendSlice(gpa, if (it0.len > 500) it0[0..500] else it0) catch {};
            if (it0.len > 500) b.appendSlice(gpa, "...") catch {};
            b.appendSlice(gpa, ";\n```") catch {};
            return b.items;
        }
        // 模块别名（`pub const task = module("task.zx")`）：hover 只说这是
        // 哪个模块，别掉进「值 + 初始化表达式」的打印里。
        if (std.mem.startsWith(u8, it0, "module(")) {
            if (std.mem.indexOfScalar(u8, it0, '"')) |a| {
                if (std.mem.indexOfScalarPos(u8, it0, a + 1, '"')) |z| {
                    b.print(gpa, "{s} {s} = module `{s}`\n```", .{ kw, name, it0[a + 1 .. z] }) catch {};
                    return b.items;
                }
            }
        }
        if (doc) |d| {
            if (inferExprType(text, d, init, 0)) |t| {
                b.print(gpa, "{s} {s}: {s} = ", .{ kw, name, t }) catch {};
            } else {
                b.print(gpa, "{s} {s} = ", .{ kw, name }) catch {};
            }
        } else {
            b.print(gpa, "{s} {s} = ", .{ kw, name }) catch {};
        }
        b.appendSlice(gpa, if (init.len > 100) init[0..100] else init) catch {};
    }
    b.appendSlice(gpa, "\n```") catch {};
    return b.items;
}

// ---------------------------------------------------------------- inlay hints

const LineCol = struct { line: u32, character: u32 };

/// 字节偏移 → LSP position（UTF-16 列），与 posToOffset 互逆。
fn offsetToLineCol(text: []const u8, off_in: usize) LineCol {
    const off = @min(off_in, text.len);
    var line: u32 = 0;
    var line_start: usize = 0;
    var i: usize = 0;
    while (i < off) : (i += 1) {
        if (text[i] == '\n') {
            line += 1;
            line_start = i + 1;
        }
    }
    var units: u32 = 0;
    var q = line_start;
    while (q < off) {
        const n = std.unicode.utf8ByteSequenceLength(text[q]) catch 1;
        const end = @min(q + n, text.len);
        const cp = std.unicode.utf8Decode(text[q..end]) catch {
            units += 1;
            q += 1;
            continue;
        };
        units += if (cp < 0x10000) 1 else 2;
        q = end;
    }
    return .{ .line = line, .character = units };
}

/// 行首偏移表：offsetToLineCol 每次都要从文件头数过来，一次 inlay 请求里
/// 几千条提示就是 O(提示数 × 文件长度)——2000 行 / 6000 条提示实测 ~300ms，
/// 时间几乎全花在这个从头扫描上。建一次表 + 二分，整体回到
/// O(文件长度 + 提示数 × log 行数)。表只活在一次请求里，不做跨请求缓存：
/// 文档改一个字符长度可能不变，按 (ptr,len) 认缓存会拿到错的行首表。
const LineIndex = struct {
    starts: []const u32,

    fn build(text: []const u8) LineIndex {
        var list: std.ArrayList(u32) = .empty;
        list.append(gpa, 0) catch @panic("oom");
        for (text, 0..) |c, i| {
            if (c == '\n') list.append(gpa, @intCast(i + 1)) catch @panic("oom");
        }
        return .{ .starts = list.items };
    }

    /// 与 offsetToLineCol 同义：行号 + 该行内的 UTF-16 列。
    fn lineCol(self: LineIndex, text: []const u8, off_in: usize) LineCol {
        const off = @min(off_in, text.len);
        var lo: usize = 0;
        var hi: usize = self.starts.len;
        while (lo + 1 < hi) {
            const mid = lo + (hi - lo) / 2;
            if (self.starts[mid] <= off) lo = mid else hi = mid;
        }
        var units: u32 = 0;
        var q: usize = self.starts[lo];
        while (q < off) {
            const n = std.unicode.utf8ByteSequenceLength(text[q]) catch 1;
            const end = @min(q + n, text.len);
            const cp = std.unicode.utf8Decode(text[q..end]) catch {
                units += 1;
                q += 1;
                continue;
            };
            units += if (cp < 0x10000) 1 else 2;
            q = end;
        }
        return .{ .line = @intCast(lo), .character = units };
    }
};
/// 关键字是否处在语句开头：`[]const u8` / `"const x"` / `// const x` 都不算。
fn declStartsHere(text: []const u8, c: usize) bool {
    var p = c;
    if (p >= 4 and std.mem.eql(u8, text[p - 4 .. p], "pub ")) p -= 4;
    while (p > 0 and (text[p - 1] == ' ' or text[p - 1] == '\t')) p -= 1;
    if (p == 0) return true;
    return switch (text[p - 1]) {
        '\n', '\r', ';', '{', '}' => true,
        else => false,
    };
}

/// 按文档偏移排序；偏移相同按 kind，保证同一位置的输出稳定可复现。
fn hintLess(_: void, a: Hint, b: Hint) bool {
    if (a.off != b.off) return a.off < b.off;
    return a.kind < b.kind;
}

const Hint = struct {
    off: usize,
    line: u32,
    character: u32,
    label: []const u8,
    kind: u32,
    pad_right: bool,
};

/// 调用处参数名 inlay（kind=2）：`push(.{…})` → `push(info: .{…})`。
/// 这些 `(` 不是调用：fn 定义、控制流、分组表达式、cast 前缀。
fn isCallKeyword(w: []const u8) bool {
    const kws = [_][]const u8{ "fn", "if", "else", "while", "for", "switch", "catch", "defer", "return", "try", "and", "or", "not" };
    for (kws) |k| if (std.mem.eql(u8, w, k)) return true;
    return false;
}

fn validHintName(nm: []const u8) bool {
    if (nm.len == 0 or nm.len > 32) return false;
    if (nm[0] == '_' or (nm[0] >= '0' and nm[0] <= '9')) return false;
    for (nm) |c| if (!isIdentChar(c)) return false;
    return true;
}

/// 已经写了 `name: value` 的实参不再插提示。
fn argAlreadyNamed(seg: []const u8) bool {
    const s = trimSl(seg);
    var i: usize = 0;
    while (i < s.len and isIdentChar(s[i])) i += 1;
    return i > 0 and i < s.len and s[i] == ':';
}

/// 被调方签名里的形参名（切片指向声明文本，不复制）。支持本文件函数、
/// 模块函数（std.x / mod.fn）、实例方法与容器限定调用。
/// 形参串 `name: ty` → 名字。
fn paramName(p: []const u8) []const u8 {
    const nm = trimSl(p);
    // 无名形参（`fn (i32, struct{a:u32,b:u32}) i32`）里第一个 ':' 属于类型本身，
    // 不能当名字分隔。判据：'=' 之前那段必须是纯标识符。
    // 旧实现直接按第一个 ':' 切，`struct{a:u32,b:u32}` 变成 `struct{a`，
    // Tab 补出来的骨架就是半截类型（用户 2026-11-11 截图：
    // `P.P3.f3(i32, struct{a)`）。
    const c = std.mem.indexOfScalar(u8, nm, ':') orelse return "";
    const head = trimSl(nm[0..c]);
    if (head.len == 0) return "";
    for (head) |ch| {
        if (!isIdentChar(ch)) return "";
    }
    return head;
}

/// 无名形参的占位文本：没有名字就放**类型**，用户 Tab 过去一眼知道该填什么。
/// 匿名聚合（`struct{a:u32,b:u32}`）写成 `.{ }`——那才是实参真正的样子，
/// 把整段 struct 体塞进占位只会糊一屏。
/// 用户 2026-11-11：上一版为了不截断改成了空占位，结果是"只留了占位，没有文字"。
fn paramPlaceholderText(p: []const u8) []const u8 {
    const nm = trimSl(p);
    const ty = blk: {
        const c = std.mem.indexOfScalar(u8, nm, ':') orelse break :blk nm;
        // 有名字 → ':' 之后是类型；没名字 → 第一个 ':' 在类型体里，整段是类型
        if (paramName(p).len > 0) break :blk trimSl(nm[c + 1 ..]);
        break :blk nm;
    };
    if (std.mem.indexOfScalar(u8, ty, '{') != null) return ".{ }";
    return ty;
}

/// 片段（snippet）里的字面量转义：`\` `$` `}` 都是语法字符，出现在文本里
/// 必须加反斜杠。占位默认值里塞类型串时离不开这一步——`${2:struct{a}}`
/// 的 `}` 会提前关闭占位符，插入结果被截在 `struct{a`。
fn escapeSnippet(sb: *std.ArrayList(u8), txt: []const u8) void {
    for (txt) |ch| {
        if (ch == '\\' or ch == '$' or ch == '}') sb.append(gpa, '\\') catch return;
        sb.append(gpa, ch) catch return;
    }
}

/// 形参串 `name: struct{…}` → 匿名结构体类型串；不是匿名 struct 返回 null。
fn paramStructTy(p: []const u8) ?[]const u8 {
    const c = std.mem.indexOfScalar(u8, p, ':') orelse return null;
    const ty = trimSl(p[c + 1 ..]);
    return structTyOf(ty);
}

/// 一次 inlayHint 请求内的复用缓存。
///
/// 一次请求要走过文件里每个 `(`，同一个被调方（`std.println` 之流）会出现
/// 几十次。每次重扫目录、把全部 .zx 读一遍、再线性找声明，就是
/// O(调用点 × 全工程字节数)——工程里放几个大文件，单次请求能拖到几百毫秒，
/// 编辑器里表现为"行内提示要等一下才出来"。这里把三件事各做一次：
/// 目录扫描（fileList）、按路径读文件（readCached）、被调方签名（calleeStmtCached）。
/// 缓存的切片指向 text 或只为本次请求分配的缓冲，请求结束即作废（进程不回收，
/// 与文件里其余路径一致：gpa 是 smp_allocator，LSP 常驻进程按需增长）。
///
/// paths/callees 用 hash map 而不是数组：缓存查询在主循环里每个调用点一次，
/// 数组线性查是 O(U²)——12000 个不同名函数实测 11.7s（4 倍数据 15 倍时间），
/// 换 hash map 后回到 O(U)。
const InlayCtx = struct {
    doc: *const Document,
    text: []const u8,
    files: ?std.ArrayList([]const u8) = null,
    paths: std.StringHashMapUnmanaged([]const u8) = .empty,
    callees: std.StringHashMapUnmanaged(?[]const u8) = .empty,
    decls: ?DeclIndex = null,

    fn fileList(self: *InlayCtx) []const []const u8 {
        if (self.files == null) self.files = scanFiles(self.doc, self.text);
        return self.files.?.items;
    }

    fn readCached(self: *InlayCtx, path: []const u8) ?[]const u8 {
        if (self.paths.get(path)) |t| return t;
        const t = readFileSmall(path) orelse return null;
        const key = gpa.dupe(u8, path) catch path;
        self.paths.put(gpa, key, t) catch {};
        return t;
    }

    /// 名字相同就是同一个声明：`callee` 是 text 里的切片，命中率很高。
    /// 查不到（null）也要缓存——同一个没被解析出来的名字会出现很多次。
    fn stmtCached(self: *InlayCtx, callee: []const u8) ?[]const u8 {
        if (self.callees.get(callee)) |st| return st;
        const st = calleeStmt(self, callee);
        const key = gpa.dupe(u8, callee) catch callee;
        self.callees.put(gpa, key, st) catch {};
        return st;
    }
};

/// 被调用者的声明语句：`a.fn` → 模块文件里的 fn，`o.m` → 实例容器里的方法，
/// `fn` → 本文件。参数名/参数类型/嵌套字面量提示共用这一个解析。
/// std 编译期内建（`std.format` / `std.process.args`）→ 合成一条 fn 声明
/// 文本，让参数名 inlay 与返回类型推断沿用同一套 sigOfStmt 解析。
/// 签名来自手工对齐的 std_builtin_docs（std.zx 里没有这些声明文本）。
fn builtinDeclStmt(callee: []const u8) ?[]const u8 {
    const bd = stdBuiltinDoc(callee) orelse return null;
    const lp = std.mem.indexOfScalar(u8, bd.sig, '(') orelse return null;
    var d: usize = 0;
    var rp: ?usize = null;
    var i = lp;
    while (i < bd.sig.len) : (i += 1) {
        if (bd.sig[i] == '(') {
            d += 1;
        } else if (bd.sig[i] == ')') {
            d -= 1;
            if (d == 0) {
                rp = i;
                break;
            }
        }
    }
    const r1 = rp orelse return null;
    const name = trimSl(bd.sig[0..lp]);
    var ret = bd.sig[r1 + 1 ..];
    if (std.mem.indexOf(u8, ret, " — ")) |cut| ret = ret[0..cut];
    ret = trimSl(ret);
    return std.fmt.allocPrint(gpa, "const {s} = fn {s} {s} {{ }}",
        .{ name, bd.sig[lp .. r1 + 1], ret }) catch null;
}

/// 返回类型里的 `Self` 换成容器真名：`?Self` → `?BufReader`。
/// 成员方法的声明文本写 Self，行内提示/推断要看到真名。
fn selfToOwner(ret: []const u8, callee: []const u8) []const u8 {
    if (std.mem.indexOf(u8, ret, "Self") == null) return ret;
    const dot = std.mem.lastIndexOfScalar(u8, callee, '.') orelse return ret;
    const before = callee[0..dot];
    var owner = if (std.mem.lastIndexOfScalar(u8, before, '.')) |d2|
        trimSl(before[d2 + 1 ..])
    else
        before;
    // 泛型实例化的实参不进类型名：`HashMap(i32)` → `HashMap`
    if (std.mem.indexOfScalar(u8, owner, '(')) |lp| owner = trimSl(owner[0..lp]);
    if (owner.len == 0 or std.mem.eql(u8, owner, "Self")) return ret;
    var out: std.ArrayList(u8) = .empty;
    var sp: usize = 0;
    while (std.mem.indexOfPos(u8, ret, sp, "Self")) |hp| {
        out.appendSlice(gpa, ret[sp..hp]) catch return ret;
        out.appendSlice(gpa, owner) catch return ret;
        sp = hp + 4;
    }
    out.appendSlice(gpa, ret[sp..]) catch return ret;
    return out.items;
}

fn calleeStmt(ctx: *InlayCtx, callee0: []const u8) ?[]const u8 {
    const text = ctx.text;
    const callee = trimSl(callee0);
    if (std.mem.lastIndexOfScalar(u8, callee, '.')) |dot| {
        const bind = callee[0..dot];
        const member = callee[dot + 1 ..];
        if (member.len == 0) return null;
        // 逐段下钻：模块成员 / 容器限定调用 / 泛型实例化段
        //（`std.arena.ArenaAllocator.init`、`std.map.HashMap(i32).init`）
        const fl = ctx.fileList();
        if (resolveMemberPath(fl, ctx.doc, text, callee, 0)) |hit| return hit.stmt;
        if (instanceContainerTy(fl, text, bind)) |ty| {
            if (findContainerBody(fl, ty)) |ch| {
                if (findDeclStmt(ch.body, member)) |st| return st;
            }
        }
        // std 编译期内建：签名只在手工对齐表里（std.format / std.process.args）
        return builtinDeclStmt(callee);
    }
    // 无点号：走本文件声明索引，避免每个名字都全文扫 4 遍
    if (ctx.decls == null) ctx.decls = DeclIndex.build(text);
    const offs = ctx.decls.?.map.get(callee) orelse return null;
    for (offs.items) |c| {
        for ([_][]const u8{ "const ", "let ", "var ", "fn " }) |kw| {
            if (declStmtAt(text, c, kw, callee)) |st| return st;
        }
    }
    return null;
}

/// KEEP — 匿名形参里的**嵌套匿名字面量**类型提示（用户 2026-11-08 第 2/5 条）：
/// `fn f(cfg: struct{ opts: struct{ width: i32, height: i32 }, flag: bool })`，
/// 调用处 `f(.{ .opts = .{ … } })` 里 `.opts` 及其更深层都没有类型提示。
/// 只给"值本身是匿名字面量"的字段插 `: struct{…}`；标量字段不加
///（02:15 已拍板逐字段标太吵）。lb 是外层字面量的 '{'，limit 是它的结束位置。
fn nestedLitHints(text: []const u8, pty: []const u8, lb: usize, limit: usize,
                   hints: *std.ArrayList(Hint), lines: LineIndex,
                   start_line: u32, end_line: u32) void {
    const flds = parseAnonTy(pty) orelse return;
    var depth: usize = 1;
    var in_str = false;
    var i = lb + 1;
    while (i < limit) : (i += 1) {
        const c = text[i];
        if (in_str) {
            if (c == '\\') { i += 1; continue; }
            if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; continue; }
        if (c == '{') { depth += 1; continue; }
        if (c == '}') {
            if (depth == 0) return;
            depth -= 1;
            if (depth == 0) return;
            continue;
        }
        if (depth != 1 or c != '.') continue;
        var j = i + 1;
        if (j >= limit or !isIdentChar(text[j])) continue;
        const ns = j;
        while (j < limit and isIdentChar(text[j])) j += 1;
        const nm = text[ns..j];
        var k = j;
        while (k < limit and (text[k] == ' ' or text[k] == '\t' or text[k] == '\n' or text[k] == '\r')) k += 1;
        if (k >= limit or text[k] != '=') continue;
        k += 1;
        while (k < limit and (text[k] == ' ' or text[k] == '\t' or text[k] == '\n' or text[k] == '\r')) k += 1;
        if (k + 1 >= limit or text[k] != '.' or text[k + 1] != '{') continue;
        var hit: ?[]const u8 = null;
        for (flds.items) |f| {
            if (std.mem.eql(u8, f.name, nm)) { hit = f.ty; break; }
        }
        const fty = hit orelse continue;
        if (fty.len == 0 or fty.len > 120) continue;
        const lc = lines.lineCol(text, j);
        if (lc.line >= start_line and lc.line <= end_line) {
            const lab = std.fmt.allocPrint(gpa, ": {s}", .{fty}) catch continue;
            hints.append(gpa, .{ .off = j, .line = lc.line, .character = lc.character,
                .label = lab, .kind = 1, .pad_right = true }) catch {};
        }
        // 再往里一层：`.opts = .{ .inner = .{ … } }`
        nestedLitHints(text, fty, k + 1, limit, hints, lines, start_line, end_line);
    }
}

/// 扫全文的调用括号，按顶层逗号切实参，在每个实参起点插 `name: `。
fn appendParamHints(text: []const u8, doc: *const Document, mask: []const bool, hints: *std.ArrayList(Hint), lines: LineIndex, start_line: u32, end_line: u32) void {
    // 一次请求一份缓存：目录只扫一次，同名被调方只解析一次（见 InlayCtx）
    var ctx = InlayCtx{ .doc = doc, .text = text };
    var lp: usize = 0;
    while (std.mem.indexOfScalarPos(u8, text, lp, '(')) |p0| {
        lp = p0 + 1;
        if (!mask[p0]) continue;
        var q = p0;
        while (q > 0 and (text[q - 1] == ' ' or text[q - 1] == '\t' or
            text[q - 1] == '\n' or text[q - 1] == '\r')) q -= 1;
        // 往回取整条被调路径：`std.map.HashMap(i32).init(…)` 里泛型实例化的
        // 括号也要跨过去，否则 callee 只剩 `init`、形参名一条都出不来
        //（用户 2026-11-09 第 14 条）。
        const cs = recvExprStart(text, q);
        if (cs == q) continue; // 分组表达式 / cast 前缀，'(' 前没有标识符
        const callee = trimSl(text[cs..q]);
        if (callee.len == 0 or callee[0] == '.' or isCallKeyword(callee)) continue;

        // 配对的 ')'
        var d: usize = 0;
        var in_str = false;
        var rp: ?usize = null;
        var i = p0 + 1;
        while (i < text.len) : (i += 1) {
            const ch = text[i];
            if (in_str) {
                if (ch == '\\') { i += 1; continue; }
                if (ch == '"') in_str = false;
                continue;
            }
            if (ch == '"') { in_str = true; continue; }
            if (ch == '(') { d += 1; continue; }
            if (ch == ')') {
                if (d == 0) { rp = i; break; }
                d -= 1;
                continue;
            }
        }
        const end = rp orelse continue;
        const cstmt = ctx.stmtCached(callee) orelse continue;
        const sig = sigOfStmt(cstmt);
        if (!sig.is_fn or sig.params.len == 0) continue;
        const names = sig.params;
        // 实例方法的第一个形参 self 不占实参位置
        var offset: usize = 0;
        if (std.mem.indexOfScalar(u8, callee, '.') != null and names.len > 0 and
            std.mem.eql(u8, paramName(names[0]), "self")) offset = 1;

        var seg: usize = p0 + 1;
        d = 0;
        in_str = false;
        var argi: usize = 0;
        var k = p0 + 1;
        while (k <= end) : (k += 1) {
            const at_end = k == end;
            if (!at_end) {
                const ch = text[k];
                if (in_str) {
                    if (ch == '\\') { k += 1; continue; }
                    if (ch == '"') in_str = false;
                    continue;
                }
                if (ch == '"') { in_str = true; continue; }
                if (ch == '(' or ch == '[' or ch == '{') { d += 1; continue; }
                if (ch == ')' or ch == ']' or ch == '}') { if (d > 0) d -= 1; continue; }
                if (ch != ',' or d != 0) continue;
            }
            var s0 = seg;
            while (s0 < k and (text[s0] == ' ' or text[s0] == '\t' or
                text[s0] == '\n' or text[s0] == '\r')) s0 += 1;
            // 实参以字符串开头时，起始位置正是那个 `"`——它被 mask 标成
            // 非代码位（防止在字符串里插提示），但这里插在引号**前**是安全的。
            if (s0 < k and (mask[s0] or text[s0] == '"') and argi + offset < names.len) {
                const nm = paramName(names[argi + offset]);
                const pty = paramStructTy(names[argi + offset]);
                const anon_arg = text[s0] == '.' and s0 + 1 < k and text[s0 + 1] == '{';
                if (validHintName(nm) and !argAlreadyNamed(text[s0..k])) {
                    const lc = lines.lineCol(text, s0);
                    if (lc.line >= start_line and lc.line <= end_line) {
                        // 匿名 struct 实参把类型一起标出来：`info: struct{…}`。
                        // 用户 2026-11-08 第 5 条：模块函数的匿名结构体参数
                        // 只有名字、没有类型，嵌套成员的类型更看不到。
                        const lab = if (anon_arg and pty != null and pty.?.len <= 120)
                            std.fmt.allocPrint(gpa, "{s}: {s}", .{ nm, pty.? }) catch continue
                        else
                            std.fmt.allocPrint(gpa, "{s}: ", .{nm}) catch continue;
                        hints.append(gpa, .{ .off = s0, .line = lc.line, .character = lc.character,
                            .label = lab, .kind = 2, .pad_right = false }) catch {};
                    }
                }
                // 匿名字面量实参：内层嵌套字面量的字段补 `: struct{…}`
                if (anon_arg) {
                    if (pty) |pty2| {
                        var e2 = k;
                        while (e2 > s0 and (text[e2 - 1] == ' ' or text[e2 - 1] == '\t' or
                            text[e2 - 1] == '\n' or text[e2 - 1] == '\r')) e2 -= 1;
                        nestedLitHints(text, pty2, s0 + 1, e2, hints, lines, start_line, end_line);
                    }
                }
            }
            argi += 1;
            seg = k + 1;
        }
    }
}

/// 行内类型提示（textDocument/inlayHint）：未显式写类型的 const/let/var
/// 绑定，在名字后插 `: Type`。推断复用 hover 的 inferExprType，两处一致。
///
/// KEEP — 用户 2026-11-08 明确要求保留的能力线。Zed 的 inlay_hints.enabled
/// 一直是 true，会持续发这个请求；能力声明或本函数一旦被删，编辑器里
/// `var p = Person{...}` 就会重新变回没有 `: Person`。删除/重写 zxls 前
/// 先跑 tools/lsptest.py：3n/3o/3p 共 19 条用例是这条线的守门断言
///（含调用处参数名 kind=2 与匿名 struct 绑定的类型推断）。
fn inlayHintsAt(doc: *const Document, start_line: u32, end_line: u32) []const u8 {
    const text = doc.text;
    const kws = [_][]const u8{ "const ", "let ", "var " };
    // 代码位置掩码：注释和字符串里的 `const x = 1` 不是声明
    const mask = gpa.alloc(bool, text.len) catch @panic("oom");
    defer gpa.free(mask);
    // 行首表建一次，下面每条提示的 line/character 都靠它查（见 LineIndex）
    const lines = LineIndex.build(text);
    {
        var i: usize = 0;
        var in_block = false;
        var in_line = false;
        var in_str = false;
        while (i < text.len) : (i += 1) {
            const ch = text[i];
            const nx: u8 = if (i + 1 < text.len) text[i + 1] else 0;
            if (in_line) {
                mask[i] = false;
                if (ch == '\n') in_line = false;
                continue;
            }
            if (in_block) {
                mask[i] = false;
                if (ch == '*' and nx == '/') {
                    in_block = false;
                    i += 1;
                    mask[i] = false;
                }
                continue;
            }
            if (in_str) {
                mask[i] = false;
                if (ch == '\\' and i + 1 < text.len) {
                    i += 1;
                    mask[i] = false;
                    continue;
                }
                if (ch == '"') in_str = false;
                continue;
            }
            if (ch == '/' and nx == '/') {
                in_line = true;
                mask[i] = false;
                i += 1;
                mask[i] = false;
                continue;
            }
            if (ch == '/' and nx == '*') {
                in_block = true;
                mask[i] = false;
                i += 1;
                mask[i] = false;
                continue;
            }
            if (ch == '"') {
                in_str = true;
                mask[i] = false;
                continue;
            }
            mask[i] = true;
        }
    }
    var hints: std.ArrayList(Hint) = .empty;
    defer hints.deinit(gpa);
    var pos: usize = 0;
    while (pos < text.len) {
        // 三个关键字里取最近的一个，保证 hints 天然按位置有序。
        //
        // 注意这里是**单趟向前扫**，不能对每个关键字各做一次 indexOfPos：
        // indexOfPos 会一路搜到文件尾，于是每个绑定都是 O(文件长度)，整个
        // 循环退化成 O(绑定数 × 文件长度)——16000 个 let 实测 15s。
        var best: ?usize = null;
        var best_kw: []const u8 = "";
        var scan = pos;
        outer: while (scan < text.len) : (scan += 1) {
            for (kws) |kw| {
                if (std.mem.startsWith(u8, text[scan..], kw)) {
                    best = scan;
                    best_kw = kw;
                    break :outer;
                }
            }
        }
        const c = best orelse break;
        pos = c + 1;
        if (!mask[c]) continue;
        if (c > 0 and isIdentChar(text[c - 1])) continue;
        if (!declStartsHere(text, c)) continue;

        var ns = c + best_kw.len;
        while (ns < text.len and (text[ns] == ' ' or text[ns] == '\t')) ns += 1;
        var ne = ns;
        while (ne < text.len and isIdentChar(text[ne])) ne += 1;
        if (ne == ns) continue;
        const name = text[ns..ne];
        if (name[0] == '_' or (name[0] >= '0' and name[0] <= '9')) continue;
        var eq = ne;
        while (eq < text.len and (text[eq] == ' ' or text[eq] == '\t')) eq += 1;
        if (eq >= text.len) continue;
        if (text[eq] == ':') continue; // 已显式写了类型
        if (text[eq] != '=') continue;
        if (eq + 1 < text.len and text[eq + 1] == '=') continue;

        // '=' 到顶层 ';'：跳过字符串、跟踪括号深度（fn 体里的 ';' 不算）
        var depth: usize = 0;
        var in_str = false;
        var k = eq + 1;
        var stop = text.len;
        while (k < text.len) : (k += 1) {
            const ch = text[k];
            if (in_str) {
                if (ch == '\\') {
                    k += 1;
                    continue;
                }
                if (ch == '"') in_str = false;
                continue;
            }
            switch (ch) {
                '"' => in_str = true,
                '{', '(', '[' => depth += 1,
                '}', ')', ']' => {
                    if (depth > 0) depth -= 1;
                },
                ';' => {
                    if (depth == 0) {
                        stop = k;
                        break;
                    }
                },
                else => {},
            }
        }
        const init = trimSl(text[eq + 1 .. stop]);
        if (init.len == 0) continue;
        // 容器 / 函数 / 模块绑定不是"值"，不插类型提示
        if (std.mem.startsWith(u8, init, "struct") or
            std.mem.startsWith(u8, init, "union") or
            std.mem.startsWith(u8, init, "enum") or
            std.mem.startsWith(u8, init, "error") or
            std.mem.startsWith(u8, init, "fn ") or
            std.mem.startsWith(u8, init, "fn(") or
            std.mem.startsWith(u8, init, "module(")) continue;

        const ty = inferExprType(text, doc, init, 0) orelse continue;
        // KEEP: 上限 64 → 120（嵌套匿名类型串如
        // struct{pos:struct{x:i32,y:i32},tag:str} 有 39+ 字符，用户要求
        // 嵌套匿名类型也要有行内提示；再长就放弃，防止提示糊屏）。
        if (ty.len == 0 or ty.len > 120) continue;
        var valid = true;
        for (ty) |ch| {
            if (!(isIdentChar(ch) or ch == '.' or ch == '*' or ch == '[' or
                ch == ']' or ch == '?' or ch == '!' or ch == '{' or ch == '}' or
                ch == ',' or ch == ' ' or ch == ':')) {
                valid = false;
                break;
            }
        }
        if (!valid) continue;
        const lc = lines.lineCol(text, ne);
        if (lc.line < start_line or lc.line > end_line) continue;
        var lab: std.ArrayList(u8) = .empty;
        lab.appendSlice(gpa, ": ") catch @panic("oom");
        lab.appendSlice(gpa, ty) catch @panic("oom");
        hints.append(gpa, .{ .off = ne, .line = lc.line, .character = lc.character,
            .label = lab.items, .kind = 1, .pad_right = true }) catch @panic("oom");
    }

    appendParamHints(text, doc, mask, &hints, lines, start_line, end_line);

    // 类型提示与参数提示按文档偏移合并。两段各自有序，但要合成一条序列。
    // 这里原来是插入排序，注释写"数量很小"——大文件里并不小：48000 条提示
    // 就是 ~1.1e9 次搬移，实测单次请求 16s。改成 pdqsort，O(n log n)。
    std.mem.sort(Hint, hints.items, {}, hintLess);

    var j = Json{};
    j.buf.append(gpa, '[') catch @panic("oom");
    for (hints.items, 0..) |h, idx| {
        if (idx > 0) j.buf.append(gpa, ',') catch @panic("oom");
        j.raw("{{\"position\":{{\"line\":{d},\"character\":{d}}},\"label\":", .{ h.line, h.character });
        j.str(h.label);
        if (h.pad_right) {
            j.raw(",\"kind\":{d},\"paddingLeft\":false,\"paddingRight\":true}}", .{h.kind});
        } else {
            j.raw(",\"kind\":{d},\"paddingLeft\":false,\"paddingRight\":false}}", .{h.kind});
        }
    }
    j.buf.append(gpa, ']') catch @panic("oom");
    return j.buf.items;
}

/// 调用处参数提示：光标在调用的括号内 → 签名 + activeParameter
fn signatureHelpAt(doc: *const Document, line: u32, character: u32) ?[]const u8 {
    const text = doc.text;
    const off = posToOffset(text, line, character);
    if (off == 0 or off > text.len) return null;

    // 向后找未闭合的 '('（跳过字符串，跟踪括号嵌套）
    var i = off;
    var depth: usize = 0;
    var in_str = false;
    var lp: ?usize = null;
    while (i > 0) {
        i -= 1;
        const ch = text[i];
        if (in_str) {
            if (ch == '"' and (i == 0 or text[i - 1] != '\\')) in_str = false;
            continue;
        }
        switch (ch) {
            '"' => in_str = true,
            ')' => depth += 1,
            '(' => {
                if (depth == 0) {
                    lp = i;
                    break;
                }
                depth -= 1;
            },
            '\n' => {
                // 跨行调用允许，但连着空行就放弃
                var nl_count: u8 = 0;
                var k = i;
                while (k > 0 and nl_count < 8) : (k -= 1) {
                    if (text[k] == '\n') nl_count += 1;
                }
                if (nl_count >= 8) return null;
            },
            else => {},
        }
    }
    const lpp = lp orelse return null;

    // activeParameter：'(' 到光标之间顶层逗号数
    var ap: u32 = 0;
    depth = 0;
    in_str = false;
    var q = lpp + 1;
    while (q < off and q < text.len) : (q += 1) {
        const ch = text[q];
        if (in_str) {
            if (ch == '\\') {
                q += 1;
                continue;
            }
            if (ch == '"') in_str = false;
            continue;
        }
        if (ch == '"') {
            in_str = true;
            continue;
        }
        if (ch == '(' or ch == '[' or ch == '{') depth += 1;
        if (ch == ')' or ch == ']' or ch == '}') {
            if (depth > 0) depth -= 1;
        }
        if (ch == ',' and depth == 0) ap += 1;
    }

    // 被调方路径
    var cs = lpp;
    while (cs > 0 and (isIdentChar(text[cs - 1]) or text[cs - 1] == '.')) cs -= 1;
    const callee = text[cs..lpp];
    if (callee.len == 0) return null;

    // 解析签名
    var label: []const u8 = "";
    var params: []const []const u8 = &.{};
    var built: std.ArrayList(u8) = .empty;
    if (std.mem.lastIndexOfScalar(u8, callee, '.')) |dot| {
        const bind = callee[0..dot];
        const member = callee[dot + 1 ..];
        var mm: ?[]const u8 = moduleOfBinding(text, bind);
        if (mm == null and std.mem.eql(u8, bind, "std")) mm = "std";
        if (mm) |mod| {
            if (resolveModuleFile(doc, mod)) |p| {
                if (readFileSmall(p)) |t| {
                    if (findDeclStmt(t, member)) |stmt| {
                        const sig = sigOfStmt(stmt);
                        if (sig.is_fn) {
                            built.appendSlice(gpa, member) catch {};
                            appendSigTail(&built, sig);
                            label = built.items;
                            params = sig.params;
                        }
                    }
                }
            }
        }
        if (label.len == 0) {
            // std 内建：给个近似标签
            if (std.mem.eql(u8, bind, "std")) {
                built.appendSlice(gpa, callee) catch {};
                built.appendSlice(gpa, "(…)") catch {};
                label = built.items;
            }
        }
        if (label.len == 0) {
            // 实例方法（g.spawn → Group 体里的 spawn）与容器限定静态调用
            //（std.Task.coro / task.Obj.make / Obj.make）
            var sf = scanFiles(doc, text);
            defer sf.deinit(gpa);
            var stmt: ?[]const u8 = null;
            if (instanceContainerTy(sf.items, text, bind)) |ty| {
                if (findContainerBody(sf.items, ty)) |ch|
                    stmt = findDeclStmt(ch.body, member);
            }
            if (stmt == null) stmt = resolvePathStmt(sf.items, doc, text, callee);
            if (stmt) |s| {
                const sig = sigOfStmt(s);
                if (sig.is_fn) {
                    built.appendSlice(gpa, member) catch {};
                    appendSigTail(&built, sig);
                    label = built.items;
                    params = sig.params;
                }
            }
        }
    } else if (findDeclStmt(text, callee)) |stmt| {
        const sig = sigOfStmt(stmt);
        if (sig.is_fn) {
            built.appendSlice(gpa, callee) catch {};
            appendSigTail(&built, sig);
            label = built.items;
            params = sig.params;
        }
    }
    if (label.len == 0) return null;

    var j = Json{};
    j.buf.appendSlice(gpa, "{\"signatures\":[{\"label\":") catch return null;
    j.str(label);
    j.buf.appendSlice(gpa, ",\"parameters\":[") catch return null;
    // ParameterInformation 必须是 {"label": …} 对象（label 是签名 label 里的
    // 子串供编辑器定位高亮），发纯字符串 Zed 反序列化直接拒收整条响应。
    for (params, 0..) |p, idx| {
        if (idx > 0) j.buf.append(gpa, ',') catch {};
        j.buf.appendSlice(gpa, "{\"label\":") catch {};
        j.str(p);
        j.buf.append(gpa, '}') catch {};
    }
    j.buf.appendSlice(gpa, "]}]") catch return null;
    j.raw(",\"activeSignature\":0,\"activeParameter\":{d}}}", .{ap});
    return j.buf.items;
}

fn appendSigTail(out: *std.ArrayList(u8), sig: FnSig) void {
    out.append(gpa, '(') catch {};
    for (sig.params, 0..) |p, idx| {
        if (idx > 0) out.appendSlice(gpa, ", ") catch {};
        out.appendSlice(gpa, p) catch {};
    }
    out.append(gpa, ')') catch {};
    if (sig.ret.len > 0) {
        out.append(gpa, ' ') catch {};
        out.appendSlice(gpa, sig.ret) catch {};
    }
}

// ---------------------------------------------------------------- completion

/// tier：0 上下文成员（std.* / @ 内建 / 容器字段）
///      1 当前文件标识符
///      2 关键字
///      3 工作区其他文件的标识符
/// Zed 按 sortText 排序，前缀字母分层保证上下文匹配永远排在噪音前面。
fn addItem(list: *Json, seen: *std.StringHashMapUnmanaged(void), label: []const u8, kind: u32, detail: []const u8, tier: u8) void {
    if (seen.contains(label)) return;
    seen.put(gpa, dupe(label), {}) catch {};
    if (seen.count() > 1) list.raw(",", .{});
    list.raw("{{\"label\":", .{});
    list.str(label);
    list.raw(",\"kind\":{d}", .{kind});
    if (detail.len > 0) {
        list.raw(",\"detail\":", .{});
        list.str(detail);
    }
    var st: std.ArrayList(u8) = .empty;
    st.append(gpa, 'a' + tier) catch {};
    st.appendSlice(gpa, label) catch {};
    list.raw(",\"sortText\":", .{});
    list.str(st.items);
    list.raw("}}", .{});
}

/// LSP snippet 补全项：insertTextFormat=2 让客户端把 insertText 当 snippet
/// 解析（tabstop / 默认值），确认后按 Tab 在占位符间跳转。
/// kind=15 是 CompletionItemKind.Snippet；sortText 用 'b' 层，排在
/// 工作区噪音之前、上下文成员之后。
fn addSnippet(list: *Json, seen: *std.StringHashMapUnmanaged(void), s: Snip) void {
    if (seen.contains(s.label)) return;
    seen.put(gpa, dupe(s.label), {}) catch {};
    if (seen.count() > 1) list.raw(",", .{});
    list.raw("{{\"label\":", .{});
    list.str(s.label);
    list.raw(",\"kind\":15,\"detail\":", .{});
    list.str(s.detail);
    var st: std.ArrayList(u8) = .empty;
    st.append(gpa, 'a' + 1) catch {};
    st.appendSlice(gpa, s.label) catch {};
    list.raw(",\"sortText\":", .{});
    list.str(st.items);
    list.raw(",\"insertText\":", .{});
    list.str(s.body);
    list.raw(",\"insertTextFormat\":2", .{});
    list.raw("}}", .{});
}

/// KEEP - 表达式位置的自定义类型实例 / 函数调用骨架。用户 2026-11-08：
/// "自定义类型实例和函数调用 添加snippet 提示"。当前文件声明的容器
/// （const Name = struct/union {…}）补一条 `Name{ .f = … }` 实例骨架，
/// 声明的函数补一条带形参占位的 `name(a, b)` 调用骨架。只收当前文件——
/// 跨文件裸名不可用（名字只能来自显式绑定，那类工作区噪音已按用户要求
/// 删除）；模块成员走 `绑定名.` 点补全。首参 self 是实例方法，不列调用
/// 骨架（`o.bump()` 由点补全负责）；enum 只出变体，不造字面量骨架。
/// 骨架 label 带 `()` / `{...}` 后缀；调用/构造骨架发出时都会把裸名占进
/// seen，裸标识符项不再并存（addCallSnippet / addCtorSnippet 里做）。静态模板表
/// （if/fn/new…）先入 seen，与本函数同名也不会撞。展开形态由
/// lsptest.py completion.exprSnippet.* 钉住。
fn addSnippetRaw(list: *Json, seen: *std.StringHashMapUnmanaged(void),
                 label: []const u8, insert: []const u8, detail: []const u8) void {
    if (seen.contains(label)) return;
    seen.put(gpa, dupe(label), {}) catch {};
    if (seen.count() > 1) list.raw(",", .{});
    list.raw("{{\"label\":", .{});
    list.str(label);
    list.raw(",\"kind\":15,\"detail\":", .{});
    list.str(detail);
    var st: std.ArrayList(u8) = .empty;
    defer st.deinit(gpa);
    st.append(gpa, 'b') catch {};
    st.appendSlice(gpa, label) catch {};
    list.raw(",\"sortText\":", .{});
    list.str(st.items);
    list.raw(",\"insertText\":", .{});
    list.str(insert);
    list.raw(",\"insertTextFormat\":2", .{});
    list.raw("}}", .{});
}

/// KEEP — 模块/容器成员的调用骨架（用户 2026-11-08 第 4 条）：
/// `a.tw` → `twice(${1:x})$0`，带括号与形参占位。
/// KEEP — 实例方法同样出骨架（用户 2026-11-09 第 7 条：std.BufReader/
/// BufWriter 实例的方法没有提示和 snippet）：`self` 是隐含接收者，插入
/// 形态 `next()$0` / `write(${1:data})$0`，detail 里的 self 形参与返回类型
/// 的 `Self` 都不再暴露给用户（Self 换成 owner 容器真名）。
/// 成员签名的 detail：`name(params) ret`。首参 self 是隐含接收者，不摆给
/// 用户看；返回类型里的 `Self` 换成容器真名（owner）。普通项与骨架项共用
/// 这一份，弹窗里两条显示一致。
/// 把文本里的 `Self` 换成接收者容器名（`*const Self` → `*const Task`）。
fn appendSelfSub(sb: *std.ArrayList(u8), txt: []const u8, owner: []const u8) void {
    if (owner.len == 0 or std.mem.indexOf(u8, txt, "Self") == null) {
        sb.appendSlice(gpa, txt) catch return;
        return;
    }
    var sp: usize = 0;
    while (std.mem.indexOfPos(u8, txt, sp, "Self")) |hp| {
        sb.appendSlice(gpa, txt[sp..hp]) catch return;
        sb.appendSlice(gpa, owner) catch return;
        sp = hp + "Self".len;
    }
    sb.appendSlice(gpa, txt[sp..]) catch return;
}

/// 成员补全的 detail 签名。`keep_self` 为真时保留首个 `self: *const Self`
/// 形参——**实例**补全必须保留：不写出来，`spawn(f: …) ?usize` 这种串
/// 看着和关联函数一模一样，用户 2026-11-10 截图两次把它当成污染。
fn appendMemberSig(sb: *std.ArrayList(u8), name: []const u8, sig: FnSig,
                   owner: []const u8, keep_self: bool) void {
    sb.appendSlice(gpa, name) catch return;
    sb.append(gpa, '(') catch return;
    var first = true;
    for (sig.params, 0..) |p0, pi| {
        const is_self = pi == 0 and std.mem.eql(u8, paramName(p0), "self");
        if (is_self and !keep_self) continue;
        if (!first) sb.appendSlice(gpa, ", ") catch return;
        first = false;
        appendSelfSub(sb, p0, owner);
    }
    sb.append(gpa, ')') catch return;
    if (sig.ret.len == 0) return;
    sb.append(gpa, ' ') catch return;
    appendSelfSub(sb, trimSl(sig.ret), owner);
}

fn memberCallSnippet(list: *Json, seen: *std.StringHashMapUnmanaged(void),
                     stmt: []const u8, name: []const u8, owner: []const u8,
                     keep_self: bool) bool {
    const sig = sigOfStmt(stmt);
    if (!sig.is_fn) return false;
    const self_first = sig.params.len > 0 and
        std.mem.eql(u8, paramName(sig.params[0]), "self");
    var ins: std.ArrayList(u8) = .empty;
    defer ins.deinit(gpa);
    ins.appendSlice(gpa, name) catch return false;
    ins.append(gpa, '(') catch return false;
    var first = true;
    var tab: usize = 1;
    for (sig.params, 0..) |p0, pi| {
        if (self_first and pi == 0) continue;
        const nm = paramName(p0);
        if (tab > 6) break;
        if (!first) ins.appendSlice(gpa, ", ") catch return false;
        first = false;
        if (nm.len == 0) {
            // 无名形参（`fn (i32, struct{a:u32,b:u32})`）：占位放类型，让 Tab
            // 过去时看得见该填什么。必须 escapeSnippet——`}` 会提前关掉占位符，
            // 插入就成了半截类型（用户 2026-11-11 截图 `f3(i32, struct{a)`）。
            ins.print(gpa, "${{{d}:", .{tab}) catch return false;
            escapeSnippet(&ins, paramPlaceholderText(p0));
            ins.append(gpa, '}') catch return false;
        } else {
            ins.print(gpa, "${{{d}:", .{tab}) catch return false;
            escapeSnippet(&ins, nm);
            ins.append(gpa, '}') catch return false;
        }
        tab += 1;
    }
    ins.appendSlice(gpa, ")$0") catch return false;
    // detail 保留完整签名：骨架插的是 `name(…)`，弹窗右侧仍要能看出形参
    // 与返回类型（10b 当初的诉求），kind=15 只影响插入形态。
    var sb: std.ArrayList(u8) = .empty;
    defer sb.deinit(gpa);
    appendMemberSig(&sb, name, sig, owner, keep_self);
    // label 就是函数名本身：函数只出这一条（kind=15、带 snippet 占位），
    // 后面 addItem 的裸名项会被 seen 去重掉。用户 2026-11-09 第 13 条：
    // "只提示类型和提示 snippet 两条都要"是针对**类型**的（类型项 + `Name{...}`）；
    // 函数再并一条裸名项就是弹窗里同一个函数出现两次（截图 `createDir` /
    // `createDir(...)`），纯噪音。
    addSnippetRaw(list, seen, name, ins.items, sb.items);
    return true;
}

/// 类型项的 CompletionItemKind：struct/union → 22（Struct），enum → 13。
/// 用户 2026-11-09 第 12 条：类型项要能一眼看出"这是类型"。
fn containerKindOf(init: []const u8) u32 {
    if (std.mem.startsWith(u8, init, "enum")) return 13;
    return 22;
}

/// 模块/容器类型的构造骨架（与函数骨架同一套约定：普通项照出，这里只
/// 追加一条 `Name{...}`，label 带后缀所以不会和类型项抢去重）。
/// 带方法的容器（DirIter / BufReader / Task 这类句柄）不出字面量骨架——
/// 把 `.h = 0, .buf = …` 这些内部字段摆给用户只会误导；它们的构造入口
/// 在 `Name.` 成员补全里（open / create / single）。用户 2026-11-09 第 12 条：
/// `std.fs.` 能出 DirIter 这一层，但不该把下一层的 `DirIter.open` 混进来。
fn memberCtorSnippet(list: *Json, seen: *std.StringHashMapUnmanaged(void),
                     src: []const u8, name: []const u8) void {
    var fields: std.ArrayList(Decl) = .empty;
    defer fields.deinit(gpa);
    if (!containerMembers(&[_][]const u8{src}, name, &fields)) return;
    const st = findDeclStmt(src, name) orelse return;
    const ci = containerInitOf(st) orelse return;
    if (std.mem.startsWith(u8, ci, "enum")) return;   // enum 值不是字面量构造
    // 带方法的容器是句柄/行为类型（DirIter / BufReader / Task）：字面量骨架
    // 只会把 `.h = 0, .buf = …` 这些内部字段摆出来。这类类型的构造入口在
    // `Name.` 成员补全里（open / create / single）。
    const body = (findContainerBody(&[_][]const u8{src}, name) orelse return).body;
    for (fields.items) |dcl| {
        if (dcl.kind != .value or dcl.is_fn) continue;
        const mst = findDeclStmt(body, dcl.name) orelse continue;
        if (sigOfStmt(mst).is_fn) return;
    }
    var ins: std.ArrayList(u8) = .empty;
    defer ins.deinit(gpa);
    ins.appendSlice(gpa, name) catch return;
    var lab: std.ArrayList(u8) = .empty;
    defer lab.deinit(gpa);
    lab.print(gpa, "{s}{{...}}", .{name}) catch return;
    if (std.mem.startsWith(u8, ci, "union")) {
        // union(enum)：变体是唯一可写的成员，出首个变体
        for (fields.items) |dcl| {
            if (dcl.kind != .variant or dcl.name.len == 0) continue;
            ins.print(gpa, "{{ .{s} = ${{1:0}} }}$0", .{dcl.name}) catch return;
            addSnippetRaw(list, seen, lab.items, ins.items, "构造实例 union（首个变体）");
            return;
        }
        return;
    }
    ins.appendSlice(gpa, "{ ") catch return;
    var used: usize = 0;
    var tab: usize = 1;
    for (fields.items) |dcl| {
        if (dcl.kind != .field or dcl.name.len == 0) continue;
        if (tab > 3) break;
        if (used > 0) ins.appendSlice(gpa, ", ") catch return;
        ins.print(gpa, ".{s} = ${{{d}:0}}", .{ dcl.name, tab }) catch return;
        used += 1;
        tab += 1;
    }
    if (used == 0) return;
    ins.appendSlice(gpa, " }$0") catch return;
    addSnippetRaw(list, seen, lab.items, ins.items, "构造实例 struct");
}

fn addCallSnippet(list: *Json, seen: *std.StringHashMapUnmanaged(void),
                  text: []const u8, name: []const u8) void {
    const stmt = findDeclStmt(text, name) orelse return;
    const sig = sigOfStmt(stmt);
    if (!sig.is_fn) return;
    var ins: std.ArrayList(u8) = .empty;
    defer ins.deinit(gpa);
    ins.appendSlice(gpa, name) catch return;
    ins.append(gpa, '(') catch return;
    var first = true;
    var tab: usize = 1;
    for (sig.params) |p0| {
        const nm = paramName(p0);
        // 首参 self 是实例方法：`o.bump()` 走点补全，顶层不列调用骨架
        if (tab == 1 and std.mem.eql(u8, nm, "self")) return;
        if (tab > 6) break;
        if (!first) ins.appendSlice(gpa, ", ") catch return;
        first = false;
        if (nm.len == 0) {
            // 无名形参：占位放类型（同 addCallSnippet）
            ins.print(gpa, "${{{d}:", .{tab}) catch return;
            escapeSnippet(&ins, paramPlaceholderText(p0));
            ins.append(gpa, '}') catch return;
        } else {
            ins.print(gpa, "${{{d}:", .{tab}) catch return;
            escapeSnippet(&ins, nm);
            ins.append(gpa, '}') catch return;
        }
        tab += 1;
    }
    ins.appendSlice(gpa, ")$0") catch return;
    var lab: std.ArrayList(u8) = .empty;
    defer lab.deinit(gpa);
    lab.print(gpa, "{s}(...)", .{name}) catch return;
    addSnippetRaw(list, seen, lab.items, ins.items, "调用骨架");
    // KEEP: 与构造骨架同一条去重规则——骨架发出后把裸名占进 seen（本函数在
    // addIdents 之前跑），`push 当前文件` 不再和 `push(...) 调用骨架` 并排
    // 弹两条（用户 2026-11-08：函数的原型那条不要）。骨架没发成（首参 self
    // 的实例方法、非 fn 声明）裸名照常保留。
    seen.put(gpa, dupe(name), {}) catch {};
}

fn addCtorSnippet(list: *Json, seen: *std.StringHashMapUnmanaged(void),
                  files: []const []const u8, name: []const u8) void {
    var fields: std.ArrayList(Decl) = .empty;
    defer fields.deinit(gpa);
    if (!containerMembers(files, name, &fields)) return;
    var ins: std.ArrayList(u8) = .empty;
    defer ins.deinit(gpa);
    ins.appendSlice(gpa, name) catch return;
    ins.appendSlice(gpa, "{ ") catch return;
    var used: usize = 0;
    var tab: usize = 1;
    for (fields.items) |dcl| {
        if (dcl.is_fn or dcl.kind != .field or dcl.name.len == 0) continue;
        if (tab > 3) break;
        if (used > 0) ins.appendSlice(gpa, ", ") catch return;
        ins.print(gpa, ".{s} = ${{{d}:0}}", .{ dcl.name, tab }) catch return;
        used += 1;
        tab += 1;
    }
    if (used == 0) return;
    ins.appendSlice(gpa, " }$0") catch return;
    var lab: std.ArrayList(u8) = .empty;
    defer lab.deinit(gpa);
    lab.print(gpa, "{s}{{...}}", .{name}) catch return;
    addSnippetRaw(list, seen, lab.items, ins.items, "构造实例");
    // KEEP: 骨架发出后把裸名占进 seen——本函数在 addIdents **之前**调用，
    // 裸标识符项（`Person 当前文件`）就不会再和 `Person{...} 构造实例`
    // 并排弹两条（用户 2026-11-08：只要骨架那条）。骨架没发成（enum/零字段
    // 容器）裸名照常保留。
    seen.put(gpa, dupe(name), {}) catch {};
}

fn addExprSnippets(list: *Json, seen: *std.StringHashMapUnmanaged(void),
                   doc: *const Document, text: []const u8, prefix: []const u8) void {
    if (prefix.len == 0) return;
    var decls: std.ArrayList(Decl) = .empty;
    defer decls.deinit(gpa);
    topLevelDecls(text, &decls, false);
    var any_container = false;
    for (decls.items) |d| {
        if (!std.mem.startsWith(u8, d.name, prefix)) continue;
        // 函数性由声明本身判定：`const make = fn () …` 在 topLevelDecls 里
        // 是 .value（is_fn 只标裸 fn NAME(），那条给 extern 绑定用）。
        var is_fn_decl = false;
        if (findDeclStmt(text, d.name)) |st| {
            if (sigOfStmt(st).is_fn) {
                addCallSnippet(list, seen, text, d.name);
                is_fn_decl = true;
            }
        }
        if (!is_fn_decl) any_container = true;
    }
    if (!any_container) return;
    var sf = scanFiles(doc, text);
    defer sf.deinit(gpa);
    for (decls.items) |d| {
        if (!std.mem.startsWith(u8, d.name, prefix)) continue;
        if (findDeclStmt(text, d.name)) |st| {
            if (sigOfStmt(st).is_fn) continue;
        }
        if (isContainerIn(sf.items, d.name)) addCtorSnippet(list, seen, sf.items, d.name);
    }
}

fn isKeywordWord(w: []const u8) bool {
    for (keywords) |kw| {
        if (std.mem.eql(u8, kw, w)) return true;
    }
    return false;
}

/// KEEP - 模块导入补全。名字只能来自显式 `const <名> = module("…")` 绑定：
/// 曾试过"模块名解析不到绑定就隐式当命名空间"，用户 2026-11-08 否掉——
/// 隐式名一旦和别名/再导出撞名，就再没有语法能指回隐式命名空间
/// （`pub const glfw3 = glfw3;` 右边会被自己的绑定吃掉），文件一多必乱。
/// 所以这里不制造隐式名字：选中项插入的是**一条绑定语句**，$1 落在绑定名
/// 上，直接输入即可改成缩写（const g = module("glfw3");）。再导出同理，
/// 由人显式写 `pub const`，名字冲突完全可见、可控。
fn addImportItem(list: *Json, seen: *std.StringHashMapUnmanaged(void), stem: []const u8) void {
    if (seen.contains(stem)) return;
    seen.put(gpa, dupe(stem), {}) catch {};
    if (seen.count() > 1) list.raw(",", .{});
    var body: [1024]u8 = undefined;
    const ins = std.fmt.bufPrint(&body,
        "const ${{1:{s}}} = module(\"{s}\");$0", .{ stem, stem }) catch return;
    var st: [1024]u8 = undefined;
    const sort = std.fmt.bufPrint(&st, "b{s}", .{stem}) catch return;
    list.raw("{{\"label\":", .{});
    list.str(stem);
    list.raw(",\"kind\":9,\"detail\":\"导入模块（$1 可改别名）\"", .{});
    list.raw(",\"sortText\":", .{});
    list.str(sort);
    list.raw(",\"insertText\":", .{});
    list.str(ins);
    list.raw(",\"insertTextFormat\":2}}", .{});
}

/// 只在"声明位置"（行首，允许缩进）列导入项：表达式中间插入 const 会
/// 破坏语法。必须放在 addIdents 之前：未绑定的裸名（写错的 `glue.`）
/// 也会被 collectIdents 当普通标识符塞进 seen，把导入项静默挡掉；
/// 已导入过的模块（含别名绑定）由 moduleImported 按路径排除。
/// 已经导入过这个模块就不再提示——按 module("…") 里的路径判，而不是按
/// 绑定名：`const g = module("glfw3");` 用的是别名，绑定名 g 与模块名不同，
/// 只看绑定名会反复提示"导入 glfw3"。注释里的字样不算数。
fn moduleImported(text: []const u8, stem: []const u8) bool {
    var b1: [1024]u8 = undefined;
    const n1 = std.fmt.bufPrint(&b1, "module(\"{s}\")", .{stem}) catch return false;
    var b2: [1024]u8 = undefined;
    const n2 = std.fmt.bufPrint(&b2, "module(\"{s}.zx\")", .{stem}) catch return false;
    for ([_][]const u8{ n1, n2 }) |needle| {
        var from: usize = 0;
        while (std.mem.indexOfPos(u8, text, from, needle)) |p| {
            if (!inLineComment(text, p)) return true;
            from = p + needle.len;
        }
    }
    return false;
}

fn addImportModules(list: *Json, seen: *std.StringHashMapUnmanaged(void),
                    text: []const u8, off: usize, doc_dir: []const u8,
                    prefix: []const u8) void {
    if (prefix.len == 0 or off < prefix.len) return;
    var p = off - prefix.len;
    while (p > 0 and (text[p - 1] == ' ' or text[p - 1] == '\t')) p -= 1;
    if (p > 0 and text[p - 1] != '\n' and text[p - 1] != '\r') return;

    if (std.mem.startsWith(u8, "std", prefix) and
        !moduleImported(text, "std"))
        addImportItem(list, seen, "std");
    var dirs: std.ArrayList([]const u8) = .empty;
    defer dirs.deinit(gpa);
    modDirsInto(doc_dir, &dirs);
    for (dirs.items) |d| {
        var dir = Io.Dir.cwd().openDir(g_io, d, .{ .iterate = true }) catch continue;
        defer dir.close(g_io);
        var it = dir.iterate();
        while (it.next(g_io) catch null) |entry| {
            if (entry.kind != .file or !std.mem.endsWith(u8, entry.name, ".zx")) continue;
            const stem = entry.name[0 .. entry.name.len - 3];
            if (std.mem.eql(u8, stem, "std")) continue;
            if (std.mem.startsWith(u8, stem, prefix) and
                !moduleImported(text, stem))
                addImportItem(list, seen, stem);
        }
    }
}

fn addIdents(list: *Json, seen: *std.StringHashMapUnmanaged(void), doc_text: []const u8, prefix: []const u8) void {
    // 当前文件标识符（tier 1）；关键字不算标识符
    var local: std.StringHashMapUnmanaged(void) = .empty;
    defer local.deinit(gpa);
    collectIdents(doc_text, &local);
    var it = local.keyIterator();
    while (it.next()) |k| {
        if (isKeywordWord(k.*)) continue;
        if (prefix.len == 0 or std.mem.startsWith(u8, k.*, prefix))
            addItem(list, seen, k.*, 6, "当前文件", 1);
    }
}

/// 点补全的接收者是什么：
///   module   模块命名空间（std.* / `const a = module("…")` 绑定）—— 全是模块成员
///   ty       类型（`Task.` / `std.BufReader.` / `HashMap(i32).`）—— 只有静态成员
///   instance 值（`t.` / `v.` / `arena.?.`）—— 只有实例成员
/// 三者不能混。sema 的规则：`Task.id` 是 E0300（实例字段要先造值），
/// `t.coro()` 是 E0303（首参不是 self，不是方法），`t.Group` 是 E0304
///（实例上没有嵌套类型）。用户 2026-11-10："Task 命名空间污染了"。
const RecvKind = enum { module, ty, instance };

/// `if (arena) a { … }` / `while (x) v { … }` 的捕获绑定：返回条件表达式
/// 解包后的类型（`?Allocator` → `Allocator`）。捕获变量没有声明语句，
/// findDeclStmt 找不到，补全/hover 的整条类型链就断在 `a.` 上。
fn captureBindTy(text: []const u8, doc: *const Document, name: []const u8) ?[]const u8 {
    if (name.len == 0) return null;
    const kws = [_][]const u8{ "if (", "while (" };
    for (kws) |kw| {
        var from: usize = 0;
        while (std.mem.indexOfPos(u8, text, from, kw)) |kp| {
            from = kp + kw.len;
            if (inLineComment(text, kp)) continue;
            // 配平 '(' → ')'，条件表达式在括号内
            var d: usize = 1;
            var i = kp + kw.len;
            var in_str = false;
            while (i < text.len and d > 0) : (i += 1) {
                const c = text[i];
                if (in_str) {
                    if (c == '\\') { i += 1; continue; }
                    if (c == '"') in_str = false;
                    continue;
                }
                if (c == '"') { in_str = true; continue; }
                if (c == '(') d += 1;
                if (c == ')' and d > 0) d -= 1;
            }
            if (d != 0) continue;
            const cond = trimSl(text[kp + kw.len .. i - 1]);
            if (cond.len == 0) continue;
            // ')' 之后跳过空白就是绑定名（`if (x) v {` / `while (q) it {`）
            var k = i;
            while (k < text.len and (text[k] == ' ' or text[k] == '\t')) k += 1;
            if (!std.mem.startsWith(u8, text[k..], name)) continue;
            const e = k + name.len;
            if (e < text.len and isIdentChar(text[e])) continue;
            var t = inferExprType(text, doc, cond, 1) orelse continue;
            t = trimSl(t);
            if (t.len > 0 and (t[0] == '?' or t[0] == '!')) t = trimSl(t[1..]);
            if (t.len > 0) return t;
        }
    }
    return null;
}

/// 接收者的声明类型是可选值（`?T`）时，返回解包后的内层类型串。
/// `arena.` 在 Zx 里唯一合法后缀是 `.?`——`arena.alloc` 编译不过，所以
/// 不能把内层类型的成员摆出来（用户 2026-11-10 截图同一处）。
fn recvOptionalTy(text: []const u8, doc: *const Document, recv: []const u8) ?[]const u8 {
    if (recv.len == 0) return null;
    const ty0: []const u8 = blk: {
        if (findDeclStmt(text, recv)) |st| {
            if (declTyOf(st)) |dt| break :blk trimSl(dt);
        }
        break :blk inferExprType(text, doc, recv, 0) orelse return null;
    };
    var t = trimSl(ty0);
    if (t.len < 2 or t[0] != '?') return null;
    t = trimSl(t[1..]);
    if (t.len == 0 or t.len > 80) return null;
    return t;
}

/// 调用表达式的末位被调名（`std.map.HashMap(V: i32)` → `HashMap`，
/// `a.b.make(x)` → `make`）。不是"整条表达式就是一个调用"时返回 null。
fn finalCalleeOf(expr: []const u8) ?[]const u8 {
    const e = trimSl(expr);
    if (e.len == 0 or e[e.len - 1] != ')') return null;
    const lp = finalCallOpen(e) orelse return null;
    const callee = trimSl(e[recvExprStart(e, lp)..lp]);
    if (callee.len == 0) return null;
    var last = callee;
    if (std.mem.lastIndexOfScalar(u8, callee, '.')) |d| last = trimSl(callee[d + 1 ..]);
    if (last.len == 0 or !isIdentChar(last[0])) return null;
    return last;
}

/// `const F = fn (…) type { return X…; };` 的返回表达式（X…）。
/// 泛型工厂返回类型时，成员集合在 X… 里，不在名字上。
fn fnRetExprOf(text: []const u8, name: []const u8) ?[]const u8 {
    const st = findDeclStmt(text, name) orelse return null;
    const lp = std.mem.indexOfScalar(u8, st, '(') orelse return null;
    var d: usize = 0;
    var rp = lp;
    while (rp < st.len) : (rp += 1) {
        if (st[rp] == '(') d += 1;
        if (st[rp] == ')') {
            d -= 1;
            if (d == 0) break;
        }
    }
    if (rp >= st.len) return null;
    const lb = std.mem.indexOfScalarPos(u8, st, rp, '{') orelse return null;
    d = 0;
    var rb = lb;
    while (rb < st.len) : (rb += 1) {
        if (st[rb] == '{') d += 1;
        if (st[rb] == '}') {
            d -= 1;
            if (d == 0) break;
        }
    }
    if (rb >= st.len) return null;
    const body = st[lb + 1 .. rb];
    const rk = std.mem.indexOf(u8, body, "return ") orelse return null;
    const rest = trimSl(body[rk + 7 ..]);
    var e: usize = 0;
    var dep: usize = 0;
    var in_str = false;
    while (e < rest.len) : (e += 1) {
        const ch = rest[e];
        if (in_str) {
            if (ch == '\\') { e += 1; continue; }
            if (ch == '"') in_str = false;
            continue;
        }
        if (ch == '"') { in_str = true; continue; }
        if (ch == '(' or ch == '[' or ch == '{') dep += 1;
        if (ch == ')' or ch == ']' or ch == '}') { if (dep > 0) dep -= 1; }
        if (ch == ';' and dep == 0) break;
    }
    const out = trimSl(rest[0..e]);
    return if (out.len == 0) null else out;
}

/// 接收者是不是**类型值**而不是实例：`const HS: type = HashMap(V: i32);`、
/// `const V = ArrayList(i32);`、`const T = Make(i32)`（Make 返回 type）、
/// 以及它们的别名链。类型值上只有静态成员——判错就会把字段和方法全
/// 摆出来（用户 2026-11-10 截图：`HS.` 弹出 alloc/cap/hashes/…）。
fn isTypeValuedRecv(files: []const []const u8, doc: *const Document,
                    text: []const u8, name: []const u8, depth: u8) bool {
    if (depth > 4 or name.len == 0) return false;
    const st = findDeclStmt(text, name) orelse return false;
    if (declTyOf(st)) |dt| {
        if (std.mem.eql(u8, trimSl(dt), "type")) return true;
    }
    const init = declInitOf(st) orelse return false;
    const e = trimSl(init);
    if (e.len == 0) return false;
    // 裸别名：const A = B;
    var j: usize = 0;
    while (j < e.len and isIdentChar(e[j])) j += 1;
    if (j == e.len) return isTypeValuedRecv(files, doc, text, e, depth + 1);
    // 限定路径别名：`const AB = m.A.B;` —— 也是类型值（此前只认裸名字
    // 别名，多段引用被判成实例，`AB.` 的静态成员与嵌套类型全被过滤掉）。
    if (isPurePath(e)) {
        if (resolveContainerPath(files, doc, text, e) != null) return true;
    }
    const last = finalCalleeOf(e) orelse return false;
    if (isContainerIn(files, last) or genericBodyOf(files, last) != null) return true;
    if (findDeclStmt(text, last)) |fst| {
        const sg = sigOfStmt(fst);
        if (sg.is_fn and std.mem.eql(u8, trimSl(sg.ret), "type")) return true;
    }
    return false;
}

/// 类型值背后的容器：`HS` → HashMap、`T2` → Make 体里 return 的容器。
fn typeValuedContainer(files: []const []const u8, doc: *const Document,
                       text: []const u8, name: []const u8, depth: u8) ?[]const u8 {
    if (depth > 4 or name.len == 0) return null;
    const st = findDeclStmt(text, name) orelse return null;
    const init = declInitOf(st) orelse return null;
    const e = trimSl(init);
    if (e.len == 0) return null;
    var j: usize = 0;
    while (j < e.len and isIdentChar(e[j])) j += 1;
    if (j == e.len) return typeValuedContainer(files, doc, text, e, depth + 1);
    if (isPurePath(e)) {
        if (resolveContainerPath(files, doc, text, e)) |pc| return pc.name;
        // 退路：`const P: type = t.P1.P2;` 里 t 解析不到时按末段名字找 P2，
        // 否则 `const pp = P.P3;` 推不出 `type`、`P.` 也没有成员。
        if (resolveContainerPathLoose(files, doc, text, e)) |pc| return pc.name;
    }
    const last = finalCalleeOf(e) orelse return null;
    if (isContainerIn(files, last) or genericBodyOf(files, last) != null) return last;
    if (fnRetExprOf(text, last)) |ret| return inferContainer(files, text, ret, depth + 1);
    return null;
}

fn completeAt(doc: *const Document, line: u32, character: u32) []const u8 {
    const text = doc.text;
    const off = posToOffset(text, line, character);
    const full_prefix = identBefore(text, off);
    var prefix = full_prefix;
    var is_at_ctx = false;
    if (std.mem.startsWith(u8, prefix, "@")) {
        prefix = prefix[1..];
        is_at_ctx = true;
    } else if (off > 0 and text[off - 1] == '@') {
        // 光标紧跟 '@'（identBefore 不吃 '@'，这里单独识别）
        prefix = "";
        is_at_ctx = true;
    }

    const doc_dir = dirName(doc.path);

    var list = Json{};
    list.raw("{{\"isIncomplete\":false,\"items\":[", .{});
    var seen: std.StringHashMapUnmanaged(void) = .empty;
    defer seen.deinit(gpa);

    // 点补全：部分标识符前一个字符是 '.'。之前只认"光标紧贴点"，
    // 输入 std.p 时走的是关键字分支，std 成员一个都不出——现在
    // 空前缀（贴点）与带前缀（std.p / obj.fi）都走这条。
    const ps = off - full_prefix.len;
    const dotted = ps > 0 and text[ps - 1] == '.';

    if (dotted) {
        // 取 '.' 之前的完整路径（可含多级点：std / std.heap / obj.field）。
        // 结构体/联合字面量 `Person{.`：'.' 前是 '{'，容器名在 '{' 之前
        //（中间隔空白也认：`Person{ .circle`）。已有字段的续补
        //（`Person{.age = 10, .n`）要跨过前面的字段初始化——回扫跟踪
        // 括号深度，顶层遇 '{' 即字面量开头；遇 ';' 说明已出字面量。
        const base_end = ps - 1;
        var base_start = base_end;
        var path_end = base_end; // lit_ctx 时止于 '{'（path=容器名），否则到光标
        var lit_ctx = false;
        {
            // 光标行是否以 '.' 开头的续行（多行字面量：`X{\n .a = 1,\n .n`）。
            // 不是续行时回溯不跨换行——否则上一行残留的未闭合 `a.Cfg{.`
            // 会被当成本行上下文，`std.` 弹出 Cfg 字段（用户 2026-11-08）。
            var line_start = off;
            while (line_start > 0 and text[line_start - 1] != '\n') line_start -= 1;
            var lf = line_start;
            while (lf < text.len and (text[lf] == ' ' or text[lf] == '\t')) lf += 1;
            const cont_line = lf < text.len and text[lf] == '.';
            var q = base_end;
            var depth: usize = 0;
            var in_str = false;
            var guard: usize = 0;
            while (q > 0 and guard < 8192) {
                guard += 1;
                const ch = text[q - 1];
                if (in_str) {
                    if (ch == '"') in_str = false;
                    q -= 1;
                    continue;
                }
                if (ch == '"') {
                    in_str = true;
                    q -= 1;
                    continue;
                }
                if (ch == '}' or ch == ')') {
                    depth += 1;
                    q -= 1;
                    continue;
                }
                if (ch == '{' or ch == '(') {
                    if (depth == 0) {
                        if (ch == '{') {
                            // 校验这个 '{' 是不是字面量开头：前面是容器名
                            //（std.Task 也认，取末段）或 '.{' 匿名。if/fn 体
                            // 的 '{' 前是 ')' 或 "i32" 这类，不是容器 → 不算。
                            var q2 = q - 1;
                            while (q2 > 0 and (text[q2 - 1] == ' ' or text[q2 - 1] == '\t')) q2 -= 1;
                            var qs = q2;
                            while (qs > 0 and (isIdentChar(text[qs - 1]) or text[qs - 1] == '.')) qs -= 1;
                            const name = text[qs..q2]; // '{' 前的容器名（匿名 `.{` 时为 "."）
                            var ok = false;
                            if (name.len > 0 and name[0] == '.') {
                                ok = true; // .{ 匿名字面量
                            } else if (name.len > 0) {
                                var nm = name;
                                if (std.mem.lastIndexOfScalar(u8, nm, '.')) |d| nm = nm[d + 1 ..];
                                var sfv = scanFiles(doc, text);
                                defer sfv.deinit(gpa);
                                ok = isContainerIn(sfv.items, nm);
                                // KEEP: vendor/deps 导入的容器 `vp.Cfg{`——
                                // 本文件/std 目录扫不到，按模块绑定进模块
                                // 文件确认（2026-11-08：vendor struct 也要查）。
                                if (!ok) ok = modContainerOf(doc, text, name) != null;
                            }
                            if (ok) {
                                lit_ctx = true;
                                base_start = qs;
                                path_end = q2;
                            }
                        }
                        break;
                    }
                    depth -= 1;
                    q -= 1;
                    continue;
                }
                if (depth == 0 and ch == ';') break; // 已出字面量
                if (depth == 0 and ch == '\n' and !cont_line) break; // 上一行的字面量与本次无关
                q -= 1;
            }
            if (!lit_ctx) {
                // 普通点路径回扫：'.' 前的标识符链（std / obj.field）。
                // KEEP — 接收者可能是**调用表达式**（用户 2026-11-09 第 8 条：
                // `std.map.HashMap(i32).` 连写三段没提示），用 recvExprStart
                // 把 `(…)` `[…]` 配平吞进来，path 就是完整接收者表达式。
                base_start = recvExprStart(text, base_end);
                // 字符串字面量接收者：`"hello".` —— '"' 不是标识符字符，
                // recvExprStart 立刻停在点上，path 变成空串。往回找配对的
                // 引号，把整条字面量当接收者（用户 2026-11-09：`.ptr`/`.len`
                // 没有提示）。转义 `\"` 跳过。
                if (base_start == base_end and base_end > 0 and text[base_end - 1] == '"') {
                    var lq = base_end - 1;
                    var lok = false;
                    while (lq > 0) {
                        lq -= 1;
                        if (text[lq] == '\\') {
                            if (lq > 0) lq -= 1;
                            continue;
                        }
                        if (text[lq] == '"') { lok = true; break; }
                    }
                    if (lok) base_start = lq;
                }
            }
        }
        const path = text[base_start..path_end];

        var handled = false;
        // 接收者是类型还是值：决定成员的命名空间（见 RecvKind 注释）。
        // 默认 module——std.* / module 绑定走这条，后面各分支按需改写。
        var recv_kind: RecvKind = .module;

        // KEEP —— 可选值先解包（用户 2026-11-10）：`var arena: ?Allocator` 的
        // `arena.` 只该提示 `?`（补成 `arena.?`）。此前落到表达式兜底，把
        // init 的返回类型忽略、按接收者 ArenaAllocator 出 init/reset/deinit，
        // 三条全是编译错误（optional 没有成员，只有 `.?`）。
        if (!lit_ctx) {
            if (recvOptionalTy(text, doc, path)) |inner| {
                if (prefix.len == 0 or std.mem.startsWith(u8, "?", prefix)) {
                    var db: std.ArrayList(u8) = .empty;
                    db.print(gpa, "解包可选值 → {s}", .{inner}) catch {};
                    addItem(&list, &seen, "?", 14, db.items, 0);
                }
                handled = true;
            }
        }

        // std.<ns>.：编译器内建命名空间成员。名字表定成员集，签名/骨架从
        // std_builtin_docs 出（用户 2026-11-09 第 2 条）；输入中途
        // （std.io.op）时最后一段当成员前缀过滤。
        // 例外：`std.heap.page.` 的接收者是**值**（Allocator），不是命名空间——
        // 按命名空间表会被拆成 `std.heap` + 前缀 `page`，只弹回 page 自己。
        // 让它落到下面的表达式推断（inferExprType 特判 std.heap.page）。
        if (!std.mem.eql(u8, path, "std.heap.page") and
            std.mem.startsWith(u8, path, "std.")) {
            var ns = path["std.".len..];
            var mem_prefix: []const u8 = "";
            if (std.mem.lastIndexOfScalar(u8, ns, '.')) |d2| {
                mem_prefix = ns[d2 + 1 ..];
                ns = ns[0..d2];
            }
            for (std_ns_table) |t| {
                if (!std.mem.eql(u8, t.name, ns)) continue;
                handled = true;
                for (t.members) |m| {
                    if (mem_prefix.len > 0 and !std.mem.startsWith(u8, m, mem_prefix)) continue;
                    var fb: std.ArrayList(u8) = .empty;
                    defer fb.deinit(gpa);
                    fb.appendSlice(gpa, "std.") catch {};
                    fb.appendSlice(gpa, ns) catch {};
                    fb.append(gpa, '.') catch {};
                    fb.appendSlice(gpa, m) catch {};
                    if (stdBuiltinDoc(fb.items)) |bd| {
                        if (bd.snip.len > 0) {
                            var ins: std.ArrayList(u8) = .empty;
                            defer ins.deinit(gpa);
                            ins.appendSlice(gpa, m) catch {};
                            ins.appendSlice(gpa, bd.snip) catch {};
                            // 函数：label 就是名字（骨架项独占，裸名项被去重）；
                            // 类型：`Name{...}` 构造骨架，和下面的类型项并存
                            //（用户 2026-11-09 第 12/13 条）。
                            if (bd.snip[0] == '(') {
                                addSnippetRaw(&list, &seen, m, ins.items, bd.sig);
                            } else {
                                var lab: std.ArrayList(u8) = .empty;
                                defer lab.deinit(gpa);
                                lab.appendSlice(gpa, m) catch {};
                                lab.appendSlice(gpa, "{...}") catch {};
                                addSnippetRaw(&list, &seen, lab.items, ins.items, bd.sig);
                            }
                        }
                        addItem(&list, &seen, m,
                            if (bd.ns) 9 else stdBuiltinKind(bd.sig), bd.sig, 0);
                        continue;
                    }
                    addItem(&list, &seen, m, 3, t.detail, 0);
                }
            }
        }

        if (!handled) {
            // std 顶层：编译器 seed 的名字（println/heap/io/…，不在 std.zx 文本里）。
            // 用户 2026-11-08 第 1 条：这 13 个内建混在 std.zx 真实成员前面
            // 就是"std 命名空间污染"——改为真实成员先出（tier 0），内建最后
            // 补（tier 3，detail 只写"编译器内建"，不再带 std 前缀）。
            const std_top = std.mem.eql(u8, path, "std");

            // module 绑定？const <path> = module("…")（等号两侧可有对齐空格）
            const mod: ?[]const u8 = moduleOfBinding(text, path);
            var decls: std.ArrayList(Decl) = .empty;
            defer decls.deinit(gpa);

            // KEEP - 函数实参位置的匿名 struct 字面量：`push(.{ …`、
            // `conf("n", .{ …`、`deep(.{ .opts = .{ …`。回溯到最内层未闭合
            // '(' 找被调函数，按**实参位置**取签名里的 struct{…} 形参
            //（匿名实参在第二位就取第二个形参），再沿 `.name = .{` 嵌套
            // 字段链下钻到光标所在的字面量（3d/3x/3y 守门）。
            var p2 = path;
            while (p2.len > 0 and (p2[p2.len - 1] == ' ' or p2[p2.len - 1] == '\t' or
                p2[p2.len - 1] == '\n' or p2[p2.len - 1] == '\r')) p2 = p2[0 .. p2.len - 1];
            if (lit_ctx and p2.len > 0 and (p2[p2.len - 1] == '{' or p2[p2.len - 1] == '.')) {
                const lit_brace = path_end; // lit_ctx 时它就是目标 '{' 的位置
                // 最内层未闭合 '('：从 '.' 往左配对，跳过内层调用的括号
                var d3: usize = 0;
                var lp2: ?usize = null;
                var w = base_start;
                while (w > 0) {
                    w -= 1;
                    if (text[w] == ')') {
                        d3 += 1;
                    } else if (text[w] == '(') {
                        if (d3 == 0) { lp2 = w; break; }
                        d3 -= 1;
                    }
                }
                if (lp2) |lpx| {
                    var e2 = lpx;
                    while (e2 > 0 and (text[e2 - 1] == ' ' or text[e2 - 1] == '\t' or
                        text[e2 - 1] == '\n' or text[e2 - 1] == '\r')) e2 -= 1;
                    var s2 = e2;
                    while (s2 > 0 and (isIdentChar(text[s2 - 1]) or text[s2 - 1] == '.')) s2 -= 1;
                    const callee0 = text[s2..e2];
                    // `a.deepfn(` → 模块函数：成员名去模块文件里找签名
                    var fname = callee0;
                    var mtxt: ?[]const u8 = null;
                    if (std.mem.lastIndexOfScalar(u8, callee0, '.')) |dot| {
                        fname = callee0[dot + 1 ..];
                        const bind = callee0[0..dot];
                        var mm: ?[]const u8 = moduleOfBinding(text, bind);
                        if (mm == null and std.mem.eql(u8, bind, "std")) mm = "std";
                        if (mm) |m2| {
                            if (resolveModuleFile(doc, m2)) |mp2| mtxt = readFileSmall(mp2);
                        }
                        // 被调不是模块成员而是**类型值路径**上的函数
                        //（`const P: type = vt.P1.P2;` 之后 `P.P3.f3(0, .{ … })`）：
                        // moduleOfBinding 只认模块绑定，这里按容器路径解析，
                        // 容器体在 vendor / deps 里也拿得到 → 匿名 struct 实参
                        // 的字段补全不再落空（用户 2026-11-11 第 1 张截图）。
                        if (mtxt == null and bind.len > 0) {
                            var cf9 = scanFiles(doc, text);
                            defer cf9.deinit(gpa);
                            if (containerOfBinding(cf9.items, doc, text, bind, 0)) |pc9| {
                                if (pc9.src.len > 0 and !std.mem.eql(u8, pc9.src, text)) mtxt = pc9.src;
                            } else if (resolveContainerPathLoose(cf9.items, doc, text, bind)) |pc8| {
                                if (pc8.src.len > 0 and !std.mem.eql(u8, pc8.src, text)) mtxt = pc8.src;
                            }
                        }
                    }
                    // 实参序号：'(' 到目标 '{' 之间顶层逗号数
                    var argi: usize = 0;
                    {
                        var d4: usize = 0;
                        var in_str4 = false;
                        var q4 = lpx + 1;
                        while (q4 < lit_brace) : (q4 += 1) {
                            const c4 = text[q4];
                            if (in_str4) {
                                if (c4 == '\\') { q4 += 1; continue; }
                                if (c4 == '"') in_str4 = false;
                                continue;
                            }
                            if (c4 == '"') { in_str4 = true; continue; }
                            if (c4 == '{' or c4 == '(' or c4 == '[') d4 += 1;
                            if (c4 == '}' or c4 == ')' or c4 == ']') { if (d4 > 0) d4 -= 1; }
                            if (c4 == ',' and d4 == 0) argi += 1;
                        }
                    }
                    if (fname.len > 0) {
                        // 同目录文件也扫：函数可能定义在别的模块里
                        var afiles: std.ArrayList([]const u8) = .empty;
                        defer afiles.deinit(gpa);
                        // 模块函数（`a.deepfn`）的声明在模块文件里，排最前
                        if (mtxt) |mv| afiles.append(gpa, mv) catch {};
                        afiles.append(gpa, text) catch {};
                        var ad = Io.Dir.cwd().openDir(g_io, doc_dir, .{ .iterate = true }) catch null;
                        if (ad) |*dd| {
                            defer dd.close(g_io);
                            var ait = dd.iterate();
                            while (ait.next(g_io) catch null) |entry| {
                                if (entry.kind != .file or !std.mem.endsWith(u8, entry.name, ".zx")) continue;
                                if (std.mem.eql(u8, entry.name, baseName(doc.path))) continue;
                                var fp: std.ArrayList(u8) = .empty;
                                defer fp.deinit(gpa);
                                fp.appendSlice(gpa, doc_dir) catch continue;
                                fp.append(gpa, '/') catch continue;
                                fp.appendSlice(gpa, entry.name) catch continue;
                                if (readFileSmall(fp.items)) |txt| afiles.append(gpa, txt) catch {};
                            }
                        }
                        // 兜底：模块目录（vendor / zx.json deps / 工程根）里
                        // 找同名函数。`vendor/test.zx` 定义的 `f3` 不在文档
                        // 目录里，只扫文档目录永远看不到它的形参类型——
                        // 这与"补全要跟编译器同一条模块搜索链"是同一件事。
                        var pty_opt: ?[]const u8 = anonParamTy(afiles.items, fname, argi);
                        if (pty_opt == null) {
                            var extra: std.ArrayList([]const u8) = .empty;
                            defer extra.deinit(gpa);
                            var mdirs: std.ArrayList([]const u8) = .empty;
                            defer mdirs.deinit(gpa);
                            modDirsInto(doc_dir, &mdirs);
                            var nfiles: usize = 0;
                            for (mdirs.items) |md2| {
                                if (g_std_dir) |sd2| {
                                    if (std.mem.eql(u8, md2, sd2)) continue;
                                }
                                if (std.mem.eql(u8, md2, doc_dir)) continue;
                                var d9 = Io.Dir.cwd().openDir(g_io, md2, .{ .iterate = true }) catch continue;
                                defer d9.close(g_io);
                                var it9 = d9.iterate();
                                while (it9.next(g_io) catch null) |e9| {
                                    if (e9.kind != .file) continue;
                                    if (!std.mem.endsWith(u8, e9.name, ".zx")) continue;
                                    if (nfiles >= 240) break;
                                    nfiles += 1;
                                    var fp9: std.ArrayList(u8) = .empty;
                                    defer fp9.deinit(gpa);
                                    fp9.appendSlice(gpa, md2) catch continue;
                                    fp9.append(gpa, '/') catch continue;
                                    fp9.appendSlice(gpa, e9.name) catch continue;
                                    if (readFileSmall(fp9.items)) |txt9| extra.append(gpa, txt9) catch {};
                                }
                            }
                            if (extra.items.len > 0) pty_opt = anonParamTy(extra.items, fname, argi);
                        }
                        if (pty_opt) |pty0| {
                            var pty = pty0;
                            var okc = true;
                            // 右界 lit_brace + 1：lit_brace 是目标 '{' 前的
                            // '.'（lit 检测跳空白回退所得，必与 '{' 相邻），
                            // 目标 '{' 自己也要入扫——它前面的 `.name = `
                            // 就是链的最后一节。只扫到 lit_brace 会漏掉
                            // 目标名，嵌套链永远为空（anonDeep 假绿根源之二）。
                            var chain = anonFieldChain(text, lpx + 1, lit_brace + 1);
                            defer chain.deinit(gpa);
                            for (chain.items) |nm| {
                                const flds2 = parseAnonTy(pty) orelse { okc = false; break; };
                                var hit2: ?[]const u8 = null;
                                for (flds2.items) |f2| {
                                    if (std.mem.eql(u8, f2.name, nm)) { hit2 = f2.ty; break; }
                                }
                                pty = hit2 orelse { okc = false; break; };
                            }
                            if (okc) {
                                if (parseAnonTy(pty)) |flds| {
                                    handled = true;
                                    for (flds.items) |f| {
                                        addItem(&list, &seen, f.name, 5, f.ty, 0);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            // 模块文件文本：fn 成员的 detail 要拼真实签名（见 emit 循环）
            // 成员声明所在的文本（模块文件 / 模块容器所在文件）；emit 循环
            // 用它取签名、判容器、生成调用/构造骨架。null 表示当前文档。
            var mem_txt: ?[]const u8 = null;
            // 逐段解析得到的容器体：emit 循环查成员声明要锁在这个体里，
            // 否则同名嵌套类型会串到外层同名容器的成员（见 resolveContainerPath）。
            var mem_body_fix: ?[]const u8 = null;
            // 成员所属容器名：方法签名里 `Self` 要换成真实类型名
            // （`next(self: *Self) ?[]const u8` → `next() ?[]const u8`）。
            var owner: []const u8 = "";
            // KEEP — 链式接收者（用户 2026-11-09 第 8 条：`std.map.HashMap(i32).`
            // 连写三段没提示）。path 里带 '(' 说明接收者是**表达式**而不是名字：
            // 泛型实例 `X(T)`、模块函数 `std.time.fromUnixMs(0)`、工厂
            // `std.BufReader.open(…)`，以及它们再串一层 `.init(…)`。名字分支
            //（模块绑定 / 类型路径 / 实例变量）对它全部落空，这里统一交给
            // inferContainer（递归解析器）拿容器类型，成员装载走
            // containerMembersAny（普通容器 + 泛型容器同一入口）。
            if (!handled and std.mem.indexOfScalar(u8, path, '(') != null) {
                var cf = scanFiles(doc, text);
                defer cf.deinit(gpa);
                if (inferContainer(cf.items, text, path, 0)) |ty0| {
                    if (containerMembersAny(cf.items, ty0, &decls)) |ctxt0| {
                        handled = decls.items.len > 0;
                        if (handled) {
                            // `X(T).` 是类型（泛型实例化），`f(…).` / `x.init(…).`
                            // 是值：判据是末位调用的被调名本身是不是容器名。
                            // 混为一谈的话 `std.time.fromUnixMs(0).` 会被当成
                            // 类型，字段全被命名空间过滤掉（year/month/day 消失）。
                            recv_kind = .instance;
                            if (finalCallOpen(path)) |lp3| {
                                const callee3 = trimSl(path[recvExprStart(path, lp3)..lp3]);
                                var last3 = callee3;
                                if (std.mem.lastIndexOfScalar(u8, callee3, '.')) |d3| last3 = callee3[d3 + 1 ..];
                                if (isContainerIn(cf.items, last3) or
                                    genericBodyOf(cf.items, last3) != null) recv_kind = .ty;
                            }
                            mem_txt = ctxt0;
                            markContainerFns(ctxt0, &decls);
                            owner = ty0;
                        }
                    }
                }
                // 解析不出也保持空列表——绝不回落成全局关键字噪音
                handled = true;
            }
            // KEEP — 切片/字符串接收者的编译器成员（用户 2026-11-09 踩中：
            // `"hello".` 与 `var b: []const u8; b.` 都是空列表）。sema 的
            // TY_SLICE 分支只有 .ptr / .len（.0 取首元素，不给补全），
            // 源文件里没有声明文本，成员与类型直接按语义规则出。
            if (!handled) {
                if (sliceRecvTy(text, doc, path, 0)) |si| {
                    handled = true;
                    addSliceMembers(&list, &seen, prefix, si);
                }
            }

            // KEEP: vendor/deps 容器 `vp.Cfg{.` / `vp.Cfg.`——点路径的 path
            // 是带点的限定名，moduleOfBinding 对它不命中（绑定名本身不带点），
            // 上面的模块分支进不去，必须单独按 modContainerOf 解析容器字段。
            // lit_ctx 下 emit 循环把 .value 全过滤，字段就是全部；非字面量
            // 上下文字段单独成组。
            if (!handled) {
                if (modContainerOf(doc, text, path)) |mc0| {
                    if (containerMembersAny(&[_][]const u8{mc0.txt}, mc0.member, &decls) != null) {
                        handled = decls.items.len > 0;
                        if (handled) recv_kind = .ty;
                        mem_txt = mc0.txt;
                        // 容器名要一起带上：方法签名里的 `Self`、返回类型里的
                        // `?Self` 都靠它换成真名（以前漏了这条，`std.BufReader.`
                        // 的 `open` 显示成 `open(…) ?Self`）。
                        owner = mc0.member;
                        // KEEP — 类型路径成员（用户 2026-11-09：std.BufReader.
                        // 的方法全是 kind=6 detail=None 没骨架）：容器体在模块
                        // 文件里，extractBraceMembers 只能看到 `.value`，标回
                        // is_fn 后 emit 循环才拼签名、出调用骨架。
                        markContainerFns(mc0.txt, &decls);
                    }
                }
            }
            if (mod) |m| {
                moduleDecls(doc_dir, m, &decls);
                handled = decls.items.len > 0;
                if (handled) {
                    if (resolveModuleFile(doc, m)) |mp| {
                        mem_txt = readFileSmall(mp);
                    }
                }
            }
            // KEEP — std 门面子模块（用户 2026-11-09：std.str. 一个成员都不
            // 出）：std.zx 里 pub const str = module("str.zx")，moduleOfBinding
            // 只认单段绑定名，二级路径落不到 moduleDecls；沿门面文件解析
            // 一层别名，成员当模块成员出（pub only）。
            if (!handled and std.mem.startsWith(u8, path, "std.")) {
                const seg1 = path["std.".len..];
                if (seg1.len > 0 and std.mem.indexOfScalar(u8, seg1, '.') == null) {
                    if (resolveModuleFile(doc, "std")) |sp| {
                        if (readFileSmall(sp)) |st| {
                            if (moduleAliasIn(st, seg1)) |al| {
                                var ap: std.ArrayList(u8) = .empty;
                                defer ap.deinit(gpa);
                                ap.appendSlice(gpa, dirName(sp)) catch {};
                                ap.append(gpa, '/') catch {};
                                ap.appendSlice(gpa, al) catch {};
                                if (readFileSmall(ap.items)) |at| {
                                    topLevelDecls(at, &decls, true);
                                    handled = decls.items.len > 0;
                                    if (handled) mem_txt = at;
                                }
                            }
                        }
                    }
                }
            }
            if (!handled) {
                // 容器成员：用路径最后一段匹配容器名
                var container = path;
                if (std.mem.lastIndexOfScalar(u8, path, '.')) |dot| container = path[dot + 1 ..];

                var files = scanFiles(doc, text);
                defer files.deinit(gpa);
                // KEEP — 限定路径逐段下钻（用户 2026-11-11："嵌套的 pub 类型
                // 导出不全，只能导出第一层的，第二层丢失类型，第三层根本没
                // 提示"）。以前所有多段路径都退化成"拿末段名字在全文里配容器"：
                //   `A.B.Cfg.` 撞上顶层同名 Cfg → 第三层空列表；
                //   `A.B.Kind.` 变体给了外层 Kind 的 → 第二层类型串味。
                // 现在每一段都只在上一层的容器体里找（findMemberStmt + 取体）。
                if (!handled and std.mem.indexOfScalar(u8, path, '.') != null) {
                    if (resolveContainerPath(files.items, doc, text, path)) |pc| {
                        extractBraceMembers(pc.body, pc.is_enum, &decls);
                        handled = decls.items.len > 0;
                        if (handled) {
                            recv_kind = .ty;
                            mem_txt = pc.src;
                            mem_body_fix = pc.body;
                            markContainerFnsIn(pc.body, pc.src, &decls);
                            owner = pc.name;
                        }
                    }
                }
                // 错误集成员：`error.`（全局码名）或 `Err.`（`const Err = error{…}`）。
                if (!handled) {
                    if (std.mem.eql(u8, container, "error"))
                        handled = collectAllErrMembers(files.items, &decls)
                    else
                        handled = collectNamedErrMembers(files.items, container, &decls);
                }
                // 逐段解析已经填过成员就不要再按末段名字追加一遍——同名
                // 嵌套类型的成员会被外层同名容器污染（`A.B.C.Kind` 曾同时
                // 弹出外层 Kind 的变体）。
                if (!handled) {
                    if (containerMembersAny(files.items, container, &decls)) |ctxt| {
                        handled = decls.items.len > 0;
                        if (handled) {
                            recv_kind = .ty;
                            // KEEP — 类型路径成员（用户 2026-11-09：std.BufReader.
                            // 的方法全是 kind=6 detail=None 没骨架）：容器体可能在
                            // std/模块文件里，带上声明文件文本并标回方法，emit
                            // 循环才能拼签名、出调用骨架。
                            mem_txt = ctxt;
                            markContainerFns(ctxt, &decls);
                            owner = container;
                        }
                    }
                }
                if (!handled and builtinTyMembers(container, &decls)) {
                    // 内建类型（Allocator）没有声明文本：按编译器定义出成员
                    handled = decls.items.len > 0;
                    if (handled) recv_kind = .ty;
                }
                if (!handled) {
                    // 不是模块/类型名——按实例变量处理：推断类型再补容器成员
                    //（let v = Obj.init(...) → v. 出 Obj 的字段与方法）
                    decls.clearRetainingCapacity();
                    // KEEP — 接收者本身可能是**类型值**（用户 2026-11-10 截图）：
                    // `const HS: type = std.map.HashMap(V: i32); HS.` 是"类型上
                    // 只有静态成员"，不是实例——此前按初始化式推出 HashMap 就当
                    // 实例，alloc/cap/hashes/n/vals 和全部方法一起弹出来。
                    const recv_type = isTypeValuedRecv(files.items, doc, text, container, 0);
                    // 绑定的声明/初始化是限定类型路径（`var z: A.B.C`、
                    // `let x = A.B.Cfg{…}`）：逐段解析到那一层容器。以前
                    // inferContainer 把 `A.B.Cfg{…}` 折成末段 "Cfg"，撞上
                    // 顶层同名容器——字段变别人的、第三层直接空列表。

                    if (instancePathContainer(files.items, doc, text, container)) |pc| {
                        extractBraceMembers(pc.body, pc.is_enum, &decls);
                        handled = decls.items.len > 0;
                        if (handled) {
                            recv_kind = if (recv_type) .ty else .instance;
                            mem_txt = pc.src;
                            mem_body_fix = pc.body;
                            markContainerFnsIn(pc.body, pc.src, &decls);
                            owner = pc.name;
                        }
                    }
                    // KEEP: vendor/deps 容器实例（var c = vp.Cfg{…}）——
                    // inferContainer 对绑定名打头的初始化一律 null，先走模块
                    // 解析补齐（2026-11-08：vendor struct 也要查）。
                    if (!handled and vendorInstanceFields(doc, text, container, &decls)) {
                        handled = decls.items.len > 0;
                        if (handled) recv_kind = .instance;
                    }
                    // KEEP — 泛型形参接收者（§117，用户 2026-11-11 第 9 张截图）：
                    // `const fill = fn (T: type, val: T) void { let v = val.a; }`
                    // 的 `val.` —— 定义处不知道 T 是什么，从调用点反推：
                    // `fill(P3, p)` 的 T 位实参就是具体类型，按实例出成员。
                    // 找不到调用点保持空列表，绝不回落全局噪音。
                    if (!handled) {
                        if (genericParamRecvTy(files.items, doc, text, path, off)) |pc| {
                            extractBraceMembers(pc.body, pc.is_enum, &decls);
                            handled = decls.items.len > 0;
                            if (handled) {
                                recv_kind = .instance;
                                mem_txt = pc.src;
                                mem_body_fix = pc.body;
                                markContainerFnsIn(pc.body, pc.src, &decls);
                                owner = pc.name;
                            }
                        }
                    }
                    if (!handled) {
                        if (inferContainer(files.items, text, container, 0) orelse blk: {
                            const s = findDeclStmt(text, container) orelse break :blk null;
                            const init2 = declInitOf(s) orelse break :blk null;
                            break :blk inferContainer(files.items, text, init2, 1);
                        }) |ty| {
                            // KEEP — 实例成员要带声明文件文本（用户 2026-11-09
                            // 第 2/3 条：std.Task/ArenaAllocator 实例的方法
                            // detail 全空、不出骨架）：容器体在 std/模块文件里，
                            // findDeclStmt(当前文档) 落空。containerMembersAny
                            // 直接把声明文件文本带回来（普通容器 + 泛型容器
                            // 两种形态都认），emit 循环据此拼签名、出骨架。
                            if (containerMembersAny(files.items, ty, &decls)) |ctxt| {
                                handled = decls.items.len > 0;
                                if (handled) {
                                    recv_kind = if (recv_type) .ty else .instance;
                                    mem_txt = ctxt;
                                    markContainerFns(ctxt, &decls);
                                    owner = ty;
                                }
                            }
                        }
                    }
                    // 类型值但 inferContainer 认不出容器（泛型工厂：`const T2 =
                    // Make(i32)`，Make 的返回类型是 type，容器在它 return 的
                    // 表达式里）——按类型成员出，绝不退回实例。
                    if (!handled and recv_type) {
                        if (typeValuedContainer(files.items, doc, text, container, 0)) |cty| {
                            if (containerMembersAny(files.items, cty, &decls)) |ctxt| {
                                handled = decls.items.len > 0;
                                if (handled) {
                                    recv_kind = .ty;
                                    mem_txt = ctxt;
                                    markContainerFns(ctxt, &decls);
                                    owner = cty;
                                }
                            }
                        }
                    }
                    // KEEP — 表达式类型兜底（用户 2026-11-09 第 16 条）：
                    // `var b = buf.?`、`let slice = args[0][0]`、`const s = strs.0`
                    // 的初始化式 inferContainer 认不出（它只按名字/调用找容器），
                    // 但 inferExprType 认得——推断出来再当容器实例；结果是切片 /
                    // 定长数组时按编译器成员出 len/ptr（源文件里没有声明文本）。
                    if (!handled) {
                        // 接收者有两条候选：`container`（路径末段，普通 `obj.`）
                        // 与 `path`（整条接收者表达式，`arrs.0.` / `args[0].`）。
                        // 末段是 `.0` 这种位置访问时它根本不可能是变量名，
                        // 链式接收者全靠第二条（用户 2026-11-09 第 17 条：
                        // `arrs.0.` 一个成员都不出）。
                        var tried: []const u8 = "";
                        for ([_][]const u8{ path, container }) |recv| {
                            if (handled or recv.len == 0) continue;
                            if (tried.len > 0 and std.mem.eql(u8, tried, recv)) continue;
                            tried = recv;
                            var ity: ?[]const u8 = null;
                            // 能逐段解析的限定类型路径（`A.B.C` / `A.B.Cfg{…}`）：
                            // inferExprType 只剩末段名，同名嵌套类型会解析到外层
                            // 同名容器（用户 2026-11-11 第三层空列表的根因）。
                            var tyexpr: ?[]const u8 = null;
                            if (findDeclStmt(text, recv)) |st| {
                                if (declTyOf(st)) |dt| {
                                    ity = trimSl(dt);
                                    tyexpr = trimSl(dt);
                                } else if (declInitOf(st)) |ini| {
                                    ity = inferExprType(text, doc, ini, 0);
                                    if (literalPathOf(ini)) |lp| tyexpr = lp;
                                }
                            } else {
                                ity = inferExprType(text, doc, recv, 0);
                            }
                            if (tyexpr) |te| {
                                if (resolveContainerPath(files.items, doc, text, te)) |pc| {
                                    extractBraceMembers(pc.body, pc.is_enum, &decls);
                                    handled = decls.items.len > 0;
                                    if (handled) {
                                        recv_kind = if (recv_type) .ty else .instance;
                                        mem_txt = pc.src;
                                        mem_body_fix = pc.body;
                                        markContainerFnsIn(pc.body, pc.src, &decls);
                                        owner = pc.name;
                                    }
                                }
                            }
                            const raw = ity orelse continue;
                            const ty = containerNameOf(raw);
                            if (!handled and ty.len > 0) {
                                if (containerMembersAny(files.items, ty, &decls)) |ctxt| {
                                    handled = decls.items.len > 0;
                                    if (handled) {
                                        recv_kind = if (recv_type) .ty else .instance;
                                        mem_txt = ctxt;
                                        markContainerFns(ctxt, &decls);
                                        owner = ty;
                                    }
                                }
                            }
                            if (!handled and builtinTyMembers(ty, &decls)) {
                                // 内建类型（解包后的 Allocator 等）
                                handled = decls.items.len > 0;
                                if (handled) recv_kind = .instance;
                            }
                            if (!handled) {
                                if (sliceOrArrayRecv(raw)) |si| {
                                    handled = true;
                                    addSliceMembers(&list, &seen, prefix, si);
                                }
                            }
                        }
                    }
                    // KEEP - 匿名类型线（3v/3w/3x）：匿名绑定 `anon.`、嵌套
                    // `nv.pos.`、匿名形参 `info.` 的成员补全。命名容器路径全部
                    // 落空才到这里（`o.count` 的 Obj 实例等不受影响）。
                    if (!handled) {
                        if (anonTyOfPath(text, doc, path, off)) |aty| {
                            if (parseAnonTy(aty)) |flds| {
                                handled = flds.items.len > 0;
                                if (handled) recv_kind = .instance;
                                for (flds.items) |f| {
                                    decls.append(gpa, .{ .name = f.name, .kind = .field, .ty = f.ty }) catch {};
                                }
                            }
                        }
                    }
                    // KEEP — 模块容器实例的嵌套（用户 2026-11-08 第 3 条）：
                    // `let v = a.Outer{…}; v.inner.` 取 inner 的声明类型（含内联
                    // struct），`v.inner.a` 供 hover 用。前面解析不到才兜到这里。
                    if (!handled) {
                        var mitxt: ?[]const u8 = null;
                        if (modInstanceTyOfPath(doc, text, path, &mitxt)) |mty| {
                            if (parseAnonTy(mty)) |flds| {
                                handled = flds.items.len > 0;
                                for (flds.items) |f| {
                                    decls.append(gpa, .{ .name = f.name, .kind = .field, .ty = f.ty }) catch {};
                                }
                            } else if (mitxt) |mt| {
                                var mf: std.ArrayList(Decl) = .empty;
                                defer mf.deinit(gpa);
                                if (containerMembersAny(&[_][]const u8{mt}, mty, &mf) != null) {
                                    handled = mf.items.len > 0;
                                    if (handled) recv_kind = .instance;
                                    for (mf.items) |d2| decls.append(gpa, d2) catch {};
                                    mem_txt = mt;
                                    owner = mty;
                                    markContainerFns(mt, &decls);
                                }
                            }
                        }
                    }
                }
            }
            if (handled) {
                const src: []const u8 = mem_txt orelse text;
                // 接收者容器的体（顶层成员表）：成员声明的查找范围只在里面
                //（`t.spawn` 不能拿到 Group.spawn 的签名）。逐段路径解析过就
                //用它带回来的体——按 owner 名字再找一次会撞上外层同名容器。
                const mem_body: ?[]const u8 = mem_body_fix orelse if (owner.len > 0) blk2: {
                    if (findContainerBody(&[_][]const u8{src}, owner)) |h| break :blk2 h.body;
                    break :blk2 genericBodyOf(&[_][]const u8{src}, owner);
                } else null;
                for (decls.items) |dcl| {
                    // 字面量初始化只能写字段/变体，方法（name = fn …）不出
                    if (lit_ctx and dcl.kind == .value) continue;
                    // 已输入前缀过滤：`a.tw` 只该出 twice（此前把整份成员
                    // 列表全弹出来，用户 2026-11-08 第 3/4/6 条）
                    if (prefix.len > 0 and !std.mem.startsWith(u8, dcl.name, prefix)) continue;
                    // 声明语句（模块文件/当前文档）：签名、容器判定、骨架共用
                    // 成员声明的查找范围必须锁在接收者自己的容器体里
                    //（同名成员跨容器时全文查找会串签名，见 findMemberStmt）
                    const stmt: ?[]const u8 = blk2: {
                        if (mem_body) |b| {
                            if (findMemberStmt(b, dcl.name)) |st| break :blk2 st;
                        }
                        break :blk2 findDeclStmt(src, dcl.name);
                    };
                    // 成员来自别的文件（std/模块）时，非 `pub` 的一律不出：
                    // 编译器那边是 E0404，补全不能比它宽松。
                    // 变体（enum / union(enum)）不是可隐藏的成员：它们是值，
                    // 类型能写出来就说明变体集合是公开的，不要求逐个写 pub。
                    if (dcl.kind != .variant) {
                        if (mem_txt) |mt| {
                            if (!std.mem.eql(u8, mt, text)) {
                                if (mem_body) |b| {
                                    if (!memberPubIn(b, dcl.name)) continue;
                                }
                            }
                        }
                    }
                    const cinit: ?[]const u8 = if (stmt) |st| containerInitOf(st) else null;
                    // KEEP —— 类型 / 实例命名空间分离（用户 2026-11-10：
                    // "Task 命名空间污染了"）。补全不能比编译器宽松：
                    //   `Task.id`   E0300 实例字段；类型上只有静态成员
                    //   `t.coro()`  E0303 首参不是 self，不是方法
                    //   `t.Group`   E0304 实例上没有嵌套类型
                    // 类型接收者不出字段，值接收者不出静态函数与嵌套类型。
                    // 字面量上下文（`Person{.`）不过滤：那里只能是字段。
                    if (!lit_ctx) {
                        var has_self = false;
                        var is_fn_decl = false;
                        if (stmt) |st| {
                            const sg = sigOfStmt(st);
                            if (sg.is_fn) {
                                is_fn_decl = true;
                                has_self = sg.params.len > 0 and
                                    std.mem.eql(u8, paramName(sg.params[0]), "self");
                            }
                        }
                        switch (recv_kind) {
                            .instance => {
                                if (cinit != null) continue;            // t.Group
                                if (is_fn_decl and !has_self) continue; // t.coro()
                            },
                            .ty => {
                                if (dcl.kind == .field) continue;       // Task.id
                                // 方法（首参 self）只属于实例：类型上列出来
                                // 就是"Task. 里也有一堆 await/send/recv"，
                                // 用户 2026-11-10 截图第 2 张的污染。类型上
                                // 只留关联函数（coro/single/pool/fromId/group）、
                                // 嵌套类型与关联常量。未绑定调用
                                //（`Task.spawn(&t, f, 0)`）虽然合法，但不是
                                // 这里该推荐的东西——实例上写 `t.spawn` 更短。
                                if (is_fn_decl and has_self) continue;
                            },
                            .module => {},
                        }
                    }
                    // 类型项用类型 kind（struct/union=22、enum=13），弹窗里
                    // 一眼能看出这是类型而不是变量。
                    const k: u32 = if (dcl.is_fn) 3 else switch (dcl.kind) {
                        .field => 5,
                        .variant => 20,
                        .value => if (cinit) |ci| containerKindOf(ci) else 6,
                    };
                    // KEEP — 点补全里的 snippet（用户 2026-11-08 第 4/6 条）：
                    // 模块函数 → `twice(...)`；模块 struct → `Cfg{...}`。
                    // 骨架只**追加**、不吞掉本条：用户 2026-11-09 第 12 条——
                    // "只提示类型"和"提示 snippet"两条都要，否则弹窗里全是
                    // snippet，想只补名字（把函数当值传）时没有选项。
                    if (!lit_ctx and stmt != null) {
                        if (dcl.is_fn) {
                            // KEEP — 实例方法也要骨架（用户 2026-11-09 第 7 条：
                            // std.BufReader/BufWriter 实例的方法没有提示和
                            // snippet）。插入形态是 `next()$0` / `write(${1:data})$0`
                            //（self 是隐含接收者，不进骨架），但**签名 detail 要
                            // 带上 self**：实例上只该有方法，detail 里看不见
                            // self 就会被当成关联函数（用户 2026-11-10 两次截图）。
                            _ = memberCallSnippet(&list, &seen, stmt.?, dcl.name, owner,
                                                  recv_kind == .instance);
                        } else if (cinit != null) {
                            memberCtorSnippet(&list, &seen, src, dcl.name);
                        }
                    }
                    // detail 四态：字段/带注解常量 → 声明类型；函数 → 完整签名
                    // name(params) ret；模块绑定 → module("…")；容器 → struct/
                    // union/enum；别名 → 初始化表达式。绝不落到绑定别名
                    //（`a.Cfg` 曾整列显示 `a`，用户看不出是类型——第 3 条）。
                    const detail: []const u8 = blk: {
                        if (dcl.kind == .field and dcl.ty.len > 0) break :blk dcl.ty;
                        if (dcl.kind == .value and dcl.ty.len > 0) break :blk dcl.ty;
                        if (dcl.is_fn) {
                            if (stmt) |st| {
                                const sig = sigOfStmt(st);
                                if (sig.is_fn) {
                                    var sb: std.ArrayList(u8) = .empty;
                                    appendMemberSig(&sb, dcl.name, sig, owner,
                                                    recv_kind == .instance);
                                    break :blk sb.items;
                                }
                            }
                            break :blk "函数";
                        }
                        if (stmt) |st| {
                            if (containerInitOf(st) != null) {
                                // 类型项 detail 给类型提示（`BufReader: type`），
                                // 不再是 "struct"/"构造实例 struct" 这种看不出
                                // 是哪个类型的字眼（用户 2026-11-09）。
                                var db: std.ArrayList(u8) = .empty;
                                db.print(gpa, "{s}: type", .{dcl.name}) catch {};
                                break :blk db.items;
                            }
                            if (declInitOf(st)) |init0| {
                                const t = trimSl(init0);
                                if (t.len > 0 and t.len <= 64) break :blk t;
                            }
                        }
                        break :blk "";
                    };
                    addItem(&list, &seen, dcl.name, k, detail, 0);
                }
            } else if (!std.mem.eql(u8, path, "std")) {
                // 未知容器：不给工作区噪音，宁可空列表
            }
            if (std_top) {
                // KEEP — 用户 2026-11-09 第 4 条：Allocator/stackAlloc/heapAlloc
                // 要暴露出来，带签名 detail + snippet（Allocator 是类型 → struct
                // 字面量骨架；两个 Alloc 是函数 → 调用骨架）。Type/Field
                // 纯编译期反射，仍由 isCtBuiltin 挡住（第 1 条去污染）。
                // 第 2 条：print/println/format 从对齐表出签名 + 调用骨架；
                // debug/atomic 两个命名空间也由编译器 seed（g_std_seeded），列出。
                for (std_seeded) |m| {
                    if (isCtBuiltin(m)) continue;
                    if (prefix.len > 0 and !std.mem.startsWith(u8, m, prefix)) continue;
                    var fb: std.ArrayList(u8) = .empty;
                    defer fb.deinit(gpa);
                    fb.appendSlice(gpa, "std.") catch {};
                    fb.appendSlice(gpa, m) catch {};
                    if (stdBuiltinDoc(fb.items)) |bd| {
                        if (bd.snip.len > 0) {
                            var ins: std.ArrayList(u8) = .empty;
                            defer ins.deinit(gpa);
                            ins.appendSlice(gpa, m) catch {};
                            ins.appendSlice(gpa, bd.snip) catch {};
                            // 函数：label 就是名字（骨架独占）；类型：
                            // `Name{...}` 与类型项并存（第 12/13 条）。
                            if (bd.snip[0] == '(') {
                                addSnippetRaw(&list, &seen, m, ins.items, bd.sig);
                            } else {
                                var lab: std.ArrayList(u8) = .empty;
                                defer lab.deinit(gpa);
                                lab.appendSlice(gpa, m) catch {};
                                lab.appendSlice(gpa, "{...}") catch {};
                                addSnippetRaw(&list, &seen, lab.items, ins.items, bd.sig);
                            }
                        }
                        // 类型条目（`Allocator: type — …`、`ArrayList(T: type) type`）
                        // 按类型出图标，别和函数混成一片。
                        addItem(&list, &seen, m,
                            if (bd.ns) 9 else stdBuiltinKind(bd.sig), bd.sig, 3);
                        continue;
                    }
                    addItem(&list, &seen, m, 3, "编译器内建", 3);
                }
                for ([_][]const u8{ "debug", "atomic" }) |m| {
                    if (prefix.len > 0 and !std.mem.startsWith(u8, m, prefix)) continue;
                    const det: []const u8 = if (std.mem.eql(u8, m, "debug"))
                        "debug — 命名空间（不能直接调用）：panic/assert/errName/printTrace"
                    else
                        "atomic — 命名空间（不能直接调用）：atomicAdd/atomicLoad/atomicCAS/…";
                    addItem(&list, &seen, m, 9, det, 3);
                }
            }
        }
    } else if (is_at_ctx) {
        // @ 前缀：顶层内建
        for (toplevel_builtins) |b| {
            if (std.mem.startsWith(u8, b, prefix)) {
                var detail: []const u8 = "内建函数";
                for (builtin_docs) |bd| {
                    if (std.mem.eql(u8, bd.name, b)) detail = bd.detail;
                }
                addItem(&list, &seen, b, 3, detail, 0);
            }
        }
    } else {
        // 额外模板先占位：main / println / test 常与工作区标识符同名，
        // 放到 addIdents 之后会被 seen 去重挡掉。空前缀（Ctrl+Space）时
        // 不列，避免抢走同名标识符的补全。
        for (snippets) |sn| {
            if (isKeywordWord(sn.label)) continue;
            if (prefix.len > 0 and std.mem.startsWith(u8, sn.label, prefix))
                addSnippet(&list, &seen, sn);
        }
        // 当前文件标识符 + 关键字 + 工作区标识符（分层排序）
        // 模块导入项（不创造隐式名字，只插入绑定语句）
        addImportModules(&list, &seen, text, off, doc_dir, prefix);
        // KEEP - 自定义类型实例 / 函数调用骨架（当前文件声明）。
        // 必须在 addIdents 之前：容器拿到 `Name{...}` 构造骨架后会把裸名
        // 占进 seen，裸标识符项就不再重复弹（用户 2026-11-08：只要骨架那条）。
        addExprSnippets(&list, &seen, doc, text, prefix);
        addIdents(&list, &seen, text, prefix);
        for (keywords) |kw| {
            if (prefix.len == 0 or std.mem.startsWith(u8, kw, prefix)) {
                // 关键字里有骨架的走 snippet（写 `if` + Tab 展开），
                // 其余仍是普通关键字补全。
                if (snippetFor(kw)) |sn| {
                    addSnippet(&list, &seen, sn);
                } else {
                    addItem(&list, &seen, kw, 14, "关键字", 2);
                }
            }
        }
    }

    list.raw("]}}", .{});
    return list.buf.items;
}

// ---------------------------------------------------------------- diagnostics

/// 影子文件标成 隐藏+临时：诊断走"同目录影子文件 → zxc --check → 删除"，
/// 不标的话用户每敲一次键就看到 src/ 里有个 .zxls-*.zx 闪一下（文件
/// 监视器也跟着刷）。隐藏属性不挡 DeleteFile，删除逻辑不用动。
fn hideFile(sub_path: []const u8) void {
    const w = std.unicode.utf8ToUtf16LeAllocZ(gpa, sub_path) catch return;
    defer gpa.free(w);
    const FILE_ATTRIBUTE_HIDDEN: u32 = 0x2;
    const FILE_ATTRIBUTE_TEMPORARY: u32 = 0x100;
    _ = SetFileAttributesW(w.ptr, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY);
}
extern "kernel32" fn SetFileAttributesW(lpFileName: [*:0]const u16, dwFileAttributes: u32) i32;

const Diag = struct {
    line: u32,
    col: u32,
    code: []const u8,
    msg: []const u8,
};

/// zxc 子进程的工作目录：固定为工程根。编译器把 zx.json 的 deps 与 vendor/
/// 当**相对 cwd** 的路径，宿主（Zed/VS Code）给的 cwd 不保证是工程根——
/// 一旦不是，诊断里就会冒出"模块找不到"，而补全（走绝对路径）却正常。
/// 两边口径统一到工程根。
fn childCwd() std.process.Child.Cwd {
    if (root_path) |r| {
        if (r.len > 0 and isDirPath(r)) return .{ .path = r };
    }
    return .inherit;
}

/// 格式化：缓冲写影子文件 → `zxc fmt` 原地格式化 → 读回 → 全文 TextEdit。
/// 复用诊断的影子文件机制（同目录保证 module(...) 解析路径一致）；影子名
/// 用 .zxls-f 前缀与诊断的 .zxls- 错开，同一文档并发时互不踩。
fn formatDoc(doc: *const Document) ?[]const u8 {
    const dir = dirName(doc.path);
    const h = std.hash.Wyhash.hash(0, doc.path);
    var shadow: std.ArrayList(u8) = .empty;
    defer shadow.deinit(gpa);
    shadow.appendSlice(gpa, dir) catch return null;
    shadow.appendSlice(gpa, "/.zxls-f") catch return null;
    shadow.print(gpa, "{x}", .{h}) catch return null;
    shadow.appendSlice(gpa, ".zx") catch return null;
    Io.Dir.cwd().writeFile(g_io, .{ .sub_path = shadow.items, .data = doc.text }) catch return null;
    hideFile(shadow.items);
    defer Io.Dir.cwd().deleteFile(g_io, shadow.items) catch {};

    // fmt 是子命令：`zxc fmt <file>`。以前拼成 `zxc <file> fmt`，"fmt" 被
    // 当成第二个文件名忽略，实际跑的是编译——影子文件永远没被格式化，
    // 读回来原样，宿主拿到空编辑，保存自动格式化于是静默失效。
    var cmdline: std.ArrayList(u8) = .empty;
    defer cmdline.deinit(gpa);
    cmdline.appendSlice(gpa, zxc_path) catch return null;
    cmdline.appendSlice(gpa, " fmt ") catch return null;
    cmdline.appendSlice(gpa, shadow.items) catch return null;
    const argv = [_][]const u8{ "C:\\Windows\\System32\\cmd.exe", "/c", cmdline.items };
    _ = std.process.run(gpa, g_io, .{ .argv = &argv, .stderr_limit = .unlimited, .cwd = childCwd() }) catch return null;

    const new_text = readFileSmall(shadow.items) orelse return null;
    if (std.mem.eql(u8, new_text, doc.text)) return "[]"; // 没变化，空编辑
    // 终点 = 最后一行的行号 + 该行长度。不能假设文档以 \n 结尾：
    // "a\nb" 的全文替换终点是 (1, len("b"))，若用（换行数, 0）会把
    // 最后一行漏在替换区外，格式化结果拼接出错。
    var end_line: u32 = 0;
    var end_char: u32 = 0;
    for (doc.text) |c| {
        if (c == '\n') {
            end_line += 1;
            end_char = 0;
        } else end_char += 1;
    }
    var j = Json{};
    j.buf.appendSlice(gpa, "[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":") catch return null;
    j.raw("{d}", .{end_line});
    j.buf.appendSlice(gpa, ",\"character\":") catch return null;
    j.raw("{d}", .{end_char});
    // 两个 } ：一个闭 "end"，一个闭 "range"。少一个 newText 就会被
    // 塞进 range 对象里，宿主直接报 JSON 解析错（以前 fmt 没生效、
    // 永远走"无变化"分支，这个坑被遮到今天）。
    j.buf.appendSlice(gpa, "}},\"newText\":") catch return null;
    j.str(new_text);
    j.buf.appendSlice(gpa, "}]") catch return null;
    return j.buf.items;
}

fn runCheck(doc: *const Document, out_diags: *std.ArrayList(Diag)) void {    // 影子文件：把缓冲内容写到与源文件同目录的隐藏 .zx，zxc 检查它——
    // 诊断反映的是编辑器缓冲，而不是磁盘旧状态；同目录保证 module(...)
    // 的解析路径与真实文件一致。
    const dir = dirName(doc.path);
    const h = std.hash.Wyhash.hash(0, doc.path);
    var shadow: std.ArrayList(u8) = .empty;
    defer shadow.deinit(gpa);
    shadow.appendSlice(gpa, dir) catch return;
    shadow.appendSlice(gpa, "/.zxls-") catch return;
    shadow.print(gpa, "{x}", .{h}) catch return;
    shadow.appendSlice(gpa, ".zx") catch return;
    Io.Dir.cwd().writeFile(g_io, .{ .sub_path = shadow.items, .data = doc.text }) catch return;
    hideFile(shadow.items);
    defer Io.Dir.cwd().deleteFile(g_io, shadow.items) catch {};
    // 经 cmd /c 包装：zig 直接拉起 zxc 时，zxfe（zxc 的孙进程）的 stderr
    // 句柄继承会断链——诊断全部丢失，连好文件都会静默返回 1；
    // cmd 中转后句柄继承恢复正常（见 README M39 环境）。
    // 注意：命令串不能再加内层引号——zig 会把整个串作为单个参数包一层
    // 引号，cmd /c 规则只剥这一对外层引号；内层引号反而会被切碎
    //（Windows 引号陷阱的又一变种，与 zxc system() 的教训同源）。
    var cmdline: std.ArrayList(u8) = .empty;
    defer cmdline.deinit(gpa);
    cmdline.appendSlice(gpa, zxc_path) catch return;
    cmdline.append(gpa, ' ') catch return;
    cmdline.appendSlice(gpa, shadow.items) catch return;
    cmdline.appendSlice(gpa, " --check") catch return;
    const argv = [_][]const u8{ "C:\\Windows\\System32\\cmd.exe", "/c", cmdline.items };
    const res = std.process.run(gpa, g_io, .{
        .argv = &argv,
        .stderr_limit = .unlimited,
        .cwd = childCwd(),
    }) catch return;
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);

    var it = std.mem.splitScalar(u8, res.stderr, '\n');
    while (it.next()) |line_raw| {
        const line = std.mem.trimEnd(u8, line_raw, "\r");
        // D2/D1：渲染器跟在诊断后面的 help:/note: 行（含建议与相关位置）
        // 挂进上一条诊断的消息——编辑器里一眼看到 did you mean。
        const trimmed = std.mem.trimStart(u8, line, " ");
        if (std.mem.startsWith(u8, trimmed, "help: ") or std.mem.startsWith(u8, trimmed, "note: ")) {
            if (out_diags.items.len > 0) {
                const last = &out_diags.items[out_diags.items.len - 1];
                if (std.mem.concat(gpa, u8, &.{ last.msg, "\n", trimmed })) |joined| {
                    gpa.free(last.msg);
                    last.msg = joined;
                } else |_| {}
            }
            continue;
        }
        const marker = std.mem.indexOf(u8, line, ": error[") orelse continue;
        // 行号/列号解析失败不能整条丢弃：E0101 这类词法错误是
        // "file: error[E0101]: lexical error(s)"，没有 line:col 段——
        // 以前这里 continue，编辑器里一条波浪线都不出。
        var seg = line[0..marker];
        var ln: u32 = 0;
        var col: u32 = 0;
        if (std.mem.lastIndexOfScalar(u8, seg, ':')) |c1| {
            if (std.fmt.parseInt(u32, seg[c1 + 1 ..], 10)) |v| {
                col = v;
                const seg2 = seg[0..c1];
                if (std.mem.lastIndexOfScalar(u8, seg2, ':')) |c2| {
                    if (std.fmt.parseInt(u32, seg2[c2 + 1 ..], 10)) |v2| {
                        ln = v2;
                    } else |_| {}
                }
            } else |_| {}
        }
        // 只收本文件（影子文件）的诊断。zxc 会把子模块的错误（比如
        // vendor/glfw.zx:751 那条坏字面量）也按同一格式打到 stderr，而
        // Diag 里没有"哪个文件"的字段——以前整条按行号糊到当前文件上，
        // 于是 9 行的文件在第 751 行画波浪线。子模块的问题由导入点那条
        // "failed to parse" 讲清楚（消息里带首条错误的行列）。
        var fpath = seg;
        if (std.mem.lastIndexOfScalar(u8, fpath, ':')) |c1| {
            if (std.fmt.parseInt(u32, fpath[c1 + 1 ..], 10)) |_| {
                fpath = fpath[0..c1];
                if (std.mem.lastIndexOfScalar(u8, fpath, ':')) |c2| {
                    if (std.fmt.parseInt(u32, fpath[c2 + 1 ..], 10)) |_| {
                        fpath = fpath[0..c2];
                    } else |_| {}
                }
            } else |_| {}
        }
        if (!std.mem.eql(u8, fpath, shadow.items)) continue;
        const after = line[marker..];
        const lb = std.mem.indexOfScalar(u8, after, '[') orelse continue;
        const rb = std.mem.indexOfScalar(u8, after, ']') orelse continue;
        var msg = after[rb + 1 ..];
        if (msg.len > 0 and msg[0] == ':') msg = msg[1..];
        while (msg.len > 0 and msg[0] == ' ') msg = msg[1..];
        out_diags.append(gpa, .{
            .line = if (ln > 0) ln - 1 else 0,
            .col = if (col > 0) col - 1 else 0,
            .code = dupe(after[lb + 1 .. rb]),
            .msg = dupe(msg),
        }) catch {};
    }
}

// 诊断 range 的两件事：宽度与编码。
//
// 编译器的列号是 **1-based 字节偏移**，而 LSP 的 character 数的是
// **UTF-16 码元**——含中文的行必须换算，否则指位整体后移。
// 另外编译器只给一个起点、不给长度：range 若只有 1 个字符，
// 编辑器里点波浪线只能命中那个字节（成员名有 5 个字母，
// 点第 2 个字母就没反应）。这里按源码把 range 扩到整个 token。
fn lineSpan(text: []const u8, line0: u32) []const u8 {
    var ln: u32 = 0;
    var start: usize = 0;
    var i: usize = 0;
    while (i <= text.len) : (i += 1) {
        if (i == text.len or text[i] == '\n') {
            if (ln == line0) {
                var end = i;
                if (end > start and text[end - 1] == '\r') end -= 1;
                return text[start..end];
            }
            ln += 1;
            start = i + 1;
        }
    }
    return "";
}

fn utf16Len(s: []const u8) u32 {
    var n: u32 = 0;
    var i: usize = 0;
    while (i < s.len) {
        const c = s[i];
        if (c < 0x80) {
            n += 1;
            i += 1;
        } else if (c & 0xE0 == 0xC0) {
            n += 1;
            i += 2;
        } else if (c & 0xF0 == 0xE0) {
            n += 1;
            i += 3;
        } else if (c & 0xF8 == 0xF0) {
            n += 2; // 星光面字符在 UTF-16 里是代理对，两个码元
            i += 4;
        } else {
            n += 1;
            i += 1;
        }
    }
    return n;
}

fn isIdentByte(c: u8) bool {
    return (c >= 'a' and c <= 'z') or (c >= 'A' and c <= 'Z') or
        (c >= '0' and c <= '9') or c == '_' or c >= 0x80;
}

const Range = struct { s: u32, e: u32 };

fn diagRange(text: []const u8, line0: u32, bytecol: u32) Range {
    const ln = lineSpan(text, line0);
    var c: usize = bytecol;
    if (c > ln.len) c = ln.len;
    const s = utf16Len(ln[0..c]);
    var end = c;
    if (end < ln.len and isIdentByte(ln[end])) {
        while (end < ln.len and isIdentByte(ln[end])) end += 1;
    } else if (end < ln.len) {
        end += 1; // 非标识符（运算符、括号…）就只指这一个字符
        while (end < ln.len and (ln[end] & 0xC0) == 0x80) end += 1;
    }
    var e = utf16Len(ln[0..end]);
    if (e <= s) e = s + 1; // LSP 要求 end > start，零宽诊断编辑器不画波浪线
    return .{ .s = s, .e = e };
}

fn publishDiags(doc: *const Document, diags: []const Diag) void {
    var j = Json{};
    // 所有含字面花括号的片段用 appendSlice 写入，绕开 fmt 的 {{ }} 转义；
    // {d} 只出现在不含花括号的最小片段里。
    j.buf.appendSlice(gpa, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":") catch @panic("oom");
    j.str(doc.uri);
    j.raw(",\"version\":{d},\"diagnostics\":[", .{doc.version});
    for (diags, 0..) |d, i| {
        if (i > 0) j.buf.append(gpa, ',') catch @panic("oom");
        // {"range":{"start":{"line":L,"character":C},"end":{"line":L,"character":C2}},"severity":...
        const rg = diagRange(doc.text, d.line, d.col);
        j.buf.appendSlice(gpa, "{\"range\":{\"start\":{\"line\":") catch @panic("oom");
        j.raw("{d},\"character\":{d}", .{ d.line, rg.s });
        j.buf.appendSlice(gpa, "},\"end\":{\"line\":") catch @panic("oom");
        j.raw("{d},\"character\":{d}", .{ d.line, rg.e });
        j.buf.appendSlice(gpa, "}},\"severity\":1,\"code\":") catch @panic("oom");
        j.str(d.code);
        // codeDescription：编辑器把诊断上那个码变成可点的链接，点开就是
        // docs/errors.md 里对应的一条（锚点 = 小写码）。
        // 只在客户端声明支持时才发——不支持的老客户端看到未知字段会
        // 严格校验失败，整条 publishDiagnostics 就被丢弃，诊断全没了。
        if (cap_code_desc and d.code.len == 5) {
            j.buf.appendSlice(gpa, ",\"codeDescription\":{\"href\":\"") catch @panic("oom");
            j.buf.appendSlice(gpa, docs_base) catch @panic("oom");
            j.buf.append(gpa, '#') catch @panic("oom");
            for (d.code) |ch| {
                const lo: u8 = if (ch >= 'A' and ch <= 'Z') ch + 32 else ch;
                j.buf.append(gpa, lo) catch @panic("oom");
            }
            j.buf.appendSlice(gpa, "\"}") catch @panic("oom");
        }
        j.buf.appendSlice(gpa, ",\"source\":\"zxc\",\"message\":") catch @panic("oom");
        j.str(d.msg);
        j.buf.appendSlice(gpa, "}") catch @panic("oom");
    }
    j.buf.appendSlice(gpa, "]}}") catch @panic("oom");
    logLine("publish");
    sendRaw(j.buf.items);
}

fn diagnosticsWorker() void {
    while (true) {
        docs_mu.lockUncancelable(g_io);
        while (dirty.count() == 0 and !quitting)
            docs_cond.waitTimeout(g_io, &docs_mu, .{ .duration = .{ .raw = Io.Duration.fromMilliseconds(250), .clock = .awake } }) catch {};
        if (quitting) {
            docs_mu.unlock(g_io);
            return;
        }
        var batch: std.ArrayList(Document) = .empty;
        defer batch.deinit(gpa);
        var it = dirty.keyIterator();
        while (it.next()) |k| {
            // KEEP：拷走内容，不拷指针。下面 unlock 之后主线程随时可能
            // didChange 把 doc.text / doc.path 释放掉，浅拷贝就是读已释放
            // 内存——诊断线程这时候正在解析的就是那份文本。
            if (docs.get(k.*)) |doc| {
                const p = gpa.dupe(u8, doc.path) catch continue;
                const u = gpa.dupe(u8, doc.uri) catch {
                    gpa.free(p);
                    continue;
                };
                const t = gpa.dupe(u8, doc.text) catch {
                    gpa.free(p);
                    gpa.free(u);
                    continue;
                };
                batch.append(gpa, .{ .uri = u, .path = p, .text = t, .version = doc.version }) catch {
                    gpa.free(p);
                    gpa.free(u);
                    gpa.free(t);
                };
            }
        }
        dirty.clearRetainingCapacity();
        docs_mu.unlock(g_io);

        for (batch.items) |doc| {
            var diags: std.ArrayList(Diag) = .empty;
            defer diags.deinit(gpa);
            runCheck(&doc, &diags);
            docs_mu.lockUncancelable(g_io);
            const still = !dirty.contains(doc.path);
            docs_mu.unlock(g_io);
            if (still) publishDiags(&doc, diags.items);
        }
        for (batch.items) |doc| {
            gpa.free(doc.uri);
            gpa.free(doc.path);
            gpa.free(doc.text);
        }
    }
}

// ---------------------------------------------------------------- LSP io

fn sendRaw(payload: []const u8) void {
    out_mu.lockUncancelable(g_io);
    defer out_mu.unlock(g_io);
    var hdr: [64]u8 = undefined;
    const h = std.fmt.bufPrint(&hdr, "Content-Length: {d}\r\n\r\n", .{payload.len}) catch return;
    g_out_writer.interface.writeAll(h) catch return;
    g_out_writer.interface.writeAll(payload) catch return;
    g_out_writer.interface.flush() catch {};
    // 出站审计：方法 + 时间戳 + 字节数。宿主（Zed）日志里的报错只有
    // 时间没有内容，靠这两边的时间戳对齐才能定位是哪条消息被拒收。
    {
        var meth: []const u8 = "";
        if (std.mem.indexOf(u8, payload, "\"method\":\"")) |mi| {
            const ms = mi + "\"method\":\"".len;
            const me = std.mem.indexOfPos(u8, payload, ms, "\"") orelse payload.len;
            meth = payload[ms..me];
        } else if (std.mem.indexOf(u8, payload, "\"id\":")) |_| {
            meth = "<response>";
        }
        const now_ns = Io.Clock.now(.real, g_io).nanoseconds;
        var secs: u64 = @intCast(@divFloor(now_ns, std.time.ns_per_s));
        secs = secs % (24 * 3600) + 8 * 3600; // UTC+8
        if (secs >= 24 * 3600) secs -= 24 * 3600;
        const hh = secs / 3600;
        const mm = (secs / 60) % 60;
        const ss = secs % 60;
        var b: std.ArrayList(u8) = .empty;
        // 响应带 id（与入站审计的 id 对齐）；前 90 字节内容片段便于
        // 直接看返回了什么。
        var resp_id: []const u8 = "";
        if (std.mem.indexOf(u8, payload, "\"id\":")) |ii| {
            const is = ii + "\"id\":".len;
            const ie = std.mem.indexOfAnyPos(u8, payload, is, ",}") orelse payload.len;
            resp_id = payload[is..ie];
        }
        b.print(gpa, "{d:0>2}:{d:0>2}:{d:0>2} out {s} id={s} {d}B ", .{ hh, mm, ss, meth, resp_id, payload.len }) catch {};
        if (payload.len > 0) {
            const snip = if (payload.len > 90) payload[0..90] else payload;
            b.appendSlice(gpa, "| ") catch {};
            b.appendSlice(gpa, snip) catch {};
        }
        b.append(gpa, '\n') catch {};
        logLine(b.items);
        b.deinit(gpa);
    }
}

fn readLine(rin: *Io.Reader, out: *std.ArrayList(u8)) bool {
    out.clearRetainingCapacity();
    const line = rin.takeDelimiterInclusive('\n') catch return false;
    out.appendSlice(gpa, line) catch return false;
    // 去掉 \r\n
    if (out.items.len > 0 and out.items[out.items.len - 1] == '\n') _ = out.pop();
    if (out.items.len > 0 and out.items[out.items.len - 1] == '\r') _ = out.pop();
    return true;
}

// ---------------------------------------------------------------- dispatch

fn writeId(j: *Json, id: ?std.json.Value) void {
    if (id) |iv| switch (iv) {
        .integer => |n| j.raw("{d}", .{n}),
        .float => |f| j.raw("{d}", .{f}),
        .string => |s| j.str(s),
        else => j.raw("null", .{}),
    };
}

fn handle(v: std.json.Value) ?[]const u8 {
    if (v != .object) return null;
    const obj = v.object;
    const method = if (obj.get("method")) |m| (if (m == .string) m.string else "") else "";
    const id = obj.get("id");
    const is_req = id != null;

    if (method.len == 0) return null; // 对端响应，忽略
    const params = obj.get("params");

    // 入站审计：method + id + 请求位置，与出站时间戳对齐可定位
    // 客户端发什么、我们答什么。logLine 每次开写，hover 频率低可接受。
    {
        var lb: std.ArrayList(u8) = .empty;
        defer lb.deinit(gpa);
        const now = Io.Clock.now(.real, g_io);
        var secs: u64 = @intCast(@divFloor(now.nanoseconds, std.time.ns_per_s));
        secs = secs % (24 * 3600) + 8 * 3600; // UTC+8
        if (secs >= 24 * 3600) secs -= 24 * 3600;
        lb.print(gpa, "{d:0>2}:{d:0>2}:{d:0>2} in {s} id=", .{
            secs / 3600, (secs / 60) % 60, secs % 60, method,
        }) catch {};
        if (id) |iv| {
            switch (iv) {
                .integer => |n| lb.print(gpa, "{d}", .{n}) catch {},
                .string => |s| lb.appendSlice(gpa, s) catch {},
                else => lb.appendSlice(gpa, "?") catch {},
            }
        } else lb.appendSlice(gpa, "-") catch {};
        if (params) |pm| {
            if (pm == .object) {
                if (pm.object.get("position")) |pp| {
                    if (pp == .object) {
                        const ln = pp.object.get("line");
                        const ch = pp.object.get("character");
                        if (ln != null and ch != null and ln.? == .integer and ch.? == .integer) {
                            lb.print(gpa, " pos={d}:{d}", .{ ln.?.integer, ch.?.integer }) catch {};
                        }
                    }
                }
            }
        }
        logLine(lb.items);
    }

    if (std.mem.eql(u8, method, "initialize")) {
        if (params) |pm| {
            if (pm == .object) {
                if (pm.object.get("rootUri")) |ru| {
                    if (ru == .string) root_path = uriToPath(ru.string);
                }
                if (root_path == null) {
                    if (pm.object.get("rootPath")) |rp| {
                        if (rp == .string and rp.string.len > 0) root_path = dupe(rp.string);
                    }
                }
                // 客户端能力：支持 codeDescription 才发那个字段
                if (pm.object.get("capabilities")) |caps| {
                    if (caps == .object) {
                        if (caps.object.get("textDocument")) |td| {
                            if (td == .object) {
                                if (td.object.get("publishDiagnostics")) |pd| {
                                    if (pd == .object) {
                                        if (pd.object.get("codeDescriptionSupport")) |cd| {
                                            if (cd == .bool and cd.bool) cap_code_desc = true;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        var j = Json{};
        j.raw("{{\"jsonrpc\":\"2.0\",\"id\":", .{});
        writeId(&j, id);
        // KEEP: inlayHintProvider 不能删 —— Zed 靠这个声明决定要不要发
        // textDocument/inlayHint；删掉等于行内类型提示整条线失效。
        j.buf.appendSlice(gpa, ",\"result\":{\"capabilities\":{\"textDocumentSync\":{\"openClose\":true,\"change\":1},\"completionProvider\":{\"triggerCharacters\":[\".\",\"@\"]},\"definitionProvider\":true,\"hoverProvider\":true,\"inlayHintProvider\":true,\"signatureHelpProvider\":{\"triggerCharacters\":[\"(\",\",\"]},\"documentFormattingProvider\":true,\"documentSymbolProvider\":true,\"referencesProvider\":true}}}") catch @panic("oom");
        // 工程根的 vendor/ 与 deps：补全/跳转/悬停都靠这张表找模块文件
        initModuleDirs();
        // 落一行能力协商结果：改了 codeDescription 相关逻辑后，看这条就知道
        // 是"客户端没声明"还是"我们没发"——省得去猜编辑器版本。
        logLine(if (cap_code_desc) "init: codeDescription=yes" else "init: codeDescription=no");
        return j.buf.items;
    }
    if (std.mem.eql(u8, method, "initialized")) return null;
    if (std.mem.eql(u8, method, "shutdown")) {
        var j = Json{};
        j.raw("{{\"jsonrpc\":\"2.0\",\"id\":", .{});
        writeId(&j, id);
        j.raw(",\"result\":null}}", .{});
        return j.buf.items;
    }
    if (std.mem.eql(u8, method, "exit")) {
        docs_mu.lockUncancelable(g_io);
        quitting = true;
        docs_cond.signal(g_io);
        docs_mu.unlock(g_io);
        std.process.exit(0);
    }

    if (std.mem.eql(u8, method, "textDocument/didOpen") or std.mem.eql(u8, method, "textDocument/didChange")) {
        blk: {
            const pm = params orelse break :blk;
            if (pm != .object) break :blk;
            const td = pm.object.get("textDocument") orelse break :blk;
            if (td != .object) break :blk;
            const uri_v = td.object.get("uri") orelse break :blk;
            if (uri_v != .string) break :blk;
            const version: i64 = if (td.object.get("version")) |vv| (if (vv == .integer) vv.integer else 0) else 0;
            var text: []const u8 = "";
            if (std.mem.eql(u8, method, "textDocument/didOpen")) {
                if (td.object.get("text")) |t| {
                    if (t == .string) text = t.string;
                }
            } else {
                const changes = pm.object.get("contentChanges") orelse break :blk;
                if (changes != .array or changes.array.items.len == 0) break :blk;
                const last = changes.array.items[changes.array.items.len - 1];
                if (last != .object) break :blk;
                if (last.object.get("text")) |t| {
                    if (t == .string) text = t.string;
                }
            }
            const path = uriToPath(uri_v.string);
            const doc = Document{ .uri = dupe(uri_v.string), .path = path, .text = dupe(text), .version = version };
            docs_mu.lockUncancelable(g_io);
            // KEEP：脏表先摘，再释放旧文档。脏表的键指向 docs 里那份 path，
            // 先 free 就会留下悬垂键——下一次 put 触发扩容重哈希，或者
            // diagnosticsWorker 拿它去 docs.get，读到的是已释放内存。
            // 同一文件被 didOpen 两次时必现：ReleaseSafe 下报成
            // hash_map.zig:1029 的 panic，换个分配器就是 access violation。
            _ = dirty.fetchRemove(path);
            if (docs.fetchRemove(path)) |kv| {
                gpa.free(kv.value.uri);
                gpa.free(kv.value.path);
                gpa.free(kv.value.text);
            }
            docs.put(gpa, doc.path, doc) catch {};
            dirty.put(gpa, doc.path, {}) catch {};
            docs_cond.signal(g_io);
            docs_mu.unlock(g_io);
            logLine(uri_v.string);
        }
        return null;
    }

    if (std.mem.eql(u8, method, "textDocument/didClose")) {
        blk: {
            const pm = params orelse break :blk;
            if (pm != .object) break :blk;
            const td = pm.object.get("textDocument") orelse break :blk;
            if (td != .object) break :blk;
            const uri_v = td.object.get("uri") orelse break :blk;
            if (uri_v != .string) break :blk;
            const path = uriToPath(uri_v.string);
            docs_mu.lockUncancelable(g_io);
            // 同上：先按 path 摘脏表，再释放文档本身。
            _ = dirty.fetchRemove(path);
            if (docs.fetchRemove(path)) |kv| {
                gpa.free(kv.value.uri);
                gpa.free(kv.value.path);
                gpa.free(kv.value.text);
            }
            docs_mu.unlock(g_io);
            var j = Json{};
            j.raw("{{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{{\"uri\":", .{});
            j.str(uri_v.string);
            j.raw(",\"diagnostics\":[]}}}}", .{});
            return j.buf.items;
        }
        return null;
    }

    if (std.mem.eql(u8, method, "textDocument/completion")) {
        var result: ?[]const u8 = null;
        blk: {
            const pm = params orelse break :blk;
            if (pm != .object) break :blk;
            const td = pm.object.get("textDocument") orelse break :blk;
            const pos = pm.object.get("position") orelse break :blk;
            if (td != .object or pos != .object) break :blk;
            const uri_v = td.object.get("uri") orelse break :blk;
            const ln_v = pos.object.get("line") orelse break :blk;
            const ch_v = pos.object.get("character") orelse break :blk;
            if (uri_v != .string or ln_v != .integer or ch_v != .integer) break :blk;
            const path = uriToPath(uri_v.string);
            docs_mu.lockUncancelable(g_io);
            const doc_copy: ?Document = docs.get(path);
            docs_mu.unlock(g_io);
            if (doc_copy) |d| {
                result = completeAt(&d, @intCast(ln_v.integer), @intCast(ch_v.integer));
            }
        }
        var j = Json{};
        j.raw("{{\"jsonrpc\":\"2.0\",\"id\":", .{});
        writeId(&j, id);
        j.raw(",\"result\":", .{});
        if (result) |r| {
            j.buf.appendSlice(gpa, r) catch {};
        } else j.raw("{{\"isIncomplete\":false,\"items\":[]}}", .{});
        j.raw("}}", .{});
        return j.buf.items;
    }

    if (std.mem.eql(u8, method, "textDocument/definition")) {
        var result: ?[]const u8 = null;
        blk: {
            const pm = params orelse break :blk;
            if (pm != .object) break :blk;
            const td = pm.object.get("textDocument") orelse break :blk;
            const pos = pm.object.get("position") orelse break :blk;
            if (td != .object or pos != .object) break :blk;
            const uri_v = td.object.get("uri") orelse break :blk;
            const ln_v = pos.object.get("line") orelse break :blk;
            const ch_v = pos.object.get("character") orelse break :blk;
            if (uri_v != .string or ln_v != .integer or ch_v != .integer) break :blk;
            const path = uriToPath(uri_v.string);
            docs_mu.lockUncancelable(g_io);
            const doc_copy: ?Document = docs.get(path);
            docs_mu.unlock(g_io);
            if (doc_copy) |d| {
                result = definitionAt(&d, @intCast(ln_v.integer), @intCast(ch_v.integer));
            }
        }
        var j = Json{};
        j.raw("{{\"jsonrpc\":\"2.0\",\"id\":", .{});
        writeId(&j, id);
        j.raw(",\"result\":", .{});
        if (result) |r| {
            j.buf.appendSlice(gpa, r) catch {};
        } else j.raw("null", .{});
        j.raw("}}", .{});
        return j.buf.items;
    }

    if (std.mem.eql(u8, method, "textDocument/hover")) {
        var result: ?[]const u8 = null;
        blk: {
            const pm = params orelse break :blk;
            if (pm != .object) break :blk;
            const td = pm.object.get("textDocument") orelse break :blk;
            const pos = pm.object.get("position") orelse break :blk;
            if (td != .object or pos != .object) break :blk;
            const uri_v = td.object.get("uri") orelse break :blk;
            const ln_v = pos.object.get("line") orelse break :blk;
            const ch_v = pos.object.get("character") orelse break :blk;
            if (uri_v != .string or ln_v != .integer or ch_v != .integer) break :blk;
            const path = uriToPath(uri_v.string);
            docs_mu.lockUncancelable(g_io);
            const doc_copy: ?Document = docs.get(path);
            docs_mu.unlock(g_io);
            if (doc_copy) |d| {
                result = hoverAt(&d, @intCast(ln_v.integer), @intCast(ch_v.integer));
            }
        }
        var j = Json{};
        j.raw("{{\"jsonrpc\":\"2.0\",\"id\":", .{});
        writeId(&j, id);
        j.raw(",\"result\":", .{});
        if (result) |r| {
            j.buf.appendSlice(gpa, r) catch {};
        } else j.raw("null", .{});
        j.raw("}}", .{});
        return j.buf.items;
    }

    // KEEP: 行内类型提示入口，与上面的 capability 声明配套（见 inlayHintsAt）
    if (std.mem.eql(u8, method, "textDocument/inlayHint")) {
        var result: ?[]const u8 = null;
        blk: {
            const pm = params orelse break :blk;
            if (pm != .object) break :blk;
            const td = pm.object.get("textDocument") orelse break :blk;
            if (td != .object) break :blk;
            const uri_v = td.object.get("uri") orelse break :blk;
            if (uri_v != .string) break :blk;
            var sl: u32 = 0;
            var el: u32 = std.math.maxInt(u32);
            if (pm.object.get("range")) |rg| {
                if (rg == .object) {
                    if (rg.object.get("start")) |st| {
                        if (st == .object) {
                            if (st.object.get("line")) |slv| {
                                if (slv == .integer and slv.integer > 0) sl = @intCast(slv.integer);
                            }
                        }
                    }
                    if (rg.object.get("end")) |en| {
                        if (en == .object) {
                            if (en.object.get("line")) |elv| {
                                if (elv == .integer and elv.integer >= 0 and elv.integer < std.math.maxInt(u32)) el = @intCast(elv.integer);
                            }
                        }
                    }
                }
            }
            const path = uriToPath(uri_v.string);
            docs_mu.lockUncancelable(g_io);
            const doc_copy: ?Document = docs.get(path);
            docs_mu.unlock(g_io);
            if (doc_copy) |d| result = inlayHintsAt(&d, sl, el);
        }
        var j = Json{};
        j.raw("{{\"jsonrpc\":\"2.0\",\"id\":", .{});
        writeId(&j, id);
        j.raw(",\"result\":", .{});
        if (result) |r| {
            j.buf.appendSlice(gpa, r) catch {};
        } else j.raw("[]", .{});
        j.raw("}}", .{});
        return j.buf.items;
    }

    if (std.mem.eql(u8, method, "textDocument/signatureHelp")) {
        var result: ?[]const u8 = null;
        blk: {
            const pm = params orelse break :blk;
            if (pm != .object) break :blk;
            const td = pm.object.get("textDocument") orelse break :blk;
            const pos = pm.object.get("position") orelse break :blk;
            if (td != .object or pos != .object) break :blk;
            const uri_v = td.object.get("uri") orelse break :blk;
            const ln_v = pos.object.get("line") orelse break :blk;
            const ch_v = pos.object.get("character") orelse break :blk;
            if (uri_v != .string or ln_v != .integer or ch_v != .integer) break :blk;
            const path = uriToPath(uri_v.string);
            docs_mu.lockUncancelable(g_io);
            const doc_copy: ?Document = docs.get(path);
            docs_mu.unlock(g_io);
            if (doc_copy) |d| {
                result = signatureHelpAt(&d, @intCast(ln_v.integer), @intCast(ch_v.integer));
            }
        }
        var j = Json{};
        j.raw("{{\"jsonrpc\":\"2.0\",\"id\":", .{});
        writeId(&j, id);
        j.raw(",\"result\":", .{});
        if (result) |r| {
            j.buf.appendSlice(gpa, r) catch {};
        } else j.raw("null", .{});
        j.raw("}}", .{});
        return j.buf.items;
    }

    if (std.mem.eql(u8, method, "textDocument/formatting")) {
        var result: ?[]const u8 = null;
        blk: {
            const pm = params orelse break :blk;
            if (pm != .object) break :blk;
            const td = pm.object.get("textDocument") orelse break :blk;
            if (td != .object) break :blk;
            const uri_v = td.object.get("uri") orelse break :blk;
            if (uri_v != .string) break :blk;
            const path = uriToPath(uri_v.string);
            docs_mu.lockUncancelable(g_io);
            const doc_copy: ?Document = docs.get(path);
            docs_mu.unlock(g_io);
            if (doc_copy) |d| result = formatDoc(&d);
        }
        var j = Json{};
        j.raw("{{\"jsonrpc\":\"2.0\",\"id\":", .{});
        writeId(&j, id);
        j.raw(",\"result\":", .{});
        if (result) |r| {
            j.buf.appendSlice(gpa, r) catch {};
        } else j.raw("[]", .{});
        j.raw("}}", .{});
        return j.buf.items;
    }

    if (std.mem.eql(u8, method, "textDocument/documentSymbol")) {
        var result: ?[]const u8 = null;
        blk: {
            const pm = params orelse break :blk;
            if (pm != .object) break :blk;
            const td = pm.object.get("textDocument") orelse break :blk;
            if (td != .object) break :blk;
            const uri_v = td.object.get("uri") orelse break :blk;
            if (uri_v != .string) break :blk;
            const path = uriToPath(uri_v.string);
            docs_mu.lockUncancelable(g_io);
            const doc_copy: ?Document = docs.get(path);
            docs_mu.unlock(g_io);
            if (doc_copy) |d| result = documentSymbols(&d);
        }
        var j = Json{};
        j.raw("{{\"jsonrpc\":\"2.0\",\"id\":", .{});
        writeId(&j, id);
        j.raw(",\"result\":", .{});
        if (result) |r| {
            j.buf.appendSlice(gpa, r) catch {};
        } else j.raw("[]", .{});
        j.raw("}}", .{});
        return j.buf.items;
    }

    if (std.mem.eql(u8, method, "textDocument/references")) {
        var result: ?[]const u8 = null;
        blk: {
            const pm = params orelse break :blk;
            if (pm != .object) break :blk;
            const td = pm.object.get("textDocument") orelse break :blk;
            const pos = pm.object.get("position") orelse break :blk;
            const ctx = pm.object.get("context") orelse break :blk;
            if (td != .object or pos != .object or ctx != .object) break :blk;
            const uri_v = td.object.get("uri") orelse break :blk;
            const ln_v = pos.object.get("line") orelse break :blk;
            const ch_v = pos.object.get("character") orelse break :blk;
            if (uri_v != .string or ln_v != .integer or ch_v != .integer) break :blk;
            const inc: bool = if (ctx.object.get("includeDeclaration")) |iv|
                (if (iv == .bool) iv.bool else true)
            else
                true;
            const path = uriToPath(uri_v.string);
            docs_mu.lockUncancelable(g_io);
            const doc_copy: ?Document = docs.get(path);
            docs_mu.unlock(g_io);
            if (doc_copy) |d| {
                result = referencesAt(&d, @intCast(ln_v.integer), @intCast(ch_v.integer), inc);
            }
        }
        var j = Json{};
        j.raw("{{\"jsonrpc\":\"2.0\",\"id\":", .{});
        writeId(&j, id);
        j.raw(",\"result\":", .{});
        if (result) |r| {
            j.buf.appendSlice(gpa, r) catch {};
        } else j.raw("[]", .{});
        j.raw("}}", .{});
        return j.buf.items;
    }

    // 未知请求回错误；通知忽略
    if (is_req) {
        var j = Json{};
        j.raw("{{\"jsonrpc\":\"2.0\",\"id\":", .{});
        writeId(&j, id);
        j.raw(",\"error\":{{\"code\":-32601,\"message\":\"method not found\"}}}}", .{});
        return j.buf.items;
    }
    return null;
}

fn exeDir() []const u8 {
    // Windows：从 PEB 的 ImagePathName 取 exe 路径
    if (@import("builtin").os.tag == .windows) {
        const pp = std.os.windows.peb().ProcessParameters;
        const buf = pp.ImagePathName.Buffer orelse return ".";
        const w = buf[0 .. pp.ImagePathName.Length / 2];
        const u8buf = gpa.alloc(u8, w.len * 4) catch return ".";
        const n = std.unicode.utf16LeToUtf8(u8buf, w) catch return ".";
        const p = u8buf[0..n];
        if (std.mem.lastIndexOfScalar(u8, p, '\\')) |s| return dupe(p[0..s]);
        if (std.mem.lastIndexOfScalar(u8, p, '/')) |s| return dupe(p[0..s]);
        return ".";
    }
    return ".";
}

pub fn main() !void {
    g_threaded = Io.Threaded.init(gpa, .{});
    g_io = g_threaded.io();

    // 启动探针：确认被宿主（Zed 等）拉起。cwd 由宿主设为项目根，落 bin/zxls-lsp.log。
    logLine("boot");

    const stdout = Io.File.stdout();
    g_out_writer = stdout.writer(g_io, &g_out_buf);
    const stdin = Io.File.stdin();
    g_in_reader = stdin.reader(g_io, &g_in_buf);
    const rin = &g_in_reader.interface;

    // 兄弟 zxc.exe
    {
        var p: std.ArrayList(u8) = .empty;
        p.appendSlice(gpa, exeDir()) catch {};
        p.appendSlice(gpa, "/zxc.exe") catch {};
        zxc_path = p.items;
    }

    // std 目录（与编译器同链路）：仓库外工程 std.Task 补全的命脉
    g_std_dir = findStdDir();

    const worker = try std.Thread.spawn(.{}, diagnosticsWorker, .{});

    var hdr_line: std.ArrayList(u8) = .empty;
    defer hdr_line.deinit(gpa);

    outer: while (true) {
        var content_len: usize = 0;
        while (true) {
            if (!readLine(rin, &hdr_line)) break :outer;
            if (hdr_line.items.len == 0) break; // 空行 = 头结束
            const kv = std.mem.indexOf(u8, hdr_line.items, ": ") orelse continue;
            const key = hdr_line.items[0..kv];
            const val = hdr_line.items[kv + 2 ..];
            if (std.ascii.eqlIgnoreCase(key, "Content-Length"))
                content_len = std.fmt.parseInt(usize, val, 10) catch 0;
        }
        if (content_len == 0 or content_len > 64 * 1024 * 1024) continue;
        const body = gpa.alloc(u8, content_len) catch continue;
        defer gpa.free(body);
        rin.readSliceAll(body) catch break :outer;

        const parsed = std.json.parseFromSlice(std.json.Value, gpa, body, .{}) catch continue;
        if (handle(parsed.value)) |resp| sendRaw(resp);
    }

    docs_mu.lockUncancelable(g_io);
    quitting = true;
    docs_cond.signal(g_io);
    docs_mu.unlock(g_io);
    worker.join();
}
