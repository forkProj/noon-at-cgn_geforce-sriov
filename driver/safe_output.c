// SPDX-License-Identifier: MIT
/* Exclusive, fsynced creation of a new output file; see safe_output.h. */
#include "safe_output.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Consecutive EINTR results tolerated per system call before giving up. */
#define MAX_EINTR_RETRIES 64
/* Largest single write request; keeps the count well inside ssize_t. */
#define MAX_WRITE_CHUNK ((size_t)1 << 30)

static void set_err(char *err, size_t errlen, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
	va_list ap;

	if (!err || errlen == 0)
		return;
	va_start(ap, fmt);
	vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
}

/* Split path into its parent directory (copied to dir) and final component. */
static int split_path(const char *path, char *dir, size_t dirlen, const char **base)
{
	const char *slash = strrchr(path, '/');
	size_t n;

	if (!slash) {
		*base = path;
		if (dirlen < 2)
			return -ENAMETOOLONG;
		memcpy(dir, ".", 2);
	} else {
		*base = slash + 1;
		n = slash == path ? 1 : (size_t)(slash - path);
		if (n >= dirlen)
			return -ENAMETOOLONG;
		memcpy(dir, path, n);
		dir[n] = '\0';
	}
	if (**base == '\0' || strcmp(*base, ".") == 0 || strcmp(*base, "..") == 0)
		return -EISDIR;
	return 0;
}

static int fsync_retry(int fd)
{
	unsigned int tries = 0;

	while (fsync(fd) < 0) {
		if (errno != EINTR || ++tries >= MAX_EINTR_RETRIES)
			return -errno;
	}
	return 0;
}

/* Unlink base only while it still names the inode this call created. */
static int remove_created(int dfd, const char *base, const struct stat *created)
{
	struct stat now;

	if (fstatat(dfd, base, &now, AT_SYMLINK_NOFOLLOW) < 0)
		return -errno;
	if (now.st_dev != created->st_dev || now.st_ino != created->st_ino)
		return -ESTALE;
	if (unlinkat(dfd, base, 0) < 0)
		return -errno;
	return 0;
}

static int dir_fsync_unsupported(int e)
{
	if (e == EINVAL || e == ENOTSUP)
		return 1;
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
	if (e == EOPNOTSUPP)
		return 1;
#endif
	return 0;
}

int gs_write_new_output(const char *path, const unsigned char *data, size_t size, char *err, size_t errlen)
{
	char dir[PATH_MAX];
	char cleanup[160];
	const char *base;
	const char *stage;
	struct stat created;
	int have_created = 0;
	unsigned int tries = 0;
	size_t off = 0;
	int dir_syncable = 1;
	int dfd, fd, e, r;

	if (!path || !*path || (!data && size)) {
		set_err(err, errlen, "output: invalid arguments");
		errno = EINVAL;
		return GS_OUTPUT_FAILED;
	}
	if (strlen(path) >= PATH_MAX) {
		set_err(err, errlen, "output path is longer than %d bytes", PATH_MAX - 1);
		errno = ENAMETOOLONG;
		return GS_OUTPUT_FAILED;
	}
	r = split_path(path, dir, sizeof(dir), &base);
	if (r) {
		set_err(err, errlen, "%s: %s", path,
			r == -EISDIR ? "does not name a file (empty, '.' or '..' final component)" : strerror(-r));
		errno = -r;
		return GS_OUTPUT_FAILED;
	}

	dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
#ifdef O_PATH
	/*
	 * Creating needs only write+search permission on the directory; a
	 * directory without read permission still works through an O_PATH
	 * descriptor, which cannot be fsynced.
	 */
	if (dfd < 0 && errno == EACCES) {
		dfd = open(dir, O_PATH | O_DIRECTORY | O_CLOEXEC);
		if (dfd >= 0)
			dir_syncable = 0;
		else
			errno = EACCES;
	}
#endif
	if (dfd < 0) {
		e = errno;
		set_err(err, errlen, "%s: cannot open output directory %s: %s", path, dir, strerror(e));
		errno = e;
		return GS_OUTPUT_FAILED;
	}

	fd = openat(dfd, base, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, GS_OUTPUT_MODE);
	if (fd < 0) {
		e = errno;
		if (e == EEXIST)
			set_err(err, errlen, "%s: already exists; refusing to replace an existing file or symlink",
				path);
		else
			set_err(err, errlen, "%s: cannot create: %s", path, strerror(e));
		close(dfd);
		errno = e;
		return GS_OUTPUT_FAILED;
	}

	if (fstat(fd, &created) < 0) {
		e = errno;
		stage = "fstat";
		goto fail;
	}
	have_created = 1;

	while (off < size) {
		size_t chunk = size - off > MAX_WRITE_CHUNK ? MAX_WRITE_CHUNK : size - off;
		ssize_t n = write(fd, data + off, chunk);

		if (n < 0) {
			if (errno == EINTR && ++tries < MAX_EINTR_RETRIES)
				continue;
			e = errno;
			stage = "write";
			goto fail;
		}
		if (n == 0) {
			e = EIO;
			stage = "write (no progress)";
			goto fail;
		}
		tries = 0;
		off += (size_t)n;
	}

	r = fsync_retry(fd);
	if (r) {
		e = -r;
		stage = "fsync";
		goto fail;
	}
	/* Never retry close: on Linux the descriptor is released even on error. */
	r = close(fd);
	fd = -1;
	if (r < 0) {
		e = errno;
		stage = "close";
		goto fail;
	}

	if (!dir_syncable) {
		set_err(err, errlen,
			"%s: written and fsynced, but directory %s is not readable so it cannot be fsynced; "
			"the new name may not survive a crash",
			path, dir);
		close(dfd);
		errno = EACCES;
		return GS_OUTPUT_DIR_UNSYNCED;
	}
	r = fsync_retry(dfd);
	if (r) {
		e = -r;
		if (dir_fsync_unsupported(e)) {
			set_err(err, errlen,
				"%s: written and fsynced, but directory %s does not support fsync (%s); "
				"the new name may not survive a crash",
				path, dir, strerror(e));
			close(dfd);
			errno = e;
			return GS_OUTPUT_DIR_UNSYNCED;
		}
		stage = "directory fsync";
		goto fail;
	}
	close(dfd);
	return GS_OUTPUT_DURABLE;

fail:
	if (fd >= 0)
		close(fd);
	if (!have_created) {
		snprintf(cleanup, sizeof(cleanup), "left the new file in place because its identity is unknown");
	} else {
		r = remove_created(dfd, base, &created);
		if (r == 0)
			snprintf(cleanup, sizeof(cleanup), "removed the incomplete output");
		else if (r == -ENOENT)
			snprintf(cleanup, sizeof(cleanup), "the incomplete output is already gone");
		else if (r == -ESTALE)
			snprintf(cleanup, sizeof(cleanup), "the name now refers to another file; left it untouched");
		else
			snprintf(cleanup, sizeof(cleanup), "could not remove the incomplete output: %s",
				 strerror(-r));
	}
	set_err(err, errlen, "%s: %s failed after %zu of %zu bytes: %s; %s", path, stage, off, size, strerror(e),
		cleanup);
	close(dfd);
	errno = e;
	return GS_OUTPUT_FAILED;
}
