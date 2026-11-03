// SPDX-License-Identifier: MIT
/*
 * Read-only PCI inspection through a sysfs root, and the pure decision whether
 * `prepare` may remove and rescan a card. Nothing in this file writes.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gs_lifecycle.h"

static void read_driver(const char *devdir, char *out, size_t len)
{
	char link[GS_PATH_MAX], target[GS_PATH_MAX];
	ssize_t n;
	const char *base;

	out[0] = '\0';
	if (gs_pathf(link, sizeof(link), "%s/driver", devdir))
		return;
	n = readlink(link, target, sizeof(target) - 1);
	if (n <= 0)
		return;
	target[n] = '\0';
	base = strrchr(target, '/');
	gs_copy(out, len, base ? base + 1 : target);
}

static void read_fn(const char *sysroot, const struct gs_addr *a, struct gs_fn *fn)
{
	char dir[GS_PATH_MAX], p[GS_PATH_MAX];

	memset(fn, 0, sizeof(*fn));
	fn->bdf = *a;
	if (gs_pathf(dir, sizeof(dir), "%s/bus/pci/devices/%s", sysroot, a->str))
		return;
	if (!gs_pathf(p, sizeof(p), "%s/vendor", dir))
		gs_read_hex(p, &fn->vendor);
	if (!gs_pathf(p, sizeof(p), "%s/device", dir))
		gs_read_hex(p, &fn->device);
	if (!gs_pathf(p, sizeof(p), "%s/class", dir))
		gs_read_hex(p, &fn->class_code);
	if (!gs_pathf(p, sizeof(p), "%s/physfn", dir))
		fn->is_vf = gs_exists(p);
	read_driver(dir, fn->driver, sizeof(fn->driver));
}

/* DevCap2 ARI Forwarding Supported of a bridge, from its sysfs config file; -1 when unreadable. */
static int bridge_ari_forwarding(const char *bridge_dir)
{
	unsigned char cfg[256];
	char p[GS_PATH_MAX];
	unsigned int pos, guard = 0;
	ssize_t n;
	int fd;

	if (gs_pathf(p, sizeof(p), "%s/config", bridge_dir))
		return -1;
	fd = open(p, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = pread(fd, cfg, sizeof(cfg), 0);
	close(fd);
	/* Unprivileged reads of sysfs config stop at 64 bytes. */
	if (n < (ssize_t)sizeof(cfg) || !(cfg[0x06] & 0x10))
		return -1;
	pos = cfg[0x34] & 0xfc;
	while (pos >= 0x40 && pos < 0xfc && guard++ < 48) {
		if (cfg[pos] == 0x10) { /* PCI Express capability */
			unsigned int devcap2;

			if (pos + 0x28 > sizeof(cfg))
				return -1;
			devcap2 = (unsigned int)cfg[pos + 0x24] | (unsigned int)cfg[pos + 0x25] << 8 |
				  (unsigned int)cfg[pos + 0x26] << 16 | (unsigned int)cfg[pos + 0x27] << 24;
			return (devcap2 >> 5) & 1;
		}
		pos = cfg[pos + 1] & 0xfc;
	}
	return -1;
}

/* <pf>/resource: is BAR0 (line 0), and is any VF BAR (lines 7-12) assigned? -1 stays when unknown. */
static void read_resources(const char *dir, struct gs_card *card)
{
	char p[GS_PATH_MAX], *data, *line, *save = NULL;
	unsigned int n = 0;
	size_t len;
	int vf = 0;

	if (gs_pathf(p, sizeof(p), "%s/resource", dir) || gs_read_whole(p, &data, &len, 1 << 14))
		return;
	for (line = strtok_r(data, "\n", &save); line && n <= 12; line = strtok_r(NULL, "\n", &save), n++) {
		unsigned long long start, end, flags;

		if (sscanf(line, "%llx %llx %llx", &start, &end, &flags) != 3)
			break;
		if (n == 0)
			card->bar0_assigned = start || end;
		else if (n >= 7)
			vf |= start || end;
		if (n == 12)
			card->vf_bars_assigned = vf;
	}
	free(data);
}

int gs_card_inspect(const char *sysroot, const struct gs_addr *pf, struct gs_card *card)
{
	char dir[GS_PATH_MAX], p[GS_PATH_MAX], real[PATH_MAX], parent[PATH_MAX];
	char busname[GS_BUS_STR_SIZE];
	struct gs_fn pffn;
	DIR *d;
	struct dirent *de;
	char *slash;
	unsigned int v;
	char buf[32];

	memset(card, 0, sizeof(*card));
	card->pf = *pf;
	card->boot_vga = -1;
	card->ari_enabled = -1;
	card->bridge_ari_fwd_supported = -1;
	card->bar0_assigned = -1;
	card->vf_bars_assigned = -1;
	if (gs_pathf(dir, sizeof(dir), "%s/bus/pci/devices/%s", sysroot, pf->str))
		return -ENAMETOOLONG;
	if (!realpath(dir, real))
		return 0;
	card->present = 1;
	read_fn(sysroot, pf, &pffn);
	card->vendor = pffn.vendor;
	card->device = pffn.device;
	card->class_code = pffn.class_code;
	card->is_vf = pffn.is_vf;
	snprintf(card->driver, sizeof(card->driver), "%s", pffn.driver);
	if (!gs_pathf(p, sizeof(p), "%s/subsystem_vendor", dir))
		gs_read_hex(p, &card->subvendor);
	if (!gs_pathf(p, sizeof(p), "%s/subsystem_device", dir))
		gs_read_hex(p, &card->subdevice);
	if (card->vendor == GS_VENDOR_NVIDIA)
		card->entry = gs_device_lookup(card->device);
	if (!gs_pathf(p, sizeof(p), "%s/boot_vga", dir) && !gs_read_uint(p, &v))
		card->boot_vga = v ? 1 : 0;
	if (!gs_pathf(p, sizeof(p), "%s/sriov_totalvfs", dir) && !gs_read_uint(p, &v)) {
		card->has_sriov = 1;
		card->totalvfs = v;
		if (!gs_pathf(p, sizeof(p), "%s/sriov_numvfs", dir) && !gs_read_uint(p, &v))
			card->numvfs = v;
	}
	read_resources(dir, card);
	if (!gs_pathf(p, sizeof(p), "%s/ari_enabled", dir) && !gs_read_uint(p, &v))
		card->ari_enabled = v ? 1 : 0;
	if (!gs_pathf(p, sizeof(p), "%s/iommu_group", dir)) {
		char target[GS_PATH_MAX];
		ssize_t n = readlink(p, target, sizeof(target) - 1);

		if (n > 0) {
			target[n] = '\0';
			slash = strrchr(target, '/');
			gs_copy(card->iommu_group, sizeof(card->iommu_group), slash ? slash + 1 : target);
		}
	}

	/* Parent of the function's device directory: a bridge function or the host bridge (pciDDDD:BB). */
	snprintf(parent, sizeof(parent), "%s", real);
	slash = strrchr(parent, '/');
	if (!slash)
		return 0;
	*slash = '\0';
	slash = strrchr(parent, '/');
	if (slash && gs_addr_parse(slash + 1, &card->bridge) == 0) {
		card->bridge_is_pci = 1;
		snprintf(card->bridge_dir, sizeof(card->bridge_dir), "%s", parent);
		snprintf(busname, sizeof(busname), "%04x:%02x", pf->domain, pf->bus);
		if (!gs_pathf(card->rescan_path, sizeof(card->rescan_path), "%s/pci_bus/%s/rescan", parent,
			      busname))
			card->rescan_present = gs_exists(card->rescan_path);
		if (!gs_pathf(p, sizeof(p), "%s/power/control", parent) &&
		    gs_read_attr(p, buf, sizeof(buf)) == 0)
			gs_copy(card->bridge_pm, sizeof(card->bridge_pm), buf);
		if (!gs_pathf(p, sizeof(p), "%s/power/runtime_status", parent) &&
		    gs_read_attr(p, buf, sizeof(buf)) == 0)
			gs_copy(card->bridge_rt, sizeof(card->bridge_rt), buf);
		card->bridge_ari_fwd_supported = bridge_ari_forwarding(parent);
	}

	/* Every function on the PF's bus: the card's own (same slot) and anything else. */
	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices", sysroot))
		return 0;
	d = opendir(p);
	if (!d)
		return 0;
	while ((de = readdir(d))) {
		struct gs_addr a;
		struct gs_fn fn;

		if (gs_addr_parse(de->d_name, &a) || a.domain != pf->domain || a.bus != pf->bus ||
		    strcmp(a.str, pf->str) == 0)
			continue;
		read_fn(sysroot, &a, &fn);
		if (fn.is_vf)
			card->nvfs_on_bus++;
		if (a.slot == pf->slot) {
			if (card->nfns < GS_MAX_BUS_FNS)
				card->fns[card->nfns++] = fn;
		} else if (card->nothers < GS_MAX_BUS_FNS) {
			card->others[card->nothers++] = fn;
		}
	}
	closedir(d);
	return 0;
}

#define REFUSE(...) do { \
		plan->verdict = GS_PLAN_REFUSE; \
		snprintf(plan->reason, sizeof(plan->reason), __VA_ARGS__); \
		return; \
	} while (0)

static int is_display_class(unsigned int class_code)
{
	return (class_code >> 16) == 0x03;
}

static int is_audio_fn(const struct gs_fn *fn)
{
	return (fn->class_code >> 8) == 0x0403;
}

void gs_plan_prepare(const struct gs_card *c, const struct gs_display *d, struct gs_plan *plan)
{
	unsigned int i;

	memset(plan, 0, sizeof(*plan));
	plan->verdict = GS_PLAN_READY;
	if (!c->present)
		REFUSE("%s: no such PCI function (missing PF)", c->pf.str);
	if (c->pf.func != 0)
		REFUSE("%s: not function 0; only a physical function .0 is accepted", c->pf.str);
	if (c->is_vf)
		REFUSE("%s: is a virtual function, not a physical function", c->pf.str);
	if (c->vendor != GS_VENDOR_NVIDIA)
		REFUSE("%s: vendor 0x%04x is not NVIDIA", c->pf.str, c->vendor);
	if (!c->entry)
		REFUSE("%s: 10de:%04x is not in the device table (see `geforce-sriov devices`)",
		       c->pf.str, c->device);
	if (!is_display_class(c->class_code))
		REFUSE("%s: class 0x%06x is not a display controller", c->pf.str, c->class_code);
	if (c->boot_vga == 1 && !d)
		REFUSE("%s: is the boot display device (boot_vga=1); preparing it releases the console and needs boot_display_release in the config and `prepare --release-console=%s`",
		       c->pf.str, c->pf.str);
	if (d && c->boot_vga != 1)
		REFUSE("%s: boot_display_release grants it, but it is not the boot display device (boot_vga=%d); remove it from boot_display_release",
		       c->pf.str, c->boot_vga);
	if (c->driver[0])
		REFUSE("%s: bound to driver '%s'; preparation must run before any display driver binds the PF",
		       c->pf.str, c->driver);
	if (c->has_sriov && c->numvfs > 0)
		REFUSE("%s: %u virtual functions are active", c->pf.str, c->numvfs);
	if (c->nvfs_on_bus > 0)
		REFUSE("%s: %u virtual functions exist on bus %04x:%02x", c->pf.str, c->nvfs_on_bus,
		       c->pf.domain, c->pf.bus);
	if (c->nothers > 0)
		REFUSE("%s: bus %04x:%02x is shared with %s (%04x:%04x); removing the bus would touch an unrelated device",
		       c->pf.str, c->pf.domain, c->pf.bus, c->others[0].bdf.str, c->others[0].vendor,
		       c->others[0].device);
	for (i = 0; i < c->nfns; i++) {
		const struct gs_fn *fn = &c->fns[i];

		if (!fn->driver[0])
			continue;
		if (is_audio_fn(fn) && fn->vendor == GS_VENDOR_NVIDIA &&
		    strcmp(fn->driver, "snd_hda_intel") == 0 && plan->nunbind < GS_PLAN_MAX_UNBIND) {
			plan->unbind[plan->nunbind++] = fn->bdf;
			continue;
		}
		REFUSE("%s: function %s is bound to '%s'; only the card's HDMI audio (snd_hda_intel) is unbound automatically",
		       c->pf.str, fn->bdf.str, fn->driver);
	}
	if (!c->bridge_is_pci)
		REFUSE("%s: sits on a root bus without a PCI bridge; the bridge-scoped rescan is not possible",
		       c->pf.str);
	if (!c->rescan_present)
		REFUSE("%s: bridge %s has no pci_bus/%04x:%02x/rescan", c->pf.str, c->bridge.str, c->pf.domain,
		       c->pf.bus);
	if (!c->bridge_pm[0])
		REFUSE("%s: bridge %s power/control is unreadable; it cannot be held awake", c->pf.str,
		       c->bridge.str);
	if (c->bar0_assigned == 0)
		REFUSE("%s: BAR0 is not assigned; a rescan cannot place it", c->pf.str);
	if (c->has_sriov && c->entry && !gs_sriov_native(c->entry))
		REFUSE("%s: already presents SR-IOV (sriov_totalvfs=%u) from another kernel change (patched kernel or other livepatch); prepare does not stack on it",
		       c->pf.str, c->totalvfs);
	if (d) {
		if (d->refuse[0])
			REFUSE("%s: boot display: releasing its console is not safe: %s", c->pf.str, d->refuse);
		if (gs_display_fw_state(d) == GS_FW_BOUND) {
			gs_copy(plan->release, sizeof(plan->release), d->fw);
			gs_copy(plan->release_driver, sizeof(plan->release_driver), d->fw_driver);
		}
	}
}

/* Checks for `install --device` on a running system, where the PF may be bound to nvidia. */
int gs_check_selectable(const struct gs_card *c, int release_granted, char *reason, size_t len)
{
	if (!c->present)
		return snprintf(reason, len, "%s: no such PCI function", c->pf.str), -1;
	if (c->pf.func != 0 || c->is_vf)
		return snprintf(reason, len, "%s: not a physical function .0", c->pf.str), -1;
	if (c->vendor != GS_VENDOR_NVIDIA || !c->entry)
		return snprintf(reason, len, "%s: %04x:%04x is not in the device table", c->pf.str,
				c->vendor, c->device), -1;
	if (!is_display_class(c->class_code))
		return snprintf(reason, len, "%s: not a display controller", c->pf.str), -1;
	if (c->boot_vga == 1 && !release_granted)
		return snprintf(reason, len, "%s: is the boot display device (boot_vga=1); select it only together with --boot-display-release %s",
				c->pf.str, c->pf.str), -1;
	if (c->nothers > 0)
		return snprintf(reason, len, "%s: bus shared with %s", c->pf.str, c->others[0].bdf.str), -1;
	if (!c->bridge_is_pci)
		return snprintf(reason, len, "%s: on a root bus without a PCI bridge", c->pf.str), -1;
	return 0;
}

/* ---- boot display consumers (read-only) ----------------------------------- */

static void display_refuse(struct gs_display *d, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Keep the first reason: it names what to fix first. */
static void display_refuse(struct gs_display *d, const char *fmt, ...)
{
	va_list ap;

	if (d->refuse[0])
		return;
	va_start(ap, fmt);
	vsnprintf(d->refuse, sizeof(d->refuse), fmt, ap);
	va_end(ap);
}

/* name is prefix followed by one or more decimal digits only. */
static int name_with_number(const char *name, const char *prefix)
{
	size_t n = strlen(prefix);
	const char *p = name + n;

	if (strncmp(name, prefix, n) != 0 || !*p)
		return 0;
	for (; *p; p++)
		if (*p < '0' || *p > '9')
			return 0;
	return 1;
}

/* A platform device the kernel's sysfb creates for a firmware framebuffer (drivers/firmware/sysfb.c). */
static int is_sysfb_device(const char *name)
{
	static const char *const kinds[] = { GS_SIMPLEFB_PREFIX, "efi-framebuffer.", "vesa-framebuffer.",
					     "vga-framebuffer.", "ega-framebuffer.", "platform-framebuffer." };

	for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
		if (name_with_number(name, kinds[i]))
			return 1;
	return 0;
}

static int path_under(const char *path, const char *dir)
{
	size_t n = strlen(dir);

	return strncmp(path, dir, n) == 0 && (path[n] == '/' || path[n] == '\0');
}

static const char *base_of(const char *path)
{
	const char *s = strrchr(path, '/');

	return s ? s + 1 : path;
}

/* The firmware framebuffer platform devices: only one, a simple-framebuffer child of this PF, is accepted. */
static void scan_firmware_fb(const char *sysroot, const char *pfreal, struct gs_display *d)
{
	char dir[GS_PATH_MAX], p[GS_PATH_MAX], real[PATH_MAX], parent[PATH_MAX];
	struct dirent *de;
	DIR *dd;

	if (gs_pathf(dir, sizeof(dir), "%s/bus/platform/devices", sysroot)) {
		display_refuse(d, "sysfs path too long");
		return;
	}
	dd = opendir(dir);
	if (!dd) {
		if (errno != ENOENT)
			display_refuse(d, "%s cannot be read", dir);
		return;
	}
	while ((de = readdir(dd))) {
		if (!is_sysfb_device(de->d_name))
			continue;
		if (gs_pathf(p, sizeof(p), "%s/%s", dir, de->d_name) || !realpath(p, real)) {
			display_refuse(d, "firmware framebuffer %.64s does not resolve to a device; its owner cannot be proven",
				       de->d_name);
			continue;
		}
		gs_copy(parent, sizeof(parent), real);
		*strrchr(parent, '/') = '\0';
		if (strcmp(parent, pfreal) != 0) {
			display_refuse(d, "firmware framebuffer %.64s belongs to %.64s, not to this card; which card it scans out of cannot be proven",
				       de->d_name, base_of(parent));
			continue;
		}
		if (!name_with_number(de->d_name, GS_SIMPLEFB_PREFIX)) {
			display_refuse(d, "firmware framebuffer %.64s is not a simple-framebuffer device; only simple-framebuffer is released",
				       de->d_name);
			continue;
		}
		if (d->fw[0]) {
			display_refuse(d, "more than one firmware framebuffer (%s, %.64s)", d->fw, de->d_name);
			continue;
		}
		gs_copy(d->fw, sizeof(d->fw), de->d_name);
		gs_copy(d->fw_dir, sizeof(d->fw_dir), real);
		{
			char link[GS_PATH_MAX], target[GS_PATH_MAX];
			ssize_t n;

			if (!gs_pathf(link, sizeof(link), "%s/driver", real) &&
			    (n = readlink(link, target, sizeof(target) - 1)) > 0) {
				target[n] = '\0';
				gs_copy(d->fw_driver, sizeof(d->fw_driver), base_of(target));
			}
		}
		/* simpledrm and simplefb both register the platform driver "simple-framebuffer". */
		if (d->fw_driver[0] && strcmp(d->fw_driver, "simple-framebuffer") != 0)
			display_refuse(d, "%s is bound to '%s', not to simple-framebuffer (simpledrm or simplefb)", d->fw,
				       d->fw_driver);
	}
	closedir(dd);
}

/*
 * fb and DRM devices: each must belong to the PF's own firmware framebuffer or to another PCI
 * device. One on the PF itself, on another platform device, or without a resolvable device refuses.
 */
static void scan_class(const char *sysroot, const char *cls, const char *const *prefixes, const char *pfreal,
		       struct gs_display *d)
{
	char dir[GS_PATH_MAX], p[GS_PATH_MAX], real[PATH_MAX];
	struct dirent *de;
	DIR *dd;

	if (gs_pathf(dir, sizeof(dir), "%s/class/%s", sysroot, cls)) {
		display_refuse(d, "sysfs path too long");
		return;
	}
	dd = opendir(dir);
	if (!dd) {
		if (errno != ENOENT)
			display_refuse(d, "%s cannot be read", dir);
		return;
	}
	while ((de = readdir(dd))) {
		struct gs_addr a;
		int match = 0;

		for (size_t i = 0; prefixes[i]; i++)
			match |= name_with_number(de->d_name, prefixes[i]);
		if (!match)
			continue;
		if (gs_pathf(p, sizeof(p), "%s/%s/device", dir, de->d_name) || !realpath(p, real)) {
			display_refuse(d, "%s/%.64s has no resolvable device; which card it uses cannot be told", cls,
				       de->d_name);
			continue;
		}
		if (d->fw_dir[0] && strcmp(real, d->fw_dir) == 0) {
			d->nconsumers++;
			continue;
		}
		if (path_under(real, pfreal)) {
			display_refuse(d, "%s/%.64s is on the card itself (%.64s), not on its firmware framebuffer", cls,
				       de->d_name, base_of(real));
			continue;
		}
		if (gs_addr_parse(base_of(real), &a) != 0)
			display_refuse(d, "%s/%.64s belongs to %.64s, which is not a PCI device; it may scan out of this card",
				       cls, de->d_name, base_of(real));
	}
	closedir(dd);
}

/* The legacy VGA text console (vgacon, "VGA+") uses the boot display's legacy ranges. */
static void scan_vtconsoles(const char *sysroot, struct gs_display *d)
{
	char dir[GS_PATH_MAX], p[GS_PATH_MAX], name[128];
	struct dirent *de;
	unsigned int bind;
	DIR *dd;

	if (gs_pathf(dir, sizeof(dir), "%s/class/vtconsole", sysroot)) {
		display_refuse(d, "sysfs path too long");
		return;
	}
	dd = opendir(dir);
	if (!dd) {
		if (errno != ENOENT)
			display_refuse(d, "%s cannot be read", dir);
		return;
	}
	while ((de = readdir(dd))) {
		if (!name_with_number(de->d_name, "vtcon"))
			continue;
		if (gs_pathf(p, sizeof(p), "%s/%s/name", dir, de->d_name) || gs_read_attr(p, name, sizeof(name)) ||
		    gs_pathf(p, sizeof(p), "%s/%s/bind", dir, de->d_name) || gs_read_uint(p, &bind)) {
			display_refuse(d, "vtconsole %.64s cannot be read", de->d_name);
			continue;
		}
		if (bind && strstr(name, "VGA+"))
			display_refuse(d, "the legacy VGA text console (%s) is bound", name);
	}
	closedir(dd);
}

int gs_display_inspect(const char *sysroot, const char *procroot, const struct gs_card *c, struct gs_display *d)
{
	static const char *const fb_names[] = { "fb", NULL };
	static const char *const drm_names[] = { "card", "renderD", NULL };
	char p[GS_PATH_MAX], pfreal[PATH_MAX], why[256];
	char *text;
	size_t len;

	memset(d, 0, sizeof(*d));
	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s", sysroot, c->pf.str) || !realpath(p, pfreal)) {
		display_refuse(d, "%s does not resolve", c->pf.str);
		return 0;
	}
	scan_firmware_fb(sysroot, pfreal, d);
	scan_class(sysroot, "graphics", fb_names, pfreal, d);
	scan_class(sysroot, "drm", drm_names, pfreal, d);
	scan_vtconsoles(sysroot, d);
	if (gs_display_fw_state(d) == GS_FW_RELEASED && d->nconsumers)
		display_refuse(d, "%s is unbound, but %u fb/DRM device(s) of it remain", d->fw, d->nconsumers);
	if (!procroot)
		return 0;
	d->proc_checked = 1;
	if (gs_pathf(p, sizeof(p), "%s/iomem", procroot) || gs_read_whole(p, &text, &len, 1 << 22)) {
		display_refuse(d, "%s/iomem cannot be read", procroot);
	} else {
		if (gs_iomem_check(text, c->pf.str, gs_display_fw_state(d), why, sizeof(why)))
			display_refuse(d, "%s", why);
		free(text);
	}
	if (gs_pathf(p, sizeof(p), "%s/consoles", procroot) || gs_read_whole(p, &text, &len, 1 << 20)) {
		display_refuse(d, "%s/consoles cannot be read", procroot);
	} else {
		if (gs_consoles_check(text, why, sizeof(why)))
			display_refuse(d, "%s", why);
		free(text);
	}
	return 0;
}

int gs_iomem_check(const char *iomem, const char *pf, enum gs_fw_state fw, char *why, size_t len)
{
	const char *line = iomem;
	long bar = -1, bootfb = -1; /* indentation of the PF BAR and of BOOTFB being walked */
	unsigned int nboot = 0, lineno = 0;

	if (!iomem || !*iomem)
		return snprintf(why, len, "/proc/iomem is empty"), -1;
	while (*line) {
		const char *nl = strchr(line, '\n'), *sep, *name;
		size_t n = nl ? (size_t)(nl - line) : strlen(line), namelen;
		long ind = 0;

		lineno++;
		while ((size_t)ind < n && line[ind] == ' ')
			ind++;
		sep = memmem(line, n, " : ", 3);
		if (!sep)
			return snprintf(why, len, "/proc/iomem line %u cannot be parsed", lineno), -1;
		name = sep + 3;
		namelen = n - (size_t)(name - line);
		if (bootfb >= 0 && ind > bootfb) {
			if (fw != GS_FW_BOUND)
				return snprintf(why, len, "'%.*s' still holds memory inside BOOTFB of %s", (int)namelen,
						name, pf), -1;
		} else if (bar >= 0 && ind > bar) {
			bootfb = -1;
			if (namelen == 6 && memcmp(name, "BOOTFB", 6) == 0 && fw != GS_FW_NONE && !nboot) {
				nboot++;
				bootfb = ind;
			} else {
				return snprintf(why, len, "'%.*s' uses memory inside a BAR of %s", (int)namelen, name,
						pf), -1;
			}
		} else {
			bar = bootfb = -1;
			if (namelen == strlen(pf) && memcmp(name, pf, namelen) == 0) {
				/*
				 * r_show() indents at most 2 * MAX_IORES_LEVEL spaces (5 levels in older
				 * kernels): from there on children print level with their parent.
				 */
				if (ind >= 10)
					return snprintf(why, len, "a BAR of %s is nested too deep for /proc/iomem to show what uses it",
							pf), -1;
				bar = ind;
			}
		}
		if (!nl)
			break;
		line = nl + 1;
	}
	return 0;
}

int gs_consoles_check(const char *consoles, char *why, size_t len)
{
	const char *line = consoles;

	while (line && *line) {
		if (strncmp(line, "efifb", 5) == 0)
			return snprintf(why, len, "an early EFI framebuffer console (efifb, earlycon) is registered"), -1;
		line = strchr(line, '\n');
		if (line)
			line++;
	}
	return 0;
}

/* ---- this boot's preparation record --------------------------------------- */

int gs_prepared_read(const char *run_dir, const struct gs_addr *pf, struct gs_prepared *rec)
{
	char p[GS_PATH_MAX], *data, *line, *save = NULL;
	size_t len;

	memset(rec, 0, sizeof(*rec));
	if (gs_pathf(p, sizeof(p), "%s/" GS_PREPARED_NAME "/%s", run_dir, pf->str) ||
	    gs_read_whole(p, &data, &len, 4096))
		return 0;
	rec->present = 1;
	for (line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (strncmp(line, "boot_id=", 8) == 0)
			gs_copy(rec->boot_id, sizeof(rec->boot_id), line + 8);
		else if (strncmp(line, "console_release=", 16) == 0)
			gs_copy(rec->console_release, sizeof(rec->console_release), line + 16);
	}
	free(data);
	return 0;
}

int gs_check_enable(const struct gs_card *c, const struct gs_config *cfg, const struct gs_prepared *rec,
		    const char *boot_id, char *why, size_t len)
{
	if (!rec->present || !rec->boot_id[0] || !boot_id || !boot_id[0] || strcmp(rec->boot_id, boot_id) != 0)
		return snprintf(why, len, "%s was not prepared this boot (run `geforce-sriov prepare` before NVIDIA binds it)",
				c->pf.str), -1;
	if (!c->present || !c->entry)
		return snprintf(why, len, "%s is %s", c->pf.str, !c->present ? "missing" : "not in the device table"), -1;
	/* boot_vga can read differently after the rescan moved the BARs: the record decides as well. */
	if (c->boot_vga != 1 && !rec->console_release[0])
		return 0;
	if (!gs_config_grants_release(cfg, c->pf.str))
		return snprintf(why, len, "%s is the boot display device; boot_display_release in the config does not grant its console release",
				c->pf.str), -1;
	if (!rec->console_release[0])
		return snprintf(why, len, "%s is the boot display device and was not prepared with `geforce-sriov prepare --release-console=%s` this boot",
				c->pf.str, c->pf.str), -1;
	return 0;
}

int gs_check_presented(const struct gs_card *c, const struct gs_config *cfg, char *why, size_t len)
{
	unsigned int vfs = gs_config_vfs(cfg);

	if (!c->has_sriov)
		return snprintf(why, len, "%s presents no SR-IOV (sriov_totalvfs absent)", c->pf.str), -1;
	if (c->totalvfs < 1 || c->totalvfs > vfs)
		return snprintf(why, len, "%s presents sriov_totalvfs=%u, outside 1..%u (the configured vfs); the count is fixed at module load, so a changed vfs needs the next boot",
				c->pf.str, c->totalvfs, vfs), -1;
	return 0;
}
