/* Copyright (c) 2013-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "settings.h"
#include "dict.h"
#include "index-storage.h"

struct index_storage_attribute_iter {
	struct mailbox_attribute_iter iter;
	struct dict_iterate_context *diter;
	char *prefix;
	size_t prefix_len;
	bool dict_disabled;
};

static struct mail_namespace *
mail_user_find_attribute_namespace(struct mail_user *user)
{
	struct mail_namespace *ns;

	ns = mail_namespace_find_inbox(user->namespaces);
	if (ns != NULL)
		return ns;

	for (ns = user->namespaces; ns != NULL; ns = ns->next) {
		if (ns->type == MAIL_NAMESPACE_TYPE_PRIVATE)
			return ns;
	}
	return NULL;
}

static int
index_storage_get_user_dict(struct mail_storage *err_storage,
			    struct mail_user *user, struct dict **dict_r)
{
	struct mail_namespace *ns;
	const char *error;
	int ret;

	if (user->_attr_dict != NULL) {
		*dict_r = user->_attr_dict;
		return 0;
	}
	if (user->attr_dict_failed) {
		mail_storage_set_internal_error(err_storage);
		return -1;
	}

	ns = mail_user_find_attribute_namespace(user);
	if (ns == NULL) {
		/* probably never happens? */
		mail_storage_set_error(err_storage, MAIL_ERROR_NOTPOSSIBLE,
			"Mailbox attributes not available for this mailbox");
		return -1;
	}

	struct event *event = event_create(user->event);
	settings_event_add_filter_name(event, "mail_attribute");
	ret = dict_init_auto(event, &user->_attr_dict, &error);
	event_unref(&event);

	if (ret < 0) {
		mail_storage_set_critical(err_storage,
			"mail_attribute: dict_init_auto() failed: %s",
			error);
		user->attr_dict_failed = TRUE;
		return -1;
	}
	if (ret == 0) {
		mail_storage_set_error(err_storage, MAIL_ERROR_NOTPOSSIBLE,
				       "Mailbox attributes not enabled");
		return -1;
	}

	*dict_r = user->_attr_dict;
	return 0;
}

static int
index_storage_get_dict(struct mailbox *box, enum mail_attribute_type type_flags,
		       struct dict **dict_r, const char **mailbox_prefix_r)
{
	enum mail_attribute_type type = type_flags & MAIL_ATTRIBUTE_TYPE_MASK;
	struct mail_storage *storage = box->storage;
	struct mail_namespace *ns;
	struct mailbox_metadata metadata;
	const char *error;
	int ret;

	if ((type_flags & MAIL_ATTRIBUTE_TYPE_FLAG_VALIDATED) != 0) {
		/* IMAP METADATA support isn't enabled, so don't allow using
		   the mail_attribute's dict. */
		mail_storage_set_error(storage, MAIL_ERROR_NOTPOSSIBLE,
				       "Generic mailbox attributes not enabled");
		return -1;
	}

	if (mailbox_get_metadata(box, MAILBOX_METADATA_GUID, &metadata) < 0)
		return -1;
	*mailbox_prefix_r = guid_128_to_string(metadata.guid);

	ns = mailbox_get_namespace(box);
	if (type == MAIL_ATTRIBUTE_TYPE_PRIVATE) {
		/* private attributes are stored in user's own dict */
		return index_storage_get_user_dict(storage, storage->user, dict_r);
	} else if (ns->user == ns->owner) {
		/* user owns the mailbox. shared attributes are stored in
		   the same dict. */
		return index_storage_get_user_dict(storage, storage->user, dict_r);
	} else if (ns->owner != NULL) {
		/* accessing shared attribute of a shared mailbox.
		   use the owner's dict. */
		return index_storage_get_user_dict(storage, ns->owner, dict_r);
	}

	/* accessing shared attributes of a public mailbox. no user owns it,
	   so use the storage's dict. */
	if (storage->_shared_attr_dict != NULL) {
		*dict_r = storage->_shared_attr_dict;
		return 0;
	}
	if (storage->shared_attr_dict_failed) {
		mail_storage_set_internal_error(storage);
		return -1;
	}

	struct event *event = event_create(storage->event);
	settings_event_add_filter_name(event, "mail_attribute");
	ret = dict_init_auto(event, &storage->_shared_attr_dict, &error);
	event_unref(&event);

	if (ret < 0) {
		mail_storage_set_critical(storage,
			"mail_attribute: dict_init_auto() failed: %s",
			error);
		storage->shared_attr_dict_failed = TRUE;
		return -1;
	}
	if (ret == 0) {
		mail_storage_set_error(storage, MAIL_ERROR_NOTPOSSIBLE,
				       "Mailbox attributes not enabled");
		return -1;
	}

	*dict_r = storage->_shared_attr_dict;
	return 0;
}

static const char *
key_get_prefixed(enum mail_attribute_type type_flags, const char *mailbox_prefix,
		 const char *key)
{
	enum mail_attribute_type type = type_flags & MAIL_ATTRIBUTE_TYPE_MASK;

	switch (type) {
	case MAIL_ATTRIBUTE_TYPE_PRIVATE:
		return t_strconcat(DICT_PATH_PRIVATE, mailbox_prefix, "/",
				   key, NULL);
	case MAIL_ATTRIBUTE_TYPE_SHARED:
		return t_strconcat(DICT_PATH_SHARED, mailbox_prefix, "/",
				   key, NULL);
	}
	i_unreached();
}

static int
index_storage_attribute_get_dict_trans(struct mailbox_transaction_context *t,
				       enum mail_attribute_type type_flags,
				       struct dict_transaction_context **dtrans_r,
				       const char **mailbox_prefix_r)
{
	enum mail_attribute_type type = type_flags & MAIL_ATTRIBUTE_TYPE_MASK;
	struct dict_transaction_context **dtransp = NULL;
	struct dict *dict;
	struct mailbox_metadata metadata;

	switch (type) {
	case MAIL_ATTRIBUTE_TYPE_PRIVATE:
		dtransp = &t->attr_pvt_trans;
		break;
	case MAIL_ATTRIBUTE_TYPE_SHARED:
		dtransp = &t->attr_shared_trans;
		break;
	}
	i_assert(dtransp != NULL);

	if (*dtransp != NULL &&
	    (type_flags & MAIL_ATTRIBUTE_TYPE_FLAG_VALIDATED) == 0) {
		/* Transaction already created. Even if it was, don't use it
		   if _FLAG_VALIDATED is being used. It'll be handled below by
		   returning failure. */
		if (mailbox_get_metadata(t->box, MAILBOX_METADATA_GUID,
					 &metadata) < 0)
			return -1;
		*mailbox_prefix_r = guid_128_to_string(metadata.guid);
		*dtrans_r = *dtransp;
		return 0;
	}

	if (index_storage_get_dict(t->box, type_flags, &dict, mailbox_prefix_r) < 0)
		return -1;
	i_assert(*dtransp == NULL);

	struct mail_user *user = mailbox_list_get_user(t->box->list);
	const struct dict_op_settings *set = mail_user_get_dict_op_settings(user);
	*dtransp = *dtrans_r = dict_transaction_begin(dict, set);
	return 0;
}

int index_storage_attribute_set(struct mailbox_transaction_context *t,
				enum mail_attribute_type type_flags,
				const char *key,
				const struct mail_attribute_value *value)
{
	enum mail_attribute_type type = type_flags & MAIL_ATTRIBUTE_TYPE_MASK;
	struct dict_transaction_context *dtrans;
	const char *mailbox_prefix;
	bool pvt = type == MAIL_ATTRIBUTE_TYPE_PRIVATE;
	time_t ts = value->last_change != 0 ? value->last_change : ioloop_time;
	int ret = 0;

	if (index_storage_attribute_get_dict_trans(t, type_flags, &dtrans,
						   &mailbox_prefix) < 0)
		return -1;

	T_BEGIN {
		const char *prefixed_key =
			key_get_prefixed(type_flags, mailbox_prefix, key);
		const char *value_str;

		if (mailbox_attribute_value_to_string(t->box->storage, value,
						      &value_str) < 0) {
			ret = -1;
		} else if (value_str != NULL) {
			dict_set(dtrans, prefixed_key, value_str);
			mail_index_attribute_set(t->itrans, pvt, key,
						 ts, strlen(value_str));
		} else {
			dict_unset(dtrans, prefixed_key);
			mail_index_attribute_unset(t->itrans, pvt, key, ts);
		}
	} T_END;
	return ret;
}

int index_storage_attribute_get(struct mailbox *box,
				enum mail_attribute_type type_flags,
				const char *key,
				struct mail_attribute_value *value_r)
{
	struct dict *dict;
	const char *mailbox_prefix, *error;
	int ret;

	i_zero(value_r);

	if (index_storage_get_dict(box, type_flags, &dict, &mailbox_prefix) < 0)
		return -1;

	struct mail_user *user = mailbox_list_get_user(box->list);
	const struct dict_op_settings *set = mail_user_get_dict_op_settings(user);
	ret = dict_lookup(dict, set, pool_datastack_create(),
			  key_get_prefixed(type_flags, mailbox_prefix, key),
			  &value_r->value, &error);
	if (ret < 0) {
		mailbox_set_critical(box,
			"Failed to get attribute %s: %s", key, error);
		return -1;
	}
	return ret;
}

struct mailbox_attribute_iter *
index_storage_attribute_iter_init(struct mailbox *box,
				  enum mail_attribute_type type_flags,
				  const char *prefix)
{
	struct index_storage_attribute_iter *iter;
	struct dict *dict;
	const char *mailbox_prefix;

	iter = i_new(struct index_storage_attribute_iter, 1);
	iter->iter.box = box;
	if (index_storage_get_dict(box, type_flags, &dict, &mailbox_prefix) < 0) {
		if (mailbox_get_last_mail_error(box) == MAIL_ERROR_NOTPOSSIBLE) {
			mail_storage_clear_error(box->storage);
			iter->dict_disabled = TRUE;
		}
	} else {
		iter->prefix = i_strdup(key_get_prefixed(type_flags, mailbox_prefix,
							 prefix));
		iter->prefix_len = strlen(iter->prefix);
		struct mail_user *user = mailbox_list_get_user(box->list);
		const struct dict_op_settings *set = mail_user_get_dict_op_settings(user);
		iter->diter = dict_iterate_init(dict, set, iter->prefix,
						DICT_ITERATE_FLAG_RECURSE |
						DICT_ITERATE_FLAG_NO_VALUE);
	}
	return &iter->iter;
}

const char *
index_storage_attribute_iter_next(struct mailbox_attribute_iter *_iter)
{
	struct index_storage_attribute_iter *iter =
		(struct index_storage_attribute_iter *)_iter;
	const char *key, *value;

	if (iter->diter == NULL || !dict_iterate(iter->diter, &key, &value))
		return NULL;

	i_assert(strncmp(key, iter->prefix, iter->prefix_len) == 0);
	key += iter->prefix_len;
	return key;
}

int index_storage_attribute_iter_deinit(struct mailbox_attribute_iter *_iter)
{
	struct index_storage_attribute_iter *iter =
		(struct index_storage_attribute_iter *)_iter;
	const char *error;
	int ret;

	if (iter->diter == NULL) {
		ret = iter->dict_disabled ? 0 : -1;
	} else {
		if ((ret = dict_iterate_deinit(&iter->diter, &error)) < 0) {
			mailbox_set_critical(_iter->box,
				"dict_iterate(%s) failed: %s",
				iter->prefix, error);
		}
	}
	i_free(iter->prefix);
	i_free(iter);
	return ret;
}
