/* Copyright (c) 2011-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "str.h"
#include "ioloop.h"
#include "safe-mkstemp.h"
#include "settings.h"
#include "imapc-msgmap.h"
#include "imapc-connection.h"
#include "imapc-client-private.h"
#include "imapc-settings.h"

#include <unistd.h>

const char *imapc_command_state_names[] = {
	"OK", "NO", "BAD", "(auth failed)", "(disconnected)"
};

const struct imapc_capability_name imapc_capability_names[] = {
	{ "SASL-IR", IMAPC_CAPABILITY_SASL_IR },
	{ "LITERAL+", IMAPC_CAPABILITY_LITERALPLUS },
	{ "QRESYNC", IMAPC_CAPABILITY_QRESYNC },
	{ "IDLE", IMAPC_CAPABILITY_IDLE },
	{ "UIDPLUS", IMAPC_CAPABILITY_UIDPLUS },
	{ "AUTH=PLAIN", IMAPC_CAPABILITY_AUTH_PLAIN },
	{ "STARTTLS", IMAPC_CAPABILITY_STARTTLS },
	{ "X-GM-EXT-1", IMAPC_CAPABILITY_X_GM_EXT_1 },
	{ "CONDSTORE", IMAPC_CAPABILITY_CONDSTORE },
	{ "NAMESPACE", IMAPC_CAPABILITY_NAMESPACE },
	{ "UNSELECT", IMAPC_CAPABILITY_UNSELECT },
	{ "ESEARCH", IMAPC_CAPABILITY_ESEARCH },
	{ "WITHIN", IMAPC_CAPABILITY_WITHIN },
	{ "QUOTA", IMAPC_CAPABILITY_QUOTA },
	{ "ID", IMAPC_CAPABILITY_ID },
	{ "SAVEDATE", IMAPC_CAPABILITY_SAVEDATE },
	{ "METADATA", IMAPC_CAPABILITY_METADATA },

	{ "IMAP4REV1", IMAPC_CAPABILITY_IMAP4REV1 },
	{ "IMAP4REV2", IMAPC_CAPABILITY_IMAP4REV2 },
	{ NULL, 0 }
};

unsigned int imapc_client_cmd_tag_counter = 0;

static void
default_untagged_callback(const struct imapc_untagged_reply *reply ATTR_UNUSED,
			  void *context ATTR_UNUSED)
{
}

struct imapc_client *
imapc_client_init(const struct imapc_parameters *params,
		  struct event *event_parent)
{
	struct imapc_client *client;
	pool_t pool;

	pool = pool_alloconly_create("imapc client", 1024);
	client = p_new(pool, struct imapc_client, 1);
	client->pool = pool;
	client->refcount = 1;
	client->event = event_create(event_parent);
	client->untagged_callback = default_untagged_callback;

	client->set = settings_get_or_fatal(client->event, &imapc_setting_parser_info);
	client->params.session_id_prefix =
		p_strdup(pool, params->session_id_prefix);
	client->params.temp_path_prefix =
		p_strdup(pool, params->temp_path_prefix);
	client->params.flags = params->flags;

	client->imapc_rawlog_dir =
		(params->override_rawlog_dir != NULL) ?
			p_strdup(pool, params->override_rawlog_dir) :
			p_strdup(pool, client->set->imapc_rawlog_dir);
	client->password =
		(params->override_password != NULL) ?
			p_strdup(pool, params->override_password) :
			p_strdup(pool, client->set->imapc_password);

	event_set_append_log_prefix(client->event, t_strdup_printf(
		"imapc(%s:%u): ", client->set->imapc_host, client->set->imapc_port));

	client->ssl_mode = IMAPC_CLIENT_SSL_MODE_NONE;
	if (strcmp(client->set->imapc_ssl, "imaps") == 0) {
		client->ssl_mode = IMAPC_CLIENT_SSL_MODE_IMMEDIATE;
	} else if (strcmp(client->set->imapc_ssl, "starttls") == 0) {
		client->ssl_mode = IMAPC_CLIENT_SSL_MODE_STARTTLS;
	}

	p_array_init(&client->conns, pool, 8);
	return client;
}

void imapc_client_ref(struct imapc_client *client)
{
	i_assert(client->refcount > 0);

	client->refcount++;
}

void imapc_client_unref(struct imapc_client **_client)
{
	struct imapc_client *client = *_client;

	*_client = NULL;

	i_assert(client->refcount > 0);
	if (--client->refcount > 0)
		return;

	settings_free(client->set);

	event_unref(&client->event);
	pool_unref(&client->pool);
}

void imapc_client_disconnect(struct imapc_client *client)
{
	struct imapc_client_connection *const *conns, *conn;
	unsigned int i, count;

	conns = array_get(&client->conns, &count);
	for (i = count; i > 0; i--) {
		conn = conns[i-1];
		array_delete(&client->conns, i-1, 1);

		i_assert(imapc_connection_get_mailbox(conn->conn) == NULL);
		imapc_connection_deinit(&conn->conn);
		i_free(conn);
	}
}

void imapc_client_deinit(struct imapc_client **_client)
{
	struct imapc_client *client = *_client;

	imapc_client_disconnect(client);
	imapc_client_unref(_client);
}

void imapc_client_register_untagged(struct imapc_client *client,
				    imapc_untagged_callback_t *callback,
				    void *context)
{
	client->untagged_callback = callback;
	client->untagged_context = context;
}

static void imapc_client_run_pre(struct imapc_client *client)
{
	struct imapc_client_connection *conn;
	struct ioloop *prev_ioloop = current_ioloop;

	i_assert(client->ioloop == NULL);

	client->ioloop = io_loop_create();
	io_loop_set_running(client->ioloop);

	array_foreach_elem(&client->conns, conn) {
		imapc_connection_ioloop_changed(conn->conn);
		if (imapc_connection_get_state(conn->conn) == IMAPC_CONNECTION_STATE_DISCONNECTED)
			imapc_connection_connect(conn->conn);
	}

	if (io_loop_is_running(client->ioloop))
		io_loop_run(client->ioloop);
	io_loop_set_current(prev_ioloop);
}

static void imapc_client_run_post(struct imapc_client *client)
{
	struct imapc_client_connection *conn;
	struct ioloop *ioloop = client->ioloop;

	client->ioloop = NULL;
	array_foreach_elem(&client->conns, conn) {
		imapc_connection_ioloop_changed(conn->conn);
		if (conn->box != NULL) {
			conn->box->to_send_idle =
				io_loop_move_timeout(&conn->box->to_send_idle);
		}
	}

	io_loop_set_current(ioloop);
	io_loop_destroy(&ioloop);
}

void imapc_client_run(struct imapc_client *client)
{
	imapc_client_run_pre(client);
	imapc_client_run_post(client);
}

void imapc_client_stop(struct imapc_client *client)
{
	if (client->ioloop != NULL)
		io_loop_stop(client->ioloop);
}

void imapc_client_try_stop(struct imapc_client *client)
{
	struct imapc_client_connection *conn;
	array_foreach_elem(&client->conns, conn)
		if (imapc_connection_get_state(conn->conn) != IMAPC_CONNECTION_STATE_DISCONNECTED)
			return;
	imapc_client_stop(client);
}

bool imapc_client_is_running(struct imapc_client *client)
{
	return client->ioloop != NULL;
}

static void imapc_client_login_callback(const struct imapc_command_reply *reply,
					void *context)
{
	struct imapc_client_connection *conn = context;
	struct imapc_client *client = conn->client;
	struct imapc_client_mailbox *box = conn->box;

	if (box != NULL && box->reconnecting) {
		box->reconnecting = FALSE;

		if (reply->state == IMAPC_COMMAND_STATE_OK) {
			/* reopen the mailbox */
			box->reopen_callback(box->reopen_context);
		} else {
			imapc_connection_abort_commands(box->conn, NULL, FALSE);
		}
	}

	/* call the login callback only once */
	if (client->login_callback != NULL) {
		imapc_command_callback_t *callback = client->login_callback;
		void *context = client->login_context;

		client->login_callback = NULL;
		client->login_context = NULL;
		callback(reply, context);
	}
}

static struct imapc_client_connection *
imapc_client_add_connection(struct imapc_client *client)
{
	struct imapc_client_connection *conn;

	conn = i_new(struct imapc_client_connection, 1);
	conn->client = client;
	conn->conn = imapc_connection_init(client, imapc_client_login_callback,
					   conn);
	array_push_back(&client->conns, &conn);
	return conn;
}

static struct imapc_connection *
imapc_client_find_connection(struct imapc_client *client)
{
	struct imapc_client_connection *const *connp;

	/* FIXME: stupid algorithm */
	if (array_count(&client->conns) == 0)
		return imapc_client_add_connection(client)->conn;
	connp = array_front(&client->conns);
	return (*connp)->conn;
}

struct imapc_command *
imapc_client_cmd(struct imapc_client *client,
		 imapc_command_callback_t *callback, void *context)
{
	struct imapc_connection *conn;

	conn = imapc_client_find_connection(client);
	return imapc_connection_cmd(conn, callback, context);
}

static struct imapc_client_connection *
imapc_client_get_unboxed_connection(struct imapc_client *client)
{
	struct imapc_client_connection *const *conns;
	unsigned int i, count;

	conns = array_get(&client->conns, &count);
	for (i = 0; i < count; i++) {
		if (conns[i]->box == NULL)
			return conns[i];
	}
	return imapc_client_add_connection(client);
}


void imapc_client_login(struct imapc_client *client)
{
	struct imapc_client_connection *conn;

	i_assert(client->login_callback != NULL);
	i_assert(array_count(&client->conns) == 0);

	conn = imapc_client_add_connection(client);
	imapc_connection_connect(conn->conn);
}

struct imapc_logout_ctx {
	struct imapc_client *client;
	unsigned int logout_count;
};

static void
imapc_client_logout_callback(const struct imapc_command_reply *reply ATTR_UNUSED,
			     void *context)
{
	struct imapc_logout_ctx *ctx = context;

	i_assert(ctx->logout_count > 0);

	if (--ctx->logout_count == 0)
		imapc_client_stop(ctx->client);
}

void imapc_client_logout(struct imapc_client *client)
{
	struct imapc_logout_ctx ctx = { .client = client };
	struct imapc_client_connection *conn;
	struct imapc_command *cmd;

	client->logging_out = TRUE;

	/* send LOGOUT to all connections */
	array_foreach_elem(&client->conns, conn) {
		if (imapc_connection_get_state(conn->conn) == IMAPC_CONNECTION_STATE_DISCONNECTED)
			continue;
		imapc_connection_set_no_reconnect(conn->conn);
		ctx.logout_count++;
		cmd = imapc_connection_cmd(conn->conn,
			imapc_client_logout_callback, &ctx);
		imapc_command_set_flags(cmd, IMAPC_COMMAND_FLAG_PRELOGIN |
					IMAPC_COMMAND_FLAG_LOGOUT);
		imapc_command_send(cmd, "LOGOUT");
	}

	/* wait for LOGOUT to finish */
	while (ctx.logout_count > 0)
		imapc_client_run(client);

	/* we should have disconnected all clients already, but if there were
	   any timeouts there may be some clients left. */
	imapc_client_disconnect(client);
}

struct imapc_client_mailbox *
imapc_client_mailbox_open(struct imapc_client *client,
			  void *untagged_box_context)
{
	struct imapc_client_mailbox *box;
	struct imapc_client_connection *conn;

	box = i_new(struct imapc_client_mailbox, 1);
	box->client = client;
	box->untagged_box_context = untagged_box_context;
	conn = imapc_client_get_unboxed_connection(client);
	conn->box = box;
	box->conn = conn->conn;
	box->msgmap = imapc_msgmap_init();
	/* if we get disconnected before the SELECT is finished, allow
	   one reconnect retry. */
	box->reconnect_ok = TRUE;
	return box;
}

void imapc_client_mailbox_set_reopen_cb(struct imapc_client_mailbox *box,
					void (*callback)(void *context),
					void *context)
{
	box->reopen_callback = callback;
	box->reopen_context = context;
}

bool imapc_client_mailbox_can_reconnect(struct imapc_client_mailbox *box)
{
	/* the reconnect_ok flag attempts to avoid infinite reconnection loops
	   to a server that keeps disconnecting us (e.g. some of the commands
	   we send keeps crashing it always) */
	return box->reopen_callback != NULL && box->reconnect_ok;
}

void imapc_client_mailbox_reconnect(struct imapc_client_mailbox *box,
				    const char *errmsg)
{
	imapc_connection_try_reconnect(box->conn, errmsg, 0, FALSE);
}

void imapc_client_mailbox_close(struct imapc_client_mailbox **_box)
{
	struct imapc_client_mailbox *box = *_box;
	struct imapc_client_connection *conn;

	box->closing = TRUE;

	/* cancel any pending commands */
	imapc_connection_unselect(box, FALSE);

	if (box->reconnecting) {
		/* need to abort the reconnection so it won't try to access
		   the box */
		imapc_connection_disconnect(box->conn);
	}

	/* set this only after unselect, which may cancel some commands that
	   reference this box */
	*_box = NULL;

	array_foreach_elem(&box->client->conns, conn) {
		if (conn->box == box) {
			conn->box = NULL;
			break;
		}
	}

	imapc_msgmap_deinit(&box->msgmap);
	timeout_remove(&box->to_send_idle);
	i_free(box);
}

struct imapc_command *
imapc_client_mailbox_cmd(struct imapc_client_mailbox *box,
			 imapc_command_callback_t *callback, void *context)
{
	struct imapc_command *cmd;

	i_assert(!box->closing);

	cmd = imapc_connection_cmd(box->conn, callback, context);
	imapc_command_set_mailbox(cmd, box);
	return cmd;
}

struct imapc_msgmap *
imapc_client_mailbox_get_msgmap(struct imapc_client_mailbox *box)
{
	return box->msgmap;
}

static void imapc_client_mailbox_idle_send(struct imapc_client_mailbox *box)
{
	timeout_remove(&box->to_send_idle);
	if (imapc_client_mailbox_is_opened(box))
		imapc_connection_idle(box->conn);
}

void imapc_client_mailbox_idle(struct imapc_client_mailbox *box)
{
	/* send the IDLE with a delay to avoid unnecessary IDLEs that are
	   immediately aborted */
	if (box->to_send_idle == NULL && imapc_client_mailbox_is_opened(box)) {
		box->to_send_idle =
			timeout_add_short(IMAPC_CLIENT_IDLE_SEND_DELAY_MSECS,
					  imapc_client_mailbox_idle_send, box);
	}
	/* we're done with all work at this point. */
	box->reconnect_ok = TRUE;
}

bool imapc_client_mailbox_is_opened(struct imapc_client_mailbox *box)
{
	struct imapc_client_mailbox *selected_box;

	if (box->closing ||
	    imapc_connection_get_state(box->conn) != IMAPC_CONNECTION_STATE_DONE)
		return FALSE;

	selected_box = imapc_connection_get_mailbox(box->conn);
	if (selected_box != box) {
		if (selected_box != NULL)
			e_error(imapc_connection_get_event(box->conn),
				"Selected mailbox changed unexpectedly");
		return FALSE;
	}
	return TRUE;
}

static bool
imapc_client_get_any_capabilities(struct imapc_client *client,
				  enum imapc_capability *capabilities_r)
{
	struct imapc_client_connection *conn;

	array_foreach_elem(&client->conns, conn) {
		if (imapc_connection_get_state(conn->conn) == IMAPC_CONNECTION_STATE_DONE) {
			*capabilities_r = imapc_connection_get_capabilities(conn->conn);
			return TRUE;
		}
	}
	return FALSE;
}

int imapc_client_get_capabilities(struct imapc_client *client,
				  enum imapc_capability *capabilities_r)
{
	/* try to find a connection that is already logged in */
	if (imapc_client_get_any_capabilities(client, capabilities_r))
		return 0;

	/* if there are no connections yet, create one */
	if (array_count(&client->conns) == 0)
		(void)imapc_client_add_connection(client);

	/* wait for any of the connections to login */
	client->stop_on_state_finish = TRUE;
	imapc_client_run(client);
	client->stop_on_state_finish = FALSE;
	if (imapc_client_get_any_capabilities(client, capabilities_r))
		return 0;

	/* failed */
	return -1;
}

int imapc_client_create_temp_fd(struct imapc_client *client,
				const char **path_r)
{
	string_t *path;
	int fd;

	if (client->params.temp_path_prefix == NULL) {
		e_error(client->event,
			"temp_path_prefix not set, can't create temp file");
		return -1;
	}

	path = t_str_new(128);
	str_append(path, client->params.temp_path_prefix);
	fd = safe_mkstemp(path, 0600, (uid_t)-1, (gid_t)-1);
	if (fd == -1) {
		e_error(client->event,
			"safe_mkstemp(%s) failed: %m", str_c(path));
		return -1;
	}

	/* we just want the fd, unlink it */
	if (i_unlink(str_c(path)) < 0) {
		/* shouldn't happen.. */
		i_close_fd(&fd);
		return -1;
	}
	*path_r = str_c(path);
	return fd;
}

void imapc_client_register_state_change_callback(struct imapc_client *client,
						 imapc_state_change_callback_t *cb,
						 void *context)
{
	i_assert(client->state_change_callback == NULL);
	i_assert(client->state_change_context == NULL);

	client->state_change_callback = cb;
	client->state_change_context = context;
}

void
imapc_client_set_login_callback(struct imapc_client *client,
				imapc_command_callback_t *callback, void *context)
{
	client->login_callback = callback;
	client->login_context  = context;
}

bool imapc_client_is_ssl(struct imapc_client *client)
{
	return client->ssl_mode != IMAPC_CLIENT_SSL_MODE_NONE;
}
