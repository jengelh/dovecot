/* Copyright (c) 2017-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "test-lib.h"
#include "settings.h"
#include "sql-api-private.h"
#include "dict.h"
#include "dict-private.h"
#include "dict-sql.h"
#include "dict-sql-private.h"
#include "dict-sql-settings.h"
#include "driver-test.h"

struct dict_op_settings dict_op_settings = {
	.username = "testuser",
};

static struct settings_simple set;

static void test_setup(struct dict **dict_r)
{
	settings_simple_init(&set, (const char *const []) {
		"dict", "sql",
		"dict/sql/sql_driver", "mysql",
		"dict/sql/host", "localhost",
		"dict_map", "1 2 3 4 5",

		"dict_map/1/pattern", "shared/dictmap/$key1/$key2",
		"dict_map/1/sql_table", "table",
		"dict_map/1/dict_map_value_field", "value",
		"dict_map/1/dict_map_value_field/value/name", "value",
		"dict_map/1/dict_map_key_field", "a b",
		"dict_map/1/dict_map_key_field/a/value", "$key1",
		"dict_map/1/dict_map_key_field/b/value", "$key2",

		"dict_map/2/pattern", "shared/counters/$class/$name",
		"dict_map/2/sql_table", "counters",
		"dict_map/2/dict_map_value_field", "value",
		"dict_map/2/dict_map_value_field/value/type", "uint",
		"dict_map/2/dict_map_key_field", "class name",
		"dict_map/2/dict_map_key_field/class/value", "$class",
		"dict_map/2/dict_map_key_field/name/value", "$name",

		"dict_map/3/pattern", "priv/quota/bytes",
		"dict_map/3/sql_table", "quota",
		"dict_map/3/username_field", "username",
		"dict_map/3/dict_map_value_field", "bytes",
		"dict_map/3/dict_map_value_field/bytes/type", "uint",

		"dict_map/4/pattern", "priv/quota/count",
		"dict_map/4/sql_table", "quota",
		"dict_map/4/username_field", "username",
		"dict_map/4/dict_map_value_field", "count",
		"dict_map/4/dict_map_value_field/count/type", "uint",

		"dict_map/5/pattern", "priv/quota/folders",
		"dict_map/5/sql_table", "quota",
		"dict_map/5/username_field", "username",
		"dict_map/5/dict_map_value_field", "folders",
		"dict_map/5/dict_map_value_field/folders/type", "uint",

		NULL,
	});
	const char *error = NULL;
	struct dict *dict = NULL;

	if (dict_init_auto(set.event, &dict, &error) <= 0)
		i_fatal("cannot initialize dict: %s", error);

	*dict_r = dict;
}

static void test_teardown(struct dict **_dict)
{
	struct dict *dict = *_dict;
	*_dict = NULL;
	if (dict != NULL) {
		dict_deinit(&dict);
	}
	settings_simple_deinit(&set);
}

static void test_set_expected(struct dict *_dict,
			      const struct test_driver_result *result)
{
	struct sql_dict *dict =
		(struct sql_dict *)_dict;

	sql_driver_test_add_expected_result(dict->db, result);
}

static void test_lookup_one(void)
{
	const char *value = NULL, *error = NULL;
	struct test_driver_result_set rset = {
		.rows = 1,
		.cols = 1,
		.col_names = (const char *[]){"value", NULL},
		.row_data = (const char **[]){(const char*[]){"one", NULL}},
	};
	struct test_driver_result res = {
		.nqueries = 1,
		.queries = (const char *[]){"SELECT value FROM table WHERE a = 'hello' AND b = 'world'", NULL},
		.result = &rset,
	};
	const struct dict_op_settings set = {
		.username = "testuser",
	};
	struct dict *dict;
	pool_t pool = pool_datastack_create();

	test_begin("dict lookup one");
	test_setup(&dict);

	test_set_expected(dict, &res);

	test_assert(dict_lookup(dict, &set, pool, "shared/dictmap/hello/world", &value, &error) == 1);
	test_assert_strcmp(value, "one");
        if (error != NULL)
                i_error("dict_lookup failed: %s", error);
	test_teardown(&dict);
	test_end();
}

static void test_atomic_inc(void)
{
	const char *error;
	struct test_driver_result res = {
		.nqueries = 3,
		.queries = (const char *[]){
			"UPDATE counters SET value=value+128 WHERE class = 'global' AND name = 'counter'",
			"UPDATE quota SET bytes=bytes+128,count=count+1 WHERE username = 'testuser'",
			"UPDATE quota SET bytes=bytes+128,count=count+1,folders=folders+123 WHERE username = 'testuser'",
			NULL},
		.result = NULL,
	};
	struct dict_op_settings set = {
		.username = "testuser",
	};
	struct dict *dict;

	test_begin("dict atomic inc");
	test_setup(&dict);

	test_set_expected(dict, &res);

	/* 1 field */
	struct dict_transaction_context *ctx = dict_transaction_begin(dict, &set);
	dict_atomic_inc(ctx, "shared/counters/global/counter", 128);
	test_assert(dict_transaction_commit(&ctx, &error) == 0);
        if (error != NULL)
                i_error("dict_transaction_commit failed: %s", error);
	error = NULL;

	/* 2 fields */
	ctx = dict_transaction_begin(dict, &set);
	dict_atomic_inc(ctx, "priv/quota/bytes", 128);
	dict_atomic_inc(ctx, "priv/quota/count", 1);
	test_assert(dict_transaction_commit(&ctx, &error) == 0);
        if (error != NULL)
		i_error("dict_transaction_commit failed: %s", error);
	error = NULL;

	/* 3 fields */
	ctx = dict_transaction_begin(dict, &set);
	dict_atomic_inc(ctx, "priv/quota/bytes", 128);
	dict_atomic_inc(ctx, "priv/quota/count", 1);
	dict_atomic_inc(ctx, "priv/quota/folders", 123);
	test_assert(dict_transaction_commit(&ctx, &error) == 0);
        if (error != NULL)
                i_error("dict_transaction_commit failed: %s", error);
	test_teardown(&dict);
	test_end();
}

static void test_set(void)
{
	const char *error;
	struct test_driver_result res = {
		.affected_rows = 1,
		.nqueries = 3,
		.queries = (const char *[]){
			"INSERT INTO counters (value,class,name) VALUES (128,'global','counter') ON DUPLICATE KEY UPDATE value=128",
			"INSERT INTO quota (bytes,count,username) VALUES (128,1,'testuser') ON DUPLICATE KEY UPDATE bytes=128,count=1",
			"INSERT INTO quota (bytes,count,folders,username) VALUES (128,1,123,'testuser') ON DUPLICATE KEY UPDATE bytes=128,count=1,folders=123",
			NULL},
		.result = NULL,
	};
	struct dict *dict;

	test_begin("dict set");
	test_setup(&dict);

	test_set_expected(dict, &res);

	/* 1 field */
	struct dict_transaction_context *ctx = dict_transaction_begin(dict, &dict_op_settings);
	dict_set(ctx, "shared/counters/global/counter", "128");
	test_assert(dict_transaction_commit(&ctx, &error) == 1);
        if (error != NULL)
                i_error("dict_transaction_commit failed: %s", error);
	error = NULL;

	/* 2 fields */
	ctx = dict_transaction_begin(dict, &dict_op_settings);
	dict_set(ctx, "priv/quota/bytes", "128");
	dict_set(ctx, "priv/quota/count", "1");
	test_assert(dict_transaction_commit(&ctx, &error) == 1);
        if (error != NULL)
                i_error("dict_transaction_commit failed: %s", error);
	error = NULL;

	/* 3 fields */
	ctx = dict_transaction_begin(dict, &dict_op_settings);
	dict_set(ctx, "priv/quota/bytes", "128");
	dict_set(ctx, "priv/quota/count", "1");
	dict_set(ctx, "priv/quota/folders", "123");
	test_assert(dict_transaction_commit(&ctx, &error) == 1);
        if (error != NULL)
                i_error("dict_transaction_commit failed: %s", error);
	test_teardown(&dict);
	test_end();
}

static void test_unset(void)
{
	const char *error;
	struct test_driver_result res = {
		.affected_rows = 1,
		.nqueries = 3,
		.queries = (const char *[]){
			"DELETE FROM counters WHERE class = 'global' AND name = 'counter'",
			"DELETE FROM quota WHERE username = 'testuser'",
			"DELETE FROM quota WHERE username = 'testuser'",
			NULL},
		.result = NULL,
	};
	struct dict *dict;

	test_begin("dict unset");
	test_setup(&dict);

	test_set_expected(dict, &res);

	struct dict_transaction_context *ctx = dict_transaction_begin(dict, &dict_op_settings);
	dict_unset(ctx, "shared/counters/global/counter");
	test_assert(dict_transaction_commit(&ctx, &error) == 1);
	if (error != NULL)
                i_error("dict_transaction_commit failed: %s", error);
	error = NULL;
	ctx = dict_transaction_begin(dict, &dict_op_settings);
	dict_unset(ctx, "priv/quota/bytes");
	dict_unset(ctx, "priv/quota/count");
	test_assert(dict_transaction_commit(&ctx, &error) == 1);
        if (error != NULL)
                i_error("dict_transaction_commit failed: %s", error);
	test_teardown(&dict);
	test_end();
}

static void test_iterate(void)
{
	const char *key = NULL, *value = NULL, *error;
	struct test_driver_result_set rset = {
		.rows = 5,
		.cols = 2,
		.col_names = (const char *[]){"value", "name", NULL},
		.row_data = (const char **[]){
			(const char*[]){"one", "counter", NULL},
			(const char*[]){"two", "counter", NULL},
			(const char*[]){"three", "counter", NULL},
			(const char*[]){"four", "counter", NULL},
			(const char*[]){"five", "counter", NULL},
		},
	};
	struct test_driver_result res = {
		.nqueries = 1,
		.queries = (const char *[]){
			"SELECT value,name FROM counters WHERE class = 'global' AND name = 'counter'",
			NULL},
		.result = &rset,
	};
	struct dict *dict;

	test_begin("dict iterate");
	test_setup(&dict);

	test_set_expected(dict, &res);

	struct dict_iterate_context *iter =
		dict_iterate_init(dict, &dict_op_settings, "shared/counters/global/counter",
				  DICT_ITERATE_FLAG_EXACT_KEY);

	size_t idx = 0;
	while(dict_iterate(iter, &key, &value)) {
		i_assert(idx < rset.rows);
		test_assert_strcmp_idx(key, "shared/counters/global/counter", idx);
		test_assert_strcmp_idx(value, rset.row_data[idx][0], idx);
		idx++;
	}

	test_assert(idx == rset.rows);
	test_assert(dict_iterate_deinit(&iter, &error) == 0);
        if (error != NULL)
                i_error("dict_iterate_deinit failed: %s", error);
	error = NULL;

	res.queries = (const char*[]){
		"SELECT value,name FROM counters WHERE class = 'global' AND name LIKE '%' AND name NOT LIKE '%/%'",
		NULL
	};

	res.cur = 0;
	res.result->cur = 0;

	test_set_expected(dict, &res);

	iter = dict_iterate_init(dict, &dict_op_settings, "shared/counters/global/", 0);

	idx = 0;

	while(dict_iterate(iter, &key, &value)) {
		i_assert(idx < rset.rows);
		test_assert_strcmp_idx(key, "shared/counters/global/counter", idx);
		test_assert_strcmp_idx(value, rset.row_data[idx][0], idx);
		idx++;
	}

	test_assert(idx == rset.rows);
	test_assert(dict_iterate_deinit(&iter, &error) == 0);
	if (error != NULL)
		i_error("dict_iterate_deinit failed: %s", error);
	test_teardown(&dict);
	test_end();
}

int main(void) {
	sql_drivers_init_without_drivers();
	sql_driver_test_register();
	dict_sql_register();
	settings_info_register(&dict_map_setting_parser_info);

	static void (*const test_functions[])(void) = {
		test_lookup_one,
		test_atomic_inc,
		test_set,
		test_unset,
		test_iterate,
		NULL
	};

	int ret = test_run(test_functions);

	dict_sql_unregister();
	sql_driver_test_unregister();
	sql_drivers_deinit_without_drivers();

	return ret;
}
