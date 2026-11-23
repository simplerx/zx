/* sema_core.c - 名字解析基础设施：fix-it / 作用域 / 内建 / 模块装载。
 * 从 sema.c 拆分；共享声明见 sema_internal.h。
 */
#include "sema_internal.h"


/* ---------------- fix-it：did you mean（A2 · M42） ----------------
 * 三类建议：未知标识符（作用域链）、聚合成员（fields/methods/statics）、
 * std 与模块成员（名字表 / 模块作用域）。在 err_at 之前调 diag_help，
 * 建议行跟在那条诊断后面；没有合适的候选就不挂，诊断保持原样。 */

/* OSA 距离：Levenshtein + 相邻换位算 1 次编辑。手滑换位（naem / coutn）
 * 是最高频的拼写错误，纯 Levenshtein 会把它算成 2 次而错过建议。 */
int edit_distance(const char *a, const char *b) {
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
bool name_close(const char *bad, const char *cand) {
    if (!bad || !cand || !*cand) return false;
    return edit_distance(bad, cand) <= (strlen(bad) <= 4 ? 1 : 2);
}

/* 聚合类型的成员建议：fields + methods + statics（枚举体的 const/var）。 */
const char *suggest_member(const char *bad, Type *t) {
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

Scope *g_scope;
/* 当前函数体登记过的全部局部绑定（参数、let/var、解构槽、捕获）。
 * 为什么另立一条链：作用域是**栈上**的，出块就销毁，符号却活得久——
 * 想在函数体结束时统一问"哪些一次都没用过"，只能自己记一份。 */
Symbol *g_locals;

/* 作用域链上找与 bad 最接近的名字（fix-it）。符号名的寿命覆盖诊断
 * 打印，直接返回指针。 */
const char *suggest_ident(const char *bad) {
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

Module **g_mods;
int      g_nmods, g_cmods;
Module  *g_mod;
/* 正在检查文件根的语句列表（模块加载 / 入口根）。文件根上的惰性模块
 * 声明才允许推迟解析；函数体、comptime 块里的 decl 一律当场算。
 * check_block 进块时清掉它——嵌套块里的 decl 不得享受文件根的豁免。 */
bool g_root_stmts;
bool g_ct_stuck;
int  g_ct_exec;
/* 检查期投机折叠（fold_expr）期间置位：这段折叠只为填 ct 缓存，
 * 折不出值是常态。除零这类"值域错误"在这期间软失败（返回 NULL），
 * 不做硬错误——循环体这类死代码里的 0/0 不是用户的错（isqrt(0) 误报）。
 * 真正的 const 上下文（R1 初始化器、数组长度）不走这条路，照旧硬报。 */
bool g_ct_soft_div;
/* 折叠函数调用时，体要"先检查、再执行"（ct_call → check_block → ct_run）。
 * 检查那一步没有真实的槽位绑定：局部 `var i` 的值可能还留着上一次执行的
 * 残留，于是 `s[i]` 会用「新实参 + 旧下标」折出越界。那不是用户代码的错，
 * 真正的执行（ct_run，绑定齐全）紧随其后。这期间越界软失败即可。 */
bool g_ct_soft_bounds;
CtFlow g_ct_flow;

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
Value *sema_sym_ct(Symbol *s) { return s ? s->ct : NULL; }

int g_ntslots;   /* §173：tag 槽位计数（定义在贴标账本一节；
                        * scope_push 的槽位水位线要用，前移声明） */
void scope_push(Scope *s) {
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
void scope_pop(Scope *s)  { g_scope = s->parent; }

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
Symbol *scope_find_here(Scope *sc, const char *name) {
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

Symbol *scope_add(Scope *sc, const char *name, int kind) {
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
void local_track(Symbol *s, int line, int col) {
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
void check_unused(Symbol *head, int errs_before) {
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

Symbol *lookup(const char *name) {
    for (Scope *sc = g_scope; sc; sc = sc->parent) {
        Symbol *s = scope_find_here(sc, name);
        if (s) return s;
    }
    return NULL;
}

/* 带声明位置的版本：只有"值得被 note 指回去"的声明（顶层、类型体成员）
 * 才记位置——参数、捕获这类就地重复的，note 没有增量价值。 */
Symbol *scope_add_at(Scope *sc, const char *name, int kind, int line, int col) {
    Symbol *s = scope_add(sc, name, kind);
    if (s) { s->decl_line = line; s->decl_col = col; }
    return s;
}

const char *sema_sym_name(Symbol *s) { return s ? s->name : NULL; }
bool sema_fn_is_generic(Node *fn) { return fn_is_generic(fn); }
int  sema_fn_ncomp(Node *fn) { return fn ? (int)fn->ival : 0; }
static Label g_labels[MAX_LABEL];
int   g_nlabels;

Label *label_push(const char *name, bool is_loop) {
    if (g_nlabels >= MAX_LABEL) return NULL;
    Label *l = &g_labels[g_nlabels++];
    l->name = name; l->ty = NULL; l->has = false; l->is_loop = is_loop; l->is_fn = false;
    l->val = NULL;
    return l;
}
void label_pop(void) { if (g_nlabels) g_nlabels--; }

Label *label_find(const char *name) {
    for (int i = g_nlabels - 1; i >= 0; i--)
        if (g_labels[i].name && name && strcmp(g_labels[i].name, name) == 0)
            return &g_labels[i];
    return NULL;
}
Label *label_find_loop(const char *name) {
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

Type  *g_ret;
int    g_fn_depth;
bool   g_in_defer;
bool   g_in_tag_rebind;   /* §187：续链值里的 int→ptr cast 免 E0525 */
int    g_arm_depth;       /* §188：分支臂嵌套深度（释放禁区的判定）；
                              * 循环体不算臂（逐迭代 alloc/free 是惯用法），
                              * defer 体由 g_in_defer 单独豁免。 */
Pending *g_pending;
int g_npending, g_cappending;
DevirtPending *g_devirt_retry;
int g_ndr, g_cdr;
bool g_devirt_final;

Inst *g_insts;
int   g_ninst, g_cinst;

/* 实例化嵌套层数（防止泛型体里再写一遍自己时无限套娃） */
int   g_spec_depth;

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

void schedule_body(Node *fn) {
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

Type *base_type(const char *n) {
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
        if (bits == 16 || bits == 32 || bits == 64 || bits == 128) return ty_float(bits);
        return NULL;
    }
    if (!strcmp(n, "Allocator")) return sema_allocator_type();
    /* `@type(x)` 的两个描述符类型：源文件里没有声明，名字是编译器内建的
     * （和 Allocator 一样）。LSP 的行内提示会写 `: Type` / `: Field`。 */
    if (!strcmp(n, "Type"))  return sema_type_type();
    if (!strcmp(n, "Field")) return sema_type_field_type();
    return NULL;
}

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
Type *sema_type_field_type(void) {
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

Type *sema_type_type(void) {
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
Value *type_desc_value(Type *t, Node *inst) {
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
Symbol *std_builtin(const char *name) {
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
const char *g_std_seeded[] = {
    "print", "println", "format", "heap", "file", "io", "Allocator",
    "Type", "Field", "stackAlloc", "heapAlloc", "heapSlice", "free", "process",
    "debug", "atomic", NULL
};

Symbol *ns_member(Symbol *ns, const char *name, int line, int col) {
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

char *path_dup_dir(const char *path) {    const char *s1 = strrchr(path, '/');
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

char *path_join(const char *dir, const char *rel) {
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

Module *module_find(const char *path) {
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

Module *module_load(const char *rel, int line, int col) {
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

Module *module_load_full(const char *full_in, int line, int col) {
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
void stamp_mod(Node *n, const char *path) {
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
bool deny_member(Node *n, Type *owner, bool is_pub, const char *name) {
    if (!member_hidden(owner, is_pub)) return false;
    err_at(n->line, n->col,
           "'%s' is a private member of `%s` (declared in %s); "
           "mark it `pub` in the container body to expose it",
           name ? name : "?", ty_str(owner), owner->mod);
    return true;
}

/* ---------------- 标准库源码（std/） ---------------- */

char    g_exe_dir[1024];

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
bool find_std_path(char *out, size_t cap) {
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
