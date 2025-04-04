/* Copyright (c) 2004-2018 Dovecot authors, see the included COPYING file */

/*
   Here's a description of how we handle Maildir synchronization and
   it's problems:

   We want to be as efficient as we can. The most efficient way to
   check if changes have occurred is to stat() the new/ and cur/
   directories and uidlist file - if their mtimes haven't changed,
   there's no changes and we don't need to do anything.

   Problem 1: Multiple changes can happen within a single second -
   nothing guarantees that once we synced it, someone else didn't just
   then make a modification. Such modifications wouldn't get noticed
   until a new modification occurred later.

   Problem 2: Syncing cur/ directory is much more costly than syncing
   new/. Moving mails from new/ to cur/ will always change mtime of
   cur/ causing us to sync it as well.

   Problem 3: We may not be able to move mail from new/ to cur/
   because we're out of quota, or simply because we're accessing a
   read-only mailbox.


   MAILDIR_SYNC_SECS
   -----------------

   Several checks below use MAILDIR_SYNC_SECS, which should be maximum
   clock drift between all computers accessing the maildir (eg. via
   NFS), rounded up to next second. Our default is 1 second, since
   everyone should be using NTP.

   Note that setting it to 0 works only if there's only one computer
   accessing the maildir. It's practically impossible to make two
   clocks _exactly_ synchronized.

   It might be possible to only use file server's clock by looking at
   the atime field, but I don't know how well that would actually work.

   cur directory
   -------------

   We have dirty_cur_time variable which is set to cur/ directory's
   mtime when it's >= time() - MAILDIR_SYNC_SECS and we _think_ we have
   synchronized the directory.

   When dirty_cur_time is non-zero, we don't synchronize the cur/
   directory until

      a) cur/'s mtime changes
      b) opening a mail fails with ENOENT
      c) time() > dirty_cur_time + MAILDIR_SYNC_SECS

   This allows us to modify the maildir multiple times without having
   to sync it at every change. The sync will eventually be done to
   make sure we didn't miss any external changes.

   The dirty_cur_time is set when:

      - we change message flags
      - we expunge messages
      - we move mail from new/ to cur/
      - we sync cur/ directory and it's mtime is >= time() - MAILDIR_SYNC_SECS

   It's unset when we do the final syncing, ie. when mtime is
   older than time() - MAILDIR_SYNC_SECS.

   new directory
   -------------

   If new/'s mtime is >= time() - MAILDIR_SYNC_SECS, always synchronize
   it. dirty_cur_time-like feature might save us a few syncs, but
   that might break a client which saves a mail in one connection and
   tries to fetch it in another one. new/ directory is almost always
   empty, so syncing it should be very fast anyway. Actually this can
   still happen if we sync only new/ dir while another client is also
   moving mails from it to cur/ - it takes us a while to see them.
   That's pretty unlikely to happen however, and only way to fix it
   would be to always synchronize cur/ after new/.

   Normally we move all mails from new/ to cur/ whenever we sync it. If
   it's not possible for some reason, we mark the mail with "probably
   exists in new/ directory" flag.

   If rename() still fails because of ENOSPC or EDQUOT, we still save
   the flag changes in index with dirty-flag on. When moving the mail
   to cur/ directory, or when we notice it's already moved there, we
   apply the flag changes to the filename, rename it and remove the
   dirty flag. If there's dirty flags, this should be tried every time
   after expunge or when closing the mailbox.

   uidlist
   -------

   This file contains UID <-> filename mappings. It's updated only when
   new mail arrives, so it may contain filenames that have already been
   deleted. Updating is done by getting uidlist.lock file, writing the
   whole uidlist into it and rename()ing it over the old uidlist. This
   means there's no need to lock the file for reading.

   Whenever uidlist is rewritten, it's mtime must be larger than the old
   one's. Use utime() before rename() if needed. Note that inode checking
   wouldn't have been sufficient as inode numbers can be reused.

   This file is usually read the first time you need to know filename for
   given UID. After that it's not re-read unless new mails come that we
   don't know about.

   broken clients
   --------------

   Originally the middle identifier in Maildir filename was specified
   only as <process id>_<delivery counter>. That however created a
   problem with randomized PIDs which made it possible that the same
   PID was reused within one second.

   So if within one second a mail was delivered, MUA moved it to cur/
   and another mail was delivered by a new process using same PID as
   the first one, we likely ended up overwriting the first mail when
   the second mail was moved over it.

   Nowadays everyone should be giving a bit more specific identifier,
   for example include microseconds in it which Dovecot does.

   There's a simple way to prevent this from happening in some cases:
   Don't move the mail from new/ to cur/ if it's mtime is >= time() -
   MAILDIR_SYNC_SECS. The second delivery's link() call then fails
   because the file is already in new/, and it will then use a
   different filename. There's a few problems with this however:

      - it requires extra stat() call which is unneeded extra I/O
      - another MUA might still move the mail to cur/
      - if first file's flags are modified by either Dovecot or another
        MUA, it's moved to cur/ (you _could_ just do the dirty-flagging
	but that'd be ugly)

   Because this is useful only for very few people and it requires
   extra I/O, I decided not to implement this. It should be however
   quite easy to do since we need to be able to deal with files in new/
   in any case.

   It's also possible to never accidentally overwrite a mail by using
   link() + unlink() rather than rename(). This however isn't very
   good idea as it introduces potential race conditions when multiple
   clients are accessing the mailbox:

   Trying to move the same mail from new/ to cur/ at the same time:

      a) Client 1 uses slightly different filename than client 2,
         for example one sets read-flag on but the other doesn't.
	 You have the same mail duplicated now.

      b) Client 3 sees the mail between Client 1's and 2's link() calls
         and changes it's flag. You have the same mail duplicated now.

   And it gets worse when they're unlink()ing in cur/ directory:

      c) Client 1 changes mails's flag and client 2 changes it back
         between 1's link() and unlink(). The mail is now expunged.

      d) If you try to deal with the duplicates by unlink()ing another
         one of them, you might end up unlinking both of them.

   So, what should we do then if we notice a duplicate? First of all,
   it might not be a duplicate at all, readdir() might have just
   returned it twice because it was just renamed. What we should do is
   create a completely new base name for it and rename() it to that.
   If the call fails with ENOENT, it only means that it wasn't a
   duplicate after all.
*/

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "buffer.h"
#include "hash.h"
#include "str.h"
#include "eacces-error.h"
#include "nfs-workarounds.h"
#include "maildir-storage.h"
#include "maildir-uidlist.h"
#include "maildir-filename.h"
#include "maildir-sync.h"

#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

/* When rename()ing many files from new/ to cur/, it's possible that next
   readdir() skips some files. we don't of course wish to lose them, so we
   go and rescan the new/ directory again from beginning until no files are
   left. This value is just an optimization to avoid checking the directory
   twice needlessly. usually only NFS is the problem case. 1 is the safest
   bet here, but I guess 5 will do just fine too. */
#define MAILDIR_RENAME_RESCAN_COUNT 5

/* This is mostly to avoid infinite looping when rename() destination already
   exists as the hard link of the file itself. */
#define MAILDIR_SCAN_DIR_MAX_COUNT 5

#define DUPE_LINKS_DELETE_SECS 30

enum maildir_scan_why {
	WHY_FORCED	= 0x01,
	WHY_FIRSTSYNC	= 0x02,
	WHY_NEWCHANGED	= 0x04,
	WHY_CURCHANGED	= 0x08,
	WHY_DROPRECENT	= 0x10,
	WHY_FINDRECENT	= 0x20,
	WHY_DELAYEDNEW	= 0x40,
	WHY_DELAYEDCUR	= 0x80
};

struct maildir_sync_context {
        struct maildir_mailbox *mbox;
	const char *cur_dir;

	enum mailbox_sync_flags flags;
	time_t last_touch, last_notify;

	struct maildir_uidlist_sync_ctx *uidlist_sync_ctx;
	struct maildir_index_sync_context *index_sync_ctx;

	bool partial:1;
	bool locked:1;
	bool racing:1;
};

void maildir_sync_set_racing(struct maildir_sync_context *ctx)
{
	ctx->racing = TRUE;
}

void maildir_sync_notify(struct maildir_sync_context *ctx)
{
	time_t now;

	if (ctx == NULL) {
		/* we got here from maildir-save.c. it has no
		   maildir_sync_context,  */
		return;
	}

	now = time(NULL);
	if (now - ctx->last_touch > MAILDIR_LOCK_TOUCH_SECS && ctx->locked) {
		(void)maildir_uidlist_lock_touch(ctx->mbox->uidlist);
		ctx->last_touch = now;
	}
	if (now - ctx->last_notify > MAIL_STORAGE_NOTIFY_INTERVAL_SECS) {
		struct mailbox *box = &ctx->mbox->box;

		if (box->storage->callbacks.notify_progress != NULL) T_BEGIN {
			struct mail_storage_progress_details dtl = {};
			box->storage->callbacks.notify_progress(
				box, &dtl, box->storage->callback_context);
		} T_END;
		ctx->last_notify = now;
	}
}

static struct maildir_sync_context *
maildir_sync_context_new(struct maildir_mailbox *mbox,
			 enum mailbox_sync_flags flags)
{
        struct maildir_sync_context *ctx;

	ctx = t_new(struct maildir_sync_context, 1);
	ctx->mbox = mbox;
	ctx->cur_dir = t_strconcat(mailbox_get_path(&mbox->box), "/cur", NULL);
	ctx->last_touch = ioloop_time;
	ctx->last_notify = ioloop_time;
	ctx->flags = flags;
	return ctx;
}

static void maildir_sync_deinit(struct maildir_sync_context *ctx)
{
	if (ctx->uidlist_sync_ctx != NULL)
		(void)maildir_uidlist_sync_deinit(&ctx->uidlist_sync_ctx, FALSE);
	if (ctx->index_sync_ctx != NULL)
		maildir_sync_index_rollback(&ctx->index_sync_ctx);
	if (ctx->mbox->storage->storage.rebuild_list_index)
		(void)mail_storage_list_index_rebuild_and_set_uncorrupted(&ctx->mbox->storage->storage);
}

static int maildir_fix_duplicate(struct maildir_sync_context *ctx,
				 const char *dir, const char *fname2)
{
	struct event *event = ctx->mbox->box.event;
	const char *fname1, *path1, *path2;
	const char *new_fname;
	struct stat st1, st2;
	uoff_t size;

	fname1 = maildir_uidlist_sync_get_full_filename(ctx->uidlist_sync_ctx,
							fname2);
	i_assert(fname1 != NULL);

	path1 = t_strconcat(dir, "/", fname1, NULL);
	path2 = t_strconcat(dir, "/", fname2, NULL);

	if (stat(path1, &st1) < 0 || stat(path2, &st2) < 0) {
		/* most likely the files just don't exist anymore.
		   don't really care about other errors much. */
		return 0;
	}
	if (st1.st_ino == st2.st_ino &&
	    CMP_DEV_T(st1.st_dev, st2.st_dev)) {
		/* Files are the same. this means either a race condition
		   between stat() calls, or that the files were link()ed. */
		if (st1.st_nlink > 1 && st2.st_nlink == st1.st_nlink &&
		    st1.st_ctime == st2.st_ctime &&
		    st1.st_ctime < ioloop_time - DUPE_LINKS_DELETE_SECS) {
			/* The file has hard links and it hasn't had any
			   changes (such as renames) for a while, so this
			   isn't a race condition.

			   rename()ing one file on top of the other would fix
			   this safely, except POSIX decided that rename()
			   doesn't work that way. So we'll have unlink() one
			   and hope that another process didn't just decide to
			   unlink() the other (uidlist lock prevents this from
			   happening) */
			if (i_unlink(path2) == 0)
				e_warning(event, "Unlinked a duplicate: %s", path2);
		}
		return 0;
	}

	new_fname = maildir_filename_generate();
	/* preserve S= and W= sizes if they're available.
	   (S=size is required for mail-compress plugin to work) */
	if (maildir_filename_get_size(fname2, MAILDIR_EXTRA_FILE_SIZE, &size)) {
		new_fname = t_strdup_printf("%s,%c=%"PRIuUOFF_T,
			new_fname, MAILDIR_EXTRA_FILE_SIZE, size);
	}
	if (maildir_filename_get_size(fname2, MAILDIR_EXTRA_VIRTUAL_SIZE, &size)) {
		new_fname = t_strdup_printf("%s,%c=%"PRIuUOFF_T,
			new_fname, MAILDIR_EXTRA_VIRTUAL_SIZE, size);
	}
	return 0;
}

static int
maildir_stat(struct maildir_mailbox *mbox, const char *path, struct stat *st_r)
{
	struct mailbox *box = &mbox->box;
	int i;

	for (i = 0;; i++) {
		if (nfs_safe_stat(path, st_r) == 0)
			return 0;
		if (errno != ENOENT || i == MAILDIR_DELETE_RETRY_COUNT)
			break;

		if (!maildir_set_deleted(box))
			return -1;
		/* try again */
	}

	mailbox_set_critical(box, "stat(%s) failed: %m", path);
	return -1;
}

static int
maildir_scan_dir(struct maildir_sync_context *ctx, bool final,
		 enum maildir_scan_why why)
{
	struct event *event = ctx->mbox->box.event;
	const char *path;
	DIR *dirp;
	struct dirent *dp;
	struct stat st;
	enum maildir_uidlist_rec_flag flags;
	unsigned int time_diff, i, readdir_count = 0, move_count = 0;
	time_t start_time;
	int ret = 1;
	bool dir_changed = FALSE;

	path = ctx->cur_dir;
	for (i = 0;; i++) {
		dirp = opendir(path);
		if (dirp != NULL)
			break;

		if (errno != ENOENT || i == MAILDIR_DELETE_RETRY_COUNT) {
			if (ENOACCESS(errno)) {
				mailbox_set_critical(&ctx->mbox->box, "%s",
					eacces_error_get("opendir", path));
			} else {
				mailbox_set_critical(&ctx->mbox->box,
					"opendir(%s) failed: %m", path);
			}
			return -1;
		}

		if (!maildir_set_deleted(&ctx->mbox->box))
			return -1;
		/* try again */
	}

#ifdef HAVE_DIRFD
	if (fstat(dirfd(dirp), &st) < 0) {
		mailbox_set_critical(&ctx->mbox->box,
			"fstat(%s) failed: %m", path);
		(void)closedir(dirp);
		return -1;
	}
#else
	if (maildir_stat(ctx->mbox, path, &st) < 0) {
		(void)closedir(dirp);
		return -1;
	}
#endif

	start_time = time(NULL);
	ctx->mbox->maildir_hdr.cur_check_time = start_time;
	ctx->mbox->maildir_hdr.cur_mtime = st.st_mtime;
	ctx->mbox->maildir_hdr.cur_mtime_nsecs = ST_MTIME_NSEC(st);

	errno = 0;
	for (; (dp = readdir(dirp)) != NULL; errno = 0) {
		if (dp->d_name[0] == '.')
			continue;

		if (dp->d_name[0] == MAILDIR_INFO_SEP) {
			continue;
		}

		flags = 0;

		readdir_count++;
		if ((readdir_count % MAILDIR_SLOW_CHECK_COUNT) == 0)
			maildir_sync_notify(ctx);

		ret = maildir_uidlist_sync_next(ctx->uidlist_sync_ctx,
						dp->d_name, flags);
		if (ret <= 0) {
			if (ret < 0)
				break;

			/* possibly duplicate - try fixing it */
			T_BEGIN {
				ret = maildir_fix_duplicate(ctx, path,
							    dp->d_name);
			} T_END;
			if (ret < 0)
				break;
		}
	}

#ifdef __APPLE__
	if (errno == EINVAL && move_count > 0 && !final) {
		/* OS X HFS+: readdir() fails sometimes when rename()
		   have been done. */
		move_count = MAILDIR_RENAME_RESCAN_COUNT + 1;
	} else
#endif

	if (errno != 0) {
		mailbox_set_critical(&ctx->mbox->box,
				     "readdir(%s) failed: %m", path);
		ret = -1;
	}

	if (closedir(dirp) < 0) {
		mailbox_set_critical(&ctx->mbox->box,
				     "closedir(%s) failed: %m", path);
		ret = -1;
	}

	if (dir_changed) {
		if (stat(ctx->cur_dir, &st) == 0) {
			ctx->mbox->maildir_hdr.new_check_time =
				I_MAX(st.st_mtime, start_time);
			ctx->mbox->maildir_hdr.cur_mtime = st.st_mtime;
			ctx->mbox->maildir_hdr.cur_mtime_nsecs =
				ST_MTIME_NSEC(st);
		}
	}
	time_diff = time(NULL) - start_time;
	if (time_diff >= MAILDIR_SYNC_TIME_WARN_SECS) {
		e_warning(event,
			  "Scanning %s took %u seconds "
			  "(%u readdir()s, %u rename()s to cur/, why=0x%x)",
			  path, time_diff, readdir_count, move_count, why);
	}

	return ret < 0 ? -1 :
		(move_count <= MAILDIR_RENAME_RESCAN_COUNT || final ? 0 : 1);
}

static void maildir_sync_get_header(struct maildir_mailbox *mbox)
{
	const void *data;
	size_t data_size;

	mail_index_get_header_ext(mbox->box.view, mbox->maildir_ext_id,
				  &data, &data_size);
	if (data_size == 0) {
		/* header doesn't exist */
	} else {
		memcpy(&mbox->maildir_hdr, data,
		       I_MIN(sizeof(mbox->maildir_hdr), data_size));
	}
}

int maildir_sync_header_refresh(struct maildir_mailbox *mbox)
{
	if (mail_index_refresh(mbox->box.index) < 0) {
		mailbox_set_index_error(&mbox->box);
		return -1;
	}
	maildir_sync_get_header(mbox);
	return 0;
}

static int maildir_sync_quick_check(struct maildir_mailbox *mbox, bool undirty,
				    const char *cur_dir,
				    bool *cur_changed_r,
				    enum maildir_scan_why *why_r)
{
#define DIR_DELAYED_REFRESH(hdr, name) \
	((hdr)->name ## _check_time <= \
		(hdr)->name ## _mtime + MAILDIR_SYNC_SECS && \
	 (undirty || \
	  (time_t)(hdr)->name ## _check_time < ioloop_time - MAILDIR_SYNC_SECS))

#define DIR_MTIME_CHANGED(st, hdr, name) \
	((st).st_mtime != (time_t)(hdr)->name ## _mtime || \
	 !ST_NTIMES_EQUAL(ST_MTIME_NSEC(st), (hdr)->name ## _mtime_nsecs))

	struct maildir_index_header *hdr = &mbox->maildir_hdr;
	struct stat cur_st;
	bool refreshed = FALSE, check_cur = FALSE;

	*why_r = 0;

	if (mbox->maildir_hdr.new_mtime == 0) {
		maildir_sync_get_header(mbox);
		if (mbox->maildir_hdr.new_mtime == 0) {
			/* first sync */
			*why_r |= WHY_FIRSTSYNC;
			*cur_changed_r = TRUE;
			return 0;
		}
	}

	*cur_changed_r = FALSE;

	/* try to avoid stat()ing by first checking delayed changes */
	if ((DIR_DELAYED_REFRESH(hdr, cur) &&
	     !mbox->storage->set->maildir_very_dirty_syncs)) {
		/* refresh index and try again */
		if (maildir_sync_header_refresh(mbox) < 0)
			return -1;
		refreshed = TRUE;

		if (DIR_DELAYED_REFRESH(hdr, cur) &&
		    !mbox->storage->set->maildir_very_dirty_syncs) {
			*why_r |= WHY_DELAYEDCUR;
			*cur_changed_r = TRUE;
		}
		if (*cur_changed_r)
			return 0;
	}

	if (!*cur_changed_r) {
		if (maildir_stat(mbox, cur_dir, &cur_st) < 0)
			return -1;
		check_cur = TRUE;
	}

	for (;;) {
		if (check_cur) {
			*cur_changed_r = DIR_MTIME_CHANGED(cur_st, hdr, cur);
			if (*cur_changed_r)
				*why_r |= WHY_CURCHANGED;
		}

		if ((!*cur_changed_r) || refreshed)
			break;

		/* refresh index and try again */
		if (maildir_sync_header_refresh(mbox) < 0)
			return -1;
		refreshed = TRUE;
	}

	return 0;
}

static void maildir_sync_update_next_uid(struct maildir_mailbox *mbox)
{
	const struct mail_index_header *hdr;
	uint32_t uid_validity;

	hdr = mail_index_get_header(mbox->box.view);
	if (hdr->uid_validity == 0)
		return;

	uid_validity = maildir_uidlist_get_uid_validity(mbox->uidlist);
	if (uid_validity == hdr->uid_validity || uid_validity == 0) {
		/* make sure uidlist's next_uid is at least as large as
		   index file's. typically this happens only if uidlist gets
		   deleted. */
		maildir_uidlist_set_uid_validity(mbox->uidlist,
						 hdr->uid_validity);
		maildir_uidlist_set_next_uid(mbox->uidlist,
					     hdr->next_uid, FALSE);
	}
}

static int maildir_sync_get_changes(struct maildir_sync_context *ctx,
				    bool *cur_changed_r,
				    enum maildir_scan_why *why_r)
{
	struct maildir_mailbox *mbox = ctx->mbox;
	enum mail_index_sync_flags flags = 0;
	bool undirty = (ctx->flags & MAILBOX_SYNC_FLAG_FULL_READ) != 0;

	*why_r = 0;

	if (maildir_sync_quick_check(mbox, undirty, ctx->cur_dir,
				     cur_changed_r, why_r) < 0)
		return -1;

	if (*cur_changed_r)
		return 1;

	if ((mbox->box.flags & MAILBOX_FLAG_DROP_RECENT) != 0)
		flags |= MAIL_INDEX_SYNC_FLAG_DROP_RECENT;

	if (mbox->synced) {
		/* refresh index only after the first sync, i.e. avoid wasting
		   time on refreshing it immediately after it was just opened */
		mail_index_refresh(mbox->box.index);
	}
	return mail_index_sync_have_any(mbox->box.index, flags) ? 1 : 0;
}

static int ATTR_NULL(3)
maildir_sync_context(struct maildir_sync_context *ctx, bool forced,
		     uint32_t *find_uid, bool *lost_files_r)
{
	enum maildir_uidlist_sync_flags sync_flags;
	enum maildir_uidlist_rec_flag flags;
	bool cur_changed, lock_failure;
	const char *fname;
	enum maildir_scan_why why;
	int ret;

	*lost_files_r = FALSE;

	if (forced) {
		cur_changed = TRUE;
		why = WHY_FORCED;
	} else {
		ret = maildir_sync_get_changes(ctx, &cur_changed,
					       &why);
		if (ret <= 0)
			return ret;
	}

	/*
	   Locking, locking, locking.. Wasn't maildir supposed to be lockless?

	   We can get here either as beginning a real maildir sync, or when
	   committing changes to maildir but a file was lost (maybe renamed).

	   So, we're going to need two locks. One for index and one for
	   uidlist. To avoid deadlocking do the uidlist lock always first.

	   uidlist is needed only for figuring out UIDs for newly seen files,
	   so theoretically we wouldn't need to lock it unless there are new
	   files. It has a few problems though, assuming the index lock didn't
	   already protect it (eg. in-memory indexes):

	   1. Just because you see a new file which doesn't exist in uidlist
	   file, doesn't mean that the file really exists anymore, or that
	   your readdir() lists all new files. Meaning that this is possible:

	     A: opendir(), readdir() -> new file ...
	     -- new files are written to the maildir --
	     B: opendir(), readdir() -> new file, lock uidlist,
		readdir() -> another new file, rewrite uidlist, unlock
	     A: ... lock uidlist, readdir() -> nothing left, rewrite uidlist,
		unlock

	   The second time running A didn't see the two new files. To handle
	   this correctly, it must not remove the new unseen files from
	   uidlist. This is possible to do, but adds extra complexity.

	   2. If another process is rename()ing files while we are
	   readdir()ing, it's possible that readdir() never lists some files,
	   causing Dovecot to assume they were expunged. In next sync they
	   would show up again, but client could have already been notified of
	   that and they would show up under new UIDs, so the damage is
	   already done.

	   Both of the problems can be avoided if we simply lock the uidlist
	   before syncing and keep it until sync is finished. Typically this
	   would happen in any case, as there is the index lock..

	   The second case is still a problem with external changes though,
	   because maildir doesn't require any kind of locking. Luckily this
	   problem rarely happens except under high amount of modifications.
	*/

	if (!cur_changed) {
		ctx->partial = TRUE;
		sync_flags = MAILDIR_UIDLIST_SYNC_PARTIAL;
	} else {
		ctx->partial = FALSE;
		sync_flags = 0;
		if (forced)
			sync_flags |= MAILDIR_UIDLIST_SYNC_FORCE;
		if ((ctx->flags & MAILBOX_SYNC_FLAG_FAST) != 0)
			sync_flags |= MAILDIR_UIDLIST_SYNC_TRYLOCK;
	}
	ret = maildir_uidlist_sync_init(ctx->mbox->uidlist, sync_flags,
					&ctx->uidlist_sync_ctx);
	lock_failure = ret <= 0;
	if (ret <= 0) {
		struct mail_storage *storage = ctx->mbox->box.storage;

		if (ret == 0) {
			/* timeout */
			return 0;
		}
		/* locking failed. sync anyway without locking so that it's
		   possible to expunge messages when out of quota. */
		if (forced) {
			/* we're already forcing a sync, we're trying to find
			   a message that was probably already expunged, don't
			   loop for a long time trying to find it. */
			return -1;
		}
		ret = maildir_uidlist_sync_init(ctx->mbox->uidlist, sync_flags |
						MAILDIR_UIDLIST_SYNC_NOLOCK,
						&ctx->uidlist_sync_ctx);
		if (ret <= 0) {
			i_assert(ret != 0);
			return -1;
		}

		if (storage->callbacks.notify_no != NULL) {
			storage->callbacks.notify_no(&ctx->mbox->box,
				"Internal mailbox synchronization failure, "
				"showing only old mails.",
				storage->callback_context);
		}
	}
	ctx->locked = maildir_uidlist_is_locked(ctx->mbox->uidlist);
	if (!ctx->locked)
		ctx->partial = TRUE;

	if (!ctx->mbox->syncing_commit && (ctx->locked || lock_failure)) {
		if (maildir_sync_index_begin(ctx->mbox, ctx,
					     &ctx->index_sync_ctx) < 0)
			return -1;
	}

	if (cur_changed) {
		if (maildir_scan_dir(ctx, TRUE, why) < 0)
			return -1;

		maildir_sync_update_next_uid(ctx->mbox);

		/* finish uidlist syncing, but keep it still locked */
		maildir_uidlist_sync_finish(ctx->uidlist_sync_ctx);
	}

	if (!ctx->locked) {
		/* make sure we sync the maildir later */
		ctx->mbox->maildir_hdr.cur_mtime = 0;
	}

	if (ctx->index_sync_ctx != NULL) {
		/* NOTE: index syncing here might cause a re-sync due to
		   files getting lost, so this function might be called
		   reentrantly. */
		ret = maildir_sync_index(ctx->index_sync_ctx, ctx->partial);
		if (ret < 0)
			maildir_sync_index_rollback(&ctx->index_sync_ctx);
		else if (maildir_sync_index_commit(&ctx->index_sync_ctx) < 0)
			return -1;

		if (ret < 0)
			return -1;
		if (ret == 0)
			*lost_files_r = TRUE;

		i_assert(maildir_uidlist_is_locked(ctx->mbox->uidlist) ||
			 lock_failure);
	}

	if (find_uid != NULL && *find_uid != 0) {
		ret = maildir_uidlist_lookup(ctx->mbox->uidlist,
					     *find_uid, &flags, &fname);
		if (ret < 0)
			return -1;
		if (ret == 0) {
			/* UID is expunged */
			*find_uid = 0;
		} else if ((flags & MAILDIR_UIDLIST_REC_FLAG_NONSYNCED) == 0) {
			*find_uid = 0;
		} else {
			/* we didn't find it, possibly expunged? */
		}
	}

	return maildir_uidlist_sync_deinit(&ctx->uidlist_sync_ctx, TRUE);
}

int maildir_sync_lookup(struct maildir_mailbox *mbox, uint32_t uid,
			enum maildir_uidlist_rec_flag *flags_r,
			const char **fname_r)
{
	int ret;

	ret = maildir_uidlist_lookup(mbox->uidlist, uid, flags_r, fname_r);
	if (ret != 0)
		return ret;

	if (maildir_uidlist_is_open(mbox->uidlist)) {
		/* refresh uidlist and check again in case it was added
		   after the last mailbox sync */
		if (mbox->sync_uidlist_refreshed) {
			/* we've already refreshed it, don't bother again */
			return ret;
		}
		mbox->sync_uidlist_refreshed = TRUE;
		if (maildir_uidlist_refresh(mbox->uidlist) < 0)
			return -1;
	} else {
		/* the uidlist doesn't exist. */
		if (maildir_storage_sync_force(mbox, uid) < 0)
			return -1;
	}

	/* try again */
	return maildir_uidlist_lookup(mbox->uidlist, uid, flags_r, fname_r);
}

static int maildir_sync_run(struct maildir_mailbox *mbox,
			    enum mailbox_sync_flags flags, bool force_resync,
			    uint32_t *uid, bool *lost_files_r)
{
	struct maildir_sync_context *ctx;
	bool retry, lost_files;
	int ret;

	T_BEGIN {
		ctx = maildir_sync_context_new(mbox, flags);
		ret = maildir_sync_context(ctx, force_resync, uid, lost_files_r);
		retry = ctx->racing;
		maildir_sync_deinit(ctx);
	} T_END;

	if (retry) T_BEGIN {
		/* we're racing some file. retry the sync again to see if the
		   file is really gone or not. if it is, this is a bit of
		   unnecessary work, but if it's not, this is necessary for
		   e.g. doveadm force-resync to work. */
		ctx = maildir_sync_context_new(mbox, 0);
		ret = maildir_sync_context(ctx, TRUE, NULL, &lost_files);
		maildir_sync_deinit(ctx);
	} T_END;
	return ret;
}

int maildir_storage_sync_force(struct maildir_mailbox *mbox, uint32_t uid)
{
	bool lost_files;
	int ret;

	ret = maildir_sync_run(mbox, MAILBOX_SYNC_FLAG_FAST,
			       TRUE, &uid, &lost_files);
	if (uid != 0) {
		/* maybe it's expunged. check again. */
		ret = maildir_sync_run(mbox, 0, TRUE, NULL, &lost_files);
	}
	return ret;
}

int maildir_sync_refresh_flags_view(struct maildir_mailbox *mbox)
{
	struct mail_index_view_sync_ctx *sync_ctx;
	bool delayed_expunges;

	mail_index_refresh(mbox->box.index);
	if (mbox->flags_view == NULL)
		mbox->flags_view = mail_index_view_open(mbox->box.index);

	sync_ctx = mail_index_view_sync_begin(mbox->flags_view,
			MAIL_INDEX_VIEW_SYNC_FLAG_FIX_INCONSISTENT);
	if (mail_index_view_sync_commit(&sync_ctx, &delayed_expunges) < 0) {
		mailbox_set_index_error(&mbox->box);
		return -1;
	}
	/* make sure the map stays in private memory */
	if (mbox->flags_view->map->refcount > 1) {
		struct mail_index_map *map;

		map = mail_index_map_clone(mbox->flags_view->map);
		mail_index_unmap(&mbox->flags_view->map);
		mbox->flags_view->map = map;
	}
	mail_index_record_map_move_to_private(mbox->flags_view->map);
	mail_index_map_move_to_memory(mbox->flags_view->map);
	return 0;
}

struct mailbox_sync_context *
maildir_storage_sync_init(struct mailbox *box, enum mailbox_sync_flags flags)
{
	struct maildir_mailbox *mbox = MAILDIR_MAILBOX(box);
	bool lost_files, force_resync;
	int ret = 0;

	force_resync = (flags & MAILBOX_SYNC_FLAG_FORCE_RESYNC) != 0;
	if (index_mailbox_want_full_sync(&mbox->box, flags)) {
		ret = maildir_sync_run(mbox, flags, force_resync,
				       NULL, &lost_files);
		i_assert(!maildir_uidlist_is_locked(mbox->uidlist) ||
			 (box->flags & MAILBOX_FLAG_KEEP_LOCKED) != 0);

		if (lost_files) {
			/* lost some files from new/, see if they're in cur/ */
			ret = maildir_storage_sync_force(mbox, 0);
		}
	}

	if (mbox->storage->set->maildir_very_dirty_syncs) {
		if (maildir_sync_refresh_flags_view(mbox) < 0)
			ret = -1;
		maildir_uidlist_set_all_nonsynced(mbox->uidlist);
	}
	mbox->synced = TRUE;
	mbox->sync_uidlist_refreshed = FALSE;
	return index_mailbox_sync_init(box, flags, ret < 0);
}

int maildir_sync_is_synced(struct maildir_mailbox *mbox)
{
	bool cur_changed;
	enum maildir_scan_why why;
	int ret;

	T_BEGIN {
		const char *box_path = mailbox_get_path(&mbox->box);
		const char *cur_dir;

		cur_dir = t_strconcat(box_path, "/cur", NULL);

		ret = maildir_sync_quick_check(mbox, FALSE, cur_dir,
					       &cur_changed,
					       &why);
	} T_END;
	return ret < 0 ? -1 : (!cur_changed);
}
