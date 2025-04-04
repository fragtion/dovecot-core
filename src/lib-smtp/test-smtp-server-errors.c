/* Copyright (c) 2017-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "str.h"
#include "array.h"
#include "hostpid.h"
#include "ioloop.h"
#include "istream.h"
#include "ostream.h"
#include "time-util.h"
#include "sleep.h"
#include "connection.h"
#include "iostream-ssl.h"
#include "iostream-ssl-test.h"
#include "iostream-openssl.h"
#include "test-common.h"
#include "test-subprocess.h"
#include "settings.h"
#include "smtp-address.h"
#include "smtp-reply-parser.h"
#include "smtp-server.h"

#include <sys/signal.h>
#include <unistd.h>

#define VALGRIND_TIMEOUT_MULTIPLIER (ON_VALGRIND ? 5 : 1)

#define SERVER_MAX_TIMEOUT_MSECS (10 * VALGRIND_TIMEOUT_MULTIPLIER * 1000)
#define CLIENT_KILL_TIMEOUT_SECS (20 * VALGRIND_TIMEOUT_MULTIPLIER)

static void main_deinit(void);

/*
 * Types
 */

struct server_connection {
	struct smtp_server_connection *conn;
	void *context;
};

struct client_connection {
	struct connection conn;
	void *context;

	struct ssl_iostream *ssl_iostream;
	pool_t pool;
};

typedef void
(*test_server_init_t)(const struct smtp_server_settings *server_set);
typedef void (*test_client_init_t)(unsigned int index);

/*
 * State
 */

/* common */
static struct ip_addr bind_ip;
static in_port_t bind_port = 0;
static struct ioloop *ioloop;
const char *test_ssl_host = NULL;
static bool debug = FALSE;

/* server */
static struct smtp_server *smtp_server = NULL;
static struct io *io_listen;
static int fd_listen = -1;
static size_t server_io_buffer_size = 0;
static struct smtp_server_callbacks server_callbacks;
static unsigned int server_pending;
static bool test_server_delay_start = FALSE;

/* client */
static struct connection_list *client_conn_list;
struct ssl_iostream_context *client_ssl_ctx = NULL;
static unsigned int client_index;
static void (*test_client_connected)(struct client_connection *conn);
static void (*test_client_input)(struct client_connection *conn);
static void (*test_client_deinit)(struct client_connection *conn);

/*
 * Forward declarations
 */

/* server */
static void test_server_defaults(struct smtp_server_settings *smtp_set);
static void test_server_defaults_ssl(struct smtp_server_settings *smtp_set);
static void test_server_run(const struct smtp_server_settings *smtp_set);

/* client */
static void client_connection_deinit(struct client_connection **_conn);
static void test_client_run(unsigned int index);

/* test*/
static void
test_run_client_server(struct smtp_server_settings *server_set,
		       test_server_init_t server_test,
		       test_client_init_t client_test,
		       unsigned int client_tests_count, bool wait_for_clients);

/*
 * Slow server
 */

/* client */

static void test_slow_server_input(struct client_connection *conn ATTR_UNUSED)
{
	/* do nothing */
	sleep(10);
}

static void test_slow_server_connected(struct client_connection *conn)
{
	if (debug)
		i_debug("CONNECTED");

	o_stream_nsend_str(conn->conn.output,
		"EHLO frop\r\n");
}

static void test_client_slow_server(unsigned int index)
{
	test_client_input = test_slow_server_input;
	test_client_connected = test_slow_server_connected;
	test_client_run(index);
}

/* server */

struct _slow_server {
	struct smtp_server_cmd_ctx *cmd;
	struct timeout *to_delay;
	bool serviced:1;
};

static void
test_server_slow_server_destroyed(struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
				  struct _slow_server *ctx)
{
	test_assert(ctx->serviced);
	timeout_remove(&ctx->to_delay);
	i_free(ctx);
	io_loop_stop(ioloop);
}

static void test_server_slow_server_delayed(struct _slow_server *ctx)
{
	struct smtp_server_reply *reply;
	struct smtp_server_cmd_ctx *cmd = ctx->cmd;

	reply = smtp_server_reply_create_ehlo(cmd->cmd);
	smtp_server_reply_ehlo_add(reply, "FROP");

	smtp_server_reply_submit(reply);
	ctx->serviced = TRUE;
}

static int
test_server_slow_server_cmd_helo(void *conn_ctx ATTR_UNUSED,
				 struct smtp_server_cmd_ctx *cmd,
				 struct smtp_server_cmd_helo *data ATTR_UNUSED)
{
	struct _slow_server *ctx;

	if (debug)
		i_debug("HELO");

	ctx = i_new(struct _slow_server, 1);
	ctx->cmd = cmd;

	smtp_server_command_add_hook(cmd->cmd,
				     SMTP_SERVER_COMMAND_HOOK_DESTROY,
				     test_server_slow_server_destroyed, ctx);

	ctx->to_delay = timeout_add(4000, test_server_slow_server_delayed, ctx);

	return 0;
}

static void
test_server_slow_server(const struct smtp_server_settings *server_set)
{
	server_callbacks.conn_cmd_helo = test_server_slow_server_cmd_helo;
	test_server_run(server_set);
}

/* test */

static void test_slow_server(void)
{
	struct smtp_server_settings smtp_server_set;

	test_server_defaults(&smtp_server_set);
	smtp_server_set.max_client_idle_time_msecs = 1000;

	test_begin("slow server");
	test_run_client_server(&smtp_server_set,
			       test_server_slow_server,
			       test_client_slow_server, 1, FALSE);
	test_end();
}

/*
 * Slow client
 */

/* client */

static void test_slow_client_input(struct client_connection *conn ATTR_UNUSED)
{
	/* nothing */
}

static void test_slow_client_connected(struct client_connection *conn)
{
	if (debug)
		i_debug("CONNECTED");

	o_stream_nsend_str(conn->conn.output, "EHLO frop\r\n");
}

static void test_client_slow_client(unsigned int index)
{
	test_client_input = test_slow_client_input;
	test_client_connected = test_slow_client_connected;
	test_client_run(index);
}

/* server */

struct _slow_client {
	struct smtp_server_cmd_ctx *cmd;
	struct timeout *to_delay;
	struct timeout *to_disconnect;
	bool serviced:1;
};

static void test_server_slow_client_disconnect_timeout(struct _slow_client *ctx)
{
	test_assert(FALSE);

	timeout_remove(&ctx->to_disconnect);
	i_free(ctx);
	io_loop_stop(ioloop);
}

static void
test_server_slow_client_disconnect(void *conn_ctx, const char *reason)
{
	struct server_connection *conn = (struct server_connection *)conn_ctx;
	struct _slow_client *ctx = (struct _slow_client *)conn->context;

	if (debug)
		i_debug("DISCONNECTED: %s", reason);

	timeout_remove(&ctx->to_disconnect);
	i_free(ctx);
	io_loop_stop(ioloop);
}

static void
test_server_slow_client_cmd_destroyed(
	struct smtp_server_cmd_ctx *cmd ATTR_UNUSED, struct _slow_client *ctx)
{
	test_assert(ctx->serviced);
	timeout_remove(&ctx->to_delay);
}

static void test_server_slow_client_delayed(struct _slow_client *ctx)
{
	struct smtp_server_reply *reply;
	struct smtp_server_cmd_ctx *cmd = ctx->cmd;

	timeout_remove(&ctx->to_delay);

	reply = smtp_server_reply_create_ehlo(cmd->cmd);
	smtp_server_reply_ehlo_add(reply, "FROP");

	ctx->to_disconnect = timeout_add(
		2000, test_server_slow_client_disconnect_timeout, ctx);

	smtp_server_reply_submit(reply);
	ctx->serviced = TRUE;
}

static int
test_server_slow_client_cmd_helo(void *conn_ctx,
				 struct smtp_server_cmd_ctx *cmd,
				 struct smtp_server_cmd_helo *data ATTR_UNUSED)
{
	struct server_connection *conn = (struct server_connection *)conn_ctx;
	struct _slow_client *ctx;

	if (debug)
		i_debug("HELO");

	ctx = i_new(struct _slow_client, 1);
	ctx->cmd = cmd;

	conn->context = ctx;

	smtp_server_command_add_hook(cmd->cmd,
				     SMTP_SERVER_COMMAND_HOOK_DESTROY,
				     test_server_slow_client_cmd_destroyed,
				     ctx);

	ctx->to_delay = timeout_add_short(500,
		test_server_slow_client_delayed, ctx);

	return 0;
}

static void
test_server_slow_client(const struct smtp_server_settings *server_set)
{
	server_callbacks.conn_disconnect = test_server_slow_client_disconnect;
	server_callbacks.conn_cmd_helo = test_server_slow_client_cmd_helo;
	test_server_run(server_set);
}

/* test */

static void test_slow_client(void)
{
	struct smtp_server_settings smtp_server_set;

	test_server_defaults(&smtp_server_set);
	smtp_server_set.max_client_idle_time_msecs = 1000;

	test_begin("slow client");
	test_run_client_server(&smtp_server_set,
			       test_server_slow_client,
			       test_client_slow_client, 1, FALSE);
	test_end();
}

/*
 * Hanging command payload
 */

/* client */

static void
test_hanging_command_payload_connected(struct client_connection *conn)
{
	o_stream_nsend_str(conn->conn.output,
			   "EHLO frop\r\n"
			   "MAIL FROM:<hangman@example.com>\r\n"
			   "RCPT TO:<jerry@example.com>\r\n"
			   "DATA\r\n"
			   "To be continued... or not");
}

static void test_client_hanging_command_payload(unsigned int index)
{
	test_client_connected = test_hanging_command_payload_connected;
	test_client_run(index);
}

/* server */

struct _hanging_command_payload {
	struct istream *payload_input;
	struct io *io;

	bool serviced:1;
};

static void
test_server_hanging_command_payload_trans_free(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_transaction *trans)
{
	struct _hanging_command_payload *ctx =
		(struct _hanging_command_payload *)trans->context;

	test_assert(!ctx->serviced);
	i_free(ctx);
	io_loop_stop(ioloop);
}

static int
test_server_hanging_command_payload_rcpt(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
	struct smtp_server_recipient *rcpt)
{
	if (debug) {
		i_debug("RCPT TO:%s",
			smtp_address_encode(rcpt->path));
	}

	return 1;
}

static int
test_server_hanging_command_payload_data_begin(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
	struct smtp_server_transaction *trans, struct istream *data_input)
{
	struct _hanging_command_payload *ctx;

	if (debug)
		i_debug("DATA");

	ctx = i_new(struct _hanging_command_payload, 1);
	trans->context = ctx;

	ctx->payload_input = data_input;
	return 0;
}

static int
test_server_hanging_command_payload_data_continue(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd,
	struct smtp_server_transaction *trans)
{
	struct _hanging_command_payload *ctx =
		(struct _hanging_command_payload *)trans->context;
	const unsigned char *data;
	size_t size;
	int ret;


	if (debug)
		i_debug("DATA continue");

	while ((ret = i_stream_read_data(ctx->payload_input,
					 &data, &size, 0)) > 0)
		i_stream_skip(ctx->payload_input, size);

	if (ret == 0)
		return 0;
	if (ctx->payload_input->stream_errno != 0) {
		i_error("failed to read DATA payload: %s",
			i_stream_get_error(ctx->payload_input));
		return -1;
	}

	i_assert(ctx->payload_input->eof);

	smtp_server_reply(cmd, 250, "2.0.0", "OK");
	ctx->serviced = TRUE;

	i_stream_unref(&ctx->payload_input);
	return 1;
}

static void
test_server_hanging_command_payload(
	const struct smtp_server_settings *server_set)
{
	server_callbacks.conn_trans_free =
		test_server_hanging_command_payload_trans_free;

	server_callbacks.conn_cmd_rcpt =
		test_server_hanging_command_payload_rcpt;
	server_callbacks.conn_cmd_data_begin =
		test_server_hanging_command_payload_data_begin;
	server_callbacks.conn_cmd_data_continue =
		test_server_hanging_command_payload_data_continue;
	test_server_run(server_set);
}

/* test */

static void test_hanging_command_payload(void)
{
	struct smtp_server_settings smtp_server_set;

	test_server_defaults(&smtp_server_set);
	smtp_server_set.max_client_idle_time_msecs = 1000;

	test_begin("hanging command payload");
	test_run_client_server(&smtp_server_set,
			       test_server_hanging_command_payload,
			       test_client_hanging_command_payload, 1, FALSE);
	test_end();
}

/*
 * Bad command
 */

/* client */

static void
test_bad_command_connected(struct client_connection *conn)
{
	o_stream_nsend_str(conn->conn.output, "EHLO\tfrop\r\n");
}

static void test_client_bad_command(unsigned int index)
{
	test_client_connected = test_bad_command_connected;
	test_client_run(index);
}

/* server */

struct _bad_command {
	struct istream *payload_input;
	struct io *io;

	bool serviced:1;
};

static void
test_server_bad_command_disconnect(void *context ATTR_UNUSED,
				   const char *reason)
{
	if (debug)
		i_debug("Disconnect: %s", reason);
	io_loop_stop(ioloop);
}

static int
test_server_bad_command_helo(void *conn_ctx ATTR_UNUSED,
			     struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
			     struct smtp_server_cmd_helo *data ATTR_UNUSED)
{
	test_assert(FALSE);
	return 1;
}

static int
test_server_bad_command_rcpt(void *conn_ctx ATTR_UNUSED,
			     struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
			     struct smtp_server_recipient *rcpt ATTR_UNUSED)
{
	return 1;
}

static int
test_server_bad_command_data_begin(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd,
	struct smtp_server_transaction *trans ATTR_UNUSED,
	struct istream *data_input ATTR_UNUSED)
{
	smtp_server_reply(cmd, 250, "2.0.0", "OK");
	return 1;
}

static void
test_server_bad_command(const struct smtp_server_settings *server_set)
{
	server_callbacks.conn_disconnect =
		test_server_bad_command_disconnect;

	server_callbacks.conn_cmd_helo =
		test_server_bad_command_helo;
	server_callbacks.conn_cmd_rcpt =
		test_server_bad_command_rcpt;
	server_callbacks.conn_cmd_data_begin =
		test_server_bad_command_data_begin;
	test_server_run(server_set);
}

/* test */

static void test_bad_command(void)
{
	struct smtp_server_settings smtp_server_set;

	test_server_defaults(&smtp_server_set);
	smtp_server_set.max_client_idle_time_msecs = 1000;

	test_begin("bad command");
	test_run_client_server(&smtp_server_set,
			       test_server_bad_command,
			       test_client_bad_command, 1, FALSE);
	test_end();
}

/*
 * Many bad commands
 */

/* client */

struct _many_bad_commands_client {
	struct smtp_reply_parser *parser;
	unsigned int reply;
	bool replied:1;
};

static void
test_many_bad_commands_client_input(struct client_connection *conn)
{
	struct _many_bad_commands_client *ctx = conn->context;
	struct smtp_reply *reply;
	const char *error;
	int ret;

	while ((ret=smtp_reply_parse_next(ctx->parser, FALSE,
					  &reply, &error)) > 0) {
		if (debug)
			i_debug("REPLY #%u: %s", ctx->reply, smtp_reply_log(reply));

		switch (ctx->reply++) {
		/* greeting */
		case 0:
			i_assert(reply->status == 220);
			break;
		/* bad command reply */
		case 1: case 2: case 3: case 4: case 5:
		case 6: case 7: case 8: case 9: case 10:
			i_assert(reply->status == 500);
			break;
		case 11:
			i_assert(reply->status == 421);
			ctx->replied = TRUE;
			io_loop_stop(ioloop);
			connection_disconnect(&conn->conn);
			return;
		default:
			i_unreached();
		}
	}

	i_assert(ret >= 0);
}

static void
test_many_bad_commands_client_connected(struct client_connection *conn)
{
	struct _many_bad_commands_client *ctx;

	ctx = p_new(conn->pool, struct _many_bad_commands_client, 1);
	ctx->parser = smtp_reply_parser_init(conn->conn.input, SIZE_MAX);
	conn->context = ctx;

	switch (client_index) {
	case 0:
		o_stream_nsend_str(conn->conn.output,
				   "\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n");
		break;
	case 1:
		o_stream_nsend_str(conn->conn.output,
				   "a\r\nb\r\nc\r\nd\r\ne\r\nf\r\ng\r\nh\r\n"
				   "i\r\nj\r\nk\r\nl\r\nm\r\nn\r\no\r\np\r\n");
		break;
	default:
		i_unreached();
	}
}

static void
test_many_bad_commands_client_deinit(struct client_connection *conn)
{
	struct _many_bad_commands_client *ctx = conn->context;

	i_assert(ctx->replied);
	smtp_reply_parser_deinit(&ctx->parser);
}

static void test_client_many_bad_commands(unsigned int index)
{
	test_client_input = test_many_bad_commands_client_input;
	test_client_connected = test_many_bad_commands_client_connected;
	test_client_deinit = test_many_bad_commands_client_deinit;
	test_client_run(index);
}

/* server */

static void
test_server_many_bad_commands_disconnect(void *context ATTR_UNUSED,
					 const char *reason)
{
	struct server_connection *sconn = context;

	if (debug)
		i_debug("Disconnect: %s", reason);

	sconn->context = POINTER_CAST(POINTER_CAST_TO(sconn->context,
						      unsigned int) + 1);

	if (POINTER_CAST_TO(sconn->context, unsigned int) == 2)
		io_loop_stop(ioloop);
}

static int
test_server_many_bad_commands_helo(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
	struct smtp_server_cmd_helo *data ATTR_UNUSED)
{
	test_assert(FALSE);
	return 1;
}

static int
test_server_many_bad_commands_rcpt(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
	struct smtp_server_recipient *rcpt ATTR_UNUSED)
{
	test_assert(FALSE);
	return 1;
}

static int
test_server_many_bad_commands_data_begin(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
	struct smtp_server_transaction *trans ATTR_UNUSED,
	struct istream *data_input ATTR_UNUSED)
{
	test_assert(FALSE);
	return 1;
}

static void test_server_many_bad_commands
(const struct smtp_server_settings *server_set)
{
	server_callbacks.conn_disconnect =
		test_server_many_bad_commands_disconnect;

	server_callbacks.conn_cmd_helo =
		test_server_many_bad_commands_helo;
	server_callbacks.conn_cmd_rcpt =
		test_server_many_bad_commands_rcpt;
	server_callbacks.conn_cmd_data_begin =
		test_server_many_bad_commands_data_begin;
	test_server_run(server_set);
}

/* test */

static void test_many_bad_commands(void)
{
	struct smtp_server_settings smtp_server_set;

	test_server_defaults(&smtp_server_set);
	smtp_server_set.max_client_idle_time_msecs = 1000;
	smtp_server_set.max_bad_commands = 10;

	test_begin("many bad commands");
	test_run_client_server(&smtp_server_set,
		test_server_many_bad_commands,
		test_client_many_bad_commands, 2, FALSE);
	test_end();
}

/*
 * Long command
 */

/* client */

static void test_long_command_connected(struct client_connection *conn)
{
	o_stream_nsend_str(
		conn->conn.output,
		"EHLO some.very.very.very.very.very.long.domain\r\n");
}

static void test_client_long_command(unsigned int index)
{
	test_client_connected = test_long_command_connected;
	test_client_run(index);
}

/* server */

struct _long_command {
	struct istream *payload_input;
	struct io *io;

	bool serviced:1;
};

static void
test_server_long_command_disconnect(void *context ATTR_UNUSED,
				    const char *reason)
{
	if (debug)
		i_debug("Disconnect: %s", reason);
	io_loop_stop(ioloop);
}

static int
test_server_long_command_helo(void *conn_ctx ATTR_UNUSED,
			      struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
			      struct smtp_server_cmd_helo *data ATTR_UNUSED)
{
	test_assert(FALSE);
	return 1;
}

static int
test_server_long_command_rcpt(void *conn_ctx ATTR_UNUSED,
			      struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
			      struct smtp_server_recipient *rcpt ATTR_UNUSED)
{
	return 1;
}

static int
test_server_long_command_data_begin(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd,
	struct smtp_server_transaction *trans ATTR_UNUSED,
	struct istream *data_input ATTR_UNUSED)
{
	smtp_server_reply(cmd, 250, "2.0.0", "OK");
	return 1;
}

static void
test_server_long_command(const struct smtp_server_settings *server_set)
{
	server_callbacks.conn_disconnect =
		test_server_long_command_disconnect;

	server_callbacks.conn_cmd_helo =
		test_server_long_command_helo;
	server_callbacks.conn_cmd_rcpt =
		test_server_long_command_rcpt;
	server_callbacks.conn_cmd_data_begin =
		test_server_long_command_data_begin;
	test_server_run(server_set);
}

/* test */

static void test_long_command(void)
{
	struct smtp_server_settings smtp_server_set;

	test_server_defaults(&smtp_server_set);
	smtp_server_set.max_client_idle_time_msecs = 1000;
	smtp_server_set.command_limits.max_parameters_size = 32;

	test_begin("long command");
	test_run_client_server(&smtp_server_set,
			       test_server_long_command,
			       test_client_long_command, 1, FALSE);
	test_end();
}

/*
 * Long auth line
 */

/* client */

#define _LONG_AUTH_LINE_DATA \
	"dXNlcj10ZXN0dXNlcjEBYXV0aD1CZWFyZXIgZXlKaGJHY2lPaUpTVXpJMU5pSXNJ" \
	"blI1Y0NJZ09pQWlTbGRVSWl3aWEybGtJaUE2SUNKdVRIRlVlRnBXWVhKSlgwWndS" \
	"a0Z3Umt3MloyUnhiak4xV1VSS2R6WnNWVjlMYVZoa2JWazJialpSSW4wLmV5Smxl" \
	"SEFpT2pFMk16UTJNemMyTlRFc0ltbGhkQ0k2TVRZek5EWXpOek0xTVN3aWFuUnBJ" \
	"am9pT1RFM1lUYzFaalF0WTJZME9DMDBOVEEyTFRnNVpXSXRNRE13WldaaU5tSTVO" \
	"MlZrSWl3aWFYTnpJam9pYUhSMGNEb3ZMekU1TWk0eE5qZ3VNUzR5TVRveE9EQTRN" \
	"QzloZFhSb0wzSmxZV3h0Y3k5eVpXeDBaWE4wSWl3aVlYVmtJam9pWVdOamIzVnVk" \
	"Q0lzSW5OMVlpSTZJamhsWVRRME1UWTNMVGN6TTJVdE5EVTBZeTFpT0dJMUxXTmpa" \
	"bVl3WkRnek1URTVaQ0lzSW5SNWNDSTZJa0psWVhKbGNpSXNJbUY2Y0NJNkltUnZk" \
	"bVZqYjNRaUxDSnpaWE56YVc5dVgzTjBZWFJsSWpvaU1tTTNPVEUzWldJdE16QTFO" \
	"UzAwTkRZeExXSXdZell0WTJVeFlUbGlNVEZoTWpReklpd2lZV055SWpvaU1TSXNJ" \
	"bkpsWVd4dFgyRmpZMlZ6Y3lJNmV5SnliMnhsY3lJNld5SnZabVpzYVc1bFgyRmpZ" \
	"MlZ6Y3lJc0luVnRZVjloZFhSb2IzSnBlbUYwYVc5dUlsMTlMQ0p5WlhOdmRYSmpa" \
	"VjloWTJObGMzTWlPbnNpWVdOamIzVnVkQ0k2ZXlKeWIyeGxjeUk2V3lKdFlXNWha" \
	"MlV0WVdOamIzVnVkQ0lzSW0xaGJtRm5aUzFoWTJOdmRXNTBMV3hwYm10eklpd2lk" \
	"bWxsZHkxd2NtOW1hV3hsSWwxOWZTd2ljMk52Y0dVaU9pSndjbTltYVd4bElHVnRZ" \
	"V2xzSWl3aVpXMWhhV3hmZG1WeWFXWnBaV1FpT21aaGJITmxMQ0p1WVcxbElqb2lk" \
	"R1Z6ZEhWelpYSXhJRUYxZEc5SFpXNWxjbUYwWldRaUxDSndjbVZtWlhKeVpXUmZk" \
	"WE5sY201aGJXVWlPaUowWlhOMGRYTmxjakVpTENKbmFYWmxibDl1WVcxbElqb2lk" \
	"R1Z6ZEhWelpYSXhJaXdpWm1GdGFXeDVYMjVoYldVaU9pSkJkWFJ2UjJWdVpYSmhk" \
	"R1ZrSWl3aVpXMWhhV3dpT2lKMFpYTjBkWE5sY2pGQWJYbGtiMjFoYVc0dWIzZ2lm" \
	"US5ta2JGSURpT0FhbENCcVMwODRhVHJURjBIdDk1c1Z4cGlSbTFqZnhJd0JiN1hM" \
	"M2gzWUJkdXVrVXlZdDJqX1pqUFlhMDhDcVVYNWFrLVBOSjdSVWRTUXNmUlgwM1Zi" \
	"cXA4MHFZZjNGYzJpcDR0YmhHLXFEV0R6NzdhZDhWcEFNei16YWlSamZCclZ2R3hB" \
	"T3ZsZnFDVWhaZTJDR3ZqWjZ1Q3RKTlFaS0dyazZHOXoxX2pqekZkTjBXWjUxbEZs" \
	"US1JdE5LREpoTjNIekJ5SW93M19qQU9kWEI0R0w4R3JHM1hqU09rSFVRam5GTEQw" \
	"QUF1QXY4SkxmTXY1NGc1a2tKaklxRFgxZlgyWVo0Y2JQOWV3TUp6UV84ZWdLeW5T" \
	"VV9XSk8xRU9Qa1NVZjlMX19RX3FwY0dNbzFtTkxuTURKUlU2dmZFY3JrM2k0cVNz" \
	"MXRPdHdLaHcBAQ"

struct _long_auth_line_client {
	struct smtp_reply_parser *parser;
	unsigned int reply;

	bool replied:1;
};

static void test_long_auth_line_client_input(struct client_connection *conn)
{
	struct _long_auth_line_client *ctx = conn->context;
	struct smtp_reply *reply;
	const char *error;
	int ret;

	while ((ret = smtp_reply_parse_next(ctx->parser, FALSE,
					    &reply, &error)) > 0) {
		if (debug)
			i_debug("REPLY: %s", smtp_reply_log(reply));

		switch (ctx->reply++) {
		case 0: /* greeting */
			i_assert(reply->status == 220);
			break;
		case 1: /* EHLO reply */
			i_assert(reply->status == 250);
			break;
		case 2: /* AUTH continue */
			i_assert(reply->status == 334);
			break;
		case 3: /* AUTH reply */
			switch (client_index) {
			case 0:
				i_assert(reply->status == 235);
				break;
			case 1:
				i_assert(reply->status == 235);
				break;
			case 2:
				i_assert(reply->status == 500);
				ctx->replied = TRUE;
				io_loop_stop(ioloop);
				connection_disconnect(&conn->conn);
				return;
			default:
				i_unreached();
			}
			break;
		case 4: /* MAIL reply */
			i_assert(client_index < 2);
			i_assert(reply->status == 250);
			break;
		case 5: /* RCPT reply */
			i_assert(client_index < 2);
			i_assert(reply->status == 250);
			break;
		case 6: /* DATA initial reply */
			i_assert(client_index < 2);
			i_assert(reply->status == 354);
			break;
		case 7: /* DATA reply */
			i_assert(client_index < 2);
			i_assert(reply->status == 250);
			break;
		case 8: /* QUIT reply */
			i_assert(client_index < 2);
			i_assert(reply->status == 221);
			ctx->replied = TRUE;
			io_loop_stop(ioloop);
			connection_disconnect(&conn->conn);
			return;
		default:
			i_unreached();
		}
	}

	i_assert(ret >= 0);
}

static void test_long_auth_line_client_connected(struct client_connection *conn)
{
	struct _long_auth_line_client *ctx;
	unsigned int i;

	ctx = p_new(conn->pool, struct _long_auth_line_client, 1);
	ctx->parser = smtp_reply_parser_init(conn->conn.input, SIZE_MAX);
	conn->context = ctx;

	o_stream_nsend_str(
		conn->conn.output,
		"EHLO frop\r\n"
		"AUTH XOAUTH2\r\n");
	for (i = 0; i < (client_index > 1 ? 6 : 1); i++)
		o_stream_nsend_str(conn->conn.output, _LONG_AUTH_LINE_DATA);
	o_stream_nsend_str(
		conn->conn.output,
		"==");
	if (client_index == 1) {
		o_stream_nsend_str(
			conn->conn.output,
			"                                      ");
	}
	o_stream_nsend_str(
		conn->conn.output,
		"\r\n"
		"MAIL FROM:<user@example.com>\r\n"
		"RCPT TO:<user@example.com>\r\n"
		"DATA\r\n"
		"frop\r\n"
		".\r\n"
		"QUIT\r\n");
}

static void test_long_auth_line_client_deinit(struct client_connection *conn)
{
	struct _long_auth_line_client *ctx = conn->context;

	i_assert(ctx->replied);
	smtp_reply_parser_deinit(&ctx->parser);
}

static void test_client_long_auth_line(unsigned int index)
{
	test_client_input = test_long_auth_line_client_input;
	test_client_connected = test_long_auth_line_client_connected;
	test_client_deinit = test_long_auth_line_client_deinit;
	test_client_run(index);
}

/* server */

struct _long_auth_line {
	struct istream *payload_input;
};

static void
test_server_long_auth_line_disconnect(void *context ATTR_UNUSED,
				    const char *reason)
{
	if (debug)
		i_debug("Disconnect: %s", reason);
}

static int
test_server_long_auth_line_helo(void *conn_ctx ATTR_UNUSED,
			      struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
			      struct smtp_server_cmd_helo *data ATTR_UNUSED)
{
	return 1;
}

static int
test_server_long_auth_line_auth(void *conn_ctx ATTR_UNUSED,
				struct smtp_server_cmd_ctx *cmd,
				struct smtp_server_cmd_auth *data ATTR_UNUSED)
{
	smtp_server_cmd_auth_send_challenge(cmd, "");
	return 0;
}

static int
test_server_long_auth_line_auth_continue(void *conn_ctx ATTR_UNUSED,
					 struct smtp_server_cmd_ctx *cmd,
					 const char *response)
{
	if (strcmp(response, _LONG_AUTH_LINE_DATA"==") == 0)
		smtp_server_cmd_auth_success(cmd, "user", NULL);
	else {
		smtp_server_reply(cmd, 535, "5.7.8",
				  "Authentication credentials invalid");
	}
	return 1;
}

static int
test_server_long_auth_line_rcpt(void *conn_ctx ATTR_UNUSED,
			      struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
			      struct smtp_server_recipient *rcpt ATTR_UNUSED)
{
	if (debug)
		i_debug("RCPT");
	return 1;
}

static int
test_server_long_auth_line_data_begin(void *conn_ctx ATTR_UNUSED,
				struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
				struct smtp_server_transaction *trans,
				struct istream *data_input)
{
	struct _long_auth_line *ctx;

	if (debug)
		i_debug("DATA");

	ctx = p_new(trans->pool, struct _long_auth_line, 1);
	trans->context = ctx;

	ctx->payload_input = data_input;
	return 0;
}

static int
test_server_long_auth_line_data_continue(void *conn_ctx ATTR_UNUSED,
				   struct smtp_server_cmd_ctx *cmd,
				   struct smtp_server_transaction *trans)
{
	struct _long_auth_line *ctx = (struct _long_auth_line *)trans->context;
	struct istream *data_input = ctx->payload_input;
	size_t size;
	ssize_t ret;

	if (debug)
		i_debug("DATA continue");

	while ((ret = i_stream_read(data_input)) > 0 || ret == -2) {
		(void)i_stream_get_data(data_input, &size);
		i_stream_skip(data_input, size);
		if (!smtp_server_cmd_data_check_size(cmd))
			return -1;
	}

	if (ret == 0)
		return 0;
	if (ret < 0 && data_input->stream_errno != 0) {
		/* Client probably disconnected */
		return -1;
	}

	smtp_server_reply_all(cmd, 250, "2.0.0", "Accepted");
	return 1;
}

static void
test_server_long_auth_line(const struct smtp_server_settings *server_set)
{
	server_callbacks.conn_disconnect =
		test_server_long_auth_line_disconnect;

	server_callbacks.conn_cmd_helo =
		test_server_long_auth_line_helo;
	server_callbacks.conn_cmd_auth =
		test_server_long_auth_line_auth;
	server_callbacks.conn_cmd_auth_continue =
		test_server_long_auth_line_auth_continue;
	server_callbacks.conn_cmd_rcpt =
		test_server_long_auth_line_rcpt;
	server_callbacks.conn_cmd_data_begin =
		test_server_long_auth_line_data_begin;
	server_callbacks.conn_cmd_data_continue =
		test_server_long_auth_line_data_continue;
	test_server_run(server_set);
}

static void
test_server_long_auth_line_small_buf(
	const struct smtp_server_settings *server_set)
{
	server_io_buffer_size = 1024;

	server_callbacks.conn_disconnect =
		test_server_long_auth_line_disconnect;

	server_callbacks.conn_cmd_helo =
		test_server_long_auth_line_helo;
	server_callbacks.conn_cmd_auth =
		test_server_long_auth_line_auth;
	server_callbacks.conn_cmd_auth_continue =
		test_server_long_auth_line_auth_continue;
	server_callbacks.conn_cmd_rcpt =
		test_server_long_auth_line_rcpt;
	server_callbacks.conn_cmd_data_begin =
		test_server_long_auth_line_data_begin;
	server_callbacks.conn_cmd_data_continue =
		test_server_long_auth_line_data_continue;
	test_server_run(server_set);
}

/* test */

static void test_long_auth_line(void)
{
	struct smtp_server_settings smtp_server_set;

	test_server_defaults(&smtp_server_set);
	smtp_server_set.capabilities = SMTP_CAPABILITY_AUTH;
	smtp_server_set.max_client_idle_time_msecs = 1000;

	test_begin("long auth line");
	test_run_client_server(&smtp_server_set,
			       test_server_long_auth_line,
			       test_client_long_auth_line, 3, TRUE);
	test_end();
}

static void test_long_auth_line_small_buf(void)
{
	struct smtp_server_settings smtp_server_set;

	test_server_defaults(&smtp_server_set);
	smtp_server_set.capabilities = SMTP_CAPABILITY_AUTH;
	smtp_server_set.max_client_idle_time_msecs = 1000;

	test_begin("long auth line (small i/o buffers)");
	test_run_client_server(&smtp_server_set,
			       test_server_long_auth_line_small_buf,
			       test_client_long_auth_line, 3, TRUE);
	test_end();
}


/*
 * Big data
 */

/* client */

static void
test_big_data_connected(struct client_connection *conn)
{
	o_stream_nsend_str(conn->conn.output,
			   "EHLO frop\r\n"
			   "MAIL FROM:<sender@example.com>\r\n"
			   "RCPT TO:<recipient@example.com>\r\n"
			   "DATA\r\n"
			   "0123456789ABCDEF0123456789ABCDEF\r\n"
			   "0123456789ABCDEF0123456789ABCDEF\r\n"
			   "0123456789ABCDEF0123456789ABCDEF\r\n"
			   "0123456789ABCDEF0123456789ABCDEF\r\n"
			   "0123456789ABCDEF0123456789ABCDEF\r\n"
			   "0123456789ABCDEF0123456789ABCDEF\r\n"
			   "0123456789ABCDEF0123456789ABCDEF\r\n"
			   ".\r\n");
}

static void test_client_big_data(unsigned int index)
{
	test_client_connected = test_big_data_connected;
	test_client_run(index);
}

/* server */

struct _big_data {
	struct istream *payload_input;
	struct io *io;
};

static void
test_server_big_data_trans_free(void *conn_ctx  ATTR_UNUSED,
				struct smtp_server_transaction *trans)
{
	struct _big_data *ctx = (struct _big_data *)trans->context;

	i_free(ctx);
	io_loop_stop(ioloop);
}

static int
test_server_big_data_rcpt(void *conn_ctx ATTR_UNUSED,
			  struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
			  struct smtp_server_recipient *rcpt)
{
	if (debug) {
		i_debug("RCPT TO:%s",
			smtp_address_encode(rcpt->path));
	}
	return 1;
}

static int
test_server_big_data_data_begin(void *conn_ctx ATTR_UNUSED,
				struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
				struct smtp_server_transaction *trans,
				struct istream *data_input)
{
	struct _big_data *ctx;

	if (debug)
		i_debug("DATA");

	ctx = i_new(struct _big_data, 1);
	trans->context = ctx;

	ctx->payload_input = data_input;
	return 0;
}

static int
test_server_big_data_data_continue(void *conn_ctx ATTR_UNUSED,
				   struct smtp_server_cmd_ctx *cmd,
				   struct smtp_server_transaction *trans)
{
	static const size_t max_size = 32;
	struct _big_data *ctx =	(struct _big_data *)trans->context;
	const unsigned char *data;
	size_t size;
	int ret;


	if (debug)
		i_debug("DATA continue");

	while (ctx->payload_input->v_offset < max_size &&
	       (ret = i_stream_read_data(ctx->payload_input,
					 &data, &size, 0)) > 0) {
		if (ctx->payload_input->v_offset + size > max_size) {
			if (ctx->payload_input->v_offset >= max_size)
				size = 0;
			else
				size = max_size - ctx->payload_input->v_offset;
		}
		i_stream_skip(ctx->payload_input, size);

		if (ctx->payload_input->v_offset >= max_size)
			break;
	}

	if (ctx->payload_input->v_offset >= max_size) {
		smtp_server_reply_early(cmd, 552, "5.3.4",
					"Message too big for system");
		return -1;
	}

	if (ret == 0)
		return 0;

	test_assert(FALSE);
	return 1;
}

static void test_server_big_data(const struct smtp_server_settings *server_set)
{
	server_callbacks.conn_trans_free =
		test_server_big_data_trans_free;

	server_callbacks.conn_cmd_rcpt =
		test_server_big_data_rcpt;
	server_callbacks.conn_cmd_data_begin =
		test_server_big_data_data_begin;
	server_callbacks.conn_cmd_data_continue =
		test_server_big_data_data_continue;
	test_server_run(server_set);
}

/* test */

static void test_big_data(void)
{
	struct smtp_server_settings smtp_server_set;

	test_server_defaults(&smtp_server_set);
	smtp_server_set.max_client_idle_time_msecs = 1000;
	smtp_server_set.command_limits.max_data_size = 64;

	test_begin("big_data");
	test_run_client_server(&smtp_server_set,
			       test_server_big_data,
			       test_client_big_data, 1, FALSE);
	test_end();
}

/*
 * Bad HELO
 */

/* client */

struct _bad_helo_client {
	struct smtp_reply_parser *parser;
	unsigned int reply;

	bool replied:1;
};

static void test_bad_helo_client_input(struct client_connection *conn)
{
	struct _bad_helo_client *ctx = conn->context;
	struct smtp_reply *reply;
	const char *error;
	int ret;

	for (;;) {
		if (ctx->reply != 1 ||
		    client_index == 0 || client_index == 2) {
			ret = smtp_reply_parse_next(ctx->parser, FALSE, &reply,
						    &error);
		} else {
			ret = smtp_reply_parse_ehlo(ctx->parser, &reply,
						    &error);
		}
		if (ret <= 0)
			break;

		if (debug)
			i_debug("REPLY: %s", smtp_reply_log(reply));

		switch (ctx->reply++) {
		case 0: /* greeting */
			i_assert(reply->status == 220);
			break;
		case 1: /* bad command reply */
			switch (client_index) {
			case 0: case 1:
				i_assert(reply->status == 501);
				break;
			case 2: case 3:
				i_assert(reply->status == 250);
				break;
			default:
				i_unreached();
			}
			if (debug)
				i_debug("REPLIED");
			ctx->replied = TRUE;
			io_loop_stop(ioloop);
			connection_disconnect(&conn->conn);
			return;
		default:
			i_unreached();
		}
	}

	i_assert(ret >= 0);
}

static void test_bad_helo_client_connected(struct client_connection *conn)
{
	struct _bad_helo_client *ctx;

	ctx = p_new(conn->pool, struct _bad_helo_client, 1);
	ctx->parser = smtp_reply_parser_init(conn->conn.input, SIZE_MAX);
	conn->context = ctx;

	switch (client_index) {
	case 0:
		o_stream_nsend_str(conn->conn.output, "HELO\r\n");
		break;
	case 1:
		o_stream_nsend_str(conn->conn.output, "EHLO\r\n");
		break;
	case 2:
		o_stream_nsend_str(conn->conn.output, "HELO frop\r\n");
		break;
	case 3:
		o_stream_nsend_str(conn->conn.output, "EHLO frop\r\n");
		break;
	default:
		i_unreached();
	}
}

static void test_bad_helo_client_deinit(struct client_connection *conn)
{
	struct _bad_helo_client *ctx = conn->context;

	i_assert(ctx->replied);
	smtp_reply_parser_deinit(&ctx->parser);
}

static void test_client_bad_helo(unsigned int index)
{
	test_client_input = test_bad_helo_client_input;
	test_client_connected = test_bad_helo_client_connected;
	test_client_deinit = test_bad_helo_client_deinit;
	test_client_run(index);
}

/* server */

struct _bad_helo {
	struct istream *payload_input;
	struct io *io;

	bool serviced:1;
};

static void
test_server_bad_helo_disconnect(void *context ATTR_UNUSED, const char *reason)
{
	if (debug)
		i_debug("Disconnect: %s", reason);
}

static int
test_server_bad_helo_helo(void *conn_ctx ATTR_UNUSED,
			  struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
			  struct smtp_server_cmd_helo *data ATTR_UNUSED)
{
	return 1;
}

static int
test_server_bad_helo_rcpt(void *conn_ctx ATTR_UNUSED,
			  struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
			  struct smtp_server_recipient *rcpt ATTR_UNUSED)
{
	return 1;
}

static int
test_server_bad_helo_data_begin(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd,
	struct smtp_server_transaction *trans ATTR_UNUSED,
	struct istream *data_input ATTR_UNUSED)
{
	smtp_server_reply(cmd, 250, "2.0.0", "OK");
	return 1;
}

static void test_server_bad_helo(const struct smtp_server_settings *server_set)
{
	server_callbacks.conn_disconnect =
		test_server_bad_helo_disconnect;

	server_callbacks.conn_cmd_helo =
		test_server_bad_helo_helo;
	server_callbacks.conn_cmd_rcpt =
		test_server_bad_helo_rcpt;
	server_callbacks.conn_cmd_data_begin =
		test_server_bad_helo_data_begin;
	test_server_run(server_set);
}

/* test */

static void test_bad_helo(void)
{
	struct smtp_server_settings smtp_server_set;

	test_server_defaults(&smtp_server_set);
	smtp_server_set.max_client_idle_time_msecs = 1000;

	test_begin("bad HELO");
	test_run_client_server(&smtp_server_set,
			       test_server_bad_helo,
			       test_client_bad_helo, 4, TRUE);
	test_end();
}

/*
 * Bad MAIL
 */

/* client */

struct _bad_mail_client {
	struct smtp_reply_parser *parser;
	unsigned int reply;

	bool replied:1;
};

static void test_bad_mail_client_input(struct client_connection *conn)
{
	struct _bad_mail_client *ctx = conn->context;
	struct smtp_reply *reply;
	const char *error;
	int ret;

	while ((ret = smtp_reply_parse_next(ctx->parser, FALSE,
					    &reply, &error)) > 0) {
		if (debug)
			i_debug("REPLY: %s", smtp_reply_log(reply));

		switch (ctx->reply++) {
		case 0: /* greeting */
			i_assert(reply->status == 220);
			break;
		case 1: /* bad command reply */
			switch (client_index) {
			case 0: case 1: case 2: case 3: case 4: case 5: case 6:
				i_assert(reply->status == 501);
				break;
			case 7: case 8:
				i_assert(reply->status == 250);
				break;
			default:
				i_unreached();
			}
			ctx->replied = TRUE;
			io_loop_stop(ioloop);
			connection_disconnect(&conn->conn);
			return;
		default:
			i_unreached();
		}
	}

	i_assert(ret >= 0);
}

static void test_bad_mail_client_connected(struct client_connection *conn)
{
	struct _bad_mail_client *ctx;

	ctx = p_new(conn->pool, struct _bad_mail_client, 1);
	ctx->parser = smtp_reply_parser_init(conn->conn.input, SIZE_MAX);
	conn->context = ctx;

	switch (client_index) {
	case 0:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM: <hendrik@example.com>\r\n");
		break;
	case 1:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:hendrik@example.com\r\n");
		break;
	case 2:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM: hendrik@example.com\r\n");
		break;
	case 3:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:\r\n");
		break;
	case 4:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM: \r\n");
		break;
	case 5:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM: BODY=7BIT\r\n");
		break;
	case 6:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM: <>\r\n");
		break;
	case 7:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:<hendrik@example.com>\r\n");
		break;
	case 8:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:<>\r\n");
		break;
	default:
		i_unreached();
	}
}

static void test_bad_mail_client_deinit(struct client_connection *conn)
{
	struct _bad_mail_client *ctx = conn->context;

	i_assert(ctx->replied);
	smtp_reply_parser_deinit(&ctx->parser);
}

static void test_client_bad_mail(unsigned int index)
{
	test_client_input = test_bad_mail_client_input;
	test_client_connected = test_bad_mail_client_connected;
	test_client_deinit = test_bad_mail_client_deinit;
	test_client_run(index);
}

/* server */

static void
test_server_bad_mail_disconnect(void *context ATTR_UNUSED, const char *reason)
{
	if (debug)
		i_debug("Disconnect: %s", reason);
}

static int
test_server_bad_mail_rcpt(void *conn_ctx ATTR_UNUSED,
			  struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
			  struct smtp_server_recipient *rcpt ATTR_UNUSED)
{
	test_assert(FALSE);
	return 1;
}

static int
test_server_bad_mail_data_begin(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
	struct smtp_server_transaction *trans ATTR_UNUSED,
	struct istream *data_input ATTR_UNUSED)
{
	test_assert(FALSE);
	return 1;
}

static void test_server_bad_mail(const struct smtp_server_settings *server_set)
{
	server_callbacks.conn_disconnect =
		test_server_bad_mail_disconnect;

	server_callbacks.conn_cmd_rcpt =
		test_server_bad_mail_rcpt;
	server_callbacks.conn_cmd_data_begin =
		test_server_bad_mail_data_begin;
	test_server_run(server_set);
}

/* test */

static void test_bad_mail(void)
{
	struct smtp_server_settings smtp_server_set;

	test_server_defaults(&smtp_server_set);
	smtp_server_set.max_client_idle_time_msecs = 1000;

	test_begin("bad MAIL");
	test_run_client_server(&smtp_server_set,
			       test_server_bad_mail,
			       test_client_bad_mail, 9, TRUE);
	test_end();
}

/*
 * Bad RCPT
 */

/* client */

struct _bad_rcpt_client {
	struct smtp_reply_parser *parser;
	unsigned int reply;

	bool replied:1;
};

static void
test_bad_rcpt_client_input(struct client_connection *conn)
{
	struct _bad_rcpt_client *ctx = conn->context;
	struct smtp_reply *reply;
	const char *error;
	int ret;

	while ((ret = smtp_reply_parse_next(ctx->parser, FALSE,
					    &reply, &error)) > 0) {
		if (debug)
			i_debug("REPLY: %s", smtp_reply_log(reply));

		switch (ctx->reply++) {
		case 0: /* greeting */
			i_assert(reply->status == 220);
			break;
		case 1: /* MAIL FROM */
			i_assert(reply->status == 250);
			break;
		case 2: /* bad command reply */
			switch (client_index) {
			case 0: case 1: case 2: case 3: case 4: case 5:
				i_assert(reply->status == 501);
				break;
			case 6:
				i_assert(reply->status == 250);
				break;
			default:
				i_unreached();
			}
			ctx->replied = TRUE;
			io_loop_stop(ioloop);
			connection_disconnect(&conn->conn);
			return;
		default:
			i_unreached();
		}
	}

	i_assert(ret >= 0);
}

static void test_bad_rcpt_client_connected(struct client_connection *conn)
{
	struct _bad_rcpt_client *ctx;

	ctx = p_new(conn->pool, struct _bad_rcpt_client, 1);
	ctx->parser = smtp_reply_parser_init(conn->conn.input, SIZE_MAX);
	conn->context = ctx;

	switch (client_index) {
	case 0:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:<hendrik@example.com>\r\n"
			"RCPT TO: <harrie@example.com>\r\n");
		break;
	case 1:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:<hendrik@example.com>\r\n"
			"RCPT TO:harrie@example.com\r\n");
		break;
	case 2:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:<hendrik@example.com>\r\n"
			"RCPT TO: harrie@example.com\r\n");
		break;
	case 3:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:<hendrik@example.com>\r\n"
			"RCPT TO:\r\n");
		break;
	case 4:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:<hendrik@example.com>\r\n"
			"RCPT TO: \r\n");
		break;
	case 5:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:<hendrik@example.com>\r\n"
			"RCPT TO: NOTIFY=NEVER\r\n");
		break;
	case 6:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:<hendrik@example.com>\r\n"
			"RCPT TO:<harrie@example.com>\r\n");
		break;
	default:
		i_unreached();
	}
}

static void test_bad_rcpt_client_deinit(struct client_connection *conn)
{
	struct _bad_rcpt_client *ctx = conn->context;

	i_assert(ctx->replied);
	smtp_reply_parser_deinit(&ctx->parser);
}

static void test_client_bad_rcpt(unsigned int index)
{
	test_client_input = test_bad_rcpt_client_input;
	test_client_connected = test_bad_rcpt_client_connected;
	test_client_deinit = test_bad_rcpt_client_deinit;
	test_client_run(index);
}

/* server */

static void
test_server_bad_rcpt_disconnect(void *context ATTR_UNUSED, const char *reason)
{
	if (debug)
		i_debug("Disconnect: %s", reason);
}

static int
test_server_bad_rcpt_rcpt(void *conn_ctx ATTR_UNUSED,
			  struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
			  struct smtp_server_recipient *rcpt ATTR_UNUSED)
{
	return 1;
}

static int
test_server_bad_rcpt_data_begin(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
	struct smtp_server_transaction *trans ATTR_UNUSED,
	struct istream *data_input ATTR_UNUSED)
{
	test_assert(FALSE);
	return 1;
}

static void test_server_bad_rcpt(const struct smtp_server_settings *server_set)
{
	server_callbacks.conn_disconnect =
		test_server_bad_rcpt_disconnect;

	server_callbacks.conn_cmd_rcpt =
		test_server_bad_rcpt_rcpt;
	server_callbacks.conn_cmd_data_begin =
		test_server_bad_rcpt_data_begin;
	test_server_run(server_set);
}

/* test */

static void test_bad_rcpt(void)
{
	struct smtp_server_settings smtp_server_set;

	test_server_defaults(&smtp_server_set);
	smtp_server_set.max_client_idle_time_msecs = 1000;

	test_begin("bad RCPT");
	test_run_client_server(&smtp_server_set,
			       test_server_bad_rcpt,
			       test_client_bad_rcpt, 7, TRUE);
	test_end();
}

/*
 * Bad VRFY
 */

/* client */

struct _bad_vrfy_client {
	struct smtp_reply_parser *parser;
	unsigned int reply;

	bool replied:1;
};

static void
test_bad_vrfy_client_input(struct client_connection *conn)
{
	struct _bad_vrfy_client *ctx = conn->context;
	struct smtp_reply *reply;
	const char *error;
	int ret;

	while ((ret = smtp_reply_parse_next(ctx->parser, FALSE,
					    &reply, &error)) > 0) {
		if (debug)
			i_debug("REPLY: %s", smtp_reply_log(reply));

		switch (ctx->reply++) {
		case 0: /* greeting */
			i_assert(reply->status == 220);
			break;
		case 1: /* bad command reply */
			switch (client_index) {
			case 0: case 1: case 2:
				i_assert(reply->status == 501);
				break;
			case 3:
				i_assert(smtp_reply_is_success(reply));
				break;
			default:
				i_unreached();
			}
			ctx->replied = TRUE;
			io_loop_stop(ioloop);
			connection_disconnect(&conn->conn);
			return;
		default:
			i_unreached();
		}
	}

	i_assert(ret == 0);
}

static void
test_bad_vrfy_client_connected(struct client_connection *conn)
{
	struct _bad_vrfy_client *ctx;

	ctx = p_new(conn->pool, struct _bad_vrfy_client, 1);
	ctx->parser = smtp_reply_parser_init(conn->conn.input, SIZE_MAX);
	conn->context = ctx;

	switch (client_index) {
	case 0:
		o_stream_nsend_str(conn->conn.output,
				   "VRFY\r\n");
		break;
	case 1:
		o_stream_nsend_str(conn->conn.output,
				   "VRFY \"hendrik\r\n");
		break;
	case 2:
		o_stream_nsend_str(conn->conn.output,
				   "VRFY hen\"drik\r\n");
		break;
	case 3:
		o_stream_nsend_str(conn->conn.output,
				   "VRFY \"hendrik\"\r\n");
		break;
	default:
		i_unreached();
	}
}

static void
test_bad_vrfy_client_deinit(struct client_connection *conn)
{
	struct _bad_vrfy_client *ctx = conn->context;

	i_assert(ctx->replied);
	smtp_reply_parser_deinit(&ctx->parser);
}

static void test_client_bad_vrfy(unsigned int index)
{
	test_client_input = test_bad_vrfy_client_input;
	test_client_connected = test_bad_vrfy_client_connected;
	test_client_deinit = test_bad_vrfy_client_deinit;
	test_client_run(index);
}

/* server */

static void
test_server_bad_vrfy_disconnect(void *context ATTR_UNUSED, const char *reason)
{
	if (debug)
		i_debug("Disconnect: %s", reason);
}

static int
test_server_bad_vrfy_rcpt(void *conn_ctx ATTR_UNUSED,
			  struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
			  struct smtp_server_recipient *rcpt ATTR_UNUSED)
{
	test_assert(FALSE);
	return 1;
}

static int
test_server_bad_vrfy_data_begin(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
	struct smtp_server_transaction *trans ATTR_UNUSED,
	struct istream *data_input ATTR_UNUSED)
{
	test_assert(FALSE);
	return 1;
}

static void
test_server_bad_vrfy(const struct smtp_server_settings *server_set)
{
	server_callbacks.conn_disconnect = test_server_bad_vrfy_disconnect;

	server_callbacks.conn_cmd_rcpt = test_server_bad_vrfy_rcpt;
	server_callbacks.conn_cmd_data_begin = test_server_bad_vrfy_data_begin;
	test_server_run(server_set);
}

/* test */

static void test_bad_vrfy(void)
{
	struct smtp_server_settings smtp_server_set;

	test_server_defaults(&smtp_server_set);
	smtp_server_set.max_client_idle_time_msecs = 1000;

	test_begin("bad VRFY");
	test_run_client_server(&smtp_server_set,
			       test_server_bad_vrfy,
			       test_client_bad_vrfy, 4, TRUE);
	test_end();
}

/*
 * Bad NOOP
 */

/* client */

struct _bad_noop_client {
	struct smtp_reply_parser *parser;
	unsigned int reply;

	bool replied:1;
};

static void
test_bad_noop_client_input(struct client_connection *conn)
{
	struct _bad_noop_client *ctx = conn->context;
	struct smtp_reply *reply;
	const char *error;
	int ret;

	while ((ret = smtp_reply_parse_next(ctx->parser, FALSE,
					    &reply, &error)) > 0) {
		if (debug)
			i_debug("REPLY: %s", smtp_reply_log(reply));

		switch (ctx->reply++) {
		case 0: /* greeting */
			i_assert(reply->status == 220);
			break;
		case 1: /* bad command reply */
			switch (client_index) {
			case 1: case 2:
				i_assert(reply->status == 501);
				break;
			case 0: case 3:
				i_assert(smtp_reply_is_success(reply));
				break;
			default:
				i_unreached();
			}
			ctx->replied = TRUE;
			io_loop_stop(ioloop);
			connection_disconnect(&conn->conn);
			return;
		default:
			i_unreached();
		}
	}

	i_assert(ret == 0);
}

static void
test_bad_noop_client_connected(struct client_connection *conn)
{
	struct _bad_noop_client *ctx;

	ctx = p_new(conn->pool, struct _bad_noop_client, 1);
	ctx->parser = smtp_reply_parser_init(conn->conn.input, SIZE_MAX);
	conn->context = ctx;

	switch (client_index) {
	case 0:
		o_stream_nsend_str(conn->conn.output,
				   "NOOP\r\n");
		break;
	case 1:
		o_stream_nsend_str(conn->conn.output,
				   "NOOP \"frop\r\n");
		break;
	case 2:
		o_stream_nsend_str(conn->conn.output,
				   "NOOP fr\"op\r\n");
		break;
	case 3:
		o_stream_nsend_str(conn->conn.output,
				   "NOOP \"frop\"\r\n");
		break;
	default:
		i_unreached();
	}
}

static void
test_bad_noop_client_deinit(struct client_connection *conn)
{
	struct _bad_noop_client *ctx = conn->context;

	i_assert(ctx->replied);
	smtp_reply_parser_deinit(&ctx->parser);
}

static void test_client_bad_noop(unsigned int index)
{
	test_client_input = test_bad_noop_client_input;
	test_client_connected = test_bad_noop_client_connected;
	test_client_deinit = test_bad_noop_client_deinit;
	test_client_run(index);
}

/* server */

static void
test_server_bad_noop_disconnect(void *context ATTR_UNUSED, const char *reason)
{
	if (debug)
		i_debug("Disconnect: %s", reason);
}

static int
test_server_bad_noop_rcpt(void *conn_ctx ATTR_UNUSED,
			  struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
			  struct smtp_server_recipient *rcpt ATTR_UNUSED)
{
	test_assert(FALSE);
	return 1;
}

static int
test_server_bad_noop_data_begin(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
	struct smtp_server_transaction *trans ATTR_UNUSED,
	struct istream *data_input ATTR_UNUSED)
{
	test_assert(FALSE);
	return 1;
}

static void
test_server_bad_noop(const struct smtp_server_settings *server_set)
{
	server_callbacks.conn_disconnect = test_server_bad_noop_disconnect;

	server_callbacks.conn_cmd_rcpt = test_server_bad_noop_rcpt;
	server_callbacks.conn_cmd_data_begin = test_server_bad_noop_data_begin;
	test_server_run(server_set);
}

/* test */

static void test_bad_noop(void)
{
	struct smtp_server_settings smtp_server_set;

	test_server_defaults(&smtp_server_set);
	smtp_server_set.max_client_idle_time_msecs = 1000;

	test_begin("bad NOOP");
	test_run_client_server(&smtp_server_set,
			       test_server_bad_noop,
			       test_client_bad_noop, 4, TRUE);
	test_end();
}

/*
 * MAIL workarounds
 */

/* client */

struct _mail_workarounds_client {
	struct smtp_reply_parser *parser;
	unsigned int reply;

	bool replied:1;
};

static void test_mail_workarounds_client_input(struct client_connection *conn)
{
	struct _mail_workarounds_client *ctx = conn->context;
	struct smtp_reply *reply;
	const char *error;
	int ret;

	while ((ret = smtp_reply_parse_next(ctx->parser, FALSE,
					    &reply, &error)) > 0) {
		if (debug)
			i_debug("REPLY: %s", smtp_reply_log(reply));

		switch (ctx->reply++) {
		case 0: /* greeting */
			i_assert(reply->status == 220);
			break;
		case 1: /* bad command reply */
			switch (client_index) {
			case 5: case 6: case 7:
				i_assert(reply->status == 501);
				break;
			case 0: case 1: case 2: case 3: case 4: case 8:
			case 9: case 10:
				i_assert(reply->status == 250);
				break;
			default:
				i_unreached();
			}
			ctx->replied = TRUE;
			io_loop_stop(ioloop);
			connection_disconnect(&conn->conn);
			return;
		default:
			i_unreached();
		}
	}

	i_assert(ret >= 0);
}

static void
test_mail_workarounds_client_connected(struct client_connection *conn)
{
	struct _mail_workarounds_client *ctx;

	ctx = p_new(conn->pool, struct _mail_workarounds_client, 1);
	ctx->parser = smtp_reply_parser_init(conn->conn.input, SIZE_MAX);
	conn->context = ctx;

	switch (client_index) {
	case 0:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM: <hendrik@example.com>\r\n");
		break;
	case 1:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:\t<hendrik@example.com>\r\n");
		break;
	case 2:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:\t <hendrik@example.com>\r\n");
		break;
	case 3:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:hendrik@example.com\r\n");
		break;
	case 4:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM: hendrik@example.com\r\n");
		break;
	case 5:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:\r\n");
		break;
	case 6:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM: \r\n");
		break;
	case 7:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM: BODY=7BIT\r\n");
		break;
	case 8:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM: <>\r\n");
		break;
	case 9:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:<hendrik@example.com>\r\n");
		break;
	case 10:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:<>\r\n");
		break;
	default:
		i_unreached();
	}
}

static void
test_mail_workarounds_client_deinit(struct client_connection *conn)
{
	struct _mail_workarounds_client *ctx = conn->context;

	i_assert(ctx->replied);
	smtp_reply_parser_deinit(&ctx->parser);
}

static void test_client_mail_workarounds(unsigned int index)
{
	test_client_input = test_mail_workarounds_client_input;
	test_client_connected = test_mail_workarounds_client_connected;
	test_client_deinit = test_mail_workarounds_client_deinit;
	test_client_run(index);
}

/* server */

static void
test_server_mail_workarounds_disconnect(void *context ATTR_UNUSED,
					const char *reason)
{
	if (debug)
		i_debug("Disconnect: %s", reason);
}

static int
test_server_mail_workarounds_rcpt(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
	struct smtp_server_recipient *rcpt ATTR_UNUSED)
{
	test_assert(FALSE);
	return 1;
}

static int
test_server_mail_workarounds_data_begin(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
	struct smtp_server_transaction *trans ATTR_UNUSED,
	struct istream *data_input ATTR_UNUSED)
{
	test_assert(FALSE);
	return 1;
}

static void
test_server_mail_workarounds(const struct smtp_server_settings *server_set)
{
	server_callbacks.conn_disconnect =
		test_server_mail_workarounds_disconnect;

	server_callbacks.conn_cmd_rcpt =
		test_server_mail_workarounds_rcpt;
	server_callbacks.conn_cmd_data_begin =
		test_server_mail_workarounds_data_begin;
	test_server_run(server_set);
}

/* test */

static void test_mail_workarounds(void)
{
	struct smtp_server_settings smtp_server_set;

	test_server_defaults(&smtp_server_set);
	smtp_server_set.workarounds =
		SMTP_SERVER_WORKAROUND_WHITESPACE_BEFORE_PATH |
		SMTP_SERVER_WORKAROUND_MAILBOX_FOR_PATH;
	smtp_server_set.max_client_idle_time_msecs = 1000;

	test_begin("MAIL workarounds");
	test_run_client_server(&smtp_server_set,
			       test_server_mail_workarounds,
			       test_client_mail_workarounds, 11, TRUE);
	test_end();
}

/*
 * RCPT workarounds
 */

/* client */

struct _rcpt_workarounds_client {
	struct smtp_reply_parser *parser;
	unsigned int reply;

	bool replied:1;
};

static void test_rcpt_workarounds_client_input(struct client_connection *conn)
{
	struct _rcpt_workarounds_client *ctx = conn->context;
	struct smtp_reply *reply;
	const char *error;
	int ret;

	while ((ret = smtp_reply_parse_next(ctx->parser, FALSE,
					    &reply, &error)) > 0) {
		if (debug)
			i_debug("REPLY: %s", smtp_reply_log(reply));

		switch (ctx->reply++) {
		case 0: /* greeting */
			i_assert(reply->status == 220);
			break;
		case 1: /* MAIL FROM */
			i_assert(reply->status == 250);
			break;
		case 2: /* bad command reply */
			switch (client_index) {
			case 5: case 6: case 7:
				i_assert(reply->status == 501);
				break;
			case 0: case 1: case 2: case 3: case 4: case 8:
				i_assert(reply->status == 250);
				break;
			default:
				i_unreached();
			}
			ctx->replied = TRUE;
			io_loop_stop(ioloop);
			connection_disconnect(&conn->conn);
			return;
		default:
			i_unreached();
		}
	}

	i_assert(ret >= 0);
}

static void
test_rcpt_workarounds_client_connected(struct client_connection *conn)
{
	struct _rcpt_workarounds_client *ctx;

	ctx = p_new(conn->pool, struct _rcpt_workarounds_client, 1);
	ctx->parser = smtp_reply_parser_init(conn->conn.input, SIZE_MAX);
	conn->context = ctx;

	switch (client_index) {
	case 0:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:<hendrik@example.com>\r\n"
			"RCPT TO: <harrie@example.com>\r\n");
		break;
	case 1:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:<hendrik@example.com>\r\n"
			"RCPT TO:\t<harrie@example.com>\r\n");
		break;
	case 2:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:<hendrik@example.com>\r\n"
			"RCPT TO:\t <harrie@example.com>\r\n");
		break;
	case 3:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:<hendrik@example.com>\r\n"
			"RCPT TO:harrie@example.com\r\n");
		break;
	case 4:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:<hendrik@example.com>\r\n"
			"RCPT TO: harrie@example.com\r\n");
		break;
	case 5:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:<hendrik@example.com>\r\n"
			"RCPT TO:\r\n");
		break;
	case 6:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:<hendrik@example.com>\r\n"
			"RCPT TO: \r\n");
		break;
	case 7:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:<hendrik@example.com>\r\n"
			"RCPT TO: NOTIFY=NEVER\r\n");
		break;
	case 8:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:<hendrik@example.com>\r\n"
			"RCPT TO:<harrie@example.com>\r\n");
		break;
	default:
		i_unreached();
	}
}

static void test_rcpt_workarounds_client_deinit(struct client_connection *conn)
{
	struct _rcpt_workarounds_client *ctx = conn->context;

	i_assert(ctx->replied);
	smtp_reply_parser_deinit(&ctx->parser);
}

static void test_client_rcpt_workarounds(unsigned int index)
{
	test_client_input = test_rcpt_workarounds_client_input;
	test_client_connected = test_rcpt_workarounds_client_connected;
	test_client_deinit = test_rcpt_workarounds_client_deinit;
	test_client_run(index);
}

/* server */

static void
test_server_rcpt_workarounds_disconnect(void *context ATTR_UNUSED,
					const char *reason)
{
	if (debug)
		i_debug("Disconnect: %s", reason);
}

static int
test_server_rcpt_workarounds_rcpt(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
	struct smtp_server_recipient *rcpt ATTR_UNUSED)
{
	return 1;
}

static int
test_server_rcpt_workarounds_data_begin(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
	struct smtp_server_transaction *trans ATTR_UNUSED,
	struct istream *data_input ATTR_UNUSED)
{
	test_assert(FALSE);
	return 1;
}

static void
test_server_rcpt_workarounds(const struct smtp_server_settings *server_set)
{
	server_callbacks.conn_disconnect =
		test_server_rcpt_workarounds_disconnect;

	server_callbacks.conn_cmd_rcpt =
		test_server_rcpt_workarounds_rcpt;
	server_callbacks.conn_cmd_data_begin =
		test_server_rcpt_workarounds_data_begin;
	test_server_run(server_set);
}

/* test */

static void test_rcpt_workarounds(void)
{
	struct smtp_server_settings smtp_server_set;

	test_server_defaults(&smtp_server_set);
	smtp_server_set.workarounds =
		SMTP_SERVER_WORKAROUND_WHITESPACE_BEFORE_PATH |
		SMTP_SERVER_WORKAROUND_MAILBOX_FOR_PATH;
	smtp_server_set.max_client_idle_time_msecs = 1000;

	test_begin("RCPT workarounds");
	test_run_client_server(&smtp_server_set,
			       test_server_rcpt_workarounds,
			       test_client_rcpt_workarounds, 9, TRUE);
	test_end();
}

/*
 * Too many recipients
 */

/* client */

static void test_too_many_recipients_connected(struct client_connection *conn)
{
	o_stream_nsend_str(conn->conn.output,
			   "EHLO frop\r\n"
			   "MAIL FROM:<sender@example.com>\r\n"
			   "RCPT TO:<recipient1@example.com>\r\n"
			   "RCPT TO:<recipient2@example.com>\r\n"
			   "RCPT TO:<recipient3@example.com>\r\n"
			   "RCPT TO:<recipient4@example.com>\r\n"
			   "RCPT TO:<recipient5@example.com>\r\n"
			   "RCPT TO:<recipient6@example.com>\r\n"
			   "RCPT TO:<recipient7@example.com>\r\n"
			   "RCPT TO:<recipient8@example.com>\r\n"
			   "RCPT TO:<recipient9@example.com>\r\n"
			   "RCPT TO:<recipient10@example.com>\r\n"
			   "RCPT TO:<recipient11@example.com>\r\n"
			   "DATA\r\n"
			   "0123456789ABCDEF0123456789ABCDEF\r\n"
			   "0123456789ABCDEF0123456789ABCDEF\r\n"
			   "0123456789ABCDEF0123456789ABCDEF\r\n"
			   "0123456789ABCDEF0123456789ABCDEF\r\n"
			   "0123456789ABCDEF0123456789ABCDEF\r\n"
			   "0123456789ABCDEF0123456789ABCDEF\r\n"
			   "0123456789ABCDEF0123456789ABCDEF\r\n"
			   ".\r\n");
}

static void test_client_too_many_recipients(unsigned int index)
{
	test_client_connected = test_too_many_recipients_connected;
	test_client_run(index);
}

/* server */

static void
test_server_too_many_recipients_trans_free(
	void *conn_ctx ATTR_UNUSED,
	struct smtp_server_transaction *trans ATTR_UNUSED)
{
	io_loop_stop(ioloop);
}

static int
test_server_too_many_recipients_rcpt(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
	struct smtp_server_recipient *rcpt)
{
	if (debug) {
		i_debug("RCPT TO:%s",
			smtp_address_encode(rcpt->path));
	}
	return 1;
}

static int
test_server_too_many_recipients_data_begin(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd,
	struct smtp_server_transaction *trans,
	struct istream *data_input ATTR_UNUSED)
{
	test_assert(array_count(&trans->rcpt_to) == 10);

	smtp_server_reply(cmd, 250, "2.0.0", "OK");
	return 1;
}

static void
test_server_too_many_recipients(const struct smtp_server_settings *server_set)
{
	server_callbacks.conn_trans_free =
		test_server_too_many_recipients_trans_free;
	server_callbacks.conn_cmd_rcpt =
		test_server_too_many_recipients_rcpt;
	server_callbacks.conn_cmd_data_begin =
		test_server_too_many_recipients_data_begin;
	test_server_run(server_set);
}

/* test */

static void test_too_many_recipients(void)
{
	struct smtp_server_settings smtp_server_set;

	test_server_defaults(&smtp_server_set);
	smtp_server_set.max_client_idle_time_msecs = 1000;
	smtp_server_set.max_recipients = 10;

	test_begin("too many recipients");
	test_run_client_server(&smtp_server_set,
			       test_server_too_many_recipients,
			       test_client_too_many_recipients, 1, FALSE);
	test_end();
}

/*
 * DATA without MAIL
 */

/* client */

static void test_data_no_mail_connected(struct client_connection *conn)
{
	o_stream_nsend_str(conn->conn.output,
		"EHLO frop\r\n"
		"DATA\r\n"
		".\r\n"
		"RSET\r\n");
}

static void test_client_data_no_mail(unsigned int index)
{
	test_client_connected = test_data_no_mail_connected;
	test_client_run(index);
}

/* server */

static int
test_server_data_no_mail_rcpt(void *conn_ctx ATTR_UNUSED,
			      struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
			      struct smtp_server_recipient *rcpt ATTR_UNUSED)
{
	/* not supposed to get here */
	i_assert(FALSE);
	return 1;
}

static int
test_server_data_no_mail_data_begin(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
	struct smtp_server_transaction *trans ATTR_UNUSED,
	struct istream *data_input ATTR_UNUSED)
{
	/* not supposed to get here */
	i_assert(FALSE);
	return 1;
}

static int
test_server_data_no_mail_rset(void *conn_ctx ATTR_UNUSED,
			      struct smtp_server_cmd_ctx *cmd ATTR_UNUSED)
{
	io_loop_stop(ioloop);
	return 1;
}

static void
test_server_data_no_mail(const struct smtp_server_settings *server_set)
{
	server_callbacks.conn_cmd_rcpt =
		test_server_data_no_mail_rcpt;
	server_callbacks.conn_cmd_data_begin =
		test_server_data_no_mail_data_begin;
	server_callbacks.conn_cmd_rset =
		test_server_data_no_mail_rset;
	test_server_run(server_set);
}

/* test */

static void test_data_no_mail(void)
{
	struct smtp_server_settings smtp_server_set;

	test_server_defaults(&smtp_server_set);
	smtp_server_set.capabilities =
		SMTP_CAPABILITY_BINARYMIME | SMTP_CAPABILITY_CHUNKING;
	smtp_server_set.max_client_idle_time_msecs = 1000;
	smtp_server_set.max_recipients = 10;

	test_begin("DATA without MAIL");
	test_run_client_server(&smtp_server_set,
			       test_server_data_no_mail,
			       test_client_data_no_mail, 1, FALSE);
	test_end();
}

/*
 * DATA without RCPT
 */

/* client */

static void test_data_no_rcpt_connected(struct client_connection *conn)
{
	o_stream_nsend_str(conn->conn.output,
			   "EHLO frop\r\n"
			   "MAIL FROM:<sender@example.com>\r\n"
			   "DATA\r\n"
			   ".\r\n"
			   "RSET\r\n");
}

static void test_client_data_no_rcpt(unsigned int index)
{
	test_client_connected = test_data_no_rcpt_connected;
	test_client_run(index);
}

/* server */

static void
test_server_data_no_rcpt_trans_free(
	void *conn_ctx  ATTR_UNUSED,
	struct smtp_server_transaction *trans ATTR_UNUSED)
{
	io_loop_stop(ioloop);
}

static int
test_server_data_no_rcpt_rcpt(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
	struct smtp_server_recipient *rcpt ATTR_UNUSED)
{
	/* not supposed to get here */
	i_assert(FALSE);
	return 1;
}

static int
test_server_data_no_rcpt_data_begin(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
	struct smtp_server_transaction *trans ATTR_UNUSED,
	struct istream *data_input ATTR_UNUSED)
{
	/* not supposed to get here */
	i_assert(FALSE);
	return 1;
}

static void
test_server_data_no_rcpt(const struct smtp_server_settings *server_set)
{
	server_callbacks.conn_trans_free =
		test_server_data_no_rcpt_trans_free;
	server_callbacks.conn_cmd_rcpt =
		test_server_data_no_rcpt_rcpt;
	server_callbacks.conn_cmd_data_begin =
		test_server_data_no_rcpt_data_begin;
	test_server_run(server_set);
}

/* test */

static void test_data_no_rcpt(void)
{
	struct smtp_server_settings smtp_server_set;

	test_server_defaults(&smtp_server_set);
	smtp_server_set.capabilities =
		SMTP_CAPABILITY_BINARYMIME | SMTP_CAPABILITY_CHUNKING;
	smtp_server_set.max_client_idle_time_msecs = 1000;
	smtp_server_set.max_recipients = 10;

	test_begin("DATA without RCPT");
	test_run_client_server(&smtp_server_set,
			       test_server_data_no_rcpt,
			       test_client_data_no_rcpt, 1, FALSE);
	test_end();
}

/*
 * Bad pipelined DATA
 */

/* client */

static void test_bad_pipelined_data_connected(struct client_connection *conn)
{
	o_stream_nsend_str(conn->conn.output,
			   "MAIL FROM:<senderp@example.com>\r\n"
			   "RCPT TO:<<recipient1@example.com>\r\n"
			   "DATA\r\n"
			   "FROP!\r\n"
			   "DATA\r\n"
			   "FROP!\r\n"
			   ".\r\n"
			   "QUIT\r\n");
}

static void test_client_bad_pipelined_data(unsigned int index)
{
	test_client_connected = test_bad_pipelined_data_connected;
	test_client_run(index);
}

/* server */

static void
test_server_bad_pipelined_data_trans_free(
	void *conn_ctx  ATTR_UNUSED,
	struct smtp_server_transaction *trans ATTR_UNUSED)
{
	io_loop_stop(ioloop);
}

static int
test_server_bad_pipelined_data_rcpt(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
	struct smtp_server_recipient *rcpt ATTR_UNUSED)
{
	/* not supposed to get here */
	i_assert(FALSE);
	return 1;
}

static int
test_server_bad_pipelined_data_data_begin(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
	struct smtp_server_transaction *trans ATTR_UNUSED,
	struct istream *data_input ATTR_UNUSED)
{
	/* not supposed to get here */
	i_assert(FALSE);
	return 1;
}

static void
test_server_bad_pipelined_data(const struct smtp_server_settings *server_set)
{
	server_callbacks.conn_trans_free =
		test_server_bad_pipelined_data_trans_free;
	server_callbacks.conn_cmd_rcpt =
		test_server_bad_pipelined_data_rcpt;
	server_callbacks.conn_cmd_data_begin =
		test_server_bad_pipelined_data_data_begin;
	test_server_run(server_set);
}

/* test */

static void test_bad_pipelined_data(void)
{
	struct smtp_server_settings smtp_server_set;

	test_server_defaults(&smtp_server_set);
	smtp_server_set.capabilities =
		SMTP_CAPABILITY_BINARYMIME | SMTP_CAPABILITY_CHUNKING;
	smtp_server_set.max_client_idle_time_msecs = 1000;
	smtp_server_set.max_recipients = 10;
	smtp_server_set.max_pipelined_commands = 16;

	test_begin("Bad pipelined DATA");
	test_run_client_server(&smtp_server_set,
			       test_server_bad_pipelined_data,
			       test_client_bad_pipelined_data, 1, FALSE);
	test_end();
}

/*
 * Bad pipelined DATA #2
 */

/* client */

static void test_bad_pipelined_data2_connected(struct client_connection *conn)
{
	o_stream_nsend_str(conn->conn.output,
			   "MAIL FROM:<frop@example.com>\r\n"
			   "DATA\r\n"
			   "DATA\r\n"
			   "RCPT TO:<frop@example.com>\r\n"
			   "BDAT 0\r\n");
}

static void test_client_bad_pipelined_data2(unsigned int index)
{
	test_client_connected = test_bad_pipelined_data2_connected;
	test_client_run(index);
}

/* server */

struct _bad_pipelined_data2 {
	struct istream *payload_input;
	struct io *io;
};

static void
test_server_bad_pipelined_data2_trans_free(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_transaction *trans)
{
	struct _bad_pipelined_data2 *ctx = trans->context;

	i_free(ctx);
	io_loop_stop(ioloop);
}

static int
test_server_bad_pipelined_data2_rcpt(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
	struct smtp_server_recipient *rcpt)
{
	if (debug) {
		i_debug("RCPT TO:%s",
			smtp_address_encode(rcpt->path));
	}
	return 1;
}

static int
test_server_bad_pipelined_data2_data_begin(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
	struct smtp_server_transaction *trans, struct istream *data_input)
{
	struct _bad_pipelined_data2 *ctx;

	if (debug)
		i_debug("DATA");

	ctx = i_new(struct _bad_pipelined_data2, 1);
	trans->context = ctx;

	ctx->payload_input = data_input;
	return 0;
}

static int
test_server_bad_pipelined_data2_data_continue(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd,
	struct smtp_server_transaction *trans ATTR_UNUSED)
{
	struct _bad_pipelined_data2 *ctx = trans->context;
	size_t size;
	ssize_t ret;

	while ((ret = i_stream_read(ctx->payload_input)) > 0 || ret == -2) {
		size = i_stream_get_data_size(ctx->payload_input);
		i_stream_skip(ctx->payload_input, size);
	}

	if (ret == 0)
		return 0;
	if (ret < 0 && ctx->payload_input->stream_errno != 0) {
		/* Client probably disconnected */
		return -1;
	}

	smtp_server_reply_all(cmd, 250, "2.0.0", "Accepted");
	return 1;
}

static void
test_server_bad_pipelined_data2(const struct smtp_server_settings *server_set)
{
	server_callbacks.conn_trans_free =
		test_server_bad_pipelined_data2_trans_free;
	server_callbacks.conn_cmd_rcpt =
		test_server_bad_pipelined_data2_rcpt;
	server_callbacks.conn_cmd_data_begin =
		test_server_bad_pipelined_data2_data_begin;
	server_callbacks.conn_cmd_data_continue =
		test_server_bad_pipelined_data2_data_continue;
	test_server_run(server_set);
}

/* test */

static void test_bad_pipelined_data2(void)
{
	struct smtp_server_settings smtp_server_set;

	test_server_defaults(&smtp_server_set);
	smtp_server_set.capabilities =
		SMTP_CAPABILITY_BINARYMIME | SMTP_CAPABILITY_CHUNKING;
	smtp_server_set.max_client_idle_time_msecs = 1000;
	smtp_server_set.max_recipients = 10;
	smtp_server_set.max_pipelined_commands = 16;

	test_begin("Bad pipelined DATA #2");
	test_run_client_server(&smtp_server_set,
			       test_server_bad_pipelined_data2,
			       test_client_bad_pipelined_data2, 1, FALSE);
	test_end();
}

/*
 * DATA with BINARYMIME
 */

/* client */

static void test_data_binarymime_connected(struct client_connection *conn)
{
	o_stream_nsend_str(conn->conn.output,
			   "EHLO frop\r\n"
			   "MAIL FROM:<sender@example.com> BODY=BINARYMIME\r\n"
			   "RCPT TO:<recipient1@example.com>\r\n"
			   "DATA\r\n"
			   ".\r\n"
			   "RSET\r\n");
}

static void test_client_data_binarymime(unsigned int index)
{
	test_client_connected = test_data_binarymime_connected;
	test_client_run(index);
}

/* server */

static void
test_server_data_binarymime_trans_free(
	void *conn_ctx  ATTR_UNUSED,
	struct smtp_server_transaction *trans ATTR_UNUSED)
{
	io_loop_stop(ioloop);
}

static int
test_server_data_binarymime_rcpt(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
	struct smtp_server_recipient *rcpt)
{
	if (debug) {
		i_debug("RCPT TO:%s",
			smtp_address_encode(rcpt->path));
	}
	return 1;
}

static int
test_server_data_binarymime_data_begin(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
	struct smtp_server_transaction *trans ATTR_UNUSED,
	struct istream *data_input ATTR_UNUSED)
{
	/* not supposed to get here */
	i_assert(FALSE);
	return 1;
}

static void
test_server_data_binarymime(const struct smtp_server_settings *server_set)
{
	server_callbacks.conn_trans_free =
		test_server_data_binarymime_trans_free;
	server_callbacks.conn_cmd_rcpt =
		test_server_data_binarymime_rcpt;
	server_callbacks.conn_cmd_data_begin =
		test_server_data_binarymime_data_begin;
	test_server_run(server_set);
}

/* test */

static void test_data_binarymime(void)
{
	struct smtp_server_settings smtp_server_set;

	test_server_defaults(&smtp_server_set);
	smtp_server_set.capabilities =
		SMTP_CAPABILITY_BINARYMIME | SMTP_CAPABILITY_CHUNKING;
	smtp_server_set.max_client_idle_time_msecs = 1000;
	smtp_server_set.max_recipients = 10;

	test_begin("DATA with BINARYMIME");
	test_run_client_server(&smtp_server_set,
			       test_server_data_binarymime,
			       test_client_data_binarymime, 1, FALSE);
	test_end();
}

/*
 * MAIL broken path
 */

/* client */

struct _mail_broken_path_client {
	struct smtp_reply_parser *parser;
	unsigned int reply;

	bool replied:1;
};

static void
test_mail_broken_path_client_input(struct client_connection *conn)
{
	struct _mail_broken_path_client *ctx = conn->context;
	struct smtp_reply *reply;
	const char *error;
	int ret;

	while ((ret = smtp_reply_parse_next(ctx->parser, FALSE,
					    &reply, &error)) > 0) {
		if (debug)
			i_debug("REPLY: %s", smtp_reply_log(reply));

		switch (ctx->reply++) {
		case 0: /* greeting */
			i_assert(reply->status == 220);
			break;
		case 1: /* bad command reply */
			switch (client_index) {
			case 0: case 1: case 2: case 3: case 4: case 5:
			case 6: case 7: case 8: case 11: case 14: case 16:
				i_assert(reply->status == 501);
				break;
			case 9: case 10: case 12: case 13: case 15: case 17:
				i_assert(reply->status == 250);
				break;
			default:
				i_info("STATUS: %u", reply->status);
				i_unreached();
			}
			ctx->replied = TRUE;
			io_loop_stop(ioloop);
			connection_disconnect(&conn->conn);
			return;
		default:
			i_unreached();
		}
	}

	i_assert(ret >= 0);
}

static void
test_mail_broken_path_client_connected(struct client_connection *conn)
{
	struct _mail_broken_path_client *ctx;

	ctx = p_new(conn->pool, struct _mail_broken_path_client, 1);
	ctx->parser = smtp_reply_parser_init(conn->conn.input, SIZE_MAX);
	conn->context = ctx;

	switch (client_index) {
	case 0:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM: <hendrik@example.com>\r\n");
		break;
	case 1:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:\t<hendrik@example.com>\r\n");
		break;
	case 2:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:\t <hendrik@example.com>\r\n");
		break;
	case 3:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:hendrik@example.com\r\n");
		break;
	case 4:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM: hendrik@example.com\r\n");
		break;
	case 5:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:\r\n");
		break;
	case 6:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM: \r\n");
		break;
	case 7:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM: BODY=7BIT\r\n");
		break;
	case 8:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM: <>\r\n");
		break;
	case 9:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:<hendrik@example.com>\r\n");
		break;
	case 10:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:<>\r\n");
		break;
	case 11:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:bla$die%bla@die&bla\r\n");
		break;
	case 12:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:<u\"ser>\r\n");
		break;
	case 13:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:<u\"ser@domain.tld>\r\n");
		break;
	case 14:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:/@)$@)BLAARGH!@#$$\r\n");
		break;
	case 15:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:</@)$@)BLAARGH!@#$$>\r\n");
		break;
	case 16:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:f\xc3\xb6\xc3\xa4@\xc3\xb6\xc3\xa4\r\n");
		break;
	case 17:
		o_stream_nsend_str(
			conn->conn.output,
			"MAIL FROM:<f\xc3\xb6\xc3\xa4@\xc3\xb6\xc3\xa4>\r\n");
		break;
	default:
		i_unreached();
	}
}

static void test_mail_broken_path_client_deinit(struct client_connection *conn)
{
	struct _mail_broken_path_client *ctx = conn->context;

	i_assert(ctx->replied);
	smtp_reply_parser_deinit(&ctx->parser);
}

static void test_client_mail_broken_path(unsigned int index)
{
	test_client_input = test_mail_broken_path_client_input;
	test_client_connected = test_mail_broken_path_client_connected;
	test_client_deinit = test_mail_broken_path_client_deinit;
	test_client_run(index);
}

/* server */

static void
test_server_mail_broken_path_disconnect(void *context ATTR_UNUSED,
					const char *reason)
{
	if (debug)
		i_debug("Disconnect: %s", reason);
}

static int
test_server_mail_broken_path_rcpt(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
	struct smtp_server_recipient *rcpt ATTR_UNUSED)
{
	test_assert(FALSE);
	return 1;
}

static int
test_server_mail_broken_path_data_begin(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
	struct smtp_server_transaction *trans ATTR_UNUSED,
	struct istream *data_input ATTR_UNUSED)
{
	test_assert(FALSE);
	return 1;
}

static void
test_server_mail_broken_path(const struct smtp_server_settings *server_set)
{
	server_callbacks.conn_disconnect =
		test_server_mail_broken_path_disconnect;

	server_callbacks.conn_cmd_rcpt =
		test_server_mail_broken_path_rcpt;
	server_callbacks.conn_cmd_data_begin =
		test_server_mail_broken_path_data_begin;
	test_server_run(server_set);
}

/* test */

static void test_mail_broken_path(void)
{
	struct smtp_server_settings smtp_server_set;

	test_server_defaults(&smtp_server_set);
	smtp_server_set.mail_path_allow_broken = TRUE;
	smtp_server_set.max_client_idle_time_msecs = 1000;

	test_begin("MAIL broken path");
	test_run_client_server(&smtp_server_set,
			       test_server_mail_broken_path,
			       test_client_mail_broken_path, 18, TRUE);
	test_end();
}

/*
 * Bad pipelined MAIL
 */

/* client */

static void test_bad_pipelined_mail_connected(struct client_connection *conn)
{
	o_stream_nsend_str(conn->conn.output,
			   "MAIL FROM:<user1@example.com>\r\n"
			   "RCPT TO:<user2@example.com>\r\n"
			   "RCPT TO:<user3@example.com>\r\n"
			   "MAIL FROM:<user4@example.com>\r\n"
			   "DATA\r\n"
			   "FROP!\r\n"
			   ".\r\n"
			   "QUIT\r\n");
}

static void test_client_bad_pipelined_mail(unsigned int index)
{
	test_client_connected = test_bad_pipelined_mail_connected;
	test_client_run(index);
}

/* server */

struct _bad_pipelined_mail {
	struct istream *payload_input;
	struct io *io;
};

static void
test_server_bad_pipelined_mail_trans_free(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_transaction *trans)
{
	struct _bad_pipelined_mail *ctx = trans->context;

	i_free(ctx);
	io_loop_stop(ioloop);
}

static int
test_server_bad_pipelined_mail_rcpt(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
	struct smtp_server_recipient *rcpt)
{
	if (debug) {
		i_debug("RCPT TO:%s",
			smtp_address_encode(rcpt->path));
	}
	return 1;
}

static int
test_server_bad_pipelined_mail_data_begin(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
	struct smtp_server_transaction *trans, struct istream *data_input)
{
	struct _bad_pipelined_mail *ctx;

	if (debug)
		i_debug("DATA");

	ctx = i_new(struct _bad_pipelined_mail, 1);
	trans->context = ctx;

	ctx->payload_input = data_input;
	return 0;
}

static int
test_server_bad_pipelined_mail_data_continue(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd,
	struct smtp_server_transaction *trans ATTR_UNUSED)
{
	struct _bad_pipelined_mail *ctx = trans->context;
	size_t size;
	ssize_t ret;

	while ((ret = i_stream_read(ctx->payload_input)) > 0 || ret == -2) {
		size = i_stream_get_data_size(ctx->payload_input);
		i_stream_skip(ctx->payload_input, size);
	}

	if (ret == 0)
		return 0;
	if (ret < 0 && ctx->payload_input->stream_errno != 0) {
		/* Client probably disconnected */
		return -1;
	}

	smtp_server_reply_all(cmd, 250, "2.0.0", "Accepted");
	return 1;
}

static void
test_server_bad_pipelined_mail(const struct smtp_server_settings *server_set)
{
	server_callbacks.conn_trans_free =
		test_server_bad_pipelined_mail_trans_free;
	server_callbacks.conn_cmd_rcpt =
		test_server_bad_pipelined_mail_rcpt;
	server_callbacks.conn_cmd_data_begin =
		test_server_bad_pipelined_mail_data_begin;
	server_callbacks.conn_cmd_data_continue =
		test_server_bad_pipelined_mail_data_continue;
	test_server_run(server_set);
}

/* test */

static void test_bad_pipelined_mail(void)
{
	struct smtp_server_settings smtp_server_set;

	test_server_defaults(&smtp_server_set);
	smtp_server_set.capabilities =
		SMTP_CAPABILITY_BINARYMIME | SMTP_CAPABILITY_CHUNKING;
	smtp_server_set.max_client_idle_time_msecs = 1000;
	smtp_server_set.max_recipients = 10;
	smtp_server_set.max_pipelined_commands = 16;

	test_begin("Bad pipelined MAIL");
	test_run_client_server(&smtp_server_set,
			       test_server_bad_pipelined_mail,
			       test_client_bad_pipelined_mail, 1, FALSE);
	test_end();
}

/*
 * TLS SNI
 */

/* client */

struct _tls_sni_client {
	struct smtp_reply_parser *parser;
	unsigned int reply;

	bool replied:1;
};

static void test_tls_sni_client_input(struct client_connection *conn)
{
	struct _tls_sni_client *ctx = conn->context;
	struct smtp_reply *reply;
	const char *error;
	int ret;

	for (;;) {
		if (ctx->reply == 1) {
			ret = smtp_reply_parse_ehlo(ctx->parser, &reply,
						    &error);
		} else {
			ret = smtp_reply_parse_next(ctx->parser, FALSE, &reply,
						    &error);
		}
		if (ret <= 0)
			break;

		if (debug)
			i_debug("REPLY: %s", smtp_reply_log(reply));

		switch (ctx->reply++) {
		case 0: /* greeting */
			i_assert(reply->status == 220);
			i_assert(str_begins_with(reply->text_lines[0],
						 "chickencoop.example "));
			break;
		case 1: /* EHLO command reply */
			i_assert(reply->status == 250);
			if (debug)
				i_debug("REPLIED");
			ctx->replied = TRUE;
			io_loop_stop(ioloop);
			connection_disconnect(&conn->conn);
			return;
		default:
			i_unreached();
		}
	}

	i_assert(ret >= 0);
}

static void test_tls_sni_client_connected(struct client_connection *conn)
{
	struct _tls_sni_client *ctx;

	ctx = p_new(conn->pool, struct _tls_sni_client, 1);
	ctx->parser = smtp_reply_parser_init(conn->conn.input, SIZE_MAX);
	conn->context = ctx;

	o_stream_nsend_str(conn->conn.output, "EHLO frop\r\n");
}

static void test_tls_sni_client_deinit(struct client_connection *conn)
{
	struct _tls_sni_client *ctx = conn->context;

	i_assert(ctx->replied);
	smtp_reply_parser_deinit(&ctx->parser);
}

static void test_client_tls_sni(unsigned int index)
{
	test_client_input = test_tls_sni_client_input;
	test_client_connected = test_tls_sni_client_connected;
	test_client_deinit = test_tls_sni_client_deinit;
	test_client_run(index);
}

/* server */

static void
test_server_tls_sni_disconnect(void *context ATTR_UNUSED, const char *reason)
{
	if (debug)
		i_debug("Disconnect: %s", reason);
}

static int
test_server_tls_sni_helo(void *conn_ctx ATTR_UNUSED,
			  struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
			  struct smtp_server_cmd_helo *data ATTR_UNUSED)
{
	return 1;
}

static int
test_server_tls_sni_rcpt(void *conn_ctx ATTR_UNUSED,
			  struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
			  struct smtp_server_recipient *rcpt ATTR_UNUSED)
{
	return 1;
}

static int
test_server_tls_sni_data_begin(
	void *conn_ctx ATTR_UNUSED, struct smtp_server_cmd_ctx *cmd,
	struct smtp_server_transaction *trans ATTR_UNUSED,
	struct istream *data_input ATTR_UNUSED)
{
	smtp_server_reply(cmd, 250, "2.0.0", "OK");
	return 1;
}

static int
test_server_tls_sni_callback(void *conn_ctx, const char *name,
			     const char **error_r ATTR_UNUSED)
{
	struct server_connection *sconn = conn_ctx;

	if (debug)
		i_debug("TLS SNI: %s", name);

	smtp_server_connection_start(sconn->conn);

	return 0;
}

static void test_server_tls_sni(const struct smtp_server_settings *server_set)
{
	server_callbacks.conn_disconnect =
		test_server_tls_sni_disconnect;

	server_callbacks.conn_cmd_helo =
		test_server_tls_sni_helo;
	server_callbacks.conn_cmd_rcpt =
		test_server_tls_sni_rcpt;
	server_callbacks.conn_cmd_data_begin =
		test_server_tls_sni_data_begin;
	server_callbacks.conn_tls_sni_callback =
		test_server_tls_sni_callback;

	test_server_run(server_set);
}

/* test */

static void test_tls_sni(void)
{
	struct smtp_server_settings smtp_server_set;

	test_ssl_host = "chickencoop.example";
	test_server_delay_start = TRUE;

	test_server_defaults_ssl(&smtp_server_set);
	smtp_server_set.max_client_idle_time_msecs = 1000;

	test_begin("TLS SNI");
	test_run_client_server(&smtp_server_set,
			       test_server_tls_sni,
			       test_client_tls_sni, 1, TRUE);
	test_end();
}


/*
 * All tests
 */

static void (*const test_functions[])(void) = {
	test_slow_server,
	test_slow_client,
	test_hanging_command_payload,
	test_bad_command,
	test_many_bad_commands,
	test_long_command,
	test_long_auth_line,
	test_long_auth_line_small_buf,
	test_big_data,
	test_bad_helo,
	test_bad_mail,
	test_bad_rcpt,
	test_bad_vrfy,
	test_bad_noop,
	test_mail_workarounds,
	test_rcpt_workarounds,
	test_too_many_recipients,
	test_data_no_mail,
	test_data_no_rcpt,
	test_bad_pipelined_data,
	test_bad_pipelined_data2,
	test_data_binarymime,
	test_mail_broken_path,
	test_bad_pipelined_mail,
	test_tls_sni,
	NULL
};

/*
 * Test client
 */

/* client connection */

static int client_connection_init_ssl(struct client_connection *conn)
{
	struct ssl_iostream_settings ssl_set;
	const char *error;

	if (test_ssl_host == NULL)
		return 0;

	connection_input_halt(&conn->conn);

	ssl_iostream_test_settings_client(&ssl_set);
	ssl_set.allow_invalid_cert = TRUE;

	if (client_ssl_ctx == NULL &&
	    ssl_iostream_context_init_client(&ssl_set, &client_ssl_ctx,
					     &error) < 0) {
		i_error("SSL context initialization failed: %s", error);
		return -1;
	}

	if (io_stream_create_ssl_client(client_ssl_ctx, test_ssl_host,
					conn->conn.event, 0,
					&conn->conn.input, &conn->conn.output,
					&conn->ssl_iostream, &error) < 0) {
		i_error("SSL init failed: %s", error);
		return -1;
	}
	if (ssl_iostream_handshake(conn->ssl_iostream) < 0) {
		i_error("SSL handshake failed: %s",
			ssl_iostream_get_last_error(conn->ssl_iostream));
		return -1;
	}

	connection_input_resume(&conn->conn);
	return 0;
}

static void client_connection_input(struct connection *_conn)
{
	struct client_connection *conn = (struct client_connection *)_conn;

	if (test_client_input != NULL)
		test_client_input(conn);
}

static void client_connection_connected(struct connection *_conn, bool success)
{
	struct client_connection *conn = (struct client_connection *)_conn;

	if (debug)
		i_debug("Client connected");

	if (success) {
		if (client_connection_init_ssl(conn) < 0) {
			client_connection_deinit(&conn);
			return;
		}
		if (test_client_connected != NULL)
			test_client_connected(conn);
	}
}

static void client_connection_init(const struct ip_addr *ip, in_port_t port)
{
	struct client_connection *conn;
	pool_t pool;

	pool = pool_alloconly_create("client connection", 1024);
	conn = p_new(pool, struct client_connection, 1);
	conn->pool = pool;

	connection_init_client_ip(client_conn_list, &conn->conn, NULL,
				  ip, port);
	event_set_forced_debug(conn->conn.event, debug);
	(void)connection_client_connect(&conn->conn);
}

static void client_connection_deinit(struct client_connection **_conn)
{
	struct client_connection *conn = *_conn;

	*_conn = NULL;

	if (test_client_deinit != NULL)
		test_client_deinit(conn);

	ssl_iostream_destroy(&conn->ssl_iostream);
	connection_deinit(&conn->conn);
	pool_unref(&conn->pool);
}

static void client_connection_destroy(struct connection *_conn)
{
	struct client_connection *conn = (struct client_connection *)_conn;

	client_connection_deinit(&conn);
}

/* */

static struct connection_settings client_connection_set = {
	.input_max_size = SIZE_MAX,
	.output_max_size = SIZE_MAX,
	.client = TRUE
};

static const struct connection_vfuncs client_connection_vfuncs = {
	.destroy = client_connection_destroy,
	.client_connected = client_connection_connected,
	.input = client_connection_input
};

static void test_client_run(unsigned int index)
{
	client_index = index;

	if (debug)
		i_debug("client connecting to %u", bind_port);

	client_conn_list = connection_list_init(&client_connection_set,
						&client_connection_vfuncs);

	client_connection_init(&bind_ip, bind_port);

	io_loop_run(ioloop);

	/* close server socket */
	io_remove(&io_listen);

	connection_list_deinit(&client_conn_list);

	ssl_iostream_context_unref(&client_ssl_ctx);
}

/*
 * Test server
 */

static void test_server_defaults(struct smtp_server_settings *smtp_set)
{
	/* server settings */
	i_zero(smtp_set);
	smtp_set->max_client_idle_time_msecs = 5*1000;
	smtp_set->max_pipelined_commands = 1;
	smtp_set->auth_optional = TRUE;
	smtp_set->debug = debug;
}

static void
test_server_defaults_ssl(struct smtp_server_settings *smtp_set)
{
	static struct ssl_iostream_settings ssl_set;

	ssl_iostream_test_settings_server(&ssl_set);

	test_server_defaults(smtp_set);
	smtp_set->ssl = &ssl_set;
}

/* client connection */

static void server_connection_free(void *context)
{
	struct server_connection *sconn = (struct server_connection *)context;

	if (debug)
		i_debug("Connection freed");

	if (--server_pending == 0)
		io_loop_stop(ioloop);
	i_free(sconn);
}

static void server_connection_accept(void *context ATTR_UNUSED)
{
	struct smtp_server_connection *conn;
	struct server_connection *sconn;
	int fd;

	/* accept new client */
	fd = net_accept(fd_listen, NULL, NULL);
	if (fd == -1)
		return;
	if (fd == -2) {
		i_fatal("test server: accept() failed: %m");
	}

	if (debug)
		i_debug("Accepted connection");

	net_set_nonblock(fd, TRUE);

	sconn = i_new(struct server_connection, 1);

	server_callbacks.conn_free = server_connection_free;

	if (server_io_buffer_size == 0) {
		conn = smtp_server_connection_create(
			smtp_server, fd, fd, NULL, 0, (test_ssl_host != NULL),
			NULL, &server_callbacks, sconn);
	} else {
		struct istream *input;
		struct ostream *output;

		input = i_stream_create_fd(fd, server_io_buffer_size);
		output = o_stream_create_fd(fd, server_io_buffer_size);
		o_stream_set_no_error_handling(output, TRUE);

		conn = smtp_server_connection_create_from_streams(
			smtp_server, input, output, NULL, 0, NULL,
			&server_callbacks, sconn);

		i_stream_unref(&input);
		o_stream_unref(&output);
	}
	sconn->conn = conn;
	if (!test_server_delay_start)
		smtp_server_connection_start(conn);
}

/* */

static void test_server_timeout(void *context ATTR_UNUSED)
{
	i_fatal("Server timed out");
}

static void test_server_run(const struct smtp_server_settings *smtp_set)
{
	struct timeout *to;

	to = timeout_add(SERVER_MAX_TIMEOUT_MSECS,
			 test_server_timeout, NULL);

	/* open server socket */
	io_listen = io_add(fd_listen, IO_READ, server_connection_accept, NULL);

	smtp_server = smtp_server_init(smtp_set);

	io_loop_run(ioloop);

	if (debug)
		i_debug("Server finished");

	/* close server socket */
	io_remove(&io_listen);
	timeout_remove(&to);

	smtp_server_deinit(&smtp_server);
}

/*
 * Tests
 */

struct test_client_data {
	unsigned int index;
	test_client_init_t client_test;
	struct settings_simple *settings;
};

static int test_open_server_fd(void)
{
	int fd = net_listen(&bind_ip, &bind_port, 128);
	if (debug)
		i_debug("server listening on %u", bind_port);
	if (fd == -1) {
		i_fatal("listen(%s:%u) failed: %m",
			net_ip2addr(&bind_ip), bind_port);
	}
	return fd;
}

static int test_run_client(struct test_client_data *data)
{
	i_set_failure_prefix("CLIENT[%u]: ", data->index + 1);

	if (debug)
		i_debug("PID=%s", my_pid);

	client_ssl_ctx = NULL;

	test_subprocess_notify_signal_reset(SIGUSR1);

	/* signal server that we started */
	test_subprocess_notify_signal_send_parent(SIGUSR1);

	/* wait server to be ready */
	test_subprocess_notify_signal_wait(SIGUSR1, TEST_SIGNALS_DEFAULT_TIMEOUT_MS);

	ioloop = io_loop_create();
	data->client_test(data->index);
	io_loop_destroy(&ioloop);

	if (debug)
		i_debug("Terminated");

	main_deinit();

	/* Cleanup the test settings in the client process as well.
	   See test_run_client_server() for the appropriate cleanup call in the
	   main process. */
	settings_simple_deinit(data->settings);
	return 0;
}

static void
test_run_server(const struct smtp_server_settings *server_set,
		test_server_init_t server_test,
		unsigned int client_tests_count)
{
	i_set_failure_prefix("SERVER: ");

	if (debug)
		i_debug("PID=%s", my_pid);

	i_zero(&server_callbacks);

	server_pending = client_tests_count;

	/* signal clients that server is ready */
	test_subprocess_notify_signal_all(SIGUSR1);
	ioloop = io_loop_create();
	server_test(server_set);
	io_loop_destroy(&ioloop);

	if (debug)
		i_debug("Terminated");
}

static void
test_run_client_server(struct smtp_server_settings *server_set,
		       test_server_init_t server_test,
		       test_client_init_t client_test,
		       unsigned int client_tests_count, bool wait_for_clients)
{
	unsigned int i;

	server_io_buffer_size = 0;

	/* Add SSL settings by name into the basis of the SMTP server settings.
	   Otherwise the SMTP SNI mechanism will break when looking up the
	   relevant settings. */
	const char *const settings[] = {
		"ssl_server_ca_file",
		(server_set->ssl == NULL ? "" :
		 settings_file_get_value(unsafe_data_stack_pool,
					 &server_set->ssl->ca)),
		"ssl_server_cert_file",
		(server_set->ssl == NULL ? "" :
		 settings_file_get_value(unsafe_data_stack_pool,
					 &server_set->ssl->cert.cert)),
		"ssl_server_key_file",
		(server_set->ssl == NULL ? "" :
		 settings_file_get_value(unsafe_data_stack_pool,
					 &server_set->ssl->cert.key)),
		NULL,
	};
	struct settings_simple test_set;
	settings_simple_init(&test_set, settings);
	server_set->event_parent = test_set.event;

	fd_listen = test_open_server_fd();

	for (i = 0; i < client_tests_count; i++) {
		struct test_client_data data;

		i_zero(&data);
		data.index = i;
		data.client_test = client_test;
		data.settings = &test_set;

		/* Fork client */
		test_subprocess_notify_signal_reset(SIGUSR1);
		test_subprocess_fork(test_run_client, &data, FALSE);
		test_subprocess_notify_signal_wait(
			SIGUSR1, TEST_SIGNALS_DEFAULT_TIMEOUT_MS);
	}

	/* Run server */
	test_run_server(server_set, server_test, client_tests_count);

	if (wait_for_clients)
		test_subprocess_wait_all(CLIENT_KILL_TIMEOUT_SECS);
	i_unset_failure_prefix();
	i_close_fd(&fd_listen);
	test_subprocess_kill_all(CLIENT_KILL_TIMEOUT_SECS);

	test_ssl_host = NULL;
	test_server_delay_start = FALSE;

	ssl_iostream_context_cache_free();

	/* Cleanup the test settings in the main process.
	   Note: This needs to be called as well in the server process,
	   otherwise it will leak it's event and the looked up settings
	   struct. See test_run_server() for the appropriate cleanup call in
	   the server process. */
	settings_simple_deinit(&test_set);
}

/*
 * Main
 */

static void main_init(void)
{
	ssl_iostream_openssl_init();
}

static void main_deinit(void)
{
	ssl_iostream_context_cache_free();
	ssl_iostream_openssl_deinit();
}

int main(int argc, char *argv[])
{
	int c;
	int ret;

	lib_init();
	main_init();

	while ((c = getopt(argc, argv, "D")) > 0) {
		switch (c) {
		case 'D':
			debug = TRUE;
			break;
		default:
			i_fatal("Usage: %s [-D]", argv[0]);
		}
	}

	test_subprocesses_init(debug);

	/* listen on localhost */
	i_zero(&bind_ip);
	bind_ip.family = AF_INET;
	bind_ip.u.ip4.s_addr = htonl(INADDR_LOOPBACK);

	ret = test_run(test_functions);

	test_subprocesses_deinit();
	main_deinit();
	lib_deinit();

	return ret;
}
