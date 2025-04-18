/* Copyright (c) 2017-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "lib-event-private.h"
#include "event-filter.h"
#include "array.h"
#include "hash.h"
#include "llist.h"
#include "time-util.h"
#include "str.h"
#include "strescape.h"
#include "ioloop-private.h"

#include <ctype.h>

HASH_TABLE_DEFINE_TYPE(category_set, void *, const struct event_category *);

enum event_code {
	EVENT_CODE_ALWAYS_LOG_SOURCE	= 'a',
	EVENT_CODE_CATEGORY		= 'c',
	EVENT_CODE_TV_LAST_SENT		= 'l',
	EVENT_CODE_SENDING_NAME		= 'n',
	EVENT_CODE_SOURCE		= 's',

	EVENT_CODE_FIELD_INTMAX		= 'I',
	EVENT_CODE_FIELD_STR		= 'S',
	EVENT_CODE_FIELD_TIMEVAL	= 'T',
	EVENT_CODE_FIELD_IP		= 'P',
	EVENT_CODE_FIELD_STRLIST	= 'L',
};

/* Internal event category state.

   Each (unique) event category maps to one internal category.  (I.e., if
   two places attempt to register the same category, they will share the
   internal state.)

   This is required in order to support multiple registrations of the same
   category.  Currently, the only situation in which this occurs is the
   stats process receiving categories from other processes and also using
   the same categories internally.

   During registration, we look up the internal state based on the new
   category's name.  If found, we use it after sanity checking that the two
   are identical (i.e., they both have the same name and parent).  If not
   found, we allocate a new internal state and use it.

   We stash a pointer to the internal state in struct event_category (the
   "internal" member).  As a result, all category structs for the same
   category point to the same internal state. */
struct event_internal_category {
	/* More than one category can be represented by the internal state.
	   To give consumers a unique but consistent category pointer, we
	   return a pointer to this 'representative' category structure.
	   Because we allocated it, we know that it will live exactly as
	   long as we need it to. */
	struct event_category representative;

	struct event_internal_category *parent;
	char *name;
	int refcount;
};

struct event_reason {
	struct event *event;
};

struct event_category_iterator {
	HASH_TABLE_TYPE(category_set) hash;
	struct hash_iterate_context *iter;
};

extern struct event_passthrough event_passthrough_vfuncs;

static struct event *events = NULL;
static struct event *current_global_event = NULL;
static struct event *event_last_passthrough = NULL;
static ARRAY(event_callback_t *) event_handlers;
static ARRAY(event_category_callback_t *) event_category_callbacks;
static ARRAY(struct event_internal_category *) event_registered_categories_internal;
static ARRAY(struct event_category *) event_registered_categories_representative;
static ARRAY(struct event *) global_event_stack;
static uint64_t event_id_counter = 0;

static void get_self_rusage(struct rusage *ru_r)
{
	if (getrusage(RUSAGE_SELF, ru_r) < 0)
		i_fatal("getrusage() failed: %m");
}

static struct event *
event_create_internal(struct event *parent, const char *source_filename,
		      unsigned int source_linenum);
static struct event_internal_category *
event_category_find_internal(const char *name);

static struct event *last_passthrough_event(void)
{
	i_assert(event_last_passthrough != NULL);
	return event_last_passthrough;
}

static void event_copy_parent_defaults(struct event *event,
				       const struct event *parent)
{
	event->always_log_source = parent->always_log_source;
	event->passthrough = parent->passthrough;
	event->min_log_level = parent->min_log_level;
	event->forced_debug = parent->forced_debug;
	event->forced_never_debug = parent->forced_never_debug;
	event->disable_callbacks = parent->disable_callbacks;
}

static bool
event_find_category(const struct event *event,
		    const struct event_category *category);

static void event_set_changed(struct event *event)
{
	event->change_id++;
	/* It's unlikely that change_id will ever wrap, but lets be safe
	   anyway. */
	if (event->change_id == 0 ||
	    event->change_id == event->sent_to_stats_id)
		event->change_id++;
}

static bool
event_call_callbacks(struct event *event, enum event_callback_type type,
		     struct failure_context *ctx, const char *fmt, va_list args)
{
	event_callback_t *callback;

	if (event->disable_callbacks)
		return TRUE;

	array_foreach_elem(&event_handlers, callback) {
		bool ret;

		T_BEGIN {
			ret = callback(event, type, ctx, fmt, args);
		} T_END;
		if (!ret) {
			/* event sending was stopped */
			return FALSE;
		}
	}
	return TRUE;
}

static void
event_call_callbacks_noargs(struct event *event,
			    enum event_callback_type type, ...)
{
	va_list args;

	/* the args are empty and not used for anything, but there doesn't seem
	   to be any nice and standard way of passing an initialized va_list
	   as a parameter without va_start(). */
	va_start(args, type);
	(void)event_call_callbacks(event, type, NULL, NULL, args);
	va_end(args);
}

void event_copy_categories(struct event *to, struct event *from)
{
	unsigned int cat_count;
	struct event_category *const *categories =
		event_get_categories(from, &cat_count);
	for (unsigned int i = 1; i <= cat_count; i++)
		event_add_category(to, categories[cat_count-i]);
}

void event_copy_fields(struct event *to, struct event *from)
{
	const struct event_field *fld;
	unsigned int count;
	const char *const *values;

	if (!array_is_created(&from->fields))
		return;
	array_foreach(&from->fields, fld) {
		switch (fld->value_type) {
		case EVENT_FIELD_VALUE_TYPE_STR:
			event_add_str(to, fld->key, fld->value.str);
			break;
		case EVENT_FIELD_VALUE_TYPE_INTMAX:
			event_add_int(to, fld->key, fld->value.intmax);
			break;
		case EVENT_FIELD_VALUE_TYPE_TIMEVAL:
			event_add_timeval(to, fld->key, &fld->value.timeval);
			break;
		case EVENT_FIELD_VALUE_TYPE_IP:
			event_add_ip(to, fld->key, &fld->value.ip);
			break;
		case EVENT_FIELD_VALUE_TYPE_STRLIST:
			values = array_get(&fld->value.strlist, &count);
			for (unsigned int i = 0; i < count; i++)
				event_strlist_append(to, fld->key, values[i]);
			break;
		default:
			break;
		}
	}
}

bool event_has_all_categories(struct event *event, const struct event *other)
{
	struct event_category **cat;
	if (!array_is_created(&other->categories))
		return TRUE;
	if (!array_is_created(&event->categories))
		return FALSE;
	array_foreach_modifiable(&other->categories, cat) {
		if (!event_find_category(event, *cat))
			return FALSE;
	}
	return TRUE;
}

bool event_has_all_fields(struct event *event, const struct event *other)
{
	struct event_field *fld;
	if (!array_is_created(&other->fields))
		return TRUE;
	array_foreach_modifiable(&other->fields, fld) {
		if (event_find_field_nonrecursive(event, fld->key) == NULL)
			return FALSE;
	}
	return TRUE;
}

struct event *event_dup(const struct event *source)
{
	struct event *ret =
		event_create_internal(source->parent, source->source_filename,
				      source->source_linenum);
	string_t *str = t_str_new(256);
	const char *err;
	event_export(source, str);
	if (!event_import(ret, str_c(str), &err))
		i_panic("event_import(%s) failed: %s", str_c(str), err);
	ret->tv_created_ioloop = source->tv_created_ioloop;
	return ret;
}

/*
 * Copy the source's categories and fields recursively.
 *
 * We recurse to the parent before copying this event's data because we may
 * be overriding a field.
 */
static void event_flatten_recurse(struct event *dst, struct event *src,
				  struct event *limit)
{
	if (src->parent != limit)
		event_flatten_recurse(dst, src->parent, limit);

	event_copy_categories(dst, src);
	event_copy_fields(dst, src);
}

struct event *event_flatten(struct event *src)
{
	struct event *dst;

	/* If we don't have a parent or a global event,
	   we have nothing to flatten. */
	if (src->parent == NULL && current_global_event == NULL)
		return event_ref(src);

	/* We have to flatten the event. */

	dst = event_create_internal(NULL, src->source_filename,
				    src->source_linenum);
	dst = event_set_name(dst, src->sending_name);

	if (current_global_event != NULL)
		event_flatten_recurse(dst, current_global_event, NULL);
	event_flatten_recurse(dst, src, NULL);

	dst->tv_created_ioloop = src->tv_created_ioloop;
	dst->tv_created = src->tv_created;
	dst->tv_last_sent = src->tv_last_sent;

	return dst;
}

static inline void replace_parent_ref(struct event *event, struct event *new)
{
	if (event->parent == new)
		return; /* no-op */

	if (new != NULL)
		event_ref(new);

	event_unref(&event->parent);

	event->parent = new;
}

/*
 * Minimize the event and its ancestry.
 *
 * In general, the chain of parents starting from this event can be divided
 * up into four consecutive ranges:
 *
 *  1. the event itself
 *  2. a range of events that should be flattened into the event itself
 *  3. a range of trivial (i.e., no categories or fields) events that should
 *     be skipped
 *  4. the rest of the chain
 *
 * Except for the first range, the event itself, the remaining ranges can
 * have zero events.
 *
 * As the names of these ranges imply, we want to flatten certain parts of
 * the ancestry, skip other parts of the ancestry and leave the remainder
 * untouched.
 *
 * For example, suppose that we have an event (A) with ancestors forming the
 * following graph:
 *
 *	A -> B -> C -> D -> E -> F
 *
 * Further, suppose that B, C, and F contain some categories or fields but
 * have not yet been sent to an external process that knows how to reference
 * previously encountered events, and D contains no fields or categories of
 * its own (but it inherits some from E and F).
 *
 * We can define the 4 ranges:
 *
 *	A:     the event
 *	B-C:   flattening
 *	D:     skipping
 *	E-end: the rest
 *
 * The output would therefore be:
 *
 *	G -> E -> F
 *
 * where G contains the fields and categories of A, B, and C (and trivially
 * D because D was empty).
 *
 * Note that even though F has not yet been sent out, we send it now because
 * it is part of the "rest" range.
 *
 * TODO: We could likely apply this function recursively on the "rest"
 * range, but further investigation is required to determine whether it is
 * worth it.
 */
struct event *event_minimize(struct event *event)
{
	struct event *flatten_bound;
	struct event *skip_bound;
	struct event *new_event;
	struct event *cur;

	if (event->parent == NULL)
		return event_ref(event);

	/* find the bound for field/category flattening */
	flatten_bound = NULL;
	for (cur = event->parent; cur != NULL; cur = cur->parent) {
		if (cur->sent_to_stats_id == 0 &&
		    timeval_cmp(&cur->tv_created_ioloop,
				&event->tv_created_ioloop) == 0)
			continue;

		flatten_bound = cur;
		break;
	}

	/* continue to find the bound for empty event skipping */
	skip_bound = NULL;
	for (; cur != NULL; cur = cur->parent) {
		if (cur->sent_to_stats_id == 0 &&
		    (!array_is_created(&cur->fields) ||
		     array_is_empty(&cur->fields)) &&
		    (!array_is_created(&cur->categories) ||
		     array_is_empty(&cur->categories)))
			continue;

		skip_bound = cur;
		break;
	}

	/* fast path - no flattening and no skipping to do */
	if ((event->parent == flatten_bound) &&
	    (event->parent == skip_bound))
		return event_ref(event);

	new_event = event_dup(event);

	/* flatten */
	event_flatten_recurse(new_event, event, flatten_bound);
	replace_parent_ref(new_event, flatten_bound);

	/* skip */
	replace_parent_ref(new_event, skip_bound);

	return new_event;
}

static struct event *
event_create_internal(struct event *parent, const char *source_filename,
		      unsigned int source_linenum)
{
	struct event *event;
	pool_t pool = pool_alloconly_create(MEMPOOL_GROWING"event", 1024);

	event = p_new(pool, struct event, 1);
	event->refcount = 1;
	event->id = ++event_id_counter;
	event->pool = pool;
	event->tv_created_ioloop = ioloop_timeval;
	event->min_log_level = LOG_TYPE_INFO;
	i_gettimeofday(&event->tv_created);
	event->source_filename = p_strdup(pool, source_filename);
	event->source_linenum = source_linenum;
	event->change_id = 1;
	if (parent != NULL) {
		event->parent = parent;
		event_ref(event->parent);
		event_copy_parent_defaults(event, parent);
	}
	DLLIST_PREPEND(&events, event);
	return event;
}

#undef event_create
struct event *event_create(struct event *parent, const char *source_filename,
			   unsigned int source_linenum)
{
	struct event *event;

	event = event_create_internal(parent, source_filename, source_linenum);
	(void)event_call_callbacks_noargs(event, EVENT_CALLBACK_TYPE_CREATE);
	return event;
}

#undef event_create_passthrough
struct event_passthrough *
event_create_passthrough(struct event *parent, const char *source_filename,
			 unsigned int source_linenum)
{
	if (!parent->passthrough) {
		if (event_last_passthrough != NULL) {
			/* API is being used in a wrong or dangerous way */
			i_panic("Can't create multiple passthrough events - "
				"finish the earlier with ->event()");
		}
		struct event *event =
			event_create(parent, source_filename, source_linenum);
		event->passthrough = TRUE;
		/* This event only intends to extend the parent event.
		   Use the parent's creation timestamp. */
		event->tv_created_ioloop = parent->tv_created_ioloop;
		event->tv_created = parent->tv_created;
		memcpy(&event->ru_last, &parent->ru_last, sizeof(parent->ru_last));
		event_last_passthrough = event;
	} else {
		event_last_passthrough = parent;
	}
	return &event_passthrough_vfuncs;
}

struct event *event_ref(struct event *event)
{
	i_assert(event->refcount > 0);

	event->refcount++;
	return event;
}

void event_unref(struct event **_event)
{
	struct event *event = *_event;

	if (event == NULL)
		return;
	*_event = NULL;

	i_assert(event->refcount > 0);
	if (--event->refcount > 0)
		return;
	i_assert(event != current_global_event);

	event_call_callbacks_noargs(event, EVENT_CALLBACK_TYPE_FREE);

	if (event_last_passthrough == event)
		event_last_passthrough = NULL;
	if (event->log_prefix_from_system_pool)
		i_free(event->log_prefix);
	i_free(event->sending_name);
	event_unref(&event->parent);

	DLLIST_REMOVE(&events, event);
	pool_unref(&event->pool);
}

struct event *events_get_head(void)
{
	return events;
}

struct event *event_push_global(struct event *event)
{
	i_assert(event != NULL);

	if (current_global_event != NULL) {
		if (!array_is_created(&global_event_stack))
			i_array_init(&global_event_stack, 4);
		array_push_back(&global_event_stack, &current_global_event);
	}
	current_global_event = event;
	return event;
}

struct event *event_pop_global(struct event *event)
{
	i_assert(event != NULL);
	i_assert(event == current_global_event);
	/* If the active context's root event is popped, we'll assert-crash
	   later on when deactivating the context and the root event no longer
	   exists. */
	i_assert(event != io_loop_get_active_global_root());

	if (!array_is_created(&global_event_stack) ||
	    array_count(&global_event_stack) == 0)
		current_global_event = NULL;
	else {
		unsigned int event_count;
		struct event *const *events =
			array_get(&global_event_stack, &event_count);

		i_assert(event_count > 0);
		current_global_event = events[event_count-1];
		array_delete(&global_event_stack, event_count-1, 1);
	}
	return current_global_event;
}

struct event *event_get_global(void)
{
	return current_global_event;
}

#undef event_reason_begin
struct event_reason *
event_reason_begin(const char *reason_code, const char *source_filename,
		   unsigned int source_linenum)
{
	struct event_reason *reason;

	reason = i_new(struct event_reason, 1);
	reason->event = event_create(event_get_global(),
				     source_filename, source_linenum);
	event_strlist_append(reason->event, EVENT_REASON_CODE, reason_code);
	event_push_global(reason->event);
	return reason;
}

void event_reason_end(struct event_reason **_reason)
{
	struct event_reason *reason = *_reason;

	if (reason == NULL)
		return;
	event_pop_global(reason->event);
	/* This event was created only for global use. It shouldn't be
	   permanently stored anywhere. This assert could help catch bugs. */
	i_assert(reason->event->refcount == 1);
	event_unref(&reason->event);
	i_free(reason);
}

const char *event_reason_code(const char *module, const char *name)
{
	return event_reason_code_prefix(module, "", name);
}

static bool event_reason_code_module_validate(const char *module)
{
	const char *p;

	for (p = module; *p != '\0'; p++) {
		if (*p == ' ' || *p == '-' || *p == ':')
			return FALSE;
		if (i_isupper(*p))
			return FALSE;
	}
	return TRUE;
}

const char *event_reason_code_prefix(const char *module,
				     const char *name_prefix, const char *name)
{
	const char *p;

	i_assert(module[0] != '\0');
	i_assert(name[0] != '\0');

	if (!event_reason_code_module_validate(module)) {
		i_panic("event_reason_code_prefix(): "
			"Invalid module '%s'", module);
	}
	if (!event_reason_code_module_validate(name_prefix)) {
		i_panic("event_reason_code_prefix(): "
			"Invalid name_prefix '%s'", name_prefix);
	}

	string_t *str = t_str_new(strlen(module) + 1 +
				  strlen(name_prefix) + strlen(name));
	str_append(str, module);
	str_append_c(str, ':');
	str_append(str, name_prefix);

	for (p = name; *p != '\0'; p++) {
		switch (*p) {
		case ' ':
		case '-':
			str_append_c(str, '_');
			break;
		case ':':
			i_panic("event_reason_code_prefix(): "
				"name has ':' (%s, %s%s)",
				module, name_prefix, name);
		default:
			str_append_c(str, i_tolower(*p));
			break;
		}
	}
	return str_c(str);
}

static struct event *
event_set_log_prefix(struct event *event, const char *prefix, bool append)
{
	event->log_prefix_callback = NULL;
	event->log_prefix_callback_context = NULL;
	if (event->log_prefix == NULL) {
		/* allocate the first log prefix from the pool */
		event->log_prefix = p_strdup(event->pool, prefix);
	} else {
		/* log prefix is being updated multiple times -
		   switch to system pool so we don't keep leaking memory */
		if (event->log_prefix_from_system_pool)
			i_free(event->log_prefix);
		else
			event->log_prefix_from_system_pool = TRUE;
		event->log_prefix = i_strdup(prefix);
	}
	event->log_prefix_replace = !append;
	return event;
}

struct event *
event_set_append_log_prefix(struct event *event, const char *prefix)
{
	return event_set_log_prefix(event, prefix, TRUE);
}

struct event *event_replace_log_prefix(struct event *event, const char *prefix)
{
	return event_set_log_prefix(event, prefix, FALSE);
}

struct event *
event_drop_parent_log_prefixes(struct event *event, unsigned int count)
{
	event->log_prefixes_dropped = count;
	return event;
}

#undef event_set_log_prefix_callback
struct event *
event_set_log_prefix_callback(struct event *event,
			      bool replace,
			      event_log_prefix_callback_t *callback,
			      void *context)
{
	if (event->log_prefix_from_system_pool)
		i_free(event->log_prefix);
	else
		event->log_prefix = NULL;
	event->log_prefix_replace = replace;
	event->log_prefix_callback = callback;
	event->log_prefix_callback_context = context;
	return event;
}

#undef event_set_log_message_callback
struct event *
event_set_log_message_callback(struct event *event,
			       event_log_message_callback_t *callback,
			       void *context)
{
	event->log_message_callback = callback;
	event->log_message_callback_context = context;
	return event;
}

void event_disable_callbacks(struct event *event)
{
	event->disable_callbacks = TRUE;
}

#undef event_unset_log_message_callback
void event_unset_log_message_callback(struct event *event,
				      event_log_message_callback_t *callback,
				      void *context)
{
	i_assert(event->log_message_callback == callback);
	i_assert(event->log_message_callback_context == context);

	event->log_message_callback = NULL;
	event->log_message_callback_context = NULL;
}

struct event *
event_set_name(struct event *event, const char *name)
{
	i_free(event->sending_name);
	event->sending_name = i_strdup(name);
	return event;
}

struct event *
event_set_source(struct event *event, const char *filename,
		 unsigned int linenum, bool literal_fname)
{
	if (strcmp(event->source_filename, filename) != 0) {
		event->source_filename = literal_fname ? filename :
			p_strdup(event->pool, filename);
	}
	event->source_linenum = linenum;
	return event;
}

struct event *event_set_always_log_source(struct event *event)
{
	event->always_log_source = TRUE;
	return event;
}

struct event *event_set_min_log_level(struct event *event, enum log_type level)
{
	event->min_log_level = level;
	event_recalculate_debug_level(event);
	return event;
}

enum log_type event_get_min_log_level(const struct event *event)
{
	return event->min_log_level;
}

struct event *event_set_ptr(struct event *event, const char *key, void *value)
{
	struct event_pointer *p;

	if (!array_is_created(&event->pointers))
		p_array_init(&event->pointers, event->pool, 4);
	else {
		/* replace existing pointer if the key already exists */
		array_foreach_modifiable(&event->pointers, p) {
			if (strcmp(p->key, key) == 0) {
				p->value = value;
				return event;
			}
		}
	}
	p = array_append_space(&event->pointers);
	p->key = p_strdup(event->pool, key);
	p->value = value;
	return event;
}

void *event_get_ptr(const struct event *event, const char *key)
{
	const struct event_pointer *p;

	if (!array_is_created(&event->pointers))
		return NULL;
	array_foreach(&event->pointers, p) {
		if (strcmp(p->key, key) == 0)
			return p->value;
	}
	return NULL;
}

struct event_category *event_category_find_registered(const char *name)
{
	struct event_category *cat;

	array_foreach_elem(&event_registered_categories_representative, cat) {
		if (strcmp(cat->name, name) == 0)
			return cat;
	}
	return NULL;
}

static struct event_internal_category *
event_category_find_internal(const char *name)
{
	struct event_internal_category *internal;

	array_foreach_elem(&event_registered_categories_internal, internal) {
		if (strcmp(internal->name, name) == 0)
			return internal;
	}

	return NULL;
}

struct event_category *const *
event_get_registered_categories(unsigned int *count_r)
{
	return array_get(&event_registered_categories_representative, count_r);
}

static void
event_category_add_to_array(struct event_internal_category *internal)
{
	struct event_category *representative = &internal->representative;

	array_push_back(&event_registered_categories_internal, &internal);
	array_push_back(&event_registered_categories_representative,
			&representative);
}

static struct event_category *
event_category_register(struct event_category *category)
{
	struct event_internal_category *internal = category->internal;
	event_category_callback_t *callback;
	bool allocated;

	if (internal != NULL)
		return &internal->representative; /* case 2 - see below */

	/* register parent categories first */
	if (category->parent != NULL)
		(void) event_category_register(category->parent);

	/* There are four cases we need to handle:

	   1) a new category is registered
	   2) same category struct is re-registered - already handled above
	      internal NULL check
	   3) different category struct is registered, but it is identical
	      to the previously registered one
	   4) different category struct is registered, and it is different
	      from the previously registered one - a programming error */
	internal = event_category_find_internal(category->name);
	if (internal == NULL) {
		/* case 1: first time we saw this name - allocate new */
		internal = i_new(struct event_internal_category, 1);
		if (category->parent != NULL)
			internal->parent = category->parent->internal;
		internal->name = i_strdup(category->name);
		internal->refcount = 1;
		internal->representative.name = internal->name;
		internal->representative.parent = category->parent;
		internal->representative.internal = internal;

		event_category_add_to_array(internal);

		allocated = TRUE;
	} else {
		/* case 3 or 4: someone registered this name before - share */
		if ((category->parent != NULL) &&
		    (internal->parent != category->parent->internal)) {
			/* case 4 */
			struct event_internal_category *other =
				category->parent->internal;

			i_panic("event category parent mismatch detected: "
				"category %p internal %p (%s), "
				"internal parent %p (%s), public parent %p (%s)",
				category, internal, internal->name,
				internal->parent, internal->parent->name,
				other, other->name);
		}

		internal->refcount++;

		allocated = FALSE;
	}

	category->internal = internal;

	if (!allocated) {
		/* not the first registration of this category */
		return &internal->representative;
	}

	array_foreach_elem(&event_category_callbacks, callback) T_BEGIN {
		callback(&internal->representative);
	} T_END;

	return &internal->representative;
}

static bool
event_find_category(const struct event *event,
		    const struct event_category *category)
{
	struct event_internal_category *internal = category->internal;

	/* make sure we're always looking for a representative */
	i_assert(category == &internal->representative);

	return array_lsearch_ptr(&event->categories, category) != NULL;
}

struct event *
event_add_categories(struct event *event,
		     struct event_category *const *categories)
{
	struct event_category *representative;

	if (!array_is_created(&event->categories))
		p_array_init(&event->categories, event->pool, 4);

	for (unsigned int i = 0; categories[i] != NULL; i++) {
		representative = event_category_register(categories[i]);
		if (!event_find_category(event, representative))
			array_push_back(&event->categories, &representative);
	}
	event_set_changed(event);
	event_recalculate_debug_level(event);
	return event;
}

struct event *
event_add_category(struct event *event, struct event_category *category)
{
	struct event_category *const categories[] = { category, NULL };
	return event_add_categories(event, categories);
}

struct event_field *
event_find_field_nonrecursive(const struct event *event, const char *key)
{
	struct event_field *field;

	if (!array_is_created(&event->fields))
		return NULL;

	array_foreach_modifiable(&event->fields, field) {
		if (strcmp(field->key, key) == 0)
			return field;
	}
	return NULL;
}

const struct event_field *
event_find_field_recursive(const struct event *event, const char *key)
{
	const struct event_field *field;

	do {
		if ((field = event_find_field_nonrecursive(event, key)) != NULL)
			return field;
		event = event->parent;
	} while (event != NULL);

	/* check also the global event and its parents */
	event = event_get_global();
	while (event != NULL) {
		if ((field = event_find_field_nonrecursive(event, key)) != NULL)
			return field;
		event = event->parent;
	}
	return NULL;
}

static void
event_get_recursive_strlist(const struct event *event, pool_t pool,
			    const char *key, ARRAY_TYPE(const_string) *dest)
{
	const struct event_field *field;
	const char *str;

	if (event == NULL)
		return;

	field = event_find_field_nonrecursive(event, key);
	if (field != NULL) {
		if (field->value_type != EVENT_FIELD_VALUE_TYPE_STRLIST) {
			/* Value type unexpectedly changed. Stop recursing. */
			return;
		}
		array_foreach_elem(&field->value.strlist, str) {
			if (array_lsearch(dest, &str, i_strcmp_p) == NULL) {
				if (pool != NULL)
					str = p_strdup(pool, str);
				array_push_back(dest, &str);
			}
		}
	}
	event_get_recursive_strlist(event->parent, pool, key, dest);
}

const char *
event_find_field_recursive_str(const struct event *event, const char *key)
{
	const struct event_field *field;

	field = event_find_field_recursive(event, key);
	if (field == NULL)
		return NULL;

	switch (field->value_type) {
	case EVENT_FIELD_VALUE_TYPE_STR:
		return field->value.str;
	case EVENT_FIELD_VALUE_TYPE_INTMAX:
		return t_strdup_printf("%jd", field->value.intmax);
	case EVENT_FIELD_VALUE_TYPE_TIMEVAL:
		return t_strdup_printf("%"PRIdTIME_T".%u",
			field->value.timeval.tv_sec,
			(unsigned int)field->value.timeval.tv_usec);
	case EVENT_FIELD_VALUE_TYPE_IP:
		return net_ip2addr(&field->value.ip);
	case EVENT_FIELD_VALUE_TYPE_STRLIST: {
		ARRAY_TYPE(const_string) list;
		t_array_init(&list, 8);
		/* This is a bit different, because it needs to be merging
		   all of the parent events' and global events' lists
		   together. */
		event_get_recursive_strlist(event, NULL, key, &list);
		event_get_recursive_strlist(event_get_global(), NULL,
					    key, &list);
		return t_array_const_string_join(&list, ",");
	}
	}
	i_unreached();
}

static struct event_field *
event_get_field(struct event *event, const char *key, bool clear)
{
	struct event_field *field;

	field = event_find_field_nonrecursive(event, key);
	if (field == NULL) {
		if (!array_is_created(&event->fields))
			p_array_init(&event->fields, event->pool, 8);
		field = array_append_space(&event->fields);
		field->key = p_strdup(event->pool, key);
	} else if (clear) {
		i_zero(&field->value);
	}
	event_set_changed(event);
	return field;
}

struct event *
event_add_str(struct event *event, const char *key, const char *value)
{
	struct event_field *field;

	if (value == NULL) {
		/* Silently ignoring is perhaps better than assert-crashing?
		   However, if the field already exists, this should be the
		   same as event_field_clear() */
		if (event_find_field_recursive(event, key) == NULL)
			return event;
		value = "";
	}

	field = event_get_field(event, key, TRUE);
	field->value_type = EVENT_FIELD_VALUE_TYPE_STR;
	field->value.str = p_strdup(event->pool, value);
	return event;
}

struct event *
event_strlist_append(struct event *event, const char *key, const char *value)
{
	struct event_field *field = event_get_field(event, key, FALSE);

	if (field->value_type != EVENT_FIELD_VALUE_TYPE_STRLIST ||
	    !array_is_created(&field->value.strlist)) {
		field->value_type = EVENT_FIELD_VALUE_TYPE_STRLIST;
		p_array_init(&field->value.strlist, event->pool, 1);
	}

	/* lets not add empty values there though */
	if (value == NULL)
		return event;

	const char *str = p_strdup(event->pool, value);
	if (array_lsearch(&field->value.strlist, &str, i_strcmp_p) == NULL)
		array_push_back(&field->value.strlist, &str);
	return event;
}

struct event *
event_strlist_replace(struct event *event, const char *key,
		      const char *const *values, unsigned int count)
{
	struct event_field *field = event_get_field(event, key, TRUE);
	field->value_type = EVENT_FIELD_VALUE_TYPE_STRLIST;

	for (unsigned int i = 0; i < count; i++)
		event_strlist_append(event, key, values[i]);
	return event;
}

struct event *
event_strlist_copy_recursive(struct event *dest, const struct event *src,
			     const char *key)
{
	event_strlist_append(dest, key, NULL);
	struct event_field *field = event_get_field(dest, key, FALSE);
	i_assert(field != NULL);
	event_get_recursive_strlist(src, dest->pool, key,
				    &field->value.strlist);
	return dest;
}

struct event *
event_add_int(struct event *event, const char *key, intmax_t num)
{
	struct event_field *field;

	field = event_get_field(event, key, TRUE);
	field->value_type = EVENT_FIELD_VALUE_TYPE_INTMAX;
	field->value.intmax = num;
	return event;
}

struct event *
event_add_int_nonzero(struct event *event, const char *key, intmax_t num)
{
	if (num != 0)
		return event_add_int(event, key, num);
	return event;
}

struct event *
event_inc_int(struct event *event, const char *key, intmax_t num)
{
	struct event_field *field;

	field = event_find_field_nonrecursive(event, key);
	if (field == NULL || field->value_type != EVENT_FIELD_VALUE_TYPE_INTMAX)
		return event_add_int(event, key, num);

	field->value.intmax += num;
	event_set_changed(event);
	return event;
}

struct event *
event_add_timeval(struct event *event, const char *key,
		  const struct timeval *tv)
{
	struct event_field *field;

	field = event_get_field(event, key, TRUE);
	field->value_type = EVENT_FIELD_VALUE_TYPE_TIMEVAL;
	field->value.timeval = *tv;
	return event;
}

struct event *
event_add_ip(struct event *event, const char *key, const struct ip_addr *ip)
{
	struct event_field *field;

	if (ip->family == 0) {
		/* ignore nonexistent IP (similar to
		   event_add_str(value=NULL)) */
		if (event_find_field_recursive(event, key) != NULL)
			event_field_clear(event, key);
		return event;
	}

	field = event_get_field(event, key, TRUE);
	field->value_type = EVENT_FIELD_VALUE_TYPE_IP;
	field->value.ip = *ip;
	return event;
}

struct event *
event_add_fields(struct event *event,
		 const struct event_add_field *fields)
{
	for (unsigned int i = 0; fields[i].key != NULL; i++) {
		if (fields[i].value != NULL)
			event_add_str(event, fields[i].key, fields[i].value);
		else if (fields[i].value_timeval.tv_sec != 0) {
			event_add_timeval(event, fields[i].key,
					  &fields[i].value_timeval);
		} else if (fields[i].value_ip.family != 0) {
			event_add_ip(event, fields[i].key, &fields[i].value_ip);
		} else {
			event_add_int(event, fields[i].key,
				      fields[i].value_intmax);
		}
	}
	return event;
}

void event_field_clear(struct event *event, const char *key)
{
	event_add_str(event, key, "");
}

struct event *event_get_parent(const struct event *event)
{
	return event->parent;
}

pool_t event_get_pool(const struct event *event)
{
	return event->pool;
}

void event_get_create_time(const struct event *event, struct timeval *tv_r)
{
	*tv_r = event->tv_created;
}

bool event_get_last_send_time(const struct event *event, struct timeval *tv_r)
{
	*tv_r = event->tv_last_sent;
	return tv_r->tv_sec != 0;
}

void event_get_last_duration(const struct event *event,
			     uintmax_t *duration_usecs_r)
{
	if (event->tv_last_sent.tv_sec == 0) {
		*duration_usecs_r = 0;
		return;
	}
	long long diff = timeval_diff_usecs(&event->tv_last_sent,
					    &event->tv_created);
	i_assert(diff >= 0);
	*duration_usecs_r = diff;
}

const struct event_field *
event_get_fields(const struct event *event, unsigned int *count_r)
{
	if (!array_is_created(&event->fields)) {
		*count_r = 0;
		return NULL;
	}
	return array_get(&event->fields, count_r);
}

struct event_category *const *
event_get_categories(const struct event *event, unsigned int *count_r)
{
	if (!array_is_created(&event->categories)) {
		*count_r = 0;
		return NULL;
	}
	return array_get(&event->categories, count_r);
}

static void
insert_category(HASH_TABLE_TYPE(category_set) hash,
		const struct event_category *const cat)
{
	/* insert this category (key == the unique internal pointer) */
	hash_table_update(hash, cat->internal, cat);

	/* insert parent's categories */
	if (cat->parent != NULL)
		insert_category(hash, cat->parent);
}

struct event_category_iterator *
event_categories_iterate_init(const struct event *event)
{
	struct event_category_iterator *iter;
	struct event_category *const *cats;
	unsigned int count, i;

	cats = event_get_categories(event, &count);
	if (count == 0)
		return NULL;

	iter = i_new(struct event_category_iterator, 1);

	hash_table_create_direct(&iter->hash, default_pool,
				 3 * count /* estimate */);

	/* Insert all the categories into the hash table */
	for (i = 0; i < count; i++)
		insert_category(iter->hash, cats[i]);

	iter->iter = hash_table_iterate_init(iter->hash);

	return iter;
}

bool event_categories_iterate(struct event_category_iterator *iter,
			      const struct event_category **cat_r)
{
	void *key ATTR_UNUSED;

	if (iter == NULL) {
		*cat_r = NULL;
		return FALSE;
	}
	return hash_table_iterate(iter->iter, iter->hash, &key, cat_r);
}

void event_categories_iterate_deinit(struct event_category_iterator **_iter)
{
	struct event_category_iterator *iter = *_iter;

	if (iter == NULL)
		return;
	*_iter = NULL;

	hash_table_iterate_deinit(&iter->iter);
	hash_table_destroy(&iter->hash);
	i_free(iter);
}

void event_send(struct event *event, struct failure_context *ctx,
		const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	event_vsend(event, ctx, fmt, args);
	va_end(args);
}

void event_vsend(struct event *event, struct failure_context *ctx,
		 const char *fmt, va_list args)
{
	i_gettimeofday(&event->tv_last_sent);

	/* Skip adding user_cpu_usecs if not enabled. */
	if (event->ru_last.ru_utime.tv_sec != 0 ||
	    event->ru_last.ru_utime.tv_usec != 0) {
		struct rusage ru_current;
		get_self_rusage(&ru_current);
		long long udiff = timeval_diff_usecs(&ru_current.ru_utime,
						     &event->ru_last.ru_utime);
		event_add_int(event, "user_cpu_usecs", udiff > 0 ? udiff : 0);
	}
	if (event_call_callbacks(event, EVENT_CALLBACK_TYPE_SEND,
				 ctx, fmt, args)) {
		if (ctx->type != LOG_TYPE_DEBUG ||
		    event->sending_debug_log)
			i_log_typev(ctx, fmt, args);
	}
	event_send_abort(event);
}

void event_send_abort(struct event *event)
{
	/* if the event is sent again, it needs a new name */
	i_free(event->sending_name);
	if (event->passthrough)
		event_unref(&event);
}

static void
event_export_field_value(string_t *dest, const struct event_field *field)
{
	switch (field->value_type) {
	case EVENT_FIELD_VALUE_TYPE_STR:
		str_append_c(dest, EVENT_CODE_FIELD_STR);
		str_append_tabescaped(dest, field->key);
		str_append_c(dest, '\t');
		str_append_tabescaped(dest, field->value.str);
		break;
	case EVENT_FIELD_VALUE_TYPE_INTMAX:
		str_append_c(dest, EVENT_CODE_FIELD_INTMAX);
		str_append_tabescaped(dest, field->key);
		str_printfa(dest, "\t%jd", field->value.intmax);
		break;
	case EVENT_FIELD_VALUE_TYPE_TIMEVAL:
		str_append_c(dest, EVENT_CODE_FIELD_TIMEVAL);
		str_append_tabescaped(dest, field->key);
		str_printfa(dest, "\t%"PRIdTIME_T"\t%u",
			    field->value.timeval.tv_sec,
			    (unsigned int)field->value.timeval.tv_usec);
		break;
	case EVENT_FIELD_VALUE_TYPE_IP:
		str_append_c(dest, EVENT_CODE_FIELD_IP);
		str_append_tabescaped(dest, field->key);
		str_printfa(dest, "\t%s", net_ip2addr(&field->value.ip));
		break;
	case EVENT_FIELD_VALUE_TYPE_STRLIST: {
		unsigned int count;
		const char *const *strlist =
			array_get(&field->value.strlist, &count);
		str_append_c(dest, EVENT_CODE_FIELD_STRLIST);
		str_append_tabescaped(dest, field->key);
		str_printfa(dest, "\t%u", count);
		for (unsigned int i = 0; i < count; i++) {
			str_append_c(dest, '\t');
			str_append_tabescaped(dest, strlist[i]);
		}
	}
	}
}

void event_export(const struct event *event, string_t *dest)
{
	/* required fields: */
	str_printfa(dest, "%"PRIdTIME_T"\t%u",
		    event->tv_created.tv_sec,
		    (unsigned int)event->tv_created.tv_usec);

	/* optional fields: */
	if (event->source_filename != NULL) {
		str_append_c(dest, '\t');
		str_append_c(dest, EVENT_CODE_SOURCE);
		str_append_tabescaped(dest, event->source_filename);
		str_printfa(dest, "\t%u", event->source_linenum);
	}
	if (event->always_log_source) {
		str_append_c(dest, '\t');
		str_append_c(dest, EVENT_CODE_ALWAYS_LOG_SOURCE);
	}
	if (event->tv_last_sent.tv_sec != 0) {
		str_printfa(dest, "\t%c%"PRIdTIME_T"\t%u",
			    EVENT_CODE_TV_LAST_SENT,
			    event->tv_last_sent.tv_sec,
			    (unsigned int)event->tv_last_sent.tv_usec);
	}
	if (event->sending_name != NULL) {
		str_append_c(dest, '\t');
		str_append_c(dest, EVENT_CODE_SENDING_NAME);
		str_append_tabescaped(dest, event->sending_name);
	}

	if (array_is_created(&event->categories)) {
		struct event_category *cat;
		array_foreach_elem(&event->categories, cat) {
			str_append_c(dest, '\t');
			str_append_c(dest, EVENT_CODE_CATEGORY);
			str_append_tabescaped(dest, cat->name);
		}
	}

	if (array_is_created(&event->fields)) {
		const struct event_field *field;
		array_foreach(&event->fields, field) {
			str_append_c(dest, '\t');
			event_export_field_value(dest, field);
		}
	}
}

bool event_import(struct event *event, const char *str, const char **error_r)
{
	return event_import_unescaped(event, t_strsplit_tabescaped(str),
				      error_r);
}

static bool event_import_tv(const char *arg_secs, const char *arg_usecs,
			    struct timeval *tv_r, const char **error_r)
{
	unsigned int usecs;

	if (str_to_time(arg_secs, &tv_r->tv_sec) < 0) {
		*error_r = "Invalid timeval seconds parameter";
		return FALSE;
	}

	if (arg_usecs == NULL) {
		*error_r = "Timeval missing microseconds parameter";
		return FALSE;
	}
	if (str_to_uint(arg_usecs, &usecs) < 0 || usecs >= 1000000) {
		*error_r = "Invalid timeval microseconds parameter";
		return FALSE;
	}
	tv_r->tv_usec = usecs;
	return TRUE;
}

static bool
event_import_strlist(struct event *event, struct event_field *field,
		     const char *const **_args, const char **error_r)
{
	const char *const *args = *_args;
	unsigned int count, i;

	field->value_type = EVENT_FIELD_VALUE_TYPE_STRLIST;
	if (str_to_uint(args[0], &count) < 0) {
		*error_r = t_strdup_printf("Field '%s' has invalid count: '%s'",
					   field->key, args[0]);
		return FALSE;
	}
	p_array_init(&field->value.strlist, event->pool, count);
	for (i = 1; i <= count && args[i] != NULL; i++) {
		const char *str = p_strdup(event->pool, args[i]);
		array_push_back(&field->value.strlist, &str);
	}
	if (i < count) {
		*error_r = t_strdup_printf("Field '%s' has too few values",
					   field->key);
		return FALSE;
	}
	*_args += count;
	return TRUE;
}

static bool
event_import_field(struct event *event, enum event_code code, const char *arg,
		   const char *const **_args, const char **error_r)
{
	const char *const *args = *_args;
	const char *error;

	if (*arg == '\0') {
		*error_r = "Field name is missing";
		return FALSE;
	}
	struct event_field *field = event_get_field(event, arg, TRUE);
	if (args[0] == NULL) {
		*error_r = "Field value is missing";
		return FALSE;
	}
	switch (code) {
	case EVENT_CODE_FIELD_INTMAX:
		field->value_type = EVENT_FIELD_VALUE_TYPE_INTMAX;
		if (str_to_intmax(*args, &field->value.intmax) < 0) {
			*error_r = t_strdup_printf(
				"Invalid field value '%s' number for '%s'",
				*args, field->key);
			return FALSE;
		}
		break;
	case EVENT_CODE_FIELD_STR:
		if (field->value_type == EVENT_FIELD_VALUE_TYPE_STR &&
		    null_strcmp(field->value.str, *args) == 0) {
			/* already identical value */
			break;
		}
		field->value_type = EVENT_FIELD_VALUE_TYPE_STR;
		field->value.str = p_strdup(event->pool, *args);
		break;
	case EVENT_CODE_FIELD_TIMEVAL:
		field->value_type = EVENT_FIELD_VALUE_TYPE_TIMEVAL;
		if (!event_import_tv(args[0], args[1],
				     &field->value.timeval, &error)) {
			*error_r = t_strdup_printf("Field '%s' value '%s': %s",
						   field->key, args[1], error);
			return FALSE;
		}
		args++;
		break;
	case EVENT_CODE_FIELD_IP:
		field->value_type = EVENT_FIELD_VALUE_TYPE_IP;
		if (net_addr2ip(*args, &field->value.ip) < 0) {
			*error_r = t_strdup_printf(
				"Invalid field value '%s' IP for '%s'",
				*args, field->key);
			return FALSE;
		}
		break;
	case EVENT_CODE_FIELD_STRLIST:
		if (!event_import_strlist(event, field, &args, error_r))
			return FALSE;
		break;
	default:
		i_unreached();
	}
	*_args = args;
	return TRUE;
}


static bool
event_import_arg(struct event *event, const char *const **_args,
		 const char **error_r)
{
	const char *const *args = *_args;
	const char *error, *arg = *args;
	enum event_code code = arg[0];

	arg++;
	switch (code) {
	case EVENT_CODE_ALWAYS_LOG_SOURCE:
		event->always_log_source = TRUE;
		break;
	case EVENT_CODE_CATEGORY: {
		struct event_category *category =
			event_category_find_registered(arg);
		if (category == NULL) {
			*error_r = t_strdup_printf(
				"Unregistered category: '%s'", arg);
			return FALSE;
		}
		if (!array_is_created(&event->categories))
			p_array_init(&event->categories, event->pool, 4);
		if (!event_find_category(event, category))
			array_push_back(&event->categories, &category);
		break;
	}
	case EVENT_CODE_TV_LAST_SENT:
		if (!event_import_tv(arg, args[1], &event->tv_last_sent,
				     &error)) {
			*error_r = t_strdup_printf(
				"Invalid tv_last_sent: %s", error);
			return FALSE;
		}
		args++;
		break;
	case EVENT_CODE_SENDING_NAME:
		i_free(event->sending_name);
		event->sending_name = i_strdup(arg);
		break;
	case EVENT_CODE_SOURCE: {
		unsigned int linenum;

		if (args[1] == NULL) {
			*error_r = "Source line number missing";
			return FALSE;
		}
		if (str_to_uint(args[1], &linenum) < 0) {
			*error_r = "Invalid Source line number";
			return FALSE;
		}
		event_set_source(event, arg, linenum, FALSE);
		args++;
		break;
	}
	case EVENT_CODE_FIELD_INTMAX:
	case EVENT_CODE_FIELD_STR:
	case EVENT_CODE_FIELD_STRLIST:
	case EVENT_CODE_FIELD_TIMEVAL:
	case EVENT_CODE_FIELD_IP: {
		args++;
		if (!event_import_field(event, code, arg, &args, error_r))
			return FALSE;
		break;
	}
	}
	*_args = args;
	return TRUE;
}

bool event_import_unescaped(struct event *event, const char *const *args,
			    const char **error_r)
{
	const char *error;

	/* Event's create callback has already added service:<name> category.
	   This imported event may be coming from another service process
	   though, so clear it out. */
	if (array_is_created(&event->categories))
		array_clear(&event->categories);

	/* required fields: */
	if (args[0] == NULL) {
		*error_r = "Missing required fields";
		return FALSE;
	}
	if (!event_import_tv(args[0], args[1], &event->tv_created, &error)) {
		*error_r = t_strdup_printf("Invalid tv_created: %s", error);
		return FALSE;
	}
	args += 2;

	/* optional fields: */
	while (*args != NULL) {
		if (!event_import_arg(event, &args, error_r))
			return FALSE;
		args++;
	}
	return TRUE;
}

void event_register_callback(event_callback_t *callback)
{
	array_push_back(&event_handlers, &callback);
}

void event_unregister_callback(event_callback_t *callback)
{
	unsigned int idx;

	if (!array_lsearch_ptr_idx(&event_handlers, callback, &idx))
		i_unreached();
	array_delete(&event_handlers, idx, 1);
}

void event_category_register_callback(event_category_callback_t *callback)
{
	array_push_back(&event_category_callbacks, &callback);
}

void event_category_unregister_callback(event_category_callback_t *callback)
{
	unsigned int idx;

	if (!array_lsearch_ptr_idx(&event_category_callbacks, callback, &idx))
		i_unreached();
	array_delete(&event_category_callbacks, idx, 1);
}

static struct event_passthrough *
event_passthrough_set_append_log_prefix(const char *prefix)
{
	event_set_append_log_prefix(last_passthrough_event(), prefix);
	return &event_passthrough_vfuncs;
}

static struct event_passthrough *
event_passthrough_replace_log_prefix(const char *prefix)
{
	event_replace_log_prefix(last_passthrough_event(), prefix);
	return &event_passthrough_vfuncs;
}

static struct event_passthrough *
event_passthrough_set_name(const char *name)
{
	event_set_name(last_passthrough_event(), name);
	return &event_passthrough_vfuncs;
}

static struct event_passthrough *
event_passthrough_set_source(const char *filename,
			     unsigned int linenum, bool literal_fname)
{
	event_set_source(last_passthrough_event(), filename,
			 linenum, literal_fname);
	return &event_passthrough_vfuncs;
}

static struct event_passthrough *
event_passthrough_set_always_log_source(void)
{
	event_set_always_log_source(last_passthrough_event());
	return &event_passthrough_vfuncs;
}

static struct event_passthrough *
event_passthrough_add_categories(struct event_category *const *categories)
{
	event_add_categories(last_passthrough_event(), categories);
	return &event_passthrough_vfuncs;
}

static struct event_passthrough *
event_passthrough_add_category(struct event_category *category)
{
	event_add_category(last_passthrough_event(), category);
	return &event_passthrough_vfuncs;
}

static struct event_passthrough *
event_passthrough_add_fields(const struct event_add_field *fields)
{
	event_add_fields(last_passthrough_event(), fields);
	return &event_passthrough_vfuncs;
}

static struct event_passthrough *
event_passthrough_add_str(const char *key, const char *value)
{
	event_add_str(last_passthrough_event(), key, value);
	return &event_passthrough_vfuncs;
}

static struct event_passthrough *
event_passthrough_strlist_append(const char *key, const char *value)
{
	event_strlist_append(last_passthrough_event(), key, value);
	return &event_passthrough_vfuncs;
}

static struct event_passthrough *
event_passthrough_strlist_replace(const char *key, const char *const *values,
				  unsigned int count)
{
	event_strlist_replace(last_passthrough_event(), key, values, count);
	return &event_passthrough_vfuncs;
}

static struct event_passthrough *
event_passthrough_add_int(const char *key, intmax_t num)
{
	event_add_int(last_passthrough_event(), key, num);
	return &event_passthrough_vfuncs;
}

static struct event_passthrough *
event_passthrough_add_int_nonzero(const char *key, intmax_t num)
{
	event_add_int_nonzero(last_passthrough_event(), key, num);
	return &event_passthrough_vfuncs;
}

static struct event_passthrough *
event_passthrough_add_timeval(const char *key, const struct timeval *tv)
{
	event_add_timeval(last_passthrough_event(), key, tv);
	return &event_passthrough_vfuncs;
}

static struct event_passthrough *
event_passthrough_add_ip(const char *key, const struct ip_addr *ip)
{
	event_add_ip(last_passthrough_event(), key, ip);
	return &event_passthrough_vfuncs;
}

static struct event_passthrough *
event_passthrough_inc_int(const char *key, intmax_t num)
{
	event_inc_int(last_passthrough_event(), key, num);
	return &event_passthrough_vfuncs;
}

static struct event_passthrough *
event_passthrough_clear_field(const char *key)
{
	event_field_clear(last_passthrough_event(), key);
	return &event_passthrough_vfuncs;
}

static struct event *event_passthrough_event(void)
{
	struct event *event = last_passthrough_event();
	event_last_passthrough = NULL;
	return event;
}

struct event_passthrough event_passthrough_vfuncs = {
	.append_log_prefix = event_passthrough_set_append_log_prefix,
	.replace_log_prefix = event_passthrough_replace_log_prefix,
	.set_name = event_passthrough_set_name,
	.set_source = event_passthrough_set_source,
	.set_always_log_source = event_passthrough_set_always_log_source,
	.add_categories = event_passthrough_add_categories,
	.add_category = event_passthrough_add_category,
	.add_fields = event_passthrough_add_fields,
	.add_str = event_passthrough_add_str,
	.add_int = event_passthrough_add_int,
	.add_int_nonzero = event_passthrough_add_int_nonzero,
	.add_timeval = event_passthrough_add_timeval,
	.add_ip = event_passthrough_add_ip,
	.inc_int = event_passthrough_inc_int,
	.strlist_append = event_passthrough_strlist_append,
	.strlist_replace = event_passthrough_strlist_replace,
	.clear_field = event_passthrough_clear_field,
	.event = event_passthrough_event,
};

void event_enable_user_cpu_usecs(struct event *event)
{
	get_self_rusage(&event->ru_last);
}

void lib_event_init(void)
{
	i_array_init(&event_handlers, 4);
	i_array_init(&event_category_callbacks, 4);
	i_array_init(&event_registered_categories_internal, 16);
	i_array_init(&event_registered_categories_representative, 16);
}

void lib_event_deinit(void)
{
	struct event_internal_category *internal;

	event_unset_global_debug_log_filter();
	event_unset_global_debug_send_filter();
	event_unset_global_core_log_filter();
	for (struct event *event = events; event != NULL; event = event->next) {
		i_warning("Event %p leaked (parent=%p): %s:%u",
			  event, event->parent,
			  event->source_filename, event->source_linenum);
	}
	/* categories cannot be unregistered, so just free them here */
	array_foreach_elem(&event_registered_categories_internal, internal) {
		i_free(internal->name);
		i_free(internal);
	}
	array_free(&event_handlers);
	array_free(&event_category_callbacks);
	array_free(&event_registered_categories_internal);
	array_free(&event_registered_categories_representative);
	array_free(&global_event_stack);
}
