/* Copyright (c) 2009-2018 Dovecot authors, see the included COPYING file */

#include "lmtp-common.h"
#include "smtp-server.h"
#include "str.h"
#include "istream.h"
#include "strescape.h"
#include "time-util.h"
#include "hostpid.h"
#include "restrict-access.h"
#include "anvil-client.h"
#include "settings.h"
#include "mail-storage.h"
#include "mail-storage-service.h"
#include "mail-namespace.h"
#include "mail-deliver.h"
#include "mail-autoexpunge.h"
#include "index/raw/raw-storage.h"
#include "smtp-common.h"
#include "smtp-params.h"
#include "smtp-address.h"
#include "smtp-submit-settings.h"
#include "lda-settings.h"
#include "lmtp-settings.h"
#include "lmtp-recipient.h"
#include "lmtp-local.h"

struct lmtp_local_recipient {
	struct lmtp_recipient *rcpt;

	struct mail_storage_service_user *service_user;
	struct anvil_query *anvil_query;
	guid_128_t anvil_conn_guid;

	struct lmtp_local_recipient *duplicate;
	const struct lda_settings *lda_set;

	bool anvil_connect_sent:1;
};

struct lmtp_local {
	struct client *client;

	ARRAY(struct lmtp_local_recipient *) rcpt_to;

	struct mail *raw_mail, *first_saved_mail;
	struct mail_user *rcpt_user;

	struct smtp_server_stats stats;
};

/*
 * LMTP local
 */

static struct lmtp_local *
lmtp_local_init(struct client *client)
{
	struct lmtp_local *local;

	local = i_new(struct lmtp_local, 1);
	local->client = client;
	i_array_init(&local->rcpt_to, 8);
	const struct smtp_server_stats *stats =
	        smtp_server_connection_get_stats(local->client->conn);
	local->stats = *stats;
	return local;
}

void lmtp_local_deinit(struct lmtp_local **_local)
{
	struct lmtp_local *local = *_local;

	*_local = NULL;

	if (array_is_created(&local->rcpt_to))
		array_free(&local->rcpt_to);

	if (local->raw_mail != NULL) {
		struct mailbox_transaction_context *raw_trans =
			local->raw_mail->transaction;
		struct mailbox *raw_box = local->raw_mail->box;

		mail_free(&local->raw_mail);
		mailbox_transaction_rollback(&raw_trans);
		mailbox_free(&raw_box);
	}

	i_free(local);
}

/*
 * Recipient
 */

static void
lmtp_local_rcpt_anvil_disconnect(struct lmtp_local_recipient *llrcpt)
{
	const struct mail_storage_service_input *input;

	if (!llrcpt->anvil_connect_sent)
		return;
	llrcpt->anvil_connect_sent = FALSE;

	input = mail_storage_service_user_get_input(llrcpt->service_user);
	struct master_service_anvil_session anvil_session = {
		.username = input->username,
		.service_name = master_service_get_name(master_service),
	};
	master_service_anvil_disconnect(master_service, &anvil_session,
					llrcpt->anvil_conn_guid);
}

static void
lmtp_local_rcpt_destroy(struct smtp_server_recipient *rcpt ATTR_UNUSED,
			struct lmtp_local_recipient *llrcpt)
{
	if (llrcpt->anvil_query != NULL)
		anvil_client_query_abort(anvil, &llrcpt->anvil_query);
	lmtp_local_rcpt_anvil_disconnect(llrcpt);
	settings_free(llrcpt->lda_set);
	mail_storage_service_user_unref(&llrcpt->service_user);
}

static void
lmtp_local_rcpt_reply_overquota(struct lmtp_local_recipient *llrcpt,
				const char *error)
{
	struct smtp_server_recipient *rcpt = llrcpt->rcpt->rcpt;

	if (llrcpt->lda_set->quota_full_tempfail)
		smtp_server_recipient_reply(rcpt, 452, "4.2.2", "%s", error);
	else
		smtp_server_recipient_reply(rcpt, 552, "5.2.2", "%s", error);
}

static void ATTR_FORMAT(4,5)
lmtp_local_rcpt_fail_all(struct lmtp_local *local,
			 unsigned int status, const char *enh_code,
			 const char *fmt, ...)
{
	struct lmtp_local_recipient *const *llrcpts;
	const char *msg;
	unsigned int count, i;
	va_list args;

	va_start(args, fmt);
	msg = t_strdup_vprintf(fmt, args);
	va_end(args);

	llrcpts = array_get(&local->rcpt_to, &count);
	for (i = 0; i < count; i++) {
		struct smtp_server_recipient *rcpt = llrcpts[i]->rcpt->rcpt;

		smtp_server_recipient_reply(rcpt, status, enh_code, "%s", msg);
	}
}

/*
 * RCPT command
 */

static int
lmtp_local_rcpt_check_quota(struct lmtp_local_recipient *llrcpt)
{
	struct client *client = llrcpt->rcpt->client;
	struct smtp_server_recipient *rcpt = llrcpt->rcpt->rcpt;
	struct smtp_address *address = rcpt->path;
	struct mail_user *user;
	struct mail_namespace *ns;
	struct mailbox *box;
	struct mailbox_status status;
	enum mail_error mail_error;
	const char *error;
	int ret;

	if (!client->lmtp_set->lmtp_rcpt_check_quota)
		return 0;

	/* mail user will be created second time when mail is saved,
	   so it's session_id needs to be different,
	   but second time session_id needs to be the same as rcpt session_id and
	   mail user session id for the first rcpt should not overlap with session id
	   of the second recipient, so add custom ":quota" suffix to the session_id without
	   session_id counter increment, so next time mail user will get
	   the same session id as rcpt */
	ret = mail_storage_service_next_with_session_suffix(storage_service,
							    llrcpt->service_user,
							    "quota",
							    &user, &error);

	if (ret < 0) {
		e_error(rcpt->event, "Failed to initialize user %s: %s",
			smtp_address_encode(address), error);
		ret = -1;
	} else {
		/* Set the log prefix for the user. The default log prefix is
		   automatically restored later when user context gets
		   deactivated. */
		i_set_failure_prefix("%s",
			mail_storage_service_user_get_log_prefix(llrcpt->service_user));
		ns = mail_namespace_find_inbox(user->namespaces);
		box = mailbox_alloc(ns->list, "INBOX", 0);
		ret = mailbox_get_status(box, STATUS_CHECK_OVER_QUOTA, &status);
		if (ret < 0) {
			error = mailbox_get_last_error(box, &mail_error);
			if (mail_error == MAIL_ERROR_NOQUOTA) {
				lmtp_local_rcpt_reply_overquota(llrcpt, error);
			} else {
				e_error(rcpt->event,
					"mailbox_get_status(%s, STATUS_CHECK_OVER_QUOTA) "
					"failed: %s",
					mailbox_get_vname(box),
					mailbox_get_last_internal_error(box, NULL));
			}
			ret = -1;
		}
		mailbox_free(&box);
		mail_user_deinit(&user);
		mail_storage_service_io_deactivate_user(llrcpt->service_user);
	}

	if (ret < 0 && !smtp_server_recipient_is_replied(rcpt)) {
		smtp_server_recipient_reply(rcpt, 451, "4.3.0",
					    "Temporary internal error");
	}
	return ret;
}

static void
lmtp_local_rcpt_approved(struct smtp_server_recipient *rcpt,
			 struct lmtp_local_recipient *llrcpt)
{
	struct client *client = llrcpt->rcpt->client;
	struct lmtp_recipient *drcpt;

	/* resolve duplicate recipient */
	drcpt = lmtp_recipient_find_duplicate(llrcpt->rcpt, rcpt->trans);
	if (drcpt != NULL) {
		i_assert(drcpt->type == LMTP_RECIPIENT_TYPE_LOCAL);
		llrcpt->duplicate = drcpt->backend_context;
		i_assert(llrcpt->duplicate->duplicate == NULL);
	}

	/* add to local recipients */
	array_push_back(&client->local->rcpt_to, &llrcpt);
}

static bool
lmtp_local_rcpt_anvil_finish(struct lmtp_local_recipient *llrcpt)
{
	struct smtp_server_recipient *rcpt = llrcpt->rcpt->rcpt;
	struct smtp_server_cmd_ctx *cmd = rcpt->cmd;

	if (lmtp_local_rcpt_check_quota(llrcpt) < 0)
		return FALSE;

	smtp_server_cmd_rcpt_reply_success(cmd);
	return TRUE;
}

static void
lmtp_local_rcpt_anvil_cb(const struct anvil_reply *reply,
			 struct lmtp_local_recipient *llrcpt)
{
	struct client *client = llrcpt->rcpt->client;
	struct smtp_server_recipient *rcpt = llrcpt->rcpt->rcpt;
	const struct mail_storage_service_input *input;
	unsigned int parallel_count = 0;

	llrcpt->anvil_query = NULL;
	if (reply->error != NULL) {
		e_error(rcpt->event,
			"lmtp_user_concurrency_limit lookup failed - skipping: %s",
			reply->error);
	} else if (str_to_uint(reply->reply, &parallel_count) < 0) {
		e_error(rcpt->event, "Invalid reply from anvil: %s",
			reply->reply);
	}

	if (parallel_count >= client->lmtp_set->lmtp_user_concurrency_limit) {
		smtp_server_recipient_reply(
			rcpt, 451, "4.3.0",
			"Too many concurrent deliveries for user");
	} else if (lmtp_local_rcpt_anvil_finish(llrcpt)) {
		input = mail_storage_service_user_get_input(llrcpt->service_user);
		struct master_service_anvil_session anvil_session = {
			.username = input->username,
			.service_name = master_service_get_name(master_service),
		};
		if (master_service_anvil_connect(master_service, &anvil_session,
						 FALSE, llrcpt->anvil_conn_guid))
			llrcpt->anvil_connect_sent = TRUE;
	}
}

int lmtp_local_rcpt(struct client *client,
		    struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
		    struct lmtp_recipient *lrcpt)
{
	struct smtp_server_recipient *rcpt = lrcpt->rcpt;
	struct lmtp_local_recipient *llrcpt;
	struct mail_storage_service_input input;
	struct mail_storage_service_user *service_user;
	const char *error = NULL, *username = lrcpt->username;
	int ret = 0;

	i_zero(&input);
	input.service = "lmtp";
	input.username = username;
	input.local_ip = client->local_ip;
	input.remote_ip = client->remote_ip;
	input.local_port = client->local_port;
	input.remote_port = client->remote_port;
	input.session_id = lrcpt->session_id;
	input.local_name = client->local_name;
	input.end_client_tls_secured =
		client->end_client_tls_secured_set ?
		client->end_client_tls_secured :
		smtp_server_connection_is_ssl_secured(client->conn);
	input.forward_fields = lrcpt->forward_fields;
	input.event_parent = rcpt->event;

	ret = mail_storage_service_lookup(storage_service, &input,
					  &service_user, &error);
	if (ret < 0) {
		e_error(rcpt->event, "Failed to lookup user %s: %s",
			username, error);
		smtp_server_recipient_reply(rcpt, 451, "4.3.0",
					    "Temporary internal error");
		return -1;
	}
	if (ret == 0) {
		smtp_server_recipient_reply(rcpt, 550, "5.1.1",
					    "User doesn't exist: %s",
					    username);
		return -1;
	}

	if (client->local == NULL)
		client->local = lmtp_local_init(client);

	llrcpt = p_new(rcpt->pool, struct lmtp_local_recipient, 1);
	llrcpt->rcpt = lrcpt;
	llrcpt->service_user = service_user;

	lrcpt->type = LMTP_RECIPIENT_TYPE_LOCAL;
	lrcpt->backend_context = llrcpt;

	if (settings_get(mail_storage_service_user_get_event(service_user),
			 &lda_setting_parser_info,
			 0, &llrcpt->lda_set, &error) < 0) {
		e_error(rcpt->event, "%s", error);
		smtp_server_recipient_reply(rcpt, 451, "4.3.0",
					    "Temporary internal error");
		return -1;
	}

	smtp_server_recipient_add_hook(
		rcpt, SMTP_SERVER_RECIPIENT_HOOK_DESTROY,
		lmtp_local_rcpt_destroy, llrcpt);
	smtp_server_recipient_add_hook(
		rcpt, SMTP_SERVER_RECIPIENT_HOOK_APPROVED,
		lmtp_local_rcpt_approved, llrcpt);

	if (client->lmtp_set->lmtp_user_concurrency_limit == SET_UINT_UNLIMITED) {
		(void)lmtp_local_rcpt_anvil_finish(llrcpt);
	} else {
		/* NOTE: username may change as the result of the userdb
		   lookup. Look up the new one via service_user. */
		const struct mail_storage_service_input *input =
			mail_storage_service_user_get_input(llrcpt->service_user);
		const char *query = t_strconcat("LOOKUP\t",
			str_tabescape(input->username), "\t",
			master_service_get_name(master_service), "\t", NULL);
		llrcpt->anvil_query = anvil_client_query(anvil, query,
			ANVIL_DEFAULT_LOOKUP_TIMEOUT_MSECS,
			lmtp_local_rcpt_anvil_cb, llrcpt);
		return 0;
	}

	return 1;
}

/*
 * DATA command
 */

void lmtp_local_add_headers(struct lmtp_local *local,
			    struct smtp_server_transaction *trans,
			    string_t *headers)
{
	struct client *client = local->client;
	const struct lmtp_settings *lmtp_set = client->lmtp_set;
	struct lmtp_local_recipient *const *llrcpts;
	const struct smtp_address *rcpt_to = NULL;
	unsigned int count;

	str_printfa(headers, "Return-Path: <%s>\r\n",
		    smtp_address_encode(trans->mail_from));

	llrcpts = array_get(&local->rcpt_to, &count);
	if (count == 1) {
		struct smtp_server_recipient *rcpt = llrcpts[0]->rcpt->rcpt;

		switch (lmtp_set->parsed_lmtp_hdr_delivery_address) {
		case LMTP_HDR_DELIVERY_ADDRESS_NONE:
			break;
		case LMTP_HDR_DELIVERY_ADDRESS_FINAL:
			rcpt_to = rcpt->path;
			break;
		case LMTP_HDR_DELIVERY_ADDRESS_ORIGINAL:
			rcpt_to = rcpt->params.orcpt.addr;
			break;
		}
	}
	if (rcpt_to != NULL) {
		str_printfa(headers, "Delivered-To: %s\r\n",
			smtp_address_encode(rcpt_to));
	}
}

static int
lmtp_local_deliver(struct lmtp_local *local,
		   struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
		   struct smtp_server_transaction *trans,
		   struct lmtp_local_recipient *llrcpt,
		   struct mail *src_mail,
		   struct mail_deliver_session *session)
{
	struct client *client = local->client;
	struct lmtp_recipient *lrcpt = llrcpt->rcpt;
	struct smtp_server_recipient *rcpt = lrcpt->rcpt;
	struct mail_storage_service_user *service_user = llrcpt->service_user;
	struct lmtp_local_deliver_context lldctx;
	struct mail_user *rcpt_user;
	const struct mail_storage_service_input *input;
	const struct lmtp_pre_mail_settings *pre_mail_set;
	struct smtp_proxy_data proxy_data;
	struct mail_namespace *ns;
	const char *error, *username;
	int ret;

	input = mail_storage_service_user_get_input(service_user);
	username = t_strdup(input->username);

	if (settings_get(mail_storage_service_user_get_event(service_user),
			 &lmtp_pre_mail_setting_parser_info,
			 SETTINGS_GET_FLAG_NO_EXPAND,
			 &pre_mail_set, &error) < 0) {
		e_error(rcpt->event, "%s", error);
		smtp_server_recipient_reply(rcpt, 451, "4.3.0",
					    "Temporary internal error");
		return -1;
	}

	smtp_server_connection_get_proxy_data
		(client->conn, &proxy_data);
	if (proxy_data.timeout_secs > 0 &&
	    (pre_mail_set->mail_max_lock_timeout == 0 ||
	     pre_mail_set->mail_max_lock_timeout > proxy_data.timeout_secs)) {
		/* set lock timeout waits to be less than when proxy has
		   advertised that it's going to timeout the connection.
		   this avoids duplicate deliveries in case the delivery
		   succeeds after the proxy has already disconnected from us. */
		struct settings_instance *set_instance =
			mail_storage_service_user_get_settings_instance(service_user);
		const char *value = t_strdup_printf("%us",
				       proxy_data.timeout_secs <= 1 ? 1 :
				       proxy_data.timeout_secs-1);
		settings_override(set_instance, "*/mail_max_lock_timeout",
				  value, SETTINGS_OVERRIDE_TYPE_CODE);
	}
	settings_free(pre_mail_set);

	i_zero(&lldctx);
	lldctx.session_id = lrcpt->session_id;
	lldctx.src_mail = src_mail;
	lldctx.session = session;

	/* get the timestamp before user is created, since it starts the I/O */
	io_loop_time_refresh();
	lldctx.delivery_time_started = ioloop_timeval;

	client_update_data_state(client, username);
	if (mail_storage_service_next(storage_service, service_user,
				      &rcpt_user, &error) < 0) {
		e_error(rcpt->event, "Failed to initialize user: %s", error);
		smtp_server_recipient_reply(rcpt, 451, "4.3.0",
					    "Temporary internal error");
		return -1;
	}
	local->rcpt_user = rcpt_user;

	/* Set the log prefix for the user. The default log prefix is
	   automatically restored later when user context gets deactivated. */
	i_set_failure_prefix("%s",
		mail_storage_service_user_get_log_prefix(service_user));

	lldctx.rcpt_user = rcpt_user;
	if (settings_get(rcpt_user->event, &smtp_submit_setting_parser_info, 0,
			 &lldctx.smtp_set, &error) < 0) {
		e_error(rcpt->event, "%s", error);
		smtp_server_recipient_reply(rcpt, 451, "4.3.0",
					    "Temporary internal error");
		return -1;
	}

	if (*lrcpt->detail == '\0' ||
	    !client->lmtp_set->lmtp_save_to_detail_mailbox)
		lldctx.rcpt_default_mailbox = "INBOX";
	else {
		ns = mail_namespace_find_inbox(rcpt_user->namespaces);
		lldctx.rcpt_default_mailbox =
			t_strconcat(ns->prefix, lrcpt->detail, NULL);
	}

	ret = client->v.local_deliver(client, lrcpt, cmd, trans, &lldctx);

	lmtp_local_rcpt_anvil_disconnect(llrcpt);

	settings_free(lldctx.smtp_set);
	return ret;
}

static int
lmtp_local_default_do_deliver(struct lmtp_local *local,
			      struct lmtp_local_recipient *llrcpt,
			      struct lmtp_local_deliver_context *lldctx,
			      struct mail_deliver_context *dctx)
{
	struct smtp_server_recipient *rcpt = llrcpt->rcpt->rcpt;
	enum mail_deliver_error error_code;
	const char *error;

	if (mail_deliver(dctx, &error_code, &error) == 0) {
		if (dctx->dest_mail != NULL) {
			i_assert(local->first_saved_mail == NULL);
			local->first_saved_mail = dctx->dest_mail;
		}
		smtp_server_recipient_reply(rcpt, 250, "2.0.0", "%s Saved",
					    lldctx->session_id);
		return 0;
	}

	switch (error_code) {
	case MAIL_DELIVER_ERROR_NONE:
		i_unreached();
	case MAIL_DELIVER_ERROR_TEMPORARY:
		smtp_server_recipient_reply(rcpt, 451, "4.2.0", "%s", error);
		break;
	case MAIL_DELIVER_ERROR_REJECTED:
		smtp_server_recipient_reply(rcpt, 552, "5.2.0", "%s", error);
		break;
	case MAIL_DELIVER_ERROR_NOQUOTA:
		lmtp_local_rcpt_reply_overquota(llrcpt, error);
		break;
	case MAIL_DELIVER_ERROR_INTERNAL:
		/* This shouldn't happen */
		smtp_server_recipient_reply(rcpt, 451, "4.3.0", "%s", error);
		break;
	}

	return -1;
}

int lmtp_local_default_deliver(struct client *client,
			       struct lmtp_recipient *lrcpt,
			       struct smtp_server_cmd_ctx *cmd ATTR_UNUSED,
			       struct smtp_server_transaction *trans,
			       struct lmtp_local_deliver_context *lldctx)
{
	struct lmtp_local *local = client->local;
	struct lmtp_local_recipient *llrcpt = lrcpt->backend_context;
	struct smtp_server_recipient *rcpt = lrcpt->rcpt;
	struct smtp_address *rcpt_to = rcpt->path;
	struct mail_deliver_input dinput;
	struct mail_deliver_context dctx;
	struct event *event;
	int ret;

	event = event_create(lldctx->rcpt_user->event);
	event_drop_parent_log_prefixes(event, 3);

	i_zero(&dinput);
	dinput.session = lldctx->session;
	dinput.set = llrcpt->lda_set;
	dinput.smtp_set = lldctx->smtp_set;
	dinput.session_id = lldctx->session_id;
	dinput.event_parent = event;
	dinput.src_mail = lldctx->src_mail;

	/* MAIL FROM */
	dinput.mail_from = trans->mail_from;
	dinput.mail_params = trans->params;

	/* RCPT TO */
	dinput.rcpt_user = lldctx->rcpt_user;
	dinput.rcpt_params = rcpt->params;
	if (dinput.rcpt_params.orcpt.addr == NULL &&
	    *dinput.set->lda_original_recipient_header != '\0') {
		dinput.rcpt_params.orcpt.addr =
			mail_deliver_get_address(
				lldctx->src_mail,
				dinput.set->lda_original_recipient_header);
	}
	if (dinput.rcpt_params.orcpt.addr == NULL)
		dinput.rcpt_params.orcpt.addr = rcpt_to;
	dinput.rcpt_to = rcpt_to;
	dinput.rcpt_default_mailbox = lldctx->rcpt_default_mailbox;

	dinput.save_dest_mail = array_count(&trans->rcpt_to) > 1 &&
		local->first_saved_mail == NULL;

	dinput.session_time_msecs =
		timeval_diff_msecs(&client->state.data_end_timeval,
				   &trans->timestamp);
	dinput.delivery_time_started = lldctx->delivery_time_started;

	mail_deliver_init(&dctx, &dinput);

	/* Copy statistics to mail user session event here */
	const struct smtp_server_stats *stats =
		smtp_server_connection_get_stats(local->client->conn);
	event_add_int(event, "net_in_bytes",
		      stats->input - local->stats.input);
	event_add_int(event, "net_out_bytes",
		      stats->output - local->stats.output);
	event_add_int(lrcpt->rcpt->event, "net_in_bytes",
		      stats->input - local->stats.input);
	event_add_int(lrcpt->rcpt->event, "net_out_bytes",
		      stats->output - local->stats.output);
	local->stats = *stats;
	ret = lmtp_local_default_do_deliver(local, llrcpt, lldctx, &dctx);
	mail_deliver_deinit(&dctx);

	event_unref(&event);

	return ret;
}

static uid_t
lmtp_local_deliver_to_rcpts(struct lmtp_local *local,
			    struct smtp_server_cmd_ctx *cmd,
			    struct smtp_server_transaction *trans,
			    struct mail_deliver_session *session)
{
	struct client *client = local->client;
	uid_t first_uid = (uid_t)-1;
	struct mail *src_mail;
	struct lmtp_local_recipient *const *llrcpts;
	unsigned int count, i;
	int ret;

	src_mail = local->raw_mail;
	llrcpts = array_get(&local->rcpt_to, &count);
	for (i = 0; i < count; i++) {
		struct lmtp_local_recipient *llrcpt = llrcpts[i];
		struct smtp_server_recipient *rcpt = llrcpt->rcpt->rcpt;

		if (llrcpt->duplicate != NULL) {
			struct smtp_server_recipient *drcpt =
				llrcpt->duplicate->rcpt->rcpt;
			/* don't deliver more than once to the same recipient */
			smtp_server_reply_submit_duplicate(cmd, rcpt->index,
							   drcpt->index);
			continue;
		}

		T_BEGIN {
			ret = lmtp_local_deliver(local, cmd, trans, llrcpt,
						 src_mail, session);
		} T_END;
		client_update_data_state(client, NULL);

		/* succeeded and mail_user is not saved in first_saved_mail */
		if ((ret == 0 &&
		     (local->first_saved_mail == NULL ||
		      local->first_saved_mail == src_mail)) ||
		    /* failed. try the next one. */
		    (ret != 0 && local->rcpt_user != NULL)) {
			if (i == (count - 1))
				mail_user_autoexpunge(local->rcpt_user);
			mail_storage_service_io_deactivate_user(local->rcpt_user->service_user);
			mail_user_deinit(&local->rcpt_user);
		} else if (ret == 0) {
			/* use the first saved message to save it elsewhere too.
			   this might allow hard linking the files.
			   mail_user is saved in first_saved_mail,
			   will be unreferenced later on */
			mail_storage_service_io_deactivate_user(local->rcpt_user->service_user);
			local->rcpt_user = NULL;
			src_mail = local->first_saved_mail;
			first_uid = geteuid();
			i_assert(first_uid != 0);
		} else if (local->rcpt_user != NULL) {
			mail_storage_service_io_deactivate_user(local->rcpt_user->service_user);
		}
	}
	return first_uid;
}

static int
lmtp_local_open_raw_mail(struct lmtp_local *local,
			 struct smtp_server_transaction *trans,
			 struct istream *input)
{
	static const char *wanted_headers[] = {
		"From", "To", "Message-ID", "Subject", "Return-Path",
		NULL
	};
	struct client *client = local->client;
	struct mailbox *box;
	struct mailbox_transaction_context *mtrans;
	struct mailbox_header_lookup_ctx *headers_ctx;
	enum mail_error error;

	if (raw_mailbox_alloc_stream(client->raw_mail_user, input,
				     (time_t)-1, smtp_address_encode(trans->mail_from),
				     &box) < 0) {
		e_error(client->event, "Can't open delivery mail as raw: %s",
			mailbox_get_last_internal_error(box, &error));
		mailbox_free(&box);
		lmtp_local_rcpt_fail_all(local, 451, "4.3.0",
					 "Temporary internal error");
		return -1;
	}

	mtrans = mailbox_transaction_begin(box, 0, __func__);

	headers_ctx = mailbox_header_lookup_init(box, wanted_headers);
	local->raw_mail = mail_alloc(mtrans, 0, headers_ctx);
	mailbox_header_lookup_unref(&headers_ctx);
	mail_set_seq(local->raw_mail, 1);
	return 0;
}

void lmtp_local_data(struct client *client,
		     struct smtp_server_cmd_ctx *cmd,
		     struct smtp_server_transaction *trans,
		     struct istream *input)
{
	struct lmtp_local *local = client->local;
	struct mail_deliver_session *session;
	uid_t old_uid, first_uid;

	if (lmtp_local_open_raw_mail(local, trans, input) < 0)
		return;

	session = mail_deliver_session_init();
	old_uid = geteuid();
	first_uid = lmtp_local_deliver_to_rcpts(local, cmd, trans, session);
	mail_deliver_session_deinit(&session);

	if (local->first_saved_mail != NULL) {
		struct mail *mail = local->first_saved_mail;
		struct mailbox_transaction_context *trans = mail->transaction;
		struct mailbox *box = trans->box;
		struct mail_user *user = box->storage->user;

		/* just in case these functions are going to write anything,
		   change uid back to user's own one */
		if (first_uid != old_uid) {
			if (seteuid(0) < 0)
				i_fatal("seteuid(0) failed: %m");
			if (seteuid(first_uid) < 0)
				i_fatal("seteuid() failed: %m");
		}

		mail_storage_service_io_activate_user(user->service_user);
		mail_free(&mail);
		mailbox_transaction_rollback(&trans);
		mailbox_free(&box);
		mail_user_autoexpunge(user);
		mail_storage_service_io_deactivate_user(user->service_user);
		mail_user_deinit(&user);
	}

	if (old_uid == 0) {
		/* switch back to running as root, since that's what we're
		   practically doing anyway. it's also important in case we
		   lose e.g. config connection and need to reconnect to it. */
		mail_storage_service_restore_privileges(old_uid, base_dir,
							cmd->event);
	}
}
