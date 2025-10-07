/*
  fuse iconv module: file name charset conversion
  Copyright (C) 2007  Miklos Szeredi <miklos@szeredi.hu>
  Copyright (C) 2006-2008  Amit Singh / Google Inc.
  Copyright (C) 2011-2025  Benjamin Fleischer

  This program can be distributed under the terms of the GNU LGPLv2.
  See the file COPYING.LIB
*/

#include <fuse_config.h>

#ifdef __APPLE__
#define FUSE_DARWIN_OVERLOAD_OPERATIONS 1
#endif

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <iconv.h>
#include <pthread.h>
#include <locale.h>
#include <langinfo.h>

struct iconv {
	struct fuse_fs *next;
	pthread_mutex_t lock;
	char *from_code;
	char *to_code;
	iconv_t tofs;
	iconv_t fromfs;
};

struct iconv_dh {
	struct iconv *ic;
	void *prev_buf;
#ifdef __APPLE__
	union {
		fuse_fill_dir_t vanilla;
		fuse_darwin_fill_dir_t darwin;
	} prev_filler;
#else
	fuse_fill_dir_t prev_filler;
#endif
};

static struct iconv *iconv_get(void)
{
	return fuse_get_context()->private_data;
}

static int iconv_convpath(struct iconv *ic, const char *path, char **newpathp,
			  int fromfs)
{
	size_t pathlen;
	size_t newpathlen;
	char *newpath;
	size_t plen;
	char *p;
	size_t res;
	int err;

	if (path == NULL) {
		*newpathp = NULL;
		return 0;
	}

	pathlen = strlen(path);
	newpathlen = pathlen * 4;
	newpath = malloc(newpathlen + 1);
	if (!newpath)
		return -ENOMEM;

	plen = newpathlen;
	p = newpath;
	pthread_mutex_lock(&ic->lock);
	do {
		res = iconv(fromfs ? ic->fromfs : ic->tofs, (char **) &path,
			    &pathlen, &p, &plen);
		if (res == (size_t) -1) {
			char *tmp;
			size_t inc;

			err = -EILSEQ;
			if (errno != E2BIG)
				goto err;

			inc = (pathlen + 1) * 4;
			newpathlen += inc;
			int dp = p - newpath;
			tmp = realloc(newpath, newpathlen + 1);
			err = -ENOMEM;
			if (!tmp)
				goto err;

			p = tmp + dp;
			plen += inc;
			newpath = tmp;
		}
	} while (res == (size_t) -1);
	pthread_mutex_unlock(&ic->lock);
	*p = '\0';
	*newpathp = newpath;
	return 0;

err:
	iconv(fromfs ? ic->fromfs : ic->tofs, NULL, NULL, NULL, NULL);
	pthread_mutex_unlock(&ic->lock);
	free(newpath);
	return err;
}

static int iconv_getattr(const char *path, struct stat *stbuf,
			 struct fuse_file_info *fi)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_getattr(ic->next, newpath, stbuf, fi);
		free(newpath);
	}
	return err;
}

#ifdef __APPLE__

static int iconv_getattr$DARWIN(const char *path, struct fuse_darwin_attr *attr,
				struct fuse_file_info *fi)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_getattr$DARWIN(ic->next, newpath, attr, fi);
		free(newpath);
	}
	return err;
}

static int iconv_setattr(const char *path, struct fuse_darwin_attr *attr,
			 int to_set, struct fuse_file_info *fi)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_setattr(ic->next, newpath, attr, to_set, fi);
		free(newpath);
	}
	return err;
}

#endif

static int iconv_access(const char *path, int mask)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_access(ic->next, newpath, mask);
		free(newpath);
	}
	return err;
}

static int iconv_readlink(const char *path, char *buf, size_t size)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_readlink(ic->next, newpath, buf, size);
		if (!err) {
			char *newlink;
			err = iconv_convpath(ic, buf, &newlink, 1);
			if (!err) {
				strncpy(buf, newlink, size - 1);
				buf[size - 1] = '\0';
				free(newlink);
			}
		}
		free(newpath);
	}
	return err;
}

static int iconv_opendir(const char *path, struct fuse_file_info *fi)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_opendir(ic->next, newpath, fi);
		free(newpath);
	}
	return err;
}

static int iconv_dir_fill(void *buf, const char *name,
			  const struct stat *stbuf, off_t off,
			  enum fuse_fill_dir_flags flags)
{
	struct iconv_dh *dh = buf;
	char *newname;
	int res = 0;
	if (iconv_convpath(dh->ic, name, &newname, 1) == 0) {
#ifdef __APPLE__
		res = dh->prev_filler.vanilla(dh->prev_buf, newname, stbuf, off,
					      flags);
#else
		res = dh->prev_filler(dh->prev_buf, newname, stbuf, off, flags);
#endif
		free(newname);
	}
	return res;
}

static int iconv_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi,
			 enum fuse_readdir_flags flags)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		struct iconv_dh dh;
		dh.ic = ic;
		dh.prev_buf = buf;
#ifdef __APPLE__
		dh.prev_filler.vanilla = filler;
#else
		dh.prev_filler = filler;
#endif
		err = fuse_fs_readdir(ic->next, newpath, &dh, iconv_dir_fill,
				      offset, fi, flags);
		free(newpath);
	}
	return err;
}

#ifdef __APPLE__

static int iconv_dir_fill$DARWIN(void *buf, const char *name,
				 const struct fuse_darwin_attr *attr,
				 off_t off, enum fuse_fill_dir_flags flags)
{
	struct iconv_dh *dh = buf;
	char *newname;
	int res = 0;
	if (iconv_convpath(dh->ic, name, &newname, 1) == 0) {
		res = dh->prev_filler.darwin(dh->prev_buf, newname, attr, off,
					     flags);
		free(newname);
	}
	return res;
}

static int iconv_readdir$DARWIN(const char *path, void *buf,
				fuse_darwin_fill_dir_t filler, off_t offset,
				struct fuse_file_info *fi,
				enum fuse_readdir_flags flags)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		struct iconv_dh dh;
		dh.ic = ic;
		dh.prev_buf = buf;
		dh.prev_filler.darwin = filler;
		err = fuse_fs_readdir$DARWIN(ic->next, newpath, &dh,
					     iconv_dir_fill$DARWIN, offset, fi,
					     flags);
		free(newpath);
	}
	return err;
}

#endif

static int iconv_releasedir(const char *path, struct fuse_file_info *fi)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_releasedir(ic->next, newpath, fi);
		free(newpath);
	}
	return err;
}

static int iconv_mknod(const char *path, mode_t mode, dev_t rdev)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_mknod(ic->next, newpath, mode, rdev);
		free(newpath);
	}
	return err;
}

static int iconv_mkdir(const char *path, mode_t mode)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_mkdir(ic->next, newpath, mode);
		free(newpath);
	}
	return err;
}

static int iconv_unlink(const char *path)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_unlink(ic->next, newpath);
		free(newpath);
	}
	return err;
}

static int iconv_rmdir(const char *path)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_rmdir(ic->next, newpath);
		free(newpath);
	}
	return err;
}

static int iconv_symlink(const char *from, const char *to)
{
	struct iconv *ic = iconv_get();
	char *newfrom;
	char *newto;
	int err = iconv_convpath(ic, from, &newfrom, 0);
	if (!err) {
		err = iconv_convpath(ic, to, &newto, 0);
		if (!err) {
			err = fuse_fs_symlink(ic->next, newfrom, newto);
			free(newto);
		}
		free(newfrom);
	}
	return err;
}

static int iconv_rename(const char *from, const char *to, unsigned int flags)
{
	struct iconv *ic = iconv_get();
	char *newfrom;
	char *newto;
	int err = iconv_convpath(ic, from, &newfrom, 0);
	if (!err) {
		err = iconv_convpath(ic, to, &newto, 0);
		if (!err) {
			err = fuse_fs_rename(ic->next, newfrom, newto, flags);
			free(newto);
		}
		free(newfrom);
	}
	return err;
}

static int iconv_link(const char *from, const char *to)
{
	struct iconv *ic = iconv_get();
	char *newfrom;
	char *newto;
	int err = iconv_convpath(ic, from, &newfrom, 0);
	if (!err) {
		err = iconv_convpath(ic, to, &newto, 0);
		if (!err) {
			err = fuse_fs_link(ic->next, newfrom, newto);
			free(newto);
		}
		free(newfrom);
	}
	return err;
}

static int iconv_chmod(const char *path, mode_t mode,
		       struct fuse_file_info *fi)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_chmod(ic->next, newpath, mode, fi);
		free(newpath);
	}
	return err;
}

static int iconv_chown(const char *path, uid_t uid, gid_t gid,
		       struct fuse_file_info *fi)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_chown(ic->next, newpath, uid, gid, fi);
		free(newpath);
	}
	return err;
}

static int iconv_truncate(const char *path, off_t size,
			   struct fuse_file_info *fi)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_truncate(ic->next, newpath, size, fi);
		free(newpath);
	}
	return err;
}

static int iconv_utimens(const char *path, const struct timespec ts[2],
			 struct fuse_file_info *fi)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_utimens(ic->next, newpath, ts, fi);
		free(newpath);
	}
	return err;
}

#ifdef __APPLE__

static int iconv_utimens$DARWIN(const char *path, const struct timespec ts[3],
				struct fuse_file_info *fi)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_utimens$DARWIN(ic->next, newpath, ts, fi);
		free(newpath);
	}
	return err;
}

#endif

static int iconv_create(const char *path, mode_t mode,
			struct fuse_file_info *fi)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_create(ic->next, newpath, mode, fi);
		free(newpath);
	}
	return err;
}

static int iconv_open_file(const char *path, struct fuse_file_info *fi)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_open(ic->next, newpath, fi);
		free(newpath);
	}
	return err;
}

static int iconv_read_buf(const char *path, struct fuse_bufvec **bufp,
			  size_t size, off_t offset, struct fuse_file_info *fi)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_read_buf(ic->next, newpath, bufp, size, offset, fi);
		free(newpath);
	}
	return err;
}

static int iconv_write_buf(const char *path, struct fuse_bufvec *buf,
			   off_t offset, struct fuse_file_info *fi)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_write_buf(ic->next, newpath, buf, offset, fi);
		free(newpath);
	}
	return err;
}

static int iconv_statfs(const char *path, struct statvfs *stbuf)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_statfs(ic->next, newpath, stbuf);
		free(newpath);
	}
	return err;
}

#ifdef __APPLE__

static int iconv_statfs$DARWIN(const char *path, struct statfs *stbuf)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_statfs$DARWIN(ic->next, newpath, stbuf);
		free(newpath);
	}
	return err;
}

#endif

static int iconv_flush(const char *path, struct fuse_file_info *fi)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_flush(ic->next, newpath, fi);
		free(newpath);
	}
	return err;
}

static int iconv_release(const char *path, struct fuse_file_info *fi)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_release(ic->next, newpath, fi);
		free(newpath);
	}
	return err;
}

static int iconv_fsync(const char *path, int isdatasync,
		       struct fuse_file_info *fi)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_fsync(ic->next, newpath, isdatasync, fi);
		free(newpath);
	}
	return err;
}

static int iconv_fsyncdir(const char *path, int isdatasync,
			  struct fuse_file_info *fi)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_fsyncdir(ic->next, newpath, isdatasync, fi);
		free(newpath);
	}
	return err;
}

static int iconv_setxattr(const char *path, const char *name,
			  const char *value, size_t size, int flags)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_setxattr(ic->next, newpath, name, value, size,
				       flags);
	}
	return err;
}

#ifdef __APPLE__

static int iconv_setxattr$DARWIN(const char *path, const char *name,
				 const char *value, size_t size, int flags,
				 unsigned int position)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_setxattr$DARWIN(ic->next, newpath, name, value,
					      size, flags, position);
		free(newpath);
	}
	return err;
}

#endif

static int iconv_getxattr(const char *path, const char *name, char *value,
			  size_t size)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_getxattr(ic->next, newpath, name, value, size);
		free(newpath);
	}
	return err;
}

#ifdef __APPLE__

static int iconv_getxattr$DARWIN(const char *path, const char *name,
				 char *value, size_t size,
				 unsigned int position)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_getxattr$DARWIN(ic->next, newpath, name, value,
					      size, position);
		free(newpath);
	}
	return err;
}

#endif

static int iconv_listxattr(const char *path, char *list, size_t size)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_listxattr(ic->next, newpath, list, size);
		free(newpath);
	}
	return err;
}

static int iconv_removexattr(const char *path, const char *name)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_removexattr(ic->next, newpath, name);
		free(newpath);
	}
	return err;
}

static int iconv_lock(const char *path, struct fuse_file_info *fi, int cmd,
		      struct flock *lock)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_lock(ic->next, newpath, fi, cmd, lock);
		free(newpath);
	}
	return err;
}

static int iconv_flock(const char *path, struct fuse_file_info *fi, int op)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_flock(ic->next, newpath, fi, op);
		free(newpath);
	}
	return err;
}

static int iconv_bmap(const char *path, size_t blocksize, uint64_t *idx)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_bmap(ic->next, newpath, blocksize, idx);
		free(newpath);
	}
	return err;
}

static off_t iconv_lseek(const char *path, off_t off, int whence,
			 struct fuse_file_info *fi)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int res = iconv_convpath(ic, path, &newpath, 0);
	if (!res) {
		res = fuse_fs_lseek(ic->next, newpath, off, whence, fi);
		free(newpath);
	}
	return res;
}

#ifdef __APPLE__

static int iconv_chflags(const char *path, struct fuse_file_info *fi,
			 unsigned int flags)
{
	struct iconv *ic = iconv_get();
	char *newpath;
	int err = iconv_convpath(ic, path, &newpath, 0);
	if (!err) {
		err = fuse_fs_chflags(ic->next, newpath, fi, flags);
		free(newpath);
	}
	return err;
}

static int iconv_setvolname(const char *name)
{
	struct iconv *ic = iconv_get();
	char *newname;
	int err = iconv_convpath(ic, name, &newname, 0);
	if (!err) {
		err = fuse_fs_setvolname(ic->next, newname);
		free(newname);
	}
	return err;
}

#endif

static void *iconv_init(struct fuse_conn_info *conn,
			struct fuse_config *cfg)
{
	struct iconv *ic = iconv_get();
	fuse_fs_init(ic->next, conn, cfg);
	/* Don't touch cfg->nullpath_ok, we can work with
	   either */
	return ic;
}

static void iconv_destroy(void *data)
{
	struct iconv *ic = data;
	fuse_fs_destroy(ic->next);
	iconv_close(ic->tofs);
	iconv_close(ic->fromfs);
	pthread_mutex_destroy(&ic->lock);
	free(ic->from_code);
	free(ic->to_code);
	free(ic);
}

static const struct fuse_operations iconv_oper = {
	.destroy	= iconv_destroy,
	.init		= iconv_init,
#ifndef __APPLE__
	.getattr	= iconv_getattr,
#endif
#ifdef __APPLE__
	.setattr	= iconv_setattr,
#endif
	.access		= iconv_access,
	.readlink	= iconv_readlink,
	.opendir	= iconv_opendir,
#ifndef __APPLE__
	.readdir	= iconv_readdir,
#endif
	.releasedir	= iconv_releasedir,
	.mknod		= iconv_mknod,
	.mkdir		= iconv_mkdir,
	.symlink	= iconv_symlink,
	.unlink		= iconv_unlink,
	.rmdir		= iconv_rmdir,
	.rename		= iconv_rename,
	.link		= iconv_link,
	.chmod		= iconv_chmod,
	.chown		= iconv_chown,
	.truncate	= iconv_truncate,
#ifndef __APPLE__
	.utimens	= iconv_utimens,
#endif
	.create		= iconv_create,
	.open		= iconv_open_file,
	.read_buf	= iconv_read_buf,
	.write_buf	= iconv_write_buf,
#ifndef __APPLE__
	.statfs		= iconv_statfs,
#endif
	.flush		= iconv_flush,
	.release	= iconv_release,
	.fsync		= iconv_fsync,
	.fsyncdir	= iconv_fsyncdir,
#ifndef __APPLE__
	.setxattr	= iconv_setxattr,
	.getxattr	= iconv_getxattr,
#endif
	.listxattr	= iconv_listxattr,
	.removexattr	= iconv_removexattr,
	.lock		= iconv_lock,
	.flock		= iconv_flock,
	.bmap		= iconv_bmap,
	.lseek		= iconv_lseek,
#ifdef __APPLE__
	.chflags	= iconv_chflags,
	.setvolname	= iconv_setvolname,
#endif
};

static const struct fuse_opt iconv_opts[] = {
	FUSE_OPT_KEY("-h", 0),
	FUSE_OPT_KEY("--help", 0),
	{ "from_code=%s", offsetof(struct iconv, from_code), 0 },
	{ "to_code=%s", offsetof(struct iconv, to_code), 1 },
	FUSE_OPT_END
};

static void iconv_help(void)
{
	char *charmap;
	const char *old = setlocale(LC_CTYPE, "");

	charmap = strdup(nl_langinfo(CODESET));
	if (old)
		setlocale(LC_CTYPE, old);
	else
		perror("setlocale");

	printf(
"    -o from_code=CHARSET   original encoding of file names (default: UTF-8)\n"
"    -o to_code=CHARSET     new encoding of the file names (default: %s)\n",
		charmap);
	free(charmap);
}

static int iconv_opt_proc(void *data, const char *arg, int key,
			  struct fuse_args *outargs)
{
	(void) data; (void) arg; (void) outargs;

	if (!key) {
		iconv_help();
		return -1;
	}

	return 1;
}

static struct fuse_fs *iconv_new(struct fuse_args *args,
				 struct fuse_fs *next[])
{
	struct fuse_fs *fs;
	struct iconv *ic;
	const char *old = NULL;
	const char *from;
	const char *to;

	ic = calloc(1, sizeof(struct iconv));
	if (ic == NULL) {
		fuse_log(FUSE_LOG_ERR, "fuse-iconv: memory allocation failed\n");
		return NULL;
	}

	if (fuse_opt_parse(args, ic, iconv_opts, iconv_opt_proc) == -1)
		goto out_free;

	if (!next[0] || next[1]) {
		fuse_log(FUSE_LOG_ERR, "fuse-iconv: exactly one next filesystem required\n");
		goto out_free;
	}

	from = ic->from_code ? ic->from_code : "UTF-8";
	to = ic->to_code ? ic->to_code : "";
	/* FIXME: detect charset equivalence? */
	if (!to[0])
		old = setlocale(LC_CTYPE, "");
	ic->tofs = iconv_open(from, to);
	if (ic->tofs == (iconv_t) -1) {
		fuse_log(FUSE_LOG_ERR, "fuse-iconv: cannot convert from %s to %s\n",
			to, from);
		goto out_free;
	}
	ic->fromfs = iconv_open(to, from);
	if (ic->tofs == (iconv_t) -1) {
		fuse_log(FUSE_LOG_ERR, "fuse-iconv: cannot convert from %s to %s\n",
			from, to);
		goto out_iconv_close_to;
	}
	if (old) {
		setlocale(LC_CTYPE, old);
		old = NULL;
	}

	ic->next = next[0];
#ifdef __APPLE__
	{
		struct fuse_operations oper = iconv_oper;
		if (fuse_fs_darwin_extensions_enabled(next[0])) {
			oper.getattr.darwin = iconv_getattr$DARWIN;
			oper.readdir.darwin = iconv_readdir$DARWIN;
			oper.utimens.darwin = iconv_utimens$DARWIN;
			oper.statfs.darwin = iconv_statfs$DARWIN;
			oper.setxattr.darwin = iconv_setxattr$DARWIN;
			oper.getxattr.darwin = iconv_getxattr$DARWIN;
		} else {
			oper.getattr.vanilla = iconv_getattr;
			oper.readdir.vanilla = iconv_readdir;
			oper.utimens.vanilla = iconv_utimens;
			oper.statfs.vanilla = iconv_statfs;
			oper.setxattr.vanilla = iconv_setxattr;
			oper.getxattr.vanilla = iconv_getxattr;
		}
		fs = fuse_fs_new(&oper, sizeof(oper), ic);
	}
#else
	fs = fuse_fs_new(&iconv_oper, sizeof(iconv_oper), ic);
#endif
	if (!fs)
		goto out_iconv_close_from;

	return fs;

out_iconv_close_from:
	iconv_close(ic->fromfs);
out_iconv_close_to:
	iconv_close(ic->tofs);
out_free:
	free(ic->from_code);
	free(ic->to_code);
	free(ic);
	if (old) {
		setlocale(LC_CTYPE, old);
	}
	return NULL;
}

FUSE_REGISTER_MODULE(iconv, iconv_new);
