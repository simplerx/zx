/* main.c — zxc 驱动：参数解析、文件读写、增量编译（每模块一个 .o）。
 *
 * 用法（见 README）：
 *   zxc file.zx               lex + parse + 类型检查（默认）
 *   zxc file.zx --check       同上，显式写法
 *   zxc file.zx --tokens      打印 token 流
 *   zxc file.zx --ast         打印语法树（s-expression）
 *   zxc file.zx --emit-c      只吐生成的 C
 *   zxc file.zx -o prog       生成 C 并调 C 编译器编成可执行文件
 *   zxc file.zx -o prog --release   发布档：-O2（默认 dev 档 -O0，迭代快）
 *
 * 工程级子命令（读 zx.json）：
 *   zxc init [name]     创建 zx 工程（源码在 src/）
 *   zxc build           构建：--exe(默认)/--lib × --static(默认)/--shared、--release
 *                       中间文件进 .zxc-cache/，最终产物进 out/
 *   zxc run [args]      build 后直接运行 out/<产物>.exe（-- 之后的参数传给程序）
 *   zxc check / fmt     检查 / 格式化
 *
 * 后端的 C 编译器由 ZX_CC 环境变量指定，缺省 .toolchain/zig.exe cc。
 * -o 时每个模块一个 .c / .o，运行时函数体单独一个 _zxrt.c / .o：
 * 内容没变不重写（mtime 不动），.o 就不用重编；过期的 .o 并行重编。
 */
#include "zxc.h"
#include "ast.h"
#include "ty.h"
#include "val.h"
#include <stdarg.h>
#include <sys/stat.h>
#include <errno.h>
#ifndef _WIN32
#include <sys/wait.h>                       /* run 子进程退出码 */
#endif
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#include <io.h>
#include <direct.h>
#endif

typedef struct {
    const char *name;
    Buf         src;
} BUnit;

void  backend_emit(Node *root, Buf *out);
int   backend_emit_split(Node *root, Buf *header, Buf *rt, BUnit *units, int maxunits);
void  sema_check(Node *root);

static const char *g_cc;
static const char *g_cflags;
/* dev 档 -O0：迭代速度优先（0.4s/unit 级）；--release 才上 -O2。
 * 注意 zig cc 的 -O0 映射 Debug 会自带 UB 检查（曾把 arena 的未对齐
 * 访问直接炸成 panic——那是个真问题，但在代码层修，不靠 dev 档暴露），
 * 这里显式关掉，保持与 -O1 时代一致的语义。 */
#define CC_DEV  "-std=c11 -O0 -fno-sanitize=undefined"
#define CC_REL  "-std=c11 -O2"

/* ---------------- 并行 cc ---------------- */

typedef struct {
    char cpath[2048];
    char cmd[4096];
    int  rc;
} CCJob;

static CCJob g_jobs[65];   /* 单元数上限 64 + zxrt */

#ifdef _WIN32
#include <windows.h>
#include <process.h>

static unsigned __stdcall cc_thread(void *p) {
    CCJob *j = (CCJob *)p;
    j->rc = (system(j->cmd) != 0);
    return 0;
}

/* 并行编译：NUMBER_OF_PROCESSORS 路并发，超过 64 个句柄分批等。
 * 一旦有失败就停止收新任务（出错路径马上要退出了）。 */
static int run_cc_jobs(CCJob *jobs, int njobs) {
    if (njobs <= 0) return 0;
    int nproc = 4;
    const char *np = getenv("NUMBER_OF_PROCESSORS");
    if (np && atoi(np) > 0) nproc = atoi(np) > 16 ? 16 : atoi(np);
    if (nproc <= 1 || njobs == 1) {
        for (int i = 0; i < njobs; i++)
            if (system(jobs[i].cmd) != 0) return 1;
        return 0;
    }
    int rc = 0;
    for (int i = 0; i < njobs && !rc; ) {
        HANDLE h[64];
        CCJob *jo[64];
        int nh = 0;
        while (i < njobs && nh < 64 && nh < nproc) {
            uintptr_t th = _beginthreadex(NULL, 0, cc_thread, &jobs[i], 0, NULL);
            if (!th) { jobs[i].rc = (system(jobs[i].cmd) != 0); i++; continue; }
            h[nh] = (HANDLE)th;
            jo[nh] = &jobs[i];
            nh++;
            i++;
        }
        if (nh == 0) break;
        WaitForMultipleObjects((DWORD)nh, h, TRUE, INFINITE);
        for (int k = 0; k < nh; k++) {
            CloseHandle(h[k]);
            if (jo[k]->rc) {
                fprintf(stderr, "zxc: C compiler failed on %s\n", jo[k]->cpath);
                rc = 1;
            }
        }
    }
    return rc;
}
#else
static int run_cc_jobs(CCJob *jobs, int njobs) {
    for (int i = 0; i < njobs; i++)
        if (system(jobs[i].cmd) != 0) {
            fprintf(stderr, "zxc: C compiler failed on %s\n", jobs[i].cpath);
            return 1;
        }
    return 0;
}
#endif

/* ---------------- 文件 ---------------- */

static char *read_source(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "zxc: cannot open '%s'\n", path); exit(1); }
    size_t cap = 1 << 16, len = 0;
    char *buf = malloc(cap);
    if (!buf) { fprintf(stderr, "zxc: out of memory\n"); exit(1); }
    for (;;) {
        if (len + 4096 > cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { fprintf(stderr, "zxc: out of memory\n"); exit(1); }
            buf = nb;
        }
        size_t n = fread(buf + len, 1, cap - len - 1, f);
        len += n;
        if (n == 0) {
            if (ferror(f)) { fprintf(stderr, "zxc: read error on '%s'\n", path); exit(1); }
            break;
        }
    }
    buf[len] = 0;
    fclose(f);
    *out_len = len;
    return buf;
}

static bool write_file_if_changed(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "rb");
    if (f) {
        char same = 1;
        size_t cap = 4096;
        char *old = malloc(cap);
        size_t got = 0, n;
        while ((n = fread(old + got, 1, cap - got, f)) > 0) {
            got += n;
            if (got == cap) {
                cap *= 2;
                char *nb = realloc(old, cap);
                if (!nb) { same = 0; break; }
                old = nb;
            }
        }
        if (got != len || memcmp(old, data, len) != 0) same = 0;
        free(old);
        fclose(f);
        if (same) return false;        /* 内容没变：mtime 不动，.o 不用重编 */
    }
    f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "zxc: cannot write '%s'\n", path); exit(1); }
    if (len && fwrite(data, 1, len, f) != len) {
        fprintf(stderr, "zxc: write error on '%s'\n", path);
        exit(1);
    }
    fclose(f);
    return true;
}

static long long fmtime(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (long long)st.st_mtime;
}

/* .o 过期吗：比 .c 或共享头旧就要重编 */
static bool stale(const char *opath, const char *cpath, const char *hpath) {
    long long o = fmtime(opath);
    return o < fmtime(cpath) || o < fmtime(hpath);
}

/* ---------------- 构建可执行文件 / 库 ---------------- */

typedef enum {
    OUT_EXE,        /* 可执行文件（默认） */
    OUT_STATIC,     /* 静态库 .a */
    OUT_SHARED,     /* 动态库 .dll / .so */
} OutKind;

static bool fexists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/* 取路径最后一段（"out/app.exe" → "app.exe"） */
static void path_basename(const char *p, char *out, size_t cap) {
    const char *b = p;
    for (const char *q = p; *q; q++)
        if (*q == '/' || *q == '\\') b = q + 1;
    snprintf(out, cap, "%s", b);
}

/* 去掉最后一个扩展名（"app.exe" → "app"）；只认最后一个分隔符之后的点 */
static void path_strip_ext(char *s) {
    char *dot = strrchr(s, '.');
    char *sl = strrchr(s, '/'), *bs = strrchr(s, '\\');
    char *sep = (sl && bs) ? (sl > bs ? sl : bs) : (sl ? sl : bs);
    if (dot && (!sep || dot > sep)) *dot = 0;
}

static int mkdir_one(const char *d) {
#ifdef _WIN32
    if (_mkdir(d) != 0 && errno != EEXIST) return -1;
#else
    if (mkdir(d, 0755) != 0 && errno != EEXIST) return -1;
#endif
    return 0;
}

/* 逐级建目录（"out"、".zxc-cache" 这类构建目录） */
static int mkdir_p(const char *path) {
    if (!path || !*path) return 0;
    char tmp[2048];
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

/* 相对路径 → 绝对路径（已是绝对路径则原样返回） */
static void path_abs(const char *p, char *out, size_t cap) {
    bool isabs = (p[0] == '/' || p[0] == '\\');
    if ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z'))
        if (p[1] == ':') isabs = true;
    if (isabs) { snprintf(out, cap, "%s", p); return; }
#ifdef _WIN32
    if (_getcwd(out, (int)cap) != NULL) {
        size_t l = strlen(out);
        snprintf(out + l, cap - l, "\\%s", p);
        for (char *q = out; *q; q++) if (*q == '/') *q = '\\';
        return;
    }
#endif
    snprintf(out, cap, "%s", p);
}

/* 归档器跟编译器配套："…zig.exe cc" → "…zig.exe ar"，"… gcc" → "… gcc-ar"，
 * 其余交给 PATH 里的 ar。静态库归档用。 */
static const char *archiver_of(const char *cc) {
    static char buf[2048];
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

/* cachedir 非空：中间文件（.h/.c/.o/.flags）全部落进 cachedir/，
 * out 是最终产物路径；cachedir 为空则维持旧行为——中间文件挨着 out 放。 */
static int build_program(Node *root, const char *out, const char *cachedir, OutKind kind) {
#ifdef _WIN32
    /* zig cc -o 不补 .exe：产物没有扩展名时 cmd / 资源管理器都跑不了
     * （表现为"程序明明在却没有任何输出"），这里统一补上。 */
    static char outexe[2048];
    if (kind == OUT_EXE) {
        size_t l = strlen(out);
        if (l < 4 || _stricmp(out + l - 4, ".exe") != 0) {
            snprintf(outexe, sizeof outexe, "%s.exe", out);
            out = outexe;
        }
    }
#endif

    /* 中间文件前缀：有缓存目录就用 "缓存目录/产物名（去扩展名）" */
    char stem[2048];
    if (cachedir && *cachedir) {
        char base[1024];
        path_basename(out, base, sizeof base);
        path_strip_ext(base);
        snprintf(stem, sizeof stem, "%s/%s", cachedir, base);
    } else {
        snprintf(stem, sizeof stem, "%s", out);
    }

    Buf header, rt;
    buf_init(&header);
    buf_init(&rt);
    BUnit units[64];
    int n = backend_emit_split(root, &header, &rt, units, 64);

    char hpath[2048];
    snprintf(hpath, sizeof hpath, "%s.h", stem);
    bool hchanged = write_file_if_changed(hpath, header.data, header.len);

    const char *hbase = hpath;
    for (const char *q = hpath; *q; q++)
        if (*q == '/' || *q == '\\') hbase = q + 1;

    Buf objs;
    buf_init(&objs);
    int njobs = 0;

    /* 编译参数（cc 路径 / dev-release 档）记个 stamp：参数变了，磁盘上
     * 的旧 .o 全是别的参数编的，一律作废。首次没有 stamp 也算变了
     * （顺带自愈"旧 .o 带着别的运行时符号"之类的脏状态）。 */
    char fpath[2048], fstamp[512];
    snprintf(fpath, sizeof fpath, "%s.flags", stem);
    snprintf(fstamp, sizeof fstamp, "%s|%s\n", g_cc, g_cflags);
    bool fchanged = write_file_if_changed(fpath, fstamp, strlen(fstamp));

    /* 收集单元：先落盘 .c 并决定谁要重编（等全部落盘后并行开火） */
    for (int pass = 0; pass <= n; pass++) {
        const char *uname = (pass == 0) ? "zxrt" : units[pass - 1].name;
        const char *src   = (pass == 0) ? (rt.data ? rt.data : "")
                                        : (units[pass - 1].src.data ? units[pass - 1].src.data : "");
        char cpath[2048], opath[2048];
        snprintf(cpath, sizeof cpath, "%s_%s.c", stem, uname);
        snprintf(opath, sizeof opath, "%s_%s.o", stem, uname);

        /* 每份 .c 都 include 共享头：类型、运行时原型、跨模块原型都在里面 */
        Buf full;
        buf_init(&full);
        buf_printf(&full, "#include \"%s\"\n\n", hbase);
        buf_puts(&full, src);
        bool cchanged = write_file_if_changed(cpath, full.data ? full.data : "", full.len);

        /* 内容变了就直接作废旧 .o，不靠 mtime 判断：秒级时间戳在
         * 同一秒内重写 .c 时分不清新旧，会把上一轮的 .o 当新鲜的复用
         * （曾表现为测试随机假失败）。编译参数变了同样全作废。 */
        if (fchanged || cchanged || hchanged) remove(opath);

        if (stale(opath, cpath, hpath)) {
            if (getenv("ZX_VERBOSE")) fprintf(stderr, "  cc %s\n", cpath);
            if (njobs >= (int)(sizeof g_jobs / sizeof g_jobs[0])) {
                fprintf(stderr, "zxc: too many units\n");
                return 1;
            }
            CCJob *j = &g_jobs[njobs++];
            snprintf(j->cpath, sizeof j->cpath, "%s", cpath);
            snprintf(j->cmd, sizeof j->cmd, "%s %s -c -o \"%s\" \"%s\"",
                     g_cc, g_cflags, opath, cpath);
        }

        if (pass) buf_puts(&objs, " ");
        buf_printf(&objs, "\"%s\"", opath);
    }

    if (run_cc_jobs(g_jobs, njobs) != 0) return 1;

    /* 链接 / 归档：exe 直接链；动态库 -shared；静态库用配套的 ar 收拢 .o。
     * 最终产物的 -o 一律用绝对路径：zig cc 0.15 的 -shared 模式下
     * 相对子目录路径（out/x.dll）会报 "No such file or directory"
     * （exe / ar 无此毛病，绝对路径统一绕开）。 */
    char absout[2048];
    path_abs(out, absout, sizeof absout);
    char cmd[8192];
    if (kind == OUT_STATIC) {
        snprintf(cmd, sizeof cmd, "%s rcs \"%s\" %s", archiver_of(g_cc), absout, objs.data);
    } else {
        const char *shared = "";
#ifdef _WIN32
        if (kind == OUT_SHARED) shared = "-shared";
#else
        if (kind == OUT_SHARED) shared = "-shared -fPIC";
#endif
        snprintf(cmd, sizeof cmd, "%s %s %s -o \"%s\" %s",
                 g_cc, g_cflags, shared, absout, objs.data);
    }
    if (system(cmd) != 0) {
        fprintf(stderr, "zxc: link failed\n");
        return 1;
    }
    return 0;
}

/* ---------------- dumps ---------------- */

static void dump_toklist(Lexer *lx) {
    for (size_t i = 0; i < lx->ntoks; i++) {
        Token *t = &lx->toks[i];
        printf("%4d:%-4d %-14s", t->line, t->col, tok_kind_spell(t->kind));
        if (t->kind == TOK_STRING && t->text) {
            printf("\"%.*s\"", t->text_len, t->text);
        } else if (t->start && t->len > 0 && t->len < 64) {
            printf("%.*s", t->len, t->start);
        }
        printf("\n");
    }
}

/* ---------------- main ---------------- */

static void usage(void) {
    fprintf(stderr,
        "usage: zxc file.zx [--check|--tokens|--ast|--emit-c|--release] [-o prog]\n"
        "       zxc init [name]     创建 zx 工程（源码放 src/）\n"
        "       zxc check [file...] 静默检查（缺省当前目录，没有则扫 src/）\n"
        "       zxc fmt [file...]   格式化 [--check]\n"
        "       zxc build [--exe|--lib] [--static|--shared] [--release]\n"
        "                           按 zx.json 构建；中间文件在 .zxc-cache/，产物在 out/\n"
        "       zxc run [--release] [-- args...]\n"
        "                           build 后直接运行 out/<产物>.exe\n");
}

/* 默认工具链（`.toolchain/zig.exe cc`）钉在仓库根，而不是当前目录：
 * `zxc` 装在 bin/ 里，工具链在它的上一级。用 cwd 相对路径的话，
 * 只有"恰好站在仓库根"才能编译——在子目录里跑任何程序都会得到
 * cmd 的"系统找不到指定的路径"。 */
static char g_ccbuf[2048];

static const char *default_cc(const char *argv0) {
    char dir[1600];
    snprintf(dir, sizeof dir, "%s", argv0 ? argv0 : "");
    for (char *p = dir; *p; p++) if (*p == '/') *p = '\\';   /* cmd 只认反斜杠 */
    char *slash = strrchr(dir, '\\');
    if (!slash) return ".toolchain\\zig.exe cc";             /* 走 PATH 调的，只能靠 cwd */
    *slash = 0;
    snprintf(g_ccbuf, sizeof g_ccbuf, "%s\\..\\.toolchain\\zig.exe cc", dir);
    return g_ccbuf;
}

/* ---------------- init / check / fmt 子命令 ---------------- */

/* 收集目录里的 *.zx（跳过隐藏文件与 .zxls- 影子文件），返回 malloc 的路径数组。
 * dir 为 NULL/空串表示当前目录。 */
#ifdef _WIN32
static char **collect_zx_files(const char *dir, int *out_n) {
    char pat[2048];
    snprintf(pat, sizeof pat, "%s/*.zx", (dir && *dir) ? dir : ".");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { *out_n = 0; return NULL; }
    char **list = NULL;
    int n = 0, cap = 0;
    do {
        if (fd.cFileName[0] == '.') continue;               /* 隐藏/.zxls- 影子 */
        if (cap == n) { cap = cap ? cap * 2 : 16; list = realloc(list, cap * sizeof *list); }
        if (dir && *dir) {
            char full[2048];
            snprintf(full, sizeof full, "%s/%s", dir, fd.cFileName);
            list[n++] = strdup(full);
        } else {
            list[n++] = strdup(fd.cFileName);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    *out_n = n;
    return list;
}
#else
#include <dirent.h>
static char **collect_zx_files(const char *dir, int *out_n) {
    DIR *d = opendir((dir && *dir) ? dir : ".");
    if (!d) { *out_n = 0; return NULL; }
    char **list = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.' || l < 3 || strcmp(e->d_name + l - 3, ".zx") != 0) continue;
        if (cap == n) { cap = cap ? cap * 2 : 16; list = realloc(list, cap * sizeof *list); }
        if (dir && *dir) {
            char full[2048];
            snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
            list[n++] = strdup(full);
        } else {
            list[n++] = strdup(e->d_name);
        }
    }
    closedir(d);
    *out_n = n;
    return list;
}
#endif

/* 目录收集：先看当前目录，没有 .zx 再看 src/（配合 init 的 src/ 布局） */
static char **collect_zx_files_smart(int *out_n) {
    char **files = collect_zx_files(NULL, out_n);
    if (files) return files;
    return collect_zx_files("src", out_n);
}

/* ---- fmt：词法感知的行格式化（缩进/尾空白/空行规整），保守不动内容 ---- */

typedef struct {
    bool in_str;        /* "…" */
    bool in_chr;        /* '…' */
    bool in_block;      /* 斜杠星注释 */
} FmtState;

/* 单行扫描：返回花括号净变化，记 first_ns（首个非空白下标） */
static int fmt_scan_line(const char *s, FmtState *st, size_t *first_ns) {
    int delta = 0;
    *first_ns = 0;
    bool seen_ns = false;
    for (size_t i = 0; s[i]; i++) {
        char c = s[i];
        size_t ci = i;      /* switch 里可能 i++（块注释开头），首个非空白
                             * 下标必须用本字符的原始位置，否则块注释行
                             * 的开头两个字符会被当缩进丢掉 */
        if (st->in_block) {
            if (c == '*' && s[i + 1] == '/') { st->in_block = false; i++; }
            continue;
        }
        if (st->in_str) {
            if (c == '\\') { i++; continue; }
            if (c == '"') st->in_str = false;
            continue;
        }
        if (st->in_chr) {
            if (c == '\\') { i++; continue; }
            if (c == '\'') st->in_chr = false;
            continue;
        }
        switch (c) {
            case '"': st->in_str = true; break;
            case '\'': st->in_chr = true; break;
            case '/':
                if (s[i + 1] == '/') return delta;      /* 行注释：后面不用看 */
                if (s[i + 1] == '*') { st->in_block = true; i++; }
                break;
            case '{': delta++; break;
            case '}': delta--; break;
            default: break;
        }
        if (!seen_ns && !(c == ' ' || c == '\t')) { seen_ns = true; *first_ns = ci; }
    }
    return delta;
}

/* 格式化整个源码：4 空格缩进、去尾空白、连续空行压 1、末尾单换行。
 * CRLF 文档保持 CRLF。返回 malloc 的新文本。 */
static char *fmt_transform(const char *src) {
    bool had_cr = strstr(src, "\r\n") != NULL;
    /* 拆行（\n） */
    size_t cap = 64, n = 0;
    char **ls = malloc(cap * sizeof *ls);
    {
        const char *p = src;
        while (*p) {
            const char *q = strchr(p, '\n');
            size_t len = q ? (size_t)(q - p) : strlen(p);
            if (n + 1 >= cap) { cap *= 2; ls = realloc(ls, cap * sizeof *ls); }
            char *line = malloc(len + 1);
            memcpy(line, p, len);
            line[len] = 0;
            ls[n++] = line;
            if (!q) break;
            p = q + 1;
        }
    }

    FmtState st = {0};
    Buf out;
    buf_init(&out);
    int depth = 0;
    int blank_run = 0;
    bool any_content = false;

    for (size_t i = 0; i < n; i++) {
        char *raw = ls[i];
        /* 去尾空白 */
        size_t len = strlen(raw);
        while (len > 0 && (raw[len - 1] == ' ' || raw[len - 1] == '\t' || raw[len - 1] == '\r')) raw[--len] = 0;
        if (len == 0 && !st.in_block) {
            blank_run++;
            if (blank_run > 1 || !any_content) { free(raw); continue; }  /* 压空行，去首空行 */
            buf_puts(&out, "\n");
            free(raw);
            continue;
        }
        size_t first = 0;
        /* 该行净变化先按“不含本行”的 depth 算缩进：先扫描副本避免状态污染 */
        FmtState probe = st;
        int delta = fmt_scan_line(raw, &probe, &first);
        bool dedent = (raw[first] == '}');
        int ind = depth - (dedent ? 1 : 0);
        if (ind < 0) ind = 0;
        if (!st.in_block) {
            for (int k = 0; k < ind; k++) buf_puts(&out, "    ");
            buf_puts(&out, raw + first);                    /* 原始前导空白丢弃 */
        } else {
            buf_puts(&out, raw);                            /* 块注释延续行保留原样 */
        }
        buf_puts(&out, had_cr ? "\r\n" : "\n");
        any_content = true;
        blank_run = 0;
        depth += delta;
        if (depth < 0) depth = 0;
        st = probe;
        free(raw);
    }
    free(ls);
    return out.data ? out.data : strdup("");
}

/* ---- 子命令实现 ---- */

static int cmd_check(int argc, char **argv, int argi) {
    int nfiles = 0;
    char **files = NULL;
    if (argi < argc) {
        nfiles = argc - argi;
        files = argv + argi;
    } else {
        files = collect_zx_files_smart(&nfiles);
        if (nfiles == 0) {
            fprintf(stderr, "zxc: no .zx files in current directory\n");
            return 2;
        }
    }
    int bad = 0;
    for (int i = 0; i < nfiles; i++) {
        g_error_count = 0;
        g_diag_shown = 0;
        g_srcpath = files[i];
        Node *root = fe_parse_file(files[i]);
        if (!root || g_error_count) { bad++; continue; }
        sema_check(root);
        if (g_error_count) bad++;
        if (bad && argi >= argc) break;                     /* 全目录模式：报一个就够 */
    }
    if (bad) {
        fprintf(stderr, "zxc: %d of %d file(s) failed\n", bad, nfiles);
        return 1;
    }
    return 0;
}

static const char *INIT_MAIN =
    "/* main.zx — 工程入口。module(\"std\") 引标准库，自建模块放同目录 *.zx。 */\n"
    "const std = module(\"std\");\n"
    "\n"
    "const greet = fn (name: []const u8) void {\n"
    "    std.println(\"hello, {}!\", name);\n"
    "};\n"
    "\n"
    "const main = fn () i32 {\n"
    "    greet(\"zx\");\n"
    "    return 0;\n"
    "};\n";

static const char *INIT_GITIGNORE =
    "# 构建产物\n"
    "out/\n"
    "# 中间文件（生成的 C / 目标文件）\n"
    ".zxc-cache/\n"
    "# 编译器临时文件\n"
    ".zxfe-*.ast\n"
    ".zxls-*.zx\n"
    ".zc-cache/\n";

static const char *INIT_README =
    "# zx 项目\n"
    "\n"
    "## 构建 / 运行\n"
    "\n"
    "```sh\n"
    "zxc build             # 编译 src/ → out/<产物名>.exe（中间文件在 .zxc-cache/）\n"
    "zxc build --release   # 发布档 -O2\n"
    "zxc build --lib       # 静态库 out/<名>.a；--lib --shared 动态库 .dll/.so\n"
    "zxc run               # build 后直接运行（-- 之后的参数传给程序）\n"
    "zxc check             # 静默检查全部 *.zx（src/ 下也扫）\n"
    "zxc fmt               # 格式化全部 *.zx\n"
    "```\n"
    "\n"
    "## 模块\n"
    "\n"
    "`src/` 下的 `*.zx` 都是可用模块（与导入文件同目录即可）：\n"
    "`const m = module(\"m\");` 或 `module(\"m/inner.zx\")` 引子目录模块。\n"
    "标准库见 `std.zx`。\n";

static int write_text_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "zxc: cannot write '%s'\n", path); return -1; }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
    return 0;
}

static int cmd_init(int argc, char **argv, int argi) {
    const char *name = "app";
    /* `zxc init` → 当前目录；`zxc init name` → 创建 name/ 子目录并进入 */
    if (argi < argc) {
        name = argv[argi];
#ifdef _WIN32
        if (_mkdir(name) != 0 && errno != EEXIST) {
#else
        if (mkdir(name, 0755) != 0 && errno != EEXIST) {
#endif
            fprintf(stderr, "zxc: cannot create directory '%s'\n", name);
            return 1;
        }
        if (chdir(name) != 0) {
            fprintf(stderr, "zxc: cannot enter '%s'\n", name);
            return 1;
        }
    }
    if (access("zx.json", 0) == 0 || access("main.zx", 0) == 0) {
        fprintf(stderr, "zxc: project already exists here, aborting init\n");
        return 1;
    }
    /* 源码统一放 src/：工程根只留 zx.json、README、.gitignore 和产物目录 */
    if (mkdir_one("src") != 0) {
        fprintf(stderr, "zxc: cannot create 'src/'\n");
        return 1;
    }
    const char *mainpath = "src/main.zx";
    if (write_text_file(mainpath, INIT_MAIN) != 0) return 1;
    if (write_text_file(".gitignore", INIT_GITIGNORE) != 0) return 1;
    if (write_text_file("README.md", INIT_README) != 0) return 1;
    Buf zj;
    buf_init(&zj);
    buf_printf(&zj,
        "{\n  \"name\": \"%s\",\n  \"version\": \"0.1.0\",\n"
        "  \"main\": \"src/main.zx\",\n  \"out\": \"%s\"\n}\n",
        name, name);
    write_text_file("zx.json", zj.data ? zj.data : "{}\n");

    /* 自检：刚写的 src/main.zx 必须能过编译检查 */
    g_srcpath = mainpath;
    Node *root = fe_parse_file(mainpath);
    bool ok = root && g_error_count == 0;
    if (ok) sema_check(root);
    ok = ok && g_error_count == 0;
    printf("zxc init: created src/main.zx, zx.json, README.md, .gitignore%s\n",
           ok ? " (main.zx check ok)" : " (warning: main.zx check FAILED)");
    return ok ? 0 : 1;
}

/* ---- build / run：读 zx.json 的工程级构建 ---- */

/* 工程中间文件目录：生成的 .c/.o、共享头、编译参数 stamp 都收在这，
 * 不再散落在工程根（注意别和 build.sh 自用的 .zc-cache/ 撞名）。 */
#define ZX_CACHE_DIR ".zxc-cache"

typedef struct {
    bool ok;
    char name[256];     /* 工程名 */
    char main[1024];    /* 入口源文件 */
    char out[256];      /* 产物名（缺省 = 工程名） */
} Proj;

/* 抠 "key": "value"——zx.json 由 zxc init 生成、字段就这几个，
 * 不值得为此拖一个真 JSON 解析器进来。 */
static bool json_get_str(const char *text, const char *key, char *out, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(text, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), '"');
    if (!p) return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) out[i++] = *p++;
    out[i] = 0;
    return *p == '"';
}

static bool proj_load(Proj *pj) {
    memset(pj, 0, sizeof *pj);
    FILE *f = fopen("zx.json", "rb");
    if (!f) return false;
    char text[8192];
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    pj->ok = true;
    if (!json_get_str(text, "name", pj->name, sizeof pj->name) || !pj->name[0])
        snprintf(pj->name, sizeof pj->name, "app");
    if (!json_get_str(text, "out", pj->out, sizeof pj->out) || !pj->out[0])
        snprintf(pj->out, sizeof pj->out, "%s", pj->name);
    if (!json_get_str(text, "main", pj->main, sizeof pj->main) || !pj->main[0])
        snprintf(pj->main, sizeof pj->main,
                 fexists("src/main.zx") ? "src/main.zx" : "main.zx");
    return true;
}

/* 构建 + 产物路径写回 outpath。返回 0 成功。 */
static int do_build(Proj *pj, bool release, OutKind kind, char *outpath, size_t cap) {
    if (!fexists(pj->main)) {
        fprintf(stderr, "zxc: entry source '%s' not found\n", pj->main);
        return 1;
    }
    mkdir_p(ZX_CACHE_DIR);
    mkdir_p("out");

    const char *base = pj->out;
    if (kind == OUT_EXE) {
#ifdef _WIN32
        snprintf(outpath, cap, "out/%s.exe", base);
#else
        snprintf(outpath, cap, "out/%s", base);
#endif
    } else if (kind == OUT_STATIC) {
        snprintf(outpath, cap, "out/%s.a", base);
    } else {
#ifdef _WIN32
        snprintf(outpath, cap, "out/%s.dll", base);
#else
        snprintf(outpath, cap, "out/%s.so", base);
#endif
    }

    g_cflags = release ? CC_REL : CC_DEV;
    g_srcpath = pj->main;
    Node *root = fe_parse_file(pj->main);
    if (!root || g_error_count) return 1;
    sema_check(root);
    if (g_error_count) {
        if (g_error_count > g_diag_shown)
            fprintf(stderr, "zxc: %d error(s) (first %d shown)\n",
                    g_error_count, g_diag_shown);
        return 1;
    }
    return build_program(root, outpath, ZX_CACHE_DIR, kind);
}

/* build 旗标：--exe / --lib × --static / --shared、--release */
static int parse_kind(int argc, char **argv, int argi, bool *release, OutKind *kind) {
    bool want_lib = false, shared = false;
    *kind = OUT_EXE;
    for (int i = argi; i < argc; i++) {
        if      (!strcmp(argv[i], "--exe"))    want_lib = false;
        else if (!strcmp(argv[i], "--lib"))    want_lib = true;
        else if (!strcmp(argv[i], "--static")) shared = false;
        else if (!strcmp(argv[i], "--shared")) shared = true;
        else if (!strcmp(argv[i], "--release")) *release = true;
        else {
            fprintf(stderr, "zxc: unknown build option '%s'\n", argv[i]);
            return -1;
        }
    }
    if (want_lib) {
        *kind = shared ? OUT_SHARED : OUT_STATIC;
    } else if (shared) {
        fprintf(stderr, "zxc: --shared needs --lib (an exe is a program, not a library)\n");
        return -1;
    }
    return 0;
}

static int cmd_build(int argc, char **argv, int argi) {
    bool release = false;
    OutKind kind;
    if (parse_kind(argc, argv, argi, &release, &kind) != 0) return 2;
    Proj pj;
    if (!proj_load(&pj)) {
        fprintf(stderr, "zxc: zx.json not found here (cd into a project or 'zxc init')\n");
        return 2;
    }
    char outpath[2048];
    if (do_build(&pj, release, kind, outpath, sizeof outpath) != 0) return 1;
    printf("zxc build: %s\n", outpath);
    return 0;
}

/* run = build + 运行。子进程用 system() 拉起：同一个控制台，
 * stdout/stderr 原样落在这份终端里。 */
static int cmd_run(int argc, char **argv, int argi) {
    bool release = false;
    int first_prog = argc;
    for (int i = argi; i < argc; i++) {
        if (!strcmp(argv[i], "--"))        { first_prog = i + 1; break; }
        if (!strcmp(argv[i], "--release")) { release = true; first_prog = i + 1; continue; }
        if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "zxc: unknown run option '%s' "
                            "(program args go after '--')\n", argv[i]);
            return 2;
        }
        first_prog = i;                    /* 第一个非旗标：程序参数起点 */
        break;
    }
    Proj pj;
    if (!proj_load(&pj)) {
        fprintf(stderr, "zxc: zx.json not found here (cd into a project or 'zxc init')\n");
        return 2;
    }
    char outpath[2048];
    if (do_build(&pj, release, OUT_EXE, outpath, sizeof outpath) != 0) return 1;

    Buf cmd;
    buf_init(&cmd);
#ifdef _WIN32
    /* system() 走 cmd.exe，有两个坑：
     * ① 路径分隔符必须是反斜杠（"out/x.exe" 的斜杠会被 cmd 当开关拆掉）；
     * ② /c 的引号剥离规则会吃掉首尾引号，带参命令行得整体再包一对引号
     *    （""exe" "a" "b"" 形态，CMake/npm 同款处理）。 */
    buf_puts(&cmd, "\"");
    buf_printf(&cmd, "\"%s\"", outpath);
    for (char *p = cmd.data; p && *p; p++)
        if (*p == '/') *p = '\\';
    for (int i = first_prog; i < argc; i++)
        buf_printf(&cmd, " \"%s\"", argv[i]);
    buf_puts(&cmd, "\"");
#else
    buf_printf(&cmd, "\"%s\"", outpath);
    for (int i = first_prog; i < argc; i++)
        buf_printf(&cmd, " \"%s\"", argv[i]);
#endif
    int rc = system(cmd.data ? cmd.data : "");
    if (cmd.data) free(cmd.data);
#ifdef _WIN32
    return rc;                             /* cmd.exe 透传子进程退出码 */
#else
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    fprintf(stderr, "zxc: program terminated abnormally\n");
    return 1;
#endif
}

static int cmd_fmt(int argc, char **argv, int argi, bool check_only) {
    int nfiles = 0;
    char **files = NULL;
    bool owned = false;
    /* fmt 自己的 --check 旗标 */
    for (int i = argi; i < argc; i++) {
        if (!strcmp(argv[i], "--check")) { check_only = true; argv[i] = NULL; }
    }
    if (argi < argc) {
        nfiles = argc - argi;
        files = argv + argi;
    } else {
        files = collect_zx_files_smart(&nfiles);
        owned = true;
        if (nfiles == 0) {
            fprintf(stderr, "zxc: no .zx files in current directory\n");
            return 2;
        }
    }
    int changed = 0, bad = 0;
    for (int i = 0; i < nfiles; i++) {
        if (!files[i]) continue;
        /* 复用 zxc 的读文件；失败即跳过 */
        FILE *f = fopen(files[i], "rb");
        if (!f) { fprintf(stderr, "zxc: cannot open '%s'\n", files[i]); bad++; continue; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *src = malloc((size_t)sz + 1);
        if (sz > 0 && fread(src, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(src); bad++; continue; }
        src[sz] = 0;
        fclose(f);

        char *newtext = fmt_transform(src);
        if (strcmp(src, newtext) != 0) {
            changed++;
            printf("fmt: %s\n", files[i]);
            if (!check_only) {
                f = fopen(files[i], "wb");
                if (!f) { fprintf(stderr, "zxc: cannot write '%s'\n", files[i]); bad++; free(src); continue; }
                fwrite(newtext, 1, strlen(newtext), f);
                fclose(f);
            }
        }
        free(src);
    }
    if (owned && files) { for (int i = 0; i < nfiles; i++) free(files[i]); free(files); }
    if (check_only) {
        if (changed) { fprintf(stderr, "zxc fmt: %d file(s) need formatting\n", changed); return 1; }
        return 0;
    }
    printf("zxc fmt: %d of %d file(s) formatted\n", changed, nfiles);
    return bad ? 1 : 0;
}

int main(int argc, char **argv) {
    sema_set_argv0(argv[0]);

    g_cc = getenv("ZX_CC");
    if (!g_cc || !*g_cc) g_cc = default_cc(argv[0]);

    /* 子命令形式（file-first 旧语法不受影响） */
    if (argc >= 2 && argv[1][0] != '-') {
        if (!strcmp(argv[1], "init"))  return cmd_init(argc, argv, 2);
        if (!strcmp(argv[1], "check")) return cmd_check(argc, argv, 2);
        if (!strcmp(argv[1], "fmt"))   return cmd_fmt(argc, argv, 2, false);
        if (!strcmp(argv[1], "build")) return cmd_build(argc, argv, 2);
        if (!strcmp(argv[1], "run"))   return cmd_run(argc, argv, 2);
    }

    const char *file = NULL, *out = NULL;
    bool dump_tokens = false, dump_ast = false, emit_c = false, check = true;
    bool release = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tokens"))      dump_tokens = true;
        else if (!strcmp(argv[i], "--ast"))    dump_ast = true;
        else if (!strcmp(argv[i], "--emit-c")) emit_c = true;
        else if (!strcmp(argv[i], "--check"))  check = true;
        else if (!strcmp(argv[i], "--release")) release = true;
        else if (!strncmp(argv[i], "--fe=", 5)) {
            /* 前端选择（自举阶段 3）：默认 Zx 前端，--fe=c 交叉验证 */
            if (!strcmp(argv[i] + 5, "zx"))       fe_set_mode(true);
            else if (!strcmp(argv[i] + 5, "c"))   fe_set_mode(false);
            else {
                fprintf(stderr, "zxc: --fe expects 'zx' or 'c'\n");
                return 2;
            }
        }
        else if (!strcmp(argv[i], "-o")) {
            if (++i >= argc) { fprintf(stderr, "zxc: -o needs a path\n"); return 2; }
            out = argv[i];
        }
        else if (!strncmp(argv[i], "-o", 2) && argv[i][2]) out = argv[i] + 2;
        else if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "zxc: unknown option '%s'\n", argv[i]);
            usage();
            return 2;
        }
        else file = argv[i];
    }
    if (!file) { usage(); return 2; }
    fe_set_argv0(argv[0]);
    g_cflags = release ? CC_REL : CC_DEV;

    g_srcpath = file;

    if (dump_tokens) {
        /* 词法单元 dump 仍走 C 词法器：这是调试视图，且阶段 1 已对拍过
         * 两侧词法器逐 token 一致（bootstrap/diff_tok.sh 一族）。 */
        size_t len = 0;
        char *src = read_source(file, &len);
        if (!src) { fprintf(stderr, "zxc: cannot open '%s'\n", file); return 2; }
        Lexer lx;
        lexer_init(&lx, src, len);
        lexer_run(&lx);
        if (g_error_count) return 1;
        dump_toklist(&lx);
        return 0;
    }

    Node *root = fe_parse_file(file);
    if (!root || g_error_count) return 1;

    if (dump_ast) {
        ast_dump(root, 0);
    } else {
        if (check && g_error_count == 0) sema_check(root);
        if ((emit_c || out) && g_error_count == 0) {
            if (out) {
                if (build_program(root, out, NULL, OUT_EXE) != 0) return 1;
            } else {
                Buf csrc;
                buf_init(&csrc);
                backend_emit(root, &csrc);
                fwrite(csrc.data, 1, csrc.len, stdout);
            }
        }
    }

    int errors = g_error_count;
    if (errors) {
        if (errors > g_diag_shown)
            fprintf(stderr, "zxc: %d error(s) (first %d shown)\n", errors, g_diag_shown);
        else
            fprintf(stderr, "zxc: %d error(s)\n", errors);
    }
    return errors ? 1 : 0;
}
