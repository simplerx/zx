/* sema_expr.c - 表达式检查：指针算术 / 逃逸 / 比较 / 成员 / 格式。
 * 从 sema.c 拆分；共享声明见 sema_internal.h。
 */
#include "sema_internal.h"


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

bool expr_frame(Node *e) {
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

/* §221：表达式是否携带捕获指针（*t）的栈借用。与 expr_frame 平行，但只认
 * cap_borrow 标记的符号——普通 &local 的栈指针不算（传给函数是合法的），
 * 只有捕获指针这套借用才禁传参/禁外泄。赋值/别名沿 NK_IDENT 传播（符号的
 * cap_borrow 由 check_assign 重算），cast/下标/字段读取保持来源。 */
bool expr_borrow(Node *e) {
    if (!e) return false;
    switch (e->kind) {
    case NK_IDENT: return e->sym && e->sym->cap_borrow;
    case NK_CAST:  return expr_borrow(e->a);   /* 指针 cast 仍是借用 */
    case NK_ORELSE: case NK_TRY:
        return expr_borrow(e->a) || expr_borrow(e->b);
    case NK_BIN:
        if (e->op == TOK_PLUS || e->op == TOK_MINUS)
            return expr_borrow(e->a) || expr_borrow(e->b);
        return false;
    /* 注意：NK_FIELD / NK_INDEX / NK_UNWRAP 是"穿过借用指针读载荷"——
     * 结果是载荷值，不是借用指针本身。t[0] / t.f 不携带借用，不能在这里
     * 传播，否则 `t[0] = t[0]+1` 会被误判成把借用存进数组元素。 */
    default: return false;
    }
}

bool addr_frame(Node *o) {
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
bool frame_in_agg(Node *e, int depth) {
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

Type *check_assign(Node *n) {
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
        /* §221：捕获指针借用不许存进长生命周期位置——字段、全局/静态、
         * 数组/切片元素。局部简单变量赋值是别名传播（上面会记 cap_borrow），
         * 不算逃逸。账本开关（§225）关不掉这条（走 expr_borrow 而非 tag 账本）。 */
        if (expr_borrow(n->b)) {
            bool escapes = false;
            const char *where = "a struct field";
            if (n->a->kind == NK_FIELD) {
                escapes = true; where = "a struct field";
            } else if (n->a->kind == NK_INDEX || n->a->kind == NK_SLICE) {
                escapes = true; where = "an array/slice element";
            } else if (n->a->kind == NK_IDENT && n->a->sym && n->a->sym->is_top) {
                escapes = true; where = "a global or static variable";
            }
            if (escapes) {
                diag_code_at("E0532", n->b->line, n->b->col,
                    "cannot store a captured pointer `*t` into %s: it is a borrow "
                    "that must not outlive the capture scope", where);
                return type_of(n, ty_invalid());
            }
        }
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
        if (n->a->kind == NK_IDENT && n->a->sym) {
            n->a->sym->stack_val = expr_frame(n->b);
            /* §221：捕获指针借用沿别名传播（重算而非只置 true，赋成干净值即洗白） */
            n->a->sym->cap_borrow = expr_borrow(n->b);
        }
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
                    char bp2[64];
                    Symbol *bs2 = agt_base_of(fb->a, bp2, sizeof bp2);
                    TagSlot *ts = tag_slot(ftid2);
                    if (ts && bs2) {
                        agt_add(bs2, bp2, ftid2);
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
            /* §226：标识符基址直接取符号（路径为空串），其余
             * （含 `s.arr` 字段数组、别名切片）统一交给 agt_base_of，
             * 读写共用同一个解析，键口径才不会错位。 */
            char dbp[64]; dbp[0] = 0;
            Symbol *dbs = (n->a->a->kind == NK_IDENT)
                          ? (n->a->a->sym && n->a->a->sym->kind != SK_CONST
                             ? n->a->a->sym : NULL)
                          : agt_base_of(n->a->a, dbp, sizeof dbp);
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
                        agt_add(dbs, dbp, ftid);
                        if (ts) ts->arr_esc = true;
                    } else {
                        /* 常索引：aesc 精确账已覆盖（覆盖写入换账、
                         * 精确读/free）；不进群——群是"运行期索引"
                         * 成员的集合摘要，常索引的容器终结由
                         * agt_clear 扫 aesc 统一覆盖。 */
                        aesc_add(dbs, dbp, ix, ftid);
                        if (ts && ftst == TC_LIVE && n->b) {
                            ts->own_line = n->b->line; ts->own_col = n->b->col;
                        }
                    }
                }
            } else if (n->a->a) {
                /* §226：基址解析不了（指针别名、裸指针字段等）——追踪
                 * 必断线，保守记 pending（§174 口径不变：续链接管或终结，
                 * 没处理终扫报 E0521——标签绝不静默丢失）。旧写法只在
                 * 基址是标识符时兜底；字段数组现在能被 agt_base_of 解析，
                 * 真解析不了的场合反而只剩非标识符形态，兜底面相应放宽，
                 * 避免标签静默丢失。 */
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
bool f64_fits_int(double v, int bits, bool sign) {
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

Type *check_binary(Node *n, Type *expect) {
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

Type *check_array_lit(Node *n, Node **items, int nitems, Type *expect) {
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

Type *check_unary(Node *n, Type *expect) {
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

Type *check_cast(Node *n) {
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
                   && g_mod && g_mod->tags_enabled) {   /* §225：账本开启才要求贴标 */
            /* §187：整数转指针需显式贴标（与数组取出同一口径）——这个
             * 整数曾由指针转来，出处已丢（算术洗源/赋值重绑），转回
             * 指针等于凭空造出一个"不知道是谁的"指针：双重释放、
             * 释放别人的内存都查不了账。续链接管：
             *   tag v : tag.x = (*T)n;
             * 干净整数（从未接触指针：字面量、C FFI 句柄）照旧放行，
             * 账本不追。tag.batch 的分配器实现模块自己管账（free
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
void *cur_errset(void) {
    return (g_ret && g_ret->kind == TY_ERR) ? g_ret->eset : NULL;
}

/* 表达式可能抛出的错误集：
 *   - 类型是 `E!T` → E
 *   - `error.name` 字面量 → {name}
 *   - 其余（`!T` anyerror、`(error)Enum.x` 旧写法）→ NULL（未知，不检查） */
void *expr_errset(Node *e, Type *t) {
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
const char *errset_missing(const void *a, const void *b) {
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

Type *check_field(Node *n) {
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

Type *check_index(Node *n) {
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

Type *check_slice_expr(Node *n) {
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
        /* §222：用户裁定不再限制 at() 单元素指针造长切片——`p[0..n]`
         * 合法，长度完全由写码者负责（越界后果自负），编译器不拦。 */
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
Symbol *ns_member_symbol(Node *field) {
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

bool callee_is_generic(Node *callee) {
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
Type *instance_result(Node *g) {
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
void check_fmt_args(Node *call, int first, int nargs) {
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
bool addr_ok(Node *e) {
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

bool ty_is_allocator(Type *t) {
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
Symbol *synthetic_fn_sym(const char *name) {
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
Symbol *allocator_target(Node *e, bool want_alloc, int depth) {
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
        g_devirt_retry = xrealloc(g_devirt_retry,
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
void devirt_allocator_call(Node *n) {
    if (!n || !n->a || n->a->kind != NK_FIELD) return;
    Node *field = n->a;
    if (!g_devirt_final) devirt_retry_add(n, field);
    /* §187（用户裁定）：allocator.alloc() 是建槽的唯一入口——所有
     * 分配都经过它（heapAlloc/heapSlice 是它的类型化糖，同一入口）。
     * §225：账本默认关闭——建槽门控收在 tag_decl/tag_auto_decl 里，
     * 没写 tag.enable 的调用点模块直接落空。这里再豁免两条：
     * ① std 实现层调用点（内部的分配由各容器的 init/deinit 配对管）；
     * ② 分配目标模块是 tag.batch 批量清算型（arena 整批管理）。 */
    if (field->name && !strcmp(field->name, "alloc") && !n->xform_free &&
        n->nitems == 2 && field->a && field->a->ty &&
        ty_is_allocator(field->a->ty) &&
        !(g_mod && tag_module_exempt(g_mod)) &&
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
void devirt_retry_all(void) {
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
