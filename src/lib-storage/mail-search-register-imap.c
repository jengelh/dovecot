/* Copyright (c) 2010-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "str.h"
#include "imap-date.h"
#include "imap-seqset.h"
#include "imap-utf7.h"
#include "imap-util.h"
#include "mail-search-register.h"
#include "mail-search-parser.h"
#include "mail-search-build.h"
#include "mail-search-build.h"
#include "mail-search-mime-build.h"

struct mail_search_register *mail_search_register_imap4rev2;
struct mail_search_register *mail_search_register_imap4rev1;

static struct mail_search_arg *
imap_search_fallback(struct mail_search_build_context *ctx,
		     const char *key)
{
	struct mail_search_arg *sarg;

	if (*key == '*' || (*key >= '0' && *key <= '9')) {
		/* <message-set> */
		sarg = mail_search_build_new(ctx, SEARCH_SEQSET);
		p_array_init(&sarg->value.seqset, ctx->pool, 16);
		if (imap_seq_set_parse(key, &sarg->value.seqset) < 0) {
			ctx->_error = "Invalid messageset";
			return NULL;
		}
		return sarg;
	}
	ctx->_error = p_strconcat(ctx->pool, "Unknown argument ", key, NULL);
	return NULL;
}

static struct mail_search_arg *
imap_search_not(struct mail_search_build_context *ctx)
{
	struct mail_search_arg *sarg;

	if (mail_search_build_key(ctx, ctx->parent, &sarg) < 0)
		return NULL;

	sarg->match_not = !sarg->match_not;
	return sarg;
}

static struct mail_search_arg *
imap_search_or(struct mail_search_build_context *ctx)
{
	struct mail_search_arg *sarg, **subargs;

	/* <search-key1> <search-key2> */
	sarg = mail_search_build_new(ctx, SEARCH_OR);

	subargs = &sarg->value.subargs;
	do {
		if (mail_search_build_key(ctx, sarg, subargs) < 0)
			return NULL;
		subargs = &(*subargs)->next;

		/* <key> OR <key> OR ... <key> - put them all
		   under one SEARCH_OR list. */
	} while (mail_search_parse_skip_next(ctx->parser, "OR"));

	if (mail_search_build_key(ctx, sarg, subargs) < 0)
		return NULL;
	return sarg;
}

#define CALLBACK_STR(_func, _type) \
static struct mail_search_arg *\
imap_search_##_func(struct mail_search_build_context *ctx) \
{ \
	return mail_search_build_str(ctx, _type); \
}
static struct mail_search_arg *
imap_search_all(struct mail_search_build_context *ctx)
{
	return mail_search_build_new(ctx, SEARCH_ALL);
}

static struct mail_search_arg *
imap_search_uid(struct mail_search_build_context *ctx)
{
	struct mail_search_arg *sarg;

	/* <message set> */
	sarg = mail_search_build_str(ctx, SEARCH_UIDSET);
	if (sarg == NULL)
		return NULL;

	p_array_init(&sarg->value.seqset, ctx->pool, 16);
	if (strcmp(sarg->value.str, "$") == 0) {
		/* SEARCHRES: delay initialization */
	} else {
		if (imap_seq_set_parse(sarg->value.str,
				       &sarg->value.seqset) < 0) {
			ctx->_error = "Invalid UID messageset";
			return NULL;
		}
	}
	return sarg;
}

#define CALLBACK_FLAG(_func, _flag, _not) \
static struct mail_search_arg *\
imap_search_##_func(struct mail_search_build_context *ctx) \
{ \
	struct mail_search_arg *sarg; \
	sarg = mail_search_build_new(ctx, SEARCH_FLAGS); \
	sarg->value.flags = _flag; \
	sarg->match_not = _not; \
	return sarg; \
}
CALLBACK_FLAG(answered, MAIL_ANSWERED, FALSE)
CALLBACK_FLAG(unanswered, MAIL_ANSWERED, TRUE)
CALLBACK_FLAG(deleted, MAIL_DELETED, FALSE)
CALLBACK_FLAG(undeleted, MAIL_DELETED, TRUE)
CALLBACK_FLAG(draft, MAIL_DRAFT, FALSE)
CALLBACK_FLAG(undraft, MAIL_DRAFT, TRUE)
CALLBACK_FLAG(flagged, MAIL_FLAGGED, FALSE)
CALLBACK_FLAG(unflagged, MAIL_FLAGGED, TRUE)
CALLBACK_FLAG(seen, MAIL_SEEN, FALSE)
CALLBACK_FLAG(unseen, MAIL_SEEN, TRUE)
CALLBACK_FLAG(recent, MAIL_RECENT, FALSE)
CALLBACK_FLAG(old, MAIL_RECENT, TRUE)

static struct mail_search_arg *
imap_search_new(struct mail_search_build_context *ctx)
{
	struct mail_search_arg *sarg;

	/* NEW == (RECENT UNSEEN) */
	sarg = mail_search_build_new(ctx, SEARCH_SUB);
	sarg->value.subargs = imap_search_recent(ctx);
	sarg->value.subargs->next = imap_search_unseen(ctx);
	return sarg;
}

CALLBACK_STR(keyword, SEARCH_KEYWORDS)

static struct mail_search_arg *
imap_search_unkeyword(struct mail_search_build_context *ctx)
{
	struct mail_search_arg *sarg;

	sarg = imap_search_keyword(ctx);
	if (sarg != NULL)
		sarg->match_not = TRUE;
	return sarg;
}

static struct mail_search_arg *
arg_new_date(struct mail_search_build_context *ctx,
	     enum mail_search_arg_type type,
	     enum mail_search_date_type date_type)
{
	struct mail_search_arg *sarg;
	const char *value;

	sarg = mail_search_build_new(ctx, type);
	if (mail_search_parse_string(ctx->parser, &value) < 0)
		return NULL;
	if (!imap_parse_date(value, &sarg->value.time)) {
		ctx->_error = "Invalid search date parameter";
		return NULL;
	}
	sarg->value.date_type = date_type;
	return sarg;
}

#define CALLBACK_DATE(_func, _type, _date_type) \
static struct mail_search_arg *\
imap_search_##_func(struct mail_search_build_context *ctx) \
{ \
	return arg_new_date(ctx, _type, _date_type); \
}
CALLBACK_DATE(before, SEARCH_BEFORE, MAIL_SEARCH_DATE_TYPE_RECEIVED)
CALLBACK_DATE(on, SEARCH_ON, MAIL_SEARCH_DATE_TYPE_RECEIVED)
CALLBACK_DATE(since, SEARCH_SINCE, MAIL_SEARCH_DATE_TYPE_RECEIVED)

CALLBACK_DATE(sentbefore, SEARCH_BEFORE, MAIL_SEARCH_DATE_TYPE_SENT)
CALLBACK_DATE(senton, SEARCH_ON, MAIL_SEARCH_DATE_TYPE_SENT)
CALLBACK_DATE(sentsince, SEARCH_SINCE, MAIL_SEARCH_DATE_TYPE_SENT)

CALLBACK_DATE(savedbefore, SEARCH_BEFORE, MAIL_SEARCH_DATE_TYPE_SAVED)
CALLBACK_DATE(savedon, SEARCH_ON, MAIL_SEARCH_DATE_TYPE_SAVED)
CALLBACK_DATE(savedsince, SEARCH_SINCE, MAIL_SEARCH_DATE_TYPE_SAVED)

CALLBACK_DATE(x_savedbefore, SEARCH_BEFORE, MAIL_SEARCH_DATE_TYPE_SAVED)
CALLBACK_DATE(x_savedon, SEARCH_ON, MAIL_SEARCH_DATE_TYPE_SAVED)
CALLBACK_DATE(x_savedsince, SEARCH_SINCE, MAIL_SEARCH_DATE_TYPE_SAVED)

static struct mail_search_arg *
imap_search_savedatesupported(struct mail_search_build_context *ctx)
{
	return mail_search_build_new(ctx, SEARCH_SAVEDATESUPPORTED);
}

static struct mail_search_arg *
arg_new_size(struct mail_search_build_context *ctx,
	     enum mail_search_arg_type type)
{
	struct mail_search_arg *sarg;
	const char *value;

	sarg = mail_search_build_new(ctx, type);
	if (mail_search_parse_string(ctx->parser, &value) < 0)
		return NULL;

	if (str_to_uoff(value, &sarg->value.size) < 0) {
		ctx->_error = "Invalid search size parameter";
		return NULL;
	}
	return sarg;
}

static struct mail_search_arg *
imap_search_larger(struct mail_search_build_context *ctx)
{
	return arg_new_size(ctx, SEARCH_LARGER);
}

static struct mail_search_arg *
imap_search_smaller(struct mail_search_build_context *ctx)
{
	return arg_new_size(ctx, SEARCH_SMALLER);
}

static struct mail_search_arg *
arg_new_header(struct mail_search_build_context *ctx,
	       enum mail_search_arg_type type, const char *hdr_name)
{
	struct mail_search_arg *sarg;
	const char *value;

	sarg = mail_search_build_new(ctx, type);
	if (mail_search_parse_string(ctx->parser, &value) < 0)
		return NULL;

	if (mail_search_build_get_utf8(ctx, value, &sarg->value.str) < 0)
		return NULL;

	sarg->hdr_field_name = p_strdup(ctx->pool, hdr_name);
	return sarg;
}

#define CALLBACK_HDR(_name, _type) \
static struct mail_search_arg *\
imap_search_##_name(struct mail_search_build_context *ctx) \
{ \
	return arg_new_header(ctx, _type, #_name); \
}
CALLBACK_HDR(bcc, SEARCH_HEADER_ADDRESS)
CALLBACK_HDR(cc, SEARCH_HEADER_ADDRESS)
CALLBACK_HDR(from, SEARCH_HEADER_ADDRESS)
CALLBACK_HDR(to, SEARCH_HEADER_ADDRESS)
CALLBACK_HDR(subject, SEARCH_HEADER_COMPRESS_LWSP)

static struct mail_search_arg *
imap_search_header(struct mail_search_build_context *ctx)
{
	const char *hdr_name;

	/* <hdr-name> <string> */
	if (mail_search_parse_string(ctx->parser, &hdr_name) < 0)
		return NULL;
	if (mail_search_build_get_utf8(ctx, hdr_name, &hdr_name) < 0)
		return NULL;

	return arg_new_header(ctx, SEARCH_HEADER, t_str_ucase(hdr_name));
}

static struct mail_search_arg *
arg_new_body(struct mail_search_build_context *ctx,
	     enum mail_search_arg_type type)
{
	struct mail_search_arg *sarg;

	sarg = mail_search_build_str(ctx, type);
	if (sarg == NULL)
		return NULL;

	if (mail_search_build_get_utf8(ctx, sarg->value.str,
				       &sarg->value.str) < 0)
		return NULL;
	return sarg;
}

#define CALLBACK_BODY(_func, _type) \
static struct mail_search_arg *\
imap_search_##_func(struct mail_search_build_context *ctx) \
{ \
	return arg_new_body(ctx, _type); \
}
CALLBACK_BODY(body, SEARCH_BODY)
CALLBACK_BODY(text, SEARCH_TEXT)

static struct mail_search_arg *
arg_new_interval(struct mail_search_build_context *ctx,
		 enum mail_search_arg_type type)
{
	struct mail_search_arg *sarg;
	const char *value;
	uint32_t interval;

	sarg = mail_search_build_new(ctx, type);
	if (mail_search_parse_string(ctx->parser, &value) < 0)
		return NULL;

	if (str_to_uint32(value, &interval) < 0 || interval == 0) {
		ctx->_error = "Invalid search interval parameter";
		return NULL;
	}
	sarg->value.search_flags = MAIL_SEARCH_ARG_FLAG_UTC_TIMES;
	sarg->value.time = ioloop_time - interval;
	sarg->value.date_type = MAIL_SEARCH_DATE_TYPE_RECEIVED;
	return sarg;
}

static struct mail_search_arg *
imap_search_older(struct mail_search_build_context *ctx)
{
	struct mail_search_arg *sarg;

	sarg = arg_new_interval(ctx, SEARCH_BEFORE);
	if (sarg == NULL)
		return NULL;

	/* we need to match also equal, but SEARCH_BEFORE compares with "<" */
	sarg->value.time++;
	return sarg;
}

static struct mail_search_arg *
imap_search_younger(struct mail_search_build_context *ctx)
{
	return arg_new_interval(ctx, SEARCH_SINCE);
}

static int
arg_modseq_set_type(struct mail_search_build_context *ctx,
		    struct mail_search_modseq *modseq, const char *name)
{
	if (strcasecmp(name, "all") == 0)
		modseq->type = MAIL_SEARCH_MODSEQ_TYPE_ANY;
	else if (strcasecmp(name, "priv") == 0)
		modseq->type = MAIL_SEARCH_MODSEQ_TYPE_PRIVATE;
	else if (strcasecmp(name, "shared") == 0)
		modseq->type = MAIL_SEARCH_MODSEQ_TYPE_SHARED;
	else {
		ctx->_error = "Invalid MODSEQ type";
		return -1;
	}
	return 0;
}

static int
arg_modseq_set_ext(struct mail_search_build_context *ctx,
		   struct mail_search_arg *sarg, const char *name)
{
	const char *value;

	name = t_str_lcase(name);
	if (!str_begins(name, "/flags/", &name))
		return 0;

	/* set name */
	if (*name == '\\') {
		/* system flag */
		sarg->value.flags = imap_parse_system_flag(name);
		if (sarg->value.flags == 0 ||
		    sarg->value.flags == MAIL_RECENT) {
			ctx->_error = "Invalid MODSEQ system flag";
			return -1;
		}
	} else {
		sarg->value.str = p_strdup(ctx->pool, name);
	}

	/* set type */
	if (mail_search_parse_string(ctx->parser, &value) < 0)
		return -1;
	if (arg_modseq_set_type(ctx, sarg->value.modseq, value) < 0)
		return -1;
	return 1;
}

static struct mail_search_arg *
imap_search_modseq(struct mail_search_build_context *ctx)
{
	struct mail_search_arg *sarg;
	const char *value;
	int ret;

	/* [<name> <type>] <modseq> */
	sarg = mail_search_build_new(ctx, SEARCH_MODSEQ);
	sarg->value.modseq = p_new(ctx->pool, struct mail_search_modseq, 1);

	if (mail_search_parse_string(ctx->parser, &value) < 0)
		return NULL;

	if ((ret = arg_modseq_set_ext(ctx, sarg, value)) < 0)
		return NULL;
	if (ret > 0) {
		/* extension data used */
		if (mail_search_parse_string(ctx->parser, &value) < 0)
			return NULL;
	}

	if (str_to_uint64(value, &sarg->value.modseq->modseq) < 0) {
		ctx->_error = "Invalid MODSEQ value";
		return NULL;
	}
	return sarg;
}

static struct mail_search_arg *
imap_search_last_result(struct mail_search_build_context *ctx)
{
	struct mail_search_arg *sarg;

	/* SEARCHRES: delay initialization */
	sarg = mail_search_build_new(ctx, SEARCH_UIDSET);
	sarg->value.str = "$";
	p_array_init(&sarg->value.seqset, ctx->pool, 16);
	return sarg;
}

static void mail_search_arg_set_fuzzy(struct mail_search_arg *sarg)
{
	for (; sarg != NULL; sarg = sarg->next) {
		sarg->fuzzy = TRUE;
		switch (sarg->type) {
		case SEARCH_OR:
		case SEARCH_SUB:
		case SEARCH_INTHREAD:
			mail_search_arg_set_fuzzy(sarg->value.subargs);
			break;
		default:
			break;
		}
	}
}

static struct mail_search_arg *
imap_search_fuzzy(struct mail_search_build_context *ctx)
{
	struct mail_search_arg *sarg;

	if (mail_search_build_key(ctx, ctx->parent, &sarg) < 0)
		return NULL;
	i_assert(sarg->next == NULL);

	mail_search_arg_set_fuzzy(sarg);
	return sarg;
}

static struct mail_search_arg *
imap_search_mimepart(struct mail_search_build_context *ctx)
{
	struct mail_search_arg *sarg;

	sarg = mail_search_build_new(ctx, SEARCH_MIMEPART);
	if (mail_search_mime_build(ctx, &sarg->value.mime_part) < 0)
		return NULL;
	return sarg;
}

static struct mail_search_arg *
imap_search_inthread(struct mail_search_build_context *ctx)
{
	struct mail_search_arg *sarg;

	/* <algorithm> <search key> */
	enum mail_thread_type thread_type;
	const char *algorithm;

	if (mail_search_parse_string(ctx->parser, &algorithm) < 0)
		return NULL;
	if (!mail_thread_type_parse(algorithm, &thread_type)) {
		ctx->_error = "Unknown thread algorithm";
		return NULL;
	}

	sarg = mail_search_build_new(ctx, SEARCH_INTHREAD);
	sarg->value.thread_type = thread_type;
	if (mail_search_build_key(ctx, sarg, &sarg->value.subargs) < 0)
		return NULL;
	return sarg;
}

CALLBACK_STR(x_guid, SEARCH_GUID)

static struct mail_search_arg *
imap_search_x_mailbox(struct mail_search_build_context *ctx)
{
	struct mail_search_arg *sarg;
	string_t *utf8_name;

	sarg = mail_search_build_str(ctx, SEARCH_MAILBOX_GLOB);
	if (sarg == NULL)
		return NULL;

	utf8_name = t_str_new(strlen(sarg->value.str));
	if (imap_utf7_to_utf8(sarg->value.str, utf8_name) < 0) {
		ctx->_error = "X-MAILBOX name not mUTF-7";
		return NULL;
	}
	sarg->value.str = p_strdup(ctx->pool, str_c(utf8_name));
	return sarg;
}

static struct mail_search_arg *
imap_search_x_real_uid(struct mail_search_build_context *ctx)
{
	struct mail_search_arg *sarg;

	/* <message set> */
	sarg = mail_search_build_str(ctx, SEARCH_REAL_UID);
	if (sarg == NULL)
		return NULL;

	p_array_init(&sarg->value.seqset, ctx->pool, 16);
	if (imap_seq_set_parse(sarg->value.str,
			       &sarg->value.seqset) < 0) {
		ctx->_error = "Invalid X-REAL-UID messageset";
		return NULL;
	}
	return sarg;
}

static const struct mail_search_register_arg imap_register_args[] = {
	/* argument set operations */
	{ "NOT", imap_search_not, 0 },
	{ "OR", imap_search_or, 0 },

	/* message sets */
	{ "ALL", imap_search_all, 0 },
	{ "UID", imap_search_uid, 0 },

	/* flags */
	{ "ANSWERED", imap_search_answered, 0 },
	{ "UNANSWERED", imap_search_unanswered, 0 },
	{ "DELETED", imap_search_deleted, 0 },
	{ "UNDELETED", imap_search_undeleted, 0 },
	{ "DRAFT", imap_search_draft, 0 },
	{ "UNDRAFT", imap_search_undraft, 0 },
	{ "FLAGGED", imap_search_flagged, 0 },
	{ "UNFLAGGED", imap_search_unflagged, 0 },
	{ "SEEN", imap_search_seen, 0 },
	{ "UNSEEN", imap_search_unseen, 0 },
	{ "RECENT", imap_search_recent, MAIL_SEARCH_REGISTER_IMAP4REV1 },
	{ "OLD", imap_search_old, 0 },
	{ "NEW", imap_search_new, MAIL_SEARCH_REGISTER_IMAP4REV1 },

	/* keywords */
	{ "KEYWORD", imap_search_keyword, 0 },
	{ "UNKEYWORD", imap_search_unkeyword, 0 },

	/* dates */
	{ "BEFORE", imap_search_before, 0 },
	{ "ON", imap_search_on, 0 },
	{ "SINCE", imap_search_since, 0 },
	{ "SENTBEFORE", imap_search_sentbefore, 0 },
	{ "SENTON", imap_search_senton, 0 },
	{ "SENTSINCE", imap_search_sentsince, 0 },
	{ "SAVEDBEFORE", imap_search_savedbefore, 0 },
	{ "SAVEDON", imap_search_savedon, 0 },
	{ "SAVEDSINCE", imap_search_savedsince, 0 },
	{ "SAVEDATESUPPORTED", imap_search_savedatesupported, 0 },
	/* FIXME: remove these in v2.4: */
	{ "X-SAVEDBEFORE", imap_search_x_savedbefore, 0 },
	{ "X-SAVEDON", imap_search_x_savedon, 0 },
	{ "X-SAVEDSINCE", imap_search_x_savedsince, 0 },

	/* sizes */
	{ "LARGER", imap_search_larger, 0 },
	{ "SMALLER", imap_search_smaller, 0 },

	/* headers */
	{ "BCC", imap_search_bcc, 0 },
	{ "CC", imap_search_cc, 0 },
	{ "FROM", imap_search_from, 0 },
	{ "TO", imap_search_to, 0 },
	{ "SUBJECT", imap_search_subject, 0 },
	{ "HEADER", imap_search_header, 0 },

	/* body */
	{ "BODY", imap_search_body, 0 },
	{ "TEXT", imap_search_text, 0 },

	/* WITHIN extension: */
	{ "OLDER", imap_search_older, 0 },
	{ "YOUNGER", imap_search_younger, 0 },

	/* CONDSTORE extension: */
	{ "MODSEQ", imap_search_modseq, 0 },

	/* SEARCHRES extension: */
	{ "$", imap_search_last_result, 0 },

	/* FUZZY extension: */
	{ "FUZZY", imap_search_fuzzy, 0 },

	/* SEARCH=MIMEPART extension: */
	{ "MIMEPART", imap_search_mimepart, 0 },

	/* Other Dovecot extensions: */
	{ "INTHREAD", imap_search_inthread, 0 },
	{ "X-GUID", imap_search_x_guid, 0 },
	{ "X-MAILBOX", imap_search_x_mailbox, 0 },
	{ "X-REAL-UID", imap_search_x_real_uid, 0 }
};

static struct mail_search_register
*mail_search_register_init_imap(enum mail_search_register_arg_flags flags)
{
	struct mail_search_register *reg;

	reg = mail_search_register_init();
	for (unsigned int i = 0; i < N_ELEMENTS(imap_register_args); i++) {
		if (HAS_ALL_BITS(flags, imap_register_args[i].flags))
			mail_search_register_add(reg, &imap_register_args[i], 1);
	}
	mail_search_register_fallback(reg, imap_search_fallback);
	return reg;
}

static struct mail_search_register *mail_search_register_init_imap4rev2(void)
{
	return mail_search_register_init_imap(0);
}

static struct mail_search_register *mail_search_register_init_imap4rev1(void)
{
	return mail_search_register_init_imap(MAIL_SEARCH_REGISTER_IMAP4REV1);
}

struct mail_search_register *
mail_search_register_get_imap4rev2(void)
{
	if (mail_search_register_imap4rev2 == NULL)
		mail_search_register_imap4rev2 = mail_search_register_init_imap4rev2();
	return mail_search_register_imap4rev2;
}

struct mail_search_register *
mail_search_register_get_imap4rev1(void)
{
	if (mail_search_register_imap4rev1 == NULL)
		mail_search_register_imap4rev1 = mail_search_register_init_imap4rev1();
	return mail_search_register_imap4rev1;
}
