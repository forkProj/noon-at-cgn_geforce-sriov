/* SPDX-License-Identifier: MIT */
#include "ada.h"
#include "patch.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
/* The shipped rows are admitted only by digests of measured vendor files, which
 * cannot be reproduced synthetically. Block 1 checks the invariants of the
 * shipped table (shape, lowercase-hex digests, global digest uniqueness, every
 * row's output digest pinned to its input digest, and the three measured rows
 * with their sites and byte pairs). Block 2 builds its
 * own row over a 64-byte buffer and drives through it every accepting and
 * refusing path of gs_ada_state, gs_ada_apply and gs_ada_revert, plus the
 * argument and unknown-digest refusals of gs_ada_find.
 * Not covered here: gs_ada_find's accepting path, its "more than one row"
 * refusal and its post-match size/byte refusals, all of which need a digest
 * that is in the shipped table; applying a shipped row to its real file. Those
 * are left to the external real-file check. */
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
enum { N = 64, SITE = 32 };
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

/* Slot 2*i is row i's input digest, slot 2*i+1 its output digest. */
static const char *digest_at(size_t slot)
{
	const struct gs_ada_revision *x = gs_ada_revision_at(slot / 2);

	if (!x)
		return NULL;
	return slot % 2 ? x->output_sha : x->input_sha;
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

static void table_invariants(void)
{
	static const struct {
		const char *input_sha, *output_sha;
		size_t site;
		unsigned char old_bytes[2], new_bytes[2];
	} measured[] = {
		{"4e3abe645b6873ed478adea6a06c6955352ff06186849edeea536c9630596449", "ed3efee68582f4fa9f120cf6c28138d8642db93ff0e6ba07ab0d6d7bf1b9823b", 0xd654ef, {0x75, 0x7f}, {0x90, 0x90}},
		{"ce8a485bfaeebca3a9609f9415a6df81c160a0a53509aec093bc3361a6d12701", "f5b788cdb3e06aed18860cc6cf950dcc13143bd831d90d8e9c4bf9e744426f12", 0xd654ef, {0x75, 0x7f}, {0x90, 0x90}},
		{"7e52b76dc7ca8490e7ffbc1fd724f5db07f3791a3a2ec07f2d595bf05abdcaa7", "097f571588f3a232265103b73e38ba111ddb44ef329ac720562cff71e6170d37", 0x8fb0e, {0xf8, 0x06}, {0x00, 0x5a}},
	};
	size_t count = gs_ada_revision_count(), nmeasured = sizeof(measured) / sizeof(measured[0]), i, j, dupes = 0;

	CHECK(count >= 3, "expected at least three revisions, got %zu", count);
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
		CHECK(memcmp(x->old_bytes, x->new_bytes, 2) != 0, "row %zu old_bytes equal its new_bytes", i);
		CHECK(x->site != 0, "row %zu site is 0", i);
		/* Every row must be listed in measured[], so a new unlisted row or a
		 * wrong input/output pairing fails rather than being skipped. */
		for (j = 0; j < nmeasured; j++)
			if (x->input_sha && !strcmp(x->input_sha, measured[j].input_sha))
				break;
		CHECK(j < nmeasured && x->output_sha && !strcmp(x->output_sha, measured[j].output_sha),
		      "row %zu %s", i, j < nmeasured ? "output_sha is not the recorded output for its input digest" : "input_sha has no recorded output digest");
	}

	/* Same-row pairs are covered above; here every pair from different rows. */
	for (i = 0; i < 2 * count; i++)
		for (j = i + 1; j < 2 * count; j++) {
			const char *a = digest_at(i), *b = digest_at(j);

			if (i / 2 != j / 2 && a && b && !strcmp(a, b))
				dupes++;
		}
	CHECK(dupes == 0, "%zu digest pairs repeat across rows", dupes);

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
		CHECK(!memcmp(pick->old_bytes, measured[i].old_bytes, 2) && !memcmp(pick->new_bytes, measured[i].new_bytes, 2),
		      "measured row %.12s byte pair is not %02x %02x -> %02x %02x", measured[i].input_sha,
		      measured[i].old_bytes[0], measured[i].old_bytes[1], measured[i].new_bytes[0], measured[i].new_bytes[1]);
	}
}

static void synthetic_row(void)
{
	unsigned char orig[N], rev[N], buf[N];
	char in_sha[65], out_sha[65], bad_in[65], bad_out[65];
	const struct gs_ada_revision *out = &sentinel;
	struct gs_ada_revision mine, bad, skew, end, far, edge;
	size_t i;
	int st = 42;

	for (i = 0; i < N; i++)
		orig[i] = (unsigned char)(i * 37u + 0x5au);
	orig[SITE] = 0x11;
	orig[SITE + 1] = 0x22;
	memcpy(rev, orig, N);
	rev[SITE] = 0x33;
	rev[SITE + 1] = 0x44;
	gs_sha256(orig, N, SIZE_MAX, in_sha);
	gs_sha256(rev, N, SIZE_MAX, out_sha);
	mine = (struct gs_ada_revision){
		.name = "synthetic", .artifact = "64-byte test buffer", .version = "none",
		.input_sha = in_sha, .output_sha = out_sha, .site = SITE,
		.old_bytes = {0x11, 0x22}, .new_bytes = {0x33, 0x44},
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
	CHECK(refused(gs_ada_state(&skew, orig, N, fresh(), sizeof(err)), "measured original bytes"), "state accepted an input digest with other site bytes: %s", err);
	memcpy(buf, orig, N);
	CHECK(refused(gs_ada_apply(&skew, buf, N, fresh(), sizeof(err)), "measured original bytes"), "apply accepted an input digest with other site bytes: %s", err);
	CHECK(same(buf, orig), "apply refused for site bytes changed the buffer");
	skew = mine;
	skew.new_bytes[1] = 0x45;
	CHECK(refused(gs_ada_state(&skew, rev, N, fresh(), sizeof(err)), "measured revised bytes"), "state accepted an output digest with other site bytes: %s", err);

	/* 8. Size bound, with digests that match the buffer so only size can refuse. */
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
	far = mine;
	far.site = SIZE_MAX;
	CHECK(refused(gs_ada_state(&far, orig, N, fresh(), sizeof(err)), "smaller"), "state at site SIZE_MAX not refused for size: %s", err);
	edge = mine;
	edge.site = N - 2;
	edge.old_bytes[0] = orig[N - 2];
	edge.old_bytes[1] = orig[N - 1];
	CHECK(gs_ada_state(&edge, orig, N, fresh(), sizeof(err)) == 0, "state at site n-2 with a matching digest not accepted: %s", err);

	/* 9. Missing arguments. */
	CHECK(gs_ada_state(NULL, orig, N, fresh(), sizeof(err)) < 0 && err[0], "state without a revision not refused with a message");
	CHECK(gs_ada_apply(NULL, buf, N, fresh(), sizeof(err)) < 0 && err[0], "apply without a revision not refused with a message");
	CHECK(gs_ada_revert(NULL, buf, N, fresh(), sizeof(err)) < 0 && err[0], "revert without a revision not refused with a message");
	CHECK(gs_ada_state(&mine, NULL, 5, fresh(), sizeof(err)) < 0 && err[0], "state with NULL data and size 5 not refused with a message");
	CHECK(gs_ada_find(orig, N, NULL, &st, fresh(), sizeof(err)) < 0 && err[0] && st == 42,
	      "find without a result pointer not refused cleanly");
	CHECK(gs_ada_find(orig, N, &out, NULL, fresh(), sizeof(err)) < 0 && err[0] && out == &sentinel,
	      "find without a state pointer not refused cleanly");

	/* 10. The synthetic digest is not in the shipped table. */
	CHECK(refused(gs_ada_find(orig, N, &out, &st, fresh(), sizeof(err)), "no measured revision"), "find did not refuse an unknown digest: %s", err);
	CHECK(out == &sentinel && st == 42, "refused find wrote a result");
}

int main(void)
{
	table_invariants();
	synthetic_row();
	printf("test_patch_ada: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
