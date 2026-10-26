/* Copyright (c) Dovecot authors, see top-level COPYING file */

#include "mesh-common.h"
#include "ioloop.h"
#include "str.h"
#include "sha1.h"
#include "time-util.h"
#include "http-client.h"
#include "http-response.h"
#include "master-service.h"

#include <stdlib.h>

ARRAY_TYPE(mesh_peer) mesh_peers;

static struct http_client *mesh_http_client;
static struct timeout *mesh_heartbeat_to;

static void mesh_peer_set_up(struct mesh_peer *peer, bool up,
			     const char *reason)
{
	if (peer->up == up)
		return;
	peer->up = up;
	if (up) {
		peer->up_since = ioloop_timeval;
		i_info("Peer %s is up", peer->name);
	} else {
		i_info("Peer %s is down: %s", peer->name, reason);
	}
	mesh_queue_peers_changed();
}

static void mesh_peer_set_error(struct mesh_peer *peer, const char *error)
{
	i_free(peer->last_error);
	peer->last_error = i_strdup(error);
}

static const char *
http_response_get(const struct http_response *resp, const char *name)
{
	const char *value = http_response_header_get(resp, name);
	return value == NULL ? "" : value;
}

static void
mesh_heartbeat_response(const struct http_response *resp,
			struct mesh_peer *peer)
{
	unsigned long long remote_msecs;
	const char *node;
	long long now_msecs;

	peer->hb_req = NULL;
	if (resp->status / 100 != 2) {
		mesh_peer_set_error(peer, t_strdup_printf(
			"Heartbeat failed: %u %s", resp->status, resp->reason));
		return;
	}
	node = http_response_get(resp, "X-Mesh-Node");
	if (strcmp(node, peer->name) != 0) {
		/* a misconfigured peer address - don't sync with it */
		mesh_peer_set_error(peer, t_strdup_printf(
			"Heartbeat reply came from node '%s', expected '%s'",
			node, peer->name));
		mesh_peer_set_up(peer, FALSE, peer->last_error);
		return;
	}

	peer->last_seen = ioloop_timeval;
	peer->rtt_msecs = timeval_diff_msecs(&ioloop_timeval, &peer->hb_sent);
	if (str_to_ullong(http_response_get(resp, "X-Mesh-Time"),
			  &remote_msecs) == 0) {
		now_msecs = (long long)peer->hb_sent.tv_sec * 1000 +
			peer->hb_sent.tv_usec / 1000 + peer->rtt_msecs / 2;
		peer->clock_offset_msecs = (long long)remote_msecs - now_msecs;
	}
	i_free(peer->remote_version);
	peer->remote_version = i_strdup(http_response_get(resp, "X-Mesh-Version"));
	i_free(peer->remote_peers_up);
	peer->remote_peers_up =
		i_strdup(http_response_get(resp, "X-Mesh-Peers-Up"));
	if (str_to_uint(http_response_get(resp, "X-Mesh-Pending"),
			&peer->remote_pending) < 0)
		peer->remote_pending = 0;
	if (str_to_uint(http_response_get(resp, "X-Mesh-Running"),
			&peer->remote_running) < 0)
		peer->remote_running = 0;
	if (str_to_uint(http_response_get(resp, "X-Mesh-Users"),
			&peer->remote_users) < 0)
		peer->remote_users = 0;
	i_free(peer->last_error);
	mesh_peer_set_up(peer, TRUE, NULL);
}

static void mesh_heartbeat_send(struct mesh_peer *peer)
{
	struct http_client_request *req;

	if (peer->hb_req != NULL) {
		/* previous heartbeat still waiting */
		return;
	}
	if (peer->set->mesh_peer_http_port == 0) {
		mesh_peer_set_error(peer, "mesh_peer_http_port is not set");
		return;
	}
	req = mesh_peer_http_request(peer, "GET", "/mesh/v1/heartbeat",
		(void (*)(const struct http_response *, void *))
		mesh_heartbeat_response, peer);
	peer->hb_sent = ioloop_timeval;
	peer->hb_req = req;
	http_client_request_submit(req);
}

static void mesh_heartbeat_timeout(void *context ATTR_UNUSED)
{
	struct mesh_peer *peer;

	array_foreach_elem(&mesh_peers, peer) {
		if (peer->up &&
		    timeval_diff_msecs(&ioloop_timeval, &peer->last_seen) >
		    (long long)mesh_set->mesh_peer_down_timeout) {
			mesh_peer_set_up(peer, FALSE, peer->last_error != NULL ?
					 peer->last_error :
					 "Heartbeat timed out");
		}
		mesh_heartbeat_send(peer);
	}
}

struct http_client_request *
mesh_peer_http_request(struct mesh_peer *peer, const char *method,
		       const char *path,
		       void (*callback)(const struct http_response *resp,
					void *context),
		       void *context)
{
	struct http_client_request *req;

	/* callers have differently typed contexts, so skip the macro's
	   callback type check */
	req = (http_client_request)(mesh_http_client, method,
				    peer->set->mesh_peer_host, path,
				    (http_client_request_callback_t *)callback,
				    context);
	http_client_request_set_port(req, peer->set->mesh_peer_http_port);
	http_client_request_set_timeout_msecs(req,
		mesh_set->mesh_peer_down_timeout);
	http_client_request_add_header(req, "Authorization",
		t_strconcat("Bearer ", mesh_set->mesh_api_key, NULL));
	http_client_request_add_header(req, "X-Mesh-From", mesh_node_name);
	return req;
}

struct mesh_peer *mesh_peer_find(const char *name)
{
	struct mesh_peer *peer;

	array_foreach_elem(&mesh_peers, peer) {
		if (strcmp(peer->name, name) == 0)
			return peer;
	}
	return NULL;
}

bool mesh_peer_usable(const struct mesh_peer *peer)
{
	return peer->up && !peer->paused;
}

unsigned int mesh_peers_up_count(void)
{
	struct mesh_peer *peer;
	unsigned int count = 0;

	array_foreach_elem(&mesh_peers, peer) {
		if (peer->up)
			count++;
	}
	return count;
}

static void mesh_anchor_score(const char *username, const char *node,
			      unsigned char digest_r[SHA1_RESULTLEN])
{
	string_t *str = t_str_new(128);

	str_append(str, username);
	str_append_c(str, '\0');
	str_append(str, node);
	sha1_get_digest(str_data(str), str_len(str), digest_r);
}

/* Rendezvous hashing over the nodes that are up: every node computes the
   same anchor as long as they agree on which nodes are up, and a node going
   down moves only the users anchored to it. */
const char *mesh_anchor_name(const char *username)
{
	unsigned char best[SHA1_RESULTLEN], digest[SHA1_RESULTLEN];
	const char *best_name = mesh_node_name;
	struct mesh_peer *peer;

	T_BEGIN {
		mesh_anchor_score(username, mesh_node_name, best);
		array_foreach_elem(&mesh_peers, peer) {
			if (!peer->up)
				continue;
			mesh_anchor_score(username, peer->name, digest);
			if (memcmp(digest, best, sizeof(best)) > 0) {
				memcpy(best, digest, sizeof(best));
				best_name = peer->name;
			}
		}
	} T_END;
	return best_name;
}

struct mesh_peer *mesh_anchor_peer(const char *username)
{
	const char *name = mesh_anchor_name(username);

	if (name == mesh_node_name)
		return NULL;
	return mesh_peer_find(name);
}

void mesh_peers_init(void)
{
	const struct mesh_peer_settings *peer_set;
	struct mesh_peer *peer;
	const char *error;

	i_array_init(&mesh_peers, 8);
	array_foreach_elem(&mesh_set->parsed_peers, peer_set) {
		if (strcmp(peer_set->name, mesh_node_name) == 0) {
			/* allow the same config on every node */
			continue;
		}
		peer = i_new(struct mesh_peer, 1);
		peer->idx = array_count(&mesh_peers);
		peer->name = peer_set->name;
		peer->set = peer_set;
		array_push_back(&mesh_peers, &peer);
	}
	if (array_count(&mesh_peers) == 0)
		i_info("No mesh_peer configured - nothing to replicate to");

	if (http_client_init_auto(master_service_get_event(master_service),
				  &mesh_http_client, &error) < 0)
		i_fatal("http_client_init() failed: %s", error);
	mesh_heartbeat_to = timeout_add(mesh_set->mesh_heartbeat_interval,
					mesh_heartbeat_timeout, NULL);
	mesh_heartbeat_timeout(NULL);
}

void mesh_peers_deinit(void)
{
	struct mesh_peer *peer;

	timeout_remove(&mesh_heartbeat_to);
	array_foreach_elem(&mesh_peers, peer) {
		if (peer->hb_req != NULL)
			http_client_request_abort(&peer->hb_req);
	}
	http_client_deinit(&mesh_http_client);
	array_foreach_elem(&mesh_peers, peer) {
		i_free(peer->last_error);
		i_free(peer->remote_version);
		i_free(peer->remote_peers_up);
		i_free(peer);
	}
	array_free(&mesh_peers);
}
