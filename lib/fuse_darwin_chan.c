/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU LGPLv2.
  See the file COPYING.LIB
*/

/*
 * Copyright (c) 2006-2008 Amit Singh/Google Inc.
 * Copyright (c) 2011-2026 Benjamin Fleischer
 */

#ifdef __APPLE__

#include "fuse_lowlevel.h"
#include "fuse_kernel.h"
#include "fuse_i.h"
#include "fuse_darwin.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <DiskArbitration/DiskArbitration.h>
#include <MFMount/MFMount.h>

struct fuse_darwin_chan_data {
	pthread_mutex_t lock;
	DADiskRef disk;
	enum fuse_darwin_mount_state mount_state;
	pthread_cond_t mount_cond;
	pthread_mutex_t mount_lock;
	char *mountpoint;
	struct mount_opts *mount_opts;
	MFChannelRef mfch;
	bool mfch_closed;
};

struct fuse_darwin_chan_mount_context {
	char *mountpoint;
	struct fuse_chan *ch;
};

static pthread_mutex_t fuse_darwin_dasession_lock = PTHREAD_MUTEX_INITIALIZER;
static DASessionRef fuse_darwin_dasession;

static DASessionRef fuse_darwin_copy_dasession(void)
{
	DASessionRef session = NULL;

	pthread_mutex_lock(&fuse_darwin_dasession_lock);
	if (fuse_darwin_dasession == NULL)
		fuse_darwin_dasession = DASessionCreate(NULL);
	if (fuse_darwin_dasession != NULL)
		session = (DASessionRef)CFRetain(fuse_darwin_dasession);
	pthread_mutex_unlock(&fuse_darwin_dasession_lock);

	return session;
}

__attribute__((destructor))
static void fuse_darwin_dasession_destroy(void)
{
	DASessionRef session;

	pthread_mutex_lock(&fuse_darwin_dasession_lock);
	session = fuse_darwin_dasession;
	fuse_darwin_dasession = NULL;
	pthread_mutex_unlock(&fuse_darwin_dasession_lock);

	if (session != NULL)
		CFRelease(session);
}

static struct fuse_darwin_chan_data *
fuse_darwin_chan_data(struct fuse_chan *ch)
{
	if (ch == NULL || fuse_chan_get_type(ch) != FUSE_CHAN_TYPE_DARWIN)
		return NULL;

	return (struct fuse_darwin_chan_data *)fuse_chan_data(ch);
}

static void fuse_darwin_chan_set_disk(struct fuse_chan *ch, DADiskRef disk)
{
	struct fuse_darwin_chan_data *dch = fuse_darwin_chan_data(ch);
	DADiskRef old;

	if (dch == NULL)
		return;

	if (disk != NULL)
		CFRetain(disk);

	pthread_mutex_lock(&dch->lock);
	old = dch->disk;
	dch->disk = disk;
	pthread_mutex_unlock(&dch->lock);

	if (old != NULL)
		CFRelease(old);
}

DADiskRef fuse_chan_disk(struct fuse_chan *ch)
{
	struct fuse_darwin_chan_data *dch = fuse_darwin_chan_data(ch);
	DADiskRef disk = NULL;

	if (dch == NULL)
		return NULL;

	pthread_mutex_lock(&dch->lock);
	disk = dch->disk;
	if (disk != NULL)
		CFRetain(disk);
	pthread_mutex_unlock(&dch->lock);

	return disk;
}

void fuse_chan_cleardisk(struct fuse_chan *ch)
{
	fuse_darwin_chan_set_disk(ch, NULL);
}

static struct fuse_darwin_chan_mount_context *
fuse_darwin_chan_mount_context_new(const char *mountpoint,
				   struct fuse_chan *ch)
{
	struct fuse_darwin_chan_mount_context *mc;

	mc = calloc(1, sizeof(struct fuse_darwin_chan_mount_context));
	if (mc == NULL)
		return NULL;

	mc->mountpoint = strdup(mountpoint);
	if (mc->mountpoint == NULL) {
		free(mc);
		return NULL;
	}

	fuse_chan_retain(ch);
	mc->ch = ch;
	return mc;
}

static void
fuse_darwin_chan_mount_context_destroy(
	struct fuse_darwin_chan_mount_context *mc)
{
	free(mc->mountpoint);
	if (mc->ch != NULL)
		fuse_chan_release(mc->ch);
	free(mc);
}

static void fuse_darwin_chan_close(struct fuse_chan *ch)
{
	struct fuse_darwin_chan_data *dch = fuse_darwin_chan_data(ch);
	MFChannelRef mfch = NULL;

	if (dch == NULL)
		return;

	pthread_mutex_lock(&dch->lock);
	if (dch->mfch != NULL && !dch->mfch_closed) {
		mfch = dch->mfch;
		dch->mfch_closed = true;
	}
	pthread_mutex_unlock(&dch->lock);

	if (mfch != NULL)
		MFChannelClose(mfch);
}

void fuse_darwin_chan_interrupt(struct fuse_chan *ch)
{
	struct fuse_darwin_chan_data *dch = fuse_darwin_chan_data(ch);
	MFChannelRef mfch = NULL;

	if (dch == NULL)
		return;

	pthread_mutex_lock(&dch->lock);
	if (dch->mfch != NULL && !dch->mfch_closed)
		mfch = (MFChannelRef)MFRetain(dch->mfch);
	pthread_mutex_unlock(&dch->lock);

	if (mfch != NULL) {
		(void)MFChannelInterrupt(mfch);
		MFRelease(mfch);
	}
}

/*
 * status codes:
 * -1   => unknown error, assume mount(2) failed
 * 0    => mount operation completed successful
 * > 0  => error code returned by mount(2)
 */
static void fuse_darwin_chan_mount_callback(void *context, int status)
{
	struct fuse_darwin_chan_mount_context *mc;
	struct fuse_darwin_chan_data *dch;
	DASessionRef dasession = NULL;
	CFURLRef url = NULL;
	DADiskRef disk = NULL;

	mc = (struct fuse_darwin_chan_mount_context *)context;
	dch = fuse_darwin_chan_data(mc->ch);
	assert(dch != NULL);

	if (status != 0) {
		fprintf(stderr, "fuse: mount failed with error: %d\n",
			status);
		goto state;
	}

	dasession = fuse_darwin_copy_dasession();
	if (dasession == NULL) {
		fprintf(stderr, "fuse: failed to create DASessionRef\n");
		goto state;
	}

	url = CFURLCreateFromFileSystemRepresentation(
		NULL, (const UInt8 *)mc->mountpoint, strlen(mc->mountpoint),
		TRUE);
	if (url == NULL) {
		fprintf(stderr, "fuse: failed to create CFURLRef\n");
		goto state;
	}

	disk = DADiskCreateFromVolumePath(NULL, dasession, url);
	if (disk == NULL) {
		fprintf(stderr, "fuse: failed to create DADiskRef\n");
		goto state;
	}

state:
	pthread_mutex_lock(&dch->mount_lock);
	while (true) {
		switch (dch->mount_state) {
		case FUSE_DARWIN_MOUNT_NONE:
		case FUSE_DARWIN_MOUNT_DELAYED:
		case FUSE_DARWIN_MOUNT_MOUNTED:
		case FUSE_DARWIN_MOUNT_UNMOUNTING:
		case FUSE_DARWIN_MOUNT_FAILED:
			pthread_mutex_unlock(&dch->mount_lock);
			fprintf(stderr, "fuse: illegal mount state\n");
			abort();

		case FUSE_DARWIN_MOUNT_UNMOUNTED:
			pthread_mutex_unlock(&dch->mount_lock);

			if (disk != NULL) {
				/*
				 * fuse_unmount() was called, and the session
				 * exited while the volume was still in the
				 * process of being mounted. We need to unmount
				 * the volume.
				 */
				fuse_darwin_unmount(disk,
						    kDADiskUnmountOptionForce,
						    NULL, NULL);
				CFRelease(disk);
			}
			goto out;

		case FUSE_DARWIN_MOUNT_MOUNTING:
			/*
			 * The callback may run before the channel has been
			 * registered with the session. Wait for the mount state
			 * to settle.
			 */
			pthread_cond_wait(&dch->mount_cond, &dch->mount_lock);
			break;

		case FUSE_DARWIN_MOUNT_CONNECTING:
			if (disk == NULL) {
				dch->mount_state = FUSE_DARWIN_MOUNT_FAILED;
				pthread_cond_broadcast(&dch->mount_cond);
				pthread_mutex_unlock(&dch->mount_lock);

				fuse_darwin_chan_close(mc->ch);
			} else {
				fuse_darwin_chan_set_disk(mc->ch, disk);
				CFRelease(disk);

				dch->mount_state = FUSE_DARWIN_MOUNT_MOUNTED;
				pthread_cond_broadcast(&dch->mount_cond);
				pthread_mutex_unlock(&dch->mount_lock);
			}
			goto out;
		}
	}

out:
	if (url != NULL)
		CFRelease(url);
	if (dasession != NULL)
		CFRelease(dasession);

	fuse_darwin_chan_mount_context_destroy(mc);
}

static MFChannelRef fuse_darwin_chan_mount_real(struct fuse_chan *ch)
{
	struct fuse_darwin_chan_data *dch = fuse_darwin_chan_data(ch);
	MFChannelRef mfch = NULL;
	struct fuse_darwin_chan_mount_context *mc = NULL;
	char *mountpoint = NULL;
	int err = 0;

	assert(dch != NULL);

	pthread_mutex_lock(&dch->mount_lock);
	if (dch->mountpoint == NULL) {
		err = EINVAL;
	} else {
		mountpoint = strdup(dch->mountpoint);
		if (mountpoint == NULL)
			err = errno != 0 ? errno : ENOMEM;
	}
	pthread_mutex_unlock(&dch->mount_lock);

	if (err != 0)
		goto out;

	mc = fuse_darwin_chan_mount_context_new(mountpoint, ch);
	if (mc == NULL) {
		err = errno != 0 ? errno : ENOMEM;
		fprintf(stderr, "fuse: failed to allocate mount context\n");
		goto out;
	}

	mfch = fuse_darwin_mount(mountpoint, dch->mount_opts,
				 &fuse_darwin_chan_mount_callback, mc);
	if (mfch == NULL) {
		err = ENODEV;
		fuse_darwin_chan_mount_context_destroy(mc);
		goto out;
	}

out:
	free(mountpoint);
	if (err != 0)
		errno = err;
	return mfch;
}

int fuse_darwin_chan_mfch(struct fuse_chan *ch, MFChannelRef *mfchp)
{
	int err = 0;
	struct fuse_darwin_chan_data *dch = fuse_darwin_chan_data(ch);
	MFChannelRef mfch = NULL;

	assert(mfchp != NULL);
	*mfchp = NULL;

	if (dch == NULL)
		return 0;

	pthread_mutex_lock(&dch->mount_lock);
	while (true) {
		switch (dch->mount_state) {
		case FUSE_DARWIN_MOUNT_NONE:
			pthread_mutex_unlock(&dch->mount_lock);
			return 0;

		case FUSE_DARWIN_MOUNT_DELAYED:
			fuse_darwin_set_mount_started();
			dch->mount_state = FUSE_DARWIN_MOUNT_MOUNTING;
			pthread_cond_broadcast(&dch->mount_cond);
			pthread_mutex_unlock(&dch->mount_lock);

			mfch = fuse_darwin_chan_mount_real(ch);
			if (mfch == NULL)
				err = errno != 0 ? errno : ENODEV;

			pthread_mutex_lock(&dch->mount_lock);
			if (dch->mount_state != FUSE_DARWIN_MOUNT_MOUNTING) {
				pthread_mutex_unlock(&dch->mount_lock);
				if (mfch != NULL) {
					MFChannelClose(mfch);
					MFRelease(mfch);
				}
				pthread_mutex_lock(&dch->mount_lock);
				break;
			}
			if (mfch == NULL) {
				dch->mount_state = FUSE_DARWIN_MOUNT_FAILED;
				pthread_cond_broadcast(&dch->mount_cond);
				pthread_mutex_unlock(&dch->mount_lock);
				return -err;
			}

			pthread_mutex_lock(&dch->lock);
			dch->mfch = mfch;
			dch->mfch_closed = false;
			pthread_mutex_unlock(&dch->lock);

			dch->mount_state = FUSE_DARWIN_MOUNT_CONNECTING;
			pthread_cond_broadcast(&dch->mount_cond);
			break;

		case FUSE_DARWIN_MOUNT_MOUNTING:
			pthread_cond_wait(&dch->mount_cond, &dch->mount_lock);
			break;

		case FUSE_DARWIN_MOUNT_CONNECTING:
		case FUSE_DARWIN_MOUNT_MOUNTED:
		case FUSE_DARWIN_MOUNT_UNMOUNTING:
			pthread_mutex_unlock(&dch->mount_lock);
			pthread_mutex_lock(&dch->lock);
			*mfchp = dch->mfch;
			pthread_mutex_unlock(&dch->lock);
			return 0;

		case FUSE_DARWIN_MOUNT_FAILED:
		case FUSE_DARWIN_MOUNT_UNMOUNTED:
			pthread_mutex_unlock(&dch->mount_lock);
			return -EBADF;
		}
	}
}

static void fuse_darwin_chan_unmount_callback(DADiskRef disk,
					      DADissenterRef dissenter,
					      void *context)
{
	(void)disk;
	DAReturn *unmount_status = (DAReturn *)context;

	if (dissenter == NULL)
		*unmount_status = kDAReturnSuccess;
	else
		*unmount_status = DADissenterGetStatus(dissenter);

	CFRunLoopStop(CFRunLoopGetCurrent());
}

static bool fuse_darwin_chan_unmount_real(struct fuse_chan *ch, bool force)
{
	struct fuse_darwin_chan_data *dch = fuse_darwin_chan_data(ch);
	DADiskRef disk;
	DASessionRef dasession = NULL;
	DADiskUnmountOptions unmount_opts = kDADiskUnmountOptionDefault;
	DAReturn unmount_status = kDAReturnError;

	assert(dch != NULL);

	if (force) {
		/*
		 * The session exited, incoming messages will no longer be
		 * processed. A graceful unmount is no longer possible. Let the
		 * backend know that the server is gone.
		 */
		unmount_opts |= kDADiskUnmountOptionForce;
		fuse_darwin_chan_close(ch);
	}

	pthread_mutex_lock(&dch->lock);
	disk = dch->disk;
	dch->disk = NULL;
	pthread_mutex_unlock(&dch->lock);

	if (disk == NULL)
		return true;

	dasession = fuse_darwin_copy_dasession();
	if (dasession == NULL) {
		fprintf(stderr, "fuse: failed to create DASessionRef\n");
		goto err_out;
	}

	DASessionScheduleWithRunLoop(dasession, CFRunLoopGetCurrent(),
				     kCFRunLoopDefaultMode);
	fuse_darwin_unmount(disk, unmount_opts,
			    fuse_darwin_chan_unmount_callback,
			    &unmount_status);
	CFRunLoopRun();
	DASessionUnscheduleFromRunLoop(dasession, CFRunLoopGetCurrent(),
				       kCFRunLoopDefaultMode);
	CFRelease(dasession);

	if (unmount_status != kDAReturnSuccess) {
		fprintf(stderr, "fuse: failed to unmount DADiskRef\n");
		goto err_out;
	}

	fuse_darwin_chan_close(ch);
	CFRelease(disk);
	return true;

err_out:
	if (force) {
		CFRelease(disk);
		return true;
	} else {
		fuse_darwin_chan_set_disk(ch, disk);
		CFRelease(disk);
		return false;
	}
}

static bool fuse_darwin_chan_needs_forced_unmount(struct fuse_chan *ch)
{
	struct fuse_session *se = fuse_chan_session(ch);

	if (se == NULL)
		return true; /* No session can service teardown requests. */
	if (se->op.exited)
		return se->op.exited(se->data);
	return se->exited;
}

static void *fuse_darwin_chan_unmount_thread(void *data)
{
	struct fuse_chan *ch = (struct fuse_chan *)data;
	struct fuse_darwin_chan_data *dch = fuse_darwin_chan_data(ch);
	int res;
	bool force;
	struct timespec timeout;
	char *mountpoint = NULL;
	bool unmounted;

	if (dch == NULL)
		goto out;

	force = fuse_darwin_chan_needs_forced_unmount(ch);

	pthread_mutex_lock(&dch->mount_lock);
	while (true) {
		switch (dch->mount_state) {
		case FUSE_DARWIN_MOUNT_NONE:
		case FUSE_DARWIN_MOUNT_FAILED:
		case FUSE_DARWIN_MOUNT_UNMOUNTED:
			mountpoint = dch->mountpoint;
			dch->mountpoint = NULL;
			pthread_mutex_unlock(&dch->mount_lock);
			goto out;

		case FUSE_DARWIN_MOUNT_DELAYED:
			dch->mount_state = FUSE_DARWIN_MOUNT_UNMOUNTED;
			pthread_cond_broadcast(&dch->mount_cond);
			mountpoint = dch->mountpoint;
			dch->mountpoint = NULL;
			pthread_mutex_unlock(&dch->mount_lock);
			goto out;

		case FUSE_DARWIN_MOUNT_MOUNTING:
			pthread_cond_wait(&dch->mount_cond, &dch->mount_lock);
			break;

		case FUSE_DARWIN_MOUNT_CONNECTING:
			if (force) {
				dch->mount_state = FUSE_DARWIN_MOUNT_UNMOUNTED;
				pthread_cond_broadcast(&dch->mount_cond);
				mountpoint = dch->mountpoint;
				dch->mountpoint = NULL;
				pthread_mutex_unlock(&dch->mount_lock);

				fuse_darwin_chan_close(ch);
				goto out;
			}

			res = clock_gettime(CLOCK_REALTIME, &timeout);
			assert(res == 0);

			timeout.tv_sec++;
			pthread_cond_timedwait(&dch->mount_cond,
					       &dch->mount_lock, &timeout);
			break;

		case FUSE_DARWIN_MOUNT_MOUNTED:
			dch->mount_state = FUSE_DARWIN_MOUNT_UNMOUNTING;
			pthread_cond_broadcast(&dch->mount_cond);
			pthread_mutex_unlock(&dch->mount_lock);

			unmounted = fuse_darwin_chan_unmount_real(ch, force);

			pthread_mutex_lock(&dch->mount_lock);
			assert(dch->mount_state == FUSE_DARWIN_MOUNT_UNMOUNTING);

			if (!unmounted) {
				dch->mount_state = FUSE_DARWIN_MOUNT_MOUNTED;
				pthread_cond_broadcast(&dch->mount_cond);
				pthread_mutex_unlock(&dch->mount_lock);
				goto out;
			}

			dch->mount_state = FUSE_DARWIN_MOUNT_UNMOUNTED;
			pthread_cond_broadcast(&dch->mount_cond);
			mountpoint = dch->mountpoint;
			dch->mountpoint = NULL;
			pthread_mutex_unlock(&dch->mount_lock);
			goto out;

		case FUSE_DARWIN_MOUNT_UNMOUNTING:
			pthread_cond_wait(&dch->mount_cond, &dch->mount_lock);
			break;
		}
	}

out:
	free(mountpoint);
	fuse_chan_release(ch);
	return NULL;
}

static void fuse_darwin_chan_start_unmount_thread(struct fuse_chan *ch)
{
	pthread_attr_t attr;
	pthread_t thread;
	int err;

	fuse_chan_retain(ch);

	err = pthread_attr_init(&attr);
	if (err != 0) {
		fprintf(stderr,
			"fuse: failed to create unmount thread attributes: %s\n",
			strerror(err));
		fuse_chan_release(ch);
		return;
	}

	err = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (err != 0) {
		fprintf(stderr, "fuse: failed to detach unmount thread: %s\n",
			strerror(err));
		pthread_attr_destroy(&attr);
		fuse_chan_release(ch);
		return;
	}

	err = pthread_create(&thread, &attr, fuse_darwin_chan_unmount_thread,
			     ch);
	pthread_attr_destroy(&attr);
	if (err != 0) {
		fprintf(stderr, "fuse: failed to create unmount thread: %s\n",
			strerror(err));
		fuse_chan_release(ch);
	}
}

void fuse_darwin_chan_unmount(struct fuse_chan *ch)
{
	struct fuse_darwin_chan_data *dch = fuse_darwin_chan_data(ch);
	char *mountpoint = NULL;
	bool force;

	if (dch == NULL)
		return;

	pthread_mutex_lock(&dch->mount_lock);
	while (true) {
		switch (dch->mount_state) {
		case FUSE_DARWIN_MOUNT_NONE:
		case FUSE_DARWIN_MOUNT_FAILED:
		case FUSE_DARWIN_MOUNT_UNMOUNTED:
			mountpoint = dch->mountpoint;
			dch->mountpoint = NULL;
			pthread_mutex_unlock(&dch->mount_lock);
			goto out;

		case FUSE_DARWIN_MOUNT_DELAYED:
			dch->mount_state = FUSE_DARWIN_MOUNT_UNMOUNTED;
			pthread_cond_broadcast(&dch->mount_cond);
			mountpoint = dch->mountpoint;
			dch->mountpoint = NULL;
			pthread_mutex_unlock(&dch->mount_lock);
			goto out;

		case FUSE_DARWIN_MOUNT_MOUNTING:
		case FUSE_DARWIN_MOUNT_CONNECTING:
		case FUSE_DARWIN_MOUNT_MOUNTED:
			pthread_mutex_unlock(&dch->mount_lock);
			fuse_darwin_chan_start_unmount_thread(ch);
			return;

		case FUSE_DARWIN_MOUNT_UNMOUNTING:
			pthread_mutex_unlock(&dch->mount_lock);
			return;
		}
	}

out:
	free(mountpoint);
}

bool fuse_darwin_chan_not_mounted(struct fuse_chan *ch)
{
	struct fuse_darwin_chan_data *dch = fuse_darwin_chan_data(ch);
	bool not_mounted;

	if (dch == NULL)
		return true;

	pthread_mutex_lock(&dch->mount_lock);
	not_mounted = dch->mount_state == FUSE_DARWIN_MOUNT_NONE ||
		      dch->mount_state == FUSE_DARWIN_MOUNT_FAILED ||
		      dch->mount_state == FUSE_DARWIN_MOUNT_UNMOUNTED;
	pthread_mutex_unlock(&dch->mount_lock);

	return not_mounted;
}

static int fuse_darwin_chan_copy_body(MFMessageRef mfmsg, char *buf,
				      size_t size)
{
	ssize_t body_size;
	ssize_t body_count;
	const struct iovec *body_buffers;
	size_t offset = 0;
	size_t i;

	body_size = MFMessageGetBodySize(mfmsg);
	if (body_size == -1 || (size_t)body_size > size)
		return -EIO;

	body_count = MFMessageGetBodyBuffers(mfmsg, &body_buffers);
	if (body_count == -1)
		return -EIO;

	for (i = 0; i < (size_t)body_count; i++) {
		memcpy(buf + offset, body_buffers[i].iov_base,
		       body_buffers[i].iov_len);
		offset += body_buffers[i].iov_len;
	}

	if (offset != (size_t)body_size)
		return -EIO;
	return offset;
}

static int fuse_darwin_chan_receive(struct fuse_chan **chp, char *buf,
				    size_t size)
{
	struct fuse_chan *ch = *chp;
	int err;
	ssize_t res;
	MFChannelRef mfch = NULL;
	MFMessageRef mfmsg;
	struct fuse_session *se = fuse_chan_session(ch);
	assert(se != NULL);

	err = fuse_darwin_chan_mfch(ch, &mfch);
	if (err != 0)
		return err;
	if (mfch == NULL)
		return -ENODEV;

restart:
	mfmsg = MFChannelCopyNextMessage(mfch);
	err = errno;

	if (fuse_session_exited(se)) {
		if (mfmsg != NULL)
			MFRelease(mfmsg);
		return 0;
	}
	if (mfmsg == NULL) {
		/* ENOENT means the operation was interrupted, it's safe
		   to restart */
		if (err == ENOENT)
			goto restart;

		if (err == ENODEV) {
			fuse_session_exit(se);
			return 0;
		}

		/* Errors occurring during normal operation: EINTR (read
		   interrupted), EAGAIN (nonblocking I/O), ENODEV (filesystem
		   umounted) */
		if (err != EINTR && err != EAGAIN)
			perror("fuse: reading channel");
		return -err;
	}

	res = fuse_darwin_chan_copy_body(mfmsg, buf, size);
	if (res < 0) {
		fprintf(stderr, "failed to flatten message\n");
		MFRelease(mfmsg);
		return res;
	}
	if ((size_t) res < sizeof(struct fuse_in_header)) {
		fprintf(stderr, "short read on fuse channel\n");
		MFRelease(mfmsg);
		return -EIO;
	}

	MFRelease(mfmsg);
	return res;
}

static int fuse_darwin_chan_send(struct fuse_chan *ch, const struct iovec iov[],
				 size_t count)
{
	if (iov) {
		int err;
		MFChannelRef mfch = NULL;
		ssize_t res;

		err = fuse_darwin_chan_mfch(ch, &mfch);
		if (err != 0)
			return err;
		if (mfch == NULL)
			return -ENODEV;

		res = MFChannelSendMessage(mfch, iov, count);
		err = errno;

		if (res == -1) {
			struct fuse_session *se = fuse_chan_session(ch);

			assert(se != NULL);

			/* ENOENT means the operation was interrupted */
			if (!fuse_session_exited(se) && err != ENOENT)
				perror("fuse: writing device");
			return -err;
		}
	}
	return 0;
}

static void fuse_darwin_chan_destroy(struct fuse_chan *ch)
{
	struct fuse_darwin_chan_data *dch = fuse_darwin_chan_data(ch);
	DADiskRef disk;
	MFChannelRef mfch;

	if (dch == NULL)
		return;

	fuse_darwin_chan_close(ch);

	pthread_mutex_lock(&dch->lock);
	disk = dch->disk;
	dch->disk = NULL;
	mfch = dch->mfch;
	dch->mfch = NULL;
	pthread_mutex_unlock(&dch->lock);

	destroy_mount_opts(dch->mount_opts);
	free(dch->mountpoint);
	pthread_cond_destroy(&dch->mount_cond);
	pthread_mutex_destroy(&dch->mount_lock);
	pthread_mutex_destroy(&dch->lock);
	free(dch);

	if (disk != NULL)
		CFRelease(disk);
	if (mfch != NULL)
		MFRelease(mfch);
}

#ifdef __APPLE__
#define MIN_BUFSIZE ((FUSE_DEFAULT_USERKERNEL_BUFSIZE) + 0x1000)
#else
#define MIN_BUFSIZE 0x21000
#endif

struct fuse_chan *fuse_darwin_chan_new(const char *mountpoint,
				       struct mount_opts *mo)
{
	struct fuse_chan_ops op = {
		.receive = fuse_darwin_chan_receive,
		.send = fuse_darwin_chan_send,
		.destroy = fuse_darwin_chan_destroy,
	};

	size_t bufsize = sysconf(_SC_PAGESIZE) + 0x1000;
	struct fuse_darwin_chan_data *dch = NULL;
	struct fuse_chan *ch;

	bufsize = bufsize < MIN_BUFSIZE ? MIN_BUFSIZE : bufsize;

	if (mountpoint == NULL)
		return NULL;
	if (mo == NULL)
		return NULL;

	dch = calloc(1, sizeof(struct fuse_darwin_chan_data));
	if (dch == NULL)
		return NULL;

	dch->mountpoint = strdup(mountpoint);
	if (dch->mountpoint == NULL) {
		free(dch);
		return NULL;
	}

	pthread_mutex_init(&dch->lock, NULL);
	pthread_mutex_init(&dch->mount_lock, NULL);
	pthread_cond_init(&dch->mount_cond, NULL);
	dch->mount_state = FUSE_DARWIN_MOUNT_DELAYED;

	ch = fuse_chan_new(&op, -1, bufsize, dch);
	if (ch == NULL)
		goto err_out;

	fuse_chan_set_type(ch, FUSE_CHAN_TYPE_DARWIN);
	dch->mount_opts = mo;
	return ch;

err_out:
	pthread_cond_destroy(&dch->mount_cond);
	pthread_mutex_destroy(&dch->mount_lock);
	pthread_mutex_destroy(&dch->lock);
	free(dch->mountpoint);
	free(dch);
	return NULL;
}

#endif
