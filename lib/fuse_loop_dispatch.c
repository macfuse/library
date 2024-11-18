/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU LGPLv2.
  See the file COPYING.LIB.
*/

/*
 * Copyright (c) 2006-2008 Amit Singh/Google Inc.
 * Copyright (c) 2011-2024 Benjamin Fleischer
 * Copyright (c) 2017 Dave MacLachlan/Google Inc.
 */

#include "config.h"

#ifdef HAVE_DISPATCH_DISPATCH_H

#include "fuse_i.h"
#include "fuse_lowlevel.h"

#include <dispatch/dispatch.h>
#include <stdio.h>

int fuse_session_loop_dispatch(struct fuse_session *se)
{
	int res = 0;

	struct fuse_chan *ch = fuse_session_next_chan(se, NULL);
	size_t bufsize = fuse_chan_bufsize(ch);
	char *buf = NULL;

	dispatch_queue_t queue = NULL;
	dispatch_group_t group = NULL;

	buf = (char *)malloc(bufsize);
	if (!buf) {
		fprintf(stderr, "fuse: failed to allocate read buffer\n");
		res = -1;
		goto out;
	}

	queue = dispatch_queue_create("fuse_session_loop_dispatch",
				      DISPATCH_QUEUE_CONCURRENT);
	if (!queue) {
		fprintf(stderr, "fuse: failed to create session queue\n");
		res = -1;
		goto out;
	}

	group = dispatch_group_create();
	if (!group) {
		fprintf(stderr, "fuse: failed to create session group\n");
		res = -1;
		goto out;
	}

	while (!fuse_session_exited(se)) {
		struct fuse_chan *tmpch = ch;
		struct fuse_buf fbuf = {
			.mem = buf,
			.size = bufsize,
		};

		res = fuse_session_receive_buf(se, &fbuf, &tmpch);

		if (res == -EINTR)
			continue;
		if (res <= 0)
			break;

		/*
		 * Create a local buffer and copy because buf is huge, and the
		 * data transferred is usually orders of magnitude smaller.
		 */
		char *process_buf = (char *)malloc(res);
		if (!process_buf) {
			fprintf(stderr,
				"fuse: failed to allocate process buffer\n");
			res = -1;
			break;
		}
		memcpy(process_buf, fbuf.mem, res);
		fbuf.mem = process_buf;
		fbuf.size = res;

		dispatch_group_async(group, queue, ^{
			fuse_session_process_buf(se, &fbuf, tmpch);
			free(fbuf.mem);
		});
	}

	if (dispatch_group_wait(group, DISPATCH_TIME_FOREVER) != 0) {
		fprintf(stderr, "fuse: dispatch_group_wait timed out\n");
		res = -1;
	}

out:
	free(buf);
	if (group)
		dispatch_release(group);
	if (queue)
		dispatch_release(queue);
	fuse_session_reset(se);

	return res < 0 ? -1 : 0;
}

int fuse_loop_dispatch(struct fuse *f) {
	int res = 0;

	if (f == NULL)
		return -1;

	res = fuse_start_cleanup_thread(f);
	if (res)
		return res;

	res = fuse_session_loop_dispatch(fuse_get_session(f));

	fuse_stop_cleanup_thread(f);
	return res;
}

#endif /* HAVE_DISPATCH_DISPATCH_H */
