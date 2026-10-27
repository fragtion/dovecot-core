/* Copyright (c) Dovecot authors, see top-level COPYING file */

#include "mesh-common.h"
#include "ioloop.h"
#include "llist.h"
#include "str.h"
#include "strescape.h"
#include "time-util.h"
#include "master-service.h"
#include "master-service-settings.h"
#include "doveadm-protocol.h"
#include "doveadm-client.h"
#include "http-client.h"
#include "http-response.h"

#include <ctype.h>

/* dsync's own per-user lock timeout (-l) */
#define MESH_DSYNC_LOCK_TIMEOUT_SECS 30
/* Give up waiting for the doveadm server's reply after this long. dsync
   itself stops much earlier if data stops moving (mesh_sync_timeout), so
   this is only a safety net: a session can take long when much has to be
   copied over a slow link. */
#define MESH_SYNC_MAX_SECS (2*60*60)

struct mesh_sync {
	struct mesh_sync *prev, *next;

	struct mesh_user *user;
	struct mesh_peer *peer;
	bool full;

	/* the peer lent us the user for this session */
	struct http_client_request *lease_req;
	bool leased;
	/* renews the lease while the session runs */
	struct timeout *to_renew;
	struct http_client_request *renew_req;
	/* the newer of our and the peer's state_gen when the lease was
	   granted */
	unsigned int base_gen;

	struct doveadm_client *conn;
	string_t *output;
	struct timeval start_time;
	struct timeout *to;
	/* mesh_sync_finished event, created when the session starts so that
	   it gets the duration */
	struct event *event;
	/* the session used a dsync state */
	bool stateful;
};

static struct mesh_sync *mesh_syncs;

void mesh_url_encode(string_t *dest, const char *src)
{
	for (; *src != '\0'; src++) {
		if (i_isalnum(*src) || strchr("-._~", *src) != NULL)
			str_append_c(dest, *src);
		else
			str_printfa(dest, "%%%02X", (unsigned char)*src);
	}
}

/* A lease outlives a session that stalled until dsync gave up on it. */
static unsigned int mesh_lease_secs(void)
{
	return mesh_set->mesh_sync_timeout + 60;
}

static string_t *
mesh_lease_path(const char *action, const struct mesh_user *user)
{
	string_t *path = t_str_new(128);

	str_printfa(path, "/mesh/v1/%s?user=", action);
	mesh_url_encode(path, user->username);
	str_append(path, "&node=");
	mesh_url_encode(path, mesh_node_name);
	return path;
}

static void
mesh_lease_release_response(const struct http_response *resp,
			    void *context ATTR_UNUSED)
{
	if (resp->status / 100 != 2) {
		/* The peer keeps its older state copy, so the generations
		   differ and the next session is a stateless one. */
		e_debug(mesh_event, "Lease release failed: %u %s",
			resp->status, resp->reason);
	}
}

/* Give the user back to the peer, along with the pair's new state. ok
   tells that the session succeeded, i.e. both nodes have everything that
   was committed before the lease. */
static void mesh_sync_release(struct mesh_user *user, struct mesh_peer *peer,
			      bool ok)
{
	const struct mesh_user_peer *up = &user->peers[peer->idx];
	struct http_client_request *req;
	string_t *path = mesh_lease_path("release", user);

	str_printfa(path, "&gen=%u&ok=%d", up->state_gen, ok ? 1 : 0);
	req = mesh_peer_http_request(peer, "POST", str_c(path),
				     mesh_lease_release_response, NULL);
	if (up->dsync_state != NULL) {
		http_client_request_set_payload_data(req,
			(const unsigned char *)up->dsync_state,
			strlen(up->dsync_state));
	} else {
		http_client_request_set_payload_empty(req);
	}
	http_client_request_submit(req);
}

static void mesh_sync_free(struct mesh_sync *sync)
{
	DLLIST_REMOVE(&mesh_syncs, sync);
	if (sync->lease_req != NULL)
		http_client_request_abort(&sync->lease_req);
	if (sync->renew_req != NULL)
		http_client_request_abort(&sync->renew_req);
	timeout_remove(&sync->to_renew);
	timeout_remove(&sync->to);
	if (sync->conn != NULL)
		doveadm_client_unref(&sync->conn);
	str_free(&sync->output);
	event_unref(&sync->event);
	i_free(sync);
}

static const char *mesh_sync_result(int exit_code)
{
	switch (exit_code) {
	case 0:
		return "ok";
	case DOVEADM_EX_CHANGED:
		return "changed";
	case MESH_SYNC_EXIT_BUSY:
		return "busy";
	default:
		return "failed";
	}
}

static void mesh_sync_send_event(struct mesh_sync *sync, int exit_code,
				 const char *error, unsigned int msecs)
{
	const char *result = mesh_sync_result(exit_code);
	struct event_passthrough *e =
		event_create_passthrough(sync->event)->
		set_name("mesh_sync_finished")->
		add_str("result", result)->
		add_int("exit_code", exit_code)->
		add_int("full", sync->full ? 1 : 0)->
		add_int("stateful", sync->stateful ? 1 : 0);

	if (error != NULL)
		e->add_str("error", error);
	if (exit_code != MESH_SYNC_EXIT_BUSY) {
		mesh_status_add_sync(sync->user, sync->peer, result,
				     sync->full, sync->stateful, msecs, error);
	}
	e_debug(e->event(), "Synced %s with %s in %u ms: %s%s%s%s",
		sync->user->username, sync->peer->name, msecs, result,
		exit_code == MESH_SYNC_EXIT_BUSY || sync->stateful ? "" :
		sync->full ? " (full)" : " (stateless)",
		error == NULL ? "" : ": ", error == NULL ? "" : error);
}

static void mesh_sync_finish(struct mesh_sync *sync, int exit_code,
			     const char *error)
{
	struct mesh_user *user = sync->user;
	struct mesh_peer *peer = sync->peer;
	bool full = sync->full, leased = sync->leased;
	bool ok = exit_code == 0 || exit_code == DOVEADM_EX_CHANGED;
	struct timeval start_time = sync->start_time;
	unsigned int base_gen = sync->base_gen;
	unsigned int msecs =
		timeval_diff_msecs(&ioloop_timeval, &sync->start_time);
	const char *state = t_strdup(t_str_trim(str_c(sync->output), " \t\r\n"));

	mesh_sync_send_event(sync, exit_code, error, msecs);
	mesh_sync_free(sync);
	mesh_queue_sync_finished(user, peer, exit_code, error, state, full,
				 msecs);
	if (leased) {
		/* even an unchanged state gets a new generation, so that the
		   copies are known to match only if this release arrives */
		user->peers[peer->idx].state_gen = base_gen + 1;
		mesh_state_changed();
		mesh_sync_release(user, peer, ok);
	}
	if (ok)
		mesh_conn_user_replicated(user, &start_time);
}

static void
mesh_sync_print(const unsigned char *data, size_t size,
		bool finished ATTR_UNUSED, struct mesh_sync *sync)
{
	/* the only printed field is the new dsync state */
	str_append_data(sync->output, data, size);
}

static void
mesh_sync_reply(const struct doveadm_server_reply *reply, void *context)
{
	struct mesh_sync *sync = context;
	const char *error = NULL;

	if (reply->exit_code != 0 &&
	    reply->exit_code != DOVEADM_EX_CHANGED) {
		error = t_strdup_printf("doveadm sync failed: %s%s%s",
			doveadm_exit_code_to_str(reply->exit_code),
			reply->error == NULL ? "" : ": ",
			reply->error == NULL ? "" : reply->error);
	}
	mesh_sync_finish(sync, reply->exit_code, error);
}

static void mesh_sync_no_reply(struct mesh_sync *sync)
{
	mesh_sync_finish(sync, DOVEADM_EX_UNKNOWN, t_strdup_printf(
		"No reply from the doveadm server in %u seconds",
		MESH_SYNC_MAX_SECS));
}

static void
mesh_sync_lease_renew_response(const struct http_response *resp,
			       void *context)
{
	struct mesh_sync *sync = context;

	sync->renew_req = NULL;
	if (resp->status / 100 != 2) {
		e_debug(mesh_event, "Renewing the lease of %s from %s failed: "
			"%u %s", sync->user->username, sync->peer->name,
			resp->status, resp->reason);
	}
}

/* A session can last longer than its lease, e.g. when much has to be
   copied over a slow link. Keep the user leased while it runs, so that the
   peer doesn't start sessions of its own for the user meanwhile. */
static void mesh_sync_lease_renew(struct mesh_sync *sync)
{
	string_t *path;

	if (sync->renew_req != NULL)
		return;
	path = mesh_lease_path("lease", sync->user);
	str_printfa(path, "&secs=%u&renew=1", mesh_lease_secs());
	sync->renew_req = mesh_peer_http_request(sync->peer, "POST",
		str_c(path), mesh_sync_lease_renew_response, sync);
	http_client_request_submit(sync->renew_req);
}

static const char *mesh_doveadm_get_socket_path(void)
{
	const char *path = mesh_set->mesh_doveadm_socket_path;

	if (path[0] == '/')
		return path;
	return t_strconcat(master_service_get_service_settings(master_service)->base_dir,
			   "/", path, NULL);
}

int mesh_doveadm_client_create(struct doveadm_client **conn_r,
			       const char **error_r)
{
	/* The doveadm-server socket is mode 0600, which the doveadm server
	   treats as preauthenticated, so no real credentials are needed. */
	struct doveadm_client_settings set = {
		.socket_path = mesh_doveadm_get_socket_path(),
		.username = "doveadm",
		.password = "",
	};

	return doveadm_client_create(&set, conn_r, error_r);
}

static const char *mesh_sync_build_cmd(struct mesh_sync *sync)
{
	const struct mesh_peer_settings *pset = sync->peer->set;
	const struct mesh_user_peer *up = &sync->user->peers[sync->peer->idx];
	string_t *cmd = t_str_new(512);

	/* <flags> <username> <command> [<args>] */
	if (mesh_set->mesh_dsync_debug)
		str_append_c(cmd, DOVEADM_PROTOCOL_CMD_FLAG_DEBUG);
	str_append_c(cmd, '\t');
	str_append_tabescaped(cmd, sync->user->username);
	str_append(cmd, "\tsync");
	/* -s makes doveadm print the new state, so give it even for full
	   syncs (as empty) */
	str_append(cmd, "\t-s\t");
	if (sync->full)
		str_append(cmd, "\t-f");
	else if (up->dsync_state != NULL)
		str_append_tabescaped(cmd, up->dsync_state);
	str_printfa(cmd, "\t-l\t%u", MESH_DSYNC_LOCK_TIMEOUT_SECS);
	str_printfa(cmd, "\t-T\t%u", mesh_set->mesh_sync_timeout);
	str_append_c(cmd, '\t');
	str_append_tabescaped(cmd, t_strdup_printf("%s:%s:%u",
		pset->mesh_peer_doveadm_ssl ? "tcps" : "tcp",
		pset->mesh_peer_host, pset->mesh_peer_doveadm_port));
	str_append_c(cmd, '\n');
	return str_c(cmd);
}

static void mesh_sync_run_doveadm(struct mesh_sync *sync)
{
	struct doveadm_client_cmd_settings cmd_set = {
		.proxy_ttl = DOVEADM_PROXY_TTL,
	};
	const char *error;

	if (mesh_doveadm_client_create(&sync->conn, &error) < 0) {
		mesh_sync_finish(sync, DOVEADM_EX_UNKNOWN, t_strdup_printf(
			"Couldn't connect to doveadm server: %s", error));
		return;
	}
	sync->to = timeout_add(MESH_SYNC_MAX_SECS * 1000,
			       mesh_sync_no_reply, sync);
	sync->to_renew = timeout_add(mesh_lease_secs() * 1000 / 2,
				     mesh_sync_lease_renew, sync);
	doveadm_client_set_print(sync->conn, mesh_sync_print, sync);
	doveadm_client_cmd(sync->conn, &cmd_set, mesh_sync_build_cmd(sync),
			   NULL, mesh_sync_reply, sync);
}

/* The peer agreed to the session. It also told whether our copy of the
   pair's dsync state can be used.

   A stateful dsync session treats every mail above the state's last common
   UID as new, and decides what to do with it from the mailboxes alone. That
   only works if the mails that both nodes have were synced between them,
   in sessions the state describes. Otherwise, for example after a mail
   reached both nodes through the anchor, and was then expunged on one of
   them, dsync can't tell that from a mail new on the other node, and
   copies the mail back. So the state is used only if

    - it's the newest one, i.e. our and the peer's copies have the same
      generation (a lost release leaves the peer's older), and
    - at least one of the nodes has had no session for the user with any
      other node since their last session with each other. Then mails
      can reach both nodes only through these sessions.

   With the anchor topology a spoke syncs only with its anchor, so this
   holds except after the anchor changed. Otherwise the session is
   stateless: dsync compares the mailboxes, which is slower but safe. */
static void
mesh_sync_lease_response(const struct http_response *resp, void *context)
{
	struct mesh_sync *sync = context;
	struct mesh_user_peer *up = &sync->user->peers[sync->peer->idx];
	const char *value;
	unsigned int remote_gen;

	sync->lease_req = NULL;
	if (resp->status == 409) {
		mesh_sync_finish(sync, MESH_SYNC_EXIT_BUSY, "Peer is busy");
		return;
	}
	if (resp->status / 100 != 2) {
		mesh_sync_finish(sync, DOVEADM_EX_UNKNOWN, t_strdup_printf(
			"Lease from peer failed: %u %s",
			resp->status, resp->reason));
		return;
	}
	sync->leased = TRUE;
	value = http_response_header_get(resp, "X-Mesh-Gen");
	if (value == NULL || str_to_uint(value, &remote_gen) < 0)
		remote_gen = 0;
	sync->base_gen = I_MAX(up->state_gen, remote_gen);
	value = http_response_header_get(resp, "X-Mesh-State-Usable");
	if ((value == NULL || strcmp(value, "1") != 0) &&
	    up->dsync_state != NULL) {
		e_debug(mesh_event, "Syncing %s with %s without state: %s",
			sync->user->username, sync->peer->name,
			value == NULL ? "peer didn't answer" :
			up->state_gen != remote_gen ?
			"state generation differs" :
			"both nodes have synced with others since");
		mesh_user_peer_set_state(up, NULL);
	}
	mesh_user_mark_session(sync->user, sync->peer);
	sync->stateful = !sync->full && up->dsync_state != NULL;
	mesh_sync_run_doveadm(sync);
}

void mesh_sync_start(struct mesh_user *user, struct mesh_peer *peer,
		     bool full)
{
	const struct mesh_user_peer *up = &user->peers[peer->idx];
	struct mesh_sync *sync;
	string_t *path;

	sync = i_new(struct mesh_sync, 1);
	sync->user = user;
	sync->peer = peer;
	sync->full = full;
	sync->output = str_new(default_pool, 128);
	sync->start_time = ioloop_timeval;
	sync->event = event_create(mesh_event);
	event_add_str(sync->event, "user", user->username);
	event_add_str(sync->event, "peer", peer->name);
	DLLIST_PREPEND(&mesh_syncs, sync);

	/* Ask the peer for a lease first. The peer refuses if it's syncing
	   the user itself, so the user's sessions never overlap on either
	   node. Otherwise they'd wait for each other's locks, and the pair's
	   state would get two different successors. */
	path = mesh_lease_path("lease", user);
	str_printfa(path, "&secs=%u&gen=%u&exclusive=%d",
		    mesh_lease_secs(), up->state_gen,
		    up->exclusive ? 1 : 0);
	sync->lease_req = mesh_peer_http_request(peer, "POST", str_c(path),
		mesh_sync_lease_response, sync);
	http_client_request_submit(sync->lease_req);
}

void mesh_sync_deinit(void)
{
	while (mesh_syncs != NULL)
		mesh_sync_finish(mesh_syncs, DOVEADM_EX_UNKNOWN, "Shutting down");
}
