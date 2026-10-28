/* Copyright (c) Dovecot authors, see top-level COPYING file */

/* Which nodes have which users. Each node's userdb may list a different
   set of users; a user is replicated only between the nodes that have it.
   Every node publishes a hash of its user list in its heartbeat replies,
   and the other nodes fetch the list when the hash changes. A node whose
   userdb can't list its users (or with mesh_list_users=no) publishes "*"
   and is assumed to have every user. */

#include "mesh-common.h"
#include "ioloop.h"
#include "array.h"
#include "hash.h"
#include "hex-binary.h"
#include "sha1.h"
#include "str.h"
#include "strescape.h"
#include "master-service.h"
#include "master-service-settings.h"
#include "auth-master.h"

#define MESH_USERLIST_ALL "*"
#define MESH_USERLIST_FETCH_TIMEOUT_MSECS (60*1000)

/* lowercased names (usernames are matched case-insensitively here, since
   the userdb lists them as written while lookups usually lowercase them) */
HASH_TABLE_DEFINE_TYPE(mesh_name_set, char *, char *);

static HASH_TABLE_TYPE(mesh_name_set) mesh_local_users;
static struct timeout *mesh_userlist_to;
/* FALSE until the userdb was listed successfully */
static bool mesh_local_listed;
static char *mesh_local_hash;

struct mesh_peer_users {
	/* the hash of the list we have; NULL = none fetched yet */
	char *hash;
	HASH_TABLE_TYPE(mesh_name_set) names;
	/* users the peer refused to sync, although its list (or the lack of
	   one) said it has them; cleared when a new list arrives */
	HASH_TABLE_TYPE(mesh_name_set) missing;
	/* the hash the peer reported last, and a fetch for it */
	char *wanted_hash;
	struct mesh_peer_http_get *get;
};

static struct mesh_peer_users **mesh_peer_users;
static unsigned int mesh_peer_users_count;

static const char *mesh_name_key(const char *username)
{
	return t_str_lcase(username);
}

static void mesh_name_set_init(HASH_TABLE_TYPE(mesh_name_set) *set)
{
	hash_table_create(set, default_pool, 0, str_hash, strcmp);
}

static void mesh_name_set_deinit(HASH_TABLE_TYPE(mesh_name_set) *set)
{
	struct hash_iterate_context *iter;
	char *key, *value;

	if (!hash_table_is_created(*set))
		return;
	iter = hash_table_iterate_init(*set);
	while (hash_table_iterate(iter, *set, &key, &value))
		i_free(key);
	hash_table_iterate_deinit(&iter);
	hash_table_destroy(set);
}

static bool
mesh_name_set_add(HASH_TABLE_TYPE(mesh_name_set) set, const char *username)
{
	const char *key = mesh_name_key(username);
	char *dup;

	if (hash_table_lookup(set, key) != NULL)
		return FALSE;
	dup = i_strdup(key);
	hash_table_insert(set, dup, dup);
	return TRUE;
}

static bool
mesh_name_set_contains(HASH_TABLE_TYPE(mesh_name_set) set,
		       const char *username)
{
	return hash_table_lookup(set, mesh_name_key(username)) != NULL;
}

static int mesh_name_cmp(char *const *a, char *const *b)
{
	return strcmp(*a, *b);
}

/* SHA1 over the sorted names: the same list gives the same hash on every
   node. */
static char *mesh_name_set_hash(HASH_TABLE_TYPE(mesh_name_set) set)
{
	struct hash_iterate_context *iter;
	struct sha1_ctxt ctx;
	unsigned char digest[SHA1_RESULTLEN];
	ARRAY(char *) names;
	char *key, *value;

	t_array_init(&names, hash_table_count(set) + 1);
	iter = hash_table_iterate_init(set);
	while (hash_table_iterate(iter, set, &key, &value))
		array_push_back(&names, &key);
	hash_table_iterate_deinit(&iter);
	array_sort(&names, mesh_name_cmp);

	sha1_init(&ctx);
	array_foreach_elem(&names, key) {
		sha1_loop(&ctx, key, strlen(key));
		sha1_loop(&ctx, "\n", 1);
	}
	sha1_result(&ctx, digest);
	return i_strdup(binary_to_hex(digest, sizeof(digest)));
}

/*
 * This node's users
 */

bool mesh_local_has_user(const char *username)
{
	return !mesh_local_listed ||
		mesh_name_set_contains(mesh_local_users, username);
}

const char *mesh_userlist_hash(void)
{
	return mesh_local_listed ? mesh_local_hash : MESH_USERLIST_ALL;
}

void mesh_userlist_export(string_t *dest)
{
	struct hash_iterate_context *iter;
	char *key, *value;

	/* first line: the hash, then one name per line */
	str_append(dest, mesh_userlist_hash());
	str_append_c(dest, '\n');
	if (!mesh_local_listed)
		return;
	iter = hash_table_iterate_init(mesh_local_users);
	while (hash_table_iterate(iter, mesh_local_users, &key, &value)) {
		str_append_tabescaped(dest, key);
		str_append_c(dest, '\n');
	}
	hash_table_iterate_deinit(&iter);
}

static void mesh_userlist_refresh_real(bool push_new)
{
	static bool warned = FALSE;
	struct auth_master_connection *conn;
	struct auth_master_user_list_ctx *ctx;
	HASH_TABLE_TYPE(mesh_name_set) names;
	ARRAY_TYPE(const_string) added;
	const char *socket_path, *username;
	unsigned int count = 0;
	struct mesh_user *user;
	char *hash;

	if (!mesh_set->mesh_list_users)
		return;
	mesh_name_set_init(&names);
	t_array_init(&added, 16);
	socket_path = t_strconcat(master_service_get_service_settings(
		master_service)->base_dir, "/auth-userdb", NULL);
	conn = auth_master_init(socket_path, 0);
	ctx = auth_master_user_list_init(conn, "*", NULL);
	while ((username = auth_master_user_list_next(ctx)) != NULL) {
		count++;
		if (mesh_name_set_add(names, username) &&
		    (!mesh_local_listed ||
		     !mesh_name_set_contains(mesh_local_users, username))) {
			const char *name = t_strdup(username);
			array_push_back(&added, &name);
		}
	}
	if (auth_master_user_list_deinit(&ctx) < 0) {
		/* e.g. a userdb that can't list its users: keep what we had */
		if (!warned) {
			i_warning("Couldn't list the users from the userdb: "
				  "only users that change something are "
				  "synced (set mesh_list_users=no if the "
				  "userdb can't list users)");
			warned = TRUE;
		}
		auth_master_deinit(&conn);
		mesh_name_set_deinit(&names);
		return;
	}
	auth_master_deinit(&conn);

	hash = mesh_name_set_hash(names);
	e_debug(mesh_event, "Listed %u users from the userdb, %u of them new",
		count, array_count(&added));
	if (mesh_local_listed && strcmp(hash, mesh_local_hash) == 0) {
		i_free(hash);
		mesh_name_set_deinit(&names);
		return;
	}
	if (mesh_local_listed) {
		i_info("The userdb's users changed: %u users, %u new",
		       hash_table_count(names), array_count(&added));
	}
	mesh_name_set_deinit(&mesh_local_users);
	mesh_local_users = names;
	i_free(mesh_local_hash);
	mesh_local_hash = hash;
	mesh_local_listed = TRUE;

	/* Users that are new here are synced with the nodes that have them,
	   e.g. after being added to this node's userdb. Users that are gone
	   aren't pushed anymore. Anchors depend on who has the user. */
	array_foreach_elem(&added, username) {
		user = mesh_user_lookup(username, TRUE);
		if (push_new)
			mesh_queue_push_all(user, MESH_PRIORITY_LOW, FALSE);
	}
	mesh_queue_peers_changed();
}

void mesh_userlist_refresh(bool push_new)
{
	T_BEGIN {
		mesh_userlist_refresh_real(push_new);
	} T_END;
}

static void mesh_userlist_timeout(void *context ATTR_UNUSED)
{
	mesh_userlist_refresh(TRUE);
}

void mesh_userlist_start_refreshing(void)
{
	/* notice users added to or removed from the userdb */
	if (mesh_set->mesh_list_users &&
	    mesh_set->mesh_userlist_refresh_interval > 0 &&
	    mesh_userlist_to == NULL) {
		mesh_userlist_to = timeout_add(
			mesh_set->mesh_userlist_refresh_interval * 1000,
			mesh_userlist_timeout, NULL);
	}
}

/*
 * The peers' users
 */

static struct mesh_peer_users *mesh_peer_users_get(const struct mesh_peer *peer)
{
	i_assert(peer->idx < mesh_peer_users_count);
	return mesh_peer_users[peer->idx];
}

bool mesh_peer_has_user(const struct mesh_peer *peer, const char *username)
{
	const struct mesh_peer_users *pu = mesh_peer_users_get(peer);

	if (hash_table_count(pu->missing) > 0 &&
	    mesh_name_set_contains(pu->missing, username))
		return FALSE;
	/* not known yet, or the peer can't list its users */
	if (pu->hash == NULL || strcmp(pu->hash, MESH_USERLIST_ALL) == 0)
		return TRUE;
	return mesh_name_set_contains(pu->names, username);
}

static void
mesh_peer_users_replace(struct mesh_peer *peer, const char *hash,
			HASH_TABLE_TYPE(mesh_name_set) names)
{
	struct mesh_peer_users *pu = mesh_peer_users_get(peer);
	HASH_TABLE_TYPE(mesh_name_set) old = pu->names;
	bool old_all = pu->hash == NULL ||
		strcmp(pu->hash, MESH_USERLIST_ALL) == 0;
	struct hash_iterate_context *iter;
	struct mesh_user *user;
	char *key, *value;
	unsigned int added = 0;

	i_free(pu->hash);
	pu->hash = i_strdup(hash);
	pu->names = names;
	/* the new list says who the peer has */
	mesh_name_set_deinit(&pu->missing);
	mesh_name_set_init(&pu->missing);
	if (hash_table_is_created(names)) {
		/* Users that the peer now has: push them to it if this node
		   syncs them with it directly. */
		iter = hash_table_iterate_init(names);
		while (hash_table_iterate(iter, names, &key, &value)) {
			if (!old_all && hash_table_is_created(old) &&
			    hash_table_lookup(old, key) != NULL)
				continue;
			added++;
			if (!mesh_local_has_user(key))
				continue;
			user = mesh_user_lookup(key, TRUE);
			if (mesh_user_peer_is_direct(user, peer))
				mesh_queue_push(user, peer, MESH_PRIORITY_LOW,
						FALSE);
		}
		hash_table_iterate_deinit(&iter);
	}
	e_debug(mesh_event, "Peer %s has %u users (%u new)", peer->name,
		hash_table_is_created(names) ? hash_table_count(names) : 0,
		added);
	if (hash_table_is_created(old))
		mesh_name_set_deinit(&old);
	/* anchors depend on who has the user */
	mesh_queue_peers_changed();
}

struct mesh_userlist_fetch {
	struct mesh_peer *peer;
};

static void
mesh_peer_users_fetched(const buffer_t *body, const char *error,
			struct mesh_userlist_fetch *ctx)
{
	struct mesh_peer *peer = ctx->peer;
	struct mesh_peer_users *pu = mesh_peer_users_get(peer);
	HASH_TABLE_TYPE(mesh_name_set) names;
	const char *const *lines, *hash;
	unsigned int i;

	i_free(ctx);
	pu->get = NULL;
	if (body == NULL) {
		e_warning(mesh_event, "Couldn't get the user list of %s: %s",
			  peer->name, error);
		return;
	}
	T_BEGIN {
		lines = t_strsplit(t_strndup(body->data, body->used), "\n");
		hash = lines[0] == NULL ? MESH_USERLIST_ALL : lines[0];
		i_zero(&names);
		if (strcmp(hash, MESH_USERLIST_ALL) != 0) {
			mesh_name_set_init(&names);
			for (i = 1; lines[i] != NULL; i++) {
				if (lines[i][0] != '\0') {
					(void)mesh_name_set_add(names,
						t_str_tabunescape(lines[i]));
				}
			}
		}
		mesh_peer_users_replace(peer, hash, names);
	} T_END;
}

void mesh_peer_users_hash_seen(struct mesh_peer *peer, const char *hash)
{
	struct mesh_peer_users *pu = mesh_peer_users_get(peer);
	struct mesh_userlist_fetch *ctx;
	HASH_TABLE_TYPE(mesh_name_set) none;

	if (hash[0] == '\0') {
		/* an older version without user lists: assume everyone */
		hash = MESH_USERLIST_ALL;
	}
	if (pu->hash != NULL && strcmp(pu->hash, hash) == 0)
		return;
	if (strcmp(hash, MESH_USERLIST_ALL) == 0) {
		i_zero(&none);
		mesh_peer_users_replace(peer, hash, none);
		return;
	}
	if (pu->get != NULL && pu->wanted_hash != NULL &&
	    strcmp(pu->wanted_hash, hash) == 0)
		return;
	if (pu->get != NULL)
		mesh_peer_http_get_abort(&pu->get);
	i_free(pu->wanted_hash);
	pu->wanted_hash = i_strdup(hash);
	ctx = i_new(struct mesh_userlist_fetch, 1);
	ctx->peer = peer;
	pu->get = mesh_peer_http_get(peer, "/mesh/v1/userlist",
		MESH_USERLIST_FETCH_TIMEOUT_MSECS,
		(mesh_peer_http_get_callback_t *)mesh_peer_users_fetched, ctx);
}

void mesh_peer_user_missing(struct mesh_peer *peer, const char *username)
{
	struct mesh_peer_users *pu = mesh_peer_users_get(peer);

	if (mesh_name_set_add(pu->missing, username)) {
		e_debug(mesh_event, "Peer %s doesn't have user %s",
			peer->name, username);
		/* anchors depend on who has the user */
		mesh_queue_peers_changed();
	}
}

void mesh_userlist_init(void)
{
	unsigned int i;

	mesh_name_set_init(&mesh_local_users);
	mesh_peer_users_count = array_count(&mesh_peers);
	mesh_peer_users = i_new(struct mesh_peer_users *,
				I_MAX(mesh_peer_users_count, 1));
	for (i = 0; i < mesh_peer_users_count; i++) {
		mesh_peer_users[i] = i_new(struct mesh_peer_users, 1);
		mesh_name_set_init(&mesh_peer_users[i]->missing);
	}
}

void mesh_userlist_deinit(void)
{
	struct mesh_peer_users *pu;
	unsigned int i;

	timeout_remove(&mesh_userlist_to);
	for (i = 0; i < mesh_peer_users_count; i++) {
		pu = mesh_peer_users[i];
		if (pu->get != NULL)
			mesh_peer_http_get_abort(&pu->get);
		mesh_name_set_deinit(&pu->names);
		mesh_name_set_deinit(&pu->missing);
		i_free(pu->hash);
		i_free(pu->wanted_hash);
		i_free(pu);
	}
	i_free(mesh_peer_users);
	mesh_name_set_deinit(&mesh_local_users);
	i_free(mesh_local_hash);
}
