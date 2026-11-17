/* astload.c — 装载 Zx 前端（bootstrap/zxfe.zx）产出的可装载 AST 序列化。
 *
 * 自举阶段 3：zxc --fe=zx（默认）不再用 C 解析器，而是让 zxfe 把源文件
 * 解析成序列化 AST，这里重建 Node 树交给 sema/backend。格式见 zxfe.zx 头注：
 *
 *   node := "(" kind line col op op2 flg ival fvbits slen name label str
 *            { "a" (node|"-") | "b" … | "c" … | "d" … } "i" "(" { node } ")" ")"
 *
 * 字段映射（Zx Node → C Node）的两处约定，与 bootstrap/parse.zx 对齐：
 *   - kind/op 编号与 ast.h NodeKind / zxc.h TokKind 同序（两边顺序注释钉死）；
 *   - Zx 的 op2 承载 decl 种类（0=const 1=let 2=var）与 union(enum) 糖标记，
 *     C 侧分别放在 op（DECL）与 op（UNION_TYPE，=1）；DECL 的 flag = (op2==1)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"

typedef struct {
    const char *p;   /* 游标 */
    const char *end;
    int         depth;
    int         bad;
} Loader;

static void lskip(Loader *L) {
    while (L->p < L->end && (*L->p == ' ' || *L->p == '\n' ||
                             *L->p == '\t' || *L->p == '\r')) L->p++;
}

/* 裸 token（数字 / 标点 / 字母），读到空白或括号边界 */
static const char *ltok(Loader *L, size_t *out_len) {
    lskip(L);
    if (L->p >= L->end) { L->bad = 1; *out_len = 0; return NULL; }
    const char *s = L->p;
    while (L->p < L->end && *L->p != ' ' && *L->p != '\n' && *L->p != '\t' &&
           *L->p != '\r' && *L->p != '(' && *L->p != ')') L->p++;
    *out_len = (size_t)(L->p - s);
    if (*out_len == 0) { L->bad = 1; return NULL; }
    return s;
}

static long long lint(Loader *L) {
    size_t n = 0;
    const char *s = ltok(L, &n);
    if (!s || n == 0 || n > 21) { L->bad = 1; return 0; }
    size_t i = 0;
    bool neg = false;
    if (s[0] == '-' && n > 1) { neg = true; i = 1; }
    unsigned long long v = 0;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') { L->bad = 1; return 0; }
        v = v * 10 + (unsigned long long)(s[i] - '0');
    }
    return neg ? -(long long)v : (long long)v;
}

static unsigned long long lnum(Loader *L) { return (unsigned long long)lint(L); }

/* 字符串字段："..." 带转义，或 "-"（null）。返回 malloc 的 NUL 结尾文本。 */
static char *lstr(Loader *L, int *out_len) {
    lskip(L);
    if (L->p >= L->end) { L->bad = 1; return NULL; }
    if (*L->p == '-') {
        L->p++;
        *out_len = 0;
        return NULL;
    }
    if (*L->p != '"') { L->bad = 1; return NULL; }
    L->p++;
    /* 先量 decoded 长度（转义最长 \xNN → 1 字节） */
    const char *q = L->p;
    size_t cap = 0;
    while (q < L->end && *q != '"') {
        if (*q == '\\') { q++; if (q < L->end && *q == 'x') q += 2; }
        q++; cap++;
    }
    char *buf = malloc(cap + 1);
    if (!buf) { L->bad = 1; return NULL; }
    size_t n = 0;
    while (L->p < L->end && *L->p != '"') {
        char c = *L->p++;
        if (c == '\\') {
            if (L->p >= L->end) { free(buf); L->bad = 1; return NULL; }
            char e = *L->p++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'x': {
                if (L->p + 1 >= L->end) { free(buf); L->bad = 1; return NULL; }
                char hi = L->p[0], lo = L->p[1];
                L->p += 2;
                int hv = (hi >= '0' && hi <= '9') ? hi - '0'
                       : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10 : -1;
                int lv = (lo >= '0' && lo <= '9') ? lo - '0'
                       : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10 : -1;
                if (hv < 0 || lv < 0) { free(buf); L->bad = 1; return NULL; }
                c = (char)((hv << 4) | lv);
                break;
            }
            default: c = e; break;      /* \\ 和 \" */
            }
        }
        buf[n++] = c;
    }
    if (L->p >= L->end) { free(buf); L->bad = 1; return NULL; }
    L->p++;                              /* 收 " */
    buf[n] = '\0';
    *out_len = (int)n;
    return buf;
}

static void push_item(Node *n, Node *it) {
    Node **grown = realloc(n->items, sizeof(Node *) * (size_t)(n->nitems + 1));
    if (!grown) { n->nitems = 0; return; }   /* 一次性编译器：失败即坏文件 */
    n->items = grown;
    n->items[n->nitems++] = it;
}

static Node *lnode(Loader *L) {
    lskip(L);
    if (L->p >= L->end || *L->p != '(') { L->bad = 1; return NULL; }
    L->p++;

    unsigned long long kind = lnum(L);
    unsigned long long line = lnum(L);
    unsigned long long col  = lnum(L);
    long long           opv = lint(L);
    unsigned long long op2  = lnum(L);
    unsigned long long flg  = lnum(L);
    unsigned long long ival = lnum(L);
    unsigned long long fvb  = lnum(L);
    unsigned long long slen = lnum(L);
    if (L->bad) return NULL;
    if (kind > (unsigned long long)NK_TAG_BIND) { L->bad = 1; return NULL; } /* NK_TAG_BIND 是当前最大 kind */

    Node *n = node_new((NodeKind)kind, (int)line, (int)col);
    if (!n) { L->bad = 1; return NULL; }
    n->ival = ival;
    memcpy(&n->fval, &fvb, sizeof(double));
    n->str_len = (int)slen;

    /* op：DECL 的种类与 union(enum) 糖标记来自 op2；其余是 TokKind 编号 */
    if (kind == (unsigned long long)NK_DECL || kind == (unsigned long long)NK_UNION_TYPE)
        n->op = (int)op2;
    else
        n->op = opv < 0 ? 0 : (int)opv;
    n->flag      = (kind == (unsigned long long)NK_DECL) ? (op2 == 1) : (flg & 1);
    n->is_pub    = (flg & 2) != 0;
    n->is_extern = (flg & 4) != 0;

    int sl = 0;
    char *s;
    s = lstr(L, &sl); if (L->bad) return NULL; n->name = s;
    s = lstr(L, &sl); if (L->bad) return NULL; n->label = s;
    s = lstr(L, &sl); if (L->bad) return NULL; n->str = s;

    /* 子结点：a/b/c/d 可选，items 固定 i( … ) */
    for (;;) {
        lskip(L);
        if (L->p >= L->end) { L->bad = 1; return NULL; }
        if (*L->p == ')') { L->p++; break; }
        if (*L->p == 'a' || *L->p == 'b' || *L->p == 'c' || *L->p == 'd') {
            char which = *L->p;
            L->p++;
            Node **slot = which == 'a' ? &n->a : which == 'b' ? &n->b
                        : which == 'c' ? &n->c : &n->d;
            lskip(L);
            if (L->p < L->end && *L->p == '-') { L->p++; *slot = NULL; }
            else { Node *ch = lnode(L); if (!ch) return NULL; *slot = ch; }
        } else if (*L->p == 'i') {
            L->p++;
            lskip(L);
            if (L->p >= L->end || *L->p != '(') { L->bad = 1; return NULL; }
            L->p++;
            lskip(L);
            while (L->p < L->end && *L->p != ')') {
                Node *it = lnode(L);
                if (!it) return NULL;
                push_item(n, it);
                lskip(L);
            }
            if (L->p >= L->end) { L->bad = 1; return NULL; }
            L->p++;                      /* 收 ) */
        } else {
            L->bad = 1;
            return NULL;
        }
    }
    return n;
}

Node *ast_load_text(const char *text, size_t len) {
    Loader L = { text, text + len, 0, 0 };
    Node *root = lnode(&L);
    if (!root || L.bad) return NULL;
    lskip(&L);
    if (L.p != L.end) return NULL;       /* 尾部还有内容 = 格式坏了 */
    return root;
}

Node *ast_load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return NULL; }
    buf[sz] = '\0';
    Node *root = ast_load_text(buf, (size_t)sz);
    free(buf);
    return root;
}
