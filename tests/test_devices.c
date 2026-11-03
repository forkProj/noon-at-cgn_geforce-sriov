// SPDX-License-Identifier: MIT
/*
 * Tests of include/gs_devices.h: the reviewed device table and the pure
 * decisions the kernel module, the CLI and the hook share (lookup, VF limit,
 * capability header check, VF layout check, BDF parsing). Exit status 0 = all
 * passed.
 */
#include <stdio.h>
#include <string.h>

#include "gs_devices.h"

static int failures;
static int checks;

#define CHECK(cond)                                                          \
	do {                                                                 \
		checks++;                                                    \
		if (!(cond)) {                                               \
			failures++;                                          \
			fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__,       \
				__LINE__, #cond);                            \
		}                                                            \
	} while (0)

#define CHECK_DEV(d, cond)                                                   \
	do {                                                                 \
		checks++;                                                    \
		if (!(cond)) {                                               \
			failures++;                                          \
			fprintf(stderr, "%s:%d: FAIL 10de:%04x: %s\n",       \
				__FILE__, __LINE__, (d)->device, #cond);     \
		}                                                            \
	} while (0)

/* The six vGPU targets and what the stock vgpuConfig.xml <pgpu> says. */
struct target {
	unsigned short id;
	unsigned char gen;	/* enum gs_generation */
	const char *die;
	unsigned short subvendor, subdevice;
};

static const struct target targets[] = {
	{ 0x2230, GS_AMPERE, "GA102", 0, 0 },
	{ 0x25b6, GS_AMPERE, "GA107", 0x10de, 0x157e },
	{ 0x26b1, GS_ADA, "AD102", 0, 0 },
	{ 0x27b8, GS_ADA, "AD104", 0, 0 },
	{ 0x2bb5, GS_BLACKWELL, "GB202", 0x10de, 0x204e },
	{ 0x2c3a, GS_BLACKWELL, "GB203", 0x10de, 0x21f4 },
};

static const struct target *find_target(unsigned int id)
{
	unsigned int i;

	for (i = 0; i < sizeof(targets) / sizeof(targets[0]); i++)
		if (targets[i].id == id)
			return &targets[i];
	return NULL;
}

static void test_constants(void)
{
	CHECK(GS_DEFAULT_VFS == 4);
	CHECK(GS_VFS_MAX == 16);
	CHECK(strcmp(GS_VERSION, "0.1.0") == 0);
	CHECK(GS_MAX_PFS == 8);
	CHECK(GS_VENDOR_NVIDIA == 0x10de);
	CHECK(GS_MSIX_HIDE_BIT == 29);
	CHECK(GS_SLOT_FNS == 8);
	CHECK(GS_PCI_DOMAIN_MAX == 0x7fffffffu);
	/* Sizes hold the longest canonical text plus its NUL. */
	CHECK(GS_BDF_STR_SIZE == sizeof("7fffffff:ff:1f.7"));
	CHECK(GS_BUS_STR_SIZE == sizeof("7fffffff:ff"));
	/* GS_MAX_PFS addresses, GS_MAX_PFS - 1 commas, one NUL. */
	CHECK(GS_BDF_LIST_STR_SIZE ==
	      GS_MAX_PFS * (GS_BDF_STR_SIZE - 1) + (GS_MAX_PFS - 1) + 1);
	CHECK(strcmp(GS_NO_SAME_DIE_MSG,
		     "No vGPU target on the same die; support is experimental.") == 0);
	CHECK(strcmp(gs_generation_name(GS_AMPERE), "Ampere") == 0);
	CHECK(strcmp(gs_generation_name(GS_ADA), "Ada") == 0);
	CHECK(strcmp(gs_generation_name(GS_BLACKWELL), "Blackwell") == 0);
	CHECK(strcmp(gs_generation_name(7), "unknown") == 0);
}

static void test_table_entries(void)
{
	unsigned int i, j, supported = 0;

	CHECK(GS_DEVICE_COUNT == 38);
	for (i = 0; i < GS_DEVICE_COUNT; i++) {
		const struct gs_device *d = &gs_devices[i];
		const struct target *t = find_target(d->target);

		for (j = i + 1; j < GS_DEVICE_COUNT; j++)
			CHECK_DEV(d, gs_devices[j].device != d->device);

		CHECK_DEV(d, d->die && d->die[0]);
		CHECK_DEV(d, d->name && strncmp(d->name, "GeForce RTX ", 12) == 0);
		CHECK_DEV(d, d->target_name && d->target_name[0]);
		CHECK_DEV(d, d->same_die <= 1 && d->experimental <= 1);
		/* The configurable range covers every card's hardware default. */
		CHECK_DEV(d, d->totalvfs >= 1 && d->totalvfs <= GS_VFS_MAX);

		/* Every target is one of the six stock <pgpu> entries. */
		CHECK_DEV(d, t != NULL);
		if (!t)
			continue;
		CHECK_DEV(d, t->gen == d->generation);
		CHECK_DEV(d, d->target_subvendor == t->subvendor);
		CHECK_DEV(d, d->target_subdevice == t->subdevice);
		CHECK_DEV(d, d->same_die == (strcmp(d->die, t->die) == 0));
		/* A target ID is never itself a table key (no pro cards). */
		CHECK_DEV(d, gs_device_lookup(d->target) == NULL);

		switch (d->generation) {
		case GS_AMPERE:
			CHECK_DEV(d, d->die[0] == 'G' && d->die[1] == 'A');
			CHECK_DEV(d, d->sriov_pos == 0xbcc && d->ari_pos == 0xc14);
			CHECK_DEV(d, d->totalvfs == 16);
			CHECK_DEV(d, d->msix_pos == 0xc8 && d->msix_hide_pos == 0x41c);
			CHECK_DEV(d, !gs_sriov_native(d));
			break;
		case GS_ADA:
			CHECK_DEV(d, d->die[0] == 'A' && d->die[1] == 'D');
			CHECK_DEV(d, d->sriov_pos == 0xbcc && d->ari_pos == 0xc14);
			CHECK_DEV(d, d->totalvfs == 1);
			/* No Ada hide register is known: the module must never write. */
			CHECK_DEV(d, d->msix_pos == 0xc8 && d->msix_hide_pos == 0);
			CHECK_DEV(d, d->experimental == 1);
			CHECK_DEV(d, !gs_sriov_native(d));
			break;
		case GS_BLACKWELL:
			CHECK_DEV(d, d->die[0] == 'G' && d->die[1] == 'B');
			/* Native SR-IOV only; no guessed ARI/MSI-X offsets. */
			CHECK_DEV(d, d->sriov_pos == 0x250 && d->ari_pos == 0);
			CHECK_DEV(d, d->totalvfs == 1);
			CHECK_DEV(d, d->msix_pos == 0 && d->msix_hide_pos == 0);
			CHECK_DEV(d, d->experimental == 1);
			CHECK_DEV(d, gs_sriov_native(d));
			break;
		default:
			CHECK_DEV(d, !"generation out of range");
		}
		/* Only the demonstrated cards' ID is not experimental. */
		if (!d->experimental) {
			supported++;
			CHECK_DEV(d, d->device == 0x2206);
		}
	}
	CHECK(supported == 1);
}

static void test_lookup(void)
{
	static const unsigned short absent[] = {
		/* Turing GeForce and pro, which are excluded */
		0x1e04, 0x1e07, 0x1e87, 0x1f02, 0x2182, 0x21c4, 0x1e30, 0x1eb8,
		/* professional / datacenter (targets and others) */
		0x2230, 0x2231, 0x2235, 0x25b6, 0x26b1, 0x26ba, 0x27b8, 0x2bb5,
		0x2c3a, 0x24b0, 0x2330,
		/* laptop dies and IDs without reviewed evidence */
		0x2520, 0x2560, 0x25a2, 0x2860, 0x28e0, 0x2757, 0x2c18, 0x2d19,
		0x2f18, 0x2d98, 0x2b87, 0x2808, 0x2414, 0x24c9,
		/* audio functions and nonsense */
		0x228b, 0x0000, 0xffff, 0x10de,
	};
	const struct gs_device *d;
	unsigned int i;

	d = gs_device_lookup(0x2206);
	CHECK(d && d->target == 0x2230 && d->same_die && !d->experimental);
	CHECK(d && d->generation == GS_AMPERE && strcmp(d->die, "GA102") == 0);

	d = gs_device_lookup(0x2489);
	CHECK(d && d->target == 0x25b6 && !d->same_die && d->experimental);
	d = gs_device_lookup(0x2684);
	CHECK(d && d->target == 0x26b1 && d->same_die && d->generation == GS_ADA);
	d = gs_device_lookup(0x2705);
	CHECK(d && d->target == 0x27b8 && !d->same_die);
	d = gs_device_lookup(0x2b85);
	CHECK(d && d->target == 0x2bb5 && d->same_die &&
	      d->generation == GS_BLACKWELL);
	d = gs_device_lookup(0x2f04);
	CHECK(d && d->target == 0x2c3a && !d->same_die);

	for (i = 0; i < sizeof(absent) / sizeof(absent[0]); i++) {
		checks++;
		if (gs_device_lookup(absent[i])) {
			failures++;
			fprintf(stderr, "FAIL: 10de:%04x must not be in the table\n",
				absent[i]);
		}
	}
	/* Values wider than 16 bits never alias a table ID. */
	CHECK(gs_device_lookup(0x12206) == NULL);
}

static void test_vf_limit(void)
{
	const struct gs_device *amp = gs_device_lookup(0x2206);
	const struct gs_device *ada = gs_device_lookup(0x2783);
	const struct gs_device *bw = gs_device_lookup(0x2c02);
	unsigned int i, raw, vfs;
	int exp;

	CHECK(amp && ada && bw);
	if (!amp || !ada || !bw)
		return;

	/* The measured 10de:2206: hardware 16, default count 4. */
	CHECK(amp->totalvfs == 16);
	CHECK(gs_vf_limit(amp, amp->totalvfs, 1, GS_DEFAULT_VFS) == 4);

	/* Ampere, default count: clamp only, never raised. */
	CHECK(gs_vf_limit(amp, 16, 1, GS_DEFAULT_VFS) == 4);
	CHECK(gs_vf_limit(amp, 16, 0, GS_DEFAULT_VFS) == 4);
	CHECK(gs_vf_limit(amp, 4, 1, GS_DEFAULT_VFS) == 4);
	CHECK(gs_vf_limit(amp, 3, 1, GS_DEFAULT_VFS) == 3);
	CHECK(gs_vf_limit(amp, 1, 1, GS_DEFAULT_VFS) == 1);
	CHECK(gs_vf_limit(amp, 0, 1, GS_DEFAULT_VFS) == 0);

	/* A configured count is presented, never above the hardware value. */
	CHECK(gs_vf_limit(amp, 16, 1, 2) == 2);
	CHECK(gs_vf_limit(amp, 16, 1, 1) == 1);
	CHECK(gs_vf_limit(amp, 16, 1, 8) == 8);
	CHECK(gs_vf_limit(amp, 16, 1, 16) == 16);
	CHECK(gs_vf_limit(amp, 8, 1, 16) == 8);
	CHECK(gs_vf_limit(amp, 0, 1, 16) == 0);

	/* Ada/Blackwell: exactly 1 counts as 4, only with experimental_vfs; 0 stays 0. */
	CHECK(gs_vf_limit(ada, 1, 1, GS_DEFAULT_VFS) == 4);
	CHECK(gs_vf_limit(ada, 1, 0, GS_DEFAULT_VFS) == 1);
	CHECK(gs_vf_limit(ada, 0, 1, GS_DEFAULT_VFS) == 0);
	CHECK(gs_vf_limit(ada, 0, 0, GS_DEFAULT_VFS) == 0);
	CHECK(gs_vf_limit(bw, 0, 1, GS_DEFAULT_VFS) == 0);
	CHECK(gs_vf_limit(ada, 2, 1, GS_DEFAULT_VFS) == 2);
	CHECK(gs_vf_limit(ada, 32, 0, GS_DEFAULT_VFS) == 4);
	CHECK(gs_vf_limit(bw, 1, 1, GS_DEFAULT_VFS) == 4);
	CHECK(gs_vf_limit(bw, 1, 0, GS_DEFAULT_VFS) == 1);
	CHECK(gs_vf_limit(bw, 3, 1, GS_DEFAULT_VFS) == 3);
	CHECK(gs_vf_limit(bw, 48, 1, GS_DEFAULT_VFS) == 4);
	/* The experimental raise follows a lower count but never exceeds 4. */
	CHECK(gs_vf_limit(ada, 1, 1, 2) == 2);
	CHECK(gs_vf_limit(ada, 1, 1, 16) == 4);
	CHECK(gs_vf_limit(bw, 1, 0, 16) == 1);

	/*
	 * No entry, raw value, flag or count ever gives more than the count, nor more
	 * than the raw value except a raw 1 raised to at most 4.
	 */
	for (i = 0; i < GS_DEVICE_COUNT; i++)
		for (exp = 0; exp <= 1; exp++)
			for (vfs = 1; vfs <= GS_VFS_MAX; vfs++)
				for (raw = 0; raw <= 0xffff; raw++) {
					unsigned int v = gs_vf_limit(&gs_devices[i], raw, exp, vfs);

					if (v > vfs || (raw != 1 && v > raw) || (raw == 1 && v > GS_DEFAULT_VFS)) {
						checks++;
						failures++;
						fprintf(stderr, "FAIL: 10de:%04x raw %u exp %d vfs %u -> %u\n",
							gs_devices[i].device, raw, exp, vfs, v);
						return;
					}
				}
	checks++;
}

/*
 * The module's load-time check with each configurable count on the GA102
 * layout (hardware TotalVFs 16, first VF offset 4, stride 1, PF and audio at
 * functions 0-1): counts 1-4 pass; every count above 4 is refused at load,
 * and again when the kernel would set up that many VFs.
 */
static void test_vf_count_guard(void)
{
	const struct gs_device *amp = gs_device_lookup(0x2206);
	unsigned int vfs, limit;

	CHECK(amp != NULL);
	if (!amp)
		return;
	for (vfs = 1; vfs <= GS_VFS_MAX; vfs++) {
		limit = gs_vf_limit(amp, amp->totalvfs, 1, vfs);
		CHECK(limit == vfs);
		if (vfs <= GS_DEFAULT_VFS) {
			CHECK(gs_vf_geometry_check(limit, 4, 1, 0x03) == GS_GEOM_OK);
			CHECK(gs_vf_setup_check(limit, limit, 4, 1, 0x03) == GS_GEOM_OK);
		} else {
			CHECK(gs_vf_geometry_check(limit, 4, 1, 0x03) == GS_GEOM_BEYOND_SLOT);
			CHECK(gs_vf_setup_check(limit, limit, 4, 1, 0x03) == GS_GEOM_BEYOND_SLOT);
		}
	}
}

static void test_ext_cap_hdr(void)
{
	CHECK(gs_ext_cap_hdr_is(0xc1410010u, GS_EXT_CAP_ID_SRIOV, 1)); /* GA102/AD10x 0xBCC */
	CHECK(gs_ext_cap_hdr_is(0x2a410010u, GS_EXT_CAP_ID_SRIOV, 1)); /* GB20x 0x250 */
	CHECK(gs_ext_cap_hdr_is(0x29010010u, GS_EXT_CAP_ID_SRIOV, 1));
	CHECK(gs_ext_cap_hdr_is(0xc1c1000eu, GS_EXT_CAP_ID_ARI, 1));   /* hidden ARI 0xC14 */
	CHECK(!gs_ext_cap_hdr_is(0xc1c10015u, GS_EXT_CAP_ID_SRIOV, 1)); /* ReBAR */
	CHECK(!gs_ext_cap_hdr_is(0xc1420010u, GS_EXT_CAP_ID_SRIOV, 1)); /* version 2 */
	CHECK(!gs_ext_cap_hdr_is(0x00000000u, GS_EXT_CAP_ID_SRIOV, 1));
	CHECK(!gs_ext_cap_hdr_is(0xffffffffu, GS_EXT_CAP_ID_SRIOV, 1)); /* absent device */
	CHECK(!gs_ext_cap_hdr_is(0xc1c1000eu, GS_EXT_CAP_ID_SRIOV, 1));
}

static void test_bdf(void)
{
	static const char *const bad[] = {
		"0000:0a:00.1",	/* not function 0 */
		"0000:0A:00.0",	/* uppercase */
		"0000:03:20.0",	/* slot > 0x1f */
		"000:03:00.0",	/* short domain */
		"03:00.0",	/* no domain */
		"0000:03:00.0\n", "0000:03:00.0 ", " 0000:03:00.0",
		"0000:03:00", "0000-03:00.0", "0000:03:00.00", "",
		"010000:03:00.0",	/* extra leading zero */
		"00010000:03:00.0",	/* extra leading zeros */
		"80000000:03:00.0",	/* above INT_MAX: never a Linux domain */
		"ffffffff:03:00.0",
		"100000000:03:00.0",	/* 9 digits */
		"1000A:03:00.0",	/* uppercase in a wide domain */
		"10000:03:00.8",	/* function > 7 */
	};
	static const char *const canonical[] = {
		"0000:00:00.0", "0000:03:00.1", "000a:ff:1f.7", "ffff:01:00.0",
		"10000:00:00.0", "10001:e1:00.3", "7fffffff:ff:1f.7",
		"abcdef0:12:0a.0",
	};
	char text[GS_BDF_STR_SIZE], big[GS_BDF_LIST_STR_SIZE];
	struct gs_bdf b, list[GS_MAX_PFS + 1];
	unsigned int i;

	CHECK(gs_parse_pf_bdf("0000:03:00.0", &b));
	CHECK(b.domain == 0 && b.bus == 3 && b.devfn == 0);
	CHECK(gs_parse_pf_bdf("10de:ff:1f.0", &b));
	CHECK(b.domain == 0x10de && b.bus == 0xff && b.devfn == 0xf8);
	CHECK(!gs_parse_pf_bdf(NULL, &b));
	/* Intel VMD domains start at 10000; Linux prints them with 5+ digits. */
	CHECK(gs_parse_pf_bdf("10000:e1:00.0", &b));
	CHECK(b.domain == 0x10000 && b.bus == 0xe1 && b.devfn == 0);
	CHECK(gs_parse_pf_bdf("7fffffff:00:01.0", &b));
	CHECK(b.domain == 0x7fffffffu && b.devfn == 8);
	/* Any function for gs_parse_bdf(), function 0 only for the PF parser. */
	CHECK(gs_parse_bdf("0000:03:00.7", &b) && b.devfn == 7);
	CHECK(gs_parse_bdf("10000:03:1f.1", &b) && b.devfn == 0xf9);
	CHECK(!gs_parse_bdf(NULL, &b));
	CHECK(!gs_parse_bdf("0000:03:00.8", &b));
	CHECK(!gs_parse_pf_bdf("10000:03:00.1", &b));
	/* A failed parse leaves the cursor where it was. */
	{
		const char *s = "0000:03:00.1", *p = s;

		CHECK(!gs_parse_pf_bdf_at(&p, &b) && p == s);
	}
	/* Parsing and printing as Linux does gives back the same text. */
	for (i = 0; i < sizeof(canonical) / sizeof(canonical[0]); i++) {
		int len;

		CHECK(gs_parse_bdf(canonical[i], &b));
		len = snprintf(text, sizeof(text), "%04x:%02x:%02x.%x",
			       b.domain, b.bus, b.devfn >> 3, b.devfn & 7);
		CHECK(len > 0 && (unsigned int)len < sizeof(text));
		CHECK(strcmp(text, canonical[i]) == 0);
	}
	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		checks++;
		if (gs_parse_pf_bdf(bad[i], &b)) {
			failures++;
			fprintf(stderr, "FAIL: accepted BDF \"%s\"\n", bad[i]);
		}
	}

	CHECK(gs_parse_pf_bdf_list("", list, GS_MAX_PFS) == 0);
	CHECK(gs_parse_pf_bdf_list(NULL, list, GS_MAX_PFS) == -1);
	CHECK(gs_parse_pf_bdf_list("0000:03:00.0", list, GS_MAX_PFS) == 1);
	CHECK(gs_parse_pf_bdf_list("0000:03:00.0,0000:04:00.0", list,
				   GS_MAX_PFS) == 2);
	CHECK(list[0].bus == 3 && list[1].bus == 4);
	CHECK(gs_parse_pf_bdf_list("0000:03:00.0,0000:03:00.0", list,
				   GS_MAX_PFS) == -1);
	CHECK(gs_parse_pf_bdf_list("0000:03:00.0,", list, GS_MAX_PFS) == -1);
	CHECK(gs_parse_pf_bdf_list(",0000:03:00.0", list, GS_MAX_PFS) == -1);
	CHECK(gs_parse_pf_bdf_list("0000:03:00.0,,0000:04:00.0", list,
				   GS_MAX_PFS) == -1);
	CHECK(gs_parse_pf_bdf_list("0000:03:00.0, 0000:04:00.0", list,
				   GS_MAX_PFS) == -1);
	CHECK(gs_parse_pf_bdf_list("0000:03:00.0,0000:03:00.4", list,
				   GS_MAX_PFS) == -1);
	CHECK(gs_parse_pf_bdf_list(
		      "0000:01:00.0,0000:02:00.0,0000:03:00.0,0000:04:00.0,"
		      "0000:05:00.0,0000:06:00.0,0000:07:00.0,0000:08:00.0",
		      list, GS_MAX_PFS) == 8);
	CHECK(gs_parse_pf_bdf_list(
		      "0000:01:00.0,0000:02:00.0,0000:03:00.0,0000:04:00.0,"
		      "0000:05:00.0,0000:06:00.0,0000:07:00.0,0000:08:00.0,"
		      "0000:09:00.0",
		      list, GS_MAX_PFS) == -1);
	/* GS_MAX_PFS longest addresses fit in GS_BDF_LIST_STR_SIZE. */
	big[0] = '\0';
	for (i = 0; i < GS_MAX_PFS; i++) {
		snprintf(text, sizeof(text), "7fffff%02x:ff:1f.0", 0xf0 + i);
		if (i)
			strcat(big, ",");
		strcat(big, text);
	}
	CHECK(strlen(big) + 1 == GS_BDF_LIST_STR_SIZE);
	CHECK(gs_parse_pf_bdf_list(big, list, GS_MAX_PFS) == (int)GS_MAX_PFS);
	CHECK(list[7].domain == 0x7ffffff7u && list[7].devfn == 0xf8);
}

/*
 * Reference layout check in 64-bit arithmetic, written independently of
 * gs_vf_geometry_check(): place every VF and look at where it lands.
 */
static enum gs_vf_geometry ref_geometry(unsigned long long numvfs,
					unsigned long long offset,
					unsigned long long stride,
					unsigned int occupied)
{
	unsigned long long i, fn;

	if (numvfs == 0)
		return GS_GEOM_OK;
	if (offset == 0)
		return GS_GEOM_OFFSET_ZERO;
	if (numvfs > 1 && stride == 0)
		return GS_GEOM_STRIDE_ZERO;
	if (offset + stride * (numvfs - 1) > 7)
		return GS_GEOM_BEYOND_SLOT;
	for (i = 0; i < numvfs; i++) {
		fn = offset + stride * i;
		if (occupied & (1u << fn))
			return GS_GEOM_COLLISION;
	}
	return GS_GEOM_OK;
}

static void test_vf_geometry(void)
{
	static const unsigned int masks[] = { 0x01, 0x03, 0x11, 0x81, 0xff, 0x00 };
	unsigned int n, off, str, m, mism = 0;

	/* NumVFs 0 places nothing, whatever offset and stride read. */
	CHECK(gs_vf_geometry_check(0, 0, 0, 0xff) == GS_GEOM_OK);
	CHECK(gs_vf_geometry_check(0, 0xffff, 0xffff, 0xff) == GS_GEOM_OK);
	/* One VF: offset 0 is the PF; stride does not matter. */
	CHECK(gs_vf_geometry_check(1, 0, 1, 0x01) == GS_GEOM_OFFSET_ZERO);
	CHECK(gs_vf_geometry_check(1, 4, 0, 0x03) == GS_GEOM_OK);
	CHECK(gs_vf_geometry_check(1, 7, 0, 0x03) == GS_GEOM_OK);
	CHECK(gs_vf_geometry_check(1, 8, 0, 0x03) == GS_GEOM_BEYOND_SLOT);
	/* Several VFs with stride 0 share one function. */
	CHECK(gs_vf_geometry_check(2, 4, 0, 0x03) == GS_GEOM_STRIDE_ZERO);
	CHECK(gs_vf_geometry_check(4, 0, 0, 0x03) == GS_GEOM_OFFSET_ZERO);
	/* Offset 4 / stride 1 as read on the GA102 boards at NumVFs 0; PF + audio at 0-1. */
	for (n = 1; n <= GS_DEFAULT_VFS; n++)
		CHECK(gs_vf_geometry_check(n, 4, 1, 0x03) == GS_GEOM_OK);
	/* Any configured count above 4 is refused there, up to the hardware's 16. */
	for (n = GS_DEFAULT_VFS + 1; n <= GS_VFS_MAX; n++)
		CHECK(gs_vf_geometry_check(n, 4, 1, 0x03) == GS_GEOM_BEYOND_SLOT);
	/* Collision with an existing function, first VF or a later one. */
	CHECK(gs_vf_geometry_check(4, 1, 1, 0x03) == GS_GEOM_COLLISION);
	CHECK(gs_vf_geometry_check(4, 4, 1, 0x83) == GS_GEOM_COLLISION);
	CHECK(gs_vf_geometry_check(3, 4, 1, 0x83) == GS_GEOM_OK);
	CHECK(gs_vf_geometry_check(7, 1, 1, 0x01) == GS_GEOM_OK);
	CHECK(gs_vf_geometry_check(8, 1, 1, 0x01) == GS_GEOM_BEYOND_SLOT);
	/* Stride 2: functions 1, 3, 5, 7. */
	CHECK(gs_vf_geometry_check(4, 1, 2, 0x01) == GS_GEOM_OK);
	CHECK(gs_vf_geometry_check(4, 1, 2, 0x05) == GS_GEOM_OK);
	CHECK(gs_vf_geometry_check(4, 1, 2, 0x09) == GS_GEOM_COLLISION);
	CHECK(gs_vf_geometry_check(5, 1, 2, 0x01) == GS_GEOM_BEYOND_SLOT);
	/* Overflow: products far beyond 32 bits never wrap to "inside". */
	CHECK(gs_vf_geometry_check(0xffff, 0xffff, 0xffff, 0) == GS_GEOM_BEYOND_SLOT);
	CHECK(gs_vf_geometry_check(2, 1, 0xffff, 0) == GS_GEOM_BEYOND_SLOT);
	CHECK(gs_vf_geometry_check(0xffffffffu, 1, 1, 0) == GS_GEOM_BEYOND_SLOT);
	CHECK(gs_vf_geometry_check(0x80000001u, 1, 0x80000000u, 0) ==
	      GS_GEOM_BEYOND_SLOT);
	CHECK(gs_vf_geometry_check(1, 0xffffffffu, 0, 0) == GS_GEOM_BEYOND_SLOT);
	CHECK(gs_vf_geometry_text(GS_GEOM_OK)[0] != '\0');
	CHECK(strcmp(gs_vf_geometry_text((enum gs_vf_geometry)99), "unknown") == 0);

	/* Exhaustive against the 64-bit reference on a small cube. */
	for (n = 0; n <= 9; n++)
		for (off = 0; off <= 10; off++)
			for (str = 0; str <= 10; str++)
				for (m = 0; m < sizeof(masks) / sizeof(masks[0]); m++)
					if (gs_vf_geometry_check(n, off, str, masks[m]) !=
					    ref_geometry(n, off, str, masks[m]))
						mism++;
	CHECK(mism == 0);
}

/*
 * Pure-logic model of what the kernel does with the guard, NOT a hardware
 * claim. A fixture gives the First VF Offset / VF Stride a device reports
 * for each NumVFs, separately with the ARI Capable Hierarchy bit clear and
 * set (the only inputs the SR-IOV spec lets them depend on), and optionally
 * what NumVFs reads back after each write (NULL: the value written).
 * sriov_init_keeps() mirrors compute_max_vf_buses() in Linux
 * drivers/pci/iov.c (writes NumVFs from TotalVFs down to 1, -EIO on offset 0
 * or on stride 0 with more than one VF) reading through the guard, which
 * checks the requested count with gs_vf_setup_check() and turns a refusal
 * into offset and stride 0.
 */
struct geometry_fixture {
	const char *name;
	unsigned short offset[2][GS_DEFAULT_VFS + 1];	/* [ari][numvfs] */
	unsigned short stride[2][GS_DEFAULT_VFS + 1];
	const unsigned short *numvfs_reads;		/* [numvfs written] */
};

static int sriov_init_keeps(const struct geometry_fixture *f, int ari,
			    unsigned int total, unsigned int occupied)
{
	unsigned int nr, off, str, rb;

	for (nr = total; nr; nr--) {
		rb = f->numvfs_reads ? f->numvfs_reads[nr] : nr;
		off = f->offset[ari][nr];
		str = f->stride[ari][nr];
		if (gs_vf_setup_check(nr, rb, off, str, occupied) != GS_GEOM_OK)
			off = str = 0;
		if (!off || (nr > 1 && !str))
			return 0;
	}
	return 1;
}

static void test_vf_setup_check(void)
{
	/* Requested 0 (the final reset) is never refused. */
	CHECK(gs_vf_setup_check(0, 0, 0, 0, 0xff) == GS_GEOM_OK);
	CHECK(gs_vf_setup_check(0, 4, 0, 0, 0xff) == GS_GEOM_OK);
	/* A clamped or ignored write is refused, even with a good layout. */
	CHECK(gs_vf_setup_check(4, 1, 4, 1, 0x03) == GS_GEOM_NUMVFS_MISMATCH);
	CHECK(gs_vf_setup_check(4, 0, 4, 1, 0x03) == GS_GEOM_NUMVFS_MISMATCH);
	CHECK(gs_vf_setup_check(1, 0, 4, 1, 0x03) == GS_GEOM_NUMVFS_MISMATCH);
	CHECK(gs_vf_setup_check(2, 0xffff, 4, 1, 0x03) == GS_GEOM_NUMVFS_MISMATCH);
	/* A matching read-back checks the layout of the requested count. */
	CHECK(gs_vf_setup_check(4, 4, 4, 1, 0x03) == GS_GEOM_OK);
	CHECK(gs_vf_setup_check(4, 4, 0, 1, 0x03) == GS_GEOM_OFFSET_ZERO);
	CHECK(gs_vf_setup_check(4, 4, 4, 0, 0x03) == GS_GEOM_STRIDE_ZERO);
	CHECK(gs_vf_setup_check(4, 4, 5, 1, 0x03) == GS_GEOM_BEYOND_SLOT);
	CHECK(gs_vf_setup_check(4, 4, 4, 1, 0x83) == GS_GEOM_COLLISION);
	CHECK(strcmp(gs_vf_geometry_text(GS_GEOM_NUMVFS_MISMATCH), "unknown") != 0);
}

static void test_vf_geometry_states(void)
{
	/*
	 * All fixtures below are made up. "constant" repeats the 4/1 the GA102
	 * boards read at NumVFs 0 for every NumVFs and both ARI states; no card
	 * has been measured at NumVFs 1-4 or with the ARI bit set.
	 */
	static const struct geometry_fixture constant = {
		"constant 4/1",
		{ { 4, 4, 4, 4, 4 }, { 4, 4, 4, 4, 4 } },
		{ { 1, 1, 1, 1, 1 }, { 1, 1, 1, 1, 1 } },
		NULL,
	};
	/* Fine at NumVFs 0 and without ARI, beyond the slot once ARI is set. */
	static const struct geometry_fixture ari_moves = {
		"ARI moves VFs to 8+",
		{ { 4, 4, 4, 4, 4 }, { 4, 8, 8, 8, 8 } },
		{ { 1, 1, 1, 1, 1 }, { 1, 1, 1, 1, 1 } },
		NULL,
	};
	/* Fine at NumVFs 0 and 1, stride grows with NumVFs. */
	static const struct geometry_fixture numvfs_moves = {
		"stride grows with NumVFs",
		{ { 4, 4, 4, 4, 4 }, { 4, 4, 4, 4, 4 } },
		{ { 1, 1, 1, 1, 2 }, { 1, 1, 1, 1, 2 } },
		NULL,
	};
	/* Offset reads 0 only at NumVFs 4. */
	static const struct geometry_fixture zero_at_4 = {
		"offset 0 at NumVFs 4",
		{ { 4, 4, 4, 4, 0 }, { 4, 4, 4, 4, 0 } },
		{ { 1, 1, 1, 1, 1 }, { 1, 1, 1, 1, 1 } },
		NULL,
	};
	/* Starts on the audio function at NumVFs 1 only. */
	static const struct geometry_fixture audio_at_1 = {
		"VF0 on function 1 at NumVFs 1",
		{ { 4, 1, 4, 4, 4 }, { 4, 1, 4, 4, 4 } },
		{ { 1, 1, 1, 1, 1 }, { 1, 1, 1, 1, 1 } },
		NULL,
	};
	/* Clamps NumVFs to 1, as a card with hardware TotalVFs 1 might (unknown). */
	static const unsigned short reads_clamp_1[GS_DEFAULT_VFS + 1] = { 0, 1, 1, 1, 1 };
	static const struct geometry_fixture clamp_1 = {
		"NumVFs clamped to 1",
		{ { 4, 4, 4, 4, 4 }, { 4, 4, 4, 4, 4 } },
		{ { 1, 1, 1, 1, 1 }, { 1, 1, 1, 1, 1 } },
		reads_clamp_1,
	};
	/* Ignores every NumVFs write. */
	static const unsigned short reads_ignored[GS_DEFAULT_VFS + 1] = { 0, 0, 0, 0, 0 };
	static const struct geometry_fixture ignored = {
		"NumVFs writes ignored",
		{ { 4, 4, 4, 4, 4 }, { 4, 4, 4, 4, 4 } },
		{ { 1, 1, 1, 1, 1 }, { 1, 1, 1, 1, 1 } },
		reads_ignored,
	};

	CHECK(sriov_init_keeps(&constant, 0, 4, 0x03));
	CHECK(sriov_init_keeps(&constant, 1, 4, 0x03));
	CHECK(!sriov_init_keeps(&constant, 1, 4, 0x13));	/* fn 4 in use */
	CHECK(sriov_init_keeps(&ari_moves, 0, 4, 0x03));
	CHECK(!sriov_init_keeps(&ari_moves, 1, 4, 0x03));
	CHECK(!sriov_init_keeps(&ari_moves, 1, 1, 0x03));
	CHECK(sriov_init_keeps(&numvfs_moves, 1, 3, 0x03));
	CHECK(!sriov_init_keeps(&numvfs_moves, 1, 4, 0x03));
	CHECK(!sriov_init_keeps(&zero_at_4, 0, 4, 0x03));
	CHECK(sriov_init_keeps(&zero_at_4, 0, 3, 0x03));
	CHECK(!sriov_init_keeps(&audio_at_1, 0, 4, 0x03));
	CHECK(sriov_init_keeps(&audio_at_1, 0, 4, 0x01));
	/* Read-back differs: refused whenever more VFs are asked than kept. */
	CHECK(!sriov_init_keeps(&clamp_1, 0, 4, 0x03));
	CHECK(!sriov_init_keeps(&clamp_1, 1, 2, 0x03));
	CHECK(sriov_init_keeps(&clamp_1, 1, 1, 0x03));
	CHECK(!sriov_init_keeps(&ignored, 0, 4, 0x03));
	CHECK(!sriov_init_keeps(&ignored, 1, 1, 0x03));
	/* The NumVFs 0 values (index 0) never decide anything. */
	CHECK(sriov_init_keeps(&ari_moves, 0, 4, 0x03) ==
	      sriov_init_keeps(&constant, 0, 4, 0x03));
}

int main(void)
{
	test_constants();
	test_table_entries();
	test_lookup();
	test_vf_limit();
	test_vf_count_guard();
	test_ext_cap_hdr();
	test_bdf();
	test_vf_geometry();
	test_vf_setup_check();
	test_vf_geometry_states();

	if (failures) {
		fprintf(stderr, "test_devices: %d of %d checks failed\n",
			failures, checks);
		return 1;
	}
	printf("test_devices: %d checks passed\n", checks);
	return 0;
}
