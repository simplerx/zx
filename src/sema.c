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
        "file", "io", "Allocator", "TypeKind", "TypeInfo", "FieldInfo",
        "stackAlloc", "heapAlloc", "process", NULL };
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
    bool        fn_local;  /* hoist 时确认是函数块内的绑定：块内先声明后使用要报 */
    bool        used;      /* 被读过或写过一次；没碰过的局部绑定要报（M66） */
    struct Symbol *locals_next;  /* 本函数体的局部登记链（见 local_track） */
    bool        stack_val; /* 它的值指向当前栈帧（逃逸检查用，见 addr_frame） */
    const char *cname;     /* 后端用的 C 名字（模块内名字带前缀） */
    int         decl_line; /* 声明位置（D2 note 用；0 = 没记） */
    int         decl_col;
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
bool sema_sym_has_storage(Symbol *s) { return !(s && s->ct_bind); }
Value *sema_sym_ct(Symbol *s) { return s ? s->ct : NULL; }

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
 * 拼错就完全静默）。要求写成 `_x` 是把意图显式化：`_` 开头就是"我知道
 * 它没被用"。 */
static void check_unused(Symbol *head, int errs_before) {
    /* å½æ°ä½éå·²ç»æ¥è¿éå°±ä¸åé®"ææ²¡ææ²¡ç¨è¿çåé"ï¼
     * éè¯¯æ¢å¤ä¸è¿æ¡å¤æ­å¤åæ¯åè±¡ï¼è¡¨è¾¾å¼åºéâ
     * æ£æ¥æåæ¶å°¾âåé¢çä½¿ç¨æ²¡è¢«è®°ä¸ï¼ï¼
     * èä¸è¿ç§åªé³ä¼çä½çæ­£çæ ¹å ãä¿®å®ééç¼ï¼å®èªç¶ä¼åºç°ã */
    if (g_error_count != errs_before) return;
    for (Symbol *s = head; s; s = s->locals_next) {
        if (s->used || !s->name || s->name[0] == '_') continue;
        /* `self` 是方法接收者的惯用名：不用它的方法也得写出来（位置决定签名），
         * 叫人改成 `_self` 只会让方法看起来像函数。 */
        if (!strcmp(s->name, "self")) continue;
        if (s->kind == SK_FN || s->kind == SK_NS) continue;
        const char *what = s->kind == SK_PARAM   ? "parameter"
                         : s->kind == SK_CAPTURE ? "binding"
                         :                         "variable";
        diag_help("if you don't need it, write `_%s` or `_`", s->name);
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
} Label;

#define MAX_LABEL 256
static Label g_labels[MAX_LABEL];
static int   g_nlabels;

static Label *label_push(const char *name, bool is_loop) {
    if (g_nlabels >= MAX_LABEL) return NULL;
    Label *l = &g_labels[g_nlabels++];
    l->name = name; l->ty = NULL; l->has = false; l->is_loop = is_loop; l->is_fn = false;
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

/* 待检查函数体：先建签名、后查体，自递归/互递归天然合法。
 *
 * 还要带上"当时的作用域"：泛型类型体里的方法要晚点才查，而 T / cap 这些
 * 泛型参数只活在 specialize() 那个栈上的作用域里。Scope 很小、符号在堆上，
 * 所以照抄一份链结构就够了。 */
typedef struct { Node *fn; Module *mod; struct Scope *scope; } Pending;
static Pending *g_pending;
static int g_npending, g_cappending;

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

static Type *base_type(const char *n) {
    if (!n) return NULL;
    if (!strcmp(n, "void"))      return ty_void();
    if (!strcmp(n, "bool"))      return ty_bool();
    if (!strcmp(n, "type"))      return ty_type();
    if (!strcmp(n, "error"))     return ty_error();
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
    return NULL;
}

enum { BI_NONE, BI_PRINT, BI_PRINTLN, BI_FORMAT, BI_HEAP, BI_FILE, BI_FILE_FN,
       BI_IO, BI_IO_FN, BI_IO_PRINT, BI_STACKALLOC, BI_HEAPALLOC, BI_PROC, BI_PROC_FN,
       BI_DEBUG, BI_DEBUG_FN, BI_TASK, BI_TASK_FN, BI_SYNC, BI_SYNC_FN };

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




/* std 下由编译器提供的名字。它们都挂在 std 模块的作用域里，于是
 * `std.println` / `std.file.read` 走的是同一条"模块成员"查找路径。 */
static Symbol *std_builtin(const char *name) {
    static Symbol print_s, println_s, format_s, heap_s, page_s, file_s, alloc_s;
    static Symbol salloc_s, halloc_s;
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

        /* 统一的分配 API（decisions §44）：结果都是数组指针 `*[N]T`。
         * 栈那份由后端发 alloca；堆那份显式拿分配器，OOM 直接 abort。 */
        salloc_s.name = "stackAlloc"; salloc_s.kind = SK_NS;
        salloc_s.builtin = BI_STACKALLOC; salloc_s.state = 2;
        halloc_s.name = "heapAlloc"; halloc_s.kind = SK_NS;
        halloc_s.builtin = BI_HEAPALLOC; halloc_s.state = 2;

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

        /* @typeInfo 用到的两个类型也挂在 std 上，于是 `std.TypeKind.struct_`
         * 和 `var i: std.TypeInfo` 都写得出来 */
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

    if (!strcmp(name, "stackAlloc")) return &salloc_s;
    if (!strcmp(name, "heapAlloc"))  return &halloc_s;
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
    "TypeKind", "TypeInfo", "FieldInfo", "stackAlloc", "heapAlloc", "process",
    "debug", "atomic", NULL
};

static Symbol *ns_member(Symbol *ns, const char *name, int line, int col) {
    /* 模块本身没加载成功（打不开 / 解析失败）：那条错已经报在 module(...)
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
        if (!n->name) return noint();
        if (!strcmp(n->name, "sizeOf") || !strcmp(n->name, "alignOf")) {
            if (n->nitems < 1) return noint();
            Type *t = resolve_type(n->items[0]);
            int sz = !strcmp(n->name, "sizeOf") ? ty_size(t) : ty_align(t);
            return sz < 0 ? noint() : intv((uint64_t)sz);
        }
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

    /* `null` 的落点：`?T`（空）、裸指针/函数指针（C 的 NULL）、`bool`
     *（`p == false` 那种），以及 **`error`**——此时它就是保留码 65535，
     * 和 `error.null` 是同一个值（用户 2026-11-09 第 15 条：
     * "null 也是一种错误，或者可以接收用 null 给 error 赋值"）。 */
    if (from->kind == TY_NULL)
        return to->kind == TY_OPT || to->kind == TY_PTR ||
               to->kind == TY_FN || to->kind == TY_BOOL ||
               to->kind == TY_ERROR;

    /* 目标是 ?T / !T：先拆包装，再看载荷（R7.4：T → ?T、T → !T） */
    if (to->kind == TY_OPT) return can_coerce(from, to->elem);
    if (to->kind == TY_ERR)
        return from->kind == TY_ERROR || can_coerce(from, to->elem);

    if (from->kind == TY_COMPTIME_INT)  return ty_is_int(to) || ty_is_float(to);
    if (from->kind == TY_COMPTIME_FLOAT) return ty_is_float(to);

    if (from->kind == TY_PTR && to->kind == TY_PTR) {
        if (from->has_len && to->has_len && from->len != to->len) return false;
        if (!from->has_len && to->has_len) return false;    /* 反向要显式断言 */
        if (!ty_eq(from->elem, to->elem)) return false;
        if (from->is_const && !to->is_const) return false;
        return true;
    }

    if (from->kind == TY_PTR && to->kind == TY_SLICE) {
        if (!from->has_len) return false;
        if (!ty_eq(from->elem, to->elem)) return false;
        if (from->is_const && !to->is_const) return false;
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
        n->sym = s;   /* 点路径（std.TypeInfo）的成员查找靠它进命名空间分支 */
        if (s->ty && s->ty->kind == TY_TYPE) {
            n->ty = ty_type();
            n->type_val = s->type_val ? s->type_val : ty_invalid();
            return n->type_val;
        }
            return BAD(n, "'%s' is not a type", n->name);
    }

    case NK_PTR_TYPE: {
        Type *elem = resolve_type(n->a);
        bool has_len = n->b != NULL;
        uint64_t len = 0;
        if (has_len) {
            IntV v = ev_int(n->b);
            if (!v.ok || v.neg) { BAD(n->b, "array length must be a non-negative compile-time integer"); has_len = false; }
            else len = v.v;
        }
        return ty_ptr(elem, n->flag, has_len, len);
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

    case NK_STRUCT_TYPE: {
        Type *t = ty_agg(TY_STRUCT, n, NULL);
        return resolve_struct_body(n, t, false);
    }

    case NK_UNION_TYPE: {
        Type *t = ty_agg(TY_UNION, n, NULL);
        return resolve_struct_body(n, t, true);
    }

    case NK_ENUM_TYPE: {
        Type *t = ty_agg(TY_ENUM, n, NULL);
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

        /* 第一参数叫 `self` 就是方法（必须是 Self / *Self / *const Self）；
         * 不叫 self 就是挂在类型上的普通函数（`Ints.init(alloc)`）。 */
        Node *p0 = it->nitems > 0 ? it->items[0] : NULL;
        bool has_self = p0 && p0->name && !strcmp(p0->name, "self");

        if (!has_self) {
            if (ty_method(agg, it->name)) continue;
            Method *m0 = &agg->methods[agg->nmethods++];
            m0->name = it->name;
            m0->fn   = it;
            m0->fnty = ft;
            m0->has_self = false;
            schedule_body(it);
            continue;
        }

        Type *r0 = ft->params[0];
        Type *base = (r0->kind == TY_PTR && !r0->has_len) ? r0->elem : r0;
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
    case TY_PTR:
        /* 指针就是一个机器字：指向什么都不影响**这个**结构体的布局。
         * 定长数组（*[N]T）例外——它作为 extern 字段要展开成 C 数组，
         * 元素类型必须同样 C 兼容。 */
        if (t->has_len) return check_c_able(at, t->elem, fname);
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
    if (agg->fields) return agg;          /* 已解析 */

    /* extern struct / extern union（FFI）：镜像外部 C 结构体的布局。
     * 只允许"数据"——方法、静态成员、字段默认值都不进布局，一律拒绝。 */
    bool ext = n->is_extern;
    if (ext) {
        if (n->flag)
            err_at(n->line, n->col,
                   "extern struct does not support (packed); C structs follow "
                   "the platform's natural layout");
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
            else { s->decl = it; it->sym = s; }
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
    }

    /* 方法：函数体里的第一个参数就是接收者 */
    collect_methods(n, agg, is_union ? "union" : "struct");

    scope_pop(&sc);
    return agg;
}

static Type *resolve_enum_body(Node *n, Type *e) {
    if (e->fields) return e;
    e->elem = n->a ? resolve_type(n->a) : ty_int(32, false);   /* 底层，默认 u32 */

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
            else { s->decl = it; it->sym = s; }
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
        ensure(it->sym);
        f->ty = (it->sym && it->sym->ty) ? it->sym->ty : ty_invalid();
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
    if (a->kind == TY_PTR && b->kind == TY_PTR && a->has_len && b->has_len &&
        ty_eq(a->elem, b->elem))
        return ty_slice(a->elem, a->is_const || b->is_const);

    if (a->kind == TY_COMPTIME_INT && ty_is_num(b)) return b;
    if (b->kind == TY_COMPTIME_INT && ty_is_num(a)) return a;
    if (a->kind == TY_COMPTIME_FLOAT && ty_is_float(b)) return b;
    if (b->kind == TY_COMPTIME_FLOAT && ty_is_float(a)) return a;

    if (a->kind == TY_NULL && b->kind == TY_OPT) return b;
    if (b->kind == TY_NULL && a->kind == TY_OPT) return a;
    if (a->kind == TY_NULL && b->kind == TY_BOOL) return a;
    if (b->kind == TY_NULL && a->kind == TY_BOOL) return b;

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
    case NK_BIN:
        if (e->op == TOK_PLUS || e->op == TOK_MINUS)  /* 指针运算保持来源 */
            return expr_frame(e->a) || expr_frame(e->b);
        return false;
    default: return false;
    }
}

static bool addr_frame(Node *o) {
    if (!o) return false;
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
    default: return false;
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
        /* 赋进局部变量的指针也记来源：之后 return 它同样要拦 */
        if (n->a->kind == NK_IDENT && n->a->sym && expr_frame(n->b))
            n->a->sym->stack_val = true;
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

static Type *check_binary(Node *n, Type *expect) {
    TokKind op = (TokKind)n->op;

    if (op == TOK_ASSIGN || op == TOK_PLUSEQ || op == TOK_MINUSEQ || op == TOK_STAREQ ||
        op == TOK_SLASHEQ || op == TOK_PERCEQ || op == TOK_AMPEQ || op == TOK_PIPEEQ ||
        op == TOK_CARETEQ || op == TOK_SHLEQ || op == TOK_SHREQ)
        return check_assign(n);

    Type *a = check_expr(n->a, (op == TOK_ANDAND || op == TOK_OROR) ? NULL : expect);
    Type *b = check_expr(n->b, expect);

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
static Type *check_array_lit(Node *n, Node **items, int nitems, Type *expect) {
    Type *elem = NULL;
    if (expect) {
        if (expect->kind == TY_PTR && expect->elem) elem = expect->elem;
        else if (expect->kind == TY_SLICE) elem = expect->elem;
    }
    for (int i = 0; i < nitems; i++) {
        Type *it = check_expr(items[i], elem);
        if (!elem) {
            Type *d = scalar_default(it);
            if (d->kind == TY_INVALID) return d;
            elem = d;
        } else if (it && it->kind == TY_PTR && it->has_len && it->elem &&
                   it->elem->kind == TY_INT && it->elem->bits == 8 &&
                   elem->kind == TY_PTR && elem->has_len && elem->elem &&
                   elem->elem->kind == TY_INT && elem->elem->bits == 8) {
            /* 字符串数组：长度各不相同的字面量统一成 `[]const u8`
             *（用户 2026-11-09：字符串是 *[N]const u8；不统一的话
             * `["alpha", "beta"]` 每个元素都得显式转换）。 */
            elem = ty_slice(ty_u8(), it->is_const || elem->is_const);
        }
    }
    if (!elem) {
        if (!expect || expect->kind != TY_PTR)
            return BAD(n, "cannot infer the element type of an empty array literal; write the target type");
        elem = ty_invalid();     /* 空数组：靠期待类型 */
    }
    for (int i = 0; i < nitems; i++)
        coerce_node(items[i], items[i]->ty, elem, "array element");
    return type_of(n, ty_ptr(elem, false, true, (uint64_t)nitems));
}

static Type *check_unary(Node *n, Type *expect) {
    if (n->op == TOK_AMP) {
        if (n->a->kind == NK_TUPLE)      /* &{…}（不带点）也不该当元组 */
            return check_array_lit(n, n->a->items, n->a->nitems, expect);
        Type *t = check_expr(n->a, NULL);
        /* `*[N]T` 只是"带长度的指针"：C 那边就是 `T *`，本身有槽位，
         * `&xs` 是合法的 `**(...)`（用户 2026-11-10 确认的设计）。
         * 以前这里错误地按"内联数组"挡掉，是文档/实现的误解。 */
        bool cst = !lvalue_mutable(n->a);
        return type_of(n, ty_ptr(t, cst, false, 0));
    }

    Type *t;
    if (n->op == TOK_MINUS && n->a && n->a->kind == NK_INT) {
        g_neg_lit_ctx = true;            /* 允许字面量恰好是 max+1（INT_MIN） */
        t = check_expr(n->a, expect);
        g_neg_lit_ctx = false;
    } else
        t = check_expr(n->a, expect);
    switch (n->op) {
    case TOK_MINUS: case TOK_PLUS:
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
        n->b->ct = NULL;
        from = to;
    }
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
        Value *cv = ct_eval_probe(f->dflt);
        if (cv && f->ty) cv = val_cast(f->ty, cv);
        if (cv && cv->kind != V_UNDEF) n->ct = cv;
    }
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

    /* 类型值上的成员：Color.red / Counter.n / ShapeTag.circle */
    if (bt && bt->kind == TY_TYPE) {
        Type *tv = base->type_val ? base->type_val : ty_invalid();
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
                if (mth) { n->inst = mth->fn; n->flag = mth->has_self; return type_of(n, mth->fnty); }
                {
                    const char *sug = suggest_member(n->name, tv);
                    if (sug) diag_help("did you mean `%s`?", sug);
                }
                return BAD(n, "`%s` has no member '%s'", ty_str(tv), n->name);
            }
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
        if (bt->kind == TY_SLICE) return type_of(n, ty_usize());
        if (bt->kind == TY_TUPLE) return type_of(n, ty_comptime_int());
        if (bt->kind == TY_PTR && bt->has_len) return type_of(n, ty_usize());
        /* 其余情况：结构体自己也可以有叫 len 的字段，落到普通字段查找 */
        if (bt->kind == TY_PTR && !ty_field(bt->elem, n->name))
            return BAD(n, "`%s` has no count; use *[N]T or []T", ty_str(bt));
    }

    switch (bt->kind) {
    case TY_PTR: {
        Type *e = bt->elem;
        if (!n->name) {
            if (bt->has_len) {          /* .N 是编译期下标 */
                if (n->ival >= bt->len)
                    return BAD(n, "index %llu out of range for `%s`",
                               (unsigned long long)n->ival, ty_str(bt));
                return type_of(n, e);
            }
            if (n->ival != 0)
                return BAD(n, "a bare pointer only has `.0` (dereference); use [] for indexing");
            return type_of(n, e);
        }
        /* `.` 自动穿透一层 */
        if (n->name && !strcmp(n->name, "tag")) {
            if (e->kind == TY_UNION)
                return type_of(n, e->is_tagged && e->tag_ty ? e->tag_ty : ty_error());
            if (e->kind == TY_OPT || e->kind == TY_ERR)
                return type_of(n, e->kind == TY_OPT ? ty_bool() : ty_error());
        }
        /* 数组指针的 .ptr：指向首元素。字符串字面量 `"abc".ptr` 走这条，
         * 给 C 的 `*const u8` 形参用；元素自己叫 ptr 的字段优先。 */
        if (n->name && !strcmp(n->name, "ptr") && bt->has_len && !ty_field(e, n->name))
            return type_of(n, ty_ptr(e, bt->is_const, false, 0));
        Field *f = ty_field(e, n->name);
        if (!f) {
            Method *m = ty_method(e, n->name);
            if (m) { n->inst = m->fn; n->flag = m->has_self; return type_of(n, m->fnty); }
            {
                const char *sug = suggest_member(n->name, e);
                if (sug) diag_help("did you mean `%s`?", sug);
            }
            return BAD(n, "`%s` has no member '%s'", ty_str(e), n->name);
        }
        if (f->is_static) return static_member_ty(n, f);
        return type_of(n, f->ty);
    }

    case TY_SLICE:
        if (n->name && !strcmp(n->name, "ptr"))
            return type_of(n, ty_ptr(bt->elem, bt->is_const, false, 0));
        if (!n->name && n->ival == 0) return type_of(n, bt->elem);
        return BAD(n, "a slice only has .ptr / .len / .0");

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
            if (m) { n->inst = m->fn; n->flag = m->has_self; return type_of(n, m->fnty); }
            {
                const char *sug = suggest_member(n->name, bt);
                if (sug) diag_help("did you mean `%s`?", sug);
            }
            return BAD(n, "`%s` has no field '%s'", ty_str(bt), n->name);
        }
        /* 编译期聚合值里的 type 字段（std.FieldInfo.ty）：字段值本身就是
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
            if (m) { n->inst = m->fn; n->flag = m->has_self; return type_of(n, m->fnty); }
            return BAD(n, "`%s` has no member '%s'", ty_str(bt), n->name);
        }
        if (f->is_static) return static_member_ty(n, f);
        return type_of(n, f->ty);
    }

    case TY_ENUM: {
        if (!n->name)
            return BAD(n, "an enum value has no `.N` field; cast it to an integer instead");
        Field *f = ty_static_member(bt, n->name);      /* 枚举体的 const 成员 */
        if (f) return type_of(n, f->ty);
        Method *m = ty_method(bt, n->name);
        if (m) { n->inst = m->fn; n->flag = m->has_self; return type_of(n, m->fnty); }
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
    Type *it = check_expr(n->b, ty_usize());
    if (!ty_is_int(it)) return BAD(n->b, "index must be an integer, found `%s`", ty_str(it));
    if (bt->kind == TY_PTR && bt->has_len) {
        /* 长度在类型里、下标又是编译期值：越界在这里报，不让它拖到运行时
         * 才 trap（`arr[5]` 对 `*[3]i32` 编译期就能确定）。 */
        Value *iv = n->b->ct ? n->b->ct : ct_eval_probe(n->b);
        if (iv && iv->kind == V_INT) {
            int64_t x = val_i64(iv);
            if (x < 0 || (uint64_t)x >= bt->len)
                return BAD(n->b, "index %lld is out of bounds (len %llu)",
                           (long long)x, (unsigned long long)bt->len);
        }
        return type_of(n, bt->elem);
    }
    if (bt->kind == TY_PTR) return type_of(n, bt->elem);
    if (bt->kind == TY_SLICE) return type_of(n, bt->elem);
    if (bt->kind == TY_TUPLE)
        return BAD(n, "tuple has no runtime index; use .N with a compile-time number");
    return BAD(n, "`%s` cannot be indexed", ty_str(bt));
}

static Type *check_slice_expr(Node *n) {
    Type *bt = check_expr(n->a, NULL);
    Type *elem = NULL;
    if (bt->kind == TY_PTR && bt->has_len) elem = bt->elem;
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
    if (bt->kind == TY_PTR && bt->has_len) {
        Value *lv = n->b ? (n->b->ct ? n->b->ct : ct_eval_probe(n->b)) : NULL;
        Value *hv = n->c ? (n->c->ct ? n->c->ct : ct_eval_probe(n->c)) : NULL;
        bool lok = !n->b || (lv && lv->kind == V_INT);
        bool hik = !n->c || (hv && hv->kind == V_INT);
        int64_t lov = lok ? (n->b ? val_i64(lv) : 0) : 0;
        int64_t hiv = hik ? (n->c ? val_i64(hv) : (int64_t)bt->len) : 0;
        if (lok && n->b && lov < 0)
            return BAD(n->b, "slice lower bound %lld is negative", (long long)lov);
        if (hik && n->c && hiv > (int64_t)bt->len)
            return BAD(n->c, "slice upper bound %lld is out of bounds (len %llu)",
                       (long long)hiv, (unsigned long long)bt->len);
        if (lok && hik && n->b && n->c && lov > hiv)
            return BAD(n, "slice lower bound %lld is greater than upper bound %lld",
                       (long long)lov, (long long)hiv);
    }
    bool cst = (bt->kind == TY_PTR) ? bt->is_const : bt->is_const;
    return type_of(n, ty_slice(elem, cst));
}

/* 被调用的东西是不是泛型函数（按名字找得到，或者是一个泛型 fn 字面量） */
/* 命名空间成员（std.ArrayList / util.make）：拿到模块里那个符号 */
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
     * module("...") 的加载），先按需解析成 SK_NS 再看成员。 */
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
        /* 字符串字面量是 *[N]const u8：`{s}` 打印的是字节序列，不是地址 */
        bool str_arr = t->kind == TY_PTR && t->has_len && t->elem &&
                       t->elem->kind == TY_INT && t->elem->bits == 8;
        if (t->kind != TY_SLICE && !str_arr)
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

/* 方法调用。两种写法：
 *
 *   p.sum()          →  sum(p)          （接收者是 f 的第一个参数）
 *   Point.sum(&p)    →  sum(&p)         （接收者由调用者显式写出来）
 *
 * 接收者参数是 `*T` 时，`p.sum()` 自动写 `&p`——所以接收者必须能取地址。
 * 语义上方法就是普通函数，这里只做两件事：补第一个实参、把调用点挂到方法上，
 * 后端于是用同一套方式生成。 */
static Type *check_method_call(Node *n) {
    Node *fld = n->a;
    Type *mft = fld->ty;
    if (!mft || mft->kind != TY_FN)
        return type_of(n, ty_invalid());

    Type *ret = mft->ret ? mft->ret : ty_void();
    Type *recv_ty = fld->a->ty;

    /* 已经补过接收者了（克隆体复检：ct_call 会 clone 检查过的函数体再 check
     * 一遍）。参数表和类型都已就位，别再把接收者塞一次——那会让 arity 差一。 */
    if (n->meth_done) return type_of(n, ret);

    if (recv_ty && recv_ty->kind == TY_TYPE) {         /* Point.sum(&p) */
        if (n->nitems != mft->nparams)
            return BAD(n, "method takes %d argument(s) (receiver included), %d given",
                       mft->nparams, n->nitems);
        for (int i = 0; i < n->nitems; i++)
            coerce_node(n->items[i], check_expr(n->items[i], mft->params[i]),
                        mft->params[i], "argument");
        n->inst = fld->inst;
        n->ival = 0;
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
            if (!addr_ok(recv))
                return BAD(recv, "cannot take the address of this receiver; "
                                 "bind it to a `let` first");
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
    /* 内建命名空间方法：std.print / std.println / std.format */
    if (n->a->kind == NK_FIELD && n->a->a && n->a->a->kind == NK_IDENT) {
        Type *bt = check_expr(n->a->a, NULL);
        if (bt && bt->kind == TY_TYPE && n->a->a->sym && n->a->a->sym->kind == SK_NS) {
            Symbol *m = ns_member(n->a->a->sym, n->a->name, n->a->line, n->a->col);
            if (!m) return type_of(n, ty_invalid());
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
            /* 统一分配 API：std.stackAlloc(T, N) / std.heapAlloc(alloc, T, N)
             * 都返回数组指针 `*[N]T`（decisions §44）。N 必须编译期已知：
             * 结果类型里带着长度，运行时长度得去堆上自己管。 */
            if (m->builtin == BI_STACKALLOC || m->builtin == BI_HEAPALLOC) {
                bool heap = (m->builtin == BI_HEAPALLOC);
                if (n->nitems != (heap ? 3 : 2))
                    return BAD(n, "std.%s takes %s, a type and a count", m->name,
                               heap ? "an allocator" : "a type");
                int base = 0;
                if (heap) {
                    Type *at = check_expr(n->items[0], sema_allocator_type());
                    coerce_node(n->items[0], at, sema_allocator_type(), "allocator");
                    base = 1;
                }
                Type *elem = resolve_type(n->items[base]);
                if (!elem || elem->kind == TY_INVALID || elem->kind == TY_TYPE ||
                    elem->kind == TY_VOID)
                    return BAD(n, "std.%s cannot allocate `%s`", m->name,
                               elem ? ty_str(elem) : "?");
                check_expr(n->items[base + 1], ty_usize());
                IntV c = ev_int(n->items[base + 1]);
                if (!c.ok)
                    return BAD(n->items[base + 1],
                               "std.%s count must be known at compile time", m->name);
                n->esc = !heap;          /* 栈那份指向当前帧：逃逸检查盯它 */
                return type_of(n, ty_ptr(elem, false, true, c.v));
            }
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
    return type_of(n, ct->ret ? ct->ret : ty_void());
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
        return type_of(n, t);
    }
    (void)expect;
    return BAD(n, "`%s` is not a struct, union or tuple", ty_str(t));
}

static Type *check_tuple_lit(Node *n, Type *expect) {
    if (expect && expect->kind == TY_TUPLE && expect->nfields == n->nitems) {
        for (int i = 0; i < n->nitems; i++) {
            Type *it = check_expr(n->items[i], expect->fields[i].ty);
            coerce_node(n->items[i], it, expect->fields[i].ty, "tuple element");
        }
        return type_of(n, expect);
    }
    Type **items = calloc((size_t)(n->nitems ? n->nitems : 1), sizeof(Type *));
    for (int i = 0; i < n->nitems; i++) items[i] = check_expr(n->items[i], NULL);
    Type *t = ty_tuple(items, n->nitems);
    free(items);
    n->ty = t;
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

    if (!strcmp(nm, "sizeOf") || !strcmp(nm, "alignOf")) {
        if (n->nitems < 1) return BAD(n, "@%s needs a type", nm);
        Type *t = resolve_type(n->items[0]);
        int sz = !strcmp(nm, "sizeOf") ? ty_size(t) : ty_align(t);
        if (sz < 0)
            return BAD(n, "@%s of `%s` is not known at compile time", nm, ty_str(t));
        n->ival = (uint64_t)sz;
        return type_of(n, ty_usize());
    }

    if (!strcmp(nm, "typeName")) {
        if (n->nitems < 1) return BAD(n, "@typeName needs a type");
        Type *t = resolve_type(n->items[0]);
        n->str = ty_str(t);         /* 编译期就定下来 */
        n->str_len = (int)strlen(n->str);
        return type_of(n, ty_slice(ty_u8(), true));
    }

    if (!strcmp(nm, "compileError")) {
        /* 只有真的执行到这一行才报错：放在 `if` 里的 @compileError 是编译期
         * 分支的一部分，不能在检查函数体时就炸。 */
        return type_of(n, ty_noreturn());
    }

    if (!strcmp(nm, "trap")) return type_of(n, ty_noreturn());

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

    if (!strcmp(nm, "memcpy") || !strcmp(nm, "memmove") || !strcmp(nm, "memset")) {
        /* @memcpy(dst, src, bytes) / @memmove(dst, src, bytes) /
         * @memset(dst, byte, bytes)：批量内存操作，库容器（ArrayList 等）
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

    /* 1.2.0 起砍到只剩 9 个内建（见 docs/manual.md §7）。老代码里残留的
     * 名字给一条能照着改的提示，而不是含糊的 "unknown builtin"。 */
    {
        static const struct { const char *name, *hint; } gone[] = {
            { "as",       "type conversion is written `(T)value`" },
            { "intCast",  "type conversion is written `(T)value`" },
            { "ptrCast",  "type conversion is written `(T)value`" },
            { "bitCast",  "type conversion is written `(T)value`" },
            { "typeOf",   "take a `T: type` parameter instead" },
            { "typeInfo", "use @typeName / @sizeOf / @alignOf" },
            { "hasField", "use @typeName / @sizeOf / @alignOf" },
            { "fieldType","use @typeName / @sizeOf / @alignOf" },
            { "fieldName","use @typeName / @sizeOf / @alignOf" },
            { "field",    "write the field access directly: `v.name`" },
            { "offsetOf", "compute offsets from @sizeOf / @alignOf" },
            { "embedFile","embed the bytes yourself (a literal or std.fs)" },
            { "file",     "source-location builtins are gone" },
            { "line",     "source-location builtins are gone" },
            { "col",      "source-location builtins are gone" },
            { "fnName",   "source-location builtins are gone" },
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
        if (!s) {
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
         * 的（互递归），不受影响；懒模块声明（fn 里的 `const std = module(..)`）
         * 靠使用点触发解析，也豁免。 */
        if (s->fn_local && s->state == 0 && s->decl &&
            !(s->decl->kind == NK_DECL && decl_is_lazy_module(s->decl))) {
            diag_help("blocks are plain statement sequences: move the "
                      "declaration of '%s' above this use", n->name);
            return BAD_CODE("E0401", n, "'%s' is used before its declaration "
                            "in this block", n->name);
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
        /* 编译期就知道是空的：`.?` 在这里必然 abort，和除零同一处置（§7.R10）——
         * 能定就在编译期报，不要留到运行期。 */
        Value *cv = ct_eval_probe(n->a);
        if (cv && (cv->kind == V_NULL || cv->kind == V_UNDEF))
            return BAD_CODE("E0506", n, "`.?` on a value that is `null` at compile time -- "
                                        "it would abort every time");
        return type_of(n, t->elem);
    }

    /* `try e` —— 出错就带着同一个码往上返回，成功取载荷。
     * 它是明式的：调用点写着 `try`，读者就知道这个函数在这里可能退出；
     * 没有栈展开、没有 catch 块、类型也不变（还是那个 `!T`）。 */
    case NK_TRY: {
        Type *t = check_expr(n->a, NULL);
        if (!t || t->kind == TY_INVALID) return ty_invalid();
        /* `defer` / `errdefer` 体里不能往外抛：两者都在返回路径上执行，
         * 自己再"提前返回"没有意义（会把正在返回的值覆盖掉）。就地处理掉。 */
        if (g_in_defer)
            return BAD(n, "`try` is not allowed inside `defer`/`errdefer` -- a defer body "
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
        coerce_node(n->b, alt, elem, "`orelse` alternative");
        return type_of(n, elem);
    }
    case NK_CAST:     return check_cast(n);
    case NK_FIELD:    return check_field(n);
    case NK_INDEX:    return check_index(n);
    case NK_SLICE:    return check_slice_expr(n);
    case NK_CALL:     return check_call(n, expect);
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
            if (!n->str) return BAD(n, "module(...) needs a file path");
            n->sym = NULL;
            return type_of(n, ty_type());
        }
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
    for (int i = 0; i < blk->nitems; i++) {
        Node *s = blk->items[i];
        if (dead) err_unreachable(s, prev);
        /* 具名绑定到 fn 字面量的，把名字贴上：const helper = fn () void{…}
         * 这种写法下 fn 节点本身没有名字，@fnName() 就只会拿到 "?"。 */
        if (s->kind == NK_DECL && s->name && s->b && s->b->kind == NK_FN && !s->b->name)
            s->b->name = s->name;
        check_stmt(s);
        if (!dead && stmt_blocks_next(s)) dead = true;
        prev = s;
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

/* 顶层 const 的初值是不是"惰性模块引用"：module("...") 本身，或者指向
 * 这类别名的成员链（std.zx 顶层的 `pub const HashMap = map.HashMap;`
 * 就是后者）。
 *
 * 为什么要惰性：模块加载走的是 check_stmts(m->root) → 每条顶层 decl
 * 都 ensure 一遍，于是 `pub const str = module("str.zx")` 在 std 一被
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
    /* 成员链：根标识符必须解析到"初值是 module(...) 的 const 别名"，
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
     * state = 3 的意思是"外壳已就绪、成员表还没填"。 */
    if (!d->a && d->b &&
        (d->b->kind == NK_STRUCT_TYPE || d->b->kind == NK_UNION_TYPE ||
         d->b->kind == NK_ENUM_TYPE)) {
        Type *shell = ty_agg(d->b->kind == NK_UNION_TYPE ? TY_UNION
                            : d->b->kind == NK_ENUM_TYPE ? TY_ENUM : TY_STRUCT,
                             d->b, NULL);
        s->type_val = shell;
        s->ty = ty_type();
        s->state = 3;
    }

    Type *ann = d->a ? resolve_type(d->a) : NULL;
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
        /* let / const 绑定数组字面量 → 只读视图（R9） */
        if (!d->a && t->kind == TY_PTR && t->has_len && !t->is_const && s->kind != SK_VAR) {
            t = ty_ptr(t->elem, true, true, t->len);
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
        /* 模块加载失败：错误已经在 module(...) 那行报过。绑定照旧算"命名
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
         * 烙成 true）。s->ct 已经拿到最终值，把子树的烙印擦掉，只留
         * 根上的——顶层 const 的静态物化要读 d->b->ct。 */
        if (s->ct) {
            ct_clear(d->b->a); ct_clear(d->b->b);
            ct_clear(d->b->c); ct_clear(d->b->d);
            for (int i = 0; i < d->b->nitems; i++) ct_clear(d->b->items[i]);
        }
    }

    /* R1：凡 const，编译器必须能在编译期算出值 */
    if (s->kind == SK_CONST && d->b && !s->ct && !s->type_val &&
        s->ty && s->ty->kind != TY_INVALID && d->b->kind != NK_MODULE &&
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
 * 为什么补后缀：`module("ext")` 比 `module("ext.zx")` 是文档里一直在写的
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
    if (old) {
        if (old->loading) {
            err_at(line, col, "cyclic module dependency: '%s' is still being loaded", full);
            return NULL;
        }
        return old;
    }

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
    if (!m->root) {
        /* 这条诊断属于导入方：位置是 module(...) 调用点，源码片段也得取
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
    for (int i = 0; g_std_seeded[i]; i++) {
        Symbol *b = std_builtin(g_std_seeded[i]);
        if (!b) continue;
        Symbol *s2 = scope_add(&m->scope, b->name, b->kind);
        if (!s2) continue;
        sym_copy(s2, b, b->name);
        s2->is_pub = true;
    }
    g_root_stmts = true;
    check_stmts(m->root);
    g_root_stmts = false;

    g_srcpath = save_p;
    g_scope = save_s;
    g_mod = save_m;
    m->loading = false;
    return m;
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

/* std 源码在哪：$ZX_STD → 可执行文件旁边（bin/ 与项目根）→ 当前目录 */
static bool find_std_path(char *out, size_t cap) {
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
    for (int i = 0; g_std_seeded[i]; i++) {
        Symbol *b = std_builtin(g_std_seeded[i]);
        if (!b) continue;
        Symbol *s = scope_add(&m->scope, b->name, b->kind);
        if (!s) continue;                 /* std.zx 自己声明了同名：以它为准 */
        sym_copy(s, b, b->name);
        s->is_pub = true;
    }
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
    for (int i = 0; i < fn->nitems; i++) {
        Node *pm = fn->items[i];
        if (!pm->name) continue;
        Symbol *s = scope_add(&sc, pm->name, pm->flag ? SK_VAR : SK_PARAM);
        if (getenv("ZX_DBG")) fprintf(stderr, "[dbg]   bind param '%s' -> %p\n", pm->name ? pm->name : "(null)", (void*)s);
        if (!s) { err_at(pm->line, pm->col, "duplicate parameter '%s'", pm->name); continue; }

        s->ty = pm->ty;
        s->state = 2;
        s->in_fn = true;      /* 形参槽位也在被调方的栈帧上：&pm 不可返回 */
        local_track(s, pm->line, pm->col);
        pm->sym = s;
    }

    Type *save_ret = g_ret;
    bool  save_defer = g_in_defer;      /* 进了函数体：defer 上下文不再适用 */
    g_ret = ret;
    g_in_defer = false;
    g_fn_depth++;
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

    Type *t = ty_void();
    if (lb) t = lb->has ? lb->ty : ty_void();
    if (lb) label_pop();
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
        return;

    case NK_FN:
        if (n->sym) ensure(n->sym);
        else { resolve_fn_type(n); if (!n->flag && n->b && !fn_is_generic(n)) schedule_body(n); }
        return;

    case NK_RETURN: {
        if (!g_fn_depth) { err_at(n->line, n->col, "`return` outside a function"); return; }
        if (g_in_defer) { err_at(n->line, n->col, "`return` is not allowed inside `defer`/`errdefer`"); }
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
        /* 逃逸检查（decisions §44）：不许把指向当前栈帧的指针带出去 */
        if ((want->kind == TY_PTR || want->kind == TY_SLICE) && expr_frame(n->a))
            err_at(n->a->line, n->a->col,
                   "cannot return a pointer into the current function's stack frame "
                   "(use std.heapAlloc for outliving storage)");
        n->ty = want;
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
        if (g_in_defer) { err_at(n->line, n->col, "`defer` cannot yield"); return; }
        Label *l = label_find(n->label);
        if (!l) {
            err_at(n->line, n->col, "no enclosing scope labeled '%s'", n->label);
            if (n->a) check_expr(n->a, NULL);
            return;
        }
        if (!n->a) {
            if (l->has) l->ty = unify(n, l->ty, ty_void());
            else { l->ty = ty_void(); l->has = true; }
            n->ty = ty_void();
            return;
        }
        Type *t = check_expr(n->a, l->has ? l->ty : NULL);
        if (!l->has) { l->ty = t; l->has = true; }
        else l->ty = unify(n, l->ty, t);
        n->ty = l->ty;
        return;
    }

    case NK_DEFER:
    case NK_ERRDEFER: {
        /* errdefer 只在错误返回路径上执行——不返回错误的函数里它永远
         * 不会触发，几乎肯定是写错了（Zig 同样拒绝）。 */
        if (n->kind == NK_ERRDEFER && (!g_ret || g_ret->kind != TY_ERR))
            err_at(n->line, n->col,
                   "`errdefer` requires the enclosing function to return an "
                   "error union `!T`, found `%s`",
                   g_ret ? ty_str(g_ret) : "void");
        bool save = g_in_defer;
        g_in_defer = true;
        if (n->a->kind == NK_BLOCK) check_block(n->a);
        else check_expr(n->a, NULL);
        g_in_defer = save;
        return;
    }

    default: {
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
    if (a->kind == NK_BLOCK) return check_block(a);
    return check_expr(a, expect);
}

/* 表达式是不是"纯编译期"的：没有读 var / 参数 / 捕获，也没有调用。
 * 只有这种 if 条件才允许只检查一支。 */
static bool ct_pure(Node *n) {
    if (!n) return true;
    switch (n->kind) {
    case NK_IDENT:
        if (!n->sym) return true;
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
    {
        Scope sc;
        scope_push(&sc);
        if (cap_ty && n->name) {
            Symbol *s = scope_add(&sc, n->name, SK_CAPTURE);
            if (s) { s->ty = cap_ty; s->state = 2; }
        }
        tt = (known && !cv->bval) ? ty_void() : check_arm(n->b, expect);
        scope_pop(&sc);
    }

    Type *et = NULL;
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
        scope_pop(&sc);
    }

    Type *r;
    if (lb)                        r = lb->has ? lb->ty : ty_void();
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
            if (s) { s->ty = cap_ty; s->state = 2; }
        }
        check_block(n->b);
        scope_pop(&sc);
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
    if (base->ty && base->ty->kind == TY_PTR && base->ty->has_len) blen = base->ty->len;
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
            } else if (st->kind == TY_PTR && st->has_len) {
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
        check_block(n->b);
        scope_pop(&sc);
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

        {
            Scope sc;
            scope_push(&sc);
            if (arm->name && cap_ty) {
                Symbol *cs = scope_add(&sc, arm->name, SK_CAPTURE);
                if (cs) { cs->ty = cap_ty; cs->state = 2; }
            }
            Type *at = check_arm(arm->a, expect);
            scope_pop(&sc);

            if (al) { at = al->has ? al->ty : ty_void(); label_pop(); }

            if (!result) result = at;
            else         result = unify(arm, result, at);
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
static bool while_never_exits(Node *n) {
    if (!n->a) return false;
    if (n->a->kind == NK_BOOL) return n->a->ival != 0;
    if (!ct_pure(n->a)) return false;
    Type *c = n->a->ty;
    if (c && (c->kind == TY_OPT || c->kind == TY_ERR)) return false;
    Value *cv = n->a->ct;
    return cv && cv->kind == V_BOOL && cv->bval;
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
    switch (n->kind) {
    case NK_BLOCK:  return check_block(n);
    case NK_IF:     return check_if(n, expect);
    case NK_WHILE:  return check_while(n, expect);
    case NK_FOR:    return check_for(n, expect);
    case NK_SWITCH: return check_switch(n, expect);
    default:        return type_of(n, ty_void());
    }
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
    g_cmods = 8;
    g_mods = calloc((size_t)g_cmods, sizeof(Module *));
    g_mods[g_nmods++] = m0;
    m0->loading = true;         /* 入口文件也在"加载中"：别人导入它就是循环 */

    g_mod = m0;
    g_scope = &m0->scope;
    m0->scope.is_module = true;
    hoist(&m0->scope, root);
    g_root_stmts = true;
    check_stmts(root);
    g_root_stmts = false;

    /* 函数体：每个都回到它所属模块的作用域里检查。
     * 检查过程中可能又冒出新的待检查体（泛型实例、模块里的函数），
     * 所以这里边跑边长。 */
    for (int i = 0; i < g_npending; i++) {
        Pending *p = &g_pending[i];
        Module *sm = g_mod;
        Scope  *ss = g_scope;
        const char *sp = g_srcpath;
        if (p->mod) {
            g_mod = p->mod;
            g_scope = p->scope ? p->scope : &p->mod->scope;
            g_srcpath = p->mod->path;
        }
        check_fn_body(p->fn);
        g_mod = sm;
        g_scope = ss;
        g_srcpath = sp;
    }

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
    bool shadowed = at && n->name && (at->kind == TY_STRUCT || at->kind == TY_UNION) &&
                    ty_field(at, n->name) != NULL;
    if (!shadowed && n->name && (!strcmp(n->name, "len") || !strcmp(n->name, "size"))) {
        bool is_len = !strcmp(n->name, "len");
        Type *t = n->a ? n->a->ty : NULL;
        if (!t) return NULL;

        /* 数量在类型里：*[N]T */
        if (t->kind == TY_PTR && t->has_len) {
            if (is_len) return val_int(ty_usize(), t->len, false);
            return val_int(ty_usize(),
                           (uint64_t)ty_size_relaxed(t->elem) * t->len, false);
        }
        if (t->kind == TY_TUPLE && is_len)
            return val_int(ty_usize(), (uint64_t)t->nfields, false);

        /* 数量在值里：切片 / 字符串 */
        if (!is_len && t->kind != TY_SLICE)
            return val_int(ty_usize(), (uint64_t)ty_size_relaxed(t), false);

        Value *b = ct_eval(n->a);
        int64_t len;
        if (!b || !ct_view_len(b, t, &len)) return NULL;
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
static bool ct_store(Node *lhs, Value *v) {
    if (!lhs || !v) return false;
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
    if (!pt || pt->kind != TY_PTR) return NULL;
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
        if ((k == SK_VAR || k == SK_PARAM || k == SK_CAPTURE) && g_ct_exec == 0) break;
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
        /* 枚举变体：Color.green / std.TypeKind.struct_。check_field 不设
         * n->ct，这里按类型值上的变体表求值——泛型里 `info.kind == .struct_`
         * 要靠它折成常量 bool，后端才剪得动分支。 */
        if (n->a && n->a->ty && n->a->ty->kind == TY_TYPE && n->name) {
            Type *tv = n->a->type_val;
            if (tv && tv->kind == TY_ENUM) {
                Field *f = ty_field(tv, n->name);
                if (f && f->has_tag) { v = val_int(f->ty ? f->ty : tv, f->tag, false); break; }
            }
        }
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
        if (!strcmp(nm, "sizeOf") || !strcmp(nm, "alignOf")) {
            v = val_int(ty_usize(), n->ival, false);
        } else if (!strcmp(nm, "typeName") && n->str) {
            v = val_str(n->str, n->str_len);
        } else if (!strcmp(nm, "compileError")) {
            if (!n->flag) {            /* 折叠可能重试，只报一次 */
                n->flag = true;
                if (n->nitems >= 1 && n->items[0]->kind == NK_STR)
                    err_at(n->line, n->col, "@compileError: %.*s",
                           n->items[0]->str_len, n->items[0]->str);
                else
                    err_at(n->line, n->col, "@compileError");
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
        }
        break;
    }

    /* 类型表达式：struct{…} / *[N]T / Ident —— 类型是值 */
    case NK_TYPE_IDENT: case NK_PTR_TYPE: case NK_SLICE_TYPE:
    case NK_TUPLE_TYPE: case NK_STRUCT_TYPE: case NK_UNION_TYPE:
    case NK_ENUM_TYPE:  case NK_OPT_TYPE:   case NK_ERR_TYPE:  case NK_ERR_SET:
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
            /* 循环自己的标签收了这次收割才拿得到值，否则继续往外冒 */
            if (n->label && f.label && !strcmp(n->label, f.label)) v = f.val;
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
        Value *c = ct_eval(n->a);
        if (!c) return ct_normal();
        Node *br = val_truthy(c) ? n->b : n->c;
        return br ? ct_run_arm(br) : ct_normal();
    }

    case NK_WHILE: {
        for (long guard = 0; guard < CT_MAX_LOOPS; guard++) {
            ct_clear(n->a);
            Value *c = ct_eval(n->a);
            if (!c) return ct_normal();
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
        if (!s) return ct_normal();
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
    return false;
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
    Node *g = clone_tree(fn);
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
    if ((call->a->kind == NK_IDENT || call->a->kind == NK_TYPE_IDENT) && call->a->name)
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
