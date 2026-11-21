/* sema.c — 名字解析 + 类型检查。
 *
 * 三条贯穿全文件的原则：
 *
 *  1. 一个作用域先全部 hoist（R5：同作用域声明互相可见、与书写顺序无关），
 *     再按语句顺序检查；符号的类型是惰性求值的，成环立刻报错。
 *  2. 类型只由 ty.c 构造，判等就是指针比较；AST 上每个表达式都带 ty。
 *  3. "隐式"只有 R7 那五条（免拷贝提升），其余一律要求类型精确相等。
 */
#include "zxc.h"
#include "ast.h"
#include "ty.h"
#include "val.h"
#include <time.h>   /* clock()：ZX_TIMING2 的临时埋点 */
#include <direct.h> /* _getcwd：§189 豁免判定的相对路径绝对化 */

#define BAD(n, ...) (err_at((n)->line, (n)->col, __VA_ARGS__), ty_invalid())
/* 码由调用点指定：措辞会改，码不改（§17.1） */
#define BAD_CODE(code, n, ...) \
    (diag_code_at((code), (n)->line, (n)->col, __VA_ARGS__), ty_invalid())

/* ---------------- fix-it：did you mean（A2 · M42） ----------------
 * 三类建议：未知标识符（作用域链）、聚合成员（fields/methods/statics）、
 * std 与模块成员（名字表 / 模块作用域）。在 err_at 之前调 diag_help，
 * 建议行跟在那条诊断后面；没有合适的候选就不挂，诊断保持原样。 */

/* OSA 距离：Levenshtein + 相邻换位算 1 次编辑。手滑换位（naem / coutn）
 * 是最高频的拼写错误，纯 Levenshtein 会把它算成 2 次而错过建议。 */
static int edit_distance(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 64 || lb > 64) return 999;
    int d[66][66];
    for (int i = 0; i <= la; i++) d[i][0] = i;
    for (int j = 0; j <= lb; j++) d[0][j] = j;
    for (int i = 1; i <= la; i++) {
        for (int j = 1; j <= lb; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int m = d[i - 1][j] + 1;                          /* 删 */
            if (d[i][j - 1] + 1 < m) m = d[i][j - 1] + 1;     /* 插 */
            if (d[i - 1][j - 1] + cost < m) m = d[i - 1][j - 1] + cost;  /* 换 */
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                if (d[i - 2][j - 2] + 1 < m) m = d[i - 2][j - 2] + 1;    /* 换位 */
            d[i][j] = m;
        }
    }
    return d[la][lb];
}

/* 阈值：短名容 1 次编辑，长名容 2 次——按长度比例放太宽会把 3 字母
 * 名字配到很远的候选上。 */
static bool name_close(const char *bad, const char *cand) {
    if (!bad || !cand || !*cand) return false;
    return edit_distance(bad, cand) <= (strlen(bad) <= 4 ? 1 : 2);
}

/* 聚合类型的成员建议：fields + methods + statics（枚举体的 const/var）。 */
static const char *suggest_member(const char *bad, Type *t) {
    if (!t || !bad) return NULL;
    const char *best = NULL;
    int bd = 999;
    for (int i = 0; i < t->nfields; i++) {
        const char *c = t->fields[i].name;
        if (!name_close(bad, c)) continue;
        int d = edit_distance(bad, c);
        if (d < bd) { bd = d; best = c; }
    }
    for (int i = 0; i < t->nmethods; i++) {
        const char *c = t->methods[i].name;
        if (!name_close(bad, c)) continue;
        int d = edit_distance(bad, c);
        if (d < bd) { bd = d; best = c; }
    }
    for (int i = 0; i < t->nstatics; i++) {
        const char *c = t->statics[i].name;
        if (!name_close(bad, c)) continue;
        int d = edit_distance(bad, c);
        if (d < bd) { bd = d; best = c; }
    }
    return best;
}

/* std 命名空间的成员建议。名字表与 std_lookup 的 key 手工对齐——
 * 漂移的代价只是"少一条建议"，不会出错，别为它建机制。 */
static const char *suggest_std_member(const char *ns, const char *bad) {
    static const char *const std_top[] = { "print", "println", "format", "heap",
        "file", "io", "Allocator", "Type", "Field",
        "stackAlloc", "heapAlloc", "heapSlice", "free", "process", NULL };
    static const char *const io_m[] = { "read", "write", "openRead", "openWrite",
        "openAppend", "seek", "tell", "close", "flush", "stdin", "stdout",
        "stderr", NULL };
    static const char *const file_m[] = { "read", "write", "append", "exists",
        "remove", NULL };
    static const char *const proc_m[] = { "args", "getenv", "setenv", "exit",
        NULL };
    static const char *const heap_m[] = { "page", NULL };
    const char *const *tab = NULL;
    if (!strcmp(ns, "std"))        tab = std_top;
    else if (!strcmp(ns, "std.io"))     tab = io_m;
    else if (!strcmp(ns, "std.file"))   tab = file_m;
    else if (!strcmp(ns, "std.process")) tab = proc_m;
    else if (!strcmp(ns, "std.heap"))   tab = heap_m;
    if (!tab) return NULL;
    const char *best = NULL;
    int bd = 999;
    for (int i = 0; tab[i]; i++) {
        if (!name_close(bad, tab[i])) continue;
        int d = edit_distance(bad, tab[i]);
        if (d < bd) { bd = d; best = tab[i]; }
    }
    return best;
}

/* ---------------- 符号与作用域 ---------------- */

enum { SK_CONST, SK_LET, SK_VAR, SK_PARAM, SK_CAPTURE, SK_FN, SK_NS };

typedef struct Symbol Symbol;
typedef struct FieldTag FieldTag;   /* §173 字段账本，定义在 tag 一节 */
struct Symbol {
    const char *name;
    int         kind;
    Node       *decl;      /* NK_DECL / NK_FN / NULL（参数、捕获） */
    Type       *ty;        /* 值的类型；类型值的符号这里是 TY_TYPE */
    Type       *type_val;  /* ty == TY_TYPE 时它表示的类型 */
    int         state;     /* 0 未解析 / 1 解析中 / 2 完成 */
    int         index;     /* 解构槽位，-1 表示不是解构出来的 */
    Value      *ct;        /* 编译期值（const 一定有，其余可能有） */
    int         builtin;   /* SK_NS 成员的内建编号 */
    struct Module *mod;    /* 属于哪个模块（文件） */
    bool        is_pub;    /* 顶层声明带 pub，模块外可见 */
    bool        is_std;    /* 内建 "std" 命名空间 */
    bool        broken;    /* 解析失败的绑定（module 加载失败）：成员访问一律闭嘴 */
    bool        ct_bind;   /* 泛型的编译期实参：没有 C 变量，值直接内联 */
    bool        in_fn;     /* 声明在函数体里：它的槽位在当前栈帧上 */
    int         fn_d;      /* 声明时的函数嵌套深度（in_fn 局部用，§192 闭包检查：
                            * 更浅深度 = 外层函数的栈槽，嵌套函数读到就是捕获） */
    bool        fn_local;  /* hoist 时确认是函数块内的绑定：块内先声明后使用要报 */
    bool        is_top;    /* 文件顶层声明：后端把它发成 C 全局变量，初始值镜像归它管 */
    bool        used;      /* 被读过或写过一次；没碰过的局部绑定要报（M66） */
    struct Symbol *locals_next;  /* 本函数体的局部登记链（见 local_track） */
    bool        stack_val; /* 它的值指向当前栈帧（逃逸检查用，见 addr_frame） */
    int         reg_slot;  /* §130 发行版本 id：0 = 无 / opaque（见 region_ledger） */
    int         tag_id;    /* §170 tag 账本 id（TagSlot 下标+1，0 = 无） */
    int         tag_state; /* §170 载体状态 TC_*（0 NONE / 1 LIVE / 2 FREED /
                            * 3 COND / 4 UNKNOWN） */
    FieldTag   *ftags;     /* §173：结构体值载体携带的字段账本（FieldTag 链） */
    Node       *ft_src;    /* §192：转移溯源——字段账随 DECL/ASG 从这个表达式
                            * 子树拷来；新载体上的字段写入沿它镜像回源账本，
                            * 两个实例读写同一存储位置口径一致（跟丢防护） */
    bool        ptr_taint; /* §187：整数符号带指针出处污点（E0525 用） */
    const char *cname;     /* 后端用的 C 名字（模块内名字带前缀） */
    int         decl_line; /* 声明位置（D2 note 用；0 = 没记） */
    int         decl_col;
    /* 分配器去虚拟化（§138）的三条否定证据：声明后被重新赋值过、
     * 被 & 取过地址（指针可能改写它）、以及 if/while 捕获的初值来源。
     * 只要有一条不干净，`a.alloc(...)` 就保持原样的间接调用。 */
    int          nassign;  /* `a = …` / `a += …` 的次数 */
    bool         escaped;  /* 被 `&a` 取过地址 */
    Node        *cap_src;  /* SK_CAPTURE：捕获的源表达式（?T 解包前） */
    /* 声明在容器体（struct/union/enum 的 body）里时指向那个体。容器内的
     * 静态函数在 C 层是实例名（`a_i0`），而裸名引用查到的符号 cname 只是
     * 裸 `a` —— 两条命名路径对不上（B1001 call to undeclared）。带上体节点，
     * 引用点就能给后端同一个实例节点，见 check_expr 的 NK_IDENT。
     * 模块级函数为 NULL。 */
    Node        *in_agg;
    /* §151：`Self` 接收者按编译期大小升级成 const 指针时，形参符号的 ty
     * 仍是 `Self`（值语义），后端发射这个标识符要补 `(*self)`。 */
    bool         self_by_ptr;
    Symbol     *next;
};

/* 哈希桶里的一个挂件。不能把链接存在 Symbol 上：snap_scope 会让两个作用域
 * **共享同一批 Symbol**，各自 rehash 时就会互相改坏对方的链。 */
typedef struct HBucket {
    Symbol         *sym;
    struct HBucket *next;
} HBucket;

typedef struct Scope {
    struct Scope *parent;
    Symbol       *first;
    bool          is_module;   /* 文件级作用域：活得和模块一样久，快照时直接引用 */
    /* 名字 → 符号的哈希索引。之前是纯链表，于是"N 个顶层声明"要付 O(N²)：
     * 8000 个函数编 437 ms，16000 个 1.8 s（每翻一倍耗时约 4.3 倍）。
     * 链表仍是唯一权威（顺序、遍历、快照都看它），这张表只回答"这个名字在不在"。 */
    HBucket      **tab;
    int           ntab;        /* 桶数，2 的幂 */
    int           cnt;
    int           tag_wm;      /* §173 补丁八：push 时的槽位水位线——
                              * 本作用域期间创建的栈分配槽，退出时统一
                              * 标 free（槽可能只挂在字段/元素账上，
                              * 没有直接载体符号，按符号扫会漏） */
} Scope;

static Scope *g_scope;
/* 当前函数体登记过的全部局部绑定（参数、let/var、解构槽、捕获）。
 * 为什么另立一条链：作用域是**栈上**的，出块就销毁，符号却活得久——
 * 想在函数体结束时统一问"哪些一次都没用过"，只能自己记一份。 */
static Symbol *g_locals;

/* 作用域链上找与 bad 最接近的名字（fix-it）。符号名的寿命覆盖诊断
 * 打印，直接返回指针。 */
static const char *suggest_ident(const char *bad) {
    const char *best = NULL;
    int bd = 999;
    for (Scope *sc = g_scope; sc; sc = sc->parent) {
        for (Symbol *s = sc->first; s; s = s->next) {
            if (!name_close(bad, s->name)) continue;
            int d = edit_distance(bad, s->name);
            if (d < bd) { bd = d; best = s->name; }
        }
    }
    return best;
}

/* 一个文件就是一个模块。入口文件是 g_mods[0]。
 * 别的文件只能看到本模块里带 `pub` 的顶层声明。 */
typedef struct Module {
    const char *path;
    const char *stem;      /* 文件名（不含扩展名），用于 C 名字前缀与错误信息 */
    Node       *root;
    Scope       scope;     /* 文件级作用域 */
    Symbol     *ns;        /* 对外的命名空间符号 */
    bool        loading;   /* 正在加载：用来发现循环导入 */
    bool        tags_closed; /* §170：本模块写了 `tag.close;`，堆分配不强制贴标 */
    bool        tags_batch;  /* §188：本模块写了 `tag.batch;`——批量清算型分配器
                              * （arena）：经它的 Allocator 视图在调用点做的
                              * alloc/free 一并豁免，内存由 init/deinit 容器账
                              * 管。tag.close 只豁免模块内部申请；pool 这类
                              * 逐笔释放的分配器只有 close、没有 batch。 */
} Module;

static Module **g_mods;
static int      g_nmods, g_cmods;
static Module  *g_mod;
/* 正在检查文件根的语句列表（模块加载 / 入口根）。文件根上的惰性模块
 * 声明才允许推迟解析；函数体、comptime 块里的 decl 一律当场算。
 * check_block 进块时清掉它——嵌套块里的 decl 不得享受文件根的豁免。 */
static bool g_root_stmts;
static bool g_ct_stuck;
static int  g_ct_exec;
/* 检查期投机折叠（fold_expr）期间置位：这段折叠只为填 ct 缓存，
 * 折不出值是常态。除零这类"值域错误"在这期间软失败（返回 NULL），
 * 不做硬错误——循环体这类死代码里的 0/0 不是用户的错（isqrt(0) 误报）。
 * 真正的 const 上下文（R1 初始化器、数组长度）不走这条路，照旧硬报。 */
bool g_ct_soft_div;
/* 折叠函数调用时，体要"先检查、再执行"（ct_call → check_block → ct_run）。
 * 检查那一步没有真实的槽位绑定：局部 `var i` 的值可能还留着上一次执行的
 * 残留，于是 `s[i]` 会用「新实参 + 旧下标」折出越界。那不是用户代码的错，
 * 真正的执行（ct_run，绑定齐全）紧随其后。这期间越界软失败即可。 */
static bool g_ct_soft_bounds;
/* 成员可见性（容器体里默认私有、pub 才对外）：装载时打戳 + 访问点检查。
 * 定义在模块加载那一节，用在 check_field 里，先声明。 */
static void stamp_mod(Node *n, const char *path);
static bool deny_member(Node *n, Type *owner, bool is_pub, const char *name);
/* 一个 Comptime 栈帧：返回值 / 收割都靠它往上冒 */
typedef struct { int kind; Value *val; const char *label; } CtFlow;
static CtFlow g_ct_flow;
enum { CT_NORMAL, CT_RETURN, CT_YIELD, CT_CONTINUE };
static Value *ct_eval_probe(Node *n);

int   sema_module_count(void)      { return g_nmods; }
Node *sema_module_root(int i)      { return (i >= 0 && i < g_nmods) ? g_mods[i]->root : NULL; }
const char *sema_module_path(int i) { return (i >= 0 && i < g_nmods) ? g_mods[i]->path : NULL; }
const char *sema_module_stem(int i) { return (i >= 0 && i < g_nmods) ? g_mods[i]->stem : NULL; }
const char *sema_sym_cname(Symbol *s) { return s ? (s->cname ? s->cname : s->name) : NULL; }
/* extern 声明的符号：C 名就是外部符号名，后端不许再做关键字转义
 * （extern fn 与 extern var 都算——外部符号是别人编好的，改名就链不到） */
bool sema_sym_is_extern(Symbol *s) {
    if (!s || !s->decl || !s->decl->is_extern) return false;
    return s->decl->kind == NK_FN || s->decl->kind == NK_DECL;
}
Node *sema_sym_decl(Symbol *s) { return s ? s->decl : NULL; }
bool sema_sym_in_fn(Symbol *s) { return s ? s->in_fn : false; }
bool sema_sym_has_storage(Symbol *s) { return !(s && s->ct_bind); }
bool sema_sym_self_by_ptr(Symbol *s) { return s && s->self_by_ptr; }
Value *sema_sym_ct(Symbol *s) { return s ? s->ct : NULL; }

static int g_ntslots;   /* §173：tag 槽位计数（定义在贴标账本一节；
                        * scope_push 的槽位水位线要用，前移声明） */
static void scope_push(Scope *s) {
    s->parent = g_scope;
    s->first = NULL;
    /* 这几个必须一起清：Scope 大量是**栈上**的（`Scope sc; scope_push(&sc);`），
     * 不清的话 is_module 读到的就是上一个人用剩的栈字节 —— snap_scope 会把它
     * 当成模块作用域直接存进待检查队列，等这个栈帧没了就是悬空指针。 */
    s->is_module = false;
    s->tab = NULL;
    s->ntab = 0;
    s->cnt = 0;
    s->tag_wm = g_ntslots;
    g_scope = s;
}
static void scope_pop(Scope *s)  { g_scope = s->parent; }

static unsigned hash_name(const char *s) {
    unsigned h = 2166136261u;                    /* FNV-1a */
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

/* 按链表重建索引。链表才是权威，所以这里不看 cnt，只认它。 */
static void scope_rehash(Scope *sc, int n) {
    HBucket **t = calloc((size_t)n, sizeof *t);
    if (!t) return;
    for (Symbol *s = sc->first; s; s = s->next) {
        unsigned b = hash_name(s->name ? s->name : "") & (unsigned)(n - 1);
        HBucket *h = calloc(1, sizeof *h);
        if (!h) continue;
        h->sym = s;
        h->next = t[b];
        t[b] = h;
    }
    free(sc->tab);
    sc->tab = t;
    sc->ntab = n;
}

/* 只在本层找（不往上走）。有索引就走索引。 */
static Symbol *scope_find_here(Scope *sc, const char *name) {
    if (sc->ntab > 0) {
        unsigned b = hash_name(name) & (unsigned)(sc->ntab - 1);
        for (HBucket *h = sc->tab[b]; h; h = h->next)
            if (h->sym->name && strcmp(h->sym->name, name) == 0) return h->sym;
        return NULL;
    }
    for (Symbol *s = sc->first; s; s = s->next)
        if (s->name && strcmp(s->name, name) == 0) return s;
    return NULL;
}

static Symbol *scope_add(Scope *sc, const char *name, int kind) {
    /* `_` 是丢弃占位，不是名字：不进作用域——读不到（lookup 找不到就按
     * void 处理，正好是 `_ = expr;` 要的语义），也永远不会重名，同一作用域
     * 里写几次都行。符号还是给一个：调用方要往上面挂 ty/used 这些状态。 */
    if (name && !strcmp(name, "_")) {
        Symbol *s = calloc(1, sizeof(Symbol));
        if (!s) return NULL;
        s->name = name;
        s->kind = kind;
        s->index = -1;
        return s;
    }
    if (scope_find_here(sc, name)) return NULL;   /* 重复声明 */
    Symbol *s = calloc(1, sizeof(Symbol));
    s->name = name;
    s->kind = kind;
    s->index = -1;
    s->next = sc->first;
    sc->first = s;
    sc->cnt++;
    if (sc->ntab == 0 || sc->cnt * 4 > sc->ntab * 3) {
        scope_rehash(sc, sc->ntab ? sc->ntab * 2 : 16);   /* 重建时会把 s 一起收进去 */
    } else {
        unsigned b = hash_name(name) & (unsigned)(sc->ntab - 1);
        HBucket *h = calloc(1, sizeof *h);
        if (h) { h->sym = s; h->next = sc->tab[b]; sc->tab[b] = h; }
    }
    return s;
}

/* 登记一个局部绑定。line/col 只在符号自己没记的时候补——声明处的
 * 位置就是报"未使用"时要指的地方。 */
static void local_track(Symbol *s, int line, int col) {
    if (getenv("ZX_PROBE") && s && s->name && (!strcmp(s->name, "myidx") || !strcmp(s->name, "idx") || !strcmp(s->name, "dims3"))) fprintf(stderr, "[probe] TRACK %s sym=%p@%d:%d\n", s->name, (void*)s, line, col);
    if (!s || s->locals_next) return;
    s->locals_next = g_locals;
    g_locals = s;
    if (!s->decl_line) { s->decl_line = line; s->decl_col = col; }
}

/* 函数体收尾：把这条链上没被碰过一遍的绑定报出来。
 *
 * 为什么强制而不是警告：Zx 编到 C，未使用的局部变量在 C 里只是 warning，
 * 而用户看不到 C；更实际的是"声明了从没用过"几乎总是写漏了一半——
 * 打算用、结果没接上，或者名字拼错（拼错的地方是 E0401，但只写不读的
 * 拼错就完全静默）。不用的值只有一种写法：裸 `_`（`let _ = f();` 或
 * `_ = f();`）。`_foo` 这种"加前缀求豁免"的旧写法已删除——它让"我确实
 * 不用"和"我拼错了"长得一模一样，正是这条检查要防的东西。 */
static void check_unused(Symbol *head, int errs_before) {
    /* å½æ°ä½éå·²ç»æ¥è¿éå°±ä¸åé®"ææ²¡ææ²¡ç¨è¿çåé"ï¼
     * éè¯¯æ¢å¤ä¸è¿æ¡å¤æ­å¤åæ¯åè±¡ï¼è¡¨è¾¾å¼åºéâ
     * æ£æ¥æåæ¶å°¾âåé¢çä½¿ç¨æ²¡è¢«è®°ä¸ï¼ï¼
     * èä¸è¿ç§åªé³ä¼çä½çæ­£çæ ¹å ãä¿®å®ééç¼ï¼å®èªç¶ä¼åºç°ã */
    if (g_error_count != errs_before) return;
    for (Symbol *s = head; s; s = s->locals_next) {
        if (s->used || !s->name || !strcmp(s->name, "_")) continue;
        /* `self` 是方法接收者的惯用名：不用它的方法也得写出来（位置决定签名），
         * 叫人改成 `_self` 只会让方法看起来像函数。 */
        if (!strcmp(s->name, "self")) continue;
        if (s->kind == SK_FN || s->kind == SK_NS) continue;
        const char *what = s->kind == SK_PARAM   ? "parameter"
                         : s->kind == SK_CAPTURE ? "binding"
                         :                         "variable";
        diag_help("if you don't need it, bind it as `_`");
        err_at(s->decl_line, s->decl_col, "unused %s '%s'", what, s->name);
    }
}

static Symbol *lookup(const char *name) {
    for (Scope *sc = g_scope; sc; sc = sc->parent) {
        Symbol *s = scope_find_here(sc, name);
        if (s) return s;
    }
    return NULL;
}

/* 带声明位置的版本：只有"值得被 note 指回去"的声明（顶层、类型体成员）
 * 才记位置——参数、捕获这类就地重复的，note 没有增量价值。 */
static Symbol *scope_add_at(Scope *sc, const char *name, int kind, int line, int col) {
    Symbol *s = scope_add(sc, name, kind);
    if (s) { s->decl_line = line; s->decl_col = col; }
    return s;
}

const char *sema_sym_name(Symbol *s) { return s ? s->name : NULL; }

static bool fn_is_generic(Node *fn);
bool sema_fn_is_generic(Node *fn) { return fn_is_generic(fn); }
int  sema_fn_ncomp(Node *fn) { return fn ? (int)fn->ival : 0; }

/* ---------------- 标签栈 ---------------- */

typedef struct {
    const char *name;
    Type       *ty;
    bool        has;      /* 已经有过收割 */
    bool        is_loop;
    bool        is_fn;
    Node       *val;      /* 最后一次收割的值表达式（§170 臂出口 tag 检查用） */
} Label;

#define MAX_LABEL 256
static Label g_labels[MAX_LABEL];
static int   g_nlabels;

static Label *label_push(const char *name, bool is_loop) {
    if (g_nlabels >= MAX_LABEL) return NULL;
    Label *l = &g_labels[g_nlabels++];
    l->name = name; l->ty = NULL; l->has = false; l->is_loop = is_loop; l->is_fn = false;
    l->val = NULL;
    return l;
}
static void label_pop(void) { if (g_nlabels) g_nlabels--; }

static Label *label_find(const char *name) {
    for (int i = g_nlabels - 1; i >= 0; i--)
        if (g_labels[i].name && name && strcmp(g_labels[i].name, name) == 0)
            return &g_labels[i];
    return NULL;
}
static Label *label_find_loop(const char *name) {
    for (int i = g_nlabels - 1; i >= 0; i--) {
        Label *l = &g_labels[i];
        if (l->is_fn) return NULL;
        if (!l->is_loop) continue;
        if (!name) return l;
        if (l->name && strcmp(l->name, name) == 0) return l;
    }
    return NULL;
}

/* ---------------- 函数上下文 ---------------- */

static Type  *g_ret;
static int    g_fn_depth;
static bool   g_in_defer;
static bool   g_in_tag_rebind;   /* §187：续链值里的 int→ptr cast 免 E0525 */
static int    g_arm_depth;       /* §188：分支臂嵌套深度（释放禁区的判定）；
                              * 循环体不算臂（逐迭代 alloc/free 是惯用法），
                              * defer 体由 g_in_defer 单独豁免。 */

/* 待检查函数体：先建签名、后查体，自递归/互递归天然合法。
 *
 * 还要带上"当时的作用域"：泛型类型体里的方法要晚点才查，而 T / cap 这些
 * 泛型参数只活在 specialize() 那个栈上的作用域里。Scope 很小、符号在堆上，
 * 所以照抄一份链结构就够了。 */
typedef struct { Node *fn; Module *mod; struct Scope *scope; } Pending;
static Pending *g_pending;
static int g_npending, g_cappending;

/* §148/§149：工厂（DebugAllocator.init / Pool.allocator / ArenaAllocator.init）
 * 住在**惰性加载**的子模块里，调用方 main 的体先查时它还没查过：return
 * 的 Allocator 字面量没有 ty，`.alloc = f` 的标识符也没有 sym。
 * 反过来，否定证据（nassign / escaped）也可能在别的功能体里、比调用点
 * 更晚才置上。所以每个候选调用点都登记**原始的 NK_FIELD**，到所有排队体
 * 查完后用完整信息统一复核一次：该直调的直调，后来被抹源就回退。 */
typedef struct { Node *call; Node *field; } DevirtPending;
static DevirtPending *g_devirt_retry;
static int g_ndr, g_cdr;
static bool g_devirt_final;

typedef struct {
    Node   *fn;
    int     nargs;
    Value **args;
    Node   *clone;
    Type   *ty;
    Value  *val;      /* 类型构造函数的返回类型（V_TYPE） */
} Inst;

static Inst *g_insts;
static int   g_ninst, g_cinst;

/* 实例化嵌套层数（防止泛型体里再写一遍自己时无限套娃） */
static int   g_spec_depth;

/* 把当前作用域链拍个快照，等轮到检查这个函数体时再恢复。
 *
 * 注意模块级作用域**不能**复制：它比快照活得久，而且会继续长（编译器内建的
 * `print` / `io` 是在 std.zx 解析完、所有函数体都已排队之后才并进去的）。
 * 复制会把 `first` 冻在那一刻，之后加进来的名字就对已排队的函数体隐身——
 * 直接引用同一个 Scope，模块作用域永远是"最新"的。 */
static Scope *snap_scope(Scope *sc) {
    if (!sc) return NULL;
    if (sc->is_module) return sc;
    Scope *c = calloc(1, sizeof(Scope));
    c->parent = snap_scope(sc->parent);
    c->first  = sc->first;
    return c;
}

static void schedule_body(Node *fn) {
    if (fn->body_checked) return;
    fn->body_checked = true;
    if (g_npending == g_cappending) {
        g_cappending = g_cappending ? g_cappending * 2 : 64;
        g_pending = realloc(g_pending, sizeof(Pending) * (size_t)g_cappending);
    }
    g_pending[g_npending].fn    = fn;
    g_pending[g_npending].mod   = g_mod;
    g_pending[g_npending].scope = snap_scope(g_scope);
    g_npending++;
}

/* ---------------- 前向声明 ---------------- */

static Type *check_expr(Node *n, Type *expect);
static Type *check_expr_raw(Node *n, Type *expect);
static Value *fold(Node *n);
static void ct_clear(Node *n);      /* 在下面，ct 机器那一节 */
static void ct_clear_stmts(Node *n);
static Value *ct_eval(Node *n);
static Type *instantiate_type(Node *n);
static void  name_type_from_call(Node *call, Type *result);
static Type *instantiate_fn_type(Node *n);
static bool  fn_is_generic(Node *fn);
static Type *resolve_type(Node *n);
static void  check_stmt(Node *n);
static Type *check_block(Node *n);
static Type *check_fn_body(Node *fn);
static Type *resolve_fn_type(Node *fn);
static Type *coerce_node(Node *e, Type *from, Type *to, const char *ctx);
static Type *unify(Node *at, Type *a, Type *b);
static Type *scalar_default(Type *t);
static Type *sema_construct(Node *n, Type *expect);
static bool  falls_through(Node *blk);      /* 缺返回路径分析：块末尾可达吗 */
static void  check_return_paths(Node *fn, Type *ret);
static bool  stmt_blocks_next(Node *s);     /* 不可达代码：这条之后的下一条还到得了吗 */

/* ---------------- 内建 / 基础类型 ---------------- */
static Type *sema_type_type(void);
static Type *sema_type_field_type(void);
static Node *clone_tree(Node *n);       /* 泛型单态化用；type_desc_value 的 ptr 槽也要 */
static bool  lvalue_mutable(Node *n);   /* ptr 槽的只读性跟基址走 */

static Type *base_type(const char *n) {
    if (!n) return NULL;
    if (!strcmp(n, "void"))      return ty_void();
    if (!strcmp(n, "bool"))      return ty_bool();
    if (!strcmp(n, "type"))      return ty_type();
    if (!strcmp(n, "error"))     return ty_error();
    if (!strcmp(n, "tag"))       return ty_tag();     /* §172：tag 是内建类型 */
    if (!strcmp(n, "noreturn"))  return ty_noreturn();
    if (!strcmp(n, "usize"))     return ty_usize();
    if (!strcmp(n, "isize"))     return ty_isize();
    if (n[0] == 'u' || n[0] == 'i') {
        int bits = atoi(n + 1);
        if (bits == 8 || bits == 16 || bits == 32 || bits == 64 || bits == 128)
            return ty_int(bits, n[0] == 'i');
        return NULL;
    }
    if (n[0] == 'f') {
        int bits = atoi(n + 1);
        if (bits == 16 || bits == 32 || bits == 64) return ty_float(bits);
        return NULL;
    }
    if (!strcmp(n, "Allocator")) return sema_allocator_type();
    /* `@type(x)` 的两个描述符类型：源文件里没有声明，名字是编译器内建的
     * （和 Allocator 一样）。LSP 的行内提示会写 `: Type` / `: Field`。 */
    if (!strcmp(n, "Type"))  return sema_type_type();
    if (!strcmp(n, "Field")) return sema_type_field_type();
    return NULL;
}

enum { BI_NONE, BI_PRINT, BI_PRINTLN, BI_FORMAT, BI_HEAP, BI_FILE, BI_FILE_FN,
       BI_IO, BI_IO_FN, BI_IO_PRINT, BI_STACKALLOC, BI_HEAPALLOC, BI_HEAPSLICE,
       BI_FREE,
       BI_PROC, BI_PROC_FN, BI_DEBUG, BI_DEBUG_FN, BI_TASK, BI_TASK_FN,
       BI_SYNC, BI_SYNC_FN };

/* 后端分派裸名内建调用用：Symbol 的 builtin 编号不对外暴露，只给名字。
 * 用户自己写的同名函数 builtin == BI_NONE，返回 NULL，不会被改道。 */
const char *sema_sym_builtin_name(Symbol *s) {
    if (!s || s->kind != SK_NS) return NULL;
    switch (s->builtin) {
    case BI_PRINT:      return "print";
    case BI_PRINTLN:    return "println";
    case BI_FORMAT:     return "format";
    case BI_STACKALLOC: return "stackAlloc";
    case BI_HEAPALLOC:  return "heapAlloc";
    case BI_HEAPSLICE:  return "heapSlice";
    case BI_FREE:       return "free";
    default:            return NULL;
    }
}

/* ---------------- 内建类型 Allocator ----------------
 *
 * std.format 要把结果放回堆上，按 spec §13 的约定必须显式传分配器。
 * 分配器就是一个普通的结构体值（两个函数指针），所以用户完全可以自己造一个；
 * 类型身份靠一个合成节点，它只当"身份证"用，不进任何作用域。 */
Type *sema_allocator_type(void) {
    static Type *t;
    if (t) return t;
    static Node id;
    memset(&id, 0, sizeof id);
    id.kind = NK_STRUCT_TYPE;
    id.name = "Allocator";

    t = ty_agg(TY_STRUCT, &id, "Allocator");
    if (!t->fields) {
        /* 首字段 ctx：有状态分配器（arena / 池 / 计数器）的载体。
         * 没有它，arena 只能靠全局变量绕——v0.58 之前就是这么凑合的。 */
        Type *ap[2] = { ty_opt(ty_ptr(ty_u8(), false, false, 0)), ty_usize() };
        Type *fp[3] = { ty_opt(ty_ptr(ty_u8(), false, false, 0)),
                        ty_ptr(ty_u8(), false, false, 0), ty_usize() };
        t->nfields = 3;
        t->fields = calloc(3, sizeof(Field));
        t->fields[0].name = "ctx";
        t->fields[0].ty  = ty_opt(ty_ptr(ty_u8(), false, false, 0));
        t->fields[1].name = "alloc";
        t->fields[1].ty = ty_fn(ap, 2, ty_opt(ty_ptr(ty_u8(), false, false, 0)));
        t->fields[2].name = "free";
        t->fields[2].ty = ty_fn(fp, 3, ty_void());
    }
    return t;
}

/* ---------------- @type 的两种描述符 ----------------
 *
 * Type  = { name, size, align, ty, fieldCount, fields: []Field }
 * Field = { name, ty, offset }
 *
 * 只此一个内建（2026-11-10 用户拍板两次：先把 @sizeOf/@alignOf/@typeName
 * 并进来，再把 @typeInfo 也并进来——反射就剩 `@type(x)` 一个名字）。
 * 为什么要并：这三个答案编译器本来就知道，分成三个以 `@` 打头的关键字
 * 逼人背名字；`@type(T).size` 读起来就是"T 这个类型的大小"。
 *
 * 两个类型都只当"身份证"用，不进任何作用域；@type 返回的值全部是
 * 编译期值——Field.offset 让 @offsetOf 不再需要单独的内建，
 * Type.ty 让 @typeOf 不再需要单独的内建。
 * x 既可以是类型（@type(Point)、@type([]const u8)）也可以是表达式
 * （@type(p.x)），二合一走同一条路。 */
static Type *sema_type_field_type(void) {
    static Type *t;
    if (t) return t;
    static Node id;
    memset(&id, 0, sizeof id);
    id.kind = NK_STRUCT_TYPE;
    id.name = "Field";

    t = ty_agg(TY_STRUCT, &id, "Field");
    if (!t->fields) {
        /* 字段叫 `ty` 而不是 `type`：`type` 是关键字，当不了字段名。
         * `ptr`（§118）：字段值指针，只在 @type(左值) 的描述符上有值——
         * 类型上的占位没人读（check_field / ct_field 都对它特判）。 */
        t->nfields = 4;
        t->fields = calloc(4, sizeof(Field));
        t->fields[0].name = "name";   t->fields[0].ty = ty_slice(ty_u8(), true);
        t->fields[1].name = "ty";     t->fields[1].ty = ty_type();
        t->fields[2].name = "offset"; t->fields[2].ty = ty_usize();
        t->fields[3].name = "ptr";    t->fields[3].ty = ty_ptr(ty_u8(), true, false, 0);
    }
    return t;
}

static Type *sema_type_type(void) {
    static Type *t;
    if (t) return t;
    static Node id;
    memset(&id, 0, sizeof id);
    id.kind = NK_STRUCT_TYPE;
    id.name = "Type";

    t = ty_agg(TY_STRUCT, &id, "Type");
    if (!t->fields) {
        t->nfields = 7;
        t->fields = calloc(7, sizeof(Field));
        t->fields[0].name = "name";       t->fields[0].ty = ty_slice(ty_u8(), true);
        t->fields[1].name = "size";       t->fields[1].ty = ty_usize();
        t->fields[2].name = "align";      t->fields[2].ty = ty_usize();
        t->fields[3].name = "ty";         t->fields[3].ty = ty_type();
        t->fields[4].name = "fieldCount"; t->fields[4].ty = ty_usize();
        t->fields[5].name = "fields";     t->fields[5].ty = ty_slice(sema_type_field_type(), true);
        t->fields[6].name = "statics";    t->fields[6].ty = ty_slice(sema_type_field_type(), true);
    }
    return t;
}

/* Type / Field 只活在编译期：任何想把它们变成运行时存储的写法
 * （var/let 声明、函数形参）都在这里报错。const 绑定不受影响——那正是
 * 它的用法。（ast.h 里的声明保留：后端将来物化声明时也用它跳过。） */
bool sema_ty_ct_only(Type *t) {
    if (!t) return false;
    if (t == sema_type_type() || t == sema_type_field_type()) return true;
    if (t->kind == TY_TAG) return true;          /* §172：标签句柄只活在编译期 */
    if (t->kind == TY_SLICE || t->kind == TY_PTR || t->kind == TY_OPT)
        return sema_ty_ct_only(t->elem);
    return false;
}

/* @type(x) 的值：6 个字段全是编译期值。fields 是"切片视图"：
 * V_PTR 指向一块 V_AGG 数组值，nitems 记长度——`.len` / 下标 / 切片
 * 在 ct_eval 里都走 ct_view_len / ct_index 那条路。
 * fields 只装数据字段（关联常量/嵌套类型这些静态成员不是实例布局，
 * offset 对它们没有意义）。 */
static Value *type_desc_value(Type *t, Node *inst) {
    Value *v = val_agg(sema_type_type(), 7);
    int sz = ty_size(t);
    int al = ty_align(t);
    v->items[0] = val_str(ty_str(t), (int)strlen(ty_str(t)));
    v->items[1] = val_int(ty_usize(), (uint64_t)(sz < 0 ? 0 : sz), false);
    v->items[2] = val_int(ty_usize(), (uint64_t)(al < 0 ? 1 : al), false);
    v->items[3] = val_type(t);
    bool has_fields = t->kind == TY_STRUCT || t->kind == TY_UNION;
    int nf = 0;
    if (has_fields)
        for (int i = 0; i < t->nfields; i++)
            if (!t->fields[i].is_static) nf++;
    v->items[4] = val_int(ty_usize(), (uint64_t)nf, false);

    Type *fit = sema_type_field_type();
    Value *arr = val_agg(ty_slice(fit, true), nf);
    int k = 0;
    if (has_fields)
        for (int i = 0; i < t->nfields; i++) {
            if (t->fields[i].is_static) continue;
            Value *fv = val_agg(fit, 4);
            fv->items[0] = val_str(t->fields[i].name, (int)strlen(t->fields[i].name));
            fv->items[1] = val_type(t->fields[i].ty);
            fv->items[2] = val_int(ty_usize(), (uint64_t)t->fields[i].offset, false);
            /* §118 字段值指针（用户 2026-11-11 拍板"使用指针取值"）：inst 是
             * 有存储的左值（@type(v) / @type(p.inner)）时，ptr = 基址 + offset，
             * 编译期两头都已知。类型 *(f.ty)；只读性跟基址走（const 实例的
             * 字段不可写）。运行期地址折不成编译期常量——ct 折值在 ct_field
             * 里特判返回 NULL，发码时后端现拼。 */
            fv->items[3] = NULL;
            if (inst) {
                Value *pv = val_undef(ty_ptr(t->fields[i].ty,
                                             !lvalue_mutable(inst), false, 0));
                pv->xexpr = clone_tree(inst);
                pv->off = (uint64_t)t->fields[i].offset;
                fv->items[3] = pv;
            }
            arr->items[k++] = fv;
        }
    Value *sv = val_ptr(ty_slice(fit, true), arr, 0);
    sv->nitems = nf;
    v->items[5] = sv;

    /* statics（§118 续，用户 2026-11-11："@type(T) 还应该有静态成员的切片"；
     * 二次拍板：ptr 两种形态都有——它不依赖实例）。范围：struct/union 的
     * is_static 非 类型 成员在 fields 里，enum 的在 statics 里（ty.h：
     * "结构体的在 fields 里（is_static），枚举的在 statics 里"——上一版
     * 只扫 fields，枚举体的 const 成员漏收）。每个元素都带 ptr：后端把
     * 静态成员全部物化成 C 全局（var/const 都有存储），所以 ptr 就是
     * &全局 的真指针——const 成员 *const T，var 成员 *T。 */
    int ns = 0;
    if (t->kind == TY_STRUCT || t->kind == TY_UNION)
        for (int i = 0; i < t->nfields; i++)
            if (t->fields[i].is_static &&
                !(t->fields[i].ty && t->fields[i].ty->kind == TY_TYPE)) ns++;
    if (t->kind == TY_ENUM)
        for (int i = 0; i < t->nstatics; i++)
            if (!(t->statics[i].ty && t->statics[i].ty->kind == TY_TYPE)) ns++;
    Value *sarr = val_agg(ty_slice(fit, true), ns);
    int sk = 0;
    if (t->kind == TY_STRUCT || t->kind == TY_UNION)
        for (int i = 0; i < t->nfields; i++) {
            if (t->fields[i].is_static &&
                !(t->fields[i].ty && t->fields[i].ty->kind == TY_TYPE)) {
                Value *fv = val_agg(fit, 4);
                fv->items[0] = val_str(t->fields[i].name, (int)strlen(t->fields[i].name));
                fv->items[1] = val_type(t->fields[i].ty);
                fv->items[2] = val_int(ty_usize(), 0, false);
                Value *pv = val_undef(ty_ptr(t->fields[i].ty,
                                             !t->fields[i].is_var, false, 0));
                pv->xt = t;
                pv->off = (uint64_t)i;
                fv->items[3] = pv;
                sarr->items[sk++] = fv;
            }
        }
    if (t->kind == TY_ENUM)
        for (int i = 0; i < t->nstatics; i++) {
            if (t->statics[i].ty && t->statics[i].ty->kind == TY_TYPE) continue;
            Value *fv = val_agg(fit, 4);
            fv->items[0] = val_str(t->statics[i].name, (int)strlen(t->statics[i].name));
            fv->items[1] = val_type(t->statics[i].ty);
            fv->items[2] = val_int(ty_usize(), 0, false);
            Value *pv = val_undef(ty_ptr(t->statics[i].ty,
                                         !t->statics[i].is_var, false, 0));
            pv->xt = t;
            pv->off = (uint64_t)i;
            fv->items[3] = pv;
            sarr->items[sk++] = fv;
        }
    Value *ssv = val_ptr(ty_slice(fit, true), sarr, 0);
    ssv->nitems = ns;
    v->items[6] = ssv;
    return v;
}

/* std 下由编译器提供的名字。它们都挂在 std 模块的作用域里，于是
 * `std.println` / `std.file.read` 走的是同一条"模块成员"查找路径。 */
static Symbol *std_builtin(const char *name) {
    static Symbol print_s, println_s, format_s, heap_s, page_s, file_s, alloc_s;
    static Symbol type_s, field_s;
    static Symbol salloc_s, halloc_s, hslice_s;
    static Symbol free_s;
    static Symbol f_read, f_write, f_append, f_exists, f_remove;
    static Symbol io_s, i_openr, i_openw, i_opena, i_read, i_write;
    static Symbol i_seek, i_tell, i_close, i_flush, i_stdin, i_stdout, i_stderr;
    static Symbol i_print, i_println;
    static Symbol proc_s, p_args, p_getenv, p_setenv, p_exit;
    static Symbol debug_s, d_panic, d_assert, d_errname, d_trace;
    static Symbol atomic_s, a_add, a_sub, a_load, a_store, a_swap, a_cas;
    static bool init;
    if (!name) return NULL;
    if (!init) {
        init = true;
        print_s.name = "print";     print_s.kind = SK_NS; print_s.builtin = BI_PRINT;
        println_s.name = "println"; println_s.kind = SK_NS; println_s.builtin = BI_PRINTLN;
        format_s.name = "format";   format_s.kind = SK_NS; format_s.builtin = BI_FORMAT;
        print_s.state = println_s.state = format_s.state = 2;

        heap_s.name = "heap"; heap_s.kind = SK_NS; heap_s.builtin = BI_HEAP;
        heap_s.state = 2; heap_s.ty = ty_type(); heap_s.type_val = ty_void();

        /* 分配 API（decisions §44 + §144）：
         *   stackAlloc(T, N)    -> *[N]T   栈 alloca，N 编译期
         *   heapAlloc(a, T, N)  -> *[N]T   堆，N 编译期，可进 §130 合并
         *   heapSlice(a, T, n)  -> []T     堆，n 运行期，长度装进切片值
         * 前两个的长度写进类型（界检查 / .len），第三个是 {ptr,len} 16B。
         * 堆分配 OOM 一律 abort，所以三个都不返回 optional。 */
        salloc_s.name = "stackAlloc"; salloc_s.kind = SK_NS;
        salloc_s.builtin = BI_STACKALLOC; salloc_s.state = 2;
        halloc_s.name = "heapAlloc"; halloc_s.kind = SK_NS;
        halloc_s.builtin = BI_HEAPALLOC; halloc_s.state = 2;
        hslice_s.name = "heapSlice"; hslice_s.kind = SK_NS;
        hslice_s.builtin = BI_HEAPSLICE; hslice_s.state = 2;
        /* §177：std.free(a, s) —— 释放的对称面，改写成 a.deinit(s) */
        free_s.name = "free"; free_s.kind = SK_NS;
        free_s.builtin = BI_FREE; free_s.state = 2;

        file_s.name = "file"; file_s.kind = SK_NS; file_s.builtin = BI_FILE;
        file_s.state = 2; file_s.ty = ty_type(); file_s.type_val = ty_void();

        /* 唯一的现成分配器：直接向系统要页（C 里就是 malloc/free） */
        page_s.name = "page";
        page_s.kind = SK_CONST;
        page_s.state = 2;
        page_s.cname = "zx_alloc_page";
        page_s.ty = sema_allocator_type();
        page_s.ct = val_agg(page_s.ty, 2);
        page_s.ct->items[0] = val_fn(page_s.ty->fields[0].ty, NULL);
        page_s.ct->items[1] = val_fn(page_s.ty->fields[1].ty, NULL);

        /* `Allocator` 这个名字也能在 std.zx 里直接用 */
        alloc_s.name = "Allocator";
        alloc_s.kind = SK_CONST;
        alloc_s.state = 2;
        alloc_s.ty = ty_type();
        alloc_s.type_val = sema_allocator_type();

        /* @type 用到的两个描述符类型也挂在 std 上（`std.Type` / `std.Field`），
         * 错误信息里点得到名字；它们 ct-only，开不了运行时变量。
         * 用户 2026-11-11：LSP 的行内提示写 `const info: Type = @type(T);`，
         * 照着敲进来却是 E0401 unknown type 'Type'——名字必须真能写出。 */
        type_s.name = "Type";
        type_s.kind = SK_CONST;
        type_s.state = 2;
        type_s.ty = ty_type();
        type_s.type_val = sema_type_type();
        field_s.name = "Field";
        field_s.kind = SK_CONST;
        field_s.state = 2;
        field_s.ty = ty_type();
        field_s.type_val = sema_type_field_type();
        /* std.file：普通函数调用，C 那边是编译器生成的运行时函数 */
        Type *s8 = ty_slice(ty_u8(), true);
        Type *p1[1] = { s8 };
        Type *p2[2] = { s8, s8 };
        Type *pa[2] = { sema_allocator_type(), s8 };
        f_read.name   = "read";   f_read.cname   = "zx_file_read";
        f_read.ty     = ty_fn(pa, 2, ty_opt(s8));
        f_write.name  = "write";  f_write.cname  = "zx_file_write";
        f_write.ty    = ty_fn(p2, 2, ty_bool());
        f_append.name = "append"; f_append.cname = "zx_file_append";
        f_append.ty   = ty_fn(p2, 2, ty_bool());
        f_exists.name = "exists"; f_exists.cname = "zx_file_exists";
        f_exists.ty   = ty_fn(p1, 1, ty_bool());
        f_remove.name = "remove"; f_remove.cname = "zx_file_remove";
        f_remove.ty   = ty_fn(p1, 1, ty_bool());
        Symbol *fs[5] = { &f_read, &f_write, &f_append, &f_exists, &f_remove };
        for (int i = 0; i < 5; i++) {
            fs[i]->kind = SK_CONST;
            fs[i]->state = 2;
            fs[i]->builtin = BI_FILE_FN;
        }

        /* std.io：带句柄的流。std.file 是"整块读/整块写"，这里是"打开一次、
         * 分次读写"，缓冲层在 std.zx 里用普通 Zx 代码搭（见 BufReader）。 */
        Type *s8c  = ty_slice(ty_u8(), true);
        Type *s8m  = ty_slice(ty_u8(), false);
        Type *usz  = ty_usize();
        Type *ousz = ty_opt(usz);
        Type *p_oo[1] = { s8c };                 /* open*(path) ?usize */
        Type *p_rb[2] = { usz, s8m };            /* read(h, buf) usize */
        Type *p_wb[2] = { usz, s8c };            /* write(h, data) usize */
        Type *p_sf[2] = { usz, usz };            /* seek(h, off) bool */
        Type *p_h1[1] = { usz };                 /* tell / close / flush */

        io_s.name = "io"; io_s.kind = SK_NS; io_s.builtin = BI_IO;
        io_s.state = 2; io_s.ty = ty_type(); io_s.type_val = ty_void();

        i_openr.name = "openRead";   i_openr.cname = "zx_io_openr";
        i_openr.ty   = ty_fn(p_oo, 1, ousz);
        i_openw.name = "openWrite";  i_openw.cname = "zx_io_openw";
        i_openw.ty   = ty_fn(p_oo, 1, ousz);
        i_opena.name = "openAppend"; i_opena.cname = "zx_io_opena";
        i_opena.ty   = ty_fn(p_oo, 1, ousz);
        i_read.name  = "read";       i_read.cname  = "zx_io_read";
        i_read.ty    = ty_fn(p_rb, 2, usz);
        i_write.name = "write";      i_write.cname = "zx_io_write";
        i_write.ty   = ty_fn(p_wb, 2, usz);
        i_seek.name  = "seek";       i_seek.cname  = "zx_io_seek";
        i_seek.ty    = ty_fn(p_sf, 2, ty_bool());
        i_tell.name  = "tell";       i_tell.cname  = "zx_io_tell";
        i_tell.ty    = ty_fn(p_h1, 1, usz);
        i_close.name = "close";      i_close.cname = "zx_io_close";
        i_close.ty   = ty_fn(p_h1, 1, ty_void());
        i_flush.name = "flush";      i_flush.cname = "zx_io_flush";
        i_flush.ty   = ty_fn(p_h1, 1, ty_void());
        i_stdin.name  = "stdin";     i_stdin.cname  = "zx_io_stdin";
        i_stdin.ty    = ty_fn(NULL, 0, usz);
        i_stdout.name = "stdout";    i_stdout.cname = "zx_io_stdout";
        i_stdout.ty   = ty_fn(NULL, 0, usz);
        i_stderr.name = "stderr";    i_stderr.cname = "zx_io_stderr";
        i_stderr.ty   = ty_fn(NULL, 0, usz);

        Symbol *is[12] = { &i_openr, &i_openw, &i_opena, &i_read, &i_write,
                           &i_seek, &i_tell, &i_close, &i_flush,
                           &i_stdin, &i_stdout, &i_stderr };
        for (int i = 0; i < 12; i++) {
            is[i]->kind = SK_CONST;
            is[i]->state = 2;
            is[i]->builtin = BI_IO_FN;
        }

        /* std.io.print / std.io.println：往句柄上格式化。变参，走不了
         * 普通函数类型——调用点在 check_call 特判（按 builtin 识别），
         * 后端按 cname 识别（zx_io_print / zx_io_println）。 */
        i_print.name = "print";   i_print.cname = "zx_io_print";
        i_println.name = "println"; i_println.cname = "zx_io_println";
        i_print.builtin = i_println.builtin = BI_IO_PRINT;
        i_print.kind = i_println.kind = SK_CONST;
        i_print.state = i_println.state = 2;

        /* std.process：命令行 / 环境 / 退出。全部是普通函数调用，
         * C 那边是编译器生成的运行时函数（见后端 emit_proc_runtime）。 */
        Type *ss8 = ty_slice(s8c, false);        /* [][]const u8 */
        proc_s.name = "process"; proc_s.kind = SK_NS; proc_s.builtin = BI_PROC;
        proc_s.state = 2; proc_s.ty = ty_type(); proc_s.type_val = ty_void();

        p_args.name = "args";   p_args.cname = "zx_proc_args";
        p_args.ty   = ty_fn(NULL, 0, ss8);
        p_getenv.name = "getEnv"; p_getenv.cname = "zx_proc_getenv";
        p_getenv.ty   = ty_fn(&s8c, 1, ty_opt(s8c));
        p_setenv.name = "setEnv"; p_setenv.cname = "zx_proc_setenv";
        Type *p_se[2] = { s8c, s8c };
        p_setenv.ty   = ty_fn(p_se, 2, ty_bool());
        p_exit.name = "exit";   p_exit.cname = "zx_proc_exit";
        p_exit.ty   = ty_fn((Type *[]){ ty_u8() }, 1, ty_noreturn());

        Symbol *ps[4] = { &p_args, &p_getenv, &p_setenv, &p_exit };
        for (int i = 0; i < 4; i++) {
            ps[i]->kind = SK_CONST;
            ps[i]->state = 2;
            ps[i]->builtin = BI_PROC_FN;
        }

        /* std.debug：调试内建（decisions §85）。panic / assert 的位置实参
         * （file/line/col/fn）由编译器在调用点自动注入——变参走不了普通
         * 函数类型，check_call / 后端按 builtin 特判，同 std.io.print 模式。
         * errName / printTrace 是普通函数，走通用调用路径。 */
        debug_s.name = "debug"; debug_s.kind = SK_NS; debug_s.builtin = BI_DEBUG;
        debug_s.state = 2; debug_s.ty = ty_type(); debug_s.type_val = ty_void();

        d_panic.name = "panic";   d_panic.cname = "zx_debug_panic";
        d_assert.name = "assert"; d_assert.cname = "zx_debug_assert";
        d_panic.builtin = d_assert.builtin = BI_DEBUG_FN;
        d_panic.kind = d_assert.kind = SK_CONST;
        d_panic.state = d_assert.state = 2;

        /* errName 的实参就是错误值本身（运行时 u16）：捕到的 err 直接传进来，
         * 不用先取 .tag。码 → 名字的静态表由后端按编译期登记的错误集生成。 */
        d_errname.name = "errName"; d_errname.cname = "zx_debug_errname";
        d_errname.ty = ty_fn((Type *[]){ ty_error() }, 1, ty_slice(ty_u8(), true));
        d_trace.name = "printTrace"; d_trace.cname = "zx_debug_printtrace";
        d_trace.ty = ty_fn(NULL, 0, ty_void());
        Symbol *ds[2] = { &d_errname, &d_trace };
        for (int i = 0; i < 2; i++) {
            ds[i]->kind = SK_CONST;
            ds[i]->state = 2;
            ds[i]->builtin = BI_DEBUG_FN;
        }

                /* M58：唯一的并发抽象是 std/task.zx 里的 Task 实例（物理隔离、
         * 各管一组线程，通信也挂在实例上）。这里只保留与执行模型无关的
         * std.atomic——它是纯函数，不是并发类型。 */
        /* std.atomic：与执行模型无关的纯函数（SEQ_CST）。M58 起唯一的并发
         * 抽象是 std/task.zx 的 Task 实例，mutex/cond/thread 类型已收掉，
         * 这里只留 atomic。 */
        atomic_s.name = "atomic"; atomic_s.kind = SK_NS; atomic_s.builtin = BI_SYNC;
        atomic_s.state = 2; atomic_s.ty = ty_type(); atomic_s.type_val = ty_void();
        Type *pu64 = ty_ptr(ty_int(64, false), false, false, 0);
        Type *p_aa[2] = { pu64, usz };
        a_add.name = "atomicAdd";   a_add.cname = "zx_atomic_add";
        a_add.ty  = ty_fn(p_aa, 2, usz);
        a_sub.name = "atomicSub";   a_sub.cname = "zx_atomic_sub";
        a_sub.ty  = ty_fn(p_aa, 2, usz);
        a_load.name = "atomicLoad"; a_load.cname = "zx_atomic_load";
        a_load.ty = ty_fn(&pu64, 1, usz);
        a_store.name = "atomicStore"; a_store.cname = "zx_atomic_store";
        a_store.ty = ty_fn(p_aa, 2, ty_void());
        a_swap.name = "atomicSwap"; a_swap.cname = "zx_atomic_swap";
        a_swap.ty = ty_fn(p_aa, 2, usz);
        Type *p_cas[3] = { pu64, usz, usz };
        a_cas.name = "atomicCAS";   a_cas.cname = "zx_atomic_cas";
        a_cas.ty  = ty_fn(p_cas, 3, ty_bool());

        Symbol *ss[6] = { &a_add, &a_sub, &a_load, &a_store, &a_swap, &a_cas };
        for (int i = 0; i < 6; i++) {
            ss[i]->kind = SK_CONST;
            ss[i]->state = 2;
            ss[i]->builtin = BI_SYNC_FN;
        }

    }

    if (!strcmp(name, "print"))     return &print_s;
    if (!strcmp(name, "println"))   return &println_s;
    if (!strcmp(name, "format"))    return &format_s;
    if (!strcmp(name, "heap"))      return &heap_s;
    if (!strcmp(name, "heap.page")) return &page_s;
    if (!strcmp(name, "file"))      return &file_s;
    if (!strcmp(name, "io"))        return &io_s;
    if (!strcmp(name, "process"))   return &proc_s;
    if (!strcmp(name, "io.read"))    return &i_read;
    if (!strcmp(name, "io.write"))   return &i_write;
    if (!strcmp(name, "Allocator")) return &alloc_s;
    if (!strcmp(name, "Type"))      return &type_s;
    if (!strcmp(name, "Field"))     return &field_s;

    if (!strcmp(name, "stackAlloc")) return &salloc_s;
    if (!strcmp(name, "heapAlloc"))  return &halloc_s;
    if (!strcmp(name, "heapSlice"))  return &hslice_s;
    if (!strcmp(name, "free"))       return &free_s;
    if (!strcmp(name, "read"))      return &f_read;
    if (!strcmp(name, "write"))     return &f_write;
    if (!strcmp(name, "append"))    return &f_append;
    if (!strcmp(name, "exists"))    return &f_exists;
    if (!strcmp(name, "remove"))    return &f_remove;
    if (!strcmp(name, "io.openRead"))   return &i_openr;
    if (!strcmp(name, "io.openWrite"))  return &i_openw;
    if (!strcmp(name, "io.openAppend")) return &i_opena;
    if (!strcmp(name, "io.seek"))       return &i_seek;
    if (!strcmp(name, "io.tell"))       return &i_tell;
    if (!strcmp(name, "io.close"))      return &i_close;
    if (!strcmp(name, "io.flush"))      return &i_flush;
    if (!strcmp(name, "io.stdin"))      return &i_stdin;
    if (!strcmp(name, "io.stdout"))     return &i_stdout;
    if (!strcmp(name, "io.stderr"))     return &i_stderr;
    if (!strcmp(name, "io.print"))      return &i_print;
    if (!strcmp(name, "io.println"))    return &i_println;
    if (!strcmp(name, "process.args"))   return &p_args;
    if (!strcmp(name, "process.getEnv")) return &p_getenv;
    if (!strcmp(name, "process.setEnv")) return &p_setenv;
    if (!strcmp(name, "process.exit"))   return &p_exit;
    if (!strcmp(name, "debug"))           return &debug_s;
    if (!strcmp(name, "debug.panic"))     return &d_panic;
    if (!strcmp(name, "debug.assert"))    return &d_assert;
    if (!strcmp(name, "debug.errName"))   return &d_errname;
    if (!strcmp(name, "debug.printTrace")) return &d_trace;
    if (!strcmp(name, "atomic"))            return &atomic_s;
    if (!strcmp(name, "atomic.atomicAdd"))   return &a_add;
    if (!strcmp(name, "atomic.atomicSub"))   return &a_sub;
    if (!strcmp(name, "atomic.atomicLoad"))  return &a_load;
    if (!strcmp(name, "atomic.atomicStore")) return &a_store;
    if (!strcmp(name, "atomic.atomicSwap"))  return &a_swap;
    if (!strcmp(name, "atomic.atomicCAS"))   return &a_cas;
    return NULL;
}

/* 挂进 std 模块作用域的那些名字（std.zx 自己的声明另外由 hoist 加进来） */
static const char *g_std_seeded[] = {
    "print", "println", "format", "heap", "file", "io", "Allocator",
    "Type", "Field", "stackAlloc", "heapAlloc", "heapSlice", "free", "process",
    "debug", "atomic", NULL
};

static Symbol *ns_member(Symbol *ns, const char *name, int line, int col) {
    /* 模块本身没加载成功（打不开 / 解析失败）：那条错已经报在 @mod(...)
     * 调用点上了，这里再报 N 条 "has no member" 只会盖住真正的原因。
     * 尤其不能报 "module 'main' has no member"——'main' 是当前文件，
     * 等于告诉用户去一个根本没有该名字的模块里找。 */
    if (ns->broken) return NULL;
    if (ns->builtin == BI_HEAP) {
        if (!strcmp(name, "page")) return std_builtin("heap.page");
        {
            const char *sug = suggest_std_member("std.heap", name);
            if (sug) diag_help("did you mean `%s`?", sug);
        }
        err_at(line, col, "'std.heap' has no member '%s'", name);
        return NULL;
    }
    if (ns->builtin == BI_FILE) {
        Symbol *f = std_builtin(name);
        if (f && f->builtin) return f;
        {
            const char *sug = suggest_std_member("std.file", name);
            if (sug) diag_help("did you mean `%s`?", sug);
        }
        err_at(line, col, "'std.file' has no member '%s'", name);
        return NULL;
    }
    if (ns->builtin == BI_IO) {
        /* 和 std.file 有同名成员（read / write），所以按 "io.名字" 查。
         * print / println 是变参格式化（BI_IO_PRINT），其余是普通函数。 */
        char key[64];
        snprintf(key, sizeof key, "io.%s", name);
        Symbol *f = std_builtin(key);
        if (f && (f->builtin == BI_IO_FN || f->builtin == BI_IO_PRINT)) return f;
        {
            const char *sug = suggest_std_member("std.io", name);
            if (sug) diag_help("did you mean `%s`?", sug);
        }
        err_at(line, col, "'std.io' has no member '%s'", name);
        return NULL;
    }
    if (ns->builtin == BI_PROC) {
        char key[64];
        snprintf(key, sizeof key, "process.%s", name);
        Symbol *f = std_builtin(key);
        if (f && f->builtin == BI_PROC_FN) return f;
        {
            const char *sug = suggest_std_member("std.process", name);
            if (sug) diag_help("did you mean `%s`?", sug);
        }
        err_at(line, col, "'std.process' has no member '%s'", name);
        return NULL;
    }
    if (ns->builtin == BI_DEBUG) {
        char key[64];
        snprintf(key, sizeof key, "debug.%s", name);
        Symbol *f = std_builtin(key);
        if (f && f->builtin == BI_DEBUG_FN) return f;
        {
            const char *sug = suggest_std_member("std.debug", name);
            if (sug) diag_help("did you mean `%s`?", sug);
        }
        err_at(line, col, "'std.debug' has no member '%s'", name);
        return NULL;
    }
    if (ns->builtin == BI_SYNC) {
        char key[64];
        snprintf(key, sizeof key, "%s.%s", ns->name, name);
        Symbol *f = std_builtin(key);
        if (f && f->builtin == BI_SYNC_FN) return f;
        {
            char full[72];
            snprintf(full, sizeof full, "std.%s", ns->name);
            const char *sug = suggest_std_member(full, name);
            if (sug) diag_help("did you mean `%s`?", sug);
        }
        err_at(line, col, "'std.%s' has no member '%s'", ns->name, name);
        return NULL;
    }
    /* 文件模块：只认带 pub 的顶层声明 */
    if (ns->mod && ns->mod->root) {
        /* §206：循环导入已放开，loading 兜底不再需要。成员查找走下面的
         * 正常路径：hoist 在 check_stmts 之前完成，顶层的 pub 符号总已挂好；
         * 若目标符号的**类型**还形成环，ensure_raw 的 E0601 会精确报在
         * 那条 const/var 的初始化器上，不再是含糊的 "has no member"。 */
        for (Symbol *m = ns->mod->scope.first; m; m = m->next) {
            if (!m->name || strcmp(m->name, name)) continue;
            if (!m->is_pub) {
                if (m->decl_line && m->mod && m->mod->path)
                    diag_note(m->mod->path, m->decl_line, m->decl_col,
                              "'%s' declared here", name);
                err_at(line, col,
                       "'%s' is private to module '%s'; mark it `pub` to export it",
                       name, ns->mod->stem);
                return NULL;
            }
            return m;
        }
        {
            /* 候选就是那个模块的顶层符号——pub 与否都算，建议不给出去
             * 泄露私有名，只是帮人拼对名字。 */
            const char *best = NULL;
            int bd = 999;
            for (Symbol *m = ns->mod->scope.first; m; m = m->next) {
                if (!name_close(name, m->name)) continue;
                int d = edit_distance(name, m->name);
                if (d < bd) { bd = d; best = m->name; }
            }
            if (best) diag_help("did you mean `%s`?", best);
        }
        /* 绑定名在前：源码里写的是 `s2.nosuch`，报 "module 'std'" 会让人
         * 去找一个叫 std 的绑定——而它可能叫 s2、m、util……
         * 名字一样时不重复说一遍（`std` (module 'std') 只是噪音）。 */
        if (ns->name && ns->mod->stem && strcmp(ns->name, ns->mod->stem) != 0)
            err_at(line, col, "`%s` (module '%s') has no member '%s'",
                   ns->name, ns->mod->stem, name);
        else
            err_at(line, col, "module '%s' has no member '%s'",
                   ns->mod->stem ? ns->mod->stem : "?", name);
        return NULL;
    }
    if (ns->is_std) {
        const char *sug = suggest_std_member("std", name);
        if (sug) diag_help("did you mean `%s`?", sug);
    }
    err_at(line, col, "'%s' has no member '%s'", ns->name ? ns->name : "?", name);
    return NULL;
}

/* ---------------- 编译期整数求值 ---------------- */

typedef struct { bool ok; bool neg; uint64_t v; } IntV;

static IntV ev_int(Node *n);
static Value *ct_field(Node *n);
static void ensure(Symbol *s);
static bool decl_is_lazy_module(Node *d);
static Module *module_load(const char *rel, int line, int col);
static Module *module_load_full(const char *full, int line, int col);
static Module *module_load_std(int line, int col);
static void sym_copy(Symbol *dst, Symbol *src, const char *name);

/* 把编译器内建名字并进一个模块作用域。
 * 模块自己声明了同名就以它为准 (scope_add 返回 NULL)，
 * 用户同名函数不会被内建劫持。
 * 入口单元也调这一份 (sema_check)：§158 前 LSP 把编辑器
 * 缓冲写成同目录影子文件单查，而 std/ 里的库文件
 * 在真实构建里是 @mod 模块，裸名 stackAlloc / file 都可用。
 * 入口/模块作用域不对称会让编辑器假报 E0401。 */
static void seed_std_builtins(Scope *sc) {
    for (int i = 0; g_std_seeded[i]; i++) {
        Symbol *b = std_builtin(g_std_seeded[i]);
        if (!b) continue;
        Symbol *s = scope_add(sc, b->name, b->kind);
        if (!s) continue;
        sym_copy(s, b, b->name);
        s->is_pub = true;
    }
}
static Symbol *module_member(Module *m, const char *name);

static IntV intv(uint64_t v) { IntV r = { true, false, v }; return r; }
static IntV noint(void)      { IntV r = { false, false, 0 }; return r; }

static IntV ev_int(Node *n) {
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

static Type *scalar_default(Type *t) {
    if (!t) return t;
    if (t->kind == TY_COMPTIME_INT)   return ty_i32();
    if (t->kind == TY_COMPTIME_FLOAT) return ty_f64();
    return t;
}

static bool numeric_like(Type *t) {
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

static void lit_digits(Node *n, char *out, int cap) {
    int j = 0;
    if (n->str)
        for (const char *s = n->str; *s && j < cap - 1; s++)
            if (*s != '_') out[j++] = *s;
    out[j] = 0;
}

/* 纯整数字面量（含一元负号）的值。返回 false 表示不是纯字面量。 */
static bool lit_value(Node *e, uint64_t *v, bool *neg, const char **text) {
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
static bool g_neg_lit_ctx;

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
static bool lit_fits_expr(Node *e, Type *t) {
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

static bool can_coerce(Type *from, Type *to) {
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

/* ---------------- §198 泛型体内类型声明的身份锚定 ----------------
 * 泛型 `fn (T: type) type` 体内写的局部类型声明（如
 *   `const Fixed = struct { x: i32 };`）
 * 若其定义**不引用任何泛型参数**，它其实是一个具体类型，跟实例无关。
 * 但 clone_tree 深拷贝整个函数体，每个实例都拿到一份新的 NK_STRUCT_TYPE
 * 节点，ty_agg 按 decl 节点 intern，于是 Body(i32).Fixed 与 Body(u8).Fixed
 * 变成两个同构却不同的类型，后端发给 C 就是两个 ZxS typedef（B1001）。
 *
 * 修法：实例化时同步遍历模板与克隆树。对不引用泛型参数的类型声明节点，
 * 登记「克隆节点 -> 模板节点」；ty_agg 经 ident_decl() 取模板节点作身份，
 * 于是跨实例共享同一个 decl。引用了泛型参数的声明（含间接引用）不登记，
 * 保持每实例各自的类型——这正是泛型该有的语义。 */
typedef struct { Node *clone, *tmpl; } IdentAnchor;
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
static Node *ident_decl(Node *n) {
    if (!n) return n;
    for (int i = 0; i < g_nanchors; i++)
        if (g_anchors[i].clone == n) return g_anchors[i].tmpl;
    return n;
}

/* 当前正在实例化的泛型参数名（anchor_types 期间有效）。 */
static const char **g_ident_params;
static int g_nident_params;

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
static void anchor_types(Node *tmpl, Node *clone) {
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

static Type *resolve_type(Node *n) {
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
        Type **items = calloc((size_t)(n->nitems ? n->nitems : 1), sizeof(Type *));
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

/* §151：`Self` 接收者的按值上限。不大于它的按值复制，大于它的按
 * `const Self *` 传地址（只读、零拷贝）。16 = 两个寄存器能装下的聚合
 * 上限，与 C 的大结构体传参习惯一致。0 大小类型落在按值档——后端给
 * 空聚合补 1 字节占位保证 C 合法，不退化成关联函数。 */
#define ZX_SELF_VALUE_MAX 16

/* 方法表：函数体里的第一个参数就是接收者。表先填满签名再查体，
 * 这样方法之间互相调用（self.other()）不用关心谁先谁后。
 * 结构体 / 联合体 / 枚举共用这一份——方法本来就是"第一个参数是接收者的函数"。 */
static void collect_methods(Node *n, Type *agg, const char *what) {
    int nmeth = 0;
    for (int i = 0; i < n->nitems; i++)
        if (n->items[i]->kind == NK_FN) nmeth++;
    if (!nmeth) return;

    agg->methods = calloc((size_t)nmeth, sizeof(Method));
    for (int i = 0; i < n->nitems; i++) {
        Node *it = n->items[i];
        if (it->kind != NK_FN) continue;
        Type *ft = resolve_fn_type(it);
        if (!ft || ft->kind != TY_FN) continue;

        /* 第一参数叫 `self` 就是方法（只认 `Self` / `*Self`；`*const Self`
         * 是 §151 合并前的写法，按 `Self` 归一化）；不叫 self 就是挂在
         * 类型上的普通函数（`Ints.init(alloc)`）。 */
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
        Type *base = (r0->kind == TY_PTR && !r0->has_len && !ty_is_array_ptr(r0))
                   ? r0->elem : r0;
        if (base != agg && base->kind != TY_INVALID && r0->kind != TY_INVALID && !mrecv_dup(it))
            err_at(it->line, it->col,
                   "method '%s' must take the %s as its first parameter "
                   "(`Self` or `*Self`), found `%s`",
                   it->name ? it->name : "?", what, ty_str(r0));

        /* §151：接收者只有两种语义形态。
         *   `Self`  —— 不可变；编译器按 `ty_size(Self)` 选按值 / const 指针；
         *   `*Self` —— 可变，改的就是调用者本身。
         * 合并前的 `*const Self` 当成 `Self` 归一化。判定只落在函数类型上：
         * 形参节点 ty 恒为 `Self`，所以方法体里 self 始终是值语义；后端在
         * 发射 self 标识符时按 `sema_sym_self_by_ptr` 补 `(*self)`。 */
        bool mutable_self = r0->kind == TY_PTR && !r0->has_len &&
                            !ty_is_array_ptr(r0) && !r0->is_const;
        if (!mutable_self) {
            int z = ty_size(agg);
            if (z < 0) z = 0;
            ft->params[0] = (z > ZX_SELF_VALUE_MAX)
                          ? ty_ptr(agg, true, false, 0) : agg;
            p0->ty = agg;
        }

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
static bool check_c_able(Node *at, Type *t, const char *fname) {
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

    Field *fs = calloc((size_t)(nfields ? nfields : 1), sizeof(Field));
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
        et->fields = calloc((size_t)(cnt ? cnt : 1), sizeof(Field));
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

    e->fields = calloc((size_t)(cnt ? cnt : 1), sizeof(Field));
    e->nfields = cnt;
    if (nst) e->statics = calloc((size_t)nst, sizeof(Field));

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
static Type *resolve_fn_type(Node *fn) {
    if (fn->ty && fn->ty->kind == TY_TYPE && fn->type_val) return fn->type_val;

    int np = fn->nitems;
    Type **ps = calloc((size_t)(np ? np : 1), sizeof(Type *));
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

static Type *unify(Node *at, Type *a, Type *b) {
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

static bool lvalue_mutable(Node *n) {
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

static Type *type_of(Node *n, Type *t) { n->ty = t; return t; }

static Type *check_num_operands(Node *at, Type *a, Type *b) {
    /* 连锁抑制：操作数已经报过错（unknown identifier 之类返回 TY_INVALID），
     * 根因诊断在上面发过了——这里跟着喊一嗓子只是噪音。 */
    if ((a && a->kind == TY_INVALID) || (b && b->kind == TY_INVALID))
        return ty_invalid();
    if (!numeric_like(a) || !numeric_like(b)) {
        /* `(i32)-3` 读起来像"把 -3 转成 i32"，但 C 风格 cast 只在后面跟
         * 得上操作数时成立（`-` 不在那个集合里），于是它被读成
         * `(i32) - 3`——类型值减整数。这儿的原话没人看得懂，补一句写法。 */
        if ((a && a->kind == TY_TYPE) || (b && b->kind == TY_TYPE))
            diag_help("a cast needs its operand right after the `)`; write `(i32)(-3)`, "
                      "not `(i32)-3` -- the latter reads as subtraction");
        return BAD(at, "arithmetic needs numbers, found `%s` and `%s`", ty_str(a), ty_str(b));
    }

    /* R4：没有任何隐式数值转换。字面量（comptime_int/float）例外，它会定型成对方。 */
    if (!ty_eq(a, b) && a->kind != TY_COMPTIME_INT && b->kind != TY_COMPTIME_INT &&
        a->kind != TY_COMPTIME_FLOAT && b->kind != TY_COMPTIME_FLOAT)
        return BAD(at, "no implicit conversion between `%s` and `%s`; write the conversion explicitly",
                   ty_str(a), ty_str(b));

    return unify(at, a, b);
}

/* 指针算术。偏移量按**元素**算（与 C 的指针加法一致），后端就不必乘 sizeof。
 *
 *   p + n / n + p / p - n  → *T（*[N]T 的长度信息在加法后就没有了）
 *   p - q                  → usize（元素个数）
 *
 * 偏移量只接受 usize 或字面量（R4：没有隐式数值转换），错误信息直接告诉人怎么写。 */
static Type *ptr_offset_type(Node *n, Type *p, Type *o, Node *onode) {
    if (o->kind == TY_COMPTIME_INT) {
        onode->ty = ty_usize();           /* 字面量定型，后端好写 */
        return p;
    }
    if (!ty_eq(o, ty_usize()))
        return BAD(n, "pointer arithmetic needs a `usize` offset, found `%s`; write `(usize)...`",
                   ty_str(o));
    return p;
}

static Type *check_ptr_arith(Node *n, Type *a, Type *b, TokKind op) {
    bool ap = a && a->kind == TY_PTR;
    bool bp = b && b->kind == TY_PTR;
    if (!ap && !bp) return NULL;

    if (op != TOK_PLUS && op != TOK_MINUS)
        return BAD(n, "only `+` and `-` apply to pointers, found `%s` and `%s`",
                   ty_str(a), ty_str(b));

    if (op == TOK_PLUS) {
        if (ap && bp) return BAD(n, "cannot add two pointers");
        Type *p = ap ? a : b;
        Type *o = ap ? b : a;
        Node *on = ap ? n->b : n->a;
        Type *r = ptr_offset_type(n, p, o, on);
        if (r->kind == TY_INVALID) return r;
        return type_of(n, ty_ptr(p->elem, p->is_const, false, 0));
    }

    /* p - … */
    if (!ap)
        return BAD(n, "cannot subtract a pointer from a number");
    if (bp) {                              /* p - q */
        if (!ty_eq(a->elem, b->elem))
            return BAD(n, "cannot subtract `%s` from `%s`: different element types",
                       ty_str(b), ty_str(a));
        return type_of(n, ty_usize());
    }
    Type *r = ptr_offset_type(n, a, b, n->b);
    if (r->kind == TY_INVALID) return r;
    return type_of(n, ty_ptr(a->elem, a->is_const, false, 0));
}

/* ---------------- §130 分配区间账本 ----------------
 *
 * 一份信息两用：编译期状态机（LIVE -> FREED，二次释放硬错）与预分配合并
 * （同一区间内常量大小的 heapAlloc 合成一次 alloc + 静态偏移）。
 * token 是**符号版本** `(site, 发行序号)`，不是运行时的 u32 gen：
 * 编译期不需要知道代数数值，只要认出「同一个发行版本」。
 *
 * 硬边界（decisions §130）：sema 是单遍 AST 遍历，没有 CFG / 路径合并。
 * 所以只做词法作用域级、不做路径敏感——控制流臂里的 free 不迁移状态
 * （标 opaque），循环里同一个分配点反复发行也一律 opaque 退回运行时。
 * 后端不改指针表示：*T 仍 8 字节、[]T 仍 16 字节，token 编译完擦除。
 */
#define RS_LIVE   0
#define RS_FREED  1
#define RS_DEAD   2   /* §133：整张句柄表被 deinit 作废 */

typedef struct {
    int      state;
    int      line, col;       /* 分配点 */
    int      free_line, free_col;  /* 第一次释放点 */
    uint64_t size, align;     /* 编译期已知，预分配布局用 */
    const char *unit;         /* §132 账本：分配点所在模块（g_srcpath） */
    int      gid;             /* §132 账本：合并组，0 = 未合并 */
    uint64_t off, total;      /* §132 账本：组内静态偏移 / 组总字节 */
    Node    *site;
    Symbol  *sym;             /* 绑定的局部变量（若有） */
    int      tag;             /* §170：关联的 TagSlot 下标（0 = 无），tag.x : heapAlloc */
    bool     no_merge;        /* heapSlice 的槽：运行期长度，不进预分配合并 */
    bool     is_handle;       /* §133：Handle 发行版本，不是堆字节分配 */
    bool     tainted;         /* 发行/作废点在控制流里，静态身份不可证明 */
    Symbol  *owner_sym;       /* 表实例（接收者符号），deinit 按它整批作废 */
} RegionSlot;

static RegionSlot *g_rslots;
static int         g_nrslots, g_crslots;
static int         g_flow_depth;   /* 处在 if/while/for/switch 臂里 */
static int         g_region_gid;   /* 合并组编号，从 1 起 */

static int region_new(Node *site, uint64_t size, uint64_t align) {
    if (g_nrslots == g_crslots) {
        int nc = g_crslots ? g_crslots * 2 : 32;
        RegionSlot *p = realloc(g_rslots, (size_t)nc * sizeof(RegionSlot));
        if (!p) return 0;
        g_rslots = p;
        memset(g_rslots + g_crslots, 0,
               (size_t)(nc - g_crslots) * sizeof(RegionSlot));
        g_crslots = nc;
    }
    int id = ++g_nrslots;
    RegionSlot *r = &g_rslots[id - 1];
    r->state = RS_LIVE;
    r->line = site->line; r->col = site->col;
    r->size = size; r->align = align;
    r->unit = g_srcpath;
    r->site = site;
    return id;
}

static void region_bind(int id, Symbol *s) {
    if (id > 0 && id <= g_nrslots) g_rslots[id - 1].sym = s;
}

/* 从表达式追回发行版本。只保住「地址没变」的传播：赋值、指针→指针
 * 强转、.?, 指针算术；聚合字段取回 / 全局 / 指针↔整数 / C 边界一律断。 */
static int region_of_expr(Node *e) {
    if (!e) return 0;
    switch (e->kind) {
    case NK_IDENT:  return e->sym ? e->sym->reg_slot : 0;
    case NK_CALL:   return e->reg_slot;   /* §133：create() 发行的 Handle 版本 */
    case NK_UNWRAP: return region_of_expr(e->a);
    case NK_CAST: {
        Type *t = e->ty, *s = e->b ? e->b->ty : NULL;
        if (t && s && t->kind == TY_PTR && s->kind == TY_PTR)
            return region_of_expr(e->b);
        return 0;
    }
    case NK_BIN:
        if (e->op == TOK_PLUS || e->op == TOK_MINUS)
            return region_of_expr(e->a);
        return 0;
    default: return 0;
    }
}

static bool is_allocator_ty(Type *t) {
    if (!t) return false;
    if (t == sema_allocator_type()) return true;
    return t->kind == TY_STRUCT && t->name && !strcmp(t->name, "Allocator");
}

/* Allocator.free(ctx, ptr, size) 是一次作废事件。只在直线代码里迁移，
 * 且 provenance 没断才判；其余一律标 opaque 交运行时 side table。 */
static void region_note_free(Node *call) {
    if (g_flow_depth > 0) return;
    Node *a = call->a;
    if (!a || a->kind != NK_FIELD || !a->name || strcmp(a->name, "free") ||
        !a->a || !is_allocator_ty(a->a->ty) || call->nitems != 3) return;
    Node *ptr = call->items[1];
    int id = region_of_expr(ptr);
    if (!id || id > g_nrslots) return;
    RegionSlot *r = &g_rslots[id - 1];
    if (r->tag) return;    /* §170：贴标分配走 tag 账本，region 不重复记账 */
    if (r->state == RS_FREED) {
        /* 同一个释放点被重复检查（泛型 / 折值会重扫同一棵树）不算第二次 */
        if (r->free_line == call->line && r->free_col == call->col) return;
        diag_code_at("E0509", call->line, call->col,
            "this allocation was already freed (first free at line %d, "
            "allocated at line %d)", r->free_line, r->line);
        return;
    }
    if (r->state != RS_LIVE) return;
    r->state = RS_FREED;
    r->free_line = call->line;
    r->free_col = call->col;
}

static uint64_t region_align_up(uint64_t v, uint64_t a) {
    return a <= 1 ? v : ((v + a - 1) / a) * a;
}

/* 合并白名单：这些变量只允许出现在下标/切片/字段的 base、声明初值里。
 * return、强转、取地址、比较、传参、赋给字段、二次赋值都会改变地址或
 * 延长生命周期，命中即整组退回逐点分配。 */
static bool region_uses_ok(Node *n, Node *parent, Symbol **syms, int ns) {
    if (!n) return true;
    if (n->kind == NK_IDENT && n->sym) {
        bool hit = false;
        for (int k = 0; k < ns; k++) if (syms[k] == n->sym) { hit = true; break; }
        if (hit) {
            bool ok = false;
            if (parent && parent->a == n &&
                (parent->kind == NK_INDEX || parent->kind == NK_SLICE ||
                 parent->kind == NK_FIELD)) ok = true;
            if (parent && parent->kind == NK_DECL && parent->b == n) ok = true;
            if (!ok) return false;
        }
    }
    if (!region_uses_ok(n->a, n, syms, ns)) return false;
    if (!region_uses_ok(n->b, n, syms, ns)) return false;
    if (!region_uses_ok(n->c, n, syms, ns)) return false;
    if (!region_uses_ok(n->d, n, syms, ns)) return false;
    for (int i = 0; i < n->nitems; i++)
        if (!region_uses_ok(n->items[i], n, syms, ns)) return false;
    for (int i = 0; i < n->nunroll; i++)
        if (!region_uses_ok(n->unroll[i], n, syms, ns)) return false;
    return true;
}

/* 在一个词法块里做预分配合并（§130 第 4 条白名单）。size/align 编译期
 * 已知：N 由 ev_int 强制为常量，@type(T).size/.align 同为编译期值。 */
static void region_plan_block(Node *blk) {
    if (g_flow_depth > 0 || g_in_defer || g_fn_depth <= 0) return;
    if (blk->reg_gid > 0) return;     /* 泛型 / 编译期执行会重复检查同一棵树 */
    Node   *sites[16];
    Symbol *syms[16];
    int n = 0;
    for (int i = 0; i < blk->nitems; i++) {
        Node *s = blk->items[i];
        if (s->kind != NK_DECL || !s->b || s->b->kind != NK_CALL ||
            !s->b->reg_slot || !s->sym) continue;
        if (n >= 16) return;
        sites[n] = s->b; syms[n] = s->sym; n++;
    }
    if (n < 2) return;
    /* 同一分配器：只认同一个局部标识符，别把两个不同 arena 混在一块 */
    Node *a0 = sites[0]->items[0];
    if (!a0 || a0->kind != NK_IDENT || !a0->name) return;
    for (int k = 1; k < n; k++) {
        Node *ak = sites[k]->items[0];
        if (!ak || ak->kind != NK_IDENT || !ak->name ||
            strcmp(ak->name, a0->name)) return;
    }
    /* 区间内必须全部还活着：出现过单笔 free 就是另一种生命周期，不合并 */
    for (int k = 0; k < n; k++) {
        int id = sites[k]->reg_slot;
        if (id <= 0 || id > g_nrslots || g_rslots[id - 1].state != RS_LIVE) return;
        if (g_rslots[id - 1].no_merge) return;   /* heapSlice：运行期长度 */
    }
    if (!region_uses_ok(blk, NULL, syms, n)) return;
    uint64_t off = 0, maxal = 1;
    for (int k = 0; k < n; k++) {
        RegionSlot *r = &g_rslots[sites[k]->reg_slot - 1];
        uint64_t al = r->align ? r->align : 1;
        if (al > maxal) maxal = al;
        off = region_align_up(off, al);
        sites[k]->reg_off = off;
        off += r->size;
    }
    if (maxal > 16) return;          /* malloc 只保证 16 字节对齐 */
    uint64_t total = region_align_up(off, maxal);
    if (!total) return;
    int gid = ++g_region_gid;
    for (int k = 0; k < n; k++) {
        sites[k]->reg_gid = gid;
        RegionSlot *r = &g_rslots[sites[k]->reg_slot - 1];
        r->gid = gid;
        r->off = sites[k]->reg_off;
        r->total = total;
    }
    blk->reg_gid = gid;
    blk->reg_total = total;
    blk->reg_alloc = a0;
    if (getenv("ZX_REGION"))
        fprintf(stderr, "region merged: gid=%d slots=%d total=%llu\n",
                gid, n, (unsigned long long)total);
}

/* ---------------- §133：Handle 证明账本与按访问点擦除 ----------------
 *
 * 同一套 slot 语义的两个时态：编译期状态机管能证明的分配点，运行时
 * side table 管证明不了的句柄。这里把 Handle 也接进同一份账本：
 *   create() 发行一个 is_handle slot；free() 消费；deinit() 整批 DEAD；
 *   get() 是访问点，逐点判 PROVEN / OPAQUE。
 * release 档里 PROVEN 的 get 改写成私有的 getUnchecked（跳过 gen 比对），
 * dev 档保留 get。两种发射形态共存，ptr[] 擦不掉，擦的是比对代码。
 * 失效后使用不报硬错：side table 本来就返回 null，写成 error 会把
 * 「查询合法性」也判成错误，破坏 fail-safe 契约。
 */
typedef struct {
    int line, col;
    const char *unit;
    bool proven, erased;
    const char *reason;
} ProvePoint;

static ProvePoint *g_proves;
static int         g_nproves, g_cproves;
extern int         g_zx_release;   /* src/main.c：--release 档位 */

static void prove_add(Node *n, bool proven, bool erased, const char *reason) {
    for (int i = 0; i < g_nproves; i++)
        if (g_proves[i].line == n->line && g_proves[i].col == n->col) return;
    if (g_nproves == g_cproves) {
        int nc = g_cproves ? g_cproves * 2 : 16;
        ProvePoint *p = realloc(g_proves, (size_t)nc * sizeof(ProvePoint));
        if (!p) return;
        g_proves = p; g_cproves = nc;
    }
    ProvePoint *q = &g_proves[g_nproves++];
    q->line = n->line; q->col = n->col; q->unit = g_srcpath;
    q->proven = proven; q->erased = erased; q->reason = reason;
}

static Type *strip_ptr_ty(Type *t) {
    return (t && t->kind == TY_PTR) ? t->elem : t;
}

/* opt-in 约定：容器同时声明 get 与私有 getUnchecked，才算「有中介的表」。
 * 机制通用、策略在库；不硬编码容器名、不加内建、不改指针表示。 */
static bool handle_container(Type *recv) {
    Type *t = strip_ptr_ty(recv);
    if (!t || t->kind != TY_STRUCT) return false;
    bool has_get = false, has_unchecked = false;
    for (int i = 0; i < t->nmethods; i++) {
        const char *nm = t->methods[i].name;
        if (!nm) continue;
        if (!strcmp(nm, "get")) has_get = true;
        else if (!strcmp(nm, "getUnchecked")) has_unchecked = true;
    }
    return has_get && has_unchecked;
}

static void region_handle_call(Node *n) {
    if (!n->a || n->a->kind != NK_FIELD || !n->a->name || !n->a->a) return;
    if (!handle_container(n->a->a->ty)) return;
    const char *m = n->a->name;
    if (!strcmp(m, "getUnchecked")) return;      /* 已改写：幂等 */
    Symbol *own = n->a->a->sym;

    if (!strcmp(m, "create")) {
        if (n->reg_slot) return;
        int id = region_new(n, 0, 0);
        if (!id) return;
        n->reg_slot = id;
        RegionSlot *r = &g_rslots[id - 1];
        r->is_handle = true;
        r->owner_sym = own;
        /* 发行点在控制流构造里：同一源码点可能反复发行，静态身份分不出代 */
        if (g_flow_depth > 0 || !own) r->tainted = true;
        return;
    }
    if (!strcmp(m, "deinit")) {
        for (int i = 0; i < g_nrslots; i++) {
            RegionSlot *r = &g_rslots[i];
            if (r->is_handle && r->owner_sym && r->owner_sym == own)
                r->state = RS_DEAD;
        }
        return;
    }
    if (strcmp(m, "free") && strcmp(m, "get")) return;
    /* 方法调用已把接收者插到 items[0]，句柄实参落在 items[1] */
    Node *harg = n->meth_done ? (n->nitems >= 2 ? n->items[1] : NULL)
                             : (n->nitems >= 1 ? n->items[0] : NULL);
    int id = harg ? region_of_expr(harg) : 0;
    RegionSlot *r = (id > 0 && id <= g_nrslots) ? &g_rslots[id - 1] : NULL;

    /* §135（用户 2026-11-13 裁定）：句柄检查只在编译期做，运行时 side
     * table 这条线撤掉。于是「证明不了」不再是退回查表，而是硬错：抹源
     * （cast / 存进字段 / 跨函数）之后没有第二道防线，必须当场拒绝。
     * 两个码分工：E0510 = 编译期**确定失效**；E0511 = **证明不了身份**。 */
    const char *why = "handle provenance lost";
    bool known = false;
    if (!r)                        why = "handle provenance lost";
    else if (!r->is_handle)        why = "not a handle issued by create()";
    else if (r->tainted)           why = "issue/free point inside control flow";
    else                           known = true;

    if (!strcmp(m, "free")) {
        if (!known) {
            diag_code_at("E0511", n->line, n->col,
                "cannot prove which handle this is (%s) -- handle checks are "
                "compile-time only, so an unprovable handle is an error, not a "
                "runtime lookup", why);
            prove_add(n, false, false, why);
            return;
        }
        if (r->state == RS_FREED) {
            diag_code_at("E0509", n->line, n->col,
                "this handle was already freed (first free at line %d, issued "
                "by create at line %d)", r->free_line, r->line);
            prove_add(n, true, false, "already freed");
            return;
        }
        if (r->state == RS_DEAD) {
            diag_code_at("E0510", n->line, n->col,
                "this handle is dead at compile time (%s) -- free it before "
                "`deinit`, or drop the `deinit`", "handle table closed by deinit");
            prove_add(n, false, false, "handle table closed by deinit");
            return;
        }
        if (g_flow_depth > 0) r->tainted = true;  /* 作废事件路径不定 */
        r->state = RS_FREED;
        r->free_line = n->line; r->free_col = n->col;
        prove_add(n, true, false, "free of a live handle");
        return;
    }

    /* get(h)：能证明 LIVE 就直接发私有 getUnchecked（零检查）。两档发射同一种
     * 形态——不再有「dev 保留 get、靠 valid() 返回 null」的第二条路。 */
    if (known && g_in_defer) { why = "defer body"; known = false; }
    if (!known) {
        diag_code_at("E0511", n->line, n->col,
            "cannot prove this handle is alive (%s) -- handle table checks were "
            "removed from the runtime; keep create/get/free in straight-line "
            "code and bind the handle to a local", why);
        prove_add(n, false, false, why);
        return;
    }
    if (r->state == RS_FREED || r->state == RS_DEAD) {
        diag_code_at("E0510", n->line, n->col,
            "read of an invalid handle (%s) -- it is dead at compile time and the "
            "runtime no longer has a table to re-check it",
            r->state == RS_DEAD ? "handle table closed by deinit" : "already freed");
        prove_add(n, true, false, "invalid handle");
        return;
    }
    n->a->name = "getUnchecked";
    prove_add(n, true, true, "proven LIVE, straight-line");
}

/* §135：`.?` 不再单独判失效。句柄的失效在 get 处就报 E0510（那里既知道
 * 「确定失效」也知道「证明不了」），再在 unwrap 处报一遍会变成一条语句
 * 两个错，反而盖住真正的修法。 */

/* ---------------- §170 tag 指针生命周期账本 ----------------
 *
 * tag 是用户显式命名的编译期唯一标签：`tag.name;` 声明，`tag.name : expr`
 * 把一个堆分配或同标签载体绑到它上面。状态机五格（载体侧）：
 *   NONE / LIVE / FREED / COND（分支合并出「可能是 null」）/
 *   UNKNOWN（分支间状态不一致，未防守）。
 * tag 侧（TagSlot）只记分配事实：UNBOUND / LIVE / FREED。
 *
 * 硬规则（§170 立法，§173 扩成全链路）：
 *   - 跨函数追踪（§173）：指针形参领一张「抽象槽」（借出语义），体内
 *     deref/free/传递走同一本账本；释放器调用点把真实槽标 freed；
 *     被调方把形参/体内分配原样返回时，调用点继承 tag（返回流）。
 *   - 分配自动建槽（§173）：每个 alloc 调用点自动获得唯一标签并预绑
 *     LIVE；`tag.x : expr` 退化为「给分配起名」。stackAlloc 的槽离开
 *     作用域自动标 free（栈存储随帧消亡）。
 *   - 循环体内分配/释放必须配平（进出同状态），循环不传出贴标载体。
 *   - 分支内释放必须传出 null（载体）；分支内分配必须传出载体。
 *   - tag.close 关闭本模块的追踪与强制（库/旧代码逃生口），std 天然豁免。
 */

#define TST_UNBOUND 0
#define TST_LIVE    1
#define TST_FREED   2
#define TST_MOVED   3   /* §189：std.move —— 所有权已转移（内存还活着，
                          * 但本账本不再负责：原载体使用/free 都报 E0529，
                          * 泄漏终扫豁免）。区别于 FREED（内存已还）。 */

/* 载体状态（Node.tag_state / Symbol.tag_state） */
#define TC_NONE     0
#define TC_LIVE     1
#define TC_FREED    2
#define TC_COND     3
#define TC_UNKNOWN  4
#define TC_MOVED    5   /* §189：载体已随 std.move 转移 */

static const char *g_cur_fn_name;   /* 当前函数名（7963 附近保存/还原） */

typedef struct {
    const char *name;      /* 编译期唯一名（程序级，跨文件不可重名） */
    int         line, col;
    int         state;     /* TST_*：分配事实 */
    int         region;    /* 当前关联的 region token（0 = 无） */
    int         free_line, free_col;
    int         moved_line, moved_col;  /* §189：std.move 转移点（E0529
                                          * 指认"move at line N"） */
    bool        confirm_arm; /* §189：臂内 `tag v : null;` 确认标记——
                               * 证明使用者确认过本臂的释放（分支本就
                               * 可能不走，未确认的释放合流后当作还没
                               * 释放）。臂快照回滚时随状态清。 */
    int         confirm_line, confirm_col;  /* 确认声明处（落空报错指位） */
    int         own_line, own_col;  /* §173：当前所有权归属点（最近一次
                          LIVE 载体绑定/调用点继承的位置，0 = 仍在
                          分配处）——泄漏终扫把错误指到这里而非分配行 */
    Symbol     *carrier;   /* 当前载体符号（free 事件写它的 tag_state；
                            * 臂快照要连同它一起回滚，否则 then 臂的
                            * 释放事实会污染 else 臂的检查） */
    bool        stack;     /* §173：stackAlloc 的槽 —— 离开作用域自动标 free */
    bool        abstract;  /* §173：形参借出槽 —— 跨函数追踪，不记分配事实 */
    bool        auto_n;    /* §173：alloc 调用点自动建的槽（可被显式贴标收编） */
    bool        scope_freed; /* §173：离开作用域自动回收（stackAlloc） */
    bool        ever_freed;  /* §173：体内发生过释放（臂回滚抹不掉，
                              * 跨函数精确释放判定用） */
    bool        arr_esc;     /* §173 补丁六：逃进数组/元组元素——
                              * 元素级追踪断线时终扫给手动标记提示 */
    bool        rt_pending;  /* §174：运行期索引写入过数组（追踪必断线）。
                              * 续链接管 / 手动终结即清；没处理则终扫报
                              * E0521——"必须处理"而非当场拦死。 */
    int         rt_line, rt_col;   /* 运行期写入处（E0521 报出点） */
    Symbol     *adopt_ar;   /* §187：已被 arena 收养（ArenaAllocator.adopt）
                              * ——所有权移交给了这个 arena 变量。槽保持
                              * LIVE（内存活到 arena deinit，可解引用）；
                              * free 撞账报 E0526；deinit 统一终结。
                              * NULL = 未收养。 */
    int         adopt_line, adopt_col;   /* 收养处（E0526/E0509 指位） */
    bool        book;       /* §187：容器账——std 资源容器（List/Pool/
                              * ArenaAllocator/DebugAllocator）的 init
                              * 调用点开的账：整个容器的内存由它管，
                              * deinit 统一终结；漏 deinit = 泄漏。 */
    Node       *ret_fn;      /* §175：分配经返回流转出所有权的工厂函数
                                （fn->index = 本槽）。零调用工厂的分配
                                运行期不发生，终扫豁免。 */
    int         inst_of;     /* §175：本槽是某工厂槽的调用点实例（母槽
                                索引+1，0 = 原生槽）。工厂每次调用都是
                                一份新分配，实例槽各自记账。 */
    int         ninst;       /* §175：已派生的调用点实例数（母槽用）。
                                > 0 时终扫由实例接管，母槽退役。 */
    const char *def_fn;      /* §176：分配点所在函数名——泄漏诊断沿调用
                                边回溯到 main 的起点 */
    const char *own_fn;      /* §176：实例槽归属调用点所在函数名 */
    const char *file;        /* §176：分配点所在文件（note 指位用） */
} TagSlot;
static TagSlot *g_tslots;
static int      g_ntslots, g_ctslots;

/* §176：调用边登记（谁在哪一行调了谁）。泄漏诊断要"一直回溯到 main"，
 * 而 sema 按依赖拓扑逐函数检查、没有运行期式的调用栈——检查路径不等于
 * 调用路径。这里在每个已解析的调用点记一条边（tag_call_flow 出口），
 * 终扫沿边回溯。同一调用点重扫幂等（deps 拓扑补查会重跑 check_call）。 */
typedef struct { const char *callee, *caller; int line, col; } TagCallEdge;
static TagCallEdge *g_cedges;
static int g_ncedges, g_ccedges;

static void tag_call_edge(const char *callee, const char *caller,
                          int line, int col) {
    if (!callee || !*callee || !caller || !*caller) return;
    if (!strcmp(callee, caller)) return;            /* 递归自调用不上链 */
    for (int i = 0; i < g_ncedges; i++)
        if (g_cedges[i].line == line && g_cedges[i].col == col &&
            !strcmp(g_cedges[i].callee, callee) &&
            !strcmp(g_cedges[i].caller, caller)) return;
    if (g_ncedges == g_ccedges) {
        int nc = g_ccedges ? g_ccedges * 2 : 32;
        TagCallEdge *p = realloc(g_cedges, (size_t)nc * sizeof *p);
        if (!p) return;
        g_cedges = p;
        g_ccedges = nc;
    }
    g_cedges[g_ncedges].callee = callee;
    g_cedges[g_ncedges].caller = caller;
    g_cedges[g_ncedges].line = line;
    g_cedges[g_ncedges].col = col;
    g_ncedges++;
}

static Module *g_std_mod;       /* 5931 附近定义：std 模块（豁免判定用） */

static TagSlot *tag_slot(int id) {       /* id = TagSlot 下标 + 1 */
    return (id > 0 && id <= g_ntslots) ? &g_tslots[id - 1] : NULL;
}

static TagSlot *tag_by_name(const char *name) {
    for (int i = 0; i < g_ntslots; i++)
        if (!strcmp(g_tslots[i].name, name)) return &g_tslots[i];
    return NULL;
}

/* ---------------- §173 字段账本 ----------------
 * 结构体值携带的 tag 标记（字段名 -> 槽 id）。挂两处：节点（字面量
 * 烙印 / 调用点继承）与符号（let/var 载体）。状态不存副本——一律从
 * 槽现推，free 事件 / 作用域退出改槽，所有字段载体自动同步
 * （泄漏终扫是槽级的，也自动覆盖）。已知限制：嵌套字段（s.inner.name）、
 * 元组/结构体数组的字段、方法接收者、臂内整结构体赋值的快照回滚，
 * 都暂不追。 */

struct FieldTag {
    const char *name;          /* 字段名（AST 内部串，稳定） */
    int         slot;          /* TagSlot 下标 + 1；0 = 该字段不携带 */
    FieldTag   *next;
};

static FieldTag *ft_find(FieldTag *list, const char *name) {
    for (; list; list = list->next)
        if (list->name && name && !strcmp(list->name, name)) return list;
    return NULL;
}

static void ft_set(FieldTag **plist, const char *name, int slot) {
    FieldTag *f = ft_find(*plist, name);
    if (f) { f->slot = slot; return; }
    f = malloc(sizeof *f);
    if (!f) return;
    f->name = name; f->slot = slot; f->next = *plist;
    *plist = f;
}

static FieldTag *ft_clone(FieldTag *src) {
    FieldTag *out = NULL, **tail = &out;
    for (; src; src = src->next) {
        FieldTag *f = malloc(sizeof *f);
        if (!f) break;
        f->name = src->name; f->slot = src->slot; f->next = NULL;
        *tail = f; tail = &f->next;
    }
    return out;
}

/* §175：聚合抽取（let inner = tt.0）——取母账里 key 前缀的子树，
 * 剥掉前缀返回新链（"0.0"→槽 变 "0"→槽）。找不到返回 NULL。 */
static FieldTag *ft_subtree(FieldTag *list, const char *key) {
    FieldTag *out = NULL, **tail = &out;
    size_t kl = strlen(key);
    for (; list; list = list->next) {
        if (!list->name) continue;
        const char *rest = NULL;
        if (strncmp(list->name, key, kl) == 0 && list->name[kl] == '.')
            rest = list->name + kl + 1;
        else if (strcmp(list->name, key) == 0)
            rest = "";
        if (!rest) continue;
        FieldTag *f = malloc(sizeof *f);
        if (!f) break;
        f->name = strdup(*rest ? rest : key);
        f->slot = list->slot; f->next = NULL;
        *tail = f; tail = &f->next;
    }
    return out;
}

/* §175：清掉 key 及其子树（"key" 与 "key.xxx"）——整段值被覆盖时
 * 旧条目作废，防止后续读到陈旧槽。 */
static void ft_clear_prefix(FieldTag *list, const char *key) {
    size_t kl = strlen(key);
    for (; list; list = list->next) {
        if (!list->name) continue;
        if (!strncmp(list->name, key, kl) &&
            (list->name[kl] == '\0' || list->name[kl] == '.'))
            list->slot = 0;
    }
}

/* §192：转移溯源镜像。字段账随 DECL/ASG 从源表达式子树拷到新载体后，
 * 载体记下来源（Symbol.ft_src）；载体上的字段写入/清除沿溯源链镜像回
 * 源账本（let mid = t2[0].pt 之后写 mid.name，t2 的 "0.pt.name" 同步
 * 落账），两个实例读同一存储位置口径一致——实例怎么转移都不跟丢。
 * slot>0 写入（先清子树再落账），slot==0 只清。深度 8 防环。 */
static bool ft_path_key(Node *e, char *buf, size_t cap, Node **base_out);
/* §196：deref_tail 非 NULL 时带回「路径末端是否为裸指针穿透」（如
 * `t1[0].name.0` 的 `.0`）——末端穿透 = 写的是字段指向的存储内容，
 * 字段身份不变，ASSIGN 不得清该路径的字段账（否则 `t1[0].name.0 = 10`
 * 会把 "0.name" 的槽账抹掉，之后 free(t1[0].name[0..1]) 解析不到槽、
 * 误报 E0513）。路径中间的穿透（`t1[0].name.0.f` 的 `.0`）不影响判定。 */
static bool ft_path_key_d(Node *e, char *buf, size_t cap, Node **base_out,
                          bool *deref_tail);
static char *ft_path_dup(const char *prefix, const char *leaf);
static void ft_mirror(Symbol *s, const char *key, int slot, int depth) {
    if (!s || !s->ft_src || !key || !key[0] || depth > 8) return;
    char path[128];
    Node *fb = NULL;
    if (!ft_path_key(s->ft_src, path, sizeof path, &fb)) path[0] = 0;
    if (!fb || fb->kind != NK_IDENT || !fb->sym) return;
    Symbol *b2 = fb->sym;
    char full[160];
    if (path[0]) snprintf(full, sizeof full, "%s.%s", path, key);
    else snprintf(full, sizeof full, "%s", key);
    if (slot > 0) {
        ft_clear_prefix((FieldTag *)b2->ftags, full);
        char *pk = ft_path_dup(path[0] ? path : NULL, key);
        if (pk) ft_set((FieldTag **)&b2->ftags, pk, slot);
    } else {
        ft_clear_prefix((FieldTag *)b2->ftags, full);
    }
    ft_mirror(b2, full, slot, depth + 1);
}

/* 合并两条 return 路径的字段表；同名字段槽不一致 = 状态不一致，丢弃。 */
static FieldTag *ft_merge(FieldTag *a, FieldTag *b) {
    FieldTag *out = NULL;
    for (FieldTag *f = a; f; f = f->next) {
        FieldTag *g = ft_find(b, f->name);
        if (g && g->slot != f->slot) continue;
        ft_set(&out, f->name, f->slot);
    }
    for (FieldTag *f = b; f; f = f->next)
        if (!ft_find(a, f->name)) ft_set(&out, f->name, f->slot);
    return out;
}

/* 字段槽当前载体状态：从槽现推。 */
static int ft_state_of(int slot) {
    TagSlot *t = tag_slot(slot);
    if (!t || t->state == TST_UNBOUND) return TC_NONE;
    if (t->state == TST_MOVED) return TC_MOVED;   /* §189 */
    return t->state == TST_FREED ? TC_FREED : TC_LIVE;
}

/* §173 补丁八：字段链折叠成点路径 key。e 是最内层 FIELD
 * （s.inner.name 的 .name），沿 ->a 收集外层字段名，拼成
 * "inner.name"；最深基址（IDENT/字面量/调用）经 base_out 带回。
 * 返回 false = 不是纯字段链（深度超限/中途出现其他节点）。 */
static bool ft_path_key_d(Node *e, char *buf, size_t cap, Node **base_out,
                          bool *deref_tail) {
    const char *names[8];
    char idxbuf[8][16];
    int np = 0;
    bool deref_head = false;   /* §196：首步（最外层）是否为裸指针穿透——
                                * 循环从最外层向内折，穿透发生在 np==0 时
                                * 即「末端穿透」（`t1[0].name.0` 的 `.0` 是
                                * 路径之外最后一层）；路径中间的穿透
                                * （`t1[0].name.0.f`，此时 np>0）不影响。 */
    Node *fb = e;
    /* §174：元组字段访问被改写成 NK_FIELD（name=NULL、ival=下标）——
     * 基址类型是元组的数字分量按 "N" 进点路径；§192：基址是**数组指针**
     * 的 .N（等价 ptr[N]）同样按下标分量进路径（与 NK_INDEX 常索引同
     * 一本账，两种拼法互通）；基址是裸指针的 .0 是纯解引用（身份不变），
     * 路径透明穿过（等价自动穿透的字段访问）；运行期索引在此截断，
     * 写入处另报 E0521。 */
    while (fb && np < 8 &&
           (fb->kind == NK_FIELD &&
            (fb->name || (fb->ival >= 0 && fb->a && fb->a->ty &&
                          (fb->a->ty->kind == TY_TUPLE ||
                           fb->a->ty->kind == TY_PTR))) ||
            (fb->kind == NK_INDEX && fb->b && fb->b->kind == NK_INT &&
             fb->a))) {
        if (fb->kind == NK_FIELD) {
            if (fb->name) {
                names[np] = fb->name;
            } else if (fb->a->ty->kind == TY_PTR && !fb->a->ty->has_len &&
                       !ty_is_array_ptr(fb->a->ty)) {
                fb = fb->a;                 /* 裸指针 .0：透明穿过 */
                if (np == 0) deref_head = true;   /* §196：首步穿透 */
                continue;
            } else {
                snprintf(idxbuf[np], sizeof idxbuf[np], "%d", (int)fb->ival);
                names[np] = idxbuf[np];
            }
        } else {
            snprintf(idxbuf[np], sizeof idxbuf[np], "%d",
                     (int)fb->b->ival);
            names[np] = idxbuf[np];
        }
        np++;
        fb = fb->a;
    }
    if (!fb || np == 0 || np == 8) {
        if (base_out) *base_out = fb;   /* §175：截断点带回（运行期索引判定用） */
        return false;
    }
    if (deref_tail) *deref_tail = deref_head;   /* §196：末端穿透 */
    /* names[0] 是最内层（叶子字段名），names[np-1] 是最外层——倒着拼 */
    size_t off = 0;
    for (int i = np - 1; i >= 0; i--) {
        size_t l = strlen(names[i]);
        if (off + l + 2 > cap) return false;
        memcpy(buf + off, names[i], l);
        off += l;
        if (i > 0) buf[off++] = '.';
    }
    buf[off] = 0;
    *base_out = fb;
    return true;
}

/* §196：四参兼容包装（不关心末端穿透的调用点）。 */
static bool ft_path_key(Node *e, char *buf, size_t cap, Node **base_out) {
    return ft_path_key_d(e, buf, cap, base_out, NULL);
}

/* 点路径 key 的持久化副本（插账用——ft_set 只存指针）。 */
static char *ft_path_dup(const char *prefix, const char *leaf) {
    size_t lp = prefix ? strlen(prefix) : 0, ll = strlen(leaf);
    char *s = malloc(lp + (lp ? 1 : 0) + ll + 1);
    if (!s) return NULL;
    if (lp) { memcpy(s, prefix, lp); s[lp++] = '.'; }
    memcpy(s + lp, leaf, ll + 1);
    return s;
}

static bool ft_member_of(FieldTag *list, const char *name, int *id, int *state) {
    FieldTag *f = ft_find(list, name);
    if (!f || f->slot <= 0) return false;
    int st = ft_state_of(f->slot);
    if (st == TC_NONE) return false;
    *id = f->slot; *state = st;
    return true;
}

/* 表达式的字段账本：节点自带优先，ident 查符号。 */
static FieldTag *ftags_of_expr(Node *e) {
    if (!e) return NULL;
    if (e->ftags) return (FieldTag *)e->ftags;
    if (e->kind == NK_IDENT && e->sym && e->sym->ftags)
        return (FieldTag *)e->sym->ftags;
    return NULL;
}

static void tag_of_expr(Node *e, int *id, int *state);   /* 定义在贴标一节 */
static void tuple_stamp_ftags(Node *n);                  /* 定义在元组字面量 */
static void tag_transform_deinit(Node *call);            /* 定义在贴标一节 */
static bool ty_is_allocator(Type *t);                    /* 定义在分配器一节 */
static bool arm_ends(Node *a);                           /* 定义在缺返回分析 */
static int  arm_ret_scan(Node *nd, int slot);            /* §187 终局臂传出 */

/* §174：捕获载荷继承主语的 tag 账（switch 的 union(enum) 变体捕获、
 * if/while/try 的 ?T / !T 捕获）。
 *  - 主语顶层带 tag 且捕获类型是指针/切片（?*u8 的 .some 捕获、变体
 *    载荷本身是指针）：捕获符号接同一槽；
 *  - 主语字段账带「变体.子字段」路径（union(enum) 载荷是聚合）：剥掉
 *    变体前缀落到捕获符号；
 *  - 主语字段账无前缀（?S 载荷结构体的字段账）：原样继承。
 * 不继承就断链：臂内 free(b.0) 记不上账，泄漏终扫误报。 */
static void tag_capture_inherit(Symbol *cs, Node *subj, const char *vname) {
    if (!cs) return;
    int id, st;
    tag_of_expr(subj, &id, &st);
    bool ptr_cap = cs->ty && (cs->ty->kind == TY_PTR || cs->ty->kind == TY_SLICE);
    /* §187：容器账（book）随捕获绑定一起走——`if (ar) a { … }` 的
     * a 必须带着资源的账，deinit(a) 才终结得了。 */
    if (id && st == TC_LIVE &&
        (ptr_cap || (tag_slot(id) && tag_slot(id)->book))) {
        cs->tag_id = id;
        cs->tag_state = st;
    }
    FieldTag *sft = ftags_of_expr(subj);
    for (FieldTag *g = sft; g; g = g->next) {
        if (g->slot <= 0) continue;
        if (vname && vname[0]) {
            size_t vl = strlen(vname);
            if (!strncmp(g->name, vname, vl) && g->name[vl] == '.') {
                char *key = ft_path_dup(NULL, g->name + vl + 1);
                if (key) ft_set((FieldTag **)&cs->ftags, key, g->slot);
            } else if (!strcmp(g->name, vname) && ptr_cap && !cs->tag_id) {
                cs->tag_id = g->slot;
                cs->tag_state = TC_LIVE;
            }
        } else {
            char *key = ft_path_dup(NULL, g->name);
            if (key) ft_set((FieldTag **)&cs->ftags, key, g->slot);
        }
    }
}

/* 「形参字段被 free」的事实账（与被调/调用先后无关）：被调体检查时
 * free 的指针实参剥到「形参符号 . 字段」记一条；调用点传结构体实参
 * 时按形参符号对上，把字段槽标 FREED——所有权移交在任何声明顺序下
 * 都成立（与直接指针形参的 ever_freed 同口径：体内 free 过即终局）。 */
typedef struct { Symbol *psym; const char *fname; } ParamFieldFree;
static ParamFieldFree *g_pff;
static int g_npff, g_cpff;

static void pff_add(Symbol *ps, const char *fname) {
    for (int i = 0; i < g_npff; i++)
        if (g_pff[i].psym == ps && !strcmp(g_pff[i].fname, fname)) return;
    if (g_npff == g_cpff) {
        int nc = g_cpff ? g_cpff * 2 : 16;
        ParamFieldFree *p2 = realloc(g_pff, (size_t)nc * sizeof(ParamFieldFree));
        if (!p2) return;
        g_pff = p2; g_cpff = nc;
    }
    g_pff[g_npff].psym = ps;
    g_pff[g_npff].fname = fname;
    g_npff++;
}

static bool pff_has(Symbol *ps, const char *fname) {
    for (int i = 0; i < g_npff; i++)
        if (g_pff[i].psym == ps && !strcmp(g_pff[i].fname, fname)) return true;
    return false;
}

/* §194：形参字段被 store 新分配的事实账（pff 的对称面）。被调体检查时
 * `t[0].name = heapAlloc(...)` 这类「往形参容器的字段写入新分配」记一条
 * (形参符号, 点路径 key, 分配槽, def_fn)；调用点按形实对位把槽实例化后
 * 烙到实参载体字段账——调用方读回 `t1[0].name` 才能拿到槽并释放，否则
 * 分配槽无人接管、终扫误报 E0513（假阳性）。fname 用持久副本（ft_set
 * 只存指针），同一 key 覆盖写更新 slot（与 aesc 换账同口径）。 */
typedef struct {
    Symbol *psym;         /* 被写入的形参容器符号 */
    const char *key;      /* 点路径 key（如 "0.name"） */
    int slot;             /* 写入的槽（本体内新分配 或 透传的抽象形参槽） */
    Symbol *src_param;    /* §195：非 NULL = 透传自另一形参的抽象槽——
                           * 调用点把「那个形参对应的实参」自身的槽烙到
                           * 容器实参字段账（不 clone，所有权本在调用方）；
                           * NULL = 本体内新分配，照旧 clone 实例。 */
} ParamFieldStore;
static ParamFieldStore *g_pfs;
static int g_npfs, g_cpfs;

static void pfs_add_ex(Symbol *ps, const char *key, int slot, Symbol *src_param) {
    for (int i = 0; i < g_npfs; i++)
        if (g_pfs[i].psym == ps && !strcmp(g_pfs[i].key, key)) {
            g_pfs[i].slot = slot;
            g_pfs[i].src_param = src_param;   /* 覆盖写：换账 */
            return;
        }
    if (g_npfs == g_cpfs) {
        int nc = g_cpfs ? g_cpfs * 2 : 16;
        ParamFieldStore *p2 = realloc(g_pfs, (size_t)nc * sizeof(ParamFieldStore));
        if (!p2) return;
        g_pfs = p2; g_cpfs = nc;
    }
    g_pfs[g_npfs].psym = ps;
    g_pfs[g_npfs].key = strdup(key);
    g_pfs[g_npfs].slot = slot;
    g_pfs[g_npfs].src_param = src_param;
    g_npfs++;
}
static void pfs_add(Symbol *ps, const char *key, int slot) {
    pfs_add_ex(ps, key, slot, NULL);
}

/* §180：「形参被解引用」事实账。deps 拓扑下被调体先查、抽象槽共用，
 * 函数体内看到的形参永远是 TC_LIVE（调用点的 FREED 状态进不去）——
 * 所以 use-after-free 只能在**调用点**判：被调体检查时在这里记下哪些
 * 形参符号被 deref 过（p[0] / p.0 / p.field / *p），调用点传进来的
 * 实参若已 FREED 且对应形参在账里，就报 E0512。与 pff（体内 free →
 * 调用点标 FREED）方向相反、机制对称；按符号指针记账，跨声明序稳定。 */
static Symbol **g_pderef;
static int      g_npderef, g_cpderef;
static void pderef_add(Symbol *ps) {
    if (!ps || ps->kind != SK_PARAM) return;
    for (int i = 0; i < g_npderef; i++)
        if (g_pderef[i] == ps) return;
    if (g_npderef == g_cpderef) {
        int nc = g_cpderef ? g_cpderef * 2 : 16;
        Symbol **p2 = realloc(g_pderef, (size_t)nc * sizeof(Symbol *));
        if (!p2) return;
        g_pderef = p2; g_cpderef = nc;
    }
    g_pderef[g_npderef++] = ps;
}
static bool pderef_has(Symbol *ps) {
    for (int i = 0; i < g_npderef; i++)
        if (g_pderef[i] == ps) return true;
    return false;
}
/* 形参抽象槽 → 形参符号（别名 `let q = p; q[0]` 也能归到 p）。 */
typedef struct { int tag_id; Symbol *psym; } ParamSlotSym;
static ParamSlotSym *g_pslotsym;
static int g_npslotsym, g_cpslotsym;
static void pslot_sym_add(int tag_id, Symbol *ps) {
    if (!tag_id || !ps) return;
    for (int i = 0; i < g_npslotsym; i++)
        if (g_pslotsym[i].tag_id == tag_id) return;
    if (g_npslotsym == g_cpslotsym) {
        int nc = g_cpslotsym ? g_cpslotsym * 2 : 16;
        ParamSlotSym *p2 = realloc(g_pslotsym, (size_t)nc * sizeof(ParamSlotSym));
        if (!p2) return;
        g_pslotsym = p2; g_cpslotsym = nc;
    }
    g_pslotsym[g_npslotsym].tag_id = tag_id;
    g_pslotsym[g_npslotsym].psym = ps;
    g_npslotsym++;
}
static Symbol *pslot_sym_find(int tag_id) {
    for (int i = 0; i < g_npslotsym; i++)
        if (g_pslotsym[i].tag_id == tag_id) return g_pslotsym[i].psym;
    return NULL;
}
/* 从解引用 base 剥到形参：`p` / `p.0` / `p[i]` / `p.field` / `*p`。 */
static void pderef_note(Node *base, int tag_id) {
    Node *e = base;
    for (int g = 0; g < 12 && e; g++) {
        if (e->kind == NK_IDENT && e->sym) {
            pderef_add(e->sym);
            break;
        }
        e = e->kind == NK_UNWRAP || e->kind == NK_CAST ? (e->b ? e->b : e->a)
            : (e->kind == NK_FIELD || e->kind == NK_INDEX) ? e->a : NULL;
    }
    Symbol *ps = pslot_sym_find(tag_id);
    if (ps) pderef_add(ps);
}

/* §173 补丁十（用户裁定收紧）：数组/元组元素的贴标指针，只有**精确
 * 元素账 (base, idx>=0) → slot**——常索引写入。运行期索引（xs[i]，
 * i 是变量）编译期无法指认元素（i↔元素的对应是运行期事实，静态恢复
 * 不可判定），写入/读出/free 一律**报 E0521**（标签绝不静默丢失），
 * 使用者必须显式处理：改常索引、用续链语句接回（tag v : tag.x =
 * xs[i];）、或终结（tag.p : null;）。同 (base,idx) 覆盖换账，旧槽若
 * 再无载体由终扫 E0513 兜底。 */
typedef struct { Symbol *base; int idx; int slot; } ArrEsc;
static ArrEsc *g_aesc;
static int g_naesc, g_caesc;

static void aesc_add(Symbol *base, int idx, int slot) {
    for (int i = 0; i < g_naesc; i++)
        if (g_aesc[i].base == base && g_aesc[i].idx == idx) {
            g_aesc[i].slot = slot;          /* 覆盖写入：换账 */
            return;
        }
    if (g_naesc == g_caesc) {
        int nc = g_caesc ? g_caesc * 2 : 16;
        ArrEsc *p2 = realloc(g_aesc, (size_t)nc * sizeof(ArrEsc));
        if (!p2) return;
        g_aesc = p2; g_caesc = nc;
    }
    g_aesc[g_naesc].base = base;
    g_aesc[g_naesc].idx = idx;
    g_aesc[g_naesc].slot = slot;
    g_naesc++;
    TagSlot *t = tag_slot(slot);
    if (t) t->arr_esc = true;
}

/* 精确元素账：(base, idx) 对应的贴标槽，0 = 无账。 */
static int aesc_find(Symbol *base, int idx) {
    for (int i = 0; i < g_naesc; i++)
        if (g_aesc[i].base == base && g_aesc[i].idx == idx)
            return g_aesc[i].slot;
    return 0;
}

/* 这个数组被用来存过贴标指针吗？（运行期索引读出的断线判据：
 * 有账 = 里面可能有贴标指针，哪个元素装哪个编译期不知道。） */
static bool aesc_any(Symbol *base) {
    for (int i = 0; i < g_naesc; i++)
        if (g_aesc[i].base == base) return true;
    return false;
}

/* 索引表达式是编译期常数则取值（>=0），否则 -1。 */
static int aesc_const_index(Node *ix) {
    if (ix && ix->kind == NK_INT && ix->ival >= 0) return (int)ix->ival;
    return -1;
}

/* §201：`*[N]T` 的 `.N` / `[i]` 取的是第 i 个**数组**，不是元素；
 * 元素要继续 `[i]`。裸指针 `.0` 是解引用。两者都不算"元素访问"。 */
static bool ft_num_elem(Node *e) {
    (void)e;
    return false;
}

/* §187：表达式是否带「指针出处」污点——值（或经变量）源自指针→整数
 * 转换。算术运算不洗源（污点只进不出）；赋值重绑按右值重算。 */
static int expr_taint(Node *e) {
    if (!e) return 0;
    if (e->ptr_taint) return 1;
    if (e->kind == NK_IDENT && e->sym && e->sym->ptr_taint) return 1;
    return 0;
}

/* ---------------- §185 数组标签群 ----------------
 * 运行期索引写入的贴标槽按「数组基变量」聚合（取代 §174 的
 * rt_pending 断线拦截）：存入即登记进群，注解读取
 * （tag v : tag.x = xs[i];）查"∈ 群"后取出销账，free 整容器
 * 批量终结，没取走的由槽级泄漏终扫（E0513）兜底——群里留着的
 * 标签等同未 free。
 * 常索引条目（aesc 精确账）同样进群，free 整容器统一覆盖。
 * 别名切片（xs[0..n]、cast 往返）沿基址解析共享同一本账；
 * 解析不了的基址（指针别名等）保守回退 rt_pending 断线拦截。
 * 跨函数：形参数组的「整容器终结」事实（pffa）与「常索引元素
 * 释放」事实（pff 的 "N" 路径）在调用点/延迟重放里对账。 */
static void ft_mark_freed(int slot, Node *site);   /* 定义在字段账一节 */
static void agt_close(Symbol *base);               /* §185：定义在 pffa 一节 */
typedef struct { Symbol *base; int slot; bool taken; } AgtMem;
static AgtMem *g_agt;
static int g_nagt, g_cagt;

static void agt_add(Symbol *base, int slot) {
    if (!base || slot <= 0) return;
    for (int i = 0; i < g_nagt; i++)
        if (g_agt[i].base == base && g_agt[i].slot == slot) {
            g_agt[i].taken = false;     /* 取出后重新存入：重新登记 */
            return;
        }
    if (g_nagt == g_cagt) {
        int nc = g_cagt ? g_cagt * 2 : 16;
        AgtMem *p2 = realloc(g_agt, (size_t)nc * sizeof(AgtMem));
        if (!p2) return;
        g_agt = p2; g_cagt = nc;
    }
    g_agt[g_nagt].base = base;
    g_agt[g_nagt].slot = slot;
    g_agt[g_nagt].taken = false;
    g_nagt++;
}

static bool agt_has(Symbol *base, int slot) {
    for (int i = 0; i < g_nagt; i++)
        if (g_agt[i].base == base && g_agt[i].slot == slot)
            return !g_agt[i].taken;
    return false;
}

/* 这个数组存过贴标指针吗（读出断线判据，口径同 aesc_any）。 */
static bool agt_any(Symbol *base) {
    for (int i = 0; i < g_nagt; i++)
        if (g_agt[i].base == base) return true;
    return false;
}

static void agt_remove(Symbol *base, int slot) {
    /* 取出销账：成员保留（taken）——agt_any 不因取空而变假，
     * 后续再取同一标签才接得住 E0524；整容器终结走 agt_clear。 */
    for (int i = 0; i < g_nagt; i++)
        if (g_agt[i].base == base && g_agt[i].slot == slot)
            g_agt[i].taken = true;
}

/* 整容器终结：群内 LIVE 槽全标 FREED（free 事件现场）、群清空。
 * 常索引 aesc 条目同属这个容器——一并覆盖（E0509 由槽级 double-free
 * 检查管，FREED 槽跳过）。 */
static void agt_clear(Symbol *base, Node *site) {
    for (int i = 0; i < g_naesc; i++)
        if (g_aesc[i].base == base && g_aesc[i].slot > 0)
            ft_mark_freed(g_aesc[i].slot, site);
    for (int i = 0; i < g_nagt; ) {
        if (g_agt[i].base == base) {
            ft_mark_freed(g_agt[i].slot, site);
            g_agt[i] = g_agt[g_nagt - 1];
            g_nagt--;
        } else i++;
    }
    agt_close(base);
}

/* 数组/切片基址 → 基变量符号（别名视图共享基账）。IDENT 直接取；
 * 切片/铸型/解包沿基址剥；其余（指针算术、多级间接）解析不了返回
 * NULL——写入处回退 rt_pending 断线拦截。 */
static Symbol *agt_base_of(Node *e) {
    for (int g = 0; g < 8 && e; g++) {
        if (e->kind == NK_IDENT)
            return (e->sym && e->sym->kind != SK_CONST) ? e->sym : NULL;
        if (e->kind == NK_SLICE || e->kind == NK_CAST ||
            e->kind == NK_UNWRAP ||
            /* §201：`xs[0][i]` 里外层 [0] 取的是 [N]T 数组值（不是元素），
             * 要继续下钻才能找到数组基变量 xs，否则标签进不了群、终扫误报
             * E0513。旧语义 xs[0] 直接就是元素指针，这里从不需要下钻下标。 */
            e->kind == NK_INDEX) { e = e->a; continue; }
        return NULL;
    }
    return NULL;
}

/* §185 跨函数：「形参数组被整容器终结」事实账（体内 free(p) 且 p
 * 是数组/切片形参）——调用点把实参数组的群一起清掉（闭环：容器
 * 所有权随调用移交，调用方不再欠群内标签的释放）。 */
static Symbol **g_pffa;
static int g_npffa, g_cpffa;
/* §185：整容器终结过的基变量——终结后群里已无成员，注解读取的
 * "∈ 群"校验要靠这个标记挡住（取已终结容器的标签 = E0524）。 */
static Symbol **g_agtclosed;
static int g_nagtclosed, g_cagtclosed;
static void agt_close(Symbol *base) {
    if (!base) return;
    for (int i = 0; i < g_nagtclosed; i++)
        if (g_agtclosed[i] == base) return;
    if (g_nagtclosed == g_cagtclosed) {
        int nc = g_cagtclosed ? g_cagtclosed * 2 : 16;
        Symbol **p2 = realloc(g_agtclosed, (size_t)nc * sizeof(Symbol *));
        if (!p2) return;
        g_agtclosed = p2; g_cagtclosed = nc;
    }
    g_agtclosed[g_nagtclosed++] = base;
}
static bool agt_closed(Symbol *base) {
    for (int i = 0; i < g_nagtclosed; i++)
        if (g_agtclosed[i] == base) return true;
    return false;
}
static void pffa_add(Symbol *ps) {
    if (!ps) return;
    for (int i = 0; i < g_npffa; i++)
        if (g_pffa[i] == ps) return;
    if (g_npffa == g_cpffa) {
        int nc = g_cpffa ? g_cpffa * 2 : 16;
        Symbol **p2 = realloc(g_pffa, (size_t)nc * sizeof(Symbol *));
        if (!p2) return;
        g_pffa = p2; g_cpffa = nc;
    }
    g_pffa[g_npffa++] = ps;
}
static bool pffa_has(Symbol *ps) {
    for (int i = 0; i < g_npffa; i++)
        if (g_pffa[i] == ps) return true;
    return false;
}

/* 同一坐标只报一次（读账路径会被多个消费者走到）。 */
static bool tag_diag_once(int line, int col) {
    static struct { int line, col; } seen[64];
    static int nseen;
    for (int i = 0; i < nseen; i++)
        if (seen[i].line == line && seen[i].col == col) return false;
    if (nseen < 64) { seen[nseen].line = line; seen[nseen].col = col; nseen++; }
    return true;
}

/* 字段槽的释放事件：槽标 FREED（口径同 tag_free_event，调用点只在
 * 被调方确实 free 过时才走到这里），g_locals 上的 LIVE 载体同步。 */
static void ft_mark_freed(int slot, Node *site) {
    TagSlot *t = tag_slot(slot);
    if (!t || t->state != TST_LIVE) return;
    t->state = TST_FREED;
    t->ever_freed = true;
    if (site) { t->free_line = site->line; t->free_col = site->col; }
    for (Symbol *ls = g_locals; ls; ls = ls->locals_next)
        if (ls->tag_id == slot && ls->tag_state == TC_LIVE)
            ls->tag_state = TC_FREED;
}

/* 扫 return 的结构体值字段账本（剥铸型/解包），多路合并。 */
static void ft_scan_returns(Node *n, FieldTag **out) {
    if (!n || n->kind == NK_FN) return;
    if (n->kind == NK_RETURN && n->a) {
        Node *src = n->a;
        for (int g = 0; g < 6 && src &&
             (src->kind == NK_CAST || src->kind == NK_UNWRAP ||
              src->kind == NK_TRY); g++)
            src = src->kind == NK_CAST ? (src->b ? src->b : src->a) : src->a;
        FieldTag *fs = ftags_of_expr(src);
        if (fs) {
            if (!*out) *out = ft_clone(fs);
            else *out = ft_merge(*out, fs);
        }
        return;
    }
    ft_scan_returns(n->a, out);
    ft_scan_returns(n->b, out);
    ft_scan_returns(n->c, out);
    ft_scan_returns(n->d, out);
    for (int i = 0; i < n->nitems; i++) ft_scan_returns(n->items[i], out);
}

/* 表达式携带的 tag 载体：(id = TagSlot 下标+1, 载体状态)。id=0 = 不带 tag。
 * 顺序：节点自带的标记（贴标/合并/传播时填好）优先，ident 查符号，
 * 收割冒泡到值。 */
static void tag_of_expr(Node *e, int *id, int *state) {
    *id = 0; *state = TC_NONE;
    if (!e) return;
    if (e->tag_id) { *id = e->tag_id; *state = e->tag_state; return; }
    if (e->kind == NK_IDENT && e->sym && e->sym->tag_id) {
        *id = e->sym->tag_id; *state = e->sym->tag_state; return;
    }
    if (e->kind == NK_YIELD) { tag_of_expr(e->a, id, state); return; }
    /* §174 续链：`tag v : tag.x = old;` 降糖成 let——DECL 绑定从这里
     * 拿到续上的槽（case 里已守门：值无标签、槽 LIVE）。 */
    if (e->kind == NK_TAG_BIND && e->tag_id) {
        *id = e->tag_id; *state = TC_LIVE;
        return;
    }
    /* §173：`p.?`（未烙 COND→LIVE 印的普通解包）与切片 `.ptr` 沿基址追：
     * free 的实参常写成 `(*u8)s.ptr`，没有这条 free 事件就记不上账。 */
    if (e->kind == NK_UNWRAP && e->a) { tag_of_expr(e->a, id, state); return; }
    /* §179：切片是同一份内存的视图——p[0..] 携带基址的标签；cast 结果
     * 落在指针/切片家族时身份同样跟过去（节点烙印只盖指针→指针，切片
     * 家族在这里兜底；抽象借用槽不追）。 */
    if ((e->kind == NK_SLICE || e->kind == NK_CAST) && e->a && e->ty &&
        (e->ty->kind == TY_SLICE || e->ty->kind == TY_PTR) &&
        !(e->kind == NK_CAST && e->a->tag_id &&
          tag_slot(e->a->tag_id) && tag_slot(e->a->tag_id)->abstract)) {
        tag_of_expr(e->a, id, state);
        return;
    }
    /* §173 补丁十（收紧版）：常索引读精确元素账——`let q = xs[0][i];`
     * §201：只有**数组值**的下标才是元素访问；`*[N]T` 的 `[i]` / `.i`
     * 取的是第 i 个数组，不能按元素账处理。运行期索引：数组存过贴标
     * 指针就报 E0521（标签绝不静默丢失）。只继承 LIVE。 */
    bool idx_elem = e->kind == NK_INDEX && e->a &&
                    !(e->a->ty && ty_is_array_ptr(e->a->ty));
    if (idx_elem && e->a->kind == NK_IDENT &&
        e->a->sym && e->a->sym->kind != SK_CONST) {
        int ix = aesc_const_index(e->b);
        int aid = ix >= 0 ? aesc_find(e->a->sym, ix) : 0;
        if (ix < 0 && (aesc_any(e->a->sym) || agt_any(e->a->sym)) &&
            tag_diag_once(e->line, e->col))
            diag_code_at("E0521", e->line, e->col,
                "this element is read with a runtime index and the array "
                "holds tracked pointers -- which allocation an element "
                "carries cannot be identified; take it out with an "
                "annotated read (`tag v : tag.x = xs[i];`), or store at a "
                "constant index");
        if (aid && ft_state_of(aid) == TC_LIVE) {
            *id = aid; *state = TC_LIVE;
            return;
        }
    }
    /* §173：结构体字段携带的 tag —— `s.name` / `make().name`。符号基址
     * 查符号的字段表，表达式基址（调用/字面量）查节点字段表；铸型/
     * 解包先剥。补丁八：嵌套字段（s.inner.name）——字段链折叠成点
     * 路径 key（"inner.name"）查账，单层字段是深度 1 的特例。
     * §174：元组元素（t.0 / t[N] 改写成 name=NULL 的 NK_FIELD）走同
     * 一条点路径（数字分量）；同形状但基址是指针的是解引用，沿基址追。 */
    if (e->kind == NK_FIELD && e->a && (e->name ||
        (e->ival >= 0 && e->a->ty && e->a->ty->kind == TY_TUPLE))) {
        char key[128];
        Node *fb = NULL;
        if (ft_path_key(e, key, sizeof key, &fb)) {
            for (int g = 0; g < 6 && fb &&
                 (fb->kind == NK_CAST || fb->kind == NK_UNWRAP); g++)
                fb = fb->kind == NK_CAST ? (fb->b ? fb->b : fb->a) : fb->a;
            if (fb && fb->kind == NK_IDENT && fb->sym &&
                ft_member_of((FieldTag *)fb->sym->ftags, key, id, state))
                return;
            if (fb && ft_member_of((FieldTag *)fb->ftags, key, id, state))
                return;
            /* 前缀回退：账里有 "inner"（内层本身是贴标指针）而问的是
             * "inner.name"——`.name` 是对贴标指针的解引用，沿前缀追。 */
            char *dot = strrchr(key, '.');
            if (dot) {
                *dot = 0;
                if (fb && fb->kind == NK_IDENT && fb->sym &&
                    ft_member_of((FieldTag *)fb->sym->ftags, key, id, state))
                    return;
                if (fb && ft_member_of((FieldTag *)fb->ftags, key, id, state))
                    return;
            }
        }
    }
    if (e->kind == NK_FIELD && e->name && !strcmp(e->name, "ptr") && e->a) {
        tag_of_expr(e->a, id, state);
        return;
    }
    /* §174：(*p) 解引用（方法接收者自动补的 name=NULL NK_FIELD，基址
     * 是指针）：贴标指针经解引用身份不变。§201：数组指针的 `.N` 取数组，
     * 不是解引用也不是元素访问——上面已按精确账处理，这里不吞。 */
    if (e->kind == NK_FIELD && !e->name && e->a && e->a->ty &&
        e->a->ty->kind == TY_PTR && !e->a->ty->has_len &&
        !ty_is_array_ptr(e->a->ty)) {
        tag_of_expr(e->a, id, state);
        return;
    }
}

/* 定义 / 查重。§172：tag 名字是全局唯一键（模仿 error 名字分配码）：
 * 首次引用（tag.buf）即自动建槽，没有声明语句，同名幂等不报重复。 */
static TagSlot *tag_decl(const char *name, Node *n) {
    TagSlot *ex = tag_by_name(name);
    if (ex) return ex;
    if (g_ntslots == g_ctslots) {
        int nc = g_ctslots ? g_ctslots * 2 : 16;
        TagSlot *p = realloc(g_tslots, (size_t)nc * sizeof(TagSlot));
        if (!p) { err_at(n->line, n->col, "out of memory"); return NULL; }
        g_tslots = p; g_ctslots = nc;
    }
    TagSlot *t = &g_tslots[g_ntslots++];
    memset(t, 0, sizeof *t);
    t->name = name;
    t->line = n->line; t->col = n->col;
    /* §176：分配事实归谁调的——建槽即记当下函数与文件（槽幂等，
     * 首次建槽的上下文就是分配点的上下文） */
    t->def_fn = g_cur_fn_name;
    t->file = (g_mod && g_mod->path) ? g_mod->path : NULL;
    return t;
}

static int g_tag_auto_seq;
static bool tag_module_exempt(Module *m);   /* §173：定义在贴标登记一节 */
static char *path_normalize(const char *p); /* §189：定义在模块加载一节 */
static bool find_std_path(char *out, size_t cap);

/* §173：alloc 自动建槽。每个分配调用点自动获得唯一标签并预绑 LIVE——
 * `tag.x : expr` 只剩「给分配起名」的作用。tag.close / std 豁免照旧。 */
static void tag_auto_decl(Node *call, bool stack) {
    if (call->tag_id) return;                       /* 重扫幂等 */
    if (g_mod && (g_mod->tags_closed || tag_module_exempt(g_mod))) return;
    /* 槽名必须是可词法的标识符：手动标记 `tag.allocN : null;` 要能
     * 在源码里引用自动槽（alloc#N 的 # 写不进源码）。撞名则跳号。 */
    char namebuf[32];
    do {
        snprintf(namebuf, sizeof namebuf, "alloc%d", ++g_tag_auto_seq);
    } while (tag_by_name(namebuf));
    char *nm = strdup(namebuf);
    if (!nm) return;
    Node tmp; memset(&tmp, 0, sizeof tmp);
    tmp.line = call->line; tmp.col = call->col;
    TagSlot *t = tag_decl(nm, &tmp);
    if (!t) return;
    t->auto_n = true;
    t->stack = stack;
    t->state = TST_LIVE;
    t->region = stack ? 0 : call->reg_slot;
    if (!stack && call->reg_slot > 0 && call->reg_slot <= g_nrslots)
        g_rslots[call->reg_slot - 1].tag = (int)(t - g_tslots) + 1;
    call->tag_id = (int)(t - g_tslots) + 1;
    call->tag_state = TC_LIVE;
}

/* §175：调用点槽实例化。工厂函数（分配经返回流转出所有权）每次调用
 * 都产生一份新的运行期分配——所有调用点共享同一个静态槽会把第二次
 * free 假报成 E0509。克隆槽继承分配事实（allocated at 分配行），泄漏
 * 归属点记在调用行；母槽一旦派生实例就退役（终扫由实例接管）。
 * 注意 tag_decl 可能 realloc 槽表——先存母槽索引再回取。 */
static int tag_clone_site(TagSlot *src, Node *call, bool freed) {
    int sidx = (int)(src - g_tslots) + 1;
    char namebuf[32];
    do {
        snprintf(namebuf, sizeof namebuf, "alloc%d", ++g_tag_auto_seq);
    } while (tag_by_name(namebuf));
    char *nm = strdup(namebuf);
    if (!nm) return 0;
    Node tmp; memset(&tmp, 0, sizeof tmp);
    tmp.line = call->line; tmp.col = call->col;
    TagSlot *t = tag_decl(nm, &tmp);
    if (!t) return 0;
    t->auto_n = true;
    t->state = freed ? TST_FREED : TST_LIVE;
    t->ever_freed = freed || src->ever_freed;
    t->inst_of = sidx;
    t->stack = src->stack;             /* 文案/作用域语义随母槽 */
    t->scope_freed = src->scope_freed;
    if (freed) { t->free_line = src->free_line; t->free_col = src->free_col; }
    src = tag_slot(sidx);
    if (src) src->ninst++;
    t->line = src ? src->line : call->line;        /* 分配事实留在分配行 */
    t->col = src ? src->col : call->col;
    t->def_fn = src ? src->def_fn : t->def_fn;     /* §176：分配点随母槽 */
    t->file = src ? src->file : t->file;
    t->own_fn = g_cur_fn_name;                     /* 归属调用点所在函数 */
    t->own_line = call->line;                      /* 归属点在调用行 */
    t->own_col = call->col;
    return (int)(t - g_tslots) + 1;
}

/* §194：实参载体字段账的可写指针（store 事实镜像的落点）。剥 cast/
 * unwrap 到 IDENT 用符号的字段账，否则退回调用节点的字段账。 */
static FieldTag **ft_arg_ftags(Node *arg) {
    Node *e = arg;
    for (int g = 0; g < 6 && e &&
         (e->kind == NK_CAST || e->kind == NK_UNWRAP); g++)
        e = e->kind == NK_CAST ? (e->b ? e->b : e->a) : e->a;
    if (e && e->kind == NK_IDENT && e->sym)
        return (FieldTag **)&e->sym->ftags;
    return arg ? (FieldTag **)&arg->ftags : NULL;
}

/* §194/195：调用点镜像形参 store 事实——被调体往形参容器字段写入的指针，
 * 烙到实参载体的字段账对应路径；调用方读回该字段（t1[0].name）就拿到槽、
 * 释放有着落。与 pff（体内 free → 调用点标 FREED）方向相反、机制对称。
 * ① 本体内新分配 → tag_clone_site 实例化（每次调用一份），母槽 ninst>0
 *    终扫退役；② §195 透传的抽象形参槽 → 把「来源形参对应的实参」自身的
 *    槽烙进容器实参字段账（不 clone，所有权本在调用方）。 */
static void pfs_mirror_arg(Symbol *ps, Node *arg, Node *call, Node *fn) {
    if (!ps || !arg) return;
    for (int i = 0; i < g_npfs; i++) {
        if (g_pfs[i].psym != ps) continue;
        int nid;
        if (g_pfs[i].src_param && fn) {
            /* §195：找来源形参在被调函数形参表中的下标，取调用点同位置
             * 实参自身的槽（它不是本体内新分配，不能 clone）。 */
            int src_idx = -1;
            for (int j = 0; j < fn->nitems; j++)
                if (fn->items[j]->sym == g_pfs[i].src_param) { src_idx = j; break; }
            if (src_idx < 0 || src_idx >= call->nitems) continue;
            int atid, ast;
            tag_of_expr(call->items[src_idx], &atid, &ast);
            if (!atid || ast != TC_LIVE) continue;
            nid = atid;
        } else {
            TagSlot *src = tag_slot(g_pfs[i].slot);
            if (!src || src->abstract) continue;
            nid = tag_clone_site(src, call, src->state == TST_FREED);
            if (!nid) continue;
        }
        FieldTag **dst = ft_arg_ftags(arg);
        if (dst) {
            char *pk = ft_path_dup(NULL, g_pfs[i].key);
            if (pk) ft_set(dst, pk, nid);
        }
    }
}

/* §173：离开作用域 —— stackAlloc 的槽自动标 free（栈存储随帧消亡）。
 * 载体被存进容器/传出去之后再用，撞上的是 E0512（追踪不到容器里那份，
 * 但同一符号的 outer 副本还能追到）。 */static void tag_scope_exit(Scope *sc, Node *n) {
    Node *tail = (n->nitems > 0 && n->items[n->nitems - 1])
                 ? n->items[n->nitems - 1] : n;   /* 块的 line 是起点行 */
    for (Symbol *s = sc->first; s; s = s->next) {
        if (!s->tag_id || s->tag_state != TC_LIVE) continue;
        TagSlot *t = tag_slot(s->tag_id);
        if (!t || !t->stack || t->state != TST_LIVE) continue;
        t->state = TST_FREED;
        t->free_line = tail->line; t->free_col = tail->col;
        t->scope_freed = true;
        s->tag_state = TC_FREED;
        if (t->carrier) t->carrier->tag_state = TC_FREED;
    }
    /* §173 补丁八：本作用域期间创建的栈分配槽统一回收——贴标指针
     * 存进结构体字段/数组元素时没有直接载体符号，按符号扫会漏，
     * 槽就会在终扫被误报成堆泄漏（栈存储随帧消亡，谈不上泄漏）。 */
    for (int i = sc->tag_wm; i < g_ntslots; i++) {
        TagSlot *t = &g_tslots[i];
        if (!t->stack || t->abstract || t->state != TST_LIVE) continue;
        t->state = TST_FREED;
        t->free_line = tail->line; t->free_col = tail->col;
        t->scope_freed = true;
        if (t->carrier) t->carrier->tag_state = TC_FREED;
    }
}

/* §172：按名字解析 tag 槽 —— 先查槽位表（已建槽），再查同名 const 别名
 * （const b2 = tag.buf; 之后 `tag.b2` 也指到 buf 的槽，避免按名误建新标签），
 * 都没有就自动建槽（引用即声明）。 */
static TagSlot *tag_resolve(Node *n) {
    TagSlot *t = tag_by_name(n->name);
    if (!t) {
        Symbol *s = lookup(n->name);
        if (s) s->used = true;              /* 别名引用算使用 */
        if (s && s->tag_id) t = tag_slot(s->tag_id);
    }
    if (t) return t;
    return tag_decl(n->name, n);
}

/* 解引用点检查：base 带贴标载体时按状态拦（use-after-free / 未防守）。 */
static void tag_check_deref(Node *base) {
    int id, st;
    tag_of_expr(base, &id, &st);
    pderef_note(base, id);          /* §180：记下"形参被解引用"事实 */
    if (!id) return;
    TagSlot *t = tag_slot(id);
    if (!t) return;
    if (st == TC_FREED) {
        if (t->scope_freed)
            diag_code_at("E0512", base->line, base->col,
                "use after scope: this pointer is stack storage whose block "
                "ended at line %d -- stack memory dies with its scope, the "
                "pointer must not outlive it", t->free_line);
        else
            diag_code_at("E0512", base->line, base->col,
                "use after free: this memory was released at line %d -- a "
                "freed pointer must not be dereferenced; remove this use or "
                "move it before the free", t->free_line);
        return;
    }
    if (st == TC_UNKNOWN) {
        diag_code_at("E0515", base->line, base->col,
            "it is undetermined whether this memory was freed (the branches "
            "disagree) -- defend before use: re-bind it (`p = tag.%s : "
            "p.?;` asserts it is live) or `free` it", t->name);
        return;
    }
    /* §189：所有权已转移的载体不能再用——内存活着，但已不归本作用域。 */
    if (st == TC_MOVED) {
        diag_code_at("E0529", base->line, base->col,
            "use after move: this pointer's ownership was moved away at "
            "line %d (`std.move`) -- a moved pointer is neither "
            "dereferenceable nor freeable here", t->moved_line);
        return;
    }
    /* COND：解引用必须先判空（`.?`），这是类型系统的事，类型层拦。 */
}

/* 沿载体传播链（cast / unwrap / 收割）找到底层标识符符号：
 * free 的状态要写回真正的载体变量，后续解引用才查得到。 */
static Symbol *tag_carrier_sym(Node *e) {
    while (e) {
        if (e->kind == NK_IDENT) return e->sym;
        if (e->kind == NK_CAST || e->kind == NK_UNWRAP || e->kind == NK_YIELD)
            e = e->b ? e->b : e->a;
        else return NULL;
    }
    return NULL;
}

/* 一次释放事件（Allocator.free 直调 / 释放器调用 / 臂内 free）。
 * 返回 true = 这次 free 被记录为该 tag 的释放。 */
static bool tag_free_event(Node *site, Node *ptr) {
    int id, st;
    /* §187：释放器调用可以经取址传容器（`run(&pp)`，run 体内 deinit）
     * ——取址不改变身份，剥掉 & 追到基变量。只有释放器调用走这里，
     * 借用型 `foo(&p)` 不受影响。 */
    Node *p2 = ptr;
    for (int g = 0; g < 6 && p2 && p2->kind == NK_UN &&
         p2->op == TOK_AMP; g++) p2 = p2->a;
    tag_of_expr(p2, &id, &st);
    if (!id) return false;
    if (p2->kind == NK_NULL) return false;   /* free(null)：no-op */
    TagSlot *t = tag_slot(id);
    if (!t) return false;
    if (t->state == TST_FREED) {
        if (t->free_line == site->line && t->free_col == site->col) return true;
        if (t->scope_freed)
            diag_code_at("E0509", site->line, site->col,
                "this pointer is stack storage that already died with its "
                "scope (allocated at line %d, scope ended at line %d) -- the "
                "pointer escaped its frame, so releasing it again is a "
                "double free", t->line, t->free_line);
        else
            diag_code_at("E0509", site->line, site->col,
                "this memory was already freed (allocated at line %d, first "
                "freed at line %d) -- releasing it again is a double free",
                t->line, t->free_line);
        return true;
    }
    /* §189：所有权已随 std.move 转移——free 它等于对别的线程/作用域
     * 正在使用的内存下手，直接拦。 */
    if (t->state == TST_MOVED) {
        diag_code_at("E0529", site->line, site->col,
            "this pointer was moved away at line %d (`std.move`) -- the "
            "receiver owns it now; freeing it here is use after move",
            t->moved_line);
        return true;
    }
    /* UNKNOWN / COND：防守二（使用者宣告终结）与 null 合并载体的放行。
     * 悬垂风险由「free 后必须传 null」的分支约定承担。 */
    if (t->adopt_ar) {
        /* §187：已被 arena 收养——内存由 arena 的移交表管，deinit 时
         * 统一归还；这里再 free = deinit 时双重释放。 */
        diag_code_at("E0526", site->line, site->col,
            "this pointer is owned by an arena (adopted at line %d) -- it "
            "is freed when that arena deinits; freeing it here would be a "
            "double free", t->adopt_line);
        return true;
    }
    t->state = TST_FREED;
    t->ever_freed = true;
    t->free_line = site->line; t->free_col = site->col;
    ptr->tag_id = id; ptr->tag_state = TC_FREED;
    Symbol *cs = tag_carrier_sym(p2);
    if (cs) { cs->tag_state = TC_FREED; t->carrier = cs; }
    /* §173：副本同步 —— free(q) 之后，所有携带同一 tag 的局部载体
     * （`var q2 = q;` 之类）一起进 FREED，不然旧副本还能解引用。 */
    for (Symbol *ls = g_locals; ls; ls = ls->locals_next)
        if (ls->tag_id == id && ls->tag_state == TC_LIVE)
            ls->tag_state = TC_FREED;
    return true;
}

/* Allocator.free(ctx, ptr, size) 形状的释放事件（贴标指针走 tag 账本，
 * 不进 region 账本——同一份事实记两遍会双报 E0509）。 */
static void tag_note_free(Node *call) {
    Node *a = call->a;
    if (!a || a->kind != NK_FIELD || !a->name || strcmp(a->name, "free") ||
        !a->a || !is_allocator_ty(a->a->ty) || call->nitems != 3) return;
    /* §173：形参字段被 free 的事实账（见 pff_add 注释）。 */
    {
        Node *pa = call->items[1];
        for (int g = 0; g < 6 && pa &&
             (pa->kind == NK_CAST || pa->kind == NK_UNWRAP); g++)
            pa = pa->kind == NK_CAST ? (pa->b ? pa->b : pa->a) : pa->a;
        if (pa && pa->kind == NK_FIELD && pa->name && pa->a &&
            pa->a->kind == NK_IDENT && pa->a->sym &&
            pa->a->sym->kind == SK_PARAM)
            pff_add(pa->a->sym, pa->name);
    }
    /* §185：整容器终结——free 的实参是数组/切片基变量且群非空：
     * 群内 LIVE 槽全部标 FREED、群清空（声明语义：容器所有权到此
     * 终结，群内标签的泄漏责任一并了结；逐元素真释放按 decisions
     * §185 在 free 之前显式取出）。实参是形参时记 pffa 事实账，
     * 调用点/延迟重放把实参数组的群一起清（闭环）。 */
    /* §185（用户裁定）：free 整容器**不**批量清群——数组与元素
     * 不绑定：数组存储释放了、元素没释放，把它们标 FREED 会把真
     * 泄漏错判成已释放。整容器终结只有显式出口 `tag xs : null;`
     * （终结语句里清群 + 记 pffa 事实账），或逐个取出释放。 */
    tag_free_event(call, call->items[1]);
}

/* §187：arena 收养账。`ArenaAllocator.adopt(a, p, n)` 把 p 的所有权移
 * 交给 arena 变量 a：槽保持 LIVE（内存活到 deinit，可解引用、可注解
 * 读出），记入 a 的收养表；`ArenaAllocator.deinit(a)` 总清算时统一
 * 终结表内全部槽（标 FREED，报在 deinit 行）。运行时由 arena 自己的
 * 移交表在 deinit 里归还 backing（见 std/arena.zx adopt/deinit）。
 * v1 限制：arena 实参必须是可解析的局部基变量（形参句柄不入账）。 */
typedef struct { Symbol *ar; int slot; int line, col; bool done; } ArAdopt;
static ArAdopt *g_aradt;
static int      g_naradt, g_caradt;

/* 实参剥 cast/unwarp/取址到基变量符号（arena 句柄）。形参不入账（跨函数
 * 的 arena 句柄在调用点才落定，重放机制 v1 不覆盖收养账）。 */
static Symbol *aradt_base_of(Node *e) {
    for (int g = 0; g < 6 && e &&
         (e->kind == NK_CAST || e->kind == NK_UNWRAP ||
          (e->kind == NK_UN && e->op == TOK_AMP)); g++)
        e = e->kind == NK_CAST ? (e->b ? e->b : e->a)
            : e->kind == NK_UN ? e->a : e->a;
    return (e && e->kind == NK_IDENT && e->sym &&
            e->sym->kind != SK_CONST && e->sym->kind != SK_PARAM)
           ? e->sym : NULL;
}

static ArAdopt *aradt_find(int slot) {
    for (int i = 0; i < g_naradt; i++)
        if (g_aradt[i].slot == slot) return &g_aradt[i];
    return NULL;
}

/* `ArenaAllocator.adopt(a, p, n)` —— 所有权移交事件。 */
static void tag_note_adopt(Node *call) {
    Node *a = call->a;
    if (!a || a->kind != NK_FIELD || !a->name || strcmp(a->name, "adopt") ||
        call->nitems < 2) return;
    Symbol *ar = aradt_base_of(call->items[0]);
    if (!ar) return;
    int id, st;
    tag_of_expr(call->items[1], &id, &st);
    if (!id) return;                    /* 无标签指针：无需记账 */
    TagSlot *t = tag_slot(id);
    if (!t) return;
    ArAdopt *ent = aradt_find(id);
    if (ent) {
        if (ent->done) return;          /* 复检重放：已随 deinit 终结 */
        if (t->adopt_ar) {
            diag_code_at("E0526", call->line, call->col,
                "this pointer is already owned by an arena (adopted at line "
                "%d) -- a second adopt would make the arena free it twice",
                t->adopt_line);
        }
        return;
    }
    if (t->state == TST_FREED) {
        diag_code_at("E0509", call->line, call->col,
            "this memory was already freed (allocated at line %d, first "
            "freed at line %d) -- it cannot be adopted by an arena",
            t->line, t->free_line);
        return;
    }
    if (t->adopt_ar) {
        diag_code_at("E0526", call->line, call->col,
            "this pointer is already owned by an arena (adopted at line "
            "%d) -- a second adopt would make the arena free it twice",
            t->adopt_line);
        return;
    }
    if (g_naradt == g_caradt) {
        int nc = g_caradt ? g_caradt * 2 : 16;
        ArAdopt *p2 = realloc(g_aradt, (size_t)nc * sizeof(ArAdopt));
        if (!p2) return;
        g_aradt = p2; g_caradt = nc;
    }
    g_aradt[g_naradt].ar = ar;
    g_aradt[g_naradt].slot = id;
    g_aradt[g_naradt].line = call->line;
    g_aradt[g_naradt].col = call->col;
    g_aradt[g_naradt].done = false;
    g_naradt++;
    t->adopt_ar = ar;
    t->adopt_line = call->line;
    t->adopt_col = call->col;
}

static bool fn_is_releaser(Node *fn);   /* §173：体未查时的释放器近似 */

/* §188（用户裁定）：建槽的调用入口是 allocator.alloc()，free 的入口是
 * allocator.free()——所有分配/释放都经过这对接口，统一管理。豁免分两层，
 * 由实现模块各自声明：
 *   `tag.close;`  —— 模块内部申请不建槽（pool 的块头裸指针运算这类
 *                    实现细节不进账本）；
 *   `tag.batch;`  —— 分配器是整批清算型（arena）：经它的 Allocator
 *                    视图在调用点做的 alloc/free 一并豁免，内存由
 *                    init/deinit 容器账管（pool 不声明它，用户侧
 *                    alloc/free 照常逐笔追踪）。
 * 判定只看 batch：close 的模块内部申请走的是调用点自己的 g_mod 豁免。 */
static Symbol *allocator_target(Node *e, bool want_alloc, int depth); /* 定义在 devirt 一节 */

/* §188：这一笔调用是不是「释放」。口径与 tag_block_has_free 一致：
 * deinit 变换产物、Allocator.free 方法、用户释放器封装（体内有
 * free/deinit 的函数，释放器传递）。 */
static bool call_is_release(Node *call) {
    if (call->xform_free) return true;
    if (call->a && call->a->kind == NK_FIELD && call->a->name &&
        !strcmp(call->a->name, "free") && call->a->a &&
        (!call->a->a->ty || is_allocator_ty(call->a->a->ty)))
        return true;
    if (call->a && call->a->kind == NK_IDENT && call->a->sym &&
        call->a->sym->decl && call->a->sym->decl->kind == NK_FN &&
        fn_is_releaser(call->a->sym->decl))
        return true;
    return false;
}

/* 模块路径 → Module（豁免判定用；Node->mod 是装载时打的路径戳）。 */
static Module *module_by_path(const char *path) {
    if (!path) return NULL;
    for (int i = 0; i < g_nmods; i++)
        if (g_mods[i] && g_mods[i]->path && !strcmp(g_mods[i]->path, path))
            return g_mods[i];
    return NULL;
}

static Module *alloc_factory_module(Node *e, int depth);   /* 定义在下面 */

static bool allocator_exempt(Node *alarg) {
    Symbol *t = allocator_target(alarg, true, 0);
    if (t && t->mod) return t->mod->tags_batch;   /* §188：批量清算才豁免视图 */
    /* 目标符号还没绑定（工厂体未查，Allocator 字面量里的实现函数符号
     * 是空的）——退一步看工厂函数自己的模块：init 的 inst 节点在模块
     * 装载时就带模块戳，不依赖体已查。 */
    Module *m = alloc_factory_module(alarg, 0);
    return m && m->tags_batch;
}

/* 沿分配器值的声明链找产出它的工厂函数，返回其所在模块（只认模块，
 * 不解析实现符号——避开"体未查、符号未绑"的时序）。 */
static Module *alloc_factory_module(Node *e, int depth) {
    if (!e || depth > 6) return NULL;
    switch (e->kind) {
    case NK_IDENT: {
        Symbol *s = e->sym;
        if (!s || s->nassign != 0 || s->escaped) return NULL;
        if (s->cap_src) return alloc_factory_module(s->cap_src, depth + 1);
        if (!s->decl || s->decl->kind != NK_DECL || !s->decl->b) return NULL;
        return alloc_factory_module(s->decl->b, depth + 1);
    }
    case NK_CAST:
        return alloc_factory_module(e->b, depth + 1);
    case NK_UNWRAP:
    case NK_TRY:
        return alloc_factory_module(e->a, depth + 1);
    case NK_CALL: {
        Node *callee = e->a;
        Node *fn = NULL;
        if (callee && callee->inst && callee->inst->kind == NK_FN)
            fn = callee->inst;
        else if (callee && callee->sym && callee->sym->decl) {
            Node *d = callee->sym->decl;
            if (d->kind == NK_FN) fn = d;
            else if (d->kind == NK_DECL && d->b && d->b->kind == NK_FN) fn = d->b;
        }
        return (fn && fn->mod) ? module_by_path(fn->mod) : NULL;
    }
    default:
        return NULL;
    }
}

/* §187（用户裁定，通用规则）：建槽只有一个入口——std.heapAlloc /
 * std.heapSlice / std.stackAlloc 内建检查处，进了就建，不按容器/模块
 * 点名豁免。Allocator 接口的 alloc/free 函数指针是实现细节（std 实现
 * 层内部才允许直接碰，用户代码 E0527）。arena 这类「整批管理」的
 * 分配器，其内存的账由两条通用机制接住：
 *   1. 容器账——std 内部模块的 `.init` 调用点自动开账（不点名哪个
 *      容器：std 里叫 init 的构造器都是要 deinit 的资源，新增容器
 *      自动获得泄漏检测），`.deinit` 统一终结；
 *   2. 收养——从带容器账的分配器值里分配出来的槽，当场收养给那个
 *      分配器变量（值级规则，跟容器名无关），随它的 deinit 一起终结。 */
static bool path_is_std_internal(const char *path) {
    if (!path) return false;
    if (g_std_mod && g_std_mod->path && !strcmp(g_std_mod->path, path))
        return true;
    if (g_std_mod && g_std_mod->path) {            /* std 目录下的内部文件 */
        const char *a = strrchr(g_std_mod->path, '/');
        const char *b = strrchr(path, '/');
        const char *la = strrchr(g_std_mod->path, '\\');
        const char *lb = strrchr(path, '\\');
        if (la && (!a || la > a)) a = la;
        if (lb && (!b || lb > b)) b = lb;
        if (a && b && (a - g_std_mod->path) == (b - path) &&
            !memcmp(g_std_mod->path, path, (size_t)(a - g_std_mod->path)))
            return true;
    }
    return false;
}

/* §187：std 资源容器的 init 是容器账开账点——通用规则：被调 init
 * 函数属于 std 内部模块即开账（不点名容器）。调用点在 std 内部的
 * 照旧豁免（实现层内部互相初始化不该记账）。 */
static void tag_note_container_init(Node *call) {
    Node *a = call->a;
    if (!a || a->kind != NK_FIELD || !a->name || strcmp(a->name, "init"))
        return;
    if (g_mod && tag_module_exempt(g_mod)) return;
    Node *fn = NULL;
    if (a->inst && a->inst->kind == NK_FN) fn = a->inst;
    else if (a->sym && a->sym->decl) {
        Node *d = a->sym->decl;
        if (d->kind == NK_FN) fn = d;
        else if (d->kind == NK_DECL && d->b && d->b->kind == NK_FN) fn = d->b;
    }
    if (!fn || fn->kind != NK_FN || !path_is_std_internal(fn->mod)) return;
    /* §187 补丁：资源账只开给"从分配器取得资源"的容器——init 实参里
     * 必须有 Allocator（List/Arena 是）；定长 Array.init()（内联存储）、
     * net.init()（全局态）这类不碰分配器的初始化不是资源，不开账。 */
    bool has_al = false;
    for (int i = 0; i < call->nitems; i++) {
        Node *ai = call->items[i];
        if (ai && ai->ty && ty_is_allocator(ai->ty)) { has_al = true; break; }
    }
    if (!has_al) return;
    if (!call->tag_id) tag_auto_decl(call, false);
    TagSlot *t = tag_slot(call->tag_id);
    if (t) t->book = true;
}

/* `X.deinit(a)` / `a.deinit()` —— 总清算：终结容器账 + a 名下全部
 * 收养槽。识别口径：callee 尾段名为 deinit、第一实参（方法调用含
 * 接收者）解析到基变量；没有容器账/收养账的 deinit 调用这里是
 * no-op，不干扰其他 deinit。 */
static void tag_note_arena_deinit(Node *call) {
    Node *a = call->a;
    if (!a || a->kind != NK_FIELD || !a->name || strcmp(a->name, "deinit") ||
        call->nitems < 1) return;
    Symbol *ar = aradt_base_of(call->items[0]);
    if (!ar) return;
    /* §187：容器账终结。同点重放（体复检）静默；不同点的二次 deinit
     * 才是 double free（E0509）。 */
    if (ar->tag_id) {
        TagSlot *ct = tag_slot(ar->tag_id);
        if (ct && ct->book) {
            if (ct->state == TST_FREED) {
                if (ct->free_line != call->line || ct->free_col != call->col)
                    diag_code_at("E0509", call->line, call->col,
                        "this resource (`%s`) was already torn down (deinit at "
                        "line %d) -- a second deinit is a double free",
                        ar->name ? ar->name : "?", ct->free_line);
            } else {
                ct->state = TST_FREED;
                ct->ever_freed = true;
                ct->free_line = call->line; ct->free_col = call->col;
                for (Symbol *ls = g_locals; ls; ls = ls->locals_next)
                    if (ls->tag_id == ar->tag_id &&
                        ls->tag_state == TC_LIVE)
                        ls->tag_state = TC_FREED;
            }
        }
    }
    for (int i = 0; i < g_naradt; i++) {
        if (g_aradt[i].ar == ar && !g_aradt[i].done) {
            TagSlot *t = tag_slot(g_aradt[i].slot);
            if (t && t->state == TST_LIVE && t->adopt_ar == ar) {
                t->state = TST_FREED;
                t->ever_freed = true;
                t->free_line = call->line; t->free_col = call->col;
                t->adopt_ar = NULL;
                /* 副本同步（口径同 tag_free_event）：局部载体一起进
                 * FREED，旧副本解引用立即 E0512。 */
                for (Symbol *ls = g_locals; ls; ls = ls->locals_next)
                    if (ls->tag_id == g_aradt[i].slot &&
                        ls->tag_state == TC_LIVE)
                        ls->tag_state = TC_FREED;
            }
            g_aradt[i].done = true;    /* 复检重放幂等 */
        }
    }
}

/* ---------------- §189：std.move —— 所有权转移 ----------------
 *
 * 用户裁定，语义很简单：
 *   `let/var newvar = std.move(oldvar);`
 * 1. oldvar 的标签改 MOVED——之后对 oldvar 的任何使用或 free 都报
 *    E0529（内存还活着，只是不再归 oldvar 管）；
 * 2. 生成新变量 newvar：新标签、值等于原值。新槽就是普通 LIVE 槽，
 *    不 free 照样报 E0513，没有任何豁免。
 * 识别口径与 print/println 一致：裸名 `std.move(...)`；`const s = std;
 * s.move(...)` 别名不改道。spawn 不做自动 move（用户裁定：当普通函数）。 */

static bool call_is_std_move(Node *call) {
    return call->a && call->a->kind == NK_FIELD && call->a->name &&
           !strcmp(call->a->name, "move") && call->a->a &&
           call->a->a->kind == NK_IDENT && call->a->a->name &&
           !strcmp(call->a->a->name, "std");
}

/* 检查并落账一次 std.move。返回值 = 转移后的类型（实参类型原样）。 */
static Type *check_std_move(Node *call) {
    Node *arg = call->nitems == 1 ? call->items[0] : NULL;
    if (!arg) {
        err_at(call->line, call->col,
               "std.move takes exactly one argument -- the tagged pointer "
               "whose ownership moves out");
        return ty_error();
    }
    Type *t = check_expr(arg, NULL);
    int id, st;
    tag_of_expr(arg, &id, &st);
    if (!id) {
        diag_code_at("E0530", call->line, call->col,
            "std.move needs a pointer the tag ledger tracks -- only "
            "allocations bound to a tag (a std.heapAlloc/std.stackAlloc "
            "site or `tag.x : ...`) carry ownership to move; a bare "
            "pointer has no owner to transfer");
        return t;
    }
    TagSlot *t2 = tag_slot(id);
    if (st == TC_FREED) {
        diag_code_at("E0512", call->line, call->col,
            "this memory was freed at line %d -- moving a freed pointer "
            "is use after free", t2 ? t2->free_line : 0);
        return t;
    }
    if (st == TC_MOVED) {
        diag_code_at("E0529", call->line, call->col,
            "this pointer was already moved at line %d (`std.move`) -- a "
            "moved pointer is neither usable nor movable again",
            t2 ? t2->moved_line : 0);
        return t;
    }
    if (t2) {
        t2->state = TST_MOVED;
        t2->moved_line = call->line; t2->moved_col = call->col;
    }
    /* 载体同步：所有 LIVE 副本一起进 MOVED（口径同 free 的副本同步），
     * 旧副本解引用立即 E0529。 */
    for (Symbol *ls = g_locals; ls; ls = ls->locals_next)
        if (ls->tag_id == id && ls->tag_state == TC_LIVE)
            ls->tag_state = TC_MOVED;
    arg->tag_id = id; arg->tag_state = TC_MOVED;
    /* 返回值带新槽：`let/var newvar = std.move(oldvar);` —— newvar 从
     * 声明起就有自己的标签（普通 LIVE 槽，值等于原值；不 free 照样报
     * E0513，没有任何豁免——用户裁定）。 */
    tag_auto_decl(call, false);
    return t;
}

/* §189：spawn 的 ctx 实参不做自动 move（用户裁定：spawn 当普通函数）。
 * 多线程移交走显式 `std.move`：`g.spawn(work, (usize)std.move(p))` ——
 * 老槽 MOVED（p 封死），move 产生的新槽是普通 LIVE 槽（泄漏照报），
 * 接收方建自己的账（`tag q : (*u8)(ctx)` 建账贴标）。 */

/* 释放器识别：fn 体里直接调用过 Allocator.free（不含嵌套 fn 体）。
 * 结果缓存在 fn 节点上（tag_id: 0=未扫 1=是 2=不是）——调用点把带 tag
 * 的实参视作一次释放事件，用户的 free 封装不需要任何额外语法。 */
static bool tag_block_has_free(Node *n) {
    if (!n) return false;
    if (n->kind == NK_FN) return false;               /* 嵌套函数体不算 */
    if (n->kind == NK_CALL && n->a && n->a->kind == NK_FIELD &&
        n->a->name && !strcmp(n->a->name, "free") && n->a->a &&
        (!n->a->a->ty || is_allocator_ty(n->a->a->ty))) return true;
    /* §173：释放器传递 —— 调用的也是释放器函数同样算（A 包 B，B 里
     * free；调用 A 的调用点也要把真实槽标 freed）。 */
    if (n->kind == NK_CALL && n->a && n->a->kind == NK_IDENT &&
        n->a->sym && n->a->sym->decl && n->a->sym->decl->kind == NK_FN &&
        fn_is_releaser(n->a->sym->decl)) return true;
    if (tag_block_has_free(n->a) || tag_block_has_free(n->b) ||
        tag_block_has_free(n->c) || tag_block_has_free(n->d)) return true;
    for (int i = 0; i < n->nitems; i++)
        if (tag_block_has_free(n->items[i])) return true;
    return false;
}

static bool fn_is_releaser(Node *fn) {
    if (!fn || fn->kind != NK_FN) return false;
    if (fn->tag_id == 1) return true;
    if (fn->tag_id == 2 || fn->tag_id == 3) return false;
    fn->tag_id = 3;                       /* 扫描中：互递归按「不是」处理 */
    bool r = tag_block_has_free(fn->b);
    fn->tag_id = r ? 1 : 2;
    return r;
}

/* ---------------- §173 返回流：被调方把 tag 原样吐回来 ----------------
 *
 * 两种形态都继承到调用点：
 *   - 透传：`fn id(p: *T) *T { return p; }` —— 调用结果携带实参的 tag；
 *   - 工厂：`fn make() *T { var p = std.heapAlloc(...); ...; return p; }`
 *     —— 调用结果携带体内那张自动槽（调用方不 free 就是真泄漏）。
 * 缓存在 fn 节点：tag_state = 0 未分析 / -1 无 / k+1 返回第 k 个形参；
 * index（FN 节点不复用）= 0 未分析 / -1 无 / >0 体内 tag 槽 id。两者互斥。 */
static Node *tag_ret_source(Node *e) {
    while (e) {
        if (e->kind == NK_IDENT) return e;
        if (e->kind == NK_CAST && e->b) { e = e->b; continue; }
        if (e->kind == NK_UNWRAP && e->a) { e = e->a; continue; }
        if (e->kind == NK_YIELD) { e = e->a; continue; }
        return NULL;
    }
    return NULL;
}

static void tag_scan_returns(Node *n, Node *fn, int *pk, int *pslot, int *pst, bool *ok) {
    if (!n || !*ok) return;
    if (n->kind == NK_FN) return;                    /* 嵌套函数体不算 */
    if (n->kind == NK_RETURN) {
        if (!n->a) { *ok = false; return; }          /* 有的路径不返回值 */
        /* 已经烙了 tag 的节点（内层调用点/铸型/臂值 if）：工厂链
         * make()→return。§174：臂内分配的身份由臂值节点携带（槽状态
         * 已被 tag_restore 回滚），所以返回节点携带的终局状态一并
         * 记下（多路 return 状态不一致时作废，调用点回看槽状态）。 */
        if (n->a->tag_id) {
            /* §180：透传链（pass2 体内 `return pass1(p)`）的返回节点烙着
             * 内层调用点的 tag_id——那往往是**本函数的形参抽象槽**（借出），
             * 不是体内新分配。误判成工厂会让调用点 tag_clone_site 克隆一份
             * 新槽、与实参原槽脱钩（两层转发即泄漏 + 释放不认）。先按
             * tag_id 追到形参下标，命中即当透传形参处理。 */
            TagSlot *rts = tag_slot(n->a->tag_id);
            if (rts && rts->abstract) {
                int kk = -1;
                for (int ii = 0; ii < fn->nitems; ii++)
                    if (fn->items[ii]->sym && fn->items[ii]->sym->tag_id == n->a->tag_id) {
                        kk = ii; break;
                    }
                if (kk >= 0) {
                    if (*pslot > 0 || (*pk >= 0 && *pk != kk)) { *ok = false; return; }
                    *pk = kk;
                    return;
                }
            }
            if (*pk >= 0 || (*pslot > 0 && *pslot != n->a->tag_id)) { *ok = false; return; }
            *pslot = n->a->tag_id;
            if (*pst == 0) *pst = n->a->tag_state;
            else if (*pst != n->a->tag_state) *pst = -1;
            return;
        }
        Node *src = tag_ret_source(n->a);
        if (!src || !src->name) { *ok = false; return; }
        int k = -1;
        for (int i = 0; i < fn->nitems; i++)
            if (fn->items[i]->name && !strcmp(fn->items[i]->name, src->name)) { k = i; break; }
        if (k >= 0) {
            if (*pslot > 0 || (*pk >= 0 && *pk != k)) { *ok = false; return; }
            *pk = k;
            return;
        }
        /* 体内局部载体：工厂模式（返回自己分配的那份） */
        if (src->sym && src->sym->tag_id) {
            if (*pk >= 0 || (*pslot > 0 && *pslot != src->sym->tag_id)) { *ok = false; return; }
            *pslot = src->sym->tag_id;
            if (*pst == 0) *pst = src->sym->tag_state;
            else if (*pst != src->sym->tag_state) *pst = -1;
            return;
        }
        *ok = false;
        return;
    }
    tag_scan_returns(n->a, fn, pk, pslot, pst, ok);
    tag_scan_returns(n->b, fn, pk, pslot, pst, ok);
    tag_scan_returns(n->c, fn, pk, pslot, pst, ok);
    tag_scan_returns(n->d, fn, pk, pslot, pst, ok);
    for (int i = 0; i < n->nitems; i++) tag_scan_returns(n->items[i], fn, pk, pslot, pst, ok);
}

/* 直接调用的目标 fn 节点（直呼名字/实例化；函数指针分发拿不到）。 */
static Node *call_direct_fn(Node *call) {
    Node *a = call->a;
    if (!a) return NULL;
    if (a->sym && a->sym->decl) {
        Node *d = a->sym->decl;
        if (d->kind == NK_FN) return d;
        if (d->kind == NK_DECL && d->b && d->b->kind == NK_FN) return d->b;
        return NULL;
    }
    if (a->inst && a->inst->kind == NK_FN) return a->inst;
    return NULL;
}

/* 返回流缓存三态：1 = 有（*k_out 形参下标 / *slot_out 体内槽）；
 * 0 = 确认无（body 已查完、扫过）；-1 = 未分析（body 还没查）。
 * 扫描依赖 sema 产出的节点烙印与局部符号，前向引用的调用点先于
 * 被调方到达时当场扫必然失败——绝不能缓存成“确认无”（用户 2026-11-17
 * 实测：alloc 定义在 main 之后时逃逸检测失效），body 查完时由
 * fn_stamp_ret_flow 统一烙缓存并回填。 */
static int fn_ret_cache(Node *fn, int *k_out, int *slot_out) {
    *k_out = -1; *slot_out = 0;
    if (!fn || fn->kind != NK_FN || !fn->b) return 0;
    if (fn->tag_state == 0 && fn->index == 0) return -1;   /* 未分析 */
    if (fn->tag_state > 0) *k_out = fn->tag_state - 1;
    if (fn->index > 0) *slot_out = fn->index;
    return 1;
}

/* body 查完时烙返回流缓存（此时 cast 烙印/局部符号已就位）。 */
static void fn_stamp_ret_flow(Node *fn) {
    if (!fn || fn->kind != NK_FN || !fn->b) return;
    if (fn->tag_state != 0 || fn->index != 0) return;      /* 已烙 */
    int k = -1, slot = 0, st = 0;
    bool ok = true;
    tag_scan_returns(fn->b, fn, &k, &slot, &st, &ok);
    if (ok && k >= 0) fn->tag_state = k + 1;
    else if (ok && slot > 0) {
        fn->index = slot;
        /* §174：臂内分配由臂值携带身份、槽状态已回滚——把返回节点
         * 的终局状态烙进 fn，调用点槽状态 UNBOUND 时靠它继承。 */
        if (st == TC_LIVE || st == TC_FREED) fn->ret_tag_state = st;
        /* §175：工厂链登记——所有权经返回流转出，泄漏责任在调用方；
         * 零调用的工厂分配运行期不发生，终扫豁免其槽。 */
        TagSlot *ts = tag_slot(slot);
        if (ts && !ts->ret_fn) ts->ret_fn = fn;
    }
    else { fn->tag_state = -1; fn->index = -1; }
    /* §173：结构体返回值携带的字段账烙到 fn（多路 return 合并）；
     * 调用点继承后 DECL 绑到载体符号，`s.name` 的 free/解引用即可追。 */
    if (!fn->ftags) {
        FieldTag *fs = NULL;
        ft_scan_returns(fn->b, &fs);
        if (fs) fn->ftags = fs;
    }
    /* §175：字段账里的槽同样登记工厂链（返回结构体/元组的工厂）。 */
    for (FieldTag *g = (FieldTag *)fn->ftags; g; g = g->next) {
        if (g->slot <= 0) continue;
        TagSlot *ts = tag_slot(g->slot);
        if (ts && !ts->ret_fn) ts->ret_fn = fn;
    }
    /* §194：形参容器 store 的槽登记工厂链——所有权经调用点镜像流转到
     * 调用方，零调用时分配运行期不发生，终扫豁免（与返回流工厂同口径）。
     * 按形参符号归属筛选（pfs 是全局账，混着所有函数的 store 事实）。 */
    for (int i = 0; i < fn->nitems; i++) {
        Symbol *ps = fn->items[i] ? fn->items[i]->sym : NULL;
        if (!ps) continue;
        for (int j = 0; j < g_npfs; j++) {
            if (g_pfs[j].psym != ps) continue;
            TagSlot *ts = tag_slot(g_pfs[j].slot);
            if (ts && !ts->abstract && !ts->ret_fn) ts->ret_fn = fn;
        }
    }
}

/* 贴标（NK_TAG_APPLY 带 b）：把分配 / 同标签载体绑到 tag 上。
 * 返回载体状态（TC_*）；报错时置 *bad。 */
static int tag_check_apply(Node *n, bool *bad) {
    *bad = false;
    TagSlot *t;
    if (n->a) {
        /* §173：别名贴标（`buf : expr`，buf 是 tag 值的 const 绑定）——
         * 符号优先（局部遮蔽正确性）；符号不是 tag 值就报 E0520。 */
        Symbol *s = lookup(n->name);
        if (s) {
            s->used = true;
            t = s->tag_id ? tag_slot(s->tag_id) : NULL;
            if (!t) {
                diag_code_at("E0520", n->line, n->col,
                    "`%s` is not a label value -- `x : expr` needs "
                    "`const x = tag.x;` (or write `tag.x : expr`)", n->name);
                *bad = true;
                return TC_NONE;
            }
        } else {
            diag_code_at("E0401", n->line, n->col,
                "unknown identifier '%s' -- tag labels are written "
                "`tag.x : expr`", n->name);
            *bad = true;
            return TC_NONE;
        }
    } else {
        /* §172：引用即声明（自动建槽），没有任何豁免 */
        t = tag_resolve(n);
    }
    if (!t) {
        *bad = true;
        return TC_NONE;
    }
    int tid = (int)(t - g_tslots) + 1;
    n->ival = (uint64_t)tid;

    int id, st;
    tag_of_expr(n->b, &id, &st);
    int rid = (n->b && n->b->kind == NK_CALL) ? n->b->reg_slot : 0;

    if (rid > 0) {
        /* §173：alloc 调用点已自动建槽 —— 显式贴标收编它：自动槽退回
         * idle，分配事实记到具名槽上（自动建槽只剩兜底作用）。 */
        if (n->b->tag_id) {
            TagSlot *a = tag_slot(n->b->tag_id);
            if (a && a->auto_n && a->state == TST_LIVE) {
                a->state = TST_UNBOUND;
                a->region = 0;
            }
        }
        /* 分配贴标：tag 必须空闲。tag 是单例标签，一次 LIVE。
         * 形参借出槽不记分配事实：`tag.p : alloc` = 解除借用，p 改挂
         * 分配自己的自动槽。 */
        if (t->abstract) {
            TagSlot *a = n->b->tag_id ? tag_slot(n->b->tag_id) : NULL;
            if (a && a->auto_n) {
                n->tag_id = (int)(a - g_tslots) + 1;
                n->tag_state = TC_LIVE;
                return TC_LIVE;
            }
            n->tag_id = 0;
            return TC_NONE;
        }
        if (t->state == TST_LIVE) {
            diag_code_at("E0518", n->line, n->col,
                "this label already owns an allocation (bound at line %d) -- "
                "free it first, or use a second label", t->line);
            *bad = true;
            return TC_NONE;
        }
        t->state = TST_LIVE;
        t->region = rid;
        if (rid <= g_nrslots) g_rslots[rid - 1].tag = tid;
        n->tag_id = tid;
        n->tag_state = TC_LIVE;
        return TC_LIVE;
    }
    if (id == tid) {
        /* 同标签载体：重申（UNKNOWN/COND → 使用者担保 LIVE）或别名传递。 */
        if (st == TC_FREED) {
            diag_code_at("E0512", n->line, n->col,
                "this memory was freed at line %d -- re-binding a freed "
                "pointer is use after free; drop the re-bind", t->free_line);
            *bad = true;
            return TC_NONE;
        }
        /* §189：move 出去的所有权不能贴标签收回来——接收方建自己的账。 */
        if (st == TC_MOVED) {
            diag_code_at("E0529", n->line, n->col,
                "this pointer was moved away at line %d (`std.move`) -- "
                "re-binding it is use after move", t->moved_line);
            *bad = true;
            return TC_NONE;
        }
        if (st == TC_LIVE) t->state = TST_LIVE;   /* 防守重申：重新拥有 */
        n->tag_id = tid;
        n->tag_state = TC_LIVE;
        return TC_LIVE;
    }
    /* §189：接收半环 —— 无标签指针的建账贴标。多线程边界（spawn 的
     * usize ctx）cast 回来的裸指针没有可续的账；`tag q : expr` 让接收
     * 方从零建自己的新账（无历史事实，不违反 E0514 的跨函数防假账
     * 口径——那是「续别人的账」）。标签必须空闲。 */
    if (!id && n->b && n->b->ty &&
        (n->b->ty->kind == TY_PTR || n->b->ty->kind == TY_SLICE)) {
        if (!t->abstract && t->state == TST_LIVE) {
            diag_code_at("E0518", n->line, n->col,
                "this label already owns an allocation (bound at line %d) -- "
                "free it first, or use a second label", t->line);
            *bad = true;
            return TC_NONE;
        }
        if (!t->abstract) {
            t->state = TST_LIVE;
            t->region = 0;
            n->tag_id = tid;
            n->tag_state = TC_LIVE;
            return TC_LIVE;
        }
        /* 抽象槽：贴上即续借用链（形参借出口径），不记分配事实。 */
        n->tag_id = tid;
        n->tag_state = TC_LIVE;
        return TC_LIVE;
    }
    diag_code_at("E0514", n->line, n->col,
        "a label can only bind a std.heapAlloc/std.heapSlice allocation or "
        "a pointer it already owns -- a labeled pointer cannot cross "
        "function boundaries");
    *bad = true;
    return TC_NONE;
}

/* ---------------- §170 臂出口与分支合并 ---------------- */

typedef struct {
    int   tag;      /* TagSlot 下标 + 1（0 = 该臂没碰/没传出） */
    int   state;    /* 臂出口载体状态 */
    Node *val;      /* 臂值表达式（null 载体判定用） */
    bool  stmt;     /* §173：语句臂（armval == NULL，无载体可传出） */
} ArmExit;

/* §187：终局臂的 return 是否把槽传出。0 无 return；1 全部传出；
 * -1 有 return 没带（那条路径就是泄漏，E0514 照报）。 */
static int arm_ret_scan(Node *nd, int slot) {
    if (!nd) return 0;
    switch (nd->kind) {
    case NK_RETURN: {
        int tid, tst;
        tag_of_expr(nd->a, &tid, &tst);
        return tid == slot + 1 ? 1 : -1;
    }
    case NK_BLOCK: {
        int r = 0;
        for (int i = 0; i < nd->nitems; i++) {
            int s2 = arm_ret_scan(nd->items[i], slot);
            if (s2 == -1) return -1;
            if (s2 == 1) r = 1;
        }
        return r;
    }
    case NK_IF: {
        int t = arm_ret_scan(nd->b, slot);
        if (t == -1) return -1;
        int e = nd->c ? arm_ret_scan(nd->c, slot) : 0;
        if (e == -1) return -1;
        return (t == 1 || e == 1) ? 1 : 0;
    }
    default:
        return 0;
    }
}

/* 臂检查后对账：对比快照，臂内释放的 tag 必须以 null 收尾，臂内分配的
 * tag 必须由臂值传出。返回该臂的出口载体（无传出 → tag=0）。
 * armnode：臂的语句节点（表达式臂传 NULL）——终局臂（return/continue
 * 收尾，控制不落到 join 点）的释放/传出走别的路径，不参与对账。 */
static ArmExit tag_arm_reconcile(Node *armval, Node *armnode,
                                 const int *snap, int n0, int ntotal) {
    ArmExit e = { 0, TC_NONE, armval };
    for (int i = 0; i < g_ntslots; i++) {
        /* §172：快照之后新建的槽（自动建槽可能发生在臂内）按 idle 进入；
         * 超出快照数组容量的老槽维持旧行为（跳过） */
        int before;
        if (i < n0) before = snap[i];
        else if (i < ntotal) continue;
        else before = 0;
        if (g_tslots[i].state == before) {
            /* §189：确认声明落空——臂里没有与之配对的释放。 */
            if (g_tslots[i].confirm_arm && g_tslots[i].state == TST_LIVE)
                diag_code_at("E0528", g_tslots[i].confirm_line,
                    g_tslots[i].confirm_col,
                    "`tag : null;` confirms a release, but this pointer was "
                    "not released in this branch arm -- remove the "
                    "confirmation or release it first");
            continue;
        }
        TagSlot *t = &g_tslots[i];
        if (t->state == TST_FREED && snap[i] == TST_LIVE) {
            /* §187：终局臂（return/continue 收尾，控制不落到 join 点）
             * 的释放随路径离开——不落账（快照恢复后槽保持 LIVE），
             * 泄漏闭合由 free 事件烙下的 ever_freed 交给终扫（路径
             * 并集口径）。否则 `if (bad) { deinit(a); return 3; }`
             * 之后的正常路径会被当成"已释放"假报 E0512/E0509。 */
            if (armnode && arm_ends(armnode)) continue;
            /* §189（用户裁定）：臂内释放必须显式确认——`tag p : null;`
             * 写在释放之后，证明使用者确认过正确释放。没确认的释放
             * 不落账：合流后当作还没释放（分支本就可能不走，不能错
             * 报），不影响以后的 use/free。std 实现层 / tag.close /
             * tag.batch 模块不受此限（账由容器/模块内部闭环）。
             * 只对臂前就 LIVE 的槽把关——臂内新建的槽随臂消亡
             * （alloc/free 配对在臂内闭合是常事，无需确认）。 */
            if (before == TST_LIVE && !t->confirm_arm &&
                !(g_mod && (g_mod->tags_closed || g_mod->tags_batch ||
                            tag_module_exempt(g_mod)))) {
                diag_code_at("E0528", t->free_line, t->free_col,
                    "release in a branch arm must be confirmed -- write "
                    "`tag p : null;` (p = the carrier variable) right after "
                    "the free/deinit to confirm the pointer is given up; an "
                    "unconfirmed release is treated as not-released at the "
                    "join because the branch may not run");
                continue;
            }
            /* 臂内释放：值臂必须传出 null 载体（"在臂内释放，就返回空
             * 指针"）；§173：语句臂没有载体可传出——不报错，由 join
             * 之后把终局 FREED 落回槽与载体（惯用的
             * `if (ctx) c { ...; free(p); }` 包装器不再被迫改写）。
             * 容器账（book）同走此路：臂内 deinit 靠落账闭合（循环
             * 配平已对 book 豁免，E0516 不会假阳）。 */
            if (armval && armval->kind == NK_NULL) {
                armval->tag_id = i + 1;
                armval->tag_state = TC_FREED;
                e.tag = i + 1; e.state = TC_FREED; e.val = armval;
            } else if (!armval) {
                e.tag = i + 1; e.state = TC_FREED; e.val = NULL; e.stmt = true;
            } else {
                diag_code_at("E0514", t->free_line, t->free_col,
                    "the allocation was freed inside this branch -- the "
                    "branch must return null as its value so the freed "
                    "state travels with the carrier");
                e.tag = i + 1; e.state = TC_FREED; e.val = NULL;
            }
        } else if (t->state == TST_MOVED && snap[i] == TST_LIVE) {
            /* §189（用户裁定）：臂内 std.move —— 所有权已移交给 move
             * 产生的新槽（臂内建的槽随臂消亡，接收方的账自己闭环）。
             * 原槽跨合流落账为 MOVED：终扫不再向本作用域讨要这笔分配，
             * 原变量之后 use/free 报 E0529（move 之后原值的标签改为
             * moved，不能再用）。 */
            e.tag = i + 1; e.state = TC_MOVED; e.val = NULL; e.stmt = true;
        } else if (t->state == TST_LIVE && snap[i] != TST_LIVE) {
            /* §187：已收养给 arena 的槽不出臂——所有权已移交（内存
             * 活到 arena deinit），臂值无需携带。 */
            if (t->adopt_ar) continue;
            /* §187：终局臂的分配经 return 传出（返回流接管，调用点
             * 克隆）——只要臂里**每个** return 都携带该槽即放行（分配
             * 常常是条件发生的：capture 臂必 return，未捕获路径根本
             * 没分配）；有 return 没带的仍是泄漏，E0514 照报。 */
            if (armnode && arm_ret_scan(armnode, i) == 1) continue;
            /* 臂内分配：必须由臂值传出（指针不能留在控制流里） */
            int id, st;
            tag_of_expr(armval, &id, &st);
            if (id == i + 1 && st == TC_LIVE) {
                e.tag = i + 1; e.state = TC_LIVE; e.val = armval;
            } else {
                diag_code_at("E0514", t->line, t->col,
                    "memory allocated inside this branch is not returned by "
                    "it -- a labeled pointer cannot stay behind control "
                    "flow");
                e.tag = i + 1; e.state = TC_LIVE; e.val = NULL;
            }
        }
    }
    return e;
}

/* 两臂出口合并（decisions §170 的 join 表）。结果挂到 if 节点。 */
static void tag_join_arms(Node *n, ArmExit a, ArmExit b) {
    if (!a.tag && !b.tag) return;
    /* 一臂没碰 tag（出口 NONE, tag=0）：按同 tag 合并——join 表里
     * LIVE⊔NONE=COND / FREED⊔NONE=FREED 才能生效。否则 else 传 null
     * 的合并会被误当成单臂出口，COND 判空防守整条断掉。 */
    if (a.tag != b.tag) {
        if (a.tag && !b.tag) { b.tag = a.tag; b.state = TC_NONE; b.val = NULL; }
        else if (b.tag && !a.tag) { a.tag = b.tag; a.state = TC_NONE; a.val = NULL; }
        else {                        /* 两臂动了不同的 tag：各自记账即可 */
            n->tag_id = a.tag ? a.tag : b.tag;
            n->tag_state = a.tag ? a.state : b.state;
            return;
        }
    }
    int id = a.tag;
    int s;
    if (a.state == b.state)          s = a.state;
    else if ((a.state == TC_LIVE  && b.state == TC_FREED) ||
             (a.state == TC_FREED && b.state == TC_LIVE)) {
        /* FREED 臂的值是 null 载体 → COND（可能是 null，判空后用）；
         * 否则（free 后传了非空载体）→ UNKNOWN，下游必须防守。 */
        Node *freed = (a.state == TC_FREED) ? a.val : b.val;
        s = (freed && freed->kind == NK_NULL) ? TC_COND : TC_UNKNOWN;
    }
    else if ((a.state == TC_LIVE  && b.state == TC_NONE) ||
             (a.state == TC_NONE  && b.state == TC_LIVE))  s = TC_COND;
    else if ((a.state == TC_FREED && b.state == TC_NONE) ||
             (a.state == TC_NONE  && b.state == TC_FREED)) s = TC_FREED;
    else if ((a.state == TC_MOVED && b.state == TC_NONE) ||
             (a.state == TC_NONE  && b.state == TC_MOVED)) s = TC_MOVED;
    else                             s = TC_UNKNOWN;
    n->tag_id = id;
    n->tag_state = s;
    /* COND/UNKNOWN 不改写 tag 侧事实（free 事件已即时记账）；LIVE/FREED
     * 一致时事实已由事件本身确定。 */
}

/* ---------------- §170 循环配平 ---------------- */

#define TAG_SNAP_MAX 128
typedef struct {
    int      n;
    int      total;    /* §172：快照时的槽位总数（含超出数组容量的） */
    int      state[TAG_SNAP_MAX];
    Symbol  *carrier[TAG_SNAP_MAX];   /* 载体符号与其 tag_state 一起回滚 */
    int      cstate[TAG_SNAP_MAX];
    /* §173 补丁五：可见局部的字段账本（Symbol.ftags）一起回滚——
     * 臂内整结构体赋值（`s = .{…}`）重绑字段账，另一臂/下一轮迭代
     * 必须从同一进入状态开始。按符号指针匹配（臂内 push 的捕获变量
     * 不在快照里，退出作用域自然消亡）。超容量截断（与槽位同口径）。 */
    Symbol  *fsym[TAG_SNAP_MAX];
    FieldTag *ft[TAG_SNAP_MAX];
    int      nfts;
    /* §187：可见局部的 tag 载体状态（Symbol.tag_state）一起回滚——
     * deinit/free 钩子会同步**所有**带该槽的局部符号（含捕获名 a），
     * 只回滚 carrier 会把臂内释放的 FREED 泄进 join 点之后的正常
     * 路径（E0512 假阳）。按符号指针匹配，同字段账口径。 */
    Symbol  *lsym[TAG_SNAP_MAX];
    int      lstate[TAG_SNAP_MAX];
    int      nlsyms;
} TagSnap;

static void tag_snap(TagSnap *s) {
    s->total = g_ntslots;
    s->n = g_ntslots < TAG_SNAP_MAX ? g_ntslots : TAG_SNAP_MAX;
    for (int i = 0; i < s->n; i++) {
        s->state[i] = g_tslots[i].state;
        s->carrier[i] = g_tslots[i].carrier;
        s->cstate[i] = g_tslots[i].carrier ? g_tslots[i].carrier->tag_state : 0;
    }
    s->nfts = 0;
    for (Symbol *ls = g_locals; ls && s->nfts < TAG_SNAP_MAX; ls = ls->locals_next) {
        s->fsym[s->nfts] = ls;
        s->ft[s->nfts] = (FieldTag *)ls->ftags;
        s->nfts++;
    }
    s->nlsyms = 0;
    for (Symbol *ls = g_locals; ls && s->nlsyms < TAG_SNAP_MAX; ls = ls->locals_next) {
        if (!ls->tag_id) continue;
        s->lsym[s->nlsyms] = ls;
        s->lstate[s->nlsyms] = ls->tag_state;
        s->nlsyms++;
    }
}

/* 恢复快照：另一臂 / 下一轮迭代从同一进入状态开始检查。
 * §172：快照之后**新建**的槽位退回 idle——臂内分配的事实已经由对账收进
 * 载体链（与预声明时代槽位以 idle 进入分支的语义一致）；槽位本身保留，
 * 保证 tag_id 索引稳定。 */
static void tag_restore(const TagSnap *s) {
    for (int i = 0; i < s->n && i < g_ntslots; i++) {
        Symbol *prev = g_tslots[i].carrier;
        g_tslots[i].state = s->state[i];
        g_tslots[i].carrier = s->carrier[i];
        /* §189：臂确认标记随快照回滚——槽回非 FREED（本臂没释放或
         * 释放已随落账走人）时确认不该残留到后续臂/迭代。槽保持
         * FREED（释放发生在本臂之前）时确认仍有效。 */
        if (g_tslots[i].state != TST_FREED) g_tslots[i].confirm_arm = false;
        if (g_tslots[i].carrier) g_tslots[i].carrier->tag_state = s->cstate[i];
        /* §187：臂内 free/deinit 事件会把载体换挂到捕获名上（free_event
         * 的 tag_carrier_sym 直改符号，捕获名不在 g_locals，lsym 回滚
         * 够不到）。槽回 LIVE 时，被换挂的载体也回 LIVE——它被 FREED
         * 只因臂内释放事件，事件已随快照回滚。 */
        if (prev && prev != g_tslots[i].carrier && prev->tag_id == i + 1 &&
            g_tslots[i].state == TST_LIVE && prev->tag_state == TC_FREED)
            prev->tag_state = TC_LIVE;
    }
    for (int i = s->total; i < g_ntslots; i++) {
        g_tslots[i].state = 0;
        g_tslots[i].carrier = NULL;
    }
    /* §173 补丁五：字段账本按符号指针回滚（臂内的捕获变量不在快照里，
     * 走不到即随作用域消亡）。 */
    for (Symbol *ls = g_locals; ls; ls = ls->locals_next)
        for (int i = 0; i < s->nfts; i++)
            if (s->fsym[i] == ls) { ls->ftags = s->ft[i]; break; }
    /* §187：非 carrier 局部符号的 tag 载体状态回滚（按符号指针匹配，
     * 同字段账口径——臂内 push 的捕获变量不在快照里，随作用域消亡）。 */
    for (Symbol *ls = g_locals; ls; ls = ls->locals_next)
        for (int i = 0; i < s->nlsyms; i++)
            if (s->lsym[i] == ls) { ls->tag_state = s->lstate[i]; break; }
}

/* 臂出口收集字段账被臂改动的载体（与快照比对）。 */
static int ft_arm_collect(const TagSnap *s, Symbol **syms, FieldTag **fts, int cap) {
    int n = 0;
    for (Symbol *ls = g_locals; ls && n < cap; ls = ls->locals_next)
        for (int i = 0; i < s->nfts; i++)
            if (s->fsym[i] == ls) {
                if ((FieldTag *)ls->ftags != s->ft[i]) {
                    syms[n] = ls; fts[n] = (FieldTag *)ls->ftags; n++;
                }
                break;
            }
    return n;
}

/* 两臂字段账合并（保守口径，运行期互斥路径）：某字段两臂最终账本
 * 都映射同一槽才保留；单臂改动 / 槽不一致一律丢弃（跟踪缺口 = 漏报，
 * 绝不引入假报）。区别于 ft_merge 的 return 路径合并（那里单边字段
 * 保留是因为 return 后只有那一条路径的事实）。 */
static FieldTag *ft_arms_intersect(FieldTag *a, FieldTag *b) {
    FieldTag *out = NULL;
    for (FieldTag *f = a; f; f = f->next) {
        FieldTag *g = ft_find(b, f->name);
        if (g && g->slot == f->slot && f->slot > 0)
            ft_set(&out, f->name, f->slot);
    }
    return out;
}

static void ft_arms_join(Symbol **s1, FieldTag **f1, int n1,
                         Symbol **s2, FieldTag **f2, int n2) {
    for (int i = 0; i < n1; i++)
        for (int j = 0; j < n2; j++)
            if (s2[j] == s1[i]) {
                s1[i]->ftags = ft_arms_intersect(f1[i], f2[j]);
                break;
            }
}

/* 循环体检查后对账。合法形态只有两种：
 *   - 循环外分配（进入 LIVE）：轮内只用不释放，出口仍是 LIVE——
 *     轮内释放会让后续轮拿到悬垂载体；
 *   - 轮内自闭合（进入 idle）：alloc + free 在同一迭代里成对，出口 FREED，
 *     下一轮从空闲状态重新分配。
 * 出口 LIVE 而进入是 idle = 分配没闭合（第二轮就是重复分配）；出口 FREED
 * 而进入是 LIVE = 轮内释放了循环外的分配。都是 E0516。 */
static void tag_loop_balance(Node *body, const TagSnap *snap) {
    for (int i = 0; i < g_ntslots; i++) {
        /* §172：快照之后新建的槽按 idle 进入；超容量老槽跳过 */
        if (g_tslots[i].abstract) continue;    /* §173：借出槽不参与配平 */
        if (g_tslots[i].book) continue;        /* §187：容器账不参与配平 */
        int before, after;
        if (i < snap->n) before = snap->state[i];
        else if (i < snap->total) continue;
        else before = 0;
        after = g_tslots[i].state;
        if (before == after) continue;
        bool ok = (before == TST_LIVE && after == TST_LIVE) ||
                  (before != TST_LIVE && after == TST_FREED);
        if (ok) continue;
        TagSlot *t = &g_tslots[i];
        diag_code_at("E0516", t->line, t->col,
            "memory allocated inside the loop is not balanced (live on "
            "entry, freed on exit) -- allocate and free it within the same "
            "iteration, or keep the loop-external allocation alive until "
            "after the loop");
    }
    int id, st;
    tag_of_expr(body->kind == NK_BLOCK ? body->d : body, &id, &st);
    if (id) {
        TagSlot *t = tag_slot(id);
        if (t) diag_code_at("E0516", t->line, t->col,
            "a loop cannot yield this pointer -- it is a different allocation "
            "in each iteration; free inside the loop or allocate outside");
    }
}

/* ---------------- §170 收尾：泄漏 + 强制贴标 ----------------
 * sema 全部结束后调用一次。 */

static bool tag_module_exempt(Module *m) {
    if (!m || !m->path) return true;               /* 无模块上下文：不强制 */
    if (m == g_std_mod) return true;               /* 标准库天然豁免 */
    if (g_std_mod && g_std_mod->path) {            /* std 目录下的内部文件 */
        const char *a = strrchr(g_std_mod->path, '/');
        const char *b = strrchr(m->path, '/');
        const char *la = strrchr(g_std_mod->path, '\\');
        const char *lb = strrchr(m->path, '\\');
        if (la && (!a || la > a)) a = la;
        if (lb && (!b || lb > b)) b = lb;
        if (a && b && (a - g_std_mod->path) == (b - m->path) &&
            !memcmp(g_std_mod->path, m->path, (size_t)(a - g_std_mod->path)))
            return true;
    }
    /* §189：g_std_mod 还没被加载（直检 std/ 下的文件）时，按内置
     * stdlib 目录识别 std 内部文件——豁免口径与导入时一致：std 实现
     * 层的账由容器/模块内部闭环，不随检查入口（直检/导入）变化。
     * 入口文件路径可能是相对的（`zxc check std/fs.zx`），先补 cwd。 */
    {
        char raw[1600], stdp[1400];
        bool isabs = (m->path[0] == '/' || m->path[0] == '\\') ||
                     (((m->path[0] >= 'A' && m->path[0] <= 'Z') ||
                       (m->path[0] >= 'a' && m->path[0] <= 'z')) &&
                      m->path[1] == ':');
        if (isabs) snprintf(raw, sizeof raw, "%s", m->path);
        else {
            char cwd[1200];
            if (!_getcwd(cwd, (int)sizeof cwd)) return false;
            snprintf(raw, sizeof raw, "%s/%s", cwd, m->path);
        }
        if (find_std_path(stdp, sizeof stdp)) {
            /* find_std_path 的结果也可能是相对的（exe 相对调用时
             * g_exe_dir 是相对目录）——同样补 cwd 再归一化。 */
            bool sabs = (stdp[0] == '/' || stdp[0] == '\\') ||
                        (((stdp[0] >= 'A' && stdp[0] <= 'Z') ||
                          (stdp[0] >= 'a' && stdp[0] <= 'z')) &&
                         stdp[1] == ':');
            if (!sabs) {
                char cwd2[1200];
                char tmp[1600];
                if (_getcwd(cwd2, (int)sizeof cwd2))
                    snprintf(tmp, sizeof tmp, "%s/%s", cwd2, stdp);
                else snprintf(tmp, sizeof tmp, "%s", stdp);
                snprintf(stdp, sizeof stdp, "%s", tmp);
            }
            char *ns = path_normalize(stdp);
            char *np = path_normalize(raw);
            if (ns && np) {
                const char *a = strrchr(ns, '/');
                const char *b = strrchr(np, '/');
                const char *la = strrchr(ns, '\\');
                const char *lb = strrchr(np, '\\');
                if (la && (!a || la > a)) a = la;
                if (lb && (!b || lb > b)) b = lb;
                if (a && b && (a - ns) == (b - np) &&
                    _strnicmp(ns, np, (size_t)(a - ns)) == 0)
                    return true;
            }
            free(ns); free(np);
        }
    }
    return false;
}

/* §176：沿调用边从 cur 回溯到 main 的一层 DFS。返回值是 cur 之上的
 * 调用链（每跳 = 调用方:调用行），如 cur=load 时返回 "main:5 -> init:14"
 * ——含义是 main 在第 5 行调了 init，init 在第 14 行调了 load。
 * 多条路优先走能到 main 的；到不了 main 的由最外层兜底给首跳。 */
static bool tag_chain_walk(const char *cur, char *out, size_t cap,
                           const char *visited[], int nv, int depth) {
    if (depth > 32 || nv >= 64) return false;
    for (int i = 0; i < g_ncedges; i++) {
        if (!g_cedges[i].callee || strcmp(g_cedges[i].callee, cur)) continue;
        const char *caller = g_cedges[i].caller;
        if (!caller || !*caller) continue;
        bool seen = false;
        for (int j = 0; j < nv; j++)
            if (!strcmp(visited[j], caller)) { seen = true; break; }
        if (seen) continue;
        if (!strcmp(caller, "main")) {
            snprintf(out, cap, "main:%d", g_cedges[i].line);
            return true;
        }
        const char *vis2[64];
        int n2 = nv;
        for (int j = 0; j < nv; j++) vis2[j] = visited[j];
        vis2[n2++] = caller;
        char sub[384];
        if (tag_chain_walk(caller, sub, sizeof sub, vis2, n2, depth + 1)) {
            snprintf(out, cap, "%s -> %s:%d", sub, caller, g_cedges[i].line);
            return true;
        }
    }
    return false;
}

/* §176：一份泄漏的调用链——实例槽从归属调用点所在函数回溯（精确指认
 * 是哪一次调用），原生槽从分配点所在函数回溯。返回 false = 该函数就在
 * main / 模块顶层里，或回溯不到任何调用方。 */
static bool tag_chain_slot(TagSlot *t, char *out, size_t cap) {
    const char *start = t->own_fn ? t->own_fn : t->def_fn;
    if (!start || !*start) return false;
    const char *visited[64];
    char hops[384];
    if (!tag_chain_walk(start, hops, sizeof hops, visited, 0, 0)) {
        /* 到不了 main：至少给出首跳调用方，仍能指认是谁调进来的 */
        for (int i = 0; i < g_ncedges; i++)
            if (g_cedges[i].callee && !strcmp(g_cedges[i].callee, start) &&
                g_cedges[i].caller) {
                snprintf(out, cap, "%s:%d", g_cedges[i].caller,
                         g_cedges[i].line);
                return true;
            }
        return false;
    }
    if (t->own_fn)
        snprintf(out, cap, "%s -> %s:%d -> %s", hops, t->own_fn, t->own_line,
                 t->def_fn ? t->def_fn : "?");
    else
        snprintf(out, cap, "%s -> %s", hops, start);
    return true;
}

static void tag_ledger_final(const char *entry_path) {
    for (int i = 0; i < g_ntslots; i++) {
        TagSlot *t = &g_tslots[i];
        if (t->state != TST_LIVE || t->abstract) continue;
        /* §187：终局臂内释放过的槽——状态已随快照恢复 LIVE，但释放
         * 事件真实发生过（ever_freed），路径并集口径下不算泄漏。 */
        if (t->ever_freed) continue;
        if (t->rt_pending) continue;   /* §174：断线由 E0521 报，不双报 */
        /* §187：已收养给 arena——所有权在 arena，deinit 终结；arena
         * 自身没 deinit 的泄漏由容器账（book）报，这里不双报。 */
        if (t->adopt_ar) continue;
        /* §187：容器账专属文案——资源的 init/deinit 配对。 */
        if (t->book) {
            int rl2 = t->own_line ? t->own_line : t->line;
            int rc2 = t->own_line ? t->own_col : t->col;
            diag_code_at("E0513", rl2, rc2,
                "resource acquired at line %d is never torn down -- init "
                "and deinit must come in pairs; call its deinit before it "
                "goes out of scope (all memory it holds is released there)",
                t->line);
            continue;
        }
        /* §175：工厂零调用豁免——所有权已流转给调用方，调用方一个
         * 都没有 = 分配在运行期根本不发生。 */
        if (t->ret_fn && t->ret_fn->tag_calls == 0) continue;
        /* §175：已派生调用点实例的母槽退役——泄漏/双放由实例槽记账。 */
        if (t->ninst > 0) continue;
        /* §173：错误指到所有权归属点（调用/绑定处），分配行留在消息里。
         * 跨函数移交后泄漏，第一现场是调用方接过所有权的那一行。
         * §176：文案只讲原因 + 修法；调用链一并给出、一直回溯到 main，
         * 让人知道这份内存是在哪条调用路径上产生的。 */
        int rl = t->own_line ? t->own_line : t->line;
        int rc = t->own_line ? t->own_col : t->col;
        char chain[512];
        const char *cs = tag_chain_slot(t, chain, sizeof chain)
                         ? chain : NULL;
        const char *cp = cs ? "; called via " : "";
        if (t->arr_esc) {
            /* 提示写使用者自己的变量名（载体/数组基变量）——自动槽名
             * 不再可写（跨库不可预测）。§185：群模型口径——存入即
             * 登记在案，取走/批量终结都在追踪范围内；留到程序结束
             * = 泄漏。 */
            const char *mn = (t->carrier && t->carrier->name)
                             ? t->carrier->name : NULL;
            if (mn)
                diag_code_at("E0513", rl, rc,
                    "memory allocated at line %d is never freed -- it was "
                    "stored into an array/tuple element and is still "
                    "unreleased; take it out with an annotated read "
                    "(`tag v : tag.x = xs[i];`) and free it, or terminate "
                    "the whole container with `tag %s : null;`%s%s",
                    t->line, mn, cp, cs ? cs : "");
            else
                diag_code_at("E0513", rl, rc,
                    "memory allocated at line %d is never freed -- it was "
                    "stored into an array/tuple element and is still "
                    "unreleased; take it out with an annotated read "
                    "(`tag v : tag.x = xs[i];`) and free it, or terminate "
                    "the whole container (`tag <array> : null;`)%s%s",
                    t->line, cp, cs ? cs : "");
        }
        else if (t->own_line && t->own_line != t->line) {
            /* 报在归属点，分配行用 note 指认 */
            diag_note(t->file, t->line, t->col, "allocated here");
            diag_code_at("E0513", rl, rc,
                "memory allocated at line %d is never freed -- alloc and "
                "free must come in pairs; free it before the program "
                "ends%s%s",
                t->line, cp, cs ? cs : "");
        }
        else
            diag_code_at("E0513", rl, rc,
                "memory allocated at line %d is never freed -- alloc and "
                "free must come in pairs; free it before the program "
                "ends%s%s",
                t->line, cp, cs ? cs : "");
    }
    /* §174：运行期索引写入未处理——使用者没有续链接管（tag v :
     * tag.x = xs[i];）也没有终结（tag.<载体> : null;），追踪断线
     * 落账。报在写入处。 */
    for (int i = 0; i < g_ntslots; i++) {
        TagSlot *t = &g_tslots[i];
        if (!t->rt_pending || t->abstract) continue;
        const char *cn = (t->carrier && t->carrier->name)
                         ? t->carrier->name : NULL;
        if (cn)
            diag_code_at("E0521", t->rt_line, t->rt_col,
                "a pointer was stored into `xs[i]` with a runtime index and "
                "never handled -- the compiler cannot track it to its free "
                "site; continue the chain with `tag v : %s = xs[i];`, mark "
                "it released with `tag.%s : null;`, or store at a constant "
                "index",
                cn, cn);
        else
            diag_code_at("E0521", t->rt_line, t->rt_col,
                "a pointer was stored into `xs[i]` with a runtime index and "
                "never handled -- the compiler cannot track it to its free "
                "site; receive it into a variable first (`let tmp = f(); "
                "xs[i] = tmp;`), then continue the chain with "
                "`tag v : tmp = xs[i];`, or store at a constant index");
    }
    /* §173：E0517（强制贴标终扫）退役 —— 分配调用点自动建槽，
     * 没有不被追踪的堆分配；tag.close 模块在 tag_auto_decl 里跳过。 */
    (void)entry_path;
}

/* §132：编译期分配账本的只读视图。`zxc explain region <file.zx>` 与
 * `ZX_REGION=1` 共用这份输出；unit 记的是分配点所在模块，不是入口文件，
 * 这样多模块工程里行号才对得上。这是观测，不改任何语义。 */
void region_ledger_dump(FILE *out, const char *path) {
    int nlive = 0, nfree = 0, nmerged = 0;
    for (int i = 0; i < g_nrslots; i++) {
        if (g_rslots[i].state == RS_FREED) nfree++; else nlive++;
        if (g_rslots[i].gid > 0) nmerged++;
    }
    fprintf(out, "region ledger: %d slot(s) live=%d freed=%d merged=%d group(s)=%d\n",
            g_nrslots, nlive, nfree, nmerged, g_region_gid);
    for (int i = 0; i < g_nrslots; i++) {
        RegionSlot *r = &g_rslots[i];
        const char *unit = r->unit ? r->unit : (path ? path : "-");
        fprintf(out, "  slot %d %-5s %s:%d:%d size=%llu align=%llu free=%d:%d",
                i + 1, r->state == RS_LIVE ? "LIVE" : "FREED",
                unit, r->line, r->col,
                (unsigned long long)r->size, (unsigned long long)r->align,
                r->free_line, r->free_col);
        if (r->gid > 0)
            fprintf(out, " gid=%d off=%llu total=%llu", r->gid,
                    (unsigned long long)r->off, (unsigned long long)r->total);
        fputc('\n', out);
    }
    int nprov = 0, nerase = 0;
    for (int i = 0; i < g_nproves; i++) {
        if (g_proves[i].proven) nprov++;
        if (g_proves[i].erased) nerase++;
    }
    fprintf(out, "erase: %d/%d proven, %d erased (mode=%s)\n",
            nprov, g_nproves, nerase, g_zx_release ? "release" : "dev");
    for (int i = 0; i < g_nproves; i++) {
        ProvePoint *q = &g_proves[i];
        const char *unit = q->unit ? q->unit : (path ? path : "-");
        fprintf(out, "  point %s:%d:%d %-6s %-6s %s\n",
                unit, q->line, q->col,
                q->proven ? "PROVEN" : "OPAQUE",
                q->erased ? "erased" : "kept", q->reason);
    }
    if (!g_nrslots && !g_nproves)
        fputs("  (no std.heapAlloc site, no handle access point in this unit)\n", out);
}

/* ---------------- 逃逸检查：值是否指向当前栈帧 ----------------
 *
 * expr_frame(e)——e 的"值"是不是一个指向当前函数栈帧的指针；
 * addr_frame(o)——对 o 取地址（&o）得到的指针是不是指向当前栈帧。
 * 只在 `return` 处消费（decisions §44）：返回栈帧指针是悬垂错误。
 * 栈帧来源有四种：栈上局部变量的槽位、&.{…} 匿名存储、std.stackAlloc、
 * 以及它们的字段/下标/指针运算。已知的保守面：栈指针存进聚合体字段
 * 再整体取出来，追不到（宁漏勿误报）。 */
static bool addr_frame(Node *o);

static bool expr_frame(Node *e) {
    if (!e) return false;
    switch (e->kind) {
    case NK_CALL:  return e->esc;                     /* std.stackAlloc */
    case NK_IDENT: return e->sym && e->sym->stack_val;
    case NK_UN:    return e->op == TOK_AMP && addr_frame(e->a);
    case NK_ARRAY_LIT:                /* &.{…}：值本身就是当前帧的匿名存储 */
        return true;
    case NK_FIELD: case NK_INDEX: case NK_SLICE:
        return expr_frame(e->a);
    case NK_UNWRAP:                   /* `x.?`：载荷来源跟着 optional 走 */
        return expr_frame(e->a);
    case NK_ORELSE: case NK_TRY:      /* 两臂任一来自当前帧即为当前帧 */
        return expr_frame(e->a) || expr_frame(e->b);
    case NK_BIN:
        if (e->op == TOK_PLUS || e->op == TOK_MINUS)  /* 指针运算保持来源 */
            return expr_frame(e->a) || expr_frame(e->b);
        return false;
    default: return false;
    }
}

static bool addr_frame(Node *o) {
    if (!o) return false;
    /* §201 补丁：`&x.?` / `&x.?.f` 的 unwrap 结果（即便载荷是指针 `*T`）
     * 会被后端物化到当前帧临时，取它的地址就落在当前帧。必须先于下面的
     * "指针类型跟着被指物走"分支判定，否则 `&get(b).?.v` 会跟着堆载荷
     * 判成非栈帧，漏过 E0503、生成悬垂 C。 */
    if (o->kind == NK_UNWRAP || o->kind == NK_ORELSE || o->kind == NK_TRY)
        return true;
    Type *t = o->ty;
    if (t && (t->kind == TY_PTR || t->kind == TY_SLICE))
        return expr_frame(o);       /* 取的是被指物：来源跟着被指物走 */
    switch (o->kind) {
    case NK_IDENT:  return o->sym && o->sym->in_fn;   /* 局部变量的槽位本身 */
    case NK_FIELD:  return addr_frame(o->a);
    case NK_INDEX: {
        Type *bt = o->a ? o->a->ty : NULL;
        if (bt && (bt->kind == TY_PTR || bt->kind == TY_SLICE))
            return expr_frame(o->a);  /* 指针/切片下标：指到被指物里去了 */
        return addr_frame(o->a);      /* 数组下标：还在数组槽位里 */
    }
    case NK_ARRAY_LIT: case NK_TUPLE: case NK_STRUCT_LIT: case NK_ANON_STRUCT:
        return true;                /* &.{…} 等匿名存储落在当前栈帧 */
    case NK_UNWRAP:
        /* `&x.?` / `&x.?.f`：unwrap 是 zx_unwrap_...() 的 rvalue，后端会
         * 物化到当前帧临时；即便载荷来自堆，取地址也落在当前帧。 */
        return true;
    case NK_ORELSE: case NK_TRY:
        return true;                /* 兜底臂/捕获块都是当前帧的临时量 */
    default: return false;
    }
}

/* §199：聚合值里藏着的栈帧来源。return 逃逸检查原先只看返回类型形状
 * （want 是 TY_PTR/TY_SLICE），于是 `return S{ .s = &.{} }` —— 把匿名
 * slice 裹进结构体带走——整只漏过，生成悬垂 C。这里下钻聚合字面量的
 * 字段值，任何一个字段是指向栈帧的指针就算逃逸。纯值结构体（字段全是
 * 标量/常量）不会被误伤。 */
static bool frame_in_agg(Node *e, int depth) {
    if (!e || depth > 12) return false;
    switch (e->kind) {
    case NK_STRUCT_LIT: case NK_ANON_STRUCT: case NK_TUPLE:
        for (int i = 0; i < e->nitems; i++) {
            Node *v = e->items[i];
            if (v && v->kind == NK_PAIR) v = v->a;
            /* 字段值本身就指向当前栈帧（&.{} / &local / stackAlloc） */
            if (v && expr_frame(v)) return true;
            if (frame_in_agg(v, depth + 1)) return true;
        }
        return false;
    case NK_PAIR:
        return frame_in_agg(e->a, depth + 1);
    default:
        /* 普通字段 / 标识符 / 下标不在这里判——它们由 want 形状那条
         * expr_frame 路径负责；这里只补"聚合字面量内部藏着栈帧来源"。 */
        return false;
    }
}

static Type *check_assign(Node *n) {
    /* `_ = expr;` —— 显式丢弃（M26）：求值右值（拿副作用），不落存储。
     * 只认普通 `=`；复合赋值没有丢弃语义。 */
    if (n->op == TOK_ASSIGN && n->a->kind == NK_IDENT &&
        !strcmp(n->a->name, "_") && !n->a->sym) {
        Type *rt = check_expr(n->b, NULL);
        if (rt->kind == TY_VOID)
            return BAD(n, "`_ =` needs a value; the right side is already void");
        return type_of(n, ty_void());
    }
    Type *lt = check_expr(n->a, NULL);
    Type *rt;
    if (n->op == TOK_ASSIGN) {
        rt = check_expr(n->b, lt);
        coerce_node(n->b, rt, lt, "assignment");
        /* 重新绑定过就不再信任初值（§138：分配器去虚拟化的前提是
         * "这个变量从声明到这里没被改过"）。 */
        if (n->a->kind == NK_IDENT && n->a->sym) n->a->sym->nassign++;
        /* `a.alloc = f` 改写的是分发目标本身，不能再按声明初值直调：
         * 沿字段/下标链找到基变量，同样打上 nassign（§148）。 */
        {
            Node *base = n->a;
            int guard = 0;
            while (base && (base->kind == NK_FIELD || base->kind == NK_INDEX) &&
                   guard++ < 8)
                base = base->a;
            if (base && base->kind == NK_IDENT && base->sym)
                base->sym->nassign++;
        }
        /* 赋进局部变量的指针也记来源：之后 return 它同样要拦。
         * 重算而非只置 true——先存栈地址再改成堆/全局地址必须洗掉旧事实。 */
        if (n->a->kind == NK_IDENT && n->a->sym)
            n->a->sym->stack_val = expr_frame(n->b);
        /* §130：赋值重新绑定发行版本；赋成别的值就把旧版本标 opaque */
        if (n->a->kind == NK_IDENT && n->a->sym) {
            int rs = n->b ? n->b->reg_slot : 0;
            n->a->sym->reg_slot = rs;
            if (rs) region_bind(rs, n->a->sym);
        }
        /* §170：赋值重新绑定贴标载体。防守写法 `p = tag.x : p;`（重申
         * LIVE）与普通赋值都走这里；赋成别的值则旧载体身份作废——
         * 与 region 的 opaque 口径一致，悬垂只能来自分支未传 null。 */
        if (n->a->kind == NK_IDENT && n->a->sym) {
            /* §187：污点按右值重算（赋成干净值即洗白，赋成洗源算术
             * 结果仍带污点） */
            n->a->sym->ptr_taint = expr_taint(n->b) != 0;
            int tid, tst;
            tag_of_expr(n->b, &tid, &tst);
            if (tid) {
                n->a->sym->tag_id = tid;
                n->a->sym->tag_state = tst;
                TagSlot *ts = tag_slot(tid);
                if (ts) {
                    ts->carrier = n->a->sym;
                    if (tst == TC_LIVE && n->b) {   /* 所有权落在这个赋值点 */
                        ts->own_line = n->b->line; ts->own_col = n->b->col;
                    }
                }
            } else if (n->a->sym->tag_id) {
                TagSlot *ts = tag_slot(n->a->sym->tag_id);
                if (ts && ts->carrier == n->a->sym) ts->carrier = NULL;
                n->a->sym->tag_id = 0;
                n->a->sym->tag_state = TC_NONE;
            }
        }
        /* §173：整结构体赋值重绑字段账本（赋成不带账的值就清掉，
         * 与 tag_id 的作废口径一致）；字段赋值 `s.f = expr` 只重绑
         * 该字段。臂快照含字段账（补丁五），臂内赋值可安全回滚。 */
        if (n->a->kind == NK_IDENT && n->a->sym) {
            FieldTag *fs2 = ftags_of_expr(n->b);
            if (!fs2 && n->b && n->b->kind == NK_FIELD) {
                /* §175：聚合抽取 let inner = tt.0——值是拷贝，字段账
                 * 子树跟着拷到新载体（"0.0"→槽 变 "0"→槽）。 */
                char key2[128];
                Node *fb2 = NULL;
                if (ft_path_key(n->b, key2, sizeof key2, &fb2) &&
                    fb2->kind == NK_IDENT && fb2->sym)
                    fs2 = ft_subtree((FieldTag *)fb2->sym->ftags, key2);
            }
            n->a->sym->ftags = fs2 ? ft_clone(fs2) : NULL;
            /* §192：转移溯源——来源表达式挂到载体上，之后的字段写入
             * 经 ft_mirror 回写源账本 */
            n->a->sym->ft_src = n->b;
        } else if (n->a->kind == NK_FIELD && n->a->a &&
                   !ft_num_elem(n->a)) {
            /* §173 补丁八：字段赋值（含嵌套 s.inner.name = q）——目标
             * 字段链折成点路径；右值是结构体值时把它的字段账拍平进
             * 前缀（s.inner = structval → "inner.name"→槽）。
             * §175：元组数字分量 tup.1 = expr 同走这里（name=NULL、
             * ival=下标）——ft_path_key 对基址是 TY_TUPLE 的数字节点
             * 放行、对基址是指针的（解引用）自行拒绝。 */
            char key[128];
            Node *fb = NULL;
            bool dtail = false;   /* §196：末端裸指针穿透（内容写） */
            {
            if (ft_path_key_d(n->a, key, sizeof key, &fb, &dtail) &&
                fb->kind == NK_IDENT && fb->sym && !dtail) {
                int ftid, ftst;
                tag_of_expr(n->b, &ftid, &ftst);
                Symbol *bs = fb->sym;
                ft_clear_prefix((FieldTag *)bs->ftags, key);
                ft_mirror(bs, key, 0, 0);
                if (ftid && (ftst == TC_LIVE || ftst == TC_FREED)) {
                    char *pk = ft_path_dup(NULL, key);
                    if (pk) ft_set((FieldTag **)&bs->ftags, pk, ftid);
                    ft_mirror(bs, key, ftid, 0);
                    /* §194/195：往形参容器的字段存入指针 → 记 pfs 事实账，
                     * 调用点按形实对位镜像到实参载体（见 tag_call_flow）。
                     * 两类来源分开处理：
                     * ① 本体内新分配（def_fn == 当前函数）→ clone 实例；
                     * ② §195 透传的抽象形参槽（如 `t[0].name = p`，p 是
                     *    另一形参）→ 调用点把 p 对应实参自身的槽烙到容器
                     *    实参字段账，不 clone（所有权本在调用方）。 */
                    if (bs->kind == SK_PARAM) {
                        TagSlot *sts = tag_slot(ftid);
                        if (sts && sts->abstract) {
                            /* 透传的抽象形参槽：调用点烙来源实参自身的槽。 */
                            Symbol *src = pslot_sym_find(ftid);
                            if (src && src != bs) pfs_add_ex(bs, key, ftid, src);
                        } else if (sts && g_cur_fn_name &&
                                   ((sts->def_fn && !strcmp(sts->def_fn, g_cur_fn_name)) ||
                                    (sts->own_fn && !strcmp(sts->own_fn, g_cur_fn_name)))) {
                            /* 本体内新分配（def_fn）或本体内调用工厂派生的实例槽
                             * （own_fn）——所有权已随调用流到本函数，调用点
                             * 可安全再 clone（母槽 ninst>0 终扫退役）。 */
                            pfs_add(bs, key, ftid);
                        }
                    }
                } else {
                    FieldTag *inft = ftags_of_expr(n->b);
                    for (FieldTag *g = inft; g; g = g->next) {
                        if (g->slot <= 0) continue;
                        char *pk = ft_path_dup(key, g->name);
                        if (pk) {
                            ft_set((FieldTag **)&bs->ftags, pk, g->slot);
                            ft_mirror(bs, pk, g->slot, 0);
                        }
                    }
                }
            } else if (fb && fb->kind == NK_INDEX &&
                       (!fb->b || fb->b->kind != NK_INT)) {
                /* §175：arr[i].name = tagged（运行期索引的嵌套字段）。
                 * §185：基址能解析到数组基变量 → 槽进该数组的标签群
                 * （存入即登记，注解读取取出，free 整容器批量终结）；
                 * 解析不了（指针别名等）→ 保守记 pending，续链/终结
                 * 接管，没处理终扫报 E0521。 */
                int ftid2, ftst2;
                tag_of_expr(n->b, &ftid2, &ftst2);
                if (ftid2) {
                    Symbol *bs2 = agt_base_of(fb->a);
                    TagSlot *ts = tag_slot(ftid2);
                    if (ts && bs2) {
                        agt_add(bs2, ftid2);
                        ts->arr_esc = true;
                    } else if (ts) {
                        ts->rt_pending = true;
                        ts->rt_line = fb->b ? fb->b->line : n->a->line;
                        ts->rt_col = fb->b ? fb->b->col : n->a->col;
                        ts->arr_esc = true;
                    }
                }
            }
            }
        } else if ((n->a->kind == NK_INDEX || ft_num_elem(n->a)) &&
                   n->a->a) {
            /* §173 补丁十：贴标指针进数组/元组元素认**常索引**精确账。
             * §185：运行期索引不再一律断线——基址能解析到数组基变量
             * （含别名切片）时槽进该数组的标签群（存入即登记）；解析
             * 不了才保守记 pending（续链/终结接管，终扫 E0521）。
             * §192：数组指针的 `.N` 点形式与 ptr[N] 同走这里。 */
            Symbol *dbs = (n->a->a->kind == NK_IDENT)
                          ? (n->a->a->sym && n->a->a->sym->kind != SK_CONST
                             ? n->a->a->sym : NULL)
                          : agt_base_of(n->a->a);
            if (dbs) {
                int ftid, ftst;
                tag_of_expr(n->b, &ftid, &ftst);
                if (ftid) {
                    int ix = n->a->kind == NK_INDEX
                             ? aesc_const_index(n->a->b) : (int)n->a->ival;
                    TagSlot *ts = tag_slot(ftid);
                    if (ix < 0) {
                        /* §185：运行期索引写入 → 槽进群。群成员由注解
                         * 读出（tag v : tag.x = xs[i];）逐个取出、
                         * free(xs) 整容器批量终结；没取走的由槽级
                         * 泄漏终扫 E0513 兜底（群里留着 = 未释放）。 */
                        agt_add(dbs, ftid);
                        if (ts) ts->arr_esc = true;
                    } else {
                        /* 常索引：aesc 精确账已覆盖（覆盖写入换账、
                         * 精确读/free）；不进群——群是"运行期索引"
                         * 成员的集合摘要，常索引的容器终结由
                         * agt_clear 扫 aesc 统一覆盖。 */
                        aesc_add(dbs, ix, ftid);
                        if (ts && ftst == TC_LIVE && n->b) {
                            ts->own_line = n->b->line; ts->own_col = n->b->col;
                        }
                    }
                }
            } else if (n->a->a->kind == NK_IDENT && n->a->a->sym) {
                /* 基址解析不了的别名写入：追踪必断线，保守记 pending
                 * （§174 口径不变：续链接管或终结，没处理终扫报
                 * E0521——标签绝不静默丢失）。 */
                int ftid, ftst;
                tag_of_expr(n->b, &ftid, &ftst);
                if (ftid) {
                    TagSlot *ts = tag_slot(ftid);
                    if (ts) {
                        ts->rt_pending = true;
                        ts->rt_line = n->a->b ? n->a->b->line : n->a->line;
                        ts->rt_col = n->a->b ? n->a->b->col : n->a->col;
                        ts->arr_esc = true;
                    }
                }
            }
        }
    } else if (lt && lt->kind == TY_PTR) {
        if (n->op != TOK_PLUSEQ && n->op != TOK_MINUSEQ)
            return BAD(n, "only `+=` and `-=` apply to pointers");
        rt = check_expr(n->b, NULL);
        if (ptr_offset_type(n, lt, rt, n->b)->kind == TY_INVALID) return ty_invalid();
    } else {
        rt = check_expr(n->b, lt);
        Type *r = check_num_operands(n, lt, rt);
        if (r->kind == TY_INVALID) return r;
        if (n->op == TOK_SHL || n->op == TOK_SHR) { /* 位移两边可以不同宽 */ }
    }
    if (!lvalue_mutable(n->a)) {
        if (n->a->kind == NK_IDENT)
            err_at(n->line, n->col, "cannot assign to '%s': it is not a `var`", n->a->name);
        else
            err_at(n->line, n->col, "cannot assign through a read-only path");
        return ty_invalid();
    }
    return type_of(n, lt);
}

/* ---------------- 可比较性（E0306） ----------------
 *
 * 后端把 `==` / `!=` 原样发成 C 的 `==` / `!=`，所以只有"在 C 里也是标量"
 * 的类型才成立。切片是 {ptr, len}、结构体/元组/联合体是 C 的 struct、非扁平
 * 的 `?T` / `!T` 是 {payload, tag}——这些在 C 里根本没有 `==`。原先这几种
 * 都放过去了，最后在生成 C 时被 C 编译器拒掉、报成 B1001「编译器 bug」
 * （假 bug，把用户引到错误的方向）。这里在类型检查阶段就拦下，并给出规范
 * 里本来就有的替代写法。 */
/* 浮点值能否无损装进位宽 bits 的整数目标。边界是 2 的幂，在 double 里精确，
 * 比较不含舍入误差；NaN 一律视为装不下。运行时（backend.c 的 zx_f2i*）
 * 用同一套边界，保证编译期与运行时同一答案（R10）。 */
static bool f64_fits_int(double v, int bits, bool sign) {
    if (v != v) return false;                    /* NaN */
    double hi = 1.0;
    for (int i = 0; i < bits; i++) hi *= 2.0;    /* 2^bits，精确 */
    double lo = 0.0;
    if (sign) { lo = -hi; hi = hi / 2.0; }       /* 有符号：[-2^(b-1), 2^(b-1)) */
    return v >= lo && v < hi;
}
static bool comparable_in_c(Type *t) {
    if (!t) return false;
    switch (t->kind) {
    case TY_BOOL: case TY_INT: case TY_FLOAT: case TY_ENUM: case TY_ERROR:
    case TY_PTR:  case TY_FN:  case TY_COMPTIME_INT: case TY_COMPTIME_FLOAT:
    case TY_NULL:
        return true;
    case TY_OPT:  return ty_opt_flat(t);   /* `?*T` 的 C 表示就是裸指针 */
    default:      return false;
    }
}

/* 按类型形状给一条能照做的建议：只说这个语言里真有的东西。 */
static void cmp_help(Type *t) {
    if (!t) return;
    switch (t->kind) {
    case TY_SLICE:
        if (t->elem && t->elem->kind == TY_INT && t->elem->bits == 8 &&
            !t->elem->is_signed)
            diag_help("slices compare identity, not contents; use `std.str.eq(a, b)`");
        else
            diag_help("slices compare identity, not contents; compare element by element");
        break;
    case TY_STRUCT: case TY_UNION:
        diag_help("compare field by field");
        break;
    case TY_TUPLE:
        diag_help("compare element by element");
        break;
    case TY_OPT: case TY_ERR:
        diag_help("test a `?T` with `if (o) v { ... } else { ... }`, "
                  "or unwrap it with `orelse` / `try`");
        break;
    case TY_TYPE:
        diag_help("type values live only at compile time; both sides must be "
                  "known while compiling");
        break;
    default:
        diag_help("only integers, floats, bools, enums, errors, pointers and "
                  "function values can be compared with `==`");
        break;
    }
}

/* §162：比较结果的类型——同 lane 数的**有符号整数**向量，每个分量 0 或 -1。
 * 宽度跟操作数走：f32x4 → i32x4，u8x16 → i8x16。clang 对 `a < b` 就是这么
 * 定义的，比较指令落到的也正是掩码寄存器。 */
static Type *vec_mask_ty(Type *v) {
    int bits = (int)(ty_size(v->elem) * 8);
    if (bits <= 0) bits = 8;
    return ty_vec(ty_int(bits, true), v->len);
}

static Type *check_binary(Node *n, Type *expect) {
    TokKind op = (TokKind)n->op;

    if (op == TOK_ASSIGN || op == TOK_PLUSEQ || op == TOK_MINUSEQ || op == TOK_STAREQ ||
        op == TOK_SLASHEQ || op == TOK_PERCEQ || op == TOK_AMPEQ || op == TOK_PIPEEQ ||
        op == TOK_CARETEQ || op == TOK_SHLEQ || op == TOK_SHREQ)
        return check_assign(n);

    Type *a = check_expr(n->a, (op == TOK_ANDAND || op == TOK_OROR) ? NULL : expect);
    Type *b = check_expr(n->b, expect);

    /* §187：算术不洗源——任一操作数带「指针出处」污点，结果仍带污点
     * （标签已丢，转回指针必须显式贴标，E0525）。 */
    n->ptr_taint = (unsigned char)(expr_taint(n->a) || expr_taint(n->b));

    /* 向量（§162）：两边必须**同一个**向量类型，没有隐式 splat——
     * 想把标量铺开就用 `(@Vec(T, N))x`。`/` 只对浮点向量开：C 的
     * vector_size 类型没有整数除法，硬放行会在 B1001 那一层才炸。 */
    if ((a && a->kind == TY_VEC) || (b && b->kind == TY_VEC)) {
        if (!ty_eq(a, b))
            return BAD(n, "vector arithmetic needs the same vector type on both sides, "
                          "found `%s` and `%s` -- write `(@Vec(T, N))x` to broadcast a scalar",
                       ty_str(a), ty_str(b));
        switch (op) {
        case TOK_PLUS: case TOK_MINUS: case TOK_STAR:
            return type_of(n, a);
        /* §162：比较得到**掩码向量**——每个分量 0 或 -1（全 1 位）。
         * C 的 `a < b` 本来就是这个形状（同 lane 数的有符号整数向量），
         * 也正是 SSE/AVX 掩码寄存器的形状。不展平成 bool 向量：C 没有
         * `_Bool` 向量（clang 报 invalid vector element type），而且还要
         * 多一趟压缩。掩码给 @select 用，也能直接当整数向量读分量。 */
        case TOK_EQ: case TOK_NE: case TOK_LT: case TOK_LE:
        case TOK_GT: case TOK_GE:
            return type_of(n, vec_mask_ty(a));
        case TOK_SLASH:
            if (!ty_is_float(a->elem))
                return BAD(n, "`/` is not defined on integer vectors (C vector types "
                              "have no integer division); convert the lanes to f32 first");
            return type_of(n, a);
        default:
            return BAD(n, "this operator is not defined on vectors (`+ - *`, "
                          "`/` on float lanes, and the six comparisons)");
        }
    }

    switch (op) {
    case TOK_ANDAND: case TOK_OROR:
        if (!ty_eq(a, ty_bool()) || !ty_eq(b, ty_bool()))
            return BAD(n, "`&&` / `||` need bool, found `%s` and `%s`", ty_str(a), ty_str(b));
        return type_of(n, ty_bool());

    case TOK_EQ: case TOK_NE: {
        /* `e == Err.read`：错误码和枚举本来就同源（都是整型码，Tag 是 error，
         * 枚举底层是整型）。要求先写 `(Err)e` 只是给读代码的人添噪音——
         * 放宽比较，但**不放宽赋值**（那还是 R4 要显式转换的地盘）。 */
        bool same_code_domain =
            (a->kind == TY_ERROR && b->kind == TY_ENUM) ||
            (a->kind == TY_ENUM  && b->kind == TY_ERROR);
        if (!ty_eq(a, b) && !same_code_domain && !can_coerce(a, b) && !can_coerce(b, a))
            return BAD(n, "cannot compare `%s` with `%s`", ty_str(a), ty_str(b));
        /* 类型值的比较是纯编译期的：`@fieldType(S, "b") == u32`。`type` 在
         * 运行时没有表示（spec §7：size 0，仅编译期），只能折叠成常量。
         * 以前这里直接放行，最后在 C 里比两个空壳——B1001 假 bug。 */
        if (a->kind == TY_TYPE && b->kind == TY_TYPE) {
            Value *va = n->a->ct ? n->a->ct : ct_eval_probe(n->a);
            Value *vb = n->b->ct ? n->b->ct : ct_eval_probe(n->b);
            if (va && vb && va->kind == V_TYPE && vb->kind == V_TYPE) {
                bool same = ty_eq(va->tval, vb->tval);
                bool r = (n->op == TOK_EQ) ? same : !same;
                n->kind = NK_BOOL;          /* 折叠成字面量：C 里没有 type */
                n->ival = r ? 1 : 0;
                n->ct = val_bool(r);
                n->ty = ty_bool();
                return n->ty;
            }
            cmp_help(a);
            diag_code_at("E0306", n->line, n->col,
                         "cannot compare `type` with `type`: "
                         "the type is not known at compile time");
            return ty_invalid();
        }

        /* 类型对上不代表 C 里能比：聚合体 / 切片 / 带标签的可选值在 C 里是
         * struct，发出去的 `==` 会被 C 编译器拒掉（旧行为是报 B1001）。
         * 错误哨兵不参与——级联错误不该再叠一条。
         *
         * 例外：两侧都能在编译期算出来的，折叠阶段会把整个比较换成常量，
         * 永远到不了 C。`@typeName(f.ty) == "u64"` 就是这种写法——字符串
         * 常量的比较由 val_bin 直接给出结果（V_STR 分支）。 */
        /* §197：`opt == null` / `null == opt` —— optional 与 null 的相等比较。
         * 语义是"判空"（非扁平看 tag、扁平 ?*T 看 0 指针），只放这一对，
         * `opt == 42` / `opt == other` 仍按下面的规则拒绝。null 侧定型成
         * optional 类型，供后端识别并发 `.tag` 判定（扁平 ?*T 走通用发射）。 */
        {
            Node *on = NULL; Type *ot = NULL; Node *nn = NULL;
            if (a->kind == TY_OPT && n->b && n->b->kind == NK_NULL) {
                on = n->a; ot = a; nn = n->b;
            } else if (b->kind == TY_OPT && n->a && n->a->kind == NK_NULL) {
                on = n->b; ot = b; nn = n->a;
            }
            if (on) {
                nn->ty = ot;                 /* null 侧定型为 optional */
                /* 两侧都能编译期算出时直接折成 bool 常量——有值的 ?T 载荷是
                 * V_INT/V_STR，空值是 V_NULL，不依赖 val_bin 的 V_NULL 规则
                 * （error 的 null 也走 V_NULL，那里按 65535 整数比，不动它）。 */
                Value *vo = n->ct ? n->ct : ct_eval_probe(on);
                if (vo && vo->kind != V_UNDEF) {
                    bool is_null = (vo->kind == V_NULL);
                    bool r = (n->op == TOK_EQ) ? is_null : !is_null;
                    n->kind = NK_BOOL;
                    n->ival = r ? 1 : 0;
                    n->ct = val_bool(r);
                    n->ty = ty_bool();
                    return n->ty;
                }
                return type_of(n, ty_bool());
            }
        }
        /* 非扁平可选值（`?u64` / `?struct`）在 C 里是 struct，语言层面也没有
         * `==`，只能 `if (o) v` / `orelse` 取出来再比。这条必须放在下面的
         * 折叠豁免**之前**：`std.str.indexOf(..) == 2` 的两侧都能被
         * ct_eval_probe 估出值，于是豁免生效、检查跳过，最后发到 C 端变成
         * `ZxOpt_usize == 2`（B1001，压力测试抓到的）。折叠豁免只管
         * "C 里能比但值不同"的情形，管不了"C 里根本没有 == "的类型。 */
        if ((a->kind == TY_OPT && !ty_opt_flat(a)) ||
            (b->kind == TY_OPT && !ty_opt_flat(b))) {
            Type *culprit = (a->kind == TY_OPT && !ty_opt_flat(a)) ? a : b;
            cmp_help(culprit);
            diag_code_at("E0306", n->line, n->col,
                         "cannot compare `%s` with `%s`: `%s` has no `%s`",
                         ty_str(a), ty_str(b), ty_str(culprit),
                         n->op == TOK_EQ ? "==" : "!=");
            return ty_invalid();
        }
        bool ct_both = (n->a->ct || ct_eval_probe(n->a)) &&
                       (n->b->ct || ct_eval_probe(n->b));
        if (!ct_both && a->kind != TY_INVALID && b->kind != TY_INVALID &&
            (!comparable_in_c(a) || !comparable_in_c(b))) {
            Type *culprit = comparable_in_c(a) ? b : a;
            cmp_help(culprit);
            diag_code_at("E0306", n->line, n->col,
                         "cannot compare `%s` with `%s`: `%s` has no `%s`",
                         ty_str(a), ty_str(b), ty_str(culprit),
                         n->op == TOK_EQ ? "==" : "!=");
            return ty_invalid();
        }
        /* `e == null`：null 在这一侧定成 error（= 保留码 65535）。不定型的话
         * 后端只能按"无类型 null"发 0，而 0 是"没有错误"——比较会静默成 false。
         * `p == null`（指针/可选）不受影响：那边的 null 侧类型不是 error。 */
        if (a->kind == TY_ERROR && n->b && n->b->kind == NK_NULL) n->b->ty = a;
        if (b->kind == TY_ERROR && n->a && n->a->kind == NK_NULL) n->a->ty = b;
        return type_of(n, ty_bool());
    }

    case TOK_LT: case TOK_LE: case TOK_GT: case TOK_GE: {
        Type *c = check_num_operands(n, a, b);
        if (c->kind == TY_INVALID) return c;
        return type_of(n, ty_bool());
    }

    case TOK_SHL: case TOK_SHR: {
        if (!ty_is_int(a) || !ty_is_int(b))
            return BAD(n, "shift needs integers, found `%s` and `%s`", ty_str(a), ty_str(b));
        return type_of(n, a);
    }

    case TOK_AMP: case TOK_PIPE: case TOK_CARET: {
        if (!ty_is_int(a) || !ty_is_int(b))
            return BAD(n, "bitwise ops need integers, found `%s` and `%s`", ty_str(a), ty_str(b));
        Type *c = unify(n, a, b);
        return type_of(n, c);
    }

    case TOK_PLUS: case TOK_MINUS: case TOK_STAR: case TOK_SLASH: case TOK_PERCENT: {
        if ((a && a->kind == TY_PTR) || (b && b->kind == TY_PTR)) {
            Type *r = check_ptr_arith(n, a, b, op);
            if (r) return r;
        }
        if (op == TOK_PERCENT && (ty_is_float(a) || ty_is_float(b)))
            return BAD(n, "`%%` needs integers");
        Type *c = check_num_operands(n, a, b);
        if (c->kind == TY_INVALID) return c;
        return type_of(n, c);
    }

    default:
        return BAD(n, "unsupported binary operator");
    }
}

/* &.{…}：一块匿名存储的地址。结果类型 *[N]T（R9），const 由绑定形式再决定。 */
static bool byte_str_ptr_ty(Type *t) {
    if (!t || t->kind != TY_PTR || !t->has_len || !t->elem) return false;
    Type *e = t->elem;
    if (e->kind == TY_ARRAY) e = e->elem;
    return e && e->kind == TY_INT && e->bits == 8 && !e->is_signed;
}

static Type *check_array_lit(Node *n, Node **items, int nitems, Type *expect) {
    Type *elem = NULL;
    if (expect) {
        /* §201：期待类型是 `*[N]T`，elem 是 [N]T，字面量的元素类型要再拆一层。 */
        if (expect->kind == TY_PTR && expect->elem) {
            elem = expect->elem;
            if (elem->kind == TY_ARRAY) elem = elem->elem;
        } else if (expect->kind == TY_SLICE) elem = expect->elem;
        else if (expect->kind == TY_ARRAY) elem = expect->elem;  /* §205：.{…} 值形式 */
    }
    for (int i = 0; i < nitems; i++) {
        Type *it = check_expr(items[i], elem);
        if (!elem) {
            Type *d = scalar_default(it);
            if (d->kind == TY_INVALID) return d;
            elem = d;
        } else if (byte_str_ptr_ty(it) && byte_str_ptr_ty(elem)) {
            /* 字符串数组：长度各不相同的字面量统一成 `[]const u8`
             *（用户 2026-11-09：字符串是 *[N]const u8；不统一的话
             * `["alpha", "beta"]` 每个元素都得显式转换）。 */
            elem = ty_slice(ty_u8(), it->is_const || elem->is_const);
        }
    }
    if (!elem) {
        bool have_expect = expect && (expect->kind == TY_PTR ||
                                      expect->kind == TY_ARRAY ||
                                      expect->kind == TY_SLICE);
        if (!have_expect)
            return BAD(n, "cannot infer the element type of an empty array literal; write the target type");
        elem = ty_invalid();     /* 空数组：靠期待类型 */
    }
    for (int i = 0; i < nitems; i++)
        coerce_node(items[i], items[i]->ty, elem, "array element");
    /* §205：`.{…}` 位置形式是 [N]T 数组值；`&.{…}` 是 *[N]T 指针。 */
    if (n->flag) return type_of(n, ty_array(elem, (uint64_t)nitems));
    /* §212：`&.{…}` 就是 stackAlloc 的糖——匿名存储落在当前栈帧，块退出
     * 即消亡。建 stack TagSlot，复用 tag_scope_exit（出块标 FREED）与
     * tag_check_deref（块外解引用报 E0512 use after scope），堵住
     * "pNext 链存 &.{…} 地址、块后进 vkQueueSubmit 崩溃" 这类悬垂指针。
     * 只在运行期函数体内建槽：全局字面量走静态 backing（§199，无栈帧），
     * CT 执行中的字面量也不该带运行期标签。 */
    if (g_fn_depth > 0 && g_ct_exec == 0)
        tag_auto_decl(n, true);
    return type_of(n, ty_ptr(ty_array(elem, (uint64_t)nitems), false, true,
                             (uint64_t)nitems));
}

static Type *check_unary(Node *n, Type *expect) {
    if (n->op == TOK_AMP) {
        if (n->a->kind == NK_TUPLE)      /* &{…}（不带点）也不该当元组 */
            return check_array_lit(n, n->a->items, n->a->nitems, expect);
        Type *t = check_expr(n->a, NULL);
        /* 取过地址：别人手里有它的指针，随时可能改写（§138 否定证据）。
         * 必须在 check_expr 之后——标识符的 sym 到那时才绑定；放在前面
         * 标记会落在空 sym 上，等于没标（§148 实测修正）。
         * `&a.alloc` 把分发字段的地址交出去：沿字段/下标链找到基变量，
         * 同样打 escaped，此后不能按声明初值直调。 */
        {
            Node *base = n->a;
            int guard = 0;
            while (base && (base->kind == NK_FIELD || base->kind == NK_INDEX) &&
                   guard++ < 8)
                base = base->a;
            if (base && base->kind == NK_IDENT && base->sym)
                base->sym->escaped = true;
        }
        /* `*[N]T` 只是"带长度的指针"：C 那边就是 `T *`，本身有槽位，
         * `&xs` 是合法的 `**(...)`（用户 2026-11-10 确认的设计）。
         * 以前这里错误地按"内联数组"挡掉，是文档/实现的误解。 */
        bool cst = !lvalue_mutable(n->a);
        /* §201：取数组值的地址就是普通指针，elem 是 TY_ARRAY */
        return type_of(n, ty_ptr(t, cst, false, 0));
    }

    Type *t;
    if (n->op == TOK_MINUS && n->a && n->a->kind == NK_INT) {
        g_neg_lit_ctx = true;            /* 允许字面量恰好是 max+1（INT_MIN） */
        t = check_expr(n->a, expect);
        g_neg_lit_ctx = false;
    } else
        t = check_expr(n->a, expect);
    /* §187：一元 `-`/`+`/`~` 不洗源——污点透传（`!` 产出 bool，重置）。 */
    n->ptr_taint = (unsigned char)
        (n->op != TOK_BANG && n->op != TOK_AMP && expr_taint(n->a));
    switch (n->op) {
    case TOK_MINUS: case TOK_PLUS:
        if (t && t->kind == TY_VEC) return type_of(n, t);   /* 逐分量取负 */
        if (!numeric_like(t)) return BAD(n, "unary `-` needs a number, found `%s`", ty_str(t));
        /* 负字面量不能落到无符号类型上（M19）：u8 = -1 是截断不是 -1 */
        if (n->op == TOK_MINUS && t && t->kind == TY_INT && !t->is_signed) {
            uint64_t v; bool neg; const char *text;
            if (lit_value(n->a, &v, &neg, &text) && v != 0)
                return BAD(n, "negative literal does not fit in unsigned type `%s`", ty_str(t));
        }
        return type_of(n, t);
    case TOK_TILDE:
        if (!ty_is_int(t)) return BAD(n, "`~` needs an integer, found `%s`", ty_str(t));
        return type_of(n, t);
    case TOK_BANG:
        if (!ty_eq(t, ty_bool())) return BAD(n, "`!` needs bool, found `%s`", ty_str(t));
        return type_of(n, ty_bool());
    default:
        return BAD(n, "unsupported unary operator");
    }
}

static bool castable(Type *from, Type *to) {
    if (!from || !to) return true;
    if (from->kind == TY_INVALID || to->kind == TY_INVALID) return true;
    if (ty_eq(from, to)) return true;
    if (from->kind == TY_UNDEF || from->kind == TY_NULL) return true;
    if (from->kind == TY_COMPTIME_INT)   return to->kind != TY_VOID && to->kind != TY_TYPE;
    if (from->kind == TY_COMPTIME_FLOAT) return to->kind != TY_VOID && to->kind != TY_TYPE;

    bool fnum = ty_is_num(from) || from->kind == TY_BOOL;
    bool tnum = ty_is_num(to)   || to->kind == TY_BOOL;
    if (fnum && tnum) return true;
    if (from->kind == TY_ERROR && (ty_is_int(to) || to->kind == TY_ENUM)) return true;
    if (from->kind == TY_ENUM  && (ty_is_int(to) || to->kind == TY_ERROR)) return true;
    if (from->kind == TY_PTR && (to->kind == TY_PTR || ty_is_int(to))) return true;
    if (ty_is_int(from) && to->kind == TY_PTR) return true;
    if (from->kind == TY_FN && to->kind == TY_PTR) return true;
    if (from->kind == TY_PTR && to->kind == TY_FN) return true;
    if (to->kind == TY_OPT && can_coerce(from, to->elem)) return true;
    if (to->kind == TY_ERR && (from->kind == TY_ERROR || can_coerce(from, to->elem))) return true;
    if (to->kind == TY_SLICE && from->kind == TY_SLICE) return true;
    /* 向量之间只允许同大小的重解释（i32x4 ↔ f32x4）：按位搬，不转换。 */
    if (from->kind == TY_VEC && to->kind == TY_VEC &&
        ty_size(from) == ty_size(to) && ty_size(from) > 0)
        return true;
    /* 标量 → 向量：把标量铺满所有分量（splat）。C 的 vector_size 类型
     * 不接受 scalar cast，后端把它发成 zx_splat_<后缀>(x)。 */
    if (to->kind == TY_VEC && ty_is_num(from)) return true;
    return false;
}

/* 枚举成员在编译期的值：显式写了的取 tag，没写的取下标（与布局规则一致）。
 * `(error)Err.x` 的成员访问节点上没有折叠值，得这样反查一次。
 * base 两种形状都收：类型标注位是 NK_TYPE_IDENT，表达式位是 NK_IDENT
 * （type_val 都会指向枚举类型本体，TY_ENUM 校验兜底）。 */
static bool enum_member_value(Node *e, int64_t *out) {
    if (!e || e->kind != NK_FIELD || !e->name) return false;
    if (!e->a || (e->a->kind != NK_TYPE_IDENT && e->a->kind != NK_IDENT)) return false;
    Type *tv = e->a->type_val;
    if (!tv || tv->kind != TY_ENUM) return false;
    for (int i = 0; i < tv->nfields; i++) {
        if (!tv->fields[i].name || strcmp(tv->fields[i].name, e->name)) continue;
        *out = (int64_t)(tv->fields[i].has_tag ? tv->fields[i].tag : (uint64_t)i);
        return true;
    }
    return false;
}

static Type *check_cast(Node *n) {
    Type *to = resolve_type(n->a);
    if (to->kind == TY_INVALID) return type_of(n, to);

    /* 别把目标类型当期待类型传下去：`(u8)(i + 1)` 里 i 是 usize，
     * 一旦把 u8 传给二元表达式，`1` 会变成 u8，加法就成了 u64 + u8。 */
    Type *from = check_expr(n->b, NULL);

    /* §170：贴标载体过 cast —— 指针到指针身份没变，照常携带；
     * 其余方向抹源，证明不了就是错（§135 口径）。 */
    {
        int tid, tst;
        tag_of_expr(n->b, &tid, &tst);
        bool to_ps = (to->kind == TY_PTR || to->kind == TY_SLICE);
        bool from_ps = (from->kind == TY_PTR || from->kind == TY_SLICE);
        bool to_int = (to->kind == TY_INT || to->kind == TY_ENUM ||
                       to->kind == TY_COMPTIME_INT);
        bool from_int = (from->kind == TY_INT || from->kind == TY_ENUM ||
                         from->kind == TY_COMPTIME_INT);
        /* §187：指针→整数，出处污点随值走——只染**账本追得到**的指针
         *（tid != 0）：账本没建槽的指针（&局部变量、豁免模块的内部
         * 分配）从来不在账上，整数往返不构成洗源；已建槽的指针转整数
         * 后经算术/重绑丢身份，转回指针必须显式贴标（E0525）。整数→
         * 整数，污点透传。 */
        if (from_ps && to_int)
            n->ptr_taint = (unsigned char)(tid != 0);
        else if (from_int && to_int)
            n->ptr_taint = (unsigned char)expr_taint(n->b);
        if (tid) {
            if (to_ps && from_ps) {
                /* §179：指针/切片家族互转身份不变——切片还是同一份内存
                 * 的视图（元素重解释不改归属）；释放责任跟着标签走。 */
                n->tag_id = tid;
                n->tag_state = tst;
            } else if (tag_slot(tid) && tag_slot(tid)->abstract) {
                /* §173：借出槽的摘要 cast（`(usize)p` 之类）不追身份 */
            } else if (!to_ps && to_int) {
                /* §186：整数中转不抹源——`(usize)p` 把地址当数字读，
                 * 出处（副本）随值走：原载体照旧记账，整数本身也携带
                 * 同一槽的身份。整数经算术运算即丢（binop 不透传 tag，
                 * 赋值重绑同样清掉）；直传转回 `(*u8)n` 时身份恢复——
                 * 抹源 double-free/UAF 的直线形态从此编译期可拦。 */
                n->tag_id = tid;
                n->tag_state = tst;
            } else if (to_ps && from_int) {
                /* §186：转回——从带出处的整数直接恢复身份（直传数据
                 * 流）。算术洗源后的整数不带出处，走下面的 E0514。 */
                n->tag_id = tid;
                n->tag_state = tst;
            } else if (!to_ps) {
                /* §179：cast 成其他非指针（f64 等）不转移所有权——
                 * 标签不跟着走，原载体照旧记账；这不是断链。 */
            } else {
                TagSlot *t = tag_slot(tid);
                diag_code_at("E0514", n->line, n->col,
                    "the label's provenance is lost: a labeled pointer cannot "
                    "be cast to `%s` -- keep it in a variable of the same "
                    "pointer type",
                    t ? t->name : "?", ty_str(to));
            }
        } else if (to_ps && from_int && expr_taint(n->b) && !g_in_tag_rebind
                   && !(g_mod && g_mod->tags_closed)) {
            /* §187：整数转指针需显式贴标（与数组取出同一口径）——这个
             * 整数曾由指针转来，出处已丢（算术洗源/赋值重绑），转回
             * 指针等于凭空造出一个"不知道是谁的"指针：双重释放、
             * 释放别人的内存都查不了账。续链接管：
             *   tag v : tag.x = (*T)n;
             * 干净整数（从未接触指针：字面量、C FFI 句柄）照旧放行，
             * 账本不追。`tag.close;` 的分配器实现模块自己管账（free
             * 里对收到的贴标指针做头部分析是合法操作），豁免。 */
            diag_code_at("E0525", n->line, n->col,
                "this integer came from a pointer, but its provenance was "
                "lost (arithmetic or re-assignment) -- an integer-to-pointer "
                "cast must be annotated with the label it belongs to: "
                "`tag v : tag.x = (%s)n;` re-attaches the tracking chain",
                ty_str(to));
        }
    }

    /* 错误码 0 就是"没有错误"。`(error)X` 里 X 在编译期能算出 0 的，
     * 等于把"出错了"写成"成功了"——而 `!T` 的成功分支还会去读一段
     * 未初始化的 payload。这类错编译过、跑起来答案错，必须在编译期拦下。
     * 命名错误码请从 1 起（`enum { open = 1, … }`），别用 0。
     * 65535 同样保留：那是 error.null（§5，空可选值 try 出来的错误码）。 */
    if (to->kind == TY_ERROR) {
        int64_t cv = 0;
        bool known = false;
        Value *v = n->b->ct;
        if (!v) v = ct_eval_probe(n->b);
        if (v && v->kind == V_INT) { cv = val_i64(v); known = true; }
        else if (enum_member_value(n->b, &cv)) known = true;
        /* 枚举错误名字登记（M57 统一回放）：码就是枚举值本身，把
         * "Err.x" 占位进码→名字表，errName / printTrace 就能和命名
         * 错误走同一张表解析。probe 折叠成功也要登记，所以放在
         * known 判定之后、不分分支。 */
        if (known && n->b && n->b->kind == NK_FIELD && n->b->name &&
            n->b->a && (n->b->a->kind == NK_TYPE_IDENT ||
                        n->b->a->kind == NK_IDENT) && n->b->a->name) {
            Buf nb; buf_init(&nb);
            buf_printf(&nb, "%s.%s", n->b->a->name, n->b->name);
            ty_err_code_at(nb.data, (uint16_t)cv);
        }
        if (known && cv == 0)
            return BAD_CODE("E0508", n, "error code 0 means \"no error\": this would be read as "
                                        "success and hand back an uninitialized payload -- "
                                        "number your error codes from 1");
        if (known && cv == 65535)
            return BAD_CODE("E0508", n, "error code 65535 is reserved for `error.null` -- "
                                        "the code a null optional throws when you `try` it (spec rule 5)");
    }

    if ((from->kind == TY_COMPTIME_INT || from->kind == TY_COMPTIME_FLOAT) &&
        (ty_is_int(to) || ty_is_float(to) || to->kind == TY_ENUM)) {
        n->b->ty = to;                     /* 字面量直接定型 */
        /* 字面量的值在 ival/fval 上，ct 清掉无所谓；但折过值的字段访问
         * （`(u16)@type(u8).size`）只有 ct 记得值——清了后端只会发出
         * 0 再挂个 `.size`，C 直接报 "invalid suffix on floating constant"。
         * 用户 2026-11-10 报的就是这一条。 */
        if (n->b->kind == NK_INT || n->b->kind == NK_FLOAT || n->b->kind == NK_STR)
            n->b->ct = NULL;
        from = to;
    }
    /* §162：向量字面量。parse 把 `@Vec(f32, 4){a, b, c, d}` 脱糖成
     * "把一个元组当值转换成向量"，定型就是这里。个数必须正好是 N，
     * 每个分量再按元素类型各自转换一次。 */
    if (to->kind == TY_VEC && from->kind == TY_TUPLE) {
        if ((uint64_t)from->nfields != to->len)
            return BAD(n, "vector literal needs %llu values, found %d",
                       (unsigned long long)to->len, from->nfields);
        for (int i = 0; i < from->nfields; i++) {
            Type *ft = from->fields[i].ty;
            if (!castable(ft, to->elem))
                return BAD(n, "vector literal value %d is `%s`, which cannot be cast to `%s`",
                           i + 1, ty_str(ft), ty_str(to->elem));
        }
        return type_of(n, to);
    }
    /* 位置列表写在结构体名后面（`P{1, 2}`）：parse 按向量字面量收下了
     * （见 parse_primary 的 `{` 分支），这里说清楚结构体要写字段名。 */
    if (from->kind == TY_TUPLE && (to->kind == TY_STRUCT || to->kind == TY_UNION))
        return BAD(n, "struct literal needs named fields: `%s`{ .a = 1, .b = 2 }", ty_str(to));
    if (!castable(from, to))
        return BAD(n, "cannot cast `%s` to `%s`", ty_str(from), ty_str(to));
    return type_of(n, to);
}

/* 成员访问：base.name / base.N */
/* 当前函数声明的错误集（NULL = 返回类型是 `!T`，即 anyerror，不检查）。 */
static void *cur_errset(void) {
    return (g_ret && g_ret->kind == TY_ERR) ? g_ret->eset : NULL;
}

/* 表达式可能抛出的错误集：
 *   - 类型是 `E!T` → E
 *   - `error.name` 字面量 → {name}
 *   - 其余（`!T` anyerror、`(error)Enum.x` 旧写法）→ NULL（未知，不检查） */
static void *expr_errset(Node *e, Type *t) {
    /* 节点形状先于类型：error.x / Err.no 的集合写在节点上，而不是折后
     * 的类型上。expect 传播会把 error 字面量提前定型成目标错误联合体
     * （t = want），这时再看 t，看到的是**函数声明的**集合——
     * `return error.read` 在 `error{no}!void` 里漏报 E0901，原因就是
     * 顺序：类型分支先命中，拿 want 的集合与自己比，永远“子集”。 */
    if (e && e->kind == NK_FIELD && e->name && e->a &&
        e->a->kind == NK_TYPE_IDENT) {
        /* Err.no：base 是命名错误集类型（`const Err = error{no}`），集合就是它。
         * 不认这个，`return Err.no` 的集合检查就哑了——折出来的类型是普通
         * error，看不出集合，检查只能跳过。 */
        Type *btv = e->a->type_val;
        if (btv && btv->kind == TY_ERRORSET) return btv->eset;
        if (e->a->name && !strcmp(e->a->name, "error"))
            return ty_errset_one(e->name);
    }
    if (t && t->kind == TY_ERR) return t->eset;
    return NULL;
}

/* 把集合 A 里不在 B 的名字拼成 "read, write"（诊断用；静态轮转缓冲）。 */
static const char *errset_missing(const void *a, const void *b) {
    static char bufs[4][256];
    static int rot;
    char *out = bufs[rot++ & 3];
    size_t k = 0;
    out[0] = 0;
    for (int i = 0; i < ty_errset_size(a); i++) {
        uint16_t c = ty_errset_code_at(a, i);
        if (ty_errset_has(b, c)) continue;
        const char *nm = ty_errset_name_at(a, i);
        k += (size_t)snprintf(out + k, 256 - k, "%s%s", k ? ", " : "", nm ? nm : "?");
        if (k > 200) break;
    }
    return out;
}

/* 关联常量（写在 struct/union 体里的 `const K: i32 = 7`）：值只依赖声明，
 * 与实例无关。`O.K` 走 TY_TYPE 分支，靠 ct_eval 折成字面量；`c.K` 以前只
 * 返回类型，后端照着普通字段发 `c.K`，而 C 的 struct 里根本没这个成员
 * ——B1001（用户 2026-11-10 报的编译器 bug）。这里把声明初值折到节点上，
 * 后端 ex() 的 `n->ct` 分支直接发字面量。静态 var 成员不折（它可变）。 */
static Type *static_member_ty(Node *n, Field *f) {
    if (f->is_static && !f->is_var && f->dflt) {
        Value *cv = f->ct ? f->ct : ct_eval_probe(f->dflt);   /* §117 值槽优先 */
        if (cv && f->ty) cv = val_cast(f->ty, cv);
        if (cv && cv->kind != V_UNDEF) n->ct = cv;
        /* 嵌套类型成员（`const Inner = struct {…};` / `const Sub = Other;`）：
         * 值本身就是类型，必须把 type_val 挂到节点上。只有 TY_TYPE 没有
         * type_val 的话，`t.p1.Inner.Sub.leaf` 第二段会拿 null 当 <error>
         * （用户 2026-11-11：实例链上只能读一层类型，再往下就不认识）。
         * 纯类型路径 `O.Inner.Sub` 走 check_field 的 TY_TYPE 分支，那里一直
         * 是好的——差别正是这条。 */
        if (cv && cv->kind == V_TYPE) n->type_val = cv->tval;
    }
    if (!n->type_val && f->ty && f->ty->kind == TY_TYPE && f->dflt &&
        f->dflt->ty && f->dflt->ty->kind == TY_TYPE && f->dflt->type_val)
        n->type_val = f->dflt->type_val;
    return type_of(n, f->ty);
}

static Type *check_field(Node *n) {
    Node *base = n->a;
    /* 投机类型解析会把 `@typeName(f.ty)` 的实参吃成 TYPE_IDENT(f).ty；
     * 若 f 其实是个值（编译期聚合值），把它还原成普通 IDENT 再走值检查。 */
    if (base && base->kind == NK_TYPE_IDENT && !base->sym) {
        Symbol *s = lookup(base->name);
        if (s) {
            ensure(s);
            if (s->state != 3 && !(s->ty && s->ty->kind == TY_TYPE))
                base->kind = NK_IDENT;
        }
    }
    Type *bt = check_expr(base, NULL);
    /* §170：`.0` 与经指针的字段访问都是使用点，查贴标状态。
     * 命名空间 / 类型值 / 聚合 base 不带 tag，查了也是 no-op。 */
    tag_check_deref(base);

    /* §118 字段值指针：f.ptr —— 描述符 Field 的 ptr 槽。类型是 *(f.ty)，
     * 基址/偏移挂在节点上（b / ival），后端发 ((fldty *)((char *)&base + off))。
     * `f.ptr.0` 解引用走普通 .0 路径：读值、写值（var 实例）、取址、传给
     * *T 形参全是现成机制。const 语境折不出运行期地址——ct_field 特判 NULL，
     * R1 会拦。 */
    if (bt == sema_type_field_type() && n->name && !strcmp(n->name, "ptr")) {
        Value *fv = fold(base);
        if (!fv || fv->kind != V_AGG || fv->nitems < 4 || !fv->items[3])
            return BAD(n, "`.ptr` needs a descriptor from `@type(lvalue)`; "
                          "`@type(SomeType)` has no instance to point into");
        Value *pv = fv->items[3];
        /* statics 的 ptr 槽（§118 续二次拍板）：xt=容器、off=成员下标、
         * xexpr 空——不依赖实例，@type(T) 与 @type(左值) 两种形态都有。
         * 节点挂 type_val/ival，`.0` 解引用在 TY_PTR 分支改写成 T.<成员>。 */
        if (!pv->xexpr && pv->xt) {
            n->type_val = pv->xt;
            n->ival = pv->off;
            n->b = NULL;
            Type *pt = ty_ptr(pv->ty->elem, pv->ty->is_const, false, 0);
            pt->is_fldptr = true;
            return type_of(n, pt);
        }
        if (!pv->xexpr)
            return BAD(n, "`.ptr` needs a descriptor from `@type(lvalue)`; "
                          "`@type(SomeType)` has no instance to point into");
        n->b = clone_tree(pv->xexpr);
        n->ival = pv->off;
        Type *pt = ty_ptr(pv->ty->elem, !lvalue_mutable(n->b), false, 0);
        pt->is_fldptr = true;
        return type_of(n, pt);
    }

    /* 类型值上的成员：Color.red / Counter.n / ShapeTag.circle */
    if (bt && bt->kind == TY_TYPE) {
        Type *tv = base->type_val ? base->type_val : ty_invalid();
        /* §172：tag.buf —— 标签句柄字面量（模仿 error.foo）。引用即声明：
         * 名字来自成员名，绑到 const 时符号名只是别名。节点带 tag_id 供
         * 贴标与状态机使用；类型是编译期-only，后端不发射。 */
        if (tv->kind == TY_TAG && n->name) {
            TagSlot *ts = tag_decl(n->name, n);
            n->tag_id = ts ? (int)(ts - g_tslots) + 1 : 0;
            return type_of(n, ty_tag());
        }
        /* error.open：错误码字面量。类型是 error（运行时就是一个 u16），
         * 值在编译期折成常量，集合信息（单元素）留在节点上供 try/return 检查。 */
        if (tv->kind == TY_ERROR && n->name) {
            uint16_t code = ty_err_code(n->name);
            n->ct = val_int(ty_error(), code, false);
            return type_of(n, ty_error());
        }
        /* Err.no：命名错误集类型的成员。集合里声明的每个名字都是编译期常量
         * （同 `error.name` 字面量）；不在集合里的报错，并拿集合现有名字给
         * did you mean。注意先用**名字**对集合查——ty_err_code 对没见过的
         * 名字会顺手分配新码，成员查找不能有这个副作用（污染 errName 表）。 */
        if (tv->kind == TY_ERRORSET && n->name) {
            const char *hit = NULL, *sug = NULL;
            int bd = 999;
            for (int i = 0; i < ty_errset_size(tv->eset); i++) {
                const char *c = ty_errset_name_at(tv->eset, i);
                if (!c) continue;
                if (!strcmp(c, n->name)) { hit = c; break; }
                if (name_close(n->name, c)) {
                    int d = edit_distance(n->name, c);
                    if (d < bd) { bd = d; sug = c; }
                }
            }
            if (!hit) {
                if (sug) {
                    diag_help("did you mean `%s`?", sug);
                } else if (ty_errset_size(tv->eset) > 0 &&
                           ty_errset_size(tv->eset) <= 8) {
                    /* 小集合直接把成员列出来：完全写错名字时比沉默有用 */
                    static char names[256];
                    size_t k = 0;
                    names[0] = 0;
                    for (int i = 0; i < ty_errset_size(tv->eset); i++) {
                        const char *c = ty_errset_name_at(tv->eset, i);
                        if (!c) continue;
                        int w = snprintf(names + k, sizeof names - k,
                                         "%s%s", k ? ", " : "", c);
                        if (w < 0 || k + (size_t)w >= sizeof names - 8) break;
                        k += (size_t)w;
                    }
                    diag_help("this error set declares: %s", names);
                }
                return BAD(n, "`%s` has no member '%s'", ty_str(tv), n->name);
            }
            n->ct = val_int(ty_error(), ty_err_code(hit), false);
            return type_of(n, ty_error());
        }
        if (n->name) {
            Field *f = ty_field(tv, n->name);
            if (!f) f = ty_static_member(tv, n->name);   /* 枚举体的 const 成员 */
            if (!f) {
                /* 命名空间模块 */
                if (base->sym && base->sym->kind == SK_NS) {
                    Symbol *m = ns_member(base->sym, n->name, n->line, n->col);
                    if (!m) return type_of(n, ty_invalid());
                    n->sym = m;
                    /* 内建函数在调用点特判；std.heap / std.file 是命名空间值，
                     * 继续挂上去让 `.page` / `.write` 还能接着查一层 */
                    if (m->builtin == BI_FILE || m->builtin == BI_IO ||
                        m->builtin == BI_PROC || m->builtin == BI_DEBUG ||
                        m->builtin == BI_TASK || m->builtin == BI_SYNC) {
                        n->type_val = ty_void();
                        return type_of(n, ty_type());
                    }
                    if (m->builtin && m->builtin != BI_HEAP &&
                        m->builtin != BI_FILE_FN && m->builtin != BI_IO_FN &&
                        m->builtin != BI_PROC_FN && m->builtin != BI_DEBUG_FN &&
                        m->builtin != BI_TASK_FN && m->builtin != BI_SYNC_FN)
                        return type_of(n, ty_void());
                    ensure(m);
                    if (m->ty && m->ty->kind == TY_TYPE) {
                        n->type_val = m->type_val;
                        return type_of(n, ty_type());
                    }
                    return type_of(n, m->ty ? m->ty : ty_invalid());
                }
                Method *mth = ty_method(tv, n->name);
                if (mth) {
                    if (deny_member(n, tv, mth->is_pub, n->name)) return type_of(n, ty_invalid());
                    n->inst = mth->fn; n->flag = mth->has_self; return type_of(n, mth->fnty);
                }
                {
                    const char *sug = suggest_member(n->name, tv);
                    if (sug) diag_help("did you mean `%s`?", sug);
                }
                return BAD(n, "`%s` has no member '%s'", ty_str(tv), n->name);
            }
            if (deny_member(n, tv, f->is_pub, n->name)) return type_of(n, ty_invalid());
            if (tv->kind == TY_ENUM && !f->is_static) return type_of(n, tv);
            if (!f->is_static)
                return BAD(n, "'%s' is an instance field; construct a value first", n->name);
            /* 嵌套类型成员（`const Inner = struct { … };`）：成员的值就是这个
             * 类型。只返回 TY_TYPE 不够——`O.Inner{ .z = 1 }` 的构造、`var x:
             * O.Inner` 的标注都要 type_val，缺了它构造函数拿不到目标类型，
             * 后端就发出未声明的变量（B1001，用户 2026-11-10）。 */
            if (f->ty && f->ty->kind == TY_TYPE) {
                if (f->dflt && f->dflt->ty && f->dflt->ty->kind == TY_TYPE)
                    n->type_val = f->dflt->type_val;
                return type_of(n, ty_type());
            }
            return type_of(n, f->ty);
        }
        return BAD(n, "`%s` needs a field name", ty_str(tv));
    }

    if (!bt || bt->kind == TY_INVALID) return type_of(n, ty_invalid());

    /* .len / .size —— 编译器字段 */
    if (n->name && (!strcmp(n->name, "len") || !strcmp(n->name, "size"))) {
        /* §201：字符串字面量 `"abc".len/.size` 是字面量语法糖，不是
         * `*[N]T` 的通用内建。数组指针已经是普通指针，不再提供这些成员。 */
        if (n->a && n->a->kind == NK_STR)
            return type_of(n, ty_usize());
        if (bt->kind == TY_SLICE) return type_of(n, ty_usize());
        if (bt->kind == TY_TUPLE) return type_of(n, ty_comptime_int());
        if (bt->kind == TY_ARRAY) return type_of(n, ty_comptime_int());  /* §201 */
        if (bt->kind == TY_VEC) return type_of(n, ty_usize());
        /* §197：裸指针 elem 有同名字段就走字段，否则给人话诊断。
         * `*[N]T`（elem 是数组）落到下面 TY_PTR 分支按普通字段查找。 */
        if (bt->kind == TY_PTR && !bt->has_len && !ty_is_array_ptr(bt) &&
            !ty_field(bt->elem, n->name))
            return BAD(n, "`%s` has no count; use []T", ty_str(bt));
    }

    switch (bt->kind) {
    case TY_VEC: {
        /* v.0 / v.3：编译期分量下标。运行期下标走 v[i]（check_index）。 */
        if (!n->name) {
            if (n->ival >= bt->len)
                return BAD(n, "index %llu out of range for `%s`",
                           (unsigned long long)n->ival, ty_str(bt));
            return type_of(n, bt->elem);
        }
        return BAD(n, "`%s` has no member `%s`", ty_str(bt), n->name ? n->name : "?");
    }
    case TY_PTR: {
        Type *e = bt->elem;
        if (!n->name) {
            if (ty_is_array_ptr(bt)) {
                /* §201：*[N]T 指向数组，`.0` = 解引用取整个数组 [N]T，
                 * 于是 `p.0[i]` / `p[0][i]` 都是取元素。其他 .N 不认。 */
                if (n->ival == 0) return type_of(n, e);   /* e = [N]T */
                return BAD(n, "array pointer only has `.0` (the whole array); "
                              "write `p.0[%llu]` for an element",
                           (unsigned long long)n->ival);
            }
            if (n->ival != 0)
                return BAD(n, "a bare pointer only has `.0` (dereference); use [] for indexing");
            /* §118 续：statics 的 ptr（b==NULL、type_val=容器、ival=成员下标）
             * 的 `.0` 在这里改写成 `T.<成员>`——const 折字面量（§117 值槽），
             * var 发全局引用；读写、取址与直接写 `Cnt.count` 同一条路。
             * 不能重入 check_field：合成基节点没有 sym，按名字现查会串。 */
            if (bt->is_fldptr && !base->b && base->type_val) {
                Type *ct2 = base->type_val;
                uint64_t mi = base->ival;
                Field *sf = ct2->kind == TY_ENUM
                          ? (mi < (uint64_t)ct2->nstatics ? &ct2->statics[mi] : NULL)
                          : (mi < (uint64_t)ct2->nfields ? &ct2->fields[mi] : NULL);
                if (!sf || !sf->name)
                    return BAD(n, "internal: bad statics ptr slot");
                Node *tb = node_new(NK_IDENT, n->line, n->col);
                tb->ty = ty_type();
                tb->type_val = ct2;
                n->a = tb;
                n->name = sf->name;
                n->ival = 0;
                if (deny_member(n, ct2, sf->is_pub, sf->name))
                    return type_of(n, ty_invalid());
                if (!sf->is_var && sf->dflt) {
                    Value *cv = sf->ct ? sf->ct : ct_eval_probe(sf->dflt);
                    if (cv && sf->ty) cv = val_cast(sf->ty, cv);
                    if (cv && cv->kind != V_UNDEF) n->ct = cv;
                }
                return type_of(n, sf->ty);
            }
            return type_of(n, e);
        }
        /* `.` 自动穿透一层 */
        if (n->name && !strcmp(n->name, "tag")) {
            if (e->kind == TY_UNION)
                return type_of(n, e->is_tagged && e->tag_ty ? e->tag_ty : ty_error());
            if (e->kind == TY_OPT || e->kind == TY_ERR)
                return type_of(n, e->kind == TY_OPT ? ty_bool() : ty_error());
        }
        /* §197：字符串字面量的 `.ptr` 是字面量语法糖，取首字节指针。
         * §201：普通 `*[N]T` 没有 `.ptr` 内建——它是普通指针，`p[0]`
         * / `p.0` 取出的整个数组才有 `.ptr`。pointee 自己叫 ptr 的字段
         * 照常由下面的 ty_field 命中，不被内建遮蔽。 */
        if (n->name && !strcmp(n->name, "ptr") && n->a && n->a->kind == NK_STR)
            return type_of(n, ty_ptr(bt->elem, bt->is_const, false, 0));
        Field *f = ty_field(e, n->name);
        if (!f) {
            Method *m = ty_method(e, n->name);
            if (m) {
                if (deny_member(n, e, m->is_pub, n->name)) return type_of(n, ty_invalid());
                n->inst = m->fn; n->flag = m->has_self; return type_of(n, m->fnty);
            }
            {
                const char *sug = suggest_member(n->name, e);
                if (sug) diag_help("did you mean `%s`?", sug);
            }
            return BAD(n, "`%s` has no member '%s'", ty_str(e), n->name);
        }
        if (deny_member(n, e, f->is_pub, n->name)) return type_of(n, ty_invalid());
        if (f->is_static) return static_member_ty(n, f);
        return type_of(n, f->ty);
    }

    case TY_ARRAY: {                  /* §201：[N]T 数组值 */
        if (n->name && !strcmp(n->name, "ptr"))
            return type_of(n, ty_ptr(bt->elem, false, false, 0));  /* 首元素 *T */
        if (!n->name)
            return BAD(n, "array `.N` indexing was removed: write `a[i]`");
        return BAD(n, "an array only has .ptr / .len / .size");
    }

    case TY_SLICE:
        if (n->name && !strcmp(n->name, "ptr"))
            return type_of(n, ty_ptr(bt->elem, bt->is_const, false, 0));
        if (!n->name)
            /* §193：切片同数组，下标只认 `[i]`（`.0` 与元组分量混淆）。 */
            return BAD(n, "slice `.N` indexing was removed: write `s[i]` "
                          "-- dot-index stays only for tuples and vectors");
        return BAD(n, "a slice only has .ptr / .len");

    case TY_TUPLE: {
        if (!n->name) {
            if (n->ival >= (uint64_t)bt->nfields)
                return BAD(n, "tuple index %llu out of range (len %d)",
                           (unsigned long long)n->ival, bt->nfields);
            return type_of(n, bt->fields[n->ival].ty);
        }
        Field *f = ty_field(bt, n->name);
        if (!f) return BAD(n, "tuple has no field '%s'", n->name);
        return type_of(n, f->ty);
    }

    case TY_STRUCT: {
        if (!n->name) return BAD(n, "struct fields are accessed by name");
        Field *f = ty_field(bt, n->name);
        if (!f) {
            Method *m = ty_method(bt, n->name);
            if (m) {
                if (deny_member(n, bt, m->is_pub, n->name)) return type_of(n, ty_invalid());
                n->inst = m->fn; n->flag = m->has_self; return type_of(n, m->fnty);
            }
            {
                const char *sug = suggest_member(n->name, bt);
                if (sug) diag_help("did you mean `%s`?", sug);
            }
            return BAD(n, "`%s` has no field '%s'", ty_str(bt), n->name);
        }
        if (deny_member(n, bt, f->is_pub, n->name)) return type_of(n, ty_invalid());
        /* 静态类型成员（`const Inner = struct {…};` / `const Sub = Other;`）：
         * 成员的值就是这个类型本身，type_val 直接取声明上的。必须放在下面
         * "编译期聚合值的 type 字段" 分支**之前**——那个分支是去读运行期值
         * 的字段（std.Field.ty 这种），对普通实例只能拿到 invalid，于是
         * `t.p.Inner.Sub` 的第二段就变成 `\`<error>\` has no member`
         *（用户 2026-11-11：实例链上只能读一层类型，再往下就不认识）。 */
        if (f->is_static && f->ty && f->ty->kind == TY_TYPE && f->dflt &&
            f->dflt->ty && f->dflt->ty->kind == TY_TYPE && f->dflt->type_val) {
            n->ty = ty_type();
            n->type_val = f->dflt->type_val;
            return type_of(n, ty_type());
        }
        /* 编译期聚合值里的 type 字段（std.Field.ty）：字段值本身就是
         * 一个类型，把它折出来挂到 type_val 上，这个表达式才能当类型用。 */
        if (f->ty && f->ty->kind == TY_TYPE) {
            n->ty = ty_type();
            /* 只想问一嘴值，失败不能污染 g_ct_stuck（见 ct_eval_probe） */
            bool save_stuck = g_ct_stuck;
            CtFlow save_flow = g_ct_flow;
            Value *cv = ct_field(n);
            g_ct_stuck = save_stuck;
            g_ct_flow = save_flow;
            n->type_val = (cv && cv->kind == V_TYPE) ? cv->tval : ty_invalid();
            return type_of(n, ty_type());
        }
        if (f->is_static) return static_member_ty(n, f);
        return type_of(n, f->ty);
    }

    case TY_UNION: {
        if (!n->name) return BAD(n, "union members are accessed by name");
        if (!strcmp(n->name, "tag"))
            return type_of(n, bt->is_tagged && bt->tag_ty ? bt->tag_ty : ty_error());
        Field *f = ty_field(bt, n->name);
        if (!f) {
            Method *m = ty_method(bt, n->name);
            if (m) {
                if (deny_member(n, bt, m->is_pub, n->name)) return type_of(n, ty_invalid());
                n->inst = m->fn; n->flag = m->has_self; return type_of(n, m->fnty);
            }
            return BAD(n, "`%s` has no member '%s'", ty_str(bt), n->name);
        }
        if (deny_member(n, bt, f->is_pub, n->name)) return type_of(n, ty_invalid());
        if (f->is_static) return static_member_ty(n, f);
        return type_of(n, f->ty);
    }

    case TY_ENUM: {
        if (!n->name)
            return BAD(n, "an enum value has no `.N` field; cast it to an integer instead");
        Field *f = ty_static_member(bt, n->name);      /* 枚举体的 const 成员 */
        if (f) {
            if (deny_member(n, bt, f->is_pub, n->name)) return type_of(n, ty_invalid());
            return type_of(n, f->ty);
        }
        Method *m = ty_method(bt, n->name);
        if (m) {
            if (deny_member(n, bt, m->is_pub, n->name)) return type_of(n, ty_invalid());
            n->inst = m->fn; n->flag = m->has_self; return type_of(n, m->fnty);
        }
        return BAD(n, "`%s` has no member '%s'", ty_str(bt), n->name);
    }

    case TY_OPT: case TY_ERR:
        if (n->name && !strcmp(n->name, "tag"))
            return type_of(n, bt->kind == TY_OPT ? ty_bool() : ty_error());
        if (!n->name && n->ival == 0) return type_of(n, bt->elem);
        if (n->name && (!strcmp(n->name, "payload") || !strcmp(n->name, "some") ||
                        !strcmp(n->name, "ok")))
            return type_of(n, bt->elem);
        return BAD(n, "`%s` only has .0 / .tag", ty_str(bt));

    default:
        return BAD(n, "`%s` has no members", ty_str(bt));
    }
}

static Type *check_index(Node *n) {
    Type *bt = check_expr(n->a, NULL);
    tag_check_deref(n->a);               /* §170：use-after-free / 未防守 */
    Type *it = check_expr(n->b, ty_usize());
    if (!ty_is_int(it)) return BAD(n->b, "index must be an integer, found `%s`", ty_str(it));
    if (bt->kind == TY_ARRAY) {
        /* §201：数组值下标取元素，N 写在类型里 → 编译期越界检查。 */
        Value *iv = n->b->ct ? n->b->ct : ct_eval_probe(n->b);
        if (iv && iv->kind == V_INT) {
            int64_t x = val_i64(iv);
            if (x < 0 || (uint64_t)x >= bt->len)
                return BAD(n->b, "index %lld is out of bounds (len %llu)",
                           (long long)x, (unsigned long long)bt->len);
        }
        return type_of(n, bt->elem);
    }
    if (ty_is_array_ptr(bt)) {
        /* §201：`p: *[N]T` 的 `p[i]` 取的是**第 i 个数组**（elem 就是 [N]T）。
         * N 是数组内元素数，不是数组个数，编译期界检查不适用——数组指针
         * 没携带数组个数，运行期也不查。 */
        return type_of(n, bt->elem);   /* = [N]T */
    }
    if (bt->kind == TY_PTR) return type_of(n, bt->elem);
    if (bt->kind == TY_SLICE) return type_of(n, bt->elem);
    if (bt->kind == TY_VEC) {
        /* 向量下标：编译期常量能确定越界就现在报；运行期下标合法
         * （C 的 vector 类型本来就支持 v[i]）。 */
        Value *iv = n->b->ct ? n->b->ct : ct_eval_probe(n->b);
        if (iv && iv->kind == V_INT) {
            int64_t x = val_i64(iv);
            if (x < 0 || (uint64_t)x >= bt->len)
                return BAD(n->b, "index %lld is out of bounds (len %llu)",
                           (long long)x, (unsigned long long)bt->len);
        }
        return type_of(n, bt->elem);
    }
    if (bt->kind == TY_TUPLE) {
        /* tup[N]：N 必须是编译期值。元组是异构的——第 i 个元素和第 i+1 个
         * 类型不同，运行时索引给不出单一返回类型，所以不做成运行时下标。
         * 折值成功就地改写成 t.N（NK_FIELD，name=NULL+ival），与 .N 共用
         * 同一条 field 链路；遍历配 inline for（循环变量是编译期绑定）。 */
        Value *iv = n->b->ct ? n->b->ct : ct_eval_probe(n->b);
        if (iv && iv->kind == V_INT && !iv->neg) {
            uint64_t x = val_i64(iv);
            if (x >= (uint64_t)bt->nfields)
                return BAD(n->b, "tuple index %llu is out of range (len %d)",
                           (unsigned long long)x, bt->nfields);
            n->kind = NK_FIELD;
            n->name = NULL;
            n->ival = x;
            n->b = NULL;
            return type_of(n, bt->fields[x].ty);
        }
        return BAD(n->b, "tuple index must be a compile-time value; tuples are " 
                    "heterogeneous, so runtime indexing cannot type-check -- "
                    "use .N or `inline for` with a constant");
    }
    return BAD(n, "`%s` cannot be indexed", ty_str(bt));
}

static Type *check_slice_expr(Node *n) {
    Type *bt = check_expr(n->a, NULL);
    Type *elem = NULL;
    /* §201：*[N]T / [N]T 切片时元素是 T（elem 是 [N]T，取它的 elem） */
    if (ty_is_array_ptr(bt))
        elem = bt->elem->elem;
    else if (bt->kind == TY_PTR && bt->has_len)
        elem = bt->elem;                 /* 字符串字面量 */
    else if (bt->kind == TY_ARRAY) elem = bt->elem;
    else if (bt->kind == TY_SLICE) elem = bt->elem;
    else if (bt->kind == TY_PTR) {
        /* 裸指针没有数量：必须自己给上界（`p[0..n]`）。
         * 这是分配器回来的 `*u8` 变成 `[]T` 的唯一通道。 */
        if (!n->c)
            return BAD(n, "`%s` has no length; write `p[lo..hi]` with an explicit upper bound",
                       ty_str(bt));
        elem = bt->elem;
    }
    else
        return BAD(n, "`%s` cannot be sliced", ty_str(bt));

    if (n->b) {
        Type *lo = check_expr(n->b, ty_usize());
        if (!ty_is_int(lo)) return BAD(n->b, "slice bound must be an integer");
    }
    if (n->c) {
        Type *hi = check_expr(n->c, ty_usize());
        if (!ty_is_int(hi)) return BAD(n->c, "slice bound must be an integer");
    }
    /* 编译期已知的上下界：顺序与范围都能提前抓，不必等运行时 trap。 */
    uint64_t total = ty_is_array_ptr(bt) ? bt->elem->len
                   : (bt->kind == TY_ARRAY || bt->has_len) ? bt->len : 0;
    if (total > 0 || ty_is_array_ptr(bt)) {
        Value *lv = n->b ? (n->b->ct ? n->b->ct : ct_eval_probe(n->b)) : NULL;
        Value *hv = n->c ? (n->c->ct ? n->c->ct : ct_eval_probe(n->c)) : NULL;
        bool lok = !n->b || (lv && lv->kind == V_INT);
        bool hik = !n->c || (hv && hv->kind == V_INT);
        int64_t lov = lok ? (n->b ? val_i64(lv) : 0) : 0;
        int64_t hiv = hik ? (n->c ? val_i64(hv) : (int64_t)total) : 0;
        if (lok && n->b && lov < 0)
            return BAD(n->b, "slice lower bound %lld is negative", (long long)lov);
        if (hik && n->c && hiv > (int64_t)total)
            return BAD(n->c, "slice upper bound %lld is out of bounds (len %llu)",
                       (long long)hiv, (unsigned long long)total);
        if (lok && hik && n->b && n->c && lov > hiv)
            return BAD(n, "slice lower bound %lld is greater than upper bound %lld",
                       (long long)lov, (long long)hiv);
    }
    bool cst = (bt->kind == TY_PTR) ? bt->is_const : bt->is_const;
    return type_of(n, ty_slice(elem, cst));
}

/* 被调用的东西是不是泛型函数（按名字找得到，或者是一个泛型 fn 字面量） */
/* 命名空间成员（std.array.List / util.make）：拿到模块里那个符号 */
static Symbol *ns_member_symbol(Node *field) {
    if (!field || field->kind != NK_FIELD || !field->a) return NULL;
    Symbol *ns = NULL;
    /* 表达式位置 base 是 IDENT，类型位置（std.map.HashMap(i32)）是
     * TYPE_IDENT；再往深（std.map.HashMap）base 还是 FIELD——递归到根，
     * 每一级都必须解析成模块命名空间，否则不算模块成员链。 */
    if (field->a->kind == NK_IDENT || field->a->kind == NK_TYPE_IDENT) {
        ns = lookup(field->a->name);
    } else if (field->a->kind == NK_FIELD) {
        ns = ns_member_symbol(field->a);
    } else {
        return NULL;
    }
    if (!ns) return NULL;
    /* 惰性模块别名此刻可能还是未解析的 SK_CONST（文件根声明推迟了
     * @mod("...") 的加载），先按需解析成 SK_NS 再看成员。 */
    ensure(ns);
    if (ns->kind != SK_NS || !ns->mod) return NULL;
    return module_member(ns->mod, field->name);
}

static bool callee_is_generic(Node *callee) {
    if (!callee) return false;
    if (callee->kind == NK_FN) return fn_is_generic(callee);
    if (callee->kind == NK_FIELD) {
        Symbol *m = ns_member_symbol(callee);
        if (!m) return false;
        ensure(m);
        if (m->kind == SK_FN && m->decl) return fn_is_generic(m->decl);
        if (m->ct && m->ct->kind == V_FN) return fn_is_generic(m->ct->fn);
        return false;
    }
    if (callee->kind != NK_IDENT) return false;
    Symbol *s = lookup(callee->name);
    if (!s) return false;
    ensure(s);
    if (s->kind == SK_FN && s->decl) return fn_is_generic(s->decl);
    if (s->ct && s->ct->kind == V_FN) return fn_is_generic(s->ct->fn);
    return false;
}

/* 实例的返回类型值（类型构造函数用） */
static Type *instance_result(Node *g) {
    for (int i = 0; i < g_ninst; i++)
        if (g_insts[i].clone == g)
            return g_insts[i].val && g_insts[i].val->kind == V_TYPE ? g_insts[i].val->tval : NULL;
    return NULL;
}

/* 动词和实参类型对不对得上。对不上就在编译期说清楚——不然会生成一段
 * 编译不过的 C（`{s}` 去读结构体的 .len）。 */
static void check_fmt_verb(Node *fmt, Node *arg, int verb) {
    Type *t = arg ? arg->ty : NULL;
    if (!t || t->kind == TY_INVALID) return;

    bool integer = ty_is_int(t) || t->kind == TY_ENUM || t->kind == TY_ERROR ||
                   t->kind == TY_BOOL || t->kind == TY_COMPTIME_INT;

    if (verb == 's') {
        /* 字符串字面量（`*[N]const u8`）：`{s}` 打印字节序列，不是地址。
         * byte_str_ptr_ty 同时认旧的 elem=u8 与新的 elem=[N]u8 两种内部形状。 */
        if (t->kind != TY_SLICE && !byte_str_ptr_ty(t))
            err_at(fmt->line, fmt->col, "format specifier `{s}` needs a slice, found `%s`", ty_str(t));
    } else if (verb == 'c') {
        if (!integer)
            err_at(fmt->line, fmt->col, "format specifier `{c}` needs an integer, found `%s`", ty_str(t));
    } else if (verb == 'b') {
        if (!integer)
            err_at(fmt->line, fmt->col, "format specifier `{b}` needs an integer, found `%s`", ty_str(t));
    } else if (verb == 'd' || verb == 'x') {
        if (!integer && !ty_is_float(t))
            err_at(fmt->line, fmt->col, "format specifier `{%c}` cannot print `%s`", verb, ty_str(t));
    }
}

/* 格式串是编译期常量时，占位符个数/下标/动词类型现在就查清楚（运行时路径同理）。
 * 判不了的（fmt 是变量）留给运行时——语义一样，spec §11.1 第 3 条。 */
static void check_fmt_args(Node *call, int first, int nargs) {
    Node *fmt = call->items[first];
    const char *p = fmt->str;
    int len = fmt->str_len;
    if (!p && fmt->ct && fmt->ct->kind == V_STR) { p = fmt->ct->sval; len = fmt->ct->slen; }
    if (!p) return;

    int argi = 0;
    int hi = -1;              /* 已用到的最大实参下标：纯 {N} 索引的格式串
                                 位置游标不走，多给与否得看这个 */
    for (int i = 0; i < len; i++) {
        if (p[i] == '{' && i + 1 < len && p[i + 1] == '{') { i++; continue; }
        if (p[i] == '}' && i + 1 < len && p[i + 1] == '}') { i++; continue; }
        if (p[i] != '{') continue;
        int j = i + 1;
        while (j < len && p[j] != '}') j++;
        if (j >= len) { err_at(fmt->line, fmt->col, "unterminated `{` in format string"); return; }
        int clen = j - i - 1;
        const char *c = p + i + 1;
        /* 选项在冒号后面：{verb:opts} / {N:opts} / {:opts}（M26）。
         * 选项本身的语法（宽度上限、fill/align 组合）在后端 fmt_parse_opts
         * 里判；这里只剥离，让动词检查继续看冒号前面的部分。 */
        const char *colon = (const char *)memchr(c, ':', (size_t)clen);
        int plen = colon ? (int)(colon - c) : clen;
        int which, verb = 0;
        if (plen == 0) which = argi++;
        else if (plen == 1 && c[0] && strchr("dxbsc", c[0])) { verb = c[0]; which = argi++; }
        else if (plen == 3 && !strncmp(c, "any", 3)) { verb = 'a'; which = argi++; }
        else {
            int k = 0; which = 0;
            while (k < plen && c[k] >= '0' && c[k] <= '9') { which = which * 10 + (c[k] - '0'); k++; }
            if (k == 0 || k != plen) {
                err_at(fmt->line, fmt->col, "unknown format specifier `{%.*s}`", clen, c);
                i = j;
                continue;
            }
        }
        if (which >= nargs)
            err_at(fmt->line, fmt->col, "not enough arguments for the format string");
        else if (verb != 0)
            check_fmt_verb(fmt, call->items[first + 1 + which], verb);
        if (which > hi) hi = which;
        i = j;
    }
    /* 多给的实参以前是静默吞掉的——格式串少写一个 {} 会把后面全部错位，
     * 这种错位在运行期几乎无法排查（stdfs 测试踩过一次）。多给 = 报错。 */
    if (hi + 1 < nargs)
        err_at(fmt->line, fmt->col,
               "too many arguments for the format string: %d given, %d needed",
               nargs, hi + 1);
}

/* 取地址的实参位置：只有"放在那儿的东西"才有地址。
 *
 * `mk(1,2).sum()` 里的接收者是个临时值，C 里取不到它的地址——这种时候不是
 * 报个 C 编译错误，而是让用户先绑到 `let` 上。 */
static bool addr_ok(Node *e) {
    if (!e) return false;
    switch (e->kind) {
    case NK_IDENT:
        return true;
    case NK_FIELD:
    case NK_INDEX:
        return true;
    case NK_UN:
        return e->op == TOK_STAR;          /* *p 是左值；&x 的结果不是 */
    default:
        return false;
    }
}

/* ============ 分配器去虚拟化（decisions §138） ============
 *
 * Allocator 是 { ctx, alloc, free } 三个字段的普通结构体，调用点是
 * `a.alloc(a.ctx, n)`——C 里就是 `a.alloc(...)` 的**间接调用**。间接调用
 * 让 `inline` 关键字无能为力（能内联的是直调），-O0 下它还多两次访存
 * （取字段 + 跳转）。这里在类型检查阶段把**能证明目标**的调用改写成直调：
 *
 *     a.alloc(a.ctx, n)   ->   zx_xxx_alloc(a.ctx, n)
 *
 * 证明链只走词法可见的那几条（够覆盖 std.heap.page、ArenaAllocator /
 * DebugAllocator 这类工厂、以及用户自己 `Allocator{ .alloc = f }`）：
 *   - std.heap.page
 *   - Allocator{ .ctx = …, .alloc = F, .free = G }
 *   - `if (x) v { … }` 的捕获（初值 = 条件表达式）
 *   - 工厂调用 `F.init(…)`，且 F 的函数体里恰好一条 return 结构体字面量
 * 否定证据（任何一条成立就放弃，保持间接调用）：
 *   - 变量被重新赋值过（nassign > 0）
 *   - 变量被 `&` 取过地址（escaped）
 *   - 函数体里不止一条 return 分配器字面量（多义）
 *
 * 只改 n->a（被调），不动实参——alloc/free 的签名与字段类型逐字相同，
 * 第一个实参本来就是 ctx。改完的节点是 NK_IDENT，重复检查天然幂等。
 */

static bool ty_is_allocator(Type *t) {
    return t && t->kind == TY_STRUCT && t->name && !strcmp(t->name, "Allocator");
}

/* std.heap.page 的 alloc/free 在运行时里是两个 static C 函数
 * （emit_debug_runtime 同一条生成路径，need_by_cname 认名字置 g_need_page）。
 * 给它们造一对只有 cname/ty 的符号，后端就能当普通函数直调。 */
static Symbol *page_alloc_fn(bool want_alloc) {
    static Symbol sa, sf;
    static bool init;
    if (!init) {
        init = true;
        Type *p8 = ty_opt(ty_ptr(ty_u8(), false, false, 0));
        Type *ra = ty_opt(ty_ptr(ty_u8(), false, false, 0));
        Type *ap[2] = { p8, ty_usize() };
        sa.name = "zx_page_alloc"; sa.cname = "zx_page_alloc";
        sa.kind = SK_CONST; sa.state = 2;
        sa.ty = ty_fn(ap, 2, ra);

        Type *fp[3] = { p8, ty_ptr(ty_u8(), false, false, 0), ty_usize() };
        sf.name = "zx_page_free"; sf.cname = "zx_page_free";
        sf.kind = SK_CONST; sf.state = 2;
        sf.ty = ty_fn(fp, 3, ty_void());
    }
    return want_alloc ? &sa : &sf;
}

/* §150：去虚拟化把 `std.heap.page.alloc/free(...)` 的被调改写成只带
 * cname/ty 的合成标识符，它不挂在任何用户作用域。ct_call 折叠调用时会
 * clone_tree 克隆被调函数体再查一遍，而 clone_tree 明确清掉 sym——按名字
 * lookup 必然落空，报 E0401 unknown identifier 'zx_page_alloc'。只在小函数
 * 里写 page.alloc 再被 main 调用时触发（直接写在 main 里没事）。
 * 编译器自造的保留名在这里认领，等价于把它们登记进根作用域，又不污染
 * 用户可见的命名空间。 */
static Symbol *synthetic_fn_sym(const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "zx_page_alloc")) return page_alloc_fn(true);
    if (!strcmp(name, "zx_page_free"))  return page_alloc_fn(false);
    return NULL;
}

/* §187：容器账/收养账共用助手见上；分配器目标解析归 devirt 一节。 */

/* 结构体字面量里 .alloc / .free 的值节点 */
static Node *allocator_pair_value(Node *lit, const char *field) {
    if (!lit || lit->kind != NK_STRUCT_LIT) return NULL;
    for (int i = 0; i < lit->nitems; i++) {
        Node *p = lit->items[i];
        if (p && p->kind == NK_PAIR && p->name && !strcmp(p->name, field))
            return p->a;
    }
    return NULL;
}

/* 工厂体可能还没查过（惰性模块）：类型戳缺失时按字面量上的类型名认。
 * `Allocator{…}` 与 `std.Allocator{…}` 的 a 节点都叫 "Allocator"。 */
static bool lit_is_allocator(Node *lit) {
    if (!lit || lit->kind != NK_STRUCT_LIT) return false;
    /* 已经有类型戳就以类型为准：用户自定义的同名 Allocator 不能靠名字蒙混 */
    if (lit->ty) return ty_is_allocator(lit->ty);
    Node *te = lit->a;
    if (te && te->type_val && ty_is_allocator(te->type_val)) return true;
    return te && te->name && !strcmp(te->name, "Allocator");
}

/* 收集函数体里的 `return Allocator{…}`（不进军嵌套函数）。多于一条就不用。
 * `?Allocator` 的返回会被 coerce 裹一层 NK_CAST/NK_UNWRAP，逐层剥掉。 */
static void collect_allocator_returns(Node *n, int depth, Node **found, int *count) {
    if (!n || depth > 16 || *count > 1) return;
    if (n->kind == NK_FN) return;
    if (n->kind == NK_RETURN && n->a) {
        Node *cand = n->a;
        for (int g = 0; g < 4 && cand; g++) {
            if (cand->kind != NK_CAST && cand->kind != NK_UNWRAP &&
                cand->kind != NK_TRY && cand->kind != NK_ORELSE) break;
            cand = cand->b ? cand->b : cand->a;
        }
        if (lit_is_allocator(cand)) {
            *found = cand;
            *count = *count + 1;
            return;
        }
    }
    collect_allocator_returns(n->a, depth + 1, found, count);
    collect_allocator_returns(n->b, depth + 1, found, count);
    collect_allocator_returns(n->c, depth + 1, found, count);
    collect_allocator_returns(n->d, depth + 1, found, count);
    for (int i = 0; i < n->nitems; i++)
        collect_allocator_returns(n->items[i], depth + 1, found, count);
}

/* 从"分配器值"往前推 alloc/free 的函数符号。推不出返回 NULL。 */
static Symbol *allocator_target(Node *e, bool want_alloc, int depth) {
    if (!e || depth > 6) return NULL;
    switch (e->kind) {
    case NK_IDENT: {
        Symbol *s = e->sym;
        /* `std.heap.page` 走别名/直接引用两条形状 */
        if (s && s == std_builtin("heap.page")) return page_alloc_fn(want_alloc);
        if (!s) return NULL;
        if (s->nassign != 0 || s->escaped) return NULL;
        /* if/while 捕获：初值是条件的源表达式 */
        if (s->cap_src) return allocator_target(s->cap_src, want_alloc, depth + 1);
        Node *d = s->decl;
        if (!d || d->kind != NK_DECL || !d->b) return NULL;
        return allocator_target(d->b, want_alloc, depth + 1);
    }
    case NK_FIELD:
        if (e->sym && e->sym == std_builtin("heap.page")) return page_alloc_fn(want_alloc);
        return NULL;
    case NK_STRUCT_LIT: {
        Node *v = allocator_pair_value(e, want_alloc ? "alloc" : "free");
        if (!v) return NULL;
        if ((v->kind == NK_IDENT || v->kind == NK_FIELD) && v->sym) return v->sym;
        return NULL;
    }
    case NK_CAST:      /* coerce 插的空转换 */
        return allocator_target(e->b, want_alloc, depth + 1);
    case NK_UNWRAP:    /* `x.?` */
    case NK_TRY:
        return allocator_target(e->a, want_alloc, depth + 1);
    case NK_CALL: {
        /* 工厂：`DebugAllocator.init(page)` / `ArenaAllocator.init(page)` /
         * `Pool.allocator()` / `SyncPool.allocator()`。容器方法是 NK_FIELD
         * callee，sym 常为空，只有 inst 指向方法自己的 NK_FN。 */
        Node *callee = e->a;
        Node *fn = NULL;
        if (callee && callee->inst && callee->inst->kind == NK_FN)
            fn = callee->inst;
        else if (callee && callee->sym && callee->sym->decl) {
            Node *d = callee->sym->decl;
            if (d->kind == NK_FN) fn = d;
            else if (d->kind == NK_DECL && d->b && d->b->kind == NK_FN) fn = d->b;
        }
        if (!fn || !fn->b) return NULL;
        Node *lit = NULL;
        int cnt = 0;
        collect_allocator_returns(fn->b, 0, &lit, &cnt);
        if (cnt != 1 || !lit) return NULL;
        Node *v = allocator_pair_value(lit, want_alloc ? "alloc" : "free");
        if (!v) return NULL;
        if ((v->kind == NK_IDENT || v->kind == NK_FIELD) && v->sym) return v->sym;
        return NULL;
    }
    default:
        return NULL;
    }
}

static void devirt_retry_add(Node *n, Node *field) {
    if (g_ndr == g_cdr) {
        g_cdr = g_cdr ? g_cdr * 2 : 64;
        g_devirt_retry = realloc(g_devirt_retry,
                                 sizeof(DevirtPending) * (size_t)g_cdr);
    }
    g_devirt_retry[g_ndr++] = (DevirtPending){ .call = n, .field = field };
}

/* 只做证明，不改树。field 必须是调用点**原始**的 NK_FIELD：末轮复核时
 * n->a 可能已经被换成 NK_IDENT，得靠它拿回接收者。 */
static Symbol *devirt_prove(Node *n, Node *field) {
    if (!field || field->kind != NK_FIELD || !field->name) return NULL;
    bool want_alloc = !strcmp(field->name, "alloc");
    bool want_free  = !strcmp(field->name, "free");
    if (!want_alloc && !want_free) return NULL;
    if (n->meth_done) return NULL;                    /* 方法调用：不是字段分发 */
    if (!field->ty || field->ty->kind != TY_FN) return NULL;
    Node *recv = field->a;
    if (!recv || !ty_is_allocator(recv->ty)) return NULL;
    /* 实参个数这时已经对齐（check_call 前面报过错），签名与字段类型逐字相同 */
    if (n->nitems != (want_alloc ? 2 : 3)) return NULL;
    return allocator_target(recv, want_alloc, 0);
}

static void devirt_rewrite(Node *n, Symbol *tgt) {
    Node *id = node_new(NK_IDENT, n->a->line, n->a->col);
    id->name = tgt->name;
    id->sym  = tgt;
    id->ty   = tgt->ty;
    n->a = id;
    n->ct = NULL;      /* 别留旧被调（函数指针字段）的编译期折叠结果 */
}

/* `a.alloc(a.ctx, n)` / `a.free(a.ctx, p, n)`：能证明就直调；无论成不成
 * 都登记候选点，给末轮复核留一份原始节点。 */
static void devirt_allocator_call(Node *n) {
    if (!n || !n->a || n->a->kind != NK_FIELD) return;
    Node *field = n->a;
    if (!g_devirt_final) devirt_retry_add(n, field);
    /* §187（用户裁定）：allocator.alloc() 是建槽的唯一入口——所有
     * 分配都经过它（heapAlloc/heapSlice 是它的类型化糖，同一入口）。
     * 豁免两条：① 调用点所在模块声明了 tag.close 或是 std 实现层
     * （tag_module_exempt）——实现层内部的分配由各容器的 init/deinit
     * 配对管；② 分配目标函数所在模块声明了 tag.close——arena 这类
     * 单独申请豁免的整批管理分配器。 */
    if (field->name && !strcmp(field->name, "alloc") && !n->xform_free &&
        n->nitems == 2 && field->a && field->a->ty &&
        ty_is_allocator(field->a->ty) &&
        !(g_mod && (g_mod->tags_closed || tag_module_exempt(g_mod))) &&
        !allocator_exempt(field->a))
        tag_auto_decl(n, false);
    Symbol *tgt = devirt_prove(n, field);
    if (tgt && tgt->ty && tgt->ty->kind == TY_FN) devirt_rewrite(n, tgt);
}

/* 所有排队的函数体都查过之后，用**完整信息**复核每个候选点：
 *   - 惰性子模块的工厂此刻才有 sym/ty，第一次证明不了的现在能直调；
 *   - 别的功能体里后出现的 `a.alloc = f` / `&a.alloc` 此刻已打上
 *     nassign/escaped，第一次误直调的在这里回退成间接分发（§149）。
 * 已经改成 NK_IDENT 且仍能证明的点不动。 */
static void devirt_retry_all(void) {
    g_devirt_final = true;
    for (int i = 0; i < g_ndr; i++) {
        DevirtPending *e = &g_devirt_retry[i];
        Node *n = e->call;
        Symbol *tgt = devirt_prove(n, e->field);
        if (tgt && tgt->ty && tgt->ty->kind == TY_FN) {
            if (n->a->kind != NK_IDENT) devirt_rewrite(n, tgt);
        } else if (n->a->kind == NK_IDENT) {
            n->a = e->field;      /* 后来被抹源：回退，不回退就是假安全 */
            n->ct = NULL;
        }
    }
    g_devirt_final = false;
    free(g_devirt_retry);
    g_devirt_retry = NULL;
    g_ndr = g_cdr = 0;
}

/* 方法调用。两种写法：
 *
 *   p.sum()          →  sum(p)          （接收者是 f 的第一个参数）
 *   Point.sum(&p)    →  sum(&p)         （接收者由调用者显式写出来）
 *
 * 接收者参数是 `*T` 时，`p.sum()` 自动写 `&p`——所以接收者必须能取地址。
 * 语义上方法就是普通函数，这里只做两件事：补第一个实参、把调用点挂到方法上，
 * 后端于是用同一套方式生成。 */
static void tag_call_flow(Node *n);          /* §173：定义在 check_call 之后 */

static Type *check_method_call(Node *n) {
    Node *fld = n->a;
    Type *mft = fld->ty;
    if (!mft || mft->kind != TY_FN)
        return type_of(n, ty_invalid());

    Type *ret = mft->ret ? mft->ret : ty_void();
    Type *recv_ty = fld->a->ty;

    /* 已经补过接收者了（克隆体复检：ct_call 会 clone 检查过的函数体再 check
     * 一遍）。参数表和类型都已就位，别再把接收者塞一次——那会让 arity 差一。
     * 贴标对账幂等（释放事件/事实账都按位点去重），重跑安全。 */
    if (n->meth_done) { tag_call_flow(n); return type_of(n, ret); }

    if (recv_ty && recv_ty->kind == TY_TYPE) {         /* Point.sum(&p) */
        if (n->nitems != mft->nparams)
            return BAD(n, "method takes %d argument(s) (receiver included), %d given",
                       mft->nparams, n->nitems);
        /* §151：源码写 `Type.f(a, ...)`，但 `Self` 接收者可能已按大小升级成
         * `const Self *`。显式调用不必知道这个判定——实参是值时自动补 `&`。*/
        Type *w0 = mft->nparams > 0 ? mft->params[0] : NULL;
        for (int i = 0; i < n->nitems; i++) {
            Type *got = check_expr(n->items[i], mft->params[i]);
            if (i == 0 && w0 && w0->kind == TY_PTR && !w0->has_len &&
                !ty_is_array_ptr(w0) && w0->is_const && got && got->kind != TY_PTR &&
                ty_eq(got, w0->elem) && addr_ok(n->items[i])) {
                Node *addr = node_new(NK_UN, n->items[i]->line, n->items[i]->col);
                addr->op = (int)TOK_AMP;
                addr->a  = n->items[i];
                addr->ty = ty_ptr(got, true, false, 0);
                n->items[i] = addr;
                got = addr->ty;
            }
            coerce_node(n->items[i], got, mft->params[i], "argument");
        }
        n->inst = fld->inst;
        n->ival = 0;
        tag_call_flow(n);                /* §173 补丁七：显式接收者同链 */
        return type_of(n, ret);
    }

    Node *recv = fld->a;
    Type *rt   = fld->a->ty;
    if (!fld->flag || mft->nparams == 0)
        return BAD(recv, "`%s` is not a method (its first parameter is not `self`); "
                         "call it as `%s.%s(...)`",
                   fld->name ? fld->name : "?", rt ? ty_str(rt) : "Type",
                   fld->name ? fld->name : "?");
    Type *want = mft->params[0];
    Node *arg0 = recv;

    if (want->kind == TY_PTR && !(rt && rt->kind == TY_PTR && ty_eq(rt, want))) {
        if (rt && rt->kind == TY_PTR) {
            /* 已经是指针了：剩下的只是 const 的加减，交给普通转换 */
        } else if (rt->kind == TY_STRUCT || rt->kind == TY_UNION || rt->kind == TY_TUPLE) {
            if (!addr_ok(recv)) {
                /* §210：rvalue 接收者没法取地址，必须绑到有存储的局部变量。
                 * 建议的绑定种类要看方法要不要可变接收者——对 `*Self` 方法
                 * 若还建议 `let`，下一行 lvalue_mutable 会再挡一次，形成
                 * "先说绑 let、再说要 var" 的自相矛盾诊断。 */
                return BAD(recv, "cannot take the address of this receiver; "
                                 "bind it to a `%s` first",
                           want->is_const ? "let" : "var");
            }
            if (!want->is_const && !lvalue_mutable(recv))
                return BAD(recv, "cannot call `%s` on a read-only receiver; "
                                 "declare it `var` first", fld->name ? fld->name : "?");
            Node *addr = node_new(NK_UN, recv->line, recv->col);
            addr->op = (int)TOK_AMP;
            addr->a  = recv;
            addr->ty = ty_ptr(rt, want->is_const, false, 0);
            arg0 = addr;
        } else {
            return BAD(recv, "method receiver is `%s`, but this method takes `%s`",
                       ty_str(rt), ty_str(want));
        }
    } else if (want->kind != TY_PTR && rt && rt->kind == TY_PTR &&
               !rt->has_len && !ty_is_array_ptr(rt) && ty_eq(rt->elem, want)) {
        /* §151：按值形态的 `Self` 方法遇到指针接收者时自动解引用。
         * 大类型走 const 指针、小类型按值，两种 ABI 下 `p.f()` 写法一致，
         * 调用点不必知道编译期的大小判定。 */
        Node *deref = node_new(NK_FIELD, recv->line, recv->col);
        deref->a    = recv;
        deref->ival = 0;      /* `.0` 就是指针解引用，后端发 `(*p)` */
        deref->ty   = want;
        arg0 = deref;
    }
    if (coerce_node(arg0, arg0->ty, want, "receiver")->kind == TY_INVALID)
        return type_of(n, ty_invalid());

    int nargs = n->nitems;
    if (nargs + 1 != mft->nparams)
        return BAD(n, "method takes %d argument(s), %d given", mft->nparams - 1, nargs);

    Node **items = calloc((size_t)(nargs + 1), sizeof(Node *));
    items[0] = arg0;
    for (int i = 0; i < nargs; i++) items[i + 1] = n->items[i];
    free(n->items);
    n->items  = items;
    n->nitems = nargs + 1;
    n->meth_done = true;

    for (int i = 1; i < n->nitems; i++)
        coerce_node(n->items[i], check_expr(n->items[i], mft->params[i]),
                    mft->params[i], "argument");

    n->inst = fld->inst;
    n->ival = 0;
    /* §173 补丁七：方法调用走同一条贴标链——接收者就是实参 0，
     * 体内 free(self.field) 经 pff 事实账、返回 self 经返回流记账。 */
    tag_call_flow(n);
    return type_of(n, ret);
}

static const char *g_cur_fn_name;   /* 当前函数名（位置实参注入用） */

/* std.debug.panic / assert：把 文件 / 行 / 列 / 函数名 四个位置实参追加到实参
 * 尾部（decisions §85）。1.2.0 删掉了 @file/@line/@col/@fnName 四个内建，
 * 这里改为直接造字面量节点——效果一样（编译期常量、零运行时开销），
 * 但源码里已经没有任何位置内建可写了。 */
static void debug_inject_loc(Node *n) {
    int old = n->nitems;
    Node **items = calloc((size_t)(old + 4), sizeof(Node *));
    for (int i = 0; i < old; i++) items[i] = n->items[i];

    const char *path = (g_mod && g_mod->path) ? g_mod->path
                    : (g_srcpath ? g_srcpath : "");
    Node *nf = node_new(NK_STR, n->line, n->col);
    nf->str = (char *)path; nf->str_len = (int)strlen(path);
    check_expr(nf, NULL);

    Node *nl = node_new(NK_INT, n->line, n->col);
    nl->ival = (uint64_t)n->line;
    check_expr(nl, NULL);

    Node *nc = node_new(NK_INT, n->line, n->col);
    nc->ival = (uint64_t)n->col;
    check_expr(nc, NULL);

    const char *fn = g_cur_fn_name ? g_cur_fn_name : "?";
    Node *nn = node_new(NK_STR, n->line, n->col);
    nn->str = (char *)fn; nn->str_len = (int)strlen(fn);
    check_expr(nn, NULL);

    items[old + 0] = nf;
    items[old + 1] = nl;
    items[old + 2] = nc;
    items[old + 3] = nn;
    free(n->items);
    n->items = items;
    n->nitems = old + 4;
}

static Type *check_call(Node *n, Type *expect) {
    (void)expect;
    /* 内建调用分派：限定名 `std.println` 与模块内裸名 `println` 走同一条路。
     * 裸名是 g_std_seeded 种进每个模块作用域的；以前只有 NK_FIELD 分支认，
     * 裸名调用在 sema 里被判成 TY_INVALID 静默放过，
     * 后端再吐出未声明的 C 标识符（§157：std/net.zx 里的
     * `stackAlloc(u8,512)` 就是这么炸的，报 B1001）。 */
    Symbol *m = NULL;
    if (n->a->kind == NK_FIELD && n->a->a && n->a->a->kind == NK_IDENT) {
        Type *bt = check_expr(n->a->a, NULL);
        if (bt && bt->kind == TY_TYPE && n->a->a->sym && n->a->a->sym->kind == SK_NS) {
            m = ns_member(n->a->a->sym, n->a->name, n->a->line, n->a->col);
            if (!m) return type_of(n, ty_invalid());
        }
    } else if (n->a->kind == NK_IDENT) {
        /* 只认编译器种进去的内建符号：模块自己声明同名函数时
         * lookup 拿到的是 SK_FN（builtin == BI_NONE），不会被改道。 */
        Symbol *s = lookup(n->a->name);
        /* §192 函数别名：`const alloc = std.heapAlloc;` —— const 的
         * 别名路径（sym_link）会把内建符号的 kind/builtin 抄过来，
         * 之后与裸名内建同一条分派。顶层声明的初值在调用点可能还没
         * 解析（fn 体先于后面的顶层 decl 检查）：初值是 FIELD 且还没
         * 解析过的，先 ensure 走一遍 sym_link。函数内的前向引用不进
         * 这里（fn_local 由 E0401 管，不能被抢跑解析）。 */
        if (s && s->kind == SK_CONST && s->state == 0 && !s->fn_local &&
            s->decl && s->decl->kind == NK_DECL && s->decl->b &&
            s->decl->b->kind == NK_FIELD)
            ensure(s);
        if (s && s->kind == SK_NS &&
            (s->builtin == BI_PRINT || s->builtin == BI_PRINTLN ||
             s->builtin == BI_FORMAT || s->builtin == BI_STACKALLOC ||
             s->builtin == BI_HEAPALLOC || s->builtin == BI_HEAPSLICE ||
             s->builtin == BI_FREE)) {
            n->a->sym = s;
            s->used = true;
            m = s;
        }
    }
    if (m) {
        if (m->builtin == BI_PRINT || m->builtin == BI_PRINTLN || m->builtin == BI_FORMAT) {
            bool isfmt = (m->builtin == BI_FORMAT);
            int first = 0;
            if (isfmt) {            /* std.format(alloc, fmt, args…) */
                if (n->nitems < 2)
                    return BAD(n, "std.format needs an allocator and a format string");
                Type *at = check_expr(n->items[0], sema_allocator_type());
                coerce_node(n->items[0], at, sema_allocator_type(), "allocator");
                first = 1;
            } else if (n->nitems < 1) {
                return BAD(n, "std.%s needs a format string", m->name);
            }
            Type *ft = check_expr(n->items[first], ty_slice(ty_u8(), true));
            coerce_node(n->items[first], ft, ty_slice(ty_u8(), true), "format string");
            for (int i = first + 1; i < n->nitems; i++) check_expr(n->items[i], NULL);
            check_fmt_args(n, first, n->nitems - first - 1);
            if (isfmt) return type_of(n, ty_err(ty_slice(ty_u8(), false)));
            return type_of(n, ty_void());
        }
        /* 分配 API：stackAlloc/heapAlloc 返回 *[N]T（长度在类型里），
         * heapSlice 返回 []T（长度在值里，n 可运行期）。§130 的编译期
         * 合并依赖 N 是常量算静态布局，所以只有 heapAlloc 进 region。 */
        if (m->builtin == BI_STACKALLOC || m->builtin == BI_HEAPALLOC ||
            m->builtin == BI_HEAPSLICE) {
            bool stack = (m->builtin == BI_STACKALLOC);
            bool slice = (m->builtin == BI_HEAPSLICE);
            if (n->nitems != (stack ? 2 : 3))
                return BAD(n, "std.%s takes %s, a type and a count", m->name,
                           stack ? "a type" : "an allocator");
            int base = 0;
            if (!stack) {
                Type *at = check_expr(n->items[0], sema_allocator_type());
                coerce_node(n->items[0], at, sema_allocator_type(), "allocator");
                base = 1;
            }
            Type *elem = resolve_type(n->items[base]);
            if (!elem || elem->kind == TY_INVALID || elem->kind == TY_TYPE ||
                elem->kind == TY_VOID || elem->kind == TY_TAG)
                return BAD(n, "std.%s cannot allocate `%s`", m->name,
                           elem ? ty_str(elem) : "?");
            check_expr(n->items[base + 1], ty_usize());
            if (slice) {
                /* heapSlice：运行期长度，建 no_merge 槽只为贴标/释放追踪。
                 * §187（唯一入口）：allocator_exempt 豁免同 heapAlloc。 */
                if (!n->reg_slot) {
                    n->reg_slot = region_new(n, 1, 1);
                    if (n->reg_slot <= g_nrslots)
                        g_rslots[n->reg_slot - 1].no_merge = true;
                }
                if (!allocator_exempt(n->items[0]))
                    tag_auto_decl(n, false);        /* §173：自动建槽 */
                return type_of(n, ty_slice(elem, false));
            }
            IntV c = ev_int(n->items[base + 1]);
            if (!c.ok)
                return BAD(n->items[base + 1],
                           "std.%s count must be known at compile time", m->name);
            n->esc = stack;          /* 栈那份指向当前帧：逃逸检查盯它 */
            if (!stack) {
                if (!n->reg_slot) {
                    int sz = ty_size(elem), al = ty_align(elem);
                    n->reg_slot = region_new(n,
                        (uint64_t)(sz > 0 ? sz : 1) * c.v,
                        (uint64_t)(al > 0 ? al : 1));
                }
                /* §187（唯一入口）：heapAlloc/heapSlice 是 allocator.alloc
                 * 的类型化糖，内部同一入口——建槽；分配器实现模块声明
                 * 了 tag.close（单独申请的豁免，如 arena）→ 不建槽。 */
                if (!allocator_exempt(n->items[0]))
                    tag_auto_decl(n, false);
            } else {
                tag_auto_decl(n, true);         /* §173：栈分配也建槽，
                                                 * 作用域退出自动标 free */
            }
            /* §201：返回指向 [N]T 数组的指针（elem 就是 [N]T） */
            return type_of(n, ty_ptr(ty_array(elem, c.v), false, true, c.v));
        }
        /* §177：std.free(a, s) —— 释放的对称面。改写成 a.deinit(s)
         * 再走 §173 的同一条变换（切片/定长指针两种形状、字节量计算、
         * cast *u8、tag/region 释放事件全部复用）。分配器显式当第一
         * 实参传，与 heapAlloc/heapSlice 对称——arena/池的内存归还给
         * 原分配器，绝不悄悄落到 malloc 上。 */
        if (m->builtin == BI_FREE) {
            if (n->nitems != 2)
                return BAD(n, "std.free takes an allocator and the "
                           "slice/sized pointer to release");
            Node *recv = n->items[0], *arg = n->items[1];
            Node *nf = node_new(NK_FIELD, n->a->line, n->a->col);
            nf->a = recv; nf->name = "deinit";
            n->a = nf;
            Node **it = calloc(1, sizeof(Node *));
            if (!it) return type_of(n, ty_invalid());
            it[0] = arg;
            free(n->items);
            n->items = it;
            n->nitems = 1;
            tag_transform_deinit(n);
            return check_call(n, expect);
        }
    }

    /* 泛型函数：先按实参单态化出一份具体签名，再检查剩下的运行时实参 */
    if (callee_is_generic(n->a)) {
        Type *ft = instantiate_fn_type(n);
        if (!ft || ft->kind == TY_INVALID) return type_of(n, ty_invalid());
        Type *ret = ft->ret ? ft->ret : ty_void();

        if (ret->kind == TY_TYPE) {              /* 类型构造：值就是那个类型 */
            Type *tv = instance_result(n->inst);
            if (tv) name_type_from_call(n, tv);
            n->type_val = tv ? tv : ty_invalid();
            return type_of(n, ty_type());
        }

        int ncomp = (int)n->ival;
        if (n->nitems != ft->nparams) {
            err_at(n->line, n->col, "function takes %d argument(s), %d given",
                   ft->nparams, n->nitems);
            return type_of(n, ret);
        }
        for (int i = ncomp; i < n->nitems; i++) {
            Type *want = ft->params[i];
            Type *got = check_expr(n->items[i], want);
            coerce_node(n->items[i], got, want, "argument");
        }
        return type_of(n, ret);
    }

    Type *ct = check_expr(n->a, NULL);
    /* std.debug.panic / assert：变参走不了普通函数类型，按 builtin 识别。
     * 用户实参检查完，把位置四件套注入尾部；panic 永不返回（同 @trap），
     * assert 平时是 void，release 档运行时函数体整体擦除（-DZX_RELEASE）。 */
    if (n->a->sym && n->a->sym->builtin == BI_DEBUG_FN) {
        const char *dc = sema_sym_cname(n->a->sym);
        if (dc && !strcmp(dc, "zx_debug_panic")) {
            if (n->nitems != 1)
                return BAD(n, "std.debug.panic takes a message");
            Type *mt = check_expr(n->items[0], ty_slice(ty_u8(), true));
            coerce_node(n->items[0], mt, ty_slice(ty_u8(), true), "message");
            debug_inject_loc(n);
            return type_of(n, ty_noreturn());
        }
        if (dc && !strcmp(dc, "zx_debug_assert")) {
            if (n->nitems != 2)
                return BAD(n, "std.debug.assert takes (cond, message)");
            Type *bt = check_expr(n->items[0], ty_bool());
            coerce_node(n->items[0], bt, ty_bool(), "condition");
            Type *mt = check_expr(n->items[1], ty_slice(ty_u8(), true));
            coerce_node(n->items[1], mt, ty_slice(ty_u8(), true), "message");
            debug_inject_loc(n);
            return type_of(n, ty_void());
        }
    }
    /* std.io.print / std.io.println：往句柄（usize）上格式化。变参走不了
     * 普通函数类型，但 check_field 已把符号挂在 callee 上（std.io.print
     * 和 `const io = std.io` 别名同款）——按 builtin 识别。 */
    if (n->a->sym && n->a->sym->builtin == BI_IO_PRINT) {
        if (n->nitems < 2)
            return BAD(n, "std.io.%s needs a handle and a format string",
                       n->a->name ? n->a->name : "print");
        Type *ht = check_expr(n->items[0], ty_usize());
        coerce_node(n->items[0], ht, ty_usize(), "handle");
        Type *ft = check_expr(n->items[1], ty_slice(ty_u8(), true));
        coerce_node(n->items[1], ft, ty_slice(ty_u8(), true), "format string");
        for (int i = 2; i < n->nitems; i++) check_expr(n->items[i], NULL);
        check_fmt_args(n, 1, n->nitems - 2);
        return type_of(n, ty_void());
    }
    /* `.f` 落在方法上（check_field 记在了节点上）：补接收者、走方法调用 */
    if (n->a->kind == NK_FIELD && n->a->inst)
        return check_method_call(n);
    if (!ct || ct->kind == TY_INVALID) {
        for (int i = 0; i < n->nitems; i++) check_expr(n->items[i], NULL);
        return type_of(n, ty_invalid());
    }
    if (ct->kind != TY_FN) {
        /* 命名空间（std.debug / std.io …）被当函数调用：只说 "type" 用户
         * 看不出该怎么写（2026-11-09 截图：std.debug("create failed.")）。
         * 指向"调它的成员"这条路。类型名后跟括号（`Person(…)`）也是
         * TY_TYPE，同样给这句，措辞对两者都成立。 */
        if (ct->kind == TY_TYPE) {
            const char *nm = (n->a && n->a->kind == NK_FIELD && n->a->name) ? n->a->name : "";
            if (nm[0])
                return BAD(n, "`%s` is a namespace or type, not a function; "
                               "call one of its members (e.g. `%s.<fn>(...)`)", nm, nm);
            return BAD(n, "this value is a namespace or type, not a function; "
                           "call one of its members");
        }
        return BAD(n, "called value has type `%s`, which is not a function", ty_str(ct));
    }

    if (n->nitems != ct->nparams) {
        err_at(n->line, n->col, "function takes %d argument(s), %d given",
               ct->nparams, n->nitems);
        for (int i = 0; i < n->nitems; i++) check_expr(n->items[i], NULL);
        return type_of(n, ct->ret ? ct->ret : ty_void());
    }

    for (int i = 0; i < n->nitems; i++) {
        Type *want = ct->params[i];
        Type *got = check_expr(n->items[i], want);
        coerce_node(n->items[i], got, want, "argument");
    }
    /* §173：贴标载体过调用点（实参释放判定 / 返回流 / 字段账） */
    tag_call_flow(n);
    /* 函数指针字段的分发：能证明目标就改成直调（§138） */
    devirt_allocator_call(n);
    return type_of(n, ct->ret ? ct->ret : ty_void());
}

/* §173：调用点的贴标实参 / 返回流 / 字段账对账。普通调用与方法调用
 * 共用（补丁六之前方法调用在 check_method_call 提前 return，接收者
 * 完全绕过这条链）。前置：n->items 已是最终实参表（方法调用含接收者，
 * items[0] 对齐被调方的第 0 形参——抽象槽 / pff 按 fn->items 对位）。 */
/* §184：体未查时的调用点延迟对账。deps 拓扑只补查到「裸名 + 模块成员
 * 链」能静态解析到的函数；方法调用（`w.show(...)`，接收者是局部值）与
 * 别的漏网形态，调用点对账时被调体可能还没查——形参抽象槽（ps->tag_id）
 * 还没建、pderef/pff 事实账还是空的，E0512 / 字段移交 / 精确释放判定
 * 全部拿不到，静默漏报。这里把（调用点, 实参, 快照）记下来，等全部体
 * 查完后重放一次对账。只重放「事实到来只会更准」的分支：E0512、pff
 * 字段移交、精确释放事件；返回流继承需要整本账回卷，重放不保真，
 * 维持「未分析不继承」的已知限制（与互递归同口径）。 */
typedef struct {
    Node *call, *arg, *fn;
    Symbol *ps;                 /* 可为 NULL：体未查时形参符号还没绑定 */
    int idx;                    /* 形参位——重放时按 fn->items[idx] 补解析 */
    int tid, tst, rel;          /* 调用点当时的实参 tag 快照 / 释放判定 */
    const char *srcpath;        /* 诊断要落在调用方文件上 */
} TagDefer;
static TagDefer *g_tdefer;
static int g_ntdefer, g_ctdefer;

static void tag_defer_call(Node *call, Node *arg, Node *fn, Symbol *ps,
                           int idx, int tid, int tst, int rel) {
    if (g_ntdefer == g_ctdefer) {
        int nc = g_ctdefer ? g_ctdefer * 2 : 32;
        TagDefer *p2 = realloc(g_tdefer, sizeof(TagDefer) * (size_t)nc);
        if (!p2) return;
        g_tdefer = p2; g_ctdefer = nc;
    }
    TagDefer *d = &g_tdefer[g_ntdefer++];
    d->call = call; d->arg = arg; d->fn = fn; d->ps = ps; d->idx = idx;
    d->tid = tid; d->tst = tst; d->rel = rel;
    d->srcpath = g_srcpath;
}

static void tag_call_flow(Node *n) {
    /* §176：同一调用节点只对账一次。ct 折堆与 deps 补查
     * 会克隆已检查过的体重查，克隆节点带着首次的对账结果——
     * 重跑会重复克隆工厂实例槽、把克隆体上下文的函数名
     * 记成假调用边（注释中的"贴标对账幂等重跑安全"
     * 只对释放事件/事实账成立，克隆不幂等）。 */
    if (n->tag_done) return;
    n->tag_done = true;
    {
        Node *fn = NULL;
        if (n->a->sym && n->a->sym->decl) {
            Node *d = n->a->sym->decl;
            /* `const name = fn (…) {...};` 的符号挂在 DECL 上，fn 节点是它的初值 */
            if (d->kind == NK_FN) fn = d;
            else if (d->kind == NK_DECL && d->b && d->b->kind == NK_FN) fn = d->b;
        }
        else if (n->a->inst && n->a->inst->kind == NK_FN)
            fn = n->a->inst;
        /* §175：调用计数——零调用工厂的流转槽终扫豁免（分配运行期
         * 不发生）。递归自调用不计：外层没人进不来。 */
        if (fn && fn->kind == NK_FN && g_cur_fn_name &&
            !(fn->name && strcmp(fn->name, g_cur_fn_name) == 0)) {
            fn->tag_calls++;
            /* §176：调用边入账——泄漏终扫沿边回溯到 main */
            if (fn->name)
                tag_call_edge(fn->name, g_cur_fn_name, n->line, n->col);
        }
        bool releaser = fn_is_releaser(fn);
        /* Allocator.free(ctx, ptr, bytes) 自身就是释放通路：实参放行，
         * 释放事件由 tag_note_free 在 NK_CALL 出口统一记账。 */
        bool free_call = n->a->kind == NK_FIELD && n->a->name &&
                         !strcmp(n->a->name, "free") && n->a->a &&
                         is_allocator_ty(n->a->a->ty);
        for (int i = 0; i < n->nitems; i++) {
            int tid, tst;
            tag_of_expr(n->items[i], &tid, &tst);
            /* §173：形参字段被 free 的事实账——free 的指针实参剥到
             * 「形参符号 . 字段」记一条（必须在 devirt 改写 .free 之前，
             * tag_note_free 看到的已是改写形状）。 */
            if (free_call) {
                Node *pa = n->items[i];
                for (int g = 0; g < 6 && pa &&
                     (pa->kind == NK_CAST || pa->kind == NK_UNWRAP); g++)
                    pa = pa->kind == NK_CAST ? (pa->b ? pa->b : pa->a) : pa->a;
                /* §173 补丁八：字段链折成点路径记事实账（体内
                 * free(…, p.inner.name, …) 在调用点对得上账）。 */
                char key[128];
                Node *pfb = NULL;
                if (pa && pa->kind == NK_FIELD &&
                    ft_path_key(pa, key, sizeof key, &pfb) &&
                    pfb->kind == NK_IDENT && pfb->sym &&
                    pfb->sym->kind == SK_PARAM) {
                    char *pk = ft_path_dup(NULL, key);
                    if (pk) pff_add(pfb->sym, pk);
                }
            }
            if (!tid) continue;
            /* §173：精确释放判定 —— 被调方的第 i 个形参抽象槽真的被
             * free 掉（state FREED），调用点才记释放事件；否则是借出
             * （所有权仍在调用方槽上）。体没查过时退回释放器近似；
             * writeFloat 这类「体内 free 的是别的指针」的函数不再误伤
             * 实参。 */
            bool rel = free_call;
            if (!rel && fn) {
                bool is_recv = n->meth_done && i == 0;
                Symbol *ps = (i < fn->nitems && fn->items[i]->sym)
                             ? fn->items[i]->sym : NULL;
                if (ps && ps->tag_id) {
                    TagSlot *ats = tag_slot(ps->tag_id);
                    rel = ats && ats->abstract && ats->ever_freed;
                } else if (!is_recv) {
                    /* 体未查：释放器近似。接收者绝不近似——释放器方法
                     * 体内 free 的是 self.field（字段另有 pff 对账），
                     * 把接收者整个标 FREED 会假报 use-after-free。
                     * §189：近似只对指针/切片实参生效——Allocator 容器
                     * 这类聚合实参（无形参抽象槽）传给释放器不等于容器
                     * 本身被释放（useDbg(al) 体内 free 的是自己 alloc 的
                     * 块，不是容器账）。 */
                    Type *at = n->items[i]->ty;
                    bool ptrish = at && (at->kind == TY_PTR ||
                                         at->kind == TY_SLICE);
                    rel = ptrish && fn_is_releaser(fn);
                }
                /* §184：形参还没有抽象槽、甚至还没有符号（跨模块方法
                 * 的体未查，形参绑定发生在体检查里）= 事实账之后才会
                 * 就位，记一条延迟对账（幂等：体查过、符号和槽都在的
                 * 调用点不进来）。 */
                if (fn->kind == NK_FN && i < fn->nitems &&
                    (!ps || !ps->tag_id))
                    tag_defer_call(n, n->items[i], fn, ps, i, tid, tst, rel);
            }
            if (rel) tag_free_event(n, n->items[i]);
            /* §180：实参已 FREED、且被调体解引用了对应形参 → 调用点报
             * use-after-free。deps 拓扑下被调体先查、抽象槽共用，函数体
             * 内看到的形参永远 LIVE，FREED 只在调用点可见——与 pff
             * （体内 free → 调用点标 FREED）方向相反、机制对称。释放器/
             * free 调用自身是合法释放通路（走 E0509 double-free），跳过。
             * 报错落在实参（调用行），像 §176 调用链一样让人看到哪次调用
             * 把已释放的内存递了进去。 */
            if (!rel && !free_call && tst == TC_FREED && fn && i < fn->nitems) {
                Symbol *ps = fn->items[i]->sym;
                if (ps && pderef_has(ps)) {
                    TagSlot *ft = tag_slot(tid);
                    diag_code_at("E0512", n->items[i]->line, n->items[i]->col,
                        "use after free: this memory was released at line %d -- "
                        "it is passed to '%s' which reads it; drop this call or "
                        "free the memory later",
                        ft && ft->free_line ? ft->free_line : 0,
                        fn->name ? fn->name : "this function");
                }
            }
        }
        /* §173：返回流 —— 被调方把形参/体内分配原样吐回来时，
         * 调用结果继承 tag（调用方不 free 就是真泄漏）。 */
        if (fn && !releaser && !free_call) {
            int k = -1, slot = 0;
            if (fn_ret_cache(fn, &k, &slot) == 1) {
            if (k >= 0 && k < n->nitems) {
                int tid, tst;
                tag_of_expr(n->items[k], &tid, &tst);
                if (tid && tst == TC_LIVE) {
                    n->tag_id = tid;
                    n->tag_state = TC_LIVE;
                }
            } else if (slot > 0) {
                TagSlot *t = tag_slot(slot);
                /* 终局状态随返回值带过来：LIVE → 调用方负责 free；
                 * FREED（体内已 free，或 stackAlloc 槽随作用域消亡）→ 调用方
                 * 拿到悬垂载体，再 free 报 E0509、解引用报 E0512。
                 * §175：工厂槽按调用点实例化——每次调用都是一份新分配，
                 * 各自记账（两次调用各自 free 不再假报 E0509）。 */
                if (t && t->state != TST_UNBOUND) {
                    bool fr = t->state == TST_FREED;
                    n->tag_id = tag_clone_site(t, n, fr);
                    n->tag_state = fr ? TC_FREED : TC_LIVE;
                } else if (fn->ret_tag_state) {
                    /* §174：臂内分配的身份由臂值节点携带、槽状态已被
                     * tag_restore 回滚（UNBOUND）——继承返回节点烙的
                     * 终局状态，调用点照常对账。 */
                    n->tag_id = tag_clone_site(t, n, fn->ret_tag_state == TC_FREED);
                    n->tag_state = fn->ret_tag_state;
                }
            }
            }
            /* fn_ret_cache == -1（body 未查的前向引用）：返回流未知，
             * 不当场扫、不缓存——直接绑定的载体已在 DECL/ASSIGN
             * 登记 pending，被调 body 查完时由 fn_stamp_ret_flow 回填。 */
        }
        /* §173：结构体实参/返回值携带的字段账本：
         * ① 被调方体内 free 过的字段（pff 事实账）→ 调用点把槽标
         *    FREED——所有权已移交，调用方不再欠释放（防假 E0513），
         *    之后再 free 报 E0509、解引用报 E0512；
         * ② 结构体返回值带字段账 → 调用节点继承，DECL 再绑到载体
         *    符号（deps 拓扑补查保证被调 body 先查，fn->ftags 已就位）。 */
        if (fn) {
            for (int i = 0; i < n->nitems && i < fn->nitems; i++) {
                Symbol *ps = fn->items[i]->sym;
                if (!ps) continue;
                /* §194：形参 store 事实镜像——被调体往形参字段写入的
                 * 新分配实例化烙到实参载体，必须在 pff free 对账之前：
                 * 先有账，之后调用方读回/释放才有着落。 */
                pfs_mirror_arg(ps, n->items[i], n, fn);
                FieldTag *fs = ftags_of_expr(n->items[i]);
                for (; fs; fs = fs->next)
                    if (fs->slot > 0 && pff_has(ps, fs->name))
                        ft_mark_freed(fs->slot, n);
                /* §185：数组跨函数对账——体内 free(p.N)（常索引元素
                 * 释放，pff 事实账 "N"）→ 实参数组的对应元素槽标
                 * FREED；体内 free(p) 整容器终结（pffa）→ 实参数组
                 * 的群整批清掉（所有权随调用移交，闭环）。 */
                {
                    Node *aa = n->items[i];
                    for (int g = 0; g < 6 && aa &&
                         (aa->kind == NK_CAST || aa->kind == NK_UNWRAP ||
                          aa->kind == NK_SLICE ||
                          (aa->kind == NK_FIELD && aa->name &&
                           !strcmp(aa->name, "ptr"))); g++)
                        aa = aa->kind == NK_CAST ? (aa->b ? aa->b : aa->a)
                            : aa->kind == NK_FIELD ? aa->a : aa->a;
                    Symbol *abs2 = agt_base_of(aa);
                    if (abs2) {
                        for (int k = 0; k < 32; k++) {
                            char nk[8];
                            snprintf(nk, sizeof nk, "%d", k);
                            if (!pff_has(ps, nk)) continue;
                            int es = aesc_find(abs2, k);
                            if (es) ft_mark_freed(es, n);
                        }
                        if (pffa_has(ps)) agt_clear(abs2, n);
                    }
                }
            }
            /* §177：继承的字段账在调用点实例化——工厂（返回的结构体/
             * 元组里带着体内分配）每调一次都是一份新分配，泄漏责任
             * 落在调用行（与直接指针返回流的 tag_clone_site 同口径）。
             * 只实例化"出生在被调体内"的槽：def_fn 是被调（体内
             * heapAlloc 母槽）或 inst_of 非零（体内调用点实例，嵌套
             * 工厂链）。透传（形参字段账）与全局槽不动——它们不是
             * 每次调用新发生的分配。tag_decl 可能 realloc 槽表：先收
             * 集旧槽 id，逐个回取再克隆。 */
            if (!releaser && !free_call && fn->ftags && !n->ftags) {
                n->ftags = ft_clone((FieldTag *)fn->ftags);
                int olds[64], nolds = 0;
                for (FieldTag *g = (FieldTag *)n->ftags; g && nolds < 64; g = g->next) {
                    if (g->slot <= 0) continue;
                    bool seen = false;
                    for (int q = 0; q < nolds; q++) if (olds[q] == g->slot) { seen = true; break; }
                    if (!seen) olds[nolds++] = g->slot;
                }
                for (int q = 0; q < nolds; q++) {
                    TagSlot *ts = tag_slot(olds[q]);
                    if (!ts || ts->abstract) continue;
                    if (ts->state != TST_LIVE && ts->state != TST_FREED) continue;
                    bool born_here =
                        (ts->def_fn && fn->name && !strcmp(ts->def_fn, fn->name)) ||
                        ts->inst_of != 0;
                    if (!born_here) continue;
                    int nid = tag_clone_site(ts, n, ts->state == TST_FREED);
                    if (!nid) continue;
                    for (FieldTag *g = (FieldTag *)n->ftags; g; g = g->next)
                        if (g->slot == olds[q]) g->slot = nid;
                }
            } else if (!releaser && !free_call && fn->ftags && n->ftags) {
                /* 调用节点已带账（克隆体复检等）：槽已被实例化过，
                 * 幂等不动。 */
            }
        }
    }
}

/* §184：全部体查完后的延迟对账重放（见 tag_defer_call）。 */
static void tag_deferred_replay(void) {
    for (int i = 0; i < g_ntdefer; i++) {
        TagDefer *d = &g_tdefer[i];
        Symbol *ps = d->ps ? d->ps
                   : (d->idx < d->fn->nitems ? d->fn->items[d->idx]->sym : NULL);
        if (!ps) continue;              /* 形参最终也没绑定：无从对账 */
        TagSlot *ats = ps->tag_id ? tag_slot(ps->tag_id) : NULL;
        bool rel = ats && ats->abstract && ats->ever_freed;
        const char *sp = g_srcpath;
        g_srcpath = d->srcpath;         /* 诊断落在调用方文件 */
        /* 首跑按释放器近似没标释放、体查完证实真是释放通路：补记。
         * 所有权移交的事实到账，终扫不再把这块记成调用方的泄漏。 */
        if (!d->rel && rel)
            tag_free_event(d->call, d->arg);
        if (!rel && d->tst == TC_FREED && pderef_has(ps)) {
            TagSlot *ft = tag_slot(d->tid);
            diag_code_at("E0512", d->arg->line, d->arg->col,
                "use after free: this memory was released at line %d -- "
                "it is passed to '%s' which reads it; drop this call or "
                "free the memory later",
                ft && ft->free_line ? ft->free_line : 0,
                d->fn->name ? d->fn->name : "this function");
        }
        /* §173 补丁三：字段移交对账——被调体内 free 过的字段事实账
         * 此刻已就位，重读实参的字段链补标 FREED（之后的再 free 报
         * E0509、解引用报 E0512 才有着落）。 */
        FieldTag *fs = ftags_of_expr(d->arg);
        for (; fs; fs = fs->next)
            if (fs->slot > 0 && pff_has(ps, fs->name))
                ft_mark_freed(fs->slot, d->call);
        /* §185：数组群对账（跨模块重放）——体内整容器终结（pffa）/
         * 常索引元素释放（pff "N"）的事实账此刻已就位。 */
        {
            Node *aa = d->arg;
            for (int g = 0; g < 6 && aa &&
                 (aa->kind == NK_CAST || aa->kind == NK_UNWRAP ||
                  aa->kind == NK_SLICE ||
                  (aa->kind == NK_FIELD && aa->name &&
                   !strcmp(aa->name, "ptr"))); g++)
                aa = aa->kind == NK_CAST ? (aa->b ? aa->b : aa->a)
                    : aa->kind == NK_FIELD ? aa->a : aa->a;
            Symbol *abs2 = agt_base_of(aa);
            if (abs2) {
                for (int k = 0; k < 32; k++) {
                    char nk[8];
                    snprintf(nk, sizeof nk, "%d", k);
                    if (!pff_has(ps, nk)) continue;
                    int es = aesc_find(abs2, k);
                    if (es) ft_mark_freed(es, d->call);
                }
                if (pffa_has(ps)) agt_clear(abs2, d->call);
            }
        }
        g_srcpath = sp;
    }
}

static const char *g_cur_fn_name;   /* 当前函数名（位置实参注入用） */

/* §191：改写路径的表达式克隆要保留标识符叶子上的 sym。clone_tree 一律
 * 清 sym（泛型单态化需要干净的树），但 deinit/free 变换克隆的是**调用点
 * 现场**的表达式——后端按 sym->cname 发名（局部名带行_列后缀），丢了就
 * 发裸名，与生成的 C 改名对不上：B1001 use of undeclared 'pool'
 * （std.free(pool.allocator(), pp) 的 .ctx 克隆体丢了 pool 的符号）。
 * 源节点的 sym 都在当前作用域里活着，直接沿用安全。 */
static void clone_keep_syms(Node *o, Node *c) {
    if (!o || !c) return;
    if ((o->kind == NK_IDENT || o->kind == NK_TYPE_IDENT) && o->sym)
        c->sym = o->sym;
    clone_keep_syms(o->a, c->a);
    clone_keep_syms(o->b, c->b);
    clone_keep_syms(o->c, c->c);
    clone_keep_syms(o->d, c->d);
    for (int i = 0; i < o->nitems && i < c->nitems; i++)
        clone_keep_syms(o->items[i], c->items[i]);
}

static Node *clone_expr_keep_syms(Node *n) {
    Node *c = clone_tree(n);
    clone_keep_syms(n, c);
    return c;
}

/* §173：`a.deinit(s)` → `a.free(a.ctx, (*u8)s.ptr, s.len * size)`。
 * 接收者求值两次（分配器值通常是无副作用读）；其余与手写 free 完全
 * 同构，tag/region 两本账本照常记账。s 可以是切片（[]T）或定长指针
 * （*[N]T，heapAlloc 的返回形状）。 */
static void tag_transform_deinit(Node *call) {
    Node *f = call->a;
    if (!f || f->kind != NK_FIELD || !f->name || strcmp(f->name, "deinit") ||
        !f->a || call->nitems != 1) return;
    Type *at = check_expr(f->a, NULL);
    if (!at || !is_allocator_ty(at)) return;
    Node *arg = call->items[0];
    Type *st = check_expr(arg, NULL);
    if (!st) return;
    Node *ptrE = NULL, *lenE = NULL;
    if (st->kind == TY_SLICE) {
        int esz = ty_size(st->elem);
        if (esz <= 0) esz = 1;
        Node *pf = node_new(NK_FIELD, arg->line, arg->col);
        pf->a = clone_expr_keep_syms(arg); pf->name = "ptr";
        Node *lf = node_new(NK_FIELD, arg->line, arg->col);
        lf->a = clone_expr_keep_syms(arg); lf->name = "len";
        Node *sz = node_new(NK_INT, arg->line, arg->col);
        sz->ival = (uint64_t)esz;
        Node *mul = node_new(NK_BIN, arg->line, arg->col);
        mul->op = TOK_STAR; mul->a = lf; mul->b = sz;
        ptrE = pf; lenE = mul;
    } else if (ty_is_array_ptr(st)) {
        /* §201：elem 就是 [N]T，ty_size(elem) 已是全部字节数，别再乘 N。 */
        int total = ty_size(st->elem);
        if (total <= 0) total = 1;
        ptrE = clone_expr_keep_syms(arg);
        Node *sz = node_new(NK_INT, arg->line, arg->col);
        sz->ival = (uint64_t)total;
        lenE = sz;
    } else {
        /* §173：不可释放形状给一句人话，而不是让 `.deinit` 落进
         * E0304（Allocator 没有这个字段）。仍按 free 形状变换，
         * 让后续检查一条路走完（字节量记 0，报错程序不会真的跑）。 */
        err_at(arg->line, arg->col,
               "`deinit` needs a slice `[]T` or a sized pointer `*[N]T` -- "
               "`%s` carries no length; free it as "
               "`free(ctx, (*u8)p, bytes)` instead", ty_str(st));
        ptrE = clone_expr_keep_syms(arg);
        Node *sz0 = node_new(NK_INT, arg->line, arg->col);
        sz0->ival = 0;
        lenE = sz0;
    }
    /* cast 到 *u8：free 的形参类型（ptr→ptr 身份不变，tag 照常携带） */
    Node *u8t = node_new(NK_TYPE_IDENT, arg->line, arg->col);
    u8t->name = "u8";
    Node *pt = node_new(NK_PTR_TYPE, arg->line, arg->col);
    pt->a = u8t;
    Node *cast = node_new(NK_CAST, arg->line, arg->col);
    cast->a = pt; cast->b = ptrE;
    /* 释放器形状：a.free(a.ctx, ptr, bytes) */
    Node *cf = node_new(NK_FIELD, f->line, f->col);
    cf->a = clone_expr_keep_syms(f->a); cf->name = "ctx";
    f->name = "free";
    Node **items = calloc(3, sizeof(Node *));
    items[0] = cf; items[1] = cast; items[2] = lenE;
    call->items = items;
    call->nitems = 3;
    call->xform_free = 1;      /* §187：变换产物，唯一入口拦截放行 */
}

/* 结构体 / 联合体字面量的字段检查。`t` 已经是目标类型——具名字面量
 * `Vec{…}` 从名字拿到它，匿名字面量 `.{…}` 从期望类型拿到它。 */
/* 聚合体里有没有重名字段（声明处已报 E0402）。有的话这个类型是坏的：
 * 字面量永远"少填一个"，再叠一条 missing field 只会把真正的错埋掉。 */
static bool ty_has_dup_fields(Type *t) {
    for (int i = 0; i < t->nfields; i++) {
        if (!t->fields[i].name) continue;
        for (int j = i + 1; j < t->nfields; j++)
            if (t->fields[j].name && !strcmp(t->fields[i].name, t->fields[j].name))
                return true;
    }
    return false;
}

static Type *check_lit_fields(Node *n, Type *t) {
    bool broken = ty_has_dup_fields(t);
    if (t->kind == TY_UNION || t->kind == TY_STRUCT) {
        bool *seen = calloc((size_t)(t->nfields ? t->nfields : 1), sizeof(bool));
        for (int i = 0; i < n->nitems; i++) {
            Node *p = n->items[i];
            int idx = ty_field_index(t, p->name);
            if (idx < 0) {
                {
                    const char *sug = suggest_member(p->name, t);
                    if (sug) diag_help("did you mean `%s`?", sug);
                }
                BAD(p, "`%s` has no field '%s'", ty_str(t), p->name);
                check_expr(p->a, NULL);
                continue;
            }
            seen[idx] = true;
            /* 字面量也是在"构造别人的类型"：私有字段照样拦（同一条可见性
             * 规则，别让 `.field = …` 成为绕过私有检查的后门）。 */
            if (deny_member(p, t, t->fields[idx].is_pub, p->name)) {
                check_expr(p->a, NULL);
                continue;
            }
            /* 同一个字段写两遍：后者静默盖掉前者（`S{ .a = 1, .a = 2 }` 给 2），
             * 十有八九是复制粘贴错的——这种"能编过、结果是另一个值"的错误
             * 必须报出来，别等运行时才发现。 */
            for (int j = 0; j < i; j++)
                if (ty_field_index(t, n->items[j]->name) == idx) {
                    diag_code_at("E0307", p->line, p->col,
                                 "field '%s' is initialized twice in `%s`",
                                 p->name, ty_str(t));
                    break;
                }
            Type *vt = check_expr(p->a, t->fields[idx].ty);
            coerce_node(p->a, vt, t->fields[idx].ty, "field initializer");
            /* §173：字段值携带的 tag 烙进字面量的字段账本。
             * 补丁八：内层是结构体字面量时把它的字段账拍平进外层
             * （.{.inner = .{.name = p}} → 外层账 "inner.name"→槽）。 */
            {
                int ftid, ftst;
                tag_of_expr(p->a, &ftid, &ftst);
                if (ftid && (ftst == TC_LIVE || ftst == TC_FREED))
                    ft_set((FieldTag **)&n->ftags, p->name, ftid);
                else {
                    FieldTag *inft = ftags_of_expr(p->a);
                    for (FieldTag *g = inft; g; g = g->next) {
                        if (g->slot <= 0) continue;
                        char *key = ft_path_dup(p->name, g->name);
                        if (key) ft_set((FieldTag **)&n->ftags, key, g->slot);
                    }
                }
            }
        }
        int set = 0;
        for (int i = 0; i < t->nfields; i++) {
            if (seen[i]) { set++; continue; }
            if (t->kind == TY_UNION || t->fields[i].is_static) continue;
            if (!broken && !t->fields[i].dflt)
                err_at(n->line, n->col, "missing field '%s' in `%s`",
                       t->fields[i].name ? t->fields[i].name : "?", ty_str(t));
        }
        if (t->kind == TY_UNION && set != 1)
            err_at(n->line, n->col, "a union literal must initialize exactly one member, got %d", set);
        free(seen);
        return type_of(n, t);
    }
    return NULL;
}

static Type *check_struct_lit(Node *n, Type *expect) {
    Type *t = resolve_type(n->a);
    if (t->kind == TY_INVALID) return type_of(n, t);

    if (t->kind == TY_STRUCT || t->kind == TY_UNION) return check_lit_fields(n, t);
    if (t->kind == TY_TUPLE) {
        for (int i = 0; i < n->nitems && i < t->nfields; i++) {
            Type *vt = check_expr(n->items[i]->a, t->fields[i].ty);
            coerce_node(n->items[i]->a, vt, t->fields[i].ty, "tuple element");
        }
        tuple_stamp_ftags(n);
        return type_of(n, t);
    }
    (void)expect;
    return BAD(n, "`%s` is not a struct, union or tuple", ty_str(t));
}

/* §174：元组字面量的元素账——第 i 个元素带 tag 烙成 "i"→槽；内层是
 * 聚合字面量时把它的字段账拍平（.{p, 0} 内层 .{.name = ptr} →
 * "0.name"→槽）。 tagged-union 变体载荷是元组时，check_lit_fields 的
 * 拍平分支靠这里先把载荷元组的账立起来。 */
static void tuple_stamp_ftags(Node *n) {
    for (int i = 0; i < n->nitems; i++) {
        Node *it = n->items[i];
        int ftid, ftst;
        tag_of_expr(it, &ftid, &ftst);
        char nb[16];
        snprintf(nb, sizeof nb, "%d", i);
        char *pk = ft_path_dup(NULL, nb);       /* ft_set 只存指针：必须是持久副本 */
        if (!pk) continue;
        if (ftid && (ftst == TC_LIVE || ftst == TC_FREED)) {
            ft_set((FieldTag **)&n->ftags, pk, ftid);
        } else {
            FieldTag *inft = ftags_of_expr(it);
            for (FieldTag *g = inft; g; g = g->next) {
                if (g->slot <= 0) continue;
                char *key = ft_path_dup(nb, g->name);
                if (key) ft_set((FieldTag **)&n->ftags, key, g->slot);
            }
        }
    }
}

static Type *check_tuple_lit(Node *n, Type *expect) {
    if (expect && expect->kind == TY_TUPLE && expect->nfields == n->nitems) {
        for (int i = 0; i < n->nitems; i++) {
            Type *it = check_expr(n->items[i], expect->fields[i].ty);
            coerce_node(n->items[i], it, expect->fields[i].ty, "tuple element");
        }
        tuple_stamp_ftags(n);
        return type_of(n, expect);
    }
    Type **items = calloc((size_t)(n->nitems ? n->nitems : 1), sizeof(Type *));
    for (int i = 0; i < n->nitems; i++) items[i] = check_expr(n->items[i], NULL);
    Type *t = ty_tuple(items, n->nitems);
    free(items);
    n->ty = t;
    tuple_stamp_ftags(n);
    if (expect && !ty_eq(expect, t) && expect->kind != TY_VOID)
        coerce_node(n, t, expect, "value");
    return t;
}

static Value *arg_ct(Node *a, Type *want);      /* 在后面，泛型实参用 */

static char *path_dup_dir(const char *path);
static char *path_join(const char *dir, const char *rel);

static Type *check_builtin(Node *n, Type *expect) {
    (void)expect;
    const char *nm = n->name ? n->name : "";

    if (!strcmp(nm, "type")) {
        /* 唯一的内建：@type(x) → 编译期描述符
         *   { .name, .size, .align, .ty, .fieldCount, .fields }
         * x 是类型时就是那个类型，是表达式时取它的类型（原 @typeOf）。
         * `@type(T).size/.align/.name` 顶替了 @sizeOf/@alignOf/@typeName
         * 三个老内建（1.2.2 合并，见下面 gone[] 的提示）。 */
        if (n->nitems < 1) return BAD(n, "@type needs a type or an expression");
        Type *t = check_expr(n->items[0], NULL);
        if (t && t->kind == TY_TYPE) {
            /* 类型实参：取类型值本身。`Point`、`i32`、`[]const u8`、
             * 泛型实参 T 都走这。 */
            t = n->items[0]->type_val;
            if (!t && n->items[0]->sym) t = n->items[0]->sym->type_val;
        }
        if (!t || t->kind == TY_INVALID)
            return BAD(n, "@type: the type is not known at compile time");
        n->type_val = t;        /* ct_clear 之后 ct_eval 里可以重建 */
        /* §118：实参是有存储的左值（变量/字段/解引用）时描述符带 ptr 槽 */
        n->ct = type_desc_value(t,
            (t->kind == TY_STRUCT || t->kind == TY_UNION) &&
            !(n->items[0]->ty && n->items[0]->ty->kind == TY_TYPE) &&
            addr_ok(n->items[0]) ? n->items[0] : NULL);
        return type_of(n, sema_type_type());
    }

    if (!strcmp(nm, "error")) {
        /* 只有真的执行到这一行才报错：放在 `if` 里的 @error 是编译期
         * 分支的一部分，不能在检查函数体时就炸。 */
        return type_of(n, ty_noreturn());
    }

    if (!strcmp(nm, "trap")) return type_of(n, ty_noreturn());

    if (!strcmp(nm, "panic")) {
        /* §213：@panic(msg?) —— 运行时带 Zx 位置中止（走 CRT 无缓冲
         * 诊断，不重复 crash report）。msg 必须是字符串字面量：后端要
         * 把它直接嵌进生成 C，跟 @error 的口径一致。 */
        if (n->nitems > 1) return BAD(n, "@panic takes at most one message");
        if (n->nitems == 1 && (!n->items[0] || n->items[0]->kind != NK_STR))
            return BAD(n, "@panic: the message must be a string literal");
        return type_of(n, ty_noreturn());
    }

    if (!strcmp(nm, "tag")) {
        /* §178：@tag(p) —— 取指针携带的标签（一等 tag 值，与 tag.x 同型）。
         * 指针进数组（运行期索引）等断链的地方**之前**先取出来存住，
         * 之后 `tag v : @tag(p) = xs[i];` 把链续上。
         * 没有标签的指针报 E0523——取不到就明说，绝不静默给个空标签。 */
        if (n->nitems < 1) return BAD(n, "@tag needs a pointer: @tag(p)");
        Type *t = check_expr(n->items[0], NULL);
        if (!t || (t->kind != TY_PTR && t->kind != TY_SLICE))
            return BAD(n, "@tag: the argument must be a pointer or slice, got `%s`",
                       ty_str(t));
        int id = 0, st = TC_NONE;
        tag_of_expr(n->items[0], &id, &st);
        if (!id) {
            diag_code_at("E0523", n->line, n->col,
                "this pointer carries no label -- @tag(p) returns the label a "
                "pointer was bound with (`tag.x : expr`, or a re-bound "
                "carrier); this value was never bound to one");
            return ty_invalid();
        }
        TagSlot *ts = tag_slot(id);
        if (ts && ts->abstract) {
            diag_code_at("E0523", n->line, n->col,
                "this pointer's label is a parameter's abstract borrow -- it "
                "has no single identity to capture; re-bind at the call site "
                "where the allocation actually lives");
            return ty_invalid();
        }
        n->tag_id = id;
        n->tag_state = st == TC_NONE ? (int)TC_LIVE : st;
        return type_of(n, ty_tag());
    }

    if (!strcmp(nm, "link")) {
        /* @link("ws2_32") / @link("./vendor/foo.lib")：把外部库交给链接器。
         * 编译期动作，运行时是 no-op；名字去重后由 build_program 转成
         * `-l<name>`（带路径分隔符或扩展名的按文件路径原样传）。 */
        if (n->nitems < 1) return BAD(n, "@link needs a library name: @link(\"ws2_32\")");
        Value *sv = arg_ct(n->items[0], NULL);
        if (!sv || sv->kind != V_STR)
            return BAD(n, "@link: the library name must be a string literal");
        zx_add_link_lib(sv->sval);
        return type_of(n, ty_void());
    }

    if (!strcmp(nm, "shim")) {
        /* §192：@shim 通道已删除。它曾经把一份自包含 C TU 编成独立 .o
         * 一起链接；现在要走工程通道——`zxc include` 把 C 库放进
         * vendor/，函数用 `extern "C" fn name(…) Ret;` 声明，库用
         * @link交给链接器。解析层双前端都还认识这个名字，语义层
         * 统一在这里给人话。 */
        (void)arg_ct(n->items[0], NULL);
        return BAD_CODE("E0211", n, "@shim was removed: compile C through the "
                        "project instead (`zxc include <lib>` binds it into "
                        "vendor/; declare functions with `extern \"C\" fn "
                        "name(...) Ret;`; libraries with @link)");
    }

    if (!strcmp(nm, "memcpy") || !strcmp(nm, "memmove") || !strcmp(nm, "memset")) {
        /* @memcpy(dst, src, bytes) / @memmove(dst, src, bytes) /
         * @memset(dst, byte, bytes)：批量内存操作，库容器（std.array.List 等）
         * 的增长、搬移不再走逐元素循环。dst 是指针或切片（memcpy/memmove
         * 的 src 同样），memset 的第二参是填充字节值。返回 void。 */
        if (n->nitems < 3) return BAD(n, "@%s needs (dest, src, bytes)", nm);
        Type *d = check_expr(n->items[0], NULL);
        Type *c = check_expr(n->items[2], ty_usize());
        bool d_ok = d && (d->kind == TY_PTR || d->kind == TY_SLICE);
        if (!d_ok)
            return BAD(n, "@%s: dest must be a pointer or slice, got `%s`", nm, ty_str(d));
        if (!strcmp(nm, "memset")) {
            Type *f = check_expr(n->items[1], NULL);
            if (!f || !numeric_like(f))
                return BAD(n, "@memset: the fill value must be an integer, got `%s`", ty_str(f));
        } else {
            Type *s = check_expr(n->items[1], NULL);
            bool s_ok = s && (s->kind == TY_PTR || s->kind == TY_SLICE);
            if (!s_ok)
                return BAD(n, "@%s: src must be a pointer or slice, got `%s`", nm, ty_str(s));
        }
        if (!c || !numeric_like(c))
            return BAD(n, "@%s: the byte count must be an integer, got `%s`", nm, ty_str(c));
        return type_of(n, ty_void());
    }

    /* 1.2.0 把这批砍到只剩 9 个内建；1.2.2 反射全部收敛成一个 @type，
     * @sizeOf/@alignOf/@typeName 也并了进去。老代码里残留的名字给一条
     * 能照着改的提示，而不是含糊的 "unknown builtin"。 */
    {
        static const struct { const char *name, *hint; } gone[] = {
            { "as",       "type conversion is written `(T)value`" },
            { "intCast",  "type conversion is written `(T)value`" },
            { "ptrCast",  "type conversion is written `(T)value`" },
            { "bitCast",  "type conversion is written `(T)value`" },
            { "sizeOf",   "use @type(T).size" },
            { "alignOf",  "use @type(T).align" },
            { "typeName", "use @type(T).name" },
            { "typeInfo", "use @type(x) -- same descriptor, shorter name" },
            { "typeOf",   "use @type(x) -- its `.ty` is the type" },
            { "hasField", "use @type(T) and look through `.fields`" },
            { "fieldCount","use @type(T).fieldCount" },
            { "fieldType","use @type(T).fields[i].ty" },
            { "fieldName","use @type(T).fields[i].name" },
            { "offsetOf", "use @type(T).fields[i].offset" },
            { "field",    "reflection has no dynamic member access; use `v.name`" },
            { "embedFile","embed the bytes yourself (a literal or std.fs)" },
            { "file",     "source-location builtins are gone" },
            { "line",     "source-location builtins are gone" },
            { "col",      "source-location builtins are gone" },
            { "fnName",   "source-location builtins are gone" },
            /* §160 落地当天就改名：@cShim 太长，通道本身没有变化。 */
            { "cShim",    "renamed to @shim -- same channel, shorter name" },
            /* 1.4.1：向量的三个内建换成 std.simd 里的普通函数，内建只剩 @Vec 类型。 */
            { "select",   "moved to std.simd.select -- `@Vec` is the only vector builtin" },
            { "any",      "moved to std.simd.any" },
            { "all",      "moved to std.simd.all" },
            /* 1.4.3：@compileError 改名为 @error（内建仍是九个，通道不变）。 */
            { "compileError", "renamed to @error -- same channel, shorter name" },
        };
        for (size_t i = 0; i < sizeof gone / sizeof gone[0]; i++)
            if (!strcmp(nm, gone[i].name))
                return BAD(n, "@%s was removed: %s", nm, gone[i].hint);
    }

    return BAD(n, "unknown builtin @%s", nm);
}

static Type *check_expr_raw(Node *n, Type *expect) {
    if (!n) return ty_void();

    switch (n->kind) {
    case NK_INT:
        if (expect && (ty_is_int(expect) || ty_is_float(expect))) {
            if (ty_is_int(expect) && !lit_fits_expr(n, expect)) {
                if (n->flag) {
                    char digits[64];
                    lit_digits(n, digits, sizeof digits);
                    return BAD(n, "integer literal %s does not fit in `%s`", digits, ty_str(expect));
                }
                return BAD(n, "integer literal %llu does not fit in `%s`",
                           (unsigned long long)n->ival, ty_str(expect));
            }
            return type_of(n, expect);
        }
        if (expect && expect->kind == TY_OPT && ty_is_int(expect->elem))
            return type_of(n, expect->elem);
        return type_of(n, ty_comptime_int());

    case NK_FLOAT:
        if (expect && ty_is_float(expect)) return type_of(n, expect);
        return type_of(n, ty_comptime_float());

    case NK_CHAR:
        /* char 字面量比整数字面量多一层含义：它既是"字节"（u8），也是
         * "这个字符的码位"。落点是整数类型时按落点定型——`switch (code)`
         * 的 subject 是 i32，`'a'` 就该是 i32；没有落点才是 u8。
         * 装不进去要报错，不能静默截断（R10，和整数字面量同一条规则）。 */
        if (expect && ty_is_int(expect)) {
            if (!lit_fits_expr(n, expect))
                return BAD(n, "char literal does not fit in `%s`", ty_str(expect));
            return type_of(n, expect);
        }
        return type_of(n, ty_u8());

    case NK_STR:
        /* 字符串字面量是**定长数组指针** `*[N]const u8`（N 是字节数，
         * 不含结尾 0），不是切片——长度挂在类型里，`.len` 是编译期常量，
         * 也挡得住把字面量当可写缓冲区。传给 `[]const u8` 形参时按
         * "*[N]T → []T" 的既有规则自动降级（can_coerce 已支持）。
         * 用户 2026-11-09 定：字符串等于 zx 的数组 *[N]const u8。 */
        return type_of(n, ty_ptr(ty_u8(), true, true, (uint64_t)n->str_len));

    case NK_BOOL:
        return type_of(n, ty_bool());

    case NK_NULL:
        /* 有落点就定型：`?T` 是原来的语义；`*T` / `*fn` 是 C 互操作——
         * C 的 NULL 就是 0 指针，没必要逼人写 `(*T)(0)`。没有落点仍然
         * 无类型（`let a = null;` 照旧报错），否则"null 是值"又活了。 */
        if (expect && (expect->kind == TY_OPT || expect->kind == TY_PTR ||
                       expect->kind == TY_FN || expect->kind == TY_ERROR))
            return type_of(n, expect);
        return type_of(n, ty_null());

    case NK_UNDEF:
        if (expect) return type_of(n, expect);
        return type_of(n, ty_undef());

    case NK_IDENT: {
        Symbol *s = lookup(n->name);
        if (!s) s = synthetic_fn_sym(n->name);   /* §150 合成直调目标 */
        if (!s) {
            /* §144：module 关键字已换成内建 @mod。旧写法会落到这里，
             * 给一条能照着改的提示，而不是含糊的 unknown identifier。 */
            if (!strcmp(n->name, "module"))
                return BAD(n, "`module` was removed: use @mod(\"name\")");
            /* `_` 丢弃占位；基础类型名在值位置表示类型本身（泛型实参） */
            if (!strcmp(n->name, "_")) return type_of(n, ty_void());
            Type *b = base_type(n->name);
            if (b) { n->type_val = b; return type_of(n, ty_type()); }
            const char *sug = suggest_ident(n->name);
            if (sug) diag_help("did you mean `%s`?", sug);
            if (getenv("ZX_DBG")) fprintf(stderr, "[dbg] unknown '%s' in fn=%s depth=%d root=%d\n", n->name, g_cur_fn_name ? g_cur_fn_name : "?", g_fn_depth, (int)g_root_stmts);
            return BAD(n, "unknown identifier '%s'", n->name);
        }
        n->sym = s;
        /* 块内先声明后使用（fn_local）：作用域预登记让前向引用在 sema 悄悄
         * 解析到后面的声明，但后端按语句顺序发 C，到那边必然编不过，最后
         * 被包装成 B1001「编译器 bug」。在这里给出人话。顶层 hoist 是有意
         * 的（互递归），不受影响；懒模块声明（fn 里的 `const std = @mod(..)`）
         * 靠使用点触发解析，也豁免。 */
        if (s->fn_local && s->state == 0 && s->decl &&
            !(s->decl->kind == NK_DECL && decl_is_lazy_module(s->decl))) {
            diag_help("blocks are plain statement sequences: move the "
                      "declaration of '%s' above this use", n->name);
            return BAD_CODE("E0401", n, "'%s' is used before its declaration "
                            "in this block", n->name);
        }
        /* §192：闭包不支持。嵌套函数体里解析到**外层函数的**栈槽
         * （fn_d 更浅）——以前 sema 放行，后端发一个谁也不认识的 C 名，
         * 最后被包装成 B1001「编译器 bug」。在这里给人话。fn 值绑定
         * （`const F = fn …`）不是栈槽：嵌套函数之间的兄弟引用合法，
         * 不算捕获。 */
        if (s->fn_d > 0 && s->fn_d < g_fn_depth &&
            !(s->decl && s->decl->kind == NK_DECL && s->decl->b &&
              s->decl->b->kind == NK_FN)) {
            diag_help("Zx has no closures: pass the value as a parameter of "
                      "the inner function instead");
            return BAD_CODE("E0406", n, "'%s' is a local of the enclosing "
                            "function; nested functions cannot capture "
                            "outer variables", n->name);
        }
        if (getenv("ZX_PROBE") && (!strcmp(n->name, "myidx") || !strcmp(n->name, "idx") || !strcmp(n->name, "dims3"))) fprintf(stderr, "[probe] USE %s sym=%p tracked=%d@%d:%d\n", n->name, (void*)s, (int)(s->locals_next != 0), n->line, n->col);
        s->used = true;      /* 求值路径就是"用到了"：读、写、取地址都算 */
        /* 正在构造的聚合类型：外壳已经挂上了，名字就能用（`Color.red` 写在
         * Color 自己的体里）。成员表填完之前它只能当类型用。 */
        if (s->state == 3 && s->type_val && s->ty && s->ty->kind == TY_TYPE) {
            n->type_val = s->type_val;
            return type_of(n, ty_type());
        }
        ensure(s);
        /* 容器体内裸名引用同容器的静态函数（`b` 的函数体里写 `a()`）：
         * 后端发定义时用的是实例名（容器静态函数走 itab 懒发射，名字是
         * `<cname>_i<n>`），而裸名查到的 cname 只是 `a`。把容器体上的函数
         * 节点挂到引用点，后端两条路径就对齐了（§139）。
         * 泛型函数例外：它的名字由调用点的实例化决定，这里不插手。
         * 方法（第一参数 self）也照挂——裸名调用方法本来就该在参数处报错，
         * 名字先对齐了报错才看得懂。 */
        if (s->in_agg && s->decl && s->decl->kind == NK_FN &&
            !fn_is_generic(s->decl))
            n->inst = s->decl;
        if (s->ty && s->ty->kind == TY_TYPE) {
            n->type_val = s->type_val;
            return type_of(n, ty_type());
        }
        if (!s->ty) return type_of(n, ty_invalid());
        /* 节点类型永远是"值自己的类型"；提升（R7）发生在使用点，
         * 由 coerce_node / 后端统一处理，不在这里改。 */
        return type_of(n, s->ty);
    }

    case NK_BIN:      return check_binary(n, expect);
    case NK_UN:       return check_unary(n, expect);

    /* `e.?` —— 解包。空（?T 的 none）/ 出错（!T 的 err）在运行时 trap，
     * 与整数除零同一个处置方式：先 flush，再打一行说明，然后 abort。
     * 它只给值，不是左值——`x.? = 1` 会被 check_assign 拦掉。 */
    case NK_UNWRAP: {
        Type *t = check_expr(n->a, NULL);
        if (!t || t->kind == TY_INVALID) return ty_invalid();
        if (t->kind == TY_NULL)
            /* `null` 自己没有类型——它只是"给 ?T 填空"的那个字面量，不是 ?T。
             * 所以 `null.?` 不是"运行时空解包"，而是压根没东西可解。 */
            return BAD(n, "`.?` needs an optional or an error union, found `null` -- "
                          "`null` only fills a `?T` or a `*T`, it is not one");
        if (t->kind != TY_OPT && t->kind != TY_ERR)
            return BAD(n, "`.?` needs an optional or an error union, found `%s`", ty_str(t));
        /* §135：句柄失效在 get 处已报 E0510 / E0511，这里不再重复判——
         * 同一条语句报两个码只会把真正的修法埋掉。 */
        /* 编译期就知道是空的：`.?` 在这里必然 abort，和除零同一处置（§7.R10）——
         * 能定就在编译期报，不要留到运行期。
         * §199：只有**确定为空**（V_NULL）才报 E0506。V_UNDEF 是"算不出/未代入"
         * （编译期参数值没绑上、undefined 字面量等），不能证明值为 null——把它
         * 算作 null 会把"有值但暂时没代入"的编译期 optional 参数误判成必然 abort。
         * 放行后交给后端 trap，才是正确行为。 */
        Value *cv = ct_eval_probe(n->a);
        if (cv && cv->kind == V_NULL)
            return BAD_CODE("E0506", n, "`.?` on a value that is `null` at compile time -- "
                                        "it would abort every time");
        /* §170：COND 载体判空后按 LIVE 使用（判空担保非空、非空即已分配） */
        {
            int tid, tst;
            tag_of_expr(n->a, &tid, &tst);
            if (tid && tst == TC_COND) {
                n->tag_id = tid;
                n->tag_state = TC_LIVE;
            }
        }
        return type_of(n, t->elem);
    }

    /* `try e` —— 出错就带着同一个码往上返回，成功取载荷。
     * 它是明式的：调用点写着 `try`，读者就知道这个函数在这里可能退出；
     * 没有栈展开、没有 catch 块、类型也不变（还是那个 `!T`）。 */
    case NK_TRY: {
        Type *t = check_expr(n->a, NULL);
        if (!t || t->kind == TY_INVALID) return ty_invalid();
        /* `defer` / `defer err` 体里不能往外抛：两者都在返回路径上执行，
         * 自己再"提前返回"没有意义（会把正在返回的值覆盖掉）。就地处理掉。 */
        if (g_in_defer)
            return BAD(n, "`try` is not allowed inside `defer`/`defer err` -- a defer body "
                          "cannot propagate; handle the error there (`if` capture / `orelse`)");
        /* `?T` 也行（v0.26）：空值当错误——null 抛保留错误码 error.null（65535）。
         * "可能没有"和"可能出错"不靠嵌套表达（?!T 非法），靠同一条传播路。 */
        if (t->kind != TY_ERR && t->kind != TY_OPT)
            return BAD(n, "`try` needs an error union or an optional, found `%s` -- "
                          "use `orelse` or `if` capture to handle the empty case here",
                       ty_str(t));
        /* 往外抛要有地方抛：所在函数必须能返回错误（`!U`）。 */
        if (!g_ret || g_ret->kind != TY_ERR)
            return BAD(n, "`try` inside a function that cannot return an error -- "
                          "declare it `!%s` or handle the error here (`if` capture / `orelse`)",
                       ty_str(t->elem));
        /* 声明了错误集就得守住：往上抛的码必须都在集合里（§5.2）。 */
        void *want = cur_errset();
        if (want) {
            if (t->kind == TY_OPT) {
                if (!ty_errset_has(want, 65535))
                    return BAD_CODE("E0902", n,
                        "`try` on `?T` throws the reserved code `error.null`, "
                        "which is not in this function's error set %s",
                        ty_errset_str(want));
            } else if (!t->eset) {
                return BAD_CODE("E0903", n,
                    "this `try` can propagate any error (`!%s`), but the function declares %s -- "
                    "widen the return type to `!%s`, or handle the error here",
                    ty_str(t->elem), ty_errset_str(want), ty_str(t->elem));
            } else if (!ty_errset_subset(t->eset, want)) {
                diag_help("add `%s` to the return type: `%s!%s`",
                          errset_missing(t->eset, want),
                          ty_errset_str(ty_errset_union(want, t->eset)),
                          ty_str(t->elem));
                return BAD_CODE("E0902", n,
                    "`try` may propagate `%s`, which is not in the declared error set of this "
                    "function (%s)",
                    errset_missing(t->eset, want), ty_errset_str(want));
            }
        }
        return type_of(n, t->elem);
    }

    /* §171 `try e err { … }` —— 捕获形式：出错不外抛，err 绑错误码，
     * 块产出兜底值 T；成功取载荷。两臂 unify，整体类型 T。
     * tag 臂账本挂在本节点上（helper 不会自动覆盖新分支）。 */
    case NK_TRY_CAPTURE: {
        Type *t = check_expr(n->a, NULL);
        if (!t || t->kind == TY_INVALID) {
            check_block(n->b);             /* 别把块里的错漏掉 */
            return ty_invalid();
        }
        if (t->kind != TY_ERR && t->kind != TY_OPT)
            return BAD(n, "`try ... err` needs an error union or an optional, found `%s`",
                       ty_str(t));
        /* `?T` 的空值走捕获时错误码是保留码 error.null（65535）。 */
        Type *payload = t->elem;

        ArmExit exOK = { 0, TC_NONE, NULL };   /* 成功臂：无 tag 事件 */
        ArmExit exErr;
        TagSnap snapT;
        tag_snap(&snapT);
        Type *bt;
        {
            Scope sc;
            scope_push(&sc);
            if (n->name) {
                Symbol *s = scope_add(&sc, n->name, SK_CAPTURE);
                if (s) { s->ty = ty_error(); s->state = 2;
                         s->reg_slot = region_of_expr(n->a);
                         s->cap_src = n->a; }
            }
            g_arm_depth++;              /* §188：捕获臂同属释放禁区 */
            bt = check_block(n->b);
            g_arm_depth--;
            if (n->b->kind == NK_BLOCK) {
                exErr = tag_arm_reconcile(n->b->d, n->b, snapT.state, snapT.n, snapT.total);
            } else {
                exErr = (ArmExit){ 0, TC_NONE, NULL };
            }
            tag_restore(&snapT);
            scope_pop(&sc);
        }
        /* 成功臂没动 tag；错误臂若臂内 free/alloc 未按约定传出，reconcile 已报。
         * 合并：LIVE⊔FREED(null 载体)→COND，等待使用者判空防守。 */
        tag_join_arms(n, exOK, exErr);

        if (!bt || bt->kind == TY_INVALID) return ty_invalid();
        Type *r = unify(n, payload, bt);
        return type_of(n, r);
    }

    /* `a orelse b` —— 有值取载荷，空/错取 b。b 只在真的空/错时求值。 */
    case NK_ORELSE: {
        Type *t = check_expr(n->a, NULL);
        if (!t || t->kind == TY_INVALID) {
            check_expr(n->b, expect);      /* 右边照样检查，别把错漏过去 */
            return ty_invalid();
        }
        if (t->kind == TY_NULL)
            /* 同 `null.?`：`null` 只在"有 ?T 落点"的位置成立。`null orelse x`
             * 没有落点，也就没有左值可看——直接报错，别退化成"右边恒为答案"。 */
            return BAD(n, "`orelse` needs an optional or an error union, found `null` -- "
                          "`null` only fills a `?T` or a `*T`, it is not one");
        if (t->kind != TY_OPT && t->kind != TY_ERR)
            return BAD(n, "`orelse` needs an optional or an error union, found `%s`", ty_str(t));
        Type *elem = t->elem;
        Type *alt = check_expr(n->b, elem);
        /* §200：`orelse` 的兜底臂不产出值时允许——臂末尾是 `return` /
         * `continue` / noreturn 调用 / 无限循环（永不落到块尾），它根本
         * 不会给表达式贡献值，整条 orelse 的值只能来自左臂载荷 elem。
         * check_block 对这种臂一律回 void，不能拿它去 coerce 载荷（会误报
         * E0301）。用 stmt_ends/falls_through 的同一套"末尾可达吗"判定。 */
        bool alt_exits = (alt && alt->kind == TY_NORETURN) ||
                         (n->b && n->b->kind == NK_BLOCK && !falls_through(n->b));
        n->flag = alt_exits;         /* 后端据此跳过右支的 C 表达式位置 */
        if (!alt_exits)
            coerce_node(n->b, alt, elem, "`orelse` alternative");
        return type_of(n, elem);
    }
    case NK_CAST:     return check_cast(n);
    case NK_FIELD:    return check_field(n);
    case NK_INDEX:    return check_index(n);
    case NK_SLICE:    return check_slice_expr(n);

    case NK_TAG_APPLY: {
        /* 表达式位置：贴标（tag.x : expr）或裸引用。定义/tag.close 是
         * 语句位置的事（check_stmt）。 */
        if (!strcmp(n->name, "close")) {
            return BAD(n, "`tag.close;` is a file-scope statement, not an expression");
        }
        if (!n->b) {
            /* §172：裸引用 tag.buf —— 一等 tag 值（模仿 error.foo）：
             * 引用即自动建槽，类型是编译期-only 的 tag；没有运行期值，
             * 只能 const 绑定（const buf = tag.buf;）或去贴标点用。 */
            TagSlot *ts = tag_resolve(n);
            n->tag_id = ts ? (int)(ts - g_tslots) + 1 : 0;
            return type_of(n, ty_tag());
        }
        Type *vt = check_expr(n->b, expect);
        bool bad = false;
        tag_check_apply(n, &bad);
        if (bad) return type_of(n, vt ? vt : ty_invalid());
        n->ty = vt;
        return vt;
    }

    case NK_TAG_BIND: {
        
        /* §174 续链：tag newVar : tag.myTag = oldVar;（parse 降糖成
         * `let newVar = <本节点>`）。给**丢失了标签**的值重新绑回一个
         * **已存在**的 tag——补上数组/跨类型转换等造成的追踪断链。
         * 守门（用户裁定）：
         *   - tag 必须已存在：不自动建槽——语义是续上断掉的链，只能
         *     是之前的标签，新标签报错（绑定的正确性由使用者担保：
         *     编译期已经追踪不到这个值了）；
         *   - 值必须无标签：已有标签的值再绑 = 重复标签（E0522），
         *     新分配请用 `let p = tag.x : expr;`；
         *   - 槽必须 LIVE：终结了的链不能复活；
         *   - 值是数组元素时必须常索引：运行期索引接不回精确账。
         * 绑定后 newVar 走正常流程（free 记账、泄漏终扫指向绑定行）。 */
        if (n->b && n->b->kind == NK_INDEX)
            tag_diag_once(n->b->line, n->b->col);   /* 预占座：值读出的
                                                     * E0521 豁免（宣言场景） */
        g_in_tag_rebind = true;   /* §187：续链值里的 int→ptr cast 就是
                                   * 贴标本身，免 E0525 */
        Type *vt = check_expr(n->b, expect);
        g_in_tag_rebind = false;
        if (!vt || vt->kind == TY_INVALID) return ty_invalid();
        if (vt->kind != TY_PTR && vt->kind != TY_SLICE) {
            err_at(n->line, n->col,
                   "only a pointer can be tracked -- '%s' is a re-bind for a lost "
                   "tracked pointer, not this value", n->name ? n->name : "?");
            return ty_invalid();
        }
        TagSlot *t = NULL;
        Node *tv = n->a;
        for (int g = 0; g < 6 && tv &&
             (tv->kind == NK_CAST || tv->kind == NK_UNWRAP); g++)
            tv = tv->kind == NK_CAST ? (tv->b ? tv->b : tv->a) : tv->a;
        if (tv && tv->kind == NK_TAG_APPLY && !tv->b) {
            t = tag_by_name(tv->name);          /* 不建槽：必须是旧标签 */
            if (!t) {
                diag_code_at("E0300", tv->line, tv->col,
                    "`tag.%s` does not exist -- a re-bind must continue an "
                    "existing label; new labels only come from tagging an "
                    "allocation (`let p = tag.x : expr;`)", tv->name);
                return ty_invalid();
            }
        } else if (tv && tv->kind == NK_BUILTIN && tv->name &&
                   !strcmp(tv->name, "tag")) {
            /* §178：`tag v : @tag(p) = q;` —— @tag(p) 在标签位置（那里不
             * 走 check_expr）当场解析：实参检查 + 槽提取。 */
            check_expr(tv, NULL);
            t = tv->tag_id ? tag_slot(tv->tag_id) : NULL;
            if (!t) return ty_invalid();
        } else if (tv && tv->kind == NK_IDENT) {
            Symbol *ts = lookup(tv->name);
            if (ts) ts->used = true;        /* 标签位置不走 check_expr，手动记使用 */
            t = (ts && ts->tag_id) ? tag_slot(ts->tag_id) : NULL;
            if (!t) {
                diag_code_at("E0520", tv->line, tv->col,
                    "`%s` is not a label value -- the re-bind needs "
                    "`tag.x` (or a `const` label binding)", tv->name);
                return ty_invalid();
            }
        } else {
            err_at(n->line, n->col,
                   "the re-bind needs a label value between ':' and '=' -- "
                   "write `tag v : tag.x = lost;`");
            return ty_invalid();
        }
        if (t->abstract) {
            err_at(n->line, n->col,
                   "`tag.%s` is a parameter slot -- a re-bind cannot claim "
                   "an abstract borrow", t->name);
            return ty_invalid();
        }
        /* §185：群成员校验在状态检查**之前**——终结后再取出（槽已
         * FREED）要报 E0524（容器已终结），而不是落进笼统的"链已
         * 结束"。 */
        if (n->b->kind == NK_INDEX && n->b->a) {
            int sid = (int)(t - g_tslots) + 1;
            int ix = aesc_const_index(n->b->b);
            if (ix >= 0) {
                /* 常索引续链：宣言建精确账，之后读这个元素走账，E0521
                 * 消失——追踪真正接回数组里。 */
                if (n->b->a->kind == NK_IDENT && n->b->a->sym)
                    aesc_add(n->b->a->sym, ix, sid);
                t->arr_esc = true;
            } else {
                /* §185：运行期索引的注解读取——这是群成员的**取出**
                 * 通路：基址能解析到数组基变量且群非空时，校验
                 * tag.x ∈ 群，取出即销账（二次取出、取出后再取、
                 * 取没存过的标签都报 E0524）。基址解析不了（指针
                 * 别名）或群里没账（§175 旧断线场景）→ 保留旧语义：
                 * 宣言接管 pending，绑定正确性由使用者担保。 */
                Symbol *bs3 = agt_base_of(n->b->a);
                if (bs3 && (agt_any(bs3) || agt_closed(bs3))) {
                    if (agt_has(bs3, sid)) {
                        agt_remove(bs3, sid);
                    } else {
                        diag_code_at("E0524", n->b->line, n->b->col,
                            "`tag.%s` is not registered in this array's "
                            "group -- it was never stored here, was "
                            "already taken out, the container was already "
                            "terminated, or the store went through an "
                            "alias the compiler cannot prove; only labels "
                            "stored into this array can be taken out",
                            t->name);
                        return ty_invalid();
                    }
                }
            }
            /* 运行期索引续链也合法：宣言把该元素的释放责任接到 v1
             * 这个载体上（账跟着 v1 走，不再依赖数组元素）——接管
             * 即清写入时的 pending。数组里其他元素再读仍报 E0521，
             * 由使用者继续处理。 */
            t->rt_pending = false;
        } else {
            /* 非元素来源的续链（cast 往返、?T 解包后……）同样接管。 */
            t->rt_pending = false;
        }
        if (t->state != TST_LIVE) {
            err_at(n->line, n->col,
                   t->state == TST_FREED
                   ? "this chain already ended (freed at line %d) -- a "
                     "re-bind cannot revive a finished chain"
                   : "this label is not tracking any allocation -- nothing "
                     "to continue",
                   t->free_line);
            return ty_invalid();
        }
        {
            int id = 0, st = TC_NONE;
            tag_of_expr(n->b, &id, &st);
            if (id) {
                TagSlot *old = tag_slot(id);
                diag_code_at("E0522", n->b->line, n->b->col,
                    "this value already carries label '%s' -- a re-bind is "
                    "only for lost pointers; free the old allocation "
                    "first, or use `let p = tag.x : expr;` for a fresh "
                    "allocation", old ? old->name : "?");
                return ty_invalid();
            }
        }
        n->tag_id = (int)(t - g_tslots) + 1;
        n->tag_state = TC_LIVE;
        n->ty = vt;
        return vt;
    }

    case NK_CALL: {
        /* §189：std.move —— 编译期账本操作（所有权转移），std 里没有
         * 真实的 move 函数体，不走普通调用检查。 */
        if (call_is_std_move(n)) return type_of(n, check_std_move(n));
        tag_transform_deinit(n);    /* §173：a.deinit(s) → a.free(...) */
        /* §189（用户裁定，替代 §188 臂内释放禁区）：分支臂里允许
         * free/deinit（直接或写在 defer 里），但必须显式确认——在释放
         * 之后写 `tag p : null;`，证明使用者确认过正确释放。这是跟
         * 主路径 free 的差异：分支本就可能不走，合流时没确认的释放
         * 当作还没释放（不影响以后的 use/free），有确认的按 FREED
         * 落账。检查与落账在 tag_arm_reconcile（臂出口）统一做。 */
        Type *r = check_call(n, expect);
        /* 实参检查完才能追 provenance：Allocator.free 是一次 release 事件 */
        region_note_free(n);
        tag_note_free(n);           /* §170：贴标指针走 tag 账本 */
        if (!n->tag_hooks_done) {
            n->tag_hooks_done = 1;      /* §187：钩子节点级幂等（体复检） */
            tag_note_adopt(n);          /* §187：arena 收养（所有权移交） */
            tag_note_container_init(n); /* §187：资源容器 init 开账 */
            tag_note_arena_deinit(n);   /* §187：总清算终结容器账/收养账 */
        }
        region_handle_call(n);
        return r;
    }
    case NK_STRUCT_LIT: return check_struct_lit(n, expect);
    case NK_ARRAY_LIT:  return check_array_lit(n, n->items, n->nitems, expect);
    case NK_TUPLE:    return check_tuple_lit(n, expect);
    case NK_BUILTIN:  return check_builtin(n, expect);

    case NK_ANON_STRUCT: {
        /* .{ .x = e } —— 匿名结构体值。
         *
         * 有期望类型时由期望类型驱动（R6）：`var v: Vec = .{ .x = 1 }` 里的
         * 字面量就是 Vec，字段按 Vec 的类型检查；`fn f() Vec { return .{…}; }`
         * 同理。没有期望类型时它才自成一个匿名结构体（字段类型由初值推）。 */
        if (expect && (expect->kind == TY_STRUCT || expect->kind == TY_UNION) && expect->fields)
            return check_lit_fields(n, expect);

        Type *t = ty_agg(TY_STRUCT, n, NULL);
        if (!t->fields) {
            t->nfields = n->nitems;
            t->fields = calloc((size_t)(n->nitems ? n->nitems : 1), sizeof(Field));
            for (int i = 0; i < n->nitems; i++) {
                Node *p = n->items[i];
                t->fields[i].name = p->name;
                t->fields[i].ty = check_expr(p->a, NULL);
            }
        } else {
            for (int i = 0; i < n->nitems; i++)
                coerce_node(n->items[i]->a, check_expr(n->items[i]->a,
                            t->fields[i].ty), t->fields[i].ty, "field");
        }
        /* §173：无期望类型的匿名结构体同样烙字段账本 */
        for (int i = 0; i < n->nitems; i++) {
            int ftid, ftst;
            tag_of_expr(n->items[i]->a, &ftid, &ftst);
            if (ftid && (ftst == TC_LIVE || ftst == TC_FREED))
                ft_set((FieldTag **)&n->ftags, n->items[i]->name, ftid);
        }
        return type_of(n, t);
    }

    case NK_MODULE: {
        n->type_val = ty_void();
        if (n->str && !strcmp(n->str, "std")) {
            Module *sm = module_load_std(n->line, n->col);
            n->sym = sm ? sm->ns : NULL;
            return type_of(n, ty_type());
        }
        Module *m = n->str ? module_load(n->str, n->line, n->col) : NULL;
        if (!m) {
            if (!n->str) return BAD(n, "@mod(...) needs a file path");
            n->sym = NULL;
            return type_of(n, ty_type());
        }
        /* §206：循环导入允许。后端所有 pub 函数原型/全局声明都进共享
         * header，各单元 include 后即可互相调用——函数体互调、类型互引
         * 用都能正常链接。真正无法求值的是顶层 const/var 的值环，由
         * ensure_raw 的 E0601 精确拦截（见 §179/§206）。 */
        n->sym = m->ns;
        return type_of(n, ty_type());
    }

    case NK_FN: {
        if (n->flag) {                                  /* fn(…) T; 是类型 */
            Type *t = resolve_fn_type(n);
            n->ty = ty_type();
            n->type_val = t;
            return n->ty;
        }
        if (fn_is_generic(n)) {                         /* 签名等实例化 */
            if (n->b) n->ct = val_fn(ty_generic_fn(), n);
            return type_of(n, ty_generic_fn());
        }
        Type *ft = resolve_fn_type(n);
        if (n->b) schedule_body(n);
        n->type_val = ft;
        n->ct = val_fn(ft, n);
        return type_of(n, ft);                          /* fn(…) T { … } 是值 */
    }

    /* 类型表达式出现在值位置：`const S = struct{…}` / `*[3]i32` 等 */
    case NK_TYPE_IDENT:
    case NK_PTR_TYPE:
    case NK_SLICE_TYPE:
    case NK_TUPLE_TYPE:
    case NK_STRUCT_TYPE:
    case NK_UNION_TYPE:
    case NK_ENUM_TYPE:
    case NK_OPT_TYPE:
    case NK_ERR_TYPE:
    case NK_VEC_TYPE:
    case NK_ERR_SET: {
        Type *t = resolve_type(n);
        n->type_val = t;
        return type_of(n, ty_type());
    }

    case NK_BLOCK:
    case NK_IF:
    case NK_WHILE:
    case NK_FOR:
    case NK_SWITCH:
        return sema_construct(n, expect);

    case NK_RANGE:
        return BAD(n, "a range `lo..hi` is only valid inside `for (...)`");

    case NK_DEFER:
    case NK_ERRDEFER:
    case NK_RETURN:
    case NK_CONTINUE:
    case NK_YIELD:
    case NK_DECL:
        check_stmt(n);
        return type_of(n, ty_void());

    case NK_PAIR:
        return check_expr(n->a, expect);

    default:
        return BAD(n, "unsupported expression here (node kind %d)", (int)n->kind);
    }
}

/* ---------------- 语句 ---------------- */

/* 不可达代码：同一条语句序列里，前一条已经把控制转移走，后面就再也到不了。
 * 判定复用缺返回分析（`stmt_blocks_next` 定义在它旁边），只在源码位置报一次——
 * 泛型单态化会把同一段身体检查很多遍。 */
static int g_dead_diag[32][2];
static int g_ndead_diag;

/* 按源码位置去重：编译期折叠会把函数体 clone 一份再检查（ct_call），
 * 同一行同一列报一次就够了。 */
static bool dead_diag_dup(Node *s) {
    for (int i = 0; i < g_ndead_diag; i++)
        if (g_dead_diag[i][0] == s->line && g_dead_diag[i][1] == s->col) return true;
    if (g_ndead_diag < 32) {
        g_dead_diag[g_ndead_diag][0] = s->line;
        g_dead_diag[g_ndead_diag][1] = s->col;
        g_ndead_diag++;
    }
    return false;
}

/* 报错时说清楚"是哪一句把路堵死的" */
static void err_unreachable(Node *s, Node *prev) {
    if (dead_diag_dup(s)) return;
    const char *w;
    switch (prev->kind) {
    case NK_RETURN:   w = "`return`";        break;
    case NK_CONTINUE: w = "`continue`";      break;
    case NK_YIELD:    w = "yield";           break;
    case NK_BLOCK:    w = "`{...}` block";     break;
    case NK_IF:       w = "`if`/`else`";     break;
    case NK_SWITCH:   w = "`switch`";        break;
    case NK_WHILE:    w = "`while (true)`";  break;
    default:          w = "expression of type `noreturn`"; break;
    }
    err_at(s->line, s->col, "unreachable code: the %s above never falls through", w);
}

static void check_stmts(Node *blk) {
    Node *prev = NULL;
    bool dead = false;
    /* §181：defer/errdefer 在离开作用域时才执行，后端 run_defers 按 LIFO
     * 逆序展开。前端不能按源码顺序当场 check defer 体——否则里面登记的
     * free 会污染其后语句，把正常读取误报成 use-after-free。
     * 两遍：先查所有普通语句；再把 defer/errdefer 按源码逆序（=运行顺序）
     * 查。dead/unreachable 与 fn 名贴标仍在第一遍按源码序做一次。 */
    for (int pass = 0; pass < 2; pass++) {
        for (int j = 0; j < blk->nitems; j++) {
            int i = pass == 0 ? j : blk->nitems - 1 - j;
            Node *s = blk->items[i];
            bool is_def = (s->kind == NK_DEFER || s->kind == NK_ERRDEFER);
            if (pass == 0) {
                if (dead) err_unreachable(s, prev);
                /* 具名绑定到 fn 字面量的，把名字贴上：const helper = fn () void{…}
                 * 这种写法下 fn 节点本身没有名字，@fnName() 就只会拿到 "?"。 */
                if (s->kind == NK_DECL && s->name && s->b && s->b->kind == NK_FN && !s->b->name)
                    s->b->name = s->name;
                /* defer 体留到第二遍；这里只参与 dead 传播与 prev 记录 */
                if (is_def) { prev = s; continue; }
                check_stmt(s);
                if (!dead && stmt_blocks_next(s)) dead = true;
                prev = s;
            } else {
                /* 第二遍：仅 defer/errdefer，逆序 = 后端 LIFO 执行顺序 */
                if (is_def) check_stmt(s);
            }
        }
    }
}

static const char *mangle_name(const char *name);

/* 局部变量的 C 名：模块前缀之后再按声明位置加后缀。
 *
 * C 的块作用域允许遮蔽，但后端把语句**就地**发散在同一层 C 块里（if/loop
 * 的臂有时是单语句、不额外开块），于是两个同名局部——不论是一个遮蔽外层、
 * 还是两个兄弟块各写一个——都会撞成 "redefinition of 'x'"。用「行_列」当
 * 后缀：同一个声明每次检查得到同一个名字（幂等），不需要任何计数器状态，
 * 生成出来的 C 也还能对着源码看。 */
static const char *local_cname(const char *name, int line, int col) {
    Buf b; buf_init(&b);
    const char *base = mangle_name(name);
    buf_puts(&b, base ? base : (name && *name ? name : "v"));
    buf_printf(&b, "_%d_%d", line, col);
    return b.data;
}

/* 非入口模块的顶层名字都加前缀：两个文件里的 `add` 在 C 里不能撞车 */
static const char *mangle_name(const char *name) {
    if (!g_mod || !g_mod->stem || g_mod == g_mods[0] || !name) return NULL;
    Buf b; buf_init(&b);
    buf_puts(&b, g_mod->stem);
    buf_putc(&b, '_');
    buf_puts(&b, name);
    for (char *q = b.data; *q; q++) {         /* 文件名里的怪字符换成下划线 */
        char c = *q;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_';
        if (!ok) *q = '_';
    }
    return b.data;
}

/* ---------------- 名字别名 ---------------- */

/* 别名不复制实体，只复制"指向"：decl / cname / ty 全部沿用模块里那份。
 * 所以 `const add = util.add;` 之后，`add(1, 2)` 生成的 C 调用还是 `util_add`。
 * 这不是导入语法——`const heap = std.heap;` 就是普通 const，只是右值指向一个
 * 可以静态命名的东西（函数 / 类型 / 命名空间 / 编译期常量），于是不占存储。 */
static void sym_link(Symbol *dst, Symbol *src) {
    bool pub = dst->is_pub;   /* 导出与否看别名自己那条 `pub`，不看被指向的那份 */
    ensure(src);
    sym_copy(dst, src, NULL);
    dst->is_pub  = pub;
    dst->state   = 2;
    dst->ct_bind = true;      /* 别名没有自己的 C 存储 */
}

static Symbol *module_member(Module *m, const char *name) {
    for (Symbol *s = m->scope.first; s; s = s->next)
        if (s->name && !strcmp(s->name, name)) return s;
    return NULL;
}

/* 顶层 const 的初值是不是"惰性模块引用"：@mod("...") 本身，或者指向
 * 这类别名的成员链（std.zx 顶层的 `pub const HashMap = map.HashMap;`
 * 就是后者）。
 *
 * 为什么要惰性：模块加载走的是 check_stmts(m->root) → 每条顶层 decl
 * 都 ensure 一遍，于是 `pub const str = @mod("str.zx")` 在 std 一被
 * 加载时就把 str/map/pool/fs/json/time/task 全部拉进编译——哪怕程序
 * 只用了 std.println。惰性声明不做这次急切解析，真正被引用时 ensure
 * 会沿原有路径（NK_MODULE 分支 / 别名 sym_link）照常解析并加载文件。
 * 没人用的模块根本不进 g_mods：后端少发射几个单元、链接器少收几个 .o。
 *
 * 代价是诊断时机的变化：一个从未被引用的坏模块不再报错。这是取舍而
 * 不是缺陷——和"没人调用的泛型不实例化就不检查"是同一条哲学。 */
static bool decl_is_lazy_module(Node *d) {
    if (!d || d->kind != NK_DECL || d->a || !d->b || d->c) return false;
    Node *init = d->b;
    if (init->kind == NK_MODULE) return true;
    if (init->kind != NK_FIELD) return false;
    /* 成员链：根标识符必须解析到"初值是 @mod(...) 的 const 别名"，
     * 普通 `const X = someStruct.field;` 不在此列，维持急切解析。 */
    Node *root = init;
    while (root->kind == NK_FIELD && root->a) root = root->a;
    if (root->kind != NK_IDENT || !root->name) return false;
    Symbol *s = root->sym;
    if (!s && g_mod) s = module_member(g_mod, root->name);
    if (!s || s->kind != SK_CONST || !s->decl) return false;
    Node *sd = s->decl;
    return !sd->a && sd->b && sd->b->kind == NK_MODULE;
}

static void hoist(Scope *sc, Node *blk) {
    bool file_scope = (g_mod && sc == &g_mod->scope);

    for (int i = 0; i < blk->nitems; i++) {
        Node *it = blk->items[i];
        if (it->is_pub && !file_scope) {
            err_at(it->line, it->col, "`pub` is only allowed on file-scope declarations");
            it->is_pub = false;
        }
        if (it->kind == NK_FN && it->name && file_scope && g_mod != g_mods[0] &&
            !strcmp(it->name, "main"))
            err_at(it->line, it->col,
                   "only the entry file may declare `main`; '%s' is a module",
                   g_mod->stem);

        /* §186：入口签名在语义阶段把关——main 带形参会原样生成 C 的
         * `int main(…)`，bool/f64 之类到 clang 才炸，被报成
         * "compiler bug"（B1002）。命令行参数一律走
         * std.process.args()（argv[0] 是程序名，不在列表里）。 */
        if (file_scope && g_mod == g_mods[0]) {
            Node *mf = NULL;
            if (it->kind == NK_FN && it->name && !strcmp(it->name, "main"))
                mf = it;
            else if (it->kind == NK_DECL && it->name &&
                     !strcmp(it->name, "main") && it->b &&
                     it->b->kind == NK_FN)
                mf = it->b;
            if (mf && mf->nitems > 0)
                diag_code_at("E0309", mf->line, mf->col,
                    "main takes no parameters -- command-line arguments come "
                    "from std.process.args() (argv[0] is the program name and "
                    "is excluded); a main with parameters cannot map to a "
                    "valid C entry signature");
        }

        if (it->kind == NK_DECL) {
            if (it->c) {
                for (int k = 0; k < it->c->nitems; k++) {
                    Node *id = it->c->items[k];
                    Symbol *s = scope_add_at(sc, id->name, SK_CONST, id->line, id->col);
                    if (!s) {
                        Symbol *prev = scope_find_here(sc, id->name);
                        if (prev && prev->decl_line)
                            diag_note(NULL, prev->decl_line, prev->decl_col, "'%s' first declared here", id->name);
                        err_at(id->line, id->col, "duplicate declaration '%s'", id->name);
                        continue;
                    }
                    s->decl = it;
                    s->index = k;
                    s->mod = g_mod;
                    s->is_pub = it->is_pub;
                    s->cname = file_scope ? mangle_name(id->name)
                                          : local_cname(id->name, id->line, id->col);
                    id->sym = s;
                    if (k == 0) it->sym = s;
                    /* 判据是"当前作用域不是模块"，不能看 g_fn_depth：
                     * 模块是在**函数体里**懒性加载的（第一次用到 std 就在那时），
                     * g_fn_depth 那时已>0＜误把模块顶层声明当成局部。 */
                    if (!sc->is_module) local_track(s, id->line, id->col);
                    s->fn_local = (!sc->is_module && g_fn_depth > 0);
                    s->fn_d = s->fn_local ? g_fn_depth : 0;  /* §192 闭包检查 */
                }
            } else if (it->name) {
                int kind = it->op == 2 ? SK_VAR : it->op == 1 ? SK_LET : SK_CONST;
                Symbol *s = scope_add_at(sc, it->name, kind, it->line, it->col);
                if (!s) {
                    Symbol *prev = scope_find_here(sc, it->name);
                    if (prev && prev->decl_line)
                        diag_note(NULL, prev->decl_line, prev->decl_col, "'%s' first declared here", it->name);
                    err_at(it->line, it->col, "duplicate declaration '%s'", it->name);
                    continue;
                }
                s->decl = it;
                s->mod = g_mod;
                s->is_pub = it->is_pub;
                /* extern var 同 extern fn：外部符号用**原名**当 C 名 */
                s->cname = it->is_extern ? it->name
                         : (!sc->is_module ? local_cname(it->name, it->line, it->col)
                                           : mangle_name(it->name));
                /* 模块是在函数体里懒加载的，g_fn_depth 已 > 0；
                判文件作用域要看 sc->is_module，否则模块顶层的 extern var 会被误判成局部。 */
                s->in_fn = (!sc->is_module && g_fn_depth > 0);
                s->fn_local = s->in_fn;
                s->fn_d = s->in_fn ? g_fn_depth : 0;   /* §192 闭包检查 */
                s->is_top = file_scope; /* 只有文件根作用域才是真全局；comptime 块内的局部槽位不算 */
                it->sym = s;
                if (!sc->is_module) local_track(s, it->line, it->col);
                /* 泛型 lambda（`const L = fn (T: type) type { … }`）也要知道自己
                 * 属于哪个模块——实例化时得回到定义处的模块作用域里检查体。 */
                if (it->b && it->b->kind == NK_FN) it->b->sym = s;
            }
        } else if (it->kind == NK_FN && it->name) {
            Symbol *s = scope_add_at(sc, it->name, SK_FN, it->line, it->col);
            if (!s) {
                Symbol *prev = scope_find_here(sc, it->name);
                if (prev && prev->decl_line)
                    diag_note(NULL, prev->decl_line, prev->decl_col, "'%s' first declared here", it->name);
                err_at(it->line, it->col, "duplicate declaration '%s'", it->name);
                continue;
            }
            s->decl = it;
            s->mod = g_mod;
            s->is_pub = it->is_pub;
            /* extern fn 用**原名**当 C 名：它指向的是别人已经编好的符号，
             * 加模块前缀就链接不到了 */
            s->cname = it->is_extern ? it->name : mangle_name(it->name);
            s->fn_local = (!sc->is_module && g_fn_depth > 0);
            it->sym = s;
        }
    }
}

static void ensure_raw(Symbol *s) {
    if (!s || s->kind == SK_NS) return;
    if (s->state == 2) return;
    if (s->state == 3) return;      /* 外壳挂好了，成员表由外层那次调用填完 */
    if (s->state == 1) {
        err_at(s->decl ? s->decl->line : 0, s->decl ? s->decl->col : 0,
               "cyclic dependency while resolving '%s'", s->name ? s->name : "?");
        s->state = 2;
        s->ty = ty_invalid();
        return;
    }
    s->state = 1;

    if (s->kind == SK_FN) {
        if (fn_is_generic(s->decl)) {
            s->ty = ty_generic_fn();          /* 签名等实例化时再算 */
            s->state = 2;
            return;
        }
        s->ty = resolve_fn_type(s->decl);
        /* extern fn：外部 C 函数，没有函数体也不进 C 里定义（M23） */
        if (s->decl->is_extern) { s->state = 2; return; }
        if (s->decl->b && !fn_is_generic(s->decl))
            s->ct = val_fn(s->ty, s->decl);      /* 顶层 fn 也是编译期值 */
        schedule_body(s->decl);
        s->state = 2;
        return;
    }

    if (s->kind == SK_PARAM || s->kind == SK_CAPTURE) { s->state = 2; return; }

    Node *d = s->decl;
    if (!d) { s->state = 2; s->ty = ty_invalid(); return; }

    /* extern var：外部 C 全局变量（M24）。C 侧拥有存储——必须有类型标注、
     * 不能有初始化值；只在文件根有意义（函数里的 extern 声明没有 Zx 语义）。
     * 类型同样要过 C 布局校验：这个变量要跟 C 那边的定义逐字节对上。 */
    if (d->is_extern) {
        if (s->in_fn) {
            err_at(d->line, d->col,
                   "extern var belongs at file scope -- function-local extern "
                   "declarations have no Zx semantics");
        } else if (d->op != 2) {
            err_at(d->line, d->col,
                   "`extern var` is the only extern binding -- const/let imply "
                   "a Zx-side initializer, which would fight the C definition");
        } else if (!d->a) {
            err_at(d->line, d->col,
                   "extern var '%s' needs a type annotation (`extern var %s: T;`)",
                   s->name ? s->name : "?", s->name ? s->name : "?");
        } else if (d->b) {
            err_at(d->line, d->col,
                   "extern var '%s' takes no initializer -- the C side owns the storage",
                   s->name ? s->name : "?");
        } else {
            Type *t = resolve_type(d->a);
            check_c_able(d, t, s->name);
            s->ty = t;
            d->ty = t;
        }
        if (!s->ty) s->ty = ty_invalid();
        s->state = 2;
        return;
    }

    /* `const heap = std.heap;` / `const square = mathx.square;`
     *
     * 右值指向模块成员时，这个 const 就是个别名：不占存储、不要求 R1，
     * decl / cname / ty / ct 全部沿用那边那份。于是"只把几个名字拿进来"
     * 就是普通的 const 绑定，不需要单独的导入语法——而且这条路径对
     * 函数、类型、命名空间、编译期常量一视同仁。 */
    if (s->kind == SK_CONST && !d->a && d->b && d->b->kind == NK_FIELD) {
        check_expr(d->b, NULL);
        Symbol *src = d->b->sym;
        if (src && src != s && src->kind != SK_VAR && src->kind != SK_LET &&
            src->kind != SK_PARAM && src->kind != SK_CAPTURE) {
            sym_link(s, src);
            d->ty = s->ty;
            return;
        }
    }

    /* 解构：const {a, b, c} = tup; */
    if (s->index >= 0) {
        if (!d->ty) d->ty = check_expr(d->b, NULL);
        Type *it = d->ty;
        if (it->kind == TY_TUPLE && s->index < it->nfields)
            s->ty = it->fields[s->index].ty;
        else {
            err_at(d->line, d->col, "cannot destructure `%s` (needs a tuple of at least %d element(s))",
                   ty_str(it), s->index + 1);
            s->ty = ty_invalid();
        }
        /* 解构出来的分量也带上编译期值，这样 `const {a,b} = {1,2}` 里的 a 仍是常量 */
        Value *agg = fold(d->b);
        if (agg && agg->kind == V_AGG && s->index < agg->nitems)
            s->ct = agg->items[s->index];
        else if (s->kind == SK_CONST)
            err_at(d->line, d->col,
                   "const '%s' must be computable at compile time (R1)", s->name ? s->name : "?");
        /* 后端要按分量声明全局/局部，所以把类型和值也挂到标识符节点上 */
        if (d->c && s->index < d->c->nitems) {
            Node *id = d->c->items[s->index];
            id->ty = s->ty;
            id->ct = s->ct;
        }
        s->state = 2;
        return;
    }

    /* 自引用类型（R5）：先把聚合外壳建出来挂在符号上，字段里遇到 *Node 就有东西可取。
     * state = 3 的意思是"外壳已就绪、成员表还没填"。
     * 身份 = 声明节点：每个 struct/union/enum 声明各有自己的 Type（ty_agg
     * 按 decl 节点 intern）。跨模块同名声明是两个不同类型；别名 `const M =
     * mod.T` 由上面的 sym_link 直接复制同一份 Type*，是同一个类型。 */
    if (!d->a && d->b &&
        (d->b->kind == NK_STRUCT_TYPE || d->b->kind == NK_UNION_TYPE ||
         d->b->kind == NK_ENUM_TYPE)) {
        TyKind k = d->b->kind == NK_UNION_TYPE ? TY_UNION
                 : d->b->kind == NK_ENUM_TYPE  ? TY_ENUM
                 : TY_STRUCT;
        s->type_val = ty_agg(k, ident_decl(d->b), NULL);
        s->ty = ty_type();
        s->state = 3;
    }

    Type *ann = d->a ? resolve_type(d->a) : NULL;
    if (sema_ty_ct_only(ann) && s->kind != SK_CONST) {
        /* Type / Field 只活在编译期：var/let 要给它们开运行时
         * 存储，没有对应物。要描述符请用 const（值在 ct 里，不占存储）。
         * `const info: Type = @type(T);` 是**合法**写法（LSP 的行内提示
         * 就是这么写的），只有 var/let 才拦（用户 2026-11-11）。 */
        err_at(d->line, d->col,
               "`%s` exists only at compile time; bind it with `const` "
               "straight from `@type`", ty_str(ann));
        return;
    }
    Type *init = d->b ? check_expr(d->b, ann) : NULL;
    if (d->b) s->stack_val = expr_frame(d->b);   /* 初值指向当前栈帧？记下来 */

    if (ann && init) {
        coerce_node(d->b, init, ann, "initializer");
        s->ty = ann;
    } else if (init) {
        Type *t = init;
        if (!d->a && (t->kind == TY_UNDEF || t->kind == TY_NULL)) {
            err_at(d->line, d->col,
                   "cannot infer the type of '%s'; `%s` needs a target type",
                   d->name ? d->name : "?", t->kind == TY_UNDEF ? "undefined" : "null");
            s->ty = ty_invalid();
            s->state = 2;
            d->ty = s->ty;
            return;
        }
        /* var x = e; —— 无标注的字面量给默认宽度（R7） */
        if (!d->a && (t->kind == TY_COMPTIME_INT || t->kind == TY_COMPTIME_FLOAT)) {
            /* M19 补漏：无标注的 comptime_int 字面量默认 i32，装不下时以前
             * 会静默截断（`const big = 9223372036854775807;` 变 -1）。现在
             * 先放宽到 i64；还装不下就报 E0302，让用户写类型注解——绝不允许
             * 静默截断。非纯字面量（lit_fits_expr 管不着的）维持默认宽度。 */
            if (t->kind == TY_COMPTIME_INT && !lit_fits_expr(d->b, ty_i32())) {
                if (lit_fits_expr(d->b, ty_int(64, true))) {
                    t = ty_int(64, true);
                } else {
                    char digits[64];
                    if (d->b->flag && d->b->str) lit_digits(d->b, digits, sizeof digits);
                    else snprintf(digits, sizeof digits, "%llu", (unsigned long long)d->b->ival);
                    err_at(d->line, d->col,
                           "integer literal %s does not fit in `%s`; "
                           "add a type annotation (`i64` / `i128` / `u64` ...)",
                           digits, ty_str(ty_i32()));
                    s->ty = ty_invalid();
                    s->state = 2;
                    d->ty = s->ty;
                    return;
                }
            } else {
                t = scalar_default(t);
            }
            d->b->ty = t;
        }
        /* let / const 绑定数组字面量 → 只读视图（R9）。只收字面量本尊
         * （`&.{…}` / `.{…}`）：字面量的匿名存储可以落成只读。cast /
         * stackAlloc 出来的 *[N]T 指向的是**别人的存储**（struct 字段、
         * 栈槽），可变性跟着被指物走——一刀切变 *const 会把
         * `let p = (*[8]i32)(&h.buf)` 这种合法视图模式打死（§182）。 */
        Node *ainit = d->b;
        bool is_arrlit = ainit && (ainit->kind == NK_ARRAY_LIT ||
            (ainit->kind == NK_UN && ainit->op == TOK_AMP && ainit->a &&
             ainit->a->kind == NK_ARRAY_LIT));
        if (!d->a && ty_is_array_ptr(t) && !t->is_const &&
            s->kind != SK_VAR && is_arrlit) {
            t = ty_ptr(t->elem, true, false, 0);
            d->b->ty = t;
        }
        s->ty = t;
    } else if (ann) {
        s->ty = ann;
    } else {
        err_at(d->line, d->col, "cannot infer the type of '%s'; add a type annotation",
               d->name ? d->name : "?");
        s->ty = ty_invalid();
    }

    /* ct-only 类型从初始化器推断进来也一样拦：var info = @type(i32);
     * 要给 Type 开运行时存储，没有对应物。const 绑定放行。 */
    if (s->kind != SK_CONST && s->ty && sema_ty_ct_only(s->ty)) {
        err_at(d->line, d->col,
               "`%s` exists only at compile time; bind it with `const` "
               "straight from `@type`", ty_str(s->ty));
        s->ty = ty_invalid();
        s->state = 2;
        d->ty = s->ty;
        return;
    }

    /* 类型值：const X = struct{…} */
    if (s->ty && s->ty->kind == TY_TYPE) {
        s->type_val = d->b ? d->b->type_val : NULL;
        if (s->type_val && !s->type_val->name && d->name &&
            (s->type_val->kind == TY_STRUCT || s->type_val->kind == TY_UNION ||
             s->type_val->kind == TY_ENUM))
            s->type_val->name = d->name;
    }

    if (d->b && d->b->kind == NK_MODULE && s->ty && s->ty->kind == TY_TYPE) {
        s->kind = SK_NS;
        Symbol *ns = d->b->sym;
        if (ns && ns->mod) {          /* 文件模块：借用它的成员表（名字还是本地别名） */
            s->mod = ns->mod;
            s->ty = ty_type();
            s->type_val = ty_void();
            s->state = 2;
            return;
        }
        /* 模块加载失败：错误已经在 @mod(...) 那行报过。绑定照旧算"命名
         * 空间"（类型检查要继续走），但标成 broken —— 后面 `mod.x` 全部
         * 静默退化成 invalid，不再刷一串 "module 'main' has no member"。 */
        s->broken = true;
        s->mod = NULL;
        s->ty = ty_type();
        s->type_val = ty_void();
        s->state = 2;
        return;
    }

    if (d->b) {
        s->ct = fold(d->b);
        /* 语句形初始化器（带标签的循环 / 块）会被编译期执行，执行把
         * 最后一轮的中间值烙在真实节点上（`a*b == 6` 在 yield 那轮被
         * 烙成 true）。s->ct 已经拿到最终值，把这些**语句子树**里的烙印
         * 擦掉，只留根上的——顶层 const 的静态物化要读 d->b->ct。
         *
         * 只擦语句子树：普通表达式（字面量、`@type(T).size` 这种折过值的
         * 字段访问）的折叠结果就是最终值，擦掉等于把值弄丢——后端会发出
         * `0.size`，C 直接报 "invalid suffix on floating constant"
         * （用户 2026-11-10 报的 @type 那个 bug 就是这个）。 */
        if (s->ct) {
            ct_clear_stmts(d->b->a); ct_clear_stmts(d->b->b);
            ct_clear_stmts(d->b->c); ct_clear_stmts(d->b->d);
            for (int i = 0; i < d->b->nitems; i++) ct_clear_stmts(d->b->items[i]);
        }
    }

    /* R1：凡 const，编译器必须能在编译期算出值 */
    if (s->kind == SK_CONST && d->b && !s->ct && !s->type_val &&
        s->ty && s->ty->kind != TY_INVALID && s->ty->kind != TY_TAG &&
        d->b->kind != NK_MODULE &&
        (!init || init->kind != TY_INVALID))   /* 初始化器本身报过错就别再叠一条 */
        err_at(d->line, d->col,
               "const '%s' must be computable at compile time (R1); "
               "this initializer needs runtime values", s->name ? s->name : "?");

    d->ty = s->ty;
    s->state = 2;
}

/* ---------------- 模块（一个文件 = 一个模块） ---------------- */

/* 路径归一化：折叠 //、/./ 与 /../，分隔符统一成 '/'。
 * 模块查重靠字符串精确比较（module_find），不归一化的话同一文件经
 * "util.zx" 和 "../proj/util.zx" 两条路径进来会被当成两个模块各编一份，
 * 链接时符号重复定义。 */
static char *path_normalize(const char *p) {
    char prefix[8] = {0};              /* "" | "/" | "X:/" | "X:" */
    const char *body = p;
    if (((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) && p[1] == ':') {
        prefix[0] = p[0]; prefix[1] = ':';
        body = p + 2;
        if (*body == '/' || *body == '\\') { prefix[2] = '/'; body++; }
    } else if (*p == '/' || *p == '\\') {
        prefix[0] = '/';
        body = p + 1;
        while (*body == '/' || *body == '\\') body++;
    }
    bool rooted = (prefix[0] == '/' && (!prefix[1] || prefix[2] == '/')) ||
                  (prefix[1] == ':' && prefix[2] == '/');
    const char *segs[400];
    int slens[400], nseg = 0, up = 0;  /* up：根之上无处可去的前导 ".." */
    while (*body) {
        const char *e = body;
        while (*e && *e != '/' && *e != '\\') e++;
        int l = (int)(e - body);
        if (!(l == 1 && body[0] == '.')) {
            if (l == 2 && body[0] == '.' && body[1] == '.') {
                if (nseg > 0) nseg--;
                else if (!rooted) up++;
            } else if (nseg < 400) {
                segs[nseg] = body; slens[nseg] = l; nseg++;
            }
        }
        body = e;
        while (*body == '/' || *body == '\\') body++;
    }
    Buf b; buf_init(&b);
    buf_puts(&b, prefix);
    for (int i = 0; i < up; i++) { buf_puts(&b, ".."); buf_putc(&b, '/'); }
    for (int i = 0; i < nseg; i++) {
        buf_putn(&b, segs[i], (size_t)slens[i]);
        if (i + 1 < nseg) buf_putc(&b, '/');
    }
    if (b.len == 0) buf_puts(&b, prefix[0] ? prefix : ".");
    return b.data;
}

static char *path_dup_dir(const char *path) {    const char *s1 = strrchr(path, '/');
    const char *s2 = strrchr(path, '\\');
    const char *s = (!s1 || (s2 && s2 > s1)) ? s2 : s1;
    if (!s) return strdup(".");
    size_t n = (size_t)(s - path);
    if (n == 0) n = 1;                       /* "/x.zx" → "/" */
    char *d = malloc(n + 1);
    memcpy(d, path, n);
    d[n] = '\0';
    return d;
}

static char *path_join(const char *dir, const char *rel) {
    if (rel[0] == '/' || rel[0] == '\\' || (rel[0] && rel[1] == ':')) return strdup(rel);
    Buf b; buf_init(&b);
    if (dir && *dir && strcmp(dir, ".")) { buf_puts(&b, dir); buf_putc(&b, '/'); }
    buf_puts(&b, rel);
    return b.data;
}

/* 文件名去掉目录和扩展名，只留 [A-Za-z0-9_] */
static char *path_stem(const char *path) {
    const char *b1 = strrchr(path, '/');
    const char *b2 = strrchr(path, '\\');
    const char *b = (!b1 || (b2 && b2 > b1)) ? b2 : b1;
    b = b ? b + 1 : path;
    char *s = strdup(b);
    char *dot = strrchr(s, '.');
    if (dot && dot != s) *dot = '\0';
    if (!*s) { free(s); s = strdup("mod"); }
    return s;
}


static Module *module_load(const char *rel, int line, int col);

static Module *module_find(const char *path) {
    for (int i = 0; i < g_nmods; i++)
        if (g_mods[i]->path && !strcmp(g_mods[i]->path, path)) return g_mods[i];
    return NULL;
}

static bool file_exists(const char *p);

/* 工程级模块搜索路径（zx.json 的 "deps"，由 main.c 注入）：相对导入
 * 打不开时依次尝试 <搜索路径>/<模块名>。NULL 结尾，可为空。 */
static const char *const *g_search_paths;

void sema_set_search_paths(const char *const *dirs) { g_search_paths = dirs; }

/* 模块名末尾带了 .zx 吗（决定要不要补后缀去探） */
static bool has_zx_suffix(const char *p) {
    size_t l = strlen(p);
    return l >= 3 && !strcmp(p + l - 3, ".zx");
}

/* 探一个候选路径：原样打得开就用它；否则（模块名没写扩展名时）再试
 * <候选>.zx。返回 malloc 的路径，都没找到返回 NULL。
 *
 * 为什么补后缀：`@mod("ext")` 比 `@mod("ext.zx")` 是文档里一直在写的
 * 简写（README、init 生成的项目说明），但早期实现只认完整路径。补后缀
 * 是纯增量——显式写了 .zx 的老代码一个字都不用改。 */
static char *find_module_file(const char *cand, bool allow_zx) {
    if (file_exists(cand)) return strdup(cand);
    if (allow_zx) {
        char *w = malloc(strlen(cand) + 4);
        sprintf(w, "%s.zx", cand);
        if (file_exists(w)) return w;
        free(w);
    }
    return NULL;
}

static Module *module_load(const char *rel, int line, int col) {
    char *dir  = path_dup_dir(g_mod ? g_mod->path : g_srcpath);
    char *orig = path_join(dir, rel);
    free(dir);
    bool allow_zx = !has_zx_suffix(rel);

    /* ① 相对导入所在目录 → ② 工程级搜索路径（vendor/、deps） */
    char *full = find_module_file(orig, allow_zx);
    for (int i = 0; !full && g_search_paths && g_search_paths[i]; i++) {
        char *cand = path_join(g_search_paths[i], rel);
        full = find_module_file(cand, allow_zx);
        free(cand);
    }
    if (!full) full = orig;               /* 没找到：留着原路径去报错 */
    else free(orig);

    Module *m = module_load_full(full, line, col);
    return m;
}

static Module *module_load_full(const char *full_in, int line, int col) {
    char *full = path_normalize(full_in);   /* 查重前先归一化（见 path_normalize） */
    Module *old = module_find(full);
    if (old) return old;   /* §206：循环导入允许——已登记的模块直接复用 */

    /* 打得开吗？先探一下，保留 "cannot open module" 的语义
     * （解析/读源由 fe_parse_file 接手，Zx 前端在 zxfe 里读）。 */
    FILE *probe = fopen(full, "rb");
    if (!probe) {
        err_at(line, col, "cannot open module '%s'", full);
        return NULL;
    }
    fclose(probe);

    Module *m = calloc(1, sizeof(Module));
    m->path = full;
    m->stem = path_stem(full);
    m->loading = true;

    /* 同名文件（不同目录）要区分开，否则 C 名字会撞 */
    for (int i = 1; i < g_nmods; i++) {
        if (g_mods[i]->stem && !strcmp(g_mods[i]->stem, m->stem)) {
            char *nn = malloc(strlen(m->stem) + 12);
            sprintf(nn, "%s%d", m->stem, i + 1);
            m->stem = nn;
            break;
        }
    }

    if (g_nmods == g_cmods) {
        g_cmods = g_cmods ? g_cmods * 2 : 8;
        g_mods = realloc(g_mods, sizeof(Module *) * (size_t)g_cmods);
    }
    g_mods[g_nmods++] = m;      /* 先登记：循环导入才检测得到 */

    Module *save_m = g_mod;
    Scope  *save_s = g_scope;
    const char *save_p = g_srcpath;

    g_srcpath = m->path;
    /* 自举阶段 3：模块也走同一前端（默认 zxfe，--fe=c 时走 C 解析器）。
     * Zx 前端解析失败时诊断已由 zxfe 打到 stderr，这里补一条定位。 */
    diag_first_error_reset();
    fe_last_error_reset();
    m->root = fe_parse_file(m->path);
    if (m->root) stamp_mod(m->root, m->path);
    if (!m->root) {
        /* 这条诊断属于导入方：位置是 @mod(...) 调用点，源码片段也得取
         * 导入方那一行。之前 g_srcpath 还停在子模块上，于是行号是父文件的、
         * 内容却是子模块的同号行——把人指到一句无关的注释上。
         * 顺带把子模块的第一条错报出来，别让人只看到 failed to parse：
         * 走 C 前端时错在 diag 里，走 Zx 前端时错在 zxfe 的 stderr 里。 */
        int fl = 0, fc = 0;
        char fcode[16] = "", fmsg[256] = "";
        int have = diag_first_error(&fl, &fc, fcode, sizeof fcode, fmsg, sizeof fmsg)
                || fe_last_error(&fl, &fc, fcode, sizeof fcode, fmsg, sizeof fmsg);
        g_srcpath = save_p;
        if (have && fmsg[0])
            err_at(line, col, "module '%s' failed to parse: %d:%d: %s [%s]",
                   full, fl, fc, fmsg, fcode);
        else
            err_at(line, col, "module '%s' failed to parse", full);
        g_srcpath = m->path;
        return NULL;
    }

    g_mod = m;
    g_scope = &m->scope;        /* parent 为空：模块之间不共享名字 */
    m->scope.is_module = true;

    m->ns = calloc(1, sizeof(Symbol));
    m->ns->name = m->stem;
    m->ns->kind = SK_NS;
    m->ns->state = 2;
    m->ns->ty = ty_type();
    m->ns->type_val = ty_void();
    m->ns->mod = m;

    hoist(&m->scope, m->root);
    /* 编译器内建名字并进每个模块的作用域（print/file/io/Allocator…）。
     * 之前只种 std——std.zx 里能用裸 `io.`，fs.zx 里想转发 std.file
     * 却得绕。模块自己声明了同名：以它为准（scope_add 返回 NULL）。 */
    seed_std_builtins(&m->scope);
    g_root_stmts = true;
    check_stmts(m->root);
    g_root_stmts = false;

    g_srcpath = save_p;
    g_scope = save_s;
    g_mod = save_m;
    m->loading = false;
    return m;
}

/* ---------------- 容器成员的可见性 ---------------- */

/* 每个模块的 AST 装载后就打上"这条声明属于哪个文件"的戳。容器体里的成员
 * 默认只在本文件可见（pub 才对外），判断靠这个戳；泛型单态化克隆 AST 时
 * clone_tree 是整体拷贝，戳跟着一起过去——实例化点不会把 std 的私有成员
 * 变成"本地可见"。 */
static void stamp_mod(Node *n, const char *path) {
    if (!n) return;
    n->mod = path;
    stamp_mod(n->a, path);
    stamp_mod(n->b, path);
    stamp_mod(n->c, path);
    stamp_mod(n->d, path);
    for (int i = 0; i < n->nitems; i++) stamp_mod(n->items[i], path);
    for (int i = 0; i < n->nunroll; i++) stamp_mod(n->unroll[i], path);
}

/* 这个成员在当前文件里看不见吗？（声明在别的文件里、自己又没带 pub） */
static bool member_hidden(Type *owner, bool is_pub) {
    if (is_pub || !owner || !owner->mod) return false;
    if (!g_mod || !g_mod->path) return false;
    return strcmp(owner->mod, g_mod->path) != 0;
}

/* 看不见就报一条，返回 true（调用点据此收手）。 */
static bool deny_member(Node *n, Type *owner, bool is_pub, const char *name) {
    if (!member_hidden(owner, is_pub)) return false;
    err_at(n->line, n->col,
           "'%s' is a private member of `%s` (declared in %s); "
           "mark it `pub` in the container body to expose it",
           name ? name : "?", ty_str(owner), owner->mod);
    return true;
}

/* ---------------- 标准库源码（std/） ---------------- */

static char    g_exe_dir[1024];
static Module *g_std_mod;

void sema_set_argv0(const char *p) {
    if (!p) return;
    const char *slash = strrchr(p, '/');
    const char *bs    = strrchr(p, '\\');
    if (bs && (!slash || bs > slash)) slash = bs;
    if (!slash) { g_exe_dir[0] = 0; return; }
    size_t n = (size_t)(slash - p);
    if (n >= sizeof g_exe_dir) n = sizeof g_exe_dir - 1;
    memcpy(g_exe_dir, p, n);
    g_exe_dir[n] = 0;
}

static bool file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/* §208: 平台 std 镜像覆盖目录。zxc 交叉编译非宿主目标时，把生成的 std
 * 镜像（net/fs/time 已换成平台版）路径注入这里；find_std_path 优先用它。
 * 单目标/宿主构建不注入，行为与原来完全一致。 */
static char g_std_override[1400];

void sema_set_std_path(const char *dir) {
    if (dir && *dir) snprintf(g_std_override, sizeof g_std_override, "%s", dir);
    else g_std_override[0] = 0;
}

/* std 源码在哪：§208 平台镜像 → $ZX_STD → 可执行文件旁边 → 当前目录 */
static bool find_std_path(char *out, size_t cap) {
    if (g_std_override[0]) {
        snprintf(out, cap, "%s", g_std_override);
        if (file_exists(out)) return true;
        snprintf(out, cap, "%s/std.zx", g_std_override);
        if (file_exists(out)) return true;
    }
    const char *env = getenv("ZX_STD");
    if (env && *env) {
        snprintf(out, cap, "%s", env);
        if (file_exists(out)) return true;
        snprintf(out, cap, "%s/std.zx", env);
        if (file_exists(out)) return true;
    }
    if (g_exe_dir[0]) {
        snprintf(out, cap, "%s/std/std.zx", g_exe_dir);
        if (file_exists(out)) return true;
        snprintf(out, cap, "%s/../std/std.zx", g_exe_dir);
        if (file_exists(out)) return true;
    }
    snprintf(out, cap, "std/std.zx");
    return file_exists(out);
}

static void sym_copy(Symbol *dst, Symbol *src, const char *name) {
    dst->kind      = src->kind;
    dst->decl      = src->decl;
    dst->mod       = src->mod;
    dst->ty        = src->ty;
    dst->type_val  = src->type_val;
    dst->ct        = src->ct;
    dst->cname     = src->cname;
    dst->builtin   = src->builtin;
    dst->is_pub    = src->is_pub;
    dst->state     = src->state;
    dst->index     = src->index;
    if (name) dst->name = name;
}

static Module *module_load_std(int line, int col) {
    if (g_std_mod) return g_std_mod;
    char path[1400];
    if (!find_std_path(path, sizeof path)) {
        err_at(line, col, "cannot find the standard library (std/std.zx); "
                          "set ZX_STD to the directory that holds it");
        return NULL;
    }
    Module *m = module_load_full(path, line, col);
    if (!m) return NULL;
    g_std_mod = m;

    /* 编译器内建的名字并进 std 的作用域：用户写 `std.println`，
     * std.zx 自己写 `Allocator`，走的是同一张表。 */
    seed_std_builtins(&m->scope);
    return m;
}

/* 惰性解析一个符号。符号可能属于别的模块（`util.add`），这时要临时
 * 切到那个模块的作用域和源文件名——否则它的初始值会在本模块里找名字。 */
static void ensure(Symbol *s) {
    if (!s || s->kind == SK_NS || s->state == 2) return;
    Module *sm = g_mod;
    Scope  *ss = g_scope;
    const char *sp = g_srcpath;
    if (s->mod && s->mod != g_mod) {
        g_mod = s->mod;
        g_scope = &s->mod->scope;
        g_srcpath = s->mod->path;
    }
    ensure_raw(s);
    g_mod = sm;
    g_scope = ss;
    g_srcpath = sp;
}

static Type *check_fn_body(Node *fn) {
    const char *prev_fn = g_cur_fn_name;
    if (getenv("ZX_DBG")) fprintf(stderr, "[dbg] body fn=%s nitems=%d line=%d\n", fn->name ? fn->name : "?", fn->nitems, fn->line);
    g_cur_fn_name = fn->name ? fn->name : "?";
    Type *ft = resolve_fn_type(fn);
    if (!ft || ft->kind == TY_INVALID) return ft;
    Type *ret = ft->ret ? ft->ret : ty_void();

    Symbol *save_locals = g_locals;
    int save_errs = g_error_count;      /* 函数体里已经报过错就不再问"有没有没用过的变量"：
 * 错误恢复下这条判断多半是假象（表达式出错→
 * 检查提前收尾→后面的使用没被记上），
 * 而且这种噪音会盖住真正的根因。修完错重编，它自然会出现。 */
    g_locals = NULL;

    Scope sc;
    scope_push(&sc);
    /* §192 闭包检查：深度要先于形参绑定抬上去——形参和体内局部
     * 同属本帧（fn_d 相等），外层函数的形参/局部更浅（fn_d 更小），
     * 嵌套函数体里解析到更浅的栈槽就是捕获。 */
    g_fn_depth++;
    for (int i = 0; i < fn->nitems; i++) {
        Node *pm = fn->items[i];
        if (!pm->name) continue;
        Symbol *s = scope_add(&sc, pm->name, pm->flag ? SK_VAR : SK_PARAM);
        if (getenv("ZX_DBG")) fprintf(stderr, "[dbg]   bind param '%s' -> %p\n", pm->name ? pm->name : "(null)", (void*)s);
        if (!s) { err_at(pm->line, pm->col, "duplicate parameter '%s'", pm->name); continue; }
        if (sema_ty_ct_only(pm->ty))
            err_at(pm->line, pm->col,
                   "parameter '%s' has type `%s`, which exists only at compile time",
                   pm->name, ty_str(pm->ty));

        s->ty = pm->ty;
        s->state = 2;
        s->in_fn = true;      /* 形参槽位也在被调方的栈帧上：&pm 不可返回 */
        s->fn_d  = g_fn_depth;/* §192：本帧深度（与体内局部一致） */
        /* §151：函数类型上的 self 被升级成 `const Self *`，而形参节点的
         * ty 仍是 `Self`。给符号打标记，后端发射时补 `(*self)`。 */
        Type *w0 = (ft && ft->kind == TY_FN && ft->nparams > 0) ? ft->params[0] : NULL;
        if (i == 0 && pm->name && !strcmp(pm->name, "self") &&
            w0 && w0->kind == TY_PTR && !w0->has_len && !ty_is_array_ptr(w0) &&
            w0->is_const &&
            pm->ty && pm->ty->kind != TY_PTR && ty_eq(w0->elem, pm->ty))
            s->self_by_ptr = true;
        local_track(s, pm->line, pm->col);
        /* §173：跨函数追踪 —— 指针形参领一张抽象槽（借出语义）：体内
         * deref/free/传递走同一本 tag 账本。抽象槽不记分配事实
         * （不进 E0513/循环配平），函数体只查一次、所有调用点共用。 */
        if (pm->ty) {
            Type *pt = pm->ty;
            bool ptrish = pt->kind == TY_PTR || pt->kind == TY_SLICE ||
                          (pt->kind == TY_OPT && pt->elem &&
                           (pt->elem->kind == TY_PTR || pt->elem->kind == TY_SLICE));
            if (ptrish) {
                char nb[64];
                snprintf(nb, sizeof nb, "param#%d:%s", ++g_tag_auto_seq,
                         pm->name ? pm->name : "?");
                char *nm2 = strdup(nb);
                if (nm2) {
                    Node tmp2; memset(&tmp2, 0, sizeof tmp2);
                    tmp2.line = pm->line; tmp2.col = pm->col;
                    TagSlot *at2 = tag_decl(nm2, &tmp2);
                    if (at2) {
                        at2->abstract = true;
                        at2->state = TST_LIVE;
                        s->tag_id = (int)(at2 - g_tslots) + 1;
                        s->tag_state = TC_LIVE;
                        pslot_sym_add(s->tag_id, s);   /* §180 */
                    }
                }
            }
        }
        pm->sym = s;
    }

    Type *save_ret = g_ret;
    bool  save_defer = g_in_defer;      /* 进了函数体：defer 上下文不再适用 */
    g_ret = ret;
    g_in_defer = false;
    Label *fl = label_push(NULL, false);
    fl->is_fn = true;

    check_block(fn->b);
    check_return_paths(fn, ret);

    g_nlabels--;               /* 函数的标签作用域 */
    g_fn_depth--;
    g_ret = save_ret;
    g_in_defer = save_defer;
    check_unused(g_locals, save_errs);      /* 局部绑定问一遍，然后还上层函数的链 */
    g_locals = save_locals;
    scope_pop(&sc);
    g_cur_fn_name = prev_fn;    /* 还原外层函数名（嵌套/错误路径安全） */
    /* §173：返回流缓存在 body 查完时烙——此时 cast 烙印/局部符号已就位。 */
    fn_stamp_ret_flow(fn);
    return ft;
}

static Type *check_block(Node *n) {
    static int dbg_depth = 0;
    static double dbg_t0 = 0;
    if (getenv("ZX_TIMING2")) {
        if (dbg_depth == 0) dbg_t0 = (double)clock() / CLOCKS_PER_SEC;
        dbg_depth++;
        if (dbg_depth < 6 || dbg_depth % 250 == 0) {
            fprintf(stderr, "[blk] depth=%d t=%.3f s\n", dbg_depth,
                    (double)clock() / CLOCKS_PER_SEC - dbg_t0);
        }
    }
    Scope sc;
    scope_push(&sc);
    Label *lb = n->label ? label_push(n->label, false) : NULL;

    hoist(&sc, n);
    bool save_root = g_root_stmts;
    g_root_stmts = false;
    check_stmts(n);
    g_root_stmts = save_root;
    region_plan_block(n);

    Type *t = ty_void();
    if (lb) {
        t = lb->has ? lb->ty : ty_void();
        /* §170：把收割的值节点留给臂出口检查（贴标载体的传出通道） */
        if (lb->val) n->d = lb->val;
    }
    if (lb) label_pop();
    tag_scope_exit(&sc, n);     /* §173：stackAlloc 的槽随作用域消亡 */
    scope_pop(&sc);
    if (getenv("ZX_TIMING2")) dbg_depth--;
    n->ty = t;
    return t;
}

static void check_stmt(Node *n) {
    if (!n) return;
    switch (n->kind) {
    case NK_DECL:
        /* 文件作用域的惰性模块声明：不急切解析（见 decl_is_lazy_module）。
         * 函数体里的 decl 不在此列——那是真正的运行时绑定，必须当场算。 */
        if (g_root_stmts && decl_is_lazy_module(n)) return;
        if (n->c) {
            for (int k = 0; k < n->c->nitems; k++) ensure(n->c->items[k]->sym);
        } else if (n->sym) {
            ensure(n->sym);
        }
        /* §130：把分配点的发行版本挂到绑定符号上，free 才能查状态 */
        if (n->sym && n->b) {
            int rs = region_of_expr(n->b);
            if (rs) { n->sym->reg_slot = rs; region_bind(rs, n->sym); }
            /* §187：整数符号的出处污点跟右值走（赋值重绑同口径） */
            n->sym->ptr_taint = expr_taint(n->b) != 0;
            /* §170：贴标载体的 tag 状态跟着绑到符号上 */
            int tid, tst;
            tag_of_expr(n->b, &tid, &tst);
            if (tid) {
                n->sym->tag_id = tid; n->sym->tag_state = tst;
                TagSlot *ts = tag_slot(tid);
                /* const 别名只是句柄不是指针载体，不进账本（§172） */
                if (ts && n->sym->kind != SK_CONST) {
                    ts->carrier = n->sym;
                    if (tst == TC_LIVE && n->b) {   /* 所有权落在这个调用点 */
                        ts->own_line = n->b->line; ts->own_col = n->b->col;
                    }
                }
            }
            /* §173：结构体值的字段账本随绑定落到符号上 */
            FieldTag *fs = ftags_of_expr(n->b);
            if (!fs && n->b && n->b->kind == NK_FIELD) {
                /* §175：聚合抽取 let inner = tt.0——值是拷贝，字段账
                 * 子树跟着拷到新载体（"0.0"→槽 变 "0"→槽）。 */
                char key2[128];
                Node *fb2 = NULL;
                if (ft_path_key(n->b, key2, sizeof key2, &fb2) &&
                    fb2->kind == NK_IDENT && fb2->sym)
                    fs = ft_subtree((FieldTag *)fb2->sym->ftags, key2);
            }
            if (fs) n->sym->ftags = ft_clone(fs);
            /* §192：转移溯源——来源表达式挂到载体上（同 ASG 重绑口径） */
            n->sym->ft_src = n->b;
        }
        return;

    case NK_TAG_APPLY:
        /* 语句位置：tag 定义 / tag.close / tag.batch / 独立贴标 */
        if (!strcmp(n->name, "close")) {
            if (n->b)
                err_at(n->line, n->col, "`tag.close` takes no value");
            else if (g_mod)
                g_mod->tags_closed = true;   /* §170：本模块关闭强制贴标 */
            return;
        }
        if (!strcmp(n->name, "batch")) {
            /* §188：本模块实现的分配器是整批清算型（arena）——经它的
             * Allocator 视图在调用点做的 alloc/free 一并豁免（内存由
             * init/deinit 容器账管）。只对分配器实现模块有意义。 */
            if (n->b)
                err_at(n->line, n->col, "`tag.batch` takes no value");
            else if (g_mod)
                g_mod->tags_batch = true;
            return;
        }
        if (!n->b) {
            /* §172：声明语句已整个丢弃 —— 标签首次引用自动建槽，没有豁免：
             * 释放是 alloc/free 的职责，程序结束仍 LIVE 报 E0513。 */
            diag_code_at("E0519", n->line, n->col,
                "manual tag declaration is gone -- '%s' self-declares on "
                "first use (`tag.%s : expr`); remove this line",
                n->name, n->name);
            return;
        }
        if (n->b && n->b->kind == NK_NULL) {
            /* §174：点形式终结标记已废弃——`tag.p : null;` 里 p 是 tag
             * 名，与"标记用使用者自己的变量名"冲突。统一写
             * `tag p : null;`（空格形式，p 是变量名）。 */
            diag_code_at("E0519", n->line, n->col,
                "the `tag.<name> : null` mark form is retired -- mark by "
                "your own variable name: `tag %s : null;`", n->name);
            return;
        }
        err_at(n->line, n->col,
               "a tagged allocation must be bound to a `let` -- write "
               "`let p = tag.%s : expr;`", n->name);
        check_expr(n->b, NULL);
        return;

    case NK_TAG_BIND: {
        /* §174：语句位置（无 `= expr`）——已有变量的重绑/终结。
         *   tag v : tag.x;   已有变量续上 tag.x 的链（值必须无标签）
         *   tag v : null;    终结标记（释放责任自负，替代点形式） */
        if (n->b) {          /* 有值：走 let 降糖路径（check_expr） */
            check_expr(n, NULL);
            return;
        }
        if (n->a && n->a->kind == NK_NULL) {
            TagSlot *marks[64];
            int nmarks = 0;
            for (Scope *sc = g_scope; sc && nmarks < 64; sc = sc->parent) {
                Symbol *s = scope_find_here(sc, n->name);
                if (!s) continue;
                s->used = true;                 /* 标记也算使用（E0410） */
                /* 载体变量：当前绑定的 tag。 */
                if (s->tag_id && nmarks < 64) {
                    TagSlot *ts = tag_slot(s->tag_id);
                    if (ts) marks[nmarks++] = ts;
                }
                /* 数组基变量：存入它的所有贴标槽（释放责任对整个
                 * 数组自负，逐个标记）。§185：动态索引入群的槽同样
                 * 在此终结（群随后清空，双重标记按槽去重）。 */
                for (int i = 0; i < g_naesc && nmarks < 64; i++)
                    if (g_aesc[i].base == s && g_aesc[i].slot > 0) {
                        TagSlot *ts = tag_slot(g_aesc[i].slot);
                        if (ts) marks[nmarks++] = ts;
                    }
                for (int i = 0; i < g_nagt && nmarks < 64; i++)
                    if (g_agt[i].base == s && g_agt[i].slot > 0) {
                        bool dup = false;
                        for (int k = 0; k < nmarks; k++)
                            if (marks[k] == tag_slot(g_agt[i].slot))
                                { dup = true; break; }
                        if (!dup) {
                            TagSlot *ts = tag_slot(g_agt[i].slot);
                            if (ts) marks[nmarks++] = ts;
                        }
                    }
            /* §189（用户裁定）：臂内 `tag v : null;` 是**确认**，不是
             * 释放事件——写在臂内释放（defer 体或直接 free）之后，
             * 证明使用者确认过正确释放；合流时该槽按 FREED 落账。
             * 分支本就可能不走：没确认的释放合流后当作还没释放，
             * 不影响以后的 use/free。主路径（循环体/函数体）的终结
             * 标记保持释放事件语义不变。 */
            /* §189：结构体值载体的字段账——`tag s : null;` 一并
             * 收集 s 携带的字段槽（确认/终结按当前模式生效）。 */
            for (FieldTag *ft = s->ftags; ft && nmarks < 64; ft = ft->next)
                if (ft->slot > 0) {
                    bool dup = false;
                    for (int k = 0; k < nmarks; k++)
                        if (marks[k] == tag_slot(ft->slot))
                            { dup = true; break; }
                    if (!dup) {
                        TagSlot *ts = tag_slot(ft->slot);
                        if (ts) marks[nmarks++] = ts;
                    }
                }
            bool confirm_mode = g_arm_depth > 0;
            if (!confirm_mode) {
                /* §185：群成员随终结一并销账 + 基变量进关闭名单
                 * （终结后再注解读取报 E0524）。显式终结是整容器
                 * 出口的唯一通路；形参数组终结记 pffa 事实账，
                 * 调用点/重放把实参数组的群一起清（闭环）。
                 * 确认模式不销账——释放还没发生（可能只是 defer
                 * 里写着），销账是释放事件的一部分。 */
                agt_clear(s, n);
                if (s->kind == SK_PARAM) pffa_add(s);
            }
            if (nmarks) break;
        }
        if (!nmarks) {   /* 兜底：槽的载体名（符号可能已重绑/出账） */
            for (int i = 0; i < g_ntslots && !nmarks; i++)
                if (g_tslots[i].carrier && g_tslots[i].carrier->name &&
                    !strcmp(g_tslots[i].carrier->name, n->name))
                    marks[nmarks++] = &g_tslots[i];
        }
        if (!nmarks) {
            /* §189：豁免模块（tag.close / std 实现层）不建槽，确认/
             * 终结标记无处可挂——按 no-op 处理，不报 E0300。 */
            if (g_mod && (g_mod->tags_closed || g_mod->tags_batch ||
                          tag_module_exempt(g_mod)))
                return;
            err_at(n->line, n->col,
                   "no allocation named '%s' -- mark by a variable name: "
                   "the carrier it was bound to (`tag p : null;`) or the "
                   "array holding it (`tag xs : null;`)",
                   n->name);
        }
        else for (int i = 0; i < nmarks; i++) {
                TagSlot *ts = marks[i];
                if (g_arm_depth > 0) {
                    /* §189：确认模式——只挂确认标记，不动账。抽象槽
                     * （形参）也确认：臂内释放形参同样要过确认关。 */
                    ts->confirm_arm = true;
                    ts->confirm_line = n->line; ts->confirm_col = n->col;
                } else if (ts->abstract) {
                    continue;
                } else if (ts->state == TST_LIVE) {
                    ts->state = TST_FREED;
                    ts->ever_freed = true;
                    ts->free_line = n->line; ts->free_col = n->col;
                    ts->rt_pending = false;     /* 终结即接管运行期写入 */
                    if (ts->carrier) ts->carrier->tag_state = TC_FREED;
                }
            }
            return;
        }
        /* 续到已有变量：tag 必须已存在（不建槽）、槽 LIVE、变量无标签 */
        TagSlot *t = NULL;
        Node *tv = n->a;
        for (int g = 0; g < 6 && tv &&
             (tv->kind == NK_CAST || tv->kind == NK_UNWRAP); g++)
            tv = tv->kind == NK_CAST ? (tv->b ? tv->b : tv->a) : tv->a;
        if (tv && tv->kind == NK_TAG_APPLY && !tv->b) {
            t = tag_by_name(tv->name);          /* 不建槽：必须是旧标签 */
            if (!t) {
                diag_code_at("E0300", tv->line, tv->col,
                    "`tag.%s` does not exist -- a re-bind must continue an "
                    "existing label; new labels only come from tagging an "
                    "allocation (`let p = tag.x : expr;`)", tv->name);
                return;
            }
        } else if (tv && tv->kind == NK_BUILTIN && tv->name &&
                   !strcmp(tv->name, "tag")) {
            check_expr(tv, NULL);               /* §178：@tag(p) 当场解析 */
            t = tv->tag_id ? tag_slot(tv->tag_id) : NULL;
            if (!t) return;
        } else if (tv && tv->kind == NK_IDENT) {
            Symbol *ts = lookup(tv->name);
            if (ts) ts->used = true;        /* 标签位置不走 check_expr，手动记使用 */
            t = (ts && ts->tag_id) ? tag_slot(ts->tag_id) : NULL;
            if (!t) {
                diag_code_at("E0520", tv->line, tv->col,
                    "`%s` is not a label value -- the re-bind needs "
                    "`tag.x` (or a `const` label binding)", tv->name);
                return;
            }
        } else {
            err_at(n->line, n->col,
                   "the re-bind needs a label value after ':' -- write "
                   "`tag v : tag.x;` to continue a chain, or "
                   "`tag v : null;` to mark the release as handled");
            return;
        }
        if (t->abstract) {
            err_at(n->line, n->col,
                   "`tag.%s` is a parameter slot -- a re-bind cannot claim "
                   "an abstract borrow", t->name);
            return;
        }
        if (t->state != TST_LIVE) {
            err_at(n->line, n->col,
                   t->state == TST_FREED
                   ? "this chain already ended (freed at line %d) -- a "
                     "re-bind cannot revive a finished chain"
                   : "this label is not tracking any allocation -- nothing "
                     "to continue",
                   t->free_line);
            return;
        }
        Symbol *vs = lookup(n->name);
        if (!vs || (vs->kind != SK_VAR && vs->kind != SK_LET &&
                    vs->kind != SK_PARAM)) {
            err_at(n->line, n->col,
                   "no variable named '%s' -- the value-less re-bind "
                   "retags an existing variable; declare it first "
                   "(`let v = xs[i];`)", n->name);
            return;
        }
        if (vs->tag_id) {
            TagSlot *old = tag_slot(vs->tag_id);
            diag_code_at("E0522", n->line, n->col,
                "'%s' already carries label '%s' -- a re-bind is only for "
                "lost pointers; free the old allocation first",
                n->name, old ? old->name : "?");
            return;
        }
        vs->tag_id = (int)(t - g_tslots) + 1;
        vs->tag_state = TC_LIVE;
        t->carrier = vs;
        t->own_line = n->line;
        t->own_col = n->col;
        t->rt_pending = false;      /* 续链即接管运行期写入 */
        return;
    }

    case NK_FN:
        if (n->sym) ensure(n->sym);
        else { resolve_fn_type(n); if (!n->flag && n->b && !fn_is_generic(n)) schedule_body(n); }
        return;

    case NK_RETURN: {
        if (!g_fn_depth) { err_at(n->line, n->col, "`return` outside a function"); return; }
        if (g_in_defer) { err_at(n->line, n->col, "`return` is not allowed inside `defer`/`defer err`"); }
        Type *want = g_ret ? g_ret : ty_void();
        if (!n->a) {
            /* `!void` 的 `return;` 是"成功且没有 payload"：tag 置 0 就是全部值。
             * 没有这条，`!void` 的三条成功路径（走到底、`return;`、`return {}`）
             * 全被堵死，等于这种函数根本不能正常返回。 */
            bool ok_void = want->kind == TY_VOID ||
                           (want->kind == TY_ERR && want->elem && want->elem->kind == TY_VOID);
            if (!ok_void && want->kind != TY_INVALID)
                err_at(n->line, n->col, "function returns `%s`, but this `return` has no value",
                       ty_str(want));
            n->ty = ty_void();
            return;
        }
        Type *got = check_expr(n->a, want);
        coerce_node(n->a, got, want, "return value");
        /* 返回类型写了错误集（`E!T`）时，返回的错误码必须落在集合里。
         * 只看得出集合的情况：`return error.x`（单元素）、`return f()`（f 的集合）。
         * `(error)Enum.x` 这种旧写法看不出名字，跳过——那是 v0.2x 的码，
         * 由任何集合都收（不然升级会挡住存量代码）。 */
        if (want->kind == TY_ERR && want->eset) {
            void *gs = expr_errset(n->a, got);
            if (gs && !ty_errset_subset(gs, want->eset)) {
                diag_help("add `%s` to the return type: `%s!%s`",
                          errset_missing(gs, want->eset),
                          ty_errset_str(ty_errset_union(want->eset, gs)),
                          ty_str(want->elem));
                diag_code_at("E0901", n->a->line, n->a->col,
                    "this returns `%s`, which is not in the declared error set %s",
                    errset_missing(gs, want->eset), ty_errset_str(want->eset));
                return;
            }
        }
        /* 逃逸检查（decisions §44）：不许把指向当前栈帧的指针带出去。
         * §188（用户裁定）：看「是否携带标签」——返回值携带的任何栈槽
         * 都是悬垂，不管返回类型是裸指针还是裹进结构体/元组带走（此前
         * 只查返回类型形状，结构体裹栈指针整只漏过）。expr_frame 保留：
         * 没进账本的来源（&局部、&.{…} 匿名存储）只能靠形状看。 */
        {
            /* §201 补丁：不要只看 want 是不是裸指针/切片。`?*T` / `![]T`
             * 的载荷同样是指针，裹在 optional/error 里也照样悬垂。 */
            bool ptr_shape = want->kind == TY_PTR || want->kind == TY_SLICE ||
                ((want->kind == TY_OPT || want->kind == TY_ERR) && want->elem &&
                 (want->elem->kind == TY_PTR || want->elem->kind == TY_SLICE));
            /* §199：聚合返回值里藏的栈帧来源（S{ .s = &.{} }） */
            bool agg_esc = !ptr_shape && frame_in_agg(n->a, 0);
            /* §201 补丁：标量返回值不查栈来源。`p.a` 是 u64，即便 p 指向
             * 当前帧，读出来的标量也能安全返回；旧写法无条件算 expr_frame
             * 把 §152 的 `let p = &self; return p.a;` 全误杀了。聚合字面量
             * （S{ .s = &.{} }）自身 expr_frame 为假，但 agg_esc 直接算逃逸。 */
            bool esc = (ptr_shape && expr_frame(n->a)) || agg_esc;
            if (!esc) {
                int id, st;
                tag_of_expr(n->a, &id, &st);
                if (id) {
                    TagSlot *t = tag_slot(id);
                    if (t && t->stack && !t->abstract) esc = true;
                }
            }
            if (!esc) {
                for (FieldTag *g = ftags_of_expr(n->a); g; g = g->next) {
                    if (g->slot <= 0) continue;
                    TagSlot *t = tag_slot(g->slot);
                    if (t && t->stack && !t->abstract) { esc = true; break; }
                }
            }
            if (esc)
                err_at(n->a->line, n->a->col,
                       "cannot return a pointer into the current function's stack frame "
                       "(use std.heapAlloc for outliving storage)");
        }
        /* §173：返回贴标载体合法 —— 所有权随返回值转移，调用点的
         * 返回流分析（fn_returns_tag）会把 tag 接到调用结果上。 */
        return;
    }

    case NK_CONTINUE: {
        Label *l = label_find_loop(n->label);
        if (!l) {
            if (n->label) err_at(n->line, n->col, "no enclosing loop labeled '%s'", n->label);
            else err_at(n->line, n->col, "`continue` outside a loop");
        }
        return;
    }

    case NK_YIELD: {
        /* §213：label == NULL 是 `break`（脱糖成"最近外循环的收割"）。
         * 走 loop-only 查找：带标签的块/臂不吃 break，只认循环帧。 */
        bool is_break = n->label == NULL;
        if (g_in_defer) {
            err_at(n->line, n->col, is_break ? "`defer` cannot break" : "`defer` cannot yield");
            return;
        }
        Label *l = is_break ? label_find_loop(NULL) : label_find(n->label);
        if (!l) {
            if (is_break) err_at(n->line, n->col, "`break` requires an enclosing loop");
            else err_at(n->line, n->col, "no enclosing scope labeled '%s'", n->label);
            if (n->a) check_expr(n->a, NULL);
            return;
        }
        if (!n->a) {
            if (l->has) l->ty = unify(n, l->ty, ty_void());
            else { l->ty = ty_void(); l->has = true; }
            n->ty = ty_void();
            return;
        }
        /* §170：null 臂进来时公共类型升级成可选指针——"分支内释放返回
         * 空指针"的载体需要 `?T`，解引用前判空（`p.?`）。 */
        if (l->has && l->ty && l->ty->kind == TY_PTR &&
            n->a->kind == NK_NULL && l->ty != ty_invalid())
            l->ty = ty_opt(l->ty);
        Type *t = check_expr(n->a, l->has ? l->ty : NULL);
        if (!l->has) { l->ty = t; l->has = true; }
        else l->ty = unify(n, l->ty, t);
        l->val = n->a;
        n->ty = l->ty;
        return;
    }

    case NK_DEFER:
    case NK_ERRDEFER: {
        /* defer err 只在错误返回路径上执行——不返回错误的函数里它永远
         * 不会触发，几乎肯定是写错了（Zig 的 errdefer 同样拒绝）。 */
        if (n->kind == NK_ERRDEFER && (!g_ret || g_ret->kind != TY_ERR))
            err_at(n->line, n->col,
                   "`defer err` requires the enclosing function to return an "
                   "error union `!T`, found `%s`",
                   g_ret ? ty_str(g_ret) : "void");
        bool save = g_in_defer;
        g_in_defer = true;
        if (n->kind == NK_ERRDEFER && n->name) {
            /* §188：`defer err { … }` —— err 绑错误码（与 try 捕获同一
             * 口径：SK_CAPTURE、ty_error）。块里引用了 err 才算用过；
             * 没引用后端就不发绑定声明（省一个未用变量）。
             * B13 修复：给 err 绑定唯一 cname（行_列后缀，与局部变量同款），
             * 并挂到节点 sym。同一函数多个 defer err 在 try 展开点就地发射时，
             * 各自声明名唯一，不再撞成 C 的 err 重定义；块内 `err` 引用经符号
             * 解析拿到同一个 cname，声明与引用天然一致。 */
            Scope sc;
            scope_push(&sc);
            Symbol *s = scope_add(&sc, n->name, SK_CAPTURE);
            if (s) {
                s->ty = ty_error(); s->state = 2;
                s->cname = local_cname(n->name, n->line, n->col);
                n->sym = s;
            }
            if (n->a->kind == NK_BLOCK) check_block(n->a);
            else check_expr(n->a, NULL);
            n->ival = (s && s->used) ? 1 : 0;
            scope_pop(&sc);
        } else {
            if (n->a->kind == NK_BLOCK) check_block(n->a);
            else check_expr(n->a, NULL);
        }
        g_in_defer = save;
        return;
    }

    default: {
        /* 顶层语句是裸 @error 时只查类型、不做投机折叠：check_expr
         * 会顺手 fold 填缓存，而 fold 到 @error 就是引爆——可这里的
         * 语句值是要丢弃的，前面一条 `if (…) { return …; }` 特化成恒真
         * 返回后，这句在运行时根本到不了（2026-11-10 用户实测踩中：
         * "不支持类型"守卫一实例化就假爆）。真要炸的路径有三条，都不经过
         * 这里：if 语句的臂（check_arm 里 fold）、编译期执行真的走到
         * （ct_run）、以及当值被取的编译期表达式。 */
        if (n->kind == NK_BUILTIN && n->name && !strcmp(n->name, "error")) {
            check_expr_raw(n, NULL);
            return;
        }
        Type *st = check_expr(n, NULL);
        /* 语言规则「所有值都要被使用」：裸调用语句 `f();` 的非 void 返回值
         * 不许默默丢掉，必须显式 `_ = f();`（M26 丢弃）。void 调用、赋值
         * 语句不受影响；类型无效时上层已经报过错，别叠一条。 */
        if (n->kind == NK_CALL && st && st->kind != TY_VOID &&
            st->kind != TY_INVALID && st->kind != TY_NORETURN)
            err_at(n->line, n->col,
                   "return value of type `%s` is discarded; "
                   "receive it with `_ = f(...)` or use the value",
                   ty_str(st));
        return;
    }
    }
}

/* ---------------- 控制流结构（都是表达式） ---------------- */

static Type *check_arm(Node *a, Type *expect) {
    if (!a) return ty_void();
    g_arm_depth++;                  /* §188：臂内是释放禁区（E0528） */
    Type *r = (a->kind == NK_BLOCK) ? check_block(a) : check_expr(a, expect);
    g_arm_depth--;
    return r;
}

/* 表达式是不是"纯编译期"的：没有读 var / 参数 / 捕获，也没有调用。
 * 只有这种 if 条件才允许只检查一支。 */
static bool ct_pure(Node *n) {
    if (!n) return true;
    switch (n->kind) {
    case NK_IDENT:
        if (!n->sym) return true;
        /* 泛型编译期实参（`T: type`、`cap: usize`）是 ct_bind：没有 C 存储，
         * 值在 specialize 时烙进符号——和 const 一样纯净。没有这条例外，
         * `if (T == f64)` 这种泛型体内的编译期类型分派永远剪不了枝
         * （2026-11-10 用户实测踩中：每个实例把死分支也按当前 T 检查）。 */
        if (n->sym->ct_bind) return true;
        return n->sym->kind != SK_VAR && n->sym->kind != SK_PARAM &&
               n->sym->kind != SK_CAPTURE;
    case NK_CALL:
        return false;                /* 调用可能有运行时副作用/参数依赖 */
    case NK_BLOCK: case NK_IF: case NK_WHILE: case NK_FOR: case NK_SWITCH:
    case NK_DECL: case NK_RETURN: case NK_YIELD: case NK_DEFER:
    case NK_ERRDEFER:
        return false;
    default:
        break;
    }
    if (!ct_pure(n->a) || !ct_pure(n->b) || !ct_pure(n->c) || !ct_pure(n->d))
        return false;
    for (int i = 0; i < n->nitems; i++)
        if (!ct_pure(n->items[i])) return false;
    return true;
}

/* 被剪掉的那一臂不做类型检查，但它里面的标识符仍然算"被用过"：
 * `let v = if (DEBUG) x else y;` 里 x 只出现在死分支上，不标 used
 * 就会假报"未使用"。只标 used，不动类型——剪枝的意义就是不去碰它。 */
static void mark_uses(Node *n) {
    if (!n) return;
    if (n->kind == NK_IDENT && n->name) {
        /* 死分支没检查过，节点上的 sym 还是空的——按名字现查一次。
         * 只信 n->sym 的话这条标记等于没写（真实踩到：`let t = if (c) s else 0;`
         * 假报 s 未使用）。 */
        Symbol *s = n->sym ? n->sym : lookup(n->name);
        if (s) s->used = true;
    }
    mark_uses(n->a); mark_uses(n->b); mark_uses(n->c); mark_uses(n->d);
    for (int i = 0; i < n->nitems; i++) mark_uses(n->items[i]);
}

/* 臂的值是不是"就是捕获名本身"（M50 的糖）。语句臂会被解析器包成单语句块，
 * 所以两种形状都算。 */
static bool arm_is_capture_value(Node *a, const char *cap) {
    if (!a || !cap) return false;
    if (a->kind == NK_BLOCK) {
        if (a->nitems != 1) return false;
        a = a->items[0];
    }
    return a && a->kind == NK_IDENT && a->name && !strcmp(a->name, cap);
}

static Type *check_if(Node *n, Type *expect) {
    Label *lb = n->label ? label_push(n->label, false) : NULL;

    Type *c = check_expr(n->a, NULL);
    bool is_opt = c && c->kind == TY_OPT;
    bool is_err = c && c->kind == TY_ERR;
    if (!is_opt && !is_err && !ty_eq(c, ty_bool()))
        err_at(n->a->line, n->a->col,
               "`if` condition must be bool, or ?T / !T with a capture name; found `%s`",
               ty_str(c));
    Type *cap_ty = (is_opt || is_err) ? c->elem : NULL;

    /* `if (c) x else y`：bool 条件没有载荷可捕获，"x 是捕获名"这种读法在这里
     * 无所指——退回老语义，x 就是普通表达式（M50 的糖只在 ?T / !T 上成立）。
     * 名字又不在作用域里时，用户显然是照着糖写的：给一条指路的错，
     * 别让 `cap` 落成 "unknown identifier"。 */
    if (!is_opt && !is_err && n->name && arm_is_capture_value(n->b, n->name)) {
        if (ty_eq(c, ty_bool()) && !lookup(n->name)) {
            err_at(n->line, n->col,
                   "`%s` is not declared -- an `if` on a bool has nothing to capture; "
                   "write `if (c) { %s }` for a value arm", n->name, n->name);
            /* 这一臂别再往下检查：`cap` 必然也是"未声明"，级联只会多一条
             * 同义诊断。臂按 void 算，和 `!void` 捕获那条路一样，让调用处
             * 报真正的类型错（"expected i32, found void"）。 */
            n->name = NULL;
            n->b = NULL;
            n->meth_done = true;
        } else {
            n->name = NULL;
        }
    }

    /* `!void` / `?void` 没有载荷：`if (x) v { … }` 里的 `v` 无所指。
     * 这种写法一定是误会——想要"成功就做事"写 `if (x) { … }`，
     * 想要错误码写 `else e`。放过去会让后端生成 `void v = x.payload;`。 */
    if ((is_opt || is_err) && cap_ty && cap_ty->kind == TY_VOID && n->name) {
        err_at(n->line, n->col,
               "`%s` has no payload to capture -- write `if (x) { ... }` for the success "
               "branch, or `else e` for the error code", ty_str(c));
        cap_ty = NULL;
    }

    /* 条件是编译期常量时，只检查真的会走到的那一支：另一支可能只对
     * 某些类型成立（`if (@sizeOf(T) > 8) @compileError(…)`）。
     *
     * "编译期常量"必须是真的常量：只要读了 var / 参数 / 捕获，它就可能
     * 随运行时而变，不能拿折叠出来的临时值当作分支依据——否则另一支
     * 会连类型都不算，后端拿到一棵半成品树。 */
    Value *cv = n->a->ct;
    bool known = cv && cv->kind == V_BOOL && !cap_ty && ct_pure(n->a);
    /* 把"可以剪另一臂"的决定交给后端执行：只有语义层确认条件是编译期
     * 常量且纯净（不读 var / 参数 / 捕获）才打标。编译期执行循环会在
     * 真实节点上留下最后一轮的值（jumps.zx：`a*b == 6` 在 yield 那轮
     * 被烙成 true），那种烙印不能当剪枝依据。 */
    n->flag = known;
    /* KEEP：剪枝结果要存在节点上。后端取不到 n->a->ct（后续流程会清掉它），
     * 只靠 ct 判就会"两条臂都生成"——而被剪掉那条根本没检查过，生成的 C
     * 里引用的是未命名标识符（真实踩到的 B1001：`let m = if (a > b) a else b;`
     * 生成 `m = a;`，而局部变量实际叫 `a_3_5`）。 */
    if (known) n->ival = cv->bval ? 1 : 0;
    if (known && !cv->bval && n->b) mark_uses(n->b);
    if (known && cv->bval && n->c) mark_uses(n->c);

    Type *tt;
    ArmExit exT = { 0, TC_NONE, NULL };
    TagSnap snapT;
    tag_snap(&snapT);                 /* §170：臂出口对账的基准 */
    Symbol *chT[TAG_SNAP_MAX]; FieldTag *cfT[TAG_SNAP_MAX]; int nchT = 0;
    Symbol *chE[TAG_SNAP_MAX]; FieldTag *cfE[TAG_SNAP_MAX]; int nchE = 0;
    {
        Scope sc;
        scope_push(&sc);
        if (cap_ty && n->name) {
            Symbol *s = scope_add(&sc, n->name, SK_CAPTURE);
            if (s) { s->ty = cap_ty; s->state = 2;
                     s->reg_slot = region_of_expr(n->a);
                     s->cap_src = n->a;
                     tag_capture_inherit(s, n->a, NULL); }
        }
        tt = (known && !cv->bval) ? ty_void() : check_arm(n->b, expect);
        if (!(known && !cv->bval)) {
            /* 臂值：带标签 if 的收割挂在外层 Label（lb->val）；无标签块的
             * 收割才落在块节点上 */
            Node *tval = lb ? lb->val : ((n->b && n->b->kind == NK_BLOCK) ? n->b->d : n->b);
            exT = tag_arm_reconcile(tval, n->b, snapT.state, snapT.n, snapT.total);
        }
        /* §173 补丁五：臂内整结构体赋值改了哪些载体的字段账，恢复前先记下 */
        nchT = ft_arm_collect(&snapT, chT, cfT, TAG_SNAP_MAX);
        tag_restore(&snapT);          /* 臂 2 从同一进入状态开始 */
        scope_pop(&sc);
    }

    Type *et = NULL;
    ArmExit exE = { 0, TC_NONE, NULL };
    if (n->c && !(known && cv->bval)) {
        Scope sc;
        scope_push(&sc);
        /* `?T` 不许 else 捕获（v0.26）：值臂是"?"、错误捕获臂是"!"，
         * 同框就是被禁的 `?!T`。else **捕获**只属于 `!T`；`?T` 的空
         * 要么 `else { … }` 不带名，要么交给 `try`（抛 error.null）。
         * （meth_done 在 IF 上没有别的用途，拿它当"已报过"标记——
         *   同一节点可能经 ct 求值路径再来一遍，不能报两条。） */
        if (is_opt && n->d && n->d->kind == NK_IDENT && !n->meth_done) {
            n->meth_done = true;
            err_at(n->line, n->col,
                   "a `?T` capture cannot have an `else e` capture -- `?T` and `!T` "
                   "do not nest (?!T is illegal); use plain `else { ... }` for the empty "
                   "case, or `try` to treat null as the error `error.null`");
        }
        if (n->d && n->d->kind == NK_IDENT) {
            /* `?T` 报错后仍把 e 绑成 error 类型：只报这一条，别让
             * else 臂里对 e 的引用级联成一串未知标识符。 */
            Type *ft = is_err ? ty_error() : ty_error();
            if (ft) {
                Symbol *s = scope_add(&sc, n->d->name, SK_CAPTURE);
                if (s) { s->ty = ft; s->state = 2; }
                n->d->sym = s;
            }
        }
        et = check_arm(n->c, expect);
        Node *eval = lb ? lb->val : ((n->c && n->c->kind == NK_BLOCK) ? n->c->d : n->c);
        exE = tag_arm_reconcile(eval, n->c, snapT.state, snapT.n, snapT.total);
        Symbol *chE2[TAG_SNAP_MAX]; FieldTag *cfE2[TAG_SNAP_MAX];
        int nchE2 = ft_arm_collect(&snapT, chE2, cfE2, TAG_SNAP_MAX);
        tag_restore(&snapT);
        scope_pop(&sc);
        for (int i = 0; i < nchE2 && i < TAG_SNAP_MAX; i++) {
            chE[i] = chE2[i]; cfE[i] = cfE2[i];
        }
        nchE = nchE2;
    }

    /* §170：两臂出口合并；单臂（剪枝 / 无 else）直接沿用那支的出口 */
    if (known) {
        ArmExit one = cv->bval ? exT : exE;
        if (one.tag) { n->tag_id = one.tag; n->tag_state = one.state; }
    } else if (!n->c) {
        /* 无 else：隐含一支"返回零值（null）"，与 then 出口合并 */
        ArmExit none = { 0, TC_NONE, NULL };
        tag_join_arms(n, exT, none);
    } else {
        tag_join_arms(n, exT, exE);
    }
    /* §173 补丁五：字段账的臂间落账。剪枝 = 幸存臂的账直接生效；
     * 两臂都跑 = 逐符号保守合并（一致才保留）；无 else = then 的单边
     * 改动丢弃（运行期可能不进臂，落账会假报）。 */
    if (known) {
        Symbol **ch = cv->bval ? chT : chE;
        FieldTag **cf = cv->bval ? cfT : cfE;
        int nc = cv->bval ? nchT : nchE;
        for (int i = 0; i < nc; i++) ch[i]->ftags = cf[i];
    } else if (n->c) {
        ft_arms_join(chT, cfT, nchT, chE, cfE, nchE);
    }
    /* §173：语句臂的臂内释放 —— 状态机直接落账（槽 + 所有局部载体）：
     * 后续解引用 E0512，再 free E0509。§189：臂内 std.move 落 MOVED
     * ——原变量封死（E0529），终扫豁免（接收方的账闭环）。 */
    if ((exT.stmt || exE.stmt) && n->tag_id) {
        int jstate = exT.stmt ? exT.state : exE.state;
        TagSlot *t = tag_slot(n->tag_id);
        if (t && t->state == TST_LIVE) {
            if (jstate == TC_MOVED) {
                t->state = TST_MOVED;
                for (Symbol *ls = g_locals; ls; ls = ls->locals_next)
                    if (ls->tag_id == n->tag_id && ls->tag_state == TC_LIVE)
                        ls->tag_state = TC_MOVED;
                if (t->carrier && t->carrier->tag_state == TC_LIVE)
                    t->carrier->tag_state = TC_MOVED;
            } else {
                t->state = TST_FREED;
                t->ever_freed = true;
                t->free_line = n->line; t->free_col = n->col;
                for (Symbol *ls = g_locals; ls; ls = ls->locals_next)
                    if (ls->tag_id == n->tag_id && ls->tag_state == TC_LIVE)
                        ls->tag_state = TC_FREED;
                if (t->carrier && t->carrier->tag_state == TC_LIVE)
                    t->carrier->tag_state = TC_FREED;
            }
        }
    }

    Type *r;
    if (lb) {
        /* 前导标签的 `if` 没有任何臂对它收割（`lbl: value;`）：标签是死的，
         * 表达式也没有值——以前会静默推成 void，var x = … 就拿到一个
         * void 绑定，后端生成的代码全是垃圾。明确报出来。 */
        if (!lb->has)
            err_at(n->line, n->col,
                   "label '%s' on `if` is never harvested; write `lbl: { lbl: value; }` "
                   "inside an arm or drop the label", n->label ? n->label : "?");
        r = lb->has ? lb->ty : ty_void();
    }
    else if (known)                r = cv->bval ? tt : (et ? et : ty_void());
    else if (et)                   r = unify(n, tt, et);
    /* 没有 `else` 的 `if` 也能当值：条件为假时没有臂执行，值就是零值
     * （和"块没被收割"是 `{}` 同一条规则）。类型只能由 then 臂给出。 */
    else                           r = tt;
    if (lb) label_pop();
    n->ty = r;
    return r;
}

static Type *check_while(Node *n, Type *expect) {
    Label *lb = label_push(n->label, true);        /* 无标签也要有帧：continue 要能找到 */

    Type *c = check_expr(n->a, NULL);
    bool is_opt = c && c->kind == TY_OPT;
    bool is_err = c && c->kind == TY_ERR;
    if (!is_opt && !is_err && !ty_eq(c, ty_bool()))
        err_at(n->a->line, n->a->col,
               "`while` condition must be bool, or ?T / !T with a capture name; found `%s`",
               ty_str(c));
    Type *cap_ty = (is_opt || is_err) ? c->elem : NULL;

    {
        Scope sc;
        scope_push(&sc);
        if (cap_ty && n->name) {
            Symbol *s = scope_add(&sc, n->name, SK_CAPTURE);
            if (s) { s->ty = cap_ty; s->state = 2;
                     tag_capture_inherit(s, n->a, NULL); }
        }
        TagSnap snapW;                     /* §170：循环配平 */
        tag_snap(&snapW);
        check_block(n->b);
        tag_loop_balance(n->b, &snapW);
        tag_restore(&snapW);
        scope_pop(&sc);
    }

    /* `else` 是 zxfe 前端的历史残留：解析层收进 n->c，后端从不发射，
     * 等于一句永远不跑的死代码。语言裁定：循环没有 else——收尾逻辑写在
     * 循环后面；想区分"跑没跑过"用计数或标志位。 */
    if (n->c) {
        err_at(n->c->line, n->c->col,
               "`while` does not take an `else` branch; write that code after the loop");
        n->c = NULL;
    }

    Type *et = n->c ? check_arm(n->c, expect) : NULL;
    Type *r;
    if (n->label && lb && lb->has) r = lb->ty;
    else if (et)                   r = et;
    else                           r = ty_void();
    label_pop();
    n->ty = r;
    return r;
}

static Type *range_bind_type(Node *r, Type *lo) {
    if (!r->b) return ty_usize();                       /* 省略上界：当索引用 */
    if (lo->kind == TY_COMPTIME_INT) return ty_usize();
    return lo;
}

/* 一条序列的编译期长度。两头都能算出来才算数：
 *
 *   · 范围   `0..3`                     → 3
 *   · 视图   `xs[1..3]` / `xs[0..]`     → 拿得到基址的长度（*[N]T / 数组字面量）
 *                                        再减去下界
 *
 * 切片本身不带长度（就是 ptr+len），所以 `s[0..]` 这种没有静态长度——
 * 那是运行时的事。
 */
static bool seq_len(Node *s, uint64_t *out) {
    if (!s) return false;

    if (s->kind == NK_RANGE) {
        IntV lo = s->a ? ev_int(s->a) : intv(0);
        IntV hi = s->b ? ev_int(s->b) : noint();
        if (!s->b || !lo.ok || !hi.ok) return false;
        *out = hi.v > lo.v ? hi.v - lo.v : 0;
        return true;
    }
    if (s->kind != NK_SLICE) return false;

    Node *base = s->a;
    uint64_t blen = 0;
    if (ty_is_array_ptr(base->ty)) blen = base->ty->elem->len;
    else if (base->ty && base->ty->has_len) blen = base->ty->len;
    else if (base->kind == NK_UN && base->a && base->a->kind == NK_ARRAY_LIT) blen = (uint64_t)base->a->nitems;
    else if (base->kind == NK_ARRAY_LIT) blen = (uint64_t)base->nitems;
    else return false;

    IntV lo = s->b ? ev_int(s->b) : intv(0);
    if (!lo.ok || lo.v > blen) return false;
    uint64_t hi = blen;
    if (s->c) {
        IntV h = ev_int(s->c);
        if (!h.ok || h.v > blen) return false;
        hi = h.v;
    }
    *out = hi > lo.v ? hi - lo.v : 0;
    return true;
}

static Node *clone_tree(Node *n);      /* 在下面，泛型单态化用 */

/* inline for：编译期展开。
 *
 *   inline for (0..n) i { … }
 *
 * 要求：恰好一条序列、它得是有界的整数范围；一个绑定名；不能有标签和 else。
 * 展开方式和泛型单态化是同一套：循环体按迭代次数**克隆**，绑定名在每份里
 * 都是编译期常量（SK_CONST + ct），所以体内能拿它当数组长度、类型实参用——
 * `var buf: [i + 1]u8` 写得出来。克隆体存在 n->unroll 里，后端逐份发射。
 */
static Type *check_inline_for(Node *n) {
    int nseq  = (int)n->a->ival;
    int nbind = (int)n->a->fval;

    if (n->label) return BAD(n, "inline for cannot take a label");
    if (n->c)     return BAD(n, "inline for cannot take an `else` arm");
    if (nseq != 1 || nbind > 1)
        return BAD(n, "inline for takes exactly one range (and at most one binding name)");
    Node *r = n->items[0];
    if (r->kind != NK_RANGE || !r->b)
        return BAD(n, "inline for iterates a bounded integer range, like `0..n`");

    Type *lot = check_expr(r->a, NULL);
    if (ty_is_int(lot)) {
        Type *hit = check_expr(r->b, lot);
        if (!ty_is_int(hit))
            err_at(r->b->line, r->b->col, "range bound must be an integer, found `%s`", ty_str(hit));
    } else {
        err_at(r->a->line, r->a->col, "range bound must be an integer, found `%s`", ty_str(lot));
    }

    IntV lo = ev_int(r->a);
    IntV hi = ev_int(r->b);
    if (!lo.ok || !hi.ok)
        return BAD(n, "inline for range bounds must be known at compile time");

    Node *bind = nbind ? n->items[nseq + 0] : NULL;
    uint64_t count = hi.v > lo.v ? hi.v - lo.v : 0;
    for (uint64_t i = 0; i < count; i++) {
        Node *body = clone_tree(n->b);
        Scope sc;
        scope_push(&sc);
        if (bind) {
            Symbol *s = scope_add(&sc, bind->name, SK_CONST);
            if (!s) { scope_pop(&sc); return BAD(bind, "duplicate binding '%s'", bind->name); }
            s->ty      = ty_comptime_int();
            s->ct      = val_int(ty_comptime_int(), lo.v + i, false);
            s->state   = 2;
            s->ct_bind = true;
        }
        check_block(body);
        scope_pop(&sc);
        n->unroll = realloc(n->unroll, sizeof(Node *) * (size_t)(n->nunroll + 1));
        n->unroll[n->nunroll++] = body;
    }
    n->ty = ty_void();
    return ty_void();
}

static Type *check_for(Node *n, Type *expect) {
    if (n->flag) return check_inline_for(n);       /* inline for */
    int nseq  = (int)n->a->ival;
    int nbind = (int)n->a->fval;

    Label *lb = label_push(n->label, true);        /* 无标签也要有帧 */

    Type  *binds[8];
    bool   len_known[8];
    uint64_t lens[8];

    for (int i = 0; i < nseq && i < 8; i++) {
        Node *s = n->items[i];
        if (s->kind == NK_RANGE) {
            Type *lo = check_expr(s->a, NULL);
            if (!ty_is_int(lo)) {
                err_at(s->a->line, s->a->col, "range bound must be an integer, found `%s`", ty_str(lo));
                binds[i] = ty_invalid();
                continue;
            }
            if (s->b) {
                Type *hi = check_expr(s->b, lo);
                if (!ty_eq(lo, hi) && lo->kind != TY_COMPTIME_INT && hi->kind != TY_COMPTIME_INT)
                    err_at(s->b->line, s->b->col,
                           "range bounds must have the same type: `%s` vs `%s`",
                           ty_str(lo), ty_str(hi));
            }
            binds[i] = range_bind_type(s, lo);
        } else {
            Type *st = check_expr(s, NULL);
            if (st->kind == TY_SLICE) {
                binds[i] = st->elem;
            } else if (ty_is_array_ptr(st)) {
                err_at(s->line, s->col,
                       "an array is not a sequence; write `%s[0..]` to make a slice",
                       s->kind == NK_IDENT ? s->name : "xs");
                binds[i] = st->elem;
            } else if (st->kind == TY_INVALID) {
                binds[i] = ty_invalid();
            } else {
                err_at(s->line, s->col,
                       "`for` iterates slices or integer ranges, found `%s`", ty_str(st));
                binds[i] = ty_invalid();
            }
        }
        /* 长度：范围、定长数组上的视图都算得出来；切片本身不带长度 */
        lens[i] = 0;
        len_known[i] = seq_len(s, &lens[i]);
    }

    /* 编译期可算的长度必须一致 */
    for (int i = 0; i < nseq && i < 8; i++) {
        if (!len_known[i]) continue;
        for (int j = i + 1; j < nseq && j < 8; j++) {
            if (!len_known[j]) continue;
            if (lens[i] != lens[j])
                err_at(n->line, n->col,
                       "sequences must have equal length: %llu vs %llu",
                       (unsigned long long)lens[i], (unsigned long long)lens[j]);
        }
    }

    /* `lo..` 不是"无限范围"，只是"跟着别的序列走"的下标（spec §8.3）：
     * 一条有限序列都没有，就没有长度可取。 */
    if (nseq > 0) {
        bool any_finite = false;
        for (int i = 0; i < nseq; i++) {
            Node *s = n->items[i];
            if (s->kind != NK_RANGE || s->b) { any_finite = true; break; }
        }
        if (!any_finite)
            err_at(n->items[0]->line, n->items[0]->col,
                   "`..` without an upper bound needs another sequence to take the length from");
    }

    if (nbind != nseq)
        err_at(n->line, n->col, "%d sequence(s) but %d binding name(s)", nseq, nbind);

    {
        Scope sc;
        scope_push(&sc);
        for (int i = 0; i < nbind && i < 8; i++) {
            Node *b = n->items[nseq + i];
            Symbol *s = scope_add(&sc, b->name, SK_CAPTURE);
            if (!s) { err_at(b->line, b->col, "duplicate binding '%s'", b->name); continue; }
            s->ty = i < nseq ? binds[i] : ty_invalid();
            s->state = 2;
            b->sym = s;
            b->ty = s->ty;
        }
        TagSnap snapF;                     /* §170：循环配平 */
        tag_snap(&snapF);
        check_block(n->b);
        tag_loop_balance(n->b, &snapF);
        tag_restore(&snapF);
        scope_pop(&sc);
    }

    if (n->c) {
        err_at(n->c->line, n->c->col,
               "`for` does not take an `else` branch; write that code after the loop");
        n->c = NULL;
    }

    Type *et = n->c ? check_arm(n->c, expect) : NULL;
    Type *r;
    if (n->label && lb && lb->has) r = lb->ty;
    else if (et)                   r = et;
    else                           r = ty_void();
    label_pop();
    n->ty = r;
    return r;
}

static void check_pattern(Node *p, Type *s) {
    if (p->kind == NK_FIELD && !p->a && p->name) {
        if (s->kind == TY_ENUM) {
            if (!ty_field(s, p->name))
                err_at(p->line, p->col, "`%s` has no variant '%s'", ty_str(s), p->name);
            p->ty = s;
            return;
        }
        if (s->kind == TY_OPT) {
            if (strcmp(p->name, "some") && strcmp(p->name, "none"))
                err_at(p->line, p->col, "`%s` patterns are .some / .none", ty_str(s));
            p->ty = s;
            return;
        }
        if (s->kind == TY_ERR) {
            if (strcmp(p->name, "ok") && strcmp(p->name, "err"))
                err_at(p->line, p->col, "`%s` patterns are .ok / .err", ty_str(s));
            p->ty = s;
            return;
        }
        if (s->kind == TY_UNION) {
            if (!s->is_tagged)
                err_at(p->line, p->col,
                       "cannot switch on a plain union (no tag to match); use union(enum)");
            if (!ty_field(s, p->name))
                err_at(p->line, p->col, "`%s` has no member '%s'", ty_str(s), p->name);
            p->ty = s;
            return;
        }
        err_at(p->line, p->col, "cannot match `.%s` against `%s`", p->name, ty_str(s));
        return;
    }
    Type *t = check_expr(p, s);
    coerce_node(p, t, s, "switch pattern");
}

/* switch 覆盖了 subject 的哪些变体。
 *
 * 判据只有一份：穷尽性报错（check_switch）和缺返回分析（switch_ends）都问
 * 这里，两边不可能给出不同答案。want 的顺序跟着类型声明走，报错顺序才稳定。
 *
 * 整数 subject 永远不穷尽（nwant == 0）——一个 switch 不可能覆盖 2^32 个值。
 */
static int switch_coverage(Node *n, Type *s, bool *has_else, int *nwant_out,
                           const char **missing, int cap) {
    const char *want[32];
    int nwant = 0;

    if (s && (s->kind == TY_ENUM || (s->kind == TY_UNION && s->is_tagged))) {
        for (int i = 0; i < s->nfields && nwant < 32; i++)
            want[nwant++] = s->fields[i].name;
    } else if (s && s->kind == TY_OPT) { want[nwant++] = "some"; want[nwant++] = "none"; }
    else if (s && s->kind == TY_ERR)   { want[nwant++] = "ok";   want[nwant++] = "err";  }
    else if (s && s->kind == TY_BOOL)  { want[nwant++] = "true"; want[nwant++] = "false"; }

    *has_else = false;
    *nwant_out = nwant;

    bool seen[32] = { false };
    for (int i = 0; i < n->nitems; i++) {
        Node *arm = n->items[i];
        if (arm->flag) { *has_else = true; continue; }
        for (int k = 0; k < arm->nitems; k++) {
            Node *p = arm->items[k];
            const char *key = NULL;
            if (p->kind == NK_FIELD && !p->a && p->name) key = p->name;
            else if (p->kind == NK_BOOL) key = p->ival ? "true" : "false";
            if (!key) continue;
            for (int w = 0; w < nwant; w++)
                if (!strcmp(want[w], key)) seen[w] = true;
        }
    }

    int nmiss = 0;
    for (int w = 0; w < nwant && nmiss < cap; w++)
        if (!seen[w]) missing[nmiss++] = want[w];
    return nmiss;
}

/* 变体名怎么写成模式：枚举/联合体/`?T`/`!T` 都带点（`.red`、`.none`、`.ok`），
 * 只有 bool 是裸的 `true` / `false`。 */
static void switch_pattern_name(Buf *b, Type *s, const char *name) {
    if (s && s->kind != TY_BOOL) buf_putc(b, '.');
    buf_puts(b, name);
}

static int g_sw_diag[32][2];
static int g_nsw_diag;

static bool sw_diag_dup(Node *n) {
    for (int i = 0; i < g_nsw_diag; i++)
        if (g_sw_diag[i][0] == n->line && g_sw_diag[i][1] == n->col) return true;
    if (g_nsw_diag < 32) {
        g_sw_diag[g_nsw_diag][0] = n->line;
        g_sw_diag[g_nsw_diag][1] = n->col;
        g_nsw_diag++;
    }
    return false;
}

/* 漏变体就报错，不管这个 switch 有没有人用它的值。
 * （缺返回分析只关心"能不能掉出函数"，这里关心的是"switch 本身完整不完整"。）
 * 泛型实例化会把同一棵树检查好几遍，按源码位置去重。 */
static void check_switch_exhaustive(Node *n, Type *s) {
    if (!s || s->kind == TY_INVALID) return;
    bool has_else = false;
    int nwant = 0;
    const char *missing[32];
    int nmiss = switch_coverage(n, s, &has_else, &nwant, missing, 32);
    if (has_else || nwant == 0 || nmiss == 0) return;
    if (sw_diag_dup(n)) return;

    Buf list; buf_init(&list);
    for (int i = 0; i < nmiss; i++) {
        if (i) buf_puts(&list, ", ");
        buf_putc(&list, '`');
        switch_pattern_name(&list, s, missing[i]);
        buf_putc(&list, '`');
    }
    buf_putc(&list, '\0');
    err_at(n->line, n->col,
           "switch on `%s` is not exhaustive: missing %s; add an `else` arm",
           ty_str(s), list.data);
}

/* 捕获的类型：被捕获的就是"匹配到的东西"（M28）。联合体臂捕获变体载荷，
 * `?T` 的 .some 捕获载荷（.none 没有载荷，拒绝），`!T` 的 .ok 捕获载荷、
 * .err 捕获错误值；枚举 / 整数 / bool 的臂捕获主语值本身。 */
static Type *switch_cap_ty(Node *pat, Type *s) {
    if (s->kind == TY_UNION && s->is_tagged) {
        if (pat->kind != NK_FIELD || !pat->name) return NULL;
        Field *f = ty_field(s, pat->name);
        return f ? f->ty : NULL;
    }
    if (s->kind == TY_OPT) {
        if (!strcmp(pat->name, "none"))
            err_at(pat->line, pat->col, "`.none` has no payload to capture");
        return s->elem;
    }
    if (s->kind == TY_ERR)
        return !strcmp(pat->name, "err") ? ty_error() : s->elem;
    return s;
}

/* 模式的可比较键：`.x` / `true` / 整数字面量。其余形状（带载荷的模式、
 * 常量表达式）返回 false——它们不该被当成"肯定重复"来报。 */
static bool pat_key(Node *p, char *out, size_t cap) {
    if (p->kind == NK_FIELD && !p->a && p->name) {
        snprintf(out, cap, ".%s", p->name);
        return true;
    }
    if (p->kind == NK_BOOL) { snprintf(out, cap, "%s", p->ival ? "true" : "false"); return true; }
    if (p->kind == NK_INT && !p->flag) {
        snprintf(out, cap, "%llu", (unsigned long long)p->ival);
        return true;
    }
    if (p->kind == NK_CHAR) { snprintf(out, cap, "%llu", (unsigned long long)p->ival); return true; }
    return false;
}

/* 重复的臂：`.x` 写两遍、两个 `else`。
 *
 * 以前完全不管，一路漏到后端：枚举臂变成 C 的 duplicate case、`else` 变成
 * 两个 default，用户看到的是 B1001「编译器 bug」——而问题就在他写的那两行。
 * 这类错**必须**在这里报，因为它不是"多写了没影响"：先匹配到哪一条取决于
 * 后端的发射顺序，等于让程序行为变成实现细节。 */
static void check_switch_dup_arms(Node *n) {
    char keys[64][48];
    int  kline[64], kcol[64], nk = 0;
    bool seen_else = false;

    for (int i = 0; i < n->nitems; i++) {
        Node *arm = n->items[i];
        if (arm->flag) {
            if (seen_else)
                diag_code_at("E0504", arm->line, arm->col,
                             "duplicate `else` arm: a switch takes at most one");
            seen_else = true;
            continue;
        }
        for (int k = 0; k < arm->nitems; k++) {
            Node *p = arm->items[k];
            char key[48];
            if (!pat_key(p, key, sizeof key)) continue;
            bool dup = false;
            for (int j = 0; j < nk; j++)
                if (!strcmp(keys[j], key)) {
                    diag_note(NULL, kline[j], kcol[j], "already matched here");
                    diag_code_at("E0504", p->line, p->col,
                                 "duplicate switch pattern `%s`", key);
                    dup = true;
                    break;
                }
            if (!dup && nk < 64) {
                snprintf(keys[nk], sizeof keys[0], "%s", key);
                kline[nk] = p->line; kcol[nk] = p->col; nk++;
            }
        }
    }
}

static Type *check_switch(Node *n, Type *expect) {
    Label *lb = n->label ? label_push(n->label, false) : NULL;
    Type *s = check_expr(n->a, NULL);
    Type *result = NULL;

    /* §174：switch 臂与 if 臂同口径（臂快照 / 出口对账 / 逐臂 join）。
     * 此前臂间不回滚：第一臂的 free 事件泄漏进第二臂（两臂各 free
     * 一次被假报 E0509 双重释放），臂内分配也无法与臂值对账。 */
    TagSnap snapS;
    tag_snap(&snapS);
    ArmExit exAcc = { 0, TC_NONE, NULL, false };
    Symbol *chAcc[TAG_SNAP_MAX];
    FieldTag *cfAcc[TAG_SNAP_MAX];
    int nAcc = 0;
    n->tag_id = 0;
    n->tag_state = TC_NONE;

    for (int i = 0; i < n->nitems; i++) {
        Node *arm = n->items[i];
        Label *al = arm->label ? label_push(arm->label, false) : NULL;

        if (!arm->flag) {
            if (arm->nitems == 0)
                err_at(arm->line, arm->col, "switch arm needs at least one pattern");
            for (int k = 0; k < arm->nitems; k++) check_pattern(arm->items[k], s);
        }

        /* 捕获（M28）：`pattern: cap { … }`。捕获只允许单模式臂；
         * else 臂没有"匹配到的东西"，也拒绝。 */
        Type *cap_ty = NULL;
        if (arm->name) {
            if (arm->flag)
                err_at(arm->line, arm->col, "`else` arm has nothing to capture");
            /* 只有"单模式臂"才有得捕获。下面 switch_cap_ty 要读 items[0]：
             * else 臂（nitems==0）与多模式臂都先在这里挡掉，否则会读到 NULL
             * ——tests/bad/switch-cap-else.zx 曾因此段错误（模糊测试抓到的）。 */
            else if (arm->nitems != 1)
                err_at(arm->line, arm->col,
                       "a capture needs exactly one pattern; split the arm instead");
            if (!arm->flag && arm->nitems == 1) {
                cap_ty = switch_cap_ty(arm->items[0], s);
            }
            if (!arm->flag && arm->nitems == 1 &&
                (!cap_ty || cap_ty->kind == TY_INVALID))
                err_at(arm->line, arm->col,
                       "cannot capture in this arm (subject `%s`)", ty_str(s));
            /* `!void` / `?void` 的 `.ok` / `.some` 没有载荷可捕获——
             * 和 `if (x) v { … }` 同一条规则，否则后端会发 `void v = …`。 */
            else if (cap_ty && cap_ty->kind == TY_VOID) {
                err_at(arm->line, arm->col,
                       "this arm has no payload to capture (`%s` carries no value) -- "
                       "drop the capture and match the pattern alone", ty_str(s));
                cap_ty = NULL;
            }
        }

        ArmExit ex = { 0, TC_NONE, NULL, false };
        Symbol *chA[TAG_SNAP_MAX];
        FieldTag *cfA[TAG_SNAP_MAX];
        int nA = 0;
        {
            Scope sc;
            scope_push(&sc);
            if (arm->name && cap_ty) {
                Symbol *cs = scope_add(&sc, arm->name, SK_CAPTURE);
                if (cs) {
                    cs->ty = cap_ty;
                    cs->state = 2;
                    /* §174：捕获载荷继承主语 tag 账（变体名取自模式） */
                    const char *vn =
                        (arm->nitems == 1 && arm->items[0]->kind == NK_FIELD &&
                         arm->items[0]->name) ? arm->items[0]->name : NULL;
                    tag_capture_inherit(cs, n->a, vn);
                }
            }
            Type *at = check_arm(arm->a, expect);

            /* §174：臂出口对账（同 check_if）：臂内释放的 tag 必须以
             * null 收尾，臂内分配的必须由臂值传出；对完先收集字段账
             * 改动再恢复快照。 */
            Node *armval = lb ? lb->val
                         : ((arm->a && arm->a->kind == NK_BLOCK) ? arm->a->d : arm->a);
            ex = tag_arm_reconcile(armval, arm->a, snapS.state, snapS.n, snapS.total);
            nA = ft_arm_collect(&snapS, chA, cfA, TAG_SNAP_MAX);
            tag_restore(&snapS);

            scope_pop(&sc);

            if (al) { at = al->has ? al->ty : ty_void(); label_pop(); }

            if (!result) result = at;
            else         result = unify(arm, result, at);
        }

        /* 出口逐臂 join（§170 join 表的 N 臂推广）+ 字段账逐臂保守
         * 交集（某字段全部臂一致才保留，单臂改动丢弃——漏报不假报）。
         * 交集直接写回符号：chAcc 的符号就是 g_locals 链上的活符号。 */
        if (i == 0) {
            exAcc = ex;
            for (int k = 0; k < nA && k < TAG_SNAP_MAX; k++) {
                chAcc[k] = chA[k];
                cfAcc[k] = cfA[k];
            }
            nAcc = nA;
        } else {
            tag_join_arms(n, exAcc, ex);
            exAcc.tag = n->tag_id;
            exAcc.state = n->tag_state;
            exAcc.val = NULL;
            exAcc.stmt = exAcc.stmt || ex.stmt;
            for (int a2 = 0; a2 < nAcc && a2 < TAG_SNAP_MAX; a2++)
                for (int b2 = 0; b2 < nA && b2 < TAG_SNAP_MAX; b2++)
                    if (chA[b2] == chAcc[a2]) {
                        FieldTag *nt = ft_arms_intersect(cfAcc[a2], cfA[b2]);
                        chAcc[a2]->ftags = nt;
                        cfAcc[a2] = nt;
                        break;
                    }
        }
    }

    /* §173：语句臂的臂内释放——join 后终局落账（槽 + 所有局部载体）：
     * 后续解引用 E0512，再 free E0509。§189：臂内 std.move 落 MOVED。 */
    if (exAcc.stmt && n->tag_id) {
        TagSlot *t = tag_slot(n->tag_id);
        if (t && t->state == TST_LIVE) {
            if (exAcc.state == TC_MOVED) {
                t->state = TST_MOVED;
                for (Symbol *ls = g_locals; ls; ls = ls->locals_next)
                    if (ls->tag_id == n->tag_id && ls->tag_state == TC_LIVE)
                        ls->tag_state = TC_MOVED;
                if (t->carrier && t->carrier->tag_state == TC_LIVE)
                    t->carrier->tag_state = TC_MOVED;
            } else {
                t->state = TST_FREED;
                t->ever_freed = true;
                t->free_line = n->line;
                t->free_col = n->col;
                for (Symbol *ls = g_locals; ls; ls = ls->locals_next)
                    if (ls->tag_id == n->tag_id && ls->tag_state == TC_LIVE)
                        ls->tag_state = TC_FREED;
                if (t->carrier && t->carrier->tag_state == TC_LIVE)
                    t->carrier->tag_state = TC_FREED;
            }
        }
    }

    check_switch_dup_arms(n);
    check_switch_exhaustive(n, s);

    Type *r;
    if (lb) r = lb->has ? lb->ty : ty_void();
    else    r = result ? result : ty_void();
    if (lb) label_pop();
    n->ty = r;
    return r;
}

/* ---------------- 缺返回路径分析 ----------------
 *
 * 这不是类型问题，是到达性问题：非 void 函数只要**有一条路径**落到函数体的 `}`
 * 就是错的。能拦住路径的只有三样东西：
 *
 *   · `return` / `continue`        —— 不再落到下一条语句
 *   · 类型是 `noreturn` 的表达式    —— `@trap()`、noreturn 函数调用
 *   · 退不出来的循环                —— `while (true)`、`for (0..)`
 *
 * `if` / `switch` 要**每条出路**都拦住才算拦住；switch 还要求覆盖穷尽，
 * 因为"漏了一个枚举变体"在运行时就是一条掉出函数的路径。
 * 分析只用类型 + 编译期折叠结果，所以单态化出多少份函数体都适用。
 */

/* if 的条件是不是编译期常量：1 恒真 / -1 恒假 / 0 说不准。
 * 判定必须和 check_if 里的 `known` 一致——那里只检查会走到的那一支，
 * 另一支连类型都没有，拿它做分析就会误报。 */
static int if_const(Node *n) {
    if (!n || !n->a) return 0;
    if (n->a->kind == NK_BOOL) return n->a->ival ? 1 : -1;
    Value *cv = n->a->ct;
    Type  *c  = n->a->ty;
    if (!cv || cv->kind != V_BOOL) return 0;
    if (c && (c->kind == TY_OPT || c->kind == TY_ERR)) return 0;  /* 有捕获值 */
    if (!ct_pure(n->a)) return 0;
    return cv->bval ? 1 : -1;
}

/* while (true)：条件编译期恒真、又没有捕获，循环就没有正常出口 */
/* 体里有没有对 label 的收割（`label: ...;` 语句）。收割会 run_defers +
 * goto 循环末尾（gen_loop_value 建的标签作用域），是循环的**正常出口**。
 * （用户 2026-11-11：`n: while(true) { ...; n:{}; }; var i...;` 把后面
 *   活代码假报成 E0502 unreachable。）
 * 保守口径：嵌套的同名标签作用域理论上会"截住"同名收割（标签内层遮蔽
 *   外层，spec §8.5），这里仍当出口算——漏报一条死代码比把活代码判死安全。 */
static bool stmt_yields_label(Node *s, const char *label) {
    if (!s) return false;
    /* §213：label == NULL 是 break —— 只认最近的一层循环。递归时遇到
     * 内层循环必须停：那里的 break 收的是内层，不是我们在查的这层。 */
    if (!label) {
        switch (s->kind) {
        case NK_YIELD:
            return s->label == NULL;
        case NK_BLOCK:
            for (int i = 0; i < s->nitems; i++)
                if (stmt_yields_label(s->items[i], NULL)) return true;
            return false;
        case NK_IF:
            return stmt_yields_label(s->b, NULL) || stmt_yields_label(s->c, NULL);
        case NK_SWITCH:
            for (int i = 0; i < s->nitems; i++)
                if (stmt_yields_label(s->items[i]->a, NULL)) return true;
            return false;
        default:
            return false;   /* 嵌套 while/for 截住 break */
        }
    }
    switch (s->kind) {
    case NK_YIELD:
        return s->label && !strcmp(s->label, label);
    case NK_BLOCK:
        for (int i = 0; i < s->nitems; i++)
            if (stmt_yields_label(s->items[i], label)) return true;
        return false;
    case NK_IF:
        return stmt_yields_label(s->b, label) || stmt_yields_label(s->c, label);
    case NK_WHILE:
    case NK_FOR:
        return stmt_yields_label(s->b, label);
    case NK_SWITCH:
        for (int i = 0; i < s->nitems; i++)
            if (stmt_yields_label(s->items[i]->a, label)) return true;
        return false;
    default:
        return false;
    }
}

static bool while_never_exits(Node *n) {
    if (!n->a) return false;
    if (n->a->kind == NK_BOOL) {
        if (n->a->ival == 0) return false;
    } else {
        if (!ct_pure(n->a)) return false;
        Type *c = n->a->ty;
        if (c && (c->kind == TY_OPT || c->kind == TY_ERR)) return false;
        Value *cv = n->a->ct;
        if (!(cv && cv->kind == V_BOOL && cv->bval)) return false;
    }
    /* 条件恒真还不算"永不退出"：体里可以收割循环自己的标签
     * （`n: {}` / `n: v;` 会 goto 循环末尾），那是正常出口。 */
    if (stmt_yields_label(n->b, n->label) || stmt_yields_label(n->b, NULL)) return false;
    return true;
}

/* for：序列一定有限（`lo..` 的长度由别的序列给，spec §8.3），所以总能跑完 —— 
 * 循环体里没有 return 就一定会掉出去。 */

static bool arm_ends(Node *a);
static bool stmt_ends(Node *s);

/* 语句序列：一路走到能拦住的那条为止 */
static bool falls_through(Node *blk) {
    if (!blk) return false;
    for (int i = 0; i < blk->nitems; i++)
        if (stmt_ends(blk->items[i])) return false;
    return true;
}

/* switch 的臂有没有把情况列全。只有枚举 / 布尔 / ?T / !T / 带标签联合体能数清；
 * 整数按值匹配不需要穷尽（spec §10），一律当作会掉出去。 */
static bool switch_ends(Node *n) {
    for (int i = 0; i < n->nitems; i++)
        if (!arm_ends(n->items[i]->a)) return false;

    bool has_else = false;
    int nwant = 0;
    const char *missing[32];
    int nmiss = switch_coverage(n, n->a ? n->a->ty : NULL, &has_else, &nwant, missing, 32);
    if (has_else) return true;
    return nwant > 0 && nmiss == 0;
}

static bool stmt_ends(Node *s) {
    switch (s->kind) {
    case NK_RETURN:
    case NK_CONTINUE:
        return true;
    case NK_BLOCK:
        return !falls_through(s);
    case NK_IF: {
        int k = if_const(s);
        if (k == 1)  return arm_ends(s->b);
        if (k == -1) return arm_ends(s->c);
        if (!s->c)   return false;              /* 没有 else：条件不成立就掉出去 */
        return arm_ends(s->b) && arm_ends(s->c);
    }
    case NK_WHILE:  return while_never_exits(s);
    case NK_FOR:    return false;               /* 序列一定有限，循环总能结束 */
    case NK_SWITCH: return switch_ends(s);
    case NK_DEFER:  return false;               /* defer 是"离开时再做"，不改变流向 */
    case NK_ERRDEFER: return false;             /* 同上（且只在错误路径跑） */
    default:
        return s->ty && s->ty->kind == TY_NORETURN;
    }
}

static bool arm_ends(Node *a) {
    if (!a) return false;
    if (a->kind == NK_BLOCK) return !falls_through(a);
    return stmt_ends(a);
}

/* 不可达代码用的判定：比 `stmt_ends` 多认一个"收割"（`tag: expr;` 会跳出所在块），
 * 其余沿用缺返回分析的结论。标签块不能被判成"到不了后面"——它收完值就接着往下走，
 * 而 `falls_through` 不认收割，正好保守。 */
static bool stmt_blocks_next(Node *s) {
    switch (s->kind) {
    case NK_RETURN:
    case NK_CONTINUE:
    case NK_YIELD:
        return true;
    case NK_IF:
        /* 编译期常量的 if：后端会剪掉死臂，"够不着"是泛型特化里的常态
         * （`if (T 是结构体) {…} return x == y;`），不当不可达代码报。
         * 缺返回分析仍按常量剪——falls_through / stmt_ends 自己会看。 */
        if (if_const(s) != 0) return false;
        return stmt_ends(s);
    default:
        return stmt_ends(s);
    }
}

/* 一个函数可能被单态化出很多份，缺返回只在源码位置上报一次 */
static int g_ret_diag[16][2];
static int g_nret_diag;

static bool ret_diag_dup(Node *fn) {
    for (int i = 0; i < g_nret_diag; i++)
        if (g_ret_diag[i][0] == fn->line && g_ret_diag[i][1] == fn->col) return true;
    if (g_nret_diag < 16) {
        g_ret_diag[g_nret_diag][0] = fn->line;
        g_ret_diag[g_nret_diag][1] = fn->col;
        g_nret_diag++;
    }
    return false;
}

static void check_return_paths(Node *fn, Type *ret) {
    if (!fn->b || !ret) return;
    if (ret->kind == TY_VOID || ret->kind == TY_INVALID) return;
    /* `!void`：payload 是 void，成功就是"走到底、什么都不带"。它和 `void` 一样
     * 允许落到末尾，只是要多置一次 tag = 0（后端负责）。 */
    if (ret->kind == TY_ERR && ret->elem && ret->elem->kind == TY_VOID) return;
    if (!falls_through(fn->b)) return;          /* 每条路径都拦住了 */
    if (ret_diag_dup(fn)) return;

    /* 定义是 `const name = fn …`，名字挂在绑定的符号上，不在 NK_FN 上。 */
    const char *nm = fn->name ? fn->name
                   : (fn->sym && fn->sym->name ? fn->sym->name : "fn");

    if (ret->kind == TY_NORETURN) {
        err_at(fn->line, fn->col,
               "function '%s' is declared `noreturn`, but its body can reach the end", nm);
        return;
    }
    /* 入口例外：main 掉出末尾就是 `return 0`（后端本来就这么发） */
    if (!strcmp(nm, "main")) return;
    err_at(fn->line, fn->col,
           "function '%s' can reach the end of its body without returning a value of "
           "type `%s`", nm, ty_str(ret));
}

static Type *sema_construct(Node *n, Type *expect) {
    /* §130：控制流臂里的状态迁移不可证明，整段标 opaque（不做路径敏感） */
    bool ctl = (n->kind != NK_BLOCK);
    if (ctl) g_flow_depth++;
    Type *r;
    switch (n->kind) {
    case NK_BLOCK:  r = check_block(n); break;
    case NK_IF:     r = check_if(n, expect); break;
    case NK_WHILE:  r = check_while(n, expect); break;
    case NK_FOR:    r = check_for(n, expect); break;
    case NK_SWITCH: r = check_switch(n, expect); break;
    default:        r = type_of(n, ty_void()); break;
    }
    if (ctl) g_flow_depth--;
    return r;
}

/* ---------------- 强制转换（唯一的隐式规则集合 R7） ---------------- */

static Type *coerce_node(Node *e, Type *from, Type *to, const char *ctx) {
    if (!to || to->kind == TY_INVALID) return to;
    if (!from) return to;
    /* undefined 没有自己的类型：直接定型成期望类型，后端才写得对
     * （否则字段里的 `.buf = undefined` 会按 `undefined` 那个类型去生成）。 */
    if (from->kind == TY_UNDEF) {
        if (e) e->ty = to;
        return to;
    }
    if (can_coerce(from, to)) {
        /* comptime_int 落到具体整数类型：字面量必须拟合（M19）。
         * expect 传播覆盖不到的地方（结构体字段、元组元素…）从这里兜底。 */
        if (from->kind == TY_COMPTIME_INT && ty_is_int(to) && !lit_fits_expr(e, to)) {
            err_at(e ? e->line : 0, e ? e->col : 0,
                   "%s: integer literal does not fit in `%s`", ctx, ty_str(to));
            return ty_invalid();
        }
        return to;
    }
    err_at(e ? e->line : 0, e ? e->col : 0,
           "%s: expected `%s`, found `%s`", ctx, ty_str(to), ty_str(from));
    return ty_invalid();
}

/* ---------------- 入口 ---------------- */

/* §173：body 队列按调用依赖补查 —— 前向引用（main 调 alloc、alloc
 * 定义在后）时被调方 body 还在队，返回流未分析，调用点的
 * free/解引用记账拿不到载体标签。按生命周期链（调用依赖）
 * 先查被调方：扫 fn body AST 里的直接调用（ident 名字），从
 * 待查条目的作用域快照解析到队列里的 fn，先查它再查自己。
 * 互递归由队列摘除（置 fn=NULL）防死循环，互递对之间仍有一
 * 方先查，行为等同旧声明序（此时其返回流未分析，调用点不
 * 继承载体——已知限制）。补查发生在主循环层（不嵌套在
 * 表达式检查里），臂/循环账本的全局状态不会串台。 */
static Node *deps_fn_of_name(const char *name, Scope *ps) {
    for (Scope *sc = ps; sc; sc = sc->parent) {
        Symbol *s = scope_find_here(sc, name);
        if (!s || !s->decl) continue;
        Node *d = s->decl;
        if (d->kind == NK_FN) return d;
        if (d->kind == NK_DECL && d->b && d->b->kind == NK_FN) return d->b;
    }
    return NULL;
}

/* §184：跨模块成员调用（`g.read(...)`）的 callee 是 NK_FIELD，裸名
 * deps_fn_of_name 摸不到——被调体没被拓扑补查，调用点的 pderef
 * 事实账（E0512）/ pff / 返回流全部拿不到，跨模块 UAF 静默漏报。
 * 根标识符沿作用域链**按名字**解析（deps 扫描发生在体检查之前，
 * 节点上还没有 sym）；ensure 成 SK_NS 拿到 mod 后逐级进成员表：
 * 中间级可以是再导出的子模块别名（`std.fs.File.open`），末级落到
 * 函数。ensure 对 SK_FN 会 schedule_body 入队——deps_check_one
 * 才有东西可摘。解析不出来就返回 NULL，行为与修复前一致。 */
static Node *deps_fn_of_field(Node *fld, Scope *ps) {
    Node *root = fld;
    while (root->kind == NK_FIELD && root->a) root = root->a;
    if (root->kind != NK_IDENT || !root->name) return NULL;
    Symbol *s = NULL;
    for (Scope *sc = ps; sc && !s; sc = sc->parent)
        s = scope_find_here(sc, root->name);
    if (!s) return NULL;
    ensure(s);                      /* 惰性模块别名在此升级成 SK_NS */
    if (s->kind != SK_NS || !s->mod) return NULL;
    Module *m = s->mod;
    /* a 链收集成员（外→内是倒的）：a.b.c → [c, b]，按 b、c 序解析 */
    Node *chain[16];
    int nc = 0;
    Node *lvl = fld;
    for (; lvl && lvl->kind == NK_FIELD && nc < 16; lvl = lvl->a)
        chain[nc++] = lvl;
    if (lvl && lvl->kind == NK_FIELD) return NULL;   /* 链比 16 深：放弃 */
    for (int i = nc - 1; i >= 0; i--) {
        Symbol *ms = scope_find_here(&m->scope, chain[i]->name);
        if (!ms) return NULL;
        ensure(ms);                 /* 函数：解析签名并 schedule_body */
        /* 顶层 `pub const read = fn (…) {…}` 的符号 kind 是 SK_CONST
         * （不是 SK_FN），判断只能看 decl 形状——与 deps_fn_of_name
         * 同一口径。 */
        if (ms->decl && ms->decl->kind == NK_DECL && ms->decl->b &&
            ms->decl->b->kind == NK_FN)
            return ms->decl->b;
        if (ms->decl && ms->decl->kind == NK_FN)
            return ms->decl;
        if (ms->kind == SK_NS && ms->mod) { m = ms->mod; continue; }
        return NULL;
    }
    return NULL;
}

static void deps_check_calls(Node *n, Scope *ps);

static void deps_check_one(Node *fn) {
    for (int j = 0; j < g_npending; j++) {
        if (g_pending[j].fn != fn) continue;
        /* 进入即摘除：依赖链上的互递归（A 调 B、B 又调 A）会再
         * 找到自己——不先摘除就是无限递归爆栈。在途的体对
         * 更晚的调用点而言返回流未分析，行为等同旧声明序。 */
        g_pending[j].fn = NULL;
        Module *sm = g_mod;
        Scope  *ss = g_scope;
        const char *sp = g_srcpath;
        if (g_pending[j].mod) {
            g_mod = g_pending[j].mod;
            g_scope = g_pending[j].scope
                    ? g_pending[j].scope : &g_pending[j].mod->scope;
            g_srcpath = g_pending[j].mod->path;
        }
        if (fn->b) deps_check_calls(fn->b, g_scope);
        check_fn_body(fn);
        g_mod = sm; g_scope = ss; g_srcpath = sp;
        return;
    }
}

static void deps_check_calls(Node *n, Scope *ps) {
    if (!n || n->kind == NK_FN) return;  /* 嵌套函数体不算 */
    if (n->kind == NK_CALL && n->a && n->a->kind == NK_IDENT && n->a->name) {
        Node *dfn = deps_fn_of_name(n->a->name, ps);
        if (dfn) deps_check_one(dfn);
    }
    /* §184：模块成员调用（`g.read(...)`）同样进拓扑——跨模块被调体
     * 不提前查，§180 的 use-after-free 在调用点就瞎了。 */
    if (n->kind == NK_CALL && n->a && n->a->kind == NK_FIELD && n->a->name) {
        Node *dfn = deps_fn_of_field(n->a, ps);
        if (dfn) deps_check_one(dfn);
    }
    deps_check_calls(n->a, ps);
    deps_check_calls(n->b, ps);
    deps_check_calls(n->c, ps);
    deps_check_calls(n->d, ps);
    for (int j = 0; j < n->nitems; j++) deps_check_calls(n->items[j], ps);
}

void sema_check(Node *root) {
    Module *m0 = calloc(1, sizeof(Module));
    m0->path = g_srcpath;
    m0->stem = "main";
    m0->root = root;
    /* 多文件驱动（zxc check 一个进程查多个文件）会反复进这里：模块表
     * 和待查体队列必须清零重来。否则 g_mods 换了新数组而 g_nmods 还
     * 带着上一轮的计数——module_find 一访问 g_mods[0] 就是空指针。
     * std 的跨轮缓存一并作废：缓存里的 Module 不在新模块表里，它连带
     * 导入的 str/map/pool 都不会登记，后端这轮就看不到这些单元了
     * （AST 有内容哈希缓存，重载 std 的解析成本可以忽略）。 */
    g_nmods = 0;
    g_npending = 0;
    g_std_mod = NULL;
    g_ntslots = 0;              /* §170：tag 账本同样按轮清零 */
    g_tag_auto_seq = 0;         /* §173：自动槽序号同轮清零 */
    g_npderef = 0; g_npslotsym = 0;  /* §180：形参事实账按轮清零（符号指针
                                     * 跨轮悬垂，绝不能留到下一轮） */
    g_ntdefer = 0;              /* §184：延迟对账按轮清零（同上，节点指针悬垂） */
    stamp_mod(root, g_srcpath);
    g_cmods = 8;
    g_mods = calloc((size_t)g_cmods, sizeof(Module *));
    g_mods[g_nmods++] = m0;
    m0->loading = true;         /* 入口文件也在"加载中"：别人导入它就是循环 */

    g_mod = m0;
    g_scope = &m0->scope;
    m0->scope.is_module = true;
    hoist(&m0->scope, root);
    /* §158：入口单元也种同一张内建表。
     * 之前只给 @mod 模块种，LSP 单文件 --check 会把 std/net.zx
     * 当成入口，对 stackAlloc 报假的 E0401。
     * 入口也能用裸名是纯放宽（原来报错的写法现在合法）；
     * std. 前缀仍是推荐写法，显式能力没有变。 */
    seed_std_builtins(&m0->scope);
    g_root_stmts = true;
    check_stmts(root);
    g_root_stmts = false;


    /* 函数体：每个都回到它所属模块的作用域里检查。
     * 检查过程中可能又冒出新的待检查体（泛型实例、模块里的函数），
     * 所以这里边跑边长。 */
    for (int i = 0; i < g_npending; i++) {
        Pending *p = &g_pending[i];
        if (!p->fn) continue;       /* 被依赖补查掉的（§173） */
        Module *sm = g_mod;
        Scope  *ss = g_scope;
        const char *sp = g_srcpath;
        if (p->mod) {
            g_mod = p->mod;
            g_scope = p->scope ? p->scope : &p->mod->scope;
            g_srcpath = p->mod->path;
        }
        /* 先按调用依赖查掃它引用的、还在队里的被调方（§173）。
         * 自己先摘除（置 fn=NULL）：递归函数调自己，不摘就是
         * deps_check_one 无限重入爆栈。摘除/读都按索引访 g_pending：
         * deps 补查与 check_fn_body 里都会再 schedule 新体（队列
         * realloc 扩容），循环头取的 p 指针会悬垂。 */
        Node *pfn = p->fn;
        g_pending[i].fn = NULL;
        if (pfn->b) deps_check_calls(pfn->b, g_scope);
        check_fn_body(pfn);
        g_mod = sm;
        g_scope = ss;
        g_srcpath = sp;
    }

    /* 函数体全部查完：§184 延迟对账先重放（此刻所有 pderef/pff 事实
     * 已入账），再做 devirt 重试。 */
    tag_deferred_replay();

    /* 函数体全部查完：把第一次因工厂体未检查而没证明出来的调用点重试。 */
    devirt_retry_all();

    /* §170 收尾：泄漏 + 强制贴标。只在有诊断余量时扫，别把
     * 前面已经炸出来的错盖掉。 */
    if (!g_error_count) tag_ledger_final(g_srcpath);

    m0->loading = false;
    g_scope = NULL;
    root->ty = ty_void();
}

/* ======================================================================
 *  编译期求值（M4）
 *
 *  这一节建立在"已经检查过"的树上：所有 ident 都挂了 sym、所有节点都有 ty。
 *  求值只做一件事——在符号的 ct 槽位里填值，然后往上算。
 *
 *  两条保证：
 *    - 求值失败不会毁掉类型检查：g_ct_stuck 一置位，调用方就当"这个表达式
 *      没有编译期值"，退回运行时（`const` 才因此报错）。
 *    - 任何循环都有上限，编译期不会挂死。
 * ====================================================================== */



/* 编译期调用栈：检查递归函数的函数体时，不能再去折叠本体里的同一函数，
 * 否则 fib(-1) → fib(-2) → … 会把编译器栈打穿。
 * 执行阶段（ct_run）不受这个限制——那里走的是真实控制流，会正常终止。 */
static Node *g_ct_stack[128];
static int   g_ct_nstack;
static bool  g_ct_checking;

static bool ct_on_stack(Node *fn) {
    for (int i = 0; i < g_ct_nstack; i++)
        if (g_ct_stack[i] == fn) return true;
    return false;
}

/* 正在"执行"编译期代码的深度。var / 参数 / 捕获是只读运行时槽位，
 * 只有在编译期真的跑起来的时候（ct_run / ct_call 里）才能读它们的值；
 * 否则 `const k = runtimeVar + 1` 会被悄悄折叠成一个过时的数。 */

static CtFlow ct_run(Node *n);
static CtFlow ct_run_raw(Node *n);
static CtFlow ct_run_block(Node *b);
static CtFlow ct_run_arm(Node *n);
static Value  *ct_call(Node *n);

static CtFlow ct_normal(void) { CtFlow f = { CT_NORMAL, NULL, NULL }; return f; }

/* 只清"会执行语句"的子树：块 / if / while / for / switch 里可能烙着
 * 某一轮的中间值；普通表达式的折叠结果就是最终值，不能清。 */
static void ct_clear_stmts(Node *n) {
    if (!n) return;
    switch (n->kind) {
    case NK_BLOCK: case NK_IF: case NK_WHILE: case NK_FOR: case NK_SWITCH:
    case NK_DECL: case NK_RETURN: case NK_CONTINUE: case NK_YIELD:
    case NK_DEFER: case NK_ERRDEFER:
        ct_clear(n);
        return;
    default: break;
    }
    ct_clear_stmts(n->a); ct_clear_stmts(n->b);
    ct_clear_stmts(n->c); ct_clear_stmts(n->d);
    for (int i = 0; i < n->nitems; i++) ct_clear_stmts(n->items[i]);
}

/* 循环体每跑一轮都要把上一轮的折叠结果清掉，否则第二轮会直接返回旧值 */
static void ct_clear(Node *n) {
    if (!n) return;
    n->ct = NULL;
    ct_clear(n->a); ct_clear(n->b); ct_clear(n->c); ct_clear(n->d);
    for (int i = 0; i < n->nitems; i++) ct_clear(n->items[i]);
}

/* 表达式里出现的 `return`（`if (c) { return 1; }` 之类）没有返回值可给，
 * 于是挂在这里往上冒，由语句序列和函数体接手。 */

#define CT_MAX_CALLS  200000
#define CT_MAX_LOOPS  1000000
static long g_ct_calls;

/* 深拷贝：泛型实例化要一棵属于自己的树，这样里面的 `struct{…}` 才有
 * 独立身份（ty_agg 用节点当键）。 */
static Node *clone_tree(Node *n) {
    if (!n) return NULL;
    Node *c = calloc(1, sizeof(Node));
    *c = *n;
    c->ty = NULL; c->type_val = NULL; c->ct = NULL; c->sym = NULL;
    c->body_checked = false;
    /* inline for 的展开结果不随克隆走：unroll 数组是源节点私有的，
     * 浅拷贝指针 + nunroll 会让克隆体在重新展开时 realloc 挪块，
     * 源节点的 unroll 立刻悬垂（后端扫 unroll 读已释放内存 → 段错误。
     * 2026-11-11：ct 折叠函数调用克隆整个 fn 体时踩中——
     * `inline for` 体里给局部 var 赋 `@type(T).size` 就必崩）。 */
    c->unroll = NULL; c->nunroll = 0;
    c->a = clone_tree(n->a);
    c->b = clone_tree(n->b);
    c->c = clone_tree(n->c);
    c->d = clone_tree(n->d);
    if (n->nitems) {
        c->items = calloc((size_t)n->nitems, sizeof(Node *));
        for (int i = 0; i < n->nitems; i++) c->items[i] = clone_tree(n->items[i]);
    }
    return c;
}

/* ---------------- 字段 / 下标 / 切片 ---------------- */

/* 把一个编译期指针/视图摊成"底层数据 + 起始下标"。
 * 视图（切片）在 Value 上就是 V_PTR：base 指向整块数据，off 是元素下标，
 * nitems 是可见长度；字符串用 V_STR，off 是字节偏移。 */
static Value *ct_flat(Value *v, int64_t *off) {
    int64_t o = 0;
    while (v && v->kind == V_PTR && v->base) {
        o += (int64_t)v->off;
        v = v->base;
    }
    if (off) *off = o;
    return v;
}

/* 视图长度：*[N]T 看类型，切片 / 字符串看值。 */
static bool ct_view_len(Value *v, Type *t, int64_t *len) {
    if (ty_is_array_ptr(t)) { *len = (int64_t)t->elem->len; return true; }
    /* §201：[N]T 数组值的长度写在类型里，`arr[0][1..]` 编译期切片要用 */
    if (t && t->kind == TY_ARRAY) { *len = (int64_t)t->len; return true; }
    if (t && t->kind == TY_PTR && t->has_len) { *len = (int64_t)t->len; return true; }
    if (!v) return false;
    if (v->kind == V_STR) { *len = v->slen; return true; }
    if (v->kind == V_PTR && v->base) { *len = (int64_t)v->nitems; return true; }
    return false;
}

static Value *ct_field(Node *n) {
    /* .len / .size —— 编译器字段。但聚合体自己有同名字段时按字段走
     * （`struct { size: i32 }` 的 `s.size` 就是那个字段，不是字节数）——
     * 这条规则必须和 check_field / 后端 ex() 完全一致，否则
     * 常量折叠和运行期会给出两个不同的答案。 */
    Type *at = n->a ? n->a->ty : NULL;
    /* §197：裸指针也没有 `.size`/`.len` 内建——穿透一层 TY_PTR 查构件字段。
     * check_field 对 `*S` 早就是这样：elem 有同名字段就走字段，否则报
     * "has no count"。折叠这里必须同口径，否则 `p.size` 会被折成"指针
     * 自身字节数 8"，而类型检查/后端都当它是 S.size 字段（值 77）。 */
    Type *probe = at;
    if (probe && probe->kind == TY_PTR && !probe->has_len &&
        !ty_is_array_ptr(probe)) probe = probe->elem;
    bool shadowed = probe && n->name &&
                    (probe->kind == TY_STRUCT || probe->kind == TY_UNION) &&
                    ty_field(probe, n->name) != NULL;
    if (!shadowed && n->name && (!strcmp(n->name, "len") || !strcmp(n->name, "size"))) {
        bool is_len = !strcmp(n->name, "len");
        Type *t = n->a ? n->a->ty : NULL;
        if (!t) return NULL;

        /* §201：字符串字面量的 .len/.size 按字面值折，绝不当 *[N]T 内建。 */
        if (n->a && n->a->kind == NK_STR) {
            Value *sv = ct_eval(n->a);
            if (!sv || sv->kind != V_STR) return NULL;
            return val_int(ty_usize(), (uint64_t)sv->slen, false);
        }
        /* §201：[N]T 数组值：len=N，size=N×元素 */
        if (t->kind == TY_ARRAY) {
            if (is_len) return val_int(ty_usize(), t->len, false);
            return val_int(ty_usize(), (uint64_t)ty_size_relaxed(t), false);
        }
        if (t->kind == TY_TUPLE && is_len)
            return val_int(ty_usize(), (uint64_t)t->nfields, false);
        /* 向量的分量数是类型的一部分（§162） */
        if (t->kind == TY_VEC && is_len)
            return val_int(ty_usize(), t->len, false);

        /* `.size` 对"长度写在类型里"的聚合（元组 / 数组 / 向量等）直接按
         * 类型布局折——这段在 §201 改写 has_len 分支时被漏掉，tuple.size
         * 从此退化成 E0501。只有 []T 的 size 才依赖运行期描述符。 */
        if (!is_len && t->kind != TY_SLICE)
            return val_int(ty_usize(), (uint64_t)ty_size_relaxed(t), false);

        /* 数量在值里：只有 []T 的 .len/.size 是运行期描述符上的值；
         * `*[N]T` 已经是普通指针，不再在这里折成编译期常量。 */
        Value *b = ct_eval(n->a);
        if (t->kind != TY_SLICE || !b) return NULL;
        int64_t len;
        if (!ct_view_len(b, t, &len)) return NULL;
        if (is_len) return val_int(ty_usize(), (uint64_t)len, false);
        return val_int(ty_usize(),
                       (uint64_t)ty_size_relaxed(t->elem) * (uint64_t)len, false);
    }

    Value *b = ct_eval(n->a);
    if (!b) return NULL;

    /* Color.red：枚举变体 */
    if (b->kind == V_TYPE) {
        Type *t = b->tval;
        if (!t || !n->name) return NULL;
        if (t->kind == TY_ENUM) {
            Field *f = ty_field(t, n->name);
            if (f) return val_int(t, f->tag, false);
        }
        /* 体里的常量成员（`Color.LIMIT` / `Vec.ZERO`）：值就是它的初值 */
        Field *st = ty_static_member(t, n->name);
        if (st && st->is_static && !st->is_var && st->dflt) return ct_eval(st->dflt);
        return NULL;
    }

    /* §118：描述符的 .ptr 是运行期字段地址（@type(左值) 才有），折不成
     * 编译期常量——放行会被下面的切片 .ptr 分支误折成聚合视图。 */
    if (n->name && !strcmp(n->name, "ptr") &&
        n->a && n->a->ty == sema_type_field_type())
        return NULL;

    /* 切片的 .ptr：指向底层数据的当前位置 */
    if (n->name && !strcmp(n->name, "ptr")) {
        int64_t po = 0;
        Value *base = ct_flat(b, &po);
        if (base->kind != V_AGG && base->kind != V_STR) return NULL;
        return val_ptr(n->ty, base, (uint64_t)po);
    }

    int64_t o = 0;
    Value *base = ct_flat(b, &o);

    if (base->kind == V_STR) {                     /* 字符串按字节取 */
        if (n->name) return NULL;
        int64_t k = o + (int64_t)n->ival;
        if (k < 0 || k >= base->slen) return NULL;
        return val_int(ty_u8(), (uint64_t)(unsigned char)base->sval[k], false);
    }

    if (base->kind != V_AGG) return NULL;

    if (!n->name) {                                /* .N */
        int64_t k = o + (int64_t)n->ival;
        if (k >= 0 && k < base->nitems) return base->items[k];
        return NULL;
    }

    int idx = base->ty ? ty_field_index(base->ty, n->name) : -1;
    if (idx >= 0 && idx < base->nitems) return base->items[idx];
    return NULL;
}

/* 编译期写回：var 的槽位就是 Symbol.ct，聚合元素就地改 */
/* 编译期求值不许写**全局** var 的存储（!in_fn）：全局是运行时槽位，
 * 编译期写入的变异会烙进 C 初始值镜像（压测 s05 首次暴露：orelse 位置的
 * 调用被探测折叠，defer 对全局数组的写入变成全局初始值，运行时带着脏
 * 数据启动；数组再小一点还会假报编译期越界 E0300）。局部槽位照旧——
 * ct_run 的循环计数全靠它。 */
static bool ct_store_global(Node *lhs) {
    while (lhs && (lhs->kind == NK_INDEX || lhs->kind == NK_FIELD)) lhs = lhs->a;
    /* 只拒顶层 var（真全局槽位）。不能用 !in_fn：文件级 comptime 块（const T = build: { var t = ...; }）的
     * 局部槽位 in_fn 也是 false，建表全靠 ct_store 写，拒了它表就全是 0（comptime.zx / generic.zx）。 */
    return lhs && lhs->kind == NK_IDENT && lhs->sym &&
           lhs->sym->is_top && lhs->sym->kind == SK_VAR;
}

static bool ct_store(Node *lhs, Value *v) {
    if (!lhs || !v) return false;
    if (ct_store_global(lhs)) return false;
    if (lhs->kind == NK_IDENT) {
        if (!lhs->sym || lhs->sym->kind != SK_VAR) return false;
        lhs->sym->ct = v;
        return true;
    }
    if (lhs->kind == NK_FIELD || lhs->kind == NK_INDEX) {
        Value *b = ct_eval(lhs->a);
        if (!b) return false;
        int64_t o = 0;
        Value *base = ct_flat(b, &o);
        if (base->kind != V_AGG) return false;
        int64_t idx = -1;
        if (lhs->kind == NK_INDEX) {
            Value *i = ct_eval(lhs->b);
            if (!i) return false;
            idx = o + val_i64(i);
        } else if (!lhs->name) {
            idx = o + (int64_t)lhs->ival;
        } else if (base->ty) {
            int f = ty_field_index(base->ty, lhs->name);
            if (f >= 0) idx = f;
        }
        if (idx < 0 || idx >= base->nitems) return false;
        base->items[idx] = v;
        return true;
    }
    return false;
}

static Value *ct_index(Node *n) {
    Value *b = ct_eval(n->a);
    Value *i = ct_eval(n->b);
    if (!b || !i) return NULL;

    /* §201：数组指针 `*[N]T` 的下标取的是**数组本身**，不是首元素。
     * `&.{10,20,30}` 折成 V_PTR(base=V_AGG{10,20,30})；`p[0]` 必须给出
     * 整个 V_AGG，之后 `p[0][i]` 才取元素。旧代码直接 ct_flat 后取
     * items[k]（= 首元素标量），导致 `arr[0][0]` 二次下标折不动报 E0501。
     * 只有一个数组（V_PTR 的 base 就是那一个），k 必须是 0。 */
    Type *bt = n->a ? n->a->ty : NULL;
    if (ty_is_array_ptr(bt)) {
        if (val_i64(i) != 0) return NULL;    /* 越界/多数组视图交运行时 */
        int64_t o = 0;
        Value *base = ct_flat(b, &o);
        if (o != 0 || !base || base->kind != V_AGG) return NULL;
        return base;                          /* 整个 [N]T 数组值 */
    }

    int64_t o = 0;
    Value *base = ct_flat(b, &o);
    int64_t k = o + val_i64(i);

    if (base->kind == V_STR) {
        if (k < 0 || k >= base->slen) {
            if (g_ct_soft_bounds) return NULL;
            err_at(n->line, n->col,
                   "compile-time index %lld is out of bounds (len %d)",
                   (long long)k, base->slen);
            g_ct_stuck = true;
            return NULL;
        }
        return val_int(ty_u8(), (uint64_t)(unsigned char)base->sval[k], false);
    }
    if (base->kind != V_AGG) return NULL;
    if (k < 0 || k >= base->nitems) {
        if (g_ct_soft_bounds) return NULL;
        err_at(n->line, n->col,
               "compile-time index %lld is out of bounds (len %d)",
               (long long)k, base->nitems);
        g_ct_stuck = true;
        return NULL;
    }
    return base->items[k];
}

/* base[lo..hi] —— 切片在编译期就是"视图"：不复制数据，只挪起点、改长度。
 * 字符串切片直接产生一个小字符串值（"hello"[1..] 就是 "ello"）。 */
static Value *ct_slice(Node *n) {
    Type *bt = n->a ? n->a->ty : NULL;
    if (!bt) return NULL;
    Value *b = ct_eval(n->a);
    if (!b) return NULL;

    int64_t total = 0, o = 0;
    Value *base = ct_flat(b, &o);
    if (!ct_view_len(b, bt, &total)) return NULL;

    int64_t lo = 0, hi = total;
    if (n->b) {
        Value *l = ct_eval(n->b);
        if (!l) return NULL;
        lo = val_i64(l);
    }
    if (n->c) {
        Value *h = ct_eval(n->c);
        if (!h) return NULL;
        hi = val_i64(h);
    }
    if (lo < 0 || hi < lo || hi > total) {
        if (g_ct_soft_bounds) return NULL;
        err_at(n->line, n->col,
               "compile-time slice %lld..%lld is out of bounds (len %lld)",
               (long long)lo, (long long)hi, (long long)total);
        g_ct_stuck = true;
        return NULL;
    }

    if (base->kind == V_STR) {
        Value *r = val_str(base->sval + o + lo, (int)(hi - lo));
        r->ty = n->ty;
        return r;
    }
    if (base->kind == V_AGG) {
        Value *r = val_ptr(n->ty, base, (uint64_t)(o + lo));
        r->nitems = (int)(hi - lo);
        return r;
    }
    return NULL;
}

/* ---------------- 聚合字面量 ---------------- */

static Value *ct_struct_lit(Node *n) {
    /* 具名字面量 `Vec{…}` 的类型在 n->a 上；匿名字面量 `.{…}` 没有 n->a，
     * 类型就是检查时定下来的 n->ty（可能来自期望类型）。 */
    Type *t = n->a ? n->a->type_val : n->ty;
    if (!t) return NULL;

    int nf = t->nfields;
    Value *v = val_agg(t, nf);
    bool *seen = calloc((size_t)(nf ? nf : 1), sizeof(bool));

    for (int i = 0; i < n->nitems; i++) {
        Node *p = n->items[i];
        int idx = ty_field_index(t, p->name);
        if (idx < 0) { free(seen); return NULL; }
        seen[idx] = true;
        v->items[idx] = p->a ? ct_eval(p->a) : val_void();
    }
    /* 没写出来的字段用默认值。静态成员不是值的一部分——更不能拿它的初值来填：
     * `const ZERO = Vec{…}` 的初值就是一个 Vec 字面量，套进去会无限递归。 */
    for (int i = 0; i < nf; i++) {
        if (seen[i]) continue;
        if (t->fields[i].is_static) { v->items[i] = val_undef(t->fields[i].ty); continue; }
        if (t->fields[i].dflt) v->items[i] = ct_eval(t->fields[i].dflt);
        else v->items[i] = val_undef(t->fields[i].ty);
    }
    free(seen);
    return v;
}

static Value *ct_array_lit(Node *n) {
    Type *pt = n->ty;
    if (!pt) return NULL;
    /* §205：`.{…}` 值形式 → 直接是 V_AGG（数组值本身） */
    if (n->flag && pt->kind == TY_ARRAY) {
        Value *arr = val_agg(pt, n->nitems);
        for (int i = 0; i < n->nitems; i++) {
            arr->items[i] = ct_eval(n->items[i]);
            if (!arr->items[i]) return NULL;
        }
        return arr;
    }
    /* `&.{…}` 指针形式 → V_PTR(base=V_AGG) */
    if (pt->kind != TY_PTR) return NULL;
    Value *arr = val_agg(ty_ptr(pt->elem, true, true, pt->len), n->nitems);
    for (int i = 0; i < n->nitems; i++) {
        arr->items[i] = ct_eval(n->items[i]);
        if (!arr->items[i]) return NULL;
    }
    return val_ptr(pt, arr, 0);
}

/* ---------------- 函数调用 ---------------- */

static Value *ct_call(Node *n) {
    if (++g_ct_calls > CT_MAX_CALLS) { g_ct_stuck = true; return NULL; }

    Value *callee = ct_eval(n->a);
    if (!callee || callee->kind != V_FN || !callee->fn) return NULL;

    /* 正在检查这个函数自己的体，又要折叠对它的调用 → 交给运行时 */
    if (g_ct_checking && ct_on_stack(callee->fn)) return NULL;
    if (g_ct_nstack > 100) { g_ct_stuck = true; return NULL; }

    Value *args[32];
    if (n->nitems > 32) return NULL;
    for (int i = 0; i < n->nitems; i++) {
        args[i] = ct_eval(n->items[i]);
        if (!args[i]) return NULL;
    }

    /* 体的检查与求值都在函数**定义处**的模块里做：callee 可能是别的
     * 模块的函数（`a.api`），它认识的私有辅助函数只有那边有。跳过这一步
     * 会按调用方的作用域解释体——私有名字全变 unknown。 */
    Module *save_m = g_mod;
    Scope  *save_sc = g_scope;
    const char *save_sp = g_srcpath;
    if (callee->fn->sym && callee->fn->sym->mod && callee->fn->sym->mod != g_mod) {
        g_mod = callee->fn->sym->mod;
        g_scope = &g_mod->scope;
        g_srcpath = g_mod->path;
    }

    Node   *g = clone_tree(callee->fn);
    Scope   sc;
    scope_push(&sc);

    Type *ft = resolve_fn_type(g);
    if (!ft || ft->kind != TY_FN) {
        scope_pop(&sc);
        g_mod = save_m; g_scope = save_sc; g_srcpath = save_sp;
        return NULL;
    }
    /* KEEP: 实参个数不匹配就不折叠这个调用——check_call 已报 E0303，
     * 这里若照常克隆执行函数体，会只绑 min(形参,实参) 个形参，剩下的
     * 在体里全部假报 unknown identifier（2026-11-08 用户截图踩中）。
     * 函数体本身由 schedule_body 链路带着完整形参表正常检查。 */
    if (n->nitems != ft->nparams) {
        scope_pop(&sc);
        g_mod = save_m; g_scope = save_sc; g_srcpath = save_sp;
        return NULL;
    }

    for (int i = 0; i < g->nitems; i++) {
        Node *pm = g->items[i];
        if (!pm->name) continue;
        Symbol *s = scope_add(&sc, pm->name, SK_PARAM);
        if (!s) continue;
        s->ty = i < ft->nparams ? ft->params[i] : ty_invalid();
        s->ct = args[i];
        s->ct_bind = true;
        if (args[i]->kind == V_TYPE) s->type_val = args[i]->tval;
        s->state = 2;
        pm->sym = s;
    }

    Type *save_ret = g_ret;
    bool  save_defer = g_in_defer;
    g_ret = ft->ret ? ft->ret : ty_void();
    g_in_defer = false;
    g_fn_depth++;

    g_ct_stack[g_ct_nstack++] = callee->fn;
    bool save_checking = g_ct_checking;
    g_ct_checking = true;
    /* 克隆体只是借道折叠：它的局部符号不进调用方的 g_locals 链——
     * 不隔离的话，调用方收尾 check_unused 会把克隆里"没走到使用点"的
     * 局部报成 unused（调用方自己的 check_fn_body 会重新查一遍真身）。 */
    Symbol *save_ct_locals = g_locals;
    g_locals = NULL;
    /* 克隆检查是"借道路过"：体里的局部槽位还没被执行写过，折叠若在此
     * 撞上除零（比如死循环体里的 n/xi），那是槽位假象不是用户错误——
     * 软失败（折不动=运行时值）。真要执行到了，下面的 ct_run 照旧硬报。 */
    bool save_soft = g_ct_soft_div;
    bool save_bounds = g_ct_soft_bounds;
    g_ct_soft_div = true;
    g_ct_soft_bounds = true;
    check_block(g->b);
    g_ct_soft_div = save_soft;
    g_ct_soft_bounds = save_bounds;
    g_locals = save_ct_locals;
    g_ct_checking = save_checking;

    g_ct_flow = ct_normal();
    ct_clear(g->b);
    CtFlow f = ct_run_block(g->b);
    g_ct_nstack--;
    g_fn_depth--;
    g_ret = save_ret;
    g_in_defer = save_defer;
    if (g_ct_flow.kind == CT_RETURN) f = g_ct_flow;
    g_ct_flow = ct_normal();
    scope_pop(&sc);
    g_mod = save_m; g_scope = save_sc; g_srcpath = save_sp;

    if (g_ct_stuck) return NULL;
    if (f.kind == CT_RETURN) return f.val ? f.val : val_void();
    return val_void();
}

/* ---------------- 表达式 ---------------- */

/* 只换类型、不动值：`?T` / `!T` 的载荷在编译期值里就是载荷本身
 * （标签只存在于运行时表示），所以取载荷 = 改一下类型。 */
static Value *with_ty(Value *v, Type *t) {
    if (!v || !t) return v;
    Value *r = val_new(v->kind, t);
    *r = *v;
    r->ty = t;
    return r;
}

static Value *ct_eval(Node *n) {
    if (!n) return val_void();
    /* 标识符不缓存：var 的槽位随时会变，每次都得重新读 */
    if (n->ct && n->kind != NK_IDENT) return n->ct;
    /* 负缓存（A1）：检查阶段已经证明这块子树折不出编译期值，直接短路。
     * 没有它，每个祖先节点的 fold_expr 都把整条左脊柱重新走一遍——
     * 8000 项的 `a+a+…` 链要付 O(n²)。只在检查阶段读（g_ct_exec==0）：
     * 执行阶段 var 槽位是活的，同一棵树可能算得出值。 */
    if (n->ct_no && g_ct_exec == 0 && n->kind != NK_IDENT) return NULL;
    if (g_ct_stuck) return NULL;
    if (g_ct_flow.kind != CT_NORMAL) return NULL;

    Value *v = NULL;
    switch (n->kind) {
    case NK_INT:
        if (n->flag) {
            /* 超 u64 的字面量：文本在 n->str，转成 128 位绝对值。
             * 只接受已经定型到 i128/u128 的（lit_fits_expr 保证过）。 */
            if (!n->ty || n->ty->kind != TY_INT || n->ty->bits != 128) break;
            v = val_big128(n->ty, n->str ? n->str : "0", false);
            break;
        }
        v = val_int(n->ty ? n->ty : ty_comptime_int(), n->ival, false); break;
    case NK_CHAR:  v = val_int(n->ty ? n->ty : ty_u8(), n->ival, false); break;
    case NK_FLOAT: v = val_float(n->ty ? n->ty : ty_comptime_float(), n->fval); break;
    case NK_BOOL:  v = val_bool(n->ival != 0); break;
    case NK_STR:   v = val_str(n->str ? n->str : "", n->str_len); break;
    case NK_NULL:  v = val_null(n->ty); break;
    case NK_UNDEF: v = val_undef(n->ty); break;

    case NK_IDENT: {
        /* 类型名也是值：基础类型（`u32`）没有符号表条目，靠节点上的
         * type_val 认（check_expr 的值位置分支解析时填的）。 */
        if (n->ty && n->ty->kind == TY_TYPE && n->type_val) {
            v = val_type(n->type_val);
            break;
        }
        if (!n->sym) break;
        ensure(n->sym);
        /* 类型名在编译期就是一个类型值（`Color.red` 里的 Color） */
        if (n->sym->ty && n->sym->ty->kind == TY_TYPE && n->sym->type_val) {
            v = val_type(n->sym->type_val);
            break;
        }
        int k = n->sym->kind;
        /* SK_PARAM 只在**真的执行编译期代码**时才能读——运行时形参是只读槽位。
         * 但泛型实参（`cap: usize`、`T: type`）是 ct_bind：它本来就没有运行时
         * 存储，值就是编译期常量，检查阶段也必须读得到，否则体里
         * `const k = cap * 2;` 会报 R1（用户 2026-11-11 问的"怎么读取传进来的
         * 泛型值"就是这个）。 */
        /* SK_LET 与 SK_VAR 同列：let 是运行时绑定，检查期把初始化器值当
         * 编译期值用是错的（`let i = 5; tup[i]` 曾被折成 tup[5]——运行时
         * i 可以是任何值）。g_ct_exec>0 是真的在执行编译期代码，那时
         * let 槽位有真实值，照读。 */
        if ((k == SK_VAR || k == SK_LET || k == SK_PARAM || k == SK_CAPTURE) && g_ct_exec == 0 &&
            !n->sym->ct_bind) break;
        v = n->sym->ct;
        break;
    }

    case NK_UN: {
        Value *a = ct_eval(n->a);
        if (!a) break;
        if (n->op == TOK_AMP) {
            /* &x：指向编译期对象。指向视图就等于视图本身（视图本来就是地址）。 */
            if (a->kind == V_VOID) break;
            v = (a->kind == V_PTR) ? a : val_ptr(n->ty, a, 0);
            break;
        }
        v = val_un(n->op, a, n->line, n->col);
        break;
    }

    /* `a orelse b`：编译期能判断空不空就折出来，判不了就留给运行时。
     * 左边是字面量 `null` 时永远是空——这一支最常用（`const X = null orelse 42;`）。 */
    case NK_ORELSE: {
        Value *a = ct_eval(n->a);
        if (!a) break;                     /* `null orelse x` 已在类型检查处报错 */
        if (a->kind == V_NULL || a->kind == V_UNDEF) v = ct_eval(n->b);
        else v = with_ty(a, n->ty);        /* 有值：取载荷 */
        break;
    }

    /* `.?' 同理：编译期就知道是空的，不在这里"算出个值"——那等于把
     * 运行时 trap 悄悄变成编译期的 0。让它折不动，交给后端发 trap。 */
    case NK_UNWRAP: {
        Value *a = ct_eval(n->a);
        if (!a || a->kind == V_NULL || a->kind == V_UNDEF) break;
        v = with_ty(a, n->ty);
        break;
    }

    case NK_BIN: {
        if (n->op == TOK_ASSIGN || n->op == TOK_PLUSEQ || n->op == TOK_MINUSEQ ||
            n->op == TOK_STAREQ || n->op == TOK_SLASHEQ || n->op == TOK_PERCEQ ||
            n->op == TOK_AMPEQ  || n->op == TOK_PIPEEQ  || n->op == TOK_CARETEQ ||
            n->op == TOK_SHLEQ  || n->op == TOK_SHREQ) {
            Value *rhs = ct_eval(n->b);
            if (!rhs) break;
            if (n->op == TOK_ASSIGN) {
                if (!ct_store(n->a, rhs)) break;
                v = rhs;
                break;
            }
            Value *old = ct_eval(n->a);
            if (!old) break;
            int bop = n->op == TOK_PLUSEQ ? TOK_PLUS : n->op == TOK_MINUSEQ ? TOK_MINUS :
                      n->op == TOK_STAREQ ? TOK_STAR : n->op == TOK_SLASHEQ ? TOK_SLASH :
                      n->op == TOK_PERCEQ ? TOK_PERCENT : n->op == TOK_AMPEQ ? TOK_AMP :
                      n->op == TOK_PIPEEQ ? TOK_PIPE : n->op == TOK_CARETEQ ? TOK_CARET :
                      n->op == TOK_SHLEQ ? TOK_SHL : TOK_SHR;
            Value *nw = val_bin(bop, old, rhs, n->line, n->col);
            if (!nw || !ct_store(n->a, nw)) break;
            v = nw;
            break;
        }
        /* && / || 短路：右边可能本来就算不出来 */
        if (n->op == TOK_ANDAND || n->op == TOK_OROR) {
            Value *a = ct_eval(n->a);
            if (!a) break;
            bool left = val_truthy(a);
            if (n->op == TOK_ANDAND && !left) { v = val_bool(false); break; }
            if (n->op == TOK_OROR && left)   { v = val_bool(true);  break; }
            Value *b = ct_eval(n->b);
            if (!b) break;
            v = val_bool(val_truthy(b));
            break;
        }
        Value *a = ct_eval(n->a);
        Value *b = ct_eval(n->b);
        if (!a || !b) break;
        /* 视图上的指针算术：只挪 offset（按元素），形状交给类型 */
        if (a->kind == V_PTR && b->kind == V_INT && (n->op == TOK_PLUS || n->op == TOK_MINUS)) {
            int64_t d = (int64_t)b->ival;
            if (b->neg) d = -d;
            int64_t o = (int64_t)a->off + (n->op == TOK_PLUS ? d : -d);
            if (o < 0 || !a->base) break;
            v = val_ptr(n->ty, a->base, (uint64_t)o);
            break;
        }
        if (b->kind == V_PTR && a->kind == V_INT && n->op == TOK_PLUS) {
            int64_t d = (int64_t)a->ival;
            if (a->neg) d = -d;
            int64_t o = (int64_t)b->off + d;
            if (o < 0 || !b->base) break;
            v = val_ptr(n->ty, b->base, (uint64_t)o);
            break;
        }
        /* p - q：同一个底层对象上才算得出来 */
        if (a->kind == V_PTR && b->kind == V_PTR && n->op == TOK_MINUS) {
            if (a->base != b->base || a->off < b->off) break;
            v = val_int(ty_usize(), a->off - b->off, false);
            break;
        }
        v = val_bin(n->op, a, b, n->line, n->col);
        break;
    }

    case NK_CAST: {
        Value *a = ct_eval(n->b);
        if (!a) break;
        /* 浮点 → 整数：C 的越界转换是 UB，折叠阶段会给出垃圾（
         * (i32)(1.0e300) 实测 -344682216），而且与运行时的 INT_MIN 不一致。
         * 编译期值就该在这里报——与"字面量装不进目标类型"（E0302）同一处置。 */
        if (a->kind == V_FLOAT && n->ty && n->ty->kind == TY_INT &&
            !f64_fits_int(a->fval, (int)n->ty->bits, n->ty->is_signed)) {
            diag_code_at("E0302", n->line, n->col,
                         "floating-point value %.17g does not fit in `%s`",
                         a->fval, ty_str(n->ty));
            return NULL;
        }
        v = val_cast(n->ty, a);
        break;
    }

    case NK_FIELD:
        /* std.heap.page 这种"命名空间里的常量"：值就在符号上 */
        if (n->sym && n->sym->kind == SK_CONST) { ensure(n->sym); v = n->sym->ct; break; }
        /* 枚举变体：Color.green。check_field 不设
         * n->ct，这里按类型值上的变体表求值——泛型里 `info.kind == .struct_`
         * 要靠它折成常量 bool，后端才剪得动分支。 */
        if (n->a && n->a->ty && n->a->ty->kind == TY_TYPE && n->name) {
            Type *tv = n->a->type_val;
            if (tv && tv->kind == TY_ENUM) {
                Field *f = ty_field(tv, n->name);
                if (f && f->has_tag) { v = val_int(f->ty ? f->ty : tv, f->tag, false); break; }
            }
        }
        /* 类型值上的静态成员（§117 值槽的另一半）：X.K，X 是"求出来才知道
         * 是什么类型"的表达式——泛型工厂的链式实例 `Slots(P3,4).sz2` 最多。
         * ct_eval 折不动调用表达式，但 base 节点上挂着实例类型（type_val，
         * 后端发符号名也靠它），直接从成员的编译期值槽取；槽空再试 dflt
         * （克隆里的 sym 指针还活着）。类型成员没有值，留给 type_val 路径。 */
        if (!v && n->a && n->name && n->a->ty &&
            n->a->ty->kind == TY_TYPE && n->a->type_val) {
            Type *tv = n->a->type_val;
            Field *st = ty_field(tv, n->name);
            if (!st || !st->is_static) st = ty_static_member(tv, n->name);
            if (st && st->is_static && !st->is_var &&
                !(st->ty && st->ty->kind == TY_TYPE)) {
                if (st->ct) v = st->ct;
                else if (st->dflt) v = ct_eval_probe(st->dflt);
            }
        }
        if (v) break;
        v = ct_field(n);
        break;
    case NK_INDEX:  v = ct_index(n); break;
    case NK_SLICE:  v = ct_slice(n); break;

    case NK_TUPLE: {
        Value *agg = val_agg(n->ty, n->nitems);
        bool ok = true;
        for (int i = 0; i < n->nitems; i++) {
            agg->items[i] = ct_eval(n->items[i]);
            if (!agg->items[i]) ok = false;
        }
        if (ok) v = agg;
        break;
    }

    case NK_STRUCT_LIT: case NK_ANON_STRUCT: v = ct_struct_lit(n); break;
    case NK_ARRAY_LIT:  v = ct_array_lit(n);  break;

    case NK_FN:
        if (!n->flag) v = val_fn(n->ty, n);
        break;

    case NK_BUILTIN: {
        const char *nm = n->name ? n->name : "";
        if (!strcmp(nm, "type")) {
            /* ct_clear 会把 n->ct 洗掉（泛型体每次执行前都清）：
             * type_val 上留着类型，这里重建描述符。 */
            v = n->ct ? n->ct : (n->type_val ?
                type_desc_value(n->type_val,
                    (n->type_val->kind == TY_STRUCT || n->type_val->kind == TY_UNION) &&
                    n->nitems >= 1 &&
                    !(n->items[0]->ty && n->items[0]->ty->kind == TY_TYPE) &&
                    addr_ok(n->items[0]) ? n->items[0] : NULL) : NULL);
        } else if (!strcmp(nm, "error")) {
            if (!n->flag) {            /* 折叠可能重试，只报一次 */
                n->flag = true;
                if (n->nitems >= 1 && n->items[0]->kind == NK_STR)
                    err_at(n->line, n->col, "@error: %.*s",
                           n->items[0]->str_len, n->items[0]->str);
                else
                    err_at(n->line, n->col, "@error");
            }
            g_ct_stuck = true;
            return NULL;
        } else if (!strcmp(nm, "trap")) {
            /* 只有在真正执行编译期代码时才算"到达"；普通表达式里的
             * `@trap()` 是运行时操作，检查阶段不该报错，也不该有编译期值。 */
            if (!g_ct_exec) return NULL;
            if (!n->flag) {            /* 折叠可能重试，只报一次 */
                n->flag = true;
                err_at(n->line, n->col, "@trap reached at compile time");
            }
            g_ct_stuck = true;
            return NULL;
        } else if (!strcmp(nm, "panic")) {
            /* @panic 是运行时中止：只有真正执行编译期代码时才"到达"，
             * 与 @trap 同口径。普通函数体里的折叠不该把它变成 E0507——
             * 那会让 `if (可折叠条件) @panic(...)` 的运行时用例编译不过。
             * 常量初始化器折不动它，自然会报 E0501。 */
            if (!g_ct_exec) return NULL;
            if (!n->flag) {
                n->flag = true;
                err_at(n->line, n->col, "@panic reached at compile time");
            }
            g_ct_stuck = true;
            return NULL;
        }
        break;
    }

    /* 类型表达式：struct{…} / *[N]T / [T;N] / Ident —— 类型是值 */
    case NK_TYPE_IDENT: case NK_PTR_TYPE: case NK_SLICE_TYPE:
    case NK_TUPLE_TYPE: case NK_STRUCT_TYPE: case NK_UNION_TYPE:
    case NK_ENUM_TYPE:  case NK_OPT_TYPE:   case NK_ERR_TYPE:  case NK_ERR_SET:
    case NK_REPEAT_TYPE:
        if (n->type_val) v = val_type(n->type_val);
        break;

    case NK_CALL: v = ct_call(n); break;

    case NK_BLOCK: {
        CtFlow f = ct_run_block(n);
        if (f.kind == CT_YIELD && n->label && f.label && !strcmp(f.label, n->label)) v = f.val;
        else if (f.kind == CT_RETURN) v = f.val;
        else if (f.kind == CT_YIELD) g_ct_flow = f;   /* 不是给我的：往外层冒 */
        else v = val_void();
        break;
    }

    case NK_IF: {
        Value *c = ct_eval(n->a);
        if (!c) break;
        Node *br = val_truthy(c) ? n->b : n->c;
        if (!br) { v = val_void(); break; }
        if (br->kind != NK_BLOCK) { v = ct_eval(br); break; }   /* if (c) 3 else 1 */
        CtFlow f = ct_run_block(br);
        if (f.kind == CT_RETURN) { g_ct_flow = f; break; }
        if (f.kind == CT_YIELD) {
            if (br->label && f.label && !strcmp(f.label, br->label)) v = f.val;
            else g_ct_flow = f;                    /* 交给外层标签作用域 */
            break;
        }
        v = val_void();
        break;
    }

    case NK_WHILE:
    case NK_FOR:
    case NK_SWITCH: {
        CtFlow f = ct_run(n);
        if (f.kind == CT_YIELD) {
            /* 循环自己的标签收了这次收割才拿得到值，否则继续往外冒；
             * §213：`break` 脱糖成 label=NULL 的收割，最近循环无条件收。 */
            if ((!n->label && !f.label) ||
                (n->label && f.label && !strcmp(n->label, f.label))) v = f.val;
            else g_ct_flow = f;
            break;
        }
        v = f.kind == CT_RETURN ? f.val : val_void();
        break;
    }

    default:
        break;
    }

    if (g_ct_flow.kind != CT_NORMAL) return NULL;
    if (v && n->kind != NK_IDENT) { n->ct = v; }
    else if (!v) g_ct_stuck = true;
    return v;
}

/* ---------------- 语句 ---------------- */

static CtFlow ct_run_arm(Node *n) {
    if (!n) return ct_normal();
    if (n->kind == NK_BLOCK) return ct_run_block(n);
    if (n->kind == NK_DECL || n->kind == NK_RETURN || n->kind == NK_YIELD ||
        n->kind == NK_CONTINUE || n->kind == NK_DEFER ||
        n->kind == NK_ERRDEFER ||
        n->kind == NK_IF || n->kind == NK_WHILE || n->kind == NK_FOR ||
        n->kind == NK_SWITCH)
        return ct_run(n);
    ct_eval(n);
    return g_ct_flow;
}

static CtFlow ct_run_list(Node **items, int n, const char *label) {
    for (int i = 0; i < n; i++) {
        CtFlow f = ct_run(items[i]);
        if (g_ct_flow.kind != CT_NORMAL) { f = g_ct_flow; g_ct_flow = ct_normal(); }
        if (f.kind == CT_NORMAL) continue;
        if (f.kind == CT_YIELD && label && f.label && !strcmp(f.label, label)) return f;
        return f;
    }
    return ct_normal();
}

/* 走一层编译期执行：只有在里面读 var / 参数才拿得到值（见 g_ct_exec） */
static CtFlow ct_run(Node *n) {
    g_ct_exec++;
    CtFlow f = ct_run_raw(n);
    g_ct_exec--;
    return f;
}

static CtFlow ct_run_block(Node *b) {
    if (!b) return ct_normal();
    return ct_run_list(b->items, b->nitems, b->label);
}

static CtFlow ct_run_raw(Node *n) {
    if (!n) return ct_normal();
    if (g_ct_stuck) return ct_normal();

    switch (n->kind) {
    case NK_DECL: {
        if (!n->sym) return ct_normal();
        if (!n->b) { n->sym->ct = val_undef(n->sym->ty); return ct_normal(); }
        Value *v = ct_eval(n->b);
        n->sym->ct = v;
        return ct_normal();
    }

    case NK_RETURN: {
        CtFlow f = { CT_RETURN, NULL, NULL };
        f.val = n->a ? ct_eval(n->a) : val_void();
        return f;
    }

    case NK_YIELD: {
        CtFlow f = { CT_YIELD, NULL, n->label };
        f.val = n->a ? ct_eval(n->a) : val_void();
        return f;
    }

    case NK_CONTINUE: {
        CtFlow f = { CT_CONTINUE, NULL, n->label };
        return f;
    }

    case NK_BLOCK:
        return ct_run_block(n);

    case NK_IF: {
        /* 条件折不动 = 走不走得进这支都定不了，绝不能当"这条没发生"继续
         * 执行后面的语句——那会把提前 return 之后的死代码（尤其是裸
         * @compileError）当成活代码跑出来（2026-11-10 用户实测踩中）。
         * 置 stuck 让整次折叠放弃：调用留给运行时，谁也不会被误伤。 */
        Value *c = ct_eval(n->a);
        if (!c) { g_ct_stuck = true; return ct_normal(); }
        Node *br = val_truthy(c) ? n->b : n->c;
        return br ? ct_run_arm(br) : ct_normal();
    }

    case NK_WHILE: {
        for (long guard = 0; guard < CT_MAX_LOOPS; guard++) {
            ct_clear(n->a);
            Value *c = ct_eval(n->a);
            if (!c) { g_ct_stuck = true; return ct_normal(); }
            if (!val_truthy(c)) {
                if (n->c) return ct_run_arm(n->c);
                return ct_normal();
            }
            ct_clear(n->b);
            CtFlow f = ct_run_arm(n->b);
            if (f.kind == CT_CONTINUE) continue;
            if (f.kind == CT_YIELD) {
                if (n->label && f.label && !strcmp(f.label, n->label)) return f;
                return f;
            }
            if (f.kind == CT_RETURN) return f;
        }
        g_ct_stuck = true;      /* 算不出来就当它是运行时循环 */
        return ct_normal();
    }

    case NK_FOR: {
        int nseq  = (int)n->a->ival;
        int nbind = (int)n->a->fval;

        Value *seqv[8];
        int    len[8];
        if (nseq > 8) { g_ct_stuck = true; return ct_normal(); }

        for (int i = 0; i < nseq; i++) {
            Node *s = n->items[i];
            if (s->kind == NK_RANGE) {
                Value *lo = ct_eval(s->a);
                Value *hi = s->b ? ct_eval(s->b) : NULL;
                if (!lo || !hi) { g_ct_stuck = true; return ct_normal(); }
                int64_t a = val_i64(lo), z = val_i64(hi);
                Value *agg = val_agg(NULL, (int)(z > a ? z - a : 0));
                for (int64_t k = a; k < z; k++)
                    agg->items[k - a] = val_from_i64(s->a->ty, k);
                seqv[i] = agg;
                len[i] = agg->nitems;
            } else {
                Value *v = ct_eval(s);
                while (v && v->kind == V_PTR && v->base) v = v->base;
                if (!v || v->kind != V_AGG) { g_ct_stuck = true; return ct_normal(); }
                seqv[i] = v;
                len[i] = v->nitems;
            }
        }

        int total = nseq ? len[0] : 0;
        for (int i = 1; i < nseq; i++) if (len[i] < total) total = len[i];

        for (int k = 0; k < total; k++) {
            for (int i = 0; i < nbind && i < nseq; i++) {
                Node *b = n->items[nseq + i];
                if (b->sym) b->sym->ct = seqv[i]->items[k];
            }
            ct_clear(n->b);
            ct_clear(n->b);
            CtFlow f = ct_run_arm(n->b);
            if (f.kind == CT_CONTINUE) continue;
            if (f.kind == CT_YIELD) {
                if (n->label && f.label && !strcmp(f.label, n->label)) return f;
                return f;
            }
            if (f.kind == CT_RETURN) return f;
        }
        if (n->c) return ct_run_arm(n->c);
        return ct_normal();
    }

    case NK_SWITCH: {
        Value *s = ct_eval(n->a);
        if (!s) { g_ct_stuck = true; return ct_normal(); }
        for (int i = 0; i < n->nitems; i++) {
            Node *arm = n->items[i];
            bool hit = false;
            if (arm->flag) hit = true;
            for (int k = 0; k < arm->nitems && !hit; k++) {
                Node *p = arm->items[k];
                if (p->kind == NK_FIELD && !p->a && p->name) {
                    /* .some / .none / .ok / .err：只有 null 能用值比 */
                    if (s->kind == V_NULL && !strcmp(p->name, "none")) hit = true;
                    continue;
                }
                Value *pv = ct_eval(p);
                if (pv && val_eq(pv, s)) hit = true;
            }
            if (!hit) continue;
            CtFlow f = ct_run_arm(arm->a);
            if (f.kind == CT_YIELD) return f;
            return f;
        }
        return ct_normal();
    }

    default:
        (void)ct_eval(n);        /* 表达式语句：只为填 ct */
        return ct_normal();
    }
}

/* ---------------- 折叠入口 ---------------- */

static Value *fold(Node *n) {
    if (!n) return NULL;
    if (n->ct) return n->ct;

    /* 上一次折叠失败留下的 stuck 不能粘住：每次都从头试。
     * g_ct_flow 也一样——它是"正在执行的函数里发生了 return"，对
     * 一次独立的常量折叠没有意义。 */
    bool save = g_ct_stuck;
    CtFlow save_flow = g_ct_flow;
    g_ct_stuck = false;
    g_ct_flow = ct_normal();

    Value *v = ct_eval(n);

    /* 负缓存（A1）：这次是干净的上下文（stuck / flow 已存起再清空），
     * 折不出值就是这块子树的真性质——记下来，祖先别再重走。 */
    if (!v && g_ct_exec == 0 && n->kind != NK_IDENT) n->ct_no = true;
    if (v) { n->ct = v; }
    /* 求值失败没关系：这个表达式就"只能运行时算" */
    g_ct_stuck = save;
    g_ct_flow = save_flow;
    return v;
}

/* ct_eval_probe 的实现：见前面的注释。不写负缓存、不烙 ct，
 * 调用方拿到的就只是一个"能折/不能折"的答案。 */
static Value *ct_eval_probe(Node *n) {
    if (!n) return NULL;
    bool save_stuck = g_ct_stuck;
    CtFlow save_flow = g_ct_flow;
    Value *v = ct_eval(n);
    g_ct_stuck = save_stuck;
    g_ct_flow = save_flow;
    return v;
}

/* 检查期折叠：只折"值"，绝不执行语句。
 *
 * 语句（循环 / 块 / if / switch）里有运行时副作用——读真实 var、调真实
 * 函数。检查期把它们编译期执行，会把中间值烙进真实节点：spin 回归里
 * `while (true)` 被跑到迭代上限，`n > 100` 的最后一次求值（n=101，true）
 * 被缓存到真实条件节点上，后端就把 if 剪成了裸 return。
 *
 * const 初始化器的编译期求值走 ensure → fold，块值在那里合法地执行，
 * 不经过这里，不受影响。 */
static Value *fold_expr(Node *n) {
    switch (n->kind) {
    case NK_BLOCK: case NK_IF: case NK_WHILE: case NK_FOR:
    case NK_SWITCH: case NK_DECL: case NK_RETURN: case NK_CONTINUE:
    case NK_YIELD: case NK_DEFER:
        return NULL;
    default:
        return fold(n);
    }
}

static Type *check_expr(Node *n, Type *expect) {
    Type *t = check_expr_raw(n, expect);
    if (n && t && t->kind != TY_INVALID && !n->ct) n->ct = fold_expr(n);
    return t;
}


/* ======================================================================
 *  泛型实例化（单态化）
 *
 *  泛型不是"类型构造语法"，就是普通的编译期函数：`List(i32)` 是一次编译期
 *  调用，函数体返回一个类型。
 *
 *  实现上只做一件事——**克隆**。每个实例拿到自己那棵树，于是里面的
 *  `struct{…}` 有独立身份（ty_agg 用节点当键），`List(i32)` 和 `List(u8)`
 *  天然是两个不同类型，不需要任何"类型替换表"。
 *
 *  顺序很重要：先绑定参数，再解析签名。因为签名里就写着 `*List(T)`——
 *  不知道 T 是什么，签名解析不出来。
 * ====================================================================== */

static bool is_type_param(Node *pm) {
    return pm->a && pm->a->kind == NK_TYPE_IDENT && pm->a->name &&
           !strcmp(pm->a->name, "type");
}

static bool fn_is_generic(Node *fn) {
    if (!fn || fn->kind != NK_FN) return false;
    for (int i = 0; i < fn->nitems; i++)
        if (is_type_param(fn->items[i])) return true;
    /* 返回 `type` 的类型构造函数：参数全是编译期值，所以即使一个 `T: type`
     * 都没有（`fn (N: u64) type`），它也是泛型。少了这一条，`N` 不会被绑定，
     * 体内 `*[N]u64` 只能报「长度不是编译期常量」。 */
    return fn->a && fn->a->kind == NK_TYPE_IDENT && fn->a->name &&
           !strcmp(fn->a->name, "type");
}

/* 返回 `type` 的函数是类型构造函数：它的全部实参都在编译期求值 */
static bool node_returns_type(Node *fn) {
    if (!fn || fn->kind != NK_FN || !fn->a) return false;
    return fn->a->kind == NK_TYPE_IDENT && fn->a->name && !strcmp(fn->a->name, "type");
}

/* 实参的编译期值：类型实参变成 V_TYPE，其余取折叠结果 */
static Value *arg_ct(Node *a, Type *want) {
    /* 类型位置上的实参会解析成类型表达式：`List(T, cap)` 里的 `cap` 是个
     * usize 参数，不是类型。这种"以类型语法写出来的编译期值"要按值取。 */
    if (a->kind == NK_TYPE_IDENT && !base_type(a->name)) {
        Symbol *s = lookup(a->name);
        if (s) {
            ensure(s);
            if (s->ty && s->ty->kind != TY_TYPE && s->ct) return s->ct;
        }
    }
    Type *t = check_expr(a, want);
    if (t && t->kind == TY_TYPE) return val_type(a->type_val ? a->type_val : ty_invalid());
    return a->ct;
}

static Node *resolve_generic_callee(Node *callee, Node *at) {
    if (callee->kind == NK_FN) return callee;
    if (callee->kind == NK_FIELD) {
        Symbol *m = ns_member_symbol(callee);
        if (!m) { BAD(at, "generic callee must be a function name"); return NULL; }
        ensure(m);
        callee->sym = m;
        if (m->kind == SK_FN && m->decl) return m->decl;
        if (m->ct && m->ct->kind == V_FN && m->ct->fn) return m->ct->fn;
        BAD(at, "'%s' is not a generic function", callee->name ? callee->name : "?");
        return NULL;
    }
    if (callee->kind == NK_IDENT || callee->kind == NK_TYPE_IDENT) {
        Symbol *s = lookup(callee->name);
        if (!s) { BAD(at, "unknown generic '%s'", callee->name); return NULL; }
        ensure(s);
        if (s->kind == SK_FN && s->decl) return s->decl;
        if (s->ct && s->ct->kind == V_FN && s->ct->fn) return s->ct->fn;
        BAD(at, "'%s' is not a generic function", callee->name);
        return NULL;
    }
    BAD(at, "generic callee must be a function name");
    return NULL;
}

/* 克隆 + 绑定 + 解析签名 + 检查体；类型构造函数还会跑一遍体拿到返回类型。 */
static Node *specialize_raw(Node *fn, Node *call, int *ncomp, Type **out_ft, Value **out_val) {
    bool ret_type = node_returns_type(fn);

    /* 1. 语法上先判断哪些实参是泛型实参：类型参数，或者类型构造函数的全部实参 */
    Value *args[16];
    int nbind = 0;
    for (int i = 0; i < fn->nitems && i < call->nitems && i < 16; i++) {
        Node *pm = fn->items[i];
        if (!is_type_param(pm) && !ret_type) break;
        Value *v = arg_ct(call->items[i], NULL);
        if (!v) {
            err_at(call->items[i]->line, call->items[i]->col,
                   "generic argument %d must be known at compile time", i + 1);
            return NULL;
        }
        if (is_type_param(pm) && v->kind != V_TYPE) {
            err_at(call->items[i]->line, call->items[i]->col,
                   "parameter '%s' is a `type`; a type is required, not a value",
                   pm->name ? pm->name : "?");
            return NULL;
        }
        args[nbind++] = v;
    }
    if (nbind < 1) {
        err_at(call->line, call->col, "this function needs generic arguments");
        return NULL;
    }

    /* 2. 同一个实例必须共享同一个类型（类型身份就是指针） */
    for (int k = 0; k < g_ninst; k++) {
        if (g_insts[k].fn != fn || g_insts[k].nargs != nbind) continue;
        bool same = true;
        for (int i = 0; i < nbind; i++)
            if (!val_eq(g_insts[k].args[i], args[i])) { same = false; break; }
        if (!same) continue;
        if (ncomp)  *ncomp  = nbind;
        if (out_ft) *out_ft = g_insts[k].ty;
        if (out_val) *out_val = g_insts[k].val;
        return g_insts[k].clone;
    }

    /* 3. 克隆，先绑定参数（签名里会用到 T / n） */
    /* §198：只在类型构造函数（ret_type）的实例化期间做身份锚定。收集编译期
     * 参数名，克隆后同步遍历模板与克隆体，把不依赖参数的类型声明归一到模板
     * 节点，避免"不依赖参数的嵌套 struct"在每个实例变成不同匿名类型。 */
    const char **save_params = g_ident_params;
    int save_np = g_nident_params;
    const char **new_params = NULL;
    if (ret_type && nbind > 0) {
        new_params = calloc((size_t)nbind, sizeof(const char *));
        for (int i = 0; i < nbind; i++)
            new_params[i] = fn->items[i]->name;
        g_ident_params = new_params;
        g_nident_params = nbind;
    }
    Node *g = clone_tree(fn);
    if (new_params) {
        anchor_types(fn, g);
        g_ident_params = save_params;
        g_nident_params = save_np;
        free(new_params);
    }
    g->ival = (uint64_t)nbind;      /* 前 nbind 个参数在编译期就吃掉了 */

    /* 泛型体在定义它的模块里检查：体内的模块级名字（`str.eq`、私有的
     * 辅助函数）必须和源文件里一样可见。否则通用容器只能引用编译器内建，
     * 想调一个模块里的普通函数就报 unknown identifier。 */
    Module *sm = g_mod;
    Scope  *ss = g_scope;
    const char *sp = g_srcpath;
    if (fn->sym && fn->sym->mod && fn->sym->mod != g_mod) {
        g_mod = fn->sym->mod;
        g_scope = &g_mod->scope;
        g_srcpath = g_mod->path;
    }

    Scope sc;
    scope_push(&sc);
    for (int i = 0; i < nbind; i++) {
        Node *pm = g->items[i];
        if (!pm->name) continue;
        Symbol *s = scope_add(&sc, pm->name, SK_PARAM);
        if (!s) continue;
        s->ty = is_type_param(pm) ? ty_type()
                                  : (pm->a ? resolve_type(pm->a) : ty_invalid());
        s->ct = args[i];
        s->ct_bind = true;
        if (args[i]->kind == V_TYPE) s->type_val = args[i]->tval;
        s->state = 2;
        pm->sym = s;
    }

    Type *gft = resolve_fn_type(g);
    if (!gft || gft->kind != TY_FN) {
        scope_pop(&sc);
        g_mod = sm; g_scope = ss; g_srcpath = sp;
        return NULL;
    }

    /* 剩下的运行时参数：签名已经具体了，照签名建符号 */
    for (int i = nbind; i < g->nitems; i++) {
        Node *pm = g->items[i];
        if (!pm->name) continue;
        Symbol *s = scope_add(&sc, pm->name, pm->flag ? SK_VAR : SK_PARAM);
        if (!s) continue;
        s->ty = i < gft->nparams ? gft->params[i] : ty_invalid();
        s->state = 2;
        pm->sym = s;
    }

    Type *save_ret = g_ret;
    g_ret = gft->ret ? gft->ret : ty_void();
    g_fn_depth++;
    check_block(g->b);
    g_fn_depth--;
    check_return_paths(g, g_ret);
    g_ret = save_ret;

    Value *retv = NULL;
    if (ret_type) {
        g_ct_flow = ct_normal();
        ct_clear(g->b);
        CtFlow f = ct_run_block(g->b);
        if (g_ct_flow.kind == CT_RETURN) f = g_ct_flow;
        g_ct_flow = ct_normal();
        if (!g_ct_stuck && f.kind == CT_RETURN) retv = f.val;
    }
    scope_pop(&sc);
    g_mod = sm; g_scope = ss; g_srcpath = sp;

    if (g_ninst == g_cinst) {
        g_cinst = g_cinst ? g_cinst * 2 : 32;
        g_insts = realloc(g_insts, sizeof(Inst) * (size_t)g_cinst);
    }
    Inst *e = &g_insts[g_ninst++];
    e->fn = fn;
    e->nargs = nbind;
    e->args = calloc((size_t)nbind, sizeof(Value *));
    for (int i = 0; i < nbind; i++) e->args[i] = args[i];
    e->clone = g;
    e->ty = gft;
    e->val = retv;

    if (ncomp)  *ncomp  = nbind;
    if (out_ft) *out_ft = gft;
    if (out_val) *out_val = retv;
    return g;
}

/* 实例化嵌套层数。正常写法（`Pair(Pair(i32))`）只有两三层；层层加深说明
 * 泛型体里又写了一遍自己（`fn push(self: *List(T, cap), …)`），而那是在同一个
 * 实例还没建好时又请求它。 */
static Node *specialize(Node *fn, Node *call, int *ncomp, Type **out_ft, Value **out_val) {
    if (g_spec_depth >= 16) {
        err_at(call->line, call->col,
               "generic instantiation nests too deeply: `%s` asks for itself while it is "
               "still being defined; write `Self` for the type inside its own body",
               call->a && call->a->name ? call->a->name : "?");
        return NULL;
    }
    g_spec_depth++;
    Node *g = specialize_raw(fn, call, ncomp, out_ft, out_val);
    g_spec_depth--;
    return g;
}

/* 泛型类型构造：List(i32)
 *
 * 名字从构造式来，不从"谁给它起了别名"来——同一个实例在好几处被 `const`
 * 起名时，叫哪个名字取决于检查顺序，那就不是名字而是巧合了。 */
static void name_type_from_call(Node *call, Type *result) {
    if (!result || result->name) return;
    Buf b;
    buf_init(&b);
    /* `std.ArrayList(i32)` 的被调是点路径（NK_FIELD）：以前只认裸名字，
     * 实例名就退化成 `fn(i32)`——诊断和 @type(T).name 里都看得见。 */
    if ((call->a->kind == NK_IDENT || call->a->kind == NK_TYPE_IDENT ||
         call->a->kind == NK_FIELD) && call->a->name)
        buf_puts(&b, call->a->name);
    else buf_puts(&b, "fn");
    buf_puts(&b, "(");
    for (int i = 0; i < call->nitems; i++) {
        if (i) buf_puts(&b, ", ");
        if (call->items[i]->ty && call->items[i]->ty->kind == TY_TYPE && call->items[i]->type_val)
            buf_puts(&b, ty_str(call->items[i]->type_val));
        else {
            Value *cv = call->items[i]->ct;
            if (!cv && call->items[i]->kind == NK_TYPE_IDENT) {
                Symbol *s = lookup(call->items[i]->name);      /* 以类型语法写出来的值参数 */
                if (s) cv = s->ct;
            }
            if (cv) val_dump(cv, &b);
            else buf_puts(&b, "?");
        }
    }
    buf_puts(&b, ")");
    buf_putc(&b, '\0');
    result->name = b.data;        /* 不释放：类型名和类型同寿命 */
}

/* 泛型类型构造：List(i32) */
static Type *instantiate_type(Node *call) {
    Node *fn = resolve_generic_callee(call->a, call);
    if (!fn) return ty_invalid();
    if (!fn_is_generic(fn)) {
        err_at(call->line, call->col, "`%s` is not generic",
               call->a->kind == NK_IDENT ? call->a->name : "?");
        return ty_invalid();
    }

    bool save = g_ct_stuck;
    g_ct_stuck = false;

    int nbind = 0;
    Type *ft = NULL;
    Value *retv = NULL;
    Node *g = specialize(fn, call, &nbind, &ft, &retv);
    Type *result = (retv && retv->kind == V_TYPE) ? retv->tval : NULL;
    g_ct_stuck = save;

    if (!result) {
        if (g) err_at(call->line, call->col, "generic function did not return a type");
        return ty_invalid();
    }

    /* 给实例取个可读名字：List(i32) */
    name_type_from_call(call, result);
    return result;
}

/* 泛型函数调用：push(i32, &l, 11) —— 只做签名特化，体留给后端生成 */
static Type *instantiate_fn_type(Node *call) {
    Node *fn = resolve_generic_callee(call->a, call);
    if (!fn) return ty_invalid();
    int ncomp = 0;
    Type *ft = NULL;
    bool save = g_ct_stuck;
    g_ct_stuck = false;
    Node *g = specialize(fn, call, &ncomp, &ft, NULL);
    g_ct_stuck = save;
    if (!g) return ty_invalid();
    call->inst = g;
    call->ival = (uint64_t)ncomp;     /* 前 ncomp 个实参在编译期就吃掉了 */
    return ft ? ft : ty_void();
}
