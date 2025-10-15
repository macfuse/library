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

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <sys/socket.h>

#include <dispatch/dispatch.h>

struct fuse_socket_chan_data {
	dispatch_queue_t send;
	dispatch_queue_t receive;
};

static int receive_chunk(int fd, char *buf, size_t size)
{
	while (size > 0) {
		int res = recv(fd, buf, size, 0);
		if (res == 0) {
			/* Socket has been closed by the peer */
			return ENODEV;
		} else if (res == -1) {
			/* Errors occurring during normal operation: EINTR (read
			   interrupted), EAGAIN (nonblocking I/O) */
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return errno;
		}
		buf += res;
		size -= res;
	}
	return 0;
}

static int receive_sync(struct fuse_chan **chp, char *buf)
{
	struct fuse_chan *ch = *chp;
	int err;
	struct fuse_in_header *in = NULL;
	size_t size = sizeof(*in);
	struct fuse_session *se = fuse_chan_session(ch);
	assert(se != NULL);

	if (fuse_session_exited(se))
		return 0;

restart:
	err = receive_chunk(fuse_chan_fd(ch), buf, size);

	if (fuse_session_exited(se))
		return 0;
	if (err) {
		if (err == ENODEV || err == ECONNRESET) {
			fuse_session_exit(se);
			return 0;
		}
		fprintf(stderr, "fuse: reading socket: %s\n", strerror(err));
		return -err;
	}

	if (!in) {
		in = (struct fuse_in_header *)buf;
		buf += sizeof(*in);

		size = in->len - sizeof(*in);
		if (size > 0)
			goto restart;
	}

	return in->len;
}

static int send_sync(struct fuse_chan *ch, const struct iovec iov[],
		     size_t count)
{
	if (iov) {
		ssize_t res = writev(fuse_chan_fd(ch), iov, count);
		int err = errno;

		if (res == -1) {
			struct fuse_session *se = fuse_chan_session(ch);

			assert(se != NULL);

			/* ENOENT means the operation was interrupted */
			if (!fuse_session_exited(se) && err != ENOENT)
				perror("fuse: writing socket");
			return -err;
		}
	}
	return 0;
}

static int fuse_socket_chan_receive(struct fuse_chan **chp, char *buf,
				    size_t size)
{
	struct fuse_socket_chan_data *data =
		(struct fuse_socket_chan_data *)fuse_chan_data(*chp);
	__block int res;

	dispatch_sync(data->receive, ^{
		res = receive_sync(chp, buf);
	});

	return res;
}

static int fuse_socket_chan_send(struct fuse_chan *ch, const struct iovec iov[],
				 size_t count)
{
	struct fuse_socket_chan_data *data =
		(struct fuse_socket_chan_data *)fuse_chan_data(ch);
	__block int res;

	dispatch_sync(data->send, ^{
		res = send_sync(ch, iov, count);
	});

	return res;
}

static void fuse_socket_chan_destroy(struct fuse_chan *ch)
{
	int fd = fuse_chan_fd(ch);

	if (fd != -1) {
		(void)ioctl(fd, FUSEDEVIOCSETDAEMONDEAD, &fd);
		close(fd);
	}

	struct fuse_socket_chan_data *data =
		(struct fuse_socket_chan_data *)fuse_chan_data(ch);
	if (data) {
		dispatch_release(data->send);
		dispatch_release(data->receive);
		free(data);
	}
}

#ifdef __APPLE__
#define MIN_BUFSIZE ((FUSE_DEFAULT_USERKERNEL_BUFSIZE) + 0x1000)
#else
#define MIN_BUFSIZE 0x21000
#endif

struct fuse_chan *fuse_socket_chan_new(int fd)
{
	struct fuse_chan_ops op = {
		.receive = fuse_socket_chan_receive,
		.send = fuse_socket_chan_send,
		.destroy = fuse_socket_chan_destroy,
	};
	size_t bufsize = sysconf(_SC_PAGESIZE) + 0x1000;
	struct fuse_socket_chan_data *data = NULL;

	bufsize = bufsize < MIN_BUFSIZE ? MIN_BUFSIZE : bufsize;

	data = (struct fuse_socket_chan_data *)malloc(sizeof(*data));
	if (!data)
		return NULL;

	data->send = dispatch_queue_create("fuse_socket_chan_send",
					     DISPATCH_QUEUE_SERIAL);
	data->receive = dispatch_queue_create("fuse_socket_chan_receive",
						DISPATCH_QUEUE_SERIAL);

	return fuse_chan_new(&op, fd, bufsize, data);
}

#endif
