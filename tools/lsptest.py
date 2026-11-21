# zxls 冒烟测试：stdio LSP 会话模拟
import subprocess, json, os, sys, shutil, threading, queue, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # 仓库根（脚本在 tools/ 下）
TESTDIR = os.path.join(ROOT, ".tmp", "lsptest")
os.makedirs(TESTDIR, exist_ok=True)

# 被测的 zxls：测试脚本不假设 bin/ 里那份是最新的（Zed 可能正锁着它），
# 用 ZXLS_BIN 指到刚编出来的临时 exe。
ZXLS_BIN = os.environ.get("ZXLS_BIN") or os.path.join(ROOT, "bin", "zxls-new.exe")

MAIN = """const std = @mod("std");

const Obj = struct {
    count: i32,
    name: []const u8,

    // bump
    const bump = fn (self: *Self) i32 {
        self.count = self.count + 1;
        return self.count;
    };
};

pub const make = fn () Obj {
    return Obj{ .count = 0, .name = "x" };
};

const Shape = union {
    circle: f64,
    rect: {f64, f64},
};

pub const main = fn () i32 {
    let o = make();
    let n = o.count;
    var s = Shape{ .circle = 1.0 };
    push(.{ .a = 1010, .msg = "hi" });
    let t = std.task.Task.single();
    let g = std.task.Task.group();
    g.wait();
    g.close();
    t.close();
    std.println("{}", n);
    return 0;
};

// 匿名 struct 当参数类型：调用点 `.{ …` 要能补出字段（3d 用例）
const push = fn (info: struct{a: i32, msg: []const u8}) {
    let i = info.a;
    std.println("push:{}", i);
};

const IOErr = error{ open, read };

const AllErr = error{ IOErr, extra };

const probe = AllErr.read;

const thrower = fn (b: bool) IOErr!i32 {
    if (b) { return IOErr.open; }
    return error.read;
};

// 字面量续补：'{ ' 后无空格 + 已有字段（3l 用例）
const lit2 = fn () i32 {
    var o2 = Obj{.count = 1, .name = "x"};
    std.println("{} {}", o2.count, o2.name);
    return 0;
};

// 匿名类型线（3v/3w/3x/3y/3z 守门）：匿名绑定、嵌套匿名、匿名参数第二位、嵌套实参
const nest = fn () i32 {
    let anon = .{ .x = 1, .y = "s" };
    let nv = .{ .pos = .{ .x = 1, .y = 2 }, .tag = "t" };
    let tp = { 1, 2.5 };
    conf("n", .{ .depth = 2, .kind = "k" });
    deep(.{ .opts = .{ .width = 1, .height = 2 }, .flag = true });
    std.println("{} {} {}", anon.x, nv.pos.x, tp.0);
    let sn1 = an;
    return 0;
};

const conf = fn (name: []const u8, opts: struct{ depth: i32, kind: []const u8 }) void {
    std.println("{} {} {}", name, opts.depth, opts.kind);
};

const deep = fn (cfg: struct{ opts: struct{ width: i32, height: i32 }, flag: bool }) void {
    std.println("{} {}", cfg.opts.width, cfg.flag);
};
"""

uri = "file:///" + TESTDIR.replace("\\", "/") + "/main.zx"

# ---- vendor/ 与 deps 的模块（补全只能走编译器的同一条解析链）-----------------
SRC_DIR = os.path.join(TESTDIR, "src")
os.makedirs(SRC_DIR, exist_ok=True)
os.makedirs(os.path.join(TESTDIR, "vendor"), exist_ok=True)
os.makedirs(os.path.join(TESTDIR, "libs"), exist_ok=True)
# 真 std 目录拷到 <exe>/../std（即 .tmp/std）：3e/3f 用例依赖 LSP 与编译器
# 同一条 std 解析链（find_std_path / findStdDir），仓库外测试工程够不着
# 仓库的 std/，必须在 exe 旁备一份。
# KEEP: 每次运行都覆盖——旧版按"task.zx 存在就跳过"只拷一次，仓库 std 改了
# 之后 .tmp/std 一直是旧副本（还带着已删除的类型别名），LSP 用例对着旧 std
# 跑，用户看到的就是"std 像旧版本"（2026-11-09）。
STD_COPY = os.path.join(ROOT, ".tmp", "std")
os.makedirs(STD_COPY, exist_ok=True)
for f in os.listdir(os.path.join(ROOT, "std")):
    if f.endswith(".zx"):
        shutil.copy(os.path.join(ROOT, "std", f), os.path.join(STD_COPY, f))
open(os.path.join(TESTDIR, "zx.json"), "w", encoding="utf-8").write(
    '{\n  "name": "lsptest",\n  "main": "src/extuser.zx",\n  "out": "lsptest",\n  "deps": ["libs"]\n}\n')
open(os.path.join(TESTDIR, "vendor", "ext.zx"), "w", encoding="utf-8").write(
    'pub const VERSION = "1.2.3";\n\npub const hello = fn (n: []const u8) void {\n    n;\n};\n\n'
    'pub const Cfg = struct {\n    pub w: i32,\n    pub h: i32,\n};\n')
open(os.path.join(TESTDIR, "libs", "util.zx"), "w", encoding="utf-8").write(
    'pub const twice = fn (x: i32) i32 {\n    return x * 2;\n};\n')
EXTUSE = """const ext = @mod("ext");
const util = @mod("util");

const main = fn () i32 {
    ext.hello("x");
    ext.
    util.twice(1);
    var c = ext.Cfg{ .w = 1 };
    c.
    ext.Cfg{.
    return 0;
};
"""
exturi = "file:///" + os.path.join(SRC_DIR, "extuser.zx").replace("\\", "/")
open(os.path.join(SRC_DIR, "extuser.zx"), "w", encoding="utf-8").write(EXTUSE)

proc = subprocess.Popen([ZXLS_BIN],
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
out_q = queue.Queue()

def reader():
    while True:
        headers = {}
        line = b""
        while True:
            c = proc.stdout.read(1)
            if not c:
                out_q.put(None); return
            line += c
            if line.endswith(b"\r\n"):
                if line.strip() == b"":
                    break
                k, _, v = line.decode().partition(":")
                headers[k.strip()] = v.strip()
                line = b""
        n = int(headers["Content-Length"])
        body = b""
        while len(body) < n:
            body += proc.stdout.read(n - len(body))
        try:
            out_q.put(json.loads(body))
        except json.JSONDecodeError as e:
            with open(os.path.join(ROOT, ".tmp", "badframe.txt"), "ab") as f:
                f.write(b"==== " + str(headers).encode() + b"\n" + body + b"\n")
            out_q.put({"__badframe__": True, "err": str(e)})

threading.Thread(target=reader, daemon=True).start()

# 服务器死掉的时候，光一句 "server died" 什么也查不出来——Zig 的 panic
# 是写在 stderr 上的。这里把 stderr 持续抽干，死的时候把尾巴打出来。
err_lines = []
def err_reader():
    while True:
        l = proc.stderr.readline()
        if not l:
            return
        err_lines.append(l.decode("utf-8", "replace").rstrip())
threading.Thread(target=err_reader, daemon=True).start()

def send(msg):
    data = json.dumps(msg).encode()
    proc.stdin.write(f"Content-Length: {len(data)}\r\n\r\n".encode() + data)
    proc.stdin.flush()
def recv_id(tag, want_id, timeout=30):
    """按 id 取应答；通知（无 id）跳过但保留诊断内容供检查。"""
    notifs = []
    import time as _t
    deadline = _t.time() + timeout
    while _t.time() < deadline:
        try:
            m = out_q.get(timeout=1)
        except queue.Empty:
            continue
        if m is None:
            print(f"FAIL {tag}: server died")
            for l in err_lines[-20:]:
                print("      | " + l)
            return None, notifs
        if m.get("__badframe__"):
            print(f"FAIL {tag}: bad frame {m['err']}"); return None, notifs
        if "id" not in m:
            notifs.append(m)
            continue
        if m["id"] == want_id:
            return m, notifs
        # 别的应答（乱序）也收下
    print(f"FAIL {tag}: timeout"); return None, notifs

def pos(text, off):
    line = text.count("\n", 0, off)
    ls = text.rfind("\n", 0, off) + 1
    return {"line": line, "character": off - ls}

fails = []
def check(tag, cond, detail=""):
    if cond: print(f"ok   {tag}")
    else:
        print(f"FAIL {tag} {detail}"); fails.append(tag)

# 1 initialize
send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
    "rootUri": "file:///" + TESTDIR.replace("\\", "/"),
    # codeDescriptionSupport：模拟 Zed/VS Code。不支持这个能力的客户端
    # 不该收到 codeDescription（老客户端严格校验会丢掉整条诊断）。
    "capabilities": {"textDocument": {"publishDiagnostics": {"codeDescriptionSupport": True}}}}})
r, _ = recv_id("initialize", 1)
caps = r["result"]["capabilities"] if r and "result" in r else {}
check("initialize.caps", all(k in caps for k in (
    "completionProvider", "definitionProvider", "hoverProvider",
    "signatureHelpProvider", "documentFormattingProvider",
    "documentSymbolProvider", "referencesProvider")), str(sorted(caps.keys())))
# §193 补丁：编辑触发符——逗号重新弹补全（用户 Ctrl+J 手动唤回的场景）、
# 括号/逗号出签名且逗号可 retrigger（编辑实参时签名持续在屏）。
check("initialize.caps.triggers",
      "," in (caps.get("completionProvider") or {}).get("triggerCharacters", [])
      and set(["(", ")", ","]) <= set((caps.get("signatureHelpProvider") or {}).get("triggerCharacters", []))
      and "," in (caps.get("signatureHelpProvider") or {}).get("retriggerCharacters", []),
      json.dumps(caps.get("completionProvider")) + " / " + json.dumps(caps.get("signatureHelpProvider")))

send({"jsonrpc": "2.0", "method": "initialized", "params": {}})

# 2 didOpen
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": uri, "languageId": "zx", "version": 1, "text": MAIN}}})

# 3 completion `o.` → count/name/bump（实例推断 + 方法捕获）
off = MAIN.index("o.count") + 2          # 光标在 '.' 之后的 'c' 上
send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("completion.o", 2)
items = r["result"]["items"] if r and r.get("result") else []
labels = {i["label"] for i in items}
check("completion.o.members", {"count", "name", "bump"} <= labels, str(labels))
# GUARD: 字段候选右侧说明必须是声明类型，不能是容器名/实例名（截图反馈线）
details = {i["label"]: i.get("detail") for i in items}
check("completion.o.fieldType", details.get("count") == "i32"
      and details.get("name") == "[]const u8", str(details))

# 3b 字面量成员补全：`Obj{ .` → 只要字段，方法（bump）不出
off = MAIN.index("Obj{ .count") + len("Obj{ .")   # 光标贴在 '.' 之后
send({"jsonrpc": "2.0", "id": 9, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("completion.literal", 9)
labels = {i["label"] for i in r["result"]["items"]} if r and r.get("result") else set()
check("completion.literal.obj", {"count", "name"} <= labels and "bump" not in labels, str(sorted(labels)))
details = {i["label"]: i.get("detail") for i in r["result"]["items"]} if r and r.get("result") else {}
check("completion.literal.fieldType", details.get("count") == "i32"
      and details.get("name") == "[]const u8" and "Obj" not in details.values(), str(details))

# 3c union 字面量：`Shape{ .` → 变体（enum/union 成员）
off = MAIN.index("Shape{ .circle") + len("Shape{ .")
send({"jsonrpc": "2.0", "id": 10, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("completion.literal.union", 10)
labels = {i["label"] for i in r["result"]["items"]} if r and r.get("result") else set()
check("completion.literal.union", {"circle", "rect"} <= labels, str(sorted(labels)))
details = {i["label"]: i.get("detail") for i in r["result"]["items"]} if r and r.get("result") else {}
# {f64, f64} 里的逗号不能被当成分隔符截断
check("completion.literal.variantType", details.get("circle") == "f64"
      and details.get("rect") == "{f64, f64}", str(details))

# 3d 匿名 struct 参数：push(.{ . → 字段来自签名里的 struct{…} 形参
off = MAIN.index("push(.{ .a") + len("push(.{ .")
send({"jsonrpc": "2.0", "id": 11, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("completion.anon", 11)
labels = {i["label"] for i in r["result"]["items"]} if r and r.get("result") else set()
check("completion.anon.fields", {"a", "msg"} <= labels, str(sorted(labels)))
details = {i["label"]: i.get("detail") for i in r["result"]["items"]} if r and r.get("result") else {}
check("completion.anon.fieldType", details.get("a") == "i32"
      and details.get("msg") == "[]const u8", str(details))

# 3v 匿名 struct 绑定：`anon.` → 字段来自 .{ … } 推出的匿名类型
#     （GUARD：匿名类型线的核心——hover 有类型、点补全也必须有候选）
off = MAIN.index("anon.x") + len("anon.")
send({"jsonrpc": "2.0", "id": 30, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("completion.anonBind", 30)
labels = {i["label"] for i in r["result"]["items"]} if r and r.get("result") else set()
check("completion.anonBind.fields", {"x", "y"} <= labels, str(sorted(labels)))
details = {i["label"]: i.get("detail") for i in r["result"]["items"]} if r and r.get("result") else {}
check("completion.anonBind.fieldType", details.get("x") == "i32"
      and details.get("y") == "str", str(details))

# 3w 嵌套匿名类型：`nv.pos.` → 内层匿名 struct 的字段
off = MAIN.index("nv.pos.x") + len("nv.pos.")
send({"jsonrpc": "2.0", "id": 31, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("completion.anonNest", 31)
labels = {i["label"] for i in r["result"]["items"]} if r and r.get("result") else set()
details = {i["label"]: i.get("detail") for i in r["result"]["items"]} if r and r.get("result") else {}
check("completion.anonNest.fields", {"x", "y"} <= labels, str(sorted(labels)))
check("completion.anonNest.fieldType", details.get("x") == "i32", str(details))

# 3w2 嵌套匿名的行内类型提示 + 元组形状（{1, 2.5} 是 {i32,f64}；§146 起 .{…} 不再是元组）
send({"jsonrpc": "2.0", "id": 32, "method": "textDocument/inlayHint", "params": {
    "textDocument": {"uri": uri},
    "range": {"start": {"line": 0, "character": 0},
              "end": {"line": MAIN.count("\n"), "character": 0}}}})
r, _ = recv_id("inlayHint.anonNest", 32)
atypes = [h["label"] for h in (r["result"] or []) if h.get("kind") == 1] if r else []
check("inlayHint.type.anonNest",
      ": struct{pos:struct{x:i32,y:i32},tag:str}" in atypes, str(atypes))
check("inlayHint.type.anonTuple", ": {i32,f64}" in atypes, str(atypes))

# 3x 匿名参数第二位：conf("n", .{ … 补第二个 struct 形参的字段
#     （GUARD：旧实现只认第一个 struct 形参，这里钉死按实参位置对齐）
off = MAIN.index('conf("n", .{ .depth') + len('conf("n", .{ .')
send({"jsonrpc": "2.0", "id": 33, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("completion.anonArg2", 33)
labels = {i["label"] for i in r["result"]["items"]} if r and r.get("result") else set()
details = {i["label"]: i.get("detail") for i in r["result"]["items"]} if r and r.get("result") else {}
check("completion.anonArg2.fields", {"depth", "kind"} <= labels, str(sorted(labels)))
check("completion.anonArg2.fieldType", details.get("depth") == "i32"
      and details.get("kind") == "[]const u8", str(details))

# 3y 嵌套实参字段链：deep(.{ .opts = .{ . → 内层 struct 形参的字段
off = MAIN.index("deep(.{ .opts = .{ .width") + len("deep(.{ .opts = .{ .")
send({"jsonrpc": "2.0", "id": 34, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("completion.anonDeep", 34)
labels = {i["label"] for i in r["result"]["items"]} if r and r.get("result") else set()
check("completion.anonDeep.fields", {"width", "height"} <= labels, str(sorted(labels)))

# 3z 嵌套成员 hover：nv.pos.x → x: i32
off = MAIN.index("nv.pos.x") + len("nv.pos.")
send({"jsonrpc": "2.0", "id": 35, "method": "textDocument/hover", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("hover.anonNest", 35)
hv = r["result"]["contents"]["value"] if r and r.get("result") else ""
check("hover.anonNest", "x: i32" in hv, hv[:120])

# 3za snippet：匿名 struct 字面量模板（输入 an → anon）
off = MAIN.index("let sn1 = an;") + len("let sn1 = an")
send({"jsonrpc": "2.0", "id": 36, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("completion.anonSnippet", 36)
smap = {i["label"]: i for i in r["result"]["items"]} if r and r.get("result") else {}
check("completion.snippet.anon", smap.get("anon", {}).get("insertTextFormat") == 2
      and smap.get("anon", {}).get("insertText") == ".{ .${1:field} = ${2:0} }$0",
      str(smap.get("anon"))[:200])

# 3q LSP snippet 补全：写 `if` / `fn` + Tab 展开骨架
#     GUARD: 这几条把模板内容钉死——4 空格缩进、块尾 `;`、tabstop，
#     改坏或删掉 insertTextFormat 会连挂。用 MAIN 里已有关键字当光标点，
#     不换文档，避免 didChange 的异步诊断污染后面的用例。
def snip_at(anchor, word, rid):
    off = MAIN.index(anchor) + anchor.index(word) + len(word)
    send({"jsonrpc": "2.0", "id": rid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
    r, _ = recv_id("completion.snippet", rid)
    items = r["result"]["items"] if r and r.get("result") else []
    return {i["label"]: i for i in items}

s_if = snip_at("if (b)", "if", 24)
i_if = s_if.get("if", {})
check("completion.snippet.if", i_if.get("insertTextFormat") == 2
      and i_if.get("kind") == 15
      and i_if.get("insertText") == "if (${1:cond}) {\n    $0\n};", str(i_if)[:200])
check("completion.snippet.ifelse", s_if.get("ifelse", {}).get("insertTextFormat") == 2,
      str(sorted(s_if))[:200])

s_fn = snip_at("fn (b: bool)", "fn", 25)
check("completion.snippet.fn", s_fn.get("fn", {}).get("insertText") == "fn ($1) ${2:i32} {\n    $0\n};",
      str(s_fn.get("fn"))[:200])

s_st = snip_at("struct {", "struct", 27)
check("completion.snippet.struct", s_st.get("struct", {}).get("insertText")
      == "struct {\n    ${1:name}: ${2:i32},\n};", str(s_st.get("struct"))[:200])

s_main = snip_at("pub const main", "main", 26)
check("completion.snippet.main", s_main.get("main", {}).get("insertTextFormat") == 2,
      str(sorted(s_main))[:200])

s_pub = snip_at("pub const make", "pub", 28)
check("completion.snippet.plainKeyword",
      s_pub.get("pub", {}).get("insertTextFormat") is None
      and s_pub.get("pub", {}).get("kind") == 14, str(s_pub.get("pub"))[:200])

all_snips = [i for d in (s_if, s_fn, s_st) for i in d.values() if i.get("insertTextFormat") == 2]
check("completion.snippet.indent4",
      all("\n    " in i["insertText"] or "\n" not in i["insertText"] for i in all_snips),
      str([i["insertText"] for i in all_snips])[:240])
check("completion.snippet.semicolon",
      all(i["insertText"].endswith(";") for i in all_snips
          if i["label"] in ("if", "ifelse", "fn", "struct", "switch", "while", "for")),
      str([i["label"] for i in all_snips])[:200])

# 3r §213：@ 内建补全必须含 panic（带调用骨架）
off = MAIN.index("@mod") + 1     # 光标在 '@' 之后、标识符之前
send({"jsonrpc": "2.0", "id": 9001, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("completion.atBuiltins", 9001)
at_items = r["result"]["items"] if r and r.get("result") else []
at_map = {i["label"]: i for i in at_items}
check("completion.at.panic",
      "panic" in at_map and at_map["panic"].get("insertTextFormat") == 2
      and at_map["panic"].get("insertText") == 'panic("${1:message}")$0',
      str(at_map.get("panic"))[:200])

# 3q2 自定义类型骨架：`type` / `uniontag` / `enumtag` / `new`
#     用户 2026-11-08 追加：写自定义类型也要能 Tab 展开完整声明。
#     展开后是能直接编译的 `const Name = struct { … };` 整条声明；
#     实例构造走 `new`。
SNIP2 = """const std = @mod("std");
const t = type;
const u = union;
const e = enum;
const n = new;
"""
send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
    "textDocument": {"uri": uri, "version": 13},
    "contentChanges": [{"text": SNIP2}]}})

def snip2(word, rid):
    off = SNIP2.index("= " + word) + 2 + len(word)
    send({"jsonrpc": "2.0", "id": rid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": uri}, "position": pos(SNIP2, off)}})
    r, _ = recv_id("completion.snippet2", rid)
    items = r["result"]["items"] if r and r.get("result") else []
    return {i["label"]: i for i in items}

s2t = snip2("type", 91)
check("completion.snippet.type",
      s2t.get("type", {}).get("insertText")
      == "const ${1:Name} = struct {\n    ${2:field}: ${3:i32},\n};", str(s2t.get("type"))[:200])
s2u = snip2("union", 92)
check("completion.snippet.uniontag",
      s2u.get("uniontag", {}).get("insertText")
      == "const ${1:Name} = union(enum) {\n    ${2:a}: ${3:i32},\n};", str(s2u.get("uniontag"))[:200])
check("completion.snippet.unionOrder",
      s2u.get("union", {}).get("sortText", "") < s2u.get("uniontag", {}).get("sortText", "~"),
      str({k: v.get("sortText") for k, v in s2u.items()})[:200])
s2e = snip2("enum", 93)
check("completion.snippet.enumtag",
      s2e.get("enumtag", {}).get("insertText")
      == "const ${1:Name} = enum(u8) { ${2:a}, ${3:b} };", str(s2e.get("enumtag"))[:200])
s2n = snip2("new", 94)
check("completion.snippet.new",
      s2n.get("new", {}).get("insertText") == "const ${1:v} = ${2:Name}{ .${3:field} = $0 };",
      str(s2n.get("new"))[:200])
check("completion.snippet.typeShape",
      all(d.get(c, {}).get("kind") == 15 and d.get(c, {}).get("insertTextFormat") == 2
          for d, c in ((s2t, "type"), (s2u, "uniontag"), (s2e, "enumtag"), (s2n, "new"))),
      str([s2t.get("type"), s2u.get("uniontag"), s2e.get("enumtag"), s2n.get("new")])[:260])
send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
    "textDocument": {"uri": uri, "version": 14},
    "contentChanges": [{"text": MAIN}]}})

# 3e std.task.Task 静态面（§129 / b620057 形态）：类型命名空间只允许
# 4 个静态构造器 + 2 个编译期分支常量 + `group` 工厂（返回 Group 实例）。
# 任务能力全是实例方法，不许出现在 `Task.`；group* 已从实例上删去。
# `pub const K: T = v` 正确归为关联常量、方法（首参 self）不进类型空间、
# 形参名 f/ctx/h/v 不混入（extractBraceMembers 圆括号深度守卫）。
off = MAIN.index("std.task.Task.") + len("std.task.Task.")
send({"jsonrpc": "2.0", "id": 12, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("completion.stdTask", 12)
labels = {i["label"] for i in r["result"]["items"]} if r and r.get("result") else set()
check("completion.stdTask",
      labels == {"coro", "single", "pool", "fromId",
                 "SEL_AWAIT", "SEL_RECV", "group"}
      and not labels & {"ctx", "f", "h", "v", "spawn", "await",
                        "groupSpawn", "groupWait", "groupClose",
                        "groupRun", "wait", "run", "close"}, str(sorted(labels)))

# 3f 统一对外接口（§123/§125/§128/§129）：t. 只留任务调度、默认通道、
# select 与 close。内核线程组改由 Task.group() 工厂生成 Group 实例，
# group* 一律不进实例命名空间；静态构造器与编译期常量不得混入，
# 被删的 scope/chan/timer 不许再出现。
off = MAIN.index("    t.close();") + len("    t.")
send({"jsonrpc": "2.0", "id": 13, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("completion.unifiedApi", 13)
labels = {i["label"] for i in r["result"]["items"]} if r and r.get("result") else set()
check("completion.unifiedApi",
      {"spawn", "await", "awaitAll", "cancel", "yield",
       "send", "recv", "trySend", "tryRecv", "close", "select"} <= labels
      and not labels & {"coro", "single", "pool", "fromId",
                        "SEL_AWAIT", "SEL_RECV", "group", "groupSpawn",
                        "groupWait", "groupClose", "groupRun"},
      str(sorted(labels)))

# §128：scope / chan / timer 不做对外 API，t. 上不许再冒出这些名字。
_removed = {"scope", "scopeSpawn", "scopeJoin", "scopeJoinTimeout",
            "scopeClose", "withScope", "chan", "chanSend", "chanRecv",
            "chanTrySend", "chanTryRecv", "chanClose", "sleep",
            "awaitTimeout"}
check("completion.unifiedApi.noRemoved",
      not (_removed & labels), str(sorted(_removed & labels)))

# §129：Task.group() 工厂生成 Group 实例，g. 必须补出 Group 自己的方法，
# 且不能混入 Task 的静态面或实例方法（命名空间三分对新类型同样成立）。
off = MAIN.index("    g.wait();") + len("    g.")
send({"jsonrpc": "2.0", "id": 998, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("completion.groupInstance", 998)
glabels = {i["label"] for i in r["result"]["items"]} if r and r.get("result") else set()
check("completion.groupInstance.methods",
      {"spawn", "wait", "run", "close"} <= glabels
      and not ({"group", "SEL_AWAIT", "coro", "await", "select"} & glabels),
      str(sorted(glabels)))

# 3f2 同名成员跨容器不串签名（用户 2026-11-10 截图：实例上"显示关联函数"）：
#   `Task.spawn` 与 `Group.spawn` 同名。t. 的 detail 必须是 Task 的 `?usize` 版，
#   不能是 Group 的 `bool` 版——以前 findDeclStmt 在整份声明文件里按名字找，
#   拿到的是文件里第一个（Group.spawn，std/task.zx 里在前）。
off = MAIN.index("    t.close();") + len("    t.")
send({"jsonrpc": "2.0", "id": 991, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("completion.instance.sameNameMember", 991)
tmap = {i["label"]: i.get("detail") for i in r["result"]["items"]} if r and r.get("result") else {}
check("completion.instance.sameNameMember",
      tmap.get("spawn") == "spawn(self: Task, f: fn (usize) !usize, ctx: usize) ?usize"
      and tmap.get("awaitAll") == "awaitAll(self: Task) !void"
      and tmap.get("close") == "close(self: Task) void"
      # 关联函数（首参不是 self）一律不进实例补全。§129 起 group 是
      # Task. 上的工厂，group* 已从实例命名空间删去。
      and not ({"coro", "single", "pool", "fromId", "group",
                "groupSpawn", "groupWait", "groupClose",
                "groupRun"} & set(tmap)),
      str(tmap)[:300])

# 3g hover 变量 → 推断类型（var p = Person{…} 式结构体字面量也要出类型）
off = MAIN.index("var s = Shape") + len("var s")
send({"jsonrpc": "2.0", "id": 14, "method": "textDocument/hover", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("hover.varType", 14)
hv = r["result"]["contents"]["value"] if r and r.get("result") else ""
check("hover.varType", "var s: Shape = " in hv, repr(hv[:80]))

# 3h hover 容器名 → Zig 式 `: type`
off = MAIN.index("Shape{ .circle")     # 停在类型名上
send({"jsonrpc": "2.0", "id": 15, "method": "textDocument/hover", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("hover.containerType", 15)
hv = r["result"]["contents"]["value"] if r and r.get("result") else ""
check("hover.containerType", ": type = struct" in hv or ": type = union" in hv, repr(hv[:80]))

# 3i hover 函数形参 → `b: bool`（namedTypeNear 的 `name: Type` 形状扫描）
off = MAIN.index("fn (b: bool)") + len("fn (b")
send({"jsonrpc": "2.0", "id": 16, "method": "textDocument/hover", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("hover.param", 16)
hv = r["result"]["contents"]["value"] if r and r.get("result") else ""
check("hover.param", "b: bool" in hv, repr(hv[:80]))

# 3j hover 结构体字段声明 → `count: i32`
off = MAIN.index("    count: i32,") + len("    count")
send({"jsonrpc": "2.0", "id": 17, "method": "textDocument/hover", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("hover.field", 17)
hv = r["result"]["contents"]["value"] if r and r.get("result") else ""
check("hover.field", "count: i32" in hv, repr(hv[:80]))

# 3k hover 成员表达式右值 → 字段类型穿透（const n = o.count → i32，
# 不得把 "o.count" 整串当类型名）
off = MAIN.index("let n = o.count;") + len("let n")
send({"jsonrpc": "2.0", "id": 18, "method": "textDocument/hover", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("hover.memberRhs", 18)
hv = r["result"]["contents"]["value"] if r and r.get("result") else ""
check("hover.memberRhs", "let n: i32 = " in hv and "o.count" not in hv.split("\n")[1].split("=")[0], repr(hv[:80]))

# 3l 字面量续补：Obj{.count = 1, . → 跨过已有字段找到 '{'（'{ ' 后无空格也认）
off = MAIN.index("var o2 = Obj{.count = 1, .") + len("var o2 = Obj{.count = 1, .")
send({"jsonrpc": "2.0", "id": 19, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("completion.literalSecond", 19)
labels = {i["label"] for i in r["result"]["items"]} if r and r.get("result") else set()
check("completion.literalSecond", {"count", "name"} <= labels and "bump" not in labels, str(sorted(labels)))

# 3m 字面量续补（带前缀）：Obj{.c → Zed 会带着已敲的 'c' 请求，字段仍要全量给
off = MAIN.index("var o2 = Obj{.count = 1, .n") + len("var o2 = Obj{.count = 1, .n")
send({"jsonrpc": "2.0", "id": 20, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("completion.literalSecondPrefix", 20)
labels = {i["label"] for i in r["result"]["items"]} if r and r.get("result") else set()
check("completion.literalSecondPrefix", "name" in labels, str(sorted(labels)))

# 4 completion `std.p`（带前缀点补全，旧版这里只会出关键字）
off = MAIN.rindex("std.p") + len("std.p")   # 光标在 p 后面
send({"jsonrpc": "2.0", "id": 3, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("completion.stdp", 3)
labels = {i["label"] for i in r["result"]["items"]} if r and r.get("result") else set()
check("completion.stdp.hasPrint", "println" in labels and "print" in labels, str(sorted(labels)))
check("completion.stdp.noKeywords", "return" not in labels, str(sorted(labels)))

# 5 documentSymbol → Obj / make / main
send({"jsonrpc": "2.0", "id": 4, "method": "textDocument/documentSymbol", "params": {
    "textDocument": {"uri": uri}}})
r, _ = recv_id("documentSymbol", 4)
syms = {s["name"]: s["kind"] for s in r["result"]} if r and r.get("result") else {}
check("symbol.Obj.struct", syms.get("Obj") == 23, str(syms))
check("symbol.make.fn", syms.get("make") == 12, str(syms))
check("symbol.main.fn", syms.get("main") == 12, str(syms))

# 6 references: count → 声明 + self.count + 字面量 .count（本文件 3 处）
off = MAIN.index("count: i32")           # 停在声明 'count' 上
send({"jsonrpc": "2.0", "id": 5, "method": "textDocument/references", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off + 1),
    "context": {"includeDeclaration": True}}})
r, _ = recv_id("references", 5)
locs = r["result"] if r and r.get("result") is not None else []
check("references.count", len(locs) >= 3, f"n={len(locs)}")

# 7 formatting：didChange 成乱格式再请求。zxc fmt 只规整缩进/尾空白/
# 空行（保守不动行内内容），所以断言按它的真实行为写：行内间距原样，
# 缩进补成 4 空格。
BADFMT = "pub fn    main() i32 {\n\tlet x=1;\nreturn      x;\n}\n"
send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
    "textDocument": {"uri": uri, "version": 2},
    "contentChanges": [{"text": BADFMT}]}})
send({"jsonrpc": "2.0", "id": 6, "method": "textDocument/formatting", "params": {
    "textDocument": {"uri": uri}, "options": {"tabSize": 4, "insertSpaces": False}}})
r, _ = recv_id("formatting", 6)
edits = r["result"] if r and r.get("result") is not None else None
ok_fmt = False
if edits is not None:
    if len(edits) == 1:
        nt = edits[0]["newText"]
        ok_fmt = "\n    let x=1;\n" in nt and "\n    return      x;\n" in nt and "\t" not in nt
    else:
        ok_fmt = len(edits) == 0  # 没变化=已格式化（不该发生，但也算合法应答）
check("formatting.zxcFmt", ok_fmt, str(edits)[:200] if edits is not None else "None")

# 8 definition: make() 调用处跳声明（换回好格式的内容）
send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
    "textDocument": {"uri": uri, "version": 3},
    "contentChanges": [{"text": MAIN}]}})
off = MAIN.index("make()") + 1
send({"jsonrpc": "2.0", "id": 7, "method": "textDocument/definition", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("definition", 7)
ok_def = bool(r and r.get("result"))
check("definition.make", ok_def, str(r)[:200])

# 10 vendor/ 与 deps 的模块补全：LSP 必须和编译器走同一条解析链
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": exturi, "languageId": "zx", "version": 1, "text": EXTUSE}}})

def complete_at(text, off, rid, doc_uri=None):
    send({"jsonrpc": "2.0", "id": rid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": doc_uri or exturi}, "position": pos(text, off)}})
    r, _ = recv_id("completion", rid)
    return {i["label"] for i in r["result"]["items"]} if r and r.get("result") else set()

off = EXTUSE.index("    ext.\n") + len("    ext.")      # 光标贴在 ext. 后面
labels = complete_at(EXTUSE, off, 20)
check("completion.vendor", {"hello", "VERSION"} <= labels, str(sorted(labels))[:200])

off = EXTUSE.index("ext.hello") + len("ext.h")          # 带前缀
labels = complete_at(EXTUSE, off, 21)
check("completion.vendorPrefix", "hello" in labels, str(sorted(labels))[:200])

off = EXTUSE.index("util.twice") + len("util.")          # zx.json deps
labels = complete_at(EXTUSE, off, 22)
check("completion.deps", "twice" in labels, str(sorted(labels))[:200])

# 10b 模块成员的 detail 必须是函数签名/声明类型，不能落到绑定别名
# （2026-11-08 截图：`g.` 弹窗每条右侧都标 `g`，看不出哪条是函数）。
# `pub const hello = fn …` 这种 const 包着的函数同样按函数出（kind=3）。
def vitems_at(text, off, rid):
    send({"jsonrpc": "2.0", "id": rid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": exturi}, "position": pos(text, off)}})
    r, _ = recv_id("completion", rid)
    return r["result"]["items"] if r and r.get("result") else []

off = EXTUSE.index("    ext.\n") + len("    ext.")
vitems = vitems_at(EXTUSE, off, 200)
vd = {i["label"]: i.get("detail") for i in vitems}
vk = {i["label"]: i.get("kind") for i in vitems}
check("completion.vendorFnDetail",
      (vd.get("hello") or "").startswith("hello(n: []const u8)"), str(vd)[:240])
vt = {i["label"]: (i.get("insertText") or "", i.get("insertTextFormat")) for i in vitems}
# 2026-11-09 第 13 条：函数只出一条（label 就是名字、kind=15 带 snippet
# 占位），不再并列一条 `name(...)`；"两条都要"只针对类型（类型项 +
# `Name{...}`）。截图 `createDir` / `createDir(...)` 就是那个多余项。
check("completion.vendorFnKind",
      vk.get("hello") == 15 and vk.get("hello(...)") is None, str(vk)[:160])
check("completion.vendorFnSnippet",
      vt.get("hello") == ("hello(${1:n})$0", 2), str(vt.get("hello")))

# 10c vendor 容器（struct）的字段也要查得到——原生实例/字面量走
# scanFiles，vendor 的容器体在模块文件里，必须按绑定解析（2026-11-08）。
off = EXTUSE.index("    c.\n") + len("    c.")
labels = complete_at(EXTUSE, off, 204)
check("completion.vendorStruct.instance", {"w", "h"} <= labels, str(sorted(labels))[:200])
off = EXTUSE.index("ext.Cfg{.") + len("ext.Cfg{.")
labels = complete_at(EXTUSE, off, 205)
check("completion.vendorStruct.literal", {"w", "h"} <= labels, str(sorted(labels))[:200])

# 3b 错误集补全（M64）：`Err.` 出集合成员；`error.` 出全局码名。
# 错误码名在编译器侧是全局 intern 的，文本层就把工程里见过的都端出来。
off = MAIN.index("IOErr.") + len("IOErr.")
labels = complete_at(MAIN, off, 30, doc_uri=uri)
check("completion.errsetMembers", {"open", "read"} <= labels, str(sorted(labels))[:200])

off = MAIN.index("return error.") + len("return error.")   # 光标贴在 error. 后
labels = complete_at(MAIN, off, 31, doc_uri=uri)
check("completion.errorMembers", "open" in labels and "read" in labels, str(sorted(labels))[:200])

# 3c 集合合并的补全：AllErr 引用了 IOErr，成员要展开（M64）
off = MAIN.index("AllErr.") + len("AllErr.")
labels = complete_at(MAIN, off, 32, doc_uri=uri)
check("completion.errsetMerge", {"open", "read", "extra"} <= labels, str(sorted(labels))[:200])

# 11 definition: ext.hello → vendor/ext.zx
off = EXTUSE.index("ext.hello") + len("ext.h")
send({"jsonrpc": "2.0", "id": 23, "method": "textDocument/definition", "params": {
    "textDocument": {"uri": exturi}, "position": pos(EXTUSE, off)}})
r, _ = recv_id("definition", 23)
loc = r.get("result") if r else None
target = ""
if isinstance(loc, dict):
    target = loc.get("uri", "")
elif isinstance(loc, list) and loc:
    target = loc[0].get("uri", "")
check("definition.vendor", "vendor" in target, str(loc)[:200])

# 10b 诊断带 help/note：改出个错，publishDiagnostics 的 message 里应有
#     did you mean（渲染器的 help 行被 zxls 挂进诊断消息）。
#     第二处是类型错误：那条没有具体建议，靠 errors.c 的通用修法兜底。
BADTXT = MAIN.replace("let n = o.count;", "let n = o.coutn;\n    let bad: i32 = \"x\";")
send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
    "textDocument": {"uri": uri, "version": 7},
    "contentChanges": [{"text": BADTXT}]}})
# 诊断由后台 worker 异步发布：轮询通知流，直到拿到该文档带诊断的发布。
import time as _t2
diag_msgs = []
diags = []
deadline = _t2.time() + 60
while _t2.time() < deadline:
    try:
        m = out_q.get(timeout=1)
    except queue.Empty:
        continue
    if m is None:
        break
    if m.get("__badframe__"):
        break
    if "id" in m:
        continue
    if (m.get("method") == "textDocument/publishDiagnostics"
            and m["params"].get("uri", "").endswith("main.zx")
            # 只认本次 didChange（version 7）的发布：此前几次 didChange 也
            # 各自触发过一次检查，它们的发布可能还排在队列里（MAIN 文本本身
            # 带 two 条 note 级诊断）。真实客户端同样按 version 丢弃过期诊断，
            # 这里照做，避免测试被调度顺序左右。
            and m["params"].get("version") == 7
            and m["params"].get("diagnostics")):
        diags = m["params"]["diagnostics"]
        diag_msgs = [d.get("message", "") for d in diags]
        break
ok_help = any("did you mean" in m for m in diag_msgs)
check("diagnostics.helpInMsg", ok_help, str(diag_msgs)[:300])

# 10c 诊断要带稳定的码 + 来源名，并且能点开文档：codeDescription.href
#     指向 docs/errors.md 的锚点（手册已删，错误码参考保留——用户 2026-11-10
#     明确要求"打印提示"留着）。
d304 = next((x for x in diags if x.get("code") == "E0304"), None)
href = ((d304 or {}).get("codeDescription") or {}).get("href", "")
check("diagnostics.codeDescription",
      d304 is not None and d304.get("source") == "zxc"
      and href.endswith("docs/errors.md#e0304"),
      href or str(diags)[:300])

# 10d 没有具体 fix-it 的诊断，消息里要带上该码的通用修法（errors.c 的文案）
ok_fix = any(d.get("code") == "E0301" and "(T)expr" in d.get("message", "") for d in diags)
check("diagnostics.genericFix", ok_fix, str(diag_msgs)[:300])

# 3n 行内类型提示（textDocument/inlayHint）：没写类型的绑定要出 `: Type`
#     GUARD: 3n/3o/3p 共 19 条是 inlayHint 能力线的守门断言，实现被删/被回退会在这里
#     连挂。改 zxls 时不要为了"让测试变绿"删用例，先修实现。
#     放在诊断断言之后：didChange 会异步推诊断，放前面会污染 10b 的轮询。
#     10b 把文档改成了 BADTXT，这里先改回 MAIN 再按 MAIN 的行号断言。
send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
    "textDocument": {"uri": uri, "version": 8},
    "contentChanges": [{"text": MAIN}]}})
send({"jsonrpc": "2.0", "id": 21, "method": "textDocument/inlayHint", "params": {
    "textDocument": {"uri": uri},
    "range": {"start": {"line": 0, "character": 0},
              "end": {"line": MAIN.count("\n"), "character": 0}}}})
r, _ = recv_id("inlayHint.main", 21)
hints = r["result"] if r and r.get("result") is not None else []
byline = {(h["position"]["line"], h["position"]["character"]): h["label"] for h in hints}

def hint_at(decl):
    p = pos(MAIN, MAIN.index(decl) + len(decl))
    return byline.get((p["line"], p["character"]))

detail = str(sorted(byline.items()))[:240]
check("inlayHint.varLiteral", hint_at("var s") == ": Shape", detail)
check("inlayHint.callRet", hint_at("let o") == ": Obj", detail)
check("inlayHint.memberRhs", hint_at("let n") == ": i32", detail)
check("inlayHint.literalSecond", hint_at("var o2") == ": Obj", detail)
check("inlayHint.inlineStructField", hint_at("let i") == ": i32", detail)
check("inlayHint.skipNonValue",
      all(hint_at(k) is None for k in ("const Obj", "const Shape", "const std", "const push")), detail)
# 类型提示是 kind=1；参数名提示是 kind=2，按 label 前缀区分
check("inlayHint.kind", all(h.get("kind") == 1 for h in hints if h["label"].startswith(":")), str(hints[:2]))
# 匿名 struct 当实参也有参数名提示：`push(info: .{…})`
def label_at_call(call):
    p = pos(MAIN, MAIN.index(call) + len(call))
    return byline.get((p["line"], p["character"]))

# 2026-11-08 用户第 5 条：匿名 struct 实参要连类型一起标（`info: struct{…}`），
# 否则模块/本地函数的匿名结构体参数只剩一个名字。
check("inlayHint.param.anonStruct",
      (label_at_call("push(") or "").startswith("info: struct{a: i32, msg: []const u8}"), detail)
check("inlayHint.param.kind",
      all(h.get("kind") == 2 and ": " in h["label"] for h in hints if not h["label"].startswith(":")),
      str(hints[:2]))

# 3o 显式写了类型 / 注释 / 字符串里的伪声明都不该插 hint
TYPED = """const T = struct { a: i32 };
pub const main = fn () i32 {
    var p = T{.a = 1};
    let n = 10;
    let t: i32 = 20;
    const label = "hi";
    // const fake = 1;
    return 0;
};
"""
send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
    "textDocument": {"uri": uri, "version": 9},
    "contentChanges": [{"text": TYPED}]}})
send({"jsonrpc": "2.0", "id": 22, "method": "textDocument/inlayHint", "params": {
    "textDocument": {"uri": uri},
    "range": {"start": {"line": 0, "character": 0}, "end": {"line": 9, "character": 0}}}})
r, _ = recv_id("inlayHint.typed", 22)
th = r["result"] if r and r.get("result") is not None else []
tbody = {(h["position"]["line"], h["position"]["character"]): h["label"] for h in th}

def thint(decl):
    p = pos(TYPED, TYPED.index(decl) + len(decl))
    return tbody.get((p["line"], p["character"]))

tdetail = str(sorted(tbody.items()))[:240]
check("inlayHint.typed.var", thint("var p") == ": T", tdetail)
check("inlayHint.typed.number", thint("let n") == ": i32", tdetail)
check("inlayHint.typed.string", thint("const label") == ": str", tdetail)
check("inlayHint.typed.skipExplicit", thint("let t") is None, tdetail)
check("inlayHint.typed.skipComment", len(th) == 3, tdetail)
send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
    "textDocument": {"uri": uri, "version": 10},
    "contentChanges": [{"text": MAIN}]}})

# 3p 参数名 inlay（含 struct 形参）+ 匿名 struct 绑定的类型推断
#     守门：fn 定义 / if ( / 注释 / 字符串 / cast 前缀都不该被当成调用。
PARAM = """const std = @mod("std");
const Person = struct{ age: i32, name: []const u8 };
const add = fn (a: i32, b: i32) i32 { return a + b; };
const take = fn (v: Person, n: i32) void { std.println("{}", n); };
pub const main = fn () i32 {
    var p = Person{.age = 1, .name = "x"};
    let anon = .{ .x = 1, .y = "s" };
    let n = add(1, 2);
    take(p, n);
    if (n > 0) { std.println("hi"); }
    // add(9, 9);
    std.println("add(1, 2)");
    return 0;
};
"""
send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
    "textDocument": {"uri": uri, "version": 11},
    "contentChanges": [{"text": PARAM}]}})
send({"jsonrpc": "2.0", "id": 23, "method": "textDocument/inlayHint", "params": {
    "textDocument": {"uri": uri},
    "range": {"start": {"line": 0, "character": 0},
              "end": {"line": PARAM.count("\n") + 1, "character": 0}}}})
r, _ = recv_id("inlayHint.params", 23)
ph = r["result"] if r and r.get("result") is not None else []
params = [h for h in ph if h.get("kind") == 2]
types = [h["label"] for h in ph if h.get("kind") == 1]
pdetail = str(sorted((h["position"]["line"], h["position"]["character"], h["label"]) for h in ph))[:300]
# 第 14 条起：字符串实参也标参数名（`println(fmt: "{}", args: n)`），
# 所以 `take` 体内的 println、main 里的两次 println 各自多出 fmt/args。
check("inlayHint.param.count", len(params) == 8, pdetail)
check("inlayHint.param.labels",
      sorted(h["label"] for h in params) ==
      ["a: ", "args: ", "b: ", "fmt: ", "fmt: ", "fmt: ", "n: ", "v: "], pdetail)
# 注释里的 `// add(9, 9);`（行 10）与字符串里的 `"add(1, 2)"` 都不算调用
check("inlayHint.param.lines",
      sorted(set(h["position"]["line"] for h in params)) == [3, 7, 8, 9, 11], pdetail)
check("inlayHint.type.anonStruct", ": struct{x:i32,y:str}" in types, pdetail)
check("inlayHint.type.namedStruct", ": Person" in types, pdetail)

# 3q 深层路径 / 泛型实例化 / 可选解包（用户 2026-11-09 第 14 条截图：
#    `std.arena.ArenaAllocator.init` 没有参数名提示、`var b = buf.?` 没有
#    类型提示、`std.map.HashMap(i32).init` 与 `std.format(...)` 整条哑掉）。
#    根因：被调方解析只认 `模块.成员`（n≤3）且不跨泛型括号，返回类型拿不到。
DEEP = """const std = @mod("std");
pub const main = fn () i32 {
    var buf: ?std.BufReader = std.BufReader.init(std.heap.page, 0, 10);
    var b = buf.?;
    _ = b;
    var arena = std.arena.ArenaAllocator.init(std.heap.page);
    _ = arena;
    var m = std.map.HashMap(i32).init(std.heap.page);
    _ = m;
    _ = std.format(std.heap.page, "hello{}", "ehl");
    let args = std.process.args();
    _ = args;
    return 0;
};
"""
send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
    "textDocument": {"uri": uri, "version": 13},
    "contentChanges": [{"text": DEEP}]}})
send({"jsonrpc": "2.0", "id": 24, "method": "textDocument/inlayHint", "params": {
    "textDocument": {"uri": uri},
    "range": {"start": {"line": 0, "character": 0},
              "end": {"line": DEEP.count("\n") + 1, "character": 0}}}})
r, _ = recv_id("inlayHint.deep", 24)
dh = r["result"] if r and r.get("result") is not None else []
dby = {(h["position"]["line"], h["position"]["character"]): h["label"] for h in dh}
ddetail = str(sorted(dby.items()))[:300]

def dlabel(anchor):
    p = pos(DEEP, DEEP.index(anchor) + len(anchor))
    return dby.get((p["line"], p["character"]))

_pb = pos(DEEP, DEEP.index("var b = buf") + len("var b"))
check("inlayHint.deep.unwrapType",
      dby.get((_pb["line"], _pb["character"])) == ": std.BufReader", ddetail)
check("inlayHint.deep.deepPathType", dlabel("var arena") == ": ?Allocator", ddetail)
check("inlayHint.deep.genericInstType", dlabel("var m") == ": HashMap", ddetail)
check("inlayHint.deep.builtinRetType", dlabel("let args") == ": [][]const u8", ddetail)
_dlabels = [h["label"] for h in dh if h.get("kind") == 2]
check("inlayHint.deep.deepPathParams", "backing: " in _dlabels, ddetail)
check("inlayHint.deep.genericInstParams",
      "V: " in _dlabels and "alloc: " in _dlabels, ddetail)
check("inlayHint.deep.builtinParams",
      "fmt: " in _dlabels and "args: " in _dlabels, ddetail)
send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
    "textDocument": {"uri": uri, "version": 12},
    "contentChanges": [{"text": MAIN}]}})

# 3r extern "C" 绑定线（用户 2026-11-08 反馈：vendor 里生成的绑定、手写
#     extern "C" 模块都没有补全/提示）。根因是 zxls 只索引 pub const/var/type，
#     `pub fn NAME(...) void;` 整条漏掉，补全 / hover / 签名 / 跳转一起哑。
#     这份 fixture 同时覆盖 cbind 生成物（vendor/glue.zx）与手写模块（src/gl.zx）。
GLUE = '''@link("glue");

extern "C" {
    // cbind 生成的函数声明长这样：缩进 + pub fn + 分号
    pub fn glClearColor(r: f32, g: f32, b: f32, a: f32) void;
    pub fn glClear(mask: u32) void;
    pub fn glViewport(x: i32, y: i32, w: i32, h: i32) void;
    pub fn glSetTitle(title: *const u8) void;
}

pub extern var glState: u32;

pub const GL_COLOR_BUFFER_BIT: u32 = 0x4000;
'''
GLLOCAL = '''@link("opengl32");

extern "C" {
    pub fn glBegin(mode: u32) void;
    pub fn glEnd() void;
    pub fn glVertex2f(x: f32, y: f32) void;
}

pub const GL_QUADS: u32 = 0x0007;
'''
BIND = '''const g  = @mod("glue");
const gl = @mod("gl.zx");

const main = fn () i32 {
    g.
    gl.
    g.glClearColor(0.0, 0.0, 0.0, 1.0);
    gl.glBegin(gl.GL_QUADS);
    gl.glVertex2f(1.0, 2.0);
    gl.glEnd();
    g.glViewport(0, 0, 720, 720);
    g.glSetTitle("hello");
    return 0;
};
'''
open(os.path.join(TESTDIR, "vendor", "glue.zx"), "w", encoding="utf-8").write(GLUE)
open(os.path.join(SRC_DIR, "gl.zx"), "w", encoding="utf-8").write(GLLOCAL)
open(os.path.join(SRC_DIR, "binduser.zx"), "w", encoding="utf-8").write(BIND)
binduri = "file:///" + os.path.join(SRC_DIR, "binduser.zx").replace("\\", "/")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": binduri, "languageId": "zx", "version": 1, "text": BIND}}})

def bind_items(text, off, rid):
    send({"jsonrpc": "2.0", "id": rid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": binduri}, "position": pos(text, off)}})
    r, _ = recv_id("completion", rid)
    return r["result"]["items"] if r and r.get("result") else []

off = BIND.index("    g.\n") + len("    g.")
items = bind_items(BIND, off, 80)
labels = {i["label"] for i in items}
kinds = {i["label"]: i.get("kind") for i in items}
check("completion.externVendor.functions",
      {"glClearColor", "glClear", "glViewport"} <= labels, str(sorted(labels))[:240])
check("completion.externVendor.constVar",
      {"GL_COLOR_BUFFER_BIT", "glState"} <= labels, str(sorted(labels))[:240])
# 2026-11-08 用户要求：模块函数补全要给 snippet（带括号 + 预填形参），
# 所以 kind 从 3（Function）变成 15（Snippet），detail 仍是完整签名。
texts = {i["label"]: (i.get("insertText") or "", i.get("insertTextFormat")) for i in items}
check("completion.externVendor.fnKind",
      kinds.get("glClear") == 15 and kinds.get("glClear(...)") is None, str(kinds)[:240])
check("completion.externVendor.fnSnippet",
      texts.get("glClear") == ("glClear(${1:mask})$0", 2), str(texts.get("glClear")))
# KEEP: fn 成员的 detail 必须是完整签名，不能落到绑定别名（2026-11-08 截图：
# `g.` 弹窗每条右侧都标 `g`，用户看不出哪条是函数）。带显式类型注解的
# 常量 detail 是声明类型。
details = {i["label"]: i.get("detail") for i in items}
check("completion.externVendor.fnDetail",
      (details.get("glClear") or "").startswith("glClear(mask: u32)"), str(details)[:300])
check("completion.externVendor.constDetail",
      details.get("GL_COLOR_BUFFER_BIT") == "u32", str(details)[:300])

off = BIND.index("    gl.\n") + len("    gl.")
labels = {i["label"] for i in bind_items(BIND, off, 81)}
check("completion.externLocal",
      {"glBegin", "glEnd", "glVertex2f", "GL_QUADS"} <= labels, str(sorted(labels))[:240])

off = BIND.index("g.glViewport(") + len("g.glV")
labels = {i["label"] for i in bind_items(BIND, off, 82)}
check("completion.externPrefix", "glViewport" in labels, str(sorted(labels))[:240])

def bind_hover(anchor, rid):
    off = BIND.index(anchor) + len(anchor) - 1
    send({"jsonrpc": "2.0", "id": rid, "method": "textDocument/hover", "params": {
        "textDocument": {"uri": binduri}, "position": pos(BIND, off)}})
    r, _ = recv_id("hover", rid)
    return r["result"]["contents"]["value"] if r and r.get("result") else ""

hv = bind_hover("g.glClearColor", 83)
check("hover.externVendor.fn",
      "glClearColor(r: f32, g: f32, b: f32, a: f32) void" in hv, repr(hv[:160]))
hv = bind_hover("gl.glVertex2f", 84)
check("hover.externLocal.fn", "glVertex2f(x: f32, y: f32) void" in hv, repr(hv[:160]))
# 守卫：形参里的 `*const u8` 不能把函数头骗成 `u8(...)`
hv = bind_hover("g.glSetTitle", 88)
check("hover.externFn.constPtrParam",
      "glSetTitle(title: *const u8) void" in hv, repr(hv[:160]))

off = BIND.index("g.glClearColor(") + len("g.glClearColor(")
send({"jsonrpc": "2.0", "id": 85, "method": "textDocument/signatureHelp", "params": {
    "textDocument": {"uri": binduri}, "position": pos(BIND, off)}})
r, _ = recv_id("signatureHelp", 85)
sigs = r["result"]["signatures"] if r and r.get("result") else []
check("signatureHelp.externVendor",
      bool(sigs) and "r: f32" in sigs[0].get("label", ""), str(sigs)[:240])

off = BIND.index("g.glViewport(") + len("g.glViewport") - 1
send({"jsonrpc": "2.0", "id": 86, "method": "textDocument/definition", "params": {
    "textDocument": {"uri": binduri}, "position": pos(BIND, off)}})
r, _ = recv_id("definition", 86)
loc = r.get("result") if r else None
target = ""
if isinstance(loc, dict):
    target = loc.get("uri", "")
elif isinstance(loc, list) and loc:
    target = loc[0].get("uri", "")
check("definition.externVendor", "vendor/glue.zx" in target, str(loc)[:240])

send({"jsonrpc": "2.0", "id": 87, "method": "textDocument/inlayHint", "params": {
    "textDocument": {"uri": binduri},
    "range": {"start": {"line": 0, "character": 0},
              "end": {"line": BIND.count("\n") + 1, "character": 0}}}})
r, _ = recv_id("inlayHint.params", 87)
bh = r["result"] if r and r.get("result") is not None else []
pnames = {h["label"] for h in bh if h.get("kind") == 2}
check("inlayHint.externParams",
      {"r: ", "x: ", "mode: "} <= pnames, str(sorted(pnames))[:240])

# 3t 模块导入补全：用户 2026-11-08 否掉「裸模块名即命名空间」（隐式名与
#     别名/再导出撞名时无解），名字重新只由显式绑定产生。LSP 不制造隐式
#     名字，而是在声明位置给出「插入一条绑定」的补全项，$1 可改别名。
IPROBE = '''const main = fn () i32 {
    glue.
    gl.glClearColor(0.0, 0.0, 0.0, 1.0);
    return 0;
};
glu
foo(glu)
st
glC
are
'''
ipath = os.path.join(SRC_DIR, "importprobe.zx")
open(ipath, "w", encoding="utf-8").write(IPROBE)
ipuri = "file:///" + ipath.replace("\\", "/")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": ipuri, "languageId": "zx", "version": 1, "text": IPROBE}}})

def ip_items(text, off, rid):
    send({"jsonrpc": "2.0", "id": rid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": ipuri}, "position": pos(text, off)}})
    r, _ = recv_id("completion", rid)
    return r["result"]["items"] if r and r.get("result") else []

def imports(items, label):
    return [i for i in items if i.get("kind") == 9 and i["label"] == label]

# 行首 glu → glue 导入项：插入的是绑定语句，不是让裸名可用
off = IPROBE.index("\nglu\n") + len("\nglu")
items = ip_items(IPROBE, off, 88)
imp = imports(items, "glue")
check("completion.importModule.item", len(imp) == 1, str(items)[:240])
check("completion.importModule.body",
      bool(imp) and imp[0].get("insertText") == 'const ${1:glue} = @mod("glue");$0',
      str(imp)[:240])
check("completion.importModule.snippet",
      bool(imp) and imp[0].get("insertTextFormat") == 2, str(imp)[:240])
# 表达式中间不能插 const
off = IPROBE.index("foo(glu") + len("foo(glu")
items = ip_items(IPROBE, off, 89)
check("completion.importModule.statementOnly",
      not imports(items, "glue"), str(imports(items, "glue"))[:240])
# 没绑定时 glue. 不给假成员（不再假装裸名是命名空间）
off = IPROBE.index("glue.\n") + len("glue.")
items = ip_items(IPROBE, off, 90)
check("completion.importModule.noBareDot",
      "glClearColor" not in {i["label"] for i in items},
      str([i["label"] for i in items])[:240])
# std 也要显式绑定（println 片段的前置条件）
off = IPROBE.index("\nst\n") + len("\nst")
items = ip_items(IPROBE, off, 91)
check("completion.importModule.std",
      bool(imports(items, "std")), str(imports(items, "std"))[:240])
# 非 dot 位置不再冒出别的文件里的裸 C 函数名（输入 glfw 一堆 glfwInit 的老毛病）
off = IPROBE.index("\nglC\n") + len("\nglC")
items = ip_items(IPROBE, off, 94)
check("completion.importModule.noWorkspaceNoise",
      "glClearColor" not in {i["label"] for i in items},
      str([i["label"] for i in items])[:240])
# §166：std/ 里的 .zx 是 std 模块的内部文件，不是顶层模块。以前补全会把
# std/arena.zx 当 "arena" 推荐，用户一接受就是 @mod("arena") -> E0601。
off = IPROBE.index("\nare\n") + len("\nare")
items = ip_items(IPROBE, off, 95)
check("completion.importModule.noStdInternals",
      not any(i.get("label") in ("arena", "math", "simd", "json", "fs")
              for i in items if i.get("kind") == 9),
      str([i.get("label") for i in items if i.get("kind") == 9])[:240])
# detail 是给用户看的，不该把 snippet 占位符 $1 露出来
check("completion.importModule.detailNoSnippetVar",
      bool(imp) and all("$1" not in i.get("detail", "") for i in imp),
      str([i.get("detail") for i in imp])[:240])
# 已经写过绑定的名字不再提示导入
IBOUND = '''const glue = @mod("glue");

const main = fn () i32 {
    glu
    return 0;
};
'''
send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
    "textDocument": {"uri": ipuri, "version": 2},
    "contentChanges": [{"text": IBOUND}]}})
off = IBOUND.index("    glu") + len("    glu")
items = ip_items(IBOUND, off, 92)
check("completion.importModule.boundNameHidden",
      not imports(items, "glue"), str(imports(items, "glue"))[:240])

# 别名绑定（绑定名 g != 模块名 glue）同样算已导入，不该反复提示
IALIAS = '''const g = @mod("glue");

const main = fn () i32 {
    glu
    return 0;
};
'''
send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
    "textDocument": {"uri": ipuri, "version": 3},
    "contentChanges": [{"text": IALIAS}]}})
off = IALIAS.index("    glu") + len("    glu")
items = ip_items(IALIAS, off, 93)
check("completion.importModule.aliasBoundHidden",
      not imports(items, "glue"), str(imports(items, "glue"))[:240])

# 3u 自定义类型实例 / 函数调用骨架（用户 2026-11-08：
#     "自定义类型实例和函数调用 添加snippet 提示"）。当前文件声明的容器
#     补 `Name{ .f = … }` 实例骨架，函数补带形参占位的调用骨架；
#     方法（首参 self）不列顶层调用骨架，点补全负责。
SNIP3 = '''const Obj = struct {
    count: i32,
    name: []const u8,
};

const Shape = union {
    circle: f64,
};

pub const make = fn () Obj {
    return Obj{ .count = 0, .name = "x" };
};

pub const greet = fn (name: []const u8, n: i32) void {
    std.println("{}", name);
};

const bump = fn (self: *Obj, d: i32) i32 {
    return self.count + d;
};

pub const main = fn () i32 {
    mak
    gre
    bum
    Obj
    Sh
    return 0;
};
'''
spath = os.path.join(SRC_DIR, "snipprobe.zx")
open(spath, "w", encoding="utf-8").write(SNIP3)
spuri = "file:///" + spath.replace("\\", "/")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": spuri, "languageId": "zx", "version": 1, "text": SNIP3}}})

def sp_items(text, off, rid):
    send({"jsonrpc": "2.0", "id": rid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": spuri}, "position": pos(text, off)}})
    r, _ = recv_id("completion", rid)
    return r["result"]["items"] if r and r.get("result") else []

def sp_snips(items):
    return [i for i in items if i.get("kind") == 15]

# 函数调用骨架：零参 fn()、带参 fn(a, b)
off = SNIP3.index("    mak\n") + len("    mak")
items = sp_items(SNIP3, off, 95)
mk = [i for i in sp_snips(items) if i["label"] == "make(...)"]
check("completion.exprSnippet.call", len(mk) == 1, str(items)[:240])
check("completion.exprSnippet.callBody",
      bool(mk) and mk[0].get("insertText") == "make()$0", str(mk)[:240])
check("completion.exprSnippet.callFmt",
      bool(mk) and mk[0].get("insertTextFormat") == 2, str(mk)[:240])
# 调用骨架发出后裸名也不再并存（用户 2026-11-08：函数的原型那条不要）
check("completion.exprSnippet.callNoBareIdent",
      not any(i["label"] == "make" for i in items),
      str([i["label"] for i in items])[:240])
off = SNIP3.index("    gre\n") + len("    gre")
items = sp_items(SNIP3, off, 96)
gr = [i for i in sp_snips(items) if i["label"] == "greet(...)"]
check("completion.exprSnippet.callArgs",
      bool(gr) and gr[0].get("insertText") == "greet(${1:name}, ${2:n})$0", str(gr)[:240])
# 方法（首参 self）不列调用骨架：`o.bump()` 由点补全负责
off = SNIP3.index("    bum\n") + len("    bum")
items = sp_items(SNIP3, off, 97)
check("completion.exprSnippet.noMethod",
      "bump(...)" not in {i["label"] for i in items},
      str([i["label"] for i in items])[:240])
# 自定义类型实例骨架：struct 多字段、union 字段字面量同样成立
off = SNIP3.index("    Obj\n") + len("    Obj")
items = sp_items(SNIP3, off, 98)
ob = [i for i in sp_snips(items) if i["label"] == "Obj{...}"]
check("completion.exprSnippet.ctor",
      len(ob) == 1, str(items)[:240])
check("completion.exprSnippet.ctorBody",
      bool(ob) and ob[0].get("insertText") ==
      "Obj{ .count = ${1:0}, .name = ${2:0} }$0", str(ob)[:240])
# 构造骨架发出后裸名不再并存（用户 2026-11-08：`Person 当前文件` 那条不要，
# 只留 `Obj{...} 构造实例`）。骨架没发成的名字（enum/零字段）裸名照常保留。
check("completion.exprSnippet.noBareIdent",
      not any(i["label"] == "Obj" for i in items),
      str([i["label"] for i in items])[:240])
off = SNIP3.index("    Sh\n") + len("    Sh")
items = sp_items(SNIP3, off, 99)
sh = [i for i in sp_snips(items) if i["label"] == "Shape{...}"]
check("completion.exprSnippet.ctorUnion",
      bool(sh) and sh[0].get("insertText") == "Shape{ .circle = ${1:0} }$0", str(sh)[:240])
# 点上下文（字段/成员补全）不出骨架项
SPDOT = '''const Obj = struct {
    count: i32,
};
pub const main = fn () i32 {
    Obj{.
    return 0;
};
'''
send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
    "textDocument": {"uri": spuri, "version": 2},
    "contentChanges": [{"text": SPDOT}]}})
off = SPDOT.index("Obj{.") + len("Obj{.")
items = sp_items(SPDOT, off, 100)
check("completion.exprSnippet.noDotCtx",
      len(sp_snips(items)) == 0, str([i["label"] for i in items])[:240])

# ---------------------------------------------------------------------------
# 11 模块命名空间线（2026-11-08 用户六条）：
#  1 std 命名空间：真实成员先出，编译器内建排最后，detail 不再只是绑定名
#  3 跨模块类型：`a.Cfg` 的 detail 不能落到绑定名 `a`
#  3 模块实例嵌套：`let v = a.Outer{…}; v.inner.` / hover v.inner.a
#  4 模块函数 snippet：`a.tw` → twice(${1:x})$0（括号 + 预填形参）
#  5 模块函数的匿名 struct 参数（含嵌套）补全
#  6 模块 struct 类型的构造 snippet：`a.C` → Cfg{ .w = ${1:0} … }
NESTLIB = """pub const Cfg = struct {
    pub w: i32,
    pub h: i32,
};

pub const Outer = struct {
    pub inner: struct { a: i32, b: []const u8 },
    pub name: []const u8,
};

pub const twice = fn (x: i32) i32 {
    return x * 2;
};

pub const deepfn = fn (cfg: struct{ opts: struct{ width: i32, height: i32 }, flag: bool }) void {
    cfg;
};

pub const handler = fn (cb: fn (i32) i32, info: struct{ count: i32, name: []const u8 }) void {
    cb(info.count);
};
"""
open(os.path.join(SRC_DIR, "nestlib.zx"), "w", encoding="utf-8").write(NESTLIB)
NESTUSE = """const a = @mod("nestlib");
const std = @mod("std");

pub const main = fn () i32 {
    a.deepfn(.{ .opts = .{ .width = 1 }, .flag = true });
    a.handler(0, .{ .count = 1, .name = "y" });
    let v = a.Outer{ .inner = .{ .a = 1 }, .name = "x" };
    v.inner.a;
    a.
    let t = a.tw
    a.Cfg{.
    std.
    return 0;
};
"""
open(os.path.join(SRC_DIR, "nestuser.zx"), "w", encoding="utf-8").write(NESTUSE)
nesturi = "file:///" + os.path.join(SRC_DIR, "nestuser.zx").replace("\\", "/")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": nesturi, "languageId": "zx", "version": 1, "text": NESTUSE}}})

def nest_items(off, rid):
    send({"jsonrpc": "2.0", "id": rid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": nesturi}, "position": pos(NESTUSE, off)}})
    r, _ = recv_id("completion", rid)
    return r["result"]["items"] if r and r.get("result") else []

# 1 std 命名空间：真实成员在前，内建在后且 detail 只是"编译器内建"
off = NESTUSE.index("    std.\n") + len("    std.")
sit = nest_items(off, 300)
slab = [i["label"] for i in sit]
sd = {i["label"]: i.get("detail") for i in sit}
check("completion.std.namespaceOrder",
      slab[:1] == ["str"] and slab.index("println") > slab.index("task"), str(slab[:6])[:200])
# 2026-11-09 第 2 条：内建函数不再裸出"编译器内建"——签名 detail + 调用骨架
psn = [i for i in sit if i["label"] == "println"]
check("completion.std.builtinSnippet",
      bool(psn) and psn[0].get("kind") == 15 and psn[0].get("insertTextFormat") == 2
      and psn[0].get("insertText") == 'println("${1:fmt}", $0)'
      and (psn[0].get("detail") or "").startswith("println(fmt")
      # 函数只一条（第 13 条）：不再并列 `println(...)` 骨架项
      and len(psn) == 1, str(psn))
# 2026-11-09 第 4 条：Allocator/stackAlloc/heapAlloc 暴露且带 snippet；
# TypeKind/TypeInfo/FieldInfo 纯编译期反射仍挡住（第 1 条去污染）
check("completion.std.noCtBuiltins",
      not ({"TypeKind", "TypeInfo", "FieldInfo"} & set(slab))
      and {"Allocator", "stackAlloc", "heapAlloc"} <= set(slab),
      str(sorted(set(slab))))
al_ = [i for i in sit if i["label"] == "Allocator{...}"]
al_plain = [i for i in sit if i["label"] == "Allocator"]
check("completion.std.allocatorSnippet",
      bool(al_) and al_[0].get("kind") == 15 and al_[0].get("insertTextFormat") == 2
      and (al_[0].get("insertText") or "").startswith("Allocator{ .ctx")
      and bool(al_plain) and al_plain[0].get("kind") == 22
      and (al_plain[0].get("detail") or "").startswith("Allocator: type"),
      str(al_) + " / " + str(al_plain))
st_ = {i["label"]: i.get("insertText") for i in sit}
check("completion.std.allocSnippet",
      st_.get("stackAlloc") == "stackAlloc(${1:T}, ${2:n})$0"
      and st_.get("heapAlloc") == "heapAlloc(${1:alloc}, ${2:T}, ${3:n})$0"
      and st_.get("stackAlloc(...)") is None,
      str((st_.get("stackAlloc"), st_.get("heapAlloc"))))
check("completion.std.moduleDetail", sd.get("str") == '@mod("str.zx")', str(sd.get("str")))
# 2026-11-09 截图：`std.debug("create failed.")` 报 "called value has type
# `type`"——命名空间被当函数。补全里它们按 Module(kind=9) 出、detail 写明
# 不能直接调用，别再像个函数。
sk = {i["label"]: i.get("kind") for i in sit}
check("completion.std.namespaceKind",
      all(sk.get(x) == 9 for x in ("debug", "atomic", "heap", "io", "file", "process"))
      and "不能直接调用" in (sd.get("debug") or "")
      and "panic" in (sd.get("debug") or ""),
      str({k: (sk.get(k), sd.get(k)) for k in ("debug", "io", "heap")}))

# 3/6 跨模块类型：detail 是类型，不是绑定名；构造 skeleton
off = NESTUSE.index("    a.\n") + len("    a.")
ait = nest_items(off, 301)
ad = {i["label"]: i.get("detail") for i in ait}
ak = {i["label"]: i.get("kind") for i in ait}
at = {i["label"]: i.get("insertText") for i in ait}
check("completion.module.typeDetail",
      ad.get("Cfg") == "Cfg: type" and ak.get("Cfg") == 22, str(ad)[:200])
# 类型项与构造骨架并存：`Cfg`（只补名字、detail 是类型）+
# `Cfg{...}`（Tab 进字段），用户 2026-11-09 第 12 条。
check("completion.module.ctorSnippet",
      ak.get("Cfg{...}") == 15 and at.get("Cfg{...}") == "Cfg{ .w = ${1:0}, .h = ${2:0} }$0",
      str(at.get("Cfg{...}")))

# 4 模块函数 snippet：前缀过滤 + 括号 + 预填形参
off = NESTUSE.index("    let t = a.tw") + len("    let t = a.tw")
tit = nest_items(off, 302)
tl = [i["label"] for i in tit]
td = {i["label"]: i.get("detail") for i in tit}
tt = {i["label"]: (i.get("insertText") or "", i.get("insertTextFormat")) for i in tit}
# 函数只出一条：`twice`（kind=15 带 snippet），不再并列 `twice(...)`（第 13 条）
check("completion.module.prefixFilter", tl == ["twice"], str(tl))
check("completion.module.fnSnippet",
      tt.get("twice") == ("twice(${1:x})$0", 2), str(tt.get("twice")))
check("completion.module.fnDetail",
      (td.get("twice") or "").startswith("twice(x: i32)"), str(td.get("twice")))

# 5 模块函数的匿名 struct 参数（嵌套）
off = NESTUSE.index("a.deepfn(.{ .opts = .{ .") + len("a.deepfn(.{ .opts = .{ .")
dl = {i["label"] for i in nest_items(off, 303)}
check("completion.module.anonNestedArg", {"width", "height"} <= dl, str(sorted(dl)))
off = NESTUSE.index("a.handler(0, .{ .") + len("a.handler(0, .{ .")
hl = {i["label"] for i in nest_items(off, 304)}
check("completion.module.anonArg2", {"count", "name"} <= hl, str(sorted(hl)))

# 6 模块 struct 字面量字段
off = NESTUSE.index("    a.Cfg{.\n") + len("    a.Cfg{.")
cl = {i["label"] for i in nest_items(off, 305)}
check("completion.module.literalFields", {"w", "h"} <= cl, str(sorted(cl)))

# 6c` 容器体的 pub 成员不是模块成员（2026-11-10 同一处：`std.` 混进整屏
# 方法）。项目模块走同一条 topLevelDecls，一起守——`m.` 只该出 Box/make。
LEAKLIB = """pub const Box = struct {
    pub n: usize,
    pub const put = fn (self: *Self, v: i32) bool {
        self.n = (usize)v;
        return true;
    };
    pub const get = fn (self: *Self) i32 { return (i32)self.n; };
};

pub const make = fn () Box {
    return Box{ .n = 0 };
};
"""
open(os.path.join(SRC_DIR, "leaklib.zx"), "w", encoding="utf-8").write(LEAKLIB)
LEAKUSE = """const m = @mod("leaklib.zx");

pub const main = fn () i32 {
    let b = m.make();
    m.
    b.
    return 0;
};
"""
leakuri = "file:///" + os.path.join(SRC_DIR, "leakuse.zx").replace("\\", "/")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": leakuri, "languageId": "zx", "version": 1, "text": LEAKUSE}}})


def leak_items(off, rid):
    send({"jsonrpc": "2.0", "id": rid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": leakuri}, "position": pos(LEAKUSE, off)}})
    r, _ = recv_id("leak", rid)
    return {i["label"] for i in r["result"]["items"]} if r and r.get("result") else set()


_ml = leak_items(LEAKUSE.index("    m.\n") + len("    m."), 3060)
check("completion.module.noMemberLeak",
      {"Box", "make"} <= _ml and not ({"put", "get", "n", "Box.put", "make.get"} & _ml),
      str(sorted(_ml))[:240])
_bl = leak_items(LEAKUSE.index("    b.\n") + len("    b."), 3061)
check("completion.module.instMembers",
      {"n", "put", "get"} <= _bl and not ({"make", "Box"} & _bl), str(sorted(_bl))[:240])

# 3 模块实例嵌套：v.inner. 与 hover v.inner.a
off = NESTUSE.index("v.inner.a") + len("v.inner.")
il = {i["label"] for i in nest_items(off, 306)}
check("completion.module.instanceNested", {"a", "b"} <= il, str(sorted(il)))
send({"jsonrpc": "2.0", "id": 307, "method": "textDocument/hover", "params": {
    "textDocument": {"uri": nesturi}, "position": pos(NESTUSE, NESTUSE.index("v.inner.a") + 8)}})
r, _ = recv_id("hover", 307)
hv = ((r or {}).get("result") or {}).get("contents") or {}
check("hover.module.instanceNested", "a: i32" in (hv.get("value") or ""), str(hv)[:200])

# 2/5 嵌套匿名字面量的行内类型提示（本地函数与模块函数同一套）
send({"jsonrpc": "2.0", "id": 308, "method": "textDocument/inlayHint", "params": {
    "textDocument": {"uri": nesturi},
    "range": {"start": {"line": 0, "character": 0},
              "end": {"line": NESTUSE.count("\n"), "character": 0}}}})
r, _ = recv_id("inlayHint", 308)
hints = (r or {}).get("result") or []
hlab = [h["label"] for h in hints]
check("inlayHint.anonNestedLiteral",
      ": struct{ width: i32, height: i32 }" in hlab, str(hlab)[:300])

# ---------------------------------------------------------------------------
# 12 std 线（2026-11-09 用户条目）：
#  1 std 命名空间去污染：TypeKind/TypeInfo/FieldInfo 纯编译期反射不进补全；
#    Allocator/stackAlloc/heapAlloc 暴露且带 snippet（用户第 4 条）
#  2 std 内建函数与门面子模块（std.str.split）有签名 hover、成员有调用骨架
#  3 std 类型实体（Task/ArenaAllocator/泛型 List）成员有类型、方法有签名
#  4 std.zx 不设类型别名（用户第 4 条下午：别名和真身分不清）——
#    类型只走 std.task.Task / std.map.HashMap 这类真实路径
STDUSE = """const std = @mod("std");

pub const main = fn () u8 {
    let g = std.task.Task.single();
    g.await(1);
    let l = std.array.List(i32).init(std.heap.page);
    l.push(1);
    let t = std.task.Task{ .id = 0 };
    t.await(1);
    var m = std.map.HashMap(i32).init(std.heap.page);
    _ = m.put("a", 1);
    var dt = std.time.fromUnixMs(0);
    var sm = std.fs.stat(std.heap.page, "x");
    var r = std.BufReader.open(std.heap.page, "x", 64);
    var w = std.BufWriter.create(std.heap.page, "y", 64);
    var V: type = std.array.List(i32);
    var list = V.init(std.heap.page);
    _ = dt.year;
    _ = sm.?.size;
    _ = std.heap.page.ctx;
    _ = std.stackAlloc;
    _ = std.task;
    _ = std.map;
    r.?.
    w.?.
    list.
    std.io.
    std.str.
    std.arena.
    std.json.
    std.BufReader.
    std.map.HashMap.
    std.json.Value.
    std.pool.Pool.
    std.str.split(std.heap.page, "a", ",");
    std.println("x");
    return 0;
};
"""
stduri = "file:///" + os.path.join(SRC_DIR, "stduser.zx").replace("\\", "/")
open(os.path.join(SRC_DIR, "stduser.zx"), "w", encoding="utf-8").write(STDUSE)
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": stduri, "languageId": "zx", "version": 1, "text": STDUSE}}})

def std_items(off, rid):
    send({"jsonrpc": "2.0", "id": rid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": stduri}, "position": pos(STDUSE, off)}})
    r, _ = recv_id("completion", rid)
    return r["result"]["items"] if r and r.get("result") else []

def std_hover(anchor, within, rid):
    a = STDUSE.index(anchor)
    off = a + STDUSE[a:].index(within) + len(within)  # 词尾
    send({"jsonrpc": "2.0", "id": rid, "method": "textDocument/hover", "params": {
        "textDocument": {"uri": stduri}, "position": pos(STDUSE, off)}})
    r, _ = recv_id("hover", rid)
    res = (r or {}).get("result")
    return (res.get("contents") or {}).get("value") or "" if isinstance(res, dict) else ""

# 2a std.io. 成员：签名 detail + 调用骨架
ioit = std_items(STDUSE.index("    std.io.\n") + len("    std.io."), 400)
iod = {i["label"]: i.get("detail") for i in ioit}
iot = {i["label"]: i.get("insertText") for i in ioit}
check("completion.stdNs.memberSig", (iod.get("openRead") or "") == "openRead(path: []const u8) ?usize", str(iod.get("openRead")))
check("completion.stdNs.memberSnippet",
      iot.get("read") == "read(${1:h}, ${2:buf})$0"
      and iot.get("read(...)") is None, str(iot.get("read")))

# 2b std.str.（门面别名链）：子模块成员照常出 + 骨架
strit = std_items(STDUSE.index("    std.str.\n") + len("    std.str."), 401)
strd = {i["label"]: i.get("detail") for i in strit}
check("completion.stdFacade.subModule",
      (strd.get("split") or "").startswith("split(alloc: Allocator"), str(list(strd))[:200])

# 2c 内建 hover：std.println / std.io.openRead / std.str.split / std.heap.page
check("hover.std.builtinFn",
      std_hover("std.println(\"x\")", "println", 500) == "```zx\nprintln(fmt: []const u8, args: anytype) void\n```",
      std_hover("std.println(\"x\")", "println", 500))
check("hover.stdNs.member",
      std_hover("std.heap.page", "page", 501) == "```zx\npage: Allocator\n```",
      std_hover("std.heap.page", "page", 501))
check("hover.stdFacade.chain",
      "split(alloc: Allocator" in std_hover('std.str.split(std.heap.page, "a", ",");', "split", 502),
      std_hover('std.str.split(std.heap.page, "a", ",");', "split", 502))

# 3a std 类型实例：静态成员出骨架、实例方法出签名
off = STDUSE.index("    g.await(1);") + len("    g.")
git = std_items(off, 403)
gd = {i["label"]: i.get("detail") for i in git}
gt = {i["label"]: i.get("insertText") for i in git}
gk = {i["label"]: i.get("kind") for i in git}
# 用户 2026-11-10："Task 命名空间污染了"。`g.single()` 编译报 E0303
#（首参不是 self，不是方法），静态构造器只属于 `Task.`，不许混进实例补全。
# §129：group 是 Task. 上的工厂，不进实例补全。
check("completion.stdInstance.staticSnippet",
      not ({"single", "coro", "pool", "fromId", "group"} & set(gk)) and "id" in gk,
      str(sorted(gk)))
check("completion.stdInstance.methodSig",
      (gd.get("await") or "") == "await(self: Task, h: usize) !usize"
      and gk.get("await") == 15 and gt.get("await") == "await(${1:h})$0",
      "%r %r" % (gd.get("await"), gk.get("await")))

# 3b 泛型容器实例：std.array.List(i32) → l. 出全部成员，字段带 T 类型
off = STDUSE.index("    l.push(1);") + len("    l.")
lit_ = std_items(off, 404)
lid = {i["label"]: i.get("detail") for i in lit_}
check("completion.genericInstance.members",
      lid.get("push", "") == "push(self: *List, v: T) bool"
      and lid.get("buf") == "*T" and lid.get("len") == "usize", str(lid)[:240])

# 3c 实例方法 hover（std 类型 + 泛型）
check("hover.stdInstance.method",
      std_hover("g.await(1);", "await", 505) == "```zx\nawait(self: Self, h: usize) !usize\n```",
      std_hover("g.await(1);", "await", 505))
check("hover.genericInstance.method",
      std_hover("l.push(1);", "push", 506) == "```zx\npush(self: *Self, v: T) bool\n```",
      std_hover("l.push(1);", "push", 506))

# 3d 模块限定类型实体（`std.task.Task{…}`）：
#   旧版 inferContainer 把 `std.task.Task{...}` 认成模块名 "task"，实例成员一个
#   都不出；hover 又把容器声明当函数，吐 `Task() Self` 这种错签名。
off = STDUSE.index("    t.await(1);") + len("    t.")
tit = std_items(off, 407)
td = {i["label"]: i.get("detail") for i in tit}
check("completion.mqInstance.members",
      "await" in td and "spawn" in td and td.get("id") == "usize", str(list(td))[:240])
check("hover.mqType.definition",
      "struct {" in std_hover("std.task.Task{", "Task", 508)
      and "id: usize" in std_hover("std.task.Task{", "Task", 508),
      std_hover("std.task.Task{", "Task", 508)[:200])
# 暴露的编译期分配内建：hover 出对齐表签名（2026-11-09 用户第 4 条）
check("hover.std.ctExposed",
      "stackAlloc(T: type, n: usize) *[n]T" in std_hover("_ = std.stackAlloc;", "stackAlloc", 509),
      std_hover("_ = std.stackAlloc;", "stackAlloc", 509))
# 模块别名 hover：std.task → module `task.zx`
check("hover.stdModuleAlias",
      "module `task.zx`" in std_hover("_ = std.task;", "task", 510),
      std_hover("_ = std.task;", "task", 510))


def std_def(anchor, within, rid):
    """光标停在 within 词尾 → definition 返回的目标（uri + 行文本）。"""
    a = STDUSE.index(anchor)
    off = a + STDUSE[a:].index(within) + len(within)
    send({"jsonrpc": "2.0", "id": rid, "method": "textDocument/definition", "params": {
        "textDocument": {"uri": stduri}, "position": pos(STDUSE, off)}})
    r, _ = recv_id("definition", rid)
    loc = (r or {}).get("result")
    uri, ln = "", -1
    if isinstance(loc, dict):
        uri, ln = loc.get("uri", ""), loc["range"]["start"]["line"]
    elif isinstance(loc, list) and loc:
        uri, ln = loc[0].get("uri", ""), loc[0]["range"]["start"]["line"]
    body = ""
    if uri.startswith("file:///"):
        try:
            with open(uri[len("file:///"):], encoding="utf-8") as f:
                body = f.read().split("\n")[ln]
        except OSError:
            pass
    return uri, body


# 2026-11-10 用户：std 再导出的库跳不到源码。std.task.Task 要落进
# std/task.zx 的 `pub const Task = struct {`，std.map.HashMap 落 map.zx。
u, body = std_def("let g = std.task.Task.single();", "Task", 520)
check("definition.stdReexport.task",
      u.replace("\\", "/").endswith("std/task.zx") and "Task" in body, f"{u} {body!r}"[:240])
u, body = std_def("var m = std.map.HashMap(i32).init(std.heap.page);", "HashMap", 521)
check("definition.stdReexport.map",
      u.replace("\\", "/").endswith("std/map.zx") and "HashMap" in body, f"{u} {body!r}"[:240])
u, body = std_def("_ = std.task;", "task", 522)
check("definition.stdReexport.alias",
      u.replace("\\", "/").endswith("std/task.zx"), f"{u}"[:240])
# 5 泛型容器 hover 贴真实 struct 体（2026-11-09：std 全部类型都要有类型提示，
# 泛型不能只出一行 `X(T: type) type` 签名）。§190 后 std 容器写糖语法
# `const X = struct (T: type) {`，hover 贴体时签名就是糖形状。
check("hover.generic.facadeDefinition",
      "struct (V: type)" in std_hover("std.map.HashMap(i32)", "HashMap", 511)
      and "vals: *V" in std_hover("std.map.HashMap(i32)", "HashMap", 511),
      std_hover("std.map.HashMap(i32)", "HashMap", 511)[:200])
check("hover.generic.topLevelDefinition",
      "struct (T: type)" in std_hover("std.array.List(i32)", "List", 512)
      and "buf: *T" in std_hover("std.array.List(i32)", "List", 512),
      std_hover("std.array.List(i32)", "List", 512)[:200])
# 5 零字段容器（ArenaAllocator）：构造形态是静态 init，出 `Name.init(…)` 骨架
arit = std_items(STDUSE.index("    std.arena.\n") + len("    std.arena."), 406)
ard = {i["label"]: (i.get("kind"), i.get("insertText"), i.get("detail")) for i in arit}
# 只有方法的类型（ArenaAllocator）：`std.arena.` 这一层只出类型项，不再把
# 下一层的 `ArenaAllocator.init` 混进来（用户 2026-11-09 第 12 条：能出
# DirIter 这一层，不能出 DirIter.open）。`init` 在 `ArenaAllocator.` 里。
check("completion.facade.zeroFieldInit",
      ard.get("ArenaAllocator", (None, None, None))[0] == 22
      and ard["ArenaAllocator"][2] == "ArenaAllocator: type"
      and "ArenaAllocator.init" not in ard,
      str(ard.get("ArenaAllocator")))
# 5 union(enum)（json.Value）：出首个变体的构造骨架
jsit = std_items(STDUSE.index("    std.json.\n") + len("    std.json."), 408)
jsd = {i["label"]: (i.get("kind"), i.get("insertText"), i.get("detail")) for i in jsit}
check("completion.union.ctorSnippet",
      jsd.get("Value{...}", (None, None, None))[0] == 15
      and jsd["Value{...}"][1] == "Value{ .nul = ${1:0} }$0"
      and "首个变体" in jsd["Value{...}"][2]
      and jsd.get("Value", (None, None, None))[0] == 22,
      str(jsd.get("Value{...}")))
# enum（ParseErr）不是字面量构造：不出骨架，保留 detail=enum
check("completion.enum.noCtor",
      jsd.get("ParseErr") is not None and jsd["ParseErr"][0] != 15,
      str(jsd.get("ParseErr")))

# 6 std 类型与实例（2026-11-09 第二轮：类型提示 + snippet + 实例补全）
#  6a std. 顶层不再出现别名假项（HashMap/Pool/ArenaAllocator/Task 只走真实路径）
top_items = std_items(STDUSE.index("    std.println") + len("    std."), 409)
top_lab2 = [i["label"] for i in top_items]
top_d = {i["label"]: (i.get("kind"), i.get("detail"), i.get("insertText")) for i in top_items}
check("completion.stdTop.noAliasPhantom",
      not ({"HashMap", "Pool", "ArenaAllocator", "Task"} & set(top_lab2)),
      str(sorted(set(top_lab2))[:240]))
# KEEP — 容器体里的成员不是模块成员（2026-11-10 用户截图：`std.` 一弹混进
# ArrayList/BufReader 的整屏方法）。topLevelDecls 必须跟花括号深度；给容器
# 成员加 `pub` 之后凭行首关键字猜声明就会把 push/flush/… 全收进来。
check("completion.stdTop.noMemberLeak",
      not ({"push", "pop", "append", "appendTo", "flush", "fill", "writeByte",
            "insertAt", "removeAt", "reserve", "resize", "reverse", "clear",
            "close", "create", "open", "read", "write", "next", "at", "get",
            "put", "count", "reset", "deinit", "buffered"} & set(top_lab2)),
      str(sorted(set(top_lab2)))[:300])
#  6b 句柄类型（BufReader/BufWriter）：`std.` 这一层只出类型项；
#     `open`/`create` 这些下一层成员在 `std.BufReader.`（6c）里，
#     不再上浮成 `BufReader.open`（用户 2026-11-09 第 12 条）。
check("completion.stdTop.typeItem",
      top_d.get("BufReader", (None, None, None))[1] == "BufReader: type"
      and top_d["BufReader"][0] == 22
      and top_d.get("BufWriter", (None, None, None))[0] == 22
      and "BufReader.open" not in top_d and "BufWriter.create" not in top_d,
      str(top_d.get("BufReader")))
#  6c 类型路径成员 std.BufReader.：方法有签名、静态方法有调用骨架
rid_b = 413
brit = std_items(STDUSE.index("    std.BufReader.") + len("    std.BufReader."), rid_b)
brd = {i["label"]: (i.get("kind"), i.get("detail"), i.get("insertText")) for i in brit}
check("completion.typePath.methodSig",
      # 类型命名空间只留关联函数（`open` 首参不是 self）；方法 `next` 只在
      # 实例上出现——用户 2026-11-10："Task 空间上也是这些[方法]"。
      "next" not in brd
      and brd.get("open", (None, None, None))[0] == 15
      and brd.get("open", (None, ""))[1] == "open(alloc: Allocator, path: []const u8, cap: usize) ?BufReader"
      and brd["open"][2] == "open(${1:alloc}, ${2:path}, ${3:cap})$0",
      str(brd)[:300])
#  6d 实例补全：HashMap 实例 / 模块函数返回类型实例（DateTime / ?Stat）
mit = std_items(STDUSE.index("    _ = m.put") + len("    _ = m."), 414)
mlab = {i["label"]: i.get("detail") for i in mit}
check("completion.instance.hashMap",
      # 2026-11-11 容器成员可见性：HashMap 的字段（n/hashes/keys/vals/cap）
      # 默认私有，跨文件实例补全只出 pub 方法。
      {"get", "put", "count", "capacity"} <= set(mlab)
      and not ({"n", "hashes", "keys", "vals", "cap"} & set(mlab)), str(list(mlab))[:200])
off = STDUSE.index("    _ = dt.year;") + len("    _ = dt.")
dit = std_items(off, 415)
dlab = {i["label"] for i in dit}
check("completion.instance.moduleFnRet",
      {"year", "month", "day", "sec", "ms"} <= dlab, str(sorted(dlab)))
# 可选值先解包（用户 2026-11-10："?T 类型 .? 之后丢失类型"）：
# `sm.` 唯一合法后缀是 `.?`，只出解包项；`sm.?.` 才出 Stat 的成员。
off = STDUSE.index("    _ = sm.?.size;") + len("    _ = sm.")
sgate = {i["label"] for i in std_items(off, 4160)}
check("completion.optional.gate", sgate == {"?"}, str(sorted(sgate)))
off = STDUSE.index("    _ = sm.?.size;") + len("    _ = sm.?.")
sit2 = std_items(off, 416)
slab2 = {i["label"] for i in sit2}
check("completion.instance.optionalRet",
      {"size", "mtime", "isHidden"} <= slab2, str(sorted(slab2)))
# 内建类型 Allocator（sema 合成，源文件里没有声明）：实例的三字段
off = STDUSE.index("    _ = std.heap.page.ctx;") + len("    _ = std.heap.page.")
alit = std_items(off, 4166)
alab = {i["label"]: i.get("detail") for i in alit}
check("completion.builtin.allocatorMembers",
      alab.get("ctx") == "?*u8"
      and (alab.get("alloc") or "").startswith("fn (?*u8, usize)")
      and (alab.get("free") or "").startswith("fn (?*u8, *u8, usize)"), str(alab))
#  6e std 类型实例的方法要带签名 + 骨架（用户 2026-11-09：BufReader/BufWriter
#     实例的方法没有提示和 snippet）。self 是隐含接收者，签名里不出现。
off = STDUSE.index("    r.?.\n") + len("    r.?.")
rit = std_items(off, 417)
rd = {i["label"]: (i.get("detail"), i.get("kind"), i.get("insertText")) for i in rit}
check("completion.instance.bufReaderMethods",
      rd.get("next", (None,))[0] == "next(self: *BufReader) ?[]const u8"
      and rd.get("next", (None, None, None))[2] == "next()$0"
      and rd.get("chunk", (None,))[0] == "chunk(self: *BufReader) ?[]const u8"
      # `open` 是静态工厂（首参不是 self）：`r.open()` 报 E0303，只在
      # `BufReader.` 上出现（用户 2026-11-10 命名空间分离）。
      and "open" not in rd,
      str(rd.get("next")))
off = STDUSE.index("    w.?.\n") + len("    w.?.")
wit = std_items(off, 418)
wd = {i["label"]: (i.get("detail"), i.get("insertText")) for i in wit}
check("completion.instance.bufWriterMethods",
      wd.get("write", (None,))[0] == "write(self: *BufWriter, data: []const u8) bool"
      and wd.get("write", (None, None))[1] == "write(${1:data})$0"
      and wd.get("writeLine", (None, None))[1] == "writeLine(${1:s})$0"
      and wd.get("flush", (None, None))[1] == "flush()$0",
      str(wd.get("write")))
# 类型路径不出实例字段（用户 2026-11-10 同一处：`std.BufReader.h` 是
# E0300）。方法留在类型上（未绑定调用合法：`Task.spawn(&t, f, 0)`）。
off = STDUSE.index("    std.BufReader.") + len("    std.BufReader.")
bpt = {i["label"] for i in std_items(off, 4167)}
check("completion.typePath.noField",
      {"open", "init"} <= bpt and not ({"next", "chunk", "readAll"} & bpt)
      and not ({"h", "buf", "cap", "start", "end", "eof", "owned", "alloc"} & bpt),
      str(sorted(bpt)))
#  6f 类型变量绑定：`var V: type = std.array.List(i32)` → `V.init(…)` 的实例
#     也要推断成 List（用户 2026-11-09 截图里的写法）
off = STDUSE.index("    list.\n") + len("    list.")
lit2 = std_items(off, 419)
lld = {i["label"]: i.get("detail") for i in lit2}
check("completion.typeVar.instance",
      lld.get("push") == "push(self: *List, v: T) bool" and lld.get("len") == "usize"
      and lld.get("items") == "items(self: *List) []T", str(lld)[:240])
#  6g 三段路径的泛型容器：`std.map.HashMap.` 一个成员都不出过
off = STDUSE.index("    std.map.HashMap.") + len("    std.map.HashMap.")
hit3 = std_items(off, 420)
h3 = {i["label"]: (i.get("detail"), i.get("insertText")) for i in hit3}
check("completion.typePath.genericMembers",
      # 三段路径的泛型容器：类型上只该有静态函数。init 是 pub 静态；
      # norm 2026-11-11 起私有（无 pub），类型路径也不再出现。
      set(h3) == {"init"}
      and h3.get("init", (None, None))[1] == "init(${1:alloc})$0",
      str(sorted(h3))[:240])
#  6h union(enum) 的三段路径：Value. 出变体（构造骨架走 std.json. 类型项）
off = STDUSE.index("    std.json.Value.") + len("    std.json.Value.")
vit = std_items(off, 421)
vlab = {i["label"] for i in vit}
check("completion.typePath.unionVariants",
      {"nul", "int", "float", "str", "arr", "obj"} <= vlab, str(sorted(vlab)))
#  6i 三段路径的 Pool：类型上只有静态工厂 init，实例方法（alloc/free/
#     deinit）不进类型命名空间（用户 2026-11-10 命名空间分离）。线程安全
#     能力在另一个类型 SyncPool 上，不在 Pool 上加构造器（§143）。
off = STDUSE.index("    std.pool.Pool.") + len("    std.pool.Pool.")
pit = std_items(off, 422)
pd = {i["label"]: (i.get("detail"), i.get("insertText")) for i in pit}
check("completion.typePath.poolMethods",
      set(pd) == {"init"}
      and pd.get("init", (None, None))[1] == "init(${1:gpa})$0",
      str(sorted(pd))[:200])

# 4/1 复核：std. 顶层暴露 Allocator/stackAlloc/heapAlloc（带 snippet），
# TypeKind/TypeInfo/FieldInfo 仍不出现（stduser 文档再点一次）
off = STDUSE.index("    std.println") + len("    std.")
top_lab = {i["label"] for i in std_items(off, 405)}
check("completion.stdTop.ctExposed",
      {"Allocator", "stackAlloc", "heapAlloc"} <= top_lab
      and not ({"TypeKind", "TypeInfo", "FieldInfo"} & top_lab)
      and {"print", "println", "heap", "io", "debug", "atomic"} <= top_lab,
      str(sorted(top_lab)))


# ---------------------------------------------------------------------------
# 13 std 类型全覆盖闸门
# 用户 2026-11-09 第 7 条："递归地去找这些函数、类型实现，不能一个一个地
# 找"。这里不写死样本：直接枚举 std/*.zx 里全部 public 类型（struct /
# union / enum / 泛型 `fn (T: type) type`），为每个生成
#   `std.<mod>.<Type>.`            三段类型路径
#   `var o = std.<mod>.<Type>.<factory>(); o.`   工厂实例
# 断言成员非空、方法一律带 snippet、hover 给的是真实定义。std 以后新增
# 类型自动进入覆盖，不用再补用例。
def std_public_types():
    import re as _re
    out = []
    std_dir = os.path.join(ROOT, "std")
    for fn in sorted(os.listdir(std_dir)):
        if not fn.endswith(".zx"):
            continue
        mod = "" if fn == "std.zx" else fn[:-3]
        txt = open(os.path.join(std_dir, fn), encoding="utf-8").read()
        for m in _re.finditer(r'^(pub )?const (\w+) =', txt, _re.M):
            if not m.group(1):
                continue
            name = m.group(2)
            rest = txt[m.end():m.end() + 160].lstrip()
            if rest.startswith("struct"):
                shape = "struct"
            elif rest.startswith("union"):
                shape = "union"
            elif rest.startswith("enum"):
                shape = "enum"
            elif _re.match(r'fn\s*\(', rest) and _re.search(r'\)\s*type', rest):
                shape = "generic"
            else:
                continue
            factory = None
            for fm in _re.finditer(r'(?:pub )?const (\w+) = fn \(([^)]*)\)\s*([^\{\n]*)',
                                   txt[m.end():]):
                if fm.group(2).strip().startswith("self"):
                    continue
                if fm.group(3).strip().lstrip("?!*& ") in ("Self", name):
                    factory = fm.group(1)
                    break
            out.append((mod, name, shape, factory))
    return out


cov_types = std_public_types()

# 8i std.json.Obj 单独守门：容器体大（fields + 6 个方法）、字段全 pub、
# norm 是唯一无 self 的静态成员。曾出现"整份覆盖率文档里唯独 Obj 双空"的
# 非确定性回归（2026-11-11 调试），这里用最小文档钉住它。
OBJTXT = ('const std = @mod("std");\n\npub const main = fn () u8 {\n'
          '    var o = std.json.Obj{ };\n    std.json.Obj.\n    o.\n    return 0;\n};\n')
objuri = "file:///" + os.path.join(SRC_DIR, "objprobe.zx").replace("\\", "/")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": objuri, "languageId": "zx", "version": 1, "text": OBJTXT}}})
cov_rid = 890
for _row, _col, _tag, _want in ((4, 17, "type", ["norm"]), (5, 6, "inst", None)):
    cov_rid += 1
    send({"jsonrpc": "2.0", "id": cov_rid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": objuri}, "position": {"line": _row, "character": _col}}})
    _r, _ = recv_id("stdObj", cov_rid)
    _it = _r["result"]["items"] if _r and _r.get("result") else []
    _labs = sorted({i["label"] for i in _it})
    if _tag == "type":
        check("stdObj.typeStatic", _labs == ["norm"], str(_labs))
    else:
        check("stdObj.instMembers",
              {"keys", "vals", "n", "cap", "alloc", "put", "get", "count"} <= set(_labs),
              str(_labs)[:200])

# 8j 嵌套 pub 类型逐段解析（用户 2026-11-11："嵌套的 pub 类型导出不全，
# 只能导出第一层的，第二层丢失类型，第三层根本没提示"）。旧实现把所有多段
# 路径都退化成"拿末段名字在全文里配容器"：`A.B.Cfg` 撞上顶层同名 Cfg（第三层
# 空列表）、`A.B.Kind` 变体串成外层 Kind 的（第二层类型串味）。现在每一段只在
# 上一层的体里找（resolveContainerPath）。
DEEPLIB = """pub const Cfg = struct {
    pub w: i32,
};

pub const A = struct {
    pub const Kind = enum { ka, kb };
    pub const B = struct {
        pub const Kind = enum { kb1, kb2 };
        pub const Cfg = struct {
            pub deep: i32,
            pub const mk = fn () i32 { return 1; };
        };
        pub const C = struct {
            pub const Kind = enum { kc1, kc2 };
            pub k: i32;
        };
    };
};
"""
open(os.path.join(SRC_DIR, "deep3.zx"), "w", encoding="utf-8").write(DEEPLIB)
DEEPUSE = """const m = @mod("deep3");
const AB = m.A.B;

pub const main = fn () u8 {
    m.A.B.
    m.A.B.Cfg.
    m.A.B.C.Kind.
    m.A.B.Kind.
    AB.
    let x = m.A.B.Cfg{ .deep = 1 };
    x.
    _ = x.deep;
    return 0;
};
"""
deepuri = "file:///" + os.path.join(SRC_DIR, "deepuse.zx").replace("\\", "/")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": deepuri, "languageId": "zx", "version": 1, "text": DEEPUSE}}})
_rid = 930
for _recv, _want, _tag in (
        ("m.A.B.", {"Cfg", "C", "Kind", "Cfg{...}", "C{...}"}, "deep.l2Types"),
        ("m.A.B.Cfg.", {"mk"}, "deep.l3Static"),
        ("m.A.B.C.Kind.", {"kc1", "kc2"}, "deep.l3Enum"),
        ("m.A.B.Kind.", {"kb1", "kb2"}, "deep.l2Enum"),
        ("AB.", {"Cfg", "C", "Kind", "Cfg{...}", "C{...}"}, "deep.aliasType"),
        ("x.", {"deep"}, "deep.instField")):
    _rid += 1
    _off = DEEPUSE.index("    %s\n" % _recv) + len("    ") + len(_recv)
    send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": deepuri}, "position": pos(DEEPUSE, _off)}})
    _r, _ = recv_id("deep3", _rid)
    _labs = sorted({i["label"] for i in (_r["result"]["items"] if _r and _r.get("result") else [])})
    check("completion." + _tag, set(_labs) == _want, str(_labs))

# 8k 嵌套实例的字段 hover：`x.deep` 要报 deep: i32（容器体逐段解析过才
# 知道 x 是 A.B.Cfg，不是顶层同名 Cfg）。
_rid += 1
_off = DEEPUSE.index("    _ = x.deep;") + len("    _ = x.")
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/hover", "params": {
    "textDocument": {"uri": deepuri}, "position": pos(DEEPUSE, _off)}})
r, _ = recv_id("deep3", _rid)
_hv = r["result"]["contents"]["value"] if r and r.get("result") else ""
check("hover.deepNestField", "deep: i32" in _hv, _hv[:120])

# 8l 实例链上的嵌套类型与类型值（用户 2026-11-11：
# "可以链式读取后面的类型/函数，但是变量接收类型时不提示类型；类型和函数
#  也不能跳转"）。编译器侧同一形状的用例是 .tmp 探针；这里守 LSP：
#   r.            → 字段 top
#   r.top.        → 静态成员 MidT + 方法 f1
#   r.top.MidT.   → LeafT + f2（第二层还能继续）
#   const P = r.top.MidT;  P.  → 类型值只出静态成员（LeafT/f2）
#   let q = P.LeafT;       q.  → 实例字段 d
#   inlayHint：P / q 都要标 `: type`
#   definition：`P.LeafT` / `r.top.MidT.LeafT.mk` 落到 chain3.zx 的声明
CHAINLIB = """pub const ChainLeaf = struct {
    pub d: i32,
    pub const cmk = fn (v: i32) i32 { return v; };
};

pub const ChainMid = struct {
    pub b: i32,
    pub const LeafT = ChainLeaf;
    pub const cf2 = fn () i32 { return 2; };
};

pub const ChainTop = struct {
    pub a: i32,
    pub const MidT = ChainMid;
    pub const cf1 = fn () i32 { return 1; };
};

pub const ChainRoot = struct {
    pub top: ChainTop,
    pub const chainMake = fn () ChainRoot {
        return ChainRoot{ .top = ChainTop{ .a = 0 } };
    };
};
"""
open(os.path.join(SRC_DIR, "chain3.zx"), "w", encoding="utf-8").write(CHAINLIB)
CHAINUSE = """const c = @mod("chain3");

pub const main = fn () u8 {
    let r = c.ChainRoot.chainMake();
    r.
    r.top.
    r.top.MidT.
    const P = r.top.MidT;
    P.
    let q = P.LeafT;
    q.
    let v = P.LeafT{ .d = 3 };
    v.
    _ = r.top.MidT.LeafT.cmk(1);
    _ = v.d;
    return 0;
};
"""
chainuri = "file:///" + os.path.join(SRC_DIR, "chainuse.zx").replace("\\", "/")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": chainuri, "languageId": "zx", "version": 1, "text": CHAINUSE}}})
_rid = 960
for _recv, _want, _tag in (
        ("r.", {"top"}, "instChain.l1Field"),
        ("r.top.", {"MidT", "cf1"}, "instChain.l2Types"),
        ("r.top.MidT.", {"LeafT", "cf2"}, "instChain.l3Types"),
        ("P.", {"LeafT", "cf2"}, "instChain.typeValued"),
        ("q.", {"cmk"}, "instChain.typeValuedAlias"),
        ("v.", {"d"}, "instChain.typeValuedInst")):
    _rid += 1
    _off = CHAINUSE.index("    %s\n" % _recv) + len("    ") + len(_recv)
    send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": chainuri}, "position": pos(CHAINUSE, _off)}})
    _r, _ = recv_id("chain3", _rid)
    _labs = sorted({i["label"] for i in (_r["result"]["items"] if _r and _r.get("result") else [])})
    check("completion." + _tag, set(_labs) == _want, str(_labs))

# inlay：`const P = r.top.MidT;` / `let q = P.LeafT;` 都是类型值 → `: type`
_rid += 1
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/inlayHint", "params": {
    "textDocument": {"uri": chainuri},
    "range": {"start": {"line": 0, "character": 0},
              "end": {"line": len(CHAINUSE.split("\n")), "character": 0}}}})
_r, _ = recv_id("chain3", _rid)
_ih = {}
if _r and _r.get("result"):
    for _h in _r["result"]:
        _lbl = _h.get("label")
        if isinstance(_lbl, dict):
            _parts = _lbl.get("value", "") + "".join(p.get("value", "") for p in _lbl.get("labelParts", []) if isinstance(p, dict))
        else:
            _parts = _lbl or ""
        _ih[_h["position"]["line"]] = _parts
check("inlayHint.instChain.typeValueP", _ih.get(7) == ": type", str(_ih))
check("inlayHint.instChain.typeValueQ", _ih.get(9) == ": type", str(_ih))

# definition：类型成员与函数都能跳到 chain3.zx 的真实声明行
for _tag, _needle in (
        ("def.typeValueMember", "P.LeafT"),
        ("def.chainFn", "r.top.MidT.LeafT.cmk"),
        ("def.chainType", "r.top.MidT.")):
    _rid += 1
    # 光标停在最后一段上
    _last = _needle.rstrip(".").split(".")[-1]
    _off = CHAINUSE.index(_needle) + _needle.rindex(_last)
    send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/definition", "params": {
        "textDocument": {"uri": chainuri}, "position": pos(CHAINUSE, _off)}})
    _r, _ = recv_id("chain3", _rid)
    _loc = (_r or {}).get("result") or {}
    _ok = (isinstance(_loc, dict) and "chain3.zx" in (_loc.get("uri") or "")
           and _loc.get("range", {}).get("start", {}).get("line") is not None)
    check("definition." + _tag, _ok, str(_loc)[:200])

# definition：std 里的类型/函数也要跳（泛型实例上的成员、再导出容器里的成员）。
# `HashMap(i32).init` 的接收者带实例化实参，以前 base 只剩空的，一律 MISS；
# `std.json.Obj.norm` 走的是 std.zx 的 `pub const json = @mod("json.zx")`
# 再导出，名字在全扫描集里撞不出正确文件。
DEFSTD = """const std = @mod("std");

const main = fn () i32 {
    let al = std.heap.page;
    var m = std.map.HashMap(i32).init(al);
    let h = std.json.Obj.norm(5);
    std.println("{} {}", m.count(), h);
    return 0;
};
"""
defuri = "file:///" + os.path.join(SRC_DIR, "defstd.zx").replace("\\", "/")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": defuri, "languageId": "zx", "version": 1, "text": DEFSTD}}})
for _tag, _path, _file in (
        ("def.genericInstMethod", "std.map.HashMap(i32).init", "map.zx"),
        ("def.genericInstType", "std.map.HashMap(i32)", "map.zx"),
        ("def.reexportTypeMember", "std.json.Obj.norm", "json.zx"),
        ("def.reexportType", "std.json.Obj", "json.zx")):
    _rid += 1
    _w = _path.split(".")[-1]                  # 末段，可能是 `init` 或 `HashMap(i32)`
    if _w.endswith(")"):
        _w = _w[:_w.rindex("(")]               # 停在容器名上，不停在 `i32` 上
    _off = DEFSTD.index(_path) + _path.rindex(_w)
    send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/definition", "params": {
        "textDocument": {"uri": defuri}, "position": pos(DEFSTD, _off)}})
    _r, _ = recv_id("defstd", _rid)
    _loc = (_r or {}).get("result") or {}
    if isinstance(_loc, list):
        _loc = _loc[0] if _loc else {}
    _ok = (isinstance(_loc, dict) and _file in (_loc.get("uri") or "")
           and _loc.get("range", {}).get("start", {}).get("line") is not None)
    check("definition." + _tag, _ok, str(_loc)[:200])


# 形参 + 链式 base：`const P = t.PcP1.PcP2;`（t 是 *T 形参）也要提示 `: type`。
# 守两件事：containerOfBinding 认形参（findDeclStmt 够不着，靠 name: Type 形状扫），
# inferExprType 把链式 base（`t.PcP1`）当容器路径解。缺任一条，
# `const P = t.P1.P2;` 就一个字提示都没有（用户 2026-11-11 截图 1）。
PCHAIN = """const std = @mod("std");

const PcT = struct {
    pub const PcP1 = struct {
        pub const PcP2 = struct {
            pub const PcP3 = struct {
                pub const f3 = fn (i32, struct{a:u32,b:u32}) i32 { return 1; };
                pub const g = fn (n: i32) i32 { return n; };
            };
        };
    };
};

const pcUse = fn (t: *PcT) i32 {
    const P = t.PcP1.PcP2;
    const Pp = P.PcP3;
    return Pp.f3(1, .{ .a = 2, .b = 3 }) + Pp.g(1);
};
"""
pcuri = "file:///" + os.path.join(SRC_DIR, "paramchain.zx").replace("\\", "/")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": pcuri, "languageId": "zx", "version": 1, "text": PCHAIN}}})
_lineP = PCHAIN[:PCHAIN.index("const P = t.PcP1")].count("\n")
_linePp = PCHAIN[:PCHAIN.index("const Pp = P.PcP3")].count("\n")
_rid += 1
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/inlayHint", "params": {
    "textDocument": {"uri": pcuri},
    "range": {"start": {"line": 0, "character": 0},
              "end": {"line": len(PCHAIN.split("\n")), "character": 0}}}})
_r, _ = recv_id("paramchain", _rid)
_pih = {}
if _r and _r.get("result"):
    for _h in _r["result"]:
        _lbl = _h.get("label")
        _txt = _lbl if isinstance(_lbl, str) else (
            _lbl.get("value", "") + "".join(x.get("value", "") for x in _lbl.get("labelParts", []) if isinstance(x, dict)))
        _pih[_h["position"]["line"]] = _txt
check("inlayHint.paramChain.localTypeP", _pih.get(_lineP) == ": type", str(_pih))
check("inlayHint.paramChain.localTypePp", _pih.get(_linePp) == ": type", str(_pih))

# `Pp.` 的成员 + 无名形参的骨架插入文本
_rid += 1
_pfoff = PCHAIN.index("Pp.f3") + len("Pp.")
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": pcuri}, "position": pos(PCHAIN, _pfoff)}})
_r, _ = recv_id("paramchain", _rid)
_pitems = ((_r or {}).get("result") or {}).get("items") or []
_pby = {i["label"]: i for i in _pitems}
check("completion.paramChain.localTypeValue",
      set(_pby) == {"f3", "g"}, str(sorted(_pby)))
# 无名形参不是空占位：占位里放类型（`i32`），匿名聚合写成 `.{ }`，
# `}` 走 escapeSnippet 转义 —— 不转义会提前关闭 tabstop，插入变半截
# （用户 2026-11-11：先"struct{a"截断，再"只留占位没文字"，两头都踩过）。
check("snippet.paramChain.unnamedParams",
      _pby.get("f3", {}).get("insertText") == "f3(${1:i32}, ${2:.{ \}})$0",
      str(_pby.get("f3", {}).get("insertText")))
check("snippet.paramChain.namedParam",
      _pby.get("g", {}).get("insertText") == "g(${1:n})$0",
      str(_pby.get("g", {}).get("insertText")))

# 模块当接收者：`const P = t.P1.P2;`（t 是 module）也要 `: type`，`P.` 要出成员。
# 这是**另一条链路**：base 是 "模块名.容器名"，既不是模块绑定（moduleOfBinding
# 只认 "t"）也不是本地容器，inferExprType 以前把它整段跳过，只剩模块那一支
# 一律 `return e` —— 于是提示要么空、要么是 "t.Inner" 这种路径文本
#（用户 2026-11-11 第 4 张截图："如果 t 是 module，还是不会显示"）。
MCMOD = """pub const McRoot = struct {
    pub const McP1 = struct {
        pub const McP2 = struct {
            pub const McP3 = struct {
                pub const mf = fn (n: i32) i32 { return n; };
            };
        };
    };
};

pub const McAlias = McRoot.McP1.McP2;
"""
open(os.path.join(SRC_DIR, "modchain.zx"), "w", encoding="utf-8").write(MCMOD)
MCUSE = """const m = @mod("modchain");

const mcUse = fn () i32 {
    const McP = m.McRoot.McP1.McP2;
    const McQ = m.McRoot;
    const McA = m.McAlias;
    McP.
    McQ.
    m.McRoot.McP1.
    _ = McP.McP3.mf(1);
    return 0;
};
"""
mcuri = "file:///" + os.path.join(SRC_DIR, "moduse.zx").replace("\\", "/")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": mcuri, "languageId": "zx", "version": 1, "text": MCUSE}}})
_lineMcP = MCUSE[:MCUSE.index("const McP =")].count("\n")
_lineMcQ = MCUSE[:MCUSE.index("const McQ =")].count("\n")
_lineMcA = MCUSE[:MCUSE.index("const McA =")].count("\n")
_rid += 1
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/inlayHint", "params": {
    "textDocument": {"uri": mcuri},
    "range": {"start": {"line": 0, "character": 0},
              "end": {"line": len(MCUSE.split("\n")), "character": 0}}}})
_r, _ = recv_id("modchain", _rid)
_mih = {}
if _r and _r.get("result"):
    for _h in _r["result"]:
        _lblm = _h.get("label")
        _txtm = _lblm if isinstance(_lblm, str) else (
            _lblm.get("value", "") + "".join(x.get("value", "") for x in _lblm.get("labelParts", []) if isinstance(x, dict)))
        _mih[_h["position"]["line"]] = _txtm
check("inlayHint.modChain.typeValueP", _mih.get(_lineMcP) == ": type", str(_mih))
check("inlayHint.modChain.typeValueQ", _mih.get(_lineMcQ) == ": type", str(_mih))
check("inlayHint.modChain.typeValueAlias", _mih.get(_lineMcA) == ": type", str(_mih))

for _recv, _want, _tag in (
        ("McP.", {"McP3"}, "modChain.typeValueMembers"),
        ("McQ.", {"McP1"}, "modChain.typeValueMembersQ"),
        ("m.McRoot.McP1.", {"McP2"}, "modChain.modulePath")):
    _rid += 1
    _offm = MCUSE.index("    %s\n" % _recv) + len("    ") + len(_recv)
    send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": mcuri}, "position": pos(MCUSE, _offm)}})
    _r, _ = recv_id("modchain", _rid)
    _labsm = sorted({i["label"] for i in (_r["result"]["items"] if _r and _r.get("result") else [])})
    check("completion." + _tag, set(_labsm) == _want, str(_labsm))

# 模块链上的成员也要能跳（`McP.McP3` → modchain.zx 的嵌套类型声明）
_rid += 1
_mneed = "McP.McP3"
_moff = MCUSE.index(_mneed) + _mneed.rindex("McP3")
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/definition", "params": {
    "textDocument": {"uri": mcuri}, "position": pos(MCUSE, _moff)}})
_r, _ = recv_id("modchain", _rid)
_mloc = (_r or {}).get("result") or {}
if isinstance(_mloc, list):
    _mloc = _mloc[0] if _mloc else {}
check("definition.modChain.member",
      isinstance(_mloc, dict) and "modchain.zx" in (_mloc.get("uri") or ""),
      str(_mloc)[:200])

# 显式 `: type` 标注的类型值绑定：`const XtP: type = xt.XtRoot.XtP1.XtP2;`
# 标注本身是 "type"，不是容器路径。三处都得看**初始式**而不是标注：
#   - instancePathContainer 单段解析（`XtP.` 补成员，以前空列表）
#   - inferExprType 点路径（`const XtQ = XtP.XtP3;` 的 `: type`）
#   - definitionOfMember：`XtP.XtP3.xf3` 的 `xf3` 不能被本文件同名 const 抢
#     （用户 2026-11-11：`t` 是 module，函数跳不过去）。
XTMOD = """pub const XtRoot = struct {
    pub const XtP1 = struct {
        pub const XtP2 = struct {
            pub const XtP3 = struct {
                pub const xf3 = fn (i32, struct{a:u32,b:u32}) i32 { return 1; };
                pub const xg = fn (n: i32) i32 { return n; };
            };
        };
    };
};
"""
open(os.path.join(SRC_DIR, "xtmod.zx"), "w", encoding="utf-8").write(XTMOD)
XTUSE = """const std = @mod("std");
const xt = @mod("xtmod");

const xtUse = fn () i32 {
    const XtP: type = xt.XtRoot.XtP1.XtP2;
    const XtQ = XtP.XtP3;
    XtP.
    XtP.XtP3.
    const XtF = XtP.XtP3.xf3(1, .{ .a = 2, .b = 3 });
    return XtQ.xg(1) + XtF;
};
"""
xturi = "file:///" + os.path.join(SRC_DIR, "xtuse.zx").replace("\\", "/")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": xturi, "languageId": "zx", "version": 1, "text": XTUSE}}})
_lineXtQ = XTUSE[:XTUSE.index("const XtQ =")].count("\n")
_rid += 1
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/inlayHint", "params": {
    "textDocument": {"uri": xturi},
    "range": {"start": {"line": 0, "character": 0},
              "end": {"line": len(XTUSE.split("\n")), "character": 0}}}})
_r, _ = recv_id("xtuse", _rid)
_xih = {}
if _r and _r.get("result"):
    for _h in _r["result"]:
        _lbx = _h.get("label")
        if isinstance(_lbx, str):
            _xih[_h["position"]["line"]] = _lbx
check("inlayHint.explicitType.localTypeQ", _xih.get(_lineXtQ) == ": type", str(_xih))

for _recv, _want, _tag in (
        ("XtP.", {"XtP3"}, "explicitType.members"),
        ("XtP.XtP3.", {"xf3", "xg"}, "explicitType.membersL2")):
    _rid += 1
    _offx = XTUSE.index("    %s\n" % _recv) + len("    ") + len(_recv)
    send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": xturi}, "position": pos(XTUSE, _offx)}})
    _r, _ = recv_id("xtuse", _rid)
    _labsx = sorted({i["label"] for i in ((_r or {}).get("result") or {}).get("items") or []})
    check("completion." + _tag, set(_labsx) == _want, str(_labsx))

_rid += 1
_xoff = XTUSE.index("xf3(1") + 1
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/definition", "params": {
    "textDocument": {"uri": xturi}, "position": pos(XTUSE, _xoff)}})
_r, _ = recv_id("xtuse", _rid)
_xloc = (_r or {}).get("result") or {}
if isinstance(_xloc, list):
    _xloc = _xloc[0] if _xloc else {}
check("definition.explicitType.fnInModule",
      isinstance(_xloc, dict) and "xtmod.zx" in (_xloc.get("uri") or ""),
      str(_xloc)[:200])

# ---------------------------------------------------------------- @type 描述符
#
# 用户 2026-11-11 第 2 张截图：`const info = @type(T); info.` 一个成员都不出。
# 编译器内建类型（Allocator / Type / Field）在源文件里没有声明，
# 成员表只能硬编码（builtin_ty_table，与 src/sema.c 的 sema_type_type /
# sema_type_field_type 逐字段对齐）。三条链路缺一不可：
#   inferExprType 先认 `@type(`——落到"调用"分支会当普通函数、返回 null
#   builtinTyMembers 出 Type/Field 的字段（补全）
#   builtinFieldTy 供 `info.fields` / `info.fields[0]` 继续推（inlay）
TYUSE = """const std = @mod("std");

const TyMake = fn (T: type) type {
    return struct{
        const info = @type(T);
        const tField = info.fields[0];
        const tCount = info.fieldCount;
        info.
        info.fields[0].
    };
};
"""
tyuri = "file:///" + os.path.join(SRC_DIR, "typeuse.zx").replace("\\", "/")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": tyuri, "languageId": "zx", "version": 1, "text": TYUSE}}})
_rid += 1
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/inlayHint", "params": {
    "textDocument": {"uri": tyuri},
    "range": {"start": {"line": 0, "character": 0},
              "end": {"line": len(TYUSE.split("\n")), "character": 0}}}})
_r, _ = recv_id("typeuse", _rid)
_tih = {}
if _r and _r.get("result"):
    for _h in _r["result"]:
        _lbx = _h.get("label")
        if isinstance(_lbx, str):
            _tih[_h["position"]["line"]] = _lbx
for _need, _want, _tag in (
        ("const info = @type", ": Type", "descriptor.infoType"),
        ("const tField = ", ": Field", "descriptor.fieldElem"),
        ("const tCount = ", ": usize", "descriptor.fieldCount")):
    _ln = TYUSE[:TYUSE.index(_need)].count("\n")
    check("inlayHint." + _tag, _tih.get(_ln) == _want, str(_tih))

for _recv, _want, _tag in (
        ("info.", {"name", "size", "align", "ty", "fieldCount", "fields",
                   "statics"},
         "descriptor.typeMembers"),
        ("info.fields[0].", {"name", "ty", "offset", "ptr"},
         "descriptor.fieldMembers")):
    _rid += 1
    _offt = TYUSE.index("        %s\n" % _recv) + 8 + len(_recv)
    send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": tyuri}, "position": pos(TYUSE, _offt)}})
    _r, _ = recv_id("typeuse", _rid)
    _labt = sorted({i["label"] for i in ((_r or {}).get("result") or {}).get("items") or []})
    check("completion." + _tag, set(_labt) == _want, str(_labt))

# 类型值链的**源头**解析不到（`t` 不是模块绑定、模块文件不在扫描集里）：
# `resolveContainerPathLoose` 从末段往前退（`LsP1.LsP2` → `LsP2`），
# 否则 `const LsQ = LsP.LsP3;` 没有 `: type`、`LsQ.`/`LsP.` 空列表、
# `LsP.LsP3` 跳不过去（用户 2026-11-11 第 1 张截图："还是一样的；跳转也没有好"）。
LOSEMOD = """pub const LsP1 = struct {
    pub const LsP2 = struct {
        pub const LsP3 = struct {
            pub const lf3 = fn (n: i32) i32 { return n; };
            pub const lg3 = fn (n: i32) i32 { return n; };
        };
    };
};
"""
open(os.path.join(SRC_DIR, "losemod.zx"), "w", encoding="utf-8").write(LOSEMOD)
LOSESRC = """const std = @mod("std");

const LsP: type = t.LsP1.LsP2;
const LsQ = LsP.LsP3;
LsQ.
LsP.
const LsF = LsP.LsP3.lf3(1);
"""
loseuri = "file:///" + os.path.join(SRC_DIR, "loseuse.zx").replace("\\", "/")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": loseuri, "languageId": "zx", "version": 1, "text": LOSESRC}}})
_rid += 1
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/inlayHint", "params": {
    "textDocument": {"uri": loseuri},
    "range": {"start": {"line": 0, "character": 0},
              "end": {"line": len(LOSESRC.split("\n")), "character": 0}}}})
_r, _ = recv_id("loseuse", _rid)
_lih = {}
if _r and _r.get("result"):
    for _h in _r["result"]:
        _lbx = _h.get("label")
        if isinstance(_lbx, str):
            _lih[_h["position"]["line"]] = _lbx
_lineLsQ = LOSESRC[:LOSESRC.index("const LsQ =")].count("\n")
check("inlayHint.looseChain.localTypeQ", _lih.get(_lineLsQ) == ": type", str(_lih))

for _recv, _want, _tag in (
        ("LsQ.", {"lf3", "lg3"}, "looseChain.instMembers"),
        ("LsP.", {"LsP3"}, "looseChain.typeMembers")):
    _rid += 1
    _offl = LOSESRC.index("%s\n" % _recv) + len(_recv)
    send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": loseuri}, "position": pos(LOSESRC, _offl)}})
    _r, _ = recv_id("loseuse", _rid)
    _labl = sorted({i["label"] for i in ((_r or {}).get("result") or {}).get("items") or []})
    check("completion." + _tag, set(_labl) == _want, str(_labl))

_rid += 1
_loff = LOSESRC.index("const LsQ = LsP.LsP3") + len("const LsQ = LsP.")
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/definition", "params": {
    "textDocument": {"uri": loseuri}, "position": pos(LOSESRC, _loff)}})
_r, _ = recv_id("loseuse", _rid)
_lloc = (_r or {}).get("result") or {}
if isinstance(_lloc, list):
    _lloc = _lloc[0] if _lloc else {}
check("definition.looseChain.nestedType",
      isinstance(_lloc, dict) and "losemod.zx" in (_lloc.get("uri") or ""),
      str(_lloc)[:200])

# ---------------------------------------------------------- vendor 模块里的嵌套类型
#
# 用户 2026-11-11 第 7 张截图（真实工程：src/main.zx 里 `const t = @mod("test")`，
# 模块文件在 vendor/test.zx）：
#   const P: type = t.P1.P2;   →  P 有 : type，但 `const pp = P.P3;` 什么都没有
# 根因：scanFiles 只扫**文档目录 + std**，vendor 里的容器体不在扫描集里，于是
#   findContainerBody("P2") 落空 → inferExprType 的"类型值接收者"分支返回 null
#   （没有 : type）、resolveMemberPath 首段 P 找不到容器（`P.P3.f3(…)` 推不出
#   返回类型，`var f` 没有 : i32）、definition 拿不到声明文件（跳到本文件的行）。
# 修法：三条链统一改走 containerOfBinding（沿初值路径逐段解析，体在哪个文件都行）
# + pathOfSrcText 也搜 vendor/deps 目录。fixture 放 TESTDIR/vendor/，文档放 src/，
# 与真实工程一致——放同目录是测不出这条断链的。前缀 Vd* 避免与别的 fixture 撞名。
VDMOD = """pub const VdP1 = struct{

    pub const VdP2 = struct {
        name: i32,
        pub const VdP3 = struct {
            name: i32,
            pub const vf3 = fn (i32, struct{pub a: u32, pub b: u32}) i32 {
                return 0;
            };
        };
    };
};
"""
os.makedirs(os.path.join(TESTDIR, "vendor"), exist_ok=True)
open(os.path.join(TESTDIR, "vendor", "vendmod.zx"), "w", encoding="utf-8").write(VDMOD)
VDSRC = """const std = @mod("std");
const vt = @mod("vendmod");

const VdP: type = vt.VdP1.VdP2;
const VdQ = VdP.VdP3;
_ = VdQ;
var VdF = VdP.VdP3.vf3(10, .{ .a = 10, .b = 10 });
_ = VdF;
VdQ.
VdP.
"""
vduri = "file:///" + os.path.join(SRC_DIR, "venduse.zx").replace("\\", "/")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": vduri, "languageId": "zx", "version": 1, "text": VDSRC}}})
_rid += 1
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/inlayHint", "params": {
    "textDocument": {"uri": vduri},
    "range": {"start": {"line": 0, "character": 0},
              "end": {"line": len(VDSRC.split("\n")), "character": 0}}}})
_r, _ = recv_id("venduse", _rid)
_vih = {}
if _r and _r.get("result"):
    for _h in _r["result"]:
        _lbv = _h.get("label")
        if isinstance(_lbv, str):
            _vih[_h["position"]["line"]] = _lbv
check("inlayHint.vendorChain.localTypeQ",
      _vih.get(VDSRC[:VDSRC.index("const VdQ =")].count("\n")) == ": type", str(_vih))
check("inlayHint.vendorChain.callRet",
      _vih.get(VDSRC[:VDSRC.index("var VdF =")].count("\n")) == ": i32", str(_vih))

for _recv, _want, _tag in (
        ("VdQ.", {"vf3"}, "vendorChain.instMembers"),
        ("VdP.", {"VdP3"}, "vendorChain.typeMembers")):
    _rid += 1
    _offv = VDSRC.index("%s\n" % _recv) + len(_recv)
    send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": vduri}, "position": pos(VDSRC, _offv)}})
    _r, _ = recv_id("venduse", _rid)
    _labv = sorted({i["label"] for i in ((_r or {}).get("result") or {}).get("items") or []})
    check("completion." + _tag, set(_labv) == _want, str(_labv))

_rid += 1
_voff = VDSRC.index("const VdQ = VdP.") + len("const VdQ = VdP.")
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/definition", "params": {
    "textDocument": {"uri": vduri}, "position": pos(VDSRC, _voff)}})
_r, _ = recv_id("venduse", _rid)
_vloc = (_r or {}).get("result") or {}
if isinstance(_vloc, list):
    _vloc = _vloc[0] if _vloc else {}
check("definition.vendorChain.nestedType",
      isinstance(_vloc, dict) and "vendmod.zx" in (_vloc.get("uri") or ""),
      str(_vloc)[:200])

# ------------------------------------------------ 匿名 struct 实参里的字段（模块在 vendor/deps）
#
# 用户 2026-11-11 第 1 张截图：`P.P3.f3(0, .{ .a = 0, .b = 0 })` 类型提示有了，
# 但 `.{ .` 里不出字段。被调的声明在 vendor/deps 的模块文件里，
# anonParamTy 只在"文档目录 + 当前文件 + 模块绑定文件"里找，于是落空。
# 两条补法一起上：被调接收者按容器路径解析（containerOfBinding /
# resolveContainerPathLoose 拿到容器所在文件的文本）+ 兜底扫 modDirsInto
# 的模块目录（vendor、zx.json 的 deps、工程根）。
_rid += 1
_offa = VDSRC.index(".{ .a = 10") + len(".{ .")
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": vduri}, "position": pos(VDSRC, _offa)}})
_r, _ = recv_id("venduse", _rid)
_laba = sorted({i["label"] for i in ((_r or {}).get("result") or {}).get("items") or []})
check("completion.vendorChain.anonArgFields", set(_laba) == {"a", "b"}, str(_laba))

# zx.json 的 deps（TESTDIR/zx.json 里写的是 "deps": ["libs"]）：
# 模块文件完全不在文档目录下，只有 deps 这一条链能把它找回来。
DPMOD = """pub const DpRoot = struct{

    pub const DpMid = struct {
        pub const DpLeaf = struct {
            pub const df3 = fn (i32, struct{pub a: u32, pub b: u32}) i32 {
                return 0;
            };
        };
    };
};
"""
open(os.path.join(TESTDIR, "libs", "deepmod.zx"), "w", encoding="utf-8").write(DPMOD)
DPSRC = """const std = @mod("std");
const dm = @mod("deepmod");

const DpP: type = dm.DpRoot.DpMid;
const DpQ = DpP.DpLeaf;
_ = DpQ;
var DpF = DpP.DpLeaf.df3(1, .{ .a = 1, .b = 2 });
_ = DpF;
DpQ.
var DpLit = DpP.DpLeaf.df3(1, .{ .a = 1, .b = 2 });
_ = DpLit;
"""
dpuri = "file:///" + os.path.join(SRC_DIR, "depsuse.zx").replace("\\", "/")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": dpuri, "languageId": "zx", "version": 1, "text": DPSRC}}})
_rid += 1
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/inlayHint", "params": {
    "textDocument": {"uri": dpuri},
    "range": {"start": {"line": 0, "character": 0},
              "end": {"line": len(DPSRC.split("\n")), "character": 0}}}})
_r, _ = recv_id("depsuse", _rid)
_dih = {}
if _r and _r.get("result"):
    for _h in _r["result"]:
        _lbdd = _h.get("label")
        if isinstance(_lbdd, str):
            _dih[_h["position"]["line"]] = _lbdd
check("inlayHint.depsChain.localTypeQ",
      _dih.get(DPSRC[:DPSRC.index("const DpQ =")].count("\n")) == ": type", str(_dih))
check("inlayHint.depsChain.callRet",
      _dih.get(DPSRC[:DPSRC.index("var DpF =")].count("\n")) == ": i32", str(_dih))

_rid += 1
_offdp = DPSRC.index(".{ .a = 1") + len(".{ .")
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": dpuri}, "position": pos(DPSRC, _offdp)}})
_r, _ = recv_id("depsuse", _rid)
_labdp = sorted({i["label"] for i in ((_r or {}).get("result") or {}).get("items") or []})
check("completion.depsChain.anonArgFields", set(_labdp) == {"a", "b"}, str(_labdp))

_rid += 1
_offdq = DPSRC.index("DpQ.\n") + len("DpQ.")
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": dpuri}, "position": pos(DPSRC, _offdq)}})
_r, _ = recv_id("depsuse", _rid)
_labq = sorted({i["label"] for i in ((_r or {}).get("result") or {}).get("items") or []})
check("completion.depsChain.instMembers", "df3" in set(_labq), str(_labq))

_rid += 1
_dpoff = DPSRC.index("const DpQ = DpP.") + len("const DpQ = DpP.")
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/definition", "params": {
    "textDocument": {"uri": dpuri}, "position": pos(DPSRC, _dpoff)}})
_r, _ = recv_id("depsuse", _rid)
_dloc = (_r or {}).get("result") or {}
if isinstance(_dloc, list):
    _dloc = _dloc[0] if _dloc else {}
check("definition.depsChain.nestedType",
      isinstance(_dloc, dict) and "deepmod.zx" in (_dloc.get("uri") or ""),
      str(_dloc)[:200])

# ------------------------------------------------ 泛型形参接收者（§117）
#
# 用户 2026-11-11 第 9 张截图：`const fill = fn (T: type, val: T) void
# { let v = val.a; }` —— 定义处不知道 T 是什么，`val.` 一个成员都不出、
# `val.a` 推不出类型（inlay 没有 : i32）。修法是从扫描集里的调用点反推：
# `fill(P3, p)` 的 **T 位**实参就是具体类型，解析成容器后按实例出成员。
# 注意取的是类型形参那一位的实参，不是 val 自己那位（那是值）。
GPSRC = """const std = @mod("std");

const GpT = struct {
    ga: i32,
    gb: []const u8,
};

const gpfill = fn (T: type, val: T) void {
    let gv = val.ga;
    _ = gv;
    val.
};

pub const main = fn () u8 {
    const gp = GpT{ .ga = 1, .gb = "x" };
    gpfill(GpT, gp);
    return 0;
};
"""
gpuri = "file:///" + os.path.join(SRC_DIR, "genparam.zx").replace("\\", "/")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": gpuri, "languageId": "zx", "version": 1, "text": GPSRC}}})

_rid += 1
_gpoff = GPSRC.index("val.\n") + len("val.")
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": gpuri}, "position": pos(GPSRC, _gpoff)}})
_r, _ = recv_id("genparam", _rid)
_labgp = sorted({i["label"] for i in ((_r or {}).get("result") or {}).get("items") or []})
check("completion.genparam.valFields", set(_labgp) == {"ga", "gb"}, str(_labgp))

_rid += 1
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/inlayHint", "params": {
    "textDocument": {"uri": gpuri},
    "range": {"start": {"line": 0, "character": 0},
              "end": {"line": len(GPSRC.split("\n")), "character": 0}}}})
_r, _ = recv_id("genparam", _rid)
_gpih = {}
if _r and _r.get("result"):
    for _h in _r["result"]:
        _lbgp = _h.get("label")
        if isinstance(_lbgp, str):
            _gpih[_h["position"]["line"]] = _lbgp
check("inlayHint.genparam.fieldTy",
      _gpih.get(GPSRC[:GPSRC.index("let gv =")].count("\n")) == ": i32", str(_gpih))

# 反向守门：没有调用点就保持空列表——宁缺毋滥，绝不回落全局标识符噪音。
GPQSRC = """const GpQ = struct { qc: u8 };

const gpquiet = fn (T: type, val: T) void {
    val.
};

pub const main = fn () u8 {
    return 0;
};
"""
gpquri = "file:///" + os.path.join(SRC_DIR, "genparam_quiet.zx").replace("\\", "/")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": gpquri, "languageId": "zx", "version": 1, "text": GPQSRC}}})
_rid += 1
_gpqoff = GPQSRC.index("val.\n") + len("val.")
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": gpquri}, "position": pos(GPQSRC, _gpqoff)}})
_r, _ = recv_id("genparam_quiet", _rid)
_labgpq = sorted({i["label"] for i in ((_r or {}).get("result") or {}).get("items") or []})
check("completion.genparam.noCallSite", _labgpq == [], str(_labgpq))

# ------------------------------------------------- enum/union 静态成员与方法
# enum / union(enum) 体里同样有静态成员（const/var）、方法（首参 self）与
# 关联函数（用户 2026-11-11：补全不许漏）。以前 extractBraceMembers 把
# enum 体里的所有成员都标成 .variant，方法没有 is_fn → 没骨架没签名，
# 命名空间过滤也失效。命名空间语义与 struct 完全一致：
#   `EuColor.`  变体 + 静态常量 + 关联函数（self 方法不出）
#   `c.`        变体 + 静态常量 + 方法（关联函数不出）
EUSRC = """const EuColor = enum {
    red, green = 5, blue,

    const FIRST = EuColor.red;
    const COUNT = 3;

    const name = fn (self: Self) []const u8 {
        return switch (self) {
            .red: "red";
            .green: "green";
            .blue: "blue";
        };
    };

    const count = fn () i32 { return EuColor.COUNT; };
};

const EuShape = union(enum) {
    circle: f64,
    rect: { f64, f64 },

    const describe = fn (self: Self) []const u8 {
        switch (self.tag) {
            .circle: { return "circle"; };
            .rect: { return "rect"; };
        }
    };
};

pub const main = fn () u8 {
    const c = EuColor.green;
    const s = EuShape{ .circle = 2.0 };
    EuColor.
    c.
    EuShape.
    s.
    const cn = c.name();
    return 0;
};
"""
euuri = "file:///" + os.path.join(SRC_DIR, "enumunion.zx").replace("\\", "/")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": euuri, "languageId": "zx", "version": 1, "text": EUSRC}}})

for _anchor, _want, _tag in (
        ("    EuColor.\n", {"FIRST", "COUNT", "count", "red", "green", "blue"},
         "enum.typeMembers"),
        # 2026-11-12：静态值只属于类型命名空间（与 struct 关联常量同一条
        # 红线），实例上只留变体 + self 方法。旧断言曾期望 FIRST/COUNT。
        ("    c.\n", {"name", "red", "green", "blue"},
         "enum.instMembers"),
        ("    EuShape.\n", {"circle", "rect"}, "unionenum.typeMembers"),
        ("    s.\n", {"circle", "rect", "describe"}, "unionenum.instMembers")):
    _rid += 1
    _off = EUSRC.index(_anchor) + len(_anchor) - 1
    send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": euuri}, "position": pos(EUSRC, _off)}})
    _r, _ = recv_id("enumunion", _rid)
    _labeu = sorted({i["label"] for i in ((_r or {}).get("result") or {}).get("items") or []})
    check("completion." + _tag, set(_labeu) == _want, str(_labeu))

# 实例方法的 detail 必须带 self（与 struct 同一条红线：不带 self 就会被
# 当成关联函数——用户 2026-11-10 两次截图的污染判定标准）
_rid += 1
_euoff = EUSRC.index("    c.\n") + len("    c.")
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": euuri}, "position": pos(EUSRC, _euoff)}})
_r, _ = recv_id("enumunion", _rid)
_euby = {i["label"]: i for i in ((_r or {}).get("result") or {}).get("items") or []}
check("completion.enum.methodSelfDetail",
      "self" in (_euby.get("name", {}).get("detail") or ""),
      str(_euby.get("name", {}).get("detail")))
# 关联函数也有调用骨架（is_fn 升格后 memberCallSnippet 才会跑）
check("completion.enum.assocSnippet",
      _euby.get("name", {}).get("insertText") == "name()$0",
      str(_euby.get("name", {}).get("insertText")))

# hover：enum 实例方法 c.name → 签名卡
_rid += 1
_euoff = EUSRC.index("c.name();") + len("c.")
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/hover", "params": {
    "textDocument": {"uri": euuri}, "position": pos(EUSRC, _euoff)}})
_r, _ = recv_id("enumunion", _rid)
_euhv = ((_r or {}).get("result") or {}).get("contents", {}).get("value", "")
check("hover.enum.instanceMethod", "name" in _euhv and "self" in _euhv, _euhv[:120])

cov_lines = ['const std = @mod("std");', "", "pub const main = fn () u8 {"]
cov_marks = []
for _mod, _name, _shape, _factory in cov_types:
    _qn = ("std.%s.%s" % (_mod, _name)) if _mod else ("std.%s" % _name)
    _inst = "o_" + _name
    cov_lines.append('    var %s = %s%s;' % (
        _inst, _qn, (".%s()" % _factory) if _factory else "{ }"))
    cov_marks.append((_qn, _inst, _shape, len(cov_lines)))
    cov_lines.append("    %s." % _qn)
    cov_lines.append("    %s." % _inst)
cov_lines += ["    return 0;", "};", ""]
COVTXT = "\n".join(cov_lines) + "\n"
covuri = "file:///" + os.path.join(SRC_DIR, "stdtypes.zx").replace("\\", "/")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": covuri, "languageId": "zx", "version": 1, "text": COVTXT}}})

cov_empty, cov_nosnip, cov_badhover = [], [], []
cov_empty_pair = {}   # 类型 → [type 是否空, inst 是否空]
cov_rid = 900
for _qn, _inst, _shape, _ln in cov_marks:
    for _label, _row, _col in (("type", _ln, 4 + len(_qn) + 1),
                               ("inst", _ln + 1, 4 + len(_inst) + 1)):
        cov_rid += 1
        send({"jsonrpc": "2.0", "id": cov_rid, "method": "textDocument/completion",
              "params": {"textDocument": {"uri": covuri},
                         "position": {"line": _row, "character": _col}}})
        _r, _ = recv_id("stdCoverage", cov_rid)
        _items = _r["result"]["items"] if _r and _r.get("result") else []
        if not _items:
            _slot = cov_empty_pair.setdefault(_qn, [True, True])
            _slot[0 if _label == "type" else 1] = False
            continue
        # 第 13 条：函数只出一条——label 就是名字、kind=15、带 snippet。
        # 同名重复 / 残留裸名函数项（kind 2/3）/ snippet 缺 insertTextFormat
        # 都算回归；类型项（22/13）的 `Name{...}` 构造骨架不在此列。
        _labs = [i["label"] for i in _items]
        _miss = sorted({l for l in _labs if _labs.count(l) > 1})
        _miss += [i["label"] for i in _items
                  if i.get("kind") == 15 and i.get("insertTextFormat") != 2]
        _miss += [i["label"] for i in _items if i.get("kind") in (2, 3)]
        if _miss:
            cov_nosnip.append("%s(%s):%s" % (_qn, _label, _miss))
    # hover 停在类型名上：要贴真实定义（struct/union/enum/fn）
    cov_rid += 1
    send({"jsonrpc": "2.0", "id": cov_rid, "method": "textDocument/hover",
          "params": {"textDocument": {"uri": covuri},
                     "position": {"line": _ln, "character": 4 + _qn.rfind(".") + 2}}})
    _r, _ = recv_id("stdCoverage", cov_rid)
    _res = (_r or {}).get("result")
    _hv = ((_res or {}).get("contents") or {}).get("value", "") if isinstance(_res, dict) else ""
    if not any(k in _hv for k in ("struct", "union", "enum", "fn (")):
        cov_badhover.append("%s:%r" % (_qn, _hv.replace("\n", " ")[:60]))
send({"jsonrpc": "2.0", "method": "textDocument/didClose",
      "params": {"textDocument": {"uri": covuri}}})

check("stdCoverage.enumerated", len(cov_types) >= 12, "types=%d" % len(cov_types))
# 用户 2026-11-10：类型路径只出静态成员、实例路径只出实例成员——
#   `ArenaAllocator{ }` 没有实例成员（构造入口是静态 init）→ inst 空是正常的
#   `Stat`/`Obj`/`DateTime` 全是字段 → type 空是正常的
# 两边都空才说明解析链断了。
cov_empty = ["%s(type+inst)" % q for q, (t, i) in sorted(cov_empty_pair.items())
             if not t and not i]
check("stdCoverage.typePathMembers", not cov_empty, str(cov_empty)[:300])
check("stdCoverage.methodSnippets", not cov_nosnip, str(cov_nosnip)[:300])
check("stdCoverage.hoverDefinition", not cov_badhover, str(cov_badhover)[:300])


# ---------------------------------------------------------------------------
# 12b 链式接收者（用户 2026-11-09 第 8 条：`std.map.HashMap(i32).` 连写三段
# 没提示）。点补全的 path 回扫遇 ')' 就截止，接收者表达式解析不出 → 空列表。
# 修复后 recvExprStart 配平括号取整条接收者，inferContainer 统一解析：泛型实例
# / 模块函数返回 / 工厂调用 / 本地函数返回四种链式的成员与 hover 都必须有。
CHAIN = """const std = @mod("std");

const P = struct {
    x: i32,
};

const mk = fn () P { return P{ .x = 0 }; };

pub const main = fn () u8 {
    var HS = std.map.HashMap(i32).init(std.heap.page);
    _ = mk().x;
    _ = std.time.fromUnixMs(0).year;
    std.map.HashMap(i32).in
    std.map.HashMap(i32).
    std.time.fromUnixMs(0).
    std.BufReader.open(std.heap.page, "z", 64).?.
    mk().
    HS.
    return 0;
};
"""
chainuri = "file:///" + os.path.join(SRC_DIR, "chainuse.zx").replace("\\", "/")
open(os.path.join(SRC_DIR, "chainuse.zx"), "w", encoding="utf-8").write(CHAIN)
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": chainuri, "languageId": "zx", "version": 1, "text": CHAIN}}})

crid = 960


def chain_items(anchor):
    global crid
    crid += 1
    off = CHAIN.index(anchor) + len(anchor.rstrip("\n"))
    send({"jsonrpc": "2.0", "id": crid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": chainuri}, "position": pos(CHAIN, off)}})
    r, _ = recv_id("chain", crid)
    return r["result"]["items"] if r and r.get("result") else []


def chain_hover(anchor, within):
    global crid
    crid += 1
    a = CHAIN.index(anchor)
    off = a + CHAIN[a:].index(within) + len(within)
    send({"jsonrpc": "2.0", "id": crid, "method": "textDocument/hover", "params": {
        "textDocument": {"uri": chainuri}, "position": pos(CHAIN, off)}})
    r, _ = recv_id("chain", crid)
    res = (r or {}).get("result")
    return (res.get("contents") or {}).get("value") or "" if isinstance(res, dict) else ""


# 打到一半（`.in`）：前缀过滤后只剩 init
ctyp = sorted(i["label"] for i in chain_items("std.map.HashMap(i32).in\n"))
check("completion.chain.genericTyping", ctyp == ["init"], str(ctyp))

# 泛型类型路径：只有静态函数（`HashMap(i32).init(…)`）；方法 put/get 与
# 字段 keys/vals 都不该出现在类型上（用户 2026-11-10 命名空间分离）。
cdot = {i["label"]: i for i in chain_items("std.map.HashMap(i32).\n")}
check("completion.chain.genericDot",
      cdot.get("init", {}).get("detail") == "init(alloc: Allocator) HashMap"
      and cdot["init"].get("insertText") == "init(${1:alloc})$0"
      and not ({"put", "get", "keys", "vals", "cap", "n"} & set(cdot)),
      str(sorted(cdot))[:220])
# 字段 2026-11-11 起默认私有（keys/vals/cap/n 都没写 pub），实例补全只出
# pub 方法；`HS.keys` 在编译器侧是 E0404。
cinst = {i["label"] for i in chain_items("HS.\n")}
check("completion.chain.genericInstField",
      {"put", "get", "count", "capacity", "deinit"} <= cinst
      and not ({"keys", "vals", "cap", "n"} & cinst), str(sorted(cinst))[:220])

# 模块函数返回类型（fromUnixMs → DateTime）
cmod = {i["label"] for i in chain_items("std.time.fromUnixMs(0).\n")}
check("completion.chain.moduleFn", {"year", "month", "day"} <= cmod, str(sorted(cmod)))

# 工厂调用返回类型（BufReader.open → ?BufReader）
cfac = {i["label"]: i for i in chain_items('std.BufReader.open(std.heap.page, "z", 64).?.\n')}
check("completion.chain.factory",
      cfac.get("next", {}).get("insertText") == "next()$0"
      and cfac.get("next", {}).get("detail") == "next(self: *BufReader) ?[]const u8",
      str(sorted(cfac))[:220])

# 本地函数返回 struct（非 std 路径，证明是递归解析而不是特判）
cloc = [i["label"] for i in chain_items("mk().\n")]
check("completion.chain.localFn", cloc == ["x"], str(cloc))

check("hover.chain.field",
      "x: i32" in chain_hover("_ = mk().x;", "mk().x")
      and "year: u64" in chain_hover("_ = std.time.fromUnixMs(0).year;", "fromUnixMs(0).year"),
      chain_hover("_ = mk().x;", "mk().x")[:120])
check("hover.chain.method",
      "init(alloc: Allocator)" in chain_hover("var HS = std.map.HashMap(i32).init(std.heap.page);",
                                              "HashMap(i32).init"),
      chain_hover("var HS = std.map.HashMap(i32).init(std.heap.page);", "HashMap(i32).init")[:160])

send({"jsonrpc": "2.0", "method": "textDocument/didClose",
      "params": {"textDocument": {"uri": chainuri}}})


# ---------------------------------------------------------------------------
# 切片/字符串接收者的成员：`"hello".` 与 `[]const u8` 变量的 `.` 出 len/ptr。
# 2026-11-09 用户：字符串字面量能不能 .ptr？有没有长度 len？——能，但补全里
# 一个成员都没有（sema 的 TY_SLICE 只有 .ptr/.len/.0，源文件里没有声明）。
STRM = """const std = @mod("std");

pub const main = fn () i32 {
    var a: *const u8 = "hello".;
    var b: []const u8 = "hello";
    b.;
    var m: []u8 = undefined;
    m.;
    var n = "hello".len;
    var p = "hello".ptr;
    _ = a; _ = n; _ = p;
    return 0;
};
"""
strmuri = "file:///" + os.path.join(SRC_DIR, "strmuse.zx").replace("\\", "/")
open(os.path.join(SRC_DIR, "strmuse.zx"), "w", encoding="utf-8").write(STRM)
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": strmuri, "languageId": "zx", "version": 1, "text": STRM}}})

srid = 980


def strm_items(off):
    global srid
    srid += 1
    send({"jsonrpc": "2.0", "id": srid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": strmuri}, "position": pos(STRM, off)}})
    r, _ = recv_id("strm", srid)
    return r["result"]["items"] if r and r.get("result") else []


def strm_hover(anchor, within):
    global srid
    srid += 1
    a = STRM.index(anchor)
    off = a + STRM[a:].index(within) + len(within)
    send({"jsonrpc": "2.0", "id": srid, "method": "textDocument/hover", "params": {
        "textDocument": {"uri": strmuri}, "position": pos(STRM, off)}})
    r, _ = recv_id("strm", srid)
    res = (r or {}).get("result")
    return (res.get("contents") or {}).get("value") or "" if isinstance(res, dict) else ""


lit_m = {i["label"]: i.get("detail")
         for i in strm_items(STRM.index('"hello".;') + len('"hello".'))}
check("completion.strlit.lenPtr",
      set(lit_m) == {"len", "ptr"} and lit_m.get("len") == "usize"
      and lit_m.get("ptr") == "ptr: *const u8", str(lit_m))

sl_m = {i["label"]: i.get("detail")
        for i in strm_items(STRM.index("    b.;") + len("    b."))}
check("completion.sliceVar.members",
      set(sl_m) == {"len", "ptr"} and sl_m.get("ptr") == "ptr: *const u8", str(sl_m))

mut_m = {i["label"]: i.get("detail")
         for i in strm_items(STRM.index("    m.;") + len("    m."))}
check("completion.sliceVar.mutPtr", mut_m.get("ptr") == "ptr: *u8", str(mut_m))

check("hover.strlit.member",
      "len: usize" in strm_hover('"hello".len;', '"hello".len')
      and "ptr: *const u8" in strm_hover('"hello".ptr;', '"hello".ptr'),
      strm_hover('"hello".len;', '"hello".len')[:120])
# 字符串字面量 = *[N]const u8（用户 2026-11-09 定），不是切片
check("hover.strlit.type",
      "*[5]const u8" in strm_hover('var b: []const u8 = "hello";', '= "hell')
      and "`.ptr" in strm_hover('var b: []const u8 = "hello";', '= "hell'),
      strm_hover('var b: []const u8 = "hello";', '= "hell')[:160])

send({"jsonrpc": "2.0", "method": "textDocument/didClose",
      "params": {"textDocument": {"uri": strmuri}}})

# ---------------------------------------------------------------------------
# 3r 推导链：`.?` 解包 / 下标 / 位置访问（用户 2026-11-09 第 16 条）
#   截图三处：`var b = buf.?` 之后 `b.` 没提示、`let slice = args[0][0]`
#   没有类型提示、`const s = strs.0` 没有类型提示。根因：成员补全走的是
#   inferContainer（只认名字/调用），推导走的是 inferExprType（认表达式）——
#   两条链在"初始化式是表达式"的位置断开了。
ET = """const std = @mod("std");
pub const main = fn () i32 {
    var buf = std.BufReader.init(std.heap.page, 0, 10);
    var b = buf.?;
    b.;
    let args = std.process.args();
    let slice = args[0][0];
    let row = args[0];
    row.;
    let strs = &.{ "hello", "world", };
    const s = strs.0;
    s.;
    strs.;
    const t = { 1, 2.5 };
    const x = t.0;
    _ = slice; _ = x;
    return 0;
};
"""
eturi = "file:///" + os.path.join(SRC_DIR, "exprty_use.zx").replace("\\", "/")
open(os.path.join(SRC_DIR, "exprty_use.zx"), "w", encoding="utf-8").write(ET)
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": eturi, "languageId": "zx", "version": 1, "text": ET}}})

etrid = 990


def et_items(off):
    global etrid
    etrid += 1
    send({"jsonrpc": "2.0", "id": etrid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": eturi}, "position": pos(ET, off)}})
    r, _ = recv_id("exprty", etrid)
    return r["result"]["items"] if r and r.get("result") else []


uw_m = {i["label"]: i for i in et_items(ET.index("    b.;") + len("    b."))}
check("completion.unwrap.members",
      uw_m.get("fill", {}).get("insertText") == "fill()$0"
      and uw_m.get("deinit", {}).get("insertText") == "deinit()$0"
      and uw_m.get("cap", {}).get("detail") == "usize",
      str(sorted(uw_m))[:220])

row_m = {i["label"]: i.get("detail")
         for i in et_items(ET.index("    row.;") + len("    row."))}
check("completion.index.sliceMembers",
      set(row_m) == {"len", "ptr"} and row_m.get("ptr") == "ptr: *const u8", str(row_m))

s_m = {i["label"]: i.get("detail")
       for i in et_items(ET.index("    s.;") + len("    s."))}
check("completion.posIndex.strMembers",
      set(s_m) == {"len", "ptr"} and s_m.get("ptr") == "ptr: *const u8", str(s_m))

arr_m = {i["label"]: i.get("detail")
         for i in et_items(ET.index("    strs.;") + len("    strs."))}
check("completion.posArray.members",
      set(arr_m) == {"len", "ptr"} and arr_m.get("len") == "usize", str(arr_m))

send({"jsonrpc": "2.0", "id": 991, "method": "textDocument/inlayHint", "params": {
    "textDocument": {"uri": eturi},
    "range": {"start": {"line": 0, "character": 0},
              "end": {"line": ET.count("\n") + 1, "character": 0}}}})
r, _ = recv_id("inlayHint.exprty", 991)
eb = r["result"] if r and r.get("result") is not None else []
eby = {(h["position"]["line"], h["position"]["character"]): h["label"] for h in eb}
edetail = str(sorted(eby.items()))[:300]


def elabel(anchor, upto=None):
    p = pos(ET, ET.index(anchor) + len(upto if upto is not None else anchor))
    return eby.get((p["line"], p["character"]))


check("inlayHint.expr.unwrap", elabel("var b = buf", "var b") == ": BufReader", edetail)
check("inlayHint.expr.index", elabel("let slice") == ": const u8"
      and elabel("let row") == ": []const u8", edetail)
check("inlayHint.expr.posIndex", elabel("let strs") == ": *[2]str"
      and elabel("const s = strs", "const s") == ": str", edetail)
check("inlayHint.expr.tuple", elabel("const t") == ": {i32,f64}"
      and elabel("const x") == ": i32", edetail)

send({"jsonrpc": "2.0", "method": "textDocument/didClose",
      "params": {"textDocument": {"uri": eturi}}})


# ---------------------------------------------------------------------------
# 3s 指针 / 链式解引用（用户 2026-11-09 第 17 条）
#   截图两处：`let pp = &p; _ = pp.name` 没有类型、`arrs.0.` 一个成员都不出。
#   编译器侧 `pp.name` / `arrs.0.name` 一直是合法的（`.` 自动穿透指针、`.N`
#   对数组指针取元素），断的是 LSP：`&p` 没有 `*T` 规则、成员补全只认路径
#   末段（`arrs.0` 的末段是 `0`），链式接收者两边都落空。
PT = """const std = @mod("std");
const Person = struct { name: i32, age: i32 };
pub const main = fn () i32 {
    let p: Person = Person{ .name = 0, .age = 1 };
    let pp = &p;
    pp.;
    let nm0 = pp.name;
    let deref = pp.0;
    let dn = pp.0.name;
    let args: [][]const u8 = std.process.args();
    let slice: []const u8 = args[0];
    let char = slice.0;
    let char2 = slice[0];
    let arrs: *[2]Person = &.{ Person{ .name = 0, .age = 1 }, Person{ .name = 0, .age = 1 } };
    let pa = arrs.0;
    pa.;
    arrs.0.;
    arrs[0].;
    let nm5 = arrs[0].name;
    _ = nm0; _ = deref; _ = dn; _ = char; _ = char2; _ = pa; _ = nm5;
    return 0;
};
"""
pturi = "file:///" + os.path.join(SRC_DIR, "ptrchain_use.zx").replace("\\\\", "/")
open(os.path.join(SRC_DIR, "ptrchain_use.zx"), "w", encoding="utf-8").write(PT)
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": pturi, "languageId": "zx", "version": 1, "text": PT}}})

ptrid = 970


def pt_items(anchor_s):
    global ptrid
    ptrid += 1
    send({"jsonrpc": "2.0", "id": ptrid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": pturi},
        "position": pos(PT, PT.index(anchor_s) + len(anchor_s))}})
    r, _ = recv_id("ptrchain", ptrid)
    return r["result"]["items"] if r and r.get("result") else []


def pt_members(anchor_s):
    return {i["label"]: i.get("detail") for i in pt_items(anchor_s)}


check("completion.ptr.members", set(pt_members("    pp.")) == {"name", "age"},
      str(sorted(pt_members("    pp.")))[:200])
check("completion.ptrChain.posMembers", set(pt_members("    arrs.0.")) == {"name", "age"},
      str(sorted(pt_members("    arrs.0.")))[:200])
check("completion.ptrChain.indexMembers", set(pt_members("    arrs[0].")) == {"name", "age"},
      str(sorted(pt_members("    arrs[0].")))[:200])
check("completion.ptrChain.varMembers", set(pt_members("    pa.")) == {"name", "age"},
      str(sorted(pt_members("    pa.")))[:200])

send({"jsonrpc": "2.0", "id": 972, "method": "textDocument/inlayHint", "params": {
    "textDocument": {"uri": pturi},
    "range": {"start": {"line": 0, "character": 0},
              "end": {"line": PT.count("\n") + 1, "character": 0}}}})
r, _ = recv_id("inlayHint.ptrchain", 972)
pb = r["result"] if r and r.get("result") is not None else []
pby = {(h["position"]["line"], h["position"]["character"]): h["label"] for h in pb}
pdetail = str(sorted(pby.items()))[:300]


def plabel(anchor_s, upto=None):
    q = pos(PT, PT.index(anchor_s) + len(upto if upto is not None else anchor_s))
    return pby.get((q["line"], q["character"]))


check("inlayHint.ptr.addr", plabel("let pp") == ": *Person", pdetail)
check("inlayHint.ptr.member", plabel("let nm0") == ": i32", pdetail)
check("inlayHint.ptr.deref", plabel("let deref") == ": Person"
      and plabel("let dn") == ": i32", pdetail)
check("inlayHint.ptr.sliceIndex", plabel("let char") == ": const u8"
      and plabel("let char2") == ": const u8", pdetail)
check("inlayHint.ptr.chainMember", plabel("let nm5") == ": i32", pdetail)

send({"jsonrpc": "2.0", "method": "textDocument/didClose",
      "params": {"textDocument": {"uri": pturi}}})



# ---------------------------------------------------------------------------
# 元组下标 + @type snippet（用户 2026-11-11 第 3 张截图）。
#   · `const s = tup[2];` 类型丢失：inferExprType 的下标分支只认 elemTyOf
#     （切片/数组），元组 `{i32,i32,str}` 取第 N 项要 tupleElemTy。
#   · `@` 内建补全全部带 snippet（2026-11-18）：`@type` 是
#     `type(${1:x})$0`（insertTextFormat=2），@memcpy/@memset 等同样出骨架。
#     守门用例在 completion.atType.snippet / completion.atBuiltins.snippet。
TUPIDX = """const std = @mod("std");

const tup: {i32,i32,str} = {10,23,"hello"};
const s = tup[2];
const a = tup[0];
"""
tupuri = "file:///" + os.path.join(SRC_DIR, "tupidx.zx").replace("\\", "/")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": tupuri, "languageId": "zx", "version": 1, "text": TUPIDX}}})
_rid += 1
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/inlayHint", "params": {
    "textDocument": {"uri": tupuri},
    "range": {"start": {"line": 0, "character": 0},
              "end": {"line": len(TUPIDX.split("\n")), "character": 0}}}})
_r, _ = recv_id("tupidx", _rid)
_tih = {}
if _r and _r.get("result"):
    for _h in _r["result"]:
        _lbx = _h.get("label")
        if isinstance(_lbx, str):
            _tih[_h["position"]["line"]] = _lbx
for _need, _want, _tag in (
        ("const s = tup[2]", ": str", "tupleIdx.str"),
        ("const a = tup[0]", ": i32", "tupleIdx.i32")):
    _ln = TUPIDX[:TUPIDX.index(_need)].count("\n")
    check("inlayHint." + _tag, _tih.get(_ln) == _want, str(_tih))

# ---------------------------------------------------------------------------
# §145 裸元组字面量 + 嵌套内联 struct 的递归补全（用户 2026-11-13 截图 1/2）
# KEEP — 三条都不许删：
#   · `var tup = {1,22,Pv145Person{...}};` 没有前导点，旧链只认 `.{...}`（§146 删），
#     `let person = tup.2;` 没有 inlay 类型、`person.` 一个成员都不出。
#   · `p1.child.` 的 child 是内联 struct 字段，旧链只弹一条
#     `child{...} 构造骨架`，name/school 全被 .ty 命名空间过滤掉。
#   · 元组类型串 `{i32,i32,Pv145Person}` 不是命名成员表，补全不许出空 label。
PV145 = """const std = @mod("std");

const Pv145Person = struct {
    name: []const u8,
    age: i32,
    child: struct { name: []const u8, school: []const u8 },
};

const Pv145Deep = struct {
    lvl: struct { inner: struct { leaf: i32, tag: []const u8 } },
};

const main = fn () i32 {
    var tup = {1, 22, Pv145Person{ .name = "w", .age = 100, .child = .{ .name = "c", .school = "s" } }};
    let person = tup.2;
    person.
    let person2: Pv145Person = tup.2;
    person2.
    var ch2 = person2.child.
    person2.child.
    let arr: *[2]Pv145Person = &.{
        Pv145Person{ .name = "a", .age = 1, .child = .{ .name = "x", .school = "y" } },
        Pv145Person{ .name = "b", .age = 2, .child = .{ .name = "x", .school = "y" } },
    };
    let ii: Pv145Person = arr.1;
    ii.child.
    var p1: Pv145Person = Pv145Person{ .name = "w", .age = 100, .child = .{ .name = "child", .school = "school" } };
    p1.child.
    arr.1.
    tup.
    let d = Pv145Deep{ .lvl = .{ .inner = .{ .leaf = 1, .tag = "t" } } };
    d.lvl.inner.
    return 0;
};
"""
pv145uri = "file:///" + os.path.join(SRC_DIR, "pv145.zx").replace(chr(92), "/")
open(os.path.join(SRC_DIR, "pv145.zx"), "w", encoding="utf-8").write(PV145)
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": pv145uri, "languageId": "zx", "version": 1, "text": PV145}}})

def pv145_items(needle, rid):
    off = PV145.index(needle) + len(needle)
    send({"jsonrpc": "2.0", "id": rid, "method": "textDocument/completion",
          "params": {"textDocument": {"uri": pv145uri}, "position": pos(PV145, off)}})
    r, _ = recv_id("pv145", rid)
    return r["result"]["items"] if r and r.get("result") else []

def pv145_labels(needle):
    global _rid
    _rid += 1
    return {i["label"] for i in pv145_items(needle, _rid)}

_rid += 1
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/inlayHint", "params": {
    "textDocument": {"uri": pv145uri},
    "range": {"start": {"line": 0, "character": 0},
              "end": {"line": len(PV145.split(chr(10))), "character": 0}}}})
_r, _ = recv_id("pv145", _rid)
_pvh = {}
if _r and _r.get("result"):
    for _h in _r["result"]:
        _lab = _h.get("label")
        if isinstance(_lab, str):
            _pvh[_h["position"]["line"]] = _lab
for _need, _want, _tag in (
        ("var tup =", ": {i32,i32,Pv145Person}", "tupleBare"),
        ("let person = tup.2", ": Pv145Person", "tupleIdxPerson")):
    _ln = PV145[:PV145.index(_need)].count(chr(10))
    check("inlayHint." + _tag, _pvh.get(_ln) == _want, str(_pvh))

_pv_person = pv145_labels("    person.")
check("completion.tupleIdx.person", {"name", "age", "child"} <= _pv_person, str(sorted(_pv_person)))
_pv_p1 = pv145_labels("    p1.child.")
check("completion.nested.inlineStruct", {"name", "school"} <= _pv_p1, str(sorted(_pv_p1)))
_pv_ii = pv145_labels("    ii.child.")
check("completion.nested.arrayPtrElem", {"name", "school"} <= _pv_ii, str(sorted(_pv_ii)))
_pv_arr = pv145_labels("    arr.1.")
check("completion.nested.arrayPtrPerson", {"name", "age", "child"} <= _pv_arr, str(sorted(_pv_arr)))
_pv_tup = pv145_labels("    tup.")
# §147：元组接收者自己也要出编译器成员（旧断言写死空列表，改为钉住成员集）。
check("completion.tuple.noEmptyLabel",
      "" not in _pv_tup and {"len", "size", "0", "1", "2"} <= _pv_tup,
      str(sorted(_pv_tup)))
# 三层内联 struct：anonTyOfPath 每一段都重新判匿名串 / 具名容器，深度不设限。
_pv_deep = pv145_labels("    d.lvl.inner.")
check("completion.nested.threeLevel", {"leaf", "tag"} <= _pv_deep, str(sorted(_pv_deep)))

# §149 - tup 取出的值（含显式类型标注）再下钻内联 struct：
# 出 name/school，绝不弹 `child{...}` 构造骨架。值的字段访问不是类型命名空间，
# 给构造实例会把编辑器带进编译不过的写法（用户 2026-11-13 截图）。
_pv_p2 = pv145_labels("    person2.")
check("completion.tupleIdx.annotated", {"name", "age", "child"} <= _pv_p2,
      str(sorted(_pv_p2)))
_pv_p2c = pv145_labels("    var ch2 = person2.child.")
check("completion.tupleIdx.child.assign", {"name", "school"} <= _pv_p2c,
      str(sorted(_pv_p2c)))
check("completion.tupleIdx.child.noCtor",
      not any("{" in l and "..." in l for l in _pv_p2c), str(sorted(_pv_p2c)))
_pv_p2d = pv145_labels("    person2.child.")
check("completion.tupleIdx.child.members", {"name", "school"} <= _pv_p2d,
      str(sorted(_pv_p2d)))

_rid += 1
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/hover", "params": {
    "textDocument": {"uri": pv145uri},
    "position": pos(PV145, PV145.index("    person.") + len("    person.") - 1)}})
_r, _ = recv_id("pv145", _rid)
_pv_hover = ""
if _r and isinstance(_r.get("result"), dict):
    _c = _r["result"].get("contents")
    if isinstance(_c, dict):
        _pv_hover = _c.get("value", "")
    elif isinstance(_c, str):
        _pv_hover = _c
check("hover.tupleIdx.personTy", "Pv145Person" in _pv_hover, _pv_hover[:160])

# ---------------------------------------------------------------------------
# §150 具名嵌套类型：tup 取出的值再下钻（用户 2026-11-13 截图，0 项补全）
# KEEP — 容器体内的 `const Pv150Child = struct{…}` 不是内联匿名 struct：
# 字段 `child: Pv150Child` 让 anonTyOfPath 的叶子落在**具名类型名**上，
# 旧链只调 parseAnonTy，person.child. / p1.child. 全空。名字全局唯一，
# 避免与 §145/§146 的 fixture 串味。
PV150 = """const std = @mod("std");

const Pv150Person = struct {
    name: []const u8,
    age: i32,
    child: Pv150Child,
    const Pv150Child = struct {
        name: []const u8,
        school: []const u8,
    };
};

const main = fn () i32 {
    var tup = {1, 22, Pv150Person{ .name = "w", .age = 100, .child = .{ .name = "c", .school = "s" } }};
    let person = tup.2;
    var child = person.child.
    person.
    person.child.
    var p1 = Pv150Person{ .name = "w", .age = 100, .child = .{ .name = "c", .school = "s" } };
    p1.child.
    _ = child; _ = person; _ = p1; _ = tup;
    return 0;
};
"""
pv150uri = "file:///" + os.path.join(SRC_DIR, "pv150.zx").replace(chr(92), "/")
open(os.path.join(SRC_DIR, "pv150.zx"), "w", encoding="utf-8").write(PV150)
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": pv150uri, "languageId": "zx", "version": 1, "text": PV150}}})

def pv150_labels(needle):
    global _rid
    _rid += 1
    off = PV150.index(needle) + len(needle)
    send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/completion",
          "params": {"textDocument": {"uri": pv150uri}, "position": pos(PV150, off)}})
    r, _ = recv_id("pv150", _rid)
    return {i["label"] for i in (r["result"]["items"] if r and r.get("result") else [])}

_p150_person = pv150_labels("    person.")
check("completion.namedNested.person", {"name", "age", "child"} <= _p150_person,
      str(sorted(_p150_person)))
_p150_assign = pv150_labels("    var child = person.child.")
check("completion.namedNested.child.assign", {"name", "school"} <= _p150_assign,
      str(sorted(_p150_assign)))
check("completion.namedNested.child.noCtor",
      not any("{" in l and "..." in l for l in _p150_assign), str(sorted(_p150_assign)))
_p150_dot = pv150_labels("    person.child.")
check("completion.namedNested.child.members", {"name", "school"} <= _p150_dot,
      str(sorted(_p150_dot)))
_p150_p1 = pv150_labels("    p1.child.")
check("completion.namedNested.child.fromLiteral", {"name", "school"} <= _p150_p1,
      str(sorted(_p150_p1)))

_rid += 1
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/hover", "params": {
    "textDocument": {"uri": pv150uri},
    "position": pos(PV150, PV150.index("person.child") + len("person.child") - 1)}})
_r, _ = recv_id("pv150", _rid)
_p150_hover = ""
if _r and isinstance(_r.get("result"), dict):
    _c = _r["result"].get("contents")
    if isinstance(_c, dict):
        _p150_hover = _c.get("value", "")
    elif isinstance(_c, str):
        _p150_hover = _c
check("hover.namedNested.childTy", "child: Pv150Child" in _p150_hover, _p150_hover[:160])

# ---------------------------------------------------------------------------
# §156 类型构造函数的签名解析（用户 2026-11-13 截图 `const n: void = Gen(...)`）
# KEEP — 外层 `fn(` 与体内方法的 `fn ` 同时出现时，sigOfStmt 必须取最靠前的
# fn；旧版挑中体内方法，把 Gen 的返回类型读成 void、实参名提示写成 `self: `。
GEN154 = """const std = @mod("std");

const Gen = fn(T: type, n: T) type {
    return struct{
        mem: T = n,
        const print = fn (self:Self) void {
            std.println("N:{}", self.mem);
        };
    };
};

const main = fn () u8 {
    const n = Gen(i32,4);
    const v = n{};
    v.print();
    n.
    return 0;
};
"""
g154uri = "file:///" + os.path.join(SRC_DIR, "gen154.zx").replace(chr(92), "/")
open(os.path.join(SRC_DIR, "gen154.zx"), "w", encoding="utf-8").write(GEN154)
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": g154uri, "languageId": "zx", "version": 1, "text": GEN154}}})

_g154_line_call = GEN154[:GEN154.index("const n = Gen")].count("\n")
send({"jsonrpc": "2.0", "id": 5100, "method": "textDocument/inlayHint", "params": {
    "textDocument": {"uri": g154uri},
    "range": {"start": {"line": 0, "character": 0},
              "end": {"line": len(GEN154.split("\n")), "character": 0}}}})
_r, _ = recv_id("gen154.inlay", 5100)
_g154_types = {}
_g154_params = []
for _h in ((_r or {}).get("result") or []):
    _lb = _h.get("label")
    if not isinstance(_lb, str):
        continue
    if _h.get("kind") == 1:
        _g154_types[_h["position"]["line"]] = _lb
    elif _h["position"]["line"] == _g154_line_call:
        _g154_params.append(_lb)
check("inlayHint.typeCtor.retType", _g154_types.get(_g154_line_call) == ": type", str(_g154_types))
check("inlayHint.typeCtor.paramNames",
      "T: " in _g154_params and "n: " in _g154_params and "self: " not in _g154_params,
      str(_g154_params))

send({"jsonrpc": "2.0", "id": 5101, "method": "textDocument/hover", "params": {
    "textDocument": {"uri": g154uri},
    "position": pos(GEN154, GEN154.index("const n = Gen") + len("const n"))}})
_r, _ = recv_id("gen154.hover", 5101)
_g154_hover = ""
if _r and isinstance(_r.get("result"), dict):
    _c = _r["result"].get("contents")
    if isinstance(_c, dict):
        _g154_hover = _c.get("value", "")
    elif isinstance(_c, str):
        _g154_hover = _c
check("hover.typeCtor.notVoid",
      "const n: type" in _g154_hover and "void" not in _g154_hover, _g154_hover[:160])

send({"jsonrpc": "2.0", "id": 5102, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": g154uri},
    "position": pos(GEN154, GEN154.index("    n.\n") + len("    n."))}})
_r, _ = recv_id("gen154.completion", 5102)
_g154_items = (_r or {}).get("result", {}).get("items", [])
_g154_ctor = [i for i in _g154_items if i["label"] == "n{...}"]
check("completion.typeCtor.instance",
      len(_g154_ctor) == 1 and "mem: i32" in (_g154_ctor[0].get("detail") or ""),
      str([(i["label"], i.get("detail")) for i in _g154_items])[:200])


# ---------------------------------------------------------------------------
# §146 / §147 元组收敛为 {} + 元组接收者的编译器成员（用户 2026-11-13）
# KEEP — 四条都不许删：
#   · 位置式 `.{1, 2}` 已删除：LSP 不再把它推成 `{i32,f64}` 类型。
#   · `&.{…}` 数组字面量推断不受影响（去掉前导点后按裸 `{…}` 推）。
#   · `tup.` 出 .len/.size/.0/.1/.2，且不出空 label；显式标注的元组也出。
#   · hover 的 .len/.size/.N 要有内容（编译器字段，源文件无声明文本）。
PV146 = """const std = @mod("std");

const Pv146Pair = struct { a: i32 = 1, b: i32 = 2 };

const main = fn () i32 {
    var tup = {1, 22, "hello"};
    let t2: {i32, f64, []const u8} = {1, 2.5, "x"};
    let one = {7};
    let old = .{1, 2.5};
    let strs = &.{ "a", "b" };
    let p: Pv146Pair = .{ };
    tup.
    t2.
    one.
    tup.1
    _ = tup.len; _ = tup.size; _ = tup.2;
    _ = t2; _ = one; _ = old; _ = strs; _ = p;
    return 0;
};
"""
pv146uri = "file:///" + os.path.join(SRC_DIR, "pv146.zx").replace("\\", "/")
open(os.path.join(SRC_DIR, "pv146.zx"), "w", encoding="utf-8").write(PV146)
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": pv146uri, "languageId": "zx", "version": 1, "text": PV146}}})

def pv146_req(method, tag, off, extra=None):
    global _rid
    _rid += 1
    params = {"textDocument": {"uri": pv146uri}, "position": pos(PV146, off)}
    if extra:
        params.update(extra)
    send({"jsonrpc": "2.0", "id": _rid, "method": method, "params": params})
    r, _ = recv_id("pv146", _rid)
    return r or {}

def pv146_comp(needle, tag):
    r = pv146_req("textDocument/completion", tag, PV146.index(needle) + len(needle))
    items = (r.get("result") or {}).get("items") or []
    return {i["label"]: i.get("detail", "") for i in items}

def pv146_hover(needle):
    r = pv146_req("textDocument/hover", "hover", PV146.index(needle) + len(needle) - 1)
    c = ((r.get("result") or {}).get("contents") or {})
    return c.get("value", "") if isinstance(c, dict) else str(c)

_tup = pv146_comp("    tup.", "completion.tupleMembers")
check("completion.tupleMembers.set",
      {"len", "size", "0", "1", "2"} <= set(_tup) and "" not in _tup,
      str(sorted(_tup)))
check("completion.tupleMembers.detail",
      _tup.get("len") == "usize" and _tup.get("2") == "str", str(_tup))
_t2 = pv146_comp("    t2.", "completion.tupleMembers.annotated")
check("completion.tupleMembers.annotated",
      {"len", "size", "0", "1", "2"} <= set(_t2) and _t2.get("1") == "f64",
      str(_t2))
_one = pv146_comp("    one.", "completion.tupleMembers.single")
check("completion.tupleMembers.single",
      "0" in _one and "1" not in _one, str(sorted(_one)))
_pfx = pv146_comp("    tup.1", "completion.tupleMembers.prefix")
check("completion.tupleMembers.prefix", set(_pfx) == {"1"}, str(sorted(_pfx)))
check("hover.tupleMembers.len", "usize" in pv146_hover("tup.len"), "")
check("hover.tupleMembers.size", "usize" in pv146_hover("tup.size"), "")
check("hover.tupleMembers.index", "str" in pv146_hover("tup.2"), "")

_rid += 1
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/inlayHint", "params": {
    "textDocument": {"uri": pv146uri},
    "range": {"start": {"line": 0, "character": 0},
              "end": {"line": len(PV146.split(chr(10))), "character": 0}}}})
_r, _ = recv_id("pv146", _rid)
_p146h = {}
if _r and _r.get("result"):
    for _h in _r["result"]:
        _lab = _h.get("label")
        if isinstance(_lab, str):
            _p146h[_h["position"]["line"]] = _lab
_ln_old = PV146[:PV146.index("let old =")].count(chr(10))
_ln_arr = PV146[:PV146.index("let strs =")].count(chr(10))
check("inlayHint.noDotTupleType", _p146h.get(_ln_old) is None, str(_p146h))
check("inlayHint.arrayLiteralStillWorks", _p146h.get(_ln_arr) == ": *[2]str", str(_p146h))

atdoc = "const std = @mod(\"std\");\n@\n"
aturi = "file:///" + os.path.join(SRC_DIR, "atbuild.zx").replace("\\", "/")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": aturi, "languageId": "zx", "version": 1, "text": atdoc}}})
_rid += 1
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": aturi}, "position": pos(atdoc, atdoc.index("@\n") + 1)}})
_r, _ = recv_id("atbuild", _rid)
_ats = {i["label"]: i for i in ((_r or {}).get("result") or {}).get("items") or []}
check("completion.atType.snippet",
      _ats.get("type", {}).get("insertTextFormat") == 2
      and _ats.get("type", {}).get("insertText") == "type(${1:x})$0",
      str(_ats.get("type"))[:200])
# 2026-11-18 用户：全部内建都带 snippet（@memset 等）。骨架在 builtin_docs.snip，
# 插入文本不带 '@'（宿主只替换 '@' 之后的标识符段）。
check("completion.atBuiltins.snippet",
      _ats.get("memcpy", {}).get("insertTextFormat") == 2
      and _ats.get("memcpy", {}).get("insertText") == "memcpy(${1:dest}, ${2:src}, ${3:count})$0"
      and _ats.get("memset", {}).get("insertText") == "memset(${1:dest}, ${2:fill}, ${3:count})$0"
      and _ats.get("memmove", {}).get("insertText") == "memmove(${1:dest}, ${2:src}, ${3:count})$0"
      and _ats.get("trap", {}).get("insertText") == "trap()$0",
      str(_ats.get("memcpy"))[:200])

# ---------------------------------------------------------------------------
# 保存自动格式化（textDocument/formatting）。用户 2026-11-11："保存没有
# 格式化的功能了"。根因：formatDoc 的影子文件被 hideFile 标了 HIDDEN，
# Windows 对已有 HIDDEN 属性的文件 fopen(...,"wb") 重写直接 ACCESS_DENIED，
# zxc fmt 静默写失败 → 读回原样 → 空编辑。守门：格式化必须真的给出缩进
# 修正的 TextEdit（不是 []），且文档没有被格式化污染（影子用完即删）。
FMTDOC = 'const std = @mod("std");\n\nconst main = fn () i32 {\n        return 0;\n      };\n'
fmturi = "file:///" + os.path.join(SRC_DIR, "fmttest.zx").replace("\\", "/")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": fmturi, "languageId": "zx", "version": 1, "text": FMTDOC}}})
_rid += 1
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/formatting", "params": {
    "textDocument": {"uri": fmturi},
    "options": {"tabSize": 4, "insertSpaces": True}}})
_r, _ = recv_id("formatting", _rid)
_fres = (_r or {}).get("result") or []
_ftext = _fres[0].get("newText", "") if isinstance(_fres, list) and _fres else ""
check("formatting.indentFix",
      "    return 0;\n" in _ftext and "        return 0" not in _ftext
      and _ftext.endswith("};\n"),
      str(_fres)[:200])
# 尾逗号拆行：参数/实参表以 ',' 结尾就一行一个（用户 2026-11-11）。
# LSP formatting 与 CLI fmt 走同一条 zxc fmt 链，这里必须同样拆行。
TCDOC = 'const add = fn (a: i32, b: i32,) i32 {\n    return a + b;\n};\n'
tcuri = "file:///" + os.path.join(SRC_DIR, "tctest.zx").replace("\\", "/")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": tcuri, "languageId": "zx", "version": 1, "text": TCDOC}}})
_rid += 1
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/formatting", "params": {
    "textDocument": {"uri": tcuri},
    "options": {"tabSize": 4, "insertSpaces": True}}})
_r, _ = recv_id("tcformat", _rid)
_tcres = (_r or {}).get("result") or []
_tctext = _tcres[0].get("newText", "") if isinstance(_tcres, list) and _tcres else ""
check("formatting.trailingComma",
      "fn (\n    a: i32,\n    b: i32,\n) i32 {" in _tctext,
      str(_tcres)[:300])
# 字面量组递归拆行（18:xx 扩展）：.{…} 尾逗号也拆，嵌套项一层一层展开，
# 内层不带尾逗号的组（.{.b = 120}）保持单行。
TLCDOC = 'const main = fn () i32 {\n    _ = pa(10, .{ .name = "weo", .data = .{ .b = 1, .i = 0, .data = .{.b = 120}, }, },);\n    return 0;\n};\n'
tluri = "file:///" + os.path.join(SRC_DIR, "tltest.zx").replace("\\", "/")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": tluri, "languageId": "zx", "version": 1, "text": TLCDOC}}})
_rid += 1
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/formatting", "params": {
    "textDocument": {"uri": tluri},
    "options": {"tabSize": 4, "insertSpaces": True}}})
_r, _ = recv_id("tlformat", _rid)
_tlres = (_r or {}).get("result") or []
_tltext = _tlres[0].get("newText", "") if isinstance(_tlres, list) and _tlres else ""
check("formatting.literalGroupRec",
      "_ = pa(\n" in _tltext
      and "    .{\n" in _tltext
      and '        .name = "weo",\n' in _tltext
      and "        .data = .{\n" in _tltext
      and "            .b = 1,\n" in _tltext
      and "            .data = .{.b = 120},\n" in _tltext
      and "    },\n" in _tltext
      and "    );\n" in _tltext,
      str(_tlres)[:400])
# 手工多行区域归一（zig-fmt 口径）：闭括号前有尾逗号的组一行一项树状展开；
# 根组没尾逗号 → 保持单行（内层带尾逗号的 .{…} 照常展开），**不补逗号**
# （旧口径"缺的尾逗号由 fmt 补齐"已废）。拆行不许丢内容（曾复现丢实参 bug）。
MLTDOC = ('const main = fn () i32 {\n'
          '    _ = pa(10, .{\n'
          '        .name = "weo",\n'
          '        .data = .{\n'
          '            .b = 1, .i = 0, .data = .{.b = 120},\n'
          '        }, } );\n'
          '    return 0;\n'
          '};\n')
mluri = "file:///" + os.path.join(SRC_DIR, "mltest.zx").replace("\\", "/")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": mluri, "languageId": "zx", "version": 1, "text": MLTDOC}}})
_rid += 1
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/formatting", "params": {
    "textDocument": {"uri": mluri},
    "options": {"tabSize": 4, "insertSpaces": True}}})
_r, _ = recv_id("mlformat", _rid)
_mlres = (_r or {}).get("result") or []
_mltext = _mlres[0].get("newText", "") if isinstance(_mlres, list) and _mlres else ""
check("formatting.multilineTree",
      "    _ = pa(10, .{\n" in _mltext
      and '        .name = "weo",\n' in _mltext
      and "        .data = .{\n" in _mltext
      and "            .b = 1,\n" in _mltext
      and "            .data = .{.b = 120},\n" in _mltext
      and "        },\n" in _mltext
      and "    });\n" in _mltext
      and "        10,\n" not in _mltext
      and _mltext.count("weo") == 1 and _mltext.count("120") == 1,
      str(_mlres)[:400])
# 无尾逗号 → 整块收一行（用户截图场景：删掉尾逗号要回到一行；不补逗号）。
CPLDOC = ('const main = fn () i32 {\n'
          '    _ = pb(1,\n'
          '        2,\n'
          '        3\n'
          '    );\n'
          '    return 0;\n'
          '};\n')
cpuri = "file:///" + os.path.join(SRC_DIR, "cptest.zx").replace("\\", "/")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": cpuri, "languageId": "zx", "version": 1, "text": CPLDOC}}})
_rid += 1
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/formatting", "params": {
    "textDocument": {"uri": cpuri},
    "options": {"tabSize": 4, "insertSpaces": True}}})
_r, _ = recv_id("cpformat", _rid)
_cpres = (_r or {}).get("result") or []
_cptext = _cpres[0].get("newText", "") if isinstance(_cpres, list) and _cpres else ""
check("formatting.collapseNoTc",
      "    _ = pb(1, 2, 3);\n" in _cptext,
      str(_cpres)[:300])
# struct/enum/union 类型体没尾逗号：容器体不收拢、不拆（保持原样）。
TYDOC = 'const P = struct{\n        a: i32,\n    b: i32\n};\n'
tyuri = "file:///" + os.path.join(SRC_DIR, "tytest.zx").replace("\\", "/")
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": tyuri, "languageId": "zx", "version": 1, "text": TYDOC}}})
_rid += 1
send({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/formatting", "params": {
    "textDocument": {"uri": tyuri},
    "options": {"tabSize": 4, "insertSpaces": True}}})
_r, _ = recv_id("tyformat", _rid)
_tyres = (_r or {}).get("result") or []
_tytext = _tyres[0].get("newText", "") if isinstance(_tyres, list) and _tyres else ""
check("formatting.tybodyKeep",
      "const P = struct{\n    a: i32,\n    b: i32\n};" in _tytext,
      str(_tyres)[:300])
# 影子（.zxls- / .zxls-f 前缀）用完即删。断言前先清掉历史跑留下的旧影子
#（delete 与 zxc 子进程持句柄竞速失败会留下永久垃圾），再只看这次新增的。
import glob as _glob, time as _time
for _old in _glob.glob(os.path.join(SRC_DIR, ".zxls-*")):
    try:
        os.remove(_old)
    except OSError:
        pass
_left = None
for _ in range(10):
    _left = _glob.glob(os.path.join(SRC_DIR, ".zxls-*"))
    if not _left:
        break
    _time.sleep(0.5)
check("formatting.shadowCleaned", not _left, str(_left))

# 8z 2026-11-12 用户截图回归（KEEP：能力线被回退时这里会连挂，不许删用例）
#   ① 带类型注解的关联常量 `pub const K: T = v` 曾被成员解析器判成字段：
#      实例上冒出"很多其他的东西"、类型空间反而缺常量。任意容器都会中招。
#   ② `const Hs: type = 泛型实例;` 之后 `Hs.init(...)` 没有行内 : Type。
#   ③ vendor 定义的类型，字段再套 struct 就没补全/类型提示 —— 要递归查找：
#      内联 struct、同模块命名容器、多层匿名嵌套三种形状都守。
NESTLIB = """pub const Person = struct {
    pub name: []const u8,
    pub id: i32,
    pub data: struct { pub a: i32, pub b: i32 },
};

pub const Sub = struct {
    pub x: i32,
    pub y: f64,
};

pub const Wrap = struct {
    pub data: Sub,
    pub deep: struct { pub inner: struct { pub p: i32, pub q: i32 } },
};

pub const Limits = struct {
    pub const MAX: usize = 9;
    pub val: i32,
};
"""
open(os.path.join(TESTDIR, "vendor", "zxls8zlib.zx"), "w", encoding="utf-8").write(NESTLIB)

# 外部模块分开两个文件：geolib.zx 里的字段类型引用 nestlib.zx 的 Sub，
# 守的是「外部导入的 module 都要查一遍，它们和 src 不是同一条解析路径」。
open(os.path.join(TESTDIR, "vendor", "zxls8zcross.zx"), "w", encoding="utf-8").write(
    'const nl = @mod("zxls8zlib");\n'
    'pub const Box = fn (T: type) type {\n'
    '    return struct {\n'
    '        pub v: T,\n'
    '        pub meta: struct { pub tag: i32, pub note: i32 },\n'
    '    };\n'
    '};\n'
    '\n'
    'pub const Holder = struct {\n'
    '    pub sub: nl.Sub,\n'
    '    pub meta: struct { pub a: i32, pub b: i32 },\n'
    '};\n')

NESTUSE = """const std = @mod("std");
const t = @mod("zxls8zlib");
const g = @mod("zxls8zcross");
const Wrap = struct {
    pub const MAX: usize = 9;
    pub val: i32,
};
const Task = std.task.Task;

pub const main = fn () i32 {
    var c: Task = std.task.Task.coro();
    c.
    std.task.Task.
    var w = Wrap{ .val = 1 };
    w.
    Wrap.
    var lim = t.Limits{ .val = 1 };
    lim.
    t.Limits.
    const Hs: type = std.map.HashMap(V: i32);
    var map = Hs.init(std.heap.page);
    var p: t.Person = t.Person{ .name = "oo", .id = 0, .data = .{ .a = 1, . } };
    var ww: t.Wrap = t.Wrap{ .data = .{ .x = 1, . }, .deep = .{ .inner = .{ .p = 2, . } } };
    const B = g.Box(i32);
    var b = B{ .v = 1, .meta = .{ .tag = 2, . } };
    b.
    var h = g.Holder{ .sub = .{ .x = 1, . }, .meta = .{ .a = 1, . } };
    h.
    if
    return 0;
};
"""
nsturi = "file:///" + os.path.join(SRC_DIR, "nestuse.zx").replace("\\", "/")
open(os.path.join(SRC_DIR, "nestuse.zx"), "w", encoding="utf-8").write(NESTUSE)

# 8z 用独立短命会话：主会话此时已开过几百个文档，zxls 的扫描缓存会污染
# 嵌套字面量 / 泛型合成容器（干净会话里同样的 fixture 14/14 全通）。
p8 = subprocess.Popen([ZXLS_BIN], stdin=subprocess.PIPE,
                      stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
q8 = queue.Queue()

def r8reader():
    while True:
        hdr = b""
        while not hdr.endswith(b"\r\n\r\n"):
            c = p8.stdout.read(1)
            if not c:
                q8.put(None); return
            hdr += c
        ln = int([l for l in hdr.split(b"\r\n")
                  if l.lower().startswith(b"content-length")][0].split(b":")[1])
        body = b""
        while len(body) < ln:
            body += p8.stdout.read(ln - len(body))
        q8.put(json.loads(body))
threading.Thread(target=r8reader, daemon=True).start()

def s8(m):
    data = json.dumps(m).encode()
    p8.stdin.write(b"Content-Length: %d\r\n\r\n" % len(data) + data)
    p8.stdin.flush()

def r8(tag, want_id, timeout=30):
    import time as _t8
    end = _t8.time() + timeout
    while _t8.time() < end:
        try: m = q8.get(timeout=1)
        except queue.Empty: continue
        if m is None:
            print(f"FAIL {tag}: 8z server died"); return None, []
        if "id" not in m: continue
        if m["id"] == want_id: return m, []
    print(f"FAIL {tag}: 8z timeout"); return None, []

s8({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
    "capabilities": {}, "rootUri": "file:///" + TESTDIR.replace("\\", "/")}})
while True:
    _m8 = q8.get(timeout=30)
    if _m8 is None or _m8.get("id") == 1:
        break
s8({"jsonrpc": "2.0", "method": "initialized", "params": {}})

s8({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": nsturi, "languageId": "zx", "version": 1, "text": NESTUSE}}})

_rid = 4000

def comp_at(needle, tag):
    global _rid
    _rid += 1
    off = NESTUSE.index(needle) + len(needle)
    s8({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": nsturi}, "position": pos(NESTUSE, off)}})
    r, _ = r8(tag, _rid)
    items = (r["result"]["items"] if r and r.get("result") and isinstance(r["result"], dict)
             else (r["result"] if r and isinstance(r.get("result"), list) else []))
    return {i["label"]: i for i in items}

# ① 实例 / 类型两空间彻底分离（Task 与自定义容器各一组，证明是通用机制）
_c = comp_at("    c.", "nest.taskInst")
check("completion.nest.taskInst.clean",
      "await" in _c and "spawn" in _c and not {"SEL_AWAIT", "SEL_RECV"} & set(_c),
      str(sorted(_c)))
_t = comp_at("    std.task.Task.", "nest.taskTy")
check("completion.nest.taskTy.assocConst",
      {"SEL_AWAIT", "SEL_RECV"} <= set(_t) and "await" not in _t,
      str(sorted(_t)))
# §129：内核线程组是 Task.group() 工厂（唯一的静态入口），group* 一律
# 不进实例命名空间；类型空间也不许混入实例方法。
check("completion.nest.taskTy.noBehavior",
      not ({"groupSpawn", "groupWait", "groupClose", "groupRun",
            "spawn", "await", "select", "scope"} & set(_t)),
      str(sorted(_t)))
check("completion.nest.taskTy.groupFactory",
      "group" in _t, str(sorted(_t)))
check("completion.nest.taskInst.noGroup",
      not ({"group", "groupSpawn", "groupWait", "groupClose",
            "groupRun"} & set(_c)),
      str(sorted(_c)))
_w = comp_at("    w.", "nest.localInst")
check("completion.nest.localInst.clean",
      set(_w) == {"val"}, str(sorted(_w)))
_W = comp_at("    Wrap.", "nest.localTy")
check("completion.nest.localTy.assocConst",
      set(_W) == {"MAX"}, str(sorted(_W)))
_l = comp_at("    lim.", "nest.vendorInst")
check("completion.nest.vendorInst.clean",
      set(_l) == {"val"}, str(sorted(_l)))
_L = comp_at("    t.Limits.", "nest.vendorTy")
check("completion.nest.vendorTy.assocConst",
      set(_L) == {"MAX"}, str(sorted(_L)))

# ② 类型值别名上的调用也要有行内 : Type
_rid += 1
s8({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/inlayHint", "params": {
    "textDocument": {"uri": nsturi},
    "range": {"start": {"line": 0, "character": 0},
              "end": {"line": NESTUSE.count("\n") + 1, "character": 0}}}})
_r, _ = r8("nest.inlay", _rid)
_hs = ((_r or {}).get("result") or [])
_mapline = NESTUSE[:NESTUSE.index("var map =")].count("\n")
_maplab = [h["label"] for h in _hs if h.get("position", {}).get("line") == _mapline
           and h.get("kind") == 1]
check("inlayHint.nest.typeValuedAlias", ": HashMap" in _maplab, str(_maplab))

# ③ vendor 类型 + 嵌套字面量：内联 struct / 同模块命名类型 / 三层嵌套
_n1 = comp_at(".data = .{ .a = 1, .", "nest.inlineStruct")
check("completion.nest.inlineStruct",
      "b" in _n1 and _n1["b"].get("detail") == "i32" and "a" not in _n1,
      str(sorted(_n1)))
_n2 = comp_at(".data = .{ .x = 1, .", "nest.namedType")
check("completion.nest.namedType",
      "y" in _n2 and _n2["y"].get("detail") == "f64" and "x" not in _n2,
      str(sorted(_n2)))
_n3 = comp_at(".inner = .{ .p = 2, .", "nest.threeLevel")
check("completion.nest.threeLevel",
      "q" in _n3 and _n3["q"].get("detail") == "i32" and "p" not in _n3,
      str(sorted(_n3)))

# ④ snippet 能力线没被连坐（if 骨架仍带 insertTextFormat=2）
_s = comp_at("    if", "nest.snippet")
check("completion.nest.snippet",
      _s.get("if", {}).get("insertTextFormat") == 2, str(sorted(_s)))

# ⑤ 泛型类：类型值绑定 `const B = g.Box(i32)` 之后，实例补全、字面量
#    嵌套字段、行内 : B 三条链都要通；形参 T 要换成实参 i32。
_gb = comp_at("    b.", "nest.genericInst")
check("completion.nest.genericInst",
      set(_gb) == {"v", "meta"} and _gb["v"].get("detail") == "i32",
      str({k: v.get("detail") for k, v in _gb.items()}))
_gl = comp_at(".meta = .{ .tag = 2, .", "nest.genericLiteral")
check("completion.nest.genericLiteral",
      "note" in _gl and "tag" not in _gl, str(sorted(_gl)))
_rid += 1
s8({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/inlayHint", "params": {
    "textDocument": {"uri": nsturi},
    "range": {"start": {"line": 0, "character": 0},
              "end": {"line": NESTUSE.count("\n") + 1, "character": 0}}}})
_r, _ = r8("nest.genericInlay", _rid)
_bl = NESTUSE[:NESTUSE.index("var b = B{")].count("\n")
_blab = [h["label"] for h in ((_r or {}).get("result") or [])
         if h.get("position", {}).get("line") == _bl and h.get("kind") == 1]
check("inlayHint.nest.genericAlias", ": B" in _blab, str(_blab))

# ⑥ 跨外部模块：geolib 里的 `nl.Sub` 定义在另一个 vendor 文件，
#    容器查找必须把它们和 src 放进同一份扫描集。
_cm = comp_at(".sub = .{ .x = 1, .", "nest.crossModule")
check("completion.nest.crossModule",
      "y" in _cm and _cm["y"].get("detail") == "f64", str(sorted(_cm)))
_ch = comp_at("    h.", "nest.externalModuleInst")
check("completion.nest.externalModuleInst",
      set(_ch) == {"sub", "meta"}, str(sorted(_ch)))

# ⑦ 函数形参 / 函数体局部匿名 struct 递归解析（用户 15:10："记得函数里的
#    匿名结构体也要解析,递归解析"）。三层嵌套点补全、嵌套字面量、inlay
#    完整类型串三条链都守；删掉 anonTyOfPath / anonBindingNestedFields
#    任一条就会连挂。
ANONSRC = """const deep = fn (info: struct { a: i32, opts: struct { w: i32, sub: struct { p: i32, q: i32 } } }) i32 {
    var x = info.opts.w;
    info.
    info.opts.
    info.opts.sub.
    return 0;
};
pub const main = fn () i32 {
    const cfg = .{ .w = 1, .opts = .{ .h = 2, .deep = .{ .z = 3, .y = 4 } } };
    cfg.
    cfg.opts.
    cfg.opts.deep.
    var c2 = .{ .w = 1, .opts = .{ .h = 2, . } };
    deep(.{ .a = 1, .opts = .{ .w = 2, .sub = .{ .p = 3, . } } });
    return 0;
};
"""
anuri = "file:///" + os.path.join(SRC_DIR, "anonuse.zx").replace("\\", "/")
open(os.path.join(SRC_DIR, "anonuse.zx"), "w", encoding="utf-8").write(ANONSRC)
s8({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": anuri, "languageId": "zx", "version": 1, "text": ANONSRC}}})

def acomp(needle, tag):
    global _rid
    _rid += 1
    off = ANONSRC.index(needle) + len(needle)
    s8({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": anuri}, "position": pos(ANONSRC, off)}})
    r, _ = r8(tag, _rid)
    items = (r["result"]["items"] if r and r.get("result") and isinstance(r["result"], dict)
             else (r["result"] if r and isinstance(r.get("result"), list) else []))
    return {i["label"]: i for i in items}

_ap1 = acomp("    info.", "anon.paramTop")
check("completion.nest.anonParam.top",
      set(_ap1) == {"a", "opts"}
      and "sub: struct" in (_ap1["opts"].get("detail") or ""),
      str({k: v.get("detail") for k, v in _ap1.items()}))
_ap2 = acomp("    info.opts.", "anon.paramMid")
check("completion.nest.anonParam.mid",
      set(_ap2) == {"w", "sub"}
      and (_ap2["sub"].get("detail") or "") == "struct { p: i32, q: i32 }",
      str({k: v.get("detail") for k, v in _ap2.items()}))
_ap3 = acomp("    info.opts.sub.", "anon.paramDeep")
check("completion.nest.anonParam.deep",
      set(_ap3) == {"p", "q"} and _ap3["q"].get("detail") == "i32",
      str(sorted(_ap3)))
_ap4 = acomp(".sub = .{ .p = 3, .", "anon.paramLiteral")
check("completion.nest.anonParam.literal",
      "q" in _ap4 and _ap4["q"].get("detail") == "i32", str(sorted(_ap4)))

_rid += 1
s8({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/inlayHint", "params": {
    "textDocument": {"uri": anuri},
    "range": {"start": {"line": 0, "character": 0},
              "end": {"line": ANONSRC.count("\n") + 1, "character": 0}}}})
_r, _ = r8("anon.inlay", _rid)
_ail = [h.get("label") for h in ((_r or {}).get("result") or [])]
check("inlayHint.nest.anonParam",
      any("struct { a: i32, opts:" in (l or "") for l in _ail)
      and any("struct { p: i32, q: i32 }" in (l or "") for l in _ail),
      str(_ail[:8]))

_al1 = acomp("    cfg.", "anon.localTop")
check("completion.nest.anonLocal.top",
      set(_al1) == {"w", "opts"}
      and "deep:struct" in (_al1["opts"].get("detail") or ""),
      str({k: v.get("detail") for k, v in _al1.items()}))
_al2 = acomp("    cfg.opts.", "anon.localMid")
check("completion.nest.anonLocal.mid",
      set(_al2) == {"h", "deep"}, str(sorted(_al2)))
_al3 = acomp("    cfg.opts.deep.", "anon.localDeep")
check("completion.nest.anonLocal.deep",
      set(_al3) == {"z", "y"} and _al3["y"].get("detail") == "i32",
      str(sorted(_al3)))
_al4 = acomp(".opts = .{ .h = 2, .", "anon.localLiteral")
check("completion.nest.anonLocal.literal",
      "deep" in _al4
      and "struct{z:i32,y:i32}" in (_al4["deep"].get("detail") or ""),
      str({k: v.get("detail") for k, v in _al4.items()}))

# ⑧ union / union(enum) 静态成员、关联常量、方法函数（用户 15:21 补充）。
#    `.instance` / `.ty` 两空间分离与 struct 同规则；嵌套字面量也要递归。
UNIONSRC = """const U = union {
    pub a: i32,
    pub opts: struct { pub w: i32, pub h: i32 },
    pub const MAX: usize = 7;
    pub const name = fn (self: Self) i32 { return 1; };
    pub const make = fn () U { return U{ .a = 1, .opts = .{ .w = 1, .h = 2 } }; };
};
const UE = union(enum) {
    circle: f64,
    rect: struct { pub w: i32, pub h: i32 },
    pub const ZERO: usize = 0;
    pub const describe = fn (self: Self) []const u8 { return "x"; };
    pub const mk = fn () UE { return UE{ .circle = 1.0 }; };
};
pub const main = fn () i32 {
    var u = U{ .a = 1, .opts = .{ .w = 2, . } };
    u.
    U.
    var e = UE{ .rect = .{ .w = 1, . } };
    e.
    UE.
    return 0;
};
"""
unuri = "file:///" + os.path.join(SRC_DIR, "unionuse.zx").replace("\\", "/")
open(os.path.join(SRC_DIR, "unionuse.zx"), "w", encoding="utf-8").write(UNIONSRC)
s8({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": unuri, "languageId": "zx", "version": 1, "text": UNIONSRC}}})

def ucomp(needle, tag):
    global _rid
    _rid += 1
    off = UNIONSRC.index(needle) + len(needle)
    s8({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": unuri}, "position": pos(UNIONSRC, off)}})
    r, _ = r8(tag, _rid)
    items = (r["result"]["items"] if r and r.get("result") and isinstance(r["result"], dict)
             else (r["result"] if r and isinstance(r.get("result"), list) else []))
    return {i["label"]: i for i in items}

_u1 = ucomp("    u.", "union.inst")
check("completion.nest.unionInst",
      set(_u1) == {"a", "opts", "name"}
      and "self" in (_u1["name"].get("detail") or "")
      and _u1["name"].get("insertTextFormat") == 2,
      str({k: v.get("detail") for k, v in _u1.items()}))
_u2 = ucomp("    U.", "union.ty")
check("completion.nest.unionTy",
      set(_u2) == {"MAX", "make"}
      and "usize" in (_u2["MAX"].get("detail") or "")
      and _u2["make"].get("insertTextFormat") == 2,
      str({k: v.get("detail") for k, v in _u2.items()}))
_u3 = ucomp("    e.", "unionenum.inst")
check("completion.nest.unionEnumInst",
      set(_u3) == {"circle", "rect", "describe"}
      and "self" in (_u3["describe"].get("detail") or ""), str(sorted(_u3)))
_u4 = ucomp("    UE.", "unionenum.ty")
check("completion.nest.unionEnumTy",
      set(_u4) == {"circle", "rect", "ZERO", "mk"}, str(sorted(_u4)))
_u5 = ucomp(".opts = .{ .w = 2, .", "union.literal")
check("completion.nest.unionLiteral",
      "h" in _u5 and _u5["h"].get("detail") == "i32", str(sorted(_u5)))
_u6 = ucomp(".rect = .{ .w = 1, .", "unionenum.literal")
check("completion.nest.unionEnumLiteral",
      "h" in _u6 and _u6["h"].get("detail") == "i32", str(sorted(_u6)))

# ⑨ enum 实例静态值回归在 2745 行的 enum.instMembers 断言里守：
#    上表的 `c.` 集合去掉了 FIRST/COUNT，与 ⑧ 同一条命名空间红线。


# ⑩ 两条解析链系统性守门（用户 2026-11-12："struct/enum/union/union(enum)
#    都可以嵌套类，函数、方法的参数也可以是匿名的类，也是嵌套类……任何
#    地方都要有智能提示、补全、snippet"）。
#    链路 A：四种容器 + 泛型函数返回体里的嵌套类型、类型值别名、构造骨架。
#    链路 B：函数/方法形参的匿名嵌套 struct、形参体内 self、显式匿名类型
#    标注的局部变量、编辑中的嵌套字面量。
KLIB = """pub const SG = struct {
    pub const Inner = struct { pub a: i32, pub b: i32 };
    pub const Deeper = struct { pub const Leaf = struct { pub q: i32 }; pub p: i32 };
    pub id: i32,
};
pub const SE = enum { red, green, pub const Inner = struct { pub z: i32 }; };
pub const SU = union { pub a: i32, pub const Inner = struct { pub z: i32 }; };
pub const SUE = union(enum) { circle: f64, pub const Inner = struct { pub z: i32 }; };
pub const GG = fn (T: type) type {
    return struct {
        pub val: T,
        pub const F = struct { pub name: i32, pub flag: bool };
    };
};
pub const BX = struct {
    pub id: i32,
    pub const take = fn (self: Self, cfg: struct { a: i32, nest: struct { b: i32, c: i32 } }) i32 {
        cfg.
        cfg.nest.
        self.
        return 0;
    };
};
pub const FF2 = fn (b: i32, data: struct { b: i32, data: struct { b: i32, i: i32 } }) i32 {
    data.
    data.data.
    return 0;
};
"""
_klib_path = os.path.join(TESTDIR, "vendor", "zxls8zk.zx")
open(_klib_path, "w", encoding="utf-8").write(KLIB)
kliburi = "file:///" + _klib_path.replace("\\", "/")

KUSE = """const std = @mod("std");
const k = @mod("zxls8zk");
pub const main = fn () i32 {
    k.SG.Inner.
    k.SG.Deeper.Leaf.
    k.SE.Inner.
    k.SU.Inner.
    k.SUE.Inner.
    k.GG.F.
    k.GG(i32).F.
    const KT: type = k.GG.F;
    KT.
    var kx: KT = KT{ .name = 1, .flag = 0 };
    kx.
    KT{ .name = 1, . }
    var ka: struct { a: i32, nest: struct { b: i32, c: i32 } } = .{ .a = 1, .nest = .{ .b = 3, .c = 4 } };
    ka.
    ka.nest.
    var kb: struct { a: i32, nest: struct { b: i32, c: i32 } } = .{ .a = 1, .nest = .{ .b = 2, . } };
    if
    return 0;
};
"""
_kuse_path = os.path.join(SRC_DIR, "kchain.zx")
open(_kuse_path, "w", encoding="utf-8").write(KUSE)
kuri = "file:///" + _kuse_path.replace("\\", "/")

for _u, _txt in ((kuri, KUSE), (kliburi, KLIB)):
    s8({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
        "textDocument": {"uri": _u, "languageId": "zx", "version": 1, "text": _txt}}})


def kcomp(src, uri_, needle, tag):
    global _rid
    _rid += 1
    idx = src.find(needle)
    if idx < 0:
        print(f"FAIL {tag}: needle {needle!r} not found"); return {}
    off = idx + len(needle)
    s8({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": uri_}, "position": pos(src, off)}})
    r, _ = r8(tag, _rid)
    items = (r["result"]["items"] if r and r.get("result") and isinstance(r["result"], dict)
             else (r["result"] if r and isinstance(r.get("result"), list) else []))
    return {i["label"]: i for i in items}


def khover(src, uri_, needle, word, tag):
    global _rid
    _rid += 1
    idx = src.find(needle)
    w = needle.find(word)
    off = idx + w + len(word)
    s8({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/hover", "params": {
        "textDocument": {"uri": uri_}, "position": pos(src, off)}})
    r, _ = r8(tag, _rid)
    v = ((r or {}).get("result") or {}).get("contents", {})
    return v.get("value", "") if isinstance(v, dict) else str(v)


def ctor_ok(mp, label, shape):
    it = mp.get(label, {})
    det = it.get("detail") or ""
    te = it.get("textEdit") or {}
    nt = te.get("newText") or ""
    return (shape in det and nt.startswith("{ .")
            and te.get("range", {}).get("start") is not None
            and it.get("insertTextFormat") == 2), str(mp)[:220]


# 链路 A1：四种容器 + 三层嵌套 + 泛型返回体 + 泛型实例尾段的类型空间
for _needle, _label, _shape, _tag in (
    ("    k.SG.Inner.", "Inner{...}", "a: i32", "kchain.structInner"),
    ("    k.SG.Deeper.Leaf.", "Leaf{...}", "q: i32", "kchain.threeLevel"),
    ("    k.SE.Inner.", "Inner{...}", "z: i32", "kchain.enumInner"),
    ("    k.SU.Inner.", "Inner{...}", "z: i32", "kchain.unionInner"),
    ("    k.SUE.Inner.", "Inner{...}", "z: i32", "kchain.unionEnumInner"),
    ("    k.GG.F.", "F{...}", "name: i32", "kchain.genericNested"),
    ("    k.GG(i32).F.", "F{...}", "name: i32", "kchain.genericInstNested"),
    ("    KT.", "KT{...}", "name: i32", "kchain.typeAliasCtor"),
):
    _m = kcomp(KUSE, kuri, _needle, _tag)
    _ok, _det = ctor_ok(_m, _label, _shape)
    check("completion." + _tag, _ok, _det)

_kx = kcomp(KUSE, kuri, "    kx.", "kchain.aliasInstance")
check("completion.kchain.aliasInstance",
      set(_kx) == {"name", "flag"} and _kx["flag"].get("detail") == "bool",
      str({k_: v_.get("detail") for k_, v_ in _kx.items()}))
_ktl = kcomp(KUSE, kuri, "    KT{ .name = 1, .", "kchain.aliasLiteral")
# 续补沿用 completion.literalSecond 的既有口径：给全量字段（宿主按前缀
# 自行过滤），不在这里去重。
check("completion.kchain.aliasLiteral",
      "flag" in _ktl, str(sorted(_ktl)))
check("hover.kchain.typeAlias",
      "name: i32" in khover(KUSE, kuri, "    const KT: type = k.GG.F;", "KT", "kchain.aliasHover"))

# 链路 A2：具名嵌套类型 hover 必须贴 Inner 自己，不是外层容器
check("hover.kchain.nestedType",
      "pub const Inner" in khover(KUSE, kuri, "    k.SG.Inner.", "Inner", "kchain.innerHover"))

# 链路 B1：显式匿名类型标注的局部变量、嵌套点路径、编辑中的嵌套字面量
_ka = kcomp(KUSE, kuri, "    ka.", "kchain.anonAnnot")
check("completion.kchain.anonAnnot",
      set(_ka) == {"a", "nest"} and "struct { b: i32, c: i32 }" in (_ka["nest"].get("detail") or ""),
      str({k_: v_.get("detail") for k_, v_ in _ka.items()}))
_kan = kcomp(KUSE, kuri, "    ka.nest.", "kchain.anonAnnotDeep")
check("completion.kchain.anonAnnotDeep",
      set(_kan) == {"b", "c"} and _kan["c"].get("detail") == "i32", str(sorted(_kan)))
_kbl2 = kcomp(KUSE, kuri, ".nest = .{ .b = 2, .", "kchain.anonAnnotNestedLiteral")
check("completion.kchain.anonAnnotNestedLiteral",
      "c" in _kbl2 and "b" not in _kbl2, str(sorted(_kbl2)))
_kah = khover(KUSE, kuri, "    var ka: struct", "ka", "kchain.anonHover")
check("hover.kchain.anonAnnot",
      "struct" in _kah and ("a: i32" in _kah or "a:i32" in _kah)
      and ("nest" in _kah), _kah[:200])

# 链路 B2：vendor 里的函数/方法形参匿名嵌套 struct 与 self
_cfg = kcomp(KLIB, kliburi, "        cfg.", "kchain.methodParam")
check("completion.kchain.methodParam", set(_cfg) == {"a", "nest"}, str(sorted(_cfg)))
_cfgn = kcomp(KLIB, kliburi, "        cfg.nest.", "kchain.methodParamDeep")
check("completion.kchain.methodParamDeep", set(_cfgn) == {"b", "c"}, str(sorted(_cfgn)))
_slf = kcomp(KLIB, kliburi, "        self.", "kchain.methodSelf")
check("completion.kchain.methodSelf",
      {"id", "take"} <= set(_slf), str(sorted(_slf)))
_dat = kcomp(KLIB, kliburi, "    data.", "kchain.fnParamSameName")
check("completion.kchain.fnParamSameName",
      set(_dat) == {"b", "data"}, str(sorted(_dat)))
_datd = kcomp(KLIB, kliburi, "    data.data.", "kchain.fnParamSameNameDeep")
check("completion.kchain.fnParamSameNameDeep",
      set(_datd) == {"b", "i"}, str(sorted(_datd)))

# 链路 B3：snippet 能力线没被连坐
_s2 = kcomp(KUSE, kuri, "    if", "kchain.snippet")
check("completion.kchain.snippet",
      _s2.get("if", {}).get("insertTextFormat") == 2, str(sorted(_s2)))



# ⑪ 2026-11-12 18:xx 三张新截图（KEEP：与 ⑩ 同一条 8z 短命会话）
#   A 形参紧凑写法 `fn(b:i32,data:struct{…})` 与 `.{` 刚开括号就要有字段；
#   C `const G = m.MM; const g: type = G(T: i32);` 的别名链、g{ 与 gx.；
#   B Task 的能力全在实例 t. 上，类型空间只留 4 构造器 + 2 编译期常量。
MLIB = """pub const MM = fn (T: type) type {
    return struct {
        pub val: T,
        pub name: i32,
        pub const Inner = struct { pub z: i32 };
        pub const K: i32 = 9;
        pub const mk = fn (v: i32) i32 { return v; };
    };
};
pub const FC = fn(b:i32,data:struct{b:i32,data:struct{b:i32,i:i32}}) void {
    _ = b;
    data.
    data.data.
    _ = data;
};
"""
_mlib_path = os.path.join(TESTDIR, "vendor", "zxls8zm.zx")
open(_mlib_path, "w", encoding="utf-8").write(MLIB)
mliburi = "file:///" + _mlib_path.replace("\\", "/")

MUSE = """const std = @mod("std");
const m = @mod("zxls8zm");
const GA = m.MM;
pub const main = fn () i32 {
    m.FC(b: 0, data: struct{b:i32,data:struct{b:i32,i:i32}}.{ });
    m.FC(b: 0, data: struct{b:i32,data:struct{b:i32,i:i32}}.{ .b = 1, .data = .{ . } });
    m.FC(b: 0, data: struct{b:i32,data:struct{b:i32,i:i32}}.);
    const g: type = GA(T: i32);
    g.
    const h: type = m.MM(T: i32);
    h.
    var gx: g = g{ .name = 1, . };
    gx.
    var t = std.task.Task.single();
    t.
    std.task.Task.
    if
    return 0;
};
"""
_muse_path = os.path.join(SRC_DIR, "mchain.zx")
open(_muse_path, "w", encoding="utf-8").write(MUSE)
muri = "file:///" + _muse_path.replace("\\", "/")

# §136 泛型**值参数**容器（`fn (N: u64) type`，没有 `T: type`）：三条入口
# 都要出字段——实例 `v.`、别名字面量 `L{ .`、直接构造 `GvList(2){ .`。
# 2026-11-13 用户截图：`const L: type = List(2); L.` 只有构造骨架，
# 根因是 inferContainer 把返回类型 "type" 当容器名继续解析。
# 容器名带 Gv 前缀：std.array.List 进 std 后，SRC_DIR 里裸名 `List`
# 会把 `std.array.List(i32)` 的实例补全串到这个 fixture 上（同坑再踩）。
MGV = """const GvList = fn (N: u64) type {
    return struct {
        pub genId: *[N]u64,
        pub ptr: *[N]*void,
        pub const len = fn (self: Self) u64 { return N; };
    };
};

const L: type = GvList(2);

pub const main = fn () void {
    var v: L = L{ .genId = null, .ptr = null };
    v.
    var w = L{ .
    var z = GvList(2){ .
    var y = GvList(2){ .genId = null, .ptr = null };
    y.
};
"""
_mgv_path = os.path.join(SRC_DIR, "gval.zx")
open(_mgv_path, "w", encoding="utf-8").write(MGV)
mgvuri = "file:///" + _mgv_path.replace("\\", "/")

# §137 命名类型**形参**的成员补全（2026-11-13）：`fn (p: *P) { p. }` 一个
# 成员都不出——findDeclStmt 只认 var/let/const，形参没有声明语句；
# anonTyOfPath 只认 `name: struct{…}` 的匿名串，`*P` / `[]T` / `List(4)` 全落空。
# 容器名一律带 `Pv` 前缀：SRC_DIR 里所有 fixture 进同一个 scanFiles，与
# instChain 的 `P`、gval 的 `GvList` 同名会互相串味（本轮已踩过一次）。
PVM = """const PvUnit = struct {
    pub a: i32,
    pub b: i32,
};

const PvBox = fn (N: u64) type {
    return struct {
        pub genId: *[N]u64,
        pub cap: u64,
    };
};

const byVal = fn (pv: PvUnit) void {
    pv.
};

const byPtr = fn (pr: *PvUnit) void {
    pr.
};

const bySlice = fn (xs: []const i32) void {
    xs.
};

const byCtor = fn (l: *PvBox(4)) void {
    l.
};
"""
_pv_path = os.path.join(SRC_DIR, "pval.zx")
open(_pv_path, "w", encoding="utf-8").write(PVM)
pvuri = "file:///" + _pv_path.replace("\\", "/")

for _u, _txt in ((muri, MUSE), (mliburi, MLIB), (mgvuri, MGV), (pvuri, PVM)):
    s8({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
        "textDocument": {"uri": _u, "languageId": "zx", "version": 1, "text": _txt}}})


def mcomp(src, uri_, needle, tag, occurrence=0, back=0):
    global _rid
    _rid += 1
    idx, start = -1, 0
    for _ in range(occurrence + 1):
        idx = src.find(needle, start)
        if idx < 0:
            break
        start = idx + 1
    if idx < 0:
        print(f"FAIL {tag}: needle {needle!r} not found"); return {}
    off = idx + len(needle) - back
    s8({"jsonrpc": "2.0", "id": _rid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": uri_}, "position": pos(src, off)}})
    r, _ = r8(tag, _rid)
    items = (r["result"]["items"] if r and r.get("result") and isinstance(r["result"], dict)
             else (r["result"] if r and isinstance(r.get("result"), list) else []))
    return {i["label"]: i for i in items}


# A1 `.{` 后立刻触发（Zed 在 '{' 后就弹，光标前不是 '.'）
_m1 = mcomp(MUSE, muri, "i:i32}}.{", "mchain.litImmediate", 0)
check("completion.mchain.litImmediate",
      set(_m1) == {"b", "data"} and "struct" in (_m1.get("data", {}).get("detail") or ""),
      str(sorted(_m1)))
_m2 = mcomp(MUSE, muri, "i:i32}}.{ ", "mchain.litImmediateSpace", 0)
check("completion.mchain.litImmediateSpace", set(_m2) == {"b", "data"}, str(sorted(_m2)))

# A2 嵌套字面量 / 类型字面量后刚敲下 '.' / 紧凑 fn( 的函数体
_m3 = mcomp(MUSE, muri, ".data = .{ .", "mchain.litNested", 0)
check("completion.mchain.litNested", set(_m3) == {"b", "i"}, str(sorted(_m3)))
_m4 = mcomp(MUSE, muri, "i:i32}}.);", "mchain.typeLiteralDot", 0, 2)
check("completion.mchain.typeLiteralDot", set(_m4) == {"b", "data"}, str(sorted(_m4)))
_md = mcomp(MLIB, mliburi, "    data.", "mchain.fnBodyCompact")
check("completion.mchain.fnBodyCompact", set(_md) == {"b", "data"}, str(sorted(_md)))
_mdd = mcomp(MLIB, mliburi, "    data.data.", "mchain.fnBodyCompactDeep")
check("completion.mchain.fnBodyCompactDeep", set(_mdd) == {"b", "i"}, str(sorted(_mdd)))

# C 泛型类型值别名的别名链 / 字面量 / 实例
_mg = mcomp(MUSE, muri, "    g.", "mchain.genericAliasChain")
check("completion.mchain.genericAliasChain",
      {"Inner", "K", "mk"} <= set(_mg) and not ({"name", "val"} & set(_mg)),
      str(sorted(_mg)))
_mh = mcomp(MUSE, muri, "    h.", "mchain.genericModuleAlias")
check("completion.mchain.genericModuleAlias",
      {"Inner", "K", "mk"} <= set(_mh), str(sorted(_mh)))
_mgl = mcomp(MUSE, muri, "var gx: g = g{ .name = 1, .", "mchain.genericAliasLiteral")
check("completion.mchain.genericAliasLiteral", "val" in _mgl, str(sorted(_mgl)))
_mgi = mcomp(MUSE, muri, "    gx.", "mchain.genericAliasInstance")
check("completion.mchain.genericAliasInstance",
      {"val", "name"} <= set(_mgi), str(sorted(_mgi)))

# D 泛型值参数容器（§136）：实例 / 别名字面量 / 直接构造字面量
_gv1 = mcomp(MGV, mgvuri, "    v.", "gval.instance")
check("completion.gval.instanceMembers",
      {"genId", "ptr", "len"} <= set(_gv1), str(sorted(_gv1)))
_gv2 = mcomp(MGV, mgvuri, "var w = L{ .", "gval.aliasLiteral")
check("completion.gval.aliasLiteral",
      {"genId", "ptr"} <= set(_gv2), str(sorted(_gv2)))
_gv3 = mcomp(MGV, mgvuri, "var z = GvList(2){ .", "gval.ctorLiteral")
check("completion.gval.ctorLiteral",
      {"genId", "ptr"} <= set(_gv3), str(sorted(_gv3)))
# 实例来自构造字面量本身（用户截图的正规写法）——字段**和**方法都要出
_gv4 = mcomp(MGV, mgvuri, "    y.", "gval.ctorInstance")
check("completion.gval.ctorInstance",
      {"genId", "ptr", "len"} <= set(_gv4), str(sorted(_gv4)))

# §137 命名类型形参：值形参 / 指针形参 / 切片形参 / 泛型构造类型形参
_pv1 = mcomp(PVM, pvuri, "    pv.", "pv.valueParam")
check("completion.pv.valueParam", {"a", "b"} <= set(_pv1), str(sorted(_pv1)))
_pv2 = mcomp(PVM, pvuri, "pr.", "pv.ptrParam")
check("completion.pv.ptrParam", {"a", "b"} <= set(_pv2), str(sorted(_pv2)))
_pv3 = mcomp(PVM, pvuri, "    xs.", "pv.sliceParam")
check("completion.pv.sliceParam", {"len", "ptr"} <= set(_pv3), str(sorted(_pv3)))
_pv4 = mcomp(PVM, pvuri, "    l.", "pv.ctorParam")
check("completion.pv.ctorParam", {"genId", "cap"} <= set(_pv4), str(sorted(_pv4)))

# B Task（§128）：t. 只列保留的实例方法；scope/chan/timer 已不导出
_mt = mcomp(MUSE, muri, "    t.", "mchain.taskInstance")
check("completion.mchain.taskInstanceCapabilities",
      {"spawn", "await", "awaitAll", "cancel", "yield", "send", "recv",
       "trySend", "tryRecv", "select", "close"} <= set(_mt),
      str(sorted(_mt)))
check("completion.mchain.taskInstance.noRemoved",
      not ({"scope", "scopeSpawn", "scopeJoin", "scopeJoinTimeout",
            "scopeClose", "withScope", "chan", "chanSend", "chanRecv",
            "chanTrySend", "chanTryRecv", "chanClose", "sleep",
            "awaitTimeout"} & set(_mt)), str(sorted(_mt)))
check("completion.mchain.taskInstance.noGroup",
      not ({"group", "groupSpawn", "groupWait", "groupClose",
            "groupRun"} & set(_mt)), str(sorted(_mt)))
_mtt = mcomp(MUSE, muri, "    std.task.Task.", "mchain.taskType")
check("completion.mchain.taskTypeNamespace",
      set(_mtt) == {"coro", "single", "pool", "fromId", "SEL_AWAIT",
                    "SEL_RECV", "group"},
      str(sorted(_mtt)))
_ms = mcomp(MUSE, muri, "    if", "mchain.snippet")
check("completion.mchain.snippet",
      _ms.get("if", {}).get("insertTextFormat") == 2, str(sorted(_ms)))

# ---------------------------------------------------------------------------
# §157 类型构造式直接接字面量（用户 2026-11-13 截图 `var gen = Gen(i32,10){…}`）
# KEEP — 几条都不许删：
#   · `var gen = Ctor(i32,10){…}` 要有 `: Ctor(i32, 10)` 行内类型；
#   · `let n = gen.mem;` / `var q = gen.get();` 要有 `: i32`（泛型实参替换，
#     且字段默认值 `= n` 不能被拼进类型串）；
#   · hover 不能回显成 `let n: i32 = n = gen.mem`；
#   · `G` 补全要同时给调用骨架和 `Ctor(...){...}` 构造泛型实例骨架；
#   · `std.` 要能看到新注册的 math / net。
GEN157 = """const std = @mod("std");

const Gen157 = fn (T: type, n: T) type {
    return struct {
        mem: T = n,
        const get = fn (self: Self) T { return self.mem; };
    };
};

const main = fn () u8 {
    var gen = Gen157(i32, 10){ .mem = 10 };
    // §162 / §165：向量的行内类型：字面量 / 比较掩码 / std.simd 的返回类型。
    // 必须放在下面 `var p1 = gen.;` 之前：行内提示遍历到第一个语法错就停。
    var va = @Vec(f32, 4){1.0, 2.0, 3.0, 4.0};
    let vc = va < va;
    let vd = std.simd.select(vc, va, va);
    let vany = std.simd.any(vd);
    let vall = std.simd.all(vd);
    let m167 = std.simd.Mat4.identity();
    let q167 = std.simd.Quat.identity();
    _ = m167;
    _ = q167;
    let n = gen.mem;
    _ = n;
    std.println("{} {}", gen.get(), n);
    var q = gen.get();
    var p1 = gen.;
    var p2 = G;
    var p3 = std.;
    var p4 = std.math.sqrt;
    var p5 = std.math.;
    var p6 = std.simd.dot;
    var p7 = std.simd.;
    var p8 = @sh;
    var p9 = @Ve;
    var p10 = @se;
    var p11 = @an;
    var p12 = @al;
    var p13 = m167.;
    var p14 = q167.;
    var p15 = m167.mul;
    var p16 = std.simd.Mat4.;
    var vb = va.;
    return 0;
};
"""
g157uri = "file:///" + os.path.join(SRC_DIR, "gen157.zx").replace(chr(92), "/")
open(os.path.join(SRC_DIR, "gen157.zx"), "w", encoding="utf-8").write(GEN157)
s8({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": g157uri, "languageId": "zx", "version": 1, "text": GEN157}}})

s8({"jsonrpc": "2.0", "id": 5200, "method": "textDocument/inlayHint", "params": {
    "textDocument": {"uri": g157uri},
    "range": {"start": {"line": 0, "character": 0},
              "end": {"line": len(GEN157.split("\n")), "character": 0}}}})
_r, _ = r8("gen157.inlay", 5200)
_g157_hints = {}
for _h in ((_r or {}).get("result") or []):
    _lb = _h.get("label")
    if isinstance(_lb, str) and _h.get("kind") == 1:
        _g157_hints[(_h["position"]["line"], _h["position"]["character"])] = _lb

def g157_hint(decl):
    p = pos(GEN157, GEN157.index(decl) + len(decl))
    return _g157_hints.get((p["line"], p["character"]))

_g157_detail = str(sorted(_g157_hints.items()))[:260]
check("inlayHint.ctorLiteral.instance",
      g157_hint("var gen") == ": Gen157(i32, 10)", _g157_detail)
check("inlayHint.ctorLiteral.field", g157_hint("let n") == ": i32", _g157_detail)
check("inlayHint.ctorLiteral.method", g157_hint("var q") == ": i32", _g157_detail)

def g157_hover(decl, rid):
    s8({"jsonrpc": "2.0", "id": rid, "method": "textDocument/hover", "params": {
        "textDocument": {"uri": g157uri},
        "position": pos(GEN157, GEN157.index(decl) + len(decl))}})
    r, _ = r8("gen157.hover", rid)
    res = (r or {}).get("result")
    c = res.get("contents") if isinstance(res, dict) else None
    if isinstance(c, dict):
        return c.get("value", "")
    return c if isinstance(c, str) else ""

_h_gen = g157_hover("var gen", 5201)
check("hover.ctorLiteral.instance",
      "var gen: Gen157(i32, 10)" in _h_gen and "void" not in _h_gen, _h_gen[:160])
_h_n = g157_hover("let n", 5202)
check("hover.ctorLiteral.field",
      "let n: i32 = gen.mem" in _h_n and "= n =" not in _h_n, _h_n[:160])

def g157_comp(needle, rid):
    off = GEN157.index(needle) + len(needle)
    s8({"jsonrpc": "2.0", "id": rid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": g157uri}, "position": pos(GEN157, off)}})
    r, _ = r8("gen157.completion", rid)
    res = (r or {}).get("result")
    return res.get("items", []) if isinstance(res, dict) else (res or [])

_g157_gen = g157_comp("var p1 = gen.", 5203)
check("completion.ctorLiteral.members",
      {"mem", "get"} <= {i["label"] for i in _g157_gen},
      str([(i["label"], i.get("detail")) for i in _g157_gen])[:200])
_g157_snips = {i["label"]: i for i in g157_comp("var p2 = G", 5204)}
check("completion.ctorLiteral.callSnippet",
      "Gen157(...)" in _g157_snips
      and _g157_snips["Gen157(...)"].get("insertTextFormat") == 2,
      str(sorted(_g157_snips))[:200])
check("completion.ctorLiteral.instanceSnippet",
      "Gen157(...){...}" in _g157_snips
      and ".mem =" in (_g157_snips["Gen157(...){...}"].get("insertText") or ""),
      str(_g157_snips.get("Gen157(...){...}"))[:240])
_g157_std = {i["label"] for i in g157_comp("var p3 = std.", 5205)}
check("completion.std.mathNet", {"math", "net"} <= _g157_std,
      str(sorted(_g157_std))[:200])

# §158 用户截图 1：`std.math.sqrt` 只能出一条，
# 且签名/snippet 不能退化成 C 原型；常量不跟函数同前缀。
_g157_sqrt = g157_comp("var p4 = std.math.sqrt", 5206)
check("completion.mathSqrt.singleItem",
      len(_g157_sqrt) == 1 and _g157_sqrt[0].get("label") == "sqrt",
      str(_g157_sqrt)[:240])
check("completion.mathSqrt.zxSignature",
      len(_g157_sqrt) == 1
      and _g157_sqrt[0].get("detail") == "sqrt(x: f64) f64"
      and _g157_sqrt[0].get("insertText") == "sqrt(${1:x})$0",
      str(_g157_sqrt)[:240])
_g157_math = {i["label"] for i in g157_comp("var p5 = std.math.", 5207)}
check("completion.mathConstants.noSqrt2",
      "root2" in _g157_math and "sqrt2" not in _g157_math,
      str(sorted(_g157_math))[:240])

def g157_hover_sqrt(rid):
    off = GEN157.index("std.math.sqrt") + len("std.math.sqrt") - 1
    s8({"jsonrpc": "2.0", "id": rid, "method": "textDocument/hover", "params": {
        "textDocument": {"uri": g157uri},
        "position": pos(GEN157, off)}})
    r, _ = r8("gen157.hover.sqrt", rid)
    res = (r or {}).get("result")
    c = res.get("contents") if isinstance(res, dict) else None
    if isinstance(c, dict):
        return c.get("value", "")
    return c if isinstance(c, str) else ""

_h_sqrt = g157_hover_sqrt(5208)
check("hover.mathSqrt.zxSignature",
      "sqrt(x: f64) f64" in _h_sqrt and "sqrt(f64)" not in _h_sqrt,
      _h_sqrt[:160])

# §165：std.simd 是 `@Vec` 的数学层，补全里要带完整 Zx 签名（形参类型就是
# `@Vec` 的别名，不再是包一层的 Vec4f/Vec4i）。
_g165_dot = g157_comp("var p6 = std.simd.dot", 5209)
check("completion.simd.dot.zxSignature",
      len(_g165_dot) == 1
      and _g165_dot[0].get("label") == "dot"
      and _g165_dot[0].get("detail") == "dot(a: V4, b: V4) f32"
      and _g165_dot[0].get("insertText") == "dot(${1:a}, ${2:b})$0",
      str(_g165_dot)[:260])
_g165_members = {i["label"] for i in g157_comp("var p7 = std.simd.", 5210)}
check("completion.simd.members",
      {"V4", "M4", "Quat", "Mat3", "Mat4", "dot", "cross", "select", "any", "all"} <= _g165_members,
      str(sorted(_g165_members))[:240])

# §167：矩阵是 std.simd 里的普通容器。`Type.` 只列关联函数（构造入口），
# 实例补全才列方法——两套列表不许混。四元数互转的两个入口在实例侧。
_g167_ctors = {i["label"] for i in g157_comp("var p16 = std.simd.Mat4.", 5223)}
check("completion.simd.mat4Ctors",
      {"identity", "zero", "fromCols", "fromRows", "fromScale",
       "fromTranslation", "fromQuat", "fromAxisAngle",
       "perspective", "ortho", "lookAt"} <= _g167_ctors,
      str(sorted(_g167_ctors))[:260])
_g167_mat4 = {i["label"] for i in g157_comp("var p13 = m167.", 5220)}
check("completion.simd.mat4Methods",
      {"mul", "mulVec", "transpose", "determinant", "invert",
       "toMat3", "toQuat", "eq", "approxEq"} <= _g167_mat4,
      str(sorted(_g167_mat4))[:260])
_g167_quat = {i["label"] for i in g157_comp("var p14 = q167.", 5221)}
check("completion.simd.quatMatrixConversions",
      {"toMat3", "toMat4", "rotate", "slerp", "mul", "conj"} <= _g167_quat,
      str(sorted(_g167_quat))[:260])
_g167_mul = [i for i in g157_comp("var p15 = m167.mul", 5222)
             if i.get("label") == "mul"]
check("completion.simd.mat4.mulSignature",
      len(_g167_mul) == 1
      and "mul(self: Mat4, o: Mat4) Mat4" in (_g167_mul[0].get("detail") or ""),
      str(_g167_mul)[:260])

# §192：@shim 通道已删除——`@sh` 打到一半不再出补全（与 §165 的
# `@se` / `@an` 同款守门：删除的内建不进 `@` 列表）。
_g192_shim = g157_comp("var p8 = @sh", 5212)
check("completion.shim.removed",
      len(_g192_shim) == 0,
      str(_g192_shim)[:200])

# §162：`@Vec(T, N)` 是类型内建，也要进 `@` 补全（写到一半 `@Ve` 同样走
# 这条路）。类型内建的实参是类型，不在值位置当函数看——一样带 snippet。
_g162_vec = g157_comp("var p9 = @Ve", 5214)
check("completion.vec.partialAtIsBuiltin",
      len(_g162_vec) == 1
      and _g162_vec[0].get("label") == "Vec"
      and _g162_vec[0].get("kind") == 15
      and _g162_vec[0].get("insertText") == "Vec(${1:T}, ${2:N})$0"
      and "@Vec(T, N)" in (_g162_vec[0].get("detail") or ""),
      str(_g162_vec)[:260])

# §162 — 向量的行内类型与成员表。三件事各自守住一段推断：
#   字面量 `@Vec(f32, 4){…}`、比较结果（掩码，lane 宽度跟操作数走）、
#   `@select` 返回值的类型。`va.` 的成员在源文件里没有声明文本，全靠
#   类型串 `@Vec(f32, 4)` 现算（`.len/.size` + 分量 `.0`..`.3`）。
check("inlayHint.vec.literal", g157_hint("var va") == ": @Vec(f32, 4)", _g157_detail)
check("inlayHint.vec.cmpMask", g157_hint("let vc") == ": @Vec(i32, 4)", _g157_detail)
check("inlayHint.vec.select", g157_hint("let vd") == ": V4", _g157_detail)  # 类型按签名源码写，别名就是 V4
_g162_members = {i["label"] for i in g157_comp("var vb = va.", 5216)}
check("completion.vec.members",
      {"len", "size", "0", "1", "2", "3"} <= _g162_members and "4" not in _g162_members,
      str(sorted(_g162_members))[:240])

# §165 — `@select` 不再是内建：`@se` 不该弹出任何 `@` 内建（它现在是
# `std.simd.select`）。
_g165_sel = g157_comp("var p10 = @se", 5217)
check("completion.noSelectBuiltin", len(_g165_sel) == 0, str(_g165_sel)[:240])

# §165 — 掩码判真也是普通函数：
#   `std.simd.any` / `std.simd.all` 的行内类型是 bool（向量收敛成一个值）；
#   `@an` / `@al` 两个前缀不该再弹出任何内建。
check("inlayHint.vec.any", g157_hint("let vany") == ": bool", _g157_detail)
check("inlayHint.vec.all", g157_hint("let vall") == ": bool", _g157_detail)
_g165_any = g157_comp("var p11 = @an", 5218)
check("completion.noAnyBuiltin", len(_g165_any) == 0, str(_g165_any)[:240])
_g165_all = g157_comp("var p12 = @al", 5219)
check("completion.noAllBuiltin", len(_g165_all) == 0, str(_g165_all)[:240])



try:
    p8.kill()
except Exception:
    pass

# 9 shutdown/exit
send({"jsonrpc": "2.0", "id": 8, "method": "shutdown", "params": None})
recv_id("shutdown", 8)
send({"jsonrpc": "2.0", "method": "exit", "params": None})
try:
    proc.wait(timeout=5)
except Exception:
    proc.kill()

print("=" * 40)

# 10e 反过来的那条：客户端**没**声明 codeDescriptionSupport 时不能发这个字段。
# 老客户端对未知字段做严格校验，一条不认识的字段就能让整包诊断被丢弃——
# 表现为"编辑器里一条波浪线都没有"，最难查的一类。这里单起一个短命服务器验。
def no_cd_client():
    p = subprocess.Popen([ZXLS_BIN], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    def raw_send(msg):
        d = json.dumps(msg).encode()
        p.stdin.write(f"Content-Length: {len(d)}\r\n\r\n".encode() + d)
        p.stdin.flush()
    def raw_recv(timeout=30):
        import time as _t3
        deadline = _t3.time() + timeout
        buf = b""
        while _t3.time() < deadline:
            c = p.stdout.read(1)
            if not c:
                return None
            buf += c
            if buf.endswith(b"\r\n\r\n"):
                hdr = dict(l.split(b": ", 1) for l in buf.split(b"\r\n") if b": " in l)
                n = int(hdr[b"Content-Length"])
                body = p.stdout.read(n)
                return json.loads(body)
        return None
    raw_send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
        "rootUri": "file:///" + TESTDIR.replace("\\", "/"), "capabilities": {}}})
    raw_recv()
    raw_send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
    raw_send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
        "textDocument": {"uri": uri, "languageId": "zx", "version": 1, "text": BADTXT}}})
    got = []
    for _ in range(20):
        m = raw_recv()
        if not m:
            break
        if m.get("method") == "textDocument/publishDiagnostics" and m["params"].get("diagnostics"):
            got = m["params"]["diagnostics"]
            break
    raw_send({"jsonrpc": "2.0", "id": 2, "method": "shutdown", "params": None})
    raw_recv()
    raw_send({"jsonrpc": "2.0", "method": "exit", "params": None})
    try: p.wait(timeout=5)
    except Exception: p.kill()
    return got

plain = no_cd_client()
check("diagnostics.noCodeDescriptionForOldClients",
      bool(plain) and all("codeDescription" not in d for d in plain), str(plain)[:300])

# §134 新 E 码接进 LSP 显示通道：zxls 源码零改动。diagnosticsWorker 解析编译器
# stderr 的 `path:line:col: error[CODE]: msg` -> publishDiagnostics，
# codeDescription.href 按码小写自动拼出 docs/errors.md#e0510。用独立短命会话
# 把这条链钉死——能力线以守门用例保存，不许删用例变绿。
HANDLE_TABLE = """const std = @mod("std");

const Handle = struct { pos: usize };

const Table = fn (cap: usize) type {
    return struct {
        gens: *[cap]u64,
        ptr: *[cap]*u8,
        used: usize,

        pub const init = fn (g: *[cap]u64, p: *[cap]*u8) Self {
            return Self{ .gens = g, .ptr = p, .used = 0 };
        };
        pub const create = fn (self: *Self) ?Handle {
            if (self.used >= cap) { return null; }
            let i = self.used;
            self.used = i + 1;
            return Handle{ .pos = i };
        };
        pub const get = fn (self: *Self, h: Handle) ?*u8 {
            return self.getUnchecked(h);
        };
        const getUnchecked = fn (self: *Self, h: Handle) ?*u8 {
            return (*u8)&self.ptr[h.pos];
        };
        pub const free = fn (self: *Self, h: Handle) bool {
            self.ptr[h.pos] = null;
            return true;
        };
    };
};
"""

E0510_TXT = HANDLE_TABLE + """
pub const main = fn () u8 {
    var gens: *[4]u64 = &.{ 0, 0, 0, 0 };
    var raw: *[4]*u8 = &.{ null, null, null, null };
    var t = Table(4).init(gens, raw);
    let h = t.create().?;
    std.println("free {}", t.free(h));
    let p = t.get(h).?;
    std.println("{}", (i32)p[0]);
    return 0;
};
"""

# §135：抹源（Handle 存进结构体字段）是 E0511，不是运行时查表。
E0511_TXT = HANDLE_TABLE + """
const Holder = struct { h: Handle };

pub const main = fn () u8 {
    var gens: *[4]u64 = &.{ 0, 0, 0, 0 };
    var raw: *[4]*u8 = &.{ null, null, null, null };
    var t = Table(4).init(gens, raw);
    let h = t.create().?;
    let boxed = Holder{ .h = h };
    let p = t.get(boxed.h).?;
    std.println("{}", (i32)p[0]);
    return 0;
};
"""
def diag_client(name, text):
    uri = "file:///" + os.path.join(TESTDIR, name).replace("\\", "/")
    p = subprocess.Popen([ZXLS_BIN], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    def es(msg):
        d = json.dumps(msg).encode()
        p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(d) + d)
        p.stdin.flush()
    def er(timeout=30):
        import time as _t5
        end = _t5.time() + timeout
        buf = b""
        while _t5.time() < end:
            c = p.stdout.read(1)
            if not c:
                return None
            buf += c
            if buf.endswith(b"\r\n\r\n"):
                hdr = dict(l.split(b": ", 1) for l in buf.split(b"\r\n") if b": " in l)
                return json.loads(p.stdout.read(int(hdr[b"Content-Length"])))
        return None
    es({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
        "rootUri": "file:///" + TESTDIR.replace("\\", "/"),
        "capabilities": {"textDocument": {"publishDiagnostics": {
            "codeDescriptionSupport": True}}}}})
    er()
    es({"jsonrpc": "2.0", "method": "initialized", "params": {}})
    es({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
        "textDocument": {"uri": uri, "languageId": "zx",
                         "version": 1, "text": text}}})
    got = []
    for _ in range(20):
        m = er()
        if not m:
            break
        if (m.get("method") == "textDocument/publishDiagnostics"
                and m["params"].get("uri", "").endswith(name)
                and m["params"].get("diagnostics")):
            got = m["params"]["diagnostics"]
            break
    es({"jsonrpc": "2.0", "id": 2, "method": "shutdown", "params": None})
    er()
    es({"jsonrpc": "2.0", "method": "exit", "params": None})
    try: p.wait(timeout=5)
    except Exception: p.kill()
    return got

_e510_diags = diag_client("e0510.zx", E0510_TXT)
_d510 = next((d for d in _e510_diags if d.get("code") == "E0510"), None)
_h510 = ((_d510 or {}).get("codeDescription") or {}).get("href", "")
check("diagnostics.e0510CodeDescription",
      _d510 is not None and _d510.get("source") == "zxc"
      and _h510.endswith("docs/errors.md#e0510"),
      _h510 or str(_e510_diags)[:300])

_e511_diags = diag_client("e0511.zx", E0511_TXT)
_d511 = next((d for d in _e511_diags if d.get("code") == "E0511"), None)
_h511 = ((_d511 or {}).get("codeDescription") or {}).get("href", "")
check("diagnostics.e0511CodeDescription",
      _d511 is not None and _d511.get("source") == "zxc"
      and _h511.endswith("docs/errors.md#e0511"),
      _h511 or str(_e511_diags)[:300])

# §158 用户截图 2：LSP 单文件 --check 不能再把
# stackAlloc 判成 E0401。故意留一个真错逼诊断包非空，
# 同时证明种内建表后诊错仍然会报。
S158_BARE = """pub const main = fn () u8 {
    var st = stackAlloc(u8, 4);
    st.0 = 65;
    return bogusName;
};
"""
_s158_diags = diag_client("s158bare.zx", S158_BARE)
check("diagnostics.s158.bareBuiltinNoFalseE0401",
      any(d.get("code") == "E0401" and "bogusName" in d.get("message", "")
          for d in _s158_diags)
      and not any("stackAlloc" in d.get("message", "") for d in _s158_diags),
      str(_s158_diags)[:300])

# ---------------------------------------------------------------------------
# 同一个文件反复 didOpen（守一个真崩过的悬垂键）
#
# didOpen/didChange 换文档时的顺序是「摘旧文档 → free 它的 path/text」。
# 脏表（dirty）的**键就是那份 path**，如果 free 之后才摘脏表，脏表里就留
# 下一批悬垂键：下一次 put 触发扩容重哈希，或者诊断线程拿它去 docs.get，
# 读到的全是已释放内存。ReleaseSafe 里表现成 hash_map.zig growIfNeeded 的
# panic——整个语言服务器直接退出，用户看到的是"补全突然全没了"。
#
# 触发条件就是同一个 uri 开两次（编辑器重开文件、格式化后重载都会撞上），
# 所以这里连开 30 次、内容长度每轮不同（逼分配器换块），中间插别的文件
# 把 docs 表撑大，最后要一条 completion 还必须应答。
def dup_open_guard():
    if os.name != "nt":
        return None
    d = os.path.join(TESTDIR, "_dup")
    os.makedirs(d, exist_ok=True)
    doc = "file:///" + os.path.join(d, "dup.zx").replace("\\", "/")
    p = subprocess.Popen([ZXLS_BIN], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)

    def snd(m):
        data = json.dumps(m).encode()
        p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(data) + data)
        p.stdin.flush()

    def rcv():
        hdr = b""
        while not hdr.endswith(b"\r\n\r\n"):
            c = p.stdout.read(1)
            if not c:
                return None
            hdr += c
        ln = int([l for l in hdr.split(b"\r\n")
                  if l.lower().startswith(b"content-length")][0].split(b":")[1])
        return json.loads(p.stdout.read(ln))

    try:
        snd({"jsonrpc": "2.0", "id": 1, "method": "initialize",
             "params": {"capabilities": {}, "rootUri": "file:///" + d.replace("\\", "/")}})
        while True:
            m = rcv()
            if m is None or m.get("id") == 1:
                break
        snd({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        for i in range(30):
            body = ("const std = @mod(\"std\");\n\n"
                    "pub const main = fn () i32 {\n"
                    + ("    let a%d = %d;\n" % (i, i)) * (1 + i % 7)
                    + "    return 0;\n};\n")
            snd({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": doc, "languageId": "zx",
                                 "version": i + 1, "text": body}}})
            other = "file:///" + os.path.join(d, "o%d.zx" % i).replace("\\", "/")
            snd({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": other, "languageId": "zx", "version": 1,
                                 "text": "pub const x = fn () i32 {\n    return %d;\n};\n" % i}}})
            if i % 5 == 0:
                snd({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
                    "textDocument": {"uri": doc, "version": 100 + i},
                    "contentChanges": [{"text": body + "// tail\n"}]}})
        snd({"jsonrpc": "2.0", "id": 2, "method": "textDocument/completion", "params": {
            "textDocument": {"uri": doc}, "position": {"line": 1, "character": 0}}})
        while True:
            m = rcv()
            if m is None:
                return False
            if m.get("id") == 2:
                return True
    finally:
        try:
            p.kill()
        except Exception:
            pass
        shutil.rmtree(d, ignore_errors=True)

dup = dup_open_guard()
if dup is None:
    print("skip completion.dupOpen (非 Windows)")
else:
    check("completion.dupOpen.alive", dup,
          "同一文件重复 didOpen 之后服务器死了（脏表悬垂键）")

# §191 同名局部不串味：两个函数各有一个 `p`（main 的在前、later 的在后）。
# findDeclStmt 全文首匹配且 const→let→var→fn 按关键字顺序，later 里后出现的
# `let p`（heapSlice 切片）会抢走 main 里 `var p`（Person 实例）的声明——
# `p.` 弹出切片的 len 而不是字段 name/age（用户 2026-11-20 截图）。
# 位置感知后「光标之前、最近声明赢」，main 里的 p. 必须是 Person 的字段。
def shadow_guard():
    import tempfile
    d = tempfile.mkdtemp(prefix="zxshadow")
    os.makedirs(os.path.join(d, "src"), exist_ok=True)
    doc = "file:///" + os.path.join(d, "src", "m.zx").replace("\\", "/")
    body = ('const std = @mod("std");\n\n'
            'const Person = struct(T:type){\n'
            '    name:[]const u8,\n'
            '    age:T,\n'
            '};\n\n'
            'pub const main = fn () u8 {\n'
            '    var p = Person(i32){ .name = "a", .age = 1 };\n'
            '    p.\n'
            '    return 0;\n'
            '};\n\n'
            'const later = fn () u8 {\n'
            '    let p = std.heapSlice(std.heap.page, u8, 4);\n'
            '    _ = p;\n'
            '    return 0;\n'
            '};\n')
    try:
        p2 = subprocess.Popen([ZXLS_BIN], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, cwd=d)
        snd = lambda o: (p2.stdin.write(
            ("Content-Length: %d\r\n\r\n" % len(json.dumps(o).encode())).encode()
            + json.dumps(o).encode()), p2.stdin.flush())
        def rcv():
            hdr = b""
            while not hdr.endswith(b"\r\n\r\n"):
                c = p2.stdout.read(1)
                if not c:
                    return None
                hdr += c
            ln = int([l for l in hdr.split(b"\r\n")
                      if l.lower().startswith(b"content-length")][0].split(b":")[1])
            return json.loads(p2.stdout.read(ln))
        snd({"jsonrpc": "2.0", "id": 1, "method": "initialize",
             "params": {"capabilities": {}, "rootUri": "file:///" + d.replace("\\", "/")}})
        while True:
            m = rcv()
            if m is None or m.get("id") == 1:
                break
        snd({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        snd({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": doc, "languageId": "zx", "version": 1,
                             "text": body}}})
        line = body.split("\n").index("    p.")
        snd({"jsonrpc": "2.0", "id": 2, "method": "textDocument/completion",
             "params": {"textDocument": {"uri": doc},
                        "position": {"line": line, "character": 6}}})
        labels = None
        import time as _t
        deadline = _t.time() + 30
        while _t.time() < deadline:
            m = rcv()
            if m is None:
                break
            if m.get("id") == 2:
                r = m.get("result") or {}
                items = r.get("items", r if isinstance(r, list) else [])
                labels = [i.get("label") for i in items]
                break
        return labels
    finally:
        try:
            p2.kill()
        except Exception:
            pass
        shutil.rmtree(d, ignore_errors=True)

labels = shadow_guard()
if labels is None:
    print("skip completion.shadowGuard (超时)")
else:
    check("completion.shadowGuard.fields",
          "name" in labels and "age" in labels,
          "p. 应弹 Person 字段 name/age，实际: %s" % labels[:12])
    check("completion.shadowGuard.noSliceLen",
          "len" not in labels and "ptr" not in labels,
          "后面函数的同名 let p 不得抢声明（不应出切片成员），实际: %s" % labels[:12])

# 性能守门：inlayHint 必须是线性扫描。
#
# 这里踩过两次 O(文件长度) 的坑，都藏在"看起来无害"的写法里：
#   · 找声明关键字时对三个关键字各做一次 indexOfPos（它一路搜到文件尾），
#     于是每个绑定 = O(文件长度)，整个请求 O(绑定数 × 文件长度)；
#   · 每条提示都调 offsetToLineCol（每次从文件头数换行）。
# 16000 行 / 48000 条提示的老实现要 20s，编辑器里就是"行内提示卡死"。
# 单测断言一个绝对上限（够宽松，慢机器也不误报）+ 提示条数（防止靠"什么都不返回"
# 蒙混过关）。
def inlay_perf():
    if os.name != "nt":
        return None
    n = 16000
    body = ['const std = @mod("std");',
            'const add = fn (left: i32, right: i32) i32 { return left + right; };',
            'pub const main = fn () u8 {']
    for i in range(n):
        body.append(f'    let v{i} = add({i}, {i + 1});')
    body.append('    return 0;')
    body.append('};')
    src = "\n".join(body) + "\n"
    path = os.path.join(TESTDIR, "_perf_inlay.zx")
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    uri = "file:///" + path.replace("\\", "/")
    proc = subprocess.Popen([ZXLS_BIN], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    def snd(m):
        d = json.dumps(m).encode()
        proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(d) + d)
        proc.stdin.flush()
    def rcv():
        hdr = b""
        while not hdr.endswith(b"\r\n\r\n"):
            c = proc.stdout.read(1)
            if not c:
                return None
            hdr += c
        ln = int([l for l in hdr.split(b"\r\n")
                  if l.lower().startswith(b"content-length")][0].split(b":")[1])
        return json.loads(proc.stdout.read(ln))
    try:
        snd({"jsonrpc": "2.0", "id": 1, "method": "initialize",
             "params": {"capabilities": {}, "rootUri": uri}})
        while True:
            m = rcv()
            if m is None or m.get("id") == 1:
                break
        snd({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        snd({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": uri, "languageId": "zx", "version": 1, "text": src}}})
        # 等诊断落地，确保后面测的只有 inlay
        while True:
            m = rcv()
            if m is None:
                break
            if m.get("method") == "textDocument/publishDiagnostics":
                break
        t0 = time.time()
        snd({"jsonrpc": "2.0", "id": 2, "method": "textDocument/inlayHint", "params": {
            "textDocument": {"uri": uri},
            "range": {"start": {"line": 0, "character": 0},
                      "end": {"line": len(body), "character": 0}}}})
        got = None
        while True:
            m = rcv()
            if m is None:
                break
            if m.get("id") == 2:
                got = m
                break
        dt = time.time() - t0
        nh = len((got or {}).get("result") or [])
        return dt, nh, n
    finally:
        try:
            proc.kill()
        except Exception:
            pass
        try:
            os.remove(path)
        except Exception:
            pass


# ---------------------------------------------------------------------------
# §172 tag 补全：`tag.` → close + 本文件出现过的 tag 名（不再是 0/len/size）。
# 用独立服务器：主服务器在上面已经 shutdown/exit 退出了。
def tag_completion_probe():
    tdir = os.path.join(TESTDIR, "tagdoc%d.zx" % 0)
    turi = "file:///" + tdir.replace("\\", "/")
    tagp = subprocess.Popen([ZXLS_BIN], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    def tes(msg):
        tdata = json.dumps(msg).encode()
        tagp.stdin.write(b"Content-Length: %d\r\n\r\n" % len(tdata) + tdata)
        tagp.stdin.flush()
    def ter(timeout=30):
        import time as _t9
        end = _t9.time() + timeout
        tbuf = b""
        while _t9.time() < end:
            c = tagp.stdout.read(1)
            if not c:
                return None
            tbuf += c
            if tbuf.endswith(b"\r\n\r\n"):
                thdr = dict(l.split(b": ", 1) for l in tbuf.split(b"\r\n") if b": " in l)
                return json.loads(tagp.stdout.read(int(thdr[b"Content-Length"])))
        return None
    labels = set()
    try:
        TAGDOC = chr(10).join([
            'const std = @mod("std");',
            '',
            'const main = fn () u8 {',
            '    var p = tag.buf : std.heapAlloc(std.heap.page, i32, 4);',
            '    return 0;',
            '};',
        ]) + chr(10)
        tes({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "rootUri": "file:///" + TESTDIR.replace("\\", "/")}})
        ter()
        tes({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        tes({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": turi, "languageId": "zx", "version": 1, "text": TAGDOC}}})
        ter()                                  # 吃掉诊断通知
        _toff = TAGDOC.index("tag.buf") + 4    # 光标贴在 '.' 之后
        _tline = TAGDOC.count(chr(10), 0, _toff)
        _tls = TAGDOC.rfind(chr(10), 0, _toff) + 1
        tes({"jsonrpc": "2.0", "id": 2, "method": "textDocument/completion", "params": {
            "textDocument": {"uri": turi},
            "position": {"line": _tline, "character": _toff - _tls}}})
        _m = ter()
        if _m and _m.get("result"):
            labels = {it["label"] for it in _m["result"]["items"]}
    finally:
        try:
            tagp.kill()
        except Exception:
            pass
    return labels

_tag_labels = tag_completion_probe()
check("completion.tag.members", {"close", "buf"} <= _tag_labels
      and not ({"len", "size", "0"} & _tag_labels), str(sorted(_tag_labels)))

perf = inlay_perf()
if perf is None:
    print("skip inlayHint.perf (非 Windows)")
else:
    dt, nh, n = perf
    check(f"inlayHint.perf.{n}行", dt < 6.0 and nh >= n * 3,
          "%.2fs, %d 条提示（超时或提示数不足：多半又退化成 O(绑定数 × 文件长度)）" % (dt, nh))
    print("     inlayHint: %.3f s, %d 条提示, %d 行" % (dt, nh, n))

# ---------------------------------------------------------------------------
# §190 泛型类型的构造骨架（2026-11-20 用户截图：裸名补全只有「调用骨架」，
# 「构造泛型实例」整条消失——根因 genericBodyOf 糖模式硬编码 " = struct ("，
# 用户风格 struct(T: type, …) 括号前没有空格匹配不上）。独立进程探针。
def gctor_probe():
    d = os.path.join(TESTDIR, "gctor")
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    doc = "file:///" + os.path.join(d, "main.zx").replace("\\", "/")
    body = ('const Person = struct(T: type, n: u32){\n'
            '    age: T,\n'
            '    name: []const u8,\n'
            '};\n\n'
            'pub const main = fn () u8 {\n'
            '    return Per\n'
            '};\n')
    try:
        p3 = subprocess.Popen([ZXLS_BIN], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, cwd=d)
        snd = lambda o: (p3.stdin.write(
            ("Content-Length: %d\r\n\r\n" % len(json.dumps(o).encode())).encode()
            + json.dumps(o).encode()), p3.stdin.flush())
        def rcv():
            hdr = b""
            while not hdr.endswith(b"\r\n\r\n"):
                c = p3.stdout.read(1)
                if not c:
                    return None
                hdr += c
            ln = int([l for l in hdr.split(b"\r\n")
                      if l.lower().startswith(b"content-length")][0].split(b":")[1])
            return json.loads(p3.stdout.read(ln))
        snd({"jsonrpc": "2.0", "id": 1, "method": "initialize",
             "params": {"capabilities": {}, "rootUri": "file:///" + d.replace("\\", "/")}})
        while True:
            m = rcv()
            if m is None or m.get("id") == 1:
                break
        snd({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        snd({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": doc, "languageId": "zx", "version": 1,
                             "text": body}}})
        off = body.index("return Per") + len("return Per")
        _gline = body.count("\n", 0, off)
        _gcol = off - (body.rfind("\n", 0, off) + 1)
        snd({"jsonrpc": "2.0", "id": 2, "method": "textDocument/completion",
             "params": {"textDocument": {"uri": doc},
                        "position": {"line": _gline, "character": _gcol}}})
        items = None
        import time as _t
        deadline = _t.time() + 30
        while _t.time() < deadline:
            m = rcv()
            if m is None:
                break
            if m.get("id") == 2:
                r = m.get("result") or {}
                items = r.get("items", r if isinstance(r, list) else [])
                break
        return items
    finally:
        try:
            p3.kill()
        except Exception:
            pass
        shutil.rmtree(d, ignore_errors=True)

_gitems = gctor_probe()
_glabels = {i.get("label") for i in _gitems} if _gitems else set()
_gdet = {i.get("label"): i.get("detail") for i in _gitems} if _gitems else {}
check("completion.gctor.ctor",
      "Person(...){...}" in _glabels
      and _gdet.get("Person(...){...}") == "构造泛型实例", str(sorted(_glabels)))
check("completion.gctor.call", "Person(...)" in _glabels, str(sorted(_glabels)))

# ---------------------------------------------------------------------------
# §190 补丁二 + 嵌套类型递归解析（2026-11-20 用户两张截图）。独立进程探针：
#   1) std.heapAlloc 的类型提示出形参名 `*[n]T`，不是实参替换后的 `*[10]u8`；
#   2) 泛型实例化外层字面量里的嵌套 `.{` 没有成员提示（'{' 前是 ')' 名字回扫断）；
#   3) 函数形参是命名容器时匿名实参补全整条落空（structTyOf 只认内联 struct）。
def multi_probe(body, reqs):
    """reqs: [(method, offset, tag)]，method ∈ completion/inlayHint → {tag: result}"""
    d = os.path.join(TESTDIR, "nest")
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    doc = "file:///" + os.path.join(d, "main.zx").replace("\\", "/")
    out = {}
    try:
        p3 = subprocess.Popen([ZXLS_BIN], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, cwd=d)
        snd = lambda o: (p3.stdin.write(
            ("Content-Length: %d\r\n\r\n" % len(json.dumps(o).encode())).encode()
            + json.dumps(o).encode()), p3.stdin.flush())
        def rcv():
            hdr = b""
            while not hdr.endswith(b"\r\n\r\n"):
                c = p3.stdout.read(1)
                if not c:
                    return None
                hdr += c
            ln = int([l for l in hdr.split(b"\r\n")
                      if l.lower().startswith(b"content-length")][0].split(b":")[1])
            return json.loads(p3.stdout.read(ln))
        snd({"jsonrpc": "2.0", "id": 1, "method": "initialize",
             "params": {"capabilities": {}, "rootUri": "file:///" + d.replace("\\", "/")}})
        while True:
            m = rcv()
            if m is None or m.get("id") == 1:
                break
        snd({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        snd({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": doc, "languageId": "zx", "version": 1,
                             "text": body}}})
        import time as _t
        for rid, (method, off, tag) in enumerate(reqs, start=2):
            _line = body.count("\n", 0, off)
            _col = off - (body.rfind("\n", 0, off) + 1)
            if method == "completion":
                params = {"textDocument": {"uri": doc},
                          "position": {"line": _line, "character": _col}}
            else:
                params = {"textDocument": {"uri": doc},
                          "range": {"start": {"line": 0, "character": 0},
                                    "end": {"line": body.count("\n"), "character": 0}}}
            snd({"jsonrpc": "2.0", "id": rid, "method": "textDocument/" + method,
                 "params": params})
            deadline = _t.time() + 30
            while _t.time() < deadline:
                m = rcv()
                if m is None:
                    break
                if m.get("id") == rid:
                    r = m.get("result") or {}
                    if isinstance(r, list):
                        out[tag] = r
                    else:
                        out[tag] = r.get("items", [])
                    if os.environ.get("NESTDBG"):
                        print("DBG", rid, tag, json.dumps(out[tag])[:500], file=sys.stderr)
                    break
        return out
    finally:
        try:
            p3.kill()
        except Exception:
            pass
        shutil.rmtree(d, ignore_errors=True)

NEST_BODY = (
    'const Inner = struct {\n'
    '    x: i32,\n'
    '    y: i32,\n'
    '};\n\n'
    'const Person = struct(T: type, n: u32){\n'
    '    age: T,\n'
    '    name: []const u8,\n'
    '    data: Inner,\n'
    '    ptr: *T,\n'
    '};\n\n'
    'const take = fn (p: Person(i32, 4)) u8 {\n'
    '    return 0;\n'
    '};\n\n'
    'pub const main = fn () u8 {\n'
    '    var p = std.heapAlloc(std.heap.page, u8, 10);\n'
    '    var q = Person(i32, 4){ .age = 1, .data = .{ . } };\n'
    '    _ = take(.{ .age = 1, . } );\n'
    '    _ = take(.{ .data = .{ . } });\n'
    '    return 0;\n'
    '}\n')

_nb = NEST_BODY
_nres = multi_probe(_nb, [
    ("completion", _nb.index(".age = 1, .data = .{ .") + len(".age = 1, .data = .{ ."), "nestQ"),
    ("completion", _nb.index("take(.{ .age = 1, .") + len("take(.{ .age = 1, ."), "fnArgDirect"),
    ("completion", _nb.index("take(.{ .data = .{ .") + len("take(.{ .data = .{ ."), "fnArgNested"),
    ("inlayHint", 0, "inlay"),
])
_nq = {i.get("label") for i in _nres.get("nestQ") or []}
check("completion.nest.genericOuter.dataFields", {"x", "y"} <= _nq, str(sorted(_nq)))
_nf = {i.get("label") for i in _nres.get("fnArgDirect") or []}
check("completion.nest.fnArgNamedParam", {"name", "data", "ptr"} <= _nf, str(sorted(_nf)))
_nn = {i.get("label") for i in _nres.get("fnArgNested") or []}
check("completion.nest.fnArgNested", {"x", "y"} <= _nn, str(sorted(_nn)))
_hlabels = [h["label"] for h in (_nres.get("inlay") or []) if h.get("kind") == 1]
check("inlayHint.type.heapAlloc", ": *[10]u8" in _hlabels, str(_hlabels))

# §193 —— 函数别名（`const alloc = std.heapAlloc;`）的编辑器四件套：
# 调用骨架 / hover 签名 / signatureHelp 与真身同一口径，链式别名
#（hp → ha → alloc）沿别名链追到 std_builtin_docs。编译器语义已由
# tests/good/builtin-alias.zx 守门；这里守门 LSP 侧（用户 2026-11-20：
# 别名在编辑器里还是"变量"——没签名、没骨架、没参数提示）。独立进程探针。
def alias_probe():
    d = os.path.join(TESTDIR, "alias")
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    doc = "file:///" + os.path.join(d, "main.zx").replace("\\", "/")
    body = ('const std = @mod("std");\n'
            '\n'
            'const alloc = std.heapAlloc;\n'
            'const ha = alloc;\n'
            'const hp = ha;\n'
            '\n'
            'pub const main = fn () u8 {\n'
            '    const al = std.heap.page;\n'
            '    var s: *[1]i32 = hp(\n'
            '    var t = ha(al, i32, 2);\n'
            '    defer std.free(al, t);\n'
            '    defer std.free(al, s);\n'
            '    return 0;\n'
            '}\n')
    out = {"items": None, "hover": "", "sig": None}
    try:
        p4 = subprocess.Popen([ZXLS_BIN], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, cwd=d)
        snd = lambda o: (p4.stdin.write(
            ("Content-Length: %d\r\n\r\n" % len(json.dumps(o).encode())).encode()
            + json.dumps(o).encode()), p4.stdin.flush())
        def rcv():
            hdr = b""
            while not hdr.endswith(b"\r\n\r\n"):
                c = p4.stdout.read(1)
                if not c:
                    return None
                hdr += c
            ln = int([l for l in hdr.split(b"\r\n")
                      if l.lower().startswith(b"content-length")][0].split(b":")[1])
            return json.loads(p4.stdout.read(ln))
        snd({"jsonrpc": "2.0", "id": 1, "method": "initialize",
             "params": {"capabilities": {}, "rootUri": "file:///" + d.replace("\\", "/")}})
        while True:
            m = rcv()
            if m is None or m.get("id") == 1:
                break
        snd({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        snd({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": doc, "languageId": "zx", "version": 1,
                             "text": body}}})
        import time as _t

        def ask(rid, method, col, line=8):
            snd({"jsonrpc": "2.0", "id": rid, "method": method,
                 "params": {"textDocument": {"uri": doc},
                            "position": {"line": line, "character": col}}})
            deadline = _t.time() + 30
            while _t.time() < deadline:
                m = rcv()
                if m is None:
                    return None
                if m.get("id") == rid:
                    return m.get("result")

        r = ask(2, "textDocument/completion", 22)
        out["items"] = r.get("items", r) if isinstance(r, dict) else r
        r = ask(3, "textDocument/hover", 22)
        out["hover"] = ((((r or {}).get("contents") or {}).get("value")) or "")
        r = ask(4, "textDocument/signatureHelp", 24)
        out["sig"] = (r or {}).get("signatures")
        snd({"jsonrpc": "2.0", "id": 5, "method": "textDocument/inlayHint",
             "params": {"textDocument": {"uri": doc},
                        "range": {"start": {"line": 0, "character": 0},
                                  "end": {"line": 14, "character": 0}}}})
        out["inlay"] = ask(5, "textDocument/inlayHint", 0) or []
        # §193 补丁三：实参位降噪。line9 = `    var t = ha(al, i32, 2);`
        # col 18 → ap=1（T: type）→ 只出类型名；col 23 → ap=2（n: usize）
        # → 只出值名 + 字面量关键字；语句位（空行）关键字全量。
        r = ask(6, "textDocument/completion", 18, line=9)
        out["argty"] = r.get("items", r) if isinstance(r, dict) else r
        r = ask(7, "textDocument/completion", 23, line=9)
        out["argval"] = r.get("items", r) if isinstance(r, dict) else r
        r = ask(8, "textDocument/completion", 0, line=5)
        out["stmt"] = r.get("items", r) if isinstance(r, dict) else r
        snd({"jsonrpc": "2.0", "id": 9, "method": "shutdown", "params": None})
        snd({"jsonrpc": "2.0", "method": "exit", "params": None})
    finally:
        try:
            p4.kill()
        except Exception:
            pass
        shutil.rmtree(d, ignore_errors=True)
    return out

_ap = alias_probe()
_ait = _ap["items"] or []
_ahp = [i for i in _ait if i.get("label") == "hp(...)"]
check("completion.alias.callSnippet",
      bool(_ahp) and _ahp[0].get("kind") == 15
      and _ahp[0].get("insertText") == "hp(${1:alloc}, ${2:T}, ${3:n})$0",
      str(_ahp))
check("hover.alias.signature",
      "hp(alloc: Allocator, T: type, n: usize) *[n]T" in _ap["hover"],
      _ap["hover"][:200])
_asg = _ap["sig"] or []
check("signatureHelp.alias",
      bool(_asg) and _asg[0].get("label") == "hp(alloc: Allocator, T: type, n: usize) *[n]T"
      and len(_asg[0].get("parameters") or []) == 3,
      str(_asg)[:200])
_ahint = [h.get("label") for h in (_ap["inlay"] or []) if h.get("kind") == 1]
check("inlayHint.type.alias", ": *[2]i32" in _ahint, str(_ahint))

_aty = {i.get("label") for i in (_ap["argty"] or [])}
check("completion.argctx.types",
      "i32" in _aty and "al" not in _aty and "const" not in _aty, str(sorted(_aty))[:200])
_aval = {i.get("label") for i in (_ap["argval"] or [])}
check("completion.argctx.values",
      "al" in _aval and "null" in _aval
      and "i32" not in _aval and "const" not in _aval, str(sorted(_aval))[:200])
_astm = {i.get("label") for i in (_ap["stmt"] or [])}
check("completion.stmt.keywords",
      "const" in _astm and "al" in _astm, str(sorted(_astm))[:200])

# ---------------------------------------------------------------------------
if fails:
    print(f"SMOKE FAILED: {len(fails)} -> {fails}"); sys.exit(1)
print("SMOKE ALL PASS"); sys.exit(0)
