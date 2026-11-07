/* main.c — zxc 驱动：参数解析、文件读写、增量编译（每模块一个 .o）。
 *
 * 用法（见 README）：
 *   zxc -h                   总览；zxc <子命令> -h 看该子命令细节
 *   zxc -v                   版本（前端 / 后端工具链）
 *   zxc file.zx               lex + parse + 类型检查（默认）
 *   zxc file.zx --check       同上，显式写法
 *   zxc file.zx --tokens      打印 token 流
 *   zxc file.zx --ast         打印语法树（s-expression）
 *   zxc file.zx --emit-c      只吐生成的 C
 *   zxc file.zx -o prog       生成 C 并调 C 编译器编成可执行文件
 *   zxc file.zx -o prog --release   发布档：-O2（默认 dev 档 -O0，迭代快）
 *
 * 工程级子命令（读 zx.json）：
 *   zxc init [name]     创建 zx 工程（源码在 src/，外部模块根 vendor/）
 *   zxc build           构建：--exe(默认)/--lib × --static(默认)/--shared、--release
 *                       中间文件进 .zxc-cache/，最终产物进 out/
 *   zxc run [args]      build 后直接运行 out/<产物>.exe（-- 之后的参数传给程序）
 *   zxc test [过滤]     运行 src/test/ 测试模块（零参 pub 函数即用例）
 *   zxc check / fmt     检查 / 格式化
 *
 * zx.json：工程清单（init 生成）。name/version/main/out 之外，
 * "deps": ["vendor2", "../shared"] 是模块搜索目录——相对导入打不开时
 * 依次回退（工程根的 vendor/ 存在即自动加入）。
 *
 * 后端用的 C 编译器由 ZX_CC 环境变量指定，缺省 .toolchain/zig.exe cc。
 * -o 时每个模块一个 .c / .o，运行时函数体单独一个 _zxrt.c / .o：
 * 内容没变不重写（mtime 不动），.o 就不用重编；过期的 .o 并行重编。
 */
#include "zxc.h"
#include "ast.h"
#include "ty.h"
#include "val.h"
#include "cbind.h"
#include "csrc.h"
#include "clib.h"
#include <stdarg.h>
#include <time.h>
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
/* 生成的 .c 行 → 模块内 zx 行（供后端诊断翻译用；false = 该行没有映射） */
bool  backend_map_lookup(int unit, int cline, int *zx_line, int *zx_col);
void  sema_check(Node *root);
void  sema_set_search_paths(const char *const *dirs);   /* deps 模块搜索路径 */
const char *sema_module_path(int i);                    /* 模块 i 的源文件路径 */
static char *read_source(const char *path, size_t *out_len);

static const char *g_cc;
static const char *g_cflags;
/* dev 档 -O0：迭代速度优先（0.4s/unit 级）；--release 才上 -O2。
 * 注意 zig cc 的 -O0 映射 Debug 会自带 UB 检查（曾把 arena 的未对齐
 * 访问直接炸成 panic——那是个真问题，但在代码层修，不靠 dev 档暴露），
 * 这里显式关掉，保持与 -O1 时代一致的语义。 */
#define CC_DEV  "-std=c11 -O0 -fno-sanitize=undefined"
#define CC_REL  "-std=c11 -O2 -DZX_RELEASE"
/* 工程中间文件目录：build_program 也要用，所以定义在文件头。
 * 注意别和 build.sh 自用的 .zc-cache/ 撞名。 */
#define ZX_CACHE_DIR ".zxc-cache"

/* 链接失败的翻译：lld 会说 `undefined symbol: zx_main_foo`——模块前缀是
 * 我们自己的编码，翻回 `模块/名字` 才对用户有意义。 */
static void report_link_log(const char *logpath, int kind) {
    size_t len = 0;
    char *text = read_source(logpath, &len);
    if (!text) { fprintf(stderr, "error[B2001]: link failed\n"); return; }
    int nsym = 0;
    char line[2048];
    size_t i = 0;
    while (i < len) {
        size_t k = 0;
        while (i < len && text[i] != '\n' && k + 1 < sizeof line) line[k++] = text[i++];
        while (i < len && text[i] != '\n') i++;
        if (i < len) i++;
        line[k] = 0;

        const char *sym = strstr(line, "undefined symbol:");
        if (!sym) sym = strstr(line, "undefined reference to");
        if (!sym) continue;
        sym = strchr(sym, ':');
        if (!sym) sym = strrchr(line, '`');
        if (!sym) continue;
        sym++;
        while (*sym == ' ' || *sym == '`' || *sym == '\'') sym++;
        char name[256];
        size_t n = 0;
        while (sym[n] && sym[n] != '\'' && sym[n] != '`' && sym[n] != ' ' &&
               sym[n] != '\n' && n + 1 < sizeof name) { name[n] = sym[n]; n++; }
        name[n] = 0;
        if (!name[0]) continue;
        if (nsym++ >= 5) continue;

        /* zx_<模块stem>_<函数名> → 模块 / 函数 */
        const char *mod = name, *fn = "";
        char modbuf[128] = "";
        if (!strncmp(name, "zx_", 3)) {
            const char *u = strchr(name + 3, '_');
            if (u && u - (name + 3) < (int)sizeof modbuf) {
                snprintf(modbuf, sizeof modbuf, "%.*s", (int)(u - (name + 3)), name + 3);
                mod = modbuf;
                fn = u + 1;
            }
        }
        if (fn[0])
            fprintf(stderr, "error[B2001]: link failed: module `%s` has no definition for `%s`\n", mod, fn);
        else
            fprintf(stderr, "error[B2001]: link failed: undefined symbol `%s`\n", name);
        fprintf(stderr, "  help: declared but never implemented (extern / unimplemented intrinsic),\n"
                        "        or the module is not reachable through any import chain\n");
    }
    if (nsym == 0) {
        /* 没有符号可翻：给出第一行非空输出，但去掉工具链绝对路径 */
        for (size_t k = 0; k < len; k++) {
            if (text[k] == '\n' || k == 0) {
                const char *b = text + k + (k ? 1 : 0);
                if (*b && *b != '\n') {
                    char one[256];
                    size_t m = 0;
                    while (b[m] && b[m] != '\n' && m + 1 < sizeof one) { one[m] = b[m]; m++; }
                    one[m] = 0;
                    fprintf(stderr, "error[B2002]: link failed: %s\n", one);
                    break;
                }
            }
        }
        if (nsym == 0 && !getenv("ZX_VERBOSE"))
            fprintf(stderr, "  help: ZX_VERBOSE=1 zxc build shows the full linker output\n");
    }
    free(text);
    (void)kind;
}

/* ---------------- 并行 cc ---------------- */

typedef struct {
    char cpath[2048];
    char cmd[4096];
    char log[2048];        /* C 编译器的输出重定向到这里，失败后再翻译 */
    int  unit;             /* 单元下标（-1 = zxrt 运行时支持） */
    int  rc;
} CCJob;

static CCJob g_jobs[65];   /* 单元数上限 64 + zxrt */

/* ---------------- 后端诊断的翻译 ----------------
 *
 * C 编译器的报错默认指向缓存目录里的 .c 第几百行、或者 zig 自带的 libc
 * 头文件——那对写 zx 的人毫无意义。这里把它的输出收进日志，逐条翻成
 * 「模块:行:列: error[Bxxxx]: 人话」，指回 zx 源码。
 *
 * 为什么会有 C 编译错误：zx 的类型检查已经过了，说明是**代码生成**产出
 * 了 C 编译器不接受的东西——本质上是我们自己的缺陷。所以码段用 B 开头
 * （Backend），并在 help 里明说要反馈。 */

/* 从 "<path>:<line>:<col>: error: msg" 拆字段。Windows 盘符里也有冒号，
 * 所以从右往左找最后一个能匹配 ":N:M:" 的冒号。 */
static bool split_ccdiag(const char *s, char *path, size_t pcap,
                         int *pline, int *pcol, const char **pmsg, int *psev) {
    /* 从左往右找第一个 "行:列" 形式：Windows 盘符的 "C:" 后面不是数字，
     * 不会误命中；而 ":12:3:" 里的 ":3:" 也在后面的位置，所以必须取
     * 第一次命中（取最后一次会把行号吃进路径里）。 */
    const char *found = NULL, *found1 = NULL;
    for (const char *p = s; (p = strchr(p, ':')) != NULL; p++) {
        int l = 0, c = 0;
        if (sscanf(p, ":%d:%d:", &l, &c) == 2) { found = p; break; }
        if (!found1 && sscanf(p, ":%d:", &l) == 1) found1 = p;
    }
    if (!found) found = found1;
    if (!found) return false;
    size_t n = (size_t)(found - s);
    if (n == 0 || n >= pcap) return false;
    memcpy(path, s, n);
    path[n] = 0;

    int consumed = 0, l = 0, c = 0;
    if (sscanf(found, ":%d:%d:%n", &l, &c, &consumed) != 2) {
        if (sscanf(found, ":%d:%n", &l, &consumed) != 1) return false;
        c = 0;
    }
    *pline = l;
    *pcol  = c;

    const char *r = found + consumed;
    while (*r == ' ') r++;
    if (!strncmp(r, "fatal error:", 12))      { *psev = 2; *pmsg = r + 12; }
    else if (!strncmp(r, "error:", 6))        { *psev = 2; *pmsg = r + 6;  }
    else if (!strncmp(r, "warning:", 8))      { *psev = 1; *pmsg = r + 8;  }
    else if (!strncmp(r, "note:", 5))         { *psev = 0; *pmsg = r + 5;  }
    else return false;
    while (**pmsg == ' ') (*pmsg)++;
    return true;
}

static const char *base_name(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++)
        if (*q == '/' || *q == '\\') b = q + 1;
    return b;
}

/* 同一条诊断只报一次（一个类型错误会让十几个单元同时炸，全打出来等于刷屏）。 */
static char g_seen[32][256];
static int  g_nseen;

static bool seen_before(const char *key) {
    for (int i = 0; i < g_nseen; i++)
        if (!strcmp(g_seen[i], key)) return true;
    if (g_nseen < 32) snprintf(g_seen[g_nseen++], 256, "%s", key);
    return false;
}

/* 翻译一份 C 编译器日志。返回报出的错误条数。 */
static int report_cc_log(CCJob *j, int mode) {
    size_t len = 0;
    char *text = read_source(j->log, &len);
    if (!text) return 0;

    int nerr = 0, nwarn = 0;
    const char *cbase = base_name(j->cpath);
    const char *mod = (j->unit >= 0) ? sema_module_path(j->unit) : NULL;

    /* mode 0 只报"能定位到 zx 源码"的（对用户最有价值），mode 1 报剩下的。
     * 调用方先跑完所有作业的 mode 0，再跑 mode 1——否则 zxrt 之类的
     * 无定位报错会抢先把同一个根因的消息吃进去重表。 */
    {
        char line[2048];
        size_t i = 0;
        while (i < len) {
            size_t k = 0;
            while (i < len && text[i] != '\n' && k + 1 < sizeof line) line[k++] = text[i++];
            while (i < len && text[i] != '\n') i++;
            if (i < len) i++;
            line[k] = 0;

            char path[2048];
            int ln = 0, col = 0, sev = 0;
            const char *msg = NULL;
            if (!split_ccdiag(line, path, sizeof path, &ln, &col, &msg, &sev)) continue;
            if (sev == 0) continue;                       /* note: 挂在别的诊断后面，不单报 */
            if (sev == 1) { nwarn++; continue; }   /* 生成代码的警告是噪音 */

            /* 定位：本单元的 .c → 翻回 zx 行；共享头 / libc 头 → 只能定位到单元 */
            int zl = 0, zc = 0;
            bool mapped = false;
            if (!strcmp(base_name(path), cbase) && j->unit >= 0)
                mapped = backend_map_lookup(j->unit, ln - 2, &zl, &zc);

            if (mode == 0 && !mapped) continue;
            if (mode == 1 && mapped)  continue;
            if (seen_before(msg)) continue;

            if (j->unit < 0) {
                fprintf(stderr, "error[B1003]: the bundled C runtime (zxrt) does not compile: %s\n", msg);
                fprintf(stderr, "  help: this is the compiler's own support code, not your zx source;\n"
                                "        check ZX_CC / .toolchain, or rerun with ZX_VERBOSE=1\n");
            } else if (mapped && zl > 0) {
                fprintf(stderr, "%s:%d:%d: error[B1001]: generated C was rejected by the C compiler: %s\n",
                        mod ? mod : cbase, zl, zc > 0 ? zc : 1, msg);
                fprintf(stderr, "  help: zx type checking passed, so this is a code-generation bug;\n"
                                "        rerun with ZX_VERBOSE=1 to see the raw backend output\n");
            } else {
                fprintf(stderr, "error[B1002]: generated C for module `%s` does not compile "
                                "(location not mappable to zx source): %s\n",
                        mod ? mod : cbase, msg);
                fprintf(stderr, "  help: usually comes from the shared type header; "
                                "rerun with ZX_VERBOSE=1 and report it\n");
            }
            nerr++;
        }
    }
    free(text);
    if (getenv("ZX_VERBOSE") && nwarn)
        fprintf(stderr, "zxc: (%d backend warning(s) hidden, see %s)\n", nwarn, j->log);
    return nerr;
}

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
            if (jo[k]->rc) rc = 1;
        }
    }
    return rc;
}
#else
static int run_cc_jobs(CCJob *jobs, int njobs) {
    int rc = 0;
    for (int i = 0; i < njobs; i++)
        if (system(jobs[i].cmd) != 0) { jobs[i].rc = 1; rc = 1; }
    return rc;
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
    /* stat 而不是 fopen：Windows 上 fopen 打不开目录，vendor/ 这类
     * 目录存在性探测会被误判成不存在 */
    struct stat st;
    return stat(p, &st) == 0;
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

/* ---------------- 编译进度（stderr，"[zx] " 前缀） ----------------
 * "停几秒没动静"是最伤手的体感：每个可能超过半秒的阶段在开始/结束
 * 各打一行。只打成功路径——失败时 stderr 已经有诊断，进度行是噪音。
 * ZX_VERBOSE=1 时本来就有逐单元细节，跳过不重复。行首 "[zx] " 和
 * 诊断、`  cc ` 细节行区分开，互不干扰（test.sh 的断言都靠这个）。
 * 计时用 clock()：MinGW 下即进程启动以来的墙钟时间，秒级阶段够用。 */
static double prog_clock(void) {
    return (double)clock() / (double)CLOCKS_PER_SEC;
}
static double g_prog_t0;   /* main() 一进来记的起点：总结行报总耗时 */

static void prog(const char *fmt, ...) {
    if (getenv("ZX_VERBOSE")) return;
    va_list ap;
    fprintf(stderr, "[zx] ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* cachedir 非空：中间文件（.h/.c/.o/.flags）全部落进 cachedir/，
 * out 是最终产物路径；cachedir 为空则维持旧行为——中间文件挨着 out 放。 */
static int build_program(Node *root, const char *out, const char *cachedir, OutKind kind) {
    /* vendor/ 下的 C 源码：与 zx 生成的 C 用同一套 cc + flags 编成静态库，
     * 注入链接。改了 .c/.h、换 dev/release 档都会自动重编；重编过必须强制
     * 重链，否则"无改动重建"会拿旧 exe 糊弄过去。没有 C 源码时这里零开销。 */
    bool csrc_relink = 0;
    if (kind != OUT_STATIC) {
        char **libs = NULL, cerr[1024];
        int ncomp = 0;
        int nlib = csrc_build(g_cc, g_cflags, "vendor", ZX_CACHE_DIR, NULL, 0,
                              &libs, cerr, sizeof cerr, &ncomp);
        if (nlib < 0) { fprintf(stderr, "zxc: %s\n", cerr); return 1; }
        for (int i = 0; i < nlib; i++) {
            zx_add_link_lib(libs[i]);
            free(libs[i]);
        }
        free(libs);

        /* 目录树模式：.clib 清单里可以是指向任意深度、任意位置的源码
         * （库本身一个文件都没复制过来）。build/run/test 走这里自动
         * 增量重编，重编过同样必须强制重链。 */
        int nman = 0;
        char **mans = clib_scan_manifests("vendor", &nman);
        for (int i = 0; i < nman; i++) {
            char clib[1024], merr[1024];
            int mcomp = 0;
            int mr = clib_build(g_cc, g_cflags, mans[i], ZX_CACHE_DIR, 0,
                                clib, sizeof clib, merr, sizeof merr, &mcomp);
            free(mans[i]);
            if (mr < 0) {
                fprintf(stderr, "zxc: %s\n", merr);
                free(mans);
                return 1;
            }
            if (mr > 0 && clib[0]) zx_add_link_lib(clib);
            if (mcomp > 0) {
                csrc_relink = 1;
                prog("compiled %d C unit(s) from a .clib manifest", mcomp);
            }
        }
        free(mans);

        if (ncomp > 0) {
            csrc_relink = 1;
            prog("compiled %d C unit(s) from vendor/", ncomp);
        }
    }
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

    /* 中间文件前缀：有缓存目录就用 "缓存目录/产物名（去扩展名）"。
     * 前缀里带上优化档位（-dev / -rel）：dev 与 release 的 .o 本来就
     * 互不兼容（优化级别不同，.flags 盖章也不同），过去共用一个前缀
     * 意味着 `--release` 和 `zxc run`（-O0）来回切一次就全量重编
     * 11 个 TU。分开放之后两边各留一份，切换零成本。 */
    char stem[2048];
    if (cachedir && *cachedir) {
        char base[1024];
        path_basename(out, base, sizeof base);
        path_strip_ext(base);
        /* 别写 `g_cflags == CC_REL`：那是拿指针比字面量地址，只有编译器
         * 恰好把两份字面量合并才成立。合并与否随优化级别变——一次没合并
         * 就是 -O2 与 -O0 的 .o 混进同一个目录，症状是"改个 flag 后
         * 编译出来的程序行为莫名其妙"。按内容认。 */
        const char *mtag = (g_cflags && strstr(g_cflags, "ZX_RELEASE")) ? "-rel" : "-dev";
        snprintf(stem, sizeof stem, "%s/%s%s", cachedir, base, mtag);
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
    long long maxobj = 0;      /* 全部 .o 里最新的 mtime（判"要不要重链"） */

    /* 编译参数（cc 路径 / dev-release 档）记个 stamp：参数变了，磁盘上
     * 的旧 .o 全是别的参数编的，一律作废。首次没有 stamp 也算变了
     * （顺带自愈"旧 .o 带着别的运行时符号"之类的脏状态）。 */
    char fpath[2048], fstamp[512];
    snprintf(fpath, sizeof fpath, "%s.flags", stem);
    snprintf(fstamp, sizeof fstamp, "%s|%s\n", g_cc, g_cflags);
    bool fchanged = write_file_if_changed(fpath, fstamp, strlen(fstamp));

    /* @link 收集的外部库：内容记一份到 .libs。库的增删只影响链接、不影响
     * .o 的内容（.flags 盖不住它），所以单独盖章：变过就强制重链。 */
    bool lchanged = false;
    if (kind != OUT_STATIC) {
        Buf ls; buf_init(&ls);
        for (int i = 0; i < g_nlink_libs; i++) {
            buf_puts(&ls, g_link_libs[i]);
            buf_putc(&ls, '\n');
        }
        char lpath[2048];
        snprintf(lpath, sizeof lpath, "%s.libs", stem);
        lchanged = write_file_if_changed(lpath, ls.data ? ls.data : "", ls.len);
        buf_free(&ls);
    }

    /* 收集单元：先落盘 .c 并决定谁要重编（等全部落盘后并行开火） */
    for (int pass = 0; pass <= n; pass++) {
        const char *uname = (pass == 0) ? "zxrt" : units[pass - 1].name;
        const char *src   = (pass == 0) ? (rt.data ? rt.data : "")
                                        : (units[pass - 1].src.data ? units[pass - 1].src.data : "");
        char cpath[2048], opath[2048];
        snprintf(cpath, sizeof cpath, "%s_%s.c", stem, uname);
        snprintf(opath, sizeof opath, "%s_%s.o", stem, uname);
        {
            long long om = fmtime(opath);
            if (om > maxobj) maxobj = om;
        }

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
            j->unit = pass ? pass - 1 : -1;
            snprintf(j->cpath, sizeof j->cpath, "%s", cpath);
            snprintf(j->log, sizeof j->log, "%s_%s.log", stem, uname);
            /* 输出重定向到日志再翻译：生成的 C 的警告（libc 原型重声明之类）
             * 与写 zx 的人无关，默认 -w 关掉；失败时的报错从这里翻回源码位置。 */
            const char *warnflag = getenv("ZX_VERBOSE") ? "-Wall" : "-w";
            snprintf(j->cmd, sizeof j->cmd,
                     "%s %s %s -fno-caret-diagnostics -fno-color-diagnostics "
                     "-c -o \"%s\" \"%s\" > \"%s\" 2>&1",
                     g_cc, g_cflags, warnflag, opath, cpath, j->log);
        }

        if (pass) buf_puts(&objs, " ");
        buf_printf(&objs, "\"%s\"", opath);
    }

    double t_cc = prog_clock();
    if (njobs > 0) prog("compiling %d unit%s...", njobs, njobs == 1 ? "" : "s");
    int ccfail = run_cc_jobs(g_jobs, njobs);
    if (njobs > 0)
        prog(ccfail ? "compiling failed (%.2f s)" : "compiled ok (%.2f s)",
             prog_clock() - t_cc);
    /* 成功时把日志清掉（失败时留着，ZX_TRACE/ZX_VERBOSE 还要看） */
    if (!ccfail && !getenv("ZX_VERBOSE"))
        for (int i = 0; i < njobs; i++) remove(g_jobs[i].log);
    if (ccfail) {
        /* 把 C 编译器的抱怨翻成 zx 侧诊断（模块 + 行 + 人话），逐条报出来 */
        int nerr = 0;
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < njobs; i++)
                if (g_jobs[i].rc) nerr += report_cc_log(&g_jobs[i], pass);
        if (nerr == 0)
            fprintf(stderr, "zxc: build failed in the C backend (rerun with ZX_VERBOSE=1 for raw output)\n");
        else
            fprintf(stderr, "zxc: build failed: %d code-generation error(s) "
                            "(compiler bug, not a problem in your zx source)\n", nerr);
        return 1;
    }

    /* 一个单元都没重编、产物又比所有 .o 新：整体已经是最新的，别动链接器。
     * 链接是"无改动重建"里最贵的一步——zig cc 每次调用有约 200 ms 固定开销
     * （驱动启动 + lld + 自带 libc），占掉这条路线的七成。
     *
     * 判据必须挂在 njobs == 0 上，不能只看时间戳：Windows 的 st_mtime 是秒级，
     * 同一秒内重编出的 .o 与产物无法分先后（见上面 remove(opath) 那段注释）。
     * 有重编就一定重链，宁可多跑一次也不发旧二进制。
     * vendor/ 的 C 库刚重编过（csrc_relink）同样不许跳过链接。 */
    char absout[2048];
    path_abs(out, absout, sizeof absout);
    if (njobs == 0 && !csrc_relink) {
        long long om = fmtime(absout);
        if (om > 0 && om >= maxobj && !lchanged) {
            if (getenv("ZX_VERBOSE")) fprintf(stderr, "  link skipped (up to date: %s)\n", out);
            prog("up to date: %s (%.2f s total)", out, prog_clock() - g_prog_t0);
            return 0;
        }
    }

    /* 链接 / 归档：exe 直接链；动态库 -shared；静态库用配套的 ar 收拢 .o。
     * 最终产物的 -o 一律用绝对路径：zig cc 0.15 的 -shared 模式下
     * 相对子目录路径（out/x.dll）会报 "No such file or directory"
     * （exe / ar 无此毛病，绝对路径统一绕开）。 */
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
        /* @link("ws2_32") 收集的外部库：名字走 -l，带路径分隔符或扩展名的
         * 当文件路径原样传（vendor 目录里的 .lib / .a）。 */
        for (int i = 0; i < g_nlink_libs; i++) {
            const char *L = g_link_libs[i];
            if (strchr(L, '/') || strchr(L, '\\') || strchr(L, '.'))
                snprintf(cmd + strlen(cmd), sizeof cmd - strlen(cmd), " \"%s\"", L);
            else
                snprintf(cmd + strlen(cmd), sizeof cmd - strlen(cmd), " -l%s", L);
        }
    }
    if (getenv("ZX_VERBOSE")) fprintf(stderr, "  link cmd: %s\n", cmd);
    prog("%s...", kind == OUT_STATIC ? "archiving" : "linking");
    double t_link = prog_clock();
    {
        char llog[2048];
        snprintf(llog, sizeof llog, "%s.link.log", stem);
        char lcmd[8192 + 256];
        snprintf(lcmd, sizeof lcmd, "%s > \"%s\" 2>&1", cmd, llog);
        if (system(lcmd) != 0) {
            report_link_log(llog, kind);
            remove(llog);
            return 1;
        }
        remove(llog);
    }
    prog("%s ok (%.2f s) | done: %s (%.2f s total)",
         kind == OUT_STATIC ? "archived" : "linked", prog_clock() - t_link,
         out, prog_clock() - g_prog_t0);
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

/* 用法 / 版本 / 逐个子命令帮助。
 *
 * 分层的理由：`-h` 是"我忘了怎么写"，要能一眼扫完；跑题了才去翻
 * `zxc build -h` 这种细节。所以总览只留一行一条，子命令帮助才写旗标
 * 语义与产物路径。两处都打到 stdout 并 rc=0——这是用户主动要的信息，
 * 不是错误（参数写错时才走 stderr + rc=2）。 */
static void usage(FILE *f) {
    fprintf(f,
        "zxc %s -- Zx language compiler\n"
        "\n"
        "usage:\n"
        "  zxc <file.zx> [options]          parse + type check (default)\n"
        "  zxc <file.zx> -o <prog>          generate C and build an executable\n"
        "  zxc <subcommand> [args]          project-level ops (reads zx.json in cwd)\n"
        "\n"
        "project subcommands:\n"
        "  init [name]                      create a project: src/ sources, vendor/ module root, zx.json\n"
        "  add <name|header.h|dir>       add a C library: generate a Zx binding into vendor/\n"
        "  add ./third_party/foo          point at a library tree; no files are copied\n"
        "  cbuild [group]                 compile vendor/*.c into lib<group>.a (auto on add/build)\n"
        "  build [options]                  build per zx.json into out/ (intermediates in .zxc-cache/)\n"
        "  run [options] [-- prog args...]  build, then run\n"
        "  test [name filter]               run src/test/ cases (zero-arg pub functions)\n"
        "  check [file...]                  static check (defaults to cwd, falls back to src/, src/test/)\n"
        "  fmt [file...] [--check]          format (--check reports without writing)\n"
        "  errors                           list all error codes (by category)\n"
        "  explain <CODE>                   explain one error code: meaning and fix\n"
        "\n"
        "single-file options:\n"
        "  --check        check only (default behavior, explicit form)\n"
        "  --tokens       print the token stream\n"
        "  --ast          print the syntax tree (s-expression)\n"
        "  --emit-c       print generated C to stdout only\n"
        "  -o <path>      output executable\n"
        "  --release      -O2 release profile (default dev profile is -O0)\n"
        "  --fe=zx|c      frontend choice (default zx; c is for cross-validation only)\n"
        "\n"
        "general:\n"
        "  -h, --help     show this help; add a subcommand for details (zxc build -h)\n"
        "  -v, --version  show version\n"
        "  help [cmd]     same as -h / -h <cmd>\n"
        "\n"
        "environment:\n"
        "  ZX_CC          C compiler command (default ../.toolchain/zig.exe cc next to zxc)\n"
        "  ZX_VERBOSE=1   print build details (cc command lines, raw backend errors)\n",
        ZXC_VERSION);
}

static void print_version(FILE *f) {
    fprintf(f, "zxc %s\n", ZXC_VERSION);
}

/* 子命令级帮助。返回 0 = 认得的子命令，-1 = 没有这个子命令。 */
static int help_sub(FILE *f, const char *cmd) {
    if (!strcmp(cmd, "cbuild")) {
        fprintf(f,
            "zxc cbuild [group] [options] -- compile C sources in vendor/\n"
            "\n"
            "  zxc cbuild              compile every *.c group + every vendor/*.clib\n"
            "  zxc cbuild mini         only group / manifest named 'mini'\n"
            "  zxc cbuild --force      ignore timestamps, rebuild all\n"
            "  zxc cbuild --release    use the -O2 profile\n"
            "\n"
            "Normally you do not need this: `zxc add` compiles sibling .c files\n"
            "and `zxc build` recompiles them incrementally when a .c/.h changes.\n"
            "C sources use the exact same cc + flags as the C that zxc generates,\n"
            "so there is no sanitizer / ABI mismatch at link time.\n"
            "Grouping: <stem>.c with <stem>.h -> lib<stem>.a; mini_extra.c also\n"
            "joins libmini.a; a .c without a matching header gets its own group.\n"
            "A vendor/<name>.clib manifest (made by `zxc add <dir>`) builds all its\n"
            "listed sources into one lib<name>.a, whatever their directory depth.\n");
        return 0;
    }
    if (!strcmp(cmd, "add")) {
        fprintf(f,
            "zxc add <name|header.h|dir> [options] -- add a C library, no manual bindings\n"
            "\n"
            "  zxc add ../third_party/foo    point at a LIBRARY DIRECTORY (recommended)\n"
            "  zxc add ./vendor/raylib.h     parse one local header -> vendor/raylib.zx\n"
            "  zxc add sqlite3               built-in recipe: locate header + set @link\n"
            "  zxc add --list                show built-in recipes\n"
            "\n"
            "Directory mode: zxc walks the library's own tree (include/, src/, any\n"
            "number of nested dirs), writes vendor/<name>.clib listing what it found,\n"
            "and compiles those sources into one lib<name>.a. NO FILES ARE COPIED --\n"
            "the library can live anywhere. Comment out lines in the .clib to trim.\n"
            "\n"
            "options:\n"
            "  --lib NAME     override the library passed to @link\n"
            "  --name NAME    override the module name in directory mode\n"
            "  --no-build     skip compiling C sources\n"
            "  --force        overwrite an existing vendor/<name>.zx\n"
            "\n"
            "The generated file is ordinary, editable Zx source:\n"
            "  const ray = module(\"raylib\");   ray.InitWindow(800, 600, \"hi\");\n"
            "Completion / hover / inlay hints work on it like any other module.\n"
            "Declarations clang cannot translate are listed as comments at the end.\n"
            "C sources are compiled into a static library and linked automatically;\n"
            "zxc build recompiles them on change. tests/examples/build dirs are skipped.\n"
            "No network access and no package download involved.\n");
        return 0;
    }
    if (!strcmp(cmd, "init")) {
        fprintf(f,
            "zxc init [name] -- create a zx project\n"
            "\n"
            "  zxc init          create in the current directory (project name = dir name)\n"
            "  zxc init demo     create demo/ and scaffold inside it\n"
            "\n"
            "generated:\n"
            "  src/main.zx       entry (example: module(\"std\") + greet/main)\n"
            "  zx.json           project manifest: name / version / main / out / deps\n"
            "  vendor/           external module root (see below)\n"
            "  README.md .gitignore\n"
            "\n"
            "vendor/:\n"
            "  drop third-party *.zx modules here and module(\"name\") finds them:\n"
            "  vendor/ext.zx -> module(\"ext\"), vendor/lib/util.zx -> module(\"lib/util\").\n"
            "  search order: dir of the importing file -> vendor/ -> deps in zx.json;\n"
            "  vendor/ joins the search automatically once it exists, no deps entry needed.\n"
            "\n"
            "src/main.zx is checked once after scaffolding; failures return 1.\n");
        return 0;
    }
    if (!strcmp(cmd, "build")) {
        fprintf(f,
            "zxc build [options] -- build per zx.json\n"
            "\n"
            "  --exe        executable (default)\n"
            "  --lib        library: static .a by default, pair with --shared for .dll/.so\n"
            "  --static     static library (default)\n"
            "  --shared     shared library (requires --lib)\n"
            "  --release    -O2 release profile; default dev profile is -O0 (fast iteration)\n"
            "\n"
            "outputs: out/<out> (exe) / out/<out>.a / out/<out>.dll\n"
            "intermediates: .zxc-cache/<out>_*.c|.o|.h (unchanged files are not recompiled)\n"
            "backend C errors are mapped back to zx source lines; ZX_VERBOSE=1 for raw output.\n");
        return 0;
    }
    if (!strcmp(cmd, "run")) {
        fprintf(f,
            "zxc run [--release] [-- prog args...] -- build, then run immediately\n"
            "\n"
            "builds once, then runs out/<out>.exe; the exit code is the program's.\n"
            "program arguments must come after `--` (otherwise they are read as zxc flags).\n");
        return 0;
    }
    if (!strcmp(cmd, "test")) {
        fprintf(f,
            "zxc test [name filter] [--release] -- run test cases\n"
            "\n"
            "convention: every zero-arg pub function in src/test/*.zx is a case (no\n"
            "test_ prefix needed; functions with params/generics count as helpers). The\n"
            "name filter is substring match.\n"
            "failure: @trap, returning false, returning a non-zero integer, or std.process.exit(non-zero).\n"
            "each case runs in its own process, so one crash does not affect the rest; tests\n"
            "reference project sources via module(\"../xxx.zx\").\n");
        return 0;
    }
    if (!strcmp(cmd, "check")) {
        fprintf(f,
            "zxc check [file...] -- static check (no artifacts)\n"
            "\n"
            "parse + type check only. Without files: *.zx in cwd first, falling back\n"
            "to src/ and src/test/ (project mode adds deps / vendor search paths and\n"
            "reads zx.json automatically). Returns 1 on errors, else 0 -- fits a pre-commit hook.\n");
        return 0;
    }
    if (!strcmp(cmd, "fmt")) {
        fprintf(f,
            "zxc fmt [file...] [--check] -- format\n"
            "\n"
            "normalizes indentation (4 spaces), strips trailing whitespace, collapses\n"
            "blank runs to 1, ensures a single trailing newline; CRLF stays CRLF.\n"
            "Whitespace only, no semantic changes.\n"
            "  --check   report which files need formatting without writing (exit 1 means diff)\n");
        return 0;
    }
    if (!strcmp(cmd, "version")) { print_version(f); return 0; }
    fprintf(f, "zxc: no help for '%s'\n\n", cmd);
    usage(f);
    return -1;
}

/* 子命令参数里有没有 -h / -v（`--` 之后是程序参数，不算）。
 * 返回 1 = 要帮助，2 = 要版本，0 = 都不是。 */
static int arg_special_flag(int argc, char **argv, int from) {
    for (int i = from; i < argc; i++) {
        if (!strcmp(argv[i], "--")) break;
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) return 1;
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "-V") ||
            !strcmp(argv[i], "--version")) return 2;
    }
    return 0;
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

/* 目录收集：先看当前目录，没有 .zx 再看 src/（配合 init 的 src/ 布局）；
 * src/ 之外再补上 src/test/（测试模块也要被 check/fmt 覆盖）。
 * collect_zx_files 返回的路径已带目录前缀，直接拼接即可。 */
static char **collect_zx_files_smart(int *out_n) {
    char **files = collect_zx_files(NULL, out_n);
    if (files) return files;
    files = collect_zx_files("src", out_n);
    int n = *out_n, k = 0;
    char **sub = collect_zx_files("src/test", &k);
    if (!sub) return files;
    files = realloc(files, (size_t)(n + k) * sizeof *files);
    for (int i = 0; i < k; i++) files[n++] = sub[i];   /* 已带 src/test/ 前缀 */
    free(sub);
    *out_n = n;
    return files;
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

/* 工程清单（zx.json）。定义放前面：check 段也要读 deps。 */
struct Proj {
    bool ok;
    char name[256];     /* 工程名 */
    char main[1024];    /* 入口源文件 */
    char out[256];      /* 产物名（缺省 = 工程名） */
    char *deps[16];     /* "deps" 数组：模块搜索目录（相对工程根） */
    int  ndeps;
};
typedef struct Proj Proj;
static bool proj_load(Proj *pj);
static void setup_search_paths(const Proj *pj);

static int cmd_check(int argc, char **argv, int argi) {
    int nfiles = 0;
    char **files = NULL;
    /* 有 zx.json 就把 deps 搜索路径带上（检查也走模块解析） */
    Proj pj;
    if (proj_load(&pj)) setup_search_paths(&pj);
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

/* vendor/ 是外部模块根：放进来就能被 module("名字") 找到。空目录进不了
 * 版本控制，所以里面放一份说明——顺便把搜索顺序写在最容易被看到的地方。 */
static const char *INIT_VENDOR_README =
    "# vendor/ — 外部模块根\n"
    "\n"
    "把第三方 zx 模块（`*.zx`，可带子目录）放在这里，源码里直接引用：\n"
    "\n"
    "```zx\n"
    "const ext = module(\"ext\");        // → vendor/ext.zx\n"
    "const util = module(\"lib/util\");  // → vendor/lib/util.zx\n"
    "```\n"
    "\n"
    "搜索顺序：相对导入所在的目录 → 工程根的 `vendor/` → `zx.json` 的\n"
    "`\"deps\"` 数组。`vendor/` 存在即自动参与搜索，不用写进 deps。\n"
    "\n"
    "vendor/ 里的是源码依赖，应该提交进版本控制。\n";

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
    "zxc test              # 运行 src/test/ 里的测试（零参 pub 函数即用例）\n"
    "zxc check             # 静默检查全部 *.zx（src/ 下也扫）\n"
    "zxc fmt               # 格式化全部 *.zx\n"
    "```\n"
    "\n"
    "## 模块\n"
    "\n"
    "`src/` 下的 `*.zx` 都是可用模块（与导入文件同目录即可）：\n"
    "`const m = module(\"m\");` 或 `module(\"m/inner.zx\")` 引子目录模块。\n"
    "标准库见 `std.zx`。\n"
    "\n"
    "第三方 / 外部模块放 `vendor/`（外部模块根，存在即自动参与搜索）：\n"
    "`vendor/ext.zx` → `module(\"ext\")`；再多层目录写\n"
    "`module(\"lib/util\")` → `vendor/lib/util.zx`。要额外的搜索目录，\n"
    "把相对路径写进 `zx.json` 的 `\"deps\"` 数组。\n"
    "\n"
    "## 测试\n"
    "\n"
    "`src/test/` 下的 `*.zx` 都是测试模块：里面的零参 `pub` 函数全部是用例\n"
    "（不用 test_ 前缀；带参数/泛型的函数可当辅助）。引用工程源码用\n"
    "`module(\"../xxx.zx\")`。`zxc test [名字过滤]` 运行，每个用例独立进程。\n";

static int write_text_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "zxc: cannot write '%s'\n", path); return -1; }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
    return 0;
}

static int cmd_init(int argc, char **argv, int argi) {
    char namebuf[256];
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
    /* "zxc init ." 在当前目录落脚：工程名不能照抄 "."（产物会成 "..exe"），
     * 取当前目录的 basename，取不到就回落 "app" */
    if (argi < argc && (!strcmp(name, ".") || !strcmp(name, ".."))) {
#ifdef _WIN32
        if (_getcwd(namebuf, sizeof namebuf)) {
#else
        if (getcwd(namebuf, sizeof namebuf)) {
#endif
            const char *base = strrchr(namebuf, '/');
#ifdef _WIN32
            const char *b2 = strrchr(namebuf, '\\');
            if (b2 && (!base || b2 > base)) base = b2;
#endif
            name = (base && base[1]) ? base + 1 : "app";
        } else {
            name = "app";
        }
        if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..")) name = "app";
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
    /* vendor/：默认的外部模块根。setup_search_paths 见目录即自动加入
     * 搜索路径，所以建了就有用，不用在 zx.json 里再写一遍。空目录进不了
     * 版本控制，里面放一份说明顺带当文档。 */
    if (mkdir_one("vendor") != 0) {
        fprintf(stderr, "zxc: cannot create 'vendor/'\n");
        return 1;
    }
    if (write_text_file("vendor/README.md", INIT_VENDOR_README) != 0) return 1;
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
    printf("zxc init: created src/main.zx, zx.json, README.md, .gitignore, vendor/%s\n",
           ok ? " (main.zx check ok)" : " (warning: main.zx check FAILED)");
    return ok ? 0 : 1;
}

/* ---- add：一条命令把 C 库接进工程 ------------------------------------
 * 不做包管理器：不联网、不拉源码。消灭的是手写 FFI 里最费心的那一步——
 * 抄函数签名、抄 C 结构体布局。头文件交给 vendored clang 解析，产物是
 * 一份普通 Zx 源码放进 vendor/，之后 module("名字") 直接调。
 * 生成物是普通源码，所以 LSP 的补全 / hover / inlayHint 全部照常生效。
 */
typedef struct {
    const char *alias;      /* zxc add <alias> */
    const char *header;     /* 头文件名 */
    const char *lib;        /* @link 名，"" = 不用额外链接 */
} CRecipe;

/* 常见库配方：给个名字就能定位头文件 + 自动写 @link。
 * 本地找不到头文件副本时，用 `#include <...>` 走工具链自带的 SDK 头。 */
static const CRecipe g_crecipes[] = {
    { "c",        "stdio.h",     "" },
    { "libc",     "stdio.h",     "" },
    { "m",        "math.h",      "" },
    { "ws2_32",   "winsock2.h",  "ws2_32" },
    { "user32",   "windows.h",   "user32" },
    { "gdi32",    "windows.h",   "gdi32" },
    { "kernel32", "windows.h",   "kernel32" },
    { "opengl32", "windows.h",   "opengl32" },
    { "sqlite3",  "sqlite3.h",   "sqlite3" },
    { "z",        "zlib.h",      "z" },
    { "zlib",     "zlib.h",      "z" },
    { "raylib",   "raylib.h",    "raylib" },
    { "sdl2",     "SDL.h",       "SDL2" },
    { NULL, NULL, NULL }
};

static const CRecipe *crecipe_of(const char *name) {
    for (int i = 0; g_crecipes[i].alias; i++)
        if (!strcmp(g_crecipes[i].alias, name)) return &g_crecipes[i];
    return NULL;
}

static void cadd_stem(const char *s, char *out, size_t cap) {
    path_basename(s, out, cap);
    path_strip_ext(out);
    for (char *q = out; *q; q++) {
        int ok = (*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
                 (*q >= '0' && *q <= '9') || *q == '_';
        if (!ok) *q = '_';
    }
    if (!out[0]) snprintf(out, cap, "clib");
}

/* 参数是目录 -> 目录树模式；是 .h 文件 -> 单头模式；否则走内置配方。 */
static int path_is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && (st.st_mode & S_IFDIR) != 0;
}

/* 取头文件所在目录：C 源码与头放一起，编出的库也落在那里。 */
static void cadd_dir_of(const char *path, char *out, size_t cap) {
    const char *slash = NULL;
    for (const char *q = path; *q; q++)
        if (*q == '/' || *q == '\\') slash = q;
    if (!slash) { snprintf(out, cap, "."); return; }
    size_t n = (size_t)(slash - path);
    if (!n) n = 1;
    if (n >= cap) n = cap - 1;
    memcpy(out, path, n); out[n] = 0;
}

/* 头文件旁边若有同名 .lib/.a，自动挂上，省得再敲 --lib。 */
static int cadd_probe_lib(const char *stem, char *buf, size_t cap) {
    static const char *pat[] = {
        "vendor/%s.lib", "vendor/lib%s.a", "vendor/%s.a",
        "%s.lib", "lib%s.a", "%s.a", NULL
    };
    for (int i = 0; pat[i]; i++) {
        snprintf(buf, cap, pat[i], stem, stem);
        if (fexists(buf)) return 1;
    }
    return 0;
}

/* ---- 目录树模式：zxc add <库目录> --------------------------------------
 * 库文件保持原位：include/ 与 src/ 分离、任意深度子目录、甚至工程外的路径。
 * 只写一张 vendor/<name>.clib 清单（编译输入，可直接注释行裁剪），
 * 再把清单里所有 .c 合成一个 lib<name>.a。一个文件都不复制。
 */
static int cmd_add_tree(const char *root, const char *uname,
                        const char *ulib, int force, int no_build) {
    char err[1024];
    ClibManifest m;
    if (clib_discover(root, uname, &m, err, sizeof err) != 0) {
        fprintf(stderr, "zxc add: %s\n", err);
        return 1;
    }
    if (mkdir_one("vendor") != 0) {
        fprintf(stderr, "zxc add: cannot create vendor/\n");
        clib_free(&m); return 1;
    }
    char modname[256];
    snprintf(modname, sizeof modname, "%s", m.name);
    char regen[1200];
    if (uname) snprintf(regen, sizeof regen, "%s --name %s", root, uname);
    else snprintf(regen, sizeof regen, "%s", root);
    char mf[1024], zxpath[1024], libpath[1024];
    snprintf(mf, sizeof mf, "vendor/%s.clib", modname);
    snprintf(zxpath, sizeof zxpath, "vendor/%s.zx", modname);
    snprintf(libpath, sizeof libpath, "vendor/lib%s.a", modname);
    if (!force && (fexists(mf) || fexists(zxpath))) {
        fprintf(stderr, "zxc add: '%s' already exists (use --force to overwrite)\n",
                fexists(mf) ? mf : zxpath);
        clib_free(&m); return 1;
    }
    if (clib_write_manifest(mf, &m, root, regen, err, sizeof err) != 0) {
        fprintf(stderr, "zxc add: %s\n", err);
        clib_free(&m); return 1;
    }
    printf("zxc add: %s\n", root);
    printf("  found %d header(s), %d include dir(s), %d C source(s)\n",
           m.nhdr, m.ninc, m.nsrc);
    printf("  wrote %s -- nothing copied; comment out lines to trim\n", mf);

    /* 同一库改 --name 再 add 一次会留下两份清单：build 会编两遍、注入两个
     * 内容相同的 .a。只提醒，不擅自删用户文件。 */
    int nmans = 0;
    char **omans = clib_scan_manifests("vendor", &nmans);
    for (int i = 0; i < nmans; i++) {
        if (strcmp(omans[i], mf)) {
            ClibManifest om;
            char oerr[256];
            if (clib_read_manifest(omans[i], &om, oerr, sizeof oerr) == 0) {
                if (!strcmp(om.root, root))
                    printf("  note: %s points at the same library;"
                           " delete it if this was a rename\n", omans[i]);
                clib_free(&om);
            }
        }
        free(omans[i]);
    }
    free(omans);

    char header[1024];
    if (clib_umbrella(&m, ZX_CACHE_DIR, header, sizeof header,
                      err, sizeof err) != 0) {
        fprintf(stderr, "zxc add: %s\n", err);
        clib_free(&m); return 1;
    }
    char *incs = clib_inc_flags(&m);
    if (!incs) {
        fprintf(stderr, "zxc add: out of memory\n");
        clib_free(&m); return 1;
    }

    const char *lib = ulib;
    if (!no_build && !ulib && m.nsrc > 0) {
        int ncomp = 0;
        if (clib_build(g_cc, g_cflags ? g_cflags : CC_DEV, mf, ZX_CACHE_DIR, 0,
                       libpath, sizeof libpath, err, sizeof err, &ncomp) < 0) {
            fprintf(stderr, "zxc add: %s\n", err);
            free(incs); clib_free(&m); return 1;
        }
        printf("zxc add: compiled %d C unit(s) -> %s\n", ncomp, libpath);
    }
    /* 纯头库不写 @link；其它情况即使 --no-build 也先把路径写进绑定，
     * 之后 zxc build 会自动把库编出来。 */
    if (!lib && m.nsrc > 0) lib = libpath;

    int nskip = 0;
    char *text = cbind_generate(header, g_cc, lib, 0, incs,
                                (const char *const *)m.hdr, m.nhdr, regen,
                                err, sizeof err, &nskip);
    free(incs);
    clib_free(&m);
    if (!text) { fprintf(stderr, "zxc add: %s\n", err); return 1; }
    int rc = write_text_file(zxpath, text);
    free(text);
    if (rc != 0) return 1;
    printf("zxc add: wrote %s (%d declaration(s) skipped)\n", zxpath, nskip);
    printf("  use:  const %s = module(\"%s\");\n", modname, modname);
    if (lib && lib[0])
        printf("  link: @link(\"%s\") is already in the binding\n", lib);
    else
        printf("  note: header-only, nothing to link\n");
    printf("  later: edit %s, then `zxc build` recompiles automatically\n", mf);
    return 0;
}

static int cmd_add(int argc, char **argv, int argi) {
    const char *name = NULL, *ulib = NULL, *uname = NULL;
    int force = 0, list = 0, no_build = 0;
    for (int i = argi; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) list = 1;
        else if (!strcmp(argv[i], "--force")) force = 1;
        else if (!strcmp(argv[i], "--no-build")) no_build = 1;
        else if (!strcmp(argv[i], "--name")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "zxc add: --name needs a module name\n");
                return 2;
            }
            uname = argv[++i];
        } else if (!strcmp(argv[i], "--lib")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "zxc add: --lib needs a library name\n");
                return 2;
            }
            ulib = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "zxc add: unknown option '%s'\n", argv[i]);
            return 2;
        } else if (!name) name = argv[i];
        else {
            fprintf(stderr, "zxc add: unexpected argument '%s'\n", argv[i]);
            return 2;
        }
    }
    if (list) {
        printf("zxc add -- built-in C library recipes\n\n");
        printf("  name        header        link\n");
        for (int i = 0; g_crecipes[i].alias; i++)
            printf("  %-11s %-13s %s\n", g_crecipes[i].alias,
                   g_crecipes[i].header,
                   g_crecipes[i].lib[0] ? g_crecipes[i].lib : "(none)");
        printf("\nA plain .h path works too:  zxc add ./vendor/foo.h\n");
        printf("Use --lib NAME / --force to override the link name / overwrite.\n");
        return 0;
    }
    if (!name) {
        fprintf(stderr, "usage: zxc add <name|header.h|dir> [--name NAME] [--lib NAME] [--no-build] [--force]\n"
                        "       zxc add --list\n");
        return 2;
    }

    /* 目录参数 = 库原本的目录树：不复制文件，走清单模式。 */
    if (path_is_dir(name)) return cmd_add_tree(name, uname, ulib, force, no_build);

    const CRecipe *r = crecipe_of(name);
    char header[1024] = "";
    const char *want_h = r ? r->header : NULL;
    int allow_system = 0;

    /* 1) 显式头文件路径 */
    if (!r && fexists(name)) snprintf(header, sizeof header, "%s", name);
    /* 2) 本地能找到的头文件：配方名 / 当前目录 / vendor/ */
    if (!header[0]) {
        const char *stem = want_h ? want_h : name;
        char cand[1024];
        if (fexists(stem)) snprintf(header, sizeof header, "%s", stem);
        if (!header[0]) {
            snprintf(cand, sizeof cand, "vendor/%s", stem);
            if (fexists(cand)) snprintf(header, sizeof header, "%s", cand);
        }
        if (!header[0]) {
            snprintf(cand, sizeof cand, "%s.h", name);
            if (fexists(cand)) snprintf(header, sizeof header, "%s", cand);
        }
        if (!header[0]) {
            snprintf(cand, sizeof cand, "vendor/%s.h", name);
            if (fexists(cand)) snprintf(header, sizeof header, "%s", cand);
        }
    }
    /* 3) 都没有 → 落一个包装头，走工具链自带的系统 SDK 头 */
    if (!header[0]) {
        char incbuf[512];
        const char *inc = want_h;
        if (!inc) { snprintf(incbuf, sizeof incbuf, "%s.h", name); inc = incbuf; }
        if (mkdir_one(".zxc-cache") != 0) {
            fprintf(stderr, "zxc add: cannot create .zxc-cache/\n");
            return 1;
        }
        char wrap[1024];
        snprintf(wrap, sizeof wrap, "#include <%s>\n", inc);
        if (write_text_file(".zxc-cache/zxc-add-wrap.h", wrap) != 0) return 1;
        snprintf(header, sizeof header, ".zxc-cache/zxc-add-wrap.h");
        allow_system = 1;
        printf("zxc add: <%s> not found locally, using toolchain headers\n", inc);
    }

    char stem[256];
    cadd_stem(r ? name : header, stem, sizeof stem);

    /* 头文件旁边有 .c：用 zxc 自己的 cc + flags 编成静态库并自动 @link。
     * 这一步根治了"手工编的 C 库和 zx 的 sanitizer 档位对不上"的链接错误。 */
    char builtlib[1024] = "";
    if (!no_build && !ulib && !allow_system) {
        char hdir[1024];
        cadd_dir_of(header, hdir, sizeof hdir);
        char **libs = NULL, cerr[1024];
        int ncomp = 0;
        int nlib = csrc_build(g_cc, g_cflags ? g_cflags : CC_DEV,
                              hdir, ZX_CACHE_DIR, stem, 0,
                              &libs, cerr, sizeof cerr, &ncomp);
        if (nlib < 0) {
            fprintf(stderr, "zxc add: %s\n", cerr);
            return 1;
        }
        if (nlib > 0) {
            snprintf(builtlib, sizeof builtlib, "%s", libs[0]);
            if (ncomp > 0) printf("zxc add: compiled %d C unit(s) -> %s\n", ncomp, builtlib);
            else printf("zxc add: %s is up to date\n", builtlib);
        }
        for (int i = 0; i < nlib; i++) free(libs[i]);
        free(libs);
    }

    char libbuf[1024] = "";
    const char *lib = ulib;
    if (!lib && builtlib[0]) lib = builtlib;
    if (!lib && r && r->lib[0]) lib = r->lib;
    if (!lib && !allow_system) {
        char probe[1024];
        if (cadd_probe_lib(stem, probe, sizeof probe)) {
            snprintf(libbuf, sizeof libbuf, "%s", probe);
            lib = libbuf;
            printf("zxc add: found local library %s, linking it\n", probe);
        }
    }

    if (mkdir_one("vendor") != 0) {
        fprintf(stderr, "zxc add: cannot create vendor/\n");
        return 1;
    }
    char outpath[1024];
    snprintf(outpath, sizeof outpath, "vendor/%s.zx", stem);
    if (!force && fexists(outpath)) {
        fprintf(stderr, "zxc add: '%s' already exists (use --force to overwrite)\n", outpath);
        return 1;
    }

    char err[1024];
    int nskip = 0;
    char *text = cbind_generate(header, g_cc, lib, allow_system, NULL, NULL, 0, name,
                                err, sizeof err, &nskip);
    if (!text) { fprintf(stderr, "zxc add: %s\n", err); return 1; }
    int rc = write_text_file(outpath, text);
    free(text);
    if (rc != 0) return 1;

    printf("zxc add: wrote %s (%d declaration(s) skipped)\n", outpath, nskip);
    printf("  use:  const %s = module(\"%s\");\n", stem, stem);
    if (lib && lib[0])
        printf("  link: @link(\"%s\") is already in the binding\n", lib);
    else
        printf("  note: no library linked yet; rerun with --lib NAME if the linker complains\n");
    if (!fexists("zx.json"))
        printf("  note: no zx.json here; run this inside a project (or use vendor/ directly)\n");
    return 0;
}

/* ---- cbuild：显式重编 vendor/ 下的 C 源码 ----------------------------
 * 平时用不到：add 会编一次，build 会在 .c/.h 变化时自动增量重编。
 * 留这个入口是为了"只想编库、不想编整个 zx 工程"和排查 C 编译错误。 */
static int cmd_cbuild(int argc, char **argv, int argi) {
    const char *group = NULL;
    int force = 0, release = 0;
    for (int i = argi; i < argc; i++) {
        if      (!strcmp(argv[i], "--force"))   force = 1;
        else if (!strcmp(argv[i], "--release")) release = 1;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "zxc cbuild: unknown option '%s'\n", argv[i]);
            return 2;
        } else if (!group) group = argv[i];
        else {
            fprintf(stderr, "zxc cbuild: unexpected argument '%s'\n", argv[i]);
            return 2;
        }
    }
    if (!fexists("vendor")) {
        fprintf(stderr, "zxc cbuild: vendor/ not found (run inside a project)\n");
        return 2;
    }
    g_cflags = release ? CC_REL : CC_DEV;
    char **libs = NULL, err[1024];
    int ncomp = 0;
    int nlib = csrc_build(g_cc, g_cflags, "vendor", ZX_CACHE_DIR, group, force,
                          &libs, err, sizeof err, &ncomp);
    if (nlib < 0) { fprintf(stderr, "zxc cbuild: %s\n", err); return 1; }

    /* 清单模式：`zxc cbuild foo` 只编 vendor/foo.clib，不带名字则编全部。 */
    char **mans = NULL;
    int nman = 0;
    if (group) {
        char mf[1024];
        snprintf(mf, sizeof mf, "vendor/%s.clib", group);
        if (fexists(mf)) {
            mans = malloc(sizeof *mans);
            mans[0] = strdup(mf);
            nman = 1;
        }
    } else {
        mans = clib_scan_manifests("vendor", &nman);
    }
    char **extra = NULL;
    int nextra = 0;
    for (int i = 0; i < nman; i++) {
        char clib[1024];
        int mcomp = 0;
        int mr = clib_build(g_cc, g_cflags, mans[i], ZX_CACHE_DIR, force,
                            clib, sizeof clib, err, sizeof err, &mcomp);
        free(mans[i]);
        if (mr < 0) {
            fprintf(stderr, "zxc cbuild: %s\n", err);
            free(mans); free(extra); free(libs);
            return 1;
        }
        ncomp += mcomp;
        if (mr > 0 && clib[0]) {
            extra = realloc(extra, (size_t)(nextra + 1) * sizeof *extra);
            extra[nextra++] = strdup(clib);
        }
    }
    free(mans);

    if (nlib == 0 && nextra == 0) {
        printf("zxc cbuild: no C sources in vendor/ (nothing to do)\n");
        free(libs); free(extra);
        return 0;
    }
    printf("zxc cbuild: %d C unit(s) recompiled, %d library(ies):\n",
           ncomp, nlib + nextra);
    for (int i = 0; i < nlib; i++) {
        printf("  %s\n", libs[i]);
        free(libs[i]);
    }
    for (int i = 0; i < nextra; i++) {
        printf("  %s\n", extra[i]);
        free(extra[i]);
    }
    free(libs); free(extra);
    printf("  run 'zxc build' to relink\n");
    return 0;
}

/* ---- build / run：读 zx.json 的工程级构建 ---- */

/* 工程中间文件目录 ZX_CACHE_DIR 定义在文件头（build_program 也要用）：
 * 生成的 .c/.o、共享头、编译参数 stamp 都收在这，不再散落在工程根。 */

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

/* 抠 "key": [ "a", "b" ] 字符串数组（deps 字段用） */
static int json_get_str_array(const char *text, const char *key,
                              char **out, int max) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(text, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), '[');
    if (!p) return 0;
    p++;
    int n = 0;
    while (*p && *p != ']' && n < max) {
        if (*p == '"') {
            p++;
            const char *e = strchr(p, '"');
            if (!e) break;
            size_t l = (size_t)(e - p);
            if (l > 0) {
                char *s = malloc(l + 1);
                memcpy(s, p, l);
                s[l] = 0;
                out[n++] = s;
            }
            p = e + 1;
        } else {
            p++;
        }
    }
    return n;
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
    pj->ndeps = json_get_str_array(text, "deps", pj->deps, 16);
    return true;
}

/* deps 搜索路径：工程根的 vendor/（存在即默认加入）+ zx.json "deps" 数组。
 * 注入给 sema：相对导入打不开时依次回退这些目录。 */
static const char *g_search[48];
static void setup_search_paths(const Proj *pj) {
    int n = 0;
    if (fexists("vendor") && n < 47) g_search[n++] = "vendor";
    for (int i = 0; i < pj->ndeps && n < 47; i++) g_search[n++] = pj->deps[i];
    g_search[n] = NULL;
    sema_set_search_paths(g_search);
}

/* 构建 + 产物路径写回 outpath。返回 0 成功。 */
static int do_build(Proj *pj, bool release, OutKind kind, char *outpath, size_t cap) {
    if (!fexists(pj->main)) {
        fprintf(stderr, "zxc: entry source '%s' not found\n", pj->main);
        return 1;
    }
    mkdir_p(ZX_CACHE_DIR);
    mkdir_p("out");
    setup_search_paths(pj);

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

/* ---- test：约定 src/test/ 目录下的 *.zx 都是测试模块，里面的 pub 零参
 * 非泛型函数全部是用例（不需要 test_ 前缀；带参数/泛型函数天然被排除，
 * 可当辅助函数用）。用例失败 = 断言 @trap（注意编译期可达的 @trap 会变成
 * 编译错误）、std.process.exit(非 0)，或返回非 0 整数/false。每个测试文件
 * 编一个 runner 小程序（.zxc-cache/<stem>_runner.zx 生成），main 按命令行
 * 参数分发到单个用例：驱动逐个拉起进程、汇总 PASS/FAIL。进程隔离——
 * 一个用例炸掉不影响后面的用例。测试文件引用工程源码用
 * module("../xxx.zx")（测试目录的上一级就是 src/）。 * ---- */

/* 收集 src/test/ 下的 *.zx。返回 malloc 的路径数组。 */
static char **collect_test_files(int *out_n) {
    return collect_zx_files("src/test", out_n);
}

/* 扫顶层声明收集用例。测试模块里所有零参非泛型函数都是用例，不需要
 * test_ 前缀；带参数/泛型函数天然被排除，可当辅助函数用。返回值决定
 * 失败语义：void → 执行完即过；bool → false 即败；整型 → 非 0 即败；
 * 其余类型忽略返回值。 */
static int collect_test_fns(Node *root, Node **fns, const char **names, int max) {
    int n = 0;
    for (int i = 0; root && i < root->nitems && n < max; i++) {
        Node *d = root->items[i];
        const char *name = NULL;
        Node *fn = NULL;
        if (d->kind == NK_DECL && d->name && d->b && d->b->kind == NK_FN) {
            name = d->name;
            fn = d->b;                                          /* NK_DECL: b = 初值 */
        } else if (d->kind == NK_FN && d->name) {
            name = d->name;
            fn = d;
        }
        if (!fn || sema_fn_is_generic(fn)) continue;
        Type *ft = (fn->ty && fn->ty->kind == TY_TYPE) ? fn->type_val : NULL;
        if (ft && ft->kind == TY_FN && ft->nparams != 0) continue;   /* 需要实参，跳过 */
        fns[n] = fn;
        names[n] = name;
        n++;
    }
    return n;
}

/* 生成 runner 的单个调用语句（按返回类型选失败语义） */
static void gen_test_call(Buf *b, const char *name, const Type *ft) {
    Type *ret = (ft && ft->kind == TY_FN) ? ft->ret : NULL;
    if (ret && ret->kind == TY_BOOL) {
        buf_printf(b, "        if (!t.%s()) { std.process.exit(1); }\n", name);
    } else if (ret && ty_is_int(ret) && ret->kind != TY_COMPTIME_INT) {
        buf_printf(b, "        if (t.%s() != 0) { std.process.exit(1); }\n", name);
    } else {
        buf_printf(b, "        t.%s();\n", name);
    }
}

/* ---- test：约定 src/（或工程根）下的 *_test.zx，里面的 test_* 函数即用例 ----
 *
 * `zxc test` 外层只做轻量 parse 数用例；每个测试文件的 sema/构建/运行
 * 都在独立子进程（--test-run1 内部入口）里完成。为什么开子进程：
 * 匿名 struct 的 C 名字按全局递增的类型 id 生成（ZxS<id>），单进程内
 * 对多个文件逐轮 sema+emit 时 std 会被重新加载、类型 id 漂移，生成的
 * 头文件与函数签名引用对不上号；每文件一个进程回到"单进程一次 emit"
 * 的既有假设，顺带获得崩溃隔离——一个文件把编译器搞炸不影响其余文件。 */
static int test_run1(const char *file, const char *filter, bool release) {
    /* 父进程经 cmd.exe 传参会把 '/' 换成 '\\'；runner 源码里 module("…")
     * 的反斜杠会被当转义序列，词法直接报错——统一换回正斜杠 */
    char fbuf[2048];
    snprintf(fbuf, sizeof fbuf, "%s", file);
    for (char *p = fbuf; *p; p++) if (*p == '\\') *p = '/';
    file = fbuf;

    Proj pj;
    if (!proj_load(&pj)) return 200;
    setup_search_paths(&pj);
    mkdir_p(ZX_CACHE_DIR);
    mkdir_p("out");

    g_error_count = 0;
    g_diag_shown = 0;
    g_srcpath = file;
    Node *root = fe_parse_file(file);
    if (!root || g_error_count) return 200;
    sema_check(root);
    if (g_error_count) return 200;

    Node *fns[256];
    const char *names[256];
    int nt = collect_test_fns(root, fns, names, 256);
    if (nt == 0) return 0;             /* *_test.zx 没有用例：当共享辅助文件 */

    char stem[256], rpath[2048], bpath[2048], modrel[2100];
    path_basename(file, stem, sizeof stem);
    path_strip_ext(stem);
    snprintf(rpath, sizeof rpath, ZX_CACHE_DIR "/%s_runner.zx", stem);
    snprintf(bpath, sizeof bpath, "out/test_%s.exe", stem);
    snprintf(modrel, sizeof modrel, "../%s", file);

    /* runner 落盘（内容没变 mtime 不动，后续构建命中缓存） */
    Buf b;
    buf_init(&b);
    buf_puts(&b, "// zxc test 自动生成的 runner —— 勿手改\n");
    buf_printf(&b, "const t = module(\"%s\");\n", modrel);
    buf_puts(&b, "const std = module(\"std\");\n\n");
    buf_puts(&b, "const main = fn () i32 {\n");
    buf_puts(&b, "    let a = std.process.args();\n");   /* 不含程序名：a[0] 即测试名 */
    buf_puts(&b, "    if (a.len < 1) { std.process.exit(2); }\n");
    for (int i = 0; i < nt; i++) {
        Type *ft = (fns[i]->ty && fns[i]->ty->kind == TY_TYPE)
                   ? fns[i]->type_val : NULL;
        buf_printf(&b, "    if (std.str.eq(a[0], \"%s\")) {\n", names[i]);
        gen_test_call(&b, names[i], ft);
        buf_puts(&b, "        return 0;\n");
        buf_puts(&b, "    }\n");
    }
    buf_puts(&b, "    std.process.exit(2);\n");
    buf_puts(&b, "};\n");
    if (write_text_file(rpath, b.data ? b.data : "") != 0) return 200;

    g_error_count = 0;
    g_diag_shown = 0;
    g_srcpath = rpath;
    Node *rr = fe_parse_file(rpath);
    if (!rr || g_error_count) {
        fprintf(stderr, "zxc test: runner for '%s' failed to build\n", file);
        return 200;
    }
    sema_check(rr);
    if (g_error_count) return 200;
    g_cflags = release ? CC_REL : CC_DEV;
    if (build_program(rr, bpath, ZX_CACHE_DIR, OUT_EXE) != 0) return 200;

    int failed = 0;
    for (int ti = 0; ti < nt; ti++) {
        if (filter && !strstr(names[ti], filter)) continue;
        char cmd[2200];
        snprintf(cmd, sizeof cmd, "\"\"%s\" \"%s\"\"", bpath, names[ti]);
        for (char *p = cmd; *p; p++) if (*p == '/') *p = '\\';
        int rc = system(cmd);
#ifndef _WIN32
        rc = WIFEXITED(rc) ? WEXITSTATUS(rc) : 1;
#endif
        if (rc == 0) {
            printf("PASS  %s.%s\n", stem, names[ti]);
        } else {
            printf("FAIL  %s.%s\n", stem, names[ti]);
            failed++;
        }
    }
    return failed > 199 ? 199 : failed;   /* 200 留给"文件本身没编过" */
}

static int cmd_test(int argc, char **argv, int argi) {
    const char *filter = NULL;             /* 名字子串过滤 */
    bool release = false;
    for (int i = argi; i < argc; i++) {
        if (!strcmp(argv[i], "--release")) { release = true; continue; }
        if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "zxc: unknown test option '%s'\n", argv[i]);
            return 2;
        }
        if (!filter) filter = argv[i];
    }

    int nfiles = 0;
    char **files = collect_test_files(&nfiles);
    int total = 0, failed = 0;
    for (int fi = 0; fi < nfiles; fi++) {
        const char *f = files[fi];

        /* 父进程只轻量 parse 数用例（不碰 sema/后端），决定要不要拉子进程 */
        g_error_count = 0;
        g_diag_shown = 0;
        g_srcpath = f;
        Node *root = fe_parse_file(f);
        Node *fns[256];
        const char *names[256];
        int nt = (root && !g_error_count) ? collect_test_fns(root, fns, names, 256) : -1;
        int matched = 0;
        if (nt > 0)
            for (int i = 0; i < nt; i++)
                if (!filter || strstr(names[i], filter)) matched++;
        if (nt == 0) continue;         /* 没有用例：共享辅助文件 */
        if (nt > 0 && matched == 0) continue;
        total += (nt > 0) ? matched : 1;

        char cmd[2400];
        snprintf(cmd, sizeof cmd, "\"\"%s\" --test-run1 \"%s\" \"%s\"%s\"",
                 argv[0], f, filter ? filter : "", release ? " --release" : "");
        for (char *p = cmd; *p; p++) if (*p == '/') *p = '\\';
        int rc = system(cmd);
#ifndef _WIN32
        rc = WIFEXITED(rc) ? WEXITSTATUS(rc) : 1;
#endif
        if (rc == 200) {
            printf("FAIL  %s (file failed to build)\n", f);
            failed++;
        } else if (rc > 0) {
            failed += rc;
        }
    }
    if (files) { for (int i = 0; i < nfiles; i++) free(files[i]); free(files); }
    if (total == 0 && failed == 0) {
        printf("zxc test: no test modules in src/test/ (or no zero-arg pub fns)\n");
        return 0;
    }
    printf("zxc test: %d ran, %d failed\n", total, failed);
    return failed ? 1 : 0;
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
    g_prog_t0 = prog_clock();
    sema_set_argv0(argv[0]);
    /* zxfe.exe 与 zxc.exe 同目录，位置从 argv[0] 推——必须在这里就交给
     * 前端。以前只有单文件模式设，子命令（check / build / run / test，
     * 连 init 的建后自检）都在**没设**的状态下跑，于是 cmd 从 cwd 找
     * zxfe.exe，在工程目录里一律"找不到指定的文件"：表现为 check 只报
     * "1 of 1 file(s) failed"、build 直接失败，看起来就像子命令不生效。 */
    fe_set_argv0(argv[0]);

    g_cc = getenv("ZX_CC");
    if (!g_cc || !*g_cc) g_cc = default_cc(argv[0]);

    /* 内部入口（先于一切参数解析）：每个 *_test.zx 的构建+运行跑在
     * 独立子进程里（见 test_run1 注释） */
    if (argc >= 4 && !strcmp(argv[1], "--test-run1")) {
        bool rel = false;
        const char *flt = NULL;
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--release")) { rel = true; continue; }
            if (!flt) flt = argv[i];
        }
        return test_run1(argv[2], flt, rel);
    }

    /* 通用 -h / -v（先于子命令：`zxc -v`、`zxc help build` 都不该被当成
     * 文件或未知选项） */
    if (argc >= 2) {
        const char *a = argv[1];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
        if (!strcmp(a, "-v") || !strcmp(a, "-V") || !strcmp(a, "--version")) {
            print_version(stdout);
            return 0;
        }
        if (!strcmp(a, "version")) { print_version(stdout); return 0; }
        /* 旗标形式的同义词，给习惯 `--` 的人 */
        if (!strcmp(a, "--explain") && argc >= 3)
            return errors_print_one(stdout, argv[2]);
        if (!strcmp(a, "--errors") || !strcmp(a, "--codes")) {
            errors_print_all(stdout);
            return 0;
        }
        if (!strcmp(a, "help")) {
            if (argc < 3) { usage(stdout); return 0; }
            help_sub(stdout, argv[2]);
            return 0;      /* 不认识的子命令：help_sub 已打总览，仍算"看了帮助" */
        }
    }

    /* 子命令形式（file-first 旧语法不受影响） */
    if (argc >= 2 && argv[1][0] != '-') {
        const char *sub = argv[1];
        /* `zxc build -h` / `zxc build -v`：只看 `--` 之前的旗标 */
        int special = arg_special_flag(argc, argv, 2);
        if (special == 1) { help_sub(stdout, sub); return 0; }
        if (special == 2) { print_version(stdout); return 0; }
        /* 错误码目录：`zxc errors` 一览、`zxc explain E0304` 单条。
         * 诊断里出现的每个 E0xxx 都能在这查到——码一旦发布就不再改。 */
        if (!strcmp(sub, "errors")) { errors_print_all(stdout); return 0; }
        if (!strcmp(sub, "explain")) {
            if (argc < 3) {
                fprintf(stderr, "usage: zxc explain <CODE>    e.g. zxc explain E0304\n");
                return 2;
            }
            return errors_print_one(stdout, argv[2]);
        }
        if (!strcmp(sub, "init"))  return cmd_init(argc, argv, 2);
        if (!strcmp(sub, "add"))   return cmd_add(argc, argv, 2);
        if (!strcmp(sub, "cbuild")) return cmd_cbuild(argc, argv, 2);
        if (!strcmp(sub, "check")) return cmd_check(argc, argv, 2);
        if (!strcmp(sub, "fmt"))   return cmd_fmt(argc, argv, 2, false);
        if (!strcmp(sub, "build")) return cmd_build(argc, argv, 2);
        if (!strcmp(sub, "run"))   return cmd_run(argc, argv, 2);
        if (!strcmp(sub, "test"))  return cmd_test(argc, argv, 2);
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
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout);
            return 0;
        }
        else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
            print_version(stdout);
            return 0;
        }
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
            fprintf(stderr, "zxc: unknown option '%s'\n\n", argv[i]);
            usage(stderr);
            return 2;
        }
        else file = argv[i];
    }
    if (!file) { usage(stderr); return 2; }
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
        if (check && g_error_count == 0) {
            sema_check(root);
            /* 只查不编（没 -o）不打进度：那是诊断场景，输出越干净越好 */
            if (out && g_error_count == 0)
                prog("checked ok (%.2f s)", prog_clock() - g_prog_t0);
        }
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
