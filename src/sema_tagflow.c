/* sema_tagflow.c - 标签流：返回流 / 臂合并 / 循环配平 / 泄漏终扫。
 * 从 sema.c 拆分；共享声明见 sema_internal.h。
 */
#include "sema_internal.h"


/* 检查并落账一次 std.move。返回值 = 转移后的类型（实参类型原样）。 */
Type *check_std_move(Node *call) {
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
bool tag_block_has_free(Node *n) {
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

bool fn_is_releaser(Node *fn) {
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
int fn_ret_cache(Node *fn, int *k_out, int *slot_out) {
    *k_out = -1; *slot_out = 0;
    if (!fn || fn->kind != NK_FN || !fn->b) return 0;
    if (fn->tag_state == 0 && fn->index == 0) return -1;   /* 未分析 */
    if (fn->tag_state > 0) *k_out = fn->tag_state - 1;
    if (fn->index > 0) *slot_out = fn->index;
    return 1;
}

/* body 查完时烙返回流缓存（此时 cast 烙印/局部符号已就位）。 */
void fn_stamp_ret_flow(Node *fn) {
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
int tag_check_apply(Node *n, bool *bad) {
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
    /* §245：heapAlloc 返回 ?*[N]T，贴标写法 `tag.x : alloc(...).?` 的
     * n->b 是 NK_UNWRAP。判空是使用者的事——`.?` 只是使用者选择 trap，
     * 底层 alloc 站点/自动槽不变，这里穿透 unwrap（含 coerce 插的空 cast）
     * 取回它，否则 rid=0 会误报 E0514。 */
    Node *alloc_src = n->b;
    while (alloc_src && (alloc_src->kind == NK_UNWRAP || alloc_src->kind == NK_CAST))
        alloc_src = alloc_src->a;
    int rid = (alloc_src && alloc_src->kind == NK_CALL) ? alloc_src->reg_slot : 0;

    if (rid > 0) {
        /* §173：alloc 调用点已自动建槽 —— 显式贴标收编它：自动槽退回
         * idle，分配事实记到具名槽上（自动建槽只剩兜底作用）。
         * §245：从穿透 .? 后的 alloc_src 取 tag_id，不是 n->b。 */
        if (alloc_src->tag_id) {
            TagSlot *a = tag_slot(alloc_src->tag_id);
            if (a && a->auto_n && a->state == TST_LIVE) {
                a->state = TST_UNBOUND;
                a->region = 0;
            }
        }
        /* 分配贴标：tag 必须空闲。tag 是单例标签，一次 LIVE。
         * 形参借出槽不记分配事实：`tag.p : alloc` = 解除借用，p 改挂
         * 分配自己的自动槽。 */
        if (t->abstract) {
            TagSlot *a = alloc_src->tag_id ? tag_slot(alloc_src->tag_id) : NULL;
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

/* §187：终局臂的 return 是否把槽传出。0 无 return；1 全部传出；
 * -1 有 return 没带（那条路径就是泄漏，E0514 照报）。 */
int arm_ret_scan(Node *nd, int slot) {
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
ArmExit tag_arm_reconcile(Node *armval, Node *armnode,
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
             * 报），不影响以后的 use/free。§225：账本默认关闭，休眠
             * 模块根本没有槽，这里只对 tag.enable 的模块生效；
             * std 实现层 / tag.batch 模块不受此限（账由容器内部闭环）。
             * 只对臂前就 LIVE 的槽把关——臂内新建的槽随臂消亡
             * （alloc/free 配对在臂内闭合是常事，无需确认）。 */
            if (before == TST_LIVE && !t->confirm_arm &&
                !(g_mod && (g_mod->tags_batch ||
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
void tag_join_arms(Node *n, ArmExit a, ArmExit b) {
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

void tag_snap(TagSnap *s) {
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
void tag_restore(const TagSnap *s) {
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
int ft_arm_collect(const TagSnap *s, Symbol **syms, FieldTag **fts, int cap) {
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
FieldTag *ft_arms_intersect(FieldTag *a, FieldTag *b) {
    FieldTag *out = NULL;
    for (FieldTag *f = a; f; f = f->next) {
        FieldTag *g = ft_find(b, f->name);
        if (g && g->slot == f->slot && f->slot > 0)
            ft_set(&out, f->name, f->slot);
    }
    return out;
}

void ft_arms_join(Symbol **s1, FieldTag **f1, int n1,
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
void tag_loop_balance(Node *body, const TagSnap *snap) {
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

bool tag_module_exempt(Module *m) {
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

void tag_ledger_final(const char *entry_path) {
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
     * 没有不被追踪的堆分配；没写 tag.enable 的模块在 tag_auto_decl 里跳过。 */
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
