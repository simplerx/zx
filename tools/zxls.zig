// zxls.zig — Zx 语言服务器（LSP over stdio）。
//
// 设计：
//   - 诊断：didOpen/didChange 后防抖，spawn 兄弟目录的 `zxc <file> --check`，
//     解析 stderr 的 `path:line:col: error[CODE]: msg` 行 → publishDiagnostics。
//     直接复用自举前端（zxfe），诊断与编译器 100% 同源。
//   - 补全：
//       `@` 前缀      → 顶层内建（@sizeOf/@typeInfo/…）
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
};

// std 顶层由编译器 seed 的名字（src/sema.c g_std_seeded；std.zx 自己的声明另行扫描）
const std_seeded = [_][]const u8{
    "print",     "println", "format",   "heap",     "file",
    "io",        "Allocator", "TypeKind", "TypeInfo", "FieldInfo",
    "stackAlloc", "heapAlloc", "process",
};

const BuiltinDoc = struct { name: []const u8, detail: []const u8 };

// @ 内建（与 src/sema.c check_builtin() 逐字对齐——没有 @print/@panic，那些是 std 成员）
const builtin_docs = [_]BuiltinDoc{
    .{ .name = "sizeOf", .detail = "@sizeOf(T) 类型大小（字节）" },
    .{ .name = "alignOf", .detail = "@alignOf(T) 类型对齐" },
    .{ .name = "typeOf", .detail = "@typeOf(v) 值的类型" },
    .{ .name = "typeName", .detail = "@typeName(T) 类型名字符串" },
    .{ .name = "typeInfo", .detail = "@typeInfo(T) 编译期类型信息" },
    .{ .name = "as", .detail = "@as(T, v) 定向类型转换" },
    .{ .name = "intCast", .detail = "@intCast(T, v) 整数类型转换" },
    .{ .name = "ptrCast", .detail = "@ptrCast(T, p) 指针类型转换" },
    .{ .name = "bitCast", .detail = "@bitCast(T, v) 位型重解释" },
    .{ .name = "field", .detail = "@field(obj, \"name\") 按编译期名取字段" },
    .{ .name = "hasField", .detail = "@hasField(T, \"name\") 编译期字段检查" },
    .{ .name = "fieldType", .detail = "@fieldType(T, \"name\") 字段类型" },
    .{ .name = "fieldName", .detail = "@fieldName(T, i) 第 i 个字段名" },
    .{ .name = "memcpy", .detail = "@memcpy(dest, src, count) 内存复制" },
    .{ .name = "memmove", .detail = "@memmove(dest, src, count) 内存移动" },
    .{ .name = "memset", .detail = "@memset(dest, fill, count) 内存填充" },
    .{ .name = "trap", .detail = "@trap() 触发断点/中止" },
    .{ .name = "compileError", .detail = "@compileError(msg) 编译期报错" },
};

const toplevel_builtins = [_][]const u8{
    "sizeOf",       "alignOf", "typeOf",   "typeName", "typeInfo",
    "as",           "intCast", "ptrCast",  "bitCast",
    "field",        "hasField", "fieldType", "fieldName",
    "memcpy",       "memmove", "memset",   "trap",     "compileError",
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
fn topLevelDecls(text: []const u8, out: *std.ArrayList(Decl), pub_only: bool) void {
    var it = std.mem.splitScalar(u8, text, '\n');
    while (it.next()) |line_raw| {
        var rest = std.mem.trim(u8, line_raw, " \t\r");
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
            return .{ .body = ftext[p..], .is_enum = is_enum };
        }
    }
    return null;
}

fn containerMembers(files: []const []const u8, base: []const u8, fields: *std.ArrayList(Decl)) bool {
    const hit = findContainerBody(files, base) orelse return false;
    extractBraceMembers(hit.body, hit.is_enum, fields);
    return true;
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
        var n = q;
        while (n < body.len and isIdentChar(body[n])) n += 1;
        if (n == q) continue;
        return dupe(body[q..n]);
    }
    return null;
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

/// 点路径的函数声明解析（第一段不是模块绑定的形状）：
///   `std.Task.group` → std 目录文件里 Task 容器内的 group 方法
///   `task.Obj.make`  → 模块 task 文件里 Obj 容器内的 make
///   `Obj.make`       → 扫描集里 Obj 容器内的 make（本文件或 std）
/// 返回声明语句切片（sigOfStmt 可直接消费），找不到返回 null。
fn resolvePathStmt(files: []const []const u8, doc: *const Document, text: []const u8, path: []const u8) ?[]const u8 {
    var segs: [4][]const u8 = undefined;
    var n: usize = 0;
    var it = std.mem.splitScalar(u8, path, '.');
    while (it.next()) |seg| {
        if (seg.len == 0) return null;
        if (n >= segs.len) return null;
        segs[n] = seg;
        n += 1;
    }
    if (n < 2) return null;
    // 第一段是模块绑定 → 先限定在模块文件里找
    if (moduleOfBinding(text, segs[0])) |mod| {
        if (n > 3) return null;
        const p = resolveModuleFile(doc, mod) orelse return null;
        const t = readFileSmall(p) orelse return null;
        if (n == 2) return findDeclStmt(t, segs[1]);
        if (findContainerBody(&.{t}, segs[1])) |ch| {
            if (findDeclStmt(ch.body, segs[2])) |s| return s;
        }
        // 门面文件里没有（std.zx 只有别名 pub const Task = task.Task，
        // 真身在模块目录的 task.zx）——按编译器 module 目录语义扫整个目录。
        const mdir = dirName(p);
        var mfiles: std.ArrayList([]const u8) = .empty;
        var mdir_d = Io.Dir.cwd().openDir(g_io, mdir, .{ .iterate = true }) catch return null;
        defer mdir_d.close(g_io);
        var mit = mdir_d.iterate();
        while (mit.next(g_io) catch null) |entry| {
            if (entry.kind != .file or !std.mem.endsWith(u8, entry.name, ".zx")) continue;
            var fp: std.ArrayList(u8) = .empty;
            defer fp.deinit(gpa);
            fp.appendSlice(gpa, mdir) catch continue;
            fp.append(gpa, '/') catch continue;
            fp.appendSlice(gpa, entry.name) catch continue;
            if (readFileSmall(fp.items)) |txt| mfiles.append(gpa, txt) catch {};
        }
        if (findContainerBody(mfiles.items, segs[1])) |ch|
            return findDeclStmt(ch.body, segs[2]);
        return null;
    }
    // std.X.member / X.member：容器在扫描集里找（std 目录已并入 files）
    if (n == 3) {
        const ch = findContainerBody(files, segs[1]) orelse return null;
        return findDeclStmt(ch.body, segs[2]);
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
        const nm = trimSl(seg[0..cl]);
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

/// initialize 之后调用一次：工程根的 vendor/ 与 zx.json 的 deps
fn initModuleDirs() void {
    g_mod_dir_n = 0;
    const root = root_path orelse return;
    addModDir(joinPath(root, "vendor"));
    const cfg = joinPath(root, "zx.json");
    defer gpa.free(cfg);
    const txt = readFileSmall(cfg) orelse return;
    defer gpa.free(txt);
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

/// 模块解析目录，顺序：文件所在目录 → 工程根 → vendor/ → deps。
/// 前两个每次现算（跟着文档走），后两个 initialize 时已定。
fn modDirsInto(doc_dir: []const u8, out: *std.ArrayList([]const u8)) void {
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

    // 1) 当前文档声明
    if (findDeclLine(text, word)) |loc|
        return makeLocation(doc.uri, loc.line, loc.col, loc.col + @as(u32, @intCast(word.len)));

    // 1.5) <module>.<member>：光标停在成员上 → 模块文件的顶层声明
    if (ws > 0 and text[ws - 1] == '.') {
        var bs = ws - 1;
        while (bs > 0 and (isIdentChar(text[bs - 1]) or text[bs - 1] == '.')) bs -= 1;
        const base = text[bs .. ws - 1];
        if (base.len > 0) {
            var mm: ?[]const u8 = moduleOfBinding(text, base);
            if (mm == null and std.mem.eql(u8, base, "std")) mm = "std";
            if (mm) |mod_name| {
                if (resolveModuleFile(doc, mod_name)) |p| {
                    if (readFileSmall(p)) |t| {
                        if (findDeclLine(t, word)) |loc|
                            return makeLocation(pathToUri(p), loc.line, loc.col, loc.col + @as(u32, @intCast(word.len)));
                    }
                }
            }
        }
    }

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

fn inferExprType(text: []const u8, doc: *const Document, expr: []const u8, depth: u8) ?[]const u8 {
    const e = trimSl(expr);
    if (e.len == 0) return null;
    if (std.mem.startsWith(u8, e, "module(")) return null;
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
    // 调用：path(args)
    if (std.mem.indexOfScalar(u8, e, '(')) |lp| {
        const tail = trimSl(e[e.len - 1 ..]);
        if (std.mem.eql(u8, tail, ")")) {
            var ps = lp;
            while (ps > 0 and (isIdentChar(e[ps - 1]) or e[ps - 1] == '.')) ps -= 1;
            const callee = trimSl(e[ps..lp]);
            if (callee.len > 0) {
                if (resolveCalleeRet(text, doc, callee)) |rt| return rt;
            }
            return null;
        }
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
            if (!isIdentChar(e[k]) or (e[k] >= '0' and e[k] <= '9' and k == 0)) {
                ok = false;
                break;
            }
        }
        if (ok) {
            var sf = scanFiles(doc, text);
            defer sf.deinit(gpa);
            if (isContainerIn(sf.items, e[0..lb])) return e[0..lb];
        }
    }
    // 点路径分两类：类型名（std.Buffer、task.Obj）原样返回；
    // 实例成员访问（p.age / g.spawn）解析成字段类型或方法返回类型。
    // 此前一律原样返回，const n = p.age 的 hover 会把 "p.age" 当类型名。
    if (std.mem.indexOfScalar(u8, e, '.') != null) {
        var ok = true;
        for (e) |ch| {
            if (!isIdentChar(ch) and ch != '.') {
                ok = false;
                break;
            }
        }
        if (ok) {
            const dot = std.mem.lastIndexOfScalar(u8, e, '.') orelse return null;
            const base = e[0..dot];
            const member = e[dot + 1 ..];
            if (base.len == 0 or member.len == 0) return null;
            // base 是模块绑定（std.Buffer / task.Obj）→ 类型路径原样
            if (std.mem.eql(u8, base, "std") or moduleOfBinding(text, base) != null) return e;
            var sf = scanFiles(doc, text);
            defer sf.deinit(gpa);
            // 实例成员 → 容器体里的字段类型 / 方法返回类型
            if (instanceContainerTy(sf.items, text, base)) |ty| {
                if (findContainerBody(sf.items, ty)) |ch| {
                    if (fieldTypeIn(ch.body, member)) |t| return t;
                    if (findDeclStmt(ch.body, member)) |stmt| {
                        const sig = sigOfStmt(stmt);
                        if (sig.is_fn and sig.ret.len > 0) return sig.ret;
                    }
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
    // 裸标识符：追一层别名（const b = a）
    if (depth < 3 and isIdentChar(e[0]) and std.mem.indexOfScalar(u8, e, '.') == null) {
        if (findDeclStmt(text, e)) |stmt2| {
            const init2 = declInitOf(stmt2) orelse return null;
            return inferExprType(text, doc, init2, depth + 1);
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

/// 调用目标 → 返回类型。callee 可为 `Add`（本文件）、`other.Add`（模块成员）、
/// `std.print`（编译器内建）。
fn resolveCalleeRet(text: []const u8, doc: *const Document, callee: []const u8) ?[]const u8 {
    // std 内建（编译器 seed，不在 std.zx 文本里）
    if (std.mem.eql(u8, callee, "std.print") or std.mem.eql(u8, callee, "std.println")) return "void";
    if (std.mem.eql(u8, callee, "std.format")) return "str";

    if (std.mem.lastIndexOfScalar(u8, callee, '.')) |dot| {
        const bind = callee[0..dot];
        const member = callee[dot + 1 ..];
        if (member.len == 0) return null;
        var mm: ?[]const u8 = moduleOfBinding(text, bind);
        if (mm == null and std.mem.eql(u8, bind, "std")) mm = "std";
        const mod = mm orelse {
            // 容器限定（std.Task.group）或实例方法（g.spawn）：扫描集里解析
            var sf = scanFiles(doc, text);
            defer sf.deinit(gpa);
            if (resolvePathStmt(sf.items, doc, text, callee)) |stmt| {
                const sig = sigOfStmt(stmt);
                if (sig.is_fn and sig.ret.len > 0) return sig.ret;
            }
            if (instanceContainerTy(sf.items, text, bind)) |ty| {
                if (findContainerBody(sf.items, ty)) |ch| {
                    if (findDeclStmt(ch.body, member)) |stmt| {
                        const sig = sigOfStmt(stmt);
                        if (sig.is_fn and sig.ret.len > 0) return sig.ret;
                    }
                }
            }
            return null;
        };
        const p = resolveModuleFile(doc, mod) orelse return null;
        const t = readFileSmall(p) orelse return null;
        const stmt = findDeclStmt(t, member) orelse return null;
        const sig = sigOfStmt(stmt);
        if (sig.is_fn and sig.ret.len > 0) return sig.ret;
        return null;
    }
    const stmt = findDeclStmt(text, callee) orelse return null;
    const sig = sigOfStmt(stmt);
    if (sig.is_fn and sig.ret.len > 0) return sig.ret;
    return null;
}

/// 成员路径（a、other.Add、std.heap）全文扫描
fn identPathBefore(text: []const u8, off: usize) []const u8 {
    var s = off;
    while (s > 0 and (isIdentChar(text[s - 1]) or text[s - 1] == '.')) s -= 1;
    var e = off;
    while (e < text.len and (isIdentChar(text[e]) or text[e] == '.')) e += 1;
    return text[s..e];
}

// ---- 实例变量点补全：变量 → 容器类型 ---------------------------------------

/// base 是否是某个文件里的 struct/union/enum 容器（containerMembers 的存在性探针）
fn isContainerIn(files: []const []const u8, base: []const u8) bool {
    var tmp: std.ArrayList(Decl) = .empty;
    defer tmp.deinit(gpa);
    return containerMembers(files, base, &tmp);
}

/// 初始化表达式 → 容器类型名。识别四类：
///   `X{…}` 字面量、`X.static(...)` 静态调用（X 是容器且不是模块绑定）、
///   `std.X.method(...)`（容器是第二段，files 需含 std 目录）、
///   裸别名链（`const v = w` 递归追初始化）。识别不了返回 null。
fn inferContainer(files: []const []const u8, text: []const u8, expr: []const u8, depth: u8) ?[]const u8 {
    const e = trimSl(expr);
    if (e.len == 0 or depth > 3) return null;
    if (!isIdentChar(e[0]) or (e[0] >= '0' and e[0] <= '9')) return null;
    var j: usize = 0;
    while (j < e.len and isIdentChar(e[j])) j += 1;
    if (j == 0) return null;
    const head = e[0..j];
    var k = j;
    while (k < e.len and (e[k] == ' ' or e[k] == '\t')) k += 1;
    if (k >= e.len) {
        // 裸标识符：别名，追一层
        if (findDeclStmt(text, head)) |s2| {
            const init2 = declInitOf(s2) orelse return null;
            return inferContainer(files, text, init2, depth + 1);
        }
        return null;
    }
    if (e[k] == '{' and isContainerIn(files, head)) return head; // X{…} 字面量
    if (e[k] == '(') {
        // 裸调用 f(...)：同文件函数的返回类型（let o = make() → Obj）。
        // 跨模块的 other.make() 需要 doc，这里先不追。
        if (findDeclStmt(text, head)) |s2| {
            const sig = sigOfStmt(s2);
            if (sig.is_fn and sig.ret.len > 0) return sig.ret;
        }
        return null;
    }
    if (e[k] == '.') {
        // 容器候选：std.X.method 的第二段，或 X.static 的首段
        var cont: []const u8 = "";
        var after: usize = 0; // 容器名结束位置
        if (std.mem.eql(u8, head, "std")) {
            // std.X.method(...) / std.X{…}：判定靠 isContainerIn(files, …)，
            // 补全调用点的 files 已含 std 目录；std.print 这类内建不是容器，
            // isContainerIn 返回 false 自然落空。
            var m = k + 1;
            while (m < e.len and (e[m] == ' ' or e[m] == '\t')) m += 1;
            var n = m;
            while (n < e.len and isIdentChar(e[n])) n += 1;
            if (n == m) return null;
            cont = e[m..n];
            after = n;
        } else if (moduleOfBinding(text, head) == null) {
            if (!isContainerIn(files, head)) return null; // X.static(...) 静态调用
            cont = head;
            after = j;
        } else return null;
        // cont 后跟 .method(...)：追方法返回类型（std.Task.group() → Group，
        // let g = … 之后 g. 出的是 Group 的 spawn/wait，不是 Task 的）。
        if (after < e.len and e[after] == '.') {
            var m2 = after + 1;
            while (m2 < e.len and (e[m2] == ' ' or e[m2] == '\t')) m2 += 1;
            var n2 = m2;
            while (n2 < e.len and isIdentChar(e[n2])) n2 += 1;
            if (n2 > m2 and n2 < e.len and e[n2] == '(') {
                if (methodRetIn(files, cont, e[m2..n2])) |ret| {
                    if (isContainerIn(files, ret)) return ret;
                }
            }
        }
        return cont;
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

    const path = trimSl(identPathBefore(text, off));
    if (path.len == 0) return null;

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
            if (instanceContainerTy(sf.items, text, bind)) |ty| {
                if (findContainerBody(sf.items, ty)) |ch| {
                    if (hoverMarkdownForDecl(ch.body, member, doc)) |md|
                        return hoverPayload(md);
                    // 字段成员（p.age 的 age）：出 `age: i32`
                    if (fieldTypeIn(ch.body, member)) |t| {
                        var b: std.ArrayList(u8) = .empty;
                        b.print(gpa, "```zx\n{s}: {s}\n```", .{ member, t }) catch return null;
                        return hoverPayload(b.items);
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
                    var b: std.ArrayList(u8) = .empty;
                    b.appendSlice(gpa, "```zx\n") catch return null;
                    b.appendSlice(gpa, member) catch {};
                    appendSigTail(&b, sig);
                    b.appendSlice(gpa, "\n```") catch {};
                    return hoverPayload(b.items);
                }
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
fn hoverMarkdownForDecl(text: []const u8, name: []const u8, doc: ?*const Document) ?[]const u8 {
    const stmt = findDeclStmt(text, name) orelse return null;
    const sig = sigOfStmt(stmt);
    var b: std.ArrayList(u8) = .empty;
    b.appendSlice(gpa, "```zx\n") catch return null;
    if (sig.is_fn) {
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
    var nm = trimSl(p);
    if (std.mem.indexOfScalar(u8, nm, ':')) |c| nm = trimSl(nm[0..c]);
    return nm;
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
fn calleeStmt(ctx: *InlayCtx, callee: []const u8) ?[]const u8 {
    const text = ctx.text;
    if (std.mem.lastIndexOfScalar(u8, callee, '.')) |dot| {
        const bind = callee[0..dot];
        const member = callee[dot + 1 ..];
        if (member.len == 0) return null;
        var mm: ?[]const u8 = moduleOfBinding(text, bind);
        if (mm == null and std.mem.eql(u8, bind, "std")) mm = "std";
        if (mm) |mod| {
            if (resolveModuleFile(ctx.doc, mod)) |p| {
                if (ctx.readCached(p)) |t| {
                    if (findDeclStmt(t, member)) |st| return st;
                }
            }
        }
        const fl = ctx.fileList();
        if (instanceContainerTy(fl, text, bind)) |ty| {
            if (findContainerBody(fl, ty)) |ch| {
                if (findDeclStmt(ch.body, member)) |st| return st;
            }
        }
        return resolvePathStmt(fl, ctx.doc, text, callee);
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
        var cs = q;
        while (cs > 0 and (isIdentChar(text[cs - 1]) or text[cs - 1] == '.')) cs -= 1;
        if (cs == q) continue; // 分组表达式 / cast 前缀，'(' 前没有标识符
        const callee = text[cs..q];
        if (callee[0] == '.' or isCallKeyword(callee)) continue;

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
            if (s0 < k and mask[s0] and argi + offset < names.len) {
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
/// `a.tw` → `twice(${1:x})$0`，带括号与形参占位。首参 self 的实例方法
/// 不出骨架（调用形态是 `o.m()`，点补全负责），返回 false 让调用方退回普通项。
fn memberCallSnippet(list: *Json, seen: *std.StringHashMapUnmanaged(void),
                     stmt: []const u8, name: []const u8) bool {
    const sig = sigOfStmt(stmt);
    if (!sig.is_fn) return false;
    var ins: std.ArrayList(u8) = .empty;
    defer ins.deinit(gpa);
    ins.appendSlice(gpa, name) catch return false;
    ins.append(gpa, '(') catch return false;
    var first = true;
    var tab: usize = 1;
    for (sig.params) |p0| {
        const nm = paramName(p0);
        if (nm.len == 0) continue;
        if (tab == 1 and std.mem.eql(u8, nm, "self")) return false;
        if (tab > 6) break;
        if (!first) ins.appendSlice(gpa, ", ") catch return false;
        first = false;
        ins.print(gpa, "${{{d}:{s}}}", .{ tab, nm }) catch return false;
        tab += 1;
    }
    ins.appendSlice(gpa, ")$0") catch return false;
    // detail 保留完整签名：骨架插的是 `name(…)`，弹窗右侧仍要能看出形参
    // 与返回类型（10b 当初的诉求），kind=15 只影响插入形态。
    var sb: std.ArrayList(u8) = .empty;
    defer sb.deinit(gpa);
    sb.appendSlice(gpa, name) catch return false;
    appendSigTail(&sb, sig);
    addSnippetRaw(list, seen, dupe(name), ins.items, sb.items);
    return true;
}

/// KEEP — 模块/容器类型的构造骨架（用户 2026-11-08 第 6 条）：
/// `a.C` → `Cfg{ .w = ${1:0}, .h = ${2:0} }$0`。枚举/零字段不出骨架。
fn memberCtorSnippet(list: *Json, seen: *std.StringHashMapUnmanaged(void),
                     src: []const u8, name: []const u8) bool {
    var fields: std.ArrayList(Decl) = .empty;
    defer fields.deinit(gpa);
    if (!containerMembers(&[_][]const u8{src}, name, &fields)) return false;
    var ins: std.ArrayList(u8) = .empty;
    defer ins.deinit(gpa);
    ins.appendSlice(gpa, name) catch return false;
    ins.appendSlice(gpa, "{ ") catch return false;
    var used: usize = 0;
    var tab: usize = 1;
    for (fields.items) |dcl| {
        if (dcl.is_fn or dcl.kind != .field or dcl.name.len == 0) continue;
        if (tab > 3) break;
        if (used > 0) ins.appendSlice(gpa, ", ") catch return false;
        ins.print(gpa, ".{s} = ${{{d}:0}}", .{ dcl.name, tab }) catch return false;
        used += 1;
        tab += 1;
    }
    if (used == 0) return false;
    ins.appendSlice(gpa, " }$0") catch return false;
    var dlab: []const u8 = "构造实例";
    if (findDeclStmt(src, name)) |st| {
        if (containerInitOf(st)) |ci| {
            if (std.mem.startsWith(u8, ci, "struct")) dlab = "构造实例 struct" else if
                (std.mem.startsWith(u8, ci, "union")) dlab = "构造实例 union" else if
                (std.mem.startsWith(u8, ci, "enum")) dlab = "构造实例 enum";
        }
    }
    addSnippetRaw(list, seen, dupe(name), ins.items, dlab);
    return true;
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
        var nm = trimSl(p0);
        if (std.mem.indexOfScalar(u8, nm, ':')) |c| nm = trimSl(nm[0..c]);
        if (nm.len == 0) continue;
        // 首参 self 是实例方法：`o.bump()` 走点补全，顶层不列调用骨架
        if (tab == 1 and std.mem.eql(u8, nm, "self")) return;
        if (tab > 6) break;
        if (!first) ins.appendSlice(gpa, ", ") catch return;
        first = false;
        ins.print(gpa, "${{{d}:{s}}}", .{ tab, nm }) catch return;
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
                // 普通点路径照旧回扫：'.' 前的标识符链（std / obj.field）
                base_start = base_end;
                while (base_start > 0 and (isIdentChar(text[base_start - 1]) or text[base_start - 1] == '.')) base_start -= 1;
            }
        }
        const path = text[base_start..path_end];

        var handled = false;

        // std.<ns>.：编译器内建命名空间成员（heap/file/io/process）
        if (std.mem.startsWith(u8, path, "std.")) {
            const ns = path["std.".len..];
            for (std_ns_table) |t| {
                if (!std.mem.eql(u8, t.name, ns)) continue;
                handled = true;
                for (t.members) |m| addItem(&list, &seen, m, 3, t.detail, 0);
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
                        if (anonParamTy(afiles.items, fname, argi)) |pty0| {
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
            // KEEP: vendor/deps 容器 `vp.Cfg{.` / `vp.Cfg.`——点路径的 path
            // 是带点的限定名，moduleOfBinding 对它不命中（绑定名本身不带点），
            // 上面的模块分支进不去，必须单独按 modContainerOf 解析容器字段。
            // lit_ctx 下 emit 循环把 .value 全过滤，字段就是全部；非字面量
            // 上下文字段单独成组。
            if (!handled) {
                if (modContainerOf(doc, text, path)) |mc0| {
                    if (containerMembers(&[_][]const u8{mc0.txt}, mc0.member, &decls)) {
                        handled = true;
                        mem_txt = mc0.txt;
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
            if (!handled) {
                // 容器成员：用路径最后一段匹配容器名
                var container = path;
                if (std.mem.lastIndexOfScalar(u8, path, '.')) |dot| container = path[dot + 1 ..];

                var files = scanFiles(doc, text);
                defer files.deinit(gpa);
                // 错误集成员：`error.`（全局码名）或 `Err.`（`const Err = error{…}`）。
                if (!handled) {
                    if (std.mem.eql(u8, container, "error"))
                        handled = collectAllErrMembers(files.items, &decls)
                    else
                        handled = collectNamedErrMembers(files.items, container, &decls);
                }
                if (containerMembers(files.items, container, &decls)) handled = decls.items.len > 0;
                if (!handled) {
                    // 不是模块/类型名——按实例变量处理：推断类型再补容器成员
                    //（let v = Obj.init(...) → v. 出 Obj 的字段与方法）
                    decls.clearRetainingCapacity();
                    // KEEP: vendor/deps 容器实例（var c = vp.Cfg{…}）——
                    // inferContainer 对绑定名打头的初始化一律 null，先走模块
                    // 解析补齐（2026-11-08：vendor struct 也要查）。
                    if (vendorInstanceFields(doc, text, container, &decls)) {
                        handled = decls.items.len > 0;
                    }
                    if (!handled) {
                        if (inferContainer(files.items, text, container, 0) orelse blk: {
                            const s = findDeclStmt(text, container) orelse break :blk null;
                            const init2 = declInitOf(s) orelse break :blk null;
                            break :blk inferContainer(files.items, text, init2, 1);
                        }) |ty| {
                            if (containerMembers(files.items, ty, &decls)) handled = decls.items.len > 0;
                        }
                    }
                    // KEEP - 匿名类型线（3v/3w/3x）：匿名绑定 `anon.`、嵌套
                    // `nv.pos.`、匿名形参 `info.` 的成员补全。命名容器路径全部
                    // 落空才到这里（`o.count` 的 Obj 实例等不受影响）。
                    if (!handled) {
                        if (anonTyOfPath(text, doc, path, off)) |aty| {
                            if (parseAnonTy(aty)) |flds| {
                                handled = flds.items.len > 0;
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
                                if (containerMembers(&[_][]const u8{mt}, mty, &mf)) {
                                    handled = mf.items.len > 0;
                                    for (mf.items) |d2| decls.append(gpa, d2) catch {};
                                    mem_txt = mt;
                                }
                            }
                        }
                    }
                }
            }
            if (handled) {
                const src: []const u8 = mem_txt orelse text;
                for (decls.items) |dcl| {
                    // 字面量初始化只能写字段/变体，方法（name = fn …）不出
                    if (lit_ctx and dcl.kind == .value) continue;
                    // 已输入前缀过滤：`a.tw` 只该出 twice（此前把整份成员
                    // 列表全弹出来，用户 2026-11-08 第 3/4/6 条）
                    if (prefix.len > 0 and !std.mem.startsWith(u8, dcl.name, prefix)) continue;
                    const k: u32 = if (dcl.is_fn) 3 else switch (dcl.kind) {
                        .field => 5,
                        .variant => 20,
                        .value => 6,
                    };
                    // 声明语句（模块文件/当前文档）：签名、容器判定、骨架共用
                    const stmt: ?[]const u8 = findDeclStmt(src, dcl.name);
                    // KEEP — 点补全里的 snippet（用户 2026-11-08 第 4/6 条）：
                    // 模块函数 → `twice(${1:x})$0`；模块 struct → `Cfg{ .w = ${1:0}… }`。
                    // 骨架发出即吞掉本条，不再并排出裸名项。首参 self 的方法
                    // 走普通项（调用形态是 `o.m()`）。
                    if (!lit_ctx and stmt != null) {
                        if (dcl.is_fn) {
                            if (memberCallSnippet(&list, &seen, stmt.?, dcl.name)) continue;
                        } else if (dcl.kind == .value and containerInitOf(stmt.?) != null) {
                            if (memberCtorSnippet(&list, &seen, src, dcl.name)) continue;
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
                                    sb.appendSlice(gpa, dcl.name) catch {};
                                    appendSigTail(&sb, sig);
                                    break :blk sb.items;
                                }
                            }
                            break :blk "函数";
                        }
                        if (stmt) |st| {
                            if (containerInitOf(st)) |ci| {
                                if (std.mem.startsWith(u8, ci, "struct")) break :blk "struct";
                                if (std.mem.startsWith(u8, ci, "union")) break :blk "union";
                                if (std.mem.startsWith(u8, ci, "enum")) break :blk "enum";
                                break :blk "type";
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
                for (std_seeded) |m| {
                    if (prefix.len == 0 or std.mem.startsWith(u8, m, prefix))
                        addItem(&list, &seen, m, 3, "编译器内建", 3);
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
            if (docs.get(k.*)) |doc| batch.append(gpa, doc) catch {};
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
            if (docs.fetchRemove(path)) |kv| {
                gpa.free(kv.value.uri);
                gpa.free(kv.value.path);
                gpa.free(kv.value.text);
            }
            _ = dirty.fetchRemove(path);
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
