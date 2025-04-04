/* Copyright (c) 2002-2018 Dovecot authors, see the included COPYING file */

#include "login-common.h"
#include "buffer.h"
#include "ioloop.h"
#include "istream.h"
#include "ostream.h"
#include "safe-memset.h"
#include "str.h"
#include "settings.h"
#include "imap-parser.h"
#include "imap-id.h"
#include "imap-resp-code.h"
#include "master-service.h"
#include "ssl-settings.h"
#include "login-client.h"
#include "imap-login-client.h"
#include "client-authenticate.h"
#include "auth-client.h"
#include "imap-proxy.h"
#include "imap-quote.h"
#include "imap-login-commands.h"
#include "imap-login-settings.h"
#include "imap-util.h"

#if LOGIN_MAX_INBUF_SIZE < 1024+2
#  error LOGIN_MAX_INBUF_SIZE too short to fit all ID command parameters
#endif

/* Disconnect client when it sends too many bad commands */
#define CLIENT_MAX_BAD_COMMANDS 3

/* Skip incoming data until newline is found,
   returns TRUE if newline was found. */
bool client_skip_line(struct imap_client *client)
{
	const unsigned char *data;
	size_t i, data_size;

	data = i_stream_get_data(client->common.input, &data_size);

	for (i = 0; i < data_size; i++) {
		if (data[i] == '\n') {
			i_stream_skip(client->common.input, i+1);
			return TRUE;
		}
	}

	return FALSE;
}

bool client_handle_parser_error(struct imap_client *client,
				struct imap_parser *parser)
{
	const char *msg;
	enum imap_parser_error parse_error;

	msg = imap_parser_get_error(parser, &parse_error);
	switch (parse_error) {
	case IMAP_PARSE_ERROR_NONE:
		i_unreached();
	case IMAP_PARSE_ERROR_LITERAL_TOO_BIG:
		client_send_reply(&client->common,
				  IMAP_CMD_REPLY_BYE, msg);
		client_destroy(&client->common, msg);
		return FALSE;
	default:
		break;
	}

	client_send_reply(&client->common, IMAP_CMD_REPLY_BAD, msg);
	client->cmd_finished = TRUE;
	client->skip_line = TRUE;
	return TRUE;
}

static bool is_login_cmd_disabled(struct client *client)
{
	if (client->connection_secured) {
		if (sasl_server_find_available_mech(client, "PLAIN") == NULL) {
			/* no PLAIN authentication, can't use LOGIN command */
			return TRUE;
		}
		return FALSE;
	}
	if (!client->set->auth_allow_cleartext)
		return TRUE;
	if (strcmp(client->ssl_server_set->ssl, "required") == 0)
		return TRUE;
	return FALSE;
}

static int imap_client_reload_config(struct client *client,
				     const char **error_r);

static const char *get_capability(struct client *client)
{
	struct imap_client *imap_client =
		container_of(client, struct imap_client, common);
	string_t *cap_str = t_str_new(256);

	imap_write_capability(cap_str, &imap_client->set->imap_capability);

	if (client_is_tls_enabled(client) && !client->connection_tls_secured &&
	    !client->haproxy_terminated_tls)
		str_append(cap_str, " STARTTLS");
	if (is_login_cmd_disabled(client))
		str_append(cap_str, " LOGINDISABLED");

	client_authenticate_get_capabilities(client, cap_str);
	return str_c(cap_str);
}

static int cmd_capability(struct imap_client *imap_client,
			  const struct imap_arg *args ATTR_UNUSED)
{
	struct client *client = &imap_client->common;

	/* Client is required to send CAPABILITY after STARTTLS, so the
	   capability resp-code workaround checks only pre-STARTTLS
	   CAPABILITY commands. */
	if (!client->connection_used_starttls)
		imap_client->client_ignores_capability_resp_code = TRUE;
	client_send_raw(client, t_strconcat(
		"* CAPABILITY ", get_capability(client), "\r\n", NULL));
	client_send_reply(client, IMAP_CMD_REPLY_OK,
		"Pre-login capabilities listed, post-login capabilities have more.");
	return 1;
}

static int cmd_starttls(struct imap_client *client,
			const struct imap_arg *args ATTR_UNUSED)
{
	client_cmd_starttls(&client->common);
	return 1;
}

static void
imap_client_notify_starttls(struct client *client,
			    bool success, const char *text)
{
	if (success)
		client_send_reply(client, IMAP_CMD_REPLY_OK, text);
	else
		client_send_reply(client, IMAP_CMD_REPLY_BAD, text);
}

static int cmd_noop(struct imap_client *client,
		    const struct imap_arg *args ATTR_UNUSED)
{
	client_send_reply(&client->common, IMAP_CMD_REPLY_OK,
			  "NOOP completed.");
	return 1;
}

static int cmd_logout(struct imap_client *client,
		      const struct imap_arg *args ATTR_UNUSED)
{
	client_send_reply(&client->common, IMAP_CMD_REPLY_BYE, "Logging out");
	client_send_reply(&client->common, IMAP_CMD_REPLY_OK,
			  "Logout completed.");
	client_destroy(&client->common, CLIENT_UNAUTHENTICATED_LOGOUT_MSG);
	return 1;
}

static int cmd_enable(struct imap_client *client,
		      const struct imap_arg *args ATTR_UNUSED)
{
	client_send_raw(&client->common, "* ENABLED\r\n");
	client_send_reply(&client->common, IMAP_CMD_REPLY_OK,
			  "ENABLE ignored in non-authenticated state.");
	return 1;
}

static int client_command_execute(struct imap_client *client, const char *cmd,
				  const struct imap_arg *args)
{
	struct imap_login_command *login_cmd;

	login_cmd = imap_login_command_lookup(cmd);
	if (login_cmd == NULL)
		return -2;
	return login_cmd->func(client, args);
}

static bool client_invalid_command(struct imap_client *client)
{
	if (client->cmd_tag == NULL || *client->cmd_tag == '\0')
		client->cmd_tag = "*";
	if (++client->common.bad_counter >= CLIENT_MAX_BAD_COMMANDS) {
		client_send_reply(&client->common, IMAP_CMD_REPLY_BYE,
				  "Too many invalid IMAP commands.");
		client_destroy(&client->common, "Too many invalid commands");
		return FALSE;
	}
	client_send_reply(&client->common, IMAP_CMD_REPLY_BAD,
			  "Error in IMAP command received by server.");
	return TRUE;
}

static int client_parse_command(struct imap_client *client,
				const struct imap_arg **args_r)
{
	switch (imap_parser_read_args(client->parser, 0, 0, args_r)) {
	case -1:
		/* error */
		if (!client_handle_parser_error(client, client->parser)) {
			/* client destroyed */
			return 0;
		}
		return -1;
	case -2:
		/* not enough data */
		return 0;
	default:
		/* we read the entire line - skip over the CRLF */
		if (!client_skip_line(client))
			i_unreached();
		return 1;
	}
}

static bool client_handle_input(struct imap_client *client)
{
	const char *tag, *name;
	int ret;

	i_assert(!client->common.authenticating);

	if (client->cmd_finished) {
		/* clear the previous command from memory. don't do this
		   immediately after handling command since we need the
		   cmd_tag to stay some time after authentication commands. */
		client->cmd_tag = NULL;
		client->cmd_name = NULL;
		imap_parser_reset(client->parser);

		/* remove \r\n */
		if (client->skip_line) {
			if (!client_skip_line(client))
				return FALSE;
			client->skip_line = FALSE;
		}

		client->cmd_finished = FALSE;
	}

	if (client->cmd_tag == NULL) {
		ret = imap_parser_read_tag(client->parser, &tag);
		if (ret == 0)
			return FALSE; /* need more data */
		if (ret < 0 || strlen(tag) > IMAP_TAG_MAX_LEN) {
			/* the tag is invalid, don't allow it and don't
			   send it back. this attempts to prevent any
			   potentially dangerous replies in case someone tries
			   to access us using HTTP protocol. */
			client->skip_line = TRUE;
			client->cmd_finished = TRUE;
			if (!client_invalid_command(client))
				return FALSE;
			return client_handle_input(client);
		}
		client->cmd_tag = tag;
	}

	if (client->cmd_name == NULL) {
		ret = imap_parser_read_command_name(client->parser, &name);
		if (ret == 0)
			return FALSE; /* need more data */
		if (ret < 0) {
			client->skip_line = TRUE;
			client->cmd_finished = TRUE;
			if (!client_invalid_command(client))
				return FALSE;
			return client_handle_input(client);
		}
		client->cmd_name = name;
	}
	return client->common.v.input_next_cmd(&client->common);
}

static bool imap_client_input_next_cmd(struct client *_client)
{
	struct imap_client *client =
		container_of(_client, struct imap_client, common);
	const struct imap_arg *args;
	bool parsed;
	int ret;

	if (strcasecmp(client->cmd_name, "AUTHENTICATE") == 0) {
		/* SASL-IR may need more space than input buffer's size,
		   so we'll handle it as a special case. */
		ret = cmd_authenticate(client, &parsed);
		if (ret == 0 && !parsed)
			return FALSE;
	} else if (strcasecmp(client->cmd_name, "ID") == 0) {
		/* ID extensions allows max. 30 parameters,
		   each max. 1024 bytes long. that brings us over the input
		   buffer's size, so handle the parameters one at a time */
		ret = cmd_id(client);
		if (ret == 0)
			return FALSE;
		if (ret < 0)
			ret = 1; /* don't send the error reply again */
	} else {
		ret = client_parse_command(client, &args);
		if (ret < 0)
			return TRUE;
		if (ret == 0)
			return FALSE;
		ret = *client->cmd_tag == '\0' ? -1 :
			client_command_execute(client, client->cmd_name, args);
	}

	client->cmd_finished = TRUE;
	if (ret == -2 && strcasecmp(client->cmd_tag, "LOGIN") == 0) {
		client_send_reply(&client->common, IMAP_CMD_REPLY_BAD,
			"First parameter in line is IMAP's command tag, "
			"not the command name. Add that before the command, "
			"like: a login user pass");
	} else if (ret < 0) {
		if (!client_invalid_command(client))
			return FALSE;
	}

	return ret != 0 && !client->common.destroyed;
}

static void imap_client_input(struct client *client)
{
	struct imap_client *imap_client =
		container_of(client, struct imap_client, common);

	if (!client_read(client))
		return;

	client_ref(client);
	o_stream_cork(imap_client->common.output);
	for (;;) {
		if (!auth_client_is_connected(auth_client)) {
			/* we're not currently connected to auth process -
			   don't allow any commands */
			client_notify_status(client, FALSE,
					     AUTH_SERVER_WAITING_MSG);
			timeout_remove(&client->to_auth_waiting);

			client->input_blocked = TRUE;
			break;
		} else {
			bool ret;
			T_BEGIN {
				ret = client_handle_input(imap_client);
			} T_END;
			if (!ret)
				break;
		}
	}
	o_stream_uncork(imap_client->common.output);
	client_unref(&client);
}

static struct client *imap_client_alloc(pool_t pool)
{
	struct imap_client *imap_client;

	imap_client = p_new(pool, struct imap_client, 1);
	return &imap_client->common;
}

static int imap_client_create(struct client *client)
{
	struct imap_client *imap_client =
		container_of(client, struct imap_client, common);
	const char *error;

	if (settings_get(client->event, &imap_login_setting_parser_info, 0,
			 &imap_client->set, &error) < 0) {
		e_error(client->event, "%s", error);
		return -1;
	}

	imap_client->parser =
		imap_parser_create(imap_client->common.input,
				   imap_client->common.output,
				   IMAP_LOGIN_MAX_LINE_LENGTH);
	struct settings_instance *set_instance = settings_instance_find(client->event);
	if (set_instance == NULL) {
		set_instance = settings_instance_new(
			master_service_get_settings_root(master_service));
		imap_client->set_instance = set_instance;
		event_set_ptr(client->event, SETTINGS_EVENT_INSTANCE, set_instance);
	}

	if (imap_client->set->imap_literal_minus) {
		settings_override(set_instance,
				  "imap_capability/LITERAL+",
				  "no", SETTINGS_OVERRIDE_TYPE_CODE);
		imap_parser_enable_literal_minus(imap_client->parser);
	} else
		settings_override(set_instance,
				  "imap_capability/LITERAL-",
				  "no", SETTINGS_OVERRIDE_TYPE_CODE);
	if (!imap_client->set->imap4rev2_enable)
		settings_override(set_instance, "imap_capability/IMAP4rev2",
				  "no", SETTINGS_OVERRIDE_TYPE_CODE);

	if (imap_client_reload_config(client, &error) < 0) {
		e_error(client->event, "%s", error);
		return -1;
	}
	return 0;
}

static void imap_client_destroy(struct client *client)
{
	struct imap_client *imap_client =
		container_of(client, struct imap_client, common);

	/* Prevent memory leak of ID command if client got disconnected before
	   command was finished. */
	if (imap_client->cmd_id != NULL) {
		i_assert(!imap_client->cmd_finished);
		cmd_id_free(imap_client);
	}

	/* The client may live on as proxying, even though the imap-specific
	   parts get freed. Clear out the settings instance, so it's not
	   attempted to be used anymore. Alternatively we could delay freeing
	   it until the client is freed, but that would require more changes. */
	event_set_ptr(client->event, SETTINGS_EVENT_INSTANCE, NULL);
	settings_instance_free(&imap_client->set_instance);

	settings_free(imap_client->set);
	i_free_and_null(imap_client->proxy_backend_capability);
	imap_parser_unref(&imap_client->parser);
}

static int
imap_client_reload_config(struct client *client, const char **error_r)
{
	struct imap_client *imap_client =
		container_of(client, struct imap_client, common);

	settings_free(imap_client->set);
	return settings_get(client->event, &imap_login_setting_parser_info, 0,
			    &imap_client->set, error_r);
}

static void imap_client_notify_auth_ready(struct client *client)
{
	string_t *greet;

	greet = t_str_new(128);
	str_append(greet, "* OK ");
	str_printfa(greet, "[CAPABILITY %s] ", get_capability(client));
	str_append(greet, client->set->login_greeting);
	str_append(greet, "\r\n");

	client_send_raw(client, str_c(greet));

	client->banner_sent = TRUE;
	i_assert(client->io == NULL);
	client->io = io_add_istream(client->input, client_input, client);
}

static void imap_client_starttls(struct client *client)
{
	struct imap_client *imap_client =
		container_of(client, struct imap_client, common);

	imap_parser_unref(&imap_client->parser);
	imap_client->parser =
		imap_parser_create(imap_client->common.input,
				   imap_client->common.output,
				   IMAP_LOGIN_MAX_LINE_LENGTH);

	/* CRLF is lost from buffer when streams are reopened. */
	imap_client->skip_line = FALSE;
}

static void ATTR_NULL(3)
client_send_reply_raw(struct client *client,
		      const char *prefix, const char *resp_code,
		      const char *text, bool tagged)
{
	struct imap_client *imap_client =
		container_of(client, struct imap_client, common);

	T_BEGIN {
		string_t *line = t_str_new(256);

		if (tagged)
			str_append(line, imap_client->cmd_tag);
		else
			str_append_c(line, '*');
		str_append_c(line, ' ');
		str_append(line, prefix);
		str_append_c(line, ' ');
		if (resp_code != NULL)
			str_printfa(line, "[%s] ", resp_code);
		str_append(line, text);
		str_append(line, "\r\n");

		client_send_raw_data(client, str_data(line), str_len(line));
	} T_END;
}

void client_send_reply_code(struct client *client, enum imap_cmd_reply reply,
			    const char *resp_code, const char *text)
{
	const char *prefix = "NO";
	bool tagged = TRUE;

	switch (reply) {
	case IMAP_CMD_REPLY_OK:
		prefix = "OK";
		break;
	case IMAP_CMD_REPLY_NO:
		break;
	case IMAP_CMD_REPLY_BAD:
		prefix = "BAD";
		break;
	case IMAP_CMD_REPLY_BYE:
		prefix = "BYE";
		tagged = FALSE;
		break;
	}
	client_send_reply_raw(client, prefix, resp_code, text, tagged);
}

void client_send_reply(struct client *client, enum imap_cmd_reply reply,
		       const char *text)
{
	client_send_reply_code(client, reply, NULL, text);
}

static void
imap_client_notify_status(struct client *client, bool bad, const char *text)
{
	if (bad)
		client_send_reply_raw(client, "BAD", "ALERT", text, FALSE);
	else
		client_send_reply_raw(client, "OK", NULL, text, FALSE);
}

static void
imap_client_notify_disconnect(struct client *client,
			      enum client_disconnect_reason reason,
			      const char *text)
{
	if (reason == CLIENT_DISCONNECT_INTERNAL_ERROR) {
		client_send_reply_code(client, IMAP_CMD_REPLY_BYE,
				       IMAP_RESP_CODE_UNAVAILABLE, text);
	} else {
		client_send_reply_code(client, IMAP_CMD_REPLY_BYE, NULL, text);
	}
}

static void imap_login_preinit(void)
{
}

static const struct imap_login_command imap_login_commands[] = {
	{ "LOGIN", cmd_login },
	{ "CAPABILITY", cmd_capability },
	{ "STARTTLS", cmd_starttls },
	{ "NOOP", cmd_noop },
	{ "LOGOUT", cmd_logout },
	{ "ENABLE", cmd_enable }
};

static void imap_login_init(void)
{
	imap_login_commands_init();
	imap_login_commands_register(imap_login_commands,
				     N_ELEMENTS(imap_login_commands));
}

static void imap_login_deinit(void)
{
	clients_destroy_all();
	imap_login_commands_deinit();
}

static struct client_vfuncs imap_client_vfuncs = {
	.alloc = imap_client_alloc,
	.create = imap_client_create,
	.destroy = imap_client_destroy,
	.reload_config = imap_client_reload_config,
	.notify_auth_ready = imap_client_notify_auth_ready,
	.notify_disconnect = imap_client_notify_disconnect,
	.notify_status = imap_client_notify_status,
	.notify_starttls = imap_client_notify_starttls,
	.starttls = imap_client_starttls,
	.input = imap_client_input,
	.auth_result = imap_client_auth_result,
	.proxy_reset = imap_proxy_reset,
	.proxy_parse_line = imap_proxy_parse_line,
	.proxy_side_channel_input = imap_proxy_side_channel_input,
	.proxy_failed = imap_proxy_failed,
	.proxy_get_state = imap_proxy_get_state,
	.send_raw_data = client_common_send_raw_data,
	.input_next_cmd = imap_client_input_next_cmd,
	.free = client_common_default_free,
};

static struct login_binary imap_login_binary = {
	.protocol = "imap",
	.process_name = "imap-login",
	.default_port = 143,
	.default_ssl_port = 993,

	.event_category = {
		.name = "imap",
	},

	.client_vfuncs = &imap_client_vfuncs,
	.preinit = imap_login_preinit,
	.init = imap_login_init,
	.deinit = imap_login_deinit,

	.sasl_support_final_reply = FALSE,
	.anonymous_login_acceptable = TRUE,

	.application_protocols = (const char *const[]) {
		"imap", NULL
	},
};

int main(int argc, char *argv[])
{
	return login_binary_run(&imap_login_binary, argc, argv);
}
