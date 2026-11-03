/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * geforce-sriov device table: the only place that names a GeForce card.
 *
 * Shared by the kernel livepatch (kernel/geforce_sriov.c), the CLI and the
 * identity hook. It uses only plain C types and needs no header, so it builds
 * unchanged in the kernel and in user space.
 *
 * Every entry is one exact, reviewed PCI device ID. There are no ranges: an ID
 * that is not listed (a professional card, a laptop die, Turing, an unreleased
 * part) gets nothing from any component.
 *
 * What an entry claims, and what it does not
 * ------------------------------------------
 * - Hidden/native capability positions come from public raw config-space
 *   dumps of cards with that ID. They show the *shape* of the capability on
 *   cards of that ID; they do not show that SR-IOV works there.
 *   The kernel module re-reads and validates every header on the running card
 *   before it uses a position, and does nothing when the header does not match.
 * - The method has been demonstrated end to end only on two memory-modded
 *   RTX 3080 20GB cards, both 10de:2206 with PF subsystem 10de:146d: 4 VFs
 *   with vGPU guests running. That demonstrates the method on those cards,
 *   not the device ID: any other 10de:2206 board has never been run, and no
 *   entry, 2206 included, certifies a user's card. Every other entry has
 *   never run. This package was hardware-tested on those two cards only.
 * - Ada and Blackwell GeForce report TotalVFs = 1. Advertising more VFs there
 *   (gs_vf_limit with experimental_vfs) is untested; the GSP firmware and
 *   driver gates for VFs on those generations are unknown.
 * - target is the vGPU device ID presented to nvidia-vgpud. same_die = 1 means
 *   a professional SKU of the same die has a <pgpu> entry in the stock
 *   vgpuConfig.xml (checked in the 595.91.04 file; 580.159.01 has the same IDs
 *   except where noted). same_die = 0 means no such SKU exists and the
 *   nearest same-generation target was chosen explicitly (see each group);
 *   tools must then print GS_NO_SAME_DIE_MSG. Exposing a capability is not the
 *   same as a working vGPU.
 *
 * Fields
 * ------
 * device            GeForce PCI device ID (vendor is always GS_VENDOR_NVIDIA)
 * target            vGPU target device ID (a <pgpu> devId)
 * target_subvendor  \ subsystem of that <pgpu>; 0/0 means the stock <pgpu> has
 * target_subdevice  / subsystemId 0x0 (matches any), so keep the real one
 * sriov_pos         SR-IOV extended capability offset. Ampere/Ada: hidden
 *                   (unlinked) header; Blackwell: natively linked header
 * ari_pos           hidden ARI extended capability offset, 0 = none known
 * msix_pos          MSI-X capability offset in the standard chain, 0 = none known
 * msix_hide_pos     config dword whose bit GS_MSIX_HIDE_BIT unlinks MSI-X,
 *                   0 = mechanism unknown (the module then never writes)
 * totalvfs          hardware TotalVFs in the public dumps of this ID (the
 *                   card's own default; the module reads the real register)
 * generation        enum gs_generation
 * same_die          1 = target is a SKU of the same die
 * experimental      0 only for 10de:2206, the ID of the two cards above
 *                   (method demonstrated on subsystem 10de:146d only; NOT a
 *                   validation of a user's 2206 card). 1 = no run at all.
 *                   Tools describe 0 as "supported" (device path implemented,
 *                   method demonstrated on those cards only) and 1 as
 *                   "experimental".
 * die, name, target_name   display strings
 */
#ifndef GS_DEVICES_H
#define GS_DEVICES_H

#define GS_VERSION		"0.1.0"
#define GS_VENDOR_NVIDIA	0x10deu

/*
 * VFs per card when no count is configured (module parameter vfs, config key
 * vfs=). ARI is enabled in software, but ARI decoding in the GPU does not work
 * for now, so larger counts are expected not to work; on the measured cards
 * the VF layout check (gs_vf_geometry_check()) refuses them.
 */
#define GS_DEFAULT_VFS		4u

/* Largest VF count that can be configured: the largest totalvfs in the table. */
#define GS_VFS_MAX		16u

/* Most physical functions one module load / one config may list. */
#define GS_MAX_PFS		8u

/*
 * PCI addresses are written as Linux names them ("%04x:%02x:%02x.%d", see
 * pci_name()): the domain takes 4 to 8 lowercase hex digits (more than 4
 * only when the value needs them, e.g. 10000 behind Intel VMD). Linux keeps
 * the domain in an int (pci_domain_nr()), so no domain exceeds 0x7fffffff.
 * Sizes include the terminating NUL:
 *   GS_BDF_STR_SIZE       "dddddddd:bb:ss.f"
 *   GS_BUS_STR_SIZE       "dddddddd:bb" (as in /sys/class/pci_bus)
 *   GS_BDF_LIST_STR_SIZE  GS_MAX_PFS BDFs separated by commas
 */
#define GS_PCI_DOMAIN_MAX	0x7fffffffu
#define GS_BDF_STR_SIZE		17u
#define GS_BUS_STR_SIZE		12u
#define GS_BDF_LIST_STR_SIZE	(GS_MAX_PFS * GS_BDF_STR_SIZE)

/*
 * PCI functions per slot without ARI. Only the two demonstrated cards were
 * seen to decode just these; every listed card is held to them.
 */
#define GS_SLOT_FNS		8u

/*
 * 0x41C bit 29 is NV_XVE_PRIV_MISC_1_CYA_HIDE_MSIX_CAP in NVIDIA's GA102 XVE
 * register manual. Bit set = MSI-X unlinked from the standard chain. On the
 * two demonstrated cards above (10de:2206, subsystem 10de:146d), clearing it
 * relinks MSI-X at 0xC8, a bus remove/rescan keeps the cleared value, and FLR
 * or secondary-bus reset sets it again.
 */
#define GS_MSIX_HIDE_BIT	29u

/* Exact text every tool shows for a same_die = 0 entry. */
#define GS_NO_SAME_DIE_MSG \
	"No vGPU target on the same die; support is experimental."

/* PCIe extended capability IDs used by the table (PCIe base spec). */
#define GS_EXT_CAP_ID_ARI	0x000eu
#define GS_EXT_CAP_ID_SRIOV	0x0010u
/* PCI capability ID of MSI-X. */
#define GS_CAP_ID_MSIX		0x11u

enum gs_generation { GS_AMPERE, GS_ADA, GS_BLACKWELL };

struct gs_device {
	unsigned short device;
	unsigned short target;
	unsigned short target_subvendor;
	unsigned short target_subdevice;
	unsigned short sriov_pos;
	unsigned short ari_pos;
	unsigned short msix_pos;
	unsigned short msix_hide_pos;
	unsigned short totalvfs;
	unsigned char generation;
	unsigned char same_die;
	unsigned char experimental;
	const char *die;
	const char *name;
	const char *target_name;
};

/*
 * Ampere (GA10x): 0xBB0 ReBAR header c1c10015 links to 0xC1C and skips a
 * hidden SR-IOV header c1410010 at 0xBCC (TotalVFs 16, first VF offset 4,
 * stride 1) and a hidden ARI header c1c1000e at 0xC14, in every Ampere GeForce
 * dump checked. MSI-X sits at 0xC8; on every Ampere dump with MSI-X hidden,
 * 0x41C bit 29 is set.
 * The card decodes only 3 bits of PCI function number: on the GA102 boards
 * only functions 4-7 reach distinct VFs, and functions >= 8 alias the PF and
 * its audio function, so gs_vf_geometry_check() refuses VFs there.
 */
#define GS_AMPERE_ENTRY(dev, dname, pname, tgt, tsv, tsd, tname, same, exp) \
	{ (dev), (tgt), (tsv), (tsd), 0xbccu, 0xc14u, 0xc8u, 0x41cu, 16u,      \
	  GS_AMPERE, (same), (exp), (dname), (pname), (tname) }

/*
 * Ada (AD10x): the same hidden SR-IOV (0xBCC) and ARI (0xC14) headers as
 * Ampere in every Ada GeForce dump checked, but TotalVFs = InitialVFs = 1.
 * MSI-X is expected at 0xC8, but no Ada dump records 0x41C, so its hide
 * mechanism is unknown: msix_hide_pos = 0 and the module never writes it.
 */
#define GS_ADA_ENTRY(dev, dname, pname, tgt, tname, same) \
	{ (dev), (tgt), 0u, 0u, 0xbccu, 0xc14u, 0xc8u, 0u, 1u, \
	  GS_ADA, (same), 1u, (dname), (pname), (tname) }

/*
 * Blackwell (GB20x) desktop GeForce: SR-IOV is linked natively in the chain at
 * 0x250 (header 2a410010 or 29010010), TotalVFs 1. ARI and MSI-X are left to
 * the kernel's own discovery; no hidden offsets are assumed. Blackwell laptop
 * dies have no SR-IOV capability and are not listed.
 */
#define GS_BLACKWELL_ENTRY(dev, dname, pname, tgt, tsd, tname, same) \
	{ (dev), (tgt), GS_VENDOR_NVIDIA, (tsd), 0x250u, 0u, 0u, 0u, 1u, \
	  GS_BLACKWELL, (same), 1u, (dname), (pname), (tname) }

/*
 * Cross-die choices (same_die = 0). No professional SKU of GA103/GA104/GA106,
 * AD103/AD106/AD107 or GB205/GB206 has a <pgpu> entry. The target was then
 * picked by a HEURISTIC, not by a measured or numerically verified
 * comparison: of the same-generation targets, take the one whose die is the
 * smaller, mid-range part rather than the flagship, so that its largest vGPU
 * profile is the one nearest a mid-range GeForce framebuffer:
 *   GA104, GA106 -> 0x25B6 A2 (GA107; largest profile 16 GiB)
 *     rather than 0x2230 A6000 (GA102; 48 GiB);
 *   AD103, AD106, AD107 -> 0x27B8 L4 (AD104; 24 GiB)
 *     rather than 0x26B1 RTX 6000 Ada (AD102; 48 GiB);
 *   GB205, GB206 -> 0x2C3A RTX PRO 4500 (GB203; 32 GiB)
 *     rather than 0x2BB5 RTX PRO 6000 (GB202; 96 GiB).
 * The largest-profile sizes are from the stock vgpuConfig.xml files; die
 * sizes and card framebuffers were not re-verified for this choice. Whether
 * nvidia-vgpud and the RM accept another die's profile table is unknown, and
 * no profile is filtered against the card's real framebuffer.
 *
 * Targets and subsystems (stock vgpuConfig.xml <pgpu> devId):
 *   0x2230 RTX A6000           subsystemId 0x0 (any)
 *   0x25B6 A2                  10de:157e (A16 is 10de:14a9 on the same ID)
 *   0x26B1 RTX 6000 Ada        subsystemId 0x0 (any)
 *   0x27B8 L4                  subsystemId 0x0 (any)
 *   0x2BB5 RTX PRO 6000 BW SE  10de:204e (580.159.01 and 595.91.04; 595 also
 *                              lists 10de:220b)
 *   0x2C3A RTX PRO 4500 BW SE  10de:21f4
 */
static const struct gs_device gs_devices[] = {
	/*
	 * GA102 -> RTX A6000, same die. 2206 is the ID of the two memory-modded
	 * RTX 3080 20GB cards the method was demonstrated on (subsystem 10de:146d
	 * only); other 2206 boards are untested.
	 */
	GS_AMPERE_ENTRY(0x2203, "GA102", "GeForce RTX 3090 Ti", 0x2230, 0, 0, "RTX A6000", 1, 1),
	GS_AMPERE_ENTRY(0x2204, "GA102", "GeForce RTX 3090", 0x2230, 0, 0, "RTX A6000", 1, 1),
	GS_AMPERE_ENTRY(0x2206, "GA102", "GeForce RTX 3080", 0x2230, 0, 0, "RTX A6000", 1, 0),
	GS_AMPERE_ENTRY(0x2208, "GA102", "GeForce RTX 3080 Ti", 0x2230, 0, 0, "RTX A6000", 1, 1),
	GS_AMPERE_ENTRY(0x220a, "GA102", "GeForce RTX 3080 12GB", 0x2230, 0, 0, "RTX A6000", 1, 1),
	GS_AMPERE_ENTRY(0x2216, "GA102", "GeForce RTX 3080 LHR", 0x2230, 0, 0, "RTX A6000", 1, 1),

	/* GA104 -> A2 (GA107), cross-die. */
	GS_AMPERE_ENTRY(0x2482, "GA104", "GeForce RTX 3070 Ti", 0x25b6, 0x10de, 0x157e, "A2", 0, 1),
	GS_AMPERE_ENTRY(0x2484, "GA104", "GeForce RTX 3070", 0x25b6, 0x10de, 0x157e, "A2", 0, 1),
	GS_AMPERE_ENTRY(0x2486, "GA104", "GeForce RTX 3060 Ti", 0x25b6, 0x10de, 0x157e, "A2", 0, 1),
	GS_AMPERE_ENTRY(0x2487, "GA104", "GeForce RTX 3060", 0x25b6, 0x10de, 0x157e, "A2", 0, 1),
	GS_AMPERE_ENTRY(0x2488, "GA104", "GeForce RTX 3070 LHR", 0x25b6, 0x10de, 0x157e, "A2", 0, 1),
	GS_AMPERE_ENTRY(0x2489, "GA104", "GeForce RTX 3060 Ti LHR", 0x25b6, 0x10de, 0x157e, "A2", 0, 1),

	/* GA106 -> A2 (GA107), cross-die. */
	GS_AMPERE_ENTRY(0x2503, "GA106", "GeForce RTX 3060", 0x25b6, 0x10de, 0x157e, "A2", 0, 1),
	GS_AMPERE_ENTRY(0x2504, "GA106", "GeForce RTX 3060 LHR", 0x25b6, 0x10de, 0x157e, "A2", 0, 1),
	GS_AMPERE_ENTRY(0x2507, "GA106", "GeForce RTX 3050", 0x25b6, 0x10de, 0x157e, "A2", 0, 1),
	GS_AMPERE_ENTRY(0x2544, "GA106", "GeForce RTX 3060", 0x25b6, 0x10de, 0x157e, "A2", 0, 1),

	/*
	 * GA107 -> A2, same die. No desktop GA107 GeForce dump was available;
	 * the hidden block is shown on GA107 laptop parts (25a0, 25a2)
	 * and the GA107 RTX A1000/A400. The die of 2582/2584 is from pci.ids.
	 */
	GS_AMPERE_ENTRY(0x2582, "GA107", "GeForce RTX 3050 8GB", 0x25b6, 0x10de, 0x157e, "A2", 1, 1),
	GS_AMPERE_ENTRY(0x2584, "GA107", "GeForce RTX 3050 6GB", 0x25b6, 0x10de, 0x157e, "A2", 1, 1),

	/* AD102 -> RTX 6000 Ada, same die. */
	GS_ADA_ENTRY(0x2684, "AD102", "GeForce RTX 4090", 0x26b1, "RTX 6000 Ada", 1),
	GS_ADA_ENTRY(0x2685, "AD102", "GeForce RTX 4090 D", 0x26b1, "RTX 6000 Ada", 1),
	GS_ADA_ENTRY(0x2689, "AD102", "GeForce RTX 4070 Ti SUPER", 0x26b1, "RTX 6000 Ada", 1),

	/* AD103 -> L4 (AD104), cross-die. */
	GS_ADA_ENTRY(0x2702, "AD103", "GeForce RTX 4080 SUPER", 0x27b8, "L4", 0),
	GS_ADA_ENTRY(0x2704, "AD103", "GeForce RTX 4080", 0x27b8, "L4", 0),
	GS_ADA_ENTRY(0x2705, "AD103", "GeForce RTX 4070 Ti SUPER", 0x27b8, "L4", 0),
	GS_ADA_ENTRY(0x2709, "AD103", "GeForce RTX 4070", 0x27b8, "L4", 0),

	/* AD104 -> L4, same die. */
	GS_ADA_ENTRY(0x2782, "AD104", "GeForce RTX 4070 Ti", 0x27b8, "L4", 1),
	GS_ADA_ENTRY(0x2783, "AD104", "GeForce RTX 4070 SUPER", 0x27b8, "L4", 1),
	GS_ADA_ENTRY(0x2786, "AD104", "GeForce RTX 4070", 0x27b8, "L4", 1),
	GS_ADA_ENTRY(0x2788, "AD104", "GeForce RTX 4060 Ti", 0x27b8, "L4", 1),

	/* AD106, AD107 -> L4 (AD104), cross-die. */
	GS_ADA_ENTRY(0x2803, "AD106", "GeForce RTX 4060 Ti", 0x27b8, "L4", 0),
	GS_ADA_ENTRY(0x2805, "AD106", "GeForce RTX 4060 Ti 16GB", 0x27b8, "L4", 0),
	GS_ADA_ENTRY(0x2882, "AD107", "GeForce RTX 4060", 0x27b8, "L4", 0),

	/* GB202 -> RTX PRO 6000 Blackwell Server Edition, same die. */
	GS_BLACKWELL_ENTRY(0x2b85, "GB202", "GeForce RTX 5090", 0x2bb5, 0x204e,
			   "RTX PRO 6000 Blackwell Server Edition", 1),

	/* GB203 -> RTX PRO 4500 Blackwell Server Edition, same die. */
	GS_BLACKWELL_ENTRY(0x2c02, "GB203", "GeForce RTX 5080", 0x2c3a, 0x21f4,
			   "RTX PRO 4500 Blackwell Server Edition", 1),
	GS_BLACKWELL_ENTRY(0x2c05, "GB203", "GeForce RTX 5070 Ti", 0x2c3a, 0x21f4,
			   "RTX PRO 4500 Blackwell Server Edition", 1),

	/* GB206 -> RTX PRO 4500 (GB203), cross-die. */
	GS_BLACKWELL_ENTRY(0x2d04, "GB206", "GeForce RTX 5060 Ti", 0x2c3a, 0x21f4,
			   "RTX PRO 4500 Blackwell Server Edition", 0),
	GS_BLACKWELL_ENTRY(0x2d05, "GB206", "GeForce RTX 5060", 0x2c3a, 0x21f4,
			   "RTX PRO 4500 Blackwell Server Edition", 0),

	/* GB205 -> RTX PRO 4500 (GB203), cross-die. */
	GS_BLACKWELL_ENTRY(0x2f04, "GB205", "GeForce RTX 5070", 0x2c3a, 0x21f4,
			   "RTX PRO 4500 Blackwell Server Edition", 0),
};

#define GS_DEVICE_COUNT (sizeof(gs_devices) / sizeof(gs_devices[0]))

/* The table entry for an NVIDIA device ID, or 0 (NULL) if it is not listed. */
static inline const struct gs_device *gs_device_lookup(unsigned int device)
{
	unsigned int i;

	for (i = 0; i < GS_DEVICE_COUNT; i++)
		if (gs_devices[i].device == device)
			return &gs_devices[i];
	return 0;
}

static inline const char *gs_generation_name(unsigned int generation)
{
	switch (generation) {
	case GS_AMPERE:
		return "Ampere";
	case GS_ADA:
		return "Ada";
	case GS_BLACKWELL:
		return "Blackwell";
	}
	return "unknown";
}

/*
 * 1 if the entry's SR-IOV capability is linked natively in the chain at
 * sriov_pos (Blackwell), 0 if it is hidden there (Ampere, Ada).
 */
static inline int gs_sriov_native(const struct gs_device *d)
{
	return d->generation == GS_BLACKWELL;
}

/*
 * Is @hdr an extended capability header with this ID and version?
 * (ID in bits 15:0, version in bits 19:16.)
 */
static inline int gs_ext_cap_hdr_is(unsigned int hdr, unsigned int id,
				    unsigned int version)
{
	return (hdr & 0xffffu) == id && ((hdr >> 16) & 0xfu) == version;
}

/*
 * The most VFs geforce-sriov may present for a card whose hardware TotalVFs
 * register reads @raw: @raw itself, except that on Ada and Blackwell a raw
 * value of exactly 1 (the count their GeForce dumps report) counts as
 * GS_DEFAULT_VFS when @experimental_vfs is set (untested on hardware). 0
 * stays 0: it may mean no usable VF resources, and no dump shows it.
 */
static inline unsigned int gs_vf_ceiling(const struct gs_device *d,
					 unsigned int raw, int experimental_vfs)
{
	if (raw == 1 && experimental_vfs &&
	    (d->generation == GS_ADA || d->generation == GS_BLACKWELL))
		return GS_DEFAULT_VFS;
	return raw;
}

/*
 * The TotalVFs/InitialVFs value the kernel is shown: the configured count
 * @vfs, never more than gs_vf_ceiling() (Ampere reads 16 and gets 4 with the
 * default count).
 */
static inline unsigned int gs_vf_limit(const struct gs_device *d,
				       unsigned int raw, int experimental_vfs,
				       unsigned int vfs)
{
	unsigned int ceiling = gs_vf_ceiling(d, raw, experimental_vfs);

	return vfs < ceiling ? vfs : ceiling;
}

/*
 * Where SR-IOV puts the VFs of a PF at function 0 of a slot: VF i is routing
 * ID PF + offset + i * stride (PCIe Base Spec, SR-IOV First VF Offset and VF
 * Stride; Linux pci_iov_virtfn_devfn()). Only the two demonstrated cards were
 * seen to decode just function bits 2:0 (see the Ampere note above); no
 * other card has been observed, and every listed card gets the same
 * conservative rule: every VF must land on functions 1-7 of the PF's own
 * slot, on a function no other device already uses.
 */
enum gs_vf_geometry {
	GS_GEOM_OK = 0,
	GS_GEOM_OFFSET_ZERO,	/* VF0 would be the PF itself */
	GS_GEOM_STRIDE_ZERO,	/* several VFs on one function */
	GS_GEOM_BEYOND_SLOT,	/* a VF beyond function 7 of the PF's slot */
	GS_GEOM_COLLISION,	/* a VF on a function that is already in use */
	GS_GEOM_NUMVFS_MISMATCH,	/* NumVFs reads back another value */
};

/*
 * Check @numvfs VFs at first VF offset @offset and stride @stride (register
 * values, 16 bits) against @occupied, the mask of functions of the slot that
 * are already in use (bit n = function n). @numvfs 0 places no VF and is OK.
 * Never overflows, and loops at most GS_SLOT_FNS - 1 times.
 */
static inline enum gs_vf_geometry
gs_vf_geometry_check(unsigned int numvfs, unsigned int offset,
		     unsigned int stride, unsigned int occupied)
{
	unsigned int i, fn;

	if (!numvfs)
		return GS_GEOM_OK;
	if (!offset)
		return GS_GEOM_OFFSET_ZERO;
	if (numvfs > 1 && !stride)
		return GS_GEOM_STRIDE_ZERO;
	if (offset > GS_SLOT_FNS - 1 ||
	    (numvfs > 1 &&
	     stride > (GS_SLOT_FNS - 1 - offset) / (numvfs - 1)))
		return GS_GEOM_BEYOND_SLOT;
	/* Now offset + stride * (numvfs - 1) <= 7, so numvfs <= 7. */
	for (i = 0, fn = offset; i < numvfs; i++, fn += stride)
		if (occupied & (1u << fn))
			return GS_GEOM_COLLISION;
	return GS_GEOM_OK;
}

/*
 * The VF layout decision while Linux sets up SR-IOV: @requested is the
 * NumVFs value the kernel wrote, @readback what NumVFs reads afterwards, and
 * @offset/@stride what the card then reports. Requested 0 places no VF. A
 * card that clamps or ignores the write (readback != requested) is refused,
 * because the kernel would later create @requested VFs; otherwise the layout
 * of @requested VFs is checked.
 */
static inline enum gs_vf_geometry
gs_vf_setup_check(unsigned int requested, unsigned int readback,
		  unsigned int offset, unsigned int stride,
		  unsigned int occupied)
{
	if (!requested)
		return GS_GEOM_OK;
	if (readback != requested)
		return GS_GEOM_NUMVFS_MISMATCH;
	return gs_vf_geometry_check(requested, offset, stride, occupied);
}

static inline const char *gs_vf_geometry_text(enum gs_vf_geometry g)
{
	switch (g) {
	case GS_GEOM_OK:
		return "every VF on its own free function 1-7 of the PF's slot";
	case GS_GEOM_OFFSET_ZERO:
		return "first VF offset 0 puts VF0 on the PF itself";
	case GS_GEOM_STRIDE_ZERO:
		return "VF stride 0 puts every VF on one function";
	case GS_GEOM_BEYOND_SLOT:
		return "a VF would land beyond function 7 of the PF's slot (such a function may answer as the PF or its audio function)";
	case GS_GEOM_COLLISION:
		return "a VF would land on a function another device already uses";
	case GS_GEOM_NUMVFS_MISMATCH:
		return "NumVFs does not read back the value the kernel wrote";
	}
	return "unknown";
}

/* A PCI function address. devfn = slot << 3 | function. */
struct gs_bdf {
	unsigned int domain;
	unsigned int bus;
	unsigned int devfn;
};

static inline int gs_hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

/* Parse exactly @n lowercase hex digits at *@s and advance it. */
static inline int gs_hex_n(const char **s, unsigned int n, unsigned int *out)
{
	unsigned int v = 0, i;
	int d;

	for (i = 0; i < n; i++) {
		d = gs_hex_digit((*s)[i]);
		if (d < 0)
			return 0;
		v = v * 16 + (unsigned int)d;
	}
	*s += n;
	*out = v;
	return 1;
}

/*
 * Parse a canonical PCI domain at *@s and advance it: 4 to 8 lowercase hex
 * digits, more than 4 only without a leading 0 (as "%04x" prints it), value
 * at most GS_PCI_DOMAIN_MAX. Returns 1 on success, 0 on any deviation.
 */
static inline int gs_parse_domain_at(const char **s, unsigned int *out)
{
	const char *p = *s;
	unsigned int n = 0, v;

	while (n <= 8 && gs_hex_digit(p[n]) >= 0)
		n++;
	if (n < 4 || n > 8 || (n > 4 && p[0] == '0'))
		return 0;
	if (!gs_hex_n(&p, n, &v) || v > GS_PCI_DOMAIN_MAX)
		return 0;
	*s = p;
	*out = v;
	return 1;
}

/*
 * Parse one canonical PCI function address "dddd:bb:ss.f" (lowercase hex,
 * exactly as Linux names the device in sysfs; the domain may have up to 8
 * digits, see gs_parse_domain_at()) from *@s, advancing *@s past it.
 * Returns 1 on success, 0 on any deviation.
 */
static inline int gs_parse_bdf_at(const char **s, struct gs_bdf *out)
{
	const char *p = *s;
	unsigned int dom, bus, slot, fn;

	if (!gs_parse_domain_at(&p, &dom) || *p++ != ':')
		return 0;
	if (!gs_hex_n(&p, 2, &bus) || *p++ != ':')
		return 0;
	if (!gs_hex_n(&p, 2, &slot) || *p++ != '.')
		return 0;
	if (!gs_hex_n(&p, 1, &fn))
		return 0;
	if (slot > 0x1f || fn > 7)
		return 0;
	out->domain = dom;
	out->bus = bus;
	out->devfn = slot << 3 | fn;
	*s = p;
	return 1;
}

/* Parse a whole string that must be exactly one canonical address. */
static inline int gs_parse_bdf(const char *s, struct gs_bdf *out)
{
	if (!s || !gs_parse_bdf_at(&s, out))
		return 0;
	return *s == '\0';
}

/*
 * Parse one canonical physical-function address (gs_parse_bdf_at()) whose
 * function is 0: the card's PF is function 0 and its VFs are other
 * functions of the same slot. Returns 1 on success, 0 on any deviation.
 */
static inline int gs_parse_pf_bdf_at(const char **s, struct gs_bdf *out)
{
	const char *p = *s;
	struct gs_bdf b;

	if (!gs_parse_bdf_at(&p, &b) || (b.devfn & 7u) != 0)
		return 0;
	*out = b;
	*s = p;
	return 1;
}

/* Parse a whole string that must be exactly one canonical PF BDF. */
static inline int gs_parse_pf_bdf(const char *s, struct gs_bdf *out)
{
	if (!s || !gs_parse_pf_bdf_at(&s, out))
		return 0;
	return *s == '\0';
}

/*
 * Parse a comma-separated list of canonical PF BDFs ("" is an empty list)
 * into @out (room for @max). Returns the number parsed, or -1 if a token is
 * not canonical, a BDF repeats, or there are more than @max.
 */
static inline int gs_parse_pf_bdf_list(const char *s, struct gs_bdf *out,
				       unsigned int max)
{
	unsigned int n = 0, i;
	struct gs_bdf b;

	if (!s)
		return -1;
	if (*s == '\0')
		return 0;
	for (;;) {
		if (!gs_parse_pf_bdf_at(&s, &b))
			return -1;
		for (i = 0; i < n; i++)
			if (out[i].domain == b.domain && out[i].bus == b.bus &&
			    out[i].devfn == b.devfn)
				return -1;
		if (n == max)
			return -1;
		out[n++] = b;
		if (*s == '\0')
			return (int)n;
		if (*s++ != ',')
			return -1;
	}
}

#endif /* GS_DEVICES_H */
