/* sema.c - 语义层统一入口：依赖拓扑 + sema_check。
 * 从 sema.c 拆分；共享声明见 sema_internal.h。
 */
#include "sema_internal.h"


void deps_check_one(Node *fn) {
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

void deps_check_calls(Node *n, Scope *ps) {
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
    Module *m0 = xcalloc(1, sizeof(Module));
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
    g_mods = xcalloc((size_t)g_cmods, sizeof(Module *));
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
Node *g_ct_stack[128];
