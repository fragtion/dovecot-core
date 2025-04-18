/* Copyright (c) 2013-2018 Dovecot authors, see the included COPYING file */

#include "submission-common.h"
#include "array.h"
#include "ioloop.h"
#include "base64.h"
#include "str.h"
#include "llist.h"
#include "net.h"
#include "istream.h"
#include "ostream.h"
#include "hostpid.h"
#include "settings.h"
#include "master-service.h"
#include "mail-namespace.h"
#include "mail-storage.h"
#include "mail-storage-service.h"
#include "raw-storage.h"
#include "imap-urlauth.h"
#include "smtp-syntax.h"
#include "smtp-client-connection.h"

#include "submission-backend-relay.h"
#include "submission-recipient.h"
#include "submission-commands.h"
#include "submission-settings.h"

#include <unistd.h>

/* max. length of input command line */
#define MAX_INBUF_SIZE 4096

/* Stop reading input when output buffer has this many bytes. Once the buffer
   size has dropped to half of it, start reading input again. */
#define OUTBUF_THROTTLE_SIZE 4096

/* Disconnect client when it sends too many bad commands in a row */
#define CLIENT_MAX_BAD_COMMANDS 20

/* Disconnect client after idling this many milliseconds */
#define CLIENT_IDLE_TIMEOUT_MSECS (10*60*1000)

static const struct smtp_server_callbacks smtp_callbacks;
static const struct submission_client_vfuncs submission_client_vfuncs;

struct submission_module_register submission_module_register = { 0 };

struct client *submission_clients;
unsigned int submission_client_count;

static void client_input_pre(void *context)
{
	struct client *client = context;

	submission_backends_client_input_pre(client);
}
static void client_input_post(void *context)
{
	struct client *client = context;

	submission_backends_client_input_post(client);
}

static void client_parse_backend_capabilities(struct client *client)
{
	const struct submission_settings *set = client->set;
	const char *const *str;

	client->backend_capabilities = SMTP_CAPABILITY_NONE;
	if (array_is_empty(&set->submission_backend_capabilities))
		return;

	str = settings_boollist_get(&set->submission_backend_capabilities);
	for (; *str != NULL; str++) {
		if (strcmp(*str, "none") == 0)
			continue;
		enum smtp_capability cap = smtp_capability_find_by_name(*str);

		if (cap == SMTP_CAPABILITY_NONE) {
			e_warning(client->event,
				  "Unknown SMTP capability in submission_backend_capabilities: "
				  "%s", *str);
			continue;
		}

		client->backend_capabilities |= cap;
	}

	/* Make sure CHUNKING support is always enabled when BINARYMIME is
	   enabled by explicit configuration. */
	if (HAS_ALL_BITS(client->backend_capabilities,
			 SMTP_CAPABILITY_BINARYMIME)) {
		client->backend_capabilities |= SMTP_CAPABILITY_CHUNKING;
	}

	client->backend_capabilities_configured = TRUE;
}

void client_apply_backend_capabilities(struct client *client)
{
	enum smtp_capability caps = client->backend_capabilities;

	/* propagate capabilities */
	caps |= SMTP_CAPABILITY_AUTH | SMTP_CAPABILITY_PIPELINING |
		SMTP_CAPABILITY_SIZE | SMTP_CAPABILITY_ENHANCEDSTATUSCODES |
		SMTP_CAPABILITY_CHUNKING | SMTP_CAPABILITY_BURL;
#ifdef EXPERIMENTAL_MAIL_UTF8
	if (client->set->mail_utf8_extensions)
		caps |= SMTP_CAPABILITY_SMTPUTF8;
#endif
	caps &= SUBMISSION_SUPPORTED_SMTP_CAPABILITIES;
	smtp_server_connection_set_capabilities(client->conn, caps);
}

void client_default_backend_started(struct client *client,
				    enum smtp_capability caps)
{
	/* propagate capabilities from backend to frontend */
	if (!client->backend_capabilities_configured) {
		client->backend_capabilities = caps;
		client_apply_backend_capabilities(client);

		/* resume the server now that we have the backend
		   capabilities */
		smtp_server_connection_resume(client->conn);
	}
}

static void
client_create_backend_default(struct client *client,
			      const struct submission_settings *set)
{
	struct submision_backend_relay_settings relay_set;

	i_zero(&relay_set);
	relay_set.my_hostname = set->hostname;
	relay_set.protocol = SMTP_PROTOCOL_SMTP;
	relay_set.host = set->submission_relay_host;
	relay_set.port = set->submission_relay_port;
	relay_set.user = set->submission_relay_user;
	relay_set.master_user = set->submission_relay_master_user;
	relay_set.password = set->submission_relay_password;
	relay_set.rawlog_dir = set->submission_relay_rawlog_dir;
	relay_set.max_idle_time = set->submission_relay_max_idle_time;
	relay_set.connect_timeout_msecs = set->submission_relay_connect_timeout;
	relay_set.command_timeout_msecs = set->submission_relay_command_timeout;
	relay_set.trusted = set->submission_relay_trusted;

	if (strcmp(set->submission_relay_ssl, "smtps") == 0)
		relay_set.ssl_mode = SMTP_CLIENT_SSL_MODE_IMMEDIATE;
	else if (strcmp(set->submission_relay_ssl, "starttls") == 0)
		relay_set.ssl_mode = SMTP_CLIENT_SSL_MODE_STARTTLS;
	else
		relay_set.ssl_mode = SMTP_CLIENT_SSL_MODE_NONE;
	relay_set.ssl_verify = set->submission_relay_ssl_verify;

	client->backend_default_relay =
		submission_backend_relay_create(client, &relay_set);
	client->backend_default =
		submission_backend_relay_get(client->backend_default_relay);
}

static void client_init_urlauth(struct client *client)
{
	static const char *access_apps[] = { "submit+", NULL };
	struct imap_urlauth_config config;

	i_zero(&config);
	config.url_host = client->set->imap_urlauth_host;
	config.url_port = client->set->imap_urlauth_port;
	config.socket_path = t_strconcat(client->user->set->base_dir,
					 "/"IMAP_URLAUTH_SOCKET_NAME, NULL);
	config.session_id = client->user->session_id;
	config.access_anonymous = client->user->anonymous;
	config.access_user = client->user->username;
	config.access_service = "submission";
	config.access_applications = access_apps;

	client->urlauth_ctx = imap_urlauth_init(client->user, &config);
}

struct client *
client_create(int fd_in, int fd_out, struct event *event,
	      struct mail_user *user,
	      const struct submission_settings *set, const char *helo,
	      const struct smtp_proxy_data *proxy_data,
	      const unsigned char *pdata, unsigned int pdata_len,
	      bool no_greeting, bool have_mailbox_attribute_dict)
{
	enum submission_client_workarounds workarounds =
		set->parsed_workarounds;
	struct smtp_server_settings smtp_set;
	struct smtp_server_connection *conn;
	struct client *client;
	pool_t pool;

	/* always use nonblocking I/O */
	net_set_nonblock(fd_in, TRUE);
	net_set_nonblock(fd_out, TRUE);

	pool = pool_alloconly_create("submission client", 2048);
	client = p_new(pool, struct client, 1);
	client->pool = pool;
	client->v = submission_client_vfuncs;
	client->event = event;
	event_ref(client->event);
	client->user = user;
	client->set = set;

	i_array_init(&client->pending_backends, 4);
	i_array_init(&client->rcpt_to, 8);
	i_array_init(&client->rcpt_backends, 8);

	i_zero(&smtp_set);
	smtp_set.hostname = set->hostname;
	smtp_set.login_greeting = set->login_greeting;
	smtp_set.max_recipients = set->submission_max_recipients;
	smtp_set.max_client_idle_time_msecs = CLIENT_IDLE_TIMEOUT_MSECS;
	smtp_set.max_message_size = set->submission_max_mail_size;
	smtp_set.rawlog_dir = set->rawlog_dir;
	smtp_set.no_greeting = no_greeting;
	smtp_set.debug = event_want_debug(client->event);
	smtp_set.event_parent = event;

	if ((workarounds & SUBMISSION_WORKAROUND_WHITESPACE_BEFORE_PATH) != 0) {
		smtp_set.workarounds |=
			SMTP_SERVER_WORKAROUND_WHITESPACE_BEFORE_PATH;
	}
	if ((workarounds & SUBMISSION_WORKAROUND_MAILBOX_FOR_PATH) != 0) {
		smtp_set.workarounds |=
			SMTP_SERVER_WORKAROUND_MAILBOX_FOR_PATH;
	}

	client_parse_backend_capabilities(client);

	p_array_init(&client->module_contexts, client->pool, 5);

	conn = client->conn = smtp_server_connection_create(smtp_server,
		fd_in, fd_out, user->conn.remote_ip, user->conn.remote_port,
		FALSE, &smtp_set, &smtp_callbacks, client);
	smtp_server_connection_set_proxy_data(conn, proxy_data);
	smtp_server_connection_login(conn, client->user->username, helo,
				     pdata, pdata_len,
				     user->conn.end_client_tls_secured);

	client_create_backend_default(client, set);

	if (*set->imap_urlauth_host != '\0' && have_mailbox_attribute_dict) {
		/* Enable BURL capability only when urlauth dict is
		   configured correctly */
		client_init_urlauth(client);
	}

	submission_client_count++;
	DLLIST_PREPEND(&submission_clients, client);

	struct master_service_anvil_session anvil_session;
	mail_user_get_anvil_session(client->user, &anvil_session);
	if (master_service_anvil_connect(master_service, &anvil_session,
					 TRUE, client->anvil_conn_guid))
		client->anvil_sent = TRUE;

	if (hook_client_created != NULL)
		hook_client_created(&client);

	if (user->anonymous) {
		smtp_server_connection_abort(
			&conn, 534, "5.7.9",
			"Anonymous login is not allowed for submission");
		client = NULL;
	} else if (client->backend_capabilities_configured) {
		client_apply_backend_capabilities(client);
		smtp_server_connection_start(conn);
	} else {
		submission_backend_start(client->backend_default);
		smtp_server_connection_start_pending(conn);
	}

	submission_refresh_proctitle();
	return client;
}

static void client_state_reset(struct client *client)
{
	i_free(client->state.args);
	i_stream_unref(&client->state.data_input);
	pool_unref(&client->state.pool);

	i_zero(&client->state);
}

void client_destroy(struct client **_client, const char *prefix,
		    const char *reply_reason, const char *log_reason)
{
	struct client *client = *_client;
	struct smtp_server_connection *conn = client->conn;

	*_client = NULL;

	smtp_server_connection_terminate_full(
		&conn, (prefix == NULL ? "4.0.0" : prefix),
		reply_reason, log_reason);
}

static void
client_default_destroy(struct client *client)
{
	i_assert(client->disconnected);

	if (client->destroyed)
		return;
	client->destroyed = TRUE;

	submission_backends_destroy_all(client);
	array_free(&client->pending_backends);
	array_free(&client->rcpt_to);
	array_free(&client->rcpt_backends);

	submission_client_count--;
	DLLIST_REMOVE(&submission_clients, client);

	if (client->anvil_sent) {
		struct master_service_anvil_session anvil_session;
		mail_user_get_anvil_session(client->user, &anvil_session);
		master_service_anvil_disconnect(master_service, &anvil_session,
						client->anvil_conn_guid);
	}

	if (client->urlauth_ctx != NULL)
		imap_urlauth_deinit(&client->urlauth_ctx);

	mail_user_deinit(&client->user);

	client_state_reset(client);

	settings_free(client->set);
	event_unref(&client->event);
	pool_unref(&client->pool);

	master_service_client_connection_destroyed(master_service);
	submission_refresh_proctitle();
}

static void
client_connection_trans_start(void *context,
			      struct smtp_server_transaction *trans)
{
	struct client *client = context;

	client->state.pool =
		pool_alloconly_create("submission client state", 1024);

	client->v.trans_start(client, trans);
}

static void
client_default_trans_start(struct client *client,
			   struct smtp_server_transaction *trans)
{
	submission_backends_trans_start(client, trans);
}

static void
client_connection_trans_free(void *context,
			     struct smtp_server_transaction *trans)
{
	struct client *client = context;

	client->v.trans_free(client, trans);
}

static void
client_default_trans_free(struct client *client,
			  struct smtp_server_transaction *trans)
{
	array_clear(&client->rcpt_to);

	submission_backends_trans_free(client, trans);
	client_state_reset(client);
}

static void
client_connection_state_changed(void *context ATTR_UNUSED,
				enum smtp_server_state new_state,
				const char *new_args)
{
	struct client *client = context;

	i_free(client->state.args);

	client->state.state = new_state;
	client->state.args = i_strdup(new_args);

	if (submission_client_count == 1)
		submission_refresh_proctitle();
}

static const char *client_stats(struct client *client)
{
	const struct smtp_server_stats *stats =
		smtp_server_connection_get_stats(client->conn);
	const char *trans_id =
		smtp_server_connection_get_transaction_id(client->conn);
	const struct var_expand_table logout_tab[] = {
		{ .key = "input", .value = dec2str(stats->input) },
		{ .key = "output", .value = dec2str(stats->output) },
		{ .key = "command_count", .value = dec2str(stats->command_count) },
		{ .key = "reply_count", .value = dec2str(stats->reply_count) },
		{ .key = "transaction_id", .value = trans_id },
		VAR_EXPAND_TABLE_END
	};

	const struct var_expand_params *user_params =
		mail_user_var_expand_params(client->user);
	const struct var_expand_params params = {
		.tables_arr = (const struct var_expand_table*[]) {
			user_params->table,
			logout_tab,
			NULL
		},
		.providers = user_params->providers,
		.context =  user_params->context,
		.event = client->event,
	};

	string_t *str;
	const char *error;

	event_add_int(client->event, "net_in_bytes", stats->input);
	event_add_int(client->event, "net_out_bytes", stats->output);

	str = t_str_new(128);
	if (var_expand(str, client->set->submission_logout_format,
			   &params, &error) < 0) {
		e_error(client->event,
			"Failed to expand submission_logout_format=%s: %s",
			client->set->submission_logout_format, error);
	}

	return str_c(str);
}

static void client_connection_disconnect(void *context, const char *reason)
{
	struct client *client = context;
	const char *log_reason;

	if (client->disconnected)
		return;
	client->disconnected = TRUE;

	timeout_remove(&client->to_quit);
	submission_backends_destroy_all(client);

	if (array_is_created(&client->rcpt_to))
		array_clear(&client->rcpt_to);

	if (reason == NULL)
		log_reason = "Connection closed";
	else
		log_reason = t_str_oneline(reason);
	e_info(client->event, "Disconnected: %s %s",
	       log_reason, client_stats(client));
}

static void client_connection_free(void *context)
{
	struct client *client = context;

	client->v.destroy(client);
}

uoff_t client_get_max_mail_size(struct client *client)
{
	struct submission_backend *backend;
	uoff_t max_size, limit;

	/* Account for backend SIZE limits and calculate our own relative to
	   those. */
	max_size = client->set->submission_max_mail_size;
	if (max_size == 0)
		max_size = UOFF_T_MAX;
	for (backend = client->backends; backend != NULL;
	     backend = backend->next) {
		limit = submission_backend_get_max_mail_size(backend);

		if (limit <= SUBMISSION_MAX_ADDITIONAL_MAIL_SIZE)
			continue;
		limit -= SUBMISSION_MAX_ADDITIONAL_MAIL_SIZE;
		if (limit < max_size)
			max_size = limit;
	}

	return max_size;
}

void client_add_extra_capability(struct client *client, const char *capability,
				 const char *params)
{
	struct client_extra_capability cap;

	/* Don't add capabilities handled by lib-smtp here */
	i_assert(smtp_capability_find_by_name(capability)
		 == SMTP_CAPABILITY_NONE);

	/* Avoid committing protocol errors */
	i_assert(smtp_ehlo_keyword_is_valid(capability));
	i_assert(params == NULL || smtp_ehlo_params_str_is_valid(params));

	i_zero(&cap);
	cap.capability = p_strdup(client->pool, capability);
	cap.params = p_strdup(client->pool, params);

	if (!array_is_created(&client->extra_capabilities))
		p_array_init(&client->extra_capabilities, client->pool, 5);

	array_push_back(&client->extra_capabilities, &cap);
}

void client_kick(struct client *client, bool shutdown)
{
	mail_storage_service_io_activate_user(client->user->service_user);
	client_destroy(&client, "4.3.2", MASTER_SERVICE_SHUTTING_DOWN_MSG,
		       shutdown ? MASTER_SERVICE_SHUTTING_DOWN_MSG :
		       MASTER_SERVICE_USER_KICKED_MSG);
}

void clients_destroy_all(void)
{
	bool shutdown = !master_service_is_user_kicked(master_service);
	while (submission_clients != NULL)
		client_kick(submission_clients, shutdown);
}

static const struct smtp_server_callbacks smtp_callbacks = {
	.conn_cmd_helo = cmd_helo,

	.conn_cmd_mail = cmd_mail,
	.conn_cmd_rcpt = cmd_rcpt,
	.conn_cmd_rset = cmd_rset,

	.conn_cmd_data_begin = cmd_data_begin,
	.conn_cmd_data_continue = cmd_data_continue,

	.conn_cmd_vrfy = cmd_vrfy,

	.conn_cmd_noop = cmd_noop,
	.conn_cmd_quit = cmd_quit,

	.conn_cmd_input_pre = client_input_pre,
	.conn_cmd_input_post = client_input_post,

	.conn_trans_start = client_connection_trans_start,
	.conn_trans_free = client_connection_trans_free,

	.conn_state_changed = client_connection_state_changed,

	.conn_disconnect = client_connection_disconnect,
	.conn_free = client_connection_free,
};

static const struct submission_client_vfuncs submission_client_vfuncs = {
	client_default_destroy,

	.trans_start = client_default_trans_start,
	.trans_free = client_default_trans_free,

	.cmd_helo = client_default_cmd_helo,

	.cmd_mail = client_default_cmd_mail,
	.cmd_rcpt = client_default_cmd_rcpt,
	.cmd_rset = client_default_cmd_rset,
	.cmd_data = client_default_cmd_data,

	.cmd_vrfy = client_default_cmd_vrfy,

	.cmd_noop = client_default_cmd_noop,
	.cmd_quit = client_default_cmd_quit,
};
