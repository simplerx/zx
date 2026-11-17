#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""§180 指针传播路径穷举探测器。

递归枚举「贴标分配 -> N 步传播 -> 释放 -> 读取」的全部组合，
批量编译，找出「释放后读取却没报 E0512」的漏报分支。

用法：
    python tools/tag_probe.py            # 全矩阵（深度 1-2）
    python tools/tag_probe.py --depth 3  # 含深度 3
    python tools/tag_probe.py --verbose  # 连命中/报错一起打印

每条路径是一串传播算子；算子把上一步的载体 s 变成新载体 d。
判据：读的那个载体在释放之后 -> 必须报 E0512（或调用点的 UAF）。
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ZXC = os.path.join(ROOT, "bin", "zxc.exe")

# 传播算子：{s}=上一步载体, {d}=新载体, {i}=步序号
# 覆盖：形参传递、结构体/元组字段、切片视图、家族 cast、数组/元组常索引、别名、解包
PROPS = {
    "param":    "var {d} = pass1({s});",
    "param2":   "var {d} = pass2({s});",          # 两层形参转发
    "field":    "var {d} = W{{.v = {s}}}.v;",      # 结构体字段
    "nfield":   "var {d} = W2{{.w = W{{.v = {s}}}}}.w.v;",  # 嵌套字段
    "tuple":    "var {d} = {{{s}}}.0;",            # 元组元素
    "cast":     "var {d} = ([]u8){s};",            # 家族 cast
    "slice":    "var {d} = {s}[0..];",             # 切片视图
    "arrconst": "var a{i} = std.heapSlice(std.heap.page, []u8, 2); a{i}[0] = {s}; var {d} = a{i}[0];",  # 数组常索引
    "alias":    "var {d} = {s};",                  # 直接别名
}

# 释放方式
FREES = {
    "direct":   "std.heap.page.deinit(base);",
    "releaser": "release(base);",                  # 经释放器函数
    "free_b":   "std.free(std.heap.page, base);",
}

# 读取位置（读已释放载体）
READS = {
    "index":    "let zz = {q}[0]; _ = zz;",
    "param":    "rd({q});",                        # 经函数形参读取
    "field":    "let zz = W{{.v = {q}}}.v[0]; _ = zz;",
    "releaser": "release({q});",                   # 再次释放（double free）
}

PRELUDE = """const std = @mod("std");
const W = struct {
    pub v: []u8,
};
const W2 = struct {
    pub w: W,
};
const pass1 = fn (p: []u8) []u8 { return p; };
const pass2 = fn (p: []u8) []u8 { return pass1(p); };
const rd = fn (p: []u8) u8 { return p[0]; };
const release = fn (p: []u8) void { std.heap.page.deinit(p); };
"""


def gen_case(props, free, read):
    lines = ["pub const main = fn () !i32 {",
             "    var base = tag.h : std.heapSlice(std.heap.page, u8, 4);"]
    s = "base"
    for i, op in enumerate(props):
        d = "v%d" % (i + 1)
        body = PROPS[op]
        lines.append("    " + body.format(s=s, d=d, i=i))
        s = d
    lines.append("    " + FREES[free])
    lines.append("    " + READS[read].format(q=s))
    lines.append("    return 0;")
    lines.append("};")
    return PRELUDE + "\n".join(lines) + "\n"


def paths(depth):
    """迭代加深：长度 1..depth 的全部算子序列。"""
    for n in range(1, depth + 1):
        import itertools
        for seq in itertools.product(sorted(PROPS), repeat=n):
            yield seq


def main():
    args = sys.argv[1:]
    depth = 2
    verbose = False
    if "--depth" in args:
        depth = int(args[args.index("--depth") + 1])
    if "--verbose" in args:
        verbose = True

    tmp = tempfile.mkdtemp(prefix="zx_tagprobe_")
    miss, hit, other = [], 0, []
    total = 0
    for seq in paths(depth):
        for fk in FREES:
            for rk in READS:
                total += 1
                name = "_".join(seq) + "__" + fk + "__" + rk
                path = os.path.join(tmp, name + ".zx")
                with open(path, "wb") as fh:
                    fh.write(gen_case(seq, fk, rk).encode("utf-8"))
                out = subprocess.run([ZXC, path, "--fe=c"],
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT).stdout.decode("utf-8", "replace")
                if "E0512" in out or "E0509" in out:
                    hit += 1
                    if verbose:
                        print("HIT  ", name)
                elif "error[" not in out:
                    miss.append((name, path))
                    print("MISS ", name)
                else:
                    # 非 UAF 的诊断（E0513 泄漏等）不算漏报，但记下来供查
                    code = ""
                    for tok in out.split():
                        if tok.startswith("error["):
                            code = tok
                            break
                    other.append((name, code, path))
                    if verbose:
                        print("OTHER", name, code)

    print("\n== 汇总 ==")
    print("用例 %d / 命中 %d / 漏报 %d / 其他诊断 %d" % (total, hit, len(miss), len(other)))
    if miss:
        print("\n漏报样例（首个）源码:")
        print(open(miss[0][1], "r", encoding="utf-8").read())
    if other and verbose:
        print("\n其他诊断样例:")
        for nm, cd, pt in other[:3]:
            print("--", nm, cd)
            print(open(pt, "r", encoding="utf-8").read())
    print("临时目录:", tmp)


if __name__ == "__main__":
    main()
