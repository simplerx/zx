/* fe.c — 前端选择：Zx 前端（默认）或 C 前端（--fe=c，交叉验证用）。
 *
 * 自举阶段 3（spec/bootstrap.md）：zxc 默认走 Zx 前端——bootstrap/zxfe.zx
 * 把源文件解析成可装载的序列化 AST（src/astload.c 重建 Node 树）；
 * C 前端留在 --fe=c 开关后面，跑满一个版本周期再删。
 *
 * 诊断路径：Zx 前端的诊断由 zxfe 按同一格式（path:line:col: error[CODE]:
 * text）直接打到 stderr，zxc 只看退出码，不重复打印。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ast.h"
#include "zxc.h"

static bool g_fe_zx = true;
static const char *g_fe_argv0 = NULL;

void fe_set_mode(bool use_zx) { g_fe_zx = use_zx; }
bool fe_is_zx(void) { return g_fe_zx; }
void fe_set_argv0(const char *p) { g_fe_argv0 = p; }

/* zxfe.exe 与 zxc.exe 同目录（bin/），从 argv[0] 推 */
static char *fe_exe_path(void) {
    const char *a = g_fe_argv0 ? g_fe_argv0 : "zxc";
    const char *slash = strrchr(a, '/');
    const char *bslash = strrchr(a, '\\');
    const char *cut = slash > bslash ? slash : bslash;
    size_t dirlen = cut ? (size_t)(cut - a) : 0;
    char *s = malloc(dirlen + 16);
    if (!s) return NULL;
    if (dirlen) memcpy(s, a, dirlen);
    s[dirlen] = '\0';
    return s;   /* 空串 = 当前目录 */
}

/* 读整个文件（C 前端用；Zx 前端由 zxfe 自己读） */
static char *fe_read_source(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    buf[n] = '\0';
    fclose(f);
    if (out_len) *out_len = (size_t)n;
    return buf;
}

/* Zx 前端：起 zxfe 子进程拿序列化 AST，装载成 Node 树 */
static Node *fe_parse_zx(const char *path) {
    char *dir = fe_exe_path();
    if (!dir) return NULL;

    /* 临时文件：TEMP（或当前目录）+ 时间/地址混合键，避免并发互踩 */
    const char *tmpdir = getenv("TEMP");
    if (!tmpdir || !*tmpdir) tmpdir = ".";
    unsigned long long key = (unsigned long long)time(NULL)
                           ^ (unsigned long long)(uintptr_t)dir;
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s\\.zxfe-%llu.ast", tmpdir, key);
    char cmd[2600];
    int rc;
    if (*dir) {
        /* 注意：Windows 的 system() 会让 cmd /c 吃掉首尾引号——命令行
         * 不能以引号开头（与 main.c 里 zig cc 的拼法同一约束）。 */
        snprintf(cmd, sizeof cmd, "%s\\zxfe.exe \"%s\" -o \"%s\"", dir, path, tmp);
    } else {
        snprintf(cmd, sizeof cmd, "zxfe.exe \"%s\" -o \"%s\"", path, tmp);
    }
    free(dir);

    rc = system(cmd);
    if (rc != 0) return NULL;            /* 诊断已由 zxfe 打到 stderr */

    Node *root = ast_load_file(tmp);
    remove(tmp);
    return root;
}

/* C 前端：原有路径（读源 → 词法 → 语法），诊断走 diag 机制 */
static Node *fe_parse_c(const char *path) {
    size_t len = 0;
    char *src = fe_read_source(path, &len);
    if (!src) {
        err_at(1, 1, "cannot open '%s'", path);
        return NULL;
    }
    Lexer lx;
    lexer_init(&lx, src, len);
    lexer_run(&lx);
    if (g_error_count) { free(src); return NULL; }
    Node *root = parse_program(&lx);
    free(src);
    return root;
}

Node *fe_parse_file(const char *path) {
    if (g_fe_zx) return fe_parse_zx(path);
    return fe_parse_c(path);
}
