/* Copyright (c) 2002-2018 Dovecot authors, see the included COPYING file */

#include "auth-common.h"
#include "array.h"
#include "auth-settings.h"
#include "ioloop.h"
#include "net.h"
#include "lib-signals.h"
#include "restrict-access.h"
#include "child-wait.h"
#include "sql-api.h"
#include "module-dir.h"
#include "randgen.h"
#include "process-title.h"
#include "settings.h"
#include "master-service.h"
#include "master-service-settings.h"
#include "master-interface.h"
#include "dict.h"
#include "password-scheme.h"
#include "passdb-cache.h"
#include "mech.h"
#include "otp.h"
#include "mech-otp-common.h"
#include "auth.h"
#include "auth-penalty.h"
#include "auth-token.h"
#include "auth-request-handler.h"
#include "auth-worker-server.h"
#include "auth-worker-connection.h"
#include "auth-master-connection.h"
#include "auth-client-connection.h"
#include "auth-policy.h"
#include "db-oauth2.h"

#include <unistd.h>
#include <sys/stat.h>

#define AUTH_PENALTY_ANVIL_PATH "anvil-auth-penalty"

enum auth_socket_type {
	AUTH_SOCKET_AUTH,
	AUTH_SOCKET_AUTH_LEGACY,
	AUTH_SOCKET_LOGIN,
	AUTH_SOCKET_MASTER,
	AUTH_SOCKET_USERDB,
	AUTH_SOCKET_TOKEN,
	AUTH_SOCKET_TOKEN_LOGIN,

	AUTH_SOCKET_TOKEN_COUNT
};

struct auth_socket_listener {
	struct stat st;
	char *path;
};

static const char *const auth_socket_type_names[] = {
	"auth",
	"auth-legacy",
	"login",
	"master",
	"userdb",
	"token",
	"token-login",
};
static_assert_array_size(auth_socket_type_names, AUTH_SOCKET_TOKEN_COUNT);

bool worker = FALSE, worker_restart_request = FALSE;
time_t process_start_time;
struct auth_penalty *auth_penalty;

static struct module *modules = NULL;
static struct mechanisms_register *mech_reg;
static ARRAY(struct auth_socket_listener) listeners;

void auth_refresh_proctitle(void)
{
	if (!global_auth_settings->verbose_proctitle || worker)
		return;

	process_title_set(t_strdup_printf(
		"[%u wait, %u passdb, %u userdb]",
		auth_request_state_count[AUTH_REQUEST_STATE_NEW] +
		auth_request_state_count[AUTH_REQUEST_STATE_MECH_CONTINUE] +
		auth_request_state_count[AUTH_REQUEST_STATE_FINISHED],
		auth_request_state_count[AUTH_REQUEST_STATE_PASSDB],
		auth_request_state_count[AUTH_REQUEST_STATE_USERDB]));
}

static const char *const *read_global_settings(void)
{
	struct master_service_settings_output set_output;

	auth_settings_read(&set_output);
	global_auth_settings = auth_settings_get(NULL);
	if (set_output.specific_protocols == NULL)
		return t_new(const char *, 1);
	return set_output.specific_protocols;
}

static enum auth_socket_type auth_socket_type_get(const char *typename)
{
	unsigned int i;

	for (i = 0; i < N_ELEMENTS(auth_socket_type_names); i++) {
		if (strcmp(typename, auth_socket_type_names[i]) == 0)
			return (enum auth_socket_type)i;
	}

	/* Deprecated name suffixes */
	if (strcmp(typename, "tokenlogin") == 0)
		return AUTH_SOCKET_TOKEN_LOGIN;

	return AUTH_SOCKET_AUTH;
}

static void listeners_init(void)
{
	unsigned int i, n;
	const char *path;

	i_array_init(&listeners, 8);
	n = master_service_get_socket_count(master_service);
	for (i = 0; i < n; i++) {
		int fd = MASTER_LISTEN_FD_FIRST + i;
		struct auth_socket_listener *l;

		l = array_idx_get_space(&listeners, fd);
		if (net_getunixname(fd, &path) < 0) {
			if (errno != ENOTSOCK)
				i_fatal("getunixname(%d) failed: %m", fd);
			/* not a unix socket */
		} else {
			l->path = i_strdup(path);
			if (stat(path, &l->st) < 0)
				i_error("stat(%s) failed: %m", path);
		}
	}
}

static bool auth_module_filter(const char *name, void *context ATTR_UNUSED)
{
	if (str_begins_with(name, "authdb_") ||
	    str_begins_with(name, "mech_")) {
		/* this is lazily loaded */
		return FALSE;
	}
	return TRUE;
}

static void main_preinit(void)
{
	struct module_dir_load_settings mod_set;
	const char *const *protocols;

	/* Load built-in SQL drivers (if any) */
	sql_drivers_init();

	/* Initialize databases so their configuration files can be readable
	   only by root. Also load all modules here. */
	passdbs_init();
	userdbs_init();
	/* init schemes before plugins are loaded */
	password_schemes_register_all();

	protocols = read_global_settings();

	i_zero(&mod_set);
	mod_set.abi_version = DOVECOT_ABI_VERSION;
	mod_set.require_init_funcs = TRUE;
	mod_set.debug = global_auth_settings->debug;
	mod_set.filter_callback = auth_module_filter;

	modules = module_dir_load(AUTH_MODULE_DIR, NULL, &mod_set);
	module_dir_init(modules);

	if (!worker)
		auth_penalty = auth_penalty_init(AUTH_PENALTY_ANVIL_PATH);

	dict_drivers_register_builtin();
	mech_init(global_auth_settings);
	mech_reg = mech_register_init(global_auth_settings);
	auths_preinit(NULL, global_auth_settings, mech_reg, protocols);

	listeners_init();
	if (!worker)
		auth_token_init();

	/* Password lookups etc. may require roots, allow it. */
	restrict_access_by_env(RESTRICT_ACCESS_FLAG_ALLOW_ROOT, NULL);
	restrict_access_allow_coredumps(TRUE);
}

void auth_module_load(const char *name)
{
	const char *names[] = { name, NULL };
	struct module_dir_load_settings mod_set;

	i_zero(&mod_set);
	mod_set.abi_version = DOVECOT_ABI_VERSION;
	mod_set.require_init_funcs = TRUE;
	mod_set.debug = global_auth_settings->debug;
	mod_set.ignore_missing = TRUE;

	modules = module_dir_load_missing(modules, AUTH_MODULE_DIR, names,
					  &mod_set);
	module_dir_init(modules);
}

static void main_init(void)
{
        process_start_time = ioloop_time;

	/* If auth caches aren't used, just ignore these signals */
	lib_signals_ignore(SIGHUP, TRUE);
	lib_signals_ignore(SIGUSR2, TRUE);

	/* set proctitles before init()s, since they may set them to error */
	auth_refresh_proctitle();
	auth_worker_refresh_proctitle("");

	child_wait_init();
	auth_worker_connection_init();
	auths_init();
	auth_request_handler_init();
	auth_policy_init();

	if (global_auth_settings->allow_weak_schemes)
		password_schemes_allow_weak(TRUE);

	if (worker) {
		/* workers have only a single connection from the master
		   auth process */
		master_service_set_client_limit(master_service, 1);
		auth_worker_set_max_restart_request_count(
			master_service_get_restart_request_count(master_service));
		/* make sure this process cycles if auth connection drops */
		master_service_set_restart_request_count(master_service, 1);
	} else {
		/* caching is handled only by the main auth process */
		passdb_cache_init(global_auth_settings);
		if (global_auth_settings->allow_weak_schemes)
			i_warning("Weak password schemes are allowed");
	}
}

static void main_deinit(void)
{
	struct auth_socket_listener *l;

	shutting_down = TRUE;
	if (auth_penalty != NULL) {
		/* cancel all pending anvil penalty lookups */
		auth_penalty_deinit(&auth_penalty);
	}
	/* deinit auth workers, which aborts pending requests */
        auth_worker_connection_deinit();
	/* deinit passdbs and userdbs. it aborts any pending async requests. */
	auths_deinit();
	/* flush pending requests */
	auth_request_handler_deinit();
	/* there are no more auth requests */
	auths_free();
	dict_drivers_unregister_builtin();

	auth_token_deinit();

	auth_client_connections_destroy_all();
	auth_master_connections_destroy_all();
	auth_worker_connections_destroy_all();

	auth_policy_deinit();
	mech_register_deinit(&mech_reg);
	mech_otp_deinit();
	db_oauth2_deinit();
	mech_deinit(global_auth_settings);
	settings_free(global_auth_settings);

	/* allow modules to unregister their dbs/drivers/etc. before freeing
	   the whole data structures containing them. */
	module_dir_unload(&modules);

	userdbs_deinit();
	passdbs_deinit();
	passdb_cache_deinit();
        password_schemes_deinit();

	sql_drivers_deinit();
	child_wait_deinit();

	array_foreach_modifiable(&listeners, l)
		i_free(l->path);
	array_free(&listeners);
}

static void worker_connected(struct master_service_connection *conn)
{
	if (auth_worker_has_connections()) {
		e_error(auth_event,
			"Auth workers can handle only a single client");
		return;
	}

	master_service_client_connection_accept(conn);
	(void)auth_worker_server_create(auth_default_protocol(), conn);
}

static void client_connected(struct master_service_connection *conn)
{
	struct auth_socket_listener *l;
	struct auth *auth;
	const char *type;

	l = array_idx_modifiable(&listeners, conn->listen_fd);
	if (l->path == NULL)
		l->path = i_strdup(conn->name);

	type = master_service_connection_get_type(conn);
	auth = auth_default_protocol();
	switch (auth_socket_type_get(type)) {
	case AUTH_SOCKET_MASTER:
		(void)auth_master_connection_create(auth, conn->fd,
						    l->path, NULL, FALSE);
		break;
	case AUTH_SOCKET_USERDB:
		(void)auth_master_connection_create(auth, conn->fd,
						    l->path, &l->st, TRUE);
		break;
	case AUTH_SOCKET_LOGIN:
		auth_client_connection_create(auth, conn->fd, conn->name,
					      AUTH_CLIENT_CONNECTION_FLAG_LOGIN_REQUESTS);
		break;
	case AUTH_SOCKET_AUTH:
		auth_client_connection_create(auth, conn->fd, conn->name, 0);
		break;
	case AUTH_SOCKET_AUTH_LEGACY:
		auth_client_connection_create(auth, conn->fd, conn->name,
			AUTH_CLIENT_CONNECTION_FLAG_LEGACY);
		break;
	case AUTH_SOCKET_TOKEN_LOGIN:
		auth_client_connection_create(auth, conn->fd, conn->name,
			AUTH_CLIENT_CONNECTION_FLAG_LOGIN_REQUESTS |
			AUTH_CLIENT_CONNECTION_FLAG_TOKEN_AUTH);
		break;
	case AUTH_SOCKET_TOKEN:
		auth_client_connection_create(auth, conn->fd, conn->name,
			AUTH_CLIENT_CONNECTION_FLAG_TOKEN_AUTH);
		break;
	default:
		i_unreached();
	}
	master_service_client_connection_accept(conn);
}

static void auth_die(void)
{
	if (!worker) {
		/* do nothing. auth clients should disconnect soon. */
	} else {
		/* ask auth master to disconnect us */
		auth_worker_server_send_shutdown();
	}
}

int main(int argc, char *argv[])
{
	int c;
	enum master_service_flags service_flags =
		MASTER_SERVICE_FLAG_NO_SSL_INIT;

	master_service = master_service_init("auth", service_flags, &argc, &argv, "w");
	master_service_init_log(master_service);

	while ((c = master_getopt(master_service)) > 0) {
		switch (c) {
		case 'w':
			master_service_init_log_with_pid(master_service);
			worker = TRUE;
			break;
		default:
			return FATAL_DEFAULT;
		}
	}

	main_preinit();
	master_service_set_die_callback(master_service, auth_die);
	main_init();
	master_service_init_finish(master_service);
	master_service_run(master_service, worker ? worker_connected :
			   client_connected);
	main_deinit();
	master_service_deinit(&master_service);
        return 0;
}
