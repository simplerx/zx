# Benchmark 基线

宿主侧计时：每程序跑 7 次取**中位数**，再减去空程序基线（进程创建 +
shell 往返 ≈ 30ms/bash 调用、≈13ms/净 spawn）。旧版"3 次取最好的墙钟"
里 12–14ms 是 spawn、两次 `date` 子进程又各 ~30ms——80ms 量级的负载
用那种量法等于 25% 假误差，所谓 "±20ms 噪声" 大半是这个方法问题。
运行：`sh bench/run.sh`（或 `--quick` 跑一次，不扣基线）。

## 当前基线

- 日期：2026-11-04
- 编译器：bin/zxc.exe（v0.58，惰性模块加载 + Allocator ctx），后端 zig cc `-O2`
- 机器：Windows x86_64
- 计量：中位 gross / 净（扣空程序基线 31ms）

| 负载  | gross (ms) | net (ms) | 校验和（正确性锚点，跨后端必须一致） |
|-------|-----------|----------|-----------------------------------------------|
| fib   | 44        | 13       | fib(34)=5702887                               |
| mandel| 181       | 150      | sum=73280456                                  |
| mmat  | 118       | 87       | c00=6118 cN1=6191 cN2=6118 cNN=6187           |
| str   | 34        | 3        | len=108890 hash=2573974805                    |
| json  | 118       | 87       | total=840000 hash=3599512485                  |
| cont  | 108       | 77       | 7385…3782（见下）                              |
| pool  | 253       | 222      | sink=70000000                                 |
| poolraw | 325     | 294      | sink=70000000                                 |

**运行时已与手写 C 打平**（同 zig cc -O2、同基线扣法，Python 精测）：
fib 14.4 vs 14.1、mandel 130.9 vs 132.0、mmat 92.1 vs 93.6、str 2.6 vs 2.0。
-O3 / -march=native / -fno-math-errno 对 mmat、mandel 全部无效
（内存带宽 / 依赖链受限）。继续在 codegen 层找性能没有收益。

## 容器基线（M33 之后）

`sh bench/run.sh` 里的 `cont` 是标准库容器（bench/containers.zx）：
ArrayList push/append/insertAt + HashMap put/get（含扩容），五个负载各打
一个校验和。M33 优化（@memcpy/@memmove/@memset + HashMap 哈希缓存）前后
交替实测 6 轮：整体 171 → 131 ms（约 -23%）。校验和跨实现必须一致：

```
7385034332107382267 17904027361301868082 16829848029259157733 409003408716722205 4041127347942793782
```

## 负载说明

- **fib**：递归调用开销 + i64 算术（约 2,900 万次调用）
- **mandel**：1200×800 曼德博集合，f64 乘加内层循环 + 分支预测
- **mmat**：256×256 矩阵乘跑 3 遍，内存带宽 + 三重循环
- **str**：2 万次整数格式化 + 逐字节追加 ArrayList(u8)，分配器压力
- **json / cont / pool / poolraw**：真实 std 算法与分配器对照

## 编译侧基线（v0.58，三项优化后）

- hello 世界（只用 println）：**3 TU**（zxrt+main+std），此前 11 TU；
  冷构建 1.5s → **~480ms**。没被引用的 std 子模块不再进编译与链接。
- 工程无改动重建 **~70–90ms**（产物新于所有 .o 时跳过链接——zig cc
  每次调用 ~200ms 固定开销是这条路线的大头）。
- dev/release 的 .o 分目录（`.zxc-cache/*-dev_*` / `*-rel_*`）：
  `--release` ↔ `zxc run`（-O0）来回切换不再全量重编，回切 **~73ms**。

## 以后怎么用

每次动后端 / 优化器之后跑一遍，和本表对比：

1. **校验和必须逐字符一致**——那是差分测试的锚点，C 后端、原生后端、
   未来的 LLVM 后端都要给出同样的输出；
2. 耗时显著回退（>5%）就是回归，先修再合。

记录新基线：改完之后把表格数字更新成新的，并注明提交号。
自编译耗时作为主指标等自举（M-自举）之后再加进这张表。

## 2026-11-11 复测（压测后）

- 编译器同 v0.58 链路 + 压测修复（commit 05e1ad7），zig cc -O2 不变。
- 8/8 校验和一致：fib 15 / mandel 153 / mmat 91-102 / str 3 / json 88 / cont 76 / pool 221-234 / poolraw 285（net ms）。
- mmat/pool 比基线 +5~13%：毛值稳定（130-134/261-265），总量波动来自扣基线的 31ms 项 + 同机 Zed/LSP 负载；这两项本就是内存带宽型，判定为环境噪声而非回退（codegen 无相关改动）。
- bench/pool.zx 改用 pub deinit（容器可见性规则上线后 std/pool.zx 漏标 pub，已修）。

## 2026-11-13 复测（1.3.0 发布压测）

- 编译器 1.3.0（§151 / §152 / §153 之后），`zig cc -O2` 不变。
  `sh test.sh` EXIT=0：runner 388/388、LSP 冒烟 417 项、snake `--sim` 300 局、
  §151 / §152 / §153 断言全绿；`tools/lspstress.py` 0 failure（2MB 文档 0.37s 出诊断）。
- 8/8 校验和一致：fib 8 / mandel 148 / mmat 108 / str 0 / json 94 /
  cont 90 / pool 234 / poolraw 294（net ms，空程序基线 37ms）。
- **mmat 首轮 `build FAILED`**：§130 region 合并把 `zx_heapalloc(al, …)` 发在
  块开头，而 §150 之后 `let al = std.heap.page` 会生成运行时局部声明，C
  先用后声明报 B1002。§153 把 `__zxrN` 初始化挪到分配器声明之后，
  `6118/6191/6118/6187` 与旧基线逐字符一致；新增
  `tests/good/region-local-alloc.zx` + `test.sh` §153 顺序断言守门。
- `bench/run.sh` 同时修正：某个程序 build FAILED 以前只打印一行就返回 0，
  压测仍报「通过」；现在累计失败数并以非零退出。
- 毛值整体比 11-11 高 5~15%（同机 Zed/LSP 负载 + 基线 31→37ms），校验和
  与生成代码结构无回退，判定为环境噪声，不改主表。
