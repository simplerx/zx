// zxls.zig — Zx 语言服务器（LSP over stdio）。
//
// 设计：
//   - 诊断：didOpen/didChange 后防抖，spawn 兄弟目录的 `zxc <file> --check`，
//     解析 stderr 的 `path:line:col: error[CODE]: msg` 行 → publishDiagnostics。
//     直接复用自举前端（zxfe），诊断与编译器 100% 同源。
//   - 补全：
//       `@` 前缀      → 顶层内建（@print/@typeInfo/…）
//       `@ns.`        → 命名空间成员（heap/file/io/process）
//       `std.` 等     → module 绑定对应的模块顶层声明（const/var/type）
//       `Type.`       → struct/union 字段、enum 变体（扫描工作区声明）
//       其余          → 关键字 + 工作区标识符
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

const Decl = struct { name: []const u8, kind: DeclKind };

/// 顶层声明：`[pub] (const|var|type) NAME =`
fn topLevelDecls(text: []const u8, out: *std.ArrayList(Decl), pub_only: bool) void {
    var it = std.mem.splitScalar(u8, text, '\n');
    while (it.next()) |line_raw| {
        const line = std.mem.trim(u8, line_raw, " \t\r");
        var rest = line;
        var is_pub = false;
        if (std.mem.startsWith(u8, rest, "pub ")) {
            is_pub = true;
            rest = std.mem.trimStart(u8, rest[4..], " \t");
        }
        if (pub_only and !is_pub) continue;
        const prefixes = [_][]const u8{ "const ", "var ", "type " };
        for (prefixes) |p| {
            if (std.mem.startsWith(u8, rest, p)) {
                const tail = rest[p.len..];
                var n: usize = 0;
                while (n < tail.len and isIdentChar(tail[n])) n += 1;
                if (n > 0) out.append(gpa, .{ .name = tail[0..n], .kind = .value }) catch {};
                break;
            }
        }
    }
}

fn collectIdents(text: []const u8, set: *std.StringHashMapUnmanaged(void)) void {
    var i: usize = 0;
    while (i < text.len) {
        if (isIdentChar(text[i]) and !(text[i] >= '0' and text[i] <= '9')) {
            var j = i + 1;
            while (j < text.len and isIdentChar(text[j])) j += 1;
            if (j - i >= 2 and j - i <= 64) set.put(gpa, text[i..j], {}) catch {};
            i = j;
        } else i += 1;
    }
}

/// 在 files 里找 `const BASE = struct/union/enum …{`，提取成员
fn containerMembers(files: []const []const u8, base: []const u8, fields: *std.ArrayList(Decl)) bool {
    var pat: std.ArrayList(u8) = .empty;
    defer pat.deinit(gpa);
    pat.appendSlice(gpa, "const ") catch return false;
    pat.appendSlice(gpa, base) catch return false;
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
            extractBraceMembers(ftext[p..], is_enum, fields);
            return true;
        }
    }
    return false;
}

fn extractBraceMembers(text: []const u8, is_enum: bool, out: *std.ArrayList(Decl)) void {
    var depth: usize = 0;
    var i: usize = 1; // text[0] == '{'
    while (i < text.len) : (i += 1) {
        const c = text[i];
        if (c == '{') depth += 1;
        if (c == '}') {
            if (depth == 0) return;
            depth -= 1;
        }
        if (depth == 0 and isIdentChar(c) and !(c >= '0' and c <= '9')) {
            var j = i + 1;
            while (j < text.len and isIdentChar(text[j])) j += 1;
            var k = j;
            while (k < text.len and (text[k] == ' ' or text[k] == '\t')) k += 1;
            var kind: ?DeclKind = null;
            if (!is_enum and k < text.len and text[k] == ':') kind = .field;
            if (is_enum and k < text.len and (text[k] == ',' or text[k] == '}' or text[k] == '=' or text[k] == ':'))
                kind = .variant;
            if (kind) |kd| {
                // 前一个有效字符须是块起点/逗号/换行，避免收进类型里的 ident
                var b = i;
                while (b > 1 and (text[b - 1] == ' ' or text[b - 1] == '\t' or text[b - 1] == '\n')) b -= 1;
                const prev: u8 = if (b >= 2) text[b - 1] else '{';
                if (prev == '{' or prev == ',' or prev == '\n' or prev == ';')
                    out.append(gpa, .{ .name = text[i..j], .kind = kd }) catch {};
            }
            i = j - 1;
        }
    }
}

/// module("…") 字符串 → 模块基名（去 .zx 后缀）。编译器两种形式都收：
/// 裸名 "std"（解析 <d>/std.zx 或 <d>/std/std.zx）与路径 "std/str.zx"。
fn moduleBase(m: []const u8) []const u8 {
    if (std.mem.endsWith(u8, m, ".zx")) return m[0 .. m.len - 3];
    return m;
}

/// mod 名 → doc 目录 / root 下的 `<mod>.zx` 或 `<mod>/*.zx` 顶层声明
fn moduleDecls(doc_dir: []const u8, mod: []const u8, out: *std.ArrayList(Decl)) void {
    const m = moduleBase(mod);
    var dirs: [2][]const u8 = undefined;
    var ndirs: usize = 0;
    dirs[ndirs] = doc_dir;
    ndirs += 1;
    if (root_path) |r| {
        if (!std.mem.eql(u8, r, doc_dir)) {
            dirs[ndirs] = r;
            ndirs += 1;
        }
    }
    for (dirs[0..ndirs]) |d| {
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

fn workspaceIdents(doc_dir: []const u8, set: *std.StringHashMapUnmanaged(void)) void {
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
    for (dirs[0..nd]) |d| {
        if (searched.contains(d)) continue;
        searched.put(gpa, d, {}) catch {};
        scanDirIdents(d, set, 0);
    }
}

fn scanDirIdents(dir_path: []const u8, set: *std.StringHashMapUnmanaged(void), depth: usize) void {
    if (depth > 2) return;
    var dir = Io.Dir.cwd().openDir(g_io, dir_path, .{ .iterate = true }) catch return;
    defer dir.close(g_io);
    var it = dir.iterate();
    while (it.next(g_io) catch null) |entry| {
        switch (entry.kind) {
            .directory => {
                if (entry.name.len > 0 and (entry.name[0] == '.' or
                    std.mem.eql(u8, entry.name, "zig-cache") or
                    std.mem.eql(u8, entry.name, "bin") or
                    std.mem.eql(u8, entry.name, ".tmp"))) continue;
                var sub: std.ArrayList(u8) = .empty;
                defer sub.deinit(gpa);
                sub.appendSlice(gpa, dir_path) catch continue;
                sub.append(gpa, '/') catch continue;
                sub.appendSlice(gpa, entry.name) catch continue;
                scanDirIdents(sub.items, set, depth + 1);
            },
            .file => {
                if (!std.mem.endsWith(u8, entry.name, ".zx")) continue;
                var fp: std.ArrayList(u8) = .empty;
                defer fp.deinit(gpa);
                fp.appendSlice(gpa, dir_path) catch continue;
                fp.append(gpa, '/') catch continue;
                fp.appendSlice(gpa, entry.name) catch continue;
                if (readFileSmall(fp.items)) |txt| collectIdents(txt, set);
            },
            else => {},
        }
    }
}

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

/// 行内找 `[pub ](const|let|var|type) word` 声明；返回 0-based 行/列
fn findDeclLine(text: []const u8, word: []const u8) ?struct { line: u32, col: u32 } {
    var ln: u32 = 0;
    var it = std.mem.splitScalar(u8, text, '\n');
    while (it.next()) |line_raw| : (ln += 1) {
        var ci: usize = 0;
        while (ci < line_raw.len and (line_raw[ci] == ' ' or line_raw[ci] == '\t')) ci += 1;
        if (line_raw.len - ci >= 4 and std.mem.eql(u8, line_raw[ci .. ci + 4], "pub ")) ci += 4;
        while (ci < line_raw.len and (line_raw[ci] == ' ' or line_raw[ci] == '\t')) ci += 1;
        const rest = line_raw[ci..];
        const kws = [_][]const u8{ "const", "let", "var", "type" };
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

/// `const word = module("X")` → X
fn moduleOfBinding(text: []const u8, word: []const u8) ?[]const u8 {
    var pat: std.ArrayList(u8) = .empty;
    defer pat.deinit(gpa);
    pat.appendSlice(gpa, "const ") catch return null;
    pat.appendSlice(gpa, word) catch return null;
    pat.appendSlice(gpa, " = module(\"") catch return null;
    const hit = std.mem.indexOf(u8, text, pat.items) orelse return null;
    const rest = text[hit + pat.items.len ..];
    const e = std.mem.indexOfScalar(u8, rest, '"') orelse return null;
    return rest[0..e];
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

/// 模块名 → 真实文件路径（<dir>/<m>.zx 或 <dir>/<m>/<m>.zx）
fn resolveModuleFile(doc: *const Document, m_raw: []const u8) ?[]const u8 {
    const m = moduleBase(m_raw);
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
    for (dirs[0..nd]) |d| {
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

/// 工作区补全上限：全仓库标识符再多也不能淹没列表/撑爆报文
const MAX_WORKSPACE_COMPLETIONS = 250;

fn isKeywordWord(w: []const u8) bool {
    for (keywords) |kw| {
        if (std.mem.eql(u8, kw, w)) return true;
    }
    return false;
}

fn addIdents(list: *Json, seen: *std.StringHashMapUnmanaged(void), doc_text: []const u8, doc_dir: []const u8, prefix: []const u8) void {
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
    // 工作区（tier 3，带上限）
    var ws: std.StringHashMapUnmanaged(void) = .empty;
    defer ws.deinit(gpa);
    workspaceIdents(doc_dir, &ws);
    var count: usize = 0;
    it = ws.keyIterator();
    while (it.next()) |k| {
        if (count >= MAX_WORKSPACE_COMPLETIONS) break;
        if (isKeywordWord(k.*)) continue;
        if (prefix.len == 0 or std.mem.startsWith(u8, k.*, prefix)) {
            addItem(list, seen, k.*, 6, "", 3);
            count += 1;
        }
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

    const dotted = off > 0 and text[off - 1] == '.' and (full_prefix.len == 0);

    if (dotted) {
        // 取 '.' 之前的完整路径（可含多级点：std / std.heap / obj.field）
        const base_end = off - 1;
        var base_start = base_end;
        while (base_start > 0 and (isIdentChar(text[base_start - 1]) or text[base_start - 1] == '.')) base_start -= 1;
        const path = text[base_start..base_end];

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
            // std 顶层：编译器 seed 的名字（println/heap/io/…，不在 std.zx 文本里）
            if (std.mem.eql(u8, path, "std")) {
                for (std_seeded) |m| addItem(&list, &seen, m, 3, "std 编译器内建", 0);
            }

            // module 绑定？const <path> = module("…")
            var mod: ?[]const u8 = null;
            {
                var pat: std.ArrayList(u8) = .empty;
                defer pat.deinit(gpa);
                pat.appendSlice(gpa, "const ") catch {};
                pat.appendSlice(gpa, path) catch {};
                pat.appendSlice(gpa, " = module(\"") catch {};
                if (std.mem.indexOf(u8, text, pat.items)) |hit| {
                    const rest = text[hit + pat.items.len ..];
                    const e = std.mem.indexOfScalar(u8, rest, '"') orelse rest.len;
                    if (e > 0) mod = rest[0..e];
                }
            }
            var decls: std.ArrayList(Decl) = .empty;
            defer decls.deinit(gpa);
            if (mod) |m| {
                moduleDecls(doc_dir, m, &decls);
                handled = decls.items.len > 0;
            }
            if (!handled) {
                // 容器成员：用路径最后一段匹配容器名
                var container = path;
                if (std.mem.lastIndexOfScalar(u8, path, '.')) |dot| container = path[dot + 1 ..];

                var files: std.ArrayList([]const u8) = .empty;
                defer files.deinit(gpa);
                files.append(gpa, text) catch {};
                var d = Io.Dir.cwd().openDir(g_io, doc_dir, .{ .iterate = true }) catch null;
                if (d) |*dd| {
                    defer dd.close(g_io);
                    var it = dd.iterate();
                    while (it.next(g_io) catch null) |entry| {
                        if (entry.kind != .file or !std.mem.endsWith(u8, entry.name, ".zx")) continue;
                        if (std.mem.eql(u8, entry.name, baseName(doc.path))) continue;
                        var fp: std.ArrayList(u8) = .empty;
                        defer fp.deinit(gpa);
                        fp.appendSlice(gpa, doc_dir) catch continue;
                        fp.append(gpa, '/') catch continue;
                        fp.appendSlice(gpa, entry.name) catch continue;
                        if (readFileSmall(fp.items)) |txt| files.append(gpa, txt) catch {};
                    }
                }
                if (containerMembers(files.items, container, &decls)) handled = decls.items.len > 0;
            }
            if (handled) {
                for (decls.items) |dcl| {
                    const k: u32 = switch (dcl.kind) {
                        .field => 5,
                        .variant => 20,
                        .value => 6,
                    };
                    addItem(&list, &seen, dcl.name, k, path, 0);
                }
            } else if (!std.mem.eql(u8, path, "std")) {
                // 未知容器：不给工作区噪音，宁可空列表
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
        // 当前文件标识符 + 关键字 + 工作区标识符（分层排序）
        addIdents(&list, &seen, text, doc_dir, prefix);
        for (keywords) |kw| {
            if (prefix.len == 0 or std.mem.startsWith(u8, kw, prefix))
                addItem(&list, &seen, kw, 14, "关键字", 2);
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

fn runCheck(doc: *const Document, out_diags: *std.ArrayList(Diag)) void {
    // 影子文件：把缓冲内容写到与源文件同目录的隐藏 .zx，zxc 检查它——
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
    }) catch return;
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);

    var it = std.mem.splitScalar(u8, res.stderr, '\n');
    while (it.next()) |line_raw| {
        const line = std.mem.trimEnd(u8, line_raw, "\r");
        const marker = std.mem.indexOf(u8, line, ": error[") orelse continue;
        var seg = line[0..marker];
        const c1 = std.mem.lastIndexOfScalar(u8, seg, ':') orelse continue;
        const col = std.fmt.parseInt(u32, seg[c1 + 1 ..], 10) catch continue;
        seg = seg[0..c1];
        const c2 = std.mem.lastIndexOfScalar(u8, seg, ':') orelse continue;
        const ln = std.fmt.parseInt(u32, seg[c2 + 1 ..], 10) catch continue;
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
        j.buf.appendSlice(gpa, "{\"range\":{\"start\":{\"line\":") catch @panic("oom");
        j.raw("{d},\"character\":{d}", .{ d.line, d.col });
        j.buf.appendSlice(gpa, "},\"end\":{\"line\":") catch @panic("oom");
        j.raw("{d},\"character\":{d}", .{ d.line, d.col + 1 });
        j.buf.appendSlice(gpa, "}},\"severity\":1,\"code\":") catch @panic("oom");
        j.str(d.code);
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
            }
        }
        var j = Json{};
        j.raw("{{\"jsonrpc\":\"2.0\",\"id\":", .{});
        writeId(&j, id);
        j.buf.appendSlice(gpa, ",\"result\":{\"capabilities\":{\"textDocumentSync\":{\"openClose\":true,\"change\":1},\"completionProvider\":{\"triggerCharacters\":[\".\",\"@\"]},\"definitionProvider\":true}}}") catch @panic("oom");
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
