/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU LGPLv2.
  See the file COPYING.LIB.
*/

/*
 * Copyright (c) 2006-2008 Amit Singh/Google Inc.
 * Copyright (c) 2011-2024 Benjamin Fleischer
 */

#include "config.h"
#include "fuse_i.h"
#include "fuse_misc.h"
#include "fuse_opt.h"
#include "fuse_lowlevel.h"
#include "fuse_common_compat.h"
#ifdef __APPLE__
#  include "fuse_darwin.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <sys/param.h>

#ifdef __APPLE__
#  include <CoreFoundation/CoreFoundation.h>
#  include <DiskArbitration/DiskArbitration.h>
#endif

enum  {
	KEY_HELP,
	KEY_HELP_NOHEADER,
	KEY_VERSION,
};

struct helper_opts {
	int foreground;
	int singlethread;
	char *loop;
	int nodefault_subtype;
	char *mountpoint;
};

#define FUSE_HELPER_OPT(t, p) { t, offsetof(struct helper_opts, p), 1 }

static const struct fuse_opt fuse_helper_opts[] = {
	FUSE_HELPER_OPT("-d",		foreground),
	FUSE_HELPER_OPT("debug",	foreground),
	FUSE_HELPER_OPT("-f",		foreground),
	FUSE_HELPER_OPT("-s",		singlethread),
	FUSE_HELPER_OPT("loop=%s", 	loop),
	FUSE_HELPER_OPT("fsname=",	nodefault_subtype),
	FUSE_HELPER_OPT("subtype=",	nodefault_subtype),

	FUSE_OPT_KEY("-h",		KEY_HELP),
	FUSE_OPT_KEY("--help",		KEY_HELP),
	FUSE_OPT_KEY("-ho",		KEY_HELP_NOHEADER),
	FUSE_OPT_KEY("-V",		KEY_VERSION),
	FUSE_OPT_KEY("--version",	KEY_VERSION),
	FUSE_OPT_KEY("-d",		FUSE_OPT_KEY_KEEP),
	FUSE_OPT_KEY("debug",		FUSE_OPT_KEY_KEEP),
	FUSE_OPT_KEY("fsname=",		FUSE_OPT_KEY_KEEP),
	FUSE_OPT_KEY("subtype=",	FUSE_OPT_KEY_KEEP),
	FUSE_OPT_END
};

static void usage(const char *progname)
{
	fprintf(stderr,
		"usage: %s mountpoint [options]\n\n", progname);
	fprintf(stderr,
		"general options:\n"
		"    -o opt,[opt...]        mount options\n"
		"    -h   --help            print help\n"
		"    -V   --version         print version\n"
		"\n");
}

static void helper_help(void)
{
	fprintf(stderr,
		"FUSE options:\n"
		"    -d   -o debug          enable debug output (implies -f)\n"
		"    -f                     foreground operation\n"
		"    -s                     disable multi-threaded operation\n"
		"\n"
		);
}

static void helper_version(void)
{
	fprintf(stderr, "FUSE library version: %s\n", PACKAGE_VERSION);
}

static int fuse_helper_opt_proc(void *data, const char *arg, int key,
				struct fuse_args *outargs)
{
	struct helper_opts *hopts = data;

	switch (key) {
	case KEY_HELP:
		usage(outargs->argv[0]);
		/* fall through */

	case KEY_HELP_NOHEADER:
		helper_help();
		return fuse_opt_add_arg(outargs, "-h");

	case KEY_VERSION:
		helper_version();
		return 1;

	case FUSE_OPT_KEY_NONOPT:
		if (!hopts->mountpoint) {
			char mountpoint[PATH_MAX];
			if (realpath(arg, mountpoint) == NULL) {
#ifdef __APPLE__
				return fuse_opt_add_opt(&hopts->mountpoint,
							arg);
#else /* !__APPLE__ */
				fprintf(stderr,
					"fuse: bad mount point `%s': %s\n",
					arg, strerror(errno));
				return -1;
#endif /* !__APPLE__ */
			}
			return fuse_opt_add_opt(&hopts->mountpoint, mountpoint);
		} else {
			fprintf(stderr, "fuse: invalid argument `%s'\n", arg);
			return -1;
		}

	default:
		return 1;
	}
}

static int add_default_subtype(const char *progname, struct fuse_args *args)
{
	int res;
	char *subtype_opt;
	const char *basename = strrchr(progname, '/');
	if (basename == NULL)
		basename = progname;
	else if (basename[1] != '\0')
		basename++;

	subtype_opt = (char *) malloc(strlen(basename) + 64);
	if (subtype_opt == NULL) {
		fprintf(stderr, "fuse: memory allocation failed\n");
		return -1;
	}
	sprintf(subtype_opt, "-osubtype=%s", basename);
	res = fuse_opt_add_arg(args, subtype_opt);
	free(subtype_opt);
	return res;
}

int fuse_parse_cmdline(struct fuse_args *args, char **mountpoint,
		       int *multithreaded, int *foreground)
{
	int res;
	struct helper_opts hopts;

	memset(&hopts, 0, sizeof(hopts));
	res = fuse_opt_parse(args, &hopts, fuse_helper_opts,
			     fuse_helper_opt_proc);
	if (res == -1)
		return -1;

	if (!hopts.nodefault_subtype) {
		res = add_default_subtype(args->argv[0], args);
		if (res == -1)
			goto err;
	}
	if (mountpoint)
		*mountpoint = hopts.mountpoint;
	else
		free(hopts.mountpoint);

	if (multithreaded) {
		if (hopts.singlethread)
			*multithreaded = FUSE_LOOP_SINGLE_THREADED;
		else if (hopts.loop) {
			if (strcmp(hopts.loop, "single_threaded") == 0)
				*multithreaded = FUSE_LOOP_SINGLE_THREADED;
			else if (strcmp(hopts.loop, "multi_threaded") == 0)
				*multithreaded = FUSE_LOOP_MULTI_THREADED;
			else if (strcmp(hopts.loop, "dispatch") == 0)
				*multithreaded = FUSE_LOOP_DISPATCH;
			else {
				fprintf(stderr, "fuse: invalid option loop\n");
				goto err;
			}
		} else
			*multithreaded = FUSE_LOOP_MULTI_THREADED;
	}
	if (foreground)
		*foreground = hopts.foreground;
	return 0;

err:
	free(hopts.loop);
	free(hopts.mountpoint);
	return -1;
}

int fuse_daemonize(int foreground)
{
	if (!foreground) {
		int nullfd;
		int waiter[2];
		char completed;

		if (pipe(waiter)) {
			perror("fuse_daemonize: pipe");
			return -1;
		}

		/*
		 * demonize current process by forking it and killing the
		 * parent.  This makes current process as a child of 'init'.
		 */
		switch(fork()) {
		case -1:
			perror("fuse_daemonize: fork");
			return -1;
		case 0:
			break;
		default:
			read(waiter[0], &completed, sizeof(completed));
			_exit(0);
		}

		if (setsid() == -1) {
			perror("fuse_daemonize: setsid");
			return -1;
		}

		(void) chdir("/");

		nullfd = open("/dev/null", O_RDWR, 0);
		if (nullfd != -1) {
			(void) dup2(nullfd, 0);
			(void) dup2(nullfd, 1);
			(void) dup2(nullfd, 2);
			if (nullfd > 2)
				close(nullfd);
		}

		/* Propagate completion of daemon initializatation */
		completed = 1;
		write(waiter[1], &completed, sizeof(completed));
		close(waiter[0]);
		close(waiter[1]);
	}
	return 0;
}

#ifdef __APPLE__

static DASessionRef fuse_dasession;

__attribute__((constructor))
static void fuse_dasession_init(void)
{
	fuse_dasession = DASessionCreate(NULL);
}

__attribute__((destructor))
static void fuse_dasession_destroy(void)
{
	CFRelease(fuse_dasession);
}

struct fuse_mount_context {
	pthread_mutex_t lock;
	char mountpoint[MAXPATHLEN];
	struct fuse_chan *ch;
};

static struct fuse_mount_context *fuse_mount_context_new(const char *mountpoint)
{
	struct fuse_mount_context *mc =
		calloc(1, sizeof(struct fuse_mount_context));
	pthread_mutex_init(&mc->lock, NULL);
	strncpy(mc->mountpoint, mountpoint, sizeof(mc->mountpoint) - 1);
	return mc;
}

static void fuse_mount_context_destroy(struct fuse_mount_context *mc)
{
	pthread_mutex_destroy(&mc->lock);
	if (mc->ch)
		fuse_chan_release(mc->ch);
	free(mc);
}

/*
 * status codes:
 * -1   => unknown error, assume mount(2) failed
 * 0    => mount operation completed succesful
 * > 0  => error code returned by mount(2)
 */
static void fuse_mount_callback(void *context, int status)
{
	struct fuse_mount_context *mc = (struct fuse_mount_context *)context;
	CFURLRef url = NULL;
	DADiskRef disk = NULL;

	pthread_mutex_lock(&mc->lock);

	if (status != 0) {
		fprintf(stderr, "fuse: mount failed with error: %d\n", status);
		goto out;
	}

	url = CFURLCreateFromFileSystemRepresentation(
		NULL, (const UInt8 *)mc->mountpoint, strlen(mc->mountpoint),
		TRUE);
	disk = DADiskCreateFromVolumePath(NULL, fuse_dasession, url);
	CFRelease(url);

	if (disk) {
		fuse_chan_set_disk(mc->ch, disk);
		CFRelease(disk);
	}

out:
	pthread_mutex_unlock(&mc->lock);
	fuse_mount_context_destroy(mc);
}

#endif /* __APPLE__ */

static struct fuse_chan *fuse_mount_common(const char *mountpoint,
					   struct fuse_args *args)
{
	struct fuse_chan *ch;
	int fd;
#ifdef __APPLE__
	struct fuse_mount_context *mc = fuse_mount_context_new(mountpoint);
#endif /* __APPLE__ */

	/*
	 * Make sure file descriptors 0, 1 and 2 are open, otherwise chaos
	 * would ensue.
	 */
	do {
		fd = open("/dev/null", O_RDWR);
		if (fd > 2)
			close(fd);
	} while (fd >= 0 && fd <= 2);

#ifdef __APPLE__
	pthread_mutex_lock(&mc->lock);

	fd = fuse_kern_mount(mountpoint, args, &fuse_mount_callback, mc);
	if (fd == -1) {
		pthread_mutex_unlock(&mc->lock);

		/* fuse_mount_callback() is not going to be called */
		fuse_mount_context_destroy(mc);
		return NULL;
	}

	ch = fuse_kern_chan_new(fd);
	if (ch) {
		fuse_chan_retain(ch);
		mc->ch = ch;
	} else {
		/*
		 * Note: There is no DADiskRef we could pass to unmount because
		 * the asynchronous mount operation has not been completed, yet.
		 * However, we need to make sure fd is closed. As a result the
		 * mount operation will fail.
		 */
		fuse_kern_unmount(NULL, fd);
	}

	pthread_mutex_unlock(&mc->lock);
#else /* __APPLE__ */
	fd = fuse_mount_compat25(mountpoint, args);
	if (fd == -1)
		return NULL;

	ch = fuse_kern_chan_new(fd);
	if (!ch)
		fuse_kern_unmount(mountpoint, fd);
#endif

	return ch;
}

struct fuse_chan *fuse_mount(const char *mountpoint, struct fuse_args *args)
{
	return fuse_mount_common(mountpoint, args);
}

static void fuse_unmount_common(const char *mountpoint, struct fuse_chan *ch)
{
#ifdef __APPLE__
	/*
	 * Note: On macOS we ignore the passed in mountpoint. Once mount(2)
	 * completes, we attach a DADiskRef of our volume to the channel.
	 */
	if (ch) {
		/* fuse_chan_disk() returns retained DADiskRef */
		DADiskRef disk = fuse_chan_disk(ch);

		fuse_kern_unmount(disk, fuse_chan_fd(ch));

		if (disk) {
			CFRelease(disk);
		} else {
			/* Volume not mounted, destroy the channel */
			fuse_chan_destroy(ch);
		}
	}
#else /* __APPLE__ */
	if (mountpoint) {
		int fd = ch ? fuse_chan_clearfd(ch) : -1;
		fuse_kern_unmount(mountpoint, fd);
		if (ch)
			fuse_chan_destroy(ch);
	}
#endif /* __APPLE__ */
}

void fuse_unmount(const char *mountpoint, struct fuse_chan *ch)
{
	fuse_unmount_common(mountpoint, ch);
}

struct fuse *fuse_setup_common(int argc, char *argv[],
			       const struct fuse_operations *op,
			       size_t op_size,
			       char **mountpoint,
			       int *multithreaded,
			       int *fd,
			       void *user_data,
			       int compat)
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct fuse_chan *ch;
	struct fuse *fuse;
	int foreground;
	int res;

	res = fuse_parse_cmdline(&args, mountpoint, multithreaded, &foreground);
	if (res == -1)
		return NULL;

#ifdef __APPLE__
	if (!*mountpoint) {
		fprintf(stderr, "fuse: no mount point\n");
		return NULL;
	}
#endif

	ch = fuse_mount_common(*mountpoint, &args);
	if (!ch) {
		fuse_opt_free_args(&args);
		goto err_free;
	}

	fuse = fuse_new_common(ch, &args, op, op_size, user_data, compat);
	fuse_opt_free_args(&args);
	if (fuse == NULL)
		goto err_unmount;

	res = fuse_daemonize(foreground);
	if (res == -1)
		goto err_unmount;

	res = fuse_set_signal_handlers(fuse_get_session(fuse));
	if (res == -1)
		goto err_unmount;

	if (fd)
		*fd = fuse_chan_fd(ch);

	return fuse;

err_unmount:
	fuse_unmount_common(*mountpoint, ch);
	if (fuse)
		fuse_destroy(fuse);
err_free:
	free(*mountpoint);
	return NULL;
}

struct fuse *fuse_setup(int argc, char *argv[],
			const struct fuse_operations *op, size_t op_size,
			char **mountpoint, int *multithreaded, void *user_data)
{
	return fuse_setup_common(argc, argv, op, op_size, mountpoint,
				 multithreaded, NULL, user_data, 0);
}

static void fuse_teardown_common(struct fuse *fuse, char *mountpoint)
{
	struct fuse_session *se = fuse_get_session(fuse);
	struct fuse_chan *ch = fuse_session_next_chan(se, NULL);
	fuse_remove_signal_handlers(se);
	fuse_unmount_common(mountpoint, ch);
	fuse_destroy(fuse);
	free(mountpoint);
}

void fuse_teardown(struct fuse *fuse, char *mountpoint)
{
	fuse_teardown_common(fuse, mountpoint);
}

static int fuse_main_common(int argc, char *argv[],
			    const struct fuse_operations *op, size_t op_size,
			    void *user_data, int compat)
{
	struct fuse *fuse;
	char *mountpoint;
	int multithreaded;
	int res;

	fuse = fuse_setup_common(argc, argv, op, op_size, &mountpoint,
				 &multithreaded, NULL, user_data, compat);
	if (fuse == NULL)
		return 1;

	if (multithreaded == FUSE_LOOP_SINGLE_THREADED)
		res = fuse_loop(fuse);
	else if (multithreaded == FUSE_LOOP_MULTI_THREADED)
		res = fuse_loop_mt(fuse);
	else if (multithreaded == FUSE_LOOP_DISPATCH)
		res = fuse_loop_dispatch(fuse);
	else
		return 1;

	fuse_teardown_common(fuse, mountpoint);
	if (res == -1)
		return 1;

	return 0;
}

int fuse_main_real(int argc, char *argv[], const struct fuse_operations *op,
		   size_t op_size, void *user_data)
{
	return fuse_main_common(argc, argv, op, op_size, user_data, 0);
}

#undef fuse_main
int fuse_main(void);
int fuse_main(void)
{
	fprintf(stderr, "fuse_main(): This function does not exist\n");
	return -1;
}

int fuse_version(void)
{
	return FUSE_VERSION;
}

#include "fuse_compat.h"

#if !defined(__FreeBSD__) && !defined(__NetBSD__) && !defined(__APPLE__)

struct fuse *fuse_setup_compat22(int argc, char *argv[],
				 const struct fuse_operations_compat22 *op,
				 size_t op_size, char **mountpoint,
				 int *multithreaded, int *fd)
{
	return fuse_setup_common(argc, argv, (struct fuse_operations *) op,
				 op_size, mountpoint, multithreaded, fd, NULL,
				 22);
}

struct fuse *fuse_setup_compat2(int argc, char *argv[],
				const struct fuse_operations_compat2 *op,
				char **mountpoint, int *multithreaded,
				int *fd)
{
	return fuse_setup_common(argc, argv, (struct fuse_operations *) op,
				 sizeof(struct fuse_operations_compat2),
				 mountpoint, multithreaded, fd, NULL, 21);
}

int fuse_main_real_compat22(int argc, char *argv[],
			    const struct fuse_operations_compat22 *op,
			    size_t op_size)
{
	return fuse_main_common(argc, argv, (struct fuse_operations *) op,
				op_size, NULL, 22);
}

void fuse_main_compat1(int argc, char *argv[],
		       const struct fuse_operations_compat1 *op)
{
	fuse_main_common(argc, argv, (struct fuse_operations *) op,
			 sizeof(struct fuse_operations_compat1), NULL, 11);
}

int fuse_main_compat2(int argc, char *argv[],
		      const struct fuse_operations_compat2 *op)
{
	return fuse_main_common(argc, argv, (struct fuse_operations *) op,
				sizeof(struct fuse_operations_compat2), NULL,
				21);
}

int fuse_mount_compat1(const char *mountpoint, const char *args[])
{
	/* just ignore mount args for now */
	(void) args;
	return fuse_mount_compat22(mountpoint, NULL);
}

FUSE_SYMVER(".symver fuse_setup_compat2,__fuse_setup@");
FUSE_SYMVER(".symver fuse_setup_compat22,fuse_setup@FUSE_2.2");
FUSE_SYMVER(".symver fuse_teardown,__fuse_teardown@");
FUSE_SYMVER(".symver fuse_main_compat2,fuse_main@");
FUSE_SYMVER(".symver fuse_main_real_compat22,fuse_main_real@FUSE_2.2");

#endif /* __FreeBSD__ || __NetBSD__ || __APPLE__ */


struct fuse *fuse_setup_compat25(int argc, char *argv[],
				 const struct fuse_operations_compat25 *op,
				 size_t op_size, char **mountpoint,
				 int *multithreaded, int *fd)
{
	return fuse_setup_common(argc, argv, (struct fuse_operations *) op,
				 op_size, mountpoint, multithreaded, fd, NULL,
				 25);
}

int fuse_main_real_compat25(int argc, char *argv[],
			    const struct fuse_operations_compat25 *op,
			    size_t op_size)
{
	return fuse_main_common(argc, argv, (struct fuse_operations *) op,
				op_size, NULL, 25);
}

void fuse_teardown_compat22(struct fuse *fuse, int fd, char *mountpoint)
{
	(void) fd;
	fuse_teardown_common(fuse, mountpoint);
}

int fuse_mount_compat25(const char *mountpoint, struct fuse_args *args)
{
#ifdef __APPLE__
	return fuse_kern_mount(mountpoint, args, NULL, NULL);
#else
	return fuse_kern_mount(mountpoint, args);
#endif
}

FUSE_SYMVER(".symver fuse_setup_compat25,fuse_setup@FUSE_2.5");
#ifndef __APPLE__
FUSE_SYMVER(".symver fuse_teardown_compat22,fuse_teardown@FUSE_2.2");
#endif
FUSE_SYMVER(".symver fuse_main_real_compat25,fuse_main_real@FUSE_2.5");
FUSE_SYMVER(".symver fuse_mount_compat25,fuse_mount@FUSE_2.5");
