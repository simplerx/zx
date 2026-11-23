/* sema_stmt.c - 语句检查：check_stmt / 名字别名 / 模块装载。
 * 从 sema.c 拆分；共享声明见 sema_internal.h。
 */
#include "sema_internal.h"

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

void check_stmts(Node *blk) {
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
void sym_link(Symbol *dst, Symbol *src) {
    bool pub = dst->is_pub;   /* 导出与否看别名自己那条 `pub`，不看被指向的那份 */
    ensure(src);
    sym_copy(dst, src, NULL);
    dst->is_pub  = pub;
    dst->state   = 2;
    dst->ct_bind = true;      /* 别名没有自己的 C 存储 */
}

Symbol *module_member(Module *m, const char *name) {
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
bool decl_is_lazy_module(Node *d) {
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

void hoist(Scope *sc, Node *blk) {
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

void ensure_raw(Symbol *s) {
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
    if (d->b) {
        s->stack_val = expr_frame(d->b);   /* 初值指向当前栈帧？记下来 */
        s->cap_borrow = expr_borrow(d->b); /* §221：别名继承捕获指针借用 */
    }

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
char *path_normalize(const char *p) {
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

void sym_copy(Symbol *dst, Symbol *src, const char *name) {
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

Module *module_load_std(int line, int col) {
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
void ensure(Symbol *s) {
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

Type *check_fn_body(Node *fn) {
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

Type *check_block(Node *n) {
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

/* §229: split from void check_stmt(Node *n) { -- one static helper per case group. */
static void cs_00_nk_decl(Node *n) {
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

}

static void cs_01_nk_tag_apply(Node *n) {
        /* 语句位置：tag.enable / tag.batch / 独立贴标（§225：tag.close 已删） */
        if (!strcmp(n->name, "enable")) {
            /* §225：账本默认关闭，这一行才让本模块的标签系统生效——
             * 建槽、泄漏 E0513、双放 E0509、UAF E0512、分支确认 E0528、
             * 续链 E0521、显式贴标全部按模块独立生效，不影响其他模块。 */
            if (n->b)
                err_at(n->line, n->col, "`tag.enable` takes no value");
            else if (g_mod)
                g_mod->tags_enabled = true;
            return;
        }
        if (!strcmp(n->name, "close")) {
            /* §225：默认已经关闭，tag.close 这个逃生口没有存在意义了。 */
            diag_code_at("E0211", n->line, n->col,
                "`tag.close` is gone -- the tag ledger is off by default now "
                "(§225); delete this line, or write `tag.enable;` to turn "
                "the ledger on for this module");
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

}

static void cs_02_nk_tag_bind(Node *n) {
    {
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
                /* §226：路径传 NULL = 通配，把该载体的所有数组字段群
                 * 一并终结（`tag s : null;` 是整载体出口，与 §189
                 * 「结构体值载体的字段账一并收集」同口径）。 */
                agt_clear(s, NULL, n);
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
            /* §225：账本休眠（没写 tag.enable）或豁免模块（std 实现层 /
             * tag.batch）压根不建槽，确认/终结标记无处可挂——按 no-op
             * 处理，不报"没有这个分配"。§189 口径不变。 */
            if (!g_mod || !g_mod->tags_enabled || g_mod->tags_batch ||
                tag_module_exempt(g_mod))
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

}

static void cs_03_nk_fn(Node *n) {
        if (n->sym) ensure(n->sym);
        else { resolve_fn_type(n); if (!n->flag && n->b && !fn_is_generic(n)) schedule_body(n); }
        return;

}

static void cs_04_nk_return(Node *n) {
    {
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
            /* §221：捕获指针 *t 是栈借用，绝不许作为返回值带出作用域。
             * 独立于 tag 账本——账本开关（§225）挡不住这条。 */
            if (expr_borrow(n->a)) {
                diag_code_at("E0532", n->a->line, n->a->col,
                    "cannot return a captured pointer `*t`: it is a borrow into the "
                    "enclosing object and must not outlive the capture scope");
                return;
            }
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

}

static void cs_05_nk_continue(Node *n) {
    {
        Label *l = label_find_loop(n->label);
        if (!l) {
            if (n->label) err_at(n->line, n->col, "no enclosing loop labeled '%s'", n->label);
            else err_at(n->line, n->col, "`continue` outside a loop");
        }
        return;
    }

}

static void cs_06_nk_yield(Node *n) {
    {
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

}

static void cs_07_nk_defer(Node *n) {
    {
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

}

static void cs_08_default(Node *n) {
    {
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

void check_stmt(Node *n) {
    if (!n) return;
    switch (n->kind) {
    case NK_DECL: cs_00_nk_decl(n); return;
    case NK_TAG_APPLY: cs_01_nk_tag_apply(n); return;
    case NK_TAG_BIND: cs_02_nk_tag_bind(n); return;
    case NK_FN: cs_03_nk_fn(n); return;
    case NK_RETURN: cs_04_nk_return(n); return;
    case NK_CONTINUE: cs_05_nk_continue(n); return;
    case NK_YIELD: cs_06_nk_yield(n); return;
    case NK_DEFER: cs_07_nk_defer(n); return;
    case NK_ERRDEFER: cs_07_nk_defer(n); return;
    default: cs_08_default(n); return;
    }
}

/* ---------------- 控制流结构（都是表达式） ---------------- */

Type *check_arm(Node *a, Type *expect) {
    if (!a) return ty_void();
    g_arm_depth++;                  /* §188：臂内是释放禁区（E0528） */
    Type *r = (a->kind == NK_BLOCK) ? check_block(a) : check_expr(a, expect);
    g_arm_depth--;
    return r;
}
