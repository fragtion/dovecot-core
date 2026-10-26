/* Copyright (c) Dovecot authors, see top-level COPYING file */

#include "mesh-common.h"
#include "ioloop.h"
#include "llist.h"
#include "time-util.h"
#include "str.h"
#include "strescape.h"
#include "ostream.h"
#include "connection.h"
#include "master-service.h"

/* Two line-based protocols on UNIX sockets:

   mesh-notify (mail processes, mode 0666):
     NOTIFY <user> <high|normal> <dsync 0|1>
     WAIT <id> <user>
       Reply "DONE <id> ok" once a session that started after this
       finished successfully, i.e. what the user's process committed
       before is on another node, or right away "DONE <id> nopeers" if
       no peer is usable. Used to confirm deliveries only after that.

   mesh (doveadm mesh, mode 0600). Each command's reply is "+OK" or
   "-ERR <error>", then zero or more tab-separated rows, then an empty line:
     STATUS
     PEERS
     USERS [<user>]
     SYNC <user> [full]
     PAUSE <peer>
     RESUME <peer>
     VERIFY <user>   (rows as in mesh_verify_result_rows()) */

struct mesh_conn {
	struct connection conn;
	bool admin;
	/* VERIFY is running; input is halted until it finishes */
	struct mesh_verify *verify;
};

/* forget a waiting delivery that's this old: its process gave up long
   ago */
#define MESH_WAITER_MAX_AGE_SECS 300

struct mesh_waiter {
	struct mesh_waiter *prev, *next;
	struct mesh_conn *conn;
	char *username, *id;
	struct timeval since;
};

static struct connection_list *mesh_notify_conns, *mesh_admin_conns;
static struct mesh_waiter *mesh_waiters;

static void mesh_waiter_free(struct mesh_waiter *waiter)
{
	DLLIST_REMOVE(&mesh_waiters, waiter);
	i_free(waiter->username);
	i_free(waiter->id);
	i_free(waiter);
}

static void mesh_waiter_reply(struct mesh_conn *mconn, const char *id,
			      const char *result)
{
	string_t *str = t_str_new(64);

	str_append(str, "DONE\t");
	str_append_tabescaped(str, id);
	str_printfa(str, "\t%s\n", result);
	o_stream_nsend(mconn->conn.output, str_data(str), str_len(str));
}

static bool mesh_peers_usable(void)
{
	struct mesh_peer *peer;

	array_foreach_elem(&mesh_peers, peer) {
		if (mesh_peer_usable(peer))
			return TRUE;
	}
	return FALSE;
}

void mesh_conn_user_replicated(const struct mesh_user *user,
			       const struct timeval *start_time)
{
	struct mesh_waiter *waiter, *next;

	for (waiter = mesh_waiters; waiter != NULL; waiter = next) {
		next = waiter->next;
		if (ioloop_time - waiter->since.tv_sec >
		    MESH_WAITER_MAX_AGE_SECS) {
			mesh_waiter_free(waiter);
			continue;
		}
		if (strcmp(waiter->username, user->username) != 0 ||
		    timeval_cmp(&waiter->since, start_time) > 0)
			continue;
		e_debug(mesh_event, "Delivery for %s replicated after %lld ms",
			waiter->username,
			timeval_diff_msecs(&ioloop_timeval, &waiter->since));
		mesh_waiter_reply(waiter->conn, waiter->id, "ok");
		mesh_waiter_free(waiter);
	}
}

static enum mesh_priority mesh_priority_parse(const char *str)
{
	if (strcmp(str, "high") == 0)
		return MESH_PRIORITY_HIGH;
	return MESH_PRIORITY_NORMAL;
}

static int mesh_notify_input_args(struct connection *conn,
				  const char *const *args)
{
	struct mesh_conn *mconn = container_of(conn, struct mesh_conn, conn);
	struct mesh_waiter *waiter;

	if (str_array_length(args) >= 3 && strcmp(args[0], "WAIT") == 0) {
		if (!mesh_peers_usable()) {
			mesh_waiter_reply(mconn, args[1], "nopeers");
			return 1;
		}
		waiter = i_new(struct mesh_waiter, 1);
		waiter->conn = mconn;
		waiter->id = i_strdup(args[1]);
		waiter->username = i_strdup(args[2]);
		waiter->since = ioloop_timeval;
		DLLIST_PREPEND(&mesh_waiters, waiter);
		mesh_queue_expedite(mesh_user_lookup(args[2], TRUE));
		return 1;
	}
	if (str_array_length(args) < 4 || strcmp(args[0], "NOTIFY") != 0) {
		e_error(conn->event, "Invalid input: %s",
			t_strarray_join(args, " "));
		return -1;
	}
	mesh_queue_notify(args[1], mesh_priority_parse(args[2]),
			  strcmp(args[3], "1") == 0);
	return 1;
}

static const char *mesh_time_str(time_t t)
{
	return t == 0 ? "0" : dec2str(t);
}

static void mesh_admin_status(string_t *str)
{
	str_append(str, "+OK\n");
	str_printfa(str, "node\t%s\n", mesh_node_name);
	str_printfa(str, "topology\t%s\n", mesh_set->mesh_topology);
	str_printfa(str, "peers\t%u\n", array_count(&mesh_peers));
	str_printfa(str, "peers_up\t%u\n", mesh_peers_up_count());
	str_printfa(str, "users\t%u\n", hash_table_count(mesh_users));
	str_printfa(str, "pending\t%u\n", mesh_queue_pending_count());
	str_printfa(str, "running\t%u\n", mesh_queue_running_count());
	str_printfa(str, "uptime_secs\t%ld\n",
		    (long)(ioloop_time - mesh_start_time));
}

static void mesh_admin_peers(string_t *str)
{
	struct mesh_peer *peer;

	str_append(str, "+OK\n");
	array_foreach_elem(&mesh_peers, peer) {
		str_append_tabescaped(str, peer->name);
		str_printfa(str, "\t%s\t%s\t%u\t%lld\t%s\t%u\t%llu\t%llu\t",
			    peer->up ? "up" : "down",
			    peer->paused ? "paused" : "-",
			    peer->rtt_msecs, peer->clock_offset_msecs,
			    mesh_time_str(peer->last_seen.tv_sec),
			    peer->running_syncs,
			    (unsigned long long)peer->syncs_ok,
			    (unsigned long long)peer->syncs_failed);
		if (peer->last_error != NULL)
			str_append_tabescaped(str, peer->last_error);
		str_append_c(str, '\n');
	}
}

static void mesh_admin_user(string_t *str, struct mesh_user *user)
{
	struct mesh_peer *peer;
	const struct mesh_user_peer *up;
	const char *anchor = mesh_anchor_name(user->username);

	array_foreach_elem(&mesh_peers, peer) {
		up = &user->peers[peer->idx];
		str_append_tabescaped(str, user->username);
		str_append_c(str, '\t');
		str_append_tabescaped(str, peer->name);
		str_printfa(str, "\t%s\t%s\t%s\t%s\t%u\t%u\t",
			    anchor, mesh_user_peer_status(user, peer),
			    mesh_time_str(up->last_ok),
			    up->pending == MESH_PRIORITY_NONE ? "0" :
			    mesh_time_str(up->pending_since),
			    up->ok_count, up->fail_count);
		if (up->last_error != NULL)
			str_append_tabescaped(str, up->last_error);
		str_append_c(str, '\n');
	}
}

static void mesh_admin_users(string_t *str, const char *username)
{
	struct hash_iterate_context *iter;
	struct mesh_user *user;
	char *name;

	str_append(str, "+OK\n");
	if (username != NULL) {
		user = mesh_user_lookup(username, FALSE);
		if (user != NULL)
			mesh_admin_user(str, user);
		return;
	}
	iter = hash_table_iterate_init(mesh_users);
	while (hash_table_iterate(iter, mesh_users, &name, &user))
		mesh_admin_user(str, user);
	hash_table_iterate_deinit(&iter);
}

static void
mesh_admin_verify_done(const struct mesh_verify_result *result,
		       struct mesh_conn *mconn)
{
	string_t *str = t_str_new(1024);

	mconn->verify = NULL;
	str_append(str, "+OK\n");
	mesh_verify_result_rows(result, str);
	str_append_c(str, '\n');
	o_stream_nsend(mconn->conn.output, str_data(str), str_len(str));
	connection_input_resume(&mconn->conn);
}

static void mesh_admin_cmd(string_t *str, const char *const *args)
{
	struct mesh_peer *peer;
	struct mesh_user *user;

	if (strcmp(args[0], "STATUS") == 0)
		mesh_admin_status(str);
	else if (strcmp(args[0], "PEERS") == 0)
		mesh_admin_peers(str);
	else if (strcmp(args[0], "USERS") == 0)
		mesh_admin_users(str, args[1]);
	else if (strcmp(args[0], "SYNC") == 0 && args[1] != NULL) {
		user = mesh_user_lookup(args[1], TRUE);
		mesh_queue_push_all(user, MESH_PRIORITY_HIGH,
				    args[2] != NULL &&
				    strcmp(args[2], "full") == 0);
		str_append(str, "+OK\n");
	} else if ((strcmp(args[0], "PAUSE") == 0 ||
		    strcmp(args[0], "RESUME") == 0) && args[1] != NULL) {
		peer = mesh_peer_find(args[1]);
		if (peer == NULL) {
			str_printfa(str, "-ERR Unknown peer: %s\n",
				    str_tabescape(args[1]));
			return;
		}
		peer->paused = strcmp(args[0], "PAUSE") == 0;
		i_info("Peer %s %s by admin", peer->name,
		       peer->paused ? "paused" : "resumed");
		mesh_queue_peers_changed();
		str_append(str, "+OK\n");
	} else {
		str_printfa(str, "-ERR Unknown command: %s\n",
			    str_tabescape(args[0]));
	}
}

static int mesh_admin_input_args(struct connection *conn,
				 const char *const *args)
{
	struct mesh_conn *mconn = container_of(conn, struct mesh_conn, conn);
	string_t *str = t_str_new(1024);

	if (args[0] == NULL) {
		e_error(conn->event, "Empty command");
		return -1;
	}
	if (strcmp(args[0], "VERIFY") == 0 && args[1] != NULL) {
		/* answered when all nodes have replied */
		connection_input_halt(conn);
		mconn->verify = mesh_verify_start(args[1],
			(mesh_verify_callback_t *)mesh_admin_verify_done,
			mconn);
		return 1;
	}
	mesh_admin_cmd(str, args);
	str_append_c(str, '\n');
	o_stream_nsend(conn->output, str_data(str), str_len(str));
	return 1;
}

static void mesh_conn_destroy(struct connection *conn)
{
	struct mesh_conn *mconn = container_of(conn, struct mesh_conn, conn);

	struct mesh_waiter *waiter, *next;

	if (mconn->verify != NULL)
		mesh_verify_detach(&mconn->verify);
	for (waiter = mesh_waiters; waiter != NULL; waiter = next) {
		next = waiter->next;
		if (waiter->conn == mconn)
			mesh_waiter_free(waiter);
	}
	connection_deinit(conn);
	i_free(mconn);
	master_service_client_connection_destroyed(master_service);
}

static const struct connection_vfuncs mesh_notify_vfuncs = {
	.destroy = mesh_conn_destroy,
	.input_args = mesh_notify_input_args,
};

static const struct connection_settings mesh_notify_set = {
	.service_name_in = "mesh-notify",
	.service_name_out = "mesh",
	.major_version = 1,
	.minor_version = 0,
	.input_max_size = 1024*16,
	.output_max_size = 1024*16,
};

static const struct connection_vfuncs mesh_admin_vfuncs = {
	.destroy = mesh_conn_destroy,
	.input_args = mesh_admin_input_args,
};

static const struct connection_settings mesh_admin_set = {
	.service_name_in = "mesh-admin",
	.service_name_out = "mesh",
	.major_version = 1,
	.minor_version = 0,
	.input_max_size = 1024*16,
	.output_max_size = SIZE_MAX,
};

void mesh_conn_create(int fd, bool admin)
{
	struct mesh_conn *mconn = i_new(struct mesh_conn, 1);

	mconn->admin = admin;
	connection_init_server(admin ? mesh_admin_conns : mesh_notify_conns,
			       &mconn->conn, admin ? "mesh" : "mesh-notify",
			       fd, fd);
}

void mesh_conn_init(void)
{
	mesh_notify_conns = connection_list_init(&mesh_notify_set,
						 &mesh_notify_vfuncs);
	mesh_admin_conns = connection_list_init(&mesh_admin_set,
						&mesh_admin_vfuncs);
}

void mesh_conn_deinit(void)
{
	while (mesh_waiters != NULL)
		mesh_waiter_free(mesh_waiters);
	connection_list_deinit(&mesh_notify_conns);
	connection_list_deinit(&mesh_admin_conns);
}
