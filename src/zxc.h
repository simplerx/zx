/* zxc.h — Zx compiler: shared definitions. */
#ifndef ZXC_H
#define ZXC_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

/* ---------- version ---------- */

/* 唯一版本源：`zxc -v` / `zxc --version` 与生成的工程清单都读这里。
 * 编号跟 README 的里程碑走：次版本 = 里程碑号（M49 → 0.49），修订号
 * 留给同一里程碑里的修补。改这个数就顺手在 README 的「状态」里改一行。 */
#define ZXC_VERSION "0.49.2"

/* ---------- diagnostics ---------- */

/* 错误码按阶段分段，消息内容决定具体码（见 zxc.c 的码表）：
 *   E01xx 词法   E02xx 语法   E03xx 类型/表达式   E04xx 名字/声明
 *   E05xx 语言规则（R1–R10）  E06xx 模块   E07xx 泛型   E08xx 内建/格式
 * 输出形如 `file.zx:3:7: error[E0302]: ...`。 */
typedef enum {
    PH_LEX,
    PH_PARSE,
    PH_SEMA,
    PH_DRIVER,
} DiagPhase;

extern const char *g_srcpath;
void diag_at(DiagPhase ph, int line, int col, const char *fmt, ...);
/* 明确知道码的调用点用它（解析器几乎全是 E0201，不该让码表猜） */
void diag_code_at(const char *code, int line, int col, const char *fmt, ...);
void err_at(int line, int col, const char *fmt, ...);   /* = diag_at(PH_SEMA, …) */
/* fix-it：预挂一条 help 行，跟在下一条诊断后面（A2）。 */
void diag_help(const char *fmt, ...);
extern int g_error_count;
extern int g_diag_shown;       /* 实际打印过多少条（超过上限后只计数） */
extern int g_diag_max_print;   /* 打印上限 */

/* ---------- growable byte buffer ---------- */

typedef struct {
    char  *data;
    size_t len, cap;
} Buf;

void   buf_init(Buf *b);
void   buf_putc(Buf *b, char c);
void   buf_puts(Buf *b, const char *s);
void   buf_putn(Buf *b, const char *s, size_t n);
void   buf_printf(Buf *b, const char *fmt, ...);
void   buf_free(Buf *b);

/* ---------- tokens ---------- */

#define TOKEN_KINDS(X)                                                         \
    /* literals & names */                                                     \
    X(EOF_,     "end of file")                                                 \
    X(IDENT,    "identifier")                                                  \
    X(INT,      "integer")                                                     \
    X(FLOAT,    "float")                                                       \
    X(CHAR,     "char")                                                        \
    X(STRING,   "string")                                                      \
    /* keywords (25) */                                                        \
    X(KW_CONST,   "'const'")                                                   \
    X(KW_LET,     "'let'")                                                     \
    X(KW_VAR,     "'var'")                                                     \
    X(KW_FN,      "'fn'")                                                      \
    X(KW_MODULE,  "'module'")                                                  \
    X(KW_TYPE,    "'type'")                                                    \
    X(KW_NULL,    "'null'")                                                    \
    X(KW_UNDEF,   "'undefined'")                                               \
    X(KW_IF,      "'if'")                                                      \
    X(KW_ELSE,    "'else'")                                                    \
    X(KW_WHILE,   "'while'")                                                   \
    X(KW_FOR,     "'for'")                                                     \
    X(KW_INLINE,  "'inline'")                                                  \
    X(KW_SWITCH,  "'switch'")                                                  \
    X(KW_CONTINUE,"'continue'")                                                \
    X(KW_RETURN,  "'return'")                                                  \
    X(KW_DEFER,   "'defer'")                                                   \
    X(KW_ERRDEFER,"'errdefer'")                                                \
    X(KW_STRUCT,  "'struct'")                                                  \
    X(KW_UNION,   "'union'")                                                   \
    X(KW_ENUM,    "'enum'")                                                    \
    X(KW_ERROR,   "'error'")                                                  \
    X(KW_PUB,     "'pub'")                                                     \
    X(KW_EXTERN,  "'extern'")                                                  \
    X(KW_ORELSE,  "'orelse'")                                                  \
    X(KW_TRY,     "'try'")                                                     \
    /* punctuation & operators */                                              \
    X(LPAREN,   "'('")    X(RPAREN,   "')'")                                   \
    X(LBRACKET, "'['")    X(RBRACKET, "']'")                                   \
    X(LBRACE,   "'{'")    X(RBRACE,   "'}'")                                   \
    X(COMMA,    "','")    X(SEMI,     "';'")                                   \
    X(COLON,    "':'")    X(AT,       "'@'")                                   \
    X(DOT,      "'.'")    X(DOT_BRACE,"'.{'")                                  \
    X(DOTDOT,   "'..'")                                                              \
    X(ARROW,    "'->'")                                                              \
    X(PLUS,     "'+'")    X(MINUS,   "'-'")   X(STAR, "*")   X(SLASH, "'/'")  \
    X(PERCENT,  "'%'")    X(CARET,   "'^'")   X(TILDE, "'~'")  X(BANG, "'!'") \
    X(QUESTION, "'?'")                                                              \
    X(AMP,      "'&'")    X(PIPE,    "'|'")                                  \
    X(ANDAND,   "'&&'")   X(OROR,    "'||'")                                 \
    X(SHL,      "'<<'")   X(SHR,     "'>>'")                                 \
    X(LT,       "'<'")    X(LE,      "'<='")  X(GT, "'>'")    X(GE, "'>='") \
    X(EQ,       "'=='")   X(NE,      "'!='")                                 \
    X(ASSIGN,   "'='")                                                           \
    X(PLUSEQ,   "'+='")   X(MINUSEQ,"'-='")  X(STAREQ, "'*='") X(SLASHEQ,"'/='")\
    X(PERCEQ,   "'%='")   X(AMPEQ,  "'&='")  X(PIPEEQ, "'|='") X(CARETEQ,"'^='")\
    X(SHLEQ,    "'<<='")  X(SHREQ,  "'>>='")

typedef enum {
#define X(k, s) TOK_##k,
    TOKEN_KINDS(X)
#undef X
} TokKind;

const char *tok_kind_name(TokKind k);   /* human-readable, for errors */
const char *tok_kind_spell(TokKind k);  /* short spelling, for dumps  */

typedef struct {
    TokKind      kind;
    const char  *start;   /* pointer into source text */
    int          len;
    int          line, col;
    /* semantic values */
    union {
        uint64_t  int_val;    /* INT (unfixed base value, suffix checked later) */
        double    float_val;  /* FLOAT */
        uint32_t  char_val;   /* CHAR (code point; >255 allowed, checked later) */
    } v;
    /* STRING / CHAR: decoded bytes (chars: 1..4 bytes utf-8) in a scratch buf */
    const char *text;   /* for STRING: decoded content, NUL-terminated */
    int         text_len;
    uint32_t    str_off; /* offset into Lexer.strbuf; text fixed up after lex */
    bool        big_int; /* INT: value exceeds u64 — comptime must read text */
} Token;

typedef struct {
    const char *src;
    size_t      src_len;
    size_t      pos;
    int         line, col;
    Token      *toks;
    size_t      ntoks, captoks;
    Buf         strbuf;   /* decoded string storage */
} Lexer;

void lexer_init(Lexer *lx, const char *src, size_t len);
void lexer_run(Lexer *lx);   /* fills lx->toks; errors reported via err_at */

#endif /* ZXC_H */
