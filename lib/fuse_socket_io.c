/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2025 Benjamin Fleischer

  This program can be distributed under the terms of the GNU LGPLv2.
  See the file COPYING.LIB.
*/

#ifdef __APPLE__

#include "fuse_i.h"
#include "fuse_kernel.h"
#include "fuse_socket_io.h"

#include <pthread.h>
#include <sys/errno.h>
#include <sys/socket.h>

#include <dispatch/dispatch.h>

struct fuse_socket_io_data {
	dispatch_queue_t write;
	dispatch_queue_t read;
	struct fuse_in_header cached_in_header;
};

static struct fuse_socket_io_data *fuse_socket_io_data_new(void)
{
	struct fuse_socket_io_data *data = malloc(sizeof(*data));
	if (data) {
		data->write = dispatch_queue_create("fuse_socket_io_write",
						    DISPATCH_QUEUE_SERIAL);
		data->read = dispatch_queue_create("fuse_socket_io_read",
						   DISPATCH_QUEUE_SERIAL);
		memset(&data->cached_in_header, 0,
		       sizeof(data->cached_in_header));
	}
	return data;
}

static void fuse_socket_io_data_destroy(void *data)
{
	struct fuse_socket_io_data *d = data;
	dispatch_release(d->read);
	dispatch_release(d->write);
	free(d);
}

static ssize_t fuse_socket_io_writev(int fd, struct iovec *iov, int count,
				     void *userdata)
{
	struct fuse_socket_io_data *data = userdata;
	int cancelstate;
	__block int res;
	__block int err;

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancelstate);
	dispatch_sync(data->write, ^{
		res = writev(fd, iov, count);
		err = errno;
	});
	pthread_setcancelstate(cancelstate, NULL);

	errno = err;
	return res;
}

static int read_chunk(int fd, char *buf, size_t size)
{
	while (size > 0) {
		int res = recv(fd, buf, size, 0);
		if (res == 0) {
			/* Socket has been closed by the peer */
			return ENODEV;
		} else if (res == -1) {
			/*
			 * Errors occurring during normal operation:
			 * - EINTR (read interrupted)
			 * - EAGAIN (nonblocking I/O)
			 */
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return errno;
		}
		buf += res;
		size -= res;
	}
	return 0;
}

static int read_sync(int fd, char *buf, size_t buf_len,
		     struct fuse_socket_io_data *data)
{
	int err;
	struct fuse_in_header *in_header = NULL;
	size_t size = sizeof(*in_header);

	if (buf_len < size) {
		errno = EINVAL;
		return -1;
	}
	if (data->cached_in_header.len > 0 ) {
		if (data->cached_in_header.len > buf_len) {
			errno = EINVAL;
			return -1;
		}

		fuse_log(FUSE_LOG_INFO, "fuse: Use cached header\n");
		*((struct fuse_in_header *)buf) = data->cached_in_header;
		data->cached_in_header.len = 0;
		goto skip;
	}

read:
	err = read_chunk(fd, buf, size);
	if (err) {
		if (err == ENODEV || err == ECONNRESET) {
			errno = ENODEV;
			return -1;
		}

		fuse_log(FUSE_LOG_ERR, "fuse: reading socket: %s\n",
			 strerror(err));
		errno = err;
		return -1;
	}

skip:
	if (!in_header) {
		in_header = (struct fuse_in_header *)buf;
		if (in_header->len > buf_len) {
			/*
			 * The received message exceeds buf_len. We need to
			 * cache the header until we are called again with a
			 * larger buffer.
			 */
			data->cached_in_header = *in_header;
			errno = EINVAL;
			return -1;
		}

		buf += size;
		size = in_header->len - sizeof(*in_header);

		if (size > 0)
			goto read;
	}

	return in_header->len;
}

static ssize_t fuse_socket_io_read(int fd, void *buf, size_t buf_len,
                                   void *userdata)
{
	struct fuse_socket_io_data *data = userdata;
	int cancelstate;
	__block int res;
	__block int err;

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancelstate);
	dispatch_sync(data->read, ^{
		res = read_sync(fd, buf, buf_len, data);
		err = errno;
	});
	pthread_setcancelstate(cancelstate, NULL);

	errno = err;
	return res;
}

struct fuse_custom_io *fuse_socket_io_new(void)
{
	struct fuse_custom_io *io = calloc(1, sizeof(*io));
	if (io != NULL) {
		io->writev = fuse_socket_io_writev;
		io->read = fuse_socket_io_read;
	}
	return io;
}

struct fuse_custom_io_ctx *fuse_socket_io_ctx_new(void)
{
	struct fuse_socket_io_data *data = NULL;
	struct fuse_custom_io_ctx *ioc = NULL;

	data = fuse_socket_io_data_new();
	if (data == NULL)
		return NULL;

	ioc = fuse_custom_io_ctx_new(data, fuse_socket_io_data_destroy);
	if (ioc == NULL) {
		fuse_socket_io_data_destroy(data);
		return NULL;
	}
	return ioc;
}

#endif
