/* sema_call.c - 调用检查：分配器去虚拟化 / 调用点对账 / 泛型实例。
 * 从 sema.c 拆分；共享声明见 sema_internal.h。
 */
#include "sema_internal.h"

static void reject_borrow_arg(Node *arg) {
    diag_code_at("E0532", arg->line, arg->col,
        "cannot pass a captured pointer as a function argument: it is a "
        "borrow that must not escape the capture scope");
}

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
        /* §224：显式调用不偷偷补 &——实参是什么形态就传什么。 */
        for (int i = 0; i < n->nitems; i++) {
            Type *got = check_expr(n->items[i], mft->params[i]);
            coerce_node(n->items[i], got, mft->params[i], "argument");
            if (tag_expr_borrow(n->items[i])) {
                reject_borrow_arg(n->items[i]);
                return type_of(n, ty_invalid());
            }
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

    /* §274：不可变接收者在调用点转换，不改变方法签名或 ABI。
     * 指针调按值方法等价于显式 `.0`；值调只读指针方法等价于显式 `&`。 */
    if (want->kind != TY_PTR && rt && rt->kind == TY_PTR &&
        !rt->has_len && ty_eq(rt->elem, want)) {
        Node *deref = node_new(NK_FIELD, recv->line, recv->col);
        deref->a = recv;
        deref->ival = 0;
        deref->ty = rt->elem;
        arg0 = deref;
    } else if (want->kind == TY_PTR &&
               !(rt && rt->kind == TY_PTR && ty_eq(rt, want))) {
        if (rt && rt->kind == TY_PTR) {
            /* 已经是指针了：剩下的只是 const 的加减，交给普通转换 */
        } else if (rt->kind == TY_STRUCT || rt->kind == TY_UNION ||
                   rt->kind == TY_ENUM || rt->kind == TY_TUPLE) {
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
    }
    if (coerce_node(arg0, arg0->ty, want, "receiver")->kind == TY_INVALID)
        return type_of(n, ty_invalid());
    if (want->kind == TY_PTR && tag_expr_borrow(arg0)) {
        reject_borrow_arg(arg0);
        return type_of(n, ty_invalid());
    }

    int nargs = n->nitems;
    if (nargs + 1 != mft->nparams)
        return BAD(n, "method takes %d argument(s), %d given", mft->nparams - 1, nargs);

    Node **items = xcalloc((size_t)(nargs + 1), sizeof(Node *));
    items[0] = arg0;
    for (int i = 0; i < nargs; i++) items[i + 1] = n->items[i];
    free(n->items);
    n->items  = items;
    n->nitems = nargs + 1;
    n->meth_done = true;

    for (int i = 1; i < n->nitems; i++) {
        coerce_node(n->items[i], check_expr(n->items[i], mft->params[i]),
                    mft->params[i], "argument");
        if (tag_expr_borrow(n->items[i])) {
            reject_borrow_arg(n->items[i]);
            return type_of(n, ty_invalid());
        }
    }

    n->inst = fld->inst;
    n->ival = 0;
    /* §173 补丁七：方法调用走同一条贴标链——接收者就是实参 0，
     * 体内 free(self.field) 经 pff 事实账、返回 self 经返回流记账。 */
    tag_call_flow(n);
    return type_of(n, ret);
}

/* std.debug.panic / assert：把 文件 / 行 / 列 / 函数名 四个位置实参追加到实参
 * 尾部（decisions §85）。1.2.0 删掉了 @file/@line/@col/@fnName 四个内建，
 * 这里改为直接造字面量节点——效果一样（编译期常量、零运行时开销），
 * 但源码里已经没有任何位置内建可写了。 */
static void debug_inject_loc(Node *n) {
    int old = n->nitems;
    Node **items = xcalloc((size_t)(old + 4), sizeof(Node *));
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

Type *check_call(Node *n, Type *expect) {
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
            if (tag_expr_borrow(n->items[first])) {
                reject_borrow_arg(n->items[first]);
                return type_of(n, ty_invalid());
            }
            if (isfmt && tag_expr_borrow(n->items[0])) {
                reject_borrow_arg(n->items[0]);
                return type_of(n, ty_invalid());
            }
            for (int i = first + 1; i < n->nitems; i++) {
                check_expr(n->items[i], NULL);
                if (tag_expr_borrow(n->items[i])) {
                    reject_borrow_arg(n->items[i]);
                    return type_of(n, ty_invalid());
                }
            }
            check_fmt_args(n, first, n->nitems - first - 1);
            if (isfmt) return type_of(n, ty_err(ty_slice(ty_u8(), false)));
            return type_of(n, ty_void());
        }
        /* 分配 API（§245）：stackAlloc 返回 *[N]T（栈，不会 OOM）；
         * heapAlloc 返回 ?*[N]T、heapSlice 返回 ?[]T——判空是使用者的事，
         * 运行时不再自行 abort（原 zx_heapalloc 的 abort 已删）。N 仍在
         * 类型里（flat optional 的 C 表示就是那根指针），§130 的编译期
         * 合并、tag 建槽、逃逸检查全部照旧，只是外面多一层 optional。 */
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
                /* §245：?[]T——分配失败返回 null，使用者自己判空 */
                return type_of(n, ty_opt(ty_slice(elem, false)));
            }
            IntV c = ev_int(n->items[base + 1]);
            if (!c.ok)
                return BAD(n->items[base + 1],
                           "std.%s count must be known at compile time", m->name);
            if (!stack) {
                if (!n->reg_slot) {
                    int sz = ty_size(elem), al = ty_align(elem);
                    n->reg_slot = region_new(n,
                        (uint64_t)(sz > 0 ? sz : 1) * c.v,
                        (uint64_t)(al > 0 ? al : 1));
                }
                /* §187（唯一入口）：heapAlloc/heapSlice 是 allocator.alloc
                 * 的类型化糖，内部同一入口——建槽；allocator_exempt
                 * (arena/debug_alloc 这类整批清算的分配器) → 不建槽。 */
                if (!allocator_exempt(n->items[0]))
                    tag_auto_decl(n, false);
            } else {
                tag_auto_decl(n, true);         /* §173：栈分配也建槽，
                                                 * 作用域退出自动标 free */
            }
            /* §201 + §245：heapAlloc 返回 ?*[N]T（flat optional，C 表示就是
             * 那根指针），stackAlloc 不会失败仍返回 *[N]T。elem 就是 [N]T。 */
            Type *ret = ty_ptr(ty_array(elem, c.v), false, true, c.v);
            if (!stack) ret = ty_opt(ret);
            return type_of(n, ret);
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
            if (tag_expr_borrow(n->items[i])) {
                diag_code_at("E0532", n->items[i]->line, n->items[i]->col,
                    "cannot pass a captured pointer as a function argument: it is a "
                    "borrow that must not escape the capture scope");
                return type_of(n, ty_invalid());
            }
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
            if (tag_expr_borrow(n->items[0])) {
                reject_borrow_arg(n->items[0]);
                return type_of(n, ty_invalid());
            }
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
            if (tag_expr_borrow(n->items[0]) || tag_expr_borrow(n->items[1])) {
                reject_borrow_arg(n->items[tag_expr_borrow(n->items[0]) ? 0 : 1]);
                return type_of(n, ty_invalid());
            }
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
        for (int i = 0; i < n->nitems; i++) {
            if (i >= 2) check_expr(n->items[i], NULL);
            if (tag_expr_borrow(n->items[i])) {
                reject_borrow_arg(n->items[i]);
                return type_of(n, ty_invalid());
            }
        }
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
        /* §221：捕获指针 *t 一律不许作为实参传出（用户裁定）——它是对
         * 原对象的栈借用，函数可能把它存到比捕获作用域更长的地方。 */
        if (tag_expr_borrow(n->items[i])) {
            diag_code_at("E0532", n->items[i]->line, n->items[i]->col,
                "cannot pass a captured pointer as a function argument: it is a "
                "borrow that must not escape the capture scope");
            return type_of(n, ty_invalid());
        }
    }
    /* §173：贴标载体过调用点（实参释放判定 / 返回流 / 字段账） */
    tag_call_flow(n);
    /* 函数指针字段的分发：能证明目标就改成直调（§138） */
    devirt_allocator_call(n);
    return type_of(n, ct->ret ? ct->ret : ty_void());
}
static TagDefer *g_tdefer;
int g_ntdefer, g_ctdefer;

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

void tag_call_flow(Node *n) {
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
                    char abp2[64]; abp2[0] = 0;
                    for (int g = 0; g < 6 && aa &&
                         (aa->kind == NK_CAST || aa->kind == NK_UNWRAP ||
                          aa->kind == NK_SLICE ||
                          (aa->kind == NK_FIELD && aa->name &&
                           !strcmp(aa->name, "ptr"))); g++)
                        aa = aa->kind == NK_CAST ? (aa->b ? aa->b : aa->a)
                            : aa->kind == NK_FIELD ? aa->a : aa->a;
                    Symbol *abs2 = agt_base_of(aa, abp2, sizeof abp2);
                    if (abs2) {
                        for (int k = 0; k < 32; k++) {
                            char nk[8];
                            snprintf(nk, sizeof nk, "%d", k);
                            if (!pff_has(ps, nk)) continue;
                            int es = aesc_find(abs2, abp2, k);
                            if (es) ft_mark_freed(es, n);
                        }
                        if (pffa_has(ps)) agt_clear(abs2, abp2, n);
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
void tag_deferred_replay(void) {
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
            char abp3[64]; abp3[0] = 0;
            for (int g = 0; g < 6 && aa &&
                 (aa->kind == NK_CAST || aa->kind == NK_UNWRAP ||
                  aa->kind == NK_SLICE ||
                  (aa->kind == NK_FIELD && aa->name &&
                   !strcmp(aa->name, "ptr"))); g++)
                aa = aa->kind == NK_CAST ? (aa->b ? aa->b : aa->a)
                    : aa->kind == NK_FIELD ? aa->a : aa->a;
            Symbol *abs2 = agt_base_of(aa, abp3, sizeof abp3);
            if (abs2) {
                for (int k = 0; k < 32; k++) {
                    char nk[8];
                    snprintf(nk, sizeof nk, "%d", k);
                    if (!pff_has(ps, nk)) continue;
                    int es = aesc_find(abs2, abp3, k);
                    if (es) ft_mark_freed(es, d->call);
                }
                if (pffa_has(ps)) agt_clear(abs2, abp3, d->call);
            }
        }
        g_srcpath = sp;
    }
}

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
void tag_transform_deinit(Node *call) {
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
    Node **items = xcalloc(3, sizeof(Node *));
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
        bool *seen = xcalloc((size_t)(t->nfields ? t->nfields : 1), sizeof(bool));
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
void tuple_stamp_ftags(Node *n) {
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
    Type **items = xcalloc((size_t)(n->nitems ? n->nitems : 1), sizeof(Type *));
    for (int i = 0; i < n->nitems; i++) items[i] = check_expr(n->items[i], NULL);
    Type *t = ty_tuple(items, n->nitems);
    free(items);
    n->ty = t;
    tuple_stamp_ftags(n);
    if (expect && !ty_eq(expect, t) && expect->kind != TY_VOID)
        coerce_node(n, t, expect, "value");
    return t;
}

static Type *check_builtin(Node *n, Type *expect) {
    (void)expect;
    const char *nm = n->name ? n->name : "";

    if (!strcmp(nm, "type")) {
        /* 唯一的内建：@type(x) → 编译期描述符
         *   { .name, .size, .align, .ty, .kind }  (kind: union(enum), §257)
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
        /* §255：`@type(info.params[i])` 这种「从编译期类型值切片里取元素」
         * 的节点没有 type_val，只有编译期 V_TYPE。折一次再取，反射才能嵌套。 */
        if (!t || t->kind == TY_INVALID) {
            Value *tv = ct_eval(n->items[0]);
            if (tv && tv->kind == V_TYPE) t = tv->tval;
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
            { "hasField", "use @type(T).kind.Struct.fields" },
            { "fieldCount","use @type(T).kind.Struct.fieldCount" },
            { "fieldType","use @type(T).kind.Struct.fields[i].ty" },
            { "fieldName","use @type(T).kind.Struct.fields[i].name" },
            { "offsetOf", "use @type(T).kind.Struct.fields[i].offset" },
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

/* §229: split from Type *check_expr_raw(Node *n, Type *expect) { -- one static helper per case group. */
static Type *ce_00_nk_int(Node *n, Type *expect) {
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

}

static Type *ce_01_nk_float(Node *n, Type *expect) {
    {
        /* 显式后缀定型：f128 的十进制文本必须按 128 位解析，若先落回
         * comptime_float(double) 再转 f128，0.1f128 就只剩 53 位精度。 */
        Type *suf = NULL;
        const char *s = n->str;
        if (s) {
            size_t len = strlen(s);
            if (len >= 4 && !strcmp(s + len - 4, "f128")) suf = ty_float(128);
            else if (len >= 4 && !strcmp(s + len - 4, "f16"))  suf = ty_float(16);
            else if (len >= 4 && !strcmp(s + len - 4, "f32"))  suf = ty_float(32);
            else if (len >= 4 && !strcmp(s + len - 4, "f64"))  suf = ty_float(64);
        }
        if (suf) return type_of(n, suf);
        if (expect && ty_is_float(expect)) return type_of(n, expect);
        return type_of(n, ty_comptime_float());
    }

}

static Type *ce_02_nk_char(Node *n, Type *expect) {
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

}

static Type *ce_03_nk_str(Node *n) {
        /* 字符串字面量是**定长数组指针** `*[N]const u8`（N 是字节数，
         * 不含结尾 0），不是切片——长度挂在类型里，`.len` 是编译期常量，
         * 也挡得住把字面量当可写缓冲区。传给 `[]const u8` 形参时按
         * "*[N]T → []T" 的既有规则自动降级（can_coerce 已支持）。
         * 用户 2026-11-09 定：字符串等于 zx 的数组 *[N]const u8。 */
        return type_of(n, ty_ptr(ty_u8(), true, true, (uint64_t)n->str_len));

}

static Type *ce_04_nk_bool(Node *n) {
        return type_of(n, ty_bool());

}

static Type *ce_05_nk_null(Node *n, Type *expect) {
        /* 有落点就定型：`?T` 是原来的语义；`*T` / `*fn` 是 C 互操作——
         * C 的 NULL 就是 0 指针，没必要逼人写 `(*T)(0)`。没有落点仍然
         * 无类型（`let a = null;` 照旧报错），否则"null 是值"又活了。 */
        if (expect && (expect->kind == TY_OPT || expect->kind == TY_PTR ||
                       expect->kind == TY_FN || expect->kind == TY_ERROR))
            return type_of(n, expect);
        /* §240：`!T` 里 null 就是保留错误码 `error.null`（65535），不是
         * 载荷的空值。定型成 error（不是 `!T` 本身）才能走既有的
         * `error → !T` 提升与错误集检查；后端 coerce 的 NK_NULL 分支
         * 按目标 `!T` 发 `.tag = 65535`。这样 `return null;` 与
         * `return error.null;` 全程等价（含 E0901 声明集约束）。 */
        if (expect && expect->kind == TY_ERR) {
            n->ct = val_int(ty_error(), 65535, false);
            return type_of(n, ty_error());
        }
        return type_of(n, ty_null());

}

static Type *ce_06_nk_undef(Node *n, Type *expect) {
        if (expect) return type_of(n, expect);
        return type_of(n, ty_undef());

}

static Type *ce_07_nk_ident(Node *n) {
    {
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
        /* §192：闭包不支持。嵌套函数体里解析到**外层函数的**局部
         * （fn_d 更浅）——以前 sema 放行，后端发一个谁也不认识的 C 名，
         * 最后被包装成 B1001「编译器 bug」。在这里给人话。fn 值绑定
         * （`const F = fn …`）不是外层局部值：嵌套函数之间的兄弟引用合法，
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

}

static Type *ce_08_nk_bin(Node *n, Type *expect) {
    return check_binary(n, expect);
}

static Type *ce_09_nk_un(Node *n, Type *expect) {
    return check_unary(n, expect);
}

static Type *ce_10_nk_unwrap(Node *n) {

    /* `e.?` —— 解包。空（?T 的 none）/ 出错（!T 的 err）在运行时 trap，
     * 与整数除零同一个处置方式：先 flush，再打一行说明，然后 abort。
     * 它只给值，不是左值——`x.? = 1` 会被 check_assign 拦掉。 */
    {
        Type *t = check_expr(n->a, NULL);
        if (!t || t->kind == TY_INVALID) return ty_invalid();
        tag_check_deref(n->a);
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
}

static Type *ce_11_nk_try(Node *n) {
    {
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
}

static Type *ce_12_nk_try_capture(Node *n) {
    {
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

        ArmExit exOK = { 0 };
        tag_of_expr(n->a, &exOK.tag, &exOK.state);
        exOK.val = n->a;
        ArmExit exErr = { 0 };
        TagSnap snapT;
        tag_snap(&snapT);
        Type *bt;
        {
            Scope sc;
            scope_push(&sc);
            if (n->name) {
                Symbol *s = scope_add(&sc, n->name, SK_CAPTURE);
                if (s) { s->ty = ty_error(); s->state = 2;
                         s->in_fn = true;
                         s->storage_tag = tag_stack_new(ST_STORAGE, n->a, 0);
                         s->reg_slot = region_of_expr(n->a);
                         s->cap_src = n->a; }
            }
            g_arm_depth++;              /* §188：捕获臂同属释放禁区 */
            bt = check_block(n->b);
            g_arm_depth--;
            if (n->b->kind == NK_BLOCK) {
                exErr = tag_arm_reconcile(n->b->d, n->b, snapT.state, snapT.n, snapT.total);
            } else {
                exErr = (ArmExit){ 0 };
            }
            tag_scope_exit(&sc, n->b);
            tag_restore(&snapT);
            scope_pop(&sc);
        }
        /* 成功载荷与错误臂都可能携带既有标签，不能只在出现分配/释放
         * 事件时合并。LIVE⊔FREED(null 载体)→COND。 */
        tag_join_arms(n, exOK, exErr);
        tag_snap_free(&snapT);

        if (!bt || bt->kind == TY_INVALID) return ty_invalid();
        Type *r = unify(n, payload, bt);
        return type_of(n, r);
    }

    /* `a orelse b` —— 有值取载荷，空/错取 b。b 只在真的空/错时求值。 */
}

static Type *ce_13_nk_orelse(Node *n, Type *expect) {
    {
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
        ArmExit exLeft = { 0 };
        tag_of_expr(n->a, &exLeft.tag, &exLeft.state);
        exLeft.val = n->a;
        TagSnap snapO;
        tag_snap(&snapO);
        Type *alt = check_expr(n->b, elem);
        /* §200：`orelse` 的兜底臂不产出值时允许——臂末尾是 `return` /
         * `continue` / noreturn 调用 / 无限循环（永不落到块尾），它根本
         * 不会给表达式贡献值，整条 orelse 的值只能来自左臂载荷 elem。
         * check_block 对这种臂一律回 void，不能拿它去 coerce 载荷（会误报
         * E0301）。用 stmt_ends/falls_through 的同一套"末尾可达吗"判定。 */
        bool alt_exits = (alt && alt->kind == TY_NORETURN) ||
                         (n->b && n->b->kind == NK_BLOCK && !falls_through(n->b));
        /* §220：兜底臂末尾是收割（break / `tag: value`）时也算发散——
         * 它跳出当前匿名臂，不会给 orelse 贡献值。falls_through 不能全局
         * 认 yield（标签块本身要 fall through），所以在这里单独扫臂尾。
         * 单语句臂 `orelse break;` / `orelse out: v;` 与块臂 `{ break; }`
         * 都覆盖（后者递归进 NK_BLOCK 的 items）。 */
        if (!alt_exits && n->b) alt_exits = arm_ends_with_yield(n->b);
        ArmExit exRight = { 0 };
        if (!alt_exits && n->b) {
            Node *val = n->b->kind == NK_BLOCK ? n->b->d : n->b;
            exRight = tag_arm_reconcile(val, n->b, snapO.state, snapO.n, snapO.total);
        }
        tag_restore(&snapO);
        tag_join_arms(n, exLeft, exRight);
        tag_snap_free(&snapO);
        n->flag = alt_exits;         /* 后端据此跳过右支的 C 表达式位置 */
        if (!alt_exits)
            coerce_node(n->b, alt, elem, "`orelse` alternative");
        return type_of(n, elem);
    }
}

static Type *ce_14_nk_cast(Node *n) {
    return check_cast(n);
}

static Type *ce_15_nk_field(Node *n) {
    return check_field(n);
}

static Type *ce_16_nk_index(Node *n) {
    return check_index(n);
}

static Type *ce_17_nk_slice(Node *n) {
    return check_slice_expr(n);
}

static Type *ce_18_nk_tag_apply(Node *n, Type *expect) {

    {
        /* 表达式位置：贴标（tag.x : expr）或裸引用。开关与批量声明
         * （tag.enable / tag.batch）是语句位置的事（check_stmt）。 */
        if (!strcmp(n->name, "enable") || !strcmp(n->name, "batch")) {
            return BAD(n, "`tag.%s;` is a file-scope statement, not an expression",
                       n->name);
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

}

static Type *ce_19_nk_tag_bind(Node *n, Type *expect) {
    {
        
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
            char bp3[64]; bp3[0] = 0;
            Symbol *bv3 = agt_base_of(n->b->a, bp3, sizeof bp3);
            if (!bv3 && n->b->a->kind == NK_IDENT && n->b->a->sym &&
                n->b->a->sym->kind != SK_CONST) {
                bv3 = n->b->a->sym;     /* 标识符基址：路径为空串 */
            }
            int sid = (int)(t - g_tslots) + 1;
            int ix = aesc_const_index(n->b->b);
            if (ix >= 0) {
                /* 常索引续链：宣言建精确账，之后读这个元素走账，E0521
                 * 消失——追踪真正接回数组里。§226：字段数组 `s.arr[i]`
                 * 同一条路（键带 "arr" 路径）。 */
                if (bv3) aesc_add(bv3, bp3, ix, sid);
                t->arr_esc = true;
            } else {
                /* §185：运行期索引的注解读取——这是群成员的**取出**
                 * 通路：基址能解析到数组基变量且群非空时，校验
                 * tag.x ∈ 群，取出即销账（二次取出、取出后再取、
                 * 取没存过的标签都报 E0524）。基址解析不了（指针
                 * 别名）或群里没账（§175 旧断线场景）→ 保留旧语义：
                 * 宣言接管 pending，绑定正确性由使用者担保。 */
                if (bv3 && (agt_any(bv3, bp3) || agt_closed(bv3, bp3))) {
                    if (agt_has(bv3, bp3, sid)) {
                        agt_remove(bv3, bp3, sid);
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

}

static Type *ce_20_nk_call(Node *n, Type *expect) {
    {
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
}

static Type *ce_21_nk_struct_lit(Node *n, Type *expect) {
    return check_struct_lit(n, expect);
}

static Type *ce_22_nk_array_lit(Node *n, Type *expect) {
    return check_array_lit(n, n->items, n->nitems, expect);
}

static Type *ce_23_nk_tuple(Node *n, Type *expect) {
    return check_tuple_lit(n, expect);
}

static Type *ce_24_nk_builtin(Node *n, Type *expect) {
    return check_builtin(n, expect);
}

static Type *ce_25_nk_anon_struct(Node *n, Type *expect) {

    {
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
            t->fields = xcalloc((size_t)(n->nitems ? n->nitems : 1), sizeof(Field));
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

}

static Type *ce_26_nk_module(Node *n) {
    {
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

}

static Type *ce_27_nk_fn(Node *n) {
    {
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
}

static Type *ce_28_nk_type_ident(Node *n) {
    {
        Type *t = resolve_type(n);
        n->type_val = t;
        return type_of(n, ty_type());
    }

}

static Type *ce_29_nk_block(Node *n, Type *expect) {
        return sema_construct(n, expect);

}

static Type *ce_30_nk_range(Node *n) {
        return BAD(n, "a range `lo..hi` is only valid inside `for (...)`");

}

static Type *ce_31_nk_defer(Node *n) {
        check_stmt(n);
        return type_of(n, ty_void());

}

static Type *ce_32_nk_pair(Node *n, Type *expect) {
        return check_expr(n->a, expect);

}

/* §252：`[N]v` —— 用同一元素 v 重复 N 次初始化数组。
 * 长度必须编译期可得；元素若求值为类型（`[N]T` 写法）则整体就是数组类型值
 *（顺带补上表达式位置原先不支持的 `[4]u8` 类型别名）；否则原地脱糖成
 * NK_ARRAY_LIT（clone N 份 v），把定型/发码/逃逸/ct 全交给现成路径。 */
static Type *ce_34_nk_array_fill(Node *n, Type *expect) {
    IntV lv = ev_int(n->a);
    if (!lv.ok || lv.neg)
        return BAD(n->a, "array length must be a non-negative compile-time integer");
    if (lv.v == 0 || lv.v > 0x3fffffffULL) {
        diag_code_at("E0305", n->a->line, n->a->col,
                     "array length must be 1..1073741823 (got %llu)",
                     (unsigned long long)lv.v);
        return ty_invalid();
    }

    Type *et = check_expr(n->b, NULL);
    if (!et || et->kind == TY_INVALID) return ty_invalid();

    /* 元素是类型值：`[N]T` 就是 T 的 N 元数组类型（值位置的类型）。
     * check_expr 对类型表达式返回 ty_type()（"这是类型"），它表示的那个
     * 类型挂在节点 b->type_val 上（见 ce_07/ce_28）。 */
    if (et->kind == TY_TYPE) {
        Type *elem = n->b->type_val;
        if (!elem) return BAD(n->b, "array element is not a valid type");
        Type *arr = ty_array(elem, lv.v);
        n->ty = ty_type();
        n->type_val = arr;
        return n->ty;
    }

    /* 元素是值：脱糖。flag=true → `.{…}` 值形式 [N]T；false → `&[N]v` 指针形式。 */
    int cnt = (int)lv.v;
    Node **items = xcalloc((size_t)cnt, sizeof(Node *));
    for (int i = 0; i < cnt; i++) items[i] = clone_tree(n->b);
    n->kind = NK_ARRAY_LIT;
    n->items = items;
    n->nitems = cnt;
    return check_array_lit(n, n->items, n->nitems, expect);
}

static Type *ce_33_default(Node *n) {
        return BAD(n, "unsupported expression here (node kind %d)", (int)n->kind);
}

Type *check_expr_raw(Node *n, Type *expect) {
    if (!n) return ty_void();

    switch (n->kind) {
    case NK_INT: return ce_00_nk_int(n, expect);
    case NK_FLOAT: return ce_01_nk_float(n, expect);
    case NK_CHAR: return ce_02_nk_char(n, expect);
    case NK_STR: return ce_03_nk_str(n);
    case NK_BOOL: return ce_04_nk_bool(n);
    case NK_NULL: return ce_05_nk_null(n, expect);
    case NK_UNDEF: return ce_06_nk_undef(n, expect);
    case NK_IDENT: return ce_07_nk_ident(n);
    case NK_BIN: return ce_08_nk_bin(n, expect);
    case NK_UN: return ce_09_nk_un(n, expect);
    case NK_UNWRAP: return ce_10_nk_unwrap(n);
    case NK_TRY: return ce_11_nk_try(n);
    case NK_TRY_CAPTURE: return ce_12_nk_try_capture(n);
    case NK_ORELSE: return ce_13_nk_orelse(n, expect);
    case NK_CAST: return ce_14_nk_cast(n);
    case NK_FIELD: return ce_15_nk_field(n);
    case NK_INDEX: return ce_16_nk_index(n);
    case NK_SLICE: return ce_17_nk_slice(n);
    case NK_TAG_APPLY: return ce_18_nk_tag_apply(n, expect);
    case NK_TAG_BIND: return ce_19_nk_tag_bind(n, expect);
    case NK_CALL: return ce_20_nk_call(n, expect);
    case NK_STRUCT_LIT: return ce_21_nk_struct_lit(n, expect);
    case NK_ARRAY_LIT: return ce_22_nk_array_lit(n, expect);
    case NK_TUPLE: return ce_23_nk_tuple(n, expect);
    case NK_BUILTIN: return ce_24_nk_builtin(n, expect);
    case NK_ANON_STRUCT: return ce_25_nk_anon_struct(n, expect);
    case NK_MODULE: return ce_26_nk_module(n);
    case NK_FN: return ce_27_nk_fn(n);
    case NK_TYPE_IDENT: return ce_28_nk_type_ident(n);
    case NK_PTR_TYPE: return ce_28_nk_type_ident(n);
    case NK_SLICE_TYPE: return ce_28_nk_type_ident(n);
    case NK_TUPLE_TYPE: return ce_28_nk_type_ident(n);
    case NK_STRUCT_TYPE: return ce_28_nk_type_ident(n);
    case NK_UNION_TYPE: return ce_28_nk_type_ident(n);
    case NK_ENUM_TYPE: return ce_28_nk_type_ident(n);
    case NK_OPT_TYPE: return ce_28_nk_type_ident(n);
    case NK_ERR_TYPE: return ce_28_nk_type_ident(n);
    case NK_VEC_TYPE: return ce_28_nk_type_ident(n);
    case NK_ERR_SET: return ce_28_nk_type_ident(n);
    case NK_BLOCK: return ce_29_nk_block(n, expect);
    case NK_IF: return ce_29_nk_block(n, expect);
    case NK_WHILE: return ce_29_nk_block(n, expect);
    case NK_FOR: return ce_29_nk_block(n, expect);
    case NK_SWITCH: return ce_29_nk_block(n, expect);
    case NK_RANGE: return ce_30_nk_range(n);
    case NK_DEFER: return ce_31_nk_defer(n);
    case NK_ERRDEFER: return ce_31_nk_defer(n);
    case NK_RETURN: return ce_31_nk_defer(n);
    case NK_CONTINUE: return ce_31_nk_defer(n);
    case NK_YIELD: return ce_31_nk_defer(n);
    case NK_DECL: return ce_31_nk_defer(n);
    case NK_PAIR: return ce_32_nk_pair(n, expect);
    case NK_ARRAY_FILL: return ce_34_nk_array_fill(n, expect);
    default: return ce_33_default(n);
    }
}

/* ---------------- 语句 ---------------- */

/* 不可达代码：同一条语句序列里，前一条已经把控制转移走，后面就再也到不了。
 * 判定复用缺返回分析（`stmt_blocks_next` 定义在它旁边），只在源码位置报一次——
 * 泛型单态化会把同一段身体检查很多遍。 */
int g_dead_diag[32][2];
