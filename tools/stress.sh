#!/bin/sh
# tools/stress.sh -- full brute-force stress run.
#
# Stages:
#   regress     3 full test.sh rounds: plain / ZXC_NO_AST_CACHE=1 / ZX_CHECKED=1
#   fuzz        tools/fuzz.py mutation + pathological sources (default 3000)
#   lsp         tools/lspstress.py protocol abuse + document storm
#   numeric     600k random simd samples (3 seeds) + extreme-value build
#   concurrent  16 simultaneous builds of one file + 8 mixed files
#
# Usage:
#   sh tools/stress.sh                 all stages
#   sh tools/stress.sh regress [plain|nocache|checked]   (default: all three)
#   sh tools/stress.sh fuzz [N] | lsp | numeric | concurrent
# Exit code != 0 means at least one stage failed.
#
# Console output is ASCII only (GBK terminals mangle box drawing).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
ZXC=./bin/zxc.exe
LOG="$ROOT/.tmp/stress"
mkdir -p "$LOG"

# Same reason as test.sh: keep zig's caches inside the repo. More importantly
# here, 16+ concurrent zxc processes hammering the *default* global cache
# (%LOCALAPPDATA%\zig) deadlock on its lock file -- one backend compile sits
# at "compiling N units..." until the timeout, while the rest queue behind it.
# A repo-local shared cache handles 16 simultaneous builds in ~10s.
ROOTW="$(cd "$ROOT" && pwd -W 2>/dev/null || echo "$ROOT")"
export ZIG_LOCAL_CACHE_DIR="$ROOTW/.zc-cache/local"
export ZIG_GLOBAL_CACHE_DIR="$ROOTW/.zc-cache/global"
mkdir -p "$ZIG_LOCAL_CACHE_DIR" "$ZIG_GLOBAL_CACHE_DIR"

FAIL=0
FAILED=""

note() { printf '%s\n' "$*"; }
ok()   { printf '  PASS  %s\n' "$*"; }
bad()  { printf '  FAIL  %s\n' "$*"; FAIL=$((FAIL + 1)); FAILED="$FAILED $1"; }

# ---- stages ---------------------------------------------------------------

stage_regress() {
    modes="${1:-plain nocache checked}"
    note "== regress: $modes =="
    for mode in $modes; do
        log="$LOG/regress-$mode.log"
        t0=$(date +%s)
        case "$mode" in
            nocache) ZXC_NO_AST_CACHE=1 sh test.sh > "$log" 2>&1; rc=$? ;;
            checked) ZX_CHECKED=1 sh test.sh > "$log" 2>&1; rc=$? ;;
            *)       sh test.sh > "$log" 2>&1; rc=$? ;;
        esac
        dt=$(( $(date +%s) - t0 ))
        if [ "$rc" -eq 0 ] && grep -q 'all checks passed' "$log"; then
            ok "regress/$mode (${dt}s)"
        else
            bad "regress/$mode"
            tail -6 "$log"
        fi
    done
}

stage_fuzz() {
    n="${1:-3000}"
    note "== fuzz: $n iterations =="
    t0=$(date +%s)
    python tools/fuzz.py "$n" > "$LOG/fuzz.log" 2>&1; rc=$?
    dt=$(( $(date +%s) - t0 ))
    if [ "$rc" -eq 0 ] && grep -q '0 crash(es)' "$LOG/fuzz.log"; then
        ok "fuzz $n (${dt}s)"
    else
        bad "fuzz $n"
        tail -8 "$LOG/fuzz.log"
    fi
}

stage_lsp() {
    note "== lsp: protocol abuse + document storm =="
    t0=$(date +%s)
    ZXLS_BIN=bin/zxls.exe python tools/lspstress.py > "$LOG/lspstress.log" 2>&1; rc=$?
    dt=$(( $(date +%s) - t0 ))
    if [ "$rc" -eq 0 ] && grep -q '0 failure(s)' "$LOG/lspstress.log"; then
        ok "lspstress (${dt}s)"
    else
        bad "lspstress"
        tail -10 "$LOG/lspstress.log"
    fi
}

stage_numeric() {
    note "== numeric: 600k random samples + extreme values =="
    python - "$LOG" "$ZXC" <<'PY'
import os, re, subprocess, sys

log, zxc = sys.argv[1], os.path.abspath(sys.argv[2])
zxc = zxc.replace('\\', '/')
src = open('tests/good/simd-stress.zx', encoding='utf-8').read()
ok = True

def run(exe):
    return subprocess.run([exe], capture_output=True, text=True)

# 3 seeds x 200k / 50k samples = 600k random cases.
for seed in (20261114, 42424242, 987654321):
    s = src.replace('Rng{ .s = 20261114 }', 'Rng{ .s = %d }' % seed)
    s = s.replace('n < 20000', 'n < 200000').replace('n < 5000', 'n < 50000')
    p = os.path.join(log, 'numeric-%d.zx' % seed)
    open(p, 'w', encoding='utf-8').write(s)
    exe = os.path.splitext(p)[0] + '.exe'
    b = subprocess.run([zxc, p, '--release', '-o', exe],
                       capture_output=True, text=True)
    if b.returncode != 0:
        print('  FAIL  numeric seed %d: build failed' % seed)
        print((b.stderr or b.stdout)[-400:])
        ok = False
        continue
    r = run(exe)
    if r.returncode != 0:
        print('  FAIL  numeric seed %d: run rc=%d' % (seed, r.returncode))
        ok = False
        continue
    lines = r.stdout.splitlines()
    bads = [l for l in lines if re.search(r'bad=[1-9]', l)]
    missing = [l for l in lines if 'bad=' not in l and 'eps0' not in l
               and 'singular' not in l and 'pinned' not in l]
    if bads or missing or len(lines) < 10:
        print('  FAIL  numeric seed %d' % seed)
        for l in (bads + missing)[:6]:
            print('        ' + l)
        ok = False
    else:
        print('  PASS  numeric seed %d (600k-scale run)' % seed)

# Extreme values: huge/tiny scale, underflow, NaN, 64-lane vector.
ext = '''const std = @mod("std");
const V4 = @Vec(f32, 4);
const V64 = @Vec(f32, 64);
const Mat4 = std.simd.Mat4;

const near = fn (a: f32, b: f32, eps: f32) bool {
    var d = a - b;
    if (d < 0.0) { d = -d; }
    return d <= eps;
};

pub const main = fn () u8 {
    // det == 1 (1e20 * 1e-20), round trip must stay inside f32 noise.
    let s = Mat4.fromScale(V4{1e20, 1e-20, 1.0, 0.0});
    let r = s.mul(s.invert());
    let diag = near(r.c0.0, 1.0, 1e-3) && near(r.c1.1, 1.0, 1e-3)
            && near(r.c2.2, 1.0, 1e-3) && near(r.c3.3, 1.0, 1e-3);
    let off = r.c0.1 + r.c0.2 + r.c0.3 + r.c1.0 + r.c1.2 + r.c1.3
            + r.c2.0 + r.c2.1 + r.c2.3 + r.c3.0 + r.c3.1 + r.c3.2;
    std.println("ext scale: ok={} det={}", diag && near(off, 0.0, 1e-3),
                s.determinant());

    // det underflows to 0 -> zero matrix, no division by zero.
    let tiny = Mat4.fromScale(V4{1e-20, 1e-20, 1e-20, 0.0});
    let tz = Mat4.zero();
    let ti = tiny.invert();
    std.println("ext underflow: det={} zero={}", tiny.determinant(), ti.eq(tz));

    // NaN input must not trap; NaN is expected to propagate.
    var zf: f32 = 0.0;
    let nan = zf / zf;
    let nm = Mat4.fromScale(V4{nan, 1.0, 1.0, 0.0});
    let ni = nm.invert();
    std.println("ext nan: survived=true nanOut={}", ni.c0.0 != ni.c0.0);

    // 64-lane vector: splat, multiply, scalar reads.
    let a = (V64)1.0;
    let b = a * (V64)2.0;
    std.println("ext vec64: sum={}", b.0 + b.10 + b.63);
    return 0;
};
'''
p = os.path.join(log, 'extreme.zx')
open(p, 'w', encoding='utf-8').write(ext)
exe = os.path.splitext(p)[0] + '.exe'
b = subprocess.run([zxc, p, '-o', exe], capture_output=True, text=True)
if b.returncode != 0:
    print('  FAIL  extreme: build failed')
    print((b.stderr or b.stdout)[-400:])
    ok = False
else:
    r = run(exe)
    want = ['ext scale: ok=true', 'ext underflow: det=0 zero=true',
            'ext nan: survived=true', 'ext vec64: sum=6']
    miss = [w for w in want
            if not any(l.startswith(w) or l == w for l in r.stdout.splitlines())]
    if r.returncode != 0 or miss:
        print('  FAIL  extreme: rc=%d' % r.returncode)
        print(r.stdout[-400:])
        for w in miss:
            print('        missing: ' + w)
        ok = False
    else:
        print('  PASS  extreme (huge/tiny/underflow/NaN/vec64)')

sys.exit(0 if ok else 1)
PY
    if [ $? -eq 0 ]; then ok "numeric"; else bad "numeric"; fi
}

stage_concurrent() {
    note "== concurrent: 16 same-file + 8 mixed builds =="
    d="$LOG/concurrent"
    rm -rf "$d"
    mkdir -p "$d"

    i=0
    while [ "$i" -lt 16 ]; do
        (
            timeout 240 "$ZXC" tests/good/simd-stress.zx --release \
                -o "$d/same-$i.exe" > "$d/same-$i.log" 2>&1
            echo $? > "$d/same-$i.rc"
        ) &
        i=$((i + 1))
    done
    wait

    nok=0
    i=0
    while [ "$i" -lt 16 ]; do
        rc=$(cat "$d/same-$i.rc" 2>/dev/null || echo 99)
        if [ "$rc" = "0" ] && [ -f "$d/same-$i.exe" ]; then
            nok=$((nok + 1))
        else
            printf '  FAIL  same-%d rc=%s\n' "$i" "$rc"
            head -4 "$d/same-$i.log" 2>/dev/null
        fi
        i=$((i + 1))
    done
    if [ "$nok" -eq 16 ]; then ok "concurrent/same 16/16"; else bad "concurrent/same"; fi

    # Same source must produce identical runtime output across concurrent builds.
    ref="$d/out-0.txt"
    "$d/same-0.exe" > "$ref" 2>&1
    same=1
    i=1
    while [ "$i" -lt 4 ]; do
        "$d/same-$i.exe" > "$d/out-$i.txt" 2>&1
        cmp -s "$ref" "$d/out-$i.txt" || same=0
        i=$((i + 1))
    done
    if [ "$same" -eq 1 ]; then ok "concurrent/output identical"; else bad "concurrent/output"; fi

    mixed="tests/good/std-simd.zx tests/good/simd-stress.zx tests/good/vec.zx spec/examples/generic.zx tests/good/casts-one-way.zx tests/good/alloc-once.zx demo/snake.zx spec/examples/hello.zx"
    n=0
    for f in $mixed; do
        (
            timeout 240 "$ZXC" "$f" --release -o "$d/mix-$n.exe" \
                > "$d/mix-$n.log" 2>&1
            echo $? > "$d/mix-$n.rc"
        ) &
        n=$((n + 1))
    done
    wait

    mok=0
    i=0
    while [ "$i" -lt "$n" ]; do
        rc=$(cat "$d/mix-$i.rc" 2>/dev/null || echo 99)
        if [ "$rc" = "0" ] && [ -f "$d/mix-$i.exe" ]; then
            mok=$((mok + 1))
        else
            printf '  FAIL  mix-%d rc=%s\n' "$i" "$rc"
            head -4 "$d/mix-$i.log" 2>/dev/null
        fi
        i=$((i + 1))
    done
    if [ "$mok" -eq "$n" ]; then
        ok "concurrent/mixed $mok/$n"
    else
        bad "concurrent/mixed $mok/$n"
    fi
}

# ---- entry ----------------------------------------------------------------

case "${1:-all}" in
    all)        stage_regress; stage_fuzz 3000; stage_lsp; stage_numeric; stage_concurrent ;;
    regress)    shift; stage_regress "${1:-plain nocache checked}" ;;
    fuzz)       shift; stage_fuzz "${1:-3000}" ;;
    lsp)        stage_lsp ;;
    numeric)    stage_numeric ;;
    concurrent) stage_concurrent ;;
    *)          echo "usage: sh tools/stress.sh [all|regress [plain|nocache|checked]|fuzz N|lsp|numeric|concurrent]"; exit 2 ;;
esac

echo
if [ "$FAIL" -ne 0 ]; then
    echo "STRESS FAILED ($FAIL):$FAILED"
    exit 1
fi
echo "STRESS ALL PASS"
