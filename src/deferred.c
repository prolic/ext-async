/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2018 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Martin Schröder <m.schroeder2007@gmail.com>                 |
  +----------------------------------------------------------------------+
*/

#include "php_async.h"
#include "async_task.h"

zend_class_entry *async_deferred_ce;
zend_class_entry *async_deferred_awaitable_ce;

static zend_object_handlers async_deferred_handlers;
static zend_object_handlers async_deferred_awaitable_handlers;


static inline void trigger_ops(async_deferred_state *state)
{
	async_op *op;
	
	if (state->cancel.object != NULL) {
		ASYNC_Q_DETACH(&state->scheduler->shutdown, &state->cancel);
		state->cancel.object = NULL;
	}
	
	while (state->operations.first != NULL) {
		ASYNC_DEQUEUE_OP(&state->operations, op);
		
		if (state->status == ASYNC_OP_RESOLVED) {
			ASYNC_RESOLVE_OP(op, &state->result);
		} else {
			ASYNC_FAIL_OP(op, &state->result);
		}
	}
}

static void shutdown_state(void *obj, zval *error)
{
	async_deferred_state *state;
	
	state = (async_deferred_state *) obj;
	
	ZEND_ASSERT(state != NULL);

	state->cancel.object = NULL;
	state->status = ASYNC_DEFERRED_STATUS_FAILED;
	
	ZVAL_COPY(&state->result, error);
	
	trigger_ops(state);
}

static async_deferred_state *create_state(async_context *context)
{
	async_deferred_state *state;

	state = emalloc(sizeof(async_deferred_state));
	ZEND_SECURE_ZERO(state, sizeof(async_deferred_state));

	ZVAL_NULL(&state->result);

	state->refcount = 1;
	state->scheduler = async_task_scheduler_get();
	state->context = context;
	
	state->cancel.object = state;
	state->cancel.func = shutdown_state;
	
	ASYNC_Q_ENQUEUE(&state->scheduler->shutdown, &state->cancel);
	
	ASYNC_ADDREF(&state->scheduler->std);
	ASYNC_ADDREF(&context->std);

	return state;
}

static void release_state(async_deferred_state *state)
{
	async_op *op;

	if (0 != --state->refcount) {
		return;
	}
	
	ZEND_ASSERT(state->status == ASYNC_DEFERRED_STATUS_PENDING || state->operations.first == NULL);

	if (state->cancel.object != NULL) {
		ASYNC_Q_DETACH(&state->scheduler->shutdown, &state->cancel);
		state->cancel.object = NULL;
	}

	if (state->status == ASYNC_DEFERRED_STATUS_PENDING) {
		state->status = state->status == ASYNC_DEFERRED_STATUS_FAILED;

		if (state->operations.first != NULL) {
			ASYNC_PREPARE_ERROR(&state->result, "Awaitable has been disposed before it was resolved");

			while (state->operations.first != NULL) {
				ASYNC_DEQUEUE_OP(&state->operations, op);
				ASYNC_FAIL_OP(op, &state->result);
			}
		}
	}

	zval_ptr_dtor(&state->result);

	ASYNC_DELREF(&state->scheduler->std);
	ASYNC_DELREF(&state->context->std);

	efree(state);
}


#define ASYNC_DEFERRED_CLEANUP_CANCEL(defer) do { \
	if (Z_TYPE_P(&defer->fci.function_name) != IS_UNDEF) { \
		if (defer->cancel.object != NULL) { \
			ASYNC_Q_DETACH(&defer->state->context->cancel->callbacks, &defer->cancel); \
			defer->cancel.object = NULL; \
		} \
	} \
} while (0);


static inline void register_defer_op(async_op *op, async_deferred_state *state)
{
	if (state->status == ASYNC_OP_RESOLVED) {
		ASYNC_RESOLVE_OP(op, &state->result);
	} else if (state->status == ASYNC_OP_FAILED) {
		ASYNC_FAIL_OP(op, &state->result);
	} else {
		ASYNC_ENQUEUE_OP(&state->operations, op);
	}
}

static inline void register_task_op(async_op *op, async_task *task)
{
	if (task->fiber.status == ASYNC_OP_RESOLVED) {
		ASYNC_RESOLVE_OP(op, &task->result);
	} else if (task->fiber.status == ASYNC_OP_FAILED) {
		ASYNC_FAIL_OP(op, &task->result);
	} else {
		ASYNC_ENQUEUE_OP(&task->operations, op);
	}
}

static void cancel_defer(void *obj, zval* error)
{
	async_deferred *defer;

	zval args[2];
	zval retval;

	defer = (async_deferred *) obj;

	ZEND_ASSERT(defer != NULL);

	defer->cancel.object = NULL;

	ZVAL_OBJ(&args[0], &defer->std);
	ASYNC_ADDREF(&defer->std);

	ZVAL_COPY(&args[1], error);

	defer->fci.param_count = 2;
	defer->fci.params = args;
	defer->fci.retval = &retval;
	defer->fci.no_separation = 1;

	async_task_scheduler_call_nowait(async_task_scheduler_get(), &defer->fci, &defer->fcc);

	zval_ptr_dtor(&args[0]);
	zval_ptr_dtor(&args[1]);
	zval_ptr_dtor(&retval);

	zval_ptr_dtor(&defer->fci.function_name);

	ASYNC_CHECK_FATAL(UNEXPECTED(EG(exception)), "Must not throw an error from cancellation handler");
}


static async_deferred_awaitable *async_deferred_awaitable_object_create(async_deferred_state *state)
{
	async_deferred_awaitable *awaitable;

	awaitable = emalloc(sizeof(async_deferred_awaitable));
	ZEND_SECURE_ZERO(awaitable, sizeof(async_deferred_awaitable));

	zend_object_std_init(&awaitable->std, async_deferred_awaitable_ce);
	awaitable->std.handlers = &async_deferred_awaitable_handlers;

	awaitable->state = state;

	state->refcount++;

	return awaitable;
}

static void async_deferred_awaitable_object_destroy(zend_object *object)
{
	async_deferred_awaitable *awaitable;

	awaitable = (async_deferred_awaitable *) object;

	release_state(awaitable->state);

	zend_object_std_dtor(&awaitable->std);
}

ZEND_METHOD(DeferredAwaitable, __construct)
{
	ZEND_PARSE_PARAMETERS_NONE();

	zend_throw_error(NULL, "Deferred awaitable must not be created from userland code");
}

ZEND_METHOD(DeferredAwaitable, __debugInfo)
{
	async_deferred_state *state;

	ZEND_PARSE_PARAMETERS_NONE();

	state = ((async_deferred_awaitable *) Z_OBJ_P(getThis()))->state;

	if (USED_RET()) {
		array_init(return_value);

		add_assoc_string(return_value, "status", async_status_label(state->status));
		
		if (state->status != ASYNC_DEFERRED_STATUS_PENDING) {
			Z_TRY_ADDREF_P(&state->result);
		
			add_assoc_zval(return_value, "result", &state->result);
		}
	}
}

ZEND_BEGIN_ARG_INFO(arginfo_deferred_awaitable_debug_info, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_deferred_awaitable_ctor, 0, 0, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry deferred_awaitable_functions[] = {
	ZEND_ME(DeferredAwaitable, __construct, arginfo_deferred_awaitable_ctor, ZEND_ACC_PRIVATE)
	ZEND_ME(DeferredAwaitable, __debugInfo, arginfo_deferred_awaitable_debug_info, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};


static zend_object *async_deferred_object_create(zend_class_entry *ce)
{
	async_deferred *defer;

	defer = emalloc(sizeof(async_deferred));
	ZEND_SECURE_ZERO(defer, sizeof(async_deferred));

	zend_object_std_init(&defer->std, ce);
	defer->std.handlers = &async_deferred_handlers;
	
	defer->state = create_state(async_context_get());

	return &defer->std;
}

static void async_deferred_object_destroy(zend_object *object)
{
	async_deferred *defer;

	defer = (async_deferred *) object;

	ASYNC_DEFERRED_CLEANUP_CANCEL(defer);
	
	if (defer->state->status == ASYNC_DEFERRED_STATUS_PENDING) {
		defer->state->status = ASYNC_OP_FAILED;
		ASYNC_PREPARE_ERROR(&defer->state->result, "Awaitable has been disposed before it was resolved");

		trigger_ops(defer->state);
	}
	
	release_state(defer->state);

	zend_object_std_dtor(&defer->std);
}

ZEND_METHOD(Deferred, __construct)
{
	async_deferred *defer;
	async_context *context;
	
	defer = (async_deferred *) Z_OBJ_P(getThis());

	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_FUNC_EX(defer->fci, defer->fcc, 1, 0)
	ZEND_PARSE_PARAMETERS_END();
	
	if (ZEND_NUM_ARGS() > 0) {
		context = defer->state->context;

		if (context->cancel != NULL) {
			if (Z_TYPE_P(&context->cancel->error) != IS_UNDEF) {
				cancel_defer(defer, &context->cancel->error);
			} else {
				defer->cancel.object = defer;
				defer->cancel.func = cancel_defer;

				Z_TRY_ADDREF_P(&defer->fci.function_name);

				ASYNC_Q_ENQUEUE(&context->cancel->callbacks, &defer->cancel);
			}
		}
	}
}

ZEND_METHOD(Deferred, __debugInfo)
{
	async_deferred *defer;

	ZEND_PARSE_PARAMETERS_NONE();

	defer = (async_deferred *) Z_OBJ_P(getThis());

	if (USED_RET()) {
		array_init(return_value);

		add_assoc_string(return_value, "status", async_status_label(defer->state->status));
		
		if (defer->state->status != ASYNC_DEFERRED_STATUS_PENDING) {
			Z_TRY_ADDREF_P(&defer->state->result);
		
			add_assoc_zval(return_value, "result", &defer->state->result);
		}
	}
}

ZEND_METHOD(Deferred, awaitable)
{
	async_deferred *defer;

	zval obj;

	ZEND_PARSE_PARAMETERS_NONE();

	defer = (async_deferred *) Z_OBJ_P(getThis());

	ZVAL_OBJ(&obj, &async_deferred_awaitable_object_create(defer->state)->std);

	RETURN_ZVAL(&obj, 1, 1);
}

ZEND_METHOD(Deferred, resolve)
{
	async_deferred *defer;

	zval *val;

	val = NULL;

	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_ZVAL(val)
	ZEND_PARSE_PARAMETERS_END();

	if (val != NULL && Z_TYPE_P(val) == IS_OBJECT) {
		if (instanceof_function(Z_OBJCE_P(val), async_awaitable_ce) != 0) {

			zend_throw_error(NULL, "Deferred must not be resolved with an object implementing Awaitable");
			return;
		}
	}

	defer = (async_deferred *) Z_OBJ_P(getThis());

	if (defer->state->status != ASYNC_DEFERRED_STATUS_PENDING) {
		return;
	}

	if (val != NULL) {
		ZVAL_COPY(&defer->state->result, val);
	}

	defer->state->status = ASYNC_DEFERRED_STATUS_RESOLVED;

	ASYNC_DEFERRED_CLEANUP_CANCEL(defer);
	
	trigger_ops(defer->state);
}

ZEND_METHOD(Deferred, fail)
{
	async_deferred *defer;

	zval *error;

	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 1, 1)
		Z_PARAM_ZVAL(error)
	ZEND_PARSE_PARAMETERS_END();

	defer = (async_deferred *) Z_OBJ_P(getThis());

	if (defer->state->status != ASYNC_DEFERRED_STATUS_PENDING) {
		return;
	}

	ZVAL_COPY(&defer->state->result, error);

	defer->state->status = ASYNC_DEFERRED_STATUS_FAILED;

	ASYNC_DEFERRED_CLEANUP_CANCEL(defer);
	
	trigger_ops(defer->state);
}

ZEND_METHOD(Deferred, value)
{
	async_deferred_state *state;
	async_deferred_awaitable *awaitable;

	zval *val;
	zval obj;

	val = NULL;

	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_ZVAL(val)
	ZEND_PARSE_PARAMETERS_END();

	if (val != NULL && Z_TYPE_P(val) == IS_OBJECT) {
		if (instanceof_function(Z_OBJCE_P(val), async_awaitable_ce) != 0) {
			zend_throw_error(NULL, "Deferred must not be resolved with an object implementing Awaitable");
			return;
		}
	}

	state = create_state(async_context_get());
	awaitable = async_deferred_awaitable_object_create(state);

	state->status = ASYNC_DEFERRED_STATUS_RESOLVED;
	state->refcount--;

	if (val != NULL) {
		ZVAL_COPY(&state->result, val);
	}

	ZVAL_OBJ(&obj, &awaitable->std);

	RETURN_ZVAL(&obj, 1, 1);
}

ZEND_METHOD(Deferred, error)
{
	async_deferred_state *state;
	async_deferred_awaitable *awaitable;

	zval *error;
	zval obj;

	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 1, 1)
		Z_PARAM_ZVAL(error)
	ZEND_PARSE_PARAMETERS_END();

	state = create_state(async_context_get());
	awaitable = async_deferred_awaitable_object_create(state);
	
	state->status = ASYNC_DEFERRED_STATUS_FAILED;
	state->refcount--;

	ZVAL_COPY(&state->result, error);

	ZVAL_OBJ(&obj, &awaitable->std);

	RETURN_ZVAL(&obj, 1, 1);
}

typedef struct {
	async_deferred *defer;
	uint32_t counter;
	uint32_t started;
	zend_fcall_info fci;
	zend_fcall_info_cache fcc;
} async_defer_combine;

typedef struct {
	async_op base;
	async_defer_combine *combine;
	async_deferred_awaitable *awaitable;
	zval key;
} async_defer_combine_op;

static void combine_cb(async_op *op)
{
	async_defer_combine_op *cb;
	async_defer_combine *combined;
	async_deferred_state *state;
	uint32_t i;

	zval args[5];
	zval retval;
	zend_object *error;

	cb = (async_defer_combine_op *) op;

	ZEND_ASSERT(cb != NULL);

	combined = cb->combine;

	ZVAL_OBJ(&args[0], &combined->defer->std);
	ASYNC_ADDREF(&combined->defer->std);

	ZVAL_BOOL(&args[1], 0 == --combined->started);
	ZVAL_COPY(&args[2], &cb->key);

	zval_ptr_dtor(&cb->key);

	if (op->status == ASYNC_STATUS_RESOLVED) {
		ZVAL_NULL(&args[3]);
		ZVAL_COPY(&args[4], &op->result);
	} else {
		ZVAL_COPY(&args[3], &op->result);
		ZVAL_NULL(&args[4]);
	}

	combined->fci.param_count = 5;
	combined->fci.params = args;
	combined->fci.retval = &retval;

	error = EG(exception);
	EG(exception) = NULL;

	// TODO: Check why keeping a reference is necessary...
	if (error != NULL) {
		ASYNC_ADDREF(error);
	}

	async_task_scheduler_call_nowait(async_task_scheduler_get(), &combined->fci, &combined->fcc);

	for (i = 0; i < 5; i++) {
		zval_ptr_dtor(&args[i]);
	}

	zval_ptr_dtor(&retval);

	if (cb->awaitable != NULL) {
		ASYNC_DELREF(&cb->awaitable->std);
	}

	ASYNC_FREE_OP(op);

	state = combined->defer->state;

	if (UNEXPECTED(EG(exception))) {
		if (state->status == ASYNC_DEFERRED_STATUS_PENDING) {
			state->status = ASYNC_DEFERRED_STATUS_FAILED;

			ZVAL_OBJ(&state->result, EG(exception));
			EG(exception) = NULL;

			trigger_ops(state);
		} else {
			EG(exception) = NULL;
		}
	}

	if (0 == --combined->counter) {
		zval_ptr_dtor(&combined->fci.function_name);

		if (state->status == ASYNC_DEFERRED_STATUS_PENDING) {
			state->status = ASYNC_DEFERRED_STATUS_FAILED;

			ASYNC_PREPARE_ERROR(&state->result, "Awaitable has been disposed before it was resolved");

			trigger_ops(state);
		}

		ASYNC_DELREF(&combined->defer->std);

		efree(combined);
	}

	EG(exception) = error;
}

ZEND_METHOD(Deferred, combine)
{
	async_deferred *defer;
	async_deferred_awaitable *awaitable;
	async_defer_combine *combined;
	async_defer_combine_op *op;

	zend_class_entry *ce;
	zend_fcall_info fci;
	zend_fcall_info_cache fcc;
	uint32_t count;

	zval *args;
	zend_ulong i;
	zend_string *k;
	zval *entry;
	zval obj;

	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 2, 2)
		Z_PARAM_ARRAY(args)
		Z_PARAM_FUNC_EX(fci, fcc, 1, 0)
	ZEND_PARSE_PARAMETERS_END();

	count = zend_array_count(Z_ARRVAL_P(args));

	if (count == 0) {
		zend_throw_error(zend_ce_argument_count_error, "At least one awaitable is required");
		return;
	}

	fci.no_separation = 1;

	ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(args), entry) {
		ce = (Z_TYPE_P(entry) == IS_OBJECT) ? Z_OBJCE_P(entry) : NULL;

		if (ce != async_task_ce && ce != async_deferred_awaitable_ce) {
			zend_throw_error(zend_ce_type_error, "All input elements must be awaitable");
			return;
		}
	} ZEND_HASH_FOREACH_END();

	defer = (async_deferred *) async_deferred_object_create(async_deferred_ce);
	awaitable = async_deferred_awaitable_object_create(defer->state);

	ZVAL_OBJ(&obj, &awaitable->std);

	combined = emalloc(sizeof(async_defer_combine));
	combined->defer = defer;
	combined->counter = count;
	combined->started = count;
	combined->fci = fci;
	combined->fcc = fcc;

	Z_TRY_ADDREF_P(&combined->fci.function_name);

	ZEND_HASH_FOREACH_KEY_VAL_IND(Z_ARRVAL_P(args), i, k, entry) {
		ce = Z_OBJCE_P(entry);
		
		ASYNC_ALLOC_CUSTOM_OP(op, sizeof(async_defer_combine_op));
		
		op->combine = combined;
		op->base.callback = combine_cb;

		if (k == NULL) {
			ZVAL_LONG(&op->key, i);
		} else {
			ZVAL_STR_COPY(&op->key, k);
		}

		if (ce == async_task_ce) {
			register_task_op((async_op *) op, (async_task *) Z_OBJ_P(entry));
		} else {
			op->awaitable = (async_deferred_awaitable *) Z_OBJ_P(entry);

			// Keep a reference to the awaitable because the args array could be garbage collected before combine resolves.
			ASYNC_ADDREF(&op->awaitable->std);

			register_defer_op((async_op *) op, op->awaitable->state);
		}
	} ZEND_HASH_FOREACH_END();

	RETURN_ZVAL(&obj, 1, 1);
}

typedef struct {
	async_op base;
	async_deferred_state *state;
	zend_fcall_info fci;
	zend_fcall_info_cache fcc;	
} async_defer_transform_op;

static void transform_cb(async_op *op)
{
	async_defer_transform_op *trans;
	
	trans = (async_defer_transform_op *) op;
	
	zval args[2];
	zval retval;
	zend_object *error;
	
	if (op->status == ASYNC_STATUS_RESOLVED) {
		ZVAL_NULL(&args[0]);
		ZVAL_COPY(&args[1], &op->result);
	} else {		
		ZVAL_COPY(&args[0], &op->result);
		ZVAL_NULL(&args[1]);
	}

	trans->fci.param_count = 2;
	trans->fci.params = args;
	trans->fci.retval = &retval;

	error = EG(exception);
	EG(exception) = NULL;

	// TODO: Check why keeping a reference is necessary...
	if (error != NULL) {
		ASYNC_ADDREF(error);
	}

	async_task_scheduler_call_nowait(async_task_scheduler_get(), &trans->fci, &trans->fcc);

	zval_ptr_dtor(&trans->fci.function_name);

	zval_ptr_dtor(&args[0]);
	zval_ptr_dtor(&args[1]);

	if (UNEXPECTED(EG(exception))) {
		trans->state->status = ASYNC_DEFERRED_STATUS_FAILED;

		ZVAL_OBJ(&trans->state->result, EG(exception));
		EG(exception) = NULL;
	} else {
		trans->state->status = ASYNC_DEFERRED_STATUS_RESOLVED;

		ZVAL_COPY(&trans->state->result, &retval);
	}
	
	trigger_ops(trans->state);

	ASYNC_FREE_OP(op);

	zval_ptr_dtor(&retval);

	release_state(trans->state);

	EG(exception) = error;
}

ZEND_METHOD(Deferred, transform)
{
	async_deferred_state *state;
	async_deferred_awaitable *awaitable;
	async_defer_transform_op *op;

	zend_class_entry *ce;
	zend_fcall_info fci;
	zend_fcall_info_cache fcc;

	zval *val;
	zval obj;

	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 2, 2)
		Z_PARAM_ZVAL(val)
		Z_PARAM_FUNC_EX(fci, fcc, 1, 0)
	ZEND_PARSE_PARAMETERS_END();

	state = create_state(async_context_get());
	awaitable = async_deferred_awaitable_object_create(state);

	ZVAL_OBJ(&obj, &awaitable->std);
	
	ASYNC_ALLOC_CUSTOM_OP(op, sizeof(async_defer_transform_op));
	
	op->base.callback = transform_cb;
	op->state = state;
	op->fci = fci;
	op->fcc = fcc;

	Z_TRY_ADDREF_P(&op->fci.function_name);

	ce = Z_OBJCE_P(val);

	if (ce == async_task_ce) {
		register_task_op((async_op *) op, (async_task *) Z_OBJ_P(val));
	} else {
		register_defer_op((async_op *) op, ((async_deferred_awaitable *) Z_OBJ_P(val))->state);
	}

	RETURN_ZVAL(&obj, 1, 1);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_deferred_ctor, 0, 0, 0)
	ZEND_ARG_CALLABLE_INFO(0, cancel, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_deferred_debug_info, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_deferred_awaitable, 0, 0, Concurrent\\Awaitable, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_deferred_resolve, 0, 0, 0)
	ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_deferred_fail, 0, 0, IS_VOID, 1)
	ZEND_ARG_OBJ_INFO(0, error, Throwable, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_deferred_value, 0, 0, Concurrent\\Awaitable, 0)
	ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_deferred_error, 0, 1, Concurrent\\Awaitable, 0)
	ZEND_ARG_OBJ_INFO(0, error, Throwable, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_deferred_combine, 0, 2, Concurrent\\Awaitable, 0)
	ZEND_ARG_ARRAY_INFO(0, awaitables, 0)
	ZEND_ARG_CALLABLE_INFO(0, continuation, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_deferred_transform, 0, 2, Concurrent\\Awaitable, 0)
	ZEND_ARG_OBJ_INFO(0, awaitable, Concurrent\\Awaitable, 0)
	ZEND_ARG_CALLABLE_INFO(0, continuation, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry deferred_functions[] = {
	ZEND_ME(Deferred, __construct, arginfo_deferred_ctor, ZEND_ACC_PUBLIC)
	ZEND_ME(Deferred, __debugInfo, arginfo_deferred_debug_info, ZEND_ACC_PUBLIC)
	ZEND_ME(Deferred, awaitable, arginfo_deferred_awaitable, ZEND_ACC_PUBLIC)
	ZEND_ME(Deferred, resolve, arginfo_deferred_resolve, ZEND_ACC_PUBLIC)
	ZEND_ME(Deferred, fail, arginfo_deferred_fail, ZEND_ACC_PUBLIC)
	ZEND_ME(Deferred, value, arginfo_deferred_value, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	ZEND_ME(Deferred, error, arginfo_deferred_error, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	ZEND_ME(Deferred, combine, arginfo_deferred_combine, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	ZEND_ME(Deferred, transform, arginfo_deferred_transform, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	ZEND_FE_END
};


void async_deferred_ce_register()
{
	zend_class_entry ce;

	INIT_CLASS_ENTRY(ce, "Concurrent\\Deferred", deferred_functions);
	async_deferred_ce = zend_register_internal_class(&ce);
	async_deferred_ce->ce_flags |= ZEND_ACC_FINAL;
	async_deferred_ce->create_object = async_deferred_object_create;
	async_deferred_ce->serialize = zend_class_serialize_deny;
	async_deferred_ce->unserialize = zend_class_unserialize_deny;

	memcpy(&async_deferred_handlers, &std_object_handlers, sizeof(zend_object_handlers));
	async_deferred_handlers.free_obj = async_deferred_object_destroy;
	async_deferred_handlers.clone_obj = NULL;

	INIT_CLASS_ENTRY(ce, "Concurrent\\DeferredAwaitable", deferred_awaitable_functions);
	async_deferred_awaitable_ce = zend_register_internal_class(&ce);
	async_deferred_awaitable_ce->ce_flags |= ZEND_ACC_FINAL;
	async_deferred_awaitable_ce->serialize = zend_class_serialize_deny;
	async_deferred_awaitable_ce->unserialize = zend_class_unserialize_deny;

	memcpy(&async_deferred_awaitable_handlers, &std_object_handlers, sizeof(zend_object_handlers));
	async_deferred_awaitable_handlers.free_obj = async_deferred_awaitable_object_destroy;
	async_deferred_awaitable_handlers.clone_obj = NULL;

	zend_class_implements(async_deferred_awaitable_ce, 1, async_awaitable_ce);
}


/*
 * vim: sw=4 ts=4
 * vim600: fdm=marker
 */
