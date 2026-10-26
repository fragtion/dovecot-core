#ifndef MESH_SETTINGS_H
#define MESH_SETTINGS_H

/* <settings checks> */
enum mesh_topology {
	/* Each user has one anchor node. Other nodes sync the user only with
	   the anchor, which relays changes to everyone else. */
	MESH_TOPOLOGY_ANCHOR,
	/* Every node syncs every user directly with every other node. */
	MESH_TOPOLOGY_FULL,
};
/* </settings checks> */

struct mesh_peer_settings {
	pool_t pool;

	const char *name;
	const char *mesh_peer_host;
	in_port_t mesh_peer_doveadm_port;
	in_port_t mesh_peer_http_port;
	in_port_t mesh_peer_lmtp_port;
	bool mesh_peer_doveadm_ssl;
	bool mesh_peer_http_ssl;
};

struct mesh_settings {
	pool_t pool;

	const char *mesh_node_name;
	ARRAY_TYPE(const_string) mesh_peers;
	const char *mesh_topology;

	const char *mesh_api_key;
	const char *mesh_state_path;
	const char *mesh_doveadm_socket_path;

	unsigned int mesh_coalesce_delay;
	unsigned int mesh_heartbeat_interval;
	unsigned int mesh_peer_down_timeout;
	unsigned int mesh_incremental_sync_interval;
	unsigned int mesh_full_sync_interval;
	unsigned int mesh_sync_timeout;
	unsigned int mesh_max_parallel_syncs;
	unsigned int mesh_verify_interval;
	bool mesh_dsync_debug;

	/* generated: */
	enum mesh_topology parsed_topology;
	ARRAY(const struct mesh_peer_settings *) parsed_peers;
};

extern const struct setting_parser_info mesh_setting_parser_info;
extern const struct setting_parser_info mesh_peer_setting_parser_info;

/* Look up the mesh_peer { .. } filters into parsed_peers. */
int mesh_settings_load_peers(struct event *event, struct mesh_settings *set,
			     const char **error_r);

#endif
