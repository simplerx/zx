/* simd_impl.c —— std/simd.zx 的手写 SIMD 内核（§161）。
 *
 * 为什么存在：Zx 没有向量类型，§159 的 std.simd 只是普通循环，靠下游 C 编译器
 * 在 --release（-O2）里自动向量化——dev（-O0）一条向量指令都拿不到。这份文件
 * 用 @shim 挂进构建，把 14 个切片内核写成显式 intrinsic：-O0 下也是真指令。
 *
 * 三条实现并存，运行期选一条：
 *   avx2    8 x f32 / 8 x i32（256 位；需要 CPU 支持，func 级 target 属性）
 *   sse2    4 x f32 / 4 x i32（128 位；x86_64 的基线）
 *   scalar  普通循环（非 x86 的唯一实现，也是对照组）
 * ZX_SIMD_ISA=scalar|sse2|avx2 可以强制指定（守门用例就靠它把三条路跑成同一份
 * 输出）。设了但没有对应指令集时按实际能力回退，不会 SIGILL。
 *
 * **数值契约：三条路径逐位一致。** 三个决定都是为它服务的：
 *   1. 不用 FMA。SSE2 没有 FMA，AVX2 一旦融合，同一份输入在两种 CPU 上就不同。
 *   2. 归约固定 8 条累加链，最后按 ((t0+t1)+(t2+t3)) + ((t4+t5)+(t6+t7)) 合并。
 *      AVX2 正好一个 8 lane 寄存器，SSE2 两个 4 lane，标量八个变量——分组相同，
 *      加法结合顺序就相同。尾块一律并进第 0 条链（与标量实现逐字对应）。
 *   3. clamp / abs 用有序比较 + 选择，不用 _mm_min_ps / 清符号位：min/max 遇 NaN
 *      返回第二操作数、清符号位会把 -0.0 变成 +0.0，都和 Zx 里那段标量代码不同。
 *
 * 别名：逐元素内核允许 dst == a / dst == b（每个下标只读写自己那一格），所以
 * 这里一律不加 restrict——那是在撒谎，编译器有权据此重排。
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#  define ZX_X86 1
#endif

/* MSVC 没有 target 属性，也没法在一个 TU 里混编译级别：它只走标量路径。
 * zxc 用的是 zig cc（clang），所以这条分支实际只影响"别人拿这份文件自己编"。 */
#if defined(ZX_X86) && (defined(__clang__) || defined(__GNUC__))
#  define ZX_VEC 1
#  include <emmintrin.h>     /* SSE2 */
#  include <immintrin.h>     /* AVX2 声明；函数上带 target 属性才允许用 */
#  include <cpuid.h>         /* 自己查 CPUID，不依赖 __cpu_model 运行时符号 */
#  define ZX_AVX2 __attribute__((target("avx2")))
#endif

/* ---------------- 标量实现（对照组，也是非 x86 的唯一实现） ---------------- */

static void add_f32_scalar(float *d, const float *a, const float *b, size_t n) {
    for (size_t i = 0; i < n; i++) d[i] = a[i] + b[i];
}
static void sub_f32_scalar(float *d, const float *a, const float *b, size_t n) {
    for (size_t i = 0; i < n; i++) d[i] = a[i] - b[i];
}
static void mul_f32_scalar(float *d, const float *a, const float *b, size_t n) {
    for (size_t i = 0; i < n; i++) d[i] = a[i] * b[i];
}
static void scale_f32_scalar(float *d, const float *a, float s, size_t n) {
    for (size_t i = 0; i < n; i++) d[i] = a[i] * s;
}
static void saxpy_f32_scalar(float *d, const float *a, const float *b, float s, size_t n) {
    for (size_t i = 0; i < n; i++) d[i] = a[i] + b[i] * s;
}
static void lerp_f32_scalar(float *d, const float *a, const float *b, float t, size_t n) {
    for (size_t i = 0; i < n; i++) {
        float av = a[i];
        d[i] = av + (b[i] - av) * t;
    }
}
/* 与 Zx 侧 `if (r < lo) r = lo; if (r > hi) r = hi;` 同义：NaN 两个比较都为 false，
 * 原样留下；-0.0 与 +0.0 也保持原样。 */
static void clamp_f32_scalar(float *d, const float *a, float lo, float hi, size_t n) {
    for (size_t i = 0; i < n; i++) {
        float r = a[i];
        if (r < lo) { r = lo; }
        if (r > hi) { r = hi; }
        d[i] = r;
    }
}
static void abs_f32_scalar(float *d, const float *a, size_t n) {
    for (size_t i = 0; i < n; i++) {
        float r = a[i];
        if (r < 0.0f) { r = -r; }
        d[i] = r;
    }
}
static void fill_f32_scalar(float *d, float v, size_t n) {
    for (size_t i = 0; i < n; i++) d[i] = v;
}
/* Zx 的 i32 `+` 是回绕的，C 的有符号溢出是 UB：一律走无符号再转回来。 */
static void add_i32_scalar(int32_t *d, const int32_t *a, const int32_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) d[i] = (int32_t)((uint32_t)a[i] + (uint32_t)b[i]);
}
static void sub_i32_scalar(int32_t *d, const int32_t *a, const int32_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) d[i] = (int32_t)((uint32_t)a[i] - (uint32_t)b[i]);
}
static void fill_i32_scalar(int32_t *d, int32_t v, size_t n) {
    for (size_t i = 0; i < n; i++) d[i] = v;
}

/* 归约：8 条累加链 + 固定合并顺序（见文件头第 2 条）。 */
static float zx_reduce8(const float *t) {
    return ((t[0] + t[1]) + (t[2] + t[3])) + ((t[4] + t[5]) + (t[6] + t[7]));
}

static float dot_f32_scalar(const float *a, const float *b, size_t n) {
    float t[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
        for (int k = 0; k < 8; k++) t[k] = t[k] + a[i + k] * b[i + k];
    for (; i < n; i++) t[0] = t[0] + a[i] * b[i];
    return zx_reduce8(t);
}

static float sum_f32_scalar(const float *a, size_t n) {
    float t[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
        for (int k = 0; k < 8; k++) t[k] = t[k] + a[i + k];
    for (; i < n; i++) t[0] = t[0] + a[i];
    return zx_reduce8(t);
}

#ifdef ZX_VEC

/* ---------------- 向量实现 ---------------- */

/* 逐元素二元 op 的单指令落点。命名约定：<op>128 / <op>256 / <op>_1。 */
static inline __m128 add128(__m128 a, __m128 b) { return _mm_add_ps(a, b); }
static inline __m256 add256(__m256 a, __m256 b) { return _mm256_add_ps(a, b); }
static inline float add_1(float a, float b) { return a + b; }

static inline __m128 sub128(__m128 a, __m128 b) { return _mm_sub_ps(a, b); }
static inline __m256 sub256(__m256 a, __m256 b) { return _mm256_sub_ps(a, b); }
static inline float sub_1(float a, float b) { return a - b; }

static inline __m128 mul128(__m128 a, __m128 b) { return _mm_mul_ps(a, b); }
static inline __m256 mul256(__m256 a, __m256 b) { return _mm256_mul_ps(a, b); }
static inline float mul_1(float a, float b) { return a * b; }

static inline __m128i addi128(__m128i a, __m128i b) { return _mm_add_epi32(a, b); }
static inline __m256i addi256(__m256i a, __m256i b) { return _mm256_add_epi32(a, b); }
static inline int32_t addi_1(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }

static inline __m128i subi128(__m128i a, __m128i b) { return _mm_sub_epi32(a, b); }
static inline __m256i subi256(__m256i a, __m256i b) { return _mm256_sub_epi32(a, b); }
static inline int32_t subi_1(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }

/* d = a <op> b（f32） */
#define ZX_F32_BIN(NAME, OP)                                                  \
    static void NAME##_sse2(float *d, const float *a, const float *b, size_t n) { \
        size_t i = 0;                                                         \
        for (; i + 4 <= n; i += 4)                                            \
            _mm_storeu_ps(d + i,                                              \
                OP##128(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i)));           \
        for (; i < n; i++) d[i] = OP##_1(a[i], b[i]);                         \
    }                                                                         \
    static ZX_AVX2 void NAME##_avx2(float *d, const float *a, const float *b, \
                                    size_t n) {                               \
        size_t i = 0;                                                         \
        for (; i + 8 <= n; i += 8)                                            \
            _mm256_storeu_ps(d + i,                                           \
                OP##256(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i)));     \
        for (; i < n; i++) d[i] = OP##_1(a[i], b[i]);                         \
    }

/* d = a <op> b（i32，回绕语义同 Zx 的 `+` / `-`） */
#define ZX_I32_BIN(NAME, OP)                                                  \
    static void NAME##_sse2(int32_t *d, const int32_t *a, const int32_t *b,   \
                            size_t n) {                                       \
        size_t i = 0;                                                         \
        for (; i + 4 <= n; i += 4)                                            \
            _mm_storeu_si128((__m128i *)(d + i),                              \
                OP##128(_mm_loadu_si128((const __m128i *)(a + i)),            \
                        _mm_loadu_si128((const __m128i *)(b + i))));          \
        for (; i < n; i++) d[i] = OP##_1(a[i], b[i]);                         \
    }                                                                         \
    static ZX_AVX2 void NAME##_avx2(int32_t *d, const int32_t *a,             \
                                    const int32_t *b, size_t n) {             \
        size_t i = 0;                                                         \
        for (; i + 8 <= n; i += 8)                                            \
            _mm256_storeu_si256((__m256i *)(d + i),                           \
                OP##256(_mm256_loadu_si256((const __m256i *)(a + i)),         \
                        _mm256_loadu_si256((const __m256i *)(b + i))));       \
        for (; i < n; i++) d[i] = OP##_1(a[i], b[i]);                         \
    }

/* d = a * s */
#define ZX_F32_SCALE(NAME)                                                    \
    static void NAME##_sse2(float *d, const float *a, float s, size_t n) {    \
        const __m128 vs = _mm_set1_ps(s);                                     \
        size_t i = 0;                                                         \
        for (; i + 4 <= n; i += 4)                                            \
            _mm_storeu_ps(d + i, _mm_mul_ps(_mm_loadu_ps(a + i), vs));        \
        for (; i < n; i++) d[i] = a[i] * s;                                   \
    }                                                                         \
    static ZX_AVX2 void NAME##_avx2(float *d, const float *a, float s,        \
                                    size_t n) {                               \
        const __m256 vs = _mm256_set1_ps(s);                                  \
        size_t i = 0;                                                         \
        for (; i + 8 <= n; i += 8)                                            \
            _mm256_storeu_ps(d + i, _mm256_mul_ps(_mm256_loadu_ps(a + i), vs)); \
        for (; i < n; i++) d[i] = a[i] * s;                                   \
    }

/* d = a + b * s（saxpy）与 d = a + (b - a) * t（lerp）：同形状，op 自己拼。 */
static inline __m128 saxpy128(__m128 a, __m128 b, __m128 s) {
    return _mm_add_ps(a, _mm_mul_ps(b, s));
}
static inline __m256 saxpy256(__m256 a, __m256 b, __m256 s) {
    return _mm256_add_ps(a, _mm256_mul_ps(b, s));
}
static inline float saxpy_1(float a, float b, float s) { return a + b * s; }

static inline __m128 lerp128(__m128 a, __m128 b, __m128 t) {
    return _mm_add_ps(a, _mm_mul_ps(_mm_sub_ps(b, a), t));
}
static inline __m256 lerp256(__m256 a, __m256 b, __m256 t) {
    return _mm256_add_ps(a, _mm256_mul_ps(_mm256_sub_ps(b, a), t));
}
static inline float lerp_1(float a, float b, float t) { return a + (b - a) * t; }

#define ZX_F32_2S(NAME, OP)                                                   \
    static void NAME##_sse2(float *d, const float *a, const float *b, float s, \
                            size_t n) {                                       \
        const __m128 vs = _mm_set1_ps(s);                                     \
        size_t i = 0;                                                         \
        for (; i + 4 <= n; i += 4)                                            \
            _mm_storeu_ps(d + i, OP##128(_mm_loadu_ps(a + i),                  \
                                         _mm_loadu_ps(b + i), vs));           \
        for (; i < n; i++) d[i] = OP##_1(a[i], b[i], s);                      \
    }                                                                         \
    static ZX_AVX2 void NAME##_avx2(float *d, const float *a, const float *b, \
                                    float s, size_t n) {                      \
        const __m256 vs = _mm256_set1_ps(s);                                  \
        size_t i = 0;                                                         \
        for (; i + 8 <= n; i += 8)                                            \
            _mm256_storeu_ps(d + i, OP##256(_mm256_loadu_ps(a + i),            \
                                            _mm256_loadu_ps(b + i), vs));     \
        for (; i < n; i++) d[i] = OP##_1(a[i], b[i], s);                      \
    }

/* 有序比较 + 选择，保住 NaN 与 -0.0 的原始行为（见文件头第 3 条）。 */
static inline __m128 clamp128(__m128 v, __m128 lo, __m128 hi) {
    __m128 m = _mm_cmplt_ps(v, lo);
    v = _mm_or_ps(_mm_and_ps(m, lo), _mm_andnot_ps(m, v));
    m = _mm_cmpgt_ps(v, hi);
    v = _mm_or_ps(_mm_and_ps(m, hi), _mm_andnot_ps(m, v));
    return v;
}
static inline __m256 clamp256(__m256 v, __m256 lo, __m256 hi) {
    __m256 m = _mm256_cmp_ps(v, lo, _CMP_LT_OQ);
    v = _mm256_or_ps(_mm256_and_ps(m, lo), _mm256_andnot_ps(m, v));
    m = _mm256_cmp_ps(v, hi, _CMP_GT_OQ);
    v = _mm256_or_ps(_mm256_and_ps(m, hi), _mm256_andnot_ps(m, v));
    return v;
}
static inline float clamp_1(float v, float lo, float hi) {
    if (v < lo) { v = lo; }
    if (v > hi) { v = hi; }
    return v;
}

#define ZX_F32_CLAMP(NAME)                                                    \
    static void NAME##_sse2(float *d, const float *a, float lo, float hi,     \
                            size_t n) {                                       \
        const __m128 vlo = _mm_set1_ps(lo), vhi = _mm_set1_ps(hi);            \
        size_t i = 0;                                                         \
        for (; i + 4 <= n; i += 4)                                            \
            _mm_storeu_ps(d + i, clamp128(_mm_loadu_ps(a + i), vlo, vhi));    \
        for (; i < n; i++) d[i] = clamp_1(a[i], lo, hi);                      \
    }                                                                         \
    static ZX_AVX2 void NAME##_avx2(float *d, const float *a, float lo,       \
                                    float hi, size_t n) {                     \
        const __m256 vlo = _mm256_set1_ps(lo), vhi = _mm256_set1_ps(hi);      \
        size_t i = 0;                                                         \
        for (; i + 8 <= n; i += 8)                                            \
            _mm256_storeu_ps(d + i, clamp256(_mm256_loadu_ps(a + i), vlo, vhi)); \
        for (; i < n; i++) d[i] = clamp_1(a[i], lo, hi);                      \
    }

/* d = |a|：`v < 0 ? -v : v`，不用清符号位（那会把 -0.0 变成 +0.0）。 */
static inline __m128 abs128(__m128 v) {
    const __m128 zero = _mm_setzero_ps();
    __m128 neg = _mm_cmplt_ps(v, zero);
    return _mm_or_ps(_mm_and_ps(neg, _mm_sub_ps(zero, v)), _mm_andnot_ps(neg, v));
}
static inline __m256 abs256(__m256 v) {
    const __m256 zero = _mm256_setzero_ps();
    __m256 neg = _mm256_cmp_ps(v, zero, _CMP_LT_OQ);
    return _mm256_or_ps(_mm256_and_ps(neg, _mm256_sub_ps(zero, v)),
                        _mm256_andnot_ps(neg, v));
}
static inline float abs_1(float v) { return v < 0.0f ? -v : v; }

#define ZX_F32_ABS(NAME)                                                      \
    static void NAME##_sse2(float *d, const float *a, size_t n) {             \
        size_t i = 0;                                                         \
        for (; i + 4 <= n; i += 4)                                            \
            _mm_storeu_ps(d + i, abs128(_mm_loadu_ps(a + i)));                \
        for (; i < n; i++) d[i] = abs_1(a[i]);                                \
    }                                                                         \
    static ZX_AVX2 void NAME##_avx2(float *d, const float *a, size_t n) {     \
        size_t i = 0;                                                         \
        for (; i + 8 <= n; i += 8)                                            \
            _mm256_storeu_ps(d + i, abs256(_mm256_loadu_ps(a + i)));          \
        for (; i < n; i++) d[i] = abs_1(a[i]);                                \
    }

/* d[i] = v */
#define ZX_F32_FILL(NAME)                                                     \
    static void NAME##_sse2(float *d, float v, size_t n) {                    \
        const __m128 vv = _mm_set1_ps(v);                                     \
        size_t i = 0;                                                         \
        for (; i + 4 <= n; i += 4) _mm_storeu_ps(d + i, vv);                  \
        for (; i < n; i++) d[i] = v;                                          \
    }                                                                         \
    static ZX_AVX2 void NAME##_avx2(float *d, float v, size_t n) {            \
        const __m256 vv = _mm256_set1_ps(v);                                  \
        size_t i = 0;                                                         \
        for (; i + 8 <= n; i += 8) _mm256_storeu_ps(d + i, vv);               \
        for (; i < n; i++) d[i] = v;                                          \
    }

#define ZX_I32_FILL(NAME)                                                     \
    static void NAME##_sse2(int32_t *d, int32_t v, size_t n) {                \
        const __m128i vv = _mm_set1_epi32(v);                                 \
        size_t i = 0;                                                         \
        for (; i + 4 <= n; i += 4)                                            \
            _mm_storeu_si128((__m128i *)(d + i), vv);                         \
        for (; i < n; i++) d[i] = v;                                          \
    }                                                                         \
    static ZX_AVX2 void NAME##_avx2(int32_t *d, int32_t v, size_t n) {        \
        const __m256i vv = _mm256_set1_epi32(v);                              \
        size_t i = 0;                                                         \
        for (; i + 8 <= n; i += 8)                                            \
            _mm256_storeu_si256((__m256i *)(d + i), vv);                      \
        for (; i < n; i++) d[i] = v;                                          \
    }

ZX_F32_BIN(add_f32, add)
ZX_F32_BIN(sub_f32, sub)
ZX_F32_BIN(mul_f32, mul)
ZX_F32_SCALE(scale_f32)
ZX_F32_2S(saxpy_f32, saxpy)
ZX_F32_2S(lerp_f32, lerp)
ZX_F32_CLAMP(clamp_f32)
ZX_F32_ABS(abs_f32)
ZX_F32_FILL(fill_f32)
ZX_I32_BIN(add_i32, addi)
ZX_I32_BIN(sub_i32, subi)
ZX_I32_FILL(fill_i32)

/* 归约：AVX2 一个 8 lane 累加器，SSE2 两个 4 lane，分组与标量 8 链相同。 */
static float dot_f32_sse2(const float *a, const float *b, size_t n) {
    __m128 s0 = _mm_setzero_ps(), s1 = _mm_setzero_ps();
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i)));
        s1 = _mm_add_ps(s1, _mm_mul_ps(_mm_loadu_ps(a + i + 4), _mm_loadu_ps(b + i + 4)));
    }
    float t[8];
    _mm_storeu_ps(t, s0);
    _mm_storeu_ps(t + 4, s1);
    for (; i < n; i++) t[0] = t[0] + a[i] * b[i];
    return zx_reduce8(t);
}

static ZX_AVX2 float dot_f32_avx2(const float *a, const float *b, size_t n) {
    __m256 s = _mm256_setzero_ps();
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
        s = _mm256_add_ps(s, _mm256_mul_ps(_mm256_loadu_ps(a + i),
                                           _mm256_loadu_ps(b + i)));
    float t[8];
    _mm256_storeu_ps(t, s);
    for (; i < n; i++) t[0] = t[0] + a[i] * b[i];
    return zx_reduce8(t);
}

static float sum_f32_sse2(const float *a, size_t n) {
    __m128 s0 = _mm_setzero_ps(), s1 = _mm_setzero_ps();
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        s0 = _mm_add_ps(s0, _mm_loadu_ps(a + i));
        s1 = _mm_add_ps(s1, _mm_loadu_ps(a + i + 4));
    }
    float t[8];
    _mm_storeu_ps(t, s0);
    _mm_storeu_ps(t + 4, s1);
    for (; i < n; i++) t[0] = t[0] + a[i];
    return zx_reduce8(t);
}

static ZX_AVX2 float sum_f32_avx2(const float *a, size_t n) {
    __m256 s = _mm256_setzero_ps();
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
        s = _mm256_add_ps(s, _mm256_loadu_ps(a + i));
    float t[8];
    _mm256_storeu_ps(t, s);
    for (; i < n; i++) t[0] = t[0] + a[i];
    return zx_reduce8(t);
}

#endif /* ZX_VEC */

/* ---------------- CPU 探测 ---------------- */

/* 不用 __builtin_cpu_supports：它要链接 __cpu_model（libgcc / compiler-rt 的
 * cpuinfo 目标文件），zxc 只链 CRT 时那个符号没有，会 B2001 链接失败。
 * 自己读 CPUID 更省事，也不跨平台引入运行时依赖。 */
#if defined(ZX_VEC)
static unsigned long long zx_xgetbv0(void) {
    unsigned int lo, hi;
    __asm__ volatile(".byte 0x0f, 0x01, 0xd0" : "=a"(lo), "=d"(hi) : "c"(0));
    return ((unsigned long long)hi << 32) | lo;
}

static int zx_detect_avx2(void) {
    unsigned int a = 0, b = 0, c = 0, d = 0;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return 0;
    if (!(c & (1u << 27))) return 0;          /* OSXSAVE：XCR0 才可读 */
    if (!(c & (1u << 28))) return 0;          /* AVX */
    if ((zx_xgetbv0() & 0x6) != 0x6) return 0; /* XMM + YMM 都被 OS 保存 */
    __cpuid_count(7, 0, a, b, c, d);
    return (b & (1u << 5)) != 0;              /* AVX2 */
}
#endif

/* ---------------- 分派 ---------------- */

typedef void (*zx_bin_f32)(float *, const float *, const float *, size_t);
typedef void (*zx_s1_f32)(float *, const float *, float, size_t);
typedef void (*zx_2s_f32)(float *, const float *, const float *, float, size_t);
typedef void (*zx_clamp_f32)(float *, const float *, float, float, size_t);
typedef void (*zx_abs_f32)(float *, const float *, size_t);
typedef void (*zx_fill_f32)(float *, float, size_t);
typedef void (*zx_bin_i32)(int32_t *, const int32_t *, const int32_t *, size_t);
typedef void (*zx_fill_i32)(int32_t *, int32_t, size_t);
typedef float (*zx_dot_f32)(const float *, const float *, size_t);
typedef float (*zx_sum_f32)(const float *, size_t);

static zx_bin_f32    p_add_f32   = add_f32_scalar;
static zx_bin_f32    p_sub_f32   = sub_f32_scalar;
static zx_bin_f32    p_mul_f32   = mul_f32_scalar;
static zx_s1_f32     p_scale_f32 = scale_f32_scalar;
static zx_2s_f32     p_saxpy_f32 = saxpy_f32_scalar;
static zx_2s_f32     p_lerp_f32  = lerp_f32_scalar;
static zx_clamp_f32  p_clamp_f32 = clamp_f32_scalar;
static zx_abs_f32    p_abs_f32   = abs_f32_scalar;
static zx_fill_f32   p_fill_f32  = fill_f32_scalar;
static zx_bin_i32    p_add_i32   = add_i32_scalar;
static zx_bin_i32    p_sub_i32   = sub_i32_scalar;
static zx_fill_i32   p_fill_i32  = fill_i32_scalar;
static zx_dot_f32    p_dot_f32   = dot_f32_scalar;
static zx_sum_f32    p_sum_f32   = sum_f32_scalar;

static int g_level;   /* 0 = 未选；1 标量 / 2 sse2 / 3 avx2 */

/* 选实现。env 可以强制降档（只往低处强制，不越权上跳）。 */
static void zx_simd_select(void) {
    int want = 0;
    const char *e = getenv("ZX_SIMD_ISA");
    if (e) {
        if (!strcmp(e, "scalar")) want = 1;
        else if (!strcmp(e, "sse2")) want = 2;
        else if (!strcmp(e, "avx2")) want = 3;
    }

#if defined(ZX_VEC)
    int have = zx_detect_avx2() ? 3 : 2;
    if (want == 0 || want > have) want = have;
    if (want >= 2) {
        p_add_f32   = add_f32_sse2;
        p_sub_f32   = sub_f32_sse2;
        p_mul_f32   = mul_f32_sse2;
        p_scale_f32 = scale_f32_sse2;
        p_saxpy_f32 = saxpy_f32_sse2;
        p_lerp_f32  = lerp_f32_sse2;
        p_clamp_f32 = clamp_f32_sse2;
        p_abs_f32   = abs_f32_sse2;
        p_fill_f32  = fill_f32_sse2;
        p_add_i32   = add_i32_sse2;
        p_sub_i32   = sub_i32_sse2;
        p_fill_i32  = fill_i32_sse2;
        p_dot_f32   = dot_f32_sse2;
        p_sum_f32   = sum_f32_sse2;
    }
#  if defined(__clang__) || defined(__GNUC__)
    if (want >= 3) {
        p_add_f32   = add_f32_avx2;
        p_sub_f32   = sub_f32_avx2;
        p_mul_f32   = mul_f32_avx2;
        p_scale_f32 = scale_f32_avx2;
        p_saxpy_f32 = saxpy_f32_avx2;
        p_lerp_f32  = lerp_f32_avx2;
        p_clamp_f32 = clamp_f32_avx2;
        p_abs_f32   = abs_f32_avx2;
        p_fill_f32  = fill_f32_avx2;
        p_add_i32   = add_i32_avx2;
        p_sub_i32   = sub_i32_avx2;
        p_fill_i32  = fill_i32_avx2;
        p_dot_f32   = dot_f32_avx2;
        p_sum_f32   = sum_f32_avx2;
    }
#  endif
    g_level = want;
#else
    (void)want;
    g_level = 1;
#endif
}

/* 第一次调用进来时才选实现。指针先全写完，g_level 最后写：并发的第二个线程
 * 要么看到 0（再选一次，结果相同，无害），要么看到选完的一套。 */
static void zx_sync(void) {
    if (g_level == 0) zx_simd_select();
}

int zx_simd_isa_level(void) {
    zx_sync();
    return g_level;
}

/* ---------------- 对外 ABI（名字由 std/simd.zx 的 extern "C" 声明） ---------------- */

void zx_simd_add_f32(float *d, const float *a, const float *b, size_t n) {
    zx_sync();
    p_add_f32(d, a, b, n);
}
void zx_simd_sub_f32(float *d, const float *a, const float *b, size_t n) {
    zx_sync();
    p_sub_f32(d, a, b, n);
}
void zx_simd_mul_f32(float *d, const float *a, const float *b, size_t n) {
    zx_sync();
    p_mul_f32(d, a, b, n);
}
void zx_simd_scale_f32(float *d, const float *a, float s, size_t n) {
    zx_sync();
    p_scale_f32(d, a, s, n);
}
void zx_simd_saxpy_f32(float *d, const float *a, const float *b, float s, size_t n) {
    zx_sync();
    p_saxpy_f32(d, a, b, s, n);
}
void zx_simd_lerp_f32(float *d, const float *a, const float *b, float t, size_t n) {
    zx_sync();
    p_lerp_f32(d, a, b, t, n);
}
void zx_simd_clamp_f32(float *d, const float *a, float lo, float hi, size_t n) {
    zx_sync();
    p_clamp_f32(d, a, lo, hi, n);
}
void zx_simd_abs_f32(float *d, const float *a, size_t n) {
    zx_sync();
    p_abs_f32(d, a, n);
}
void zx_simd_fill_f32(float *d, float v, size_t n) {
    zx_sync();
    p_fill_f32(d, v, n);
}
void zx_simd_add_i32(int32_t *d, const int32_t *a, const int32_t *b, size_t n) {
    zx_sync();
    p_add_i32(d, a, b, n);
}
void zx_simd_sub_i32(int32_t *d, const int32_t *a, const int32_t *b, size_t n) {
    zx_sync();
    p_sub_i32(d, a, b, n);
}
void zx_simd_fill_i32(int32_t *d, int32_t v, size_t n) {
    zx_sync();
    p_fill_i32(d, v, n);
}
float zx_simd_dot_f32(const float *a, const float *b, size_t n) {
    zx_sync();
    return p_dot_f32(a, b, n);
}
float zx_simd_sum_f32(const float *a, size_t n) {
    zx_sync();
    return p_sum_f32(a, n);
}
