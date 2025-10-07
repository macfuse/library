/*
  fuse volicon module: custom volume icon support for macOS
  Copyright (C) 2006-2008  Amit Singh / Google Inc.
  Copyright (C) 2012-2025  Benjamin Fleischer

  - Original "volicon" code by Amit Singh
  - Made into a libfuse stack module by Andrew de los Reyes
  - xattr'ification and overhaul by Amit Singh

  This program can be distributed under the terms of the GNU LGPLv2.
  See the file COPYING.LIB
*/

#include <fuse_config.h>

#ifdef __APPLE__
#define FUSE_DARWIN_OVERLOAD_OPERATIONS 1
#endif

#include <fuse.h>
#include <errno.h>
#include <libgen.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/attr.h>
#include <sys/xattr.h>
#include <sys/vnode.h>

#define VOLICON_ROOT_MAGIC_PATH "/"
#define VOLICON_ICON_MAGIC_PATH "/.VolumeIcon.icns"
#define VOLICON_ICON_MAXSIZE (4 * 1024 * 1024)

#define VOLICON_ACCESS_MASK (R_OK | _READ_OK | _RATTR_OK | _REXT_OK | _RPERM_OK)

struct FndrGenericInfo {
	u_int32_t   ignored0;
	u_int32_t   ignored1;
	u_int16_t   flags;
	struct {
		int16_t ignored2;
		int16_t ignored3;
	} fdLocation;
	int16_t     ignored4;
} __attribute__((aligned(2), packed));
typedef struct FndrGenericInfo FndrGenericInfo;

#define kHasCustomIcon 0x0400

static const char finder_info[32] = {
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
};
#define XATTR_FINDERINFO_SIZE 32

#define ERROR_IF_MAGIC_FILE(path, e) \
	if (volicon_is_a_magic_file(path)) { \
		return -e; \
	}

struct volicon {
	char *volicon;
	char *volicon_data;
	off_t volicon_size;
	uid_t volicon_uid;
	time_t volicon_time;
	struct fuse_fs *next;
};

static struct volicon *volicon_get(void)
{
	return fuse_get_context()->private_data;
}

static inline int volicon_is_icon_magic_file(const char *path)
{
	if (!path)
		return 0;

	return (!strcmp(path, VOLICON_ICON_MAGIC_PATH));
}

static inline int volicon_is_a_magic_file(const char *path)
{
	return (volicon_is_icon_magic_file(path));
}

static int volicon_getattr(const char *path, struct stat *stbuf,
			   struct fuse_file_info *fi)
{
	int res = 0;

	if (volicon_is_icon_magic_file(path)) {
		memset((void *)stbuf, 0, sizeof(*stbuf));

		stbuf->st_mode = S_IFREG | 0444;
		stbuf->st_nlink = 1;
		stbuf->st_uid = volicon_get()->volicon_uid;
		stbuf->st_gid = 0;
		stbuf->st_size = volicon_get()->volicon_size;
		stbuf->st_atimespec.tv_sec = volicon_get()->volicon_time;
		stbuf->st_ctimespec = stbuf->st_atimespec;
		stbuf->st_mtimespec = stbuf->st_atimespec;
	} else {
		res = fuse_fs_getattr(volicon_get()->next, path, stbuf, fi);
	}

	return res;
}

static int volicon_getattr$DARWIN(const char *path,
				  struct fuse_darwin_attr *attr,
				  struct fuse_file_info *fi)
{
	int res = 0;

	if (volicon_is_icon_magic_file(path)) {
		memset((void *)attr, 0, sizeof(*attr));

		attr->mode = S_IFREG | 0444;
		attr->nlink = 1;
		attr->uid = volicon_get()->volicon_uid;
		attr->gid = 0;
		attr->size = volicon_get()->volicon_size;
		attr->atimespec.tv_sec = volicon_get()->volicon_time;
		attr->ctimespec = attr->atimespec;
		attr->mtimespec = attr->atimespec;
	} else {
		res = fuse_fs_getattr$DARWIN(volicon_get()->next, path, attr,
					     fi);
	}

	return res;
}

static int volicon_setattr(const char *path, struct fuse_darwin_attr *attr,
			   int to_set, struct fuse_file_info *fi)
{
	ERROR_IF_MAGIC_FILE(path, EACCES);

	return fuse_fs_setattr(volicon_get()->next, path, attr, to_set, fi);
}

static int volicon_access(const char *path, int mask)
{
	if (volicon_is_a_magic_file(path)) {
		if (mask & ~VOLICON_ACCESS_MASK)
			return -EACCES;

		return 0;
	}

	return fuse_fs_access(volicon_get()->next, path, mask);
}

static int volicon_readlink(const char *path, char *buf, size_t size)
{
	ERROR_IF_MAGIC_FILE(path, EINVAL);

	return fuse_fs_readlink(volicon_get()->next, path, buf, size);
}

static int volicon_opendir(const char *path, struct fuse_file_info *fi)
{
	ERROR_IF_MAGIC_FILE(path, ENOTDIR);

	return fuse_fs_opendir(volicon_get()->next, path, fi);
}

static int volicon_readdir(const char *path, void *buf,
			   fuse_fill_dir_t filler, off_t offset,
			   struct fuse_file_info *fi,
			   enum fuse_readdir_flags flags)
{
	ERROR_IF_MAGIC_FILE(path, ENOTDIR);

	return fuse_fs_readdir(volicon_get()->next, path, buf, filler, offset,
			       fi, flags);
}

static int volicon_readdir$DARWIN(const char *path, void *buf,
				  fuse_darwin_fill_dir_t filler, off_t offset,
				  struct fuse_file_info *fi,
				  enum fuse_readdir_flags flags)
{
	ERROR_IF_MAGIC_FILE(path, ENOTDIR);

	return fuse_fs_readdir$DARWIN(volicon_get()->next, path, buf, filler,
				      offset, fi, flags);
}

static int volicon_releasedir(const char *path, struct fuse_file_info *fi)
{
	ERROR_IF_MAGIC_FILE(path, ENOTDIR);

	return fuse_fs_releasedir(volicon_get()->next, path, fi);
}

static int volicon_mknod(const char *path, mode_t mode, dev_t rdev)
{
	ERROR_IF_MAGIC_FILE(path, EEXIST);

	return fuse_fs_mknod(volicon_get()->next, path, mode, rdev);
}

static int volicon_mkdir(const char *path, mode_t mode)
{
	ERROR_IF_MAGIC_FILE(path, EEXIST);

	return fuse_fs_mkdir(volicon_get()->next, path, mode);
}

static int volicon_unlink(const char *path)
{
	ERROR_IF_MAGIC_FILE(path, EACCES);

	return fuse_fs_unlink(volicon_get()->next, path);
}

static int volicon_rmdir(const char *path)
{
	ERROR_IF_MAGIC_FILE(path, ENOTDIR);

	return fuse_fs_rmdir(volicon_get()->next, path);
}

static int volicon_symlink(const char *from, const char *path)
{
	ERROR_IF_MAGIC_FILE(path, EEXIST);

	return fuse_fs_symlink(volicon_get()->next, from, path);
}

static int volicon_rename(const char *from, const char *to, unsigned int flags)
{
	ERROR_IF_MAGIC_FILE(from, EACCES);
	ERROR_IF_MAGIC_FILE(to, EACCES);

	return fuse_fs_rename(volicon_get()->next, from, to, flags);
}

static int volicon_link(const char *from, const char *to)
{
	ERROR_IF_MAGIC_FILE(from, EACCES);
	ERROR_IF_MAGIC_FILE(to, EACCES);

	return fuse_fs_link(volicon_get()->next, from, to);
}

static int volicon_chmod(const char *path, mode_t mode,
			 struct fuse_file_info *fi)
{
	ERROR_IF_MAGIC_FILE(path, EACCES);

	return fuse_fs_chmod(volicon_get()->next, path, mode, fi);
}

static int volicon_chown(const char *path, uid_t uid, gid_t gid,
			 struct fuse_file_info *fi)
{
	ERROR_IF_MAGIC_FILE(path, EACCES);

	return fuse_fs_chown(volicon_get()->next, path, uid, gid, fi);
}

static int volicon_truncate(const char *path, off_t size,
			    struct fuse_file_info *fi)
{
	ERROR_IF_MAGIC_FILE(path, EACCES);

	return fuse_fs_truncate(volicon_get()->next, path, size, fi);
}

static int volicon_utimens(const char *path, const struct timespec ts[2],
			  struct fuse_file_info *fi)
{
	ERROR_IF_MAGIC_FILE(path, EACCES);

	return fuse_fs_utimens(volicon_get()->next, path, ts, fi);
}

#ifdef __APPLE__

static int volicon_utimens$DARWIN(const char *path, const struct timespec ts[3],
				  struct fuse_file_info *fi)
{
	ERROR_IF_MAGIC_FILE(path, EACCES);

	return fuse_fs_utimens$DARWIN(volicon_get()->next, path, ts, fi);
}

#endif

static int volicon_create(const char *path, mode_t mode,
			 struct fuse_file_info *fi)
{
	ERROR_IF_MAGIC_FILE(path, EEXIST);

	return fuse_fs_create(volicon_get()->next, path, mode, fi);
}

static int volicon_open(const char *path, struct fuse_file_info *fi)
{
	if (volicon_is_a_magic_file(path)) {
		if (fi && ((fi->flags & O_ACCMODE) != O_RDONLY))
			return -EACCES;

		return 0;
	}

	return fuse_fs_open(volicon_get()->next, path, fi);
}

static int volicon_read_buf(const char *path, struct fuse_bufvec **bufp,
			    size_t size, off_t offset, struct fuse_file_info *fi)
{
	int res = 0;

	if (volicon_is_icon_magic_file(path)) {
		struct fuse_bufvec *buf;
		void *mem;
		size_t a_size;

		buf = malloc(sizeof(**bufp));
		if (buf == NULL) {
			return -ENOMEM;
		}

		if ((offset + size) <= volicon_get()->volicon_size) {
			a_size = size;
		} else if (offset < volicon_get()->volicon_size) {
			a_size = volicon_get()->volicon_size - offset;
		} else {
			a_size = 0;
		}

		mem = malloc(a_size);
		if (mem == NULL) {
			free(buf);
			return -ENOMEM;
		}
		memcpy(mem, (char *)(volicon_get()->volicon_data) + offset,
		       a_size);

		*buf = FUSE_BUFVEC_INIT(size);
		buf->buf[0].mem = mem;
		buf->buf[0].size = a_size;
		*bufp = buf;
	} else {
		res = fuse_fs_read_buf(volicon_get()->next, path, bufp, size,
				       offset, fi);
	}

	return res;
}

static int volicon_write_buf(const char *path, struct fuse_bufvec *buf,
			     off_t offset, struct fuse_file_info *fi)
{
	ERROR_IF_MAGIC_FILE(path, EACCES);

	return fuse_fs_write_buf(volicon_get()->next, path, buf, offset, fi);
}

static int volicon_statfs(const char *path, struct statvfs *stbuf)
{
	if (volicon_is_a_magic_file(path)) {
		return fuse_fs_statfs(volicon_get()->next, "/", stbuf);
	}

	return fuse_fs_statfs(volicon_get()->next, path, stbuf);
}

static int volicon_statfs$DARWIN(const char *path, struct statfs *stbuf)
{
	if (volicon_is_a_magic_file(path)) {
		return fuse_fs_statfs$DARWIN(volicon_get()->next, "/", stbuf);
	}

	return fuse_fs_statfs$DARWIN(volicon_get()->next, path, stbuf);
}

static int volicon_flush(const char *path, struct fuse_file_info *fi)
{
	ERROR_IF_MAGIC_FILE(path, 0);

	return fuse_fs_flush(volicon_get()->next, path, fi);
}

static int volicon_release(const char *path, struct fuse_file_info *fi)
{
	ERROR_IF_MAGIC_FILE(path, 0);

	return fuse_fs_release(volicon_get()->next, path, fi);
}

static int volicon_fsync(const char *path, int isdatasync,
			struct fuse_file_info *fi)
{
	ERROR_IF_MAGIC_FILE(path, 0);

	return fuse_fs_fsync(volicon_get()->next, path, isdatasync, fi);
}

static int volicon_fsyncdir(const char *path, int isdatasync,
			   struct fuse_file_info *fi)
{
	ERROR_IF_MAGIC_FILE(path, ENOTDIR);

	return fuse_fs_fsyncdir(volicon_get()->next, path, isdatasync, fi);
}

static int volicon_setxattr(const char *path, const char *name,
			   const char *value, size_t size, int flags)
{
	ERROR_IF_MAGIC_FILE(path, EPERM);

	if (strcmp(path, VOLICON_ROOT_MAGIC_PATH) == 0
	    && strcmp(name, XATTR_FINDERINFO_NAME) == 0) {
		if (size >= 8 && size <= XATTR_FINDERINFO_SIZE) {
			char finder_info[XATTR_FINDERINFO_SIZE];
			memcpy(finder_info, value, size);
			((struct FndrGenericInfo *)&finder_info)->flags |=
				ntohs(0x0400);
			//finder_info[8] |= 0x100;
			return fuse_fs_setxattr(volicon_get()->next, path, name,
						finder_info, size, flags);
		}
	}

	return fuse_fs_setxattr(volicon_get()->next, path, name, value, size,
				flags);
}

static int volicon_setxattr$DARWIN(const char *path, const char *name,
				   const char *value, size_t size, int flags,
				   unsigned int position)
{
	ERROR_IF_MAGIC_FILE(path, EPERM);

	if (strcmp(path, VOLICON_ROOT_MAGIC_PATH) == 0
	    && strcmp(name, XATTR_FINDERINFO_NAME) == 0) {
		if (size >= 8 && size <= XATTR_FINDERINFO_SIZE) {
			char finder_info[XATTR_FINDERINFO_SIZE];
			memcpy(finder_info, value, size);
			((struct FndrGenericInfo *)&finder_info)->flags |=
				ntohs(0x0400);
			//finder_info[8] |= 0x100;
			return fuse_fs_setxattr$DARWIN(
				volicon_get()->next, path, name, finder_info,
				size, flags, position);
		}
	}

	return fuse_fs_setxattr$DARWIN(volicon_get()->next, path, name, value,
				       size, flags, position);
}

static int volicon_getxattr(const char *path, const char *name, char *value,
			    size_t size)
{
	ERROR_IF_MAGIC_FILE(path, ENOATTR);

	int res = 0;

	if (strcmp(path, VOLICON_ROOT_MAGIC_PATH) == 0
	    && strcmp(name, XATTR_FINDERINFO_NAME) == 0) {
		if (!size || !value)
			return XATTR_FINDERINFO_SIZE;
		if (size < XATTR_FINDERINFO_SIZE)
			return -ERANGE;

		res = fuse_fs_getxattr(volicon_get()->next, path, name, value,
				       size);

		if (res != XATTR_FINDERINFO_SIZE)
			memcpy(value, finder_info, XATTR_FINDERINFO_SIZE);
		((struct FndrGenericInfo *)value)->flags |= ntohs(0x0400);
		return XATTR_FINDERINFO_SIZE;
	}

	res = fuse_fs_getxattr(volicon_get()->next, path, name, value, size);

	if (res == -ENOSYS)
		res = -ENOTSUP;
	return res;
}

static int volicon_getxattr$DARWIN(const char *path, const char *name,
				   char *value, size_t size,
				   unsigned int position)
{
	ERROR_IF_MAGIC_FILE(path, ENOATTR);

	int res = 0;

	if (strcmp(path, VOLICON_ROOT_MAGIC_PATH) == 0
	    && strcmp(name, XATTR_FINDERINFO_NAME) == 0) {
		if (!size || !value)
			return XATTR_FINDERINFO_SIZE;
		if (size < XATTR_FINDERINFO_SIZE)
			return -ERANGE;

		res = fuse_fs_getxattr$DARWIN(volicon_get()->next, path, name,
					      value, size, position);

		if (res != XATTR_FINDERINFO_SIZE)
			memcpy(value, finder_info, XATTR_FINDERINFO_SIZE);
		((struct FndrGenericInfo *)value)->flags |= ntohs(0x0400);
		return XATTR_FINDERINFO_SIZE;
	}

	res = fuse_fs_getxattr$DARWIN(volicon_get()->next, path, name, value,
				      size, position);

	if (res == -ENOSYS)
		res = -ENOTSUP;
	return res;
}

static int volicon_listxattr(const char *path, char *list, size_t size)
{
	ERROR_IF_MAGIC_FILE(path, 0);

	struct fuse_fs *next = volicon_get()->next;
	int res = fuse_fs_listxattr(next, path, list, size);

	if (strcmp(path, VOLICON_ROOT_MAGIC_PATH) == 0) {
		int done = 0;

		if (res == -ENOSYS)
			res = 0;

		if (!list) { /* size being queried */
			ssize_t sz = 0;
			int r = fuse_fs_getxattr$DARWIN(
				next, VOLICON_ROOT_MAGIC_PATH,
				XATTR_FINDERINFO_NAME, NULL, 0, 0);
			if (r < 0) {
				sz += sizeof(XATTR_FINDERINFO_NAME);
			}
			if (res > 0) {
				sz += res;
			}
			return sz;
		}

		/* list is good */

		if (res == -ERANGE)
			return -ERANGE;

		if (res > 0) {
			size_t len = 0;
			char *curr = list;
			do {
				size_t thislen = strlen(curr) + 1;
				if (strcmp(curr, XATTR_FINDERINFO_NAME) == 0) {
					done = 1;
					break;
				}
				curr += thislen;
				len += thislen;
			} while (len < res);
		}

		if (done)
			return res;

		if (size < res + sizeof(XATTR_FINDERINFO_NAME))
			return -ERANGE;

		memcpy((char *)list + res, XATTR_FINDERINFO_NAME,
		       sizeof(XATTR_FINDERINFO_NAME));

		return (res + sizeof(XATTR_FINDERINFO_NAME));
	}

	if (res == -ENOSYS)
		res = -ENOTSUP;
	return res;
}

static int volicon_removexattr(const char *path, const char *name)
{
	ERROR_IF_MAGIC_FILE(path, EPERM);

	if ((strcmp(path, VOLICON_ROOT_MAGIC_PATH) == 0) &&
	    (strcmp(name, XATTR_FINDERINFO_NAME) == 0)) {
		return -EACCES;
	}

	return fuse_fs_removexattr(volicon_get()->next, path, name);
}

static int volicon_lock(const char *path, struct fuse_file_info *fi, int cmd,
		        struct flock *lock)
{
	ERROR_IF_MAGIC_FILE(path, ENOTSUP);

	return fuse_fs_lock(volicon_get()->next, path, fi, cmd, lock);
}

static int volicon_flock(const char *path, struct fuse_file_info *fi, int op)
{
	ERROR_IF_MAGIC_FILE(path, ENOTSUP);

	return fuse_fs_flock(volicon_get()->next, path, fi, op);
}

static int volicon_bmap(const char *path, size_t blocksize, uint64_t *idx)
{
	ERROR_IF_MAGIC_FILE(path, ENOTSUP);

	return fuse_fs_bmap(volicon_get()->next, path, blocksize, idx);
}

static off_t volicon_lseek(const char *path, off_t off, int whence,
			   struct fuse_file_info *fi)
{
	ERROR_IF_MAGIC_FILE(path, ENOTSUP);

	return fuse_fs_lseek(volicon_get()->next, path, off, whence, fi);
}

static int volicon_chflags(const char *path, struct fuse_file_info *fi,
			  unsigned int flags)
{
	ERROR_IF_MAGIC_FILE(path, EACCES);

	return fuse_fs_chflags(volicon_get()->next, path, fi, flags);
}

static int volicon_setvolname(const char *name)
{
	return fuse_fs_setvolname(volicon_get()->next, name);
}

static void *volicon_init(struct fuse_conn_info *conn,
			  struct fuse_config *cfg)
{
	struct volicon *v = volicon_get();
	fuse_fs_init(v->next, conn, cfg);
	/* Don't touch cfg->nullpath_ok, we can work with
	   either */
	return v;
}

static void volicon_destroy(void *data)
{
	struct volicon *v = data;
	fuse_fs_destroy(v->next);
	free(v->volicon);
	free(v->volicon_data);
	free(v);
}

static const struct fuse_operations volicon_oper = {
	.destroy	= volicon_destroy,
	.init		= volicon_init,
#ifndef __APPLE__
	.getattr	= volicon_getattr,
#endif
#ifdef __APPLE__
	.setattr	= volicon_setattr,
#endif
	.access		= volicon_access,
	.readlink	= volicon_readlink,
	.opendir	= volicon_opendir,
#ifndef __APPLE__
	.readdir	= volicon_readdir,
#endif
	.releasedir	= volicon_releasedir,
	.mknod		= volicon_mknod,
	.mkdir		= volicon_mkdir,
	.symlink	= volicon_symlink,
	.unlink		= volicon_unlink,
	.rmdir		= volicon_rmdir,
	.rename		= volicon_rename,
	.link		= volicon_link,
	.chmod		= volicon_chmod,
	.chown		= volicon_chown,
	.truncate	= volicon_truncate,
#ifndef __APPLE__
	.utimens	= volicon_utimens,
#endif
	.create		= volicon_create,
	.open		= volicon_open,
	.read_buf	= volicon_read_buf,
	.write_buf	= volicon_write_buf,
#ifndef __APPLE__
	.statfs		= volicon_statfs,
#endif
	.flush		= volicon_flush,
	.release	= volicon_release,
	.fsync		= volicon_fsync,
	.fsyncdir	= volicon_fsyncdir,
#ifndef __APPLE__
	.setxattr	= volicon_setxattr,
	.getxattr	= volicon_getxattr,
#endif
	.listxattr	= volicon_listxattr,
	.removexattr	= volicon_removexattr,
	.lock		= volicon_lock,
	.flock		= volicon_flock,
	.bmap		= volicon_bmap,
	.lseek		= volicon_lseek,
#ifdef __APPLE__
	.chflags	= volicon_chflags,
	.setvolname	= volicon_setvolname,
#endif
};

static const struct fuse_opt volicon_opts[] = {
	FUSE_OPT_KEY("-h", 0),
	FUSE_OPT_KEY("--help", 0),
	{ "iconpath=%s", offsetof(struct volicon, volicon), 0 },
	FUSE_OPT_END
};

static void volicon_help(void)
{
	fprintf(stderr,
"    -o iconpath=<icon path> display volume with custom icon\n");
}

static int volicon_opt_proc(void *data, const char *arg, int key,
			   struct fuse_args *outargs)
{
	(void) data; (void) arg; (void) outargs;

	if (!key) {
		volicon_help();
		return -1;
	}

	return 1;
}

static struct fuse_fs *volicon_new(struct fuse_args *args,
				   struct fuse_fs *next[])
{
	int fd = -1;
	int res;
	struct stat sb;
	struct fuse_operations oper = volicon_oper;
	struct fuse_fs *fs;
	struct volicon *v;

	v = calloc(1, sizeof(*v));
	if (v == NULL) {
		fuse_log(FUSE_LOG_ERR,
			 "fuse-volicon: memory allocation failed\n");
		return NULL;
	}

	if (fuse_opt_parse(args, v, volicon_opts, volicon_opt_proc) == -1)
		goto out_free;

	if (!next[0] || next[1]) {
		fuse_log(FUSE_LOG_ERR,
			 "fuse-volicon: exactly one next filesystem "
			 "required\n");
		goto out_free;
	}

	if (!v->volicon) {
		fuse_log(FUSE_LOG_ERR,
			 "fuse-volicon: missing 'iconpath' option\n");
		goto out_free;
	}

	fd = open(v->volicon, O_RDONLY);
	if (fd < 0) {
		fuse_log(FUSE_LOG_ERR,
			 "fuse-volicon: failed to access volume icon file "
			 "(%d)\n",
			 errno);
		goto out_free;
	}

	res = fstat(fd, &sb);
	if (res) {
		fuse_log(FUSE_LOG_ERR,
			 "fuse-volicon: failed to stat volume icon file (%d)\n",
			 errno);
		goto out_free;
	}

	if (sb.st_size > (VOLICON_ICON_MAXSIZE)) {
		fuse_log(FUSE_LOG_ERR,
			 "fuse-volicon: size limit exceeded for volume icon "
			 "file\n");
		goto out_free;
	}

	v->volicon_data = malloc(sb.st_size);
	if (!v->volicon_data) {
		fuse_log(FUSE_LOG_ERR,
			 "fuse-volicon: failed to allocate memory for volume "
			 "icon data\n");
		goto out_free;
	}

	res = read(fd, v->volicon_data, sb.st_size);
	if (res != sb.st_size) {
		fuse_log(FUSE_LOG_ERR,
			 "fuse-volicon: failed to read data from volume icon "
			 "file\n");
		goto out_free;
	}

	close(fd);
	fd = -1;

	v->volicon_size = sb.st_size;
	v->volicon_uid = getuid();
	v->volicon_time = time(NULL);

	v->next = next[0];

	if (fuse_fs_darwin_extensions_enabled(next[0])) {
		oper.getattr.darwin = volicon_getattr$DARWIN;
		oper.readdir.darwin = volicon_readdir$DARWIN;
		oper.utimens.darwin = volicon_utimens$DARWIN;
		oper.statfs.darwin = volicon_statfs$DARWIN;
		oper.setxattr.darwin = volicon_setxattr$DARWIN;
		oper.getxattr.darwin = volicon_getxattr$DARWIN;
	} else {
		oper.getattr.vanilla = volicon_getattr;
		oper.readdir.vanilla = volicon_readdir;
		oper.utimens.vanilla = volicon_utimens;
		oper.statfs.vanilla = volicon_statfs;
		oper.setxattr.vanilla = volicon_setxattr;
		oper.getxattr.vanilla = volicon_getxattr;
	}
	fs = fuse_fs_new(&oper, sizeof(oper), v);
	if (!fs)
		goto out_free;

	return fs;

out_free:
	if (fd >= 0)
		close(fd);
	if (v->volicon_data)
		free(v->volicon_data);
	if (v->volicon)
		free(v->volicon);
	free(v);
	return NULL;
}

FUSE_REGISTER_MODULE(volicon, volicon_new);
