/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2005-2008 Csaba Henk <csaba.henk@creo.hu>
  Copyright (C) 2006-2008 Amit Singh / Google Inc.
  Copyright (C) 2011-2026 Benjamin Fleischer

  Architecture specific file system mounting (Darwin). Derived from mount_bsd.c
  from the FUSE distribution.

  This program can be distributed under the terms of the GNU LGPLv2.
  See the file COPYING.LIB.
*/

#include "fuse_i.h"
#include "fuse_darwin.h"
#include "fuse_opt.h"

#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <paths.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <DiskArbitration/DiskArbitration.h>
#include <MFMount/MFMount.h>

enum {
	KEY_ALLOW_ROOT,
	KEY_AUTO_CACHE,
	KEY_DIO,
	KEY_IGNORED,
	KEY_KERN,
	KEY_RO
};

struct mount_opts {
	bool allow_other;
	bool allow_root;
	bool quiet_mode;
	unsigned max_read;
	char *backend;
	char *kernel_opts;
};

static const struct fuse_opt fuse_mount_opts[] = {
	{ "allow_other", offsetof(struct mount_opts, allow_other), true },
	{ "allow_root", offsetof(struct mount_opts, allow_root), true },
	{ "quiet", offsetof(struct mount_opts, quiet_mode), true },
	{ "max_read=%u", offsetof(struct mount_opts, max_read), 1 },
	{ "backend=%s", offsetof(struct mount_opts, backend), 1 },
	FUSE_OPT_KEY("allow_root",	      KEY_ALLOW_ROOT),
	FUSE_OPT_KEY("auto_cache",	      KEY_AUTO_CACHE),
	FUSE_OPT_KEY("-r",		      KEY_RO),
	/* Standard FreeBSD mount options */
	FUSE_OPT_KEY("dev",		      KEY_KERN),
	FUSE_OPT_KEY("async",		      KEY_KERN),
	FUSE_OPT_KEY("atime",		      KEY_KERN),
	FUSE_OPT_KEY("dev",		      KEY_KERN),
	FUSE_OPT_KEY("exec",		      KEY_KERN),
	FUSE_OPT_KEY("suid",		      KEY_KERN),
	FUSE_OPT_KEY("symfollow",	      KEY_KERN),
	FUSE_OPT_KEY("rdonly",		      KEY_KERN),
	FUSE_OPT_KEY("sync",		      KEY_KERN),
	FUSE_OPT_KEY("union",		      KEY_KERN),
	FUSE_OPT_KEY("userquota",	      KEY_KERN),
	FUSE_OPT_KEY("groupquota",	      KEY_KERN),
	FUSE_OPT_KEY("clusterr",	      KEY_KERN),
	FUSE_OPT_KEY("clusterw",	      KEY_KERN),
	FUSE_OPT_KEY("suiddir",		      KEY_KERN),
	FUSE_OPT_KEY("snapshot",	      KEY_KERN),
	FUSE_OPT_KEY("multilabel",	      KEY_KERN),
	FUSE_OPT_KEY("acls",		      KEY_KERN),
	FUSE_OPT_KEY("force",		      KEY_KERN),
	FUSE_OPT_KEY("update",		      KEY_KERN),
	FUSE_OPT_KEY("ro",		      KEY_KERN),
	FUSE_OPT_KEY("rw",		      KEY_KERN),
	FUSE_OPT_KEY("auto",		      KEY_KERN),
	/* Options supported under both Linux and FreeBSD */
	FUSE_OPT_KEY("allow_other",	      KEY_KERN),
	FUSE_OPT_KEY("default_permissions",   KEY_KERN),
	/* FreeBSD FUSE specific mount options */
	FUSE_OPT_KEY("private",		      KEY_KERN),
	FUSE_OPT_KEY("neglect_shares",	      KEY_KERN),
	FUSE_OPT_KEY("push_symlinks_in",      KEY_KERN),
	/* Stock FreeBSD mountopt parsing routine lets anything be negated... */
	FUSE_OPT_KEY("nodev",		      KEY_KERN),
	FUSE_OPT_KEY("noasync",		      KEY_KERN),
	FUSE_OPT_KEY("noatime",		      KEY_KERN),
	FUSE_OPT_KEY("nodev",		      KEY_KERN),
	FUSE_OPT_KEY("noexec",		      KEY_KERN),
	FUSE_OPT_KEY("nosuid",		      KEY_KERN),
	FUSE_OPT_KEY("nosymfollow",	      KEY_KERN),
	FUSE_OPT_KEY("nordonly",	      KEY_KERN),
	FUSE_OPT_KEY("nosync",		      KEY_KERN),
	FUSE_OPT_KEY("nounion",		      KEY_KERN),
	FUSE_OPT_KEY("nouserquota",	      KEY_KERN),
	FUSE_OPT_KEY("nogroupquota",	      KEY_KERN),
	FUSE_OPT_KEY("noclusterr",	      KEY_KERN),
	FUSE_OPT_KEY("noclusterw",	      KEY_KERN),
	FUSE_OPT_KEY("nosuiddir",	      KEY_KERN),
	FUSE_OPT_KEY("nosnapshot",	      KEY_KERN),
	FUSE_OPT_KEY("nomultilabel",	      KEY_KERN),
	FUSE_OPT_KEY("noacls",		      KEY_KERN),
	FUSE_OPT_KEY("noforce",		      KEY_KERN),
	FUSE_OPT_KEY("noupdate",	      KEY_KERN),
	FUSE_OPT_KEY("noro",		      KEY_KERN),
	FUSE_OPT_KEY("norw",		      KEY_KERN),
	FUSE_OPT_KEY("noauto",		      KEY_KERN),
	FUSE_OPT_KEY("noallow_other",	      KEY_KERN),
	FUSE_OPT_KEY("nodefault_permissions", KEY_KERN),
	FUSE_OPT_KEY("noprivate",	      KEY_KERN),
	FUSE_OPT_KEY("noneglect_shares",      KEY_KERN),
	FUSE_OPT_KEY("nopush_symlinks_in",    KEY_KERN),
	/* Darwin FUSE specific mount options */
	FUSE_OPT_KEY("allow_recursion",	      KEY_KERN),
	FUSE_OPT_KEY("allow_root",	      KEY_KERN),
	FUSE_OPT_KEY("auto_xattr",	      KEY_KERN),
	FUSE_OPT_KEY("automounted",	      KEY_IGNORED),
	FUSE_OPT_KEY("blocksize=",	      KEY_KERN),
	FUSE_OPT_KEY("daemon_timeout=",	      KEY_KERN),
	FUSE_OPT_KEY("default_permissions",   KEY_KERN),
	FUSE_OPT_KEY("defer_permissions",     KEY_KERN),
	FUSE_OPT_KEY("direct_io",	      KEY_DIO),
	FUSE_OPT_KEY("excl_create",	      KEY_KERN),
	FUSE_OPT_KEY("extended_security",     KEY_KERN),
	FUSE_OPT_KEY("fair_locking",	      KEY_KERN),
	FUSE_OPT_KEY("fsid=",		      KEY_KERN),
	FUSE_OPT_KEY("fsname=",		      KEY_KERN),
	FUSE_OPT_KEY("fssubtype=",	      KEY_KERN),
	FUSE_OPT_KEY("fstypename=",	      KEY_KERN),
	FUSE_OPT_KEY("init_timeout=",	      KEY_KERN),
	FUSE_OPT_KEY("iosize=",		      KEY_KERN),
	FUSE_OPT_KEY("jail_symlinks",	      KEY_KERN),
	FUSE_OPT_KEY("kill_on_unmount",	      KEY_KERN),
	FUSE_OPT_KEY("local",		      KEY_KERN),
	FUSE_OPT_KEY("native_xattr",	      KEY_KERN),
	FUSE_OPT_KEY("negative_vncache",      KEY_KERN),
	FUSE_OPT_KEY("noalerts",	      KEY_KERN),
	FUSE_OPT_KEY("noappledouble",	      KEY_KERN),
	FUSE_OPT_KEY("noapplexattr",	      KEY_KERN),
	FUSE_OPT_KEY("noattrcache",	      KEY_KERN),
	FUSE_OPT_KEY("noautonotify",	      KEY_KERN),
	FUSE_OPT_KEY("nobrowse",	      KEY_KERN),
	FUSE_OPT_KEY("nolocalcaches",	      KEY_KERN),
	FUSE_OPT_KEY("noping_diskarb",	      KEY_IGNORED),
	FUSE_OPT_KEY("noreadahead",	      KEY_KERN),
	FUSE_OPT_KEY("nosynconclose",	      KEY_KERN),
	FUSE_OPT_KEY("nosyncwrites",	      KEY_KERN),
	FUSE_OPT_KEY("noubc",		      KEY_KERN),
	FUSE_OPT_KEY("novncache",	      KEY_KERN),
	FUSE_OPT_KEY("ping_diskarb",	      KEY_IGNORED),
	FUSE_OPT_KEY("slow_statfs",	      KEY_KERN),
	FUSE_OPT_KEY("sparse",		      KEY_KERN),
	FUSE_OPT_KEY("subtype=",	      KEY_IGNORED),
	FUSE_OPT_KEY("volname=",	      KEY_KERN),
	FUSE_OPT_END
};

static void fuse_mount_run(const char *mount_args)
{
	int err = 0;
	char *mount_tool_path = NULL;
	char *mount_command = NULL;

	mount_tool_path = fuse_darwin_resource_path(FUSE_MOUNT_PROG);
	if (!mount_tool_path) {
		fuse_log(FUSE_LOG_ERR, "fuse: mount tool missing\n");
		goto out;
	}

	err = asprintf(&mount_command, "%s %s", mount_tool_path, mount_args);
	if (err == -1) {
		goto out;
	}
	system(mount_command);

out:
	free(mount_tool_path);
	free(mount_command);
}

void fuse_mount_version(void)
{
	fuse_mount_run("--version");
}

unsigned get_max_read(struct mount_opts *o)
{
	return o->max_read;
}

static int fuse_mount_opt_proc(void *data, const char *arg, int key,
			       struct fuse_args *outargs)
{
	struct mount_opts *mo = data;

	switch (key) {
	case KEY_AUTO_CACHE:
		if (fuse_opt_add_opt(&mo->kernel_opts, "auto_cache") == -1
		    || fuse_opt_add_arg(outargs, "-oauto_cache") == -1)
			return -1;
		return 0;

	case KEY_ALLOW_ROOT:
		if (fuse_opt_add_opt(&mo->kernel_opts, "allow_other") == -1
		    || fuse_opt_add_arg(outargs, "-oallow_root") == -1)
			return -1;
		return 0;

	case KEY_RO:
		arg = "ro";
		/* fall through */

	case KEY_KERN:
		return fuse_opt_add_opt_escaped(&mo->kernel_opts, arg);

	case KEY_DIO:
		if (fuse_opt_add_opt(&mo->kernel_opts, "direct_io") == -1
		    || fuse_opt_add_arg(outargs, "-odirect_io") == -1)
			return -1;
		return 0;

	case KEY_IGNORED:
		return 0;
	}

	/* Pass through unknown options */
	return 1;
}

void fuse_darwin_unmount(DADiskRef disk, DADiskUnmountOptions options)
{
	if (disk)
		DADiskUnmount(disk, options, NULL, NULL);
}

/*
 * return value:
 * >= 0	 => fd
 * -1	 => error
 */
static int receive_fd(int sock_fd)
{
	struct msghdr msg;
	struct iovec iov;
	char buf[1];
	size_t rv;
	char ccmsg[CMSG_SPACE(sizeof(int))];
	struct cmsghdr *cmsg;
	int fd;

	iov.iov_base = buf;
	iov.iov_len = 1;

	memset(&msg, 0, sizeof(msg));
	msg.msg_name = 0;
	msg.msg_namelen = 0;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = ccmsg;
	msg.msg_controllen = sizeof(ccmsg);

	while (((rv = recvmsg(sock_fd, &msg, 0)) == -1) && errno == EINTR);
	if (rv == -1) {
		fuse_log(FUSE_LOG_ERR, "fuse: recvmsg() failed\n");
		return -1;
	}
	if (!rv) {
		/* EOF */
		return -1;
	}

	cmsg = CMSG_FIRSTHDR(&msg);
	if (!cmsg)
		return -1;
	if (cmsg->cmsg_type != SCM_RIGHTS) {
		fuse_log(FUSE_LOG_ERR,
			 "fuse: received message of unknown type %d\n",
			 cmsg->cmsg_type);
		return -1;
	}

	memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
	return fd;
}

struct fuse_mount_core_wait_arg {
	int fd;
	void (*callback)(void *context, int res);
	void *context;
};

static void *fuse_mount_core_wait(void *arg)
{
	struct fuse_mount_core_wait_arg *a =
		(struct fuse_mount_core_wait_arg *)arg;
	int32_t status = -1;
	ssize_t rv = 0;

	if (!a->callback) {
		goto out;
	}

	while (((rv = recv(a->fd, &status, sizeof(status), 0)) == -1) &&
	       errno == EINTR);
	if (rv == -1 || rv == 0) {
		/*
		 * We did not receive a mount status, but we still need to
		 * invoke the callback, otherweise we might leak a->context.
		 * Assume the mount operation failed with an unknown error.
		 */
		status = -1;
		fuse_log(FUSE_LOG_ERR, "fuse: unknown mount status\n");
	}

	a->callback(a->context, status);

out:
	free(arg);
	return NULL;
}

static int fuse_mount_core(const char *mountpoint, struct mount_opts *mo,
			   void (*callback)(void *, int), void *context)
{
	int fd = -1;
	int result;
	char *mount_tool_path;
	int fds[2];
	pid_t pid;
	int status;

	if (!mountpoint) {
		fuse_log(FUSE_LOG_ERR,
			 "fuse: missing or invalid mount point\n");
		return -1;
	}

	signal(SIGCHLD, SIG_DFL); /* So that we can wait4() below. */

	if (getenv("FUSE_NO_MOUNT") || ! mountpoint) {
		goto out;
	}

	mount_tool_path = fuse_darwin_resource_path(FUSE_MOUNT_PROG);
	if (!mount_tool_path) {
		fuse_log(FUSE_LOG_ERR, "fuse: mount program missing\n");
		return -1;
	}

	result = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
	if (result == -1) {
		fuse_log(FUSE_LOG_ERR, "fuse: socketpair() failed\n");
		return -1;
	}

	pid = fork();

	if (pid == -1) {
		fuse_log(FUSE_LOG_ERR, "fuse: fork failed\n");
		close(fds[0]);
		close(fds[1]);
		return -1;
	}

	if (pid == 0) {
		pid_t cpid = fork();

		if (cpid == -1) {
			fuse_log(FUSE_LOG_ERR, "fuse: double-fork failed\n");
			close(fds[0]);
			close(fds[1]);
			_exit(1);
		}

		if (cpid == 0) {
			char daemon_path[PROC_PIDPATHINFO_MAXSIZE];
			char commfd[10];

			const char *argv[32];
			int a = 0;

			close(fds[1]);
			fcntl(fds[0], F_SETFD, 0);

			if (proc_pidpath(getpid(), daemon_path,
					 PROC_PIDPATHINFO_MAXSIZE)) {
				setenv("_FUSE_DAEMON_PATH", daemon_path, 1);
			}

			snprintf(commfd, sizeof(commfd), "%i", fds[0]);
			setenv("_FUSE_COMMFD", commfd, 1);
			setenv("_FUSE_COMMVERS", "2", 1);

			argv[a++] = mount_tool_path;
			if (mo->kernel_opts) {
				argv[a++] = "-o";
				argv[a++] = mo->kernel_opts;
			}
			if (mo->quiet_mode) {
				argv[a++] = "-q";
			}
			argv[a++] = mountpoint;
			argv[a++] = NULL;

			execv(mount_tool_path, (char **)argv);
			fuse_log(FUSE_LOG_ERR,
				 "fuse: failed to exec mount program\n");
			_exit(1);
		}

		_exit(0);
	}

	free(mount_tool_path);

	close(fds[0]);
	fd = receive_fd(fds[1]);

	if (fd != -1 && callback) {
		int res = -1;
		pthread_t mount_wait_thread;

		struct fuse_mount_core_wait_arg *arg =
			calloc(1, sizeof(struct fuse_mount_core_wait_arg));
		arg->fd = fds[1];
		arg->callback = callback;
		arg->context = context;

		res = fuse_start_thread(&mount_wait_thread,
					&fuse_mount_core_wait, (void *)arg);
		if (res) {
			fuse_log(FUSE_LOG_ERR,
				 "fuse: failed to wait for mount status\n");
			goto mount_err_out;
		}

		pthread_detach(mount_wait_thread);
	}

	if (waitpid(pid, &status, 0) == -1 || WEXITSTATUS(status) != 0) {
		fuse_log(FUSE_LOG_ERR, "fuse: failed to mount file system\n");
		goto mount_err_out;
	}

	goto out;

mount_err_out:
	close(fd);
	fd = -1;

out:
	return fd;
}

struct fuse_mount_ext_arg {
	char *mountpoint;
	char *options;
	bool quiet_mode;
	MFChannelRef mfch;
	void (*callback)(void *context, int res);
	void *context;
};

static void *fuse_mount_ext_bg(void *arg)
{
	struct fuse_mount_ext_arg *a = (struct fuse_mount_ext_arg *)arg;
	int res = -1;

	res = MFMount(a->mfch, a->mountpoint, a->options, a->quiet_mode);
	if (a->callback) {
		a->callback(a->context, res);
	}

	free(a->mountpoint);
	free(a->options);
	MFRelease(a->mfch);
	free(a);

	return NULL;
}

static MFChannelRef fuse_mount_ext(const char *mountpoint,
				   struct mount_opts *mo,
				   void (*callback)(void *, int), void *context)
{
	int res = -1;
	struct fuse_mount_ext_arg *arg = NULL;
	pthread_t mount_ext_thread;

	MFChannelRef mfch = MFChannelCreate();
	if (mfch == NULL) {
		return NULL;
	}

	arg = calloc(1, sizeof(struct fuse_mount_ext_arg));
	if (!arg) {
		fuse_log(FUSE_LOG_ERR,
			 "fuse: failed to allocate fuse_mount_ext_arg\n");
		MFRelease(mfch);
		return NULL;
	}

	arg->mountpoint = strdup(mountpoint);
	arg->options = strdup(mo->kernel_opts);
	arg->quiet_mode = mo->quiet_mode;
	arg->mfch = MFRetain(mfch);
	arg->callback = callback;
	arg->context = context;

	if (!arg->mountpoint || !arg->options) {
		fuse_log(FUSE_LOG_ERR,
			 "fuse: failed to initialize fuse_mount_ext_arg\n");
		goto out_free;
	}

	res = fuse_start_thread(&mount_ext_thread,
				&fuse_mount_ext_bg, (void *)arg);
	if (res) {
		fuse_log(FUSE_LOG_ERR, "fuse: failed to mount volume\n");
		goto out_free;
	}

	pthread_detach(mount_ext_thread);
	return mfch;

out_free:
	MFRelease(mfch);
	free(arg->mountpoint);
	free(arg->options);
	MFRelease(arg->mfch);
	free(arg);
	return NULL;
}

struct mount_opts *parse_mount_opts(struct fuse_args *args)
{
	struct mount_opts *mo;

	mo = (struct mount_opts *)malloc(sizeof(struct mount_opts));
	if (mo == NULL)
		return NULL;

	memset(mo, 0, sizeof(struct mount_opts));

	if (args &&
	    fuse_opt_parse(args, mo, fuse_mount_opts,
			   fuse_mount_opt_proc) == -1)
		goto err_out;

	return mo;

err_out:
	destroy_mount_opts(mo);
	return NULL;
}

void destroy_mount_opts(struct mount_opts *mo)
{
	free(mo->backend);
	free(mo->kernel_opts);
	free(mo);
}

MFChannelRef fuse_darwin_mount(const char *mountpoint, struct mount_opts *mo,
			       void (*callback)(void *, int), void *context)
{
	if (mo->allow_other && mo->allow_root) {
		fuse_log(FUSE_LOG_ERR,
			 "fuse: allow_other and allow_root are mutually exclusive\n");
		return NULL;
	}

	if (mo->backend && strcmp(mo->backend, "fskit") == 0) {
		return fuse_mount_ext(mountpoint, mo, callback, context);
	} else {
		int fd;
		MFChannelRef mfch;

		/* Notify mount tool that it is called from lib */
		setenv("_FUSE_CALL_BY_LIB", "1", 1);

		fd = fuse_mount_core(mountpoint, mo, callback, context);
		if (fd < 0)
			return NULL;

		mfch = MFChannelCreateWithDeviceFileDescriptor(fd);
		if (mfch == NULL)
			close(fd);
		return mfch;
	}
}
