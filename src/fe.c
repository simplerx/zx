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
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#endif

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

/* ---- AST 序列化缓存 ----------------------------------------------------
 * Zx 前端每解析一个文件就要起一次 zxfe 子进程；一个普通程序 import std
 * 后是 4~5 个模块 = 4~5 次 spawn，Windows 上 cmd /c 一次就要 ~50ms——
 * zxc --check 0.35s 里 spawn 占了七成。而 std 这些模块的内容在两次编译
 * 之间根本不变，序列化结果也不变。
 *
 * 解法：按「源内容 + 源路径 + zxfe.exe 状态」为键缓存序列化 AST，命中
 * 就直接 ast_load_file，一个进程都不用起。键里放编译器二进制的
 * mtime+size：重编 zxfe（枚举编号一变序列化就不兼容，M41 的教训）自动
 * 全部失效。ZXC_NO_AST_CACHE 置位可整体关闭（调试序列化问题时用）。
 * 缓存装载失败一律退回 spawn 路径，宁可慢不可错。 */

/* FNV-1a 64 位：够散、够快、无状态依赖 */
static unsigned long long fnv1a(const char *data, size_t len,
                                unsigned long long h) {
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* 把一个文件的大小 + mtime 揉进哈希（二进制身份指纹） */
static unsigned long long fnv_file(const char *path, unsigned long long h) {
    struct stat st;
    if (stat(path, &st) != 0) return h ^ 0x9e3779b97f4a7c15ULL;
    h = fnv1a((char *)&st.st_size, sizeof st.st_size, h);
    h = fnv1a((char *)&st.st_mtime, sizeof st.st_mtime, h);
    return h;
}

static void ast_cache_paths(char *cache, size_t csz, const char *tmpdir,
                            const char *path, const char *src, size_t slen) {
    unsigned long long key = 1469598103934665603ULL;
    key = fnv1a(src, slen, key);
    key = fnv1a(path, strlen(path), key);
    /* 编译器两侧的二进制身份：任何一个重编过，旧缓存整体作废 */
    key = fnv_file(g_fe_argv0 ? g_fe_argv0 : "zxc", key);

    char *dir = fe_exe_path();
    if (dir) {
        char exe[2048];
        size_t dl = strlen(dir);
        if (dl && dir[dl - 1] != '\\' && dir[dl - 1] != '/')
            snprintf(exe, sizeof exe, "%s\\zxfe.exe", dir);
        else
            snprintf(exe, sizeof exe, "%szxfe.exe", dir);
        key = fnv_file(exe, key);
        free(dir);
    }

    size_t tl = strlen(tmpdir);
    if (tl > 0 && (tmpdir[tl - 1] == '\\' || tmpdir[tl - 1] == '/'))
        snprintf(cache, csz, "%szxc-ast-%016llx.ast", tmpdir, key);
    else
        snprintf(cache, csz, "%s\\zxc-ast-%016llx.ast", tmpdir, key);
}

/* Zx 前端：先查缓存，未命中起 zxfe 子进程拿序列化 AST，装载成 Node 树 */
static Node *fe_parse_zx(const char *path) {
    /* 缓存路径先行：源读不出来就让 spawn 路径去报错（zxfe 的诊断
     * 带 path:line:col，比我们在这一层模仿更可信） */
    size_t slen = 0;
    char *src = fe_read_source(path, &slen);
    char cache[1400];
    bool caching = src && !getenv("ZXC_NO_AST_CACHE");

    /* 临时目录：优先 TEMP 环境变量（老行为，MSYS 下 TMP 可能是 test.sh
     * 沙箱前缀这类不存在的目录，GetTempPath 会优先吃 TMP 而踩坑），
     * TEMP 缺失时退 GetTempPath（Zed 拉起的进程链里 TEMP 可能缺失，
     * 回退 "." 会把 .zxfe-*.ast 写进项目根，触发文件监视报错刷屏）；
     * 时间/地址/进程号混合键防互踩 */
    char tmpdirbuf[1024];
    const char *tmpdir = getenv("TEMP");
    if (!tmpdir || !*tmpdir) {
#ifdef _WIN32
        DWORD n = GetTempPathA(sizeof tmpdirbuf, tmpdirbuf);
        if (n > 0 && n < sizeof tmpdirbuf) tmpdir = tmpdirbuf;
#endif
    }
    if (!tmpdir || !*tmpdir) {
        const char *t = getenv("TMPDIR");
        tmpdir = (t && *t) ? t : ".";
    }

    if (caching) {
        ast_cache_paths(cache, sizeof cache, tmpdir, path, src, slen);
        free(src);
        Node *hit = ast_load_file(cache);   /* 格式坏/截断 → NULL，走重编 */
        if (hit) {
            if (getenv("ZX_DEBUG_FE")) fprintf(stderr, "[fe] cache hit  %s\n", path);
            return hit;
        }
        if (getenv("ZX_DEBUG_FE")) fprintf(stderr, "[fe] cache miss %s\n", path);
    } else {
        free(src);
    }

    char *dir = fe_exe_path();
    if (!dir) return NULL;

    unsigned long long key = (unsigned long long)time(NULL)
                           ^ (unsigned long long)(uintptr_t)dir;
#ifdef _WIN32
    key ^= (unsigned long long)GetCurrentProcessId() << 32;
#else
    key ^= (unsigned long long)getpid() << 32;
#endif
    /* 临时 .ast 落 zxc 自己的 exe 目录——和 AST 缓存同一优先级，必然可写。
     * 之前落 TEMP：zxls（zig Io 的 spawn）等宿主拉起的进程链里环境可能
     * 整个缺失，GetTempPath 就退到 C:\WINDOWS——不可写，zxfe 写不了
     * 临时文件，zxc --check 静默返回 1，诊断全空（LSP 一直"没错误"）。 */
    char tmp[1200];
    {
        size_t dl = strlen(dir);
        if (dl > 0 && (dir[dl - 1] == '\\' || dir[dl - 1] == '/'))
            snprintf(tmp, sizeof tmp, "%s.zxfe-%llu.ast", dir, key);
        else
            snprintf(tmp, sizeof tmp, "%s\\.zxfe-%llu.ast", dir, key);
    }
    char cmd[2800];
    if (*dir) {
        /* 注意：Windows 的 system() 会让 cmd /c 吃掉首尾引号——命令行
         * 不能以引号开头（与 main.c 里 zig cc 的拼法同一约束）。 */
        snprintf(cmd, sizeof cmd, "%s\\zxfe.exe \"%s\" -o \"%s\"", dir, path, tmp);
    } else {
        snprintf(cmd, sizeof cmd, "zxfe.exe \"%s\" -o \"%s\"", path, tmp);
    }
    free(dir);

    int rc = system(cmd);
    if (rc != 0) return NULL;            /* 诊断已由 zxfe 打到 stderr */

    Node *root = ast_load_file(tmp);
    /* 装载成功才写缓存；rename 顺带把临时文件清掉（同目录，同盘） */
    if (root && caching) {
        remove(cache);
        if (rename(tmp, cache) != 0) { /* 缓存写失败只是慢一点，不是错 */ }
        else tmp[0] = '\0';              /* 已搬走，别再 remove */
    }
    if (tmp[0]) remove(tmp);
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
    lx.fname = path;          /* @file() 按模块捕获自己的源路径 */
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
