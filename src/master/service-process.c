/* Copyright (c) 2005-2018 Dovecot authors, see the included COPYING file */

#include "common.h"
#include "array.h"
#include "aqueue.h"
#include "ioloop.h"
#include "istream.h"
#include "ostream.h"
#include "write-full.h"
#include "base64.h"
#include "hash.h"
#include "str.h"
#include "strescape.h"
#include "llist.h"
#include "hostpid.h"
#include "env-util.h"
#include "restrict-access.h"
#include "restrict-process-size.h"
#include "eacces-error.h"
#include "var-expand.h"
#include "master-service.h"
#include "master-service-settings.h"
#include "dup2-array.h"
#include "service.h"
#include "service-anvil.h"
#include "service-listen.h"
#include "service-log.h"
#include "service-process-notify.h"
#include "service-process.h"

#include <unistd.h>
#include <fcntl.h>
#include <syslog.h>
#include <signal.h>
#include <sys/wait.h>

static void service_reopen_inet_listeners(struct service *service)
{
	struct service_listener *const *listeners;
	unsigned int i, count;
	int old_fd;

	listeners = array_get(&service->listeners, &count);
	for (i = 0; i < count; i++) {
		if (!listeners[i]->reuse_port || listeners[i]->fd == -1)
			continue;

		old_fd = listeners[i]->fd;
		listeners[i]->fd = -1;
		if (service_listener_listen(listeners[i]) < 0)
			listeners[i]->fd = old_fd;
	}
}

static int
service_unix_pid_listener_get_path(struct service_listener *l, pid_t pid,
				   string_t *path, const char **error_r)
{
	const struct var_expand_params params = {
		.table = (const struct var_expand_table[]) {
			{ .key = "pid", .value = dec2str(pid) },
			VAR_EXPAND_TABLE_END
		},
		.event = l->service->event,
	};

	str_truncate(path, 0);
	return var_expand(path, l->set.fileset.set->path, &params, error_r);
}

static void
service_dup_fds(struct service *service)
{
	struct service_listener *const *listeners;
	ARRAY_TYPE(dup2) dups;
	string_t *listener_settings;
	int fd = MASTER_LISTEN_FD_FIRST;
	unsigned int i, count, socket_listener_count;

	/* stdin/stdout is already redirected to /dev/null. Other master fds
	   should have been opened with fd_close_on_exec() so we don't have to
	   worry about them.

	   because the destination fd might be another one's source fd we have
	   to be careful not to overwrite anything. dup() the fd when needed */

        socket_listener_count = 0;
	listeners = array_get(&service->listeners, &count);
	t_array_init(&dups, count + 10);

	switch (service->type) {
	case SERVICE_TYPE_LOG:
		i_assert(fd == MASTER_LISTEN_FD_FIRST);
		services_log_dup2(&dups, service->list, fd,
				  &socket_listener_count);
		fd += socket_listener_count;
		break;
	case SERVICE_TYPE_ANVIL:
		dup2_append(&dups, service_anvil_global->log_fdpass_fd[0],
			    MASTER_ANVIL_LOG_FDPASS_FD);
		/* nonblocking anvil fd must be the first one. anvil treats it
		   as the master's fd */
		dup2_append(&dups, service_anvil_global->nonblocking_fd[0], fd++);
		dup2_append(&dups, service_anvil_global->blocking_fd[0], fd++);
		socket_listener_count += 2;
		break;
	default:
		break;
	}

	/* add listeners */
	listener_settings = t_str_new(256);
	for (i = 0; i < count; i++) {
		if (listeners[i]->fd != -1) {
			str_truncate(listener_settings, 0);
			str_append_tabescaped(listener_settings, listeners[i]->name);

			if (listeners[i]->type == SERVICE_LISTENER_INET) {
				if (listeners[i]->set.inetset.set->ssl)
					str_append(listener_settings, "\tssl");
				if (listeners[i]->set.inetset.set->haproxy)
					str_append(listener_settings, "\thaproxy");
				if (*listeners[i]->set.inetset.set->type != '\0') {
					str_append(listener_settings, "\ttype=");
					str_append_tabescaped(
						listener_settings,
						listeners[i]->set.inetset.set->type);
				}
			}
			if (listeners[i]->type == SERVICE_LISTENER_FIFO ||
			    listeners[i]->type == SERVICE_LISTENER_UNIX) {
				if (*listeners[i]->set.fileset.set->type != '\0') {
					str_append(listener_settings, "\ttype=");
					str_append_tabescaped(
						listener_settings,
						listeners[i]->set.fileset.set->type);
				}
			}

			dup2_append(&dups, listeners[i]->fd, fd++);

			env_put(t_strdup_printf("SOCKET%d_SETTINGS",
						socket_listener_count),
				str_c(listener_settings));
			socket_listener_count++;
		}
	}
	if (array_is_created(&service->unix_pid_listeners)) {
		struct service_listener *const *listenerp, *l;
		string_t *path = t_str_new(128);
		const char *error;
		int ret;
		pid_t pid = getpid();

		array_foreach(&service->unix_pid_listeners, listenerp) {
			l = *listenerp;
			ret = service_unix_pid_listener_get_path(l, pid, path, &error);
			if (ret == 0) {
				ret = service_unix_listener_listen(l,
					str_c(path), FALSE, &error);
			}
			if (ret <= 0) {
				i_fatal("Failed to create per-PID unix_listener %s: %s",
					l->name, error);
			}

			str_truncate(listener_settings, 0);
			str_append_tabescaped(listener_settings, l->name);
			str_append(listener_settings, "\tpid");
			dup2_append(&dups, l->fd, fd++);

			env_put(t_strdup_printf("SOCKET%d_SETTINGS",
						socket_listener_count),
				str_c(listener_settings));
			socket_listener_count++;
		}
	}

	if (service->login_notify_fd != -1) {
		dup2_append(&dups, service->login_notify_fd,
			    MASTER_LOGIN_NOTIFY_FD);
	}
	switch (service->type) {
	case SERVICE_TYPE_LOG:
	case SERVICE_TYPE_ANVIL:
	case SERVICE_TYPE_CONFIG:
		dup2_append(&dups, dev_null_fd, MASTER_ANVIL_FD);
		break;
	case SERVICE_TYPE_UNKNOWN:
	case SERVICE_TYPE_LOGIN:
	case SERVICE_TYPE_STARTUP:
	case SERVICE_TYPE_WORKER:
		dup2_append(&dups, service_anvil_global->blocking_fd[1],
			    MASTER_ANVIL_FD);
		break;
	}
	dup2_append(&dups, service->status_fd[1], MASTER_STATUS_FD);
	if (service->type != SERVICE_TYPE_ANVIL) {
		dup2_append(&dups, service->master_dead_pipe_fd[1],
			    MASTER_DEAD_FD);
	} else {
		dup2_append(&dups, global_master_dead_pipe_fd[1],
			    MASTER_DEAD_FD);
	}

	if (service->type == SERVICE_TYPE_LOG) {
		/* keep stderr as-is. this is especially important when
		   log_path=/dev/stderr, but might be helpful even in other
		   situations for logging startup errors */
	} else {
		/* set log file to stderr. dup2() here immediately so that
		   we can set up logging to it without causing any log messages
		   to be lost. */
		i_assert(service->log_fd[1] != -1);

		env_put(MASTER_SERVICE_LOG_SERVICE_ENV, "1");
		if (dup2(service->log_fd[1], STDERR_FILENO) < 0)
			i_fatal("dup2(log fd) failed: %m");
		i_set_failure_internal();
	}

	if (service->type == SERVICE_TYPE_LOG) {
		/* Pass our config fd to the log process, so it won't depend
		   on config process. Note that we don't want to do this for
		   other processes, since it prevents config reload. */
		i_assert(global_config_fd != -1);
		if (lseek(global_config_fd, 0, SEEK_SET) < 0)
			i_fatal("lseek(config fd, 0) failed: %m");
		dup2_append(&dups, global_config_fd, MASTER_CONFIG_FD);
		env_put(DOVECOT_CONFIG_FD_ENV, dec2str(MASTER_CONFIG_FD));
	}

	/* Switch log writing back to stderr before the log fds are closed.
	   There's no guarantee that writing to stderr is visible anywhere, but
	   it's better than the process just dying with FATAL_LOGWRITE. */
	i_set_failure_file("/dev/stderr",
		t_strdup_printf("service(%s): ", service->set->name));

	/* make sure we don't leak syslog fd. try to do it as late as possible,
	   but also before dup2()s in case syslog fd is one of them. */
	closelog();

	if (dup2_array(&dups) < 0)
		i_fatal("service(%s): dup2s failed", service->set->name);

	i_assert(fd == MASTER_LISTEN_FD_FIRST + (int)socket_listener_count);
	env_put(MASTER_SERVICE_SOCKET_COUNT_ENV, dec2str(socket_listener_count));
}

static void
drop_privileges(struct service *service)
{
	struct restrict_access_settings rset;
	bool allow_root;
	size_t len;

	if (service->vsz_limit != 0)
		restrict_process_size(service->vsz_limit);

	restrict_access_init(&rset);
	rset.uid = service->uid;
	rset.gid = service->gid;
	rset.privileged_gid = service->privileged_gid;
	rset.chroot_dir = *service->set->chroot == '\0' ? NULL :
		service->set->chroot;
	if (rset.chroot_dir != NULL) {
		/* drop trailing / if it exists */
		len = strlen(rset.chroot_dir);
		if (rset.chroot_dir[len-1] == '/')
			rset.chroot_dir = t_strndup(rset.chroot_dir, len-1);
	}
	rset.extra_groups = service->extra_gids;

	restrict_access_set_env(&rset);
	if (service->set->drop_priv_before_exec) {
		allow_root = service->type != SERVICE_TYPE_LOGIN;
		restrict_access(&rset,
				allow_root ? RESTRICT_ACCESS_FLAG_ALLOW_ROOT : 0,
				NULL);
	}
}

static void service_process_setup_config_environment(struct service *service)
{
	switch (service->type) {
	case SERVICE_TYPE_CONFIG:
		env_put(MASTER_CONFIG_FILE_ENV, service->config_file_path);
		break;
	default:
		env_put(MASTER_CONFIG_FILE_ENV, services->config->config_file_path);
		env_put(MASTER_CONFIG_FILE_SOCKET_ENV,
			services_get_config_socket_path(service->list));
		break;
	}
}

static void
service_process_setup_environment(struct service *service, unsigned int uid,
				  const char *hostdomain)
{
	const struct master_service_settings *service_set =
		master_service_get_service_settings(master_service);
	master_service_env_clean();

	env_put(MASTER_IS_PARENT_ENV, "1");
	service_process_setup_config_environment(service);
	env_put(MASTER_SERVICE_ENV, service->set->name);
	env_put(MASTER_CLIENT_LIMIT_ENV, dec2str(service->client_limit));
	env_put(MASTER_PROCESS_LIMIT_ENV, dec2str(service->process_limit));
	env_put(MASTER_PROCESS_MIN_AVAIL_ENV,
		dec2str(service->set->process_min_avail));
	env_put(MASTER_SERVICE_IDLE_KILL_INTERVAL_ENV,
		dec2str(service->idle_kill_interval));
	if (service->set->restart_request_count != 0) {
		env_put(MASTER_SERVICE_COUNT_ENV,
			dec2str(service->set->restart_request_count));
	}
	env_put(MASTER_UID_ENV, dec2str(uid));
	env_put(MY_HOSTNAME_ENV, my_hostname);
	env_put(MY_HOSTDOMAIN_ENV, hostdomain);

	if (service_set->verbose_proctitle)
		env_put(MASTER_VERBOSE_PROCTITLE_ENV, "1");
	if (!service->list->set->version_ignore)
		env_put(MASTER_DOVECOT_VERSION_ENV, PACKAGE_VERSION);

	if (service_set->stats_writer_socket_path[0] == '\0')
		; /* stats-writer socket disabled */
	else if (service->set->chroot[0] != '\0') {
		/* In a chroot - expect stats-writer socket to be in the
		   current directory. */
		env_put(DOVECOT_STATS_WRITER_SOCKET_PATH,
			service_set->stats_writer_socket_path);
	} else {
		env_put(DOVECOT_STATS_WRITER_SOCKET_PATH,
			t_strdup_printf("%s/%s", service_set->base_dir,
					service_set->stats_writer_socket_path));
	}
	if (ssl_manual_key_password != NULL && service->have_inet_listeners) {
		/* manually given SSL password. give it only to services
		   that have inet listeners. */
		env_put(MASTER_SSL_KEY_PASSWORD_ENV, ssl_manual_key_password);
	}
	if (service->type == SERVICE_TYPE_ANVIL &&
	    service_anvil_global->restarted)
		env_put("ANVIL_RESTARTED", "1");
	env_put(DOVECOT_LOG_DEBUG_ENV, service_set->log_debug);
}

static void service_process_status_timeout(struct service_process *process)
{
	e_error(process->service->event,
		"Initial status notification not received in %d "
		"seconds, killing the process",
		SERVICE_FIRST_STATUS_TIMEOUT_SECS);
	if (kill(process->pid, SIGKILL) < 0 && errno != ESRCH) {
		e_error(process->service->event, "kill(%s, SIGKILL) failed: %m",
			dec2str(process->pid));
	}
	timeout_remove(&process->to_status);
}

struct service_process *service_process_create(struct service *service)
{
	static unsigned int uid_counter = 0;
	struct service_process *process;
	unsigned int uid = ++uid_counter;
	const char *hostdomain;
	pid_t pid;
	bool process_forked;

	i_assert(service->status_fd[0] != -1);

	if (service->to_throttle != NULL) {
		/* throttling service, don't create new processes */
		return NULL;
	}
	if (service->list->destroying) {
		/* these services are being destroyed, no point in creating
		   new processes now */
		return NULL;
	}
	/* look this up before fork()ing so that it gets cached for all the
	   future lookups. */
	hostdomain = my_hostdomain();

	if (service->type == SERVICE_TYPE_ANVIL &&
	    service_anvil_global->pid != 0) {
		pid = service_anvil_global->pid;
		uid = service_anvil_global->uid;
		process_forked = FALSE;
	} else {
		pid = fork();
		process_forked = TRUE;
		service->list->fork_counter++;
	}

	if (pid < 0) {
		int fork_errno = errno;
		rlim_t limit;
		const char *limit_str = "";

		if (fork_errno == EAGAIN &&
		    restrict_get_process_limit(&limit) == 0) {
			limit_str = t_strdup_printf(" (ulimit -u %llu reached?)",
						    (unsigned long long)limit);
		}
		errno = fork_errno;
		e_error(service->event, "fork() failed: %m%s", limit_str);
		return NULL;
	}
	if (pid == 0) {
		/* child */
		service_process_setup_environment(service, uid, hostdomain);
		service_reopen_inet_listeners(service);
		service_dup_fds(service);
		drop_privileges(service);
		process_exec(service->executable);
	}
	i_assert(hash_table_lookup(service_pids, POINTER_CAST(pid)) == NULL);

	process = i_new(struct service_process, 1);
	process->service = service;
	process->refcount = 1;
	process->pid = pid;
	process->uid = uid;
	process->create_time = ioloop_time;
	if (process_forked) {
		process->to_status =
			timeout_add(SERVICE_FIRST_STATUS_TIMEOUT_SECS * 1000,
				    service_process_status_timeout, process);
	}

	process->available_count = service->client_limit;
	process->idle_start = ioloop_time;
	service->process_count_total++;
	service->process_count++;
	service->process_avail++;
	service->process_idling++;
	DLLIST2_APPEND(&service->idle_processes_head,
		       &service->idle_processes_tail, process);

	service_list_ref(service->list);
	hash_table_insert(service_pids, POINTER_CAST(process->pid), process);

	if (service->type == SERVICE_TYPE_ANVIL && process_forked)
		service_anvil_process_created(process);
	return process;
}

void service_process_destroy(struct service_process *process)
{
	struct service *service = process->service;
	struct service_list *service_list = service->list;

	i_assert(!process->destroyed);

	if (array_is_created(&service->unix_pid_listeners)) {
		struct service_listener *const *listenerp;
		string_t *path = t_str_new(128);
		const char *error;

		array_foreach(&service->unix_pid_listeners, listenerp) {
			str_truncate(path, 0);
			if (service_unix_pid_listener_get_path(*listenerp,
					process->pid, path, &error) == 0)
				i_unlink_if_exists(str_c(path));
		}
	}

	if (process->idle_start == 0)
		DLLIST_REMOVE(&service->busy_processes, process);
	else {
		DLLIST2_REMOVE(&service->idle_processes_head,
			       &service->idle_processes_tail, process);
		i_assert(service->process_idling > 0);
		i_assert(service->process_idling <= service->process_avail);
		service->process_idling--;
		service->process_idling_lowwater_since_kills =
			I_MIN(service->process_idling_lowwater_since_kills,
			      service->process_idling);
	}
	hash_table_remove(service_pids, POINTER_CAST(process->pid));

	if (process->available_count > 0) {
		i_assert(service->process_avail > 0);
		service->process_avail--;
		i_assert(service->process_idling <= service->process_avail);
	}
	i_assert(service->process_count > 0);
	service->process_count--;
	i_assert(service->process_avail <= service->process_count);

	timeout_remove(&process->to_status);
	timeout_remove(&process->to_idle_kill);
	if (service->list->log_byes != NULL)
		service_process_notify_add(service->list->log_byes, process);

	process->destroyed = TRUE;
	service_process_unref(process);

	if (service->process_count < service->process_limit &&
	    service->type == SERVICE_TYPE_LOGIN)
		service_login_notify(service, FALSE);

	service_list_unref(service_list);
}

void service_process_ref(struct service_process *process)
{
	i_assert(process->refcount > 0);

	process->refcount++;
}

void service_process_unref(struct service_process *process)
{
	i_assert(process->refcount > 0);

	if (--process->refcount > 0)
		return;

	i_assert(process->destroyed);
	i_free(process);
}

static const char *
get_exit_status_message(struct service *service, enum fatal_exit_status status)
{
	string_t *str;

	switch (status) {
	case FATAL_LOGOPEN:
		return "Can't open log file";
	case FATAL_LOGWRITE:
		return "Can't write to log file";
	case FATAL_LOGERROR:
		return "Internal logging error";
	case FATAL_OUTOFMEM:
		str = t_str_new(128);
		str_append(str, "Out of memory");
		if (service->vsz_limit != 0) {
			str_printfa(str, " (service %s { vsz_limit=%"PRIuUOFF_T" MB }, "
				    "you may need to increase it)",
				    service->set->name,
				    service->vsz_limit/1024/1024);
		}
		if (getenv("CORE_OUTOFMEM") == NULL)
			str_append(str, " - set CORE_OUTOFMEM=1 environment to get core dump");
		return str_c(str);
	case FATAL_EXEC:
		return "exec() failed";

	case FATAL_DEFAULT:
		return "Fatal failure";
	}

	return NULL;
}

static bool linux_proc_fs_suid_is_dumpable(struct event *event, unsigned int *value_r)
{
	int fd = open(LINUX_PROC_FS_SUID_DUMPABLE, O_RDONLY);
	if (fd == -1) {
		/* we already checked that it exists - shouldn't get here */
		e_error(event,
			"open(%s) failed: %m", LINUX_PROC_FS_SUID_DUMPABLE);
		have_proc_fs_suid_dumpable = FALSE;
		return FALSE;
	}
	char buf[10];
	ssize_t ret = read(fd, buf, sizeof(buf)-1);
	if (ret < 0) {
		e_error(event,
			"read(%s) failed: %m", LINUX_PROC_FS_SUID_DUMPABLE);
		have_proc_fs_suid_dumpable = FALSE;
		*value_r = 0;
	} else {
		buf[ret] = '\0';
		if (ret > 0 && buf[ret-1] == '\n')
			buf[ret-1] = '\0';
		if (str_to_uint(buf, value_r) < 0)
			*value_r = 0;
	}
	i_close_fd(&fd);
	return *value_r != 0;
}

static bool linux_is_absolute_core_pattern(struct event *event)
{
	int fd = open(LINUX_PROC_SYS_KERNEL_CORE_PATTERN, O_RDONLY);
	if (fd == -1) {
		/* we already checked that it exists - shouldn't get here */
		e_error(event,
			"open(%s) failed: %m", LINUX_PROC_SYS_KERNEL_CORE_PATTERN);
		have_proc_sys_kernel_core_pattern = FALSE;
		return FALSE;
	}
	char buf[10];
	ssize_t ret = read(fd, buf, sizeof(buf)-1);
	if (ret < 0) {
		e_error(event,
			"read(%s) failed: %m", LINUX_PROC_SYS_KERNEL_CORE_PATTERN);
		have_proc_sys_kernel_core_pattern = FALSE;
		buf[0] = '\0';
	}
	i_close_fd(&fd);
	return buf[0] == '/' || buf[0] == '|';
}

static void
log_coredump(struct service *service, string_t *str, int status)
{
#define CORE_DUMP_URL "https://dovecot.org/bugreport.html#coredumps"
#ifdef WCOREDUMP
	int signum = WTERMSIG(status);
	unsigned int dumpable;

	if (WCOREDUMP(status) != 0) {
		str_append(str, " (core dumped)");
		return;
	}

	if (signum != SIGABRT && signum != SIGSEGV && signum != SIGBUS)
		return;

	/* let's try to figure out why we didn't get a core dump */
	if (core_dumps_disabled) {
		str_printfa(str, " (core dumps disabled - "CORE_DUMP_URL")");
		return;
	}
	str_append(str, " (core not dumped - "CORE_DUMP_URL);

	/* If we're running on Linux, the best way to get core dumps is to set
	   fs.suid_dumpable=2 and sys.kernel.core_pattern to be an absolute
	   path. */
	if (!have_proc_fs_suid_dumpable)
		;
	else if (!linux_proc_fs_suid_is_dumpable(service->event, &dumpable)) {
		str_printfa(str, " - set %s to 2)", LINUX_PROC_FS_SUID_DUMPABLE);
		return;
	} else if (dumpable == 2 && have_proc_sys_kernel_core_pattern &&
		   !linux_is_absolute_core_pattern(service->event)) {
		str_printfa(str, " - set %s to absolute path)",
			    LINUX_PROC_SYS_KERNEL_CORE_PATTERN);
		return;
	} else if (dumpable == 1 || have_proc_sys_kernel_core_pattern) {
		str_append(str, " - core wasn't writable?)");
		return;
	}

#ifndef HAVE_PR_SET_DUMPABLE
	if (!service->set->drop_priv_before_exec && service->uid != 0) {
		str_printfa(str, " - set service %s "
			    "{ drop_priv_before_exec=yes })",
			    service->set->name);
		return;
	}
	if (*service->set->privileged_group != '\0' && service->uid != 0) {
		str_printfa(str, " - service %s "
			    "{ privileged_group } prevented it)",
			    service->set->name);
		return;
	}
#else
	if (!service->set->login_dump_core &&
	    service->type == SERVICE_TYPE_LOGIN) {
		str_printfa(str, " - add -D parameter to "
			    "service %s { executable }", service->set->name);
		return;
	}
#endif
	if (service->set->chroot[0] != '\0') {
		str_printfa(str, " - try to clear "
			    "service %s { chroot = } )", service->set->name);
		return;
	}
	str_append_c(str, ')');
#endif
}

static void
service_process_get_status_error(string_t *str, struct service_process *process,
				 int status, bool *default_fatal_r)
{
	struct service *service = process->service;
	const char *msg;

	*default_fatal_r = FALSE;

	str_printfa(str, "service(%s): child %s ", service->set->name,
		    dec2str(process->pid));
	if (WIFSIGNALED(status)) {
		str_printfa(str, "killed with signal %d", WTERMSIG(status));
		log_coredump(service, str, status);
		return;
	}
	if (!WIFEXITED(status)) {
		str_printfa(str, "died with status %d", status);
		return;
	}

	status = WEXITSTATUS(status);
	if (status == 0) {
		str_truncate(str, 0);
		return;
	}
	str_printfa(str, "returned error %d", status);

	msg = get_exit_status_message(service, status);
	if (msg != NULL)
		str_printfa(str, " (%s)", msg);

	if (status == FATAL_DEFAULT)
		*default_fatal_r = TRUE;
}

static void service_process_log(struct service_process *process,
				bool default_fatal, const char *str)
{
	const char *data;

	if (process->service->log_fd[1] == -1) {
		e_error(process->service->event, "%s", str);
		return;
	}

	/* log it via the log process in charge of handling
	   this process's logging */
	data = t_strdup_printf("%d %s %s %s\n",
			       process->service->log_process_internal_fd,
			       dec2str(process->pid),
			       default_fatal ? "DEFAULT-FATAL" : "FATAL", str);
	if (write(process->service->list->master_log_fd[1],
		  data, strlen(data)) < 0) {
		e_error(process->service->event, "write(log process) failed: %m");
		e_error(process->service->event, "%s", str);
	}
}

void service_process_log_status_error(struct service_process *process,
				      int status)
{
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		/* fast path */
		return;
	}
	T_BEGIN {
		string_t *str = t_str_new(256);
		bool default_fatal;

		service_process_get_status_error(str, process, status,
						 &default_fatal);
		if (str_len(str) > 0)
			service_process_log(process, default_fatal, str_c(str));
	} T_END;
}
