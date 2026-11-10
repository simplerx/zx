/* zxc.c — diagnostics + growable buffer. */
#include "zxc.h"
#include <stdarg.h>

const char *g_srcpath = "<stdin>";
int g_error_count = 0;

/* ---------- 外部库链接（@link） ----------
 * 语义阶段由 @link("ws2_32") 填，driver 拼链接命令时消费。
 * 去重：同名只记一次（多模块都 @link 同一个库很常见）。 */
char *g_link_libs[ZXC_MAX_LINK_LIBS];
int   g_nlink_libs = 0;

void zx_add_link_lib(const char *name) {
    if (!name || !*name) return;
    for (int i = 0; i < g_nlink_libs; i++)
        if (!strcmp(g_link_libs[i], name)) return;
    if (g_nlink_libs >= ZXC_MAX_LINK_LIBS) return;
    g_link_libs[g_nlink_libs++] = strdup(name);
}

/* 超过这个数就不再打印（继续计数）：一个语法错能连出几十条级联报错，
 * 把终端刷满反而看不见第一个错——第一个错通常就是根因。 */
int g_diag_max_print = 30;
int g_diag_shown = 0;

/* 消息片段 → 错误码。一条消息从这张表里找第一个命中的片段，
 * 没命中就按阶段给默认码（E01xx / E02xx / E03xx）。
 * 加新错误时：能归到已有码就复用，没有合适的就是新码——码一旦发布不要改。 */
typedef struct { const char *match; const char *code; } DiagRule;

static const DiagRule g_rules[] = {
    /* ---- 先匹配最具体的，笼统的放后面（第一条命中即用） ---- */

    /* 模块 */
    { "standard library",                 "E0601" },
    { "module",                           "E0601" },
    { "cycl",                             "E0601" },

    /* 泛型 */
    { "generic",                          "E0701" },
    { "type parameters",                  "E0701" },

    /* 内建与格式串 */
    { "was removed:",                     "E0211" },
    { "format specifier",                 "E0802" },
    { "format string",                    "E0802" },
    { "sequence(s) but",                  "E0802" },

    /* 语言规则 */
    { "division by zero",                 "E0505" },
    { "@compileError",                    "E0507" },
    { "@trap",                            "E0507" },
    { "reached at compile time",          "E0507" },
    { "must be computable at compile time", "E0501" },
    { "compile time",                     "E0501" },
    { "compile-time",                     "E0501" },
    { "can reach the end of its body",    "E0502" },
    { "declared `noreturn`",              "E0502" },
    { "unreachable",                      "E0502" },
    { "never falls through",              "E0502" },
    { "stack frame",                      "E0503" },
    { "escapes",                          "E0503" },
    { "exhaustive",                       "E0504" },

    /* 控制流位置：continue/return/defer 放错地方，标签找不到 */
    { "outside a loop",                   "E0405" },
    { "outside a function",               "E0405" },
    /* errdefer 放错函数：消息里带 "found `T`"，不先命中就会落到
     * 下面那条笼统的 "found `"（E0301），help 变成"加显式转换"——完全
     * 不对路的建议比没有建议更糟。 */
    { "`errdefer`",                       "E0405" },
    { "inside `defer`",                   "E0405" },
    { "labeled",                          "E0405" },
    { "labelled",                         "E0405" },

    /* 类型与表达式 */
    { "does not fit in",                  "E0302" },
    { "negative literal",                 "E0302" },
    { "mismatched types",                 "E0301" },
    { "cannot convert",                   "E0301" },
    { "cannot coerce",                    "E0301" },
    { "no implicit conversion",           "E0301" },
    { "cannot infer the type",            "E0301" },
    { "has no member",                    "E0304" },
    { "has no field",                     "E0304" },
    { "has no variant",                   "E0304" },
    /* 语义层的 "expected `i32`, found `[]const u8`" 是类型错，不是语法错：
     * 必须在笼统的 "expected"（E0201）之前命中 */
    { "found `",                          "E0301" },
    /* 类型形状：指针有没有数量、?T 能不能嵌套 */
    { "has no count",                     "E0305" },
    { "cannot nest",                      "E0305" },
    /* 聚合字面量 */
    { "union literal must initialize",    "E0307" },
    /* 运算符与操作数 */
    { "cannot add two pointers",          "E0306" },
    { "is not a sequence",                "E0306" },
    { "must have equal length",           "E0306" },
    { "cannot take the address",          "E0403" },

    /* 名字、声明、可见性 */
    { "unused",                           "E0410" },
    { "discarded",                        "E0411" },
    { "unknown identifier",               "E0401" },
    { "unknown type",                     "E0401" },
    { "not found",                        "E0401" },
    { "already a member",                 "E0402" },
    { "duplicate",                        "E0402" },
    { "cannot assign",                    "E0403" },
    { "read-only",                        "E0403" },
    { "is private",                       "E0404" },
    { "private member",                   "E0404" },
    { "only allowed on file-scope",       "E0404" },

    /* 参数个数：比 "needs" 更靠前，`fn takes 2 argument(s)` 要落到这里 */
    { "argument",                         "E0303" },
    { "parameter",                        "E0303" },

    /* 笼统的操作数类型 */
    { "needs",                            "E0306" },
    { "cannot match",                     "E0306" },

    /* 语法 */
    { "expected",                         "E0201" },
    { "only be used",                     "E0202" },
    { "cannot contain",                   "E0202" },
    { "unexpected",                       "E0202" },

    /* 词法 */
    { "unterminated",                     "E0101" },
    { "invalid",                          "E0102" },
};

static const char *code_for(DiagPhase ph, const char *msg) {
    for (size_t i = 0; i < sizeof g_rules / sizeof g_rules[0]; i++)
        if (strstr(msg, g_rules[i].match)) return g_rules[i].code;
    switch (ph) {
    /* 词法错误一律按 E0102（非法字符或字面量）；没闭合的字面量/注释由
     * 上面的规则先命中 E0101。原先的"未分类"兜底码 E0100 已退役：
     * 每个词法错都能归到这两类之一，留着它只会让诊断更含糊。 */
    case PH_LEX:    return "E0102";
    case PH_PARSE:  return "E0200";
    case PH_SEMA:   return "E0300";
    default:        return "E0900";
    }
}

/* 同一个位置完全相同的诊断只留一条。
 *
 * 语义检查对同一个表达式可能求值两次（一次定型、一次常量折叠），
 * 而求值是带副作用的（会 err_at）——于是 `1 / 0` 会报两遍。
 * 与其在每个求值点上去重（那样的补丁会越打越多），不如在诊断的唯一出口
 * 按「位置 + 码 + 文字」判定重复：用户看到的条数必须等于摘要里的数字。 */
static struct { int line, col; char code[8]; char msg[160]; } g_recent[8];
static int g_recent_n = 0;

static bool diag_is_dup(const char *code, int line, int col, const char *msg) {
    for (int i = 0; i < 8 && i < g_recent_n; i++)
        if (g_recent[i].line == line && g_recent[i].col == col &&
            !strcmp(g_recent[i].code, code) && !strcmp(g_recent[i].msg, msg))
            return true;
    return false;
}

/* fix-it（A2）：预挂一条 help 行，跟在下一条发出的诊断后面（rustc 的
 * `help:` 风格）。调用方在 err_at 之前调 diag_help；诊断被去重或超上限
 * 截断时一并丢弃，help 永远不会孤立出现。 */
static char g_help[240];
static bool g_help_pending;

void diag_help(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_help, sizeof g_help, fmt, ap);
    va_end(ap);
    g_help_pending = true;
}

/* ---------- 源码渲染器（D1） ----------
 * 诊断不再只有一行头：后面跟出错那行的源码和 ^ 指位——"第 18 列"变成
 * 直接看得见的箭头。列号是编译器口径（按字节），渲染时换算显示宽度：
 * UTF-8 多字节字符按 2 格算（CJK 为主体），tab 展开到 4 的倍数，指针才落得准。
 * 源码按需读、按路径缓存多槽（note 常在另一个模块里，单槽会来回重读）。 */
typedef struct { char *path; char **lines; int n; } SrcFile;

static SrcFile g_srcs[8];
static int g_nsrcs = 0;

static SrcFile *src_file(const char *path) {
    if (!path || !*path) return NULL;
    for (int i = 0; i < g_nsrcs; i++)
        if (strcmp(g_srcs[i].path, path) == 0) return &g_srcs[i];
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0 || sz > 32 * 1024 * 1024) { fclose(fp); return NULL; }
    rewind(fp);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[rd] = '\0';
    int cap = 2;                                   /* 段数最多 \n 数 + 2（尾部空段） */
    for (size_t i = 0; i < rd; i++) if (buf[i] == '\n') cap++;
    char **lines = malloc(sizeof(char *) * (size_t)cap);
    if (!lines) { free(buf); return NULL; }
    int li = 0;
    char *start = buf;
    for (size_t i = 0; i <= rd; i++) {
        char c = buf[i];               /* 先存原字符——覆写 NUL 后再判就分不清行尾和文件尾了 */
        if (c == '\n' || c == '\0') {
            buf[i] = '\0';
            size_t len = strlen(start);
            if (len && start[len - 1] == '\r') start[len - 1] = '\0';
            if (li < cap) lines[li++] = start;
            start = buf + i + 1;
            if (c == '\0') break;
        }
    }
    if (g_nsrcs == 8) {                            /* 缓存满：腾最旧的 */
        free(g_srcs[0].path); free(g_srcs[0].lines);   /* 行指针指向同一块 buf */
        for (int i = 1; i < 8; i++) g_srcs[i - 1] = g_srcs[i];
        g_nsrcs--;
    }
    SrcFile *sf = &g_srcs[g_nsrcs++];
    sf->path = strdup(path);
    sf->lines = lines;
    sf->n = li;
    return sf;
}

/* 一个字符的显示宽度：0xC0-0xDF 开头是西文等窄字符（1 格），
 * 0xE0+ 是 CJK/韩文等宽字符（2 格）。近似值——只求指针落得大致准。 */
static int disp_width_of(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 1;
    return 2;
}

static void render_snippet(const char *path, int line, int col) {
    if (!path || !*path || line <= 0) return;
    SrcFile *sf = src_file(path);
    if (!sf || line > sf->n) return;
    const char *text = sf->lines[line - 1];
    /* 超长行（压缩过的/生成的代码）只显示插入点周围的一窗：整行打出来会把
     * 真正的诊断冲走，几千列的 `^` 更是没法看。窗口 160 列，两端用 ... 标。 */
    char win[512];
    int  shift = 0;                 /* 显示窗口前裁掉了几个字节 */
    size_t tlen = strlen(text);
    if (tlen > 160) {
        size_t c = col > 0 ? (size_t)(col - 1) : 0;
        if (c > tlen) c = tlen;
        size_t a = c > 60 ? c - 60 : 0;
        size_t b = a + 160;
        if (b > tlen) { b = tlen; a = b > 160 ? b - 160 : 0; }
        size_t o = 0;
        if (a > 0) { win[o++] = '.'; win[o++] = '.'; win[o++] = '.'; }
        size_t n = b - a;
        if (n > sizeof win - o - 5) n = sizeof win - o - 5;
        memcpy(win + o, text + a, n);
        o += n;
        if (b < tlen) { win[o++] = '.'; win[o++] = '.'; win[o++] = '.'; }
        win[o] = 0;
        shift = (int)a - (a > 0 ? 3 : 0);
        text = win;
        col -= shift;
        if (col < 1) col = 1;
    }
    int gw = 1;
    for (int t = line; t >= 10; t /= 10) gw++;
    Buf body;
    buf_init(&body);
    int dcol = 0, caret = 0, idx = 1;
    bool found = col == 1;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (idx == col) { caret = dcol; found = true; }
        if (*p == '\t') { do { buf_putc(&body, ' '); dcol++; } while (dcol & 3); }
        else {
            buf_putc(&body, (char)*p);
            if ((*p & 0xC0) != 0x80) dcol += disp_width_of(*p);
        }
        idx++;
    }
    if (!found) caret = dcol;                      /* 列越界：指到行尾 */
    fprintf(stderr, " %*d | %s\n", gw, line, body.data ? body.data : "");
    fprintf(stderr, " %*s | ", gw, "");
    for (int i = 0; i < caret; i++) fputc(' ', stderr);
    fprintf(stderr, "^\n");
    buf_free(&body);
}

/* note（D2）：预挂一个"相关位置"，跟在下一条诊断后面——`redeclared` 带
 * `first declared here`，`is private` 带声明点。生命周期同 help：诊断被
 * 去重或超上限截断时一并丢弃，note 永远不会孤立出现。 */
static struct { char path[512]; int line, col; char text[200]; } g_note;
static bool g_note_pending;

/* 兜底修法：没有具体 fix-it 时，把该码在 errors.c 里的通用修法当 help 行
 * 印出来——用户不必再 `zxc explain` 一次就知道往哪改。
 *
 * 每个码**只印一次**：一类错误常常连成一片（十个 E0301），同一句修法重复
 * 十遍只是噪音，反而把真正的诊断挤出屏幕。后面同码的只留消息。 */
static char g_fix_shown[16][8];
static int g_fix_shown_n;

static bool fix_shown(const char *code) {
    for (int i = 0; i < g_fix_shown_n; i++)
        if (!strcmp(g_fix_shown[i], code)) return true;
    return false;
}

static void fix_mark(const char *code) {
    if (g_fix_shown_n >= (int)(sizeof g_fix_shown / sizeof g_fix_shown[0])) return;
    snprintf(g_fix_shown[g_fix_shown_n], sizeof g_fix_shown[0], "%s", code);
    g_fix_shown_n++;
}

void diag_note(const char *path, int line, int col, const char *fmt, ...) {
    if (!path || !*path) path = g_srcpath;
    snprintf(g_note.path, sizeof g_note.path, "%s", path);
    g_note.line = line;
    g_note.col = col;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_note.text, sizeof g_note.text, fmt, ap);
    va_end(ap);
    g_note_pending = true;
}

/* 子单元（模块）解析失败时，父诊断里要能说出"第一条错在哪、错在什么码"。
 * 只留一句 "module 'x' failed to parse"，用户就得自己钻进几千行的生成代码
 * 里找——而那条错（比如 `0x` 少了数字）本来就该直接呈现在眼前。 */
static char g_first_err_msg[256];
static char g_first_err_code[16];
static int  g_first_err_line, g_first_err_col, g_first_err_set;

void diag_first_error_reset(void) { g_first_err_set = 0; }

int diag_first_error(int *line, int *col, char *code, size_t ccap,
                     char *msg, size_t mcap) {
    if (!g_first_err_set) return 0;
    if (line) *line = g_first_err_line;
    if (col)  *col  = g_first_err_col;
    if (code && ccap) snprintf(code, ccap, "%s", g_first_err_code);
    if (msg && mcap)  snprintf(msg, mcap, "%s", g_first_err_msg);
    return 1;
}

static void diag_remember(const char *code, int line, int col, const char *msg) {
    int i = g_recent_n % 8;
    g_recent[i].line = line;
    g_recent[i].col = col;
    snprintf(g_recent[i].code, sizeof g_recent[i].code, "%s", code);
    snprintf(g_recent[i].msg, sizeof g_recent[i].msg, "%s", msg);
    g_recent_n++;
}

static void diag_emit(const char *code, int line, int col, const char *msg) {
    if (diag_is_dup(code, line, col, msg)) {
        g_help_pending = false;
        g_note_pending = false;
        return;
    }
    diag_remember(code, line, col, msg);
    if (!g_first_err_set) {
        g_first_err_set = 1;
        g_first_err_line = line;
        g_first_err_col = col;
        snprintf(g_first_err_code, sizeof g_first_err_code, "%s", code);
        snprintf(g_first_err_msg, sizeof g_first_err_msg, "%s", msg);
    }
    g_error_count++;
    /* 上限可从环境变量调（ZX_DIAG_MAX=0 表示全打）：迁移期一次看全，
     * 平时仍然限 30 条免得刷屏。 */
    {
        const char *cap = getenv("ZX_DIAG_MAX");
        if (cap && *cap) g_diag_max_print = atoi(cap) > 0 ? atoi(cap) : 1 << 30;
    }
    if (g_error_count > g_diag_max_print) {
        g_help_pending = false;
        g_note_pending = false;
        return;                                    /* 只计数，不再刷屏 */
    }
    g_diag_shown++;
    fprintf(stderr, "%s:%d:%d: error[%s]: %s\n", g_srcpath, line, col, code, msg);
    /* D1：头下面跟源码行 + 指位 */
    render_snippet(g_srcpath, line, col);
    /* fix-it（A2）：diag_help 预挂的建议行；note（D2）带声明点片段。
     * 没有预挂建议时用错误码目录里的通用修法兜底（每码只印一次）。 */
    if (g_help_pending) fprintf(stderr, "  help: %s\n", g_help);
    else {
        const char *fix = errors_fix(code);
        if (fix && !fix_shown(code)) {
            fprintf(stderr, "  help: %s\n", fix);
            fix_mark(code);
        }
    }
    if (g_note_pending) {
        if (strcmp(g_note.path, g_srcpath) == 0)
            fprintf(stderr, "  note: %s\n", g_note.text);
        else
            fprintf(stderr, "  note: %s (%s:%d:%d)\n", g_note.text,
                    g_note.path, g_note.line, g_note.col);
        render_snippet(g_note.path, g_note.line, g_note.col);
    }
    g_help_pending = false;
    g_note_pending = false;
}

static void diag_v(DiagPhase ph, int line, int col, const char *fmt, va_list ap) {
    char msg[1024];
    vsnprintf(msg, sizeof msg, fmt, ap);
    diag_emit(code_for(ph, msg), line, col, msg);
}

void diag_at(DiagPhase ph, int line, int col, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    diag_v(ph, line, col, fmt, ap);
    va_end(ap);
}

void diag_code_at(const char *code, int line, int col, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    diag_emit(code, line, col, msg);
}

void err_at(int line, int col, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    diag_v(PH_SEMA, line, col, fmt, ap);
    va_end(ap);
}

void buf_init(Buf *b) { b->data = NULL; b->len = b->cap = 0; }

static void buf_grow(Buf *b, size_t need) {
    if (b->len + need <= b->cap) return;
    size_t ncap = b->cap ? b->cap * 2 : 256;
    while (ncap < b->len + need) ncap *= 2;
    b->data = realloc(b->data, ncap);
    if (!b->data) { fprintf(stderr, "zxc: out of memory\n"); exit(1); }
    b->cap = ncap;
}

/* 缓冲区始终以 NUL 结尾：调用点可以放心把 data 当字符串用
 * （sizeof 里读 type 名、生成 C 代码时拼接片段都依赖这一点）。 */
void buf_putc(Buf *b, char c) {
    buf_grow(b, 2);
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}
void buf_putn(Buf *b, const char *s, size_t n) {
    buf_grow(b, n + 1);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}
void buf_puts(Buf *b, const char *s) { buf_putn(b, s, strlen(s)); }

void buf_printf(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    buf_grow(b, (size_t)n + 1);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

void buf_free(Buf *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

const char *tok_kind_name(TokKind k) {
    static const char *names[] = {
#define X(k, s) s,
        TOKEN_KINDS(X)
#undef X
    };
    return names[k];
}

const char *tok_kind_spell(TokKind k) {
    static const char *spells[] = {
#define X_(k, s) #k,
#define X(k, s) X_(k, s)
        TOKEN_KINDS(X)
#undef X
#undef X_
    };
    return spells[k];
}
