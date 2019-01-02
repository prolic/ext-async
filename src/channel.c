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
#include "zend_inheritance.h"

zend_class_entry *async_channel_ce;
zend_class_entry *async_channel_closed_exception_ce;
zend_class_entry *async_channel_iterator_ce;

static zend_object_handlers async_channel_handlers;
static zend_object_handlers async_channel_iterator_handlers;

static async_channel_iterator *async_channel_iterator_object_create(async_channel *channel);

typedef struct {
	async_op base;
	zval* value;
} async_channel_send_op;

static void dispose_channel(void *arg, zval *error)
{
	async_channel *channel;
	async_op *op;
	
	channel = (async_channel *) arg;
	
	ZEND_ASSERT(channel != NULL);
	
	channel->cancel.func = NULL;
	channel->flags |= ASYNC_CHANNEL_FLAG_CLOSED;
	
	if (Z_TYPE_P(&channel->error) == IS_UNDEF) {
		if (error != NULL) {
			ZVAL_COPY(&channel->error, error);
		}
	}
	
	while (channel->receivers.first != NULL) {
		ASYNC_DEQUEUE_OP(&channel->receivers, op);
		
		if (Z_TYPE_P(&channel->error) == IS_UNDEF) {
			ASYNC_FINISH_OP(op);
		} else {
			ASYNC_FAIL_OP(op, &channel->error);
		}
	}
	
	while (channel->senders.first != NULL) {
		ASYNC_DEQUEUE_OP(&channel->senders, op);
		
		if (Z_TYPE_P(&channel->error) == IS_UNDEF) {
			ASYNC_FINISH_OP(op);
		} else {
			ASYNC_FAIL_OP(op, &channel->error);
		}
	}
}


static zend_object *async_channel_object_create(zend_class_entry *ce)
{
	async_channel *channel;
	
	channel = emalloc(sizeof(async_channel));
	ZEND_SECURE_ZERO(channel, sizeof(async_channel));
	
	zend_object_std_init(&channel->std, ce);
	channel->std.handlers = &async_channel_handlers;
	
	channel->scheduler = async_task_scheduler_get();
	
	ASYNC_ADDREF(&channel->scheduler->std);
	
	channel->cancel.object = channel;
	channel->cancel.func = dispose_channel;
	
	ASYNC_Q_ENQUEUE(&channel->scheduler->shutdown, &channel->cancel);
	
	return &channel->std;
}

static void async_channel_object_dtor(zend_object *object)
{
	async_channel *channel;
	
	channel = (async_channel *) object;
	
	if (channel->cancel.func != NULL) {
		ASYNC_Q_DETACH(&channel->scheduler->shutdown, &channel->cancel);
		
		channel->cancel.func(channel, NULL);
	}
}

static void async_channel_object_destroy(zend_object *object)
{
	async_channel *channel;
	
	channel = (async_channel *) object;
	
	zval_ptr_dtor(&channel->error);
	
	ASYNC_DELREF(&channel->scheduler->std);
	
	zend_object_std_dtor(&channel->std);
}

ZEND_METHOD(Channel, __construct)
{
	async_channel *channel;
	
	channel = (async_channel *) Z_OBJ_P(getThis());
	
	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(channel->size)
	ZEND_PARSE_PARAMETERS_END();
}

ZEND_METHOD(Channel, getIterator)
{
	async_channel *channel;
	async_channel_iterator *it;
	
	zval obj;
	
	ZEND_PARSE_PARAMETERS_NONE();
	
	channel = (async_channel *) Z_OBJ_P(getThis());
	
	it = async_channel_iterator_object_create(channel);
	
	ZVAL_OBJ(&obj, &it->std);
	
	RETURN_ZVAL(&obj, 1, 1);
}

ZEND_METHOD(Channel, close)
{
	async_channel *channel;
	
	zval *val;

	val = NULL;

	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_ZVAL(val)
	ZEND_PARSE_PARAMETERS_END();
	
	channel = (async_channel *) Z_OBJ_P(getThis());
	
	if (channel->cancel.func == NULL) {
		return;
	}
	
	ASYNC_Q_DETACH(&channel->scheduler->shutdown, &channel->cancel);
	
	channel->cancel.func(channel, (val == NULL || Z_TYPE_P(val) == IS_NULL) ? NULL :val);
}

ZEND_METHOD(Channel, send)
{
	async_channel *channel;
	async_channel_send_op *send;
	async_op *op;
	
	zval *val;
	
	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 1, 1)
		Z_PARAM_ZVAL(val)
	ZEND_PARSE_PARAMETERS_END();
	
	channel = (async_channel *) Z_OBJ_P(getThis());
	
	if (Z_TYPE_P(&channel->error) != IS_UNDEF) {
		Z_ADDREF_P(&channel->error);
		
		execute_data->opline--;
		zend_throw_exception_internal(&channel->error);
		execute_data->opline++;
	
		return;
	}
	
	if (channel->receivers.first != NULL) {
		ASYNC_DEQUEUE_OP(&channel->receivers, op);
		ASYNC_RESOLVE_OP(op, val);
		
		return;
	}
	
	ASYNC_ALLOC_CUSTOM_OP(send, sizeof(async_channel_send_op));
	
	send->value = val;
	
	ASYNC_ENQUEUE_OP(&channel->senders, send);
	
	if (async_await_op((async_op *) send) == FAILURE) {
		ASYNC_FORWARD_OP_ERROR(send);
	}
	
	ASYNC_FREE_OP(send);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_channel_ctor, 0, 0, 0)
	ZEND_ARG_TYPE_INFO(0, size, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_channel_get_iterator, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_channel_close, 0, 0, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, error, Throwable, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_channel_send, 0, 1, IS_VOID, 0)
	ZEND_ARG_INFO(0, message)
ZEND_END_ARG_INFO()

static const zend_function_entry channel_functions[] = {
	ZEND_ME(Channel, __construct, arginfo_channel_ctor, ZEND_ACC_PUBLIC)
	ZEND_ME(Channel, getIterator, arginfo_channel_get_iterator, ZEND_ACC_PUBLIC)
	ZEND_ME(Channel, close, arginfo_channel_close, ZEND_ACC_PUBLIC)
	ZEND_ME(Channel, send, arginfo_channel_send, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};


static void fetch_next_entry(async_channel_iterator *it)
{
	async_channel *channel;
	async_channel_send_op *send;
	async_op *op;
	
	zval error;
	
	ASYNC_CHECK_ERROR(it->flags & ASYNC_CHANNEL_ITERATOR_FLAG_FETCHING, "Cannot advance iterator while already awaiting next channel value");
	
	channel = it->channel;
	
	if (Z_TYPE_P(&channel->error) != IS_UNDEF) {
		ASYNC_PREPARE_EXCEPTION(&error, async_channel_closed_exception_ce, "Channel has been closed");

		zend_exception_set_previous(Z_OBJ_P(&error), Z_OBJ_P(&channel->error));
		Z_ADDREF_P(&channel->error);
		
		EG(current_execute_data)->opline--;
		zend_throw_exception_internal(&error);
		EG(current_execute_data)->opline++;
	
		return;
	}
	
	if (channel->flags & ASYNC_CHANNEL_FLAG_CLOSED) {
		return;
	}
	
	it->flags |= ASYNC_CHANNEL_ITERATOR_FLAG_FETCHING;
	
	if (channel->senders.first != NULL) {
		ASYNC_DEQUEUE_CUSTOM_OP(&channel->senders, send, async_channel_send_op);
		
		it->pos++;
		
		ZVAL_COPY(&it->entry, send->value);
		
		ASYNC_FINISH_OP(send);
	} else {	
		ASYNC_ALLOC_OP(op);	
		ASYNC_ENQUEUE_OP(&channel->receivers, op);
		
		if (async_await_op(op) == FAILURE) {
			ASYNC_PREPARE_EXCEPTION(&error, async_channel_closed_exception_ce, "Channel has been closed");
	
			zend_exception_set_previous(Z_OBJ_P(&error), Z_OBJ_P(&op->result));
			Z_ADDREF_P(&op->result);
			
			EG(current_execute_data)->opline--;
			zend_throw_exception_internal(&error);
			EG(current_execute_data)->opline++;
		} else if (!(channel->flags & ASYNC_CHANNEL_FLAG_CLOSED)) {
			it->pos++;
			
			ZVAL_COPY(&it->entry, &op->result);
		}
		
		ASYNC_FREE_OP(op);
	}
	
	it->flags &= ~ASYNC_CHANNEL_ITERATOR_FLAG_FETCHING;
}

static async_channel_iterator *async_channel_iterator_object_create(async_channel *channel)
{
	async_channel_iterator *it;
	
	it = emalloc(sizeof(async_channel));
	ZEND_SECURE_ZERO(it, sizeof(async_channel));
	
	zend_object_std_init(&it->std, async_channel_iterator_ce);
	it->std.handlers = &async_channel_iterator_handlers;
	
	it->pos = -1;
	it->channel = channel;
	
	ASYNC_ADDREF(&channel->std);
	
	return it;
}

static void async_channel_iterator_object_destroy(zend_object *object)
{
	async_channel_iterator *it;
	
	it = (async_channel_iterator *) object;
	
	zval_ptr_dtor(&it->entry);
	
	ASYNC_DELREF(&it->channel->std);
	
	zend_object_std_dtor(&it->std);
}

ZEND_METHOD(ChannelIterator, rewind)
{
	async_channel_iterator *it;
	
	ZEND_PARSE_PARAMETERS_NONE();
	
	it = (async_channel_iterator *) Z_OBJ_P(getThis());
	
	if (it->pos < 0 && !(it->channel->flags & ASYNC_CHANNEL_FLAG_CLOSED)) {
		fetch_next_entry(it);
	}
}

ZEND_METHOD(ChannelIterator, valid)
{
	async_channel_iterator *it;
	
	ZEND_PARSE_PARAMETERS_NONE();
	
	it = (async_channel_iterator *) Z_OBJ_P(getThis());
	
	RETURN_BOOL(it->pos >= 0 && Z_TYPE_P(&it->entry) != IS_UNDEF);
}

ZEND_METHOD(ChannelIterator, current)
{
	async_channel_iterator *it;
	
	ZEND_PARSE_PARAMETERS_NONE();
	
	it = (async_channel_iterator *) Z_OBJ_P(getThis());
	
	if (it->pos < 0) {
		fetch_next_entry(it);
	}
	
	if (Z_TYPE_P(&it->entry) != IS_UNDEF) {
		RETURN_ZVAL(&it->entry, 1, 0);
	}
}

ZEND_METHOD(ChannelIterator, key)
{
	async_channel_iterator *it;
	
	ZEND_PARSE_PARAMETERS_NONE();
	
	it = (async_channel_iterator *) Z_OBJ_P(getThis());
	
	if (it->pos < 0) {
		fetch_next_entry(it);
	}
	
	if (it->pos >= 0 && Z_TYPE_P(&it->entry) != IS_UNDEF) {
		RETURN_LONG(it->pos);
	}
}

ZEND_METHOD(ChannelIterator, next)
{
	async_channel_iterator *it;

	ZEND_PARSE_PARAMETERS_NONE();
	
	it = (async_channel_iterator *) Z_OBJ_P(getThis());
	
	zval_ptr_dtor(&it->entry);
	ZVAL_UNDEF(&it->entry);
	
	if (!(it->channel->flags & ASYNC_CHANNEL_FLAG_CLOSED)) {
		fetch_next_entry(it);
	}
}

ZEND_BEGIN_ARG_INFO(arginfo_channel_iterator_void, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry channel_iterator_functions[] = {
	ZEND_ME(ChannelIterator, rewind, arginfo_channel_iterator_void, ZEND_ACC_PUBLIC)
	ZEND_ME(ChannelIterator, valid, arginfo_channel_iterator_void, ZEND_ACC_PUBLIC)
	ZEND_ME(ChannelIterator, current, arginfo_channel_iterator_void, ZEND_ACC_PUBLIC)
	ZEND_ME(ChannelIterator, key, arginfo_channel_iterator_void, ZEND_ACC_PUBLIC)
	ZEND_ME(ChannelIterator, next, arginfo_channel_iterator_void, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};


static const zend_function_entry empty_funcs[] = {
	ZEND_FE_END
};


void async_channel_ce_register()
{
	zend_class_entry ce;

	INIT_CLASS_ENTRY(ce, "Concurrent\\Channel", channel_functions);
	async_channel_ce = zend_register_internal_class(&ce);
	async_channel_ce->ce_flags |= ZEND_ACC_FINAL;
	async_channel_ce->create_object = async_channel_object_create;
	async_channel_ce->serialize = zend_class_serialize_deny;
	async_channel_ce->unserialize = zend_class_unserialize_deny;

	memcpy(&async_channel_handlers, &std_object_handlers, sizeof(zend_object_handlers));
	async_channel_handlers.free_obj = async_channel_object_destroy;
	async_channel_handlers.dtor_obj = async_channel_object_dtor;
	async_channel_handlers.clone_obj = NULL;
	
	zend_class_implements(async_channel_ce, 1, zend_ce_aggregate);
	
	INIT_CLASS_ENTRY(ce, "Concurrent\\ChannelIterator", channel_iterator_functions);
	async_channel_iterator_ce = zend_register_internal_class(&ce);
	async_channel_iterator_ce->ce_flags |= ZEND_ACC_FINAL;
	async_channel_iterator_ce->serialize = zend_class_serialize_deny;
	async_channel_iterator_ce->unserialize = zend_class_unserialize_deny;

	memcpy(&async_channel_iterator_handlers, &std_object_handlers, sizeof(zend_object_handlers));
	async_channel_iterator_handlers.free_obj = async_channel_iterator_object_destroy;
	async_channel_iterator_handlers.clone_obj = NULL;
	
	zend_class_implements(async_channel_iterator_ce, 1, zend_ce_iterator);
	
	INIT_CLASS_ENTRY(ce, "Concurrent\\ChannelClosedException", empty_funcs);
	async_channel_closed_exception_ce = zend_register_internal_class(&ce);

	zend_do_inheritance(async_channel_closed_exception_ce, zend_ce_exception);
}


/*
 * vim: sw=4 ts=4
 * vim600: fdm=marker
 */