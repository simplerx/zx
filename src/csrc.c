/* csrc.c — 把 vendor/ 下的 C 源码编成静态库，供 Zx 绑定直接 @link。
 *
 * 设计要点：
 *  - 只认一层目录里的 *.c，按同名头文件分组（见 csrc.h）。第三方 C 库
 *    通常就是一个头 + 一两个实现文件，约定够用且不猜用户的目录结构。
 *  - 编译参数来自调用方的 g_cc / g_cflags，和 zx 生成的 C **逐字一致**。
 *    这是整个功能的意义所在：不再有"手工编的库 sanitizer 对不上"这种问题。
 *  - 增量：.flags 记 cc + flags + 本组所有 .c/.h 的**内容指纹**，没变且
 *    .o/.a 都在就跳过；换 dev/release 档、改任一源码/头、改编译参数，
 *    整组全量重编（C 库很小，全量比维护头文件依赖图可靠得多）。
 *    不靠 mtime：Windows 秒级时间戳、未来时间戳都会误判。
 *  - 归档前先删旧 .a：ar rcs 是"追加/替换成员"，不删会残留已移除源文件
 *    的旧 .o，链接出莫名其妙的重复符号。
 *  - 全程 ASCII 控制台输出；第三方 C 的诊断只取首行、非 ASCII 字节过滤掉。
 */
#include "csrc.h"
#include "zxc.h"        /* §232：xmalloc / xrealloc */
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

#define CPATH   1024
#define MAXGRP  64
#define MAXFILE 256

typedef struct {
    char name[256];
    char *files[MAXFILE];
    int nfiles;
} Group;

static Group g_groups[MAXGRP];
static int   g_ngroups;

static int fexists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static void base_of(const char *p, char *out, size_t cap) {
    const char *b = p;
    for (const char *q = p; *q; q++)
        if (*q == '/' || *q == '\\') b = q + 1;
    snprintf(out, cap, "%s", b);
}

static void stem_of(const char *p, char *out, size_t cap) {
    base_of(p, out, cap);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
}

static int mkdir_one(const char *d) {
#ifdef _WIN32
    if (_mkdir(d) != 0 && errno != EEXIST) return -1;
#else
    if (mkdir(d, 0755) != 0 && errno != EEXIST) return -1;
#endif
    return 0;
}

static int mkdir_p(const char *path) {
    char tmp[CPATH];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp; *p; p++) if (*p == '\\') *p = '/';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir_one(tmp) != 0) return -1;
            *p = '/';
        }
    }
    return mkdir_one(tmp);
}

/* 收集 dir 下指定后缀的文件，返回 malloc 的字符串数组（每项 strdup 全路径）。 */
static char **collect_ext(const char *dir, const char *ext, int *out_n) {
    *out_n = 0;
    char **list = NULL;
    int n = 0, cap = 0;
#ifdef _WIN32
    char pat[CPATH];
    snprintf(pat, sizeof pat, "%s/*%s", dir, ext);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (fd.cFileName[0] == '.') continue;
        size_t l = strlen(fd.cFileName), el = strlen(ext);
        if (l < el || strcmp(fd.cFileName + l - el, ext) != 0) continue;
        if (n == cap) { cap = cap ? cap * 2 : 16; list = xrealloc(list, cap * sizeof *list); }
        char full[CPATH];
        snprintf(full, sizeof full, "%s/%s", dir, fd.cFileName);
        list[n++] = xstrdup(full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return NULL;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name), el = strlen(ext);
        if (e->d_name[0] == '.' || l < el) continue;
        if (strcmp(e->d_name + l - el, ext) != 0) continue;
        if (n == cap) { cap = cap ? cap * 2 : 16; list = xrealloc(list, cap * sizeof *list); }
        char full[CPATH];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        list[n++] = xstrdup(full);
    }
    closedir(d);
#endif
    *out_n = n;
    return list;
}

static void free_list(char **list, int n) {
    if (!list) return;
    for (int i = 0; i < n; i++) free(list[i]);
    free(list);
}

/* 归档器跟编译器配套：".../zig.exe cc" -> ".../zig.exe ar"。 */
static const char *archiver_of(const char *cc) {
    static char buf[CPATH];
    size_t l = strlen(cc);
    if (l > 3 && !strcmp(cc + l - 3, " cc")) {
        snprintf(buf, sizeof buf, "%.*s ar", (int)(l - 3), cc);
        return buf;
    }
    if (l > 5 && !strcmp(cc + l - 5, " gcc")) {
        snprintf(buf, sizeof buf, "%.*s gcc-ar", (int)(l - 5), cc);
        return buf;
    }
    return "ar";
}

/* <stem>.c 归到哪一组：优先同名 <stem>.h，否则按 '_' 逐级往回找头文件。 */
static void group_of(const char *dir, const char *stem, char *out, size_t cap) {
    char probe[CPATH];
    snprintf(probe, sizeof probe, "%s/%s.h", dir, stem);
    if (fexists(probe)) { snprintf(out, cap, "%s", stem); return; }
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", stem);
    char *us;
    while ((us = strrchr(tmp, '_')) && us > tmp) {
        *us = 0;
        snprintf(probe, sizeof probe, "%s/%s.h", dir, tmp);
        if (fexists(probe)) { snprintf(out, cap, "%s", tmp); return; }
    }
    snprintf(out, cap, "%s", stem);
}

static Group *group_get(const char *name) {
    for (int i = 0; i < g_ngroups; i++)
        if (!strcmp(g_groups[i].name, name)) return &g_groups[i];
    if (g_ngroups >= MAXGRP) return NULL;
    Group *g = &g_groups[g_ngroups++];
    memset(g, 0, sizeof *g);
    snprintf(g->name, sizeof g->name, "%s", name);
    return g;
}

/* 内容指纹（FNV-1a 64）：增量判据不看 mtime。Windows 的 st_mtime 是秒级，
 * 同一秒内改文件分不出新旧；从压缩包解出来的文件时间戳还可能在未来，
 * 用 mtime 会每次都重编。和 zx 自身 write_file_if_changed 的哲学一致。 */
static unsigned long long hash_byte(unsigned long long h, unsigned char c) {
    return (h ^ c) * 1099511628211ULL;
}
static unsigned long long hash_str(unsigned long long h, const char *s) {
    for (; *s; s++) h = hash_byte(h, (unsigned char)*s);
    return h;
}
static unsigned long long hash_file(unsigned long long h, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return h;
    unsigned char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        for (size_t i = 0; i < n; i++) h = hash_byte(h, buf[i]);
    fclose(f);
    return h;
}

static int text_same(const char *path, const char *text) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return !strcmp(buf, text);
}

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

/* 取编译日志第一行有用信息，过滤非 ASCII（控制台是 GBK，UTF-8 会乱码）。 */
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

static int run_logged(const char *cmd, const char *log, char *err, size_t errcap) {
    char full[CPATH * 8];
    snprintf(full, sizeof full, "%s > \"%s\" 2>&1", cmd, log);
    int rc = system(full);
    if (rc == 0) { remove(log); return 0; }
    char detail[512];
    log_first_line(log, detail, sizeof detail);
    snprintf(err, errcap, "C compile failed%s%s (see %s)",
             detail[0] ? ": " : "", detail, log);
    return -1;
}

int csrc_build(const char *cc, const char *cflags, const char *dir,
               const char *cache, const char *only_group, int force,
               char ***outlibs, char *err, size_t errcap, int *ncompiled) {
    if (outlibs) *outlibs = NULL;
    if (ncompiled) *ncompiled = 0;
    if (err && errcap) err[0] = 0;
    if (!cc || !*cc) { snprintf(err, errcap, "no C compiler configured"); return -1; }
    if (!cflags) cflags = "";

    int nsrc = 0;
    char **srcs = collect_ext(dir, ".c", &nsrc);
    if (!nsrc) { free_list(srcs, nsrc); return 0; }   /* 没有 C 源码：零开销 */

    /* 头文件全组共依赖：改任意一个 .h 就把整组重编（C 库通常很小）。
     * 只算一次指纹，后面每组复用。 */
    int nhdr = 0;
    char **hdrs = collect_ext(dir, ".h", &nhdr);
    unsigned long long hdr_hash = 1469598103934665603ULL;
    for (int i = 0; i < nhdr; i++) hdr_hash = hash_file(hdr_hash, hdrs[i]);
    free_list(hdrs, nhdr);

    g_ngroups = 0;
    for (int i = 0; i < nsrc; i++) {
        char stem[256], grp[256];
        stem_of(srcs[i], stem, sizeof stem);
        group_of(dir, stem, grp, sizeof grp);
        Group *g = group_get(grp);
        if (!g) continue;
        if (g->nfiles < MAXFILE) g->files[g->nfiles++] = srcs[i];
    }

    char objdir[CPATH];
    snprintf(objdir, sizeof objdir, "%s/csrc", cache);
    if (mkdir_p(objdir) != 0) {
        snprintf(err, errcap, "cannot create '%s'", objdir);
        free_list(srcs, nsrc);
        return -1;
    }

    char **libs = NULL;
    int nlibs = 0, lcap = 0;
    for (int gi = 0; gi < g_ngroups; gi++) {
        Group *g = &g_groups[gi];
        if (only_group && strcmp(only_group, g->name)) continue;

        char libpath[CPATH], flagpath[CPATH], stamp[64];
        snprintf(libpath, sizeof libpath, "%s/lib%s.a", dir, g->name);
        snprintf(flagpath, sizeof flagpath, "%s/%s.flags", objdir, g->name);

        /* 指纹 = cc + flags + 本组所有 .c + 目录里所有 .h 的内容。
         * 内容没变就跳过；.o / .a 被删了、或 --force，也重编。 */
        unsigned long long h = hdr_hash;
        h = hash_str(h, cc);
        h = hash_str(h, cflags);
        for (int i = 0; i < g->nfiles; i++) h = hash_file(h, g->files[i]);
        snprintf(stamp, sizeof stamp, "%016llx\n", h);

        int dirty = force || !fexists(libpath) || !text_same(flagpath, stamp);
        for (int i = 0; i < g->nfiles && !dirty; i++) {
            char stem[256], opath[CPATH];
            stem_of(g->files[i], stem, sizeof stem);
            snprintf(opath, sizeof opath, "%s/%s_%s.o", objdir, g->name, stem);
            if (!fexists(opath)) dirty = 1;
        }

        if (dirty) {
            for (int i = 0; i < g->nfiles; i++) {
                char stem[256], opath[CPATH], log[CPATH], cmd[CPATH * 3];
                stem_of(g->files[i], stem, sizeof stem);
                snprintf(opath, sizeof opath, "%s/%s_%s.o", objdir, g->name, stem);
                snprintf(log, sizeof log, "%s/%s_%s.log", objdir, g->name, stem);
                snprintf(cmd, sizeof cmd,
                         "%s %s -I\"%s\" -w -fno-caret-diagnostics "
                         "-fno-color-diagnostics -c -o \"%s\" \"%s\"",
                         cc, cflags, dir, opath, g->files[i]);
                if (run_logged(cmd, log, err, errcap) != 0) {
                    free_list(srcs, nsrc);
                    free_list(libs, nlibs);
                    return -1;
                }
                if (ncompiled) (*ncompiled)++;
            }
            /* 重新归档：先删，避免 ar 保留已移除源文件留下的旧成员。 */
            remove(libpath);
            char arcmd[32768];
            snprintf(arcmd, sizeof arcmd, "%s rcs \"%s\"", archiver_of(cc), libpath);
            for (int i = 0; i < g->nfiles; i++) {
                char stem[256], opath[CPATH], one[CPATH + 8];
                stem_of(g->files[i], stem, sizeof stem);
                snprintf(opath, sizeof opath, "%s/%s_%s.o", objdir, g->name, stem);
                snprintf(one, sizeof one, " \"%s\"", opath);
                size_t used = strlen(arcmd);
                if (used + strlen(one) + 1 >= sizeof arcmd) break;
                memcpy(arcmd + used, one, strlen(one) + 1);
            }
            char arlog[CPATH];
            snprintf(arlog, sizeof arlog, "%s/%s.ar.log", objdir, g->name);
            if (run_logged(arcmd, arlog, err, errcap) != 0) {
                free_list(srcs, nsrc);
                free_list(libs, nlibs);
                return -1;
            }
            write_text(flagpath, stamp);
        }

        if (nlibs == lcap) {
            lcap = lcap ? lcap * 2 : 8;
            libs = xrealloc(libs, lcap * sizeof *libs);
        }
        libs[nlibs++] = xstrdup(libpath);
    }

    free_list(srcs, nsrc);
    if (outlibs) *outlibs = libs;
    else free_list(libs, nlibs);
    return nlibs;
}
