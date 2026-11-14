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

    case TY_VEC:
        return a->len == b->len && same(a->elem, b->elem);

    case TY_OPT:
        return same(a->elem, b->elem);
    case TY_ERR:
        /* 集合参与 intern 身份（`!i32` 与 `error{open}!i32` 是两个类型对象，
         * 这样 ty_str 才写得出声明的集合），但 ty_eq 会忽略它——运行时
         * 表示完全一样，赋值/传参不该因为集合宽窄而失败。 */
        return same(a->elem, b->elem) && a->eset == b->eset;
    case TY_ERRORSET:
        return a->eset == b->eset;

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

Type *ty_vec(Type *elem, uint64_t n) {
    Type *t = ty_new(TY_VEC);
    t->elem = elem;
    t->len = n;
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

/* 带错误集的 !T：`error{open}!i32`。布局与 !T 完全一致，只是多带了
 * "允许哪些码"这份编译期信息。 */
Type *ty_err_set(Type *elem, void *set) {
    if (!set) return ty_err(elem);
    Type *t = ty_new(TY_ERR);
    t->elem = elem;
    t->eset = set;
    return intern(t);
}

/* ---------------- 错误集 ---------------- */

/* 名字 → 码的登记表：码就是 u16 值，首次出现分配，进程内稳定。
 * 65535 保留给 error.null（§5），命名码从 1 起到 65534。 */
#define ZX_ERRCODE_MAX 1024
static const char *g_errnames[ZX_ERRCODE_MAX + 1];   /* 下标 = 码 */
static int g_nerrcodes;

uint16_t ty_err_code(const char *name) {
    if (!name || !*name) return 0;
    /* `null` 是**保留码 65535**（§5.2）：`?T` 被 `try` 的时候抛的就是它。
     * 不进码表、不参与顺序分配——它有固定值，写 `error.null` 就是这个码
     *（用户 2026-11-09：null 本身是一种错误，`error.null` 要能写）。 */
    if (!strcmp(name, "null")) return 65535;
    for (int i = 0; i < g_nerrcodes; i++)
        if (g_errnames[i] && !strcmp(g_errnames[i], name)) return (uint16_t)(i + 1);
    /* 找第一个空槽：显式占位（ty_err_code_at，枚举错误用）可能把中间的
     * 槽占了，顺序分配得绕开它们，不然两个名字挤一个码。 */
    for (int i = 0; i < ZX_ERRCODE_MAX; i++) {
        if (g_errnames[i]) continue;
        g_errnames[i] = strdup(name);
        if (i + 1 > g_nerrcodes) g_nerrcodes = i + 1;
        return (uint16_t)(i + 1);
    }
    return 65534;                  /* 表满：共用一个码，别崩 */
}

/* 显式占位登记：枚举错误 `(error)Err.x` 的码就是枚举值本身，不走顺序
 * 分配——把它在编译期登记进同一张码→名字表，errName / printTrace 的
 * 回放就能统一解析（M57）。槽位已有别的名字就不覆盖（先到先得；
 * 两个名字挤一个码时回放按先登记者，文档里写明这个边界）。 */
void ty_err_code_at(const char *name, uint16_t code) {
    if (!name || !*name || code == 0 || code == 65535 || code >= ZX_ERRCODE_MAX) return;
    if (g_errnames[code - 1]) return;
    g_errnames[code - 1] = strdup(name);
    if ((int)code > g_nerrcodes) g_nerrcodes = code;
}

const char *ty_err_name(uint16_t code) {
    if (code == 65535) return "null";   /* 保留码：errName(error.null) → "null" */
    if (code == 0 || code > g_nerrcodes) return NULL;
    return g_errnames[code - 1];
}

/* 登记过的错误码总数：后端给 std.debug.errName 铺静态表用 */
int ty_err_count(void) {
    return g_nerrcodes;
}

/* 集合本体：错误码升序数组 + 同名数组。集合都很小，线性查找足够，
 * 关键是可以按内容 intern —— 于是"两个集合相等"就是指针相等。 */
typedef struct ErrSet {
    int       n;
    uint16_t  codes[64];
    const char *names[64];
} ErrSet;

static ErrSet *g_esets;
static int     g_nesets, g_cesets;

static ErrSet *eset_intern(ErrSet *s) {
    for (int i = 0; i < g_nesets; i++) {
        ErrSet *o = &g_esets[i];
        if (o->n != s->n) continue;
        bool eq = true;
        for (int k = 0; k < s->n; k++)
            if (o->codes[k] != s->codes[k]) { eq = false; break; }
        if (eq) return o;
    }
    if (g_nesets == g_cesets) {
        g_cesets = g_cesets ? g_cesets * 2 : 32;
        g_esets = realloc(g_esets, sizeof(ErrSet) * (size_t)g_cesets);
        if (!g_esets) { fprintf(stderr, "zxc: out of memory\n"); exit(1); }
    }
    g_esets[g_nesets] = *s;
    return &g_esets[g_nesets++];
}

void *ty_errset_of(const char **names, int n) {
    ErrSet s;
    memset(&s, 0, sizeof s);
    for (int i = 0; i < n && s.n < 64; i++) {
        if (!names[i]) continue;
        uint16_t c = ty_err_code(names[i]);
        if (c == 0) continue;
        /* 插入排序去重：集合小，写起来最省事 */
        int k = s.n;
        while (k > 0 && s.codes[k - 1] > c) {
            s.codes[k] = s.codes[k - 1];
            s.names[k] = s.names[k - 1];
            k--;
        }
        if (k > 0 && s.codes[k - 1] == c) continue;      /* 重复成员 */
        s.codes[k] = c;
        s.names[k] = ty_err_name(c);
        s.n++;
    }
    return eset_intern(&s);
}

void *ty_errset_one(const char *name) { return ty_errset_of(&name, 1); }

int ty_errset_size(const void *set) {
    return set ? ((const ErrSet *)set)->n : 0;
}

const char *ty_errset_name_at(const void *set, int i) {
    if (!set || i < 0 || i >= ((const ErrSet *)set)->n) return NULL;
    return ((const ErrSet *)set)->names[i];
}

uint16_t ty_errset_code_at(const void *set, int i) {
    if (!set || i < 0 || i >= ((const ErrSet *)set)->n) return 0;
    return ((const ErrSet *)set)->codes[i];
}

bool ty_errset_has(const void *set, uint16_t code) {
    if (!set) return true;                  /* 未声明集合 = 什么都收 */
    const ErrSet *s = set;
    for (int i = 0; i < s->n; i++) if (s->codes[i] == code) return true;
    return false;
}

bool ty_errset_subset(const void *a, const void *b) {
    if (!a) return b == NULL;               /* a 是 anyerror：只有 b 也是 anyerror 才成立 */
    if (!b) return true;
    const ErrSet *x = a, *y = b;
    for (int i = 0; i < x->n; i++)
        if (!ty_errset_has(y, x->codes[i])) return false;
    return true;
}

void *ty_errset_union(const void *a, const void *b) {
    ErrSet r;
    memset(&r, 0, sizeof r);
    for (int side = 0; side < 2; side++) {
        const ErrSet *x = side == 0 ? (const ErrSet *)a : (const ErrSet *)b;
        if (!x) continue;
        for (int i = 0; i < x->n && r.n < 64; i++) {
            if (ty_errset_has(&r, x->codes[i])) continue;   /* 已收 */
            r.codes[r.n] = x->codes[i];
            r.names[r.n] = x->names[i];
            r.n++;
        }
    }
    /* 排序，保证 intern 判等按内容生效 */
    for (int i = 1; i < r.n; i++) {
        uint16_t c = r.codes[i];
        const char *nm = r.names[i];
        int k = i - 1;
        while (k >= 0 && r.codes[k] > c) { r.codes[k + 1] = r.codes[k]; r.names[k + 1] = r.names[k]; k--; }
        r.codes[k + 1] = c;
        r.names[k + 1] = nm;
    }
    return eset_intern(&r);
}

const char *ty_errset_str(const void *set) {
    static char bufs[8][256];
    static int rot;
    char *b = bufs[rot++ & 7];
    if (!set) { snprintf(b, 256, "error{*}"); return b; }
    const ErrSet *s = set;
    size_t k = 0;
    k += (size_t)snprintf(b + k, 256 - k, "error{");
    for (int i = 0; i < s->n && k < 240; i++)
        k += (size_t)snprintf(b + k, 256 - k, "%s%s", i ? ", " : "", s->names[i]);
    snprintf(b + k, 256 - k, "}");
    return b;
}

Type *ty_errset_type(void *set) {
    Type *t = ty_new(TY_ERRORSET);
    t->eset = set;
    return intern(t);
}

void *ty_type_errset(Type *t) {
    return (t && t->kind == TY_ERRORSET) ? t->eset : NULL;
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
    /* 错误集合是检查用的元数据，不是表示的一部分：`!i32` 与
     * `error{open}!i32` 在内存里一模一样，赋值/传参按后者相等处理。 */
    if (a->kind == TY_ERR && b->kind == TY_ERR) return ty_eq(a->elem, b->elem);
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

/* extern 聚合体（FFI）里的定长数组字段在 C 里是内联数组：大小 = N × 元素，
 * 对齐 = 元素对齐。普通 Zx 结构体的 *[N]T 是指针（8 字节），不能用这条路。
 * 返回 false = 不是 extern 数组字段，按普通规则算。 */
static bool ext_array_field(Type *agg, Type *ft, int *fs, int *fa) {
    if (!agg || !agg->decl || !agg->decl->is_extern) return false;
    if (!ft || ft->kind != TY_PTR || !ft->has_len) return false;
    int es = ty_size(ft->elem);
    if (es < 0) return false;
    *fs = es * (int)ft->len;
    *fa = ty_align(ft->elem);
    return true;
}

bool ty_opt_flat(Type *t) {
    return t && t->kind == TY_OPT && t->elem && t->elem->kind == TY_PTR;
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
    case TY_ERRORSET:
    case TY_COMPTIME_INT: case TY_COMPTIME_FLOAT: case TY_NULL: case TY_UNDEF:
        a = 1; break;
    case TY_BOOL:  a = 1; break;
    case TY_ERROR: a = 2; break;
    case TY_INT: case TY_FLOAT: a = t->bits / 8; break;
    case TY_ENUM:  a = ty_align(t->elem ? t->elem : ty_i32()); break;
    case TY_PTR: case TY_FN: a = 8; break;
    case TY_SLICE: a = 8; break;
    case TY_VEC: {
        /* 与 clang 的 vector_size 类型一致：对齐 = 大小（4 float = 16B，
         * 8 float = 32B）。结构体布局两边必须同一个数，否则字段会错位。 */
        int es = ty_align(t->elem);
        a = es * (int)t->len;
        break;
    }
    case TY_OPT: case TY_ERR: {
        /* 规格 §5：载荷在前、标签在尾。
         * ?T 的标签是 bool（对齐 1），所以 align = align(T)；
         * !T 的标签是 error(u16)，对齐 2，所以 align(!T) = max(align(T), 2)
         * —— 这一点必须和后端生成的 C 结构体一致，否则 @sizeOf 会骗人。
         * 例外：?*T 无标签（M26），布局就是指针本身。 */
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
            Type *ft = t->fields[i].ty;
            int fa;
            if (ft && ft->kind == TY_PTR && ft->has_len &&
                t->decl && t->decl->is_extern)
                fa = ty_align(ft->elem);        /* extern 内联数组按元素对齐 */
            else
                fa = ty_align(ft);
            if (fa > a) a = fa;
        }
        break;
    }
    case TY_UNION: {
        for (int i = 0; i < t->nfields; i++) {
            Type *ft = t->fields[i].ty;
            int fa;
            if (ft && ft->kind == TY_PTR && ft->has_len &&
                t->decl && t->decl->is_extern)
                fa = ty_align(ft->elem);
            else
                fa = ty_align(ft);
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
    case TY_ERRORSET:                  /* 编译期类型：没有运行时大小 */
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
    case TY_VEC: {
        int es = ty_size(t->elem);
        s = es < 0 ? -1 : es * (int)t->len;
        break;
    }
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
            int fa, fs;
            if (ext_array_field(t, ft, &fs, &fa)) {          /* extern 内联数组 */
                t->fields[i].offset = (uint64_t)align_up(off, fa);
                off = (int)t->fields[i].offset + fs;
                if (fa > maxa) maxa = fa;
                continue;
            }
            fa = ty_align(ft); fs = ty_size(ft);
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
            Type *ft = t->fields[i].ty;
            int fs, fa;
            if (!ext_array_field(t, ft, &fs, &fa)) {
                fs = ty_size(ft);
                fa = ty_align(ft);
            }
            if (fs < 0) { s = -1; goto done; }
            if (fs > maxs) maxs = fs;
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
        if (t->kind == TY_OPT && ty_opt_flat(t)) { s = align_up(es, ty_align(t)); break; }
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

    /* 匿名枚举只打变体名，**不打变体类型**：枚举成员的 ty 就是枚举自己，
     * 跟着打会一层层展开成 `enum{x: enum{x: enum{…`, 直到深度上限才停。
     * 变体的类型恒为枚举本身，写出来也是噪音。 */
    case TY_ENUM:
        if (t->name) { buf_puts(b, t->name); return; }
        buf_puts(b, "enum{");
        for (int i = 0; i < t->nfields; i++) {
            if (i) buf_puts(b, ", ");
            buf_puts(b, t->fields[i].name ? t->fields[i].name : "?");
        }
        buf_putc(b, '}');
        return;

    case TY_STRUCT:
    case TY_UNION:
        if (t->name) { buf_puts(b, t->name); return; }
        buf_puts(b, t->kind == TY_STRUCT ? "struct{" : "union{");
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

    case TY_VEC:
        buf_puts(b, "@Vec(");
        ty_print_into(b, t->elem, depth + 1);
        buf_printf(b, ", %llu)", (unsigned long long)t->len);
        return;
    case TY_OPT: buf_putc(b, '?'); ty_print_into(b, t->elem, depth + 1); return;
    case TY_ERRORSET:
        buf_puts(b, ty_errset_str(t->eset));
        return;
    case TY_ERR:
        if (t->eset) { buf_puts(b, ty_errset_str(t->eset)); buf_putc(b, '!'); }
        else buf_putc(b, '!');
        ty_print_into(b, t->elem, depth + 1);
        return;

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
        if (t->kind == TY_OPT && ty_opt_flat(t)) return align_up(es, ty_align_relaxed(t));
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
    case TY_ERRORSET: return "error set";
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
    case TY_VEC: return "vector";
    }
    return "?";
}
