/**
 * tree-sitter-zx —— Zx 语言的 tree-sitter 语法。
 *
 * Zx：编译到 C11 的系统级语言，语法参考见 zx 仓库 docs/guide.md。
 * 本语法面向**着色与大纲**，不求类型级精确：表达式层用 GLR 宽容解析，
 * 靠 error recovery 兜底，保证半成品代码也能着色。
 *
 * 命名约定（guide §2）：类型 PascalCase、值 camelCase/snake_case，
 * 所以 `type_identifier` 直接用大写开头的 token。
 */
const PREC = {
  PAREN_DECL: -2,      // (T, U) 元组类型 vs 括号表达式
  CAST: -1,            // (T)e 与括号表达式的 GLR 冲突
  RANGE: 1,            // ..
  ORELSE: 2,           // orelse
  OR: 3,
  AND: 4,
  EQUAL: 5,            // == !=
  COMPARE: 6,          // < <= > >=
  SHIFT: 7,
  BIT_OR_XOR: 8,
  BIT_AND: 9,
  ADD: 10,
  MUL: 11,
  UNARY: 12,
  TAG: 13,             // 标签 `tag: expr` 收束最松
  CALL: 14,
};

module.exports = grammar({
  name: "zx",

  word: ($) => $.identifier,

  conflicts: ($) => [
    [$.expression_statement, $._arm_body, $.capture_expr],    [$._expression, $.capture_block, $.capture_expr, $.for_expression, $.container_literal],    [$._expression, $.capture_block, $.capture_expr, $.container_literal],    [$._expression, $.for_expression, $.container_literal],    [$._type, $._expression, $.container_literal],    [$.enum_variant, $._expression, $.container_literal],    [$._type, $.container_literal],    [$._expression, $.container_literal],    [$.container_literal, $.function_expression],
    [$.union_payload, $.payload_type],    [$.destructure_pattern, $._expression],    [$._type, $.qualified_type, $._postfix_base],    [$._type, $._postfix_base],    [$._expression, $.switch_arm, $._postfix_base],    [$.switch_arm, $._postfix_base],    [$._type, $.container_type],    [$.expression_statement, $._arm_body],
    [$.container_type],    [$.expression_statement],
    [$.expression_statement, $.initializer_list],    [$.for_expression, $._postfix_base],    [$._expression, $.for_expression, $._postfix_base],    [$._type, $.qualified_type, $._expression, $._postfix_base],    [$._type, $._expression, $._postfix_base],    [$.qualified_type, $._expression, $._postfix_base],    [$.enum_variant, $._expression, $._postfix_base],    [$.qualified_type, $._postfix_base],    [$._expression, $._postfix_base],    [$._statement, $._expression],
    [$._type, $._expression],

    [$._expression, $.if_expression],
    [$._expression, $.switch_arm],
    [$._expression, $.qualified_type],
    [$._type, $.qualified_type],
    [$._type, $.qualified_type, $._expression],
    [$.error_set_type],
    [$.union_payload, $.tuple_literal],
    [$._expression, $.enum_variant],

    [$._arm_body, $._expression],
    [$._arm_body],
    [$._expression, $._arm_body, $.capture_block],
    [$._expression, $.capture_block],
    [$._expression, $.capture_block, $.for_expression],
    [$._arm_body, $._statement],
    [$._statement, $._expression, $._arm_body],
    [$.container_literal, $._arm_body],
    [$._expression, $.for_expression],

    [$.block, $.initializer_list],
  ],

  extras: ($) => [/\s/, $.line_comment, $.block_comment],

  rules: {
    source_file: ($) => repeat($._statement),

    // ------------------------------------------------------------ 注释 --
    line_comment: ($) => token(seq("//", /[^\n]*/)),
    block_comment: ($) => token(seq("/*", /[^*]*\*+([^/*][^*]*\*+)*/, "/")),

    // -------------------------------------------------------- 标识符 --
    identifier: ($) => token(/[a-zA-Z_][A-Za-z0-9_]*/),
    // PascalCase —— Zx 的类型约定（guide §2）
    type_identifier: ($) => token(prec(1, /[A-Z][A-Za-z0-9_]*/)),
    field_identifier: ($) => $.identifier,

    // ---------------------------------------------------------- 字面量 --
    integer: ($) =>
      token(
        seq(
          choice(
            /0[xX][0-9a-fA-F_]+/,
            /0[bB][01_]+/,
            /0[oO][0-7_]+/,
            /[0-9][0-9_]*/,
          ),
          optional(/[uU][0-9]*|[iI][0-9]*|usize|isize|u8|u16|u32|u64|u128|i8|i16|i32|i64|i128/),
        ),
      ),
    float: ($) =>
      token(
        seq(
          choice(
            /[0-9][0-9_]*\.[0-9][0-9_]*/,
            /[0-9][0-9_]*[eE][+-]?[0-9]+/,
            /[0-9][0-9_]*\.[0-9][0-9_]*[eE][+-]?[0-9]+/,
          ),
          optional(/f16|f32|f64|f128|f/),
        ),
      ),
    boolean: ($) => choice("true", "false"),
    character: ($) =>
      seq(
        "'",
        optional(choice($.escape_sequence, /[^'\\]/)),
        "'",
      ),
    string: ($) =>
      seq(
        '"',
        repeat(choice($.escape_sequence, token.immediate(/[^"\\\n]+/))),
        token.immediate('"'),
      ),
    escape_sequence: ($) =>
      token(/\\(n|t|r|0|\\|"|'|a|b|f|v|e|x[0-9a-fA-F]{2}|u[0-9a-fA-F]{4})/),

    // ------------------------------------------------------------ 声明 --
    _statement: ($) =>
      choice(
        $.const_declaration,
        $.let_declaration,
        $.var_declaration,
        $.extern_declaration,
        $.assignment_statement,
        $.return_statement,
        $.continue_statement,
        $.defer_statement,
        $.errdefer_statement,
        $.expression_statement,
        $.block,
      ),

    const_declaration: ($) =>
      seq(
        optional("pub"),
        optional("inline"),
        "const",
        field("name", choice($.identifier, $.destructure_pattern)),
        optional($.type_annotation),
        "=",
        field("value", $._expression),
        ";",
      ),

    let_declaration: ($) =>
      seq(
        "let",
        field("name", $.identifier),
        optional($.type_annotation),
        "=",
        field("value", $._expression),
        ";",
      ),

    var_declaration: ($) =>
      seq(
        "var",
        field("name", $.identifier),
        optional($.type_annotation),
        optional(seq("=", field("value", $._expression))),
        ";",
      ),

    // const {a, b, c} = tup; —— 元组解构
    destructure_pattern: ($) =>
      seq("{", commaSep1(field("member", $.identifier)), optional(","), "}"),

    type_annotation: ($) => seq(":", field("type", $._type)),

    extern_declaration: ($) =>
      seq(
        optional("pub"),
        "extern",
        "fn",
        field("name", $.identifier),
        $.parameters,
        optional(field("return_type", $._type)),
        ";",
      ),

    assignment_statement: ($) =>
      seq(
        field("left", $._place),
        field("operator", choice("=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=")),
        field("right", $._expression),
        ";",
      ),

    expression_statement: ($) => choice(seq($._expression, optional(";")), ";"),

    // ------------------------------------------------------------ 函数 --
    // `const f = fn (…) T { … }` 的匿名函数体；具名声明也长这样（Zx 无 fn name 形式）
    function_expression: ($) =>
      prec.right(
        seq(
          optional("inline"),
          "fn",
          field("parameters", $.parameters),
          optional(field("return_type", $._type)),
          optional(field("body", $.block)),
        ),
      ),

    parameters: ($) =>
      seq(
        "(",
        optional(commaSep1($._parameter)),
        optional(","),
        ")",
      ),
    _parameter: ($) =>
      choice(
        seq(field("name", choice($.identifier, $.type_identifier)), ":", field("type", $._type)),
        field("type", $._type), // 匿名参数
      ),

    // ------------------------------------------------------------ 容器 --
    // struct / union / enum / error 的身体：字段 + 嵌套 const（方法）
    container_body: ($) =>
      seq(
        "{",
        repeat(
          choice(
            $.field_declaration,
            $.enum_variant,
            $.const_declaration,
            $.let_declaration,
            $.var_declaration,
            $.expression_statement,
            ",",
          ),
        ),
        "}",
      ),

    field_declaration: ($) =>
      prec.right(seq(
        field("name", $.identifier),
        ":",
        field("type", choice($._type, $.union_payload)),
        optional(seq("=", field("default", $._expression))),
        optional(","),
      )),

    // tagged union 载荷：`circle: {ShapeTag.circle, f64}`（首项是判别值表达式）
    union_payload: ($) =>
      seq("{", commaSep1(choice($._expression, $._type)), optional(","), "}"),

    // enum / error set 的变体：`red`、`red = 3,`
    enum_variant: ($) =>
      prec.right(seq(field("name", $.identifier), optional(seq("=", field("value", $._expression))), optional(","))),

    // ------------------------------------------------------------ 类型 --
    _type: ($) =>
      choice(
        $.type_identifier,
        $.primitive_type,
        $.pointer_type,
        $.optional_type,
        $.error_type,
        $.slice_type,
        $.array_type,
        $.tuple_type,
        $.container_type,
        $.error_set_type,
        $.qualified_type,
        $.const_type,
        $.call_expression,
        $.payload_type,
        $.function_expression,
      ),

    primitive_type: ($) =>
      token(
        prec(1, choice(
          "bool", "void",
          "u8", "u16", "u32", "u64", "u128",
          "i8", "i16", "i32", "i64", "i128",
          "usize", "isize",
          "f16", "f32", "f64", "f128",
          "anyopaque", "noreturn", "type",
        )),
      ),

    pointer_type: ($) => seq("*", $._type),

    // struct { … } / union { … } / enum { … }
    container_type: ($) =>
      seq(
        choice("struct", "union", "enum"),
        optional(seq("(", field("backing", choice($._type, $.enum_tag)), ")")),
        field("body", $.container_body),
      ),
    // error { neg, div0 } / 裸 error（cast `(error)…` 用）
    error_set_type: ($) => seq("error", optional(field("body", $.container_body))),
    // union(enum) / union(enum(u8)) 的标签简写（内层 enum 无身体）
    enum_tag: ($) => seq("enum", optional(seq("(", $._type, ")"))),
    // std.Pool 这类跨模块类型
    qualified_type: ($) =>
      seq(
        choice($.identifier, $.type_identifier),
        ".",
        repeat(seq($.identifier, ".")),
        field("name", $.type_identifier),
      ),
    optional_type: ($) => seq("?", $._type),
    error_type: ($) => seq("!", $._type),
    slice_type: ($) => seq("[", "]", $._type),
    array_type: ($) => seq("[", field("size", $._expression), "]", $._type),
    tuple_type: ($) => seq("(", commaSep1($._type), optional(","), ")"),
    // []const u8 —— const 限定
    const_type: ($) => seq("const", $._type),
    // 花括号元组类型：?{usize, usize}、{i32, f64}
    payload_type: ($) => seq("{", commaSep1($._type), optional(","), "}"),

    // ------------------------------------------------------------ 表达式 --
    _expression: ($) =>
      choice(
        $.identifier,
        $.type_identifier,
        $.integer,
        $.float,
        $.boolean,
        $.character,
        $.string,
        $.null_literal,
        $.undefined_literal,
        $.builtin_function,
        $.module_expression,
        $.parenthesized_expression,
        $.cast_expression,
        $.labeled_expression,
        $.block,
        $.if_expression,
        $.while_expression,
        $.for_expression,
        $.switch_expression,
        $.function_expression,
        $.container_literal,
        $.anonymous_container,
        $.tuple_literal,
        $.call_expression,
        $.field_expression,
        $.index_expression,
        $.unwrap_expression,
        $.slice_expression,
        $.unary_expression,
        $.binary_expression,
        $.range_expression,
        $.try_expression,
        $.container_type,
        $.error_set_type,
      ),

    null_literal: ($) => "null",
    undefined_literal: ($) => "undefined",

    module_expression: ($) =>
      seq("module", "(", field("path", choice($.string, $.identifier)), ")"),

    parenthesized_expression: ($) => seq("(", $._expression, ")"),

    // (T)e 显式转换（guide §5.3）
    cast_expression: ($) =>
      prec(PREC.CAST, seq("(", field("type", $._type), ")", field("value", $._expression))),

    // 标签：`tag: { … }` 打标签块 / `tag: v;` 收割（guide §7.5）
    labeled_expression: ($) =>
      prec(PREC.TAG, seq(field("label", $.identifier), ":", $._expression)),

    block: ($) => seq("{", repeat($._statement), "}"),

    if_expression: ($) =>
      prec.right(
        seq(
          "if",
          "(",
          field("condition", $._expression),
          ")",
          field("consequence", $._arm_body),
          optional(seq("else", field("alternative", choice($.if_expression, $._arm_body)))),
        ),
      ),
    // 臂：块 / 捕获绑定 `if (maybe) v { … }` / 单语句 / 值臂（裸表达式，低优先级：
    // `let x = if (c) a else b;` 里值臂不吃分号，语句臂 `use(n);` 才带）
    _arm_body: ($) =>
      choice($.block, $.capture_block, $.capture_expr, $._statement, prec(-1, $._expression)),
    // 块体捕获：`if (maybe) v { … }` —— 高优先级，见到就是它
    capture_block: ($) =>
      prec(
        PREC.CALL + 1,
        seq(field("binding", $.identifier), field("body", $.block)),
      ),
    // 表达式体捕获：`return if (o) v v else 0;` —— 只在裸臂活不下去时兜底
    // （`if (c) std.println(...)` 里 capture(std)+`.println` 不成句，
    //   静态优先级太高会把无错的裸臂路径压死）
    capture_expr: ($) =>
      prec(-2, seq(field("binding", $.identifier), field("body", $._expression))),

    while_expression: ($) =>
      prec.right(
        seq(
        "while",
        "(",
        field("condition", $._expression),
        ")",
        // 身体即臂体：`while (it.next()) v { … }` 的 v 是捕获绑定、块是身体
        field("body", $._arm_body),
        optional(seq("else", field("alternative", $._arm_body))),
      ),
      ),

    // for (xs, 0..n) x, i { … } else { … }
    for_expression: ($) =>
      prec.right(
      seq(
        optional("inline"),
        "for",
        "(",
        commaSep1(field("iterator", $._expression)),
        ")",
        optional(commaSep1(field("binding", choice($.identifier, $.type_identifier)))),
        field("body", $._arm_body),
        optional(seq("else", field("alternative", $._arm_body))),
      ),
      ),

    switch_expression: ($) =>
      seq(
        "switch",
        "(",
        field("subject", $._expression),
        ")",
        "{",
        repeat(choice($.switch_arm, ",")),
        "}",
      ),

    // 臂：`.circle a: { … }`（模式 + 捕获）、`'0', '1': { … }`、`else: v`
    switch_arm: ($) =>
      prec.right(
        seq(
          commaSep1($._pattern),
          optional(field("binding_pre", $.identifier)),
          ":",
          optional(field("binding", $.identifier)),
          field("value", choice($._expression, $.block)),
          optional(","),
        ),
      ),
    _pattern: ($) =>
      choice(
        $.enum_pattern,
        $.integer,
        $.float,
        $.character,
        $.string,
        $.boolean,
        $.identifier, // else / 具名常量
        $.cast_expression, // (error)Err.notFound
        seq("-", choice($.integer, $.float)),
      ),
    enum_pattern: ($) => seq(".", field("member", $.identifier)),

    // 花括号元组字面量：const tup = {1, "two", 3.5};（比块优先）
    tuple_literal: ($) =>
      prec(PREC.CALL + 1, seq("{", commaSep1($._expression), optional(","), "}")),

    container_literal: ($) =>
      seq(
        field("type", choice($.type_identifier, $.identifier, $.call_expression, $.field_expression)),
        field("body", $.initializer_list),
      ),
    anonymous_container: ($) => seq(".", field("body", $.initializer_list)),

    initializer_list: ($) =>
      seq(
        "{",
        repeat(
          choice(
            $.field_initializer,
            $._expression,
            ",",
          ),
        ),
        "}",
      ),
    field_initializer: ($) =>
      seq(".", field("name", $.identifier), "=", field("value", $._expression)),

    // 后缀链的合法基座 —— 故意不含 if/while/for/switch/block/labeled：
    // 这些控制流表达式后面跟 `.`/`(`/`[` 时，链必须断（否则 `if …;` 的
    // 分号被吞、下一行的 `.method()` 会跨行挂上来，guide_examples 的经典翻车点）。
    _postfix_base: ($) =>
      choice(
        $.identifier,
        $.type_identifier,
        $.builtin_function,
        $.parenthesized_expression,
        $.cast_expression,
        $.call_expression,
        $.field_expression,
        $.index_expression,
        $.unwrap_expression,
        $.slice_expression,
        $.container_literal,
        $.anonymous_container,
        $.tuple_literal,
        $.module_expression,
        $.string,
        $.integer,
        $.float,
        $.character,
        $.boolean,
        $.null_literal,
        $.undefined_literal,
      ),

    call_expression: ($) =>
      prec(PREC.CALL, seq(field("function", $._postfix_base), field("arguments", $.argument_list))),
    argument_list: ($) =>
      seq("(", optional(commaSep(choice($._expression, $._type))), optional(","), ")"),

    field_expression: ($) =>
      prec(
        PREC.CALL,
        seq(
          field("object", $._postfix_base),
          ".",
          field("field", choice($.field_identifier, $.integer)),
        ),
      ),

    // 可赋值位置：标识符 / 成员 / 下标 / 解包 / 解引用
    _place: ($) =>
      choice(
        $.identifier,
        $.field_expression,
        $.index_expression,
        $.unwrap_expression,
        $.call_expression,
        prec(PREC.UNARY, seq("*", $._place)),
      ),

    // o.? —— 解包可选值
    unwrap_expression: ($) =>
      prec(PREC.CALL, seq(field("object", $._postfix_base), ".", "?")),

    index_expression: ($) =>
      prec(PREC.CALL, seq(field("object", $._postfix_base), "[", field("index", $._expression), "]")),

    // xs[0..] / xs[a..b]
    slice_expression: ($) =>
      prec(
        PREC.CALL,
        seq(
          field("object", $._postfix_base),
          "[",
          field("start", $._expression),
          "..",
          optional(field("end", $._expression)),
          "]",
        ),
      ),

    range_expression: ($) =>
      prec.right(
        PREC.RANGE,
        seq($._expression, "..", optional($._expression)),
      ),

    unary_expression: ($) =>
      prec(
        PREC.UNARY,
        seq(field("operator", choice("!", "-", "~", "*", "&", "?")), field("operand", $._expression)),
      ),

    binary_expression: ($) =>
      choice(
        ...[
          ["+", PREC.ADD],
          ["-", PREC.ADD],
          ["*", PREC.MUL],
          ["/", PREC.MUL],
          ["%", PREC.MUL],
          ["<<", PREC.SHIFT],
          [">>", PREC.SHIFT],
          ["&", PREC.BIT_AND],
          ["|", PREC.BIT_OR_XOR],
          ["^", PREC.BIT_OR_XOR],
          ["==", PREC.EQUAL],
          ["!=", PREC.EQUAL],
          ["<", PREC.COMPARE],
          ["<=", PREC.COMPARE],
          [">", PREC.COMPARE],
          [">=", PREC.COMPARE],
          ["&&", PREC.AND],
          ["||", PREC.OR],
        ].map(([op, p]) => prec.left(p, seq(field("left", $._expression), field("operator", op), field("right", $._expression)))),
        prec.left(PREC.ORELSE, seq(field("left", $._expression), "orelse", field("right", $._expression))),
        // 单独列出以便着色 and/or/not 关键字形状
        prec.left(PREC.AND, seq(field("left", $._expression), field("operator", "and"), field("right", $._expression))),
        prec.left(PREC.OR, seq(field("left", $._expression), field("operator", "or"), field("right", $._expression))),
      ),

    try_expression: ($) => prec(PREC.UNARY, seq("try", field("operand", $._expression))),

    return_statement: ($) =>
      seq("return", optional(field("value", $._expression)), ";"),

    defer_statement: ($) =>
      seq("defer", field("body", $._statement)),

    errdefer_statement: ($) =>
      seq("errdefer", field("body", $._statement)),

    continue_statement: ($) =>
      seq("continue", optional(seq(":", field("label", $.identifier))), ";"),

    builtin_function: ($) => token(/@[a-zA-Z]+/),
  },
});

/**
 * 逗号分隔辅助。
 */
function commaSep1(rule) {
  return seq(rule, repeat(seq(",", rule)));
}
function commaSep(rule) {
  return optional(commaSep1(rule));
}
