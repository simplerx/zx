/* sema_tag.c - 标签账本：槽位 / 字段账 / 数组群 / 释放事件。
 * 从 sema.c 拆分；共享声明见 sema_internal.h。
 */
#include "sema_internal.h"


const char *g_cur_fn_name;   /* 当前函数名（7963 附近保存/还原） */
TagSlot *g_tslots;
int g_ctslots;
TagCallEdge *g_cedges;
int g_ncedges, g_ccedges;

void tag_call_edge(const char *callee, const char *caller,
                          int line, int col) {
    if (!callee || !*callee || !caller || !*caller) return;
    if (!strcmp(callee, caller)) return;            /* 递归自调用不上链 */
    for (int i = 0; i < g_ncedges; i++)
        if (g_cedges[i].line == line && g_cedges[i].col == col &&
            !strcmp(g_cedges[i].callee, callee) &&
            !strcmp(g_cedges[i].caller, caller)) return;
    if (g_ncedges == g_ccedges) {
        int nc = g_ccedges ? g_ccedges * 2 : 32;
        TagCallEdge *p = realloc(g_cedges, (size_t)nc * sizeof *p);
        if (!p) return;
        g_cedges = p;
        g_ccedges = nc;
    }
    g_cedges[g_ncedges].callee = callee;
    g_cedges[g_ncedges].caller = caller;
    g_cedges[g_ncedges].line = line;
    g_cedges[g_ncedges].col = col;
    g_ncedges++;
}

Module *g_std_mod;       /* 5931 附近定义：std 模块（豁免判定用） */

TagSlot *tag_slot(int id) {       /* id = TagSlot 下标 + 1 */
    return (id > 0 && id <= g_ntslots) ? &g_tslots[id - 1] : NULL;
}

TagSlot *tag_by_name(const char *name) {
    for (int i = 0; i < g_ntslots; i++)
        if (!strcmp(g_tslots[i].name, name)) return &g_tslots[i];
    return NULL;
}

FieldTag *ft_find(FieldTag *list, const char *name) {
    for (; list; list = list->next)
        if (list->name && name && !strcmp(list->name, name)) return list;
    return NULL;
}

void ft_set(FieldTag **plist, const char *name, int slot) {
    FieldTag *f = ft_find(*plist, name);
    if (f) { f->slot = slot; return; }
    f = malloc(sizeof *f);
    if (!f) return;
    f->name = name; f->slot = slot; f->next = *plist;
    *plist = f;
}

FieldTag *ft_clone(FieldTag *src) {
    FieldTag *out = NULL, **tail = &out;
    for (; src; src = src->next) {
        FieldTag *f = malloc(sizeof *f);
        if (!f) break;
        f->name = src->name; f->slot = src->slot; f->next = NULL;
        *tail = f; tail = &f->next;
    }
    return out;
}

/* §175：聚合抽取（let inner = tt.0）——取母账里 key 前缀的子树，
 * 剥掉前缀返回新链（"0.0"→槽 变 "0"→槽）。找不到返回 NULL。 */
FieldTag *ft_subtree(FieldTag *list, const char *key) {
    FieldTag *out = NULL, **tail = &out;
    size_t kl = strlen(key);
    for (; list; list = list->next) {
        if (!list->name) continue;
        const char *rest = NULL;
        if (strncmp(list->name, key, kl) == 0 && list->name[kl] == '.')
            rest = list->name + kl + 1;
        else if (strcmp(list->name, key) == 0)
            rest = "";
        if (!rest) continue;
        FieldTag *f = malloc(sizeof *f);
        if (!f) break;
        f->name = xstrdup(*rest ? rest : key);
        f->slot = list->slot; f->next = NULL;
        *tail = f; tail = &f->next;
    }
    return out;
}

/* §175：清掉 key 及其子树（"key" 与 "key.xxx"）——整段值被覆盖时
 * 旧条目作废，防止后续读到陈旧槽。 */
void ft_clear_prefix(FieldTag *list, const char *key) {
    size_t kl = strlen(key);
    for (; list; list = list->next) {
        if (!list->name) continue;
        if (!strncmp(list->name, key, kl) &&
            (list->name[kl] == '\0' || list->name[kl] == '.'))
            list->slot = 0;
    }
}
void ft_mirror(Symbol *s, const char *key, int slot, int depth) {
    if (!s || !s->ft_src || !key || !key[0] || depth > 8) return;
    char path[128];
    Node *fb = NULL;
    if (!ft_path_key(s->ft_src, path, sizeof path, &fb)) path[0] = 0;
    if (!fb || fb->kind != NK_IDENT || !fb->sym) return;
    Symbol *b2 = fb->sym;
    char full[160];
    if (path[0]) snprintf(full, sizeof full, "%s.%s", path, key);
    else snprintf(full, sizeof full, "%s", key);
    if (slot > 0) {
        ft_clear_prefix((FieldTag *)b2->ftags, full);
        char *pk = ft_path_dup(path[0] ? path : NULL, key);
        if (pk) ft_set((FieldTag **)&b2->ftags, pk, slot);
    } else {
        ft_clear_prefix((FieldTag *)b2->ftags, full);
    }
    ft_mirror(b2, full, slot, depth + 1);
}

/* 合并两条 return 路径的字段表；同名字段槽不一致 = 状态不一致，丢弃。 */
FieldTag *ft_merge(FieldTag *a, FieldTag *b) {
    FieldTag *out = NULL;
    for (FieldTag *f = a; f; f = f->next) {
        FieldTag *g = ft_find(b, f->name);
        if (g && g->slot != f->slot) continue;
        ft_set(&out, f->name, f->slot);
    }
    for (FieldTag *f = b; f; f = f->next)
        if (!ft_find(a, f->name)) ft_set(&out, f->name, f->slot);
    return out;
}

/* 字段槽当前载体状态：从槽现推。 */
static int ft_state_of(int slot) {
    TagSlot *t = tag_slot(slot);
    if (!t || t->state == TST_UNBOUND) return TC_NONE;
    if (t->state == TST_MOVED) return TC_MOVED;   /* §189 */
    return t->state == TST_FREED ? TC_FREED : TC_LIVE;
}

/* §173 补丁八：字段链折叠成点路径 key。e 是最内层 FIELD
 * （s.inner.name 的 .name），沿 ->a 收集外层字段名，拼成
 * "inner.name"；最深基址（IDENT/字面量/调用）经 base_out 带回。
 * 返回 false = 不是纯字段链（深度超限/中途出现其他节点）。 */
bool ft_path_key_d(Node *e, char *buf, size_t cap, Node **base_out,
                          bool *deref_tail) {
    const char *names[8];
    char idxbuf[8][16];
    int np = 0;
    bool deref_head = false;   /* §196：首步（最外层）是否为裸指针穿透——
                                * 循环从最外层向内折，穿透发生在 np==0 时
                                * 即「末端穿透」（`t1[0].name.0` 的 `.0` 是
                                * 路径之外最后一层）；路径中间的穿透
                                * （`t1[0].name.0.f`，此时 np>0）不影响。 */
    Node *fb = e;
    /* §174：元组字段访问被改写成 NK_FIELD（name=NULL、ival=下标）——
     * 基址类型是元组的数字分量按 "N" 进点路径；§192：基址是**数组指针**
     * 的 .N（等价 ptr[N]）同样按下标分量进路径（与 NK_INDEX 常索引同
     * 一本账，两种拼法互通）；基址是裸指针的 .0 是纯解引用（身份不变），
     * 路径透明穿过（等价自动穿透的字段访问）；运行期索引在此截断，
     * 写入处另报 E0521。 */
    while (fb && np < 8 &&
           (fb->kind == NK_FIELD &&
            (fb->name || (fb->ival >= 0 && fb->a && fb->a->ty &&
                          (fb->a->ty->kind == TY_TUPLE ||
                           fb->a->ty->kind == TY_PTR))) ||
            (fb->kind == NK_INDEX && fb->b && fb->b->kind == NK_INT &&
             fb->a))) {
        if (fb->kind == NK_FIELD) {
            if (fb->name) {
                names[np] = fb->name;
            } else if (fb->a->ty->kind == TY_PTR && !fb->a->ty->has_len &&
                       !ty_is_array_ptr(fb->a->ty)) {
                fb = fb->a;                 /* 裸指针 .0：透明穿过 */
                if (np == 0) deref_head = true;   /* §196：首步穿透 */
                continue;
            } else {
                snprintf(idxbuf[np], sizeof idxbuf[np], "%d", (int)fb->ival);
                names[np] = idxbuf[np];
            }
        } else {
            snprintf(idxbuf[np], sizeof idxbuf[np], "%d",
                     (int)fb->b->ival);
            names[np] = idxbuf[np];
        }
        np++;
        fb = fb->a;
    }
    if (!fb || np == 0 || np == 8) {
        if (base_out) *base_out = fb;   /* §175：截断点带回（运行期索引判定用） */
        return false;
    }
    if (deref_tail) *deref_tail = deref_head;   /* §196：末端穿透 */
    /* names[0] 是最内层（叶子字段名），names[np-1] 是最外层——倒着拼 */
    size_t off = 0;
    for (int i = np - 1; i >= 0; i--) {
        size_t l = strlen(names[i]);
        if (off + l + 2 > cap) return false;
        memcpy(buf + off, names[i], l);
        off += l;
        if (i > 0) buf[off++] = '.';
    }
    buf[off] = 0;
    *base_out = fb;
    return true;
}

/* §196：四参兼容包装（不关心末端穿透的调用点）。 */
bool ft_path_key(Node *e, char *buf, size_t cap, Node **base_out) {
    return ft_path_key_d(e, buf, cap, base_out, NULL);
}

/* 点路径 key 的持久化副本（插账用——ft_set 只存指针）。 */
char *ft_path_dup(const char *prefix, const char *leaf) {
    size_t lp = prefix ? strlen(prefix) : 0, ll = strlen(leaf);
    char *s = malloc(lp + (lp ? 1 : 0) + ll + 1);
    if (!s) return NULL;
    if (lp) { memcpy(s, prefix, lp); s[lp++] = '.'; }
    memcpy(s + lp, leaf, ll + 1);
    return s;
}

static bool ft_member_of(FieldTag *list, const char *name, int *id, int *state) {
    FieldTag *f = ft_find(list, name);
    if (!f || f->slot <= 0) return false;
    int st = ft_state_of(f->slot);
    if (st == TC_NONE) return false;
    *id = f->slot; *state = st;
    return true;
}

/* 表达式的字段账本：节点自带优先，ident 查符号。 */
FieldTag *ftags_of_expr(Node *e) {
    if (!e) return NULL;
    if (e->ftags) return (FieldTag *)e->ftags;
    if (e->kind == NK_IDENT && e->sym && e->sym->ftags)
        return (FieldTag *)e->sym->ftags;
    return NULL;
}

/* §174：捕获载荷继承主语的 tag 账（switch 的 union(enum) 变体捕获、
 * if/while/try 的 ?T / !T 捕获）。
 *  - 主语顶层带 tag 且捕获类型是指针/切片（?*u8 的 .some 捕获、变体
 *    载荷本身是指针）：捕获符号接同一槽；
 *  - 主语字段账带「变体.子字段」路径（union(enum) 载荷是聚合）：剥掉
 *    变体前缀落到捕获符号；
 *  - 主语字段账无前缀（?S 载荷结构体的字段账）：原样继承。
 * 不继承就断链：臂内 free(b.0) 记不上账，泄漏终扫误报。 */
void tag_capture_inherit(Symbol *cs, Node *subj, const char *vname) {
    if (!cs) return;
    int id, st;
    tag_of_expr(subj, &id, &st);
    bool ptr_cap = cs->ty && (cs->ty->kind == TY_PTR || cs->ty->kind == TY_SLICE);
    /* §187：容器账（book）随捕获绑定一起走——`if (ar) a { … }` 的
     * a 必须带着资源的账，deinit(a) 才终结得了。 */
    if (id && st == TC_LIVE &&
        (ptr_cap || (tag_slot(id) && tag_slot(id)->book))) {
        cs->tag_id = id;
        cs->tag_state = st;
    }
    FieldTag *sft = ftags_of_expr(subj);
    for (FieldTag *g = sft; g; g = g->next) {
        if (g->slot <= 0) continue;
        if (vname && vname[0]) {
            size_t vl = strlen(vname);
            if (!strncmp(g->name, vname, vl) && g->name[vl] == '.') {
                char *key = ft_path_dup(NULL, g->name + vl + 1);
                if (key) ft_set((FieldTag **)&cs->ftags, key, g->slot);
            } else if (!strcmp(g->name, vname) && ptr_cap && !cs->tag_id) {
                cs->tag_id = g->slot;
                cs->tag_state = TC_LIVE;
            }
        } else {
            char *key = ft_path_dup(NULL, g->name);
            if (key) ft_set((FieldTag **)&cs->ftags, key, g->slot);
        }
    }
}
static ParamFieldFree *g_pff;
static int g_npff, g_cpff;

void pff_add(Symbol *ps, const char *fname) {
    for (int i = 0; i < g_npff; i++)
        if (g_pff[i].psym == ps && !strcmp(g_pff[i].fname, fname)) return;
    if (g_npff == g_cpff) {
        int nc = g_cpff ? g_cpff * 2 : 16;
        ParamFieldFree *p2 = realloc(g_pff, (size_t)nc * sizeof(ParamFieldFree));
        if (!p2) return;
        g_pff = p2; g_cpff = nc;
    }
    g_pff[g_npff].psym = ps;
    g_pff[g_npff].fname = fname;
    g_npff++;
}

bool pff_has(Symbol *ps, const char *fname) {
    for (int i = 0; i < g_npff; i++)
        if (g_pff[i].psym == ps && !strcmp(g_pff[i].fname, fname)) return true;
    return false;
}
ParamFieldStore *g_pfs;
int g_npfs, g_cpfs;

void pfs_add_ex(Symbol *ps, const char *key, int slot, Symbol *src_param) {
    for (int i = 0; i < g_npfs; i++)
        if (g_pfs[i].psym == ps && !strcmp(g_pfs[i].key, key)) {
            g_pfs[i].slot = slot;
            g_pfs[i].src_param = src_param;   /* 覆盖写：换账 */
            return;
        }
    if (g_npfs == g_cpfs) {
        int nc = g_cpfs ? g_cpfs * 2 : 16;
        ParamFieldStore *p2 = realloc(g_pfs, (size_t)nc * sizeof(ParamFieldStore));
        if (!p2) return;
        g_pfs = p2; g_cpfs = nc;
    }
    g_pfs[g_npfs].psym = ps;
    g_pfs[g_npfs].key = xstrdup(key);
    g_pfs[g_npfs].slot = slot;
    g_pfs[g_npfs].src_param = src_param;
    g_npfs++;
}
void pfs_add(Symbol *ps, const char *key, int slot) {
    pfs_add_ex(ps, key, slot, NULL);
}

/* §180：「形参被解引用」事实账。deps 拓扑下被调体先查、抽象槽共用，
 * 函数体内看到的形参永远是 TC_LIVE（调用点的 FREED 状态进不去）——
 * 所以 use-after-free 只能在**调用点**判：被调体检查时在这里记下哪些
 * 形参符号被 deref 过（p[0] / p.0 / p.field / *p），调用点传进来的
 * 实参若已 FREED 且对应形参在账里，就报 E0512。与 pff（体内 free →
 * 调用点标 FREED）方向相反、机制对称；按符号指针记账，跨声明序稳定。 */
static Symbol **g_pderef;
int      g_npderef, g_cpderef;
static void pderef_add(Symbol *ps) {
    if (!ps || ps->kind != SK_PARAM) return;
    for (int i = 0; i < g_npderef; i++)
        if (g_pderef[i] == ps) return;
    if (g_npderef == g_cpderef) {
        int nc = g_cpderef ? g_cpderef * 2 : 16;
        Symbol **p2 = realloc(g_pderef, (size_t)nc * sizeof(Symbol *));
        if (!p2) return;
        g_pderef = p2; g_cpderef = nc;
    }
    g_pderef[g_npderef++] = ps;
}
bool pderef_has(Symbol *ps) {
    for (int i = 0; i < g_npderef; i++)
        if (g_pderef[i] == ps) return true;
    return false;
}
static ParamSlotSym *g_pslotsym;
int g_npslotsym, g_cpslotsym;
void pslot_sym_add(int tag_id, Symbol *ps) {
    if (!tag_id || !ps) return;
    for (int i = 0; i < g_npslotsym; i++)
        if (g_pslotsym[i].tag_id == tag_id) return;
    if (g_npslotsym == g_cpslotsym) {
        int nc = g_cpslotsym ? g_cpslotsym * 2 : 16;
        ParamSlotSym *p2 = realloc(g_pslotsym, (size_t)nc * sizeof(ParamSlotSym));
        if (!p2) return;
        g_pslotsym = p2; g_cpslotsym = nc;
    }
    g_pslotsym[g_npslotsym].tag_id = tag_id;
    g_pslotsym[g_npslotsym].psym = ps;
    g_npslotsym++;
}
Symbol *pslot_sym_find(int tag_id) {
    for (int i = 0; i < g_npslotsym; i++)
        if (g_pslotsym[i].tag_id == tag_id) return g_pslotsym[i].psym;
    return NULL;
}
/* 从解引用 base 剥到形参：`p` / `p.0` / `p[i]` / `p.field` / `*p`。 */
static void pderef_note(Node *base, int tag_id) {
    Node *e = base;
    for (int g = 0; g < 12 && e; g++) {
        if (e->kind == NK_IDENT && e->sym) {
            pderef_add(e->sym);
            break;
        }
        e = e->kind == NK_UNWRAP || e->kind == NK_CAST ? (e->b ? e->b : e->a)
            : (e->kind == NK_FIELD || e->kind == NK_INDEX) ? e->a : NULL;
    }
    Symbol *ps = pslot_sym_find(tag_id);
    if (ps) pderef_add(ps);
}
ArrEsc *g_aesc;
int g_naesc, g_caesc;

/* 路径持久化：调用方的缓冲是栈上的，账里存副本。空指针=空串。 */
static void agt_path_put(char *dst, const char *src) {
    if (!src || !*src) { dst[0] = 0; return; }
    size_t l = strlen(src);
    if (l >= 64) l = 63;
    memcpy(dst, src, l); dst[l] = 0;
}

void aesc_add(Symbol *base, const char *path, int idx, int slot) {
    for (int i = 0; i < g_naesc; i++)
        if (g_aesc[i].base == base && !strcmp(g_aesc[i].path, path ? path : "")) {
            if (g_aesc[i].idx != idx) continue;
            g_aesc[i].slot = slot;          /* 覆盖写入：换账 */
            return;
        }
    if (g_naesc == g_caesc) {
        int nc = g_caesc ? g_caesc * 2 : 16;
        ArrEsc *p2 = realloc(g_aesc, (size_t)nc * sizeof(ArrEsc));
        if (!p2) return;
        g_aesc = p2; g_caesc = nc;
    }
    g_aesc[g_naesc].base = base;
    agt_path_put(g_aesc[g_naesc].path, path);
    g_aesc[g_naesc].idx = idx;
    g_aesc[g_naesc].slot = slot;
    g_naesc++;
    TagSlot *t = tag_slot(slot);
    if (t) t->arr_esc = true;
}

/* 精确元素账：(base, path, idx) 对应的贴标槽，0 = 无账（§226）。 */
int aesc_find(Symbol *base, const char *path, int idx) {
    for (int i = 0; i < g_naesc; i++)
        if (g_aesc[i].base == base &&
            !strcmp(g_aesc[i].path, path ? path : "") &&
            g_aesc[i].idx == idx)
            return g_aesc[i].slot;
    return 0;
}

/* 这个数组被用来存过贴标指针吗？（运行期索引读出的断线判据：
 * 有账 = 里面可能有贴标指针，哪个元素装哪个编译期不知道。） */
static bool aesc_any(Symbol *base, const char *path) {
    for (int i = 0; i < g_naesc; i++)
        if (g_aesc[i].base == base &&
            !strcmp(g_aesc[i].path, path ? path : "")) return true;
    return false;
}

/* 索引表达式是编译期常数则取值（>=0），否则 -1。 */
int aesc_const_index(Node *ix) {
    if (ix && ix->kind == NK_INT && ix->ival >= 0) return (int)ix->ival;
    return -1;
}

/* §201：`*[N]T` 的 `.N` / `[i]` 取的是第 i 个**数组**，不是元素；
 * 元素要继续 `[i]`。裸指针 `.0` 是解引用。两者都不算"元素访问"。 */
bool ft_num_elem(Node *e) {
    (void)e;
    return false;
}

/* §187：表达式是否带「指针出处」污点——值（或经变量）源自指针→整数
 * 转换。算术运算不洗源（污点只进不出）；赋值重绑按右值重算。 */
int expr_taint(Node *e) {
    if (!e) return 0;
    if (e->ptr_taint) return 1;
    if (e->kind == NK_IDENT && e->sym && e->sym->ptr_taint) return 1;
    return 0;
}
static void agt_close(Symbol *base, const char *path);  /* §185/§226 */
AgtMem *g_agt;
int g_nagt, g_cagt;

/* §226：群键 = 基变量 + 字段路径。path 传 NULL 表示「该基变量的全部
 * 群」（整容器终结 `tag s : null;` 这种整载体出口），传 "arr" 只指
 * `s.arr` 那一个数组字段。空串 = 数组就是基变量本身（旧行为）。 */
static bool agt_path_eq(const char *a, const char *b) {
    return !strcmp(a ? a : "", b ? b : "");
}
/* 通配匹配：want=NULL 命中该基变量的任意路径；否则要求路径相等。 */
static bool agt_path_match(const char *stored, const char *want) {
    if (!want) return true;
    return !strcmp(stored, want);
}

void agt_add(Symbol *base, const char *path, int slot) {
    if (!base || slot <= 0) return;
    for (int i = 0; i < g_nagt; i++)
        if (g_agt[i].base == base && agt_path_eq(g_agt[i].path, path) &&
            g_agt[i].slot == slot) {
            g_agt[i].taken = false;     /* 取出后重新存入：重新登记 */
            return;
        }
    if (g_nagt == g_cagt) {
        int nc = g_cagt ? g_cagt * 2 : 16;
        AgtMem *p2 = realloc(g_agt, (size_t)nc * sizeof(AgtMem));
        if (!p2) return;
        g_agt = p2; g_cagt = nc;
    }
    g_agt[g_nagt].base = base;
    agt_path_put(g_agt[g_nagt].path, path);
    g_agt[g_nagt].slot = slot;
    g_agt[g_nagt].taken = false;
    g_nagt++;
}

bool agt_has(Symbol *base, const char *path, int slot) {
    for (int i = 0; i < g_nagt; i++)
        if (g_agt[i].base == base && agt_path_eq(g_agt[i].path, path) &&
            g_agt[i].slot == slot)
            return !g_agt[i].taken;
    return false;
}

/* 这个数组存过贴标指针吗（读出断线判据，口径同 aesc_any）。 */
bool agt_any(Symbol *base, const char *path) {
    for (int i = 0; i < g_nagt; i++)
        if (g_agt[i].base == base && agt_path_eq(g_agt[i].path, path))
            return true;
    return false;
}

void agt_remove(Symbol *base, const char *path, int slot) {
    /* 取出销账：成员保留（taken）——agt_any 不因取空而变假，
     * 后续再取同一标签才接得住 E0524；整容器终结走 agt_clear。 */
    for (int i = 0; i < g_nagt; i++)
        if (g_agt[i].base == base && agt_path_eq(g_agt[i].path, path) &&
            g_agt[i].slot == slot)
            g_agt[i].taken = true;
}

/* 整容器终结：群内 LIVE 槽全标 FREED（free 事件现场）、群清空。
 * 常索引 aesc 条目同属这个容器——一并覆盖（E0509 由槽级 double-free
 * 检查管，FREED 槽跳过）。path=NULL 清该基变量的所有路径群。 */
void agt_clear(Symbol *base, const char *path, Node *site) {
    for (int i = 0; i < g_naesc; i++)
        if (g_aesc[i].base == base && agt_path_match(g_aesc[i].path, path) &&
            g_aesc[i].slot > 0)
            ft_mark_freed(g_aesc[i].slot, site);
    for (int i = 0; i < g_nagt; ) {
        if (g_agt[i].base == base && agt_path_match(g_agt[i].path, path)) {
            ft_mark_freed(g_agt[i].slot, site);
            g_agt[i] = g_agt[g_nagt - 1];
            g_nagt--;
        } else i++;
    }
    agt_close(base, path);
}

/* 数组/切片基址 → 基变量符号（别名视图共享基账）。IDENT 直接取；
 * 切片/铸型/解包沿基址剥；其余（指针算术、多级间接）解析不了返回
 * NULL——写入处回退 rt_pending 断线拦截。
 *
 * §226：新增字段路径维度。`s.arr` / `s.inner.arr` 这类**数组/切片字段**
 * 也能解析到基变量 s，同时把点路径（"arr" / "inner.arr"，与 §196 字段
 * 账同口径）写进 path——群键是「基变量 + 路径」，同结构体的 `s.a` 与
 * `s.b` 因此是两本独立的账（旧实现一律返回 NULL，字段数组里的标签
 * 既进不了群也读不回，free 掉了终扫还假报 E0513）。
 * path 传 NULL 表示调用方不关心路径（按空串处理）。只认数组/切片/
 * 数组指针字段；普通标量字段与裸指针字段（那是指针算术，语义不同）
 * 一律解析不了 → NULL，保持旧的 rt_pending 保守拦截。
 * 找到数组字段后，再向外的字段（`s.inner.arr` 的 `.inner`）只是载体
 * 定位，照 §196 的点路径口径一并记进路径。 */
Symbol *agt_base_of(Node *e, char *path, size_t cap) {
    const char *names[8];
    char idxbuf[8][16];
    int np = 0;
    bool seen_agg = false;
    if (path && cap) path[0] = 0;
    for (int g = 0; g < 8 && e; g++) {
        if (e->kind == NK_IDENT) {
            Symbol *s = (e->sym && e->sym->kind != SK_CONST) ? e->sym : NULL;
            if (path && cap) {
                /* names[] 由外层向基址收集，拼出的点路径要倒过来：
                 * 最外层字段在前（基址向外读的顺序）。 */
                size_t off = 0;
                for (int i = np - 1; i >= 0; i--) {
                    size_t l = strlen(names[i]);
                    if (off + l + 2 > cap) { off = 0; break; }  /* 超长→退化无路径 */
                    memcpy(path + off, names[i], l); off += l;
                    if (i > 0) path[off++] = '.';
                }
                path[off] = 0;
            }
            return s;
        }
        if (e->kind == NK_SLICE || e->kind == NK_CAST ||
            e->kind == NK_UNWRAP ||
            /* §201：`xs[0][i]` 里外层 [0] 取的是 [N]T 数组值（不是元素），
             * 要继续下钻才能找到数组基变量 xs，否则标签进不了群、终扫误报
             * E0513。旧语义 xs[0] 直接就是元素指针，这里从不需要下钻下标。 */
            e->kind == NK_INDEX) { e = e->a; continue; }
        if (e->kind == NK_FIELD && np < 8) {
            /* 数组性看**字段自身**的类型（e->ty），不是基址的类型
             * （e->a->ty 那是装这个字段的聚合体）。 */
            Type *t = e->ty;
            bool agg = (t && (t->kind == TY_ARRAY || t->kind == TY_SLICE ||
                              (t->kind == TY_PTR && ty_is_array_ptr(t))));
            if (agg) {
                if (e->name) {
                    names[np++] = e->name;
                } else {
                    /* 元组字段里的数组：按下标进路径（与 §196 同口径） */
                    snprintf(idxbuf[np], sizeof idxbuf[np], "%d", (int)e->ival);
                    names[np++] = idxbuf[np];
                }
                seen_agg = true; e = e->a; continue;
            }
            /* 已经定位到数组字段之后，向外的命名字段只是载体路径。 */
            if (seen_agg && e->name && e->a) { names[np++] = e->name; e = e->a; continue; }
        }
        return NULL;
    }
    return NULL;
}

/* §185 跨函数：「形参数组被整容器终结」事实账（体内 free(p) 且 p
 * 是数组/切片形参）——调用点把实参数组的群一起清掉（闭环：容器
 * 所有权随调用移交，调用方不再欠群内标签的释放）。 */
static Symbol **g_pffa;
static int g_npffa, g_cpffa;
static AgtClosed *g_agtclosed;
static int g_nagtclosed, g_cagtclosed;
static void agt_close(Symbol *base, const char *path) {
    if (!base) return;
    const char *p = path ? path : "*";
    for (int i = 0; i < g_nagtclosed; i++)
        if (g_agtclosed[i].base == base && !strcmp(g_agtclosed[i].path, p))
            return;
    if (g_nagtclosed == g_cagtclosed) {
        int nc = g_cagtclosed ? g_cagtclosed * 2 : 16;
        AgtClosed *p2 = realloc(g_agtclosed, (size_t)nc * sizeof(AgtClosed));
        if (!p2) return;
        g_agtclosed = p2; g_cagtclosed = nc;
    }
    g_agtclosed[g_nagtclosed].base = base;
    agt_path_put(g_agtclosed[g_nagtclosed].path, p);
    g_nagtclosed++;
}
bool agt_closed(Symbol *base, const char *path) {
    for (int i = 0; i < g_nagtclosed; i++)
        if (g_agtclosed[i].base == base) {
            if (g_agtclosed[i].path[0] == '*') return true;   /* 通配关闭 */
            if (path && !strcmp(g_agtclosed[i].path, path)) return true;
        }
    return false;
}
void pffa_add(Symbol *ps) {
    if (!ps) return;
    for (int i = 0; i < g_npffa; i++)
        if (g_pffa[i] == ps) return;
    if (g_npffa == g_cpffa) {
        int nc = g_cpffa ? g_cpffa * 2 : 16;
        Symbol **p2 = realloc(g_pffa, (size_t)nc * sizeof(Symbol *));
        if (!p2) return;
        g_pffa = p2; g_cpffa = nc;
    }
    g_pffa[g_npffa++] = ps;
}
bool pffa_has(Symbol *ps) {
    for (int i = 0; i < g_npffa; i++)
        if (g_pffa[i] == ps) return true;
    return false;
}

/* 同一坐标只报一次（读账路径会被多个消费者走到）。 */
bool tag_diag_once(int line, int col) {
    static struct { int line, col; } seen[64];
    static int nseen;
    for (int i = 0; i < nseen; i++)
        if (seen[i].line == line && seen[i].col == col) return false;
    if (nseen < 64) { seen[nseen].line = line; seen[nseen].col = col; nseen++; }
    return true;
}

/* 字段槽的释放事件：槽标 FREED（口径同 tag_free_event，调用点只在
 * 被调方确实 free 过时才走到这里），g_locals 上的 LIVE 载体同步。 */
void ft_mark_freed(int slot, Node *site) {
    TagSlot *t = tag_slot(slot);
    if (!t || t->state != TST_LIVE) return;
    t->state = TST_FREED;
    t->ever_freed = true;
    if (site) { t->free_line = site->line; t->free_col = site->col; }
    for (Symbol *ls = g_locals; ls; ls = ls->locals_next)
        if (ls->tag_id == slot && ls->tag_state == TC_LIVE)
            ls->tag_state = TC_FREED;
}

/* 扫 return 的结构体值字段账本（剥铸型/解包），多路合并。 */
void ft_scan_returns(Node *n, FieldTag **out) {
    if (!n || n->kind == NK_FN) return;
    if (n->kind == NK_RETURN && n->a) {
        Node *src = n->a;
        for (int g = 0; g < 6 && src &&
             (src->kind == NK_CAST || src->kind == NK_UNWRAP ||
              src->kind == NK_TRY); g++)
            src = src->kind == NK_CAST ? (src->b ? src->b : src->a) : src->a;
        FieldTag *fs = ftags_of_expr(src);
        if (fs) {
            if (!*out) *out = ft_clone(fs);
            else *out = ft_merge(*out, fs);
        }
        return;
    }
    ft_scan_returns(n->a, out);
    ft_scan_returns(n->b, out);
    ft_scan_returns(n->c, out);
    ft_scan_returns(n->d, out);
    for (int i = 0; i < n->nitems; i++) ft_scan_returns(n->items[i], out);
}

/* 表达式携带的 tag 载体：(id = TagSlot 下标+1, 载体状态)。id=0 = 不带 tag。
 * 顺序：节点自带的标记（贴标/合并/传播时填好）优先，ident 查符号，
 * 收割冒泡到值。 */
void tag_of_expr(Node *e, int *id, int *state) {
    *id = 0; *state = TC_NONE;
    if (!e) return;
    if (e->tag_id) { *id = e->tag_id; *state = e->tag_state; return; }
    if (e->kind == NK_IDENT && e->sym && e->sym->tag_id) {
        *id = e->sym->tag_id; *state = e->sym->tag_state; return;
    }
    if (e->kind == NK_YIELD) { tag_of_expr(e->a, id, state); return; }
    /* §174 续链：`tag v : tag.x = old;` 降糖成 let——DECL 绑定从这里
     * 拿到续上的槽（case 里已守门：值无标签、槽 LIVE）。 */
    if (e->kind == NK_TAG_BIND && e->tag_id) {
        *id = e->tag_id; *state = TC_LIVE;
        return;
    }
    /* §173：`p.?`（未烙 COND→LIVE 印的普通解包）与切片 `.ptr` 沿基址追：
     * free 的实参常写成 `(*u8)s.ptr`，没有这条 free 事件就记不上账。 */
    if (e->kind == NK_UNWRAP && e->a) { tag_of_expr(e->a, id, state); return; }
    /* §179：切片是同一份内存的视图——p[0..] 携带基址的标签；cast 结果
     * 落在指针/切片家族时身份同样跟过去（节点烙印只盖指针→指针，切片
     * 家族在这里兜底；抽象借用槽不追）。 */
    if ((e->kind == NK_SLICE || e->kind == NK_CAST) && e->a && e->ty &&
        (e->ty->kind == TY_SLICE || e->ty->kind == TY_PTR) &&
        !(e->kind == NK_CAST && e->a->tag_id &&
          tag_slot(e->a->tag_id) && tag_slot(e->a->tag_id)->abstract)) {
        tag_of_expr(e->a, id, state);
        return;
    }
    /* §173 补丁十（收紧版）：常索引读精确元素账——`let q = xs[0][i];`
     * §201：只有**数组值**的下标才是元素访问；`*[N]T` 的 `[i]` / `.i`
     * 取的是第 i 个数组，不能按元素账处理。运行期索引：数组存过贴标
     * 指针就报 E0521（标签绝不静默丢失）。只继承 LIVE。
     * §226：基址解析与写入侧统一走 agt_base_of，因此字段数组
     * `s.arr[i]` 的读取也能对上 `s.arr` 那本账（路径键一致）。 */
    bool idx_elem = e->kind == NK_INDEX && e->a &&
                    !(e->a->ty && ty_is_array_ptr(e->a->ty));
    if (idx_elem) {
        char bpath[64];
        Symbol *bvar = agt_base_of(e->a, bpath, sizeof bpath);
        if (bvar) {
            int ix = aesc_const_index(e->b);
            int aid = ix >= 0 ? aesc_find(bvar, bpath, ix) : 0;
            if (ix < 0 && (aesc_any(bvar, bpath) || agt_any(bvar, bpath)) &&
                tag_diag_once(e->line, e->col))
                diag_code_at("E0521", e->line, e->col,
                    "this element is read with a runtime index and the array "
                    "holds tracked pointers -- which allocation an element "
                    "carries cannot be identified; take it out with an "
                    "annotated read (`tag v : tag.x = xs[i];`), or store at a "
                    "constant index");
            if (aid && ft_state_of(aid) == TC_LIVE) {
                *id = aid; *state = TC_LIVE;
                return;
            }
        }
    }
    /* §173：结构体字段携带的 tag —— `s.name` / `make().name`。符号基址
     * 查符号的字段表，表达式基址（调用/字面量）查节点字段表；铸型/
     * 解包先剥。补丁八：嵌套字段（s.inner.name）——字段链折叠成点
     * 路径 key（"inner.name"）查账，单层字段是深度 1 的特例。
     * §174：元组元素（t.0 / t[N] 改写成 name=NULL 的 NK_FIELD）走同
     * 一条点路径（数字分量）；同形状但基址是指针的是解引用，沿基址追。 */
    if (e->kind == NK_FIELD && e->a && (e->name ||
        (e->ival >= 0 && e->a->ty && e->a->ty->kind == TY_TUPLE))) {
        char key[128];
        Node *fb = NULL;
        if (ft_path_key(e, key, sizeof key, &fb)) {
            for (int g = 0; g < 6 && fb &&
                 (fb->kind == NK_CAST || fb->kind == NK_UNWRAP); g++)
                fb = fb->kind == NK_CAST ? (fb->b ? fb->b : fb->a) : fb->a;
            if (fb && fb->kind == NK_IDENT && fb->sym &&
                ft_member_of((FieldTag *)fb->sym->ftags, key, id, state))
                return;
            if (fb && ft_member_of((FieldTag *)fb->ftags, key, id, state))
                return;
            /* 前缀回退：账里有 "inner"（内层本身是贴标指针）而问的是
             * "inner.name"——`.name` 是对贴标指针的解引用，沿前缀追。 */
            char *dot = strrchr(key, '.');
            if (dot) {
                *dot = 0;
                if (fb && fb->kind == NK_IDENT && fb->sym &&
                    ft_member_of((FieldTag *)fb->sym->ftags, key, id, state))
                    return;
                if (fb && ft_member_of((FieldTag *)fb->ftags, key, id, state))
                    return;
            }
        }
    }
    if (e->kind == NK_FIELD && e->name && !strcmp(e->name, "ptr") && e->a) {
        tag_of_expr(e->a, id, state);
        return;
    }
    /* §174：(*p) 解引用（方法接收者自动补的 name=NULL NK_FIELD，基址
     * 是指针）：贴标指针经解引用身份不变。§201：数组指针的 `.N` 取数组，
     * 不是解引用也不是元素访问——上面已按精确账处理，这里不吞。 */
    if (e->kind == NK_FIELD && !e->name && e->a && e->a->ty &&
        e->a->ty->kind == TY_PTR && !e->a->ty->has_len &&
        !ty_is_array_ptr(e->a->ty)) {
        tag_of_expr(e->a, id, state);
        return;
    }
}

/* 定义 / 查重。§172：tag 名字是全局唯一键（模仿 error 名字分配码）：
 * 首次引用（tag.buf）即自动建槽，没有声明语句，同名幂等不报重复。 */
TagSlot *tag_decl(const char *name, Node *n) {
    /* §225：账本默认关闭——当前模块没写 `tag.enable;` 就一个槽都不建。
     * 建槽是全部检查（泄漏 E0513、双放 E0509、UAF E0512、分支确认
     * E0528、续链 E0521、显式贴标）的共同前提，门控收在这一个入口，
     * 休眠模块连槽都没有，终扫与事件检查自然全部落空。std 不写
     * tag.enable，因此也走这里返回 NULL——豁免从"额外逻辑"变成"默认休眠"。
     * 幂等：同名的已存在槽（enabled 模块先建的）照旧返回。 */
    if (!(g_mod && g_mod->tags_enabled)) return NULL;
    TagSlot *ex = tag_by_name(name);
    if (ex) return ex;
    if (g_ntslots == g_ctslots) {
        int nc = g_ctslots ? g_ctslots * 2 : 16;
        TagSlot *p = realloc(g_tslots, (size_t)nc * sizeof(TagSlot));
        if (!p) { err_at(n->line, n->col, "out of memory"); return NULL; }
        g_tslots = p; g_ctslots = nc;
    }
    TagSlot *t = &g_tslots[g_ntslots++];
    memset(t, 0, sizeof *t);
    t->name = name;
    t->line = n->line; t->col = n->col;
    /* §176：分配事实归谁调的——建槽即记当下函数与文件（槽幂等，
     * 首次建槽的上下文就是分配点的上下文） */
    t->def_fn = g_cur_fn_name;
    t->file = (g_mod && g_mod->path) ? g_mod->path : NULL;
    return t;
}

int g_tag_auto_seq;

/* §173：alloc 自动建槽。每个分配调用点自动获得唯一标签并预绑 LIVE——
 * `tag.x : expr` 只剩「给分配起名」的作用。§225：没写 tag.enable 的模块连槽都不建，std 天然休眠。 */
void tag_auto_decl(Node *call, bool stack) {
    if (call->tag_id) return;                       /* 重扫幂等 */
    /* §225：账本默认关闭，没写 tag.enable 的模块不建自动槽。 */
    if (!(g_mod && g_mod->tags_enabled)) return;
    /* 槽名必须是可词法的标识符：手动标记 `tag.allocN : null;` 要能
     * 在源码里引用自动槽（alloc#N 的 # 写不进源码）。撞名则跳号。 */
    char namebuf[32];
    do {
        snprintf(namebuf, sizeof namebuf, "alloc%d", ++g_tag_auto_seq);
    } while (tag_by_name(namebuf));
    char *nm = xstrdup(namebuf);
    if (!nm) return;
    Node tmp; memset(&tmp, 0, sizeof tmp);
    tmp.line = call->line; tmp.col = call->col;
    TagSlot *t = tag_decl(nm, &tmp);
    if (!t) return;
    t->auto_n = true;
    t->stack = stack;
    t->state = TST_LIVE;
    t->region = stack ? 0 : call->reg_slot;
    if (!stack && call->reg_slot > 0 && call->reg_slot <= g_nrslots)
        g_rslots[call->reg_slot - 1].tag = (int)(t - g_tslots) + 1;
    call->tag_id = (int)(t - g_tslots) + 1;
    call->tag_state = TC_LIVE;
}

/* §175：调用点槽实例化。工厂函数（分配经返回流转出所有权）每次调用
 * 都产生一份新的运行期分配——所有调用点共享同一个静态槽会把第二次
 * free 假报成 E0509。克隆槽继承分配事实（allocated at 分配行），泄漏
 * 归属点记在调用行；母槽一旦派生实例就退役（终扫由实例接管）。
 * 注意 tag_decl 可能 realloc 槽表——先存母槽索引再回取。 */
int tag_clone_site(TagSlot *src, Node *call, bool freed) {
    int sidx = (int)(src - g_tslots) + 1;
    char namebuf[32];
    do {
        snprintf(namebuf, sizeof namebuf, "alloc%d", ++g_tag_auto_seq);
    } while (tag_by_name(namebuf));
    char *nm = xstrdup(namebuf);
    if (!nm) return 0;
    Node tmp; memset(&tmp, 0, sizeof tmp);
    tmp.line = call->line; tmp.col = call->col;
    TagSlot *t = tag_decl(nm, &tmp);
    if (!t) return 0;
    t->auto_n = true;
    t->state = freed ? TST_FREED : TST_LIVE;
    t->ever_freed = freed || src->ever_freed;
    t->inst_of = sidx;
    t->stack = src->stack;             /* 文案/作用域语义随母槽 */
    t->scope_freed = src->scope_freed;
    if (freed) { t->free_line = src->free_line; t->free_col = src->free_col; }
    src = tag_slot(sidx);
    if (src) src->ninst++;
    t->line = src ? src->line : call->line;        /* 分配事实留在分配行 */
    t->col = src ? src->col : call->col;
    t->def_fn = src ? src->def_fn : t->def_fn;     /* §176：分配点随母槽 */
    t->file = src ? src->file : t->file;
    t->own_fn = g_cur_fn_name;                     /* 归属调用点所在函数 */
    t->own_line = call->line;                      /* 归属点在调用行 */
    t->own_col = call->col;
    return (int)(t - g_tslots) + 1;
}

/* §194：实参载体字段账的可写指针（store 事实镜像的落点）。剥 cast/
 * unwrap 到 IDENT 用符号的字段账，否则退回调用节点的字段账。 */
static FieldTag **ft_arg_ftags(Node *arg) {
    Node *e = arg;
    for (int g = 0; g < 6 && e &&
         (e->kind == NK_CAST || e->kind == NK_UNWRAP); g++)
        e = e->kind == NK_CAST ? (e->b ? e->b : e->a) : e->a;
    if (e && e->kind == NK_IDENT && e->sym)
        return (FieldTag **)&e->sym->ftags;
    return arg ? (FieldTag **)&arg->ftags : NULL;
}

/* §194/195：调用点镜像形参 store 事实——被调体往形参容器字段写入的指针，
 * 烙到实参载体的字段账对应路径；调用方读回该字段（t1[0].name）就拿到槽、
 * 释放有着落。与 pff（体内 free → 调用点标 FREED）方向相反、机制对称。
 * ① 本体内新分配 → tag_clone_site 实例化（每次调用一份），母槽 ninst>0
 *    终扫退役；② §195 透传的抽象形参槽 → 把「来源形参对应的实参」自身的
 *    槽烙进容器实参字段账（不 clone，所有权本在调用方）。 */
void pfs_mirror_arg(Symbol *ps, Node *arg, Node *call, Node *fn) {
    if (!ps || !arg) return;
    for (int i = 0; i < g_npfs; i++) {
        if (g_pfs[i].psym != ps) continue;
        int nid;
        if (g_pfs[i].src_param && fn) {
            /* §195：找来源形参在被调函数形参表中的下标，取调用点同位置
             * 实参自身的槽（它不是本体内新分配，不能 clone）。 */
            int src_idx = -1;
            for (int j = 0; j < fn->nitems; j++)
                if (fn->items[j]->sym == g_pfs[i].src_param) { src_idx = j; break; }
            if (src_idx < 0 || src_idx >= call->nitems) continue;
            int atid, ast;
            tag_of_expr(call->items[src_idx], &atid, &ast);
            if (!atid || ast != TC_LIVE) continue;
            nid = atid;
        } else {
            TagSlot *src = tag_slot(g_pfs[i].slot);
            if (!src || src->abstract) continue;
            nid = tag_clone_site(src, call, src->state == TST_FREED);
            if (!nid) continue;
        }
        FieldTag **dst = ft_arg_ftags(arg);
        if (dst) {
            char *pk = ft_path_dup(NULL, g_pfs[i].key);
            if (pk) ft_set(dst, pk, nid);
        }
    }
}

/* §173：离开作用域 —— stackAlloc 的槽自动标 free（栈存储随帧消亡）。
 * 载体被存进容器/传出去之后再用，撞上的是 E0512（追踪不到容器里那份，
 * 但同一符号的 outer 副本还能追到）。 */void tag_scope_exit(Scope *sc, Node *n) {
    Node *tail = (n->nitems > 0 && n->items[n->nitems - 1])
                 ? n->items[n->nitems - 1] : n;   /* 块的 line 是起点行 */
    for (Symbol *s = sc->first; s; s = s->next) {
        if (!s->tag_id || s->tag_state != TC_LIVE) continue;
        TagSlot *t = tag_slot(s->tag_id);
        if (!t || !t->stack || t->state != TST_LIVE) continue;
        t->state = TST_FREED;
        t->free_line = tail->line; t->free_col = tail->col;
        t->scope_freed = true;
        s->tag_state = TC_FREED;
        if (t->carrier) t->carrier->tag_state = TC_FREED;
    }
    /* §173 补丁八：本作用域期间创建的栈分配槽统一回收——贴标指针
     * 存进结构体字段/数组元素时没有直接载体符号，按符号扫会漏，
     * 槽就会在终扫被误报成堆泄漏（栈存储随帧消亡，谈不上泄漏）。 */
    for (int i = sc->tag_wm; i < g_ntslots; i++) {
        TagSlot *t = &g_tslots[i];
        if (!t->stack || t->abstract || t->state != TST_LIVE) continue;
        t->state = TST_FREED;
        t->free_line = tail->line; t->free_col = tail->col;
        t->scope_freed = true;
        if (t->carrier) t->carrier->tag_state = TC_FREED;
    }
}

/* §172：按名字解析 tag 槽 —— 先查槽位表（已建槽），再查同名 const 别名
 * （const b2 = tag.buf; 之后 `tag.b2` 也指到 buf 的槽，避免按名误建新标签），
 * 都没有就自动建槽（引用即声明）。 */
TagSlot *tag_resolve(Node *n) {
    TagSlot *t = tag_by_name(n->name);
    if (!t) {
        Symbol *s = lookup(n->name);
        if (s) s->used = true;              /* 别名引用算使用 */
        if (s && s->tag_id) t = tag_slot(s->tag_id);
    }
    if (t) return t;
    return tag_decl(n->name, n);
}

/* 解引用点检查：base 带贴标载体时按状态拦（use-after-free / 未防守）。 */
void tag_check_deref(Node *base) {
    int id, st;
    tag_of_expr(base, &id, &st);
    pderef_note(base, id);          /* §180：记下"形参被解引用"事实 */
    if (!id) return;
    TagSlot *t = tag_slot(id);
    if (!t) return;
    if (st == TC_FREED) {
        if (t->scope_freed)
            diag_code_at("E0512", base->line, base->col,
                "use after scope: this pointer is stack storage whose block "
                "ended at line %d -- stack memory dies with its scope, the "
                "pointer must not outlive it", t->free_line);
        else
            diag_code_at("E0512", base->line, base->col,
                "use after free: this memory was released at line %d -- a "
                "freed pointer must not be dereferenced; remove this use or "
                "move it before the free", t->free_line);
        return;
    }
    if (st == TC_UNKNOWN) {
        diag_code_at("E0515", base->line, base->col,
            "it is undetermined whether this memory was freed (the branches "
            "disagree) -- defend before use: re-bind it (`p = tag.%s : "
            "p.?;` asserts it is live) or `free` it", t->name);
        return;
    }
    /* §189：所有权已转移的载体不能再用——内存活着，但已不归本作用域。 */
    if (st == TC_MOVED) {
        diag_code_at("E0529", base->line, base->col,
            "use after move: this pointer's ownership was moved away at "
            "line %d (`std.move`) -- a moved pointer is neither "
            "dereferenceable nor freeable here", t->moved_line);
        return;
    }
    /* COND：解引用必须先判空（`.?`），这是类型系统的事，类型层拦。 */
}

/* 沿载体传播链（cast / unwrap / 收割）找到底层标识符符号：
 * free 的状态要写回真正的载体变量，后续解引用才查得到。 */
Symbol *tag_carrier_sym(Node *e) {
    while (e) {
        if (e->kind == NK_IDENT) return e->sym;
        if (e->kind == NK_CAST || e->kind == NK_UNWRAP || e->kind == NK_YIELD)
            e = e->b ? e->b : e->a;
        else return NULL;
    }
    return NULL;
}

/* 一次释放事件（Allocator.free 直调 / 释放器调用 / 臂内 free）。
 * 返回 true = 这次 free 被记录为该 tag 的释放。 */
bool tag_free_event(Node *site, Node *ptr) {
    int id, st;
    /* §187：释放器调用可以经取址传容器（`run(&pp)`，run 体内 deinit）
     * ——取址不改变身份，剥掉 & 追到基变量。只有释放器调用走这里，
     * 借用型 `foo(&p)` 不受影响。 */
    Node *p2 = ptr;
    for (int g = 0; g < 6 && p2 && p2->kind == NK_UN &&
         p2->op == TOK_AMP; g++) p2 = p2->a;
    tag_of_expr(p2, &id, &st);
    if (!id) return false;
    if (p2->kind == NK_NULL) return false;   /* free(null)：no-op */
    TagSlot *t = tag_slot(id);
    if (!t) return false;
    if (t->state == TST_FREED) {
        if (t->free_line == site->line && t->free_col == site->col) return true;
        if (t->scope_freed)
            diag_code_at("E0509", site->line, site->col,
                "this pointer is stack storage that already died with its "
                "scope (allocated at line %d, scope ended at line %d) -- the "
                "pointer escaped its frame, so releasing it again is a "
                "double free", t->line, t->free_line);
        else
            diag_code_at("E0509", site->line, site->col,
                "this memory was already freed (allocated at line %d, first "
                "freed at line %d) -- releasing it again is a double free",
                t->line, t->free_line);
        return true;
    }
    /* §189：所有权已随 std.move 转移——free 它等于对别的线程/作用域
     * 正在使用的内存下手，直接拦。 */
    if (t->state == TST_MOVED) {
        diag_code_at("E0529", site->line, site->col,
            "this pointer was moved away at line %d (`std.move`) -- the "
            "receiver owns it now; freeing it here is use after move",
            t->moved_line);
        return true;
    }
    /* UNKNOWN / COND：防守二（使用者宣告终结）与 null 合并载体的放行。
     * 悬垂风险由「free 后必须传 null」的分支约定承担。 */
    if (t->adopt_ar) {
        /* §187：已被 arena 收养——内存由 arena 的移交表管，deinit 时
         * 统一归还；这里再 free = deinit 时双重释放。 */
        diag_code_at("E0526", site->line, site->col,
            "this pointer is owned by an arena (adopted at line %d) -- it "
            "is freed when that arena deinits; freeing it here would be a "
            "double free", t->adopt_line);
        return true;
    }
    t->state = TST_FREED;
    t->ever_freed = true;
    t->free_line = site->line; t->free_col = site->col;
    ptr->tag_id = id; ptr->tag_state = TC_FREED;
    Symbol *cs = tag_carrier_sym(p2);
    if (cs) { cs->tag_state = TC_FREED; t->carrier = cs; }
    /* §173：副本同步 —— free(q) 之后，所有携带同一 tag 的局部载体
     * （`var q2 = q;` 之类）一起进 FREED，不然旧副本还能解引用。 */
    for (Symbol *ls = g_locals; ls; ls = ls->locals_next)
        if (ls->tag_id == id && ls->tag_state == TC_LIVE)
            ls->tag_state = TC_FREED;
    return true;
}

/* Allocator.free(ctx, ptr, size) 形状的释放事件（贴标指针走 tag 账本，
 * 不进 region 账本——同一份事实记两遍会双报 E0509）。 */
void tag_note_free(Node *call) {
    Node *a = call->a;
    if (!a || a->kind != NK_FIELD || !a->name || strcmp(a->name, "free") ||
        !a->a || !is_allocator_ty(a->a->ty) || call->nitems != 3) return;
    /* §173：形参字段被 free 的事实账（见 pff_add 注释）。 */
    {
        Node *pa = call->items[1];
        for (int g = 0; g < 6 && pa &&
             (pa->kind == NK_CAST || pa->kind == NK_UNWRAP); g++)
            pa = pa->kind == NK_CAST ? (pa->b ? pa->b : pa->a) : pa->a;
        if (pa && pa->kind == NK_FIELD && pa->name && pa->a &&
            pa->a->kind == NK_IDENT && pa->a->sym &&
            pa->a->sym->kind == SK_PARAM)
            pff_add(pa->a->sym, pa->name);
    }
    /* §185：整容器终结——free 的实参是数组/切片基变量且群非空：
     * 群内 LIVE 槽全部标 FREED、群清空（声明语义：容器所有权到此
     * 终结，群内标签的泄漏责任一并了结；逐元素真释放按 decisions
     * §185 在 free 之前显式取出）。实参是形参时记 pffa 事实账，
     * 调用点/延迟重放把实参数组的群一起清（闭环）。 */
    /* §185（用户裁定）：free 整容器**不**批量清群——数组与元素
     * 不绑定：数组存储释放了、元素没释放，把它们标 FREED 会把真
     * 泄漏错判成已释放。整容器终结只有显式出口 `tag xs : null;`
     * （终结语句里清群 + 记 pffa 事实账），或逐个取出释放。 */
    tag_free_event(call, call->items[1]);
}
static ArAdopt *g_aradt;
static int      g_naradt, g_caradt;

/* 实参剥 cast/unwarp/取址到基变量符号（arena 句柄）。形参不入账（跨函数
 * 的 arena 句柄在调用点才落定，重放机制 v1 不覆盖收养账）。 */
static Symbol *aradt_base_of(Node *e) {
    for (int g = 0; g < 6 && e &&
         (e->kind == NK_CAST || e->kind == NK_UNWRAP ||
          (e->kind == NK_UN && e->op == TOK_AMP)); g++)
        e = e->kind == NK_CAST ? (e->b ? e->b : e->a)
            : e->kind == NK_UN ? e->a : e->a;
    return (e && e->kind == NK_IDENT && e->sym &&
            e->sym->kind != SK_CONST && e->sym->kind != SK_PARAM)
           ? e->sym : NULL;
}

static ArAdopt *aradt_find(int slot) {
    for (int i = 0; i < g_naradt; i++)
        if (g_aradt[i].slot == slot) return &g_aradt[i];
    return NULL;
}

/* `ArenaAllocator.adopt(a, p, n)` —— 所有权移交事件。 */
void tag_note_adopt(Node *call) {
    Node *a = call->a;
    if (!a || a->kind != NK_FIELD || !a->name || strcmp(a->name, "adopt") ||
        call->nitems < 2) return;
    Symbol *ar = aradt_base_of(call->items[0]);
    if (!ar) return;
    int id, st;
    tag_of_expr(call->items[1], &id, &st);
    if (!id) return;                    /* 无标签指针：无需记账 */
    TagSlot *t = tag_slot(id);
    if (!t) return;
    ArAdopt *ent = aradt_find(id);
    if (ent) {
        if (ent->done) return;          /* 复检重放：已随 deinit 终结 */
        if (t->adopt_ar) {
            diag_code_at("E0526", call->line, call->col,
                "this pointer is already owned by an arena (adopted at line "
                "%d) -- a second adopt would make the arena free it twice",
                t->adopt_line);
        }
        return;
    }
    if (t->state == TST_FREED) {
        diag_code_at("E0509", call->line, call->col,
            "this memory was already freed (allocated at line %d, first "
            "freed at line %d) -- it cannot be adopted by an arena",
            t->line, t->free_line);
        return;
    }
    if (t->adopt_ar) {
        diag_code_at("E0526", call->line, call->col,
            "this pointer is already owned by an arena (adopted at line "
            "%d) -- a second adopt would make the arena free it twice",
            t->adopt_line);
        return;
    }
    if (g_naradt == g_caradt) {
        int nc = g_caradt ? g_caradt * 2 : 16;
        ArAdopt *p2 = realloc(g_aradt, (size_t)nc * sizeof(ArAdopt));
        if (!p2) return;
        g_aradt = p2; g_caradt = nc;
    }
    g_aradt[g_naradt].ar = ar;
    g_aradt[g_naradt].slot = id;
    g_aradt[g_naradt].line = call->line;
    g_aradt[g_naradt].col = call->col;
    g_aradt[g_naradt].done = false;
    g_naradt++;
    t->adopt_ar = ar;
    t->adopt_line = call->line;
    t->adopt_col = call->col;
}

/* §188：这一笔调用是不是「释放」。口径与 tag_block_has_free 一致：
 * deinit 变换产物、Allocator.free 方法、用户释放器封装（体内有
 * free/deinit 的函数，释放器传递）。 */
static bool call_is_release(Node *call) {
    if (call->xform_free) return true;
    if (call->a && call->a->kind == NK_FIELD && call->a->name &&
        !strcmp(call->a->name, "free") && call->a->a &&
        (!call->a->a->ty || is_allocator_ty(call->a->a->ty)))
        return true;
    if (call->a && call->a->kind == NK_IDENT && call->a->sym &&
        call->a->sym->decl && call->a->sym->decl->kind == NK_FN &&
        fn_is_releaser(call->a->sym->decl))
        return true;
    return false;
}

/* 模块路径 → Module（豁免判定用；Node->mod 是装载时打的路径戳）。 */
static Module *module_by_path(const char *path) {
    if (!path) return NULL;
    for (int i = 0; i < g_nmods; i++)
        if (g_mods[i] && g_mods[i]->path && !strcmp(g_mods[i]->path, path))
            return g_mods[i];
    return NULL;
}

static Module *alloc_factory_module(Node *e, int depth);   /* 定义在下面 */

bool allocator_exempt(Node *alarg) {
    Symbol *t = allocator_target(alarg, true, 0);
    if (t && t->mod) return t->mod->tags_batch;   /* §188：批量清算才豁免视图 */
    /* 目标符号还没绑定（工厂体未查，Allocator 字面量里的实现函数符号
     * 是空的）——退一步看工厂函数自己的模块：init 的 inst 节点在模块
     * 装载时就带模块戳，不依赖体已查。 */
    Module *m = alloc_factory_module(alarg, 0);
    return m && m->tags_batch;
}

/* 沿分配器值的声明链找产出它的工厂函数，返回其所在模块（只认模块，
 * 不解析实现符号——避开"体未查、符号未绑"的时序）。 */
static Module *alloc_factory_module(Node *e, int depth) {
    if (!e || depth > 6) return NULL;
    switch (e->kind) {
    case NK_IDENT: {
        Symbol *s = e->sym;
        if (!s || s->nassign != 0 || s->escaped) return NULL;
        if (s->cap_src) return alloc_factory_module(s->cap_src, depth + 1);
        if (!s->decl || s->decl->kind != NK_DECL || !s->decl->b) return NULL;
        return alloc_factory_module(s->decl->b, depth + 1);
    }
    case NK_CAST:
        return alloc_factory_module(e->b, depth + 1);
    case NK_UNWRAP:
    case NK_TRY:
        return alloc_factory_module(e->a, depth + 1);
    case NK_CALL: {
        Node *callee = e->a;
        Node *fn = NULL;
        if (callee && callee->inst && callee->inst->kind == NK_FN)
            fn = callee->inst;
        else if (callee && callee->sym && callee->sym->decl) {
            Node *d = callee->sym->decl;
            if (d->kind == NK_FN) fn = d;
            else if (d->kind == NK_DECL && d->b && d->b->kind == NK_FN) fn = d->b;
        }
        return (fn && fn->mod) ? module_by_path(fn->mod) : NULL;
    }
    default:
        return NULL;
    }
}

/* §187（用户裁定，通用规则）：建槽只有一个入口——std.heapAlloc /
 * std.heapSlice / std.stackAlloc 内建检查处，进了就建，不按容器/模块
 * 点名豁免。Allocator 接口的 alloc/free 函数指针是实现细节（std 实现
 * 层内部才允许直接碰，用户代码 E0527）。arena 这类「整批管理」的
 * 分配器，其内存的账由两条通用机制接住：
 *   1. 容器账——std 内部模块的 `.init` 调用点自动开账（不点名哪个
 *      容器：std 里叫 init 的构造器都是要 deinit 的资源，新增容器
 *      自动获得泄漏检测），`.deinit` 统一终结；
 *   2. 收养——从带容器账的分配器值里分配出来的槽，当场收养给那个
 *      分配器变量（值级规则，跟容器名无关），随它的 deinit 一起终结。 */
static bool path_is_std_internal(const char *path) {
    if (!path) return false;
    if (g_std_mod && g_std_mod->path && !strcmp(g_std_mod->path, path))
        return true;
    if (g_std_mod && g_std_mod->path) {            /* std 目录下的内部文件 */
        const char *a = strrchr(g_std_mod->path, '/');
        const char *b = strrchr(path, '/');
        const char *la = strrchr(g_std_mod->path, '\\');
        const char *lb = strrchr(path, '\\');
        if (la && (!a || la > a)) a = la;
        if (lb && (!b || lb > b)) b = lb;
        if (a && b && (a - g_std_mod->path) == (b - path) &&
            !memcmp(g_std_mod->path, path, (size_t)(a - g_std_mod->path)))
            return true;
    }
    return false;
}

/* §187：std 资源容器的 init 是容器账开账点——通用规则：被调 init
 * 函数属于 std 内部模块即开账（不点名容器）。调用点在 std 内部的
 * 照旧豁免（实现层内部互相初始化不该记账）。 */
void tag_note_container_init(Node *call) {
    Node *a = call->a;
    if (!a || a->kind != NK_FIELD || !a->name || strcmp(a->name, "init"))
        return;
    if (g_mod && tag_module_exempt(g_mod)) return;
    Node *fn = NULL;
    if (a->inst && a->inst->kind == NK_FN) fn = a->inst;
    else if (a->sym && a->sym->decl) {
        Node *d = a->sym->decl;
        if (d->kind == NK_FN) fn = d;
        else if (d->kind == NK_DECL && d->b && d->b->kind == NK_FN) fn = d->b;
    }
    if (!fn || fn->kind != NK_FN || !path_is_std_internal(fn->mod)) return;
    /* §187 补丁：资源账只开给"从分配器取得资源"的容器——init 实参里
     * 必须有 Allocator（List/Arena 是）；定长 Array.init()（内联存储）、
     * net.init()（全局态）这类不碰分配器的初始化不是资源，不开账。 */
    bool has_al = false;
    for (int i = 0; i < call->nitems; i++) {
        Node *ai = call->items[i];
        if (ai && ai->ty && ty_is_allocator(ai->ty)) { has_al = true; break; }
    }
    if (!has_al) return;
    if (!call->tag_id) tag_auto_decl(call, false);
    TagSlot *t = tag_slot(call->tag_id);
    if (t) t->book = true;
}

/* `X.deinit(a)` / `a.deinit()` —— 总清算：终结容器账 + a 名下全部
 * 收养槽。识别口径：callee 尾段名为 deinit、第一实参（方法调用含
 * 接收者）解析到基变量；没有容器账/收养账的 deinit 调用这里是
 * no-op，不干扰其他 deinit。 */
void tag_note_arena_deinit(Node *call) {
    Node *a = call->a;
    if (!a || a->kind != NK_FIELD || !a->name || strcmp(a->name, "deinit") ||
        call->nitems < 1) return;
    Symbol *ar = aradt_base_of(call->items[0]);
    if (!ar) return;
    /* §187：容器账终结。同点重放（体复检）静默；不同点的二次 deinit
     * 才是 double free（E0509）。 */
    if (ar->tag_id) {
        TagSlot *ct = tag_slot(ar->tag_id);
        if (ct && ct->book) {
            if (ct->state == TST_FREED) {
                if (ct->free_line != call->line || ct->free_col != call->col)
                    diag_code_at("E0509", call->line, call->col,
                        "this resource (`%s`) was already torn down (deinit at "
                        "line %d) -- a second deinit is a double free",
                        ar->name ? ar->name : "?", ct->free_line);
            } else {
                ct->state = TST_FREED;
                ct->ever_freed = true;
                ct->free_line = call->line; ct->free_col = call->col;
                for (Symbol *ls = g_locals; ls; ls = ls->locals_next)
                    if (ls->tag_id == ar->tag_id &&
                        ls->tag_state == TC_LIVE)
                        ls->tag_state = TC_FREED;
            }
        }
    }
    for (int i = 0; i < g_naradt; i++) {
        if (g_aradt[i].ar == ar && !g_aradt[i].done) {
            TagSlot *t = tag_slot(g_aradt[i].slot);
            if (t && t->state == TST_LIVE && t->adopt_ar == ar) {
                t->state = TST_FREED;
                t->ever_freed = true;
                t->free_line = call->line; t->free_col = call->col;
                t->adopt_ar = NULL;
                /* 副本同步（口径同 tag_free_event）：局部载体一起进
                 * FREED，旧副本解引用立即 E0512。 */
                for (Symbol *ls = g_locals; ls; ls = ls->locals_next)
                    if (ls->tag_id == g_aradt[i].slot &&
                        ls->tag_state == TC_LIVE)
                        ls->tag_state = TC_FREED;
            }
            g_aradt[i].done = true;    /* 复检重放幂等 */
        }
    }
}

/* ---------------- §189：std.move —— 所有权转移 ----------------
 *
 * 用户裁定，语义很简单：
 *   `let/var newvar = std.move(oldvar);`
 * 1. oldvar 的标签改 MOVED——之后对 oldvar 的任何使用或 free 都报
 *    E0529（内存还活着，只是不再归 oldvar 管）；
 * 2. 生成新变量 newvar：新标签、值等于原值。新槽就是普通 LIVE 槽，
 *    不 free 照样报 E0513，没有任何豁免。
 * 识别口径与 print/println 一致：裸名 `std.move(...)`；`const s = std;
 * s.move(...)` 别名不改道。spawn 不做自动 move（用户裁定：当普通函数）。 */

bool call_is_std_move(Node *call) {
    return call->a && call->a->kind == NK_FIELD && call->a->name &&
           !strcmp(call->a->name, "move") && call->a->a &&
           call->a->a->kind == NK_IDENT && call->a->a->name &&
           !strcmp(call->a->a->name, "std");
}
