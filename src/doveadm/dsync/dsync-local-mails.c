/* Copyright (c) Dovecot authors, see top-level COPYING file */

#include "lib.h"
#include "array.h"
#include "hash.h"
#include "mail-namespace.h"
#include "mail-storage-private.h"
#include "mail-search-build.h"
#include "mailbox-list-iter.h"
#include "dsync-local-mails.h"

/* Scan the other mailboxes only if at least this many mails, or this much
   mail, would otherwise be transferred. Scanning a large account takes a
   few hundred milliseconds, which isn't worth it for a single new mail. */
#define DSYNC_LOCAL_SCAN_MIN_MAILS 8
#define DSYNC_LOCAL_SCAN_MIN_BYTES (512*1024)

struct dsync_local_mail {
	const char *vname;
	uint32_t uid;
	const char *guid;
};

struct dsync_local_box {
	const char *vname;
	struct mailbox *box;
	struct mailbox_transaction_context *trans;
	struct mail *mail;
};

struct dsync_local_mails {
	pool_t pool;
	struct mail_user *user;
	struct event *event;
	enum mailbox_transaction_flags transaction_flags;

	/* deferred expunges, in the order they were added */
	ARRAY(struct dsync_local_mail *) expunges;
	/* GUID -> local mail, from the deferred expunges and scans */
	HASH_TABLE(const char *, struct dsync_local_mail *) guids;
	/* mailboxes opened by dsync_local_mails_find() */
	ARRAY(struct dsync_local_box) boxes;
	/* GUIDs dsync_local_mails_scan() is still looking for */
	HASH_TABLE(const char *, const char *) scan_wanted;

	unsigned int found_count, scan_count;
};

struct dsync_local_mails *
dsync_local_mails_init(struct mail_user *user,
		       enum mailbox_transaction_flags transaction_flags,
		       struct event *event_parent)
{
	struct dsync_local_mails *lm;
	pool_t pool;

	pool = pool_alloconly_create("dsync local mails", 4096);
	lm = p_new(pool, struct dsync_local_mails, 1);
	lm->pool = pool;
	lm->user = user;
	lm->event = event_create(event_parent);
	lm->transaction_flags = transaction_flags;
	p_array_init(&lm->expunges, pool, 64);
	p_array_init(&lm->boxes, pool, 4);
	hash_table_create(&lm->guids, pool, 0, str_hash, strcmp);
	return lm;
}

/* Returns 1 if the mailbox was opened, 0 if it doesn't exist and -1 on
   error. */
static int
dsync_local_mails_box_open(struct dsync_local_mails *lm, const char *vname,
			   struct mailbox **box_r, const char **error_r)
{
	struct mail_namespace *ns;
	struct mailbox *box;
	enum mailbox_existence existence;
	int ret;

	ns = mail_namespace_find(lm->user->namespaces, vname);
	box = mailbox_alloc(ns->list, vname, 0);
	/* Opening an auto=create mailbox that doesn't exist would create it,
	   in the middle of the session and with a new GUID. */
	if (mailbox_exists(box, FALSE, &existence) < 0)
		ret = -1;
	else if (existence != MAILBOX_EXISTENCE_SELECT)
		ret = 0;
	else if (mailbox_open(box) < 0 || mailbox_sync(box, 0) < 0)
		ret = -1;
	else {
		*box_r = box;
		return 1;
	}
	if (ret < 0) {
		*error_r = t_strdup(mailbox_get_last_internal_error(box,
								    NULL));
	}
	mailbox_free(&box);
	return ret;
}

static int
dsync_local_mails_expunge_box(struct dsync_local_mails *lm,
			      struct dsync_local_mail *const *mails,
			      unsigned int count)
{
	struct mailbox_transaction_context *trans;
	struct mailbox *box;
	struct mail *mail;
	const char *guid, *error;
	unsigned int i;
	int ret;

	if ((ret = dsync_local_mails_box_open(lm, mails[0]->vname,
					      &box, &error)) <= 0) {
		if (ret == 0) {
			/* deleted (or renamed) meanwhile */
			return 0;
		}
		e_error(lm->event, "Mailbox %s: Couldn't open for expunging: %s",
			mails[0]->vname, error);
		return -1;
	}
	trans = mailbox_transaction_begin(box, lm->transaction_flags,
					  __func__);
	mail = mail_alloc(trans, MAIL_FETCH_GUID, NULL);
	for (i = 0; i < count; i++) {
		/* the mail may have been expunged or renumbered meanwhile */
		if (mail_set_uid(mail, mails[i]->uid) &&
		    mail_get_special(mail, MAIL_FETCH_GUID, &guid) == 0 &&
		    strcmp(guid, mails[i]->guid) == 0)
			mail_expunge(mail);
	}
	mail_free(&mail);
	/* Sync right away, like the importer does after its commit, so that
	   the expunges are done by us and not left pending in the
	   transaction log for whichever process syncs the mailbox next. */
	if (mailbox_transaction_commit(&trans) < 0 ||
	    mailbox_sync(box, 0) < 0) {
		e_error(lm->event, "Mailbox %s: Expunging failed: %s",
			mailbox_get_vname(box),
			mailbox_get_last_internal_error(box, NULL));
		ret = -1;
	} else {
		ret = 0;
	}
	mailbox_free(&box);
	return ret;
}

int dsync_local_mails_finish(struct dsync_local_mails **_lm)
{
	struct dsync_local_mails *lm = *_lm;
	struct dsync_local_mail *const *mails;
	unsigned int i, j, count;
	int ret = 0;

	*_lm = NULL;
	dsync_local_mails_release(lm);

	/* expunges were added one mailbox at a time */
	mails = array_get(&lm->expunges, &count);
	for (i = 0; i < count; i = j) {
		for (j = i + 1; j < count; j++) {
			if (strcmp(mails[j]->vname, mails[i]->vname) != 0)
				break;
		}
		T_BEGIN {
			if (dsync_local_mails_expunge_box(lm, mails + i,
							  j - i) < 0)
				ret = -1;
		} T_END;
	}
	if (lm->found_count > 0 || count > 0) {
		e_debug(lm->event, "Copied %u mails from other local mailboxes "
			"instead of transferring them (%u scans), "
			"applied %u deferred expunges",
			lm->found_count, lm->scan_count, count);
	}
	hash_table_destroy(&lm->guids);
	event_unref(&lm->event);
	pool_unref(&lm->pool);
	return ret;
}

void dsync_local_mails_add_expunge(struct dsync_local_mails *lm,
				   const char *vname, uint32_t uid,
				   const char *guid)
{
	struct dsync_local_mail *mail;

	mail = p_new(lm->pool, struct dsync_local_mail, 1);
	mail->vname = p_strdup(lm->pool, vname);
	mail->uid = uid;
	mail->guid = p_strdup(lm->pool, guid);
	array_push_back(&lm->expunges, &mail);
	if (hash_table_lookup(lm->guids, mail->guid) == NULL)
		hash_table_insert(lm->guids, mail->guid, mail);
}

bool dsync_local_mails_want_scan(unsigned int missing_count,
				 uoff_t missing_bytes)
{
	return missing_count >= DSYNC_LOCAL_SCAN_MIN_MAILS ||
		missing_bytes >= DSYNC_LOCAL_SCAN_MIN_BYTES;
}

/* Returns the number of wanted GUIDs found in the mailbox. */
static unsigned int
dsync_local_mails_scan_box(struct dsync_local_mails *lm, const char *vname)
{
	struct mailbox *box;
	struct mailbox_transaction_context *trans;
	struct mail_search_args *search_args;
	struct mail_search_context *search_ctx;
	struct dsync_local_mail *lmail;
	struct mail *mail;
	const char *guid, *key, *error;
	unsigned int found = 0;
	int ret;

	if ((ret = dsync_local_mails_box_open(lm, vname, &box, &error)) <= 0) {
		if (ret < 0) {
			e_debug(lm->event, "Mailbox %s: Couldn't open: %s",
				vname, error);
		}
		return 0;
	}
	if (strcmp(box->storage->name, "virtual") == 0) {
		/* its mails are the other mailboxes' mails */
		mailbox_free(&box);
		return 0;
	}
	trans = mailbox_transaction_begin(box, 0, __func__);
	search_args = mail_search_build_init();
	mail_search_build_add_all(search_args);
	search_ctx = mailbox_search_init(trans, search_args, NULL,
					 MAIL_FETCH_GUID, NULL);
	mail_search_args_unref(&search_args);
	while (mailbox_search_next(search_ctx, &mail)) {
		if (mail_get_special(mail, MAIL_FETCH_GUID, &guid) < 0)
			continue;
		key = hash_table_lookup(lm->scan_wanted, guid);
		if (key == NULL)
			continue;
		lmail = p_new(lm->pool, struct dsync_local_mail, 1);
		lmail->vname = p_strdup(lm->pool, vname);
		lmail->uid = mail->uid;
		lmail->guid = p_strdup(lm->pool, guid);
		hash_table_insert(lm->guids, lmail->guid, lmail);
		hash_table_remove(lm->scan_wanted, key);
		found++;
		if (hash_table_count(lm->scan_wanted) == 0)
			break;
	}
	if (mailbox_search_deinit(&search_ctx) < 0) {
		e_debug(lm->event, "Mailbox %s: Search failed: %s", vname,
			mailbox_get_last_internal_error(box, NULL));
	}
	(void)mailbox_transaction_commit(&trans);
	mailbox_free(&box);
	return found;
}

void dsync_local_mails_scan(struct dsync_local_mails *lm,
			    struct mailbox *exclude_box,
			    const ARRAY_TYPE(const_string) *guids)
{
	static const char *const patterns[] = { "*", NULL };
	struct mailbox_list_iterate_context *iter;
	const struct mailbox_info *info;
	const char *guid, *exclude_vname = mailbox_get_vname(exclude_box);
	unsigned int found = 0;

	hash_table_create(&lm->scan_wanted, default_pool, 0, str_hash, strcmp);
	array_foreach_elem(guids, guid) {
		if (hash_table_lookup(lm->guids, guid) == NULL)
			hash_table_update(lm->scan_wanted, guid, guid);
	}
	if (hash_table_count(lm->scan_wanted) == 0) {
		hash_table_destroy(&lm->scan_wanted);
		return;
	}
	lm->scan_count++;
	e_debug(lm->event, "Looking for %u mails in the other mailboxes",
		hash_table_count(lm->scan_wanted));

	iter = mailbox_list_iter_init_namespaces(lm->user->namespaces,
		patterns, MAIL_NAMESPACE_TYPE_PRIVATE,
		MAILBOX_LIST_ITER_SKIP_ALIASES |
		MAILBOX_LIST_ITER_NO_AUTO_BOXES |
		MAILBOX_LIST_ITER_RETURN_NO_FLAGS);
	while (hash_table_count(lm->scan_wanted) > 0 &&
	       (info = mailbox_list_iter_next(iter)) != NULL) {
		if (strcmp(info->vname, exclude_vname) == 0)
			continue;
		T_BEGIN {
			found += dsync_local_mails_scan_box(lm, info->vname);
		} T_END;
	}
	if (mailbox_list_iter_deinit(&iter) < 0) {
		e_debug(lm->event, "Listing mailboxes failed: %s",
			mailbox_list_get_last_internal_error(
				lm->user->namespaces->list, NULL));
	}
	e_debug(lm->event, "Found %u of them, %u not", found,
		hash_table_count(lm->scan_wanted));
	hash_table_destroy(&lm->scan_wanted);
}

static struct dsync_local_box *
dsync_local_mails_get_box(struct dsync_local_mails *lm, const char *vname)
{
	struct dsync_local_box *lbox;
	struct mailbox *box;
	const char *error;
	int ret;

	array_foreach_modifiable(&lm->boxes, lbox) {
		if (strcmp(lbox->vname, vname) == 0)
			return lbox;
	}
	if ((ret = dsync_local_mails_box_open(lm, vname, &box, &error)) <= 0) {
		if (ret < 0) {
			e_debug(lm->event, "Mailbox %s: Couldn't open: %s",
				vname, error);
		}
		return NULL;
	}
	lbox = array_append_space(&lm->boxes);
	lbox->vname = p_strdup(lm->pool, vname);
	lbox->box = box;
	lbox->trans = mailbox_transaction_begin(box, 0, __func__);
	lbox->mail = mail_alloc(lbox->trans, MAIL_FETCH_GUID, NULL);
	return lbox;
}

struct mail *dsync_local_mails_find(struct dsync_local_mails *lm,
				    const char *guid,
				    struct mailbox *exclude_box)
{
	struct dsync_local_mail *lmail;
	struct dsync_local_box *lbox;
	const char *mail_guid;

	lmail = hash_table_lookup(lm->guids, guid);
	if (lmail == NULL ||
	    strcmp(lmail->vname, mailbox_get_vname(exclude_box)) == 0)
		return NULL;
	lbox = dsync_local_mails_get_box(lm, lmail->vname);
	if (lbox == NULL)
		return NULL;
	/* the mail may have been expunged or renumbered since */
	if (!mail_set_uid(lbox->mail, lmail->uid) ||
	    mail_get_special(lbox->mail, MAIL_FETCH_GUID, &mail_guid) < 0 ||
	    strcmp(mail_guid, guid) != 0)
		return NULL;
	lm->found_count++;
	return lbox->mail;
}

void dsync_local_mails_release(struct dsync_local_mails *lm)
{
	struct dsync_local_box *lbox;

	array_foreach_modifiable(&lm->boxes, lbox) {
		mail_free(&lbox->mail);
		(void)mailbox_transaction_commit(&lbox->trans);
		mailbox_free(&lbox->box);
	}
	array_clear(&lm->boxes);
}
