; §193/§233：回车自动缩进。Zed 的 @end 必须**嵌在 @indent 查询内部**才生效：
;   (node "}" @end) @indent
; 表示缩进范围从节点开头延伸到闭括号之前。此前把 @end 写成平行查询（落空），
; 又用 "}" @outdent 在闭括号处结束最内层范围——在 `{|}` 中间回车时新行正好
; 落在 outdent 边界，光标直接退到行首（vim o 同理）。
;
; @start 把起点移到开括号之后，@end 把终点移到闭括号之前，所以花括号内部的
; 新行缩进一级、闭括号所在行回到原级。缩进宽度取 config.toml 的 tab_size。
;
; 覆盖：语句块 / struct·enum·union·error 身体 / 调用实参表 / 函数形参表 /
; 容器字面量（T{...} 与 .{...}）/ switch 匿名花括号。
; capture_block / try_capture_expression 内部已含 block 节点，由 block 覆盖，
; 不再单独 @indent（重复会多缩一级）。

(block "{" @start "}" @end) @indent

(container_body "{" @start "}" @end) @indent

(parameters "(" @start ")" @end) @indent

(argument_list "(" @start ")" @end) @indent

(initializer_list "{" @start "}" @end) @indent

(switch_expression "{" @start "}" @end) @indent
