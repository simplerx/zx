/* clib.c — C 库目录树模式：自动发现 + 可编辑清单 + 按清单增量编译。
 *
 * 和 csrc.c 的分工：csrc.c 管「vendor/ 一层、头源同目录」的简单约定；
 * 本文件管「库原本长什么样就怎么用」——include/ 与 src/ 分离、多层子目录、
 * 库目录甚至可以在工程外。库文件一个都不复制。
 *
 * 发现规则（可预测比聪明更重要）：
 *  - 递归收集所有 .h：其所在目录全部进 -I，文件本身进伞头 / 宏扫描；
 *  - 递归收集所有 .c，跳过 tests/examples/build/docs 等目录和异平台源码目录；
 *  - 结果写成 vendor/<name>.clib，用户可以直接注释/删行来裁剪。
 *
 * 编译：所有 .c 合成**一个** lib<name>.a。真实 C 库里多个 .c 互相引用，
 * 按文件分组会在链接时缺符号。.o 落在 .zxc-cache/clib/，用路径哈希命名，
 * 不同子目录的同名 .c 不会撞车。
 */
#include "clib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

#define CPATH    CLIB_MAX_PATH
#define MAXENTRY 1024

/* ---------------- 通用小工具 ---------------- */

static int fexists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static int is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && (st.st_mode & S_IFDIR) != 0;
}

static void slash(char *s) {
    for (char *p = s; *p; p++) if (*p == '\\') *p = '/';
}

static const char *base_of(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) if (*q == '/' || *q == '\\') b = q + 1;
    return b;
}

static void stem_of(const char *p, char *out, size_t cap) {
    snprintf(out, cap, "%s", base_of(p));
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
}

static int mkdir_p(const char *path) {
    char tmp[CPATH];
    snprintf(tmp, sizeof tmp, "%s", path);
    slash(tmp);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
#ifdef _WIN32
            if (_mkdir(tmp) != 0 && errno != EEXIST) return -1;
#else
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
#endif
            *p = '/';
        }
    }
#ifdef _WIN32
    if (_mkdir(tmp) != 0 && errno != EEXIST) return -1;
#else
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
#endif
    return 0;
}

static char *dupstr(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static int eq_nocase(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

/* 自动排除的目录：测试、示例、构建产物、文档、版本库元数据。
 * 这些都是「编进去多半出重复 main / 缺依赖」的来源，但排除规则必须看得见：
 * 写清单时会把命中的目录列在注释里。 */
static int skip_dir(const char *name) {
    if (name[0] == '.') return 1;
    static const char *bad[] = {
        "test", "tests", "testing", "unittest", "unittests", "gtest",
        "example", "examples", "sample", "samples", "demo", "demos",
        "bench", "benchmark", "benchmarks", "build", "builds", "out",
        "doc", "docs", "man", "cmake", "m4", "fuzz", "fuzzing",
        "cmake-build-debug", "cmake-build-release", NULL
    };
    for (int i = 0; bad[i]; i++) if (eq_nocase(name, bad[i])) return 1;
    /* 异平台源码目录：Windows 下不编 posix/，POSIX 下不编 win32/。 */
#ifdef _WIN32
    static const char *other[] = { "posix", "unix", "linux", "darwin", "macos", "apple", NULL };
#else
    static const char *other[] = { "win32", "windows", "msvc", "mingw", "winsock", NULL };
#endif
    for (int i = 0; other[i]; i++) if (eq_nocase(name, other[i])) return 1;
    return 0;
}

static int has_ext(const char *file, const char *ext) {
    size_t l = strlen(file), e = strlen(ext);
    return l >= e && eq_nocase(file + l - e, ext);
}

/* ---------------- 动态字符串数组 ---------------- */

static void push(char ***arr, int *n, const char *s) {
    if (*n >= MAXENTRY) return;
    for (int i = 0; i < *n; i++) if (!strcmp((*arr)[i], s)) return;  /* 去重 */
    *arr = realloc(*arr, (size_t)(*n + 1) * sizeof **arr);
    if (!*arr) return;
    (*arr)[(*n)++] = dupstr(s);
}

static void sort_strings(char **arr, int n) {
    for (int i = 1; i < n; i++) {
        char *v = arr[i];
        int j = i - 1;
        while (j >= 0 && strcmp(arr[j], v) > 0) { arr[j + 1] = arr[j]; j--; }
        arr[j + 1] = v;
    }
}

void clib_free(ClibManifest *m) {
    if (!m) return;
    char **sets[] = { m->inc, m->hdr, m->src, m->def };
    int    ns[]   = { m->ninc, m->nhdr, m->nsrc, m->ndef };
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < ns[k]; i++) free(sets[k][i]);
        free(sets[k]);
    }
    memset(m, 0, sizeof *m);
}

/* ---------------- 递归发现 ---------------- */

typedef struct {
    ClibManifest *m;
    int skipped;
} WalkCtx;

static void walk(const char *root, const char *rel, WalkCtx *w, int depth) {
    if (depth > 12) return;                    /* 防止符号链接/异常深目录 */
    char absdir[CPATH];
    if (rel[0]) snprintf(absdir, sizeof absdir, "%s/%s", root, rel);
    else snprintf(absdir, sizeof absdir, "%s", root);

#ifdef _WIN32
    char pat[CPATH];
    snprintf(pat, sizeof pat, "%s/*", absdir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    for (int go = 1; go; go = FindNextFileA(h, &fd)) {
        if (fd.cFileName[0] == '.') continue;
        const char *name = fd.cFileName;
        int isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    DIR *d = opendir(absdir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        const char *name = e->d_name;
        char probe[CPATH];
        if (rel[0]) snprintf(probe, sizeof probe, "%s/%s/%s", root, rel, name);
        else snprintf(probe, sizeof probe, "%s/%s", root, name);
        int isdir = is_dir(probe);
#endif
        char child[CPATH], childrel[CPATH];
        if (rel[0]) {
            snprintf(child, sizeof child, "%s/%s/%s", root, rel, name);
            snprintf(childrel, sizeof childrel, "%s/%s", rel, name);
        } else {
            snprintf(child, sizeof child, "%s/%s", root, name);
            snprintf(childrel, sizeof childrel, "%s", name);
        }

        if (isdir) {
            if (skip_dir(name)) { w->skipped++; continue; }
            walk(root, childrel, w, depth + 1);
        } else if (has_ext(name, ".h")) {
            push(&w->m->hdr, &w->m->nhdr, child);
            /* 该头所在目录进 -I：库内部 `#include "util.h"` 也能找到。 */
            char idir[CPATH];
            snprintf(idir, sizeof idir, "%s", child);
            char *last = strrchr(idir, '/');
            if (last) *last = 0;
            push(&w->m->inc, &w->m->ninc, idir);
        } else if (has_ext(name, ".c")) {
            push(&w->m->src, &w->m->nsrc, child);
        }
#ifdef _WIN32
    }
    FindClose(h);
#else
    }
    closedir(d);
#endif
}

/* 常见的 include 根：库把公开头放在 include/，内部头在 src/，
 * 两边都要能被 #include <foo/bar.h> 找到。把 root/include 与 root 补进 -I。 */
static void add_include_roots(ClibManifest *m) {
    char buf[CPATH];
    push(&m->inc, &m->ninc, m->root);
    static const char *subs[] = { "include", "inc", "public", "src", NULL };
    for (int i = 0; subs[i]; i++) {
        snprintf(buf, sizeof buf, "%s/%s", m->root, subs[i]);
        if (is_dir(buf)) push(&m->inc, &m->ninc, buf);
    }
}

int clib_discover(const char *root, const char *name, ClibManifest *m,
                  char *err, size_t errcap) {
    memset(m, 0, sizeof *m);
    if (err && errcap) err[0] = 0;
    if (!root || !*root || !is_dir(root)) {
        snprintf(err, errcap, "'%s' is not a directory", root ? root : "(null)");
        return -1;
    }
    snprintf(m->root, sizeof m->root, "%s", root);
    slash(m->root);
    /* 去掉尾部斜杠，避免拼出 "//" */
    size_t rl = strlen(m->root);
    while (rl > 1 && m->root[rl - 1] == '/') m->root[--rl] = 0;

    if (name && *name) snprintf(m->name, sizeof m->name, "%s", name);
    else {
        char b[256];
        snprintf(b, sizeof b, "%s", base_of(m->root));
        /* foo-src / foo_src / foo-master -> foo */
        char *cut = strstr(b, "-src");
        if (!cut) cut = strstr(b, "_src");
        if (!cut) cut = strstr(b, "-master");
        if (!cut) cut = strstr(b, "-main");
        if (cut) *cut = 0;
        for (char *q = b; *q; q++) {
            int ok = isalnum((unsigned char)*q) || *q == '_';
            if (!ok) *q = '_';
        }
        snprintf(m->name, sizeof m->name, "%s", b[0] ? b : "clib");
    }

    WalkCtx w = { m, 0 };
    walk(m->root, "", &w, 0);
    add_include_roots(m);
    sort_strings(m->inc, m->ninc);
    sort_strings(m->hdr, m->nhdr);
    sort_strings(m->src, m->nsrc);
    if (!m->nhdr) {
        snprintf(err, errcap, "no .h files found under '%s'", root);
        return -1;
    }
    return 0;
}

/* ---------------- 清单读写 ---------------- */

int clib_write_manifest(const char *path, const ClibManifest *m,
                        const char *root_as_written, const char *regen_arg,
                        char *err, size_t errcap) {
    FILE *f = fopen(path, "wb");
    if (!f) { snprintf(err, errcap, "cannot write '%s'", path); return -1; }
    const char *root = root_as_written ? root_as_written : m->root;
    const char *regen = (regen_arg && *regen_arg) ? regen_arg : root;
    fprintf(f,
        "# zxc C library manifest\n"
        "# generated by: zxc add %s\n"
        "#\n"
        "# The library files stay where they are -- nothing was copied.\n"
        "# This list IS the build input: comment out or delete any `source =`\n"
        "# line you do not want compiled. Added a new .c later? Run\n"
        "#   zxc add %s --force\n"
        "# to refresh, or add a `source =` line by hand.\n"
        "# `include = DIR` becomes -I\"DIR\"; `define = NAME` becomes -DNAME.\n"
        "# `header =` lines feed binding generation; headers under src/ are\n"
        "# kept for -I only and do not appear in the public Zx module.\n"
        "#\n",
        regen, regen);
    fprintf(f, "root = %s\n\n", root);
    fprintf(f, "# --- include search paths ---\n");
    for (int i = 0; i < m->ninc; i++) fprintf(f, "include = %s\n", m->inc[i]);
    fprintf(f, "\n# --- headers (binding generation) ---\n");
    for (int i = 0; i < m->nhdr; i++) fprintf(f, "header = %s\n", m->hdr[i]);
    fprintf(f, "\n# --- C sources to compile ---\n");
    for (int i = 0; i < m->nsrc; i++) fprintf(f, "source = %s\n", m->src[i]);
    fprintf(f, "\n# define = SOME_FEATURE_FLAG\n");
    fclose(f);
    return 0;
}

static void trim(char *s) {
    char *a = s;
    while (*a == ' ' || *a == '\t' || *a == '\r' || *a == '\n') a++;
    if (a != s) memmove(s, a, strlen(a) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                 s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
}

int clib_read_manifest(const char *path, ClibManifest *m,
                       char *err, size_t errcap) {
    memset(m, 0, sizeof *m);
    stem_of(path, m->name, sizeof m->name);
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errcap, "cannot read '%s'", path); return -1; }
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\r' || *s == '\n' || !*s) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *val = eq + 1;
        trim(s); trim(val);
        if (!*val) continue;
        if      (!strcmp(s, "root"))    snprintf(m->root, sizeof m->root, "%s", val);
        else if (!strcmp(s, "include")) push(&m->inc,  &m->ninc,  val);
        else if (!strcmp(s, "header"))  push(&m->hdr,  &m->nhdr,  val);
        else if (!strcmp(s, "source"))  push(&m->src,  &m->nsrc,  val);
        else if (!strcmp(s, "define"))  push(&m->def,  &m->ndef,  val);
    }
    fclose(f);
    /* 纯头文件库是合法的（所有实现都在头里 / 只用系统库）：允许 0 个 source，
     * 由 clib_build 直接返回「没有静态库」，不要当成清单损坏。 */
    return 0;
}

char *clib_inc_flags(const ClibManifest *m) {
    size_t cap = 256;
    for (int i = 0; i < m->ninc; i++) cap += strlen(m->inc[i]) + 16;
    for (int i = 0; i < m->ndef; i++) cap += strlen(m->def[i]) + 16;
    char *s = malloc(cap);
    if (!s) return NULL;
    size_t n = 0;
    s[0] = 0;
    for (int i = 0; i < m->ninc; i++) {
        int k = snprintf(s + n, cap - n, " -I\"%s\"", m->inc[i]);
        if (k > 0) n += (size_t)k;
    }
    for (int i = 0; i < m->ndef; i++) {
        int k = snprintf(s + n, cap - n, " -D%s", m->def[i]);
        if (k > 0) n += (size_t)k;
    }
    return s;
}

/* ---------------- 伞头 ---------------- */

static void abs_path(const char *p, char *out, size_t cap) {
#ifdef _WIN32
    char buf[CPATH];
    if (_fullpath(buf, p, sizeof buf)) snprintf(out, cap, "%s", buf);
    else snprintf(out, cap, "%s", p);
#else
    char buf[CPATH];
    if (realpath(p, buf)) snprintf(out, cap, "%s", buf);
    else snprintf(out, cap, "%s", p);
#endif
    slash(out);
}

/* 头 A 里 `#include "treelib/vec.h"` 时，vec.h 不该再被伞头单独 include：
 * 否则没写 include guard 的头会重复定义（clang 直接报 redefinition），
 * 即使有 guard 也会让 AST 里多出一轮声明。用路径后缀 + 文件名兜底匹配。 */
static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 256 * 1024) { fclose(f); return NULL; }
    char *s = malloc((size_t)n + 1);
    if (!s) { fclose(f); return NULL; }
    size_t got = fread(s, 1, (size_t)n, f);
    s[got] = 0;
    fclose(f);
    return s;
}

static int header_includes(const char *from, const char *candidate) {
    char *text = read_text(from);
    if (!text) return 0;
    char cand[CPATH];
    snprintf(cand, sizeof cand, "%s", candidate);
    slash(cand);
    const char *cb = base_of(cand);
    int found = 0;
    for (char *p = text; *p && !found; ) {
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        char *s = p;
        while (*s == ' ' || *s == '\t') s++;
        if (!strncmp(s, "#include", 8)) {
            s += 8;
            while (*s == ' ' || *s == '\t') s++;
            char open = *s;
            if (open == '"' || open == '<') {
                char close = open == '"' ? '"' : '>';
                const char *a = s + 1, *b = strchr(a, close);
                if (b && b > a) {
                    char inc[512];
                    size_t l = (size_t)(b - a);
                    if (l >= sizeof inc) l = sizeof inc - 1;
                    memcpy(inc, a, l); inc[l] = 0;
                    slash(inc);
                    size_t cl = strlen(cand), il = strlen(inc);
                    if (cl >= il && !strcmp(cand + cl - il, inc)) found = 1;
                    else if (!strcmp(cb, base_of(inc))) found = 1;
                }
            }
        }
        if (!e) break;
        p = e + 1;
    }
    free(text);
    return found;
}

/* 真实库里 include/ 是公开 API，src/ 下的 .h 是实现细节（常带平台相关
 * 声明）。它们要进 -I 参与编译，但不该出现在对外绑定里。库若根本没有
 * include/ 布局（扁平目录），则所有头都当公开头。 */
static int is_public_header(const ClibManifest *m, const char *p, int any_inc) {
    if (strstr(p, "/include/") || strstr(p, "/inc/") || strstr(p, "/public/")) return 1;
    if (!any_inc) return 1;
    size_t rl = strlen(m->root);
    if (!strncmp(p, m->root, rl) && p[rl] == '/' && !strchr(p + rl + 1, '/')) return 1;
    return 0;
}

int clib_umbrella(const ClibManifest *m, const char *cache,
                  char *outpath, size_t cap, char *err, size_t errcap) {
    int any_inc = 0;
    for (int i = 0; i < m->nhdr; i++)
        if (strstr(m->hdr[i], "/include/") || strstr(m->hdr[i], "/inc/") ||
            strstr(m->hdr[i], "/public/")) { any_inc = 1; break; }

    char **pub = NULL;
    int npub = 0;
    for (int i = 0; i < m->nhdr; i++)
        if (is_public_header(m, m->hdr[i], any_inc)) push(&pub, &npub, m->hdr[i]);
    if (!npub) {                       /* 全被判成内部头：退回全部，宁多勿缺 */
        for (int i = 0; i < m->nhdr; i++) push(&pub, &npub, m->hdr[i]);
    }

    /* 只留「入口头」：被别的公开头 #include 过的头不再单独进伞头。
     * 头文件没写 include guard 时，重复 include 会让 clang 直接报
     * redefinition；真实库里这种散装头并不少见。 */
    char *redun = calloc((size_t)npub, 1);
    if (redun)
        for (int i = 0; i < npub; i++)
            for (int j = 0; j < npub; j++)
                if (i != j && !redun[j] && header_includes(pub[i], pub[j]))
                    redun[j] = 1;
    char **entry = NULL;
    int nentry = 0;
    for (int i = 0; i < npub; i++)
        if (!redun || !redun[i]) push(&entry, &nentry, pub[i]);
    if (!nentry) push(&entry, &nentry, pub[0]);
    free(redun);
    for (int i = 0; i < npub; i++) free(pub[i]);
    free(pub);

    if (nentry == 1) {
        snprintf(outpath, cap, "%s", entry[0]);
        for (int i = 0; i < nentry; i++) free(entry[i]);
        free(entry);
        return 0;
    }
    char dir[CPATH];
    snprintf(dir, sizeof dir, "%s/clib", cache);
    if (mkdir_p(dir) != 0) { snprintf(err, errcap, "cannot create '%s'", dir); return -1; }
    char path[CPATH];
    snprintf(path, sizeof path, "%s/%s.h", dir, m->name);
    FILE *f = fopen(path, "wb");
    if (!f) { snprintf(err, errcap, "cannot write '%s'", path); return -1; }
    fprintf(f, "/* zxc umbrella header -- generated, do not edit. */\n");
    for (int i = 0; i < nentry; i++) {
        char ap[CPATH];
        abs_path(entry[i], ap, sizeof ap);
        fprintf(f, "#include \"%s\"\n", ap);
    }
    fclose(f);
    for (int i = 0; i < nentry; i++) free(entry[i]);
    free(entry);
    snprintf(outpath, cap, "%s", path);
    return 0;
}

/* ---------------- 内容指纹 ---------------- */

static unsigned long long hb(unsigned long long h, unsigned char c) {
    return (h ^ c) * 1099511628211ULL;
}
static unsigned long long hs(unsigned long long h, const char *s) {
    for (; *s; s++) h = hb(h, (unsigned char)*s);
    return h;
}
static unsigned long long hf(unsigned long long h, const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return hb(h, 0xff);
    unsigned char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        for (size_t i = 0; i < n; i++) h = hb(h, buf[i]);
    fclose(f);
    return h;
}

static int text_same(const char *path, const char *text) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[512];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return !strcmp(buf, text);
}
static void write_text(const char *path, const char *s) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(s, 1, strlen(s), f);
    fclose(f);
}

static void log_first_line(const char *path, char *out, size_t cap) {
    out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[2048];
    if (!fgets(line, sizeof line, f)) { fclose(f); return; }
    fclose(f);
    size_t k = 0;
    for (size_t i = 0; line[i] && line[i] != '\n' && line[i] != '\r' && k + 1 < cap; i++) {
        unsigned char c = (unsigned char)line[i];
        out[k++] = (c < 0x80 && c >= 0x20) ? (char)c : '?';
    }
    out[k] = 0;
}

static const char *archiver_of(const char *cc) {
    static char buf[CPATH];
    size_t l = strlen(cc);
    if (l > 3 && !strcmp(cc + l - 3, " cc")) {
        snprintf(buf, sizeof buf, "%.*s ar", (int)(l - 3), cc);
        return buf;
    }
    return "ar";
}

static int run_logged(const char *cmd, const char *log, char *err, size_t errcap) {
    char full[CPATH * 8];
    snprintf(full, sizeof full, "%s > \"%s\" 2>&1", cmd, log);
    if (system(full) == 0) { remove(log); return 0; }
    char detail[512];
    log_first_line(log, detail, sizeof detail);
    snprintf(err, errcap, "C compile failed%s%s (see %s)",
             detail[0] ? ": " : "", detail, log);
    return -1;
}

/* .o 名字用「路径哈希 + 文件名」：多层目录下的同名 .c 不会互相覆盖。 */
static void obj_name(const ClibManifest *m, const char *src, char *out, size_t cap) {
    char stem[256];
    stem_of(src, stem, sizeof stem);
    unsigned long long h = hf(1469598103934665603ULL, src);
    snprintf(out, cap, "%s_%016llx_%s.o", m->name, h & 0x7fffffffffffULL, stem);
}

/* ---------------- 按清单编译 ---------------- */

int clib_build(const char *cc, const char *cflags, const char *manifest_path,
               const char *cache, int force, char *outlib, size_t outcap,
               char *err, size_t errcap, int *ncompiled) {
    if (ncompiled) *ncompiled = 0;
    if (err && errcap) err[0] = 0;
    if (!cc || !*cc) { snprintf(err, errcap, "no C compiler configured"); return -1; }
    if (!cflags) cflags = "";

    ClibManifest m;
    if (clib_read_manifest(manifest_path, &m, err, errcap) != 0) {
        clib_free(&m);
        return -1;
    }
    if (m.nsrc == 0) {           /* header-only 库：不产出 .a */
        if (outlib && outcap) outlib[0] = 0;
        clib_free(&m);
        return 0;
    }
    char objdir[CPATH];
    snprintf(objdir, sizeof objdir, "%s/clib", cache);
    if (mkdir_p(objdir) != 0) {
        snprintf(err, errcap, "cannot create '%s'", objdir);
        clib_free(&m); return -1;
    }

    /* 静态库仍落在 vendor/：链接路径稳定，也不污染库原本的目录。 */
    char manifest_dir[CPATH];
    snprintf(manifest_dir, sizeof manifest_dir, "%s", manifest_path);
    char *sl = strrchr(manifest_dir, '/');
    if (sl) *sl = 0; else snprintf(manifest_dir, sizeof manifest_dir, "vendor");
    char libpath[CPATH];
    snprintf(libpath, sizeof libpath, "%s/lib%s.a", manifest_dir, m.name);
    snprintf(outlib, outcap, "%s", libpath);

    /* 指纹 = cc + flags + 清单本身 + 所有源文件 + 所有头文件的内容。
     * 清单被手工编辑过（增删 source/include/define）也会触发重编。 */
    unsigned long long h = 1469598103934665603ULL;
    h = hs(h, cc); h = hs(h, cflags);
    h = hf(h, manifest_path);
    for (int i = 0; i < m.nsrc; i++) h = hf(h, m.src[i]);
    for (int i = 0; i < m.nhdr; i++) h = hf(h, m.hdr[i]);
    char stamp[64], flagpath[CPATH];
    snprintf(stamp, sizeof stamp, "%016llx\n", h);
    snprintf(flagpath, sizeof flagpath, "%s/%s.flags", objdir, m.name);

    int dirty = force || !fexists(libpath) || !text_same(flagpath, stamp);
    for (int i = 0; i < m.nsrc && !dirty; i++) {
        char on[CPATH], op[CPATH];
        obj_name(&m, m.src[i], on, sizeof on);
        snprintf(op, sizeof op, "%s/%s", objdir, on);
        if (!fexists(op)) dirty = 1;
    }

    if (dirty) {
        char *incs = clib_inc_flags(&m);
        if (!incs) { snprintf(err, errcap, "out of memory"); clib_free(&m); return -1; }
        /* 先逐条编，任一失败立刻停下并保留日志。 */
        for (int i = 0; i < m.nsrc; i++) {
            char on[CPATH], op[CPATH], logp[CPATH], cmd[CPATH * 8];
            obj_name(&m, m.src[i], on, sizeof on);
            snprintf(op, sizeof op, "%s/%s", objdir, on);
            snprintf(logp, sizeof logp, "%s/%s.log", objdir, on);
            snprintf(cmd, sizeof cmd,
                     "%s %s%s -w -fno-caret-diagnostics -fno-color-diagnostics "
                     "-c -o \"%s\" \"%s\"",
                     cc, cflags, incs, op, m.src[i]);
            if (run_logged(cmd, logp, err, errcap) != 0) {
                free(incs); clib_free(&m); return -1;
            }
            if (ncompiled) (*ncompiled)++;
        }
        free(incs);

        remove(libpath);                    /* ar rcs 会保留旧成员 */
        char arcmd[32768];
        snprintf(arcmd, sizeof arcmd, "%s rcs \"%s\"", archiver_of(cc), libpath);
        for (int i = 0; i < m.nsrc; i++) {
            char on[CPATH], op[CPATH], one[CPATH + 8];
            obj_name(&m, m.src[i], on, sizeof on);
            snprintf(op, sizeof op, "%s/%s", objdir, on);
            snprintf(one, sizeof one, " \"%s\"", op);
            size_t used = strlen(arcmd);
            if (used + strlen(one) + 1 >= sizeof arcmd) break;
            memcpy(arcmd + used, one, strlen(one) + 1);
        }
        char arlog[CPATH];
        snprintf(arlog, sizeof arlog, "%s/%s.ar.log", objdir, m.name);
        if (run_logged(arcmd, arlog, err, errcap) != 0) { clib_free(&m); return -1; }
        write_text(flagpath, stamp);
    }
    clib_free(&m);
    return 1;
}

/* ---------------- 枚举清单 ---------------- */

char **clib_scan_manifests(const char *dir, int *nout) {
    *nout = 0;
    char **list = NULL;
    int n = 0;
#ifdef _WIN32
    char pat[CPATH];
    snprintf(pat, sizeof pat, "%s/*.clib", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!has_ext(fd.cFileName, ".clib")) continue;
        char full[CPATH];
        snprintf(full, sizeof full, "%s/%s", dir, fd.cFileName);
        list = realloc(list, (size_t)(n + 1) * sizeof *list);
        list[n++] = dupstr(full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return NULL;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!has_ext(e->d_name, ".clib")) continue;
        char full[CPATH];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        list = realloc(list, (size_t)(n + 1) * sizeof *list);
        list[n++] = dupstr(full);
    }
    closedir(d);
#endif
    sort_strings(list, n);
    *nout = n;
    return list;
}
