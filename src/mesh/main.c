/* Copyright (c) Dovecot authors, see top-level COPYING file */

#include "mesh-common.h"
#include "ioloop.h"
#include "restrict-access.h"
#include "hostpid.h"
#include "settings.h"
#include "master-service.h"
#include "master-service-settings.h"

#include <unistd.h>

static struct event_category event_category_mesh = {
	.name = "mesh",
};

struct event *mesh_event;
const struct mesh_settings *mesh_set;
const char *mesh_node_name;
time_t mesh_start_time;

static void client_connected(struct master_service_connection *conn)
{
	const char *type = master_service_connection_get_type(conn);

	master_service_client_connection_accept(conn);
	if (strcmp(type, "http") == 0)
		mesh_http_conn_create(conn);
	else if (strcmp(type, "admin") == 0)
		mesh_conn_create(conn->fd, TRUE);
	else
		mesh_conn_create(conn->fd, FALSE);
}

static void main_init(void)
{
	struct mesh_settings *set;
	const char *error;

	set = (struct mesh_settings *)settings_get_or_fatal(
		master_service_get_event(master_service),
		&mesh_setting_parser_info);
	if (mesh_settings_load_peers(master_service_get_event(master_service),
				     set, &error) < 0)
		i_fatal("%s", error);
	mesh_set = set;
	mesh_event = event_create(master_service_get_event(master_service));
	event_add_category(mesh_event, &event_category_mesh);
	mesh_node_name = mesh_set->mesh_node_name[0] != '\0' ?
		mesh_set->mesh_node_name : my_hostdomain();
	mesh_start_time = ioloop_time;

	mesh_conn_init();
	mesh_peers_init();
	mesh_queue_init();
	mesh_state_init();
	mesh_http_init();
	mesh_queue_startup_sync();
	i_info("Mesh node %s started: topology=%s, %u peers",
	       mesh_node_name, mesh_set->mesh_topology,
	       array_count(&mesh_peers));
}

static void main_deinit(void)
{
	mesh_sync_deinit();
	mesh_state_deinit();
	mesh_http_deinit();
	mesh_queue_deinit();
	mesh_peers_deinit();
	mesh_conn_deinit();
	settings_free(mesh_set);
	event_unref(&mesh_event);
}

int main(int argc, char *argv[])
{
	master_service = master_service_init("mesh", 0, &argc, &argv, "");
	if (master_getopt(master_service) > 0)
		return FATAL_DEFAULT;

	const char *error;
	if (master_service_settings_read_simple(master_service, &error) < 0)
		i_fatal("%s", error);

	master_service_init_log(master_service);
	restrict_access_by_env(RESTRICT_ACCESS_FLAG_ALLOW_ROOT, NULL);
	restrict_access_allow_coredumps(TRUE);

	main_init();
	master_service_init_finish(master_service);
	master_service_run(master_service, client_connected);
	main_deinit();

	master_service_deinit(&master_service);
	return 0;
}
