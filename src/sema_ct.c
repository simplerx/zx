/* sema_ct.c - 编译期求值机：常量折叠 / 泛型特化。
 * 从 sema.c 拆分；共享声明见 sema_internal.h。
 */
#include "sema_internal.h"

static int   g_ct_nstack;
static bool  g_ct_checking;

static bool ct_on_stack(Node *fn) {
    for (int i = 0; i < g_ct_nstack; i++)
        if (g_ct_stack[i] == fn) return true;
    return false;
}
static CtFlow ct_run_raw(Node *n);
static CtFlow ct_run_block(Node *b);
static CtFlow ct_run_arm(Node *n);

static CtFlow ct_normal(void) { CtFlow f = { CT_NORMAL, NULL, NULL }; return f; }

/* 只清"会执行语句"的子树：块 / if / while / for / switch 里可能烙着
 * 某一轮的中间值；普通表达式的折叠结果就是最终值，不能清。 */
void ct_clear_stmts(Node *n) {
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
void ct_clear(Node *n) {
    if (!n) return;
    n->ct = NULL;
    ct_clear(n->a); ct_clear(n->b); ct_clear(n->c); ct_clear(n->d);
    for (int i = 0; i < n->nitems; i++) ct_clear(n->items[i]);
}
static long g_ct_calls;

/* 深拷贝：泛型实例化要一棵属于自己的树，这样里面的 `struct{…}` 才有
 * 独立身份（ty_agg 用节点当键）。 */
Node *clone_tree(Node *n) {
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
bool ct_view_len(Value *v, Type *t, int64_t *len) {
    if (ty_is_array_ptr(t)) { *len = (int64_t)t->elem->len; return true; }
    /* §201：[N]T 数组值的长度写在类型里，`arr[0][1..]` 编译期切片要用 */
    if (t && t->kind == TY_ARRAY) { *len = (int64_t)t->len; return true; }
    if (t && t->kind == TY_PTR && t->has_len) { *len = (int64_t)t->len; return true; }
    if (!v) return false;
    if (v->kind == V_STR) { *len = v->slen; return true; }
    if (v->kind == V_PTR && v->base) { *len = (int64_t)v->nitems; return true; }
    return false;
}

Value *ct_field(Node *n) {
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

Value *ct_index(Node *n) {
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

Value *ct_call(Node *n) {
    if (++g_ct_calls > CT_MAX_CALLS) { g_ct_stuck = true; return NULL; }

    Value *callee = ct_eval(n->a);
    if (!callee || callee->kind != V_FN || !callee->fn) return NULL;
    /* 泛型调用在 check_call 里已经单态化：n->inst 是绑定好 T 的实例，
     * callee->fn 只是原始模板。模板签名里的 T 只在 specialize_raw 的临时
     * 作用域里有意义，clone 后拿它在 ct_call 的模块作用域里重新解析必然
     * 落空，报假的 E0401 unknown type 'T'——所以折叠泛型调用时绝不 clone
     * 模板（也不 clone 实例：那会把实例体里的方法/局部重新检查一遍，报假的
     * unused parameter）。实例在 specialize_raw 里已经算好签名并跑过体：
     * 类型构造函数直接取它的返回值，其余泛型调用不参与编译期折叠。 */
    if (fn_is_generic(callee->fn)) {
        if (!n->inst) return NULL;
        Type *tv = instance_result(n->inst);
        return tv ? val_type(tv) : NULL;
    }

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

Value *ct_eval(Node *n) {
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
    case NK_FLOAT:
        v = val_float_lit(n->ty ? n->ty : ty_comptime_float(), n->str, n->fval);
        break;
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
CtFlow ct_run(Node *n) {
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

Value *fold(Node *n) {
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
Value *ct_eval_probe(Node *n) {
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
Value *fold_expr(Node *n) {
    switch (n->kind) {
    case NK_BLOCK: case NK_IF: case NK_WHILE: case NK_FOR:
    case NK_SWITCH: case NK_DECL: case NK_RETURN: case NK_CONTINUE:
    case NK_YIELD: case NK_DEFER:
        return NULL;
    default:
        return fold(n);
    }
}

Type *check_expr(Node *n, Type *expect) {
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

bool fn_is_generic(Node *fn) {
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
Value *arg_ct(Node *a, Type *want) {
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
Node *specialize(Node *fn, Node *call, int *ncomp, Type **out_ft, Value **out_val) {
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
void name_type_from_call(Node *call, Type *result) {
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
Type *instantiate_type(Node *call) {
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
Type *instantiate_fn_type(Node *call) {
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
