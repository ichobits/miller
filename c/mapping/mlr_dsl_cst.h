#ifndef MLR_DSL_CST_H
#define MLR_DSL_CST_H

#include "cli/mlrcli.h"
#include "mapping/mlr_dsl_ast.h"
#include "containers/type_decl.h"
#include "containers/lhmsmv.h"
#include "containers/local_stack.h"
#include "containers/loop_stack.h"
#include "mapping/mlr_dsl_blocked_ast.h"
#include "mapping/rval_evaluators.h"
#include "mapping/function_manager.h"
#include "output/multi_out.h"
#include "output/multi_lrec_writer.h"

// ================================================================
// Concrete syntax tree (CST) derived from an abstract syntax tree (AST).
//
// Statements are of the form:
//
// * Assignment of mlrval (i.e. result of expression evaluation, e.g. $name or f($x,$y)) to oosvar (out-of-stream
// variables, prefixed with @ sigil)
//
// * Assignment to srec (in-stream records, with field names prefixed with $ sigil)
//
// * Copying full srec ($* syntax) to/from an oosvar
//
// * Oosvar-to-oosvar assignments (recursively if RHS is non-terminal)
//
// * pattern-action statements: boolean expression with curly-braced statements which are executed only
//   when the boolean evaluates to true.
//
// * bare-boolean statements: no-ops unless they have side effects: namely, the matches/does-not-match
//   operators =~ and !=~ setting regex captures \1, \2, etc.
//
// * emit statements: these place oosvar key-value pairs into the output stream.  These can be of the following forms:
//
//   o 'emit @a; emit @b' which produce separate records such as a=3 and b=4
//
//   o 'emitf @a, @b' which produce records such as a=3,b=4
//
//   o For nested maps, 'emit @c, "x", "y"' in which case the first two map levels are pulled out and named "x" and "y"
//   in separate fields. See containers/mlhmmv.h for more information.
//
// Further, these statements are organized into three groups:
//
// * begin: executed once, before the first input record is read.
// * main:  executed for each input record.
// * end:   executed once, after the last input record is read.
//
// The exceptions being, of course, assignment to/from srec is disallowed for begin/end statements since those occur
// before/after stream processing, respectively.
// ================================================================

// ----------------------------------------------------------------
// Two-pass stack allocator which operates on the block-structured AST
// before the CST is build (mlr_dsl_stack_allocate.c).
void blocked_ast_allocate_locals(blocked_ast_t* paast, int trace);

// ----------------------------------------------------------------
// Forward references for virtual-function prototypes
struct _mlr_dsl_cst_t;
struct _mlr_dsl_cst_statement_t;
struct _mlr_dsl_cst_statement_vararg_t;
struct _subr_defsite_t;

// Parameter bag to reduce parameter-marshaling
typedef struct _cst_outputs_t {
	int*    pshould_emit_rec;
	sllv_t* poutrecs;
	char*   oosvar_flatten_separator;
	cli_writer_opts_t* pwriter_opts;
} cst_outputs_t;

// ----------------------------------------------------------------
typedef struct _cst_statement_block_t {
	int subframe_var_count;
	sllv_t* pstatements;
} cst_statement_block_t;

cst_statement_block_t* cst_statement_block_alloc(int subframe_var_count);
void cst_statement_block_free(cst_statement_block_t* pblock);

// ----------------------------------------------------------------
typedef struct _cst_top_level_statement_block_t {
	local_stack_frame_t* pframe;
	int max_var_depth;
	cst_statement_block_t* pblock;
} cst_top_level_statement_block_t;

cst_top_level_statement_block_t* cst_top_level_statement_block_alloc(int max_var_depth, int subframe_var_count);
void cst_top_level_statement_block_free(cst_top_level_statement_block_t* pblock);

// ----------------------------------------------------------------
// Generic handler for a statement.

// Subhandler for emitf/unset vararg items: e.g. in 'unset @o, $s' there is one for the @o and one for the $s.
typedef void unset_vararg_handler_t(
	struct _mlr_dsl_cst_statement_vararg_t* pvararg,
	variables_t*                    pvars,
	cst_outputs_t*                  pcst_outputs);

// Most statements have one item, except emit and unset.
typedef struct _mlr_dsl_cst_statement_vararg_t {
	unset_vararg_handler_t* punset_handler;
	int                     unset_local_variable_frame_relative_index;
	char*                   emitf_or_unset_srec_field_name;
	sllv_t*                 punset_oosvar_keylist_evaluators;
	rval_evaluator_t*       punset_srec_field_name_evaluator;
	rval_evaluator_t*       pemitf_arg_evaluator;
} mlr_dsl_cst_statement_vararg_t;

// Handler for statement lists: begin/main/end; cond/if/for/while/do-while.
typedef void mlr_dsl_cst_block_handler_t(
	cst_statement_block_t* pblock,
	variables_t*           pvars,
	cst_outputs_t*         pcst_outputs);

// ----------------------------------------------------------------
// xxx cst-statement-federation project

typedef struct _mlr_dsl_cst_statement_t* mlr_dsl_cst_statement_allocator_t(
	struct _mlr_dsl_cst_t* pcst,
	mlr_dsl_ast_node_t*    pnode,
	int                    type_inferencing,
	int                    context_flags);

typedef void mlr_dsl_cst_statement_handler_t(
	struct _mlr_dsl_cst_statement_t* pstatement,
	variables_t*                     pvars,
	cst_outputs_t*                   pcst_outputs);

typedef void mlr_dsl_cst_statement_freer_t(
	struct _mlr_dsl_cst_statement_t* pstatement);

// ----------------------------------------------------------------
// MLR_DSL_CST_STATEMENT OBJECT

// These hold all the member data needed to evaluate any CST statement. No one kind of statement
// uses all of them. They aren't expressed as a union since their count is small: there's one CST
// per mlr-put invocation, independent of the number of stream records processed.
//
// Difference between keylist and namelist: in emit @a[$b]["c"], "d", @e, the keylist is ["a", $b, "c"]
// and the namelist is ["d", @e].

typedef struct _mlr_dsl_cst_statement_t {

	//  - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
	// Common to most or all statement types

	// For trace-mode.
	mlr_dsl_ast_node_t* past_node;

	// Function-pointer for the handler of the given statement type, e.g. srec-assignment, while-loop, etc.
	mlr_dsl_cst_statement_handler_t* pstatement_handler;

	// The reason for this being a function pointer is that there are two variants of
	// statement-list handlers: one for inside loop bodies which has to check
	// break/continue flags after each statement, and another for outside loop bodies
	// which doesn't need to check those. (This is a micro-optimization.) For bodyless
	// statements (e.g. assignment) this is null.
	cst_statement_block_t* pblock;
	mlr_dsl_cst_block_handler_t* pblock_handler;

	mlr_dsl_cst_statement_freer_t* pstatement_freer;

	//  - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
	// Specific to each statement type:

	void* pvstate;

	//  - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
	// To be federated:

	// Assignment to oosvar
	sllv_t* poosvar_target_keylist_evaluators;

	// Assignment to local
	// The variable name is used only for type-decl exceptions. Otherwise the
	// name is replaced with the frame-relative index by the stack allocator.
	char* local_lhs_variable_name;
	int   local_lhs_frame_relative_index;
	int   local_lhs_type_mask;

	// Assignment to srec
	char* srec_lhs_field_name;

	// Assignment to ENV (i.e. putenv)
	char* env_lhs_name;

	// Indirect assignment to srec
	rval_evaluator_t* psrec_lhs_evaluator;

	// Assignments to srec or oosvar, as well as the boolean expression in filter, cond, and bare-boolean
	rval_evaluator_t* prhs_evaluator;

	// Assigning full srec from oosvar:
	sllv_t* poosvar_rhs_keylist_evaluators;

	// Vararg stuff for emit and unset
	sllv_t* pvarargs;

	char** for_map_k_variable_names;
	int*  for_map_k_frame_relative_indices;
	int*  for_map_k_type_masks;
	int   for_map_k_count;

	char* for_v_variable_name;
	int   for_v_frame_relative_index;
	int   for_v_type_mask;

	int   for_map_target_frame_relative_index;

} mlr_dsl_cst_statement_t;

mlr_dsl_cst_statement_t* mlr_dsl_cst_statement_valloc(
	mlr_dsl_ast_node_t*                    past_node,
	mlr_dsl_cst_statement_handler_t*       pstatement_handler,
	mlr_dsl_cst_statement_freer_t*         pstatement_freer,
	void*                                  pvstate);

mlr_dsl_cst_statement_t* mlr_dsl_cst_statement_valloc_with_block(
	mlr_dsl_ast_node_t*                    past_node,
	mlr_dsl_cst_statement_handler_t*       pstatement_handler,
	cst_statement_block_t*                 pblock,
	mlr_dsl_cst_block_handler_t*           pblock_handler,
	mlr_dsl_cst_statement_freer_t*         pstatement_freer,
	void*                                  pvstate);

// ----------------------------------------------------------------
// MLR_DSL_CST OBJECT

typedef struct _mlr_dsl_cst_t {
	sllv_t* pbegin_blocks;
	cst_top_level_statement_block_t* pmain_block;
	sllv_t* pend_blocks;

	// Function manager for built-in functions as well as user-defined functions (which are CST-specific).
	fmgr_t* pfmgr;

	// Subroutine bodies
	lhmsv_t* psubr_defsites;

	// Subroutine callsites, used to bootstrap (e.g. subroutine f calls subroutine g before the latter
	// has been defined).
	sllv_t* psubr_callsite_statements_to_resolve;

	// fflush on emit/tee/print/dump
	int flush_every_record;

	// The CST object retains the AST pointer (in order to reuse its strings etc. with minimal copying)
	// and will free the AST in the CST destructor.
	blocked_ast_t* paast;
} mlr_dsl_cst_t;

// ----------------------------------------------------------------
// CONSTRUCTORS/DESTRUCTORS/METHODS

// Notes:
// * do_final_filter is FALSE for mlr put, TRUE for mlr filter.
// * negate_final_filter is TRUE for mlr filter -x.
// * The CST object strips nodes off the raw AST, constructed by the Lemon parser, in order
//   to do analysis on it. Nonetheless the caller should free what's left.
mlr_dsl_cst_t* mlr_dsl_cst_alloc(mlr_dsl_ast_t* past, int print_ast, int trace_stack_allocation,
	int type_inferencing, int flush_every_record, int do_final_filter, int negate_final_filter);

mlr_dsl_cst_statement_t* mlr_dsl_cst_alloc_statement(mlr_dsl_cst_t* pcst, mlr_dsl_ast_node_t* pnode,
	int type_inferencing, int context_flags);

mlr_dsl_cst_statement_t* mlr_dsl_cst_alloc_final_filter_statement(mlr_dsl_cst_t* pcst,
	mlr_dsl_ast_node_t* pnode, int negate_final_filter, int type_inferencing, int context_flags);

void mlr_dsl_cst_free(mlr_dsl_cst_t* pcst);
void mlr_dsl_cst_statement_free(mlr_dsl_cst_statement_t* pstatement);

// Top-level entry point, e.g. from mapper_put.
void mlr_dsl_cst_handle_top_level_statement_blocks(
	sllv_t*      ptop_level_blocks, // block bodies for begins, main, ends
	variables_t* pvars,
	cst_outputs_t* pcst_outputs);

void mlr_dsl_cst_handle_top_level_statement_block(
	cst_top_level_statement_block_t* ptop_level_block,
	variables_t* pvars,
	cst_outputs_t* pcst_outputs);

// Recursive entry point: block bodies for begin, main, end; cond, if, for, while.
void mlr_dsl_cst_handle_statement_block(
	cst_statement_block_t* pblock,
	variables_t*           pvars,
	cst_outputs_t*         pcst_outputs);

void handle_statement_block_with_break_continue( // xxx rename w/ prefix
	cst_statement_block_t* pblock,
	variables_t*           pvars,
	cst_outputs_t*         pcst_outputs);

// Statement lists which are not curly-braced bodies: start/continuation/update statements for triple-for.
void mlr_dsl_cst_handle_statement_list(
	sllv_t*        pstatements,
	variables_t*   pvars,
	cst_outputs_t* pcst_outputs);

// ================================================================
// mapping/mlr_dsl_cst_func_subr.c

// ----------------------------------------------------------------
// cst_udf_state_t is data needed to execute the body of a user-defined function which is implemented by CST statements.
// udf_defsite_state_t is data needed for any user-defined function (no matter how implemented).
typedef struct _cst_udf_state_t {
	char*     name;
	int       arity;
	char**    parameter_names;
	int*      parameter_type_masks;
	cst_top_level_statement_block_t* ptop_level_block;
} cst_udf_state_t;

udf_defsite_state_t* mlr_dsl_cst_alloc_udf(
	mlr_dsl_cst_t*      pcst,
	mlr_dsl_ast_node_t* pnode,
	int                 type_inferencing,
	int                 context_flags);

void mlr_dsl_cst_free_udf(cst_udf_state_t* pstate);

// ----------------------------------------------------------------

typedef struct _subr_callsite_t {
	char* name;
	int   arity;
	int   type_inferencing;
	int   context_flags;
} subr_callsite_t;

typedef struct _subr_defsite_t {
	char*     name;
	int       arity;
	char**    parameter_names;
	int*      parameter_type_masks;
	cst_top_level_statement_block_t* ptop_level_block;
} subr_defsite_t;

subr_defsite_t* mlr_dsl_cst_alloc_subroutine(
	mlr_dsl_cst_t*      pcst,
	mlr_dsl_ast_node_t* pnode,
	int                 type_inferencing,
	int                 context_flags);

void mlr_dsl_cst_free_subroutine(subr_defsite_t* psubr_defsite);

// Invoked directly from the CST statement handler for a subroutine callsite.
// (Functions, by contrast, are invoked by callback from the right-hand-site-evaluator logic
// -- hence no execute-function method here.)
void mlr_dsl_cst_execute_subroutine(subr_defsite_t* pstate, variables_t* pvars,
	cst_outputs_t* pcst_outputs, int callsite_arity, mv_t* args);

// ================================================================
// For on-line help / manpage
// mapping/mlr_dsl_cst_keywords.c

void mlr_dsl_list_all_keywords_raw(FILE* output_stream);

// Pass function_name == NULL to get usage for all keywords:
void mlr_dsl_keyword_usage(FILE* output_stream, char* keyword);

// ================================================================
// xxx comment/reorg ...

//  - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// mapping/mlr_dsl_cst_condish_statements.c
mlr_dsl_cst_statement_allocator_t alloc_conditional_block;
mlr_dsl_cst_statement_allocator_t alloc_if_head;
mlr_dsl_cst_statement_allocator_t alloc_while;
mlr_dsl_cst_statement_allocator_t alloc_do_while;
mlr_dsl_cst_statement_allocator_t alloc_bare_boolean;

mlr_dsl_cst_statement_t* alloc_filter(
	mlr_dsl_cst_t*      pcst,
	mlr_dsl_ast_node_t* pnode,
	int                 type_inferencing,
	int                 context_flags);

mlr_dsl_cst_statement_t* alloc_final_filter(
	mlr_dsl_cst_t*      pcst,
	mlr_dsl_ast_node_t* pnode,
	int                 negate_final_filter,
	int                 type_inferencing,
	int                 context_flags);

//  - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// mapping/mlr_dsl_cst_assignment_statements.c
mlr_dsl_cst_statement_allocator_t alloc_srec_assignment;
mlr_dsl_cst_statement_allocator_t alloc_indirect_srec_assignment;
mlr_dsl_cst_statement_allocator_t alloc_local_non_map_variable_assignment;
mlr_dsl_cst_statement_allocator_t alloc_local_map_variable_assignment;
mlr_dsl_cst_statement_allocator_t alloc_oosvar_assignment;
mlr_dsl_cst_statement_allocator_t alloc_oosvar_from_full_srec_assignment;

//  - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// mapping/mlr_dsl_cst_for_srec_statements.c
mlr_dsl_cst_statement_allocator_t alloc_for_srec;

//  - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// mapping/mlr_dsl_cst_triple_for_statements.c
mlr_dsl_cst_statement_allocator_t alloc_triple_for;

//  - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// mapping/mlr_dsl_cst_loop_control_statements.c
mlr_dsl_cst_statement_allocator_t alloc_break;
mlr_dsl_cst_statement_allocator_t alloc_continue;

//  - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// mapping/mlr_dsl_cst_return_statements.c
mlr_dsl_cst_statement_allocator_t alloc_return_void;  // For subroutines
mlr_dsl_cst_statement_allocator_t alloc_return_value; // For UDFs

//  - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// mapping/mlr_dsl_cst_output_statements.c

mlr_dsl_cst_statement_allocator_t alloc_tee;

mlr_dsl_cst_statement_allocator_t alloc_emitf;

mlr_dsl_cst_statement_t* alloc_emit(
	mlr_dsl_cst_t*      pcst,
	mlr_dsl_ast_node_t* pnode,
	int                 type_inferencing,
	int                 context_flags,
	int                 do_full_prefixing);

mlr_dsl_cst_statement_t* alloc_emit_lashed(
	mlr_dsl_cst_t*      pcst,
	mlr_dsl_ast_node_t* pnode,
	int                 type_inferencing,
	int                 context_flags,
	int                 do_full_prefixing);

mlr_dsl_cst_statement_t* alloc_print(
	mlr_dsl_cst_t*      pcst,
	mlr_dsl_ast_node_t* pnode,
	int                 type_inferencing,
	int                 context_flags,
	char*               print_terminator);

mlr_dsl_cst_statement_allocator_t alloc_dump;

//  - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
// mapping/mlr_dsl_cst_func_subr.c

// When we allocate a callsite we can do so before the callee has been defined.
// Hence the two-step process, with the second step being an object-binding step.
mlr_dsl_cst_statement_allocator_t alloc_subr_callsite_statement;
void mlr_dsl_cst_resolve_subr_callsite(mlr_dsl_cst_t* pcst, mlr_dsl_cst_statement_t* pstatement);

// ----------------------------------------------------------------
mlr_dsl_cst_statement_vararg_t* mlr_dsl_cst_statement_vararg_alloc(
	int               unset_local_variable_frame_relative_index,
	char*             emitf_or_unset_srec_field_name,
	rval_evaluator_t* punset_srec_field_name_evaluator,
	rval_evaluator_t* pemitf_arg_evaluator,
	sllv_t*           punset_oosvar_keylist_evaluators);

void cst_statement_vararg_free(mlr_dsl_cst_statement_vararg_t* pvararg);

sllv_t* allocate_keylist_evaluators_from_oosvar_node(
	mlr_dsl_cst_t*      pcst,
	mlr_dsl_ast_node_t* pnode,
	int                 type_inferencing,
	int                 context_flags);

#endif // MLR_DSL_CST_H
