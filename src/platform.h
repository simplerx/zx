/* platform.h —— zx 多平台目标抽象（§208）。
 *
 * 设计：用户只在 zx.json 里写目标名（如 "linux-x86_64"），本层把名字
 * 解析成一条完整的构建配置：编译器 target triple、产物后缀、默认链接库、
 * 平台族。后端生成的 C 靠 `_WIN32` 宏自动切分支，所以换 triple 就能交叉
 * 编出对应平台的可执行文件；std 的平台差异靠生成的 std 镜像（ZX_STD）。
 *
 * 无平台抽象之前，main.c 里到处散着 #ifdef _WIN32 与写死的 .exe/zig.exe；
 * 这里把所有"目标相关"的数据集中成一张表，驱动层只读表、不写死。
 */
#ifndef ZX_PLATFORM_H
#define ZX_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>

/* 内置目标预设。名字 = zx.json "targets" 数组里写的字符串。 */
typedef struct {
    const char *name;        /* "linux-x86_64" */
    const char *os;          /* "windows" | "linux" | "macos" */
    const char *arch;        /* "x86_64" | "aarch64" */
    const char *family;      /* "windows" | "posix" */
    const char *triple;      /* zig cc --target= 的值 */
    const char *exe_suffix;  /* ".exe" 或 "" */
    const char *dll_suffix;  /* ".dll" | ".so" | ".dylib" */
    const char *lib_prefix;  /* "" (win) 或 "lib" (posix) */
    const char *lib_suffix;  /* ".lib" | ".a" */
    const char *default_libs;/* 默认附加链接参数，如 "-lm -ldl -lpthread" */
} PlatPreset;

/* 解析后的运行时目标（含 zx.json 覆盖项）。 */
typedef struct {
    char name[64];
    char os[16];
    char arch[16];
    char family[16];
    char triple[64];
    char exe_suffix[8];
    char dll_suffix[8];
    char lib_prefix[8];
    char lib_suffix[8];
    char cc_target[128];     /* "--target=<triple>"，native 时为空串 */
    char default_libs[256];  /* 平台默认链接参数 */
    char extra_cflags[256];  /* targetOverrides.<name>.cflags */
    char extra_link[256];    /* targetOverrides.<name>.link，如 "-lfoo" */
    bool valid;
} Target;

/* 按名字找内置预设，找不到返回 NULL。 */
const PlatPreset *plat_find(const char *name);

/* 按索引遍历预设（i=0.. 直到返回 NULL）。 */
const PlatPreset *plat_at(int i);

/* 宿主默认目标名：_WIN32 / __APPLE__ / __linux__ × 指针宽度。
 * 编译 zxc 用的是 zig cc 默认 native target，这些宏按宿主定义。 */
const char *plat_host_name(void);

/* 由名字初始化 Target（不含 overrides，调用方随后填 extra_*）。 */
bool target_init(Target *t, const char *name);

/* 是否交叉编译（triple 与宿主不同）。用于提示与缓存隔离。 */
bool target_is_cross(const Target *t);

#endif
