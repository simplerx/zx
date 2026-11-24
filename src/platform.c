/* platform.c —— 内置目标预设表与解析（§208）。见 platform.h。 */
#include "platform.h"
#include <string.h>
#include <stdio.h>

/* 预设表。顺序即 zxc targets -h 的展示顺序。
 * default_libs 只放"平台必有"的系统库：
 *   linux: m/dl/pthread —— 后端生成的 C 用到 clock_gettime/dlopen/pthread
 *   macos: m —— 数学库在系统库里，但显式 -lm 无害且兼容
 *   windows: 空 —— Winsock/user32 等由 std.net 的 @link("ws2_32") 提供
 * 注意不要把 libc 已声明的函数当链接样本（后端会排除 libc_declared）。 */
static const PlatPreset k_presets[] = {
    { "windows-x86_64", "windows", "x86_64",  "windows", "x86_64-windows-gnu",  ".exe", ".dll",   "",    ".lib", "" },
    { "windows-aarch64","windows", "aarch64", "windows", "aarch64-windows-gnu", ".exe", ".dll",   "",    ".lib", "" },
    { "linux-x86_64",   "linux",   "x86_64",  "posix",   "x86_64-linux-gnu",     "",     ".so",    "lib", ".a",   "-lm -ldl -lpthread" },
    { "linux-aarch64",  "linux",   "aarch64", "posix",   "aarch64-linux-gnu",    "",     ".so",    "lib", ".a",   "-lm -ldl -lpthread" },
    { "macos-x86_64",   "macos",   "x86_64",  "posix",   "x86_64-macos-none",    "",     ".dylib", "lib", ".a",   "-lm" },
    { "macos-aarch64",  "macos",   "aarch64", "posix",   "aarch64-macos-none",   "",     ".dylib", "lib", ".a",   "-lm" },
};
static const int k_npresets = (int)(sizeof k_presets / sizeof k_presets[0]);

const PlatPreset *plat_find(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < k_npresets; i++)
        if (!strcmp(k_presets[i].name, name)) return &k_presets[i];
    return NULL;
}

const PlatPreset *plat_at(int i) {
    if (i < 0 || i >= k_npresets) return NULL;
    return &k_presets[i];
}

const char *plat_host_name(void) {
#if defined(_WIN32)
#if defined(_M_ARM64) || defined(__aarch64__)
    return "windows-aarch64";
#else
    return "windows-x86_64";
#endif
#elif defined(__APPLE__)
#if defined(__aarch64__)
    return "macos-aarch64";
#else
    return "macos-x86_64";
#endif
#else
#if defined(__aarch64__)
    return "linux-aarch64";
#else
    return "linux-x86_64";
#endif
#endif
}

bool target_init(Target *t, const char *name) {
    memset(t, 0, sizeof *t);
    const PlatPreset *p = plat_find(name);
    if (!p) return false;
    snprintf(t->name, sizeof t->name, "%s", p->name);
    snprintf(t->os, sizeof t->os, "%s", p->os);
    snprintf(t->arch, sizeof t->arch, "%s", p->arch);
    snprintf(t->family, sizeof t->family, "%s", p->family);
    snprintf(t->triple, sizeof t->triple, "%s", p->triple);
    snprintf(t->exe_suffix, sizeof t->exe_suffix, "%s", p->exe_suffix);
    snprintf(t->dll_suffix, sizeof t->dll_suffix, "%s", p->dll_suffix);
    snprintf(t->lib_prefix, sizeof t->lib_prefix, "%s", p->lib_prefix);
    snprintf(t->lib_suffix, sizeof t->lib_suffix, "%s", p->lib_suffix);
    snprintf(t->default_libs, sizeof t->default_libs, "%s", p->default_libs);
    /* §244：quad 只在 x86_64 的 Windows / Linux 上可用（实测 zig cc：
     * linux-x86_64 连 __FLOAT128__ 宏都有，windows-x86_64 无宏但能编译
     * 链接；aarch64-linux、macos-x86_64、macos-aarch64 一律
     * "__float128 is not supported on this target"）。用结构式判据而不是
     * __FLOAT128__ 宏——后者在 windows 上缺失，会误判成不支持。 */
    t->f128 = (strcmp(p->arch, "x86_64") == 0 &&
               (strcmp(p->os, "windows") == 0 || strcmp(p->os, "linux") == 0));
    /* 交叉编译才注入 --target：native 构建不写，避免 zig cc 选到与宿主
     * 不同的 libc 变体而找不到头文件（Windows 仓库默认就是 native）。 */
    if (strcmp(p->name, plat_host_name()) != 0)
        snprintf(t->cc_target, sizeof t->cc_target, "--target=%s", p->triple);
    t->valid = true;
    return true;
}

bool target_is_cross(const Target *t) {
    return t && t->cc_target[0] != 0;
}
