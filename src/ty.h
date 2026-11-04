/* ty.h — Zx type representation.
 *
 * 类型是语义分析阶段唯一的"真理"：AST 上每个表达式节点都挂着它的类型，
 * 后端只认这里构造出来的 Type。
 *
 * 两类类型：
 *   结构类型（指针/切片/元组/?T/!T/fn）—— 结构化比较，全局 intern，
 *     所以两个同构类型是同一个指针，判等就是 ==。
 *   名义类型（struct/union/enum）—— 身份来自那条类型声明的 AST 节点，
 *     同一个节点解析两次得到同一个 Type。
 */
#ifndef ZXC_TY_H
#define ZXC_TY_H

#include "zxc.h"

typedef struct Node Node;
typedef struct Type Type;

typedef enum {
    TY_INVALID = 0,        /* 错误哨兵：吸收所有级联错误 */
    TY_VOID,
    TY_NORETURN,
    TY_BOOL,
    TY_ERROR,              /* error ≡ u16 */
    TY_TYPE,               /* 类型值本身（只存在于编译期） */
    TY_INT,                /* bits = 1/8/16/32/64/128 */
    TY_FLOAT,              /* bits = 16/32/64 */
    TY_COMPTIME_INT,       /* 整数字面量，宽度未定 */
    TY_COMPTIME_FLOAT,
    TY_NULL,               /* null 字面量：期望类型为 ?T 时定型，否则是 bool */
    TY_UNDEF,              /* undefined：只表示"要空间不要值"，能匹配任何类型 */
    TY_PTR,                /* *T / *[N]T / *const T */
    TY_SLICE,              /* []T / []const T  → {ptr, len} 16 字节 */
    TY_TUPLE,              /* {T1, T2, …} 值类型，内联 */
    TY_STRUCT,
    TY_UNION,
    TY_ENUM,               /* 就是整数，elem 是底层类型 */
    TY_OPT,                /* ?T  = {payload T @0, tag bool @sizeof(T)} */
    TY_ERR,                /* !T  = {payload T @0, tag error @sizeof(T)} */
    TY_ERRORSET,           /* error{a, b}：编译期类型，运行时就是 error(u16) */
    TY_FN,                 /* 8 字节代码指针 */
} TyKind;

/* 聚合体的一个成员：结构体字段、联合体成员、枚举变体都用它。 */
typedef struct Field {
    const char *name;
    Type       *ty;
    Node       *dflt;      /* 字段默认值表达式，可为 NULL */
    bool        is_static; /* 结构体体里的 `var` / `const`（静态命名空间成员） */
    bool        is_var;    /* 静态成员是否可写 */
    uint64_t    offset;    /* 布局时填 */
    bool        has_tag;   /* 枚举变体值 / 带标签联合体的判别值 */
    uint64_t    tag;
} Field;

/* 聚合体上的一个方法：名字 + 那个函数本身。函数的第一参数就是接收者。 */
typedef struct Method {
    const char *name;
    Node       *fn;        /* NK_FN */
    Type       *fnty;      /* 它的函数类型 */
    bool        has_self;  /* 第一参数是接收者（`self: Self`）；否则是挂在类型上的普通函数 */
} Method;

struct Type {
    TyKind      kind;
    int         bits;
    bool        is_signed;  /* TY_INT：有符号 */

    bool        is_const;   /* TY_PTR / TY_SLICE：只读视图 */
    bool        has_len;    /* TY_PTR：数量写在类型里（*[N]T） */
    uint64_t    len;

    bool        is_tagged;  /* TY_UNION：成员是 {标签, 载荷} 形式 */
    Type       *tag_ty;     /* 带标签联合体的标签类型（枚举） */

    Type       *elem;       /* PTR/SLICE/OPT/ERR 的元素；ENUM 的底层类型 */
    Field      *fields;
    int         nfields;

    Method     *methods;    /* 结构体 / 联合体 / 枚举体里写的 fn */
    int         nmethods;

    /* 枚举体的 `const` / `var` 成员。不能塞进 fields——fields 是变体表，
     * switch 穷尽性、`{any}` 的变体名、C 那边的枚举常量都按它遍历。 */
    Field      *statics;
    int         nstatics;

    Type      **params;     /* FN 参数类型 */
    int         nparams;
    Type       *ret;        /* FN 返回类型 */

    Node       *decl;       /* struct/union/enum 的名义身份 */
    const char *name;       /* 显示名，可空 */
    void       *eset;       /* TY_ERRORSET 的错误集（ErrSet*）；TY_ERR 上表示
                             * "这个 !T 只会带这些错误码"（NULL = anyerror） */

    int         size;       /* 记忆化，-1 = 未算 */
    int         align;
};

/* ---------- 单例 ---------- */
Type *ty_invalid(void);
Type *ty_void(void);
Type *ty_bool(void);
Type *ty_error(void);
Type *ty_type(void);
Type *ty_noreturn(void);
Type *ty_usize(void);
Type *ty_isize(void);
Type *ty_u8(void);
Type *ty_i32(void);
Type *ty_f32(void);
Type *ty_f64(void);
Type *ty_comptime_int(void);
Type *ty_comptime_float(void);
Type *ty_null(void);
Type *ty_undef(void);

/* ---------- 构造（结构类型自动 intern） ---------- */
Type *ty_int(int bits, bool is_signed);
Type *ty_float(int bits);
Type *ty_ptr(Type *elem, bool is_const, bool has_len, uint64_t len);
Type *ty_slice(Type *elem, bool is_const);
Type *ty_tuple(Type **items, int n);
Type *ty_opt(Type *elem);
Type *ty_err(Type *elem);
Type *ty_fn(Type **params, int n, Type *ret);
Type *ty_generic_fn(void);       /* 泛型函数还没单态化时的占位类型 */
Type *ty_agg(TyKind kind, Node *decl, const char *name);

/* ---------- 错误集（error{a, b}） ---------- */
/* 错误码是 u16；名字首次出现时分配码（1..65534，65535 留给 error.null）。
 * 集合在编译期才有意义，运行时就是普通 error 值——TY_ERR 上挂不挂集合
 * 都不改变布局，只改变检查强度。 */
uint16_t    ty_err_code(const char *name);          /* 名字 → 码（首次出现即分配） */
const char *ty_err_name(uint16_t code);             /* 码 → 名字；未知返回 NULL */
int         ty_err_count(void);                     /* 已登记的错误码总数 */
void       *ty_errset_of(const char **names, int n);/* 名字集合 → interned 集合 */
void       *ty_errset_one(const char *name);        /* 单元素集合 */
bool        ty_errset_has(const void *set, uint16_t code);
bool        ty_errset_subset(const void *a, const void *b);   /* a ⊆ b? */
void       *ty_errset_union(const void *a, const void *b);     /* a ∪ b */
int         ty_errset_size(const void *set);
const char *ty_errset_name_at(const void *set, int i);
uint16_t    ty_errset_code_at(const void *set, int i);
const char *ty_errset_str(const void *set);         /* "error{a, b}"（interned） */
Type       *ty_errset_type(void *set);              /* TY_ERRORSET 的类型对象 */
Type       *ty_err_set(Type *elem, void *set);      /* 带错误集的 !T */
void       *ty_type_errset(Type *t);                /* TY_ERRORSET → ErrSet*，否则 NULL */

/* ---------- 查询 ---------- */
bool  ty_eq(Type *a, Type *b);          /* 结构判等（指针相等或同构） */
bool  ty_is_int(Type *t);
bool  ty_is_float(Type *t);
bool  ty_is_num(Type *t);
bool  ty_is_scalar(Type *t);
bool  ty_is_agg(Type *t);
int   ty_size(Type *t);                 /* -1 = 编译期类型，没有运行时大小 */
int   ty_align(Type *t);

/* "放宽"的布局：comptime_int / comptime_float 按 R7 的默认宽度（i32 / f64）算。
 * 元组的 .size 和 @sizeOf 都要用这个，而不是 ty_size——后者对
 * `{1, "two", 3.0}` 这种含字面量的元组会返回 -1。 */
int   ty_size_relaxed(Type *t);
int   ty_align_relaxed(Type *t);
Field *ty_field(Type *t, const char *name);
int   ty_field_index(Type *t, const char *name);
/* 静态成员：结构体的在 fields 里（is_static），枚举的在 statics 里 */
Field *ty_static_member(Type *t, const char *name);
Method *ty_method(Type *t, const char *name);
const char *ty_str(Type *t);            /* interned 显示串 */
const char *ty_kind_name(TyKind k);

#endif /* ZXC_TY_H */
