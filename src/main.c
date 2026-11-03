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

/* ---------------- 构建可执行文件 ---------------- */

static int build_program(Node *root, const char *out) {
    Buf header, rt;
    buf_init(&header);
    buf_init(&rt);
    BUnit units[64];
    int n = backend_emit_split(root, &header, &rt, units, 64);

    char hpath[2048];
    snprintf(hpath, sizeof hpath, "%s.h", out);
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
    snprintf(fpath, sizeof fpath, "%s.flags", out);
    snprintf(fstamp, sizeof fstamp, "%s|%s\n", g_cc, g_cflags);
    bool fchanged = write_file_if_changed(fpath, fstamp, strlen(fstamp));

    /* 收集单元：先落盘 .c 并决定谁要重编（等全部落盘后并行开火） */
    for (int pass = 0; pass <= n; pass++) {
        const char *uname = (pass == 0) ? "zxrt" : units[pass - 1].name;
        const char *src   = (pass == 0) ? (rt.data ? rt.data : "")
                                        : (units[pass - 1].src.data ? units[pass - 1].src.data : "");
        char cpath[2048], opath[2048];
        snprintf(cpath, sizeof cpath, "%s_%s.c", out, uname);
        snprintf(opath, sizeof opath, "%s_%s.o", out, uname);

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

    /* 链接（Windows 上 -o prog 会得 prog.exe） */
    char cmd[8192];
    snprintf(cmd, sizeof cmd, "%s %s -o \"%s\" %s", g_cc, g_cflags, out, objs.data);
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
        "       zxc init [name]     当前目录（或 name/ 子目录）创建 zx 工程\n"
        "       zxc check [file...] 静默检查（缺省检查当前目录全部 *.zx）\n"
        "       zxc fmt [file...]   格式化 [--check]（缺省全部 *.zx）\n");
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

/* 收集目录里的 *.zx（跳过隐藏文件与 .zxls- 影子文件），返回 malloc 的路径数组 */
#ifdef _WIN32
static char **collect_zx_files(int *out_n) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("*.zx", &fd);
    if (h == INVALID_HANDLE_VALUE) { *out_n = 0; return NULL; }
    char **list = NULL;
    int n = 0, cap = 0;
    do {
        if (fd.cFileName[0] == '.') continue;               /* 隐藏/.zxls- 影子 */
        if (cap == n) { cap = cap ? cap * 2 : 16; list = realloc(list, cap * sizeof *list); }
        list[n++] = strdup(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    *out_n = n;
    return list;
}
#else
#include <dirent.h>
static char **collect_zx_files(int *out_n) {
    DIR *d = opendir(".");
    if (!d) { *out_n = 0; return NULL; }
    char **list = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.' || l < 3 || strcmp(e->d_name + l - 3, ".zx") != 0) continue;
        if (cap == n) { cap = cap ? cap * 2 : 16; list = realloc(list, cap * sizeof *list); }
        list[n++] = strdup(e->d_name);
    }
    closedir(d);
    *out_n = n;
    return list;
}
#endif

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
        if (!seen_ns && !(c == ' ' || c == '\t')) { seen_ns = true; *first_ns = i; }
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
        files = collect_zx_files(&nfiles);
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
    "app\n"
    "app.exe\n"
    "app_*.c\n"
    "app_*.o\n"
    "app.h\n"
    "app.flags\n"
    "*.o\n"
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
    "zxc main.zx -o app        # 编译（产物 app / app.exe）\n"
    "zxc check                 # 静默检查全部 *.zx\n"
    "zxc fmt                   # 格式化全部 *.zx\n"
    "```\n"
    "\n"
    "## 模块\n"
    "\n"
    "同目录的 `*.zx` 都是可用模块：`const m = module(\"m\");` 或\n"
    "`module(\"m/inner.zx\")` 引子目录模块。标准库见 `std.zx`。\n";

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
    if (access("main.zx", 0) == 0) {
        fprintf(stderr, "zxc: main.zx already exists, aborting init\n");
        return 1;
    }
    if (write_text_file("main.zx", INIT_MAIN) != 0) return 1;
    if (write_text_file(".gitignore", INIT_GITIGNORE) != 0) return 1;
    if (write_text_file("README.md", INIT_README) != 0) return 1;
    Buf zj;
    buf_init(&zj);
    buf_printf(&zj, "{\n  \"name\": \"%s\",\n  \"version\": \"0.1.0\",\n  \"main\": \"main.zx\",\n  \"out\": \"%s\"\n}\n", name, name);
    write_text_file("zx.json", zj.data ? zj.data : "{}\n");

    /* 自检：刚写的 main.zx 必须能过编译检查 */
    g_srcpath = "main.zx";
    Node *root = fe_parse_file("main.zx");
    bool ok = root && g_error_count == 0;
    if (ok) sema_check(root);
    ok = ok && g_error_count == 0;
    printf("zxc init: created main.zx, zx.json, README.md, .gitignore%s\n",
           ok ? " (main.zx check ok)" : " (warning: main.zx check FAILED)");
    return ok ? 0 : 1;
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
        files = collect_zx_files(&nfiles);
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
                if (build_program(root, out) != 0) return 1;
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
