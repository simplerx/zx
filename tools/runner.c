/* tools/runner.c —— Zx 回归 + 性能 runner（取代 tools/runner.zx）
 *
 * 为什么重写而不是修旧的：
 *   ① 旧版用 system() 拼 cmd 命令行。自己一启子进程，被重定向的 stdout 就整块
 *      丢失（实测 1/6），"全过"三个字可能只留在内存里 —— 报告丢了比测试挂了更糟。
 *   ② cmd 的引号规则得绕着走（要自己先包一层引号让它剥），路径还得手工转反斜杠。
 *   ③ 没有超时。一个卡住的用例能把 CI 挂到天荒地老。
 *   ④ 没有计时。"编译变慢了多少"永远只能靠感觉。
 *
 * 所以这里自己起进程（Windows: CreateProcessA / POSIX: fork+exec），自管重定向、
 * 超时、计时，全程不经过 shell；判定规则与原版逐条一致：
 *
 *   tests/bad、tests/multibad：必须编译失败，且 stderr 里有 `// expect:` 的文本
 *                              （可选 `// expect-code:` 断码、`// expect-count:` 断条数）
 *   其他：必须编译通过；源码里有 `// expect-output`（且是完整入口）时还要编成
 *         二进制跑一遍，stdout 逐行对得上，并可 `// expect-exit:` 断退出码。
 *
 * 用法：
 *   runner [选项] <zxc> <tmp前缀> <file.zx>...
 *
 *   --perf / --no-perf   打印（默认）/不打印性能小结
 *   --repeat N           前端计时取 N 次里最快的一次（默认 3；只影响计时，
 *                        不重复跑 zig cc）
 *   --save F             把每项的前端耗时写成基线 TSV
 *   --baseline F         与基线比，列出回归 / 变快（默认只提示，见 --fail-on-regress）
 *   --fail-on-regress P  规模耗时超过基线 P%（默认 25）就以非零退出
 *   --timeout MS         单个子进程的上限（默认 20000）
 *   --filter S           只跑路径里含 S 的用例
 *   --quiet              只打失败与小结点
 */
#ifdef _WIN32
#  define _CRT_SECURE_NO_WARNINGS
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <unistd.h>
#  include <sys/wait.h>
#  include <sys/types.h>
#  include <fcntl.h>
#  include <signal.h>
#  include <time.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/* ---------------------------------------------------------------- 小工具 */

typedef struct { char *p; size_t n, cap; } Buf;

static void die(const char *msg) {
    fprintf(stderr, "runner: %s\n", msg);
    exit(2);
}

static void buf_reserve(Buf *b, size_t extra) {
    if (b->n + extra + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->n + extra + 1) cap *= 2;
    char *np = (char *)realloc(b->p, cap);
    if (!np) die("out of memory");
    b->p = np;
    b->cap = cap;
}

static void buf_add(Buf *b, const char *s, size_t n) {
    buf_reserve(b, n);
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void buf_puts(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }
static void buf_putc(Buf *b, char c) { buf_add(b, &c, 1); }
static void buf_free(Buf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

static bool read_file(const char *path, Buf *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char tmp[8192];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) buf_add(out, tmp, n);
    fclose(f);
    return true;
}

static bool contains(const char *hay, size_t hlen, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || nlen > hlen) return false;
    for (size_t i = 0; i + nlen <= hlen; i++)
        if (!memcmp(hay + i, needle, nlen)) return true;
    return false;
}

/* 路径里有没有某一段（两种分隔符都认，绝对路径也能判） */
static bool has_dir(const char *p, const char *dir) {
    char pat[128];
    snprintf(pat, sizeof pat, "%s/", dir);
    if (!strncmp(p, pat, strlen(pat))) return true;
    if (contains(p, strlen(p), pat)) return true;
    snprintf(pat, sizeof pat, "%s\\", dir);
    if (!strncmp(p, pat, strlen(pat))) return true;
    if (contains(p, strlen(p), pat)) return true;
    return false;
}

/* ---------------------------------------------------------------- 计时 */

static unsigned long long now_ns(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static bool init;
    LARGE_INTEGER c;
    if (!init) { QueryPerformanceFrequency(&freq); init = true; }
    QueryPerformanceCounter(&c);
    unsigned long long f = (unsigned long long)freq.QuadPart;
    return (unsigned long long)(c.QuadPart / f) * 1000000000ULL +
           (unsigned long long)(c.QuadPart % f) * 1000000000ULL / f;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec;
#endif
}

static long proc_id(void) {
#ifdef _WIN32
    return (long)GetCurrentProcessId();
#else
    return (long)getpid();
#endif
}

/* 临时文件前缀一律转绝对路径：Windows 的 CreateProcess 要完整路径，
 * 相对路径 + 正斜杠有时起不来 —— 而且是**静默**起不来（拿不到退出码）。 */
static const char *abs_prefix(const char *prefix, char *buf, size_t cap) {
#ifdef _WIN32
    char full[4096];
    DWORD n = GetFullPathNameA(prefix, sizeof full, full, NULL);
    if (n == 0 || n >= sizeof full) return prefix;
    for (char *p = full; *p; p++)
        if (*p == '/') *p = '\\';
    snprintf(buf, cap, "%s", full);
    return buf;
#else
    (void)buf;
    (void)cap;
    return prefix;
#endif
}

static int cmp_ull(const void *a, const void *b) {
    unsigned long long x = *(const unsigned long long *)a, y = *(const unsigned long long *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* ---------------------------------------------------------------- 起进程 */

typedef struct {
    int code;              /* 退出码；-1 = 没起来 / 被杀 */
    bool timed_out;
    unsigned long long ns;
} RunRes;

#ifdef _WIN32
/* 按 Windows 的命令行规则给一个参数加引号（反斜杠只在引号前加倍）。 */
static void win_quote(Buf *cl, const char *arg) {
    bool need = (*arg == 0);
    for (const char *p = arg; *p; p++)
        if (*p == ' ' || *p == '\t' || *p == '"') need = true;
    if (!need) { buf_puts(cl, arg); return; }

    buf_putc(cl, '"');
    for (const char *p = arg;;) {
        size_t bs = 0;
        while (*p == '\\') { p++; bs++; }
        if (*p == '"') {
            for (size_t i = 0; i < bs * 2 + 1; i++) buf_putc(cl, '\\');
            buf_putc(cl, '"');
            p++;
        } else if (*p == 0) {
            for (size_t i = 0; i < bs * 2; i++) buf_putc(cl, '\\');
            break;
        } else {
            for (size_t i = 0; i < bs; i++) buf_putc(cl, '\\');
            buf_putc(cl, *p);
            p++;
        }
    }
    buf_putc(cl, '"');
}

/* 跑一个程序，stdout 与 stderr 一起收进 out_path（等价于旧的 `> f 2>&1`）。 */
static RunRes run_to_file(char *const argv[], const char *out_path, int timeout_ms) {
    RunRes r = { -1, false, 0 };

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE hout = CreateFileA(out_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hout == INVALID_HANDLE_VALUE) return r;
    HANDLE hnul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &sa, OPEN_EXISTING, 0, NULL);

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hnul;
    si.hStdOutput = hout;
    si.hStdError = hout;

    Buf cl = { 0 };
    for (int i = 0; argv[i]; i++) {
        if (i) buf_putc(&cl, ' ');
        win_quote(&cl, argv[i]);
    }

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof pi);
    unsigned long long t0 = now_ns();
    BOOL ok = CreateProcessA(argv[0], cl.p, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(hout);
    if (hnul != INVALID_HANDLE_VALUE) CloseHandle(hnul);
    buf_free(&cl);
    if (!ok) return r;

    DWORD w = WaitForSingleObject(pi.hProcess, timeout_ms > 0 ? (DWORD)timeout_ms : INFINITE);
    if (w == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 0x7FFFFFFF);
        WaitForSingleObject(pi.hProcess, 5000);
        r.timed_out = true;
    }
    DWORD ec = 0;
    GetExitCodeProcess(pi.hProcess, &ec);
    r.ns = now_ns() - t0;
    r.code = (int)ec;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return r;
}
#else
static RunRes run_to_file(char *const argv[], const char *out_path, int timeout_ms) {
    RunRes r = { -1, false, 0 };
    int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return r;

    unsigned long long t0 = now_ns();
    pid_t pid = fork();
    if (pid == 0) {
        int nfd = open("/dev/null", O_RDONLY);
        if (nfd >= 0) { dup2(nfd, 0); close(nfd); }
        dup2(fd, 1);
        dup2(fd, 2);
        close(fd);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fd);
    if (pid < 0) return r;

    int st = 0;
    for (;;) {
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) break;
        if (w < 0) break;
        if (timeout_ms > 0 && (now_ns() - t0) / 1000000ULL >= (unsigned long long)timeout_ms) {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            r.timed_out = true;
            break;
        }
        usleep(1000);
    }
    r.ns = now_ns() - t0;
    r.code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return r;
}
#endif

/* Windows 上刚跑完的 exe 还被映像占用，删不掉会让下一次链接失败。
 * 每个用例的产物名都带序号，删不掉也不影响别人。 */
static void remove_retry(const char *path) {
    for (int i = 0; i < 20; i++) {
        if (remove(path) == 0) return;
#ifdef _WIN32
        Sleep(5);
#else
        usleep(5000);
#endif
    }
}

/* ---------------------------------------------------------------- 源码解析 */

typedef struct { const char *s; size_t len; } Line;

static Line *split_lines(char *data, size_t len, int *out_n) {
    Line *ls = (Line *)malloc(sizeof(Line) * (len / 2 + 8));
    if (!ls) die("out of memory");
    int n = 0;
    size_t i = 0;
    for (;;) {
        size_t j = i;
        while (j < len && data[j] != '\n') j++;
        size_t e = j;
        if (e > i && data[e - 1] == '\r') e--;
        while (e > i && (data[e - 1] == ' ' || data[e - 1] == '\t')) e--;
        ls[n].s = data + i;
        ls[n].len = e - i;
        n++;
        if (j >= len) break;
        i = j + 1;
    }
    *out_n = n;
    return ls;
}

/* 取第一条 `// key` 之后的文本（key 里含 "// " 前缀，比如 "// expect: "）。 */
static const char *directive(Line *ls, int n, const char *key, size_t *out_len) {
    size_t k = strlen(key);
    for (int i = 0; i < n; i++) {
        if (ls[i].len >= k && !memcmp(ls[i].s, key, k)) {
            if (out_len) *out_len = ls[i].len - k;
            return ls[i].s + k;
        }
    }
    return NULL;
}

static bool line_is(Line *l, const char *s) {
    size_t n = strlen(s);
    return l->len == n && !memcmp(l->s, s, n);
}

/* `// expect-output` 之后连续的 `// ` 行拼成期望的 stdout。
 * 裸写标记而后面没有内容 = "这个程序什么都不打印"，也是一条断言 ——
 * 所以用 present 表示"写了没写"，不能拿"行数为 0"代替。 */
static void expect_output(Line *ls, int n, bool *present, Buf *out) {
    *present = false;
    bool on = false;
    int cnt = 0;
    for (int i = 0; i < n; i++) {
        if (!on) {
            if (line_is(&ls[i], "// expect-output")) { on = true; *present = true; }
            continue;
        }
        if (!(ls[i].len >= 3 && !memcmp(ls[i].s, "// ", 3))) { on = false; continue; }
        if (cnt++) buf_putc(out, '\n');
        buf_add(out, ls[i].s + 3, ls[i].len - 3);
    }
}

/* 入口两种写法都算有 main：旧式 `fn main(`，和 v0.24 起的 `const main = fn (…)`。
 * 判错了不会报错 —— 只会静默跳过运行，让 expect-output 用例退化成纯类型检查。 */
static bool has_main(const char *src, size_t len) {
    return contains(src, len, "fn main(") || contains(src, len, "main = fn");
}

/* 去掉 \r，并去掉末尾空白 —— 重定向出来的 stdout 是 CRLF，期望里写的是 LF。 */
static void normalize(const char *s, size_t len, Buf *out) {
    for (size_t i = 0; i < len; i++)
        if (s[i] != '\r') buf_putc(out, s[i]);
    while (out->n > 0) {
        char c = out->p[out->n - 1];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') out->n--;
        else break;
    }
    if (out->p) out->p[out->n] = 0;
}

static bool str_eq(const Buf *a, const Buf *b) {
    if (a->n != b->n) return false;
    if (a->n == 0) return true;
    return !memcmp(a->p, b->p, a->n);
}

static void print_indented(const char *s, size_t len) {
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && s[j] != '\n') j++;
        size_t e = j;
        if (e > i && s[e - 1] == '\r') e--;
        printf("       %.*s\n", (int)(e - i), s + i);
        i = j + 1;
    }
    fflush(stdout);
}

static size_t count_errors(const char *log, size_t len) {
    size_t n = 0;
    for (size_t i = 0; i + 6 < len; i++) {
        if (!memcmp(log + i, "error[E", 7)) { n++; i += 6; }
    }
    return n;
}

/* ---------------------------------------------------------------- 统计 */

typedef struct {
    const char *path;
    unsigned long long front_ns, build_ns, run_ns;
    long lines;
    long bytes;
    bool is_err;
    bool did_build, did_run;
} CaseStat;

static CaseStat *g_cases;
static int g_ncases, g_cap;

static CaseStat *new_stat(void) {
    if (g_ncases == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 128;
        g_cases = (CaseStat *)realloc(g_cases, sizeof(CaseStat) * (size_t)g_cap);
        if (!g_cases) die("out of memory");
    }
    CaseStat *c = &g_cases[g_ncases++];
    memset(c, 0, sizeof *c);
    return c;
}

static void fmt_ms(char *dst, size_t cap, unsigned long long ns) {
    if (ns < 1000000ULL) snprintf(dst, cap, "%.2f ms", (double)ns / 1e6);
    else if (ns < 1000000000ULL) snprintf(dst, cap, "%.0f ms", (double)ns / 1e6);
    else snprintf(dst, cap, "%.2f s", (double)ns / 1e9);
}

/* offset = 每项里要扣掉的固定开销（进程启动）。小文件的前端耗时几乎全是它，
 * 不扣的话 136 项全是 16 ms，看谁慢只能看见抖动。 */
static void print_bucket(const char *label, unsigned long long *v, int n,
                         unsigned long long offset) {
    if (n <= 0) return;
    unsigned long long total = 0;
    unsigned long long *sorted = (unsigned long long *)malloc(sizeof(unsigned long long) * (size_t)n);
    if (!sorted) return;
    for (int i = 0; i < n; i++) {
        unsigned long long x = v[i] > offset ? v[i] - offset : 0;
        sorted[i] = x;
        total += x;
    }
    qsort(sorted, (size_t)n, sizeof(unsigned long long), cmp_ull);

    unsigned long long med = sorted[n / 2];
    unsigned long long p90 = sorted[(int)((double)(n - 1) * 0.9)];
    unsigned long long mx = sorted[n - 1];
    char a[32], b[32], c[32], d[32], e[32];
    fmt_ms(a, sizeof a, total);
    fmt_ms(b, sizeof b, total / (unsigned long long)n);
    fmt_ms(c, sizeof c, med);
    fmt_ms(d, sizeof d, p90);
    fmt_ms(e, sizeof e, mx);
    printf("  %-18s %3d 项   总 %s   平均 %s   中位 %s   p90 %s   最慢 %s\n",
           label, n, a, b, c, d, e);
    free(sorted);
}

/* 基线文件：TSV，每行 `path<TAB>front_ns<TAB>lines`。 */
typedef struct { char *path; unsigned long long ns; } BaseRec;

static int g_nbase;
static BaseRec *g_base;

static void load_baseline(const char *file) {
    Buf b = { 0 };
    if (!read_file(file, &b)) {
        printf("  ! 读不到基线 %s，跳过对比\n", file);
        return;
    }
    int n = 0;
    Line *ls = split_lines(b.p, b.n, &n);
    g_base = (BaseRec *)malloc(sizeof(BaseRec) * (size_t)(n + 1));
    for (int i = 0; i < n; i++) {
        if (ls[i].len == 0 || ls[i].s[0] == '#') continue;
        /* 手切三列：路径里可能有空格，但不会有 TAB */
        const char *s = ls[i].s;
        const char *t1 = memchr(s, '\t', ls[i].len);
        if (!t1) continue;
        size_t plen = (size_t)(t1 - s);
        char *path = (char *)malloc(plen + 1);
        memcpy(path, s, plen);
        path[plen] = 0;
        g_base[g_nbase].path = path;
        g_base[g_nbase].ns = strtoull(t1 + 1, NULL, 10);
        g_nbase++;
    }
    free(ls);
    buf_free(&b);
}

static unsigned long long base_lookup(const char *path) {
    for (int i = 0; i < g_nbase; i++)
        if (!strcmp(g_base[i].path, path)) return g_base[i].ns;
    return 0;
}

/* ---------------------------------------------------------------- 规模检查 */

static bool g_quiet;
static int g_timeout_ms = 20000;
static unsigned long long g_startup_ns;
/* 规模曲线的结果。基线只认这一组数：它是单进程、净耗时够大，噪声 < 10%；
 * 单项耗时的抖动（进程启动 ±5 ms）比它要测的东西还大，拿来对比只会误报。 */
static unsigned long long g_scale_ns[2];
static long               g_scale_lines[2];
static int                g_scale_n[2];

/* 合成一份 N 个函数 + N 次调用的源码。用生成的而不是现成的大文件，
 * 是因为规模要能按整数倍放大 —— 曲线才看得见。 */
static int write_scale_src(const char *path, int n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "// 自动生成，只用来量前端耗时：%d 个函数 + %d 次调用\n", n, n);
    fprintf(f, "const std = module(\"std\");\n\n");
    for (int i = 0; i < n; i++)
        fprintf(f, "const f%d = fn (a: i32) i32 { return a * %d + %d; };\n", i, i + 1, i);
    fprintf(f, "\nconst main = fn () i32 {\n    var s: i32 = 0;\n");
    for (int i = 0; i < n; i++)
        fprintf(f, "    s = s + f%d(s);\n", i);
    fprintf(f, "    return s;\n};\n");
    fclose(f);
    return 0;
}

static long count_lines_file(const char *path) {
    Buf b = { 0 };
    if (!read_file(path, &b)) return 0;
    long n = 0;
    for (size_t i = 0; i < b.n; i++)
        if (b.p[i] == '\n') n++;
    buf_free(&b);
    return n;
}

/* 编一份源码 repeat 次取最快；*code 带回第一遍的退出码。 */
static unsigned long long measure_front(const char *zxc, const char *file,
                                        const char *log, int repeat, int *code) {
    char *av[3];
    av[0] = (char *)zxc;
    av[1] = (char *)file;
    av[2] = NULL;
    unsigned long long best = 0;
    int first = 0;
    for (int k = 0; k < repeat; k++) {
        RunRes r = run_to_file(av, log, g_timeout_ms * 6);
        if (k == 0) first = r.timed_out ? -2 : r.code;
        if (!r.timed_out && r.code == first && (best == 0 || r.ns < best)) best = r.ns;
    }
    if (code) *code = first;
    return best;
}

/* 四倍代码应该用四倍时间。超过 8 倍就是有超线性路径（哈希退化成链表、
 * 每个声明都全表扫一遍之类），这种问题在小用例上永远看不出来。 */
static void run_scale_check(const char *zxc, const char *tmp, int repeat) {
    /* 档位要选在"净耗时远大于进程启动"的地方，否则量到的是 CreateProcess。
     * 2000 / 8000 个函数大约 20 / 80 ms（去掉启动），曲线看得清。 */
    int n1 = 2000, n2 = 8000;
    char p1[4096], p2[4096], log[4096];
    snprintf(log, sizeof log, "%s.slog", tmp);

    unsigned long long t1 = 0, t2 = 0;
    long l1 = 0, l2 = 0;

    for (int attempt = 0; attempt < 4; attempt++) {
        snprintf(p1, sizeof p1, "%s.s%d.zx", tmp, attempt * 2);
        snprintf(p2, sizeof p2, "%s.s%d.zx", tmp, attempt * 2 + 1);
        if (write_scale_src(p1, n1) != 0 || write_scale_src(p2, n2) != 0) {
            printf("\n== 规模检查：写不了临时文件 %s，跳过 ==\n", p1);
            return;
        }
        int c1 = 0, c2 = 0;
        t1 = measure_front(zxc, p1, log, repeat, &c1);
        t2 = measure_front(zxc, p2, log, repeat, &c2);
        l1 = count_lines_file(p1);
        l2 = count_lines_file(p2);
        if (c1 != 0 || c2 != 0) {
            printf("\n== 规模检查：合成的源码编不过（exit %d/%d），跳过 ==\n", c1, c2);
            remove_retry(p1);
            remove_retry(p2);
            return;
        }
        /* 两边的"净耗时"（去掉进程启动）都太小就放大规模再量一次 */
        unsigned long long a2 = t2 > g_startup_ns ? t2 - g_startup_ns : 0;
        if (a2 >= 8000000ULL || attempt == 3) break;
        n1 *= 4;
        n2 *= 4;
        remove_retry(p1);
        remove_retry(p2);
    }

    unsigned long long a1 = t1 > g_startup_ns ? t1 - g_startup_ns : 0;
    unsigned long long a2 = t2 > g_startup_ns ? t2 - g_startup_ns : 0;

    printf("\n== 规模（前端随代码量怎么长）==\n");
    char s1[32], s2[32];
    fmt_ms(s1, sizeof s1, a1);
    fmt_ms(s2, sizeof s2, a2);
    printf("  %5d 函数  %6ld 行  净 %s  %.2f us/行\n", n1, l1, s1,
           l1 ? (double)a1 / 1000.0 / (double)l1 : 0.0);
    printf("  %5d 函数  %6ld 行  净 %s  %.2f us/行\n", n2, l2, s2,
           l2 ? (double)a2 / 1000.0 / (double)l2 : 0.0);
    double code_ratio = l1 ? (double)l2 / (double)l1 : 0.0;
    double time_ratio = a1 ? (double)a2 / (double)a1 : 0.0;
    if (a1 < 100000ULL) {
        printf("  净耗时 < 0.1 ms，量不出来 —— 前端太快，进程启动掩盖了一切\n");
    } else {
        bool super = time_ratio > 8.0;
        printf("  %.1f 倍代码 → %.1f 倍耗时（%s）\n", code_ratio, time_ratio,
               super ? "超线性，有地方在按平方长大" : "线性，没问题");
    }
    g_scale_ns[0] = a1; g_scale_ns[1] = a2;
    g_scale_lines[0] = l1; g_scale_lines[1] = l2;
    g_scale_n[0] = n1; g_scale_n[1] = n2;
    remove_retry(p1);
    remove_retry(p2);
    remove_retry(log);
}

/* ---------------------------------------------------------------- 主流程 */

static void report_ok(const char *path, unsigned long long ns) {
    if (g_quiet) return;
    char t[32];
    fmt_ms(t, sizeof t, ns);
    printf("  ok   %8s  %s\n", t, path);
    fflush(stdout);
}

int main(int argc, char **argv) {
    const char *zxc = NULL, *tmp = NULL;
    const char **files = (const char **)malloc(sizeof(char *) * (size_t)argc);
    int nfiles = 0;

    bool perf = true;
    int repeat = 3;
    const char *save = NULL, *baseline = NULL, *filter = NULL;
    double regress_pct = 25.0;
    bool fail_on_regress = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--perf")) perf = true;
        else if (!strcmp(a, "--no-perf")) perf = false;
        else if (!strcmp(a, "--quiet")) g_quiet = true;
        else if (!strcmp(a, "--fail-on-regress")) fail_on_regress = true;
        else if (!strcmp(a, "--repeat") && i + 1 < argc) repeat = atoi(argv[++i]);
        else if (!strcmp(a, "--timeout") && i + 1 < argc) g_timeout_ms = atoi(argv[++i]);
        else if (!strcmp(a, "--save") && i + 1 < argc) save = argv[++i];
        else if (!strcmp(a, "--baseline") && i + 1 < argc) baseline = argv[++i];
        else if (!strcmp(a, "--filter") && i + 1 < argc) filter = argv[++i];
        else if (!strncmp(a, "--fail-on-regress=", 18)) { fail_on_regress = true; regress_pct = atof(a + 18); }
        else if (!zxc) zxc = a;
        else if (!tmp) tmp = a;
        else files[nfiles++] = a;
    }
    if (!zxc || !tmp) {
        fprintf(stderr, "usage: runner [options] <zxc> <tmp-prefix> <file.zx>...\n");
        return 2;
    }
    if (repeat < 1) repeat = 1;

    /* stdout 一律行缓冲并处处显式刷 —— 旧版就栽在"报告只留在缓冲里"。
     * 这里连缓冲都不要：报告是给人看的，也是给 CI 抓的。 */
    setvbuf(stdout, NULL, _IOLBF, 0);

    char absbuf[4096];
    tmp = abs_prefix(tmp, absbuf, sizeof absbuf);
    zxc = abs_prefix(zxc, absbuf + 2048, sizeof absbuf - 2048);

    Buf logpath = { 0 }, outpath = { 0 };
    buf_puts(&logpath, tmp);
    buf_puts(&logpath, ".log");
    buf_puts(&outpath, tmp);
    buf_puts(&outpath, ".out");

    /* 进程启动的地板价：zxc 不带参数只会打 usage 就退，量的就是
     * "起一个进程"要多少钱。小文件的前端耗时几乎全是它。 */
    if (perf) {
        char *av0[2];
        av0[0] = (char *)zxc;
        av0[1] = NULL;
        for (int k = 0; k < 5; k++) {
            RunRes r0 = run_to_file(av0, logpath.p, g_timeout_ms);
            if (r0.code >= 0 && (g_startup_ns == 0 || r0.ns < g_startup_ns)) g_startup_ns = r0.ns;
        }
    }

    int pass = 0, fail = 0;

    for (int fi = 0; fi < nfiles; fi++) {
        const char *f = files[fi];
        if (filter && !contains(f, strlen(f), filter)) continue;

        Buf src = { 0 };
        if (!read_file(f, &src)) {
            printf("  FAIL %s — cannot read\n", f);
            fflush(stdout);
            fail++;
            continue;
        }
        int nlines = 0;
        Line *ls = split_lines(src.p, src.n, &nlines);

        bool is_err = has_dir(f, "tests/bad") || has_dir(f, "tests/multibad");
        CaseStat *st = new_stat();
        st->path = f;
        st->lines = nlines;
        st->bytes = (long)src.n;
        st->is_err = is_err;

        char binpath[4096];
        snprintf(binpath, sizeof binpath, "%s.p%ld.%d.exe", tmp, proc_id(), fi);

        char *av_check[4];
        av_check[0] = (char *)zxc;
        av_check[1] = (char *)f;
        av_check[2] = NULL;
        av_check[3] = NULL;

        /* ---- 前端（lex + parse + sema）：顺带就是"必须通过/必须失败"的判定 ---- */
        RunRes r = run_to_file(av_check, logpath.p, g_timeout_ms);
        st->front_ns = r.ns;
        if (r.timed_out) {
            printf("  FAIL %s -- 前端超过 %d ms，已杀\n", f, g_timeout_ms);
            fflush(stdout);
            fail++;
            free(ls);
            buf_free(&src);
            continue;
        }
        /* 其余几遍只为取更稳的计时：判定用第一遍的结果 */
        for (int k = 1; k < repeat; k++) {
            RunRes r2 = run_to_file(av_check, logpath.p, g_timeout_ms);
            if (!r2.timed_out && r2.code == r.code && r2.ns < st->front_ns) st->front_ns = r2.ns;
        }

        Buf log = { 0 };
        read_file(logpath.p, &log);

        if (is_err) {
            bool ok = true;
            if (r.code == 0) {
                printf("  FAIL %s — should be rejected, but compiled\n", f);
                fflush(stdout);
                ok = false;
            } else {
                size_t wl = 0;
                const char *want = directive(ls, nlines, "// expect: ", &wl);
                if (want) {
                    /* 期望文本按整行匹配：`// expect: ` 之后的部分可以带空格 */
                    char needle[512];
                    size_t n = wl < sizeof needle - 1 ? wl : sizeof needle - 1;
                    memcpy(needle, want, n);
                    needle[n] = 0;
                    if (!contains(log.p, log.n, needle)) {
                        printf("  FAIL %s -- stderr lacks \"%s\"\n", f, needle);
                        print_indented(log.p, log.n);
                        ok = false;
                    }
                }
                size_t cl = 0;
                const char *cs = directive(ls, nlines, "// expect-count: ", &cl);
                if (cs) {
                    long want_n = strtol(cs, NULL, 10);
                    long got = (long)count_errors(log.p, log.n);
                    if (got != want_n) {
                        printf("  FAIL %s -- %ld error(s) reported, want %ld\n", f, got, want_n);
                        print_indented(log.p, log.n);
                        ok = false;
                    }
                }
                size_t kl = 0;
                const char *codes = directive(ls, nlines, "// expect-code: ", &kl);
                if (codes) {
                    char list[256];
                    size_t n = kl < sizeof list - 1 ? kl : sizeof list - 1;
                    memcpy(list, codes, n);
                    list[n] = 0;
                    for (char *t = strtok(list, " "); t; t = strtok(NULL, " ")) {
                        if (!contains(log.p, log.n, t)) {
                            printf("  FAIL %s -- no diagnostic with code %s\n", f, t);
                            print_indented(log.p, log.n);
                            ok = false;
                        }
                    }
                }
            }
            if (ok) { pass++; report_ok(f, st->front_ns); }
            else fail++;
        } else if (r.code != 0) {
            printf("  FAIL %s -- type check failed\n", f);
            print_indented(log.p, log.n);
            fail++;
        } else {
            bool case_ok = true;
            pass++;                       /* 类型检查这一关过了 */
            bool checked = false;

            bool want_present = false;
            Buf want = { 0 };
            expect_output(ls, nlines, &want_present, &want);

            if (want_present && has_main(src.p, src.n)) {
                char *av_build[6];
                av_build[0] = (char *)zxc;
                av_build[1] = (char *)f;
                av_build[2] = (char *)"-o";
                av_build[3] = binpath;
                av_build[4] = NULL;
                av_build[5] = NULL;

                RunRes b = run_to_file(av_build, logpath.p, g_timeout_ms * 3);
                st->build_ns = b.ns;
                st->did_build = true;
                if (b.timed_out || b.code != 0) {
                    remove_retry(binpath);
                    printf("  FAIL %s -- %s\n", f,
                           b.timed_out ? "backend compile timed out" : "backend compile failed");
                    Buf blog = { 0 };
                    read_file(logpath.p, &blog);
                    print_indented(blog.p, blog.n);
                    buf_free(&blog);
                    case_ok = false;
                    fail++;
                } else {
                    /* 编出来的东西先跑，跑完再删 —— 删早了就是在测空气 */
                    char *av_run[2];
                    av_run[0] = binpath;
                    av_run[1] = NULL;
                    RunRes rr = run_to_file(av_run, outpath.p, g_timeout_ms);
                    st->run_ns = rr.ns;
                    st->did_run = true;
                    remove_retry(binpath);
                    checked = true;
                    if (rr.timed_out || rr.code < 0) {
                        printf("  FAIL %s -- %s\n", f,
                               rr.timed_out ? "运行超时，已杀" : "程序没能启动");
                        case_ok = false;
                        fail++;
                    } else {
                        Buf got = { 0 }, want_norm = { 0 };
                        Buf raw = { 0 };
                        read_file(outpath.p, &raw);
                        normalize(raw.p ? raw.p : "", raw.n, &got);
                        normalize(want.p ? want.p : "", want.n, &want_norm);
                        bool ok = str_eq(&got, &want_norm);
                        if (!ok) {
                            printf("  FAIL %s -- stdout mismatch\n", f);
                            printf("    want: %s\n", want_norm.p ? want_norm.p : "");
                            printf("    got:  %s\n", got.p ? got.p : "");
                        }
                        size_t el = 0;
                        const char *ex = directive(ls, nlines, "// expect-exit: ", &el);
                        if (ok && ex) {
                            long want_code = strtol(ex, NULL, 10);
                            if (want_code != (long)rr.code) {
                                printf("  FAIL %s -- exit %d, want %ld\n", f, rr.code, want_code);
                                ok = false;
                            }
                        }
                        if (ok) pass++;       /* 运行这一关也过了 */
                        else { fail++; case_ok = false; }
                        buf_free(&raw);
                        buf_free(&got);
                        buf_free(&want_norm);
                    }
                }
            }
            (void)checked;
            buf_free(&want);
            if (case_ok) report_ok(f, st->front_ns);
        }

        buf_free(&log);
        free(ls);
        buf_free(&src);
    }

    remove_retry(logpath.p);
    remove_retry(outpath.p);

    if (perf && g_ncases > 0) run_scale_check(zxc, tmp, repeat);

    /* ---- 性能小结 ---- */
    if (perf && g_ncases > 0) {
        unsigned long long *front = (unsigned long long *)malloc(sizeof(unsigned long long) * (size_t)g_ncases);
        unsigned long long *ferr = (unsigned long long *)malloc(sizeof(unsigned long long) * (size_t)g_ncases);
        unsigned long long *build = (unsigned long long *)malloc(sizeof(unsigned long long) * (size_t)g_ncases);
        unsigned long long *run = (unsigned long long *)malloc(sizeof(unsigned long long) * (size_t)g_ncases);
        int nf = 0, ne = 0, nb = 0, nr = 0;
        long total_lines = 0, total_bytes = 0;
        unsigned long long front_total = 0;
        for (int i = 0; i < g_ncases; i++) {
            CaseStat *c = &g_cases[i];
            if (c->front_ns) {
                if (c->is_err) {
                    ferr[ne++] = c->front_ns;           /* 反例还多付诊断格式化的钱 */
                } else {
                    front[nf++] = c->front_ns;
                    /* 净耗时：扣掉每项都要付的进程启动，剩下才是编译器干的活 */
                    front_total += c->front_ns > g_startup_ns ? c->front_ns - g_startup_ns : 0;
                    total_lines += c->lines;
                    total_bytes += c->bytes;
                }
            }
            if (c->did_build) build[nb++] = c->build_ns;
            if (c->did_run) run[nr++] = c->run_ns;
        }

        printf("\n== 性能（wall-clock，已扣掉进程启动）==\n");
        if (g_startup_ns) {
            char s[32];
            fmt_ms(s, sizeof s, g_startup_ns);
            printf("  %-18s %s（zxc 不带参数空跑，每项都已扣掉）\n", "启动下限", s);
        }
        print_bucket("前端 正例", front, nf, g_startup_ns);
        print_bucket("前端 反例（含诊断）", ferr, ne, g_startup_ns);
        if (total_lines > 0 && front_total > 0) {
            double klps = (double)total_lines / ((double)front_total / 1e9) / 1000.0;
            double us_per_line = (double)front_total / 1000.0 / (double)total_lines;
            printf("  %-18s %ld 行 / %.0f KB → %.1f k行/s（%.2f us/行）\n",
                   "吞吐", total_lines, (double)total_bytes / 1024.0, klps, us_per_line);
        }
        print_bucket("后端 + zig cc", build, nb, 0);
        print_bucket("运行程序", run, nr, 0);
        printf("  （单项噪声 ±5 ms 上下：判断快慢看总量、规模曲线和基线对比，别盯单条）\n");

        /* 最慢的前端：按耗时降序取前 5，并标出每行微秒 ——
         * 单看毫秒分不出"文件大"还是"这个文件特别慢"。 */
        int top = nf < 5 ? nf : 5;
        if (top > 0) {
            printf("  最慢前端（净）：\n");
            bool *used = (bool *)calloc((size_t)g_ncases, sizeof(bool));
            for (int k = 0; k < top; k++) {
                int best = -1;
                for (int i = 0; i < g_ncases; i++) {
                    if (used[i] || !g_cases[i].front_ns || g_cases[i].is_err) continue;
                    if (best < 0 || g_cases[i].front_ns > g_cases[best].front_ns) best = i;
                }
                if (best < 0) break;
                used[best] = true;
                CaseStat *c = &g_cases[best];
                unsigned long long net = c->front_ns > g_startup_ns ? c->front_ns - g_startup_ns : 0;
                char t[32];
                fmt_ms(t, sizeof t, net);
                double upl = c->lines > 0 ? (double)net / 1000.0 / (double)c->lines : 0.0;
                printf("    %9s  %5ld 行  %7.2f us/行  %s\n", t, c->lines, upl, c->path);
            }
            free(used);
        }

        /* 基线对比：只比规模曲线。
         * 单项耗时看着诱人，但进程启动的抖动（±5 ms）比一个测试文件的前端耗时
         * 本身还大——同一份代码连跑两次能"快 22%"。规模曲线是单进程、净耗时
         * 17/73 ms 起，噪声在 10% 以内，才拿得动 15% 这条线。 */
        if (baseline) {
            load_baseline(baseline);
            printf("\n== 与基线对比（规模曲线，%+.0f%% 为界）==\n", regress_pct);
            if (g_nbase == 0) {
                printf("  （基线里没有规模数据）\n");
            } else {
                double lim = 1.0 + regress_pct / 100.0;
                const unsigned long long NOISE = 3000000ULL;   /* 3 ms 以下不判 */
                int worse = 0;
                /* 只判大档：小档净耗时 ~25 ms，正好卡在启动噪声的边缘，
                 * 同一份代码能抖出 1.3x。大档 100 ms 起，稳定在 ±5% 内。 */
                for (int k = 1; k < 2; k++) {
                    unsigned long long b = base_lookup(k == 0 ? "scale/small" : "scale/big");
                    if (!b || !g_scale_ns[k]) continue;
                    unsigned long long now = g_scale_ns[k];
                    double ratio = (double)now / (double)b;
                    char sn[32], sb[32];
                    fmt_ms(sn, sizeof sn, now);
                    fmt_ms(sb, sizeof sb, b);
                    bool bad = now > b + NOISE && ratio > lim;
                    printf("  %5d 函数  现在 %s  基线 %s  %5.2fx%s\n",
                           g_scale_n[k], sn, sb, ratio, bad ? "  ← 回归" : "");
                    if (bad) worse++;
                }
                if (fail_on_regress && worse) {
                    printf("  规模回归超过 %.0f%%：判失败\n", regress_pct);
                    fail++;
                }
            }
        }

        free(front);
        free(ferr);
        free(build);
        free(run);
    }

    /* 基线落盘：只存规模曲线（见上面"为什么不是逐项"）。 */
    if (save) {
        FILE *fp = fopen(save, "wb");
        if (!fp) {
            printf("  ! 写不了基线 %s\n", save);
        } else {
            fprintf(fp, "# zx-runner-baseline 2\n");
            fprintf(fp, "# 列：名字<TAB>净耗时ns<TAB>行数<TAB>函数数\n");
            for (int k = 0; k < 2; k++) {
                if (!g_scale_ns[k]) continue;
                fprintf(fp, "%s\t%llu\t%ld\t%d\n",
                        k == 0 ? "scale/small" : "scale/big",
                        g_scale_ns[k], g_scale_lines[k], g_scale_n[k]);
            }
            fclose(fp);
            printf("\n  基线已写入 %s\n", save);
        }
    }

    printf("\npassed %d, failed %d\n", pass, fail);
    fflush(stdout);
    free(g_cases);
    free(files);
    return fail ? 1 : 0;
}
