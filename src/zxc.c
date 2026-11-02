/* zxc.c — diagnostics + growable buffer. */
#include "zxc.h"
#include <stdarg.h>

const char *g_srcpath = "<stdin>";
int g_error_count = 0;

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
    { "unknown identifier",               "E0401" },
    { "unknown type",                     "E0401" },
    { "not found",                        "E0401" },
    { "already a member",                 "E0402" },
    { "duplicate",                        "E0402" },
    { "cannot assign",                    "E0403" },
    { "read-only",                        "E0403" },
    { "is private",                       "E0404" },
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
    case PH_LEX:    return "E0100";
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

static void diag_remember(const char *code, int line, int col, const char *msg) {
    int i = g_recent_n % 8;
    g_recent[i].line = line;
    g_recent[i].col = col;
    snprintf(g_recent[i].code, sizeof g_recent[i].code, "%s", code);
    snprintf(g_recent[i].msg, sizeof g_recent[i].msg, "%s", msg);
    g_recent_n++;
}

static void diag_emit(const char *code, int line, int col, const char *msg) {
    if (diag_is_dup(code, line, col, msg)) return;
    diag_remember(code, line, col, msg);
    g_error_count++;
    if (g_error_count > g_diag_max_print) return;   /* 只计数，不再刷屏 */
    g_diag_shown++;
    fprintf(stderr, "%s:%d:%d: error[%s]: %s\n", g_srcpath, line, col, code, msg);
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
