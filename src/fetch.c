/* fetch.c — C 库远端获取：两层声明/锁 + 安全落地。
 *
 * 设计要点（"简洁，但别裸奔"）：
 *  - 不自己写下载器：curl / git / tar 走 argv 数组起进程，URL 不经过 shell，
 *    没有命令注入面；校验用自带的 SHA-256，不依赖 PATH 里的 sha256sum。
 *  - 只允许 https://（外加 loopback http:// 和显式开关的 file:// 供离线
 *    镜像/测试）。拒绝 userinfo、控制字符、反斜杠等。
 *  - 下载先落在 .zxc-cache/fetch/<name>/stage，全部校验通过后才搬进 vendor/。
 *    锁里的 sha256 锚住"下载到的字节"、tree hash 锚住"落地的文件树"、
 *    resolved commit 锚住"tag 背后真正的那次提交"；zx.json 只留 URL。
 *  - 解压/克隆可能带恶意路径：落地时只按规范化过的相对路径逐个复制普通
 *    文件，遇到 ".."、绝对路径、符号链接一律拒绝；文件数与体积有上限。
 *  - 不执行库里的任何构建脚本，不解析它的配置，不联网解析依赖。
 */
#include "fetch.h"
#include "zxc.h"
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#endif

#define FPATH       1024
#define MAX_FILES   4000
#define MAX_ONE     (64u * 1024u * 1024u)
#define MAX_TOTAL   (256u * 1024u * 1024u)
#define FETCH_BACKUP ".zxc-cache/fetch-backup"

/* ---------------- 通用小工具 ---------------- */

static int fexists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static int is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && (st.st_mode & S_IFDIR) != 0;
}
static void slash(char *s) { for (char *p = s; *p; p++) if (*p == '\\') *p = '/'; }
static const char *base_of(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) if (*q == '/' || *q == '\\') b = q + 1;
    return b;
}
static char *dupstr(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
static int eqs(const char *a, const char *b) { return strcmp(a, b) == 0; }
static void trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t'))
        s[--n] = 0;
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

static int mkdir_p(const char *path) {
    char tmp[FPATH];
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
    return _mkdir(tmp) == 0 || errno == EEXIST ? 0 : -1;
#else
    return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
#endif
}

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *p = malloc((size_t)n + 1);
    if (!p) { fclose(f); return NULL; }
    size_t got = fread(p, 1, (size_t)n, f);
    fclose(f);
    p[got] = 0;
    if (len) *len = got;
    return p;
}

static int put_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = strlen(text);
    int ok = fwrite(text, 1, n, f) == n;
    fclose(f);
    return ok ? 0 : -1;
}

static int copy_file(const char *src, const char *dst) {
    FILE *a = fopen(src, "rb");
    if (!a) return -1;
    FILE *b = fopen(dst, "wb");
    if (!b) { fclose(a); return -1; }
    char buf[65536];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof buf, a)) > 0)
        if (fwrite(buf, 1, n, b) != n) { ok = 0; break; }
    fclose(a); fclose(b);
    return ok ? 0 : -1;
}

typedef struct { char name[256]; int isdir; } FEntry;

static FEntry *list_entries(const char *dir, int *nout) {
    *nout = 0;
    FEntry *v = NULL;
    int n = 0, cap = 0;
#ifdef _WIN32
    char pat[FPATH];
    snprintf(pat, sizeof pat, "%s/*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            v = realloc(v, (size_t)cap * sizeof *v);
            if (!v) break;
        }
        snprintf(v[n].name, sizeof v[n].name, "%s", fd.cFileName);
        v[n].isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return NULL;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char full[FPATH];
        snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            v = realloc(v, (size_t)cap * sizeof *v);
            if (!v) break;
        }
        snprintf(v[n].name, sizeof v[n].name, "%s", de->d_name);
        v[n].isdir = S_ISDIR(st.st_mode) != 0;
        n++;
    }
    closedir(d);
#endif
    *nout = n;
    if (!v) { v = malloc(sizeof *v); }   /* 空目录：返回可 free 的非 NULL，n = 0 */
    return v;
}

/* 删内部临时目录专用；路径全部由本文件生成，不接受外部输入。 */
static void rm_rf(const char *path) {
    if (is_dir(path)) {
        int n = 0;
        FEntry *es = list_entries(path, &n);
        for (int i = 0; i < n; i++) {
            char sub[FPATH];
            snprintf(sub, sizeof sub, "%s/%s", path, es[i].name);
            if (es[i].isdir) rm_rf(sub);
            else {
#ifdef _WIN32
                _chmod(sub, _S_IWRITE);      /* git 的 objects 是只读的 */
#endif
                remove(sub);
            }
        }
        free(es);
#ifdef _WIN32
        _rmdir(path);
#else
        rmdir(path);
#endif
    } else {
        remove(path);
    }
}

/* ---------------- SHA-256（自带，不依赖外部工具） ---------------- */

typedef struct {
    uint32_t h[8];
    uint64_t total;
    unsigned char buf[64];
    size_t used;
} Sha;

static const uint32_t SK[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};
#define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_init(Sha *c) {
    static const uint32_t iv[8] = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
    };
    memcpy(c->h, iv, sizeof iv);
    c->total = 0; c->used = 0;
}
static void sha_block(Sha *c, const unsigned char *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15],7) ^ ROR(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2],17) ^ ROR(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=c->h[0],b=c->h[1],cc=c->h[2],d=c->h[3];
    uint32_t e=c->h[4],f=c->h[5],g=c->h[6],h=c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e,6) ^ ROR(e,11) ^ ROR(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + SK[i] + w[i];
        uint32_t S0 = ROR(a,2) ^ ROR(a,13) ^ ROR(a,22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d;
    c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}
static void sha_upd(Sha *c, const void *data, size_t n) {
    const unsigned char *p = data;
    c->total += n;
    while (n) {
        size_t take = 64 - c->used;
        if (take > n) take = n;
        memcpy(c->buf + c->used, p, take);
        c->used += take; p += take; n -= take;
        if (c->used == 64) { sha_block(c, c->buf); c->used = 0; }
    }
}
static void sha_fin(Sha *c, char hex[65]) {
    uint64_t bits = c->total * 8;
    unsigned char pad = 0x80;
    sha_upd(c, &pad, 1);
    unsigned char zero = 0;
    while (c->used != 56) sha_upd(c, &zero, 1);
    unsigned char lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (unsigned char)(bits >> (56 - i*8));
    sha_upd(c, lenb, 8);
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 4; j++) {
            unsigned char x = (unsigned char)(c->h[i] >> (24 - j*8));
            hex[i*8+j*2] = H[x >> 4]; hex[i*8+j*2+1] = H[x & 15];
        }
    hex[64] = 0;
}
static int sha_file(const char *path, char hex[65]) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    Sha c; sha_init(&c);
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) sha_upd(&c, buf, n);
    int bad = ferror(f) != 0;
    fclose(f);
    if (bad) return -1;
    sha_fin(&c, hex);
    return 0;
}
static void sha_bytes(const char *s, size_t n, char hex[65]) {
    Sha c; sha_init(&c); sha_upd(&c, s, n); sha_fin(&c, hex);
}
static int is_hex64(const char *s) {
    if (strlen(s) != 64) return 0;
    for (int i = 0; i < 64; i++)
        if (!isxdigit((unsigned char)s[i])) return 0;
    return 1;
}

/* ---------------- 子进程：argv 数组，不经 shell ---------------- */

static int run_argv(char **av) {
#ifdef _WIN32
    fflush(NULL);
    int rc = (int)_spawnvp(_P_WAIT, av[0], (const char *const *)av);
    return rc < 0 ? -1 : rc;
#else
    fflush(NULL);
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) { execvp(av[0], av); _exit(127); }
    int st = 0;
    if (waitpid(p, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif
}

/* 只用于固定格式的 `git rev-parse`：路径是本文件生成的相对路径。 */
static int capture(const char *cmd, char *out, size_t cap) {
#ifdef _WIN32
    FILE *p = _popen(cmd, "r");
#else
    FILE *p = popen(cmd, "r");
#endif
    if (!p) return -1;
    size_t n = fread(out, 1, cap - 1, p);
    out[n] = 0;
#ifdef _WIN32
    _pclose(p);
#else
    pclose(p);
#endif
    trim(out);
    return 0;
}

/* ---------------- URL / 源类型 ---------------- */

int fetch_url_check(const char *url, char *err, size_t cap) {
    if (!url || !*url || strlen(url) > 900) {
        snprintf(err, cap, "empty or overlong URL"); return -1;
    }
    int https = strncmp(url, "https://", 8) == 0;
    int loop  = strncmp(url, "http://127.0.0.1:", 17) == 0 ||
                strncmp(url, "http://localhost:", 17) == 0;
    int local = strncmp(url, "file://", 7) == 0 &&
                getenv("ZX_ALLOW_LOCAL_FETCH") != NULL;
    if (!https && !loop && !local) {
        snprintf(err, cap, "only https:// is allowed (loopback http:// is allowed for local mirrors;"
                           " file:// needs ZX_ALLOW_LOCAL_FETCH=1)");
        return -1;
    }
    /* 拒绝 shell/路径元字符与 userinfo，即使走 argv 数组也不给可乘之机 */
    for (const char *p = url; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x21 || c > 0x7e || c == '\\' || c == '"' || c == '`' ||
            c == '|' || c == '<' || c == '>' || c == '$' || c == '^' ||
            c == '{' || c == '}' || c == '\'' || c == '@') {
            snprintf(err, cap, "URL contains a forbidden character"); return -1;
        }
    }
    if (local) return 0;                 /* file:// 没有 host 段 */
    const char *host = strstr(url, "://") + 3;
    const char *end = host;
    while (*end && *end != '/' && *end != '?' && *end != '#') end++;
    if (end == host) { snprintf(err, cap, "URL has no host"); return -1; }
    for (const char *p = host; p < end; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '.' || c == '-' || c == ':' || c == '_')) {
            snprintf(err, cap, "URL host contains a forbidden character"); return -1;
        }
    }
    return 0;
}

static const char *url_tail(const char *url, char *out, size_t cap) {
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *slashp = strchr(p, '/');
    const char *base = slashp ? slashp + 1 : p;
    while (*base == '/') base++;
    const char *q = base;
    const char *last = base;
    while (*q) { if (*q == '/') last = q + 1; q++; }
    snprintf(out, cap, "%s", last);
    char *cut = strpbrk(out, "?#");
    if (cut) *cut = 0;
    return out;
}

int fetch_kind_of(const char *url, const char *ref, int force_git,
                  FetchKind *k, char *err, size_t cap) {
    (void)err; (void)cap;
    char tail[512];
    url_tail(url, tail, sizeof tail);
    size_t l = strlen(tail);
    static const char *arcs[] = {
        ".tar.gz", ".tar.bz2", ".tar.xz", ".tgz", ".tbz2", ".txz",
        ".tar", ".zip", NULL
    };
    int arc = 0;
    for (int i = 0; arcs[i]; i++) {
        size_t al = strlen(arcs[i]);
        if (l >= al && strcmp(tail + l - al, arcs[i]) == 0) { arc = 1; break; }
    }
    if (force_git || (ref && *ref) || (l >= 4 && strcmp(tail + l - 4, ".git") == 0))
        *k = FK_GIT;
    else if (arc)
        *k = FK_ARCHIVE;
    else
        *k = FK_FILE;
    return 0;
}

static void sanitize_name(char *s) {
    for (char *p = s; *p; p++)
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-')) *p = '_';
    if (!*s) snprintf(s, 128, "lib");
}

int fetch_name_of(const char *url, char *out, size_t cap) {
    char tail[512];
    url_tail(url, tail, sizeof tail);
    /* 归档的双扩展名要一起去掉 */
    static const char *db[] = { ".tar.gz", ".tar.bz2", ".tar.xz", NULL };
    for (int i = 0; db[i]; i++) {
        size_t al = strlen(db[i]), l = strlen(tail);
        if (l > al && strcmp(tail + l - al, db[i]) == 0) { tail[l-al] = 0; break; }
    }
    char *dot = strrchr(tail, '.');
    if (dot && dot > tail) *dot = 0;
    /* GitHub 归档/分支常见尾巴 */
    static const char *suf[] = { "-master", "-main", NULL };
    for (int i = 0; suf[i]; i++) {
        size_t sl = strlen(suf[i]), l = strlen(tail);
        if (l > sl && strcmp(tail + l - sl, suf[i]) == 0) { tail[l-sl] = 0; break; }
    }
    snprintf(out, cap, "%s", tail);
    sanitize_name(out);
    return 0;
}

/* ---------------- zx.json 声明层 ---------------- */

/* zx.json 的 "vendor" 只放 URL 字符串数组，和 "deps" 同构：
 *
 *   "vendor": [ "https://.../stb_image.h", "https://.../foo.git" ]
 *
 * 没有兼容层：旧版「每库一个对象」的写法直接报错、要求按新格式改写。
 * 人只写「从哪拿」；sha256 / git commit / tree hash / 落地路径全部由 zxc
 * 在首次 add 时算好写进 zx.lock，fetch 复现这些锚点即可，安全不打折。
 * 库名由 fetch_name_of(url) 唯一推导（URL 尾段去扩展名），同时是 vendor/
 * 落地名、zx.lock 的 [名]、以及 `zxc fetch <name>` 里的 name——不是地址。
 */

/* 从 from 处的 '[' 找配对 ']'，返回 [from, *end)。 */
static int arr_span(const char *t, size_t from, size_t *end) {
    int depth = 0, instr = 0, esc = 0;
    for (size_t i = from; t[i]; i++) {
        char c = t[i];
        if (instr) {
            if (esc) esc = 0;
            else if (c == '\\') esc = 1;
            else if (c == '"') instr = 0;
            continue;
        }
        if (c == '"') { instr = 1; continue; }
        if (c == '[') depth++;
        else if (c == ']' && --depth == 0) { *end = i + 1; return 0; }
    }
    return -1;
}

/* 读一个 JSON 字符串字面量，返回右引号之后的位置。 */
static size_t json_string(const char *t, size_t at, char *out, size_t cap) {
    if (t[at] != '"') { out[0] = 0; return at; }
    at++;
    size_t i = 0;
    while (t[at] && t[at] != '"') {
        if (t[at] == '\\' && t[at + 1]) at++;
        if (i + 1 < cap) out[i++] = t[at];
        at++;
    }
    out[i] = 0;
    return t[at] ? at + 1 : at;
}

/* "vendor" 值的位置。只认数组；返回 1 = 还是旧版对象写法。 */
static int vendor_span(char *t, size_t *vs, size_t *ve) {
    *vs = *ve = 0;
    char *p = strstr(t, "\"vendor\"");
    if (!p) return 0;
    char *c = strchr(p + 8, ':');
    if (!c) return 0;
    p = c + 1;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '{') return 1;
    if (*p != '[') return 0;
    size_t end;
    if (arr_span(t, (size_t)(p - t), &end) != 0) return 0;
    *vs = (size_t)(p - t); *ve = end;
    return 0;
}

static const char *LEGACY_MSG =
    "zx.json: \"vendor\" must be a URL list -- "
    "rewrite it as \"vendor\": [\"https://...\", ...]";

static void decl_from_url(FetchDecl *d, const char *url) {
    memset(d, 0, sizeof *d);
    snprintf(d->url, sizeof d->url, "%s", url);
    fetch_name_of(url, d->name, sizeof d->name);
    fetch_kind_of(url, NULL, 0, &d->kind, NULL, 0);
}

static int parse_decls(const char *block, FetchDecl **out, int *nout) {
    FetchDecl *v = NULL;
    int n = 0, cap = 0;
    size_t len = strlen(block);
    for (size_t i = 0; i < len; ) {
        if (block[i] != '"') { i++; continue; }
        char url[1024];
        i = json_string(block, i, url, sizeof url);
        if (!url[0]) continue;
        if (n == cap) { cap = cap ? cap * 2 : 8; v = realloc(v, (size_t)cap * sizeof *v); }
        decl_from_url(&v[n], url);
        n++;
    }
    *out = v; *nout = n;
    return n;
}

int fetch_decls_load(FetchDecl **out, char *err, size_t cap) {
    *out = NULL;
    if (err && cap) err[0] = 0;
    size_t len = 0;
    char *t = slurp("zx.json", &len);
    if (!t) return 0;                       /* 没有工程清单 = 没有声明 */
    size_t vs, ve;
    int legacy = vendor_span(t, &vs, &ve);
    if (legacy) {
        if (err && cap) snprintf(err, cap, "%s", LEGACY_MSG);
        free(t);
        return -1;
    }
    int n = 0;
    if (ve > vs) {
        char *block = malloc(ve - vs + 1);
        memcpy(block, t + vs, ve - vs);
        block[ve - vs] = 0;
        parse_decls(block, out, &n);
        free(block);
    }
    free(t);
    return n;
}

void fetch_decls_free(FetchDecl *v, int n) { (void)n; free(v); }

static void json_escape(const char *s, char *out, size_t cap) {
    size_t i = 0;
    for (const char *p = s; *p && i + 2 < cap; p++) {
        if (*p == '"' || *p == '\\') out[i++] = '\\';
        out[i++] = *p;
    }
    out[i] = 0;
}

/* 只往 zx.json 里写 URL；安全参数全在 zx.lock，声明层不留任何可选字段。 */
int fetch_decl_save(const char *url, char *err, size_t cap) {
    size_t len = 0;
    char *t = slurp("zx.json", &len);
    if (!t) {
        snprintf(err, cap, "zx.json not found (run `zxc init` first)");
        return -1;
    }
    size_t vs, ve;
    if (vendor_span(t, &vs, &ve)) {
        snprintf(err, cap, "%s", LEGACY_MSG);
        free(t);
        return -1;
    }
    int n = 0;
    FetchDecl *old = NULL;
    if (ve > vs) {
        char *block = malloc(ve - vs + 1);
        memcpy(block, t + vs, ve - vs);
        block[ve - vs] = 0;
        parse_decls(block, &old, &n);
        free(block);
    }

    Buf nb; buf_init(&nb);
    buf_puts(&nb, "[");
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(old[i].url, url)) continue;
        char u[1100];
        json_escape(old[i].url, u, sizeof u);
        buf_printf(&nb, "%s\n    \"%s\"", m++ ? "," : "", u);
    }
    char u[1100];
    json_escape(url, u, sizeof u);
    buf_printf(&nb, "%s\n    \"%s\"", m++ ? "," : "", u);
    if (m) buf_puts(&nb, "\n  ");
    buf_puts(&nb, "]");
    fetch_decls_free(old, n);

    Buf out; buf_init(&out);
    if (ve > vs) {
        buf_putn(&out, t, vs);
        buf_puts(&out, nb.data ? nb.data : "[]");
        buf_puts(&out, t + ve);
    } else {
        /* 没有 vendor 段：插在最后一个 '}' 前，视前面有无逗号决定前缀 */
        size_t last = len;
        while (last > 0 && isspace((unsigned char)t[last-1])) last--;
        if (last == 0 || t[last-1] != '}') {
            snprintf(err, cap, "zx.json is not a JSON object");
            buf_free(&nb); buf_free(&out); free(t);
            return -1;
        }
        size_t at = last - 1;
        size_t k = at;
        while (k > 0 && isspace((unsigned char)t[k-1])) k--;
        int has_comma = (k > 0 && t[k-1] == ',');
        size_t open = 0;
        while (open < at && t[open] != '{') open++;
        int empty = 1;
        for (size_t q = open + 1; q < at; q++)
            if (!isspace((unsigned char)t[q]) && t[q] != ',') { empty = 0; break; }
        buf_putn(&out, t, k);
        if (!has_comma && !empty) buf_puts(&out, ",");
        buf_puts(&out, "\n  \"vendor\": ");
        buf_puts(&out, nb.data ? nb.data : "[]");
        buf_puts(&out, "\n");
        buf_puts(&out, t + at);
    }
    int rc = put_text("zx.json", out.data ? out.data : "");
    if (rc != 0) snprintf(err, cap, "cannot write zx.json");
    buf_free(&nb); buf_free(&out); free(t);
    return rc;
}

/* ---------------- zx.lock 锁定层 ---------------- */

static const char *kind_str(FetchKind k) {
    return k == FK_GIT ? "git" : (k == FK_ARCHIVE ? "archive" : "file");
}
static FetchKind kind_val(const char *s) {
    if (eqs(s, "git")) return FK_GIT;
    if (eqs(s, "archive")) return FK_ARCHIVE;
    return FK_FILE;
}

int fetch_lock_load(FetchLock **out, char *err, size_t cap) {
    *out = NULL;
    if (err && cap) err[0] = 0;
    size_t len = 0;
    char *t = slurp(ZX_LOCK_PATH, &len);
    if (!t) return 0;
    FetchLock *v = NULL;
    int n = 0, vcap = 0, cur = -1;
    char line[2048];
    size_t i = 0;
    while (i < len) {
        size_t k = 0;
        while (i < len && t[i] != '\n' && k + 1 < sizeof line) line[k++] = t[i++];
        while (i < len && t[i] != '\n') i++;
        if (i < len) i++;
        line[k] = 0;
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        if (line[0] == '[') {
            char *e = strchr(line, ']');
            if (!e) continue;
            *e = 0;
            if (n == vcap) { vcap = vcap ? vcap*2 : 8; v = realloc(v, (size_t)vcap * sizeof *v); }
            cur = n++;
            memset(&v[cur], 0, sizeof v[cur]);
            snprintf(v[cur].d.name, sizeof v[cur].d.name, "%s", line + 1);
            continue;
        }
        if (cur < 0) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        trim(key); trim(val);
        FetchLock *l = &v[cur];
        if      (eqs(key,"kind"))     l->d.kind = kind_val(val);
        else if (eqs(key,"url"))      snprintf(l->d.url, sizeof l->d.url, "%s", val);
        else if (eqs(key,"ref"))      snprintf(l->d.ref, sizeof l->d.ref, "%s", val);
        else if (eqs(key,"sha256"))   snprintf(l->d.sha256, sizeof l->d.sha256, "%s", val);
        else if (eqs(key,"subdir"))   snprintf(l->d.subdir, sizeof l->d.subdir, "%s", val);
        else if (eqs(key,"resolved")) snprintf(l->resolved, sizeof l->resolved, "%s", val);
        else if (eqs(key,"tree"))     snprintf(l->tree, sizeof l->tree, "%s", val);
        else if (eqs(key,"path"))     snprintf(l->path, sizeof l->path, "%s", val);
        else if (eqs(key,"nfiles"))   l->nfiles = atoi(val);
    }
    free(t);
    *out = v;
    return n;
}

void fetch_lock_free(FetchLock *v, int n) { (void)n; free(v); }

int fetch_lock_save(const FetchLock *v, int n, char *err, size_t cap) {
    Buf b; buf_init(&b);
    buf_puts(&b, "# zx.lock -- generated by `zxc add` / `zxc fetch`. Commit this file.\n"
                 "# It pins what was actually downloaded; `zxc fetch` verifies vendor/ against it.\n\n");
    for (int i = 0; i < n; i++) {
        const FetchLock *l = &v[i];
        buf_printf(&b, "[%s]\n", l->d.name);
        buf_printf(&b, "kind = %s\n", kind_str(l->d.kind));
        buf_printf(&b, "url = %s\n", l->d.url);
        if (l->d.ref[0])     buf_printf(&b, "ref = %s\n", l->d.ref);
        if (l->d.sha256[0])  buf_printf(&b, "sha256 = %s\n", l->d.sha256);
        if (l->d.subdir[0])  buf_printf(&b, "subdir = %s\n", l->d.subdir);
        buf_printf(&b, "resolved = %s\n", l->resolved[0] ? l->resolved : "-");
        buf_printf(&b, "tree = %s\n", l->tree);
        buf_printf(&b, "path = %s\n", l->path);
        buf_printf(&b, "nfiles = %d\n\n", l->nfiles);
    }
    int rc = put_text(ZX_LOCK_PATH, b.data ? b.data : "");
    if (rc != 0) snprintf(err, cap, "cannot write " ZX_LOCK_PATH);
    buf_free(&b);
    return rc;
}

/* ---------------- 文件树：收集 / 复制 / 哈希 ---------------- */

static int push_str(char ***v, int *n, int *cap, const char *s) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        char **nv = realloc(*v, (size_t)*cap * sizeof *nv);
        if (!nv) return -1;
        *v = nv;
    }
    (*v)[(*n)++] = dupstr(s);
    return 0;
}

static int path_safe(const char *rel) {
    if (!rel[0] || rel[0] == '/' || strstr(rel, "..")) return 0;
    for (const char *p = rel; *p; p++)
        if ((unsigned char)*p < 0x20) return 0;
    return 1;
}

static int collect_tree(const char *root, const char *rel, char ***v,
                        int *n, int *cap, char *err, size_t ecap) {
    char full[FPATH];
    if (rel[0]) snprintf(full, sizeof full, "%s/%s", root, rel);
    else snprintf(full, sizeof full, "%s", root);
    int ne = 0;
    FEntry *es = list_entries(full, &ne);
    if (!es) return 0;
    for (int i = 0; i < ne; i++) {
        if (!strcmp(es[i].name, ".git")) continue;
        char r2[FPATH];
        if (rel[0]) snprintf(r2, sizeof r2, "%s/%s", rel, es[i].name);
        else snprintf(r2, sizeof r2, "%s", es[i].name);
        if (!path_safe(r2)) {
            snprintf(err, ecap, "unsafe path in downloaded tree: %s", r2);
            free(es); return -1;
        }
        if (es[i].isdir) {
            if (collect_tree(root, r2, v, n, cap, err, ecap) != 0) { free(es); return -1; }
        } else {
            if (*n >= MAX_FILES) {
                snprintf(err, ecap, "too many files in downloaded tree (limit %d)", MAX_FILES);
                free(es); return -1;
            }
            push_str(v, n, cap, r2);
        }
    }
    free(es);
    return 0;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* 对「base 下 rels[] 这些文件」算稳定的树 hash：
 * sha256( 排序后的 "<rel> <file-sha256>\n" 拼接 )。 */
static int tree_hash(const char *base, char **rels, int n,
                     char out[65], char *err, size_t ecap) {
    qsort(rels, (size_t)n, sizeof *rels, cmp_str);
    Buf b; buf_init(&b);
    unsigned long long total = 0;
    for (int i = 0; i < n; i++) {
        char full[FPATH], h[65];
        snprintf(full, sizeof full, "%s/%s", base, rels[i]);
        struct stat st;
        if (stat(full, &st) != 0 || sha_file(full, h) != 0) {
            snprintf(err, ecap, "cannot hash %s", rels[i]);
            buf_free(&b); return -1;
        }
        if ((unsigned long long)st.st_size > MAX_ONE) {
            snprintf(err, ecap, "file too large: %s", rels[i]);
            buf_free(&b); return -1;
        }
        total += (unsigned long long)st.st_size;
        if (total > MAX_TOTAL) {
            snprintf(err, ecap, "downloaded tree too large");
            buf_free(&b); return -1;
        }
        buf_printf(&b, "%s %s\n", rels[i], h);
    }
    sha_bytes(b.data ? b.data : "", b.len, out);
    buf_free(&b);
    return 0;
}

static int tree_at(const char *path, int is_file, char out[65], int *nout,
                   char *err, size_t ecap) {
    char **rels = NULL;
    int n = 0, cap = 0;
    char base[FPATH];
    if (is_file) {
        const char *b = base_of(path);
        size_t l = (size_t)(b - path);
        if (l == 0) snprintf(base, sizeof base, ".");
        else { memcpy(base, path, l - 1); base[l-1] = 0; }
        if (push_str(&rels, &n, &cap, b) != 0) return -1;
    } else {
        snprintf(base, sizeof base, "%s", path);
        if (collect_tree(path, "", &rels, &n, &cap, err, ecap) != 0) {
            for (int i = 0; i < n; i++) free(rels[i]);
            free(rels); return -1;
        }
    }
    int rc = tree_hash(base, rels, n, out, err, ecap);
    for (int i = 0; i < n; i++) free(rels[i]);
    free(rels);
    if (nout) *nout = n;
    return rc;
}

static int copy_tree(const char *src, const char *dst, char *err, size_t ecap) {
    if (mkdir_p(dst) != 0) { snprintf(err, ecap, "cannot create %s", dst); return -1; }
    char **rels = NULL;
    int n = 0, cap = 0;
    if (collect_tree(src, "", &rels, &n, &cap, err, ecap) != 0) {
        for (int i = 0; i < n; i++) free(rels[i]);
        free(rels); return -1;
    }
    for (int i = 0; i < n; i++) {
        char s[FPATH], d[FPATH], dir[FPATH];
        snprintf(s, sizeof s, "%s/%s", src, rels[i]);
        snprintf(d, sizeof d, "%s/%s", dst, rels[i]);
        snprintf(dir, sizeof dir, "%s", d);
        char *sl = strrchr(dir, '/');
        if (sl) *sl = 0;
        if (mkdir_p(dir) != 0 || copy_file(s, d) != 0) {
            snprintf(err, ecap, "cannot copy %s", rels[i]);
            for (int k = 0; k < n; k++) free(rels[k]);
            free(rels); return -1;
        }
    }
    for (int i = 0; i < n; i++) free(rels[i]);
    free(rels);
    return 0;
}

static int backup_path(const char *dest, char *err, size_t ecap) {
    static int seq = 0;
    char bk[FPATH];
    snprintf(bk, sizeof bk, "%s/%s-%d", FETCH_BACKUP,
             base_of(dest), ++seq);
    if (mkdir_p(bk) != 0) { snprintf(err, ecap, "cannot create backup dir"); return -1; }
    char to[FPATH];
    snprintf(to, sizeof to, "%s/%s", bk, base_of(dest));
    if (rename(dest, to) != 0) {
        snprintf(err, ecap, "cannot move existing %s aside (remove it manually)", dest);
        return -1;
    }
    return 0;
}

/* ---------------- 拉取 + 落地 ---------------- */

static int download(const char *url, const char *out, char *err, size_t ecap) {
    char *av[] = {
        "curl", "-fsSL", "--retry", "2", "--max-time", "120",
        "--proto", "=https,http,file",
        "--proto-redir", "=https,http,file",
        "-o", (char *)out, (char *)url, NULL
    };
    int rc = run_argv(av);
    if (rc == -1) { snprintf(err, ecap, "cannot run curl (is it on PATH?)"); return -1; }
    if (rc != 0 || !fexists(out)) {
        snprintf(err, ecap, "download failed (curl exit %d): %s", rc, url);
        return -1;
    }
    return 0;
}

static int unpack(const char *payload, const char *dir, char *err, size_t ecap) {
    if (mkdir_p(dir) != 0) { snprintf(err, ecap, "cannot create unpack dir"); return -1; }
    size_t l = strlen(payload);
    int zip = l >= 4 && strcmp(payload + l - 4, ".zip") == 0;
    int rc;
    if (zip) {
        char *av[] = { "unzip", "-q", "-o", (char *)payload, "-d", (char *)dir, NULL };
        rc = run_argv(av);
        if (rc != 0) {
            char *av2[] = { "tar", "-xf", (char *)payload, "-C", (char *)dir, NULL };
            rc = run_argv(av2);
        }
    } else {
        char *av[] = { "tar", "-xf", (char *)payload, "-C", (char *)dir, NULL };
        rc = run_argv(av);
    }
    if (rc != 0) {
        snprintf(err, ecap, "cannot unpack archive (need tar / unzip on PATH)");
        return -1;
    }
    return 0;
}

/* 归档/Git 常有唯一顶层目录；没指定 subdir 时自动下钻一层。 */
static void pick_root(const char *dir, const char *subdir, char *out, size_t cap) {
    if (subdir && subdir[0]) {
        snprintf(out, cap, "%s/%s", dir, subdir);
        return;
    }
    int n = 0, nfiles = 0;
    FEntry *es = list_entries(dir, &n);
    char only[256] = "";
    for (int i = 0; i < n; i++) {
        if (es[i].name[0] == '.') continue;      /* .git / .github 不算库根内容 */
        if (es[i].isdir) snprintf(only, sizeof only, "%s", es[i].name);
        else nfiles++;
    }
    free(es);
    if (n == 1 && nfiles == 0 && only[0]) snprintf(out, cap, "%s/%s", dir, only);
    else snprintf(out, cap, "%s", dir);
}

int fetch_materialize(const FetchDecl *d, const FetchLock *locked,
                      int overwrite, FetchLock *res, char *err, size_t ecap) {
    memset(res, 0, sizeof *res);
    res->d = *d;
    snprintf(res->resolved, sizeof res->resolved, "-");
    /* 任何实际下载都从这里过，URL 校验放在最内侧而不是各命令里，
     * 免得遗漏；纯本地校验路径不受 URL scheme 影响。 */
    if (fetch_url_check(d->url, err, ecap) != 0) return -1;

    char tmp[FPATH], stage[FPATH], payload[FPATH], dest[FPATH];
    snprintf(tmp, sizeof tmp, "%s/%s", ZX_FETCH_TMP, d->name);
    rm_rf(tmp);
    if (mkdir_p(tmp) != 0) { snprintf(err, ecap, "cannot create %s", tmp); return -1; }
    snprintf(stage, sizeof stage, "%s/stage", tmp);
    if (mkdir_p(stage) != 0) { snprintf(err, ecap, "cannot create stage dir"); return -1; }

    /* 目标路径以锁为准（重拉时 --name 可能与首次不同） */
    if (locked && locked->path[0]) snprintf(dest, sizeof dest, "%s", locked->path);
    else if (d->kind == FK_FILE) {
        char tail[512], ext[64] = ".h";
        url_tail(d->url, tail, sizeof tail);
        char *dot = strrchr(tail, '.');
        if (dot && dot > tail && strlen(dot) < sizeof ext) snprintf(ext, sizeof ext, "%s", dot);
        char *cut = strpbrk(ext, "?#"); if (cut) *cut = 0;
        snprintf(dest, sizeof dest, "vendor/%s%s", d->name, ext);
    } else {
        snprintf(dest, sizeof dest, "vendor/%s", d->name);
    }
    snprintf(res->path, sizeof res->path, "%s", dest);

    char dlhash[65] = "";
    if (d->kind == FK_FILE || d->kind == FK_ARCHIVE) {
        char tail[512];
        url_tail(d->url, tail, sizeof tail);
        char *q = strpbrk(tail, "?#"); if (q) *q = 0;
        const char *ext = strrchr(tail, '.');
        snprintf(payload, sizeof payload, "%s/payload%s", tmp, ext ? ext : ".bin");
        if (download(d->url, payload, err, ecap) != 0) goto fail;
        if (sha_file(payload, dlhash) != 0) { snprintf(err, ecap, "cannot hash download"); goto fail; }
        if (d->sha256[0] && !is_hex64(d->sha256)) {
            snprintf(err, ecap, "declared sha256 is not 64 hex digits"); goto fail;
        }
        if (d->sha256[0] && strcmp(d->sha256, dlhash) != 0) {
            snprintf(err, ecap, "sha256 mismatch: expected %s, got %s (upstream changed?)",
                     d->sha256, dlhash);
            goto fail;
        }
        snprintf(res->d.sha256, sizeof res->d.sha256, "%s", dlhash);
    }

    if (d->kind == FK_FILE) {
        char to[FPATH];
        snprintf(to, sizeof to, "%s/%s", stage, base_of(dest));
        if (copy_file(payload, to) != 0) { snprintf(err, ecap, "cannot stage download"); goto fail; }
    } else if (d->kind == FK_ARCHIVE) {
        char unpackdir[FPATH], root[FPATH];
        snprintf(unpackdir, sizeof unpackdir, "%s/unpack", tmp);
        if (unpack(payload, unpackdir, err, ecap) != 0) goto fail;
        pick_root(unpackdir, d->subdir, root, sizeof root);
        if (!is_dir(root)) { snprintf(err, ecap, "subdir not found in archive: %s", d->subdir); goto fail; }
        if (copy_tree(root, stage, err, ecap) != 0) goto fail;
    } else {
        char repo[FPATH];
        snprintf(repo, sizeof repo, "%s/repo", tmp);
        /* core.autocrlf 必须关掉：否则 Windows / Linux clone 出来的字节不同，
         * zx.lock 的 tree hash 会跨平台误报。detached HEAD 提示也要压掉。 */
        char *av[20];
        int i = 0;
        av[i++] = "git";
        av[i++] = "-c"; av[i++] = "core.autocrlf=false";
        av[i++] = "-c"; av[i++] = "core.eol=lf";
        av[i++] = "-c"; av[i++] = "advice.detachedHead=false";
        av[i++] = "clone"; av[i++] = "--quiet";
        av[i++] = "--depth"; av[i++] = "1";
        if (d->ref[0]) { av[i++] = "--branch"; av[i++] = (char *)d->ref; }
        av[i++] = (char *)d->url; av[i++] = repo; av[i] = NULL;
        int rc = run_argv(av);
        if (rc == -1) { snprintf(err, ecap, "cannot run git (is it on PATH?)"); goto fail; }
        if (rc != 0) { snprintf(err, ecap, "git clone failed (exit %d): %s", rc, d->url); goto fail; }
        char cmd[FPATH], head[128] = "";
        snprintf(cmd, sizeof cmd, "git -C \"%s\" rev-parse HEAD", repo);
        if (capture(cmd, head, sizeof head) != 0 || strlen(head) < 7) {
            snprintf(err, ecap, "cannot resolve git commit"); goto fail;
        }
        head[strcspn(head, "\r\n")] = 0;      /* 只要第一行，别让 git 提示混进 commit */
        snprintf(res->resolved, sizeof res->resolved, "%s", head);
        char root[FPATH];
        if (d->subdir[0]) snprintf(root, sizeof root, "%s/%s", repo, d->subdir);
        else snprintf(root, sizeof root, "%s", repo);
        if (!is_dir(root)) { snprintf(err, ecap, "subdir not found in repo: %s", d->subdir); goto fail; }
        if (copy_tree(root, stage, err, ecap) != 0) goto fail;
    }

    if (tree_at(stage, 0, res->tree, &res->nfiles, err, ecap) != 0) goto fail;

    if (locked) {
        if (locked->tree[0] && strcmp(locked->tree, res->tree) != 0) {
            snprintf(err, ecap, "lock mismatch: downloaded tree hash differs"
                                " (upstream ref was rewritten or content changed)");
            goto fail;
        }
        if (locked->resolved[0] && strcmp(locked->resolved, "-") != 0 &&
            strcmp(locked->resolved, res->resolved) != 0) {
            snprintf(err, ecap, "lock mismatch: git commit is %s, expected %s",
                     res->resolved, locked->resolved);
            goto fail;
        }
        if (locked->d.sha256[0] && dlhash[0] && strcmp(locked->d.sha256, dlhash) != 0) {
            snprintf(err, ecap, "lock mismatch: download sha256 differs");
            goto fail;
        }
    }

    if (mkdir_p("vendor") != 0) { snprintf(err, ecap, "cannot create vendor/"); goto fail; }
    if (fexists(dest)) {
        if (!overwrite) {
            snprintf(err, ecap, "%s already exists (use --force to replace)", dest);
            goto fail;
        }
        if (backup_path(dest, err, ecap) != 0) goto fail;
    }
    if (d->kind == FK_FILE) {
        char from[FPATH];
        snprintf(from, sizeof from, "%s/%s", stage, base_of(dest));
        if (copy_file(from, dest) != 0) { snprintf(err, ecap, "cannot write %s", dest); goto fail; }
    } else if (copy_tree(stage, dest, err, ecap) != 0) {
        goto fail;
    }
    rm_rf(tmp);
    return 0;

fail:
    rm_rf(tmp);
    return -1;
}

/* ---------------- 纯本地校验 ---------------- */

int fetch_verify_entry(const FetchLock *l, char *err, size_t cap) {
    if (err && cap) err[0] = 0;
    if (!l->path[0]) { snprintf(err, cap, "lock entry has no path"); return -1; }
    if (!fexists(l->path)) {
        snprintf(err, cap, "%s is missing", l->path);
        return 1;
    }
    char h[65]; int n = 0;
    int is_file = (l->d.kind == FK_FILE);
    if (tree_at(l->path, is_file, h, &n, err, cap) != 0) return -1;
    if (strcmp(h, l->tree) != 0) {
        snprintf(err, cap, "%s content differs from " ZX_LOCK_PATH, l->path);
        return 2;
    }
    return 0;
}

int fetch_check_local(int deep, FILE *out) {
    char e[512];
    FetchLock *v = NULL;
    int n = fetch_lock_load(&v, e, sizeof e);
    if (n <= 0) { fetch_lock_free(v, n); return 0; }
    int bad = 0;
    for (int i = 0; i < n; i++) {
        char msg[512];
        int rc = deep ? fetch_verify_entry(&v[i], msg, sizeof msg) :
                        (fexists(v[i].path) ? 0 : (snprintf(msg, sizeof msg,
                            "%s is missing", v[i].path), 1));
        if (rc != 0) {
            bad++;
            fprintf(out, "zxc: vendor '%s': %s\n", v[i].d.name, msg);
        }
    }
    if (bad) fprintf(out, "zxc: run `zxc fetch` to restore vendor/ from " ZX_LOCK_PATH
                          " (build itself never touches the network)\n");
    fetch_lock_free(v, n);
    return bad;
}
