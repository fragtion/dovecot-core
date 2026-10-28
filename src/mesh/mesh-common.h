#ifndef MESH_COMMON_H
#define MESH_COMMON_H

#include "lib.h"
#include "array.h"
#include "hash.h"
#include "mesh-settings.h"

struct http_client_request;
struct http_response;
struct master_service_connection;

/* mesh_queue_sync_finished() exit code: the peer is syncing the user with
   someone else right now. Not an error; retried shortly. */
#define MESH_SYNC_EXIT_BUSY -1000
/* mesh_queue_sync_finished() exit code: the peer doesn't have the user
   (its userdb doesn't list it). Not an error; the user is synced with the
   nodes that have it. */
#define MESH_SYNC_EXIT_NOUSER -1001

/* Why a user needs to be pushed to a peer. Higher wins. */
enum mesh_priority {
	MESH_PRIORITY_NONE = 0,
	/* periodic catch-up sync */
	MESH_PRIORITY_LOW,
	/* flag changes, expunges, mailbox changes, relays */
	MESH_PRIORITY_NORMAL,
	/* new mail */
	MESH_PRIORITY_HIGH,
};

struct mesh_peer {
	unsigned int idx;
	const char *name;
	const struct mesh_peer_settings *set;

	bool up;
	bool paused;
	/* last successful heartbeat */
	struct timeval last_seen;
	struct timeval up_since;
	unsigned int rtt_msecs;
	long long clock_offset_msecs;
	char *last_error;

	struct http_client_request *hb_req;
	struct timeval hb_sent;

	/* reported by the peer in its last heartbeat */
	char *remote_version;
	unsigned int remote_pending, remote_running, remote_users;
	char *remote_peers_up;

	unsigned int running_syncs;
	uint64_t syncs_ok, syncs_failed;
	time_t last_sync_ok, last_sync_failed;
};
ARRAY_DEFINE_TYPE(mesh_peer, struct mesh_peer *);

/* What this node knows about syncing one user with one peer. */
struct mesh_user_peer {
	/* push needed (NONE = nothing to do) */
	enum mesh_priority pending;
	bool pending_full;
	/* don't start the push before this */
	struct timeval due;
	/* oldest change that hasn't been pushed yet */
	time_t pending_since;
	/* consecutive failures, for backoff */
	unsigned int failures;
	/* consecutive "changes during sync" results */
	unsigned int changed_count;

	/* persistent: */
	/* The pair's dsync state. Both nodes keep a copy, and every session
	   bumps state_gen and gives the new state to the other node when
	   releasing its lease, so the copies normally match. */
	char *dsync_state;
	unsigned int state_gen;
	/* No session for the user with any other peer since the last one
	   with this peer. If neither node of a pair has had other sessions,
	   mails can't have reached both of them some other way, which the
	   state can't describe (see mesh_sync_lease_response()). */
	bool exclusive;
	time_t last_ok, last_attempt, last_full;
	char *last_error;
	unsigned int ok_count, fail_count;
	unsigned int last_msecs;
};

struct mesh_user {
	char *username;
	/* indexed by mesh_peer.idx */
	struct mesh_user_peer *peers;

	/* a sync for this user is running (only one at a time) */
	struct mesh_peer *running_peer;
	/* this peer is running a sync for the user with us */
	struct mesh_peer *lease_peer;
	time_t lease_expires;
	struct timeval lease_start;
	/* node the user was anchored to when last checked */
	const char *anchor;
	time_t last_change;

	/* last verification: ok, mismatch or error (NULL = never) */
	time_t verify_time;
	const char *verify_result;
	/* consecutive mismatches */
	unsigned int verify_mismatches;
};
HASH_TABLE_DEFINE_TYPE(mesh_user, char *, struct mesh_user *);

extern struct event *mesh_event;
extern const struct mesh_settings *mesh_set;
extern const char *mesh_node_name;
extern ARRAY_TYPE(mesh_peer) mesh_peers;
extern HASH_TABLE_TYPE(mesh_user) mesh_users;
extern time_t mesh_start_time;

/* mesh-peer.c */
void mesh_peers_init(void);
void mesh_peers_deinit(void);
struct mesh_peer *mesh_peer_find(const char *name);
/* GET a path from the peer's mesh service and read the whole body. body is
   NULL on failure. */
typedef void mesh_peer_http_get_callback_t(const buffer_t *body,
					   const char *error, void *context);
struct mesh_peer_http_get *
mesh_peer_http_get(struct mesh_peer *peer, const char *path,
		   unsigned int timeout_msecs,
		   mesh_peer_http_get_callback_t *callback, void *context);
void mesh_peer_http_get_abort(struct mesh_peer_http_get **get);
bool mesh_peer_usable(const struct mesh_peer *peer);
unsigned int mesh_peers_up_count(void);
/* Returns the node name of the user's anchor (may be mesh_node_name). */
const char *mesh_anchor_name(const char *username);
/* Returns the anchor peer, or NULL if this node is the anchor. */
struct mesh_peer *mesh_anchor_peer(const char *username);
/* Send an authenticated HTTP request to the peer's mesh service. */
struct http_client_request *
mesh_peer_http_request(struct mesh_peer *peer, const char *method,
		       const char *path,
		       void (*callback)(const struct http_response *resp,
					void *context),
		       void *context);

/* mesh-queue.c */
void mesh_queue_init(void);
void mesh_queue_deinit(void);
struct mesh_user *mesh_user_lookup(const char *username, bool create);
/* A mail process reported a change. dsync=TRUE if dsync made it. */
void mesh_queue_notify(const char *username, enum mesh_priority priority,
		       bool dsync);
/* Push the user to the peers that need it according to the topology,
   as if the user had changed locally. */
void mesh_queue_push_all(struct mesh_user *user, enum mesh_priority priority,
			 bool full);
void mesh_queue_push(struct mesh_user *user, struct mesh_peer *peer,
		     enum mesh_priority priority, bool full);
/* Peers went up/down or got paused/resumed. */
void mesh_queue_peers_changed(void);
void mesh_queue_sync_finished(struct mesh_user *user, struct mesh_peer *peer,
			      int exit_code, const char *error,
			      const char *new_state, bool full,
			      unsigned int msecs);
void mesh_queue_schedule(void);
/* Start the user's pending pushes now instead of waiting for more
   changes to coalesce: a delivery is waiting for them. */
void mesh_queue_expedite(struct mesh_user *user);
/* TRUE if another peer currently holds the user's lease. */
bool mesh_user_is_leased(const struct mesh_user *user);
/* FALSE if the user's changes reach the peer through the anchor instead
   of syncs between this node and the peer. */
bool mesh_user_peer_is_direct(const struct mesh_user *user,
			      const struct mesh_peer *peer);
/* syncing, absent (the peer doesn't have the user), pending, error,
   relayed, never or ok */
const char *mesh_user_peer_status(const struct mesh_user *user,
				  const struct mesh_peer *peer);
/* A session for the user with the peer is starting. */
void mesh_user_mark_session(struct mesh_user *user, struct mesh_peer *peer);
/* Replace the pair's dsync state (NULL/"" = none). */
void mesh_user_peer_set_state(struct mesh_user_peer *up, const char *state);
/* Push every known user once, e.g. after startup. */
void mesh_queue_startup_sync(void);
unsigned int mesh_queue_pending_count(void);
unsigned int mesh_queue_running_count(void);

/* mesh-userlist.c */
void mesh_userlist_init(void);
void mesh_userlist_deinit(void);
/* Read this node's users from the userdb. push_new: push the users that
   are new here to the nodes that have them. */
void mesh_userlist_refresh(bool push_new);
/* Refresh the list every mesh_userlist_refresh_interval from now on. */
void mesh_userlist_start_refreshing(void);
/* Hash of this node's user list, "*" if it isn't known (then the node is
   assumed to have every user). */
const char *mesh_userlist_hash(void);
/* The hash line, then one user per line. */
void mesh_userlist_export(string_t *dest);
/* Whether this node's userdb lists the user (TRUE if not known). */
bool mesh_local_has_user(const char *username);
/* Whether the peer's userdb lists the user (TRUE if not known). */
bool mesh_peer_has_user(const struct mesh_peer *peer, const char *username);
/* The peer reported this user list hash: fetch its list if it changed. */
void mesh_peer_users_hash_seen(struct mesh_peer *peer, const char *hash);
/* The peer refused to sync the user, because it doesn't have it. */
void mesh_peer_user_missing(struct mesh_peer *peer, const char *username);

/* mesh-sync.c */
struct doveadm_client;
void mesh_sync_start(struct mesh_user *user, struct mesh_peer *peer,
		     bool full);
void mesh_sync_deinit(void);
/* Append src %-encoded for a URL query parameter. */
void mesh_url_encode(string_t *dest, const char *src);
/* Connect to the local doveadm server. */
int mesh_doveadm_client_create(struct doveadm_client **conn_r,
			       const char **error_r);

/* mesh-verify.c */
struct mesh_fingerprint;
struct mesh_fingerprint_request;
struct mesh_verify;

struct mesh_verify_result_node {
	const char *name;
	/* NULL if the node's fingerprint was received */
	const char *error;
};

struct mesh_verify_mailbox_node {
	bool present;
	unsigned int messages;
	const char *hash;
};

struct mesh_verify_mailbox {
	const char *guid, *name;
	/* TRUE if all nodes that answered have the same mails */
	bool match;
	/* indexed like mesh_verify_result.nodes */
	struct mesh_verify_mailbox_node *nodes;
};

struct mesh_verify_result {
	const char *username;
	/* ok, mismatch, or error (no differences, but some node failed) */
	const char *result;
	/* this node first, then the peers that were up */
	ARRAY(struct mesh_verify_result_node) nodes;
	/* differing mailboxes first */
	ARRAY(struct mesh_verify_mailbox *) mailboxes;
};

/* fp is NULL on failure. The callback owns fp. */
typedef void mesh_fingerprint_callback_t(struct mesh_fingerprint *fp,
					 const char *error, void *context);
/* result is valid only during the callback */
typedef void mesh_verify_callback_t(const struct mesh_verify_result *result,
				    void *context);

/* Compute the user's fingerprint on this node. */
struct mesh_fingerprint_request *
mesh_fingerprint_local(const char *username,
		       mesh_fingerprint_callback_t *callback, void *context);
void mesh_fingerprint_request_abort(struct mesh_fingerprint_request **req);
void mesh_fingerprint_export(const struct mesh_fingerprint *fp,
			     string_t *dest);
void mesh_fingerprint_free(struct mesh_fingerprint **fp);

/* Compare the user's fingerprints on all nodes that are up. */
struct mesh_verify *
mesh_verify_start(const char *username, mesh_verify_callback_t *callback,
		  void *context);
/* Don't call the callback anymore; the verification finishes on its own. */
void mesh_verify_detach(struct mesh_verify **verify);
void mesh_verify_result_rows(const struct mesh_verify_result *result,
			     string_t *dest);
void mesh_verify_result_json(const struct mesh_verify_result *result,
			     string_t *dest);
void mesh_verify_init(void);
void mesh_verify_deinit(void);

/* mesh-state.c */
void mesh_state_init(void);
void mesh_state_deinit(void);
void mesh_state_changed(void);

/* mesh-conn.c */
void mesh_conn_init(void);
void mesh_conn_deinit(void);
void mesh_conn_create(int fd, bool admin);
/* A session for the user that started at start_time finished
   successfully, so everything committed before that is on another node
   now. Answers the deliveries waiting for that. */
void mesh_conn_user_replicated(const struct mesh_user *user,
			       const struct timeval *start_time);

/* mesh-http.c */
void mesh_http_init(void);
void mesh_http_deinit(void);
void mesh_http_conn_create(struct master_service_connection *conn);

/* mesh-status.c */
void mesh_status_init(void);
void mesh_status_deinit(void);
/* Remember a finished session for the activity list. */
void mesh_status_add_sync(const struct mesh_user *user,
			  const struct mesh_peer *peer, const char *result,
			  bool full, bool stateful, unsigned int msecs,
			  const char *error);
void mesh_status_json(string_t *dest);
void mesh_activity_json(string_t *dest, unsigned int limit);
struct mesh_users_filter {
	/* only this user */
	const char *user;
	/* user name contains this */
	const char *query;
	/* only users with a peer that's pending, syncing or failing, or
	   never synced with */
	bool problems;
	/* 0 = unlimited */
	unsigned int limit;
};
void mesh_users_json(string_t *dest, const struct mesh_users_filter *filter);
void mesh_heartbeat_headers(ARRAY_TYPE(const_string) *headers);

#endif
