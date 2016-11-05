#include <stdlib.h>
#include "lib/mlr_globals.h"
#include "lib/mlrutil.h"
#include "mlr_dsl_cst.h"
#include "context_flags.h"

// ================================================================
// The Lemon parser in dsls/mlr_dsl_parse.y builds up an abstract syntax tree
// specifically for the CST builder here.
//
// For clearer visuals on what the ASTs look like:
// * See dsls/mlr_dsl_parse.y
// * See reg_test/run's filter -v and put -v outputs, e.g. in reg_test/expected/out
// * Do "mlr -n put -v 'your expression goes here'"
// ================================================================

// ----------------------------------------------------------------
static mlr_dsl_cst_statement_t* alloc_blank(mlr_dsl_ast_node_t* past_node);
void mlr_dsl_cst_statement_free(mlr_dsl_cst_statement_t* pstatement);

//  - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
static mlr_dsl_cst_statement_allocator_t alloc_unset;

//  - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
static mlr_dsl_cst_statement_allocator_t alloc_for_oosvar;
static mlr_dsl_cst_statement_allocator_t alloc_for_oosvar_key_only;
static mlr_dsl_cst_statement_allocator_t alloc_for_local_map;
static mlr_dsl_cst_statement_allocator_t alloc_for_local_map_key_only;

// ----------------------------------------------------------------
static mlr_dsl_cst_statement_handler_t handle_unset;
static mlr_dsl_cst_statement_handler_t handle_unset_all;

static mlr_dsl_cst_statement_handler_t handle_for_oosvar;
static mlr_dsl_cst_statement_handler_t handle_for_oosvar_key_only;
static mlr_dsl_cst_statement_handler_t handle_for_local_map;
static mlr_dsl_cst_statement_handler_t handle_for_local_map_key_only;

static void handle_for_oosvar_aux(
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs,
	mlhmmv_value_t           submap,
	char**                   prest_for_k_variable_names,
	int*                     prest_for_k_frame_relative_indices,
	int*                     prest_for_k_frame_type_masks,
	int                      prest_for_k_count);

static void handle_for_local_map_aux(
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs,
	mlhmmv_value_t           submap,
	char**                   prest_for_k_variable_names,
	int*                     prest_for_k_frame_relative_indices,
	int*                     prest_for_k_frame_type_masks,
	int                      prest_for_k_count);

static void handle_unset_local_variable(
	mlr_dsl_cst_statement_vararg_t* pvararg,
	variables_t*                    pvars,
	cst_outputs_t*                  pcst_outputs);

static void handle_unset_vararg_oosvar(
	mlr_dsl_cst_statement_vararg_t* pvararg,
	variables_t*                    pvars,
	cst_outputs_t*                  pcst_outputs);

static void handle_unset_vararg_full_srec(
	mlr_dsl_cst_statement_vararg_t* pvararg,
	variables_t*                    pvars,
	cst_outputs_t*                  pcst_outputs);

static void handle_unset_vararg_srec_field_name(
	mlr_dsl_cst_statement_vararg_t* pvararg,
	variables_t*                    pvars,
	cst_outputs_t*                  pcst_outputs);

static void handle_unset_vararg_indirect_srec_field_name(
	mlr_dsl_cst_statement_vararg_t* pvararg,
	variables_t*                    pvars,
	cst_outputs_t*                  pcst_outputs);


// ================================================================
cst_statement_block_t* cst_statement_block_alloc(int subframe_var_count) {
	cst_statement_block_t* pblock = mlr_malloc_or_die(sizeof(cst_statement_block_t));

	pblock->subframe_var_count = subframe_var_count;
	pblock->pstatements     = sllv_alloc();

	return pblock;
}

// ----------------------------------------------------------------
void cst_statement_block_free(cst_statement_block_t* pblock) {
	if (pblock == NULL)
		return;
	for (sllve_t* pe = pblock->pstatements->phead; pe != NULL; pe = pe->pnext) {
		mlr_dsl_cst_statement_free(pe->pvvalue);
	}
	sllv_free(pblock->pstatements);
	free(pblock);
}

// ================================================================
// ALLOCATORS
// ================================================================
cst_top_level_statement_block_t* cst_top_level_statement_block_alloc(int max_var_depth, int subframe_var_count) {
	cst_top_level_statement_block_t* ptop_level_block = mlr_malloc_or_die(sizeof(cst_top_level_statement_block_t));

	ptop_level_block->max_var_depth = max_var_depth;
	ptop_level_block->pframe        = local_stack_frame_alloc(max_var_depth);
	ptop_level_block->pblock        = cst_statement_block_alloc(subframe_var_count);

	return ptop_level_block;
}

// ----------------------------------------------------------------
void cst_top_level_statement_block_free(cst_top_level_statement_block_t* ptop_level_block) {
	if (ptop_level_block == NULL)
		return;
	local_stack_frame_free(ptop_level_block->pframe);
	cst_statement_block_free(ptop_level_block->pblock);
	free(ptop_level_block);
}

// ================================================================
// The parser accepts many things that are invalid, e.g.
// * begin{end{}} -- begin/end not at top level
// * begin{$x=1} -- references to stream records at begin/end
// * break/continue outside of for/while/do-while
// * $x=x -- boundvars outside of for-loop variable bindings
//
// All of the above are enforced here by the CST builder, which takes the parser's output AST as
// input.  This is done (a) to keep the parser from being overly complex, and (b) so we can get much
// more informative error messages in C than in Lemon ('syntax error').
//
// In this file we set up left-hand sides for assignments, as well as right-hand sides for emit and
// unset.  Most right-hand sides are set up in rval_expr_evaluators.c so the context_flags are
// passed through to there as well.

mlr_dsl_cst_statement_t* mlr_dsl_cst_alloc_statement(mlr_dsl_cst_t* pcst, mlr_dsl_ast_node_t* pnode,
	int type_inferencing, int context_flags)
{
	switch(pnode->type) {

	case MD_AST_NODE_TYPE_FUNC_DEF:
		fprintf(stderr, "%s: func statements are only valid at top level.\n", MLR_GLOBALS.bargv0);
		exit(1);
		break;

	case MD_AST_NODE_TYPE_SUBR_DEF:
		fprintf(stderr, "%s: subr statements are only valid at top level.\n", MLR_GLOBALS.bargv0);
		exit(1);
		break;

	case MD_AST_NODE_TYPE_BEGIN:
		fprintf(stderr, "%s: begin statements are only valid at top level.\n", MLR_GLOBALS.bargv0);
		exit(1);
		break;

	case MD_AST_NODE_TYPE_END:
		fprintf(stderr, "%s: end statements are only valid at top level.\n", MLR_GLOBALS.bargv0);
		exit(1);
		break;

	case MD_AST_NODE_TYPE_RETURN_VALUE:
		if (!(context_flags & IN_FUNC_DEF)) {
			fprintf(stderr, "%s: return-value statements are only valid within func blocks.\n",
				MLR_GLOBALS.bargv0);
			exit(1);
		}
		return alloc_return_value(pcst, pnode, type_inferencing, context_flags);
		break;

	case MD_AST_NODE_TYPE_RETURN_VOID:
		if (!(context_flags & IN_SUBR_DEF)) {
			fprintf(stderr, "%s: return-void statements are only valid within subr blocks.\n",
				MLR_GLOBALS.bargv0);
			exit(1);
		}
		return alloc_return_void(pcst, pnode, type_inferencing, context_flags);
		break;

	case MD_AST_NODE_TYPE_SUBR_CALLSITE:
		if (context_flags & IN_FUNC_DEF) {
			fprintf(stderr, "%s: subroutine calls are not valid within func blocks.\n",
				MLR_GLOBALS.bargv0);
			exit(1);
		}
		return alloc_subr_callsite_statement(pcst, pnode, type_inferencing, context_flags);
		break;

	case MD_AST_NODE_TYPE_CONDITIONAL_BLOCK:
		return alloc_conditional_block(pcst, pnode, type_inferencing, context_flags);
		break;
	case MD_AST_NODE_TYPE_IF_HEAD:
		return alloc_if_head(pcst, pnode, type_inferencing, context_flags);
		break;

	case MD_AST_NODE_TYPE_WHILE:
		return alloc_while(pcst, pnode, type_inferencing, context_flags | IN_BREAKABLE);
		break;
	case MD_AST_NODE_TYPE_DO_WHILE:
		return alloc_do_while(pcst, pnode, type_inferencing, context_flags | IN_BREAKABLE);
		break;

	case MD_AST_NODE_TYPE_FOR_SREC:
		if (context_flags & IN_BEGIN_OR_END) {
			fprintf(stderr, "%s: statements involving $-variables are not valid within begin or end blocks.\n",
				MLR_GLOBALS.bargv0);
			exit(1);
		}
		return alloc_for_srec(pcst, pnode, type_inferencing, context_flags | IN_BREAKABLE);
		break;

	case MD_AST_NODE_TYPE_FOR_OOSVAR:
		return alloc_for_oosvar(pcst, pnode, type_inferencing, context_flags | IN_BREAKABLE);
		break;
	case MD_AST_NODE_TYPE_FOR_OOSVAR_KEY_ONLY:
		return alloc_for_oosvar_key_only(pcst, pnode, type_inferencing, context_flags | IN_BREAKABLE);
		break;

	case MD_AST_NODE_TYPE_FOR_LOCAL_MAP:
		return alloc_for_local_map(pcst, pnode, type_inferencing, context_flags | IN_BREAKABLE);
		break;
	case MD_AST_NODE_TYPE_FOR_LOCAL_MAP_KEY_ONLY:
		return alloc_for_local_map_key_only(pcst, pnode, type_inferencing, context_flags | IN_BREAKABLE);
		break;

	case MD_AST_NODE_TYPE_TRIPLE_FOR:
		return alloc_triple_for(pcst, pnode, type_inferencing, context_flags | IN_BREAKABLE);
		break;

	case MD_AST_NODE_TYPE_BREAK:
		if (!(context_flags & IN_BREAKABLE)) {
			fprintf(stderr, "%s: break statements are only valid within for, while, or do-while.\n",
				MLR_GLOBALS.bargv0);
			exit(1);
		}
		return alloc_break(pcst, pnode, type_inferencing, context_flags);
		break;
	case MD_AST_NODE_TYPE_CONTINUE:
		if (!(context_flags & IN_BREAKABLE)) {
			fprintf(stderr, "%s: break statements are only valid within for, while, or do-while.\n",
				MLR_GLOBALS.bargv0);
			exit(1);
		}
		return alloc_continue(pcst, pnode, type_inferencing, context_flags);
		break;

	case MD_AST_NODE_TYPE_UNTYPED_LOCAL_DEFINITION:
		return alloc_local_variable_definition(pcst, pnode, type_inferencing, context_flags, TYPE_MASK_ANY);

	case MD_AST_NODE_TYPE_NUMERIC_LOCAL_DEFINITION:
		return alloc_local_variable_definition(pcst, pnode, type_inferencing, context_flags, TYPE_MASK_NUMERIC);
		break;

	case MD_AST_NODE_TYPE_INT_LOCAL_DEFINITION:
		return alloc_local_variable_definition(pcst, pnode, type_inferencing, context_flags, TYPE_MASK_INT);
		break;

	case MD_AST_NODE_TYPE_FLOAT_LOCAL_DEFINITION:
		return alloc_local_variable_definition(pcst, pnode, type_inferencing, context_flags, TYPE_MASK_FLOAT);
		break;

	case MD_AST_NODE_TYPE_BOOLEAN_LOCAL_DEFINITION:
		return alloc_local_variable_definition(pcst, pnode, type_inferencing, context_flags, TYPE_MASK_BOOLEAN);
		break;

	case MD_AST_NODE_TYPE_STRING_LOCAL_DEFINITION:
		return alloc_local_variable_definition(pcst, pnode, type_inferencing, context_flags, TYPE_MASK_STRING);
		break;

	case MD_AST_NODE_TYPE_MAP_LOCAL_DECLARATION:
		return alloc_local_variable_definition(pcst, pnode, type_inferencing, context_flags, TYPE_MASK_MAP);
		break;

	case MD_AST_NODE_TYPE_LOCAL_NON_MAP_ASSIGNMENT:
		return alloc_local_non_map_variable_assignment(pcst, pnode, type_inferencing, context_flags);
		break;

	case MD_AST_NODE_TYPE_LOCAL_MAP_ASSIGNMENT:
		return alloc_local_map_variable_assignment(pcst, pnode, type_inferencing, context_flags);
		break;

	case MD_AST_NODE_TYPE_SREC_ASSIGNMENT:
		if (context_flags & IN_BEGIN_OR_END) {
			fprintf(stderr, "%s: assignments to $-variables are not valid within begin or end blocks.\n",
				MLR_GLOBALS.bargv0);
			exit(1);
		}
		if (context_flags & IN_FUNC_DEF) {
			fprintf(stderr, "%s: assignments to $-variables are not valid within func blocks.\n",
				MLR_GLOBALS.bargv0);
			exit(1);
		}
		return alloc_srec_assignment(pcst, pnode, type_inferencing, context_flags);
		break;

	case MD_AST_NODE_TYPE_INDIRECT_SREC_ASSIGNMENT:
		if (context_flags & IN_BEGIN_OR_END) {
			fprintf(stderr, "%s: assignments to $-variables are not valid within begin or end blocks.\n",
				MLR_GLOBALS.bargv0);
			exit(1);
		}
		if (context_flags & IN_FUNC_DEF) {
			fprintf(stderr, "%s: assignments to $-variables are not valid within func blocks.\n",
				MLR_GLOBALS.bargv0);
			exit(1);
		}
		return alloc_indirect_srec_assignment(pcst, pnode, type_inferencing, context_flags);
		break;

	case MD_AST_NODE_TYPE_OOSVAR_ASSIGNMENT:
		return alloc_oosvar_assignment(pcst, pnode, type_inferencing, context_flags);
		break;

	case MD_AST_NODE_TYPE_OOSVAR_FROM_FULL_SREC_ASSIGNMENT:
		if (context_flags & IN_BEGIN_OR_END) {
			fprintf(stderr, "%s: assignments from $-variables are not valid within begin or end blocks.\n",
				MLR_GLOBALS.bargv0);
			exit(1);
		}
		return alloc_oosvar_from_full_srec_assignment(pcst, pnode, type_inferencing, context_flags);
		break;

	case MD_AST_NODE_TYPE_FULL_SREC_FROM_OOSVAR_ASSIGNMENT:
		if (context_flags & IN_BEGIN_OR_END) {
			fprintf(stderr, "%s: assignments to $-variables are not valid within begin or end blocks.\n",
				MLR_GLOBALS.bargv0);
			exit(1);
		}
		if (context_flags & IN_FUNC_DEF) {
			fprintf(stderr, "%s: assignments to $-variables are not valid within func blocks.\n",
				MLR_GLOBALS.bargv0);
			exit(1);
		}
		return alloc_full_srec_from_oosvar_assignment(pcst, pnode, type_inferencing, context_flags);
		break;

	case MD_AST_NODE_TYPE_ENV_ASSIGNMENT:
		return alloc_env_assignment(pcst, pnode, type_inferencing, context_flags);
		break;

	case MD_AST_NODE_TYPE_UNSET:
		if (context_flags & IN_FUNC_DEF) {
			fprintf(stderr, "%s: unset statements are not valid within func blocks.\n",
				MLR_GLOBALS.bargv0);
			exit(1);
		}
		return alloc_unset(pcst, pnode, type_inferencing, context_flags);
		break;

	case MD_AST_NODE_TYPE_TEE:
		if (context_flags & IN_FUNC_DEF) {
			fprintf(stderr, "%s: tee statements are not valid within func blocks.\n",
				MLR_GLOBALS.bargv0);
			exit(1);
		}
		return alloc_tee(pcst, pnode, type_inferencing, context_flags);
		break;

	case MD_AST_NODE_TYPE_EMITF:
		if (context_flags & IN_FUNC_DEF) {
			fprintf(stderr, "%s: emitf statements are not valid within func blocks.\n",
				MLR_GLOBALS.bargv0);
			exit(1);
		}
		return alloc_emitf(pcst, pnode, type_inferencing, context_flags);
		break;
	case MD_AST_NODE_TYPE_EMITP:
		if (context_flags & IN_FUNC_DEF) {
			fprintf(stderr, "%s: emitp statements are not valid within func blocks.\n",
				MLR_GLOBALS.bargv0);
			exit(1);
		}
		return alloc_emit(pcst, pnode, type_inferencing, context_flags, TRUE);
		break;
	case MD_AST_NODE_TYPE_EMIT:
		if (context_flags & IN_FUNC_DEF) {
			fprintf(stderr, "%s: emit statements are not valid within func blocks.\n",
				MLR_GLOBALS.bargv0);
			exit(1);
		}
		return alloc_emit(pcst, pnode, type_inferencing, context_flags, FALSE);
		break;

	case MD_AST_NODE_TYPE_EMITP_LASHED:
		if (context_flags & IN_FUNC_DEF) {
			fprintf(stderr, "%s: emitp statements are not valid within func blocks.\n",
				MLR_GLOBALS.bargv0);
			exit(1);
		}
		return alloc_emit_lashed(pcst, pnode, type_inferencing, context_flags, TRUE);
		break;
	case MD_AST_NODE_TYPE_EMIT_LASHED:
		if (context_flags & IN_FUNC_DEF) {
			fprintf(stderr, "%s: emit statements are not valid within func blocks.\n",
				MLR_GLOBALS.bargv0);
			exit(1);
		}
		return alloc_emit_lashed(pcst, pnode, type_inferencing, context_flags, FALSE);
		break;

	case MD_AST_NODE_TYPE_FILTER:
		if (context_flags & IN_FUNC_DEF) {
			fprintf(stderr, "%s: filter statements are not valid within func blocks.\n",
				MLR_GLOBALS.bargv0);
			exit(1);
		}
		if (context_flags & IN_MLR_FILTER) {
			fprintf(stderr, "%s filter: expressions must not also contain the \"filter\" keyword.\n",
				MLR_GLOBALS.bargv0);
			exit(1);
		}
		return alloc_filter(pcst, pnode, type_inferencing, context_flags);
		break;

	case MD_AST_NODE_TYPE_DUMP:
		return alloc_dump(pcst, pnode, type_inferencing, context_flags);
		break;

	case MD_AST_NODE_TYPE_PRINT:
		return alloc_print(pcst, pnode, type_inferencing, context_flags, "\n");
		break;

	case MD_AST_NODE_TYPE_PRINTN:
		return alloc_print(pcst, pnode, type_inferencing, context_flags, "");
		break;

	default:
		return alloc_bare_boolean(pcst, pnode, type_inferencing, context_flags);
		break;
	}
}

// ----------------------------------------------------------------
// mlr put and mlr filter are almost entirely the same code. The key difference is that the final
// statement for the latter must be a bare boolean expression.

mlr_dsl_cst_statement_t* mlr_dsl_cst_alloc_final_filter_statement(mlr_dsl_cst_t* pcst, mlr_dsl_ast_node_t* pnode,
	int negate_final_filter, int type_inferencing, int context_flags)
{
	switch(pnode->type) {

	case MD_AST_NODE_TYPE_FILTER:
		fprintf(stderr, "%s filter: expressions must not also contain the \"filter\" keyword.\n",
			MLR_GLOBALS.bargv0);
		exit(1);
		break;

	case MD_AST_NODE_TYPE_FUNC_DEF:
	case MD_AST_NODE_TYPE_SUBR_DEF:
	case MD_AST_NODE_TYPE_BEGIN:
	case MD_AST_NODE_TYPE_END:
	case MD_AST_NODE_TYPE_RETURN_VALUE:
	case MD_AST_NODE_TYPE_RETURN_VOID:
	case MD_AST_NODE_TYPE_SUBR_CALLSITE:
	case MD_AST_NODE_TYPE_CONDITIONAL_BLOCK:
	case MD_AST_NODE_TYPE_IF_HEAD:
	case MD_AST_NODE_TYPE_WHILE:
	case MD_AST_NODE_TYPE_DO_WHILE:
	case MD_AST_NODE_TYPE_FOR_SREC:
	case MD_AST_NODE_TYPE_FOR_OOSVAR:
	case MD_AST_NODE_TYPE_TRIPLE_FOR:
	case MD_AST_NODE_TYPE_BREAK:
	case MD_AST_NODE_TYPE_CONTINUE:
	case MD_AST_NODE_TYPE_UNTYPED_LOCAL_DEFINITION:
	case MD_AST_NODE_TYPE_NUMERIC_LOCAL_DEFINITION:
	case MD_AST_NODE_TYPE_INT_LOCAL_DEFINITION:
	case MD_AST_NODE_TYPE_FLOAT_LOCAL_DEFINITION:
	case MD_AST_NODE_TYPE_BOOLEAN_LOCAL_DEFINITION:
	case MD_AST_NODE_TYPE_STRING_LOCAL_DEFINITION:
	case MD_AST_NODE_TYPE_LOCAL_NON_MAP_ASSIGNMENT:
	case MD_AST_NODE_TYPE_SREC_ASSIGNMENT:
	case MD_AST_NODE_TYPE_INDIRECT_SREC_ASSIGNMENT:
	case MD_AST_NODE_TYPE_OOSVAR_ASSIGNMENT:
	case MD_AST_NODE_TYPE_OOSVAR_FROM_FULL_SREC_ASSIGNMENT:
	case MD_AST_NODE_TYPE_FULL_SREC_FROM_OOSVAR_ASSIGNMENT:
	case MD_AST_NODE_TYPE_UNSET:
	case MD_AST_NODE_TYPE_TEE:
	case MD_AST_NODE_TYPE_EMITF:
	case MD_AST_NODE_TYPE_EMITP:
	case MD_AST_NODE_TYPE_EMIT:
	case MD_AST_NODE_TYPE_EMITP_LASHED:
	case MD_AST_NODE_TYPE_EMIT_LASHED:
	case MD_AST_NODE_TYPE_DUMP:
	case MD_AST_NODE_TYPE_PRINT:
	case MD_AST_NODE_TYPE_PRINTN:
		fprintf(stderr, "%s: filter expressions must end in a final boolean statement.\n", MLR_GLOBALS.bargv0);
		exit(1);
		break;

	default:
		return alloc_final_filter(pcst, pnode, negate_final_filter, type_inferencing, context_flags);
		break;
	}
}

// ----------------------------------------------------------------
static mlr_dsl_cst_statement_t* alloc_blank(mlr_dsl_ast_node_t* past_node) {
	mlr_dsl_cst_statement_t* pstatement = mlr_malloc_or_die(sizeof(mlr_dsl_cst_statement_t));

	// xxx post-federation
	pstatement->past_node           = past_node;
	pstatement->pstatement_handler  = NULL;
	pstatement->pstatement_freer    = NULL;
	pstatement->pblock_handler      = NULL;
	pstatement->pvstate             = FALSE;

	// xxx pre-federation

	pstatement->poosvar_target_keylist_evaluators   = NULL;
	pstatement->local_lhs_variable_name             = 0;
	pstatement->local_lhs_frame_relative_index      = 0;
	pstatement->local_lhs_type_mask                 = TYPE_MASK_ANY;
	pstatement->psrec_lhs_evaluator                 = NULL;
	pstatement->prhs_evaluator                      = NULL;
	pstatement->poosvar_rhs_keylist_evaluators      = NULL;
	pstatement->pvarargs                            = NULL;
	pstatement->pblock                              = NULL;

	pstatement->for_map_k_variable_names            = NULL;
	pstatement->for_map_k_frame_relative_indices    = NULL;
	pstatement->for_map_k_type_masks                = NULL;
	pstatement->for_map_k_count                     = 0;
	pstatement->for_v_variable_name                 = NULL;
	pstatement->for_v_frame_relative_index          = 0;
	pstatement->for_v_type_mask                     = TYPE_MASK_ANY;
	pstatement->for_map_target_frame_relative_index = 0;

	return pstatement;
}

// ----------------------------------------------------------------
// xxx post-federation

mlr_dsl_cst_statement_t* mlr_dsl_cst_statement_valloc(
	mlr_dsl_ast_node_t*              past_node,
	mlr_dsl_cst_statement_handler_t* pstatement_handler,
	mlr_dsl_cst_statement_freer_t*   pstatement_freer,
	void*                            pvstate)
{
	mlr_dsl_cst_statement_t* pstatement = mlr_malloc_or_die(sizeof(mlr_dsl_cst_statement_t));
	memset(pstatement, 0, sizeof(*pstatement)); // xxx temp
	pstatement->past_node           = past_node;
	pstatement->pstatement_handler  = pstatement_handler;
	pstatement->pblock              = NULL;
	pstatement->pblock_handler      = NULL;
	pstatement->pstatement_freer    = pstatement_freer;
	pstatement->pvstate             = pvstate;
	return pstatement;
}

mlr_dsl_cst_statement_t* mlr_dsl_cst_statement_valloc_with_block(
	mlr_dsl_ast_node_t*              past_node,
	mlr_dsl_cst_statement_handler_t* pstatement_handler,
	cst_statement_block_t*           pblock,
	mlr_dsl_cst_block_handler_t*     pblock_handler,
	mlr_dsl_cst_statement_freer_t*   pstatement_freer,
	void*                            pvstate)
{
	mlr_dsl_cst_statement_t* pstatement = mlr_malloc_or_die(sizeof(mlr_dsl_cst_statement_t));
	memset(pstatement, 0, sizeof(*pstatement)); // xxx temp
	pstatement->past_node           = past_node;
	pstatement->pstatement_handler  = pstatement_handler;
	pstatement->pblock              = pblock;
	pstatement->pblock_handler      = pblock_handler;
	pstatement->pstatement_freer    = pstatement_freer;
	pstatement->pvstate             = pvstate;
	return pstatement;
}

// ----------------------------------------------------------------
static mlr_dsl_cst_statement_t* alloc_unset(mlr_dsl_cst_t* pcst, mlr_dsl_ast_node_t* pnode,
	int type_inferencing, int context_flags)
{
	mlr_dsl_cst_statement_t* pstatement = alloc_blank(pnode);

	pstatement->pstatement_handler = handle_unset;
	pstatement->pvarargs = sllv_alloc();
	for (sllve_t* pe = pnode->pchildren->phead; pe != NULL; pe = pe->pnext) {
		mlr_dsl_ast_node_t* pnode = pe->pvvalue;

		if (pnode->type == MD_AST_NODE_TYPE_ALL || pnode->type == MD_AST_NODE_TYPE_FULL_OOSVAR) {
			// The grammar allows only 'unset all', not 'unset @x, all, $y'.
			// So if 'all' appears at all, it's the only name. Likewise with '@*'.
			pstatement->pstatement_handler = handle_unset_all;

		} else if (pnode->type == MD_AST_NODE_TYPE_FULL_SREC) {
			if (context_flags & IN_BEGIN_OR_END) {
				fprintf(stderr, "%s: unset of $-variables is not valid within begin or end blocks.\n",
					MLR_GLOBALS.bargv0);
				exit(1);
			}
			sllv_append(pstatement->pvarargs, mlr_dsl_cst_statement_vararg_alloc(
				MD_UNUSED_INDEX,
				NULL,
				NULL,
				NULL,
				NULL));

		} else if (pnode->type == MD_AST_NODE_TYPE_FIELD_NAME) {
			if (context_flags & IN_BEGIN_OR_END) {
				fprintf(stderr, "%s: unset of $-variables is not valid within begin or end blocks.\n",
					MLR_GLOBALS.bargv0);
				exit(1);
			}
			sllv_append(pstatement->pvarargs, mlr_dsl_cst_statement_vararg_alloc(
				MD_UNUSED_INDEX,
				pnode->text,
				NULL,
				NULL,
				NULL));

		} else if (pnode->type == MD_AST_NODE_TYPE_INDIRECT_FIELD_NAME) {
			if (context_flags & IN_BEGIN_OR_END) {
				fprintf(stderr, "%s: unset of $-variables are not valid within begin or end blocks.\n",
					MLR_GLOBALS.bargv0);
				exit(1);
			}
			sllv_append(pstatement->pvarargs, mlr_dsl_cst_statement_vararg_alloc(
				MD_UNUSED_INDEX,
				NULL,
				rval_evaluator_alloc_from_ast(pnode->pchildren->phead->pvvalue, pcst->pfmgr,
					type_inferencing, context_flags),
				NULL,
				NULL));

		} else if (pnode->type == MD_AST_NODE_TYPE_OOSVAR_KEYLIST) {
			sllv_append(pstatement->pvarargs, mlr_dsl_cst_statement_vararg_alloc(
				MD_UNUSED_INDEX,
				NULL,
				NULL,
				NULL,
				allocate_keylist_evaluators_from_oosvar_node(pcst, pnode,
					type_inferencing, context_flags)));

		} else if (pnode->type == MD_AST_NODE_TYPE_LOCAL_NON_MAP_VARIABLE) {
			MLR_INTERNAL_CODING_ERROR_IF(pnode->vardef_frame_relative_index == MD_UNUSED_INDEX);
			sllv_append(pstatement->pvarargs, mlr_dsl_cst_statement_vararg_alloc(
				pnode->vardef_frame_relative_index,
				NULL,
				NULL,
				NULL,
				NULL));

		} else {
			MLR_INTERNAL_CODING_ERROR();
		}
	}
	return pstatement;
}

// ----------------------------------------------------------------
// $ mlr -n put -v 'for((k1,k2,k3),v in @a["4"][$5]) { $6 = 7; $8 = 9}'
// AST ROOT:
// text="list", type=statement_list:
//     text="for", type=for_oosvar:
//         text="key_and_value_variables", type=for_variables:
//             text="key_variables", type=for_variables:
//                 text="k1", type=non_sigil_name.
//                 text="k2", type=non_sigil_name.
//                 text="k3", type=non_sigil_name.
//             text="v", type=non_sigil_name.
//         text="oosvar_keylist", type=oosvar_keylist:
//             text="a", type=string_literal.
//             text="4", type=numeric_literal.
//             text="5", type=field_name.
//         text="list", type=statement_list:
//             text="=", type=srec_assignment:
//                 text="6", type=field_name.
//                 text="7", type=numeric_literal.
//             text="=", type=srec_assignment:
//                 text="8", type=field_name.
//                 text="9", type=numeric_literal.

static mlr_dsl_cst_statement_t* alloc_for_oosvar(mlr_dsl_cst_t* pcst, mlr_dsl_ast_node_t* pnode,
	int type_inferencing, int context_flags)
{
	mlr_dsl_cst_statement_t* pstatement = alloc_blank(pnode);

	// Left child node is list of bound variables.
	//   Left subnode is namelist for key boundvars.
	//   Right subnode is name for value boundvar.
	// Middle child node is keylist for basepoint in the oosvar mlhmmv.
	// Right child node is the list of statements in the body.
	mlr_dsl_ast_node_t* pleft     = pnode->pchildren->phead->pvvalue;
	mlr_dsl_ast_node_t* psubleft  = pleft->pchildren->phead->pvvalue;
	mlr_dsl_ast_node_t* psubright = pleft->pchildren->phead->pnext->pvvalue;
	mlr_dsl_ast_node_t* pmiddle   = pnode->pchildren->phead->pnext->pvvalue;
	mlr_dsl_ast_node_t* pright    = pnode->pchildren->phead->pnext->pnext->pvvalue;

	pstatement->for_map_k_variable_names = mlr_malloc_or_die(sizeof(char*) * psubleft->pchildren->length);
	pstatement->for_map_k_frame_relative_indices = mlr_malloc_or_die(sizeof(int) * psubleft->pchildren->length);
	pstatement->for_map_k_type_masks = mlr_malloc_or_die(sizeof(int) * psubleft->pchildren->length);
	pstatement->for_map_k_count = 0;
	for (sllve_t* pe = psubleft->pchildren->phead; pe != NULL; pe = pe->pnext) {
		mlr_dsl_ast_node_t* pnamenode = pe->pvvalue;
		MLR_INTERNAL_CODING_ERROR_IF(pnamenode->vardef_frame_relative_index == MD_UNUSED_INDEX);
		pstatement->for_map_k_variable_names[pstatement->for_map_k_count] =
			mlr_strdup_or_die(pnamenode->text);
		pstatement->for_map_k_frame_relative_indices[pstatement->for_map_k_count] =
			pnamenode->vardef_frame_relative_index;
		pstatement->for_map_k_type_masks[pstatement->for_map_k_count] =
			mlr_dsl_ast_node_type_to_type_mask(pnamenode->type);
		pstatement->for_map_k_count++;
	}
	pstatement->for_v_variable_name = mlr_strdup_or_die(psubright->text);
	MLR_INTERNAL_CODING_ERROR_IF(psubright->vardef_frame_relative_index == MD_UNUSED_INDEX);
	pstatement->for_v_frame_relative_index = psubright->vardef_frame_relative_index;
	pstatement->for_v_type_mask = mlr_dsl_ast_node_type_to_type_mask(psubright->type);

	pstatement->poosvar_target_keylist_evaluators = allocate_keylist_evaluators_from_oosvar_node(
		pcst, pmiddle, type_inferencing, context_flags);

	MLR_INTERNAL_CODING_ERROR_IF(pnode->subframe_var_count == MD_UNUSED_INDEX);
	pstatement->pblock = cst_statement_block_alloc(pnode->subframe_var_count);

	for (sllve_t* pe = pright->pchildren->phead; pe != NULL; pe = pe->pnext) {
		mlr_dsl_ast_node_t* pbody_ast_node = pe->pvvalue;
		sllv_append(pstatement->pblock->pstatements, mlr_dsl_cst_alloc_statement(pcst, pbody_ast_node,
			type_inferencing, context_flags));
	}
	pstatement->pstatement_handler = handle_for_oosvar;
	pstatement->pblock_handler = handle_statement_block_with_break_continue;

	return pstatement;
}

static mlr_dsl_cst_statement_t* alloc_for_oosvar_key_only(mlr_dsl_cst_t* pcst, mlr_dsl_ast_node_t* pnode,
	int type_inferencing, int context_flags)
{
	mlr_dsl_cst_statement_t* pstatement = alloc_blank(pnode);

	// Left child node is single bound variable
	// Middle child node is keylist for basepoint in the oosvar mlhmmv.
	// Right child node is the list of statements in the body.
	mlr_dsl_ast_node_t* pleft     = pnode->pchildren->phead->pvvalue;
	mlr_dsl_ast_node_t* pmiddle   = pnode->pchildren->phead->pnext->pvvalue;
	mlr_dsl_ast_node_t* pright    = pnode->pchildren->phead->pnext->pnext->pvvalue;

	pstatement->for_map_k_variable_names = mlr_malloc_or_die(sizeof(char*));
	pstatement->for_map_k_frame_relative_indices = mlr_malloc_or_die(sizeof(int));
	pstatement->for_map_k_type_masks = mlr_malloc_or_die(sizeof(int));
	MLR_INTERNAL_CODING_ERROR_IF(pleft->vardef_frame_relative_index == MD_UNUSED_INDEX);
	pstatement->for_map_k_variable_names[0] = mlr_strdup_or_die(pleft->text);
	pstatement->for_map_k_frame_relative_indices[0] = pleft->vardef_frame_relative_index;
	pstatement->for_map_k_type_masks[0] = mlr_dsl_ast_node_type_to_type_mask(pleft->type);

	pstatement->poosvar_target_keylist_evaluators = allocate_keylist_evaluators_from_oosvar_node(
		pcst, pmiddle, type_inferencing, context_flags);

	MLR_INTERNAL_CODING_ERROR_IF(pnode->subframe_var_count == MD_UNUSED_INDEX);
	pstatement->pblock = cst_statement_block_alloc(pnode->subframe_var_count);

	for (sllve_t* pe = pright->pchildren->phead; pe != NULL; pe = pe->pnext) {
		mlr_dsl_ast_node_t* pbody_ast_node = pe->pvvalue;
		sllv_append(pstatement->pblock->pstatements, mlr_dsl_cst_alloc_statement(pcst, pbody_ast_node,
			type_inferencing, context_flags));
	}
	pstatement->pstatement_handler = handle_for_oosvar_key_only;
	pstatement->pblock_handler = handle_statement_block_with_break_continue;

	return pstatement;
}

// ----------------------------------------------------------------
static mlr_dsl_cst_statement_t* alloc_for_local_map(mlr_dsl_cst_t* pcst, mlr_dsl_ast_node_t* pnode,
	int type_inferencing, int context_flags)
{
	mlr_dsl_cst_statement_t* pstatement = alloc_blank(pnode);

	// Left child node is list of bound variables.
	//   Left subnode is namelist for key boundvars.
	//   Right subnode is name for value boundvar.
	// Middle child node is keylist for basepoint in the local mlhmmv.
	// Right child node is the list of statements in the body.
	mlr_dsl_ast_node_t* pleft     = pnode->pchildren->phead->pvvalue;
	mlr_dsl_ast_node_t* psubleft  = pleft->pchildren->phead->pvvalue;
	mlr_dsl_ast_node_t* psubright = pleft->pchildren->phead->pnext->pvvalue;
	mlr_dsl_ast_node_t* pmiddle   = pnode->pchildren->phead->pnext->pvvalue;
	mlr_dsl_ast_node_t* pright    = pnode->pchildren->phead->pnext->pnext->pvvalue;

	pstatement->for_map_k_variable_names = mlr_malloc_or_die(sizeof(char*) * psubleft->pchildren->length);
	pstatement->for_map_k_frame_relative_indices = mlr_malloc_or_die(sizeof(int) * psubleft->pchildren->length);
	pstatement->for_map_k_type_masks = mlr_malloc_or_die(sizeof(int) * psubleft->pchildren->length);
	pstatement->for_map_k_count = 0;
	for (sllve_t* pe = psubleft->pchildren->phead; pe != NULL; pe = pe->pnext) {
		mlr_dsl_ast_node_t* pnamenode = pe->pvvalue;
		MLR_INTERNAL_CODING_ERROR_IF(pnamenode->vardef_frame_relative_index == MD_UNUSED_INDEX);
		pstatement->for_map_k_variable_names[pstatement->for_map_k_count] =
			mlr_strdup_or_die(pnamenode->text);
		pstatement->for_map_k_frame_relative_indices[pstatement->for_map_k_count] =
			pnamenode->vardef_frame_relative_index;
		pstatement->for_map_k_type_masks[pstatement->for_map_k_count] =
			mlr_dsl_ast_node_type_to_type_mask(pnamenode->type);
		pstatement->for_map_k_count++;
	}
	pstatement->for_v_variable_name = mlr_strdup_or_die(psubright->text);
	MLR_INTERNAL_CODING_ERROR_IF(psubright->vardef_frame_relative_index == MD_UNUSED_INDEX);
	pstatement->for_v_frame_relative_index = psubright->vardef_frame_relative_index;
	pstatement->for_v_type_mask = mlr_dsl_ast_node_type_to_type_mask(psubright->type);

	// xxx comment liberally
	MLR_INTERNAL_CODING_ERROR_IF(pmiddle->vardef_frame_relative_index == MD_UNUSED_INDEX);
	pstatement->for_map_target_frame_relative_index = pmiddle->vardef_frame_relative_index;
	pstatement->poosvar_target_keylist_evaluators = allocate_keylist_evaluators_from_oosvar_node( // xxx rename x 2
		pcst, pmiddle, type_inferencing, context_flags);

	MLR_INTERNAL_CODING_ERROR_IF(pnode->subframe_var_count == MD_UNUSED_INDEX);
	pstatement->pblock = cst_statement_block_alloc(pnode->subframe_var_count);

	for (sllve_t* pe = pright->pchildren->phead; pe != NULL; pe = pe->pnext) {
		mlr_dsl_ast_node_t* pbody_ast_node = pe->pvvalue;
		sllv_append(pstatement->pblock->pstatements, mlr_dsl_cst_alloc_statement(pcst, pbody_ast_node,
			type_inferencing, context_flags));
	}
	pstatement->pstatement_handler = handle_for_local_map;
	pstatement->pblock_handler = handle_statement_block_with_break_continue;

	return pstatement;
}

static mlr_dsl_cst_statement_t* alloc_for_local_map_key_only(mlr_dsl_cst_t* pcst, mlr_dsl_ast_node_t* pnode,
	int type_inferencing, int context_flags)
{
	mlr_dsl_cst_statement_t* pstatement = alloc_blank(pnode);

	// Left child node is single bound variable
	// Middle child node is keylist for basepoint in the oosvar mlhmmv.
	// Right child node is the list of statements in the body.
	mlr_dsl_ast_node_t* pleft     = pnode->pchildren->phead->pvvalue;
	mlr_dsl_ast_node_t* pmiddle   = pnode->pchildren->phead->pnext->pvvalue;
	mlr_dsl_ast_node_t* pright    = pnode->pchildren->phead->pnext->pnext->pvvalue;

	pstatement->for_map_k_variable_names = mlr_malloc_or_die(sizeof(char*));
	pstatement->for_map_k_frame_relative_indices = mlr_malloc_or_die(sizeof(int));
	pstatement->for_map_k_type_masks = mlr_malloc_or_die(sizeof(int));
	MLR_INTERNAL_CODING_ERROR_IF(pleft->vardef_frame_relative_index == MD_UNUSED_INDEX);
	pstatement->for_map_k_variable_names[0] = mlr_strdup_or_die(pleft->text);
	pstatement->for_map_k_frame_relative_indices[0] = pleft->vardef_frame_relative_index;
	pstatement->for_map_k_type_masks[0] = mlr_dsl_ast_node_type_to_type_mask(pleft->type);

	// xxx comment liberally
	MLR_INTERNAL_CODING_ERROR_IF(pmiddle->vardef_frame_relative_index == MD_UNUSED_INDEX);
	pstatement->for_map_target_frame_relative_index = pmiddle->vardef_frame_relative_index;
	pstatement->poosvar_target_keylist_evaluators = allocate_keylist_evaluators_from_oosvar_node( // xxx rename x 2
		pcst, pmiddle, type_inferencing, context_flags);

	MLR_INTERNAL_CODING_ERROR_IF(pnode->subframe_var_count == MD_UNUSED_INDEX);
	pstatement->pblock = cst_statement_block_alloc(pnode->subframe_var_count);

	for (sllve_t* pe = pright->pchildren->phead; pe != NULL; pe = pe->pnext) {
		mlr_dsl_ast_node_t* pbody_ast_node = pe->pvvalue;
		sllv_append(pstatement->pblock->pstatements, mlr_dsl_cst_alloc_statement(pcst, pbody_ast_node,
			type_inferencing, context_flags));
	}
	pstatement->pstatement_handler = handle_for_local_map_key_only;
	pstatement->pblock_handler = handle_statement_block_with_break_continue;

	return pstatement;
}

// ----------------------------------------------------------------
// Example ASTs, with and without indexing on the left-hand-side oosvar name:

// $ mlr -n put -v '@x[1]["2"][$3][@4]=5'
// AST ROOT:
// text="list", type=statement_list:
//     text="=", type=oosvar_assignment:
//         text="oosvar_keylist", type=oosvar_keylist:
//             text="x", type=string_literal.
//             text="1", type=numeric_literal.
//             text="2", type=numeric_literal.
//             text="3", type=field_name.
//             text="oosvar_keylist", type=oosvar_keylist:
//                 text="4", type=string_literal.
//         text="5", type=numeric_literal.
//
// $ mlr -n put -v '@x = $y'
// AST ROOT:
// text="list", type=statement_list:
//     text="=", type=oosvar_assignment:
//         text="oosvar_keylist", type=oosvar_keylist:
//             text="x", type=string_literal.
//         text="y", type=field_name.
//
// $ mlr -n put -q -v 'emit @v, "a", "b", "c"'
// AST ROOT:
// text="list", type=statement_list:
//     text="emit", type=emit:
//         text="emit", type=emit:
//             text="oosvar_keylist", type=oosvar_keylist:
//                 text="v", type=string_literal.
//             text="emit_namelist", type=emit:
//                 text="a", type=numeric_literal.
//                 text="b", type=numeric_literal.
//                 text="c", type=numeric_literal.
//         text="stream", type=stream:
//
// $ mlr -n put -q -v 'emit @v[1][2], "a", "b","c"'
// AST ROOT:
// text="list", type=statement_list:
//     text="emit", type=emit:
//         text="emit", type=emit:
//             text="oosvar_keylist", type=oosvar_keylist:
//                 text="v", type=string_literal.
//                 text="1", type=numeric_literal.
//                 text="2", type=numeric_literal.
//             text="emit_namelist", type=emit:
//                 text="a", type=numeric_literal.
//                 text="b", type=numeric_literal.
//                 text="c", type=numeric_literal.
//         text="stream", type=stream:

// pnode is input; pkeylist_evaluators is appended to.
sllv_t* allocate_keylist_evaluators_from_oosvar_node(mlr_dsl_cst_t* pcst, // xxx rename
	mlr_dsl_ast_node_t* pnode, int type_inferencing, int context_flags)
{
	sllv_t* pkeylist_evaluators = sllv_alloc();

	// xxx comment
	if (pnode->pchildren != NULL) {
		for (sllve_t* pe = pnode->pchildren->phead; pe != NULL; pe = pe->pnext) {
			mlr_dsl_ast_node_t* pkeynode = pe->pvvalue;
			if (pkeynode->type == MD_AST_NODE_TYPE_STRING_LITERAL) {
				sllv_append(pkeylist_evaluators, rval_evaluator_alloc_from_string(pkeynode->text));
			} else {
				sllv_append(pkeylist_evaluators, rval_evaluator_alloc_from_ast(pkeynode, pcst->pfmgr,
					type_inferencing, context_flags));
			}
		}
	}

	return pkeylist_evaluators;
}

// ----------------------------------------------------------------
void mlr_dsl_cst_statement_free(mlr_dsl_cst_statement_t* pstatement) {

	// xxx post-federation
	if (pstatement->pstatement_freer != NULL) {
		pstatement->pstatement_freer(pstatement);
	}

	free(pstatement->local_lhs_variable_name);

	if (pstatement->poosvar_target_keylist_evaluators != NULL) {
		for (sllve_t* pe = pstatement->poosvar_target_keylist_evaluators->phead; pe != NULL; pe = pe->pnext) {
			rval_evaluator_t* phandler = pe->pvvalue;
			phandler->pfree_func(phandler);
		}
		sllv_free(pstatement->poosvar_target_keylist_evaluators);
	}

	if (pstatement->psrec_lhs_evaluator != NULL) {
		pstatement->psrec_lhs_evaluator->pfree_func(pstatement->psrec_lhs_evaluator);
	}

	if (pstatement->prhs_evaluator != NULL) {
		pstatement->prhs_evaluator->pfree_func(pstatement->prhs_evaluator);
	}

	if (pstatement->poosvar_rhs_keylist_evaluators != NULL) {
		for (sllve_t* pe = pstatement->poosvar_rhs_keylist_evaluators->phead; pe != NULL; pe = pe->pnext) {
			rval_evaluator_t* phandler = pe->pvvalue;
			phandler->pfree_func(phandler);
		}
		sllv_free(pstatement->poosvar_rhs_keylist_evaluators);
	}

	if (pstatement->pvarargs != NULL) {
		for (sllve_t* pe = pstatement->pvarargs->phead; pe != NULL; pe = pe->pnext)
			cst_statement_vararg_free(pe->pvvalue);
		sllv_free(pstatement->pvarargs);
	}

	cst_statement_block_free(pstatement->pblock);

	if (pstatement->for_map_k_variable_names != NULL) {
		for (int i = 0; i < pstatement->for_map_k_count; i++)
			free(pstatement->for_map_k_variable_names[i]);
		free(pstatement->for_map_k_variable_names);
	}
	free(pstatement->for_map_k_frame_relative_indices);
	free(pstatement->for_map_k_type_masks);
	free(pstatement->for_v_variable_name);

	free(pstatement);
}

// ================================================================
mlr_dsl_cst_statement_vararg_t* mlr_dsl_cst_statement_vararg_alloc(
	int               unset_local_variable_frame_relative_index,
	char*             emitf_or_unset_srec_field_name,
	rval_evaluator_t* punset_srec_field_name_evaluator,
	rval_evaluator_t* pemitf_arg_evaluator,
	sllv_t*           punset_oosvar_keylist_evaluators)
{
	mlr_dsl_cst_statement_vararg_t* pvararg = mlr_malloc_or_die(sizeof(mlr_dsl_cst_statement_vararg_t));
	pvararg->punset_handler = NULL;
	pvararg->unset_local_variable_frame_relative_index = unset_local_variable_frame_relative_index;
	pvararg->emitf_or_unset_srec_field_name = emitf_or_unset_srec_field_name == NULL
		? NULL : mlr_strdup_or_die(emitf_or_unset_srec_field_name);
	pvararg->punset_oosvar_keylist_evaluators = punset_oosvar_keylist_evaluators;
	pvararg->punset_srec_field_name_evaluator = punset_srec_field_name_evaluator;
	pvararg->pemitf_arg_evaluator             = pemitf_arg_evaluator;

	if (pvararg->unset_local_variable_frame_relative_index != MD_UNUSED_INDEX) {
		pvararg->punset_handler = handle_unset_local_variable;
	} else if (pvararg->punset_oosvar_keylist_evaluators != NULL) {
		pvararg->punset_handler = handle_unset_vararg_oosvar;
	} else if (pvararg->punset_srec_field_name_evaluator != NULL) {
		pvararg->punset_handler = handle_unset_vararg_indirect_srec_field_name;
	} else if (pvararg->emitf_or_unset_srec_field_name != NULL) {
		pvararg->punset_handler = handle_unset_vararg_srec_field_name;
	} else {
		pvararg->punset_handler = handle_unset_vararg_full_srec;
	}

	return pvararg;
}

void cst_statement_vararg_free(mlr_dsl_cst_statement_vararg_t* pvararg) {
	if (pvararg == NULL)
		return;
	free(pvararg->emitf_or_unset_srec_field_name);

	if (pvararg->punset_srec_field_name_evaluator != NULL) {
		pvararg->punset_srec_field_name_evaluator->pfree_func(pvararg->punset_srec_field_name_evaluator);
	}

	if (pvararg->punset_oosvar_keylist_evaluators != NULL) {
		for (sllve_t* pe = pvararg->punset_oosvar_keylist_evaluators->phead; pe != NULL; pe = pe->pnext) {
			rval_evaluator_t* phandler = pe->pvvalue;
			phandler->pfree_func(phandler);
		}
		sllv_free(pvararg->punset_oosvar_keylist_evaluators);
	}

	if (pvararg->pemitf_arg_evaluator != NULL)
		pvararg->pemitf_arg_evaluator->pfree_func(pvararg->pemitf_arg_evaluator);

	free(pvararg);
}

// ================================================================
// Top-level entry point for statement-handling, e.g. from mapper_put.

void mlr_dsl_cst_handle_top_level_statement_blocks(
	sllv_t*      ptop_level_blocks, // block bodies for begins, main, ends
	variables_t* pvars,
	cst_outputs_t* pcst_outputs)
{
	for (sllve_t* pe = ptop_level_blocks->phead; pe != NULL; pe = pe->pnext) {
		mlr_dsl_cst_handle_top_level_statement_block(pe->pvvalue, pvars, pcst_outputs);
	}
}

void mlr_dsl_cst_handle_top_level_statement_block(
	cst_top_level_statement_block_t* ptop_level_block,
	variables_t* pvars,
	cst_outputs_t* pcst_outputs)
{
	local_stack_push(pvars->plocal_stack, local_stack_frame_enter(ptop_level_block->pframe));
	local_stack_frame_t* pframe = local_stack_get_top_frame(pvars->plocal_stack);
	local_stack_subframe_enter(pframe, ptop_level_block->pblock->subframe_var_count);

	mlr_dsl_cst_handle_statement_block(ptop_level_block->pblock, pvars, pcst_outputs);

	local_stack_subframe_exit(pframe, ptop_level_block->pblock->subframe_var_count);
	local_stack_frame_exit(local_stack_pop(pvars->plocal_stack));
}

// ================================================================
// HANDLERS
// ================================================================
// This is for statement lists not recursively contained within a loop body -- including the
// main/begin/end statements.  Since there is no containing loop body, there is no need to check
// for break or continue flags after each statement.
void mlr_dsl_cst_handle_statement_block(
	cst_statement_block_t* pblock,
	variables_t*           pvars,
	cst_outputs_t*         pcst_outputs)
{
	if (pvars->trace_execution) { // xxx find a better way to control this ...
		for (sllve_t* pe = pblock->pstatements->phead; pe != NULL; pe = pe->pnext) {
			mlr_dsl_cst_statement_t* pstatement = pe->pvvalue;
			fprintf(stderr, "TRACE ");
			mlr_dsl_ast_node_pretty_fprint(pstatement->past_node, stderr);
			pstatement->pstatement_handler(pstatement, pvars, pcst_outputs);
			// The UDF/subroutine executor will clear the flag, and consume the retval if there is one.
			if (pvars->return_state.returned) {
				break;
			}
		}
	} else {
		for (sllve_t* pe = pblock->pstatements->phead; pe != NULL; pe = pe->pnext) {
			mlr_dsl_cst_statement_t* pstatement = pe->pvvalue;
			pstatement->pstatement_handler(pstatement, pvars, pcst_outputs);
			// The UDF/subroutine executor will clear the flag, and consume the retval if there is one.
			if (pvars->return_state.returned) {
				break;
			}
		}
	}
}

// This is for statement lists recursively contained within a loop body.
// It checks for break or continue flags after each statement.
void handle_statement_block_with_break_continue(
	cst_statement_block_t* pblock,
	variables_t*   pvars,
	cst_outputs_t* pcst_outputs)
{
	if (pvars->trace_execution) { // xxx find a better way to control this ...
		for (sllve_t* pe = pblock->pstatements->phead; pe != NULL; pe = pe->pnext) {
			mlr_dsl_cst_statement_t* pstatement = pe->pvvalue;
			fprintf(stderr, "TRACE ");
			mlr_dsl_ast_node_pretty_fprint(pstatement->past_node, stderr);
			pstatement->pstatement_handler(pstatement, pvars, pcst_outputs);
			if (loop_stack_get(pvars->ploop_stack) != 0) {
				break;
			}
			// The UDF/subroutine executor will clear the flag, and consume the retval if there is one.
			if (pvars->return_state.returned) {
				break;
			}
		}
	} else {
		for (sllve_t* pe = pblock->pstatements->phead; pe != NULL; pe = pe->pnext) {
			mlr_dsl_cst_statement_t* pstatement = pe->pvvalue;
			pstatement->pstatement_handler(pstatement, pvars, pcst_outputs);
			if (loop_stack_get(pvars->ploop_stack) != 0) {
				break;
			}
			// The UDF/subroutine executor will clear the flag, and consume the retval if there is one.
			if (pvars->return_state.returned) {
				break;
			}
		}
	}
}

// Triple-for start/continuation/update statement lists
void mlr_dsl_cst_handle_statement_list(
	sllv_t*        pstatements,
	variables_t*   pvars,
	cst_outputs_t* pcst_outputs)
{
	if (pvars->trace_execution) { // xxx find a better way to control this ...
		for (sllve_t* pe = pstatements->phead; pe != NULL; pe = pe->pnext) {
			mlr_dsl_cst_statement_t* pstatement = pe->pvvalue;
			fprintf(stderr, "TRACE ");
			mlr_dsl_ast_node_pretty_fprint(pstatement->past_node, stderr);
			pstatement->pstatement_handler(pstatement, pvars, pcst_outputs);
		}
	} else {
		for (sllve_t* pe = pstatements->phead; pe != NULL; pe = pe->pnext) {
			mlr_dsl_cst_statement_t* pstatement = pe->pvvalue;
			pstatement->pstatement_handler(pstatement, pvars, pcst_outputs);
		}
	}
}

// ----------------------------------------------------------------
static void handle_unset(
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs)
{
	for (sllve_t* pf = pstatement->pvarargs->phead; pf != NULL; pf = pf->pnext) {
		mlr_dsl_cst_statement_vararg_t* pvararg = pf->pvvalue;
		pvararg->punset_handler(pvararg, pvars, pcst_outputs);
	}
}

static void handle_unset_local_variable(
	mlr_dsl_cst_statement_vararg_t* pvararg,
	variables_t*                    pvars,
	cst_outputs_t*                  pcst_outputs)
{
	local_stack_frame_t* pframe = local_stack_get_top_frame(pvars->plocal_stack);
	local_stack_frame_assign_non_map(pframe, pvararg->unset_local_variable_frame_relative_index, mv_absent());
}

static void handle_unset_vararg_oosvar(
	mlr_dsl_cst_statement_vararg_t* pvararg,
	variables_t*                    pvars,
	cst_outputs_t*                  pcst_outputs)
{
	int all_non_null_or_error = TRUE;
	sllmv_t* pmvkeys = evaluate_list(pvararg->punset_oosvar_keylist_evaluators, pvars, &all_non_null_or_error);
	if (all_non_null_or_error)
		mlhmmv_remove(pvars->poosvars, pmvkeys);
	sllmv_free(pmvkeys);
}

static void handle_unset_vararg_full_srec(
	mlr_dsl_cst_statement_vararg_t* pvararg,
	variables_t*                    pvars,
	cst_outputs_t*                  pcst_outputs)
{
	lrec_clear(pvars->pinrec);
}

static void handle_unset_vararg_srec_field_name(
	mlr_dsl_cst_statement_vararg_t* pvararg,
	variables_t*                    pvars,
	cst_outputs_t*                  pcst_outputs)
{
	lrec_remove(pvars->pinrec, pvararg->emitf_or_unset_srec_field_name);
}

static void handle_unset_vararg_indirect_srec_field_name(
	mlr_dsl_cst_statement_vararg_t* pvararg,
	variables_t*                    pvars,
	cst_outputs_t*                  pcst_outputs)
{
	rval_evaluator_t* pevaluator = pvararg->punset_srec_field_name_evaluator;
	mv_t nameval = pevaluator->pprocess_func(pevaluator->pvstate, pvars);
	char free_flags = NO_FREE;
	char* field_name = mv_maybe_alloc_format_val(&nameval, &free_flags);
	lrec_remove(pvars->pinrec, field_name);
	if (free_flags & FREE_ENTRY_VALUE)
		free(field_name);
	mv_free(&nameval);
}

// ----------------------------------------------------------------
static void handle_unset_all(
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs)
{
	sllmv_t* pempty = sllmv_alloc();
	mlhmmv_remove(pvars->poosvars, pempty);
	sllmv_free(pempty);
}

// ----------------------------------------------------------------
static void handle_for_oosvar(
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs)
{
	// Evaluate the keylist: e.g. in 'for ((k1, k2), v in @a[$3][x]) { ... }', find the values of $3
	// and x for the current record and stack frame. The keylist bindings are outside the scope
	// of the for-loop, while the k1/k2/v are bound within the for-loop.

	int keys_all_non_null_or_error = FALSE;
	sllmv_t* plhskeylist = evaluate_list(pstatement->poosvar_target_keylist_evaluators, pvars,
		&keys_all_non_null_or_error);
	if (keys_all_non_null_or_error) {

		local_stack_frame_t* pframe = local_stack_get_top_frame(pvars->plocal_stack);
		local_stack_subframe_enter(pframe, pstatement->pblock->subframe_var_count);
		loop_stack_push(pvars->ploop_stack);

		// Locate and copy the submap indexed by the keylist. E.g. in 'for ((k1, k2), v in @a[3][$4]) { ... }', the
		// submap is indexed by ["a", 3, $4].  Copy it for the very likely case that it is being updated inside the
		// for-loop.
		mlhmmv_value_t submap = mlhmmv_copy_submap_from_root(pvars->poosvars, plhskeylist);

		if (!submap.is_terminal && submap.u.pnext_level != NULL) {
			// Recurse over the for-k-names, e.g. ["k1", "k2"], on each call descending one level
			// deeper into the submap.  Note there must be at least one k-name so we are assuming
			// the for-loop within handle_for_oosvar_aux was gone through once & thus
			// handle_statement_block_with_break_continue was called through there.

			handle_for_oosvar_aux(pstatement, pvars, pcst_outputs, submap,
				pstatement->for_map_k_variable_names, pstatement->for_map_k_frame_relative_indices,
				pstatement->for_map_k_type_masks, pstatement->for_map_k_count);

			if (loop_stack_get(pvars->ploop_stack) & LOOP_BROKEN) {
				loop_stack_clear(pvars->ploop_stack, LOOP_BROKEN);
			}
			if (loop_stack_get(pvars->ploop_stack) & LOOP_CONTINUED) {
				loop_stack_clear(pvars->ploop_stack, LOOP_CONTINUED);
			}
		}

		mlhmmv_free_submap(submap);

		loop_stack_pop(pvars->ploop_stack);
		local_stack_subframe_exit(pframe, pstatement->pblock->subframe_var_count);
	}
	sllmv_free(plhskeylist);
}

static void handle_for_oosvar_aux(
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs,
	mlhmmv_value_t           submap,
	char**                   prest_for_k_variable_names,
	int*                     prest_for_k_frame_relative_indices,
	int*                     prest_for_k_type_masks,
	int                      prest_for_k_count)
{
	if (prest_for_k_count > 0) { // Keep recursing over remaining k-names

		if (submap.is_terminal) {
			// The submap was too shallow for the user-specified k-names; there are no terminals here.
		} else {
			// Loop over keys at this submap level:
			for (mlhmmv_level_entry_t* pe = submap.u.pnext_level->phead; pe != NULL; pe = pe->pnext) {
				// Bind the k-name to the entry-key mlrval:
				local_stack_frame_t* pframe = local_stack_get_top_frame(pvars->plocal_stack);
				local_stack_frame_define(pframe, prest_for_k_variable_names[0], prest_for_k_frame_relative_indices[0],
					prest_for_k_type_masks[0], mv_copy(&pe->level_key));
				// Recurse into the next-level submap:
				handle_for_oosvar_aux(pstatement, pvars, pcst_outputs, pe->level_value,
					&prest_for_k_variable_names[1], &prest_for_k_frame_relative_indices[1], &prest_for_k_type_masks[1],
					prest_for_k_count - 1);

				if (loop_stack_get(pvars->ploop_stack) & LOOP_BROKEN) {
					// Bit cleared in recursive caller
					return;
				} else if (loop_stack_get(pvars->ploop_stack) & LOOP_CONTINUED) {
					loop_stack_clear(pvars->ploop_stack, LOOP_CONTINUED);
				}

			}
		}

	} else { // End of recursion: k-names have all been used up

		if (!submap.is_terminal) {
			// The submap was too deep for the user-specified k-names; there are no terminals here.
		} else {
			// Bind the v-name to the terminal mlrval:
			local_stack_frame_t* pframe = local_stack_get_top_frame(pvars->plocal_stack);
			local_stack_frame_define(pframe, pstatement->for_v_variable_name, pstatement->for_v_frame_relative_index,
				pstatement->for_v_type_mask, mv_copy(&submap.u.mlrval));
			// Execute the loop-body statements:
			pstatement->pblock_handler(pstatement->pblock, pvars, pcst_outputs);
		}

	}
}

// ----------------------------------------------------------------
static void handle_for_oosvar_key_only(
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs)
{
	// Evaluate the keylist: e.g. in 'for (k in @a[$3][x]) { ... }', find the values of $3
	// and x for the current record and stack frame. The keylist bindings are outside the scope
	// of the for-loop, while the k is bound within the for-loop.

	int keys_all_non_null_or_error = FALSE;
	sllmv_t* plhskeylist = evaluate_list(pstatement->poosvar_target_keylist_evaluators, pvars,
		&keys_all_non_null_or_error);
	if (keys_all_non_null_or_error) {
		// Locate the submap indexed by the keylist and copy its keys. E.g. in 'for (k1 in @a[3][$4]) { ... }', the
		// submap is indexed by ["a", 3, $4].  Copy it for the very likely case that it is being updated inside the
		// for-loop.

		local_stack_frame_t* pframe = local_stack_get_top_frame(pvars->plocal_stack);
		local_stack_subframe_enter(pframe, pstatement->pblock->subframe_var_count);
		loop_stack_push(pvars->ploop_stack);

		sllv_t* pkeys = mlhmmv_copy_keys_from_submap(pvars->poosvars, plhskeylist);

		for (sllve_t* pe = pkeys->phead; pe != NULL; pe = pe->pnext) {
			// Bind the v-name to the terminal mlrval:
			local_stack_frame_define(pframe,
				pstatement->for_map_k_variable_names[0], pstatement->for_map_k_frame_relative_indices[0],
				pstatement->for_map_k_type_masks[0], mv_copy(pe->pvvalue));

			// Execute the loop-body statements:
			pstatement->pblock_handler(pstatement->pblock, pvars, pcst_outputs);

			if (loop_stack_get(pvars->ploop_stack) & LOOP_BROKEN) {
				loop_stack_clear(pvars->ploop_stack, LOOP_BROKEN);
			}
			if (loop_stack_get(pvars->ploop_stack) & LOOP_CONTINUED) {
				loop_stack_clear(pvars->ploop_stack, LOOP_CONTINUED);
			}

			mv_free(pe->pvvalue);
			free(pe->pvvalue);
		}

		loop_stack_pop(pvars->ploop_stack);
		local_stack_subframe_exit(pframe, pstatement->pblock->subframe_var_count);

		sllv_free(pkeys);
	}
	sllmv_free(plhskeylist);
}

// ----------------------------------------------------------------
static void handle_for_local_map( // xxx vardef_frame_relative_index
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs)
{
	// Evaluate the keylist: e.g. in 'for ((k1, k2), v in @a[$3][x]) { ... }', find the values of $3
	// and x for the current record and stack frame. The keylist bindings are outside the scope
	// of the for-loop, while the k1/k2/v are bound within the for-loop.

	int keys_all_non_null_or_error = FALSE;
	// xxx confusing 'oosvar' name ... clean up please.
	// xxx confusing 'keylist' name ... clean up please.
	sllmv_t* plhskeylist = evaluate_list(pstatement->poosvar_target_keylist_evaluators, pvars,
		&keys_all_non_null_or_error);
	if (keys_all_non_null_or_error) {

		// In '(for a, b in c) { ... }' the 'c' is evaluated in the outer scope and
		// the a, b are bound within the inner scope.
		local_stack_frame_t* pframe = local_stack_get_top_frame(pvars->plocal_stack);

		// Locate and copy the submap indexed by the keylist. E.g. in 'for ((k1, k2), v in a[3][$4])
		// { ... }', the submap is first indexed by the stack-frame slot for "a", then further
		// indexed by [3, $4].  Copy it for the very likely case that it is being updated inside the
		// for-loop.

		mlhmmv_value_t *psubmap = local_stack_frame_get_map_value(pframe,
			pstatement->for_map_target_frame_relative_index, plhskeylist);

		if (psubmap != NULL) {
			mlhmmv_value_t submap = mlhmmv_copy_aux(psubmap);

			local_stack_subframe_enter(pframe, pstatement->pblock->subframe_var_count);
			loop_stack_push(pvars->ploop_stack);

			if (!submap.is_terminal && submap.u.pnext_level != NULL) {
				// Recurse over the for-k-names, e.g. ["k1", "k2"], on each call descending one level
				// deeper into the submap.  Note there must be at least one k-name so we are assuming
				// the for-loop within handle_for_local_map_aux was gone through once & thus
				// handle_statement_block_with_break_continue was called through there.

				handle_for_local_map_aux(pstatement, pvars, pcst_outputs, submap,
					pstatement->for_map_k_variable_names, pstatement->for_map_k_frame_relative_indices,
					pstatement->for_map_k_type_masks, pstatement->for_map_k_count);

				if (loop_stack_get(pvars->ploop_stack) & LOOP_BROKEN) {
					loop_stack_clear(pvars->ploop_stack, LOOP_BROKEN);
				}
				if (loop_stack_get(pvars->ploop_stack) & LOOP_CONTINUED) {
					loop_stack_clear(pvars->ploop_stack, LOOP_CONTINUED);
				}
			}

			mlhmmv_free_submap(submap);

			loop_stack_pop(pvars->ploop_stack);
			local_stack_subframe_exit(pframe, pstatement->pblock->subframe_var_count);
		}
	}
	sllmv_free(plhskeylist);
}

static void handle_for_local_map_aux(
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs,
	mlhmmv_value_t           submap,
	char**                   prest_for_k_variable_names,
	int*                     prest_for_k_frame_relative_indices,
	int*                     prest_for_k_type_masks,
	int                      prest_for_k_count)
{
	if (prest_for_k_count > 0) { // Keep recursing over remaining k-names

		if (submap.is_terminal) {
			// The submap was too shallow for the user-specified k-names; there are no terminals here.
		} else {
			// Loop over keys at this submap level:
			for (mlhmmv_level_entry_t* pe = submap.u.pnext_level->phead; pe != NULL; pe = pe->pnext) {
				// Bind the k-name to the entry-key mlrval:
				local_stack_frame_t* pframe = local_stack_get_top_frame(pvars->plocal_stack);
				local_stack_frame_define(pframe, prest_for_k_variable_names[0], prest_for_k_frame_relative_indices[0],
					prest_for_k_type_masks[0], mv_copy(&pe->level_key));
				// Recurse into the next-level submap:
				handle_for_local_map_aux(pstatement, pvars, pcst_outputs, pe->level_value,
					&prest_for_k_variable_names[1], &prest_for_k_frame_relative_indices[1], &prest_for_k_type_masks[1],
					prest_for_k_count - 1);

				if (loop_stack_get(pvars->ploop_stack) & LOOP_BROKEN) {
					// Bit cleared in recursive caller
					return;
				} else if (loop_stack_get(pvars->ploop_stack) & LOOP_CONTINUED) {
					loop_stack_clear(pvars->ploop_stack, LOOP_CONTINUED);
				}

			}
		}

	} else { // End of recursion: k-names have all been used up

		if (!submap.is_terminal) {
			// The submap was too deep for the user-specified k-names; there are no terminals here.
		} else {
			// Bind the v-name to the terminal mlrval:
			local_stack_frame_t* pframe = local_stack_get_top_frame(pvars->plocal_stack);
			local_stack_frame_define(pframe, pstatement->for_v_variable_name, pstatement->for_v_frame_relative_index,
				pstatement->for_v_type_mask, mv_copy(&submap.u.mlrval));
			// Execute the loop-body statements:
			pstatement->pblock_handler(pstatement->pblock, pvars, pcst_outputs);
		}

	}
}

// ----------------------------------------------------------------
static void handle_for_local_map_key_only( // xxx vardef_frame_relative_index
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs)
{
	// Evaluate the keylist: e.g. in 'for (k in @a[$3][x]) { ... }', find the values of $3
	// and x for the current record and stack frame. The keylist bindings are outside the scope
	// of the for-loop, while the k is bound within the for-loop.

	int keys_all_non_null_or_error = FALSE;
	// xxx rename plhskeylist to ptarget_keylist
	sllmv_t* plhskeylist = evaluate_list(pstatement->poosvar_target_keylist_evaluators, pvars,
		&keys_all_non_null_or_error);
	if (keys_all_non_null_or_error) {
		// Locate the submap indexed by the keylist and copy its keys. E.g. in 'for (k1 in @a[3][$4]) { ... }', the
		// submap is indexed by ["a", 3, $4].  Copy it for the very likely case that it is being updated inside the
		// for-loop.

		local_stack_frame_t* pframe = local_stack_get_top_frame(pvars->plocal_stack);

		// xxx this is wrong. each of these two statements is subscripting. only one or the other should.
		mlhmmv_value_t *psubmap = local_stack_frame_get_map_value(pframe,
			pstatement->for_map_target_frame_relative_index, plhskeylist);
		sllv_t* pkeys = mlhmmv_copy_keys_from_submap_xxx_rename(psubmap, NULL); // xxx refactor w/o null

		local_stack_subframe_enter(pframe, pstatement->pblock->subframe_var_count);
		loop_stack_push(pvars->ploop_stack);

		for (sllve_t* pe = pkeys->phead; pe != NULL; pe = pe->pnext) {
			// Bind the v-name to the terminal mlrval:
			local_stack_frame_define(pframe,
				pstatement->for_map_k_variable_names[0], pstatement->for_map_k_frame_relative_indices[0],
				pstatement->for_map_k_type_masks[0], mv_copy(pe->pvvalue));

			// Execute the loop-body statements:
			pstatement->pblock_handler(pstatement->pblock, pvars, pcst_outputs);

			if (loop_stack_get(pvars->ploop_stack) & LOOP_BROKEN) {
				loop_stack_clear(pvars->ploop_stack, LOOP_BROKEN);
			}
			if (loop_stack_get(pvars->ploop_stack) & LOOP_CONTINUED) {
				loop_stack_clear(pvars->ploop_stack, LOOP_CONTINUED);
			}

			mv_free(pe->pvvalue);
			free(pe->pvvalue);
		}

		loop_stack_pop(pvars->ploop_stack);
		local_stack_subframe_exit(pframe, pstatement->pblock->subframe_var_count);

		sllv_free(pkeys);
	}
	sllmv_free(plhskeylist);
}
