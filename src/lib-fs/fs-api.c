/* Copyright (c) 2010-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "module-dir.h"
#include "llist.h"
#include "str.h"
#include "hash-method.h"
#include "istream.h"
#include "istream-seekable.h"
#include "ostream.h"
#include "stats-dist.h"
#include "time-util.h"
#include "settings.h"
#include "istream-fs-stats.h"
#include "fs-api-private.h"

static bool fs_settings_check(void *_set, pool_t pool, const char **error_r);

#undef DEF
#define DEF(type, name) \
	SETTING_DEFINE_STRUCT_##type(#name, name, struct fs_settings)
static const struct setting_define fs_setting_defines[] = {
	DEF(STR, fs_name),
	DEF(STR, fs_driver),
	{ .type = SET_FILTER_ARRAY, .key = "fs",
	  .offset = offsetof(struct fs_settings, fs),
	  .filter_array_field_name = "fs_name", },

	SETTING_DEFINE_LIST_END
};
static const struct fs_settings fs_default_settings = {
	.fs_name = "",
	.fs_driver = "",
	.fs = ARRAY_INIT,
};
const struct setting_parser_info fs_setting_parser_info = {
	.name = "fs",

	.defines = fs_setting_defines,
	.defaults = &fs_default_settings,

	.struct_size = sizeof(struct fs_settings),
	.pool_offset1 = 1 + offsetof(struct fs_settings, pool),

	.check_func = fs_settings_check,
};

static struct event_category event_category_fs = {
	.name = "fs"
};

struct fs_api_module_register fs_api_module_register = { 0 };

static struct module *fs_modules = NULL;
static ARRAY(const struct fs *) fs_classes;

static void fs_classes_init(void);

static struct event *fs_create_event(struct fs *fs, struct event *parent)
{
	struct event *event;

	event = event_create(parent);
	event_add_category(event, &event_category_fs);
	event_set_append_log_prefix(event,
		t_strdup_printf("fs-%s: ", fs->name));
	return event;
}

void fs_class_register(const struct fs *fs_class)
{
	if (!array_is_created(&fs_classes))
		fs_classes_init();
	array_push_back(&fs_classes, &fs_class);
}

static void fs_classes_deinit(void)
{
	array_free(&fs_classes);
}

static void fs_classes_init(void)
{
	i_array_init(&fs_classes, 8);
	fs_class_register(&fs_class_dict);
	fs_class_register(&fs_class_posix);
	fs_class_register(&fs_class_randomfail);
	fs_class_register(&fs_class_metawrap);
	fs_class_register(&fs_class_sis);
	fs_class_register(&fs_class_sis_queue);
	fs_class_register(&fs_class_test);
	lib_atexit(fs_classes_deinit);
}

static const struct fs *fs_class_find(const char *driver)
{
	const struct fs *class;

	if (!array_is_created(&fs_classes))
		fs_classes_init();

	array_foreach_elem(&fs_classes, class) {
		if (strcmp(class->name, driver) == 0)
			return class;
	}
	return NULL;
}

static void fs_class_deinit_modules(void)
{
	module_dir_unload(&fs_modules);
}

static const char *fs_driver_module_name(const char *driver)
{
	return t_str_replace(driver, '-', '_');
}

static void fs_class_try_load_plugin(const char *driver)
{
	const char *module_names[] = {
		t_strdup_printf("fs_%s", fs_driver_module_name(driver)),
		NULL
	};
	struct module *module;
	struct module_dir_load_settings mod_set;
	const struct fs *fs_class;

	i_zero(&mod_set);
	mod_set.abi_version = DOVECOT_ABI_VERSION;
	mod_set.ignore_missing = TRUE;

	fs_modules = module_dir_load_missing(fs_modules, MODULE_DIR,
					     module_names, &mod_set);
	module_dir_init(fs_modules);

	module = module_dir_find(fs_modules, module_names[0]);
	fs_class = module == NULL ? NULL :
		module_get_symbol(module, t_strdup_printf(
			"fs_class_%s", fs_driver_module_name(driver)));
	if (fs_class != NULL)
		fs_class_register(fs_class);

	lib_atexit(fs_class_deinit_modules);
}

static int
fs_alloc(const char *driver, struct event *event_parent,
	 const struct fs_parameters *params,
	 struct fs **fs_r, const char **error_r)
{
	const struct fs *fs_class;
	struct fs *fs;
	const char *temp_dir, *temp_file_prefix;

	fs_class = fs_class_find(driver);
	if (fs_class == NULL) {
		T_BEGIN {
			fs_class_try_load_plugin(driver);
		} T_END;
		fs_class = fs_class_find(driver);
	}
	if (fs_class == NULL) {
		*error_r = t_strdup_printf("Unknown fs driver: %s", driver);
		return -1;
	}

	fs = fs_class->v.alloc();
	fs->refcount = 1;
	fs->enable_timing = params->enable_timing;
	fs->username = i_strdup(params->username);
	fs->session_id = i_strdup(params->session_id);
	i_array_init(&fs->module_contexts, 5);
	fs->event = fs_create_event(fs, event_parent);
	event_set_ptr(fs->event, FS_EVENT_FIELD_FS, fs);

	temp_dir = params->temp_dir != NULL ? params->temp_dir : "/tmp";
	temp_file_prefix = params->temp_file_prefix != NULL ?
		params->temp_file_prefix : ".temp.dovecot";
	fs->temp_path_prefix = i_strconcat(temp_dir, "/", temp_file_prefix, NULL);

	*fs_r = fs;
	return 0;
}

static bool fs_settings_check(void *_set, pool_t pool ATTR_UNUSED,
			      const char **error_r ATTR_UNUSED)
{
	struct fs_settings *set = _set;

	if (set->fs_driver[0] == '\0' && set->fs_name[0] != '\0') {
		/* default an empty fs_driver to fs_name, so it's possible to
		   configure simply: fs driver { .. }, but to still allow the
		   same driver to be used multiple times if necessary. */
		set->fs_driver = set->fs_name;
	}
	return TRUE;
}

static int fs_init(struct event *event,
		   const struct fs_parameters *params,
		   const ARRAY_TYPE(const_string) *fs_list,
		   unsigned int fs_list_idx,
		   unsigned int *init_fs_last_list_idx,
		   struct fs **fs_r, const char **error_r)
{
	const struct fs_settings *fs_set;
	struct fs *fs;
	const char *fs_name, *error;
	int ret;

	fs_name = array_idx_elem(fs_list, fs_list_idx);
	if (settings_get_filter(event, "fs", fs_name, &fs_setting_parser_info,
				0, &fs_set, error_r) < 0)
		return -1;

	if (fs_set->fs_driver[0] == '\0') {
		*error_r = "fs_driver is empty";
		settings_free(fs_set);
		return -1;
	}

	event_add_str(event, "fs", fs_name);
	settings_event_add_list_filter_name(event, "fs", fs_name);

	ret = fs_alloc(fs_set->fs_driver, event, params, &fs, error_r);
	settings_free(fs_set);
	if (ret < 0)
		return -1;

	fs->init_fs_list = fs_list;
	fs->init_fs_list_idx = fs_list_idx;
	fs->init_fs_last_list_idx = init_fs_last_list_idx;
	*init_fs_last_list_idx = fs_list_idx;
	T_BEGIN {
		ret = fs->v.init(fs, params, &error);
	} T_END_PASS_STR_IF(ret < 0, &error);
	if (ret < 0) {
		*error_r = t_strdup_printf("%s: %s", fs->name, error);
		fs_unref(&fs);
		return -1;
	}
	/* fs's parent event points to the fs parent's event. This is normally
	   wanted. However, we don't want the parent fs's settings to be read
	   for this fs. We don't expect settings to be read anymore after
	   init(). Drop settings_filter_name so if settings are attempted to be
	   read later on, it will be obvious enough that it's not using any
	   fs settings. */
	event_set_ptr(event, SETTINGS_EVENT_FILTER_NAME, NULL);
	fs->init_fs_list = NULL;
	*fs_r = fs;
	return 0;
}

int fs_init_auto(struct event *event, const struct fs_parameters *params,
		 struct fs **fs_r, const char **error_r)
{
	const struct fs_settings *fs_set;
	struct fs *fs;
	unsigned int last_list_idx;
	int ret;

	if (settings_get(event, &fs_setting_parser_info, 0,
			 &fs_set, error_r) < 0)
		return -1;
	if (array_is_empty(&fs_set->fs)) {
		settings_free(fs_set);
		*error_r = "fs { .. } named list filter is missing";
		return 0;
	}

	event = event_create(event);
	ret = fs_init(event, params, &fs_set->fs, 0,
		      &last_list_idx, &fs, error_r);
	event_unref(&event);

	if (ret == 0 && last_list_idx + 1 < array_count(&fs_set->fs)) {
		const char *fs_name_last =
			array_idx_elem(&fs_set->fs, last_list_idx);
		const char *fs_name_extra =
			array_idx_elem(&fs_set->fs, last_list_idx + 1);
		*error_r = t_strdup_printf(
			"Extra fs %s { .. } named list filter - "
			"the parent fs %s { .. } doesn't support a child fs",
			fs_name_extra, fs_name_last);
		settings_free(fs_set);
		fs_unref(&fs);
		return -1;
	}
	settings_free(fs_set);
	if (ret < 0)
		return -1;
	*fs_r = fs;
	return 1;
}

int fs_init_parent(struct fs *fs, const struct fs_parameters *params,
		   const char **error_r)
{
	if (fs->init_fs_list_idx + 1 >= array_count(fs->init_fs_list)) {
		*error_r = "Next fs { .. } named list filter is missing";
		return -1;
	}

	/* Remove the parent fs's settings_filter_name while initializing a
	   child fs, so the parent settings won't be attempted to be read. */
	char *old_filter = event_get_ptr(event_get_parent(fs->event),
					 SETTINGS_EVENT_FILTER_NAME);
	event_set_ptr(event_get_parent(fs->event),
		      SETTINGS_EVENT_FILTER_NAME, NULL);

	struct event *event = event_create(fs->event);
	/* Drop the parent "fs-name: " prefix */
	event_drop_parent_log_prefixes(event, 1);
	int ret = fs_init(event, params,
			  fs->init_fs_list, fs->init_fs_list_idx + 1,
			  fs->init_fs_last_list_idx,
			  &fs->parent, error_r);
	event_unref(&event);
	/* Restore the old settings_filter_name, since the caller's init()
	   could still need it. */
	event_set_ptr(event_get_parent(fs->event),
		      SETTINGS_EVENT_FILTER_NAME, old_filter);
	return ret;
}

void fs_deinit(struct fs **fs)
{
	fs_unref(fs);
}

void fs_ref(struct fs *fs)
{
	i_assert(fs->refcount > 0);

	fs->refcount++;
}

void fs_unref(struct fs **_fs)
{
	struct fs *fs = *_fs;
	struct array module_contexts_arr;
	unsigned int i;

	if (fs == NULL)
		return;

	module_contexts_arr = fs->module_contexts.arr;

	i_assert(fs->refcount > 0);

	*_fs = NULL;

	if (--fs->refcount > 0)
		return;

	if (fs->files_open_count > 0) {
		i_panic("fs-%s: %u files still open (first = %s)",
			fs->name, fs->files_open_count, fs_file_path(fs->files));
	}
	i_assert(fs->files == NULL);

	if (fs->v.deinit != NULL)
		fs->v.deinit(fs);

	fs_deinit(&fs->parent);
	event_unref(&fs->event);
	i_free(fs->username);
	i_free(fs->session_id);
	i_free(fs->temp_path_prefix);
	for (i = 0; i < FS_OP_COUNT; i++) {
		if (fs->stats.timings[i] != NULL)
			stats_dist_deinit(&fs->stats.timings[i]);
	}
	T_BEGIN {
		fs->v.free(fs);
	} T_END;
	array_free_i(&module_contexts_arr);
}

struct fs *fs_get_parent(struct fs *fs)
{
	return fs->parent;
}

const char *fs_get_driver(struct fs *fs)
{
	return fs->name;
}

struct fs *fs_get_root_fs(struct fs *fs)
{
	while (fs->parent != NULL)
		fs = fs->parent;
	return fs;
}

const char *fs_get_root_driver(struct fs *fs)
{
	return fs_get_root_fs(fs)->name;
}

struct fs_file *fs_file_init(struct fs *fs, const char *path, int mode_flags)
{
	return fs_file_init_with_event(fs, fs->event, path, mode_flags);
}

struct fs_file *fs_file_init_with_event(struct fs *fs, struct event *event,
					const char *path, int mode_flags)
{
	struct fs_file *file;

	i_assert(path != NULL);
	i_assert((mode_flags & FS_OPEN_FLAG_ASYNC_NOQUEUE) == 0 ||
		 (mode_flags & FS_OPEN_FLAG_ASYNC) != 0);

	T_BEGIN {
		file = fs->v.file_alloc();
		file->fs = fs;
		file->flags = mode_flags & ENUM_NEGATE(FS_OPEN_MODE_MASK);
		file->event = fs_create_event(fs, event);
		event_set_ptr(file->event, FS_EVENT_FIELD_FS, fs);
		event_set_ptr(file->event, FS_EVENT_FIELD_FILE, file);
		fs->v.file_init(file, path, mode_flags & FS_OPEN_MODE_MASK,
				mode_flags & ENUM_NEGATE(FS_OPEN_MODE_MASK));
	} T_END;

	fs->files_open_count++;
	DLLIST_PREPEND(&fs->files, file);

	fs_set_metadata(file, FS_METADATA_ORIG_PATH, path);
	return file;
}

void fs_file_deinit(struct fs_file **_file)
{
	struct fs_file *file = *_file;

	if (file == NULL)
		return;

	i_assert(file->fs->files_open_count > 0);

	*_file = NULL;

	fs_file_close(file);

	DLLIST_REMOVE(&file->fs->files, file);
	file->fs->files_open_count--;
	T_BEGIN {
		file->fs->v.file_deinit(file);
	} T_END;
}

void fs_file_free(struct fs_file *file)
{
	if (file->last_error_changed) {
		/* fs_set_error() used without ever accessing it via
		   fs_file_last_error(). Log it to make sure it's not lost.
		   Note that the errors are always set only to the file at
		   the root of the parent hierarchy. */
		e_error(file->event, "%s (in file %s deinit)",
			file->last_error, fs_file_path(file));
	}

	fs_file_deinit(&file->parent);
	event_unref(&file->event);
	pool_unref(&file->metadata_pool);
	i_free(file->last_error);
}

void fs_file_set_flags(struct fs_file *file,
		       enum fs_open_flags add_flags,
		       enum fs_open_flags remove_flags)
{
	file->flags |= add_flags;
	file->flags &= ENUM_NEGATE(remove_flags);

	if (file->parent != NULL)
		fs_file_set_flags(file->parent, add_flags, remove_flags);
}

void fs_file_close(struct fs_file *file)
{
	if (file == NULL)
		return;

	i_assert(!file->writing_stream);
	i_assert(file->output == NULL);

	if (file->pending_read_input != NULL)
		i_stream_unref(&file->pending_read_input);
	if (file->seekable_input != NULL)
		i_stream_unref(&file->seekable_input);

	if (file->copy_input != NULL) {
		i_stream_unref(&file->copy_input);
		fs_write_stream_abort_error(file, &file->copy_output, "fs_file_close(%s)",
					    o_stream_get_name(file->copy_output));
	}
	i_free_and_null(file->write_digest);
	if (file->fs->v.file_close != NULL) T_BEGIN {
		file->fs->v.file_close(file);
	} T_END;

	/* check this only after closing, because some of the fs backends keep
	   the istream internally open and don't call the destroy-callback
	   until after file_close() */
	i_assert(!file->istream_open);
}

enum fs_properties fs_get_properties(struct fs *fs)
{
	return fs->v.get_properties(fs);
}

void fs_metadata_init(struct fs_file *file)
{
	if (file->metadata_pool == NULL) {
		i_assert(!array_is_created(&file->metadata));
		file->metadata_pool = pool_alloconly_create("fs metadata", 1024);
		p_array_init(&file->metadata, file->metadata_pool, 8);
	}
}

void fs_metadata_init_or_clear(struct fs_file *file)
{
	if (file->metadata_pool == NULL)
		fs_metadata_init(file);
	else T_BEGIN {
		const struct fs_metadata *md;
		ARRAY_TYPE(fs_metadata) internal_metadata;

		t_array_init(&internal_metadata, 4);
		array_foreach(&file->metadata, md) {
			if (strncmp(md->key, FS_METADATA_INTERNAL_PREFIX,
				    strlen(FS_METADATA_INTERNAL_PREFIX)) == 0)
				array_push_back(&internal_metadata, md);
		}
		array_clear(&file->metadata);
		array_append_array(&file->metadata, &internal_metadata);
	} T_END;
}

static struct fs_metadata *
fs_metadata_find_md(const ARRAY_TYPE(fs_metadata) *metadata,
		    const char *key)
{
	struct fs_metadata *md;

	array_foreach_modifiable(metadata, md) {
		if (strcmp(md->key, key) == 0)
			return md;
	}
	return NULL;
}

void fs_default_set_metadata(struct fs_file *file,
			     const char *key, const char *value)
{
	struct fs_metadata *metadata;

	fs_metadata_init(file);
	metadata = fs_metadata_find_md(&file->metadata, key);
	if (metadata == NULL) {
		metadata = array_append_space(&file->metadata);
		metadata->key = p_strdup(file->metadata_pool, key);
	}
	metadata->value = p_strdup(file->metadata_pool, value);
}

const char *fs_metadata_find(const ARRAY_TYPE(fs_metadata) *metadata,
			     const char *key)
{
	const struct fs_metadata *md;

	if (!array_is_created(metadata))
		return NULL;

	md = fs_metadata_find_md(metadata, key);
	return md == NULL ? NULL : md->value;
}

void fs_set_metadata(struct fs_file *file, const char *key, const char *value)
{
	i_assert(key != NULL);
	i_assert(value != NULL);
	i_assert(strchr(key, '_') == NULL);

	if (file->fs->v.set_metadata != NULL) T_BEGIN {
		file->fs->v.set_metadata(file, key, value);
		if (strncmp(key, FS_METADATA_INTERNAL_PREFIX,
			    strlen(FS_METADATA_INTERNAL_PREFIX)) == 0) {
			/* internal metadata change, which isn't stored. */
		} else {
			file->metadata_changed = TRUE;
		}
	} T_END;
}

static void fs_file_timing_start(struct fs_file *file, enum fs_op op)
{
	if (!file->fs->enable_timing)
		return;
	if (file->timing_start[op].tv_sec == 0)
		i_gettimeofday(&file->timing_start[op]);
}

static void
fs_timing_end(struct stats_dist **timing, const struct timeval *start_tv)
{
	struct timeval now;
	long long diff;

	i_gettimeofday(&now);

	diff = timeval_diff_usecs(&now, start_tv);
	if (diff > 0) {
		if (*timing == NULL)
			*timing = stats_dist_init();
		stats_dist_add(*timing, diff);
	}
}

void fs_file_timing_end(struct fs_file *file, enum fs_op op)
{
	if (!file->fs->enable_timing || file->timing_start[op].tv_sec == 0)
		return;

	fs_timing_end(&file->fs->stats.timings[op], &file->timing_start[op]);
	/* don't count this again */
	file->timing_start[op].tv_sec = 0;
}

int fs_get_metadata_full(struct fs_file *file,
			 enum fs_get_metadata_flags flags,
			 const ARRAY_TYPE(fs_metadata) **metadata_r)
{
	int ret;

	if (file->fs->v.get_metadata == NULL) {
		if (array_is_created(&file->metadata)) {
			/* Return internal metadata. */
			*metadata_r = &file->metadata;
			return 0;
		}
		fs_set_error(file->event, ENOTSUP, "Metadata not supported by backend");
		return -1;
	}
	if (!file->read_or_prefetch_counted &&
	    !file->lookup_metadata_counted) {
		if ((flags & FS_GET_METADATA_FLAG_LOADED_ONLY) == 0) {
			file->lookup_metadata_counted = TRUE;
			file->fs->stats.lookup_metadata_count++;
		}
		fs_file_timing_start(file, FS_OP_METADATA);
	}
	T_BEGIN {
		ret = file->fs->v.get_metadata(file, flags, metadata_r);
	} T_END;
	if (!(ret < 0 && errno == EAGAIN))
		fs_file_timing_end(file, FS_OP_METADATA);
	return ret;
}

int fs_get_metadata(struct fs_file *file,
		    const ARRAY_TYPE(fs_metadata) **metadata_r)
{
	return fs_get_metadata_full(file, 0, metadata_r);
}

int fs_lookup_metadata(struct fs_file *file, const char *key,
		       const char **value_r)
{
	const ARRAY_TYPE(fs_metadata) *metadata;

	if (fs_get_metadata(file, &metadata) < 0)
		return -1;
	*value_r = fs_metadata_find(metadata, key);
	return *value_r != NULL ? 1 : 0;
}

const char *fs_lookup_loaded_metadata(struct fs_file *file, const char *key)
{
	const ARRAY_TYPE(fs_metadata) *metadata;

	if (fs_get_metadata_full(file, FS_GET_METADATA_FLAG_LOADED_ONLY, &metadata) < 0)
		i_panic("FS_GET_METADATA_FLAG_LOADED_ONLY lookup can't fail");
	return fs_metadata_find(metadata, key);
}

const char *fs_file_path(struct fs_file *file)
{
	return file->fs->v.get_path == NULL ? file->path :
		file->fs->v.get_path(file);
}

struct fs *fs_file_fs(struct fs_file *file)
{
	return file->fs;
}

struct event *fs_file_event(struct fs_file *file)
{
	return file->event;
}

static struct fs_file *fs_file_get_error_file(struct fs_file *file)
{
	/* the error is always kept in the parent-most file */
	while (file->parent != NULL)
		file = file->parent;
	return file;
}

static void ATTR_FORMAT(2, 0)
fs_set_verror(struct event *event, const char *fmt, va_list args)
{
	struct event *fs_event = event;
	struct fs_file *file;
	struct fs_iter *iter;

	/* NOTE: the event might be a passthrough event. We must log it exactly
	   once so it gets freed. */

	/* figure out if the error is for a file or iter */
	while ((file = event_get_ptr(fs_event, FS_EVENT_FIELD_FILE)) == NULL &&
	       (iter = event_get_ptr(fs_event, FS_EVENT_FIELD_ITER)) == NULL) {
		fs_event = event_get_parent(fs_event);
		i_assert(fs_event != NULL);
	}

	char *new_error = i_strdup_vprintf(fmt, args);
	/* Don't flood the debug log with "Asynchronous operation in progress"
	   messages. They tell nothing useful. */
	if (errno != EAGAIN)
		e_debug(event, "%s", new_error);
	else
		event_send_abort(event);

	/* free old error after strdup in case args point to the old error */
	if (file != NULL) {
		file = fs_file_get_error_file(file);
		char *old_error = file->last_error;

		if (old_error == NULL) {
			i_assert(!file->last_error_changed);
		} else if (strcmp(old_error, new_error) == 0) {
			/* identical error - ignore */
		} else if (file->last_error_changed) {
			/* multiple fs_set_error() calls used without
			   fs_file_last_error() in the middle. */
			e_error(file->event, "%s (overwriting error for file %s)",
				old_error, fs_file_path(file));
		}
		if (errno == EAGAIN || errno == ENOENT || errno == EEXIST ||
		    errno == ENOTEMPTY) {
			/* These are (or can be) expected errors - don't log
			   them if they have a missing fs_file_last_error()
			   call */
			file->last_error_changed = FALSE;
		} else {
			file->last_error_changed = TRUE;
		}

		i_free(file->last_error);
		file->last_error = new_error;
	} else {
		i_assert(iter != NULL);
		if (iter->last_error != NULL &&
		    strcmp(iter->last_error, new_error) == 0) {
			/* identical error - ignore */
		} else if (iter->last_error != NULL) {
			/* multiple fs_set_error() calls before the iter
			   finishes */
			e_error(iter->fs->event, "%s (overwriting error for file %s)",
				iter->last_error, iter->path);
		}
		i_free(iter->last_error);
		iter->last_error = new_error;
	}
}

const char *fs_file_last_error(struct fs_file *file)
{
	struct fs_file *error_file = fs_file_get_error_file(file);

	error_file->last_error_changed = FALSE;
	if (error_file->last_error == NULL)
		return "BUG: Unknown file error";
	return error_file->last_error;
}

bool fs_prefetch(struct fs_file *file, uoff_t length)
{
	bool ret;

	if (!file->read_or_prefetch_counted) {
		file->read_or_prefetch_counted = TRUE;
		file->fs->stats.prefetch_count++;
		fs_file_timing_start(file, FS_OP_PREFETCH);
	}
	T_BEGIN {
		ret = file->fs->v.prefetch(file, length);
	} T_END;
	fs_file_timing_end(file, FS_OP_PREFETCH);
	return ret;
}

ssize_t fs_read_via_stream(struct fs_file *file, void *buf, size_t size)
{
	const unsigned char *data;
	size_t data_size;
	ssize_t ret;

	i_assert(size > 0);

	if (file->pending_read_input == NULL)
		file->pending_read_input = fs_read_stream(file, size+1);
	ret = i_stream_read_bytes(file->pending_read_input, &data,
				  &data_size, size);
	if (ret == 0) {
		fs_file_set_error_async(file);
		return -1;
	}
	if (ret < 0 && file->pending_read_input->stream_errno != 0) {
		fs_set_error(file->event,
			     file->pending_read_input->stream_errno,
			     "read(%s) failed: %s",
			     i_stream_get_name(file->pending_read_input),
			     i_stream_get_error(file->pending_read_input));
	} else {
		ret = I_MIN(size, data_size);
		if (ret > 0)
			memcpy(buf, data, ret);
	}
	i_stream_unref(&file->pending_read_input);
	return ret;
}

ssize_t fs_read(struct fs_file *file, void *buf, size_t size)
{
	int ret;

	if (!file->read_or_prefetch_counted) {
		file->read_or_prefetch_counted = TRUE;
		file->fs->stats.read_count++;
		fs_file_timing_start(file, FS_OP_READ);
	}

	if (file->fs->v.read != NULL) {
		T_BEGIN {
			ret = file->fs->v.read(file, buf, size);
		} T_END;
		if (!(ret < 0 && errno == EAGAIN))
			fs_file_timing_end(file, FS_OP_READ);
		return ret;
	}

	/* backend didn't bother to implement read(), but we can do it with
	   streams. */
	return fs_read_via_stream(file, buf, size);
}

static void fs_file_istream_destroyed(struct fs_file *file)
{
	i_assert(file->istream_open);

	file->istream_open = FALSE;
}

struct istream *fs_read_stream(struct fs_file *file, size_t max_buffer_size)
{
	struct istream *input, *inputs[2];
	const unsigned char *data;
	size_t size;
	ssize_t ret;
	bool want_seekable = FALSE;

	if (!file->read_or_prefetch_counted) {
		file->read_or_prefetch_counted = TRUE;
		file->fs->stats.read_count++;
		fs_file_timing_start(file, FS_OP_READ);
	}

	if (file->seekable_input != NULL) {
		/* allow multiple open streams, each in a different position */
		input = i_stream_create_limit(file->seekable_input, UOFF_T_MAX);
		i_stream_seek(input, 0);
		return input;
	}
	i_assert(!file->istream_open);
	T_BEGIN {
		input = file->fs->v.read_stream(file, max_buffer_size);
	} T_END;
	if (input->stream_errno != 0) {
		/* read failed already */
		fs_file_timing_end(file, FS_OP_READ);
		return input;
	}
	if (file->fs->enable_timing) {
		struct istream *input2 = i_stream_create_fs_stats(input, file);

		i_stream_unref(&input);
		input = input2;
	}

	if ((file->flags & FS_OPEN_FLAG_SEEKABLE) != 0)
		want_seekable = TRUE;
	else if ((file->flags & FS_OPEN_FLAG_ASYNC) == 0 && !input->blocking)
		want_seekable = TRUE;

	if (want_seekable && !input->seekable) {
		/* need to make the stream seekable */
		inputs[0] = input;
		inputs[1] = NULL;
		input = i_stream_create_seekable_path(inputs, max_buffer_size,
						file->fs->temp_path_prefix);
		i_stream_set_name(input, i_stream_get_name(inputs[0]));
		i_stream_unref(&inputs[0]);
	}
	file->seekable_input = input;
	i_stream_ref(file->seekable_input);

	if ((file->flags & FS_OPEN_FLAG_ASYNC) == 0 && !input->blocking) {
		/* read the whole input stream before returning */
		while ((ret = i_stream_read_more(input, &data, &size)) >= 0) {
			i_stream_skip(input, size);
			if (ret == 0)
				fs_wait_async(file->fs);
		}
		i_stream_seek(input, 0);
	}
	file->istream_open = TRUE;
	i_stream_add_destroy_callback(input, fs_file_istream_destroyed, file);
	return input;
}

int fs_write_via_stream(struct fs_file *file, const void *data, size_t size)
{
	struct ostream *output;
	ssize_t ret;
	int err;

	if (!file->write_pending) {
		output = fs_write_stream(file);
		if ((ret = o_stream_send(output, data, size)) < 0) {
			err = errno;
			fs_write_stream_abort_error(file, &output, "fs_write(%s) failed: %s",
						    o_stream_get_name(output),
						    o_stream_get_error(output));
			errno = err;
			return -1;
		}
		i_assert((size_t)ret == size);
		ret = fs_write_stream_finish(file, &output);
	} else {
		ret = fs_write_stream_finish_async(file);
	}
	if (ret == 0) {
		fs_file_set_error_async(file);
		file->write_pending = TRUE;
		return -1;
	}
	file->write_pending = FALSE;
	return ret < 0 ? -1 : 0;
}

int fs_write(struct fs_file *file, const void *data, size_t size)
{
	int ret;

	if (file->fs->v.write != NULL) {
		fs_file_timing_start(file, FS_OP_WRITE);
		T_BEGIN {
			ret = file->fs->v.write(file, data, size);
		} T_END;
		if (!(ret < 0 && errno == EAGAIN)) {
			file->fs->stats.write_count++;
			file->fs->stats.write_bytes += size;
			fs_file_timing_end(file, FS_OP_WRITE);
		}
		return ret;
	}

	/* backend didn't bother to implement write(), but we can do it with
	   streams. */
	return fs_write_via_stream(file, data, size);
}

struct ostream *fs_write_stream(struct fs_file *file)
{
	i_assert(!file->writing_stream);
	i_assert(file->output == NULL);

	file->writing_stream = TRUE;
	file->fs->stats.write_count++;
	T_BEGIN {
		file->fs->v.write_stream(file);
	} T_END;
	i_assert(file->output != NULL);
	o_stream_cork(file->output);
	return file->output;
}

static int fs_write_stream_finish_int(struct fs_file *file, bool success)
{
	int ret;

	i_assert(file->writing_stream);

	fs_file_timing_start(file, FS_OP_WRITE);
	T_BEGIN {
		ret = file->fs->v.write_stream_finish(file, success);
	} T_END;
	if (ret != 0) {
		fs_file_timing_end(file, FS_OP_WRITE);
		file->metadata_changed = FALSE;
	} else {
		/* write didn't finish yet. this shouldn't happen if we
		   indicated a failure. */
		i_assert(success);
	}
	if (ret != 0) {
		i_assert(file->output == NULL);
		file->writing_stream = FALSE;
	}
	return ret;
}

int fs_write_stream_finish(struct fs_file *file, struct ostream **output)
{
	bool success = TRUE;
	int ret;

	i_assert(*output == file->output || *output == NULL);
	i_assert(output != &file->output);

	*output = NULL;
	if (file->output != NULL) {
		o_stream_uncork(file->output);
		if ((ret = o_stream_finish(file->output)) <= 0) {
			i_assert(ret < 0);
			fs_set_error(file->event, file->output->stream_errno,
				     "write(%s) failed: %s",
				     o_stream_get_name(file->output),
				     o_stream_get_error(file->output));
			success = FALSE;
		}
		file->fs->stats.write_bytes += file->output->offset;
	}
	return fs_write_stream_finish_int(file, success);
}

int fs_write_stream_finish_async(struct fs_file *file)
{
	return fs_write_stream_finish_int(file, TRUE);
}

static void fs_write_stream_abort(struct fs_file *file, struct ostream **output)
{
	int ret;

	i_assert(*output == file->output);
	i_assert(file->output != NULL);
	i_assert(output != &file->output);

	*output = NULL;
	o_stream_abort(file->output);
	/* make sure we don't have an old error lying around */
	ret = fs_write_stream_finish_int(file, FALSE);
	i_assert(ret != 0);
}

void fs_write_stream_abort_error(struct fs_file *file, struct ostream **output, const char *error_fmt, ...)
{
	va_list args;
	va_start(args, error_fmt);
	fs_set_verror(file->event, error_fmt, args);
	/* the error shouldn't be automatically logged if
	   fs_file_last_error() is no longer used */
	fs_file_get_error_file(file)->last_error_changed = FALSE;
	fs_write_stream_abort(file, output);
	va_end(args);
}

void fs_write_stream_abort_parent(struct fs_file *file, struct ostream **output)
{
	i_assert(file->parent != NULL);
	i_assert(fs_file_last_error(file->parent) != NULL);
	fs_write_stream_abort(file->parent, output);
}

void fs_write_set_hash(struct fs_file *file, const struct hash_method *method,
		       const void *digest)
{
	file->write_digest_method = method;

	i_free(file->write_digest);
	file->write_digest = i_malloc(method->digest_size);
	memcpy(file->write_digest, digest, method->digest_size);
}

#undef fs_file_set_async_callback
void fs_file_set_async_callback(struct fs_file *file,
				fs_file_async_callback_t *callback,
				void *context)
{
	if (file->fs->v.set_async_callback != NULL)
		file->fs->v.set_async_callback(file, callback, context);
	else
		callback(context);
}

void fs_wait_async(struct fs *fs)
{
	/* recursion not allowed */
	i_assert(fs->prev_ioloop == NULL);

	if (fs->v.wait_async != NULL) T_BEGIN {
		fs->prev_ioloop = current_ioloop;
		fs->v.wait_async(fs);
		i_assert(current_ioloop == fs->prev_ioloop);
		fs->prev_ioloop = NULL;
	} T_END;
}

bool fs_switch_ioloop(struct fs *fs)
{
	bool ret = FALSE;

	if (fs->v.switch_ioloop != NULL) {
		T_BEGIN {
			ret = fs->v.switch_ioloop(fs);
		} T_END;
	} else if (fs->parent != NULL) {
		ret = fs_switch_ioloop(fs->parent);
	}
	return ret;
}

int fs_lock(struct fs_file *file, unsigned int secs, struct fs_lock **lock_r)
{
	int ret;

	T_BEGIN {
		ret = file->fs->v.lock(file, secs, lock_r);
	} T_END;
	return ret;
}

void fs_unlock(struct fs_lock **_lock)
{
	struct fs_lock *lock = *_lock;

	if (lock == NULL)
		return;

	*_lock = NULL;
	T_BEGIN {
		lock->file->fs->v.unlock(lock);
	} T_END;
}

int fs_exists(struct fs_file *file)
{
	struct stat st;
	int ret;

	if (file->fs->v.exists == NULL) {
		/* fallback to stat() */
		if (fs_stat(file, &st) == 0)
			return 1;
		else
			return errno == ENOENT ? 0 : -1;
	}
	fs_file_timing_start(file, FS_OP_EXISTS);
	T_BEGIN {
		ret = file->fs->v.exists(file);
	} T_END;
	if (!(ret < 0 && errno == EAGAIN)) {
		file->fs->stats.exists_count++;
		fs_file_timing_end(file, FS_OP_EXISTS);
	}
	return ret;
}

int fs_stat(struct fs_file *file, struct stat *st_r)
{
	int ret;

	if (file->fs->v.stat == NULL) {
		fs_set_error(file->event, ENOTSUP, "fs_stat() not supported");
		return -1;
	}

	if (!file->read_or_prefetch_counted &&
	    !file->lookup_metadata_counted && !file->stat_counted) {
		file->stat_counted = TRUE;
		file->fs->stats.stat_count++;
		fs_file_timing_start(file, FS_OP_STAT);
	}
	T_BEGIN {
		ret = file->fs->v.stat(file, st_r);
	} T_END;
	if (!(ret < 0 && errno == EAGAIN))
		fs_file_timing_end(file, FS_OP_STAT);
	return ret;
}

int fs_get_nlinks(struct fs_file *file, nlink_t *nlinks_r)
{
	int ret;

	if (file->fs->v.get_nlinks == NULL) {
		struct stat st;

		if (fs_stat(file, &st) < 0)
			return -1;
		*nlinks_r = st.st_nlink;
		return 0;
	}

	if (!file->read_or_prefetch_counted &&
	    !file->lookup_metadata_counted && !file->stat_counted) {
		file->stat_counted = TRUE;
		file->fs->stats.stat_count++;
		fs_file_timing_start(file, FS_OP_STAT);
	}
	T_BEGIN {
		ret = file->fs->v.get_nlinks(file, nlinks_r);
	} T_END;
	if (!(ret < 0 && errno == EAGAIN))
		fs_file_timing_end(file, FS_OP_STAT);
	return ret;
}

int fs_default_copy(struct fs_file *src, struct fs_file *dest)
{
	int tmp_errno;
	/* we're going to be counting this as read+write, so don't update
	   copy_count */
	dest->copy_counted = TRUE;

	if (dest->copy_src != NULL) {
		i_assert(src == NULL || src == dest->copy_src);
		if (dest->copy_output == NULL) {
			i_assert(dest->copy_input == NULL);
			if (fs_write_stream_finish_async(dest) <= 0)
				return -1;
			dest->copy_src = NULL;
			return 0;
		}
	} else {
		dest->copy_src = src;
		dest->copy_input = fs_read_stream(src, IO_BLOCK_SIZE);
		dest->copy_output = fs_write_stream(dest);
	}
	switch (o_stream_send_istream(dest->copy_output, dest->copy_input)) {
	case OSTREAM_SEND_ISTREAM_RESULT_FINISHED:
		break;
	case OSTREAM_SEND_ISTREAM_RESULT_WAIT_INPUT:
	case OSTREAM_SEND_ISTREAM_RESULT_WAIT_OUTPUT:
		fs_file_set_error_async(dest);
		return -1;
	case OSTREAM_SEND_ISTREAM_RESULT_ERROR_INPUT:
		fs_write_stream_abort_error(dest, &dest->copy_output,
					    "read(%s) failed: %s",
					    i_stream_get_name(dest->copy_input),
					    i_stream_get_error(dest->copy_input));
		errno = dest->copy_input->stream_errno;
		i_stream_unref(&dest->copy_input);
		return -1;
	case OSTREAM_SEND_ISTREAM_RESULT_ERROR_OUTPUT:
		/* errno might not survive abort error */
		tmp_errno = dest->copy_output->stream_errno;
		fs_write_stream_abort_error(dest, &dest->copy_output,
					    "write(%s) failed: %s",
					    o_stream_get_name(dest->copy_output),
					    o_stream_get_error(dest->copy_output));
		errno = tmp_errno;
		i_stream_unref(&dest->copy_input);
		return -1;
	}
	i_stream_unref(&dest->copy_input);
	if (fs_write_stream_finish(dest, &dest->copy_output) <= 0)
		return -1;
	dest->copy_src = NULL;
	return 0;
}

int fs_copy(struct fs_file *src, struct fs_file *dest)
{
	int ret;

	i_assert(src->fs == dest->fs);

	if (src->fs->v.copy == NULL) {
		fs_set_error(src->event, ENOTSUP, "fs_copy() not supported");
		return -1;
	}

	fs_file_timing_start(dest, FS_OP_COPY);
	T_BEGIN {
		ret = src->fs->v.copy(src, dest);
	} T_END;
	if (!(ret < 0 && errno == EAGAIN)) {
		fs_file_timing_end(dest, FS_OP_COPY);
		if (dest->copy_counted)
			dest->copy_counted = FALSE;
		else
			dest->fs->stats.copy_count++;
		dest->metadata_changed = FALSE;
	}
	return ret;
}

int fs_copy_finish_async(struct fs_file *dest)
{
	int ret;

	T_BEGIN {
		ret = dest->fs->v.copy(NULL, dest);
	} T_END;
	if (!(ret < 0 && errno == EAGAIN)) {
		fs_file_timing_end(dest, FS_OP_COPY);
		if (dest->copy_counted)
			dest->copy_counted = FALSE;
		else
			dest->fs->stats.copy_count++;
		dest->metadata_changed = FALSE;
	}
	return ret;
}

int fs_rename(struct fs_file *src, struct fs_file *dest)
{
	int ret;

	i_assert(src->fs == dest->fs);

	fs_file_timing_start(dest, FS_OP_RENAME);
	T_BEGIN {
		ret = src->fs->v.rename(src, dest);
	} T_END;
	if (!(ret < 0 && errno == EAGAIN)) {
		dest->fs->stats.rename_count++;
		fs_file_timing_end(dest, FS_OP_RENAME);
	}
	return ret;
}

int fs_delete(struct fs_file *file)
{
	int ret;

	fs_file_timing_start(file, FS_OP_DELETE);
	T_BEGIN {
		ret = file->fs->v.delete_file(file);
	} T_END;
	if (!(ret < 0 && errno == EAGAIN)) {
		file->fs->stats.delete_count++;
		fs_file_timing_end(file, FS_OP_DELETE);
	}
	return ret;
}

struct fs_iter *
fs_iter_init(struct fs *fs, const char *path, enum fs_iter_flags flags)
{
	return fs_iter_init_with_event(fs, fs->event, path, flags);
}

struct fs_iter *
fs_iter_init_with_event(struct fs *fs, struct event *event,
			const char *path, enum fs_iter_flags flags)
{
	struct fs_iter *iter;
	struct timeval now = ioloop_timeval;

	i_assert((flags & FS_ITER_FLAG_OBJECTIDS) == 0 ||
		 (fs_get_properties(fs) & FS_PROPERTY_OBJECTIDS) != 0);

	fs->stats.iter_count++;
	if (fs->enable_timing)
		i_gettimeofday(&now);
	if (fs->v.iter_init == NULL)
		iter = i_new(struct fs_iter, 1);
	else
		iter = fs->v.iter_alloc();
	iter->fs = fs;
	iter->event = fs_create_event(fs, event);
	event_set_ptr(iter->event, FS_EVENT_FIELD_FS, fs);
	event_set_ptr(iter->event, FS_EVENT_FIELD_ITER, iter);
	if (fs->v.iter_init != NULL) T_BEGIN {
		iter->flags = flags;
		iter->path = i_strdup(path);
		fs->v.iter_init(iter, path, flags);
	} T_END;
	iter->start_time = now;
	DLLIST_PREPEND(&fs->iters, iter);
	return iter;
}

int fs_iter_deinit(struct fs_iter **_iter, const char **error_r)
{
	struct fs_iter *iter = *_iter;
	struct fs *fs;
	struct event *event;
	int ret;

	if (iter == NULL)
		return 0;

	fs = iter->fs;
	event = iter->event;

	*_iter = NULL;
	DLLIST_REMOVE(&fs->iters, iter);

	if (fs->v.iter_deinit == NULL) {
		fs_set_error(event, ENOTSUP, "FS iteration not supported");
		ret = -1;
	} else T_BEGIN {
		ret = iter->fs->v.iter_deinit(iter);
	} T_END;
	if (ret < 0)
		*error_r = t_strdup(iter->last_error);
	i_free(iter->last_error);
	i_free(iter->path);
	i_free(iter);
	event_unref(&event);
	return ret;
}

const char *fs_iter_next(struct fs_iter *iter)
{
	const char *ret;

	if (iter->fs->v.iter_next == NULL)
		return NULL;
	T_BEGIN {
		ret = iter->fs->v.iter_next(iter);
	} T_END;
	if (iter->start_time.tv_sec != 0 &&
	    (ret != NULL || !fs_iter_have_more(iter))) {
		/* first result returned - count this as the finish time, since
		   we don't want to count the time caller spends on this
		   iteration. */
		fs_timing_end(&iter->fs->stats.timings[FS_OP_ITER], &iter->start_time);
		/* don't count this again */
		iter->start_time.tv_sec = 0;
	}
	return ret;
}

#undef fs_iter_set_async_callback
void fs_iter_set_async_callback(struct fs_iter *iter,
				fs_file_async_callback_t *callback,
				void *context)
{
	iter->async_callback = callback;
	iter->async_context = context;
}

bool fs_iter_have_more(struct fs_iter *iter)
{
	return iter->async_have_more;
}

const struct fs_stats *fs_get_stats(struct fs *fs)
{
	return &fs->stats;
}

void fs_set_error(struct event *event, int err, const char *fmt, ...)
{
	va_list args;

	i_assert(err != 0);

	errno = err;
	va_start(args, fmt);
	fs_set_verror(event, fmt, args);
	va_end(args);
}

void fs_set_error_errno(struct event *event, const char *fmt, ...)
{
	va_list args;

	i_assert(errno != 0);

	va_start(args, fmt);
	fs_set_verror(event, fmt, args);
	va_end(args);
}

void fs_file_set_error_async(struct fs_file *file)
{
	fs_set_error(file->event, EAGAIN, "Asynchronous operation in progress");
}

static uint64_t
fs_stats_count_ops(const struct fs_stats *stats, const enum fs_op ops[],
		   unsigned int ops_count)
{
	uint64_t ret = 0;

	for (unsigned int i = 0; i < ops_count; i++) {
		if (stats->timings[ops[i]] != NULL)
			ret += stats_dist_get_sum(stats->timings[ops[i]]);
	}
	return ret;
}

uint64_t fs_stats_get_read_usecs(const struct fs_stats *stats)
{
	const enum fs_op read_ops[] = {
		FS_OP_METADATA, FS_OP_PREFETCH, FS_OP_READ, FS_OP_EXISTS,
		FS_OP_STAT, FS_OP_ITER
	};
	return fs_stats_count_ops(stats, read_ops, N_ELEMENTS(read_ops));
}

uint64_t fs_stats_get_write_usecs(const struct fs_stats *stats)
{
	const enum fs_op write_ops[] = {
		FS_OP_WRITE, FS_OP_COPY, FS_OP_DELETE
	};
	return fs_stats_count_ops(stats, write_ops, N_ELEMENTS(write_ops));
}

struct fs_file *
fs_file_init_parent(struct fs_file *parent, const char *path,
		    enum fs_open_mode mode, enum fs_open_flags flags)
{
	return fs_file_init_with_event(parent->fs->parent, parent->event,
				       path, (int)mode | (int)flags);
}

struct fs_iter *
fs_iter_init_parent(struct fs_iter *parent,
		    const char *path, enum fs_iter_flags flags)
{
	return fs_iter_init_with_event(parent->fs->parent, parent->event,
				       path, flags);
}

struct event *fs_get_event(struct fs *fs)
{
	return fs->event;
}
