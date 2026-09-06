/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU LGPLv2.
  See the file COPYING.LIB
*/

/*
 * Copyright (c) 2017-2026 Benjamin Fleischer
 */

#include "fuse.h"
#include "fuse_lowlevel.h"

#ifdef __APPLE__
#  include <DiskArbitration/DiskArbitration.h>
#  include <MFMount/MFMount.h>
#  include <stdbool.h>
#  include <stdatomic.h>
#endif

struct fuse_chan;
struct fuse_ll;
struct mount_opts;

#ifdef __APPLE__
enum fuse_chan_type {
	FUSE_CHAN_TYPE_FD,
	FUSE_CHAN_TYPE_DARWIN,
};

enum fuse_darwin_mount_state {
	FUSE_DARWIN_MOUNT_NONE,
	FUSE_DARWIN_MOUNT_DELAYED,
	FUSE_DARWIN_MOUNT_MOUNTING,
	FUSE_DARWIN_MOUNT_CONNECTING,
	FUSE_DARWIN_MOUNT_MOUNTED,
	FUSE_DARWIN_MOUNT_UNMOUNTING,
	FUSE_DARWIN_MOUNT_UNMOUNTED,
	FUSE_DARWIN_MOUNT_FAILED,
};
#endif

struct fuse_session {
	struct fuse_session_ops op;

	int (*receive_buf)(struct fuse_session *se, struct fuse_buf *buf,
			   struct fuse_chan **chp);

	void (*process_buf)(void *data, const struct fuse_buf *buf,
			    struct fuse_chan *ch);

	void *data;

	volatile int exited;

#ifdef __APPLE__
	_Atomic bool sig_interrupt;
#endif

	struct fuse_chan *ch;
};

struct fuse_req {
	struct fuse_ll *f;
	uint64_t unique;
	int ctr;
	pthread_mutex_t lock;
	struct fuse_ctx ctx;
	struct fuse_chan *ch;
	int interrupted;
	unsigned int ioctl_64bit : 1;
	union {
		struct {
			uint64_t unique;
		} i;
		struct {
			fuse_interrupt_func_t func;
			void *data;
		} ni;
	} u;
#ifdef __APPLE__
	MFMessageRef mfmsg;
#endif
	struct fuse_req *next;
	struct fuse_req *prev;
};

struct fuse_notify_req {
	uint64_t unique;
	void (*reply)(struct fuse_notify_req *, fuse_req_t, fuse_ino_t,
		      const void *, const struct fuse_buf *);
	struct fuse_notify_req *next;
	struct fuse_notify_req *prev;
};

struct fuse_ll {
	int debug;
	int allow_root;
	int atomic_o_trunc;
	int no_remote_posix_lock;
	int no_remote_flock;
	int big_writes;
	int splice_write;
	int splice_move;
	int splice_read;
	int no_splice_write;
	int no_splice_move;
	int no_splice_read;
	struct fuse_lowlevel_ops op;
	int got_init;
	struct cuse_data *cuse_data;
	void *userdata;
	uid_t owner;
	struct fuse_conn_info conn;
	struct fuse_req list;
	struct fuse_req interrupts;
	pthread_mutex_t lock;
	int got_destroy;
	pthread_key_t pipe_key;
	int broken_splice_nonblock;
	uint64_t notify_ctr;
	struct fuse_notify_req notify_list;
};

struct fuse_cmd {
	char *buf;
	size_t buflen;
	struct fuse_chan *ch;
};

struct fuse *fuse_new_common(struct fuse_chan *ch, struct fuse_args *args,
			     const struct fuse_operations *op,
			     size_t op_size, void *user_data, int compat);

int fuse_sync_compat_args(struct fuse_args *args);

struct fuse_chan *fuse_kern_chan_new(int fd);

#ifdef __APPLE__
void fuse_chan_set_type(struct fuse_chan *ch, enum fuse_chan_type type);
enum fuse_chan_type fuse_chan_get_type(struct fuse_chan *ch);

struct fuse_chan *fuse_darwin_chan_new(const char *mountpoint,
				       struct mount_opts *mo);
int fuse_darwin_chan_mfch(struct fuse_chan *ch, MFChannelRef *mfchp);
void fuse_darwin_chan_interrupt(struct fuse_chan *ch);
void fuse_darwin_chan_unmount(struct fuse_chan *ch);
bool fuse_darwin_chan_not_mounted(struct fuse_chan *ch);

void fuse_darwin_set_mount_started(void);
bool fuse_darwin_mount_started(void);

typedef void (*fuse_darwin_mount_notify_callback_t)(void *context, int status);

/*
 * Wait for the currently pending mounts to complete.
 *
 * The callback is invoked exactly once, only after every captured mount has
 * succeeded, failed, or been canceled. Its status is 0 if every captured mount
 * succeeded, or -1 if any mount failed or was canceled before reaching the
 * mounted state, including cancellation of a delayed mount.
 *
 * The callback may run before this function returns. If no mounts are pending,
 * it is invoked immediately with 0. In case this function returns before the
 * callback is invoked, the caller must continue servicing file system requests
 * for mounting to complete.
 *
 * @param callback callback to invoke
 * @param context caller-defined context passed to @p callback
 * @return 0 if notification setup succeeded, which does not imply that mounting
 * has completed, or -1 on setup failure. On setup failure no callback is
 * invoked and the caller retains responsibility for @p context.
 */
int fuse_darwin_mount_notify(fuse_darwin_mount_notify_callback_t callback,
			     void *context);
#endif

struct fuse_session *fuse_lowlevel_new_common(struct fuse_args *args,
					const struct fuse_lowlevel_ops *op,
					size_t op_size, void *userdata);

#ifndef __APPLE__
void fuse_kern_unmount_compat22(const char *mountpoint);
#endif

#ifdef __APPLE__
void fuse_chan_retain(struct fuse_chan *ch);
void fuse_chan_release(struct fuse_chan *ch);
#endif

int fuse_chan_clearfd(struct fuse_chan *ch);

#ifdef __APPLE__
void fuse_darwin_unmount(DADiskRef disk, DADiskUnmountOptions options,
			 DADiskUnmountCallback callback, void *context);
#else
void fuse_kern_unmount(const char *mountpoint, int fd);
#endif

#ifdef __APPLE__
struct mount_opts *parse_mount_opts(struct fuse_args *args);
void destroy_mount_opts(struct mount_opts *mo);
int fuse_darwin_check_mount_opts(struct mount_opts *mo);
MFChannelRef fuse_darwin_mount(const char *mountpoint, struct mount_opts *mo,
			       void (*callback)(void *, int), void *context);
#else
int fuse_kern_mount(const char *mountpoint, struct fuse_args *args);
#endif

int fuse_send_reply_iov_nofree(fuse_req_t req, int error, struct iovec *iov,
			       int count);
void fuse_free_req(fuse_req_t req);


struct fuse *fuse_setup_common(int argc, char *argv[],
			       const struct fuse_operations *op,
			       size_t op_size,
			       char **mountpoint,
			       int *multithreaded,
			       int *fd,
			       void *user_data,
			       int compat);

void cuse_lowlevel_init(fuse_req_t req, fuse_ino_t nodeide, const void *inarg);

int fuse_start_thread(pthread_t *thread_id, void *(*func)(void *), void *arg);
