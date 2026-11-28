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
    { "E0211", "removed spelling (old builtin or removed syntax)",
      "1.6.2 removed `@shim` (compile C via `zxc include` + `extern \"C\" fn` "
      "+ @link), `struct(packed)` (plain or `extern struct` only) and array/"
      "slice `.N` dot-indexing (write `a[i]`; dot-index stays only for tuples "
      "and vectors). Older removals: write conversions as `(T)value`; "
      "reflection is the single `@type(x)` (`.size` / `.align` / `.name` / "
      "`.fields`); vector math lives in `std.simd`. §222 also removed the C-style "
      "boolean spellings `&&` and `||` -- write `and` / `or` (same short-circuit "
      "behavior and precedence). §225 removed `tag.close;` -- the tag ledger is "
      "off by default now, write `tag.enable;` to turn it on for a module. "
      "`zxc explain E0211` lists which names went away "
      "and what to write instead" },

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
      "write multi-level pointers in full `* *T`; `?T` cannot nest `?`; `?*T` is "
      "the nullable pointer; a `@Vec(T, N)` length is 1/2/4/8/16/32/64 only" },
    { "E0306", "operator or operand type mismatch",
      "arithmetic takes numbers only; pointers cannot be added; slice concat needs matching element types" },
    { "E0307", "aggregate literal is malformed",
      "a union names exactly one variant (`U{ .circle = 1.0 }`, never `U{}`); "
      "no field may be initialized twice" },

    { "E0308", "invalid return type",
      "a bare `error` (or a bare `error{a, b}`) says nothing about whether the "
      "function can fail: return `error.x` from a `!T` function, declare "
      "`error{a, b}!void`, or use `!void`" },
    { "E0309", "invalid entry signature",
      "main takes no parameters -- read command-line arguments with "
      "std.process.args() instead (argv[0] is the program name and is "
      "excluded from the list)" },

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
      "`defer`, and `defer err` only in a function that returns `!T`; loops have "
      "no `else` branch -- write that code after the loop" },
    { "E0406", "nested function captures an outer local (no closures)",
      "Zx has no closures: a nested function sees only its own parameters and "
      "locals; pass the outer value in as a parameter instead" },
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
    { "E0507", "`@error` triggered / `@trap` reached at compile time",
      "this is a deliberate error: read its message; inside a generic it means these arguments are unsupported" },
    { "E0508", "error code uses a reserved value",
      "`0` means \"no error\", `65535` belongs to `error.null` -- pick another number, and do not name a member `null` (that code is fixed)" },
    { "E0509", "the same allocation was freed twice",
      "each allocation has exactly one release event; delete the second `free`, or move ownership explicitly (the compile-time ledger only tracks straight-line local paths)" },
    { "E0510", "read of a handle that is already dead at compile time",
      "`free(h)` / `deinit()` end a handle's life; the compile-time ledger knows it, and there is no runtime table to re-check -- stop reading it, or drop the `deinit`" },
    { "E0511", "cannot prove that a handle is alive",
      "handle checks are compile-time only (no runtime side table): keep `create`/`get`/`free` in straight-line code, bind the handle to a local, and do not store it in a struct, pass it through a cast, or issue it inside control flow" },

    /* ---- tag 指针生命周期账本（§170） ---- */
    { "E0512", "use after free of a tracked pointer",
      "free must come before the last use -- a freed pointer must not be dereferenced, and re-binding it (`tag.x : p`) is also use after free" },
    { "E0513", "an allocation was never freed",
      "alloc and free must come in pairs -- free the pointer before the program ends, or mark it released (`tag p : null;`)" },
    { "E0514", "tracked pointer provenance lost",
      "a tracked pointer cannot be cast to a non-pointer or stay behind control flow; a branch that frees must yield null as its value -- across functions, pointers travel as borrowed parameters or through returns" },
    { "E0515", "it is undetermined whether the memory was freed -- defend first",
      "branches disagree on whether it was freed; re-bind it (`p = tag.x : p;`) to assert it is live, or `free` it to end it, before any other use" },
    { "E0516", "an allocation inside a loop is not balanced",
      "allocate and free within the same iteration, or move the allocation outside the loop; a loop cannot yield a tracked pointer" },
    { "E0520", "the label is not a tag value",
      "`x : expr` labels need `const x = tag.x;` first (or write `tag.x : expr` directly)" },
    { "E0518", "the label already owns an allocation",
      "a label is single-instance: free the current allocation or use a second label" },
    { "E0519", "manual tag declaration removed -- tags self-declare on first use",
      "a tag slot is created automatically the first time you reference it (`tag.x : expr` or `const h = tag.x;`); there is no declaration statement any more -- remove the line" },
    { "E0521", "a tracked pointer cannot be tracked through a runtime-indexed element",
      "which allocation an element holds cannot be identified at compile time; store at a constant index, continue the chain (`tag v : tag.x = xs[i];`), or mark it released (`tag p : null;`)" },
    { "E0522", "the value already carries a label",
      "a re-bind (`tag v : tag.x = lost;`) is only for lost pointers; free the old allocation first, or use `let p = tag.x : expr;` for a fresh allocation" },
    { "E0523", "@tag: the pointer carries no label",
      "@tag(p) returns the label a pointer was bound with (`tag.x : expr`, or a re-bound carrier); bind this value to a label first, or pass a pointer that already carries one" },
    { "E0524", "the label is not registered in this array's group",
      "an annotated read (`tag v : tag.x = xs[i];`) takes a label out of the array's group -- only labels stored into this array qualify, and each only once; it was never stored, already taken out, or the store went through an alias the compiler cannot prove" },
    { "E0525", "integer-to-pointer cast needs a label",
      "this integer came from a pointer and its provenance was lost (arithmetic or re-assignment) -- re-attach the tracking chain with `tag v : tag.x = (*T)n;`; integers that never touched a pointer need no annotation" },
    { "E0526", "the pointer is owned by an arena",
      "this pointer was adopted by an arena (`ArenaAllocator.adopt`) -- the arena frees it at deinit; free it here and the arena would free it again. To take ownership back is not supported: design the hand-over one-way" },
    { "E0528", "release in a branch arm must be confirmed",
      "free/deinit inside `if`/`switch`/`try` arms needs explicit confirmation: write `tag p : null;` (p = the carrier variable) right after the release to confirm the pointer is given up -- a branch may not run, so an unconfirmed release is treated as not-released at the join and the pointer stays usable; loop bodies are not arms, per-iteration release there is fine" },
    { "E0529", "use after move",
      "`std.move(p)` transferred the pointer's ownership to the receiver (line given in the message) -- the original variable is sealed: it can be neither dereferenced, freed, moved again, nor handed off again. Use the variable the move produced; to keep tracking on the receiving side, bind a tag there (`tag q : tag.x = ...`)" },
    { "E0530", "std.move needs a pointer the tag ledger tracks",
      "`std.move` transfers ownership, so the argument must be an allocation the compiler tracks -- a `std.heapAlloc`/`std.stackAlloc` site or a pointer bound with `tag.x : ...`. A bare pointer (e.g. cast back from `usize` in a thread) has no owner to transfer; bind a tag to it first" },
    { "E0532", "captured pointer escape or invalid capture",
      "`if (o) *t { }` / `while (o) *t { }` / `switch (s) { .tag: *t { } }` capture a POINTER to the matched payload so you can write back to the original object (`t[0] = value`). The condition/subject must be an addressable, mutable left-hand side (a `var`), because a temporary has no original object to write back to. The captured pointer is a stack borrow: it may be used inside the arm to read/write the payload and aliased to other locals, but it must never escape — not returned, not stored into a struct field, global, or array/slice element, and not passed as a function argument. Taking the address of an ordinary value capture also creates a capture-local borrow for argument checks; returning that address instead reports E0503 because it points into capture storage. Borrow provenance and this check are part of the tag ledger: `tag.enable` creates the borrow tag and enables the check; without it, no borrow tag or E0532 diagnostic is produced" },

    { "E0533", "optional dereferenced without unwrap",
      "unwrap `?T` / `!T` with `.?` first -- `.0` is only dereference for a bare pointer "
      "or a tuple component; use `x.?` / `if (x) v { ... }` / `orelse`" },

    /* ---- 模块 ---- */
    { "E0601", "module resolution failed",
      "check the path/spelling of `@mod(\"name\")`; circular module imports are allowed, but a cycle in top-level const/var VALUES needs an explicit initial value; `std` must resolve next to the compiler" },

    /* ---- 泛型 ---- */
    { "E0701", "generic instantiation failed",
      "the argument types do not satisfy the function body; see the follow-up diagnostics" },

    /* ---- 内建与格式串 ---- */
    { "E0802", "format string does not match arguments",
      "count `{}` placeholders vs arguments; the index in `{1:>8.2}` must be in range" },

    /* ---- 错误集与传播 ---- */
    { "E0900", "internal error (uncategorized)",
      "most likely a compiler bug: please file a minimal reproduction" },
    { "E0901", "returned error code is not in the function's error set",
      "add the missing code to the return type per the `help` line: `!{a, b}T`, or handle it here" },
    { "E0902", "`try` may propagate codes outside the declared error set",
      "same as above: widen the return type, or `catch` before the `try`" },
    { "E0903", "`try` propagates any error set but the function declares a narrow one",
      "declare the return type as `!T` (no set), or handle the errors one by one" },

    /* ---- 工具链与环境 ---- */
    { "E1001", "cannot launch the Zx frontend (zxfe)",
      "this is an environment fault, not a problem in your source: check that "
      "zxfe.exe sits next to zxc.exe, that its directory is writable, and that "
      "antivirus/sandbox software is not blocking unsigned executables" },
    { "E1002", "the Zx frontend (zxfe) exited abnormally without a diagnostic",
      "the frontend died (crash or non-zero exit) before it could report "
      "anything -- retry with `--fe=c` to narrow it down, and report the "
      "smallest reproducing file if it persists" },
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
