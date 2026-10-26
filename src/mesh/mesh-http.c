/* Copyright (c) Dovecot authors, see top-level COPYING file */

#include "mesh-common.h"
#include "ioloop.h"
#include "str.h"
#include "base64.h"
#include "istream.h"
#include "http-url.h"
#include "http-auth.h"
#include "http-server.h"
#include "master-service.h"
#include "mesh-ui.h"

/* HTTP API, all under /mesh, authenticated with mesh_api_key either as
   "Authorization: Bearer <key>" or HTTP Basic with any user name and the
   key as the password (for browsers):

     GET  /mesh/v1/heartbeat           node status in X-Mesh-* headers
     GET  /mesh/v1/status              node and peers, JSON
     GET  /mesh/v1/users[?user=<u>]    per user and peer sync status, JSON
     POST /mesh/v1/sync?user=<u>[&full=1]
     POST /mesh/v1/pause?peer=<p>, /mesh/v1/resume?peer=<p>
     POST /mesh/v1/lease?user=<u>&node=<n>&secs=<s>&gen=<g>&exclusive=<0|1>
     POST /mesh/v1/release?user=<u>&node=<n>&gen=<g>   (between nodes,
                                       the payload is the pair's new state)
     GET  /mesh/ui/...                 the status web UI */

#define MESH_HTTP_REALM "Dovecot mesh"
/* dsync states are ~70 bytes per mailbox */
#define MESH_HTTP_MAX_STATE_SIZE (16*1024*1024)

struct mesh_http_client {
	struct http_server_connection *http_conn;
};

static struct http_server *mesh_http_server;
static struct http_server_resource *mesh_http_resource, *mesh_root_resource;
static bool mesh_http_no_key_logged = FALSE;

static void mesh_http_conn_destroy(void *context, const char *reason);

static bool mesh_http_authorized(struct http_server_request *req)
{
	struct http_auth_credentials creds;
	const char *key = mesh_set->mesh_api_key, *data, *p;
	const buffer_t *decoded;

	if (key[0] == '\0') {
		if (!mesh_http_no_key_logged) {
			i_error("HTTP request refused: mesh_api_key is not set");
			mesh_http_no_key_logged = TRUE;
		}
		http_server_request_fail(req, 403, "Forbidden");
		return FALSE;
	}
	if (http_server_request_get_auth(req, &creds) > 0 &&
	    creds.data != NULL) {
		if (strcasecmp(creds.scheme, "Bearer") == 0 &&
		    str_equals_timing_almost_safe(creds.data, key))
			return TRUE;
		if (strcasecmp(creds.scheme, "Basic") == 0) {
			decoded = t_base64_decode_str(creds.data);
			data = t_strndup(decoded->data, decoded->used);
			p = strchr(data, ':');
			if (p != NULL &&
			    str_equals_timing_almost_safe(p + 1, key))
				return TRUE;
		}
	}
	http_server_request_fail_auth_basic(req, "Unauthorized",
					    MESH_HTTP_REALM);
	return FALSE;
}

static int mesh_url_decode(const char *src, string_t *dest)
{
	for (; *src != '\0'; src++) {
		if (*src == '+')
			str_append_c(dest, ' ');
		else if (*src != '%')
			str_append_c(dest, *src);
		else {
			unsigned int value = 0, i;

			for (i = 1; i <= 2; i++) {
				char c = src[i];
				value <<= 4;
				if (c >= '0' && c <= '9')
					value |= c - '0';
				else if (c >= 'a' && c <= 'f')
					value |= c - 'a' + 10;
				else if (c >= 'A' && c <= 'F')
					value |= c - 'A' + 10;
				else
					return -1;
			}
			str_append_c(dest, (char)value);
			src += 2;
		}
	}
	return 0;
}

/* Returns the decoded value of query parameter key, or NULL. */
static const char *
mesh_http_query_param(const struct http_request *hreq, const char *key)
{
	const char *query = hreq->target.url == NULL ? NULL :
		hreq->target.url->enc_query;
	const char *const *params;
	size_t key_len = strlen(key);
	string_t *value;

	if (query == NULL)
		return NULL;
	for (params = t_strsplit(query, "&"); *params != NULL; params++) {
		if (strncmp(*params, key, key_len) != 0 ||
		    (*params)[key_len] != '=')
			continue;
		value = t_str_new(64);
		if (mesh_url_decode(*params + key_len + 1, value) < 0)
			return NULL;
		return str_c(value);
	}
	return NULL;
}

static void
mesh_http_send(struct http_server_request *req, unsigned int status,
	       const char *content_type, const string_t *body)
{
	struct http_server_response *resp;

	resp = http_server_response_create(req, status,
					   status == 200 ? "OK" : "Error");
	http_server_response_add_header(resp, "Content-Type", content_type);
	http_server_response_add_header(resp, "Cache-Control", "no-store");
	http_server_response_set_payload_data(resp, str_data(body),
					      str_len(body));
	http_server_response_submit(resp);
}

static void mesh_http_send_json(struct http_server_request *req,
				const string_t *body)
{
	mesh_http_send(req, 200, "application/json", body);
}

static void mesh_http_send_ok(struct http_server_request *req)
{
	string_t *body = t_str_new(16);

	str_append(body, "{\"ok\":true}");
	mesh_http_send_json(req, body);
}

static void mesh_http_heartbeat(struct http_server_request *req)
{
	struct http_server_response *resp;
	ARRAY_TYPE(const_string) headers;
	const char *const *h;
	unsigned int i, count;

	t_array_init(&headers, 16);
	mesh_heartbeat_headers(&headers);
	resp = http_server_response_create(req, 200, "OK");
	h = array_get(&headers, &count);
	for (i = 0; i + 1 < count; i += 2)
		http_server_response_add_header(resp, h[i], h[i+1]);
	http_server_response_set_payload_data(resp,
		(const unsigned char *)"OK\n", 3);
	http_server_response_submit(resp);
}

/* A peer wants to sync the user with us. Grant it only if no other session
   for the user is running, so that the user's sessions never overlap. */
static void
mesh_http_lease(struct http_server_request *req,
		const struct http_request *hreq)
{
	const char *username = mesh_http_query_param(hreq, "user");
	const char *node = mesh_http_query_param(hreq, "node");
	const char *value;
	struct mesh_peer *peer = node == NULL ? NULL : mesh_peer_find(node);
	struct http_server_response *resp;
	struct mesh_user_peer *up;
	struct mesh_user *user;
	unsigned int secs, gen;
	bool usable;

	if (username == NULL || username[0] == '\0' || peer == NULL) {
		http_server_request_fail(req, 400, "Missing user or node");
		return;
	}
	value = mesh_http_query_param(hreq, "secs");
	if (value == NULL || str_to_uint(value, &secs) < 0)
		secs = mesh_set->mesh_sync_timeout;
	value = mesh_http_query_param(hreq, "gen");
	if (value == NULL || str_to_uint(value, &gen) < 0) {
		http_server_request_fail(req, 400, "Missing gen");
		return;
	}
	user = mesh_user_lookup(username, TRUE);
	if (user->running_peer != NULL ||
	    (mesh_user_is_leased(user) && user->lease_peer != peer)) {
		http_server_request_fail(req, 409, "Busy");
		return;
	}
	user->lease_peer = peer;
	user->lease_expires = ioloop_time + secs;

	/* see mesh_sync_lease_response() */
	up = &user->peers[peer->idx];
	value = mesh_http_query_param(hreq, "exclusive");
	usable = gen == up->state_gen &&
		(up->exclusive || (value != NULL && strcmp(value, "1") == 0));
	mesh_user_mark_session(user, peer);

	resp = http_server_response_create(req, 200, "OK");
	http_server_response_add_header(resp, "X-Mesh-Gen",
					dec2str(up->state_gen));
	http_server_response_add_header(resp, "X-Mesh-State-Usable",
					usable ? "1" : "0");
	http_server_response_set_payload_data(resp,
		(const unsigned char *)"OK\n", 3);
	http_server_response_submit(resp);
}

struct mesh_http_release {
	struct http_server_request *req;
	char *username, *node;
	unsigned int gen;
	buffer_t *state;
};

static void mesh_http_release_destroyed(struct mesh_http_release *ctx)
{
	buffer_free(&ctx->state);
	i_free(ctx->username);
	i_free(ctx->node);
	i_free(ctx);
}

static void mesh_http_release_finished(struct mesh_http_release *ctx)
{
	struct mesh_peer *peer = mesh_peer_find(ctx->node);
	struct mesh_user *user = mesh_user_lookup(ctx->username, TRUE);
	struct mesh_user_peer *up = &user->peers[peer->idx];

	if (user->lease_peer == peer) {
		user->lease_peer = NULL;
		user->lease_expires = 0;
	}
	if (ctx->gen > up->state_gen) {
		up->state_gen = ctx->gen;
		mesh_user_peer_set_state(up, str_c(ctx->state));
		mesh_state_changed();
	}
	mesh_queue_schedule();
	mesh_http_send_ok(ctx->req);
}

/* The peer finished its session with us. */
static void
mesh_http_release(struct http_server_request *req,
		  const struct http_request *hreq)
{
	const char *username = mesh_http_query_param(hreq, "user");
	const char *node = mesh_http_query_param(hreq, "node");
	const char *gen_str = mesh_http_query_param(hreq, "gen");
	struct mesh_http_release *ctx;
	unsigned int gen;
	uoff_t size;

	if (username == NULL || username[0] == '\0' || node == NULL ||
	    mesh_peer_find(node) == NULL) {
		http_server_request_fail(req, 400, "Missing user or node");
		return;
	}
	if (gen_str == NULL || str_to_uint(gen_str, &gen) < 0) {
		http_server_request_fail(req, 400, "Missing gen");
		return;
	}
	ctx = i_new(struct mesh_http_release, 1);
	ctx->req = req;
	ctx->username = i_strdup(username);
	ctx->node = i_strdup(node);
	ctx->gen = gen;
	ctx->state = buffer_create_dynamic(default_pool, 256);
	http_server_request_set_destroy_callback(req,
		mesh_http_release_destroyed, ctx);
	if (hreq->payload == NULL ||
	    (i_stream_get_size(hreq->payload, TRUE, &size) > 0 &&
	     size == 0)) {
		/* no state (buffering an empty payload would never
		   finish) */
		mesh_http_release_finished(ctx);
		return;
	}
	http_server_request_buffer_payload(req, ctx->state,
		MESH_HTTP_MAX_STATE_SIZE, mesh_http_release_finished, ctx);
}

static void
mesh_http_api(struct http_server_request *req, const char *path)
{
	const struct http_request *hreq = http_server_request_get(req);
	bool is_get = strcmp(hreq->method, "GET") == 0;
	bool is_post = strcmp(hreq->method, "POST") == 0;
	struct mesh_peer *peer;
	struct mesh_user *user;
	const char *value;
	string_t *body = t_str_new(4096);

	if (strcmp(path, "heartbeat") == 0 && is_get)
		mesh_http_heartbeat(req);
	else if (strcmp(path, "status") == 0 && is_get) {
		mesh_status_json(body);
		mesh_http_send_json(req, body);
	} else if (strcmp(path, "users") == 0 && is_get) {
		mesh_users_json(body, mesh_http_query_param(hreq, "user"));
		mesh_http_send_json(req, body);
	} else if (strcmp(path, "sync") == 0 && is_post) {
		value = mesh_http_query_param(hreq, "user");
		if (value == NULL || value[0] == '\0') {
			http_server_request_fail(req, 400, "Missing user");
			return;
		}
		user = mesh_user_lookup(value, TRUE);
		value = mesh_http_query_param(hreq, "full");
		mesh_queue_push_all(user, MESH_PRIORITY_HIGH,
				    value != NULL && strcmp(value, "1") == 0);
		mesh_http_send_ok(req);
	} else if ((strcmp(path, "pause") == 0 ||
		    strcmp(path, "resume") == 0) && is_post) {
		value = mesh_http_query_param(hreq, "peer");
		peer = value == NULL ? NULL : mesh_peer_find(value);
		if (peer == NULL) {
			http_server_request_fail(req, 404, "Unknown peer");
			return;
		}
		peer->paused = strcmp(path, "pause") == 0;
		i_info("Peer %s %s by admin (HTTP)", peer->name,
		       peer->paused ? "paused" : "resumed");
		mesh_queue_peers_changed();
		mesh_http_send_ok(req);
	} else if (strcmp(path, "lease") == 0 && is_post) {
		mesh_http_lease(req, hreq);
	} else if (strcmp(path, "release") == 0 && is_post) {
		mesh_http_release(req, hreq);
	} else if (!is_get && !is_post) {
		http_server_request_fail_bad_method(req, "GET, POST");
	} else {
		http_server_request_fail(req, 404, "Not Found");
	}
}

static void
mesh_http_resource_request(void *context ATTR_UNUSED,
			   struct http_server_request *req,
			   const char *sub_path)
{
	const struct mesh_ui_file *file;
	string_t *body;

	if (!mesh_http_authorized(req))
		return;

	/* sub_path is relative to /mesh/ */
	if (str_begins(sub_path, "v1/", &sub_path)) {
		mesh_http_api(req, sub_path);
		return;
	}
	if (strcmp(sub_path, "") == 0 || strcmp(sub_path, "ui") == 0)
		sub_path = "ui/";
	if (str_begins(sub_path, "ui/", &sub_path)) {
		if (sub_path[0] == '\0')
			sub_path = "index.html";
		file = mesh_ui_file_find(sub_path);
		if (file == NULL) {
			http_server_request_fail(req, 404, "Not Found");
			return;
		}
		body = t_str_new(file->size);
		str_append_data(body, file->data, file->size);
		mesh_http_send(req, 200, file->content_type, body);
		return;
	}
	http_server_request_fail(req, 404, "Not Found");
}

static void
mesh_http_root_request(void *context ATTR_UNUSED,
		       struct http_server_request *req,
		       const char *sub_path ATTR_UNUSED)
{
	struct http_server_response *resp;

	resp = http_server_response_create(req, 302, "Found");
	http_server_response_add_header(resp, "Location", "/mesh/ui/");
	http_server_response_submit(resp);
}

static void
mesh_http_handle_request(void *context ATTR_UNUSED,
			 struct http_server_request *req)
{
	http_server_request_fail(req, 404, "Not Found");
}

static const struct http_server_callbacks mesh_http_callbacks = {
	.connection_destroy = mesh_http_conn_destroy,
	.handle_request = mesh_http_handle_request,
};

void mesh_http_conn_create(struct master_service_connection *conn)
{
	struct mesh_http_client *client = i_new(struct mesh_http_client, 1);

	client->http_conn = http_server_connection_create(mesh_http_server,
		conn->fd, conn->fd, conn->ssl, &mesh_http_callbacks, client);
}

static void mesh_http_conn_destroy(void *context, const char *reason ATTR_UNUSED)
{
	struct mesh_http_client *client = context;

	if (client->http_conn == NULL)
		return;
	client->http_conn = NULL;
	i_free(client);
	master_service_client_connection_destroyed(master_service);
}

void mesh_http_init(void)
{
	struct event *event = master_service_get_event(master_service);
	const char *error;
	pool_t pool;

	if (http_server_init_auto(event, &mesh_http_server, &error) < 0)
		i_fatal("http_server_init() failed: %s", error);

	pool = pool_alloconly_create("mesh http resources", 1024);
	mesh_http_resource = http_server_resource_create(mesh_http_server,
		pool, mesh_http_resource_request, NULL);
	http_server_resource_add_location(mesh_http_resource, "/mesh");
	mesh_root_resource = http_server_resource_create(mesh_http_server,
		pool, mesh_http_root_request, NULL);
	http_server_resource_add_location(mesh_root_resource, "/");
	pool_unref(&pool);
}

void mesh_http_deinit(void)
{
	http_server_deinit(&mesh_http_server);
}
