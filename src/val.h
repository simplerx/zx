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
    bool        neg;       /* V_INT：负数，ival 是绝对值 */
    bool        big;       /* V_INT：超出 u64，值在 {hi, ival}（绝对值的 128 位表示） */
    uint64_t    hi;        /* V_INT：绝对值的高 64 位（big 时有效） */
    uint64_t    ival;
    double      fval;
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
