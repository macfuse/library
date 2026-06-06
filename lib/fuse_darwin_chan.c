/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU LGPLv2.
  See the file COPYING.LIB
*/

/*
 * Copyright (c) 2006-2008 Amit Singh/Google Inc.
 * Copyright (c) 2011-2025 Benjamin Fleischer
 */

#ifdef __APPLE__

#include "fuse_lowlevel.h"
#include "fuse_kernel.h"
#include "fuse_i.h"
#include "fuse_darwin.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <MFMount/MFMount.h>

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
	MFChannelRef mfch = (MFChannelRef)fuse_chan_data(ch);
	MFMessageRef mfmsg;
	struct fuse_session *se = fuse_chan_session(ch);
	assert(se != NULL);

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
		MFChannelRef mfch = (MFChannelRef)fuse_chan_data(ch);
		ssize_t res = MFChannelSendMessage(mfch, iov, count);
		int err = errno;

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
	MFChannelRef mfch = (MFChannelRef)fuse_chan_data(ch);
	MFChannelClose(mfch);
	MFRelease(mfch);
}

#ifdef __APPLE__
#define MIN_BUFSIZE ((FUSE_DEFAULT_USERKERNEL_BUFSIZE) + 0x1000)
#else
#define MIN_BUFSIZE 0x21000
#endif

struct fuse_chan *fuse_darwin_chan_new(MFChannelRef channel)
{
	struct fuse_chan_ops op = {
		.receive = fuse_darwin_chan_receive,
		.send = fuse_darwin_chan_send,
		.destroy = fuse_darwin_chan_destroy,
	};

	size_t bufsize = sysconf(_SC_PAGESIZE) + 0x1000;
	bufsize = bufsize < MIN_BUFSIZE ? MIN_BUFSIZE : bufsize;

	return fuse_chan_new(&op, -1, bufsize, MFRetain(channel));
}

#endif
