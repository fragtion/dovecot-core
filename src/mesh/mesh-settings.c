/* Copyright (c) Dovecot authors, see top-level COPYING file */

#include "lib.h"
#include "array.h"
#include "settings.h"
#include "settings-parser.h"
#include "service-settings.h"
#include "mesh-settings.h"

static bool mesh_settings_check(void *_set, pool_t pool, const char **error_r);

struct service_settings mesh_service_settings = {
	.name = "mesh",
	.protocol = "",
	.type = "",
	.executable = "mesh",
	/* root, like doveadm: it needs the doveadm-server socket and a
	   writable state file */
	.user = "",
	.group = "",
	.privileged_group = "",
	.extra_groups = ARRAY_INIT,
	.chroot = "",

	.drop_priv_before_exec = FALSE,

	/* keep one process running: it sends heartbeats and runs timers */
	.process_min_avail = 1,
	.process_limit = 1,
	.idle_kill_interval = SET_TIME_INFINITE,

	.unix_listeners = ARRAY_INIT,
	.fifo_listeners = ARRAY_INIT,
	.inet_listeners = ARRAY_INIT,

	.process_limit_1 = TRUE
};

const struct setting_keyvalue mesh_service_settings_defaults[] = {
	{ "unix_listener", "mesh mesh-notify" },

	/* doveadm mesh */
	{ "unix_listener/mesh/path", "mesh" },
	{ "unix_listener/mesh/type", "admin" },
	{ "unix_listener/mesh/mode", "0600" },

	/* change notifications from the mesh plugin in mail processes */
	{ "unix_listener/mesh-notify/path", "mesh-notify" },
	{ "unix_listener/mesh-notify/type", "notify" },
	{ "unix_listener/mesh-notify/mode", "0666" },

	{ "inet_listener", "mesh-http" },
	{ "inet_listener/mesh-http/name", "mesh-http" },
	{ "inet_listener/mesh-http/type", "http" },
	{ "inet_listener/mesh-http/port", "0" },

	{ NULL, NULL }
};

#undef DEF
#define DEF(type, name) \
	SETTING_DEFINE_STRUCT_##type(#name, name, struct mesh_peer_settings)

static const struct setting_define mesh_peer_setting_defines[] = {
	SETTING_DEFINE_STRUCT_STR("mesh_peer_name", name,
				  struct mesh_peer_settings),
	DEF(STR, mesh_peer_host),
	DEF(IN_PORT, mesh_peer_doveadm_port),
	DEF(IN_PORT, mesh_peer_http_port),
	DEF(IN_PORT, mesh_peer_lmtp_port),
	DEF(BOOL, mesh_peer_doveadm_ssl),
	DEF(BOOL, mesh_peer_http_ssl),

	SETTING_DEFINE_LIST_END
};

static const struct mesh_peer_settings mesh_peer_default_settings = {
	.name = "",
	.mesh_peer_host = "",
	.mesh_peer_doveadm_port = 0,
	.mesh_peer_http_port = 0,
	.mesh_peer_lmtp_port = 24,
	.mesh_peer_doveadm_ssl = FALSE,
	.mesh_peer_http_ssl = FALSE,
};

const struct setting_parser_info mesh_peer_setting_parser_info = {
	.name = "mesh_peer",

	.defines = mesh_peer_setting_defines,
	.defaults = &mesh_peer_default_settings,

	.struct_size = sizeof(struct mesh_peer_settings),
	.pool_offset1 = 1 + offsetof(struct mesh_peer_settings, pool),
};

#undef DEF
#define DEF(type, name) \
	SETTING_DEFINE_STRUCT_##type(#name, name, struct mesh_settings)

static const struct setting_define mesh_setting_defines[] = {
	DEF(STR, mesh_node_name),
	{ .type = SET_FILTER_ARRAY, .key = "mesh_peer",
	  .offset = offsetof(struct mesh_settings, mesh_peers),
	  .filter_array_field_name = "mesh_peer_name", },
	DEF(ENUM, mesh_topology),

	DEF(STR, mesh_api_key),
	DEF(STR, mesh_state_path),
	DEF(STR, mesh_doveadm_socket_path),

	DEF(TIME_MSECS, mesh_coalesce_delay),
	DEF(TIME_MSECS, mesh_heartbeat_interval),
	DEF(TIME_MSECS, mesh_peer_down_timeout),
	DEF(TIME, mesh_incremental_sync_interval),
	DEF(TIME, mesh_full_sync_interval),
	DEF(TIME, mesh_sync_timeout),
	DEF(UINT, mesh_max_parallel_syncs),
	DEF(TIME, mesh_verify_interval),
	DEF(BOOL, mesh_dsync_debug),

	SETTING_DEFINE_LIST_END
};

static const struct mesh_settings mesh_default_settings = {
	.mesh_node_name = "",
	.mesh_peers = ARRAY_INIT,
	.mesh_topology = "anchor:full",

	.mesh_api_key = "",
	.mesh_state_path = "mesh.db",
	.mesh_doveadm_socket_path = "doveadm-server",

	.mesh_coalesce_delay = 100,
	.mesh_heartbeat_interval = 2*1000,
	.mesh_peer_down_timeout = 7*1000,
	.mesh_incremental_sync_interval = 10*60,
	.mesh_full_sync_interval = 24*60*60,
	.mesh_sync_timeout = 10*60,
	.mesh_max_parallel_syncs = 4,
	.mesh_verify_interval = 0,
	.mesh_dsync_debug = FALSE,
};

const struct setting_parser_info mesh_setting_parser_info = {
	.name = "mesh",

	.defines = mesh_setting_defines,
	.defaults = &mesh_default_settings,

	.struct_size = sizeof(struct mesh_settings),
	.pool_offset1 = 1 + offsetof(struct mesh_settings, pool),
	.check_func = mesh_settings_check,
};

/* <settings checks> */
static bool mesh_settings_check(void *_set, pool_t pool ATTR_UNUSED,
				const char **error_r)
{
	struct mesh_settings *set = _set;

	if (strcmp(set->mesh_topology, "anchor") == 0)
		set->parsed_topology = MESH_TOPOLOGY_ANCHOR;
	else if (strcmp(set->mesh_topology, "full") == 0)
		set->parsed_topology = MESH_TOPOLOGY_FULL;
	else {
		*error_r = t_strdup_printf("Unknown mesh_topology: %s",
					   set->mesh_topology);
		return FALSE;
	}
	if (set->mesh_max_parallel_syncs == 0) {
		*error_r = "mesh_max_parallel_syncs must not be 0";
		return FALSE;
	}
	if (set->mesh_heartbeat_interval == 0) {
		*error_r = "mesh_heartbeat_interval must not be 0";
		return FALSE;
	}
	if (set->mesh_peer_down_timeout <= set->mesh_heartbeat_interval) {
		*error_r = "mesh_peer_down_timeout must be larger than "
			"mesh_heartbeat_interval";
		return FALSE;
	}
	return TRUE;
}
/* </settings checks> */

int mesh_settings_load_peers(struct event *event, struct mesh_settings *set,
			     const char **error_r)
{
	const struct mesh_peer_settings *peer_set;
	const char *peer_name, *error;

	p_array_init(&set->parsed_peers, set->pool, 4);
	if (!array_is_created(&set->mesh_peers))
		return 0;

	array_foreach_elem(&set->mesh_peers, peer_name) {
		if (settings_get_filter(event, "mesh_peer", peer_name,
					&mesh_peer_setting_parser_info, 0,
					&peer_set, &error) < 0) {
			*error_r = t_strdup_printf(
				"Failed to get mesh_peer %s: %s",
				peer_name, error);
			return -1;
		}
		if (peer_set->mesh_peer_host[0] == '\0') {
			*error_r = t_strdup_printf(
				"mesh_peer %s: mesh_peer_host is empty",
				peer_name);
			settings_free(peer_set);
			return -1;
		}
		if (peer_set->mesh_peer_doveadm_port == 0) {
			*error_r = t_strdup_printf(
				"mesh_peer %s: mesh_peer_doveadm_port is 0",
				peer_name);
			settings_free(peer_set);
			return -1;
		}
		pool_add_external_ref(set->pool, peer_set->pool);
		array_push_back(&set->parsed_peers, &peer_set);
		settings_free(peer_set);
	}
	return 0;
}
