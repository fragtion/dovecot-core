/* Copyright (c) Dovecot authors, see top-level COPYING file */

#include "mesh-common.h"
#include "ioloop.h"
#include "str.h"
#include "strescape.h"
#include "sha1.h"
#include "hex-binary.h"
#include "json-generator.h"
#include "doveadm-protocol.h"
#include "doveadm-client.h"

/* Verifying a user compares fingerprints of its mailboxes on every node
   that's up: for each mailbox (by GUID) the number of mails, and a hash of
   each mail's UID, GUID and flags in UID order. Once the nodes have
   synced, these must be identical. A fingerprint is computed by the node's
   own doveadm server, so nodes send each other only the fingerprints. */

#define MESH_FINGERPRINT_TIMEOUT_MSECS (5*60*1000)
/* recheck a mismatch this long after the full sync it started */
#define MESH_VERIFY_RECHECK_SECS 60
/* don't verify users that changed more recently than this */
#define MESH_VERIFY_IDLE_SECS 30

struct mesh_fingerprint_mailbox {
	const char *guid, *name;
	unsigned int messages;
	struct sha1_ctxt sha1;
	const char *hash;
};

struct mesh_fingerprint {
	pool_t pool;
	ARRAY(struct mesh_fingerprint_mailbox *) mailboxes;
	HASH_TABLE(const char *, struct mesh_fingerprint_mailbox *) by_guid;
};

enum mesh_fingerprint_step {
	MESH_FINGERPRINT_STEP_MAILBOXES,
	MESH_FINGERPRINT_STEP_MAILS,
};

struct mesh_fingerprint_request {
	char *username;
	struct mesh_fingerprint *fp;
	enum mesh_fingerprint_step step;

	struct doveadm_client *conn;
	struct timeout *to_start;
	/* the value being received */
	string_t *partial;
	ARRAY_TYPE(const_string) fields;
	pool_t fields_pool;

	mesh_fingerprint_callback_t *callback;
	void *context;
};

struct mesh_verify_node {
	const char *name;
	struct mesh_fingerprint *fp;
	const char *error;
	struct mesh_fingerprint_request *local_req;
	struct mesh_peer_http_get *peer_req;
	struct mesh_verify *verify;
};

struct mesh_verify {
	pool_t pool;
	char *username;
	ARRAY(struct mesh_verify_node *) nodes;
	unsigned int pending;

	mesh_verify_callback_t *callback;
	void *context;
};

static struct timeout *mesh_verify_to;
static char *mesh_verify_last_user;
static struct mesh_verify *mesh_verify_periodic;

/*
 * Fingerprints
 */

static struct mesh_fingerprint *mesh_fingerprint_new(void)
{
	pool_t pool = pool_alloconly_create("mesh fingerprint", 1024);
	struct mesh_fingerprint *fp = p_new(pool, struct mesh_fingerprint, 1);

	fp->pool = pool;
	p_array_init(&fp->mailboxes, pool, 16);
	hash_table_create(&fp->by_guid, pool, 0, str_hash, strcmp);
	return fp;
}

void mesh_fingerprint_free(struct mesh_fingerprint **_fp)
{
	struct mesh_fingerprint *fp = *_fp;

	if (fp == NULL)
		return;
	*_fp = NULL;
	hash_table_destroy(&fp->by_guid);
	pool_unref(&fp->pool);
}

static struct mesh_fingerprint_mailbox *
mesh_fingerprint_get_mailbox(struct mesh_fingerprint *fp, const char *guid,
			     const char *name)
{
	struct mesh_fingerprint_mailbox *box;

	box = hash_table_lookup(fp->by_guid, guid);
	if (box != NULL)
		return box;
	box = p_new(fp->pool, struct mesh_fingerprint_mailbox, 1);
	box->guid = p_strdup(fp->pool, guid);
	box->name = p_strdup(fp->pool, name);
	sha1_init(&box->sha1);
	hash_table_insert(fp->by_guid, box->guid, box);
	array_push_back(&fp->mailboxes, &box);
	return box;
}

static int mesh_fingerprint_mailbox_cmp(struct mesh_fingerprint_mailbox *const *b1,
					struct mesh_fingerprint_mailbox *const *b2)
{
	return strcmp((*b1)->name, (*b2)->name);
}

static int mesh_flag_cmp(const char *const *f1, const char *const *f2)
{
	return strcmp(*f1, *f2);
}

static void
mesh_fingerprint_add_mail(struct mesh_fingerprint *fp,
			  const char *const *fields)
{
	/* mailbox-guid mailbox uid guid flags */
	struct mesh_fingerprint_mailbox *box =
		mesh_fingerprint_get_mailbox(fp, fields[0], fields[1]);
	ARRAY_TYPE(const_string) flags;
	const char *const *flagp;
	string_t *str = t_str_new(128);

	/* \Recent differs between nodes and isn't replicated */
	t_array_init(&flags, 8);
	for (flagp = t_strsplit_spaces(fields[4], " "); *flagp != NULL; flagp++) {
		if (strcasecmp(*flagp, "\\Recent") != 0)
			array_push_back(&flags, flagp);
	}
	array_sort(&flags, mesh_flag_cmp);
	array_append_zero(&flags);
	str_printfa(str, "%s %s %s\n", fields[2], fields[3],
		    t_strarray_join(array_front(&flags), " "));
	sha1_loop(&box->sha1, str_data(str), str_len(str));
	box->messages++;
}

static void mesh_fingerprint_finish(struct mesh_fingerprint *fp)
{
	struct mesh_fingerprint_mailbox *box;
	unsigned char digest[SHA1_RESULTLEN];

	array_foreach_elem(&fp->mailboxes, box) {
		sha1_result(&box->sha1, digest);
		box->hash = p_strdup(fp->pool,
			binary_to_hex(digest, sizeof(digest)));
	}
	array_sort(&fp->mailboxes, mesh_fingerprint_mailbox_cmp);
}

/* One line per mailbox: <guid> <name> <messages> <hash> */
void mesh_fingerprint_export(const struct mesh_fingerprint *fp,
			     string_t *dest)
{
	struct mesh_fingerprint_mailbox *box;

	array_foreach_elem(&fp->mailboxes, box) {
		str_append_tabescaped(dest, box->guid);
		str_append_c(dest, '\t');
		str_append_tabescaped(dest, box->name);
		str_printfa(dest, "\t%u\t%s\n", box->messages, box->hash);
	}
}

static struct mesh_fingerprint *
mesh_fingerprint_import(const buffer_t *data, const char **error_r)
{
	struct mesh_fingerprint *fp = mesh_fingerprint_new();
	struct mesh_fingerprint_mailbox *box;
	const char *const *lines, *const *args;

	lines = t_strsplit(t_strndup(data->data, data->used), "\n");
	for (; *lines != NULL; lines++) {
		if (**lines == '\0')
			continue;
		args = t_strsplit_tabescaped(*lines);
		if (str_array_length(args) < 4) {
			*error_r = t_strdup_printf("Invalid fingerprint line: %s",
						   *lines);
			mesh_fingerprint_free(&fp);
			return NULL;
		}
		box = mesh_fingerprint_get_mailbox(fp, args[0], args[1]);
		if (str_to_uint(args[2], &box->messages) < 0) {
			*error_r = t_strdup_printf("Invalid fingerprint line: %s",
						   *lines);
			mesh_fingerprint_free(&fp);
			return NULL;
		}
		box->hash = p_strdup(fp->pool, args[3]);
	}
	return fp;
}

static void mesh_fingerprint_request_free(struct mesh_fingerprint_request *req)
{
	timeout_remove(&req->to_start);
	if (req->conn != NULL)
		doveadm_client_unref(&req->conn);
	mesh_fingerprint_free(&req->fp);
	str_free(&req->partial);
	pool_unref(&req->fields_pool);
	i_free(req->username);
	i_free(req);
}

void mesh_fingerprint_request_abort(struct mesh_fingerprint_request **_req)
{
	struct mesh_fingerprint_request *req = *_req;

	*_req = NULL;
	mesh_fingerprint_request_free(req);
}

static void
mesh_fingerprint_request_finish(struct mesh_fingerprint_request *req,
				const char *error)
{
	struct mesh_fingerprint *fp = NULL;

	if (error == NULL) {
		fp = req->fp;
		req->fp = NULL;
		mesh_fingerprint_finish(fp);
	}
	req->callback(fp, error, req->context);
	mesh_fingerprint_request_free(req);
}

static void
mesh_fingerprint_handle_record(struct mesh_fingerprint_request *req,
			       const char *const *fields)
{
	if (req->step == MESH_FINGERPRINT_STEP_MAILBOXES) {
		/* mailbox messages guid. Only to include empty mailboxes,
		   the mails are counted from the fetch. */
		(void)mesh_fingerprint_get_mailbox(req->fp, fields[2],
						   fields[0]);
	} else {
		mesh_fingerprint_add_mail(req->fp, fields);
	}
}

static void
mesh_fingerprint_print(const unsigned char *data, size_t size,
		       bool finished, struct mesh_fingerprint_request *req)
{
	unsigned int field_count =
		req->step == MESH_FINGERPRINT_STEP_MAILBOXES ? 3 : 5;
	const char *value;

	/* Called with each (tab-escaped) value, possibly in pieces;
	   finished=TRUE ends the value. */
	str_append_data(req->partial, data, size);
	if (!finished)
		return;
	value = p_strdup(req->fields_pool,
			 str_tabunescape(t_strdup_noconst(str_c(req->partial))));
	str_truncate(req->partial, 0);
	array_push_back(&req->fields, &value);
	if (array_count(&req->fields) < field_count)
		return;
	mesh_fingerprint_handle_record(req, array_front(&req->fields));
	array_clear(&req->fields);
	p_clear(req->fields_pool);
	p_array_init(&req->fields, req->fields_pool, 8);
}

static void mesh_fingerprint_run(struct mesh_fingerprint_request *req);

static void
mesh_fingerprint_reply(const struct doveadm_server_reply *reply,
		       struct mesh_fingerprint_request *req)
{
	if (reply->exit_code != 0) {
		mesh_fingerprint_request_finish(req, t_strdup_printf(
			"doveadm %s failed: %s%s%s",
			req->step == MESH_FINGERPRINT_STEP_MAILBOXES ?
			"mailbox status" : "fetch",
			doveadm_exit_code_to_str(reply->exit_code),
			reply->error == NULL ? "" : ": ",
			reply->error == NULL ? "" : reply->error));
		return;
	}
	if (req->step == MESH_FINGERPRINT_STEP_MAILBOXES) {
		req->step = MESH_FINGERPRINT_STEP_MAILS;
		doveadm_client_unref(&req->conn);
		mesh_fingerprint_run(req);
		return;
	}
	mesh_fingerprint_request_finish(req, NULL);
}

static void mesh_fingerprint_run(struct mesh_fingerprint_request *req)
{
	struct doveadm_client_cmd_settings cmd_set = {
		.proxy_ttl = DOVEADM_PROXY_TTL,
	};
	string_t *cmd = t_str_new(128);
	const char *error;

	timeout_remove(&req->to_start);
	if (mesh_doveadm_client_create(&req->conn, &error) < 0) {
		mesh_fingerprint_request_finish(req, t_strdup_printf(
			"Couldn't connect to doveadm server: %s", error));
		return;
	}
	str_truncate(req->partial, 0);
	array_clear(&req->fields);

	/* <flags> <username> <command> [<args>] */
	str_append_c(cmd, '\t');
	str_append_tabescaped(cmd, req->username);
	if (req->step == MESH_FINGERPRINT_STEP_MAILBOXES)
		str_append(cmd, "\tmailbox\tstatus\tmessages guid\t*\n");
	else
		str_append(cmd, "\tfetch\tmailbox-guid mailbox uid guid flags\tall\n");
	doveadm_client_set_print(req->conn, mesh_fingerprint_print, req);
	doveadm_client_cmd(req->conn, &cmd_set, str_c(cmd), NULL,
			   (doveadm_client_cmd_callback_t *)
			   mesh_fingerprint_reply, req);
}

struct mesh_fingerprint_request *
mesh_fingerprint_local(const char *username,
		       mesh_fingerprint_callback_t *callback, void *context)
{
	struct mesh_fingerprint_request *req;

	req = i_new(struct mesh_fingerprint_request, 1);
	req->username = i_strdup(username);
	req->fp = mesh_fingerprint_new();
	req->partial = str_new(default_pool, 256);
	req->fields_pool = pool_alloconly_create("mesh fingerprint fields", 512);
	p_array_init(&req->fields, req->fields_pool, 8);
	req->callback = callback;
	req->context = context;
	/* the callback must not run before the caller has the request */
	req->to_start = timeout_add_short(0, mesh_fingerprint_run, req);
	return req;
}

/*
 * Verifying a user on all nodes
 */

static void mesh_verify_free(struct mesh_verify *verify)
{
	struct mesh_verify_node *node;

	array_foreach_elem(&verify->nodes, node) {
		if (node->local_req != NULL)
			mesh_fingerprint_request_abort(&node->local_req);
		if (node->peer_req != NULL)
			mesh_peer_http_get_abort(&node->peer_req);
		mesh_fingerprint_free(&node->fp);
	}
	i_free(verify->username);
	pool_unref(&verify->pool);
}

static const struct mesh_fingerprint_mailbox *
mesh_verify_node_mailbox(const struct mesh_verify_node *node,
			 const char *guid)
{
	if (node->fp == NULL)
		return NULL;
	return hash_table_lookup(node->fp->by_guid, guid);
}

static int mesh_verify_mailbox_cmp(struct mesh_verify_mailbox *const *b1,
				   struct mesh_verify_mailbox *const *b2)
{
	/* differences first */
	if ((*b1)->match != (*b2)->match)
		return (*b1)->match ? 1 : -1;
	return strcmp((*b1)->name, (*b2)->name);
}

static struct mesh_verify_result *mesh_verify_compare(struct mesh_verify *verify)
{
	struct mesh_verify_result *result;
	struct mesh_verify_mailbox *rbox;
	const struct mesh_fingerprint_mailbox *box, *first;
	struct mesh_fingerprint_mailbox *fbox;
	struct mesh_verify_node *node;
	HASH_TABLE(const char *, struct mesh_verify_mailbox *) seen;
	unsigned int i, node_count = array_count(&verify->nodes);
	bool errors = FALSE, differs = FALSE;

	result = p_new(verify->pool, struct mesh_verify_result, 1);
	result->username = p_strdup(verify->pool, verify->username);
	p_array_init(&result->nodes, verify->pool, node_count);
	p_array_init(&result->mailboxes, verify->pool, 16);
	array_foreach_elem(&verify->nodes, node) {
		struct mesh_verify_result_node *rnode =
			array_append_space(&result->nodes);
		rnode->name = node->name;
		rnode->error = node->error;
		if (node->error != NULL)
			errors = TRUE;
	}

	/* collect all mailboxes */
	hash_table_create(&seen, verify->pool, 0, str_hash, strcmp);
	array_foreach_elem(&verify->nodes, node) {
		if (node->fp == NULL)
			continue;
		array_foreach_elem(&node->fp->mailboxes, fbox) {
			if (hash_table_lookup(seen, fbox->guid) != NULL)
				continue;
			rbox = p_new(verify->pool, struct mesh_verify_mailbox, 1);
			rbox->guid = fbox->guid;
			rbox->name = fbox->name;
			rbox->nodes = p_new(verify->pool,
				struct mesh_verify_mailbox_node, node_count);
			hash_table_insert(seen, rbox->guid, rbox);
			array_push_back(&result->mailboxes, &rbox);
		}
	}
	hash_table_destroy(&seen);

	array_foreach_elem(&result->mailboxes, rbox) {
		first = NULL;
		rbox->match = TRUE;
		for (i = 0; i < node_count; i++) {
			node = array_idx_elem(&verify->nodes, i);
			if (node->fp == NULL)
				continue;
			box = mesh_verify_node_mailbox(node, rbox->guid);
			if (box == NULL) {
				rbox->match = FALSE;
				continue;
			}
			rbox->nodes[i].present = TRUE;
			rbox->nodes[i].messages = box->messages;
			rbox->nodes[i].hash = box->hash;
			if (first == NULL)
				first = box;
			else if (first->messages != box->messages ||
				 strcmp(first->hash, box->hash) != 0)
				rbox->match = FALSE;
		}
		if (!rbox->match)
			differs = TRUE;
	}
	array_sort(&result->mailboxes, mesh_verify_mailbox_cmp);
	result->result = differs ? "mismatch" : errors ? "error" : "ok";
	return result;
}

static void mesh_verify_log(const struct mesh_verify_result *result,
			    bool periodic)
{
	const struct mesh_verify_result_node *node;
	struct mesh_verify_mailbox *box;
	string_t *str = t_str_new(256);
	unsigned int count = 0;

	array_foreach(&result->nodes, node) {
		if (node->error == NULL)
			continue;
		if (str_len(str) > 0)
			str_append(str, ", ");
		str_printfa(str, "%s: %s", node->name, node->error);
	}
	if (strcmp(result->result, "error") == 0) {
		e_warning(mesh_event, "Couldn't verify %s on all nodes: %s",
			  result->username, str_c(str));
		return;
	}
	if (strcmp(result->result, "ok") == 0) {
		e_debug(mesh_event, "Verified %s: all %u nodes match",
			result->username, array_count(&result->nodes));
		return;
	}
	str_truncate(str, 0);
	array_foreach_elem(&result->mailboxes, box) {
		if (box->match)
			continue;
		if (count++ < 5) {
			if (str_len(str) > 0)
				str_append(str, ", ");
			str_append(str, box->name);
		}
	}
	if (count > 5)
		str_printfa(str, " and %u more", count - 5);
	e_warning(mesh_event, "Verifying %s: nodes differ in %s%s",
		  result->username, str_c(str),
		  periodic ? " - starting a full sync" : "");
}

/* Remember the result for the users list, and with the periodic check
   start a full sync to repair differences. */
static void mesh_verify_record(const struct mesh_verify_result *result,
			       bool periodic)
{
	struct mesh_user *user = mesh_user_lookup(result->username, TRUE);

	user->verify_time = ioloop_time;
	/* result is a static string */
	user->verify_result = result->result;
	mesh_verify_log(result, periodic);
	if (strcmp(result->result, "mismatch") != 0) {
		user->verify_mismatches = 0;
		return;
	}
	user->verify_mismatches++;
	if (periodic) {
		if (user->verify_mismatches > 1) {
			e_error(mesh_event, "%s still differs between nodes "
				"after a full sync (%u checks in a row)",
				user->username, user->verify_mismatches);
		}
		mesh_queue_push_all(user, MESH_PRIORITY_NORMAL, TRUE);
	}
}

static void mesh_verify_node_done(struct mesh_verify_node *node)
{
	struct mesh_verify *verify = node->verify;
	struct mesh_verify_result *result;

	i_assert(verify->pending > 0);
	if (--verify->pending > 0)
		return;

	result = mesh_verify_compare(verify);
	mesh_verify_record(result, verify == mesh_verify_periodic);
	if (verify == mesh_verify_periodic)
		mesh_verify_periodic = NULL;
	if (verify->callback != NULL)
		verify->callback(result, verify->context);
	mesh_verify_free(verify);
}

static void
mesh_verify_local_callback(struct mesh_fingerprint *fp, const char *error,
			   struct mesh_verify_node *node)
{
	node->local_req = NULL;
	node->fp = fp;
	if (error != NULL)
		node->error = p_strdup(node->verify->pool, error);
	mesh_verify_node_done(node);
}

static void
mesh_verify_peer_callback(const buffer_t *body, const char *error,
			  struct mesh_verify_node *node)
{
	node->peer_req = NULL;
	if (body != NULL)
		node->fp = mesh_fingerprint_import(body, &error);
	if (node->fp == NULL) {
		node->error = p_strdup_printf(node->verify->pool,
			"Fingerprint request failed: %s", error);
	}
	mesh_verify_node_done(node);
}

struct mesh_verify *
mesh_verify_start(const char *username, mesh_verify_callback_t *callback,
		  void *context)
{
	struct mesh_verify *verify;
	struct mesh_verify_node *node;
	struct mesh_peer *peer;
	string_t *path;
	pool_t pool;

	pool = pool_alloconly_create("mesh verify", 4096);
	verify = p_new(pool, struct mesh_verify, 1);
	verify->pool = pool;
	verify->username = i_strdup(username);
	verify->callback = callback;
	verify->context = context;
	p_array_init(&verify->nodes, pool, 8);

	/* count first: a callback may run immediately on failure */
	verify->pending = 1;
	array_foreach_elem(&mesh_peers, peer) {
		if (peer->up && mesh_peer_has_user(peer, username))
			verify->pending++;
	}

	node = p_new(pool, struct mesh_verify_node, 1);
	node->name = mesh_node_name;
	node->verify = verify;
	array_push_back(&verify->nodes, &node);

	path = t_str_new(128);
	str_append(path, "/mesh/v1/fingerprint?user=");
	mesh_url_encode(path, username);
	array_foreach_elem(&mesh_peers, peer) {
		if (!peer->up || !mesh_peer_has_user(peer, username))
			continue;
		struct mesh_verify_node *pnode =
			p_new(pool, struct mesh_verify_node, 1);
		pnode->name = peer->name;
		pnode->verify = verify;
		array_push_back(&verify->nodes, &pnode);
	}

	/* start the requests only after all nodes are in the array */
	array_foreach_elem(&verify->nodes, node) {
		if (node->name == mesh_node_name) {
			node->local_req = mesh_fingerprint_local(username,
				(mesh_fingerprint_callback_t *)
				mesh_verify_local_callback, node);
		} else {
			peer = mesh_peer_find(node->name);
			node->peer_req = mesh_peer_http_get(peer, str_c(path),
				MESH_FINGERPRINT_TIMEOUT_MSECS,
				(mesh_peer_http_get_callback_t *)
				mesh_verify_peer_callback, node);
		}
	}
	return verify;
}

void mesh_verify_detach(struct mesh_verify **_verify)
{
	struct mesh_verify *verify = *_verify;

	*_verify = NULL;
	verify->callback = NULL;
}

/* Rows: R <result> <user>, N <node> <error>, then for each mailbox
   M <name> <guid> <match|differs> and for each node <messages>:<hash>,
   or "-" if the mailbox is missing there, or "?" if the node failed. */
void mesh_verify_result_rows(const struct mesh_verify_result *result,
			     string_t *dest)
{
	const struct mesh_verify_result_node *node, *nodes;
	struct mesh_verify_mailbox *box;
	unsigned int i, count;

	str_printfa(dest, "R\t%s\t", result->result);
	str_append_tabescaped(dest, result->username);
	str_append_c(dest, '\n');
	array_foreach(&result->nodes, node) {
		str_append(dest, "N\t");
		str_append_tabescaped(dest, node->name);
		str_append_c(dest, '\t');
		if (node->error != NULL)
			str_append_tabescaped(dest, node->error);
		str_append_c(dest, '\n');
	}
	nodes = array_get(&result->nodes, &count);
	array_foreach_elem(&result->mailboxes, box) {
		str_append(dest, "M\t");
		str_append_tabescaped(dest, box->name);
		str_append_c(dest, '\t');
		str_append_tabescaped(dest, box->guid);
		str_append(dest, box->match ? "\tmatch" : "\tdiffers");
		for (i = 0; i < count; i++) {
			if (nodes[i].error != NULL)
				str_append(dest, "\t?");
			else if (!box->nodes[i].present)
				str_append(dest, "\t-");
			else {
				str_printfa(dest, "\t%u:%.12s",
					    box->nodes[i].messages,
					    box->nodes[i].hash);
			}
		}
		str_append_c(dest, '\n');
	}
}

static void json_append_key(string_t *dest, const char *key)
{
	str_append_c(dest, '"');
	str_append(dest, key);
	str_append(dest, "\":");
}

static void json_append_string(string_t *dest, const char *value)
{
	str_append_c(dest, '"');
	json_append_escaped(dest, value);
	str_append_c(dest, '"');
}

void mesh_verify_result_json(const struct mesh_verify_result *result,
			     string_t *dest)
{
	const struct mesh_verify_result_node *node;
	struct mesh_verify_mailbox *box;
	unsigned int i, count = array_count(&result->nodes);

	str_append_c(dest, '{');
	json_append_key(dest, "user");
	json_append_string(dest, result->username);
	str_append_c(dest, ',');
	json_append_key(dest, "result");
	json_append_string(dest, result->result);
	str_append_c(dest, ',');
	json_append_key(dest, "nodes");
	str_append_c(dest, '[');
	array_foreach(&result->nodes, node) {
		if (node != array_front(&result->nodes))
			str_append_c(dest, ',');
		str_append_c(dest, '{');
		json_append_key(dest, "name");
		json_append_string(dest, node->name);
		str_append_c(dest, ',');
		json_append_key(dest, "error");
		if (node->error == NULL)
			str_append(dest, "null");
		else
			json_append_string(dest, node->error);
		str_append_c(dest, '}');
	}
	str_append(dest, "],");
	json_append_key(dest, "mailboxes");
	str_append_c(dest, '[');
	array_foreach_elem(&result->mailboxes, box) {
		if (box != array_idx_elem(&result->mailboxes, 0))
			str_append_c(dest, ',');
		str_append_c(dest, '{');
		json_append_key(dest, "name");
		json_append_string(dest, box->name);
		str_append_c(dest, ',');
		json_append_key(dest, "guid");
		json_append_string(dest, box->guid);
		str_append_c(dest, ',');
		json_append_key(dest, "match");
		str_append(dest, box->match ? "true" : "false");
		str_append_c(dest, ',');
		json_append_key(dest, "nodes");
		str_append_c(dest, '[');
		for (i = 0; i < count; i++) {
			if (i > 0)
				str_append_c(dest, ',');
			if (!box->nodes[i].present) {
				str_append(dest, "null");
				continue;
			}
			str_printfa(dest, "{\"messages\":%u,\"hash\":",
				    box->nodes[i].messages);
			json_append_string(dest, box->nodes[i].hash);
			str_append_c(dest, '}');
		}
		str_append(dest, "]}");
	}
	str_append(dest, "]}");
}

/*
 * Periodic verification
 */

static bool mesh_verify_candidate(const struct mesh_user *user)
{
	struct mesh_peer *peer;

	/* each user is checked by its anchor */
	if (mesh_anchor_peer(user->username) != NULL)
		return FALSE;
	/* only while nothing is being synced */
	if (user->running_peer != NULL || mesh_user_is_leased(user) ||
	    ioloop_time - user->last_change < MESH_VERIFY_IDLE_SECS)
		return FALSE;
	array_foreach_elem(&mesh_peers, peer) {
		if (user->peers[peer->idx].pending != MESH_PRIORITY_NONE)
			return FALSE;
	}
	return TRUE;
}

/* Users that differed last time come first, once their full sync has had
   time to finish. Otherwise go through the users in name order. */
static struct mesh_user *mesh_verify_next_user(void)
{
	struct hash_iterate_context *iter;
	struct mesh_user *user, *recheck = NULL, *next = NULL, *first = NULL;
	char *username;

	iter = hash_table_iterate_init(mesh_users);
	while (hash_table_iterate(iter, mesh_users, &username, &user)) {
		if (!mesh_verify_candidate(user))
			continue;
		if (user->verify_mismatches > 0 &&
		    ioloop_time - user->verify_time >= MESH_VERIFY_RECHECK_SECS) {
			recheck = user;
			break;
		}
		if (first == NULL || strcmp(username, first->username) < 0)
			first = user;
		if ((mesh_verify_last_user == NULL ||
		     strcmp(username, mesh_verify_last_user) > 0) &&
		    (next == NULL || strcmp(username, next->username) < 0))
			next = user;
	}
	hash_table_iterate_deinit(&iter);
	if (recheck != NULL)
		return recheck;
	return next != NULL ? next : first;
}

static void mesh_verify_timeout(void *context ATTR_UNUSED)
{
	struct mesh_user *user;

	if (mesh_verify_periodic != NULL)
		return;
	user = mesh_verify_next_user();
	if (user == NULL)
		return;
	i_free(mesh_verify_last_user);
	mesh_verify_last_user = i_strdup(user->username);
	mesh_verify_periodic = mesh_verify_start(user->username, NULL, NULL);
}

void mesh_verify_init(void)
{
	if (mesh_set->mesh_verify_interval > 0) {
		mesh_verify_to = timeout_add(mesh_set->mesh_verify_interval *
					     1000, mesh_verify_timeout, NULL);
	}
}

void mesh_verify_deinit(void)
{
	timeout_remove(&mesh_verify_to);
	if (mesh_verify_periodic != NULL) {
		struct mesh_verify *verify = mesh_verify_periodic;

		mesh_verify_periodic = NULL;
		mesh_verify_free(verify);
	}
	i_free(mesh_verify_last_user);
}
