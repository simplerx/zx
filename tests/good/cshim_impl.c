/* cshim_impl.c —— @cShim 的守门样本（§160）。
 *
 * 它是一份**自包含的 C 翻译单元**：只 include 自己需要的 C 头，不引用 zx
 * 生成的任何类型或函数。zxc 用与 zx 代码相同的 cc / flags 编译它，产出
 * 独立的 .o 后一起链接——所以它写 C 的规矩就是普通 C 的规矩。
 *
 * 这里同时演示真正的 intrinsic 路径：SSE2 在 x86-64 上是基线，不需要
 * target 属性；实在不是 x86 的机器上退回标量循环，结果完全一致。 */

#include <stddef.h>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(__SSE2__)
#include <emmintrin.h>
#define ZX_SHIM_HAVE_SSE2 1
#else
#define ZX_SHIM_HAVE_SSE2 0
#endif

/* 私有助手：每份 shim 自成 .o，static 名字不会和别人的撞。 */
static float sum_scalar(const float *a, size_t n) {
    float s = 0.0f;
    for (size_t i = 0; i < n; i++) s += a[i];
    return s;
}

float zx_shim_sum_f32(const float *a, size_t n) {
    return sum_scalar(a, n);
}

/* v * 2 而不是 v：证明 C 侧的算术真的执行了，而不是把值原样透传。 */
void zx_shim_fill_i32(int *dst, size_t n, int v) {
    for (size_t i = 0; i < n; i++) dst[i] = v * 2;
}

void zx_shim_add4(float *dst, const float *a, const float *b) {
#if ZX_SHIM_HAVE_SSE2
    __m128 x = _mm_loadu_ps(a);
    __m128 y = _mm_loadu_ps(b);
    _mm_storeu_ps(dst, _mm_add_ps(x, y));
#else
    for (size_t i = 0; i < 4; i++) dst[i] = a[i] + b[i];
#endif
}

int zx_shim_vec_ok(void) {
    return ZX_SHIM_HAVE_SSE2;
}
