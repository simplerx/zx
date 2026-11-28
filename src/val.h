/* val.h — 编译期值模型。
 *
 * 一个 Value 是"已经算出来的东西"：数字、布尔、字符串、类型、聚合值、
 * 函数。它只在编译期存在，编译器用它做三件事：
 *
 *   1. 折叠常量（`const` 必须算出来，R1）；
 *   2. 给泛型传参（`List(i32)` 里 i32 是一个 V_TYPE）；
 *   3. 物化静态数据（`&.{1,2,3}` 变成一块 const 数组）。
 *
 * 内存只分配不回收：编译器进程活得很短，值通常也很小。
 */
#ifndef ZXC_VAL_H
#define ZXC_VAL_H

#include "zxc.h"
#include "ty.h"

typedef struct Value Value;

/* §244：f128 的编译期值容器。宿主编译器必须真有 IEEE-754 quad——
 * `__SIZEOF_FLOAT128__` 在 zig cc 的 aarch64 目标上也报 16（宏不可信），
 * 所以按"结构 + 该结构是否真为 quad"判：x86_64 且非 macOS。macOS 的
 * long double 是 8 位宽的 IEEE-128double（对齐 8、双-double 实现），
 * 不是 quad，拿它冒充 f128 会造成静默精度错误。
 * 宿主不支持时 Zx 仍**能编译**（编译器自身可移植），但用到 f128 的程序
 * 会被 §244 的能力门控挡下并给明确诊断——绝不悄悄降级成 f64，那正是
 * R10 禁止的「两个答案」。 */
#if (defined(__SIZEOF_FLOAT128__) && __SIZEOF_FLOAT128__ == 16 && \
     defined(__LDBL_MANT_DIG__) && __LDBL_MANT_DIG__ == 113) || \
    (defined(__x86_64__) && !defined(__APPLE__))
#define ZX_HAVE_F128 1
typedef __float128 ZxF128;
#else
#define ZX_HAVE_F128 0
typedef long double ZxF128;   /* 占位：ZX_HAVE_F128=0 时永不参与运算 */
#endif

typedef enum {
    V_VOID,
    V_UNDEF,      /* undefined：有类型没有值 */
    V_NULL,       /* null：?T 的空 */
    V_INT,        /* ival + neg；big 时没有精确值 */
    V_FLOAT,
    V_BOOL,
    V_STR,        /* sval/slen，指向源文本或静态串 */
    V_TYPE,       /* tval：类型是一等值 */
    V_AGG,        /* items：按字段顺序；结构体/联合体/元组/数组都用它 */
    V_FN,         /* fn：函数节点（单态化时克隆） */
    V_PTR,        /* base + off：指向另一个 Value 内部 */
} ValKind;

struct Value {
    ValKind     kind;
    Type       *ty;        /* 值的类型；V_TYPE 时是 ty_type() */
    Type       *tval;      /* V_TYPE */
    Node       *fn;        /* V_FN */
    Value      *base;      /* V_PTR */
    uint64_t    off;       /* V_PTR：字节偏移；V_UNDEF（ptr 槽）：字段字节偏移 */
    Node       *xexpr;     /* §118 ptr 槽：基址表达式（&inst 的节点克隆）。
                            * ptr 是运行期地址，折不成编译期常量——ct 折值
                            * 遇到它返回 NULL（R1），发码时后端现拼。 */
    Type       *xt;        /* §118 续（用户二次拍板）：statics 的 ptr 槽——
                            * 容器类型（xexpr 为空时生效）。后端把静态成员全部
                            * 物化成 C 全局，所以这就是 &全局 的真指针，不需要
                            * 实例；off 是成员在 fields（struct/union）或
                            * statics（enum）数组里的下标，`.0` 在检查期改写
                            * 成 T.<成员>。 */
    bool        neg;       /* V_INT：负数，ival 是绝对值 */
    bool        big;       /* V_INT：超出 u64，值在 {hi, ival}（绝对值的 128 位表示） */
    uint64_t    hi;        /* V_INT：绝对值的高 64 位（big 时有效） */
    uint64_t    ival;      /* V_INT：绝对值；V_AGG(TY_UNION)：active 字段，UINT64_MAX 未知 */
    double      fval;
    ZxF128      f128val;    /* V_FLOAT 且 ty 是 f128 时生效 */
    bool        bval;
    const char *sval;
    int         slen;
    Value     **items;
    int         nitems;
};

Value *val_new(ValKind k, Type *t);
Value *val_void(void);
Value *val_undef(Type *t);
Value *val_null(Type *t);
Value *val_int(Type *t, uint64_t v, bool neg);
/* 超出 u64 的十进制文本 → 128 位绝对值。t 必须是 i128/u128。 */
Value *val_big128(Type *t, const char *text, bool neg);
void   val_big_split(const char *text, uint64_t *hi, uint64_t *lo);
Value *val_from_i64(Type *t, int64_t x);
Value *val_float(Type *t, double f);
Value *val_f128(Type *t, ZxF128 f);
/* 浮点字面量：f128 按原文解析（strtod 丢精度），其余按目标精度舍入。 */
Value *val_float_lit(Type *t, const char *text, double f);
Value *val_bool(bool b);
Value *val_str(const char *s, int n);
Value *val_type(Type *t);
Value *val_agg(Type *t, int n);
Value *val_fn(Type *t, Node *fn);
Value *val_ptr(Type *t, Value *base, uint64_t off);

/* 查询 */
bool    val_is_int(Value *v);
int64_t val_i64(Value *v);
bool    val_truthy(Value *v);          /* 编译期 if/while 的条件 */
bool    val_eq(Value *a, Value *b);    /* 用于 switch / 模式比较 */

/* 运算：返回 NULL 表示"这个操作在编译期做不了"（例如浮点取模） */
Value  *val_bin(int op, Value *a, Value *b, int line, int col);
Value  *val_un(int op, Value *a, int line, int col);
/* 显式转换（(T)e）：只做截断与重解释 */
Value  *val_cast(Type *to, Value *v);

void    val_dump(Value *v, Buf *b);

#endif /* ZXC_VAL_H */
