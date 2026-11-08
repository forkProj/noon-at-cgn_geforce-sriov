// SPDX-License-Identifier: MIT
/*
 * devices, doctor, status, check (read-only, any sysfs root) and prepare, activate
 * (live system; prepare and activate also against a fixture host in the tests).
 *
 * The prepare sequence follows these rules: one guarded attempt, refuse the
 * boot display unless the config grants its console release and the operator
 * consents by hand, refuse shared buses, check every function for a bound
 * driver, hold the bridge awake, fail loudly, and never retry a rescan when a
 * PF is lost: recovery is a cold power cycle, never a repeated rescan.
 */
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "gs_hook.h"
#include "gs_lifecycle.h"

#define LIVEPATCH_WAIT_MS 15000
/* How long prepare waits for a runtime-suspended PF to wake after power/control=on. */
#define PF_WAKE_MS 5000u
#define BRIDGE_WAKE_MS 2000
#define REMOVE_WAIT_MS 5000
#define RETURN_WAIT_MS 10000
#define SRIOV_ATTR_WAIT_MS 3000
#define FN_RETURN_WAIT_MS 3000
#define VF_BIND_WAIT_MS 15000
#define POLL_MS 100
#define RELEASE_WAIT_MS 2000
#define MGR_ACTIVE_WAIT_MS 10000
#define PF_REBIND_WAIT_MS 10000
#define VF_TYPES_WAIT_MS 15000
#define VF_ACTIVE_WAIT_MS 1000
/* activate gives nvidia-vgpu-mgr 3 s after its start before the PF re-probe. */
#define MGR_SETTLE_MS 3000

static const char *const recovery_msg =
	"Recovery: do not rescan again or restart in a loop. Power the machine off and on, run\n"
	"`geforce-sriov doctor`, then `geforce-sriov prepare --clear-guard`. Until then the boot unit\n"
	"refuses to run.";

/* What may fix an unassigned BAR; prepare never resizes the bridge windows above the card. */
#define BAR_REMEDY "try enabling Above 4G decoding in the firmware, or boot with pci=realloc"
/* Doctor's note for every selected Ada card: what the package does not provide. */
#define ADA_STACK_NOTE \
	"Ada needs your own NVIDIA 580.159.01 proprietary host stack in CPU-RM mode (NVreg_EnableGpuFirmware=0): " \
	"nvidia.ko rebuilt with the scheduler, Ada probe-gate and VF-count changes, and the patched plugin copy of " \
	"libnvidia-vgpu.so, SHA-256 " PATCHED_PLUGIN_SHA " (docs/driver-patch.md#revisions). `install` prepares both " \
	"with geforce-sriov-driver unless --driver=skip is given"
/* NVIDIA's unchanged plugin of GS_NVIDIA_VERSION (docs/ada.md). */
#define ADA_PLUGIN_STOCK_SHA "7e52b76dc7ca8490e7ffbc1fd724f5db07f3791a3a2ec07f2d595bf05abdcaa7"
/* The earlier Ada-only (H5) plugin copy: refused, the patched plugin copy replaces it. */
#define ADA_PLUGIN_H5_SHA "097f571588f3a232265103b73e38ba111ddb44ef329ac720562cff71e6170d37"
/* The earlier copy with the first plugin patch only (plugin-mixed): refused, `install` replaces it. */
#define ADA_PLUGIN_MIXED_SHA "7ddfad458bbfdb0c4d6902a9e045bd15c84f63aea19314f3d3a60967224b73c6"
/* The patched plugin copy (plugin-mixed and plugin-div4), for Ada and Ampere cards on one host: the only one
 * activate accepts. */
#define PATCHED_PLUGIN_SHA "aa4fdeb5288b5e56e7d4972466813d78e10cfb428fcf62fb53b84fec718cf07c"

static int cmdline_noari(const char *procroot);
static const char *plugin_link(void);
static int wait_attr_eq(const char *path, const char *want, unsigned int ms);
static const struct gs_display *granted_display(const struct gs_config *cfg, const char *sys, const char *proc,
						const struct gs_card *c, struct gs_display *buf);

static const char *sw(int v)
{
	return v == GS_SW_OFF ? "off" : "on";
}

/*
 * Is a power/runtime_status value a runtime-suspended device? A hybrid laptop's GPU then may sit in
 * D3cold, where its config space reads all-ones.
 */
static int runtime_suspended(const char *status)
{
	return strcmp(status, "suspended") == 0 || strcmp(status, "suspending") == 0;
}

/* <pf>/power/runtime_status into buf: 0, or -1 when it is absent or unreadable (buf ""). */
static int pf_runtime_status(const char *sysroot, const struct gs_addr *pf, char *buf, size_t len)
{
	char p[GS_PATH_MAX];

	buf[0] = '\0';
	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/power/runtime_status", sysroot, pf->str) ||
	    gs_read_attr(p, buf, len)) {
		buf[0] = '\0';
		return -1;
	}
	return 0;
}

int gs_cmd_devices(void)
{
	unsigned int i;

	printf("geforce-sriov %s device table (exact IDs only; %u VFs per GPU by default)\n", GS_VERSION,
	       GS_DEFAULT_VFS);
	printf("Every listed card is supported. nvidia-vgpud sees each generation as one card:\n");
	for (i = 0; i < GS_GENERATION_COUNT; i++)
		printf("  %-7s presented as %s 10de:%04x, default vGPU type %u\n", gs_generation_name(i),
		       gs_targets[i].name, gs_targets[i].device, gs_vgpu_type_default(i));
	printf("\n%-9s %-7s %-6s %s\n", "DEVICE", "GEN", "DIE", "NAME");
	for (i = 0; i < GS_DEVICE_COUNT; i++) {
		const struct gs_device *e = &gs_devices[i];

		printf("10de:%04x %-7s %-6s %s\n", e->device, gs_generation_name(e->generation), e->die, e->name);
	}
	printf("\n`install --vfs N` (vfs=N in the config) sets the VF count: 1 up to the card's hardware default, or up\n"
	       "to %u on an Ada card that reports 1. doctor shows both counts. ARI decoding in the GPU does not work,\n"
	       "so above %u VFs the module refuses to load when the card's VF layout check fails.\n", GS_DEFAULT_VFS,
	       GS_DEFAULT_VFS);
	printf("geforce-sriov does nothing for an ID not in this table. A presented capability alone is not a working vGPU.\n");
	return GS_EXIT_OK;
}

/* ---- shared readers ---------------------------------------------------- */

struct module_state {
	int loaded;
	char ari[8], msix[8], vfs[8], bdfs[256];
	int lp_present;
	unsigned int lp_enabled, lp_transition;
	int lp_ok;
};

static void read_module_state(const char *sysroot, struct module_state *m)
{
	char p[GS_PATH_MAX];

	memset(m, 0, sizeof(*m));
	if (!gs_pathf(p, sizeof(p), "%s/module/%s", sysroot, GS_MODULE_NAME))
		m->loaded = gs_exists(p);
	if (!gs_pathf(p, sizeof(p), "%s/module/%s/parameters/ari", sysroot, GS_MODULE_NAME))
		gs_read_attr(p, m->ari, sizeof(m->ari));
	if (!gs_pathf(p, sizeof(p), "%s/module/%s/parameters/msix", sysroot, GS_MODULE_NAME))
		gs_read_attr(p, m->msix, sizeof(m->msix));
	if (!gs_pathf(p, sizeof(p), "%s/module/%s/parameters/vfs", sysroot, GS_MODULE_NAME))
		gs_read_attr(p, m->vfs, sizeof(m->vfs));
	if (!gs_pathf(p, sizeof(p), "%s/module/%s/parameters/bdfs", sysroot, GS_MODULE_NAME))
		gs_read_attr(p, m->bdfs, sizeof(m->bdfs));
	if (!gs_pathf(p, sizeof(p), "%s/kernel/livepatch/%s", sysroot, GS_MODULE_NAME))
		m->lp_present = gs_exists(p);
	if (m->lp_present) {
		int a = gs_pathf(p, sizeof(p), "%s/kernel/livepatch/%s/enabled", sysroot, GS_MODULE_NAME) ||
			gs_read_uint(p, &m->lp_enabled);
		int b = gs_pathf(p, sizeof(p), "%s/kernel/livepatch/%s/transition", sysroot, GS_MODULE_NAME) ||
			gs_read_uint(p, &m->lp_transition);

		m->lp_ok = !a && !b && m->lp_enabled == 1 && m->lp_transition == 0;
	}
}

/* The VF count the loaded module uses; a module without the vfs parameter uses GS_DEFAULT_VFS. */
static unsigned int module_vfs(const struct module_state *m)
{
	unsigned int v;

	return m->vfs[0] && !gs_parse_vfs(m->vfs, &v) ? v : GS_DEFAULT_VFS;
}

static int is_live_root(const char *sysroot)
{
	return strcmp(sysroot, GS_LIVE_SYSFS) == 0;
}

/* NVIDIA physical functions (no physfn) under sysroot, display class only when asked; sorted. */
static unsigned int scan_nvidia_pfs(const char *sysroot, struct gs_addr *out, unsigned int max, int display_only)
{
	char p[GS_PATH_MAX];
	unsigned int n = 0;
	struct dirent *de;
	DIR *d;

	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices", sysroot))
		return 0;
	d = opendir(p);
	if (!d)
		return 0;
	while ((de = readdir(d)) && n < max) {
		struct gs_addr a;
		unsigned int vendor = 0, class_code = 0;
		char q[GS_PATH_MAX];

		if (gs_addr_parse(de->d_name, &a))
			continue;
		if (gs_pathf(q, sizeof(q), "%s/%s/vendor", p, a.str) || gs_read_hex(q, &vendor) ||
		    vendor != GS_VENDOR_NVIDIA)
			continue;
		if (display_only && (gs_pathf(q, sizeof(q), "%s/%s/class", p, a.str) || gs_read_hex(q, &class_code) ||
				     (class_code >> 16) != 0x03))
			continue;
		if (gs_pathf(q, sizeof(q), "%s/%s/physfn", p, a.str) || gs_exists(q))
			continue;
		out[n++] = a;
	}
	closedir(d);
	/* readdir order is unspecified: sort for stable output */
	for (unsigned int i = 1; i < n; i++)
		for (unsigned int j = i; j > 0 && strcmp(out[j - 1].str, out[j].str) > 0; j--) {
			struct gs_addr t = out[j];

			out[j] = out[j - 1];
			out[j - 1] = t;
		}
	return n;
}

/*
 * Config for read-only commands: an explicit --config, else $GEFORCE_SRIOV_CONFIG,
 * else the system file only on the live root (a fixture run never reads host config).
 */
static int diag_config(const struct gs_diag_opts *o, struct gs_config *cfg, const char **used, char *err,
		       size_t errlen)
{
	const char *path = o->config_path;
	const char *env = getenv(GS_CONFIG_ENV);

	if (!path && env && env[0])
		path = env;
	if (!path && is_live_root(o->sysroot ? o->sysroot : GS_LIVE_SYSFS))
		path = GS_CONFIG_PATH;
	*used = path;
	if (!path) {
		gs_config_defaults(cfg);
		return 0;
	}
	return gs_config_load(path, cfg, err, errlen);
}

static unsigned int selected_devices(const struct gs_diag_opts *o, const struct gs_config *cfg,
				     const char *sysroot, struct gs_addr *out, const char **source)
{
	unsigned int i;

	if (o->ndevices) {
		for (i = 0; i < o->ndevices; i++)
			out[i] = o->devices[i];
		*source = "--device";
		return o->ndevices;
	}
	if (cfg->nbdfs) {
		for (i = 0; i < cfg->nbdfs; i++)
			out[i] = cfg->bdfs[i];
		*source = "config bdfs";
		return cfg->nbdfs;
	}
	*source = "all NVIDIA display functions";
	return scan_nvidia_pfs(sysroot, out, GS_CFG_MAX_BDFS, 1);
}

static int addr_listed(const struct gs_addr *list, unsigned int n, const char *s)
{
	for (unsigned int i = 0; i < n; i++)
		if (strcmp(list[i].str, s) == 0)
			return 1;
	return 0;
}

/* The same addresses in any order. */
static int same_selection(const struct gs_addr *a, unsigned int na, const struct gs_addr *b, unsigned int nb)
{
	if (na != nb)
		return 0;
	for (unsigned int i = 0; i < na; i++)
		if (!addr_listed(b, nb, a[i].str))
			return 0;
	return 1;
}

/* Does the loaded module's bdfs parameter name pf, so that a presented capability is its own? */
static int module_selects(const struct module_state *m, const struct gs_addr *pf)
{
	struct gs_addr list[GS_CFG_MAX_BDFS];
	unsigned int n = 0;
	char err[256];

	return m->loaded && gs_parse_addr_list("bdfs", m->bdfs, list, &n, err, sizeof(err)) == 0 &&
	       addr_listed(list, n, pf->str);
}

/* ---- laptop power guards ------------------------------------------------------------------------ */

/* What a refusal of a GPU that does not come back from a power-off suggests. */
#define WAKE_REMEDY \
	"Switch the laptop to its discrete GPU (dGPU or discrete mode in the firmware setup), or stop whatever powers " \
	"the GPU off (for example bbswitch), then run prepare again"

/* The table entry of pf by its device attribute (the kernel's copy of the ID, readable while the card is off). */
static const struct gs_device *pf_entry(const char *sysroot, const struct gs_addr *pf, unsigned int *device)
{
	char p[GS_PATH_MAX];
	unsigned int d = 0;

	*device = 0;
	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/device", sysroot, pf->str) || gs_read_hex(p, &d))
		return NULL;
	*device = d;
	return gs_device_lookup(d);
}

unsigned int gs_laptop_gpus(const char *sys, const struct gs_addr *list, unsigned int n, struct gs_addr *out)
{
	unsigned int k = 0, device;

	for (unsigned int i = 0; i < n; i++) {
		const struct gs_device *e = pf_entry(sys, &list[i], &device);

		if (e && e->laptop)
			out[k++] = list[i];
	}
	return k;
}

int gs_d3cold_usage(void)
{
	fprintf(stderr, "geforce-sriov: --disable-d3cold requires a selected laptop GPU. Nothing changed.\n"
		"Try 'geforce-sriov --help'.\n");
	return GS_EXIT_USAGE;
}

/* s at the end of the string in buf (len bytes), cut short when it does not fit. */
static void str_add(char *buf, size_t len, const char *s)
{
	size_t n = strlen(buf);

	snprintf(buf + n, len - n, "%s", s);
}

void gs_d3cold_why(char *buf, size_t len, const struct gs_addr *laptops, unsigned int n, const struct gs_addr *sel,
		   unsigned int nsel, const struct gs_addr *rel, unsigned int nrel, const char *indent)
{
	unsigned int i;

	buf[0] = '\0';
	str_add(buf, len, n > 1 ? "no D3cold consent in the config for laptop GPUs " :
				  "no D3cold consent in the config for laptop GPU ");
	for (i = 0; i < n; i++) {
		if (i)
			str_add(buf, len, ", ");
		str_add(buf, len, laptops[i].str);
	}
	str_add(buf, len, ".\n");
	str_add(buf, len, indent);
	str_add(buf, len, "Run geforce-sriov setup, or:\n");
	str_add(buf, len, indent);
	/* --device replaces the selection and its grants: name them all, so the install keeps them. */
	str_add(buf, len, "  geforce-sriov install");
	for (i = 0; i < nsel; i++) {
		str_add(buf, len, " --device ");
		str_add(buf, len, sel[i].str);
	}
	for (i = 0; i < nrel; i++) {
		str_add(buf, len, " --boot-display-release ");
		str_add(buf, len, rel[i].str);
	}
	str_add(buf, len, " --disable-d3cold\n");
	str_add(buf, len, indent);
	str_add(buf, len, "Why: docs/laptops.md#d3cold.");
}

/* n bytes of <dir>/config from off into buf: 0, or -1 when fewer can be read (unprivileged reads stop at 64). */
static int config_read(const char *dir, unsigned int off, void *buf, size_t n)
{
	char p[GS_PATH_MAX];
	ssize_t got;
	int fd;

	if (gs_pathf(p, sizeof(p), "%s/config", dir) || (fd = open(p, O_RDONLY | O_CLOEXEC)) < 0)
		return -1;
	got = pread(fd, buf, n, (off_t)off);
	close(fd);
	return got == (ssize_t)n ? 0 : -1;
}

static int config_dword(const char *dir, unsigned int off, unsigned int *out)
{
	unsigned char b[4];

	if (config_read(dir, off, b, sizeof(b)))
		return -1;
	*out = (unsigned int)b[0] | (unsigned int)b[1] << 8 | (unsigned int)b[2] << 16 | (unsigned int)b[3] << 24;
	return 0;
}

/*
 * The config-read gate. A sysfs config read powers a D3cold device up first, so what it returns is the card's own
 * answer, which runtime_status is not: the first 256 bytes must read vendor 10de with device, and the control
 * register of the PCI power management capability must say D0. 0, or -1 with what was read in why.
 */
static int config_gate(const char *dir, unsigned int device, char *why, size_t len)
{
	unsigned char cfg[256];
	unsigned int pos, guard = 0;

	if (config_read(dir, 0, cfg, sizeof(cfg))) {
		snprintf(why, len, "its config space cannot be read");
		return -1;
	}
	if (((unsigned int)cfg[0] | (unsigned int)cfg[1] << 8) != GS_VENDOR_NVIDIA ||
	    ((unsigned int)cfg[2] | (unsigned int)cfg[3] << 8) != device) {
		snprintf(why, len, "its config space reads %02x%02x:%02x%02x, not 10de:%04x", cfg[1], cfg[0], cfg[3], cfg[2],
			 device);
		return -1;
	}
	pos = cfg[0x06] & 0x10 ? cfg[0x34] & 0xfcu : 0;
	while (pos >= 0x40 && pos < 0xf8 && cfg[pos] != 0x01 && guard++ < 48)
		pos = cfg[pos + 1] & 0xfcu;
	if (pos < 0x40 || pos >= 0xf8 || cfg[pos] != 0x01) {
		snprintf(why, len, "its config space shows no power management capability");
		return -1;
	}
	if (cfg[pos + 4] & 3) {
		snprintf(why, len, "its power management control register says D%u, not D0", cfg[pos + 4] & 3u);
		return -1;
	}
	return 0;
}

/* The sysfs directory of the PCI bridge above pf into out: 0, or -1 when its parent is not a PCI function. */
static int pf_bridge_dir(const char *sysroot, const struct gs_addr *pf, char *out, size_t len)
{
	char p[GS_PATH_MAX], real[PATH_MAX], *slash;
	struct gs_addr a;

	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s", sysroot, pf->str) || !realpath(p, real) ||
	    !(slash = strrchr(real, '/')))
		return -1;
	*slash = '\0';
	slash = strrchr(real, '/');
	if (!slash || gs_addr_parse(slash + 1, &a))
		return -1;
	gs_copy(out, len, real);
	return 0;
}

/*
 * on to the power/control of the bridge at dir, then up to BRIDGE_WAKE_MS for its runtime_status to say active
 * ("unsupported" means runtime PM is disabled for it, so it is not runtime-suspended). 0, the write's -errno, or
 * 1 when it did not say so.
 */
static int bridge_awake(int (*write_attr)(const char *, const char *), const char *dir)
{
	char p[GS_PATH_MAX];
	int r;

	if (gs_pathf(p, sizeof(p), "%s/power/control", dir))
		return -ENAMETOOLONG;
	if ((r = write_attr(p, "on")))
		return r;
	if (gs_pathf(p, sizeof(p), "%s/power/runtime_status", dir))
		return -ENAMETOOLONG;
	return wait_attr_eq(p, "unsupported", 0) && wait_attr_eq(p, "active", BRIDGE_WAKE_MS) ? 1 : 0;
}

/* What wake_card() changed on one PF, so that it can be given back. */
struct woken {
	struct gs_addr pf;
	int laptop;
	int wrote_control, wrote_d3cold, wrote_bridge;
	char dir[GS_PATH_MAX];    /* the PF's sysfs directory */
	char control[16];         /* its power/control before */
	char d3cold[8];           /* its d3cold_allowed before */
	char bridge[GS_PATH_MAX]; /* the bridge's sysfs directory; "" without one */
	char bridge_control[16];  /* the bridge's power/control before */
	char done[320];           /* one line on what was done */
};

/* Write the old value back to path, the attribute what of who; nothing when the old value is unknown. */
static void give_back(int (*write_attr)(const char *, const char *), const char *path, const char *old,
		      const char *who, const char *what)
{
	if (!old[0])
		return;
	if (write_attr(path, old))
		fprintf(stderr, "geforce-sriov: %s: restoring %s=%s failed\n", who, what, old);
	else
		printf("%s: %s restored to %s\n", who, what, old);
}

/* Give back what wake_card() wrote on each of the n PFs (none of them was prepared). */
static void unwake(int (*write_attr)(const char *, const char *), const struct woken *w, unsigned int n)
{
	char p[GS_PATH_MAX];

	for (unsigned int i = 0; i < n; i++) {
		const char *slash = strrchr(w[i].bridge, '/');

		if (w[i].wrote_d3cold && !gs_pathf(p, sizeof(p), "%s/d3cold_allowed", w[i].dir))
			give_back(write_attr, p, w[i].d3cold, w[i].pf.str, "d3cold_allowed");
		if (w[i].wrote_control && !gs_pathf(p, sizeof(p), "%s/power/control", w[i].dir))
			give_back(write_attr, p, w[i].control, w[i].pf.str, "power/control");
		if (w[i].wrote_bridge && strcmp(w[i].bridge_control, "on") != 0 && slash &&
		    !gs_pathf(p, sizeof(p), "%s/power/control", w[i].bridge))
			give_back(write_attr, p, w[i].bridge_control, slash + 1, "bridge power/control");
	}
}

/*
 * Before the first config-space read of pf: a runtime-suspended PF (power/runtime_status suspended or suspending)
 * and every laptop row are brought up and checked by the config-read gate (config_gate()). In this order: a laptop
 * PF whose status says it is up must already pass the gate (nothing is written otherwise); the bridge above is
 * woken (bridge_awake()); the PF's power/control goes to on; a laptop PF's d3cold_allowed goes to 0, so it cannot
 * drop to D3cold (a cold reset that loses its SR-IOV state and every vGPU); then up to PF_WAKE_MS for the status to
 * leave suspend and the gate to pass. A laptop's bridge stays on for the session; a desktop card's bridge gets its
 * old setting back. A desktop PF that is not suspended is left alone: nothing is read, written or printed.
 * 1 done (*w says what, w->done in one line); 0 nothing to do; -1 a write failed, -2 the card did not come back.
 * On -1 and -2 everything written is given back and why says what happened.
 */
static int wake_card(const char *sysroot, int (*write_attr)(const char *, const char *), const struct gs_addr *pf,
		     struct woken *w, char *why, size_t len)
{
	char st[GS_PATH_MAX], p[GS_PATH_MAX], was[32], now[32] = "", gate[96] = "";
	unsigned int device = 0;
	const struct gs_device *e = pf_entry(sysroot, pf, &device);
	const char *bname;
	int r, suspended;

	memset(w, 0, sizeof(*w));
	w->pf = *pf;
	w->laptop = e && e->laptop;
	pf_runtime_status(sysroot, pf, was, sizeof(was));
	suspended = runtime_suspended(was);
	if ((!suspended && !w->laptop) || gs_pathf(w->dir, sizeof(w->dir), "%s/bus/pci/devices/%s", sysroot, pf->str) ||
	    gs_pathf(st, sizeof(st), "%s/power/runtime_status", w->dir))
		return 0;
	if (!suspended && config_gate(w->dir, device, gate, sizeof(gate))) {
		snprintf(why, len, "%s does not answer although its runtime_status is %s: %s (a laptop GPU that did not come "
			 "back from D3cold)", pf->str, was[0] ? was : "absent", gate);
		return -2;
	}
	if (!pf_bridge_dir(sysroot, pf, w->bridge, sizeof(w->bridge))) {
		bname = strrchr(w->bridge, '/') + 1;
		if (gs_pathf(p, sizeof(p), "%s/power/control", w->bridge) ||
		    gs_read_attr(p, w->bridge_control, sizeof(w->bridge_control)))
			w->bridge_control[0] = '\0';
		r = bridge_awake(write_attr, w->bridge);
		if (r < 0) {
			snprintf(why, len, "%s: writing on to the power/control of its bridge %s failed: %s", pf->str, bname,
				 strerror(-r));
			return -1;
		}
		w->wrote_bridge = 1;
		if (r > 0) {
			unwake(write_attr, w, 1);
			snprintf(why, len, "%s: its bridge %s did not report runtime_status=active", pf->str, bname);
			return -2;
		}
	}
	if (gs_pathf(p, sizeof(p), "%s/power/control", w->dir) || gs_read_attr(p, w->control, sizeof(w->control)))
		w->control[0] = '\0';
	if (strcmp(w->control, "on") != 0) {
		if ((r = write_attr(p, "on"))) {
			unwake(write_attr, w, 1);
			snprintf(why, len, "%s: writing on to its power/control failed: %s", pf->str, strerror(-r));
			return -1;
		}
		w->wrote_control = 1;
	}
	if (w->laptop) {
		if (gs_pathf(p, sizeof(p), "%s/d3cold_allowed", w->dir) || gs_read_attr(p, w->d3cold, sizeof(w->d3cold)))
			w->d3cold[0] = '\0';
		if (strcmp(w->d3cold, "0") != 0) {
			if ((r = write_attr(p, "0"))) {
				unwake(write_attr, w, 1);
				snprintf(why, len, "%s: writing 0 to its d3cold_allowed failed: %s", pf->str, strerror(-r));
				return -1;
			}
			w->wrote_d3cold = 1;
		}
	}
	for (unsigned int t = 0;; t += POLL_MS) {
		if (gs_read_attr(st, now, sizeof(now)))
			now[0] = '\0';
		if (!config_gate(w->dir, device, gate, sizeof(gate)) && !runtime_suspended(now))
			break;
		if (t >= PF_WAKE_MS) {
			unwake(write_attr, w, 1);
			snprintf(why, len, "%s is powered down (runtime_status was %s) and did not come back within %u s of "
				 "power/control=on: %s (runtime_status %s)", pf->str, was[0] ? was : "absent", PF_WAKE_MS / 1000,
				 gate, now[0] ? now : "unreadable");
			return -2;
		}
		gs_sleep_ms(POLL_MS);
	}
	bname = w->bridge[0] ? strrchr(w->bridge, '/') + 1 : "";
	if (w->laptop) {
		snprintf(w->done, sizeof(w->done), "laptop GPU: %s%.32s%spower/control %s%s, d3cold_allowed %s%s, config "
			 "reads 10de:%04x in D0", bname[0] ? "bridge " : "", bname, bname[0] ? " power/control on (kept on), " : "",
			 w->wrote_control ? w->control : "", w->wrote_control ? " -> on" : "on",
			 w->wrote_d3cold ? w->d3cold : "", w->wrote_d3cold ? " -> 0" : "0", device);
		return 1;
	}
	if (w->wrote_bridge && strcmp(w->bridge_control, "on") != 0 && !gs_pathf(p, sizeof(p), "%s/power/control", w->bridge))
		give_back(write_attr, p, w->bridge_control, bname, "bridge power/control");
	w->wrote_bridge = 0;
	snprintf(w->done, sizeof(w->done), "was runtime_status=%s: power/control %s -> on, config reads 10de:%04x in D0", was,
		 w->control[0] ? w->control : "?", device);
	return 1;
}

/*
 * Signs in the config space of a prepared laptop GPU that it was powered off (D3cold, a cold reset) since: VF Enable
 * clear while the kernel still counts VFs, and MSI-X hidden again although prepare recorded it linked (run_dir's
 * record: a record without msix_linked, as 0.2.0 wrote, or no run_dir, says nothing). Reads need root (offsets
 * above 64); what cannot be read says nothing. Each finding goes to out[n]; returns how many.
 */
static unsigned int laptop_lost(const char *sysroot, const char *run_dir, const struct gs_card *c,
				const struct module_state *m, char out[2][256])
{
	struct gs_prepared rec;
	char dir[GS_PATH_MAX];
	unsigned int v, n = 0;

	if (!c->entry || !c->entry->laptop || gs_pathf(dir, sizeof(dir), "%s/bus/pci/devices/%s", sysroot, c->pf.str))
		return 0;
	if (c->has_sriov && c->numvfs > 0 && !config_dword(dir, c->entry->sriov_pos + 8u, &v) && !(v & 1))
		snprintf(out[n++], 256, "%s: the GPU lost its SR-IOV state (D3cold reset); power the machine off and on, then "
			 "run setup again", c->pf.str);
	if (run_dir && !gs_prepared_read(run_dir, &c->pf, &rec) && rec.msix_linked == 1 && c->has_sriov &&
	    strcmp(m->msix, "Y") == 0 && module_selects(m, &c->pf) && !config_dword(dir, c->entry->msix_hide_pos, &v) &&
	    v != 0xffffffffu && (v >> GS_MSIX_HIDE_BIT & 1))
		snprintf(out[n++], 256, "%s: MSI-X was hidden again (the GPU was power cycled); power the machine off and on, "
			 "then run setup again", c->pf.str);
	return n;
}

/*
 * Members of the card's IOMMU group other than its own functions (its domain:bus:slot): whether
 * its immediate bridge is one, how many others there are and the first of them. -1 when the group
 * is unknown or cannot be listed.
 */
static int iommu_peers(const char *sysroot, const struct gs_card *c, int *with_bridge, char *first, size_t len,
		       unsigned int *nothers)
{
	char p[GS_PATH_MAX];
	struct dirent *de;
	DIR *d;

	*with_bridge = 0;
	*nothers = 0;
	first[0] = '\0';
	if (!c->iommu_group[0] ||
	    gs_pathf(p, sizeof(p), "%s/kernel/iommu_groups/%s/devices", sysroot, c->iommu_group))
		return -1;
	d = opendir(p);
	if (!d)
		return -1;
	while ((de = readdir(d))) {
		struct gs_addr a;

		if (de->d_name[0] == '.')
			continue;
		if (gs_addr_parse(de->d_name, &a) == 0 && a.domain == c->pf.domain && a.bus == c->pf.bus &&
		    a.slot == c->pf.slot)
			continue;
		if (c->bridge_is_pci && strcmp(de->d_name, c->bridge.str) == 0) {
			*with_bridge = 1;
			continue;
		}
		if (!(*nothers)++)
			gs_copy(first, len, de->d_name);
	}
	closedir(d);
	return 0;
}

/* Every parent directory of path must be a root-owned directory nobody else can write. */
static int parents_safe(const char *path, char *why, size_t len)
{
	char dir[GS_PATH_MAX], *slash;
	struct stat st;

	if (gs_pathf(dir, sizeof(dir), "%s", path))
		return snprintf(why, len, "config path too long"), -1;
	while ((slash = strrchr(dir, '/'))) {
		if (slash == dir)
			slash[1] = '\0';
		else
			*slash = '\0';
		if (lstat(dir, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 022))
			return snprintf(why, len, "%.300s must be a directory owned by root and writable only by root", dir),
			       -1;
		if (strcmp(dir, "/") == 0)
			break;
	}
	return 0;
}

static int stat_safe(const char *path, const struct stat *st, char *why, size_t len)
{
	if (!S_ISREG(st->st_mode))
		return snprintf(why, len, "%.300s is not a regular file", path), -1;
	if (st->st_uid != 0)
		return snprintf(why, len, "%.300s is not owned by root", path), -1;
	if (st->st_mode & 022)
		return snprintf(why, len, "%.300s is group- or world-writable", path), -1;
	return parents_safe(path, why, len);
}

static int config_file_safe(const char *path, char *why, size_t len)
{
	struct stat st;

	if (lstat(path, &st) < 0)
		return snprintf(why, len, "%.300s: %s", path, strerror(errno)), -1;
	return stat_safe(path, &st, why, len);
}

static int installed_module_present(void)
{
	static const char *const dirs[] = { "updates/dkms", "extra", "updates" };
	static const char *const exts[] = { ".ko", ".ko.xz", ".ko.zst", ".ko.gz" };
	struct utsname u;
	char p[GS_PATH_MAX];

	if (uname(&u) < 0)
		return 0;
	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
		for (size_t j = 0; j < sizeof(exts) / sizeof(exts[0]); j++)
			if (!gs_pathf(p, sizeof(p), "/lib/modules/%s/%s/%s%s", u.release, dirs[i],
				      GS_MODULE_NAME, exts[j]) && gs_exists(p))
				return 1;
	return 0;
}

static int has_suffix(const char *s, const char *suffix)
{
	size_t n = strlen(s), m = strlen(suffix);

	return n > m && strcmp(s + n - m, suffix) == 0;
}

/*
 * Does this file name an nvidia module? anywhere = 0: a module-list line starting with
 * "nvidia" (modules-load.d, /etc/modules, initramfs-tools/modules); anywhere = 1: any
 * non-comment line mentioning nvidia (dracut add_drivers/force_drivers).
 */
static int file_names_nvidia(const char *path, int anywhere)
{
	char *data, *line, *save = NULL;
	size_t dlen;
	int found = 0;

	if (gs_read_whole(path, &data, &dlen, 1 << 20))
		return 0;
	for (line = strtok_r(data, "\n", &save); line && !found; line = strtok_r(NULL, "\n", &save)) {
		while (*line == ' ' || *line == '\t')
			line++;
		if (*line == '#')
			continue;
		found = anywhere ? strstr(line, "nvidia") != NULL : strncmp(line, "nvidia", 6) == 0;
	}
	free(data);
	return found;
}

/* First *.conf in dir that names nvidia, copied to where. */
static int dir_names_nvidia(const char *dir, int anywhere, char *where, size_t len)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	char p[GS_PATH_MAX];
	int found = 0;

	if (!d)
		return 0;
	while (!found && (de = readdir(d))) {
		if (!has_suffix(de->d_name, ".conf") || gs_pathf(p, sizeof(p), "%s/%s", dir, de->d_name))
			continue;
		if (file_names_nvidia(p, anywhere)) {
			gs_copy(where, len, p);
			found = 1;
		}
	}
	closedir(d);
	return found;
}

/* Boot-time loading of nvidia: 1 = modules-load list, 2 = initramfs configuration, 0 = none found. */
static int nvidia_autoload(char *where, size_t len)
{
	static const char *const load_dirs[] = { "/etc/modules-load.d", "/run/modules-load.d",
						 "/usr/lib/modules-load.d", "/lib/modules-load.d" };
	static const char *const dracut_dirs[] = { "/etc/dracut.conf.d", "/usr/lib/dracut/dracut.conf.d" };
	size_t i;

	for (i = 0; i < sizeof(load_dirs) / sizeof(load_dirs[0]); i++)
		if (dir_names_nvidia(load_dirs[i], 0, where, len))
			return 1;
	if (file_names_nvidia("/etc/modules", 0)) {
		gs_copy(where, len, "/etc/modules");
		return 1;
	}
	if (file_names_nvidia("/etc/initramfs-tools/modules", 0)) {
		gs_copy(where, len, "/etc/initramfs-tools/modules");
		return 2;
	}
	if (file_names_nvidia("/etc/dracut.conf", 1)) {
		gs_copy(where, len, "/etc/dracut.conf");
		return 2;
	}
	for (i = 0; i < sizeof(dracut_dirs) / sizeof(dracut_dirs[0]); i++)
		if (dir_names_nvidia(dracut_dirs[i], 1, where, len))
			return 2;
	return 0;
}

/* systemd's unit search path, most significant first. */
static const char *const unit_dirs[] = { "/etc/systemd/system", "/run/systemd/system", "/usr/local/lib/systemd/system",
					 "/usr/lib/systemd/system", "/lib/systemd/system" };

int gs_unit_file_present(const char *name)
{
	char p[GS_PATH_MAX];

	for (size_t i = 0; i < sizeof(unit_dirs) / sizeof(unit_dirs[0]); i++)
		if (!gs_pathf(p, sizeof(p), "%s/%s", unit_dirs[i], name) && gs_exists(p))
			return 1;
	return 0;
}

/* The x.y.z version in procroot/driver/nvidia/version; "" when NVIDIA's driver is not loaded. */
static void nvidia_version(const char *procroot, char *out, size_t len)
{
	char *data, *tok, *save = NULL, p[GS_PATH_MAX];
	size_t dlen;

	out[0] = '\0';
	if (gs_pathf(p, sizeof(p), "%s/driver/nvidia/version", procroot) || gs_read_whole(p, &data, &dlen, 1 << 16))
		return;
	for (tok = strtok_r(data, " \t\n", &save); tok; tok = strtok_r(NULL, " \t\n", &save)) {
		unsigned int a, b, c;
		char extra;

		if (sscanf(tok, "%u.%u.%u%c", &a, &b, &c, &extra) == 3) {
			snprintf(out, len, "%s", tok);
			break;
		}
	}
	free(data);
}

int gs_driver_version_line(const char *ver, char *line, size_t len)
{
	if (strcmp(ver, GS_NVIDIA_VERSION) == 0)
		return snprintf(line, len, "NVIDIA's driver %s is supported", ver), 0;
	return snprintf(line, len, "NVIDIA's driver %.63s is not supported", ver), 1;
}

/* The GNU build-id among ELF notes of the host's byte order in buf, as lowercase hex: 0, or -1. */
static int note_build_id(const unsigned char *buf, size_t size, char *out, size_t len)
{
	size_t off = 0;

	/* Notes: namesz, descsz, type, then the name and the descriptor, each padded to 4 bytes. */
	while (off + 12 <= size) {
		Elf64_Word nh[3];
		size_t name, desc;

		memcpy(nh, buf + off, sizeof(nh));
		name = off + 12;
		desc = name + ((nh[0] + 3u) & ~3u);
		if (desc > size || nh[1] > size - desc)
			break;
		if (nh[2] == NT_GNU_BUILD_ID && nh[0] == 4 && memcmp(buf + name, "GNU", 4) == 0 && nh[1] &&
		    nh[1] * 2u < len) {
			for (Elf64_Word k = 0; k < nh[1]; k++)
				snprintf(out + 2 * k, 3, "%02x", buf[desc + k]);
			return 0;
		}
		off = desc + ((nh[1] + 3u) & ~3u);
	}
	return -1;
}

/* The GNU build-id of an ELF64 file of the host's byte order (a kernel module) as lowercase hex: 0, or -1. */
static int elf_build_id(const char *path, char *out, size_t len)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	const unsigned char order = ELFDATA2LSB;
#else
	const unsigned char order = ELFDATA2MSB;
#endif
	Elf64_Ehdr eh;
	int fd = open(path, O_RDONLY | O_CLOEXEC), r = -1;

	out[0] = '\0';
	if (fd < 0)
		return -1;
	if (pread(fd, &eh, sizeof(eh), 0) != (ssize_t)sizeof(eh) || memcmp(eh.e_ident, ELFMAG, SELFMAG) ||
	    eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_ident[EI_DATA] != order || eh.e_shentsize != sizeof(Elf64_Shdr))
		goto out;
	for (unsigned int i = 0; i < eh.e_shnum && r; i++) {
		Elf64_Shdr sh;
		unsigned char *buf;

		if (pread(fd, &sh, sizeof(sh), (off_t)(eh.e_shoff + (Elf64_Off)i * sizeof(sh))) != (ssize_t)sizeof(sh))
			break;
		if (sh.sh_type != SHT_NOTE || sh.sh_size > 65536 || !(buf = malloc(sh.sh_size ? sh.sh_size : 1)))
			continue;
		if (pread(fd, buf, sh.sh_size, (off_t)sh.sh_offset) == (ssize_t)sh.sh_size)
			r = note_build_id(buf, sh.sh_size, out, len);
		free(buf);
	}
out:
	close(fd);
	return r;
}

/*
 * The sentence doctor, status and check add when the installed nvidia.ko in b is the driver step's
 * build and the loaded nvidia module (its build-id note under sysroot/module) is not: 1 with line set;
 * 0 when the installed module is not the driver step's build, the module is not loaded, its build-id
 * is unknown, or they match.
 */
static int loaded_line(const char *sysroot, const struct gs_build *b, char *line, size_t len)
{
	char p[GS_PATH_MAX], id[129], *data;
	size_t dlen;
	int r;

	if (b->state != GS_BUILD_MATCHES || gs_pathf(p, sizeof(p), "%s/module/nvidia/notes/.note.gnu.build-id", sysroot) ||
	    gs_read_whole(p, &data, &dlen, 4096))
		return 0;
	r = note_build_id((const unsigned char *)data, dlen, id, sizeof(id));
	free(data);
	if (r || strcmp(id, b->found_id) == 0)
		return 0;
	snprintf(line, len, "the running nvidia module (build-id %.12s) is not the installed one (%.12s); power the "
		 "machine off and on to load it", id, b->found_id);
	return 1;
}

enum gs_build_state gs_driver_build_read(const char *record, const char *release, const char *module,
					 struct gs_build *b)
{
	char *data, *line, *save = NULL;
	size_t dlen;

	memset(b, 0, sizeof(*b));
	gs_copy(b->record, sizeof(b->record), record);
	gs_copy(b->release, sizeof(b->release), release);
	gs_copy(b->module, sizeof(b->module), module ? module : "");
	if (b->module[0])
		elf_build_id(b->module, b->found_id, sizeof(b->found_id));
	if (gs_read_whole(record, &data, &dlen, 4096))
		return b->state = GS_BUILD_ABSENT;
	b->record_read = 1;
	for (line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (strncmp(line, "kernel=", 7) == 0)
			gs_copy(b->rec_kernel, sizeof(b->rec_kernel), line + 7);
		else if (strncmp(line, "module_sha256=", 14) == 0)
			gs_copy(b->rec_sha256, sizeof(b->rec_sha256), line + 14);
		else if (strncmp(line, "module_build_id=", 16) == 0)
			gs_copy(b->rec_id, sizeof(b->rec_id), line + 16);
	}
	free(data);
	if (strcmp(b->rec_kernel, release) != 0)
		return b->state = GS_BUILD_ABSENT;
	return b->state = b->found_id[0] && strcmp(b->found_id, b->rec_id) == 0 ? GS_BUILD_MATCHES : GS_BUILD_PRESENT;
}

/* The first line `modinfo -k release -n nvidia` prints: the module file the kernel would load. 0, or -1. */
static int modinfo_path(const char *release, char *out, size_t len)
{
	const char *const argv[] = { "modinfo", "-k", release, "-n", "nvidia", NULL };

	gs_run_capture(argv, out, len);
	out[strcspn(out, "\n")] = '\0';
	if (out[0] != '/') {
		out[0] = '\0';
		return -1;
	}
	return 0;
}

int gs_nvidia_module_path(const char *release, char *out, size_t len)
{
	if (modinfo_path(release, out, len) == 0)
		return 0;
	if (gs_pathf(out, len, "/lib/modules/%s/updates/dkms/nvidia.ko", release) || !gs_exists(out)) {
		out[0] = '\0';
		return -1;
	}
	return 0;
}

/*
 * The driver step's record against the nvidia.ko the kernel would load, found as the driver step finds it;
 * the one reading doctor, status, activate and check share. src NULL: the live host's when live, else
 * nothing is judged (a fixture without sources). 1 with b set, 0 when not judged.
 */
static int module_identity(const struct gs_build_src *src, int live, struct gs_build *b)
{
	char release[128] = "", module[GS_PATH_MAX] = "";
	struct utsname u;

	if (!src && !live)
		return 0;
	if (src && src->release)
		gs_copy(release, sizeof(release), src->release);
	else if (uname(&u) == 0)
		gs_copy(release, sizeof(release), u.release);
	if (src && src->module)
		gs_copy(module, sizeof(module), src->module);
	else if (release[0])
		gs_nvidia_module_path(release, module, sizeof(module));
	gs_driver_build_read(src && src->record ? src->record : GS_DRIVER_BUILD, release, module, b);
	return 1;
}

int gs_loaded_not_installed(const char *sysroot, const struct gs_build_src *src)
{
	char line[256];
	struct gs_build b;

	return module_identity(src, is_live_root(sysroot), &b) && loaded_line(sysroot, &b, line, sizeof(line));
}

/* Room for build_line: the module, record and release paths, both build-ids and the sentence. */
#define BUILD_LINE_MAX (3 * GS_PATH_MAX)

/*
 * The sentence doctor, activate and check print: 0 when the module is the driver step's build. A
 * mismatch names both identities, the module's and the recorded one.
 */
static int build_line(const struct gs_build *b, char *line, size_t len)
{
	char found[GS_PATH_MAX + 160];

	if (b->state == GS_BUILD_MATCHES)
		return snprintf(line, len, "nvidia.ko for %s is the driver step's build: %s, build-id %s", b->release,
				b->module, b->found_id) < 0;
	if (b->state == GS_BUILD_ABSENT) {
		if (!b->record_read)
			snprintf(found, sizeof(found), "%s is absent", b->record);
		else if (b->rec_kernel[0])
			snprintf(found, sizeof(found), "%s records kernel %s", b->record, b->rec_kernel);
		else
			snprintf(found, sizeof(found), "%s names no kernel", b->record);
		snprintf(line, len, "nvidia.ko for %s is not known to be the driver step's build: %s; rerun the driver step "
			 "for this kernel (`geforce-sriov-driver --kernel %s`)%s", b->release, found, b->release,
			 b->record_read ? "" :
			 "; if the module already is that build, `install --driver=skip` keeps it");
		return 1;
	}
	if (!b->module[0])
		snprintf(found, sizeof(found), "no nvidia.ko found (modinfo -k %s -n nvidia)", b->release);
	else if (!b->found_id[0])
		snprintf(found, sizeof(found), "%s has no readable GNU build-id", b->module);
	else
		snprintf(found, sizeof(found), "%s has build-id %s", b->module, b->found_id);
	snprintf(line, len, "nvidia.ko for %s is not the driver step's build: %s, %s records build-id %s; rerun the "
		 "driver step for this kernel (`geforce-sriov-driver --kernel %s`)", b->release, found, b->record,
		 b->rec_id[0] ? b->rec_id : "(none)", b->release);
	return 1;
}

/* ---- doctor ------------------------------------------------------------- */

struct tally {
	unsigned int fail, warn;
};

static void check(struct tally *t, const char *level, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static void check(struct tally *t, const char *level, const char *fmt, ...)
{
	va_list ap;

	if (strcmp(level, "FAIL") == 0)
		t->fail++;
	else if (strcmp(level, "WARN") == 0)
		t->warn++;
	printf("[%-4s] ", level);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
}

/* Each line of text, prefixed with indent (continuation lines of a doctor check). */
static void print_indented(const char *indent, const char *text)
{
	while (*text) {
		size_t n = strcspn(text, "\n");

		printf("%s%.*s\n", indent, (int)n, text);
		text += n + (text[n] == '\n');
	}
}

/*
 * What the device table says about the card: the VF override is a warning; a VF count above what the
 * card offers fails (prepare refuses it).
 */
static void entry_checks(struct tally *t, const struct gs_card *c, const struct gs_config *cfg)
{
	const struct gs_device *e = c->entry;
	char why[512];

	printf("  %s (%s, %s) -> vGPU target 10de:%04x %s\n", e->name, e->die, gs_generation_name(e->generation),
	       gs_target_of(e)->device, gs_target_of(e)->name);
	gs_vfs_describe(e, cfg, why, sizeof(why));
	printf("  VFs: %s\n", why);
	switch (gs_vfs_check(e, cfg, why, sizeof(why))) {
	case -1:
		check(t, "FAIL", "%s: %s; prepare refuses it", c->pf.str, why);
		break;
	case 1:
		check(t, "WARN", "%s: %s", c->pf.str, why);
		break;
	}
	if (e->generation == GS_ADA)
		check(t, "INFO", "%s: " ADA_STACK_NOTE, c->pf.str);
}

/*
 * Selected cards of an Ada card's own and another generation: the first Ada card in ada, the first
 * other one in other (its generation in *gen). 1 when both exist.
 */
static int selection_mixes_ada(const char *sysroot, const struct gs_addr *sel, unsigned int n, char *ada,
			       char *other, size_t len, unsigned int *gen)
{
	ada[0] = other[0] = '\0';
	for (unsigned int i = 0; i < n; i++) {
		struct gs_card c;

		gs_card_inspect(sysroot, &sel[i], &c);
		if (!c.present || !c.entry)
			continue;
		if (c.entry->generation == GS_ADA && !ada[0]) {
			snprintf(ada, len, "%s", c.pf.str);
		} else if (c.entry->generation != GS_ADA && !other[0]) {
			snprintf(other, len, "%s", c.pf.str);
			*gen = c.entry->generation;
		}
	}
	return ada[0] && other[0];
}

/* Enabled VFs (virtfn0..numvfs-1) whose IOMMU group is the PF's; the first goes to first. */
static unsigned int vfs_sharing_pf_group(const char *sysroot, const struct gs_card *c, char *first, size_t len)
{
	char p[GS_PATH_MAX], target[GS_PATH_MAX];
	unsigned int i, n = 0;

	first[0] = '\0';
	if (!c->iommu_group[0])
		return 0;
	for (i = 0; i < c->numvfs && i < 256; i++) {
		struct gs_addr a;
		struct gs_card vf;
		const char *base;
		ssize_t k;

		if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/virtfn%u", sysroot, c->pf.str, i))
			break;
		k = readlink(p, target, sizeof(target) - 1);
		if (k <= 0)
			continue;
		target[k] = '\0';
		base = strrchr(target, '/');
		if (gs_addr_parse(base ? base + 1 : target, &a))
			continue;
		gs_card_inspect(sysroot, &a, &vf);
		if (strcmp(vf.iommu_group, c->iommu_group) == 0 && !n++)
			gs_copy(first, len, a.str);
	}
	return n;
}

/* The card's IOMMU grouping as the kernel reports it. */
static void iommu_checks(struct tally *t, const char *sysroot, const struct gs_card *c)
{
	char first[256], what[512];
	unsigned int nothers;
	int with_bridge;

	if (iommu_peers(sysroot, c, &with_bridge, first, sizeof(first), &nothers))
		return;
	if (!with_bridge && !nothers) {
		check(t, "INFO", "%s: IOMMU group %s holds only this card's functions", c->pf.str, c->iommu_group);
		return;
	}
	if (!nothers) {
		check(t, "INFO", "%s: IOMMU group %s also holds its bridge %s; whether VFIO accepts that depends on the bridge's driver",
		      c->pf.str, c->iommu_group, c->bridge.str);
		return;
	}
	if (with_bridge)
		snprintf(what, sizeof(what), "its bridge %s and %u other device(s), first %s", c->bridge.str, nothers, first);
	else
		snprintf(what, sizeof(what), "%u other device(s), first %s", nothers, first);
	check(t, "WARN", "%s: IOMMU group %s also holds %s. VFs in that group are not isolated from those devices or each other, and VFIO may refuse them (an ACS override adds no real isolation)",
	      c->pf.str, c->iommu_group, what);
}

/* One NVIDIA physical function as the identity hook reads it from sysfs. */
struct id_fn {
	struct gs_addr a;
	unsigned int device;
	int display;
};

#define ID_SCAN_MAX 64u

/*
 * The PCI scan the identity hook relies on, read as strictly as the hook reads it: every entry of
 * bus/pci/devices must be a canonical address with readable vendor and device, and every NVIDIA
 * physical function (no physfn) a readable class. Returns -1 and the offending entry in bad
 * otherwise; doctor then cannot confirm the mapping. Sorted by address.
 */
static int scan_identity(const char *sysroot, struct id_fn *out, unsigned int *n, char *bad, size_t len)
{
	char p[GS_PATH_MAX];
	struct dirent *de;
	int r = 0;
	DIR *d;

	*n = 0;
	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices", sysroot) || !(d = opendir(p))) {
		snprintf(bad, len, "%s/bus/pci/devices", sysroot);
		return -1;
	}
	while (!r && (de = readdir(d))) {
		char q[GS_PATH_MAX];
		unsigned int vendor = 0, device = 0, class_code = 0;
		struct gs_addr a;

		if (de->d_name[0] == '.')
			continue;
		if (gs_addr_parse(de->d_name, &a) ||
		    gs_pathf(q, sizeof(q), "%s/%s/vendor", p, a.str) || gs_read_hex(q, &vendor) ||
		    gs_pathf(q, sizeof(q), "%s/%s/device", p, a.str) || gs_read_hex(q, &device)) {
			r = -1;
		} else if (vendor != GS_VENDOR_NVIDIA ||
			   (!gs_pathf(q, sizeof(q), "%s/%s/physfn", p, a.str) && gs_exists(q))) {
			continue;
		} else if (gs_pathf(q, sizeof(q), "%s/%s/class", p, a.str) || gs_read_hex(q, &class_code) ||
			   *n == ID_SCAN_MAX) {
			r = -1;
		} else {
			out[*n].a = a;
			out[*n].device = device;
			out[(*n)++].display = (class_code >> 16) == 0x03;
			continue;
		}
		snprintf(bad, len, "%s/%.64s", p, de->d_name);
	}
	closedir(d);
	for (unsigned int i = 1; i < *n; i++)
		for (unsigned int j = i; j > 0 && strcmp(out[j - 1].a.str, out[j].a.str) > 0; j--) {
			struct id_fn tmp = out[j];

			out[j] = out[j - 1];
			out[j - 1] = tmp;
		}
	return r;
}

/*
 * The identity hook's decision (hook/gs_hook.c) for a selection. A selected card is mappable when
 * it is an NVIDIA display physical function in the device table and the only one in its
 * domain:bus:slot. At runtime the hook attributes each PCI-info reply to a bus address through the
 * driver's own GPU-ID and PCI-info queries and rewrites only mappable selected cards. Only when
 * those queries fail does it fall back to the device ID, and only for an ID that every NVIDIA
 * physical function having it (any class) is a mapped card. HOST is reported only for a mapped
 * card whose address query succeeds; the fallback never forces it. Whether the installed driver
 * answers those queries is unknown until nvidia-vgpud runs; doctor never issues them.
 */
static void identity_checks(struct tally *t, const char *sysroot, const struct gs_addr *sel, unsigned int nsel,
			    int force_host)
{
	struct id_fn fns[ID_SCAN_MAX];
	struct gs_addr mapped[GS_CFG_MAX_BDFS];
	unsigned int mdev[GS_CFG_MAX_BDFS], nfns, nmapped = 0, i, j;
	char bad[GS_PATH_MAX + 80];

	if (scan_identity(sysroot, fns, &nfns, bad, sizeof(bad))) {
		check(t, "WARN", "identity: cannot confirm the mapping: %s is unreadable or invalid, or there are more than %u NVIDIA functions. The hook reads the same entries and may leave the selected GPUs unchanged",
		      bad, ID_SCAN_MAX);
		return;
	}
	for (i = 0; i < nsel; i++) {
		struct gs_card c;
		unsigned int inslot = 0;

		gs_card_inspect(sysroot, &sel[i], &c);
		if (!c.present || !c.entry || c.is_vf || (c.class_code >> 16) != 0x03) {
			check(t, "INFO", "identity: selected %s is %s; the hook skips it", sel[i].str,
			      !c.present ? "absent" : "not an NVIDIA display physical function in the device table");
			continue;
		}
		for (j = 0; j < nfns; j++)
			inslot += fns[j].display && fns[j].a.domain == c.pf.domain && fns[j].a.bus == c.pf.bus &&
				  fns[j].a.slot == c.pf.slot;
		if (inslot != 1) {
			check(t, "WARN", "identity: selected %s shares its slot with another NVIDIA display function; the hook leaves an ambiguous slot unchanged",
			      c.pf.str);
			continue;
		}
		mapped[nmapped] = c.pf;
		mdev[nmapped++] = c.device;
		check(t, "OK", "identity: selected %s (10de:%04x) matches by bus address; nvidia-vgpud sees it as 10de:%04x %s (the driver's address queries are checked only when nvidia-vgpud runs)",
		      c.pf.str, c.device, gs_target_of(c.entry)->device, gs_target_of(c.entry)->name);
	}
	/* The device-ID fallback, per mapped card: refused while another, unmapped PF has the same ID. */
	for (i = 0; i < nmapped; i++) {
		for (j = 0; j < nfns; j++) {
			int selected = addr_listed(sel, nsel, fns[j].a.str);

			if (fns[j].device != mdev[i] || addr_listed(mapped, nmapped, fns[j].a.str))
				continue;
			if (!selected)
				check(t, "INFO", "identity: unselected %s has the same device ID; matching by bus address leaves it unchanged and rewrites only %s",
				      fns[j].a.str, mapped[i].str);
			check(t, "WARN", "identity: %s %s has the same device ID 10de:%04x as selected %s; if the driver's address queries fail, the device-ID fallback is refused and both stay 10de:%04x",
			      selected ? "selected but unmappable" : "unselected", fns[j].a.str, mdev[i], mapped[i].str,
			      mdev[i]);
			break;
		}
	}
	for (j = 0; j < nfns; j++)
		if (fns[j].display && !addr_listed(sel, nsel, fns[j].a.str))
			check(t, "INFO", "identity: unselected NVIDIA GPU %s (10de:%04x) is not rewritten", fns[j].a.str,
			      fns[j].device);
	if (force_host && nmapped)
		check(t, "INFO", "identity_force_host: HOST is reported only for a selected card whose bus address the driver returns; other virtualization-mode replies stay unchanged");
}

/*
 * What the *.conf files of a modprobe.d directory say about nvidia, in name order: the first file other
 * than the driver step's own that sets NVreg_EnableGpuFirmware to a value other than 0 (on any
 * `options nvidia` line, among any other options), the first that sets it to 0, and the first file with
 * a `blacklist nvidia` line and the first with an `install nvidia /bin/false` line, which together hold
 * nvidia back.
 */
struct modprobe_view {
	char other[GS_PATH_MAX], other_value[64], zero[GS_PATH_MAX];
	char blacklist[GS_PATH_MAX], install_false[GS_PATH_MAX];
};

static void modprobe_file(const char *path, int own, struct modprobe_view *v)
{
	char *data, *line, *save = NULL;
	size_t len;

	if (gs_read_whole(path, &data, &len, GS_CONFIG_MAX_BYTES))
		return;
	for (line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *tok[3], *rest = NULL, *t;
		unsigned int n = 0;

		for (t = strtok_r(line, " \t\r", &rest); t && n < 3; t = n < 3 ? strtok_r(NULL, " \t\r", &rest) : NULL)
			tok[n++] = t;
		if (n >= 2 && strcmp(tok[0], "blacklist") == 0 && strcmp(tok[1], "nvidia") == 0 && !v->blacklist[0])
			gs_copy(v->blacklist, sizeof(v->blacklist), path);
		if (n == 3 && strcmp(tok[0], "install") == 0 && strcmp(tok[1], "nvidia") == 0 &&
		    strcmp(tok[2], "/bin/false") == 0 && !strtok_r(NULL, " \t\r", &rest) && !v->install_false[0])
			gs_copy(v->install_false, sizeof(v->install_false), path);
		if (own || n < 3 || strcmp(tok[0], "options") != 0 || strcmp(tok[1], "nvidia") != 0)
			continue;
		for (t = tok[2]; t; t = strtok_r(NULL, " \t\r", &rest)) {
			const char *value;

			if (strncmp(t, "NVreg_EnableGpuFirmware", 23) != 0 || (t[23] != '=' && t[23] != '\0'))
				continue;
			value = t[23] ? t + 24 : "";
			if (strcmp(value, "0") != 0 && !v->other[0]) {
				gs_copy(v->other, sizeof(v->other), path);
				gs_copy(v->other_value, sizeof(v->other_value), value);
			} else if (strcmp(value, "0") == 0 && !v->zero[0]) {
				gs_copy(v->zero, sizeof(v->zero), path);
			}
		}
	}
	free(data);
}

static void modprobe_read(const char *dir, struct modprobe_view *v)
{
	const char *own = strrchr(GS_NVIDIA_MODPROBE, '/') + 1;
	struct dirent **names;
	char p[GS_PATH_MAX];
	int n, i;

	memset(v, 0, sizeof(*v));
	n = scandir(dir, &names, NULL, alphasort);
	for (i = 0; i < n; i++) {
		size_t len = strlen(names[i]->d_name);

		if (len > 5 && strcmp(names[i]->d_name + len - 5, ".conf") == 0 &&
		    !gs_pathf(p, sizeof(p), "%s/%s", dir, names[i]->d_name))
			modprobe_file(p, strcmp(names[i]->d_name, own) == 0, v);
		free(names[i]);
	}
	if (n >= 0)
		free(names);
}

/* The file, or "a and b" when the two lines are in different files, that holds nvidia back; 0, or -1 when none does. */
static int hold_files(const struct modprobe_view *v, char *files, size_t len)
{
	if (!v->blacklist[0] || !v->install_false[0])
		return -1;
	if (strcmp(v->blacklist, v->install_false) == 0)
		snprintf(files, len, "%s", v->blacklist);
	else
		snprintf(files, len, "%s and %s", v->blacklist, v->install_false);
	return 0;
}

int gs_nvidia_hold(const char *dir, char *files, size_t len)
{
	struct modprobe_view v;

	modprobe_read(dir, &v);
	return hold_files(&v, files, len);
}

/*
 * The option the driver step checks; with a console release granted, the hold that keeps nvidia unbound
 * (or, with hold_planned, that install writes it); with an installed config that grants none, a hold left
 * over, which keeps nvidia from loading at all.
 */
static void modprobe_checks(struct tally *t, const char *dir, const struct gs_config *cfg, int hold_planned)
{
	struct modprobe_view v;
	char own[GS_PATH_MAX], files[2 * GS_PATH_MAX + 8];

	modprobe_read(dir, &v);
	if (v.other[0])
		check(t, "FAIL", "%s sets NVreg_EnableGpuFirmware=%s; this stack needs 0", v.other, v.other_value);
	else if (v.zero[0])
		check(t, "INFO", "%s already sets NVreg_EnableGpuFirmware=0; the driver step keeps it%s", v.zero,
		      !gs_pathf(own, sizeof(own), "%s/%s", dir, strrchr(GS_NVIDIA_MODPROBE, '/') + 1) && gs_exists(own) ?
		      "; its own file stays until uninstall" : " and writes no file of its own");
	if (!cfg->nrelease) {
		/* The documented hold file goes as a whole; any other file may hold more than the two lines. */
		if (cfg->present && !hold_files(&v, files, sizeof(files))) {
			const char *base = strrchr(v.blacklist, '/');
			int whole = strcmp(v.blacklist, v.install_false) == 0 && base &&
				    strcmp(base + 1, "nvidia-manual-load.conf") == 0;

			check(t, "WARN", "nvidia is held back at boot by %s, and no console release is granted. activate will "
			      "refuse. Remove %s%s (docs/installation.md#returning-to-automatic-preparation)", files,
			      whole ? "" : "the `blacklist nvidia` and `install nvidia /bin/false` lines from ", files);
		}
		return;
	}
	if (!hold_files(&v, files, sizeof(files))) {
		check(t, "OK", "nvidia is held back at boot (`blacklist nvidia` and `install nvidia /bin/false` in %s)", dir);
	} else if (hold_planned) {
		check(t, "INFO", "hold planned by --hold-nvidia: install writes %s", GS_NVIDIA_HOLD);
	} else {
		char rel[GS_BDF_LIST_STR_SIZE];

		gs_addr_list_str(cfg->boot_display_release, cfg->nrelease, rel, sizeof(rel));
		check(t, "WARN", "nvidia is not held back at boot, so it would bind %s, the console card, before prepare runs; "
		      "prepare cannot act on a bound card. Add the hold from docs/installation.md#boot-display-card", rel);
	}
}

/*
 * Doctor's line for the vGPU plugin nvidia-vgpu-mgr loads, with activate's verdict: OK for the patched
 * plugin copy, else a warning with the reason. Before the driver step has run here (no record and no
 * install manifest beside it, as for the build line) the stock plugin is expected, so that is noted only.
 */
static void plugin_checks(struct tally *t, const char *path, const char *record)
{
	char sha[65], real[GS_PATH_MAX], why[1024], manifest[GS_PATH_MAX];
	const char *slash = strrchr(record, '/');
	size_t n;
	int r = gs_sha256_file_resolved(path, sha, real);
	int fresh = slash && !gs_exists(record) &&
		    !gs_pathf(manifest, sizeof(manifest), "%.*s/" GS_MANIFEST_NAME, (int)(slash - record), record) &&
		    !gs_exists(manifest);

	if (r) {
		check(t, fresh ? "INFO" : "WARN", "cannot read the vGPU plugin %s (from NVIDIA's host package): %s", path,
		      strerror(-r));
		return;
	}
	if (!gs_plugin_verdict(path, real, sha, why, sizeof(why))) {
		check(t, "OK", "vGPU plugin %s -> %s: the patched plugin copy", path, real);
		return;
	}
	n = strcspn(why, "\n");
	check(t, fresh ? "INFO" : "WARN", "%.*s", (int)n, why);
	if (why[n])
		print_indented("       ", why + n + 1);
	if (fresh)
		print_indented("       ", "the driver step has not run on this host; install makes the patched plugin copy");
}

/* The LD_PRELOAD that the Environment= lines of the drop-in at path set, the last one winning: 1 with it in value, else 0. */
static int dropin_preload(const char *path, char *value, size_t len)
{
	FILE *f = fopen(path, "re");
	char *line = NULL;
	size_t cap = 0;
	int found = 0;

	if (!f)
		return 0;
	while (getline(&line, &cap, f) > 0) {
		const char *start = line + strspn(line, " \t"), *q;

		if (strncmp(start, "Environment=", 12) != 0)
			continue;
		/* An assignment starts the value, or follows a blank or an opening quote, which also ends it. */
		for (start += 12, q = start; (q = strstr(q, "LD_PRELOAD=")); q++)
			if (q == start || strchr(" \t\"'", q[-1])) {
				const char *end = q > start && (q[-1] == '"' || q[-1] == '\'') ? (q[-1] == '"' ? "\"\r\n" : "'\r\n") :
						  " \t\"'\r\n";

				snprintf(value, len, "%.*s", (int)strcspn(q + 11, end), q + 11);
				found = 1;
			}
	}
	free(line);
	fclose(f);
	return found;
}

/* 1 when the LD_PRELOAD list value, separated by colons or blanks, names hook. */
static int preload_has(const char *value, const char *hook)
{
	size_t n = strlen(hook);

	for (const char *p = value; *p; p += strcspn(p, ": \t")) {
		p += strspn(p, ": \t");
		if (strncmp(p, hook, n) == 0 && (p[n] == '\0' || strchr(": \t", p[n])))
			return 1;
	}
	return 0;
}

/*
 * Drop-ins of NVIDIA's vGPU daemons, other than the package's own, that set an LD_PRELOAD without the hook.
 * systemd reads a unit's drop-ins in file name order across its unit directories, a name in a more
 * significant directory hiding it in the others, and the last LD_PRELOAD wins, so in nvidia-vgpud.service.d
 * only the files after the package's own replace the hook. dir: the one unit directory to read; NULL: systemd's.
 */
static void dropin_checks(struct tally *t, const char *dir)
{
	static const char *const units[] = { "nvidia-vgpud", "nvidia-vgpu-mgr" };
	const char *const *dirs = dir ? &dir : unit_dirs;
	size_t ndirs = dir ? 1 : sizeof(unit_dirs) / sizeof(unit_dirs[0]);
	char hook[GS_PATH_MAX] = "", p[GS_PATH_MAX], value[GS_PATH_MAX];
	int own = 0;

	/* The hook the package's own drop-in preloads; without one, where install puts it by default. */
	for (size_t d = 0; d < ndirs; d++)
		if (!gs_pathf(p, sizeof(p), "%s/%s.service.d/" GS_VGPUD_DROPIN_NAME, dirs[d], units[0]) && gs_exists(p)) {
			own = dropin_preload(p, hook, sizeof(hook));
			break;
		}
	if (!own)
		gs_copy(hook, sizeof(hook), GS_DEFAULT_PREFIX "/lib/geforce-sriov/libgeforce_sriov.so");
	for (size_t u = 0; u < sizeof(units) / sizeof(units[0]); u++)
		for (size_t d = 0; d < ndirs; d++) {
			struct dirent **names;
			int n;

			if (gs_pathf(p, sizeof(p), "%s/%s.service.d", dirs[d], units[u]))
				continue;
			n = scandir(p, &names, NULL, alphasort);
			for (int i = 0; i < n; i++) {
				const char *name = names[i]->d_name;
				size_t len = strlen(name);
				int skip = len <= 5 || strcmp(name + len - 5, ".conf") != 0 ||
					   (u == 0 && (strcmp(name, GS_VGPUD_DROPIN_NAME) == 0 ||
						       (own && strcmp(name, GS_VGPUD_DROPIN_NAME) < 0)));

				for (size_t e = 0; !skip && e < d; e++)
					skip = !gs_pathf(p, sizeof(p), "%s/%s.service.d/%s", dirs[e], units[u], name) &&
					       gs_exists(p);
				if (!skip && !gs_pathf(p, sizeof(p), "%s/%s.service.d/%s", dirs[d], units[u], name) &&
				    dropin_preload(p, value, sizeof(value)) && !preload_has(value, hook))
					check(t, "WARN", "%s sets LD_PRELOAD for %s and overrides the geforce-sriov identity hook. "
					      "Remove it or point it at %s.", p, units[u], hook);
				free(names[i]);
			}
			if (n >= 0)
				free(names);
		}
}

/* activate's marker in o's run directory (GS_RUN_DIR when live, none on a fixture without one): 1 when it exists. */
static int activating_left(const struct gs_diag_opts *o, int live, char *p, size_t len)
{
	const char *run = o->run_dir ? o->run_dir : live ? GS_RUN_DIR : NULL;

	return run && !gs_pathf(p, len, "%s/" GS_ACTIVATING_NAME, run) && gs_exists(p);
}

/* 1 when a *.conf file in the modprobe.d directory dir has `options nvidia ... NVreg_DynamicPowerManagement=0` (file names it). */
static int nvidia_rtd3_off(const char *dir, char *file, size_t len)
{
	struct dirent *de;
	DIR *d = opendir(dir);
	int found = 0;

	if (!d)
		return 0;
	while (!found && (de = readdir(d))) {
		char p[GS_PATH_MAX], *data, *line, *save = NULL;
		size_t n = strlen(de->d_name);

		if (n < 6 || strcmp(de->d_name + n - 5, ".conf") != 0 || gs_pathf(p, sizeof(p), "%s/%s", dir, de->d_name) ||
		    gs_read_whole(p, &data, &n, 1 << 16))
			continue;
		for (line = strtok_r(data, "\n", &save); line && !found; line = strtok_r(NULL, "\n", &save)) {
			char *tok, *save2 = NULL;
			unsigned int k = 0;

			for (tok = strtok_r(line, " \t", &save2); tok && !found; tok = strtok_r(NULL, " \t", &save2), k++) {
				size_t tl = strlen(tok);

				if ((k == 0 && strcmp(tok, "options") != 0) || (k == 1 && strcmp(tok, "nvidia") != 0))
					break;
				/* NVIDIA's documentation writes the option in double quotes. */
				if (tl >= 2 && tok[0] == '"' && tok[tl - 1] == '"') {
					tok[tl - 1] = '\0';
					tok++;
				}
				found = k > 1 && (strcmp(tok, "NVreg_DynamicPowerManagement=0") == 0 ||
						  strcmp(tok, "NVreg_DynamicPowerManagement=0x00") == 0);
			}
		}
		free(data);
		if (found)
			gs_copy(file, len, p);
	}
	closedir(d);
	return found;
}

/* 1 when sleep.target in the unit directory dir is masked (a link to /dev/null). */
static int sleep_masked(const char *dir)
{
	char p[GS_PATH_MAX], target[64];
	ssize_t n;

	if (gs_pathf(p, sizeof(p), "%s/sleep.target", dir) || (n = readlink(p, target, sizeof(target) - 1)) < 0)
		return 0;
	target[n] = '\0';
	return strcmp(target, "/dev/null") == 0;
}

/*
 * A host with the laptop rows laptops (n) selected: nvidia's runtime power management and system sleep each power
 * the GPU off (D3cold), which resets its SR-IOV state and every vGPU. doctor only names the change, once per GPU
 * while it is missing; the package makes neither.
 */
static void laptop_host_checks(struct tally *t, const struct gs_diag_opts *o, int live, const struct gs_addr *laptops,
			       unsigned int n)
{
	const char *mpd = o->modprobe_dir ? o->modprobe_dir : live ? "/etc/modprobe.d" : NULL;
	const char *units = o->unit_dir ? o->unit_dir : live ? GS_UNIT_DIR : NULL;
	char file[GS_PATH_MAX];
	unsigned int i;

	if (mpd && nvidia_rtd3_off(mpd, file, sizeof(file)))
		check(t, "OK", "laptop GPU: nvidia's runtime power management is off (NVreg_DynamicPowerManagement=0 in %s)",
		      file);
	else if (mpd)
		for (i = 0; i < n; i++)
			check(t, "WARN", "%s: nvidia may power off this GPU: set options nvidia "
			      "NVreg_DynamicPowerManagement=0 (docs/laptops.md#d3cold)", laptops[i].str);
	if (units && sleep_masked(units))
		check(t, "OK", "laptop host: system sleep is blocked (sleep.target is masked)");
	else if (units)
		for (i = 0; i < n; i++)
			check(t, "WARN", "%s: suspend is not blocked; sleep powers off this GPU (docs/laptops.md#d3cold)",
			      laptops[i].str);
}

int gs_cmd_doctor(const struct gs_diag_opts *o)
{
	const char *sysroot = o->sysroot ? o->sysroot : GS_LIVE_SYSFS;
	int live = is_live_root(sysroot);
	struct gs_addr devs[GS_CFG_MAX_BDFS], laptops[GS_CFG_MAX_BDFS];
	struct module_state m;
	struct gs_config cfg;
	struct tally t = { 0, 0 };
	const char *cfg_path, *source, *isrc;
	const struct gs_addr *isel;
	char err[512], p[GS_PATH_MAX], act[GS_PATH_MAX];
	unsigned int n, i, nisel, nlaptops;
	int id_on, id_force, activating = activating_left(o, live, act, sizeof(act));

	printf("geforce-sriov %s doctor (sysfs root %s%s)\n", GS_VERSION, sysroot,
	       live ? "" : ", diagnostic fixture: host checks skipped");
	if (diag_config(o, &cfg, &cfg_path, err, sizeof(err))) {
		check(&t, "FAIL", "config: %s", err);
		gs_config_defaults(&cfg);
	} else if (!cfg_path) {
		check(&t, "INFO", "config: not read (fixture root; pass --config)");
	} else if (!cfg.present) {
		check(&t, "INFO", "config: %s absent; %s", cfg_path,
		      o->ndevices ? "the selected devices come from --device" : "defaults apply and no device is selected");
	} else {
		check(&t, "OK", "config: %s: %u device(s) selected; ari=%s msix=%s vfs=%u%s identity=%s",
		      cfg_path, cfg.nbdfs, sw(cfg.ari), sw(cfg.msix),
		      gs_config_vfs(&cfg), cfg.vfs ? "" : " (default)", cfg.identity == GS_SW_ON ? "on" : "off");
		if (live && config_file_safe(cfg_path, err, sizeof(err)))
			check(&t, "FAIL", "config: %s; prepare and activate refuse it", err);
		if (cfg.nrelease) {
			char rel[GS_BDF_LIST_STR_SIZE];

			gs_addr_list_str(cfg.boot_display_release, cfg.nrelease, rel, sizeof(rel));
			check(&t, "INFO", "boot_display_release=%s: the boot unit defers and prepares none of the selected GPUs.\n"
			      "       " GS_RELEASE_STEPS_LEAD, rel);
			gs_release_steps(stdout, "         ", &cfg, 1);
			print_indented("       ", GS_CONSOLE_RELEASE_NOTE);
		}
	}
	/*
	 * --boot-display-release: the grants `install --device ... --boot-display-release` writes in place
	 * of the config's, so each card is judged with its console release granted.
	 */
	if (o->nrelease) {
		char rel[GS_BDF_LIST_STR_SIZE];

		for (i = 0; i < o->nrelease; i++)
			cfg.boot_display_release[i] = o->release[i];
		cfg.nrelease = o->nrelease;
		gs_addr_list_str(cfg.boot_display_release, cfg.nrelease, rel, sizeof(rel));
		check(&t, "INFO", "config: console release of %s planned by --boot-display-release", rel);
	}
	/* Like install, --hold-nvidia needs a console release: one in the config or one planned above. */
	if (o->hold_nvidia && !cfg.nrelease) {
		fprintf(stderr, "geforce-sriov: --hold-nvidia needs --boot-display-release BDF (or boot_display_release in the "
			"config): only a boot display GPU granted a console\nrelease needs nvidia held back at boot\n");
		return GS_EXIT_USAGE;
	}
	n = selected_devices(o, &cfg, sysroot, devs, &source);
	nlaptops = gs_laptop_gpus(sysroot, devs, n, laptops);
	/* Like install, --disable-d3cold needs a laptop GPU among the GPUs judged. */
	if (o->disable_d3cold && !nlaptops)
		return gs_d3cold_usage();
	/*
	 * Identity as the hook will see it: the config's settings or, for a --device selection the
	 * config does not hold yet, what `install --device` writes (identity=on, identity_force_host=on).
	 */
	isel = cfg.bdfs;
	nisel = cfg.nbdfs;
	id_on = cfg.identity == GS_SW_ON;
	id_force = cfg.identity_force_host == GS_SW_ON;
	isrc = cfg_path ? cfg_path : "defaults";
	if (o->ndevices && !same_selection(o->devices, o->ndevices, cfg.bdfs, cfg.nbdfs)) {
		isel = o->devices;
		nisel = o->ndevices;
		id_on = id_force = 1;
		isrc = "planned: `install --device` writes identity=on and identity_force_host=on";
	}

	if (!gs_pathf(p, sizeof(p), "%s/kernel/livepatch", sysroot) && gs_exists(p))
		check(&t, "OK", "kernel livepatch support present");
	else
		check(&t, "FAIL", "kernel: no %s/kernel/livepatch (CONFIG_LIVEPATCH is required)", sysroot);
	if (live && cmdline_noari(GS_LIVE_PROC))
		check(&t, "INFO", "kernel command line has pci=noari: prepare loads %s with ari=0", GS_MODULE_NAME);
	if (!gs_pathf(p, sizeof(p), "%s/kernel/iommu_groups", sysroot)) {
		DIR *d = opendir(p);
		int any = 0;
		struct dirent *de;

		while (d && (de = readdir(d)))
			if (de->d_name[0] != '.')
				any = 1;
		if (d)
			closedir(d);
		check(&t, any ? "OK" : "WARN", "IOMMU groups %s", any ? "present" : "absent: VFs cannot be assigned to VMs");
	}
	read_module_state(sysroot, &m);
	if (!m.loaded)
		check(&t, "INFO", "module %s not loaded (prepare loads it)", GS_MODULE_NAME);
	else
		check(&t, m.lp_ok ? "OK" : "WARN",
		      "module %s loaded: ari=%s msix=%s vfs=%s bdfs=%s; livepatch %s",
		      GS_MODULE_NAME, m.ari, m.msix, m.vfs[0] ? m.vfs : "(none)",
		      m.bdfs[0] ? m.bdfs : "(none)",
		      !m.lp_present ? "directory missing" : m.lp_ok ? "enabled, transition complete" :
		      "not enabled or still in transition");
	if (m.loaded && module_vfs(&m) != gs_config_vfs(&cfg))
		check(&t, "WARN", "module %s was loaded with vfs=%u, the config has vfs=%u; the new count takes effect at the next boot",
		      GS_MODULE_NAME, module_vfs(&m), gs_config_vfs(&cfg));

	if (live) {
		char where[GS_PATH_MAX], ver[64];
		int mod = installed_module_present();
		int dkms = gs_find_program("dkms", p, sizeof(p)) == 0;
		int sysctl = gs_find_program("systemctl", p, sizeof(p)) == 0;
		int deps_ok = 1;
		static const char *const deps[] = { "lspci", "setpci" };

		if (getuid() != 0)
			check(&t, "INFO", "not root: config space beyond 64 bytes is unreadable; bridge ARI support unknown");
		check(&t, mod ? "OK" : "WARN", "kernel module for the running kernel %s",
		      mod ? "installed" : "not found under /lib/modules/$(uname -r) (geforce-sriov install builds it with DKMS)");
		/* What the driver step needs: headers for the running kernel, a C compiler and make. */
		{
			char rel[64] = "", hp[GS_PATH_MAX];
			const char *lvl = mod ? "WARN" : "FAIL";
			const char *why = mod ? ": only a later `install` needs it (the module is installed for this kernel)"
					      : ": the driver step cannot build the module";

			gs_read_attr(GS_LIVE_PROC "/sys/kernel/osrelease", rel, sizeof(rel));
			if (rel[0] && (hp[0] = '\0', !gs_pathf(hp, sizeof(hp), "/lib/modules/%s/build/Makefile", rel)) &&
			    gs_exists(hp))
				check(&t, "OK", "kernel headers for %s present", rel);
			else
				check(&t, lvl, "kernel headers for %s missing (%s)%s", rel[0] ? rel : "the running kernel",
				      rel[0] ? hp : "/lib/modules/<release>/build", why);
			if (gs_find_program("cc", p, sizeof(p)) == 0)
				check(&t, "OK", "C compiler found at %s", p);
			else
				check(&t, lvl, "cc not found%s", why);
			if (gs_find_program("make", p, sizeof(p)) == 0)
				check(&t, "OK", "make found at %s", p);
			else
				check(&t, lvl, "make not found%s", why);
			const char *nsrc = o->nvidia_source ? o->nvidia_source : GS_NVIDIA_SRC;
			char npath[GS_PATH_MAX];

			/* DIR may be the kernel tree or an extracted package root, whose tree is in kernel/. */
			if (gs_pathf(npath, sizeof(npath), "%s/nvidia/nv-kernel.o_binary", nsrc))
				gs_copy(npath, sizeof(npath), nsrc);
			if (!gs_exists(npath) && gs_pathf(npath, sizeof(npath), "%s/kernel/nvidia/nv-kernel.o_binary", nsrc) == 0 &&
			    gs_exists(npath))
				gs_pathf(npath, sizeof(npath), "%s/kernel", nsrc);
			if (o->driver_file) {
				check(&t, "INFO", "NVIDIA's kernel source tree: install extracts it from %s into %s/<version>/kernel",
				      o->driver_file, GS_NVIDIA_PKG_DIR);
				if (gs_exists("/sys/module/nouveau"))
					check(&t, "WARN", "nouveau is loaded: install refuses to run NVIDIA's installer while it is. Put\n"
					      "       `blacklist nouveau` and `options nouveau modeset=0` in /etc/modprobe.d/blacklist-nouveau.conf,\n"
					      "       rebuild the initramfs (`update-initramfs -u`, or `dracut -f`) and power the machine off and on");
			} else if (gs_exists(npath)) {
				check(&t, "OK", "NVIDIA's kernel source tree found at %s", nsrc);
			} else {
				check(&t, mod ? "WARN" : "FAIL",
				      "%s not found: the driver step needs NVIDIA's %s kernel source tree (/usr/src/nvidia-%s\n"
				      "       after a DKMS install, else <extracted package>/kernel). Pass `--driver-file PATH` (NVIDIA's\n"
				      "       .run package) to extract it, `--nvidia-source DIR` to name an extracted tree, or\n"
				      "       `--driver=skip` to install geforce-sriov alone",
				      nsrc, GS_NVIDIA_VERSION, GS_NVIDIA_VERSION);
			}
		}
		/* What the later steps need: a live install (dkms, systemctl). */
		if (dkms && sysctl)
			check(&t, "OK", "dkms and systemctl found (a live install needs both)");
		else if (!dkms && sysctl)
			check(&t, "WARN", "dkms not found: a live `geforce-sriov install` refuses without it%s",
			      mod ? " (the module is already installed for this kernel)" : "");
		else
			check(&t, "WARN", "%s not found: a live `geforce-sriov install`, with or without --device, needs dkms and systemctl%s",
			      !dkms ? "dkms and systemctl" : "systemctl",
			      mod ? " (the module is already installed for this kernel)" : "");
		{
			char rel[64] = "";
			int missing = 0;

			gs_read_attr(GS_LIVE_PROC "/sys/kernel/osrelease", rel, sizeof(rel));
			for (unsigned int d = 0; d < GS_DEP_COUNT; d++)
				missing |= !gs_dep_present((enum gs_dep)d, rel);
			if (missing)
				check(&t, "INFO", "%s", GS_PREREQ_MSG);
		}
		if (gs_exists(GS_ATTEMPT_MARKER)) {
			check(&t, "FAIL", "guard %s is set: a preparation attempt did not finish.", GS_ATTEMPT_MARKER);
			print_indented("       ", recovery_msg);
		} else if (!activating) {
			check(&t, "OK", "no unfinished preparation attempt");
		}
		if (cfg_path && cfg.nbdfs && strcmp(cfg_path, GS_CONFIG_PATH) == 0) {
			if (!gs_exists(GS_UNIT_WANTS))
				check(&t, "WARN", "%s is not enabled, so the next boot does not prepare the selected GPUs; `geforce-sriov install --device BDF` enables it",
				      GS_UNIT_PREPARE);
			else if (cfg.nrelease)
				check(&t, "OK", "%s is enabled: at the next boot it defers (boot_display_release) and holds nvidia-vgpud back",
				      GS_UNIT_PREPARE);
			else
				check(&t, "OK", "%s is enabled: the next boot prepares the selected GPUs", GS_UNIT_PREPARE);
		}
		if (gs_exists(GS_DEFERRED))
			check(&t, "INFO", "%s: the boot unit deferred this boot; nvidia-vgpud is held back until every selected GPU is prepared",
			      GS_DEFERRED);
		switch (nvidia_autoload(where, sizeof(where))) {
		case 1:
			check(&t, "WARN", "%s loads an nvidia module at boot; if it binds the PF before preparation, prepare refuses",
			      where);
			break;
		case 2:
			check(&t, "WARN", "%s puts nvidia into the initramfs configuration; if it binds the PF before preparation, prepare refuses",
			      where);
			break;
		default:
			check(&t, "INFO", "no nvidia module in modules-load.d, /etc/modules or initramfs-tools/dracut configuration (built initramfs images are not inspected)");
		}
		if (gs_find_program(cfg.sriov_manage, p, sizeof(p)) == 0)
			check(&t, "OK", "sriov-manage found at %s", cfg.sriov_manage);
		else if (o->driver_file)
			check(&t, "INFO", "%s not found: install takes it from %s when the package has it; with\n"
			      "       --accept-nvidia-license, NVIDIA's installer installs it", cfg.sriov_manage, o->driver_file);
		else
			check(&t, "WARN", "%s not found: `activate` needs it, from NVIDIA's vGPU host driver package. `geforce-sriov\n"
			      "       install --driver-file PATH --accept-nvidia-license` installs it", cfg.sriov_manage);
		{
			int vgpud = gs_unit_file_present("nvidia-vgpud.service");
			int mgr = gs_unit_file_present("nvidia-vgpu-mgr.service");

			if (vgpud && mgr)
				check(&t, "OK", "nvidia-vgpud.service and nvidia-vgpu-mgr.service unit files found");
			else if (o->driver_file)
				check(&t, "INFO", "%s not found in systemd's unit directories: install copies them from %s",
				      !vgpud && !mgr ? "nvidia-vgpud.service and nvidia-vgpu-mgr.service" :
				      !vgpud ? "nvidia-vgpud.service" : "nvidia-vgpu-mgr.service", o->driver_file);
			else
				check(&t, "WARN", "%s not found in systemd's unit directories; without NVIDIA's vGPU daemons no vGPU types appear.\n"
				      "       `geforce-sriov install --driver-file PATH` copies them from NVIDIA's package. By hand: copy\n"
				      "       them unchanged to /etc/systemd/system and run `systemctl daemon-reload`",
				      !vgpud && !mgr ? "nvidia-vgpud.service and nvidia-vgpu-mgr.service" :
				      !vgpud ? "nvidia-vgpud.service" : "nvidia-vgpu-mgr.service");
		}
		/* `activate`: sriov-manage needs lspci and setpci, found through the PATH gs_run gives it. */
		for (size_t k = 0; k < sizeof(deps) / sizeof(deps[0]); k++) {
			if (gs_find_program(deps[k], p, sizeof(p)) == 0)
				continue;
			deps_ok = 0;
			check(&t, "WARN", "%s (pciutils) not found: sriov-manage needs lspci and setpci, so `activate` refuses to enable VFs",
			      deps[k]);
		}
		if (deps_ok)
			check(&t, "OK", "lspci and setpci found (sriov-manage needs them for `activate`)");
		nvidia_version(GS_LIVE_PROC, ver, sizeof(ver));
		if (ver[0]) {
			char line[256];
			int known = 0;

			check(&t, gs_driver_version_line(ver, line, sizeof(line)) ? "WARN" : "OK", "%s", line);
			for (i = 0; i < GS_HOOK_DRIVER_VERSION_COUNT; i++)
				known |= strcmp(ver, gs_hook_driver_versions[i]) == 0;
			check(&t, known || !id_on ? "INFO" : "WARN",
			      "NVIDIA driver %s loaded%s", ver,
			      known ? "; the identity hook acts on this version" :
			      "; the identity hook passes this version through unchanged");
		}
	}
	if (activating)
		check(&t, "WARN", "%s exists: geforce-sriov activate is running or did not finish; check reports NOT READY until activate succeeds",
		      act);
	if (o->unit_dir || live)
		dropin_checks(&t, o->unit_dir);
	if (o->modprobe_dir || live)
		modprobe_checks(&t, o->modprobe_dir ? o->modprobe_dir : "/etc/modprobe.d", &cfg, o->hold_nvidia);
	if (o->plugin || live)
		plugin_checks(&t, o->plugin ? o->plugin : plugin_link(),
			      o->build && o->build->record ? o->build->record : GS_DRIVER_BUILD);
	{
		struct gs_build b;
		char line[BUILD_LINE_MAX], manifest[GS_PATH_MAX];
		const char *slash;

		if (module_identity(o->build, live, &b)) {
			/*
			 * No record and no install manifest beside it (GS_STATE_DIR): install has not run here, or
			 * uninstall removed it, and install runs the driver step. After `install --driver=skip` the
			 * manifest stays, and so does the usual line.
			 */
			slash = strrchr(b.record, '/');
			if (b.state == GS_BUILD_ABSENT && !b.record_read && slash &&
			    !gs_pathf(manifest, sizeof(manifest), "%.*s/" GS_MANIFEST_NAME, (int)(slash - b.record), b.record) &&
			    !gs_exists(manifest))
				check(&t, "INFO", "the driver step has not run on this host; install runs it");
			else
				check(&t, build_line(&b, line, sizeof(line)) ? "WARN" : "OK", "%s", line);
			if (loaded_line(sysroot, &b, line, sizeof(line)))
				check(&t, "WARN", "%s", line);
		}
	}

	if (n == 0)
		check(&t, "WARN", "no NVIDIA display function found (%s)", source);

	for (i = 0; i < n; i++) {
		struct gs_card c;
		struct gs_plan plan;
		struct gs_display disp;
		const struct gs_display *d;
		int ours, prepared = 0;

		printf("\n%s (%s)\n", devs[i].str, source);
		/*
		 * A laptop row's power state, read only: doctor writes nothing. A sysfs config read resumes a
		 * runtime-suspended device by itself, so prepare's config-read gate is a plain read here.
		 */
		{
			unsigned int dev;
			const struct gs_device *pe = pf_entry(sysroot, &devs[i], &dev);
			char dir[GS_PATH_MAX], bdir[GS_PATH_MAX], q[GS_PATH_MAX], gate[96], rt[32];
			char pc[16] = "?", d3[8] = "?", bpc[16] = "?";

			/*
			 * prepare refuses a laptop row without D3cold consent: in the config, or planned by --disable-d3cold
			 * (install writes it).
			 */
			if (pe && pe->laptop && o->disable_d3cold) {
				check(&t, "INFO", "%s: laptop GPU: D3cold consent planned by --disable-d3cold; install writes "
				      "d3cold=off, and prepare keeps this GPU powered", devs[i].str);
			} else if (pe && pe->laptop && cfg.d3cold_off) {
				check(&t, "INFO", "%s: laptop GPU: D3cold consent in the config (d3cold=off); prepare keeps this "
				      "GPU powered", devs[i].str);
			} else if (pe && pe->laptop) {
				char why[GS_D3COLD_WHY_SIZE];

				gs_d3cold_why(why, sizeof(why), &devs[i], 1, devs, n, cfg.boot_display_release, cfg.nrelease,
					      "       ");
				check(&t, "FAIL", "%s", why);
			}
			if (pe && pe->laptop && live && getuid() != 0) {
				check(&t, "INFO", "%s: laptop GPU: run doctor as root to check that it answers", devs[i].str);
			} else if (pe && pe->laptop && !gs_pathf(dir, sizeof(dir), "%s/bus/pci/devices/%s", sysroot, devs[i].str)) {
				pf_runtime_status(sysroot, &devs[i], rt, sizeof(rt));
				if (config_gate(dir, dev, gate, sizeof(gate))) {
					check(&t, "FAIL", "%s does not answer although its runtime_status is %s: %s (a laptop GPU that did "
					      "not come back from D3cold); prepare refuses this card. " WAKE_REMEDY, devs[i].str,
					      rt[0] ? rt : "absent", gate);
					continue;
				}
				if (!gs_pathf(q, sizeof(q), "%s/power/control", dir) && gs_read_attr(q, pc, sizeof(pc)))
					gs_copy(pc, sizeof(pc), "?");
				if (!gs_pathf(q, sizeof(q), "%s/d3cold_allowed", dir) && gs_read_attr(q, d3, sizeof(d3)))
					gs_copy(d3, sizeof(d3), "?");
				if (!pf_bridge_dir(sysroot, &devs[i], bdir, sizeof(bdir)) &&
				    !gs_pathf(q, sizeof(q), "%s/power/control", bdir) && gs_read_attr(q, bpc, sizeof(bpc)))
					gs_copy(bpc, sizeof(bpc), "?");
				check(&t, "INFO", "%s: laptop GPU answers (10de:%04x in D0): runtime_status %s, power/control %s, "
				      "d3cold_allowed %s, bridge power/control %s; prepare sets power/control and the bridge to on "
				      "and d3cold_allowed to 0", devs[i].str, dev, rt[0] ? rt : "absent", pc, d3, bpc);
			}
		}
		gs_card_inspect(sysroot, &devs[i], &c);
		ours = module_selects(&m, &c.pf);
		if (live && !gs_pathf(p, sizeof(p), "%s/%s", GS_PREPARED_DIR, c.pf.str))
			prepared = gs_exists(p);
		d = c.present ? granted_display(&cfg, sysroot, live ? GS_LIVE_PROC : NULL, &c, &disp) : NULL;
		if (c.present) {
			printf("  id 10de:%04x subsystem %04x:%04x class 0x%06x driver %s boot_vga %d iommu_group %s\n",
			       c.device, c.subvendor, c.subdevice, c.class_code, c.driver[0] ? c.driver : "none",
			       c.boot_vga, c.iommu_group[0] ? c.iommu_group : "none");
			if (c.entry)
				entry_checks(&t, &c, &cfg);
			if (c.entry && c.entry->laptop) {
				char lost[2][256];
				unsigned int nlost = laptop_lost(sysroot, o->run_dir ? o->run_dir : live ? GS_RUN_DIR : NULL, &c, &m,
								 lost);

				for (unsigned int k = 0; k < nlost; k++)
					check(&t, "WARN", "%s", lost[k]);
			}
			if (c.bridge_is_pci)
				printf("  bridge %s power/control=%s runtime_status=%s ARI forwarding supported=%s\n",
				       c.bridge.str, c.bridge_pm[0] ? c.bridge_pm : "?", c.bridge_rt[0] ? c.bridge_rt : "?",
				       c.bridge_ari_fwd_supported < 0 ? "unknown" :
				       c.bridge_ari_fwd_supported ? "yes" : "no");
			if (c.has_sriov)
				printf("  sriov_totalvfs=%u sriov_numvfs=%u ari_enabled=%d\n", c.totalvfs, c.numvfs,
				       c.ari_enabled);
			if (c.bridge_ari_fwd_supported == 0 && cfg.ari != GS_SW_OFF)
				check(&t, "WARN", "%s: the bridge does not support ARI forwarding; ari=on has no effect there",
				      c.pf.str);
			if (c.bar0_assigned == 0)
				check(&t, "WARN", "%s: BAR0 is not assigned, so the card is unusable this boot. Do not rescan again; " BAR_REMEDY,
				      c.pf.str);
			if (c.has_sriov && c.vf_bars_assigned == 0)
				check(&t, "WARN", "%s: no VF BAR is assigned: enabling VFs fails with \"not enough MMIO resources for SR-IOV\"; " BAR_REMEDY,
				      c.pf.str);
			iommu_checks(&t, sysroot, &c);
			{
				char vf1[GS_BDF_STR_SIZE];
				unsigned int nshare = vfs_sharing_pf_group(sysroot, &c, vf1, sizeof(vf1));

				if (nshare)
					check(&t, "WARN", "%s: %u enabled VF(s), first %s, share IOMMU group %s with the PF: they are not isolated from the PF, and VFIO may refuse them",
					      c.pf.str, nshare, vf1, c.iommu_group);
			}
			if (d) {
				printf("  console: %s%s%s%s, %u fb/DRM device(s) on it\n",
				       d->fw[0] ? d->fw : "no firmware framebuffer", d->fw[0] ? " (" : "",
				       d->fw[0] ? (d->fw_driver[0] ? d->fw_driver : "unbound: released") : "",
				       d->fw[0] ? ")" : "", d->nconsumers);
				if (!d->proc_checked)
					check(&t, "INFO", "%s: /proc/iomem and /proc/consoles are checked on the live system only (prepare checks them again)",
					      c.pf.str);
			}
			if (live && prepared) {
				struct gs_prepared rec;

				gs_prepared_read(GS_RUN_DIR, &c.pf, &rec);
				if (rec.console_release[0])
					printf("  console released this boot: %s\n", rec.console_release);
			}
		}
		gs_plan_prepare(&c, d, &plan);
		if (plan.verdict == GS_PLAN_READY && d) {
			check(&t, "OK", "%s: boot display: `prepare --release-console=%s` would %s, then remove and rescan this card%s",
			      c.pf.str, c.pf.str,
			      plan.release[0] ? "release its console (unbind the firmware framebuffer) before the module loads" :
			      d->fw[0] ? "find its firmware framebuffer already released" :
			      "find no firmware framebuffer to release",
			      plan.nunbind ? " after unbinding its HDMI audio function" : "");
		} else if (plan.verdict == GS_PLAN_READY) {
			check(&t, "OK", "%s: prepare would remove and rescan this card%s", c.pf.str,
			      plan.nunbind ? " after unbinding its HDMI audio function" : "");
		} else if (c.present && c.entry && gs_check_presented(&c, &cfg, err, sizeof(err)) == 0 && ours &&
			   m.lp_ok) {
			check(&t, "OK", "%s: SR-IOV presented by %s (sriov_totalvfs=%u, %u VF(s) enabled)%s; prepare has nothing left to do this boot",
			      c.pf.str, GS_MODULE_NAME, c.totalvfs, c.numvfs, prepared ? ", prepared this boot" : "");
		} else if (c.present && c.driver[0]) {
			/* The usual state on a running host: judge the card as the boot unit will see it. */
			struct gs_card at_boot = c;
			struct gs_plan boot_plan;

			at_boot.driver[0] = '\0';
			for (unsigned int k = 0; k < at_boot.nfns; k++)
				at_boot.fns[k].driver[0] = '\0';
			/* VFs and a presented capability do not exist yet at boot. */
			at_boot.numvfs = 0;
			at_boot.nvfs_on_bus = 0;
			at_boot.nothers = 0;
			for (unsigned int k = 0; k < c.nothers; k++)
				if (!c.others[k].is_vf)
					at_boot.others[at_boot.nothers++] = c.others[k];
			/* Only this module's own presented capability is absent at boot; another kernel's is not. */
			if (at_boot.entry && ours)
				at_boot.has_sriov = 0;
			/* Firmware assigns BAR0 again at boot. */
			at_boot.bar0_assigned = -1;
			gs_plan_prepare(&at_boot, d, &boot_plan);
			if (boot_plan.verdict == GS_PLAN_READY && cfg.nrelease)
				check(&t, "WARN", "%s: bound to %s, so prepare cannot act on it this boot. After the next (deferred) boot, run prepare by hand before anything loads nvidia",
				      c.pf.str, c.driver);
			else if (boot_plan.verdict == GS_PLAN_READY)
				check(&t, "WARN", "%s: bound to %s, so prepare cannot act on it this boot. At the next boot the unit prepares it before NVIDIA binds, if nothing loads nvidia earlier",
				      c.pf.str, c.driver);
			else
				check(&t, "FAIL", "%s", boot_plan.reason);
		} else {
			check(&t, "FAIL", "%s", plan.reason);
		}
	}
	if (nlaptops)
		laptop_host_checks(&t, o, live, laptops, nlaptops);
	{
		char ada[GS_BDF_STR_SIZE], other[GS_BDF_STR_SIZE];
		unsigned int gen = 0;

		if (selection_mixes_ada(sysroot, devs, n, ada, other, sizeof(ada), &gen))
			check(&t, "WARN", "selection mixes Ada (%s) with %s (%s): one host module drives both generations, "
			      "with %u VFs per card, and one vGPU plugin copy serves both",
			      ada, gs_generation_name(gen), other, gs_config_vfs(&cfg));
	}
	if (nisel && !id_on) {
		check(&t, "INFO", "identity: off (%s): the hook passes every call through, so nvidia-vgpud sees the GeForce IDs and registers no vGPU types for them",
		      isrc);
	} else if (nisel) {
		printf("\nidentity (%s)\n", isrc);
		identity_checks(&t, sysroot, isel, nisel, id_force);
	}
	printf("\n%u failure(s), %u warning(s)\n", t.fail, t.warn);
	return t.fail ? GS_EXIT_REFUSED : GS_EXIT_OK;
}

/* ---- status ------------------------------------------------------------- */

static void print_vfs(const char *sysroot, const struct gs_card *c)
{
	char p[GS_PATH_MAX], target[GS_PATH_MAX];
	unsigned int i;

	for (i = 0; i < c->numvfs && i < 256; i++) {
		struct gs_addr a;
		struct gs_card vf;
		ssize_t n;
		const char *base;

		if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/virtfn%u", sysroot, c->pf.str, i))
			break;
		n = readlink(p, target, sizeof(target) - 1);
		if (n <= 0) {
			printf("  virtfn%u: missing\n", i);
			continue;
		}
		target[n] = '\0';
		base = strrchr(target, '/');
		base = base ? base + 1 : target;
		if (gs_addr_parse(base, &a)) {
			printf("  virtfn%u: %s\n", i, base);
			continue;
		}
		gs_card_inspect(sysroot, &a, &vf);
		printf("  virtfn%u: %s driver %s iommu_group %s\n", i, a.str, vf.driver[0] ? vf.driver : "none",
		       vf.iommu_group[0] ? vf.iommu_group : "none");
		if (vf.iommu_group[0] && strcmp(vf.iommu_group, c->iommu_group) == 0)
			printf("  warning: %s shares IOMMU group %s with its PF: it is not isolated from the PF, and VFIO may refuse it\n",
			       a.str, c->iommu_group);
	}
}

/* A card that does not present SR-IOV: why, and what to do next. */
static void status_next(const struct gs_card *c, const struct gs_config *cfg, const char *cfg_path,
			const struct module_state *m, int live)
{
	if (!c->entry) {
		printf("  not in the device table: geforce-sriov does not act on it\n");
		return;
	}
	if (c->boot_vga == 1 && (!cfg_path || !gs_config_grants_release(cfg, c->pf.str))) {
		printf("  boot display device: preparing it releases its Linux console at each prepared boot\n"
		       "  (docs/installation.md#boot-display-card). If you accept that, next:\n"
		       "  `sudo geforce-sriov install --device %s --boot-display-release %s` (add --device for each other GPU)\n",
		       c->pf.str, c->pf.str);
		return;
	}
	if (!cfg_path || !addr_listed(cfg->bdfs, cfg->nbdfs, c->pf.str)) {
		printf("  not selected for preparation at boot. Next: `geforce-sriov doctor --device %s`, then\n"
		       "  `sudo geforce-sriov install --device %s` (one --device per GPU), then power the machine off and on\n",
		       c->pf.str, c->pf.str);
		return;
	}
	if (live && module_selects(m, &c->pf)) {
		printf("  %s was loaded for it this boot, but SR-IOV is not presented. See `journalctl -b -u %s`\n"
		       "  (or the output of `geforce-sriov prepare`) and `geforce-sriov doctor`\n",
		       GS_MODULE_NAME, GS_UNIT_PREPARE);
		return;
	}
	if (cfg->nrelease) {
		printf("  deferred: with boot_display_release in %s, the boot unit prepares none of the selected GPUs.\n"
		       "  " GS_RELEASE_STEPS_LEAD "\n", cfg_path);
		gs_release_steps(stdout, "    ", cfg, 1);
		return;
	}
	printf("  expected until a prepared boot: %s selects it, and %s presents SR-IOV at boot, before NVIDIA\n"
	       "  binds the card. Next: run `geforce-sriov doctor`, then power the machine off and on\n",
	       cfg_path, GS_UNIT_PREPARE);
}

int gs_cmd_status(const struct gs_diag_opts *o)
{
	const char *sysroot = o->sysroot ? o->sysroot : GS_LIVE_SYSFS;
	int live = is_live_root(sysroot), all_ok = 1, stale = 0;
	unsigned int ready = 0;
	struct gs_addr devs[GS_CFG_MAX_BDFS];
	struct module_state m;
	struct gs_config cfg;
	const char *cfg_path, *source;
	char err[512], build[768] = "", loaded[256], act[GS_PATH_MAX];
	unsigned int n, i;
	struct gs_build b;

	if (diag_config(o, &cfg, &cfg_path, err, sizeof(err))) {
		fprintf(stderr, "geforce-sriov: config: %s\n", err);
		gs_config_defaults(&cfg);
		all_ok = 0;
	}
	/* The driver step's build, on the line of each Ada or Ampere card (the cards whose stack it builds). */
	if (module_identity(o->build, live, &b)) {
		if (b.state == GS_BUILD_MATCHES)
			snprintf(build, sizeof(build), "; nvidia.ko for %.100s: the driver step's build", b.release);
		else if (b.state == GS_BUILD_PRESENT)
			snprintf(build, sizeof(build), "; nvidia.ko for %.100s: not the driver step's build (build-id %s, recorded %s), rerun the driver step",
				 b.release, b.found_id[0] ? b.found_id : "unreadable", b.rec_id[0] ? b.rec_id : "none");
		else
			snprintf(build, sizeof(build), "; nvidia.ko for %.100s: no driver step record for this kernel, rerun the driver step",
				 b.release);
		if (loaded_line(sysroot, &b, loaded, sizeof(loaded))) {
			snprintf(build + strlen(build), sizeof(build) - strlen(build), "; %s", loaded);
			stale = 1;
		}
	}
	read_module_state(sysroot, &m);
	printf("module %s: %s", GS_MODULE_NAME, m.loaded ? "loaded" : "not loaded");
	if (m.loaded)
		printf(" (ari=%s msix=%s vfs=%s bdfs=%s; livepatch enabled=%u transition=%u)",
		       m.ari, m.msix, m.vfs[0] ? m.vfs : "(none)", m.bdfs[0] ? m.bdfs : "(none)",
		       m.lp_enabled, m.lp_transition);
	putchar('\n');
	if (live) {
		printf("guard: %s\n", gs_exists(GS_ATTEMPT_MARKER) ?
		       "SET - a preparation attempt did not finish (see `geforce-sriov doctor`)" : "clear");
		if (gs_exists(GS_ATTEMPT_MARKER))
			all_ok = 0;
		if (gs_exists(GS_DEFERRED)) {
			printf("boot unit: deferred this boot (boot_display_release); nvidia-vgpud is held back until every selected GPU is prepared\n");
			all_ok = 0;
		}
	}
	/*
	 * The loaded module lacks what the installed build has: the power cycle is the next step, status
	 * is not ready, and the next steps that run activate are left out here and on each card (activate
	 * would run on that module).
	 */
	if (stale) {
		printf("next: power the machine off and on (the running nvidia module is not the installed one)\n");
		all_ok = 0;
	}
	if (activating_left(o, live, act, sizeof(act))) {
		printf("activate: %s exists (activate is running or did not finish)\n", act);
		if (!stale)
			printf("next: stop every VM that uses a vGPU, then run `geforce-sriov activate`\n");
		all_ok = 0;
	}
	n = selected_devices(o, &cfg, sysroot, devs, &source);
	if (n == 0) {
		printf("no NVIDIA display function selected or found (%s)\n"
		       "next: select a GPU with `sudo geforce-sriov install --device BDF`; `geforce-sriov devices` lists the supported IDs\n",
		       source);
		return GS_EXIT_FAIL;
	}
	for (i = 0; i < n; i++) {
		struct gs_card c;
		char p[GS_PATH_MAX], first[256];
		unsigned int nothers;
		int prepared = 0, with_bridge;

		gs_card_inspect(sysroot, &devs[i], &c);
		printf("\n%s", devs[i].str);
		if (!c.present) {
			printf(": absent\n  check the address: `geforce-sriov doctor` lists every NVIDIA GPU it finds\n");
			all_ok = 0;
			continue;
		}
		printf(": 10de:%04x %s driver %s%s\n", c.device, c.entry ? c.entry->name : "(not in device table)",
		       c.driver[0] ? c.driver : "none", c.entry ? build : "");
		if (c.entry) {
			char vfs[128];

			gs_vfs_describe(c.entry, &cfg, vfs, sizeof(vfs));
			printf("  VFs: %s\n", vfs);
		}
		if (live && !gs_pathf(p, sizeof(p), "%s/%s", GS_PREPARED_DIR, c.pf.str)) {
			struct gs_prepared rec;

			prepared = gs_exists(p);
			printf("  prepared this boot: %s\n", prepared ? "yes" : "no");
			gs_prepared_read(GS_RUN_DIR, &c.pf, &rec);
			if (rec.console_release[0])
				printf("  console released this boot: %s\n", rec.console_release);
		}
		if (iommu_peers(sysroot, &c, &with_bridge, first, sizeof(first), &nothers) == 0 && nothers)
			printf("  warning: IOMMU group %s also holds %u other device(s), first %s: VFs may share it (see `geforce-sriov doctor`)\n",
			       c.iommu_group, nothers, first);
		if (c.has_sriov) {
			printf("  SR-IOV: sriov_totalvfs=%u sriov_numvfs=%u ari_enabled=%d\n", c.totalvfs, c.numvfs,
			       c.ari_enabled);
			if (gs_check_presented(&c, &cfg, err, sizeof(err))) {
				printf("  %s: do not enable VFs\n", err);
				all_ok = 0;
			} else if (c.bar0_assigned == 0) {
				printf("  BAR0 is not assigned: the card is unusable this boot; see `geforce-sriov doctor`\n");
				all_ok = 0;
			} else if (c.vf_bars_assigned == 0) {
				printf("  no VF BAR is assigned: enabling VFs would fail; see `geforce-sriov doctor`\n");
				all_ok = 0;
			} else if (live && !prepared) {
				printf("  presented, but not prepared this boot; `activate` acts only on GPUs prepared this boot\n");
				all_ok = 0;
			} else {
				ready++;
				if (c.bar0_assigned < 0 || c.vf_bars_assigned < 0)
					printf("  BAR assignment: not confirmed (resource file unreadable or incomplete)\n");
				/* With the loaded module not the installed build, the power cycle above is the next step. */
				if (live && !stale && c.numvfs == 0 && strcmp(c.driver, "nvidia") != 0)
					printf("  next: bind the PF to NVIDIA's vGPU host driver (it is bound to %s), then run `sudo geforce-sriov activate`\n",
					       c.driver[0] ? c.driver : "nothing");
				else if (live && !stale && c.numvfs == 0)
					printf("  next: `sudo geforce-sriov activate` (it enables the VFs and checks sriov-manage and pciutils)\n");
			}
			print_vfs(sysroot, &c);
		} else {
			printf("  SR-IOV: not presented\n");
			status_next(&c, &cfg, cfg_path, &m, live);
			all_ok = 0;
		}
	}
	if (ready < n)
		printf("\n%u of %u selected GPU(s) present SR-IOV as prepared (1..%u VFs, no BAR known to be unassigned%s).\n"
		       "status exits 1 until all do (expected before the first prepared boot); each card above names its next step\n",
		       ready, n, gs_config_vfs(&cfg), live ? ", prepared this boot" : "");
	return all_ok ? GS_EXIT_OK : GS_EXIT_FAIL;
}

/* ---- prepare (live only) ------------------------------------------------- */

#define SYSDEV GS_LIVE_SYSFS "/bus/pci/devices"

/*
 * Root's configuration: opened once without following links, checked through the same
 * descriptor (owner, mode, parents) and parsed from it, so no check/use gap exists.
 */
static int load_live_config(struct gs_config *cfg)
{
	const char *path = gs_config_path();
	char err[512] = "cannot stat the open file", *buf;
	struct stat st;
	size_t off = 0;
	int fd, r;

	if (path[0] != '/') {
		fprintf(stderr, "geforce-sriov: refusing config %s: $%s must be an absolute path\n", path, GS_CONFIG_ENV);
		return GS_EXIT_REFUSED;
	}
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		if (errno == ENOENT)
			fprintf(stderr, "geforce-sriov: %s does not exist; select devices with `geforce-sriov install --device BDF`\n",
				path);
		else
			fprintf(stderr, "geforce-sriov: refusing config %s: %s\n", path, strerror(errno));
		return GS_EXIT_REFUSED;
	}
	if (fstat(fd, &st) < 0 || stat_safe(path, &st, err, sizeof(err))) {
		fprintf(stderr, "geforce-sriov: refusing config: %s\n", err);
		close(fd);
		return GS_EXIT_REFUSED;
	}
	buf = malloc(GS_CONFIG_MAX_BYTES + 2);
	if (!buf) {
		close(fd);
		return GS_EXIT_FAIL;
	}
	for (;;) {
		ssize_t n = read(fd, buf + off, GS_CONFIG_MAX_BYTES + 1 - off);

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			fprintf(stderr, "geforce-sriov: refusing config %s: read failed: %s\n", path, strerror(errno));
			free(buf);
			close(fd);
			return GS_EXIT_REFUSED;
		}
		if (n == 0)
			break;
		off += (size_t)n;
		if (off > GS_CONFIG_MAX_BYTES)
			break;
	}
	close(fd);
	buf[off] = '\0';
	r = gs_config_parse(buf, off, cfg, err, sizeof(err));
	free(buf);
	if (r) {
		fprintf(stderr, "geforce-sriov: config: %s: %s\n", path, err);
		return GS_EXIT_REFUSED;
	}
	if (cfg->nbdfs == 0) {
		fprintf(stderr, "geforce-sriov: no device selected (bdfs= in %s), so nothing is done\n",
			path);
		return GS_EXIT_REFUSED;
	}
	return 0;
}

static int require_root(const char *cmd)
{
	if (geteuid() != 0) {
		fprintf(stderr, "geforce-sriov: %s must run as root\n", cmd);
		return GS_EXIT_REFUSED;
	}
	return 0;
}

const struct gs_host gs_host_live = {
	GS_LIVE_SYSFS, GS_LIVE_PROC, GS_STATE_DIR, GS_RUN_DIR, gs_write_attr, gs_run, gs_run_capture,
};

static int write_marker(const struct gs_host *h, const char *bdfs, const char *step)
{
	char boot[64] = "unknown", buf[1024], path[GS_PATH_MAX];
	int n, r;

	gs_boot_id(boot, sizeof(boot));
	n = snprintf(buf, sizeof(buf), "boot_id=%s\ntime=%lld\nbdfs=%s\nstep=%s\n", boot, (long long)time(NULL),
		     bdfs, step);
	if (n < 0 || (size_t)n >= sizeof(buf))
		return -EOVERFLOW;
	if (gs_pathf(path, sizeof(path), "%s/" GS_ATTEMPT_NAME, h->state_dir))
		return -ENAMETOOLONG;
	r = gs_mkdir_p(h->state_dir, 0755);
	if (r)
		return r;
	return gs_write_file_atomic(path, buf, (size_t)n, 0644);
}

static int clear_marker(const struct gs_host *h)
{
	char path[GS_PATH_MAX];

	if (gs_pathf(path, sizeof(path), "%s/" GS_ATTEMPT_NAME, h->state_dir))
		return -ENAMETOOLONG;
	if (unlink(path) < 0 && errno != ENOENT)
		return -errno;
	return gs_fsync_dir(h->state_dir);
}

static int clear_guard(void)
{
	char *data, boot[64] = "", *line, *save = NULL;
	size_t len;
	int r;

	if (gs_read_whole(GS_ATTEMPT_MARKER, &data, &len, 4096)) {
		printf("guard %s is not set\n", GS_ATTEMPT_MARKER);
		return GS_EXIT_OK;
	}
	printf("guard %s:\n%s", GS_ATTEMPT_MARKER, data);
	gs_boot_id(boot, sizeof(boot));
	for (line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (strncmp(line, "boot_id=", 8) == 0 && boot[0] && strcmp(line + 8, boot) == 0) {
			free(data);
			fprintf(stderr, "geforce-sriov: the unfinished attempt is from this boot, so the guard stays.\n%s\n",
				recovery_msg);
			return GS_EXIT_COLD_CYCLE;
		}
	}
	free(data);
	r = clear_marker(&gs_host_live);
	if (r) {
		fprintf(stderr, "geforce-sriov: cannot remove %s: %s\n", GS_ATTEMPT_MARKER, strerror(-r));
		return GS_EXIT_FAIL;
	}
	printf("guard cleared. The boot ID changed, but geforce-sriov cannot tell whether the machine was powered off and on. The next boot runs preparation again\n");
	return GS_EXIT_OK;
}

static int wait_attr_eq(const char *path, const char *want, unsigned int ms)
{
	char buf[64];

	for (unsigned int t = 0;; t += POLL_MS) {
		if (gs_read_attr(path, buf, sizeof(buf)) == 0 && strcmp(buf, want) == 0)
			return 0;
		if (t >= ms)
			return -1;
		gs_sleep_ms(POLL_MS);
	}
}

static int wait_exists(const char *path, int want, unsigned int ms)
{
	for (unsigned int t = 0;; t += POLL_MS) {
		if (gs_exists(path) == want)
			return 0;
		if (t >= ms)
			return -1;
		gs_sleep_ms(POLL_MS);
	}
}

static int wait_livepatch(const struct gs_host *h)
{
	char en[GS_PATH_MAX], tr[GS_PATH_MAX];
	unsigned int e, t;

	gs_pathf(en, sizeof(en), "%s/kernel/livepatch/%s/enabled", h->sys, GS_MODULE_NAME);
	gs_pathf(tr, sizeof(tr), "%s/kernel/livepatch/%s/transition", h->sys, GS_MODULE_NAME);
	for (unsigned int ms = 0;; ms += POLL_MS) {
		if (gs_read_uint(en, &e) == 0 && gs_read_uint(tr, &t) == 0 && e == 1 && t == 0)
			return 0;
		if (ms >= LIVEPATCH_WAIT_MS)
			return -1;
		gs_sleep_ms(POLL_MS);
	}
}

/* 1 when the kernel command line carries pci=...noari... (the module cannot see it). */
static int cmdline_noari(const char *procroot)
{
	char *data, *tok, *save = NULL, p[GS_PATH_MAX];
	size_t len;
	int found = 0;

	if (gs_pathf(p, sizeof(p), "%s/cmdline", procroot) || gs_read_whole(p, &data, &len, 1 << 16))
		return 0;
	for (tok = strtok_r(data, " \t\n", &save); tok && !found; tok = strtok_r(NULL, " \t\n", &save)) {
		char *opt, *save2 = NULL;

		if (strncmp(tok, "pci=", 4) != 0)
			continue;
		for (opt = strtok_r(tok + 4, ",", &save2); opt; opt = strtok_r(NULL, ",", &save2))
			if (strcmp(opt, "noari") == 0)
				found = 1;
	}
	free(data);
	return found;
}

static const char *yn(int v)
{
	return v == GS_SW_OFF ? "N" : "Y";
}

static int load_module(const struct gs_host *h, const struct gs_config *cfg, const char *bdfs)
{
	struct module_state m;
	char a[16], x[16], v[16], b[256];
	int r;

	read_module_state(h->sys, &m);
	if (m.loaded) {
		if (strcmp(m.ari, yn(cfg->ari)) || strcmp(m.msix, yn(cfg->msix)) ||
		    module_vfs(&m) != gs_config_vfs(cfg) || strcmp(m.bdfs, bdfs)) {
			fprintf(stderr,
				"geforce-sriov: %s is already loaded with ari=%s msix=%s vfs=%u bdfs=%s, not the requested ari=%s msix=%s vfs=%u bdfs=%s; power the machine off and on to change them\n",
				GS_MODULE_NAME, m.ari, m.msix, module_vfs(&m), m.bdfs, yn(cfg->ari),
				yn(cfg->msix), gs_config_vfs(cfg), bdfs);
			return GS_EXIT_REFUSED;
		}
		printf("%s already loaded with the requested parameters\n", GS_MODULE_NAME);
	} else {
		/* vfs=N only when the config sets it; the module's default is GS_DEFAULT_VFS. */
		const char *argv[] = { "modprobe", GS_MODULE_NAME, a, x, cfg->vfs ? v : b, cfg->vfs ? b : NULL, NULL };

		snprintf(a, sizeof(a), "ari=%d", cfg->ari == GS_SW_OFF ? 0 : 1);
		snprintf(x, sizeof(x), "msix=%d", cfg->msix == GS_SW_OFF ? 0 : 1);
		snprintf(v, sizeof(v), "vfs=%u", cfg->vfs);
		if (gs_pathf(b, sizeof(b), "bdfs=%s", bdfs))
			return GS_EXIT_FAIL;
		printf("modprobe");
		for (size_t k = 1; argv[k]; k++)
			printf(" %s", argv[k]);
		putchar('\n');
		r = h->run(argv);
		if (r != 0) {
			fprintf(stderr,
				"geforce-sriov: modprobe %s failed (%d); nothing was removed. `journalctl -k -b` names the\n"
				"refused GPU and the reason\n",
				GS_MODULE_NAME, r);
			return GS_EXIT_FAIL;
		}
	}
	if (wait_livepatch(h)) {
		fprintf(stderr, "geforce-sriov: livepatch %s did not reach enabled=1 transition=0 within %u ms; nothing was removed\n",
			GS_MODULE_NAME, LIVEPATCH_WAIT_MS);
		return GS_EXIT_FAIL;
	}
	printf("livepatch %s enabled, transition complete\n", GS_MODULE_NAME);
	return 0;
}

enum card_result { CARD_DONE, CARD_REFUSED, CARD_FAILED_BACK, CARD_LOST };

static void restore_pm(const struct gs_host *h, const struct gs_card *c, const char *old)
{
	char p[GS_PATH_MAX];

	/* A laptop GPU's bridge stays on for the whole session (wake_card()). */
	if (c->entry && c->entry->laptop)
		return;

	if (strcmp(old, "on") == 0 || gs_pathf(p, sizeof(p), "%s/power/control", c->bridge_dir))
		return;
	if (h->write_attr(p, old))
		fprintf(stderr, "geforce-sriov: %s: restoring power/control=%s failed\n", c->bridge.str, old);
	else
		printf("%s: bridge power/control restored to %s\n", c->bridge.str, old);
}

/*
 * After a refusal that follows an audio unbind: give each function back to snd_hda_intel,
 * but only while it is still present, unbound and the same device. Otherwise say what is
 * left; nothing is rescanned or reset.
 */
static void rebind_audio(const struct gs_host *h, const struct gs_fn *fns, unsigned int n)
{
	char p[GS_PATH_MAX];

	gs_pathf(p, sizeof(p), "%s/bus/pci/drivers/snd_hda_intel/bind", h->sys);
	for (unsigned int i = 0; i < n; i++) {
		struct gs_card now;
		int r;

		gs_card_inspect(h->sys, &fns[i].bdf, &now);
		if (!now.present || now.driver[0] || now.vendor != fns[i].vendor || now.device != fns[i].device) {
			fprintf(stderr, "geforce-sriov: residual state: audio function %s was unbound and is now %s; it was not rebound\n",
				fns[i].bdf.str, !now.present ? "missing" : now.driver[0] ? "bound elsewhere" : "a different device");
			continue;
		}
		r = h->write_attr(p, fns[i].bdf.str);
		if (r)
			fprintf(stderr, "geforce-sriov: residual state: rebinding audio function %s to snd_hda_intel failed: %s\n",
				fns[i].bdf.str, strerror(-r));
		else
			printf("%s: audio function rebound to snd_hda_intel\n", fns[i].bdf.str);
	}
}

/*
 * The console of a boot display PF that this run prepares. The firmware framebuffer lives in a
 * BAR of the PF; once the PF's removal has been attempted the BARs may move, so it is never
 * rebound after that, whatever the remove write returned.
 */
struct console {
	struct gs_addr pf;
	char fw[64];               /* the PF's simple-framebuffer.N; "" when it has none */
	char driver[64];           /* its driver before the release; "" when nothing is to release */
	char fw_dir[PATH_MAX];     /* resolved device directory of fw */
	char pf_dir[PATH_MAX];     /* resolved PF directory at release */
	char resource[4096];       /* <pf>/resource at release */
	int released;              /* this run wrote the unbind (whatever it returned) */
	int removal_attempted;     /* the card's remove was attempted: never rebind */
};

/* The display state for a plan: NULL unless the config grants the card a console release. */
static const struct gs_display *granted_display(const struct gs_config *cfg, const char *sys, const char *proc,
						const struct gs_card *c, struct gs_display *buf)
{
	if (!gs_config_grants_release(cfg, c->pf.str))
		return NULL;
	gs_display_inspect(sys, proc, c, buf);
	return buf;
}

static int read_pf_identity(const struct gs_host *h, const struct gs_addr *pf, char *dir, size_t dlen,
			    char *res, size_t rlen)
{
	char p[GS_PATH_MAX], real[PATH_MAX];

	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s", h->sys, pf->str) || !realpath(p, real) ||
	    gs_pathf(p, sizeof(p), "%s/resource", real) || gs_read_attr(p, res, rlen))
		return -1;
	gs_copy(dir, dlen, real);
	return 0;
}

/*
 * Unbind the PF's firmware framebuffer from its driver and confirm that nothing of the card is
 * left on the console. The guard is written first. -1: refuse before any removal.
 */
static int release_console(const struct gs_host *h, const char *bdfs, struct console *k)
{
	char p[GS_PATH_MAX], step[64];
	struct gs_display d;
	struct gs_card c;
	int r;

	snprintf(step, sizeof(step), "release-console:%s", k->pf.str);
	if ((r = write_marker(h, bdfs, step))) {
		fprintf(stderr, "geforce-sriov: cannot write the guard: %s; nothing changed\n", strerror(-r));
		return -1;
	}
	if (read_pf_identity(h, &k->pf, k->pf_dir, sizeof(k->pf_dir), k->resource, sizeof(k->resource))) {
		fprintf(stderr, "geforce-sriov: refused: %s: its directory or resource file cannot be read; the console is left alone\n",
			k->pf.str);
		return -1;
	}
	gs_pathf(p, sizeof(p), "%s/bus/platform/devices/%s/driver/unbind", h->sys, k->fw);
	k->released = 1;
	r = h->write_attr(p, k->fw);
	if (r) {
		fprintf(stderr, "geforce-sriov: refused: %s: unbinding %s from %s failed: %s\n", k->pf.str, k->fw,
			k->driver, strerror(-r));
		return -1;
	}
	printf("%s: console released: %s unbound from %s\n", k->pf.str, k->fw, k->driver);
	/* Its fb, DRM device and memory region go with the driver; allow the teardown a moment. */
	for (unsigned int t = 0;; t += POLL_MS) {
		gs_card_inspect(h->sys, &k->pf, &c);
		gs_display_inspect(h->sys, h->proc, &c, &d);
		if (strcmp(d.fw, k->fw) == 0 && gs_display_fw_state(&d) == GS_FW_RELEASED && !d.refuse[0])
			return 0;
		if (t >= RELEASE_WAIT_MS)
			break;
		gs_sleep_ms(POLL_MS);
	}
	fprintf(stderr, "geforce-sriov: refused before removal: %s: the console is not released: %s\n", k->pf.str,
		d.refuse[0] ? d.refuse : strcmp(d.fw, k->fw) ? "the firmware framebuffer changed" :
		"its firmware framebuffer is still bound");
	return -1;
}

/*
 * Give a released console back, only before the card's removal was attempted and only while the
 * PF is the same unbound device with the same resources and the framebuffer is the same unbound
 * child of it. Anything else stays as it is until the next boot.
 */
static void rebind_console(const struct gs_host *h, const struct console *k)
{
	char p[GS_PATH_MAX], dir[PATH_MAX], res[4096], fw[PATH_MAX];
	const char *why = NULL;
	struct gs_card c;
	int r;

	if (!k->released)
		return;
	if (k->removal_attempted) {
		printf("%s: the console stays released for this boot: the card's removal was attempted, so its BARs may have moved\n",
		       k->pf.str);
		return;
	}
	gs_card_inspect(h->sys, &k->pf, &c);
	gs_pathf(p, sizeof(p), "%s/bus/platform/devices/%s", h->sys, k->fw);
	/* A failed unbind may have left it bound: then nothing was released and nothing is restored. */
	if (realpath(p, fw) && strcmp(fw, k->fw_dir) == 0 && !gs_pathf(p, sizeof(p), "%s/driver", fw) &&
	    gs_exists(p)) {
		printf("%s: the console is still bound to its firmware framebuffer; nothing to restore\n", k->pf.str);
		return;
	}
	gs_pathf(p, sizeof(p), "%s/bus/platform/devices/%s", h->sys, k->fw);
	if (!c.present || read_pf_identity(h, &k->pf, dir, sizeof(dir), res, sizeof(res)))
		why = "the card cannot be read";
	else if (c.driver[0])
		why = "the card is now bound to a driver";
	else if (strcmp(dir, k->pf_dir) != 0 || strcmp(res, k->resource) != 0)
		why = "the card's device or resources changed";
	else if (!realpath(p, fw) || strcmp(fw, k->fw_dir) != 0)
		why = "its firmware framebuffer changed";
	if (why) {
		fprintf(stderr, "geforce-sriov: residual state: the console of %s stays released (%s); it returns at the next boot\n",
			k->pf.str, why);
		return;
	}
	gs_pathf(p, sizeof(p), "%s/bus/platform/drivers/%s/bind", h->sys, k->driver);
	r = h->write_attr(p, k->fw);
	if (r)
		fprintf(stderr, "geforce-sriov: residual state: rebinding %s to %s failed: %s; the console returns at the next boot\n",
			k->fw, k->driver, strerror(-r));
	else
		printf("%s: console restored: %s rebound to %s\n", k->pf.str, k->fw, k->driver);
}

static void rebind_consoles(const struct gs_host *h, const struct console *k, unsigned int n)
{
	for (unsigned int i = 0; i < n; i++)
		rebind_console(h, &k[i]);
}

static enum card_result prepare_card(const struct gs_host *h, const struct gs_addr *pf, const char *bdfs,
				     const struct gs_config *cfg, struct console *k)
{
	struct gs_card c, after;
	struct gs_plan plan;
	struct gs_display disp;
	char p[GS_PATH_MAX], pm_old[16], step[64], pfdir[GS_PATH_MAX], boot[64] = "unknown", rec[256];
	unsigned int i, vendor = 0, device = 0, nunbound = 0;
	struct gs_fn unbound[GS_PLAN_MAX_UNBIND];
	sigset_t block, saved;
	int r;

	gs_card_inspect(h->sys, pf, &c);
	gs_plan_prepare(&c, granted_display(cfg, h->sys, h->proc, &c, &disp), &plan);
	if (plan.verdict != GS_PLAN_READY || plan.release[0]) {
		fprintf(stderr, "geforce-sriov: refused: %s\n",
			plan.verdict != GS_PLAN_READY ? plan.reason : "the boot display's console is bound");
		return CARD_REFUSED;
	}

	/* Hold the upstream bridge awake for the whole remove/rescan/return sequence. */
	snprintf(pm_old, sizeof(pm_old), "%s", c.bridge_pm);
	if ((r = bridge_awake(h->write_attr, c.bridge_dir)) < 0) {
		fprintf(stderr, "geforce-sriov: refused: %s: bridge %s power/control=on failed: %s\n", pf->str,
			c.bridge.str, strerror(-r));
		return CARD_REFUSED;
	}
	if (r) {
		fprintf(stderr, "geforce-sriov: refused: %s: bridge %s did not report runtime_status=active\n", pf->str,
			c.bridge.str);
		restore_pm(h, &c, pm_old);
		return CARD_REFUSED;
	}
	printf("%s: bridge %s power/control %s -> on (held awake)\n", pf->str, c.bridge.str, pm_old);

	/* Only the selected card's own HDMI audio function is ever unbound. */
	for (i = 0; i < plan.nunbind; i++) {
		struct gs_card fn;

		gs_card_inspect(h->sys, &plan.unbind[i], &fn);
		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/driver/unbind", h->sys, plan.unbind[i].str);
		if ((r = h->write_attr(p, plan.unbind[i].str))) {
			fprintf(stderr, "geforce-sriov: refused: unbinding %s failed: %s\n", plan.unbind[i].str,
				strerror(-r));
			rebind_audio(h, unbound, nunbound);
			restore_pm(h, &c, pm_old);
			return CARD_REFUSED;
		}
		unbound[nunbound].bdf = plan.unbind[i];
		unbound[nunbound].vendor = fn.vendor;
		unbound[nunbound].device = fn.device;
		nunbound++;
		printf("%s: unbound audio function %s\n", pf->str, plan.unbind[i].str);
		printf("%s: note: removing an audio function that was active this boot may log a blocked DMAR fault; unbinding does not prevent it\n",
		       pf->str);
	}

	/* Fresh look right before removal: nothing bound, nothing shared, no VFs, no console on it. */
	gs_card_inspect(h->sys, pf, &c);
	gs_plan_prepare(&c, granted_display(cfg, h->sys, h->proc, &c, &disp), &plan);
	if (plan.verdict != GS_PLAN_READY || plan.nunbind || plan.release[0]) {
		fprintf(stderr, "geforce-sriov: refused before removal: %s\n",
			plan.verdict != GS_PLAN_READY ? plan.reason : plan.nunbind ? "a card function is still bound" :
			"the boot display's console is bound");
		rebind_audio(h, unbound, nunbound);
		restore_pm(h, &c, pm_old);
		return CARD_REFUSED;
	}
	vendor = c.vendor;
	device = c.device;

	snprintf(step, sizeof(step), "remove-rescan:%s%s", pf->str, k ? ":console-released" : "");
	if ((r = write_marker(h, bdfs, step))) {
		fprintf(stderr, "geforce-sriov: cannot update the guard: %s; nothing removed\n", strerror(-r));
		rebind_audio(h, unbound, nunbound);
		restore_pm(h, &c, pm_old);
		return CARD_REFUSED;
	}

	/* Do not let SIGINT/SIGTERM leave an emptied bus behind. */
	sigemptyset(&block);
	sigaddset(&block, SIGINT);
	sigaddset(&block, SIGTERM);
	sigaddset(&block, SIGHUP);
	sigaddset(&block, SIGQUIT);
	sigprocmask(SIG_BLOCK, &block, &saved);

	/* From here on the card's BARs may move: a released console is never given back. */
	if (k)
		k->removal_attempted = 1;
	for (i = c.nfns; i-- > 0;) {
		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/remove", h->sys, c.fns[i].bdf.str);
		r = h->write_attr(p, "1");
		printf("%s: remove %s%s%s\n", pf->str, c.fns[i].bdf.str, r ? " failed: " : "", r ? strerror(-r) : "");
		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s", h->sys, c.fns[i].bdf.str);
		wait_exists(p, 0, REMOVE_WAIT_MS);
	}
	gs_pathf(pfdir, sizeof(pfdir), "%s/bus/pci/devices/%s", h->sys, pf->str);
	gs_pathf(p, sizeof(p), "%s/remove", pfdir);
	r = h->write_attr(p, "1");
	printf("%s: remove %s%s%s\n", pf->str, pf->str, r ? " failed: " : "", r ? strerror(-r) : "");
	if (!r && wait_exists(pfdir, 0, REMOVE_WAIT_MS))
		printf("%s: still listed %u ms after remove\n", pf->str, REMOVE_WAIT_MS);

	r = h->write_attr(c.rescan_path, "1");
	printf("%s: rescan %s%s%s\n", pf->str, c.rescan_path, r ? " failed: " : "", r ? strerror(-r) : "");

	if (wait_exists(pfdir, 1, RETURN_WAIT_MS)) {
		sigprocmask(SIG_SETMASK, &saved, NULL);
		fprintf(stderr, "geforce-sriov: %s did not come back after the rescan (bridge %s left at power/control=on).\n%s\n",
			pf->str, c.bridge.str, recovery_msg);
		return CARD_LOST;
	}
	gs_pathf(p, sizeof(p), "%s/sriov_totalvfs", pfdir);
	wait_exists(p, 1, SRIOV_ATTR_WAIT_MS);
	for (i = 0; i < c.nfns; i++) {
		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s", h->sys, c.fns[i].bdf.str);
		if (wait_exists(p, 1, FN_RETURN_WAIT_MS))
			printf("%s: warning: function %s did not come back\n", pf->str, c.fns[i].bdf.str);
	}
	sigprocmask(SIG_SETMASK, &saved, NULL);

	gs_card_inspect(h->sys, pf, &after);
	if (after.vendor != vendor || after.device != device) {
		fprintf(stderr, "geforce-sriov: %s came back as %04x:%04x, not %04x:%04x.\n%s\n", pf->str, after.vendor,
			after.device, vendor, device, recovery_msg);
		return CARD_LOST;
	}
	/* The rescan made a new device, with D3cold allowed again: block it before anything binds the PF. */
	if (c.entry && c.entry->laptop) {
		gs_pathf(p, sizeof(p), "%s/d3cold_allowed", pfdir);
		if ((r = h->write_attr(p, "0"))) {
			fprintf(stderr, "geforce-sriov: %s: writing 0 to the d3cold_allowed of the rescanned PF failed: %s; a laptop "
				"GPU that can power off must not get VFs\n", pf->str, strerror(-r));
			return CARD_FAILED_BACK;
		}
		printf("%s: D3cold disabled (d3cold_allowed=0); bridge %s stays powered\n", pf->str, c.bridge.str);
	}
	restore_pm(h, &c, pm_old);
	/* Read-only resource check: a rescan can only resize the card's own bridge window. */
	if (c.bar0_assigned == 1 && after.bar0_assigned == 0) {
		fprintf(stderr, "geforce-sriov: %s came back without BAR0 (it was assigned before): the card is unusable this boot.\n"
			"Do not rescan again; " BAR_REMEDY ". Until then, geforce_sriov.skip on the kernel command line skips preparation\n",
			pf->str);
		return CARD_FAILED_BACK;
	}
	if (!after.has_sriov) {
		fprintf(stderr, "geforce-sriov: %s came back without sriov_totalvfs, so SR-IOV is not presented; see `geforce-sriov doctor` and `journalctl -k -b`\n",
			pf->str);
		return CARD_FAILED_BACK;
	}
	{
		char why[512];

		if (gs_check_presented(&after, cfg, why, sizeof(why))) {
			fprintf(stderr, "geforce-sriov: %s; do not enable VFs\n", why);
			return CARD_FAILED_BACK;
		}
	}
	printf("%s: sriov_totalvfs=%u ari_enabled=%d (ari=%s)\n", pf->str, after.totalvfs, after.ari_enabled,
	       sw(cfg->ari));
	if (cfg->ari != GS_SW_OFF && after.ari_enabled != 1)
		printf("%s: ARI is not enabled: the module presents ARI only when the card's hidden ARI capability names a\n"
		       "next function and the bridge supports ARI forwarding (kernel log: \"ARI stays off for this card\")\n",
		       pf->str);
	if (cfg->msix == GS_SW_ON && after.msix_linked == 1)
		printf("%s: msix=on: MSI-X is linked\n", pf->str);
	else if (cfg->msix == GS_SW_ON && after.msix_linked == 0)
		printf("%s: msix=on: MSI-X stays hidden; the kernel log says why: journalctl -k -b | grep geforce_sriov\n",
		       pf->str);
	if (after.vf_bars_assigned == 0)
		printf("%s: warning: no VF BAR was assigned, so enabling VFs will fail with \"not enough MMIO resources for\n"
		       "SR-IOV\"; " BAR_REMEDY "\n", pf->str);

	gs_boot_id(boot, sizeof(boot));
	r = snprintf(rec, sizeof(rec), "boot_id=%s\ntotalvfs=%u\nari_enabled=%d\n", boot, after.totalvfs,
		     after.ari_enabled);
	if (k)
		r += snprintf(rec + r, sizeof(rec) - (size_t)r, "console_release=%s\n", k->fw[0] ? k->fw : "none");
	/* Whether MSI-X was linked when the card was prepared: doctor and check warn when it is hidden again. */
	if (after.msix_linked >= 0)
		r += snprintf(rec + r, sizeof(rec) - (size_t)r, "msix_linked=%d\n", after.msix_linked);
	gs_pathf(p, sizeof(p), "%s/" GS_PREPARED_NAME, h->run_dir);
	if (gs_mkdir_p(p, 0755) || gs_pathf(p, sizeof(p), "%s/" GS_PREPARED_NAME "/%s", h->run_dir, pf->str) ||
	    gs_write_file_atomic(p, rec, (size_t)r, 0644))
		fprintf(stderr, "geforce-sriov: warning: cannot record %s\n", p);
	return CARD_DONE;
}

static int prepared_now(const struct gs_host *h, const struct gs_addr *pf)
{
	char p[GS_PATH_MAX];

	return !gs_pathf(p, sizeof(p), "%s/" GS_PREPARED_NAME "/%s", h->run_dir, pf->str) && gs_exists(p);
}

/* "--release-console=A --release-console=B" for the config's grants. */
static void consent_args(const struct gs_config *cfg, char *buf, size_t len)
{
	size_t off = 0;

	buf[0] = '\0';
	for (unsigned int i = 0; i < cfg->nrelease && off < len; i++) {
		int n = snprintf(buf + off, len - off, "%s--release-console=%s", i ? " " : "",
				 cfg->boot_display_release[i].str);

		if (n < 0)
			break;
		off += (size_t)n;
	}
}

void gs_release_steps(FILE *f, const char *indent, const struct gs_config *cfg, int or_setup)
{
	char consent[GS_CFG_MAX_BDFS * 40];

	consent_args(cfg, consent, sizeof(consent));
	fprintf(f, "%sgeforce-sriov prepare %s\n"
		"%smodprobe --ignore-install nvidia\n"
		"%sgeforce-sriov activate\n"
		"%sgeforce-sriov check\n", indent, consent, indent, indent, indent);
	if (or_setup)
		fprintf(f, "%sor run geforce-sriov setup, which asks, then runs them\n", indent);
}

/* Where NVIDIA's host package puts the vGPU plugin link: the first that exists, else the first. */
static const char *plugin_link(void)
{
	static const char *const plugins[] = { "/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so", "/usr/lib64/libnvidia-vgpu.so",
					       "/usr/lib/libnvidia-vgpu.so" };

	for (size_t k = 0; k < sizeof(plugins) / sizeof(plugins[0]); k++)
		if (gs_exists(plugins[k]))
			return plugins[k];
	return plugins[0];
}

/* Once every selected card is prepared this boot, the boot unit's deferral is over. */
static void end_deferral(const struct gs_host *h, const struct gs_config *cfg)
{
	char p[GS_PATH_MAX];

	for (unsigned int i = 0; i < cfg->nbdfs; i++)
		if (!prepared_now(h, &cfg->bdfs[i]))
			return;
	if (gs_pathf(p, sizeof(p), "%s/" GS_DEFERRED_NAME, h->run_dir) || !gs_exists(p))
		return;
	if (unlink(p) != 0) {
		fprintf(stderr, "geforce-sriov: cannot remove %s: %s; nvidia-vgpud stays blocked\n", p, strerror(errno));
		return;
	}
	printf("every selected GPU is prepared: the boot unit's deferral is over (%s removed).\n", p);
}

/*
 * The boot unit with a console-release grant: change nothing on any card and record the deferral,
 * which also blocks nvidia-vgpud (its drop-in asserts the record is absent).
 */
static int defer_boot(const struct gs_host *h, const struct gs_config *cfg, const char *bdfs)
{
	char rel[GS_BDF_LIST_STR_SIZE], boot[64] = "unknown", buf[1024], p[GS_PATH_MAX];
	int n, r;

	gs_addr_list_str(cfg->boot_display_release, cfg->nrelease, rel, sizeof(rel));
	gs_boot_id(boot, sizeof(boot));
	n = snprintf(buf, sizeof(buf), "boot_id=%s\nbdfs=%s\nboot_display_release=%s\n", boot, bdfs, rel);
	if (n < 0 || (size_t)n >= sizeof(buf) || gs_pathf(p, sizeof(p), "%s/" GS_DEFERRED_NAME, h->run_dir))
		r = -EOVERFLOW;
	else if (!(r = gs_mkdir_p(h->run_dir, 0755)))
		r = gs_write_file_atomic(p, buf, (size_t)n, 0644);
	if (r) {
		fprintf(stderr, "geforce-sriov: deferred, nothing changed, but %s cannot be written (%s): nvidia-vgpud is not held back\n",
			p, strerror(-r));
		return GS_EXIT_FAIL;
	}
	printf("deferred: boot_display_release=%s grants a console release, so the boot unit prepares none of\n"
	       "the selected GPUs (%s) and changes nothing; nvidia-vgpud is held back until they are prepared.\n"
	       GS_RELEASE_STEPS_LEAD "\n", rel, bdfs);
	gs_release_steps(stdout, "  ", cfg, 1);
	return GS_EXIT_OK;
}

int gs_prepare(const struct gs_host *h, struct gs_config *cfg, const struct gs_prepare_opts *po)
{
	char bdfs[GS_BDF_LIST_STR_SIZE], consent[GS_CFG_MAX_BDFS * 40], p[GS_PATH_MAX];
	struct gs_addr todo[GS_CFG_MAX_BDFS];
	struct console *cons;
	struct woken woke[GS_CFG_MAX_BDFS];
	unsigned int ntodo = 0, ncons = 0, nwoke = 0, i;
	int r, rc = GS_EXIT_OK;

	if (gs_pathf(p, sizeof(p), "%s/" GS_ATTEMPT_NAME, h->state_dir) || gs_exists(p)) {
		fprintf(stderr, "geforce-sriov: guard %s is set: an earlier preparation did not finish. Nothing done.\n%s\n",
			p, recovery_msg);
		return GS_EXIT_COLD_CYCLE;
	}
	/* A laptop GPU without D3cold consent: refused before the deferral and before any wake or write. */
	if (!cfg->d3cold_off) {
		struct gs_addr laptops[GS_CFG_MAX_BDFS];
		unsigned int nl = gs_laptop_gpus(h->sys, cfg->bdfs, cfg->nbdfs, laptops);

		if (nl) {
			char why[GS_D3COLD_WHY_SIZE];

			gs_d3cold_why(why, sizeof(why), laptops, nl, cfg->bdfs, cfg->nbdfs, cfg->boot_display_release,
				      cfg->nrelease, "");
			fprintf(stderr, "geforce-sriov: refused, nothing changed: %s\n", why);
			return GS_EXIT_REFUSED;
		}
	}
	gs_addr_list_str(cfg->bdfs, cfg->nbdfs, bdfs, sizeof(bdfs));
	consent_args(cfg, consent, sizeof(consent));
	/* Consent: the boot unit never gives it; by hand it must name exactly the config's grants. */
	if (cfg->nrelease && po->boot)
		return defer_boot(h, cfg, bdfs);
	if (cfg->nrelease && !same_selection(po->release, po->nrelease, cfg->boot_display_release, cfg->nrelease)) {
		fprintf(stderr, "geforce-sriov: refused, nothing changed: the config grants a console release (boot_display_release), so\n"
			"prepare needs exactly that consent: `geforce-sriov prepare %s`\n%s\n", consent,
			GS_CONSOLE_RELEASE_NOTE);
		return GS_EXIT_REFUSED;
	}
	if (!cfg->nrelease && po->nrelease) {
		fprintf(stderr, "geforce-sriov: refused, nothing changed: --release-console names %s, but the config grants no console release (boot_display_release)\n",
			po->release[0].str);
		return GS_EXIT_REFUSED;
	}
	if (cfg->ari != GS_SW_OFF && cmdline_noari(h->proc)) {
		printf("kernel command line has pci=noari: loading %s with ari=0\n", GS_MODULE_NAME);
		cfg->ari = GS_SW_OFF;
	}
	if (gs_pathf(p, sizeof(p), "%s/kernel/livepatch", h->sys) || !gs_exists(p)) {
		fprintf(stderr, "geforce-sriov: the kernel has no livepatch support (CONFIG_LIVEPATCH)\n");
		return GS_EXIT_REFUSED;
	}
	cons = calloc(GS_CFG_MAX_BDFS, sizeof(*cons));
	if (!cons)
		return GS_EXIT_FAIL;

	/* Preflight every selected card, consoles included, before anything changes. */
	for (i = 0; i < cfg->nbdfs; i++) {
		struct gs_display disp;
		const struct gs_display *d;
		struct gs_card c;
		struct gs_plan plan;

		if (prepared_now(h, &cfg->bdfs[i])) {
			printf("%s: already prepared this boot\n", cfg->bdfs[i].str);
			continue;
		}
		/* Power first: a laptop GPU off in D3cold reads all-ones until it is woken and passes the config-read gate. */
		{
			char why[1024];

			r = wake_card(h->sys, h->write_attr, &cfg->bdfs[i], &woke[nwoke], why, sizeof(why));
			if (r < 0) {
				fprintf(stderr, "geforce-sriov: refused, nothing changed: %s. " WAKE_REMEDY "\n", why);
				unwake(h->write_attr, woke, nwoke);
				free(cons);
				return GS_EXIT_REFUSED;
			}
			if (r > 0)
				printf("%s: %s\n", cfg->bdfs[i].str, woke[nwoke++].done);
		}
		gs_card_inspect(h->sys, &cfg->bdfs[i], &c);
		d = granted_display(cfg, h->sys, h->proc, &c, &disp);
		gs_plan_prepare(&c, d, &plan);
		if (plan.verdict != GS_PLAN_READY) {
			fprintf(stderr, "geforce-sriov: refused, nothing changed: %s\n", plan.reason);
			unwake(h->write_attr, woke, nwoke);
			free(cons);
			return GS_EXIT_REFUSED;
		}
		if (c.entry) {
			char why[512];
			int v = gs_vfs_check(c.entry, cfg, why, sizeof(why));

			if (v < 0) {
				fprintf(stderr, "geforce-sriov: refused, nothing changed: %s: %s\n", c.pf.str, why);
				unwake(h->write_attr, woke, nwoke);
				free(cons);
				return GS_EXIT_REFUSED;
			}
			if (v > 0)
				printf("%s: warning: %s\n", c.pf.str, why);
		}
		if (d) {
			struct console *k = &cons[ncons++];

			k->pf = c.pf;
			gs_copy(k->fw, sizeof(k->fw), d->fw);
			gs_copy(k->fw_dir, sizeof(k->fw_dir), d->fw_dir);
			gs_copy(k->driver, sizeof(k->driver), plan.release_driver);
			printf("%s: boot display: %s\n", c.pf.str,
			       plan.release[0] ? "its console is released before the module loads" :
			       d->fw[0] ? "its firmware framebuffer is already unbound" :
			       "it has no firmware framebuffer to release");
		}
		todo[ntodo++] = cfg->bdfs[i];
	}
	if (ntodo == 0) {
		end_deferral(h, cfg);
		free(cons);
		return GS_EXIT_OK;
	}
	if (ncons)
		printf("%s\n", GS_CONSOLE_RELEASE_NOTE);

	/* The guard is on disk before the first change; release_console() writes it first. */
	for (i = 0; i < ncons; i++) {
		if (!cons[i].driver[0])
			continue;
		if (release_console(h, bdfs, &cons[i])) {
			rebind_consoles(h, cons, ncons);
			clear_marker(h);
			unwake(h->write_attr, woke, nwoke);
			free(cons);
			return GS_EXIT_REFUSED;
		}
	}
	if ((r = write_marker(h, bdfs, "load-module"))) {
		fprintf(stderr, "geforce-sriov: cannot write the guard: %s; nothing done\n", strerror(-r));
		rebind_consoles(h, cons, ncons);
		clear_marker(h);
		unwake(h->write_attr, woke, nwoke);
		free(cons);
		return GS_EXIT_FAIL;
	}
	r = load_module(h, cfg, bdfs);
	if (r) {
		rebind_consoles(h, cons, ncons);
		clear_marker(h);
		unwake(h->write_attr, woke, nwoke);
		free(cons);
		return r;
	}
	for (i = 0; i < ntodo; i++) {
		struct console *k = NULL;
		enum card_result res;

		for (unsigned int j = 0; j < ncons; j++)
			if (strcmp(cons[j].pf.str, todo[i].str) == 0)
				k = &cons[j];
		res = prepare_card(h, &todo[i], bdfs, cfg, k);
		if (res == CARD_LOST) {
			free(cons);
			return GS_EXIT_COLD_CYCLE; /* the guard stays set; nothing is given back */
		}
		/*
		 * The module is loaded and earlier cards may be prepared: a card refusing now is a
		 * failure, not exit 3 ("refused, nothing changed").
		 */
		if (res == CARD_REFUSED || res == CARD_FAILED_BACK) {
			/*
			 * Give back the wake of each card that was never removed: this one when it was refused
			 * before its removal (CARD_REFUSED), and every card after it. A card that was removed
			 * and rescanned is a new device and is never written to here.
			 */
			for (unsigned int j = 0; j < nwoke; j++)
				for (unsigned int l = res == CARD_REFUSED ? i : i + 1; l < ntodo; l++)
					if (strcmp(woke[j].pf.str, todo[l].str) == 0)
						unwake(h->write_attr, &woke[j], 1);
			rc = GS_EXIT_FAIL;
			break;
		}
	}
	if (rc != GS_EXIT_OK)
		rebind_consoles(h, cons, ncons);
	free(cons);
	/* Every card is present and in a known state: the guard is no longer needed. */
	if ((r = clear_marker(h))) {
		fprintf(stderr, "geforce-sriov: cannot remove the guard: %s\n", strerror(-r));
		return GS_EXIT_FAIL;
	}
	if (rc == GS_EXIT_OK)
		end_deferral(h, cfg);
	return rc;
}

int gs_cmd_prepare(const struct gs_overrides *ov, const struct gs_prepare_opts *po, int clear)
{
	struct gs_config cfg;
	int r;

	if ((r = require_root("prepare")))
		return r;
	if ((r = gs_lock()))
		return r;
	if (clear)
		return clear_guard();
	if (gs_exists(GS_ATTEMPT_MARKER)) {
		fprintf(stderr, "geforce-sriov: guard %s is set: an earlier preparation did not finish. Nothing done.\n%s\n",
			GS_ATTEMPT_MARKER, recovery_msg);
		return GS_EXIT_COLD_CYCLE;
	}
	if ((r = load_live_config(&cfg)))
		return r;
	if (ov->ari != GS_SW_UNSET)
		cfg.ari = ov->ari;
	if (ov->msix != GS_SW_UNSET)
		cfg.msix = ov->msix;
	return gs_prepare(&gs_host_live, &cfg, po);
}

/* ---- VF enabling for activate (live only) ------------------------------------ */

static int sriov_manage_safe(const char *path, char *why, size_t len)
{
	struct stat st;

	if (stat(path, &st) < 0)
		return snprintf(why, len, "%.300s: %s (install NVIDIA's vGPU host driver)", path, strerror(errno)), -1;
	if (!S_ISREG(st.st_mode) || !(st.st_mode & S_IXUSR))
		return snprintf(why, len, "%.300s is not an executable file", path), -1;
	if (st.st_uid != 0 || (st.st_mode & 022))
		return snprintf(why, len, "%.300s is not root-owned or is group/world-writable", path), -1;
	return 0;
}

static int vfs_bound(const char *sys, const struct gs_card *c, unsigned int want, char *bad, size_t len)
{
	char p[GS_PATH_MAX], target[GS_PATH_MAX];
	unsigned int i;

	for (i = 0; i < want; i++) {
		struct gs_addr a;
		struct gs_card vf;
		ssize_t n;
		const char *base;

		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/virtfn%u", sys, c->pf.str, i);
		n = readlink(p, target, sizeof(target) - 1);
		if (n <= 0)
			return snprintf(bad, len, "virtfn%u missing", i), -1;
		target[n] = '\0';
		base = strrchr(target, '/');
		if (gs_addr_parse(base ? base + 1 : target, &a))
			return snprintf(bad, len, "virtfn%u unparsable", i), -1;
		gs_card_inspect(sys, &a, &vf);
		if (strcmp(vf.driver, "nvidia") != 0)
			return snprintf(bad, len, "VF %s bound to %s", a.str, vf.driver[0] ? vf.driver : "nothing"), -1;
	}
	return 0;
}

#define SRIOV_MANAGE_NOTE \
	"note: sriov-manage briefly stops nvidia-xid-logd, moves each GPU to pci-pf-stub to set sriov_numvfs,\n" \
	"binds it and the other functions on its bus to nvidia and runs `nvidia-smi -pm 1`; the result is\n" \
	"judged from sysfs, not from its exit status\n"

/* sriov-manage is a root-owned executable and lspci and setpci are there for it to run. 0, or -1 with why. */
static int enable_prereq(const struct gs_config *cfg, char *why, size_t len)
{
	static const char *const deps[] = { "lspci", "setpci" };
	char p[GS_PATH_MAX];

	if (sriov_manage_safe(cfg->sriov_manage, why, len))
		return -1;
	/*
	 * sriov-manage needs lspci (address normalisation; without it it exits 1 silently) and
	 * setpci (class check), found through the PATH gs_run gives it.
	 */
	for (size_t k = 0; k < sizeof(deps) / sizeof(deps[0]); k++)
		if (gs_find_program(deps[k], p, sizeof(p)))
			return snprintf(why, len, "%.200s needs %s (pciutils), not found in /usr/sbin:/usr/bin:/sbin:/bin",
					cfg->sriov_manage, deps[k]), -1;
	return 0;
}

/*
 * sriov-manage -e on a PF that is on nvidia, judged by state alone. c is the PF as inspected before
 * and, on return, as it stands. GS_EXIT_OK, GS_EXIT_REFUSED (not on nvidia; nothing changed) or GS_EXIT_FAIL.
 */
static int enable_pf(const struct gs_host *h, const struct gs_config *cfg, const struct gs_addr *pf, struct gs_card *c)
{
	const char *argv[] = { cfg->sriov_manage, "-e", pf->str, NULL };
	char bad[256];
	int r;

	if (strcmp(c->driver, "nvidia") != 0) {
		fprintf(stderr, "geforce-sriov: refused: %s is bound to %s, not nvidia (load NVIDIA's vGPU host driver first)\n",
			pf->str, c->driver[0] ? c->driver : "nothing");
		return GS_EXIT_REFUSED;
	}
	printf("%s -e %s\n", cfg->sriov_manage, pf->str);
	r = h->run(argv);
	if (r != 0)
		fprintf(stderr, "geforce-sriov: %s exited with %d\n", cfg->sriov_manage, r);
	/* sriov-manage may exit 0 without doing anything; judge only by state. */
	for (unsigned int t = 0;; t += POLL_MS) {
		gs_card_inspect(h->sys, pf, c);
		if (c->numvfs == c->totalvfs && c->numvfs >= 1 && c->numvfs <= gs_config_vfs(cfg) &&
		    vfs_bound(h->sys, c, c->numvfs, bad, sizeof(bad)) == 0 && strcmp(c->driver, "nvidia") == 0)
			break;
		if (t >= VF_BIND_WAIT_MS) {
			if (c->numvfs == 0)
				snprintf(bad, sizeof(bad), "no VF was enabled");
			else if (c->numvfs != c->totalvfs || c->numvfs > gs_config_vfs(cfg))
				snprintf(bad, sizeof(bad), "VF count is not the presented 1..%u", gs_config_vfs(cfg));
			else if (strcmp(c->driver, "nvidia") != 0)
				snprintf(bad, sizeof(bad), "PF bound to %s%s", c->driver[0] ? c->driver : "nothing",
					 strcmp(c->driver, "pci-pf-stub") == 0 ?
					 " (sriov-manage stopped mid-way; its driver_override may still name pci-pf-stub)" : "");
			else
				vfs_bound(h->sys, c, c->numvfs, bad, sizeof(bad));
			fprintf(stderr, "geforce-sriov: %s: enable did not complete: sriov_numvfs=%u of %u; %s\n",
				pf->str, c->numvfs, c->totalvfs, bad);
			return GS_EXIT_FAIL;
		}
		gs_sleep_ms(POLL_MS);
	}
	printf("%s: %u VFs enabled and bound to nvidia\n", pf->str, c->numvfs);
	return GS_EXIT_OK;
}

/* ---- activate and check: the Ada and Ampere CPU-RM host stack ------------------------------ */

int gs_parse_vgpu_type(const char *value, unsigned int *out)
{
	unsigned long v;
	char *end;

	if (!value || value[0] < '1' || value[0] > '9')
		return -1;
	errno = 0;
	v = strtoul(value, &end, 10);
	if (errno || *end || v > GS_VGPU_TYPE_MAX)
		return -1;
	*out = (unsigned int)v;
	return 0;
}

/* One selected PF, its enabled VFs as its virtfnN and their physfn links name them, and its vGPU type. */
struct vgpu_card {
	struct gs_addr pf;
	struct gs_addr vf[GS_VFS_MAX];
	unsigned int nvf;
	int enable;        /* no VFs yet: activate enables them first */
	unsigned int generation;
	unsigned int type; /* --type, or gs_config_vgpu_type() of its generation */
};

static int read_dev_attr(const char *sys, const char *bdf, const char *name, char *buf, size_t len)
{
	char p[GS_PATH_MAX];

	buf[0] = '\0';
	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/%s", sys, bdf, name))
		return -ENAMETOOLONG;
	return gs_read_attr(p, buf, len);
}

/* The name of the driver bdf is bound to; "" when unbound. */
static void dev_driver(const char *sys, const char *bdf, char *out, size_t len)
{
	char p[GS_PATH_MAX], target[GS_PATH_MAX];
	const char *base;
	ssize_t n;

	out[0] = '\0';
	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/driver", sys, bdf))
		return;
	n = readlink(p, target, sizeof(target) - 1);
	if (n <= 0)
		return;
	target[n] = '\0';
	base = strrchr(target, '/');
	gs_copy(out, len, base ? base + 1 : target);
}

/* virtfn0..numvfs-1 of pf, each confirmed by its own physfn link back to pf. 0, or -1 with why. */
static int vf_list(const char *sys, const struct gs_addr *pf, unsigned int numvfs, struct gs_addr *out, char *why,
		   size_t len)
{
	char p[GS_PATH_MAX], real[PATH_MAX];
	const char *base;

	if (numvfs > GS_VFS_MAX)
		return snprintf(why, len, "%s has %u VFs, more than %u", pf->str, numvfs, GS_VFS_MAX), -1;
	for (unsigned int i = 0; i < numvfs; i++) {
		if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/virtfn%u", sys, pf->str, i) || !realpath(p, real))
			return snprintf(why, len, "%s: virtfn%u is missing", pf->str, i), -1;
		base = strrchr(real, '/');
		if (gs_addr_parse(base ? base + 1 : real, &out[i]))
			return snprintf(why, len, "%s: virtfn%u names no PCI address", pf->str, i), -1;
		if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/physfn", sys, out[i].str) || !realpath(p, real) ||
		    !(base = strrchr(real, '/')) || strcmp(base + 1, pf->str) != 0)
			return snprintf(why, len, "%s: the physfn link of its virtfn%u %s does not lead back to it", pf->str, i,
					out[i].str), -1;
	}
	return 0;
}

/*
 * The selected GPUs, or those named by --device, each with the vGPU type to set: --type, else the
 * config's type for its generation, else the generation default. A --device outside the selection, a
 * missing card, a card that is not in the device table, and --type on a selection of both generations
 * (no type is creatable on both targets) are refused.
 */
static int select_cards(const char *sys, const struct gs_config *cfg, const struct gs_activate_opts *o,
			struct vgpu_card *out, unsigned int *n)
{
	const struct gs_addr *ada = NULL, *ampere = NULL;

	*n = 0;
	for (unsigned int i = 0; i < o->ndevices; i++) {
		if (!addr_listed(cfg->bdfs, cfg->nbdfs, o->devices[i].str)) {
			fprintf(stderr, "geforce-sriov: refused, nothing changed: --device %s is not selected (bdfs= in %s)\n",
				o->devices[i].str, gs_config_path());
			return GS_EXIT_REFUSED;
		}
	}
	for (unsigned int i = 0; i < cfg->nbdfs; i++) {
		const struct gs_addr *pf = &cfg->bdfs[i];
		const struct gs_device *e;
		struct gs_card c;

		if (o->ndevices && !addr_listed(o->devices, o->ndevices, pf->str))
			continue;
		gs_card_inspect(sys, pf, &c);
		if (!c.present) {
			fprintf(stderr, "geforce-sriov: refused, nothing changed: %s is missing\n", pf->str);
			return GS_EXIT_REFUSED;
		}
		e = c.entry;
		if (!e) {
			fprintf(stderr, "geforce-sriov: refused, nothing changed: %s (%04x:%04x) is not in the device table\n",
				pf->str, c.vendor, c.device);
			return GS_EXIT_REFUSED;
		}
		if (e->generation == GS_ADA && !ada)
			ada = pf;
		else if (e->generation == GS_AMPERE && !ampere)
			ampere = pf;
		out[*n].pf = *pf;
		out[*n].generation = e->generation;
		out[(*n)++].type = o->type ? o->type : gs_config_vgpu_type(cfg, e->generation);
	}
	if (*n == 0) {
		fprintf(stderr, "geforce-sriov: refused, nothing changed: no GPU is selected (bdfs= in %s)\n", gs_config_path());
		return GS_EXIT_REFUSED;
	}
	if (o->type && ada && ampere) {
		fprintf(stderr, "geforce-sriov: refused, nothing changed: --type %u would set one vGPU type on Ada (%s) and Ampere (%s)\n"
			"cards, and no type fits both (%u is not creatable on the Ampere target, %u not on the Ada target). Omit --type\n"
			"for the per-generation defaults (%u Ada, %u Ampere), or, for check, name one generation's GPUs with --device.\n"
			"Set type_ada= and type_ampere= in %s to choose one per generation\n",
			o->type, ada->str, ampere->str, GS_VGPU_TYPE_ADA, GS_VGPU_TYPE_AMPERE, gs_config_vgpu_type(cfg, GS_ADA),
			gs_config_vgpu_type(cfg, GS_AMPERE), gs_config_path());
		return GS_EXIT_REFUSED;
	}
	return 0;
}

/*
 * An NVIDIA VF on the host whose PF is not one of cards (activate's cards are every selected GPU):
 * 1 with which, 0 none, -1 when the device list is unreadable.
 */
static int foreign_vf(const char *sys, const struct vgpu_card *cards, unsigned int n, char *which, size_t len)
{
	char p[GS_PATH_MAX], real[PATH_MAX];
	const char *base;
	struct dirent *de;
	unsigned int vendor, i;
	int found = 0;
	DIR *d;

	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices", sys) || !(d = opendir(p)))
		return snprintf(which, len, "%s/bus/pci/devices is unreadable", sys), -1;
	while (!found && (de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/physfn", sys, de->d_name) || !realpath(p, real))
			continue;
		base = strrchr(real, '/');
		base = base ? base + 1 : real;
		if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/vendor", sys, de->d_name) || gs_read_hex(p, &vendor) ||
		    vendor != 0x10de)
			continue;
		for (i = 0; i < n && strcmp(cards[i].pf.str, base) != 0; i++)
			;
		if (i < n)
			continue;
		snprintf(which, len, "VF %.32s of %.32s", de->d_name, base);
		found = 1;
	}
	closedir(d);
	return found;
}

/* How many VFs with a vGPU the refusal lists, one per line; the command after it names them all. */
#define HOST_VGPU_SHOWN 8

struct host_vgpus {
	char first[160];  /* the first VF's line, or why the device list is unreadable */
	char lines[1024]; /* "  <line>\n" for each of the first HOST_VGPU_SHOWN, then "  and N more\n" */
	char bdfs[2048];  /* every such VF, space-separated */
};

/*
 * The VFs of NVIDIA PFs anywhere on the host that have a vGPU: current_vgpu_type not 0, or present
 * and unreadable, in address order. Their count, 0 none, -1 when the device list is unreadable.
 */
static int host_vgpu(const char *sys, struct host_vgpus *v)
{
	char p[GS_PATH_MAX], type[32], line[160];
	struct dirent **names;
	size_t lo = 0, bo = 0;
	unsigned int vendor;
	int found = 0, n, i, r;

	v->first[0] = v->lines[0] = v->bdfs[0] = '\0';
	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices", sys) || (n = scandir(p, &names, NULL, alphasort)) < 0)
		return snprintf(v->first, sizeof(v->first), "%.100s/bus/pci/devices is unreadable", sys), -1;
	for (i = 0; i < n; i++) {
		const char *d = names[i]->d_name;

		line[0] = '\0';
		if (d[0] != '.' && !gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/physfn", sys, d) && gs_exists(p) &&
		    !gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/vendor", sys, d) && !gs_read_hex(p, &vendor) &&
		    vendor == 0x10de) {
			r = read_dev_attr(sys, d, "nvidia/current_vgpu_type", type, sizeof(type));
			if (r == 0 && strcmp(type, "0") != 0)
				snprintf(line, sizeof(line), "VF %.32s has vGPU type %.31s", d, type);
			else if (r && r != -ENOENT)
				snprintf(line, sizeof(line), "VF %.32s: nvidia/current_vgpu_type is unreadable (%.64s)", d,
					 strerror(-r));
		}
		if (line[0]) {
			if (!found++)
				gs_copy(v->first, sizeof(v->first), line);
			if (found <= HOST_VGPU_SHOWN && lo < sizeof(v->lines))
				lo += (size_t)snprintf(v->lines + lo, sizeof(v->lines) - lo, "  %s\n", line);
			if (bo < sizeof(v->bdfs))
				bo += (size_t)snprintf(v->bdfs + bo, sizeof(v->bdfs) - bo, "%s%.32s", bo ? " " : "", d);
		}
		free(names[i]);
	}
	free(names);
	if (found > HOST_VGPU_SHOWN && lo < sizeof(v->lines))
		snprintf(v->lines + lo, sizeof(v->lines) - lo, "  and %d more\n", found - HOST_VGPU_SHOWN);
	return found;
}

/* NVIDIA's driver loaded with NVreg_EnableGpuFirmware=0 (GSP off): 0, or -1 with why. */
static int check_gsp_off(const char *procroot, char *why, size_t len)
{
	char p[GS_PATH_MAX], *data, *line, *save = NULL;
	const char *v;
	size_t dlen;
	int r = -1;

	if (gs_pathf(p, sizeof(p), "%s/driver/nvidia/params", procroot) || gs_read_whole(p, &data, &dlen, 1 << 20))
		return snprintf(why, len, "%.300s is unreadable: NVIDIA's driver is not loaded", p), -1;
	snprintf(why, len, "%.300s has no EnableGpuFirmware line; Ada and Ampere need NVreg_EnableGpuFirmware=0", p);
	for (line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (strncmp(line, "EnableGpuFirmware:", 18) != 0)
			continue;
		for (v = line + 18; *v == ' ' || *v == '\t'; v++)
			;
		if (strcmp(v, "0") == 0)
			r = 0;
		else
			snprintf(why, len, "NVIDIA's driver is loaded with EnableGpuFirmware: %.32s; Ada and Ampere need NVreg_EnableGpuFirmware=0 (GSP off, CPU-RM)",
				 v);
		break;
	}
	free(data);
	return r;
}

/* The verdict half of check_plugin, on a digest already taken; prints nothing. */
int gs_plugin_verdict(const char *path, const char *real, const char *sha, char *why, size_t len)
{
	if (strcmp(sha, PATCHED_PLUGIN_SHA) == 0)
		return 0;
	if (strcmp(sha, ADA_PLUGIN_MIXED_SHA) == 0)
		return snprintf(why, len, "%.300s (%.300s) is an earlier copy without the Ada framebuffer change. Run\n"
				"`sudo geforce-sriov install` again to replace it with the patched plugin copy, SHA-256 " PATCHED_PLUGIN_SHA
				" (docs/driver-patch.md#revisions)", path, real), -1;
	if (strcmp(sha, ADA_PLUGIN_H5_SHA) == 0)
		return snprintf(why, len, "%.300s (%.300s) is the earlier Ada-only copy; replace it with the patched plugin copy,\n"
				"SHA-256 " PATCHED_PLUGIN_SHA " (docs/driver-patch.md#revisions)", path, real), -1;
	if (strcmp(sha, ADA_PLUGIN_STOCK_SHA) == 0)
		return snprintf(why, len, "%.300s is NVIDIA's unchanged 580.159.01 plugin (%.300s); Ada and Ampere need its patched plugin copy,\n"
				"SHA-256 " PATCHED_PLUGIN_SHA " (docs/driver-patch.md#revisions)", path, real), -1;
	return snprintf(why, len, "%.300s (%.300s) has SHA-256 %s: not the patched plugin copy of NVIDIA's 580.159.01\n"
			"plugin, SHA-256 " PATCHED_PLUGIN_SHA, path, real, sha), -1;
}

/*
 * The plugin nvidia-vgpu-mgr loads must be the patched plugin copy of 580.159.01's: 0, or -1 with why.
 * path is documented as a link to the versioned copy, so it is hashed through the link and real names that copy.
 */
static int check_plugin(const char *path, char *why, size_t len)
{
	char sha[65], real[GS_PATH_MAX];
	int r = gs_sha256_file_resolved(path, sha, real);

	if (r)
		return snprintf(why, len, "cannot read the vGPU plugin %.300s (from NVIDIA's host package): %s", path,
				strerror(-r)), -1;
	if (gs_plugin_verdict(path, real, sha, why, len))
		return -1;
	printf("vGPU plugin %s -> %s: the patched plugin copy\n", path, real);
	return 0;
}

/* 1 when the VF's nvidia/creatable_vgpu_types lists type (lines like "942 : NVIDIA RTX6000-Ada-2Q"). */
static int type_creatable(const char *sys, const char *vf, unsigned int type)
{
	char p[GS_PATH_MAX], *data, *line, *save = NULL;
	size_t dlen;
	int found = 0;

	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/nvidia/creatable_vgpu_types", sys, vf) ||
	    gs_read_whole(p, &data, &dlen, 1 << 16))
		return 0;
	for (line = strtok_r(data, "\n", &save); line && !found; line = strtok_r(NULL, "\n", &save)) {
		unsigned long v;
		char *end;

		while (*line == ' ' || *line == '\t')
			line++;
		if (*line < '0' || *line > '9')
			continue;
		v = strtoul(line, &end, 10);
		while (*end == ' ' || *end == '\t')
			end++;
		found = v == type && *end == ':';
	}
	free(data);
	return found;
}

/*
 * The re-probe of a laptop PF: its bridge at power/control=on, its own power/control at on and its d3cold_allowed at
 * 0, before the unbind and again after the bind, so that it cannot power off (D3cold) in between. Only the values
 * that differ are written, each named. 0, or GS_EXIT_FAIL with the failed write named.
 */
static int laptop_hold(const struct gs_host *h, const struct gs_addr *pf)
{
	static const char *const attrs[][2] = { { "power/control", "on" }, { "d3cold_allowed", "0" } };
	char dir[GS_PATH_MAX], p[GS_PATH_MAX], v[16];
	unsigned int device;
	const struct gs_device *e = pf_entry(h->sys, pf, &device);
	int r;

	if (!e || !e->laptop)
		return 0;
	if (!pf_bridge_dir(h->sys, pf, dir, sizeof(dir)) && !gs_pathf(p, sizeof(p), "%s/power/control", dir) &&
	    (gs_read_attr(p, v, sizeof(v)) || strcmp(v, "on") != 0)) {
		if ((r = h->write_attr(p, "on"))) {
			fprintf(stderr, "geforce-sriov: %s: writing on to its bridge's power/control failed: %s\n", pf->str,
				strerror(-r));
			return GS_EXIT_FAIL;
		}
		printf("%s: bridge power/control -> on (laptop GPU)\n", pf->str);
	}
	for (size_t k = 0; k < sizeof(attrs) / sizeof(attrs[0]); k++) {
		if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/%s", h->sys, pf->str, attrs[k][0]))
			return GS_EXIT_FAIL;
		if (!gs_read_attr(p, v, sizeof(v)) && strcmp(v, attrs[k][1]) == 0)
			continue;
		if ((r = h->write_attr(p, attrs[k][1]))) {
			fprintf(stderr, "geforce-sriov: %s: writing %s to %s failed: %s\n", pf->str, attrs[k][1], attrs[k][0],
				strerror(-r));
			return GS_EXIT_FAIL;
		}
		printf("%s: %s -> %s (laptop GPU)\n", pf->str, attrs[k][0], attrs[k][1]);
	}
	return 0;
}

/*
 * The PF re-probe (driver unbind, then bind; never remove or rescan), behind the guard
 * and NVIDIA's unbindLock, which the driver grants only while no client holds the GPU (the count
 * an unbind would otherwise wait on). 0; GS_EXIT_REFUSED: lock not granted, nothing unbound;
 * GS_EXIT_FAIL: still bound, nothing else changed; GS_EXIT_COLD_CYCLE: not back on nvidia, guard set.
 */
static int reprobe_pf_unmasked(const struct gs_host *h, const struct gs_addr *pf)
{
	char lock[GS_PATH_MAX], unbind_path[GS_PATH_MAX], bind_path[GS_PATH_MAX], drv[GS_PATH_MAX], val[16] = "", now[64];
	int r;

	if (gs_pathf(lock, sizeof(lock), "%s/driver/nvidia/gpus/%s/unbindLock", h->proc, pf->str) ||
	    gs_pathf(unbind_path, sizeof(unbind_path), "%s/bus/pci/drivers/nvidia/unbind", h->sys) ||
	    gs_pathf(bind_path, sizeof(bind_path), "%s/bus/pci/drivers/nvidia/bind", h->sys) ||
	    gs_pathf(drv, sizeof(drv), "%s/bus/pci/devices/%s/driver", h->sys, pf->str))
		return GS_EXIT_FAIL;
	if ((r = laptop_hold(h, pf)))
		return r;
	if ((r = write_marker(h, pf->str, "reprobe"))) {
		fprintf(stderr, "geforce-sriov: cannot write the guard: %s; %s was not re-probed\n", strerror(-r), pf->str);
		return GS_EXIT_FAIL;
	}
	r = h->write_attr(lock, "1");
	if (r || gs_read_attr(lock, val, sizeof(val)) || strcmp(val, "1") != 0) {
		h->write_attr(lock, "0");
		clear_marker(h);
		fprintf(stderr, "geforce-sriov: %s: NVIDIA's driver did not grant unbindLock (it reads '%s'%s%s), so it was not unbound.\n"
			"A client still holds the GPU, such as nvidia-smi, nvidia-persistenced or a CUDA program; stop it, then run activate again\n",
			pf->str, val, r ? "; the write failed: " : "", r ? strerror(-r) : "");
		return GS_EXIT_REFUSED;
	}
	printf("%s: unbindLock granted\necho %s > /sys/bus/pci/drivers/nvidia/unbind\n", pf->str, pf->str);
	r = h->write_attr(unbind_path, pf->str);
	if (wait_exists(drv, 0, PF_REBIND_WAIT_MS)) {
		h->write_attr(lock, "0");
		clear_marker(h);
		fprintf(stderr, "geforce-sriov: %s is still bound after the unbind%s%s; unbindLock released, nothing else changed\n",
			pf->str, r ? ": " : "", r ? strerror(-r) : "");
		return GS_EXIT_FAIL;
	}
	printf("echo %s > /sys/bus/pci/drivers/nvidia/bind\n", pf->str);
	r = h->write_attr(bind_path, pf->str);
	for (unsigned int t = 0;; t += POLL_MS) {
		dev_driver(h->sys, pf->str, now, sizeof(now));
		if (strcmp(now, "nvidia") == 0)
			break;
		if (t >= PF_REBIND_WAIT_MS) {
			fprintf(stderr, "geforce-sriov: %s is bound to %s, not nvidia, after the re-probe%s%s. The guard stays set and no\n"
				"VF is powered.\n%s\n",
				pf->str, now[0] ? now : "nothing", r ? ": " : "", r ? strerror(-r) : "", recovery_msg);
			return GS_EXIT_COLD_CYCLE;
		}
		gs_sleep_ms(POLL_MS);
	}
	if ((r = clear_marker(h))) {
		char g[GS_PATH_MAX];
		struct stat st;
		int e;

		/* clear_marker also fails when the unlink worked and only the directory sync did not;
		 * only ENOENT proves the guard is gone, any other lstat error leaves its state unknown. */
		e = -gs_pathf(g, sizeof(g), "%s/" GS_ATTEMPT_NAME, h->state_dir);
		if (e == 0 && lstat(g, &st) < 0)
			e = errno;
		if (e == 0)
			fprintf(stderr, "geforce-sriov: %s is bound to nvidia again, but removing the guard failed: %s. The guard stays\n"
				"set.\n%s\n", pf->str, strerror(-r), recovery_msg);
		else if (e == ENOENT)
			fprintf(stderr, "geforce-sriov: %s is bound to nvidia again and the guard was removed, but syncing its\n"
				"directory failed: %s\n", pf->str, strerror(-r));
		else
			fprintf(stderr, "geforce-sriov: %s is bound to nvidia again, but removing the guard failed: %s. Whether the\n"
				"guard still exists cannot be told (%s); treat it as set.\n%s\n", pf->str, strerror(-r), strerror(e),
				recovery_msg);
		return GS_EXIT_FAIL;
	}
	if ((r = laptop_hold(h, pf)))
		return r;
	printf("%s: re-probed, bound to nvidia again\n", pf->str);
	return 0;
}

/* The re-probe with SIGINT/SIGTERM/SIGHUP/SIGQUIT held back, so the unit's start timeout never lands between unbind and bind. */
static int reprobe_pf(const struct gs_host *h, const struct gs_addr *pf)
{
	sigset_t block, saved;
	int r;

	sigemptyset(&block);
	sigaddset(&block, SIGINT);
	sigaddset(&block, SIGTERM);
	sigaddset(&block, SIGHUP);
	sigaddset(&block, SIGQUIT);
	sigprocmask(SIG_BLOCK, &block, &saved);
	r = reprobe_pf_unmasked(h, pf);
	sigprocmask(SIG_SETMASK, &saved, NULL);
	return r;
}

/* The card's vGPU type on one VF: current_vgpu_type must be 0 or type, and type listed as creatable within wait ms. */
static int set_type_vf(const struct gs_host *h, const struct gs_addr *vf, unsigned int type, unsigned int wait)
{
	char p[GS_PATH_MAX], cur[32], want[16];
	int r;

	snprintf(want, sizeof(want), "%u", type);
	if ((r = read_dev_attr(h->sys, vf->str, "nvidia/current_vgpu_type", cur, sizeof(cur)))) {
		fprintf(stderr, "geforce-sriov: %s: nvidia/current_vgpu_type is unreadable: %s\n", vf->str, strerror(-r));
		return GS_EXIT_FAIL;
	}
	if (strcmp(cur, want) == 0) {
		printf("%s: vGPU type already %s\n", vf->str, want);
		return 0;
	}
	if (strcmp(cur, "0") != 0) {
		fprintf(stderr, "geforce-sriov: %s has vGPU type %s, not 0 or %s; nothing written\n", vf->str, cur, want);
		return GS_EXIT_FAIL;
	}
	for (unsigned int t = 0; !type_creatable(h->sys, vf->str, type); t += POLL_MS) {
		if (t >= wait) {
			fprintf(stderr, "geforce-sriov: %s: type %s is not in nvidia/creatable_vgpu_types (`cat` it for the types this GPU\n"
				"offers); nothing written. The driver keeps the vGPU types it registered first after a boot, so after a\n"
				"change of identity or target, power the machine off and on\n", vf->str, want);
			return GS_EXIT_FAIL;
		}
		gs_sleep_ms(POLL_MS);
	}
	gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/nvidia/current_vgpu_type", h->sys, vf->str);
	printf("%s: echo %s > nvidia/current_vgpu_type\n", vf->str, want);
	if ((r = h->write_attr(p, want))) {
		fprintf(stderr, "geforce-sriov: %s: writing the vGPU type failed: %s\n", vf->str, strerror(-r));
		return GS_EXIT_FAIL;
	}
	if (read_dev_attr(h->sys, vf->str, "nvidia/current_vgpu_type", cur, sizeof(cur)) || strcmp(cur, want) != 0) {
		fprintf(stderr, "geforce-sriov: %s: current_vgpu_type reads '%s' after writing %s\n", vf->str, cur, want);
		return GS_EXIT_FAIL;
	}
	return 0;
}

/* power/control=on on one VF, then the gate: on and active read back after the write. */
static int power_on_vf(const struct gs_host *h, const struct gs_addr *vf)
{
	char ctl[GS_PATH_MAX], st[GS_PATH_MAX], c[32] = "", s[32] = "";
	int r;

	if (gs_pathf(ctl, sizeof(ctl), "%s/bus/pci/devices/%s/power/control", h->sys, vf->str) ||
	    gs_pathf(st, sizeof(st), "%s/bus/pci/devices/%s/power/runtime_status", h->sys, vf->str))
		return GS_EXIT_FAIL;
	if (gs_read_attr(ctl, c, sizeof(c)) == 0 && strcmp(c, "on") == 0) {
		printf("%s: power/control already on\n", vf->str);
	} else if ((r = h->write_attr(ctl, "on"))) {
		fprintf(stderr, "geforce-sriov: %s: writing on to power/control failed: %s; do not start a VM on it\n", vf->str,
			strerror(-r));
		return GS_EXIT_FAIL;
	} else {
		printf("%s: echo on > power/control\n", vf->str);
	}
	wait_attr_eq(st, "active", VF_ACTIVE_WAIT_MS);
	c[0] = s[0] = '\0';
	gs_read_attr(ctl, c, sizeof(c));
	gs_read_attr(st, s, sizeof(s));
	printf("%s: gate: power/control=%s runtime_status=%s\n", vf->str, c, s);
	if (strcmp(c, "on") != 0 || strcmp(s, "active") != 0) {
		fprintf(stderr, "geforce-sriov: %s is not on/active after the write; do not start a VM on it\n", vf->str);
		return GS_EXIT_FAIL;
	}
	return 0;
}

/*
 * After the last re-probe: the same VFs again, registered by the vGPU stack, then the card's type and power.
 * wait: ms for a VF's types to appear.
 */
static int finish_card(const struct gs_host *h, const struct vgpu_card *k, unsigned int wait)
{
	struct gs_addr now[GS_VFS_MAX];
	char why[512], p[GS_PATH_MAX], drv[64];
	struct gs_card c;
	unsigned int j;

	gs_card_inspect(h->sys, &k->pf, &c);
	if (c.numvfs != k->nvf || vf_list(h->sys, &k->pf, c.numvfs, now, why, sizeof(why))) {
		fprintf(stderr, "geforce-sriov: %s: after the re-probe it has %u VFs, not the %u it had%s%s\n", k->pf.str,
			c.numvfs, k->nvf, c.numvfs == k->nvf ? ": " : "", c.numvfs == k->nvf ? why : "");
		return GS_EXIT_FAIL;
	}
	for (j = 0; j < k->nvf; j++) {
		if (strcmp(now[j].str, k->vf[j].str) != 0) {
			fprintf(stderr, "geforce-sriov: %s: virtfn%u is %s after the re-probe, not %s\n", k->pf.str, j, now[j].str,
				k->vf[j].str);
			return GS_EXIT_FAIL;
		}
	}
	for (j = 0; j < k->nvf; j++) {
		const struct gs_addr *vf = &k->vf[j];

		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/nvidia/creatable_vgpu_types", h->sys, vf->str);
		for (unsigned int t = 0;; t += POLL_MS) {
			dev_driver(h->sys, vf->str, drv, sizeof(drv));
			if (strcmp(drv, "nvidia") == 0 && gs_exists(p))
				break;
			if (t >= wait) {
				fprintf(stderr, "geforce-sriov: %s: no nvidia/creatable_vgpu_types %u ms after the re-probe (bound to %s);\n"
					"nvidia-vgpu-mgr publishes it (`journalctl -u nvidia-vgpu-mgr -b`). Do not start a VM on it\n",
					vf->str, wait, drv[0] ? drv : "nothing");
				return GS_EXIT_FAIL;
			}
			gs_sleep_ms(POLL_MS);
		}
		if (set_type_vf(h, vf, k->type, wait) || power_on_vf(h, vf))
			return GS_EXIT_FAIL;
	}
	return 0;
}

/* The card's VFs, all of them, each on nvidia with a power/control: k->vf and k->nvf set. 0, or -1 with why. */
static int vfs_ready(const struct gs_host *h, struct vgpu_card *k, const struct gs_card *c, char *why, size_t len)
{
	char p[GS_PATH_MAX], drv[64];
	unsigned int j;

	if (c->numvfs < 1 || c->numvfs != c->totalvfs)
		return snprintf(why, len, "%s has %u of %u VFs enabled, an unexpected state:\n"
				"write 0 to its sriov_numvfs, then run activate again", k->pf.str, c->numvfs, c->totalvfs), -1;
	if (vf_list(h->sys, &k->pf, c->numvfs, k->vf, why, len))
		return -1;
	k->nvf = c->numvfs;
	for (j = 0; j < k->nvf; j++) {
		dev_driver(h->sys, k->vf[j].str, drv, sizeof(drv));
		if (strcmp(drv, "nvidia") != 0)
			return snprintf(why, len, "VF %s of %s is bound to %s, not nvidia", k->vf[j].str, k->pf.str,
					drv[0] ? drv : "nothing"), -1;
		if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/power/control", h->sys, k->vf[j].str) || !gs_exists(p))
			return snprintf(why, len, "VF %s has no power/control", k->vf[j].str), -1;
	}
	return 0;
}

static int vfio_loaded(const struct gs_host *h)
{
	char p[GS_PATH_MAX];

	return gs_pathf(p, sizeof(p), "%s/module/nvidia_vgpu_vfio", h->sys) == 0 && gs_exists(p);
}

/*
 * The Ada slot hint. On an Ada card each VF has a fixed framebuffer slot: VF k (0 for the first, in VF
 * order) occupies [k * (P - 32), (k + 1) * (P - 32)) MiB, P the profile size of its vGPU type in MiB.
 * A VF whose slot ends near the top of the card's memory may be refused when its VM starts, even alone
 * on an empty card, while the type write succeeds. SLOT_HINT_PERCENT is a threshold for the warning,
 * not an exact limit. Ampere cards have no such limit.
 */
#define SLOT_HINT_PERCENT 75u
static int slot_high(unsigned int k, unsigned long long profile_mib, unsigned long long card_mib)
{
	return (k + 1ULL) * (profile_mib - 32) * 100 > card_mib * SLOT_HINT_PERCENT;
}

/* The profileSize of <vgpuType id="type"> in vgpuConfig.xml text, in MiB; 0 when absent or not readable. */
static unsigned long long profile_mib(const char *xml, unsigned int type)
{
	const char *p = xml, *end, *size;
	unsigned long long v;
	char tag[40], *e;
	size_t n;

	n = (size_t)snprintf(tag, sizeof(tag), "<vgpuType id=\"%u\"", type);
	while ((p = strstr(p, tag)) && p[n] != ' ' && p[n] != '>')
		p += n;
	if (!p || !(end = strstr(p, "</vgpuType>")) || !(size = strstr(p, "<profileSize>")) || size > end)
		return 0;
	size += strlen("<profileSize>");
	if (size[0] != '0' || (size[1] != 'x' && size[1] != 'X'))
		return 0;
	errno = 0;
	v = strtoull(size, &e, 16);
	if (errno || e == size + 2 || *e != '<' || (v >> 20) <= 32)
		return 0;
	return v >> 20;
}

/* memory.total of pf in MiB from `nvidia-smi --query-gpu=pci.bus_id,memory.total` lines ("00000000:03:00.0, 12282"); 0 when absent. */
static unsigned long long card_mib(char *smi, const struct gs_addr *pf)
{
	char *line, *save = NULL;

	for (line = strtok_r(smi, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		unsigned int d, b, s, f;
		unsigned long long mib;
		char extra;

		if (sscanf(line, "%x:%x:%x.%x, %llu %c", &d, &b, &s, &f, &mib, &extra) == 5 && d == pf->domain &&
		    b == pf->bus && s == pf->slot && f == pf->func)
			return mib;
	}
	return 0;
}

/*
 * After the types are set: one line for each Ada card with VFs whose slot ends above the threshold. A hint
 * only; prints nothing when vgpuConfig.xml or nvidia-smi is missing or says nothing about the card.
 */
static void slot_hint(const struct gs_host *h, const struct gs_activate_opts *o, const struct vgpu_card *cards,
		      unsigned int n)
{
	static const char *const smi[] = { "nvidia-smi", "--query-gpu=pci.bus_id,memory.total",
					   "--format=csv,noheader,nounits", NULL };
	char real[PATH_MAX], out[4096], text[4096], *xml;
	unsigned long long profile[GS_CFG_MAX_BDFS] = { 0 }, mem;
	unsigned int i, j, high, any = 0;
	size_t len;

	if (!o->vgpu_config || !h->capture || !realpath(o->vgpu_config, real) || gs_read_whole(real, &xml, &len, 64u << 20))
		return;
	for (i = 0; i < n; i++)
		if (cards[i].generation == GS_ADA && (profile[i] = profile_mib(xml, cards[i].type)))
			any = 1;
	free(xml);
	if (!any || h->capture(smi, out, sizeof(out)) != 0)
		return;
	for (i = 0; i < n; i++) {
		const struct vgpu_card *k = &cards[i];
		char list[GS_VFS_MAX * (GS_BDF_STR_SIZE + 8)] = "";
		size_t off = 0;

		memcpy(text, out, sizeof(text));
		if (!profile[i] || !(mem = card_mib(text, &k->pf)))
			continue;
		/* The slot end grows with k, so the VFs above the threshold are the last ones. */
		for (j = high = 0; j < k->nvf; j++) {
			if (!slot_high(j, profile[i], mem))
				continue;
			off += (size_t)snprintf(list + off, sizeof(list) - off, "%s%s",
						high++ == 0 ? "" : j + 1 == k->nvf ? " and " : ", ", k->vf[j].str);
		}
		if (high)
			printf("%s: with type %u the %s %s may not start: %s near the top of the card's %llu MiB. "
			       "Put VMs on the lower VFs, or choose a smaller type.\n", k->pf.str, k->type,
			       high == 1 ? "VF" : "VFs", list,
			       high == 1 ? "its framebuffer slot ends" : "their framebuffer slots end", mem);
	}
}

int gs_activate(const struct gs_host *h, const struct gs_config *cfg, const struct gs_activate_opts *o)
{
	static const char *const vgpud[] = { "systemctl", "restart", "nvidia-vgpud.service", NULL };
	static const char *const mgr[] = { "systemctl", "restart", "nvidia-vgpu-mgr.service", NULL };
	static const char *const mgr_active[] = { "systemctl", "is-active", "--quiet", "nvidia-vgpu-mgr.service", NULL };
	char why[1024], which[256], p[GS_PATH_MAX], g[GS_PATH_MAX], boot[64] = "", ver[32];
	struct host_vgpus vgpus;
	struct vgpu_card *cards;
	unsigned int n = 0, i, j;
	struct stat st;
	int r, e, rc = GS_EXIT_OK, other_version;
	int created = 0; /* activate enabled at least one card's VFs in this run */

	if (gs_pathf(p, sizeof(p), "%s/" GS_ATTEMPT_NAME, h->state_dir) || gs_exists(p)) {
		fprintf(stderr, "geforce-sriov: guard %s is set; nothing done.\n%s\n", p, recovery_msg);
		return GS_EXIT_COLD_CYCLE;
	}
	cards = calloc(GS_CFG_MAX_BDFS, sizeof(*cards));
	if (!cards)
		return GS_EXIT_FAIL;
	if ((r = select_cards(h->sys, cfg, o, cards, &n))) {
		free(cards);
		return r;
	}
	/*
	 * --boot: the boot unit runs while NVIDIA's driver may not be loaded and the cards may not be
	 * bound yet, so wait up to GS_BOOT_BIND_WAIT_MS for the version file and for each selected PF
	 * before any check judges them. What never arrives is left to the checks below, which refuse
	 * with the reason.
	 */
	if (o->boot && !o->dry_run) {
		unsigned int budget = o->boot_wait_ms ? o->boot_wait_ms : GS_BOOT_BIND_WAIT_MS;

		for (i = 0; i < n; i++) {
			struct gs_card c;
			unsigned int waited = 0;

			gs_card_inspect(h->sys, &cards[i].pf, &c);
			while (waited < budget) {
				nvidia_version(h->proc, ver, sizeof(ver));
				if (ver[0] && strcmp(c.driver, "nvidia") == 0)
					break;
				gs_sleep_ms(POLL_MS);
				waited += POLL_MS;
				gs_card_inspect(h->sys, &cards[i].pf, &c);
			}
			if (strcmp(c.driver, "nvidia") != 0)
				printf("%s: still bound to %s after %u ms\n", cards[i].pf.str,
				       c.driver[0] ? c.driver : "nothing", budget);
		}
	}

	/*
	 * Preflight: every check passes before anything changes. Another driver version than
	 * GS_NVIDIA_VERSION is not supported: its GSP and plugin checks warn instead of refusing.
	 */
	nvidia_version(h->proc, ver, sizeof(ver));
	if (!ver[0]) {
		snprintf(why, sizeof(why), "NVIDIA's driver is not loaded");
		goto refuse;
	}
	if ((other_version = gs_driver_version_line(ver, why, sizeof(why))))
		fprintf(stderr, "geforce-sriov: %s; continuing\n", why);
	if (check_gsp_off(h->proc, why, sizeof(why))) {
		if (!other_version)
			goto refuse;
		fprintf(stderr, "geforce-sriov: %s; continuing on this driver version\n", why);
	}
	if (o->plugin && check_plugin(o->plugin, why, sizeof(why))) {
		if (!other_version)
			goto refuse;
		fprintf(stderr, "geforce-sriov: %s; continuing on this driver version\n", why);
	}
	/*
	 * After a kernel update DKMS puts its stock nvidia.ko back, so activate would bring up a module without
	 * the driver step's changes. Whenever the driver step's record exists (for this kernel or another), the
	 * module must be the recorded build; only a host without any record (--driver=skip) is warned and goes on.
	 * A loaded nvidia module that is not the installed build is warned about too: a cold boot loads the
	 * installed one.
	 */
	{
		struct gs_build b;
		char line[BUILD_LINE_MAX];

		if (module_identity(o->build, is_live_root(h->sys), &b)) {
			if (build_line(&b, line, sizeof(line))) {
				if (b.record_read) {
					snprintf(why, sizeof(why),
						 "%.800s, or run `sudo geforce-sriov install`", line);
					goto refuse;
				}
				fprintf(stderr, "geforce-sriov: warning: %s\n", line);
			} else if (loaded_line(h->sys, &b, line, sizeof(line))) {
				fprintf(stderr, "geforce-sriov: warning: %s\n", line);
			}
		}
	}
	gs_boot_id(boot, sizeof(boot));
	for (i = 0; i < n; i++) {
		struct vgpu_card *k = &cards[i];
		struct gs_prepared rec;
		struct gs_card c;
		char drv[64];

		gs_card_inspect(h->sys, &k->pf, &c);
		gs_prepared_read(h->run_dir, &k->pf, &rec);
		if (gs_check_enable(&c, cfg, &rec, boot, why, sizeof(why)) || gs_check_presented(&c, cfg, why, sizeof(why)))
			goto refuse;
		if (strcmp(c.driver, "nvidia") != 0) {
			snprintf(why, sizeof(why), "%s is bound to %s, not nvidia", k->pf.str, c.driver[0] ? c.driver : "nothing");
			goto refuse;
		}
		if (read_dev_attr(h->sys, k->pf.str, "driver_override", drv, sizeof(drv)) == 0 && drv[0] &&
		    strcmp(drv, "(null)") != 0 && strcmp(drv, "nvidia") != 0) {
			snprintf(why, sizeof(why), "%s has driver_override=%s, so the re-probe could not bind it to nvidia. Clear it first:\n"
				 "`echo > /sys/bus/pci/devices/%s/driver_override`", k->pf.str, drv, k->pf.str);
			goto refuse;
		}
		if (c.numvfs == 0)
			k->enable = 1; /* enabled below, after the checks */
		else if (vfs_ready(h, k, &c, why, sizeof(why)))
			goto refuse;
		if (gs_pathf(p, sizeof(p), "%s/driver/nvidia/gpus/%s/unbindLock", h->proc, k->pf.str) || !gs_exists(p)) {
			snprintf(why, sizeof(why), "%.300s is missing: the re-probe needs NVIDIA's unbindLock for %s", p, k->pf.str);
			goto refuse;
		}
	}
	for (i = 0; i < n; i++)
		if (cards[i].enable) {
			if (enable_prereq(cfg, why, sizeof(why)))
				goto refuse;
			break;
		}
	/* The daemons serve every GPU on the host: a restart would end any vGPU there. */
	if ((r = host_vgpu(h->sys, &vgpus)) < 0) {
		snprintf(why, sizeof(why), "%s", vgpus.first);
		goto refuse;
	}
	if (r) {
		fprintf(stderr, "geforce-sriov: refused, nothing changed: each VF below has a vGPU, and restarting NVIDIA's vGPU "
			"daemons would end it:\n%s"
			"Stop the VMs that use them, then write 0 to each nvidia/current_vgpu_type, for example:\n"
			"  for f in %s; do echo 0 > /sys/bus/pci/devices/$f/nvidia/current_vgpu_type; done\n",
			vgpus.lines, vgpus.bdfs);
		free(cards);
		return GS_EXIT_REFUSED;
	}
	/* The restart also re-registers the VFs of GPUs outside the selection: refused. */
	if ((r = foreign_vf(h->sys, cards, n, which, sizeof(which)))) {
		if (r < 0)
			snprintf(why, sizeof(why), "%s", which);
		else
			snprintf(why, sizeof(why), "%s: that GPU is not selected, and the restart of NVIDIA's vGPU daemons would\n"
				 "re-register it too. Disable its VFs first", which);
		goto refuse;
	}

	if (o->dry_run) {
		printf("dry run: every check passed; activate would run, in this order:\n");
		for (i = 0; i < n; i++)
			if (cards[i].enable)
				printf("  %s: %s -e %s (it has no VFs), then wait until its VFs are bound to nvidia\n", cards[i].pf.str,
				       cfg->sriov_manage, cards[i].pf.str);
		if (!vfio_loaded(h))
			printf("  modprobe nvidia_vgpu_vfio\n");
		printf("  systemctl restart nvidia-vgpud.service\n"
		       "  systemctl restart nvidia-vgpu-mgr.service, then wait until it is active\n");
		for (i = 0; i < n; i++)
			printf("  %s: unbindLock, then echo %s > /sys/bus/pci/drivers/nvidia/unbind and .../nvidia/bind\n",
			       cards[i].pf.str, cards[i].pf.str);
		for (i = 0; i < n; i++) {
			if (cards[i].enable)
				printf("  %s: each of its VFs: echo %u > nvidia/current_vgpu_type, echo on > power/control, then read back on and active\n",
				       cards[i].pf.str, cards[i].type);
			for (j = 0; j < cards[i].nvf; j++)
				printf("  %s: echo %u > nvidia/current_vgpu_type, echo on > power/control, then read back on and active\n",
				       cards[i].vf[j].str, cards[i].type);
		}
		free(cards);
		return GS_EXIT_OK;
	}

	/*
	 * From the first change until the final readback, check reports NOT READY. The record stays
	 * on every failure, so check fails closed until an activate finishes (or the next boot).
	 */
	{
		char rec[GS_BDF_LIST_STR_SIZE + 128];
		size_t off;

		gs_boot_id(boot, sizeof(boot));
		off = (size_t)snprintf(rec, sizeof(rec), "boot_id=%s\npfs=", boot);
		for (i = 0; i < n && off < sizeof(rec); i++)
			off += (size_t)snprintf(rec + off, sizeof(rec) - off, "%s%s", i ? "," : "", cards[i].pf.str);
		if (off < sizeof(rec))
			off += (size_t)snprintf(rec + off, sizeof(rec) - off, "\n");
		if (off >= sizeof(rec) || gs_pathf(p, sizeof(p), "%s/" GS_ACTIVATING_NAME, h->run_dir) ||
		    (r = gs_mkdir_p(h->run_dir, 0755)) || (r = gs_write_file_atomic(p, rec, off, 0644))) {
			fprintf(stderr, "geforce-sriov: cannot write %s/" GS_ACTIVATING_NAME "; nothing changed\n", h->run_dir);
			free(cards);
			return GS_EXIT_FAIL;
		}
	}
	/* The bring-up order: the VFs exist, nvidia_vgpu_vfio is loaded, both daemons start, then the PF re-probe. */
	for (i = 0; i < n; i++)
		if (cards[i].enable) {
			printf(SRIOV_MANAGE_NOTE);
			break;
		}
	for (i = 0; i < n; i++) {
		struct vgpu_card *k = &cards[i];
		struct gs_card c;

		if (!k->enable)
			continue;
		gs_card_inspect(h->sys, &k->pf, &c);
		if ((r = enable_pf(h, cfg, &k->pf, &c))) {
			fprintf(stderr, "geforce-sriov: activate stopped at %s before nvidia_vgpu_vfio was loaded and the daemons were\n"
				"restarted; no GPU was re-probed\n", k->pf.str);
			rc = r;
			goto stopped;
		}
		created = 1;
		if (vfs_ready(h, k, &c, why, sizeof(why))) {
			fprintf(stderr, "geforce-sriov: %s\nno GPU was re-probed\n", why);
			rc = GS_EXIT_FAIL;
			goto stopped;
		}
	}
	if (!vfio_loaded(h)) {
		static const char *const vfio[] = { "modprobe", "nvidia_vgpu_vfio", NULL };

		printf("modprobe nvidia_vgpu_vfio\n");
		if ((r = h->run(vfio))) {
			fprintf(stderr, "geforce-sriov: modprobe nvidia_vgpu_vfio exited with %d (`journalctl -k -b` says why); nvidia-vgpud\n"
				"and nvidia-vgpu-mgr were not restarted and no GPU was re-probed%s. Install NVIDIA's vGPU host package\n"
				"so that `modprobe nvidia_vgpu_vfio` works, then run activate again\n",
				r, created ? "; the VFs created above stay enabled" : "");
			/* Exit 3 means nothing changed; once a card was enabled, that is no longer true. */
			rc = created ? GS_EXIT_FAIL : GS_EXIT_REFUSED;
			goto stopped;
		}
	}
	printf("systemctl restart nvidia-vgpud.service\n");
	if ((r = h->run(vgpud))) {
		fprintf(stderr, "geforce-sriov: systemctl restart nvidia-vgpud.service failed (%d); no GPU was re-probed\n"
			"(`journalctl -u nvidia-vgpud -b` says why)\n", r);
		rc = GS_EXIT_FAIL;
		goto stopped;
	}
	/* A VM may have started since the preflight: nvidia-vgpud restarted, nothing else changed yet. */
	if ((r = host_vgpu(h->sys, &vgpus))) {
		fprintf(stderr, "geforce-sriov: %s since the checks; nvidia-vgpud was restarted, nvidia-vgpu-mgr was not and no GPU\n"
			"was re-probed\n", vgpus.first);
		rc = GS_EXIT_FAIL;
		goto stopped;
	}
	printf("systemctl restart nvidia-vgpu-mgr.service\n");
	if ((r = h->run(mgr))) {
		fprintf(stderr, "geforce-sriov: systemctl restart nvidia-vgpu-mgr.service failed (%d); no GPU was re-probed\n"
			"(`journalctl -u nvidia-vgpu-mgr -b` says why)\n", r);
		rc = GS_EXIT_FAIL;
		goto stopped;
	}
	for (unsigned int t = 0; h->run(mgr_active) != 0; t += POLL_MS) {
		if (t >= MGR_ACTIVE_WAIT_MS) {
			fprintf(stderr, "geforce-sriov: nvidia-vgpu-mgr.service is not active %u ms after its restart; no GPU was re-probed\n",
				MGR_ACTIVE_WAIT_MS);
			rc = GS_EXIT_FAIL;
			goto stopped;
		}
		gs_sleep_ms(POLL_MS);
	}
	if (o->settle_ms)
		gs_sleep_ms(o->settle_ms);
	for (i = 0; i < n; i++) {
		if ((r = reprobe_pf(h, &cards[i].pf)) == 0)
			continue;
		if (r == GS_EXIT_COLD_CYCLE) {
			rc = r;
			goto stopped;
		}
		/* The daemons were restarted: that is a change, so this is a failure, not a refusal. */
		fprintf(stderr, "geforce-sriov: activate stopped at %s after nvidia-vgpud and nvidia-vgpu-mgr were restarted;\n"
			"%u GPU(s) before it were re-probed and are finished now; it and the GPUs after it are not ready\n",
			cards[i].pf.str, i);
		for (j = 0; j < i; j++)
			finish_card(h, &cards[j], o->types_wait_ms ? o->types_wait_ms : VF_TYPES_WAIT_MS);
		rc = GS_EXIT_FAIL;
		goto stopped;
	}
	/* Every re-probe has run: power/control=on is written and read back last, after the VFs' final registration. */
	for (i = 0; i < n; i++)
		if (finish_card(h, &cards[i], o->types_wait_ms ? o->types_wait_ms : VF_TYPES_WAIT_MS))
			rc = GS_EXIT_FAIL;
	if (rc == GS_EXIT_OK && unlink(p) < 0 && errno != ENOENT) {
		fprintf(stderr, "geforce-sriov: cannot remove %s: %s; check stays NOT READY\n", p, strerror(errno));
		rc = GS_EXIT_FAIL;
	}
	if (rc == GS_EXIT_OK) {
		char arg[32] = "";
		int verdict;

		slot_hint(h, o, cards, n);
		if (o->type)
			snprintf(arg, sizeof(arg), " --type %u", o->type);
		printf("\nthe same lines `geforce-sriov check%s` prints:\n", arg);
		verdict = gs_check_vfs(h, cfg, o);
		printf("If nvidia-vgpu-mgr restarts or another card's PF is re-probed, VFs can fall back to power/control=auto:\n"
		       "then stop the VMs on this host and run activate again\n");
		free(cards);
		return verdict;
	}
stopped:
	/* Only ENOENT proves the guard gone: exit 5, a guard that stays and an unknown state count as set. */
	e = rc == GS_EXIT_COLD_CYCLE ? 0 : -gs_pathf(g, sizeof(g), "%s/" GS_ATTEMPT_NAME, h->state_dir);
	if (rc != GS_EXIT_COLD_CYCLE && e == 0 && lstat(g, &st) < 0)
		e = errno;
	if (e == 0)
		fprintf(stderr, "geforce-sriov: %s and the guard stay, so `geforce-sriov check` reports NOT READY and `geforce-sriov\n"
			"activate` refuses (exit 5). Power the machine off and on, then run `geforce-sriov prepare --clear-guard`\n",
			p);
	else if (e != ENOENT)
		fprintf(stderr, "geforce-sriov: %s stays and the guard may be set (%s), so `geforce-sriov check` reports NOT READY.\n"
			"Treat the guard as set: power the machine off and on, then run `geforce-sriov prepare --clear-guard`\n",
			p, strerror(e));
	else
		fprintf(stderr, "geforce-sriov: %s stays, so `geforce-sriov check` reports NOT READY until an activate\n"
			"finishes or the host reboots\n", p);
	free(cards);
	return rc;
refuse:
	fprintf(stderr, "geforce-sriov: refused, nothing changed: %s\n", why);
	free(cards);
	return GS_EXIT_REFUSED;
}

int gs_cmd_activate(const struct gs_activate_opts *in)
{
	struct gs_activate_opts o = *in;
	struct gs_config cfg;
	char prog[GS_PATH_MAX];
	int r;

	if ((r = require_root("activate")))
		return r;
	if ((r = gs_lock()))
		return r;
	if ((r = load_live_config(&cfg)))
		return r;
	if (gs_find_program("systemctl", prog, sizeof(prog))) {
		fprintf(stderr, "geforce-sriov: refused, nothing changed: systemctl not found in /usr/sbin:/usr/bin:/sbin:/bin\n");
		return GS_EXIT_REFUSED;
	}
	/* A missing plugin link is refused by its check. */
	o.plugin = plugin_link();
	o.vgpu_config = GS_VGPU_CONFIG;
	o.settle_ms = MGR_SETTLE_MS;
	return gs_activate(&gs_host_live, &cfg, &o);
}

int gs_check_vfs(const struct gs_host *h, const struct gs_config *cfg, const struct gs_activate_opts *o)
{
	const char *sys = h->sys;
	struct vgpu_card *cards;
	unsigned int n = 0, i, j;
	int r, ready = 1;
	struct module_state m;

	read_module_state(sys, &m);

	cards = calloc(GS_CFG_MAX_BDFS, sizeof(*cards));
	if (!cards)
		return GS_EXIT_FAIL;
	if ((r = select_cards(sys, cfg, o, cards, &n))) {
		free(cards);
		return r;
	}
	/* An unfinished activate or prepare: the VF state read now may be about to change. */
	for (i = 0; i < 2; i++) {
		char p[GS_PATH_MAX];
		struct stat st;

		if (gs_pathf(p, sizeof(p), "%s/%s", i ? h->run_dir : h->state_dir, i ? GS_ACTIVATING_NAME : GS_ATTEMPT_NAME)) {
			printf("NOT READY: the %s path is too long\n", i ? GS_ACTIVATING_NAME : GS_ATTEMPT_NAME);
			free(cards);
			return GS_EXIT_FAIL;
		}
		if (lstat(p, &st) == 0) {
			printf("NOT READY: %s exists: %s did not finish; start no VM\n", p,
			       i ? "`geforce-sriov activate` is running or" : "a preparation or re-probe");
			free(cards);
			return GS_EXIT_FAIL;
		}
		if (errno != ENOENT) {
			printf("NOT READY: cannot tell whether %s exists: %s\n", p, strerror(errno));
			free(cards);
			return GS_EXIT_FAIL;
		}
	}
	/* The driver version and the driver step's build are reported, never judged: the VF lines below decide the exit code. */
	{
		char ver[32], line[BUILD_LINE_MAX], loaded[256];
		struct gs_build b;

		nvidia_version(h->proc, ver, sizeof(ver));
		if (ver[0]) {
			gs_driver_version_line(ver, line, sizeof(line));
			printf("%s\n", line);
		}
		if (module_identity(o->build, is_live_root(sys), &b)) {
			build_line(&b, line, sizeof(line));
			if (loaded_line(sys, &b, loaded, sizeof(loaded)))
				printf("%s; %s\n", line, loaded);
			else
				printf("%s\n", line);
		}
	}
	for (i = 0; i < n; i++) {
		struct vgpu_card *k = &cards[i];
		char why[512], want[16];
		struct gs_card c;

		snprintf(want, sizeof(want), "%u", k->type);
		gs_card_inspect(sys, &k->pf, &c);
		if (strcmp(c.driver, "nvidia") != 0) {
			printf("NOT READY %s: bound to %s, not nvidia\n", k->pf.str, c.driver[0] ? c.driver : "nothing");
			ready = 0;
			continue;
		}
		if (c.numvfs < 1) {
			printf("NOT READY %s: no VF is enabled (`geforce-sriov activate` enables them)\n", k->pf.str);
			ready = 0;
			continue;
		}
		if (vf_list(sys, &k->pf, c.numvfs, k->vf, why, sizeof(why))) {
			printf("NOT READY %s\n", why);
			ready = 0;
			continue;
		}
		/* A laptop GPU powered off since prepare (warnings only; the VF lines decide). */
		{
			char lost[2][256];
			unsigned int nlost = laptop_lost(sys, h->run_dir, &c, &m, lost);

			for (j = 0; j < nlost; j++)
				printf("WARNING %s\n", lost[j]);
		}
		for (j = 0; j < c.numvfs; j++) {
			char drv[64], ctl[32], st[32], type[32], dir[GS_PATH_MAX];
			unsigned int id08 = 0xffffffffu;
			int ok, dead = 0;

			dev_driver(sys, k->vf[j].str, drv, sizeof(drv));
			read_dev_attr(sys, k->vf[j].str, "power/control", ctl, sizeof(ctl));
			read_dev_attr(sys, k->vf[j].str, "power/runtime_status", st, sizeof(st));
			ok = read_dev_attr(sys, k->vf[j].str, "nvidia/current_vgpu_type", type, sizeof(type)) == 0 &&
			     strcmp(drv, "nvidia") == 0 && strcmp(ctl, "on") == 0 && strcmp(st, "active") == 0 &&
			     strcmp(type, want) == 0;
			/*
			 * A laptop VF's liveness: its revision/class dword (offset 0x08). A VF's vendor and device IDs read
			 * ffff by design, so they are no test; a VF that does not answer reads ffffffff here too.
			 */
			if (c.entry && c.entry->laptop &&
			    (gs_pathf(dir, sizeof(dir), "%s/bus/pci/devices/%s", sys, k->vf[j].str) ||
			     config_dword(dir, 0x08, &id08) || id08 == 0xffffffffu))
				ok = 0, dead = 1;
			printf("%s %s (VF of %s): driver=%s type=%s expected=%s power/control=%s runtime_status=%s\n",
			       ok ? "OK" : "NOT READY", k->vf[j].str, k->pf.str, drv[0] ? drv : "none", type[0] ? type : "unreadable",
			       want, ctl[0] ? ctl : "unreadable", st[0] ? st : "unreadable");
			if (dead)
				printf("NOT READY %s: its config space reads ffffffff at 0x08, or cannot be read; the GPU may have "
				       "been powered off\n", k->vf[j].str);
			if (!ok)
				ready = 0;
		}
	}
	/* An activate or prepare that started during the reads: what was read above may be changing. */
	for (i = 0; i < 2; i++) {
		char p[GS_PATH_MAX];
		struct stat st;

		if (gs_pathf(p, sizeof(p), "%s/%s", i ? h->run_dir : h->state_dir, i ? GS_ACTIVATING_NAME : GS_ATTEMPT_NAME)) {
			printf("NOT READY: the %s path is too long\n", i ? GS_ACTIVATING_NAME : GS_ATTEMPT_NAME);
			free(cards);
			return GS_EXIT_FAIL;
		}
		if (lstat(p, &st) == 0) {
			printf("NOT READY: %s appeared during the check (%s); the readings above do not count; start no VM\n",
			       p, i ? "`geforce-sriov activate` started" : "a preparation or re-probe started");
			free(cards);
			return GS_EXIT_FAIL;
		}
		if (errno != ENOENT) {
			printf("NOT READY: cannot tell whether %s exists: %s; the readings above do not count\n", p,
			       strerror(errno));
			free(cards);
			return GS_EXIT_FAIL;
		}
	}
	free(cards);
	if (!ready)
		fprintf(stderr, "geforce-sriov: not ready: start no VM on a VF marked NOT READY. After a PF re-probe or a restart of\n"
			"nvidia-vgpu-mgr, stop the VMs that use a vGPU and run `geforce-sriov activate` again\n");
	return ready ? GS_EXIT_OK : GS_EXIT_FAIL;
}

int gs_cmd_check(const struct gs_activate_opts *o)
{
	const char *path = gs_config_path();
	struct gs_config cfg;
	char err[512];

	if (gs_config_load(path, &cfg, err, sizeof(err))) {
		fprintf(stderr, "geforce-sriov: config: %s: %s\n", path, err);
		return GS_EXIT_REFUSED;
	}
	if (cfg.nbdfs == 0) {
		fprintf(stderr, "geforce-sriov: no device selected (bdfs= in %s)\n", path);
		return GS_EXIT_REFUSED;
	}
	return gs_check_vfs(&gs_host_live, &cfg, o);
}
