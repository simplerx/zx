; Zx 语法着色 —— 节点名与 tree-sitter-zx 语法一一对应。
; 捕获名用 Zed 标准主题约定（@keyword / @type / @function / @string …）。

; ------------------------------------------------------------- 注释 --
(line_comment) @comment
(block_comment) @comment

; ----------------------------------------------------------- 字面量 --
(string) @string
(escape_sequence) @string.escape
(character) @string
(integer) @number
(float) @number
(boolean) @constant.builtin
(null_literal) @constant.builtin
(undefined_literal) @constant.builtin

; ----------------------------------------------------------- 关键字 --
; 24 个关键字（guide.md §3.2）+ and/or；§144 起 `module` 收成内建 `@mod`。
; null/undefined 由 null_literal/undefined_literal 捕获（树里没有匿名 token）
[
  "const" "let" "var" "fn"
  "if" "else" "while" "for" "inline" "switch" "continue" "return"
  "defer" "errdefer" "struct" "union" "enum" "error" "pub" "extern" "try"
  "and" "or"
] @keyword

"orelse" @keyword.operator

; §170 贴标：只染 `tag.` 的 tag（上下文关键字，v.tag 里的 tag 不受影响）
(tag_application "tag" @keyword)

; §171 try 捕获形式（try 本身已在关键字表里，这里染捕获绑定名）
(try_capture_expression binding: (identifier) @variable)

; ------------------------------------------------------------ 内建 --
(builtin_function) @function.builtin

; ------------------------------------------------------------- 类型 --
(type_identifier) @type
(primitive_type) @type.builtin

(container_type
  ["struct" "union" "enum"] @keyword)
(error_set_type
  "error" @keyword)
(enum_tag
  "enum" @keyword)

; ----------------------------------------------------------- 声明 --
; `const foo = fn …` —— 函数定义：名字着函数色
(const_declaration
  name: (identifier) @function
  value: (function_expression))

; `const Foo = struct { … }` —— 类型定义：名字着类型色
(const_declaration
  name: (identifier) @type
  value: (container_type))

; 普通常量/变量名
(let_declaration name: (identifier) @variable)
(var_declaration name: (identifier) @variable)

(destructure_pattern
  (identifier) @constant)

; 函数参数名
(parameters
  (identifier) @variable.parameter)

; 字段
(field_declaration
  name: (identifier) @field)

; 枚举 / error set 变体
(enum_variant
  name: (identifier) @constant)

; ------------------------------------------------------------- 调用 --
; 具名函数调用 foo(...)
(call_expression
  function: (identifier) @function.call)

; 方法调用 x.y(...) —— y 着方法色
(call_expression
  function: (field_expression
    field: (field_identifier) @function.method))

; 字段访问 x.y —— y 着属性色
(field_expression
  field: (field_identifier) @property)

; 模块表达式。`@mod("std")` 现在由 builtin_function / call_expression 捕获
; （@ 前缀内建走上面的内建着色）；下面这条只服务仍用旧 `module(...)` 的历史文件。
(module_expression
  "module" @keyword
  path: (string) @string.special)

; --------------------------------------------------------- 控制流 --
; 收割/标签 `tag: expr` 的标签
(labeled_expression
  label: (identifier) @label)

; switch 的 `.member` 模式
(enum_pattern
  member: (identifier) @constant)

; for 的绑定名
(for_expression
  binding: (identifier) @variable)

; if/while 的捕获绑定名
(capture_block
  binding: (identifier) @variable)
(capture_expr
  binding: (identifier) @variable)

; switch 臂的捕获绑定
(switch_arm
  binding: (identifier) @variable)

; ------------------------------------------------------------ 运算符 --
(binary_expression
  operator: [
    "+" "-" "*" "/" "%" "<<" ">>" "&" "|" "^"
    "==" "!=" "<" "<=" ">" ">=" "&&" "||" "and" "or"
  ] @operator)

(unary_expression
  operator: ["!" "-" "~" "*" "&" "?"] @operator)

(assignment_statement
  operator: [
    "=" "+=" "-=" "*=" "/=" "%=" "&=" "|=" "^=" "<<=" ">>="
  ] @operator)

[
  ".." "." "?"
] @operator

; ------------------------------------------------------------ 标点 --
[
  "(" ")" "[" "]" "{" "}"
] @punctuation.bracket

[
  "," ":" ";"
] @punctuation.delimiter
