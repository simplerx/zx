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

/* 顺序 = 码的顺序，方便和 zxc.c 的 g_rules 对照。 */
static const ErrDoc g_errs[] = {
    /* ---- 词法 ---- */
    { "E0100", "词法错误（未归类）",
      "看报错指出的字符；多半是写法不被词法器接受" },
    { "E0101", "字面量或注释没有闭合",
      "补上配对的 `\"` / `'` / 注释结束符，或检查转义`\\\"`" },
    { "E0102", "非法字符或非法字面量",
      "字符串里不该出现裸换行；`\\x` 转义要两位十六进制；标识符只允许 ASCII" },

    /* ---- 语法 ---- */
    { "E0200", "语法错误（未归类）",
      "从报错位置往前找最近一个没写完的结构" },
    { "E0201", "期待某个记号——结构没写完",
      "报错说 `expected ';'` 之类：把缺的那个符号补上；声明结尾都要 `;`" },
    { "E0202", "记号出现在不允许的位置",
      "常见于顶层多写了语句、表达式里混进声明、switch 臂没收尾" },

    /* ---- 类型 ---- */
    { "E0300", "类型错误（未归类）",
      "看指位处表达式的类型与期望类型" },
    { "E0301", "类型不匹配，且不能隐式转换",
      "加显式转换 `@intCast` / `@floatCast`，或改一边的类型；算术两边要同型" },
    { "E0302", "字面量超出目标类型范围",
      "换更宽的类型（`i32`→`i64`），或确认字面量本身没写错" },
    { "E0303", "参数个数或类型对不上",
      "数实参个数，检查有没有漏传/多传；默认值不支持，缺的显式给" },
    { "E0304", "类型没有这个成员、字段或变体",
      "查拼写（看 `did you mean` 建议）；成员存在但没 `pub` 时报的是 E0404" },
    { "E0305", "类型形状不合法",
      "多级指针要写全 `* *T`；`?T` 不能套 `?`；`?*T` 才是可空指针" },
    { "E0306", "运算符或操作数类型不匹配",
      "算术只接受数字；指针不能相加；切片拼接要求元素同型" },
    { "E0307", "联合体字面量必须指定变体",
      "写 `U{ .circle = 1.0 }`，不能写 `U{}`——联合体没有\"空\"状态" },

    /* ---- 名字与可见性 ---- */
    { "E0401", "未知的标识符或类型",
      "查拼写与作用域；跨模块要 `mod.name` 且对方 `pub`" },
    { "E0402", "重复声明",
      "同名声明只能有一处；`note` 行会指出前一处的位置" },
    { "E0403", "不可赋值 / 不可取地址 / 只读",
      "`let`/`const` 绑定不能改；`&` 只能取左值的地址" },
    { "E0404", "私有成员，或声明位置不合法",
      "在声明所在模块加 `pub`；`extern` 一类只能写在文件作用域" },
    { "E0405", "控制流语句放错位置",
      "`continue` 要在循环里，`return` 要在函数里，`defer` 里不能 `return`" },
    { "E0410", "未使用的局部变量、参数或绑定",
      "不用就写成 `_x` 或 `_`；准备以后用就先删掉" },

    /* ---- 编译期与语言规则 ---- */
    { "E0501", "这个值必须能在编译期算出来",
      "`const` 初值、数组长度、泛型实参都得是编译期常量；改 `let` 或换成字面量" },
    { "E0502", "控制流完整性不成立",
      "有返回值的函数每条路径都要 `return`；`noreturn` 函数不能返回" },
    { "E0503", "指针逃逸出当前栈帧",
      "别返回指向局部变量的指针/切片；要活得久就用 `std.heapAlloc` 分配" },
    { "E0504", "switch 没有穷尽",
      "枚举/联合体/`?T`/`!T`/bool 要列全，或加 `else` 兜底" },
    { "E0505", "编译期除以零",
      "除数在编译期为 0；运行时才会为 0 的，用变量算别用常量" },
    { "E0506", "对编译期就是 null 的值做 `.?` 解包",
      "先判空再取；或改用 `orelse` 给默认值" },
    { "E0507", "`@compileError` 触发 / `@trap` 在编译期执行到",
      "这是主动报错：看它给的文字；泛型里说明这组实参不被支持" },
    { "E0508", "错误码用了保留值",
      "`0` 表示\"没有错误\"，`65535` 留给 `error.null`——换个数" },

    /* ---- 模块 ---- */
    { "E0601", "模块解析失败",
      "查 `module(\"名字\")` 的路径/拼写；循环依赖要去掉一条边；`std` 要能随编译器找到" },

    /* ---- 泛型 ---- */
    { "E0701", "泛型实例化失败",
      "实参类型不满足函数体的要求；看后面跟的具体诊断" },

    /* ---- 内建与格式串 ---- */
    { "E0802", "格式串与参数不匹配",
      "数 `{}` 的个数与实参个数；`{[1]:>8.2}` 里的索引不能越界" },

    /* ---- 错误集与传播 ---- */
    { "E0900", "内部错误（未归类）",
      "多半是编译器的锅：把最小复现贴到 issue" },
    { "E0901", "返回的错误码不在函数声明的错误集里",
      "按 `help` 行把缺的码加进返回类型：`!{a, b}T`，或在这里处理掉" },
    { "E0902", "`try` 可能传播的错误码不在声明的错误集里",
      "同上：加宽返回类型，或在 `try` 前用 `catch` 处理" },
    { "E0903", "`try` 传播任意错误集，但函数声明了窄集合",
      "把返回类型写成 `!T`（不加集合），或逐个处理错误" },
};

#define NERRS ((int)(sizeof g_errs / sizeof g_errs[0]))

/* 错误码说明文档的对外地址。每个 `### E0xxx` 标题前都有显式锚点
 * `<a id="e0304"></a>`，所以拼上 `#小写码` 就能直接跳到那一条。
 *
 * 这份地址在 tools/zxls.zig 里还有一份（LSP 的 codeDescription 只能给
 * URL，拿不到这里的 C 常量）。test.sh 会校验两处字面量一致——改地址
 * 记得两边一起改，否则编辑器里的链接会指向一个旧位置。 */
#define ZX_DOCS_URL "https://gitcode.com/simplex/zx/blob/main/docs/errors.md"

const char *errors_doc_url(void) { return ZX_DOCS_URL; }

/* 码前两位 → 类别名。表里没出现的类别就归"其它"。 */
static const char *category(const char *code) {
    if (!code || strlen(code) < 4) return "其它";
    if (!strncmp(code, "E01", 3)) return "词法";
    if (!strncmp(code, "E02", 3)) return "语法";
    if (!strncmp(code, "E03", 3)) return "类型";
    if (!strncmp(code, "E04", 3)) return "名字与可见性";
    if (!strncmp(code, "E05", 3)) return "编译期与语言规则";
    if (!strncmp(code, "E06", 3)) return "模块";
    if (!strncmp(code, "E07", 3)) return "泛型";
    if (!strncmp(code, "E08", 3)) return "内建与格式串";
    if (!strncmp(code, "E09", 3)) return "错误集与传播";
    return "其它";
}

void errors_print_all(FILE *out) {
    fprintf(out, "错误码一览（共 %d 个）\n", NERRS);
    const char *cur = NULL;
    for (int i = 0; i < NERRS; i++) {
        const char *cat = category(g_errs[i].code);
        if (!cur || strcmp(cur, cat)) {
            fprintf(out, "\n%s\n", cat);
            cur = cat;
        }
        fprintf(out, "  %s  %s\n", g_errs[i].code, g_errs[i].title);
    }
    fprintf(out, "\n用 `zxc explain <CODE>` 看单条：什么意思、怎么修。\n");
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
        fprintf(out, "%s — %s\n\n", g_errs[i].code, g_errs[i].title);
        fprintf(out, "  修法：%s\n", g_errs[i].fix);
        fprintf(out, "  类别：%s\n", category(g_errs[i].code));
        /* 锚点用小写码，与 docs/errors.md 里的 `<a id="e0304">` 对齐 */
        fprintf(out, "  详解：%s#", ZX_DOCS_URL);
        for (size_t k = 0; k < n; k++) fputc((char)(want[k] >= 'A' && want[k] <= 'Z' ? want[k] - 'A' + 'a' : want[k]), out);
        fputc('\n', out);
        return 0;
    }
    fprintf(out, "没有 %s 这个错误码。\n用 `zxc errors` 看全部 %d 个。\n", code, NERRS);
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
