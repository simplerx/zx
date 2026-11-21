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
 *   zxc sync            扫 vendor/ 补生成缺失的 C 绑定（build/run 自动做）
 *   zxc include         接入本地/工程外的 C 库（原 add 的全部能力）
 *   zxc add <url>       从网站拉 C 库进 vendor/；zx.json 只记 URL，锚点全进 zx.lock
 *   zxc fetch           按 zx.lock 校验/补齐 vendor/（唯一可能联网的补齐入口）
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
#include "fetch.h"
#include "platform.h"
#include <ctype.h>
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
void  region_ledger_dump(FILE *out, const char *path);   /* §132 分配账本 */
static void setup_search_paths_for_file(const char *file);
void  sema_set_search_paths(const char *const *dirs);   /* deps 模块搜索路径 */
void  sema_set_std_path(const char *dir);               /* §208 平台 std 镜像覆盖 */
const char *sema_module_path(int i);                    /* 模块 i 的源文件路径 */
static char *read_source(const char *path, size_t *out_len);
static int  vendor_autoscan(void);   /* vendor/ 自动接管：build/run/test 解析源码前调 */
static int  cmd_add(int argc, char **argv, int argi);
static int  cmd_fetch(int argc, char **argv, int argi);

static const char *g_cc;
static const char *g_cflags;
/* §208: 当前构建目标。NULL = 宿主单目标（老路径），非 NULL = 走目标配置。
 * 用指针而非改所有 build_program 调用点：init 自检、cmd_run、cmd_test
 * 仍走宿主默认，只有 zxc build 的多目标循环会设置它。 */
static Target *g_target;
static const char *g_argv0;      /* §208: 生成层定位 std 源目录用 */
int g_zx_release = 0;   /* §133：--release 档位，sema 的按点擦除读它 */
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

/* §203：单元数不再硬编码。按 sema_module_count() 动态分配，
 * 容量 = 模块单元数 + 1（zxrt）。旧定长 [65] 在第 65 个模块时静默截断。 */
static CCJob *g_jobs;
static int    g_jobs_cap;

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
        /* 串行路径也必须把 rc 写回 job：诊断翻译（report_cc_log）只认
         * g_jobs[i].rc。以前这里直接 return，单个单元编译失败时 rc 恒 0，
         * 于是只剩一句 "build failed in the C backend"、具体错误被吞掉
         * （§160 的 C shim 单独重编时最容易撞到：那时 njobs 正好是 1）。 */
        for (int i = 0; i < njobs; i++) {
            jobs[i].rc = (system(jobs[i].cmd) != 0);
            if (jobs[i].rc) return 1;
        }
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
    /* §208: 有效编译参数 = 档位 + 目标 triple + 目标覆盖 cflags。
     * 三处消费（stamp / 编译 / 链接）统一用这一份，避免漏注入。 */
    char cc_flags[1200];
    snprintf(cc_flags, sizeof cc_flags, "%s%s%s%s",
             g_cflags ? g_cflags : "",
             (g_target && g_target->cc_target[0]) ? " " : "",
             (g_target && g_target->cc_target[0]) ? g_target->cc_target : "",
             (g_target && g_target->extra_cflags[0]) ? g_target->extra_cflags : "");

    /* §208: 按目标补可执行文件后缀。windows 目标补 .exe；posix 目标不补。
     * 宿主单目标维持原行为（Windows 补 .exe）。 */
    {
        const char *suffix = NULL;
        if (g_target) {
            /* 有目标时完全听目标的：posix 目标 exe_suffix 为空串 = 不补。
             * 绝不能落到下面的宿主 #ifdef，否则 Windows 宿主编 linux 会
             * 错补 .exe（产物变成 ELF 却带 .exe 后缀，直接运行会失败）。 */
            if (g_target->exe_suffix[0]) suffix = g_target->exe_suffix;
        } else {
#ifdef _WIN32
            suffix = ".exe";
#endif
        }
        if (suffix && kind == OUT_EXE) {
            size_t l = strlen(out), sl = strlen(suffix);
            if (l < sl || strcmp(out + l - sl, suffix) != 0) {
                static char outexe[2048];
                snprintf(outexe, sizeof outexe, "%s%s", out, suffix);
                out = outexe;
            }
        }
    }

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
        /* §208: 缓存路径带目标名——同一工程的 windows/linux 目标中间文件
         * 分开放，切换目标不会拿错平台的 .o（它们的 ABI 不兼容）。 */
        if (g_target && g_target->name[0])
            snprintf(stem, sizeof stem, "%s/%s/%s%s", cachedir, g_target->name, base, mtag);
        else
            snprintf(stem, sizeof stem, "%s/%s%s", cachedir, base, mtag);
    } else {
        snprintf(stem, sizeof stem, "%s", out);
    }

    Buf header, rt;
    buf_init(&header);
    buf_init(&rt);
    int nmods = sema_module_count();
    if (nmods <= 0) nmods = 1;
    BUnit *units = calloc((size_t)nmods, sizeof(BUnit));
    int n = backend_emit_split(root, &header, &rt, units, nmods);
    if (n < 0) { free(units); return 1; }

    /* 编译作业数 = n 个模块单元 + 1 个 zxrt；按需扩容。 */
    int need_jobs = n + 1;
    if (need_jobs > g_jobs_cap) {
        g_jobs_cap = need_jobs;
        g_jobs = realloc(g_jobs, sizeof(CCJob) * (size_t)g_jobs_cap);
    }

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
    snprintf(fstamp, sizeof fstamp, "%s|%s\n", g_cc, cc_flags);
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
            if (njobs >= g_jobs_cap) {
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
                     g_cc, cc_flags, warnflag, opath, cpath, j->log);
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
        /* §208: -fPIC 按目标族决定，而非宿主：交叉编 linux 时即使宿主是
         * Windows 也要带 -fPIC。 */
        const char *shared = "";
        bool posix_target = g_target ? !strcmp(g_target->family, "posix") : false;
#ifndef _WIN32
        posix_target = true;
#endif
        if (kind == OUT_SHARED) shared = posix_target ? "-shared -fPIC" : "-shared";
        snprintf(cmd, sizeof cmd, "%s %s %s -o \"%s\" %s",
                 g_cc, cc_flags, shared, absout, objs.data);
        /* §208: 平台默认系统库（linux 的 -lm -ldl -lpthread 等），放在
         * @link 收集项之前。链接顺序上系统库靠后更稳，但 zig cc/lld 对
         * -l 顺序不敏感，这里统一追加即可。 */
        if (g_target && g_target->default_libs[0])
            snprintf(cmd + strlen(cmd), sizeof cmd - strlen(cmd), " %s", g_target->default_libs);
        if (g_target && g_target->extra_link[0])
            snprintf(cmd + strlen(cmd), sizeof cmd - strlen(cmd), " %s", g_target->extra_link);
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
        "  include <name|header.h|dir>  include a local C library: binding into vendor/\n"
        "  include ./third_party/foo     point at a library tree; no files are copied\n"
        "  add <url> [--force]            fetch a C library into vendor/ (network, pinned)\n"
        "  fetch [name|url] [--force]     verify/restore vendor/ from zx.lock\n"
        "  sync                            scan vendor/, generate missing C bindings now\n"
        "  cbuild [group]                 compile vendor/*.c into lib<group>.a (auto on include/build)\n"
        "  build [options]                  build per zx.json into out/ (intermediates in .zxc-cache/)\n"
        "  targets                          list built-in platform targets for zx.json \"targets\"\n"
        "  run [options] [-- prog args...]  build, then run\n"
        "  test [name filter]               run src/test/ cases (zero-arg pub functions)\n"
        "  check [file...]                  static check (defaults to cwd, falls back to src/, src/test/)\n"
        "  fmt [file...] [--check]          format (--check reports without writing)\n"
        "  errors                           list all error codes (by category)\n"
        "  explain <CODE>                   explain one error code: meaning and fix\n"
        "  explain region <file.zx>         print the allocation ledger (slot / state / merge)\n"
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

/* §132：`zxc explain region <file.zx>` 只跑前端 + 语义分析，把编译期
 * 分配账本打出来——不生成 C、不建可执行文件，是纯只读视图。 */
static int explain_region(const char *file) {
    g_srcpath = file;
    setup_search_paths_for_file(file);
    Node *root = fe_parse_file(file);
    if (!root || g_error_count) return 1;
    sema_check(root);
    region_ledger_dump(stdout, file);
    return g_error_count ? 1 : 0;
}

/* 子命令级帮助。返回 0 = 认得的子命令，-1 = 没有这个子命令。 */
static int help_sub(FILE *f, const char *cmd) {
    if (!strcmp(cmd, "add")) {
        fprintf(f,
            "zxc add <url> [--force] -- fetch a C library into vendor/ (network)\n"
            "\n"
            "  zxc add https://raw.githubusercontent.com/nothings/stb/master/stb_image.h\n"
            "  zxc add https://example.com/foo.tar.gz\n"
            "  zxc add https://gitcode.com/some/foo.git\n"
            "\n"
            "Two files, both committed:\n"
            "  zx.json   \"vendor\": [ \"<url>\", ... ]   just the URLs (like \"deps\")\n"
            "  zx.lock   written by zxc: sha256, git commit, tree hash, path, nfiles\n"
            "\n"
            "The manifest has NO options -- no ref / sha256 / subdir to remember.\n"
            "zxc computes every integrity pin on the first add and stores it in\n"
            "zx.lock; `zxc fetch` reproduces those pins on another machine.\n"
            "Safety comes from the lock, not from extra fields in zx.json.\n"
            "\n"
            "The library name is derived from the URL's last segment (extension\n"
            "stripped): .../stb_image.h -> stb_image. It is the landing name under\n"
            "vendor/, the [name] in zx.lock, and the argument of `zxc fetch <name>`.\n"
            "It is NOT an address.\n"
            "\n"
            "Sources: a single file, a .tar/.tgz/.zip archive, or a git repo (URL\n"
            "ending in .git). Archives auto-descend into a single top-level dir.\n"
            "Only https:// is accepted. loopback http:// is allowed for local mirrors;\n"
            "file:// needs ZX_ALLOW_LOCAL_FETCH=1. No build scripts, no registry, no\n"
            "dependency resolution. Uses curl / git / tar from PATH.\n"
            "\n"
            "To change the source or re-pin, run the same add with --force.\n"
            "For a library already on disk use `zxc include <path>` instead.\n");
        return 0;
    }
    if (!strcmp(cmd, "fetch")) {
        fprintf(f,
            "zxc fetch [name|url] [--force] -- verify / restore vendor/ from zx.lock\n"
            "\n"
            "  zxc fetch                  check every library (the usual case)\n"
            "  zxc fetch stb_image        check one by library name\n"
            "  zxc fetch \"https://...\"    check one by its URL\n"
            "  zxc fetch --force          re-download / restore even if modified\n"
            "\n"
            "name is NOT an address. It is the landing name under vendor/, derived\n"
            "from the URL's last segment (.../stb_image.h -> stb_image), and is the\n"
            "[name] key in zx.lock. If unsure, just run `zxc fetch` with no argument.\n"
            "\n"
            "A library present and matching zx.lock is left untouched (no network).\n"
            "A missing one is downloaded and MUST reproduce the locked sha256 / commit\n"
            "/ tree hash. A locally modified one is reported instead of silently\n"
            "overwritten -- --force restores it (old copy: .zxc-cache/fetch-backup/).\n"
            "A changed URL in zx.json is refused: re-pin with `zxc add <url> --force`.\n"
            "A URL with no lock entry is refused too: fetch never adds a new source;\n"
            "use `zxc add <url>` explicitly to introduce one.\n"
            "This is the only command that may download during a normal workflow.\n");
        return 0;
    }
    if (!strcmp(cmd, "sync")) {
        fprintf(f,
            "zxc sync -- scan vendor/ and generate missing C bindings now\n"
            "\n"
            "build / run / test already do this automatically. Use sync when you\n"
            "want the generated vendor/<name>.zx to exist BEFORE typing code, so\n"
            "the editor offers @mod(\"name\") completion and hover right away.\n"
            "\n"
            "Only NEW C libraries are touched: an existing .clib or .zx is never\n"
            "overwritten (edit it freely, or use one to opt a directory out).\n"
            "C sources are compiled later by `zxc build`, not by sync.\n");
        return 0;
    }
    if (!strcmp(cmd, "cbuild")) {
        fprintf(f,
            "zxc cbuild [group] [options] -- compile C sources in vendor/\n"
            "\n"
            "  zxc cbuild              compile every *.c group + every vendor/*.clib\n"
            "  zxc cbuild mini         only group / manifest named 'mini'\n"
            "  zxc cbuild --force      ignore timestamps, rebuild all\n"
            "  zxc cbuild --release    use the -O2 profile\n"
            "\n"
            "Normally you do not need this: `zxc include` compiles sibling .c files\n"
            "and `zxc build` recompiles them incrementally when a .c/.h changes.\n"
            "C sources use the exact same cc + flags as the C that zxc generates,\n"
            "so there is no sanitizer / ABI mismatch at link time.\n"
            "Grouping: <stem>.c with <stem>.h -> lib<stem>.a; mini_extra.c also\n"
            "joins libmini.a; a .c without a matching header gets its own group.\n"
            "A vendor/<name>.clib manifest (made by `zxc include <dir>`) builds all its\n"
            "listed sources into one lib<name>.a, whatever their directory depth.\n");
        return 0;
    }
    if (!strcmp(cmd, "include")) {
        fprintf(f,
            "zxc include <name|header.h|dir> [options] -- include a local C library\n"
            "\n"
            "  zxc include ../third_party/foo    point at a LIBRARY DIRECTORY (recommended)\n"
            "  zxc include ./vendor/raylib.h     parse one local header -> vendor/raylib.zx\n"
            "  zxc include sqlite3               built-in recipe: locate header + set @link\n"
            "  zxc include --list                show built-in recipes\n"
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
            "  const ray = @mod(\"raylib\");   ray.InitWindow(800, 600, \"hi\");\n"
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
            "  src/main.zx       entry (example: @mod(\"std\") + greet/main)\n"
            "  zx.json           project manifest: name / version / main / out / deps / vendor\n"
            "                    (optional \"targets\" for cross-platform, see `zxc targets -h`)\n"
            "  vendor/           external module root (see below)\n"
            "  README.md .gitignore\n"
            "\n"
            "vendor/:\n"
            "  drop third-party *.zx modules here and @mod(\"name\") finds them:\n"
            "  vendor/ext.zx -> @mod(\"ext\"), vendor/lib/util.zx -> @mod(\"lib/util\").\n"
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
            "outputs: out/<out> (host, single target) / out/<target>/<out> (multi-target);\n"
            "         also .a / .dll / .so / .dylib for library kinds.\n"
            "intermediates: .zxc-cache/<out>_*.c|.o|.h, or .zxc-cache/<target>/ when zx.json\n"
            "               has \"targets\" (unchanged files are not recompiled).\n"
            "backend C errors are mapped back to zx source lines; ZX_VERBOSE=1 for raw output.\n");
        return 0;
    }
    if (!strcmp(cmd, "targets")) {
        fprintf(f,
            "zxc targets -- list built-in platform targets\n"
            "\n"
            "  zxc targets        print every preset (name / triple / arch / family / exe suffix)\n"
            "  zxc targets -h     this help\n"
            "\n"
            "Cross-platform builds are configured in zx.json ONLY -- add a \"targets\" array:\n"
            "\n"
            "  {\n"
            "    \"name\": \"myapp\",\n"
            "    \"main\": \"src/main.zx\",\n"
            "    \"out\": \"myapp\",\n"
            "    \"targets\": [\"windows-x86_64\", \"linux-x86_64\", \"macos-aarch64\"]\n"
            "  }\n"
            "\n"
            "Built-in names (6): windows-x86_64 / windows-aarch64 / linux-x86_64 /\n"
            "  linux-aarch64 / macos-x86_64 / macos-aarch64.\n"
            "No \"targets\" = host single target (old behavior, nothing generated).\n"
            "\n"
            "On `zxc build` everything else is generated automatically:\n"
            "  .zxc-cache/platform/<target>/std/  std mirror: platform net/fs/time + platform.zx\n"
            "  out/<target>/<out>                 per-target artifact (out/<out> for one target)\n"
            "  .zxc-cache/<target>/               per-target intermediates (no ABI cross-talk)\n"
            "Each target builds in its own process. `zxc run` is for the host single target;\n"
            "use `zxc build` for multi-target projects. Cross artifacts need the target OS to run.\n"
            "\n"
            "Optional zx.json fields: \"platform\": { \"generate\": true, \"dir\": \"...\" }\n"
            "  (generate defaults to true when targets are present). Read compile-time target\n"
            "  constants with @mod(\"platform.zx\"): os / arch / family / triple /\n"
            "  is_windows / is_posix.\n");
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
            "reference project sources via @mod(\"../xxx.zx\").\n");
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
#ifdef _WIN32
    char dir[1600];
    snprintf(dir, sizeof dir, "%s", argv0 ? argv0 : "");
    for (char *p = dir; *p; p++) if (*p == '/') *p = '\\';   /* cmd 只认反斜杠 */
    char *slash = strrchr(dir, '\\');
    if (!slash) return ".toolchain\\zig.exe cc";             /* 走 PATH 调的，只能靠 cwd */
    *slash = 0;
    snprintf(g_ccbuf, sizeof g_ccbuf, "%s\\..\\.toolchain\\zig.exe cc", dir);
    return g_ccbuf;
#else
    /* §208: POSIX 上 zig 二进制叫 zig（无 .exe），路径用正斜杠。
     * 优先 ZX_ZIG 环境变量 > zxc 同级的 ../.toolchain/zig > PATH 上的 zig。 */
    const char *env = getenv("ZX_ZIG");
    if (env && *env) { snprintf(g_ccbuf, sizeof g_ccbuf, "%s cc", env); return g_ccbuf; }
    char dir[1600];
    snprintf(dir, sizeof dir, "%s", argv0 ? argv0 : "");
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        snprintf(g_ccbuf, sizeof g_ccbuf, "%s/../.toolchain/zig cc", dir);
        if (fexists(g_ccbuf) || true) return g_ccbuf;
    }
    return "zig cc";
#endif
}

/* §208: 定位源 std 目录（与 sema.find_std_path 同策略：ZX_STD > exe 同级
 * ../std > cwd/std）。argv0 用来算 exe 所在目录。返回静态缓冲，找不到 NULL。 */
static const char *find_std_source_dir(const char *argv0) {
    static char dir[1600];
    const char *env = getenv("ZX_STD");
    if (env && *env) {
        snprintf(dir, sizeof dir, "%s", env);
        char probe[1660];
        snprintf(probe, sizeof probe, "%s/std.zx", dir);
        if (fexists(probe)) return dir;
    }
    /* zxc 在 bin/ 里，std/ 在它的上一级。 */
    if (argv0) {
        char exedir[1600];
        snprintf(exedir, sizeof exedir, "%s", argv0);
        char *slash = strrchr(exedir, '/');
        if (!slash) slash = strrchr(exedir, '\\');
        if (slash) {
            *slash = 0;
            snprintf(dir, sizeof dir, "%s/../std", exedir);
            char probe[1660]; snprintf(probe, sizeof probe, "%s/std.zx", dir);
            if (fexists(probe)) return dir;
            snprintf(dir, sizeof dir, "%s/std", exedir);
            snprintf(probe, sizeof probe, "%s/std.zx", dir);
            if (fexists(probe)) return dir;
        }
    }
    if (fexists("std/std.zx")) { snprintf(dir, sizeof dir, "std"); return dir; }
    return NULL;
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

/* 开组栈：'(' 与 '{' 各占一层。缩进口径（zig fmt）：组内容层 = 开组那一行的
 * 层 + 1；同一行连开的组（foo(c: .{ …）共享一层——链式开组不加深，否则字面量
 * 字段会比 zig fmt 多缩一级；闭括号开头的行退到该组的开组行层。
 * （2026-11-11 晚实测：depth + 圆括号深 的旧算法在 foo(c: .{ 上多缩一级、
 * 闭行 }); 又少退一级，跑两遍来回摆）。 */
typedef struct { int inner; int line; } FmtGrp;
typedef struct {
    bool in_str;        /* "…" */
    bool in_chr;        /* '…' */
    bool in_block;      /* 斜杠星注释 */
    int top;            /* 开组栈顶，-1 = 空 */
    FmtGrp g[128];
    int curline;        /* 当前行号（链式开组判定） */
} FmtState;

/* 单行词法推进：记 first_ns（首个非空白下标），更新 in_str/in_chr/in_block。 */
static void fmt_scan_line(const char *s, FmtState *st, size_t *first_ns) {
    *first_ns = 0;
    bool seen_ns = false;
    for (size_t i = 0; s[i]; i++) {
        char c = s[i];
        size_t ci = i;      /* 开头可能 i++（块注释），首个非空白下标要用
                             * 本字符的原始位置 */
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
        if (c == '"') st->in_str = true;
        else if (c == '\'') st->in_chr = true;
        else if (c == '/' && s[i + 1] == '/') {
            /* 行注释提前返回：纯注释行也要记 first_ns，否则缩进翻倍 */
            if (!seen_ns) { seen_ns = true; *first_ns = ci; }
            return;
        }
        else if (c == '/' && s[i + 1] == '*') { st->in_block = true; i++; }
        if (!seen_ns && !(c == ' ' || c == '\t')) { seen_ns = true; *first_ns = ci; }
    }
}

/* 本行缩进层（用本行之前的栈）。闭括号开头的行退到对应组的开组行层；
 * 多个前导闭括号（});）取最外层。 */
static int fmt_line_indent(const char *s, size_t first, FmtState *st) {
    if (st->in_block || st->in_str || st->in_chr)     /* 注释/字符串延续行 */
        return st->top < 0 ? 0 : st->g[st->top].inner;
    int ind = -1, k = 0;
    size_t p = first;
    while (s[p] == ')' || s[p] == '}') {          /* 只认 '(' '{' 两种开组 */
        int t = st->top - k;
        if (t >= 0) {
            int cand = st->g[t].inner - 1;
            if (ind < 0 || cand < ind) ind = cand;
        }
        k++; p++;
    }
    if (ind < 0) ind = st->top < 0 ? 0 : st->g[st->top].inner;
    return ind < 0 ? 0 : ind;
}

/* 本行的开/闭组入栈出栈（字符串与注释内不算）。 */
static void fmt_apply_groups(const char *s, bool in_str, bool in_chr, bool in_block, FmtState *st) {
    bool ins = in_str, inc = in_chr, inb = in_block;
    for (size_t i = 0; s[i]; i++) {
        char c = s[i];
        if (inb) { if (c == '*' && s[i + 1] == '/') { inb = false; i++; } continue; }
        if (ins) { if (c == '\\') { i++; continue; } if (c == '"') ins = false; continue; }
        if (inc) { if (c == '\\') { i++; continue; } if (c == '\'') inc = false; continue; }
        if (c == '"') { ins = true; continue; }
        if (c == '\'') { inc = true; continue; }
        if (c == '/') {
            if (s[i + 1] == '/') break;
            if (s[i + 1] == '*') { inb = true; i++; }
            continue;
        }
        if (c == '(' || c == '{') {
            bool chain = st->top >= 0 && st->g[st->top].line == st->curline;
            int inner = st->top >= 0 ? st->g[st->top].inner : 0;
            if (!chain) inner++;
            if (st->top + 1 < (int)(sizeof st->g / sizeof st->g[0])) {
                st->top++;
                st->g[st->top].inner = inner;
                st->g[st->top].line = st->curline;
            }
        } else if (c == ')' || c == '}') {
            if (st->top >= 0) st->top--;
        }
    }
}

/* ---- 尾逗号拆行（zig-fmt 口径，用户 2026-11-11 晚二次拍板）----
 * 每个可拆组（'(' 或字面量 '{'：.{…}、T{…}）只看它自己的闭括号：
 *   闭括号前最后一个非空白是 ',' → 该组展开成一行一项（树状，递归）；
 *   否则 → 该组收在一行（跨行手工排布由 fmt_normalize 先并回一行）；
 *   组内更浅位置/组后带的尾逗号组照常展开——**永不增删逗号**，展开用的
 *   逗号全部来自源码（分隔逗号与尾逗号）。
 * struct/enum/union 类型体没有尾逗号时整体原样保留（容器体不收拢）；
 * 语句块的 `{` 前面不是标识符，天然不是可拆组。 */

typedef struct {
    char *text;                             /* 行文本（不带缩进） */
    int lvl;                                /* 相对缩进级别（0 = 与原行同深） */
} FmtRow;

typedef struct { FmtRow *v; int n, cap; } RowVec;
typedef struct { char **v; size_t n, cap; } StrVec;

static void rv_push(RowVec *rv, char *text, int lvl) {
    if (rv->n >= rv->cap) {
        rv->cap = rv->cap ? rv->cap * 2 : 16;
        rv->v = realloc(rv->v, (size_t)rv->cap * sizeof *rv->v);
    }
    rv->v[rv->n].text = text;
    rv->v[rv->n].lvl = lvl;
    rv->n++;
}

static void sv_push(StrVec *sv, char *s) {
    if (sv->n >= sv->cap) {
        sv->cap = sv->cap ? sv->cap * 2 : 64;
        sv->v = realloc(sv->v, sv->cap * sizeof *sv->v);
    }
    sv->v[sv->n++] = s;
}

static bool fmt_ident_ch(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* 找**第一个**可拆组（'(' 或字面量 '{'）到它的闭合，报告该组自己是否以
 * 尾逗号收尾（*tc），以及 '{' 前的标识符是不是 struct/enum/union 容器
 * 类型体（*tybody）。tc/tybody 允许 NULL。字符串/字符里的括号不算；
 * 见行注释保守放弃；根组必须在本串内闭合，否则返回 false。 */
static bool fmt_first_group(const char *s, size_t *op, size_t *cl, bool *tc,
                            bool *tybody) {
    bool in_str = false, in_chr = false;
    size_t open_stk[64], last_ns[64];
    char kind[64];
    bool lit[64], seen[64];
    int sp = 0;
    bool have = false;
    int root_sp = 0;
    if (tc) *tc = false;
    if (tybody) *tybody = false;
    for (size_t i = 0; s[i]; i++) {
        char c = s[i];
        if (in_str) { if (c == '\\') i++; else if (c == '"') in_str = false; continue; }
        if (in_chr) { if (c == '\\') i++; else if (c == '\'') in_chr = false; continue; }
        if (c == '(' || c == '{') {
            if (sp >= 64) return false;
            kind[sp] = c;
            lit[sp] = (c == '{') && i > 0 &&
                      (s[i - 1] == '.' || fmt_ident_ch(s[i - 1]));
            if (!have && (c == '(' || lit[sp])) {
                have = true; root_sp = sp; *op = i;
                if (tybody && c == '{' && fmt_ident_ch(s[i - 1])) {
                    size_t j = i;
                    while (j > 0 && fmt_ident_ch(s[j - 1])) j--;
                    size_t wl = i - j;
                    *tybody = (wl == 6 && memcmp(s + j, "struct", 6) == 0) ||
                              (wl == 4 && memcmp(s + j, "enum", 4) == 0) ||
                              (wl == 5 && memcmp(s + j, "union", 5) == 0);
                }
            }
            open_stk[sp] = i; last_ns[sp] = 0; seen[sp] = false; sp++;
            continue;
        }
        if (c == ')' || c == '}') {
            if (sp > 0) {
                sp--;
                if (have && (int)sp == root_sp && (kind[sp] == '(' || lit[sp])) {
                    *cl = i;
                    if (tc) *tc = seen[sp] && s[last_ns[sp]] == ',';
                    return true;
                }
            }
            continue;
        }
        if (c == '"') { in_str = true; }
        else if (c == '\'') { in_chr = true; }
        else if (c == '/') {
            if (s[i + 1] == '/') return false;      /* 行注释：保守放弃 */
            if (s[i + 1] == '*') {                  /* 同行块注释：整段跳过 */
                i += 2;
                while (s[i] && !(s[i] == '*' && s[i + 1] == '/')) i++;
                if (!s[i]) return false;
                continue;
            }
        }
        if (sp > 0 && c != ' ' && c != '\t') {
            seen[sp - 1] = true; last_ns[sp - 1] = i;
        }
    }
    return false;                                    /* 根没在本串内闭合 */
}

static char *dup_span(const char *s, size_t a, size_t b) {
    char *r = malloc(b - a + 1);
    memcpy(r, s + a, b - a);
    r[b - a] = 0;
    return r;
}

static char *str_append(char *dst, const char *src) {
    size_t a = strlen(dst), b = strlen(src);
    char *r = realloc(dst, a + b + 1);
    memcpy(r + a, src, b + 1);
    return r;
}

/* 递归拆行：首个可拆组自带尾逗号 → 展开成一行一项（项文本自带源逗号，
 * 递归到稳定形态）；没尾逗号 → 本组保持单行，但组内更浅位置与前缀/
 * 后缀里的尾逗号组照样展开（head/tail 拼接组合，见 keep-inline 段）。
 * struct/enum/union 类型体没尾逗号 → 整体不动。无任何可拆改动返回
 * NULL。行数组 *nr_out 行，lvl 递增表示更深一层缩进；depth 防病态嵌套。
 * 项文本的结尾 ',' 全部来自源码——**永不新增逗号**。 */
static FmtRow *fmt_rows_rec(const char *s, size_t start, int lvl, int *nr_out, int depth) {
    *nr_out = 0;
    if (depth > 12) return NULL;
    size_t op, cl;
    bool tc = false, tybody = false;
    if (!fmt_first_group(s + start, &op, &cl, &tc, &tybody)) return NULL;
    op += start; cl += start;
    if (tybody && !tc) return NULL;          /* 容器类型体无尾逗号：原样保留 */
    RowVec rv = {0};
    if (tc) {
        /* ---- 展开：按根组顶层逗号切项（逗号全是源码的） ---- */
        size_t cuts[256]; int ncuts = 0;
        bool in_str = false, in_chr = false;
        int pd = 0, bd = 0;
        for (size_t i = op + 1; i < cl; i++) {
            char c = s[i];
            if (in_str) { if (c == '\\') i++; else if (c == '"') in_str = false; continue; }
            if (in_chr) { if (c == '\\') i++; else if (c == '\'') in_chr = false; continue; }
            if (c == '"') { in_str = true; continue; }
            if (c == '\'') { in_chr = true; continue; }
            if (c == '/' && s[i + 1] == '/') goto bail;      /* 行注释：放弃 */
            if (c == '/' && s[i + 1] == '*') {
                i += 2;
                while (i < cl && !(s[i] == '*' && s[i + 1] == '/')) i++;
                if (i >= cl) goto bail;
                continue;
            }
            if (c == '(') { pd++; continue; }
            if (c == ')') { if (pd > 0) pd--; continue; }
            if (c == '{') bd++;
            else if (c == '}') { if (bd > 0) bd--; }
            else if (c == ',' && pd == 0 && bd == 0 && ncuts < 256) cuts[ncuts++] = i;
        }
        if (ncuts == 0) goto bail;
        {   /* 最后一刀必须紧贴闭括号（中间只许空白）——尾逗号的定义 */
            size_t k = cuts[ncuts - 1] + 1;
            while (k < cl && (s[k] == ' ' || s[k] == '\t')) k++;
            if (k != cl) goto bail;
        }
        rv_push(&rv, dup_span(s, start, op + 1), lvl);      /* 首行：… 开括号 */
        size_t prev = op + 1;
        for (int j = 0; j < ncuts; j++) {
            size_t a = prev, b = cuts[j];
            while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
            while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
            if (a == b) goto bail;                           /* 空项（foo(  ,)）放弃 */
            char *item = malloc(b - a + 2);
            memcpy(item, s + a, b - a);
            item[b - a] = ','; item[b - a + 1] = 0;
            int snr = 0;
            FmtRow *sub = fmt_rows_rec(item, 0, lvl + 1, &snr, depth + 1);
            if (sub) {                                      /* 项本身是尾逗号组：展开 */
                free(item);
                for (int k2 = 0; k2 < snr; k2++) rv_push(&rv, sub[k2].text, sub[k2].lvl);
                free(sub);
            } else {
                rv_push(&rv, item, lvl + 1);
            }
            prev = cuts[j] + 1;
        }
        {   /* 末行：闭括号起接余下原文。余文里若还有尾逗号组（同行先后两个组）
             * 就继续拆，否则一次 fmt 拆不完、不幂等。 */
            char *suffix = dup_span(s, cl, strlen(s));
            int snr = 0;
            FmtRow *sub = fmt_rows_rec(suffix, 0, lvl, &snr, depth + 1);
            if (sub) {
                free(suffix);
                for (int k2 = 0; k2 < snr; k2++) rv_push(&rv, sub[k2].text, sub[k2].lvl);
                free(sub);
            } else {
                rv_push(&rv, suffix, lvl);
            }
        }
        *nr_out = rv.n;
        return rv.v;
    }
    /* ---- 收拢保留（zig-fmt 口径的核心）：根组没尾逗号，整组收在一行；
     * 但组内嵌套的尾逗号组与组后余文里的尾逗号组照常展开：递归 inner /
     * post，首段拼进 head（开括号行）、首段拼进 tail（闭括号行）。
     * 内外都没有可拆组 → 返回 NULL（整行原样/由 normalize 收拢）。 */
    {
        char *inner = dup_span(s, op + 1, cl);
        char *post = dup_span(s, cl + 1, strlen(s));
        {   /* 贴括号的空白在重组时归一（只在确有改动时用到） */
            size_t a = 0, b = strlen(inner);
            while (a < b && (inner[a] == ' ' || inner[a] == '\t')) a++;
            while (b > a && (inner[b - 1] == ' ' || inner[b - 1] == '\t')) b--;
            memmove(inner, inner + a, b - a); inner[b - a] = 0;
        }
        {
            size_t a = 0, b = strlen(post);
            while (a < b && (post[a] == ' ' || post[a] == '\t')) a++;
            memmove(post, post + a, b - a); post[b - a] = 0;
        }
        int inr = 0, pnr = 0;
        FmtRow *irows = *inner ? fmt_rows_rec(inner, 0, lvl, &inr, depth + 1) : NULL;
        FmtRow *prows = *post ? fmt_rows_rec(post, 0, lvl, &pnr, depth + 1) : NULL;
        if (!irows && !prows) {
            free(inner); free(post);
            return NULL;
        }
        char *head = dup_span(s, start, op + 1);     /* 前缀 + 开括号 */
        head = str_append(head, irows ? irows[0].text : inner);
        rv_push(&rv, head, lvl);
        if (irows) {
            for (int r = 1; r < inr; r++) rv_push(&rv, irows[r].text, irows[r].lvl);
            free(irows[0].text);
        }
        {   /* 闭括号 + 后缀贴到当前最后一行（inner 没展开时是 head 行，
             * 展开时是 inner 的末行）——根组的 ')' 紧跟组内最后一行文本，
             * 单独成行就是掉队（2026-11-11 晚实测复现）。 */
            char *tail = dup_span(s, cl, cl + 1);
            tail = str_append(tail, prows ? prows[0].text : post);
            rv.v[rv.n - 1].text = str_append(rv.v[rv.n - 1].text, tail);
            free(tail);
        }
        if (prows) {
            for (int r = 1; r < pnr; r++) rv_push(&rv, prows[r].text, prows[r].lvl);
            free(prows[0].text);
        }
        free(irows); free(prows);
        free(inner); free(post);
        *nr_out = rv.n;
        return rv.v;
    }
bail:
    for (int k2 = 0; k2 < rv.n; k2++) free(rv.v[k2].text);
    free(rv.v);
    return NULL;
}

/* ---- 跨行区域归一化（zig-fmt 口径）----
 * 手工换行拆开的组，行级 fmt 碰都不碰；区域（某行结束时仍有未闭合的
 * 可拆组，到围深归 0 那行为止）里没有注释就并回一行走递归拆分：
 *   根组有尾逗号 → 展开成规范树（递归）；
 *   根组没尾逗号 → **整块收成一行**（zig-fmt：删掉尾逗号就回到一行），
 *     不自动补逗号；只有 struct/enum/union 类型体没尾逗号时原样保留。
 * 带注释的区域原样保留（并回一行会把注释后面的内容吞掉）。
 * 单行组仍由主循环的 fmt_rows_rec 处理。行所有权：原样保留/收拢的行
 * 移动进新数组；被拆行替换的区域原行在这里 free。 */
static char *sv_join(StrVec *sv) {
    size_t total = 2;
    for (size_t i = 0; i < sv->n; i++) total += strlen(sv->v[i]) + 1;
    char *buf = malloc(total);
    size_t k = 0;
    for (size_t i = 0; i < sv->n; i++) {
        if (i) {
            /* 边界空白规则：前一行以开括号（( { [ .）结尾、或本行以
             * 闭括号/分隔符（) } ] , ;）开头时不加空格，其余补一个，
             * token 永不粘连。 */
            char ae = k ? buf[k - 1] : 0;
            char b0 = sv->v[i][0];
            bool no_sp = ae == '(' || ae == '[' || ae == '.' ||
                         b0 == ')' || b0 == ']' ||
                         b0 == ',' || b0 == ';';
            if (ae == '{') no_sp = (b0 == '}' || b0 == '{');   /* .{ .a = 1 } 组内留空格 */
            if (b0 == '}') no_sp = (ae == '{' || ae == '(' || ae == '[');
            if (!no_sp) buf[k++] = ' ';
        }
        size_t l = strlen(sv->v[i]);
        memcpy(buf + k, sv->v[i], l);
        k += l;
    }
    buf[k] = 0;
    return buf;
}

/* inds：每行"归一后"的缩进层（fmt_transform 预先算好，见调用点）。
 * 宽度保护必须按最终缩进算——按原始前导空白算会在"原始 16 格 → 归一
 * 4 格"的行上来回摆（第一遍超宽保留、第二遍不超宽收拢，不幂等，
 * 2026-11-11 晚 fmt 压测逮住）。 */
static char **fmt_normalize(char **ls, size_t *out_n, const int *inds) {
    size_t n = *out_n;
    StrVec sv = {0};
    bool in_str = false, in_chr = false, in_block = false;
    char kind[64];
    bool lit[64];
    int sp = 0, spl = 0;                /* spl：未闭合的可拆组（( 或字面量 {）数 */
    size_t reg_start = 0;
    bool reg_active = false, reg_bad = false;
    StrVec reg; reg.v = NULL; reg.n = 0; reg.cap = 0;

    for (size_t i = 0; i < n; i++) {
        char *s = ls[i];
        size_t len = strlen(s);
        while (len && (s[len - 1] == '\r' || s[len - 1] == ' ' || s[len - 1] == '\t'))
            s[--len] = 0;

        if (!reg_active) {
            for (size_t p = 0; s[p]; p++) {
                char c = s[p];
                if (in_block) { if (c == '*' && s[p + 1] == '/') { in_block = false; p++; } continue; }
                if (in_str) { if (c == '\\') p++; else if (c == '"') in_str = false; continue; }
                if (in_chr) { if (c == '\\') p++; else if (c == '\'') in_chr = false; continue; }
                if (c == '"') in_str = true;
                else if (c == '\'') in_chr = true;
                else if (c == '/') {
                    if (s[p + 1] == '/') break;         /* 行注释后不看不影响括号 */
                    if (s[p + 1] == '*') { in_block = true; p++; }
                }
                else if (c == '(' || c == '{') {
                    bool is_lit = (c == '{') && p > 0 &&
                                  (s[p - 1] == '.' || fmt_ident_ch(s[p - 1]));
                    if (sp < 64) { kind[sp] = c; lit[sp] = is_lit; sp++; }
                    if (is_lit || c == '(') spl++;
                }
                else if (c == ')' || c == '}') {
                    if (sp > 0) {
                        sp--;
                        if (kind[sp] == '(' && c == ')' && spl > 0) spl--;
                        else if (kind[sp] == '{' && c == '}' && lit[sp] && spl > 0) spl--;
                    }
                }
            }
            if (spl > 0) {                       /* 行末仍有未闭合可拆组：区域起点 */
                reg_active = true; reg_start = i; reg_bad = false;
                reg.n = 0;
                {   char *t = s;                 /* 去前导空白 */
                    while (*t == ' ' || *t == '\t') t++;
                    sv_push(&reg, strdup(t));
                }
                continue;                        /* 本行暂存，等区域闭合一起处理 */
            }
            sv_push(&sv, s);                     /* 普通行直接通过（所有权移动） */
            continue;
        }

        /* ---- 区域内 ---- */
        if (len == 0) continue;                  /* 空行：并接时跳过 */
        for (size_t p = 0; s[p]; p++) {
            char c = s[p];
            if (in_block) { if (c == '*' && s[p + 1] == '/') { in_block = false; p++; } continue; }
            if (in_str) { if (c == '\\') p++; else if (c == '"') in_str = false; continue; }
            if (in_chr) { if (c == '\\') p++; else if (c == '\'') in_chr = false; continue; }
            if (c == '"') in_str = true;
            else if (c == '\'') in_chr = true;
            else if (c == '/') {
                if (s[p + 1] == '/') { reg_bad = true; break; }
                if (s[p + 1] == '*') { in_block = true; p++; reg_bad = true; }
            }
            else if (c == '(' || c == '{') {
                bool is_lit = (c == '{') && p > 0 &&
                              (s[p - 1] == '.' || fmt_ident_ch(s[p - 1]));
                if (sp < 64) { kind[sp] = c; lit[sp] = is_lit; sp++; }
                if (is_lit || c == '(') spl++;
            }
            else if (c == ')' || c == '}') {
                if (sp > 0) {
                    sp--;
                    if (kind[sp] == '(' && c == ')' && spl > 0) spl--;
                    else if (kind[sp] == '{' && c == '}' && lit[sp] && spl > 0) spl--;
                }
            }
        }
        {   char *t = s;
            while (*t == ' ' || *t == '\t') t++;
            sv_push(&reg, strdup(t));
        }
        if (spl == 0) {                          /* 区域闭合：并回一行再拆 */
            char *joined = sv_join(&reg);
            int nr = 0;
            FmtRow *rows = fmt_rows_rec(joined, 0, 0, &nr, 0);
            if (rows) {
                for (int r = 0; r < nr; r++) sv_push(&sv, rows[r].text);
                free(rows);
                for (size_t k = reg_start; k <= i; k++) free(ls[k]);
                free(joined);
            } else {
                /* 展开不了：根组没尾逗号——整块收成一行，不补逗号；
                 * struct/enum/union 类型体没尾逗号、或区域带注释时原样保留。 */
                size_t o2, c2;
                bool tc2 = false, tb2 = false;
                {   /* 收拢后会超宽就保留原样：手工折行的长表达式不该被并成
                     * 一条 120 列的长行（zig fmt 也不产超宽行）。 */
                    int lvl = (inds && (int)reg_start < (int)n) ? inds[reg_start] : 0;
                    if (lvl < 0) lvl = 0;
                    if ((size_t)lvl * 4 + strlen(joined) > 100) reg_bad = true;
                }
                bool keep = reg_bad ||
                            (fmt_first_group(joined, &o2, &c2, &tc2, &tb2) &&
                             tb2 && !tc2);
                if (keep) {
                    free(joined);
                    for (size_t k = reg_start; k <= i; k++) sv_push(&sv, ls[k]);
                } else {
                    sv_push(&sv, joined);        /* 收拢：一行 */
                    for (size_t k = reg_start; k <= i; k++) free(ls[k]);
                }
            }
            for (size_t k = 0; k < reg.n; k++) free(reg.v[k]);
            free(reg.v); reg.v = NULL; reg.n = reg.cap = 0;
            reg_active = false;
        }
    }
    if (reg_active) {                            /* 到 EOF 都没闭合：原样保留 */
        for (size_t k = reg_start; k < n; k++) sv_push(&sv, ls[k]);
        for (size_t k = 0; k < reg.n; k++) free(reg.v[k]);
        free(reg.v);
    }
    free(ls);
    *out_n = sv.n;
    return sv.v;
}

/* 格式化整个源码：4 空格缩进、去尾空白、连续空行压 1、末尾单换行。
 * zig-fmt 口径尾逗号规则：组闭括号前有尾逗号 → 一行一项树状展开（递归）；
 * 没有尾逗号 → 跨行区域并回一行整块收拢（不自动补逗号）；struct/enum/union
 * 类型体没尾逗号原样保留。CRLF 文档保持 CRLF。返回 malloc 的新文本。 */
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

    /* 缩进预算 pass：先按开组栈算出每行归一后的缩进层。区域归一的
     * 宽度保护要用最终缩进（原始前导空白会被后面的缩进主循环改写）。 */
    int *inds = malloc((n ? n : 1) * sizeof(int));
    {
        FmtState pst = {0};
        pst.top = -1;
        for (size_t i2 = 0; i2 < n; i2++) {
            const char *s = ls[i2];
            size_t l2 = strlen(s);
            while (l2 && (s[l2 - 1] == ' ' || s[l2 - 1] == '\t' || s[l2 - 1] == '\r')) l2--;
            if (l2 == 0 && !pst.in_block) { inds[i2] = 0; continue; }
            FmtState probe = pst;
            bool pre_str = pst.in_str, pre_chr = pst.in_chr, pre_block = pst.in_block;
            size_t first = 0;
            fmt_scan_line(s, &probe, &first);
            inds[i2] = fmt_line_indent(s, first, &pst);
            pst = probe;
            pst.curline = (int)i2;
            fmt_apply_groups(s, pre_str, pre_chr, pre_block, &pst);
        }
    }

    /* 跨行区域归一化（树状铺开）：在缩进主循环之前做，行所有权移交 */
    ls = fmt_normalize(ls, &n, inds);
    free(inds);

    FmtState st = {0};
    st.top = -1;              /* 空栈；{0} 会把栈顶当 0（不存在的组） */
    Buf out;
    buf_init(&out);
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
            buf_puts(&out, had_cr ? "\r\n" : "\n");   /* CRLF 文档空行也要 CRLF（原先写死 \n，混出行尾） */
            free(raw);
            continue;
        }
        size_t first = 0;
        /* 该行净变化先按“不含本行”的 depth 算缩进：先扫描副本避免状态污染 */
        FmtState probe = st;
        bool pre_str = st.in_str, pre_chr = st.in_chr, pre_block = st.in_block;
        fmt_scan_line(raw, &probe, &first);          /* 只取 first_ns 与行末词法状态 */
        int ind = fmt_line_indent(raw, first, &st);  /* 缩进按*本行前*的开组栈算 */
        /* 尾逗号拆行：组自带尾逗号就一行一项；根组没尾逗号但内层带时
         * 内层展开、根组保持单行。整行花括号 delta 只加一次（拆出的行
         * 共享），缩进深度不变。 */
        FmtRow *rows = NULL;
        int nrows = 0;
        if (!st.in_block && !st.in_str && !st.in_chr)
            rows = fmt_rows_rec(raw, first, 0, &nrows, 0);
        if (rows) {
            for (int r = 0; r < nrows; r++) {
                int rind = ind + rows[r].lvl;               /* 首末行 lvl=0，嵌套项递增 */
                if (rind < 0) rind = 0;
                for (int q = 0; q < rind; q++) buf_puts(&out, "    ");
                buf_puts(&out, rows[r].text);
                buf_puts(&out, had_cr ? "\r\n" : "\n");
                free(rows[r].text);
            }
            free(rows);
            any_content = true;
            blank_run = 0;
            st = probe;
            st.curline = (int)i;
            fmt_apply_groups(raw, pre_str, pre_chr, pre_block, &st);
            free(raw);
            continue;
        }
        if (!st.in_block) {
            for (int k = 0; k < ind; k++) buf_puts(&out, "    ");
            buf_puts(&out, raw + first);                    /* 原始前导空白丢弃 */
        } else {
            buf_puts(&out, raw);                            /* 块注释延续行保留原样 */
        }
        buf_puts(&out, had_cr ? "\r\n" : "\n");
        any_content = true;
        blank_run = 0;
        st = probe;
        st.curline = (int)i;
        fmt_apply_groups(raw, pre_str, pre_chr, pre_block, &st);
        free(raw);
    }
    free(ls);
    return out.data ? out.data : strdup("");
}

/* ---- 子命令实现 ---- */

/* 工程清单（zx.json）。定义放前面：check 段也要读 deps。 */
/* §208 多平台目标：用户在 zx.json "targets" 里写预设名；没写就用宿主。 */
#define PROJ_MAX_TARGETS 16
struct Proj {
    bool ok;
    char name[256];     /* 工程名 */
    char main[1024];    /* 入口源文件 */
    char out[256];      /* 产物名（缺省 = 工程名） */
    char *deps[16];     /* "deps" 数组：模块搜索目录（相对工程根） */
    int  ndeps;
    /* §208: "targets": ["windows-x86_64", "linux-x86_64", ...] */
    char *targets[PROJ_MAX_TARGETS];
    int  ntargets;
    /* §208: "platform": { "generate": bool, "dir": str } —— 后台生成 std 镜像 */
    bool platform_gen;          /* 缺省 false；写了 platform 段且 generate != false 时开 */
    char platform_dir[256];     /* 缺省 .zxc-cache/platform */
};
typedef struct Proj Proj;
static int write_text_file(const char *path, const char *content);  /* §208 生成层前置声明 */
/* §208: 静默拷贝一个文件（二进制安全），失败返回 -1。 */
static int plat_copy_file(const char *src, const char *dst) {
    FILE *f = fopen(src, "rb");
    if (!f) return -1;
    FILE *o = fopen(dst, "wb");
    if (!o) { fclose(f); return -1; }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) fwrite(buf, 1, n, o);
    fclose(f);
    fclose(o);
    return 0;
}

/* §208: 为单个目标生成 std 镜像 + platform.zx。
 * 镜像目录 <dir>/<target>/std/：先拷全部源 std/*.zx，再用平台特化文件
 * （src/platform_std/<family>/{net,fs,time}.zx）覆盖，并写 platform.zx。
 * 返回镜像的 std 目录路径（写入 mirror_out），成功 0。
 *
 * 增量：比较每个源文件的 mtime，未变则不重写——反复 build 不做无谓拷贝。 */
static int platform_gen_std(const Proj *pj, const Target *t,
                            char *mirror_out, size_t mcap) {
    const char *srcdir = find_std_source_dir(g_argv0);
    if (!srcdir) {
        fprintf(stderr, "zxc: cannot locate std/ source (set ZX_STD)\n");
        return -1;
    }
    char mirror[2048];
    snprintf(mirror, sizeof mirror, "%s/%s/std", pj->platform_dir, t->name);
    mkdir_p(mirror);

    /* 1) 拷全部 std/*.zx */
    int nsrc = 0;
    char **srcs = collect_zx_files(srcdir, &nsrc);
    for (int i = 0; i < nsrc; i++) {
        const char *base = strrchr(srcs[i], '/');
        if (!base) base = strrchr(srcs[i], '\\');
        base = base ? base + 1 : srcs[i];
        char dst[2300];
        snprintf(dst, sizeof dst, "%s/%s", mirror, base);
        struct stat ss, ds;
        bool need = stat(srcs[i], &ss) != 0;
        if (!need) need = stat(dst, &ds) != 0 || ss.st_mtime > ds.st_mtime;
        if (need && plat_copy_file(srcs[i], dst) != 0) {
            fprintf(stderr, "zxc: platform gen: cannot copy %s\n", srcs[i]);
        }
        free(srcs[i]);
    }
    free(srcs);

    /* 2) 用平台特化文件覆盖（net/fs/time）。源在 zxc 旁边的
     * ../src/platform_std/<family>/，随编译器一起分发。 */
    char overdir[1600];
    if (g_argv0) {
        char exedir[1600];
        snprintf(exedir, sizeof exedir, "%s", g_argv0);
        char *slash = strrchr(exedir, '/');
        if (!slash) slash = strrchr(exedir, '\\');
        if (slash) {
            *slash = 0;
            snprintf(overdir, sizeof overdir, "%s/../src/platform_std/%s",
                     exedir, t->family);
        } else {
            snprintf(overdir, sizeof overdir, "src/platform_std/%s", t->family);
        }
    } else {
        snprintf(overdir, sizeof overdir, "src/platform_std/%s", t->family);
    }
    static const char *k_override[] = { "net.zx", "fs.zx", "time.zx", NULL };
    for (int i = 0; k_override[i]; i++) {
        char s[1660], d[2300];
        snprintf(s, sizeof s, "%s/%s", overdir, k_override[i]);
        snprintf(d, sizeof d, "%s/%s", mirror, k_override[i]);
        if (fexists(s) && plat_copy_file(s, d) != 0)
            fprintf(stderr, "zxc: platform gen: cannot copy %s\n", s);
    }

    /* 2b) §208: struct stat 的字段偏移因 OS 而异。POSIX 模板按 Linux
     * x86_64 写（size=48/mtime=88/mode=24），macOS 是 96/136/8。
     * 按目标改写这三个常量——只改本次生成的镜像，不动模板源。 */
    if (!strcmp(t->os, "macos")) {
        char fpath[2300];
        snprintf(fpath, sizeof fpath, "%s/fs.zx", mirror);
        size_t flen = 0;
        char *fsrc = read_source(fpath, &flen);
        if (fsrc) {
            int rep = 0;
            const char *p = fsrc;
            Buf patched; buf_init(&patched);
            while (p && *p) {
                const char *hit = NULL;
                const char *a = strstr(p, "const STAT_SIZE_OFF: usize = 48;");
                const char *b = strstr(p, "const STAT_MTIME_OFF: usize = 88;");
                const char *c = strstr(p, "const STAT_MODE_OFF: usize = 24;");
                if (a && (!hit || a < hit)) hit = a;
                if (b && (!hit || b < hit)) hit = b;
                if (c && (!hit || c < hit)) hit = c;
                if (!hit) { buf_puts(&patched, p); break; }
                buf_putn(&patched, p, (size_t)(hit - p));
                if (hit == a) { buf_puts(&patched, "const STAT_SIZE_OFF: usize = 96;"); p = hit + strlen("const STAT_SIZE_OFF: usize = 48;"); }
                else if (hit == b) { buf_puts(&patched, "const STAT_MTIME_OFF: usize = 136;"); p = hit + strlen("const STAT_MTIME_OFF: usize = 88;"); }
                else { buf_puts(&patched, "const STAT_MODE_OFF: usize = 8;"); p = hit + strlen("const STAT_MODE_OFF: usize = 24;"); }
                rep++;
            }
            if (rep == 3) write_text_file(fpath, patched.data ? patched.data : "");
            buf_free(&patched);
            free(fsrc);
        }
        /* macOS 的 CLOCK_MONOTONIC=6（Linux=1）。模板按 Linux 写，替换。 */
        char tpath[2300];
        snprintf(tpath, sizeof tpath, "%s/time.zx", mirror);
        size_t tlen = 0;
        char *tsrc = read_source(tpath, &tlen);
        if (tsrc) {
            const char *old = "const CLOCK_MONOTONIC: i32 = 1;";
            const char *neu = "const CLOCK_MONOTONIC: i32 = 6;";
            const char *hit = strstr(tsrc, old);
            if (hit) {
                Buf patched; buf_init(&patched);
                buf_putn(&patched, tsrc, (size_t)(hit - tsrc));
                buf_puts(&patched, neu);
                buf_puts(&patched, hit + strlen(old));
                write_text_file(tpath, patched.data ? patched.data : "");
                buf_free(&patched);
            }
            free(tsrc);
        }
    }

    /* 3) platform.zx：编译期常量，用户代码 @mod("platform.zx") 可读。
     * 值是字符串/整数常量，用 Zx 字面量语法。 */
    char pz[2048];
    snprintf(pz, sizeof pz,
        "// generated by zxc for target %s — do not edit\n"
        "pub const os = \"%s\";\n"
        "pub const arch = \"%s\";\n"
        "pub const family = \"%s\";\n"
        "pub const triple = \"%s\";\n"
        "pub const exe_suffix = \"%s\";\n"
        "pub const is_windows = %s;\n"
        "pub const is_posix = %s;\n",
        t->name, t->os, t->arch, t->family, t->triple, t->exe_suffix,
        !strcmp(t->family, "windows") ? "true" : "false",
        !strcmp(t->family, "posix") ? "true" : "false");
    char pzpath[2300];
    snprintf(pzpath, sizeof pzpath, "%s/platform.zx", mirror);
    write_text_file(pzpath, pz);

    snprintf(mirror_out, mcap, "%s", mirror);
    return 0;
}

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
    "/* main.zx — 工程入口。@mod(\"std\") 引标准库，自建模块放同目录 *.zx。 */\n"
    "const std = @mod(\"std\");\n"
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

/* vendor/ 是外部模块根：放进来就能被 @mod("名字") 找到。空目录进不了
 * 版本控制，所以里面放一份说明——顺便把搜索顺序写在最容易被看到的地方。 */
static const char *INIT_VENDOR_README =
    "# vendor/ — 外部模块根\n"
    "\n"
    "把第三方 zx 模块（`*.zx`，可带子目录）放在这里，源码里直接引用：\n"
    "\n"
    "```zx\n"
    "const ext = @mod(\"ext\");        // → vendor/ext.zx\n"
    "const util = @mod(\"lib/util\");  // → vendor/lib/util.zx\n"
    "```\n"
    "\n"
    "搜索顺序：相对导入所在的目录 → 工程根的 `vendor/` → `zx.json` 的\n"
    "`\"deps\"` 数组。`vendor/` 存在即自动参与搜索，不用写进 deps。\n"
    "\n"
    "C 库也直接放这里，不用敲任何接入命令：\n"
    "\n"
    "```sh\n"
    "cp -r ../third_party/foo vendor/foo   # include/ + 多层 src/ 原样搬入\n"
    "# 或者把 foo.h / foo.c 直接放在 vendor/ 一层\n"
    "zxc run                               # 自动生成绑定、编译 C 源码、链接\n"
    "```\n"
    "\n"
    "第一次会生成 `vendor/foo.zx`（普通 Zx 源码）和 `vendor/foo.clib`（编译清单），\n"
    "两个都是可编辑的工程文件，建议提交。已有同名文件就不会再动；想让编辑器在写码\n"
    "前先有补全，跑一次 `zxc sync`。\n"
    "\n"
    "库在远端时用 `zxc add <url>` 拉进来，同样落在这里：\n"
    "\n"
    "```sh\n"
    "zxc add https://example.com/foo.h        # 单头库\n"
    "zxc add https://example.com/foo.git      # git 仓库\n"
    "zxc fetch                               # 换机器后按 zx.lock 补齐\n"
    "```\n"
    "\n"
    "`zx.json` 的 `\"vendor\"` 就是一串 URL（和 `deps` 一样，没有别的参数），\n"
    "`zx.lock` 由 zxc 写、把下载字节 / commit / 文件树 hash 全部钉死。两个都要提交。\n"
    "`zxc run` / `build` / `test` 永远不联网，缺库时跑 `zxc fetch`。\n"
    "\n"
    "vendor/ 里的是源码依赖，应该提交进版本控制。\n";

static const char *INIT_README =
    "# zx 项目\n"
    "\n"
    "## 构建 / 运行\n"
    "\n"
    "```sh\n"
    "zxc build             # 编译 src/ → out/<产物名>（中间文件在 .zxc-cache/）\n"
    "zxc build --release   # 发布档 -O2\n"
    "zxc build --lib       # 静态库 out/<名>.a；--lib --shared 动态库 .dll/.so\n"
    "zxc run               # build 后直接运行（-- 之后的参数传给程序）\n"
    "zxc test              # 运行 src/test/ 里的测试（零参 pub 函数即用例）\n"
    "zxc check             # 静默检查全部 *.zx（src/ 下也扫）\n"
    "zxc fmt               # 格式化全部 *.zx\n"
    "```\n"
    "\n"
    "## 多平台构建\n"
    "\n"
    "只在 `zx.json` 里加 `\"targets\"`，其余由 zxc 后台自动生成：\n"
    "\n"
    "```json\n"
    "{\n"
    "  \"name\": \"myapp\",\n"
    "  \"main\": \"src/main.zx\",\n"
    "  \"out\": \"myapp\",\n"
    "  \"targets\": [\"windows-x86_64\", \"linux-x86_64\", \"macos-aarch64\"]\n"
    "}\n"
    "```\n"
    "\n"
    "内置 6 个预设：`windows-x86_64` / `windows-aarch64` / `linux-x86_64` /\n"
    "`linux-aarch64` / `macos-x86_64` / `macos-aarch64`，用 `zxc targets` 列出；\n"
    "不写 `targets` 就是宿主单目标（保持老行为，什么都不生成）。\n"
    "\n"
    "`zxc build` 会按目标生成平台 std 镜像（`.zxc-cache/platform/<目标>/std/`，\n"
    "换好 net/fs/time 后端并加 `platform.zx` 常量模块），产物在 `out/<目标>/`，\n"
    "每目标独立进程构建、缓存按目标隔离。多目标用 `zxc build`；`zxc run` 只适合\n"
    "宿主单目标（交叉产物要在对应平台才能运行）。\n"
    "\n"
    "可选 `\"platform\": { \"generate\": true, \"dir\": \"...\" }`（有 targets 时默认生成）。\n"
    "源码里 `@mod(\"platform.zx\")` 可读 os/arch/family/triple/is_windows/is_posix。\n"
    "细节见 `zxc targets -h`。\n"
    "\n"
    "## 模块\n"
    "\n"
    "`src/` 下的 `*.zx` 都是可用模块（与导入文件同目录即可）：\n"
    "`const m = @mod(\"m\");` 或 `@mod(\"m/inner.zx\")` 引子目录模块。\n"
    "标准库见 `std.zx`。\n"
    "\n"
    "第三方 / 外部模块放 `vendor/`（外部模块根，存在即自动参与搜索）：\n"
    "`vendor/ext.zx` → `@mod(\"ext\")`；再多层目录写\n"
    "`@mod(\"lib/util\")` → `vendor/lib/util.zx`。要额外的搜索目录，\n"
    "把相对路径写进 `zx.json` 的 `\"deps\"` 数组。\n"
    "\n"
    "## 测试\n"
    "\n"
    "`src/test/` 下的 `*.zx` 都是测试模块：里面的零参 `pub` 函数全部是用例\n"
    "（不用 test_ 前缀；带参数/泛型的函数可当辅助）。引用工程源码用\n"
    "`@mod(\"../xxx.zx\")`。`zxc test [名字过滤]` 运行，每个用例独立进程。\n";

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
 * 一份普通 Zx 源码放进 vendor/，之后 @mod("名字") 直接调。
 * 生成物是普通源码，所以 LSP 的补全 / hover / inlayHint 全部照常生效。
 */
typedef struct {
    const char *alias;      /* zxc include <alias> */
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

/* ---- vendor/ 自动接管：把库放进去，直接 zxc run -----------------------
 *
 * build/run/test 在解析源码之前扫一遍 vendor/，把还没接管的 C 库补成
 * "add 过"的状态：
 *   - 子目录里含 .h  -> 目录树模式，写 <name>.clib + <name>.zx
 *   - 顶层散的 *.h   -> 单头模式，写 <stem>.zx（.c 由 csrc_build 编）
 * 只生成绑定和清单，编译/链接仍由 build_program 里既有的 clib_build /
 * csrc_build 增量路径负责，不在这里重复编一次。
 *
 * 三条硬约束：
 *   ① 不覆盖：已有同名 .clib/.zx 一律不动。用户可能手工裁剪过清单、
 *      手改过绑定；放一个同名占位文件也是天然的 opt-out。
 *   ② 不阻断：某个目录解析失败只警告、继续构建——"丢进去就跑"不能
 *      因为一个不相干的目录变脆。
 *   ③ 不半途落盘：绑定文本生成成功后才一起写清单和绑定，避免只留下
 *      清单、下次却因"已存在"跳过而永远缺绑定。
 */
#ifndef _WIN32
#include <dirent.h>
#endif

typedef struct { char name[256]; int isdir; } VEntry;

static VEntry *vendor_entries(int *nout) {
    *nout = 0;
    VEntry *list = NULL;
    int n = 0, cap = 0;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("vendor/*", &fd);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    do {
        if (fd.cFileName[0] == '.') continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            list = realloc(list, (size_t)cap * sizeof *list);
        }
        VEntry *e = &list[n++];
        snprintf(e->name, sizeof e->name, "%s", fd.cFileName);
        e->isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir("vendor");
    if (!d) return NULL;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char full[1024];
        snprintf(full, sizeof full, "vendor/%s", de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            list = realloc(list, (size_t)cap * sizeof *list);
        }
        VEntry *e = &list[n++];
        snprintf(e->name, sizeof e->name, "%s", de->d_name);
        e->isdir = S_ISDIR(st.st_mode) != 0;
    }
    closedir(d);
#endif
    *nout = n;
    return list;
}

/* vendor 下的 .c 有没有归到 stem 这一组的（同名或 <stem>_xxx）。
 * 只用来决定绑定里写不写 @link；链接本身由 build_program 注入。 */
static int vendor_has_c_for(const VEntry *ents, int n, const char *stem) {
    char exact[300], pref[300];
    snprintf(exact, sizeof exact, "%s.c", stem);
    snprintf(pref, sizeof pref, "%s_", stem);
    size_t pl = strlen(pref);
    for (int i = 0; i < n; i++) {
        if (ents[i].isdir) continue;
        const char *p = ents[i].name;
        size_t l = strlen(p);
        if (l < 3 || strcmp(p + l - 2, ".c") != 0) continue;
        if (!strcmp(p, exact) || !strncmp(p, pref, pl)) return 1;
    }
    return 0;
}

/* 已有清单是否已经指向这个目录：`zxc include vendor/foo --name bar` 会留下一张
 * vendor/bar.clib，此时不能再自动补一份 vendor/foo.zx。 */
static int vendor_root_claimed(const char *root) {
    int nm = 0, claimed = 0;
    char **mans = clib_scan_manifests("vendor", &nm);
    for (int i = 0; i < nm && !claimed; i++) {
        ClibManifest om;
        char e[256];
        if (clib_read_manifest(mans[i], &om, e, sizeof e) == 0) {
            claimed = !strcmp(om.root, root);
            clib_free(&om);
        }
        free(mans[i]);
    }
    free(mans);
    return claimed;
}

static int vendor_autoscan(void) {
    if (!path_is_dir("vendor")) return 0;
    int n = 0;
    VEntry *ents = vendor_entries(&n);
    if (!n) { free(ents); return 0; }
    const char *cc = (g_cc && *g_cc) ? g_cc : "";
    if (!cc[0] || mkdir_p(ZX_CACHE_DIR) != 0) { free(ents); return 0; }

    int added = 0;
    char err[1024];

    /* ① 子目录：整棵库树（include/ + src/ 任意深度） */
    for (int i = 0; i < n; i++) {
        if (!ents[i].isdir) continue;
        const char *dir = ents[i].name;
        char root[1100], quick1[1100], quick2[1100];
        snprintf(root, sizeof root, "vendor/%s", dir);
        snprintf(quick1, sizeof quick1, "vendor/%s.clib", dir);
        snprintf(quick2, sizeof quick2, "vendor/%s.zx", dir);
        if (fexists(quick1) || fexists(quick2)) continue;
        if (vendor_root_claimed(root)) continue;

        ClibManifest m;
        if (clib_discover(root, NULL, &m, err, sizeof err) != 0 || m.nhdr == 0) {
            clib_free(&m); continue;          /* 普通 zx 模块目录，不是 C 库 */
        }
        /* discover 会剥掉 -src/-master 之类的尾巴，清单名以 m.name 为准；
         * 否则 vendor/foo-master/ 每次 run 都会重新生成一遍。 */
        char mf[1100], zxpath[1100], libpath[1100], regen[1200];
        snprintf(mf, sizeof mf, "vendor/%s.clib", m.name);
        snprintf(zxpath, sizeof zxpath, "vendor/%s.zx", m.name);
        if (fexists(mf) || fexists(zxpath)) { clib_free(&m); continue; }
        snprintf(libpath, sizeof libpath, "vendor/lib%s.a", m.name);
        snprintf(regen, sizeof regen, "%s", root);

        char header[1100];
        if (clib_umbrella(&m, ZX_CACHE_DIR, header, sizeof header,
                          err, sizeof err) != 0) {
            fprintf(stderr, "zxc: %s: %s (skipped)\n", root, err);
            clib_free(&m); continue;
        }
        char *incs = clib_inc_flags(&m);
        int nskip = 0;
        char *text = cbind_generate(header, cc, m.nsrc > 0 ? libpath : NULL, 0,
                                    incs, (const char *const *)m.hdr, m.nhdr,
                                    regen, err, sizeof err, &nskip);
        free(incs);
        if (!text) {
            fprintf(stderr, "zxc: %s: %s (skipped)\n", root, err);
            clib_free(&m); continue;
        }
        int ok = (clib_write_manifest(mf, &m, root, regen, err, sizeof err) == 0);
        if (ok && write_text_file(zxpath, text) != 0) { remove(mf); ok = 0; }
        if (ok) {
            added++;
            printf("zxc: auto-added C library %s -> %s (%d header(s), %d source(s))\n",
                   root, zxpath, m.nhdr, m.nsrc);
        } else {
            fprintf(stderr, "zxc: %s: %s (skipped)\n", root, err);
        }
        free(text);
        clib_free(&m);
    }

    /* ② 顶层散放的 *.h：小库 / 单头库 */
    for (int i = 0; i < n; i++) {
        if (ents[i].isdir) continue;
        const char *file = ents[i].name;
        size_t l = strlen(file);
        if (l < 3 || strcmp(file + l - 2, ".h") != 0) continue;
        char stem[256];
        snprintf(stem, sizeof stem, "%.*s", (int)(l - 2), file);
        char zxpath[1100];
        snprintf(zxpath, sizeof zxpath, "vendor/%s.zx", stem);
        if (fexists(zxpath)) continue;

        char header[1100], libpath[1100], regen[1200];
        snprintf(header, sizeof header, "vendor/%s", file);
        snprintf(libpath, sizeof libpath, "vendor/lib%s.a", stem);
        snprintf(regen, sizeof regen, "%s", header);
        int nskip = 0;
        char *text = cbind_generate(header, cc,
                                    vendor_has_c_for(ents, n, stem) ? libpath : NULL,
                                    0, NULL, NULL, 0, regen, err, sizeof err, &nskip);
        if (!text) {
            fprintf(stderr, "zxc: %s: %s (skipped)\n", header, err);
            continue;
        }
        if (write_text_file(zxpath, text) == 0) {
            added++;
            printf("zxc: auto-added C header %s -> %s\n", header, zxpath);
        }
        free(text);
    }
    free(ents);
    return added;
}

/* 显式同步入口：编辑器里第一次写 @mod("foo") 之前先跑一次，
 * 生成的绑定才会出现在补全里（build/run 自己也会调，不必先 sync）。 */
static int cmd_sync(int argc, char **argv, int argi) {
    (void)argc; (void)argv; (void)argi;
    int n = vendor_autoscan();
    if (n > 0) printf("zxc sync: added %d C library(ies); `zxc build` will compile them\n", n);
    else printf("zxc sync: vendor/ is up to date, no new C libraries\n");
    return 0;
}

/* ---- 目录树模式：zxc include <库目录> --------------------------------------
 * 库文件保持原位：include/ 与 src/ 分离、任意深度子目录、甚至工程外的路径。
 * 只写一张 vendor/<name>.clib 清单（编译输入，可直接注释行裁剪），
 * 再把清单里所有 .c 合成一个 lib<name>.a。一个文件都不复制。
 */
static int cmd_include_tree(const char *root, const char *uname,
                        const char *ulib, int force, int no_build) {
    char err[1024];
    ClibManifest m;
    if (clib_discover(root, uname, &m, err, sizeof err) != 0) {
        fprintf(stderr, "zxc include: %s\n", err);
        return 1;
    }
    if (mkdir_one("vendor") != 0) {
        fprintf(stderr, "zxc include: cannot create vendor/\n");
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
        fprintf(stderr, "zxc include: '%s' already exists (use --force to overwrite)\n",
                fexists(mf) ? mf : zxpath);
        clib_free(&m); return 1;
    }
    if (clib_write_manifest(mf, &m, root, regen, err, sizeof err) != 0) {
        fprintf(stderr, "zxc include: %s\n", err);
        clib_free(&m); return 1;
    }
    printf("zxc include: %s\n", root);
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
        fprintf(stderr, "zxc include: %s\n", err);
        clib_free(&m); return 1;
    }
    char *incs = clib_inc_flags(&m);
    if (!incs) {
        fprintf(stderr, "zxc include: out of memory\n");
        clib_free(&m); return 1;
    }

    const char *lib = ulib;
    if (!no_build && !ulib && m.nsrc > 0) {
        int ncomp = 0;
        if (clib_build(g_cc, g_cflags ? g_cflags : CC_DEV, mf, ZX_CACHE_DIR, 0,
                       libpath, sizeof libpath, err, sizeof err, &ncomp) < 0) {
            fprintf(stderr, "zxc include: %s\n", err);
            free(incs); clib_free(&m); return 1;
        }
        printf("zxc include: compiled %d C unit(s) -> %s\n", ncomp, libpath);
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
    if (!text) { fprintf(stderr, "zxc include: %s\n", err); return 1; }
    int rc = write_text_file(zxpath, text);
    free(text);
    if (rc != 0) return 1;
    printf("zxc include: wrote %s (%d declaration(s) skipped)\n", zxpath, nskip);
    printf("  use:  const %s = @mod(\"%s\");\n", modname, modname);
    if (lib && lib[0])
        printf("  link: @link(\"%s\") is already in the binding\n", lib);
    else
        printf("  note: header-only, nothing to link\n");
    printf("  later: edit %s, then `zxc build` recompiles automatically\n", mf);
    return 0;
}

static int cmd_include(int argc, char **argv, int argi) {
    const char *name = NULL, *ulib = NULL, *uname = NULL;
    int force = 0, list = 0, no_build = 0;
    for (int i = argi; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) list = 1;
        else if (!strcmp(argv[i], "--force")) force = 1;
        else if (!strcmp(argv[i], "--no-build")) no_build = 1;
        else if (!strcmp(argv[i], "--name")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "zxc include: --name needs a module name\n");
                return 2;
            }
            uname = argv[++i];
        } else if (!strcmp(argv[i], "--lib")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "zxc include: --lib needs a library name\n");
                return 2;
            }
            ulib = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "zxc include: unknown option '%s'\n", argv[i]);
            return 2;
        } else if (!name) name = argv[i];
        else {
            fprintf(stderr, "zxc include: unexpected argument '%s'\n", argv[i]);
            return 2;
        }
    }
    if (list) {
        printf("zxc include -- built-in C library recipes\n\n");
        printf("  name        header        link\n");
        for (int i = 0; g_crecipes[i].alias; i++)
            printf("  %-11s %-13s %s\n", g_crecipes[i].alias,
                   g_crecipes[i].header,
                   g_crecipes[i].lib[0] ? g_crecipes[i].lib : "(none)");
        printf("\nA plain .h path works too:  zxc include ./vendor/foo.h\n");
        printf("Use --lib NAME / --force to override the link name / overwrite.\n");
        return 0;
    }
    if (!name) {
        fprintf(stderr, "usage: zxc include <name|header.h|dir> [--name NAME] [--lib NAME] [--no-build] [--force]\n"
                        "       zxc include --list\n");
        return 2;
    }

    /* 目录参数 = 库原本的目录树：不复制文件，走清单模式。 */
    if (path_is_dir(name)) return cmd_include_tree(name, uname, ulib, force, no_build);

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
            fprintf(stderr, "zxc include: cannot create .zxc-cache/\n");
            return 1;
        }
        char wrap[1024];
        snprintf(wrap, sizeof wrap, "#include <%s>\n", inc);
        if (write_text_file(".zxc-cache/zxc-include-wrap.h", wrap) != 0) return 1;
        snprintf(header, sizeof header, ".zxc-cache/zxc-include-wrap.h");
        allow_system = 1;
        printf("zxc include: <%s> not found locally, using toolchain headers\n", inc);
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
            fprintf(stderr, "zxc include: %s\n", cerr);
            return 1;
        }
        if (nlib > 0) {
            snprintf(builtlib, sizeof builtlib, "%s", libs[0]);
            if (ncomp > 0) printf("zxc include: compiled %d C unit(s) -> %s\n", ncomp, builtlib);
            else printf("zxc include: %s is up to date\n", builtlib);
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
            printf("zxc include: found local library %s, linking it\n", probe);
        }
    }

    if (mkdir_one("vendor") != 0) {
        fprintf(stderr, "zxc include: cannot create vendor/\n");
        return 1;
    }
    char outpath[1024];
    snprintf(outpath, sizeof outpath, "vendor/%s.zx", stem);
    if (!force && fexists(outpath)) {
        fprintf(stderr, "zxc include: '%s' already exists (use --force to overwrite)\n", outpath);
        return 1;
    }

    char err[1024];
    int nskip = 0;
    char *text = cbind_generate(header, g_cc, lib, allow_system, NULL, NULL, 0, name,
                                err, sizeof err, &nskip);
    if (!text) { fprintf(stderr, "zxc include: %s\n", err); return 1; }
    int rc = write_text_file(outpath, text);
    free(text);
    if (rc != 0) return 1;

    printf("zxc include: wrote %s (%d declaration(s) skipped)\n", outpath, nskip);
    printf("  use:  const %s = @mod(\"%s\");\n", stem, stem);
    if (lib && lib[0])
        printf("  link: @link(\"%s\") is already in the binding\n", lib);
    else
        printf("  note: no library linked yet; rerun with --lib NAME if the linker complains\n");
    if (!fexists("zx.json"))
        printf("  note: no zx.json here; run this inside a project (or use vendor/ directly)\n");
    return 0;
}

/* ---- add / fetch：从网站拉 C 库，两层声明与锁 -------------------------
 * `zxc add <url>`  往 zx.json 加一行 URL、把实际结果锁进 zx.lock、落地 vendor/。
 * `zxc fetch`      只按这两份文件校验 / 补齐，不改写声明。
 * run / build / test 一行联网代码都没有，只在缺库时提示去跑 fetch。
 */
static int fetch_lock_upsert(const FetchLock *item, char *err, size_t cap) {
    FetchLock *v = NULL;
    int n = fetch_lock_load(&v, err, cap);
    if (n < 0) return -1;
    FetchLock *all = malloc((size_t)(n + 1) * sizeof *all);
    if (!all) { fetch_lock_free(v, n); return -1; }
    int m = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(v[i].d.name, item->d.name) != 0) all[m++] = v[i];
    all[m++] = *item;
    int rc = fetch_lock_save(all, m, err, cap);
    free(all);
    fetch_lock_free(v, n);
    return rc;
}

/* `zxc add <url>` 重新构造后只剩一个参数：URL（外加 --force 重新锁定）。
 * 库名、源类型、sha256、commit、tree hash 全由工具推导并写进 zx.lock，
 * 人不用记第二个名字，也不用填任何可选字段。 */
static int cmd_add(int argc, char **argv, int argi) {
    const char *url = NULL;
    int force = 0;
    for (int i = argi; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--force")) force = 1;
        else if (a[0] == '-') {
            fprintf(stderr, "zxc add: unknown option '%s'\n", a);
            return 2;
        } else if (!url) url = a;
        else {
            fprintf(stderr, "zxc add: unexpected argument '%s'\n", a);
            return 2;
        }
    }
    if (!url) {
        fprintf(stderr, "usage: zxc add <url> [--force]\n");
        return 2;
    }
    if (!strstr(url, "://")) {
        fprintf(stderr, "zxc add: expected an https:// URL; a library already on disk"
                        " is handled by `zxc include <path>`\n");
        return 2;
    }
    if (!fexists("zx.json")) {
        fprintf(stderr, "zxc add: no zx.json here (run `zxc init` first)\n");
        return 1;
    }
    char err[1024];
    if (fetch_url_check(url, err, sizeof err) != 0) {
        fprintf(stderr, "zxc add: %s\n", err);
        return 2;
    }
    FetchDecl d;
    memset(&d, 0, sizeof d);
    fetch_name_of(url, d.name, sizeof d.name);
    snprintf(d.url, sizeof d.url, "%s", url);
    fetch_kind_of(d.url, NULL, 0, &d.kind, err, sizeof err);

    FetchLock *locks = NULL;
    int nl = fetch_lock_load(&locks, err, sizeof err);
    FetchLock *hit = NULL;
    for (int i = 0; i < nl; i++)
        if (!strcmp(locks[i].d.name, d.name)) hit = &locks[i];
    if (hit && !force) {
        fprintf(stderr, "zxc add: '%s' is already pinned in zx.lock;"
                        " use `zxc fetch` or --force to re-pin\n", d.name);
        fetch_lock_free(locks, nl);
        return 1;
    }
    FetchLock res;
    if (fetch_materialize(&d, NULL, force, &res, err, sizeof err) != 0) {
        fprintf(stderr, "zxc add: %s\n", err);
        fetch_lock_free(locks, nl);
        return 1;
    }
    fetch_lock_free(locks, nl);
    if (fetch_decl_save(url, err, sizeof err) != 0 ||
        fetch_lock_upsert(&res, err, sizeof err) != 0) {
        fprintf(stderr, "zxc add: %s\n", err);
        return 1;
    }
    vendor_autoscan();
    printf("zxc add: %s -> %s (%d file(s))\n", d.name, res.path, res.nfiles);
    printf("  zx.json: one URL added; zx.lock: tree %.12s\n", res.tree);
    if (d.kind == FK_GIT) printf("  resolved commit %s\n", res.resolved);
    else printf("  download sha256 %s\n", res.d.sha256);
    printf("  `zxc run` works offline from here; `zxc fetch %s` restores it elsewhere\n",
           d.name);
    return 0;
}

static int cmd_fetch(int argc, char **argv, int argi) {
    const char *only = NULL;
    int force = 0;
    for (int i = argi; i < argc; i++) {
        if (!strcmp(argv[i], "--force")) force = 1;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "zxc fetch: unknown option '%s'\n", argv[i]);
            return 2;
        } else if (!only) only = argv[i];
        else {
            fprintf(stderr, "zxc fetch: unexpected argument '%s'\n", argv[i]);
            return 2;
        }
    }
    char err[1024];
    FetchDecl *ds = NULL;
    FetchLock *ls = NULL;
    int nd = fetch_decls_load(&ds, err, sizeof err);
    if (nd < 0) {
        fprintf(stderr, "zxc fetch: %s\n", err);
        return 1;
    }
    int nl = fetch_lock_load(&ls, err, sizeof err);
    if (nd == 0) {
        printf("zxc fetch: no \"vendor\" URLs in zx.json\n");
        fetch_lock_free(ls, nl);
        return 0;
    }
    int ok = 0, bad = 0, seen = 0;
    for (int i = 0; i < nd; i++) {
        FetchDecl *d = &ds[i];
        /* name 或 URL 都收，省得为「这个 name 是什么」查文档。 */
        if (only && strcmp(only, d->name) != 0 && strcmp(only, d->url) != 0) continue;
        seen++;
        FetchLock *lk = NULL;
        for (int k = 0; k < nl; k++)
            if (!strcmp(ls[k].d.name, d->name)) lk = &ls[k];
        if (lk) {
            /* 声明里只有 URL：URL 变了就是换源，绝不猜。其余安全参数
             * （sha256 / ref / commit / subdir / tree）全在锁里，以锁为准。 */
            if (strcmp(d->url, lk->d.url) != 0) {
                fprintf(stderr, "zxc fetch: %s: URL in zx.json changed;"
                                " re-pin with `zxc add <url> --force`\n", d->name);
                bad++; continue;
            }
            char msg[512];
            int st = fetch_verify_entry(lk, msg, sizeof msg);
            if (st == 0 && !force) {
                printf("zxc fetch: %s up to date (%s)\n", d->name, lk->path);
                ok++; continue;
            }
            if (st == 2 && !force) {
                fprintf(stderr, "zxc fetch: %s: %s (use --force to restore)\n", d->name, msg);
                bad++; continue;
            }
            FetchDecl dd = lk->d;
            FetchLock res;
            if (fetch_materialize(&dd, lk, 1, &res, err, sizeof err) != 0) {
                fprintf(stderr, "zxc fetch: %s: %s\n", d->name, err);
                bad++; continue;
            }
            printf("zxc fetch: restored %s -> %s\n", d->name, res.path);
            ok++;
        } else {
            /* 锁里没有的源一律不在这里下载：fetch 只负责复现锁，引入新源
             * 必须显式 `zxc add <url>`。否则有人改一行 zx.json 就能让 fetch
             * 从任意地址拉代码进来编译，供应链口子太大。 */
            fprintf(stderr, "zxc fetch: %s: not in " ZX_LOCK_PATH
                            "; run `zxc add <url>` to pin a new source\n", d->name);
            bad++; continue;
        }
    }
    if (only && !seen) {
        fprintf(stderr, "zxc fetch: no vendor library named '%s' (name = URL tail;"
                        " run `zxc fetch` to check all)\n", only);
        bad++;
    }
    if (ok) vendor_autoscan();
    printf("zxc fetch: %d ok, %d problem(s)\n", ok, bad);
    fetch_decls_free(ds, nd);
    fetch_lock_free(ls, nl);
    return bad ? 1 : 0;
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
    g_zx_release = release;
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

/* §208: 抠出一个嵌套对象的正文区间 {"key": { ... }} -> [start, end)。
 * 大括号配平、跳过字符串里的大括号与转义。返回 false 表示没有该对象。
 * 用途：读 "platform": {...} 这类嵌套字段，避免 strstr 撞到别处的同名 key。 */
static bool json_obj_span(const char *text, const char *key,
                          const char **bs, const char **be) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(text, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), '{');
    if (!p) return false;
    const char *s = p + 1;
    int depth = 1;
    bool instr = false, esc = false;
    for (const char *q = s; *q; q++) {
        if (instr) {
            if (esc) { esc = false; continue; }
            if (*q == '\\') { esc = true; continue; }
            if (*q == '"') instr = false;
            continue;
        }
        if (*q == '"') { instr = true; continue; }
        if (*q == '{') depth++;
        else if (*q == '}') {
            if (--depth == 0) { *bs = s; *be = q; return true; }
        }
    }
    return false;
}

/* 在一个子串区间里抠 bool：找 "key": true/false。缺省返回 def。 */
static bool json_bool_in(const char *bs, const char *be, const char *key, bool def) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    size_t plen = strlen(pat);
    for (const char *p = bs; p && p + plen < be; p++) {
        if (strncmp(p, pat, plen)) continue;
        const char *q = p + plen;
        while (q < be && (*q == ' ' || *q == ':' || *q == '\t')) q++;
        if (!strncmp(q, "true", 4)) return true;
        if (!strncmp(q, "false", 5)) return false;
    }
    return def;
}

/* 在一个子串区间里抠 "key": "value"（区间版 json_get_str）。 */
static bool json_str_in(const char *bs, const char *be, const char *key,
                        char *out, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    size_t plen = strlen(pat);
    for (const char *p = bs; p && p + plen < be; p++) {
        if (strncmp(p, pat, plen)) continue;
        const char *q = p + plen;
        while (q < be && *q != '"') { if (*q == '}') return false; q++; }
        if (q >= be) return false;
        q++;
        size_t i = 0;
        while (q < be && *q != '"' && i + 1 < cap) out[i++] = *q++;
        out[i] = 0;
        return true;
    }
    return false;
}

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

    /* §208: targets 数组。缺省为空 = 宿主单目标（向后兼容，老 zx.json 不变）。 */
    pj->ntargets = json_get_str_array(text, "targets", pj->targets, PROJ_MAX_TARGETS);

    /* §208: "platform": { "generate": bool, "dir": str }。
     * 缺省：generate=true、dir=.zxc-cache/platform——只要有 targets，默认就
     * 生成平台镜像（用户只改 targets 一项即可跨平台）。段内可覆盖。 */
    pj->platform_gen = true;
    snprintf(pj->platform_dir, sizeof pj->platform_dir, ".zxc-cache/platform");
    const char *pbs, *pbe;
    if (json_obj_span(text, "platform", &pbs, &pbe)) {
        pj->platform_gen = json_bool_in(pbs, pbe, "generate", true);
        if (json_str_in(pbs, pbe, "dir", pj->platform_dir, sizeof pj->platform_dir) &&
            !pj->platform_dir[0])
            snprintf(pj->platform_dir, sizeof pj->platform_dir, ".zxc-cache/platform");
    }
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

/* ------------------------------------------------- 单文件模式的搜索路径
 * 单文件分派（`zxc <file> --check` / `zxc <file> -o ...`）也要认识工程级
 * 模块搜索路径。build/run 走 do_build -> setup_search_paths，`zxc check`
 * 子命令也调它，唯独这条分派以前没调：编译器只试了「导入文件所在目录 +
 * 模块名」就放弃，vendor/ 里的绑定明明存在。LSP 的诊断正是 spawn
 * `zxc <影子文件> --check`，影子文件落在 src/ 下，于是出现「能编过、
 * 编辑器却报 cannot open module」的假报错。
 *
 * KEEP: 这条调用别删。删了 LSP 会重新误报，而 zxc build/run 仍然全绿，
 * 光跑全量回归发现不了——守门在 test.sh 2.54d。
 *
 * 只往 sema 注入路径，不落盘、不联网：诊断是高频只读操作，绝不在这里跑
 * vendor_autoscan / fetch_check_local。cwd 未必是工程根（Zed 多根工作区、
 * 命令行从别处给绝对路径），所以 cwd 无 zx.json 时从源文件目录逐级向上
 * 找工程根，把 vendor/ 与 deps 拼成绝对路径再注入；找不到就维持原状，
 * 孤立 .zx 文件的行为不变。
 */
static bool path_is_abs_c(const char *p) {
    if (!p || !p[0]) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    return p[1] == ':' && ((p[0] >= 'A' && p[0] <= 'Z') ||
                           (p[0] >= 'a' && p[0] <= 'z'));
}

static char g_search_abs[48][1024];   /* 绝对路径缓冲，进程内常驻、零泄漏 */

static void setup_search_paths_for_file(const char *file) {
    Proj pj;
    if (proj_load(&pj)) { setup_search_paths(&pj); return; }

    char dir[1024];
    path_abs(file, dir, sizeof dir);
    for (char *q = dir; *q; q++) if (*q == '\\') *q = '/';
    char *last = strrchr(dir, '/');
    if (!last) return;
    *last = 0;

    /* 工程根不唯一也不重要：沿向上路径把**每一层**的 vendor/ 都收进搜索
     * 列表（近→远），zx.json 命中层再读它的 deps。@mod("name") 依次
     * 尝试：源文件目录 → 各层 vendor/ → deps。嵌套工程、测试影子目录
     * 都不会抢根——搜索路径本来就是一个列表，不需要"唯一的根"。 */
    char root[1024];
    root[0] = 0;
    int n = 0;
    char zx[1060];
    for (int up = 0; up < 32 && dir[0]; up++) {
        snprintf(zx, sizeof zx, "%s/vendor", dir);
        if (fexists(zx) && n < 47) {
            snprintf(g_search_abs[n], sizeof g_search_abs[n], "%s", zx);
            g_search[n++] = g_search_abs[n];
        }
        snprintf(zx, sizeof zx, "%s/zx.json", dir);
        if (fexists(zx) && !root[0]) snprintf(root, sizeof root, "%s", dir);
        char *cut = strrchr(dir, '/');
        if (!cut || cut == dir) break;
        if (cut - dir == 2 && dir[1] == ':') break;   /* E:/ 已到盘符根 */
        *cut = 0;
    }
    if (root[0]) {
        snprintf(zx, sizeof zx, "%s/zx.json", root);
        FILE *f = fopen(zx, "rb");
        if (f) {
            char text[8192];
            size_t len = fread(text, 1, sizeof text - 1, f);
            fclose(f);
            text[len] = 0;
            char *deps[16];
            int nd = json_get_str_array(text, "deps", deps, 16);
            for (int i = 0; i < nd && n < 47; i++) {
                char d[1024];
                if (path_is_abs_c(deps[i])) snprintf(d, sizeof d, "%s", deps[i]);
                else snprintf(d, sizeof d, "%s/%s", root, deps[i]);
                if (fexists(d)) {
                    snprintf(g_search_abs[n], sizeof g_search_abs[n], "%s", d);
                    g_search[n] = g_search_abs[n];
                    n++;
                }
                free(deps[i]);
            }
        }
    }
    g_search[n] = NULL;
    sema_set_search_paths(g_search);
}

/* §208: 构建单个目标。target == NULL 表示宿主单目标（老路径）。
 * 产物路径写回 outpath。返回 0 成功。 */
static int do_build_one(Proj *pj, const Target *target, bool release,
                        OutKind kind, char *outpath, size_t cap) {
    g_target = (Target *)target;
    if (!fexists(pj->main)) {
        fprintf(stderr, "zxc: entry source '%s' not found\n", pj->main);
        return 1;
    }
    /* §208: 非宿主目标先生成平台 std 镜像，注入 sema；宿主目标清空覆盖。 */
    if (target && pj->platform_gen) {
        char mirror[2048];
        if (platform_gen_std(pj, target, mirror, sizeof mirror) != 0) return 1;
        sema_set_std_path(mirror);
    } else {
        sema_set_std_path(NULL);
    }

    /* 只读、绝不联网：zx.lock 里登记的库若被删了，先给一条可操作的提示。
     * 构建本身永远不会去下载，缺库时交给用户显式跑 `zxc fetch`。 */
    fetch_check_local(0, stderr);
    /* 必须在 fe_parse_file 之前：入口源码里 @mod("foo") 要能找到刚
     * 生成的 vendor/foo.zx，否则第一次 `zxc run` 就报模块找不到。 */
    vendor_autoscan();
    mkdir_p(ZX_CACHE_DIR);
    mkdir_p("out");
    /* §208: 目标中间文件目录 .zxc-cache/<target>/，与 build_program 的 stem 对应 */
    if (target && target->name[0]) {
        char tcdir[600];
        snprintf(tcdir, sizeof tcdir, "%s/%s", ZX_CACHE_DIR, target->name);
        mkdir_p(tcdir);
    }
    setup_search_paths(pj);

    const char *base = pj->out;
    /* §208: 多目标产物分目录 out/<target>/，避免互相覆盖。 */
    if (target && target_is_cross(target))
        snprintf(outpath, cap, "out/%s/%s", target->name, base);
    else
        snprintf(outpath, cap, "out/%s", base);
    /* 后缀由 build_program 按目标补（.exe / 空），这里不写死。库后缀按目标。 */
    if (kind == OUT_STATIC) {
        const char *pfx = (target && target->lib_prefix[0]) ? target->lib_prefix : "";
        const char *sfx = (target && target->lib_suffix[0]) ? target->lib_suffix : ".a";
        char name[512];
        snprintf(name, sizeof name, "%s%s%s", pfx, base, sfx);
        if (target && target_is_cross(target))
            snprintf(outpath, cap, "out/%s/%s", target->name, name);
        else
            snprintf(outpath, cap, "out/%s", name);
    } else if (kind == OUT_SHARED) {
        const char *sfx = (target && target->dll_suffix[0]) ? target->dll_suffix :
#ifdef _WIN32
                          ".dll";
#else
                          ".so";
#endif
        strncat(outpath, sfx, cap - strlen(outpath) - 1);
    }
    /* §208: 产物在 out/<target>/ 时先建目录。 */
    if (target && target_is_cross(target)) {
        char odir[600];
        snprintf(odir, sizeof odir, "out/%s", target->name);
        mkdir_p(odir);
    }

    g_cflags = release ? CC_REL : CC_DEV;
    g_zx_release = release;
    g_srcpath = pj->main;
    Node *root = fe_parse_file(pj->main);
    if (!root || g_error_count) { g_target = NULL; return 1; }
    sema_check(root);
    if (g_error_count) {
        if (g_error_count > g_diag_shown)
            fprintf(stderr, "zxc: %d error(s) (first %d shown)\n",
                    g_error_count, g_diag_shown);
        g_target = NULL;
        return 1;
    }
    int rc = build_program(root, outpath, ZX_CACHE_DIR, kind);
    g_target = NULL;
    return rc;
}

/* 构建 + 产物路径写回 outpath。§208: 有 targets 就逐目标构建，
 * outpath 返回最后一个（供 run 用；多目标时 run 本身无意义，见 cmd_run）。 */
static int do_build(Proj *pj, bool release, OutKind kind, char *outpath, size_t cap) {
    if (!fexists(pj->main)) {
        fprintf(stderr, "zxc: entry source '%s' not found\n", pj->main);
        return 1;
    }
    if (pj->ntargets == 0)
        return do_build_one(pj, NULL, release, kind, outpath, cap);

    /* §208: 每个目标在独立子进程里构建。原因：sema 的后端 typedef 编号、
     * 模块表、AST 缓存是进程级全局，跨目标复用会串味（实测：先 windows
     * 后 linux 时 zxrt 拿到 Windows 版 typedef、net 拿到 closesocket）。
     * 子进程天然隔离所有全局状态，代价是每个目标多一次 zxc 启动。 */
    const char *kindflag = kind == OUT_STATIC ? "--static" :
                           kind == OUT_SHARED ? "--shared" : "--exe";
    const char *relflag = release ? "--release" : "";
    int built = 0;
    for (int i = 0; i < pj->ntargets; i++) {
        Target t;
        if (!target_init(&t, pj->targets[i])) {
            fprintf(stderr, "zxc: unknown target '%s' (see zxc targets)\n", pj->targets[i]);
            return 1;
        }
        prog("=== target %s (%s) ===", t.name, t.triple);
        char cmd[2400];
        snprintf(cmd, sizeof cmd, "\"\"%s\" --build-one \"%s\" %s %s\"",
                 g_argv0 ? g_argv0 : "zxc", t.name, kindflag, relflag);
        int rc = system(cmd);
        if (rc != 0) {
            fprintf(stderr, "zxc: build failed for target %s\n", t.name);
            return 1;
        }
        built++;
        /* outpath 返回最后一个目标的产物路径 */
        if (target_is_cross(&t))
            snprintf(outpath, cap, "out/%s/%s", t.name, pj->out);
        else
            snprintf(outpath, cap, "out/%s", pj->out);
    }
    if (built == 0) return 1;
    return 0;
}

/* §208: 内部入口 —— 只构建一个目标（由 do_build 的多目标循环 spawn）。 */
static int build_one_mode(const char *target_name, bool release, OutKind kind) {
    Proj pj;
    if (!proj_load(&pj)) {
        fprintf(stderr, "zxc: zx.json not found here (--build-one)\n");
        return 2;
    }
    Target t;
    if (!target_init(&t, target_name)) {
        fprintf(stderr, "zxc: unknown target '%s'\n", target_name);
        return 2;
    }
    char outpath[2048];
    return do_build_one(&pj, &t, release, kind, outpath, sizeof outpath);
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

/* §208: zxc targets —— 列出内置目标预设。 */
static int cmd_targets(void) {
    printf("Built-in targets (put names in zx.json \"targets\"):\n\n");
    printf("  %-18s %-22s %-6s %-7s %s\n", "name", "triple", "arch", "family", "exe");
    for (int i = 0; ; i++) {
        const PlatPreset *p = plat_at(i);
        if (!p) break;
        printf("  %-18s %-22s %-6s %-7s %s\n", p->name, p->triple, p->arch,
               p->family, p->exe_suffix[0] ? p->exe_suffix : "(none)");
    }
    printf("\nHost target: %s\n", plat_host_name());
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
 * @mod("../xxx.zx")（测试目录的上一级就是 src/）。 * ---- */

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
    /* 父进程经 cmd.exe 传参会把 '/' 换成 '\\'；runner 源码里 @mod("…")
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
    buf_printf(&b, "const t = @mod(\"%s\");\n", modrel);
    buf_puts(&b, "const std = @mod(\"std\");\n\n");
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
    g_zx_release = release;
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
    fetch_check_local(0, stderr);          /* 只读：缺库给提示，不联网 */
    vendor_autoscan();                     /* 测试模块可能引用 vendor 里的 C 库 */
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
    g_argv0 = argv[0];
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
    /* §208: 单目标构建子进程（由多目标循环的 system() 拉起）。 */
    if (argc >= 3 && !strcmp(argv[1], "--build-one")) {
        bool rel = false;
        OutKind kind = OUT_EXE;
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--release")) rel = true;
            else if (!strcmp(argv[i], "--static")) kind = OUT_STATIC;
            else if (!strcmp(argv[i], "--shared")) kind = OUT_SHARED;
        }
        return build_one_mode(argv[2], rel, kind);
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
            /* §132：region 不是错误码，是分配账本查询 */
            if (!strcmp(argv[2], "region")) {
                if (argc < 4) {
                    fprintf(stderr, "usage: zxc explain region <file.zx>\n");
                    return 2;
                }
                return explain_region(argv[3]);
            }
            return errors_print_one(stdout, argv[2]);
        }
        if (!strcmp(sub, "init"))  return cmd_init(argc, argv, 2);
        if (!strcmp(sub, "include")) return cmd_include(argc, argv, 2);
        if (!strcmp(sub, "add"))     return cmd_add(argc, argv, 2);
        if (!strcmp(sub, "fetch"))   return cmd_fetch(argc, argv, 2);
        if (!strcmp(sub, "sync"))    return cmd_sync(argc, argv, 2);
        if (!strcmp(sub, "cbuild")) return cmd_cbuild(argc, argv, 2);
        if (!strcmp(sub, "targets")) return cmd_targets();
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
    g_zx_release = release;

    g_srcpath = file;
    /* KEEP: LSP 诊断走这条单文件分派；没有工程搜索路径就会假报
     * `cannot open module '.../src/<name>'`（vendor/ 里的模块明明在）。 */
    setup_search_paths_for_file(file);

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

    double t_fe0 = prog_clock();
    Node *root = fe_parse_file(file);
    if (getenv("ZX_TIMING"))
        fprintf(stderr, "[timing] frontend %.3f s\n", prog_clock() - t_fe0);
    if (!root || g_error_count) return 1;

    if (dump_ast) {
        ast_dump(root, 0);
    } else {
        if (check && g_error_count == 0) {
            double t_s0 = prog_clock();
            sema_check(root);
            if (getenv("ZX_REGION") && g_error_count == 0)
                region_ledger_dump(stderr, file);
            if (getenv("ZX_TIMING"))
                fprintf(stderr, "[timing] sema %.3f s\n", prog_clock() - t_s0);
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
                /* §192：@shim 通道已删除，--emit-c 不再往末尾贴 C TU。 */
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
