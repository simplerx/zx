/* csrc.h — vendor/ 下 C 源码的自动编译（`zxc include` / `zxc build` / `zxc cbuild` 共用）。
 *
 * 存在的理由：C 静态库必须和 zxc 生成的 C 用**同一套 cc + flags**编译。
 * 手工用别的命令编（哪怕只差 -fno-sanitize=undefined），链接时就会缺
 * __ubsan_handle_* 之类的运行时符号。把这一步收进 zxc，用户不用记参数。
 */
#ifndef ZX_CSRC_H
#define ZX_CSRC_H

#include <stddef.h>

/* 扫描 dir 下的 *.c，按同名的 *.h 分组，增量编译并归档成 dir/lib<group>.a。
 *
 *   cc / cflags   与 zx 生成 C 完全相同的编译器与参数（调用方传 g_cc / g_cflags）
 *   cache         中间文件目录（工程里是 .zxc-cache），.o / .flags / .log 都落这里
 *   only_group    非 NULL 时只处理这一组（zxc include 只编当前头文件对应的库）
 *   force         忽略时间戳全量重编
 *   outlibs       成功时返回 malloc 的路径数组（每个元素也要 free），无 C 源码时为 NULL
 *   err           失败原因（ASCII）
 *   ncompiled     本次真正重编的 .c 条数（可为 NULL）
 *
 * 返回生成的静态库数量；-1 = 编译/归档失败。
 * 分组规则：<stem>.c 若存在 <stem>.h 自成一组；否则按最后一个 '_' 往前找，
 * 命中 <prefix>.h 就归入该组（mini_extra.c -> libmini.a）；都不命中自成一组。
 */
int csrc_build(const char *cc, const char *cflags, const char *dir,
               const char *cache, const char *only_group, int force,
               char ***outlibs, char *err, size_t errcap, int *ncompiled);

#endif
