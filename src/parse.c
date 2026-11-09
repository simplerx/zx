/* parse.c — recursive-descent parser for Zx v0.5.
 *
 * The two things this parser exists to get right:
 *   1. `{ … }`  — block or tuple?  Decided purely lexically: scan the braces at
 *      depth 1 for `;` or a declaration/control keyword.  Forced-block positions
 *      (fn body, if arm, type body, switch body) skip the scan.
 *   2. `IDENT ":"` — label or yield?  Look at what follows: a construct
 *      (for/while/switch/if) → label it; a block → labeled block; anything else
 *      → yield `tag: value;`.
 */
#include "ast.h"
#include <setjmp.h>
#include <stdarg.h>

Node *parse_program(Lexer *lx);

/* ---------------- arena ---------------- */

static Node *node_alloc(NodeKind k, int line, int col) {
    Node *n = calloc(1, sizeof(Node));
    if (!n) { fprintf(stderr, "zxc: out of memory\n"); exit(1); }
    n->kind = k;
    n->line = line;
    n->col = col;
    return n;
}

Node *node_new(NodeKind k, int line, int col) { return node_alloc(k, line, col); }

static void push(Node *n, Node *item) {
    n->items = realloc(n->items, sizeof(Node *) * (size_t)(n->nitems + 1));
    n->items[n->nitems++] = item;
}

/* ---------------- parser state ---------------- */

typedef struct {
    Token  *t;
    size_t  n, i;
    int     last_line;
    size_t *match;   /* A1：括号配对表（下标=token 序号，值为配对括号序号）。
                      * 进 parser 前算一次 O(n)；cast 消歧用它做 O(1) 必要
                      * 条件检查，嵌套括号不再付试探 parse_type 的 O(n²)。 */
} P;

static jmp_buf g_spec;
static int     g_spec_depth = 0;
static const char *g_parse_fname;   /* 当前模块的源路径，供 @file() 捕获 */

static Token *cur(P *p) { return &p->t[p->i < p->n ? p->i : p->n - 1]; }
static Token *peek(P *p, size_t k) {
    size_t j = p->i + k;
    return &p->t[j < p->n ? j : p->n - 1];
}
static bool at(P *p, TokKind k) { return cur(p)->kind == k; }
static bool accept(P *p, TokKind k) {
    if (at(p, k)) { p->i++; return true; }
    return false;
}
static Token *advance(P *p) {
    Token *t = cur(p);
    if (p->i + 1 < p->n) p->i++;
    return t;
}

static void perr(P *p, const char *fmt, ...) {
    if (g_spec_depth > 0) longjmp(g_spec, 1);
    Token *t = cur(p);
    va_list ap;
    va_start(ap, fmt);
    char msg[512];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    /* 语法错的码由解析器自己定，不交给码表猜（语义层也有
     * "expected X, found Y" 形状的消息，那是类型错 E0301）。 */
    const char *code = strncmp(msg, "expected", 8) == 0 ? "E0201" : "E0202";
    diag_code_at(code, t->line, t->col, "%s", msg);
    /* crude recovery: don't advance here; callers usually expect() and move on */
}

static Token *expect(P *p, TokKind k) {
    if (at(p, k)) return advance(p);
    Token *t = cur(p);
    if (t->kind == TOK_IDENT || t->kind == TOK_INT || t->kind == TOK_FLOAT ||
        t->kind == TOK_STRING)
        perr(p, "expected %s, found %s `%.*s`", tok_kind_name(k),
             tok_kind_name(t->kind), t->len < 24 ? t->len : 24, t->start);
    else
        perr(p, "expected %s, found %s", tok_kind_name(k), tok_kind_name(t->kind));
    return t;   /* don't consume; let caller continue */
}

/* return interned-ish slice: we keep a pointer into source + length is lost,
 * so copy into a stable buffer. */
static const char *dup_tok(Token *t) {
    char *s = malloc((size_t)t->len + 1);
    memcpy(s, t->start, (size_t)t->len);
    s[t->len] = '\0';
    return s;
}

static const char *take_ident(P *p) {
    if (at(p, TOK_IDENT)) return dup_tok(advance(p));
    perr(p, "expected identifier, found %s", tok_kind_name(cur(p)->kind));
    return "<error>";
}

/* ---------------- lookahead helpers ---------------- */

/* true / false 在 lex 里是标识符（故意不设关键字），但它们在 switch 臂里
 * 是**模式**，不是臂标签：`true: { … },` 不能被读成"给块加标签 true"。 */
static bool is_bool_word(Token *t) {
    if (!t || t->kind != TOK_IDENT) return false;
    return (t->len == 4 && !memcmp(t->start, "true", 4)) ||
           (t->len == 5 && !memcmp(t->start, "false", 5));
}

/* 这个 token 能不能作为一个 switch 模式的**开头**（用来判断模式列表里的
 * 逗号是"还有一个模式"还是别的）。
 *
 * 能开头的不多：`.成员`、整数字面量、char 字面量（v0.25）、`true`/`false`
 * （它们在 lexer 里是标识符）、负号、以及 `(` 开头的转换式模式 `(error)Err.x`。
 * 只认这几种，别的一律当成"模式列表结束"——报错落在 expect(TOK_COLON) 上，
 * 信息比"expected a pattern"更贴近实际写错的地方。 */
static bool starts_pattern(Token *t) {
    if (!t) return false;
    switch (t->kind) {
    case TOK_DOT: case TOK_INT: case TOK_CHAR: case TOK_MINUS: case TOK_LPAREN:
        return true;
    default:
        return is_bool_word(t);
    }
}

/* Is the `{` at index i a block (true) or a tuple literal (false)? */
static bool brace_is_block(P *p, size_t i) {
    int depth = 0;
    for (size_t j = i; j < p->n; j++) {
        TokKind k = p->t[j].kind;
        switch (k) {
        case TOK_LBRACE: case TOK_DOT_BRACE: depth++; break;
        case TOK_RBRACE:
            depth--;
            if (depth == 0) return false;   /* scanned whole body, saw no ';' */
            break;
        case TOK_SEMI:
            if (depth == 1) return true;
            break;
        case TOK_KW_CONST: case TOK_KW_LET: case TOK_KW_VAR: case TOK_KW_FN:
        case TOK_KW_PUB:
        case TOK_KW_DEFER: case TOK_KW_RETURN: case TOK_KW_CONTINUE:
        case TOK_KW_IF: case TOK_KW_WHILE: case TOK_KW_FOR: case TOK_KW_SWITCH:
            if (depth == 1) return true;
            break;
        default: break;
        }
    }
    return false;
}

/* tokens that, right after a `(T)` cast candidate, mean "this is a cast" */
static bool can_follow_cast(TokKind k) {
    switch (k) {
    case TOK_IDENT: case TOK_INT: case TOK_FLOAT: case TOK_STRING: case TOK_CHAR:
    case TOK_LBRACE: case TOK_DOT_BRACE: case TOK_AMP: case TOK_LPAREN:
    case TOK_KW_NULL: case TOK_KW_UNDEF: case TOK_BANG: case TOK_TILDE:
    case TOK_KW_IF: case TOK_KW_MODULE:
    case TOK_AT:            /* (T)@builtin(...) — `@` 永远不是中缀运算符，无歧义 */
        return true;
    default: return false;
    }
}

/* ---------------- types ---------------- */

static Node *parse_type(P *p);
static Node *parse_expr(P *p);
static Node *parse_block(P *p);
static Node *parse_primary(P *p);
static Node *parse_decl(P *p);
static Node *parse_fn(P *p, bool allow_name);
static Node *parse_stmt(P *p);
static Node *parse_construct(P *p, const char *label);
static Node *parse_for(P *p, const char *label, bool allow_nobind);
static Node *parse_yield(P *p);

static Node *type_ident(const char *name, int line, int col) {
    Node *n = node_new(NK_TYPE_IDENT, line, col);
    n->name = name;
    return n;
}

/* fold `IDENT . IDENT . …` into a field chain (for union tags like ShapeTag.circle) */
static Node *parse_type_or_const(P *p) {
    /* speculative type parse; on failure fall back to expression */
    size_t save = p->i;
    jmp_buf saveBuf;
    memcpy(&saveBuf, &g_spec, sizeof(jmp_buf));
    int savedepth = g_spec_depth;
    g_spec_depth++;
    if (setjmp(g_spec) == 0) {
        Node *t = parse_type(p);
        TokKind after = cur(p)->kind;
        bool clean = after == TOK_COMMA || after == TOK_RPAREN ||
                     after == TOK_RBRACE || after == TOK_RBRACKET ||
                     after == TOK_SEMI || after == TOK_ASSIGN ||
                     after == TOK_COLON || after == TOK_EOF_;
        if (clean) { g_spec_depth = savedepth; return t; }
        longjmp(g_spec, 1);          /* looked like a type but isn't */
    }
    g_spec_depth = savedepth;
    memcpy(&g_spec, &saveBuf, sizeof(jmp_buf));
    p->i = save;
    return parse_expr(p);
}

static Node *parse_type_primary(P *p) {
    Token *t = cur(p);
    switch (t->kind) {
    case TOK_IDENT: {
        advance(p);
        Node *base = type_ident(dup_tok(t), t->line, t->col);
        for (;;) {
            while (at(p, TOK_LPAREN)) {
                Token *lp = advance(p);
                Node *app = node_new(NK_CALL, lp->line, lp->col);
                app->a = base;
                if (!at(p, TOK_RPAREN)) {
                    for (;;) {
                        push(app, parse_type_or_const(p));
                        if (!accept(p, TOK_COMMA)) break;
                    }
                }
                expect(p, TOK_RPAREN);
                base = app;
            }
            /* 点路径：std.HashMap / std.TypeInfo 这类"模块里的类型"，
             * 和泛型实参交错（std.HashMap(i32).Entry 也要能写） */
            if (!at(p, TOK_DOT)) break;
            Token *dot = advance(p);
            (void)dot;
            const char *nm = take_ident(p);
            Node *f = node_new(NK_FIELD, cur(p)->line, cur(p)->col);
            f->a = base;
            f->name = nm;
            base = f;
        }
        return base;
    }

    case TOK_KW_ERROR: {
        advance(p);
        /* `error{…}` 才是错误集字面量。可 `fn () error { … }` 的 `{` 长得一模一样
         * ——它开的是**函数体**。两 token 前瞻区分：`{` 后面是 `}`，或者
         * 「标识符 / null + `,` 或 `}`」，才是集合；否则（`return`、`let`、
         * `if`、`foo(`…）就把花括号留给调用方（函数体/块），这里只吐裸 `error`，
         * 由 sema 用 E0308 说清楚"光秃秃的 error 不是返回类型"。 */
        bool errset = false;
        if (at(p, TOK_LBRACE)) {
            TokKind k1 = peek(p, 1)->kind;
            TokKind k2 = peek(p, 2)->kind;
            if (k1 == TOK_RBRACE) errset = true;
            else if ((k1 == TOK_IDENT || k1 == TOK_KW_NULL) &&
                     (k2 == TOK_COMMA || k2 == TOK_RBRACE)) errset = true;
        }
        if (errset) {                            /* error{a, b}：错误集类型 */
            advance(p);
            Node *n = node_new(NK_ERR_SET, t->line, t->col);
            while (!at(p, TOK_RBRACE) && !at(p, TOK_EOF_)) {
                Node *m = node_new(NK_PAIR, cur(p)->line, cur(p)->col);
                // null 也是合法的错误名（用户 2026-11-09：`null` 本身就是
                // 一种错误，`error{null}` / `error.null` 都要能写）
                if (at(p, TOK_KW_NULL)) {
                    advance(p);
                    m->name = "null";
                } else {
                    m->name = take_ident(p);
                }
                push(n, m);
                if (!accept(p, TOK_COMMA)) break;
            }
            expect(p, TOK_RBRACE);
            return n;
        }
        return type_ident("error", t->line, t->col);
    }

    case TOK_KW_TYPE:
        advance(p);
        return type_ident("type", t->line, t->col);

    case TOK_LBRACE: {
        /* tuple type {T1, T2} — elements may also be comptime consts (union tags) */
        advance(p);
        Node *n = node_new(NK_TUPLE_TYPE, t->line, t->col);
        if (!at(p, TOK_RBRACE)) {
            for (;;) {
                push(n, parse_type_or_const(p));
                if (!accept(p, TOK_COMMA)) break;
                if (at(p, TOK_RBRACE)) break;
            }
        }
        expect(p, TOK_RBRACE);
        return n;
    }

    case TOK_KW_STRUCT: {
        advance(p);
        Node *n = node_new(NK_STRUCT_TYPE, t->line, t->col);
        if (accept(p, TOK_LPAREN)) {
            const char *mode = take_ident(p);
            if (strcmp(mode, "packed") == 0) n->flag = true;
            expect(p, TOK_RPAREN);
        }
        /* body: fields, methods, decls */
        expect(p, TOK_LBRACE);
        while (!at(p, TOK_RBRACE) && !at(p, TOK_EOF_)) {
            /* 方法只有一种写法：`const name = fn (…) R { … };`（v0.24）。
             * 解析成带名字的 NK_FN——方法表吃的一直是这个形状，语义层
             * 不用为"两种写法"分叉。 */
            if (at(p, TOK_KW_CONST) && peek(p, 1)->kind == TOK_IDENT &&
                peek(p, 2)->kind == TOK_ASSIGN && peek(p, 3)->kind == TOK_KW_FN) {
                advance(p);                      /* const */
                const char *mname = take_ident(p);
                advance(p);                      /* '=' */
                Node *f = parse_fn(p, true);
                f->name = mname;
                expect(p, TOK_SEMI);
                push(n, f);
                continue;
            }
            if (at(p, TOK_KW_CONST) || at(p, TOK_KW_LET) || at(p, TOK_KW_VAR)) {
                push(n, parse_decl(p));
                continue;
            }
            if (at(p, TOK_KW_FN)) { push(n, parse_fn(p, false)); continue; }
            /* 位置取字段名，不取 `struct` 关键字：字段相关的诊断（重名、
             * 默认值类型不对、递归包含）都该指在那一行那一列上。 */
            int fline = cur(p)->line, fcol = cur(p)->col;
            const char *fname = take_ident(p);
            expect(p, TOK_COLON);
            Node *ty = parse_type(p);
            Node *f = node_new(NK_PARAM, fline, fcol);
            f->name = fname;
            f->a = ty;
            if (accept(p, TOK_ASSIGN)) f->b = parse_expr(p);
            accept(p, TOK_COMMA);
            push(n, f);
        }
        expect(p, TOK_RBRACE);
        return n;
    }

    case TOK_KW_UNION: {
        advance(p);
        Node *n = node_new(NK_UNION_TYPE, t->line, t->col);
        /* union(enum) / union(enum(u8))：Zig 式带标签联合体（M22）。
         * 变体名即隐式枚举，标签值按声明顺序 0..n-1。op=1 是糖标记。 */
        if (accept(p, TOK_LPAREN)) {
            expect(p, TOK_KW_ENUM);
            if (accept(p, TOK_LPAREN)) {
                n->b = parse_type(p);
                expect(p, TOK_RPAREN);
            }
            expect(p, TOK_RPAREN);
            n->op = 1;
        }
        expect(p, TOK_LBRACE);
        while (!at(p, TOK_RBRACE) && !at(p, TOK_EOF_)) {
            /* 方法只有一种写法：`const name = fn (…) R { … };`（v0.24）。
             * 解析成带名字的 NK_FN——方法表吃的一直是这个形状，语义层
             * 不用为"两种写法"分叉。 */
            if (at(p, TOK_KW_CONST) && peek(p, 1)->kind == TOK_IDENT &&
                peek(p, 2)->kind == TOK_ASSIGN && peek(p, 3)->kind == TOK_KW_FN) {
                advance(p);                      /* const */
                const char *mname = take_ident(p);
                advance(p);                      /* '=' */
                Node *f = parse_fn(p, true);
                f->name = mname;
                expect(p, TOK_SEMI);
                push(n, f);
                continue;
            }
            if (at(p, TOK_KW_CONST) || at(p, TOK_KW_LET) || at(p, TOK_KW_VAR)) {
                push(n, parse_decl(p));
                continue;
            }
            if (at(p, TOK_KW_FN)) { push(n, parse_fn(p, false)); continue; }
            int mline = cur(p)->line, mcol = cur(p)->col;
            const char *mname = take_ident(p);
            expect(p, TOK_COLON);
            Node *ty = parse_type(p);
            Node *m = node_new(NK_PAIR, mline, mcol);
            m->name = mname;
            m->a = ty;
            accept(p, TOK_COMMA);
            push(n, m);
        }
        expect(p, TOK_RBRACE);
        return n;
    }

    case TOK_KW_ENUM: {
        advance(p);
        Node *n = node_new(NK_ENUM_TYPE, t->line, t->col);
        if (accept(p, TOK_LPAREN)) {
            n->a = parse_type(p);
            expect(p, TOK_RPAREN);
        }
        expect(p, TOK_LBRACE);
        while (!at(p, TOK_RBRACE) && !at(p, TOK_EOF_)) {
            /* 体也是作用域：方法 / 常量成员和变体混着写 */
            /* 方法只有一种写法：`const name = fn (…) R { … };`（v0.24）。
             * 解析成带名字的 NK_FN——方法表吃的一直是这个形状，语义层
             * 不用为"两种写法"分叉。 */
            if (at(p, TOK_KW_CONST) && peek(p, 1)->kind == TOK_IDENT &&
                peek(p, 2)->kind == TOK_ASSIGN && peek(p, 3)->kind == TOK_KW_FN) {
                advance(p);                      /* const */
                const char *mname = take_ident(p);
                advance(p);                      /* '=' */
                Node *f = parse_fn(p, true);
                f->name = mname;
                expect(p, TOK_SEMI);
                push(n, f);
                continue;
            }
            if (at(p, TOK_KW_CONST) || at(p, TOK_KW_LET) || at(p, TOK_KW_VAR)) {
                push(n, parse_decl(p));
                continue;
            }
            if (at(p, TOK_KW_FN)) { push(n, parse_fn(p, false)); continue; }
            Node *m = node_new(NK_PAIR, cur(p)->line, cur(p)->col);
            m->name = take_ident(p);
            if (accept(p, TOK_ASSIGN)) m->a = parse_expr(p);
            accept(p, TOK_COMMA);
            push(n, m);
        }
        expect(p, TOK_RBRACE);
        return n;
    }

    case TOK_LPAREN: {
        advance(p);
        Node *n = parse_type(p);
        expect(p, TOK_RPAREN);
        return n;
    }

    default:
        if (g_spec_depth > 0) {
            perr(p, "expected a type, found %s", tok_kind_name(t->kind));
            longjmp(g_spec, 1);
        }
        perr(p, "expected a type, found %s", tok_kind_name(t->kind));
        advance(p);
        return type_ident("<error>", t->line, t->col);
    }
}

static Node *parse_type_prefix(P *p);

/* `E!T`：先把前缀类型读完，再看后面有没有 `!`——有就说明左边那个是
 * 错误集（`error{open}`、或它的别名），右边是载荷类型。`!T`（没有集合）
 * 仍然走前缀分支，两条写法共用 NK_ERR_TYPE，区别只看 b 有没有值。 */
static Node *parse_type(P *p) {
    Node *t = parse_type_prefix(p);
    if (at(p, TOK_BANG)) {
        Token *b = advance(p);
        Node *n = node_new(NK_ERR_TYPE, b->line, b->col);
        n->a = t;
        n->b = parse_type(p);
        return n;
    }
    return t;
}

static Node *parse_type_prefix(P *p) {
    Token *t = cur(p);

    switch (t->kind) {
    case TOK_STAR: {
        advance(p);
        Node *n = node_new(NK_PTR_TYPE, t->line, t->col);
        if (accept(p, TOK_KW_CONST)) n->flag = true;      /* *const T */
        /* `*[N]T` 是指向数组的指针；`*[]T` 是指向切片的指针。
         * 两者的 `[` 后面长得一样，只有 `]` 紧跟着才说明数目为空。 */
        if (at(p, TOK_LBRACKET) && peek(p, 1)->kind != TOK_RBRACKET) {
            advance(p);
            n->b = parse_expr(p);
            expect(p, TOK_RBRACKET);
        }
        n->a = parse_type(p);
        return n;
    }

    case TOK_LBRACKET: {
        advance(p);
        expect(p, TOK_RBRACKET);
        Node *n = node_new(NK_SLICE_TYPE, t->line, t->col);
        if (accept(p, TOK_KW_CONST)) n->flag = true;
        n->a = parse_type(p);
        return n;
    }

    case TOK_QUESTION: {
        advance(p);
        Node *n = node_new(NK_OPT_TYPE, t->line, t->col);
        n->a = parse_type(p);
        return n;
    }

    case TOK_BANG: {
        advance(p);
        Node *n = node_new(NK_ERR_TYPE, t->line, t->col);
        n->a = parse_type(p);
        return n;
    }

    case TOK_KW_FN: {
        advance(p);
        Node *n = node_new(NK_FN, t->line, t->col);
        n->flag = true;   /* flag: this is a fn TYPE unless a body follows */
        expect(p, TOK_LPAREN);
        if (!at(p, TOK_RPAREN)) {
            for (;;) {
                Node *pm = node_new(NK_PARAM, cur(p)->line, cur(p)->col);
                if (at(p, TOK_IDENT) && peek(p, 1)->kind == TOK_COLON) {
                    pm->name = take_ident(p);
                    expect(p, TOK_COLON);
                }
                pm->a = parse_type(p);
                push(n, pm);
                if (!accept(p, TOK_COMMA)) break;
            }
        }
        expect(p, TOK_RPAREN);
        if (!at(p, TOK_LBRACE) && !at(p, TOK_SEMI) && !at(p, TOK_COMMA) &&
            !at(p, TOK_RPAREN) && !at(p, TOK_RBRACKET) && !at(p, TOK_RBRACE) &&
            !at(p, TOK_ASSIGN))
            n->a = parse_type(p);          /* return type */
        if (at(p, TOK_LBRACE)) {           /* actually a fn literal with body */
            n->flag = false;
            n->b = parse_block(p);
        }
        return n;
    }

    case TOK_KW_EXTERN: {
        /* extern struct / extern union（FFI）：类型前缀形式，配合
         * `const Vec3 = extern struct { … };` 使用——和普通类型声明
         * `const P = struct { … }` 同一套"类型即值"的习惯。
         * 字段的 C 布局约束由 sema 校验（resolve_struct_body）。 */
        advance(p);
        if (at(p, TOK_STRING)) {           /* extern "C" struct … */
            Token *ab = advance(p);
            if (!(ab->text_len == 1 && ab->text[0] == 'C'))
                perr(p, "only `extern \"C\"` is supported for now");
        }
        if (!at(p, TOK_KW_STRUCT) && !at(p, TOK_KW_UNION)) {
            perr(p, "expected `struct` or `union` after `extern` "
                    "(functions use `extern fn name(params) Ret;`)");
            return node_new(NK_STRUCT_TYPE, t->line, t->col);
        }
        Node *n = parse_type_primary(p);
        if (n) n->is_extern = true;
        return n;
    }

    default:
        return parse_type_primary(p);
    }
}

/* ---------------- expressions ---------------- */

/* 优先级：数字越大绑得越紧。`orelse` 只比赋值高一层——`a orelse b + 1`
 * 读作 `a orelse (b + 1)`，`a orelse b == c` 读作 `(a orelse b) == c`。
 * 与 Zig 一致：它是"取个值回来"，不该比四则运算还黏。 */
static int bin_prec(TokKind k) {
    switch (k) {
    case TOK_KW_ORELSE: return 1;
    case TOK_OROR: return 2;
    case TOK_ANDAND: return 3;
    case TOK_EQ: case TOK_NE: return 4;
    case TOK_LT: case TOK_LE: case TOK_GT: case TOK_GE: return 5;
    case TOK_PIPE: return 6;
    case TOK_CARET: return 7;
    case TOK_AMP: return 8;
    case TOK_SHL: case TOK_SHR: return 9;
    case TOK_PLUS: case TOK_MINUS: return 10;
    case TOK_STAR: case TOK_SLASH: case TOK_PERCENT: return 11;
    default: return 0;
    }
}

static bool is_assign_op(TokKind k) {
    switch (k) {
    case TOK_ASSIGN: case TOK_PLUSEQ: case TOK_MINUSEQ: case TOK_STAREQ:
    case TOK_SLASHEQ: case TOK_PERCEQ: case TOK_AMPEQ: case TOK_PIPEEQ:
    case TOK_CARETEQ: case TOK_SHLEQ: case TOK_SHREQ:
        return true;
    default: return false;
    }
}

static Node *parse_expr(P *p);
static Node *parse_unary(P *p);

static Node *parse_binary_rest(P *p, Node *lhs, int min_prec) {
    int chain = 0;
    for (;;) {
        TokKind k = cur(p)->kind;
        int prec = bin_prec(k);
        if (prec == 0 || prec < min_prec) break;
        /* 同层运算符连成左深链，链长 = AST 深度；序列化/装载/检查都是递归的，
         * 8000 个 `+` 会把栈撑爆（无声退出）。与 Zx 前端同一条上限。 */
        if (++chain > 2048) {
            err_at(cur(p)->line, cur(p)->col,
                   "an expression chains over 2048 operators -- split it into several statements");
            break;
        }
        advance(p);
        Node *rhs = parse_binary_rest(p, parse_unary(p), prec + 1);
        /* orelse 不是算术：它自带一个节点（语义是"空则取右边"，
         * 后端要短路，不能当普通二元运算发） */
        Node *n = node_new(k == TOK_KW_ORELSE ? NK_ORELSE : NK_BIN, lhs->line, lhs->col);
        n->op = (int)k;
        n->a = lhs;
        n->b = rhs;
        lhs = n;
    }
    return lhs;
}

static Node *parse_binary(P *p, int min_prec) {
    return parse_binary_rest(p, parse_unary(p), min_prec);
}

static Node *parse_expr(P *p) {
    Node *lhs = parse_binary(p, 1);
    if (is_assign_op(cur(p)->kind)) {
        TokKind k = cur(p)->kind;
        advance(p);
        Node *rhs = parse_expr(p);   /* right-assoc */
        Node *n = node_new(NK_BIN, lhs->line, lhs->col);
        n->op = (int)k;
        n->a = lhs;
        n->b = rhs;
        return n;
    }
    return lhs;
}

/* 只有这些形状后面才可能是调用：`f(x)`、`a.b(x)`、`xs[i](x)`、`f()(x)`。
 * 控制流表达式（`if (c) { … }`）不是可以调用的值——它后面那个 `(` 只能是
 * **下一句**的开头。不这么判的话，`if (…) { … }` 换行写 `(p + 1).0 = v;`
 * 会被读成「调用 if 的结果」。 */
static bool callable_shape(const Node *n) {
    if (!n) return false;
    switch (n->kind) {
    case NK_IDENT: case NK_TYPE_IDENT: case NK_FIELD: case NK_INDEX:
    case NK_CALL: case NK_CAST: case NK_BUILTIN: case NK_MODULE:
        return true;
    default: return false;
    }
}

static Node *parse_postfix(P *p, Node *base) {
    for (;;) {
        Token *t = cur(p);
        if (t->kind == TOK_DOT) {
            /* `.?` —— 解包；空/错则运行时 trap。`.` 后面只可能是名字、下标
             * 或 `?`，三者不冲突，所以紧跟 `?` 就是解包。 */
            if (peek(p, 1)->kind == TOK_QUESTION) {
                advance(p);
                advance(p);
                Node *u = node_new(NK_UNWRAP, t->line, t->col);
                u->a = base;
                base = u;
                continue;
            }
            advance(p);
            if (at(p, TOK_IDENT)) {
                Token *id = advance(p);
                /* 位置记**成员名**而不是点号：`o.coutn` 的诊断要指在 coutn 上，
                 * 指在 `.` 上编辑器里既点不中、也没法做整词高亮。 */
                Node *n = node_new(NK_FIELD, id->line, id->col);
                n->a = base;
                n->name = dup_tok(id);
                base = n;
            } else if (at(p, TOK_KW_NULL) && base->kind == NK_TYPE_IDENT) {
                /* `error.null` / `Err.null`：错误名可以叫 null（用户
                 * 2026-11-09：null 本身是一种错误）。只在类型值的成员位置
                 * 放宽——`o.null` 这种值上的字段访问仍按错拼处理。 */
                Token *id = advance(p);
                Node *n = node_new(NK_FIELD, id->line, id->col);
                n->a = base;
                n->name = "null";
                base = n;
            } else if (at(p, TOK_INT)) {
                Token *ix = advance(p);
                Node *n = node_new(NK_FIELD, ix->line, ix->col);
                n->a = base;
                n->ival = ix->v.int_val;
                n->flag = false;   /* flag=false → index access */
                base = n;
            } else {
                perr(p, "expected field name or index after '.'");
                return base;
            }
            continue;
        }
        if (t->kind == TOK_LBRACKET) {
            advance(p);
            Node *first = parse_expr(p);
            if (accept(p, TOK_DOTDOT)) {
                Node *n = node_new(NK_SLICE, t->line, t->col);
                n->a = base;
                n->b = first;                       /* lo */
                if (!at(p, TOK_RBRACKET)) n->c = parse_expr(p);   /* hi */
                expect(p, TOK_RBRACKET);
                base = n;
            } else {
                Node *n = node_new(NK_INDEX, t->line, t->col);
                n->a = base;
                n->b = first;
                expect(p, TOK_RBRACKET);
                base = n;
            }
            continue;
        }
        if (t->kind == TOK_LPAREN && callable_shape(base)) {
            advance(p);
            Node *n = node_new(NK_CALL, t->line, t->col);
            n->a = base;
            if (!at(p, TOK_RPAREN)) {
                for (;;) {
                    /* 普通调用的实参是表达式；只有类型应用 Ident(…) 与
                     * 内建 @f(…) 才把裸标识符优先当类型看。 */
                    push(n, parse_expr(p));
                    if (!accept(p, TOK_COMMA)) break;
                }
            }
            expect(p, TOK_RPAREN);
            base = n;
            continue;
        }
        /* Typed literal: Type{ .field = e }  — only when the brace body starts
         * with `.name =`, which no block ever does. */
        if (t->kind == TOK_LBRACE &&
            (base->kind == NK_IDENT || base->kind == NK_TYPE_IDENT ||
             base->kind == NK_FIELD || base->kind == NK_STRUCT_TYPE ||
             base->kind == NK_CALL)) {
            bool is_lit = false;
            if (peek(p, 1)->kind == TOK_RBRACE) is_lit = true;
            else if (peek(p, 1)->kind == TOK_DOT && peek(p, 2)->kind == TOK_IDENT &&
                     peek(p, 3)->kind == TOK_ASSIGN)
                is_lit = true;
            if (is_lit) {
                advance(p);
                Node *n = node_new(NK_STRUCT_LIT, t->line, t->col);
                n->a = base;
                while (!at(p, TOK_RBRACE) && !at(p, TOK_EOF_)) {
                    Node *pair = node_new(NK_PAIR, cur(p)->line, cur(p)->col);
                    expect(p, TOK_DOT);
                    pair->name = take_ident(p);
                    expect(p, TOK_ASSIGN);
                    pair->a = parse_expr(p);
                    push(n, pair);
                    if (!accept(p, TOK_COMMA)) break;
                }
                expect(p, TOK_RBRACE);
                base = n;
                continue;
            }
        }
        break;
    }
    return base;
}

/* `{ … }` in expression position: tuple literal, or block */
static Node *parse_brace_expr(P *p) {
    Token *t = cur(p);
    if (brace_is_block(p, p->i)) return parse_block(p);

    advance(p);   /* '{' */
    Node *n = node_new(NK_TUPLE, t->line, t->col);
    if (!at(p, TOK_RBRACE)) {
        for (;;) {
            if (at(p, TOK_DOT) && peek(p, 1)->kind == TOK_IDENT &&
                peek(p, 2)->kind == TOK_ASSIGN) {
                /* mixed forms are an error, but keep going for better messages */
                perr(p, "tuple literal cannot contain field initializers");
            }
            push(n, parse_expr(p));
            if (!accept(p, TOK_COMMA)) break;
            if (at(p, TOK_RBRACE)) break;
        }
    }
    expect(p, TOK_RBRACE);
    return n;
}

/* `.{ … }` — always a literal */
static Node *parse_dot_brace(P *p) {
    Token *t = advance(p);
    bool is_struct = false;
    if (at(p, TOK_DOT) && peek(p, 1)->kind == TOK_IDENT && peek(p, 2)->kind == TOK_ASSIGN)
        is_struct = true;

    if (is_struct) {
        Node *n = node_new(NK_ANON_STRUCT, t->line, t->col);
        while (!at(p, TOK_RBRACE) && !at(p, TOK_EOF_)) {
            Node *pair = node_new(NK_PAIR, cur(p)->line, cur(p)->col);
            expect(p, TOK_DOT);
            pair->name = take_ident(p);
            expect(p, TOK_ASSIGN);
            pair->a = parse_expr(p);
            push(n, pair);
            if (!accept(p, TOK_COMMA)) break;
        }
        expect(p, TOK_RBRACE);
        return n;
    }

    Node *n = node_new(NK_TUPLE, t->line, t->col);
    if (!at(p, TOK_RBRACE)) {
        for (;;) {
            push(n, parse_expr(p));
            if (!accept(p, TOK_COMMA)) break;
            if (at(p, TOK_RBRACE)) break;
        }
    }
    expect(p, TOK_RBRACE);
    return n;
}

static Node *parse_unary(P *p) {
    Token *t = cur(p);
    switch (t->kind) {
    case TOK_AMP:
        advance(p);
        if (at(p, TOK_DOT_BRACE)) {
            /* &.{ … } — array literal: one anonymous storage block */
            Node *lit = parse_dot_brace(p);
            Node *n = node_new(NK_ARRAY_LIT, t->line, t->col);
            n->items = lit->items;
            n->nitems = lit->nitems;
            return n;
        }
        {
            Node *n = node_new(NK_UN, t->line, t->col);
            n->op = TOK_AMP;
            n->a = parse_unary(p);
            return n;
        }
    case TOK_MINUS: case TOK_PLUS: case TOK_BANG: case TOK_TILDE: {
        advance(p);
        Node *n = node_new(NK_UN, t->line, t->col);
        n->op = (int)t->kind;
        n->a = parse_unary(p);
        return n;
    }
    case TOK_KW_TRY: {
        /* `try e`：e 是 !T，成功取载荷，出错带着同一个码返回。
         * 它只是 `if (e) v { v } else err { return err; }` 的简写——
         * 没有栈展开、没有 catch 块，错误值始终在返回值里。 */
        advance(p);
        Node *n = node_new(NK_TRY, t->line, t->col);
        n->a = parse_unary(p);
        return n;
    }
    default:
        return parse_postfix(p, parse_primary(p));
    }
}

static Node *parse_primary(P *p) {
    Token *t = cur(p);

    switch (t->kind) {
    case TOK_INT: {
        advance(p);
        Node *n = node_new(NK_INT, t->line, t->col);
        n->ival = t->v.int_val;
        n->flag = t->big_int;
        n->str = dup_tok(t);   /* keep the text for big literal / suffix handling */
        return n;
    }
    case TOK_FLOAT:
        advance(p);
        {
            Node *n = node_new(NK_FLOAT, t->line, t->col);
            n->fval = t->v.float_val;
            n->str = dup_tok(t);
            return n;
        }
    case TOK_STRING: {
        advance(p);
        Node *n = node_new(NK_STR, t->line, t->col);
        n->str = t->text;
        n->str_len = t->text_len;
        return n;
    }
    case TOK_CHAR: {
        advance(p);
        Node *n = node_new(NK_CHAR, t->line, t->col);
        n->ival = t->v.char_val;
        n->str = t->text;
        n->str_len = t->text_len;
        return n;
    }
    case TOK_KW_NULL: advance(p); return node_new(NK_NULL, t->line, t->col);
    case TOK_KW_UNDEF: advance(p); return node_new(NK_UNDEF, t->line, t->col);

    case TOK_IDENT: {
        /* label on a block/construct in expression position:  body: { … }
         * `true` / `false` 除外——它们是 bool 字面量，不是标签。 */
        if (peek(p, 1)->kind == TOK_COLON && !is_bool_word(t)) {
            TokKind after = peek(p, 2)->kind;
            if (after == TOK_LBRACE && !brace_is_block(p, p->i + 2)) {
                /* 花括号是值（`{}` / `{a, b}`）而不是块：这是收割，
                 * 与语句位置同一套判定。 */
                return parse_yield(p);
            }
            if (after == TOK_LBRACE || after == TOK_KW_FOR || after == TOK_KW_WHILE ||
                after == TOK_KW_SWITCH || after == TOK_KW_IF) {
                const char *label = dup_tok(advance(p));
                advance(p);   /* ':' */
                return parse_construct(p, label);
            }
        }
        advance(p);
        if (t->len == 4 && !memcmp(t->start, "true", 4)) {
            Node *b = node_new(NK_BOOL, t->line, t->col);
            b->ival = 1;
            return b;
        }
        if (t->len == 5 && !memcmp(t->start, "false", 5))
            return node_new(NK_BOOL, t->line, t->col);
        Node *n = node_new(NK_IDENT, t->line, t->col);
        n->name = dup_tok(t);
        return n;
    }

    case TOK_LPAREN: {
        /* cast (T)e  vs  parenthesized expr (e) */
        /* A1：先做 O(1) 必要条件检查再试探——真 cast 的 `)` 一定是括号
         * 配对的那个（类型里括号自平衡），且其后必须能跟 cast。不满足
         * 就直接走表达式路径：嵌套括号下，试探 parse_type 每层都要把
         * 剩余 token 全部走一遍，O(n²) 就是这么来的。 */
        size_t close = p->match ? p->match[p->i] : SIZE_MAX;
        bool maybe_cast = close != SIZE_MAX && close + 1 < p->n &&
                          can_follow_cast(p->t[close + 1].kind);
        if (!maybe_cast) {
            advance(p);   /* '(' */
            Node *e = parse_expr(p);
            expect(p, TOK_RPAREN);
            return e;
        }
        size_t save = p->i;
        jmp_buf saveBuf;
        memcpy(&saveBuf, &g_spec, sizeof(jmp_buf));
        int savedepth = g_spec_depth;
        g_spec_depth++;
        if (setjmp(g_spec) == 0) {
            advance(p);   /* '(' */
            Node *ty = parse_type(p);
            if (at(p, TOK_RPAREN) && can_follow_cast(peek(p, 1)->kind)) {
                advance(p);   /* ')' */
                g_spec_depth = savedepth;
                Node *n = node_new(NK_CAST, t->line, t->col);
                n->a = ty;
                n->b = parse_unary(p);
                return n;
            }
        }
        g_spec_depth = savedepth;
        memcpy(&g_spec, &saveBuf, sizeof(jmp_buf));
        p->i = save;
        advance(p);   /* '(' */
        Node *e = parse_expr(p);
        expect(p, TOK_RPAREN);
        return e;
    }

    case TOK_LBRACE:  return parse_brace_expr(p);
    case TOK_DOT_BRACE: return parse_dot_brace(p);

    case TOK_KW_IF: case TOK_KW_WHILE: case TOK_KW_FOR: case TOK_KW_SWITCH:
        return parse_construct(p, NULL);

    case TOK_KW_INLINE: {
        /* `inline for (a..b) i { … }`：编译期展开，循环体按迭代次数克隆 */
        if (peek(p, 1)->kind != TOK_KW_FOR) {
            perr(p, "'inline' can only be used before 'for'");
            advance(p);
            return node_new(NK_BLOCK, t->line, t->col);
        }
        advance(p);                       /* inline */
        Node *n = parse_for(p, NULL, true);
        n->flag = true;
        return n;
    }

    case TOK_KW_STRUCT: case TOK_KW_UNION: case TOK_KW_ENUM:
    case TOK_KW_EXTERN:
    case TOK_QUESTION:
        return parse_type(p);   /* types are values（extern struct/union 走前缀分支） */

    /* 类型是值：`[]const u8` / `*i32` 也能出现在表达式位置（泛型实参、
     * 结构体字面量的构造者）。primary 位置不可能以二元运算符开头，
     * 所以这里看到 `[` / `*` 只可能是类型。 */
    case TOK_LBRACKET: case TOK_STAR:
        return parse_type(p);

    case TOK_KW_ERROR: case TOK_KW_TYPE:
        return parse_type(p);

    case TOK_KW_FN:
        return parse_fn(p, false);

    case TOK_KW_MODULE: {
        advance(p);
        expect(p, TOK_LPAREN);
        Node *n = node_new(NK_MODULE, t->line, t->col);
        if (at(p, TOK_STRING)) {
            Token *s = advance(p);
            n->str = s->text;
            n->str_len = s->text_len;
        } else {
            perr(p, "module() needs a string name");
        }
        expect(p, TOK_RPAREN);
        return n;
    }

    case TOK_AT: {
        advance(p);
        Node *n = node_new(NK_BUILTIN, t->line, t->col);
        n->name = take_ident(p);

        /* 语法只有两种形状：@sizeOf/@alignOf/@typeName 先收类型，
         * @memcpy/@memmove/@memset 与 @compileError 先收表达式。
         * （1.2.0 删掉了 @typeOf/@field/@*Cast 这批，转换统一写 `(T)value`。） */
        bool value_first = n->name &&
            (!strcmp(n->name, "memcpy") || !strcmp(n->name, "memmove") ||
             !strcmp(n->name, "memset"));
        expect(p, TOK_LPAREN);
        if (!at(p, TOK_RPAREN)) {
            int ai = 0;
            for (;;) {
                bool as_value = value_first || ai > 0;
                push(n, as_value ? parse_expr(p) : parse_type_or_const(p));
                ai++;
                if (!accept(p, TOK_COMMA)) break;
            }
        }
        expect(p, TOK_RPAREN);
        return n;
    }

    default:
        perr(p, "expected an expression, found %s", tok_kind_name(t->kind));
        advance(p);
        return node_new(NK_UNDEF, t->line, t->col);
    }
}

/* ---------------- statements ---------------- */

static Node *parse_decl(P *p) {
    Token *t = advance(p);   /* const / let / var */
    Node *n = node_new(NK_DECL, t->line, t->col);
    n->flag = (t->kind == TOK_KW_LET) ? true : false;
    n->op = (t->kind == TOK_KW_VAR) ? 2 : (t->kind == TOK_KW_LET ? 1 : 0);

    /* destructuring: const {a, b, c} = tup;  /  let .{a, b} = t; */
    if (at(p, TOK_LBRACE) || at(p, TOK_DOT_BRACE)) {
        if (at(p, TOK_DOT_BRACE)) advance(p); else advance(p);
        Node *pat = node_new(NK_TUPLE, t->line, t->col);
        if (!at(p, TOK_RBRACE)) {
            for (;;) {
                Node *id = node_new(NK_IDENT, cur(p)->line, cur(p)->col);
                id->name = take_ident(p);
                push(pat, id);
                if (!accept(p, TOK_COMMA)) break;
                if (at(p, TOK_RBRACE)) break;
            }
        }
        expect(p, TOK_RBRACE);
        n->c = pat;
        expect(p, TOK_ASSIGN);
        n->b = parse_expr(p);
        expect(p, TOK_SEMI);
        return n;
    }

    n->name = take_ident(p);
    if (accept(p, TOK_COLON)) n->a = parse_type(p);
    if (accept(p, TOK_ASSIGN)) n->b = parse_expr(p);
    expect(p, TOK_SEMI);
    return n;
}

static Node *parse_fn(P *p, bool allow_name) {
    Token *t = advance(p);   /* fn */
    Node *n = node_new(NK_FN, t->line, t->col);
    if (at(p, TOK_IDENT) && peek(p, 1)->kind == TOK_LPAREN) {
        /* `fn name(…)` 只留给 `extern`（没有函数体，只是外部符号声明）。
         * 有函数体的定义一律是值：`const name = fn (…) R { … };`——名字属于
         * 绑定，不属于函数；名字在 `fn` 后面会让"函数有个自己的名字"和
         * "一个值被绑到名字上"变成两套东西。 */
        Token *id = cur(p);
        if (!allow_name)
            perr(p, "functions are values: write `const %.*s = fn (...) ... { ... };`",
                 (int)(id->len < 40 ? id->len : 40), id->start);
        n->name = take_ident(p);
    }
    expect(p, TOK_LPAREN);
    if (!at(p, TOK_RPAREN)) {
        for (;;) {
            Node *pm = node_new(NK_PARAM, cur(p)->line, cur(p)->col);
            if (at(p, TOK_KW_VAR)) { advance(p); pm->flag = true; }
            if (at(p, TOK_IDENT) && peek(p, 1)->kind == TOK_COLON) {
                pm->name = take_ident(p);
                expect(p, TOK_COLON);
            }
            pm->a = parse_type(p);
            push(n, pm);
            if (!accept(p, TOK_COMMA)) break;
        }
    }
    expect(p, TOK_RPAREN);
    if (!at(p, TOK_LBRACE) && !at(p, TOK_SEMI)) n->a = parse_type(p);  /* return type */
    if (at(p, TOK_LBRACE)) n->b = parse_block(p);
    else n->flag = true;      /* no body → this is a fn TYPE (value is a code pointer) */
    return n;
}

static Node *parse_construct(P *p, const char *label);
static Node *parse_for(P *p, const char *label, bool allow_nobind);

/* 语句关键字：放在捕获名后面一定是漏了块。
 * `if (o) v return 1;` 读起来像"捕获 v，然后 return"，但语法上 `v` 只是个
 * 表达式、`return` 已经是兄弟语句了——所以要求写块。
 * 例外见 arm_stmt_only：`return` / `continue` / `defer` 不会引出嵌套控制流，
 * 单条就能当臂用（M19 的语法糖）。 */
static bool starts_stmt_kw(TokKind k) {
    switch (k) {
    case TOK_KW_IF: case TOK_KW_WHILE: case TOK_KW_FOR:
    case TOK_KW_SWITCH: case TOK_KW_RETURN: case TOK_KW_CONTINUE:
    case TOK_KW_DEFER: case TOK_KW_CONST: case TOK_KW_LET: case TOK_KW_VAR:
        return true;
    default: return false;
    }
}

/* 单条语句就能当 if 臂的那几个关键字（不用写花括号）：
 *   if (c) return x;      if (o) v return v;      if (c) defer f();
 * 它们不像 if/while/for 那样会引出"第二个分支体 vs 兄弟语句"的歧义。 */
static bool arm_stmt_only(TokKind k) {
    return k == TOK_KW_RETURN || k == TOK_KW_CONTINUE || k == TOK_KW_DEFER ||
           k == TOK_KW_ERRDEFER;
}

/* 能开始一个表达式、且不是二元运算符的 token。
 * 用来区分 `if (o) v v`（捕获名 + 返回值）和 `if (o) v + 1`（v 是外部变量）。 */
static bool starts_value(TokKind k) {
    switch (k) {
    case TOK_INT: case TOK_FLOAT: case TOK_CHAR: case TOK_STRING:
    case TOK_IDENT: case TOK_KW_NULL: case TOK_KW_UNDEF:
        return true;
    default: return false;
    }
}

/* 只算真正的二元运算符：`.` / `(` / `[` 是后缀，走 parse_binary_rest 会被
 * 直接退回（它不认识后缀），于是 `if (c) a else self.cap` 里的 `.cap`
 * 会漏到外层、挂到 if 结果上。 */
static bool starts_binop(TokKind k) {
    switch (k) {
    case TOK_STAR: case TOK_SLASH: case TOK_PERCENT:
    case TOK_PLUS: case TOK_MINUS:
    case TOK_LT: case TOK_LE: case TOK_GT: case TOK_GE: case TOK_EQ: case TOK_NE:
    case TOK_ANDAND: case TOK_OROR: case TOK_PIPE: case TOK_CARET: case TOK_AMP:
    case TOK_SHL: case TOK_SHR:
        return true;
    default: return false;
    }
}

/* 捕获名后面紧跟这些 token，说明臂到这里就结束了——臂的值只能是捕获值本身
 * （M50 的糖）。块和值都写不出这种形状，所以判定不歧义。 */
static bool ends_arm_here(TokKind k) {
    switch (k) {
    case TOK_SEMI: case TOK_COMMA: case TOK_RPAREN:
    case TOK_RBRACE: case TOK_RBRACKET: case TOK_KW_ELSE:
        return true;
    default: return false;
    }
}

/* One `if` arm.
 *   t: { … }        labeled block arm（标签=块；`t: v;` 交出臂的值）
 *   v { … }         capture + block
 *   v t: { … }      capture + labeled block
 *   { … }           plain block arm
 *   v expr          capture + single value (sugar)
 *   v               capture, and the arm is the capture itself (sugar, M50)
 *   expr            expression arm
 *   10              expression arm
 *
 * implicit_ok 只在"真值臂"上为真：`else` 臂那边 `if (c) x else y` 的 x/y 一直是
 * 普通表达式（老写法），不能读成捕获。 */
static Node *parse_if_arm(P *p, const char **capture, bool implicit_ok) {
    if (at(p, TOK_IDENT) && peek(p, 1)->kind == TOK_COLON) {
        /* `lbl:` 后面跟什么决定它是标签还是收割，臂位置也得照着分：
         *   构造（if/while/for/switch）→ 带标签的构造
         *   块（里面真有语句）        → 带标签的块
         *   其余（`{}` / `{a, b}` / 表达式）→ 收割
         * 少了这条，`if (c) tag: {};` 会静默变成"带标签的空块"（什么都
         * 不做，看着像 break），`if (c) tag: 0;` 则报"expected '{'"。 */
        TokKind after = peek(p, 2)->kind;
        bool ctor = after == TOK_KW_IF || after == TOK_KW_WHILE ||
                    after == TOK_KW_FOR || after == TOK_KW_SWITCH;
        bool blk = after == TOK_LBRACE && brace_is_block(p, p->i + 2);
        if (!ctor && !blk) return parse_yield(p);
        const char *lb = dup_tok(advance(p));
        advance(p);                       /* ':' */
        Node *b = parse_construct(p, lb);
        return b;
    }
    if (at(p, TOK_IDENT) && peek(p, 1)->kind == TOK_LBRACE) {
        *capture = take_ident(p);
        return parse_block(p);
    }
    /* 捕获 + 标签块：`if (o) v lbl: { … }`——和 switch 臂同一套写法：
     * 名字后紧跟 `{` 是捕获，跟 `:` 的是标签。 */
    if (at(p, TOK_IDENT) && !is_bool_word(cur(p)) &&
        peek(p, 1)->kind == TOK_IDENT && peek(p, 2)->kind == TOK_COLON) {
        *capture = take_ident(p);
        const char *lb = dup_tok(advance(p));
        advance(p);                       /* ':' */
        Node *b = parse_block(p);
        b->label = lb;
        return b;
    }
    if (at(p, TOK_LBRACE)) return parse_block(p);

    /* 捕获即返回值（M50）：`if (o) v;` / `if (o) v else 0`——臂的值就是捕获值
     * 本身，不用再写标签块（`if (o) v t: { t: v; }`）。
     * 判据：捕获名后面紧跟"臂到此为止"的 token。真值臂与 else 臂共用这套写法，
     * 但 else 臂不读捕获（那里的裸名字一直是普通表达式）。 */
    if (implicit_ok && at(p, TOK_IDENT) && ends_arm_here(peek(p, 1)->kind)) {
        Token *idt = advance(p);
        *capture = dup_tok(idt);
        Node *idn = node_new(NK_IDENT, idt->line, idt->col);
        idn->name = *capture;
        return idn;
    }

    /* 单条语句臂（语法糖，M19）：`if (c) return x;` / `continue;` / `defer f();`
     * 语句直接当块用——包一层单语句的块，语义零变化。 */
    if (arm_stmt_only(cur(p)->kind)) {
        Node *blk = node_new(NK_BLOCK, cur(p)->line, cur(p)->col);
        push(blk, parse_stmt(p));
        return blk;
    }

    /* 带捕获的单条语句：`if (o) v return v;` */
    if (at(p, TOK_IDENT) && arm_stmt_only(peek(p, 1)->kind)) {
        *capture = take_ident(p);
        Node *blk = node_new(NK_BLOCK, cur(p)->line, cur(p)->col);
        push(blk, parse_stmt(p));
        return blk;
    }

    /* 捕获名后面只有两种合法写法：块，或者单个返回值（语法糖）。
     *
     * 语句一律要求写进块里。`if (o) v if (v > 0) { … }` 这种写法在语法上
     * 无法区分"第二个 if 是分支体"和"第二个 if 是兄弟语句"——两种读法都能
     * 通过，而语义完全不同（后者会先判断 o、丢掉结果，然后无条件判断 v）。
     * 与其猜，不如要求写块：`if (o) v { if (v > 0) { … } }`。 */
    if (at(p, TOK_IDENT) && starts_stmt_kw(peek(p, 1)->kind)) {
        perr(p, "a capture name must be followed by `{ ... }` or a single value; "
                "put the statement inside a block");
    }
    /* 捕获名 + 单个返回值（语法糖）：`if (o) v v else 0`。
     * 两者之间没有分隔符，只能靠"下一个 token 能不能开始一个值"来认——
     * 运算符开头（`v + 1`）仍是普通表达式，`v` 就是外层变量。 */
    if (at(p, TOK_IDENT) && starts_value(peek(p, 1)->kind)) {
        *capture = take_ident(p);
        return parse_expr(p);
    }
    /* `if (m) v * 2 else 0`：v 既是捕获名，又是表达式的一部分。 */
    if (at(p, TOK_IDENT) && starts_binop(peek(p, 1)->kind)) {
        Token *id = advance(p);
        *capture = dup_tok(id);
        Node *lhs = node_new(NK_IDENT, id->line, id->col);
        lhs->name = *capture;
        return parse_binary_rest(p, lhs, 1);
    }
    return parse_expr(p);
}

static Node *parse_if(P *p, const char *label) {
    Token *t = advance(p);   /* if */
    Node *n = node_new(NK_IF, t->line, t->col);
    n->label = label;
    expect(p, TOK_LPAREN);
    n->a = parse_expr(p);
    expect(p, TOK_RPAREN);

    n->b = parse_if_arm(p, &n->name, true);

    /* 单表达式语句臂（语法糖）：`if (c) call(); else other();`
     * 臂就是一条表达式语句，分号属于臂本身，不是外层语句的结束符。
     * 只在 `;` 后面紧跟 `else` 时才这么读——否则那个分号归外层语句
     * （`if (c) call();` 照旧），`let x = if (c) a else b;` 的值语义
     * 也完全不受影响（then 臂后面跟的是 else，不是分号）。 */
    bool stmt_arm = false;
    if (at(p, TOK_SEMI) && peek(p, 1)->kind == TOK_KW_ELSE) {
        advance(p);
        if (n->b->kind != NK_BLOCK) {
            Node *blk = node_new(NK_BLOCK, n->b->line, n->b->col);
            push(blk, n->b);
            n->b = blk;
        }
        stmt_arm = true;
    }

    if (accept(p, TOK_KW_ELSE)) {
        if (at(p, TOK_KW_IF)) {
            n->c = parse_if(p, NULL);
        } else {
            const char *cap2 = NULL;
            n->c = parse_if_arm(p, &cap2, false);
            if (cap2) {
                Node *id = node_new(NK_IDENT, t->line, t->col);
                id->name = cap2;
                n->d = id;                 /* failure-branch capture */
            }
            /* then 臂是语句形态，整个 if 也就按语句读：else 臂同样是臂，
             * 但末尾那个分号留给外层语句（`parse_stmt` / 声明本来就会吃），
             * 这样 `let y = if (c) f(); else g();` 报的是「void 不能赋给 y」，
             * 而不是莫名其妙的「缺分号」。 */
            if (stmt_arm && n->c->kind != NK_BLOCK) {
                Node *blk = node_new(NK_BLOCK, n->c->line, n->c->col);
                push(blk, n->c);
                n->c = blk;
            }
        }
    }
    return n;
}

static Node *parse_while(P *p, const char *label) {
    Token *t = advance(p);
    Node *n = node_new(NK_WHILE, t->line, t->col);
    n->label = label;
    expect(p, TOK_LPAREN);
    n->a = parse_expr(p);
    expect(p, TOK_RPAREN);
    /* `while (c) v { … }` 捕获。标签不写在这里——循环的名字写在关键字前面
     * （`done: while (…) { … }`）：标签是循环的出口，写成后缀看起来像
     * "体的名字"，读者会以为它只跳到体末、下一轮继续。 */
    if (at(p, TOK_IDENT) && peek(p, 1)->kind == TOK_LBRACE) n->name = take_ident(p);
    if (at(p, TOK_IDENT) && !is_bool_word(cur(p)) && peek(p, 1)->kind == TOK_COLON) {
        Token *id = cur(p);
        if (n->label)
            perr(p, "`while` is already labeled before the keyword");
        else
            perr(p, "the label of a loop goes before the keyword: `%.*s: while (...) { ... }`",
                 (int)(id->len < 40 ? id->len : 40), id->start);
        advance(p); advance(p);           /* 名字 + ':' */
    }
    n->b = parse_block(p);
    if (accept(p, TOK_KW_ELSE)) n->c = at(p, TOK_LBRACE) ? parse_block(p) : parse_expr(p);
    return n;
}

static Node *parse_for(P *p, const char *label, bool allow_nobind) {
    Token *t = advance(p);
    Node *n = node_new(NK_FOR, t->line, t->col);
    n->label = label;
    expect(p, TOK_LPAREN);
    /* sequences */
    Node *seqs[8]; int nseq = 0;
    for (;;) {
        Node *e = parse_expr(p);
        if (accept(p, TOK_DOTDOT)) {
            Node *r = node_new(NK_RANGE, e->line, e->col);
            r->a = e;
            if (!at(p, TOK_COMMA) && !at(p, TOK_RPAREN)) r->b = parse_expr(p);
            e = r;
        }
        if (nseq < 8) seqs[nseq++] = e;
        if (!accept(p, TOK_COMMA)) break;
    }
    expect(p, TOK_RPAREN);
    /* bindings（inline for 允许一个都不写：纯按次数重复） */
    Node *binds[8]; int nbind = 0;
    for (;;) {
        if (allow_nobind && !at(p, TOK_IDENT)) break;
        const char *bn = take_ident(p);
        Node *b = node_new(NK_IDENT, cur(p)->line, cur(p)->col);
        b->name = bn;
        if (nbind < 8) binds[nbind++] = b;
        if (!accept(p, TOK_COMMA)) break;
    }
    for (int i = 0; i < nseq; i++) push(n, seqs[i]);
    for (int i = 0; i < nbind; i++) push(n, binds[i]);
    /* 绑定之后不再接受标签：循环的名字写在关键字前面
     * （`done: for (xs) x { … }`），那是循环的出口。 */
    if (at(p, TOK_IDENT) && !is_bool_word(cur(p)) && peek(p, 1)->kind == TOK_COLON) {
        Token *id = cur(p);
        if (n->label)
            perr(p, "`for` is already labeled before the keyword");
        else
            perr(p, "the label of a loop goes before the keyword: `%.*s: for (...) x { ... }`",
                 (int)(id->len < 40 ? id->len : 40), id->start);
        advance(p); advance(p);           /* 名字 + ':' */
    }
    n->a = node_new(NK_TUPLE, t->line, t->col);   /* arity markers */
    n->a->ival = (uint64_t)nseq;
    n->a->fval = (double)nbind;
    n->b = parse_block(p);
    if (accept(p, TOK_KW_ELSE)) n->c = at(p, TOK_LBRACE) ? parse_block(p) : parse_expr(p);
    return n;
}

static Node *parse_switch(P *p, const char *label) {
    Token *t = advance(p);
    Node *n = node_new(NK_SWITCH, t->line, t->col);
    n->label = label;
    expect(p, TOK_LPAREN);
    n->a = parse_expr(p);
    expect(p, TOK_RPAREN);
    expect(p, TOK_LBRACE);

    while (!at(p, TOK_RBRACE) && !at(p, TOK_EOF_)) {
        Node *arm = node_new(NK_ARM, cur(p)->line, cur(p)->col);
        if (accept(p, TOK_KW_ELSE)) {
            arm->flag = true;                 /* else arm */
        } else {
            for (;;) {
                /* pattern: .name | int | char | (error)Err.x | … */
                if (at(p, TOK_DOT) && peek(p, 1)->kind == TOK_IDENT) {
                    advance(p);
                    Token *id = advance(p);
                    Node *pat = node_new(NK_FIELD, id->line, id->col);
                    pat->name = dup_tok(id);
                    push(arm, pat);
                } else {
                    push(arm, parse_unary(p));
                }
                /* 逗号只作"同臂并列"：`,` 后必须还能开一个新模式。
                 * 臂分隔已交给 `;`，这里不再有歧义。 */
                if (at(p, TOK_COMMA)) {
                    advance(p);
                    if (!starts_pattern(cur(p)))
                        perr(p, "expected another pattern after ',' -- ',' joins patterns of one arm, arms end with ';'");
                }
                else
                    break;
            }
        }
        /* v0.22 起标签写在冒号后面（`模式: 标签: {…}`），和捕获名同一位置。
         * 旧的 `模式 标签:` 写法在这里点一下，省得报一句没头没脑的
         * "expected ':'"。 */
        if (at(p, TOK_IDENT) && peek(p, 1)->kind == TOK_COLON && !is_bool_word(cur(p)))
            perr(p, "the arm label goes after the `:` -- write `pattern: [cap] lbl: { ... }`");
        expect(p, TOK_COLON);
        /* 冒号后面、臂体前面最多两个名字（v0.22）：
         *   cap { … }        捕获：名字后紧跟 `{`——绑定的就是"匹配到的东西"
         *   lbl: { … }       标签块臂：名字后跟 `:`，块里 `lbl: v;` 交出臂的值
         *   cap lbl: { … }   两者都要：先捕获，再给块贴标签
         * 名字后面既不是 `{` 也不是 `:` 时，它就是表达式臂的开头（`.x: foo`）。
         * 捕获只配块臂：`cap 值` 和裸 `值` 没法区分，索性不允许。
         * 例外是 M50 的糖：名字后面**什么都没有**（紧跟 `;` / `}`）时，臂的值
         * 就是捕获值本身——`pattern: cap;` ≡ `pattern: cap { cap }`；
         * 想要外部常量请加括号：`pattern: (cap)`。 */
        Node *implicit = NULL;
        if (at(p, TOK_IDENT) && !is_bool_word(cur(p))) {
            bool ends = peek(p, 1)->kind == TOK_SEMI || peek(p, 1)->kind == TOK_RBRACE;
            if (!arm->flag && ends) {
                Token *idt = advance(p);
                arm->name = dup_tok(idt);
                implicit = node_new(NK_IDENT, idt->line, idt->col);
                implicit->name = arm->name;
            } else if (peek(p, 1)->kind == TOK_LBRACE) {
                arm->name = take_ident(p);
                if (at(p, TOK_IDENT) && peek(p, 1)->kind == TOK_COLON) {
                    arm->label = take_ident(p);
                    advance(p);                /* ':' */
                }
            } else if (peek(p, 1)->kind == TOK_COLON) {
                arm->label = take_ident(p);
                advance(p);                     /* ':' */
            } else if (peek(p, 1)->kind == TOK_IDENT && peek(p, 2)->kind == TOK_COLON) {
                arm->name = take_ident(p);      /* cap lbl: { … } */
                arm->label = take_ident(p);
                advance(p);                     /* ':' */
            }
        }
        if (arm->label && !at(p, TOK_LBRACE))
            perr(p, "a labeled arm needs a block: the label is the scope its `lbl: v;` yields to");
        arm->a = implicit ? implicit
                          : (at(p, TOK_LBRACE) ? parse_block(p) : parse_expr(p));
        push(n, arm);
        if (!accept(p, TOK_SEMI))
            perr(p, "expected ';' after switch arm");
    }
    expect(p, TOK_RBRACE);
    return n;
}

static Node *parse_construct(P *p, const char *label) {
    switch (cur(p)->kind) {
    case TOK_KW_IF:     return parse_if(p, label);
    case TOK_KW_WHILE:  return parse_while(p, label);
    case TOK_KW_FOR:    return parse_for(p, label, false);
    case TOK_KW_SWITCH: return parse_switch(p, label);
    case TOK_LBRACE: {
        Node *b = parse_block(p);
        b->label = label;
        return b;
    }
    default:
        perr(p, "expected a labelable construct (block/if/while/for/switch)");
        return node_new(NK_BLOCK, cur(p)->line, cur(p)->col);
    }
}

/* yield stmt: tag: expr;   or   tag: {tuple};   */
static Node *parse_yield(P *p) {
    Token *t = cur(p);
    const char *tag = take_ident(p);
    expect(p, TOK_COLON);
    Node *n = node_new(NK_YIELD, t->line, t->col);
    n->label = tag;
    if (at(p, TOK_LBRACE)) {
        /* brace_is_block said "tuple" (else we'd be in label path) */
        n->a = parse_brace_expr(p);
    } else {
        n->a = parse_expr(p);
    }
    expect(p, TOK_SEMI);
    return n;
}

/* extern "C" { … } 块内的一项。`extern` 语义由块本身带来，所以这里
 * 不吃 extern 关键字：fn → extern fn、var → extern var、
 * const Name = struct/union → extern struct/union。其余给出诊断。
 * `pub` 允许（绑定模块的 extern 就是它的公开 API），直接透传。 */
static Node *parse_extern_item(P *p) {
    Token *t = cur(p);
    if (t->kind == TOK_KW_PUB) {
        advance(p);
        Node *inn = parse_extern_item(p);
        if (inn) inn->is_pub = true;
        return inn;
    }
    if (t->kind == TOK_KW_FN) {
        Node *fn = parse_fn(p, true);
        if (fn) { fn->is_extern = true; fn->flag = false; }
        expect(p, TOK_SEMI);
        return fn;
    }
    if (t->kind == TOK_KW_VAR) {
        Node *d = parse_decl(p);
        if (d) d->is_extern = true;
        return d;
    }
    if (t->kind == TOK_KW_CONST || t->kind == TOK_KW_LET) {
        Node *d = parse_decl(p);
        if (d && d->b && (d->b->kind == NK_STRUCT_TYPE || d->b->kind == NK_UNION_TYPE)) {
            d->b->is_extern = true;
            return d;
        }
        perr(p, "inside `extern \"C\"` only `fn` / `var` / "
                "`const Name = struct|union` declarations are allowed");
        return d;
    }
    perr(p, "expected `fn`, `var` or `const Name = struct|union` inside `extern \"C\"`");
    return node_new(NK_BLOCK, t->line, t->col);
}

static Node *parse_stmt(P *p) {
    Token *t = cur(p);

    /* pub 只对定义/声明有意义：`pub fn f() {}`、`pub const X = 1;` */
    if (t->kind == TOK_KW_PUB) {
        advance(p);
        Node *in = parse_stmt(p);
        if (in && (in->kind == NK_DECL || in->kind == NK_FN)) {
            in->is_pub = true;
        }
        else perr(p, "`pub` must be followed by `fn`, `const`, `let` or `var`");
        return in;
    }

    /* extern ["C"] fn name(params) Ret;   —— 外部 C 函数声明（M23）。
     * 目前只有 C 一种 ABI，写不写 "C" 都当 C；将来多 ABI 时字符串就有用了。 */
    if (t->kind == TOK_KW_EXTERN) {
        advance(p);
        if (at(p, TOK_STRING)) {
            Token *ab = advance(p);
            if (!(ab->text_len == 1 && ab->text[0] == 'C'))
                perr(p, "only `extern \"C\"` is supported for now");
        }
        /* extern var name: T;   —— 外部 C 全局变量（M24）。 */
        if (at(p, TOK_KW_VAR)) {
            Node *d = parse_decl(p);
            if (d) d->is_extern = true;
            return d;
        }
        /* extern ["C"] { … } —— 块糖：整组 extern 声明一次给出。
         * 返回标记过的 NK_BLOCK，parse_program 把块内声明上提到文件根。 */
        if (at(p, TOK_LBRACE)) {
            Node *blk = node_new(NK_BLOCK, t->line, t->col);
            advance(p);
            while (!at(p, TOK_RBRACE) && !at(p, TOK_EOF_)) {
                size_t before = p->i;
                Node *it = parse_extern_item(p);
                if (it) push(blk, it);
                if (p->i == before) advance(p);
            }
            expect(p, TOK_RBRACE);
            blk->is_extern = true;
            return blk;
        }
        if (!at(p, TOK_KW_FN)) {
            if (at(p, TOK_KW_STRUCT) || at(p, TOK_KW_UNION)) {
                /* `extern struct Name { … };` 语句形式：Zx 的类型是值，
                 * 命名类型一律 `const Name = …`，这里给出对齐写法的提示。 */
                perr(p, "named types are values in Zx -- write "
                        "`const Name = extern struct { ... };`");
                return node_new(NK_BLOCK, t->line, t->col);
            }
            perr(p, "expected `fn` after `extern`");
            return node_new(NK_BLOCK, t->line, t->col);
        }
        Node *fn = parse_fn(p, true);
        fn->is_extern = true;
        fn->flag = false;              /* 不是 fn 类型值，是"有名字、没函数体"的声明 */
        expect(p, TOK_SEMI);
        return fn;
    }

    switch (t->kind) {
    case TOK_KW_CONST: case TOK_KW_LET: case TOK_KW_VAR:
        return parse_decl(p);

    case TOK_KW_FN: {
        Node *f = parse_fn(p, false);
        accept(p, TOK_SEMI);
        return f;
    }

    case TOK_KW_DEFER:
    case TOK_KW_ERRDEFER: {
        advance(p);
        Node *n = node_new(t->kind == TOK_KW_ERRDEFER ? NK_ERRDEFER : NK_DEFER,
                           t->line, t->col);
        /* 块形式不要尾分号（与普通块一致）；表达式形式才收 ';'。
         * rbrace 例外是 if 单臂糖：`if (c) defer f()` 后面直接是 `}`。 */
        bool body_block = at(p, TOK_LBRACE);
        n->a = body_block ? parse_block(p) : parse_expr(p);
        if (!body_block && !at(p, TOK_RBRACE)) expect(p, TOK_SEMI);
        return n;
    }

    case TOK_KW_RETURN: {
        advance(p);
        Node *n = node_new(NK_RETURN, t->line, t->col);
        if (!at(p, TOK_SEMI)) n->a = parse_expr(p);
        expect(p, TOK_SEMI);
        return n;
    }

    case TOK_KW_CONTINUE: {
        advance(p);
        Node *n = node_new(NK_CONTINUE, t->line, t->col);
        if (accept(p, TOK_COLON)) n->label = take_ident(p);
        expect(p, TOK_SEMI);
        return n;
    }

    default: break;
    }

    /* IDENT ':' — yield, or a label on a construct */
    if (t->kind == TOK_IDENT && peek(p, 1)->kind == TOK_COLON && !is_bool_word(t)) {
        TokKind after = peek(p, 2)->kind;
        if (after == TOK_KW_FOR || after == TOK_KW_WHILE || after == TOK_KW_SWITCH ||
            after == TOK_KW_IF) {
            const char *label = dup_tok(advance(p));
            advance(p);
            Node *n = parse_construct(p, label);
            /* 语句形态的 if 可能带着尾分号（`lbl: if (c) f(); else g();`）：
             * 普通语句路径由下面的 accept 收，这个分支直接返回，得自己收。 */
            accept(p, TOK_SEMI);
            return n;
        }
        if (after == TOK_LBRACE) {
            if (brace_is_block(p, p->i + 2)) {
                const char *label = dup_tok(advance(p));
                advance(p);
                Node *b = parse_block(p);
                b->label = label;
                return b;
            }
            return parse_yield(p);
        }
        return parse_yield(p);
    }

    Node *e = parse_expr(p);
    accept(p, TOK_SEMI);
    return e;
}

static Node *parse_block(P *p) {
    Token *t = expect(p, TOK_LBRACE);
    Node *n = node_new(NK_BLOCK, t->line, t->col);
    while (!at(p, TOK_RBRACE) && !at(p, TOK_EOF_)) {
        size_t before = p->i;
        push(n, parse_stmt(p));
        if (p->i == before) advance(p);   /* guarantee progress on errors */
    }
    expect(p, TOK_RBRACE);
    return n;
}

/* ---------------- program ---------------- */

/* 顶层恢复（M20）：一个声明里出了语法错，跳到下一个**可能开始声明**的
 * token，让文件后半段还能继续解析。没有这一步，`fn` 里漏个花括号就能
 * 把后面所有函数一起吞掉，用户只能一轮修一个错。 */
static bool tok_starts_decl(TokKind k) {
    switch (k) {
    case TOK_KW_FN: case TOK_KW_PUB: case TOK_KW_CONST: case TOK_KW_VAR:
    case TOK_KW_STRUCT: case TOK_KW_ENUM: case TOK_KW_UNION: case TOK_KW_MODULE:
    case TOK_KW_EXTERN:
        return true;
    default:
        return false;
    }
}

static void sync_decl(P *p) {
    int depth = 0;
    while (!at(p, TOK_EOF_)) {
        TokKind k = cur(p)->kind;
        if (k == TOK_LBRACE || k == TOK_DOT_BRACE) depth++;
        else if (k == TOK_RBRACE) { if (depth == 0) { advance(p); continue; } depth--; }
        else if (depth == 0 && tok_starts_decl(k)) return;
        advance(p);
    }
}

/* 括号配对预计算（A1）。栈式走一遍 token 流；失配/未闭合的括号位置记
 * SIZE_MAX（消歧处落回旧试探路径——那是错误用例，不赶时间）。分配失败
 * 返回 NULL，同样落回旧路径，解析照常工作。 */
static size_t *bracket_match(Token *t, size_t n) {
    size_t *m = malloc(sizeof(size_t) * (n ? n : 1));
    if (!m) return NULL;
    size_t *stk = malloc(sizeof(size_t) * (n ? n : 1));
    if (!stk) { free(m); return NULL; }
    size_t top = 0;
    for (size_t i = 0; i < n; i++) {
        m[i] = SIZE_MAX;
        switch (t[i].kind) {
        case TOK_LPAREN: case TOK_LBRACKET: case TOK_LBRACE: case TOK_DOT_BRACE:
            stk[top++] = i;
            break;
        case TOK_RPAREN: case TOK_RBRACKET: case TOK_RBRACE: {
            if (top == 0) break;                      /* 多余的闭括号：留给报错 */
            TokKind open = t[stk[top - 1]].kind;
            bool pairs = (t[i].kind == TOK_RPAREN   && open == TOK_LPAREN) ||
                         (t[i].kind == TOK_RBRACKET && open == TOK_LBRACKET) ||
                         (t[i].kind == TOK_RBRACE  && (open == TOK_LBRACE || open == TOK_DOT_BRACE));
            if (!pairs) { top = 0; break; }           /* 交叉嵌套：栈作废重来 */
            size_t j = stk[--top];
            m[i] = j;
            m[j] = i;
            break;
        }
        default: break;
        }
    }
    free(stk);
    return m;
}

/* 括号嵌套上限：递归下降的栈深度跟着括号走。超了明确报错，不让进程撑爆
 * 栈（撑爆的表现是"没有任何诊断就退出"，用户 2026-11-09 压力测试抓到）。
 * 与 Zx 前端 bootstrap/parse.zx 的 NEST_MAX 保持一致。 */
#define ZX_NEST_MAX 256

static bool brackets_too_deep(Lexer *lx) {
    int d = 0, maxd = 0;
    for (size_t i = 0; i < lx->ntoks; i++) {
        switch (lx->toks[i].kind) {
        case TOK_LPAREN: case TOK_LBRACE: case TOK_LBRACKET: case TOK_DOT_BRACE:
            if (++d > maxd) maxd = d;
            break;
        case TOK_RPAREN: case TOK_RBRACE: case TOK_RBRACKET:
            if (d > 0) d--;
            break;
        default: break;
        }
    }
    return maxd > ZX_NEST_MAX;
}

Node *parse_program(Lexer *lx) {
    g_parse_fname = lx->fname;   /* @file() 按模块捕获自己的源路径 */
    if (brackets_too_deep(lx)) {
        err_at(1, 1, "brackets nest too deeply (over %d levels) -- split it into smaller pieces",
               ZX_NEST_MAX);
        return NULL;
    }
    P p = { .t = lx->toks, .n = lx->ntoks, .i = 0, .last_line = 0,
            .match = bracket_match(lx->toks, lx->ntoks) };
    Node *root = node_new(NK_ROOT, 1, 1);
    while (!at(&p, TOK_EOF_)) {
        size_t before = p.i;
        int errs = g_error_count;
        Node *st = parse_stmt(&p);
        if (st && st->kind == NK_BLOCK && st->is_extern) {
            /* extern "C" { … }：块内声明上提到文件根（extern 只在根合法） */
            for (int i = 0; i < st->nitems; i++) push(root, st->items[i]);
        } else push(root, st);
        if (p.i == before) advance(&p);
        /* 这个声明吃到了错：同步到下一个声明开头再继续 */
        if (g_error_count > errs) sync_decl(&p);
    }
    return root;
}

/* ---------------- dump ---------------- */

static const char *nk_name(NodeKind k) {
    switch (k) {
    case NK_INT: return "int";
    case NK_FLOAT: return "float";
    case NK_STR: return "str";
    case NK_CHAR: return "char";
    case NK_IDENT: return "id";
    case NK_BOOL: return "bool";
    case NK_NULL: return "null";
    case NK_UNDEF: return "undefined";
    case NK_BIN: return "bin";
    case NK_RANGE: return "range";
    case NK_UN: return "un";
    case NK_CAST: return "cast";
    case NK_TRY: return "try";
    case NK_INDEX: return "index";
    case NK_SLICE: return "slice";
    case NK_FIELD: return "field";
    case NK_CALL: return "call";
    case NK_TUPLE: return "tuple";
    case NK_ANON_STRUCT: return "anon-struct";
    case NK_STRUCT_LIT: return "struct-lit";
    case NK_ARRAY_LIT: return "array-lit";
    case NK_PAIR: return "pair";
    case NK_BLOCK: return "block";
    case NK_IF: return "if";
    case NK_WHILE: return "while";
    case NK_FOR: return "for";
    case NK_SWITCH: return "switch";
    case NK_ARM: return "arm";
    case NK_UNWRAP: return "unwrap";
    case NK_ORELSE: return "orelse";
    case NK_RETURN: return "return";
    case NK_CONTINUE: return "continue";
    case NK_YIELD: return "yield";
    case NK_DEFER: return "defer";
    case NK_ERRDEFER: return "errdefer";
    case NK_BUILTIN: return "builtin";
    case NK_MODULE: return "module";
    case NK_DECL: return "decl";
    case NK_FN: return "fn";
    case NK_PARAM: return "param";
    case NK_TYPE_IDENT: return "T";
    case NK_PTR_TYPE: return "ptr";
    case NK_SLICE_TYPE: return "sliceT";
    case NK_TUPLE_TYPE: return "tupleT";
    case NK_STRUCT_TYPE: return "struct";
    case NK_UNION_TYPE: return "union";
    case NK_ENUM_TYPE: return "enum";
    case NK_OPT_TYPE: return "opt";
    case NK_ERR_TYPE: return "err";
    case NK_ERR_SET:  return "errset";
    case NK_ROOT: return "root";
    }
    return "?";
}

void ast_dump(const Node *n, int depth) {
    if (!n) return;
    for (int i = 0; i < depth; i++) fputs("  ", stdout);
    printf("(%s", nk_name(n->kind));
    printf(" @%d:%d", n->line, n->col);   /* 阶段 4：位置进对拍 */
    if (n->name) printf(" %s", n->name);
    if (n->label) printf(" :%s", n->label);
    if (n->kind == NK_INT) printf(" %llu", (unsigned long long)n->ival);
    if (n->kind == NK_FLOAT) printf(" %s", n->str ? n->str : "");
    if (n->kind == NK_STR) printf(" \"%.*s\"", n->str_len, n->str ? n->str : "");
    if (n->kind == NK_BUILTIN) printf(" @%s", n->name ? n->name : "");
    if (n->kind == NK_FIELD && !n->name) printf(" .%llu", (unsigned long long)n->ival);
    if ((n->kind == NK_BIN || n->kind == NK_UN) && n->op)
        printf(" %s", tok_kind_spell((TokKind)n->op));
    if (n->is_pub) fputs(" pub", stdout);
    if (n->kind == NK_DECL) printf(" %s", n->op == 0 ? "const" : n->op == 1 ? "let" : "var");
    if (n->flag && (n->kind == NK_STRUCT_TYPE || n->kind == NK_PTR_TYPE ||
                    n->kind == NK_SLICE_TYPE || n->kind == NK_ARM))
        fputs(" !", stdout);

    bool items_first = n->kind == NK_ROOT || n->kind == NK_BLOCK ||
                       n->kind == NK_TUPLE || n->kind == NK_FOR ||
                       n->kind == NK_SWITCH || n->kind == NK_ARM;
    if (items_first) {
        for (int i = 0; i < n->nitems; i++) {
            fputc('\n', stdout);
            ast_dump(n->items[i], depth + 1);
        }
    }
    if (n->a) { fputc('\n', stdout); ast_dump(n->a, depth + 1); }
    if (n->b) { fputc('\n', stdout); ast_dump(n->b, depth + 1); }
    if (n->c) { fputc('\n', stdout); ast_dump(n->c, depth + 1); }
    if (n->d) { fputc('\n', stdout); ast_dump(n->d, depth + 1); }
    if (!items_first) {
        for (int i = 0; i < n->nitems; i++) {
            fputc('\n', stdout);
            ast_dump(n->items[i], depth + 1);
        }
    }
    fputc(')', stdout);
    if (depth == 0) fputc('\n', stdout);
}
