#!/bin/sh
# Zx benchmark：宿主侧计时（进程墙钟，含启动），每程序跑 3 次取最好。
# 用法：sh bench/run.sh           # 编译 + 计时
#       sh bench/run.sh --quick   # 每程序只跑 1 次
set -e
cd "$(dirname "$0")/.."

ZXC=${ZXC:-bin/zxc.exe}
OUT=".tmp/zxbench"
mkdir -p "$OUT"
REPEAT=3
[ "$1" = "--quick" ] && REPEAT=1

ns() { date +%s%N; }

run() {
    name=$1; file=$2
    # 基准量的是发布性能：--release（-O2）。dev 档默认 -O0，测不得。
    if ! $ZXC "$file" -o "$OUT/$name" --release >/dev/null 2>&1; then
        printf "  %-8s build FAILED\n" "$name"
        return
    fi
    best=""
    out=""
    i=1
    while [ $i -le $REPEAT ]; do
        t0=$(ns)
        out=$("$OUT/$name")
        t1=$(ns)
        d=$(( (t1 - t0) / 1000000 ))
        if [ -z "$best" ] || [ "$d" -lt "$best" ]; then best=$d; fi
        i=$((i + 1))
    done
    printf "  %-8s %6d ms   | %s\n" "$name" "$best" "$out"
}

echo "== zx benchmark (best of $REPEAT, wall-clock ms) =="
run fib    bench/fib.zx
run mandel bench/mandel.zx
run mmat   bench/mmat.zx
run str    bench/str.zx
run json   bench/json.zx
run cont   bench/containers.zx
run pool    bench/pool.zx
run poolraw bench/poolraw.zx
