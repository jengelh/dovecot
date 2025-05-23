#ifndef MAILBOX_LIST_PRIVATE_H
#define MAILBOX_LIST_PRIVATE_H

#include "mailbox-log.h"
#include "mailbox-list-notify.h"
#include "mail-namespace.h"
#include "mailbox-list.h"
#include "mailbox-list-iter.h"
#include "mail-storage-settings.h"
#include "mail-index.h"

#define MAILBOX_LIST_NAME_MAILDIRPLUSPLUS "maildir++"
#define MAILBOX_LIST_NAME_IMAPDIR "imapdir"
#define MAILBOX_LIST_NAME_FS "fs"
#define MAILBOX_LIST_NAME_INDEX "index"
#define MAILBOX_LIST_NAME_NONE "none"

#define MAILBOX_LOG_FILE_NAME "dovecot.mailbox.log"

#define T_MAILBOX_LIST_ERR_NOT_FOUND(list, name) \
	t_strdup_printf(MAIL_ERRSTR_MAILBOX_NOT_FOUND, \
			mailbox_list_get_vname(list, name))

struct stat;
struct dirent;
struct fs;
struct imap_match_glob;
struct mailbox_tree_context;
struct mailbox_list_notify;
struct mailbox_list_notify_rec;

#define MAILBOX_INFO_FLAGS_FINISHED(flags) \
	(((flags) & (MAILBOX_SELECT | MAILBOX_NOSELECT | \
		     MAILBOX_NONEXISTENT)) != 0)

struct mailbox_list_vfuncs {
	struct mailbox_list *(*alloc)(void);
	int (*init)(struct mailbox_list *list, const char **error_r);
	void (*deinit)(struct mailbox_list *list);

	int (*get_storage)(struct mailbox_list **list, const char **vname,
			   enum mailbox_list_get_storage_flags flags,
			   struct mail_storage **storage_r);

	char (*get_hierarchy_sep)(struct mailbox_list *list);
	const char *(*get_vname)(struct mailbox_list *list,
				 const char *storage_name);
	const char *(*get_storage_name)(struct mailbox_list *list,
					const char *vname);
	int (*get_path)(struct mailbox_list *list, const char *name,
			enum mailbox_list_path_type type, const char **path_r);

	const char *(*get_temp_prefix)(struct mailbox_list *list, bool global);
	const char *(*join_refpattern)(struct mailbox_list *list,
				       const char *ref, const char *pattern);

	struct mailbox_list_iterate_context *
		(*iter_init)(struct mailbox_list *list,
			     const char *const *patterns,
			     enum mailbox_list_iter_flags flags);
	const struct mailbox_info *
		(*iter_next)(struct mailbox_list_iterate_context *ctx);
	int (*iter_deinit)(struct mailbox_list_iterate_context *ctx);

	int (*get_mailbox_flags)(struct mailbox_list *list,
				 const char *dir, const char *fname,
				 enum mailbox_list_file_type type,
				 enum mailbox_info_flags *flags_r);
	/* Returns TRUE if name is mailbox's internal file/directory.
	   If it does, mailbox deletion assumes it can safely delete it. */
	bool (*is_internal_name)(struct mailbox_list *list, const char *name);

	/* Read subscriptions from src_list, but place them into
	   dest_list->subscriptions. Set errors to dest_list. */
	int (*subscriptions_refresh)(struct mailbox_list *src_list,
				     struct mailbox_list *dest_list);
	int (*set_subscribed)(struct mailbox_list *list,
			      const char *name, bool set);
	int (*delete_mailbox)(struct mailbox_list *list, const char *name);
	int (*delete_dir)(struct mailbox_list *list, const char *name);
	int (*delete_symlink)(struct mailbox_list *list, const char *name);
	int (*rename_mailbox)(struct mailbox_list *oldlist, const char *oldname,
			      struct mailbox_list *newlist, const char *newname);

	int (*notify_init)(struct mailbox_list *list,
			   enum mailbox_list_notify_event mask,
			   struct mailbox_list_notify **notify_r);
	int (*notify_next)(struct mailbox_list_notify *notify,
			   const struct mailbox_list_notify_rec **rec_r);
	void (*notify_deinit)(struct mailbox_list_notify *notify);
	void (*notify_wait)(struct mailbox_list_notify *notify,
			    void (*callback)(void *context), void *context);
	void (*notify_flush)(struct mailbox_list_notify *notify);
};

struct mailbox_list_module_register {
	unsigned int id;
};

union mailbox_list_module_context {
	struct mailbox_list_vfuncs super;
	struct mailbox_list_module_register *reg;
};

struct mailbox_list {
	const char *name;
	struct event *event;
	enum mailbox_list_properties props;
	size_t mailbox_name_max_length;

	struct mailbox_list_vfuncs v, *vlast;

/* private: */
	pool_t pool;
	struct mail_namespace *ns;
	const struct mail_storage_settings *mail_set;
	const struct mailbox_settings *default_box_set;
	enum mailbox_list_flags flags;

	/* may not be set yet, use mailbox_list_get_permissions() to access */
	struct mailbox_permissions root_permissions;

	struct mailbox_tree_context *subscriptions;
	time_t subscriptions_mtime, subscriptions_read_time;

	struct mailbox_log *changelog;
	time_t changelog_timestamp;

	struct file_lock *lock;
	int lock_refcount;

	pool_t guid_cache_pool;
	HASH_TABLE(uint8_t *, struct mailbox_guid_cache_rec *) guid_cache;
	bool guid_cache_errors;

	/* Last error set in mailbox_list_set_critical(). */
	char *last_internal_error;

	char *error_string;
	enum mail_error error;
	bool temporary_error;
	ARRAY(struct mail_storage_error) error_stack;

	ARRAY(union mailbox_list_module_context *) module_contexts;

	bool index_root_dir_created:1;
	bool list_index_root_dir_created:1;
	bool guid_cache_updated:1;
	bool disable_rebuild_on_corruption:1;
	bool guid_cache_invalidated:1;
	bool last_error_is_internal:1;
};

union mailbox_list_iterate_module_context {
	struct mailbox_list_module_register *reg;
};

struct mailbox_list_iterate_context {
	struct mailbox_list *list;
	pool_t pool;
	enum mailbox_list_iter_flags flags;
	bool failed;
	bool index_iteration;
	bool iter_from_index_dir;

	struct imap_match_glob *glob;
	struct mailbox_list_autocreate_iterate_context *autocreate_ctx;
	struct mailbox_info specialuse_info;
	char *specialuse_info_flags;

	ARRAY(union mailbox_list_iterate_module_context *) module_contexts;
	HASH_TABLE(const char *, void*) found_mailboxes;
};

struct mailbox_list_iter_update_context {
	struct mailbox_list_iterate_context *iter_ctx;
	struct mailbox_tree_context *tree_ctx;

	struct imap_match_glob *glob;
	enum mailbox_info_flags leaf_flags, parent_flags;

	bool update_only:1;
	bool match_parents:1;
};

/* Modules should use do "my_id = mailbox_list_module_id++" and
   use objects' module_contexts[id] for their own purposes. */
extern struct mailbox_list_module_register mailbox_list_module_register;

void mailbox_lists_init(void);
void mailbox_lists_deinit(void);

const char *
mailbox_list_escape_name_params(const char *vname, const char *ns_prefix,
				char ns_sep, char list_sep, char escape_char,
				const char *maildir_name);
const char *
mailbox_list_unescape_name_params(const char *src, const char *ns_prefix,
				  char ns_sep, char list_sep, char escape_char);

int mailbox_list_default_get_storage(struct mailbox_list **list,
				     const char **vname,
				     enum mailbox_list_get_storage_flags flags,
				     struct mail_storage **storage_r);
const char *mailbox_list_default_get_storage_name(struct mailbox_list *list,
						  const char *vname);
const char *mailbox_list_default_get_vname(struct mailbox_list *list,
					   const char *storage_name);
const char *mailbox_list_get_unexpanded_path(struct mailbox_list *list,
					     enum mailbox_list_path_type type);
bool mailbox_list_default_get_root_path(struct mailbox_list *list,
					enum mailbox_list_path_type type,
					const char **path_r);

int mailbox_list_delete_index_control(struct mailbox_list *list,
				      const char *name);

void mailbox_list_iter_update(struct mailbox_list_iter_update_context *ctx,
			      const char *name);
int mailbox_list_iter_subscriptions_refresh(struct mailbox_list *list);
const struct mailbox_info *
mailbox_list_iter_default_next(struct mailbox_list_iterate_context *ctx);

enum mailbox_list_file_type mailbox_list_get_file_type(const struct dirent *d);
int mailbox_list_dirent_is_alias_symlink(struct mailbox_list *list,
					 const char *dir_path,
					 const struct dirent *d);
bool mailbox_list_try_get_absolute_path(struct mailbox_list *list,
					const char **name);
void mailbox_permissions_copy(struct mailbox_permissions *dest,
			      const struct mailbox_permissions *src,
			      pool_t pool);

void mailbox_list_add_change(struct mailbox_list *list,
			     enum mailbox_log_record_type type,
			     const guid_128_t guid_128);
void mailbox_name_get_sha128(const char *name, guid_128_t guid_128_r);

void mailbox_list_clear_error(struct mailbox_list *list);
void mailbox_list_set_error(struct mailbox_list *list,
			    enum mail_error error, const char *string);
void mailbox_list_set_critical(struct mailbox_list *list, const char *fmt, ...)
	ATTR_FORMAT(2, 3);
void mailbox_list_set_internal_error(struct mailbox_list *list);
bool mailbox_list_set_error_from_errno(struct mailbox_list *list);

const struct mailbox_info *
mailbox_list_iter_autocreate_filter(struct mailbox_list_iterate_context *ctx,
				    const struct mailbox_info *_info);

int mailbox_list_lock(struct mailbox_list *list);
void mailbox_list_unlock(struct mailbox_list *list);

#endif
