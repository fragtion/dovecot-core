/* Copyright (c) Dovecot authors, see top-level COPYING file */

#include "lib.h"
#include "array.h"
#include "str.h"
#include "strescape.h"
#include "time-util.h"
#include "connection.h"
#include "istream.h"
#include "ostream.h"
#include "doveadm.h"
#include "doveadm-print.h"

#include <unistd.h>

/* Talks to the mesh service's admin socket. Each reply is "+OK" or
   "-ERR <error>", then zero or more tab-separated rows, then an empty
   line. */
#define MESH_SOCKET_NAME "mesh"
/* VERIFY waits for every node to fingerprint the user's mailboxes */
#define MESH_REPLY_TIMEOUT_SECS (6*60)

struct mesh_cmd_field {
	const char *name;
	enum doveadm_print_header_flags flags;
};

static const struct mesh_cmd_field mesh_peer_fields[] = {
	{ "peer", 0 },
	{ "state", 0 },
	{ "paused", 0 },
	{ "rtt_ms", DOVEADM_PRINT_HEADER_FLAG_RIGHT_JUSTIFY },
	{ "clock_offset_ms", DOVEADM_PRINT_HEADER_FLAG_RIGHT_JUSTIFY },
	{ "last_seen", 0 },
	{ "running", DOVEADM_PRINT_HEADER_FLAG_RIGHT_JUSTIFY },
	{ "syncs_ok", DOVEADM_PRINT_HEADER_FLAG_RIGHT_JUSTIFY },
	{ "syncs_failed", DOVEADM_PRINT_HEADER_FLAG_RIGHT_JUSTIFY },
	{ "last_error", 0 },
};

static const struct mesh_cmd_field mesh_user_fields[] = {
	{ "user", 0 },
	{ "peer", 0 },
	{ "anchor", 0 },
	{ "status", 0 },
	{ "last_ok", 0 },
	{ "pending_since", 0 },
	{ "syncs_ok", DOVEADM_PRINT_HEADER_FLAG_RIGHT_JUSTIFY },
	{ "syncs_failed", DOVEADM_PRINT_HEADER_FLAG_RIGHT_JUSTIFY },
	{ "last_error", 0 },
};

/* fields that are UNIX timestamps (0 = never) */
static const char *const mesh_time_fields[] = {
	"last_seen", "last_ok", "pending_since", NULL
};

static struct istream *mesh_send_cmd(const char *const *args)
{
	const char *path = t_strconcat(doveadm_settings->base_dir,
				       "/"MESH_SOCKET_NAME, NULL);
	const struct connection_settings set = {
		.service_name_out = "mesh-admin",
		.service_name_in = "mesh",
		.major_version = 1,
		.minor_version = 0,
	};
	struct istream *input;
	struct ostream *output;
	const char *error;
	string_t *str = t_str_new(128);

	if (doveadm_blocking_connect(path, &set, &input, &output, &error) < 0)
		i_fatal("%s (is the mesh service running?)", error);
	for (; *args != NULL; args++) {
		if (str_len(str) > 0)
			str_append_c(str, '\t');
		str_append_tabescaped(str, *args);
	}
	str_append_c(str, '\n');
	o_stream_nsend(output, str_data(str), str_len(str));
	if (o_stream_flush(output) < 0)
		i_fatal("write(%s) failed: %s", path, o_stream_get_error(output));
	o_stream_unref(&output);
	return input;
}

/* Run the command and return the reply's rows. Fails the doveadm command
   on -ERR. */
static int mesh_cmd(struct doveadm_cmd_context *cctx, const char *const *args,
		    ARRAY_TYPE(const_string) *rows_r)
{
	struct istream *input = mesh_send_cmd(args);
	const char *line;
	int ret = 0;

	t_array_init(rows_r, 16);
	alarm(MESH_REPLY_TIMEOUT_SECS);
	line = i_stream_read_next_line(input);
	if (line == NULL) {
		i_fatal("read(mesh) failed: %s",
			i_stream_get_disconnect_reason(input));
	}
	if (str_begins(line, "-ERR ", &line)) {
		e_error(cctx->event, "%s", line);
		doveadm_exit_code = EX_USAGE;
		ret = -1;
	} else if (strcmp(line, "+OK") != 0)
		i_fatal("mesh: Unexpected reply: %s", line);
	while ((line = i_stream_read_next_line(input)) != NULL) {
		if (line[0] == '\0')
			break;
		line = t_strdup(line);
		array_push_back(rows_r, &line);
	}
	if (line == NULL) {
		i_fatal("read(mesh) failed: %s",
			i_stream_get_disconnect_reason(input));
	}
	alarm(0);
	i_stream_destroy(&input);
	return ret;
}

static bool mesh_field_is_time(const char *name)
{
	return str_array_find(mesh_time_fields, name);
}

static void mesh_print_value(const char *field, const char *value)
{
	time_t t;

	if (mesh_field_is_time(field) && str_to_time(value, &t) == 0) {
		if (t == 0)
			value = "-";
		else
			value = t_strflocaltime("%Y-%m-%d %H:%M:%S", t);
	}
	doveadm_print(value);
}

static void
mesh_print_rows(const struct mesh_cmd_field *fields, unsigned int count,
		const ARRAY_TYPE(const_string) *rows)
{
	const char *row, *const *args;
	unsigned int i;

	doveadm_print_init(DOVEADM_PRINT_TYPE_TABLE);
	for (i = 0; i < count; i++) {
		doveadm_print_header(fields[i].name, fields[i].name,
				     fields[i].flags);
	}
	array_foreach_elem(rows, row) {
		args = t_strsplit_tabescaped(row);
		for (i = 0; i < count; i++)
			mesh_print_value(fields[i].name,
					 args[i] == NULL ? "" : args[i]);
	}
}

static void cmd_mesh_status(struct doveadm_cmd_context *cctx)
{
	const char *const args[] = { "STATUS", NULL };
	ARRAY_TYPE(const_string) rows;
	const char *row, *const *kv;

	if (mesh_cmd(cctx, args, &rows) < 0)
		return;

	/* rows are <key> <value> */
	doveadm_print_init(DOVEADM_PRINT_TYPE_TABLE);
	array_foreach_elem(&rows, row) {
		kv = t_strsplit_tabescaped(row);
		doveadm_print_header_simple(kv[0]);
	}
	array_foreach_elem(&rows, row) {
		kv = t_strsplit_tabescaped(row);
		doveadm_print(kv[1] == NULL ? "" : kv[1]);
	}
}

static void cmd_mesh_peers(struct doveadm_cmd_context *cctx)
{
	const char *const args[] = { "PEERS", NULL };
	ARRAY_TYPE(const_string) rows;

	if (mesh_cmd(cctx, args, &rows) == 0) {
		mesh_print_rows(mesh_peer_fields,
				N_ELEMENTS(mesh_peer_fields), &rows);
	}
}

static void cmd_mesh_users(struct doveadm_cmd_context *cctx)
{
	const char *user;
	ARRAY_TYPE(const_string) rows;

	if (!doveadm_cmd_param_str(cctx, "user", &user))
		user = NULL;
	const char *const args[] = { "USERS", user, NULL };
	if (mesh_cmd(cctx, args, &rows) == 0) {
		mesh_print_rows(mesh_user_fields,
				N_ELEMENTS(mesh_user_fields), &rows);
	}
}

static void cmd_mesh_sync(struct doveadm_cmd_context *cctx)
{
	const char *user;
	ARRAY_TYPE(const_string) rows;
	bool full;

	if (!doveadm_cmd_param_str(cctx, "user", &user))
		help_ver2(&doveadm_cmd_mesh_sync);
	if (!doveadm_cmd_param_bool(cctx, "full", &full))
		full = FALSE;
	const char *const args[] = { "SYNC", user, full ? "full" : NULL, NULL };
	(void)mesh_cmd(cctx, args, &rows);
}

/* Rows: R <result> <user>, N <node> <error>, then
   M <mailbox> <guid> <match|differs> <cell for each node>. */
static void cmd_mesh_verify(struct doveadm_cmd_context *cctx)
{
	const char *user, *row, *const *args, *result = NULL;
	ARRAY_TYPE(const_string) rows, nodes;
	unsigned int i, node_count;

	if (!doveadm_cmd_param_str(cctx, "user", &user))
		help_ver2(&doveadm_cmd_mesh_verify);
	const char *const cmd_args[] = { "VERIFY", user, NULL };
	if (mesh_cmd(cctx, cmd_args, &rows) < 0)
		return;

	t_array_init(&nodes, 8);
	array_foreach_elem(&rows, row) {
		args = t_strsplit_tabescaped(row);
		if (strcmp(args[0], "R") == 0)
			result = t_strdup(args[1]);
		else if (strcmp(args[0], "N") == 0 && args[1] != NULL) {
			array_push_back(&nodes, &args[1]);
			if (args[2] != NULL && args[2][0] != '\0') {
				e_error(cctx->event, "%s: %s", args[1],
					args[2]);
			}
		}
	}
	if (result == NULL)
		i_fatal("mesh: Unexpected VERIFY reply");

	node_count = array_count(&nodes);
	doveadm_print_init(DOVEADM_PRINT_TYPE_TABLE);
	doveadm_print_header_simple("mailbox");
	doveadm_print_header_simple("status");
	for (i = 0; i < node_count; i++) {
		const char *name = array_idx_elem(&nodes, i);
		doveadm_print_header(name, name, 0);
	}
	array_foreach_elem(&rows, row) {
		args = t_strsplit_tabescaped(row);
		if (strcmp(args[0], "M") != 0 ||
		    str_array_length(args) < 4 + node_count)
			continue;
		doveadm_print(args[1]);
		doveadm_print(args[3]);
		for (i = 0; i < node_count; i++)
			doveadm_print(args[4 + i]);
	}

	if (strcmp(result, "mismatch") == 0) {
		e_error(cctx->event, "%s differs between nodes", user);
		doveadm_exit_code = EX_DATAERR;
	} else if (strcmp(result, "error") == 0)
		doveadm_exit_code = EX_TEMPFAIL;
}

static void cmd_mesh_pause_resume(struct doveadm_cmd_context *cctx,
				  const char *cmd,
				  struct doveadm_cmd_ver2 *help_cmd)
{
	const char *peer;
	ARRAY_TYPE(const_string) rows;

	if (!doveadm_cmd_param_str(cctx, "peer", &peer))
		help_ver2(help_cmd);
	const char *const args[] = { cmd, peer, NULL };
	(void)mesh_cmd(cctx, args, &rows);
}

static void cmd_mesh_pause(struct doveadm_cmd_context *cctx)
{
	cmd_mesh_pause_resume(cctx, "PAUSE", &doveadm_cmd_mesh_pause);
}

static void cmd_mesh_resume(struct doveadm_cmd_context *cctx)
{
	cmd_mesh_pause_resume(cctx, "RESUME", &doveadm_cmd_mesh_resume);
}

struct doveadm_cmd_ver2 doveadm_cmd_mesh_status = {
	.cmd = cmd_mesh_status,
	.name = "mesh status",
	.usage = "",
DOVEADM_CMD_PARAMS_START
DOVEADM_CMD_PARAMS_END
};

struct doveadm_cmd_ver2 doveadm_cmd_mesh_peers = {
	.cmd = cmd_mesh_peers,
	.name = "mesh peers",
	.usage = "",
DOVEADM_CMD_PARAMS_START
DOVEADM_CMD_PARAMS_END
};

struct doveadm_cmd_ver2 doveadm_cmd_mesh_users = {
	.cmd = cmd_mesh_users,
	.name = "mesh users",
	.usage = "[<user>]",
DOVEADM_CMD_PARAMS_START
DOVEADM_CMD_PARAM('\0', "user", CMD_PARAM_STR, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAMS_END
};

struct doveadm_cmd_ver2 doveadm_cmd_mesh_sync = {
	.cmd = cmd_mesh_sync,
	.name = "mesh sync",
	.usage = "[-f] <user>",
DOVEADM_CMD_PARAMS_START
DOVEADM_CMD_PARAM('f', "full", CMD_PARAM_BOOL, 0)
DOVEADM_CMD_PARAM('\0', "user", CMD_PARAM_STR, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAMS_END
};

struct doveadm_cmd_ver2 doveadm_cmd_mesh_verify = {
	.cmd = cmd_mesh_verify,
	.name = "mesh verify",
	.usage = "<user>",
DOVEADM_CMD_PARAMS_START
DOVEADM_CMD_PARAM('\0', "user", CMD_PARAM_STR, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAMS_END
};

struct doveadm_cmd_ver2 doveadm_cmd_mesh_pause = {
	.cmd = cmd_mesh_pause,
	.name = "mesh pause",
	.usage = "<peer>",
DOVEADM_CMD_PARAMS_START
DOVEADM_CMD_PARAM('\0', "peer", CMD_PARAM_STR, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAMS_END
};

struct doveadm_cmd_ver2 doveadm_cmd_mesh_resume = {
	.cmd = cmd_mesh_resume,
	.name = "mesh resume",
	.usage = "<peer>",
DOVEADM_CMD_PARAMS_START
DOVEADM_CMD_PARAM('\0', "peer", CMD_PARAM_STR, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAMS_END
};
