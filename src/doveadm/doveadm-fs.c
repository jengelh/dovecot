/* Copyright (c) 2013-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "md5.h"
#include "sha2.h"
#include "hash-method.h"
#include "hex-binary.h"
#include "istream.h"
#include "ostream.h"
#include "iostream-ssl.h"
#include "settings.h"
#include "fs-api.h"
#include "doveadm.h"
#include "doveadm-fs.h"
#include "doveadm-print.h"

#include <stdio.h>
#include <unistd.h>

static void fs_cmd_help(struct doveadm_cmd_context *cctx) ATTR_NORETURN;
static void cmd_fs_delete(struct doveadm_cmd_context *cctx);

static struct fs *doveadm_fs_init(struct doveadm_cmd_context *cctx)
{
	struct fs_parameters fs_param;
	struct fs *fs;
	const char *filter_name, *error;

	if (!doveadm_cmd_param_str(cctx, "filter-name", &filter_name))
		help_ver2(cctx->cmd);

	i_zero(&fs_param);
	fs_param.temp_dir = doveadm_settings->mail_temp_dir;
	fs_param.base_dir = doveadm_settings->base_dir;

	settings_event_add_filter_name(cctx->set_event, filter_name);
	if (fs_init_auto(cctx->set_event, &fs_param, &fs, &error) <= 0)
		i_fatal("fs_init() failed: %s", error);
	event_set_ptr(cctx->set_event, SETTINGS_EVENT_FILTER_NAME, NULL);
	return fs;
}

static void cmd_fs_get(struct doveadm_cmd_context *cctx)
{
	const char *path;

	doveadm_print_init(DOVEADM_PRINT_TYPE_PAGER);
	doveadm_print_header("content", "content", DOVEADM_PRINT_HEADER_FLAG_HIDE_TITLE);

	if (!doveadm_cmd_param_str(cctx, "path", &path))
		fs_cmd_help(cctx);
	doveadm_fs_get(cctx, path);
}

void doveadm_fs_get(struct doveadm_cmd_context *cctx, const char *path)
{
	struct fs *fs;
	struct fs_file *file;
	struct istream *input;
	const unsigned char *data;
	size_t size;
	ssize_t ret;

	fs = doveadm_fs_init(cctx);

	file = fs_file_init(fs, path, FS_OPEN_MODE_READONLY);
	input = fs_read_stream(file, IO_BLOCK_SIZE);
	while ((ret = i_stream_read_more(input, &data, &size)) > 0) {
		doveadm_print_stream(data, size);
		i_stream_skip(input, size);
	}
	doveadm_print_stream("", 0);
	i_assert(ret == -1);
	if (input->stream_errno == ENOENT) {
		e_error(cctx->event, "%s doesn't exist: %s", fs_file_path(file),
			i_stream_get_error(input));
		doveadm_exit_code = DOVEADM_EX_NOTFOUND;
	} else if (input->stream_errno != 0) {
		e_error(cctx->event, "read(%s) failed: %s", fs_file_path(file),
			i_stream_get_error(input));
		doveadm_exit_code = EX_TEMPFAIL;
	}
	i_stream_unref(&input);
	fs_file_deinit(&file);
	fs_deinit(&fs);
}

static void cmd_fs_put(struct doveadm_cmd_context *cctx)
{
	const char *hash_str, *src_path, *dest_path;
	buffer_t *hash = NULL;

	if (!doveadm_cmd_param_str(cctx, "input-path", &src_path) ||
	    !doveadm_cmd_param_str(cctx, "path", &dest_path))
		fs_cmd_help(cctx);
	if (doveadm_cmd_param_str(cctx, "hash", &hash_str)) {
		hash = t_buffer_create(32);
		if (hex_to_binary(optarg, hash) < 0)
			i_fatal("Invalid -h parameter: Hash not in hex");
	}
	doveadm_fs_put(cctx, src_path, dest_path, hash);
}

void doveadm_fs_put(struct doveadm_cmd_context *cctx,
		    const char *src_path, const char *dest_path,
		    const buffer_t *hash)
{
	struct fs *fs;
	enum fs_properties props;
	const char *const *metas;
	struct fs_file *file;
	struct istream *input;
	struct ostream *output;

	fs = doveadm_fs_init(cctx);

	file = fs_file_init(fs, dest_path, FS_OPEN_MODE_REPLACE);
	props = fs_get_properties(fs);
	if (hash == NULL)
		;
	else if (hash->used == hash_method_md5.digest_size) {
		if ((props & FS_PROPERTY_WRITE_HASH_MD5) == 0)
			i_fatal("fs backend doesn't support MD5 hashes");
		fs_write_set_hash(file,
			hash_method_lookup(hash_method_md5.name), hash->data);
	} else  if (hash->used == hash_method_sha256.digest_size) {
		if ((props & FS_PROPERTY_WRITE_HASH_SHA256) == 0)
			i_fatal("fs backend doesn't support SHA256 hashes");
		fs_write_set_hash(file,
			hash_method_lookup(hash_method_sha256.name), hash->data);
	}

	if (doveadm_cmd_param_array(cctx, "metadata", &metas)) {
		if ((props & FS_PROPERTY_METADATA) == 0)
			i_fatal("No metadata support");
		for (const char *const *meta = metas; *meta != NULL; meta++) {
			const char *key, *value;
			if (!t_split_key_value_eq(*meta, &key, &value)) {
				i_fatal("Metadata must be in key=value format, got '%s'",
					*meta);
			}
			fs_set_metadata(file, key, value);
		};
	}

	output = fs_write_stream(file);
	input = i_stream_create_file(src_path, IO_BLOCK_SIZE);
	o_stream_nsend_istream(output, input);
	i_stream_destroy(&input);
	if (fs_write_stream_finish(file, &output) < 0) {
		e_error(cctx->event, "fs_write_stream_finish() failed: %s",
			fs_file_last_error(file));
		doveadm_exit_code = EX_TEMPFAIL;
	}
	fs_file_deinit(&file);
	fs_deinit(&fs);
}

static void cmd_fs_copy(struct doveadm_cmd_context *cctx)
{
	const char *src_path, *dest_path;

	if (!doveadm_cmd_param_str(cctx, "source-path", &src_path) ||
	    !doveadm_cmd_param_str(cctx, "destination-path", &dest_path))
		fs_cmd_help(cctx);
	doveadm_fs_copy(cctx, src_path, dest_path);
}

void doveadm_fs_copy(struct doveadm_cmd_context *cctx,
		     const char *src_path, const char *dest_path)
{
	struct fs *fs;
	struct fs_file *src_file, *dest_file;

	fs = doveadm_fs_init(cctx);
	src_file = fs_file_init(fs, src_path, FS_OPEN_MODE_READONLY);
	dest_file = fs_file_init(fs, dest_path, FS_OPEN_MODE_REPLACE);
	if (fs_copy(src_file, dest_file) == 0) ;
	else if (errno == ENOENT) {
		e_error(cctx->event, "%s doesn't exist: %s", src_path,
			fs_file_last_error(dest_file));
		doveadm_exit_code = DOVEADM_EX_NOTFOUND;
	} else {
		e_error(cctx->event, "fs_copy(%s, %s) failed: %s",
			src_path, dest_path, fs_file_last_error(dest_file));
		doveadm_exit_code = EX_TEMPFAIL;
	}
	fs_file_deinit(&src_file);
	fs_file_deinit(&dest_file);
	fs_deinit(&fs);
}

static void cmd_fs_stat(struct doveadm_cmd_context *cctx)
{
	const char *path;

	doveadm_print_init(DOVEADM_PRINT_TYPE_FORMATTED);
	doveadm_print_formatted_set_format("%{path} size=%{size}");
	doveadm_print_header_simple("path");
	doveadm_print_header("size", "size", DOVEADM_PRINT_HEADER_FLAG_NUMBER);

	if (!doveadm_cmd_param_str(cctx, "path", &path))
		fs_cmd_help(cctx);
	doveadm_fs_stat(cctx, path);
}

void doveadm_fs_stat(struct doveadm_cmd_context *cctx, const char *path)
{
	struct fs *fs;
	struct fs_file *file;
	struct stat st;

	fs = doveadm_fs_init(cctx);
	file = fs_file_init(fs, path, FS_OPEN_MODE_READONLY);

	if (fs_stat(file, &st) == 0) {
		doveadm_print(fs_file_path(file));
		doveadm_print(dec2str(st.st_size));
	} else if (errno == ENOENT) {
		e_error(cctx->event,
			"%s doesn't exist: %s", fs_file_path(file),
			fs_file_last_error(file));
		doveadm_exit_code = DOVEADM_EX_NOTFOUND;
	} else {
		e_error(cctx->event, "fs_stat(%s) failed: %s",
			fs_file_path(file), fs_file_last_error(file));
		doveadm_exit_code = EX_TEMPFAIL;
	}
	fs_file_deinit(&file);
	fs_deinit(&fs);
}

static void cmd_fs_metadata(struct doveadm_cmd_context *cctx)
{
	const char *path;

	doveadm_print_init(DOVEADM_PRINT_TYPE_FORMATTED);
	doveadm_print_formatted_set_format("%{key}=%{value}\n");
	doveadm_print_header_simple("key");
	doveadm_print_header_simple("value");

	if (!doveadm_cmd_param_str(cctx, "path", &path))
		fs_cmd_help(cctx);
	doveadm_fs_metadata(cctx, path);
}

void doveadm_fs_metadata(struct doveadm_cmd_context *cctx, const char *path)
{
	struct fs *fs;
	struct fs_file *file;
	const struct fs_metadata *m;
	const ARRAY_TYPE(fs_metadata) *metadata;

	fs = doveadm_fs_init(cctx);
	file = fs_file_init(fs, path, FS_OPEN_MODE_READONLY);
	if (fs_get_metadata(file, &metadata) == 0) {
		array_foreach(metadata, m) {
			doveadm_print(m->key);
			doveadm_print(m->value);
		}
	} else if (errno == ENOENT) {
		e_error(cctx->event,
			"%s doesn't exist: %s", fs_file_path(file),
			fs_file_last_error(file));
		doveadm_exit_code = DOVEADM_EX_NOTFOUND;
	} else {
		e_error(cctx->event, "fs_stat(%s) failed: %s",
			fs_file_path(file), fs_file_last_error(file));
		doveadm_exit_code = EX_TEMPFAIL;
	}
	fs_file_deinit(&file);
	fs_deinit(&fs);
}

struct fs_delete_ctx {
	struct doveadm_cmd_context *cctx;
	struct fs *fs;
	const char *path_prefix;

	unsigned int files_count;
	struct fs_file **files;
};

static int cmd_fs_delete_ctx_run(struct fs_delete_ctx *ctx)
{
	unsigned int i;
	int ret = 0;

	for (i = 0; i < ctx->files_count; i++) {
		if (ctx->files[i] == NULL)
			;
		else if (fs_delete(ctx->files[i]) == 0)
			fs_file_deinit(&ctx->files[i]);
		else if (errno == EAGAIN) {
			if (ret == 0)
				ret = 1;
		} else if (errno == ENOENT) {
			e_error(ctx->cctx->event,
				"%s doesn't exist: %s", fs_file_path(ctx->files[i]),
				fs_file_last_error(ctx->files[i]));
			doveadm_exit_code = DOVEADM_EX_NOTFOUND;
			ret = -1;
		} else {
			e_error(ctx->cctx->event,
				"fs_delete(%s) failed: %s",
				fs_file_path(ctx->files[i]),
				fs_file_last_error(ctx->files[i]));
			doveadm_exit_code = EX_TEMPFAIL;
			ret = -1;
		}
	}
	return ret;
}

static int doveadm_fs_delete_async_fname(struct fs_delete_ctx *ctx,
					 const char *fname)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ctx->files_count; i++) {
		if (ctx->files[i] != NULL)
			continue;

		ctx->files[i] = fs_file_init(ctx->fs,
				t_strdup_printf("%s%s", ctx->path_prefix, fname),
				FS_OPEN_MODE_READONLY | FS_OPEN_FLAG_ASYNC |
				FS_OPEN_FLAG_ASYNC_NOQUEUE);
		fname = NULL;
		break;
	}
	if ((ret = cmd_fs_delete_ctx_run(ctx)) < 0)
		return -1;
	if (fname != NULL) {
		if (ret > 0)
			fs_wait_async(ctx->fs);
		return doveadm_fs_delete_async_fname(ctx, fname);
	}
	return 0;
}

static void doveadm_fs_delete_async_finish(struct fs_delete_ctx *ctx)
{
	unsigned int i;

	while (doveadm_exit_code == 0 && cmd_fs_delete_ctx_run(ctx) > 0) {
		fs_wait_async(ctx->fs);
	}
	for (i = 0; i < ctx->files_count; i++) {
		fs_file_deinit(&ctx->files[i]);
	}
}

static void
cmd_fs_delete_dir_recursive(struct doveadm_cmd_context *cctx,
			    struct fs *fs, unsigned int async_count,
			    const char *path_prefix)
{
	struct fs_iter *iter;
	ARRAY_TYPE(const_string) fnames;
	struct fs_delete_ctx ctx;
	const char *fname, *error;
	int ret;

	i_zero(&ctx);
	ctx.cctx = cctx;
	ctx.fs = fs;
	ctx.path_prefix = path_prefix;
	ctx.files_count = I_MAX(async_count, 1);
	ctx.files = t_new(struct fs_file *, ctx.files_count);

	/* delete subdirs first. all fs backends can't handle recursive
	   lookups, so save the list first. */
	t_array_init(&fnames, 8);
	iter = fs_iter_init(fs, path_prefix, FS_ITER_FLAG_DIRS);
	while ((fname = fs_iter_next(iter)) != NULL) {
		/* append "/" so that if FS_PROPERTY_DIRECTORIES is set,
		   we'll include the "/" suffix in the filename when deleting
		   it. */
		fname = t_strconcat(fname, "/", NULL);
		array_push_back(&fnames, &fname);
	}
	if (fs_iter_deinit(&iter, &error) < 0) {
		e_error(cctx->event, "fs_iter_deinit(%s) failed: %s",
			path_prefix, error);
		doveadm_exit_code = EX_TEMPFAIL;
	}
	array_foreach_elem(&fnames, fname) T_BEGIN {
		cmd_fs_delete_dir_recursive(cctx, fs, async_count,
			t_strdup_printf("%s%s", path_prefix, fname));
	} T_END;

	/* delete files. again because we're doing this asynchronously finish
	   the iteration first. */
	if ((fs_get_properties(fs) & FS_PROPERTY_DIRECTORIES) != 0) {
		/* we need to explicitly delete also the directories */
	} else {
		array_clear(&fnames);
	}
	iter = fs_iter_init(fs, path_prefix, 0);
	while ((fname = fs_iter_next(iter)) != NULL) {
		fname = t_strdup(fname);
		array_push_back(&fnames, &fname);
	}
	if (fs_iter_deinit(&iter, &error) < 0) {
		e_error(cctx->event, "fs_iter_deinit(%s) failed: %s",
			path_prefix, error);
		doveadm_exit_code = EX_TEMPFAIL;
	}

	array_foreach_elem(&fnames, fname) {
		T_BEGIN {
			ret = doveadm_fs_delete_async_fname(&ctx, fname);
		} T_END;
		if (ret < 0)
			break;
	}
	doveadm_fs_delete_async_finish(&ctx);
}

static void cmd_fs_delete_recursive_path(struct doveadm_cmd_context *cctx,
					 struct fs *fs, const char *path,
					 unsigned int async_count)
{
	struct fs_file *file;
	size_t path_len;

	path_len = strlen(path);
	if (path_len > 0 && path[path_len-1] != '/')
		path = t_strconcat(path, "/", NULL);

	cmd_fs_delete_dir_recursive(cctx, fs, async_count, path);
	if ((fs_get_properties(fs) & FS_PROPERTY_DIRECTORIES) != 0) {
		/* delete the root itself */
		file = fs_file_init(fs, path, FS_OPEN_MODE_READONLY);
		if (fs_delete(file) < 0) {
			e_error(cctx->event, "fs_delete(%s) failed: %s",
				fs_file_path(file), fs_file_last_error(file));
			doveadm_exit_code = EX_TEMPFAIL;
		}
		fs_file_deinit(&file);
	}
}

void doveadm_fs_delete_recursive(struct doveadm_cmd_context *cctx,
				 const char *const *paths,
				 unsigned int async_count)
{
	struct fs *fs;
	unsigned int i;

	fs = doveadm_fs_init(cctx);
	for (i = 0; paths[i] != NULL; i++)
		cmd_fs_delete_recursive_path(cctx, fs, paths[i], async_count);
	fs_deinit(&fs);
}

void doveadm_fs_delete_paths(struct doveadm_cmd_context *cctx,
			     const char *const *paths,
			     unsigned int async_count)
{
	struct fs_delete_ctx ctx;
	unsigned int i;
	int ret;

	i_zero(&ctx);
	ctx.cctx = cctx;
	ctx.fs = doveadm_fs_init(cctx);
	ctx.path_prefix = "";
	ctx.files_count = I_MAX(async_count, 1);
	ctx.files = t_new(struct fs_file *, ctx.files_count);

	for (i = 0; paths[i] != NULL; i++) {
		T_BEGIN {
			ret = doveadm_fs_delete_async_fname(&ctx, paths[i]);
		} T_END;
		if (ret < 0)
			break;
	}
	doveadm_fs_delete_async_finish(&ctx);
	fs_deinit(&ctx.fs);
}

static void cmd_fs_delete(struct doveadm_cmd_context *cctx)
{
	const char *const *paths;
	bool recursive = FALSE;
	int64_t async_count = 0;

	(void)doveadm_cmd_param_bool(cctx, "recursive", &recursive);
	(void)doveadm_cmd_param_int64(cctx, "max-parallel", &async_count);

	if (!doveadm_cmd_param_array(cctx, "path", &paths))
		fs_cmd_help(cctx);

	if (recursive)
		doveadm_fs_delete_recursive(cctx, paths, async_count);
	else
		doveadm_fs_delete_paths(cctx, paths, async_count);
}

static void cmd_fs_iter_full(struct doveadm_cmd_context *cctx,
			     enum fs_iter_flags flags)
{
	const char *path;
	bool b;

	if (doveadm_cmd_param_bool(cctx, "no-cache", &b) && b)
		flags |= FS_ITER_FLAG_NOCACHE;
	if (doveadm_cmd_param_bool(cctx, "object-ids", &b) && b)
		flags |= FS_ITER_FLAG_OBJECTIDS;

	if (!doveadm_cmd_param_str(cctx, "path", &path))
		fs_cmd_help(cctx);

	doveadm_print_init(DOVEADM_PRINT_TYPE_FORMATTED);
	doveadm_print_formatted_set_format("%{path}\n");
	doveadm_print_header_simple("path");
	doveadm_fs_iter(cctx, flags, path);
}

void doveadm_fs_iter(struct doveadm_cmd_context *cctx,
		     enum fs_iter_flags flags, const char *path)
{
	struct fs *fs;
	struct fs_iter *iter;
	const char *fname, *error;

	fs = doveadm_fs_init(cctx);
	iter = fs_iter_init(fs, path, flags);
	while ((fname = fs_iter_next(iter)) != NULL) {
		doveadm_print(fname);
	}
	if (fs_iter_deinit(&iter, &error) < 0) {
		e_error(cctx->event,
			"fs_iter_deinit(%s) failed: %s", path, error);
		doveadm_exit_code = EX_TEMPFAIL;
	}
	fs_deinit(&fs);
}

static void cmd_fs_iter(struct doveadm_cmd_context *cctx)
{
	cmd_fs_iter_full(cctx, 0);
}

static void cmd_fs_iter_dirs(struct doveadm_cmd_context *cctx)
{
	cmd_fs_iter_full(cctx, FS_ITER_FLAG_DIRS);
}

struct doveadm_cmd_ver2 doveadm_cmd_fs[] = {
{
	.name = "fs get",
	.cmd = cmd_fs_get,
	.usage = "<config-filter-name> <path>",
DOVEADM_CMD_PARAMS_START
DOVEADM_CMD_PARAM('\0', "filter-name", CMD_PARAM_STR, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAM('\0', "path", CMD_PARAM_STR, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAMS_END
},
{
	.name = "fs put",
	.cmd = cmd_fs_put,
	.usage = "[-h <hash>] [-m <key>=<value>] <config-filter-name> <input path> <path>",
DOVEADM_CMD_PARAMS_START
DOVEADM_CMD_PARAM('h', "hash", CMD_PARAM_STR, 0)
DOVEADM_CMD_PARAM('m', "metadata", CMD_PARAM_ARRAY, 0)
DOVEADM_CMD_PARAM('\0', "filter-name", CMD_PARAM_STR, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAM('\0', "input-path", CMD_PARAM_STR, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAM('\0', "path", CMD_PARAM_STR, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAMS_END
},
{
	.name = "fs copy",
	.cmd = cmd_fs_copy,
	.usage = "<config-filter-name> <source path> <dest path>",
DOVEADM_CMD_PARAMS_START
DOVEADM_CMD_PARAM('\0', "filter-name", CMD_PARAM_STR, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAM('\0', "source-path", CMD_PARAM_STR, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAM('\0', "destination-path", CMD_PARAM_STR, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAMS_END
},
{
	.name = "fs stat",
	.cmd = cmd_fs_stat,
	.usage = "<config-filter-name> <path>",
DOVEADM_CMD_PARAMS_START
DOVEADM_CMD_PARAM('\0', "filter-name", CMD_PARAM_STR, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAM('\0', "path", CMD_PARAM_STR, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAMS_END
},
{
	.name = "fs metadata",
	.cmd = cmd_fs_metadata,
	.usage = "<config-filter-name> <path>",
DOVEADM_CMD_PARAMS_START
DOVEADM_CMD_PARAM('\0', "filter-name", CMD_PARAM_STR, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAM('\0', "path", CMD_PARAM_STR, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAMS_END
},
{
	.name = "fs delete",
	.cmd = cmd_fs_delete,
	.usage = "[-R] [-n <count>] <config-filter-name> <path> [<path> ...]",
DOVEADM_CMD_PARAMS_START
DOVEADM_CMD_PARAM('R', "recursive", CMD_PARAM_BOOL, 0)
DOVEADM_CMD_PARAM('n', "max-parallel", CMD_PARAM_INT64, CMD_PARAM_FLAG_UNSIGNED)
DOVEADM_CMD_PARAM('\0', "filter-name", CMD_PARAM_STR, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAM('\0', "path", CMD_PARAM_ARRAY, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAMS_END
},
{
	.name = "fs iter",
	.cmd = cmd_fs_iter,
	.usage = "[--no-cache] [--object-ids] <config-filter-name> <path>",
DOVEADM_CMD_PARAMS_START
DOVEADM_CMD_PARAM('C', "no-cache", CMD_PARAM_BOOL, 0)
DOVEADM_CMD_PARAM('O', "object-ids", CMD_PARAM_BOOL, 0)
DOVEADM_CMD_PARAM('\0', "filter-name", CMD_PARAM_STR, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAM('\0', "path", CMD_PARAM_STR, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAMS_END
},
{
	.name = "fs iter-dirs",
	.cmd = cmd_fs_iter_dirs,
	.usage = "<config-filter-name> <path>",
DOVEADM_CMD_PARAMS_START
DOVEADM_CMD_PARAM('\0', "filter-name", CMD_PARAM_STR, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAM('\0', "path", CMD_PARAM_STR, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAMS_END
}
};

static void fs_cmd_help(struct doveadm_cmd_context *cctx)
{
	unsigned int i;

	for (i = 0; i < N_ELEMENTS(doveadm_cmd_fs); i++) {
		if (doveadm_cmd_fs[i].cmd == cctx->cmd->cmd)
			help_ver2(&doveadm_cmd_fs[i]);
	}
	i_unreached();
}

void doveadm_register_fs_commands(void)
{
	unsigned int i;

	for (i = 0; i < N_ELEMENTS(doveadm_cmd_fs); i++)
		doveadm_cmd_register_ver2(&doveadm_cmd_fs[i]);
}
