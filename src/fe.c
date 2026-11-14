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
#ifdef _WIN32
static unsigned long g_fe_spawn_winerr = 0;  /* CreateProcess/CreateFile 失败原因 */
#endif

void fe_set_mode(bool use_zx) { g_fe_zx = use_zx; }
bool fe_is_zx(void) { return g_fe_zx; }
void fe_set_argv0(const char *p) { g_fe_argv0 = p; }

/* zxfe.exe 与 zxc.exe 同目录（bin/）。argv[0] 不带目录（裸 `zxc`、宿主
 * 程序用短名拉起——Zed 的 LSP 链就是这么调的）时，退回 GetModuleFileName
 * 拿真实 exe 路径。以前空串当"当前目录"用，.zxfe-*.ast 就落在调用方的
 * cwd 里：写完立刻 rename/remove，用户看到的是"src/ 里有个文件一闪而过"，
 * 还会触发文件监视器刷屏。 */
static char *fe_exe_path(void) {
    char buf[1024];
    const char *a = g_fe_argv0 ? g_fe_argv0 : "zxc";
    const char *slash = strrchr(a, '/');
    const char *bslash = strrchr(a, '\\');
    const char *cut = slash > bslash ? slash : bslash;
    size_t dirlen = cut ? (size_t)(cut - a) : 0;
    if (dirlen == 0) {
#ifdef _WIN32
        DWORD n = GetModuleFileNameA(NULL, buf, sizeof buf);
        if (n > 0 && n < sizeof buf) {
            char *s2 = strrchr(buf, '\\');
            if (s2) {
                *s2 = '\0';
                char *s = malloc(strlen(buf) + 1);
                if (s) strcpy(s, buf);
                return s;
            }
        }
#endif
        /* 实在拿不到 exe 路径（极端环境）：退回旧行为"当前目录"。
         * 这个函数还兼着定位 zxfe.exe，不能随便换成 TEMP。 */
        char *s = malloc(1);
        if (!s) return NULL;
        s[0] = '\0';
        return s;
    }
    char *s = malloc(dirlen + 1);
    if (!s) return NULL;
    memcpy(s, a, dirlen);
    s[dirlen] = '\0';
    return s;
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

/* zxfe 的诊断由它自己按同一格式打到 stderr，zxc 不重复打印。但导入方报
 * "module 'x' failed to parse" 时必须说得清原因——所以子进程的 stderr 先
 * 落临时文件，再原样回放（用户看到的内容一字不差），顺手记住第一条
 * `path:line:col: error[CODE]: msg`。回放而不是直连，纯粹是为了能读一遍。 */
static struct {
    char path[512], code[16], msg[256];
    int  line, col, has;
} g_fe_err;

void fe_last_error_reset(void) { g_fe_err.has = 0; }

bool fe_last_error(int *line, int *col, char *code, size_t ccap,
                   char *msg, size_t mcap) {
    if (!g_fe_err.has) return false;
    if (line) *line = g_fe_err.line;
    if (col)  *col  = g_fe_err.col;
    if (code && ccap) snprintf(code, ccap, "%s", g_fe_err.code);
    if (msg && mcap)  snprintf(msg, mcap, "%s", g_fe_err.msg);
    return true;
}

static void fe_note_error_line(const char *line) {
    if (g_fe_err.has) return;
    const char *m = strstr(line, ": error[");
    if (!m) return;
    const char *lb = m + 7;                  /* 指向 '[' */
    const char *rb = strchr(lb, ']');
    if (!rb) return;
    size_t cl = (size_t)(rb - lb - 1);
    if (cl == 0 || cl >= sizeof g_fe_err.code) return;
    memcpy(g_fe_err.code, lb + 1, cl);
    g_fe_err.code[cl] = 0;
    const char *msg = rb + 1;
    if (*msg == ':') msg++;
    while (*msg == ' ') msg++;
    snprintf(g_fe_err.msg, sizeof g_fe_err.msg, "%s", msg);
    /* `path:line:col` 全在 ": error[" 之前：末两段是数字，前面是路径 */
    char head[512];
    snprintf(head, sizeof head, "%.*s", (int)(size_t)(m - line), line);
    int ln = 0, cc = 0;
    char *c1 = strrchr(head, ':');
    if (c1) {
        cc = atoi(c1 + 1); *c1 = 0;
        char *c2 = strrchr(head, ':');
        if (c2) { ln = atoi(c2 + 1); *c2 = 0; }
    }
    snprintf(g_fe_err.path, sizeof g_fe_err.path, "%s", head);
    g_fe_err.line = ln;
    g_fe_err.col = cc;
    g_fe_err.has = 1;
}

/* 把子进程 stderr 文件回放到自己的 stderr，并记住第一条错误 */
/* 回放 zxfe 的 stderr，返回"看起来是诊断"的行数。返回 0 而 rc!=0 就说明
 * 前端是**无声失败**（栈爆、OOM、内部错误），由调用方补一条可定位的消息。 */
static int fe_replay_stderr(const char *logpath) {
    FILE *f = fopen(logpath, "rb");
    if (!f) return 0;
    static char buf[65536];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    if (!n) return 0;
    fwrite(buf, 1, n, stderr);
    fflush(stderr);
    int nlines = 0;
    char *p = buf;
    while (p && *p) {
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        if (strstr(p, ": error[")) nlines++;
        fe_note_error_line(p);
        p = e ? e + 1 : NULL;
    }
    return nlines;
}

/* zxfe 的启动：不再借 cmd.exe 的 `2>` 重定向。
 *
 * 以前是 system("zxfe.exe ... 2> <log>")，两层 cmd（LSP 拉 zxc 一层、zxc
 * 拉 zxfe 一层）。宿主环境一旦限制命令解释器创建文件，重定向就失败，
 * 子进程压根没跑起来，rc=1、日志为空——上层只看到一句
 * "the Zx frontend failed without a diagnostic ... this is a compiler bug"，
 * 编辑器里连正常的 `@mod("std")` 都会被标红说编译器崩了。
 * 直接 CreateProcess + 把 stderr 句柄指到日志文件：不需要 shell，
 * 少一层进程，也少一类"其实不是编译器问题"的假崩溃。
 */
#ifdef _WIN32
static HANDLE fe_std_or_nul(DWORD which, bool *owned) {
    *owned = false;
    HANDLE h = GetStdHandle(which);
    if (h && h != INVALID_HANDLE_VALUE) return h;
    SECURITY_ATTRIBUTES sa;
    ZeroMemory(&sa, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, NULL);
    if (nul != INVALID_HANDLE_VALUE) *owned = true;
    return nul;
}

/* 返回子进程退出码；-1 表示根本没能启动（附 GetLastError 给上层） */
static int fe_spawn_zxfe(const char *cmdline, const char *logp) {
    SECURITY_ATTRIBUTES sa;
    ZeroMemory(&sa, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE hlog = CreateFileA(logp, GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hlog == INVALID_HANDLE_VALUE) {
        g_fe_spawn_winerr = (unsigned long)GetLastError();
        return -1;
    }

    bool in_owned = false, out_owned = false;
    HANDLE hin  = fe_std_or_nul(STD_INPUT_HANDLE, &in_owned);
    HANDLE hout = fe_std_or_nul(STD_OUTPUT_HANDLE, &out_owned);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    ZeroMemory(&pi, sizeof pi);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = hin;
    si.hStdOutput = hout;
    si.hStdError  = hlog;

    /* CreateProcessA 会改写 lpCommandLine，不能传只读字面量 */
    char *buf = (char *)malloc(strlen(cmdline) + 1);
    if (!buf) { CloseHandle(hlog); return -1; }
    strcpy(buf, cmdline);
    BOOL ok = CreateProcessA(NULL, buf, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    if (!ok) g_fe_spawn_winerr = (unsigned long)GetLastError();
    free(buf);

    CloseHandle(hlog);  /* 子进程已有自己的副本 */
    if (in_owned && hin && hin != INVALID_HANDLE_VALUE) CloseHandle(hin);
    if (out_owned && hout && hout != INVALID_HANDLE_VALUE) CloseHandle(hout);
    if (!ok) return -1;
    if (pi.hThread) CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}
#else
/* POSIX 仍走 shell 重定向（那里没有 cmd.exe 的句柄继承坑） */
static int fe_spawn_zxfe(const char *cmdline, const char *logp) {
    char full[3600];
    snprintf(full, sizeof full, "%s 2> \"%s\"", cmdline, logp);
    return system(full);
}
#endif

static Node *fe_parse_zx(const char *path) {
    fe_last_error_reset();
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
    char logp[1300];
    snprintf(logp, sizeof logp, "%s.log", tmp);
#ifdef _WIN32
    const char *sep = "\\";
#else
    const char *sep = "/";
#endif
    char exe[1400];
    if (*dir) snprintf(exe, sizeof exe, "%s%szxfe.exe", dir, sep);
    else snprintf(exe, sizeof exe, "zxfe.exe");
    /* CreateProcess 自己按首 token 拆程序名，引号不会被 shell 吃掉；
     * 这里不再写 `2> ...`，stderr 由 fe_spawn_zxfe 直接接到日志文件。 */
    char cmd[2800];
    snprintf(cmd, sizeof cmd, "\"%s\" \"%s\" -o \"%s\"", exe, path, tmp);
    free(dir);

    int rc = fe_spawn_zxfe(cmd, logp);
    int ndiag = fe_replay_stderr(logp);   /* 诊断原样回放，顺带记下第一条 */
    remove(logp);
    if (rc != 0) {
        /* 前端挂了却一条诊断都没有（栈爆掉、OOM、内部断言）——以前这里直接
         * return NULL，上层只看到"检查失败、零条消息"。补一条能定位的话。 */
        if (ndiag == 0) {
            if (rc < 0) {
#ifdef _WIN32
                err_at(1, 1, "cannot launch the Zx frontend (zxfe): win32 error %lu -- "
                             "check that zxfe.exe sits next to zxc.exe and that its "
                             "directory is writable", g_fe_spawn_winerr);
#else
                err_at(1, 1, "cannot launch the Zx frontend (zxfe): check the executable "
                             "next to zxc and the directory permissions");
#endif
            }
            else if (rc == 139 || rc == 134)
                err_at(1, 1, "the Zx frontend (zxfe) crashed while parsing this file "
                             "(signal %d) -- this is a compiler bug", rc - 128);
            else
                err_at(1, 1, "the Zx frontend (zxfe) failed without a diagnostic "
                             "(exit %d) -- check zxfe.exe next to zxc.exe, or use --fe=c", rc);
        }
        return NULL;
    }

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
