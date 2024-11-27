/*
 * Copyright (c) 2006-2008 Amit Singh/Google Inc.
 * Copyright (c) 2011-2024 Benjamin Fleischer
 */

#ifdef __APPLE__

#ifndef _FUSE_DARWIN_PRIVATE_H_
#define _FUSE_DARWIN_PRIVATE_H_

#include <stdint.h>
#include <sys/ioctl.h>

#ifndef FUSE_DEFAULT_USERKERNEL_BUFSIZE
#  define FUSE_DEFAULT_USERKERNEL_BUFSIZE 33554432
#endif

#ifndef FUSE_MOUNT_PROG
#  define FUSE_MOUNT_PROG "/Library/Filesystems/macfuse.fs/Contents/Resources/mount_macfuse"
#endif

#ifndef FUSE_VOLUME_ICON
#  define FUSE_VOLUME_ICON "/Library/Filesystems/macfuse.fs/Contents/Resources/Volume.icns"
#endif

// Mark the daemon as dead
#define FUSEDEVIOCSETDAEMONDEAD _IOW('F', 3,  u_int32_t)

/* lock operations for flock(2) */
#ifndef LOCK_SH
#  define LOCK_SH         0x01            /* shared file lock */
#  define LOCK_EX         0x02            /* exclusive file lock */
#  define LOCK_NB         0x04            /* don't block when locking */
#  define LOCK_UN         0x08            /* unlock file */
#endif /* !LOCK_SH */

char *fuse_resource_path(const char *path);

#endif /* _FUSE_DARWIN_PRIVATE_H_ */

#endif /* __APPLE__ */
