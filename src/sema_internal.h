/************************************************************************
 * sema_internal.h - sema_*.c 共享类型 / 全局 / 内部接口（统一入口）。
 * 由 sema.c 拆分生成；不要在这里放实现，实现各归 sema_*.c。
 *************************************************************************/
#ifndef ZX_SEMA_INTERNAL_H
#define ZX_SEMA_INTERNAL_H

#include "zxc.h"
#include "ast.h"
#include "ty.h"
#include "val.h"
#include <time.h>
#include <direct.h>

#define BAD(n, ...) (err_at((n)->line, (n)->col, __VA_ARGS__), ty_invalid())
#define BAD_CODE(code, n, ...) \
    (diag_code_at((code), (n)->line, (n)->col, __VA_ARGS__), ty_invalid())
#define MAX_LABEL 256
#define RS_LIVE   0
#define RS_FREED  1
#define RS_DEAD   2   /* §133：整张句柄表被 deinit 作废 */
#define TST_UNBOUND 0
#define TST_LIVE    1
#define TST_FREED   2
#define TST_MOVED   3   
#define TC_NONE     0
#define TC_LIVE     1
#define TC_FREED    2
#define TC_COND     3
#define TC_UNKNOWN  4
#define TC_MOVED    5   /* §189：载体已随 std.move 转移 */
#define TAG_SNAP_MAX 128
#define CT_MAX_CALLS  200000
#define CT_MAX_LOOPS  1000000

/* ---- shared types ---- */

enum { SK_CONST, SK_LET, SK_VAR, SK_PARAM, SK_CAPTURE, SK_FN, SK_NS };
typedef struct Symbol Symbol;
typedef struct FieldTag FieldTag;
struct Symbol { const char *name; int kind; Node *decl; Type *ty; Type *type_val; int state; int index; Value *ct; int builtin; struct Module *mod; bool is_pub; bool is_std; bool broken; bool ct_bind; bool in_fn; int fn_d; bool fn_local; bool is_top; bool used; struct Symbol *locals_next; bool stack_val; bool cap_borrow; int reg_slot; int tag_id; int tag_state; FieldTag *ftags; Node *ft_src; bool ptr_taint; const char *cname; int decl_line; int decl_col; int nassign; bool escaped; Node *cap_src; Node *in_agg; Symbol *next; };
typedef struct HBucket { Symbol *sym; struct HBucket *next; } HBucket;
typedef struct Scope { struct Scope *parent; Symbol *first; bool is_module; HBucket **tab; int ntab; int cnt; int tag_wm; } Scope;
typedef struct Module { const char *path; const char *stem; Node *root; Scope scope; Symbol *ns; bool loading; bool tags_enabled; bool tags_batch; } Module;
typedef struct { int kind; Value *val; const char *label; } CtFlow;
enum { CT_NORMAL, CT_RETURN, CT_YIELD, CT_CONTINUE };
typedef struct { const char *name; Type *ty; bool has; bool is_loop; bool is_fn; Node *val; } Label;
typedef struct { Node *fn; Module *mod; struct Scope *scope; } Pending;
typedef struct { Node *call; Node *field; } DevirtPending;
typedef struct { Node *fn; int nargs; Value **args; Node *clone; Type *ty; Value *val; } Inst;
enum { BI_NONE, BI_PRINT, BI_PRINTLN, BI_FORMAT, BI_HEAP, BI_FILE, BI_FILE_FN, BI_IO, BI_IO_FN, BI_IO_PRINT, BI_STACKALLOC, BI_HEAPALLOC, BI_HEAPSLICE, BI_FREE, BI_PROC, BI_PROC_FN, BI_DEBUG, BI_DEBUG_FN, BI_TASK, BI_TASK_FN, BI_SYNC, BI_SYNC_FN };
typedef struct { bool ok; bool neg; uint64_t v; } IntV;
typedef struct { Node *clone, *tmpl; } IdentAnchor;
typedef struct { int state; int line, col; int free_line, free_col; uint64_t size, align; const char *unit; int gid; uint64_t off, total; Node *site; Symbol *sym; int tag; bool no_merge; bool is_handle; bool tainted; Symbol *owner_sym; } RegionSlot;
typedef struct { int line, col; const char *unit; bool proven, erased; const char *reason; } ProvePoint;
typedef struct { const char *name; int line, col; int state; int region; int free_line, free_col; int moved_line, moved_col; bool confirm_arm; int confirm_line, confirm_col; int own_line, own_col; Symbol *carrier; bool stack; bool abstract; bool auto_n; bool scope_freed; bool ever_freed; bool arr_esc; bool rt_pending; int rt_line, rt_col; Symbol *adopt_ar; int adopt_line, adopt_col; bool book; Node *ret_fn; int inst_of; int ninst; const char *def_fn; const char *own_fn; const char *file; } TagSlot;
typedef struct { const char *callee, *caller; int line, col; } TagCallEdge;
struct FieldTag { const char *name; int slot; FieldTag *next; };
typedef struct { Symbol *psym; const char *fname; } ParamFieldFree;
typedef struct { Symbol *psym; const char *key; int slot; Symbol *src_param; } ParamFieldStore;
typedef struct { int tag_id; Symbol *psym; } ParamSlotSym;
typedef struct { Symbol *base; char path[64]; int idx; int slot; } ArrEsc;
typedef struct { Symbol *base; char path[64]; int slot; bool taken; } AgtMem;
typedef struct { Symbol *base; char path[64]; } AgtClosed;
typedef struct { Symbol *ar; int slot; int line, col; bool done; } ArAdopt;
typedef struct { int tag; int state; Node *val; bool stmt; } ArmExit;
typedef struct { int n; int total; int state[TAG_SNAP_MAX]; Symbol *carrier[TAG_SNAP_MAX]; int cstate[TAG_SNAP_MAX]; Symbol *fsym[TAG_SNAP_MAX]; FieldTag *ft[TAG_SNAP_MAX]; int nfts; Symbol *lsym[TAG_SNAP_MAX]; int lstate[TAG_SNAP_MAX]; int nlsyms; } TagSnap;
typedef struct { Node *call, *arg, *fn; Symbol *ps; int idx; int tid, tst, rel; const char *srcpath; } TagDefer;
typedef struct YieldScope { const char *label; const struct YieldScope *up; } YieldScope;

/* ---- shared globals ---- */

extern Scope *g_scope;
extern Symbol *g_locals;
extern Module **g_mods;
extern int g_nmods, g_cmods;
extern Module *g_mod;
extern bool g_root_stmts;
extern bool g_ct_stuck;
extern int g_ct_exec;
extern bool g_ct_soft_div;
extern bool g_ct_soft_bounds;
extern CtFlow g_ct_flow;
extern int g_ntslots;
extern int g_nlabels;
extern Type *g_ret;
extern int g_fn_depth;
extern bool g_in_defer;
extern bool g_in_tag_rebind;
extern int g_arm_depth;
extern Pending *g_pending;
extern int g_npending, g_cappending;
extern DevirtPending *g_devirt_retry;
extern int g_ndr, g_cdr;
extern bool g_devirt_final;
extern Inst *g_insts;
extern int g_ninst, g_cinst;
extern int g_spec_depth;
extern const char *g_std_seeded[];
extern bool g_neg_lit_ctx;
extern const char **g_ident_params;
extern int g_nident_params;
extern RegionSlot *g_rslots;
extern int g_nrslots, g_crslots;
extern int g_flow_depth;
extern int g_region_gid;
extern ProvePoint *g_proves;
extern int g_nproves, g_cproves;
extern int g_zx_release;
extern const char *g_cur_fn_name;
extern TagSlot *g_tslots;
extern TagCallEdge *g_cedges;
extern int g_ncedges, g_ccedges;
extern Module *g_std_mod;
extern ParamFieldStore *g_pfs;
extern int g_npfs, g_cpfs;
extern int g_npderef, g_cpderef;
extern int g_npslotsym, g_cpslotsym;
extern ArrEsc *g_aesc;
extern int g_naesc, g_caesc;
extern AgtMem *g_agt;
extern int g_nagt, g_cagt;
extern int g_tag_auto_seq;
extern int g_ntdefer, g_ctdefer;
extern int g_dead_diag[32][2];
extern char g_exe_dir[1024];
extern Node *g_ct_stack[128];

/* ---- shared functions ---- */

int edit_distance(const char *a, const char *b);
bool name_close(const char *bad, const char *cand);
const char *suggest_member(const char *bad, Type *t);
const char *suggest_ident(const char *bad);
const char *sema_sym_cname(Symbol *s);
void scope_push(Scope *s);
void scope_pop(Scope *s);
Symbol *scope_find_here(Scope *sc, const char *name);
Symbol *scope_add(Scope *sc, const char *name, int kind);
void local_track(Symbol *s, int line, int col);
void check_unused(Symbol *head, int errs_before);
Symbol *lookup(const char *name);
Symbol *scope_add_at(Scope *sc, const char *name, int kind, int line, int col);
Label *label_push(const char *name, bool is_loop);
void label_pop(void);
Label *label_find(const char *name);
Label *label_find_loop(const char *name);
void schedule_body(Node *fn);
Type *base_type(const char *n);
Type *sema_allocator_type(void);
Type *sema_type_field_type(void);
Type *sema_type_type(void);
bool sema_ty_ct_only(Type *t);
Value *type_desc_value(Type *t, Node *inst);
Symbol *std_builtin(const char *name);
Symbol *ns_member(Symbol *ns, const char *name, int line, int col);
void seed_std_builtins(Scope *sc);
IntV intv(uint64_t v);
IntV noint(void);
IntV ev_int(Node *n);
Type *scalar_default(Type *t);
bool numeric_like(Type *t);
void lit_digits(Node *n, char *out, int cap);
bool lit_value(Node *e, uint64_t *v, bool *neg, const char **text);
bool lit_fits_expr(Node *e, Type *t);
bool can_coerce(Type *from, Type *to);
Node *ident_decl(Node *n);
void anchor_types(Node *tmpl, Node *clone);
Type *resolve_type(Node *n);
bool check_c_able(Node *at, Type *t, const char *fname);
Type *resolve_fn_type(Node *fn);
Type *unify(Node *at, Type *a, Type *b);
bool lvalue_mutable(Node *n);
Type *type_of(Node *n, Type *t);
int region_new(Node *site, uint64_t size, uint64_t align);
void region_bind(int id, Symbol *s);
int region_of_expr(Node *e);
bool is_allocator_ty(Type *t);
void region_note_free(Node *call);
void region_plan_block(Node *blk);
void region_handle_call(Node *n);
void tag_call_edge(const char *callee, const char *caller, int line, int col);
TagSlot *tag_slot(int id);
TagSlot *tag_by_name(const char *name);
FieldTag *ft_find(FieldTag *list, const char *name);
void ft_set(FieldTag **plist, const char *name, int slot);
FieldTag *ft_clone(FieldTag *src);
FieldTag *ft_subtree(FieldTag *list, const char *key);
void ft_clear_prefix(FieldTag *list, const char *key);
void ft_mirror(Symbol *s, const char *key, int slot, int depth);
FieldTag *ft_merge(FieldTag *a, FieldTag *b);
bool ft_path_key_d(Node *e, char *buf, size_t cap, Node **base_out, bool *deref_tail);
bool ft_path_key(Node *e, char *buf, size_t cap, Node **base_out);
char *ft_path_dup(const char *prefix, const char *leaf);
FieldTag *ftags_of_expr(Node *e);
void tag_capture_inherit(Symbol *cs, Node *subj, const char *vname);
void pff_add(Symbol *ps, const char *fname);
bool pff_has(Symbol *ps, const char *fname);
void pfs_add_ex(Symbol *ps, const char *key, int slot, Symbol *src_param);
void pfs_add(Symbol *ps, const char *key, int slot);
bool pderef_has(Symbol *ps);
void pslot_sym_add(int tag_id, Symbol *ps);
Symbol *pslot_sym_find(int tag_id);
void aesc_add(Symbol *base, const char *path, int idx, int slot);
int aesc_find(Symbol *base, const char *path, int idx);
int aesc_const_index(Node *ix);
bool ft_num_elem(Node *e);
int expr_taint(Node *e);
void agt_add(Symbol *base, const char *path, int slot);
bool agt_has(Symbol *base, const char *path, int slot);
bool agt_any(Symbol *base, const char *path);
void agt_remove(Symbol *base, const char *path, int slot);
void agt_clear(Symbol *base, const char *path, Node *site);
Symbol *agt_base_of(Node *e, char *path, size_t cap);
bool agt_closed(Symbol *base, const char *path);
void pffa_add(Symbol *ps);
bool pffa_has(Symbol *ps);
bool tag_diag_once(int line, int col);
void ft_mark_freed(int slot, Node *site);
void ft_scan_returns(Node *n, FieldTag **out);
void tag_of_expr(Node *e, int *id, int *state);
TagSlot *tag_decl(const char *name, Node *n);
void tag_auto_decl(Node *call, bool stack);
int tag_clone_site(TagSlot *src, Node *call, bool freed);
void pfs_mirror_arg(Symbol *ps, Node *arg, Node *call, Node *fn);
void tag_scope_exit(Scope *sc, Node *n);
TagSlot *tag_resolve(Node *n);
void tag_check_deref(Node *base);
Symbol *tag_carrier_sym(Node *e);
bool tag_free_event(Node *site, Node *ptr);
void tag_note_free(Node *call);
void tag_note_adopt(Node *call);
bool allocator_exempt(Node *alarg);
void tag_note_container_init(Node *call);
void tag_note_arena_deinit(Node *call);
bool call_is_std_move(Node *call);
Type *check_std_move(Node *call);
bool tag_block_has_free(Node *n);
bool fn_is_releaser(Node *fn);
int fn_ret_cache(Node *fn, int *k_out, int *slot_out);
void fn_stamp_ret_flow(Node *fn);
int tag_check_apply(Node *n, bool *bad);
int arm_ret_scan(Node *nd, int slot);
ArmExit tag_arm_reconcile(Node *armval, Node *armnode, const int *snap, int n0, int ntotal);
void tag_join_arms(Node *n, ArmExit a, ArmExit b);
void tag_snap(TagSnap *s);
void tag_restore(const TagSnap *s);
int ft_arm_collect(const TagSnap *s, Symbol **syms, FieldTag **fts, int cap);
FieldTag *ft_arms_intersect(FieldTag *a, FieldTag *b);
void ft_arms_join(Symbol **s1, FieldTag **f1, int n1, Symbol **s2, FieldTag **f2, int n2);
void tag_loop_balance(Node *body, const TagSnap *snap);
bool tag_module_exempt(Module *m);
void tag_ledger_final(const char *entry_path);
bool expr_frame(Node *e);
bool expr_borrow(Node *e);
bool addr_frame(Node *o);
bool frame_in_agg(Node *e, int depth);
Type *check_assign(Node *n);
bool f64_fits_int(double v, int bits, bool sign);
Type *check_binary(Node *n, Type *expect);
Type *check_array_lit(Node *n, Node **items, int nitems, Type *expect);
Type *check_unary(Node *n, Type *expect);
Type *check_cast(Node *n);
void *cur_errset(void);
void *expr_errset(Node *e, Type *t);
const char *errset_missing(const void *a, const void *b);
Type *check_field(Node *n);
Type *check_index(Node *n);
Type *check_slice_expr(Node *n);
Symbol *ns_member_symbol(Node *field);
bool callee_is_generic(Node *callee);
Type *instance_result(Node *g);
void check_fmt_args(Node *call, int first, int nargs);
bool addr_ok(Node *e);
bool ty_is_allocator(Type *t);
Symbol *synthetic_fn_sym(const char *name);
Symbol *allocator_target(Node *e, bool want_alloc, int depth);
void devirt_allocator_call(Node *n);
void devirt_retry_all(void);
Type *check_call(Node *n, Type *expect);
void tag_call_flow(Node *n);
void tag_deferred_replay(void);
void tag_transform_deinit(Node *call);
void tuple_stamp_ftags(Node *n);
Type *check_expr_raw(Node *n, Type *expect);
void check_stmts(Node *blk);
void sym_link(Symbol *dst, Symbol *src);
Symbol *module_member(Module *m, const char *name);
bool decl_is_lazy_module(Node *d);
void hoist(Scope *sc, Node *blk);
void ensure_raw(Symbol *s);
char *path_normalize(const char *p);
char *path_dup_dir(const char *path);
char *path_join(const char *dir, const char *rel);
Module *module_find(const char *path);
Module *module_load(const char *rel, int line, int col);
Module *module_load_full(const char *full_in, int line, int col);
void stamp_mod(Node *n, const char *path);
bool deny_member(Node *n, Type *owner, bool is_pub, const char *name);
bool find_std_path(char *out, size_t cap);
void sym_copy(Symbol *dst, Symbol *src, const char *name);
Module *module_load_std(int line, int col);
void ensure(Symbol *s);
Type *check_fn_body(Node *fn);
Type *check_block(Node *n);
void check_stmt(Node *n);
Type *check_arm(Node *a, Type *expect);
bool falls_through(Node *blk);
bool stmt_ends(Node *s);
bool arm_ends(Node *a);
bool arm_ends_with_yield(Node *n);
bool stmt_blocks_next(Node *s);
void check_return_paths(Node *fn, Type *ret);
Type *sema_construct(Node *n, Type *expect);
Type *coerce_node(Node *e, Type *from, Type *to, const char *ctx);
Node *deps_fn_of_name(const char *name, Scope *ps);
Node *deps_fn_of_field(Node *fld, Scope *ps);
void deps_check_one(Node *fn);
void deps_check_calls(Node *n, Scope *ps);
void sema_check(Node *root);
void ct_clear_stmts(Node *n);
void ct_clear(Node *n);
Node *clone_tree(Node *n);
bool ct_view_len(Value *v, Type *t, int64_t *len);
Value *ct_field(Node *n);
Value *ct_index(Node *n);
Value *ct_call(Node *n);
Value *ct_eval(Node *n);
CtFlow ct_run(Node *n);
Value *fold(Node *n);
Value *ct_eval_probe(Node *n);
Value *fold_expr(Node *n);
Type *check_expr(Node *n, Type *expect);
bool fn_is_generic(Node *fn);
Value *arg_ct(Node *a, Type *want);
Node *specialize(Node *fn, Node *call, int *ncomp, Type **out_ft, Value **out_val);
void name_type_from_call(Node *call, Type *result);
Type *instantiate_type(Node *call);
Type *instantiate_fn_type(Node *call);

#endif /* ZX_SEMA_INTERNAL_H */
