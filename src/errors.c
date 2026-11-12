/* errors.c — 错误码目录。
 *
 *   zxc errors            按类别列出全部错误码
 *   zxc explain E0304     单条详情：什么意思、怎么修
 *
 * 码的**唯一真源**是 zxc.c 里的 g_rules（消息片段 → 码），这里只放
 * 面向用户的文案：一条标题 + 一条修法。新增码时两边一起加——
 * tests/good 里有一条用例钉住"码表里的码这里都有"，漏了会红。
 *
 * 为什么值得有：诊断只给 `error[E0304]`，用户拿这个码搜不到任何东西；
 * 而错误码一旦发布就不能改（见 g_rules 上方注释），它就是对外契约，
 * 契约得有说明书。
 */
#include "zxc.h"

typedef struct {
    const char *code;
    const char *title;   /* 一句话：这个码是什么意思 */
    const char *fix;     /* 一句话：通常怎么改 */
} ErrDoc;

/* 顺序 = 码的顺序，方便和 zxc.c 的 g_rules 对照。
 * 文案一律 ASCII 英文：诊断要走 stderr，Windows 控制台代码页（GBK/CP936）
 * 下 UTF-8 中文会渲染成乱码（“鈥?”），不再放中文。 */
static const ErrDoc g_errs[] = {
    /* ---- 词法 ---- */
    { "E0101", "unterminated literal or comment",
      "add the matching `\"` / `'` / comment terminator, or check the `\\\"` escape" },
    { "E0102", "illegal character or literal",
      "no raw newlines inside strings; `\\x` escapes need two hex digits; identifiers are ASCII only" },

    /* ---- 语法 ---- */
    { "E0200", "syntax error (uncategorized)",
      "scan back from the error position for the nearest unfinished construct" },
    { "E0201", "expected a token -- construct not finished",
      "the error says `expected ';'` and so on: add the missing token; declarations end with `;`" },
    { "E0202", "token in a disallowed position",
      "usually a statement at top level, a declaration inside an expression, or an unterminated switch arm" },

    /* ---- 类型 ---- */
    { "E0211", "builtin removed (1.2.2 trimmed the set to seven)",
      "write conversions as `(T)value`; reflection is the single `@type(x)` "
      "(`.size` / `.align` / `.name` / `.fields`); `zxc explain E0211` lists "
      "which names went away and what to write instead" },

    { "E0300", "type error (uncategorized)",
      "compare the expression's type at the caret with the expected type" },
    { "E0301", "type mismatch, no implicit conversion",
      "write the conversion explicitly as `(T)expr` -- that is the only conversion "
      "syntax (the @intCast/@as family is gone); both sides of arithmetic and "
      "assignment must have the same type" },
    { "E0302", "literal out of range for the target type",
      "use a wider type (`i32` -> `i64`), or double-check the literal itself" },
    { "E0303", "argument count or type mismatch",
      "count the arguments; defaults are not supported, pass missing ones explicitly" },
    { "E0304", "type has no such member, field or variant",
      "check spelling (see the `did you mean` hint); for a private member use E0404 instead" },
    { "E0305", "illegal type shape",
      "write multi-level pointers in full `* *T`; `?T` cannot nest `?`; `?*T` is the nullable pointer" },
    { "E0306", "operator or operand type mismatch",
      "arithmetic takes numbers only; pointers cannot be added; slice concat needs matching element types" },
    { "E0307", "aggregate literal is malformed",
      "a union names exactly one variant (`U{ .circle = 1.0 }`, never `U{}`); "
      "no field may be initialized twice" },

    { "E0308", "invalid return type",
      "a bare `error` (or a bare `error{a, b}`) says nothing about whether the "
      "function can fail: return `error.x` from a `!T` function, declare "
      "`error{a, b}!void`, or use `!void`" },

    /* ---- 名字与可见性 ---- */
    { "E0401", "unknown identifier or type",
      "check spelling and scope; across modules use `mod.name` and the target must be `pub`" },
    { "E0402", "duplicate declaration",
      "only one declaration per name; the `note` line points at the previous one" },
    { "E0403", "not assignable / not addressable / read-only",
      "`let`/`const` bindings cannot be reassigned; `&` needs something that has "
      "storage of its own (a temporary has none)" },
    { "E0404", "private member, or declaration not allowed here",
      "add `pub` in the declaring module; `extern` and friends live at file scope only" },
    { "E0405", "control-flow statement in the wrong place",
      "`continue` belongs in a loop, `return` in a function, no `return` inside "
      "`defer`, and `errdefer` only in a function that returns `!T`; loops have "
      "no `else` branch -- write that code after the loop" },
    { "E0410", "unused local variable, parameter or binding",
      "rename unused ones to `_x` or `_`; otherwise delete them until needed" },
    { "E0411", "return value discarded without `_`",
      "every value must be used; write `_ = f(...)` to drop it on purpose" },

    /* ---- 编译期与语言规则 ---- */
    { "E0501", "value must be computable at compile time",
      "`const` initializers, array lengths and generic arguments are compile-time constants; use `let` or a literal" },
    { "E0502", "control flow integrity violated",
      "every path of a function with a return value must `return`; `noreturn` functions cannot return" },
    { "E0503", "pointer escapes the current stack frame",
      "do not return pointers/slices to locals; use `std.heapAlloc` for longer lifetimes" },
    { "E0504", "switch arms do not form a valid partition of the subject",
      "cover every case of the enum/union/`?T`/`!T`/bool, or add an `else` arm; "
      "each pattern (and `else`) may appear only once" },
    { "E0505", "division by zero at compile time",
      "the divisor is 0 at compile time; compute runtime values through variables instead of constants" },
    { "E0506", "`.?` unwrap on a value that is null at compile time",
      "check for null first, or use `orelse` to provide a default" },
    { "E0507", "`@compileError` triggered / `@trap` reached at compile time",
      "this is a deliberate error: read its message; inside a generic it means these arguments are unsupported" },
    { "E0508", "error code uses a reserved value",
      "`0` means \"no error\", `65535` belongs to `error.null` -- pick another number, and do not name a member `null` (that code is fixed)" },
    { "E0509", "the same allocation was freed twice",
      "each allocation has exactly one release event; delete the second `free`, or move ownership explicitly (the compile-time ledger only tracks straight-line local paths)" },
    { "E0510", "forced unwrap on an invalid handle",
      "`get(h)` returns `null` after `free(h)` / `deinit()`; use `if (t.get(h)) p { ... }` to check validity instead of `.?`" },

    /* ---- 模块 ---- */
    { "E0601", "module resolution failed",
      "check the path/spelling of `module(\"name\")`; break circular deps; `std` must resolve next to the compiler" },

    /* ---- 泛型 ---- */
    { "E0701", "generic instantiation failed",
      "the argument types do not satisfy the function body; see the follow-up diagnostics" },

    /* ---- 内建与格式串 ---- */
    { "E0802", "format string does not match arguments",
      "count `{}` placeholders vs arguments; the index in `{[1]:>8.2}` must be in range" },

    /* ---- 错误集与传播 ---- */
    { "E0900", "internal error (uncategorized)",
      "most likely a compiler bug: please file a minimal reproduction" },
    { "E0901", "returned error code is not in the function's error set",
      "add the missing code to the return type per the `help` line: `!{a, b}T`, or handle it here" },
    { "E0902", "`try` may propagate codes outside the declared error set",
      "same as above: widen the return type, or `catch` before the `try`" },
    { "E0903", "`try` propagates any error set but the function declares a narrow one",
      "declare the return type as `!T` (no set), or handle the errors one by one" },
};

#define NERRS ((int)(sizeof g_errs / sizeof g_errs[0]))

/* 错误码说明文档的对外地址（`<base>#e0304` 直达一条）。每个 `### E0xxx`
 * 标题前都有显式锚点 `<a id="e0304"></a>`，拼上小写码就能跳过去。
 *
 * 这份地址在 tools/zxls.zig 里还有一份（LSP 的 codeDescription 只能给
 * URL，拿不到这里的 C 常量）。test.sh 会校验两处字面量一致——改地址
 * 记得两边一起改，否则编辑器里的链接会指向一个旧位置。
 *
 * 注意：删掉的是**手册与离线文档站**（std 还没定型，写出来就是错的），
 * 错误码参考（docs/errors.md）保留——诊断里的链接要能点开。 */
#define ZX_DOCS_URL "https://gitcode.com/simplex/zx/blob/main/docs/errors.md"

const char *errors_doc_url(void) { return ZX_DOCS_URL; }

/* 码前两位 → 类别名。表里没出现的类别就归"其它"。 */
static const char *category(const char *code) {
    if (!code || strlen(code) < 4) return "other";
    if (!strncmp(code, "E01", 3)) return "lexical";
    if (!strncmp(code, "E02", 3)) return "syntax";
    if (!strncmp(code, "E03", 3)) return "types";
    if (!strncmp(code, "E04", 3)) return "names & visibility";
    if (!strncmp(code, "E05", 3)) return "compile-time & language rules";
    if (!strncmp(code, "E06", 3)) return "modules";
    if (!strncmp(code, "E07", 3)) return "generics";
    if (!strncmp(code, "E08", 3)) return "builtins & format strings";
    if (!strncmp(code, "E09", 3)) return "error sets & propagation";
    return "other";
}

void errors_print_all(FILE *out) {
    fprintf(out, "error codes (%d total)\n", NERRS);
    const char *cur = NULL;
    for (int i = 0; i < NERRS; i++) {
        const char *cat = category(g_errs[i].code);
        if (!cur || strcmp(cur, cat)) {
            fprintf(out, "\n%s\n", cat);
            cur = cat;
        }
        fprintf(out, "  %s  %s\n", g_errs[i].code, g_errs[i].title);
    }
    fprintf(out, "\nuse `zxc explain <CODE>` for details (meaning and fix).\n");
}

/* 0 = 找到了并打印；1 = 没有这个码 */
int errors_print_one(FILE *out, const char *code) {
    /* 大小写不敏感：`zxc explain e0304` 也该能用 */
    char want[16];
    size_t n = strlen(code);
    if (n >= sizeof want) n = sizeof want - 1;
    for (size_t i = 0; i < n; i++) {
        char c = code[i];
        want[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    want[n] = '\0';

    for (int i = 0; i < NERRS; i++) {
        if (strcmp(g_errs[i].code, want)) continue;
        fprintf(out, "%s -- %s\n\n", g_errs[i].code, g_errs[i].title);
        fprintf(out, "  fix: %s\n", g_errs[i].fix);
        fprintf(out, "  category: %s\n", category(g_errs[i].code));
        /* 锚点用小写码，与 docs/errors.md 里的 `<a id="e0304">` 对齐 */
        fprintf(out, "  details: %s#", ZX_DOCS_URL);
        for (size_t k = 0; k < n; k++)
            fputc((char)(want[k] >= 'A' && want[k] <= 'Z' ? want[k] - 'A' + 'a' : want[k]), out);
        fputc('\n', out);
        return 0;
    }
    fprintf(out, "no such error code: %s\nuse `zxc errors` to list all %d.\n", code, NERRS);
    return 1;
}

/* 码 → 通用修法（大小写不敏感）。诊断渲染器在没有具体建议时拿它兜底；
 * 表里没有这个码就返回 NULL，调用点静默跳过。 */
const char *errors_fix(const char *code) {
    if (!code || !*code) return NULL;
    char want[16];
    size_t n = strlen(code);
    if (n >= sizeof want) n = sizeof want - 1;
    for (size_t i = 0; i < n; i++) {
        char c = code[i];
        want[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    want[n] = '\0';
    for (int i = 0; i < NERRS; i++)
        if (!strcmp(g_errs[i].code, want)) return g_errs[i].fix;
    return NULL;
}

int errors_count(void) { return NERRS; }

const char *errors_code_at(int i) {
    return (i >= 0 && i < NERRS) ? g_errs[i].code : NULL;
}
