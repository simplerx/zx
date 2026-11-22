/* backend.c — C 代码生成（M5）。
 *
 * 后端不做任何"聪明的优化"：它只把已经通过类型检查的树，一对一地翻成 C。
 * 三条约定：
 *
 *  1. **类型只由 ty.c 决定**。每个 Type 对应一个 C 类型，结构类型按需生成
 *     typedef（`ZxSlice_i32` / `ZxTup_i32_f64` / `ZxOpt_i32` …），聚合类型
 *     用 `ZxS<n>` / `ZxU<n>` / `ZxE<n>`，编号来自那条类型声明的 AST 节点。
 *
 *  2. **控制流表达式落在槽位里**。C 的 if/switch/for 都是语句，所以
 *     「有值的 if / 块 / 循环 / switch」统一编译成「把值写进一个临时变量 +
 *     goto 出口」。标签收割直接变成 `slot = v; goto Lend;`，`continue`
 *     跳到每轮末尾的 `Lcont:`（在 defer 之前，这样 defer 仍然会跑）。
 *
 *  3. **defer 是编译期展开**。进入作用域时把 defer 记在栈上，离开时逆序
 *     内联展开；`return` / 收割 / `continue` 都会先展开"要离开的那些作用域"
 *     的 defer，所以提前退出不会漏掉清理。
 */
#include "zxc.h"
#include "ast.h"
#include "val.h"
#include "ty.h"
#include "val.h"
#include <stdarg.h>

/* ======================================================================
 *  输出
 * ====================================================================== */

static Buf  g_btypes;      /* typedef */
static Buf  g_bglobals;    /* 顶层变量 / 静态成员 */
static Buf  g_bproto;      /* 函数原型 */
static Buf  g_bbody;       /* 函数体 */
static Buf *g_out;
static int  g_ind;

static void cname(Buf *b, const char *s);   /* 成员名转义（定义在下方），ensure_type 先用到 */
static void cstr(Buf *b, const char *s, int n);  /* C 字符串字面量：静态成员初始化先用 */
static void mname(Buf *b, const char *s);
static void dotname(Buf *b, const char *s);
static void arrowname(Buf *b, const char *s);
static bool val_has_fn(Value *v);   /* §117 值槽：静态成员初始化先用（定义在下方） */
static void val_init(Buf *b, Value *v, Type *t);

/* ======================================================================
 *  行映射：生成的 C 行 → zx 源码位置
 *
 *  产物是 C，但用户写的是 zx。C 编译器一旦报错（后端生成了它不收的代码），
 *  默认输出指向缓存目录里的 .c 第几百行——对用户毫无意义。这里在发射每个
 *  语句/表达式时记一笔「当前输出行 ← 当前节点」，驱动层据此把 C 的
 *  file:line 翻回 「模块:行:列」，诊断就不再泄漏后端细节。
 *
 *  只记最外层节点：ex() 是递归的，同一输出行进来的第一个（也就是最外层
 *  那个语句）才是用户能认的位置。
 * ====================================================================== */

typedef struct { int unit, buf, line, zx_line, zx_col; } BMapEnt;

static BMapEnt *g_bmap;
static int      g_nbmap, g_cbmap;
static int      g_cur_unit;                 /* 当前正在发射的模块下标 */
/* 三个正文缓冲各自的增量扫描状态（内容只追加，所以可以增量数换行） */
static size_t   g_scan_pos[3];
static int      g_scan_lines[3];

static int buf_slot(Buf *b) {
    if (b == &g_bproto)   return 0;
    if (b == &g_bglobals) return 1;
    if (b == &g_bbody)    return 2;
    return -1;                              /* 头文件 / 临时缓冲：不记 */
}

/* 该缓冲"下一条将被写入的行"（1 基） */
static int buf_line_now(int slot) {
    Buf *b = slot == 0 ? &g_bproto : slot == 1 ? &g_bglobals : &g_bbody;
    if (g_scan_pos[slot] > b->len) { g_scan_pos[slot] = 0; g_scan_lines[slot] = 0; }
    int nl = g_scan_lines[slot];
    for (size_t i = g_scan_pos[slot]; i < b->len; i++)
        if (b->data[i] == '\n') nl++;
    g_scan_pos[slot] = b->len;
    g_scan_lines[slot] = nl;
    return nl + 1;
}

static int count_lines(const char *s) {
    if (!s) return 0;
    int n = 0;
    for (const char *p = s; *p; p++) if (*p == '\n') n++;
    /* 末尾没有换行符也算一行 */
    size_t l = strlen(s);
    if (l > 0 && s[l - 1] != '\n') n++;
    return n;
}

static void bmap_note(Buf *b, Node *n) {
    if (!n || n->line <= 0) return;
    int slot = buf_slot(b);
    if (slot < 0) return;
    int ln = buf_line_now(slot);
    if (g_nbmap > 0) {
        BMapEnt *p = &g_bmap[g_nbmap - 1];
        if (p->unit == g_cur_unit && p->buf == slot && p->line == ln) return;  /* 同一行只留最外层 */
    }
    if (g_nbmap == g_cbmap) {
        g_cbmap = g_cbmap ? g_cbmap * 2 : 512;
        g_bmap = realloc(g_bmap, sizeof(BMapEnt) * (size_t)g_cbmap);
        if (!g_bmap) { fprintf(stderr, "zxc: out of memory\n"); exit(1); }
    }
    g_bmap[g_nbmap].unit    = g_cur_unit;
    g_bmap[g_nbmap].buf     = slot;
    g_bmap[g_nbmap].line    = ln;
    g_bmap[g_nbmap].zx_line = n->line;
    g_bmap[g_nbmap].zx_col  = n->col;
    g_nbmap++;
}

/* 驱动层查询：生成的 .c 第 cline 行（含开头两行 include）对应哪个 zx 位置。
 * 找到就填 *zx_line 与 *zx_col 并返回 true；没有映射则 false（调用方按
 * "无法定位"处理，不要编造行号）。 */
bool backend_map_lookup(int unit, int cline, int *zx_line, int *zx_col) {
    if (getenv("ZX_TRACE_MAP")) fprintf(stderr, "[map] unit=%d cline=%d n=%d\n", unit, cline, g_nbmap);
    int best = -1;
    for (int i = 0; i < g_nbmap; i++)
        if (g_bmap[i].unit == unit && g_bmap[i].line <= cline) best = i;
    if (best < 0 || g_bmap[best].unit != unit) return false;
    if (zx_line) *zx_line = g_bmap[best].zx_line;
    if (zx_col)  *zx_col  = g_bmap[best].zx_col;
    return true;
}

static void vfmt(Buf *b, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) return;
    char *tmp = malloc((size_t)n + 1);
    vsnprintf(tmp, (size_t)n + 1, fmt, ap);
    buf_puts(b, tmp);
    free(tmp);
}

static void line(const char *fmt, ...) {
    for (int i = 0; i < g_ind; i++) buf_puts(g_out, "    ");
    va_list ap;
    va_start(ap, fmt);
    vfmt(g_out, fmt, ap);
    va_end(ap);
    buf_putc(g_out, '\n');
}

static void bline(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfmt(b, fmt, ap);
    va_end(ap);
    buf_putc(b, '\n');
}

static void ex(Buf *b, Node *n);
static void line(const char *fmt, ...);

/* ======================================================================
 *  类型名
 * ====================================================================== */

typedef struct { Type *t; int state; } TEnt;
static TEnt *g_ts;
static int   g_nts, g_cts;

static int tslot(Type *t) {
    for (int i = 0; i < g_nts; i++) if (g_ts[i].t == t) return i;
    if (g_nts == g_cts) {
        g_cts = g_cts ? g_cts * 2 : 64;
        g_ts = realloc(g_ts, sizeof(TEnt) * (size_t)g_cts);
    }
    g_ts[g_nts].t = t;
    g_ts[g_nts].state = 0;
    return g_nts++;
}

/* 聚合类型（struct/union/enum）的 C 名：同一个声明节点 → 同一个编号 */
static void agg_name(Buf *b, Type *t) {
    static Node *decls[4096];
    static int   n;
    int id = -1;
    for (int i = 0; i < n; i++) if (decls[i] == t->decl) { id = i + 1; break; }
    if (id < 0) {
        if (n >= 4096) { id = 4096; }
        else { decls[n++] = t->decl; id = n; }
    }
    buf_printf(b, "Zx%c%d", t->kind == TY_STRUCT ? 'S' : t->kind == TY_UNION ? 'U' : 'E', id);
}

/* 类型名后缀：只由类型结构决定，所以天然唯一 */
static void tsuffix(Buf *b, Type *t) {
    if (!t) { buf_puts(b, "void"); return; }
    switch (t->kind) {
    case TY_VOID:     buf_puts(b, "void"); break;
    case TY_NORETURN: buf_puts(b, "noret"); break;
    case TY_BOOL:     buf_puts(b, "bool"); break;
    case TY_ERROR:    buf_puts(b, "err");  break;
    case TY_TYPE:     buf_puts(b, "type"); break;
    case TY_TAG:      buf_puts(b, "tag"); break;
    case TY_INT:
        if (t->bits == 64 && !t->is_signed) buf_puts(b, "usize");
        else if (t->bits == 64)             buf_puts(b, "isize");
        else buf_printf(b, "%c%d", t->is_signed ? 'i' : 'u', t->bits);
        break;
    case TY_FLOAT: buf_printf(b, "f%d", t->bits); break;
    case TY_PTR:
        buf_puts(b, "p");
        tsuffix(b, t->elem);       /* elem 已是 [N]T，后缀自带长度 */
        break;
    case TY_ARRAY:
        buf_puts(b, "a"); tsuffix(b, t->elem);
        buf_printf(b, "_%llu", (unsigned long long)t->len);
        break;
    case TY_SLICE: buf_puts(b, "s"); tsuffix(b, t->elem); break;
    case TY_OPT:   buf_puts(b, "o"); tsuffix(b, t->elem); break;
    case TY_ERR:   buf_puts(b, "e"); tsuffix(b, t->elem); break;
    case TY_VEC:
        /* 分量数与元素都进名字：@Vec(f32,4) → vf32_4 */
        buf_puts(b, "v");
        tsuffix(b, t->elem);
        buf_printf(b, "_%llu", (unsigned long long)t->len);
        break;
    case TY_TUPLE:
        buf_puts(b, "t");
        for (int i = 0; i < t->nfields; i++) {
            if (i) buf_putc(b, '_');
            tsuffix(b, t->fields[i].ty);
        }
        break;
    case TY_FN:
        buf_puts(b, "fn");
        for (int i = 0; i < t->nparams; i++) {
            buf_putc(b, '_');
            tsuffix(b, t->params[i]);
        }
        buf_puts(b, "_r");
        tsuffix(b, t->ret);
        break;
    default: agg_name(b, t); break;
    }
}

static void tn(Buf *b, Type *t) {
    switch (t->kind) {
    case TY_SLICE: buf_puts(b, "ZxSlice_"); tsuffix(b, t->elem); break;
    case TY_OPT:   buf_puts(b, "ZxOpt_");   tsuffix(b, t->elem); break;
    case TY_ERR:   buf_puts(b, "ZxErr_");   tsuffix(b, t->elem); break;
    case TY_ARRAY: buf_puts(b, "ZxArr_");   tsuffix(b, t);       break;  /* §201 */
    case TY_TUPLE: buf_puts(b, "ZxTup_");   tsuffix(b, t);       break;
    case TY_FN:    buf_puts(b, "ZxFn_");    tsuffix(b, t);       break;
    case TY_VEC:   buf_puts(b, "ZxVec_");   tsuffix(b, t);       break;
    default: agg_name(b, t); break;
    }
}

static void ctype(Buf *b, Type *t);
static void ensure_type(Type *t);

/* 只要"名字"不要"定义"：`?*Node` / `[]Node` 的 typedef 里写的是
 * `Node *payload`——指针指向不完整类型在 C 里是合法的，所以这里
 * 只落 `typedef struct Node Node;`，完整定义留给后续的 ensure_type
 * （state=3 表示"名字已落，身体待补"）。
 *
 * 为什么需要它：自引用的 optional 会把顺序打环——
 *   ensure(?*Region) → ensure(Region) → Region 的字段又是 ?*Region
 *   （state=1，跳过）→ Region 的 body 引用还没发射的 Opt typedef。
 * 先把名字落下来，环就断了。 */
static void ensure_fwd(Type *t) {
    if (!t) return;
    if (t->kind == TY_PTR) { ensure_fwd(t->elem); return; }
    if (t->kind != TY_STRUCT && t->kind != TY_UNION) { ensure_type(t); return; }
    int i = tslot(t);
    if (g_ts[i].state) return;          /* 1/2/3：名字都已经在（或正在） */
    g_ts[i].state = 3;
    Buf *save = g_out;
    g_out = &g_btypes;
    Buf n; buf_init(&n);
    tn(&n, t);
    buf_printf(g_out, t->kind == TY_STRUCT ? "typedef struct %s %s;\n"
                                           : "typedef union %s %s;\n",
               n.data, n.data);
    g_out = save;
}

static void ensure_type(Type *t) {
    if (!t) return;
    /* 指针自己不占 typedef，但被指向的结构体得先有名字：
     * `?*Node` 的 typedef 里会写 `ZxS1 *payload;`。 */
    if (t->kind == TY_PTR) { ensure_type(t->elem); return; }
    switch (t->kind) {
    case TY_SLICE: case TY_OPT: case TY_ERR: case TY_TUPLE:
    case TY_FN: case TY_STRUCT: case TY_UNION: case TY_ENUM: case TY_VEC:
    case TY_ARRAY:                  /* §201 */
        break;
    default: return;
    }
    int i = tslot(t);
    /* state=3（ensure_fwd 只落过前向声明）要继续把身体补完 */
    if (g_ts[i].state == 1 || g_ts[i].state == 2) return;
    bool need_fwd = (g_ts[i].state == 0);
    g_ts[i].state = 1;

    Buf *save = g_out;
    g_out = &g_btypes;
    Buf n; buf_init(&n);
    tn(&n, t);

    /* 两个 Type 可能映射到同一个 C 名（[]u8 与 []const u8 的表示一样）；
     * 名字已经发过就不再发第二份 typedef。 */
    for (int k = 0; k < g_nts; k++) {
        if (k == i || g_ts[k].state != 2) continue;
        Buf o; buf_init(&o);
        tn(&o, g_ts[k].t);
        if (!strcmp(o.data, n.data)) { g_ts[i].state = 2; g_out = save; return; }
    }

    switch (t->kind) {
    case TY_SLICE: {
        if (t->elem->kind == TY_STRUCT || t->elem->kind == TY_UNION) {
            /* 元素按指针存：名字先落，typedef 随后，完整定义最后补。
             * 载荷类型名直接用 tn（ctype 会对结构体顺手 ensure_type，
             * 把身体插进这行 typedef 中间）。 */
            ensure_fwd(t->elem);
            Buf pn; buf_init(&pn);
            tn(&pn, t->elem);
            buf_printf(g_out, "typedef struct { %s *ptr; size_t len; } %s;\n",
                       pn.data, n.data);
            ensure_type(t->elem);
        } else {
            ensure_type(t->elem);
            buf_puts(g_out, "typedef struct { ");
            ctype(g_out, t->elem);
            buf_printf(g_out, " *ptr; size_t len; } %s;\n", n.data);
        }
        break;
    }
    case TY_OPT: {
        if (ty_opt_flat(t)) {
            /* ?*T（M26）：无标签 optional，C 表示就是 T*，没有 typedef。
             * 只要保证指过去的类型名在。 */
            ensure_type(t->elem);
            break;
        }
        if (t->elem->kind == TY_PTR &&
            (t->elem->elem->kind == TY_STRUCT || t->elem->elem->kind == TY_UNION)) {
            /* 同 SLICE：`?*T` 的载荷是 `T *`，只要名字。
             * 不先落名字的话，`?*Region` 嵌在 Region 自己的字段里时
             * （Region 的 body 引用 Opt 的名字），Opt 的 typedef 会
             * 被排到 body 之后，C 编译器直接报 unknown type。 */
            ensure_fwd(t->elem->elem);
            Buf pn; buf_init(&pn);
            tn(&pn, t->elem->elem);
            buf_printf(g_out, "typedef struct { %s * payload; bool tag; } %s;\n",
                       pn.data, n.data);
            ensure_type(t->elem);   /* 完整定义照常补上 */
        } else {
            ensure_type(t->elem);
            buf_puts(g_out, "typedef struct { ");
            ctype(g_out, t->elem);
            buf_printf(g_out, " payload; bool tag; } %s;\n", n.data);
        }
        break;
    }
    case TY_ERR: {
        ensure_type(t->elem);
        buf_puts(g_out, "typedef struct { ");
        /* `!void` 只有 tag 一个字段：C 里写 `void payload;` 是非法的，而且
         * 0 字节的 payload 本来也没有意义。 */
        if (t->elem->kind != TY_VOID) {
            ctype(g_out, t->elem);
            buf_puts(g_out, " payload; ");
        }
        buf_printf(g_out, "uint16_t tag; } %s;\n", n.data);
        break;
    }
    case TY_ARRAY: {                /* §201：[N]T → struct { T v[N]; } */
        ensure_type(t->elem);
        buf_printf(g_out, "typedef struct { ");
        ctype(g_out, t->elem);
        buf_printf(g_out, " v[%llu]; } %s;\n", (unsigned long long)t->len, n.data);
        break;
    }
    case TY_TUPLE: {
        for (int k = 0; k < t->nfields; k++) ensure_type(t->fields[k].ty);
        buf_printf(g_out, "typedef struct { ");
        /* §151：0 大小聚合在 C 里是编译器扩展（GCC sizeof=0 / MSVC =1），
         * 补一个占位成员让布局跨平台稳定；zx 侧 ty_size 仍是 0。 */
        if (!t->nfields) buf_puts(g_out, "uint8_t _zx_pad; ");
        for (int k = 0; k < t->nfields; k++) {
            ctype(g_out, t->fields[k].ty);
            buf_printf(g_out, " f%d; ", k);
        }
        buf_printf(g_out, "} %s;\n", n.data);
        break;
    }
    case TY_FN: {
        ensure_type(t->ret);
        for (int k = 0; k < t->nparams; k++) ensure_type(t->params[k]);
        buf_printf(g_out, "typedef ");
        ctype(g_out, t->ret);
        buf_printf(g_out, " (*%s)(", n.data);
        if (t->nparams == 0) buf_puts(g_out, "void");
        for (int k = 0; k < t->nparams; k++) {
            if (k) buf_puts(g_out, ", ");
            ctype(g_out, t->params[k]);
        }
        buf_puts(g_out, ");\n");
        break;
    }
    case TY_VEC: {
        /* §162：语言级向量直接落到 clang/gcc 的 vector_size 类型上——
         * 语言层不做 SIMD 抽象，用不用向量指令交给 C 编译器。
         * 对齐规则与 ty_align 一致（对齐 = 大小），否则同一种值在 zx 侧
         * 和 C 侧算出来的结构体布局会错位。
         *
         * 随类型一起发一个 splat 助手：C 的 vector_size 类型不接受
         * scalar cast（clang 直接报 invalid conversion），只能靠复合
         * 字面量逐分量铺。写成函数是为了让实参只求值一次。 */
        ensure_type(t->elem);
        int vbytes = ty_size(t);
        Buf sn; buf_init(&sn);
        tsuffix(&sn, t);
        buf_puts(g_out, "typedef ");
        ctype(g_out, t->elem);
        buf_printf(g_out, " %s __attribute__((vector_size(%d)));\n",
                   n.data, vbytes > 0 ? vbytes : 1);
        buf_printf(g_out, "static %s zx_splat_%s(", n.data, sn.data);
        ctype(g_out, t->elem);
        buf_puts(g_out, " x) { return (");
        buf_puts(g_out, n.data);
        buf_puts(g_out, "){");
        for (uint64_t k = 0; k < t->len; k++) buf_printf(g_out, "%sx", k ? ", " : "");
        buf_puts(g_out, "}; }\n");

        break;
    }
    case TY_STRUCT: case TY_UNION: {
        /* 前向声明**先**落地，再处理字段类型：
         * 字段可能是 `*Node`（指向自己），也可能是 `?*Node` 这种"引用自己的
         * 另一个 typedef"——两种都要求名字在别人引用它之前就存在。
         * state=3 进来的（ensure_fwd 已落过名字）就不重复发了。 */
        bool tagged_u = (t->kind == TY_UNION && t->is_tagged && t->tag_ty);
        if (need_fwd) {
            buf_printf(g_out, t->kind == TY_STRUCT || tagged_u
                                  ? "typedef struct %s %s;\n" : "typedef union %s %s;\n",
                       n.data, n.data);
        }
        for (int k = 0; k < t->nfields; k++)
            if (!t->fields[k].is_static) ensure_type(t->fields[k].ty);

        if (t->kind == TY_STRUCT) {
            buf_printf(g_out, "struct %s {\n", n.data);
            for (int k = 0; k < t->nfields; k++) {
                Field *f = &t->fields[k];
                if (f->is_static) continue;
                buf_puts(g_out, "    ");
                /* §201：`[N]T` 的 C 表示是 ZxArr_… 结构体（里面就一个
                 * `T v[N]`），布局与 C 原生数组成员一致，发码也统一走 .v。 */
                ctype(g_out, f->ty);
                buf_putc(g_out, ' ');
                mname(g_out, f->name);
                buf_puts(g_out, ";\n");
            }
            int ndata = 0;
            for (int k = 0; k < t->nfields; k++)
                if (!t->fields[k].is_static) ndata++;
            /* §151：空 struct 是 C 编译器扩展，补占位成员。 */
            if (!ndata) buf_puts(g_out, "    uint8_t _zx_pad;\n");
            buf_puts(g_out, "};\n");

            /* 静态成员：结构体体里声明的 var / const，全程序一份 */
            for (int k = 0; k < t->nfields; k++) {
                Field *f = &t->fields[k];
                if (!f->is_static) continue;
                /* 静态**类型**成员（`const Inner = struct { … };`）：它是
                 * 类型不是值，没有运行期表示。以前照样物化成 C 变量，
                 * ctype 把类型发成 void、初值是 struct 类型字面量节点，
                 * 后端直接撞 E0300 + B1003（用户 2026-11-10 报的 B1001
                 * 同一条线上的坑）。 */
                if (f->ty && (f->ty->kind == TY_TYPE || f->ty->kind == TY_TAG)) continue;
                Buf s; buf_init(&s);
                buf_puts(&s, "static ");
                ctype(&s, f->ty);
                buf_printf(&s, " %s_", n.data);
                mname(&s, f->name);
                if (f->dflt) {
                    buf_puts(&s, " = ");
                    /* 切片类型的字符串初值：C 里 `static ZxSlice_u8 x =
                     * (uint8_t*)"…"` 不是合法的聚合初始化（会报
                     * incompatible type）。这里显式发 {ptr, len}。 */
                    if (f->ty && f->ty->kind == TY_SLICE && f->dflt->kind == NK_STR) {
                        Buf c; buf_init(&c);
                        cstr(&c, f->dflt->str ? f->dflt->str : "", f->dflt->str_len);
                        Buf ts; buf_init(&ts);
                        ctype(&ts, f->ty);
                        buf_printf(&s, "((%s){ (uint8_t*)%s, %d })",
                                   ts.data, c.data, f->dflt->str_len);
                    } else if (f->ct && !val_has_fn(f->ct)) {
                        /* §117 值槽优先：关联常量的初值 sema 已折成编译期值
                         * （泛型容器里 `N * @type(T).size` 这类只活编译期的
                         * 子式，克隆体 ct 洗掉后裸发会变成 `0.size` 这种非法
                         * C——B1003 "invalid suffix 'size'"）。 */
                        val_init(&s, f->ct, f->ty);
                    } else {
                        ex(&s, f->dflt);
                    }
                }
                buf_puts(&s, ";\n");
                buf_puts(g_out, s.data);      /* 跟类型定义一起进头文件 */
            }
        } else if (t->is_tagged && t->tag_ty) {
            /* 带标签联合体：{ 载荷, 标签 }，载荷在前、标签在后（规格 §3.4，
             * 和 ?T 的 {值, ok} 同一个布局方向；union(enum) 糖共用此布局） */
            ensure_type(t->tag_ty);
            buf_printf(g_out, "struct %s {\n", n.data);
            buf_puts(g_out, "    union {\n");
            for (int k = 0; k < t->nfields; k++) {
                Field *f = &t->fields[k];
                if (f->is_static) continue;
                buf_puts(g_out, "        ");
                ctype(g_out, f->ty);
                buf_putc(g_out, ' ');
                mname(g_out, f->name);
                buf_puts(g_out, ";\n");
            }
            buf_puts(g_out, "    } u;\n");
            buf_puts(g_out, "    ");
            ctype(g_out, t->tag_ty);
            buf_printf(g_out, " tag;\n");
            buf_puts(g_out, "};\n");
        } else {
            buf_printf(g_out, "union %s {\n", n.data);
            /* §151：空 union 是 C 编译器扩展，补占位成员。 */
            {
                int ndata = 0;
                for (int k = 0; k < t->nfields; k++)
                    if (!t->fields[k].is_static) ndata++;
                if (!ndata) buf_puts(g_out, "    uint8_t _zx_pad;\n");
            }
            for (int k = 0; k < t->nfields; k++) {
                Field *f = &t->fields[k];
                if (f->is_static) continue;
                buf_puts(g_out, "    ");
                ctype(g_out, f->ty);
                buf_putc(g_out, ' ');
                mname(g_out, f->name);
                buf_puts(g_out, ";\n");
            }
            buf_puts(g_out, "};\n");
        }
        break;
    }
    case TY_ENUM: {
        Type *bt = t->elem ? t->elem : ty_i32();
        buf_printf(g_out, "typedef ");
        ctype(g_out, bt);
        buf_printf(g_out, " %s;\n", n.data);
        buf_printf(g_out, "enum {\n");
        for (int k = 0; k < t->nfields; k++) {
            Field *f = &t->fields[k];
            buf_printf(g_out, "    %s_", n.data);
            mname(g_out, f->name);
            buf_printf(g_out, " = %llu,\n",
                       (unsigned long long)(f->has_tag ? f->tag : (uint64_t)k));
        }
        buf_puts(g_out, "};\n");

        /* 枚举体里的 `const` / `var` 成员：和结构体的静态成员一样，全程序一份 */
        for (int k = 0; k < t->nstatics; k++) {
            Field *f = &t->statics[k];
            /* 类型成员没有 C 表示（同 struct 静态成员） */
            if (f->ty && (f->ty->kind == TY_TYPE || f->ty->kind == TY_TAG)) continue;
            ensure_type(f->ty);
            Buf s; buf_init(&s);
            buf_puts(&s, "static ");
            ctype(&s, f->ty);
            buf_printf(&s, " %s_", n.data);
            mname(&s, f->name);
            if (f->dflt) {
                buf_puts(&s, " = ");
                ex(&s, f->dflt);
            }
            buf_puts(&s, ";\n");
            buf_puts(g_out, s.data);          /* 跟类型定义一起进头文件 */
        }
        break;
    }
    default: break;
    }

    g_ts[i].state = 2;         /* 发完了：后面同名/同构的直接跳过 */
    g_out = save;
}

static void ctype(Buf *b, Type *t) {
    if (!t) { buf_puts(b, "void"); return; }
    switch (t->kind) {
    case TY_VOID:     buf_puts(b, "void"); break;
    case TY_NORETURN: buf_puts(b, "void"); break;
    case TY_BOOL:     buf_puts(b, "bool"); break;
    case TY_ERROR:    buf_puts(b, "uint16_t"); break;
    case TY_TYPE:     buf_puts(b, "void"); break;
    case TY_TAG:      buf_puts(b, "void"); break;
    case TY_INT:
        if (t->bits == 8)  buf_puts(b, t->is_signed ? "int8_t"  : "uint8_t");
        else if (t->bits == 16) buf_puts(b, t->is_signed ? "int16_t" : "uint16_t");
        else if (t->bits == 32) buf_puts(b, t->is_signed ? "int32_t" : "uint32_t");
        else if (t->bits == 64) buf_puts(b, t->is_signed ? "int64_t" : "uint64_t");
        else buf_puts(b, t->is_signed ? "__int128" : "unsigned __int128");
        break;
    case TY_FLOAT:
        buf_puts(b, t->bits == 16 ? "_Float16" : t->bits == 32 ? "float" : "double");
        break;
    case TY_COMPTIME_INT:   buf_puts(b, "int64_t"); break;
    case TY_COMPTIME_FLOAT: buf_puts(b, "double"); break;
    case TY_NULL:  buf_puts(b, "bool"); break;
    case TY_UNDEF: buf_puts(b, "char"); break;
    case TY_PTR:
        ctype(b, t->elem);          /* elem 是 [N]T 的 typedef → "ZxA.. *" */
        buf_puts(b, " *");
        break;
    case TY_SLICE: case TY_OPT: case TY_ERR: case TY_TUPLE: case TY_ARRAY:
    case TY_FN: case TY_STRUCT: case TY_UNION: case TY_ENUM: case TY_VEC: {
        if (t->kind == TY_OPT && ty_opt_flat(t)) {   /* ?*T：C 表示就是 T* */
            ctype(b, t->elem);
            return;
        }
        ensure_type(t);
        Buf n; buf_init(&n);
        tn(&n, t);
        buf_puts(b, n.data);
        break;
    }
    default: buf_puts(b, "void"); break;
    }
}

/* extern 原型专用：ctype 有意把 `*const T` 擦成 `T*`（Zx 内部只影响 C 的
 * 诊断），但和 libc 的 `const char *` 对上时 const 必须在——否则
 * `conflicting types for 'puts'`。
 * 另外 `*u8` 在 FFI 边界上映射成 `char*`：Zx 没有 char 类型，一个字节就是
 * u8；libc 的字符串函数收的都是 `const char*`，用 uint8_t* 声明会和
 * stdio.h 冲突。 */
static void ctype_c(Buf *b, Type *t) {
    if (t && t->kind == TY_PTR) {
        if (t->is_const) buf_puts(b, "const ");
        Type *e = t->elem;
        if (e && e->kind == TY_INT && e->bits == 8) buf_puts(b, "char");
        else ctype_c(b, e);
        buf_puts(b, " *");
        return;
    }
    ctype(b, t);
}

static char *cty(Type *t) {
    Buf b; buf_init(&b);
    ctype(&b, t);
    return b.data;
}

/* `!void`：payload 是 0 字节，整个值就是一个 u16 标签。成功 = tag 0，
 * 所以"正常返回"在 C 里是 `{ .tag = 0 }`，不是 `return;`（C 函数返回结构体）。 */
static bool ret_is_errvoid(Type *t) {
    return t && t->kind == TY_ERR && t->elem && t->elem->kind == TY_VOID;
}

/* ======================================================================
 *  符号名
 * ====================================================================== */

/* 生成的 C 会 include 这几个头：里面的函数**已经声明过**。
 * extern 声明撞上这些名字时不能再发一遍原型（`int32_t puts(...)` 会和
 * `int puts(const char *)` 打架），直接用头里的声明。 */
static bool libc_declared(const char *s) {
    static const char *lib[] = {
        "abs","labs","llabs","div","printf","fprintf","sprintf","snprintf","vprintf",
        "puts","putchar","getchar","scanf","sscanf","fopen","fclose","fread","fwrite",
        "malloc","calloc","realloc","free","exit","abort","atexit","atoi","atol","atof",
        "strtol","strtoul","strtod","qsort","bsearch","getenv","system",
        "memcpy","memmove","memset","memcmp","strcpy","strncpy","strcat","strncat",
        "strcmp","strncmp","strlen","strchr","strrchr","strstr","strdup","index",
        "sqrt","pow","sin","cos","tan","log","exp","floor","ceil","round","fmod","fabs",
        "NULL","EOF","FILE",
        NULL
    };
    for (int i = 0; lib[i]; i++) if (!strcmp(s, lib[i])) return true;
    return false;
}

/* C 关键字 + 我们 include 的那些头文件里的公共名字：
 * 撞上任何一个都加个下划线后缀，免得把 abs() 之类重定义掉。 */
static bool c_keyword(const char *s) {
    if (libc_declared(s)) return true;
    static const char *kw[] = {
        /* C11 关键字 */
        "auto","break","case","char","const","continue","default","do","double",
        "else","enum","extern","float","for","goto","if","inline","int","long",
        "register","restrict","return","short","signed","sizeof","static","struct",
        "switch","typedef","union","unsigned","void","volatile","while","bool",
        "true","false","main","noreturn",
        /* stdint / stddef / stdbool 里的类型名 */
        "size_t","ptrdiff_t","intptr_t","uintptr_t","intmax_t","uintmax_t",
        "int8_t","int16_t","int32_t","int64_t","uint8_t","uint16_t","uint32_t","uint64_t",
        NULL
    };
    for (int i = 0; kw[i]; i++) if (!strcmp(s, kw[i])) return true;
    return false;
}

static void cname(Buf *b, const char *s) {
    if (!s) { buf_puts(b, "_"); return; }
    if (c_keyword(s)) buf_printf(b, "%s_", s);
    else buf_puts(b, s);
}

/* 成员名发射：与访问路径（lvalue 等处的 cname）同一套关键字转义。
 * 类型定义和访问点必须拼出同一个 C 名——zx 结构体/枚举成员可以叫
 * `int`、`float`（它们不是 zx 关键字），定义侧不转义就会生成非法 C，
 * 而访问侧转义成 `int_`，两边对不上（自举 lex.zx 的 Kind 枚举首次踩中）。 */
static void mname(Buf *b, const char *s) {
    cname(b, s && *s ? s : "?");
}

/* 字段访问点。定义侧走 mname 转义了，访问侧也必须走同一套，否则
 * `alloc.free` 这种名字会出现"结构体里叫 free_、访问却写 free"的裂口
 * （C 编译器直接报 no member named 'free'）。 */
static void dotname(Buf *b, const char *s) {
    buf_putc(b, '.');
    mname(b, s && *s ? s : "?");
}

static void arrowname(Buf *b, const char *s) {
    buf_puts(b, "->");
    mname(b, s && *s ? s : "?");
}

/* 符号的 C 名字：非入口模块的顶层名字已经带了模块前缀（见 sema） */
static const char *sym_cname(Node *n) {
    const char *c = n && n->sym ? sema_sym_cname(n->sym) : NULL;
    if (c) return c;
    const char *s = n && n->sym ? sema_sym_name(n->sym) : NULL;
    if (s) return s;
    return n ? n->name : NULL;
}

/* 符号名发射：extern 用原符号名（不转义），其他走 cname 的关键字转义。
 * `puts` 这类名字在转义表里（避免和 include 进来的 stdio 撞车），但
 * extern 声明的就是要那个外部符号，加了下划线反而链接不到。 */
static void emit_sym_name(Buf *b, Node *n) {
    if (n && n->sym && sema_sym_is_extern(n->sym)) {
        const char *cn = sym_cname(n);
        if (cn) buf_puts(b, cn);
        return;
    }
    cname(b, sym_cname(n));
}

static char *cnames(const char *s) {
    Buf b; buf_init(&b);
    cname(&b, s);
    return b.data;
}

/* `_` 是丢弃占位、不进作用域，但 C 里还得有名字承载初值的副作用。
 * 每次绑定给一个独立的哑名：同一函数里写两次 `let _ = …` 才不会
 * 撞成 C 重定义（名字不会被任何引用点用到）。 */
static char *ignored_cname(void) {
    static int n;
    char *s = (char *)malloc(24);
    if (!s) return NULL;
    snprintf(s, 24, "_ignored_%d", n++);
    return s;
}

/* ======================================================================
 *  泛型实例：每个实例都是独立的一份函数，名字沿用声明名 + 序号
 * ====================================================================== */

typedef struct {
    Node *fn;          /* specialize() 克隆出来的那份 */
    char *name;        /* 已经 mangling 过的 C 名 */
} InstEnt;

static InstEnt *g_itab;
static int      g_nitab, g_citab;

static bool g_need_bin;      /* 用过 {b} 就要带上二进制打印辅助函数 */
static bool g_need_pad;      /* 用过 {:0>2} / {:^7} 这类非 printf 原生填充 */
static bool g_need_f64str;   /* 浮点按"最短往返"打印，需要 zx_f64_str */
static bool g_need_fmt;      /* fmt 不是编译期常量 → 带上运行时格式化器 */
static bool g_need_allocfmt; /* 用过 std.format → 还要带上 zx_format */
static bool g_need_file;    /* 用过 std.file → 带上 zx_file_* */
static bool g_need_io;      /* 用过 std.io → 带上 zx_io_*（句柄流） */
static bool g_need_proc;    /* 用过 std.process → 带上 zx_proc_* */
static bool g_need_debug;   /* 用过 std.debug → 带上 zx_debug_* */
static bool g_need_sync;  /* 用过 std.atomic → 带上原子运行时 */
static bool g_need_task;   /* 用过 std.Task / else 捕获 / printTrace → 实例调度器 + 捕获环 */
static const char *g_cur_mod_path;  /* 正在发射的模块源路径（捕获点记录用） */
static const char *g_cur_fn_zx;     /* 正在发射的函数 Zx 名（捕获点记录用） */
static bool g_need_page;     /* 用到 std.heap.page → 带上页分配器 */
static bool g_need_any;      /* 打印过聚合值 → 带上结构展开器与类型描述符 */
static bool g_need_div;      /* 用过整数 / % → 带上除零 trap 辅助（M19） */
static bool g_need_i128;     /* 打印过 i128/u128 → 带上 128 位转十进制（M19） */
static Buf  g_bany;          /* 类型描述符的辅助数组（字段表 / 名字表） */
static char **g_anydef;      /* 每个描述符的初始化项，按索引就位 */

/* 懒发射的运行时（页分配器 / 文件读写）按 C 名字置位：直接写 `std.heap.page`
 * 和先 `const page = std.heap.page;` 再使用，最终落到同一个 C 名字上。 */
static void need_by_cname(const char *cn) {
    if (!cn) return;
    /* zx_page_alloc / zx_page_free：分配器去虚拟化（§138）把
     * `a.alloc(...)` 改成直调这两个运行时函数，声明与定义由 g_need_page
     * 那一段一起发。 */
    if (!strcmp(cn, "zx_alloc_page") ||
        !strcmp(cn, "zx_page_alloc") || !strcmp(cn, "zx_page_free"))
        g_need_page = true;
    if (!strncmp(cn, "zx_file_", 8))  g_need_file = true;
    if (!strncmp(cn, "zx_io_", 6))    g_need_io = true;
    if (!strncmp(cn, "zx_proc_", 8))  g_need_proc = true;
    if (!strncmp(cn, "zx_debug_", 9)) g_need_debug = true;
    if (!strncmp(cn, "zx_atomic_", 10)) g_need_sync = true;
    /* zx_tg_*（group）、zx_sc_*（Scope）、zx_ch_*（Chan）、zx_sel_*（select）
     * 与实例调度器同在 emit_task_runtime 里。只调 group / 只开 scope / 只
     * 建通道 / 只调 select 的程序可能没有任何 zx_task_* 引用，漏掉任一条
     * 就不发射运行时，链接期报 undefined。 */
    if (!strncmp(cn, "zx_task_", 8) || !strncmp(cn, "zx_tg_", 6) ||
        !strncmp(cn, "zx_sc_", 6) || !strncmp(cn, "zx_ch_", 6) ||
        !strncmp(cn, "zx_cap_", 7) || !strncmp(cn, "zx_sel_", 7))
        g_need_task = true;
}

/* 预扫描：main 的签名要在发函数体之前定（用没用 std.process 决定
 * 要不要接 argc/argv），而 need 标志通常在发体时才置位——来不及。
 * 这里在进入代码生成前把整棵树走一遍，认 `std.process` 这个访问形状。 */
static bool scan_proc_use(Node *n) {
    if (!n) return false;
    if (n->kind == NK_FIELD && n->name && !strcmp(n->name, "process") &&
        n->a && n->a->kind == NK_IDENT && n->a->name &&
        !strcmp(n->a->name, "std"))
        return true;
    if (scan_proc_use(n->a) || scan_proc_use(n->b) || scan_proc_use(n->c))
        return true;
    for (int i = 0; i < n->nitems; i++)
        if (scan_proc_use(n->items[i])) return true;
    for (int i = 0; i < n->nunroll; i++)
        if (scan_proc_use(n->unroll[i])) return true;
    return false;
}

/* 同款预扫描：await 门的注入点在 gen_fn（发体时），而 g_need_task 平时
 * 在发体过程中才置位——第一个被生成的函数就赶不上。
 *
 * 主判据是**符号的 C 名字**，不是源码形状：任务运行时的对外入口全部叫
 * zx_task_* / zx_tg_* / zx_sc_* / zx_ch_* / zx_sel_* / zx_cap_*，别名、再导出、局部绑定最终都落到这些
 * extern 名上。旧写法只认 `std.Task` 这个访问形状，现行门面是
 * `std.task.Task`（三段），`const T = std.task.Task` 之类更认不出，
 * 于是只调 group() 的程序不发射运行时（task_group.zx 守门）。形状识别
 * 留作快速路径，不再作为唯一依据。 */
static bool scan_task_use(Node *n) {
    if (!n) return false;
    if (n->kind == NK_FIELD && n->name && !strcmp(n->name, "Task") && n->a) {
        if (n->a->kind == NK_IDENT && n->a->name &&
            !strcmp(n->a->name, "std")) return true;
        if (n->a->kind == NK_FIELD && n->a->name &&
            !strcmp(n->a->name, "task") && n->a->a &&
            n->a->a->kind == NK_IDENT && n->a->a->name &&
            !strcmp(n->a->a->name, "std")) return true;
    }
    if (n->sym) {
        const char *cn = sym_cname(n);
        if (cn && (!strncmp(cn, "zx_task_", 8) || !strncmp(cn, "zx_tg_", 6) ||
                   !strncmp(cn, "zx_sc_", 6) || !strncmp(cn, "zx_ch_", 6) ||
                   !strncmp(cn, "zx_sel_", 7) || !strncmp(cn, "zx_cap_", 7))) return true;
    }
    if (scan_task_use(n->a) || scan_task_use(n->b) || scan_task_use(n->c))
        return true;
    for (int i = 0; i < n->nitems; i++)
        if (scan_task_use(n->items[i])) return true;
    for (int i = 0; i < n->nunroll; i++)
        if (scan_task_use(n->unroll[i])) return true;
    return false;
}

static char *inst_name(Node *g, const char *base) {
    for (int i = 0; i < g_nitab; i++)
        if (g_itab[i].fn == g) return g_itab[i].name;
    if (g_nitab == g_citab) {
        g_citab = g_citab ? g_citab * 2 : 16;
        g_itab = realloc(g_itab, sizeof(InstEnt) * (size_t)g_citab);
    }
    Buf b; buf_init(&b);
    cname(&b, base ? base : "gen");
    buf_printf(&b, "_i%d", g_nitab);
    buf_putc(&b, '\0');
    g_itab[g_nitab].fn   = g;
    g_itab[g_nitab].name = b.data;
    return g_itab[g_nitab++].name;
}

/* ======================================================================
 *  作用域（defer 栈）
 * ====================================================================== */

#define MAX_CDEFER 64

typedef struct CScope {
    struct CScope *parent;
    Node          *defer[MAX_CDEFER];   /* 外层节点（NK_DEFER / NK_ERRDEFER），体在 ->a */
    int            ndefer;
    /* §130 预分配：这一层登记的一块合并存储，离开作用域时整体归还 */
    int            reg_gid;
    uint64_t       reg_total;
    Node          *reg_alloc;
} CScope;

static CScope *g_csc;
static int     g_depth;

static void cscope_push(CScope *s) {
    s->parent = g_csc; s->ndefer = 0;
    s->reg_gid = 0; s->reg_total = 0; s->reg_alloc = NULL;
    g_csc = s; g_depth++;
}
static void cscope_pop(void)       { if (g_csc) { g_csc = g_csc->parent; g_depth--; } }

/* 标签（收割目标 / 循环） */
typedef struct {
    const char *tag;
    const char *slot;      /* 收割写进哪个 C 变量；NULL = 无值 */
    Type       *slot_ty;
    int         end;       /* 出口标签编号 */
    int         cont;      /* 循环的 continue 标签编号，0 = 不是循环 */
    int         depth;     /* 进入时的作用域深度 */
} CLab;

static CLab g_labs[128];
#define MAX_CLABS 128
static int  g_nlabs;
static int  g_labid;

static void gen_stmt(Node *n);
static bool fn_is_generic(Node *fn);
static char *localfn_impl_name(Node *d);
static void gen_value(Node *n, const char *slot, Type *slot_ty);
static void gen_if_value(Node *n, const char *slot, Type *slot_ty);
static void gen_loop_value(Node *n, const char *slot, Type *slot_ty);
static void gen_for_body(Node *n, int id);
static void gen_block_stmts(Node *b);
static void gen_arm(Node *arm, const char *slot, Type *slot_ty);
static void run_defers(int down_to);
static void emit_region_free(int gid, uint64_t total, Node *al);
static char *inst_name(Node *g, const char *base);   /* 泛型实例的 C 名 */

/* 当前函数：生成 return 时要看返回类型 / 是不是 main */
static Type *g_cur_ret;
static bool  g_is_main;
static bool  g_saw_errdefer;   /* 函数体里登记过 errdefer：return 要做 tag 检查 */

/* B13 bug2：局部变量声明提升。含 errdefer 的函数里，defer 体在 try 展开点
 * 就地发射，可能引用函数后部才声明的局部变量（sema 允许，Zig 语义），但 C
 * 要求先声明后使用。对这类函数，函数入口先统一发所有局部变量的裸声明，
 * gen_decl_inner 只保留初始化赋值——C89 风格 hoist，其他函数零影响。 */
static bool g_hoist_locals;

/* main 的 C 签名固定是 `int main(...)`，但 Zx 里它可以是 void、u8，
 * 也可以是 !void / !T / ?T。真值要在这里折成进程退出码：
 *   !T  → 标签就是错误码，0 = 成功（正好是 shell 的约定）
 *   ?T  → 有值退出 0，空退出 1
 * 其余类型原样转 int。expr 为 NULL 表示"没给值"（`return;` / 走到底）。 */
static void main_exit(const char *expr) {
    Type *t = g_cur_ret;
    if (!expr) { line("return 0;"); return; }
    if (t && t->kind == TY_ERR)       line("return (int)((%s).tag);", expr);
    else if (t && t->kind == TY_OPT && ty_opt_flat(t)) line("return (%s) ? 0 : 1;", expr);
    else if (t && t->kind == TY_OPT)  line("return ((%s).tag) ? 0 : 1;", expr);
    else                              line("return (int)(%s);", expr);
}
/* 正在发函数体。`orelse` 要把左边存进一个临时量（只求值一次），
 * 临时量只能落在函数里——顶层初始化器里没有"前一行"可插。 */
static bool  g_in_fn;

/* 正常离开作用域：先把这一层的 defer 逆序展开，再弹栈 */
static void run_defers(int down_to);
static void cscope_leave(void) { run_defers(g_depth - 1); cscope_pop(); }

static CLab *lab_find(const char *tag) {
    for (int i = g_nlabs - 1; i >= 0; i--)
        if (g_labs[i].tag && tag && !strcmp(g_labs[i].tag, tag)) return &g_labs[i];
    return NULL;
}

static CLab *lab_find_loop(const char *tag) {
    for (int i = g_nlabs - 1; i >= 0; i--) {
        if (!g_labs[i].cont) continue;
        if (!tag) return &g_labs[i];
        if (g_labs[i].tag && !strcmp(g_labs[i].tag, tag)) return &g_labs[i];
    }
    return NULL;
}

/* 把 depth-1 .. down_to 这些作用域的 defer 逆序展开。
 * mode 0 = 普通离开（块尾/循环/yield/隐式 return）：只跑 NK_DEFER——
 *        errdefer 只属于"函数以错误返回"这一条路。
 * mode 1 = 错误返回路径：NK_DEFER 照跑；NK_ERRDEFER 在 errcond 非 NULL 时
 *        包一层 `if (errcond) { … }`（返回值 tag 运行时才知道），NULL 表示
 *        错误已注定（try 传播），直接跑。两种条目按登记逆序交织——
 *        defer 和 errdefer 相邻登记时，清理顺序仍然正确。
 * errval = 展开点的错误码表达式（`_dr3.tag` / `65535` / NULL）：`defer err`
 *        的 err 绑定声明在展开点就地发射（每个展开点自己的块作用域，
 *        C 遮蔽保证体内引用拿到的是本点的码）。errval NULL 时绑定按 0
 *        发（不触发绑定声明的路径没有错误码可给）。 */
static void run_defers_m(int down_to, int mode, const char *errcond,
                         const char *errval) {
    for (int d = g_depth - 1; d >= down_to; d--) {
        CScope *s = g_csc;
        for (int k = 0; k < g_depth - 1 - d && s; k++) s = s->parent;
        if (!s) continue;
        for (int k = s->ndefer - 1; k >= 0; k--) {
            Node *dn = s->defer[k];
            bool iserr = dn->kind == NK_ERRDEFER;
            if (iserr && mode == 0) continue;
            if (iserr && errcond) {
                line("if (%s) {", errcond);
                g_ind++;
                if (dn->name && dn->ival) {
                    /* B13：绑定名取 err 符号的唯一 cname，与块内 `err` 引用
                     * 完全对齐；同函数多个 defer err 不再重定义。 */
                    const char *eb = dn->sym ? sym_cname(dn) : cnames(dn->name);
                    line("%s %s = %s;", cty(ty_error()), eb,
                         errval ? errval : "0");
                }
                gen_stmt(dn->a);
                g_ind--;
                line("}");
            } else {
                if (iserr && dn->name && dn->ival && errval) {
                    const char *eb = dn->sym ? sym_cname(dn) : cnames(dn->name);
                    line("%s %s = %s;", cty(ty_error()), eb, errval);
                }
                gen_stmt(dn->a);
            }
        }
        /* §130：合并区间的整体归还排在用户 defer 之后，正常/错误出口都覆盖。
         * 函数级展开（down_to==0，例如 return）已经把这条路径上所有层都
         * 还过了；随后块尾的 cscope_leave 不能再发一遍（goto 型出口会变成
         * 真的二次释放），发完即把该层登记清掉。 */
        if (s->reg_gid > 0) {
            emit_region_free(s->reg_gid, s->reg_total, s->reg_alloc);
            if (down_to == 0) s->reg_gid = 0;
        }
    }
}

static void run_defers(int down_to) { run_defers_m(down_to, 0, NULL, NULL); }

/* ======================================================================
 *  表达式
 * ====================================================================== */

static void coerce(Buf *b, Node *e, Type *to);

static char *exs(Node *n) {
    Buf b; buf_init(&b);
    ex(&b, n);
    return b.data;
}

/* 条件表达式：ex 已经产出 `(a == b)` 这种整体带括号的形式，
   再套一层就是 `if ((a == b))`，clang 会唠叨。这里只保证恰好一层。 */
static char *exs_cond(Node *n) {
    char *s = exs(n);
    if (s && s[0] == '(') {
        int d = 0, i = 0;
        for (; s[i]; i++) {
            if (s[i] == '(') d++;
            else if (s[i] == ')' && --d == 0) break;
        }
        if (d == 0 && s[i] == ')' && s[i + 1] == '\0') return s;
    }
    Buf b; buf_init(&b);
    buf_putc(&b, '(');
    buf_puts(&b, s ? s : "");
    buf_putc(&b, ')');
    return b.data;
}

static char *cxs(Node *e, Type *to) {
    Buf b; buf_init(&b);
    coerce(&b, e, to);
    return b.data;
}

/* §200：裸 `fn (…) … { … }` 当值用（struct 字段初值 / 实参 / 返回值…）时，
 * 它是 NK_FN 节点、没有绑定名字——命名 fn 靠 NK_IDENT + localfn_impl_name
 * 绕开了表达式发射器，裸 fn 字面量却砸进 ex() 的 default（"node 38"）。
 * 和局部 fn 绑定同一套办法：给它发一个 static C 函数，值位置发函数名字。
 * ex(NK_FN) 只登记名字；函数体在本单元正文发完后由 flush_anon_fns 补齐。 */
static char *anonfn_cname(Node *fn);

static bool is_ctl(Node *n) {
    if (!n) return false;
    switch (n->kind) {
    case NK_IF: case NK_WHILE: case NK_FOR: case NK_SWITCH:
        return true;
    case NK_BLOCK:
        return n->label != NULL;
    default:
        return false;
    }
}

/* 字符串字面量 → C 字符串字面量（含引号） */
static void cstr(Buf *b, const char *s, int n) {
    buf_putc(b, '"');
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '\n': buf_puts(b, "\\n"); break;
        case '\t': buf_puts(b, "\\t"); break;
        case '\r': buf_puts(b, "\\r"); break;
        case '"':  buf_puts(b, "\\\""); break;
        case '\\': buf_puts(b, "\\\\"); break;
        default:
            if (c < 32 || c >= 127) buf_printf(b, "\\x%02x\"\"", c);
            else buf_putc(b, (char)c);
        }
    }
    buf_putc(b, '"');
}

/* ---- 运行时越界检查（R10 / §8.3）----
 *
 * 下标、切片在编译期算不出来时，dev 档插一次运行时断言：越界就带源码位置
 * 报错并 abort（和除零、裸解包同一条路子）。release 档里这两个函数退化成
 * 恒等，零成本。
 *
 * 只对**简单基址**（标识符 / 字段链）插下标检查：取 `.len` 需要把基址再发
 * 一遍，如果基址是调用结果，两次求值就是语义变化——那比越界更糟。 */
static bool g_need_bounds;
static bool g_need_f2i;

static bool simple_index_base(Node *n) {
    if (!n) return false;
    switch (n->kind) {
    case NK_IDENT: return true;
    case NK_FIELD: return simple_index_base(n->a);
    case NK_INDEX: return !n->name && simple_index_base(n->a);
    default:       return false;
    }
}

/* 切片上下界只允许"再算一次也不会变"的表达式 */
static bool simple_bound_expr(Node *n) {
    if (!n) return true;
    if (n->kind == NK_INT || n->kind == NK_CHAR) return true;
    if (n->kind == NK_BIN && (n->op == TOK_PLUS || n->op == TOK_MINUS))
        return simple_bound_expr(n->a) && simple_bound_expr(n->b);
    return simple_index_base(n);
}

/* "模块:行"——运行时错误消息里的位置（与 zx: division by zero 同一口径） */
static void bounds_loc(Buf *b, Node *n) {
    const char *p = g_cur_mod_path ? g_cur_mod_path : "?";
    const char *base = p;
    for (const char *q = p; *q; q++)
        if (*q == '/' || *q == '\\') base = q + 1;
    buf_putc(b, '"');
    for (; *base; base++) {
        char c = *base;
        if (c == '"' || c == '\\') c = '_';
        buf_putc(b, c);
    }
    buf_printf(b, ":%d\"", n->line);
}

/* §216：非简单基址的切片下标——把整个切片按值传进类型特化助手，基址只求值
 * 一次，助手内部用 .len 做 zx_bound 检查后返回元素指针。这样 `p[0..2][i]`
 * / 调用结果[ i ] / unwrap 结果[i] 这类链式基址也能拿到越界检查；旧实现
 * 因为要二次发基址取 .len，遇到非简单基址就直接放弃检查，读越界零诊断。 */
static Type **g_idx_types;
static int g_idx_n, g_idx_cap;
static bool g_need_idx;

static void idx_helper_add(Type *sl) {
    ensure_type(sl);
    /* §218：按 **C 名**去重，不按 Type* 指针。[]u8 与 []const u8 是两个
     * 不同的 Type*，但 tn() 都得到 ZxSlice_u8（const 不进名字，C 表示也
     * 一样）；按指针去重会漏，于是同一个 zx_idx_ZxSlice_u8 原型 + 定义
     * 被发两遍，cc 直接报 redefinition。与 ensure_type 里 typedef 的
     * 去重口径（§「两个 Type 映射到同一 C 名」）保持一致。 */
    Buf sn; buf_init(&sn); tn(&sn, sl);
    for (int i = 0; i < g_idx_n; i++) {
        Buf o; buf_init(&o); tn(&o, g_idx_types[i]);
        bool same = !strcmp(o.data, sn.data);
        buf_free(&o);
        if (same) { buf_free(&sn); return; }
    }
    if (g_idx_n == g_idx_cap) {
        g_idx_cap = g_idx_cap ? g_idx_cap * 2 : 8;
        g_idx_types = realloc(g_idx_types, sizeof(Type *) * (size_t)g_idx_cap);
    }
    g_idx_types[g_idx_n++] = sl;
    Buf ct; buf_init(&ct); ctype(&ct, sl->elem);
    bline(&g_bproto, "%s *zx_idx_%s(%s s, long long i, const char *loc);",
          ct.data, sn.data, sn.data);
    bline(&g_bproto, "%s *zx_idxu_%s(%s s, uint64_t i, const char *loc);",
          ct.data, sn.data, sn.data);
    buf_free(&sn); buf_free(&ct);
    g_need_idx = true;
    /* §216: 助手实现调用 zx_bound/zx_bound_s，必须一并带上 bounds runtime，
     * 否则只有链式下标、没有简单基址下标的程序会 B1003（zx_bound 未声明）。 */
    g_need_bounds = true;
}

/* NK_INDEX 的统一发射口：ex() 与 lvalue() 都必须走这里，否则两条路径的
 * 越界检查会再次分叉。 */
static void emit_index_checked(Buf *b, Node *n, Type *bt);

static void emit_idx_node(Buf *b, Node *n) {
    Type *bt = n->a ? n->a->ty : NULL;
    if (bt && bt->kind == TY_SLICE && !simple_index_base(n->a)) {
        idx_helper_add(bt);
        Buf sn; buf_init(&sn); tn(&sn, bt);
        bool sgn = n->b && n->b->ty && n->b->ty->kind == TY_INT && n->b->ty->is_signed;
        buf_printf(b, "(*zx_%s%s(", sgn ? "idx_" : "idxu_", sn.data);
        ex(b, n->a);
        buf_puts(b, ", ");
        ex(b, n->b);
        buf_puts(b, ", ");
        bounds_loc(b, n);
        buf_puts(b, "))");
        return;
    }
    /* §201：数组值 [N]T → C struct { T v[N]; }，下标取 v 字段。 */
    if (bt && bt->kind == TY_ARRAY) {
        buf_puts(b, "(");
        ex(b, n->a);
        buf_puts(b, ").v");
    } else {
        /* 括号包住基址：`.ptr` / `&x.v[0]` / `p+1` 这类表达式直接接
         * `[i]` 会被 C 的优先级拆开（`&(t).v[0][0]` 错，应 `(&(t).v[0])[0]`）。 */
        buf_puts(b, "(");
        ex(b, n->a);
        buf_puts(b, ")");
        if (bt && bt->kind == TY_SLICE) buf_puts(b, ".ptr");
    }
    buf_putc(b, '[');
    emit_index_checked(b, n, bt);
    buf_putc(b, ']');
}

/* `base[i]` 里的 i：能拿到长度就包一层 zx_bound */
static void emit_index_checked(Buf *b, Node *n, Type *bt) {
    bool is_slice = bt && bt->kind == TY_SLICE;
    /* §201：`*[N]T` 的下标选的是第 i 个**数组**，类型里没有数组个数，
     * 不能拿内层 N 去卡界。数组值 [N]T 与字符串字面量的 N 才是真长度。 */
    bool is_arr   = (bt && bt->kind == TY_ARRAY) ||
                    (bt && bt->kind == TY_PTR && bt->has_len &&
                     !ty_is_array_ptr(bt)) ||          /* 字符串字面量 */
                    (bt && bt->kind == TY_VEC);   /* §162：分量数在类型里 */
    if (!is_slice && !is_arr) {
        ex(b, n->b);
        return;
    }
    /* 数组/字符串的长度在类型里，检查不重发基址——链式基址也照样能查。
     * 切片只可能是简单基址走到这里，其余已由 emit_idx_node 的助手接走。 */
    if (is_slice && !simple_index_base(n->a)) {
        ex(b, n->b);
        return;
    }
    g_need_bounds = true;
    Type *it = n->b ? n->b->ty : NULL;
    if (it && it->kind == TY_INT && it->is_signed) {
        /* 有符号下标报原始值：-1 不能打成 18446744073709551615 */
        buf_puts(b, "zx_bound_s((long long)(");
        ex(b, n->b);
        buf_puts(b, "), (size_t)(");
    } else {
    buf_puts(b, "zx_bound((size_t)(");
    ex(b, n->b);
    buf_puts(b, "), (size_t)(");
    }
    if (is_slice) {
        ex(b, n->a);
        buf_puts(b, ").len");
    } else {
        buf_printf(b, "%llu)", (unsigned long long)bt->len);
    }
    buf_puts(b, ", ");
    bounds_loc(b, n);
    buf_putc(b, ')');
}

/* 切片长度表达式（已算好）→ 包一层 zx_slice_len；total 未知就不包 */
static void wrap_slice_len(Buf *b, Buf *len, Node *n, const char *total_c) {
    if (!total_c) { buf_puts(b, len->data); return; }
    g_need_bounds = true;
    buf_puts(b, "zx_slice_len((long long)(");
    buf_puts(b, len->data);
    buf_puts(b, ")");
    buf_printf(b, ", (size_t)(%s), ", total_c);
    bounds_loc(b, n);
    buf_putc(b, ')');
}

/* §209：ct 折叠值是否被"运行期来源"污染——基址链里含 CALL / UNWRAP /
 * ORELSE / TRY。sema 的 ct_call 会真的执行函数体来折值，当函数返回指向
 * 运行期存储的指针时（`getCell()` → &全局 var），`.0` 再解引用会把 var 的
 * **编译期初值镜像**折出来（典型是 0），烙进节点 ct。后端若照单全收，
 * `getCell().0.v = 7` 就发成 `0ULL.v = 7`（B1001）或写到临时拷贝丢值。
 * 这类节点的真实值只能运行时算，ct 一律不信。与 rvalue_base 的区别：
 * 那个判"左值是否合法"，这个判"ct 常量是否可信"。 */
static bool ct_runtime_tainted(Node *n) {
    if (!n) return false;
    switch (n->kind) {
    case NK_CALL:
        /* 只拦「返回指针」的调用：它才会让 `.0` 解引用到运行时存储、
         * 把 var 初值镜像折成假常量。返回 TY_TYPE 的泛型工厂（Slots(...)
         * → struct 类型）取静态成员是合法的编译期值槽，不能拦（§117）。 */
        return n->ty && (n->ty->kind == TY_PTR || n->ty->kind == TY_SLICE);
    case NK_UNWRAP: case NK_ORELSE: case NK_TRY:
        return true;
    case NK_FIELD: case NK_INDEX: {
        /* 指针解引用 `.0`（name 为空、基址是指针）本身就是读运行时存储。 */
        Type *bt = n->a ? n->a->ty : NULL;
        if (n->kind == NK_FIELD && !n->name && bt &&
            (bt->kind == TY_PTR || bt->kind == TY_SLICE)) return true;
        return ct_runtime_tainted(n->a);
    }
    default:
        return false;
    }
}

/* §199：字段基址链里是否藏着"不可取地址的右值"（.? / 调用结果）。
 * `x.?.field` 的 x.? 是 zx_unwrap_...() 返回的临时结构体，C 里对它的字段
 * 取地址是非法的（B1001）。检测到就把整个字段物化到临时数组取 [0]。 */
static bool rvalue_base(Node *n) {
    if (!n) return false;
    switch (n->kind) {
    case NK_UNWRAP: case NK_CALL: case NK_ORELSE: case NK_TRY:
    case NK_STRUCT_LIT: case NK_ANON_STRUCT: case NK_TUPLE:
    case NK_ARRAY_LIT: case NK_INT: case NK_STR: case NK_BOOL:
        return true;
    case NK_FIELD: {
        /* 判定原则是「结果落在哪」，不是「基址表达式是什么节点」：
         * 基址是指针/切片时，字段（含 `.0` 解引用）落在它指向的存储里，
         * 是合法左值——即使基址本身是 call/unwrap 的 rvalue。若在这里
         * 物化，赋值写到临时拷贝静默丢值（§199 NK_INDEX 踩过），甚至把
         * 基址表达式发错。必须先看类型，再看节点种类。 */
        Type *bt = n->a ? n->a->ty : NULL;
        if (bt && (bt->kind == TY_PTR || bt->kind == TY_SLICE)) return false;
        /* 基址是 call/unwrap 且返回「值」（结构体/元组）：字段落在这份
         * 临时载荷上，是 rvalue，需物化成合法左值。 */
        if (n->a && (n->a->kind == NK_UNWRAP || n->a->kind == NK_CALL))
            return true;
        return rvalue_base(n->a);
    }
    case NK_INDEX: {
        /* `ptr()[i]` / `slice[i]` 的基址虽然是 call，但索引结果落在指针
         * 指向的存储里，是合法左值——不能当 rvalue 物化（否则赋值写到
         * 临时拷贝，静默丢值，array-module 的 set() 踩到）。只有基址本身
         * 是数组值（TY_ARRAY / TY_TUPLE 且非指针结果）才可能因基址
         * 是 rvalue 而整体是 rvalue；这里保守返回 false。 */
        Type *bt = n->a ? n->a->ty : NULL;
        if (bt && (bt->kind == TY_PTR || bt->kind == TY_SLICE)) return false;
        return rvalue_base(n->a);
    }
    default:
        return false;
    }
}

/* §199：把右值物化成临时数组的第 0 个元素，得到一个块作用域内的合法左值。
 * `(T[]){ e }[0]` 对函数返回值 / unwrap / 字面量都成立（any_addr 同款手法）。 */
static void rvalue_lvalue(Buf *b, Node *n) {
    Type *t = n->ty ? n->ty : ty_i32();
    ensure_type(t);
    buf_puts(b, "(");
    ctype(b, t);
    buf_puts(b, "[]){ ");
    ex(b, n);
    buf_puts(b, " }[0]");
}

/* ---- 可写位置（左值） ---- */
static void lvalue(Buf *b, Node *n) {
    /* §199：对右值（.? / 返回值的字段 / 字面量）取地址——先物化整个值到
     * 临时数组再取 [0]，作为函数实参（调用期间有效）安全。
     * rvalue_base(n) 已递归判断字段/下标基址：基址是指针时结果落在别处
     * 存储、是合法左值，不会被物化。不要再单独看 n->a，否则返回指针的
     * 调用链 `get().0` 会被误判，写入落到临时拷贝。 */
    if (rvalue_base(n)) {
        rvalue_lvalue(b, n);
        return;
    }
    switch (n->kind) {
    case NK_IDENT:
        /* §151：`&self` / 赋值左侧也要遵守同样的 ABI 透明规则。按指针传的
         * `Self` 接收者在 C 里是 `T *self`，直接发 `&(self)` 会得到 `T **`
         * （B1001）。交给 ex() 补 `(*self)`，`&(*self)` 才是 `*Self`。 */
        if (n->sym && sema_sym_self_by_ptr(n->sym)) { ex(b, n); return; }
        cname(b, sym_cname(n));
        return;
    case NK_FIELD: {
        Type *bt = n->a ? n->a->ty : NULL;
        if (bt && bt->kind == TY_TYPE) {   /* 静态成员 */
            Buf nm; buf_init(&nm); tn(&nm, n->a->type_val);
            buf_printf(b, "%s_", nm.data);
            cname(b, n->name);
            return;
        }
        if (bt && bt->kind == TY_ENUM) {   /* 枚举体的常量成员 */
            Field *f = ty_static_member(bt, n->name);
            if (f) {
                Buf nm; buf_init(&nm); tn(&nm, bt);
                buf_printf(b, "%s_", nm.data);
                cname(b, n->name);
                return;
            }
        }
        if (!n->name) {                    /* .0 解引用 / 元组第 N 个 */
            if (bt && bt->kind == TY_TUPLE) {
                ex(b, n->a);
                buf_printf(b, ".f%llu", (unsigned long long)n->ival);
                return;
            }
            if (bt && bt->kind == TY_SLICE) {
                ex(b, n->a);
                buf_puts(b, ".ptr[0]");
                return;
            }
            if (bt && bt->kind == TY_VEC) {   /* §162：v.2 是分量左值 */
                ex(b, n->a);
                buf_printf(b, "[%llu]", (unsigned long long)n->ival);
                return;
            }
            if (ty_is_array_ptr(bt)) {
                ex(b, n->a);
                buf_printf(b, "[%llu]", (unsigned long long)n->ival);
                return;
            }
            buf_puts(b, "(*");
            ex(b, n->a);
            buf_puts(b, ")");
            return;
        }
        if (bt && bt->kind == TY_PTR) {
            if (bt->elem && (bt->elem->kind == TY_STRUCT || bt->elem->kind == TY_UNION)) {
                Type *e = bt->elem;
                ex(b, n->a);
                if (e->kind == TY_UNION && n->name && strcmp(n->name, "tag")) {
                    if (e->is_tagged) { buf_puts(b, "->u."); mname(b, n->name); }
                    else arrowname(b, n->name);
                } else {
                    arrowname(b, n->name);
                }
                return;
            }
        }
        if (bt && (bt->kind == TY_OPT || bt->kind == TY_ERR)) {
            bool flat = bt->kind == TY_OPT && ty_opt_flat(bt);
            ex(b, n->a);
            if (flat)
                buf_puts(b, n->name && !strcmp(n->name, "tag") ? " != 0" : "");
            else
                buf_puts(b, n->name && !strcmp(n->name, "tag") ? ".tag" : ".payload");
            return;
        }
        if (bt && bt->kind == TY_UNION && n->name && strcmp(n->name, "tag")) {
            ex(b, n->a);
            if (bt->is_tagged) buf_puts(b, ".u");
            dotname(b, n->name);
            return;
        }
        ex(b, n->a);
        dotname(b, n->name);
        return;
    }
    case NK_INDEX:
        emit_idx_node(b, n);
        return;
    default:
        /* 不是左值：退化成表达式，交给 C 编译器报错 */
        ex(b, n);
        return;
    }
}

/* ---- coerce：唯一的隐式转换点 ---- */

static void coerce(Buf *b, Node *e, Type *to) {
    if (!to || !e) { ex(b, e); return; }
    Type *from = e->ty ? e->ty : to;

    /* null / undefined 需要目标类型 */
    if (e->kind == NK_NULL) {
        if (to->kind == TY_OPT) {
            if (ty_opt_flat(to)) { buf_puts(b, "0"); return; }   /* null 指针 */
            ensure_type(to); buf_puts(b, "(("); Buf n; buf_init(&n); ctype(&n, to);
            buf_printf(b, "%s){0})", n.data); return;
        }
        /* 错误值：null 就是保留码 65535，和 `error.null` 同一个值 */
        if (to->kind == TY_ERROR) { buf_puts(b, "65535"); return; }
        /* 裸指针 / 函数指针：null 就是 0（与 C 的 NULL 同一含义） */
        if (to->kind == TY_PTR || to->kind == TY_FN) { buf_puts(b, "0"); return; }
        buf_puts(b, "false");
        return;
    }
    if (e->kind == NK_UNDEF) {
        if (to->kind == TY_OPT && ty_opt_flat(to)) { buf_puts(b, "0"); return; }
        ensure_type(to);
        Buf n; buf_init(&n); ctype(&n, to);
        buf_printf(b, "((%s){0})", n.data);
        return;
    }

    /* comptime 浮点 → 窄浮点：必须真转一次。
     * ty_eq 把 comptime_float 与任何浮点视为相容，直接放行会让 f32 常量以
     * double 身份进 C 表达式，和运行时的 float 运算差 1 ULP。 */
    if (from->kind == TY_COMPTIME_FLOAT && to->kind == TY_FLOAT && to->bits != 64) {
        buf_puts(b, "((");
        ctype(b, to);
        buf_puts(b, ")(");
        ex(b, e);
        buf_puts(b, "))");
        return;
    }

    if (ty_eq(from, to)) { ex(b, e); return; }

    /* §201：数组值 [N]T → 切片 / 裸指针：C 表示是 struct{ T v[N]; }，
     * 首元素地址取 &e.v。 */
    if (from->kind == TY_ARRAY && (to->kind == TY_SLICE || to->kind == TY_PTR)) {
        if (to->kind == TY_SLICE) {
            ensure_type(to);
            Buf n; buf_init(&n); tn(&n, to);
            buf_printf(b, "((%s){ ((", n.data);
            ctype(b, to->elem);
            buf_puts(b, "*)&(");
            ex(b, e);
            buf_printf(b, ").v[0]), %llu })", (unsigned long long)from->len);
        } else {
            buf_puts(b, "&(");
            ex(b, e);
            buf_puts(b, ").v[0]");
        }
        return;
    }
    /* 字符串字面量 → 切片（历史特例，C 基址本身就是首字节指针）。
     * §201：不能只看 e->kind == NK_STR——`let p = "abc";` 之后实参是
     * NK_IDENT，类型仍是 has_len 的历史形状，必须照包 {ptr,len}，否则
     * 实参位置发出裸 uint8_t* 传给 ZxSlice_u8 形参触发 B1001。 */
    if (from->kind == TY_PTR && from->has_len && to->kind == TY_SLICE) {
        ensure_type(to);
        Buf n; buf_init(&n); tn(&n, to);
        buf_printf(b, "((%s){ (", n.data);
        ctype(b, to->elem);
        buf_puts(b, "*)(");
        ex(b, e);
        buf_printf(b, "), %llu })", (unsigned long long)from->len);
        return;
    }
    /* 数组指针 *[N]T → 切片（R7）：首元素地址 = &e->v */
    if (ty_is_array_ptr(from) && to->kind == TY_SLICE) {
        ensure_type(to);
        Buf n; buf_init(&n); tn(&n, to);
        buf_printf(b, "((%s){ ((", n.data);
        ctype(b, to->elem);
        buf_puts(b, "*)&(");
        ex(b, e);
        buf_puts(b, ")->v), ");
        /* §201：普通 *[N]T 的 len 字段已停用（has_len=false），数组长度
         * 写在 elem=[N]T 里。旧代码取 from->len 得到 0，切片全是空串。 */
        buf_printf(b, "%llu })", (unsigned long long)from->elem->len);
        return;
    }
    /* 字符串字面量 → 裸指针：直接强转基址（同上，按类型形状识别，
     * 不能只认 NK_STR——绑定后的标识符也是同一历史形状）。 */
    if (from->kind == TY_PTR && from->has_len && to->kind == TY_PTR &&
        !to->has_len) {
        buf_puts(b, "((");
        ctype(b, to);
        buf_puts(b, ")(");
        ex(b, e);
        buf_puts(b, "))");
        return;
    }
    /* 数组指针 → 裸指针：取 &e->v */
    if (ty_is_array_ptr(from) && to->kind == TY_PTR && !ty_is_array_ptr(to)) {
        buf_puts(b, "((");
        ctype(b, to);
        buf_puts(b, ")&(");
        ex(b, e);
        buf_puts(b, ")->v)");
        return;
    }
    /* 裸指针 → 数组指针：显式断言（C 里都是结构指针，强转即可） */
    if (from->kind == TY_PTR && !ty_is_array_ptr(from) &&
        to->kind == TY_PTR && ty_is_array_ptr(to)) {
        buf_puts(b, "((");
        ctype(b, to);
        buf_puts(b, ")(");
        ex(b, e);
        buf_puts(b, "))");
        return;
    }
    /* 指针 / 切片之间换 const 或换元素类型（显式转换已由 sema 把关） */
    if ((from->kind == TY_PTR && to->kind == TY_PTR) ||
        (from->kind == TY_SLICE && to->kind == TY_SLICE)) {
        buf_puts(b, "(");
        ctype(b, to);
        buf_puts(b, ")(");
        ex(b, e);
        buf_puts(b, ")");
        return;
    }
    /* T → ?T */
    if (to->kind == TY_OPT) {
        if (from->kind == TY_OPT) { ex(b, e); return; }
        if (ty_opt_flat(to)) {       /* ?*T：值即指针，null 即 0 */
            coerce(b, e, to->elem);
            return;
        }
        ensure_type(to);
        Buf n; buf_init(&n); tn(&n, to);
        buf_printf(b, "((%s){ ", n.data);
        /* 载荷类型 */
        coerce(b, e, to->elem);
        buf_puts(b, ", true })");
        return;
    }
    /* T → !T  /  error → !T */
    if (to->kind == TY_ERR) {
        if (from->kind == TY_ERR) { ex(b, e); return; }
        ensure_type(to);
        Buf n; buf_init(&n); tn(&n, to);
        bool void_payload = to->elem && to->elem->kind == TY_VOID;
        buf_printf(b, "((%s){ ", n.data);
        /* 一律用指定初始化：`!void` 没有 payload 字段，位置初始化会多给一个
         * 初始值（`{ 0, 3 }` 对只有一个字段的结构体 = excess elements）。 */
        if (from->kind == TY_ERROR) {
            buf_puts(b, ".tag = ");
            coerce(b, e, ty_error());
        } else if (void_payload) {
            buf_puts(b, ".tag = 0");
        } else {
            buf_puts(b, ".payload = ");
            coerce(b, e, to->elem);
            buf_puts(b, ", .tag = 0");
        }
        buf_puts(b, " })");
        return;
    }
    /* 指针 ↔ 整数：显式转换已由 sema 把关，这里走 uintptr_t 中转，
     * 免得 64 位机上 C 报 "pointer truncation" */
    if (from->kind == TY_PTR && ty_is_int(to) && to->kind != TY_ERROR) {
        buf_puts(b, "((");
        ctype(b, to);
        buf_puts(b, ")(uintptr_t)(");
        ex(b, e);
        buf_puts(b, "))");
        return;
    }
    if (ty_is_int(from) && from->kind != TY_ERROR && to->kind == TY_PTR) {
        buf_puts(b, "((");
        ctype(b, to);
        buf_puts(b, ")(uintptr_t)(");
        ex(b, e);
        buf_puts(b, "))");
        return;
    }
    /* 数值 / 枚举 / 布尔：C 的强制转换就够 */
    if ((ty_is_num(from) || from->kind == TY_BOOL || from->kind == TY_ENUM ||
         from->kind == TY_COMPTIME_INT || from->kind == TY_COMPTIME_FLOAT ||
         from->kind == TY_ERROR) &&
        (ty_is_num(to) || to->kind == TY_ENUM || to->kind == TY_BOOL ||
         to->kind == TY_ERROR)) {
        buf_puts(b, "((");
        ctype(b, to);
        buf_puts(b, ")(");
        ex(b, e);
        buf_puts(b, "))");
        return;
    }
    /* 兜底：直接把 C 值搬过去，类型不同交给 C 报错 */
    ex(b, e);
}

/* ---- 运算符 ---- */

static const char *cop(int k) {
    switch (k) {
    case TOK_PLUS: return "+";
    case TOK_MINUS: return "-";
    case TOK_STAR: return "*";
    case TOK_SLASH: return "/";
    case TOK_PERCENT: return "%";
    case TOK_AMP: return "&";
    case TOK_PIPE: return "|";
    case TOK_CARET: return "^";
    case TOK_SHL: return "<<";
    case TOK_SHR: return ">>";
    case TOK_EQ: return "==";
    case TOK_NE: return "!=";
    case TOK_LT: return "<";
    case TOK_LE: return "<=";
    case TOK_GT: return ">";
    case TOK_GE: return ">=";
    case TOK_ANDAND: return "&&";
    case TOK_OROR: return "||";
    default: return NULL;
    }
}

static bool is_assign_op(int k) {
    switch (k) {
    case TOK_ASSIGN: case TOK_PLUSEQ: case TOK_MINUSEQ: case TOK_STAREQ:
    case TOK_SLASHEQ: case TOK_PERCEQ: case TOK_AMPEQ: case TOK_PIPEEQ:
    case TOK_CARETEQ: case TOK_SHLEQ: case TOK_SHREQ:
        return true;
    default: return false;
    }
}

/* ---- 内建 ---- */

static void ex_builtin(Buf *b, Node *n) {
    const char *nm = n->name ? n->name : "";

    /* 反射统一走 @type(x)：它的值全是编译期值（.size/.align/.name/…），
     * 任何取字段的写法在 sema 已经折成字面量，走到这里的只可能是把整块
     * 描述符当值用的残留——那本来就该在 sema 被 ct_only 拦下。 */
    if (!strcmp(nm, "type")) {
        buf_puts(b, "0");
        return;
    }
    if (!strcmp(nm, "panic")) {
        /* §213：带 Zx 位置的运行时中止。msg 是编译期字面量，直接嵌 C 串；
         * 走 zx_panic 而不是裸 abort：无缓冲打点 + 单条诊断，crash handler
         * 不会再补一条 fatal。无消息时也带位置，绝不静默 abort。 */
        Buf m; buf_init(&m);
        if (n->nitems == 1) cstr(&m, n->items[0]->str, n->items[0]->str_len);
        else buf_puts(&m, "\"\"");
        const char *mp = g_cur_mod_path ? g_cur_mod_path : "?";
        Buf f; buf_init(&f); cstr(&f, mp, (int)strlen(mp));
        buf_printf(b, "zx_panic(%s, %s, %d, %d)", m.data, f.data, n->line, n->col);
        buf_free(&m); buf_free(&f);
        return;
    }
    if (!strcmp(nm, "trap")) {
        buf_puts(b, "abort()");   /* 裸 trap：留给 crash handler 打点 */
        return;
    }
    if (!strcmp(nm, "link")) {
        /* @link 在语义阶段已经把库名记给了 driver；运行时是 no-op */
        buf_puts(b, "((void)0)");
        return;
    }
    if (!strcmp(nm, "memcpy") || !strcmp(nm, "memmove") || !strcmp(nm, "memset")) {
        /* 直接落 C 的批量内存原语：std.array.List 增长 / 搬移不再逐元素。
         * 切片实参取 .ptr（C 侧是 ZxSlice_* 结构体，不能直接转 void*）。 */
        Buf d; buf_init(&d); ex(&d, n->items[0]);
        Buf s; buf_init(&s); ex(&s, n->items[1]);
        Buf c; buf_init(&c); ex(&c, n->items[2]);
        Type *dt = n->items[0]->ty;
        if (dt && dt->kind == TY_SLICE) { buf_printf(&d, ".ptr"); }
        if (!strcmp(nm, "memset")) {
            buf_printf(b, "memset((void*)(%s), (int)(%s), (size_t)(%s))",
                       d.data, s.data, c.data);
        } else {
            Type *st = n->items[1]->ty;
            if (st && st->kind == TY_SLICE) { buf_printf(&s, ".ptr"); }
            buf_printf(b, "%s((void*)(%s), (const void*)(%s), (size_t)(%s))",
                       nm, d.data, s.data, c.data);
        }
        buf_free(&d); buf_free(&s); buf_free(&c);
        return;
    }
    if (!strcmp(nm, "error")) {
        buf_puts(b, "abort()");
        return;
    }
    buf_puts(b, "0");
}

/* ---- 打印 ---- */

/* 格式动词：0 默认、'd' 十进制、'x' 十六进制、'b' 二进制、's' 字符串、'c' 字符 */

/* comptime_int 的字面量是否超出 64 位——决定打印要不要走 128 位路径。
 * 小值仍走 %lld，保住 {:x} / {:b} 这类动词的原有行为。 */
static bool ct_int_big(Node *a) {
    if (!a) return false;
    if (a->kind == NK_INT && a->flag) return true;
    if (a->ct && a->ct->kind == V_INT && a->ct->big) return true;
    if (a->kind == NK_UN && a->op == TOK_MINUS) return ct_int_big(a->a);
    return false;
}

/* ---- 宽度/填充/对齐（M26）----
 *
 * 语法：{[idx][verb]:[fill][align][width][.prec]}
 *   {:>8}   右对齐宽 8（数字默认右）
 *   {:0>2}  零填充（只对数字有意义）
 *   {:*^7}  自定义填充 + 居中
 *   {:<6}   左对齐（字符串默认左）
 *   {:.3}   浮点精度（定点，%f 语义）
 *
 * 空格填充直接映射 printf 旗标（零运行时成本）；零填充+右对齐用
 * printf 的 '0' 旗标；其余（自定义填充、居中）走 pad 助手。 */
typedef struct {
    char fill;    /* 默认 ' ' */
    char align;   /* '<' '^' '>'；0 = 未指定（数字右、字符串左） */
    int  width;   /* 0 = 不限 */
    int  prec;    /* -1 = 不限，只对浮点生效 */
} FmtOpt;

/* 解析 `:` 后面的选项段。语法不对报 E 并返回 false。 */
static bool fmt_parse_opts(const char *s, int n, FmtOpt *o, Node *fmt) {
    o->fill = ' '; o->align = 0; o->width = 0; o->prec = -1;
    int end = n;
    /* 精度在尾巴上：.N（必须在剥宽度之前看，否则 .2 的 2 会被当宽度） */
    if (end > 0 && s[end - 1] >= '0' && s[end - 1] <= '9') {
        int k = end;
        while (k > 0 && s[k - 1] >= '0' && s[k - 1] <= '9') k--;
        if (k > 0 && s[k - 1] == '.') { o->prec = atoi(s + k); end = k - 1; }
    }
    /* 宽度：紧邻的数字段 */
    int wk = end;
    while (wk > 0 && s[wk - 1] >= '0' && s[wk - 1] <= '9') wk--;
    for (int k = wk; k < end; k++) o->width = o->width * 10 + (s[k] - '0');
    end = wk;
    if (o->width > 256) {
        err_at(fmt->line, fmt->col, "format width %d is out of range (max 256)", o->width);
        return false;
    }
    if (end > 0 && (s[end - 1] == '<' || s[end - 1] == '^' || s[end - 1] == '>')) {
        o->align = s[end - 1];
        end--;
        if (end > 0) { o->fill = s[end - 1]; end--; }
    }
    if (end != 0) {
        err_at(fmt->line, fmt->col, "unknown format option `{:%.*s}`", n, s);
        return false;
    }
    return true;
}

/* 非空格填充/居中的发射都内联在 fmt_spec 各分支里（数值/字符串形态
 * 的实参构成不同，抽公共函数反而要传表达式）。 */

static void fmt_spec(Buf *spec, Buf *args, Node *a, int verb, bool *need_bin, const FmtOpt *o) {
    Type *t = a->ty ? a->ty : ty_i32();
    bool pad = o && o->width > 0 &&
               (o->fill != ' ' || o->align == '^');   /* printf 旗标覆盖不了的情况 */
    bool zerofill = o && o->fill == '0' && (o->align == 0 || o->align == '>');

    if (verb == 's' || t->kind == TY_SLICE) {
        /* 字符串字面量是 *[N]const u8：长度在类型里，指针就是数据起点，
         * 不能去读切片的 .len/.ptr。 */
        Type *se = (t && t->elem && t->elem->kind == TY_ARRAY) ? t->elem->elem : t ? t->elem : NULL;
        bool str_arr = t && t->kind == TY_PTR && t->has_len && se &&
                       se->kind == TY_INT && se->bits == 8;
        if (str_arr) {
            if (pad) {
                g_need_pad = true;
                buf_puts(spec, "%s");
                buf_puts(args, ", zx_fmt_pads((char[512]){0}, 512, (const char*)(");
                ex(args, a);
                buf_printf(args, "), %llu, '%c', %d, '%c')",
                           (unsigned long long)t->len,
                           o->fill, o->width, o->align ? o->align : '<');
                return;
            }
            if (o && o->width > 0) {
                char al = o->align ? o->align : '<';
                buf_printf(spec, al == '<' ? "%%-%d.*s" : "%%%d.*s", o->width);
            } else {
                buf_puts(spec, "%.*s");
            }
            buf_printf(args, ", (int)%llu, (char*)(",
                       (unsigned long long)t->len);
            ex(args, a);
            buf_puts(args, ")");
            return;
        }
        if (pad) {
            /* 字符串：ptr/len 直通助手，不经过 printf，长串不截断不了——
             * 上限 512 内安全；超长串请别配填充。 */
            g_need_pad = true;
            buf_puts(spec, "%s");
            buf_puts(args, ", zx_fmt_pads((char[512]){0}, 512, (const char*)(");
            ex(args, a);
            buf_puts(args, ").ptr, (int)(");
            ex(args, a);
            buf_printf(args, ").len, '%c', %d, '%c')",
                       o->fill, o->width, o->align ? o->align : '<');
            return;
        }
        if (o && o->width > 0) {
            char al = o->align ? o->align : '<';       /* 字符串默认左 */
            buf_printf(spec, al == '<' ? "%%-%d.*s" : "%%%d.*s", o->width);
        } else {
            buf_puts(spec, "%.*s");
        }
        buf_puts(args, ", (int)(");
        ex(args, a);
        buf_puts(args, ").len, (char*)(");
        ex(args, a);
        buf_puts(args, ").ptr");
        return;
    }
    if (verb == 'c') {
        if (pad) {
            g_need_pad = true;
            buf_puts(spec, "%s");
            buf_printf(args, ", zx_fmt_pad((char[512]){0}, 512, '%c', %d, '%c', \"%%c\", (int)(",
                       o->fill, o->width, o->align ? o->align : '>');
            ex(args, a);
            buf_puts(args, "))");
            return;
        }
        if (o && o->width > 0) {
            char al = o->align ? o->align : '>';
            buf_printf(spec, al == '<' ? "%%-%dc" : "%%%dc", o->width);
        } else {
            buf_puts(spec, "%c");
        }
        buf_puts(args, ", (int)(");
        ex(args, a);
        buf_puts(args, ")");
        return;
    }
    if (t->kind == TY_BOOL && verb != 'd' && verb != 'x' && verb != 'b') {
        if (pad) {
            g_need_pad = true;
            buf_puts(spec, "%s");
            buf_printf(args, ", zx_fmt_pad((char[512]){0}, 512, '%c', %d, '%c', \"%%s\", (",
                       o->fill, o->width, o->align ? o->align : '>');
            ex(args, a);
            buf_puts(args, ") ? \"true\" : \"false\")");
            return;
        }
        if (o && o->width > 0) {
            char al = o->align ? o->align : '>';
            buf_printf(spec, al == '<' ? "%%-%ds" : "%%%ds", o->width);
        } else {
            buf_puts(spec, "%s");
        }
        buf_puts(args, ", (");
        ex(args, a);
        buf_puts(args, ") ? \"true\" : \"false\"");
        return;
    }
    if (ty_is_float(t)) {
        /* 没点名精度时按"最短往返"打：`%g` 只有 6 位有效数字，
         * 0.1+0.2 会打成 0.3——打出来的数读回去和原值不是一个值。
         * 显式写了 `{:.2}` 就照用户说的来。 */
        if (pad) {
            char cf[16];
            if (o->prec >= 0) snprintf(cf, sizeof cf, "%%.%df", o->prec);
            else snprintf(cf, sizeof cf, "%%s");
            g_need_pad = true;
            buf_puts(spec, "%s");
            if (o->prec >= 0) {
                buf_printf(args, ", zx_fmt_pad((char[512]){0}, 512, '%c', %d, '%c', \"%s\", (double)(",
                           o->fill, o->width, o->align ? o->align : '>', cf);
                ex(args, a);
                buf_puts(args, "))");
            } else {
                g_need_f64str = true;
                buf_printf(args, ", zx_fmt_pad((char[512]){0}, 512, '%c', %d, '%c', \"%s\", zx_f64_str((char[64]){0}, 64, (double)(",
                           o->fill, o->width, o->align ? o->align : '>', cf);
                ex(args, a);
                buf_puts(args, ")))");
            }
            return;
        }
        if (o && o->prec >= 0) {
            char al = o->align ? o->align : '>';
            const char *neg = al == '<' ? "-" : (zerofill ? "0" : "");
            buf_printf(spec, "%%%s%d.%df", neg, o->width, o->prec);
            buf_puts(args, ", (double)(");
            ex(args, a);
            buf_puts(args, ")");
            return;
        }
        g_need_f64str = true;
        if (o && o->width > 0) {
            char al = o->align ? o->align : '>';
            const char *neg = al == '<' ? "-" : (zerofill ? "0" : "");
            buf_printf(spec, "%%%s%ds", neg, o->width);
        } else {
            buf_puts(spec, "%s");
        }
        buf_puts(args, ", zx_f64_str((char[64]){0}, 64, (double)(");
        ex(args, a);
        buf_puts(args, "))");
        return;
    }
    if (verb == 'b') {
        /* printf 没有二进制：记号交给生成的辅助函数（宽度/填充先不管） */
        *need_bin = true;
        buf_puts(args, ", (uint64_t)(");
        ex(args, a);
        buf_puts(args, ")");
        return;
    }
    if (ty_is_int(t) || t->kind == TY_ENUM || t->kind == TY_ERROR || t->kind == TY_BOOL) {
        bool sgn = (t->kind == TY_INT && t->is_signed) || t->kind == TY_ENUM ||
                   t->kind == TY_COMPTIME_INT;
        if (t->kind == TY_ENUM && t->elem) sgn = t->elem->is_signed;
        if ((t->kind == TY_INT && t->bits == 128) ||
            (t->kind == TY_COMPTIME_INT && verb != 'x' && verb != 'b' && ct_int_big(a))) {
            /* printf 没有 128 位动词：转成十进制字符串再 %s（M19）。
             * 有宽度时垫一层 pad（值 ≤ 40 字符，缓冲内安全）。 */
            g_need_i128 = true;
            if (pad) {
                g_need_pad = true;
                buf_puts(spec, "%s");
                buf_printf(args, ", zx_fmt_pad((char[512]){0}, 512, '%c', %d, '%c', \"%%s\", ",
                           o->fill, o->width, o->align ? o->align : '>');
                buf_puts(args, sgn ? "zx_i128_str((char[48]){0}, (__int128)("
                                   : "zx_u128_str((char[48]){0}, (unsigned __int128)(");
                ex(args, a);
                buf_puts(args, "))");
                return;
            }
            buf_puts(spec, "%s");
            buf_puts(args, sgn ? ", zx_i128_str((char[48]){0}, (__int128)("
                               : ", zx_u128_str((char[48]){0}, (unsigned __int128)(");
            ex(args, a);
            buf_puts(args, "))");
            return;
        }
        const char *cf = verb == 'x' ? "%llx" : (sgn ? "%lld" : "%llu");
        if (pad) {
            g_need_pad = true;
            buf_puts(spec, "%s");
            buf_printf(args, ", zx_fmt_pad((char[512]){0}, 512, '%c', %d, '%c', \"%s\", ",
                       o->fill, o->width, o->align ? o->align : '>', cf);
            buf_puts(args, sgn ? "(long long)(" : "(unsigned long long)(");
            ex(args, a);
            buf_puts(args, "))");
            return;
        }
        if (o && o->width > 0) {
            const char *fl = zerofill ? "0" : (o->align == '<' ? "-" : "");
            if (verb == 'x')
                buf_printf(spec, "%%%s%dllx", fl, o->width);
            else
                buf_printf(spec, sgn ? "%%%s%dlld" : "%%%s%dllu", fl, o->width);
        } else {
            if (verb == 'x') buf_puts(spec, "%llx");
            else buf_puts(spec, sgn ? "%lld" : "%llu");
        }
        buf_puts(args, sgn ? ", (long long)(" : ", (unsigned long long)(");
        ex(args, a);
        buf_puts(args, ")");
        return;
    }
    /* 其余类型给个地址，至少不会崩 */
    buf_puts(spec, "%p");
    buf_puts(args, ", (void*)&(");
    ex(args, a);
    buf_puts(args, ")");
}

/* 用逗号把若干段 printf / 辅助调用串成一个表达式 */
typedef struct {
    Buf out;         /* 已完成的段 */
    Buf spec;        /* 当前段的格式串 */
    Buf args;        /* 当前段的实参 */
    int nseg;        /* 已经收掉的段数 */
} PrintSeg;

static void seg_flush(PrintSeg *g) {
    if (g->nseg && g->spec.len == 0 && g->args.len == 0) return;   /* 空段 */
    if (g->nseg) buf_puts(&g->out, ", ");
    buf_printf(&g->out, "printf(\"%.*s\"%s)",
               (int)g->spec.len, g->spec.data ? g->spec.data : "",
               g->args.data ? g->args.data : "");
    g->nseg++;
    g->spec.len = 0;
    g->args.len = 0;
    if (g->spec.data) g->spec.data[0] = 0;
    if (g->args.data) g->args.data[0] = 0;
}

/* 把一段字面文本塞进格式串（% 要转义，其余交给 cstr 做 C 转义） */
static void seg_lit(Buf *spec, const char *p, int n) {
    for (int i = 0; i < n; i++) {
        if (p[i] == '%') { buf_putc(spec, '%'); buf_putc(spec, '%'); continue; }
        Buf one; buf_init(&one);
        cstr(&one, p + i, 1);
        buf_putn(spec, one.data + 1, strlen(one.data) - 2);   /* 去掉两端引号 */
    }
}

/* ---- {any}：结构展开 ----
 *
 * 一个值怎么打印，取决于它的"形状"，不取决于它叫什么。所以给每个用到
 * 的类型生成一份**描述符**：种类、字段名、字段偏移、元素描述符下标。
 * 运行时的 zx_any 拿到值指针 + 描述符就能递归打印，编译期路径和运行时
 * 路径共用这一个实现，输出不可能不一致。
 *
 * 偏移直接写 C 的 offsetof，编译器不必自己再复算一遍布局（也不会和
 * ty_size 的填充规则打架）。描述符之间用**下标**互相引用，整个 zx_at[]
 * 一次性定义，所以递归类型（Node 里放 ?*Node）不需要前向声明。
 */
enum { AK_VOID = 0, AK_INT, AK_FLOAT, AK_BOOL, AK_ENUM, AK_ERROR,
       AK_PTR, AK_SLICE, AK_STRUCT, AK_UNION, AK_TUNION, AK_OPT, AK_ERR, AK_FN,
       AK_STRPTR };

static const char *ak_name(int k) {
    static const char *n[] = { "ZX_AK_VOID", "ZX_AK_INT", "ZX_AK_FLOAT", "ZX_AK_BOOL",
                               "ZX_AK_ENUM", "ZX_AK_ERROR", "ZX_AK_PTR", "ZX_AK_SLICE",
                               "ZX_AK_STRUCT", "ZX_AK_UNION", "ZX_AK_TUNION", "ZX_AK_OPT",
                               "ZX_AK_ERR", "ZX_AK_FN", "ZX_AK_STRPTR" };
    return (k >= 0 && k <= AK_STRPTR) ? n[k] : "ZX_AK_VOID";
}

static Type **g_adt;         /* 描述符 ↔ 类型 */
static int    g_nad, g_cad;

/* 这个类型有描述符可用（能结构展开） */
static bool any_has_desc(Type *t) {
    if (!t) return false;
    switch (t->kind) {
    case TY_STRUCT: case TY_UNION: case TY_TUPLE: case TY_OPT: case TY_ERR:
    case TY_ENUM: case TY_PTR: case TY_SLICE: case TY_VEC:
        return true;
    default: return false;
    }
}

/* 字符串字面量 / *[N]u8：定长字节数组指针（用户 2026-11-09：字符串 =
 * zx 的数组 *[N]const u8）。打印/格式化要按字节序列处理，不能当指针。 */
static bool str_ptr_ty(Type *t) {
    if (!t || t->kind != TY_PTR || !t->has_len || !t->elem) return false;
    Type *e = (t->elem->kind == TY_ARRAY) ? t->elem->elem : t->elem;
    return e && e->kind == TY_INT && e->bits == 8 && !e->is_signed;
}

/* []u8 / []const u8：默认 {} 当字符串打（规格 §11.1） */
static bool any_is_str(Type *t) {
    return t && t->kind == TY_SLICE && t->elem && t->elem->kind == TY_INT &&
           t->elem->bits == 8 && !t->elem->is_signed;
}

/* 默认 {} 就要展开的类型：这些值给个地址毫无意义 */
static bool default_any(Type *t) {
    if (!t) return false;
    switch (t->kind) {
    case TY_STRUCT: case TY_UNION: case TY_TUPLE: case TY_OPT: case TY_ERR:
    case TY_VEC:
        return true;
    case TY_PTR: return !str_ptr_ty(t);   /* 字符串按文本打，不展开地址 */
    case TY_SLICE: return !any_is_str(t);
    default: return false;
    }
}

static void qname(Buf *b, const char *s) {
    if (!s) { buf_puts(b, "0"); return; }
    cstr(b, s, (int)strlen(s));
}

static int any_desc(Type *t) {
    if (!t) return 0;
    for (int i = 0; i < g_nad; i++) if (g_adt[i] == t) return i;
    if (g_nad == g_cad) {
        g_cad = g_cad ? g_cad * 2 : 32;
        g_adt = realloc(g_adt, sizeof(Type *) * (size_t)g_cad);
        g_anydef = realloc(g_anydef, sizeof(char *) * (size_t)g_cad);
    }
    int idx = g_nad++;
    g_adt[idx] = t;
    g_need_any = true;

    ensure_type(t);
    Buf cn; buf_init(&cn);
    ctype(&cn, t);                  /* offsetof 里的那个类型名 */

    Buf fld;  buf_init(&fld);
    Buf tags; buf_init(&tags);
    Buf tns;  buf_init(&tns);
    int kind = AK_VOID, size = ty_size(t), sign = 0, nf = 0, nt = 0, elem = 0;
    int auxsize = 0, is_str = 0;

    switch (t->kind) {
    case TY_INT: case TY_COMPTIME_INT:
        kind = AK_INT;
        sign = t->is_signed;
        if (size < 0) size = 4;
        break;
    case TY_FLOAT: case TY_COMPTIME_FLOAT:
        kind = AK_FLOAT;
        if (size < 0) size = 8;
        break;
    case TY_BOOL:  kind = AK_BOOL; size = 1; break;
    case TY_ERROR: kind = AK_ERROR; size = 2; break;
    case TY_FN:    kind = AK_FN; size = 8; break;
    case TY_PTR:
        if (str_ptr_ty(t)) {
            /* 嵌在聚合里的字符串：按文本展开（长度在类型里，走 aux_size） */
            kind = AK_STRPTR;
            size = 8;
            auxsize = t->len > 0 ? (int)t->len : 1;
            break;
        }
        kind = AK_PTR;
        size = 8;                   /* 不跟指针走：打印值只印地址，避免环 */
        break;
    case TY_SLICE:
        kind = AK_SLICE;
        elem = any_desc(t->elem);
        is_str = any_is_str(t) ? 1 : 0;
        size = 16;
        break;
    case TY_OPT:
        if (ty_opt_flat(t)) {           /* ?*T：按指针展开（M26） */
            kind = AK_PTR;
            size = 8;
            break;
        }
        kind = AK_OPT;
        elem = any_desc(t->elem);
        break;
    case TY_ERR:
        kind = AK_ERR;
        elem = any_desc(t->elem);
        break;
    case TY_ENUM: {
        kind = AK_ENUM;
        Type *bt = t->elem ? t->elem : ty_i32();
        sign = bt->is_signed;
        size = ty_size(bt);
        for (int i = 0; i < t->nfields; i++) {
            Field *f = &t->fields[i];
            buf_printf(&tags, "%lld, ", (long long)(f->has_tag ? f->tag : (uint64_t)i));
            qname(&tns, f->name); buf_puts(&tns, ", ");
            nt++;
        }
        break;
    }
    case TY_TUPLE:
        kind = AK_STRUCT;
        for (int i = 0; i < t->nfields; i++) {
            int child = any_desc(t->fields[i].ty);
            buf_printf(&fld, "{ 0, offsetof(%s, f%d), %d },\n", cn.data, i, child);
            nf++;
        }
        break;
    case TY_VEC: {
        /* §162：向量按 N 个同类型分量展开——`v.0 … v.N-1` 本来就是编译期
         * 成员，打印成 `{1.5, 1.5, 1.5, 1.5}` 才和索引看到的是一回事。
         * C 的 vector 类型不支持 offsetof，偏移自己按元素大小算。 */
        kind = AK_STRUCT;
        int es = t->elem ? ty_size(t->elem) : 0;
        if (es <= 0) es = 1;
        int child = any_desc(t->elem);
        for (uint64_t i = 0; i < t->len; i++) {
            buf_printf(&fld, "{ 0, %llu, %d },\n",
                       (unsigned long long)(i * (uint64_t)es), child);
            nf++;
        }
        break;
    }
    case TY_STRUCT:
        kind = AK_STRUCT;
        for (int i = 0; i < t->nfields; i++) {
            Field *f = &t->fields[i];
            if (f->is_static) continue;
            int child = any_desc(f->ty);
            buf_puts(&fld, "{ ");
            qname(&fld, f->name);
            buf_printf(&fld, ", offsetof(%s, %s), %d },\n", cn.data,
                       f->name ? f->name : "?", child);
            nf++;
        }
        break;
    case TY_UNION:
        if (t->is_tagged && t->tag_ty) {
            kind = AK_TUNION;
            Type *tt = t->tag_ty;
            auxsize = ty_size(tt);
            for (int i = 0; i < t->nfields; i++) {
                Field *f = &t->fields[i];
                if (f->is_static) continue;
                int child = any_desc(f->ty);
                buf_puts(&fld, "{ ");
                qname(&fld, f->name);
                buf_printf(&fld, ", offsetof(%s, u", cn.data);
                dotname(&fld, f->name ? f->name : "?");
                buf_printf(&fld, "), %d },\n", child);
                buf_printf(&tags, "%lld, ", (long long)f->tag);
                qname(&tns, f->name); buf_puts(&tns, ", ");
                nf++; nt++;
            }
        } else {
            kind = AK_UNION;
            for (int i = 0; i < t->nfields; i++) {
                Field *f = &t->fields[i];
                if (f->is_static) continue;
                int child = any_desc(f->ty);
                buf_puts(&fld, "{ ");
                qname(&fld, f->name);
                buf_printf(&fld, ", offsetof(%s, %s), %d },\n", cn.data,
                           f->name ? f->name : "?", child);
                nf++;
            }
        }
        break;
    default: break;
    }

    if (nf) buf_printf(&g_bany, "static const ZxAnyField zx_af_%d[] = {\n%s};\n", idx, fld.data);
    if (kind == AK_ENUM || kind == AK_TUNION) {
        buf_printf(&g_bany, "static const long long zx_ag_%d[] = { %s};\n", idx, tags.data);
        buf_printf(&g_bany, "static const char *const zx_an_%d[] = { %s};\n", idx, tns.data);
    }

    Buf ent; buf_init(&ent);
    buf_printf(&ent, "{ .kind = %s, .size = %d, .sign = %d, .n = %d",
               ak_name(kind), size, sign,
               (kind == AK_ENUM || kind == AK_TUNION) ? nt : nf);
    if (t->name && (kind == AK_STRUCT || kind == AK_UNION ||
                    kind == AK_TUNION || kind == AK_ENUM)) {
        buf_puts(&ent, ", .name = ");
        qname(&ent, t->name);
    }
    if (nf) buf_printf(&ent, ", .fields = zx_af_%d", idx);
    if (kind == AK_SLICE || kind == AK_OPT || kind == AK_ERR)
        buf_printf(&ent, ", .elem = %d", elem);
    if (kind == AK_OPT || kind == AK_ERR || kind == AK_TUNION) {
        buf_printf(&ent, ", .aux_off = offsetof(%s, tag)", cn.data);
        buf_printf(&ent, ", .aux_size = %d", kind == AK_ERR ? 2 : auxsize > 0 ? auxsize : 1);
    }
    if (kind == AK_ENUM || kind == AK_TUNION)
        buf_printf(&ent, ", .tags = zx_ag_%d, .names = zx_an_%d", idx, idx);
    if (kind == AK_STRPTR)
        buf_printf(&ent, ", .aux_size = %d", auxsize);
    if (kind == AK_SLICE) buf_printf(&ent, ", .is_str = %d", is_str);
    buf_puts(&ent, " }");
    g_anydef[idx] = ent.data;
    return idx;
}

/* ---- 运行时格式化（fmt 不是编译期常量时走这条路） ----
 *
 * 编译期路径把格式串拆成一串 printf，零解析零分配；这里保留同一套语法，
 * 只是把解析推迟到运行时：实参先统一成 ZxFmtArg，再由 zx_fmt 边解析边写。
 * 语义与编译期那条完全一致（spec §11.1 第 3 条）。 */

static void any_addr(Buf *b, Node *e);

static void fmt_arg_one(Buf *b, Node *a) {
    Type *t = a->ty ? a->ty : ty_i32();

    /* 字符串字面量 / *[N]u8：按字节序列打，不能走指针的描述符
     *（那样 `{}` 会打印地址）。长度在类型里，直接包成切片。 */
    if (str_ptr_ty(t)) {
        ensure_type(ty_slice(ty_u8(), true));
        buf_puts(b, "zx_fa_s(((ZxSlice_u8){ (uint8_t*)(");
        ex(b, a);
        buf_printf(b, "), %llu }))", (unsigned long long)t->len);
        return;
    }
    /* 聚合值 / 指针 / 枚举：带上描述符，运行时才知道怎么展开 */
    if (any_has_desc(t)) {
        int d = any_desc(t);
        buf_printf(b, "zx_fa_any(&zx_at[%d], ", d);
        any_addr(b, a);
        buf_putc(b, ')');
        return;
    }
    if (t->kind == TY_SLICE && t->elem && t->elem->kind == TY_INT && t->elem->bits == 8) {
        buf_puts(b, "zx_fa_s(");
        ex(b, a);
        buf_putc(b, ')');
        return;
    }
    if (t->kind == TY_BOOL) {
        buf_puts(b, "zx_fa_b(");
        ex(b, a);
        buf_putc(b, ')');
        return;
    }
    if (ty_is_float(t)) {
        buf_puts(b, "zx_fa_f((double)(");
        ex(b, a);
        buf_puts(b, "))");
        return;
    }
    if (ty_is_int(t) || t->kind == TY_ENUM || t->kind == TY_ERROR ||
        t->kind == TY_COMPTIME_INT) {
        bool sgn = (t->kind == TY_INT && t->is_signed) || t->kind == TY_ENUM ||
                   t->kind == TY_COMPTIME_INT;
        if (t->kind == TY_ENUM && t->elem) sgn = t->elem->is_signed;
        /* 128 位与「超出 64 位的 comptime_int」走全精度路径；
         * 小字面量仍走 64 位，保住 {:x} / {:b} 的原有行为（M19） */
        if ((t->kind == TY_INT && t->bits == 128) ||
            (t->kind == TY_COMPTIME_INT && ct_int_big(a))) {
            g_need_i128 = true;
            buf_puts(b, sgn ? "zx_fa_i128((__int128)(" : "zx_fa_u128((unsigned __int128)(");
            ex(b, a);
            buf_puts(b, "))");
            return;
        }
        buf_puts(b, sgn ? "zx_fa_i((long long)(" : "zx_fa_u((unsigned long long)(");
        ex(b, a);
        buf_puts(b, "))");
        return;
    }
    buf_puts(b, "zx_fa_p((const void*)&(");
    ex(b, a);
    buf_puts(b, "))");
}

/* 实参数组：每个实参只求值一次 */
static void fmt_args(Buf *b, Node *n, int first) {
    if (n->nitems <= first) { buf_puts(b, "((const ZxFmtArg*)0)"); return; }
    buf_puts(b, "(const ZxFmtArg[]){ ");
    for (int i = first; i < n->nitems; i++) {
        if (i > first) buf_puts(b, ", ");
        fmt_arg_one(b, n->items[i]);
    }
    buf_puts(b, " }");
}

/* §211：CRT 无缓冲诊断写 + 崩溃自动打点。
 * 背景：stderr 走 stdio 全缓冲，abort()/SIGSEGV 不走退出清理，重定向时
 * 诊断打点整段丢失；用户靠 CRT _write(2,...) 才定位到悬垂指针崩溃。
 * 这里把所有运行时诊断统一到 zx_diagf（vsnprintf 到栈缓冲再一次
 * _write/write 到 fd 2），并装崩溃处理器：崩溃瞬间把信号/异常码 +
 * zx 位置影子变量无缓冲打出来。位置影子由 zx_loc() 在语句边界更新，
 * 报的是 zx 文件:行，不是 C 行号。
 * Windows 只能靠生成 C 里的 _WIN32（zig cc / mingw 提供 _write）；
 * POSIX 用 write(2)。两个平台都不经过 stdio 缓冲。 */
static void emit_diag_runtime(void) {
    bline(&g_bproto, "void zx_diag_raw(const char *s, size_t n);");
    bline(&g_bproto, "void zx_diagf(const char *fmt, ...);");
    bline(&g_bproto, "void zx_diag_flush(void);");
    bline(&g_bproto, "void zx_abort_reported(void);");
    bline(&g_bproto, "void zx_panic(const char *msg, const char *file, int line, int col);");
    bline(&g_bproto, "void zx_loc(const char *file, int line, int col);");
    bline(&g_bproto, "void zx_crash_init(void);");
    buf_puts(g_out,
        "#include <stdarg.h>\n"
        "#include <signal.h>\n"
        "#include <string.h>\n"
        "#if defined(_WIN32)\n"
        "#include <io.h>\n"
        "#else\n"
        "#include <unistd.h>\n"
        "#endif\n\n"
        "/* fd 2 low-level write: bypasses stdio buffering entirely. */\n"
        "void zx_diag_raw(const char *s, size_t n) {\n"
        "#if defined(_WIN32)\n"
        "    if (n) (void)_write(2, s, (unsigned)n);\n"
        "#else\n"
        "    if (n) (void)write(2, s, n);\n"
        "#endif\n"
        "}\n\n"
        "/* Diagnostics must appear before stdout's buffered reports. */\n"
        "void zx_diag_flush(void) { fflush(stdout); fflush(stderr); }\n\n"
        "void zx_diagf(const char *fmt, ...) {\n"
        "    char b[2048];\n"
        "    va_list ap; va_start(ap, fmt);\n"
        "    int n = vsnprintf(b, sizeof b, fmt, ap);\n"
        "    va_end(ap);\n"
        "    if (n < 0) return;\n"
        "    if ((size_t)n >= sizeof b) n = (int)sizeof b - 1;\n"
        "    zx_diag_raw(b, (size_t)n);\n"
        "}\n\n"
        "/* zx source position shadow, updated at statement boundaries. */\n"
        "static const char *zx_loc_file = \"?\";\n"
        "static int zx_loc_line = 0, zx_loc_col = 0;\n"
        "static volatile int zx_reported;   /* 显式诊断已打点 → 崩溃 handler 不重复报 */\n"
        "void zx_loc(const char *file, int line, int col) {\n"
        "    zx_loc_file = file; zx_loc_line = line; zx_loc_col = col;\n"
        "}\n"
        "void zx_abort_reported(void) {\n"
        "    zx_reported = 1;\n"
        "    fflush(stdout);   /* fatal: 冲掉退出清理带不走的 stdout 缓冲 */\n"
        "    abort();\n"
        "}\n\n"
        "/* @panic: one deterministic diagnostic at the zx source position. */\n"
        "void zx_panic(const char *msg, const char *file, int line, int col) {\n"
        "    if (msg && *msg) zx_diagf(\"panic: %s\\n  at %s:%d:%d\\n\", msg, file, line, col);\n"
        "    else             zx_diagf(\"panic\\n  at %s:%d:%d\\n\", file, line, col);\n"
        "    zx_abort_reported();\n"
        "}\n\n");
    /* 崩溃处理器：POSIX sigaction 全信号；Windows SetUnhandledExceptionFilter。
     * 全部只用 zx_diag_raw（async-signal-safe 的 write）输出，不碰 malloc/stdio。 */
    buf_puts(g_out,
        "/* §217: C/OS 层的故障信息（信号/异常码 + 故障地址），只在这里拼一次。\n"
        " * has_fault 独立于地址值：访问地址 0 也是有效信息，不能当\"无地址\"丢掉。 */\n"
        "static void zx_crash_report(const char *tag, void *fault_addr, int has_fault) {\n"
        "    zx_diag_raw(\"zx: fatal \", 10);\n"
        "    zx_diag_raw(tag, strlen(tag));\n"
        "    zx_diag_raw(\" at \", 4);\n"
        "    zx_diag_raw(zx_loc_file, strlen(zx_loc_file));\n"
        "    zx_diag_raw(\":\", 1);\n"
        "    char ln[24]; int m = snprintf(ln, sizeof ln, \"%d:%d\", zx_loc_line, zx_loc_col);\n"
        "    if (m > 0) zx_diag_raw(ln, (size_t)m < sizeof ln ? (size_t)m : sizeof ln - 1);\n"
        "    if (has_fault) {                  /* C/OS 报的非法访问地址，原样转发 */\n"
        "        zx_diag_raw(\" [fault addr \", 13);\n"
        "        char ab[32]; int an = snprintf(ab, sizeof ab, \"%p]\", fault_addr);\n"
        "        if (an > 0) zx_diag_raw(ab, (size_t)an < sizeof ab ? (size_t)an : sizeof ab - 1);\n"
        "    }\n"
        "    zx_diag_raw(\"\\n\", 1);\n"
        "}\n\n");
    /* 信号处理器两平台都装：abort()/除零走 CRT 的 signal，不走 SEH。
     * Windows 再额外挂 SEH filter 兜住真正的访问违例（SIGSEGV 在 mingw
     * CRT 下未必映射到 SEH）。两路都只调 zx_diag_raw，可重入、无缓冲。 */
    buf_puts(g_out,
        "static const char *zx_signal_name(int sig) {\n"
        "    switch (sig) {\n"
        "    case SIGSEGV: return \"SIGSEGV (invalid memory access)\";\n"
        "#ifndef _WIN32\n"
        "    case SIGBUS:  return \"SIGBUS (bad memory alignment)\";\n"
        "#endif\n"
        "    case SIGFPE:  return \"SIGFPE (floating point / divide by zero)\";\n"
        "    case SIGILL:  return \"SIGILL (illegal instruction)\";\n"
        "    case SIGABRT: return \"SIGABRT (abort)\";\n"
        "    default: return \"signal\";\n"
        "    }\n"
        "}\n\n"
        "static void zx_crash_signal(int sig) {\n"
        "    if (!zx_reported) zx_crash_report(zx_signal_name(sig), NULL, 0);\n"
        "    signal(sig, SIG_DFL); raise(sig);   /* keep the real exit status */\n"
        "}\n\n"
        "#ifndef _WIN32\n"
        "/* §217: SA_SIGINFO handler 拿到 C/OS 报的故障地址（si_addr），原样转发。\n"
        " * 第二参数用 void* 而非 siginfo_t*：原型提取器(buf_protos_from)会把\n"
        " * 签名提到共享头且不带平台条件，用平台类型会让 Windows 编译炸。 */\n"
        "static void zx_crash_fault(int sig, void *sip, void *ctx) {\n"
        "    (void)ctx;\n"
        "    siginfo_t *si = (siginfo_t *)sip;\n"
        "    if (!zx_reported) zx_crash_report(zx_signal_name(sig), si ? si->si_addr : NULL, si != NULL);\n"
        "    signal(sig, SIG_DFL); raise(sig);\n"
        "}\n"
        "#endif\n\n"
        "#if defined(_WIN32)\n"
        "/* MinGW / zig cc windows-gnu: EXCEPTION_POINTERS via windows.h is heavy;\n"
        " * declare the two records we read (ExceptionCode / ExceptionAddress). */\n"
        "typedef struct { unsigned long ExceptionCode; unsigned long ExceptionFlags;\n"
        "                 struct _ZX_ER *ExceptionRecord; void *ExceptionAddress;\n"
        "                 unsigned long NumberParameters; unsigned long long ExceptionInformation[15];\n"
        "} _ZX_ER;\n"
        "typedef struct { _ZX_ER *ExceptionRecord; void *ContextRecord; } _ZX_EP;\n"
        "typedef long (__stdcall *ZX_SEH_FN)(_ZX_EP *);\n"
        "extern void *SetUnhandledExceptionFilter(ZX_SEH_FN);\n"
        "static long __stdcall zx_seh_filter(void *arg) {\n"
        "    _ZX_EP *ep = (_ZX_EP *)arg;\n"
        "    char b[64];\n"
        "    unsigned long code = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode : 0;\n"
        "    /* 访问违例(0xC0000005)的 ExceptionInformation[1] 就是故障地址 */\n"
        "    void *addr = NULL; int has_addr = 0;\n"
        "    if (code == 0xC0000005UL) {\n"
        "        if (ep && ep->ExceptionRecord && ep->ExceptionRecord->NumberParameters >= 2) {\n"
        "            addr = (void *)(size_t)ep->ExceptionRecord->ExceptionInformation[1];\n"
        "            has_addr = 1;\n"
        "        }\n"
        "        snprintf(b, sizeof b, \"SIGSEGV (invalid memory access)\");\n"
        "    } else {\n"
        "        snprintf(b, sizeof b, \"seh exception 0x%lx\", (unsigned long)code);\n"
        "    }\n"
        "    if (!zx_reported) zx_crash_report(b, addr, has_addr);\n"
        "    return 1;  /* EXCEPTION_EXECUTE_HANDLER: terminate */\n"
        "}\n"
        "#endif\n\n"
        "void zx_crash_init(void) {\n"
        "#if defined(_WIN32)\n"
        "    /* §217: SIGSEGV 交给 SEH filter——signal() 的 handler 拿不到故障地址，\n"
        "     * mingw CRT 又会把访问违例先转成 SIGSEGV；不装它，SEH 才能报出地址。 */\n"
        "    signal(SIGFPE,  zx_crash_signal);\n"
        "    signal(SIGILL,  zx_crash_signal);\n"
        "    signal(SIGABRT, zx_crash_signal);\n"
        "    (void)SetUnhandledExceptionFilter((ZX_SEH_FN)zx_seh_filter);\n"
        "#else\n"
        "    /* §217: 内存类信号用 SA_SIGINFO，拿到 si_addr 原样转发 */\n"
        "    struct sigaction sa;\n"
        "    memset(&sa, 0, sizeof sa);\n"
        "    sa.sa_sigaction = (void (*)(int, siginfo_t *, void *))zx_crash_fault;\n"
        "    sa.sa_flags = SA_SIGINFO;\n"
        "    sigemptyset(&sa.sa_mask);\n"
        "    sigaction(SIGSEGV, &sa, NULL);\n"
        "    sigaction(SIGBUS,  &sa, NULL);\n"
        "    /* 其余信号没有故障地址意义，沿用简单 handler */\n"
        "    signal(SIGFPE,  zx_crash_signal);\n"
        "    signal(SIGILL,  zx_crash_signal);\n"
        "    signal(SIGABRT, zx_crash_signal);\n"
        "#endif\n"
        "}\n\n");
}

/* std.file：就是几个 fopen 包装。它们是普通函数调用——sema 给的符号上
 * 挂着 C 名字，所以调用点不需要任何特判。 */
/* std.heapAlloc 的运行时一端：穿过分配器的 alloc 函数指针。
 * 分配失败没有可选可返回（结果是非可选数组指针），直接 abort。 */
static void emit_heapalloc_runtime(void) {
    Type *al = sema_allocator_type();
    Type *op8 = ty_opt(ty_ptr(ty_u8(), false, false, 0));
    ensure_type(al); ensure_type(op8);
    Buf an; buf_init(&an); tn(&an, al);
    Buf on; buf_init(&on); tn(&on, op8);
    Buf un; buf_init(&un); ctype(&un, ty_usize());

    buf_printf(&g_bglobals, "static void *zx_heapalloc(%s al, %s n);\n\n",
               an.data, un.data);
    Buf *sv = g_out;
    int si = g_ind;
    g_out = &g_bbody;
    g_ind = 0;
    Buf mt; buf_init(&mt); ctype(&mt, op8);   /* ?*u8 的 C 类型：指针 */
    buf_printf(g_out,
        "static void *zx_heapalloc(%s al, %s n) {\n"
        "    %s r = al.alloc(al.ctx, n);\n"
        "    if (!r) { zx_diagf(\"zx: out of memory\\n\"); zx_abort_reported(); }\n"
        "    return r;\n"
        "}\n\n",
        an.data, un.data, mt.data);
    /* §130 预分配的对称释放：一次 alloc 一块，区间末整体归还。
     * 字段名走 dotname，与定义侧 mname 的 C 关键字转义保持一致。 */
    Buf fr; buf_init(&fr); dotname(&fr, "free");
    Buf cx; buf_init(&cx); dotname(&cx, "ctx");
    buf_printf(g_out,
        "static void zx_heapfree(%s al, void *p, %s n) {\n"
        "    al%s(al%s, (uint8_t *)p, n);\n"
        "}\n\n",
        an.data, un.data, fr.data, cx.data);
    g_out = sv;
    g_ind = si;
}

static void emit_file_runtime(void) {
    Type *s8  = ty_slice(ty_u8(), true);
    Type *os8 = ty_opt(s8);
    Type *op8 = ty_opt(ty_ptr(ty_u8(), false, false, 0));
    Type *al  = sema_allocator_type();
    ensure_type(s8); ensure_type(os8); ensure_type(op8); ensure_type(al);

    Buf sn, osn, opn, an, un, opcn;
    buf_init(&sn);  tn(&sn, s8);
    buf_init(&osn); tn(&osn, os8);
    buf_init(&opn); tn(&opn, op8);
    buf_init(&opcn); ctype(&opcn, op8);   /* ?*u8 的 C 类型（M26 后是指针） */
    buf_init(&an);  tn(&an, al);
    buf_init(&un);  ctype(&un, ty_usize());

    buf_printf(&g_bglobals,
        "static %s zx_file_read(%s al, %s path);\n"
        "static bool zx_file_put(%s path, %s data, const char *mode);\n"
        "static bool zx_file_write(%s path, %s data);\n"
        "static bool zx_file_append(%s path, %s data);\n"
        "static bool zx_file_exists(%s path);\n"
        "static bool zx_file_remove(%s path);\n\n",
        osn.data, an.data, sn.data,
        sn.data, sn.data,
        sn.data, sn.data, sn.data, sn.data, sn.data, sn.data);

    Buf *sv = g_out;
    int si = g_ind;
    g_out = &g_bbody;
    g_ind = 0;
    buf_printf(g_out,
        "static %s zx_file_read(%s al, %s path) {\n"
        "    %s r;\n"
        "    r.tag = false; r.payload.ptr = 0; r.payload.len = 0;\n"
        "    char *c = zx_path_cstr(path);\n"
        "    if (!c) return r;\n"
        "    FILE *f = fopen(c, \"rb\");\n"
        "    free(c);\n"
        "    if (!f) return r;\n"
        "    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return r; }\n"
        "    long n = ftell(f);\n"
        "    if (n < 0) { fclose(f); return r; }\n"
        "    rewind(f);\n"
        "    %s mem = al.alloc(al.ctx, (%s)n + 1);\n"
        "    if (!mem) { fclose(f); return r; }\n"
        "    %s got = fread(mem, 1, (%s)n, f);\n"
        "    fclose(f);\n"
        "    mem[got] = 0;      /* 当 C 串用也安全 */\n"
        "    r.payload.ptr = mem;\n"
        "    r.payload.len = got;\n"
        "    r.tag = true;\n"
        "    return r;\n"
        "}\n\n",
        osn.data, an.data, sn.data, osn.data, opcn.data, un.data, un.data, un.data);
    buf_printf(g_out,
        "static bool zx_file_put(%s path, %s data, const char *mode) {\n"
        "    char *c = zx_path_cstr(path);\n"
        "    if (!c) return false;\n"
        "    FILE *f = fopen(c, mode);\n"
        "    free(c);\n"
        "    if (!f) return false;\n"
        "    bool ok = (data.len == 0) || (fwrite(data.ptr, 1, data.len, f) == data.len);\n"
        "    if (fclose(f) != 0) ok = false;\n"
        "    return ok;\n"
        "}\n\n"
        "static bool zx_file_write(%s path, %s data) { return zx_file_put(path, data, \"wb\"); }\n"
        "static bool zx_file_append(%s path, %s data) { return zx_file_put(path, data, \"ab\"); }\n"
        "static bool zx_file_exists(%s path) {\n"
        "    char *c = zx_path_cstr(path);\n"
        "    if (!c) return false;\n"
        "    FILE *f = fopen(c, \"rb\");\n"
        "    free(c);\n"
        "    if (!f) return false;\n"
        "    fclose(f);\n"
        "    return true;\n"
        "}\n"
        "static bool zx_file_remove(%s path) {\n"
        "    char *c = zx_path_cstr(path);\n"
        "    if (!c) return false;\n"
        "    int rc = remove(c);\n"
        "    free(c);\n"
        "    return rc == 0;\n"
        "}\n\n",
        sn.data, sn.data,
        sn.data, sn.data, sn.data, sn.data, sn.data, sn.data);
    g_out = sv;
    g_ind = si;
}

/* 路径切片 → C 串。std.file 与 std.io 共用一份（两边都用它就是"重复定义"）。 */
static void emit_cstr_runtime(void) {
    Type *s8 = ty_slice(ty_u8(), true);
    ensure_type(s8);
    Buf sn; buf_init(&sn); tn(&sn, s8);
    buf_printf(&g_bglobals, "static char *zx_path_cstr(%s s);\n\n", sn.data);

    Buf *sv = g_out;
    int si = g_ind;
    g_out = &g_bbody;
    g_ind = 0;
    buf_printf(g_out,
        "static char *zx_path_cstr(%s s) {\n"
        "    char *p = (char*)malloc(s.len + 1);\n"
        "    if (!p) return 0;\n"
        "    if (s.len) memcpy(p, s.ptr, s.len);\n"
        "    p[s.len] = 0;\n"
        "    return p;\n"
        "}\n\n",
        sn.data);
    g_out = sv;
    g_ind = si;
}

/* std.io：句柄就是 FILE*，装进 usize 里。句柄值 0 表示"没打开"，
 * close/flush 遇到 0 就当无事——省掉调用方的判空。 */
/* std.process 的运行时：命令行 / 环境变量 / 退出。
 * argc/argv 由 main 接住存进外部全局量（zx_proc_argc/argv），这里懒初始化。 */
static void emit_proc_runtime(void) {
    Type *s8 = ty_slice(ty_u8(), true);
    Type *ss = ty_slice(s8, false);          /* [][]const u8 */
    Type *os = ty_opt(s8);
    ensure_type(s8); ensure_type(ss); ensure_type(os);
    Buf sn; buf_init(&sn); tn(&sn, s8);
    Buf snn; buf_init(&snn); tn(&snn, ss);
    Buf on; buf_init(&on); tn(&on, os);

    buf_printf(&g_bglobals,
        "extern int zx_proc_argc;\n"
        "extern char **zx_proc_argv;\n"
        "static void zx_proc_init(int argc, char **argv);\n"
        "static %s zx_proc_args(void);\n"
        "static %s zx_proc_getenv(%s name);\n"
        "static bool zx_proc_setenv(%s name, %s value);\n"
        "static void zx_proc_exit(uint8_t code);\n\n",
        snn.data,                            /* args */
        on.data, sn.data,                    /* getEnv */
        sn.data, sn.data);                   /* setEnv */

    /* 定义在入口单元（main 前面）；这里只有 extern 声明——头文件会被
     * 每个单元 include，定义放这儿会变成多份外部定义。 */
    buf_printf(g_out,
        "static int zx_p_argc = 0;\n"
        "static char **zx_p_argv = 0;\n"
        "static bool zx_p_args_ok = false;\n"
        "static %s zx_p_args_v;\n\n"
        "static void zx_proc_init(int argc, char **argv) {\n"
        "    zx_p_argc = argc;\n"
        "    zx_p_argv = argv;\n"
        "}\n\n"
        "static %s zx_proc_args(void) {\n"
        "    if (!zx_p_args_ok) {\n"
        "        if (!zx_p_argv) zx_proc_init(zx_proc_argc, zx_proc_argv);\n"
        "        int n = zx_p_argc > 0 ? zx_p_argc - 1 : 0;   /* argv[0] 是程序名 */\n"
        "        %s *arr = (%s *)malloc(sizeof(%s) * (size_t)(n > 0 ? n : 1));\n"
        "        for (int i = 0; i < n; i++) {\n"
        "            char *s = zx_p_argv[i + 1];\n"
        "            arr[i].ptr = (uint8_t *)s;\n"
        "            arr[i].len = strlen(s);\n"
        "        }\n"
        "        zx_p_args_v.ptr = arr;\n"
        "        zx_p_args_v.len = (size_t)n;\n"
        "        zx_p_args_ok = true;\n"
        "    }\n"
        "    return zx_p_args_v;\n"
        "}\n\n"
        "static %s zx_proc_getenv(%s name) {\n"
        "    %s r; r.tag = false;\n"
        "    char buf[512];\n"
        "    size_t n = name.len < (sizeof buf) - 1 ? name.len : (sizeof buf) - 1;\n"
        "    if (name.ptr && n) memcpy(buf, name.ptr, n);\n"
        "    buf[n] = 0;\n"
        "    const char *v = getenv(buf);\n"
        "    if (v) {\n"
        "        r.tag = true;\n"
        "        r.payload.ptr = (uint8_t *)v;\n"
        "        r.payload.len = strlen(v);\n"
        "    }\n"
        "    return r;\n"
        "}\n\n"
        "static bool zx_proc_setenv(%s name, %s value) {\n"
        "    char nb[512], vb[2048];\n"
        "    size_t n = name.len < (sizeof nb) - 1 ? name.len : (sizeof nb) - 1;\n"
        "    size_t m = value.len < (sizeof vb) - 1 ? value.len : (sizeof vb) - 1;\n"
        "    if (name.ptr && n) memcpy(nb, name.ptr, n);\n"
        "    nb[n] = 0;\n"
        "    if (value.ptr && m) memcpy(vb, value.ptr, m);\n"
        "    vb[m] = 0;\n"
        "#ifdef _WIN32\n"
        "    char both[2560];\n"
        "    snprintf(both, sizeof both, \"%%s=%%s\", nb, vb);\n"
        "    return _putenv(both) == 0;\n"
        "#else\n"
        "    return setenv(nb, vb, 1) == 0;\n"
        "#endif\n"
        "}\n\n"
        "static void zx_proc_exit(uint8_t code) { exit((int)code); }\n\n",
        snn.data,                                        /* cache var */
        snn.data, sn.data, sn.data, sn.data,             /* args：返回外层切片，arr 是元素切片 */
        on.data, sn.data, on.data,                       /* getEnv */
        sn.data, sn.data);                               /* setEnv */
}

static void emit_io_runtime(void) {
    Type *s8 = ty_slice(ty_u8(), true);
    Type *ou = ty_opt(ty_usize());
    ensure_type(s8); ensure_type(ou);
    Buf sn; buf_init(&sn); tn(&sn, s8);
    Buf on; buf_init(&on); tn(&on, ou);
    Buf un; buf_init(&un); ctype(&un, ty_usize());

    /* 纯同步 io（decisions §123）：调用点直接落到 libc 的 fread/fwrite/
     * fseek/ftell/fclose/fflush，没有 hook、没有 offload、没有隐式线程。
     * 要并发就显式用 std.task 起任务再做 io —— 编译器不替用户做调度决策。
     * 原型放 header 是因为生成 C 是多单元编译。 */
    buf_printf(&g_bglobals,
        "static %s zx_io_openm(%s path, const char *mode);\n"
        "static %s zx_io_openr(%s path);\n"
        "static %s zx_io_openw(%s path);\n"
        "static %s zx_io_opena(%s path);\n"
        "static %s zx_io_read(%s h, %s buf);\n"
        "static %s zx_io_write(%s h, %s data);\n"
        "static bool zx_io_seek(%s h, %s off);\n"
        "static %s zx_io_tell(%s h);\n"
        "static void zx_io_close(%s h);\n"
        "static void zx_io_flush(%s h);\n"
        "static %s zx_io_stdin(void);\n"
        "static %s zx_io_stdout(void);\n"
        "static %s zx_io_stderr(void);\n\n",
        on.data, sn.data,          /* openm */
        on.data, sn.data,          /* openr */
        on.data, sn.data,          /* openw */
        on.data, sn.data,          /* opena */
        un.data, un.data, sn.data, /* read  */
        un.data, un.data, sn.data, /* write */
        un.data, un.data,          /* seek  */
        un.data, un.data,          /* tell  */
        un.data,                   /* close */
        un.data,                   /* flush */
        un.data, un.data, un.data);

    Buf *sv = g_out;
    int si = g_ind;
    g_out = &g_bbody;
    g_ind = 0;
    buf_printf(g_out,
        "static %s zx_io_openm(%s path, const char *mode) {\n"
        "    %s r; r.payload = 0; r.tag = false;\n"
        "    char *c = zx_path_cstr(path);\n"
        "    if (!c) return r;\n"
        "    FILE *f = fopen(c, mode);\n"
        "    free(c);\n"
        "    if (!f) return r;\n"
        "    r.payload = (uint64_t)(size_t)f; r.tag = true;\n"
        "    return r;\n"
        "}\n"
        "static %s zx_io_openr(%s path)  { return zx_io_openm(path, \"rb\"); }\n"
        "static %s zx_io_openw(%s path)  { return zx_io_openm(path, \"wb\"); }\n"
        "static %s zx_io_opena(%s path)  { return zx_io_openm(path, \"ab\"); }\n"
        "/* 纯同步：调用点直接落到 libc，没有 hook / offload / 隐式线程。\n"
        "  * 要并发就显式用 std.task 起任务再做 io（decisions 123）。 */\n"
        "static %s zx_io_read(%s h, %s buf) {\n"
        "    return h ? fread((void *)buf.ptr, 1, buf.len, (FILE *)(size_t)h) : 0;\n"
        "}\n"
        "static %s zx_io_write(%s h, %s data) {\n"
        "    return (h && data.len) ? fwrite((const void *)data.ptr, 1, data.len, (FILE *)(size_t)h) : 0;\n"
        "}\n"
        "static bool zx_io_seek(%s h, %s off) {\n"
        "    return h && fseek((FILE *)(size_t)h, (long)off, SEEK_SET) == 0;\n"
        "}\n"
        "static %s zx_io_tell(%s h) {\n"
        "    long pos = h ? ftell((FILE *)(size_t)h) : -1;\n"
        "    return pos < 0 ? 0 : (%s)pos;\n"
        "}\n"
        "static void zx_io_close(%s h) {\n"
        "    if (h) fclose((FILE *)(size_t)h);\n"
        "}\n"
        "static void zx_io_flush(%s h) {\n"
        "    if (h) fflush((FILE *)(size_t)h);\n"
        "}\n"
        "static %s zx_io_stdin(void)  { return (%s)(size_t)stdin; }\n"
        "static %s zx_io_stdout(void) { return (%s)(size_t)stdout; }\n"
        "static %s zx_io_stderr(void) { return (%s)(size_t)stderr; }\n\n",
        on.data, sn.data, on.data, /* openm */
        on.data, sn.data,          /* openr */
        on.data, sn.data,          /* openw */
        on.data, sn.data,          /* opena */
        un.data, un.data, sn.data, /* read  */
        un.data, un.data, sn.data, /* write */
        un.data, un.data,          /* seek  */
        un.data, un.data, un.data, /* tell: 返回 / h / cast */
        un.data,                   /* close */
        un.data,                   /* flush */
        un.data, un.data,          /* stdin */
        un.data, un.data,          /* stdout */
        un.data, un.data);         /* stderr */
    g_out = sv;
    g_ind = si;
}

/* std.debug 运行时：panic / assert 的位置实参由编译器在调用点注入成字面量；
 * errName 的码 → 名字静态表在这里铺出（进代码生成时 sema 已把整个工程的
 * 错误集登记完，直接按码序展开成字符串数组，查表 O(1)）；
 * assert 的函数体在 release（-DZX_RELEASE）下整体擦除。 */
static void emit_debug_runtime(void) {
    /* 函数签名与返回值都写 ZxSlice_u8，类型必须先有 typedef——
     * 调用点传常量错误码时会顺带 ensure，传运行期错误值就没有
     * 别的路径补上（README 的 demo 撞出来的），在这里自己 ensure。 */
    ensure_type(ty_slice(ty_u8(), true));
    int ne = ty_err_count();
    /* 表无条件发射：枚举错误（(error)Err.x）不进登记表，ne 可能是 0，
     * 但 printTrace 的代码路径始终引用它——空表 + 哨兵兜底。 */
    buf_puts(g_out, "static const char *zx_err_names[] = {\n    NULL");
    for (uint16_t c = 1; c <= (uint16_t)ne; c++) {
        const char *nm = ty_err_name(c);
        if (!nm) nm = "?";
        int nl = (int)strlen(nm);
        Buf s; buf_init(&s);
        cstr(&s, nm, nl);
        buf_printf(g_out, ",\n    %s", s.data);
    }
    buf_puts(g_out, "\n};\n\n");
    bline(&g_bproto, "ZxSlice_u8 zx_debug_errname(uint64_t code);");
    buf_puts(g_out, "ZxSlice_u8 zx_debug_errname(uint64_t code) {\n");
    buf_printf(g_out,
        "    ZxSlice_u8 r;\n"
        /* 65535 是 null 的保留码（`?T` 被 try 时抛的就是它），不在顺序
         * 码表里，得单独认；否则 errName(error.null) 打 "?"。 */
        "    const char *s = (code == 65535) ? \"null\"\n"
        "                  : ((code > 0 && code <= %d) ? zx_err_names[code] : \"?\");\n"
        "    r.ptr = (uint8_t *)s; r.len = strlen(s);\n"
        "    return r;\n"
        "}\n\n", ne);

    bline(&g_bproto, "void zx_debug_panic(ZxSlice_u8 msg, ZxSlice_u8 file,"
                     " int32_t line, int32_t col, ZxSlice_u8 fn);");
    bline(&g_bproto, "void zx_debug_assert(bool cond, ZxSlice_u8 msg, ZxSlice_u8 file,"
                     " int32_t line, int32_t col, ZxSlice_u8 fn);");
    buf_puts(g_out,
        "void zx_debug_panic(ZxSlice_u8 msg, ZxSlice_u8 file, int32_t line, int32_t col, ZxSlice_u8 fn) {\n"
        /* 刷 panic 之前先把 stdout 冲下去：被重定向时 stdout 是全缓冲，
         * abort() 不走退出清理，诊断前的报告本来会丢。即便是控制台的行缓冲也一样。 */
        "    zx_diagf(\"panic: %.*s\\n  at %.*s:%d:%d in %.*s\\n\",\n"
        "            (int)msg.len, (const char *)msg.ptr,\n"
        "            (int)file.len, (const char *)file.ptr, line, col,\n"
        "            (int)fn.len, (const char *)fn.ptr);\n"
        "    zx_abort_reported();\n"
        "}\n\n"
        "void zx_debug_assert(bool cond, ZxSlice_u8 msg, ZxSlice_u8 file, int32_t line, int32_t col, ZxSlice_u8 fn) {\n"
        "#ifndef ZX_RELEASE\n"
        "    if (!cond) zx_debug_panic(msg, file, line, col, fn);\n"
        "#else\n"
        "    (void)cond; (void)msg; (void)file; (void)line; (void)col; (void)fn;\n"
        "#endif\n"
        "}\n\n");

    /* 真正的异步感知调用链由捕获感知纤程提供：`else err { … }` 的捕获点
     * （gen_if_body）把 {码, file, line, col, fn} 压进当前上下文的捕获环，
     * printTrace 回放。捕获环在纤程运行时里（emit_fiber_runtime 先发）。 */
    bline(&g_bproto, "void zx_debug_printtrace(void);");
    buf_printf(g_out,
        "void zx_debug_printtrace(void) {\n"
        "    ZxCap *c = (ZxCap *)zx_cap_current();\n"
        "    if (c->n == 0) { zx_diagf(\"zx trace: no captured errors\\n\"); return; }\n"
        "    zx_diagf(\"zx trace (oldest first):\\n\");\n"
        "    for (int i = 0; i < c->n; i++) {\n"
        "        ZxCapRec *r = &c->recs[(c->start + i) %% ZX_CAP_MAX];\n"
        "        const char *nm = (r->code == 65535) ? \"null\"\n"
        "                       : ((r->code > 0 && r->code <= %d) ? zx_err_names[r->code] : \"?\");\n"
        "        zx_diagf(\"  %%s at %%s:%%u:%%u in %%s\\n\",\n"
        "                nm, r->file, r->line, r->col, r->fn);\n"
        "    }\n"
        "}\n\n", ne);
}

/* std.atomic 运行时（纯函数，SEQ_CST）。M58 起并发类型只留 Task 实例，
 * mutex/cond/thread 的运行时一并收掉。按需发射——不用原子就不背这份代码。 */
static void emit_sync_runtime(void) {
    /* M58：并发类型只留 Task 实例，mutex/cond 的运行时一并收掉。
     * std.atomic 是纯函数（不是并发类型），保留。 */
    bline(&g_bproto, "uint64_t zx_atomic_add(uint64_t *p, uint64_t v);");
    bline(&g_bproto, "uint64_t zx_atomic_sub(uint64_t *p, uint64_t v);");
    bline(&g_bproto, "uint64_t zx_atomic_load(uint64_t *p);");
    bline(&g_bproto, "void zx_atomic_store(uint64_t *p, uint64_t v);");
    bline(&g_bproto, "uint64_t zx_atomic_swap(uint64_t *p, uint64_t v);");
    bline(&g_bproto, "bool zx_atomic_cas(uint64_t *p, uint64_t old, uint64_t nv);");
    buf_puts(g_out,
        "uint64_t zx_atomic_add(uint64_t *p, uint64_t v) {\n"
        "    return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);\n"
        "}\n\n"
        "uint64_t zx_atomic_sub(uint64_t *p, uint64_t v) {\n"
        "    return __atomic_fetch_sub(p, v, __ATOMIC_SEQ_CST);\n"
        "}\n\n"
        "uint64_t zx_atomic_load(uint64_t *p) {\n"
        "    return __atomic_load_n(p, __ATOMIC_SEQ_CST);\n"
        "}\n\n"
        "void zx_atomic_store(uint64_t *p, uint64_t v) {\n"
        "    __atomic_store_n(p, v, __ATOMIC_SEQ_CST);\n"
        "}\n\n"
        "uint64_t zx_atomic_swap(uint64_t *p, uint64_t v) {\n"
        "    return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);\n"
        "}\n\n"
        "bool zx_atomic_cas(uint64_t *p, uint64_t old, uint64_t nv) {\n"
        "    return __atomic_compare_exchange_n(p, &old, nv, 0,\n"
        "        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);\n"
        "}\n\n");
}

static void emit_task_runtime(void) {
    unsigned cc = (unsigned)ty_err_code("cancelled");
    unsigned cl = (unsigned)ty_err_code("closed");
    unsigned wb = (unsigned)ty_err_code("would_block");
    unsigned to = (unsigned)ty_err_code("timeout");
    Type *task_t = ty_fn((Type *[]){ ty_usize() }, 1, ty_err(ty_usize()));
    Type *erru   = ty_err(ty_usize());
    Type *optu   = ty_opt(ty_usize());
    Type *errv   = ty_err(ty_void());
    ensure_type(task_t);
    ensure_type(erru);
    ensure_type(optu);
    ensure_type(errv);
    Buf tb; buf_init(&tb); ctype(&tb, task_t);
    Buf en; buf_init(&en); tn(&en, erru);
    Buf ob; buf_init(&ob); tn(&ob, optu);
    Buf ev; buf_init(&ev); tn(&ev, errv);

    /* 对外 API 的原型由 std/task.zx 的 extern 声明提供；这里只发
     * 编译器注入的门与捕获环需要的原型。 */
    bline(&g_bproto, "void zx_cap_push(unsigned code, const char *file,"
                     " unsigned line, unsigned col, const char *fn);");
    bline(&g_bproto, "void *zx_cap_current(void);");
    bline(&g_bproto, "bool zx_task_await_gate(void);");
    bline(&g_bproto, "bool zx_task_cancel_check(void);");

    buf_printf(g_out,
        "#define ZX_ERR_CANCELLED %uu\n"
        "#define ZX_ERR_CLOSED %uu\n"
        "#define ZX_ERR_WOULDBLOCK %uu\n"
        "#define ZX_ERR_TIMEOUT %uu\n"
        "#define ZX_TASKFN %s\n"
        "#define ZX_ERRU %s\n"
        "#define ZX_OPTU %s\n"
        "#define ZX_ERRV %s\n\n",
        cc, cl, wb, to, tb.data, en.data, ob.data, ev.data);
    buf_puts(g_out,
        " \n"
        " /* ===== M58：per-instance 调度器 =====\n"
        "  * 每个 Task 实例物理隔离：自己的任务表、就绪队列、idle cv、等待者链和\n"
        "  * 一组线程。实例表只增不减、close 不释放内存 —— 任意线程并发持有旧\n"
        "  * 槽位不复用（避免 ABA）；close 后 stop=true，所有操作返回 closed。 */\n"
        " #ifdef _WIN32\n"
        " extern void *ConvertThreadToFiber(void *arg);\n"
        " extern void *CreateFiber(unsigned long long st, void (*fn)(void *), void *arg);\n"
        " extern void SwitchToFiber(void *f);\n"
        " extern void DeleteFiber(void *f);\n"
        " extern uint64_t CreateThread(void *attr, unsigned long long stack,\n"
        "     uint64_t (*fn)(void *), void *arg, uint64_t flags, uint64_t *tid);\n"
        " extern void InitializeConditionVariable(void *cv);\n"
        " extern int SleepConditionVariableSRW(void *cv, void *srw,\n"
        "     unsigned long ms, unsigned long flags);\n"
        " extern void WakeAllConditionVariable(void *cv);\n"
        " extern void AcquireSRWLockExclusive(void *l);\n"
        " extern void ReleaseSRWLockExclusive(void *l);\n"
        " extern unsigned short GetActiveProcessorCount(unsigned short group);\n"
        " extern int CloseHandle(uint64_t h);\n"
        " extern void Sleep(unsigned long ms);\n"
        " #define ZX_TASK_STACK 0\n"
        " typedef void *ZxLk;\n"
        " typedef void *ZxCv;\n"
        " typedef uint64_t (*ZxThreadFn)(void *);\n"
        " #else\n"
        " #ifndef _POSIX_C_SOURCE\n"
        " #define _POSIX_C_SOURCE 200809L\n"
        " #endif\n"
        " #include <pthread.h>\n"
        " #include <unistd.h>\n"
        " #include <time.h>\n"
        " #include <ucontext.h>\n"
        " #define ZX_TASK_STACK (1024 * 1024)\n"
        " typedef pthread_mutex_t ZxLk;\n"
        " typedef pthread_cond_t ZxCv;\n"
        " typedef void *(*ZxThreadFn)(void *);\n"
        " extern int sched_yield(void);\n"
        " #endif\n"
        " \n"
        " /* 单调毫秒钟：定时器 / deadline 用。不引入后台 timer 线程 —— worker\n"
        "  * 的 cv_wait 本来就是 5ms 超时，根上下文的 drive 循环也按同样节奏\n"
        "  * 醒来，两边轮询 timer 链即可（精度与调度器心跳一致）。 */\n"
        " #ifdef _WIN32\n"
        " extern unsigned long long GetTickCount64(void);\n"
        " static uint64_t zx_now_ms(void) { return (uint64_t)GetTickCount64(); }\n"
        " #else\n"
        " static uint64_t zx_now_ms(void) {\n"
        "     struct timespec ts;\n"
        "     clock_gettime(CLOCK_MONOTONIC, &ts);\n"
        "     return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;\n"
        " }\n"
        " #endif\n"
        " \n"
        " /* ===== 原语层：zx 表达不了的平台机制 =====\n"
        "  * 这里只放「造/切/回收执行载体」和「问平台」的函数：CPU 数、OS 线程、\n"
        "  * 纤程栈、锁与条件变量、单调钟、__thread 当前纤程。上面的类型与下面\n"
        "  * 的调度层（就绪队列 / 任务表 / 等待链 / timer / Scope / select）一律\n"
        "  * 通过这些原语访问平台，不再直接出现 CreateThread / ucontext / SRWLOCK。\n"
        "  * 库层 std/task.zx 只见到 extern 入口，内部机制对用户不可见。 */\n"
        " static int zx_rt_cpu_count(void) {\n"
        " #ifdef _WIN32\n"
        "     unsigned short n = GetActiveProcessorCount(0xFFFF);\n"
        "     return (int)n;\n"
        " #else\n"
        "     long n = sysconf(_SC_NPROCESSORS_ONLN);\n"
        "     return (int)(n > 0 ? n : 1);\n"
        " #endif\n"
        " }\n"
        " static bool zx_rt_thread_spawn(ZxThreadFn fn, void *arg) {\n"
        " #ifdef _WIN32\n"
        "     uint64_t h = CreateThread(NULL, 0, fn, arg, 0, NULL);\n"
        "     if (!h) return false;\n"
        "     CloseHandle(h);\n"
        "     return true;\n"
        " #else\n"
        "     pthread_t th;\n"
        "     if (pthread_create(&th, NULL, fn, arg) != 0) return false;\n"
        "     pthread_detach(th);\n"
        "     return true;\n"
        " #endif\n"
        " }\n"
        " static void zx_rt_backoff(int rest) {\n"
        " #ifdef _WIN32\n"
        "     if (rest < 64) Sleep(0); else Sleep(1);\n"
        " #else\n"
        "     if (rest < 64) sched_yield();\n"
        "     else { struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL); }\n"
        " #endif\n"
        " }\n"
        " \n"
        " /* 句柄是实例内单调递增的序号，永不复用 —— 不存在 ABA，也不会因为\n"
        "  * 槽位回收让「awaitAll 之后再 await(h) 取结果」失效（§94）。\n"
        "  * 结果按 id 存分段表：段按需 calloc，ZxTask 对象用完即 free，\n"
        "  * close 时整表一起释放。累计 spawn 数不再受 1024 限制。 */\n"
        " #define ZX_SEG_SIZE 256\n"
        " #define ZX_MAX_SEGS 4096\n"
        " #define ZX_CAP_MAX 64\n"
        " /* done_ms：结算时刻（zx_now_ms）。deadline 判定时不能只看「现在是否\n"
        "  * 全完成」——纯计算任务可能在一次调度里跑过截止时间才结算，那时\n"
        "  * 轮询循环回头一看 left==0 就误判成功。用完成时刻精确判定。 */\n"
        " typedef struct { uint64_t ret, done_ms; uint16_t tag; bool done; } ZxSlot;\n"
        " typedef struct { uint16_t code; const char *file; unsigned line, col; const char *fn; } ZxCapRec;\n"
        " typedef struct { ZxCapRec recs[ZX_CAP_MAX]; int n, start; } ZxCap;\n"
        " static __thread ZxCap zx_cap_root;\n"
        " \n"
        " /* 停靠状态：挂链到切走之间唤醒方可能抢先。用 pstate 仲裁，保证\n"
        "  * 「任务在就绪队列里的时刻必然不在运行」这条不变量。 */\n"
        " #define ZX_RUN 0\n"
        " #define ZX_PARKING 1\n"
        " #define ZX_PARKED 2\n"
        " #define ZX_WOKEN 3\n"
        " \n"
        " typedef struct ZxSched ZxSched;\n"
        " typedef struct ZxScope ZxScope;\n"
        " typedef struct ZxTask {\n"
        " #ifdef _WIN32\n"
        "     void *h;\n"
        " #else\n"
        "     ucontext_t ctx;\n"
        "     char *stk;\n"
        " #endif\n"
        "     ZX_TASKFN fn;\n"
        "     uint64_t arg;\n"
        "     struct ZxTask *back, *next, *waiters;\n"
        "     ZxSched *s;\n"
        "     bool done, cancelled, started, is_root, inited, requeue;\n"
        "     int pstate;\n"
        "     char deleted;\n"
        "     uint16_t tag;\n"
        "     uint64_t ret, id, cval;\n"
        "     bool cok;\n"
        "     ZxCap cap;\n"
        "     ZxScope *sc;              /* 所属作用域；finish 时做 fail-fast 登记 */\n"
        "     uint64_t deadline;        /* 定时器到点时刻（毫秒，zx_now_ms） */\n"
        "     bool timed, tm_fired;\n"
        "     struct ZxTask *tm_next;   /* 实例 timer 链；只在 tm_lk 下改 */\n"
        "     struct ZxChan *park_ch;  /* 停靠在哪条通道上；NULL = 实例默认通道 */\n"
        " } ZxTask;\n"
        " \n"
        " /* 命名通道：与实例自带的默认 rendezvous 同构，只是各自有等待链。\n"
        "  * 壳挂在实例的 ch_list 上，由 zx_task_close 统一 free —— close 之后\n"
        "  * 旧通道句柄仍可安全查询 closed，不会悬垂（与 Scope 同策略）。 */\n"
        " typedef struct ZxChan {\n"
        "     struct ZxChan *next_all;\n"
        "     ZxSched *s;\n"
        "     ZxTask *rwait, *wwait;\n"
        "     bool closed;\n"
        " } ZxChan;\n"
        " \n"
        " typedef struct { ZxTask *task[ZX_SEG_SIZE]; ZxSlot slot[ZX_SEG_SIZE]; } ZxSeg;\n"
        " \n"
        " struct ZxSched {\n"
        "     uint64_t id;\n"
        "     int mode, nworkers, alive_workers;\n"
        "     bool stop, closed;\n"
        "     ZxSeg *segs[ZX_MAX_SEGS];\n"
        "     uint64_t nids;\n"
        "     ZxLk tab_lk;\n"
        "     ZxLk qlk;\n"
        "     ZxTask *qhead, *qtail;\n"
        "     ZxLk idle_lk;\n"
        "     ZxCv idle_cv;\n"
        "     int sleeping;\n"
        "     ZxLk wait_lk;\n"
        "     int pending;\n"
        "     ZxTask *all_waiters;\n"
        "     ZxLk chan_lk;\n"
        "     ZxTask *rwait, *wwait;\n"
        "     ZxLk tm_lk;               /* 定时器链 + 通道表的共用保护 */\n"
        "     ZxTask *timers;\n"
        "     ZxChan *ch_list;          /* 命名通道，close 时统一回收 */\n"
        "     ZxScope *sc_list;          /* 本实例开过的作用域，close 时统一回收 */\n"
        " };\n"
        " \n"
        " /* ===== 结构化并发：Scope 是绑定到某个实例上的「词法作用域」 =====\n"
        "  * spawn 登记成员；任一成员带非 0 tag 结算时记下首个错误并立刻取消\n"
        "  * 其余兄弟（fail-fast）；join/close 等全体成员收尾。作用域退出时\n"
        "  * 由 std/task.zx 的 defer 调 close —— 正常返回与 !T 错误传播都会走，\n"
        "  * 所以父任务被取消时子任务树必然被级联取消，不会有任务泄漏。\n"
        "  * 边界同 §94：scope()/join()/close() 由创建者串行使用，spawn 可并发。 */\n"
        " struct ZxScope {\n"
        "     ZxSched *s;\n"
        "     uint64_t *ids;\n"
        "     int n, cap;\n"
        "     uint16_t first_tag;\n"
        "     uint64_t first_ret;\n"
        "     bool failed, closed;\n"
        "     ZxLk lk;\n"
        "     ZxScope *next_all;\n"
        " };\n"
        " static void zx_sc_cancel_all(ZxScope *sc);\n"
        " static void zx_task_cancel_one(ZxSched *s, ZxTask *t);\n"
        " \n"
        " /* 实例表只由外部调用者访问；worker 线程只碰自己实例内部的状态，不需要全局表锁 */\n"
        " static ZxSched **zx_sched_tab;\n"
        " static int zx_nsched, zx_sched_cap;\n"
        " \n"
        " static __thread ZxTask zx_tk_root;\n"
        " static __thread ZxTask *zx_fb_cur;\n"
        " \n"
        " static void zx_lk_init(ZxLk *l) {\n"
        " #ifdef _WIN32\n"
        "     *l = NULL;\n"
        " #else\n"
        "     pthread_mutex_init(l, NULL);\n"
        " #endif\n"
        " }\n"
        " static void zx_cv_init(ZxCv *c) {\n"
        " #ifdef _WIN32\n"
        "     InitializeConditionVariable(c);\n"
        " #else\n"
        "     pthread_cond_init(c, NULL);\n"
        " #endif\n"
        " }\n"
        " static void zx_lk(ZxLk *l) {\n"
        " #ifdef _WIN32\n"
        "     AcquireSRWLockExclusive(l);\n"
        " #else\n"
        "     pthread_mutex_lock(l);\n"
        " #endif\n"
        " }\n"
        " static void zx_ulk(ZxLk *l) {\n"
        " #ifdef _WIN32\n"
        "     ReleaseSRWLockExclusive(l);\n"
        " #else\n"
        "     pthread_mutex_unlock(l);\n"
        " #endif\n"
        " }\n"
        " static void zx_cv_bcast(ZxCv *c) {\n"
        " #ifdef _WIN32\n"
        "     WakeAllConditionVariable(c);\n"
        " #else\n"
        "     pthread_cond_broadcast(c);\n"
        " #endif\n"
        " }\n"
        " static void zx_cv_wait(ZxCv *c, ZxLk *l) {\n"
        " #ifdef _WIN32\n"
        "     SleepConditionVariableSRW(c, l, 5, 0);\n"
        " #else\n"
        "     struct timespec ts;\n"
        "     clock_gettime(CLOCK_REALTIME, &ts);\n"
        "     ts.tv_nsec += 5000000L;\n"
        "     if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }\n"
        "     pthread_cond_timedwait(c, l, &ts);\n"
        " #endif\n"
        " }\n"
        " \n"
        " /* ===== 调度层（策略）=====\n"
        "  * 从这里往下全部是纯策略：实例表、就绪队列、结果槽、等待链、timer、\n"
        "  * Scope 记账、select 轮询。它们只通过上面的 zx_rt_* 原语碰平台。\n"
        "  * 对外入口 zx_task_* / zx_sc_* / zx_ch_* / zx_sel_* 由 std/task.zx\n"
        "  * 的私有 extern 声明调用；用户只看得到 Task 的方法。 */\n"
        " static void zx_tab_init(void) {\n"
        "     if (!zx_sched_tab) {\n"
        "         zx_sched_cap = 8;\n"
        "         zx_sched_tab = (ZxSched **)calloc(zx_sched_cap, sizeof(ZxSched *));\n"
        "     }\n"
        " }\n"
        " static ZxSched *zx_sched_get(uint64_t h) {\n"
        "     if (!h) return NULL;\n"
        "     zx_tab_init();\n"
        "     ZxSched *s = (h <= (uint64_t)zx_nsched) ? zx_sched_tab[h - 1] : NULL;\n"
        "     if (s && __atomic_load_n(&s->stop, __ATOMIC_SEQ_CST)) return NULL;\n"
        "     return s;\n"
        " }\n"
        " static ZxSched *zx_sched_raw(uint64_t h) {\n"
        "     if (!h) return NULL;\n"
        "     zx_tab_init();\n"
        "     ZxSched *s = (h <= (uint64_t)zx_nsched) ? zx_sched_tab[h - 1] : NULL;\n"
        "     return s;\n"
        " }\n"
        " /* id 是 1-based 的单调序号；段表按 (id-1)/ZX_SEG_SIZE 定位，\n"
        "  * 段只在第一次用到时 calloc，指针用原子发布，worker 读侧无锁。 */\n"
        " static uint32_t zx_seg_key(uint64_t id) { return (uint32_t)((id - 1) % ZX_SEG_SIZE); }\n"
        " static ZxSeg *zx_seg_get(ZxSched *s, uint64_t id) {\n"
        "     if (!id) return NULL;\n"
        "     uint64_t si = (id - 1) / ZX_SEG_SIZE;\n"
        "     if (si >= ZX_MAX_SEGS) return NULL;\n"
        "     return (ZxSeg *)__atomic_load_n(&s->segs[si], __ATOMIC_SEQ_CST);\n"
        " }\n"
        " static ZxSlot *zx_slot_get(ZxSched *s, uint64_t id) {\n"
        "     ZxSeg *seg = zx_seg_get(s, id);\n"
        "     return seg ? &seg->slot[zx_seg_key(id)] : NULL;\n"
        " }\n"
        " static ZxTask *zx_task_get(ZxSched *s, uint64_t id) {\n"
        "     ZxSeg *seg = zx_seg_get(s, id);\n"
        "     if (!seg) return NULL;\n"
        "     return (ZxTask *)__atomic_load_n(&seg->task[zx_seg_key(id)], __ATOMIC_SEQ_CST);\n"
        " }\n"
        " \n"
        " /* 就绪队列：入队后一律 idle_wake（锁内复查 sleeping），睡眠方在 idle\n"
        "  * 锁内复查队列 —— 两边谁先谁后都不丢唤醒（§92 的无窗口协议）。 */\n"
        " static void zx_ready_push(ZxSched *s, ZxTask *t) {\n"
        "     zx_lk(&s->qlk);\n"
        "     t->next = NULL;\n"
        "     if (s->qtail) s->qtail->next = t; else s->qhead = t;\n"
        "     s->qtail = t;\n"
        "     zx_ulk(&s->qlk);\n"
        "     zx_lk(&s->idle_lk);\n"
        "     if (s->sleeping > 0) zx_cv_bcast(&s->idle_cv);\n"
        "     zx_ulk(&s->idle_lk);\n"
        " }\n"
        " static ZxTask *zx_ready_pop(ZxSched *s) {\n"
        "     zx_lk(&s->qlk);\n"
        "     ZxTask *t = s->qhead;\n"
        "     if (t) { s->qhead = t->next; if (!s->qhead) s->qtail = NULL; t->next = NULL; }\n"
        "     zx_ulk(&s->qlk);\n"
        "     return t;\n"
        " }\n"
        " \n"
        " /* 唤醒一个停靠任务：谁抢到「入队权」由 pstate 仲裁。\n"
        "  * PARKED  -> 本方入队；PARKING -> 任务还没切走，留给 run_one 代劳。 */\n"
        " static void zx_unpark(ZxTask *w) {\n"
        "     if (!w) return;\n"
        "     int prev = __atomic_exchange_n(&w->pstate, ZX_WOKEN, __ATOMIC_SEQ_CST);\n"
        "     if (prev == ZX_PARKED) zx_ready_push(w->s, w);\n"
        " }\n"
        " \n"
        " static ZxTask *zx_wait_pop(ZxTask **head) {\n"
        "     ZxTask *t = *head;\n"
        "     if (t) { *head = t->next; t->next = NULL; }\n"
        "     return t;\n"
        " }\n"
        " static void zx_wait_append(ZxTask **head, ZxTask *t) {\n"
        "     t->next = NULL;\n"
        "     if (!*head) { *head = t; return; }\n"
        "     ZxTask *p = *head;\n"
        "     while (p->next) p = p->next;\n"
        "     p->next = t;\n"
        " }\n"
        " \n"
        " static void zx_task_switch_to(ZxTask *from, ZxTask *to) {\n"
        "     zx_fb_cur = to;\n"
        " #ifdef _WIN32\n"
        "     SwitchToFiber(to->h);\n"
        " #else\n"
        "     swapcontext(&from->ctx, &to->ctx);\n"
        " #endif\n"
        " }\n"
        " static ZxTask *zx_task_rootctx(void) {\n"
        "     if (!zx_tk_root.inited) {\n"
        "         zx_tk_root.inited = true;\n"
        "         zx_tk_root.is_root = true;\n"
        " #ifdef _WIN32\n"
        "         zx_tk_root.h = ConvertThreadToFiber(0);\n"
        "         if (!zx_tk_root.h) return 0;\n"
        " #endif\n"
        "     }\n"
        "     return &zx_tk_root;\n"
        " }\n"
        " \n"
        " void *zx_cap_current(void) {\n"
        "     return (zx_fb_cur && !zx_fb_cur->is_root) ? (void *)&zx_fb_cur->cap : (void *)&zx_cap_root;\n"
        " }\n"
        " void zx_cap_push(unsigned code, const char *file, unsigned line, unsigned col, const char *fn) {\n"
        "     ZxCap *c = (ZxCap *)zx_cap_current();\n"
        "     ZxCapRec *r;\n"
        "     if (c->n < ZX_CAP_MAX) { r = &c->recs[(c->start + c->n) % ZX_CAP_MAX]; c->n++; }\n"
        "     else { r = &c->recs[c->start]; c->start = (c->start + 1) % ZX_CAP_MAX; }\n"
        "     r->code = (uint16_t)code; r->file = file; r->line = line; r->col = col; r->fn = fn;\n"
        " }\n"
        " \n"
        " bool zx_task_await_gate(void) {\n"
        "     ZxTask *c = zx_fb_cur;\n"
        "     if (!c || c->is_root) return false;\n"
        "     if (!c->done && c->back) {\n"
        "         c->requeue = true;\n"
        "         zx_task_switch_to(c, c->back);\n"
        "     }\n"
        "     return __atomic_load_n(&c->cancelled, __ATOMIC_SEQ_CST);\n"
        " }\n"
        " \n"
        " /* 循环回边专用：只读取消旗标，绝不挂起。await 门只在 !T 函数入口，\n"
        "  * 一个不调用任何 !T 函数的计算密集循环会一直跑到完，cancel 石沉大海；\n"
        "  * 后端在 !T 函数的循环体开头插这一句，取消就能在下一轮迭代生效。 */\n"
        " bool zx_task_cancel_check(void) {\n"
        "     ZxTask *c = zx_fb_cur;\n"
        "     if (!c || c->is_root) return false;\n"
        "     return __atomic_load_n(&c->cancelled, __ATOMIC_SEQ_CST);\n"
        " }\n"
        " \n"
        " static void zx_task_finish(ZxTask *self, uint64_t ret, uint16_t tag) {\n"
        "     __atomic_store_n(&self->ret, ret, __ATOMIC_SEQ_CST);\n"
        "     __atomic_store_n(&self->tag, tag, __ATOMIC_SEQ_CST);\n"
        "     ZxSched *s = self->s;\n"
        "     ZxSlot *sl = zx_slot_get(s, self->id);\n"
        "     if (sl) {\n"
        "         __atomic_store_n(&sl->ret, ret, __ATOMIC_SEQ_CST);\n"
        "         __atomic_store_n(&sl->tag, tag, __ATOMIC_SEQ_CST);\n"
        "         __atomic_store_n(&sl->done_ms, zx_now_ms(), __ATOMIC_SEQ_CST);\n"
        "     }\n"
        "     /* done 必须在 scope 段之后才发布：await/close 看到 done 时，\n"
        "      * finish 里对 sc 的访问必须已经结束，否则 close 立刻 free(sc) 会 UAF。 */\n"
        "     /* 首个错误立刻触发 fail-fast：取消同作用域里还在跑的兄弟。\n"
        "      * 只记第一个，后续错误不覆盖 —— join 回放的就是它。 */\n"
        "     ZxScope *sc = self->sc;\n"
        "     bool first_fail = false;\n"
        "     if (sc) {\n"
        "         zx_lk(&sc->lk);\n"
        "         if (tag != 0 && !sc->failed) {\n"
        "             sc->failed = true;\n"
        "             sc->first_tag = tag;\n"
        "             sc->first_ret = ret;\n"
        "             first_fail = true;\n"
        "         }\n"
        "         zx_ulk(&sc->lk);\n"
        "     }\n"
        "     if (first_fail) zx_sc_cancel_all(sc);\n"
        "     if (sl) __atomic_store_n(&sl->done, true, __ATOMIC_SEQ_CST);\n"
        "     __atomic_store_n(&self->done, true, __ATOMIC_SEQ_CST);\n"
        "     zx_lk(&s->wait_lk);\n"
        "     ZxTask *w = self->waiters; self->waiters = NULL;\n"
        "     s->pending--;\n"
        "     ZxTask *all = NULL;\n"
        "     if (s->pending <= 0) { all = s->all_waiters; s->all_waiters = NULL; }\n"
        "     zx_ulk(&s->wait_lk);\n"
        "     while (w) { ZxTask *n = w->next; w->next = NULL; zx_unpark(w); w = n; }\n"
        "     while (all) { ZxTask *n = all->next; all->next = NULL; zx_unpark(all); all = n; }\n"
        "     zx_lk(&s->idle_lk);\n"
        "     if (s->sleeping > 0) zx_cv_bcast(&s->idle_cv);\n"
        "     zx_ulk(&s->idle_lk);\n"
        " }\n"
        " \n"
        " static void zx_task_main(void *p) {\n"
        "     ZxTask *self = (ZxTask *)p;\n"
        "     ZX_ERRU r = self->fn(self->arg);\n"
        "     zx_task_finish(self, r.payload, r.tag);\n"
        "     zx_task_switch_to(self, self->back);\n"
        " }\n"
        " \n"
        " /* 原语层续：纤程栈的创建与回收。调度层只看到「造一个能跑任务函数\n"
        "  * 的纤程」和「回收它」，CreateFiber / getcontext / makecontext 不再\n"
        "  * 散落在 spawn / run_one 的策略代码里。 */\n"
        " static bool zx_rt_fiber_make(ZxTask *t) {\n"
        " #ifdef _WIN32\n"
        "     t->h = CreateFiber(ZX_TASK_STACK, zx_task_main, t);\n"
        "     return t->h != NULL;\n"
        " #else\n"
        "     if (getcontext(&t->ctx) != 0) return false;\n"
        "     t->stk = (char *)malloc(ZX_TASK_STACK);\n"
        "     if (!t->stk) return false;\n"
        "     t->ctx.uc_stack.ss_sp = t->stk;\n"
        "     t->ctx.uc_stack.ss_size = ZX_TASK_STACK;\n"
        "     t->ctx.uc_link = 0;\n"
        "     makecontext(&t->ctx, (void (*)(void))zx_task_main, 1, t);\n"
        "     return true;\n"
        " #endif\n"
        " }\n"
        " static void zx_rt_fiber_drop(ZxTask *t) {\n"
        " #ifdef _WIN32\n"
        "     DeleteFiber(t->h);\n"
        " #else\n"
        "     free(t->stk); t->stk = NULL;\n"
        " #endif\n"
        " }\n"
        " \n"
        " /* 所有权规则（§92）：切回后按「为什么切回」一次性判定，判定后立即收手。\n"
        "  * 停靠再加一层 pstate 仲裁，杜绝「挂链后还没切走就被别人入队」。 */\n"
        " static void zx_task_run_one(ZxTask *t, ZxTask *root) {\n"
        "     if (t->cancelled && !t->started) {\n"
        "         t->started = true;\n"
        "         zx_task_finish(t, 0, ZX_ERR_CANCELLED);\n"
        "     } else {\n"
        "         t->started = true;\n"
        "         t->back = root;\n"
        "         __atomic_store_n(&t->pstate, ZX_RUN, __ATOMIC_SEQ_CST);\n"
        "         zx_task_switch_to(root, t);\n"
        "         if (t->requeue) {\n"
        "             t->requeue = false;\n"
        "             zx_ready_push(t->s, t);\n"
        "             return;\n"
        "         }\n"
        "         if (__atomic_load_n(&t->pstate, __ATOMIC_SEQ_CST) != ZX_RUN) {\n"
        "             int prev = __atomic_exchange_n(&t->pstate, ZX_PARKED, __ATOMIC_SEQ_CST);\n"
        "             if (prev == ZX_WOKEN) zx_ready_push(t->s, t);\n"
        "             return;\n"
        "         }\n"
        "         if (!__atomic_load_n(&t->done, __ATOMIC_SEQ_CST)) return;\n"
        "     }\n"
        "     if (!__atomic_exchange_n(&t->deleted, (char)1, __ATOMIC_SEQ_CST)) {\n"
        "         zx_rt_fiber_drop(t);\n"
        "         ZxSched *s = t->s;\n"
        "         ZxSeg *sg = zx_seg_get(s, t->id);\n"
        "         if (sg) __atomic_store_n(&sg->task[zx_seg_key(t->id)], NULL, __ATOMIC_SEQ_CST);\n"
        "         free(t);\n"
        "     }\n"
        " }\n"
        " \n"
        " /* ===== 定时器：无后台线程，靠调度器心跳轮询 =====\n"
        "  * 新等待 API（sleep / awaitTimeout / select）统一走「轮询 + 停靠一个\n"
        "  * tick」：停靠任务只挂在 timer 链上，不挂任何等待者链，所以只有 timer\n"
        "  * 一个唤醒源，不会有多路 select 常见的「两个来源同时 unpark」竞态。\n"
        "  * close 先把链整体摘空再 unpark，与 poll 在 tm_lk 下互斥，仍是单源。 */\n"
        " static void zx_tm_arm(ZxSched *s, ZxTask *t, uint64_t ms) {\n"
        "     uint64_t now = zx_now_ms(), max = ~(uint64_t)0;\n"
        "     t->deadline = (ms > max - now) ? max : now + ms;\n"
        "     t->tm_fired = false;\n"
        "     t->timed = true;\n"
        "     zx_lk(&s->tm_lk);\n"
        "     t->tm_next = s->timers; s->timers = t;\n"
        "     zx_ulk(&s->tm_lk);\n"
        " }\n"
        " static void zx_tm_disarm(ZxSched *s, ZxTask *t) {\n"
        "     zx_lk(&s->tm_lk);\n"
        "     ZxTask **pp = &s->timers;\n"
        "     while (*pp) {\n"
        "         if (*pp == t) { *pp = t->tm_next; break; }\n"
        "         pp = &(*pp)->tm_next;\n"
        "     }\n"
        "     t->tm_next = NULL; t->timed = false;\n"
        "     zx_ulk(&s->tm_lk);\n"
        " }\n"
        " static void zx_tm_poll(ZxSched *s) {\n"
        "     uint64_t now = zx_now_ms();\n"
        "     bool stopped = __atomic_load_n(&s->stop, __ATOMIC_SEQ_CST);\n"
        "     ZxTask *fire = NULL;\n"
        "     zx_lk(&s->tm_lk);\n"
        "     ZxTask **pp = &s->timers;\n"
        "     while (*pp) {\n"
        "         ZxTask *t = *pp;\n"
        "         if (stopped || now >= t->deadline ||\n"
        "             __atomic_load_n(&t->cancelled, __ATOMIC_SEQ_CST)) {\n"
        "             *pp = t->tm_next;\n"
        "             t->tm_next = NULL; t->timed = false; t->tm_fired = true;\n"
        "             t->next = fire; fire = t;   /* 借用 next 串待唤醒链 */\n"
        "         } else pp = &t->tm_next;\n"
        "     }\n"
        "     zx_ulk(&s->tm_lk);\n"
        "     while (fire) { ZxTask *n = fire->next; fire->next = NULL; zx_unpark(fire); fire = n; }\n"
        " }\n"
        " \n"
        " /* 停靠一个 tick（>=1ms）。任务上下文挂 timer 后切走；根上下文推进本\n"
        "  * 实例的就绪队列直到到点（coro 模式没有 worker，必须自己 drive）。\n"
        "  * 返回 0 正常 / 1 实例已关闭 / 2 当前任务已取消。 */\n"
        " static int zx_park_tick(ZxSched *s, uint64_t ms) {\n"
        "     if (ms == 0) ms = 1;\n"
        "     if (__atomic_load_n(&s->stop, __ATOMIC_SEQ_CST)) return 1;\n"
        "     ZxTask *self = zx_fb_cur;\n"
        "     if (self && !self->is_root &&\n"
        "         __atomic_load_n(&self->cancelled, __ATOMIC_SEQ_CST)) return 2;\n"
        "     if (!self || self->is_root || !self->back) {\n"
        "         uint64_t end = zx_now_ms() + ms;\n"
        "         ZxTask *root = zx_task_rootctx();\n"
        "         if (!root) return 1;\n"
        "         for (;;) {\n"
        "             zx_tm_poll(s);\n"
        "             if (zx_now_ms() >= end) return 0;\n"
        "             if (__atomic_load_n(&s->stop, __ATOMIC_SEQ_CST)) return 1;\n"
        "             zx_lk(&s->idle_lk);\n"
        "             ZxTask *w = zx_ready_pop(s);\n"
        "             if (!w) { s->sleeping++; zx_cv_wait(&s->idle_cv, &s->idle_lk); s->sleeping--; }\n"
        "             zx_ulk(&s->idle_lk);\n"
        "             if (w) zx_task_run_one(w, root);\n"
        "         }\n"
        "     }\n"
        "     zx_tm_poll(s);\n"
        "     /* pstate 必须先于 arm：poll 看到这个任务时它已经处于停靠态，\n"
        "      * 否则 unpark 会在 PARKING 之前发生而被吞掉（永久挂起）。 */\n"
        "     __atomic_store_n(&self->pstate, ZX_PARKING, __ATOMIC_SEQ_CST);\n"
        "     zx_tm_arm(s, self, ms);\n"
        "     self->requeue = false;\n"
        "     zx_task_switch_to(self, self->back);\n"
        "     zx_tm_disarm(s, self);\n"
        "     if (__atomic_load_n(&s->stop, __ATOMIC_SEQ_CST)) return 1;\n"
        "     if (__atomic_load_n(&self->cancelled, __ATOMIC_SEQ_CST)) return 2;\n"
        "     return 0;\n"
        " }\n"
        " \n"
        " static void zx_task_worker_loop(ZxSched *s) {\n"
        "     ZxTask *root = zx_task_rootctx();\n"
        "     if (!root) return;\n"
        "     for (;;) {\n"
        "         zx_tm_poll(s);            /* 定时器心跳：cv_wait 5ms 唤醒一次 */\n"
        "         zx_lk(&s->idle_lk);\n"
        "         ZxTask *t = zx_ready_pop(s);\n"
        "         bool stop = __atomic_load_n(&s->stop, __ATOMIC_SEQ_CST);\n"
        "         if (!t && stop) {\n"
        "             __atomic_sub_fetch(&s->alive_workers, 1, __ATOMIC_SEQ_CST);\n"
        "             zx_cv_bcast(&s->idle_cv);\n"
        "             zx_ulk(&s->idle_lk);\n"
        "             return;\n"
        "         }\n"
        "         if (!t) {\n"
        "             s->sleeping++;\n"
        "             zx_cv_wait(&s->idle_cv, &s->idle_lk);\n"
        "             s->sleeping--;\n"
        "         }\n"
        "         zx_ulk(&s->idle_lk);\n"
        "         if (t) zx_task_run_one(t, root);\n"
        "     }\n"
        " }\n"
        " #ifdef _WIN32\n"
        " static uint64_t zx_task_worker_tramp(void *p) { zx_task_worker_loop((ZxSched *)p); return 0; }\n"
        " #else\n"
        " static void *zx_task_worker_tramp(void *p) { zx_task_worker_loop((ZxSched *)p); return NULL; }\n"
        " #endif\n"
        " \n"
        " /* ---- 实例构造：三种物理形态 ---- */\n"
        " uint64_t zx_task_new(uint64_t mode, uint64_t n) {\n"
        "     zx_tab_init();\n"
        "     ZxSched *s = (ZxSched *)calloc(1, sizeof(ZxSched));\n"
        "     if (!s || !zx_sched_tab) return 0;\n"
        "     s->mode = (int)mode;\n"
        "     zx_lk_init(&s->qlk); zx_lk_init(&s->idle_lk);\n"
        "     zx_cv_init(&s->idle_cv);\n"
        "     zx_lk_init(&s->wait_lk); zx_lk_init(&s->chan_lk);\n"
        "     zx_lk_init(&s->tab_lk); zx_lk_init(&s->tm_lk);\n"
        "     if (zx_nsched >= zx_sched_cap) {\n"
        "         int nc = zx_sched_cap * 2;\n"
        "         ZxSched **nt = (ZxSched **)realloc(zx_sched_tab, nc * sizeof(ZxSched *));\n"
        "         if (nt) {\n"
        "             for (int i = zx_sched_cap; i < nc; i++) nt[i] = NULL;\n"
        "             zx_sched_tab = nt; zx_sched_cap = nc;\n"
        "         }\n"
        "     }\n"
        "     if (zx_nsched >= zx_sched_cap) { return 0; }\n"
        "     zx_sched_tab[zx_nsched] = s;\n"
        "     s->id = (uint64_t)(++zx_nsched);\n"
        " \n"
        "     int nw = 0;\n"
        "     if (mode == 1) nw = 1;\n"
        "     else if (mode == 2) {\n"
        "         if (n) nw = (int)n;\n"
        "         else { int nc = zx_rt_cpu_count(); nw = nc > 1 ? nc - 1 : 0; }\n"
        "     }\n"
        "     s->nworkers = nw;\n"
        "     s->alive_workers = nw;\n"
        "     for (int i = 0; i < nw; i++) zx_rt_thread_spawn(zx_task_worker_tramp, s);\n"
        "     return s->id;\n"
        " }\n"
        " \n"
        " /* sc 非空时把任务登记进作用域。登记必须在 zx_ready_push 之前完成：\n"
        "  * 一旦入队，worker 可能立刻跑完并 free(t)，此后再碰 t 就是 UAF。\n"
        "  * 扩容也放在 tab_lk 内，失败只留一个不对外发布的 id 空洞，回滚干净。 */\n"
        " static ZX_OPTU zx_task_spawn_impl(uint64_t h, ZX_TASKFN f, uint64_t ctx,\n"
        "                                  ZxScope *sc) {\n"
        "     ZX_OPTU r; r.payload = 0; r.tag = false;\n"
        "     ZxSched *s = zx_sched_get(h);\n"
        "     if (!s || __atomic_load_n(&s->stop, __ATOMIC_SEQ_CST)) return r;\n"
        "     ZxTask *root = zx_task_rootctx();\n"
        "     if (!root) return r;\n"
        "     zx_lk(&s->tab_lk);\n"
        "     uint64_t id = ++s->nids;\n"
        "     uint64_t si = (id - 1) / ZX_SEG_SIZE;\n"
        "     if (si >= ZX_MAX_SEGS) { zx_ulk(&s->tab_lk); return r; }\n"
        "     if (!s->segs[si]) {\n"
        "         ZxSeg *sg = (ZxSeg *)calloc(1, sizeof(ZxSeg));\n"
        "         if (!sg) { zx_ulk(&s->tab_lk); return r; }\n"
        "         __atomic_store_n(&s->segs[si], sg, __ATOMIC_SEQ_CST);\n"
        "     }\n"
        "     ZxSeg *seg = s->segs[si];\n"
        "     uint32_t k = (uint32_t)((id - 1) % ZX_SEG_SIZE);\n"
        "     __atomic_store_n(&seg->slot[k].done, false, __ATOMIC_SEQ_CST);\n"
        "     __atomic_store_n(&seg->slot[k].ret, 0, __ATOMIC_SEQ_CST);\n"
        "     __atomic_store_n(&seg->slot[k].tag, 0, __ATOMIC_SEQ_CST);\n"
        "     __atomic_store_n(&seg->slot[k].done_ms, 0, __ATOMIC_SEQ_CST);\n"
        "     bool doomed = false;\n"
        "     if (sc) {\n"
        "         zx_lk(&sc->lk);\n"
        "         if (sc->closed) { zx_ulk(&sc->lk); zx_ulk(&s->tab_lk); return r; }\n"
        "         if (sc->n == sc->cap) {\n"
        "             int nc = sc->cap ? sc->cap * 2 : 16;\n"
        "             uint64_t *ni = (uint64_t *)realloc(sc->ids, (size_t)nc * sizeof(uint64_t));\n"
        "             if (ni) { sc->ids = ni; sc->cap = nc; }\n"
        "         }\n"
        "         if (sc->n >= sc->cap) { zx_ulk(&sc->lk); zx_ulk(&s->tab_lk); return r; }\n"
        "         sc->ids[sc->n++] = id;\n"
        "         doomed = sc->failed;   /* 兄弟已失败：新成员一律直接判 cancelled */\n"
        "         zx_ulk(&sc->lk);\n"
        "     }\n"
        "     zx_ulk(&s->tab_lk);\n"
        "     ZxTask *t = (ZxTask *)calloc(1, sizeof(ZxTask));\n"
        "     if (!t) return r;\n"
        "     t->fn = f; t->arg = ctx; t->s = s;\n"
        "     t->id = id;\n"
        "     t->sc = sc;\n"
        "     t->cancelled = doomed;\n"
        "     t->pstate = ZX_RUN;\n"
        "     if (!zx_rt_fiber_make(t)) { free(t); return r; }\n"
        "     zx_lk(&s->wait_lk); s->pending++; zx_ulk(&s->wait_lk);\n"
        "     /* 入队前复查一次：登记到入队之间若兄弟已失败 / 作用域已关闭，\n"
        "      * cancel_all 遍历时这个 t 还没发布（拿不到），必须在这里补判。 */\n"
        "     if (sc) {\n"
        "         zx_lk(&sc->lk);\n"
        "         if (sc->failed || sc->closed) t->cancelled = true;\n"
        "         zx_ulk(&sc->lk);\n"
        "     }\n"
        "     __atomic_store_n(&seg->task[k], t, __ATOMIC_SEQ_CST);\n"
        "     zx_ready_push(s, t);\n"
        "     r.payload = t->id; r.tag = true;\n"
        "     return r;\n"
        " }\n"
        " \n"
        " ZX_OPTU zx_task_spawn(uint64_t h, ZX_TASKFN f, uint64_t ctx) {\n"
        "     return zx_task_spawn_impl(h, f, ctx, NULL);\n"
        " }\n"
        " \n"
        " ZX_ERRU zx_task_await(uint64_t h, uint64_t id) {\n"
        "     ZX_ERRU r; r.payload = 0; r.tag = ZX_ERR_CANCELLED;\n"
        "     ZxSched *s = zx_sched_get(h);\n"
        "     if (!s) { r.tag = ZX_ERR_CLOSED; return r; }\n"
        "     ZxSlot *sl = zx_slot_get(s, id);\n"
        "     if (!sl) return r;                 /* 陈旧句柄：槽位已被复用 */\n"
        "     if (__atomic_load_n(&sl->done, __ATOMIC_SEQ_CST)) { r.payload = sl->ret; r.tag = sl->tag; return r; }\n"
        "     ZxTask *t = zx_task_get(s, id);\n"
        "     if (!t) return r;\n"
        "     ZxTask *self = zx_fb_cur;\n"
        "     if (!self || self->is_root) {\n"
        "         ZxTask *root = zx_task_rootctx();\n"
        "         if (!root) return r;\n"
        "         int idle = 0;\n"
        "         for (;;) {\n"
        "             zx_tm_poll(s);\n"
        "             zx_lk(&s->idle_lk);\n"
        "             if (__atomic_load_n(&sl->done, __ATOMIC_SEQ_CST)) { zx_ulk(&s->idle_lk); break; }\n"
        "             if (__atomic_load_n(&s->stop, __ATOMIC_SEQ_CST)) { zx_ulk(&s->idle_lk); r.tag = ZX_ERR_CLOSED; return r; }\n"
        "             ZxTask *w = zx_ready_pop(s);\n"
        "             if (!w) { s->sleeping++; zx_cv_wait(&s->idle_cv, &s->idle_lk); s->sleeping--; idle++; }\n"
        "             else idle = 0;\n"
        "             zx_ulk(&s->idle_lk);\n"
        "             if (w) zx_task_run_one(w, root);\n"
        "             if (idle > 200 && s->nworkers == 0) { r.tag = ZX_ERR_WOULDBLOCK; return r; }\n"
        "         }\n"
        "         r.payload = sl->ret; r.tag = sl->tag;\n"
        "         return r;\n"
        "     }\n"
        "     zx_lk(&s->wait_lk);\n"
        "     if (__atomic_load_n(&sl->done, __ATOMIC_SEQ_CST)) {\n"
        "         zx_ulk(&s->wait_lk); r.payload = sl->ret; r.tag = sl->tag; return r;\n"
        "     }\n"
        "     self->next = t->waiters; t->waiters = self;\n"
        "     __atomic_store_n(&self->pstate, ZX_PARKING, __ATOMIC_SEQ_CST);\n"
        "     zx_ulk(&s->wait_lk);\n"
        "     self->requeue = false;\n"
        "     zx_task_switch_to(self, self->back);\n"
        "     r.payload = sl->ret; r.tag = sl->tag;\n"
        "     return r;\n"
        " }\n"
        " \n"
        " /* ===== 超时等待：sleep / awaitTimeout / joinTimeout =====\n"
        "  * 全部基于 zx_park_tick 轮询，不占用 worker，也不往等待者链上挂东西\n"
        "  * —— 超时后不会留下「已恢复但仍在链上」的幽灵任务（§92）。精度是\n"
        "  * 调度器心跳级（毫秒级），不是硬实时；ms=0 表示立即探测一次。 */\n"
        " static uint64_t zx_deadline(uint64_t ms) {\n"
        "     uint64_t now = zx_now_ms(), max = ~(uint64_t)0;\n"
        "     return (ms > max - now) ? max : now + ms;\n"
        " }\n"
        " ZX_ERRV zx_task_sleep(uint64_t h, uint64_t ms) {\n"
        "     ZX_ERRV r; r.tag = 0;\n"
        "     ZxSched *s = zx_sched_get(h);\n"
        "     if (!s) { r.tag = ZX_ERR_CLOSED; return r; }\n"
        "     uint64_t end = zx_deadline(ms);\n"
        "     for (;;) {\n"
        "         if (zx_now_ms() >= end) return r;\n"
        "         int st = zx_park_tick(s, 1);\n"
        "         if (st == 1) { r.tag = ZX_ERR_CLOSED; return r; }\n"
        "         if (st == 2) { r.tag = ZX_ERR_CANCELLED; return r; }\n"
        "     }\n"
        " }\n"
        " \n"
        " ZX_ERRU zx_task_await_timeout(uint64_t h, uint64_t id, uint64_t ms) {\n"
        "     ZX_ERRU r; r.payload = 0; r.tag = ZX_ERR_TIMEOUT;\n"
        "     ZxSched *s = zx_sched_get(h);\n"
        "     if (!s) { r.tag = ZX_ERR_CLOSED; return r; }\n"
        "     ZxSlot *sl = zx_slot_get(s, id);\n"
        "     if (!sl) { r.tag = ZX_ERR_CANCELLED; return r; }\n"
        "     uint64_t end = zx_deadline(ms);\n"
        "     for (;;) {\n"
        "         if (__atomic_load_n(&sl->done, __ATOMIC_SEQ_CST)) {\n"
        "             /* 结算时刻晚于 deadline 仍算超时：轮询可能在一个 tick\n"
        "              * 里把任务整个跑完，回来时「已完成」不等于「按时完成」。 */\n"
        "             if (__atomic_load_n(&sl->done_ms, __ATOMIC_SEQ_CST) > end)\n"
        "                 { r.tag = ZX_ERR_TIMEOUT; return r; }\n"
        "             r.payload = __atomic_load_n(&sl->ret, __ATOMIC_SEQ_CST);\n"
        "             r.tag = __atomic_load_n(&sl->tag, __ATOMIC_SEQ_CST);\n"
        "             return r;\n"
        "         }\n"
        "         if (zx_now_ms() >= end) return r;\n"
        "         int st = zx_park_tick(s, 1);\n"
        "         if (st == 1) { r.tag = ZX_ERR_CLOSED; return r; }\n"
        "         if (st == 2) { r.tag = ZX_ERR_CANCELLED; return r; }\n"
        "     }\n"
        " }\n"
        " \n"
        " /* joinTimeout：等全体成员，deadline 到点返回 timeout 并 cancel 全体\n"
        "  * （deadline 语义 = 不能留活任务）。调用方仍可用 await(h) 取已完成的\n"
        "  * 结果，defer sc.close() 照常兜底。 */\n"
        " ZX_ERRU zx_sc_join_timeout(uint64_t sch, uint64_t ms) {\n"
        "     ZX_ERRU r; r.payload = 0; r.tag = 0;\n"
        "     ZxScope *sc = (ZxScope *)(uintptr_t)sch;\n"
        "     if (!sc) { r.tag = ZX_ERR_CLOSED; return r; }\n"
        "     uint64_t h = sc->s->id;\n"
        "     if (!zx_sched_get(h)) { r.tag = ZX_ERR_CLOSED; return r; }\n"
        "     zx_lk(&sc->lk);\n"
        "     if (sc->closed) { zx_ulk(&sc->lk); r.tag = ZX_ERR_CLOSED; return r; }\n"
        "     int n = sc->n;\n"
        "     uint64_t *ids = (uint64_t *)malloc((size_t)(n ? n : 1) * sizeof(uint64_t));\n"
        "     if (ids) { for (int i = 0; i < n; i++) ids[i] = sc->ids[i]; }\n"
        "     zx_ulk(&sc->lk);\n"
        "     if (!ids) { r.tag = ZX_ERR_CLOSED; return r; }\n"
        "     uint64_t end = zx_deadline(ms);\n"
        "     int outcome = 0;                /* 0 全体完成 / 1 超时 / 2 关闭 / 3 取消 */\n"
        "     for (;;) {\n"
        "         int left = 0;\n"
        "         bool late = false;\n"
        "         for (int i = 0; i < n; i++) {\n"
        "             ZxSlot *sl = zx_slot_get(sc->s, ids[i]);\n"
        "             if (!sl || !__atomic_load_n(&sl->done, __ATOMIC_SEQ_CST)) left++;\n"
        "             else if (__atomic_load_n(&sl->done_ms, __ATOMIC_SEQ_CST) > end) late = true;\n"
        "         }\n"
        "         /* 全体完成但有人超了 deadline，仍按超时处理并 cancel 兜底。 */\n"
        "         if (left == 0) { outcome = late ? 1 : 0; break; }\n"
        "         if (zx_now_ms() >= end) { outcome = 1; break; }\n"
        "         int st = zx_park_tick(sc->s, 1);\n"
        "         if (st == 1) { outcome = 2; break; }\n"
        "         if (st == 2) { outcome = 3; break; }\n"
        "     }\n"
        "     if (outcome == 1) zx_sc_cancel_all(sc);\n"
        "     if (outcome == 2) r.tag = ZX_ERR_CLOSED;\n"
        "     else if (outcome == 3) r.tag = ZX_ERR_CANCELLED;\n"
        "     else if (outcome == 1) r.tag = ZX_ERR_TIMEOUT;\n"
        "     else {\n"
        "         zx_lk(&sc->lk);\n"
        "         r.tag = sc->failed ? sc->first_tag : 0;\n"
        "         r.payload = sc->failed ? sc->first_ret : 0;\n"
        "         uint64_t first = sc->n ? sc->ids[0] : 0;\n"
        "         zx_ulk(&sc->lk);\n"
        "         if (!r.tag && first) {\n"
        "             ZxSlot *sl = zx_slot_get(sc->s, first);\n"
        "             if (sl) r.payload = __atomic_load_n(&sl->ret, __ATOMIC_SEQ_CST);\n"
        "         }\n"
        "     }\n"
        "     free(ids);\n"
        "     return r;\n"
        " }\n"
        " \n"
        " /* awaitAll：等本实例 pending 归零。纯屏障，结果仍用 await(h) 取。 */\n"
        " ZX_ERRV zx_task_await_all(uint64_t h) {\n"
        "     ZX_ERRV r; r.tag = 0;\n"
        "     ZxSched *s = zx_sched_get(h);\n"
        "     if (!s) { r.tag = ZX_ERR_CLOSED; return r; }\n"
        "     ZxTask *self = zx_fb_cur;\n"
        "     if (!self || self->is_root) {\n"
        "         ZxTask *root = zx_task_rootctx();\n"
        "         if (!root) { r.tag = ZX_ERR_CANCELLED; return r; }\n"
        "         int idle = 0;\n"
        "         for (;;) {\n"
        "             zx_tm_poll(s);\n"
        "             zx_lk(&s->idle_lk);\n"
        "             zx_lk(&s->wait_lk);\n"
        "             int left = s->pending;\n"
        "             zx_ulk(&s->wait_lk);\n"
        "             if (left <= 0) { zx_ulk(&s->idle_lk); return r; }\n"
        "             if (__atomic_load_n(&s->stop, __ATOMIC_SEQ_CST)) { zx_ulk(&s->idle_lk); r.tag = ZX_ERR_CLOSED; return r; }\n"
        "             ZxTask *w = zx_ready_pop(s);\n"
        "             if (!w) { s->sleeping++; zx_cv_wait(&s->idle_cv, &s->idle_lk); s->sleeping--; idle++; }\n"
        "             else idle = 0;\n"
        "             zx_ulk(&s->idle_lk);\n"
        "             if (w) zx_task_run_one(w, root);\n"
        "             if (idle > 200 && s->nworkers == 0) { r.tag = ZX_ERR_WOULDBLOCK; return r; }\n"
        "         }\n"
        "     }\n"
        "     zx_lk(&s->wait_lk);\n"
        "     if (s->pending <= 0) { zx_ulk(&s->wait_lk); return r; }\n"
        "     self->next = s->all_waiters; s->all_waiters = self;\n"
        "     __atomic_store_n(&self->pstate, ZX_PARKING, __ATOMIC_SEQ_CST);\n"
        "     zx_ulk(&s->wait_lk);\n"
        "     self->requeue = false;\n"
        "     zx_task_switch_to(self, self->back);\n"
        "     if (__atomic_load_n(&self->cancelled, __ATOMIC_SEQ_CST)) r.tag = ZX_ERR_CANCELLED;\n"
        "     return r;\n"
        " }\n"
        " \n"
        " void zx_task_yield(uint64_t h) {\n"
        "     ZxSched *s = zx_sched_get(h);\n"
        "     if (!s) return;\n"
        "     ZxTask *c = zx_fb_cur;\n"
        "     if (!c || c->is_root || !c->back) return;\n"
        "     c->requeue = true;\n"
        "     zx_task_switch_to(c, c->back);\n"
        " }\n"
        " \n"
        " /* cancel：置旗标 + 提前唤醒正停靠在通信等待链上的任务。\n"
        "  * 拆出 _one 供 Scope 批量取消复用；不得在这里调任何 zx_sc_* 函数\n"
        "  * （scope 的批量取消持 sc->lk，会重入死锁）。 */\n"
        " static void zx_task_cancel_one(ZxSched *s, ZxTask *t) {\n"
        "     __atomic_store_n(&t->cancelled, true, __ATOMIC_SEQ_CST);\n"
        "     zx_lk(&s->chan_lk);\n"
        "     /* park_ch 记着任务停在命名通道还是默认通道；只摘它真正在的\n"
        "      * 那两条链，否则 cancel 一个停在命名通道上的任务会没人唤醒。 */\n"
        "     ZxChan *c = (ZxChan *)__atomic_load_n(&t->park_ch, __ATOMIC_SEQ_CST);\n"
        "     ZxTask **rw = c ? &c->rwait : &s->rwait;\n"
        "     ZxTask **ww = c ? &c->wwait : &s->wwait;\n"
        "     ZxTask **pp = rw; ZxTask *found = NULL;\n"
        "     while (*pp) { if (*pp == t) { found = *pp; *pp = t->next; t->next = NULL; break; } pp = &(*pp)->next; }\n"
        "     if (!found) {\n"
        "         pp = ww;\n"
        "         while (*pp) { if (*pp == t) { found = *pp; *pp = t->next; t->next = NULL; break; } pp = &(*pp)->next; }\n"
        "     }\n"
        "     zx_ulk(&s->chan_lk);\n"
        "     if (found) { found->cok = false; zx_unpark(found); }\n"
        " }\n"
        " void zx_task_cancel(uint64_t h, uint64_t id) {\n"
        "     ZxSched *s = zx_sched_get(h);\n"
        "     if (!s) return;\n"
        "     ZxTask *t = zx_task_get(s, id);\n"
        "     if (!t) return;\n"
        "     zx_task_cancel_one(s, t);\n"
        " }\n"
        " \n"
        " /* ---- 通信：无缓冲 rendezvous ----\n"
        "  * c == NULL 走实例自带的默认通道（t.send/recv），否则走命名通道\n"
        "  * （usize 句柄）。两者共用 chan_lk，行为完全一致；park_ch 记住任务停在\n"
        "  * 哪条通道上，cancel 才能把它从正确的等待链里摘出来。 */\n"
        " static ZxTask **zx_ch_rw(ZxSched *s, ZxChan *c) { return c ? &c->rwait : &s->rwait; }\n"
        " static ZxTask **zx_ch_ww(ZxSched *s, ZxChan *c) { return c ? &c->wwait : &s->wwait; }\n"
        " static bool zx_ch_closed(ZxSched *s, ZxChan *c) {\n"
        "     return c ? __atomic_load_n(&c->closed, __ATOMIC_SEQ_CST)\n"
        "              : __atomic_load_n(&s->closed, __ATOMIC_SEQ_CST);\n"
        " }\n"
        " static ZX_ERRV zx_chan_send(ZxSched *s, ZxChan *c, uint64_t v, bool nopark) {\n"
        "     ZX_ERRV r; r.tag = 0;\n"
        "     ZxTask *self = zx_fb_cur;\n"
        "     bool can_park = !nopark && self && !self->is_root && self->back;\n"
        "     zx_lk(&s->chan_lk);\n"
        "     if (zx_ch_closed(s, c)) { zx_ulk(&s->chan_lk); r.tag = ZX_ERR_CLOSED; return r; }\n"
        "     ZxTask *rv = zx_wait_pop(zx_ch_rw(s, c));\n"
        "     if (rv) {\n"
        "         rv->cval = v; rv->cok = true;\n"
        "         zx_ulk(&s->chan_lk);\n"
        "         zx_unpark(rv);\n"
        "         return r;\n"
        "     }\n"
        "     if (!can_park) { zx_ulk(&s->chan_lk); r.tag = ZX_ERR_WOULDBLOCK; return r; }\n"
        "     self->cval = v; self->cok = true;\n"
        "     __atomic_store_n(&self->park_ch, c, __ATOMIC_SEQ_CST);\n"
        "     zx_wait_append(zx_ch_ww(s, c), self);\n"
        "     __atomic_store_n(&self->pstate, ZX_PARKING, __ATOMIC_SEQ_CST);\n"
        "     zx_ulk(&s->chan_lk);\n"
        "     self->requeue = false;\n"
        "     zx_task_switch_to(self, self->back);\n"
        "     if (__atomic_load_n(&self->cancelled, __ATOMIC_SEQ_CST)) { r.tag = ZX_ERR_CANCELLED; return r; }\n"
        "     if (!self->cok) r.tag = ZX_ERR_CLOSED;\n"
        "     return r;\n"
        " }\n"
        " static ZX_ERRU zx_chan_recv(ZxSched *s, ZxChan *c, bool nopark) {\n"
        "     ZX_ERRU r; r.payload = 0; r.tag = ZX_ERR_CLOSED;\n"
        "     ZxTask *self = zx_fb_cur;\n"
        "     bool can_park = !nopark && self && !self->is_root && self->back;\n"
        "     zx_lk(&s->chan_lk);\n"
        "     ZxTask *w = zx_wait_pop(zx_ch_ww(s, c));\n"
        "     if (w) {\n"
        "         uint64_t v = w->cval;\n"
        "         w->cok = true;\n"
        "         zx_ulk(&s->chan_lk);\n"
        "         zx_unpark(w);\n"
        "         r.payload = v; r.tag = 0;\n"
        "         return r;\n"
        "     }\n"
        "     if (zx_ch_closed(s, c)) { zx_ulk(&s->chan_lk); return r; }\n"
        "     if (!can_park) { zx_ulk(&s->chan_lk); r.tag = ZX_ERR_WOULDBLOCK; return r; }\n"
        "     self->cval = 0; self->cok = true;\n"
        "     __atomic_store_n(&self->park_ch, c, __ATOMIC_SEQ_CST);\n"
        "     zx_wait_append(zx_ch_rw(s, c), self);\n"
        "     __atomic_store_n(&self->pstate, ZX_PARKING, __ATOMIC_SEQ_CST);\n"
        "     zx_ulk(&s->chan_lk);\n"
        "     self->requeue = false;\n"
        "     zx_task_switch_to(self, self->back);\n"
        "     if (__atomic_load_n(&self->cancelled, __ATOMIC_SEQ_CST)) { r.tag = ZX_ERR_CANCELLED; return r; }\n"
        "     if (!self->cok) { r.tag = ZX_ERR_CLOSED; return r; }\n"
        "     r.payload = self->cval; r.tag = 0;\n"
        "     return r;\n"
        " }\n"
        " \n"
        " ZX_ERRV zx_task_send(uint64_t h, uint64_t v) {\n"
        "     ZxSched *s = zx_sched_get(h);\n"
        "     if (!s) { ZX_ERRV r; r.tag = ZX_ERR_CLOSED; return r; }\n"
        "     return zx_chan_send(s, NULL, v, false);\n"
        " }\n"
        " ZX_ERRU zx_task_recv(uint64_t h) {\n"
        "     ZxSched *s = zx_sched_get(h);\n"
        "     if (!s) { ZX_ERRU r; r.payload = 0; r.tag = ZX_ERR_CLOSED; return r; }\n"
        "     return zx_chan_recv(s, NULL, false);\n"
        " }\n"
        " ZX_ERRV zx_task_try_send(uint64_t h, uint64_t v) {\n"
        "     ZxSched *s = zx_sched_get(h);\n"
        "     if (!s) { ZX_ERRV r; r.tag = ZX_ERR_CLOSED; return r; }\n"
        "     return zx_chan_send(s, NULL, v, true);\n"
        " }\n"
        " ZX_ERRU zx_task_try_recv(uint64_t h) {\n"
        "     ZxSched *s = zx_sched_get(h);\n"
        "     if (!s) { ZX_ERRU r; r.payload = 0; r.tag = ZX_ERR_CLOSED; return r; }\n"
        "     return zx_chan_recv(s, NULL, true);\n"
        " }\n"
        " \n"
        " /* 命名通道：句柄就是 ZxChan*，壳挂在 ch_list 上由实例 close 统一\n"
        "  * free，所以 close 之后旧句柄仍可安全返回 closed，不会悬垂。 */\n"
        " uint64_t zx_ch_new(uint64_t h) {\n"
        "     ZxSched *s = zx_sched_get(h);\n"
        "     if (!s) return 0;\n"
        "     ZxChan *c = (ZxChan *)calloc(1, sizeof(ZxChan));\n"
        "     if (!c) return 0;\n"
        "     c->s = s;\n"
        "     zx_lk(&s->tm_lk);\n"
        "     c->next_all = s->ch_list; s->ch_list = c;\n"
        "     zx_ulk(&s->tm_lk);\n"
        "     return (uint64_t)(uintptr_t)c;\n"
        " }\n"
        " /* 封口并摘下全部停靠者，返回链头（不 unpark，交给调用方统一做） */\n"
        " static ZxTask *zx_ch_drain(ZxSched *s, ZxChan *c) {\n"
        "     zx_lk(&s->chan_lk);\n"
        "     __atomic_store_n(&c->closed, true, __ATOMIC_SEQ_CST);\n"
        "     ZxTask *r = c->rwait; c->rwait = NULL;\n"
        "     ZxTask *w = c->wwait; c->wwait = NULL;\n"
        "     zx_ulk(&s->chan_lk);\n"
        "     if (!r) return w;\n"
        "     ZxTask *p = r; while (p->next) p = p->next; p->next = w;\n"
        "     return r;\n"
        " }\n"
        " void zx_ch_close(uint64_t ch) {\n"
        "     ZxChan *c = (ZxChan *)(uintptr_t)ch;\n"
        "     if (!c) return;\n"
        "     ZxTask *list = zx_ch_drain(c->s, c);\n"
        "     while (list) { ZxTask *n = list->next; list->next = NULL; list->cok = false; zx_unpark(list); list = n; }\n"
        " }\n"
        " static ZxSched *zx_ch_sched(uint64_t ch) {\n"
        "     ZxChan *c = (ZxChan *)(uintptr_t)ch;\n"
        "     return c ? zx_sched_get(c->s->id) : NULL;\n"
        " }\n"
        " ZX_ERRV zx_ch_send(uint64_t ch, uint64_t v) {\n"
        "     ZxSched *s = zx_ch_sched(ch);\n"
        "     if (!s) { ZX_ERRV r; r.tag = ZX_ERR_CLOSED; return r; }\n"
        "     return zx_chan_send(s, (ZxChan *)(uintptr_t)ch, v, false);\n"
        " }\n"
        " ZX_ERRU zx_ch_recv(uint64_t ch) {\n"
        "     ZxSched *s = zx_ch_sched(ch);\n"
        "     if (!s) { ZX_ERRU r; r.payload = 0; r.tag = ZX_ERR_CLOSED; return r; }\n"
        "     return zx_chan_recv(s, (ZxChan *)(uintptr_t)ch, false);\n"
        " }\n"
        " ZX_ERRV zx_ch_try_send(uint64_t ch, uint64_t v) {\n"
        "     ZxSched *s = zx_ch_sched(ch);\n"
        "     if (!s) { ZX_ERRV r; r.tag = ZX_ERR_CLOSED; return r; }\n"
        "     return zx_chan_send(s, (ZxChan *)(uintptr_t)ch, v, true);\n"
        " }\n"
        " ZX_ERRU zx_ch_try_recv(uint64_t ch) {\n"
        "     ZxSched *s = zx_ch_sched(ch);\n"
        "     if (!s) { ZX_ERRU r; r.payload = 0; r.tag = ZX_ERR_CLOSED; return r; }\n"
        "     return zx_chan_recv(s, (ZxChan *)(uintptr_t)ch, true);\n"
        " }\n"
        " \n"
        " /* ===== select：多分支首个完成者 =====\n"
        "  * br 每 2 项描述一个分支 [kind, arg]：\n"
        "  *   kind 0 = await 句柄 arg；kind 1 = recv 通道 arg（0 = 默认通道）\n"
        "  * 按数组顺序取第一个就绪分支（同时就绪时索引小者胜，确定性）。\n"
        "  * 不往任何等待者链挂东西：轮询 + zx_park_tick，所以超时/取消都不\n"
        "  * 会留下「已恢复但仍在链上」的幽灵任务。which 写胜者索引，超时=n。 */\n"
        " ZX_ERRU zx_sel_wait(uint64_t h, const uint64_t *br, uint64_t n,\n"
        "                     uint64_t ms, uint64_t *which) {\n"
        "     ZX_ERRU r; r.payload = 0; r.tag = ZX_ERR_TIMEOUT;\n"
        "     if (which) *which = n;\n"
        "     ZxSched *s = zx_sched_get(h);\n"
        "     if (!s) { r.tag = ZX_ERR_CLOSED; return r; }\n"
        "     uint64_t end = zx_deadline(ms);\n"
        "     for (;;) {\n"
        "         for (uint64_t i = 0; i < n; i++) {\n"
        "             uint64_t kind = br[2 * i], arg = br[2 * i + 1];\n"
        "             if (kind == 0) {\n"
        "                 ZxSlot *sl = zx_slot_get(s, arg);\n"
        "                 if (sl && __atomic_load_n(&sl->done, __ATOMIC_SEQ_CST)) {\n"
        "                     if (which) *which = i;\n"
        "                     r.payload = __atomic_load_n(&sl->ret, __ATOMIC_SEQ_CST);\n"
        "                     r.tag = __atomic_load_n(&sl->tag, __ATOMIC_SEQ_CST);\n"
        "                     return r;\n"
        "                 }\n"
        "             } else if (kind == 1) {\n"
        "                 ZxChan *c = arg ? (ZxChan *)(uintptr_t)arg : NULL;\n"
        "                 ZX_ERRU v = zx_chan_recv(s, c, true);\n"
        "                 if (v.tag == 0 || v.tag == ZX_ERR_CLOSED) {\n"
        "                     if (which) *which = i;\n"
        "                     r.payload = v.payload; r.tag = v.tag;\n"
        "                     return r;\n"
        "                 }\n"
        "             }\n"
        "         }\n"
        "         if (zx_now_ms() >= end) return r;\n"
        "         int st = zx_park_tick(s, 1);\n"
        "         if (st == 1) { r.tag = ZX_ERR_CLOSED; return r; }\n"
        "         if (st == 2) { r.tag = ZX_ERR_CANCELLED; return r; }\n"
        "     }\n"
        " }\n"
        " \n"
        " /* close：拒绝新操作 + 唤醒所有停靠者 + 停线程组。实例内存不释放，\n"
        "  * 外部调用者构造/awaitAll/close 串行使用；worker 内部同步由各实例自己的锁负责。 */\n"
        " void zx_task_close(uint64_t h) {\n"
        "     ZxSched *s = zx_sched_raw(h);\n"
        "     if (!s) return;\n"
        "     if (__atomic_load_n(&s->stop, __ATOMIC_SEQ_CST)) return;\n"
        "     __atomic_store_n(&s->closed, true, __ATOMIC_SEQ_CST);\n"
        "     __atomic_store_n(&s->stop, true, __ATOMIC_SEQ_CST);\n"
        "     zx_lk(&s->chan_lk);\n"
        "     ZxTask *rw = s->rwait; s->rwait = NULL;\n"
        "     ZxTask *ww = s->wwait; s->wwait = NULL;\n"
        "     zx_ulk(&s->chan_lk);\n"
        "     zx_lk(&s->wait_lk);\n"
        "     ZxTask *all = s->all_waiters; s->all_waiters = NULL;\n"
        "     zx_ulk(&s->wait_lk);\n"
        "     /* timer 链整体摘空：与 zx_tm_poll 在 tm_lk 下互斥，保证这些\n"
        "      * 停靠者只被 close 唤醒一次，不会有第二次 unpark。 */\n"
        "     zx_lk(&s->tm_lk);\n"
        "     ZxTask *tm = s->timers; s->timers = NULL;\n"
        "     zx_ulk(&s->tm_lk);\n"
        "     ZxTask *tc = tm;\n"
        "     while (tc) { ZxTask *n = tc->tm_next; tc->tm_next = NULL; tc->tm_fired = false; tc->next = n; tc = n; }\n"
        "     /* 命名通道全部封口：停靠者摘链后并入统一唤醒列表，不漏一个。 */\n"
        "     ZxChan *ch = s->ch_list;\n"
        "     ZxTask *chwait = NULL;\n"
        "     while (ch) {\n"
        "         ZxTask *seg = zx_ch_drain(s, ch);\n"
        "         if (seg) {\n"
        "             if (chwait) { ZxTask *p = chwait; while (p->next) p = p->next; p->next = seg; }\n"
        "             else chwait = seg;\n"
        "         }\n"
        "         ch = ch->next_all;\n"
        "     }\n"
        "     ZxTask *list = rw;\n"
        "     if (list) { ZxTask *p = list; while (p->next) p = p->next; p->next = ww; }\n"
        "     else list = ww;\n"
        "     ZxTask *tail = list;\n"
        "     if (tail) { while (tail->next) tail = tail->next; tail->next = all; }\n"
        "     else list = all;\n"
        "     ZxTask *tail2 = list;\n"
        "     if (tail2) { while (tail2->next) tail2 = tail2->next; tail2->next = tm; }\n"
        "     else list = tm;\n"
        "     ZxTask *tail3 = list;\n"
        "     if (tail3) { while (tail3->next) tail3 = tail3->next; tail3->next = chwait; }\n"
        "     else list = chwait;\n"
        "     while (list) {\n"
        "         ZxTask *n = list->next; list->next = NULL;\n"
        "         list->cok = false;\n"
        "         zx_unpark(list);\n"
        "         list = n;\n"
        "     }\n"
        "     zx_lk(&s->idle_lk);\n"
        "     zx_cv_bcast(&s->idle_cv);\n"
        "     while (__atomic_load_n(&s->alive_workers, __ATOMIC_SEQ_CST) > 0)\n"
        "         zx_cv_wait(&s->idle_cv, &s->idle_lk);\n"
        "     zx_ulk(&s->idle_lk);\n"
        "     /* 句柄先失效；只有全部任务结算完（worker 已退出、pending 归零）\n"
        "      * 才整块释放调度器。还有未完成任务时留着内存，避免正在跑的\n"
        "      * fiber 踩到已释放的实例 —— 这是 §94 并发边界允许的最早点。 */\n"
        "     __atomic_store_n(&zx_sched_tab[h - 1], NULL, __ATOMIC_SEQ_CST);\n"
        "     zx_lk(&s->wait_lk);\n"
        "     int left = s->pending;\n"
        "     zx_ulk(&s->wait_lk);\n"
        "     if (left <= 0) {\n"
        " #ifndef _WIN32\n"
        "         pthread_mutex_destroy(&s->qlk); pthread_mutex_destroy(&s->idle_lk);\n"
        "         pthread_cond_destroy(&s->idle_cv); pthread_mutex_destroy(&s->wait_lk);\n"
        "         pthread_mutex_destroy(&s->chan_lk); pthread_mutex_destroy(&s->tab_lk);\n"
        "         pthread_mutex_destroy(&s->tm_lk);\n"
        " #endif\n"
        "         for (int i = 0; i < ZX_MAX_SEGS; i++) {\n"
        "             ZxSeg *sg = __atomic_load_n(&s->segs[i], __ATOMIC_SEQ_CST);\n"
        "             if (sg) free(sg);\n"
        "         }\n"
        "         ZxScope *sc = s->sc_list;\n"
        "         while (sc) {\n"
        "             ZxScope *nx = sc->next_all;\n"
        " #ifndef _WIN32\n"
        "             pthread_mutex_destroy(&sc->lk);\n"
        " #endif\n"
        "             free(sc->ids); free(sc); sc = nx;\n"
        "         }\n"
        "         ZxChan *ch2 = s->ch_list;\n"
        "         while (ch2) { ZxChan *nx = ch2->next_all; free(ch2); ch2 = nx; }\n"
        "         free(s);\n"
        "     }\n"
        " }\n"
        " \n"
        " /* ===== Scope：结构化并发 =====\n"
        "  * close 只让句柄失效并回收 ids 数组；scope 壳挂在实例链表上，由\n"
        "  * zx_task_close 统一 free —— 这样「close 之后仍有并发 spawn 拿着旧\n"
        "  * scope 指针」不是悬垂指针，拿 lk 检查 closed 就能安全拒绝。\n"
        "  * 自动清理靠 std/task.zx 的 defer：正常返回和 !T 错误传播都触发，\n"
        "  * 所以父任务被 cancel 时，整棵子任务树也会被级联取消。 */\n"
        " static void zx_sc_cancel_all(ZxScope *sc) {\n"
        "     if (!sc) return;\n"
        "     zx_lk(&sc->lk);\n"
        "     for (int i = 0; i < sc->n; i++) {\n"
        "         ZxTask *t = zx_task_get(sc->s, sc->ids[i]);\n"
        "         if (t && !__atomic_load_n(&t->done, __ATOMIC_SEQ_CST))\n"
        "             zx_task_cancel_one(sc->s, t);\n"
        "     }\n"
        "     zx_ulk(&sc->lk);\n"
        " }\n"
        " \n"
        " uint64_t zx_sc_new(uint64_t h) {\n"
        "     ZxSched *s = zx_sched_get(h);\n"
        "     if (!s) return 0;\n"
        "     ZxScope *sc = (ZxScope *)calloc(1, sizeof(ZxScope));\n"
        "     if (!sc) return 0;\n"
        "     sc->s = s;\n"
        "     zx_lk_init(&sc->lk);\n"
        "     zx_lk(&s->tab_lk);\n"
        "     sc->next_all = s->sc_list; s->sc_list = sc;\n"
        "     zx_ulk(&s->tab_lk);\n"
        "     return (uint64_t)(uintptr_t)sc;\n"
        " }\n"
        " \n"
        " ZX_OPTU zx_sc_spawn(uint64_t sch, ZX_TASKFN f, uint64_t ctx) {\n"
        "     ZX_OPTU r; r.payload = 0; r.tag = false;\n"
        "     ZxScope *sc = (ZxScope *)(uintptr_t)sch;\n"
        "     if (!sc) return r;\n"
        "     return zx_task_spawn_impl(sc->s->id, f, ctx, sc);\n"
        " }\n"
        " \n"
        " /* 等全体成员收尾：逐个 await。根上下文 drive 调度器，任务上下文挂起\n"
        "  * 等待，已结算的槽位立刻返回。ids 先快照，等待期间并发 spawn 不会\n"
        "  * 改动正在遍历的数组（§94 允许 spawn 与等待并发）。 */\n"
        " static void zx_sc_wait_all(ZxScope *sc) {\n"
        "     uint64_t h = sc->s->id;\n"
        "     zx_lk(&sc->lk);\n"
        "     int n = sc->n;\n"
        "     uint64_t *ids = (uint64_t *)malloc((size_t)(n ? n : 1) * sizeof(uint64_t));\n"
        "     if (ids) { for (int i = 0; i < n; i++) ids[i] = sc->ids[i]; }\n"
        "     zx_ulk(&sc->lk);\n"
        "     if (!ids) return;\n"
        "     for (int i = 0; i < n; i++) (void)zx_task_await(h, ids[i]);\n"
        "     free(ids);\n"
        " }\n"
        " \n"
        " /* join：等全体 + 回放首个错误；全成功时返回首个 spawn 任务的返回值。\n"
        "  * 结果仍可用 t.await(h) 逐个取，和 awaitAll 的纯屏障用法一致。 */\n"
        " ZX_ERRU zx_sc_join(uint64_t sch) {\n"
        "     ZX_ERRU r; r.payload = 0; r.tag = 0;\n"
        "     ZxScope *sc = (ZxScope *)(uintptr_t)sch;\n"
        "     if (!sc) { r.tag = ZX_ERR_CLOSED; return r; }\n"
        "     uint64_t h = sc->s->id;\n"
        "     if (!zx_sched_get(h)) { r.tag = ZX_ERR_CLOSED; return r; }\n"
        "     zx_lk(&sc->lk);\n"
        "     if (sc->closed) { zx_ulk(&sc->lk); r.tag = ZX_ERR_CLOSED; return r; }\n"
        "     zx_ulk(&sc->lk);\n"
        "     zx_sc_wait_all(sc);\n"
        "     zx_lk(&sc->lk);\n"
        "     r.tag = sc->failed ? sc->first_tag : 0;\n"
        "     r.payload = sc->failed ? sc->first_ret : 0;\n"
        "     uint64_t first = sc->n ? sc->ids[0] : 0;\n"
        "     zx_ulk(&sc->lk);\n"
        "     if (!r.tag && first) {\n"
        "         ZxSlot *sl = zx_slot_get(sc->s, first);\n"
        "         if (sl) r.payload = __atomic_load_n(&sl->ret, __ATOMIC_SEQ_CST);\n"
        "     }\n"
        "     return r;\n"
        " }\n"
        " \n"
        " /* close：先取消未完成成员再等全体收尾，作用域退出绝不留活任务。 */\n"
        " void zx_sc_close(uint64_t sch) {\n"
        "     ZxScope *sc = (ZxScope *)(uintptr_t)sch;\n"
        "     if (!sc) return;\n"
        "     zx_lk(&sc->lk);\n"
        "     if (sc->closed) { zx_ulk(&sc->lk); return; }\n"
        "     sc->closed = true;\n"
        "     zx_ulk(&sc->lk);\n"
        "     zx_sc_cancel_all(sc);\n"
        "     zx_sc_wait_all(sc);\n"
        "     zx_lk(&sc->lk);\n"
        "     uint64_t *ids = sc->ids;\n"
        "     sc->ids = NULL; sc->n = 0; sc->cap = 0;\n"
        "     zx_ulk(&sc->lk);\n"
        "     free(ids);\n"
        " }\n"
        " \n"
        " /* ===== t.group()：内核调度线程组（零锁） =====\n"
        "  * 与实例调度器互补的另一极：不建用户态线程池/队列/纤程，spawn 即起\n"
        "  * 一条 OS 线程，调度完全交给内核。分组只是一个原子计数（spawn 先加\n"
        "  * 计数后起线程，起线程失败回退，等待方不会丢任何完成事件）；wait\n"
        "  * 用 让步 → 短睡 的退避循环读计数 —— 全程无互斥锁、无条件变量。\n"
        "  * 任务即发即忘：返回值与错误不回收（要结果/取消/通信用 Task 实例）。\n"
        "  * 组句柄是 8 字节计数器；close() 等 pending 归零后释放它。 */\n"
        " typedef struct ZxTgGrp { uint64_t pending; } ZxTgGrp;\n"
        " typedef struct ZxTgJob { ZX_TASKFN f; uint64_t ctx; ZxTgGrp *g; } ZxTgJob;\n"
        " \n"
        " uint64_t zx_tg_new(void) {\n"
        "     ZxTgGrp *g = (ZxTgGrp *)calloc(1, sizeof(ZxTgGrp));\n"
        "     return (uint64_t)(uintptr_t)g;\n"
        " }\n"
        " \n"
        " #ifdef _WIN32\n"
        " static uint64_t zx_tg_tramp(void *p) {\n"
        " #else\n"
        " static void *zx_tg_tramp(void *p) {\n"
        " #endif\n"
        "     ZxTgJob *j = (ZxTgJob *)p;\n"
        "     j->f(j->ctx);\n"
        "     __atomic_sub_fetch(&j->g->pending, 1, __ATOMIC_ACQ_REL);\n"
        "     free(j);\n"
        "     return 0;\n"
        " }\n"
        " \n"
        " uint64_t zx_tg_spawn(uint64_t gh, ZX_TASKFN f, uint64_t ctx) {\n"
        "     ZxTgGrp *g = (ZxTgGrp *)(uintptr_t)gh;\n"
        "     if (!g) return 0;\n"
        "     ZxTgJob *j = (ZxTgJob *)malloc(sizeof(ZxTgJob));\n"
        "     if (!j) return 0;\n"
        "     j->f = f; j->ctx = ctx; j->g = g;\n"
        "     __atomic_add_fetch(&g->pending, 1, __ATOMIC_ACQ_REL);\n"
        "     if (!zx_rt_thread_spawn(zx_tg_tramp, j)) {\n"
        "         __atomic_sub_fetch(&g->pending, 1, __ATOMIC_ACQ_REL); free(j); return 0;\n"
        "     }\n"
        "     return 1;\n"
        " }\n"
        " \n"
        " /* wait：无锁等 pending 归零。内存序保证：任务体里的 SEQ_CST 原子写\n"
        "  * happens-before 完成方的 ACQ_REL 减计数，等待方 ACQUIRE 读到 0 时\n"
        "  * 全部可见 —— 计数归零即数据就绪，这就是「安全的多线程」的全部开销。 */\n"
        " void zx_tg_wait(uint64_t gh) {\n"
        "     ZxTgGrp *g = (ZxTgGrp *)(uintptr_t)gh;\n"
        "     if (!g) return;\n"
        "     int rest = 0;\n"
        "     while (__atomic_load_n(&g->pending, __ATOMIC_ACQUIRE) != 0) zx_rt_backoff(++rest);\n"
        " }\n"
        " \n"
        " /* close：等组内任务全部收尾再释放计数器，之后句柄不可再用。\n"
        "  * 先 wait 是为了让 tramp 里最后一次 pending-- 落在有效内存上。 */\n"
        " void zx_tg_close(uint64_t gh) {\n"
        "     ZxTgGrp *g = (ZxTgGrp *)(uintptr_t)gh;\n"
        "     if (!g) return;\n"
        "     zx_tg_wait(gh);\n"
        "     free(g);\n"
        " }\n"
        " \n"
    );
}

/* 整个运行时格式化器（只在用到时才生成） */
static void emit_fmt_runtime(void) {
    bool core = g_need_fmt || g_need_allocfmt;   /* 只提到分配器时不用带整个格式化器 */
    Type *alloc_t = sema_allocator_type();
    ensure_type(alloc_t);
    Buf an; buf_init(&an); tn(&an, alloc_t);

    Type *opt_p = ty_opt(ty_ptr(ty_u8(), false, false, 0));
    ensure_type(opt_p);
    Buf on; buf_init(&on); tn(&on, opt_p);

    Type *err_sl = ty_err(ty_slice(ty_u8(), false));
    ensure_type(err_sl);
    Buf en; buf_init(&en); tn(&en, err_sl);

    Buf un;   buf_init(&un);   ctype(&un, ty_usize());
    Buf u8p;  buf_init(&u8p);  ctype(&u8p, ty_ptr(ty_u8(), false, false, 0));

    Buf *save0 = g_out;
    int save0_ind = g_ind;

    /* ZxSink 是格式化器和 {any} 展开器共用的目的地；ZxFmtArg 只给格式化器 */
    if (core || g_need_any)
        buf_puts(&g_btypes,
            "typedef struct ZxAnyType ZxAnyType;\n"
            "typedef struct { FILE *fp; uint8_t *buf; size_t cap; size_t len; } ZxSink;\n\n");
    if (core)
        buf_puts(&g_btypes,
            "typedef struct { int kind; long long i; double d; const uint8_t *p; size_t n;\n"
            "                 unsigned __int128 u; const ZxAnyType *ty; } ZxFmtArg;\n\n");

    if (g_need_page) {
        /* ?*u8 无标签（M26）：C 类型就是 uint8_t*，null 即"空" */
        buf_printf(&g_bglobals, "static %s zx_page_alloc(%s ctx, %s n);\n", u8p.data, u8p.data, un.data);
        buf_printf(&g_bglobals, "static void zx_page_free(%s ctx, %s p, %s n);\n", u8p.data, u8p.data, un.data);
        buf_printf(&g_bglobals, "static %s zx_alloc_page = { 0, zx_page_alloc, zx_page_free };\n\n",
                   an.data);

        Buf *sv = g_out;
        int si = g_ind;
        g_out = &g_bbody;
        g_ind = 0;
        buf_printf(g_out,
            "static %s zx_page_alloc(%s ctx, %s n) {\n"
            "    (void)ctx;                 /* malloc 后端无状态 */\n"
            "    return (%s)malloc(n ? n : 1);   /* 0 = 分配失败 = optional 空 */\n"
            "}\n\n"
            "static void zx_page_free(%s ctx, %s p, %s n) { (void)ctx; (void)n; free(p); }\n\n",
            u8p.data, u8p.data, un.data, u8p.data, u8p.data, u8p.data, un.data);
        g_out = sv;
        g_ind = si;
    }

    if (!core && !g_need_any) { g_out = save0; g_ind = save0_ind; return; }

    /* 落点：两个打印器都往 ZxSink 里写 */
    bline(&g_bproto, "static void zx_sink_put(ZxSink *s, const char *p, size_t n);");
    {
        Buf *sv = g_out;
        int si = g_ind;
        g_out = &g_bbody;
        g_ind = 0;
        buf_puts(g_out,
            "static void zx_sink_put(ZxSink *s, const char *p, size_t n) {\n"
            "    if (n == 0) return;\n"
            "    if (s->fp) { fwrite(p, 1, n, s->fp); s->len += n; return; }\n"
            "    if (s->buf && s->len + n <= s->cap) memcpy(s->buf + s->len, p, n);\n"
            "    s->len += n;\n"
            "}\n\n");
        g_out = sv;
        g_ind = si;
    }
    if (!core) { g_out = save0; g_ind = save0_ind; return; }
    if (g_need_allocfmt)
        buf_printf(&g_bproto, "static %s zx_format(%s al, ZxSlice_u8 fmt, const ZxFmtArg *args, int nargs);\n",
                   en.data, an.data);
    bline(&g_bproto, "static int zx_fmt_out(FILE *fp, ZxSlice_u8 fmt, const ZxFmtArg *args, int nargs, int nl);");
    bline(&g_bproto, "static void zx_fmt_fail(const char *msg);");
    bline(&g_bproto, "static void zx_put_bin(ZxSink *out, unsigned long long v);");
    bline(&g_bproto, "static ZxFmtArg zx_fa_i(long long v);");
    bline(&g_bproto, "static ZxFmtArg zx_fa_u(unsigned long long v);");
    bline(&g_bproto, "static ZxFmtArg zx_fa_i128(__int128 v);");
    bline(&g_bproto, "static ZxFmtArg zx_fa_u128(unsigned __int128 v);");
    bline(&g_bproto, "static ZxFmtArg zx_fa_f(double v);");
    bline(&g_bproto, "static ZxFmtArg zx_fa_b(int v);");
    bline(&g_bproto, "static ZxFmtArg zx_fa_s(ZxSlice_u8 v);");
    bline(&g_bproto, "static ZxFmtArg zx_fa_c(long long v);");
    bline(&g_bproto, "static ZxFmtArg zx_fa_p(const void *v);");
    bline(&g_bproto, "static ZxFmtArg zx_fa_any(const ZxAnyType *t, const void *v);");
    bline(&g_bproto, "static void zx_fmt(ZxSink *out, ZxSlice_u8 fmt, const ZxFmtArg *args, int nargs);");

    Buf *save = g_out;
    int save_ind = g_ind;
    g_out = &g_bbody;
    g_ind = 0;
    buf_puts(g_out,
        "static void zx_fmt_fail(const char *msg) {\n"
        "    zx_diagf(\"zx: format error: %s\\n\", msg);\n"
        "    zx_abort_reported();\n"
        "}\n\n"
        "static void zx_put_bin(ZxSink *out, unsigned long long v) {\n"
        "    char b[65];\n"
        "    int i = 64;\n"
        "    b[i] = 0;\n"
        "    if (v == 0) { zx_sink_put(out, \"0\", 1); return; }\n"
        "    while (v) { b[--i] = (char)('0' + (v & 1)); v >>= 1; }\n"
        "    zx_sink_put(out, &b[i], (size_t)(64 - i));\n"
        "}\n\n"
        "static ZxFmtArg zx_fa_i(long long v) { ZxFmtArg a; a.kind = 1; a.i = v; a.d = 0; a.p = 0; a.n = 0; a.u = 0; a.ty = 0; return a; }\n"
        "static ZxFmtArg zx_fa_u(unsigned long long v) { ZxFmtArg a; a.kind = 2; a.i = (long long)v; a.d = 0; a.p = 0; a.n = 0; a.u = 0; a.ty = 0; return a; }\n"
        "static ZxFmtArg zx_fa_i128(__int128 v) { ZxFmtArg a; a.kind = 9; a.i = 0; a.d = 0; a.p = 0; a.n = 0; a.u = (unsigned __int128)v; a.ty = 0; return a; }\n"
        "static ZxFmtArg zx_fa_u128(unsigned __int128 v) { ZxFmtArg a; a.kind = 10; a.i = 0; a.d = 0; a.p = 0; a.n = 0; a.u = v; a.ty = 0; return a; }\n"
        "static ZxFmtArg zx_fa_f(double v) { ZxFmtArg a; a.kind = 3; a.i = 0; a.d = v; a.p = 0; a.n = 0; a.ty = 0; return a; }\n"
        "static ZxFmtArg zx_fa_b(int v) { ZxFmtArg a; a.kind = 4; a.i = v ? 1 : 0; a.d = 0; a.p = 0; a.n = 0; a.ty = 0; return a; }\n"
        "static ZxFmtArg zx_fa_s(ZxSlice_u8 v) { ZxFmtArg a; a.kind = 5; a.i = 0; a.d = 0; a.p = v.ptr; a.n = v.len; a.ty = 0; return a; }\n"
        "static ZxFmtArg zx_fa_c(long long v) { ZxFmtArg a; a.kind = 6; a.i = v; a.d = 0; a.p = 0; a.n = 0; a.ty = 0; return a; }\n"
        "static ZxFmtArg zx_fa_p(const void *v) { ZxFmtArg a; a.kind = 7; a.i = 0; a.d = 0; a.p = (const uint8_t*)v; a.n = 0; a.ty = 0; return a; }\n"
        "static ZxFmtArg zx_fa_any(const ZxAnyType *t, const void *v) { ZxFmtArg a; a.kind = 8; a.i = 0; a.d = 0; a.p = (const uint8_t*)v; a.n = 0; a.ty = t; return a; }\n\n"
        "static void zx_fmt(ZxSink *out, ZxSlice_u8 fmt, const ZxFmtArg *args, int nargs) {\n"
        "    size_t i = 0;\n"
        "    int next = 0;\n"
        "    char tmp[80];\n"
        "    while (i < fmt.len) {\n"
        "        uint8_t c = fmt.ptr[i];\n"
        "        if (c == '{' && i + 1 < fmt.len && fmt.ptr[i + 1] == '{') { zx_sink_put(out, \"{\", 1); i += 2; continue; }\n"
        "        if (c == '}' && i + 1 < fmt.len && fmt.ptr[i + 1] == '}') { zx_sink_put(out, \"}\", 1); i += 2; continue; }\n"
        "        if (c != '{') {\n"
        "            size_t s0 = i;\n"
        "            while (i < fmt.len && fmt.ptr[i] != '{' && fmt.ptr[i] != '}') i++;\n"
        "            if (i == s0) { zx_sink_put(out, (const char*)&fmt.ptr[i], 1); i++; }\n"
        "            else zx_sink_put(out, (const char*)&fmt.ptr[s0], i - s0);\n"
        "            continue;\n"
        "        }\n"
        "        size_t j = i + 1;\n"
        "        while (j < fmt.len && fmt.ptr[j] != '}') j++;\n"
        "        if (j >= fmt.len) zx_fmt_fail(\"unterminated `{` in format string\");\n"
        "        size_t clen = j - i - 1;\n"
        "        const uint8_t *spec = fmt.ptr + i + 1;\n"
        "        int which = -1, verb = 0;\n"
        "        if (clen == 0) which = next++;\n"
        "        else if (clen == 1 && (spec[0]=='d'||spec[0]=='x'||spec[0]=='b'||spec[0]=='s'||spec[0]=='c')) { verb = spec[0]; which = next++; }\n"
        "        else if (clen == 3 && spec[0]=='a' && spec[1]=='n' && spec[2]=='y') { verb = 'a'; which = next++; }\n"
        "        else {\n"
        "            size_t k = 0; int idx = 0;\n"
        "            while (k < clen && spec[k] >= '0' && spec[k] <= '9') { idx = idx * 10 + (spec[k] - '0'); k++; }\n"
        "            if (k == 0 || k != clen) zx_fmt_fail(\"unknown format specifier\");\n"
        "            which = idx;\n"
        "        }\n"
        "        if (which < 0 || which >= nargs) zx_fmt_fail(\"not enough arguments for the format string\");\n"
        "        {\n"
        "            const ZxFmtArg *a = &args[which];\n"
        "            if (a->ty && (verb == 0 || verb == 'a' || verb == 's' || a->kind == 8))\n"
        "                zx_any(out, a->ty, a->p, 0, verb == 'a' ? 1 : 0);\n"
        "            else if (a->kind == 5) zx_sink_put(out, (const char*)a->p, a->n);\n"
        "            else if (verb == 'c') { tmp[0] = (char)(int)a->i; zx_sink_put(out, tmp, 1); }\n"
        "            else if (a->kind == 4 && verb != 'd' && verb != 'x' && verb != 'b')\n"
        "                zx_sink_put(out, a->i ? \"true\" : \"false\", a->i ? 4 : 5);\n"
        "            else if (a->kind == 3) { char fb[64]; const char *fs = zx_f64_str(fb, sizeof fb, a->d); zx_sink_put(out, fs, strlen(fs)); }\n"
        "            else if (a->kind == 9 || a->kind == 10) {\n"
        "                char big[48];\n"
        "                if (a->kind == 9) zx_i128_str(big, (__int128)a->u);\n"
        "                else zx_u128_to(big, a->u);\n"
        "                zx_sink_put(out, big, strlen(big));\n"
        "            }\n"
        "            else if (verb == 'b') zx_put_bin(out, (unsigned long long)a->i);\n"
        "            else if (a->kind == 1 || a->kind == 2 || a->kind == 4 || a->kind == 6) {\n"
        "                if (verb == 'x') snprintf(tmp, sizeof tmp, \"%llx\", (unsigned long long)a->i);\n"
        "                else if (a->kind == 1) snprintf(tmp, sizeof tmp, \"%lld\", a->i);\n"
        "                else snprintf(tmp, sizeof tmp, \"%llu\", (unsigned long long)a->i);\n"
        "                zx_sink_put(out, tmp, strlen(tmp));\n"
        "            } else { snprintf(tmp, sizeof tmp, \"%p\", (const void*)a->p); zx_sink_put(out, tmp, strlen(tmp)); }\n"
        "        }\n"
        "        i = j + 1;\n"
        "    }\n"
        "}\n\n"
        "static int zx_fmt_out(FILE *fp, ZxSlice_u8 fmt, const ZxFmtArg *args, int nargs, int nl) {\n"
        "    ZxSink s;\n"
        "    s.fp = fp; s.buf = 0; s.cap = 0; s.len = 0;\n"
        "    zx_fmt(&s, fmt, args, nargs);\n"
        "    if (nl) zx_sink_put(&s, \"\\n\", 1);\n"
        "    return 0;\n"
        "}\n\n");

    if (g_need_allocfmt) {
        buf_printf(g_out,
            "static %s zx_format(%s al, ZxSlice_u8 fmt, const ZxFmtArg *args, int nargs) {\n"
            "    %s r;\n"
            "    ZxSink m;\n"
            "    %s mem;\n"
            "    ZxSink w;\n"
            "    size_t need;\n"
            "    m.fp = 0; m.buf = 0; m.cap = 0; m.len = 0;\n"
            "    zx_fmt(&m, fmt, args, nargs);\n"
            "    need = m.len + 1;\n"
            "    mem = al.alloc(al.ctx, need);\n"
            "    if (!mem) { r.payload.ptr = 0; r.payload.len = 0; r.tag = 1; return r; }\n"
            "    w.fp = 0; w.buf = mem; w.cap = need; w.len = 0;\n"
            "    zx_fmt(&w, fmt, args, nargs);\n"
            "    mem[w.len] = 0;\n"
            "    r.payload.ptr = mem;\n"
            "    r.payload.len = w.len;\n"
            "    r.tag = 0;\n"
            "    return r;\n"
            "}\n\n",
            en.data, an.data, en.data, u8p.data);
    }

    g_out = save;
    g_ind = save_ind;
}

/* 值的地址。C 里没有"右值的地址"，但数组复合字面量是左值，
 * 而数组元素可以拿任意同类型表达式初始化——`(T[]){ e }[0]` 就得到一个
 * 有块作用域的临时对象。对函数返回值、字面量、标识符一概成立。 */
static void any_addr(Buf *b, Node *e) {
    Type *t = e->ty ? e->ty : ty_i32();
    ensure_type(t);
    buf_puts(b, "&(");
    ctype(b, t);
    buf_puts(b, "[]){ (");
    ex(b, e);
    buf_puts(b, ") }[0]");
}

/* {any} 的运行时：一个描述符驱动的递归打印器。
 * 编译期路径（格式串是常量）也调用它——两条路的输出因此不可能分叉。 */
static void emit_any_runtime(void) {
    if (!g_need_any && !g_need_fmt && !g_need_allocfmt) return;

    bline(&g_bproto, "static void zx_any(ZxSink *out, const ZxAnyType *t, const void *p, int depth, int mode);");

    /* 一个描述符都没有：格式化器仍然要能链接，给个空壳就行 */
    if (g_nad == 0) {
        Buf *sv0 = g_out;
        int si0 = g_ind;
        g_out = &g_bbody;
        g_ind = 0;
        buf_puts(g_out,
            "static void zx_any(ZxSink *out, const ZxAnyType *t, const void *p, int depth, int mode) {\n"
            "    (void)t; (void)p; (void)depth; (void)mode;\n"
            "    zx_sink_put(out, \"?\", 1);\n"
            "}\n\n");
        g_out = sv0;
        g_ind = si0;
        return;
    }

    buf_puts(&g_btypes,
        "typedef struct { const char *name; size_t off; int ty; } ZxAnyField;\n"
        "enum { ZX_AK_VOID = 0, ZX_AK_INT, ZX_AK_FLOAT, ZX_AK_BOOL, ZX_AK_ENUM,\n"
        "       ZX_AK_ERROR, ZX_AK_PTR, ZX_AK_SLICE, ZX_AK_STRUCT, ZX_AK_UNION,\n"
        "       ZX_AK_TUNION, ZX_AK_OPT, ZX_AK_ERR, ZX_AK_FN, ZX_AK_STRPTR };\n"
        "struct ZxAnyType { int kind; const char *name; int size; int sign; int n;\n"
        "                   const ZxAnyField *fields; int elem; int aux_off; int aux_size;\n"
        "                   const long long *tags; const char *const *names; int is_str; };\n"
        "typedef struct { const void *ptr; size_t len; } ZxAnySlice;\n\n");

    bline(&g_bproto, "static void zx_any_put(ZxSink *out, const char *s);");
    bline(&g_bproto, "static long long zx_any_int(const void *p, int size, int sign);");
    bline(&g_bproto, "static void zx_any_qstr(ZxSink *out, const uint8_t *p, size_t n);");
    bline(&g_bproto, "static void zx_any(ZxSink *out, const ZxAnyType *t, const void *p, int depth, int mode);");
    bline(&g_bproto, "static int zx_any_stdout(const ZxAnyType *t, const void *p, int mode);");

    Buf *sv = g_out;
    int si = g_ind;
    g_out = &g_bbody;
    g_ind = 0;

    buf_puts(g_out,
        "static void zx_any_put(ZxSink *out, const char *s) { zx_sink_put(out, s, strlen(s)); }\n\n"
        "static long long zx_any_int(const void *p, int size, int sign) {\n"
        "    switch (size) {\n"
        "    case 1: return sign ? (long long)*(const int8_t *)p : (long long)*(const uint8_t *)p;\n"
        "    case 2: return sign ? (long long)*(const int16_t *)p : (long long)*(const uint16_t *)p;\n"
        "    case 4: return sign ? (long long)*(const int32_t *)p : (long long)*(const uint32_t *)p;\n"
        "    default: return sign ? (long long)*(const int64_t *)p : (long long)*(const uint64_t *)p;\n"
        "    }\n"
        "}\n\n"
        "static void zx_any_qstr(ZxSink *out, const uint8_t *p, size_t n) {\n"
        "    size_t i;\n"
        "    zx_any_put(out, \"\\\"\");\n"
        "    for (i = 0; i < n; i++) {\n"
        "        uint8_t c = p[i];\n"
        "        char b[8];\n"
        "        if (c == '\"') zx_any_put(out, \"\\\\\\\"\");\n"
        "        else if (c == '\\\\') zx_any_put(out, \"\\\\\\\\\");\n"
        "        else if (c == '\\n') zx_any_put(out, \"\\\\n\");\n"
        "        else if (c == '\\t') zx_any_put(out, \"\\\\t\");\n"
        "        else if (c == '\\r') zx_any_put(out, \"\\\\r\");\n"
        "        else if (c < 32 || c >= 127) { snprintf(b, sizeof b, \"\\\\x%02x\", (unsigned)c); zx_any_put(out, b); }\n"
        "        else { b[0] = (char)c; zx_sink_put(out, b, 1); }\n"
        "    }\n"
        "    zx_any_put(out, \"\\\"\");\n"
        "}\n\n");

    buf_puts(g_out,
        "static void zx_any(ZxSink *out, const ZxAnyType *t, const void *p, int depth, int mode) {\n"
        "    char b[64];\n"
        "    if (!t) { zx_any_put(out, \"?\"); return; }\n"
        "    if (depth > 8) { zx_any_put(out, \"...\"); return; }\n"
        "    switch (t->kind) {\n"
        "    case ZX_AK_VOID: zx_any_put(out, \"void\"); break;\n"
        "    case ZX_AK_BOOL: zx_any_put(out, *(const bool *)p ? \"true\" : \"false\"); break;\n"
        "    case ZX_AK_INT:\n"
        "        if (t->size == 16) {\n"
        "            char big[48];\n"
        "            if (t->sign) zx_i128_str(big, *(const __int128 *)p);\n"
        "            else zx_u128_str(big, *(const unsigned __int128 *)p);\n"
        "            zx_any_put(out, big);\n"
        "            break;\n"
        "        }\n"
        "        if (t->sign) snprintf(b, sizeof b, \"%lld\", zx_any_int(p, t->size, 1));\n"
        "        else snprintf(b, sizeof b, \"%llu\", (unsigned long long)zx_any_int(p, t->size, 0));\n"
        "        zx_any_put(out, b);\n"
        "        break;\n"
        "    case ZX_AK_FLOAT:\n"
        "        if (t->size == 4) zx_f64_str(b, sizeof b, (double)*(const float *)p);\n"
        "        else zx_f64_str(b, sizeof b, *(const double *)p);\n"
        "        zx_any_put(out, b);\n"
        "        break;\n"
        "    case ZX_AK_ENUM: {\n"
        "        long long v = zx_any_int(p, t->size, t->sign);\n"
        "        int i;\n"
        "        if (mode != 0) {\n"
        "            for (i = 0; i < t->n; i++) {\n"
        "                if (t->tags[i] != v) continue;\n"
        "                if (t->name) { zx_any_put(out, t->name); zx_any_put(out, \".\"); }\n"
        "                zx_any_put(out, t->names[i]);\n"
        "                return;\n"
        "            }\n"
        "        }\n"
        "        snprintf(b, sizeof b, \"%lld\", v);\n"
        "        zx_any_put(out, b);\n"
        "        break;\n"
        "    }\n"
        "    case ZX_AK_ERROR:\n"
        "        snprintf(b, sizeof b, \"error(%u)\", (unsigned)*(const uint16_t *)p);\n"
        "        zx_any_put(out, b);\n"
        "        break;\n"
        "    case ZX_AK_PTR: {\n"
        "        const void *q = *(const void *const *)p;\n"
        "        if (!q) { zx_any_put(out, \"null\"); break; }\n"
        "        snprintf(b, sizeof b, \"0x%llx\", (unsigned long long)(uintptr_t)q);\n"
        "        zx_any_put(out, b);\n"
        "        break;\n"
        "    }\n"
        "    case ZX_AK_FN: {\n"
        "        uintptr_t q = 0;\n"
        "        memcpy(&q, p, sizeof q);\n"
        "        snprintf(b, sizeof b, \"fn@0x%llx\", (unsigned long long)q);\n"
        "        zx_any_put(out, b);\n"
        "        break;\n"
        "    }\n"
        "    case ZX_AK_STRPTR: {\n"
        "        /* 字符串字面量（*[N]const u8）：值就是基址，长度在描述符里 */\n"
        "        const void *q = *(const void *const *)p;\n"
        "        size_t n = (size_t)t->aux_size;\n"
        "        if (!q) { zx_any_put(out, \"null\"); break; }\n"
        "        if (mode == 0) zx_sink_put(out, (const char *)q, n);\n"
        "        else zx_any_qstr(out, (const uint8_t *)q, n);\n"
        "        break;\n"
        "    }\n"
        "    case ZX_AK_SLICE: {\n"
        "        const ZxAnySlice *s = (const ZxAnySlice *)p;\n"
        "        size_t i, lim;\n"
        "        if (t->is_str) {\n"
        "            if (mode == 0) zx_sink_put(out, (const char *)s->ptr, s->len);\n"
        "            else zx_any_qstr(out, (const uint8_t *)s->ptr, s->len);\n"
        "            break;\n"
        "        }\n"
        "        zx_any_put(out, \"[\");\n"
        "        lim = s->len > 64 ? 64 : s->len;\n"
        "        for (i = 0; i < lim; i++) {\n"
        "            if (i) zx_any_put(out, \", \");\n"
        "            zx_any(out, &zx_at[t->elem],\n"
        "                  (const char *)s->ptr + i * (size_t)zx_at[t->elem].size, depth + 1, 1);\n"
        "        }\n"
        "        if (s->len > lim) zx_any_put(out, \", ...\");\n"
        "        zx_any_put(out, \"]\");\n"
        "        break;\n"
        "    }\n"
        "    case ZX_AK_STRUCT: {\n"
        "        int i;\n"
        "        if (t->name) zx_any_put(out, t->name);\n"
        "        zx_any_put(out, \"{\");\n"
        "        for (i = 0; i < t->n; i++) {\n"
        "            if (i) zx_any_put(out, \", \");\n"
        "            if (t->fields[i].name) { zx_any_put(out, t->fields[i].name); zx_any_put(out, \": \"); }\n"
        "            zx_any(out, &zx_at[t->fields[i].ty], (const char *)p + t->fields[i].off, depth + 1, 1);\n"
        "        }\n"
        "        zx_any_put(out, \"}\");\n"
        "        break;\n"
        "    }\n"
        "    case ZX_AK_UNION: {\n"
        "        int i;\n"
        "        if (t->name) zx_any_put(out, t->name);\n"
        "        zx_any_put(out, \"{\");\n"
        "        for (i = 0; i < t->n; i++) {\n"
        "            if (i) zx_any_put(out, \" | \");\n"
        "            if (t->fields[i].name) { zx_any_put(out, t->fields[i].name); zx_any_put(out, \": \"); }\n"
        "            zx_any(out, &zx_at[t->fields[i].ty], (const char *)p + t->fields[i].off, depth + 1, 1);\n"
        "        }\n"
        "        zx_any_put(out, \"}\");\n"
        "        break;\n"
        "    }\n"
        "    case ZX_AK_TUNION: {\n"
        "        long long tag = zx_any_int((const char *)p + t->aux_off, t->aux_size, 0);\n"
        "        int i;\n"
        "        for (i = 0; i < t->n; i++) {\n"
        "            if (t->tags[i] != tag) continue;\n"
        "            if (t->name) zx_any_put(out, t->name);\n"
        "            zx_any_put(out, \"{\");\n"
        "            zx_any_put(out, t->names[i]);\n"
        "            zx_any_put(out, \": \");\n"
        "            zx_any(out, &zx_at[t->fields[i].ty], (const char *)p + t->fields[i].off, depth + 1, 1);\n"
        "            zx_any_put(out, \"}\");\n"
        "            return;\n"
        "        }\n"
        "        zx_any_put(out, \"?\");\n"
        "        break;\n"
        "    }\n"
        "    case ZX_AK_OPT:\n"
        "        if (!*(const bool *)((const char *)p + t->aux_off)) { zx_any_put(out, \"null\"); break; }\n"
        "        zx_any_put(out, \"some(\");\n"
        "        zx_any(out, &zx_at[t->elem], p, depth + 1, 1);\n"
        "        zx_any_put(out, \")\");\n"
        "        break;\n"
        "    case ZX_AK_ERR: {\n"
        "        uint16_t tag = *(const uint16_t *)((const char *)p + t->aux_off);\n"
        "        if (tag == 0) {\n"
        "            zx_any_put(out, \"ok(\");\n"
        "            zx_any(out, &zx_at[t->elem], p, depth + 1, 1);\n"
        "            zx_any_put(out, \")\");\n"
        "        } else {\n"
        "            snprintf(b, sizeof b, \"err(error(%u))\", (unsigned)tag);\n"
        "            zx_any_put(out, b);\n"
        "        }\n"
        "        break;\n"
        "    }\n"
        "    default: zx_any_put(out, \"?\"); break;\n"
        "    }\n"
        "}\n\n"
        "static int zx_any_stdout(const ZxAnyType *t, const void *p, int mode) {\n"
        "    ZxSink s;\n"
        "    s.fp = stdout; s.buf = 0; s.cap = 0; s.len = 0;\n"
        "    zx_any(&s, t, p, 0, mode);\n"
        "    return 0;\n"
        "}\n\n");

    g_out = sv;
    g_ind = si;

    /* 描述符本体：所有 zx_af_* 辅助表都在这之前落好了，一次性定义 */
    buf_puts(&g_bany, "static const ZxAnyType zx_at[] = {\n");
    if (g_nad == 0)
        buf_puts(&g_bany, "{ .kind = ZX_AK_VOID },\n");
    for (int i = 0; i < g_nad; i++) {
        buf_puts(&g_bany, g_anydef[i]);
        buf_puts(&g_bany, ",\n");
    }
    buf_puts(&g_bany, "};\n\n");
}

/* ---- 统一分配 API（decisions §44）----
 * 两个的 sema 结果类型都是 `*[N]T`，这里只负责把"申请一段数组存储"发出来：
 * 栈那份走 alloca（活到函数返回，和 Zig 的栈分配一个语义）；
 * 堆那份穿过分配器的 alloc 函数指针，OOM 直接 abort——结果类型是
 * 非可选的数组指针，没有失败路径可表达。 */
static void ex_stackalloc(Buf *b, Node *n) {
    Type *et = (n->ty && n->ty->kind == TY_PTR) ? n->ty->elem : ty_u8();
    /* §201：堆/栈分配返回 *[N]T，et 是 [N]T；第二参 N 是**元素**个数，
     * 字节数要按元素类型算。拿 sizeof([N]T) 再乘 N 会变成 N² 字节。 */
    Type *ut = (et && et->kind == TY_ARRAY) ? et->elem : et;
    Buf t; buf_init(&t); ctype(&t, et);
    Buf u; buf_init(&u); ctype(&u, ut);
    buf_puts(b, "((");
    buf_puts(b, t.data);
    buf_puts(b, " *)(__builtin_alloca((size_t)(");
    coerce(b, n->items[1], ty_usize());
    buf_printf(b, ") * sizeof(%s))))", u.data);
}

static bool g_need_heapalloc;   /* 用过 std.heapAlloc → 带上 zx_heapalloc */

static void ex_heapalloc(Buf *b, Node *n) {
    g_need_heapalloc = true;
    Type *et = (n->ty && n->ty->kind == TY_PTR) ? n->ty->elem : ty_u8();
    /* §130 预分配：这个点不再单独申请，按编译期算好的静态偏移落位 */
    if (n->reg_gid > 0) {
        Buf t; buf_init(&t); ctype(&t, et);
        buf_printf(b, "((%s *)(__zxr%d + (uint64_t)%llu))",
                   t.data, n->reg_gid, (unsigned long long)n->reg_off);
        return;
    }
    /* §201：同 ex_stackalloc——第二参是元素个数，字节数按元素类型算。 */
    Type *ut = (et && et->kind == TY_ARRAY) ? et->elem : et;
    Buf t; buf_init(&t); ctype(&t, et);
    Buf u; buf_init(&u); ctype(&u, ut);
    buf_puts(b, "((");
    buf_puts(b, t.data);
    buf_puts(b, " *)(zx_heapalloc(");
    ex(b, n->items[0]);          /* 分配器 */
    buf_puts(b, ", (uint64_t)(");
    coerce(b, n->items[2], ty_usize());
    buf_printf(b, ") * sizeof(%s))))", u.data);
}

/* std.heapSlice(a, T, n) -> []T。n 只在函数实参里求值一次：
 * 若直接发 `((ZxSlice_T){ .ptr = ..., .len = n })`，n 会在生成的 C 里
 * 出现两次，`heapSlice(a, T, f())` 就会把 f 调用两遍。所以按 []T 生成
 * 一份特化的运行时助手，定义统一落在 zxrt 单元（与 zx_heapalloc 同处）。 */
static Type *g_hslice_types[32];
static int   g_nhslice;

static void ex_heapslice(Buf *b, Node *n) {
    g_need_heapalloc = true;
    Type *sl = (n->ty && n->ty->kind == TY_SLICE) ? n->ty
                                                  : ty_slice(ty_u8(), false);
    ensure_type(sl);
    Buf sn; buf_init(&sn); tn(&sn, sl);
    bool have = false;
    for (int i = 0; i < g_nhslice; i++) {
        Buf o; buf_init(&o); tn(&o, g_hslice_types[i]);
        if (!strcmp(o.data, sn.data)) { have = true; break; }
    }
    if (!have && g_nhslice < 32) g_hslice_types[g_nhslice++] = sl;
    buf_printf(b, "zx_heapslice_%s(", sn.data);
    ex(b, n->items[0]);                      /* 分配器 */
    buf_puts(b, ", (uint64_t)(");
    coerce(b, n->items[2], ty_usize());      /* n：只在这里求值一次 */
    buf_puts(b, "))");
}

static void emit_heapslice_runtime(void) {
    Type *al = sema_allocator_type();
    ensure_type(al);
    Buf an; buf_init(&an); tn(&an, al);
    for (int i = 0; i < g_nhslice; i++) {
        Type *sl = g_hslice_types[i];
        Buf sn; buf_init(&sn); tn(&sn, sl);
        Buf ct; buf_init(&ct); ctype(&ct, sl->elem);
        buf_printf(&g_bglobals, "static %s zx_heapslice_%s(%s al, uint64_t n);\n",
                   sn.data, sn.data, an.data);
        buf_printf(g_out,
            "static %s zx_heapslice_%s(%s al, uint64_t n) {\n"
            "    %s r;\n"
            "    r.ptr = (%s *)zx_heapalloc(al, n * sizeof(%s));\n"
            "    r.len = (size_t)n;\n"
            "    return r;\n"
            "}\n\n",
            sn.data, sn.data, an.data, sn.data, ct.data, ct.data);
    }
}

static void ex_print(Buf *b, Node *n, bool newline) {
    Node *fmt = n->items[0];

    /* 格式串必须是编译期已知的字符串 */
    const char *p = fmt->str;
    int len = fmt->str_len;
    if (!p && fmt->ct && fmt->ct->kind == V_STR) { p = fmt->ct->sval; len = fmt->ct->slen; }
    if (!p) {
        /* 运行时格式串：交给 zx_fmt_out（同一套语法，行为一致，只是慢） */
        buf_puts(b, "zx_fmt_out(stdout, ");
        coerce(b, fmt, ty_slice(ty_u8(), true));
        buf_puts(b, ", ");
        fmt_args(b, n, 1);
        buf_printf(b, ", %d, %d)", n->nitems - 1, newline ? 1 : 0);
        g_need_fmt = true;
        return;
    }

    PrintSeg g;
    buf_init(&g.out); buf_init(&g.spec); buf_init(&g.args);
    g.nseg = 0;
    bool need_bin = false;

    int argi = 1;                 /* 顺序取参数的游标 */
    int lit = 0;                  /* 尚未输出的字面量起点 */
    for (int i = 0; i < len; i++) {
        if (p[i] == '{' && i + 1 < len && p[i + 1] == '{') {
            seg_lit(&g.spec, p + lit, i - lit);
            buf_putc(&g.spec, '{');
            i++; lit = i + 1;
            continue;
        }
        if (p[i] == '}' && i + 1 < len && p[i + 1] == '}') {
            seg_lit(&g.spec, p + lit, i - lit);
            buf_putc(&g.spec, '}');
            i++; lit = i + 1;
            continue;
        }
        if (p[i] != '{') continue;

        int j = i + 1;
        while (j < len && p[j] != '}') j++;
        if (j >= len) {
            err_at(fmt->line, fmt->col, "unterminated `{` in format string");
            break;
        }
        int clen = j - i - 1;
        const char *c = p + i + 1;

        /* 选项在冒号后面：{verb:opts} / {N:opts} / {:opts}（M26） */
        const char *colon = (const char *)memchr(c, ':', (size_t)clen);
        int plen = colon ? (int)(colon - c) : clen;
        FmtOpt fopt = { ' ', 0, 0, -1 };
        if (colon && !fmt_parse_opts(colon + 1, clen - plen - 1, &fopt, fmt)) {
            i = j; lit = i + 1;
            continue;
        }

        /* 取参数 */
        int which = -1;
        int verb = 0;
        if (plen == 0) {
            which = argi++;
        } else if (plen <= 2 && (plen == 1 && (c[0] == 'd' || c[0] == 'x' || c[0] == 'b' ||
                                              c[0] == 's' || c[0] == 'c'))) {
            which = argi++;
            verb = c[0];
        } else if (plen == 3 && !strncmp(c, "any", 3)) {
            which = argi++;
            verb = 'a';
        } else {
            /* 数字索引 */
            int k = 0; which = 0;
            while (k < plen && c[k] >= '0' && c[k] <= '9') { which = which * 10 + (c[k] - '0'); k++; }
            if (k != plen) {
                err_at(fmt->line, fmt->col, "unknown format specifier `{%.*s}`", clen, c);
                i = j;
                lit = i + 1;
                continue;
            }
            which += 1;      /* 实参下标：0 是 fmt 自己 */
            if (which < 1 || which >= n->nitems) {
                err_at(fmt->line, fmt->col, "format argument {%d} is out of range", which - 1);
                i = j;
                lit = i + 1;
                continue;
            }
        }
        if (which < 1 || which >= n->nitems) {
            err_at(fmt->line, fmt->col, "not enough arguments for the format string");
            i = j; lit = i + 1;
            continue;
        }

        Type *at = n->items[which]->ty;
        bool anyv = (verb == 'a') || (verb == 0 && default_any(at));

        if (verb == 'b') {
            /* 二进制要先收掉当前段 */
            seg_lit(&g.spec, p + lit, i - lit);
            seg_flush(&g);
            Buf one; buf_init(&one);
            fmt_spec(&g.spec, &one, n->items[which], 'b', &need_bin, &fopt);
            if (g.nseg) buf_puts(&g.out, ", ");
            buf_printf(&g.out, "zx_print_bin((uint64_t)(");
            ex(&g.out, n->items[which]);
            buf_puts(&g.out, "))");
            g.nseg++;
        } else if (anyv) {
            /* 结构展开也是一段独立的表达式（printf 塞不进复合值） */
            seg_lit(&g.spec, p + lit, i - lit);
            seg_flush(&g);
            if (str_ptr_ty(at)) {
                /* `{any}` 打在字符串上：包成切片再展开，输出带引号的文本 */
                ensure_type(ty_slice(ty_u8(), true));
                int ds = any_desc(ty_slice(ty_u8(), true));
                if (g.nseg) buf_puts(&g.out, ", ");
                buf_printf(&g.out, "zx_any_stdout(&zx_at[%d], &((ZxSlice_u8){ (uint8_t*)(", ds);
                ex(&g.out, n->items[which]);
                buf_printf(&g.out, "), %llu }), %d)", (unsigned long long)at->len,
                           verb == 'a' ? 1 : 0);
                g.nseg++;
            } else {
            int d = any_desc(at);
            if (g.nseg) buf_puts(&g.out, ", ");
            buf_printf(&g.out, "zx_any_stdout(&zx_at[%d], ", d);
            any_addr(&g.out, n->items[which]);
            buf_printf(&g.out, ", %d)", verb == 'a' ? 1 : 0);
            g.nseg++;
            }
        } else if (str_ptr_ty(at) && fopt.width == 0) {
            /* `{}` 打在字符串字面量上：长度在类型里，指针就是数据起点，
             * 按字节原样写出（与切片同一条路径，NUL 不截断） */
            seg_lit(&g.spec, p + lit, i - lit);
            seg_flush(&g);
            if (g.nseg) buf_puts(&g.out, ", ");
            buf_puts(&g.out, "fwrite((const char*)(");
            ex(&g.out, n->items[which]);
            buf_printf(&g.out, "), 1, (size_t)%llu, stdout)", (unsigned long long)at->len);
            g.nseg++;
        } else if (at && at->kind == TY_SLICE && fopt.width == 0) {
            /* 不带宽度的字符串：按长度原样写出去，不走 `%.*s`。
             * printf 的 %.*s 遇 NUL 就停，"a\0b" 只会打出 "a"——字节切片
             * 该原样落地（§11.1 的 fwrite 形态）。 */
            seg_lit(&g.spec, p + lit, i - lit);
            seg_flush(&g);
            if (g.nseg) buf_puts(&g.out, ", ");
            buf_printf(&g.out, "fwrite((const char*)(");
            ex(&g.out, n->items[which]);
            buf_printf(&g.out, ").ptr, 1, (size_t)(");
            ex(&g.out, n->items[which]);
            buf_printf(&g.out, ").len, stdout)");
            g.nseg++;
        } else {
            seg_lit(&g.spec, p + lit, i - lit);
            fmt_spec(&g.spec, &g.args, n->items[which], verb, &need_bin, &fopt);
        }
        i = j;
        lit = i + 1;
    }
    seg_lit(&g.spec, p + lit, len - lit);
    if (newline) buf_puts(&g.spec, "\\n");
    seg_flush(&g);

    if (need_bin) g_need_bin = true;

    if (g.out.data) {
        if (g.nseg > 1) { buf_putc(b, '('); buf_puts(b, g.out.data); buf_putc(b, ')'); }
        else buf_puts(b, g.out.data);
    } else {
        buf_puts(b, "0");
    }
}

/* std.io.print(h, fmt, args…) / std.io.println —— stdout 之外的格式化输出。
 * 句柄就是 FILE*（std.io 的约定），装在 usize 里。一律走运行时格式化器
 * zx_fmt_out：
 *   ① 句柄只求值一次（编译期拆段的话 FILE* 得在每段重复）；
 *   ② 和 std.print / std.println 共用同一套 zx_fmt 语法，输出不会分叉；
 *   ③ 动词合法性 sema 已经查过（check_fmt_args），运行时不会踩到坏格式串。
 * 代价是格式串在运行时才解析——文件 I/O 的开销面前不值一提（decisions §46）。 */
static void ex_io_print(Buf *b, Node *n, bool newline) {
    g_need_io = true;
    g_need_fmt = true;
    buf_puts(b, "zx_fmt_out((FILE *)(uintptr_t)(");
    ex(b, n->items[0]);
    buf_puts(b, "), ");
    coerce(b, n->items[1], ty_slice(ty_u8(), true));
    buf_puts(b, ", ");
    fmt_args(b, n, 2);
    buf_printf(b, ", %d, %d)", n->nitems - 2, newline ? 1 : 0);
}

/* 布局大小：comptime_int / comptime_float 没有运行时宽度，
 * 但元组的 .size 得给个数——按 R7 的默认宽度（i32 / f64）算。 */
/* std.format(alloc, fmt, args…) → ![]u8：先量长度、再分配、再写一遍 */
static void ex_format(Buf *b, Node *n) {
    buf_puts(b, "zx_format(");
    ex(b, n->items[0]);
    buf_puts(b, ", ");
    coerce(b, n->items[1], ty_slice(ty_u8(), true));
    buf_puts(b, ", ");
    fmt_args(b, n, 2);
    buf_printf(b, ", %d)", n->nitems - 2);
    g_need_fmt = true;
    g_need_allocfmt = true;
}

static int layout_size(Type *t) {
    if (!t) return 0;
    if (t->kind == TY_COMPTIME_INT)   return 4;
    if (t->kind == TY_COMPTIME_FLOAT) return 8;
    if (t->kind == TY_TUPLE) {
        int a = 1, off = 0;
        for (int i = 0; i < t->nfields; i++) {
            Type *ft = t->fields[i].ty;
            int fa = ft && (ft->kind == TY_COMPTIME_INT || ft->kind == TY_COMPTIME_FLOAT)
                     ? layout_size(ft) : ty_align(ft);
            int fs = layout_size(ft);
            if (fa > a) a = fa;
            off = (off + fs + fa - 1) / fa * fa;
        }
        return (off + a - 1) / a * a;
    }
    int sz = ty_size(t);
    return sz < 0 ? 0 : sz;
}

/* ---- 主分发 ---- */

/* 超 u64 的 128 位绝对值 → C 字面量（M19）。负数直接发二补码位模式
 * （-mag mod 2^128），对 INT128_MIN 也精确——先转有符号再取负的写法
 * 在最小值上是 UB，不能用。位模式转 __int128 在 gcc/clang 上是良定义的。 */
static void emit_big128(Buf *b, uint64_t hi, uint64_t lo, bool neg) {
    if (neg) {
        uint64_t nlo = lo ? (uint64_t)(0 - lo) : 0;
        uint64_t nhi = lo ? (uint64_t)(~hi) : (uint64_t)(0 - hi);
        buf_printf(b, "((__int128)((unsigned __int128)0x%016llxULL << 64 | 0x%016llxULL))",
                   (unsigned long long)nhi, (unsigned long long)nlo);
    } else
        buf_printf(b, "((unsigned __int128)0x%016llxULL << 64 | 0x%016llxULL)",
                   (unsigned long long)hi, (unsigned long long)lo);
}

/* 整数位宽 → C 无符号类型名（同宽度；带符号比较/位运算用） */

/* M19 数值语义的统一发射：整数二元运算在 C 里的形态。
 *
 *  - 有符号 + - *：按无符号算再转回（二补码回绕；C 的有符号溢出是 UB）；
 *  - <<：移位数量按位宽掩码（>= bits 的移位是 UB），有符号左移同样走无符号；
 *  - >>：保留算术右移（符号扩展，规格钉住）；
 *  - / %：除零是运行时 trap（zx_div_* / zx_mod_* 辅助）。
 *
 * 返回 false = 不归这里管（浮点、比较、位运算照旧直译）。 */
/* 计算用的"加宽无符号"类型：至少 32 位。
 *
 * R10 要求结果按**目标位宽**回绕。C 的整数提升会把 uint8_t/uint16_t
 * 的操作数抬成 int——`(uint8_t)a * (uint8_t)b` 两个 255 相乘在 int 里是
 * 65025（没事），但 uint16_t 的 65535*65535 就溢出 int，成了 UB。所以在
 * 一个"装得下任意两个本宽度数之和/积"的无符号类型里算：宽度 ≤32 用
 * uint32_t（(2^16-1)^2 < 2^32），≤64 用 uint64_t，128 用 unsigned __int128。 */
static void wtype_of_bits(Buf *b, int bits) {
    if (bits <= 32)      buf_puts(b, "uint32_t");
    else if (bits <= 64) buf_puts(b, "uint64_t");
    else                 buf_puts(b, "unsigned __int128");
}

static bool num_bin_c(Buf *b, Type *ty, int op, const char *lhs, const char *rhs) {
    if (!ty) return false;
    /* 未定型整数字面量（comptime_int）在 C 里就是 int64_t：同样要按 64 位
     * 掩码/回绕，否则 `1 << 32` 直接落进 C 的 UB（右操作数 ≥ 宽度），
     * 打印出来是垃圾值。 */
    bool comptime = (ty->kind == TY_COMPTIME_INT);
    if (ty->kind != TY_INT && !comptime) return false;
    int bits = comptime ? 64 : ty->bits;
    bool sgn = comptime ? true : ty->is_signed;
    int mask = bits - 1;

    if (op == TOK_SLASH || op == TOK_PERCENT) {
        g_need_div = true;
        buf_printf(b, "zx_%s_%s%d(%s, %s)",
                   op == TOK_SLASH ? "div" : "mod", sgn ? "i" : "u", bits, lhs, rhs);
        return true;
    }
    if (op == TOK_PLUS || op == TOK_MINUS || op == TOK_STAR) {
        /* 有符号靠"无符号算 + 回转"避开 C 的有符号溢出 UB；无符号同样要回转，
         * 否则 u8/u16 的结果会被整数提升带出位宽（spec R10 明确禁止）。 */
        Buf ct; buf_init(&ct); ctype(&ct, ty);
        Buf wt; buf_init(&wt); wtype_of_bits(&wt, bits);
        buf_printf(b, "((%s)((%s)(%s) %s (%s)(%s)))",
                   ct.data, wt.data, lhs, cop(op), wt.data, rhs);
        return true;
    }
    if (op == TOK_SHL) {
        /* 左移在 C 里是最容易踩 UB 的一个：负值、或结果溢出符号位都算 UB。
         * 统一在无符号加宽类型里移、再截回目标位宽；移位量按 bits-1 掩码。 */
        Buf ct; buf_init(&ct); ctype(&ct, ty);
        Buf wt; buf_init(&wt); wtype_of_bits(&wt, bits);
        buf_printf(b, "((%s)((%s)(%s) << ((%s) & %d)))",
                   ct.data, wt.data, lhs, rhs, mask);
        return true;
    }
    if (op == TOK_SHR) {
        /* 算术右移：有符号数在 C 里是"实现定义"，但 GCC/Clang/MSVC 一致；
         * 为了不依赖它，有符号仍走带符号加宽类型（符号位照抄）。 */
        Buf ct; buf_init(&ct); ctype(&ct, ty);
        Buf wt; buf_init(&wt);
        if (sgn) {
            if (bits <= 32)      buf_puts(&wt, "int32_t");
            else if (bits <= 64) buf_puts(&wt, "int64_t");
            else                 buf_puts(&wt, "__int128");
        } else {
            wtype_of_bits(&wt, bits);
        }
        buf_printf(b, "((%s)((%s)(%s) >> ((%s) & %d)))",
                   ct.data, wt.data, lhs, rhs, mask);
        return true;
    }
    return false;
}

/* ---- `.?' 的解包辅助（G1）----
 *
 * 每个用到的可选/错误联合体类型配一个静态函数：单次求值、失败即 trap。
 * 不写成宏是因为 `?T` 是结构体，宏要拆两次求值；不写成全局函数是因为
 * payload 的类型每处不同。名字按类型名的规则生成，天然唯一。
 *
 * 它们进共享头（各单元各得一份 static 副本，链接时互不干扰）。 */
static Type **g_unw;
static int    g_nuw, g_cuw;

static void unwrap_need(Type *t) {
    if (!t) return;
    Buf mine; buf_init(&mine);
    tn(&mine, t);
    for (int i = 0; i < g_nuw; i++) {
        Buf o; buf_init(&o);
        tn(&o, g_unw[i]);
        if (!strcmp(o.data, mine.data)) return;   /* 同一个 C 类型只要一份 */
    }
    if (g_nuw == g_cuw) {
        g_cuw = g_cuw ? g_cuw * 2 : 8;
        g_unw = realloc(g_unw, sizeof(Type *) * (size_t)g_cuw);
    }
    g_unw[g_nuw++] = t;
}

static void emit_unwrap_runtime(void) {
    for (int i = 0; i < g_nuw; i++) {
        Type *t = g_unw[i];
        Buf nm; buf_init(&nm); tn(&nm, t);
        Buf ct; buf_init(&ct); ctype(&ct, t->elem);
        if (t->kind == TY_OPT && ty_opt_flat(t)) {
            Buf pt; buf_init(&pt); ctype(&pt, t);   /* ?*T：参数类型就是指针本身 */
            buf_printf(g_out,
                "static %s zx_unwrap_%s(%s o, const char *w) {\n"
                "    if (!o) { zx_diagf(\"zx: unwrap of a null optional at %%s\\n\", w); zx_abort_reported(); }\n"
                "    return o;\n"
                "}\n\n", ct.data, nm.data, pt.data);
        } else if (t->kind == TY_OPT) {
            buf_printf(g_out,
                "static %s zx_unwrap_%s(%s o, const char *w) {\n"
                "    if (!o.tag) { zx_diagf(\"zx: unwrap of a null optional at %%s\\n\", w); zx_abort_reported(); }\n"
                "    return o.payload;\n"
                "}\n\n", ct.data, nm.data, nm.data);
        } else {
            buf_printf(g_out,
                "static %s zx_unwrap_%s(%s o, const char *w) {\n"
                "    if (o.tag) { zx_diagf(\"zx: unwrap of error %%u at %%s\\n\", (unsigned)o.tag, w); zx_abort_reported(); }\n"
                "    return o.payload;\n"
                "}\n\n", ct.data, nm.data, nm.data);
        }
    }
}

/* 把 "文件:行" 变成一个可嵌入 C 的字符串字面量 */
static void where_lit(Buf *b, int line) {
    char pos[512];
    snprintf(pos, sizeof pos, "%s:%d", g_srcpath ? g_srcpath : "?", line);
    cstr(b, pos, (int)strlen(pos));
}


/* §216：把非简单基址切片下标的助手实现落到 zxrt 单元。原型已在
 * idx_helper_add() 里登记到共享头，各 TU 直接调用。 */
static void emit_idx_runtime(void) {
    for (int i = 0; i < g_idx_n; i++) {
        Type *sl = g_idx_types[i];
        Buf sn; buf_init(&sn); tn(&sn, sl);
        Buf ct; buf_init(&ct); ctype(&ct, sl->elem);
        buf_printf(g_out,
            "%s *zx_idx_%s(%s s, long long i, const char *loc) {\n"
            "    return s.ptr + zx_bound_s(i, s.len, loc);\n"
            "}\n\n"
            "%s *zx_idxu_%s(%s s, uint64_t i, const char *loc) {\n"
            "    return s.ptr + zx_bound(i, s.len, loc);\n"
            "}\n\n",
            ct.data, sn.data, sn.data, ct.data, sn.data, sn.data);
        buf_free(&sn); buf_free(&ct);
    }
}

static void emit_bounds_runtime(void) {
    bline(&g_bproto, "static size_t zx_bound(size_t i, size_t n, const char *loc);");
    bline(&g_bproto, "static size_t zx_bound_s(long long i, size_t n, const char *loc);");
    bline(&g_bproto, "static size_t zx_slice_len(long long len, size_t total, const char *loc);");
    bline(&g_bproto, "static int64_t zx_f2i64(double v, int bits);");
    bline(&g_bproto, "static uint64_t zx_f2u64(double v, int bits);");
    bline(&g_bproto, "static unsigned __int128 zx_f2i128(double v, int bits, int sign);");
    buf_puts(g_out,
        "static size_t zx_bound(size_t i, size_t n, const char *loc) {\n"
        "#ifdef ZX_RELEASE\n"
        "    (void)n; (void)loc; return i;\n"
        "#else\n"
        "    if (i >= n) {\n"
        "        zx_diagf(\"zx: index %llu is out of bounds (len %llu) at %s\\n\",\n"
        "                (unsigned long long)i, (unsigned long long)n, loc);\n"
        "        zx_abort_reported();\n"
        "    }\n"
        "    return i;\n"
        "#endif\n"
        "}\n\n"
        "static size_t zx_bound_s(long long i, size_t n, const char *loc) {\n"
        "#ifdef ZX_RELEASE\n"
        "    (void)n; (void)loc; return (size_t)i;\n"
        "#else\n"
        "    if (i < 0 || (unsigned long long)i >= n) {\n"
        "        zx_diagf(\"zx: index %lld is out of bounds (len %llu) at %s\\n\",\n"
        "                i, (unsigned long long)n, loc);\n"
        "        zx_abort_reported();\n"
        "    }\n"
        "    return (size_t)i;\n"
        "#endif\n"
        "}\n\n"
        "static size_t zx_slice_len(long long len, size_t total, const char *loc) {\n"
        "#ifdef ZX_RELEASE\n"
        "    (void)total; (void)loc; return len;\n"
        "#else\n"
        "    /* hi < lo 时长度是负的；参数收 long long，报出来的就是负数，\n"
        "     * 不再是 2^64-2 这种重解释出来的巨大值 */\n"
        "    if (len < 0 || (unsigned long long)len > total) {\n"
        "        zx_diagf(\"zx: slice is out of bounds (len %lld > total %llu) at %s\\n\",\n"
        "                len, (unsigned long long)total, loc);\n"
        "        zx_abort_reported();\n"
        "    }\n"
        "    return len;\n"
        "#endif\n"
        "}\n\n");
}

/* 浮点 → 整数 cast 的运行时检查。边界与 sema.c 的 f64_fits_int 一致，
 * 编译期与运行时同一答案（R10）。 */
static void emit_f2i_runtime(void) {
    buf_puts(g_out,
        "static int64_t zx_f2i64(double v, int bits) {\n"
        "    double hi = 1.0;\n"
        "    for (int i = 0; i < bits; i++) hi *= 2.0;\n"
        "    double lo = -(hi / 2.0); hi = hi / 2.0;\n"
        "#ifdef ZX_RELEASE\n"
        "    (void)lo; (void)hi;\n"
        "#else\n"
        "    if (!(v >= lo && v < hi)) {\n"
        "        zx_diagf(\"zx: float-to-int cast is out of range (%g)\\n\", v);\n"
        "        zx_abort_reported();\n"
        "    }\n"
        "#endif\n"
        "    return (int64_t)v;\n"
        "}\n\n"
        "static uint64_t zx_f2u64(double v, int bits) {\n"
        "    double hi = 1.0;\n"
        "    for (int i = 0; i < bits; i++) hi *= 2.0;\n"
        "#ifdef ZX_RELEASE\n"
        "    (void)hi;\n"
        "#else\n"
        "    if (!(v >= 0.0 && v < hi)) {\n"
        "        zx_diagf(\"zx: float-to-int cast is out of range (%g)\\n\", v);\n"
        "        zx_abort_reported();\n"
        "    }\n"
        "#endif\n"
        "    return (uint64_t)v;\n"
        "}\n\n"
        "static unsigned __int128 zx_f2i128(double v, int bits, int sign) {\n"
        "    double hi = 1.0;\n"
        "    for (int i = 0; i < bits; i++) hi *= 2.0;\n"
        "    double lo = 0.0;\n"
        "    if (sign) { lo = -(hi / 2.0); hi = hi / 2.0; }\n"
        "#ifdef ZX_RELEASE\n"
        "    (void)lo; (void)hi; (void)sign;\n"
        "#else\n"
        "    if (!(v >= lo && v < hi)) {\n"
        "        zx_diagf(\"zx: float-to-int cast is out of range (%g)\\n\", v);\n"
        "        zx_abort_reported();\n"
        "    }\n"
        "#endif\n"
        "    if (sign) return (unsigned __int128)(__int128)v;\n"
        "    return (unsigned __int128)v;\n"
        "}\n\n");
}

static void emit_div_runtime(void) {
    bline(&g_bproto, "static void zx_div0(void);");
    buf_puts(g_out,
        "static void zx_div0(void) {\n"
        "    zx_diagf(\"zx: division by zero\\n\");\n"
        "    zx_abort_reported();\n"
        "}\n\n");
    static const struct { int bits; bool sgn; } all[] = {
        {8, false}, {16, false}, {32, false}, {64, false}, {128, false},
        {8, true},  {16, true},  {32, true},  {64, true},  {128, true},
    };
    for (size_t i = 0; i < sizeof all / sizeof *all; i++) {
        int bits = all[i].bits;
        bool sgn = all[i].sgn;
        char line1[400];
        if (sgn) {
            /* b == -1：除法结果回绕到自身（i8 min / -1 = i8 min），取模得 0 */
            const char *ct = bits == 128 ? "__int128" : bits == 64 ? "int64_t" :
                             bits == 32 ? "int32_t" : bits == 16 ? "int16_t" : "int8_t";
            const char *ut = bits == 128 ? "unsigned __int128" : bits == 64 ? "uint64_t" :
                             bits == 32 ? "uint32_t" : bits == 16 ? "uint16_t" : "uint8_t";
            snprintf(line1, sizeof line1,
                "static %s zx_div_i%d(%s a, %s b) { if (!b) zx_div0(); "
                "if (b == -1) return (%s)((%s)0u - (%s)a); return (%s)(a / b); }",
                ct, bits, ct, ct, ct, ut, ut, ct);
            buf_printf(g_out, "%s\n", line1);
            snprintf(line1, sizeof line1,
                "static %s zx_mod_i%d(%s a, %s b) { if (!b) zx_div0(); "
                "if (b == -1) return 0; return (%s)(a %% b); }",
                ct, bits, ct, ct, ct);
            buf_printf(g_out, "%s\n", line1);
        } else {
            const char *ct = bits == 128 ? "unsigned __int128" : bits == 64 ? "uint64_t" :
                             bits == 32 ? "uint32_t" : bits == 16 ? "uint16_t" : "uint8_t";
            snprintf(line1, sizeof line1,
                "static %s zx_div_u%d(%s a, %s b) { if (!b) zx_div0(); return (%s)(a / b); }",
                ct, bits, ct, ct, ct);
            buf_printf(g_out, "%s\n", line1);
            snprintf(line1, sizeof line1,
                "static %s zx_mod_u%d(%s a, %s b) { if (!b) zx_div0(); return (%s)(a %% b); }",
                ct, bits, ct, ct, ct);
            buf_printf(g_out, "%s\n", line1);
        }
    }
}

/* i128/u128 转十进制（M19）：printf 没有 128 位动词 */
static void emit_i128_runtime(void) {
    bline(&g_bproto, "static void zx_u128_to(char *b, unsigned __int128 v);");
    bline(&g_bproto, "static const char *zx_i128_str(char *b, __int128 v);");
    bline(&g_bproto, "static const char *zx_u128_str(char *b, unsigned __int128 v);");
    buf_puts(g_out,
        "static void zx_u128_to(char *b, unsigned __int128 v) {\n"
        "    char tmp[48]; int i = 0;\n"
        "    if (v == 0) { b[0] = '0'; b[1] = 0; return; }\n"
        "    while (v) { tmp[i++] = (char)('0' + (int)(v % 10)); v /= 10; }\n"
        "    for (int j = 0; j < i; j++) b[j] = tmp[i - 1 - j];\n"
        "    b[i] = 0;\n"
        "}\n"
        "static const char *zx_i128_str(char *b, __int128 v) {\n"
        "    unsigned __int128 m = (unsigned __int128)v;\n"
        "    if (v < 0) { b[0] = '-'; zx_u128_to(b + 1, ~m + 1); }\n"
        "    else zx_u128_to(b, m);\n"
        "    return b;\n"
        "}\n"
        "static const char *zx_u128_str(char *b, unsigned __int128 v) { zx_u128_to(b, v); return b; }\n\n");
}

/* 折叠过的标量：直接把编译期值写成字面量。
 *
 * `@type(T).align` 这类表达式在语义层已经算出值了，但它在 C 里长得像
 * "读一块全零描述符的字段"——照结构访问发出去会得到 0。谁折算了就用谁的结果。 */
static bool ex_ct_scalar(Buf *b, Node *n) {
    Value *v = n->ct;
    if (!v || !n->ty) return false;
    switch (v->kind) {
    case V_BOOL:  buf_puts(b, v->bval ? "true" : "false"); return true;
    case V_FLOAT:
        /* f32 的编译期值必须**以 float 身份**落进 C：裸 `%.17g` 是 double
         * 字面量，跟 float 比较/参与运算时会被提升成 double，值和运行时
         * 差 1 ULP（`(f32)(0.1+0.2) == (f32)0.3` 编译期 false）。 */
        if (n->ty && n->ty->kind == TY_FLOAT && n->ty->bits == 32) buf_puts(b, "(float)");
        buf_printf(b, "%.17g", v->fval);
        return true;
    case V_INT:
        if (v->big) {
            if (!n->ty || n->ty->kind != TY_INT || n->ty->bits != 128) return false;
            emit_big128(b, v->hi, v->ival, v->neg);
            return true;
        }
        if (v->neg) buf_printf(b, "(-%lluLL)", (unsigned long long)v->ival);
        else        buf_printf(b, "%lluULL", (unsigned long long)v->ival);
        return true;
    case V_STR: {
        Buf s;  buf_init(&s);
        Buf ty; buf_init(&ty);
        cstr(&s, v->sval ? v->sval : "", v->slen);
        ctype(&ty, n->ty);
        buf_printf(b, "((%s){ (uint8_t*)%s, %d })", ty.data, s.data, v->slen);
        return true;
    }
    default: return false;      /* 聚合值仍按原路发（有自己的初始化逻辑） */
    }
}

static void ex(Buf *b, Node *n) {
    if (!n) { buf_puts(b, "0"); return; }
    bmap_note(b, n);

    /* 已折叠成标量的值：直接发字面量（`info.kind == .struct_` 这种
     * 泛型里的编译期判断要靠它，否则会发成读一块全零描述符）。
     * 内建除外——它们有的要发自己的形状（@typeName / @fieldName）。 */
    /* §209：基址含运行期来源（返回指针的调用 / unwrap）时，ct 是 sema
     * 越界折出的初值镜像（0），不能当常量发——否则 `getCell().0.v` 发成
     * `0ULL.v`。让它走下面的正常字段发射（真指针解引用）。 */
    if (n->kind == NK_FIELD && n->ct && !ct_runtime_tainted(n) &&
        ex_ct_scalar(b, n)) return;

    switch (n->kind) {
    case NK_INT: {
        if (n->flag && n->str) {            /* 超出 u64：按 128 位字面量发（M19） */
            uint64_t hi, lo;
            val_big_split(n->str, &hi, &lo);
            emit_big128(b, hi, lo, false);
            return;
        }
        if (n->ival <= 0x7fffffffULL) buf_printf(b, "%llu", (unsigned long long)n->ival);
        else buf_printf(b, "%lluULL", (unsigned long long)n->ival);
        return;
    }
    case NK_FLOAT: {
        Buf t; buf_init(&t);
        buf_printf(&t, "%.17g", n->fval);
        if (!strpbrk(t.data, ".eEnN")) buf_puts(&t, ".0");
        if (n->ty && n->ty->kind == TY_FLOAT && n->ty->bits == 32) buf_puts(&t, "f");
        buf_puts(b, t.data);
        return;
    }
    case NK_STR: {
        Buf s; buf_init(&s);
        cstr(&s, n->str ? n->str : "", n->str_len);
        /* 字符串字面量的类型是 *[N]const u8（用户 2026-11-09）：C 侧就是
         * 指向字节的指针，长度留在 Zx 类型里（.len 编译期算）。落点要切片时
         * 由 coerce 的 "*[N]T → []T" 包装成 {ptr,len}。 */
        if (n->ty && n->ty->kind == TY_PTR) {
            buf_printf(b, "((uint8_t*)%s)", s.data);
            return;
        }
        /* 兜底：没有落点类型的字面量仍按切片发射（个别内建调用直接要
         * ZxSlice_u8），确保 typedef 已发射。 */
        ensure_type(ty_slice(ty_u8(), true));
        buf_printf(b, "((ZxSlice_u8){ (uint8_t*)%s, %d })", s.data, n->str_len);
        return;
    }
    case NK_CHAR:
        /* 类型由 sema 定（落点 i32 就是 i32）。硬写 uint8_t 会把 `'中'`
         * 这种多字节字符截成低位字节。 */
        buf_puts(b, "((");
        ctype(b, (n->ty && n->ty->kind == TY_INT) ? n->ty : ty_u8());
        buf_printf(b, ")%llu)", (unsigned long long)n->ival);
        return;
    case NK_BOOL:
        buf_puts(b, n->ival ? "true" : "false");
        return;
    case NK_NULL:
        /* 无 coerce 直接发射的情形（比较的另一侧、`null == null`）：
         * 发 0——它同时是合法的空指针常量与 false，`p == false` 那种
         * 指针比 bool 的代码就不会出现了。类型是 error 的例外：那是
         * 保留码 65535，发 0 会把"空"比成"没有错误"。 */
        buf_puts(b, (n->ty && n->ty->kind == TY_ERROR) ? "65535" : "0");
        return;
    case NK_UNDEF:
        buf_puts(b, "0");
        return;
    case NK_IDENT: {
        /* 泛型的编译期参数（`n: usize`）在 C 里没有变量：直接用它的值。
         * 这些符号没有 decl —— 有 decl 的常量都有自己的 C 名字。 */
        Value *cv = sema_sym_has_storage(n->sym) ? NULL : sema_sym_ct(n->sym);
        if (cv && cv->kind == V_INT) {
            if (cv->neg) buf_printf(b, "(-%llu)", (unsigned long long)cv->ival);
            else         buf_printf(b, "%llu", (unsigned long long)cv->ival);
            return;
        }
        if (cv && cv->kind == V_FLOAT) {
            /* 同 ex_ct_scalar：f32 常量要带上 float 身份 */
            if (n->ty && n->ty->kind == TY_FLOAT && n->ty->bits == 32) buf_puts(b, "(float)");
            buf_printf(b, "%.17g", cv->fval);
            return;
        }
        if (cv && cv->kind == V_BOOL)  { buf_puts(b, cv->bval ? "true" : "false"); return; }
        /* 局部 fn 绑定的引用（当值传 / 被调用）：C 层函数是 <cname>_impl
         * （gen_local_fns 发的），不是变量槽。词法上内层函数能看到外层
         * 函数的局部绑定，C 层是两个独立函数——变量槽跨不过去，直接
         * 引用会变成"未声明函数"的隐式声明（s09 压测首次暴露）。 */
        {
            Node *dn = (n->sym && sema_sym_in_fn(n->sym))
                     ? sema_sym_decl(n->sym) : NULL;
            if (dn && dn->kind == NK_DECL && dn->b && dn->b->kind == NK_FN &&
                dn->b->b && !fn_is_generic(dn->b->b)) {
                char *inm = localfn_impl_name(dn);
                need_by_cname(inm);
                buf_puts(b, inm);
                free(inm);
                return;
            }
        }
        /* 容器体内裸名引用同容器的静态函数（§139）：定义发的是实例名
         * `a_i<n>`，引用点必须发同一个，否则 C 那边就是"未声明的函数"。 */
        if (n->inst) { buf_puts(b, inst_name(n->inst, n->name)); return; }
        /* §151：编译期判定为「大类型」的 `Self` 接收者，C 形参发的是
         * `const T *self`，而方法体里 self 仍是值语义（sema 保留形参符号的
         * ty 为 `Self`）。补一层解引用，`self.x` 就落成 `(*self).x`。 */
        if (n->sym && sema_sym_self_by_ptr(n->sym)) {
            need_by_cname(sym_cname(n));
            buf_puts(b, "(*");
            emit_sym_name(b, n);
            buf_putc(b, ')');
            return;
        }
        need_by_cname(sym_cname(n));
        emit_sym_name(b, n);
        return;
    }

    case NK_FIELD: {
        /* 方法（p.f 没被调用时才会走到这儿）：它就是那个 C 函数 */
        if (n->inst) { buf_puts(b, inst_name(n->inst, n->name)); return; }
        /* 模块成员（util.add / geo.Point.…）：直接用它自己的 C 名字 */
        if (n->sym) {
            const char *cn = sym_cname(n);
            need_by_cname(cn);
            emit_sym_name(b, n);
            return;
        }
        Type *bt = n->a ? n->a->ty : NULL;
        if (bt && bt->kind == TY_TYPE && n->a->type_val) {
            /* 类型上的成员：枚举变体 / 静态成员 */
            Buf nm; buf_init(&nm);
            tn(&nm, n->a->type_val);
            buf_printf(b, "%s_", nm.data);
            cname(b, n->name);
            return;
        }
        /* §118 字段值指针：f.ptr —— @type(左值) 描述符的 ptr 槽。
         * 发 ((fldty *)((char *)&(base) + off))：指向这个实例的字段。
         * f.ptr.0 解引用、赋值、传给 *T 形参都是普通 C 指针语义。
         * 基址克隆在 n->b、字节偏移在 n->ival（检查期放好的）。 */
        if (n->ty && n->ty->kind == TY_PTR && n->ty->is_fldptr && n->b) {
            Buf et; buf_init(&et);
            ctype(&et, n->ty->elem);
            buf_printf(b, "((%s *)((char *)&(", et.data);
            ex(b, n->b);
            buf_printf(b, ") + %llu))", (unsigned long long)n->ival);
            return;
        }
        /* §118 续：statics 的 ptr 裸用（没跟 .0）——静态成员全被物化成
         * C 全局，这就是 (&全局) 的真指针；const 成员的 *const 由 ctype
         * 表达。`.0` 已在检查期改写成 T.<成员>，走不到这里。 */
        if (n->ty && n->ty->kind == TY_PTR && n->ty->is_fldptr && !n->b &&
            n->type_val) {
            Type *ct3 = n->type_val;
            Field *sf = ct3->kind == TY_ENUM
                      ? (n->ival < (uint64_t)ct3->nstatics ? &ct3->statics[n->ival] : NULL)
                      : (n->ival < (uint64_t)ct3->nfields ? &ct3->fields[n->ival] : NULL);
            if (sf && sf->name) {
                Buf sn2; buf_init(&sn2);
                tn(&sn2, ct3);
                buf_printf(b, "(&%s_", sn2.data);
                cname(b, sf->name);
                buf_putc(b, ')');
                return;
            }
        }
        /* 编译期数量 / 大小 */
        if (bt && n->name) {
            /* 聚合体自己有同名字段时，**字段优先**：`.size` 对数组/元组是
             * 编译期字节数，但对 `struct { size: i32 }` 就是那个字段。
             * sema 一直是这么判的（结构体走普通字段查找），后端以前无条件
             * 发 `layout_size`，于是 `h.size` 静默变成 `sizeof(h)`——
             * 编译过、答案是 8。两边必须用同一条规则判。 */
            /* §197：裸指针 *S 没有 .size/.len 内建——穿透一层查 S 的字段，
             * 命中就走普通字段访问（下面 TY_PTR 分支发 p->size）。与
             * check_field / ct_field 同口径：elem 有同名字段即字段优先。 */
            Type *probe = bt;
            if (probe && probe->kind == TY_PTR && !probe->has_len &&
                !ty_is_array_ptr(probe)) probe = probe->elem;
            bool shadowed = (probe->kind == TY_STRUCT || probe->kind == TY_UNION) &&
                            ty_field(probe, n->name) != NULL;
            bool is_len = !shadowed && !strcmp(n->name, "len");
            bool is_size = !shadowed && !strcmp(n->name, "size");
            if (is_size) {
                /* §201：数组值 [N]T 的大小已是 N×元素 */
                if (bt->kind == TY_ARRAY) {
                    int es = ty_size(bt);
                    if (es >= 0) { buf_printf(b, "((size_t)%d)", es); return; }
                }
                /* §201：字符串字面量的 .size 按字节数折；普通 *[N]T 没有
                 * .size 内建——它是普通指针，先 p[0] 取数组再问大小。 */
                if (bt->kind == TY_PTR && bt->has_len && n->a &&
                    n->a->kind == NK_STR) {
                    buf_printf(b, "((size_t)%llu)", (unsigned long long)bt->len);
                    return;
                }
                /* §154：[]T 的 .size 是运行期的 `len * sizeof(elem)`，不是
                 * 描述符自己的 16 字节。sema 的 ct_field 也是这么折的——
                 * 类型检查 / 常量折叠 / 代码生成三处必须同一条规则。 */
                if (bt->kind == TY_SLICE && bt->elem) {
                    int es = ty_size(bt->elem);
                    if (es >= 0) {
                        buf_printf(b, "((size_t)(");
                        ex(b, n->a);
                        buf_printf(b, ".len) * (size_t)%d)", es);
                        return;
                    }
                }
                int sz = layout_size(bt);
                if (sz >= 0) { buf_printf(b, "((size_t)%d)", sz); return; }
            }
            if (is_len) {
                /* §201：数组值 [N]T */
                if (bt->kind == TY_ARRAY) {
                    buf_printf(b, "((size_t)%llu)", (unsigned long long)bt->len);
                    return;
                }
                /* 字符串字面量保留 .len；*[N]T 普通指针不再有这个内建。 */
                if (bt->kind == TY_PTR && bt->has_len && n->a &&
                    n->a->kind == NK_STR) {
                    buf_printf(b, "((size_t)%llu)", (unsigned long long)bt->len);
                    return;
                }
                if (bt->kind == TY_TUPLE) {
                    buf_printf(b, "((size_t)%d)", bt->nfields);
                    return;
                }
                if (bt->kind == TY_VEC) {
                    /* §162：分量数与数组一样写在类型里，是编译期常量 */
                    buf_printf(b, "((size_t)%llu)", (unsigned long long)bt->len);
                    return;
                }
            }
        }
        if (!n->name) {                    /* .N */
            if (ty_is_array_ptr(bt)) {
                ex(b, n->a);
                buf_printf(b, "[%llu]", (unsigned long long)n->ival);
                return;
            }
            if (bt && bt->kind == TY_PTR) {
                buf_puts(b, "(*");
                ex(b, n->a);
                buf_puts(b, ")");
                return;
            }
            if (bt && bt->kind == TY_TUPLE) {
                buf_printf(b, "(");
                ex(b, n->a);
                buf_printf(b, ").f%llu", (unsigned long long)n->ival);
                return;
            }
            if (bt && bt->kind == TY_VEC) {
                /* v.0 / v.3：编译期分量，C 的向量类型本来就支持下标 */
                buf_putc(b, '(');
                ex(b, n->a);
                buf_printf(b, ")[%llu]", (unsigned long long)n->ival);
                return;
            }
            if (bt && bt->kind == TY_SLICE) {
                buf_puts(b, "(*");
                ex(b, n->a);
                buf_puts(b, ".ptr)");
                return;
            }
        }
        /* §197：字符串字面量的 .ptr 是字面量语法糖，基址本身已经是首字节
         * 指针（ex(NK_STR) 发 `((uint8_t*)"...")`）。§201：普通 *[N]T 没有
         * .ptr 内建——p[0] / p.0 取出的整个数组才有 .ptr。 */
        if (bt && bt->kind == TY_PTR && bt->has_len && n->name &&
            !strcmp(n->name, "ptr") && n->a && n->a->kind == NK_STR) {
            ex(b, n->a);
            return;
        }
        if (bt && bt->kind == TY_SLICE) {
            ex(b, n->a);
            buf_puts(b, !strcmp(n->name, "ptr") ? ".ptr" : ".len");
            return;
        }
        if (bt && (bt->kind == TY_OPT || bt->kind == TY_ERR)) {
            bool flat = bt->kind == TY_OPT && ty_opt_flat(bt);
            ex(b, n->a);
            if (flat)
                buf_puts(b, !strcmp(n->name, "tag") ? " != 0" : "");
            else
                buf_puts(b, !strcmp(n->name, "tag") ? ".tag" : ".payload");
            return;
        }
        /* §201：数组值 [N]T 的 .ptr 取首元素地址。C 表示是 struct{ T v[N]; }，
         * 必须取 &base.v[0]；&base.v 的类型是 T(*)[N]，不是 T*。 */
        if (bt && bt->kind == TY_ARRAY && n->name && !strcmp(n->name, "ptr")) {
            buf_puts(b, "&(");
            ex(b, n->a);
            buf_puts(b, ").v[0]");
            return;
        }
        if (bt && bt->kind == TY_PTR) {
            Type *e = bt->elem;
            ex(b, n->a);
            if (e && e->kind == TY_UNION && n->name && strcmp(n->name, "tag")) {
                if (e->is_tagged) { buf_puts(b, "->u."); mname(b, n->name); }
                else arrowname(b, n->name);
            } else {
                arrowname(b, n->name);
            }
            return;
        }
        if (bt && bt->kind == TY_UNION && n->name && strcmp(n->name, "tag")) {
            ex(b, n->a);
            if (bt->is_tagged) buf_puts(b, ".u");
            dotname(b, n->name);
            return;
        }
        ex(b, n->a);
        dotname(b, n->name);
        return;
    }

    case NK_INDEX:
        emit_idx_node(b, n);
        return;

    case NK_SLICE: {
        Type *bt = n->a->ty;
        Type *rt = n->ty;
        ensure_type(rt);
        Buf nm; buf_init(&nm); tn(&nm, rt);
        /* 长度检查要总长度：切片取 .len，*[N]T / [N]T 用类型里的 N，裸指针没有 */
        Buf tot; buf_init(&tot);
        bool is_slice_base = bt && bt->kind == TY_SLICE;
        bool is_arr_ptr = ty_is_array_ptr(bt);
        bool is_arr_val = bt && bt->kind == TY_ARRAY;
        const char *total_c = NULL;
        if (is_slice_base) {
            Buf tb; buf_init(&tb);
            ex(&tb, n->a);
            buf_printf(&tot, "(%s).len", tb.data);
            total_c = tot.data;
        }
        uint64_t total = is_arr_ptr ? bt->elem->len
                       : (is_arr_val || (bt && bt->has_len)) ? bt->len : 0;
        if (!is_slice_base && total > 0) {
            buf_printf(&tot, "%llu", (unsigned long long)total);
            total_c = tot.data;
        }
        bool can_chk = total_c && simple_bound_expr(n->b) && simple_bound_expr(n->c);

        Buf len; buf_init(&len);
        if (n->c) {
            Buf hb; buf_init(&hb); ex(&hb, n->c);
            Buf lb; buf_init(&lb); if (n->b) ex(&lb, n->b); else buf_puts(&lb, "0");
            buf_printf(&len, "%s - (%s)", hb.data, lb.data);
        } else {
            Buf lb; buf_init(&lb); if (n->b) ex(&lb, n->b); else buf_puts(&lb, "0");
            if (is_slice_base) buf_printf(&len, "%s - (%s)", tot.data, lb.data);
            else buf_printf(&len, "%lld - (%s)", (long long)total, lb.data);
        }

        if (is_slice_base) {
            buf_printf(b, "((%s){ (", nm.data);
            ex(b, n->a);
            buf_printf(b, ").ptr + ");
            if (n->b) ex(b, n->b); else buf_puts(b, "0");
            buf_puts(b, ", ");
            if (can_chk) wrap_slice_len(b, &len, n, total_c);
            else         buf_puts(b, len.data);
            buf_puts(b, " })");
            return;
        }
        /* §201：数组值 / 数组指针切片都要先拿首元素地址，C 基址是
         * struct{ T v[N]; }（或其指针），不能对整个数组做指针算术。 */
        if (is_arr_ptr || is_arr_val) {
            buf_printf(b, "((%s){ ((", nm.data);
            ctype(b, rt->elem);
            buf_puts(b, "*)&(");
            ex(b, n->a);
            buf_puts(b, is_arr_ptr ? ")->v) + " : ").v[0]) + ");
            if (n->b) ex(b, n->b); else buf_puts(b, "0");
            buf_puts(b, ", ");
            if (can_chk) wrap_slice_len(b, &len, n, total_c);
            else         buf_puts(b, len.data);
            buf_puts(b, " })");
            return;
        }
        buf_printf(b, "((%s){ (", nm.data);
        ex(b, n->a);
        buf_printf(b, ") + ");
        if (n->b) ex(b, n->b); else buf_puts(b, "0");
        buf_puts(b, ", ");
        if (can_chk) wrap_slice_len(b, &len, n, total_c);
        else         buf_puts(b, len.data);
        buf_puts(b, " })");
        return;
    }

    case NK_UN: {
        switch (n->op) {
        case TOK_AMP:
            if (n->a->kind == NK_ARRAY_LIT || (n->a->kind == NK_UN && n->a->op == TOK_AMP)) {
                ex(b, n->a);
                return;
            }
            buf_puts(b, "&(");
            lvalue(b, n->a);
            buf_puts(b, ")");
            return;
        case TOK_MINUS: buf_puts(b, "(-"); ex(b, n->a); buf_puts(b, ")"); return;
        case TOK_BANG:  buf_puts(b, "(!"); ex(b, n->a); buf_puts(b, ")"); return;
        case TOK_TILDE: buf_puts(b, "(~"); ex(b, n->a); buf_puts(b, ")"); return;
        default:        buf_puts(b, "(");  ex(b, n->a); buf_puts(b, ")"); return;
        }
    }

    case NK_UNWRAP: {
        Type *t = n->a->ty;
        if (!t || (t->kind != TY_OPT && t->kind != TY_ERR)) { ex(b, n->a); return; }
        unwrap_need(t);
        Buf nm; buf_init(&nm); tn(&nm, t);
        Buf w;  buf_init(&w);  where_lit(&w, n->line);
        buf_printf(b, "zx_unwrap_%s(", nm.data);
        ex(b, n->a);
        buf_printf(b, ", %s)", w.data);
        return;
    }

    /* `try e`：e 是 !T。先把整个联合体落进临时量（只求值一次），
     * 然后"出错就带着同一个 tag 返回"。payload 在出错时无意义，
     * 所以返回值的做法是 `{0}` 起一个干净值、只拷 tag——这样 `!void`
     * （没有 payload 字段）也走得通。 */
    case NK_TRY: {
        Type *t = n->a->ty;
        if (!t || (t->kind != TY_ERR && t->kind != TY_OPT)) { ex(b, n->a); return; }
        Type *ret = g_cur_ret;
        if (!ret || ret->kind != TY_ERR) { ex(b, n->a); return; }

        static int tn_;
        char tmp[64], rv[64];
        snprintf(tmp, sizeof tmp, "_ty%d", tn_);
        snprintf(rv,  sizeof rv,  "_tyr%d", tn_);
        tn_++;

        if (t->kind == TY_OPT) {
            /* `?T` 的 try（v0.26）：tag 是 bool，true=有值。空（null）抛
             * 保留错误码 error.null ≡ 65535（§5），沿同一条传播路上行。
             * ?*T（M26）：没有标签，判指针本身。 */
            bool flat = ty_opt_flat(t);
            line("%s %s = %s;", cty(t), tmp, cxs(n->a, t));
            if (flat) line("if (!%s) {", tmp);
            else      line("if (!%s.tag) {", tmp);
            g_ind++;
            if (g_is_main) {
                run_defers_m(0, 1, NULL, "65535");
                /* main 交出去的是 int：error.null 的退出码就是 65535（OS 截 8 位） */
                line("return (int)(65535);");
            } else {
                line("%s %s = {0};", cty(ret), rv);
                line("%s.tag = 65535;", rv);
                run_defers_m(0, 1, NULL, "65535");
                line("return %s;", rv);
            }
            g_ind--;
            line("}");
            if (t->elem && t->elem->kind == TY_VOID) buf_puts(b, "(void)0");
            else if (flat) buf_puts(b, tmp);
            else buf_printf(b, "%s.payload", tmp);
            return;
        }

        line("%s %s = %s;", cty(t), tmp, cxs(n->a, t));
        line("if (%s.tag != 0) {", tmp);
        char ev[72];
        snprintf(ev, sizeof ev, "%s.tag", tmp);
        g_ind++;
        if (g_is_main) {
            /* main 交出去的是 int：标签本身就是错误码，原样当退出码用 */
            run_defers_m(0, 1, NULL, ev);
            main_exit(tmp);
        } else {
            line("%s %s = {0};", cty(ret), rv);
            line("%s.tag = %s.tag;", rv, tmp);
            run_defers_m(0, 1, NULL, ev);
            line("return %s;", rv);
        }
        g_ind--;
        line("}");
        /* `try g()` 作语句时（`!void`），载荷没有值可读——发个 `(void)0`
         * 让表达式语句合法，别留下 `_ty1.payload;` 这种无用值警告。 */
        if (t->elem && t->elem->kind == TY_VOID) buf_puts(b, "(void)0");
        else buf_printf(b, "%s.payload", tmp);
        return;
    }

    /* §171 `try e err { … }`：出错不外抛。用一个临时槽收两臂的兜底值：
     * 成功写载荷，失败进块写兜底（块里的 err 绑到错误码）。整表达式的值
     * 就是该槽——和 gen_if_value 的两臂写槽同一套路子。 */
    case NK_TRY_CAPTURE: {
        Type *t = n->a->ty;
        Type *slot_ty = n->ty;
        if (!t || (t->kind != TY_ERR && t->kind != TY_OPT)) { ex(b, n->a); return; }
        static int tc_;
        char tmp[64], slot[64];
        snprintf(tmp,  sizeof tmp,  "_tcv%d", tc_);
        snprintf(slot, sizeof slot, "_tcr%d", tc_);
        tc_++;
        line("%s %s = %s;", cty(t), tmp, cxs(n->a, t));

        bool flat = t->kind == TY_OPT && ty_opt_flat(t);
        if (slot_ty && slot_ty->kind != TY_VOID)
            line("%s %s;", cty(slot_ty), slot);

        /* 成功分支：写载荷 */
        line(flat ? "if (%s) {" : (t->kind == TY_OPT ? "if (%s.tag) {" : "if (%s.tag == 0) {"), tmp);
        g_ind++;
        {
            CScope s; cscope_push(&s);
            if (slot_ty && slot_ty->kind != TY_VOID)
                line("%s = %s%s;", slot, tmp, (flat || t->elem->kind == TY_VOID) ? "" : ".payload");
            cscope_leave();
        }
        g_ind--;
        line("} else {");
        g_ind++;
        {
            CScope s; cscope_push(&s);
            /* 捕获名绑错误码：!T 是 .tag，?T 的空是保留码 65535。 */
            if (n->name) {
                const char *cpt = (!strcmp(n->name, "_")) ? ignored_cname() : cnames(n->name);
                if (t->kind == TY_OPT) line("%s %s = 65535;", cty(ty_error()), cpt);
                else                   line("%s %s = %s.tag;", cty(ty_error()), cpt, tmp);
            }
            gen_arm(n->b, slot_ty && slot_ty->kind != TY_VOID ? slot : NULL, slot_ty);
            cscope_leave();
        }
        g_ind--;
        line("}");
        if (slot_ty && slot_ty->kind != TY_VOID) buf_puts(b, slot);
        else buf_puts(b, "(void)0");
        return;
    }

    /* `a orelse b`：右边只在左边空/错时才求值，所以不能摊成一个 C 三元式
     * （那会把 b 提前算掉）。左边先落进临时量，保证只求值一次。 */
    case NK_ORELSE: {
        Type *t = n->a->ty;
        /* `null orelse x` 是类型错误（见 sema），这里只处理真正的 ?T / !T。
         * 留一条兜底：类型缺失时按左值发，别静默变成别的东西。 */
        if (!t || (t->kind != TY_OPT && t->kind != TY_ERR)) { ex(b, n->a); return; }
        bool flat = t->kind == TY_OPT && ty_opt_flat(t);
        const char *has = (t->kind == TY_OPT) ? (flat ? " != 0" : ".tag") : ".tag == 0";
        const char *pay = flat ? "" : ".payload";
        if (!g_in_fn) {                    /* 顶层初始化器：只能老老实实写三元式 */
            buf_puts(b, "((");
            ex(b, n->a);
            buf_printf(b, ")%s ? (", has);
            ex(b, n->a);
            buf_printf(b, ")%s : (", pay);
            /* 右值按载荷类型落：`s orelse ""` 的 "" 是 *[0]const u8，
             * 三元式的另一支是切片，必须在这里包成切片。 */
            coerce(b, n->b, t->elem);
            buf_puts(b, "))");
            return;
        }
        static int on;
        char tmp[64];
        snprintf(tmp, sizeof tmp, "_or%d", on++);
        line("%s %s = %s;", cty(t), tmp, cxs(n->a, t));
        if (n->flag) {
            /* §200：兜底臂发散（末尾 `return` / `continue` / noreturn 调用 /
             * 无限循环）——它不产出值，是纯语句。 sema 已把这条 orelse 的
             * 类型定为左载荷 elem，别再拿 void 兜底臂去 coerce。
             * 发射等价于：左为空则跑右臂（其中的 return 直接退函数），
             * 能走到表达式位置的只有"左有值"，所以值就是左载荷。 */
            line("if (!((%s)%s)) {", tmp, has);
            g_ind++;
            {
                CScope s; cscope_push(&s);
                gen_arm(n->b, NULL, NULL);   /* slot=NULL：当语句发，保留 return */
                cscope_leave();
            }
            g_ind--;
            line("}");
            /* 值就是左载荷：扁平 ?T 是 tmp 本身，带标签的是 tmp.payload */
            buf_printf(b, "(%s%s)", tmp, pay);
        } else {
            buf_printf(b, "((%s)%s ? %s%s : (", tmp, has, tmp, pay);
            coerce(b, n->b, t->elem);
            buf_puts(b, "))");
        }
        return;
    }

    case NK_BIN: {
        const char *op = cop(n->op);
        if (!op) { ex(b, n->a); return; }
        /* §197：`opt == null` / `null == opt` 的运行时判定。非扁平 `?T` 在 C
         * 里是 `{payload, tag}` struct，不能整体 `== 0`（B1001）；语义是判 tag
         * （tag=true 有值）。扁平 `?*T` 是指针，走通用 `p == 0` 即可。
         * 用 `((expr).tag)`——C 对调用结果取字段只求值一次。 */
        if (n->op == TOK_EQ || n->op == TOK_NE) {
            Node *opt = NULL;
            Type *ot = NULL;
            if (n->a->ty && n->a->ty->kind == TY_OPT &&
                n->b && n->b->kind == NK_NULL) { opt = n->a; ot = n->a->ty; }
            else if (n->b->ty && n->b->ty->kind == TY_OPT &&
                     n->a && n->a->kind == NK_NULL) { opt = n->b; ot = n->b->ty; }
            if (opt && ot && !ty_opt_flat(ot)) {
                buf_puts(b, n->op == TOK_EQ ? "(!(" : "((");
                ex(b, opt);
                buf_puts(b, n->op == TOK_EQ ? ").tag)" : ").tag)");
                return;
            }
        }
        /* §162：向量比较。C 里 `a < b` 本来就给出逐分量的有符号整数向量
         * （0 / -1），但目标类型是 sema 算出来的掩码类型，写显式 cast 比
         * 让 C 的隐式转换去猜清楚。算术（+ - * /）不需要这层，直接逐分量。 */
        if (n->ty && n->ty->kind == TY_VEC &&
            (n->op == TOK_EQ || n->op == TOK_NE || n->op == TOK_LT ||
             n->op == TOK_LE || n->op == TOK_GT || n->op == TOK_GE)) {
            buf_puts(b, "((");
            ctype(b, n->ty);
            buf_puts(b, ")(");
            ex(b, n->a);
            buf_printf(b, " %s ", op);
            ex(b, n->b);
            buf_puts(b, "))");
            return;
        }
        /* 指针算术。C 里 `*[N]T` 是 `T(*)[N]`，直接 `+1` 会跳 N 个元素，
         * 所以先转成元素的 C 指针再加减——语义是"n 个元素"。 */
        if (n->ty && n->ty->kind == TY_PTR) {
            Type *et = n->ty->elem;
            bool aptr = n->a && n->a->ty && n->a->ty->kind == TY_PTR;
            Node *pv = aptr ? n->a : n->b;
            Node *ov = aptr ? n->b : n->a;
            buf_puts(b, "((");
            ctype(b, n->ty);
            buf_puts(b, ")((");
            ctype(b, et);
            buf_puts(b, "*)(");
            ex(b, pv);
            buf_printf(b, ") %s (", op);
            ex(b, ov);
            buf_puts(b, ")))");
            return;
        }
        /* p - q → 元素个数 */
        if (n->op == TOK_MINUS && n->a && n->a->ty && n->a->ty->kind == TY_PTR) {
            Type *et = n->a->ty->elem;
            buf_puts(b, "((size_t)((");
            ctype(b, et);
            buf_puts(b, "*)(");
            ex(b, n->a);
            buf_puts(b, ") - (");
            ctype(b, et);
            buf_puts(b, "*)(");
            ex(b, n->b);
            buf_puts(b, ")))");
            return;
        }
        /* M19 数值语义：整数算术/移位/除法的确定性行为（回绕、trap、掩码） */
        if (n->ty && (n->ty->kind == TY_INT || n->ty->kind == TY_COMPTIME_INT) &&
            (n->op == TOK_PLUS || n->op == TOK_MINUS || n->op == TOK_STAR ||
             n->op == TOK_SLASH || n->op == TOK_PERCENT ||
             n->op == TOK_SHL || n->op == TOK_SHR)) {
            Buf lb; buf_init(&lb);
            Buf rb; buf_init(&rb);
            ex(&lb, n->a);
            ex(&rb, n->b);
            if (num_bin_c(b, n->ty, n->op, lb.data, rb.data)) return;
            /* 不归数值语义管（浮点等），落回普通发射 */
        }
        buf_puts(b, "(");
        ex(b, n->a);
        buf_printf(b, " %s ", op);
        ex(b, n->b);
        buf_puts(b, ")");
        return;
    }

    case NK_CAST: {
        Type *to = n->ty;
        Type *vt = n->b ? n->b->ty : NULL;
        if (to && to->kind == TY_VEC && vt) {
            if (vt->kind == TY_VEC) {
                /* 向量之间只有同大小的重解释（sema 已保证），C 里必须写
                 * 显式 cast：隐式转换在 clang 上是错。 */
                buf_puts(b, "((");
                ctype(b, to);
                buf_puts(b, ")(");
                ex(b, n->b);
                buf_puts(b, "))");
                return;
            }
            if (vt->kind == TY_TUPLE) {
                /* §162：向量字面量 `@Vec(f32, 4){a, b, c, d}`（parse 脱糖
                 * 成"转换一个元组"）。落成复合字面量，每个分量显式写到元素
                 * 类型——`{1, 2, 3, 4}` 里的 1 是 comptime_int，不写这层
                 * 转换 C 会按 double 往上提。 */
                buf_puts(b, "((");
                ctype(b, to);
                buf_puts(b, "){");
                for (int i = 0; i < vt->nfields; i++) {
                    if (i) buf_puts(b, ", ");
                    buf_puts(b, "(");
                    ctype(b, to->elem);
                    buf_puts(b, ")(");
                    ex(b, n->b->items[i]);
                    buf_putc(b, ')');
                }
                buf_puts(b, "})");
                return;
            }
            if (ty_is_num(vt)) {    /* 标量 → 向量：铺开 */
                ensure_type(to);
                Buf sn; buf_init(&sn);
                tsuffix(&sn, to);
                buf_printf(b, "zx_splat_%s(", sn.data);
                ex(b, n->b);
                buf_putc(b, ')');
                return;
            }
        }
        /* 显式转换在 C 里也要显式：`(f32)(0.1 + 0.2)` 的两个操作数在 C 里
         * 是 double 字面量，省掉这层 cast 就整段按 double 算——`== (f32)0.3`
         * 于是给出 false（运行时是 true）。数值目标一律套一层转换。 */
        bool num_to = to && (ty_is_num(to) || to->kind == TY_COMPTIME_FLOAT ||
                             to->kind == TY_COMPTIME_INT || to->kind == TY_ENUM ||
                             to->kind == TY_BOOL || to->kind == TY_ERROR);
        Type *ft = n->b ? n->b->ty : NULL;
        bool num_from = ft && (ty_is_num(ft) || ft->kind == TY_COMPTIME_FLOAT ||
                               ft->kind == TY_COMPTIME_INT || ft->kind == TY_ENUM ||
                               ft->kind == TY_BOOL || ft->kind == TY_ERROR);
        if (num_to && num_from) {
            /* 浮点 → 整数：C 的越界转换是 UB（x86 上 1e300 → INT_MIN，
             * 静默出错值）。包一层运行时检查，越界 trap——与除零同一处置；
             * RELEASE 档照 bounds 惯例擦除。边界与 sema 的 f64_fits_int 一致。 */
            if (ty_is_float(ft) && to->kind == TY_INT && to->bits >= 8) {
                g_need_f2i = true;
                buf_puts(b, "((");
                ctype(b, to);
                if (to->bits <= 64) {
                    buf_puts(b, to->is_signed ? ")zx_f2i64((double)(" : ")zx_f2u64((double)(");
                    ex(b, n->b);
                    buf_printf(b, "), %d))", (int)to->bits);
                } else {
                    buf_puts(b, ")zx_f2i128((double)(");
                    ex(b, n->b);
                    buf_printf(b, "), %d, %d))", (int)to->bits, to->is_signed ? 1 : 0);
                }
                return;
            }
            buf_puts(b, "((");
            ctype(b, to);
            buf_puts(b, ")(");
            ex(b, n->b);
            buf_puts(b, "))");
            return;
        }
        /* B13b：指针 ↔ 函数指针的显式 cast 必须在 C 里也显式写出。sema 的
         * castable 允许这对转换（vkGetInstanceProcAddr 返回的 void* 转扩展入口
         * 函数指针就是典型用例），但 coerce 走普通赋值路径，clang 会报
         * incompatible pointer types。这里补一层 C cast。 */
        if (to && to->kind == TY_FN && ft &&
            (ft->kind == TY_PTR || ft->kind == TY_FN)) {
            buf_puts(b, "(("); ctype(b, to); buf_puts(b, ")(");
            ex(b, n->b);
            buf_puts(b, "))");
            return;
        }
        if (ft && ft->kind == TY_FN && to &&
            (to->kind == TY_PTR || to->kind == TY_FN)) {
            buf_puts(b, "(("); ctype(b, to); buf_puts(b, ")(");
            ex(b, n->b);
            buf_puts(b, "))");
            return;
        }
        coerce(b, n->b, to);
        return;
    }

    case NK_CALL: {
        /* std.print / println / format */
        if (n->a->kind == NK_FIELD && n->a->a && n->a->a->kind == NK_IDENT &&
            n->a->a->name && !strcmp(n->a->a->name, "std")) {
            if (!strcmp(n->a->name, "print"))   { ex_print(b, n, false); return; }
            if (!strcmp(n->a->name, "println")) { ex_print(b, n, true);  return; }
            if (!strcmp(n->a->name, "format"))  { ex_format(b, n); return; }
            if (!strcmp(n->a->name, "stackAlloc")) { ex_stackalloc(b, n); return; }
            if (!strcmp(n->a->name, "heapAlloc"))  { ex_heapalloc(b, n); return; }
            if (!strcmp(n->a->name, "heapSlice"))  { ex_heapslice(b, n); return; }
            /* §189：std.move —— 所有权转移是编译期账本操作，运行期恒等。 */
            if (!strcmp(n->a->name, "move")) {
                if (n->nitems >= 1) ex(b, n->items[0]);
                else buf_puts(b, "0");
                return;
            }
            buf_puts(b, "0");
            return;
        }
        /* §157 — 模块内裸名内建（g_std_seeded 种进每个模块作用域）。
         * sema 已把内建符号挂在 callee 上；按符号而不是按名字分派，
         * 用户自己写的同名函数不会被改道。 */
        if (n->a->kind == NK_IDENT && n->a->sym) {
            const char *bi = sema_sym_builtin_name(n->a->sym);
            if (bi) {
                if (!strcmp(bi, "print"))       { ex_print(b, n, false); return; }
                if (!strcmp(bi, "println"))     { ex_print(b, n, true);  return; }
                if (!strcmp(bi, "format"))      { ex_format(b, n); return; }
                if (!strcmp(bi, "stackAlloc"))  { ex_stackalloc(b, n); return; }
                if (!strcmp(bi, "heapAlloc"))   { ex_heapalloc(b, n); return; }
                if (!strcmp(bi, "heapSlice"))   { ex_heapslice(b, n); return; }
            }
        }
        /* std.io.print / println：往句柄流上格式化。按符号的 C 名字认——
         * std.io.print 和 `const io = std.io` 别名拿到的都是同一个符号。 */
        {
            const char *ioc = sym_cname(n->a);
            if (ioc && !strcmp(ioc, "zx_io_print"))   { ex_io_print(b, n, false); return; }
            if (ioc && !strcmp(ioc, "zx_io_println")) { ex_io_print(b, n, true);  return; }
        }
        /* std.debug.panic / assert：sema 已把 @file/@line/@col/@fnName 追加到
         * 实参尾部，这里按注入后的实参直接调运行时——位置全是编译期字面量，
         * release 档 assert 的擦除发生在运行时函数体里，调用点不变。 */
        {
            const char *dc = sym_cname(n->a);
            if (dc && (!strcmp(dc, "zx_debug_panic") || !strcmp(dc, "zx_debug_assert"))) {
                need_by_cname(dc);      /* 特判路径绕过了通用发射，这里补置 need 标志 */
                /* 参数表要和运行时原型一致：消息/文件名/函数名是 ZxSlice_u8。
                 * 字符串字面量的 Zx 类型是 `*[N]const u8`，不 coerce 就直接
                 * 落成 uint8_t*，C 编译器会拒。 */
                Type *want[6];
                int np = 0;
                if (!strcmp(dc, "zx_debug_assert")) want[np++] = ty_bool();
                want[np++] = ty_slice(ty_u8(), true);   /* msg */
                want[np++] = ty_slice(ty_u8(), true);   /* file */
                want[np++] = ty_i32();                  /* line */
                want[np++] = ty_i32();                  /* col */
                want[np++] = ty_slice(ty_u8(), true);   /* fn */
                buf_printf(b, "%s(", dc);
                for (int i = 0; i < n->nitems; i++) {
                    if (i) buf_puts(b, ", ");
                    if (i < np) coerce(b, n->items[i], want[i]);
                    else        ex(b, n->items[i]);
                }
                buf_putc(b, ')');
                return;
            }
        }
        if (n->inst) {
            int nc = (int)n->ival;                  /* 前 nc 个实参在编译期吃掉了 */
            const char *nm = (n->a && n->a->name) ? n->a->name : NULL;
            Type *fft = n->inst->type_val;
            buf_printf(b, "%s(", inst_name(n->inst, nm));
            for (int i = nc; i < n->nitems; i++) {
                if (i > nc) buf_puts(b, ", ");
                Type *want = (fft && fft->kind == TY_FN && i < fft->nparams) ? fft->params[i] : NULL;
                if (want) coerce(b, n->items[i], want);
                else      ex(b, n->items[i]);
            }
            buf_putc(b, ')');
            return;
        }
        Type *ft = n->a->ty;
        /* 被调用者：可能是函数名、函数指针、或泛型实例 */
        if (n->a->kind == NK_IDENT || n->a->kind == NK_FIELD) {
            ex(b, n->a);
        } else {
            buf_puts(b, "(");
            ex(b, n->a);
            buf_puts(b, ")");
        }
        buf_putc(b, '(');
        /* extern 调用：指针实参显式转成原型的 C 类型。Zx 的 `*u8` 是
         * `uint8_t*`，而原型为了和 libc 对上写的是 `char*`——不转会出
         * -Wpointer-sign 警告一大堆。 */
        bool callee_ext = n->a && n->a->sym && sema_sym_is_extern(n->a->sym);
        for (int i = 0; i < n->nitems; i++) {
            if (i) buf_puts(b, ", ");
            Type *want = (ft && ft->kind == TY_FN && i < ft->nparams) ? ft->params[i] : NULL;
            if (want && callee_ext && want->kind == TY_PTR) {
                Buf ct; buf_init(&ct);
                ctype_c(&ct, want);
                buf_printf(b, "(%s)", ct.data);
            }
            /* C 回调：函数值实参先转 void*。Zx 回调签名（如
             * fn(*const u8,*const u8)i32）与 libc 原型（const void*）
             * 文本上永远对不齐，隐式转换在 clang 16+ 是硬错误；
             * 显式 (void*) 中转是 C 标准允许的合法路径。 */
            if (want && callee_ext && want->kind == TY_FN) {
                buf_puts(b, "(void *)");
            }
            if (want) coerce(b, n->items[i], want);
            else ex(b, n->items[i]);
        }
        buf_putc(b, ')');
        return;
    }

    case NK_TUPLE: case NK_ARRAY_LIT: {
        Type *t = n->ty;
        if (ty_is_array_ptr(t)) t = NULL;   /* 数组字面量按元素写 */
        Buf nm; buf_init(&nm);
        if (n->kind == NK_ARRAY_LIT) {
            Type *at = n->ty;
            /* §205：`.{…}` 值形式 at=[N]T，发 ((ZxArr){ .v = {…} })（无 &）；
             * `&.{…}` 指针形式 at=*[N]T，发 &((ZxArr){ .v = {…} })。 */
            bool is_val = n->flag && at && at->kind == TY_ARRAY;
            Type *arr = is_val ? at : at->elem;
            Type *et = (arr && arr->kind == TY_ARRAY) ? arr->elem : arr;
            ensure_type(arr);
            Buf nm; buf_init(&nm); tn(&nm, arr);
            if (is_val) buf_printf(b, "((%s){ .v = { ", nm.data);
            else        buf_printf(b, "(&((%s){ .v = { ", nm.data);
            for (int i = 0; i < n->nitems; i++) {
                if (i) buf_puts(b, ", ");
                coerce(b, n->items[i], et);
            }
            if (is_val) buf_puts(b, " } })");
            else        buf_puts(b, " } }))");
            return;
        }
        if (!t) { buf_puts(b, "(0)"); return; }
        ensure_type(t);
        tn(&nm, t);
        buf_printf(b, "((%s){ ", nm.data);
        for (int i = 0; i < n->nitems; i++) {
            if (i) buf_puts(b, ", ");
            Type *et = (t->kind == TY_TUPLE && i < t->nfields) ? t->fields[i].ty : NULL;
            if (et) coerce(b, n->items[i], et);
            else ex(b, n->items[i]);
        }
        buf_puts(b, " })");
        return;
    }

    case NK_STRUCT_LIT: case NK_ANON_STRUCT: {
        Type *t = n->ty;
        if (!t || (t->kind != TY_STRUCT && t->kind != TY_UNION)) { ex(b, n->a); return; }
        ensure_type(t);
        Buf nm; buf_init(&nm); tn(&nm, t);
        buf_printf(b, "((%s){ ", nm.data);
        if (t->kind == TY_UNION) {
            Node *pair = n->nitems ? n->items[0] : NULL;
            Field *f = pair ? ty_field(t, pair->name) : NULL;
            if (f && pair) {
                /* 带标签联合体必须把标签写出来：省略初始化器里的 `.tag`
                 * 就等于把每一个成员都当成第一个成员（C 的指定初始化会零填）。
                 * 注意载荷类型早就被 sema 解析成纯载荷了，不是 `{tag, T}` 元组。 */
                if (t->is_tagged && t->tag_ty) {
                    if (!f->has_tag)
                        err_at(pair->line, pair->col,
                               "member '%s' of a tagged union needs a tag: `%s: {SomeTag, T}`",
                               pair->name ? pair->name : "?", pair->name ? pair->name : "?");
                    buf_printf(b, ".tag = %llu, ", (unsigned long long)f->tag);
                }
                if (t->is_tagged) buf_puts(b, ".u");
                dotname(b, pair->name);
                buf_puts(b, " = ");
                coerce(b, pair->a, f->ty);
            }
            buf_puts(b, " })");
            return;
        }
        /* 结构体：按字段顺序写，缺的用默认值补（C 的指定初始化已经零填） */
        int written = 0;
        for (int i = 0; i < n->nitems; i++) {
            Node *pair = n->items[i];
            if (pair->kind != NK_PAIR) continue;
            if (written++) buf_puts(b, ", ");
            dotname(b, pair->name ? pair->name : "?");
            buf_puts(b, " = ");
            Field *f = ty_field(t, pair->name);
            if (f) coerce(b, pair->a, f->ty);
            else   ex(b, pair->a);
        }
        for (int i = 0; i < t->nfields; i++) {
            Field *f = &t->fields[i];
            if (f->is_static || !f->dflt) continue;
            bool given = false;
            for (int k = 0; k < n->nitems; k++)
                if (n->items[k]->kind == NK_PAIR && n->items[k]->name &&
                    !strcmp(n->items[k]->name, f->name)) { given = true; break; }
            if (given) continue;
            if (written++) buf_puts(b, ", ");
            dotname(b, f->name);
            buf_puts(b, " = ");
            coerce(b, f->dflt, f->ty);
        }
        buf_puts(b, " })");
        return;
    }

    /* §200：匿名 fn 字面量当值用——它是 NK_FN 但没有绑定符号，命名 fn 的
     * NK_IDENT 分支够不着。登记一个 static C 函数名，值位置发这个名字；
     * 函数体在单元正文发完后统一补发（flush_anon_fns）。泛型 fn 没有单态
     * 实例，当普通值用没有意义，退回 0 免得砸出未定义符号。 */
    case NK_FN: {
        if (n->b && !fn_is_generic(n)) {
            const char *nm = anonfn_cname(n);
            need_by_cname(nm);
            buf_puts(b, nm);
        } else {
            buf_puts(b, "0");
        }
        return;
    }

    case NK_BUILTIN:
        ex_builtin(b, n);
        return;

    case NK_MODULE:
        return;

    case NK_PAIR:
        ex(b, n->a);
        return;

    /* §170 tag 贴标：运行期就是被贴的表达式本身，账本信息编译完擦除 */
    case NK_TAG_APPLY:
        ex(b, n->b);
        return;

    /* §174 续链：同上——运行期就是被绑的表达式本身（let 的初值） */
    case NK_TAG_BIND:
        ex(b, n->b);
        return;

    /* 有值的控制流当普通表达式用（`std.println("{}", if (c) 1 else 0)`、
     * `f(s: blk)`、switch 表达式……）：开一个临时量，用 gen_value 把值写
     * 进去，表达式本身引用临时量。line() 在这里落盘的语句会打印在"包着
     * 本表达式的那条语句"之前——那条语句此刻还攒在 Buf 里没输出，顺序
     * 天然正确（与 NK_ORELSE / try 的 _or / _ty 临时量同一套路）。
     * 注意临时量不能包 {}：表达式在"语句"里还要引用它。 */
    default:
        if (is_ctl(n) && n->ty) {
            static int ivn;
            char tmp[64];
            snprintf(tmp, sizeof tmp, "_iv%d", ivn++);
            line("%s %s = {0};", cty(n->ty), tmp);
            gen_value(n, tmp, n->ty);
            buf_puts(b, tmp);
            return;
        }
        err_at(n->line, n->col, "internal: expression node %d reached the C emitter", (int)n->kind);
        buf_puts(b, "0");
        return;
    }
}

/* 供 ensure_type 里的静态成员默认值使用 */
void backend_ex(Buf *b, Node *e) { ex(b, e); }

/* ======================================================================
 *  语句
 * ====================================================================== */

/* 声明一个局部变量（数组类型 → C 数组，指针类型 → C 指针） */
static bool fn_is_generic(Node *fn);

/* 局部 fn 绑定（const F = fn (…) ret { … }; 写在函数体里）的 C 函数名：
 * 绑定符号的 C 名 + "_impl"。局部绑定的 sym_cname 是 name_line_col 形态
 * （F_4_5），加后缀不会和任何用户名字撞车。 */
static char *localfn_impl_name(Node *d) {
    const char *cn = sym_cname(d);
    size_t n = strlen(cn ? cn : "anon");
    char *s = malloc(n + 8);
    snprintf(s, n + 8, "%s_impl", cn ? cn : "anon");
    return s;
}

static void gen_decl_inner(Node *d) {
    Type *t = d->ty;
    if (!t || t->kind == TY_INVALID) return;
    /* 类型值（const P = Pair(i32);）只在编译期存在，C 里没有对应的东西 */
    if (t->kind == TY_TYPE) return;
    /* std.Type / std.Field 只在编译期存在：这些"声明"是编译期绑定，
     * 所有使用点在语义层都折成字面量了，C 里不该有它们。 */
    if (sema_ty_ct_only(t)) return;
    /* 别名（const heap = std.heap;）也是个编译期名字：它没有自己的存储，
     * 引用处直接落到被指向的那份上 */
    if (!sema_sym_has_storage(d->sym)) return;

    /* `void` 没有存储：编译期已知为假、又没有 `else` 的 `if` 就是这种类型。
     * 绑定只是给"没有值"起个名字，初始化器的副作用照发，C 里不建变量。 */
    if (t->kind == TY_VOID) {
        if (d->b) gen_value(d->b, NULL, NULL);
        return;
    }

    const char *nm = sym_cname(d);
    char *cn = (nm && !strcmp(nm, "_")) ? ignored_cname() : cnames(nm);

    bool is_var = (d->op == 2);
    /* B13：声明提升模式下，变量槽已在函数入口裸声明（非 const），这里只发
     * 初始化赋值。C 里已声明变量不能再用 `{0}` 初始化，聚合/标量统一用
     * compound literal `(T){0}`；指针用 `= 0`。非提升模式保持原发射不变。 */
    bool hz = g_hoist_locals;

    if (ty_is_array_ptr(t)) {
        Node *init = d->b;
        /* §201：*[N]T 的 elem 已是 [N]T；这里所有"元素/数组"口径都按它拆。 */
        Type *arr = t->elem;
        Type *elt = (arr && arr->kind == TY_ARRAY) ? arr->elem : arr;
        bool lit = init && init->kind == NK_ARRAY_LIT;
        if (lit) {
            /* 字面量先落一个匿名 [N]T 数组当存储，再让指针指向它。
             * _lit 是独立存储，不参与提升，仍在原位声明。 */
            Buf litdecl; buf_init(&litdecl);
            if (!is_var) buf_puts(&litdecl, "const ");
            ctype(&litdecl, arr);
            buf_printf(&litdecl, " %s_lit", cn);
            buf_puts(&litdecl, " = { .v = { ");
            for (int i = 0; i < init->nitems; i++) {
                if (i) buf_puts(&litdecl, ", ");
                coerce(&litdecl, init->items[i], elt);
            }
            buf_puts(&litdecl, " } };");
            line("%s", litdecl.data);
            if (hz) line("%s = &%s_lit;", cn, cn);
            else    line("%s%s *%s = &%s_lit;", is_var ? "" : "const ",
                         cty(arr), cn, cn);
            return;
        }
        /* R9（v0.10 起）：数组指针 + undefined 不再隐式分配存储。
         * `undefined` 只是"这个指针槽不初始化"；要内存请走
         * std.stackAlloc / std.heapAlloc / &.{…}。指针本身清零，
         * 误用会撞空指针而不是悄悄改栈。 */
        if (!init || init->kind == NK_UNDEF) {
            if (hz) line("%s = 0;", cn);
            else    line("%s%s *%s = 0;", is_var ? "" : "const ", cty(t->elem), cn);
            return;
        }
        /* 普通指针初始化 */
        if (is_ctl(init)) {
            if (!hz) line("%s *%s = 0;", cty(t->elem), cn);
            gen_value(init, cn, t);
            return;
        }
        if (hz) line("%s = %s;", cn, cxs(init, t));
        else    line("%s *%s = %s;", cty(t->elem), cn, cxs(init, t));
        return;
    }

    if (!d->b || d->b->kind == NK_UNDEF) {
        if (hz) line("%s = (%s){0};", cn, cty(t));
        else    line("%s %s = {0};", cty(t), cn);
        return;
    }
    /* 初值是控制流表达式（带标签的块 / if / 循环）：先开槽，再往里写 */
    if (is_ctl(d->b)) {
        if (hz) line("%s = (%s){0};", cn, cty(t));
        else    line("%s %s = {0};", cty(t), cn);
        gen_value(d->b, cn, t);
        return;
    }
    /* 局部 fn 绑定：fn 字面量已由 gen_fn 的预扫发成同单元的 static C 函数
     * （gen_local_fns），这里把函数指针槽初始化成它的地址。没有这一步，
     * fn 字面量节点会砸进表达式发射器，槽发成 0——调用即空指针。 */
    if (d->b->kind == NK_FN) {
        Node *lfn = d->b;
        if (lfn->b && !fn_is_generic(lfn)) {
            char *inm = localfn_impl_name(d);
            if (hz) line("%s = %s;", cn, inm);
            else    line("%s %s = %s;", cty(t), cn, inm);
            free(inm);
        } else {
            if (hz) line("%s = 0;", cn);
            else    line("%s %s = 0;", cty(t), cn);
        }
        return;
    }
    if (hz) line("%s = %s;", cn, cxs(d->b, t));
    else    line("%s %s = %s;", cty(t), cn, cxs(d->b, t));
}

static void gen_if_stmt(Node *n);
static void gen_while_for(Node *n);
static void gen_switch(Node *n, const char *slot, Type *slot_ty);

static void gen_stmt(Node *n) {
    if (!n) return;
    bmap_note(g_out, n);
    /* §211: 语句边界刷新 zx 位置影子——崩溃处理器据此报 zx 文件:行。
     * 只在函数体内（g_in_fn）且有源码行时更新，避免合成节点污染位置。 */
    if (g_in_fn && n->line > 0) {
        Buf zf; buf_init(&zf);
        const char *mp = g_cur_mod_path ? g_cur_mod_path : "?";
        cstr(&zf, mp, (int)strlen(mp));
        line("zx_loc(%s, %d, %d);", zf.data, n->line, n->col);
        buf_free(&zf);
    }

    switch (n->kind) {
    case NK_DECL:
        if (n->c) {
            /* 解构：const {a,b,c} = tup; */
            Type *t = n->ty;
            Type *tt = t;
            char tmp[64];
            static int dn;
            snprintf(tmp, sizeof tmp, "_ds%d", dn++);
            line("%s %s = %s;", cty(tt), tmp, cxs(n->b, tt));
            for (int i = 0; i < n->c->nitems; i++) {
                Node *id = n->c->items[i];
                Type *ft = (tt && tt->kind == TY_TUPLE && i < tt->nfields)
                           ? tt->fields[i].ty : ty_i32();
                const char *dnm = sym_cname(id);
                const char *dn2 = (dnm && !strcmp(dnm, "_"))
                                      ? ignored_cname() : cnames(dnm);
                line("%s %s = %s.f%d;", cty(ft), dn2, tmp, i);
            }
            return;
        }
        gen_decl_inner(n);
        return;

    case NK_RETURN: {
        Type *want = g_cur_ret ? g_cur_ret : (n->a ? n->a->ty : ty_void());
        if (!n->a) {
            run_defers(0);
            if (g_is_main) main_exit(NULL);
            else if (ret_is_errvoid(g_cur_ret)) line("return (%s){ .tag = 0 };", cty(g_cur_ret));
            else line("return;");
            return;
        }

        /* errdefer 在场且返回错误联合：值先进临时量，defer/errdefer 按
         * 登记逆序交织展开（errdefer 包 if (tag != 0)——是不是错误只有
         * 运行时知道），最后返回临时量。 */
        bool errcheck = g_saw_errdefer && want && want->kind == TY_ERR;

        /* 返回的是控制流表达式：先算进临时量，再统一收尾 */
        if (is_ctl(n->a)) {
            static int rn;
            char tmp[32], cond[80];
            snprintf(tmp, sizeof tmp, "_ret%d", rn++);
            line("%s %s = {0};", cty(want), tmp);
            gen_value(n->a, tmp, want);
            if (errcheck) {
                snprintf(cond, sizeof cond, "(%s).tag != 0", tmp);
                char tagb[80];
                snprintf(tagb, sizeof tagb, "%s.tag", tmp);
                run_defers_m(0, 1, cond, tagb);
            } else run_defers(0);
            if (g_is_main) main_exit(tmp); else line("return %s;", tmp);
            return;
        }

        char *v = cxs(n->a, want);
        /* 返回表达式必须先求值进临时量，再展开 defer。原来把 cxs() 的
         * 字符串直接塞进 `return %s;`，defer 语句却排在它前面发 ——
         * `return f();` 配 defer 会变成「先跑 defer，再调 f」：返回值可能
         * 被 defer 改掉，锁也会在临界区之前就放开。is_ctl 分支早就按
         * 「先存临时量」处理，这里补齐普通表达式（§142 撞出来的）。 */
        if (want && want->kind == TY_VOID) {
            /* void 表达式没有可存的值，但仍要先让它求值（副作用在前）。 */
            line("%s;", v);
            run_defers(0);
            if (g_is_main) main_exit(NULL); else line("return;");
            return;
        }
        static int drn;
        char tmp[32], cond[80];
        snprintf(tmp, sizeof tmp, "_dr%d", drn++);
        line("{ %s %s = %s;", cty(want), tmp, v);
        if (errcheck) {
            snprintf(cond, sizeof cond, "(%s).tag != 0", tmp);
            char tagb[80];
            snprintf(tagb, sizeof tagb, "%s.tag", tmp);
            run_defers_m(0, 1, cond, tagb);
        } else run_defers(0);
        if (g_is_main) main_exit(tmp); else line("return %s;", tmp);
        line("}");
        return;
    }

    case NK_CONTINUE: {
        CLab *l = lab_find_loop(n->label);
        if (!l) return;
        run_defers(l->depth + 1);
        line("goto Lcont%d;", l->cont);
        return;
    }

    case NK_YIELD: {
        /* §213：label=NULL 的收割是 `break`——绑到最近外循环的 CLab
         * （lab_find_loop(NULL) 连无标签循环都有帧，见 gen_loop_value）。 */
        CLab *l = n->label ? lab_find(n->label) : lab_find_loop(NULL);
        /* 没人接的收割不是合法程序：语义层本该拦住，走到这里说明标签作用域
         * 漏建了（M25 的 bug ② 就是这么发生的）。宁可报内部错误，也不要
         * 静默丢一条赋值——那会生成"能跑、答案错"的程序。 */
        if (!l) {
            err_at(n->line, n->col,
                   "internal: yield target '%s' has no enclosing label scope",
                   n->label ? n->label : "?");
            return;
        }
        if (l->slot) {
            if (n->a) line("%s = %s;", l->slot, cxs(n->a, l->slot_ty));
            else      line("%s = (%s){0};", l->slot, cty(l->slot_ty));
        }
        run_defers(l->depth);
        line("goto Lend%d;", l->end);
        return;
    }

    case NK_DEFER:
    case NK_ERRDEFER: {
        if (!g_csc) return;
        if (n->kind == NK_ERRDEFER) g_saw_errdefer = true;
        if (g_csc->ndefer < MAX_CDEFER) g_csc->defer[g_csc->ndefer++] = n;
        return;
    }

    case NK_BLOCK: {
        if (n->label) {
            /* 带标签的块作为语句：值没人要，但收割要能跳 */
            int id = ++g_labid;
            CLab *l = &g_labs[g_nlabs++];
            l->tag = n->label; l->slot = NULL; l->slot_ty = NULL;
            l->end = id; l->cont = 0; l->depth = g_depth;
            CScope s; cscope_push(&s);
            gen_block_stmts(n);
            cscope_leave();
            g_nlabs--;
            line("Lend%d: ;", id);
            return;
        }
        CScope s; cscope_push(&s);
        gen_block_stmts(n);
        cscope_leave();
        return;
    }

    case NK_IF:
        gen_if_stmt(n);
        return;

    case NK_WHILE: case NK_FOR:
        if (n->flag) {
            /* inline for：sema 已按迭代次数克隆循环体，逐份发射。
             * 绑定名是编译期常量（SK_CONST + ct），ident 发射时直接出字面量。
             * 每份包一层大括号：克隆体里的 let/var 不得互相撞名。
             * nunroll == 0（死分支里 / 迭代零次）就什么都不发。 */
            for (int i = 0; i < n->nunroll; i++) {
                line("{");
                g_depth++;
                gen_stmt(n->unroll[i]);
                g_depth--;
                line("}");
            }
            return;
        }
        gen_while_for(n);
        return;

    case NK_SWITCH:
        gen_switch(n, NULL, NULL);
        return;

    case NK_BIN:
        if (is_assign_op(n->op)) {
            /* `_ = expr;`：只要副作用，不落存储（M26）。sema 已拦住
             * 右值为 void 的情况；这里把右值求值成一条表达式语句。
             * 走槽位路径，右值是 if/switch 表达式也能接住。 */
            if (n->op == TOK_ASSIGN && n->a->kind == NK_IDENT &&
                !strcmp(n->a->name, "_") && !n->a->sym) {
                /* `_ = <编译期值>;`（`const t: type = info.fields[0].ty;`
                 * 之后的 `_ = t;`）：右侧根本没有运行期表示，C 里既不该
                 * 开临时量也不该引用它——以前发成 `void _ds0 = {0};` +
                 * `_ds0 = t;`，C 报 incomplete type 'void' / undeclared
                 * identifier（用户 2026-11-11 第 2 张截图）。 */
                if (n->b->ty && (n->b->ty->kind == TY_TYPE ||
                                 sema_ty_ct_only(n->b->ty))) return;
                static int ad;
                char tmp[32];
                /* 前缀用 _ig：解构临时（NK_DECL 分支）的 _ds 计数器是独立
                 * 的，两套计数器同发 _ds0 会撞名 —— `_ = p;` 跟在
                 * `const {a,b} = t;` 后面就 B1001 redefinition
                 * （2026-11-11 reference 校对踩中）。 */
                snprintf(tmp, sizeof tmp, "_ig%d", ad++);
                line("%s %s = {0};", cty(n->b->ty), tmp);
                gen_value(n->b, tmp, n->b->ty);
                return;
            }
            Buf l, r;
            buf_init(&l); buf_init(&r);
            lvalue(&l, n->a);
            if (n->op == TOK_ASSIGN) {
                if (is_ctl(n->b)) {
                    /* 左边是简单名字时直接当槽位用 */
                    bool simple = n->a->kind == NK_IDENT;
                    if (simple) {
                        gen_value(n->b, l.data, n->a->ty);
                    } else {
                        static int an;
                        char tmp[32];
                        snprintf(tmp, sizeof tmp, "_as%d", an++);
                        line("%s %s = {0};", cty(n->a->ty), tmp);
                        gen_value(n->b, tmp, n->a->ty);
                        line("%s = %s;", l.data, tmp);
                    }
                    return;
                }
                coerce(&r, n->b, n->a->ty);
                line("%s = %s;", l.data, r.data);
            } else {
                const char *op = n->op == TOK_PLUSEQ ? "+" : n->op == TOK_MINUSEQ ? "-" :
                                 n->op == TOK_STAREQ ? "*" : n->op == TOK_SLASHEQ ? "/" :
                                 n->op == TOK_PERCEQ ? "%" : n->op == TOK_AMPEQ   ? "&" :
                                 n->op == TOK_PIPEEQ ? "|" : n->op == TOK_CARETEQ ? "^" :
                                 n->op == TOK_SHLEQ  ? "<<" : ">>";
                Type *lt = n->a->ty;
                if (lt && lt->kind == TY_PTR && (n->op == TOK_PLUSEQ || n->op == TOK_MINUSEQ)) {
                    Buf ec; buf_init(&ec); ctype(&ec, lt->elem);
                    Buf dc; buf_init(&dc); ctype(&dc, lt);
                    line("%s = (%s)((%s*)%s %s %s);", l.data, dc.data, ec.data, l.data, op,
                         cxs(n->b, ty_usize()));
                } else {
                    /* M19：复合赋值走同一套数值语义（回绕 / 除零 trap / 移位掩码）。
                     * 必须先把 `+=`/`<<=` 这类**归一成基础运算符**——num_bin_c
                     * 只认 TOK_PLUS/TOK_SHL，不归一的话移位掩码与回绕全会漏掉
                     * （`a <<= 127` 在 i8 上就是 C 的 UB）。 */
                    int bop = n->op;
                    switch (n->op) {
                    case TOK_PLUSEQ: bop = TOK_PLUS; break;
                    case TOK_MINUSEQ: bop = TOK_MINUS; break;
                    case TOK_STAREQ: bop = TOK_STAR; break;
                    case TOK_SLASHEQ: bop = TOK_SLASH; break;
                    case TOK_PERCEQ: bop = TOK_PERCENT; break;
                    case TOK_AMPEQ: bop = TOK_AMP; break;
                    case TOK_PIPEEQ: bop = TOK_PIPE; break;
                    case TOK_CARETEQ: bop = TOK_CARET; break;
                    case TOK_SHLEQ: bop = TOK_SHL; break;
                    case TOK_SHREQ: bop = TOK_SHR; break;
                    default: break;
                    }
                    Buf r; buf_init(&r);
                    const char *rs = cxs(n->b, n->a->ty);
                    if (!num_bin_c(&r, lt, bop, l.data, rs))
                        buf_printf(&r, "%s %s %s", l.data, op, rs);
                    line("%s = %s;", l.data, r.data);
                }
            }
            return;
        }
        line("%s;", exs(n));
        return;

    default:
        if (is_ctl(n)) {
            /* 有值的控制流出现在语句位置：值丢掉 */
            gen_value(n, NULL, NULL);
            return;
        }
        line("%s;", exs(n));
        return;
    }
}

/* 有值的表达式 → 写进 slot（slot == NULL 表示当语句用，值丢掉） */
static void gen_value(Node *n, const char *slot, Type *slot_ty) {
    if (!n) return;
    switch (n->kind) {
    case NK_BLOCK:
        if (n->label) {
            int id = ++g_labid;
            CLab *l = &g_labs[g_nlabs++];
            l->tag = n->label; l->slot = slot; l->slot_ty = slot_ty;
            l->end = id; l->cont = 0; l->depth = g_depth;
            CScope s; cscope_push(&s);
            gen_block_stmts(n);
            cscope_leave();
            g_nlabs--;
            line("Lend%d: ;", id);
            return;
        }
        gen_stmt(n);
        return;
    case NK_IF:      gen_if_value(n, slot, slot_ty); return;
    case NK_WHILE:
    case NK_FOR:
        if (n->flag) {
            for (int i = 0; i < n->nunroll; i++) {
                line("{");
                g_depth++;
                gen_stmt(n->unroll[i]);
                g_depth--;
                line("}");
            }
            return;      /* inline for 没有值（sema 不允许标签和 else） */
        }
        gen_loop_value(n, slot, slot_ty); return;
    case NK_SWITCH:  gen_switch(n, slot, slot_ty); return;
    case NK_YIELD:
        /* 裸收割臂落到值语境：gen_stmt 的 yield 分支负责 lab_find、
         * 写标签的 slot、run_defers、goto Lend——当表达式发只会是 0。 */
        gen_stmt(n); return;
    default:
        if (slot) line("%s = %s;", slot, cxs(n, slot_ty));
        else      line("%s;", exs(n));
        return;
    }
}

/* §130 合并存储的整体归还，跟 run_defers_m 同走「离开作用域」通道 */
static void emit_region_free(int gid, uint64_t total, Node *al) {
    if (!al) return;
    Buf a; buf_init(&a); ex(&a, al);
    line("zx_heapfree(%s, __zxr%d, (uint64_t)%llu);",
         a.data, gid, (unsigned long long)total);
}

/* §153：region 的分配器恒是 NK_IDENT（§130 只合并同一个局部标识符）。
 * 若这个标识符就在本块里声明，`__zxrN` 的初始化必须排到那条声明之后，
 * 否则生成的 C 先用后声明（B1002）。§150 让 `std.heap.page` 变成
 * late-init 全局 + 运行时局部副本后，bench/mmat.zx 首次暴露这条顺序错误。 */
static void emit_region_alloc(Node *b) {
    Buf a; buf_init(&a); ex(&a, b->reg_alloc);
    g_need_heapalloc = true;
    line("uint8_t *__zxr%d = (uint8_t *)zx_heapalloc(%s, (uint64_t)%llu);",
         b->reg_gid, a.data, (unsigned long long)b->reg_total);
    if (g_csc) {
        g_csc->reg_gid = b->reg_gid;
        g_csc->reg_total = b->reg_total;
        g_csc->reg_alloc = b->reg_alloc;
    }
}

static void gen_block_stmts(Node *b) {
    int after = -1;
    if (b->reg_gid > 0 && b->reg_alloc && b->reg_alloc->kind == NK_IDENT &&
        b->reg_alloc->name) {
        for (int i = 0; i < b->nitems; i++) {
            Node *s = b->items[i];
            if (s->kind == NK_DECL && s->name &&
                !strcmp(s->name, b->reg_alloc->name)) { after = i; break; }
        }
    }
    bool done = false;
    if (b->reg_gid > 0 && after < 0) { emit_region_alloc(b); done = true; }
    for (int i = 0; i < b->nitems; i++) {
        gen_stmt(b->items[i]);
        /* 声明语句发射完再放 alloc：它一定早于第一个 heapAlloc 站点，
         * 因为源码里分配器不声明就写不出那次调用。 */
        if (!done && i == after) { emit_region_alloc(b); done = true; }
    }
    if (b->reg_gid > 0 && !done) emit_region_alloc(b);
}

/* 函数返回类型 / 是否是 main：生成 return 时需要 */

/* ---- if ---- */

static void gen_arm(Node *arm, const char *slot, Type *slot_ty) {
    if (!arm) return;
    CScope s; cscope_push(&s);
    if (arm->kind == NK_BLOCK) {
        if (arm->label) {
            int id = ++g_labid;
            CLab *l = &g_labs[g_nlabs++];
            l->tag = arm->label; l->slot = slot; l->slot_ty = slot_ty;
            l->end = id; l->cont = 0; l->depth = g_depth;
            gen_block_stmts(arm);
            g_nlabs--;
            line("Lend%d: ;", id);
        } else {
            gen_block_stmts(arm);
        }
    } else if (slot) {
        /* 臂本身是控制流（`else if (…)` 就是这种形状）或裸收割
         * （`n: 210;`——zxfe 解析成 NK_YIELD）：值由 gen_value / gen_stmt
         * 写进 slot，不能当成普通表达式 `slot = <expr>` 发——NK_YIELD
         * 被当表达式只会发成 `slot = 0`，值和 goto 全丢，生成"能跑、
         * 答案错"的程序。 */
        if (is_ctl(arm) || arm->kind == NK_YIELD) gen_value(arm, slot, slot_ty);
        else             line("%s = %s;", slot, cxs(arm, slot_ty));
    } else {
        gen_stmt(arm);
    }
    cscope_leave();
}

static void gen_if_stmt(Node *n) {
    gen_value(n, NULL, NULL);
}

/* 把 if 的值写进 slot（slot == NULL 表示当语句用） */
static void gen_if_body(Node *n, const char *slot, Type *slot_ty);

/* 标签加在 `if` 上时，两条臂共用同一个标签作用域——收割从任意一条臂
 * 直接跳到 if 之后（规格 §8.1 的"推荐写法"）：
 *
 *     let sign = s: if (x > 0) { s: 1; } else { s: 0; };
 *
 * 臂上的标签由 gen_arm 自己管；这里管的是 if 本身那一层。漏了它，臂里的
 * `s: 1;` 在 lab_find 里找不到目标，会被静默丢弃——生成空臂、返回 0。
 */
static void gen_if_value(Node *n, const char *slot, Type *slot_ty) {
    if (n->label && g_nlabs < MAX_CLABS) {
        int id = ++g_labid;
        CLab *l = &g_labs[g_nlabs++];
        l->tag = n->label; l->slot = slot; l->slot_ty = slot_ty;
        l->end = id; l->cont = 0; l->depth = g_depth;
        gen_if_body(n, slot, slot_ty);
        g_nlabs--;
        line("Lend%d: ;", id);
        return;
    }
    gen_if_body(n, slot, slot_ty);
}

static void gen_if_body(Node *n, const char *slot, Type *slot_ty) {
    /* 语义层打了标（条件是编译期常量且纯净，见 check_if）：只发会走的那条臂。
     * 语义层剪分支时另一条根本没检查——发出来多半是合法的 C 编不过的。
     * 只看 n->flag 而不是 cond->ct：编译期执行循环会在真实节点上留下
     * 最后一轮的条件值，那种烙印不是真的常量。 */
    if (n->flag) {
        /* 剪枝依据：优先看条件上现存的折叠值；它常被后续流程清掉，
         * 那就用语义层存下的结论（n->ival，见 check_if 的 KEEP）。 */
        bool take = n->a->ct && n->a->ct->kind == V_BOOL ? n->a->ct->bval : (n->ival != 0);
        if (take)      gen_arm(n->b, slot, slot_ty);
        else if (n->c) gen_arm(n->c, slot, slot_ty);
        return;
    }

    Type *c = n->a->ty;
    bool is_opt = c && c->kind == TY_OPT;
    bool is_err = c && c->kind == TY_ERR;

    /* `?T` / `!T` 作条件：判的是 tag（`if (x)` 对 `!void` 也要走这条，
     * 那时没有载荷可捕获，只有 tag 可判）。 */
    if (is_opt || is_err) {
        bool flat = is_opt && ty_opt_flat(c);
        /* §221：指针捕获——条件是可写左值（语义层保证），取它的地址，
         * 让 t 指向原对象的载荷而非临时副本，写回才能落到原对象。
         * flat（?*T）：载荷就是那个指针值本身，t 指向整个 optional 变量（**T）。 */
        bool capptr = n->cap_ptr && n->name;
        static int cn;
        int my = cn++;
        char tmp[64], ptr[72], tagref[80];
        snprintf(tmp, sizeof tmp, "_c%d", my);
        snprintf(ptr, sizeof ptr, "_c%dp", my);
        if (capptr) {
            Buf lv; buf_init(&lv);
            lvalue(&lv, n->a);            /* 条件的左值（原对象存储） */
            line("%s *%s = &(%s);", cty(c), ptr, lv.data);
            buf_free(&lv);
            snprintf(tagref, sizeof tagref, "%s->", ptr);
        } else {
            line("%s %s = %s;", cty(c), tmp, cxs(n->a, c));
            snprintf(tagref, sizeof tagref, "%s.", tmp);
        }
        if (flat)       line("if (%s) {", capptr ? ptr : tmp);
        else if (is_opt) line("if (%stag) {", tagref);
        else            line("if (%stag == 0) {", tagref);
        g_ind++;
        CScope s; cscope_push(&s);
        /* `!void` 的载荷是 0 字节，没有可声明的变量（语义层已就此报错，
         * 这里只是不让坏代码变成 `void v = …` 这种非法 C）。 */
        if (n->name && c->elem->kind != TY_VOID) {
            if (capptr && flat)
                /* flat ?*T：载荷就是这个指针值，t 指向整个 optional 变量（**T） */
                line("%s *%s = %s;", cty(c->elem), cnames(n->name), ptr);
            else if (capptr)
                /* t 的类型是 *T（sema 已绑），指向原对象载荷 */
                line("%s *%s = &%spayload;", cty(c->elem), cnames(n->name), tagref);
            else
                line("%s %s = %s%s;", cty(c->elem), cnames(n->name), tmp, flat ? "" : ".payload");
        }
        gen_arm(n->b, slot, slot_ty);
        cscope_pop();
        g_ind--;
        line("} else {");
        g_ind++;
        if (n->c) {
            CScope s2; cscope_push(&s2);
            /* 捕获点（decisions §87）：`else err { … }` 一进入就登记一条
             * {码, file, line, col, fn} 到当前上下文的捕获环——错误值在
             * 这里落地绑定，trace 就是它的脚印。dev 档才有；release 里
             * 整个调用被预处理器抹掉，零开销。 */
            if (is_err) {
                g_need_task = true;
                Buf pf, pn;
                buf_init(&pf); buf_init(&pn);
                const char *mp = g_cur_mod_path ? g_cur_mod_path : "";
                const char *fp = g_cur_fn_zx ? g_cur_fn_zx : "?";
                cstr(&pf, mp, (int)strlen(mp));
                cstr(&pn, fp, (int)strlen(fp));
                line("#ifndef ZX_RELEASE");
                line("zx_cap_push((unsigned)%stag, %s, %d, %d, %s);",
                     tagref, pf.data, n->line, n->col, pn.data);
                line("#endif");
                buf_free(&pf); buf_free(&pn);
            }
            if (n->d && n->d->kind == NK_IDENT) {
                /* else 捕获只属于 `!T`（`?T` + else 捕获已被 sema 拒掉）。
                 * is_opt 分支只是防御：tag 是 bool。 */
                Type *et = is_err ? ty_error() : ty_bool();
                const char *cpt = (n->d->name && !strcmp(n->d->name, "_"))
                                      ? ignored_cname() : cnames(n->d->name);
                line("%s %s = %stag;", cty(et), cpt, tagref);
            }
            gen_arm(n->c, slot, slot_ty);
            cscope_pop();
        }
        g_ind--;
        line("}");
        return;
    }

    line("if %s {", exs_cond(n->a));
    g_ind++;
    gen_arm(n->b, slot, slot_ty);
    g_ind--;
    if (n->c) {
        line("} else {");
        g_ind++;
        gen_arm(n->c, slot, slot_ty);
        g_ind--;
    }
    line("}");
}

/* ---- while / for ---- */

static void gen_while_for(Node *n) { gen_value(n, NULL, NULL); }

/* 循环回边取消检查：只在 !T 函数（能构造 cancelled 返回值）里注入。
 * 根上下文 / main 里恒 false，不必白付一次跨单元调用。纯检查不挂起，
 * 所以不改变协作调度的交错顺序（task_basic 的 1212123 守门）。
 *
 * 这条注入的 return 是「函数以错误返回」，必须自己展开 defer/errdefer：
 * defer 是编译期展开的，NK_RETURN 分支才调 run_defers；直接发一句裸
 * return 会让取消路径漏掉所有清理 —— Scope 的自动 close 全靠 defer，
 * 漏了就等于子任务树泄漏。errcond 传 NULL：这里的错误已注定是 cancelled。 */
static void emit_loop_cancel_check(void) {
    if (!g_need_task || g_is_main || !g_cur_ret || g_cur_ret->kind != TY_ERR)
        return;
    line("if (zx_task_cancel_check()) {");
    char cev[24];
    snprintf(cev, sizeof cev, "%u", (unsigned)ty_err_code("cancelled"));
    g_ind++;
    run_defers_m(0, 1, NULL, cev);
    line("return (%s){ .tag = %u };",
         cty(g_cur_ret), (unsigned)ty_err_code("cancelled"));
    g_ind--;
    line("}");
}

static void gen_loop_value(Node *n, const char *slot, Type *slot_ty) {
    int id = ++g_labid;
    CLab *l = &g_labs[g_nlabs++];
    l->tag = n->label; l->slot = slot; l->slot_ty = slot_ty;
    l->end = id; l->cont = id; l->depth = g_depth;

    if (n->kind == NK_WHILE) {
        Type *c = n->a->ty;
        bool is_opt = c && c->kind == TY_OPT;
        bool is_err = c && c->kind == TY_ERR;
        if (is_opt || is_err) {
            line("for (;;) {");
            g_ind++;
            static int cn;
            bool wflat = is_opt && ty_opt_flat(c);
            bool capptr = n->cap_ptr && n->name;   /* §221 */
            int my = cn++;
            char tmp[64], ptr[72], tagref[80];
            snprintf(tmp, sizeof tmp, "_w%d", my);
            snprintf(ptr, sizeof ptr, "_w%dp", my);
            if (capptr) {
                Buf lv; buf_init(&lv);
                lvalue(&lv, n->a);
                line("%s *%s = &(%s);", cty(c), ptr, lv.data);
                buf_free(&lv);
                snprintf(tagref, sizeof tagref, "%s->", ptr);
            } else {
                line("%s %s = %s;", cty(c), tmp, cxs(n->a, c));
                snprintf(tagref, sizeof tagref, "%s.", tmp);
            }
            if (wflat)      line("if (!%s) break;", capptr ? ptr : tmp);
            else if (is_opt) line("if (!%stag) break;", tagref);
            else            line("if (%stag != 0) break;", tagref);
            CScope s; cscope_push(&s);
            if (n->name && c->elem->kind != TY_VOID) {
                if (capptr && wflat)
                    line("%s *%s = %s;", cty(c->elem), cnames(n->name), ptr);
                else if (capptr)
                    line("%s *%s = &%spayload;", cty(c->elem), cnames(n->name), tagref);
                else
                    line("%s %s = %s%s;", cty(c->elem), cnames(n->name), tmp, wflat ? "" : ".payload");
            }
            emit_loop_cancel_check();
            gen_block_stmts(n->b);
            line("Lcont%d: ;", id);
            cscope_leave();
            g_ind--;
            line("}");
        } else {
            line("while %s {", exs_cond(n->a));
            g_ind++;
            CScope s; cscope_push(&s);
            emit_loop_cancel_check();
            gen_block_stmts(n->b);
            line("Lcont%d: ;", id);
            cscope_leave();
            g_ind--;
            line("}");
        }
    } else {
        gen_for_body(n, id);
    }

    if (n->c && slot) line("%s = %s;", slot, cxs(n->c, slot_ty));
    g_nlabs--;
    line("Lend%d: ;", id);
}

/* for：切片 / 整数范围 / 多绑定 */
static void gen_for_body(Node *n, int id) {
    int nseq  = (int)n->a->ival;
    int nbind = (int)n->a->fval;
    static int fi;

    char seqname[8][32];
    Buf pre; buf_init(&pre);
    int fid = fi++;

    /* 每条序列先落一个临时量，避免重复求值 */
    for (int i = 0; i < nseq && i < 8; i++) {
        Node *s = n->items[i];
        snprintf(seqname[i], sizeof seqname[i], "_sq%d_%d", fid, i);
        if (s->kind == NK_RANGE) {
            Type *lt = s->a->ty;
            bline(&pre, "%s %s_lo = %s;", cty(lt), seqname[i], cxs(s->a, lt));
        } else {
            Type *st = s->ty;
            bline(&pre, "%s %s = %s;", cty(st), seqname[i], cxs(s, st));
        }
    }

    /* 长度：第一个具体序列的 len，或者第一个闭合范围的 hi-lo。
     * 只有下限的范围（`for (0..)`）没有上界 —— 只要没有别的有限序列，
     * 这就是一个死循环，不能当成"长度 0"。 */
    Buf lim; buf_init(&lim);
    bool found = false, unbounded = false;
    for (int i = 0; i < nseq && !found; i++) {
        Node *s = n->items[i];
        if (s->kind == NK_RANGE) {
            if (s->b) {
                buf_printf(&lim, "((size_t)(%s_lo_hi - %s_lo))", seqname[i], seqname[i]);
                buf_printf(&pre, "%s %s_lo_hi = %s;\n", cty(s->a->ty), seqname[i], cxs(s->b, s->a->ty));
                found = true;
            } else {
                unbounded = true;
            }
        } else {
            buf_printf(&lim, "%s.len", seqname[i]);
            found = true;
        }
    }
    if (!found) buf_puts(&lim, "0");
    bool infinite = !found && unbounded;

    /* 前置声明要在循环之前输出 */
    {
        /* pre 里可能有后补的行，逐行输出 */
        buf_puts(g_out, "");
        char *p = pre.data;
        while (p && *p) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            line("%s", p);
            if (!nl) break;
            p = nl + 1;
        }
    }

    char iv[32];
    snprintf(iv, sizeof iv, "_i%d", fid);
    if (infinite) line("for (size_t %s = 0; ; %s++) {", iv, iv);
    else          line("for (size_t %s = 0; %s < %s; %s++) {", iv, iv, lim.data, iv);
    g_ind++;
    CScope s; cscope_push(&s);

    for (int i = 0; i < nbind && i < 8; i++) {
        Node *b = n->items[nseq + i];
        if (!b || b->kind != NK_IDENT) continue;
        Type *bt = b->ty;
        if (i < nseq) {
            Node *sq = n->items[i];
            if (sq->kind == NK_RANGE) {
                line("%s %s = (%s)(%s_lo + %s);", cty(bt), cnames(b->name), cty(bt), seqname[i], iv);
            } else {
                line("%s %s = %s.ptr[%s];", cty(bt), cnames(b->name), seqname[i], iv);
            }
        } else {
            line("%s %s = (%s)%s;", cty(bt), cnames(b->name), cty(bt), iv);
        }
    }

    emit_loop_cancel_check();
    gen_block_stmts(n->b);
    line("Lcont%d: ;", id);
    cscope_leave();
    g_ind--;
    line("}");
}

/* ---- switch ---- */

/* 聚合主语（?T / !T / 带标签联合体）的 switch 头部：C 的 switch 吃不了
 * 结构体，得换成判别值。返回静态轮换缓冲，嵌套 switch 也安全。 */
static const char *tagref(const char *tmp, Type *st) {
    static char bufs[4][80];
    static int rot;
    char *b = bufs[rot++ & 3];
    if (st && st->kind == TY_OPT && ty_opt_flat(st))
        snprintf(b, 80, "(int)((%s) != 0)", tmp);   /* ?*T：标签即判空（M26） */
    else
        snprintf(b, 80, "%s.tag", tmp);
    return b;
}

static void gen_switch(Node *n, const char *slot, Type *slot_ty) {
    Type *st = n->a->ty;
    bool subj_agg = st && (st->kind == TY_OPT || st->kind == TY_ERR ||
                           (st->kind == TY_UNION && st->is_tagged));
    /* 有捕获臂、或者主语是聚合（C 的 switch 吃不了结构体）：
     * 先把主语落进临时量，头部用判别值，臂里用临时量取载荷。 */
    bool hoist = subj_agg;
    for (int i = 0; i < n->nitems && !hoist; i++)
        if (n->items[i]->name) hoist = true;

    const char *hdr;
    static int swn;
    char tmp[32];
    if (hoist) {
        snprintf(tmp, sizeof tmp, "_sw%d", swn++);
        line("%s %s = %s;", cty(st), tmp, cxs(n->a, st));
        hdr = subj_agg ? tagref(tmp, st) : tmp;
        buf_putc(g_out, '\n');
        if (st && st->kind == TY_BOOL)
            line("switch ((int)%s) {", hdr);
        else
            line("switch (%s) {", hdr);
    } else {
        buf_putc(g_out, '\n');
        /* C 的 `switch (a_bool)` 会触发 -Wswitch-bool：主语是 bool 时先转 int，
         * 臂里的 `case true:` / `case false:` 本来就是 1 / 0。 */
        if (st && st->kind == TY_BOOL)
            line("switch ((int)%s) {", exs_cond(n->a));
        else
            line("switch %s {", exs_cond(n->a));   /* exs_cond 自带括号 */
    }

    int end = ++g_labid;
    int narm = n->nitems;

    for (int i = 0; i < narm; i++) {
        Node *arm = n->items[i];
        g_ind++;
        if (arm->flag) {
            line("default: {");
        } else {
            /* 多个模式共享一个体 */
            for (int k = 0; k < arm->nitems; k++) {
                Node *p = arm->items[k];
                Buf pb; buf_init(&pb);
                if (p->kind == NK_FIELD && !p->a && p->name) {
                    /* 枚举变体 / ?T / !T / 带标签联合体的模式 */
                    if (st && st->kind == TY_ENUM) {
                        Buf nm; buf_init(&nm); tn(&nm, st);
                        buf_printf(&pb, "%s_", nm.data);
                        mname(&pb, p->name);
                    } else if (st && st->kind == TY_OPT) {
                        buf_puts(&pb, !strcmp(p->name, "some") ? "true" : "false");
                    } else if (st && st->kind == TY_ERR) {
                        buf_puts(&pb, !strcmp(p->name, "ok") ? "0" : "*");
                    } else if (st && st->kind == TY_UNION && st->is_tagged) {
                        /* 模式 → 标签判别值：标签是枚举就发枚举变体名，
                         * 是整数就发字面量（显式标签联合体允许自定义数值） */
                        Field *f = NULL;
                        for (int q = 0; q < st->nfields; q++)
                            if (st->fields[q].name && !strcmp(st->fields[q].name, p->name))
                                { f = &st->fields[q]; break; }
                        if (f && st->tag_ty && st->tag_ty->kind == TY_ENUM) {
                            ensure_type(st->tag_ty);
                            Buf nm; buf_init(&nm); tn(&nm, st->tag_ty);
                            buf_printf(&pb, "%s_", nm.data);
                            mname(&pb, p->name);
                        } else if (f) {
                            buf_printf(&pb, "%llu", (unsigned long long)f->tag);
                        } else {
                            ex(&pb, p);
                        }
                    } else {
                        ex(&pb, p);
                    }
                } else {
                    ex(&pb, p);
                }
                if (strchr(pb.data, '*')) line("default:");   /* .err 的兜底写法 */
                else line("case %s:", pb.data);
            }
            line("{");
        }
        g_ind++;
        CScope s; cscope_push(&s);
        /* 捕获声明（M28）：联合体 → 变体载荷，?T/.some → payload，
         * !T 的 .err → 错误值，枚举 / 整数 / bool → 主语本身 */
        if (arm->name && !arm->flag) {
            Node *pat = arm->nitems ? arm->items[0] : NULL;
            const char *vname = pat && pat->kind == NK_FIELD ? pat->name : NULL;
            Type *cap = NULL;
            const char *src = NULL;
            if (st && st->kind == TY_UNION && st->is_tagged) {
                for (int q = 0; q < st->nfields; q++)
                    if (st->fields[q].name && vname && !strcmp(st->fields[q].name, vname))
                        cap = st->fields[q].ty;
                if (cap) { Buf nb; buf_init(&nb); buf_printf(&nb, "%s.u.", tmp); mname(&nb, vname); src = nb.data; }
            } else if (st && st->kind == TY_OPT) {
                cap = st->elem;
                if (ty_opt_flat(st)) { src = tmp; }
                else { Buf nb; buf_init(&nb); buf_printf(&nb, "%s.payload", tmp); src = nb.data; }
            } else if (st && st->kind == TY_ERR) {
                bool is_err_pat = vname && !strcmp(vname, "err");
                if (is_err_pat) { cap = ty_error();  Buf nb; buf_init(&nb); buf_printf(&nb, "%s.tag", tmp); src = nb.data; }
                else            { cap = st->elem;    Buf nb; buf_init(&nb); buf_printf(&nb, "%s.payload", tmp); src = nb.data; }
            } else if (st) {
                cap = st;
                src = tmp;
            }
            /* §221：switch 捕获指针——取原主语（可写左值）载荷的地址，
             * 让 *t 写回原对象，而不是写进 hoist 的临时副本。flat optional
             * 没有 payload 字段，载荷就是主语指针本身，取主语变量地址。 */
            if (arm->cap_ptr) {
                Buf lv; buf_init(&lv);
                lvalue(&lv, n->a);
                if (st && st->kind == TY_UNION && st->is_tagged && cap && vname) {
                    Buf ab; buf_init(&ab);
                    buf_printf(&ab, "&(%s).u.", lv.data); mname(&ab, vname);
                    src = ab.data;
                } else if (st && st->kind == TY_OPT && !ty_opt_flat(st)) {
                    Buf ab; buf_init(&ab);
                    buf_printf(&ab, "&(%s).payload", lv.data);
                    src = ab.data;
                } else if (st && st->kind == TY_OPT && ty_opt_flat(st)) {
                    Buf ab; buf_init(&ab);
                    buf_printf(&ab, "&(%s)", lv.data);
                    src = ab.data;
                } else if (st && st->kind == TY_ERR) {
                    /* .err 捕获错误值取 tag 地址；.ok 取 payload 地址 */
                    bool err_pat = vname && !strcmp(vname, "err");
                    Buf ab; buf_init(&ab);
                    if (err_pat) buf_printf(&ab, "&(%s).tag", lv.data);
                    else         buf_printf(&ab, "&(%s).payload", lv.data);
                    src = ab.data;
                } else {
                    Buf ab; buf_init(&ab);
                    buf_printf(&ab, "&(%s)", lv.data);
                    src = ab.data;
                }
                buf_free(&lv);
            }
            /* 0 字节的载荷（`!void` 的 `.ok`）没有可声明的 C 变量；
             * 语义层已经就此报错，这里只是别生成 `void v = …`。 */
            if (cap && src && cap->kind != TY_VOID) {
                if (arm->cap_ptr)
                    line("%s *%s = %s;", cty(cap), cnames(arm->name), src);
                else
                    line("%s %s = %s;", cty(cap), cnames(arm->name), src);
            }
        }
        if (arm->label) {
            CLab *l = &g_labs[g_nlabs++];
            l->tag = arm->label; l->slot = slot; l->slot_ty = slot_ty;
            l->end = end; l->cont = 0; l->depth = g_depth;
            gen_block_stmts(arm->a);
            g_nlabs--;
        } else if (arm->a && arm->a->kind == NK_BLOCK) {
            gen_block_stmts(arm->a);
        } else if (arm->a) {
            if (slot) line("%s = %s;", slot, cxs(arm->a, slot_ty));
            else      gen_stmt(arm->a);
        }
        cscope_leave();
        g_ind--;
        line("}");
        line("break;");
        g_ind--;
    }
    line("}");
    line("Lend%d: ;", end);
    (void)st;
}

/* ======================================================================
 *  函数
 * ====================================================================== */

static void gen_fn(Node *fn, const char *name, bool is_main, bool exported);

/* 预扫函数体：把里面的局部 fn 绑定先发成同单元的 static C 函数。
 * C 没有嵌套函数，函数定义不能落在函数体中间；原型进 g_bproto、
 * 体进 g_bbody，发射顺序无所谓，最终 C 文件里原型永远在前面。
 * 递归：局部 fn 的体里还能再有局部 fn（gen_fn 内部会再调本函数）。
 * fn 字面量声明节点不往 b 里走——那棵子树由这次 gen_fn 接管，重复
 * 走会发两份函数定义（C 重定义错误）。 */
/* B13：扫描函数体是否含 errdefer（决定要不要启用声明提升） */
static bool has_errdefer(Node *n) {
    if (!n) return false;
    if (n->kind == NK_ERRDEFER) return true;
    if (has_errdefer(n->a) || has_errdefer(n->b) ||
        has_errdefer(n->c) || has_errdefer(n->d)) return true;
    for (int i = 0; i < n->nitems; i++)
        if (has_errdefer(n->items[i])) return true;
    for (int i = 0; i < n->nunroll; i++)
        if (has_errdefer(n->unroll[i])) return true;
    return false;
}

/* B13：为一个局部声明发"裸声明"（只有类型+名字，不初始化）。
 * 与 gen_decl_inner 的类型口径一致：数组指针 *[N]T → T *name；
 * 其余 → cty(t) name。跳过编译期绑定 / void / 局部 fn 绑定。 */
static void hoist_decl_one(Node *d) {
    Type *t = d->ty;
    if (!t || t->kind == TY_INVALID || t->kind == TY_TYPE) return;
    if (sema_ty_ct_only(t)) return;
    if (!sema_sym_has_storage(d->sym)) return;
    if (t->kind == TY_VOID) return;
    /* 局部 fn 绑定也要声明槽：gen_decl_inner 在 hz 模式下会发 `cn = impl`
     * 初始化赋值（函数体已由 gen_local_fns 发成 static 函数）。 */
    const char *nm = sym_cname(d);
    char *cn = (nm && !strcmp(nm, "_")) ? ignored_cname() : cnames(nm);
    if (ty_is_array_ptr(t))
        line("%s *%s;", cty(t->elem), cn);
    else
        line("%s %s;", cty(t), cn);
}

/* B13：遍历函数体，把所有局部声明的裸声明发到函数入口。
 * 与 gen_local_fns 同样的子树口径（a/b/c/d/items/unroll）。
 * 注意：嵌套局部 fn 的函数体不属于本函数，遇到 NK_FN 不深入。 */
static void hoist_local_decls(Node *n) {
    if (!n) return;
    if (n->kind == NK_FN) return;
    if (n->kind == NK_DECL) hoist_decl_one(n);
    hoist_local_decls(n->a);
    hoist_local_decls(n->b);
    hoist_local_decls(n->c);
    hoist_local_decls(n->d);
    for (int i = 0; i < n->nitems; i++) hoist_local_decls(n->items[i]);
    for (int i = 0; i < n->nunroll; i++) hoist_local_decls(n->unroll[i]);
}

static void gen_local_fns(Node *n) {
    if (!n) return;
    if (n->kind == NK_DECL && n->b && n->b->kind == NK_FN) {
        Node *lfn = n->b;
        if (lfn->b && !fn_is_generic(lfn)) {
            char *nm = localfn_impl_name(n);
            gen_fn(lfn, nm, false, false);
            free(nm);
        }
        return;
    }
    gen_local_fns(n->a);
    gen_local_fns(n->b);
    gen_local_fns(n->c);
    for (int i = 0; i < n->nitems; i++) gen_local_fns(n->items[i]);
    for (int i = 0; i < n->nunroll; i++) gen_local_fns(n->unroll[i]);
}

/* 跨模块可见的函数原型（不带 static），最后进共享头文件 */
static Buf  g_bpub;
/* 每个模块的延迟初始化函数名，main 里依次调用 */
static char **g_initname;           /* 动态：一个模块一个 init 函数（§203） */
static int    g_init_cap;
static int    g_ninit;

static void gen_params(Buf *b, Node *fn, bool is_main) {
    /* 形参别叫 argc/argv：用户局部变量是**原名**发的，叫 `argv` 就跟 C 层形参重定义。
     * 翻译错误还是 "redefinition of 'argv' with a different
     * type" —— 读起来像用户的错。 */
    if (is_main && g_need_proc) {          /* std.process 要接住命令行 */
        buf_puts(b, "int zx_argc, char **zx_argv");
        return;
    }
    Type *ft = fn->type_val;
    int n  = fn->nitems;
    int nc = sema_fn_ncomp(fn);          /* 泛型实例：前 nc 个参数只在编译期存在 */
    if (n <= nc) { buf_puts(b, "void"); return; }
    for (int i = nc; i < n; i++) {
        Node *pm = fn->items[i];
        if (i > nc) buf_puts(b, ", ");
        Type *pt = (ft && i < ft->nparams) ? ft->params[i] : pm->ty;
        ctype(b, pt);
        buf_putc(b, ' ');
        if (pm->kind == NK_PARAM && pm->name && strcmp(pm->name, "_")) cname(b, pm->name);
        else buf_printf(b, "_p%d", i);
    }
}

/* extern fn：只发一条 `extern` 原型进共享头，不生成函数体。
 * 名字**原样**发（不是 cname）：它指向的是别人已经编好的符号（libc 等），
 * 加模块前缀就链接不到了（M23）。 */
static void gen_extern(Node *fn) {
    Type *ft = fn->type_val;
    Type *ret = (ft && ft->kind == TY_FN) ? ft->ret : ty_void();
    /* 标准头已经声明过的名字（malloc / gets / memcpy…）不再重复声明：
     * C 里 `char* malloc(uint64_t)` 和 `void* malloc(size_t)` 是冲突类型。
     * 用头里的声明，调用点仍按 Zx 这边的签名做转换。 */
    if (fn->name && libc_declared(fn->name)) return;
    Buf sig; buf_init(&sig);
    buf_puts(&sig, "extern ");
    ctype_c(&sig, ret);
    buf_putc(&sig, ' ');
    buf_puts(&sig, fn->name ? fn->name : "anon");
    buf_putc(&sig, '(');
    int n = fn->nitems;
    if (n == 0) buf_puts(&sig, "void");
    for (int i = 0; i < n; i++) {
        Node *pm = fn->items[i];
        Type *pt = (ft && i < ft->nparams) ? ft->params[i] : pm->ty;
        if (i) buf_puts(&sig, ", ");
        ctype_c(&sig, pt);
        buf_putc(&sig, ' ');
        /* 无名形参必须按位置编号：所有参数都叫 `_p` 会让多参 extern 原型
         * 变成 `double atan2(double _p, double _p)`，C 直接报参数重定义。
         * 与 emit_params 的 `_p%d` 保持同一套命名。 */
        if (pm->name) buf_puts(&sig, pm->name);
        else buf_printf(&sig, "_p%d", i);
    }
    buf_putc(&sig, ')');
    bline(&g_bpub, "%s;", sig.data);
}

/* 自动内联的规模判据（decisions §138）。
 *
 * dev 档是 -O0，C 编译器一次内联都不做——而 zx 的 std 里满是"一行包装"
 * （align16 / classPop / 字段 getter / 分配器转发）。这里给它们打上
 * always_inline，省掉调用/返回与寄存器保存。
 *
 * 判据保守到只看形状：节点数 <= 24、没有循环、没有嵌套 fn、不递归。
 * 超出的交给 C 编译器（-O2 自己会内联）。不做收益分析——那需要 profile，
 * 编译期拿不到，硬猜只会让编译变慢。 */
typedef struct { bool loop; bool self; } InlScan;

static void inl_scan(Node *n, const char *cname_, const char *src_name,
                     InlScan *st, int *cnt, int depth) {
    if (!n || depth > 24) return;
    *cnt = *cnt + 1;
    if (n->kind == NK_FOR || n->kind == NK_WHILE) st->loop = true;
    if (n->kind == NK_FN) { st->loop = true; return; }
    if (n->kind == NK_CALL && n->a) {
        Node *c = n->a;
        while (c && (c->kind == NK_FIELD || c->kind == NK_UNWRAP)) c = c->a;
        if (c && c->kind == NK_IDENT) {
            const char *cn = sym_cname(c);
            if ((cname_ && cn && !strcmp(cn, cname_)) ||
                (src_name && c->name && !strcmp(c->name, src_name)))
                st->self = true;
        }
    }
    inl_scan(n->a, cname_, src_name, st, cnt, depth + 1);
    inl_scan(n->b, cname_, src_name, st, cnt, depth + 1);
    inl_scan(n->c, cname_, src_name, st, cnt, depth + 1);
    inl_scan(n->d, cname_, src_name, st, cnt, depth + 1);
    for (int i = 0; i < n->nitems; i++)
        inl_scan(n->items[i], cname_, src_name, st, cnt, depth + 1);
}

static bool fn_small_for_inline(Node *fn, const char *name) {
    if (!fn || !fn->b || fn->is_extern) return false;
    InlScan st = { false, false };
    int cnt = 0;
    inl_scan(fn->b, name, fn->name, &st, &cnt, 0);
    if (st.loop || st.self) return false;
    return cnt <= 24;
}

static void gen_fn(Node *fn, const char *name, bool is_main, bool exported) {
    Type *ft = fn->type_val;
    Type *ret = (ft && ft->kind == TY_FN) ? ft->ret : ty_void();
    /* 自动内联：只对**本模块私有**的函数有意义——exported 要给别的单元
     * 调（那边只有原型，没有函数体），main 也不能 inline。 */
    bool inl = !is_main && !exported && fn_small_for_inline(fn, name);

    Buf sig; buf_init(&sig);
    if (is_main) {
        buf_puts(&sig, "int ");
        buf_puts(&sig, "main");
    } else {
        ctype(&sig, ret);
        buf_putc(&sig, ' ');
        cname(&sig, name ? name : "anon");
    }
    buf_putc(&sig, '(');
    gen_params(&sig, fn, is_main);
    buf_putc(&sig, ')');

    /* 原型一直是 static：属性只写在定义处（同单元 clang 会用定义处的），
     * 别的单元本来就看不见 static 函数。 */
    if (is_main) bline(&g_bproto, "%s;", sig.data);
    else bline(exported ? &g_bpub : &g_bproto, exported ? "%s;" : "static %s;", sig.data);

    /* 函数体内的局部 fn 绑定：先把它们发成同单元的 static 函数，
     * gen_decl_inner 再把局部槽初始化成函数地址（见 gen_local_fns）。 */
    gen_local_fns(fn->b);

    g_out = &g_bbody;
    if (is_main && g_need_proc) {
        /* std.process 的数据源：main 收到的 argc/argv 存进这两份
         * 外部链接的全局量（运行时辅助在共享头里声明为 extern）。 */
        line("int zx_proc_argc = 0;");
        line("char **zx_proc_argv = 0;");
    }
    if (is_main || exported) line("%s {", sig.data);
    else if (inl) line("ZX_INLINE %s {", sig.data);
    else line("static %s {", sig.data);
    g_ind++;
    if (is_main) {
        line("zx_crash_init();");   /* §211: 崩溃自动打点，在任何用户代码前装好 */
        for (int i = 0; i < g_ninit; i++) line("%s();", g_initname[i]);
        if (g_need_proc) {
            line("zx_proc_argc = zx_argc;");
            line("zx_proc_argv = zx_argv;");
        }
    }

    /* await 门（decisions §88）：栈有纤程下，`!T` 函数的入口就是 await 点。
     * 在纤程里：先挂起回 resumer（调度点），续跑回来后看取消旗标——被取消
     * 就以 error.cancelled 走 !T 传播链，顺着 try 一路冒到捕获点。
     * 根上下文里这是一次纯 false 检查，直接进函数体。 */
    if (!is_main && g_need_task && ret->kind == TY_ERR) {
        line("if (zx_task_await_gate()) return (%s){ .tag = %u };",
             cty(ret), (unsigned)ty_err_code("cancelled"));
    }

    g_cur_ret = ret;
    g_is_main = is_main;
    g_saw_errdefer = false;
    g_cur_fn_zx = fn->name ? fn->name : "?";

    /* B13 bug2：含 errdefer 的函数启用局部变量声明提升——defer 体在 try
     * 展开点就地发射，可能引用其后才声明的局部变量。先在函数入口发所有
     * 局部变量的裸声明，gen_decl_inner 只发初始化赋值。无 errdefer 的
     * 函数完全走原路径，零影响。 */
    g_hoist_locals = has_errdefer(fn->b);
    if (g_hoist_locals && fn->b) hoist_local_decls(fn->b);

    g_depth = 0;
    g_csc = NULL;
    g_in_fn = true;
    CScope s; cscope_push(&s);
    if (fn->b) gen_block_stmts(fn->b);
    /* 函数末尾：展开 defer，补上隐式 return */
    run_defers(0);
    cscope_pop();

    if (ret->kind == TY_VOID || ret->kind == TY_NORETURN || is_main)
        line("return%s;", is_main ? " 0" : "");
    else if (ret_is_errvoid(ret)) line("return (%s){ .tag = 0 };", cty(ret));
    g_ind--;
    line("}");
    buf_putc(g_out, '\n');
    g_cur_ret = NULL;
    g_is_main = false;
    g_in_fn = false;
    g_hoist_locals = false;
    g_cur_fn_zx = NULL;
}

/* ======================================================================
 *  顶层
 * ====================================================================== */

static bool fn_is_generic(Node *fn) {
    return sema_fn_is_generic(fn);
}

/* 编译期值 → C 初始化器 */
static void val_init(Buf *b, Value *v, Type *t) {
    if (!v) { buf_puts(b, "{0}"); return; }
    /* 标量放进 ?T / !T：补上载荷+标签；?*T 无标签（M26），值即本体 */
    if (t && (t->kind == TY_OPT || t->kind == TY_ERR) && !ty_opt_flat(t) &&
        v->kind != V_AGG && v->kind != V_NULL && v->kind != V_UNDEF && v->kind != V_VOID) {
        buf_puts(b, "{ ");
        val_init(b, v, t->elem);
        buf_puts(b, t->kind == TY_OPT ? ", true }" : ", 0 }");
        return;
    }
    switch (v->kind) {
    case V_INT:
        if (v->big) { emit_big128(b, v->hi, v->ival, v->neg); return; }
        if (v->neg) buf_printf(b, "(-%llu%s)", (unsigned long long)v->ival,
                               t && t->kind == TY_INT && t->bits > 32 ? "ULL" : "");
        else buf_printf(b, "%llu%s", (unsigned long long)v->ival,
                        t && t->kind == TY_INT && t->bits > 32 ? "ULL" : "");
        return;
    case V_FLOAT:
        buf_printf(b, "%.17g", v->fval);
        return;
    case V_BOOL:
        buf_puts(b, v->bval ? "true" : "false");
        return;
    case V_STR: {
        Buf s; buf_init(&s);
        cstr(&s, v->sval ? v->sval : "", v->slen);
        /* 定长数组（*[N]u8）在 C 里就是 char 数组，只给指针；其余一律按
         * 切片（ptr+len）初始化——字符串在 Zx 里的类型就是 []const u8。 */
        if (t && t->kind == TY_PTR && t->has_len) {
            buf_printf(b, "{ (uint8_t*)%s }", s.data);
        } else {
            buf_printf(b, "{ (uint8_t*)%s, %d }", s.data, v->slen);
        }
        return;
    }
    case V_NULL:
        /* `error` 上的 null 是保留码 65535，不是 0（0 = "没有错误"） */
        if (t && t->kind == TY_ERROR) buf_puts(b, "65535");
        else buf_puts(b, "{0}");
        return;
    case V_UNDEF:
    case V_VOID:
        buf_puts(b, "{0}");
        return;
    case V_AGG: {
        /* 数组字面量：先把外壳剥掉，元素类型单独记着。剥完 `t` 就不再是
         * 容器了，所以下面取元素类型时不能靠 `t` 自己判断——否则
         * `&.{ "a", "b" }` 的每个元素都拿不到类型，字符串长度被丢掉。 */
        Type *arr_et = NULL;
        if (t && t->kind == TY_ARRAY) arr_et = t->elem;
        else if (ty_is_array_ptr(t)) { arr_et = t->elem->elem; t = t->elem; }
        buf_puts(b, "{ ");
        bool first = true;
        for (int i = 0; i < v->nitems; i++) {
            Type *et = arr_et;
            if (!et && t) {
                if (t->kind == TY_TUPLE && i < t->nfields) et = t->fields[i].ty;
                else if ((t->kind == TY_STRUCT || t->kind == TY_UNION) && i < t->nfields) {
                    if (t->fields[i].is_static) continue;    /* 静态成员不在 C 结构体里 */
                    et = t->fields[i].ty;
                }
                else if (t->kind == TY_SLICE) et = t->elem;  /* 切片字面量 */
            }
            if (!first) buf_puts(b, ", ");
            first = false;
            val_init(b, v->items[i], et);
        }
        buf_puts(b, " }");
        return;
    }
    case V_PTR:
        if (v->base) {
            /* §197：字符串字面量的 .ptr 折成 V_PTR(base=V_STR)。它是静态
             * 只读数据地址，是编译期常量指针——必须发裸指针 `((uint8_t*)s)`，
             * 不能递归 val_init(base) 那样会按切片形状发 `{ptr, len}`（元素
             * 类型是裸指针时就形状不匹配）。off 是子串偏移，按字节加。 */
            if (v->base->kind == V_STR) {
                Buf s; buf_init(&s);
                cstr(&s, v->base->sval ? v->base->sval : "", v->base->slen);
                if (v->off)
                    buf_printf(b, "((uint8_t*)%s + %llu)", s.data,
                               (unsigned long long)v->off);
                else
                    buf_printf(b, "((uint8_t*)%s)", s.data);
                return;
            }
            val_init(b, v->base, t && t->kind == TY_PTR ? t->elem : t);
            return;
        }
        buf_puts(b, "{0}");
        return;
    default:
        buf_puts(b, "{0}");
        return;
    }
}

/* 这个初始化器能不能直接当 C 的静态初始化？（不能的交给 zx_init_globals） */
static bool is_const_init(Node *n) {
    if (!n) return true;
    switch (n->kind) {
    case NK_INT: case NK_FLOAT: case NK_STR: case NK_CHAR:
    case NK_BOOL: case NK_NULL: case NK_UNDEF: case NK_BUILTIN:
        return true;
    case NK_UN: case NK_CAST: case NK_BIN: case NK_PAIR:
        return is_const_init(n->a) && is_const_init(n->b);
    case NK_TUPLE: case NK_ARRAY_LIT: case NK_STRUCT_LIT: case NK_ANON_STRUCT:
        for (int i = 0; i < n->nitems; i++)
            if (!is_const_init(n->items[i])) return false;
        return true;
    case NK_IDENT:
        /* 标量常量是 C 常量表达式；切片/聚合是"读一块内存"，不是 */
        return n->ty && n->ty->kind != TY_SLICE && !ty_is_agg(n->ty);
    case NK_FIELD: {
        if (!n->name) return false;
        /* 同 ex()：聚合体自己的同名字段优先，那不是编译期常量 */
        Type *bt = n->a ? n->a->ty : NULL;
        /* §197：裸指针穿透一层查字段——`p.size` 若是 S 的字段就不是常量 */
        Type *probe = bt;
        if (probe && probe->kind == TY_PTR && !probe->has_len &&
            !ty_is_array_ptr(probe)) probe = probe->elem;
        if (probe && (probe->kind == TY_STRUCT || probe->kind == TY_UNION) &&
            ty_field(probe, n->name))
            return false;
        /* §154：切片自己存了 len，.len 和 .size 都是运行期值；
         * 只有长度写在类型里（*[N]T / 元组 / 聚合）才是 C 常量表达式。 */
        if (!strcmp(n->name, "size") || !strcmp(n->name, "len"))
            return !(bt && bt->kind == TY_SLICE);
        return false;
    }
    default:
        return false;
    }
}

/* 初值是不是"视图"——一个指向已有对象的地址（arr[0..] / &x / 它们的转换）。
 * 这种值不能物化成静态数据：那样会把数据复制一份，别名关系就断了。
 * 反过来，块 / 调用 / if 这些是编译期算出来的东西，物化才对。 */
static bool is_view_expr(Node *n) {
    if (!n) return false;
    switch (n->kind) {
    case NK_SLICE: case NK_UN: case NK_CAST: case NK_IDENT:
        return true;
    case NK_FIELD:
        /* §197：字符串字面量的 .ptr 指向静态只读数据，是货真价实的编译期
         * 地址常量——不是"指向已有可变对象的视图"，物化不会断别名。放行
         * 它走 val_init 静态初始化（生成 `((uint8_t*)"...")`）。 */
        if (n->name && !strcmp(n->name, "ptr") && n->a && n->a->kind == NK_STR)
            return false;
        return true;
    /* 指针算术的结果仍是视图：物化就成了"另拷一份数据" */
    case NK_BIN:
        return n->ty && n->ty->kind == TY_PTR &&
               (n->op == TOK_PLUS || n->op == TOK_MINUS);
    default:
        return false;
    }
}

/* §200：匿名 fn 字面量当值用的 C 名字登记。ex(NK_FN) 把节点登记进来，
 * 单元正文发完后 flush_anon_fns 统一发函数体——不能在 ex() 中途调用
 * gen_fn：它会改 g_out/g_ind、压 CScope，套在半条表达式里会错位。 */
typedef struct { Node *fn; const char *name; } AnonFn;
static AnonFn *g_anonfns;
static int     g_nanon, g_canon;

static char *anonfn_cname(Node *fn) {
    for (int i = 0; i < g_nanon; i++)
        if (g_anonfns[i].fn == fn) return (char *)g_anonfns[i].name;
    char buf[80];
    snprintf(buf, sizeof buf, "anonfn_%d_%d", g_cur_unit, g_nanon);
    char *nm = malloc(strlen(buf) + 1);
    strcpy(nm, buf);
    if (g_nanon == g_canon) {
        g_canon = g_canon ? g_canon * 2 : 8;
        g_anonfns = realloc(g_anonfns, sizeof(AnonFn) * (size_t)g_canon);
    }
    g_anonfns[g_nanon].fn = fn;
    g_anonfns[g_nanon].name = nm;
    g_nanon++;
    return nm;
}

/* 把一个单元登记过的匿名 fn 全部发成 static C 函数。gen_fn 内部可能
 * 又登记新的（嵌套匿名 fn），循环到不再增长为止。 */
static void flush_anon_fns(void) {
    int done = 0;
    while (done < g_nanon) {
        Node *fn = g_anonfns[done].fn;
        const char *nm = g_anonfns[done].name;
        done++;
        gen_fn(fn, nm, false, false);
    }
    g_nanon = 0;                 /* 跨单元：名字带单元号，不复用 */
}

static Node **g_late;      /* 需要运行时初始化的全局 */
static int    g_nlate, g_clate;

/* §150：编译期 Value 里带着函数引用（V_FN）时，val_init 没有函数符号的
 * 表示，会静默折成 {0} —— `var G = Allocator{ .alloc = cAlloc, ... };`
 * 的全局函数指针字段全成 NULL，解引用就段错误。这类聚合不能物化成静态
 * 数据，改由 zx_init_<unit>() 在 main 开头按源码表达式赋值。 */
static bool val_has_fn(Value *v) {
    if (!v) return false;
    if (v->kind == V_FN) return true;
    if (v->kind == V_AGG)
        for (int i = 0; i < v->nitems; i++)
            if (val_has_fn(v->items[i])) return true;
    if (v->kind == V_PTR && v->base) return val_has_fn(v->base);
    return false;
}

static void gen_global(Node *d, bool exported) {
    const char *st = exported ? "" : "static ";
    Type *t = d->ty;
    if (!t || t->kind == TY_INVALID || t->kind == TY_TYPE) return;
    if (t->kind == TY_VOID) return;      /* `const x = { 42; }` 没有运行时表示 */
    /* Type / Field 描述符只活在编译期：使用点全部折成字面量，
     * 全局里不物化（跟 gen_decl_inner 同一条规则）。 */
    if (sema_ty_ct_only(t)) return;

    /* extern var：外部 C 全局变量——只发一条 extern 声明进共享头，
     * 不定义存储（定义在 C 那边）。名字原样发，跟 extern fn 同一条规则。 */
    if (d->is_extern) {
        ensure_type(t);
        Buf e; buf_init(&e);
        buf_puts(&e, "extern ");
        ctype(&e, t);
        buf_printf(&e, " %s;", d->name ? d->name : "_");
        buf_puts(&g_bpub, e.data);
        return;
    }

    /* 别名（const heap = std.heap;）不占存储：它连的是模块里那份 */
    if (!sema_sym_has_storage(d->sym)) return;

    /* 解构：const {a, b} = tup; —— 每个分量都是一个独立的全局 */
    if (d->c) {
        for (int k = 0; k < d->c->nitems; k++) {
            Node *id = d->c->items[k];
            Type *it = id->ty;
            if (!it || it->kind == TY_INVALID || it->kind == TY_VOID) continue;
            const char *inm = sym_cname(id);
            char *inn = (inm && !strcmp(inm, "_")) ? ignored_cname() : cnames(inm);
            if (!id->ct) {
                err_at(id->line, id->col,
                       "destructuring at file scope needs a compile-time value (R1)");
                continue;
            }
            Buf s; buf_init(&s);
            val_init(&s, id->ct, it);
            bline(&g_bglobals, "%s%s %s = %s;", st, cty(it), inn, s.data);
        }
        return;
    }

    const char *gnm = sym_cname(d);
    char *cn = (gnm && !strcmp(gnm, "_")) ? ignored_cname() : cnames(gnm);
    if (exported) {                     /* 别的单元要能链到：extern 声明进头文件 */
        Buf e; buf_init(&e);
        buf_puts(&e, "extern ");
        ctype(&e, t);
        buf_printf(&e, " %s;\n", cn);
        buf_puts(&g_bpub, e.data);
    }

    /* §201：数组值 `[N]T` 的全局也用局部同一套 `struct { T v[N]; }` 表示。
     * 以前这里图省事发裸 C 数组 `T g[N]`，于是 `.ptr` 的 `&g.v[0]`、下标的
     * `(g).v[i]` 在全局上全都发不出合法的 C（B1001）；extern 声明早已走
     * ctype() 的结构体名，这里必须一致。字符串字面量是 *[N]const u8 指针，
     * 走下面普通标量分支，不会误物化成 T[N]。 */
    if (t->kind == TY_ARRAY) {
        Node *init = d->b;
        Buf s; buf_init(&s);
        buf_puts(&s, st);
        ensure_type(t);
        ctype(&s, t);
        buf_printf(&s, " %s", cn);
        if (init && init->ct && init->kind != NK_UNDEF) {
            buf_puts(&s, " = ");
            val_init(&s, init->ct, t);
        } else if (init && init->kind != NK_UNDEF && init->nitems) {
            buf_puts(&s, " = { ");
            for (int i = 0; i < init->nitems; i++) {
                if (i) buf_puts(&s, ", ");
                coerce(&s, init->items[i], t->elem);
            }
            buf_puts(&s, " }");
        }
        buf_puts(&s, ";");
        bline(&g_bglobals, "%s", s.data);
        return;
    }

    if (!d->b) {
        bline(&g_bglobals, "%s%s %s = {0};", st, cty(t), cn);
        return;
    }
    if (d->b->kind == NK_UNDEF) {
        bline(&g_bglobals, "%s%s %s = {0};", st, cty(t), cn);
        return;
    }

    /* 编译期值：直接物化成静态数据（块/循环算出来的表也走这里）。
     * 视图例外——它是个地址，复制数据会让别名关系断掉。 */
    bool view = d->b->ct && d->b->ct->kind == V_PTR && d->b->ct->base &&
                is_view_expr(d->b);
    bool has_fn = d->b->ct && val_has_fn(d->b->ct);
    if (d->b->ct && !view && !has_fn) {
        /* §199/§201：`const G: []const T = &.{ ... }` / `const G: *[N]T = &.{ ... }`
         * 的编译期值是 V_PTR(base=V_AGG)。val_init 会把 V_AGG 发成元素序列
         * `{ e0, e1, ... }`，可目标是 slice（`{ptr,len}`）或数组指针（一个地址）
         * ——于是得到 `ZxSlice_T G = { 1, 2, 3 }` / `ZxArr_T *G = { 1, 2, 3 }`
         * 被 C 拒（B1002）。正确做法：先发一份静态 backing，再发 {ptr,len} / &backing。 */
        if ((t->kind == TY_SLICE || ty_is_array_ptr(t)) && d->b->ct->kind == V_PTR &&
            d->b->ct->base && d->b->ct->base->kind == V_AGG) {
            Value *arr = d->b->ct->base;
            bool arrptr = ty_is_array_ptr(t);
            Type *et = arrptr ? t->elem->elem : t->elem;
            if (arr->nitems > 0) {
                char *lit = ignored_cname();
                Buf a; buf_init(&a);
                buf_puts(&a, st);
                if (arrptr) {
                    /* 数组指针指向的是 [N]T，C 表示是 ZxArr_… 结构体，
                     * backing 必须同类型，&backing 才能直接赋给 *ZxArr_… */
                    ensure_type(t->elem);
                    ctype(&a, t->elem);
                    buf_printf(&a, " %s = { .v = { ", lit);
                    for (int i = 0; i < arr->nitems; i++) {
                        if (i) buf_puts(&a, ", ");
                        val_init(&a, arr->items[i], et);
                    }
                    buf_puts(&a, " } };");
                } else {
                    ctype(&a, et);
                    buf_printf(&a, " %s[%d] = { ", lit, arr->nitems);
                    for (int i = 0; i < arr->nitems; i++) {
                        if (i) buf_puts(&a, ", ");
                        val_init(&a, arr->items[i], et);
                    }
                    buf_puts(&a, " };");
                }
                bline(&g_bglobals, "%s", a.data);

                Buf s; buf_init(&s);
                if (arrptr) {
                    buf_printf(&s, "%s%s %s = &%s;", st, cty(t), cn, lit);
                } else {
                    buf_printf(&s, "%s%s %s = { (", st, cty(t), cn);
                    ctype(&s, et);
                    buf_printf(&s, "*)%s, %d };", lit, arr->nitems);
                }
                bline(&g_bglobals, "%s", s.data);
            } else {
                bline(&g_bglobals, "%s%s %s = {0};", st, cty(t), cn);
            }
            return;
        }
        Buf s; buf_init(&s);
        val_init(&s, d->b->ct, t);
        bline(&g_bglobals, "%s%s %s = %s;", st, cty(t), cn, s.data);
        return;
    }

    /* 不是 C 常量表达式：先声明，等 main 开头再赋值 */
    if (view || has_fn || !is_const_init(d->b) || is_ctl(d->b)) {
        bline(&g_bglobals, "%s%s %s = {0};", st, cty(t), cn);
        if (g_nlate == g_clate) {
            g_clate = g_clate ? g_clate * 2 : 16;
            g_late = realloc(g_late, sizeof(Node *) * (size_t)g_clate);
        }
        g_late[g_nlate++] = d;
        return;
    }

    bline(&g_bglobals, "%s%s %s = %s;", st, cty(t), cn, cxs(d->b, t));
}

/* ======================================================================
 *  多 translation unit
 *
 *  一个模块一个 .c：类型定义、运行时、跨模块原型放共用的 .h，
 *  每个模块只发自己的函数体和全局数据。改一个模块只重编它自己。
 * ====================================================================== */

typedef struct { char *name; Buf src; } BUnit;

/* 文件名用的短名：入口是 main，其余取模块 stem */
static void unit_stem(int mi, char *buf, size_t n) {
    const char *s = (mi == 0) ? "main" : sema_module_stem(mi);
    if (!s || !*s) s = "mod";
    size_t k = 0;
    for (; s[k] && k + 1 < n; k++) {
        char c = s[k];
        buf[k] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_') ? c : '_';
    }
    buf[k] = 0;
}

/* 这个单元的延迟初始化函数（顶层 const 里算不出来的那些） */
static void emit_init_fn(int mi) {
    char stem[64], nm[96];
    unit_stem(mi, stem, sizeof stem);
    snprintf(nm, sizeof nm, "zx_init_%s", stem);

    g_out = &g_bbody;
    g_depth = 0;
    g_csc = NULL;
    line("void %s(void) {", nm);
    g_ind++;
    CScope s; cscope_push(&s);
    for (int i = 0; i < g_nlate; i++) {
        Node *d = g_late[i];
        Type *t = d->ty;
        const char *lnm = sym_cname(d);
        char *cn = (lnm && !strcmp(lnm, "_")) ? ignored_cname() : cnames(lnm);
        if (is_ctl(d->b)) gen_value(d->b, cn, t);
        else              line("%s = %s;", cn, cxs(d->b, t));
    }
    run_defers(0);
    cscope_pop();
    g_ind--;
    line("}");
    buf_putc(g_out, '\n');
    g_nlate = 0;
}

/* 把 Buf 里列首的 "static " 摘掉：运行时原型/函数体从"头文件内联 static"
 * 改为"外部链接 + 独立 zxrt 单元定义"。data_too=false 时带 '=' 的行
 * （数据定义，如 zx_alloc_page）保持 static——数据仍按单元各留一份，
 * 函数定义则必须全局只有一份。 */
static void buf_destatic(Buf *b, bool data_too) {
    if (!b->data || b->len == 0) return;
    Buf nb;
    buf_init(&nb);
    char *p = b->data, *end = b->data + b->len;
    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t ll = nl ? (size_t)(nl - p) + 1 : (size_t)(end - p);
        if (ll >= 7 && memcmp(p, "static ", 7) == 0 &&
            (data_too || !memchr(p, '=', ll)))
            buf_printf(&nb, "%.*s", (int)(ll - 7), p + 7);
        else
            buf_printf(&nb, "%.*s", (int)ll, p);
        p += ll;
    }
    free(b->data);
    *b = nb;
}

/* 从函数体缓冲提取原型（签名 = 定义首行到 '{' 之前）：运行时函数体搬去
 * zxrt 单元后，用户单元能看到的只有头文件——那些"定义即声明"的运行时
 * 函数（div 家族、unwrap 家族）必须自动补上原型，否则调用点成了隐式声明。
 * 规则：列首行含 '(' ')' 且 '{' 在 ')' 之后 → 函数定义；其余（数据定义、
 * #ifdef、空行）跳过。与显式原型重复的声明在 C 里合法。 */
static void buf_protos_from(Buf *bodies, Buf *protos) {
    if (!bodies->data) return;
    char *p = bodies->data, *end = bodies->data + bodies->len;
    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t ll = nl ? (size_t)(nl - p) : (size_t)(end - p);
        char *pa = memchr(p, '(', ll);
        char *br = memchr(p, '{', ll);
        if (ll > 0 && *p != ' ' && *p != '\t' && *p != '#' &&
            pa && br && pa < br && memchr(pa, ')', (size_t)(br - pa))) {
            size_t sl = (size_t)(br - p);
            while (sl && p[sl - 1] == ' ') sl--;
            buf_printf(protos, "%.*s;\n", (int)sl, p);
        }
        p += ll + (nl ? 1 : 0);
    }
}

/* 切开：header 收类型/运行时原型/数据，units 每项是一个模块的 .c 正文，
 * rt 收运行时函数体（main.c 把它落成独立的 _zxrt.c 单元，只编一次）。
 * 返回单元数。 */
/* units 由调用方分配，容量必须 >= sema_module_count()。返回实际单元数。
 * 不再静默截断：模块数与容量不符时是调用 bug，宁可报错也不丢模块。 */
int backend_emit_split(Node *root, Buf *header, Buf *rt, BUnit *units, int maxunits) {
    buf_init(&g_btypes);
    /* 自动内联（decisions §138）：小函数在 -O0（dev 档）也展开。
     * 为什么要宏：MSVC 不认 __attribute__，而默认工具链 zig cc 是 clang。
     * always_inline 只在**定义**处需要——原型保持 static，同单元内
     * clang 会采用定义处的属性；别的单元根本调不到 static 函数。 */
    buf_puts(&g_btypes,
        "/* ZX_INLINE: small functions are inlined even at -O0 */\n"
        "#if defined(_MSC_VER) && !defined(__clang__)\n"
        "#define ZX_INLINE static __forceinline\n"
        "#else\n"
        "#define ZX_INLINE static inline __attribute__((always_inline))\n"
        "#endif\n\n");
    buf_init(&g_bpub);
    buf_init(&g_bglobals);
    buf_init(&g_bproto);
    buf_init(&g_bbody);
    g_out = &g_bbody;
    g_ind = 0;
    g_nbmap = 0;
    g_cur_unit = 0;
    for (int k = 0; k < 3; k++) { g_scan_pos[k] = 0; g_scan_lines[k] = 0; }

    int nmods = sema_module_count();
    if (nmods <= 0) nmods = 1;
    /* §203：过去这里 `if (nmods > maxunits) nmods = maxunits;` 静默丢掉
     * 第 65 个及之后的模块——sema 已检查过它们、头文件也声明了它们的原型，
     * 却没发 .c，链接时报 undeclared。容量不够是调用方的 bug，明确报错。 */
    if (nmods > maxunits) {
        fprintf(stderr, "zxc: internal: unit capacity %d < module count %d\n",
                maxunits, nmods);
        return -1;
    }
    #define MOD_ROOT(mi) ((mi) == 0 ? root : sema_module_root(mi))

    g_need_proc = scan_proc_use(root);
    for (int mi = 1; mi < nmods && !g_need_proc; mi++)
        g_need_proc = scan_proc_use(MOD_ROOT(mi));

    g_need_task = scan_task_use(root);
    for (int mi = 1; mi < nmods && !g_need_task; mi++)
        g_need_task = scan_task_use(MOD_ROOT(mi));

    /* 初始化函数表：被依赖的先初始化（模块表里入口排在最前）。
     * §203：g_initname 原本是 [64] 定长，模块超过 64 时这里也跟着截断，
     * 与 units 同为 bug 根因；改成按 nmods 动态扩容。 */
    if (nmods > g_init_cap) {
        g_init_cap = nmods;
        g_initname = realloc(g_initname, sizeof(char *) * (size_t)g_init_cap);
    }
    g_ninit = 0;
    for (int mi = nmods - 1; mi >= 0; mi--) {
        char stem[64];
        unit_stem(mi, stem, sizeof stem);
        char *nm = malloc(96);
        snprintf(nm, 96, "zx_init_%s", stem);
        g_initname[g_ninit++] = nm;
    }

    /* 先登记所有聚合类型：字段里的类型要按依赖顺序出现 */
    for (int mi = 0; mi < nmods; mi++) {
        Node *mr = MOD_ROOT(mi);
        if (!mr) continue;
        for (int i = 0; i < mr->nitems; i++) {
            Node *it = mr->items[i];
            if (it->kind == NK_DECL && it->b && it->b->type_val &&
                it->b->ty && it->b->ty->kind == TY_TYPE) {
                Type *t = it->b->type_val;
                if (t->kind == TY_STRUCT || t->kind == TY_UNION || t->kind == TY_ENUM)
                    ensure_type(t);
            }
        }
    }

    /* 逐模块发射正文 */
    for (int mi = 0; mi < nmods; mi++) {
        Node *mr = MOD_ROOT(mi);
        g_cur_unit = mi;
        g_cur_mod_path = sema_module_path(mi);
        char stem[64];
        unit_stem(mi, stem, sizeof stem);
        units[mi].name = malloc(strlen(stem) + 1);
        strcpy(units[mi].name, stem);
        buf_init(&units[mi].src);

        buf_init(&g_bglobals);
        buf_init(&g_bproto);
        buf_init(&g_bbody);
        g_out = &g_bbody;
        g_ind = 0;
        g_nlate = 0;

        if (mr) {
            /* extern 声明：原型进共享头，没有定义 */
            for (int i = 0; i < mr->nitems; i++) {
                Node *it = mr->items[i];
                if (it->kind == NK_FN && it->is_extern && it->name)
                    gen_extern(it);
            }
            /* 函数：先原型，后函数体 */
            for (int i = 0; i < mr->nitems; i++) {
                Node *it = mr->items[i];
                Node *fn = NULL;
                if (it->kind == NK_FN) fn = it;
                else if (it->kind == NK_DECL && it->b && it->b->kind == NK_FN) fn = it->b;
                if (!fn || !fn->b || fn_is_generic(fn)) continue;
                const char *nm = sym_cname(it);
                bool is_main = (mi == 0) && nm && !strcmp(nm, "main");
                /* 模块导出的东西别的单元要能链到：去掉 static，原型进头文件。
                 * 非 pub 的也得导出：结构体方法体是发射到**使用方**单元的，
                 * 方法里引用本模块的私有自由函数时，static 符号跨单元断链
                 * （自举 lex.zx 首次暴露）。模块符号带 stem 前缀，不会撞名。 */
                bool exported = (mi != 0) && !is_main && nm && strcmp(nm, "main");
                gen_fn(fn, nm, is_main, exported);
            }
            /* 全局数据 */
            for (int i = 0; i < mr->nitems; i++) {
                Node *it = mr->items[i];
                if (it->kind != NK_DECL) continue;
                /* extern var：没有初值（b 为空）也要发 extern 声明 */
                if (it->is_extern) { gen_global(it, false); continue; }
                if (it->c) { gen_global(it, false); continue; }
                if (!it->b) continue;
                if (it->b->kind == NK_FN || it->b->kind == NK_MODULE) continue;
                /* 惰性模块声明没被引用就没解析（b->ty 为空）：它只是
                 * 一个名字别名，不占存储、不进初始化表 */
                if (!it->b->ty) continue;
                if (it->b->ty->kind == TY_TYPE) continue;
                /* 同函数：模块全局也要能被（发射到别处的）方法体引用 */
                bool exported = (mi != 0);
                gen_global(it, exported);
            }
        }
        emit_init_fn(mi);
        flush_anon_fns();        /* §200：本单元 ex() 登记的匿名 fn 补发函数体 */

        /* 组装顺序 (proto, globals, body) 决定三个缓冲在单元源文件里的
         * 起始行；映射表里记的是"缓冲内行号"，这里换算成"单元内行号"
         * （驱动层写 .c 时再补开头两行 include）。 */
        int nproto   = count_lines(g_bproto.data);
        int nglobals = count_lines(g_bglobals.data);
        int base[3];
        base[0] = 1;
        base[1] = base[0] + nproto + 1;
        base[2] = base[1] + nglobals + 1;
        for (int k = 0; k < g_nbmap; k++)
            if (g_bmap[k].unit == mi)
                g_bmap[k].line = base[g_bmap[k].buf] + g_bmap[k].line - 1;

        buf_puts(&units[mi].src, g_bproto.data ? g_bproto.data : "");
        buf_putc(&units[mi].src, '\n');
        buf_puts(&units[mi].src, g_bglobals.data ? g_bglobals.data : "");
        buf_putc(&units[mi].src, '\n');
        buf_puts(&units[mi].src, g_bbody.data ? g_bbody.data : "");
    }

    /* 泛型实例：上面任何一处引用过它们，这里统一下饺子。
     * 名字是全局唯一的（_i<n>），所以全部放进入口单元即可。
     * 链接必须是**外部**的：多个单元会实例化同一个签名（比如模块里
     * 也用了 ArrayList(u8)），而原型进共享头——写成 static 就是
     * "内部链接却无定义"，用到它的单元直接编译失败。 */
    if (g_nitab) {
        buf_init(&g_bproto);
        buf_init(&g_bbody);
        g_out = &g_bbody;
        g_ind = 0;
        for (int i = 0; i < g_nitab; i++)
            gen_fn(g_itab[i].fn, g_itab[i].name, false, true);
        flush_anon_fns();        /* §200：泛型实例体里登记的匿名 fn */
        buf_puts(&units[0].src, "\n");
        buf_puts(&units[0].src, g_bproto.data ? g_bproto.data : "");
        buf_putc(&units[0].src, '\n');
        buf_puts(&units[0].src, g_bbody.data ? g_bbody.data : "");
    }

    /* 运行时辅助：必须等所有函数体（含泛型实例）都发完，
     * 否则"只有实例里用到的"运行时会被漏掉。原型进头文件（去 static，
     * 外部链接），函数体统一落进独立 zxrt 单元——各用户单元不再
     * 重复编译同一份 runtime。 */
    buf_init(&g_bproto);
    buf_init(&g_bglobals);
    buf_init(&g_bbody);
    g_out = &g_bbody;
    g_ind = 0;

    emit_diag_runtime();   /* §211: 无缓冲诊断 + 崩溃打点，所有程序都带 */

    if (g_need_pad || g_need_fmt) {
        /* 宽度/填充助手（M26）：printf 旗标覆盖不了的填充形态走这里。
         * width 上限 256（fmt_parse_opts 把关），512 缓冲足够宽裕。 */
        bline(&g_bproto, "static char *zx_fmt_pad(char *out, size_t cap, char fill, int width, int align, const char *fmt, ...);");
        bline(&g_bproto, "static char *zx_fmt_pads(char *out, size_t cap, const char *s, int n, char fill, int width, int align);");
        buf_puts(g_out,
            "#include <stdarg.h>\n"
            "static char *zx_fmt_pad(char *out, size_t cap, char fill, int width, int align, const char *fmt, ...) {\n"
            "    va_list ap; va_start(ap, fmt);\n"
            "    int n = vsnprintf(out, cap, fmt, ap);\n"
            "    va_end(ap);\n"
            "    if (n < 0 || width <= 0 || n >= width || (size_t)n >= cap) return out;\n"
            "    int pad = width - n;\n"
            "    if (align == '<') { memset(out + n, fill, (size_t)pad); out[width] = 0; }\n"
            "    else if (align == '>') { memmove(out + pad, out, (size_t)n + 1); memset(out, fill, (size_t)pad); }\n"
            "    else { int l = pad / 2; memmove(out + l, out, (size_t)n + 1);\n"
            "           memset(out, fill, (size_t)l); memset(out + l + n, fill, (size_t)(pad - l)); }\n"
            "    return out;\n"
            "}\n"
            "static char *zx_fmt_pads(char *out, size_t cap, const char *s, int n, char fill, int width, int align) {\n"
            "    if (n < 0) n = 0;\n"
            "    if (width <= 0 || n >= width) { size_t m = (size_t)n < cap - 1 ? (size_t)n : cap - 1;\n"
            "        memmove(out, s, m); out[m] = 0; return out; }\n"
            "    int pad = width - n;\n"
            "    size_t lim = cap - 1;\n"
            "    if (align == '<') { size_t m = (size_t)n < lim ? (size_t)n : lim;\n"
            "        memmove(out, s, m); int rest = width - (int)m;\n"
            "        for (int k = 0; k < rest && (size_t)(m + k) < lim; k++) out[m + k] = fill;\n"
            "        out[m + (rest < lim - m ? rest : (int)(lim - m))] = 0; }\n"
            "    else { int lead = align == '^' ? pad / 2 : pad;\n"
            "        size_t w = 0;\n"
            "        for (int k = 0; k < lead && w < lim; k++) out[w++] = fill;\n"
            "        size_t m = (size_t)n < lim - w ? (size_t)n : lim - w;\n"
            "        memmove(out + w, s, m); w += m;\n"
            "        for (int k = lead; k < pad && w < lim; k++) out[w++] = fill;\n"
            "        out[w] = 0; }\n"
            "    return out;\n"
            "}\n\n");
    }
    if (g_need_f64str || g_need_fmt || g_need_allocfmt || g_need_page || g_need_any) {
        /* 浮点按"最短往返"打印：15 位起步，回读相等即停，至多 17 位。
         * %g 默认只给 6 位有效数字，0.30000000000000004 会被截成 0.3，
         * 且 NaN 在各家 CRT 上文案不一（"-nan(ind)" / "nan"）。 */
        bline(&g_bproto, "static const char *zx_f64_str(char *out, size_t cap, double v);");
        buf_puts(g_out,
            "static const char *zx_f64_str(char *out, size_t cap, double v) {\n"
            "    if (!cap) return out;\n"
            "    if (v != v) { snprintf(out, cap, \"nan\"); return out; }\n"
            "    if (isinf(v)) { snprintf(out, cap, \"%s\", v < 0 ? \"-inf\" : \"inf\"); return out; }\n"
            "    /* 能精确表示成 float 的值按 float 的最短形式打：f32 的\n"
            "     * 3.4028234663852886e+38 不该膨胀成一长串。判据只看值——\n"
            "     * 运行时路径的 ZxFmtArg 只剩 double，看不到原类型，\n"
            "     * 两条路必须靠同一个判据才不分叉。 */\n"
            "    if ((double)(float)v == v) {\n"
            "        for (int prec = 6; prec <= 9; prec++) {\n"
            "            snprintf(out, cap, \"%.*g\", prec, v);\n"
            "            if ((double)strtof(out, NULL) == v) return out;\n"
            "        }\n"
            "    }\n"
            "    for (int prec = 15; prec <= 17; prec++) {\n"
            "        snprintf(out, cap, \"%.*g\", prec, v);\n"
            "        if (strtod(out, NULL) == v) return out;\n"
            "    }\n"
            "    return out;\n"
            "}\n\n");
    }
    if (g_need_bin) {
        bline(&g_bproto, "static void zx_print_bin(uint64_t v);");
        buf_puts(g_out,
            "static void zx_print_bin(uint64_t v) {\n"
            "    char b[65];\n"
            "    int i = 64;\n"
            "    b[i] = 0;\n"
            "    if (v == 0) { fputc('0', stdout); return; }\n"
            "    while (v) { b[--i] = (char)('0' + (v & 1)); v >>= 1; }\n"
            "    fputs(&b[i], stdout);\n"
            "}\n\n");
    }
    if (g_need_file || g_need_io) emit_cstr_runtime();
    if (g_need_file) emit_file_runtime();
    if (g_need_io)   emit_io_runtime();
    if (g_need_proc) emit_proc_runtime();
    /* 纤程运行时必须在 debug 之前：printTrace 回放捕获环，
     * 而 ZxCap / ZxCapRec 类型定义在 fiber 运行时里。只要用过
     * std.debug（printTrace）或发生过 else 捕获，就带捕获环。 */
    if (g_need_task || g_need_debug) emit_task_runtime();
    if (g_need_debug) emit_debug_runtime();
    if (g_need_sync) emit_sync_runtime();
    if (g_need_heapalloc) emit_heapalloc_runtime();
    if (g_nhslice) emit_heapslice_runtime();   /* 依赖 zx_heapalloc 的原型 */
    if (g_need_div) emit_div_runtime();
    if (g_need_bounds) emit_bounds_runtime();
    if (g_need_idx) emit_idx_runtime();   /* §216：非简单基址切片下标 */
    if (g_need_f2i) emit_f2i_runtime();
    if (g_nuw) emit_unwrap_runtime();
    /* 128 位转十进制：编译期 printf 路径（g_need_i128）与 zx_fmt 的
     * kind 9/10 分支（fmt 运行时一存在就可能碰到）都要有。 */
    if (g_need_i128 || g_need_fmt || g_need_allocfmt || g_need_page || g_need_any)
        emit_i128_runtime();
    if (g_need_fmt || g_need_allocfmt || g_need_page || g_need_any) emit_fmt_runtime();
    emit_any_runtime();

    /* 头文件 */
    buf_puts(header, "/* generated by zxc — do not edit */\n");
    /* §217: POSIX feature macro 必须在任何系统头之前定义——-std=c11 下
     * sigaction/siginfo_t/setenv 等 POSIX 符号默认不声明。放这里让下面
     * 以及各 .c 里的系统头都能看到。Windows 不需要（mingw 隐式提供）。 */
    buf_puts(header,
        "#ifndef _WIN32\n"
        "#ifndef _POSIX_C_SOURCE\n"
        "#define _POSIX_C_SOURCE 200809L\n"
        "#endif\n"
        "#endif\n\n");
    buf_puts(header, "#ifndef ZX_GEN_H\n#define ZX_GEN_H\n\n");
    buf_puts(header, "#include <stdint.h>\n#include <stdbool.h>\n");
    buf_puts(header, "#include <stddef.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <math.h>\n");
    buf_puts(header, "#include <string.h>\n");   /* @memcpy / @memmove / @memset */
    buf_puts(header, "#include <string.h>\n\n");
    buf_puts(header, g_btypes.data ? g_btypes.data : "");
    buf_putc(header, '\n');
    buf_puts(header, g_bpub.data ? g_bpub.data : "");
    for (int i = 0; i < g_ninit; i++) {
        buf_printf(header, "void %s(void);\n", g_initname[i]);
    }
    buf_putc(header, '\n');
    buf_puts(header, g_bany.data ? g_bany.data : "");
    buf_putc(header, '\n');
    buf_destatic(&g_bproto, false);
    buf_destatic(&g_bglobals, false);
    buf_destatic(&g_bbody, true);
    buf_protos_from(&g_bbody, &g_bproto);
    buf_puts(header, g_bproto.data ? g_bproto.data : "");
    buf_putc(header, '\n');
    buf_puts(header, g_bglobals.data ? g_bglobals.data : "");
    buf_putc(header, '\n');
    /* 运行时函数体不再进共享头：main.c 把它们落成 _zxrt.c，单独编译一次。 */
    if (rt) {
        buf_init(rt);
        buf_puts(rt, g_bbody.data ? g_bbody.data : "");
    }
    buf_puts(header, "\n#endif /* ZX_GEN_H */\n");

    return nmods;
}

/* 单文件模式（--emit-c）：头文件 + 运行时 + 各单元拼成一份，方便肉眼检查 */
void backend_emit(Node *root, Buf *out) {
    Buf header, rt;
    buf_init(&header);
    buf_init(&rt);
    int nmods = sema_module_count();
    if (nmods <= 0) nmods = 1;
    BUnit *units = calloc((size_t)nmods, sizeof(BUnit));
    int n = backend_emit_split(root, &header, &rt, units, nmods);
    if (n < 0) return;
    buf_puts(out, header.data);
    if (rt.data && rt.len) {
        buf_puts(out, "\n/* ======== runtime ======== */\n");
        buf_puts(out, rt.data);
    }
    for (int i = 0; i < n; i++) {
        buf_puts(out, "\n/* ======== unit ");
        buf_puts(out, units[i].name);
        buf_puts(out, " ======== */\n");
        buf_puts(out, units[i].src.data);
    }
    for (int i = 0; i < n; i++) { free(units[i].name); buf_free(&units[i].src); }
    free(units);
}
