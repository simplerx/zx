# 探针：hover 类型提示专项（const i = 10 / 函数参数 / 字段 / 成员表达式）
import subprocess, json, os, sys, threading, queue

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TESTDIR = os.path.join(ROOT, ".tmp", "hoverprobe")
os.makedirs(TESTDIR, exist_ok=True)
ZXLS_BIN = os.environ.get("ZXLS_BIN") or os.path.join(ROOT, "bin", "zxls-new.exe")

SRC = """const std = module("std");

const Person = struct {
    name: []const u8,
    age: i32,

    const grow = fn (self: *Self, years: i32) i32 {
        self.age = self.age + years;
        return self.age;
    };
};

const ten = 10;
const half = 3.5;
const flag = true;
const label = "hi";

const halve = fn (x: f64) f64 {
    return x / 2;
};

pub const add = fn (a: i32, b: i32) i32 {
    return a + b;
};

pub const main = fn () i32 {
    const i = 10;
    const h = halve(half);
    const p = Person{ .name = "zx", .age = 1 };
    const n = p.age;
    const s = p.name;
    std.println("{}", n);
    return 0;
};
"""

uri = "file:///" + TESTDIR.replace("\\", "/") + "/main.zx"
open(os.path.join(TESTDIR, "main.zx"), "w", encoding="utf-8").write(SRC)

proc = subprocess.Popen([ZXLS_BIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
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
            out_q.put(json.loads(body.decode("utf-8")))
        except Exception as e:
            out_q.put({"__badframe__": True, "err": str(e)})

threading.Thread(target=reader, daemon=True).start()

def send(msg):
    data = json.dumps(msg).encode()
    proc.stdin.write(f"Content-Length: {len(data)}\r\n\r\n".encode() + data)
    proc.stdin.flush()

def recv_id(want_id, timeout=15):
    import time as _t
    deadline = _t.time() + timeout
    while _t.time() < deadline:
        try:
            m = out_q.get(timeout=1)
        except queue.Empty:
            continue
        if m is None:
            return None
        if m.get("__badframe__"):
            print("bad frame", m["err"]); return None
        if "id" in m and m["id"] == want_id:
            return m
    return None

def pos(text, off):
    line = text.count("\n", 0, off)
    ls = text.rfind("\n", 0, off) + 1
    return {"line": line, "character": off - ls}

send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"rootUri": "file:///" + TESTDIR.replace("\\", "/"), "capabilities": {}}})
recv_id(1)
send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": uri, "languageId": "zx", "version": 1, "text": SRC}}})

_id = 100
def hover(tag, needle, delta):
    """hover：光标落在 needle 内的 delta 偏移上"""
    global _id
    _id += 1
    off = SRC.index(needle) + delta
    send({"jsonrpc": "2.0", "id": _id, "method": "textDocument/hover",
          "params": {"textDocument": {"uri": uri}, "position": pos(SRC, off)}})
    r = recv_id(_id)
    hv = r["result"]["contents"]["value"] if r and r.get("result") else "(null)"
    print(f"[{tag}] {hv.strip()!r}")

# 用户核心诉求
hover("local.i=10   ", "const i = 10;", len("const i"))
hover("top.ten=10    ", "const ten = 10;", len("const ten"))
hover("top.half=3.5  ", "const half = 3.5;", len("const half"))
hover("top.flag      ", "const flag = true;", len("const flag"))
hover("top.label     ", 'const label = "hi";', len("const label"))
hover("param.add.a   ", "(a: i32, b: i32)", len("("))          # 形参 a
hover("param.add.b   ", "(a: i32, b: i32)", len("(a: i32, "))   # 形参 b
hover("param.grow.yrs", "self: *Self, years: i32", len("self: *Self, "))  # 第二参
hover("field.age     ", "    age: i32,", len("    "))             # 结构体字段声明
hover("field.name    ", "    name: []const u8,", len("    "))
hover("rhs.p.age     ", "const n = p.age;", len("const n = "))    # 成员表达式右值
hover("rhs.p.name    ", "const s = p.name;", len("const s = "))
hover("call.halve    ", "const h = halve(half);", len("const h = "))
hover("self.in.grow  ", "self.age = self.age + years;", 0)        # 方法里的 self
hover("param.use.yrs ", "self.age = self.age + years;", SRC.index("self.age = self.age + years;", SRC.index("self.age = self.age + years;")) - SRC.index("self.age = self.age + years;") + len("self.age = self.age + "))  # years 使用点
proc.kill()
