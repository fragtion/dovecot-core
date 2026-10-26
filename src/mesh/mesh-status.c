/* Copyright (c) Dovecot authors, see top-level COPYING file */

#include "mesh-common.h"
#include "ioloop.h"
#include "str.h"
#include "json-generator.h"

static void json_append_str(string_t *dest, const char *key, const char *value)
{
	str_append_c(dest, '"');
	str_append(dest, key);
	str_append(dest, "\":\"");
	json_append_escaped(dest, value == NULL ? "" : value);
	str_append_c(dest, '"');
}

static void json_append_num(string_t *dest, const char *key, long long value)
{
	str_printfa(dest, "\"%s\":%lld", key, value);
}

static void json_append_bool(string_t *dest, const char *key, bool value)
{
	str_printfa(dest, "\"%s\":%s", key, value ? "true" : "false");
}

static long long mesh_now_msecs(void)
{
	return (long long)ioloop_timeval.tv_sec * 1000 +
		ioloop_timeval.tv_usec / 1000;
}

static void mesh_peer_json(string_t *dest, const struct mesh_peer *peer)
{
	str_append_c(dest, '{');
	json_append_str(dest, "name", peer->name);
	str_append_c(dest, ',');
	json_append_str(dest, "host", peer->set->mesh_peer_host);
	str_append_c(dest, ',');
	json_append_bool(dest, "up", peer->up);
	str_append_c(dest, ',');
	json_append_bool(dest, "paused", peer->paused);
	str_append_c(dest, ',');
	json_append_num(dest, "rtt_ms", peer->rtt_msecs);
	str_append_c(dest, ',');
	json_append_num(dest, "clock_offset_ms", peer->clock_offset_msecs);
	str_append_c(dest, ',');
	json_append_num(dest, "last_seen", peer->last_seen.tv_sec);
	str_append_c(dest, ',');
	json_append_num(dest, "up_since", peer->up ? peer->up_since.tv_sec : 0);
	str_append_c(dest, ',');
	json_append_num(dest, "running", peer->running_syncs);
	str_append_c(dest, ',');
	json_append_num(dest, "syncs_ok", peer->syncs_ok);
	str_append_c(dest, ',');
	json_append_num(dest, "syncs_failed", peer->syncs_failed);
	str_append_c(dest, ',');
	json_append_num(dest, "last_sync_ok", peer->last_sync_ok);
	str_append_c(dest, ',');
	json_append_num(dest, "last_sync_failed", peer->last_sync_failed);
	str_append_c(dest, ',');
	json_append_str(dest, "last_error", peer->last_error);
	str_append(dest, ",\"remote\":{");
	json_append_str(dest, "version", peer->remote_version);
	str_append_c(dest, ',');
	json_append_num(dest, "pending", peer->remote_pending);
	str_append_c(dest, ',');
	json_append_num(dest, "running", peer->remote_running);
	str_append_c(dest, ',');
	json_append_num(dest, "users", peer->remote_users);
	str_append_c(dest, ',');
	json_append_str(dest, "peers_up", peer->remote_peers_up);
	str_append(dest, "}}");
}

void mesh_status_json(string_t *dest)
{
	struct mesh_peer *peer;
	bool first = TRUE;

	str_append_c(dest, '{');
	json_append_str(dest, "node", mesh_node_name);
	str_append_c(dest, ',');
	json_append_str(dest, "version", PACKAGE_VERSION);
	str_append_c(dest, ',');
	json_append_str(dest, "topology", mesh_set->mesh_topology);
	str_append_c(dest, ',');
	json_append_num(dest, "time_ms", mesh_now_msecs());
	str_append_c(dest, ',');
	json_append_num(dest, "started", mesh_start_time);
	str_append_c(dest, ',');
	json_append_num(dest, "users", hash_table_count(mesh_users));
	str_append_c(dest, ',');
	json_append_num(dest, "pending", mesh_queue_pending_count());
	str_append_c(dest, ',');
	json_append_num(dest, "running", mesh_queue_running_count());
	str_append_c(dest, ',');
	json_append_num(dest, "max_parallel_syncs",
			mesh_set->mesh_max_parallel_syncs);
	str_append(dest, ",\"peers\":[");
	array_foreach_elem(&mesh_peers, peer) {
		if (!first)
			str_append_c(dest, ',');
		first = FALSE;
		mesh_peer_json(dest, peer);
	}
	str_append(dest, "]}");
}

static const char *
mesh_user_peer_status_str(const struct mesh_user *user,
			  const struct mesh_peer *peer)
{
	const struct mesh_user_peer *up = &user->peers[peer->idx];

	if (user->running_peer == peer)
		return "syncing";
	if (up->pending != MESH_PRIORITY_NONE)
		return up->failures > 0 ? "error" : "pending";
	if (up->last_ok == 0)
		return "never";
	return "ok";
}

static void mesh_user_json(string_t *dest, const struct mesh_user *user)
{
	const struct mesh_user_peer *up;
	struct mesh_peer *peer;
	bool first = TRUE;

	str_append_c(dest, '{');
	json_append_str(dest, "user", user->username);
	str_append_c(dest, ',');
	json_append_str(dest, "anchor", mesh_anchor_name(user->username));
	str_append_c(dest, ',');
	json_append_num(dest, "last_change", user->last_change);
	str_append(dest, ",\"peers\":[");
	array_foreach_elem(&mesh_peers, peer) {
		up = &user->peers[peer->idx];
		if (!first)
			str_append_c(dest, ',');
		first = FALSE;
		str_append_c(dest, '{');
		json_append_str(dest, "peer", peer->name);
		str_append_c(dest, ',');
		json_append_str(dest, "status",
				mesh_user_peer_status_str(user, peer));
		str_append_c(dest, ',');
		json_append_num(dest, "last_ok", up->last_ok);
		str_append_c(dest, ',');
		json_append_num(dest, "last_attempt", up->last_attempt);
		str_append_c(dest, ',');
		json_append_num(dest, "last_full", up->last_full);
		str_append_c(dest, ',');
		json_append_num(dest, "pending_since",
				up->pending == MESH_PRIORITY_NONE ? 0 :
				up->pending_since);
		str_append_c(dest, ',');
		json_append_num(dest, "ok_count", up->ok_count);
		str_append_c(dest, ',');
		json_append_num(dest, "fail_count", up->fail_count);
		str_append_c(dest, ',');
		json_append_num(dest, "failures", up->failures);
		str_append_c(dest, ',');
		json_append_num(dest, "last_ms", up->last_msecs);
		str_append_c(dest, ',');
		json_append_bool(dest, "has_state", up->dsync_state != NULL);
		str_append_c(dest, ',');
		json_append_num(dest, "state_gen", up->state_gen);
		str_append_c(dest, ',');
		json_append_bool(dest, "exclusive", up->exclusive);
		str_append_c(dest, ',');
		json_append_str(dest, "last_error", up->last_error);
		str_append_c(dest, '}');
	}
	str_append(dest, "]}");
}

void mesh_users_json(string_t *dest, const char *only_user)
{
	struct hash_iterate_context *iter;
	struct mesh_user *user;
	char *username;
	bool first = TRUE;

	str_append_c(dest, '{');
	json_append_str(dest, "node", mesh_node_name);
	str_append(dest, ",\"users\":[");
	if (only_user != NULL) {
		user = mesh_user_lookup(only_user, FALSE);
		if (user != NULL)
			mesh_user_json(dest, user);
	} else {
		iter = hash_table_iterate_init(mesh_users);
		while (hash_table_iterate(iter, mesh_users, &username, &user)) {
			if (!first)
				str_append_c(dest, ',');
			first = FALSE;
			mesh_user_json(dest, user);
		}
		hash_table_iterate_deinit(&iter);
	}
	str_append(dest, "]}");
}

void mesh_heartbeat_headers(ARRAY_TYPE(const_string) *headers)
{
	struct mesh_peer *peer;
	string_t *peers_up = t_str_new(64);
	const char *value;

	array_foreach_elem(&mesh_peers, peer) {
		if (!peer->up)
			continue;
		if (str_len(peers_up) > 0)
			str_append_c(peers_up, ',');
		str_append(peers_up, peer->name);
	}

#define ADD(key, val) STMT_START { \
	value = key; array_push_back(headers, &value); \
	value = val; array_push_back(headers, &value); \
	} STMT_END
	ADD("X-Mesh-Node", mesh_node_name);
	ADD("X-Mesh-Version", PACKAGE_VERSION);
	ADD("X-Mesh-Time", t_strdup_printf("%lld", mesh_now_msecs()));
	ADD("X-Mesh-Pending", dec2str(mesh_queue_pending_count()));
	ADD("X-Mesh-Running", dec2str(mesh_queue_running_count()));
	ADD("X-Mesh-Users", dec2str(hash_table_count(mesh_users)));
	ADD("X-Mesh-Peers-Up", str_c(peers_up));
#undef ADD
}
