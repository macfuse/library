/*
  fuse threadid module: per-thread override identity support for macOS
  Copyright (C) 2006-2008  Amit Singh / Google Inc.
  Copyright (C) 2012-2025  Benjamin Fleischer

  This program can be distributed under the terms of the GNU LGPLv2.
  See the file COPYING.LIB
*/

#include <fuse_config.h>

#ifdef __APPLE__
#define FUSE_DARWIN_OVERLOAD_OPERATIONS 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <sys/kauth.h>
#include <sys/types.h>
#include <sys/unistd.h>
#include <unistd.h>
#include <fuse.h>

#ifdef HAVE_STATX
#error "Support for statx() not implemented"
#endif

static inline int threadid_pthread_setugid_np(uid_t uid, gid_t gid)
{
	_Pragma("clang diagnostic push")
	_Pragma("clang diagnostic ignored \"-Wdeprecated-declarations\"")
	int res = pthread_setugid_np(uid, gid);
	_Pragma("clang diagnostic pop")
	return res;
}

#define THREADID_PRE \
	struct fuse_context *context = fuse_get_context(); \
	uid_t calleruid = context->uid; \
	gid_t callergid = context->gid; \
	uid_t issuser = !geteuid(); \
	int needsettid = (issuser && calleruid); \
	if (needsettid) { \
		threadid_pthread_setugid_np(calleruid, callergid); \
	}

#define THREADID_POST \
	if (needsettid) { \
		threadid_pthread_setugid_np(KAUTH_UID_NONE, KAUTH_GID_NONE); \
	}

struct threadid {
	struct fuse_fs *next;
};

static struct threadid *threadid_get(void)
{
	return fuse_get_context()->private_data;
}

static int threadid_getattr(const char *path, struct stat *stbuf,
			    struct fuse_file_info *fi)
{
	THREADID_PRE
	int res = fuse_fs_getattr(threadid_get()->next, path, stbuf, fi);
	THREADID_POST

	return res;
}

static int threadid_getattr$DARWIN(const char *path,
				   struct fuse_darwin_attr *attr,
				   struct fuse_file_info *fi)
{
	THREADID_PRE
	int res = fuse_fs_getattr$DARWIN(threadid_get()->next, path, attr, fi);
	THREADID_POST

	return res;
}

static int threadid_setattr(const char *path, struct fuse_darwin_attr *attr,
			    int to_set, struct fuse_file_info *fi)
{
	THREADID_PRE
	int res = fuse_fs_setattr(threadid_get()->next, path, attr, to_set, fi);
	THREADID_POST

	return res;
}

static int threadid_access(const char *path, int mask)
{
	THREADID_PRE
	int res = fuse_fs_access(threadid_get()->next, path, mask);
	THREADID_POST

	return res;
}

static int threadid_readlink(const char *path, char *buf, size_t size)
{
	THREADID_PRE
	int res = fuse_fs_readlink(threadid_get()->next, path, buf, size);
	THREADID_POST

	return res;
}

static int threadid_opendir(const char *path, struct fuse_file_info *fi)
{
	THREADID_PRE
	int res = fuse_fs_opendir(threadid_get()->next, path, fi);
	THREADID_POST

	return res;
}

static int threadid_readdir(const char *path, void *buf,
			    fuse_fill_dir_t filler, off_t offset,
			    struct fuse_file_info *fi,
			    enum fuse_readdir_flags flags)
{
	THREADID_PRE
	int res = fuse_fs_readdir(threadid_get()->next, path, buf, filler,
				  offset, fi, flags);
	THREADID_POST

	return res;
}

static int threadid_readdir$DARWIN(const char *path, void *buf,
				   fuse_darwin_fill_dir_t filler, off_t offset,
				   struct fuse_file_info *fi,
				   enum fuse_readdir_flags flags)
{
	THREADID_PRE
	int res = fuse_fs_readdir$DARWIN(threadid_get()->next, path, buf,
					 filler, offset, fi, flags);
	THREADID_POST

	return res;
}

static int threadid_releasedir(const char *path, struct fuse_file_info *fi)
{
	THREADID_PRE
	int res = fuse_fs_releasedir(threadid_get()->next, path, fi);
	THREADID_POST

	return res;
}

static int threadid_mknod(const char *path, mode_t mode, dev_t rdev)
{
	THREADID_PRE
	int res = fuse_fs_mknod(threadid_get()->next, path, mode, rdev);
	THREADID_POST

	return res;
}

static int threadid_mkdir(const char *path, mode_t mode)
{
	THREADID_PRE
	int res = fuse_fs_mkdir(threadid_get()->next, path, mode);
	THREADID_POST

	return res;
}

static int threadid_unlink(const char *path)
{
	THREADID_PRE
	int res = fuse_fs_unlink(threadid_get()->next, path);
	THREADID_POST

	return res;
}

static int threadid_rmdir(const char *path)
{
	THREADID_PRE
	int res = fuse_fs_rmdir(threadid_get()->next, path);
	THREADID_POST

	return res;
}

static int threadid_symlink(const char *from, const char *path)
{
	THREADID_PRE
	int res = fuse_fs_symlink(threadid_get()->next, from, path);
	THREADID_POST

	return res;
}

static int threadid_rename(const char *from, const char *to, unsigned int flags)
{
	THREADID_PRE
	int res = fuse_fs_rename(threadid_get()->next, from, to, flags);
	THREADID_POST

	return res;
}

static int threadid_link(const char *from, const char *to)
{
	THREADID_PRE
	int res = fuse_fs_link(threadid_get()->next, from, to);
	THREADID_POST

	return res;
}

static int threadid_chmod(const char *path, mode_t mode,
			  struct fuse_file_info *fi)
{
	THREADID_PRE
	int res = fuse_fs_chmod(threadid_get()->next, path, mode, fi);
	THREADID_POST

	return res;
}

static int threadid_chown(const char *path, uid_t uid, gid_t gid,
			  struct fuse_file_info *fi)
{
	THREADID_PRE
	int res = fuse_fs_chown(threadid_get()->next, path, uid, gid, fi);
	THREADID_POST

	return res;
}

static int threadid_truncate(const char *path, off_t size,
			     struct fuse_file_info *fi)
{
	THREADID_PRE
	int res = fuse_fs_truncate(threadid_get()->next, path, size, fi);
	THREADID_POST

	return res;
}

static int threadid_utimens(const char *path, const struct timespec ts[2],
			    struct fuse_file_info *fi)
{
	THREADID_PRE
	int res = fuse_fs_utimens(threadid_get()->next, path, ts, fi);
	THREADID_POST

	return res;
}

#ifdef __APPLE__
static int threadid_utimens$DARWIN(const char *path,
				   const struct timespec ts[3],
				   struct fuse_file_info *fi)
{
	THREADID_PRE
	int res = fuse_fs_utimens$DARWIN(threadid_get()->next, path, ts, fi);
	THREADID_POST

	return res;
}
#endif

static int threadid_create(const char *path, mode_t mode,
			   struct fuse_file_info *fi)
{
	THREADID_PRE
	int res = fuse_fs_create(threadid_get()->next, path, mode, fi);
	THREADID_POST

	return res;
}

static int threadid_open(const char *path, struct fuse_file_info *fi)
{
	THREADID_PRE
	int res = fuse_fs_open(threadid_get()->next, path, fi);
	THREADID_POST

	return res;
}

static int threadid_read_buf(const char *path, struct fuse_bufvec **bufp,
			     size_t size, off_t offset,
			     struct fuse_file_info *fi)
{
	THREADID_PRE
	int res = fuse_fs_read_buf(threadid_get()->next, path, bufp, size,
				   offset, fi);
	THREADID_POST

	return res;
}

static int threadid_write_buf(const char *path, struct fuse_bufvec *buf,
			      off_t offset, struct fuse_file_info *fi)
{
	THREADID_PRE
	int res = fuse_fs_write_buf(threadid_get()->next, path, buf, offset,
				    fi);
	THREADID_POST

	return res;
}

static int threadid_statfs(const char *path, struct statvfs *stbuf)
{
	THREADID_PRE
	int res = fuse_fs_statfs(threadid_get()->next, path, stbuf);
	THREADID_POST

	return res;
}

static int threadid_statfs$DARWIN(const char *path, struct statfs *stbuf)
{
	THREADID_PRE
	int res = fuse_fs_statfs$DARWIN(threadid_get()->next, path, stbuf);
	THREADID_POST

	return res;
}

static int threadid_flush(const char *path, struct fuse_file_info *fi)
{
	THREADID_PRE
	int res = fuse_fs_flush(threadid_get()->next, path, fi);
	THREADID_POST

	return res;
}

static int threadid_release(const char *path, struct fuse_file_info *fi)
{
	THREADID_PRE
	int res = fuse_fs_release(threadid_get()->next, path, fi);
	THREADID_POST

	return res;
}

static int threadid_fsync(const char *path, int isdatasync,
			  struct fuse_file_info *fi)
{
	THREADID_PRE
	int res = fuse_fs_fsync(threadid_get()->next, path, isdatasync, fi);
	THREADID_POST

	return res;
}

static int threadid_fsyncdir(const char *path, int isdatasync,
			     struct fuse_file_info *fi)
{
	THREADID_PRE
	int res = fuse_fs_fsyncdir(threadid_get()->next, path, isdatasync, fi);
	THREADID_POST

	return res;
}

static int threadid_setxattr(const char *path, const char *name,
			     const char *value, size_t size, int flags)
{
	THREADID_PRE
	int res = fuse_fs_setxattr(threadid_get()->next, path, name, value,
				   size, flags);
	THREADID_POST

	return res;
}

static int threadid_setxattr$DARWIN(const char *path, const char *name,
				    const char *value, size_t size, int flags,
				    unsigned int position)
{
	THREADID_PRE
	int res = fuse_fs_setxattr$DARWIN(threadid_get()->next, path, name,
					  value, size, flags, position);
	THREADID_POST

	return res;
}

static int threadid_getxattr(const char *path, const char *name, char *value,
			     size_t size)
{
	THREADID_PRE
	int res = fuse_fs_getxattr(threadid_get()->next, path, name, value,
				   size);
	THREADID_POST

	return res;
}

static int threadid_getxattr$DARWIN(const char *path, const char *name,
				    char *value, size_t size,
				    unsigned int position)
{
	THREADID_PRE
	int res = fuse_fs_getxattr$DARWIN(threadid_get()->next, path, name,
					  value, size, position);
	THREADID_POST

	return res;
}

static int threadid_listxattr(const char *path, char *list, size_t size)
{
	THREADID_PRE
	int res = fuse_fs_listxattr(threadid_get()->next, path, list, size);
	THREADID_POST

	return res;
}

static int threadid_removexattr(const char *path, const char *name)
{
	THREADID_PRE
	int res = fuse_fs_removexattr(threadid_get()->next, path, name);
	THREADID_POST

	return res;
}

static int threadid_lock(const char *path, struct fuse_file_info *fi, int cmd,
			 struct flock *lock)
{
	THREADID_PRE
	int res = fuse_fs_lock(threadid_get()->next, path, fi, cmd, lock);
	THREADID_POST

	return res;
}

static int threadid_flock(const char *path, struct fuse_file_info *fi, int op)
{
	THREADID_PRE
	int res = fuse_fs_flock(threadid_get()->next, path, fi, op);
	THREADID_POST

	return res;
}

static int threadid_bmap(const char *path, size_t blocksize, uint64_t *idx)
{
	THREADID_PRE
	int res = fuse_fs_bmap(threadid_get()->next, path, blocksize, idx);
	THREADID_POST

	return res;
}

static off_t threadid_lseek(const char *path, off_t off, int whence,
			    struct fuse_file_info *fi)
{
	THREADID_PRE
	int res = fuse_fs_lseek(threadid_get()->next, path, off, whence, fi);
	THREADID_POST

	return res;
}

static int threadid_chflags(const char *path, struct fuse_file_info *fi,
			    unsigned int flags)
{
	THREADID_PRE
	int res = fuse_fs_chflags(threadid_get()->next, path, fi, flags);
	THREADID_POST

	return res;
}

static int threadid_setvolname(const char *name)
{
	THREADID_PRE
	int res = fuse_fs_setvolname(threadid_get()->next, name);
	THREADID_POST

	return res;
}

static void threadid_monitor(const char *path, uint32_t flags)
{
	THREADID_PRE
	fuse_fs_monitor(threadid_get()->next, path, flags);
	THREADID_POST
}

static void *threadid_init(struct fuse_conn_info *conn,
			   struct fuse_config *cfg)
{
	struct threadid *t = threadid_get();
	fuse_fs_init(t->next, conn, cfg);
	/* Don't touch cfg->nullpath_ok, we can work with
	   either */
	return t;
}

static void threadid_destroy(void *data)
{
	struct threadid *t = data;
	fuse_fs_destroy(t->next);
	free(t);
}

static const struct fuse_operations threadid_oper = {
	.destroy	= threadid_destroy,
	.init		= threadid_init,
#ifndef __APPLE__
	.getattr	= threadid_getattr,
#endif
#ifdef __APPLE__
	.setattr	= threadid_setattr,
#endif
	.access		= threadid_access,
	.readlink	= threadid_readlink,
	.opendir	= threadid_opendir,
#ifndef __APPLE__
	.readdir	= threadid_readdir,
#endif
	.releasedir	= threadid_releasedir,
	.mknod		= threadid_mknod,
	.mkdir		= threadid_mkdir,
	.symlink	= threadid_symlink,
	.unlink		= threadid_unlink,
	.rmdir		= threadid_rmdir,
	.rename		= threadid_rename,
	.link		= threadid_link,
	.chmod		= threadid_chmod,
	.chown		= threadid_chown,
	.truncate	= threadid_truncate,
#ifndef __APPLE__
	.utimens	= threadid_utimens,
#endif
	.create		= threadid_create,
	.open		= threadid_open,
	.read_buf	= threadid_read_buf,
	.write_buf	= threadid_write_buf,
#ifndef __APPLE__
	.statfs		= threadid_statfs,
#endif
	.flush		= threadid_flush,
	.release	= threadid_release,
	.fsync		= threadid_fsync,
	.fsyncdir	= threadid_fsyncdir,
#ifndef __APPLE__
	.setxattr	= threadid_setxattr,
	.getxattr	= threadid_getxattr,
#endif
	.listxattr	= threadid_listxattr,
	.removexattr	= threadid_removexattr,
	.lock		= threadid_lock,
	.flock		= threadid_flock,
	.bmap		= threadid_bmap,
	.lseek		= threadid_lseek,
#ifdef __APPLE__
	.chflags	= threadid_chflags,
	.setvolname	= threadid_setvolname,
	.monitor	= threadid_monitor,
#endif
};

static struct fuse_opt threadid_opts[] = {
	FUSE_OPT_KEY("-h", 0),
	FUSE_OPT_KEY("--help", 0),
	FUSE_OPT_END
};

static void threadid_help(void)
{
}

static int threadid_opt_proc(void *data, const char *arg, int key,
			   struct fuse_args *outargs)
{
	(void) data; (void) arg; (void) outargs;

	if (!key) {
		threadid_help();
		return -1;
	}

	return 1;
}

static struct fuse_fs *threadid_new(struct fuse_args *args,
				    struct fuse_fs *next[])
{
	struct fuse_operations oper = threadid_oper;
	struct fuse_fs *fs;
	struct threadid *t;

	t = calloc(1, sizeof(*t));
	if (t == NULL) {
		fuse_log(FUSE_LOG_ERR,
			 "fuse-threadid: memory allocation failed\n");
		return NULL;
	}

	if (fuse_opt_parse(args, t, threadid_opts, threadid_opt_proc) == -1)
		goto out_free;

	if (!next[0] || next[1]) {
		fuse_log(FUSE_LOG_ERR,
			 "fuse-threadid: exactly one next filesystem "
			 "required\n");
		goto out_free;
	}

	t->next = next[0];

	if (fuse_fs_darwin_extensions_enabled(next[0])) {
		oper.getattr.darwin = threadid_getattr$DARWIN;
		oper.readdir.darwin = threadid_readdir$DARWIN;
		oper.utimens.darwin = threadid_utimens$DARWIN;
		oper.statfs.darwin = threadid_statfs$DARWIN;
		oper.setxattr.darwin = threadid_setxattr$DARWIN;
		oper.getxattr.darwin = threadid_getxattr$DARWIN;
	} else {
		oper.getattr.vanilla = threadid_getattr;
		oper.readdir.vanilla = threadid_readdir;
		oper.utimens.vanilla = threadid_utimens;
		oper.statfs.vanilla = threadid_statfs;
		oper.setxattr.vanilla = threadid_setxattr;
		oper.getxattr.vanilla = threadid_getxattr;
	}
	fs = fuse_fs_new(&oper, sizeof(oper), t);
	if (!fs)
		goto out_free;

	return fs;

out_free:
	free(t);
	return NULL;
}

FUSE_REGISTER_MODULE(threadid, threadid_new);
