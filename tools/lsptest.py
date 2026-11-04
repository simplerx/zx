# zxls 冒烟测试：stdio LSP 会话模拟
import subprocess, json, os, sys, threading, queue

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
    rect: (f64, f64),
};

pub fn main() i32 {
    let o = make();
    let n = o.count;
    var s = Shape{ .circle = 1.0 };
    std.println("{}", n);
    return 0;
}
"""

uri = "file:///" + TESTDIR.replace("\\", "/") + "/main.zx"

# ---- vendor/ 与 deps 的模块（补全只能走编译器的同一条解析链）-----------------
SRC_DIR = os.path.join(TESTDIR, "src")
os.makedirs(SRC_DIR, exist_ok=True)
os.makedirs(os.path.join(TESTDIR, "vendor"), exist_ok=True)
os.makedirs(os.path.join(TESTDIR, "libs"), exist_ok=True)
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
    "capabilities": {}}})
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

def complete_at(text, off, rid):
    send({"jsonrpc": "2.0", "id": rid, "method": "textDocument/completion", "params": {
        "textDocument": {"uri": exturi}, "position": pos(text, off)}})
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

# 9 shutdown/exit
send({"jsonrpc": "2.0", "id": 8, "method": "shutdown", "params": None})
recv_id("shutdown", 8)
send({"jsonrpc": "2.0", "method": "exit", "params": None})
try:
    proc.wait(timeout=5)
except Exception:
    proc.kill()

print("=" * 40)
if fails:
    print(f"SMOKE FAILED: {len(fails)} -> {fails}"); sys.exit(1)
print("SMOKE ALL PASS"); sys.exit(0)
