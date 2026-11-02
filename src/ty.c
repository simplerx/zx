/* ty.c — type construction, interning, layout, printing. */
#include "ty.h"
#include "ast.h"

static Type **g_pool;
static int    g_npool, g_cappool;

static Type *ty_new(TyKind k) {
    Type *t = calloc(1, sizeof(Type));
    if (!t) { fprintf(stderr, "zxc: out of memory\n"); exit(1); }
    t->kind  = k;
    t->size  = -1;
    t->align = -1;
    return t;
}

static bool same(const Type *a, const Type *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    if (a->kind == TY_INVALID) return true;

    switch (a->kind) {
    case TY_INT:
        return a->bits == b->bits && a->is_signed == b->is_signed;
    case TY_FLOAT:
        return a->bits == b->bits;

    case TY_PTR:
        return a->is_const == b->is_const && a->has_len == b->has_len &&
               (!a->has_len || a->len == b->len) && same(a->elem, b->elem);

    case TY_SLICE:
        return a->is_const == b->is_const && same(a->elem, b->elem);

    case TY_OPT:
    case TY_ERR:
        return same(a->elem, b->elem);

    case TY_TUPLE:
        if (a->nfields != b->nfields) return false;
        for (int i = 0; i < a->nfields; i++)
            if (!same(a->fields[i].ty, b->fields[i].ty)) return false;
        return true;

    case TY_FN:
        if (a->nparams != b->nparams) return false;
        for (int i = 0; i < a->nparams; i++)
            if (!same(a->params[i], b->params[i])) return false;
        return same(a->ret, b->ret);

    case TY_STRUCT:
    case TY_UNION:
    case TY_ENUM:
        /* nominal: identity is the declaration node */
        return a->decl == b->decl;

    default:
        return true;      /* void / bool / error / type / noreturn / comptime */
    }
}

static Type *intern(Type *t) {
    for (int i = 0; i < g_npool; i++)
        if (same(g_pool[i], t)) return g_pool[i];
    if (g_npool == g_cappool) {
        g_cappool = g_cappool ? g_cappool * 2 : 128;
        g_pool = realloc(g_pool, sizeof(Type *) * (size_t)g_cappool);
        if (!g_pool) { fprintf(stderr, "zxc: out of memory\n"); exit(1); }
    }
    g_pool[g_npool++] = t;
    return t;
}

Type *ty_int(int bits, bool is_signed) {
    Type *t = ty_new(TY_INT);
    t->bits = bits;
    t->is_signed = is_signed;
    return intern(t);
}

Type *ty_float(int bits) {
    Type *t = ty_new(TY_FLOAT);
    t->bits = bits;
    return intern(t);
}

Type *ty_ptr(Type *elem, bool is_const, bool has_len, uint64_t len) {
    Type *t = ty_new(TY_PTR);
    t->elem = elem;
    t->is_const = is_const;
    t->has_len = has_len;
    t->len = len;
    return intern(t);
}

Type *ty_slice(Type *elem, bool is_const) {
    Type *t = ty_new(TY_SLICE);
    t->elem = elem;
    t->is_const = is_const;
    return intern(t);
}

Type *ty_tuple(Type **items, int n) {
    Type *t = ty_new(TY_TUPLE);
    t->nfields = n;
    t->fields = n ? calloc((size_t)n, sizeof(Field)) : NULL;
    for (int i = 0; i < n; i++) t->fields[i].ty = items[i];
    return intern(t);
}

Type *ty_opt(Type *elem) {
    Type *t = ty_new(TY_OPT);
    t->elem = elem;
    return intern(t);
}

Type *ty_err(Type *elem) {
    Type *t = ty_new(TY_ERR);
    t->elem = elem;
    return intern(t);
}

Type *ty_fn(Type **params, int n, Type *ret) {
    Type *t = ty_new(TY_FN);
    t->nparams = n;
    t->params = n ? malloc(sizeof(Type *) * (size_t)n) : NULL;
    for (int i = 0; i < n; i++) t->params[i] = params[i];
    t->ret = ret;
    return intern(t);
}

/* 名义类型：同一个 decl 节点只产出一个 Type，字段随后由 sema 填。 */
/* 泛型函数的占位类型：ret == NULL 是它的唯一特征（真函数类型一定有 ret）。
 * 它的存在只是让"取一个泛型函数的名字"这件事有类型可挂，调用点永远先走
 * 单态化，不会真的拿它当 fn 类型用。 */
Type *ty_generic_fn(void) { return ty_fn(NULL, 0, NULL); }

Type *ty_agg(TyKind kind, Node *decl, const char *name) {
    for (int i = 0; i < g_npool; i++) {
        Type *t = g_pool[i];
        if (t->kind == kind && t->decl == decl && decl != NULL) return t;
    }
    Type *t = ty_new(kind);
    t->decl = decl;
    t->name = name;
    return intern(t);
}

/* ---------- singletons ---------- */

static Type *mk_single(TyKind k) {
    static Type *cache[TY_FN + 1];
    if (!cache[k]) cache[k] = intern(ty_new(k));
    return cache[k];
}

Type *ty_invalid(void)       { return mk_single(TY_INVALID); }
Type *ty_void(void)          { return mk_single(TY_VOID); }
Type *ty_bool(void)          { return mk_single(TY_BOOL); }
Type *ty_error(void)         { return mk_single(TY_ERROR); }
Type *ty_type(void)          { return mk_single(TY_TYPE); }
Type *ty_noreturn(void)      { return mk_single(TY_NORETURN); }
Type *ty_comptime_int(void)  { return mk_single(TY_COMPTIME_INT); }
Type *ty_comptime_float(void){ return mk_single(TY_COMPTIME_FLOAT); }
Type *ty_null(void)         { return mk_single(TY_NULL); }
Type *ty_undef(void)        { return mk_single(TY_UNDEF); }
Type *ty_usize(void)         { return ty_int(64, false); }
Type *ty_isize(void)         { return ty_int(64, true); }
Type *ty_u8(void)            { return ty_int(8, false); }
Type *ty_i32(void)           { return ty_int(32, true); }
Type *ty_f32(void)           { return ty_float(32); }
Type *ty_f64(void)           { return ty_float(64); }

/* ---------- queries ---------- */

bool ty_eq(Type *a, Type *b) {
    if (!a || !b) return false;
    if (a->kind == TY_INVALID || b->kind == TY_INVALID) return true;
    return a == b || same(a, b);
}

bool ty_is_int(Type *t) {
    return t && (t->kind == TY_INT || t->kind == TY_COMPTIME_INT ||
                 t->kind == TY_ERROR || t->kind == TY_ENUM);
}

bool ty_is_float(Type *t) {
    return t && (t->kind == TY_FLOAT || t->kind == TY_COMPTIME_FLOAT);
}

bool ty_is_num(Type *t) { return ty_is_int(t) || ty_is_float(t); }

bool ty_is_scalar(Type *t) {
    if (!t) return false;
    switch (t->kind) {
    case TY_BOOL: case TY_INT: case TY_FLOAT: case TY_ENUM:
    case TY_ERROR: case TY_PTR: case TY_FN: case TY_COMPTIME_INT:
    case TY_COMPTIME_FLOAT: case TY_OPT: case TY_ERR: case TY_NULL:
        return true;
    default:
        return false;
    }
}

bool ty_is_agg(Type *t) {
    return t && (t->kind == TY_STRUCT || t->kind == TY_UNION || t->kind == TY_TUPLE);
}

Field *ty_field(Type *t, const char *name) {
    if (!t || !name) return NULL;
    for (int i = 0; i < t->nfields; i++)
        if (t->fields[i].name && strcmp(t->fields[i].name, name) == 0)
            return &t->fields[i];
    return NULL;
}

int ty_field_index(Type *t, const char *name) {
    Field *f = ty_field(t, name);
    return f ? (int)(f - t->fields) : -1;
}

Method *ty_method(Type *t, const char *name) {
    if (!t || !name) return NULL;
    for (int i = 0; i < t->nmethods; i++)
        if (t->methods[i].name && strcmp(t->methods[i].name, name) == 0)
            return &t->methods[i];
    return NULL;
}

Field *ty_static_member(Type *t, const char *name) {
    if (!t || !name) return NULL;
    for (int i = 0; i < t->nstatics; i++)
        if (t->statics[i].name && strcmp(t->statics[i].name, name) == 0)
            return &t->statics[i];
    for (int i = 0; i < t->nfields; i++)
        if (t->fields[i].is_static && t->fields[i].name &&
            strcmp(t->fields[i].name, name) == 0)
            return &t->fields[i];
    return NULL;
}

static int align_up(int n, int a) {
    if (a <= 1) return n;
    return (n + a - 1) / a * a;
}

int ty_align(Type *t) {
    if (!t) return 1;
    if (t->align >= 0) return t->align;
    /* -2 = 正在算：说明经过一条不含指针的环（`struct { x: Node }`），
     * 值类型的大小不存在。返回 1 让它自然走到"大小为未知"那条路。 */
    if (t->align == -2) return 1;
    t->align = -2;
    int a = 1;
    switch (t->kind) {
    case TY_INVALID: case TY_VOID: case TY_NORETURN: case TY_TYPE:
    case TY_COMPTIME_INT: case TY_COMPTIME_FLOAT: case TY_NULL: case TY_UNDEF:
        a = 1; break;
    case TY_BOOL:  a = 1; break;
    case TY_ERROR: a = 2; break;
    case TY_INT: case TY_FLOAT: a = t->bits / 8; break;
    case TY_ENUM:  a = ty_align(t->elem ? t->elem : ty_i32()); break;
    case TY_PTR: case TY_FN: a = 8; break;
    case TY_SLICE: a = 8; break;
    case TY_OPT: case TY_ERR: {
        /* 规格 §5：载荷在前、标签在尾。
         * ?T 的标签是 bool（对齐 1），所以 align = align(T)；
         * !T 的标签是 error(u16)，对齐 2，所以 align(!T) = max(align(T), 2)
         * —— 这一点必须和后端生成的 C 结构体一致，否则 @sizeOf 会骗人。 */
        Type *e = t->elem;
        a = e ? ty_align(e) : 1;
        if (t->kind == TY_ERR && a < 2) a = 2;
        break;
    }
    case TY_TUPLE: {
        for (int i = 0; i < t->nfields; i++) {
            int fa = ty_align(t->fields[i].ty);
            if (fa > a) a = fa;
        }
        if (a < 1) a = 1;
        break;
    }
    case TY_STRUCT: {
        for (int i = 0; i < t->nfields; i++) {
            if (t->fields[i].is_static) continue;
            int fa = ty_align(t->fields[i].ty);
            if (fa > a) a = fa;
        }
        break;
    }
    case TY_UNION: {
        for (int i = 0; i < t->nfields; i++) {
            int fa = ty_align(t->fields[i].ty);
            if (fa > a) a = fa;
        }
        if (t->is_tagged && t->tag_ty) {
            int ta = ty_align(t->tag_ty);
            if (ta > a) a = ta;
        }
        break;
    }
    }
    if (a < 1) a = 1;
    t->align = a;
    return a;
}

int ty_size(Type *t) {
    if (!t) return 0;
    if (t->size >= 0) return t->size;
    if (t->size == -2) return -1;      /* 正在算：值类型里出现了自己，没有大小 */
    t->size = -2;
    int s = 0;
    switch (t->kind) {
    case TY_INVALID: case TY_VOID: case TY_NORETURN: case TY_TYPE:
        s = 0; break;
    case TY_COMPTIME_INT: case TY_COMPTIME_FLOAT:
        s = -1; break;                 /* 还没有运行时宽度 */
    case TY_NULL: case TY_UNDEF:
        s = 0; break;
    case TY_BOOL:  s = 1; break;
    case TY_ERROR: s = 2; break;
    case TY_INT: case TY_FLOAT: s = t->bits / 8; break;
    case TY_ENUM:  s = t->elem ? ty_size(t->elem) : 4; break;
    case TY_PTR: case TY_FN: s = 8; break;
    case TY_SLICE: s = 16; break;
    case TY_TUPLE: {
        int off = 0, maxa = 1;
        for (int i = 0; i < t->nfields; i++) {
            Type *ft = t->fields[i].ty;
            int fa = ty_align(ft), fs = ty_size(ft);
            if (fs < 0) { s = -1; goto done; }
            if (fa > maxa) maxa = fa;
            off = align_up(off, fa);
            t->fields[i].offset = (uint64_t)off;
            off += fs;
        }
        s = align_up(off, maxa);
        break;
    }
    case TY_STRUCT: {
        int off = 0, maxa = 1;
        for (int i = 0; i < t->nfields; i++) {
            if (t->fields[i].is_static) continue;
            Type *ft = t->fields[i].ty;
            int fa = ty_align(ft), fs = ty_size(ft);
            if (fs < 0) { s = -1; goto done; }
            if (fa > maxa) maxa = fa;
            off = align_up(off, fa);
            t->fields[i].offset = (uint64_t)off;
            off += fs;
        }
        s = align_up(off, maxa);
        break;
    }
    case TY_UNION: {
        int maxs = 0, maxa = 1;
        for (int i = 0; i < t->nfields; i++) {
            int fs = ty_size(t->fields[i].ty);
            if (fs < 0) { s = -1; goto done; }
            if (fs > maxs) maxs = fs;
            int fa = ty_align(t->fields[i].ty);
            if (fa > maxa) maxa = fa;
            t->fields[i].offset = 0;
        }
        if (t->is_tagged) maxs += 2;      /* 判别值跟在载荷后面 */
        s = align_up(maxs, maxa);
        break;
    }
    case TY_OPT: case TY_ERR: {
        Type *e = t->elem;
        int es = e ? ty_size(e) : 0;
        if (es < 0) { s = -1; goto done; }
        s = align_up(es + (t->kind == TY_ERR ? 2 : 1), ty_align(t));
        break;
    }
    }
done:
    t->size = s;
    return s;
}

/* ---------- printing ---------- */

static void ty_print_into(Buf *b, Type *t, int depth);

static void print_tuple(Buf *b, Type *t, int depth) {
    buf_putc(b, '{');
    for (int i = 0; i < t->nfields; i++) {
        if (i) buf_puts(b, ", ");
        ty_print_into(b, t->fields[i].ty, depth + 1);
    }
    buf_putc(b, '}');
}

static void ty_print_into(Buf *b, Type *t, int depth) {
    if (!t) { buf_puts(b, "<null>"); return; }
    switch (t->kind) {
    case TY_INVALID:   buf_puts(b, "<error>"); return;
    case TY_VOID:      buf_puts(b, "void"); return;
    case TY_NORETURN:  buf_puts(b, "noreturn"); return;
    case TY_BOOL:      buf_puts(b, "bool"); return;
    case TY_ERROR:     buf_puts(b, "error"); return;
    case TY_TYPE:      buf_puts(b, "type"); return;
    case TY_COMPTIME_INT:   buf_puts(b, "comptime_int"); return;
    case TY_COMPTIME_FLOAT: buf_puts(b, "comptime_float"); return;
    case TY_NULL:  buf_puts(b, "null"); return;
    case TY_UNDEF: buf_puts(b, "undefined"); return;
    case TY_INT:
        buf_printf(b, t->is_signed ? "i%d" : "u%d", t->bits);
        return;
    case TY_FLOAT: buf_printf(b, "f%d", t->bits); return;

    case TY_PTR:
        buf_putc(b, '*');
        if (t->is_const) buf_puts(b, "const ");
        if (t->has_len) buf_printf(b, "[%llu]", (unsigned long long)t->len);
        ty_print_into(b, t->elem, depth + 1);
        return;

    case TY_SLICE:
        buf_puts(b, "[]");
        if (t->is_const) buf_puts(b, "const ");
        ty_print_into(b, t->elem, depth + 1);
        return;

    case TY_TUPLE: print_tuple(b, t, depth); return;

    case TY_STRUCT:
    case TY_UNION:
    case TY_ENUM:
        if (t->name) { buf_puts(b, t->name); return; }
        buf_puts(b, t->kind == TY_STRUCT ? "struct{" : t->kind == TY_UNION ? "union{" : "enum{");
        if (depth < 3) {
            for (int i = 0; i < t->nfields; i++) {
                if (i) buf_puts(b, ", ");
                buf_puts(b, t->fields[i].name ? t->fields[i].name : "?");
                buf_puts(b, ": ");
                ty_print_into(b, t->fields[i].ty, depth + 1);
            }
        }
        buf_putc(b, '}');
        return;

    case TY_OPT: buf_putc(b, '?'); ty_print_into(b, t->elem, depth + 1); return;
    case TY_ERR: buf_putc(b, '!'); ty_print_into(b, t->elem, depth + 1); return;

    case TY_FN:
        buf_puts(b, "fn(");
        for (int i = 0; i < t->nparams; i++) {
            if (i) buf_puts(b, ", ");
            ty_print_into(b, t->params[i], depth + 1);
        }
        buf_putc(b, ')');
        if (t->ret && t->ret->kind != TY_VOID) {
            buf_putc(b, ' ');
            ty_print_into(b, t->ret, depth + 1);
        }
        return;
    }
}

const char *ty_str(Type *t) {
    Buf b; buf_init(&b);
    ty_print_into(&b, t, 0);
    buf_putc(&b, '\0');
    return b.data;      /* 泄漏到进程结束：编译器的类型名都活得和进程一样久 */
}

/* ---------------- 放宽的布局 ---------------- */

/* comptime_int / comptime_float 没有运行时宽度；但 `tup.size` 和 @sizeOf
 * 必须给个数，所以按 R7 的默认宽度（i32 / f64）算。 */
int ty_align_relaxed(Type *t) {
    if (!t) return 1;
    switch (t->kind) {
    case TY_COMPTIME_INT:   return 4;
    case TY_COMPTIME_FLOAT: return 8;
    case TY_TUPLE: {
        int a = 1;
        for (int i = 0; i < t->nfields; i++) {
            int fa = ty_align_relaxed(t->fields[i].ty);
            if (fa > a) a = fa;
        }
        return a;
    }
    case TY_STRUCT: {
        int a = 1;
        for (int i = 0; i < t->nfields; i++) {
            if (t->fields[i].is_static) continue;
            int fa = ty_align_relaxed(t->fields[i].ty);
            if (fa > a) a = fa;
        }
        return a;
    }
    case TY_UNION: {
        int a = 1;
        for (int i = 0; i < t->nfields; i++) {
            int fa = ty_align_relaxed(t->fields[i].ty);
            if (fa > a) a = fa;
        }
        if (t->is_tagged && t->tag_ty) {
            int ta = ty_align_relaxed(t->tag_ty);
            if (ta > a) a = ta;
        }
        return a;
    }
    case TY_OPT: case TY_ERR: {
        int a = ty_align_relaxed(t->elem);
        if (t->kind == TY_ERR && a < 2) a = 2;
        return a;
    }
    default:
        return ty_align(t);
    }
}

int ty_size_relaxed(Type *t) {
    if (!t) return 0;
    switch (t->kind) {
    case TY_COMPTIME_INT:   return 4;
    case TY_COMPTIME_FLOAT: return 8;
    default: break;
    }
    int s = ty_size(t);
    if (s >= 0) return s;

    /* 只有含"还没有宽度"的成员的聚合会走到这里，自己按放宽对齐重排一遍 */
    switch (t->kind) {
    case TY_TUPLE: case TY_STRUCT: {
        int off = 0, maxa = 1;
        for (int i = 0; i < t->nfields; i++) {
            if (t->kind == TY_STRUCT && t->fields[i].is_static) continue;
            int fa = ty_align_relaxed(t->fields[i].ty);
            int fs = ty_size_relaxed(t->fields[i].ty);
            if (fa > maxa) maxa = fa;
            off = align_up(off, fa);
            off += fs;
        }
        return align_up(off, maxa);
    }
    case TY_UNION: {
        int maxs = 0, maxa = 1;
        for (int i = 0; i < t->nfields; i++) {
            int fs = ty_size_relaxed(t->fields[i].ty);
            int fa = ty_align_relaxed(t->fields[i].ty);
            if (fs > maxs) maxs = fs;
            if (fa > maxa) maxa = fa;
        }
        if (t->is_tagged) maxs += 2;
        return align_up(maxs, maxa);
    }
    case TY_OPT: case TY_ERR: {
        int es = ty_size_relaxed(t->elem);
        return align_up(es + (t->kind == TY_ERR ? 2 : 1), ty_align_relaxed(t));
    }
    default:
        return s < 0 ? 0 : s;
    }
}

const char *ty_kind_name(TyKind k) {
    switch (k) {
    case TY_INVALID: return "error";
    case TY_VOID: return "void";
    case TY_NORETURN: return "noreturn";
    case TY_BOOL: return "bool";
    case TY_ERROR: return "error";
    case TY_TYPE: return "type";
    case TY_INT: return "int";
    case TY_FLOAT: return "float";
    case TY_COMPTIME_INT: return "comptime_int";
    case TY_COMPTIME_FLOAT: return "comptime_float";
    case TY_NULL: return "null";
    case TY_UNDEF: return "undefined";
    case TY_PTR: return "pointer";
    case TY_SLICE: return "slice";
    case TY_TUPLE: return "tuple";
    case TY_STRUCT: return "struct";
    case TY_UNION: return "union";
    case TY_ENUM: return "enum";
    case TY_OPT: return "optional";
    case TY_ERR: return "error union";
    case TY_FN: return "function";
    }
    return "?";
}
