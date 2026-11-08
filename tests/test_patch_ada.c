/* SPDX-License-Identifier: MIT */
#include "ada.h"
#include "patch.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
/* The shipped rows are admitted only by digests of measured vendor files, which
 * cannot be reproduced synthetically. Block 1 checks the invariants of the
 * shipped table (shape, width, lowercase-hex digests, no digest repeated on the
 * same side of two rows, exactly two rows chained from another row's output,
 * every row's output digest pinned to its input digest, and the five measured
 * rows with their sites, widths of 1 to 16 bytes and byte pairs). Block 2 builds
 * its own two-byte row, a four-byte row chained after it and a fifteen-byte row
 * over a 64-byte buffer and drives
 * through them every accepting and refusing path of gs_ada_state, gs_ada_apply
 * and gs_ada_revert, and every mode of gs_ada_find_in, plus the argument and
 * unknown-digest refusals of gs_ada_find over the shipped table. Block 3 takes the
 * shipped plugin-mixed and plugin-div4 rows (sites, widths, bytes) with the digests
 * of a synthetic file that carries their original bytes at their file offsets, and
 * runs the chain stock -> mixed -> div4 and back, with its refusals.
 * Not covered here: gs_ada_find accepting a shipped row, and applying a shipped
 * row to its real file, both of which need the measured vendor files. Those are
 * left to the external real-file check. */
static int failures, checks;
/* Always evaluated, unlike assert(), so the checks also run under -DNDEBUG. */
#define CHECK(cond, ...) do { \
		checks++; \
		if (!(cond)) { \
			failures++; \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__); \
			fputc('\n', stderr); \
		} \
	} while (0)
enum { N = 64, SITE = 32, SITE4 = 40, SITE15 = 44 };
static const struct gs_ada_revision sentinel;
static char err[256];

/* Exactly 64 lowercase hex characters; stops at the first non-hex byte, so it
 * never reads past the terminator of a shorter string. */
static int hex64(const char *s)
{
	size_t i;

	if (!s)
		return 0;
	for (i = 0; i < 64; i++)
		if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
			return 0;
	return s[64] == '\0';
}

/* Clears the message buffer before a call so a stale message cannot pass. */
static char *fresh(void)
{
	err[0] = '\0';
	return err;
}

/* Refused, and the message names the expected reason. */
static int refused(int rc, const char *why)
{
	return rc < 0 && strstr(err, why) != NULL;
}

static int same(const unsigned char *a, const unsigned char *b)
{
	return memcmp(a, b, N) == 0;
}

/* gs_ada_find_in over rows selects rows[want] in state `state`. */
static int finds(const struct gs_ada_revision *rows, size_t count, const unsigned char *data, int mode, size_t want, int state)
{
	const struct gs_ada_revision *out = &sentinel;
	int st = 42;

	return gs_ada_find_in(rows, count, data, N, mode, &out, &st, fresh(), sizeof(err)) == 0 && out == &rows[want] && st == state;
}

/* gs_ada_find_in over rows refuses for the expected reason and writes no result. */
static int misses(const struct gs_ada_revision *rows, size_t count, const unsigned char *data, int mode, const char *why)
{
	const struct gs_ada_revision *out = &sentinel;
	int st = 42;

	return refused(gs_ada_find_in(rows, count, data, N, mode, &out, &st, fresh(), sizeof(err)), why) && out == &sentinel && st == 42;
}

static void table_invariants(void)
{
	static const struct {
		const char *input_sha, *output_sha;
		size_t site;
		unsigned char width, old_bytes[16], new_bytes[16];
	} measured[] = {
		{"4e3abe645b6873ed478adea6a06c6955352ff06186849edeea536c9630596449", "ed3efee68582f4fa9f120cf6c28138d8642db93ff0e6ba07ab0d6d7bf1b9823b", 0xd654ef, 2, {0x75, 0x7f}, {0x90, 0x90}},
		{"ce8a485bfaeebca3a9609f9415a6df81c160a0a53509aec093bc3361a6d12701", "f5b788cdb3e06aed18860cc6cf950dcc13143bd831d90d8e9c4bf9e744426f12", 0xd654ef, 2, {0x75, 0x7f}, {0x90, 0x90}},
		{"f5b788cdb3e06aed18860cc6cf950dcc13143bd831d90d8e9c4bf9e744426f12", "946b27b8bcf0f08285335f47904516860c65b726619e5a36d89785981c70ba29", 0x126644, 4, {0x0f, 0xb7, 0x45, 0x06}, {0x31, 0xc0, 0xb0, 0x04}},
		{"7e52b76dc7ca8490e7ffbc1fd724f5db07f3791a3a2ec07f2d595bf05abdcaa7", "7ddfad458bbfdb0c4d6902a9e045bd15c84f63aea19314f3d3a60967224b73c6", 0x8fc17, 15,
		 {0x4c, 0x39, 0xc1, 0x0f, 0x84, 0x08, 0x03, 0x00, 0x00, 0x48, 0x8d, 0x05, 0x91, 0x78, 0x2d},
		 {0x48, 0x8b, 0x93, 0x60, 0x2b, 0x00, 0x00, 0x48, 0x89, 0x4a, 0x08, 0xe9, 0x01, 0x03, 0x00}},
		{"7ddfad458bbfdb0c4d6902a9e045bd15c84f63aea19314f3d3a60967224b73c6", "aa4fdeb5288b5e56e7d4972466813d78e10cfb428fcf62fb53b84fec718cf07c", 0xa5f28, 6,
		 {0x8b, 0xbb, 0xd0, 0x29, 0x00, 0x00}, {0xbf, 0x04, 0x00, 0x00, 0x00, 0x90}},
	};
	size_t count = gs_ada_revision_count(), nmeasured = sizeof(measured) / sizeof(measured[0]), i, j, dupes = 0, links = 0;

	CHECK(count == nmeasured, "expected %zu revisions, got %zu", nmeasured, count);
	CHECK(gs_ada_revision_at(count) == NULL, "index equal to the count must be NULL");
	CHECK(gs_ada_revision_at(SIZE_MAX) == NULL, "index SIZE_MAX must be NULL");

	for (i = 0; i < count; i++) {
		const struct gs_ada_revision *x = gs_ada_revision_at(i);

		CHECK(x != NULL, "row %zu below the count is NULL", i);
		if (!x)
			continue;
		CHECK(x->name && x->artifact && x->version && x->input_sha && x->output_sha, "row %zu has a NULL field", i);
		CHECK(hex64(x->input_sha), "row %zu input_sha is not 64 lowercase hex characters", i);
		CHECK(hex64(x->output_sha), "row %zu output_sha is not 64 lowercase hex characters", i);
		if (x->input_sha && x->output_sha)
			CHECK(strcmp(x->input_sha, x->output_sha) != 0, "row %zu input_sha equals its output_sha", i);
		CHECK(x->width >= 1 && x->width <= sizeof(x->old_bytes), "row %zu width %u is outside 1 to 16", i, (unsigned)x->width);
		CHECK(x->width >= 1 && x->width <= sizeof(x->old_bytes) && memcmp(x->old_bytes, x->new_bytes, x->width) != 0,
		      "row %zu old_bytes equal its new_bytes over its width", i);
		CHECK(x->site != 0, "row %zu site is 0", i);
		/* Every row must be listed in measured[], so a new unlisted row or a
		 * wrong input/output pairing fails rather than being skipped. */
		for (j = 0; j < nmeasured; j++)
			if (x->input_sha && !strcmp(x->input_sha, measured[j].input_sha))
				break;
		CHECK(j < nmeasured && x->output_sha && !strcmp(x->output_sha, measured[j].output_sha),
		      "row %zu %s", i, j < nmeasured ? "output_sha is not the recorded output for its input digest" : "input_sha has no recorded output digest");
	}

	/* Same-row pairs are covered above; here every pair from different rows.
	 * No two rows share an input or an output, so each direction selects at
	 * most one row. An output may equal another row's input only where the
	 * measured stack chains revisions: module-gate's stack output into vf-count,
	 * and plugin-mixed's output into plugin-div4. */
	for (i = 0; i < count; i++)
		for (j = 0; j < count; j++) {
			const struct gs_ada_revision *a = gs_ada_revision_at(i), *b = gs_ada_revision_at(j);

			if (i == j || !a || !b || !a->input_sha || !a->output_sha || !b->input_sha || !b->output_sha)
				continue;
			if (j > i && !strcmp(a->input_sha, b->input_sha))
				dupes++;
			if (j > i && !strcmp(a->output_sha, b->output_sha))
				dupes++;
			if (!strcmp(a->output_sha, b->input_sha))
				links++;
		}
	CHECK(dupes == 0, "%zu digest pairs repeat on the same side across rows", dupes);
	CHECK(links == 2, "%zu rows take another row's output as input, expected exactly 2", links);

	for (i = 0; i < nmeasured; i++) {
		const struct gs_ada_revision *pick = NULL;
		size_t hits = 0;

		for (j = 0; j < count; j++) {
			const struct gs_ada_revision *x = gs_ada_revision_at(j);

			if (x && x->input_sha && !strcmp(x->input_sha, measured[i].input_sha)) {
				pick = x;
				hits++;
			}
		}
		CHECK(hits == 1, "measured row %.12s present %zu times, expected once", measured[i].input_sha, hits);
		if (hits != 1)
			continue;
		CHECK(pick->site == measured[i].site, "measured row %.12s site is not 0x%zx", measured[i].input_sha, measured[i].site);
		CHECK(pick->width == measured[i].width && !memcmp(pick->old_bytes, measured[i].old_bytes, measured[i].width) &&
		      !memcmp(pick->new_bytes, measured[i].new_bytes, measured[i].width),
		      "measured row %.12s is not width %u with the recorded old and new bytes over that width",
		      measured[i].input_sha, (unsigned)measured[i].width);
	}
}

static void synthetic_row(void)
{
	static const unsigned char old4[4] = {0x0f, 0xb7, 0x45, 0x06}, new4[4] = {0x31, 0xc0, 0xb0, 0x04};
	static const unsigned char old15[15] = {0x4c, 0x39, 0xc1, 0x0f, 0x84, 0x08, 0x03, 0x00, 0x00, 0x48, 0x8d, 0x05, 0x91, 0x78, 0x2d},
				   new15[15] = {0x48, 0x8b, 0x93, 0x60, 0x2b, 0x00, 0x00, 0x48, 0x89, 0x4a, 0x08, 0xe9, 0x01, 0x03, 0x00};
	static const int modes[] = {GS_ADA_ANY, GS_ADA_FORWARD, GS_ADA_BACKWARD};
	unsigned char orig[N], rev[N], chained[N], fifteen[N], buf[N];
	char in_sha[65], out_sha[65], next_sha[65], fif_sha[65], bad_in[65], bad_out[65];
	const struct gs_ada_revision *out = &sentinel;
	struct gs_ada_revision mine, next, long15, bad, skew, end, far, edge, wide[2], pair[2];
	size_t i;
	int st = 42;

	for (i = 0; i < N; i++)
		orig[i] = (unsigned char)(i * 37u + 0x5au);
	orig[SITE] = 0x11;
	orig[SITE + 1] = 0x22;
	memcpy(orig + SITE4, old4, sizeof(old4));
	memcpy(orig + SITE15, old15, sizeof(old15));
	memcpy(rev, orig, N);
	rev[SITE] = 0x33;
	rev[SITE + 1] = 0x44;
	memcpy(chained, rev, N);
	memcpy(chained + SITE4, new4, sizeof(new4));
	memcpy(fifteen, orig, N);
	memcpy(fifteen + SITE15, new15, sizeof(new15));
	gs_sha256(orig, N, SIZE_MAX, in_sha);
	gs_sha256(rev, N, SIZE_MAX, out_sha);
	gs_sha256(chained, N, SIZE_MAX, next_sha);
	gs_sha256(fifteen, N, SIZE_MAX, fif_sha);
	mine = (struct gs_ada_revision){
		.name = "synthetic", .artifact = "64-byte test buffer", .version = "none",
		.input_sha = in_sha, .output_sha = out_sha, .site = SITE, .width = 2,
		.old_bytes = {0x11, 0x22}, .new_bytes = {0x33, 0x44},
	};
	/* Chained after mine: its input digest is mine's output digest. */
	next = (struct gs_ada_revision){
		.name = "synthetic-4", .artifact = "64-byte test buffer", .version = "none",
		.input_sha = out_sha, .output_sha = next_sha, .site = SITE4, .width = 4,
		.old_bytes = {0x0f, 0xb7, 0x45, 0x06}, .new_bytes = {0x31, 0xc0, 0xb0, 0x04},
	};
	/* Fifteen bytes over the original, the width of the measured plugin row. */
	long15 = (struct gs_ada_revision){
		.name = "synthetic-15", .artifact = "64-byte test buffer", .version = "none",
		.input_sha = in_sha, .output_sha = fif_sha, .site = SITE15, .width = 15,
		.old_bytes = {0x4c, 0x39, 0xc1, 0x0f, 0x84, 0x08, 0x03, 0x00, 0x00, 0x48, 0x8d, 0x05, 0x91, 0x78, 0x2d},
		.new_bytes = {0x48, 0x8b, 0x93, 0x60, 0x2b, 0x00, 0x00, 0x48, 0x89, 0x4a, 0x08, 0xe9, 0x01, 0x03, 0x00},
	};

	/* 1. Classification of the two measured states. */
	CHECK(gs_ada_state(&mine, orig, N, fresh(), sizeof(err)) == 0, "original buffer is not state 0: %s", err);
	CHECK(gs_ada_state(&mine, rev, N, fresh(), sizeof(err)) == 1, "revised buffer is not state 1: %s", err);
	memcpy(buf, orig, N);
	buf[0] ^= 0x01;
	CHECK(refused(gs_ada_state(&mine, buf, N, fresh(), sizeof(err)), "neither"), "state accepted a buffer changed away from the site: %s", err);

	/* 2. Apply on the original. */
	memcpy(buf, orig, N);
	CHECK(gs_ada_apply(&mine, buf, N, fresh(), sizeof(err)) == 0, "apply on the original failed: %s", err);
	CHECK(buf[SITE] == 0x33 && buf[SITE + 1] == 0x44, "apply did not write 33 44 at the site");
	CHECK(gs_ada_state(&mine, buf, N, fresh(), sizeof(err)) == 1, "applied buffer is not state 1: %s", err);

	/* 3. Revert completes the round trip. */
	CHECK(gs_ada_revert(&mine, buf, N, fresh(), sizeof(err)) == 0, "revert of the applied buffer failed: %s", err);
	CHECK(same(buf, orig), "apply then revert is not byte-identical to the original");

	/* 4. Apply on an already revised buffer. */
	memcpy(buf, rev, N);
	CHECK(refused(gs_ada_apply(&mine, buf, N, fresh(), sizeof(err)), "already revised"), "apply on a revised buffer not refused as already revised: %s", err);
	CHECK(same(buf, rev), "refused apply changed the revised buffer");

	/* 5. Revert on an unrevised buffer. */
	memcpy(buf, orig, N);
	CHECK(refused(gs_ada_revert(&mine, buf, N, fresh(), sizeof(err)), "not revised"), "revert on an unrevised buffer not refused as not revised: %s", err);
	CHECK(same(buf, orig), "refused revert changed the original buffer");

	/* 6. Result digest mismatch restores the two bytes, in both directions. */
	memcpy(bad_out, out_sha, sizeof(bad_out));
	bad_out[0] = bad_out[0] == '0' ? '1' : '0';
	bad = mine;
	bad.output_sha = bad_out;
	memcpy(buf, orig, N);
	CHECK(refused(gs_ada_apply(&bad, buf, N, fresh(), sizeof(err)), "restored"), "apply with a wrong output digest not refused with restore: %s", err);
	CHECK(same(buf, orig), "apply with a wrong output digest did not restore the original bytes");
	memcpy(bad_in, in_sha, sizeof(bad_in));
	bad_in[0] = bad_in[0] == '0' ? '1' : '0';
	bad = mine;
	bad.input_sha = bad_in;
	memcpy(buf, rev, N);
	CHECK(refused(gs_ada_revert(&bad, buf, N, fresh(), sizeof(err)), "restored"), "revert with a wrong input digest not refused with restore: %s", err);
	CHECK(same(buf, rev), "revert with a wrong input digest did not restore the revised bytes");

	/* 7. Digest matches but the bytes at the site do not. */
	skew = mine;
	skew.old_bytes[1] = 0x23;
	CHECK(refused(gs_ada_state(&skew, orig, N, fresh(), sizeof(err)), "recorded original bytes"), "state accepted an input digest with other site bytes: %s", err);
	memcpy(buf, orig, N);
	CHECK(refused(gs_ada_apply(&skew, buf, N, fresh(), sizeof(err)), "recorded original bytes"), "apply accepted an input digest with other site bytes: %s", err);
	CHECK(same(buf, orig), "apply refused for site bytes changed the buffer");
	CHECK(misses(&skew, 1, orig, GS_ADA_FORWARD, "recorded original bytes"), "find_in accepted an input digest with other site bytes: %s", err);
	skew = mine;
	skew.new_bytes[1] = 0x45;
	CHECK(refused(gs_ada_state(&skew, rev, N, fresh(), sizeof(err)), "recorded revised bytes"), "state accepted an output digest with other site bytes: %s", err);

	/* 8. Size bound, with digests that match the buffer so only size can refuse:
	 * a width-2 row at site n-1 and a width-4 row at site n-3 are each one byte short. */
	end = mine;
	end.site = N - 1;
	end.old_bytes[0] = orig[N - 1];
	end.old_bytes[1] = 0;
	CHECK(refused(gs_ada_state(&end, orig, N, fresh(), sizeof(err)), "smaller"), "state at site n-1 not refused for size: %s", err);
	memcpy(buf, orig, N);
	CHECK(refused(gs_ada_apply(&end, buf, N, fresh(), sizeof(err)), "smaller"), "apply at site n-1 not refused for size: %s", err);
	CHECK(same(buf, orig), "apply refused for size changed the buffer");
	CHECK(refused(gs_ada_revert(&end, buf, N, fresh(), sizeof(err)), "smaller"), "revert at site n-1 not refused for size: %s", err);
	CHECK(same(buf, orig), "revert refused for size changed the buffer");
	CHECK(misses(&end, 1, orig, GS_ADA_FORWARD, "smaller"), "find_in at site n-1 not refused for size: %s", err);
	end = mine;
	end.site = N - 3;
	end.width = 4;
	memcpy(end.old_bytes, orig + N - 3, 3);
	end.old_bytes[3] = 0;
	CHECK(refused(gs_ada_state(&end, orig, N, fresh(), sizeof(err)), "smaller"), "width-4 state at site n-3 not refused for size: %s", err);
	memcpy(buf, orig, N);
	CHECK(refused(gs_ada_apply(&end, buf, N, fresh(), sizeof(err)), "smaller"), "width-4 apply at site n-3 not refused for size: %s", err);
	CHECK(same(buf, orig), "width-4 apply refused for size changed the buffer");
	CHECK(refused(gs_ada_revert(&end, buf, N, fresh(), sizeof(err)), "smaller"), "width-4 revert at site n-3 not refused for size: %s", err);
	CHECK(same(buf, orig), "width-4 revert refused for size changed the buffer");
	far = mine;
	far.site = SIZE_MAX;
	CHECK(refused(gs_ada_state(&far, orig, N, fresh(), sizeof(err)), "smaller"), "state at site SIZE_MAX not refused for size: %s", err);
	edge = mine;
	edge.site = N - 2;
	edge.old_bytes[0] = orig[N - 2];
	edge.old_bytes[1] = orig[N - 1];
	CHECK(gs_ada_state(&edge, orig, N, fresh(), sizeof(err)) == 0, "state at site n-2 with a matching digest not accepted: %s", err);
	edge.site = N - 4;
	edge.width = 4;
	memcpy(edge.old_bytes, orig + N - 4, 4);
	CHECK(gs_ada_state(&edge, orig, N, fresh(), sizeof(err)) == 0, "width-4 state at site n-4 with a matching digest not accepted: %s", err);

	/* 9. Width outside 1 to 16 is refused before digest or site bytes are read. */
	wide[0] = mine;
	wide[0].width = 0;
	wide[1] = mine;
	wide[1].width = 17;
	for (i = 0; i < 2; i++) {
		CHECK(refused(gs_ada_state(&wide[i], orig, N, fresh(), sizeof(err)), "1 to 16"), "state accepted width %u: %s", (unsigned)wide[i].width, err);
		memcpy(buf, orig, N);
		CHECK(refused(gs_ada_apply(&wide[i], buf, N, fresh(), sizeof(err)), "1 to 16") && same(buf, orig),
		      "apply with width %u not refused for width, or changed the buffer: %s", (unsigned)wide[i].width, err);
		memcpy(buf, rev, N);
		CHECK(refused(gs_ada_revert(&wide[i], buf, N, fresh(), sizeof(err)), "1 to 16") && same(buf, rev),
		      "revert with width %u not refused for width, or changed the buffer: %s", (unsigned)wide[i].width, err);
	}

	/* 10. Four-byte round trip over the revised buffer, and a four-byte restore. */
	CHECK(gs_ada_state(&next, rev, N, fresh(), sizeof(err)) == 0, "revised buffer is not state 0 of the four-byte row: %s", err);
	memcpy(buf, rev, N);
	CHECK(gs_ada_apply(&next, buf, N, fresh(), sizeof(err)) == 0, "four-byte apply failed: %s", err);
	CHECK(!memcmp(buf + SITE4, new4, sizeof(new4)), "four-byte apply did not write 31 c0 b0 04 at the site");
	CHECK(gs_ada_state(&next, buf, N, fresh(), sizeof(err)) == 1, "four-byte applied buffer is not state 1: %s", err);
	CHECK(gs_ada_revert(&next, buf, N, fresh(), sizeof(err)) == 0, "four-byte revert failed: %s", err);
	CHECK(same(buf, rev), "four-byte apply then revert is not byte-identical to the start");
	memcpy(bad_out, next_sha, sizeof(bad_out));
	bad_out[0] = bad_out[0] == '0' ? '1' : '0';
	bad = next;
	bad.output_sha = bad_out;
	memcpy(buf, rev, N);
	CHECK(refused(gs_ada_apply(&bad, buf, N, fresh(), sizeof(err)), "restored") && same(buf, rev),
	      "four-byte apply with a wrong output digest did not restore all four bytes: %s", err);

	/* 11. Fifteen-byte round trip over the original, and a fifteen-byte restore. */
	CHECK(gs_ada_state(&long15, orig, N, fresh(), sizeof(err)) == 0, "original buffer is not state 0 of the fifteen-byte row: %s", err);
	memcpy(buf, orig, N);
	CHECK(gs_ada_apply(&long15, buf, N, fresh(), sizeof(err)) == 0, "fifteen-byte apply failed: %s", err);
	CHECK(!memcmp(buf + SITE15, new15, sizeof(new15)), "fifteen-byte apply did not write all 15 new bytes at the site");
	CHECK(gs_ada_state(&long15, buf, N, fresh(), sizeof(err)) == 1, "fifteen-byte applied buffer is not state 1: %s", err);
	CHECK(gs_ada_revert(&long15, buf, N, fresh(), sizeof(err)) == 0, "fifteen-byte revert failed: %s", err);
	CHECK(same(buf, orig), "fifteen-byte apply then revert is not byte-identical to the start");
	memcpy(bad_out, fif_sha, sizeof(bad_out));
	bad_out[0] = bad_out[0] == '0' ? '1' : '0';
	bad = long15;
	bad.output_sha = bad_out;
	memcpy(buf, orig, N);
	CHECK(refused(gs_ada_apply(&bad, buf, N, fresh(), sizeof(err)), "restored") && same(buf, orig),
	      "fifteen-byte apply with a wrong output digest did not restore all fifteen bytes: %s", err);

	/* 12. Chained rows: rev's digest is mine's output and next's input, so the
	 * mode decides. This digest was once refused as ambiguous. */
	pair[0] = mine;
	pair[1] = next;
	CHECK(finds(pair, 2, rev, GS_ADA_FORWARD, 1, 0), "forward on the chain digest did not select the later row in state 0: %s", err);
	CHECK(finds(pair, 2, rev, GS_ADA_BACKWARD, 0, 1), "backward on the chain digest did not select the earlier row in state 1: %s", err);
	CHECK(finds(pair, 2, rev, GS_ADA_ANY, 1, 0), "any on the chain digest did not prefer the input match: %s", err);
	CHECK(finds(pair, 2, chained, GS_ADA_ANY, 1, 1), "any on an output-only digest did not fall back to the output match: %s", err);
	CHECK(misses(pair, 2, chained, GS_ADA_FORWARD, "already at output"), "forward on an output-only digest not refused: %s", err);
	CHECK(misses(pair, 2, orig, GS_ADA_BACKWARD, "not at output"), "backward on an input-only digest not refused: %s", err);
	pair[1] = mine;
	CHECK(misses(pair, 2, orig, GS_ADA_FORWARD, "more than one"), "two rows with one input digest not refused as ambiguous: %s", err);
	CHECK(misses(pair, 2, orig, 3, "unknown lookup direction"), "mode 3 not refused: %s", err);

	/* 13. Missing arguments. */
	CHECK(gs_ada_state(NULL, orig, N, fresh(), sizeof(err)) < 0 && err[0], "state without a revision not refused with a message");
	CHECK(gs_ada_apply(NULL, buf, N, fresh(), sizeof(err)) < 0 && err[0], "apply without a revision not refused with a message");
	CHECK(gs_ada_revert(NULL, buf, N, fresh(), sizeof(err)) < 0 && err[0], "revert without a revision not refused with a message");
	CHECK(gs_ada_state(&mine, NULL, 5, fresh(), sizeof(err)) < 0 && err[0], "state with NULL data and size 5 not refused with a message");
	CHECK(gs_ada_find(orig, N, GS_ADA_ANY, NULL, &st, fresh(), sizeof(err)) < 0 && err[0] && st == 42,
	      "find without a result pointer not refused cleanly");
	CHECK(gs_ada_find(orig, N, GS_ADA_ANY, &out, NULL, fresh(), sizeof(err)) < 0 && err[0] && out == &sentinel,
	      "find without a state pointer not refused cleanly");
	CHECK(misses(NULL, 1, orig, GS_ADA_ANY, "missing"), "find_in with a NULL table and count 1 not refused: %s", err);

	/* 14. The synthetic digests are not in the shipped table, in any mode. */
	for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		CHECK(refused(gs_ada_find(orig, N, modes[i], &out, &st, fresh(), sizeof(err)), "no supported revision"),
		      "find in mode %d did not refuse an unknown digest: %s", modes[i], err);
		CHECK(out == &sentinel && st == 42, "refused find in mode %d wrote a result", modes[i]);
	}
}

/* A synthetic file the size of the plugin's sites: filler bytes, with the original bytes of
 * the two shipped plugin rows at their file offsets. No vendor bytes beyond those sites. */
enum { P = 0xa6000 };
static unsigned char pstock[P], pmixed[P], pfinal[P], pbuf[P];

static const struct gs_ada_revision *shipped(const char *name)
{
	size_t i;

	for (i = 0; i < gs_ada_revision_count(); i++)
		if (!strcmp(gs_ada_revision_at(i)->name, name))
			return gs_ada_revision_at(i);
	return NULL;
}

/* gs_ada_find_in over the two plugin rows selects rows[want] in state `state`. */
static int pfinds(const struct gs_ada_revision *rows, const unsigned char *data, int mode, size_t want, int state)
{
	const struct gs_ada_revision *out = &sentinel;
	int st = 42;

	return gs_ada_find_in(rows, 2, data, P, mode, &out, &st, fresh(), sizeof(err)) == 0 && out == &rows[want] && st == state;
}

/* gs_ada_find_in over the two plugin rows refuses for the expected reason and writes no result. */
static int pmisses(const struct gs_ada_revision *rows, const unsigned char *data, int mode, const char *why)
{
	const struct gs_ada_revision *out = &sentinel;
	int st = 42;

	return refused(gs_ada_find_in(rows, 2, data, P, mode, &out, &st, fresh(), sizeof(err)), why) && out == &sentinel && st == 42;
}

static void plugin_chain(void)
{
	static const unsigned char div4_new[6] = {0xbf, 0x04, 0x00, 0x00, 0x00, 0x90};
	const struct gs_ada_revision *mixed = shipped("plugin-mixed"), *div4 = shipped("plugin-div4");
	char stock_sha[65], mixed_sha[65], final_sha[65], got[65], skew_sha[65];
	struct gs_ada_revision rows[2], skew;
	size_t i;

	CHECK(mixed && div4, "shipped table lacks plugin-mixed or plugin-div4");
	if (!mixed || !div4)
		return;
	CHECK(!strcmp(mixed->output_sha, div4->input_sha), "plugin-div4 does not take plugin-mixed's output as its input");
	CHECK(div4 == mixed + 1, "plugin-div4 is not the row after plugin-mixed");
	for (i = 0; i < P; i++)
		pstock[i] = (unsigned char)(i * 131u + 7u);
	memcpy(pstock + mixed->site, mixed->old_bytes, mixed->width);
	memcpy(pstock + div4->site, div4->old_bytes, div4->width);
	memcpy(pmixed, pstock, P);
	memcpy(pmixed + mixed->site, mixed->new_bytes, mixed->width);
	memcpy(pfinal, pmixed, P);
	memcpy(pfinal + div4->site, div4->new_bytes, div4->width);
	gs_sha256(pstock, P, SIZE_MAX, stock_sha);
	gs_sha256(pmixed, P, SIZE_MAX, mixed_sha);
	gs_sha256(pfinal, P, SIZE_MAX, final_sha);
	/* The shipped rows with this file's three digests in place of the vendor file's. */
	rows[0] = *mixed;
	rows[0].input_sha = stock_sha;
	rows[0].output_sha = mixed_sha;
	rows[1] = *div4;
	rows[1].input_sha = mixed_sha;
	rows[1].output_sha = final_sha;

	/* Forward: stock -> mixed -> div4, each step reproducing its recorded digest. */
	memcpy(pbuf, pstock, P);
	CHECK(pfinds(rows, pbuf, GS_ADA_FORWARD, 0, 0), "forward on the stock file did not select plugin-mixed at input: %s", err);
	CHECK(gs_ada_apply(&rows[0], pbuf, P, fresh(), sizeof(err)) == 0 && !memcmp(pbuf, pmixed, P),
	      "plugin-mixed apply on the stock file did not give the mixed file: %s", err);
	gs_sha256(pbuf, P, SIZE_MAX, got);
	CHECK(!strcmp(got, mixed_sha), "after plugin-mixed the digest is %s, not the mixed digest", got);
	CHECK(pfinds(rows, pbuf, GS_ADA_FORWARD, 1, 0), "forward on the mixed file did not select plugin-div4 at input: %s", err);
	CHECK(pfinds(rows, pbuf, GS_ADA_ANY, 1, 0), "inspect on the mixed file did not prefer plugin-div4 at input: %s", err);
	CHECK(gs_ada_apply(&rows[1], pbuf, P, fresh(), sizeof(err)) == 0 && !memcmp(pbuf, pfinal, P),
	      "plugin-div4 apply on the mixed file did not give the final file: %s", err);
	gs_sha256(pbuf, P, SIZE_MAX, got);
	CHECK(!strcmp(got, final_sha), "after plugin-div4 the digest is %s, not the final digest", got);
	CHECK(!memcmp(pbuf + 0xa5f28, div4_new, sizeof(div4_new)) && !memcmp(pbuf + 0x8fc17, mixed->new_bytes, 15),
	      "the final file does not carry both revisions at 0x8fc17 and 0xa5f28");
	CHECK(pfinds(rows, pbuf, GS_ADA_ANY, 1, 1), "inspect on the final file did not select plugin-div4 at output: %s", err);
	CHECK(pmisses(rows, pbuf, GS_ADA_FORWARD, "already at output"), "apply on the final file not refused: %s", err);

	/* Backward: div4 -> mixed -> stock, byte-identical to each earlier step. */
	CHECK(pfinds(rows, pbuf, GS_ADA_BACKWARD, 1, 1), "revert on the final file did not select plugin-div4 at output: %s", err);
	CHECK(gs_ada_revert(&rows[1], pbuf, P, fresh(), sizeof(err)) == 0 && !memcmp(pbuf, pmixed, P),
	      "plugin-div4 revert did not give back the mixed file: %s", err);
	CHECK(pfinds(rows, pbuf, GS_ADA_BACKWARD, 0, 1), "revert on the mixed file did not select plugin-mixed at output: %s", err);
	CHECK(gs_ada_revert(&rows[0], pbuf, P, fresh(), sizeof(err)) == 0 && !memcmp(pbuf, pstock, P),
	      "plugin-mixed revert did not give back the stock file: %s", err);
	CHECK(pmisses(rows, pbuf, GS_ADA_BACKWARD, "not at output"), "revert on the stock file not refused: %s", err);

	/* Refusals of plugin-div4: an input that is not the mixed file, and wrong bytes at its site. */
	memcpy(pbuf, pstock, P);
	CHECK(refused(gs_ada_apply(&rows[1], pbuf, P, fresh(), sizeof(err)), "neither") && !memcmp(pbuf, pstock, P),
	      "plugin-div4 apply on the stock file not refused for its digest, or changed it: %s", err);
	memcpy(pbuf, pmixed, P);
	pbuf[0] ^= 0x01;
	CHECK(refused(gs_ada_apply(&rows[1], pbuf, P, fresh(), sizeof(err)), "neither"),
	      "plugin-div4 apply on a changed mixed file not refused for its digest: %s", err);
	CHECK(pmisses(rows, pbuf, GS_ADA_FORWARD, "no supported revision"), "forward on a changed mixed file not refused: %s", err);
	memcpy(pbuf, pfinal, P);
	CHECK(refused(gs_ada_apply(&rows[1], pbuf, P, fresh(), sizeof(err)), "already revised") && !memcmp(pbuf, pfinal, P),
	      "plugin-div4 apply on the final file not refused as already revised: %s", err);
	memcpy(pbuf, pmixed, P);
	CHECK(refused(gs_ada_revert(&rows[1], pbuf, P, fresh(), sizeof(err)), "not revised") && !memcmp(pbuf, pmixed, P),
	      "plugin-div4 revert on the mixed file not refused as not revised: %s", err);
	/* A file whose digest the row names but whose site holds other bytes. */
	pbuf[div4->site + 1] ^= 0x01;
	gs_sha256(pbuf, P, SIZE_MAX, skew_sha);
	skew = rows[1];
	skew.input_sha = skew_sha;
	CHECK(refused(gs_ada_apply(&skew, pbuf, P, fresh(), sizeof(err)), "recorded original bytes"),
	      "plugin-div4 apply with other bytes at 0xa5f28 not refused: %s", err);
	pbuf[div4->site + 1] ^= 0x01;
	CHECK(!memcmp(pbuf, pmixed, P), "refused plugin-div4 apply with other bytes at 0xa5f28 changed the file");
	/* A wrong recorded output digest restores all six bytes. */
	skew = rows[1];
	memcpy(got, final_sha, sizeof(got));
	got[0] = got[0] == '0' ? '1' : '0';
	skew.output_sha = got;
	memcpy(pbuf, pmixed, P);
	CHECK(refused(gs_ada_apply(&skew, pbuf, P, fresh(), sizeof(err)), "restored") && !memcmp(pbuf, pmixed, P),
	      "plugin-div4 apply with a wrong output digest did not restore the six bytes: %s", err);
}

int main(void)
{
	table_invariants();
	synthetic_row();
	plugin_chain();
	printf("test_patch_ada: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
