#ifndef __MBOX_SYNC_PRIVATE_H
#define __MBOX_SYNC_PRIVATE_H

#include "md5.h"
#include "mail-index.h"

enum mbox_sync_flags {
	MBOX_SYNC_LAST_COMMIT	= 0x01,
	MBOX_SYNC_HEADER	= 0x02,
	MBOX_SYNC_LOCK_READING	= 0x04,
	MBOX_SYNC_UNDIRTY	= 0x08,
	MBOX_SYNC_REWRITE	= 0x10,
	MBOX_SYNC_FORCE_SYNC	= 0x20
};

struct mbox_flag_type {
	char chr;
	enum mail_flags flag;
};

enum header_position {
	MBOX_HDR_STATUS,
	MBOX_HDR_X_IMAPBASE,
	MBOX_HDR_X_KEYWORDS,
	MBOX_HDR_X_STATUS,
	MBOX_HDR_X_UID,

        MBOX_HDR_COUNT
};

/* kludgy. swap MAIL_RECENT with MBOX_NONRECENT_KLUDGE when writing Status
   header, because 'O' flag means non-recent but internally we want to use
   recent flag. */
#define MBOX_NONRECENT_KLUDGE MAIL_RECENT
#define MBOX_EXPUNGED 0x40

#define STATUS_FLAGS_MASK (MAIL_SEEN|MBOX_NONRECENT_KLUDGE)
#define XSTATUS_FLAGS_MASK (MAIL_ANSWERED|MAIL_FLAGGED|MAIL_DRAFT|MAIL_DELETED)
extern struct mbox_flag_type mbox_status_flags[];
extern struct mbox_flag_type mbox_xstatus_flags[];

struct mbox_sync_mail {
	uint32_t uid;
	uint32_t idx_seq;
	uint8_t flags;
	array_t ARRAY_DEFINE(keywords, unsigned int);

	uoff_t from_offset;
	uoff_t body_size;

	/* following variables have a bit overloaded functionality:

	   a) space <= 0 : offset points to beginning of headers. space is the
	      amount of space missing that is required to be able to rewrite
	      the headers
	   b) space > 0 : offset points to beginning of whitespace that can
	      be removed. space is the amount of data that can be removed from
	      there. note that the message may contain more whitespace
	      elsewhere. */
	uoff_t offset;
	off_t space;
};

struct mbox_sync_mail_context {
	struct mbox_sync_context *sync_ctx;
	struct mbox_sync_mail mail;

	uint32_t seq;
	uoff_t hdr_offset, body_offset;

	size_t header_first_change, header_last_change;
	string_t *header;

	unsigned char hdr_md5_sum[16];

	uoff_t content_length;

	size_t hdr_pos[MBOX_HDR_COUNT];
	uint32_t parsed_uid;
	unsigned int last_uid_value_start_pos;

	unsigned int have_eoh:1;
	unsigned int need_rewrite:1;
	unsigned int seen_imapbase:1;
	unsigned int pseudo:1;
	unsigned int updated:1;
	unsigned int recent:1;
	unsigned int dirty:1;
	unsigned int uid_broken:1;
	unsigned int imapbase_rewrite:1;
	unsigned int imapbase_updated:1;
};

struct mbox_sync_context {
	struct mbox_mailbox *mbox;
        enum mbox_sync_flags flags;
	struct istream *input, *file_input;
	int write_fd;

	struct mail_index_sync_ctx *index_sync_ctx;
	struct mail_index_view *sync_view;
	struct mail_index_transaction *t;
	const struct mail_index_header *hdr;

	string_t *header, *from_line;

	/* header state: */
	uint32_t base_uid_validity, base_uid_last;
	uoff_t base_uid_last_offset;

	/* mail state: */
	array_t ARRAY_DEFINE(mails, struct mbox_sync_mail);
	array_t ARRAY_DEFINE(syncs, struct mail_index_sync_rec);
	struct mail_index_sync_rec sync_rec;

	pool_t mail_keyword_pool;

	uint32_t prev_msg_uid, next_uid, idx_next_uid;
	uint32_t seq, idx_seq, need_space_seq;
	off_t expunged_space, space_diff;

	unsigned int dest_first_mail:1;

	/* global flags: */
	unsigned int delay_writes:1;
};

int mbox_sync(struct mbox_mailbox *mbox, enum mbox_sync_flags flags);
int mbox_sync_has_changed(struct mbox_mailbox *mbox, int leave_dirty);

void mbox_sync_parse_next_mail(struct istream *input,
			       struct mbox_sync_mail_context *ctx);
int mbox_sync_parse_match_mail(struct mbox_mailbox *mbox,
			       struct mail_index_view *view, uint32_t seq);

void mbox_sync_update_header(struct mbox_sync_mail_context *ctx);
void mbox_sync_update_header_from(struct mbox_sync_mail_context *ctx,
				  const struct mbox_sync_mail *mail);
int mbox_sync_try_rewrite(struct mbox_sync_mail_context *ctx, off_t move_diff);
int mbox_sync_rewrite(struct mbox_sync_context *sync_ctx,
		      uoff_t end_offset, off_t move_diff, uoff_t extra_space,
		      uint32_t first_seq, uint32_t last_seq);

void mbox_sync_apply_index_syncs(struct mbox_sync_context *sync_ctx,
				 struct mbox_sync_mail *mail,
				 int *keywords_changed_r);
int mbox_sync_seek(struct mbox_sync_context *sync_ctx, uoff_t from_offset);
int mbox_move(struct mbox_sync_context *sync_ctx,
	      uoff_t dest, uoff_t source, uoff_t size);
void mbox_sync_move_buffer(struct mbox_sync_mail_context *ctx,
			   size_t pos, size_t need, size_t have);
void mbox_sync_headers_add_space(struct mbox_sync_mail_context *ctx,
				 size_t size);

#endif
