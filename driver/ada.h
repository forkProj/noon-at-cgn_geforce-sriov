/* SPDX-License-Identifier: MIT */
#ifndef GS_ADA_H
#define GS_ADA_H
#include <stddef.h>
/* One measured two-byte revision of a user-supplied file, admitted only by the
 * exact SHA-256 of the whole file before (input_sha) or after (output_sha). */
struct gs_ada_revision {
	const char *name;          /* short stable identifier, e.g. "module-gate" */
	const char *artifact;      /* what kind of file this row applies to */
	const char *version;       /* version string of the measured build */
	const char *input_sha;     /* 64 lowercase hex, expected digest of the input file */
	const char *output_sha;    /* 64 lowercase hex, expected digest after the revision */
	size_t site;               /* byte offset of the two changed bytes */
	unsigned char old_bytes[2];
	unsigned char new_bytes[2];
};

size_t gs_ada_revision_count(void);
/* Returns NULL when index is out of range. */
const struct gs_ada_revision *gs_ada_revision_at(size_t index);

/* Returns 0 when data matches input_sha and the two bytes at site equal old_bytes;
 * 1 when data matches output_sha and the two bytes at site equal new_bytes;
 * -1 (refused, message in err) for anything else, including a size smaller than site+2. */
int gs_ada_state(const struct gs_ada_revision *r, const unsigned char *data, size_t size, char *err, size_t errlen);

/* Requires state 0. Writes new_bytes at site, then re-verifies output_sha over the whole buffer. */
int gs_ada_apply(const struct gs_ada_revision *r, unsigned char *data, size_t size, char *err, size_t errlen);

/* Requires state 1. Writes old_bytes at site, then re-verifies input_sha over the whole buffer. */
int gs_ada_revert(const struct gs_ada_revision *r, unsigned char *data, size_t size, char *err, size_t errlen);

/* Selects the single table row whose input_sha or output_sha equals the digest of data.
 * *state receives 0 or 1 as above. Refuses when no row matches or when more than one row matches. */
int gs_ada_find(const unsigned char *data, size_t size, const struct gs_ada_revision **out, int *state, char *err, size_t errlen);
#endif
