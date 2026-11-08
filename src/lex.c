/* lex.c — Zx lexer.
 *
 * Lexical rules (spec v0.5 §1):
 *   - `.{` is one token (DOT_BRACE); `&.{…}` is '&' + DOT_BRACE.
 *   - `.` followed by a digit is an index (t.0.0), NOT a float:
 *     floats must have an integer part (0.5 ok, .5 invalid).
 *   - `..` is the range token; `1..2` lexes as INT DOTDOT INT.
 *   - line comments with two slashes; block comments open slash-star
 *     and close star-slash (no nesting).
 *   - INT suffixes: u8 u16 u32 u64 i8 i16 i32 i64 usize isize
 *     FLOAT suffixes: f16 f32 f64 f128
 *   - CHAR: 'a' '\n' '\xHH' — decoded to 1..4 UTF-8 bytes.
 *   - STRING: "…" with escapes; {} placeholders pass through untouched
 *     (interpretation is a formatting-layer concern).
 */
#include "zxc.h"
#include <stdarg.h>

/* forward decls */
static int hexval(char c);
static int utf8_encode(uint32_t cp, char out[4]);
static bool is_hex(char c);
static bool is_alnum_in_base(char c, int base);

/* ---- keyword table ---- */

typedef struct { const char *name; TokKind kind; } KwEntry;
static const KwEntry g_keywords[] = {
    {"const", TOK_KW_CONST},   {"let", TOK_KW_LET},       {"var", TOK_KW_VAR},
    {"fn", TOK_KW_FN},         {"module", TOK_KW_MODULE}, {"type", TOK_KW_TYPE},
    {"null", TOK_KW_NULL},     {"undefined", TOK_KW_UNDEF},
    {"if", TOK_KW_IF},         {"else", TOK_KW_ELSE},
    {"while", TOK_KW_WHILE},   {"for", TOK_KW_FOR},
    {"inline", TOK_KW_INLINE},
    {"switch", TOK_KW_SWITCH}, {"continue", TOK_KW_CONTINUE},
    {"return", TOK_KW_RETURN}, {"defer", TOK_KW_DEFER},
    {"errdefer", TOK_KW_ERRDEFER},
    {"struct", TOK_KW_STRUCT}, {"union", TOK_KW_UNION},
    {"enum", TOK_KW_ENUM},     {"error", TOK_KW_ERROR},
    {"pub", TOK_KW_PUB},       {"extern", TOK_KW_EXTERN},
    {"orelse", TOK_KW_ORELSE},
    {"try", TOK_KW_TRY},
};

static TokKind ident_kind(const char *s, int len) {
    for (size_t i = 0; i < sizeof g_keywords / sizeof g_keywords[0]; i++) {
        if ((int)strlen(g_keywords[i].name) == len &&
            memcmp(g_keywords[i].name, s, len) == 0)
            return g_keywords[i].kind;
    }
    return TOK_IDENT;
}

/* ---- helpers ---- */

static bool is_ident_start(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static bool is_ident_char(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

typedef struct {
    Lexer *lx;
    const char *p;    /* current pointer */
    int line, col;
} Lex;

/* Advance, maintaining line/col. */
static char adv(Lex *l) {
    char c = *l->p++;
    if (c == '\n') { l->line++; l->col = 1; } else { l->col++; }
    return c;
}
static char peek(Lex *l) { return *l->p; }
static char peek2(Lex *l) { return l->p[1]; }

static Token *emit(Lex *l, TokKind kind, const char *start) {
    Lexer *lx = l->lx;
    if (lx->ntoks == lx->captoks) {
        lx->captoks = lx->captoks ? lx->captoks * 2 : 1024;
        lx->toks = realloc(lx->toks, lx->captoks * sizeof(Token));
        if (!lx->toks) { fprintf(stderr, "zxc: out of memory\n"); exit(1); }
    }
    Token *t = &lx->toks[lx->ntoks++];
    t->kind = kind;
    t->start = start;
    t->len = (int)(l->p - start);
    t->line = l->line;
    t->col = l->col - t->len;   /* col of token start */
    /* recalc: col was advanced past token; token start col = col - len only
       holds when no newline inside; newlines can't occur in tokens we emit here. */
    t->text = NULL;
    t->text_len = 0;
    return t;
}

static void lex_error(Lex *l, const char *start, const char *fmt, ...) {
    (void)start;
    va_list ap;
    va_start(ap, fmt);
    char msg[512];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    diag_at(PH_LEX, l->line, l->col, "%s", msg);
}

/* ---- comments & whitespace ---- */

static void skip_trivia(Lex *l) {
    for (;;) {
        char c = peek(l);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { adv(l); continue; }
        if (c == '/' && peek2(l) == '/') {
            while (peek(l) && peek(l) != '\n') adv(l);
            continue;
        }
        if (c == '/' && peek2(l) == '*') {
            const char *start = l->p;
            int sline = l->line, scol = l->col;
            adv(l); adv(l);
            bool closed = false;
            while (peek(l)) {
                if (peek(l) == '*' && peek2(l) == '/') { adv(l); adv(l); closed = true; break; }
                adv(l);
            }
            if (!closed) {
                l->line = sline; l->col = scol;
                lex_error(l, start, "unterminated block comment");
                l->line = sline; /* restore */
            }
            continue;
        }
        break;
    }
}

/* ---- strings & chars ---- */

/* Decode one escape sequence after '\\'. 'p' points at the escape char.
 * Appends UTF-8 bytes to `out` (a small local array), returns bytes written,
 * or -1 on error. */
static int decode_escape(Lex *l, char esc, char out[4]) {
    switch (esc) {
    case 'n':  out[0] = '\n'; return 1;
    case 't':  out[0] = '\t'; return 1;
    case 'r':  out[0] = '\r'; return 1;
    case '0':  out[0] = '\0'; return 1;
    case '\\': out[0] = '\\'; return 1;
    case '"':  out[0] = '"';  return 1;
    case '\'': out[0] = '\''; return 1;
    case 'a':  out[0] = '\a'; return 1;
    case 'b':  out[0] = '\b'; return 1;
    case 'f':  out[0] = '\f'; return 1;
    case 'v':  out[0] = '\v'; return 1;
    case 'e':  out[0] = 0x1b; return 1;
    case 'x': {
        int hi = hexval(adv(l)), lo = hexval(adv(l));
        if (hi < 0 || lo < 0) { lex_error(l, l->p, "\\x needs two hex digits"); return -1; }
        out[0] = (char)(hi * 16 + lo);
        return 1;
    }
    case 'u': {
        /* \u{HEX+} → UTF-8（EBNF 只写了这一种写法）。老的 \uXXXX 直接
         * 说"要加花括号"——两种写法只会让人猜该用哪个。 */
        if (peek(l) != '{') {
            lex_error(l, l->p, "\\u takes braces: write \\u{4F60}");
            return -1;
        }
        adv(l);
        uint32_t cp = 0;
        int nd = 0;
        while (nd < 6 && hexval(peek(l)) >= 0) { cp = cp * 16 + (uint32_t)hexval(adv(l)); nd++; }
        if (nd == 0) { lex_error(l, l->p, "\\u{...} needs at least one hex digit"); return -1; }
        if (peek(l) != '}') { lex_error(l, l->p, "\\u{...} is missing its closing '}'"); return -1; }
        adv(l);
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            lex_error(l, l->p, "code point out of range (U+0000..U+10FFFF, surrogates excluded)");
            return -1;
        }
        return utf8_encode(cp, out);
    }
    default:
        lex_error(l, l->p, "unknown escape '\\%c'", esc);
        return -1;
    }
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int utf8_encode(uint32_t cp, char out[4]) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static void lex_string(Lex *l) {
    const char *start = l->p;
    adv(l); /* opening quote */
    Buf *sb = &l->lx->strbuf;
    size_t content_begin = sb->len;

    for (;;) {
        char c = peek(l);
        if (c == '\0' || c == '\n') {
            lex_error(l, start, "unterminated string");
            break;
        }
        if (c == '"') { adv(l); break; }
        if (c == '\\') {
            adv(l); /* backslash */
            char out[4];
            int n = decode_escape(l, adv(l), out);
            if (n > 0) buf_putn(sb, out, (size_t)n);
            continue;
        }
        /* UTF-8 continuation bytes pass through */
        buf_putc(sb, adv(l));
    }

    Token *t = emit(l, TOK_STRING, start);
    t->str_off = (uint32_t)content_begin;
    buf_putc(sb, '\0');   /* NUL terminator for convenience */
    t->text_len = (int)(sb->len - content_begin - 1);
}

static void lex_char(Lex *l) {
    const char *start = l->p;
    adv(l); /* opening quote */
    char out[4];
    int n = 0;
    char c = peek(l);
    if (c == '\'' || c == '\0' || c == '\n') {
        lex_error(l, start, "empty or unterminated char literal");
        if (c == '\'') adv(l);
        emit(l, TOK_CHAR, start);
        return;
    }
    if (c == '\\') {
        adv(l);
        n = decode_escape(l, adv(l), out);
        if (n < 0) n = 1, out[0] = '?';
    } else {
        out[0] = adv(l);
        n = 1;
        /* absorb UTF-8 continuation bytes */
        while (((unsigned char)peek(l) & 0xC0) == 0x80) out[n++] = adv(l);
    }
    if (peek(l) != '\'') {
        lex_error(l, start, "char literal must contain exactly one value");
        while (peek(l) && peek(l) != '\'' && peek(l) != '\n') adv(l);
    } else {
        adv(l);
    }
    Token *t = emit(l, TOK_CHAR, start);
    /* store decoded bytes into string buffer; pointers fixed up after lexing */
    Buf *sb = &l->lx->strbuf;
    t->str_off = (uint32_t)sb->len;
    buf_putn(sb, out, (size_t)n);
    buf_putc(sb, '\0');
    t->text_len = n;
    /* value: single byte → its value; multi-byte → UTF-8 code point-ish (bytes packed) */
    if (n == 1) t->v.char_val = (unsigned char)out[0];
    else {
        /* decode utf-8 back */
        uint32_t cp = (unsigned char)out[0];
        if ((cp & 0xE0) == 0xC0) cp = cp & 0x1F;
        else if ((cp & 0xF0) == 0xE0) cp = cp & 0x0F;
        else cp = cp & 0x07;
        for (int i = 1; i < n; i++) cp = (cp << 6) | ((unsigned char)out[i] & 0x3F);
        t->v.char_val = cp;
    }
}

/* ---- numbers ---- */

static void lex_number(Lex *l) {
    const char *start = l->p;
    Token dummy;
    Token *t = &dummy;
    bool is_float = false;
    int base = 10;

    if (peek(l) == '0' && (peek2(l) == 'x' || peek2(l) == 'X')) {
        base = 16; adv(l); adv(l);
        if (!is_hex(peek(l))) { lex_error(l, start, "hex literal needs digits after 0x"); }
    } else if (peek(l) == '0' && (peek2(l) == 'b' || peek2(l) == 'B')) {
        base = 2; adv(l); adv(l);
        if (peek(l) != '0' && peek(l) != '1') { lex_error(l, start, "binary literal needs digits after 0b"); }
    } else if (peek(l) == '0' && (peek2(l) == 'o' || peek2(l) == 'O')) {
        base = 8; adv(l); adv(l);
        if (peek(l) < '0' || peek(l) > '7') { lex_error(l, start, "octal literal needs digits after 0o"); }
    }

    uint64_t val = 0;
    bool any_digit = false, overflow = false;

    if (base != 10) {
        while (is_alnum_in_base(peek(l), base) || peek(l) == '_') {
            char c = adv(l);
            if (c == '_') continue;
            any_digit = true;
            int d = hexval(c);
            uint64_t nv = val * (uint64_t)base + (uint64_t)d;
            if (nv < val) overflow = true;   /* wrapped */
            val = nv;
        }
        if (!any_digit) { emit(l, TOK_INT, start); return; }  /* error already reported */
        /* 扫描停在数字上：这个底数里没有它（0b12 的 2）。不指出来，
         * 后面的后缀扫描会把 12 当成后缀，报"unknown suffix"——差得太远。 */
        if (is_digit(peek(l))) {
            lex_error(l, start, "invalid digit '%c' in %s literal", peek(l),
                      base == 2 ? "binary" : base == 8 ? "octal" : "hexadecimal");
            /* 一口气吃掉剩下的标识字符：不然后缀扫描又把 12 当后缀，
             * 多报一条"unknown suffix"——第一条已经说清了。 */
            while (is_ident_char(peek(l))) adv(l);
        }
    } else {
        while (is_digit(peek(l)) || peek(l) == '_') {
            char c = adv(l);
            if (c == '_') continue;
            any_digit = true;
            uint64_t nv = val * 10 + (uint64_t)(c - '0');
            if (nv < val) overflow = true;
            val = nv;
        }
        if (!any_digit) { emit(l, TOK_INT, start); return; }

        /* float? '.' must be followed by a digit (t.0.0 rule) */
        if (peek(l) == '.' && is_digit(peek2(l))) {
            is_float = true;
            adv(l); /* '.' */
            while (is_digit(peek(l)) || peek(l) == '_') adv(l);
            /* exponent */
            if (peek(l) == 'e' || peek(l) == 'E') {
                const char *save = l->p;
                int sline = l->line, scol = l->col;
                adv(l);
                if (peek(l) == '+' || peek(l) == '-') adv(l);
                if (is_digit(peek(l))) {
                    while (is_digit(peek(l))) adv(l);
                } else {
                    /* not an exponent; leave 'e' to the suffix scan (error later) */
                    l->p = save; l->line = sline; l->col = scol;
                }
            }
        }

        /* exponent without a decimal point: `1e7` == `1.0e7` (EBNF allows both,
         * so the lexer must accept both). Roll back if no digits follow. */
        if (peek(l) == 'e' || peek(l) == 'E') {
            const char *save = l->p;
            int sline = l->line, scol = l->col;
            adv(l);
            if (peek(l) == '+' || peek(l) == '-') adv(l);
            if (is_digit(peek(l))) {
                is_float = true;
                while (is_digit(peek(l))) adv(l);
            } else {
                l->p = save; l->line = sline; l->col = scol;
            }
        }
    }

    if (is_float) {
        /* parse via strtod on a scratch copy with underscores removed */
        char tmp[512];
        size_t n = (size_t)(l->p - start);
        if (n >= sizeof tmp) n = sizeof tmp - 1;
        size_t j = 0;
        for (size_t i = 0; i < n; i++) if (start[i] != '_') tmp[j++] = start[i];
        tmp[j] = '\0';
        t = emit(l, TOK_FLOAT, start);
        t->v.float_val = strtod(tmp, NULL);
    } else {
        t = emit(l, TOK_INT, start);
        t->v.int_val = overflow ? 0 : val;
        t->big_int = overflow;   /* comptime reads text; sema decides if it fits */
    }

    /* suffix */
    const char *suf_start = l->p;
    while (is_ident_char(peek(l))) adv(l);
    size_t slen = (size_t)(l->p - suf_start);
    if (slen == 0) return;

    static const char *int_sufs[]  = {"u8","u16","u32","u64","u128","i8","i16","i32","i64","i128","usize","isize"};
    static const char *float_sufs[] = {"f16","f32","f64","f128"};
    char suf[16];
    if (slen >= sizeof suf) { lex_error(l, start, "invalid number suffix"); return; }
    memcpy(suf, suf_start, slen); suf[slen] = '\0';

    if (is_float) {
        for (size_t i = 0; i < sizeof float_sufs / sizeof *float_sufs; i++)
            if (strcmp(suf, float_sufs[i]) == 0) return;
        lex_error(l, start, "invalid float suffix '%s'", suf);
    } else if (strcmp(suf, "e") == 0 || strcmp(suf, "E") == 0) {
        /* `1e` —— 指数后面没有数字，回滚后 e 被当成整数后缀；
         * 报"invalid suffix"不如直接说该写什么。 */
        lex_error(l, start, "float exponent needs digits: write `1e7` or `1.0e7`");
    } else {
        for (size_t i = 0; i < sizeof int_sufs / sizeof *int_sufs; i++)
            if (strcmp(suf, int_sufs[i]) == 0) return;
        lex_error(l, start, "invalid integer suffix '%s'", suf);
    }
}

static bool is_hex(char c) { return hexval(c) >= 0; }
static bool is_alnum_in_base(char c, int base) {
    int d = hexval(c);
    return d >= 0 && d < base;
}

/* ---- main token loop ---- */

static void lex_token(Lex *l) {
    const char *start = l->p;
    char c = adv(l);

    switch (c) {
    case '\0':
        emit(l, TOK_EOF_, start);
        return;

    /* punctuation */
    case '(': emit(l, TOK_LPAREN, start); return;
    case ')': emit(l, TOK_RPAREN, start); return;
    case '[': emit(l, TOK_LBRACKET, start); return;
    case ']': emit(l, TOK_RBRACKET, start); return;
    case '{': emit(l, TOK_LBRACE, start); return;
    case '}': emit(l, TOK_RBRACE, start); return;
    case ',': emit(l, TOK_COMMA, start); return;
    case ';': emit(l, TOK_SEMI, start); return;
    case '@': emit(l, TOK_AT, start); return;
    case '~': emit(l, TOK_TILDE, start); return;
    case '?': emit(l, TOK_QUESTION, start); return;

    case '.':
        if (peek(l) == '{') { adv(l); emit(l, TOK_DOT_BRACE, start); return; }
        if (peek(l) == '.') { adv(l); emit(l, TOK_DOTDOT, start); return; }
        /* '.' + digit: index — the NUMBER is lexed by the caller loop;
           here emit DOT and let the next iteration lex the int. */
        emit(l, TOK_DOT, start);
        return;

    case ':': emit(l, TOK_COLON, start); return;

    /* operators */
    case '+': if (peek(l) == '=') { adv(l); emit(l, TOK_PLUSEQ, start); } else emit(l, TOK_PLUS, start); return;
    case '-':
        if (peek(l) == '=') { adv(l); emit(l, TOK_MINUSEQ, start); return; }
        if (peek(l) == '>') { adv(l); emit(l, TOK_ARROW, start); return; }
        emit(l, TOK_MINUS, start); return;
    case '*': if (peek(l) == '=') { adv(l); emit(l, TOK_STAREQ, start); } else emit(l, TOK_STAR, start); return;
    case '/': if (peek(l) == '=') { adv(l); emit(l, TOK_SLASHEQ, start); } else emit(l, TOK_SLASH, start); return;
    case '%': if (peek(l) == '=') { adv(l); emit(l, TOK_PERCEQ, start); } else emit(l, TOK_PERCENT, start); return;
    case '^': if (peek(l) == '=') { adv(l); emit(l, TOK_CARETEQ, start); } else emit(l, TOK_CARET, start); return;

    case '&':
        if (peek(l) == '=') { adv(l); emit(l, TOK_AMPEQ, start); return; }
        if (peek(l) == '&') { adv(l); emit(l, TOK_ANDAND, start); return; }
        emit(l, TOK_AMP, start); return;

    case '|':
        if (peek(l) == '=') { adv(l); emit(l, TOK_PIPEEQ, start); return; }
        if (peek(l) == '|') { adv(l); emit(l, TOK_OROR, start); return; }
        emit(l, TOK_PIPE, start); return;

    case '=':
        if (peek(l) == '=') { adv(l); emit(l, TOK_EQ, start); return; }
        emit(l, TOK_ASSIGN, start); return;
    case '!':
        if (peek(l) == '=') { adv(l); emit(l, TOK_NE, start); return; }
        emit(l, TOK_BANG, start); return;

    case '<':
        if (peek(l) == '<') {
            adv(l);
            if (peek(l) == '=') { adv(l); emit(l, TOK_SHLEQ, start); } else emit(l, TOK_SHL, start);
            return;
        }
        if (peek(l) == '=') { adv(l); emit(l, TOK_LE, start); return; }
        emit(l, TOK_LT, start); return;

    case '>':
        if (peek(l) == '>') {
            adv(l);
            if (peek(l) == '=') { adv(l); emit(l, TOK_SHREQ, start); } else emit(l, TOK_SHR, start);
            return;
        }
        if (peek(l) == '=') { adv(l); emit(l, TOK_GE, start); return; }
        emit(l, TOK_GT, start); return;

    default: break;
    }

    if (is_digit(c)) { l->p = start; l->col--; lex_number(l); return; }
    if (is_ident_start(c)) {
        while (is_ident_char(peek(l))) adv(l);
        emit(l, ident_kind(start, (int)(l->p - start)), start);
        return;
    }
    if (c == '"') { l->p = start; l->col--; lex_string(l); return; }
    if (c == '\'') { l->p = start; l->col--; lex_char(l); return; }

    lex_error(l, start, "unexpected character '%c' (0x%02X)", c, (unsigned char)c);
}

void lexer_init(Lexer *lx, const char *src, size_t len) {
    memset(lx, 0, sizeof *lx);
    lx->src = src;
    lx->src_len = len;
    buf_init(&lx->strbuf);
}

void lexer_run(Lexer *lx) {
    Lex l = { .lx = lx, .p = lx->src, .line = 1, .col = 1 };
    for (;;) {
        skip_trivia(&l);
        if (peek(&l) == '\0' && l.p >= lx->src + lx->src_len) {
            emit(&l, TOK_EOF_, l.p);
            break;
        }
        if (peek(&l) == '\0') {
            /* embedded NUL in source: treat as error and stop */
            lex_error(&l, l.p, "unexpected NUL byte in source");
            break;
        }
        lex_token(&l);
        if (lx->ntoks > 0 && lx->toks[lx->ntoks - 1].kind == TOK_EOF_) break;
    }

    /* fix up string/char text pointers now that strbuf is stable */
    for (size_t i = 0; i < lx->ntoks; i++) {
        Token *t = &lx->toks[i];
        if (t->kind == TOK_STRING || t->kind == TOK_CHAR)
            t->text = lx->strbuf.data + t->str_off;
    }
}
