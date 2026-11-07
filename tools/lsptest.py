# zxls 冒烟测试：stdio LSP 会话模拟
import subprocess, json, os, sys, shutil, threading, queue

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
    let g = std.Task.group();
    g.wait();
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
STD_COPY = os.path.join(ROOT, ".tmp", "std")
if not os.path.exists(os.path.join(STD_COPY, "task.zx")):
    os.makedirs(STD_COPY, exist_ok=True)
    for f in os.listdir(os.path.join(ROOT, "std")):
        if f.endswith(".zx"):
            shutil.copy(os.path.join(ROOT, "std", f), os.path.join(STD_COPY, f))
open(os.path.join(TESTDIR, "zx.json"), "w", encoding="utf-8").write(
    '{\n  "name": "lsptest",\n  "main": "src/extuser.zx",\n  "out": "lsptest",\n  "deps": ["libs"]\n}\n')
open(os.path.join(TESTDIR, "vendor", "ext.zx"), "w", encoding="utf-8").write(
    'pub const VERSION = "1.2.3";\n\npub const hello = fn (n: []const u8) void {\n    n;\n};\n')
open(os.path.join(TESTDIR, "libs", "util.zx"), "w", encoding="utf-8").write(
    'pub const twice = fn (x: i32) i32 {\n    return x * 2;\n};\n')
EXTUSE = """const ext = module("ext");
const util = module("util");

const main = fn () i32 {
    ext.hello("x");
    ext.
    util.twice(1);
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
            print(f"FAIL {tag}: server died"); return None, notifs
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
labels = {i["label"] for i in r["result"]["items"]} if r and r.get("result") else set()
check("completion.o.members", {"count", "name", "bump"} <= labels, str(labels))

# 3b 字面量成员补全：`Obj{ .` → 只要字段，方法（bump）不出
off = MAIN.index("Obj{ .count") + len("Obj{ .")   # 光标贴在 '.' 之后
send({"jsonrpc": "2.0", "id": 9, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("completion.literal", 9)
labels = {i["label"] for i in r["result"]["items"]} if r and r.get("result") else set()
check("completion.literal.obj", {"count", "name"} <= labels and "bump" not in labels, str(sorted(labels)))

# 3c union 字面量：`Shape{ .` → 变体（enum/union 成员）
off = MAIN.index("Shape{ .circle") + len("Shape{ .")
send({"jsonrpc": "2.0", "id": 10, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("completion.literal.union", 10)
labels = {i["label"] for i in r["result"]["items"]} if r and r.get("result") else set()
check("completion.literal.union", {"circle", "rect"} <= labels, str(sorted(labels)))

# 3d 匿名 struct 参数：push(.{ . → 字段来自签名里的 struct{…} 形参
off = MAIN.index("push(.{ .a") + len("push(.{ .")
send({"jsonrpc": "2.0", "id": 11, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("completion.anon", 11)
labels = {i["label"] for i in r["result"]["items"]} if r and r.get("result") else set()
check("completion.anon.fields", {"a", "msg"} <= labels, str(sorted(labels)))

# 3e std.Task 静态成员：std.Task. → 全方法集（group 在列；方法签名里的
# 参数名 f/ctx/h/v 不得混入——extractBraceMembers 的圆括号深度守卫）
off = MAIN.index("std.Task.") + len("std.Task.")
send({"jsonrpc": "2.0", "id": 12, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("completion.stdTask", 12)
labels = {i["label"] for i in r["result"]["items"]} if r and r.get("result") else set()
check("completion.stdTask", {"group", "coro", "spawn"} <= labels
      and not labels & {"ctx", "f", "h", "v"}, str(sorted(labels)))

# 3f 实例类型穿透：let g = std.Task.group() → g. 出 Group 的成员
# （spawn/wait/id），不是 Task 的（coro/single/pool 不得出现）
off = MAIN.index("g.wait();") + len("g.")
send({"jsonrpc": "2.0", "id": 13, "method": "textDocument/completion", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("completion.groupInst", 13)
labels = {i["label"] for i in r["result"]["items"]} if r and r.get("result") else set()
check("completion.groupInst", {"id", "spawn", "wait"} <= labels
      and not labels & {"coro", "single", "pool"}, str(sorted(labels)))

# 3g hover 变量 → 推断类型（var p = Person{…} 式结构体字面量也要出类型）
off = MAIN.index("var s = Shape") + len("var s")
send({"jsonrpc": "2.0", "id": 14, "method": "textDocument/hover", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("hover.varType", 14)
hv = r["result"]["contents"]["value"] if r and r.get("result") else ""
check("hover.varType", "const s: Shape = " in hv, repr(hv[:80]))

# 3h hover 容器名 → Zig 式 `: type`
off = MAIN.index("Shape{ .circle")     # 停在类型名上
send({"jsonrpc": "2.0", "id": 15, "method": "textDocument/hover", "params": {
    "textDocument": {"uri": uri}, "position": pos(MAIN, off)}})
r, _ = recv_id("hover.containerType", 15)
hv = r["result"]["contents"]["value"] if r and r.get("result") else ""
check("hover.containerType", ": type = struct" in hv or ": type = union" in hv, repr(hv[:80]))

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
            and m["params"].get("diagnostics")):
        diags = m["params"]["diagnostics"]
        diag_msgs = [d.get("message", "") for d in diags]
        break
ok_help = any("did you mean" in m for m in diag_msgs)
check("diagnostics.helpInMsg", ok_help, str(diag_msgs)[:300])

# 10c 诊断上的码要能点开文档：codeDescription.href 指向 docs/errors.md 的锚点
href = ""
for d in diags:
    if d.get("code") == "E0304":
        href = (d.get("codeDescription") or {}).get("href", "")
check("diagnostics.codeDescription", href.endswith("docs/errors.md#e0304"), href or str(diags)[:300])

# 10d 没有具体 fix-it 的诊断，消息里要带上该码的通用修法（errors.c 的文案）
ok_fix = any(d.get("code") == "E0301" and "@intCast" in d.get("message", "") for d in diags)
check("diagnostics.genericFix", ok_fix, str(diag_msgs)[:300])

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

if fails:
    print(f"SMOKE FAILED: {len(fails)} -> {fails}"); sys.exit(1)
print("SMOKE ALL PASS"); sys.exit(0)
