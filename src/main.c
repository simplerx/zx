/* main.c — zxc 驱动：参数解析、文件读写、增量编译（每模块一个 .o）。
 *
 * 用法（见 README）：
 *   zxc file.zx               lex + parse + 类型检查（默认）
 *   zxc file.zx --check       同上，显式写法
 *   zxc file.zx --tokens      打印 token 流
 *   zxc file.zx --ast         打印语法树（s-expression）
 *   zxc file.zx --emit-c      只吐生成的 C
 *   zxc file.zx -o prog       生成 C 并调 C 编译器编成可执行文件
 *
 * 后端的 C 编译器由 ZX_CC 环境变量指定，缺省 .toolchain/zig.exe cc。
 * -o 时每个模块一个 .c / .o：内容没变不重写（mtime 不动），.o 就不用重编。
 */
#include "zxc.h"
#include "ast.h"
#include "ty.h"
#include "val.h"
#include <stdarg.h>
#include <sys/stat.h>

typedef struct {
    const char *name;
    Buf         src;
} BUnit;

void  backend_emit(Node *root, Buf *out);
int   backend_emit_split(Node *root, Buf *header, BUnit *units, int maxunits);
void  sema_check(Node *root);

static const char *g_cc;
#define CC_FLAGS "-std=c11 -O1"

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
    Buf header;
    buf_init(&header);
    BUnit units[64];
    int n = backend_emit_split(root, &header, units, 64);

    char hpath[2048];
    snprintf(hpath, sizeof hpath, "%s.h", out);
    bool hchanged = write_file_if_changed(hpath, header.data, header.len);

    Buf objs;
    buf_init(&objs);

    for (int i = 0; i < n; i++) {
        char cpath[2048], opath[2048], cmd[4096];
        snprintf(cpath, sizeof cpath, "%s_%s.c", out, units[i].name);
        snprintf(opath, sizeof opath, "%s_%s.o", out, units[i].name);

        /* 每份 .c 都 include 共享头：类型、运行时、跨模块原型都在里面 */
        const char *hbase = hpath;
        for (const char *q = hpath; *q; q++)
            if (*q == '/' || *q == '\\') hbase = q + 1;
        Buf full;
        buf_init(&full);
        buf_printf(&full, "#include \"%s\"\n\n", hbase);
        buf_puts(&full, units[i].src.data ? units[i].src.data : "");
        bool cchanged = write_file_if_changed(cpath, full.data ? full.data : "", full.len);

        /* 内容变了就直接作废旧 .o，不靠 mtime 判断：秒级时间戳在
         * 同一秒内重写 .c 时分不清新旧，会把上一轮的 .o 当新鲜的复用
         * （曾表现为测试随机假失败）。 */
        if (cchanged || hchanged) remove(opath);

        if (stale(opath, cpath, hpath)) {
            if (getenv("ZX_VERBOSE")) fprintf(stderr, "  cc %s\n", cpath);
            snprintf(cmd, sizeof cmd, "%s %s -c -o \"%s\" \"%s\"", g_cc, CC_FLAGS, opath, cpath);
            if (system(cmd) != 0) {
                fprintf(stderr, "zxc: C compiler failed on %s\n", cpath);
                return 1;
            }
        }

        if (i) buf_puts(&objs, " ");
        buf_printf(&objs, "\"%s\"", opath);
    }

    /* 链接（Windows 上 -o prog 会得 prog.exe） */
    char cmd[8192];
    snprintf(cmd, sizeof cmd, "%s %s -o \"%s\" %s", g_cc, CC_FLAGS, out, objs.data);
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
        "usage: zxc file.zx [--check|--tokens|--ast|--emit-c] [-o prog]\n");
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

int main(int argc, char **argv) {
    sema_set_argv0(argv[0]);

    g_cc = getenv("ZX_CC");
    if (!g_cc || !*g_cc) g_cc = default_cc(argv[0]);

    const char *file = NULL, *out = NULL;
    bool dump_tokens = false, dump_ast = false, emit_c = false, check = true;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tokens"))      dump_tokens = true;
        else if (!strcmp(argv[i], "--ast"))    dump_ast = true;
        else if (!strcmp(argv[i], "--emit-c")) emit_c = true;
        else if (!strcmp(argv[i], "--check"))  check = true;
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

    size_t len = 0;
    char *src = read_source(file, &len);
    g_srcpath = file;

    Lexer lx;
    lexer_init(&lx, src, len);
    lexer_run(&lx);
    if (g_error_count) return 1;

    if (dump_tokens) {
        dump_toklist(&lx);
        return 0;
    }

    if (dump_ast && g_error_count == 0) {
        Node *root = parse_program(&lx);
        ast_dump(root, 0);
    } else {
        Node *root = parse_program(&lx);
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
