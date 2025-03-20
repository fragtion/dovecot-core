#ifndef EVENT_FILTER_H
#define EVENT_FILTER_H

struct event;

enum event_filter_merge_op {
	EVENT_FILTER_MERGE_OP_OR,
	EVENT_FILTER_MERGE_OP_AND,
};

struct event_filter_field {
	const char *key;
	const char *value;
};

typedef bool event_filter_cmp(const char *value, const char *wanted_field);

struct event_filter *event_filter_create(void);
struct event_filter *event_filter_create_with_pool(pool_t pool);
struct event_filter *event_filter_create_fragment(pool_t pool);
void event_filter_ref(struct event_filter *filter);
void event_filter_unref(struct event_filter **filter);

/* Add queries from source filter to destination filter. */
void event_filter_merge(struct event_filter *dest,
			const struct event_filter *src,
			enum event_filter_merge_op op);
/* Add queries from source filter to destination filter, but with supplied
   context overriding whatever context source queries had. */
void event_filter_merge_with_context(struct event_filter *dest,
				     const struct event_filter *src,
				     enum event_filter_merge_op op,
				     void *new_context);

/* Remove query with given context from filter.
   Returns TRUE if query was removed, otherwise FALSE. */
bool event_filter_remove_queries_with_context(struct event_filter *filter,
					      void *context);

/* Export the filter into a string.  The context pointers aren't exported. */
void event_filter_export(struct event_filter *filter, string_t *dest);
/* Add queries to the filter from the given string. The string is expected to
   be generated by event_filter_export(). Returns TRUE on success, FALSE on
   invalid string. */
#define event_filter_import(filter, str, error_r) \
	(event_filter_parse((str), (filter), (error_r)) == 0)

/* Parse a string-ified query, filling the passed in filter */
int event_filter_parse(const char *str, struct event_filter *filter,
		       const char **error_r);
/* Same as event_filter_parse(), but use case-sensitive comparisons. */
int event_filter_parse_case_sensitive(const char *str,
				      struct event_filter *filter,
				      const char **error_r);
/* Find key=value from the event filter and return the value, or NULL if not
   found. This works only for string values. NOT key=value is not returned. */
const char *event_filter_find_field_exact(struct event_filter *filter,
					  const char *key, bool *op_not_r);
/* Returns TRUE if the event filter has key=prefix prefix string. */
bool event_filter_has_field_prefix(struct event_filter *filter,
				   const char *key, const char *prefix);

/* Returns TRUE if the event matches the event filter. */
bool event_filter_match(struct event_filter *filter, struct event *event,
			const struct failure_context *ctx);
/* Same as event_filter_match(), but use the given source filename:linenum
   instead of taking it from the event. */
bool event_filter_match_source(struct event_filter *filter, struct event *event,
			       const char *source_filename,
			       unsigned int source_linenum,
			       const struct failure_context *ctx);

/* Iterate through all queries with non-NULL context that match the event. */
struct event_filter_match_iter *
event_filter_match_iter_init(struct event_filter *filter, struct event *event,
			     const struct failure_context *ctx);
/* Return context for the query that matched, or NULL when there are no more
   matches.  Note: This skips over any queries that have NULL context. */
void *event_filter_match_iter_next(struct event_filter_match_iter *iter);
void event_filter_match_iter_deinit(struct event_filter_match_iter **iter);

/* Register a comparator function for the key. event_filter_match() will use
   this function when matching the values for the key. */
void event_filter_register_cmp(struct event_filter *filter, const char *key,
			       event_filter_cmp *cmp);

void event_filter_init(void);
void event_filter_deinit(void);

#endif
