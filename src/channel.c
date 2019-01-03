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

#include "ext/standard/php_mt_rand.h"

zend_class_entry *async_channel_ce;
zend_class_entry *async_channel_closed_exception_ce;
zend_class_entry *async_channel_iterator_ce;

static zend_object_handlers async_channel_handlers;
static zend_object_handlers async_channel_iterator_handlers;

static async_channel_iterator *async_channel_iterator_object_create(async_channel *channel);

#define ASYNC_CHANNEL_READABLE(channel) (!((channel)->flags & ASYNC_CHANNEL_FLAG_CLOSED) || (channel)->receivers.first != NULL || (channel)->buffer.first != NULL)

typedef struct {
	async_op base;
	zval* value;
} async_channel_send_op;

static inline void forward_error(zval *cause)
{
	zval error;
	
	ASYNC_PREPARE_EXCEPTION(&error, async_channel_closed_exception_ce, "Channel has been closed");

	zend_exception_set_previous(Z_OBJ_P(&error), Z_OBJ_P(cause));
	Z_ADDREF_P(cause);
	
	EG(current_execute_data)->opline--;
	zend_throw_exception_internal(&error);
	EG(current_execute_data)->opline++;
}

static inline int fetch_noblock(async_channel *channel, zval *entry)
{
	async_channel_buffer *buffer;
	async_channel_send_op *send;

	if (channel->buffer.first != NULL) {
		ASYNC_Q_DEQUEUE(&channel->buffer, buffer);
		
		ZVAL_COPY(entry, &buffer->value);
		zval_ptr_dtor(&buffer->value);
		
		efree(buffer);
		
		// Release first pending send operation into the channel's buffer queue.
		if (channel->senders.first != NULL) {
			ASYNC_DEQUEUE_CUSTOM_OP(&channel->senders, send, async_channel_send_op);
			
			buffer = emalloc(sizeof(async_channel_buffer));
			
			ZVAL_COPY(&buffer->value, send->value);

			ASYNC_Q_ENQUEUE(&channel->buffer, buffer);
			
			ASYNC_FINISH_OP(send);
		} else {
			channel->buffered--;
		}
		
		return SUCCESS;
	}
	
	// Grab next message the first pending send operation.
	if (channel->senders.first != NULL) {
		ASYNC_DEQUEUE_CUSTOM_OP(&channel->senders, send, async_channel_send_op);
		
		ZVAL_COPY(entry, send->value);
		
		ASYNC_FINISH_OP(send);
		
		return SUCCESS;
	}
	
	return FAILURE;
}

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
	async_channel_buffer *buffer;
	
	channel = (async_channel *) object;
	
	while (channel->buffer.first != NULL) {
		ASYNC_Q_DEQUEUE(&channel->buffer, buffer);
		
		zval_ptr_dtor(&buffer->value);
		
		efree(buffer);
	}
	
	zval_ptr_dtor(&channel->error);
	
	ASYNC_DELREF(&channel->scheduler->std);
	
	zend_object_std_dtor(&channel->std);
}

ZEND_METHOD(Channel, __construct)
{
	async_channel *channel;
	
	zend_long size;
	
	size = 0;
		
	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(size)
	ZEND_PARSE_PARAMETERS_END();
	
	ASYNC_CHECK_ERROR(size < 0, "Channel buffer size must not be negative");
	
	channel = (async_channel *) Z_OBJ_P(getThis());
	
	channel->size = (uint32_t) size;
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
	
	if (channel->cancel.func != NULL) {
		ASYNC_Q_DETACH(&channel->scheduler->shutdown, &channel->cancel);
		
		channel->cancel.func(channel, (val == NULL || Z_TYPE_P(val) == IS_NULL) ? NULL : val);
	}
}

ZEND_METHOD(Channel, send)
{
	async_channel *channel;
	async_channel_buffer *buffer;
	async_channel_send_op *send;
	async_op *op;
	
	zval *val;
	
	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 1, 1)
		Z_PARAM_ZVAL(val)
	ZEND_PARSE_PARAMETERS_END();
	
	channel = (async_channel *) Z_OBJ_P(getThis());
	
	if (Z_TYPE_P(&channel->error) != IS_UNDEF) {
		forward_error(&channel->error);
	
		return;
	}
	
	ASYNC_CHECK_EXCEPTION(channel->flags & ASYNC_CHANNEL_FLAG_CLOSED, async_channel_closed_exception_ce, "Channel has been closed");
	
	// Fast forward message to first waiting receiver.
	if (channel->receivers.first != NULL) {	
		ASYNC_DEQUEUE_OP(&channel->receivers, op);
		ASYNC_RESOLVE_OP(op, val);

		return;
	}
	
	// There is space in the channel's buffer, enqueue value and return.
	if (channel->buffered < channel->size) {
		buffer = emalloc(sizeof(async_channel_buffer));
		
		ZVAL_COPY(&buffer->value, val);
		
		ASYNC_Q_ENQUEUE(&channel->buffer, buffer);
		
		channel->buffered++;
		
		return;
	}
	
	// Allocate async operationd and queue it up.
	ASYNC_ALLOC_CUSTOM_OP(send, sizeof(async_channel_send_op));
	
	send->value = val;
	
	ASYNC_ENQUEUE_OP(&channel->senders, send);
	
	if (async_await_op((async_op *) send) == FAILURE) {
		ASYNC_FORWARD_OP_ERROR(send);
	}
	
	ASYNC_FREE_OP(send);
}

typedef struct {
	async_op base;
	uint16_t pending;
	zval key;
} async_channel_select_op;

typedef struct {
	async_op base;
	async_channel_iterator *it;
	zval key;
} async_channel_select_entry;

static void continue_select(async_op *op)
{
	async_channel_select_entry *entry;
	async_channel_select_op *select;
	
	entry = (async_channel_select_entry *) op;
	select = (async_channel_select_op *) op->arg;
	
	select->pending--;
	
	if (select->base.status != ASYNC_STATUS_RUNNING) {
		return;
	}
	
	if (entry->it->channel->flags & ASYNC_CHANNEL_FLAG_CLOSED) {
		if (select->pending == 0) {
			ASYNC_FINISH_OP(select);
		}
		
		return;
	}
	
	ZVAL_COPY(&select->key, &entry->key);
	
	ASYNC_RESOLVE_OP(select, &op->result);
}

#define ASYNC_CHANNEL_SELECT_CLEANUP_ENTRIES(entries, count) do { \
	int i; \
	for (i = 0; i < count; i++) { \
		ASYNC_DELREF(&(entries)[i].it->std); \
		zval_ptr_dtor(&(entries)[i].key); \
	} \
	efree(entries); \
} while (0)

ZEND_METHOD(Channel, select)
{
	async_channel_select_entry *entries;
	async_channel_select_entry se;
	async_channel_select_op *select;
	
	zend_long block;
	int count;
	int i;
	uint32_t j;
	
	zend_long h;
	zend_string *k;

	HashTable *map;
	zval tmp;
	zval *entry;

	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 1, 2)
		Z_PARAM_ARRAY_HT(map)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(block)
	ZEND_PARSE_PARAMETERS_END();
	
	entries = ecalloc(zend_array_count(map), sizeof(async_channel_select_entry));
	count = 0;
	
	// Validate input channels and prepare channel op entries.
	ZEND_HASH_FOREACH_KEY_VAL(map, h, k, entry) {
		if (Z_TYPE_P(entry) != IS_OBJECT) {
			ASYNC_CHANNEL_SELECT_CLEANUP_ENTRIES(entries, count);
		
			zend_throw_error(NULL, "Select requires all inputs to be objects");
			return;
		}
		
		if (!instanceof_function(Z_OBJCE_P(entry), async_channel_iterator_ce)) {
			if (!instanceof_function(Z_OBJCE_P(entry), zend_ce_aggregate)) {
				ASYNC_CHANNEL_SELECT_CLEANUP_ENTRIES(entries, count);
			
				zend_throw_error(NULL, "Select requires all inputs to be channel iterators or provide such an iterator via IteratorAggregate");
				return;
			}
			
			zend_call_method_with_0_params(entry, Z_OBJCE_P(entry), NULL, "getiterator", &tmp);
			
			if (!instanceof_function(Z_OBJCE_P(&tmp), async_channel_iterator_ce)) {
				zval_ptr_dtor(&tmp);
				
				ASYNC_CHANNEL_SELECT_CLEANUP_ENTRIES(entries, count);
			
				zend_throw_error(NULL, "Aggregated iterator is not a channel iterator");
				return;
			}
			
			entries[count].it = (async_channel_iterator *) Z_OBJ_P(&tmp);
		} else {
			Z_ADDREF_P(entry);
			
			entries[count].it = (async_channel_iterator *) Z_OBJ_P(entry);
		}
		
		if (k == NULL) {
			ZVAL_LONG(&entries[count].key, h);
		} else {
			ZVAL_STR_COPY(&entries[count].key, k);
		}
		
		count++;
	} ZEND_HASH_FOREACH_END();
	
	// Perform a Fisher–Yates shuffle to randomize the channel array.
	for (i = count - 1; i > 0; i--) {
		j = php_mt_rand_common(0, i);
		se = entries[i];
		
		entries[i] = entries[j];
		entries[j] = se;
	}
	
	j = 0;
	
	// See if any input channel can provide a value without blocking.
	for (i = 0; i < count; i++) {
		if (fetch_noblock(entries[i].it->channel, &tmp) == SUCCESS) {
			array_init(return_value);
			add_next_index_zval(return_value, &entries[i].key);
			add_next_index_zval(return_value, &tmp);
			
			zval_ptr_dtor(&tmp);
			
			ASYNC_CHANNEL_SELECT_CLEANUP_ENTRIES(entries, count);
			
			return;
		}
		
		if (entries[i].it->channel->flags & ASYNC_CHANNEL_FLAG_CLOSED) {
			j++;
		}
	}
	
	// Bail out if no blocking select is requested or no input channel is ready for read.
	if (!block || count == j) {		
		array_init(return_value);
		add_next_index_null(return_value);
		add_next_index_null(return_value);
		
		ASYNC_CHANNEL_SELECT_CLEANUP_ENTRIES(entries, count);
		
		return;
	}
	
	// Alloc master select op and register an additional operation with each channel.
	ASYNC_ALLOC_CUSTOM_OP(select, sizeof(async_channel_select_op));
	
	select->pending = count - j;
	
	for (i = 0; i < count; i++) {
		if (!(entries[i].it->channel->flags & ASYNC_CHANNEL_FLAG_CLOSED)) {
			entries[i].base.status = ASYNC_STATUS_RUNNING;
			entries[i].base.callback = continue_select;
			entries[i].base.arg = select;
		
			ASYNC_ENQUEUE_OP(&entries[i].it->channel->receivers, &entries[i]);
		}
	}
	
	if (async_await_op((async_op *) select) == FAILURE) {
		ASYNC_FORWARD_OP_ERROR(select);
	}
	
	// Populate result element.
	if (EXPECTED(EG(exception) == NULL)) {
		array_init(return_value);
		
		if (Z_TYPE_P(&select->key) == IS_UNDEF) {
			add_next_index_null(return_value);
			add_next_index_null(return_value);
		} else {
			add_next_index_zval(return_value, &select->key);
			add_next_index_zval(return_value, &select->base.result);
		}
	}
	
	// Manual cleanup needed, we cannot use FREE_OP because it requires heap-allocated memory.
	for (i = 0; i < count; i++) {
		zval_ptr_dtor(&entries[i].base.result);
		zval_ptr_dtor(&entries[i].key);
		
		ASYNC_DELREF(&entries[i].it->std);
		
		ASYNC_Q_DETACH(&entries[i].it->channel->receivers, (async_op *) &entries[i]);
	}
	
	efree(entries);
	
	zval_ptr_dtor(&select->key);
	
	ASYNC_FREE_OP(select);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_channel_ctor, 0, 0, 0)
	ZEND_ARG_TYPE_INFO(0, capacity, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_channel_get_iterator, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_channel_close, 0, 0, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, error, Throwable, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_channel_send, 0, 1, IS_VOID, 0)
	ZEND_ARG_INFO(0, message)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_channel_select, 0, 1, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, channels, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, block, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry channel_functions[] = {
	ZEND_ME(Channel, __construct, arginfo_channel_ctor, ZEND_ACC_PUBLIC)
	ZEND_ME(Channel, getIterator, arginfo_channel_get_iterator, ZEND_ACC_PUBLIC)
	ZEND_ME(Channel, close, arginfo_channel_close, ZEND_ACC_PUBLIC)
	ZEND_ME(Channel, send, arginfo_channel_send, ZEND_ACC_PUBLIC)
	ZEND_ME(Channel, select, arginfo_channel_select, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	ZEND_FE_END
};


static void fetch_next_entry(async_channel_iterator *it)
{
	async_channel *channel;
	async_op *op;
	
	ASYNC_CHECK_ERROR(it->flags & ASYNC_CHANNEL_ITERATOR_FLAG_FETCHING, "Cannot advance iterator while already awaiting next channel value");
	
	channel = it->channel;
	
	if (fetch_noblock(channel, &it->entry) == SUCCESS) {
		it->pos++;
	
		return;
	}
	
	// Queue up receiver and mark the iterator as fetching next value.
	it->flags |= ASYNC_CHANNEL_ITERATOR_FLAG_FETCHING;
	
	ASYNC_ALLOC_OP(op);
	ASYNC_ENQUEUE_OP(&channel->receivers, op);
	
	if (async_await_op(op) == FAILURE) {
		forward_error(&op->result);
	} else if (!(channel->flags & ASYNC_CHANNEL_FLAG_CLOSED)) {
		it->pos++;
		
		ZVAL_COPY(&it->entry, &op->result);
	}
	
	ASYNC_FREE_OP(op);
	
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
	
	zval_ptr_dtor(&it->entry);
	ZVAL_UNDEF(&it->entry);
	
	if (it->pos < 0 && ASYNC_CHANNEL_READABLE(it->channel)) {
		fetch_next_entry(it);
	} else if (Z_TYPE_P(&it->channel->error) != IS_UNDEF) {
		forward_error(&it->channel->error);
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
	
	if (it->pos < 0 && ASYNC_CHANNEL_READABLE(it->channel)) {
		fetch_next_entry(it);
	} else if (Z_TYPE_P(&it->channel->error) != IS_UNDEF) {
		forward_error(&it->channel->error);
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
	
	if (it->pos < 0 && ASYNC_CHANNEL_READABLE(it->channel)) {
		fetch_next_entry(it);
	} else if (Z_TYPE_P(&it->channel->error) != IS_UNDEF) {
		forward_error(&it->channel->error);
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
	
	if (ASYNC_CHANNEL_READABLE(it->channel)) {
		fetch_next_entry(it);
	} else if (Z_TYPE_P(&it->channel->error) != IS_UNDEF) {
		forward_error(&it->channel->error);
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
