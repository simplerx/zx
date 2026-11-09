# zxls 冒烟测试：stdio LSP 会话模拟
import subprocess, json, os, sys, shutil, threading, queue, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # 仓库根（脚本在 tools/ 下）
TESTDIR = os.path.join(ROOT, ".tmp", "lsptest")
os.makedirs(TESTDIR, exist_ok=True)

# 被测的 zxls：测试脚本不假设 bin/ 里那份是最新的（Zed 可能正锁着它），
# 用 ZXLS_BIN 指到刚编出来的临时 exe。
ZXLS_BIN = os.environ.get("ZXLS_BIN") or os.path.join(ROOT, "bin", "zxls-new.exe")

MAIN = """const std = module("std");

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
    let g = std.task.Task.group();
    g.wait();
    let t = std.task.Task.single();
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
    let tp = .{ 1, 2.5 };
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
    'pub const Cfg = struct {\n    w: i32,\n    h: i32,\n};\n')
open(os.path.join(TESTDIR, "libs", "util.zx"), "w", encoding="utf-8").write(
    'pub const twice = fn (x: i32) i32 {\n    return x * 2;\n};\n')
EXTUSE = """const ext = module("ext");
const util = module("util");

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

# 3w2 嵌套匿名的行内类型提示 + 匿名元组形状（.{1, 2.5} 是 {i32,f64}）
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

# 3q2 自定义类型骨架：`type` / `uniontag` / `enumtag` / `new`
#     用户 2026-11-08 追加：写自定义类型也要能 Tab 展开完整声明。
#     展开后是能直接编译的 `const Name = struct { … };` 整条声明；
#     实例构造走 `new`。
SNIP2 = """const std = module("std");
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

# 3e std.task.Task 关联函数：std.task.Task. → 只有 coro/single/pool/fromId/group。
# 方法（首参 self）**不进类型命名空间**——用户 2026-11-10 第 2 张截图：
# "Task 空间上也是这些[方法]"，类型上列方法和实例重复，纯污染。签名里的
# 参数名 f/ctx/h/v 也不得混进来（extractBraceMembers 的圆括号深度守卫）。
off = MAIN.index("std.task.Task.") + len("std.task.Task.")
send({"jsonrpc": "2.0", "id": 12, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("completion.stdTask", 12)
labels = {i["label"] for i in r["result"]["items"]} if r and r.get("result") else set()
check("completion.stdTask",
      labels == {"coro", "single", "pool", "fromId", "group"}
      and not labels & {"ctx", "f", "h", "v"}, str(sorted(labels)))

# 3f 实例类型穿透：let g = std.task.Task.group() → g. 出 Group 的成员
# （spawn/wait/id），不是 Task 的（coro/single/pool 不得出现）
off = MAIN.index("g.wait();") + len("g.")
send({"jsonrpc": "2.0", "id": 13, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("completion.groupInst", 13)
labels = {i["label"] for i in r["result"]["items"]} if r and r.get("result") else set()
check("completion.groupInst", {"id", "spawn", "wait"} <= labels
      and not labels & {"coro", "single", "pool"}, str(sorted(labels)))

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
      tmap.get("spawn") == "spawn(self: *const Task, f: fn (usize) !usize, ctx: usize) ?usize"
      and tmap.get("awaitAll") == "awaitAll(self: *const Task) !void"
      and tmap.get("close") == "close(self: *const Task) void"
      # 关联函数（首参不是 self）一律不进实例补全
      and not ({"coro", "single", "pool", "fromId", "group"} & set(tmap)),
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
PARAM = """const std = module("std");
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
DEEP = """const std = module("std");
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
BIND = '''const g  = module("glue");
const gl = module("gl.zx");

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
      bool(imp) and imp[0].get("insertText") == 'const ${1:glue} = module("glue");$0',
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
# 已经写过绑定的名字不再提示导入
IBOUND = '''const glue = module("glue");

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
IALIAS = '''const g = module("glue");

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
    w: i32,
    h: i32,
};

pub const Outer = struct {
    inner: struct { a: i32, b: []const u8 },
    name: []const u8,
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
NESTUSE = """const a = module("nestlib");
const std = module("std");

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
check("completion.std.moduleDetail", sd.get("str") == 'module("str.zx")', str(sd.get("str")))
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
#  3 std 类型实体（Task/ArenaAllocator/泛型 ArrayList）成员有类型、方法有签名
#  4 std.zx 不设类型别名（用户第 4 条下午：别名和真身分不清）——
#    类型只走 std.task.Task / std.map.HashMap 这类真实路径
STDUSE = """const std = module("std");

pub const main = fn () u8 {
    let g = std.task.Task.single();
    g.await(1);
    let l = std.ArrayList(i32).init(std.heap.page);
    l.push(1);
    let t = std.task.Task{ .id = 0 };
    t.await(1);
    var m = std.map.HashMap(i32).init(std.heap.page);
    _ = m.put("a", 1);
    var dt = std.time.fromUnixMs(0);
    var sm = std.fs.stat(std.heap.page, "x");
    var r = std.BufReader.open(std.heap.page, "x", 64);
    var w = std.BufWriter.create(std.heap.page, "y", 64);
    var V: type = std.ArrayList(i32);
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
check("completion.stdInstance.staticSnippet",
      not ({"single", "coro", "pool", "fromId", "group"} & set(gk)) and "id" in gk,
      str(sorted(gk)))
check("completion.stdInstance.methodSig",
      (gd.get("await") or "") == "await(self: *const Task, h: usize) !usize"
      and gk.get("await") == 15 and gt.get("await") == "await(${1:h})$0",
      "%r %r" % (gd.get("await"), gk.get("await")))

# 3b 泛型容器实例：std.ArrayList(i32) → l. 出全部成员，字段带 T 类型
off = STDUSE.index("    l.push(1);") + len("    l.")
lit_ = std_items(off, 404)
lid = {i["label"]: i.get("detail") for i in lit_}
check("completion.genericInstance.members",
      lid.get("push", "") == "push(self: *ArrayList, v: T) bool"
      and lid.get("buf") == "*T" and lid.get("len") == "usize", str(lid)[:240])

# 3c 实例方法 hover（std 类型 + 泛型）
check("hover.stdInstance.method",
      std_hover("g.await(1);", "await", 505) == "```zx\nawait(self: *const Self, h: usize) !usize\n```",
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
# 5 泛型容器 hover 贴真实 struct 体（2026-11-09：std 全部类型都要有类型提示，
# 泛型不能只出一行 `X(T: type) type` 签名）
check("hover.generic.facadeDefinition",
      "struct {" in std_hover("std.map.HashMap(i32)", "HashMap", 511)
      and "vals: *V" in std_hover("std.map.HashMap(i32)", "HashMap", 511),
      std_hover("std.map.HashMap(i32)", "HashMap", 511)[:200])
check("hover.generic.topLevelDefinition",
      "struct {" in std_hover("std.ArrayList(i32)", "ArrayList", 512)
      and "buf: *T" in std_hover("std.ArrayList(i32)", "ArrayList", 512),
      std_hover("std.ArrayList(i32)", "ArrayList", 512)[:200])
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
      str(sorted(set(top_lab2)))[:240])
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
      "get" in mlab and "put" in mlab and (mlab.get("n") or "") == "usize", str(list(mlab))[:200])
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
#  6f 类型变量绑定：`var V: type = std.ArrayList(i32)` → `V.init(…)` 的实例
#     也要推断成 ArrayList（用户 2026-11-09 截图里的写法）
off = STDUSE.index("    list.\n") + len("    list.")
lit2 = std_items(off, 419)
lld = {i["label"]: i.get("detail") for i in lit2}
check("completion.typeVar.instance",
      lld.get("push") == "push(self: *ArrayList, v: T) bool" and lld.get("len") == "usize"
      and lld.get("items") == "items(self: *ArrayList) []T", str(lld)[:240])
#  6g 三段路径的泛型容器：`std.map.HashMap.` 一个成员都不出过
off = STDUSE.index("    std.map.HashMap.") + len("    std.map.HashMap.")
hit3 = std_items(off, 420)
h3 = {i["label"]: (i.get("detail"), i.get("insertText")) for i in hit3}
check("completion.typePath.genericMembers",
      # 三段路径的泛型容器：类型上只该有静态函数（init 与 norm 都不带
      # self），get/put/count 都是方法（首参 self），只在实例上出现。
      set(h3) == {"init", "norm"}
      and h3.get("init", (None, None))[1] == "init(${1:alloc})$0",
      str(sorted(h3))[:240])
#  6h union(enum) 的三段路径：Value. 出变体（构造骨架走 std.json. 类型项）
off = STDUSE.index("    std.json.Value.") + len("    std.json.Value.")
vit = std_items(off, 421)
vlab = {i["label"] for i in vit}
check("completion.typePath.unionVariants",
      {"nul", "int", "float", "str", "arr", "obj"} <= vlab, str(sorted(vlab)))
#  6i 三段路径的 Pool：类型上只有静态 init，实例方法（alloc/free/create/
#     deinit）不进类型命名空间（用户 2026-11-10 命名空间分离）。
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
cov_lines = ['const std = module("std");', "", "pub const main = fn () u8 {"]
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
CHAIN = """const std = module("std");

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
# 字段在实例上（`HS.keys`），类型上取字段是 E0300
cinst = {i["label"] for i in chain_items("HS.\n")}
check("completion.chain.genericInstField",
      {"keys", "vals", "cap", "n"} <= cinst, str(sorted(cinst))[:220])

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
STRM = """const std = module("std");

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
ET = """const std = module("std");
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
    const t = .{ 1, 2.5 };
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
PT = """const std = module("std");
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
            body = ("const std = module(\"std\");\n\n"
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

# ---------------------------------------------------------------------------
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
    body = ['const std = module("std");',
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

perf = inlay_perf()
if perf is None:
    print("skip inlayHint.perf (非 Windows)")
else:
    dt, nh, n = perf
    check(f"inlayHint.perf.{n}行", dt < 6.0 and nh >= n * 3,
          "%.2fs, %d 条提示（超时或提示数不足：多半又退化成 O(绑定数 × 文件长度)）" % (dt, nh))
    print("     inlayHint: %.3f s, %d 条提示, %d 行" % (dt, nh, n))

# ---------------------------------------------------------------------------
if fails:
    print(f"SMOKE FAILED: {len(fails)} -> {fails}"); sys.exit(1)
print("SMOKE ALL PASS"); sys.exit(0)
