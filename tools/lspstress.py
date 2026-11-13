#!/usr/bin/env python3
# lspstress.py -- zxls language-server robustness stress test.
#
# Scenarios (fresh server process per group):
#   A. raw-frame fuzz: garbage bytes, broken headers, truncated/huge
#      Content-Length, invalid JSON, binary junk -- server must not crash.
#   B. missing-field abuse: valid JSON with missing/empty/wrong-typed
#      params, requests before initialize, unknown methods, duplicate ids.
#   C. load: rapid didChange storm (1500 revisions), 2MB document,
#      document with control bytes -- then hover/completion must still
#      answer and diagnostics must still flow.
#
# PASS = the server survives every group AND, after the abuse, still
# answers a valid initialize -> didOpen -> hover round trip, and shuts
# down cleanly (shutdown -> exit -> process exit code 0).
#
# Usage: ZXLS_BIN=/path/zxls.exe python tools/lspstress.py
import json
import os
import queue
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ZXLS = os.environ.get("ZXLS_BIN") or os.path.join(ROOT, "bin", "zxls.exe")
TESTDIR = os.path.join(ROOT, ".tmp", "lspstress")
os.makedirs(TESTDIR, exist_ok=True)

GOOD_DOC = """const std = @mod("std");

const Obj = struct {
    count: i32,
    name: []const u8,
    const bump = fn (self: *Self) i32 {
        self.count = self.count + 1;
        return self.count;
    };
};

pub const main = fn () i32 {
    let o = Obj{ .count = 0, .name = "x" };
    std.println("{}", o.count);
    return 0;
};
"""

fails = []


def note(msg):
    print(msg, flush=True)


class Client:
    def __init__(self):
        self.proc = subprocess.Popen(
            [ZXLS], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        self.out_q = queue.Queue()
        self.err_tail = []
        threading.Thread(target=self._reader, daemon=True).start()
        threading.Thread(target=self._err_reader, daemon=True).start()

    def _reader(self):
        f = self.proc.stdout
        while True:
            headers = {}
            line = b""
            while True:
                c = f.read(1)
                if not c:
                    self.out_q.put(None)
                    return
                line += c
                if line.endswith(b"\r\n"):
                    if line.strip() == b"":
                        break
                    k, _, v = line.decode("latin1").partition(":")
                    headers[k.strip().lower()] = v.strip()
                    line = b""
            try:
                n = int(headers["content-length"])
            except (KeyError, ValueError):
                self.out_q.put({"__badframe__": "bad headers %r" % headers})
                return
            if n < 0 or n > 64 << 20:
                self.out_q.put({"__badframe__": "absurd length %d" % n})
                return
            body = b""
            while len(body) < n:
                chunk = f.read(n - len(body))
                if not chunk:
                    self.out_q.put(None)
                    return
                body += chunk
            try:
                self.out_q.put(json.loads(body))
            except Exception as e:
                self.out_q.put({"__badframe__": str(e)})

    def _err_reader(self):
        for raw in self.proc.stderr:
            self.err_tail.append(raw.decode("utf-8", "replace").rstrip())
            del self.err_tail[:-25]

    def send_raw(self, data):
        try:
            self.proc.stdin.write(data)
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError):
            pass

    def send(self, msg):
        d = json.dumps(msg).encode()
        self.send_raw(b"Content-Length: %d\r\n\r\n" % len(d) + d)

    def recv(self, timeout=30):
        """next parsed message, or None on death/timeout/bad frame."""
        try:
            m = self.out_q.get(timeout=timeout)
        except queue.Empty:
            return None
        if m is None or (isinstance(m, dict) and m.get("__badframe__")):
            if m is not None:
                note("  bad frame: %s" % m["__badframe__"])
            return None
        return m

    def rpc(self, ident, method, params, timeout=30):
        self.send({"jsonrpc": "2.0", "id": ident, "method": method,
                   "params": params})
        deadline = time.time() + timeout
        while time.time() < deadline:
            m = self.recv(timeout=1)
            if m is None:
                return None
            if m.get("id") == ident:
                return m
        return None

    def wait_diag(self, uri=None, timeout=30):
        """wait for the next publishDiagnostics (optionally for one uri).

        queue timeouts are normal (debounce + a spawned zxc --check can
        take a while under load); only death or a bad frame fails early."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                m = self.out_q.get(timeout=1)
            except queue.Empty:
                continue
            if m is None:
                return False
            if isinstance(m, dict) and m.get("__badframe__"):
                return False
            if m.get("method") == "textDocument/publishDiagnostics":
                if uri is None or m.get("params", {}).get("uri", "").endswith(
                        uri.rsplit("/", 1)[-1]):
                    return True
        return False

    def alive(self):
        return self.proc.poll() is None

    def kill(self):
        try:
            self.proc.kill()
        except OSError:
            pass


def uri_of(path):
    return "file:///" + path.replace("\\", "/")


def report(group, ok, why=""):
    tag = "ok  " if ok else "FAIL"
    note("  %s %s%s" % (tag, group, (" -- " + why) if why and not ok else ""))
    if not ok:
        fails.append(group)


def scenario_raw_frames():
    """A. protocol garbage -- anything but a crash is acceptable."""
    cl = Client()
    junk = [
        b"hello there\r\n\r\n",
        b"Content-Length: 5\r\n\r\n{}",
        b"Content-Length: abc\r\n\r\n{}",
        b"Content-Length:\r\n\r\n",
        b"Content-Length: 99999999999999999999\r\n\r\n{}",
        b"Content-Length: 100\r\n\r\n{\"short\":1}",          # truncated body
        b"Content-Length: 2\r\n\r\n" + b"x" * 200,            # overlong body
        b"\x00\x01\x02\xff" * 64,
        b"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":nop}",
        b"[1,2,3]",
        b"Content-length: 2\r\nX-Other: y\r\n\r\n{}",         # lowercase key
        b"Content-Length: 0\r\n\r\n",
        b"Content-Length: 2\r\n\r\nnull",
    ]
    for payload in junk:
        cl.send_raw(payload)
    # invalid JSON with a correct frame header
    for body in (b"{", b"{bad}", b"\"str\"", b"123", b"\xff\xfe", b""):
        d = body
        cl.send_raw(b"Content-Length: %d\r\n\r\n" % len(d) + d)
    time.sleep(0.5)
    report("raw-frames.alive", cl.alive(),
           "server died on protocol garbage; stderr: %r" % cl.err_tail[-5:])
    # the server may have desynced its stream parser -- that is fine as
    # long as it lives; kill and move on
    cl.kill()


def scenario_bad_params():
    """B. well-formed frames, abusive payloads."""
    cl = Client()
    # requests before initialize
    m = cl.rpc(1, "textDocument/hover",
               {"textDocument": {"uri": "file:///x.zx"},
                "position": {"line": 0, "character": 0}}, timeout=10)
    report("pre-init.no-crash", cl.alive() and (m is None or "error" in m or "result" in m),
           "stderr: %r" % cl.err_tail[-5:])
    # initialize with garbage params
    for params in (None, {}, {"capabilities": "not-an-object"},
                   {"capabilities": {}, "rootUri": 42}):
        cl.send({"jsonrpc": "2.0", "id": 2, "method": "initialize",
                 "params": params})
        deadline = time.time() + 10
        while time.time() < deadline:
            m = cl.recv(timeout=1)
            if m is None or m.get("id") == 2:
                break
    report("init.garbage-params", cl.alive(),
           "died; stderr: %r" % cl.err_tail[-5:])
    # unknown methods, notifications with missing fields, duplicate ids
    cl.send({"jsonrpc": "2.0", "id": 3, "method": "textDocument/doesNotExist",
             "params": {}})
    cl.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {}})
    cl.send({"jsonrpc": "2.0", "method": "$/unknown", "params": None})
    cl.send({"jsonrpc": "2.0", "id": 4, "method": "textDocument/hover"})   # no params
    cl.send({"jsonrpc": "2.0", "id": 4, "method": "textDocument/hover",
             "params": {"textDocument": {"uri": "file:///nope.zx"},
                        "position": {"line": 999999, "character": 999999}}})
    time.sleep(0.5)
    report("abuse.unknown-methods", cl.alive(),
           "died; stderr: %r" % cl.err_tail[-5:])
    cl.kill()


def scenario_load():
    """C. volume: change storm, huge doc, control bytes."""
    cl = Client()
    path = os.path.join(TESTDIR, "storm.zx")
    with open(path, "w", encoding="utf-8") as f:
        f.write(GOOD_DOC)
    uri = uri_of(path)
    m = cl.rpc(1, "initialize", {"capabilities": {}, "rootUri": uri})
    if m is None:
        report("load.initialize", False, "no initialize reply")
        cl.kill()
        return
    cl.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
    cl.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
        "textDocument": {"uri": uri, "languageId": "zx", "version": 1,
                         "text": GOOD_DOC}}})
    report("load.didOpen-diagnostics", cl.wait_diag(uri=uri, timeout=60),
           "no publishDiagnostics after didOpen")

    # 1500 didChange revisions back to back (full-text sync)
    for i in range(1500):
        txt = GOOD_DOC.replace("o.count", "o.count + %d" % (i % 7)) if i % 2 \
            else GOOD_DOC
        cl.send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
            "textDocument": {"uri": uri, "version": 2 + i},
            "contentChanges": [{"text": txt}]}})
    report("load.change-storm.alive", cl.alive(),
           "died mid-storm; stderr: %r" % cl.err_tail[-5:])
    m = cl.rpc(7, "textDocument/hover",
               {"textDocument": {"uri": uri},
                "position": {"line": 15, "character": 20}}, timeout=30)
    report("load.change-storm.hover", cl.alive() and m is not None,
           "no hover reply after storm")

    # 2 MB document
    big = ['const std = @mod("std");']
    for i in range(30000):
        big.append("const v%d: i32 = %d;" % (i, i))
    big_text = "\n".join(big)
    path2 = os.path.join(TESTDIR, "big.zx")
    with open(path2, "w", encoding="utf-8") as f:
        f.write(big_text)
    uri2 = uri_of(path2)
    t0 = time.time()
    cl.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
        "textDocument": {"uri": uri2, "languageId": "zx", "version": 1,
                         "text": big_text}}})
    ok_diag = cl.wait_diag(uri=uri2, timeout=60)
    dt = time.time() - t0
    report("load.2MB-doc", ok_diag and cl.alive(),
           "%.1fs, diagnostics=%s" % (dt, ok_diag))
    note("       (2MB doc: %.2fs to diagnostics)" % dt)

    # control bytes / lone surrogates in text
    path3 = os.path.join(TESTDIR, "ctrl.zx")
    weird = 'const s = "a\\x01\\x02b";\nconst main = fn () u8 { return 0; };\n'
    with open(path3, "w", encoding="utf-8") as f:
        f.write(weird)
    uri3 = uri_of(path3)
    cl.send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
        "textDocument": {"uri": uri3, "languageId": "zx", "version": 1,
                         "text": weird}}})
    cl.wait_diag(uri=uri3, timeout=20)
    report("load.control-bytes", cl.alive(),
           "died; stderr: %r" % cl.err_tail[-5:])

    # shutdown handshake must still work after all of that
    m = cl.rpc(9, "shutdown", None, timeout=10)
    report("shutdown.after-abuse", m is not None, "no shutdown reply")
    cl.send({"jsonrpc": "2.0", "method": "exit"})
    try:
        rc = cl.proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        rc = None
        cl.kill()
    report("exit.clean", rc == 0, "exit code %r" % rc)


def main():
    if not os.path.exists(ZXLS):
        note("lspstress: zxls not found at %s" % ZXLS)
        return 2
    note("lspstress: target=%s" % ZXLS)
    t0 = time.time()
    note("== A. raw frame fuzz ==")
    scenario_raw_frames()
    note("== B. abusive payloads ==")
    scenario_bad_params()
    note("== C. load ==")
    scenario_load()
    note("== done in %.1fs: %d failure(s) ==" % (time.time() - t0, len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
