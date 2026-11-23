/* sema_flow.c - 控制流：if/while/switch / 缺返回路径分析。
 * 从 sema.c 拆分；共享声明见 sema_internal.h。
 */
#include "sema_internal.h"


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

/* §221：捕获指针的前置校验。条件必须是可写左值——捕获的 *t 指向"原对象"
 * 的载荷存储，右值（函数调用结果、字面量）没有原对象可写回。
 * 成功返回指向载荷的指针类型 *T；失败报错并返回 NULL。 */
static Type *cap_ptr_prepare(Node *n, Type *cap_ty, bool is_opt_or_err) {
    if (!n->cap_ptr) return cap_ty;              /* 普通值捕获，原样 */
    if (!is_opt_or_err || !cap_ty || cap_ty->kind == TY_VOID) {
        err_at(n->line, n->col,
               "`*t` pointer capture needs a ?T / !T condition with a payload");
        return NULL;
    }
    if (!lvalue_mutable(n->a)) {
        err_at(n->a->line, n->a->col,
               "pointer capture `*t` needs an addressable, mutable condition "
               "(write `var o = ...; if (o) *t { ... }`; a temporary has no "
               "original object to write back to)");
        return NULL;
    }
    return ty_ptr(cap_ty, cap_ty->is_const, false, 0);
}

/* 给捕获符号打 §221 栈借用标记。stack_val 让通用 return 逃逸检查覆盖它，
 * cap_borrow 让 §221 专用出口检查（实参/字段/全局）拦它。 */
static void cap_mark_borrow(Symbol *s) {
    if (!s) return;
    s->cap_borrow = true;
    s->stack_val = true;
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
    /* §221：指针捕获——载荷类型换成 *T，条件必须是可写左值 */
    if (n->cap_ptr && n->name)
        cap_ty = cap_ptr_prepare(n, cap_ty, is_opt || is_err);

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
                     tag_capture_inherit(s, n->a, NULL);
                     if (n->cap_ptr) cap_mark_borrow(s); }
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
    /* §221：while 指针捕获 —— 载荷换 *T，条件必须可写左值 */
    if (n->cap_ptr && n->name)
        cap_ty = cap_ptr_prepare(n, cap_ty, is_opt || is_err);

    {
        Scope sc;
        scope_push(&sc);
        if (cap_ty && n->name) {
            Symbol *s = scope_add(&sc, n->name, SK_CAPTURE);
            if (s) { s->ty = cap_ty; s->state = 2;
                     tag_capture_inherit(s, n->a, NULL);
                     if (n->cap_ptr) cap_mark_borrow(s); }
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
            /* §221：switch 捕获指针 `pattern: *cap { … }` —— 载荷换 *T，
             * 主语必须是可写左值（有原对象可写回）。逐臂校验，错误每臂一次。 */
            if (arm->cap_ptr && cap_ty) {
                if (!lvalue_mutable(n->a)) {
                    err_at(n->a->line, n->a->col,
                           "pointer capture `*t` needs an addressable, mutable switch "
                           "subject (write `var s = ...; switch (s) { ... }`)");
                    cap_ty = NULL;
                } else {
                    cap_ty = ty_ptr(cap_ty, cap_ty->is_const, false, 0);
                }
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
                    if (arm->cap_ptr) cap_mark_borrow(cs);
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

/* 语句序列：一路走到能拦住的那条为止 */
bool falls_through(Node *blk) {
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

bool stmt_ends(Node *s) {
    switch (s->kind) {
    case NK_RETURN:
    case NK_CONTINUE:
        return true;
    case NK_YIELD:
        /* §220：`break`（label==NULL）跳出最近外循环，对当前块发散。
         * 带 label 的收割不能在这里算发散：它跳到标签块末尾，标签块本身
         * 收完值还会继续往下（见 stmt_blocks_next 的注释），全局认它会把
         * `out: {…}; return -1;` 的 return 误判成不可达。orelse 匿名兜底
         * 臂里的标签收割由 check_slice 的 alt_exits 单独识别。 */
        return s->label == NULL;
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

bool arm_ends(Node *a) {
    if (!a) return false;
    if (a->kind == NK_BLOCK) return !falls_through(a);
    return stmt_ends(a);
}

static bool yield_targets_local_block(const char *label, const YieldScope *sc) {
    if (!label) return false;                 /* break：永远是真发散 */
    for (; sc; sc = sc->up)
        if (sc->label && !strcmp(sc->label, label)) return true;
    return false;
}

static bool arm_ends_with_yield_r(Node *n, const YieldScope *sc) {
    if (!n) return false;
    if (n->kind == NK_YIELD) return !yield_targets_local_block(n->label, sc);
    if (n->kind == NK_BLOCK && n->nitems > 0) {
        YieldScope here;
        here.label = n->label;
        here.up = sc;
        return arm_ends_with_yield_r(n->items[n->nitems - 1],
                                     n->label ? &here : sc);
    }
    return false;
}

bool arm_ends_with_yield(Node *n) {
    return arm_ends_with_yield_r(n, NULL);
}

/* 不可达代码用的判定：比 `stmt_ends` 多认一个"收割"（`tag: expr;` 会跳出所在块），
 * 其余沿用缺返回分析的结论。标签块不能被判成"到不了后面"——它收完值就接着往下走，
 * 而 `falls_through` 不认收割，正好保守。 */
bool stmt_blocks_next(Node *s) {
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

void check_return_paths(Node *fn, Type *ret) {
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

Type *sema_construct(Node *n, Type *expect) {
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

Type *coerce_node(Node *e, Type *from, Type *to, const char *ctx) {
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
Node *deps_fn_of_name(const char *name, Scope *ps) {
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
Node *deps_fn_of_field(Node *fld, Scope *ps) {
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
