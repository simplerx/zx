import json, os, queue, shutil, subprocess, threading, time

ROOT = r"E:\workbuddy\zx"
BIN = os.path.join(ROOT, ".tmp", "zxlsnew", "zxls.exe")
DIR = os.path.join(ROOT, ".tmp", "slprobe")
SRC = os.path.join(DIR, "src")
os.makedirs(SRC, exist_ok=True)
shutil.rmtree(os.path.join(DIR, "std"), ignore_errors=True)
shutil.copytree(os.path.join(ROOT, "std"), os.path.join(DIR, "std"))
open(os.path.join(DIR, "zx.json"), "w", encoding="utf-8").write(
    '{"name":"slprobe","main":"src/main.zx","out":"slprobe"}\n')

TEXT = '''const std = module("std");

const main = fn () i32 {
    var a: *const u8 = "hello".;
    var b: []const u8 = "hello";
    b.;
    var c = "hello".len;
    var d = "hello".ptr;
    std.;
    std.debug.;
    return 0;
};
'''
path = os.path.join(SRC, "main.zx")
open(path, "w", encoding="utf-8", newline="\n").write(TEXT)
uri = "file:///" + path.replace("\\", "/")

proc = subprocess.Popen([BIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=subprocess.DEVNULL, cwd=DIR)
q = queue.Queue()

def reader():
    while True:
        hdr = b""
        while not hdr.endswith(b"\r\n\r\n"):
            c = proc.stdout.read(1)
            if not c:
                q.put(None); return
            hdr += c
        n = int([l for l in hdr.decode().split("\r\n")
                 if l.lower().startswith("content-length")][0].split(":")[1])
        body = b""
        while len(body) < n:
            body += proc.stdout.read(n - len(body))
        q.put(json.loads(body))

threading.Thread(target=reader, daemon=True).start()

def send(o):
    dd = json.dumps(o).encode()
    proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(dd) + dd); proc.stdin.flush()

def wait_id(want, timeout=25):
    end = time.time() + timeout
    while time.time() < end:
        try:
            m = q.get(timeout=1)
        except queue.Empty:
            continue
        if m is None:
            print("server died"); return None
        if "id" not in m:
            continue
        if m["id"] == want:
            return m
    print("timeout", want); return None

def pos(off):
    line = TEXT.count("\n", 0, off)
    ls = TEXT.rfind("\n", 0, off) + 1
    return {"line": line, "character": off - ls}

send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
    "rootUri": "file:///" + DIR.replace("\\", "/"), "capabilities": {}}})
wait_id(1)
send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
    "textDocument": {"uri": uri, "languageId": "zx", "version": 1, "text": TEXT}}})
time.sleep(1.5)

def complete(tag, off):
    rid = 100 + off
    send({"jsonrpc": "2.0", "id": rid, "method": "textDocument/completion",
          "params": {"textDocument": {"uri": uri}, "position": pos(off)}})
    m = wait_id(rid)
    items = []
    if m and isinstance(m.get("result"), dict):
        items = m["result"].get("items") or []
    elif m and isinstance(m.get("result"), list):
        items = m["result"]
    print(f"\n=== {tag}  (n={len(items)})")
    for it in items:
        print(f"  {it.get('label'):<24} kind={it.get('kind')} fmt={it.get('insertTextFormat')} "
              f"det={str(it.get('detail'))[:56]!r}")
    return items

def hover(tag, off):
    rid = 500 + off
    send({"jsonrpc": "2.0", "id": rid, "method": "textDocument/hover",
          "params": {"textDocument": {"uri": uri}, "position": pos(off)}})
    m = wait_id(rid)
    r = (m or {}).get("result")
    txt = ""
    if isinstance(r, dict):
        c = r.get("contents")
        txt = c.get("value", "") if isinstance(c, dict) else str(c)
    print(f"\n=== hover {tag}: {txt[:200]!r}")

complete('string literal "hello".', TEXT.index('"hello".') + len('"hello".'))
complete('slice var b.', TEXT.index("b.;") + 2)
complete('std.', TEXT.index("    std.;") + 8)
complete('std.debug.', TEXT.index("std.debug.;") + 10)

hover('"hello" literal', TEXT.index('var b: []const u8 = "hello"') + 24)
hover('"hello".len member', TEXT.index('"hello".len') + len('"hello".le'))
proc.kill()
