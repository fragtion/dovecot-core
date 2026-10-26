#ifndef DSYNC_LOCAL_MAILS_H
#define DSYNC_LOCAL_MAILS_H

/* Finds local copies of mails that the remote has in some mailbox where we
   don't have them yet, so that they can be copied locally instead of being
   transferred again. This is what happens when mails are moved or copied
   to another mailbox: dsync syncs one mailbox at a time, and the mails
   appear as new mails in the destination mailbox.

   Expunges that the remote sent are applied only at the end of the session,
   so that a mail moved away from a mailbox that was synced first can still
   be found there. Its GUID is then already known, so it costs nothing to
   find. Mails moved away from a mailbox that is synced later are found by
   scanning the other mailboxes for their GUIDs, which is done only when
   enough mail would otherwise be transferred. */

struct mail;
struct mail_user;
struct mailbox;
struct dsync_local_mails;

struct dsync_local_mails *
dsync_local_mails_init(struct mail_user *user,
		       enum mailbox_transaction_flags transaction_flags,
		       struct event *event_parent);
/* Apply the deferred expunges, and free everything. Returns -1 if some of
   them couldn't be applied. */
int dsync_local_mails_finish(struct dsync_local_mails **lm);

/* The mail's expunge is deferred until dsync_local_mails_finish(). Called
   only for mailboxes whose import succeeded. */
void dsync_local_mails_add_expunge(struct dsync_local_mails *lm,
				   const char *vname, uint32_t uid,
				   const char *guid);

/* Should mails missing from a mailbox be looked up from all the other
   mailboxes before requesting them from the remote? */
bool dsync_local_mails_want_scan(unsigned int missing_count,
				 uoff_t missing_bytes);
/* Look for the given GUIDs in all mailboxes except exclude_box. The results
   are used by dsync_local_mails_find(). */
void dsync_local_mails_scan(struct dsync_local_mails *lm,
			    struct mailbox *exclude_box,
			    const ARRAY_TYPE(const_string) *guids);
/* Find a local mail with the GUID outside exclude_box. Returns NULL if not
   found. The mail stays usable until dsync_local_mails_release(). */
struct mail *dsync_local_mails_find(struct dsync_local_mails *lm,
				    const char *guid,
				    struct mailbox *exclude_box);
/* Close the mailboxes opened by dsync_local_mails_find(). */
void dsync_local_mails_release(struct dsync_local_mails *lm);

#endif
