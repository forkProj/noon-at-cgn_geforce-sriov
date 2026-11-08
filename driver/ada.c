/* SPDX-License-Identifier: MIT */
/* Table of measured revisions of 1 to 16 bytes. Each row acts only on the exact
 * file whose whole-file SHA-256 was measured before or after the revision. */
#include "ada.h"
#include "patch.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const struct gs_ada_revision revisions[] = {
	{"module-gate",
	 "nvidia host kernel object (580.159.01, proprietary, CPU-side mode); input state is the stock object; output state is the gate-only object, not the supported stack",
	 "580.159.01",
	 "4e3abe645b6873ed478adea6a06c6955352ff06186849edeea536c9630596449",
	 "ed3efee68582f4fa9f120cf6c28138d8642db93ff0e6ba07ab0d6d7bf1b9823b",
	 0xd654ef, 2, {0x75, 0x7f}, {0x90, 0x90}},
	{"module-gate",
	 "nvidia host kernel object (580.159.01, proprietary, CPU-side mode); input state is the object with the scheduler revision; output state is the supported stack",
	 "580.159.01",
	 "ce8a485bfaeebca3a9609f9415a6df81c160a0a53509aec093bc3361a6d12701",
	 "f5b788cdb3e06aed18860cc6cf950dcc13143bd831d90d8e9c4bf9e744426f12",
	 0xd654ef, 2, {0x75, 0x7f}, {0x90, 0x90}},
	{"vf-count",
	 "nvidia host kernel object (580.159.01, proprietary, CPU-side mode); input state carries the probe gate and the scheduler revision; output state reports 4 total VFs",
	 "580.159.01",
	 "f5b788cdb3e06aed18860cc6cf950dcc13143bd831d90d8e9c4bf9e744426f12",
	 "946b27b8bcf0f08285335f47904516860c65b726619e5a36d89785981c70ba29",
	 0x126644, 4, {0x0f, 0xb7, 0x45, 0x06}, {0x31, 0xc0, 0xb0, 0x04}},
	{"plugin-mixed",
	 "NVIDIA vGPU plugin shared library (580.159.01); input state is the stock plugin; output state is the input of plugin-div4, not the installed copy",
	 "580.159.01",
	 "7e52b76dc7ca8490e7ffbc1fd724f5db07f3791a3a2ec07f2d595bf05abdcaa7",
	 "7ddfad458bbfdb0c4d6902a9e045bd15c84f63aea19314f3d3a60967224b73c6",
	 0x8fc17, 15,
	 {0x4c, 0x39, 0xc1, 0x0f, 0x84, 0x08, 0x03, 0x00, 0x00, 0x48, 0x8d, 0x05, 0x91, 0x78, 0x2d},
	 {0x48, 0x8b, 0x93, 0x60, 0x2b, 0x00, 0x00, 0x48, 0x89, 0x4a, 0x08, 0xe9, 0x01, 0x03, 0x00}},
	{"plugin-div4",
	 "NVIDIA vGPU plugin shared library (580.159.01); input state carries plugin-mixed; output state divides the Ada host framebuffer by 4 VFs and is the patched plugin copy for Ada and Ampere hosts",
	 "580.159.01",
	 "7ddfad458bbfdb0c4d6902a9e045bd15c84f63aea19314f3d3a60967224b73c6",
	 "aa4fdeb5288b5e56e7d4972466813d78e10cfb428fcf62fb53b84fec718cf07c",
	 0xa5f28, 6, {0x8b, 0xbb, 0xd0, 0x29, 0x00, 0x00}, {0xbf, 0x04, 0x00, 0x00, 0x00, 0x90}}
};
#define REVISION_COUNT (sizeof(revisions) / sizeof(revisions[0]))

static int fail(char *e, size_t n, const char *s)
{
	if (e && n)
		snprintf(e, n, "%s", s);
	return -1;
}

/* Width has already been admitted by admit_size. */
static int bytes_equal(const struct gs_ada_revision *r, const unsigned char *data, const unsigned char *want)
{
	return !memcmp(data + r->site, want, r->width);
}

/* Size has already been admitted; hash is the whole-file digest of data. */
static int classify(const struct gs_ada_revision *r, const unsigned char *data, const char *hash, char *err, size_t errlen)
{
	if (!strcmp(hash, r->input_sha)) {
		if (!bytes_equal(r, data, r->old_bytes))
			return fail(err, errlen, "input SHA-256 matches but the bytes at the revision site are not the recorded original bytes");
		return 0;
	}
	if (!strcmp(hash, r->output_sha)) {
		if (!bytes_equal(r, data, r->new_bytes))
			return fail(err, errlen, "output SHA-256 matches but the bytes at the revision site are not the recorded revised bytes");
		return 1;
	}
	return fail(err, errlen, "SHA-256 matches neither the recorded input nor the recorded output of this revision");
}

static int admit_size(const struct gs_ada_revision *r, const unsigned char *data, size_t size, char *err, size_t errlen)
{
	if (!r || (!data && size))
		return fail(err, errlen, "missing revision or data");
	if (r->width == 0 || r->width > sizeof(r->old_bytes))
		return fail(err, errlen, "revision width is outside 1 to 16 bytes");
	if (r->site > size || size - r->site < r->width)
		return fail(err, errlen, "file is smaller than the revision site");
	return 0;
}

size_t gs_ada_revision_count(void)
{
	return REVISION_COUNT;
}

const struct gs_ada_revision *gs_ada_revision_at(size_t index)
{
	return index < REVISION_COUNT ? &revisions[index] : NULL;
}

int gs_ada_state(const struct gs_ada_revision *r, const unsigned char *data, size_t size, char *err, size_t errlen)
{
	char hash[65];

	if (admit_size(r, data, size, err, errlen))
		return -1;
	gs_sha256(data, size, SIZE_MAX, hash);
	return classify(r, data, hash, err, errlen);
}

/* Moves the buffer from state `from` to the other state, restoring the
 * previous width bytes if the resulting whole-file digest is not `expect`. */
static int rewrite(const struct gs_ada_revision *r, unsigned char *data, size_t size, int from,
		   const unsigned char *to, const char *expect, char *err, size_t errlen)
{
	unsigned char prev[sizeof(r->old_bytes)];
	char hash[65];
	int st = gs_ada_state(r, data, size, err, errlen);

	if (st < 0)
		return -1;
	if (st != from)
		return fail(err, errlen, from == 0 ? "apply requires the unrevised input; this file is already revised"
						   : "revert requires the revised output; this file is not revised");
	memcpy(prev, data + r->site, r->width);
	memcpy(data + r->site, to, r->width);
	gs_sha256(data, size, SIZE_MAX, hash);
	if (strcmp(hash, expect)) {
		memcpy(data + r->site, prev, r->width);
		return fail(err, errlen, "resulting SHA-256 does not match the recorded digest; buffer restored");
	}
	return 0;
}

int gs_ada_apply(const struct gs_ada_revision *r, unsigned char *data, size_t size, char *err, size_t errlen)
{
	if (!r)
		return fail(err, errlen, "missing revision");
	return rewrite(r, data, size, 0, r->new_bytes, r->output_sha, err, errlen);
}

int gs_ada_revert(const struct gs_ada_revision *r, unsigned char *data, size_t size, char *err, size_t errlen)
{
	if (!r)
		return fail(err, errlen, "missing revision");
	return rewrite(r, data, size, 1, r->old_bytes, r->input_sha, err, errlen);
}

/* Returns the row whose input_sha (state 0) or output_sha (state 1) equals hash, or NULL;
 * *many is set when more than one row matches on that side. */
static const struct gs_ada_revision *match(const struct gs_ada_revision *rows, size_t count,
					   const char *hash, int state, int *many)
{
	const struct gs_ada_revision *pick = NULL;
	size_t i;

	*many = 0;
	for (i = 0; i < count; i++) {
		if (strcmp(hash, state ? rows[i].output_sha : rows[i].input_sha))
			continue;
		if (pick)
			*many = 1;
		pick = &rows[i];
	}
	return pick;
}

int gs_ada_find_in(const struct gs_ada_revision *rows, size_t count, const unsigned char *data, size_t size,
		   int mode, const struct gs_ada_revision **out, int *state, char *err, size_t errlen)
{
	const struct gs_ada_revision *pick = NULL;
	char hash[65];
	int st = 0, many = 0;

	if (!out || !state || (!data && size) || (!rows && count))
		return fail(err, errlen, "missing data, table or result pointers");
	if (mode != GS_ADA_ANY && mode != GS_ADA_FORWARD && mode != GS_ADA_BACKWARD)
		return fail(err, errlen, "unknown lookup direction");
	gs_sha256(data, size, SIZE_MAX, hash);
	if (mode != GS_ADA_BACKWARD)
		pick = match(rows, count, hash, 0, &many);
	if (!pick && mode != GS_ADA_FORWARD) {
		pick = match(rows, count, hash, 1, &many);
		st = 1;
	}
	if (!pick) {
		if (mode == GS_ADA_FORWARD && match(rows, count, hash, 1, &many))
			return fail(err, errlen, "already at output state and no supported revision takes this file as input; refusing to repatch or create output");
		if (mode == GS_ADA_BACKWARD && match(rows, count, hash, 0, &many))
			return fail(err, errlen, "not at output state of any supported revision; refusing to revert or create output");
		return fail(err, errlen, "no supported revision matches this file's SHA-256");
	}
	if (many)
		return fail(err, errlen, "SHA-256 matches more than one supported revision in this direction; refusing ambiguous selection");
	if (admit_size(pick, data, size, err, errlen))
		return -1;
	if (!bytes_equal(pick, data, st ? pick->new_bytes : pick->old_bytes))
		return fail(err, errlen, st ? "output SHA-256 matches but the bytes at the revision site are not the recorded revised bytes"
					    : "input SHA-256 matches but the bytes at the revision site are not the recorded original bytes");
	*out = pick;
	*state = st;
	return 0;
}

int gs_ada_find(const unsigned char *data, size_t size, int mode,
		const struct gs_ada_revision **out, int *state, char *err, size_t errlen)
{
	return gs_ada_find_in(revisions, REVISION_COUNT, data, size, mode, out, state, err, errlen);
}
