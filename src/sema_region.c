/* sema_region.c - 分配区间账本与句柄证明账本。
 * 从 sema.c 拆分；共享声明见 sema_internal.h。
 */
#include "sema_internal.h"


RegionSlot *g_rslots;
int         g_nrslots, g_crslots;
int         g_flow_depth;   /* 处在 if/while/for/switch 臂里 */
int         g_region_gid;   /* 合并组编号，从 1 起 */

int region_new(Node *site, uint64_t size, uint64_t align) {
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

void region_bind(int id, Symbol *s) {
    if (id > 0 && id <= g_nrslots) g_rslots[id - 1].sym = s;
}

/* 从表达式追回发行版本。只保住「地址没变」的传播：赋值、指针→指针
 * 强转、.?, 指针算术；聚合字段取回 / 全局 / 指针↔整数 / C 边界一律断。 */
int region_of_expr(Node *e) {
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

bool is_allocator_ty(Type *t) {
    if (!t) return false;
    if (t == sema_allocator_type()) return true;
    return t->kind == TY_STRUCT && t->name && !strcmp(t->name, "Allocator");
}

/* Allocator.free(ctx, ptr, size) 是一次作废事件。只在直线代码里迁移，
 * 且 provenance 没断才判；其余一律标 opaque 交运行时 side table。 */
void region_note_free(Node *call) {
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
void region_plan_block(Node *blk) {
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

ProvePoint *g_proves;
int         g_nproves, g_cproves;
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

void region_handle_call(Node *n) {
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
