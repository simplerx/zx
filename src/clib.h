/* clib.h — C 库「目录树模式」：不复制文件，指一个库根目录就够。
 *
 * 背景：真实 C 库几乎都是 include/ 放头、src/ 放下多层源码，甚至还带
 * tests/ examples/ 之类的干扰目录。要求用户把这些文件摊平复制到 vendor/
 * 一层既费心又容易漏，和 `zxc add` 的初衷相悖。
 *
 * 做法：递归扫一遍库**原本的**目录树，把发现结果写成一张可读可改的
 * vendor/<name>.clib 清单（root/include/source/define 行）。库文件一个都不动，
 * 之后 add/build/cbuild 全按清单编译。觉得自动发现多选了，注释掉那行即可。
 *
 * 清单不是缓存，是给用户看的、可编辑的工程文件，所以要进版本库。
 */
#ifndef ZX_CLIB_H
#define ZX_CLIB_H

#include <stddef.h>

#define CLIB_MAX_PATH 1024

typedef struct {
    char name[256];              /* 清单文件名（也是 module 名 / lib<name>.a 名） */
    char root[CLIB_MAX_PATH];    /* 库根目录，按用户给的写法原样保存 */
    char **inc;  int ninc;       /* 所有含 .h 的目录 -> 全部作为 -I */
    char **hdr;  int nhdr;       /* 所有公开头 -> 生成伞头 + 扫宏 */
    char **src;  int nsrc;       /* 所有 .c（已排除 tests/examples/...） */
    char **def;  int ndef;       /* 额外的 -D 宏 */
} ClibManifest;

/* 递归发现。root 可以是相对路径或绝对路径；name 为 NULL 时从目录名推。 */
int  clib_discover(const char *root, const char *name, ClibManifest *m,
                   char *err, size_t errcap);
void clib_free(ClibManifest *m);

/* 清单读写。root_as_written 写进 `root =` 行（真实的库路径）；
 * regen_arg 写进头部注释里的重生成命令（含 --name 等选项）。 */
int  clib_write_manifest(const char *path, const ClibManifest *m,
                         const char *root_as_written, const char *regen_arg,
                         char *err, size_t errcap);
int  clib_read_manifest(const char *path, ClibManifest *m,
                        char *err, size_t errcap);

/* 拼好的 `-I"..." -I"..."`（含 define 的 -D）。返回 malloc 字符串。 */
char *clib_inc_flags(const ClibManifest *m);

/* 多公开头时生成伞头（逐个 #include 绝对路径），单头时直接复用原头。
 * outpath 里返回**实际该交给 cbind 的头文件路径**。 */
int  clib_umbrella(const ClibManifest *m, const char *cache,
                   char *outpath, size_t cap, char *err, size_t errcap);

/* 按清单编译：所有 .c -> 缓存里的 .o -> vendor/lib<name>.a。
 * 增量判据 = cc + flags + 清单 + 所有源文件/头文件的**内容指纹**（FNV-1a），
 * 不看 mtime。outlib 返回静态库路径；ncompiled 是本次真正重编的 .c 数。 */
int  clib_build(const char *cc, const char *cflags, const char *manifest_path,
                const char *cache, int force, char *outlib, size_t outcap,
                char *err, size_t errcap, int *ncompiled);

/* 列出 dir 下的 *.clib 清单（返回 malloc 的字符串数组，每项也要 free）。 */
char **clib_scan_manifests(const char *dir, int *nout);

#endif
