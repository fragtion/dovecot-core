/* Copyright (c) Dovecot authors, see top-level COPYING file */

#include "mesh-common.h"
#include "ioloop.h"
#include "str.h"
#include "strescape.h"
#include "istream.h"
#include "ostream.h"
#include "master-service.h"
#include "master-service-settings.h"

#include <fcntl.h>
#include <unistd.h>

/* Persistent per user and peer replication state, one line each:

   U <user> <peer> <dsync state> <state gen> <exclusive> <last_ok>
     <last_attempt> <last_full> <ok_count> <fail_count> <last_msecs>
     <pending> <pending_since> <last_error>

   Losing this file loses no mail: the next sync for each user is just a
   full comparison instead of an incremental one. */

#define MESH_STATE_WRITE_DELAY_MSECS 1000

static char *mesh_state_path;
static struct timeout *mesh_state_to;
static bool mesh_state_write_failed;

static const char *mesh_state_get_path(void)
{
	const char *path = mesh_set->mesh_state_path;

	if (path[0] == '\0' || path[0] == '/')
		return path;
	return t_strconcat(master_service_get_service_settings(master_service)->state_dir,
			   "/", path, NULL);
}

static void mesh_state_parse_line(const char *line, unsigned int linenum)
{
	const char *const *args = t_strsplit_tabescaped(line);
	struct mesh_user_peer *up;
	struct mesh_user *user;
	struct mesh_peer *peer;
	unsigned int pending;
	long long last_ok, last_attempt, last_full, pending_since;

	if (str_array_length(args) < 15 || strcmp(args[0], "U") != 0) {
		i_error("%s line %u: Invalid line, ignoring",
			mesh_state_path, linenum);
		return;
	}
	peer = mesh_peer_find(args[2]);
	if (peer == NULL) {
		/* peer was removed from configuration */
		return;
	}
	user = mesh_user_lookup(args[1], TRUE);
	up = &user->peers[peer->idx];
	if (str_to_uint(args[4], &up->state_gen) < 0 ||
	    str_to_llong(args[6], &last_ok) < 0 ||
	    str_to_llong(args[7], &last_attempt) < 0 ||
	    str_to_llong(args[8], &last_full) < 0 ||
	    str_to_uint(args[9], &up->ok_count) < 0 ||
	    str_to_uint(args[10], &up->fail_count) < 0 ||
	    str_to_uint(args[11], &up->last_msecs) < 0 ||
	    str_to_uint(args[12], &pending) < 0 ||
	    str_to_llong(args[13], &pending_since) < 0) {
		i_error("%s line %u: Invalid numbers, ignoring",
			mesh_state_path, linenum);
		return;
	}
	mesh_user_peer_set_state(up, args[3]);
	up->exclusive = strcmp(args[5], "1") == 0;
	up->last_ok = last_ok;
	up->last_attempt = last_attempt;
	up->last_full = last_full;
	if (args[14][0] != '\0')
		up->last_error = i_strdup(args[14]);
	if (pending > MESH_PRIORITY_NONE && pending <= MESH_PRIORITY_HIGH) {
		/* the push didn't finish before we stopped */
		mesh_queue_push(user, peer, (enum mesh_priority)pending, FALSE);
		up->pending_since = pending_since;
	}
}

static void mesh_state_read(void)
{
	struct istream *input;
	const char *line;
	unsigned int linenum = 0;
	int fd;

	fd = open(mesh_state_path, O_RDONLY);
	if (fd == -1) {
		if (errno != ENOENT)
			i_error("open(%s) failed: %m", mesh_state_path);
		return;
	}
	input = i_stream_create_fd_autoclose(&fd, SIZE_MAX);
	while ((line = i_stream_read_next_line(input)) != NULL) T_BEGIN {
		mesh_state_parse_line(line, ++linenum);
	} T_END;
	if (input->stream_errno != 0) {
		i_error("read(%s) failed: %s", mesh_state_path,
			i_stream_get_error(input));
	}
	i_stream_destroy(&input);
}

static void mesh_state_write_user(string_t *str, struct mesh_user *user)
{
	struct mesh_peer *peer;
	struct mesh_user_peer *up;

	array_foreach_elem(&mesh_peers, peer) {
		up = &user->peers[peer->idx];
		if (up->dsync_state == NULL && up->state_gen == 0 &&
		    !up->exclusive && up->last_attempt == 0 &&
		    up->pending == MESH_PRIORITY_NONE)
			continue;
		str_append(str, "U\t");
		str_append_tabescaped(str, user->username);
		str_append_c(str, '\t');
		str_append_tabescaped(str, peer->name);
		str_append_c(str, '\t');
		if (up->dsync_state != NULL)
			str_append_tabescaped(str, up->dsync_state);
		str_printfa(str, "\t%u\t%d", up->state_gen,
			    up->exclusive ? 1 : 0);
		str_printfa(str, "\t%lld\t%lld\t%lld\t%u\t%u\t%u\t%u\t%lld\t",
			    (long long)up->last_ok, (long long)up->last_attempt,
			    (long long)up->last_full, up->ok_count,
			    up->fail_count, up->last_msecs,
			    (unsigned int)(user->running_peer == peer &&
					   up->pending == MESH_PRIORITY_NONE ?
					   MESH_PRIORITY_NORMAL : up->pending),
			    (long long)up->pending_since);
		if (up->last_error != NULL)
			str_append_tabescaped(str, up->last_error);
		str_append_c(str, '\n');
	}
}

static int mesh_state_write(void)
{
	struct hash_iterate_context *iter;
	struct mesh_user *user;
	struct ostream *output;
	const char *temp_path;
	string_t *str;
	char *username;
	int fd, ret = 0;

	if (mesh_state_path[0] == '\0')
		return 0;

	temp_path = t_strconcat(mesh_state_path, ".tmp", NULL);
	fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd == -1) {
		if (!mesh_state_write_failed)
			i_error("open(%s) failed: %m", temp_path);
		mesh_state_write_failed = TRUE;
		return -1;
	}
	output = o_stream_create_fd_file_autoclose(&fd, 0);
	o_stream_cork(output);
	str = t_str_new(1024);
	iter = hash_table_iterate_init(mesh_users);
	while (hash_table_iterate(iter, mesh_users, &username, &user)) {
		str_truncate(str, 0);
		mesh_state_write_user(str, user);
		o_stream_nsend(output, str_data(str), str_len(str));
	}
	hash_table_iterate_deinit(&iter);
	if (o_stream_finish(output) < 0) {
		i_error("write(%s) failed: %s", temp_path,
			o_stream_get_error(output));
		ret = -1;
	}
	o_stream_destroy(&output);
	if (ret == 0 && rename(temp_path, mesh_state_path) < 0) {
		i_error("rename(%s, %s) failed: %m", temp_path,
			mesh_state_path);
		ret = -1;
	}
	if (ret < 0)
		i_unlink_if_exists(temp_path);
	else
		mesh_state_write_failed = FALSE;
	return ret;
}

static void mesh_state_timeout(void *context ATTR_UNUSED)
{
	timeout_remove(&mesh_state_to);
	(void)mesh_state_write();
}

void mesh_state_changed(void)
{
	if (mesh_state_to == NULL && mesh_state_path != NULL) {
		mesh_state_to = timeout_add(MESH_STATE_WRITE_DELAY_MSECS,
					    mesh_state_timeout, NULL);
	}
}

void mesh_state_init(void)
{
	mesh_state_path = i_strdup(mesh_state_get_path());
	if (mesh_state_path[0] != '\0')
		mesh_state_read();
}

void mesh_state_deinit(void)
{
	timeout_remove(&mesh_state_to);
	(void)mesh_state_write();
	i_free_and_null(mesh_state_path);
}
