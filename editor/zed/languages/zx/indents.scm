; §193：回车自动缩进。Zed indents.scm 约定：@indent 开层（进入节点后
; 缩进），@end 收口（节点结束行回到原层级），@outdent 闭括号退层。
; 缩进宽度跟编辑器 tab_size（默认 4 空格）。
; 覆盖：语句块 / struct·enum·union·error 身体 / 调用实参表 / 函数形参表 /
; 容器字面量（T1{...} 与 .{...}）/ if·while 捕获块 / try 捕获块。
; switch 的大括号是匿名 token（switch_expression 直接含 {...}），不在
; 独立 body 节点里，交给 "}" @outdent 兜底。

[
  (block)
  (container_body)
  (argument_list)
  (parameters)
  (initializer_list)
  (capture_block)
  (try_capture_expression)
] @indent

[
  (block)
  (container_body)
  (argument_list)
  (parameters)
  (initializer_list)
  (capture_block)
  (try_capture_expression)
] @end

[
  "}"
  ")"
  "]"
] @outdent
