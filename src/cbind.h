/* cbind.h — C 头文件 → Zx 绑定源码生成器（`zxc add` 的后端）。
 *
 * 只有一份：把 C 头喂给 vendored clang 的文本 AST，抽出函数 / 变量 /
 * struct / union / 枚举常量 / 数字宏，翻成普通的 Zx `extern "C"` 模块。
 * 生成物是**普通 Zx 源码**，可读可改，LSP 的补全、hover、inlayHint 照常生效。
 */
#ifndef ZX_CBIND_H
#define ZX_CBIND_H

#include <stddef.h>

/* 生成一份绑定源码。成功返回 malloc 的字符串（调用方 free），失败返回 NULL。
 *   header   目标 C 头文件路径（或系统头名，由调用方落成临时包装头）
 *   cc       C 编译器命令行（ZX_CC 或缺省的 "<toolchain>/zig.exe cc"）
 *   link_lib 要写进 @link 的库名；NULL / "" 表示不写
 *   allow_system 1 = 目标是系统头（配方给的名字），放行 libc/SDK 里的声明
 *   err      失败原因（ASCII）
 *   nskip    解析不了、被跳过的声明条数（可为 NULL）
 */
char *cbind_generate(const char *header, const char *cc, const char *link_lib,
                     int allow_system, char *err, size_t errcap, int *nskip);

#endif
