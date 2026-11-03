/* SPDX-License-Identifier: MIT */
#ifndef GS_SAFE_OUTPUT_H
#define GS_SAFE_OUTPUT_H

#include <stddef.h>

/* Return values of gs_write_new_output(). */
#define GS_OUTPUT_DURABLE 0	/* data and directory entry fsynced */
#define GS_OUTPUT_DIR_UNSYNCED 1	/* data fsynced; directory fsync unsupported, err holds a warning */
#define GS_OUTPUT_FAILED (-1)	/* failed; err says whether a partial file was left (see below) */

/* Permission bits requested for a new output file, further reduced by the umask. */
#define GS_OUTPUT_MODE 0644

/*
 * Create path as a new regular file holding exactly size bytes of data.
 *
 * The final path component is created with O_CREAT|O_EXCL|O_NOFOLLOW, so an
 * existing file, hard link of the input, or symlink (even a dangling one) at
 * path is never opened, followed, truncated or replaced; that case fails with
 * errno EEXIST. Directory components of path are resolved normally. The
 * parent directory is opened first and all later steps are relative to it.
 *
 * Writes retry EINTR and short writes, then fsync(2) and close(2) the file and
 * fsync(2) the parent directory. If any step fails, the file created by this
 * call is unlinked when its name still refers to it, err describes the failed
 * step, the byte count reached and the cleanup outcome, errno is set, and
 * GS_OUTPUT_FAILED is returned. Cleanup can itself fail, or be skipped when the
 * name no longer refers to the created file or its identity is unknown, so a
 * failure can leave a partial file at path; err says which case occurred.
 *
 * A caller that reports status after a successful return (as
 * geforce-sriov-patch does on stdout) can still fail with the complete output
 * already in place; its diagnostic must say so.
 *
 * GS_OUTPUT_DIR_UNSYNCED is returned when the file is complete and fsynced
 * but the parent directory cannot be fsynced: the filesystem rejects fsync on
 * directories (EINVAL/ENOTSUP), or the directory is writable but not readable
 * and was opened with O_PATH (EACCES). The new name may then be lost by a
 * crash and err explains this. errno is then set to the reason the directory
 * could not be fsynced even though the output itself succeeded; it is not a
 * failure indication. Callers test `< 0` for failure.
 *
 * Durability means only that the kernel and filesystem acknowledged fsync;
 * storage that ignores flush requests is outside what this call can verify.
 * err may be NULL when errlen is 0.
 */
int gs_write_new_output(const char *path, const unsigned char *data, size_t size, char *err, size_t errlen);

#endif
