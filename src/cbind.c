/* cbind.c — `zxc include`：把 C 头文件翻成一份可直接 module() 的 Zx 绑定。
 *
 * 为什么走 clang 的文本 AST：C 头的脏活（typedef、#ifdef、匿名 struct、
 * 调用约定、位宽）clang 已经做完了，AST 里每个声明都带着规整的 qualType
 * 和所在文件。这里只做「挑出来 + 翻类型名」，不引入 JSON 解析器，也不
 * 自己预处理 C。工具链就是项目里 vendored 的 zig cc。
 *
 * 生成物是普通 Zx 源码：extern "C" 块 + @link + 常量。它进 vendor/ 之后
 * 和手写的绑定没有任何区别，LSP 的补全 / hover / inlayHint 全部照常。
 * 翻不了的声明一律跳过并在文件末尾留注释，绝不猜一个错的 ABI 出来。
 *
 * 控制台输出只用 ASCII；写进 .zx 的注释是模板内容，可以是中文。
 */
#include "cbind.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <direct.h>
#else
#include <unistd.h>
#endif

#define MAX_TYPE 512
#define MAX_NAME 128
#define MAX_TAGS 1024
#define MAX_ITEMS 96

/* 系统头（内置配方）模式：只导出基础类型函数签名。SDK 内部结构彼此
 * 引用、还带位域/匿名 union，硬生成只会产出编译不过的绑定。要结构体
 * 就把库的头文件放进工程、用路径方式 add（那条路径是完整的）。 */
static int g_basic_only;

/* 不完整 C 句柄（typedef struct X X; 但没有 definition）：Zx 没有 opaque
 * 类型，生成一个 1 字节占位 struct，引用它的函数签名才不会 E0401。
 * 这些句柄只传指针、不解引用，占位布局不影响 ABI。 */
#define MAX_OPAQUE 64
static char g_opaque[MAX_OPAQUE][MAX_NAME];
static int  g_nopaque;
static char g_defrec[MAX_OPAQUE][MAX_NAME];
static int  g_ndefrec;

static void rec_put(char tab[][MAX_NAME], int *n, const char *name) {
    if (!name || !*name || *n >= MAX_OPAQUE) return;
    for (int i = 0; i < *n; i++) if (!strcmp(tab[i], name)) return;
    snprintf(tab[*n], MAX_NAME, "%s", name);
    (*n)++;
}
static int rec_has(char tab[][MAX_NAME], int n, const char *name) {
    for (int i = 0; i < n; i++) if (!strcmp(tab[i], name)) return 1;
    return 0;
}

/* ---------------- 小工具 ---------------- */

typedef struct { char *p; size_t n, cap; } SB;

static void sb_need(SB *b, size_t extra) {
    if (b->n + extra + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 1024;
    while (cap < b->n + extra + 1) cap *= 2;
    char *p = (char *)realloc(b->p, cap);
    if (!p) { fprintf(stderr, "zxc include: out of memory\n"); exit(1); }
    b->p = p; b->cap = cap;
}
static void sb_addn(SB *b, const char *s, size_t n) {
    sb_need(b, n);
    memcpy(b->p + b->n, s, n);
    b->n += n; b->p[b->n] = 0;
}
static void sb_puts(SB *b, const char *s) { sb_addn(b, s, strlen(s)); }
static void sb_printf(SB *b, const char *fmt, ...) {
    char tmp[2048];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof tmp) n = (int)sizeof tmp - 1;
    sb_addn(b, tmp, (size_t)n);
}

static char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = 0;
    if (len) *len = got;
    return buf;
}

static int write_text(const char *path, const char *s) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(s, 1, strlen(s), f);
    fclose(f);
    return 0;
}

static int ident_ch(int c) { return isalnum((unsigned char)c) || c == '_'; }

static void str_trim(char *s) {
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

static void path_dir(const char *path, char *out, size_t cap) {
    const char *slash = strrchr(path, '/');
    const char *bs = strrchr(path, '\\');
    if (bs && (!slash || bs > slash)) slash = bs;
    if (!slash) { snprintf(out, cap, "."); return; }
    size_t n = (size_t)(slash - path);
    if (!n) n = 1;                        /* "/foo.h" -> "/" */
    if (n >= cap) n = cap - 1;
    memcpy(out, path, n); out[n] = 0;
}

static const char *path_base(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *bs = strrchr(path, '\\');
    if (bs && (!slash || bs > slash)) slash = bs;
    return slash ? slash + 1 : path;
}

/* 系统头（zig libc / Windows SDK）：目标头是本地库时不收这些声明，
 * 免得把 stdio.h 内部那几百个符号灌进绑定。目标本身就是系统头时放行。 */
static int is_system_header(const char *path) {
    return strstr(path, ".toolchain") || strstr(path, "libc\\include") ||
           strstr(path, "libc/include") || strstr(path, "/usr/include") ||
           strstr(path, "Windows Kits");
}

/* ---------------- typedef 表（pass 0 收集，pass 1 展开） ---------------- */

typedef struct { char name[MAX_NAME]; char type[MAX_TYPE]; } Tag;
static Tag g_tags[MAX_TAGS];
static int g_ntags;

static void tag_put(const char *name, const char *type) {
    if (!name || !*name || !type || !*type) return;
    for (int i = 0; i < g_ntags; i++)
        if (!strcmp(g_tags[i].name, name)) {
            snprintf(g_tags[i].type, MAX_TYPE, "%s", type);
            return;
        }
    if (g_ntags >= MAX_TAGS) return;
    snprintf(g_tags[g_ntags].name, MAX_NAME, "%s", name);
    snprintf(g_tags[g_ntags].type, MAX_TYPE, "%s", type);
    g_ntags++;
}
static const char *tag_get(const char *name) {
    for (int i = 0; i < g_ntags; i++)
        if (!strcmp(g_tags[i].name, name)) return g_tags[i].type;
    return NULL;
}

/* Zx 关键字：C 头里的参数名撞上就加个下划线，别让生成物直接编译不过。 */
static const char *g_kw[] = {
    "fn","let","var","const","pub","if","else","while","for","switch","case",
    "return","break","continue","struct","union","enum","module","extern",
    "type","self","try","catch","defer","inline","comptime","test","error",
    "true","false","null","undefined","and","or","not","as","is","in",NULL
};
static int is_zx_kw(const char *s) {
    for (int i = 0; g_kw[i]; i++) if (!strcmp(g_kw[i], s)) return 1;
    return 0;
}

/* ---------------- C 类型 → Zx 类型 ---------------- */

/* 去掉 C 的调用约定 / 限定词 / attribute 噪声，只留能映射的部分。 */
static void clean_type(const char *src, char *dst, size_t cap) {
    size_t n = 0;
    const char *p = src;
    static const char *noise[] = {
        "__attribute__", "__declspec", "__cdecl", "__stdcall", "__fastcall",
        "WINAPI", "APIENTRY", "CALLBACK", "restrict", "__restrict",
        "__restrict__", "_Nonnull", "_Nullable", "volatile", "__extension__", NULL
    };
    while (*p && n + 1 < cap) {
        int hit = 0;
        for (int i = 0; noise[i]; i++) {
            size_t l = strlen(noise[i]);
            if (!strncmp(p, noise[i], l)) {
                /* __attribute__((...)) / __declspec(...) 连括号一起吞 */
                p += l;
                if (!strcmp(noise[i], "__attribute__") ||
                    !strcmp(noise[i], "__declspec")) {
                    if (*p == '(') {
                        int dep = 0;
                        do {
                            if (*p == '(') dep++;
                            else if (*p == ')') dep--;
                            p++;
                        } while (*p && dep > 0);
                    }
                }
                if (n && dst[n-1] != ' ') dst[n++] = ' ';
                hit = 1; break;
            }
        }
        if (hit) continue;
        dst[n++] = *p++;
    }
    dst[n] = 0;
    str_trim(dst);
}

/* 基础类型名 → Zx。整数宽度按 Windows LLP64（long = 32 位）算，
 * 这正是项目默认目标；指针一律按机器字。 */
static int map_base(const char *t, char *out, size_t cap, int depth) {
    if (depth > 6) return 0;
    const char *td = tag_get(t);
    if (td) return map_base(td, out, cap, depth + 1);

    struct { const char *c; const char *z; } tbl[] = {
        { "void", "void" }, { "_Bool", "bool" }, { "bool", "bool" },
        { "signed char", "i8" }, { "unsigned char", "u8" }, { "char", "u8" },
        { "short", "i16" }, { "short int", "i16" }, { "signed short", "i16" },
        { "unsigned short", "u16" }, { "unsigned short int", "u16" },
        { "int", "i32" }, { "signed", "i32" }, { "signed int", "i32" },
        { "unsigned", "u32" }, { "unsigned int", "u32" },
        { "long", "i32" }, { "long int", "i32" }, { "signed long", "i32" },
        { "unsigned long", "u32" }, { "unsigned long int", "u32" },
        { "long long", "i64" }, { "long long int", "i64" },
        { "signed long long", "i64" }, { "unsigned long long", "u64" },
        { "unsigned long long int", "u64" },
        { "float", "f32" }, { "double", "f64" }, { "long double", "f64" },
        { "int8_t", "i8" }, { "uint8_t", "u8" },
        { "int16_t", "i16" }, { "uint16_t", "u16" },
        { "int32_t", "i32" }, { "uint32_t", "u32" },
        { "int64_t", "i64" }, { "uint64_t", "u64" },
        { "size_t", "usize" }, { "rsize_t", "usize" },
        { "ssize_t", "isize" }, { "ptrdiff_t", "isize" },
        { "intptr_t", "isize" }, { "uintptr_t", "usize" },
        { "wchar_t", "u16" }, { "__int8", "i8" }, { "__int16", "i16" },
        { "__int32", "i32" }, { "__int64", "i64" },
        { "unsigned __int8", "u8" }, { "unsigned __int16", "u16" },
        { "unsigned __int32", "u32" }, { "unsigned __int64", "u64" },
        { NULL, NULL }
    };
    for (int i = 0; tbl[i].c; i++)
        if (!strcmp(t, tbl[i].c)) { snprintf(out, cap, "%s", tbl[i].z); return 1; }

    /* struct X / union X -> X（生成物里同名 extern struct）；enum X -> i32 */
    if (!strncmp(t, "struct ", 7) || !strncmp(t, "union ", 6) ||
        !strncmp(t, "enum ", 5)) {
        if (g_basic_only && strncmp(t, "enum ", 5)) return 0;
        const char *nm = strchr(t, ' ') + 1;
        str_trim((char *)nm);
        if (*nm && ident_ch((unsigned char)*nm)) {
            snprintf(out, cap, "%s", !strncmp(t, "enum ", 5) ? "i32" : nm);
            return 1;
        }
    }
    return 0;
}

/* 把一个 C 声明类型翻成 Zx 类型。
 *   field=1 时数组参数保留成 `*[N]T`（extern struct 的 C 内联数组）；
 *   field=0 时数组退化为指针（C 函数形参本来就是指针）。 */
static int map_type(const char *raw, char *out, size_t cap, int field) {
    char t[MAX_TYPE];
    clean_type(raw, t, sizeof t);
    str_trim(t);
    if (!*t) return 0;

    /* 函数指针：RET (*)(ARGS) -> fn (ARGS) RET。只收规整的单层形式。 */
    const char *fp = strstr(t, "(*)");
    if (fp) {
        char ret[MAX_TYPE], args[MAX_TYPE];
        size_t rn = (size_t)(fp - t);
        if (rn >= sizeof ret) return 0;
        memcpy(ret, t, rn); ret[rn] = 0; str_trim(ret);
        const char *lp = strchr(fp + 3, '(');
        const char *rp = strrchr(t, ')');
        if (!lp || !rp || rp <= lp) return 0;
        size_t an = (size_t)(rp - lp - 1);
        if (an >= sizeof args) return 0;
        memcpy(args, lp + 1, an); args[an] = 0; str_trim(args);
        char rzx[MAX_TYPE];
        if (!map_type(ret, rzx, sizeof rzx, 0)) return 0;
        SB a; memset(&a, 0, sizeof a);
        sb_printf(&a, "fn (");
        if (*args && strcmp(args, "void")) {
            /* 按顶层逗号切参数类型；嵌套括号里的逗号不算 */
            int dep = 0, start = 0, first = 1;
            for (size_t i = 0; i <= strlen(args); i++) {
                char c = args[i];
                if (c == '(') dep++;
                else if (c == ')') dep--;
                if ((c == ',' && dep == 0) || c == 0) {
                    char part[MAX_TYPE], pz[MAX_TYPE];
                    size_t ln = i - (size_t)start;
                    if (ln >= sizeof part) goto fp_fail;
                    memcpy(part, args + start, ln); part[ln] = 0; str_trim(part);
                    if (!map_type(part, pz, sizeof pz, 0)) goto fp_fail;
                    if (!first) sb_puts(&a, ", ");
                    sb_puts(&a, pz); first = 0;
                    start = (int)i + 1;
                }
            }
        }
        sb_printf(&a, ") %s", rzx);
        if (a.n + 1 > cap) { free(a.p); return 0; }
        memcpy(out, a.p, a.n + 1); free(a.p);
        return 1;
fp_fail:
        free(a.p); return 0;
    }

    /* 分离数组、指针、const */
    int array_n = 0;
    char *br = strchr(t, '[');
    if (br) {
        array_n = atoi(br + 1);
        *br = 0;
        str_trim(t);
    }
    int np = 0;
    char *star = strchr(t, '*');
    while (star) {
        np++;
        char *q = star;
        while (*q) { q[0] = q[1]; q++; }   /* 抹掉这个 '*' */
        star = strchr(t, '*');
    }
    str_trim(t);
    if (np > 1 && !field) return 0;        /* 多级指针：不猜，跳过 */
    if (np > 1) return 0;

    int const_pointee = 0;
    if (!strncmp(t, "const ", 6)) { const_pointee = 1; memmove(t, t + 6, strlen(t + 6) + 1); }
    size_t ln = strlen(t);
    if (ln > 6 && !strcmp(t + ln - 6, " const")) { t[ln - 6] = 0; str_trim(t); }
    if (!strncmp(t, "volatile ", 9)) memmove(t, t + 9, strlen(t + 9) + 1);
    str_trim(t);

    char base[MAX_TYPE];
    if (!map_base(t, base, sizeof base, 0)) return 0;
    /* Zx 没有 void*：C 的 void* 就是一个机器字，用 *u8 占位，ABI 等价。 */
    if (!strcmp(base, "void") && (np || array_n)) snprintf(base, sizeof base, "u8");
    if (!strcmp(base, "void")) { snprintf(out, cap, "void"); return 1; }

    if (array_n > 0) {
        if (field) snprintf(out, cap, "*[%d]%s", array_n, base);
        else snprintf(out, cap, "*%s", base);
        return 1;
    }
    if (np == 1) snprintf(out, cap, "*%s%s", const_pointee ? "const " : "", base);
    else snprintf(out, cap, "%s", base);
    return 1;
}

/* ---------------- clang 文本 AST 的一行 ---------------- */

typedef struct {
    char kind[40];
    char name[MAX_NAME];
    char type[MAX_TYPE];
    char file[512];
    int  depth;
    int  implicit, is_inline, is_def, is_extern, is_static;
} ALine;

static int known_kind(const char *k) {
    static const char *ks[] = {
        "FunctionDecl","ParmVarDecl","VarDecl","FieldDecl","RecordDecl",
        "TypedefDecl","EnumDecl","EnumConstantDecl","TranslationUnitDecl", NULL
    };
    for (int i = 0; ks[i]; i++) if (!strcmp(ks[i], k)) return 1;
    return 0;
}

/* 取最后一对单引号里的内容（canonical qualType，typedef 已展开）。 */
static int last_quote(const char *s, char *out, size_t cap) {
    const char *q[4] = { NULL, NULL, NULL, NULL };
    int n = 0;
    for (const char *p = s; *p && n < 4; p++)
        if (*p == '\'') q[n++] = p;
    if (n < 2) return 0;
    /* clang 对 typedef 会写 'A':'B'（A=sugar，B=canonical）。
     * B 更干净：typedef 已展开、attribute 已剥掉，取它。 */
    const char *a, *b;
    if (n >= 4 && q[2] - q[1] <= 2) { a = q[2] + 1; b = q[3]; }
    else                            { a = q[0] + 1; b = q[1]; }
    if (!b || b <= a) return 0;
    size_t len = (size_t)(b - a);
    if (len >= cap) len = cap - 1;
    memcpy(out, a, len); out[len] = 0;
    str_trim(out);
    return 1;
}

static int first_quote_text(const char *s, char *out, size_t cap) {
    const char *a = strchr(s, '\'');
    if (!a) return 0;
    const char *b = strchr(a + 1, '\'');
    if (!b) return 0;
    size_t n = (size_t)(b - a - 1);
    if (n >= cap) n = cap - 1;
    memcpy(out, a + 1, n); out[n] = 0;
    str_trim(out);
    return 1;
}

/* 从行里抠 `<...>` 的位置信息；若带文件路径就写进 a->file。 */
static void parse_loc(const char *s, ALine *a) {
    const char *lt = strchr(s, '<');
    if (!lt) return;
    const char *gt = strchr(lt + 1, '>');
    if (!gt) return;
    size_t n = (size_t)(gt - lt - 1);
    if (n >= 511) n = 510;
    char loc[512];
    memcpy(loc, lt + 1, n); loc[n] = 0;
    if (!strncmp(loc, "line:", 5) || !strncmp(loc, "col:", 4) ||
        strstr(loc, "invalid sloc")) return;
    /* 找 ":数字:数字"（或 ", col:" 之前的那个），其前面就是路径 */
    const char *p = loc;
    const char *best = NULL;
    for (; *p; p++) {
        if (*p != ':') continue;
        if (!isdigit((unsigned char)p[1])) continue;
        const char *q = p + 1;
        while (isdigit((unsigned char)*q)) q++;
        if (*q != ':') continue;
        best = p;
        break;
    }
    if (!best) return;
    size_t fn = (size_t)(best - loc);
    if (!fn || fn >= sizeof a->file) return;
    memcpy(a->file, loc, fn); a->file[fn] = 0;
}

static int parse_line(const char *line, ALine *a) {
    memset(a, 0, sizeof *a);
    const char *p = line;
    int depth = 0;
    for (;;) {
        if (p[0] == '|' && p[1] == ' ') { depth++; p += 2; continue; }
        if (p[0] == ' ' && p[1] == ' ') { depth++; p += 2; continue; }
        break;
    }
    if (!((p[0] == '|' && p[1] == '-') || (p[0] == '`' && p[1] == '-'))) return 0;
    p += 2;
    if (!isalpha((unsigned char)*p)) return 0;
    size_t k = 0;
    while (ident_ch((unsigned char)p[k]) && k + 1 < sizeof a->kind) k++;
    memcpy(a->kind, p, k); a->kind[k] = 0;
    if (!known_kind(a->kind)) return 0;
    a->depth = depth;
    const char *rest = p + k;

    a->implicit   = strstr(rest, "implicit")    != NULL;
    a->is_inline  = strstr(rest, "inline")      != NULL;
    a->is_def     = strstr(rest, "definition")  != NULL;
    a->is_extern  = strstr(rest, " extern")     != NULL;
    a->is_static  = strstr(rest, " static")     != NULL;
    parse_loc(rest, a);

    const char *q = strchr(rest, '\'');
    if (q) {
        last_quote(rest, a->type, sizeof a->type);
        /* 名字 = 第一个引号往前的一个标识符 */
        const char *e = q;
        while (e > rest && isspace((unsigned char)e[-1])) e--;
        const char *b = e;
        while (b > rest && ident_ch((unsigned char)b[-1])) b--;
        if (b < e) {
            size_t nn = (size_t)(e - b);
            if (nn >= sizeof a->name) nn = sizeof a->name - 1;
            memcpy(a->name, b, nn); a->name[nn] = 0;
            /* 无名形参的引号前面是 `col:48` 这种位置标记，倒扫会把数字
             * 当成名字。数字开头一律按无名处理（调用处补 a0/a1）。 */
            if (!isalpha((unsigned char)a->name[0]) && a->name[0] != '_')
                a->name[0] = 0;
        }
    } else {
        /* RecordDecl / EnumDecl 行没有引号。按 token 扫 `<...>` 之后的部分，
         * 丢掉 col:/line: 位置标记和 used/referenced 之类修饰，
         * `struct|union|enum` 后那个标识符才是名字。 */
        const char *gt = strchr(rest, '>');
        const char *s = gt ? gt + 1 : rest;
        int after_tag = 0;
        while (*s) {
            while (*s && isspace((unsigned char)*s)) s++;
            if (!*s) break;
            const char *b = s;
            while (*s && !isspace((unsigned char)*s)) s++;
            size_t l = (size_t)(s - b);
            char tok[64];
            if (l >= sizeof tok) l = sizeof tok - 1;
            memcpy(tok, b, l); tok[l] = 0;
            if (tok[0] == '<' || isdigit((unsigned char)tok[0]) ||
                !strncmp(tok, "col:", 4) || !strncmp(tok, "line:", 5) ||
                !strcmp(tok, "used") || !strcmp(tok, "referenced") ||
                !strcmp(tok, "implicit") || !strcmp(tok, "extern") ||
                !strcmp(tok, "static") || !strcmp(tok, "prev") ||
                !strcmp(tok, "definition")) continue;
            if (!strcmp(tok, "struct") || !strcmp(tok, "union") ||
                !strcmp(tok, "enum")) { after_tag = 1; continue; }
            if (!strcmp(a->kind, "RecordDecl") && !after_tag) continue;
            snprintf(a->name, sizeof a->name, "%s", tok);
            break;
        }
    }
    return 1;
}

/* ---------------- 声明收集与输出 ---------------- */

typedef struct {
    char name[MAX_NAME];
    char type[MAX_TYPE];
    int  have_val;
    long long val;
} Item;

typedef struct {
    char kind[40], name[MAX_NAME], type[MAX_TYPE], file[512];
    int active, anonymous, is_union, is_def, is_inline, is_extern, is_static;
    Item items[MAX_ITEMS];
    int nitems;
} Cur;

static int g_nskip;
static SB g_skipped;
static char g_seen[MAX_TAGS][MAX_NAME * 2];
static int g_nseen;

/* 同名声明去重：math.h 里的 sin 会被 clang 吐两遍（implicit 内建 + 真声明），
 * 重复发到 Zx 侧就是 duplicate declaration。保留第一次遇到的那条。 */
static int seen_dup(const char *kind, const char *name) {
    if (!name || !*name) return 0;
    if (strcmp(kind, "FunctionDecl") && strcmp(kind, "VarDecl") &&
        strcmp(kind, "RecordDecl") && strcmp(kind, "EnumDecl")) return 0;
    char key[MAX_NAME * 2];
    snprintf(key, sizeof key, "%s:%s", kind, name);
    for (int i = 0; i < g_nseen; i++)
        if (!strcmp(g_seen[i], key)) return 1;
    if (g_nseen < MAX_TAGS) { snprintf(g_seen[g_nseen++], MAX_NAME * 2, "%s", key); }
    return 0;
}

static void note_skip(const char *kind, const char *name, const char *why) {
    g_nskip++;
    sb_printf(&g_skipped, "//   skipped %s %s%s%s\n", kind,
              name && *name ? name : "<anonymous>", why ? " -- " : "", why ? why : "");
}

static void emit_struct(SB *types, const char *name, Cur *c, int is_union) {
    /* 先把所有字段翻完再落笔：少一个字段就整条作废，绝不吐出半截
     * struct —— C 结构体布局错一字节，运行时会安静地算错。 */
    char zt[MAX_ITEMS][MAX_TYPE];
    for (int i = 0; i < c->nitems; i++) {
        if (!map_type(c->items[i].type, zt[i], MAX_TYPE, 1) ||
            !strcmp(zt[i], "void") || !c->items[i].name[0]) {
            note_skip("struct", name, "unsupported field type (layout unsafe)");
            return;
        }
        /* C 的函数指针字段用 *u8 占位：布局同为指针宽度。直接写
         * `fn (...) void` 会被 extern struct 的 C 布局校验判为不合法。 */
        if (!strncmp(zt[i], "fn (", 4)) snprintf(zt[i], MAX_TYPE, "*u8");
    }
    sb_printf(types, "    pub const %s = %s {\n", name, is_union ? "union" : "struct");
    for (int i = 0; i < c->nitems; i++) {
        /* 字段名撞 Zx 关键字（Windows SDK 里的 `type`）就加个下划线：
         * 名字不进 C 布局，改名不影响 ABI，但能保住生成物可编译。 */
        char fn[MAX_NAME + 2];
        snprintf(fn, sizeof fn, "%s%s", c->items[i].name,
                 is_zx_kw(c->items[i].name) ? "_" : "");
        sb_printf(types, "        %s: %s%s\n", fn, zt[i],
                  i + 1 < c->nitems ? "," : "");
    }
    sb_puts(types, "    };\n");
}

/* 返回这次 flush 是否算「已输出」；匿名 RecordDecl 会转存到 pending。 */
static void flush_cur(Cur *c, SB *types, SB *funcs, SB *vars, SB *consts,
                      Cur *pending) {
    if (!c->active) return;
    const char *k = c->kind;
    const char *nm = c->name;

    if (!strcmp(k, "RecordDecl")) {
        if (!c->is_def) { c->active = 0; return; }   /* 前向声明 */
        if (!*nm) { *pending = *c; c->active = 0; return; }
        emit_struct(types, nm, c, c->is_union);
        c->active = 0; return;
    }
    if (!strcmp(k, "TypedefDecl")) {
        if (pending->active) {
            Cur tmp = *pending;
            pending->active = 0;
            emit_struct(types, nm, &tmp, tmp.is_union);
        }
        c->active = 0; return;
    }
    if (pending->active) {
        note_skip("struct", "<anonymous>", "typedef name not found next to it");
        pending->active = 0;
    }

    if (!strcmp(k, "EnumDecl")) {
        long long next = 0;
        for (int i = 0; i < c->nitems; i++) {
            long long v = c->items[i].have_val ? c->items[i].val : next;
            sb_printf(consts, "pub const %s: i32 = %lld;\n", c->items[i].name, v);
            next = v + 1;
        }
        c->active = 0; return;
    }
    if (!strcmp(k, "VarDecl")) {
        if (!*nm) { c->active = 0; return; }
        if (c->is_static || c->is_inline) { c->active = 0; return; }
        char zt[MAX_TYPE];
        if (!map_type(c->type, zt, sizeof zt, 0) || !strcmp(zt, "void")) {
            note_skip("var", nm, "unsupported C type"); c->active = 0; return;
        }
        /* extern var 必须单独写在文件根（manual §3.14）：放在 extern "C"
         * 块里、且前面已有 extern fn 声明时，sema 会因 fn depth 把它当成
         * 函数内声明。块外逐条 `pub extern var` 才是规范写法。 */
        sb_printf(vars, "pub extern var %s: %s;\n", nm, zt);
        c->active = 0; return;
    }
    if (!strcmp(k, "FunctionDecl")) {
        if (!*nm || c->is_inline) { c->active = 0; return; }
        if (is_zx_kw(nm)) {
            note_skip("fn", nm, "name is a Zx keyword (cannot be emitted as a C symbol)");
            c->active = 0; return;
        }
        if (strstr(c->type, "...")) {
            note_skip("fn", nm, "variadic C function"); c->active = 0; return;
        }
        /* 返回类型 = qualType 里第一个 '(' 之前。不能只找 " ("：
         * `const char *(void)` 的星号后面紧跟括号，没有空格。 */
        char ret[MAX_TYPE];
        const char *lp = strchr(c->type, '(');
        if (!lp) { note_skip("fn", nm, "cannot parse signature"); c->active = 0; return; }
        size_t rn = (size_t)(lp - c->type);
        if (rn >= sizeof ret) { c->active = 0; return; }
        memcpy(ret, c->type, rn); ret[rn] = 0; str_trim(ret);
        char zr[MAX_TYPE];
        if (!map_type(ret, zr, sizeof zr, 0)) {
            note_skip("fn", nm, "unsupported return type"); c->active = 0; return;
        }
        SB sig; memset(&sig, 0, sizeof sig);
        sb_printf(&sig, "    pub fn %s(", nm);
        for (int i = 0; i < c->nitems; i++) {
            char zt[MAX_TYPE];
            if (!map_type(c->items[i].type, zt, sizeof zt, 0) ||
                !strcmp(zt, "void")) {
                note_skip("fn", nm, "unsupported parameter type");
                free(sig.p); c->active = 0; return;
            }
            char pn[MAX_NAME];
            snprintf(pn, sizeof pn, "%s", c->items[i].name);
            if (!*pn || is_zx_kw(pn)) snprintf(pn, sizeof pn, "a%d", i);
            if (i) sb_puts(&sig, ", ");
            sb_printf(&sig, "%s: %s", pn, zt);
        }
        sb_printf(&sig, ") %s;\n", zr);
        sb_addn(funcs, sig.p, sig.n);
        free(sig.p);
        c->active = 0; return;
    }
    c->active = 0;
}

/* 逐行扫 AST。pass=0 只填 typedef 表；pass=1 生成文本。
 * 只收「与目标头同目录」的声明：系统 libc 内部头、builtin 隐式声明不进绑定。 */
static void scan_ast(const char *text, int pass, int allow_system,
                     SB *types, SB *funcs, SB *vars, SB *consts) {
    char cur_file[512] = "";
    Cur cur, pending;
    memset(&cur, 0, sizeof cur);
    memset(&pending, 0, sizeof pending);

    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t ln = e ? (size_t)(e - p) : strlen(p);
        char line[4096];
        if (ln >= sizeof line) ln = sizeof line - 1;
        memcpy(line, p, ln); line[ln] = 0;
        p = e ? e + 1 : p + ln;

        ALine a;
        /* 枚举常量值：`|-value: Int 1` 这类行没有 Kind，单独认 */
        if (pass && cur.active && !strcmp(cur.kind, "EnumDecl") &&
            cur.nitems > 0 && strstr(line, "-value: ")) {
            const char *v = strstr(line, "-value: ");
            /* 形如 "-value: Int 1" / "-value: Long -3" */
            const char *sp = strchr(v + 8, ' ');
            if (sp) {
                cur.items[cur.nitems - 1].val = strtoll(sp + 1, NULL, 10);
                cur.items[cur.nitems - 1].have_val = 1;
            }
        }

        if (!parse_line(line, &a)) continue;
        if (a.file[0]) snprintf(cur_file, sizeof cur_file, "%s", a.file);
        /* 每个顶层节点（哪怕是要跳过的 implicit 内建声明）都要先结算上一条：
         * 否则它的 ParmVarDecl 子节点会漏进上一个函数，出现 sin(x, a1) 这种
         * 凭空多出来的参数。 */
        if (pass == 1 && a.depth == 0) {
            flush_cur(&cur, types, funcs, vars, consts, &pending);
            memset(&cur, 0, sizeof cur);
        }
        if (a.implicit || !strcmp(a.kind, "TranslationUnitDecl")) continue;
        if (!cur_file[0]) continue;                 /* builtin / 无位置信息 */
        if (is_system_header(cur_file) && !allow_system) continue;
        if (!strncmp(a.name, "__", 2)) continue;     /* 编译器/运行时内部符号 */
        /* CRT / SDK 内部符号：只过滤顶层声明，别把 math.h 形参 `_X` 也丢掉 */
        if (allow_system && a.depth == 0 && a.name[0] == '_') continue;

        if (pass == 0) {
            if (!strcmp(a.kind, "TypedefDecl") && a.name[0] && a.type[0])
                tag_put(a.name, a.type);
            continue;
        }

        /* 分清「前向声明」和「有 definition 的 struct」：前者要补占位类型。
         * 必须在 seen_dup 之前收，重复的 RecordDecl 才会被正确去重。 */
        if (a.depth == 0 && !strcmp(a.kind, "RecordDecl") && a.name[0])
            rec_put(a.is_def ? g_defrec : g_opaque,
                    a.is_def ? &g_ndefrec : &g_nopaque, a.name);

        if (allow_system && a.depth == 0 &&
            (!strcmp(a.kind, "RecordDecl") || !strcmp(a.kind, "VarDecl") ||
             !strcmp(a.kind, "EnumDecl"))) continue;

        if (a.depth == 0) {
            if (seen_dup(a.kind, a.name)) continue;
            cur.active = 1;
            snprintf(cur.kind, sizeof cur.kind, "%s", a.kind);
            snprintf(cur.name, sizeof cur.name, "%s", a.name);
            snprintf(cur.type, sizeof cur.type, "%s", a.type);
            snprintf(cur.file, sizeof cur.file, "%s", cur_file);
            cur.is_def = a.is_def; cur.is_inline = a.is_inline;
            cur.is_extern = a.is_extern; cur.is_static = a.is_static;
            cur.is_union = strstr(line, " union ") != NULL;
            cur.anonymous = !a.name[0];
            continue;
        }
        if (!cur.active) continue;
        if (!strcmp(cur.kind, "FunctionDecl") && !strcmp(a.kind, "ParmVarDecl")) {
            if (cur.nitems < MAX_ITEMS) {
                Item *it = &cur.items[cur.nitems++];
                snprintf(it->name, sizeof it->name, "%s", a.name);
                /* canonical 是展开后的类型；sugar 保留 typedef 名更好读 */
                char sugar[MAX_TYPE];
                if (first_quote_text(line, sugar, sizeof sugar) && sugar[0])
                    snprintf(it->type, sizeof it->type, "%s", sugar);
                else snprintf(it->type, sizeof it->type, "%s", a.type);
            }
        } else if (!strcmp(cur.kind, "RecordDecl") && !strcmp(a.kind, "FieldDecl")) {
            if (cur.nitems < MAX_ITEMS) {
                Item *it = &cur.items[cur.nitems++];
                snprintf(it->name, sizeof it->name, "%s", a.name);
                snprintf(it->type, sizeof it->type, "%s", a.type);
            }
        } else if (!strcmp(cur.kind, "EnumDecl") && !strcmp(a.kind, "EnumConstantDecl")) {
            if (cur.nitems < MAX_ITEMS) {
                Item *it = &cur.items[cur.nitems++];
                snprintf(it->name, sizeof it->name, "%s", a.name);
                it->val = 0; it->have_val = 0;
            }
        }
    }
    if (pass == 1) {
        flush_cur(&cur, types, funcs, vars, consts, &pending);
        if (pending.active) note_skip("struct", "<anonymous>", "no typedef name");
    }
}

/* ---------------- 头文件里的简单 #define 常量 ---------------- */

static int numeric_macro(const char *v, char *out, size_t cap, const char **zty) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", v);
    str_trim(buf);
    /* 去掉一层外层括号 */
    size_t n = strlen(buf);
    if (n > 2 && buf[0] == '(' && buf[n-1] == ')') {
        buf[n-1] = 0; memmove(buf, buf + 1, n - 1); str_trim(buf);
    }
    n = strlen(buf);
    if (!n) return 0;
    int is_float = 0, uns = 0;
    for (size_t i = 0; i < n; i++) {
        char c = buf[i];
        if (isdigit((unsigned char)c) || c == 'x' || c == 'X' ||
            (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') ||
            c == '+' || c == '-' || c == '.') continue;
        if (c == 'U' || c == 'u' || c == 'L' || c == 'l') { uns = 1; continue; }
        if (c == 'F' || c == 'f' || c == 'e' || c == 'E') { is_float = 1; continue; }
        return 0;                    /* 表达式宏：不猜，跳过 */
    }
    /* 剥掉 U/L/F 后缀（Zx 不认 42UL 这种写法） */
    char clean[256]; size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        char c = buf[i];
        if (c == 'U' || c == 'u' || c == 'L' || c == 'l' || c == 'F' || c == 'f') continue;
        clean[m++] = c;
    }
    clean[m] = 0;
    if (!m) return 0;
    *zty = is_float ? "f64" : (uns ? "u32" : "i32");
    snprintf(out, cap, "%s", clean);
    return 1;
}

/* 目录树模式下多个头可能定义同名宏，生成的 Zx 常量不能重复。 */
static char g_mseen[1024][MAX_NAME];
static int  g_nmseen;

static int macro_seen(const char *name) {
    for (int i = 0; i < g_nmseen; i++)
        if (!strcmp(g_mseen[i], name)) return 1;
    if (g_nmseen < (int)(sizeof g_mseen / sizeof g_mseen[0]))
        snprintf(g_mseen[g_nmseen++], MAX_NAME, "%s", name);
    return 0;
}

static void scan_macros(const char *header, SB *consts) {
    size_t len = 0;
    char *text = read_all(header, &len);
    if (!text) return;
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t ln = e ? (size_t)(e - p) : strlen(p);
        char line[1024];
        if (ln >= sizeof line) ln = sizeof line - 1;
        memcpy(line, p, ln); line[ln] = 0;
        p = e ? e + 1 : p + ln;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (strncmp(s, "#define", 7)) continue;
        s += 7;
        while (*s == ' ' || *s == '\t') s++;
        if (!ident_ch((unsigned char)*s) || isdigit((unsigned char)*s)) continue;
        char name[MAX_NAME]; size_t k = 0;
        while (ident_ch((unsigned char)s[k]) && k + 1 < sizeof name) k++;
        memcpy(name, s, k); name[k] = 0;
        if (name[0] == '_') continue;                 /* 内部宏 */
        if (macro_seen(name)) continue;               /* 多头同名宏只生成一个常量 */
        const char *val = s + k;
        if (*val == '(') continue;                    /* 函数式宏 */
        char zval[256], zty[16] = "i32";
        const char *ty = NULL;
        if (!numeric_macro(val, zval, sizeof zval, &ty)) continue;
        snprintf(zty, sizeof zty, "%s", ty);
        /* 0x80000000 这类无符号宏转成十进制后落到 i32 会溢出（E0302）；
         * 值在 u32 范围内就改用 u32 —— C 里它本来就是 unsigned。 */
        if (!strcmp(zty, "i32")) {
            char *end = NULL;
            long long mv = strtoll(zval, &end, 0);   /* 认 0x / 十进制两种写法 */
            if (end && !*end && mv > 2147483647LL && mv <= 4294967295LL)
                snprintf(zty, sizeof zty, "u32");
        }
        sb_printf(consts, "pub const %s: %s = %s;\n", name, zty, zval);
    }
    free(text);
}

/* ---------------- 入口 ---------------- */

static int write_cmd_err(const char *path, char *err, size_t cap) {
    size_t n = 0;
    char *t = read_all(path, &n);
    if (!t || !n) return 0;
    /* 优先报带 error 的那行：zig cc 常先吐一条无关的 unused 警告 */
    const char *p = t, *best = NULL;
    while (p && *p) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l > 4) {
            char line[1024];
            if (l >= sizeof line) l = sizeof line - 1;
            memcpy(line, p, l); line[l] = 0;
            if (strstr(line, "error:") && (!best || strlen(line) < (size_t)600)) best = p;
        }
        p = e ? e + 1 : NULL;
    }
    if (!best) { best = t; while (*best == '\n' || *best == '\r') best++; }
    size_t k = 0;
    while (best[k] && best[k] != '\n' && best[k] != '\r' && k + 1 < cap) k++;
    memcpy(err, best, k); err[k] = 0;
    free(t);
    return 1;
}

/* zig cc 在 -fsyntax-only + -ast-dump 下仍会去 rename 一个并不存在的
 * .obj，报 "failed to rename ... FileNotFound" 并让退出码变成 1——但 AST
 * 其实已经完整写到 stdout 了。所以不能拿 system() 的返回码判成败：
 * 只认 AST 内容 + stderr 里除这条噪声之外的真编译错误。 */
static int clang_real_error(const char *errpath, char *detail, size_t cap) {
    size_t n = 0;
    char *t = read_all(errpath, &n);
    if (!t || !n) return 0;
    const char *p = t;
    while (p && *p) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        char line[1024];
        if (l >= sizeof line) l = sizeof line - 1;
        memcpy(line, p, l); line[l] = 0;
        if (strstr(line, "error:") && !strstr(line, "failed to rename")) {
            snprintf(detail, cap, "%s", line);
            free(t);
            return 1;
        }
        p = e ? e + 1 : NULL;
    }
    free(t);
    return 0;
}

/* 写进 .zx 文本的路径一律用正斜杠。Windows 的 `E:\code\lib\foo.h` 里
 * `\c` / `\z` / `\l` 会被 Zx 词法器当成非法字符串转义，生成出来的绑定
 * 反而编译不过（E0101）。正斜杠在 Windows 的路径 API 与 zig cc 里同样
 * 可用，所以这不是显示层的小事，是生成物可用性的硬要求。 */
static const char *zx_text_path(const char *src, char *buf, size_t cap) {
    if (!src) { buf[0] = 0; return buf; }
    size_t i = 0;
    for (; src[i] && i + 1 < cap; i++) {
        char c = src[i];
        buf[i] = (c == '\\') ? '/' : (c == '"') ? '\'' : c;
    }
    buf[i] = 0;
    return buf;
}

char *cbind_generate(const char *header, const char *cc, const char *link_lib,
                     int allow_system, const char *extra_incs,
                     const char *const *macro_headers, int nmacro,
                     const char *regen_arg,
                     char *err, size_t errcap, int *nskip) {
    if (errcap) err[0] = 0;
    g_ntags = 0; g_nskip = 0; g_nseen = 0; g_nmseen = 0;
    g_nopaque = 0; g_ndefrec = 0;
    g_basic_only = allow_system;
    memset(&g_skipped, 0, sizeof g_skipped);

    /* 包装头（配方走系统头时生成的 `#include <...>`）在文件头注释里
     * 显示真正被引用的头名，别让用户看到 zxc-include-wrap.h 这种内部文件。 */
    char shown[256];
    snprintf(shown, sizeof shown, "%s", path_base(header));
    {
        size_t hl = 0;
        char *ht = read_all(header, &hl);
        if (ht) {
            const char *inc = strstr(ht, "#include <");
            if (inc) {
                const char *a = inc + 10, *b = strchr(a, '>');
                if (b && b - a > 0 && b - a < 200)
                    snprintf(shown, sizeof shown, "%.*s", (int)(b - a), a);
            }
            free(ht);
        }
    }

    char dir[512];
    path_dir(header, dir, sizeof dir);

#ifdef _WIN32
    _mkdir(".zxc-cache");
#else
    mkdir(".zxc-cache", 0755);
#endif
    const char *astp = ".zxc-cache/zxc-add.ast.txt";
    const char *errp = ".zxc-cache/zxc-add.ast.err";
    const char *probec = ".zxc-cache/zxc-add-probe.c";

    /* zig cc 不能直接拿 .h 当输入：它会去找一个并不存在的 .obj 做
     * rename，然后报 FileNotFound。喂一个只含 #include 的 .c 包装，
     * 声明在 AST 里依然带着原始头文件的 loc，过滤逻辑不受影响。 */
    char probe_src[1200];
    snprintf(probe_src, sizeof probe_src, "#include \"%s\"\n", path_base(header));
    if (write_text(probec, probe_src) != 0) {
        snprintf(err, errcap, "cannot write %s", probec);
        return NULL;
    }
    char cmd[8192];
    snprintf(cmd, sizeof cmd,
        "%s -Xclang -ast-dump -fsyntax-only -fno-color-diagnostics "
        "-I\"%s\"%s \"%s\" > \"%s\" 2> \"%s\"",
        cc, dir, extra_incs ? extra_incs : "", probec, astp, errp);
    (void)system(cmd);

    char detail[512] = "";
    int bad = clang_real_error(errp, detail, sizeof detail);
    size_t alen = 0;
    char *ast = read_all(astp, &alen);
    if (!ast || !strstr(ast, "TranslationUnitDecl")) {
        bad = 1;
        if (!detail[0]) write_cmd_err(errp, detail, sizeof detail);
    }
    remove(astp); remove(errp); remove(probec);
    if (bad) {
        snprintf(err, errcap, "cannot parse C header '%s'%s%s",
                 header, detail[0] ? ": " : "", detail);
        free(ast);
        return NULL;
    }

    SB types, funcs, vars, consts;
    memset(&types, 0, sizeof types);
    memset(&funcs, 0, sizeof funcs);
    memset(&vars, 0, sizeof vars);
    memset(&consts, 0, sizeof consts);

    scan_ast(ast, 0, allow_system, &types, &funcs, &vars, &consts);
    scan_ast(ast, 1, allow_system, &types, &funcs, &vars, &consts);
    if (!allow_system) {
        if (nmacro > 0 && macro_headers)
            for (int i = 0; i < nmacro; i++) scan_macros(macro_headers[i], &consts);
        else
            scan_macros(header, &consts);
    }
    free(ast);

    SB out; memset(&out, 0, sizeof out);
    char ra_buf[1200], link_buf[1200];
    const char *ra = zx_text_path(
        (regen_arg && *regen_arg) ? regen_arg : shown, ra_buf, sizeof ra_buf);
    sb_printf(&out,
        "// Generated by `zxc include %s` from %s -- ordinary Zx source, edit freely.\n"
        "// Regenerate with `zxc include %s --force` (overwrites your edits).\n"
        "// C declarations translated by clang; unsupported ones are listed at the end.\n\n",
        ra, shown, ra);
    if (allow_system)
        sb_puts(&out, "// Toolchain header mode: function signatures only (no SDK structs/vars).\n"
                      "// For full bindings, put the library's .h into the project and add it by path.\n\n");
    if (link_lib && *link_lib)
        sb_printf(&out, "@link(\"%s\");\n\n",
                  zx_text_path(link_lib, link_buf, sizeof link_buf));
    if (vars.n) { sb_addn(&out, vars.p, vars.n); sb_puts(&out, "\n"); }
    sb_puts(&out, "extern \"C\" {\n");
    /* 不完整句柄先落占位定义，函数签名里的 *GLFWwindow 等才解析得到。 */
    for (int i = 0; i < g_nopaque; i++) {
        if (rec_has(g_defrec, g_ndefrec, g_opaque[i])) continue;
        char pat[MAX_NAME + 32];
        snprintf(pat, sizeof pat, "pub const %s = ", g_opaque[i]);
        if (types.p && strstr(types.p, pat)) continue;
        sb_printf(&out, "    pub const %s = struct { _opaque: u8 };\n", g_opaque[i]);
    }
    if (types.n) sb_addn(&out, types.p, types.n);
    if (types.n && funcs.n) sb_puts(&out, "\n");
    if (funcs.n) sb_addn(&out, funcs.p, funcs.n);
    sb_puts(&out, "}\n");                 /* extern "C" 是声明块，末尾不带分号 */
    if (consts.n) {
        sb_puts(&out, "\n// C constants / enum values\n");
        sb_addn(&out, consts.p, consts.n);
    }
    if (g_skipped.n) {
        sb_printf(&out, "\n// %d declaration(s) skipped -- add them by hand if needed:\n", g_nskip);
        sb_addn(&out, g_skipped.p, g_skipped.n);
    }

    free(types.p); free(funcs.p); free(vars.p); free(consts.p); free(g_skipped.p);
    if (nskip) *nskip = g_nskip;
    return out.p;
}
