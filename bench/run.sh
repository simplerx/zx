#!/bin/sh
# Zx benchmark：每程序跑 K 次取中位数，再减去空程序基线（Windows 上进程
# 创建要 12–14ms，80ms 量级的负载用单次墙钟量等于 25% 误差——之前
# BASELINE.md 里的 "±20ms 噪声" 大半是这个方法问题，不是机器抖）。
# 用法：sh bench/run.sh           # 每程序 7 次
#       sh bench/run.sh --quick   # 每程序 1 次（不扣基线，只看墙钟）
set -e
cd "$(dirname "$0")/.."

ZXC=${ZXC:-bin/zxc.exe}
OUT=".tmp/zxbench"
mkdir -p "$OUT"
REPEAT=7
[ "$1" = "--quick" ] && REPEAT=1


# 跑一个已编译的程序 REPEAT 次，把每次墙钟（ms）打到 stdout，一行一个。
# 计时用 $EPOCHREALTIME（bash 内建，无子进程开销）——`date +%s%N` 每次调用
# 自己就是一个 30ms 级的进程创建，会把 80ms 的负载量出 25% 的假误差。
# 去掉小数点直接得到微秒整数，64 位整数除出毫秒。
btimes() {
    i=1
    while [ $i -le $REPEAT ]; do
        t0=${EPOCHREALTIME/./}
        "$1" >/dev/null 2>&1
        t1=${EPOCHREALTIME/./}
        echo $(( (t1 - t0) / 1000 ))
        i=$((i + 1))
    done
}

# 中位数：sort -n 取正中（偶数个取偏小那个，稳定）
median() {
    sort -n | awk '{ a[NR]=$1 } END { print (NR==0) ? 0 : a[(NR+1)/2] }'
}

# ---- 空程序基线：进程创建 + shell 往返的固定开销 ----
cat > "$OUT/empty.zx" <<'EOF'
const main = fn () u8 { return 0; };
EOF
$ZXC "$OUT/empty.zx" -o "$OUT/empty" --release >/dev/null 2>&1

BASE=0
if [ $REPEAT -gt 1 ]; then
    # Windows 上 zig cc 的产物一律补了 .exe；万一没有就退回裸名
    empty="$OUT/empty.exe"
    [ -x "$empty" ] || empty="$OUT/empty"
    BASE=$(btimes "$empty" | median)
fi

run() {
    name=$1; file=$2
    # 基准量的是发布性能：--release（-O2）。dev 档默认 -O0，测不得。
    if ! $ZXC "$file" -o "$OUT/$name" --release >/dev/null 2>&1; then
        printf "  %-8s build FAILED\n" "$name"
        return
    fi
    prog="$OUT/$name.exe"
    [ -x "$prog" ] || prog="$OUT/$name"
    out=$("$prog")
    med=$(btimes "$prog" | median)
    net=$(( med - BASE ))
    [ $net -lt 0 ] && net=0
    printf "  %-8s gross %5d ms | net %5d ms | %s\n" "$name" "$med" "$net" "$out"
}

echo "== zx benchmark (median of $REPEAT, base $BASE ms) =="
run fib     bench/fib.zx
run mandel  bench/mandel.zx
run mmat    bench/mmat.zx
run str     bench/str.zx
run json    bench/json.zx
run cont    bench/containers.zx
run pool    bench/pool.zx
run poolraw bench/poolraw.zx
