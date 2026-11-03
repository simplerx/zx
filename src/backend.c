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
static void mname(Buf *b, const char *s);
static void dotname(Buf *b, const char *s);
static void arrowname(Buf *b, const char *s);

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
    case TY_INT:
        if (t->bits == 64 && !t->is_signed) buf_puts(b, "usize");
        else if (t->bits == 64)             buf_puts(b, "isize");
        else buf_printf(b, "%c%d", t->is_signed ? 'i' : 'u', t->bits);
        break;
    case TY_FLOAT: buf_printf(b, "f%d", t->bits); break;
    case TY_PTR:
        buf_puts(b, "p");
        tsuffix(b, t->elem);
        if (t->has_len) buf_printf(b, "_%llu", (unsigned long long)t->len);
        break;
    case TY_SLICE: buf_puts(b, "s"); tsuffix(b, t->elem); break;
    case TY_OPT:   buf_puts(b, "o"); tsuffix(b, t->elem); break;
    case TY_ERR:   buf_puts(b, "e"); tsuffix(b, t->elem); break;
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
    case TY_TUPLE: buf_puts(b, "ZxTup_");   tsuffix(b, t);       break;
    case TY_FN:    buf_puts(b, "ZxFn_");    tsuffix(b, t);       break;
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
    case TY_FN: case TY_STRUCT: case TY_UNION: case TY_ENUM:
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
    case TY_TUPLE: {
        for (int k = 0; k < t->nfields; k++) ensure_type(t->fields[k].ty);
        buf_printf(g_out, "typedef struct { ");
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
                ctype(g_out, f->ty);
                buf_putc(g_out, ' ');
                mname(g_out, f->name);
                buf_puts(g_out, ";\n");
            }
            buf_puts(g_out, "};\n");

            /* 静态成员：结构体体里声明的 var / const，全程序一份 */
            for (int k = 0; k < t->nfields; k++) {
                Field *f = &t->fields[k];
                if (!f->is_static) continue;
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
        ctype(b, t->elem);
        buf_puts(b, " *");
        break;
    case TY_SLICE: case TY_OPT: case TY_ERR: case TY_TUPLE:
    case TY_FN: case TY_STRUCT: case TY_UNION: case TY_ENUM: {
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
static bool g_need_fmt;      /* fmt 不是编译期常量 → 带上运行时格式化器 */
static bool g_need_allocfmt; /* 用过 std.format → 还要带上 zx_format */
static bool g_need_file;    /* 用过 std.file → 带上 zx_file_* */
static bool g_need_io;      /* 用过 std.io → 带上 zx_io_*（句柄流） */
static bool g_need_proc;    /* 用过 std.process → 带上 zx_proc_* */
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
    if (!strcmp(cn, "zx_alloc_page")) g_need_page = true;
    if (!strncmp(cn, "zx_file_", 8))  g_need_file = true;
    if (!strncmp(cn, "zx_io_", 6))    g_need_io = true;
    if (!strncmp(cn, "zx_proc_", 8))  g_need_proc = true;
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
} CScope;

static CScope *g_csc;
static int     g_depth;

static void cscope_push(CScope *s) { s->parent = g_csc; s->ndefer = 0; g_csc = s; g_depth++; }
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
static void gen_value(Node *n, const char *slot, Type *slot_ty);
static void gen_if_value(Node *n, const char *slot, Type *slot_ty);
static void gen_loop_value(Node *n, const char *slot, Type *slot_ty);
static void gen_for_body(Node *n, int id);
static void gen_block_stmts(Node *b);
static void run_defers(int down_to);
static char *inst_name(Node *g, const char *base);   /* 泛型实例的 C 名 */

/* 当前函数：生成 return 时要看返回类型 / 是不是 main */
static Type *g_cur_ret;
static bool  g_is_main;
static bool  g_saw_errdefer;   /* 函数体里登记过 errdefer：return 要做 tag 检查 */

/* main 的 C 签名固定是 `int main(...)`，但 Zx 里它可以是 void、u8，
 * 也可以是 !void / !T / ?T。真值要在这里折成进程退出码：
 *   !T  → 标签就是错误码，0 = 成功（正好是 shell 的约定）
 *   ?T  → 有值退出 0，空退出 1
 * 其余类型原样转 int。expr 为 NULL 表示"没给值"（`return;` / 走到底）。 */
static void main_exit(const char *expr) {
    Type *t = g_cur_ret;
    if (!expr) { line("return 0;"); return; }
    if (t && t->kind == TY_ERR)       line("return (int)((%s).tag);", expr);
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
 *        defer 和 errdefer 相邻登记时，清理顺序仍然正确。 */
static void run_defers_m(int down_to, int mode, const char *errcond) {
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
                gen_stmt(dn->a);
                g_ind--;
                line("}");
            } else {
                gen_stmt(dn->a);
            }
        }
    }
}

static void run_defers(int down_to) { run_defers_m(down_to, 0, NULL); }

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

/* ---- 可写位置（左值） ---- */
static void lvalue(Buf *b, Node *n) {
    switch (n->kind) {
    case NK_IDENT:
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
            if (bt && bt->kind == TY_PTR && bt->has_len) {
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
            if (bt->has_len) {             /* 数组指针：元素是结构体时 .name 穿一层 */
                ex(b, n->a);
                buf_printf(b, "->%s", n->name);
                return;
            }
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
            ex(b, n->a);
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
    case NK_INDEX: {
        Type *bt = n->a->ty;
        ex(b, n->a);
        if (bt && bt->kind == TY_SLICE) buf_puts(b, ".ptr");
        buf_putc(b, '[');
        ex(b, n->b);
        buf_putc(b, ']');
        return;
    }
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
        if (to->kind == TY_OPT) { ensure_type(to); buf_puts(b, "(("); Buf n; buf_init(&n); ctype(&n, to);
            buf_printf(b, "%s){0})", n.data); return; }
        buf_puts(b, "false");
        return;
    }
    if (e->kind == NK_UNDEF) {
        ensure_type(to);
        Buf n; buf_init(&n); ctype(&n, to);
        buf_printf(b, "((%s){0})", n.data);
        return;
    }

    if (ty_eq(from, to)) { ex(b, e); return; }

    /* 数组 → 切片（R7） */
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
    /* 数组 → 裸指针（R7） */
    if (from->kind == TY_PTR && from->has_len && to->kind == TY_PTR && !to->has_len) {
        buf_puts(b, "(");
        ex(b, e);
        buf_puts(b, ")");
        return;
    }
    /* 裸指针 → 数组：显式断言 */
    if (from->kind == TY_PTR && !from->has_len && to->kind == TY_PTR && to->has_len) {
        buf_puts(b, "(");
        ctype(b, to);
        buf_puts(b, ")(");
        ex(b, e);
        buf_puts(b, ")");
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

    if (!strcmp(nm, "sizeOf") || !strcmp(nm, "alignOf") || !strcmp(nm, "offsetOf")) {
        /* sema 已经把结果算好放在 ival 里（它是编译期值，不是运行时 sizeof） */
        buf_printf(b, "((size_t)%llu)", (unsigned long long)n->ival);
        return;
    }
    if (!strcmp(nm, "typeName")) {
        /* sema 已经把名字算进 n->str 了（@typeName(@typeOf(x)) 这种套娃
         * 只能靠它——取 items[0]->ty 会拿到外层那个 `type`） */
        const char *p = n->str;
        int len = n->str_len;
        if (!p && n->nitems && n->items[0]->ty) { p = ty_str(n->items[0]->ty); }
        if (!p) { buf_puts(b, "((ZxSlice_u8){0})"); return; }
        if (len <= 0) len = (int)strlen(p);
        Buf s; buf_init(&s);
        cstr(&s, p, len);
        buf_printf(b, "((ZxSlice_u8){(uint8_t*)%s, %d})", s.data, len);
        return;
    }
    if (!strcmp(nm, "as") || !strcmp(nm, "intCast") || !strcmp(nm, "ptrCast") ||
        !strcmp(nm, "bitCast")) {
        Type *to = n->ty;
        if (n->nitems >= 2) coerce(b, n->items[1], to);
        else buf_puts(b, "0");
        return;
    }
    if (!strcmp(nm, "hasField")) {
        buf_printf(b, "%d", n->ival ? 1 : 0);
        return;
    }
    if (!strcmp(nm, "fieldType")) {
        buf_puts(b, "0");           /* 类型只活在编译期 */
        return;
    }
    if (!strcmp(nm, "fieldName")) {
        /* 和 typeName 一样：sema 已把名字算进 n->str，直接发字面量切片 */
        const char *p = n->str;
        int len = n->str_len;
        if (!p) { buf_puts(b, "((ZxSlice_u8){0})"); return; }
        if (len <= 0) len = (int)strlen(p);
        Buf s; buf_init(&s);
        cstr(&s, p, len);
        buf_printf(b, "((ZxSlice_u8){(uint8_t*)%s, %d})", s.data, len);
        return;
    }
    if (!strcmp(nm, "typeInfo")) {
        /* 正常情况下它会在编译期折叠掉；真漏到运行时就是一块全零的描述 */
        if (n->ty && n->ty->kind != TY_INVALID) {
            ensure_type(n->ty);
            Buf t; buf_init(&t); ctype(&t, n->ty);
            buf_printf(b, "((%s){0})", t.data);
        } else buf_puts(b, "0");
        return;
    }
    if (!strcmp(nm, "trap") || !strcmp(nm, "panic")) {
        buf_puts(b, "abort()");
        return;
    }
    if (!strcmp(nm, "memcpy") || !strcmp(nm, "memmove") || !strcmp(nm, "memset")) {
        /* 直接落 C 的批量内存原语：ArrayList 增长 / 搬移不再逐元素。
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
    if (!strcmp(nm, "compileError")) {
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

static void fmt_spec(Buf *spec, Buf *args, Node *a, int verb, bool *need_bin) {
    Type *t = a->ty ? a->ty : ty_i32();

    if (verb == 's' || t->kind == TY_SLICE) {
        buf_puts(spec, "%.*s");
        buf_puts(args, ", (int)(");
        ex(args, a);
        buf_puts(args, ").len, (char*)(");
        ex(args, a);
        buf_puts(args, ").ptr");
        return;
    }
    if (verb == 'c') {
        buf_puts(spec, "%c");
        buf_puts(args, ", (int)(");
        ex(args, a);
        buf_puts(args, ")");
        return;
    }
    if (t->kind == TY_BOOL && verb != 'd' && verb != 'x' && verb != 'b') {
        buf_puts(spec, "%s");
        buf_puts(args, ", (");
        ex(args, a);
        buf_puts(args, ") ? \"true\" : \"false\"");
        return;
    }
    if (ty_is_float(t)) {
        buf_puts(spec, "%g");
        buf_puts(args, ", (double)(");
        ex(args, a);
        buf_puts(args, ")");
        return;
    }
    if (verb == 'b') {
        /* printf 没有二进制：记号交给生成的辅助函数 */
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
            /* printf 没有 128 位动词：转成十进制字符串再 %s（M19） */
            g_need_i128 = true;
            buf_puts(spec, "%s");
            buf_puts(args, sgn ? ", zx_i128_str((char[48]){0}, (__int128)("
                               : ", zx_u128_str((char[48]){0}, (unsigned __int128)(");
            ex(args, a);
            buf_puts(args, "))");
            return;
        }
        if (verb == 'x') {
            buf_puts(spec, "%llx");
            buf_puts(args, ", (unsigned long long)(");
            ex(args, a);
            buf_puts(args, ")");
            return;
        }
        buf_puts(spec, sgn ? "%lld" : "%llu");
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
       AK_PTR, AK_SLICE, AK_STRUCT, AK_UNION, AK_TUNION, AK_OPT, AK_ERR, AK_FN };

static const char *ak_name(int k) {
    static const char *n[] = { "ZX_AK_VOID", "ZX_AK_INT", "ZX_AK_FLOAT", "ZX_AK_BOOL",
                               "ZX_AK_ENUM", "ZX_AK_ERROR", "ZX_AK_PTR", "ZX_AK_SLICE",
                               "ZX_AK_STRUCT", "ZX_AK_UNION", "ZX_AK_TUNION", "ZX_AK_OPT",
                               "ZX_AK_ERR", "ZX_AK_FN" };
    return (k >= 0 && k <= AK_FN) ? n[k] : "ZX_AK_VOID";
}

static Type **g_adt;         /* 描述符 ↔ 类型 */
static int    g_nad, g_cad;

/* 这个类型有描述符可用（能结构展开） */
static bool any_has_desc(Type *t) {
    if (!t) return false;
    switch (t->kind) {
    case TY_STRUCT: case TY_UNION: case TY_TUPLE: case TY_OPT: case TY_ERR:
    case TY_ENUM: case TY_PTR: case TY_SLICE:
        return true;
    default: return false;
    }
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
    case TY_STRUCT: case TY_UNION: case TY_TUPLE: case TY_OPT: case TY_ERR: case TY_PTR:
        return true;
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
    buf_printf(g_out,
        "static void *zx_heapalloc(%s al, %s n) {\n"
        "    %s r = al.alloc(n);\n"
        "    if (!r.tag) { fputs(\"zx: out of memory\\n\", stderr); abort(); }\n"
        "    return r.payload;\n"
        "}\n\n",
        an.data, un.data, on.data);
    g_out = sv;
    g_ind = si;
}

static void emit_file_runtime(void) {
    Type *s8  = ty_slice(ty_u8(), true);
    Type *os8 = ty_opt(s8);
    Type *op8 = ty_opt(ty_ptr(ty_u8(), false, false, 0));
    Type *al  = sema_allocator_type();
    ensure_type(s8); ensure_type(os8); ensure_type(op8); ensure_type(al);

    Buf sn, osn, opn, an, un;
    buf_init(&sn);  tn(&sn, s8);
    buf_init(&osn); tn(&osn, os8);
    buf_init(&opn); tn(&opn, op8);
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
        "    %s mem = al.alloc((%s)n + 1);\n"
        "    if (!mem.tag) { fclose(f); return r; }\n"
        "    %s got = fread(mem.payload, 1, (%s)n, f);\n"
        "    fclose(f);\n"
        "    mem.payload[got] = 0;      /* 当 C 串用也安全 */\n"
        "    r.payload.ptr = mem.payload;\n"
        "    r.payload.len = got;\n"
        "    r.tag = true;\n"
        "    return r;\n"
        "}\n\n",
        osn.data, an.data, sn.data, osn.data, opn.data, un.data, un.data, un.data);
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
        "static %s zx_io_read(%s h, %s buf) {\n"
        "    if (!h) return 0;\n"
        "    return fread(buf.ptr, 1, buf.len, (FILE*)(size_t)h);\n"
        "}\n"
        "static %s zx_io_write(%s h, %s data) {\n"
        "    if (!h || !data.len) return 0;\n"
        "    return fwrite(data.ptr, 1, data.len, (FILE*)(size_t)h);\n"
        "}\n"
        "static bool zx_io_seek(%s h, %s off) {\n"
        "    if (!h) return false;\n"
        "    return fseek((FILE*)(size_t)h, (long)off, SEEK_SET) == 0;\n"
        "}\n"
        "static %s zx_io_tell(%s h) {\n"
        "    if (!h) return 0;\n"
        "    long p = ftell((FILE*)(size_t)h);\n"
        "    return p < 0 ? 0 : (%s)p;\n"
        "}\n"
        "static void zx_io_close(%s h) { if (h) fclose((FILE*)(size_t)h); }\n"
        "static void zx_io_flush(%s h) { if (h) fflush((FILE*)(size_t)h); }\n"
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
        un.data, un.data, un.data, /* tell  */
        un.data,                   /* close */
        un.data,                   /* flush */
        un.data, un.data,          /* stdin */
        un.data, un.data,          /* stdout */
        un.data, un.data);         /* stderr */
    g_out = sv;
    g_ind = si;
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
        buf_printf(&g_bglobals, "static %s zx_page_alloc(%s n);\n", on.data, un.data);
        buf_printf(&g_bglobals, "static void zx_page_free(%s p, %s n);\n", u8p.data, un.data);
        buf_printf(&g_bglobals, "static %s zx_alloc_page = { zx_page_alloc, zx_page_free };\n\n",
                   an.data);

        Buf *sv = g_out;
        int si = g_ind;
        g_out = &g_bbody;
        g_ind = 0;
        buf_printf(g_out,
            "static %s zx_page_alloc(%s n) {\n"
            "    %s r;\n"
            "    r.payload = (%s)malloc(n ? n : 1);\n"
            "    r.tag = (r.payload != 0);   /* ?T：tag 为真表示有值 */\n"
            "    return r;\n"
            "}\n\n"
            "static void zx_page_free(%s p, %s n) { (void)n; free(p); }\n\n",
            on.data, un.data, on.data, u8p.data, u8p.data, un.data);
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
        "    fflush(stdout);\n"
        "    fprintf(stderr, \"zx: format error: %s\\n\", msg);\n"
        "    abort();\n"
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
        "            else if (a->kind == 3) { snprintf(tmp, sizeof tmp, \"%g\", a->d); zx_sink_put(out, tmp, strlen(tmp)); }\n"
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
            "    mem = al.alloc(need);\n"
            "    if (!mem.tag) { r.payload.ptr = 0; r.payload.len = 0; r.tag = 1; return r; }\n"
            "    w.fp = 0; w.buf = mem.payload; w.cap = need; w.len = 0;\n"
            "    zx_fmt(&w, fmt, args, nargs);\n"
            "    mem.payload[w.len] = 0;\n"
            "    r.payload.ptr = mem.payload;\n"
            "    r.payload.len = w.len;\n"
            "    r.tag = 0;\n"
            "    return r;\n"
            "}\n\n",
            en.data, an.data, en.data, on.data);
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
        "       ZX_AK_TUNION, ZX_AK_OPT, ZX_AK_ERR, ZX_AK_FN };\n"
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
        "        if (t->size == 4) snprintf(b, sizeof b, \"%g\", (double)*(const float *)p);\n"
        "        else snprintf(b, sizeof b, \"%g\", *(const double *)p);\n"
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
    Buf t; buf_init(&t); ctype(&t, et);
    buf_puts(b, "((");
    buf_puts(b, t.data);
    buf_puts(b, " *)(__builtin_alloca((size_t)(");
    coerce(b, n->items[1], ty_usize());
    buf_printf(b, ") * sizeof(%s))))", t.data);
}

static bool g_need_heapalloc;   /* 用过 std.heapAlloc → 带上 zx_heapalloc */

static void ex_heapalloc(Buf *b, Node *n) {
    g_need_heapalloc = true;
    Type *et = (n->ty && n->ty->kind == TY_PTR) ? n->ty->elem : ty_u8();
    Buf t; buf_init(&t); ctype(&t, et);
    buf_puts(b, "((");
    buf_puts(b, t.data);
    buf_puts(b, " *)(zx_heapalloc(");
    ex(b, n->items[0]);          /* 分配器 */
    buf_puts(b, ", (uint64_t)(");
    coerce(b, n->items[2], ty_usize());
    buf_printf(b, ") * sizeof(%s))))", t.data);
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
        ex(b, fmt);
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

        /* 取参数 */
        int which = -1;
        int verb = 0;
        if (clen == 0) {
            which = argi++;
        } else if (clen <= 2 && (clen == 1 && (c[0] == 'd' || c[0] == 'x' || c[0] == 'b' ||
                                              c[0] == 's' || c[0] == 'c'))) {
            which = argi++;
            verb = c[0];
        } else if (clen == 3 && !strncmp(c, "any", 3)) {
            which = argi++;
            verb = 'a';
        } else {
            /* 数字索引 */
            int k = 0; which = 0;
            while (k < clen && c[k] >= '0' && c[k] <= '9') { which = which * 10 + (c[k] - '0'); k++; }
            if (k != clen) {
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
            fmt_spec(&g.spec, &one, n->items[which], 'b', &need_bin);
            if (g.nseg) buf_puts(&g.out, ", ");
            buf_printf(&g.out, "zx_print_bin((uint64_t)(");
            ex(&g.out, n->items[which]);
            buf_puts(&g.out, "))");
            g.nseg++;
        } else if (anyv) {
            /* 结构展开也是一段独立的表达式（printf 塞不进复合值） */
            seg_lit(&g.spec, p + lit, i - lit);
            seg_flush(&g);
            int d = any_desc(at);
            if (g.nseg) buf_puts(&g.out, ", ");
            buf_printf(&g.out, "zx_any_stdout(&zx_at[%d], ", d);
            any_addr(&g.out, n->items[which]);
            buf_printf(&g.out, ", %d)", verb == 'a' ? 1 : 0);
            g.nseg++;
        } else {
            seg_lit(&g.spec, p + lit, i - lit);
            fmt_spec(&g.spec, &g.args, n->items[which], verb, &need_bin);
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
    ex(b, n->items[1]);
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
    ex(b, n->items[1]);
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

/* 整数位宽 → C 无符号类型名（回绕算术用，M19） */
static void utype_of_bits(Buf *b, int bits) {
    switch (bits) {
    case 8:   buf_puts(b, "uint8_t"); break;
    case 16:  buf_puts(b, "uint16_t"); break;
    case 32:  buf_puts(b, "uint32_t"); break;
    case 128: buf_puts(b, "unsigned __int128"); break;
    default:  buf_puts(b, "uint64_t"); break;
    }
}

/* M19 数值语义的统一发射：整数二元运算在 C 里的形态。
 *
 *  - 有符号 + - *：按无符号算再转回（二补码回绕；C 的有符号溢出是 UB）；
 *  - <<：移位数量按位宽掩码（>= bits 的移位是 UB），有符号左移同样走无符号；
 *  - >>：保留算术右移（符号扩展，规格钉住）；
 *  - / %：除零是运行时 trap（zx_div_* / zx_mod_* 辅助）。
 *
 * 返回 false = 不归这里管（浮点、比较、位运算照旧直译）。 */
static bool num_bin_c(Buf *b, Type *ty, int op, const char *lhs, const char *rhs) {
    if (!ty || ty->kind != TY_INT) return false;
    int bits = ty->bits;
    bool sgn = ty->is_signed;
    int mask = bits - 1;

    if (op == TOK_SLASH || op == TOK_PERCENT) {
        int widx = bits <= 8 ? 0 : bits <= 16 ? 1 : bits <= 32 ? 2 : bits <= 64 ? 3 : 4;
        g_need_div = true;
        (void)widx;
        buf_printf(b, "zx_%s_%s%d(%s, %s)",
                   op == TOK_SLASH ? "div" : "mod", sgn ? "i" : "u", bits, lhs, rhs);
        return true;
    }
    if (op == TOK_PLUS || op == TOK_MINUS || op == TOK_STAR) {
        if (!sgn) return false;      /* 无符号本来就回绕，C 保证 */
        Buf ut; buf_init(&ut); utype_of_bits(&ut, bits);
        Buf ct; buf_init(&ct); ctype(&ct, ty);
        buf_printf(b, "((%s)((%s)(%s) %s (%s)(%s)))",
                   ct.data, ut.data, lhs, cop(op), ut.data, rhs);
        return true;
    }
    if (op == TOK_SHL) {
        if (sgn) {
            Buf ut; buf_init(&ut); utype_of_bits(&ut, bits);
            Buf ct; buf_init(&ct); ctype(&ct, ty);
            buf_printf(b, "((%s)((%s)(%s) << ((%s) & %d)))",
                       ct.data, ut.data, lhs, rhs, mask);
        } else {
            buf_printf(b, "((%s) << ((%s) & %d))", lhs, rhs, mask);
        }
        return true;
    }
    if (op == TOK_SHR) {
        buf_printf(b, "((%s) >> ((%s) & %d))", lhs, rhs, mask);
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
        if (t->kind == TY_OPT) {
            buf_printf(g_out,
                "static %s zx_unwrap_%s(%s o, const char *w) {\n"
                "    if (!o.tag) { fflush(stdout); fprintf(stderr, \"zx: unwrap of a null optional at %%s\\n\", w); abort(); }\n"
                "    return o.payload;\n"
                "}\n\n", ct.data, nm.data, nm.data);
        } else {
            buf_printf(g_out,
                "static %s zx_unwrap_%s(%s o, const char *w) {\n"
                "    if (o.tag) { fflush(stdout); fprintf(stderr, \"zx: unwrap of error %%u at %%s\\n\", (unsigned)o.tag, w); abort(); }\n"
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


static void emit_div_runtime(void) {
    bline(&g_bproto, "static void zx_div0(void);");
    buf_puts(g_out,
        "static void zx_div0(void) {\n"
        "    fflush(stdout);\n"
        "    fprintf(stderr, \"zx: division by zero\\n\");\n"
        "    abort();\n"
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
 * `@typeInfo(T).align` 这类表达式在语义层已经算出值了，但它在 C 里长得像
 * "读一块全零描述符的字段"——照结构访问发出去会得到 0。谁折算了就用谁的结果。 */
static bool ex_ct_scalar(Buf *b, Node *n) {
    Value *v = n->ct;
    if (!v || !n->ty) return false;
    switch (v->kind) {
    case V_BOOL:  buf_puts(b, v->bval ? "true" : "false"); return true;
    case V_FLOAT: buf_printf(b, "%.17g", v->fval); return true;
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

    /* 已折叠成标量的值：直接发字面量（`info.kind == .struct_` 这种
     * 泛型里的编译期判断要靠它，否则会发成读一块全零描述符）。
     * 内建除外——它们有的要发自己的形状（@typeName / @fieldName）。 */
    if (n->kind == NK_FIELD && n->ct && ex_ct_scalar(b, n)) return;

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
        buf_puts(b, "false");
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
        if (cv && cv->kind == V_FLOAT) { buf_printf(b, "%.17g", cv->fval); return; }
        if (cv && cv->kind == V_BOOL)  { buf_puts(b, cv->bval ? "true" : "false"); return; }
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
        /* 编译期数量 / 大小 */
        if (bt && n->name) {
            /* 聚合体自己有同名字段时，**字段优先**：`.size` 对数组/元组是
             * 编译期字节数，但对 `struct { size: i32 }` 就是那个字段。
             * sema 一直是这么判的（结构体走普通字段查找），后端以前无条件
             * 发 `layout_size`，于是 `h.size` 静默变成 `sizeof(h)`——
             * 编译过、答案是 8。两边必须用同一条规则判。 */
            bool shadowed = (bt->kind == TY_STRUCT || bt->kind == TY_UNION) &&
                            ty_field(bt, n->name) != NULL;
            bool is_len = !shadowed && !strcmp(n->name, "len");
            bool is_size = !shadowed && !strcmp(n->name, "size");
            if (is_size) {
                if (bt->kind == TY_PTR && bt->has_len) {
                    int es = ty_size(bt->elem);
                    if (es >= 0) {
                        buf_printf(b, "((size_t)%llu)",
                                   (unsigned long long)es * bt->len);
                        return;
                    }
                }
                int sz = layout_size(bt);
                if (sz >= 0) { buf_printf(b, "((size_t)%d)", sz); return; }
            }
            if (is_len) {
                if (bt->kind == TY_PTR && bt->has_len) {
                    buf_printf(b, "((size_t)%llu)", (unsigned long long)bt->len);
                    return;
                }
                if (bt->kind == TY_TUPLE) {
                    buf_printf(b, "((size_t)%d)", bt->nfields);
                    return;
                }
            }
        }
        if (!n->name) {                    /* .N */
            if (bt && bt->kind == TY_PTR && bt->has_len) {
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
            if (bt && bt->kind == TY_SLICE) {
                buf_puts(b, "(*");
                ex(b, n->a);
                buf_puts(b, ".ptr)");
                return;
            }
        }
        if (bt && bt->kind == TY_SLICE) {
            ex(b, n->a);
            buf_puts(b, !strcmp(n->name, "ptr") ? ".ptr" : ".len");
            return;
        }
        if (bt && (bt->kind == TY_OPT || bt->kind == TY_ERR)) {
            ex(b, n->a);
            buf_puts(b, !strcmp(n->name, "tag") ? ".tag" : ".payload");
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

    case NK_INDEX: {
        Type *bt = n->a->ty;
        ex(b, n->a);
        if (bt && bt->kind == TY_SLICE) buf_puts(b, ".ptr");
        buf_putc(b, '[');
        ex(b, n->b);
        buf_putc(b, ']');
        return;
    }

    case NK_SLICE: {
        Type *bt = n->a->ty;
        Type *rt = n->ty;
        ensure_type(rt);
        Buf nm; buf_init(&nm); tn(&nm, rt);
        if (bt->kind == TY_SLICE) {
            buf_printf(b, "((%s){ (", nm.data);
            ex(b, n->a);
            buf_printf(b, ").ptr + ");
            if (n->b) ex(b, n->b); else buf_puts(b, "0");
            buf_puts(b, ", ");
            if (n->c) {
                ex(b, n->c);
                buf_puts(b, " - (");
                if (n->b) ex(b, n->b); else buf_puts(b, "0");
                buf_puts(b, ")");
            } else {
                buf_puts(b, "(");
                ex(b, n->a);
                buf_puts(b, ").len - (");
                if (n->b) ex(b, n->b); else buf_puts(b, "0");
                buf_puts(b, ")");
            }
            buf_puts(b, " })");
            return;
        }
        buf_printf(b, "((%s){ (", nm.data);
        ex(b, n->a);
        buf_printf(b, ") + ");
        if (n->b) ex(b, n->b); else buf_puts(b, "0");
        buf_puts(b, ", ");
        if (n->c) {
            ex(b, n->c);
            buf_puts(b, " - (");
            if (n->b) ex(b, n->b); else buf_puts(b, "0");
            buf_puts(b, ")");
        } else {
            int64_t L = bt->has_len ? (int64_t)bt->len : 0;
            buf_printf(b, "%lld - (", (long long)L);
            if (n->b) ex(b, n->b); else buf_puts(b, "0");
            buf_puts(b, ")");
        }
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
             * 保留错误码 error.null ≡ 65535（§5），沿同一条传播路上行。 */
            line("%s %s = %s;", cty(t), tmp, cxs(n->a, t));
            line("if (!%s.tag) {", tmp);
            g_ind++;
            if (g_is_main) {
                run_defers_m(0, 1, NULL);
                /* main 交出去的是 int：error.null 的退出码就是 65535（OS 截 8 位） */
                line("return (int)(65535);");
            } else {
                line("%s %s = {0};", cty(ret), rv);
                line("%s.tag = 65535;", rv);
                run_defers_m(0, 1, NULL);
                line("return %s;", rv);
            }
            g_ind--;
            line("}");
            if (t->elem && t->elem->kind == TY_VOID) buf_puts(b, "(void)0");
            else buf_printf(b, "%s.payload", tmp);
            return;
        }

        line("%s %s = %s;", cty(t), tmp, cxs(n->a, t));
        line("if (%s.tag != 0) {", tmp);
        g_ind++;
        if (g_is_main) {
            /* main 交出去的是 int：标签本身就是错误码，原样当退出码用 */
            run_defers_m(0, 1, NULL);
            main_exit(tmp);
        } else {
            line("%s %s = {0};", cty(ret), rv);
            line("%s.tag = %s.tag;", rv, tmp);
            run_defers_m(0, 1, NULL);
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

    /* `a orelse b`：右边只在左边空/错时才求值，所以不能摊成一个 C 三元式
     * （那会把 b 提前算掉）。左边先落进临时量，保证只求值一次。 */
    case NK_ORELSE: {
        Type *t = n->a->ty;
        /* `null orelse x` 是类型错误（见 sema），这里只处理真正的 ?T / !T。
         * 留一条兜底：类型缺失时按左值发，别静默变成别的东西。 */
        if (!t || (t->kind != TY_OPT && t->kind != TY_ERR)) { ex(b, n->a); return; }
        const char *has = (t->kind == TY_OPT) ? ".tag" : ".tag == 0";
        if (!g_in_fn) {                    /* 顶层初始化器：只能老老实实写三元式 */
            buf_puts(b, "((");
            ex(b, n->a);
            buf_printf(b, ")%s ? (", has);
            ex(b, n->a);
            buf_puts(b, ").payload : (");
            ex(b, n->b);
            buf_puts(b, "))");
            return;
        }
        static int on;
        char tmp[64];
        snprintf(tmp, sizeof tmp, "_or%d", on++);
        line("%s %s = %s;", cty(t), tmp, cxs(n->a, t));
        buf_printf(b, "((%s)%s ? %s.payload : (", tmp, has, tmp);
        ex(b, n->b);
        buf_puts(b, "))");
        return;
    }

    case NK_BIN: {
        const char *op = cop(n->op);
        if (!op) { ex(b, n->a); return; }
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
        if (n->ty && n->ty->kind == TY_INT &&
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
            buf_puts(b, "0");
            return;
        }
        /* std.io.print / println：往句柄流上格式化。按符号的 C 名字认——
         * std.io.print 和 `const io = std.io` 别名拿到的都是同一个符号。 */
        {
            const char *ioc = sym_cname(n->a);
            if (ioc && !strcmp(ioc, "zx_io_print"))   { ex_io_print(b, n, false); return; }
            if (ioc && !strcmp(ioc, "zx_io_println")) { ex_io_print(b, n, true);  return; }
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
            if (want) coerce(b, n->items[i], want);
            else ex(b, n->items[i]);
        }
        buf_putc(b, ')');
        return;
    }

    case NK_TUPLE: case NK_ARRAY_LIT: {
        Type *t = n->ty;
        if (t && t->kind == TY_PTR && t->has_len) t = NULL;   /* 数组字面量按元素写 */
        Buf nm; buf_init(&nm);
        if (n->kind == NK_ARRAY_LIT) {
            Type *at = n->ty;
            buf_puts(b, "((");
            ctype(b, at->elem);
            buf_printf(b, "[%d]){ ", n->nitems);
            for (int i = 0; i < n->nitems; i++) {
                if (i) buf_puts(b, ", ");
                coerce(b, n->items[i], at->elem);
            }
            buf_puts(b, " })");
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

    case NK_BUILTIN:
        ex_builtin(b, n);
        return;

    case NK_MODULE:
        return;

    case NK_PAIR:
        ex(b, n->a);
        return;

    /* 有值的控制流：走到这里说明被当成普通表达式用了，先取个临时量 */
    default:
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
static void gen_decl_inner(Node *d) {
    Type *t = d->ty;
    if (!t || t->kind == TY_INVALID) return;
    /* 类型值（const P = Pair(i32);）只在编译期存在，C 里没有对应的东西 */
    if (t->kind == TY_TYPE) return;
    /* std.TypeInfo / std.FieldInfo 只在编译期存在：这些"声明"是编译期
     * 绑定，所有使用点在语义层都折成字面量了，C 里不该有它们。
     * 但字段类型（TypeKind 枚举之类）可能被没折干净的 switch 引用，照发。 */
    if (sema_ty_ct_only(t)) {
        if (t->kind == TY_STRUCT || t->kind == TY_UNION)
            for (int i = 0; i < t->nfields; i++)
                if (!sema_ty_ct_only(t->fields[i].ty)) ensure_type(t->fields[i].ty);
        return;
    }
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
    char *cn = cnames(nm);

    bool is_var = (d->op == 2);

    if (t->kind == TY_PTR && t->has_len) {
        Node *init = d->b;
        bool lit = init && (init->kind == NK_ARRAY_LIT ||
                            (init->kind == NK_UN && init->op == TOK_AMP));
        if (lit) {
            Buf decl; buf_init(&decl);
            if (!is_var) buf_puts(&decl, "const ");
            ctype(&decl, t->elem);
            buf_printf(&decl, " %s[%llu]", cn, (unsigned long long)t->len);
            buf_puts(&decl, " = { ");
            for (int i = 0; i < init->nitems; i++) {
                if (i) buf_puts(&decl, ", ");
                coerce(&decl, init->items[i], t->elem);
            }
            buf_puts(&decl, " };");
            line("%s", decl.data);
            return;
        }
        /* R9（v0.10 起）：数组指针 + undefined 不再隐式分配存储。
         * `undefined` 只是"这个指针槽不初始化"；要内存请走
         * std.stackAlloc / std.heapAlloc / &.{…}。指针本身清零，
         * 误用会撞空指针而不是悄悄改栈。 */
        if (!init || init->kind == NK_UNDEF) {
            line("%s%s *%s = 0;", is_var ? "" : "const ", cty(t->elem), cn);
            return;
        }
        /* 普通指针初始化 */
        if (is_ctl(init)) {
            line("%s *%s = 0;", cty(t->elem), cn);
            gen_value(init, cn, t);
            return;
        }
        line("%s *%s = %s;", cty(t->elem), cn, cxs(init, t));
        return;
    }

    if (!d->b) {
        line("%s %s = {0};", cty(t), cn);
        return;
    }
    if (d->b->kind == NK_UNDEF) {
        line("%s %s = {0};", cty(t), cn);
        return;
    }
    /* 初值是控制流表达式（带标签的块 / if / 循环）：先开槽，再往里写 */
    if (is_ctl(d->b)) {
        line("%s %s = {0};", cty(t), cn);
        gen_value(d->b, cn, t);
        return;
    }
    line("%s %s = %s;", cty(t), cn, cxs(d->b, t));
}

static void gen_if_stmt(Node *n);
static void gen_while_for(Node *n);
static void gen_switch(Node *n, const char *slot, Type *slot_ty);

static void gen_stmt(Node *n) {
    if (!n) return;

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
                line("%s %s = %s.f%d;", cty(ft), cnames(id->name), tmp, i);
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
                run_defers_m(0, 1, cond);
            } else run_defers(0);
            if (g_is_main) main_exit(tmp); else line("return %s;", tmp);
            return;
        }

        char *v = cxs(n->a, want);
        if (errcheck) {
            static int rvn;
            char tmp[32], cond[80];
            snprintf(tmp, sizeof tmp, "_rv%d", rvn++);
            line("{ %s %s = %s;", cty(want), tmp, v);
            snprintf(cond, sizeof cond, "(%s).tag != 0", tmp);
            run_defers_m(0, 1, cond);
            if (g_is_main) main_exit(tmp); else line("return %s;", tmp);
            line("}");
            return;
        }
        run_defers(0);
        if (g_is_main) main_exit(v); else line("return %s;", v);
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
        CLab *l = lab_find(n->label);
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
                    /* M19：复合赋值走同一套数值语义（回绕 / 除零 trap / 移位掩码） */
                    Buf r; buf_init(&r);
                    const char *rs = cxs(n->b, n->a->ty);
                    if (!num_bin_c(&r, lt, n->op, l.data, rs))
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
    default:
        if (slot) line("%s = %s;", slot, cxs(n, slot_ty));
        else      line("%s;", exs(n));
        return;
    }
}

static void gen_block_stmts(Node *b) {
    for (int i = 0; i < b->nitems; i++) gen_stmt(b->items[i]);
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
        /* 臂本身是控制流（`else if (…)` 就是这种形状）：值由内层的
         * gen_value 写进 slot，不能当成普通表达式 `slot = <expr>` 发。 */
        if (is_ctl(arm)) gen_value(arm, slot, slot_ty);
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
    if (n->flag && n->a->ct && n->a->ct->kind == V_BOOL) {
        if (n->a->ct->bval)   gen_arm(n->b, slot, slot_ty);
        else if (n->c)        gen_arm(n->c, slot, slot_ty);
        return;
    }

    Type *c = n->a->ty;
    bool is_opt = c && c->kind == TY_OPT;
    bool is_err = c && c->kind == TY_ERR;

    /* `?T` / `!T` 作条件：判的是 tag（`if (x)` 对 `!void` 也要走这条，
     * 那时没有载荷可捕获，只有 tag 可判）。 */
    if (is_opt || is_err) {
        static int cn;
        char tmp[64];
        snprintf(tmp, sizeof tmp, "_c%d", cn++);
        line("%s %s = %s;", cty(c), tmp, cxs(n->a, c));
        if (is_opt) line("if (%s.tag) {", tmp);
        else        line("if (%s.tag == 0) {", tmp);
        g_ind++;
        CScope s; cscope_push(&s);
        /* `!void` 的载荷是 0 字节，没有可声明的变量（语义层已就此报错，
         * 这里只是不让坏代码变成 `void v = …` 这种非法 C）。 */
        if (n->name && c->elem->kind != TY_VOID)
            line("%s %s = %s.payload;", cty(c->elem), cnames(n->name), tmp);
        gen_arm(n->b, slot, slot_ty);
        cscope_pop();
        g_ind--;
        line("} else {");
        g_ind++;
        if (n->c) {
            CScope s2; cscope_push(&s2);
            if (n->d && n->d->kind == NK_IDENT) {
                /* else 捕获只属于 `!T`（`?T` + else 捕获已被 sema 拒掉）。
                 * is_opt 分支只是防御：tag 是 bool。 */
                Type *et = is_err ? ty_error() : ty_bool();
                line("%s %s = %s.tag;", cty(et), cnames(n->d->name), tmp);
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
            char tmp[64];
            snprintf(tmp, sizeof tmp, "_w%d", cn++);
            line("%s %s = %s;", cty(c), tmp, cxs(n->a, c));
            if (is_opt) line("if (!%s.tag) break;", tmp);
            else        line("if (%s.tag != 0) break;", tmp);
            CScope s; cscope_push(&s);
            if (n->name && c->elem->kind != TY_VOID)
                line("%s %s = %s.payload;", cty(c->elem), cnames(n->name), tmp);
            gen_block_stmts(n->b);
            line("Lcont%d: ;", id);
            cscope_leave();
            g_ind--;
            line("}");
        } else {
            line("while %s {", exs_cond(n->a));
            g_ind++;
            CScope s; cscope_push(&s);
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
    static char bufs[4][64];
    static int rot;
    char *b = bufs[rot++ & 3];
    (void)st;
    snprintf(b, 64, "%s.tag", tmp);
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
                Buf nb; buf_init(&nb); buf_printf(&nb, "%s.payload", tmp); src = nb.data;
            } else if (st && st->kind == TY_ERR) {
                bool is_err_pat = vname && !strcmp(vname, "err");
                if (is_err_pat) { cap = ty_error();  Buf nb; buf_init(&nb); buf_printf(&nb, "%s.tag", tmp); src = nb.data; }
                else            { cap = st->elem;    Buf nb; buf_init(&nb); buf_printf(&nb, "%s.payload", tmp); src = nb.data; }
            } else if (st) {
                cap = st;
                src = tmp;
            }
            /* 0 字节的载荷（`!void` 的 `.ok`）没有可声明的 C 变量；
             * 语义层已经就此报错，这里只是别生成 `void v = …`。 */
            if (cap && src && cap->kind != TY_VOID)
                line("%s %s = %s;", cty(cap), cnames(arm->name), src);
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

/* 跨模块可见的函数原型（不带 static），最后进共享头文件 */
static Buf  g_bpub;
/* 每个模块的延迟初始化函数名，main 里依次调用 */
static char *g_initname[64];
static int   g_ninit;

static void gen_params(Buf *b, Node *fn, bool is_main) {
    if (is_main && g_need_proc) {          /* std.process 要接住命令行 */
        buf_puts(b, "int argc, char **argv");
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
        if (pm->kind == NK_PARAM && pm->name) cname(b, pm->name);
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
        buf_puts(&sig, pm->name ? pm->name : "_p");
    }
    buf_putc(&sig, ')');
    bline(&g_bpub, "%s;", sig.data);
}

static void gen_fn(Node *fn, const char *name, bool is_main, bool exported) {
    Type *ft = fn->type_val;
    Type *ret = (ft && ft->kind == TY_FN) ? ft->ret : ty_void();

    Buf sig; buf_init(&sig);
    if (is_main) {
        buf_puts(&sig, "int ");
        buf_puts(&sig, "main");
    } else {
        if (!exported) buf_puts(&sig, "static ");
        ctype(&sig, ret);
        buf_putc(&sig, ' ');
        cname(&sig, name ? name : "anon");
    }
    buf_putc(&sig, '(');
    gen_params(&sig, fn, is_main);
    buf_putc(&sig, ')');

    bline(exported ? &g_bpub : &g_bproto, "%s;", sig.data);

    g_out = &g_bbody;
    if (is_main && g_need_proc) {
        /* std.process 的数据源：main 收到的 argc/argv 存进这两份
         * 外部链接的全局量（运行时辅助在共享头里声明为 extern）。 */
        line("int zx_proc_argc = 0;");
        line("char **zx_proc_argv = 0;");
    }
    line("%s {", sig.data);
    g_ind++;
    if (is_main) {
        for (int i = 0; i < g_ninit; i++) line("%s();", g_initname[i]);
        if (g_need_proc) {
            line("zx_proc_argc = argc;");
            line("zx_proc_argv = argv;");
        }
    }

    g_cur_ret = ret;
    g_is_main = is_main;
    g_saw_errdefer = false;

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
    /* 标量放进 ?T / !T：补上载荷+标签 */
    if (t && (t->kind == TY_OPT || t->kind == TY_ERR) &&
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
        buf_puts(b, "{0}");
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
        if (t && t->kind == TY_PTR && t->has_len) { arr_et = t->elem; t = t->elem; }
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
        if (bt && (bt->kind == TY_STRUCT || bt->kind == TY_UNION) &&
            ty_field(bt, n->name))
            return false;
        if (!strcmp(n->name, "size")) return true;             /* comptime */
        if (!strcmp(n->name, "len"))                            /* 切片自己存了 len */
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
    case NK_SLICE: case NK_UN: case NK_CAST: case NK_IDENT: case NK_FIELD:
        return true;
    /* 指针算术的结果仍是视图：物化就成了"另拷一份数据" */
    case NK_BIN:
        return n->ty && n->ty->kind == TY_PTR &&
               (n->op == TOK_PLUS || n->op == TOK_MINUS);
    default:
        return false;
    }
}

static Node **g_late;      /* 需要运行时初始化的全局 */
static int    g_nlate, g_clate;

static void gen_global(Node *d, bool exported) {
    const char *st = exported ? "" : "static ";
    Type *t = d->ty;
    if (!t || t->kind == TY_INVALID || t->kind == TY_TYPE) return;
    if (t->kind == TY_VOID) return;      /* `const x = { 42; }` 没有运行时表示 */
    if (sema_ty_ct_only(t)) {            /* std.TypeInfo 等：编译期绑定，不物化 */
        if (t->kind == TY_STRUCT || t->kind == TY_UNION)
            for (int i = 0; i < t->nfields; i++)
                if (!sema_ty_ct_only(t->fields[i].ty)) ensure_type(t->fields[i].ty);
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
            char *inn = cnames(id->name ? id->name : "_");
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

    char *cn = cnames(sym_cname(d));
    if (exported) {                     /* 别的单元要能链到：extern 声明进头文件 */
        Buf e; buf_init(&e);
        buf_puts(&e, "extern ");
        if (t->kind == TY_PTR && t->has_len) {
            ctype(&e, t->elem);
            buf_printf(&e, " %s[];\n", cn);
        } else {
            ctype(&e, t);
            buf_printf(&e, " %s;\n", cn);
        }
        buf_puts(&g_bpub, e.data);
    }

    /* 数组：常量字面量 / undefined / 编译期算出来的表 → 真正的 C 数组 */
    if (t->kind == TY_PTR && t->has_len) {
        Node *init = d->b;
        /* 视图（切片 / 指向已有对象的指针）：值就是一个地址。物化成静态数据会把
         * 数据复制一份——之后写进去也看不见，所以按源码表达式求值。 */
        bool view = init && init->ct && init->ct->kind == V_PTR && init->ct->base &&
                    is_view_expr(init);
        if (init && init->ct && !view) {
            Buf s; buf_init(&s);
            buf_puts(&s, st);
            ctype(&s, t->elem);
            buf_printf(&s, " %s[%llu] = ", cn, (unsigned long long)t->len);
            val_init(&s, init->ct, t);
            buf_puts(&s, ";");
            bline(&g_bglobals, "%s", s.data);
            return;
        }
        if (!view && (!init || init->kind == NK_UNDEF ||
                      init->kind == NK_ARRAY_LIT ||
                      (init->kind == NK_UN && init->op == TOK_AMP))) {
            Buf s; buf_init(&s);
            buf_puts(&s, st);
            ctype(&s, t->elem);
            buf_printf(&s, " %s[%llu]", cn, (unsigned long long)t->len);
            if (init && init->kind != NK_UNDEF) {
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
        Buf s; buf_init(&s);
        buf_printf(&s, "%s%s *%s = %s;", st, cty(t->elem), cn, cxs(init, t));
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
    if (d->b->ct && !view) {
        Buf s; buf_init(&s);
        val_init(&s, d->b->ct, t);
        bline(&g_bglobals, "%s%s %s = %s;", st, cty(t), cn, s.data);
        return;
    }

    /* 不是 C 常量表达式：先声明，等 main 开头再赋值 */
    if (view || !is_const_init(d->b) || is_ctl(d->b)) {
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
        char *cn = cnames(sym_cname(d));
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
int backend_emit_split(Node *root, Buf *header, Buf *rt, BUnit *units, int maxunits) {
    buf_init(&g_btypes);
    buf_init(&g_bpub);
    buf_init(&g_bglobals);
    buf_init(&g_bproto);
    buf_init(&g_bbody);
    g_out = &g_bbody;
    g_ind = 0;

    int nmods = sema_module_count();
    if (nmods <= 0) nmods = 1;
    if (nmods > maxunits) nmods = maxunits;
    #define MOD_ROOT(mi) ((mi) == 0 ? root : sema_module_root(mi))

    g_need_proc = scan_proc_use(root);
    for (int mi = 1; mi < nmods && !g_need_proc; mi++)
        g_need_proc = scan_proc_use(MOD_ROOT(mi));

    /* 初始化函数表：被依赖的先初始化（模块表里入口排在最前） */
    g_ninit = 0;
    for (int mi = nmods - 1; mi >= 0 && g_ninit < 64; mi--) {
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
                if (it->c) { gen_global(it, false); continue; }
                if (!it->b) continue;
                if (it->b->kind == NK_FN || it->b->kind == NK_MODULE) continue;
                if (it->b->ty && it->b->ty->kind == TY_TYPE) continue;
                /* 同函数：模块全局也要能被（发射到别处的）方法体引用 */
                bool exported = (mi != 0);
                gen_global(it, exported);
            }
        }
        emit_init_fn(mi);

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
    if (g_need_heapalloc) emit_heapalloc_runtime();
    if (g_need_div) emit_div_runtime();
    if (g_nuw) emit_unwrap_runtime();
    /* 128 位转十进制：编译期 printf 路径（g_need_i128）与 zx_fmt 的
     * kind 9/10 分支（fmt 运行时一存在就可能碰到）都要有。 */
    if (g_need_i128 || g_need_fmt || g_need_allocfmt || g_need_page || g_need_any)
        emit_i128_runtime();
    if (g_need_fmt || g_need_allocfmt || g_need_page || g_need_any) emit_fmt_runtime();
    emit_any_runtime();

    /* 头文件 */
    buf_puts(header, "/* generated by zxc — do not edit */\n");
    buf_puts(header, "#ifndef ZX_GEN_H\n#define ZX_GEN_H\n\n");
    buf_puts(header, "#include <stdint.h>\n#include <stdbool.h>\n");
    buf_puts(header, "#include <stddef.h>\n#include <stdio.h>\n#include <stdlib.h>\n");
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
    BUnit units[64];
    int n = backend_emit_split(root, &header, &rt, units, 64);
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
}
