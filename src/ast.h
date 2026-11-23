/* ast.h — AST node definitions for zxc. */
#ifndef ZXC_AST_H
#define ZXC_AST_H

#include "zxc.h"

typedef enum {
    NK_INT, NK_FLOAT, NK_STR, NK_CHAR,
    NK_IDENT,           /* name */
    NK_BOOL, NK_NULL, NK_UNDEF,
    NK_BIN,             /* a op b (op = TokKind) */
    NK_RANGE,           /* lo .. [hi]  (hi==NULL → open, for/multi-bind only) */
    NK_UN,              /* op a  — -, +, !, ~, & (addr) */
    NK_UNWRAP,          /* a.? — 可选/错误联合体解包；空则 trap（G1） */
    NK_ORELSE,          /* a orelse b — 空/错则取 b（G1） */
    NK_TRY,             /* try a — !T 出错就把同一个码原样返回（v0.23） */
    NK_CAST,            /* (T)e — a=type, b=expr */
    NK_INDEX,           /* base[idx] */
    NK_SLICE,           /* base[lo..hi] (hi NULL → open) */
    NK_FIELD,           /* base.name / base.N (name NULL → idx in ival) */
    NK_CALL,            /* callee(args) */
    NK_TUPLE,           /* { e, ... }  items */
    NK_ANON_STRUCT,     /* .{ .x = e } items = NK_PAIR */
    NK_STRUCT_LIT,      /* Type{ .f = e } a=type, items = NK_PAIR */
    NK_ARRAY_LIT,       /* &.{ e, ... } items */
    NK_PAIR,            /* .name = expr  (name; a = value) */
    NK_BLOCK,           /* { stmts } label? */
    NK_IF,              /* a=cond, b=then, c=else; name=capture; str=label? */
    NK_WHILE,           /* a=cond, b=body, c=else; label */
    NK_FOR,             /* seqs+binds in items (seqs first, then binds), b=body, c=else */
    NK_SWITCH,          /* a=subject, items = NK_ARM */
    NK_ARM,             /* pats in items (or flag=else), a=body, label */
    NK_RETURN,          /* a */
    NK_CONTINUE,        /* label = tag? */
    NK_YIELD,           /* label = tag, a = value (NULL → {}) */
    NK_DEFER,           /* a */
    NK_ERRDEFER,        /* a —— errdefer：仅当函数以错误返回（.tag != 0，含 try 传播）时触发 */
    NK_BUILTIN,         /* @name(args) */
    NK_MODULE,          /* @mod("str") */
    NK_DECL,            /* const/let/var name [: T] = init  (flag: 0=const,1=let,2=var) */
    NK_FN,              /* [name](params) ret body — ret NULL/flag when none */
    NK_PARAM,           /* name: type (name NULL → anon), flag = is_var */
    /* type expressions */
    NK_TYPE_IDENT,      /* name (error/type keywords too) */
    NK_PTR_TYPE,        /* *[N]elem / *elem — a=elem, b=len (NULL→none), flag=is_const */
    NK_SLICE_TYPE,      /* []elem — flag = is_const */
    NK_TUPLE_TYPE,      /* { T, ... } */
    NK_STRUCT_TYPE,     /* flag: unused (packed 修饰符已删除，§192) */
    NK_UNION_TYPE,      /* tagged if any member is tuple-typed */
    NK_ENUM_TYPE,       /* a = backing type */
    NK_OPT_TYPE,        /* ?elem */
    NK_ERR_TYPE,        /* !elem；带集合时 a=错误集, b=elem（`error{a}!elem`） */
    NK_ROOT,            /* file */
    /* 追加在末尾：kind 编号在自举前端（bootstrap/parse.zx 的 NK）里是
     * 同序镜像，插在中间会让所有旧编号漂移。 */
    NK_ERR_SET,         /* error{a, b}：items = NK_PAIR（name 即成员名） */
    NK_VEC_TYPE,        /* @Vec(T, N)：a = 元素类型，b = 长度（编译期整数） */
    NK_TAG_APPLY,       /* tag.name / tag.name.static / tag.name : expr
                         * name = 标签名，b = 被贴标表达式（NULL = 定义/引用），
                         * flag = static（声明处），ival = tag 账本 id（sema 填） */
    NK_TRY_CAPTURE,     /* try expr err { ... }：a = !T/?T 表达式，
                         * name = 错误捕获名，b = 捕获块（产出兜底值 T）。
                         * 出错不外抛，在块里处理；tag 臂账本挂在本节点上。 */
    NK_TAG_BIND,        /* §174 续链：tag newVar : tag.myTag = oldVar; —— parse
                         * 降糖成 `let newVar = <本节点>`。a = tag 值（已存在
                         * 的 tag.myTag / const 名），b = 丢失标签的旧值；
                         * ival = tag 账本 id（sema 填）。只绑**无标签**值。
                         * 追加在末尾——与 bootstrap/parse.zx 的 NK 同序
                         * 镜像，插在中间会让所有旧编号漂移。 */
    NK_REPEAT_TYPE,     /* [T; N]：N 个 T 的内联元组值类型（Rust 风格）。
                         * a = 元素类型，b = 长度（编译期整数）。sema 期展开成
                         * N 个 T 的 TY_TUPLE，复用元组的布局/发码/下标/反射。
                         * 追加在末尾，保持与 bootstrap NK 同序。 */
} NodeKind;

struct Node;
typedef struct Node Node;
struct Type;
struct Symbol;

struct Node {
    NodeKind    kind;
    int         line, col;
    int         op;         /* TokKind for BIN/UN */
    const char *name;       /* ident / field / capture / builtin */
    const char *label;      /* tag on a block/loop/switch, or yield target */
    uint64_t    ival;       /* INT value / field index */
    double      fval;
    const char *str;        /* string content / enum tag name */
    int         str_len;
    Node       *a, *b, *c, *d;
    Node      **items;
    int         nitems;
    bool        flag;
    /* §221：捕获指针——`if (?T) *t {}` / `while (?T) *t {}` / switch 捕获臂
     * 的 `*` 前缀。挂在 NK_IF / NK_WHILE / NK_ARM 上，与 n->name（捕获名）
     * 配套：为真时捕获名绑定到 *载荷类型而非载荷值，后端取原对象地址写回。
     * 序列化走 flg bit3，见 astload.c / bootstrap/zxfe.zx。 */
    bool        cap_ptr;
    /* inline for：sema 按迭代次数克隆循环体，后端逐份发射。
     * 只挂在 NK_FOR 上（flag = 是不是 inline）。 */
    Node      **unroll;
    int         nunroll;
    bool        is_pub;     /* 带 pub：模块外可见（顶层声明与容器成员都算） */
    const char *mod;        /* 声明所在的文件（模块路径）；sema 装载时统一打戳 */
    bool        is_extern;  /* `extern fn …;`：外部 C 函数，没有函数体 */
    /* filled in by sema */
    struct Type   *ty;        /* 表达式的类型（类型值则是 TY_TYPE） */
    struct Type   *type_val;  /* ty == TY_TYPE 时，它表示的那个类型 */
    struct Value  *ct;        /* 编译期值；NULL = 只能运行时算 */
    struct Node   *inst;      /* 泛型调用：单态化出来的那份函数 */
    struct Symbol *sym;
    bool          esc;        /* 值指向当前栈帧（std.stackAlloc 调用打上；逃逸检查用） */
    bool          body_checked;
    bool          meth_done;  /* 方法调用已补过接收者（重复检查时直接返回类型） */
    bool          ct_no;      /* A1 负缓存：检查阶段这块子树折不出编译期值。
                               只在 g_ct_exec==0 时读写——编译期执行阶段 var
                               槽位是活的，同一棵树可能算得出值。 */
    int           index;    /* destructuring slot */

    /* §130 分配区间账本：编译期状态机的发行版本 + 预分配合并布局。
     * 全是编译期信息，编译完擦除；指针仍是 8 字节、[]T 仍是 16 字节。
     * reg_slot   NK_CALL(std.heapAlloc) 的发行版本 id（0 = 无 / opaque）。
     * reg_gid    NK_CALL 所属合并组 / NK_BLOCK 上的组标记（0 = 无，从 1 起）。
     * reg_off    NK_CALL 在合并块里的字节偏移。
     * reg_total  NK_BLOCK 上该组的总字节数。
     * reg_alloc  NK_BLOCK 上该组共用的分配器表达式节点。 */
    int           reg_slot;
    int           reg_gid;
    uint64_t      reg_off;
    uint64_t      reg_total;
    Node         *reg_alloc;

    /* tag 指针生命周期账本：贴标载体在节点上携带 (tag id, 载体状态)。
     * tag_state: 0 = 无 / 1 = LIVE / 2 = FREED / 3 = COND（分支合并出
     * 「可能是 null」）/ 4 = UNKNOWN（分支间状态不一致，未防守）。
     * 全是编译期信息，编译完擦除。 */
    int           tag_id;
    int           tag_state;
    /* §187：整数表达式带「指针出处」污点——值曾由指针→整数转换得来
     * （算术运算不洗源）。污点整数再转回指针必须显式贴标（E0525）。
     * 编译期信息，编译完擦除。 */
    unsigned char ptr_taint;
    /* §187：std.free / a.deinit(s) 变换产物（改写形状 a.free(...)）。
     * 唯一入口拦截（E0527）要放行变换产物、只拦手写的 al.free(...)。 */
    unsigned char xform_free;
    /* §187：adopt / 容器开账 / 总清算三个钩子已在该调用节点跑过
     * （deps 拓扑会复检函数体，钩子必须节点级幂等）。 */
    unsigned char tag_hooks_done;
    void          *ftags;     /* §173：结构体值携带的字段标记账本
                                 * （FieldTag 链，sema 私有类型） */

    /* §174：返回流烙的载体终局状态（0 无 / TC_LIVE / TC_FREED）。
     * 臂内分配的身份由臂值节点携带、槽状态已被 tag_restore 回滚，
     * 跨函数返回流在调用点读不到槽状态时靠这个字段继承。
     * 只挂 NK_FN（sema 私有，编译完擦除）。 */
    int           ret_tag_state;

    /* §175：本函数被直接调用的次数（sema 私有，只挂 NK_FN）。
     * 工厂函数（分配经返回流转出所有权）零调用 = 分配在运行期
     * 根本不发生，终扫豁免其槽。 */
    int           tag_calls;

    /* §176：tag_call_flow 已在本节点完成对账（sema 私有，只挂调用节点）。
     * ct 折堆 / deps 补查会克隆已检查过的函数体重查一遍，
     * 克隆节点带着首次检查的这个标记——重跑只会
     * 重复克隆工厂实例槽、记下假调用边，直接跳过。 */
    bool          tag_done;
};

Node *node_new(NodeKind k, int line, int col);

/* 后端要用符号名：Symbol 本体是 sema.c 私有的，这里只暴露名字。 */
const char *sema_sym_name(struct Symbol *s);
/* 模块内裸名内建调用：返回六个内建 API 的名字，否则 NULL。
 * Symbol 本体是 sema.c 私有的，后端只按这个字符串分派发射。 */
const char *sema_sym_builtin_name(struct Symbol *s);
/* 命令行第 0 个参数：用来找随附的 std/ 源码 */
void sema_set_argv0(const char *p);
Node *parse_program(Lexer *lx);
/* 后端生成 C 名字用：模块内的名字带模块前缀，避免多文件重名 */
const char *sema_sym_cname(struct Symbol *s);
/* 这个符号是 `extern fn` 声明的外部 C 函数吗？（名字原样发，不转义） */
bool sema_sym_is_extern(struct Symbol *s);
/* 符号指向的声明节点（NK_DECL / NK_FN / NULL）。局部 fn 绑定引用要
 * 用它判断"这是函数体里的 fn 字面量绑定"——C 层函数名带 _impl 后缀。 */
Node *sema_sym_decl(struct Symbol *s);
/* 符号声明在函数体里吗（它的槽位在栈帧上） */
bool sema_sym_in_fn(struct Symbol *s);
/* 模块表：0 是入口文件 */
int   sema_module_count(void);
Node *sema_module_root(int i);
const char *sema_module_path(int i);
const char *sema_module_stem(int i);
bool sema_fn_is_generic(Node *fn);
/* 这个符号在生成的 C 里有自己的存储吗？泛型实参、运行时形参都没有。 */
bool sema_sym_has_storage(struct Symbol *s);
/* 符号上挂的编译期值（泛型实参用得到） */
struct Value *sema_sym_ct(struct Symbol *s);
/* 这个类型只在编译期存在吗？（std.TypeInfo / std.FieldInfo——里面装着
 * type 值，C 里没有对应物；后端用它跳过这些"声明"的物化） */
bool sema_ty_ct_only(struct Type *t);
/* 内建分配器类型（std.heap.page 的类型） */
struct Type *sema_allocator_type(void);
/* 泛型实例：签名前面还有几个参数是编译期的 */
int  sema_fn_ncomp(Node *fn);

/* s-expression dump for debugging */
void ast_dump(const Node *n, int depth);

/* 自举阶段 3：装载 Zx 前端（zxfe）产出的序列化 AST。
 * 失败（格式坏 / 打不开）返回 NULL。文本版给测试用。 */
Node *ast_load_file(const char *path);
Node *ast_load_text(const char *text, size_t len);

/* 前端选择（src/fe.c）：默认 Zx 前端，--fe=c 切回 C 解析器交叉验证。 */
void  fe_set_mode(bool use_zx);
void  fe_set_argv0(const char *p);
bool  fe_is_zx(void);
Node *fe_parse_file(const char *path);
/* Zx 前端（zxfe 子进程）刚报过的第一条错：导入方拼 "failed to parse" 时
 * 带上它，否则真正的原因（比如子模块里一条坏字面量）用户根本看不到。 */
void fe_last_error_reset(void);
bool fe_last_error(int *line, int *col, char *code, size_t ccap,
                    char *msg, size_t mcap);

#endif
