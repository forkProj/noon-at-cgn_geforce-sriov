/* SPDX-License-Identifier: MIT */
#ifndef GS_ADA_H
#define GS_ADA_H
#include <stddef.h>
/* One measured revision of 1 to 16 bytes in a user-supplied file, admitted only by
 * the exact SHA-256 of the whole file before (input_sha) or after (output_sha). */
struct gs_ada_revision {
	const char *name;          /* short stable identifier, e.g. "module-gate" */
	const char *artifact;      /* what kind of file this row applies to */
	const char *version;       /* version string of the measured build */
	const char *input_sha;     /* 64 lowercase hex, expected digest of the input file */
	const char *output_sha;    /* 64 lowercase hex, expected digest after the revision */
	size_t site;               /* byte offset of the first changed byte */
	unsigned char width;       /* number of bytes replaced, 1 to 16 */
	unsigned char old_bytes[16]; /* read only up to width */
	unsigned char new_bytes[16]; /* read only up to width */
};

size_t gs_ada_revision_count(void);
/* Returns NULL when index is out of range. */
const struct gs_ada_revision *gs_ada_revision_at(size_t index);

/* Returns 0 when data matches input_sha and the width bytes at site equal old_bytes;
 * 1 when data matches output_sha and the width bytes at site equal new_bytes;
 * -1 (refused, message in err) for anything else, including a width outside 1 to 16
 * or a size smaller than site+width. */
int gs_ada_state(const struct gs_ada_revision *r, const unsigned char *data, size_t size, char *err, size_t errlen);

/* Requires state 0. Writes new_bytes at site, then re-verifies output_sha over the whole buffer. */
int gs_ada_apply(const struct gs_ada_revision *r, unsigned char *data, size_t size, char *err, size_t errlen);

/* Requires state 1. Writes old_bytes at site, then re-verifies input_sha over the whole buffer. */
int gs_ada_revert(const struct gs_ada_revision *r, unsigned char *data, size_t size, char *err, size_t errlen);

/* Lookup direction for gs_ada_find. Consecutive rows may chain: one row's output_sha can be
 * the next row's input_sha, so the direction decides which row a digest selects. */
enum { GS_ADA_ANY = 0, GS_ADA_FORWARD = 1, GS_ADA_BACKWARD = 2 };

/* Selects the table row whose digest equals that of data, by mode:
 * GS_ADA_FORWARD the row whose input_sha matches (*state 0), to be applied;
 * GS_ADA_BACKWARD the row whose output_sha matches (*state 1), to be reverted;
 * GS_ADA_ANY an input_sha match if one exists, else an output_sha match.
 * Then requires the width bytes at site to equal old_bytes (state 0) or new_bytes (state 1).
 * Refuses when no row matches in that direction, when more than one does, when the file is
 * smaller than site+width, or when the digest matches but the site bytes do not. */
int gs_ada_find(const unsigned char *data, size_t size, int mode,
		const struct gs_ada_revision **out, int *state, char *err, size_t errlen);

/* Applies exactly the rules of gs_ada_find, over a caller-supplied table of count rows
 * instead of the shipped one, so the lookup can be exercised against constructed rows.
 * Also refuses rows == NULL with count > 0. gs_ada_find is this over the shipped table. */
int gs_ada_find_in(const struct gs_ada_revision *rows, size_t count, const unsigned char *data, size_t size,
		   int mode, const struct gs_ada_revision **out, int *state, char *err, size_t errlen);
#endif
