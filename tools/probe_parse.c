/* probe_parse.c —— A1 排查用：只跑 lex + parse（C 前端路径），量净耗时。
 * 链接: zig cc -O2 -o .tmp/probe_parse.exe tools/probe_parse.c \
 *       bin/zxc.exe_lex.o bin/zxc.exe_parse.o bin/zxc.exe_zxc.o
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "zxc.h"
#include "ast.h"

void sema_check(Node *root);   /* src/sema.c（ast.h 未声明，探针自带） */

static char *read_src(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    buf[n] = 0;
    *out_len = (size_t)n;
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: probe_parse file.zx\n"); return 2; }
    size_t len = 0;
    char *src = read_src(argv[1], &len);
    if (!src) { fprintf(stderr, "cannot open\n"); return 2; }

    clock_t t0 = clock();
    Lexer lx;
    lexer_init(&lx, src, len);
    lexer_run(&lx);
    clock_t t1 = clock();
    Node *root = parse_program(&lx);
    clock_t t2 = clock();

    /* 数节点：粗略遍历，防止全被优化掉；顺带报告规模 */
    unsigned long count = 0;
    /* 只统计根的第一层 items，够防优化即可 */
    if (root && root->items && root->nitems) count = (unsigned long)root->nitems;

    /* 可选第二参数 = 跑 sema（需要链接 sema.c/backend.c 等） */
    if (argc > 2 && strcmp(argv[2], "--sema") == 0) {
        clock_t t3 = clock();
        sema_check(root);
        clock_t t4 = clock();
        fprintf(stderr, "sema  %8.1f ms\n", (double)(t4 - t3) * 1000.0 / CLOCKS_PER_SEC);
    }

    fprintf(stderr, "lex   %8.1f ms\n", (double)(t1 - t0) * 1000.0 / CLOCKS_PER_SEC);
    fprintf(stderr, "parse %8.1f ms\n", (double)(t2 - t1) * 1000.0 / CLOCKS_PER_SEC);
    fprintf(stderr, "顶层声明 %lu, 诊断 %d\n", count, g_error_count);
    return g_error_count ? 1 : 0;
}
