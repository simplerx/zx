/* val.c — 编译期值模型的构造、比较与运算。 */
#include "zxc.h"
#include "ast.h"
#include "ty.h"
#include "val.h"

extern bool g_ct_soft_div;   /* sema.c：投机折叠期间除零软失败 */

static Value *valloc(ValKind k, Type *t) {
    Value *v = calloc(1, sizeof(Value));
    v->kind = k;
    v->ty = t;
    return v;
}

Value *val_new(ValKind k, Type *t) { return valloc(k, t); }
Value *val_void(void)               { return valloc(V_VOID, ty_void()); }
Value *val_undef(Type *t)           { return valloc(V_UNDEF, t); }
Value *val_null(Type *t)            { return valloc(V_NULL, t ? t : ty_null()); }

Value *val_int(Type *t, uint64_t v, bool neg) {
    Value *r = valloc(V_INT, t);
    r->ival = v;
    r->neg = neg && v != 0;
    return r;
}

Value *val_from_i64(Type *t, int64_t x) {
    Value *r = valloc(V_INT, t);
    if (x < 0) { r->neg = true; r->ival = (uint64_t)(-(x + 1)) + 1; }
    else       { r->neg = false; r->ival = (uint64_t)x; }
    return r;
}

/* 十进制文本（可含 '_'）→ 128 位绝对值。假定文本合法（lexer 已保证）。 */
void val_big_split(const char *text, uint64_t *hi, uint64_t *lo) {
    uint64_t h = 0, l = 0;
    for (const char *s = text; *s; s++) {
        if (*s == '_') continue;
        unsigned d = (unsigned)(*s - '0');
        /* {h,l} = {h,l} * 10 + d，128 位乘加 */
        unsigned __int128 m = (((unsigned __int128)h) << 64 | l) * 10 + d;
        h = (uint64_t)(m >> 64);
        l = (uint64_t)m;
    }
    *hi = h;
    *lo = l;
}

Value *val_big128(Type *t, const char *text, bool neg) {
    Value *r = valloc(V_INT, t);
    val_big_split(text, &r->hi, &r->ival);
    r->big = true;   /* 调用方只在超 u64 时走这条路 */
    r->neg = neg;
    return r;
}

Value *val_float(Type *t, double f) {
    Value *r = valloc(V_FLOAT, t);
    r->fval = f;
    return r;
}

Value *val_bool(bool b) {
    Value *r = valloc(V_BOOL, ty_bool());
    r->bval = b;
    return r;
}

Value *val_str(const char *s, int n) {
    Value *r = valloc(V_STR, ty_slice(ty_u8(), true));
    r->sval = s;
    r->slen = n;
    return r;
}

Value *val_type(Type *t) {
    Value *r = valloc(V_TYPE, ty_type());
    r->tval = t;
    return r;
}

Value *val_agg(Type *t, int n) {
    Value *r = valloc(V_AGG, t);
    r->nitems = n;
    if (n) r->items = calloc((size_t)n, sizeof(Value *));
    return r;
}

Value *val_fn(Type *t, Node *fn) {
    Value *r = valloc(V_FN, t);
    r->fn = fn;
    return r;
}

Value *val_ptr(Type *t, Value *base, uint64_t off) {
    Value *r = valloc(V_PTR, t);
    r->base = base;
    r->off = off;
    return r;
}

/* ---------------- 查询 ---------------- */

bool    val_is_int(Value *v) { return v && v->kind == V_INT; }

int64_t val_i64(Value *v) {
    if (!v) return 0;
    if (v->kind == V_BOOL) return v->bval ? 1 : 0;
    if (v->kind != V_INT) return 0;
    return v->neg ? -(int64_t)v->ival : (int64_t)v->ival;
}

bool val_truthy(Value *v) {
    if (!v) return false;
    switch (v->kind) {
    case V_BOOL:  return v->bval;
    case V_INT:   return v->ival != 0;
    case V_FLOAT: return v->fval != 0.0;
    case V_NULL:  return false;
    default:      return true;
    }
}

bool val_eq(Value *a, Value *b) {
    if (!a || !b) return false;
    if (a->kind != b->kind) {
        /* 整数与布尔在编译期可以互相比较 */
        if (val_is_int(a) && b->kind == V_BOOL) return (val_i64(a) != 0) == b->bval;
        if (val_is_int(b) && a->kind == V_BOOL) return (val_i64(b) != 0) == a->bval;
        return false;
    }
    switch (a->kind) {
    case V_VOID: case V_UNDEF: case V_NULL: return true;
    case V_BOOL:  return a->bval == b->bval;
    case V_INT:   return a->neg == b->neg && a->hi == b->hi && a->ival == b->ival;
    case V_FLOAT: return a->fval == b->fval;
    case V_STR:   return a->slen == b->slen && (a->slen == 0 || !memcmp(a->sval, b->sval, (size_t)a->slen));
    case V_TYPE:  return ty_eq(a->tval, b->tval);
    case V_FN:    return a->fn == b->fn;
    case V_PTR:   return a->base == b->base && a->off == b->off;
    case V_AGG: {
        if (a->nitems != b->nitems) return false;
        for (int i = 0; i < a->nitems; i++)
            if (!val_eq(a->items[i], b->items[i])) return false;
        return true;
    }
    }
    return false;
}

/* ---------------- 运算 ---------------- */

static bool is_floatish(Value *v) { return v && v->kind == V_FLOAT; }

/* 编译期整数运算的结果按"运算数类型"回绕（M19：编译期与运行时一致，
 * 都是二补码回绕）。comptime_int（宽度未定）不截——按 64 位算。 */
static int64_t wrap_to(Type *t, uint64_t u) {
    if (!t || t->kind != TY_INT || t->bits >= 64) return (int64_t)u;
    int b = t->bits;
    uint64_t m = (b >= 64) ? ~0ull : (1ull << b) - 1;
    u &= m;
    if (t->is_signed && b < 64 && ((u >> (b - 1)) & 1)) u |= ~m;
    return (int64_t)u;
}

static Value *int_bin(int op, Value *a, Value *b, int line, int col) {
    Type *t = a->ty ? a->ty : ty_comptime_int();
    /* 128 位值只在"纯字面量"形态下有精确表示；一旦要运算，
     * 编译期就算不了（R1 的 const 会报错，运行时路径照常算）。 */
    if (a->big || b->big) return NULL;
    int64_t x = val_i64(a), y = val_i64(b);
    switch (op) {
    case TOK_PLUS:     return val_from_i64(t, wrap_to(t, (uint64_t)x + (uint64_t)y));
    case TOK_MINUS:    return val_from_i64(t, wrap_to(t, (uint64_t)x - (uint64_t)y));
    case TOK_STAR:     return val_from_i64(t, wrap_to(t, (uint64_t)x * (uint64_t)y));
    case TOK_SLASH:
        if (y == 0) { if (!g_ct_soft_div) { err_at(line, col, "division by zero in a compile-time expression"); } return NULL; }
        if (x == INT64_MIN && y == -1) return val_from_i64(t, INT64_MIN);
        return val_from_i64(t, wrap_to(t, (uint64_t)(x / y)));
    case TOK_PERCENT:
        if (y == 0) { if (!g_ct_soft_div) { err_at(line, col, "division by zero in a compile-time expression"); } return NULL; }
        if (y == -1) return val_from_i64(t, 0);
        return val_from_i64(t, wrap_to(t, (uint64_t)(x % y)));
    case TOK_AMP:      return val_from_i64(t, wrap_to(t, (uint64_t)x & (uint64_t)y));
    case TOK_PIPE:     return val_from_i64(t, wrap_to(t, (uint64_t)x | (uint64_t)y));
    case TOK_CARET:    return val_from_i64(t, wrap_to(t, (uint64_t)x ^ (uint64_t)y));
    case TOK_SHL:      return val_from_i64(t, wrap_to(t, (uint64_t)x << (y & 63)));
    case TOK_SHR:      return val_from_i64(t, wrap_to(t, (int64_t)((uint64_t)x >> (y & 63))));
    default: return NULL;
    }
}

Value *val_bin(int op, Value *a, Value *b, int line, int col) {
    if (!a || !b) return NULL;

    /* 比较运算：任何数值对都能比，结果是 bool */
    if (is_floatish(a) || is_floatish(b)) {
        double x = a->kind == V_FLOAT ? a->fval : (double)val_i64(a);
        double y = b->kind == V_FLOAT ? b->fval : (double)val_i64(b);
        switch (op) {
        case TOK_PLUS:  return val_float(a->ty && a->kind == V_FLOAT ? a->ty : b->ty, x + y);
        case TOK_MINUS: return val_float(a->ty && a->kind == V_FLOAT ? a->ty : b->ty, x - y);
        case TOK_STAR:  return val_float(a->ty && a->kind == V_FLOAT ? a->ty : b->ty, x * y);
        case TOK_SLASH:
            /* 浮点除零遵循 IEEE-754（±inf / NaN），编译期与运行时一致（M19） */
            return val_float(a->ty && a->kind == V_FLOAT ? a->ty : b->ty, x / y);
        case TOK_LT: return val_bool(x <  y);
        case TOK_LE: return val_bool(x <= y);
        case TOK_GT: return val_bool(x >  y);
        case TOK_GE: return val_bool(x >= y);
        case TOK_EQ: return val_bool(x == y);
        case TOK_NE: return val_bool(x != y);
        default: return NULL;
        }
    }

    if (a->kind == V_BOOL && b->kind == V_BOOL) {
        switch (op) {
        case TOK_ANDAND: return val_bool(a->bval && b->bval);
        case TOK_OROR:   return val_bool(a->bval || b->bval);
        case TOK_AMP:    return val_bool(a->bval & b->bval);
        case TOK_PIPE:   return val_bool(a->bval | b->bval);
        case TOK_CARET:  return val_bool(a->bval ^ b->bval);
        case TOK_EQ:     return val_bool(a->bval == b->bval);
        case TOK_NE:     return val_bool(a->bval != b->bval);
        default: return NULL;
        }
    }

    if (a->kind == V_INT && b->kind == V_INT) {
        if (a->big || b->big) return NULL;   /* 128 位值编译期不比（见 int_bin） */
        switch (op) {
        case TOK_LT: return val_bool(val_i64(a) <  val_i64(b));
        case TOK_LE: return val_bool(val_i64(a) <= val_i64(b));
        case TOK_GT: return val_bool(val_i64(a) >  val_i64(b));
        case TOK_GE: return val_bool(val_i64(a) >= val_i64(b));
        case TOK_EQ: return val_bool(val_i64(a) == val_i64(b));
        case TOK_NE: return val_bool(val_i64(a) != val_i64(b));
        default: return int_bin(op, a, b, line, col);
        }
    }

    if (a->kind == V_STR && b->kind == V_STR) {
        bool same = a->slen == b->slen &&
                    (a->slen == 0 || !memcmp(a->sval, b->sval, (size_t)a->slen));
        if (op == TOK_EQ) return val_bool(same);
        if (op == TOK_NE) return val_bool(!same);
    }

    return NULL;
}

Value *val_un(int op, Value *a, int line, int col) {
    (void)line; (void)col;
    if (!a) return NULL;
    switch (op) {
    case TOK_MINUS:
        if (a->kind == V_INT) {
            if (a->big) {
                /* 128 位取负：绝对值翻符号。零不产生负号。 */
                Value *r = val_new(V_INT, a->ty);
                r->hi = a->hi; r->ival = a->ival;
                r->big = true;
                r->neg = !a->neg && (a->hi || a->ival);
                return r;
            }
            return val_from_i64(a->ty, -val_i64(a));
        }
        if (a->kind == V_FLOAT) return val_float(a->ty, -a->fval);
        return NULL;
    case TOK_PLUS:
        return a;
    case TOK_BANG:
        if (a->kind == V_BOOL) return val_bool(!a->bval);
        if (a->kind == V_INT)  return val_from_i64(a->ty, !val_i64(a));
        return NULL;
    case TOK_TILDE:
        if (a->kind == V_INT) return val_from_i64(a->ty, ~val_i64(a));
        return NULL;
    default:
        return NULL;
    }
}

Value *val_cast(Type *to, Value *v) {
    if (!v) return NULL;
    if (!to || to->kind == TY_INVALID) return v;

    /* 指针/函数指针之间的显式转换：地址不变，编译期照原样带着走 */
    if (to->kind == TY_PTR || to->kind == TY_FN) {
        if (v->kind == V_PTR || v->kind == V_FN) return v;
        return NULL;
    }

    if (to->kind == TY_BOOL) {
        if (v->kind == V_INT || v->kind == V_BOOL)
            return val_bool(val_truthy(v));
        return NULL;
    }
    if (ty_is_int(to)) {
        if (v->kind == V_INT) {
            uint64_t bits = (uint64_t)to->bits;
            if (bits > 0 && bits < 64) {
                uint64_t mask = (1ULL << bits) - 1;
                int64_t x = val_i64(v);
                uint64_t raw = (uint64_t)x & mask;
                if (!to->is_signed) return val_int(to, raw, false);
                /* 有符号：按位宽做符号扩展 */
                uint64_t sign = 1ULL << (bits - 1);
                if (raw & sign) return val_int(to, (uint64_t)(-(int64_t)((~raw & mask) + 1)), true);
                return val_int(to, raw, false);
            }
            return v;
        }
        if (v->kind == V_BOOL) return val_int(to, v->bval ? 1 : 0, false);
        if (v->kind == V_FLOAT) return val_from_i64(to, (int64_t)v->fval);
        return NULL;
    }
    if (ty_is_float(to)) {
        if (v->kind == V_INT)   return val_float(to, (double)val_i64(v));
        if (v->kind == V_FLOAT) return val_float(to, v->fval);
        return NULL;
    }
    if (to->kind == TY_ENUM && v->kind == V_INT) {
        Value *r = val_int(to, v->ival, v->neg);
        return r;
    }
    return NULL;
}

/* ---------------- 打印 ---------------- */

void val_dump(Value *v, Buf *b) {
    if (!v) { buf_puts(b, "<none>"); return; }
    switch (v->kind) {
    case V_VOID:  buf_puts(b, "{}"); break;
    case V_UNDEF: buf_puts(b, "undefined"); break;
    case V_NULL:  buf_puts(b, "null"); break;
    case V_BOOL:  buf_puts(b, v->bval ? "true" : "false"); break;
    case V_INT:
        if (v->big) buf_puts(b, "<big>");
        else if (v->neg) buf_printf(b, "-%llu", (unsigned long long)v->ival);
        else buf_printf(b, "%llu", (unsigned long long)v->ival);
        break;
    case V_FLOAT: buf_printf(b, "%g", v->fval); break;
    case V_STR:   buf_putc(b, '"'); buf_putn(b, v->sval, (size_t)v->slen); buf_putc(b, '"'); break;
    case V_TYPE:  buf_puts(b, ty_str(v->tval)); break;
    case V_FN:    buf_puts(b, "<function>"); break;
    case V_PTR:   buf_puts(b, "<ptr>"); break;
    case V_AGG:
        buf_puts(b, "{ ");
        for (int i = 0; i < v->nitems; i++) {
            if (i) buf_puts(b, ", ");
            val_dump(v->items[i], b);
        }
        buf_puts(b, " }");
        break;
    }
}
