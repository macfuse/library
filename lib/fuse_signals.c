/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU LGPLv2.
  See the file COPYING.LIB
*/

/*
 * Copyright (c) 2006-2008 Amit Singh/Google Inc.
 * Copyright (c) 2011-2024 Benjamin Fleischer
 */

#include "fuse_lowlevel.h"

#include <stdio.h>
#include <string.h>
#include <signal.h>

#ifdef __APPLE__
#  include <dispatch/dispatch.h>
#endif

static struct fuse_session *fuse_instance;

#ifdef __APPLE__
static dispatch_queue_t fuse_signal_queue;

static int fuse_unmount_signals[] = { SIGHUP, SIGINT, SIGTERM };
#define fuse_unmount_signals_count() \
	(sizeof(fuse_unmount_signals) / sizeof(fuse_unmount_signals[0]))

static dispatch_source_t fuse_unmount_sources[fuse_unmount_signals_count()];
#endif /* __APPLE__ */

static void exit_handler(int sig)
{
	(void) sig;
	if (fuse_instance) {
#ifdef __APPLE__
		struct fuse_chan *ch = fuse_session_next_chan(fuse_instance,
							      NULL);
		if (ch) {
			/*
			 * Note: The volume will not be unmounted in case the
			 * signal is received before the mount operation has
			 * been completed (and the DADiskRef has been attached
			 * to the channel).
			 */
			fuse_unmount(NULL, ch);
		}
#else /* __APPLE__ */
		fuse_session_exit(fuse_instance);
#endif /* __APPLE__ */
	}
}

#ifdef __APPLE__

static dispatch_source_t register_signal_source(int sig)
{
	dispatch_source_t source =
		dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, sig, 0,
				       fuse_signal_queue);
	if (!source) {
		fprintf(stderr, "fuse: failed to create source for signal %d\n",
			sig);
		return NULL;
	}

	dispatch_source_set_event_handler(source, ^{
		exit_handler(sig);
	});
	dispatch_resume(source);
	return source;
}

__attribute__((constructor))
static void fuse_signal_init(void)
{
	fuse_signal_queue = dispatch_queue_create("fuse_signal_queue",
						  DISPATCH_QUEUE_SERIAL);
}

__attribute__((destructor))
static void fuse_signal_destroy(void)
{
	dispatch_release(fuse_signal_queue);
}

#endif /* __APPLE__ */

static int set_one_signal_handler(int sig, void (*handler)(int), int remove)
{
	struct sigaction sa;
	struct sigaction old_sa;

	memset(&sa, 0, sizeof(struct sigaction));
	sa.sa_handler = remove ? SIG_DFL : handler;
	sigemptyset(&(sa.sa_mask));
	sa.sa_flags = 0;

	if (sigaction(sig, NULL, &old_sa) == -1) {
		perror("fuse: cannot get old signal handler");
		return -1;
	}

	if (old_sa.sa_handler == (remove ? handler : SIG_DFL) &&
	    sigaction(sig, &sa, NULL) == -1) {
		perror("fuse: cannot set signal handler");
		return -1;
	}
	return 0;
}

int fuse_set_signal_handlers(struct fuse_session *se)
{
#ifdef __APPLE__
	for (int i = 0, c = fuse_unmount_signals_count(); i < c; i++) {
		int signal = fuse_unmount_signals[i];
		if (set_one_signal_handler(signal, SIG_IGN, 0) == -1)
			return -1;

		if (fuse_unmount_sources[i]) {
			fprintf(stderr, "fuse: cannot register signal source");
			return -1;
		}
		fuse_unmount_sources[i] = register_signal_source(signal);
	}

	if (set_one_signal_handler(SIGPIPE, SIG_IGN, 0) == -1)
		return -1;
#else /* __APPLE__ */
	if (set_one_signal_handler(SIGHUP, exit_handler, 0) == -1 ||
	    set_one_signal_handler(SIGINT, exit_handler, 0) == -1 ||
	    set_one_signal_handler(SIGTERM, exit_handler, 0) == -1 ||
	    set_one_signal_handler(SIGPIPE, SIG_IGN, 0) == -1)
		return -1;
#endif /* __APPLE__ */

	fuse_instance = se;
	return 0;
}

void fuse_remove_signal_handlers(struct fuse_session *se)
{
	if (fuse_instance != se)
		fprintf(stderr,
			"fuse: fuse_remove_signal_handlers: unknown session\n");
	else
		fuse_instance = NULL;

#ifdef __APPLE__
	for (int i = 0, c = fuse_unmount_signals_count(); i < c; i++) {
		set_one_signal_handler(fuse_unmount_signals[i], SIG_IGN, 1);

		if (fuse_unmount_sources[i] != NULL) {
			dispatch_release(fuse_unmount_sources[i]);
			fuse_unmount_sources[i] = NULL;
		}
	}

	set_one_signal_handler(SIGPIPE, SIG_IGN, 1);
#else /* __APPLE__ */
	set_one_signal_handler(SIGHUP, exit_handler, 1);
	set_one_signal_handler(SIGINT, exit_handler, 1);
	set_one_signal_handler(SIGTERM, exit_handler, 1);
	set_one_signal_handler(SIGPIPE, SIG_IGN, 1);
#endif /* __APPLE__ */
}
