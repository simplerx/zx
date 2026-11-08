/* fetch.h — C 库远端获取：声明（zx.json）+ 锁定（zx.lock）+ 落地 vendor/。
 *
 * 两层分工：
 *   zx.json 的 "vendor" 段  人写的意图：url / git ref / 期望 sha256 / 子目录。
 *                           可 review、可 diff、可手工改。
 *   zx.lock                 机器解析的结果：git 实际 commit、落地文件树 hash、
 *                           落地路径。由 zxc add / fetch 生成，必须提交。
 *
 * 没有版本求解、没有 registry、没有传递依赖。zxc 只负责把远端字节安全地
 * 搬进 vendor/，之后交给既有的 vendor_autoscan + clib/csrc 增量编译链路。
 * `zxc run` 永远不碰网络。
 */
#ifndef ZX_FETCH_H
#define ZX_FETCH_H

#include <stddef.h>
#include <stdio.h>

#define ZX_LOCK_PATH "zx.lock"
#define ZX_FETCH_TMP ".zxc-cache/fetch"

typedef enum { FK_FILE = 0, FK_ARCHIVE, FK_GIT } FetchKind;

typedef struct {
    char name[128];       /* 模块名，也是 vendor 内的落地名 */
    char url[1024];       /* https://...（或 loopback / file:// 测试源） */
    char ref[128];        /* git tag / branch / commit；非 git 源为空 */
    char sha256[65];      /* 声明里期望的下载字节 hash（git 源为空） */
    char subdir[256];     /* 仓库/归档内只取这个子目录 */
    FetchKind kind;
} FetchDecl;

typedef struct {
    FetchDecl d;
    char resolved[128];   /* git 实际 commit；其它源为 "-" */
    char tree[65];        /* 落地文件树的内容 hash */
    char path[512];       /* vendor 内落地路径（文件或目录） */
    int  nfiles;
} FetchLock;

/* 声明：读 zx.json 的 "vendor" 段。返回条目数；-1 = zx.json 读不了。 */
int  fetch_decls_load(FetchDecl **out, char *err, size_t cap);
/* 文本级 upsert：只替换/插入 "vendor" 段，其余字段原样保留（含手写格式）。 */
int  fetch_decl_save(const FetchDecl *d, char *err, size_t cap);
void fetch_decls_free(FetchDecl *v, int n);

/* 锁：返回条目数；-1 = 格式错。 */
int  fetch_lock_load(FetchLock **out, char *err, size_t cap);
int  fetch_lock_save(const FetchLock *v, int n, char *err, size_t cap);
void fetch_lock_free(FetchLock *v, int n);

/* URL 安全校验（scheme 白名单 + 字符白名单，拒绝 userinfo 与路径穿越）。 */
int  fetch_url_check(const char *url, char *err, size_t cap);
/* 源类型推断 + 从 URL 推模块名。 */
int  fetch_kind_of(const char *url, const char *ref, int force_git,
                   FetchKind *k, char *err, size_t cap);
int  fetch_name_of(const char *url, char *out, size_t cap);

/* 拉取并落地。locked 非 NULL 时结果必须与之逐字节一致，否则不落地；
 * overwrite=1 时先把旧目标移到 .zxc-cache/fetch-backup/ 再落地。 */
int  fetch_materialize(const FetchDecl *d, const FetchLock *locked,
                       int overwrite, FetchLock *res, char *err, size_t cap);

/* 单条纯本地校验：0 = 完好，1 = 缺失，2 = 内容被改，-1 = 出错。 */
int  fetch_verify_entry(const FetchLock *l, char *err, size_t cap);
/* 全量纯本地检查，只打印问题，不联网、不写文件。
 * deep=0 只查存在性（build 用），deep=1 逐文件重算 tree hash（fetch 用）。 */
int  fetch_check_local(int deep, FILE *out);

#endif /* ZX_FETCH_H */
