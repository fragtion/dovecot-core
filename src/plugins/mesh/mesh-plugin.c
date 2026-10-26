/* Copyright (c) Dovecot authors, see top-level COPYING file */

#include "lib.h"
#include "net.h"
#include "str.h"
#include "strescape.h"
#include "ostream.h"
#include "file-lock.h"
#include "settings.h"
#include "settings-parser.h"
#include "mail-user.h"
#include "mail-storage-private.h"
#include "mail-storage-hooks.h"
#include "notify-plugin.h"
#include "mesh-plugin.h"

#include <fcntl.h>

#define MESH_USER_CONTEXT(obj) \
	MODULE_CONTEXT(obj, mesh_user_module)
#define MESH_USER_CONTEXT_REQUIRE(obj) \
	MODULE_CONTEXT_REQUIRE(obj, mesh_user_module)
#define MESH_CONTEXT(obj) \
	MODULE_CONTEXT_REQUIRE(obj, mesh_storage_module)

/* Don't let notifications pile up if the mesh service is stuck. */
#define MESH_NOTIFY_MAX_BUFFER_SIZE 4096

struct mesh_plugin_settings {
	pool_t pool;

	const char *mesh_notify_socket_path;
	unsigned int mesh_save_lock_timeout;
	unsigned int mesh_dsync_lock_timeout;
};

enum mesh_priority {
	MESH_PRIORITY_NONE = 0,
	MESH_PRIORITY_NORMAL,
	MESH_PRIORITY_HIGH,
};

struct mesh_user {
	union mail_user_module_context module_ctx;

	const struct mesh_plugin_settings *set;
	const char *lock_path;
	int lock_fd;
	/* exclusive lock held for the lifetime of a dsync session */
	struct file_lock *dsync_lock;
	const char *dsync_lock_error;

	struct ostream *notify_output;
	bool notify_connect_failed:1;
};

struct mesh_transaction {
	struct mail_user *user;
	enum mesh_priority priority;
};

const char *mesh_plugin_version = DOVECOT_ABI_VERSION;
const char *mesh_plugin_dependencies[] = { "notify", NULL };

static MODULE_CONTEXT_DEFINE_INIT(mesh_user_module,
				  &mail_user_module_register);
static MODULE_CONTEXT_DEFINE_INIT(mesh_storage_module,
				  &mail_storage_module_register);

static struct notify_context *mesh_notify_ctx;

#undef DEF
#define DEF(type, name) \
	SETTING_DEFINE_STRUCT_##type(#name, name, struct mesh_plugin_settings)

static const struct setting_define mesh_plugin_setting_defines[] = {
	DEF(STR, mesh_notify_socket_path),
	DEF(TIME, mesh_save_lock_timeout),
	DEF(TIME, mesh_dsync_lock_timeout),

	SETTING_DEFINE_LIST_END
};

static const struct mesh_plugin_settings mesh_plugin_default_settings = {
	.mesh_notify_socket_path = "mesh-notify",
	.mesh_save_lock_timeout = 30,
	.mesh_dsync_lock_timeout = 10,
};

const struct setting_parser_info mesh_plugin_setting_parser_info = {
	.name = "mesh_plugin",
	.plugin_dependency = "lib25_mesh_plugin",

	.defines = mesh_plugin_setting_defines,
	.defaults = &mesh_plugin_default_settings,

	.struct_size = sizeof(struct mesh_plugin_settings),
	.pool_offset1 = 1 + offsetof(struct mesh_plugin_settings, pool),
};

/*
 * Notifications to the mesh service
 */

static bool mesh_notify_connect(struct mail_user *user,
				struct mesh_user *muser)
{
	const char *path = muser->set->mesh_notify_socket_path;
	int fd;

	if (muser->notify_output != NULL)
		return TRUE;
	if (muser->notify_connect_failed)
		return FALSE;

	if (path[0] != '/') {
		path = t_strconcat(user->set->base_dir, "/", path, NULL);
	}
	fd = net_connect_unix(path);
	if (fd == -1) {
		/* The mesh service isn't running here. Changes are still
		   replicated by the next periodic sync, so only debug log. */
		e_debug(user->event, "mesh: net_connect_unix(%s) failed: %m",
			path);
		muser->notify_connect_failed = TRUE;
		return FALSE;
	}
	muser->notify_output = o_stream_create_fd_autoclose(&fd,
		MESH_NOTIFY_MAX_BUFFER_SIZE);
	o_stream_set_no_error_handling(muser->notify_output, TRUE);
	o_stream_nsend_str(muser->notify_output, "VERSION\tmesh-notify\t1\t0\n");
	return TRUE;
}

static void mesh_notify(struct mail_user *user, enum mesh_priority priority)
{
	struct mesh_user *muser = MESH_USER_CONTEXT(user);
	string_t *str;

	if (muser == NULL || priority == MESH_PRIORITY_NONE)
		return;
	if (!mesh_notify_connect(user, muser))
		return;

	/* NOTIFY <user> <high|normal> <dsync=0|1> */
	str = t_str_new(128);
	str_append(str, "NOTIFY\t");
	str_append_tabescaped(str, user->username);
	str_append(str, priority == MESH_PRIORITY_HIGH ?
		   "\thigh\t" : "\tnormal\t");
	str_append(str, user->dsyncing ? "1" : "0");
	str_append_c(str, '\n');
	o_stream_nsend(muser->notify_output, str_data(str), str_len(str));
	if (o_stream_flush(muser->notify_output) < 0) {
		e_debug(user->event, "mesh: write(%s) failed: %s",
			o_stream_get_name(muser->notify_output),
			o_stream_get_error(muser->notify_output));
		o_stream_destroy(&muser->notify_output);
	}
}

static void *mesh_mail_transaction_begin(struct mailbox_transaction_context *t)
{
	struct mesh_transaction *mt = i_new(struct mesh_transaction, 1);

	mt->user = mail_storage_get_user(mailbox_get_storage(t->box));
	return mt;
}

static void mesh_txn_set_priority(void *txn, enum mesh_priority priority)
{
	struct mesh_transaction *mt = txn;

	if (mt->priority < priority)
		mt->priority = priority;
}

static void mesh_mail_save(void *txn, struct mail *mail ATTR_UNUSED)
{
	mesh_txn_set_priority(txn, MESH_PRIORITY_HIGH);
}

static void mesh_mail_copy(void *txn, struct mail *src ATTR_UNUSED,
			   struct mail *dst ATTR_UNUSED)
{
	mesh_txn_set_priority(txn, MESH_PRIORITY_HIGH);
}

static void mesh_mail_expunge(void *txn, struct mail *mail ATTR_UNUSED)
{
	mesh_txn_set_priority(txn, MESH_PRIORITY_NORMAL);
}

static void mesh_mail_update_flags(void *txn, struct mail *mail ATTR_UNUSED,
				   enum mail_flags old_flags ATTR_UNUSED)
{
	mesh_txn_set_priority(txn, MESH_PRIORITY_NORMAL);
}

static void
mesh_mail_update_keywords(void *txn, struct mail *mail ATTR_UNUSED,
			  const char *const *old_keywords ATTR_UNUSED)
{
	mesh_txn_set_priority(txn, MESH_PRIORITY_NORMAL);
}

static void
mesh_mail_transaction_commit(void *txn,
			     struct mail_transaction_commit_changes *changes ATTR_UNUSED)
{
	struct mesh_transaction *mt = txn;

	mesh_notify(mt->user, mt->priority);
	i_free(mt);
}

static void mesh_mail_transaction_rollback(void *txn)
{
	i_free(txn);
}

static void mesh_mailbox_changed(struct mailbox *box)
{
	mesh_notify(mail_storage_get_user(mailbox_get_storage(box)),
		    MESH_PRIORITY_NORMAL);
}

static void
mesh_mailbox_delete_commit(void *txn ATTR_UNUSED, struct mailbox *box)
{
	mesh_mailbox_changed(box);
}

static void
mesh_mailbox_rename(struct mailbox *src ATTR_UNUSED, struct mailbox *dest)
{
	mesh_mailbox_changed(dest);
}

static void
mesh_mailbox_set_subscribed(struct mailbox *box,
			    bool subscribed ATTR_UNUSED)
{
	mesh_mailbox_changed(box);
}

static const struct notify_vfuncs mesh_vfuncs = {
	.mail_transaction_begin = mesh_mail_transaction_begin,
	.mail_save = mesh_mail_save,
	.mail_copy = mesh_mail_copy,
	.mail_expunge = mesh_mail_expunge,
	.mail_update_flags = mesh_mail_update_flags,
	.mail_update_keywords = mesh_mail_update_keywords,
	.mail_transaction_commit = mesh_mail_transaction_commit,
	.mail_transaction_rollback = mesh_mail_transaction_rollback,
	.mailbox_create = mesh_mailbox_changed,
	/* No .mailbox_update: dsync updates mailbox metadata (UIDNEXT,
	   HIGHESTMODSEQ, cache fields) at the end of every session, so
	   relaying those would make the anchor sync forever. Local clients
	   practically never change it; the periodic sync covers them. */
	.mailbox_delete_commit = mesh_mailbox_delete_commit,
	.mailbox_rename = mesh_mailbox_rename,
	.mailbox_set_subscribed = mesh_mailbox_set_subscribed,
};

/*
 * Locking between dsync sessions and local saves
 */

static int mesh_lock_open(struct mail_user *user, struct mesh_user *muser,
			  const char **error_r)
{
	const char *home;

	if (muser->lock_fd != -1)
		return 0;
	if (mail_user_get_home(user, &home) <= 0) {
		*error_r = "User has no home directory";
		return -1;
	}
	muser->lock_path = p_strconcat(user->pool, home, "/",
				       MESH_LOCK_FILENAME, NULL);
	muser->lock_fd = open(muser->lock_path, O_RDWR | O_CREAT, 0600);
	if (muser->lock_fd == -1 && errno == ENOENT) {
		if (mail_user_home_mkdir(user) < 0) {
			*error_r = "Couldn't create home directory";
			return -1;
		}
		muser->lock_fd = open(muser->lock_path, O_RDWR | O_CREAT,
				      0600);
	}
	if (muser->lock_fd == -1) {
		*error_r = t_strdup_printf("open(%s) failed: %m",
					   muser->lock_path);
		return -1;
	}
	return 0;
}

static int mesh_lock(struct mail_user *user, int lock_type,
		     unsigned int timeout_secs, struct file_lock **lock_r,
		     const char **error_r)
{
	struct mesh_user *muser = MESH_USER_CONTEXT_REQUIRE(user);
	const struct file_lock_settings lock_set = {
		.lock_method = FILE_LOCK_METHOD_FLOCK,
	};
	const char *error;
	int ret;

	if (mesh_lock_open(user, muser, error_r) < 0)
		return -1;
	ret = file_wait_lock(muser->lock_fd, muser->lock_path, lock_type,
			     &lock_set, timeout_secs, lock_r, &error);
	if (ret <= 0) {
		*error_r = ret < 0 ? error : t_strdup_printf(
			"Timed out after %u seconds waiting for %s",
			timeout_secs, muser->lock_path);
		return -1;
	}
	return 0;
}

/* dsync holds the lock exclusively from the first mailbox it opens until
   the user is freed, i.e. for the whole session. */
static int mesh_dsync_lock(struct mailbox *box)
{
	struct mail_user *user = box->storage->user;
	struct mesh_user *muser = MESH_USER_CONTEXT(user);
	const char *error;

	if (muser == NULL || !user->dsyncing || muser->dsync_lock != NULL)
		return 0;
	if (muser->dsync_lock_error == NULL &&
	    mesh_lock(user, F_WRLCK, muser->set->mesh_dsync_lock_timeout,
		      &muser->dsync_lock, &error) < 0) {
		muser->dsync_lock_error = p_strdup(user->pool, error);
	}
	if (muser->dsync_lock_error != NULL) {
		/* Probably two servers are syncing the user with each other at
		   the same time. Fail this session (every mailbox of it,
		   without waiting again); it's retried later. */
		mail_storage_set_error(box->storage, MAIL_ERROR_INUSE,
			t_strdup_printf("mesh: dsync lock failed: %s",
					muser->dsync_lock_error));
		return -1;
	}
	return 0;
}

static int mesh_mailbox_open(struct mailbox *box)
{
	union mailbox_module_context *mbox = MESH_CONTEXT(box);

	if (mesh_dsync_lock(box) < 0)
		return -1;
	return mbox->super.open(box);
}

static int
mesh_transaction_commit(struct mailbox_transaction_context *t,
			struct mail_transaction_commit_changes *changes_r)
{
	struct mailbox *box = t->box;
	union mailbox_module_context *mbox = MESH_CONTEXT(box);
	struct mail_user *user = box->storage->user;
	struct mesh_user *muser = MESH_USER_CONTEXT(user);
	struct file_lock *lock = NULL;
	const char *error;
	int ret;

	if (muser == NULL || user->dsyncing || t->save_count == 0)
		return mbox->super.transaction_commit(t, changes_r);

	/* New mails get their UIDs during commit. Wait until no dsync
	   session is importing mails for this user. If the wait takes too
	   long, commit anyway: delivering the mail matters more. */
	if (mesh_lock(user, F_RDLCK, muser->set->mesh_save_lock_timeout,
		      &lock, &error) < 0)
		e_warning(box->event, "mesh: Saving without lock: %s", error);
	ret = mbox->super.transaction_commit(t, changes_r);
	if (lock != NULL)
		file_unlock(&lock);
	return ret;
}

static void mesh_mailbox_allocated(struct mailbox *box)
{
	struct mailbox_vfuncs *v = box->vlast;
	union mailbox_module_context *mbox;

	if (MESH_USER_CONTEXT(box->storage->user) == NULL)
		return;

	mbox = p_new(box->pool, union mailbox_module_context, 1);
	mbox->super = *v;
	box->vlast = &mbox->super;
	v->open = mesh_mailbox_open;
	v->transaction_commit = mesh_transaction_commit;
	MODULE_CONTEXT_SET_SELF(box, mesh_storage_module, mbox);
}

static void mesh_user_deinit(struct mail_user *user)
{
	struct mesh_user *muser = MESH_USER_CONTEXT_REQUIRE(user);

	if (muser->dsync_lock != NULL)
		file_unlock(&muser->dsync_lock);
	i_close_fd(&muser->lock_fd);
	o_stream_destroy(&muser->notify_output);
	settings_free(muser->set);

	muser->module_ctx.super.deinit(user);
}

static void mesh_mail_user_created(struct mail_user *user)
{
	struct mail_user_vfuncs *v = user->vlast;
	struct mesh_user *muser;
	const struct mesh_plugin_settings *set;
	const char *error;

	if (settings_get(user->event, &mesh_plugin_setting_parser_info, 0,
			 &set, &error) < 0) {
		user->error = p_strdup(user->pool, error);
		return;
	}

	muser = p_new(user->pool, struct mesh_user, 1);
	muser->set = set;
	muser->lock_fd = -1;
	muser->module_ctx.super = *v;
	user->vlast = &muser->module_ctx.super;
	v->deinit = mesh_user_deinit;
	MODULE_CONTEXT_SET(user, mesh_user_module, muser);
}

static struct mail_storage_hooks mesh_mail_storage_hooks = {
	.mail_user_created = mesh_mail_user_created,
	.mailbox_allocated = mesh_mailbox_allocated,
};

void mesh_plugin_init(struct module *module)
{
	mesh_notify_ctx = notify_register(&mesh_vfuncs);
	mail_storage_hooks_add(module, &mesh_mail_storage_hooks);
}

void mesh_plugin_deinit(void)
{
	mail_storage_hooks_remove(&mesh_mail_storage_hooks);
	notify_unregister(&mesh_notify_ctx);
}
