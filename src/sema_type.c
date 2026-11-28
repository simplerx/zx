/* sema_type.c - 类型系统：整数折叠 / 类型表达式 / 聚合与方法 / 强转。
 * 从 sema.c 拆分；共享声明见 sema_internal.h。
 */
#include "sema_internal.h"


/* 把编译器内建名字并进一个模块作用域。
 * 模块自己声明了同名就以它为准 (scope_add 返回 NULL)，
 * 用户同名函数不会被内建劫持。
 * 入口单元也调这一份 (sema_check)：§158 前 LSP 把编辑器
 * 缓冲写成同目录影子文件单查，而 std/ 里的库文件
 * 在真实构建里是 @mod 模块，裸名 stackAlloc / file 都可用。
 * 入口/模块作用域不对称会让编辑器假报 E0401。 */
void seed_std_builtins(Scope *sc) {
    for (int i = 0; g_std_seeded[i]; i++) {
        Symbol *b = std_builtin(g_std_seeded[i]);
        if (!b) continue;
        Symbol *s = scope_add(sc, b->name, b->kind);
        if (!s) continue;
        sym_copy(s, b, b->name);
        s->is_pub = true;
    }
}

IntV intv(uint64_t v) { IntV r = { true, false, v }; return r; }
IntV noint(void)      { IntV r = { false, false, 0 }; return r; }

IntV ev_int(Node *n) {
    if (!n) return noint();
    switch (n->kind) {
    case NK_INT:
        if (n->flag) return noint();   /* 超 u64：128 位值不走 IntV，编译期算不了 */
        return intv(n->ival);

    case NK_CHAR:
        return intv(n->ival);

    case NK_BOOL:
        return intv(n->ival ? 1 : 0);

    case NK_UN: {
        IntV a = ev_int(n->a);
        if (!a.ok) return a;
        if (n->op == TOK_MINUS) { a.neg = !a.neg; return a; }
        if (n->op == TOK_PLUS) return a;
        if (n->op == TOK_TILDE) return intv(~a.v);
        if (n->op == TOK_BANG)  return intv(!a.v);
        return noint();
    }

    case NK_CAST: {
        IntV a = ev_int(n->b);
        return a;                        /* 截断交给后端 */
    }

    case NK_IDENT:
    case NK_TYPE_IDENT: {
        if (!n->sym) n->sym = lookup(n->name);
        if (!n->sym) return noint();
        ensure(n->sym);
        if (n->sym->ct && n->sym->ct->kind == V_INT) {
            IntV r = { true, n->sym->ct->neg, n->sym->ct->ival };
            return r;
        }
        if (n->sym->decl && n->sym->decl->b) return ev_int(n->sym->decl->b);
        return noint();
    }

    case NK_FIELD: {
        /* Color.green / ShapeTag.circle —— 枚举变体 */
        if (!n->a || !n->name) return noint();
        if (!n->sym) check_expr(n, NULL);
        Type *bt = n->a->ty;
        if (bt && bt->kind == TY_TYPE) bt = n->a->type_val;
        if (bt && bt->kind == TY_ENUM) {
            Field *f = ty_field(bt, n->name);
            if (f && f->has_tag) return intv(f->tag);
        }
        /* 聚合值的字段访问（.len / .name 之类）。
         * check_expr 已把它折进 n->ct。 */
        if (n->ct && n->ct->kind == V_INT) {
            IntV r = { true, n->ct->neg, n->ct->ival };
            return r;
        }
        return noint();
    }

    case NK_BUILTIN: {
        /* 1.2.2 起没有"直接给出整数"的内建了：`@type(T).size` 走 NK_FIELD，
         * check_expr 已经把值折进 n->ct。 */
        return noint();
    }

    case NK_BIN: {
        IntV a = ev_int(n->a), b = ev_int(n->b);
        if (!a.ok || !b.ok) return noint();
        uint64_t x = a.neg ? (uint64_t)(-(int64_t)a.v) : a.v;
        uint64_t y = b.neg ? (uint64_t)(-(int64_t)b.v) : b.v;
        switch (n->op) {
        case TOK_PLUS:  return intv(x + y);
        case TOK_MINUS: { bool neg = false; uint64_t r = x - y;
                          if (x < y) { neg = true; r = y - x; }
                          IntV o = { true, neg, r }; return o; }
        case TOK_STAR:  return intv(x * y);
        case TOK_SLASH: return y ? intv(x / y) : noint();
        case TOK_PERCENT: return y ? intv(x % y) : noint();
        case TOK_SHL:   return intv(x << (y & 63));
        case TOK_SHR:   return intv(x >> (y & 63));
        case TOK_AMP:   return intv(x & y);
        case TOK_PIPE:  return intv(x | y);
        case TOK_CARET: return intv(x ^ y);
        case TOK_EQ: case TOK_NE: case TOK_LT: case TOK_LE: case TOK_GT: case TOK_GE: {
            int64_t sx = a.neg ? -(int64_t)x : (int64_t)x;
            int64_t sy = b.neg ? -(int64_t)y : (int64_t)y;
            bool r = n->op == TOK_EQ ? sx == sy : n->op == TOK_NE ? sx != sy :
                     n->op == TOK_LT ? sx <  sy : n->op == TOK_LE ? sx <= sy :
                     n->op == TOK_GT ? sx >  sy : sx >= sy;
            return intv(r ? 1 : 0);
        }
        default: return noint();
        }
    }
    default:
        return noint();
    }
}

/* ---------------- 布局辅助 ---------------- */

Type *scalar_default(Type *t) {
    if (!t) return t;
    if (t->kind == TY_COMPTIME_INT)   return ty_i32();
    if (t->kind == TY_COMPTIME_FLOAT) return ty_f64();
    return t;
}

bool numeric_like(Type *t) {
    return t && (ty_is_num(t) || t->kind == TY_ENUM);
}

/* ---------------- 字面量拟合检查（M19） ----------------
 *
 * 字面量定型到具体整数类型时必须在范围内——这是语言发布前必须钉住的规则。
 * 超 u64 的大字面量（lexer 打 big 标记、文本在 n->str）只有 i128/u128 能
 * 接住，用十进制文本比较判定；有负号的一律拒绝（i64/i128 的最小值差一个
 * 数，混在字面量规则里得不偿失，要用就写 `(-9223372036854775807i64 - 1)`）。 */

static bool lit128_le(const char *digits, const char *max) {
    size_t a = strlen(digits), b = strlen(max);
    if (a != b) return a < b;
    return strcmp(digits, max) <= 0;
}

void lit_digits(Node *n, char *out, int cap) {
    int j = 0;
    if (n->str)
        for (const char *s = n->str; *s && j < cap - 1; s++)
            if (*s != '_') out[j++] = *s;
    out[j] = 0;
}

/* 纯整数字面量（含一元负号）的值。返回 false 表示不是纯字面量。 */
bool lit_value(Node *e, uint64_t *v, bool *neg, const char **text) {
    *neg = false;
    *text = NULL;
    if (!e) return false;
    if (e->kind == NK_INT) { *v = e->ival; *text = e->flag ? e->str : NULL; return true; }
    /* char 字面量也是字面量：`let c: u8 = '中';` 该报"装不下"，不是静默截断 */
    if (e->kind == NK_CHAR) { *v = e->ival; return true; }
    if (e->kind == NK_UN && e->op == TOK_MINUS && e->a && e->a->kind == NK_INT) {
        *neg = true;
        *v = e->a->ival;
        *text = e->a->flag ? e->a->str : NULL;
        return true;
    }
    return false;
}

static bool lit_fits_u64(uint64_t v, Type *t) {
    if (t->bits >= 128) return true;
    if (t->bits >= 64) return t->is_signed ? v <= 0x7fffffffffffffffULL : true;
    uint64_t max = t->is_signed ? (1ull << (t->bits - 1)) - 1 : (1ull << t->bits) - 1;
    return v <= max;
}

/* 当前字面量是否紧跟在一元 `-` 下（用于放行 INT_MIN：-2^(bits-1) 本身
 * 是字面量 max+1，取负后才落回范围内）。递归单线程，用静态标志安全。 */
bool g_neg_lit_ctx;

/* 128 位绝对值 {hi,lo}（带符号标志）能否装进整数类型 t */
static bool fit_big128(bool neg, uint64_t hi, uint64_t lo, Type *t) {
    if (!t || t->kind != TY_INT || t->bits != 128) return false;
    if (neg) {
        if (!t->is_signed) return hi == 0 && lo == 0;
        /* -mag >= -2^127 ⇔ mag <= 2^127（等于即类型最小值） */
        return hi < 0x8000000000000000ULL || (hi == 0x8000000000000000ULL && lo == 0);
    }
    if (t->is_signed) return hi < 0x8000000000000000ULL;
    return true;                             /* u128：128 位全装得下 */
}

/* 64 位绝对值 v（带符号标志）能否装进整数类型 t */
static bool fit_val(bool neg, uint64_t v, Type *t) {
    if (neg) {
        if (!t->is_signed) return v == 0;
        uint64_t mag = (t->bits >= 64) ? 0x8000000000000000ULL : (1ull << (t->bits - 1));
        return v <= mag;                     /* 允许恰好是 -2^(bits-1)（类型最小值） */
    }
    if (g_neg_lit_ctx && t->is_signed) {
        uint64_t mag = (t->bits >= 64) ? 0x8000000000000000ULL : (1ull << (t->bits - 1));
        if (v == mag) return true;           /* max+1，取负后是类型最小值 */
    }
    return lit_fits_u64(v, t);
}

/* 节点 e 的字面量能否装进整数类型 t。不是纯字面量时放行（管不着）。 */
bool lit_fits_expr(Node *e, Type *t) {
    if (!e || !t || t->kind != TY_INT) return true;
    uint64_t v; bool neg; const char *text;
    if (!lit_value(e, &v, &neg, &text)) {
        /* 折叠过的编译期值同样算"字面量"：`let x: i8 = 100 * 2;` 折叠成 200，
         * 装不进 i8 就该报错，不能静默截断（R10）。 */
        if (e->ct && e->ct->kind == V_INT && e->ct->ty &&
            e->ct->ty->kind == TY_COMPTIME_INT) {
            if (e->ct->big) return fit_big128(e->ct->neg, e->ct->hi, e->ct->ival, t);
            return fit_val(e->ct->neg, e->ct->ival, t);
        }
        return true;
    }
    if (text) {
        if (t->bits != 128) return false;
        char digits[64];
        lit_digits(e->a && e->a->kind == NK_INT ? e->a : e, digits, sizeof digits);
        if (neg) {                           /* 负的大字面量：绝对值上限是类型最小值的模 */
            if (!t->is_signed) return false;
            return lit128_le(digits, "170141183460469231731687303715884105728");
        }
        if (t->is_signed) {
            if (g_neg_lit_ctx)               /* -170141183460469231731687303715884105728 是 i128 最小值 */
                return lit128_le(digits, "170141183460469231731687303715884105728");
            return lit128_le(digits, "170141183460469231731687303715884105727");
        }
        return lit128_le(digits, "340282366920938463463374607431768211455");
    }
    return fit_val(neg, v, t);
}

/* ---------------- 免拷贝提升（R7） ---------------- */

bool can_coerce(Type *from, Type *to) {
    if (!from || !to) return true;
    if (from->kind == TY_INVALID || to->kind == TY_INVALID) return true;
    if (ty_eq(from, to)) return true;

    if (from->kind == TY_UNDEF) return true;

    /* `null` 的落点：`?T`（空）、裸指针/函数指针（C 的 NULL），以及
     * **`error`**——此时它就是保留码 65535，和 `error.null` 是同一个值
     * （用户 2026-11-09 第 15 条）。**不含 bool**：bool 只有 true/false
     * 两个值，`var b: bool = null;` 必须报错（用户 2026-11-16）。 */
    if (from->kind == TY_NULL)
        return to->kind == TY_OPT || to->kind == TY_PTR ||
               to->kind == TY_FN || to->kind == TY_ERROR;

    /* 目标是 ?T / !T：先拆包装，再看载荷（R7.4：T → ?T、T → !T） */
    if (to->kind == TY_OPT) return can_coerce(from, to->elem);
    if (to->kind == TY_ERR)
        return from->kind == TY_ERROR || can_coerce(from, to->elem);

    if (from->kind == TY_COMPTIME_INT)  return ty_is_int(to) || ty_is_float(to);
    if (from->kind == TY_COMPTIME_FLOAT) return ty_is_float(to);

    if (from->kind == TY_PTR && to->kind == TY_PTR) {
        /* §201：*[N]T 就是 elem 为 [N]T 的普通指针；→ *T 就是普通指针换 elem。 */
        if (ty_is_array_ptr(from) && !ty_is_array_ptr(to) &&
            ty_eq(from->elem->elem, to->elem))
            return true;
        /* 字符串字面量的历史形状（*[N]const u8）→ *const u8 */
        if (from->has_len && !to->has_len)
            return ty_eq(from->elem, to->elem) && (!from->is_const || to->is_const);
        if (to->has_len && !from->has_len) return false;   /* 显式断言 */
        if (!ty_eq(from->elem, to->elem)) return false;
        if (from->is_const && !to->is_const) return false;
        return true;
    }

    if (from->kind == TY_PTR && to->kind == TY_SLICE) {
        /* 数组指针按 elem=[N]T 拆；字符串字面量仍走 has_len 的历史形状。 */
        if (!ty_is_array_ptr(from) && !from->has_len) return false;
        Type *fe = ty_is_array_ptr(from) ? from->elem->elem : from->elem;
        if (!ty_eq(fe, to->elem)) return false;
        if (from->is_const && !to->is_const) return false;
        return true;
    }
    /* §201：[N]T 数组值 → []T 切片（首元素视图） */
    if (from->kind == TY_ARRAY && to->kind == TY_SLICE) {
        if (!ty_eq(from->elem, to->elem)) return false;
        return true;
    }

    if (from->kind == TY_SLICE && to->kind == TY_SLICE) {
        if (!ty_eq(from->elem, to->elem)) return false;
        if (from->is_const && !to->is_const) return false;
        return true;
    }

    if (from->kind == TY_FN && to->kind == TY_FN) return ty_eq(from, to);
    if (from->kind == TY_ENUM && to->kind == TY_INT) return false;   /* 要显式 */

    return false;
}

/* ---------------- 类型表达式 ---------------- */

static Type *resolve_struct_body(Node *n, Type *agg, bool is_union);
static Type *resolve_enum_body(Node *n, Type *e);
static IdentAnchor *g_anchors;
static int g_nanchors, g_canchors;

static void ident_anchor_set(Node *clone, Node *tmpl) {
    if (g_nanchors == g_canchors) {
        g_canchors = g_canchors ? g_canchors * 2 : 32;
        g_anchors = realloc(g_anchors, sizeof(IdentAnchor) * (size_t)g_canchors);
        if (!g_anchors) { fprintf(stderr, "zxc: out of memory\n"); exit(1); }
    }
    g_anchors[g_nanchors].clone = clone;
    g_anchors[g_nanchors].tmpl  = tmpl;
    g_nanchors++;
}

/* 类型 C 名用的 decl：克隆节点若在锚表里就换成模板节点。 */
Node *ident_decl(Node *n) {
    if (!n) return n;
    for (int i = 0; i < g_nanchors; i++)
        if (g_anchors[i].clone == n) return g_anchors[i].tmpl;
    return n;
}

/* 当前正在实例化的泛型参数名（anchor_types 期间有效）。 */
const char **g_ident_params;
int g_nident_params;

static bool subtree_refs_params(Node *n) {
    if (!n) return false;
    if (n->kind == NK_TYPE_IDENT || n->kind == NK_IDENT) {
        if (n->name) {
            for (int i = 0; i < g_nident_params; i++)
                if (g_ident_params[i] && !strcmp(g_ident_params[i], n->name))
                    return true;
        }
    }
    if (subtree_refs_params(n->a) || subtree_refs_params(n->b) ||
        subtree_refs_params(n->c) || subtree_refs_params(n->d))
        return true;
    if (n->items)
        for (int i = 0; i < n->nitems; i++)
            if (subtree_refs_params(n->items[i])) return true;
    return false;
}

/* 同步遍历模板 tmpl 与克隆 clone（结构一一对应），给"不依赖泛型参数"的
 * 类型声明节点登记身份锚点。 */
void anchor_types(Node *tmpl, Node *clone) {
    if (!tmpl || !clone) return;
    if (tmpl->kind == NK_STRUCT_TYPE || tmpl->kind == NK_UNION_TYPE ||
        tmpl->kind == NK_ENUM_TYPE) {
        if (!subtree_refs_params(tmpl)) ident_anchor_set(clone, tmpl);
    }
    anchor_types(tmpl->a, clone->a);
    anchor_types(tmpl->b, clone->b);
    anchor_types(tmpl->c, clone->c);
    anchor_types(tmpl->d, clone->d);
    if (tmpl->items && clone->items && tmpl->nitems == clone->nitems)
        for (int i = 0; i < tmpl->nitems; i++)
            anchor_types(tmpl->items[i], clone->items[i]);
}

Type *resolve_type(Node *n) {
    if (!n) return ty_void();

    switch (n->kind) {
    case NK_TYPE_IDENT: {
        Type *b = base_type(n->name);
        if (b) { n->ty = ty_type(); n->type_val = b; return b; }
        Symbol *s = lookup(n->name);
        if (!s) {
            const char *sug = suggest_ident(n->name);
            if (sug) diag_help("did you mean `%s`?", sug);
            BAD(n, "unknown type '%s'", n->name);
            return ty_invalid();
        }
        /* 正在构造的聚合类型：`struct { next: *Node }` 里的 Node 就是它自己（R5）。
         * 只给外壳，成员表填完之前不能用它算大小。 */
        if (s->state == 3 && s->type_val) {
            n->ty = ty_type();
            n->type_val = s->type_val;
            return s->type_val;
        }
        ensure(s);
        n->sym = s;   /* 点路径（std.heap.page）的成员查找靠它进命名空间分支 */
        if (s->ty && s->ty->kind == TY_TYPE) {
            n->ty = ty_type();
            n->type_val = s->type_val ? s->type_val : ty_invalid();
            return n->type_val;
        }
        return BAD(n, "'%s' is not a type", n->name);
    }

    case NK_VEC_TYPE: {
        /* @Vec(T, N)（§162）：T 是数值元素类型，N 是 1/2/4/8/16/32/64。
         * 长度必须是 2 的幂——后端的 C 用的是 vector_size，它也只接受这种。 */
        int vl = n->line, vc = n->col;
        Type *elem = resolve_type(n->a);
        if (elem->kind != TY_INVALID && !ty_is_num(elem))
            return BAD(n->a, "@Vec element type must be numeric, got `%s`", ty_str(elem));
        IntV v = ev_int(n->b);
        if (!v.ok || v.neg)
            return BAD(n->b, "@Vec length must be a non-negative compile-time integer");
        if (v.v == 0 || v.v > 64 || (v.v & (v.v - 1)) != 0) {
            diag_code_at("E0305", vl, vc,
                         "@Vec length must be 1, 2, 4, 8, 16, 32 or 64 (got %llu)",
                         (unsigned long long)v.v);
            return ty_invalid();
        }
        return ty_vec(elem, v.v);
    }

    case NK_PTR_TYPE: {
        Type *elem = resolve_type(n->a);
        /* §201：`*[N]T` 就是普通指针，elem 是 TY_ARRAY，长度信息在 elem 里。
         * has_len 只留给字符串字面量等历史形状，这里一律 false。 */
        return ty_ptr(elem, n->flag, false, 0);
    }

    case NK_SLICE_TYPE:
        return ty_slice(resolve_type(n->a), n->flag);

    case NK_OPT_TYPE: {
        Type *e = resolve_type(n->a);
        if (e->kind == TY_OPT || e->kind == TY_ERR) {
            BAD(n, "?T / !T cannot nest (got %s)", ty_str(e));
            return ty_invalid();
        }
        return ty_opt(e);
    }

    case NK_ERR_SET: {
        /* 集合字面量里的名字有两重身份：引用已声明的错误集时是**合并**
         * （`const All = error{ IoErr, extra }` 把 IoErr 的码并进来），
         * 其余是普通成员名——成员不需要声明，查不到符号很正常。
         * 只认“类型值恰好是错误集”的名字：同名函数/常量不合并，
         * 照旧当成员。解析中的符号（state==1，互相引用）也当成员，
         * 让循环引用自然落在字面量一侧而不是挂死。 */
        void *refs = NULL;
        const char *names[64];
        int k = 0;
        for (int i = 0; i < n->nitems; i++) {
            Node *m = n->items[i];
            if (!m || !m->name) continue;
            Symbol *s = lookup(m->name);
            if (s && s->state != 1 && s->ty && s->ty->kind == TY_TYPE &&
                s->type_val && s->type_val->kind == TY_ERRORSET) {
                refs = refs ? ty_errset_union(refs, s->type_val->eset)
                            : s->type_val->eset;
                continue;
            }
            /* `null` 是保留码 65535（`?T` 被 try 时抛的"空"），不能声明成
             * 命名成员：`error{null}` 会拿到 65535，和"没有值"撞车（E0508）。
             * 用户 2026-11-09：`error.null` 这个**值**要能写，但集合里不能
             * 再起一个同名成员（spec §5.2）。 */
            if (!strcmp(m->name, "null"))
                return BAD_CODE("E0508", m, "`null` is the reserved error code 65535 "
                                            "(what a null optional throws) -- pick another name");
            if (k < 64) names[k++] = m->name;
        }
        void *lit = ty_errset_of(names, k);
        return ty_errset_type(refs ? ty_errset_union(refs, lit) : lit);
    }

    case NK_ERR_TYPE: {
        if (n->b) {                       /* E!T：左边是错误集，右边是载荷 */
            Type *st = resolve_type(n->a);
            Type *e  = resolve_type(n->b);
            if (e->kind == TY_OPT || e->kind == TY_ERR) {
                BAD(n, "?T / !T cannot nest (got %s!%s)", ty_str(st), ty_str(e));
                return ty_invalid();
            }
            if (st->kind != TY_ERRORSET) {
                BAD(n->a, "the left side of `!` must be an error set "
                          "(e.g. `error{open, read}!i32`), found `%s`", ty_str(st));
                return ty_invalid();
            }
            return ty_err_set(e, ty_type_errset(st));
        }
        Type *e = resolve_type(n->a);
        if (e->kind == TY_OPT || e->kind == TY_ERR) {
            BAD(n, "?T / !T cannot nest (got %s)", ty_str(e));
            return ty_invalid();
        }
        return ty_err(e);
    }

    case NK_TUPLE_TYPE: {
        Type **items = xcalloc((size_t)(n->nitems ? n->nitems : 1), sizeof(Type *));
        for (int i = 0; i < n->nitems; i++) items[i] = resolve_type(n->items[i]);
        Type *t = ty_tuple(items, n->nitems);
        free(items);
        return t;
    }

    case NK_REPEAT_TYPE: {
        /* §201：[N]T 数组值类型（取代旧的 [T; N] 内联元组）。占 N×size(T)
         * 栈内存，内建 .ptr/.len；不再是 TY_TUPLE，也不再按元组发码。
         * 节点沿用 NK_REPEAT_TYPE（字段布局 a=元素类型, b=长度不变），
         * 避免自举前端 / zxls 的节点编号镜像漂移。 */
        Type *elem = resolve_type(n->a);
        if (elem->kind == TY_INVALID) return ty_invalid();
        IntV v = ev_int(n->b);
        if (!v.ok || v.neg)
            return BAD(n->b, "array length must be a non-negative compile-time integer");
        /* §201：TY_ARRAY 是 struct{ T v[N]; }，不再像旧 TY_TUPLE 那样
         * 展开 N 份字段；上限只防 C 数组维度溢出，放宽到 int 范围。 */
        if (v.v == 0 || v.v > 0x3fffffffULL) {
            diag_code_at("E0305", n->line, n->col,
                         "array length must be 1..1073741823 (got %llu)",
                         (unsigned long long)v.v);
            return ty_invalid();
        }
        Type *t = ty_array(elem, v.v);
        n->ty = ty_type();
        n->type_val = t;
        return t;
    }

    case NK_STRUCT_TYPE: {
        Type *t = ty_agg(TY_STRUCT, ident_decl(n), NULL);
        return resolve_struct_body(n, t, false);
    }

    case NK_UNION_TYPE: {
        Type *t = ty_agg(TY_UNION, ident_decl(n), NULL);
        return resolve_struct_body(n, t, true);
    }

    case NK_ENUM_TYPE: {
        Type *t = ty_agg(TY_ENUM, ident_decl(n), NULL);
        return resolve_enum_body(n, t);
    }

    case NK_FN: {
        /* 函数类型（也可能带体，但类型部分一样） */
        Type *t = resolve_fn_type(n);
        n->ty = ty_type();
        n->type_val = t;
        return t;
    }

    case NK_CALL: {
        /* List(i32) / Buffer(u8, 4)：泛型实例化 = 编译期调用 */
        Type *t = instantiate_type(n);
        n->ty = ty_type();
        n->type_val = t;
        if (t && t->kind != TY_INVALID) n->ct = val_type(t);
        return t;
    }

    /* 表达式形式的类型值：@typeOf(x) 等 */
    default: {
        Type *t = check_expr(n, NULL);
        if (t && t->kind == TY_TYPE) return n->type_val ? n->type_val : ty_invalid();
        return BAD(n, "expected a type, found a value of type `%s`", ty_str(t));
    }
    }
}

/* struct / union 体：字段、静态成员。
 * union 成员是 `name: T`；带标签联合体写成 `name: {Tag.variant, Payload}`。 */
/* 同一个类型可能被实例化好几次，方法接收者的报错会重复；按源码位置去重 */
static int g_mrecv_diag[8][2], g_nmrecv_diag;
static bool mrecv_dup(Node *it) {
    for (int i = 0; i < g_nmrecv_diag; i++)
        if (g_mrecv_diag[i][0] == it->line && g_mrecv_diag[i][1] == it->col) return true;
    if (g_nmrecv_diag < 8) {
        g_mrecv_diag[g_nmrecv_diag][0] = it->line;
        g_mrecv_diag[g_nmrecv_diag][1] = it->col;
        g_nmrecv_diag++;
    }
    return false;
}

/* §224：接收者形态完全由写法决定，编译器不再按大小改 ABI。`self: Self`
 * 是普通按值参数（进函数时复制一份）；要指针必须显式写：`self: *const Self`
 * 只读、`self: *Self` 可改调用者本身。§274 只在调用点转换不可变接收者：
 * 值可自动取只读地址，指针可自动解引用后按值传递。 */

/* 方法表：函数体里的第一个参数就是接收者。表先填满签名再查体，
 * 这样方法之间互相调用（self.other()）不用关心谁先谁后。
 * 结构体 / 联合体 / 枚举共用这一份——方法本来就是"第一个参数是接收者的函数"。 */
static void collect_methods(Node *n, Type *agg, const char *what) {
    int nmeth = 0;
    for (int i = 0; i < n->nitems; i++)
        if (n->items[i]->kind == NK_FN) nmeth++;
    if (!nmeth) return;

    agg->methods = xcalloc((size_t)nmeth, sizeof(Method));
    for (int i = 0; i < n->nitems; i++) {
        Node *it = n->items[i];
        if (it->kind != NK_FN) continue;
        Type *ft = resolve_fn_type(it);
        if (!ft || ft->kind != TY_FN) continue;

        /* 第一参数叫 `self` 就是方法（`Self` / `*Self` / `*const Self` 三种
         * 形态各按写法生效，编译器不改）；不叫 self 就是挂在类型上的
         * 普通函数（`Ints.init(alloc)`）。 */
        Node *p0 = it->nitems > 0 ? it->items[0] : NULL;
        bool has_self = p0 && p0->name && !strcmp(p0->name, "self");

        if (!has_self) {
            if (ty_method(agg, it->name)) continue;
            Method *m0 = &agg->methods[agg->nmethods++];
            m0->name = it->name;
            m0->fn   = it;
            m0->fnty = ft;
            m0->has_self = false;
            m0->is_pub = it->is_pub;
            schedule_body(it);
            continue;
        }

        Type *r0 = ft->params[0];
        /* §224：接收者写什么形态就是什么形态，编译器不再按大小改写。
         * 认三种：`Self`（按值，复制一份）、`*Self`（可改调用者本身）、
         * `*const Self`（只读）。指针只允许指向聚合体本身——带长度的
         * 切片指针、数组指针都不算接收者形态。 */
        bool recv_ptr = r0->kind == TY_PTR && !r0->has_len && !ty_is_array_ptr(r0);
        Type *base = recv_ptr ? r0->elem : r0;
        if (base != agg && base->kind != TY_INVALID && r0->kind != TY_INVALID && !mrecv_dup(it))
            err_at(it->line, it->col,
                   "method '%s' must take the %s as its first parameter "
                   "(`Self`, `*Self` or `*const Self`), found `%s`",
                   it->name ? it->name : "?", what, ty_str(r0));

        if (ty_field(agg, it->name) || ty_static_member(agg, it->name))
            err_at(it->line, it->col,
                   "'%s' is already a member; a method cannot share its name",
                   it->name ? it->name : "?");
        for (int k = 0; k < ft->nparams; k++)
            if (ft->params[k]->kind == TY_TYPE) {
                err_at(it->line, it->col, "method '%s' cannot be generic",
                       it->name ? it->name : "?");
                break;
            }
        if (ty_method(agg, it->name)) continue;      /* 重名已经报过 */
        Method *m = &agg->methods[agg->nmethods++];
        m->name = it->name;
        m->fn   = it;
        m->fnty = ft;
        m->has_self = true;
        m->is_pub = it->is_pub;
        schedule_body(it);
    }
}

/* extern struct / extern union 的字段类型必须映射成 C 布局兼容的东西。
 * 返回 false 时已经报过错。递归校验：嵌套聚合体必须自己也是 extern 的，
 * 定长数组（*[N]T）逐元素校验，函数指针逐参数校验。 */
bool check_c_able(Node *at, Type *t, const char *fname) {
    if (!t || t->kind == TY_INVALID) return false;
    switch (t->kind) {
    case TY_INT:
        if (t->bits == 1) {
            err_at(at->line, at->col,
                   "field '%s': i1 has no C equivalent (use bool or u8)",
                   fname ? fname : "?");
            return false;
        }
        return true;
    case TY_FLOAT: case TY_BOOL: case TY_ENUM:
        return true;
    case TY_ARRAY:                    /* §201：[N]T 作为 extern 字段展开成 C 数组 */
        return check_c_able(at, t->elem, fname);
    case TY_PTR:
        /* 指针就是一个机器字：指向什么都不影响**这个**结构体的布局。
         * 定长数组指针（*[N]T）例外——它作为 extern 字段要展开成 C 数组，
         * 元素类型必须同样 C 兼容。 */
        if (ty_is_array_ptr(t)) return check_c_able(at, t->elem, fname);
        return true;
    case TY_FN: {
        bool ok = t->ret ? check_c_able(at, t->ret, fname) : true;
        for (int i = 0; ok && i < t->nparams; i++)
            ok = check_c_able(at, t->params[i], fname);
        return ok;
    }
    case TY_STRUCT: case TY_UNION:
        if (t->decl && t->decl->is_extern) return true;
        err_at(at->line, at->col,
               "field '%s': nested aggregates in `extern struct`/`extern union` "
               "must be `extern` too (Zx aggregates carry runtime metadata; "
               "the C layout is not guaranteed to match)",
               fname ? fname : "?");
        return false;
    case TY_OPT:
        err_at(at->line, at->col,
               "field '%s': ?T carries a Zx tag bool -- a C pointer is just "
               "`*T` (null is 0)", fname ? fname : "?");
        return false;
    case TY_SLICE:
        err_at(at->line, at->col,
               "field '%s': []T is a {ptr, len} pair with no C equivalent -- "
               "pass a pointer `*T` and the length as separate fields",
               fname ? fname : "?");
        return false;
    case TY_ERR: case TY_ERRORSET:
        err_at(at->line, at->col,
               "field '%s': error unions are Zx-only, they never cross the FFI boundary",
               fname ? fname : "?");
        return false;
    case TY_TUPLE:
        err_at(at->line, at->col,
               "field '%s': use an `extern struct` instead of a tuple for C layout",
               fname ? fname : "?");
        return false;
    default:
        err_at(at->line, at->col,
               "field '%s': `%s` has no C-compatible layout",
               fname ? fname : "?", ty_str(t));
        return false;
    }
}

static Type *resolve_struct_body(Node *n, Type *agg, bool is_union) {
    if (agg->fields != NULL) return agg;              /* 已解析 */
    /* 成员默认只在本文件可见：把声明文件记在类型上（声明处的模块先由
     * sema 打戳进 Node.mod，泛型单态化克隆 AST 时一并带过来）。 */
    agg->mod = n->mod;

    /* extern struct / extern union（FFI）：镜像外部 C 结构体的布局。
     * 只允许"数据"——方法、静态成员、字段默认值都不进布局，一律拒绝。 */
    bool ext = n->is_extern;
    if (ext) {
        /* packed 已整体删除（§192）：struct(packed) 在解析层就报 E0211，
         * 这里不再可能有带 flag 的 struct 进来。 */
        if (is_union && n->op == 1)
            err_at(n->line, n->col,
                   "extern union cannot be union(enum) -- tagged unions are a "
                   "Zx layout ({payload, tag}); a C union is plain");
        for (int i = 0; i < n->nitems; i++) {
            Node *it = n->items[i];
            if (it->kind == NK_FN)
                err_at(it->line, it->col,
                       "extern struct/union cannot have methods -- attach free "
                       "functions to the type from outside");
            else if (it->kind == NK_DECL)
                err_at(it->line, it->col,
                       "extern struct/union cannot have static members -- "
                       "only plain `name: Type` fields");
        }
    }

    Scope sc;
    scope_push(&sc);

    /* `Self`：体里指自己。泛型类型体里尤其重要——`self: *Self` 不必把
     * T / cap 再写一遍，也就不会在"实例化自身"那里绕回来。 */
    {
        Symbol *s = scope_add(&sc, "Self", SK_CONST);
        if (s) { s->ty = ty_type(); s->type_val = agg; s->state = 2; }
    }

    /* 先 hoist 静态成员（`var n: i32 = 10;` / `const LIMIT = 100;`） */
    int nfields = 0;
    for (int i = 0; i < n->nitems; i++) {
        Node *it = n->items[i];
        if (it->kind == NK_PARAM || it->kind == NK_PAIR) {
            /* 字段名也查重。以前只查了静态成员和方法，`struct { a: i32, a: i32 }`
             * 会静默造出两个同名字段——字面量 `D{ .a = 1 }` 永远"少填一个"，
             * 报出来的却是 E0300 missing field，读者根本猜不到原因。 */
            if (it->name) {
                bool dup = false;
                for (int j = 0; j < i && !dup; j++) {
                    Node *pv = n->items[j];
                    if (pv->name && !strcmp(pv->name, it->name)) {
                        diag_note(NULL, pv->line, pv->col, "'%s' first declared here", it->name);
                        err_at(it->line, it->col, "duplicate field '%s' in `%s`",
                               it->name, is_union ? "union" : "struct");
                        dup = true;
                    }
                }
                if (!dup) {
                    Symbol *prev = scope_find_here(&sc, it->name);
                    if (prev) {
                        if (prev->decl_line)
                            diag_note(NULL, prev->decl_line, prev->decl_col,
                                      "'%s' first declared here", it->name);
                        err_at(it->line, it->col, "duplicate member '%s'", it->name);
                    }
                }
            }
            nfields++;
        } else if (it->kind == NK_DECL && it->name) {
            Symbol *s = scope_add_at(&sc, it->name, it->op == 2 ? SK_VAR : SK_CONST, it->line, it->col);
            if (!s) {
                Symbol *prev = scope_find_here(&sc, it->name);
                if (prev && prev->decl_line)
                    diag_note(NULL, prev->decl_line, prev->decl_col, "'%s' first declared here", it->name);
                err_at(it->line, it->col, "duplicate member '%s'", it->name);
            }
            else { s->decl = it; it->sym = s; }
            nfields++;
        } else if (it->kind == NK_FN) {
            Symbol *s = scope_add_at(&sc, it->name ? it->name : "?", SK_FN, it->line, it->col);
            if (!s) {
                Symbol *prev = scope_find_here(&sc, it->name ? it->name : "?");
                if (prev && prev->decl_line)
                    diag_note(NULL, prev->decl_line, prev->decl_col, "'%s' first declared here", it->name);
                err_at(it->line, it->col, "duplicate member '%s'", it->name ? it->name : "?");
            }
            else { s->decl = it; it->sym = s; s->in_agg = n; }
        }
    }

    Field *fs = xcalloc((size_t)(nfields ? nfields : 1), sizeof(Field));
    int k = 0;
    bool tagged = false;
    Type *tag_ty = NULL;

    /* union(enum) / union(enum(u8)) 糖（M22）：变体名即隐式枚举。
     * 合成一个 TY_ENUM 当标签类型（变体按声明顺序取值 0..n-1，底层类型
     * 可用 union(enum(u8)) 指定），每个变体的载荷就是成员类型本身——
     * 之后 is_tagged / tag_ty / .tag / .u.成员 / 构造写标签全部复用
     * 显式标签联合体那套机制。 */
    bool sugar = is_union && n->op == 1;
    int vidx = 0;
    if (sugar) {
        Type *et = ty_agg(TY_ENUM, n, NULL);
        et->elem = n->b ? resolve_type(n->b) : ty_int(32, false);
        int cnt = 0;
        for (int i = 0; i < n->nitems; i++)
            if (n->items[i]->kind == NK_PAIR) cnt++;
        et->fields = xcalloc((size_t)(cnt ? cnt : 1), sizeof(Field));
        et->nfields = cnt;
        tag_ty = et;
    }

    for (int i = 0; i < n->nitems; i++) {
        Node *it = n->items[i];

        if (it->kind == NK_DECL) {
            /* 类型/初值都放到第二遍算：静态成员可以就用这个类型本身
             * （`const ZERO = Vec{ .x = 0, .y = 0 };`），那时候字段还没填好。 */
            fs[k].name = it->name;
            fs[k].is_static = true;
            fs[k].is_var = it->op == 2;
            fs[k].dflt = it->b;
            fs[k].is_pub = it->is_pub;
            it->index = k;
            k++;
            continue;
        }
        if (it->kind == NK_FN) continue;   /* 方法：方法表里统一处理 */

        const char *fname = it->name;
        Type *ft = NULL;
        if (sugar) {
            /* union(enum)：成员类型就是载荷。注意这个分支必须优先——
             * 载荷本身完全可以是元组（`rect: {f64, f64}`），不能让旧式的
             * "{标签, 载荷}" 元组检测抢先把它当成标签元组。 */
            ft = it->a ? resolve_type(it->a) : ty_invalid();
            fs[k].has_tag = true;
            fs[k].tag = (uint64_t)vidx;
            tagged = true;
            /* 隐式枚举的变体同步登记（switch 模式 / 打印 / 比较 都查它） */
            Field *ef = &tag_ty->fields[vidx];
            ef->name = fname;
            ef->ty = tag_ty;
            ef->has_tag = true;
            ef->tag = (uint64_t)vidx;
            ef->is_pub = true;
            vidx++;
        } else if (is_union && it->a && it->a->kind == NK_TUPLE_TYPE && it->a->nitems == 2) {
            Node *t0 = it->a->items[0];
            if (t0->kind == NK_FIELD || t0->kind == NK_INT) {
                Type *tt = check_expr(t0, NULL);
                IntV tv = ev_int(t0);
                ft = resolve_type(it->a->items[1]);
                if (tv.ok) { fs[k].has_tag = true; fs[k].tag = tv.v; tagged = true; }
                if (!tag_ty) tag_ty = tt;
            }
        }
        if (!ft) ft = it->a ? resolve_type(it->a) : ty_invalid();
        if (ft == agg)
            err_at(it->line, it->col,
                   "field '%s' contains the %s by value; a recursive type needs a pointer "
                   "(write `*T` instead of `T`)",
                   fname ? fname : "?", is_union ? "union" : "struct");
        fs[k].name = fname;
        fs[k].ty = ft;
        fs[k].dflt = it->b;
        fs[k].is_pub = sugar ? true : it->is_pub;   /* union(enum) 变体是值，恒公开 */
        k++;
        /* 默认值按字段类型检查 */
        if (it->b) {
            if (ext)
                err_at(it->line, it->col,
                       "field '%s': extern struct/union fields cannot have "
                       "default values (the C side owns the layout)",
                       fname ? fname : "?");
            coerce_node(it->b, check_expr(it->b, ft), ft, "field default");
        }
        /* extern 聚合体：字段类型必须 C 布局兼容 */
        if (ext && ft != agg) check_c_able(it, ft, fname);
    }

    agg->fields = fs;
    agg->nfields = nfields;
    agg->is_tagged = tagged;
    agg->tag_ty = tag_ty;

    /* 第二遍：静态成员。这时 agg 的字段已经就位，`Vec{ … }` 这种自己当类型的
     * 初值才算是完整的值。 */
    for (int i = 0; i < n->nitems; i++) {
        Node *it = n->items[i];
        if (it->kind != NK_DECL) continue;
        ensure(it->sym);
        Type *ft = (it->sym && it->sym->ty) ? it->sym->ty : ty_invalid();
        if (it->index >= 0 && it->index < nfields) agg->fields[it->index].ty = ft;
        /* 编译期值槽（§117）：const 静态成员的初值此刻（泛型实参 T / cap 还
         * 在作用域里）已经折进 sym->ct，抄进 Field 留作后用。惰性引用——
         * 尤其 `Slots(P3,4).sz2` 这种链式实例——发生在作用域弹掉之后。
         * var 成员可变不进槽；类型成员没有"值"，走 type_val 那条路。 */
        if (it->index >= 0 && it->index < nfields && it->op != 2 &&
            it->sym && it->sym->ct &&
            !(it->sym->ty && it->sym->ty->kind == TY_TYPE))
            agg->fields[it->index].ct = it->sym->ct;
    }

    /* 方法：函数体里的第一个参数就是接收者 */
    collect_methods(n, agg, is_union ? "union" : "struct");

    scope_pop(&sc);
    return agg;
}

static Type *resolve_enum_body(Node *n, Type *e) {
    if (e->fields != NULL) return e;                  /* 已解析 */
    e->mod = n->mod;
    e->elem = n->a ? resolve_type(n->a) : ty_int(32, false);  /* 底层，默认 u32 */

    /* 枚举体也是作用域：变体之外还能写 `const` 和 `fn`（方法）。 */
    Scope sc;
    scope_push(&sc);
    {
        Symbol *s = scope_add(&sc, "Self", SK_CONST);
        if (s) { s->ty = ty_type(); s->type_val = e; s->state = 2; }
    }

    int cnt = 0, nst = 0;
    for (int i = 0; i < n->nitems; i++)
        if (n->items[i]->kind == NK_PAIR) cnt++;

    /* hoist 成员名（常量 / 函数），它们的值稍后算 */
    for (int i = 0; i < n->nitems; i++) {
        Node *it = n->items[i];
        if (it->kind == NK_DECL && it->name) {
            Symbol *s = scope_add_at(&sc, it->name, it->op == 2 ? SK_VAR : SK_CONST, it->line, it->col);
            if (!s) {
                Symbol *prev = scope_find_here(&sc, it->name);
                if (prev && prev->decl_line)
                    diag_note(NULL, prev->decl_line, prev->decl_col, "'%s' first declared here", it->name);
                err_at(it->line, it->col, "duplicate member '%s'", it->name);
            }
            else { s->decl = it; it->sym = s; }
            nst++;
        } else if (it->kind == NK_FN) {
            Symbol *s = scope_add_at(&sc, it->name ? it->name : "?", SK_FN, it->line, it->col);
            if (!s) {
                Symbol *prev = scope_find_here(&sc, it->name ? it->name : "?");
                if (prev && prev->decl_line)
                    diag_note(NULL, prev->decl_line, prev->decl_col, "'%s' first declared here", it->name);
                err_at(it->line, it->col, "duplicate member '%s'",
                       it->name ? it->name : "?");
            }
            else { s->decl = it; it->sym = s; s->in_agg = n; }
        }
    }

    e->fields = xcalloc((size_t)(cnt ? cnt : 1), sizeof(Field));
    e->nfields = cnt;
    if (nst) e->statics = xcalloc((size_t)nst, sizeof(Field));

    uint64_t next = 0;
    int k = 0;
    for (int i = 0; i < n->nitems; i++) {
        Node *m = n->items[i];
        if (m->kind != NK_PAIR) continue;
        Field *f = &e->fields[k++];
        f->name = m->name;
        f->ty = e;
        f->has_tag = true;
        f->is_pub = true;      /* 变体是值，不是可隐藏的内部成员 */
        if (m->a) {
            IntV v = ev_int(m->a);
            if (!v.ok) { BAD(m->a, "enum value must be a compile-time integer"); f->tag = next; }
            else f->tag = v.v;
        } else {
            f->tag = next;
            m->ival = next;      /* 记下来，后端不用再算 */
        }
        next = f->tag + 1;
    }

    /* 第二遍：常量成员。这时变体已经就位，`const FIRST = Color.red;` 才算得出来。 */
    int si = 0;
    for (int i = 0; i < n->nitems; i++) {
        Node *it = n->items[i];
        if (it->kind != NK_DECL) continue;
        Field *f = &e->statics[si++];
        f->name = it->name;
        f->is_static = true;
        f->is_var = it->op == 2;
        f->dflt = it->b;
        f->is_pub = it->is_pub;
        ensure(it->sym);
        f->ty = (it->sym && it->sym->ty) ? it->sym->ty : ty_invalid();
        /* 编译期值槽（§117）：同 struct 静态成员。 */
        if (!f->is_var && it->sym && it->sym->ct &&
            !(f->ty && f->ty->kind == TY_TYPE))
            f->ct = it->sym->ct;
    }
    e->nstatics = si;

    collect_methods(n, e, "enum");

    scope_pop(&sc);
    return e;
}

/* 函数签名。参数名可省略（fn 类型）。 */
Type *resolve_fn_type(Node *fn) {
    if (fn->ty && fn->ty->kind == TY_TYPE && fn->type_val) return fn->type_val;

    int np = fn->nitems;
    Type **ps = xcalloc((size_t)(np ? np : 1), sizeof(Type *));
    for (int i = 0; i < np; i++) {
        Node *pm = fn->items[i];
        ps[i] = pm->a ? resolve_type(pm->a) : ty_invalid();
        pm->ty = ps[i];
    }
    Type *ret = fn->a ? resolve_type(fn->a) : ty_void();
    /* 光秃秃的 `error` / `error{a, b}` 不是返回类型：它只说了"码"，没说这条
     * 返回路径能不能失败（用户 2026-11-09：要么 `error.x`、要么
     * `error{xx}!void`、要么 `!void`）。放在这里是因为所有 fn 类型都过这道门
     * ——写成别名（`const E = error;`）也拦得住。 */
    if (ret->kind == TY_ERROR || ret->kind == TY_ERRORSET) {
        diag_code_at("E0308", fn->a ? fn->a->line : fn->line,
                     fn->a ? fn->a->col : fn->col,
                     "`%s` is not a return type: it does not say whether the "
                     "function can fail", ty_str(ret));
        /* 修法由 errors.c 的 E0308 目录文本给（`zxc explain E0308` 同一句），
         * 这里不重复。 */
        ret = ty_invalid();
    }
    Type *t = ty_fn(ps, np, ret);
    free(ps);
    fn->ty = ty_type();
    fn->type_val = t;
    return t;
}

/* ---------------- 统一两个类型（臂 / 收割 / else） ---------------- */

Type *unify(Node *at, Type *a, Type *b) {
    if (!a) return b;
    if (!b) return a;
    if (a->kind == TY_INVALID || b->kind == TY_INVALID) return ty_invalid();
    if (ty_eq(a, b)) return a;

    /* 两个长度不同的数组指针（典型：switch 各臂返回不同长度的字符串字面量）
     * 统一成切片：`*[4]const u8` 与 `*[5]const u8` → `[]const u8`。长度是
     * 类型的一部分，硬要相等的话每个臂都得显式转换，没法写出
     * `switch (k) { .ident: "IDENT", … }`。 */
    if (ty_is_array_ptr(a) && ty_is_array_ptr(b) && ty_eq(a->elem, b->elem))
        return ty_slice(a->elem->elem, a->is_const || b->is_const);
    /* 字符串字面量历史形状：长度不同但 elem 都是 u8 → 统一成 []const u8 */
    if (a->kind == TY_PTR && b->kind == TY_PTR && a->has_len && b->has_len &&
        ty_eq(a->elem, b->elem))
        return ty_slice(a->elem, a->is_const || b->is_const);

    if (a->kind == TY_COMPTIME_INT && ty_is_num(b)) return b;
    if (b->kind == TY_COMPTIME_INT && ty_is_num(a)) return a;
    if (a->kind == TY_COMPTIME_FLOAT && ty_is_float(b)) return b;
    if (b->kind == TY_COMPTIME_FLOAT && ty_is_float(a)) return a;

    if (a->kind == TY_NULL && b->kind == TY_OPT) return b;
    if (b->kind == TY_NULL && a->kind == TY_OPT) return a;
    /* 指针臂与 null 臂统一成可选指针（§170 分支传出："释放后返回空指针"
     * 就落在这里——`ok: b;` 与 `ok: null;` 的公共类型是 `?*[N]T`） */
    if (a->kind == TY_NULL && b->kind == TY_PTR) return ty_opt(b);
    if (b->kind == TY_NULL && a->kind == TY_PTR) return ty_opt(a);

    if (a->kind == TY_ERROR) return ty_err(b);
    if (b->kind == TY_ERROR) return ty_err(a);

    if (can_coerce(a, b)) return b;
    if (can_coerce(b, a)) return a;

    if (at) err_at(at->line, at->col, "mismatched types: `%s` and `%s`", ty_str(a), ty_str(b));
    return ty_invalid();
}

/* ---------------- 赋值 / lvalue ---------------- */

bool lvalue_mutable(Node *n) {
    if (!n) return false;
    switch (n->kind) {
    case NK_IDENT:
        if (!n->sym) n->sym = lookup(n->name);
        return n->sym && n->sym->kind == SK_VAR;
    case NK_FIELD: {
        Type *bt = n->a ? n->a->ty : NULL;
        if (bt && bt->kind == TY_PTR) return !bt->is_const;
        if (bt && bt->kind == TY_TYPE) {           /* 静态成员：Counter.n */
            Type *bt = n->a->type_val ? n->a->type_val : ty_invalid();
            Field *f = ty_field(bt, n->name);
            if (!f) f = ty_static_member(bt, n->name);   /* 枚举体的 var 成员 */
            return f && f->is_static && f->is_var;
        }
        return lvalue_mutable(n->a);
    }
    case NK_INDEX:
    case NK_SLICE: {
        Type *bt = n->a ? n->a->ty : NULL;
        if (bt && (bt->kind == TY_SLICE || bt->kind == TY_PTR)) return !bt->is_const;
        return lvalue_mutable(n->a);
    }
    case NK_UN:
        return n->op == TOK_AMP && lvalue_mutable(n->a);
    default:
        return false;
    }
}

/* ---------------- 表达式检查 ---------------- */

Type *type_of(Node *n, Type *t) { n->ty = t; return t; }
