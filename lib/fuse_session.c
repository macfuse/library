/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU LGPLv2.
  See the file COPYING.LIB
*/

/*
 * Copyright (c) 2006-2008 Amit Singh/Google Inc.
 * Copyright (c) 2011-2017 Benjamin Fleischer
 */

#include "fuse_i.h"
#include "fuse_misc.h"
#include "fuse_common_compat.h"
#include "fuse_lowlevel_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#ifdef __APPLE__
#  include <sys/param.h>
#endif

#ifdef __APPLE__
#  include <CoreFoundation/CoreFoundation.h>
#endif

struct fuse_chan {
#ifdef __APPLE__
	pthread_mutex_t lock;
	int retain_count;
#endif

	struct fuse_chan_ops op;

	struct fuse_session *se;

	int fd;

#ifdef __APPLE__
	DADiskRef disk;
#endif

	size_t bufsize;

	void *data;

	int compat;
};

struct fuse_session *fuse_session_new(struct fuse_session_ops *op, void *data)
{
	struct fuse_session *se = (struct fuse_session *) malloc(sizeof(*se));
	if (se == NULL) {
		fprintf(stderr, "fuse: failed to allocate session\n");
		return NULL;
	}

	memset(se, 0, sizeof(*se));
	se->op = *op;
	se->data = data;

	return se;
}

void fuse_session_add_chan(struct fuse_session *se, struct fuse_chan *ch)
{
	assert(se->ch == NULL);
	assert(ch->se == NULL);
	se->ch = ch;
	ch->se = se;
}

void fuse_session_remove_chan(struct fuse_chan *ch)
{
	struct fuse_session *se = ch->se;
	if (se) {
		assert(se->ch == ch);
		se->ch = NULL;
		ch->se = NULL;
	}
}

struct fuse_chan *fuse_session_next_chan(struct fuse_session *se,
					 struct fuse_chan *ch)
{
	assert(ch == NULL || ch == se->ch);
	if (ch == NULL)
		return se->ch;
	else
		return NULL;
}

void fuse_session_process(struct fuse_session *se, const char *buf, size_t len,
			  struct fuse_chan *ch)
{
	se->op.process(se->data, buf, len, ch);
}

void fuse_session_process_buf(struct fuse_session *se,
			      const struct fuse_buf *buf, struct fuse_chan *ch)
{
	if (se->process_buf) {
		se->process_buf(se->data, buf, ch);
	} else {
		assert(!(buf->flags & FUSE_BUF_IS_FD));
		fuse_session_process(se->data, buf->mem, buf->size, ch);
	}
}

int fuse_session_receive_buf(struct fuse_session *se, struct fuse_buf *buf,
			     struct fuse_chan **chp)
{
	int res;

	if (se->receive_buf) {
		res = se->receive_buf(se, buf, chp);
	} else {
		res = fuse_chan_recv(chp, buf->mem, buf->size);
		if (res > 0)
			buf->size = res;
	}

	return res;
}


void fuse_session_destroy(struct fuse_session *se)
{
	if (se->op.destroy)
		se->op.destroy(se->data);
	if (se->ch != NULL)
		fuse_chan_destroy(se->ch);
	free(se);
}

void fuse_session_exit(struct fuse_session *se)
{
	if (se->op.exit)
		se->op.exit(se->data, 1);
	se->exited = 1;
}

void fuse_session_reset(struct fuse_session *se)
{
	if (se->op.exit)
		se->op.exit(se->data, 0);
	se->exited = 0;
}

int fuse_session_exited(struct fuse_session *se)
{
	if (se->op.exited)
		return se->op.exited(se->data);
	else
		return se->exited;
}

void *fuse_session_data(struct fuse_session *se)
{
	return se->data;
}

static struct fuse_chan *fuse_chan_new_common(struct fuse_chan_ops *op, int fd,
					      size_t bufsize, void *data,
					      int compat)
{
	struct fuse_chan *ch = (struct fuse_chan *) malloc(sizeof(*ch));
	if (ch == NULL) {
		fprintf(stderr, "fuse: failed to allocate channel\n");
		return NULL;
	}

	memset(ch, 0, sizeof(*ch));
#ifdef __APPLE__
	pthread_mutex_init(&ch->lock, NULL);
	ch->retain_count = 1;
#endif
	ch->op = *op;
	ch->fd = fd;
	ch->bufsize = bufsize;
	ch->data = data;
	ch->compat = compat;

	return ch;
}

struct fuse_chan *fuse_chan_new(struct fuse_chan_ops *op, int fd,
				size_t bufsize, void *data)
{
	return fuse_chan_new_common(op, fd, bufsize, data, 0);
}

struct fuse_chan *fuse_chan_new_compat24(struct fuse_chan_ops_compat24 *op,
					 int fd, size_t bufsize, void *data)
{
	return fuse_chan_new_common((struct fuse_chan_ops *) op, fd, bufsize,
				    data, 24);
}

#ifdef __APPLE__

void fuse_chan_retain(struct fuse_chan *ch)
{
	pthread_mutex_lock(&ch->lock);
	ch->retain_count++;
	pthread_mutex_unlock(&ch->lock);
}

void fuse_chan_release(struct fuse_chan *ch)
{
	bool release = false;

	pthread_mutex_lock(&ch->lock);
	release = --ch->retain_count == 0;
	pthread_mutex_unlock(&ch->lock);

	if (release) {
		pthread_mutex_destroy(&ch->lock);
		if (ch->disk)
			CFRelease(ch->disk);
		free(ch);
	}
}

#endif

int fuse_chan_fd(struct fuse_chan *ch)
{
	return ch->fd;
}

int fuse_chan_clearfd(struct fuse_chan *ch)
{
	int fd = ch->fd;
	ch->fd = -1;
	return fd;
}

#ifdef __APPLE__

DADiskRef fuse_chan_disk(struct fuse_chan *ch)
{
	DADiskRef disk = NULL;

	pthread_mutex_lock(&ch->lock);
	disk = ch->disk;
	if (disk)
		CFRetain(disk);
	pthread_mutex_unlock(&ch->lock);

	return disk;
}

void fuse_chan_set_disk(struct fuse_chan *ch, DADiskRef disk)
{
	DADiskRef old = NULL;

	if (disk)
		CFRetain(disk);

	pthread_mutex_lock(&ch->lock);
	old = ch->disk;
	ch->disk = disk;
	pthread_mutex_unlock(&ch->lock);

	if (old)
		CFRelease(old);
}

void fuse_chan_cleardisk(struct fuse_chan *ch)
{
	fuse_chan_set_disk(ch, NULL);
}

#endif /* __APPLE__ */

size_t fuse_chan_bufsize(struct fuse_chan *ch)
{
	return ch->bufsize;
}

void *fuse_chan_data(struct fuse_chan *ch)
{
	return ch->data;
}

struct fuse_session *fuse_chan_session(struct fuse_chan *ch)
{
	return ch->se;
}

int fuse_chan_recv(struct fuse_chan **chp, char *buf, size_t size)
{
	struct fuse_chan *ch = *chp;
	if (ch->compat)
		return ((struct fuse_chan_ops_compat24 *) &ch->op)
			->receive(ch, buf, size);
	else
		return ch->op.receive(chp, buf, size);
}

int fuse_chan_receive(struct fuse_chan *ch, char *buf, size_t size)
{
	int res;

	res = fuse_chan_recv(&ch, buf, size);
	return res >= 0 ? res : (res != -EINTR && res != -EAGAIN) ? -1 : 0;
}

int fuse_chan_send(struct fuse_chan *ch, const struct iovec iov[], size_t count)
{
	return ch->op.send(ch, iov, count);
}

void fuse_chan_destroy(struct fuse_chan *ch)
{
	fuse_session_remove_chan(ch);
	if (ch->op.destroy)
		ch->op.destroy(ch);
#ifdef __APPLE__
	fuse_chan_release(ch);
#else
	free(ch);
#endif
}

#if !defined(__FreeBSD__) && !defined(__APPLE__)
FUSE_SYMVER(".symver fuse_chan_new_compat24,fuse_chan_new@FUSE_2.4");
#endif
