/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2025 Benjamin Fleischer

  This program can be distributed under the terms of the GNU LGPLv2.
  See the file COPYING.LIB.
*/

#ifdef __APPLE__

#include "fuse_lowlevel.h"

struct fuse_custom_io_ctx;

struct fuse_custom_io *fuse_socket_io_new(void);
struct fuse_custom_io_ctx *fuse_socket_io_ctx_new(void);

#endif
