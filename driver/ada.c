/* SPDX-License-Identifier: MIT */
/* Table of measured two-byte revisions. Each row acts only on the exact file
 * whose whole-file SHA-256 was measured before or after the revision. */
#include "ada.h"
#include "patch.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const struct gs_ada_revision revisions[] = {
	{"module-gate",
	 "nvidia host kernel object (580.159.01, proprietary, CPU-side mode); revised state is the gate-only object, not the measured stack",
	 "580.159.01",
	 "4e3abe645b6873ed478adea6a06c6955352ff06186849edeea536c9630596449",
	 "ed3efee68582f4fa9f120cf6c28138d8642db93ff0e6ba07ab0d6d7bf1b9823b",
	 0xd654ef, {0x75, 0x7f}, {0x90, 0x90}},
	{"module-gate",
	 "nvidia host kernel object (580.159.01, proprietary, CPU-side mode); revised state is the measured stack",
	 "580.159.01",
	 "ce8a485bfaeebca3a9609f9415a6df81c160a0a53509aec093bc3361a6d12701",
	 "f5b788cdb3e06aed18860cc6cf950dcc13143bd831d90d8e9c4bf9e744426f12",
	 0xd654ef, {0x75, 0x7f}, {0x90, 0x90}},
	{"plugin-placement", "NVIDIA vGPU plugin shared library (580.159.01)", "580.159.01",
	 "7e52b76dc7ca8490e7ffbc1fd724f5db07f3791a3a2ec07f2d595bf05abdcaa7",
	 "097f571588f3a232265103b73e38ba111ddb44ef329ac720562cff71e6170d37",
	 0x8fb0e, {0xf8, 0x06}, {0x00, 0x5a}}
};
#define REVISION_COUNT (sizeof(revisions) / sizeof(revisions[0]))

static int fail(char *e, size_t n, const char *s)
{
	if (e && n)
		snprintf(e, n, "%s", s);
	return -1;
}

static int bytes_equal(const unsigned char *data, size_t site, const unsigned char want[2])
{
	return data[site] == want[0] && data[site + 1] == want[1];
}

/* Size has already been admitted; hash is the whole-file digest of data. */
static int classify(const struct gs_ada_revision *r, const unsigned char *data, const char *hash, char *err, size_t errlen)
{
	if (!strcmp(hash, r->input_sha)) {
		if (!bytes_equal(data, r->site, r->old_bytes))
			return fail(err, errlen, "input SHA-256 matches but the bytes at the revision site are not the measured original bytes");
		return 0;
	}
	if (!strcmp(hash, r->output_sha)) {
		if (!bytes_equal(data, r->site, r->new_bytes))
			return fail(err, errlen, "output SHA-256 matches but the bytes at the revision site are not the measured revised bytes");
		return 1;
	}
	return fail(err, errlen, "SHA-256 matches neither the measured input nor the measured output of this revision");
}

static int admit_size(const struct gs_ada_revision *r, const unsigned char *data, size_t size, char *err, size_t errlen)
{
	if (!r || (!data && size))
		return fail(err, errlen, "missing revision or data");
	if (r->site > size || size - r->site < 2)
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
 * previous two bytes if the resulting whole-file digest is not `expect`. */
static int rewrite(const struct gs_ada_revision *r, unsigned char *data, size_t size, int from,
		   const unsigned char to[2], const char *expect, char *err, size_t errlen)
{
	unsigned char prev[2];
	char hash[65];
	int st = gs_ada_state(r, data, size, err, errlen);

	if (st < 0)
		return -1;
	if (st != from)
		return fail(err, errlen, from == 0 ? "apply requires the unrevised input; this file is already revised"
						   : "revert requires the revised output; this file is not revised");
	prev[0] = data[r->site];
	prev[1] = data[r->site + 1];
	data[r->site] = to[0];
	data[r->site + 1] = to[1];
	gs_sha256(data, size, SIZE_MAX, hash);
	if (strcmp(hash, expect)) {
		data[r->site] = prev[0];
		data[r->site + 1] = prev[1];
		return fail(err, errlen, "resulting SHA-256 does not match the measured digest; buffer restored");
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

int gs_ada_find(const unsigned char *data, size_t size, const struct gs_ada_revision **out, int *state, char *err, size_t errlen)
{
	const struct gs_ada_revision *pick = NULL;
	char hash[65];
	size_t i, matches = 0;
	int st;

	if (!out || !state || (!data && size))
		return fail(err, errlen, "missing data or result pointers");
	gs_sha256(data, size, SIZE_MAX, hash);
	for (i = 0; i < REVISION_COUNT; i++) {
		if (!strcmp(hash, revisions[i].input_sha)) {
			pick = &revisions[i];
			matches++;
		}
		if (!strcmp(hash, revisions[i].output_sha)) {
			pick = &revisions[i];
			matches++;
		}
	}
	if (!matches)
		return fail(err, errlen, "no measured revision matches this file's SHA-256");
	if (matches > 1)
		return fail(err, errlen, "SHA-256 matches more than one measured revision; refusing ambiguous selection");
	if (admit_size(pick, data, size, err, errlen))
		return -1;
	st = classify(pick, data, hash, err, errlen);
	if (st < 0)
		return -1;
	*out = pick;
	*state = st;
	return 0;
}
