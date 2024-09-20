#ifndef LDAP_SETTINGS_H
#define LDAP_SETTINGS_H

struct ldap_client_settings {
	pool_t pool;

	const char *uris;
	const char *auth_dn;
	const char *auth_dn_password;

	unsigned int timeout_secs;
	unsigned int max_idle_time_secs;
	unsigned int debug_level;
	bool require_ssl;
	bool starttls;

	struct event *event_parent;
	const struct ssl_iostream_settings *ssl_ioset;
};

#endif
