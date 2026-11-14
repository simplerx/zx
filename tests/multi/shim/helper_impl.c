/* shim/helper_impl.c —— 模块自带的 C 实现（§160）。
 *
 * 关键点：@shim 的路径相对**写它的那份 .zx** 解析，所以一个库可以把
 * `foo.zx` 和 `foo_impl.c` 放在同一目录发布，使用方不需要知道这些文件
 * 在哪、也不需要额外加编译参数。 */

int zx_shim_mod_val(void) {
    return 42;
}

int zx_shim_mod_twice(int v) {
    return v * 2;
}
