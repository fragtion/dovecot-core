/* Copyright (c) Dovecot authors, see top-level COPYING file */

#include "mesh-common.h"
#include "ioloop.h"
#include "time-util.h"
#include "doveadm-protocol.h"
#include "master-service.h"
#include "master-service-settings.h"
#include "auth-master.h"

/* how often to check whether a periodic sync is due */
#define MESH_PERIODIC_CHECK_MSECS (30*1000)
/* backoff after failures: 1s, 2s, 4s, .. up to this */
#define MESH_MAX_BACKOFF_SECS 60
/* after this many consecutive failures, retry without the saved state */
#define MESH_STATE_RESET_FAILURES 3

HASH_TABLE_TYPE(mesh_user) mesh_users;

/* users that have at least one pending push */
static HASH_TABLE(char *, struct mesh_user *) mesh_pending_users;
static struct timeout *mesh_queue_to, *mesh_periodic_to, *mesh_startup_to;
static unsigned int mesh_running_count;
static time_t mesh_last_incremental, mesh_last_full;

struct mesh_user *mesh_user_lookup(const char *username, bool create)
{
	struct mesh_user *user;

	user = hash_table_lookup(mesh_users, username);
	if (user != NULL || !create)
		return user;

	user = i_new(struct mesh_user, 1);
	user->username = i_strdup(username);
	user->peers = i_new(struct mesh_user_peer,
			    I_MAX(array_count(&mesh_peers), 1));
	user->anchor = mesh_anchor_name(username);
	hash_table_insert(mesh_users, user->username, user);
	return user;
}

static void mesh_user_free(struct mesh_user *user)
{
	unsigned int i, count = array_count(&mesh_peers);

	for (i = 0; i < count; i++) {
		i_free(user->peers[i].dsync_state);
		i_free(user->peers[i].last_error);
	}
	i_free(user->peers);
	i_free(user->username);
	i_free(user);
}

static void mesh_queue_schedule_soon(void);

void mesh_queue_push(struct mesh_user *user, struct mesh_peer *peer,
		     enum mesh_priority priority, bool full)
{
	struct mesh_user_peer *up = &user->peers[peer->idx];
	struct timeval due = ioloop_timeval;

	/* wait a moment so that a burst of changes becomes one sync */
	if (priority != MESH_PRIORITY_LOW)
		timeval_add_msecs(&due, mesh_set->mesh_coalesce_delay);
	if (up->pending == MESH_PRIORITY_NONE) {
		up->pending_since = ioloop_time;
		up->due = due;
	} else if (up->failures == 0 && timeval_cmp(&due, &up->due) < 0) {
		up->due = due;
	}
	if (up->pending < priority)
		up->pending = priority;
	if (full)
		up->pending_full = TRUE;
	hash_table_update(mesh_pending_users, user->username, user);
	mesh_state_changed();
	mesh_queue_schedule_soon();
}

void mesh_queue_push_all(struct mesh_user *user, enum mesh_priority priority,
			 bool full)
{
	struct mesh_peer *peer, *anchor;

	if (mesh_set->parsed_topology == MESH_TOPOLOGY_ANCHOR) {
		anchor = mesh_anchor_peer(user->username);
		if (anchor != NULL) {
			/* we're a spoke: the anchor relays to the others */
			mesh_queue_push(user, anchor, priority, full);
			return;
		}
	}
	array_foreach_elem(&mesh_peers, peer)
		mesh_queue_push(user, peer, priority, full);
}

void mesh_queue_notify(const char *username, enum mesh_priority priority,
		       bool dsync)
{
	struct mesh_user *user = mesh_user_lookup(username, TRUE);
	struct mesh_peer *peer;

	e_debug(mesh_event,
		"Change notification: user=%s priority=%d dsync=%d "
		"running_peer=%s lease_peer=%s", username, priority, dsync,
		user->running_peer == NULL ? "-" : user->running_peer->name,
		!mesh_user_is_leased(user) ? "-" : user->lease_peer->name);
	user->last_change = ioloop_time;
	if (!dsync) {
		mesh_queue_push_all(user, priority, FALSE);
		return;
	}

	/* The change was imported by dsync from another node. With a full
	   mesh every node pushes its own changes to everyone, and with the
	   anchor topology only the anchor relays. */
	if (mesh_set->parsed_topology == MESH_TOPOLOGY_FULL ||
	    mesh_anchor_peer(username) != NULL)
		return;

	/* Only one session per user runs at a time (our own, or a peer's
	   push under our lease), so a dsync change seen now came from that
	   session's peer, which doesn't need it back. */
	array_foreach_elem(&mesh_peers, peer) {
		if (peer != user->running_peer &&
		    !(peer == user->lease_peer && mesh_user_is_leased(user)))
			mesh_queue_push(user, peer, MESH_PRIORITY_NORMAL, FALSE);
	}
}

/* With the anchor topology, move pushes queued for a peer that's no
   longer a direct partner of the user (the anchor moved) to the anchor.
   Otherwise e.g. pushes queued at startup, before any peer was known to
   be up, would sync spokes with each other. */
static void mesh_queue_redirect_pending(struct mesh_user *user)
{
	struct mesh_peer *peer, *anchor = mesh_anchor_peer(user->username);
	struct mesh_user_peer *up;
	enum mesh_priority priority = MESH_PRIORITY_NONE;
	bool full = FALSE;

	array_foreach_elem(&mesh_peers, peer) {
		up = &user->peers[peer->idx];
		if (up->pending == MESH_PRIORITY_NONE ||
		    mesh_user_peer_is_direct(user, peer))
			continue;
		if (priority < up->pending)
			priority = up->pending;
		full = full || up->pending_full;
		up->pending = MESH_PRIORITY_NONE;
		up->pending_full = FALSE;
		up->failures = 0;
	}
	if (priority != MESH_PRIORITY_NONE && anchor != NULL)
		mesh_queue_push(user, anchor, priority, full);
}

static bool mesh_user_has_pending(const struct mesh_user *user);

void mesh_queue_peers_changed(void)
{
	struct hash_iterate_context *iter;
	struct mesh_user *user;
	ARRAY(struct mesh_user *) redirect;
	const char *anchor;
	char *username;

	if (mesh_set->parsed_topology == MESH_TOPOLOGY_ANCHOR) {
		/* Users whose anchor moved are pushed to the new anchor, or
		   from it to everyone if this node is the new anchor. */
		iter = hash_table_iterate_init(mesh_users);
		while (hash_table_iterate(iter, mesh_users, &username, &user)) {
			anchor = mesh_anchor_name(username);
			if (strcmp(anchor, user->anchor) != 0) {
				user->anchor = anchor;
				mesh_queue_push_all(user, MESH_PRIORITY_NORMAL,
						    FALSE);
			}
		}
		hash_table_iterate_deinit(&iter);

		i_array_init(&redirect, 16);
		iter = hash_table_iterate_init(mesh_pending_users);
		while (hash_table_iterate(iter, mesh_pending_users,
					  &username, &user))
			array_push_back(&redirect, &user);
		hash_table_iterate_deinit(&iter);
		array_foreach_elem(&redirect, user) {
			mesh_queue_redirect_pending(user);
			/* A user whose sync is running stays, even without
			   pending syncs: mesh_queue_sync_finished() removes
			   it. */
			if (!mesh_user_has_pending(user) &&
			    user->running_peer == NULL) {
				hash_table_remove(mesh_pending_users,
						  user->username);
			}
		}
		array_free(&redirect);
	}
	mesh_queue_schedule_soon();
}

bool mesh_user_is_leased(const struct mesh_user *user)
{
	return user->lease_peer != NULL && user->lease_expires > ioloop_time;
}

void mesh_queue_expedite(struct mesh_user *user)
{
	struct mesh_peer *peer;
	struct mesh_user_peer *up;

	array_foreach_elem(&mesh_peers, peer) {
		up = &user->peers[peer->idx];
		if (up->pending != MESH_PRIORITY_NONE && up->failures == 0 &&
		    timeval_cmp(&up->due, &ioloop_timeval) > 0)
			up->due = ioloop_timeval;
	}
	mesh_queue_schedule_soon();
}

bool mesh_user_peer_is_direct(const struct mesh_user *user,
			      const struct mesh_peer *peer)
{
	const struct mesh_peer *anchor;

	if (mesh_set->parsed_topology != MESH_TOPOLOGY_ANCHOR)
		return TRUE;
	anchor = mesh_anchor_peer(user->username);
	return anchor == NULL || anchor == peer;
}

const char *mesh_user_peer_status(const struct mesh_user *user,
				  const struct mesh_peer *peer)
{
	const struct mesh_user_peer *up = &user->peers[peer->idx];

	if (user->running_peer == peer)
		return "syncing";
	if (up->pending != MESH_PRIORITY_NONE)
		return up->failures > 0 ? "error" : "pending";
	if (!mesh_user_peer_is_direct(user, peer))
		return "relayed";
	if (up->last_ok == 0)
		return "never";
	return "ok";
}

void mesh_user_mark_session(struct mesh_user *user, struct mesh_peer *peer)
{
	struct mesh_peer *p;

	array_foreach_elem(&mesh_peers, p)
		user->peers[p->idx].exclusive = p == peer;
	mesh_state_changed();
}

void mesh_user_peer_set_state(struct mesh_user_peer *up, const char *state)
{
	i_free(up->dsync_state);
	if (state != NULL && state[0] != '\0')
		up->dsync_state = i_strdup(state);
}

static unsigned int mesh_backoff_msecs(unsigned int failures)
{
	unsigned int secs = 1;

	while (--failures > 0 && secs < MESH_MAX_BACKOFF_SECS)
		secs *= 2;
	return I_MIN(secs, MESH_MAX_BACKOFF_SECS) * 1000;
}

static bool mesh_user_has_pending(const struct mesh_user *user)
{
	unsigned int i, count = array_count(&mesh_peers);

	for (i = 0; i < count; i++) {
		if (user->peers[i].pending != MESH_PRIORITY_NONE)
			return TRUE;
	}
	return FALSE;
}

void mesh_queue_sync_finished(struct mesh_user *user, struct mesh_peer *peer,
			      int exit_code, const char *error,
			      const char *new_state, bool full,
			      unsigned int msecs)
{
	struct mesh_user_peer *up = &user->peers[peer->idx];

	i_assert(user->running_peer == peer);
	user->running_peer = NULL;
	i_assert(mesh_running_count > 0);
	mesh_running_count--;
	i_assert(peer->running_syncs > 0);
	peer->running_syncs--;

	if (exit_code == MESH_SYNC_EXIT_BUSY) {
		/* the peer is busy with this user - retry soon, with some
		   jitter so that competing nodes don't keep colliding */
		mesh_queue_push(user, peer, MESH_PRIORITY_NORMAL, full);
		up->due = ioloop_timeval;
		timeval_add_msecs(&up->due, 100 + i_rand_limit(400));
		mesh_queue_schedule_soon();
		return;
	}

	up->last_attempt = ioloop_time;
	up->last_msecs = msecs;
	if (exit_code == 0 || exit_code == DOVEADM_EX_CHANGED) {
		/* empty if dsync wants a full resync */
		mesh_user_peer_set_state(up, new_state);
		up->last_ok = ioloop_time;
		up->ok_count++;
		up->failures = 0;
		i_free(up->last_error);
		if (full)
			up->last_full = ioloop_time;
		peer->syncs_ok++;
		peer->last_sync_ok = ioloop_time;
		if (exit_code != DOVEADM_EX_CHANGED)
			up->changed_count = 0;
		else if (++up->changed_count >= 2) {
			/* The state itself may be what keeps failing (e.g. it
			   refers to modseqs no longer in the transaction log),
			   and dsync then just prints it back unchanged. Start
			   over without it. */
			mesh_user_peer_set_state(up, NULL);
			up->changed_count = 0;
			mesh_queue_push(user, peer, MESH_PRIORITY_NORMAL,
					FALSE);
		} else {
			/* mails changed while syncing - sync again */
			mesh_queue_push(user, peer, MESH_PRIORITY_NORMAL,
					FALSE);
		}
	} else {
		/* dsync still prints the state of the mailboxes it finished,
		   which describes the pair better than the old state. (It's
		   empty if the session didn't get that far.) */
		if (new_state != NULL && new_state[0] != '\0')
			mesh_user_peer_set_state(up, new_state);
		up->failures++;
		up->fail_count++;
		i_free(up->last_error);
		up->last_error = i_strdup(error);
		peer->syncs_failed++;
		peer->last_sync_failed = ioloop_time;
		i_warning("Syncing %s with %s failed (attempt %u): %s",
			  user->username, peer->name, up->failures, error);
		if (up->failures >= MESH_STATE_RESET_FAILURES) {
			/* the saved state may be what's failing */
			mesh_user_peer_set_state(up, NULL);
		}
		mesh_queue_push(user, peer, MESH_PRIORITY_NORMAL, full);
		up->due = ioloop_timeval;
		timeval_add_msecs(&up->due, mesh_backoff_msecs(up->failures));
	}
	if (!mesh_user_has_pending(user))
		hash_table_remove(mesh_pending_users, user->username);
	mesh_state_changed();
	mesh_queue_schedule_soon();
}

static void mesh_sync_run(struct mesh_user *user, struct mesh_peer *peer)
{
	struct mesh_user_peer *up = &user->peers[peer->idx];
	bool full = up->pending_full;

	/* Clear the pending flag now, so that changes made while the sync
	   runs mark it pending again. */
	up->pending = MESH_PRIORITY_NONE;
	up->pending_full = FALSE;
	user->running_peer = peer;
	peer->running_syncs++;
	mesh_running_count++;
	mesh_sync_start(user, peer, full);
}

/* Returns TRUE if a is a better candidate than b. */
static bool mesh_candidate_better(const struct mesh_user_peer *a,
				  const struct mesh_user_peer *b)
{
	if (b == NULL)
		return TRUE;
	if (a->pending != b->pending)
		return a->pending > b->pending;
	return timeval_cmp(&a->due, &b->due) < 0;
}

static void mesh_queue_timeout(void *context ATTR_UNUSED)
{
	timeout_remove(&mesh_queue_to);
	mesh_queue_schedule();
}

static void mesh_queue_schedule_soon(void)
{
	if (mesh_queue_to == NULL)
		mesh_queue_to = timeout_add_short(0, mesh_queue_timeout, NULL);
}

void mesh_queue_schedule(void)
{
	struct hash_iterate_context *iter;
	struct mesh_user *user, *best_user;
	struct mesh_peer *peer, *best_peer;
	struct mesh_user_peer *up, *best;
	struct timeval next_due;
	bool have_next_due;
	char *username;

	for (;;) {
		best = NULL; best_user = NULL; best_peer = NULL;
		have_next_due = FALSE;
		iter = hash_table_iterate_init(mesh_pending_users);
		while (hash_table_iterate(iter, mesh_pending_users,
					  &username, &user)) {
			if (user->running_peer != NULL)
				continue;
			if (mesh_user_is_leased(user)) {
				/* a peer is pushing it - wait for the release
				   (or the lease to expire) */
				struct timeval lease_tv = {
					.tv_sec = user->lease_expires,
				};
				if (!have_next_due ||
				    timeval_cmp(&lease_tv, &next_due) < 0)
					next_due = lease_tv;
				have_next_due = TRUE;
				continue;
			}
			array_foreach_elem(&mesh_peers, peer) {
				up = &user->peers[peer->idx];
				if (up->pending == MESH_PRIORITY_NONE ||
				    !mesh_peer_usable(peer))
					continue;
				if (timeval_cmp(&up->due, &ioloop_timeval) > 0) {
					if (!have_next_due ||
					    timeval_cmp(&up->due, &next_due) < 0)
						next_due = up->due;
					have_next_due = TRUE;
					continue;
				}
				if (mesh_candidate_better(up, best)) {
					best = up;
					best_user = user;
					best_peer = peer;
				}
			}
		}
		hash_table_iterate_deinit(&iter);

		if (best == NULL ||
		    mesh_running_count >= mesh_set->mesh_max_parallel_syncs)
			break;
		mesh_sync_run(best_user, best_peer);
	}

	timeout_remove(&mesh_queue_to);
	if (best == NULL && have_next_due) {
		long long msecs = timeval_diff_msecs(&next_due, &ioloop_timeval);
		mesh_queue_to = timeout_add(I_MAX(msecs, 1) + 1,
					    mesh_queue_timeout, NULL);
	}
	/* if we're limited by mesh_max_parallel_syncs, a finishing sync
	   reschedules */
}

/* Learn all users from the userdb. Otherwise only users who changed
   something, or that a peer asked about, are known, and users who stay
   idle would never be synced to a new or emptied node. */
static void mesh_queue_discover_users(void)
{
	static bool warned = FALSE;
	struct auth_master_connection *conn;
	struct auth_master_user_list_ctx *ctx;
	const char *socket_path, *username;
	unsigned int count = 0, new_count = 0;

	if (!mesh_set->mesh_list_users)
		return;
	socket_path = t_strconcat(master_service_get_service_settings(
		master_service)->base_dir, "/auth-userdb", NULL);
	conn = auth_master_init(socket_path, 0);
	ctx = auth_master_user_list_init(conn, "*", NULL);
	while ((username = auth_master_user_list_next(ctx)) != NULL) {
		count++;
		if (mesh_user_lookup(username, FALSE) == NULL) {
			(void)mesh_user_lookup(username, TRUE);
			new_count++;
		}
	}
	if (auth_master_user_list_deinit(&ctx) < 0) {
		/* e.g. a userdb that can't list its users */
		if (!warned) {
			i_warning("Couldn't list the users from the userdb: "
				  "only users that change something are "
				  "synced (set mesh_list_users=no if the "
				  "userdb can't list users)");
			warned = TRUE;
		}
	} else {
		e_debug(mesh_event, "Listed %u users from the userdb, "
			"%u of them new", count, new_count);
	}
	auth_master_deinit(&conn);
}

static void mesh_queue_push_everyone(bool full)
{
	struct hash_iterate_context *iter;
	struct mesh_user *user;
	char *username;

	iter = hash_table_iterate_init(mesh_users);
	while (hash_table_iterate(iter, mesh_users, &username, &user))
		mesh_queue_push_all(user, MESH_PRIORITY_LOW, full);
	hash_table_iterate_deinit(&iter);
}

static void mesh_periodic_timeout(void *context ATTR_UNUSED)
{
	if (mesh_set->mesh_full_sync_interval > 0 &&
	    ioloop_time - mesh_last_full >=
	    (time_t)mesh_set->mesh_full_sync_interval) {
		mesh_last_full = ioloop_time;
		mesh_last_incremental = ioloop_time;
		mesh_queue_discover_users();
		mesh_queue_push_everyone(TRUE);
	} else if (mesh_set->mesh_incremental_sync_interval > 0 &&
		   ioloop_time - mesh_last_incremental >=
		   (time_t)mesh_set->mesh_incremental_sync_interval) {
		/* catches changes whose notification got lost */
		mesh_last_incremental = ioloop_time;
		mesh_queue_push_everyone(FALSE);
	}
}

unsigned int mesh_queue_pending_count(void)
{
	struct hash_iterate_context *iter;
	struct mesh_user *user;
	unsigned int i, count = array_count(&mesh_peers), pending = 0;
	char *username;

	iter = hash_table_iterate_init(mesh_pending_users);
	while (hash_table_iterate(iter, mesh_pending_users, &username, &user)) {
		for (i = 0; i < count; i++) {
			if (user->peers[i].pending != MESH_PRIORITY_NONE)
				pending++;
		}
	}
	hash_table_iterate_deinit(&iter);
	return pending;
}

unsigned int mesh_queue_running_count(void)
{
	return mesh_running_count;
}

void mesh_queue_init(void)
{
	hash_table_create(&mesh_users, default_pool, 0, str_hash, strcmp);
	hash_table_create(&mesh_pending_users, default_pool, 0,
			  str_hash, strcmp);
	mesh_last_incremental = mesh_last_full = ioloop_time;
	mesh_periodic_to = timeout_add(MESH_PERIODIC_CHECK_MSECS,
				       mesh_periodic_timeout, NULL);
}

static void mesh_queue_startup_timeout(void *context ATTR_UNUSED)
{
	timeout_remove(&mesh_startup_to);
	mesh_queue_discover_users();
	mesh_queue_push_everyone(FALSE);
}

void mesh_queue_startup_sync(void)
{
	/* Push everyone once at startup, in case changes were made while
	   this service wasn't running. Unchanged users cost only a quick
	   state comparison. Wait until the peers have had time to answer
	   heartbeats, so that the anchors are known. */
	mesh_startup_to = timeout_add(mesh_set->mesh_peer_down_timeout,
				      mesh_queue_startup_timeout, NULL);
}

void mesh_queue_deinit(void)
{
	struct hash_iterate_context *iter;
	struct mesh_user *user;
	char *username;

	timeout_remove(&mesh_queue_to);
	timeout_remove(&mesh_periodic_to);
	timeout_remove(&mesh_startup_to);
	hash_table_destroy(&mesh_pending_users);
	iter = hash_table_iterate_init(mesh_users);
	while (hash_table_iterate(iter, mesh_users, &username, &user))
		mesh_user_free(user);
	hash_table_iterate_deinit(&iter);
	hash_table_destroy(&mesh_users);
}
