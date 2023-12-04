/* Copyright (c) 2013-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "net.h"
#include "str.h"
#include "hash.h"
#include "array.h"
#include "ioloop.h"
#include "istream.h"
#include "ostream.h"
#include "connection.h"
#include "dns-lookup.h"
#include "iostream-rawlog.h"
#include "iostream-ssl.h"
#include "settings.h"
#include "http-url.h"

#include "http-server-private.h"

static struct event_category event_category_http_server = {
	.name = "http-server"
};

/*
 * Server
 */

struct http_server *http_server_init(const struct http_server_settings *set,
				     struct event *event_parent)
{
	struct http_server *server;
	pool_t pool;
	size_t pool_size;

	pool_size = (set->ssl != NULL) ? 10240 : 1024; /* ca/cert/key will be >8K */
	pool = pool_alloconly_create("http server", pool_size);
	server = p_new(pool, struct http_server, 1);
	server->pool = pool;

	if (set->default_host != NULL && *set->default_host != '\0')
		server->set.default_host = p_strdup(pool, set->default_host);
	if (set->rawlog_dir != NULL && *set->rawlog_dir != '\0')
		server->set.rawlog_dir = p_strdup(pool, set->rawlog_dir);
	if (set->ssl != NULL) {
		server->set.ssl = set->ssl;
		pool_ref(server->set.ssl->pool);
	}
	server->set.max_client_idle_time_msecs = set->max_client_idle_time_msecs;
	server->set.max_pipelined_requests =
		(set->max_pipelined_requests > 0 ? set->max_pipelined_requests : 1);
	server->set.request_max_target_length = set->request_max_target_length;
	server->set.request_max_payload_size = set->request_max_payload_size;
	server->set.request_hdr_max_size = set->request_hdr_max_size;
	server->set.request_hdr_max_field_size = set->request_hdr_max_field_size;
	server->set.request_hdr_max_fields = set->request_hdr_max_fields;
	server->set.socket_send_buffer_size = set->socket_send_buffer_size;
	server->set.socket_recv_buffer_size = set->socket_recv_buffer_size;

	server->event = event_create(event_parent);
	event_add_category(server->event, &event_category_http_server);
	event_set_append_log_prefix(server->event, "http-server: ");

	server->conn_list = http_server_connection_list_init();

	p_array_init(&server->resources, pool, 4);
	p_array_init(&server->locations, pool, 4);

	return server;
}

void http_server_deinit(struct http_server **_server)
{
	struct http_server *server = *_server;
	struct http_server_resource *res;

	*_server = NULL;

	connection_list_deinit(&server->conn_list);

	array_foreach_elem(&server->resources, res)
		http_server_resource_free(&res);
	i_assert(array_count(&server->locations) == 0);

	settings_free(server->set.ssl);
	event_unref(&server->event);
	pool_unref(&server->pool);
}

void http_server_switch_ioloop(struct http_server *server)
{
	struct connection *_conn = server->conn_list->connections;

	/* move connections */
	/* FIXME: we wouldn't necessarily need to switch all of them
	   immediately, only those that have requests now. but also connections
	   that get new requests before ioloop is switched again.. */
	for (; _conn != NULL; _conn = _conn->next) {
		struct http_server_connection *conn =
			(struct http_server_connection *)_conn;

		http_server_connection_switch_ioloop(conn);
	}
}

void http_server_shut_down(struct http_server *server)
{
	struct connection *_conn, *_next;

	server->shutting_down = TRUE;

	for (_conn = server->conn_list->connections;
		_conn != NULL; _conn = _next) {
		struct http_server_connection *conn =
			(struct http_server_connection *)_conn;

		_next = _conn->next;
		(void)http_server_connection_shut_down(conn);
	}
}
