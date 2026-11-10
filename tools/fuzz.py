#!/usr/bin/env python3
# fuzz.py -- zxc compiler robustness fuzzer.
#
# What it does:
#   1. mutation fuzzing: take valid seeds (tests/good/*.zx, std/*.zx,
#      demo/*.zx), apply byte/line/splice mutations, feed to zxc.
#   2. pathological sources: deep nesting, huge structs, cyclic generics,
#      giant literals, bad encodings -- hand-written worst cases.
#
# PASS criterion per run: zxc exits cleanly (0 or 1, with diagnostics).
# CRASH = exit code >= 0x80000000 (access violation / stack overflow /
# abort) or a signal, or the process hangs past the timeout.
# A crash prints the mutant to <outdir>/crash-NNN.zx for reproduction.
#
# Usage:
#   python tools/fuzz.py [iterations] [outdir]
#   ZXC_BIN=/path/zxc python tools/fuzz.py 2000
#
# Entry points covered: plain check (fast path, most iterations),
# full build (-o, exercises backend + zig cc), --emit-c, --ast,
# --tokens, fmt. std/ is copied next to the fuzz dir so mutated
# imports still resolve.
import os
import random
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ZXC = os.environ.get("ZXC_BIN", os.path.join(ROOT, "bin", "zxc.exe"))
OUTDIR = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, ".tmp", "fuzz")
ITERS = int(sys.argv[1]) if len(sys.argv) > 1 else 600
TIMEOUT = 20          # seconds per compile
CRASH_BASE = 0x80000000

random.seed(20261110)

# ---- console: ASCII only (GBK console mangles UTF-8 dashes etc.) ----

def note(msg):
    print(msg, flush=True)


def load_seeds():
    seeds = []
    for sub in ("tests/good", "std", "demo"):
        d = os.path.join(ROOT, sub)
        for name in sorted(os.listdir(d)):
            p = os.path.join(d, name)
            if os.path.isfile(p) and (name.endswith(".zx") or sub == "std" and name.endswith(".zx")):
                try:
                    with open(p, "rb") as f:
                        data = f.read()
                    if 0 < len(data) < 400_000:
                        seeds.append((name, data))
                except OSError:
                    pass
    return seeds


PRINTABLE = (b"{}()[]<>;,:\"'`~!@#$%^&*-_=+\\|/? .\t\n0123456789"
             b"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ")
WEIRD = b"\x00\xff\xc3\xa9\xe4\xb8\xad\x80\xf0\x9f\x92\xa9\r\n\x7f"


def mutate(data, rng):
    kind = rng.randrange(8)
    b = bytearray(data)
    if kind == 0 and len(b) > 4:                      # random byte pokes
        for _ in range(rng.randrange(1, 12)):
            b[rng.randrange(len(b))] = rng.choice(PRINTABLE + WEIRD)
    elif kind == 1 and len(b) > 4:                    # delete a chunk
        i = rng.randrange(len(b) - 1)
        del b[i:i + rng.randrange(1, 400)]
    elif kind == 2:                                   # duplicate a chunk
        i = rng.randrange(max(1, len(b) - 2))
        chunk = b[i:i + rng.randrange(1, 800)]
        b[i:i] = chunk
    elif kind == 3 and len(b) > 8:                    # splice two halves
        i = rng.randrange(len(b))
        b[:i] = data[:i][::-1] if rng.randrange(2) else b[:i]
        del b[i:i + rng.randrange(1, 200)]
    elif kind == 4:                                   # junk insert
        i = rng.randrange(len(b) + 1)
        b[i:i] = bytes(rng.choice(PRINTABLE + WEIRD) for _ in range(rng.randrange(1, 60)))
    elif kind == 5 and len(b) > 4:                    # swap two regions
        i = rng.randrange(len(b) - 1)
        j = rng.randrange(len(b) - 1)
        b[i], b[j] = b[j], b[i]
    elif kind == 6:                                   # truncate
        del b[rng.randrange(len(b)):]
    else:                                             # line surgery
        lines = bytearray(b).split(b"\n")
        if len(lines) > 2:
            k = rng.randrange(len(lines))
            op = rng.randrange(3)
            if op == 0:
                del lines[k]
            elif op == 1:
                lines.insert(k, lines[rng.randrange(len(lines))])
            else:
                lines[k] = lines[k][:rng.randrange(len(lines[k]) + 1)]
        b = b"\n".join(lines)
    return bytes(b[:600_000])


def pathological():
    """Hand-written worst cases: (name, source bytes)."""
    cases = []
    w = cases.append
    w(("deep-parens", b"const main = fn () u8 { return " + b"(" * 4000 + b"1" + b")" * 4000 + b"; };"))
    w(("deep-blocks", b"const main = fn () u8 {" + b"{" * 2000 + b"}" * 2000 + b" return 0; };"))
    w(("deep-expr", b"const main = fn () u8 { return " + b"1+" * 20000 + b"1; };"))
    w(("deep-nest-struct", b"const S0 = struct { x: i32 };\n" +
       b"".join(b"const S%d = struct { x: S%d };" % (i, i - 1) for i in range(1, 300)) +
       b"\nconst main = fn () u8 { return 0; };"))
    w(("huge-struct", b"const Big = struct {\n" +
       b"".join(b"    f%d: i32,\n" % i for i in range(5000)) + b"};\n" +
       b"const main = fn () u8 { return 0; };"))
    w(("many-fns", b"".join(b"const f%d = fn (x: i32) i32 { return x + %d; };\n" % (i, i)
                            for i in range(3000)) + b"const main = fn () u8 { return 0; };"))
    w(("giant-string", b'const main = fn () u8 { std.println("%s"); return 0; };'
      % (b"A" * 500_000)))
    w(("long-ident", b"const " + b"a" * 60000 + b" = 1;\nconst main = fn () u8 { return 0; };"))
    w(("generic-cycle", b"const G = fn (T: type) type {\n"
        b"    return struct { v: G(T) };\n};\nconst main = fn () u8 { return 0; };"))
    w(("generic-self", b"const P = fn (T: type) type {\n"
        b"    return struct { inner: P(struct{}) };\n};\nconst main = fn () u8 { return 0; };"))
    w(("const-cycle", b"const a = b + 1;\nconst b = a + 1;\nconst main = fn () u8 { return 0; };"))
    w(("comptime-loop", b"const main = fn () u8 {\n"
        b"    comptime_var i = 0;\n    while (true) { i = i + 1; }\n"
        b"    return 0;\n};"))
    w(("huge-array", b"const main = fn () u8 {\n"
        b"    const a = [1000000]i32{ 1 };\n    return 0;\n};"))
    w(("bad-utf8", b"const main = fn () u8 { return 0; } // \xff\xfe\x80 bad"))
    w(("null-bytes", b"const main = fn () u8 { ret\x00urn 0; };"))
    w(("bom", b"\xef\xbb\xbfconst main = fn () u8 { return 0; };"))
    w(("crlf-mix", b"const main = fn () u8 {\r\n    return 0;\n};\r\n"))
    w(("empty", b""))
    w(("only-nul", b"\x00" * 4096))
    w(("deep-nested-generic",
       b"const Box = fn (T: type) type { return struct { v: T }; };\n"
       b"const main = fn () u8 {\n    var b = Box(Box(Box(Box(Box(i32))))){ .v = undefined };\n"
       b"    return 0;\n};"))
    w(("many-instantiations",
       b"const Box = fn (T: type) type { return struct { v: T, n: @type(T).size };\n};\n" +
       b"".join(b"const B%d = Box(i%d);\n" % (i, i + 1) for i in range(200)) +
       b"const main = fn () u8 { return 0; };"))
    return cases


def run(args, cwd):
    try:
        p = subprocess.run([ZXC] + args, cwd=cwd, capture_output=True,
                           timeout=TIMEOUT, stdin=subprocess.DEVNULL)
        return p.returncode, p.stdout[-4000:] + p.stderr[-4000:]
    except subprocess.TimeoutExpired:
        return "TIMEOUT", b""
    except OSError as e:
        return "OSERR", str(e).encode()


def is_crash(rc):
    if rc == "TIMEOUT" or rc == "OSERR":
        return True
    if rc < 0:            # signal (posix semantics)
        return True
    return rc >= CRASH_BASE


FULL_BUILD_EVERY = 25   # every Nth mutant goes through the full pipeline


def main():
    if not os.path.exists(ZXC):
        note("fuzz: zxc not found at %s" % ZXC)
        return 2
    os.makedirs(OUTDIR, exist_ok=True)
    fdir = os.path.join(OUTDIR, "work")
    shutil.rmtree(fdir, ignore_errors=True)
    os.makedirs(fdir)
    std_src = os.path.join(ROOT, "std")
    std_dst = os.path.join(fdir, "std")
    if os.path.isdir(std_src):
        shutil.copytree(std_src, std_dst,
                        ignore=shutil.ignore_patterns("*.o", "*.c", "*.exe"))

    seeds = load_seeds()
    if not seeds:
        note("fuzz: no seeds found")
        return 2
    note("fuzz: %d seeds, %d iterations, zxc=%s" % (len(seeds), ITERS, ZXC))

    crashes = 0
    runs = 0
    t0 = time.time()

    def trial(name, src, extra):
        nonlocal crashes, runs
        path = os.path.join(fdir, name)
        with open(path, "wb") as f:
            f.write(src)
        rc, out = run(extra + [name], fdir)
        runs += 1
        if is_crash(rc):
            crashes += 1
            keep = os.path.join(OUTDIR, "crash-%03d.zx" % crashes)
            shutil.copyfile(path, keep)
            note("CRASH rc=%s args=%s -> %s" % (rc, extra, keep))
            if out:
                note("  tail: %r" % out[-300:])
        return rc

    # 1. pathological inputs: full pipeline on a few, check-only on the rest
    note("== pathological cases ==")
    for name, src in pathological():
        extra = ["-o", "p.exe"] if name in ("deep-parens", "huge-struct", "many-fns") else []
        # single fixed filename, overwritten each time: avoids mass-delete
        # hooks and keeps the work dir tiny
        trial("cur.zx", src, extra)

    # 2. mutation fuzzing
    for i in range(ITERS):
        seed_name, data = seeds[random.randrange(len(seeds))]
        src = mutate(data, random)
        if i % FULL_BUILD_EVERY == 0:
            extra = ["-o", "m.exe"]           # lex->parse->sema->backend->zig cc
        elif i % 5 == 1:
            extra = ["--emit-c"]
        elif i % 5 == 2:
            extra = ["--ast"]
        elif i % 5 == 3:
            extra = ["--tokens"]
        elif i % 5 == 4:
            extra = ["fmt", "--check"]
        else:
            extra = []                         # plain check (fast path)
        trial("cur.zx", src, extra)
        if (i + 1) % 100 == 0:
            el = time.time() - t0
            note("  %d/%d  %.1fs  crashes=%d" % (i + 1, ITERS, el, crashes))

    # 3. CLI edge cases
    note("== cli edges ==")
    for args in ([], ["nope.zx"], ["."], ["std"], ["--check"], ["-o"],
                 ["mut-missing.zx", "-o", "x.exe"]):
        try:
            p = subprocess.run([ZXC] + args, cwd=fdir, capture_output=True,
                               timeout=TIMEOUT, stdin=subprocess.DEVNULL)
            if is_crash(p.returncode):
                crashes += 1
                note("CRASH rc=%s args=%s" % (p.returncode, args))
        except subprocess.TimeoutExpired:
            crashes += 1
            note("CRASH TIMEOUT args=%s" % (args,))

    shutil.rmtree(fdir, ignore_errors=True)
    note("== done: %d runs, %d crash(es), %.1fs ==" % (runs, crashes, time.time() - t0))
    return 1 if crashes else 0


if __name__ == "__main__":
    sys.exit(main())
