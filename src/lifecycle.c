// SPDX-License-Identifier: MIT
/*
 * devices, doctor, status, check (read-only, any sysfs root) and prepare, enable, activate
 * (live system; prepare and activate also against a fixture host in the tests).
 *
 * The prepare sequence follows these rules: one guarded attempt, refuse the
 * boot display unless the config grants its console release and the operator
 * consents by hand, refuse shared buses, check every function for a bound
 * driver, hold the bridge awake, fail loudly, and never retry a rescan when a
 * PF is lost: recovery is a cold power cycle, never a repeated rescan.
 */
#include <dirent.h>
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
#include <time.h>
#include <unistd.h>

#include "gs_hook.h"
#include "gs_lifecycle.h"

#define LIVEPATCH_WAIT_MS 15000
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
/* The measured bring-up paused 3 s after starting nvidia-vgpu-mgr before the PF re-probe. */
#define MGR_SETTLE_MS 3000

static const char *const recovery_msg =
	"Recovery: do not rescan again and do not warm-reboot in a loop. Power the machine fully off\n"
	"(cold power cycle), boot, run `geforce-sriov doctor`, then `geforce-sriov prepare --clear-guard`.\n"
	"Until then the boot unit refuses to run.";

/* The next step of a deferred boot; %s is the --release-console consent. */
#define CONSENT_NEXT "Next, after boot, as root: `geforce-sriov doctor`, then `geforce-sriov prepare %s`"
/* What may fix an unassigned BAR; prepare never resizes the bridge windows above the card. */
#define BAR_REMEDY "enabling Above 4G decoding in the firmware or booting with pci=realloc may help"
/* Doctor's note for every selected Blackwell card. */
#define BLACKWELL_DRIVER_NOTE \
	"the closed 580.159.01 driver does not support this GPU; Blackwell needs NVIDIA's open kernel module, " \
	"which geforce-sriov does not support yet"
/* Doctor's note for every selected Ada card: what the package does not provide. */
#define ADA_STACK_NOTE \
	"Ada needs your own NVIDIA 580.159.01 proprietary (CPU-RM) host stack: nvidia.ko rebuilt with the Ada probe-gate " \
	"and scheduler changes, loaded with NVreg_EnableGpuFirmware=0, and the H5 copy of libnvidia-vgpu.so " \
	"(docs/ada.md); geforce-sriov builds and installs none of them. After `enable`: `geforce-sriov activate`, " \
	"and `geforce-sriov check` before every VM start"
/* The measured Ada host stack (docs/ada.md); activate refuses anything else. */
#define ADA_DRIVER_VERSION "580.159.01"
#define ADA_PLUGIN_STOCK_SHA "7e52b76dc7ca8490e7ffbc1fd724f5db07f3791a3a2ec07f2d595bf05abdcaa7"
#define ADA_PLUGIN_H5_SHA "097f571588f3a232265103b73e38ba111ddb44ef329ac720562cff71e6170d37"
/* The module refuses to load with twin_id=1 when no listed PF has twin dwords. */
#define TWIN_ID_NONE "twin_id=on, but no selected GPU is a GB202, so the module refuses to load"

static int cmdline_noari(const char *procroot);
static const struct gs_display *granted_display(const struct gs_config *cfg, const char *sys, const char *proc,
						const struct gs_card *c, struct gs_display *buf);
static void consent_args(const struct gs_config *cfg, char *buf, size_t len);

static const char *sw(int v)
{
	return v == GS_SW_OFF ? "off" : "on";
}

int gs_cmd_devices(void)
{
	unsigned int i;

	printf("geforce-sriov %s device table (exact IDs only; %u VFs per GPU by default, Ada/Blackwell 1)\n",
	       GS_VERSION, GS_DEFAULT_VFS);
	printf("%-9s %-10s %-7s %-40s %-11s %-6s %s\n", "DEVICE", "GEN", "DIE", "NAME", "VGPU TARGET", "HW VFS",
	       "STATUS");
	for (i = 0; i < GS_DEVICE_COUNT; i++) {
		const struct gs_device *e = &gs_devices[i];

		printf("10de:%04x %-10s %-7s %-40s 10de:%04x   %-6u %s%s\n", e->device,
		       gs_generation_name(e->generation), e->die, e->name, e->target, e->totalvfs,
		       e->experimental ? "experimental" : "supported",
		       e->same_die ? "" : "; cross-die target");
	}
	printf("\nVGPU TARGET: the professional PCI device ID presented to nvidia-vgpud for that card.\n");
	printf("HW VFS: the card's hardware default TotalVFs. geforce-sriov presents %u by default (Ada/Blackwell: 1,\n"
	       "see experimental_vfs); `install --vfs N` (vfs=N in the config) sets 1 up to that value. ARI decoding\n"
	       "in the GPU does not work for now, so more than %u VFs are expected not to work.\n", GS_DEFAULT_VFS,
	       GS_DEFAULT_VFS);
	printf("experimental: this device ID is not verified end to end.\n");
	printf("Cross-die entries: %s\n", GS_NO_SAME_DIE_MSG);
	printf("%s\n", GS_EXPVFS_OPT_MSG);
	printf("%s\n", GS_TWIN_ID_OPT_MSG);
	printf("Turing, laptop and professional IDs are excluded. Exposing a capability is not a working vGPU.\n");
	return GS_EXIT_OK;
}

/* ---- shared readers ---------------------------------------------------- */

struct module_state {
	int loaded;
	char ari[8], msix[8], expvfs[8], twinid[8], vfs[8], bdfs[256];
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
	if (!gs_pathf(p, sizeof(p), "%s/module/%s/parameters/experimental_vfs", sysroot, GS_MODULE_NAME))
		gs_read_attr(p, m->expvfs, sizeof(m->expvfs));
	if (!gs_pathf(p, sizeof(p), "%s/module/%s/parameters/twin_id", sysroot, GS_MODULE_NAME))
		gs_read_attr(p, m->twinid, sizeof(m->twinid));
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
			return snprintf(why, len, "directory %.300s is not a root-owned directory writable only by root", dir),
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

/* A unit file for name in one of systemd's system unit directories (a link counts). */
static int unit_file_present(const char *name)
{
	static const char *const dirs[] = { "/etc/systemd/system", "/run/systemd/system", "/usr/local/lib/systemd/system",
					    "/usr/lib/systemd/system", "/lib/systemd/system" };
	char p[GS_PATH_MAX];

	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
		if (!gs_pathf(p, sizeof(p), "%s/%s", dirs[i], name) && gs_exists(p))
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
 * What the device table says about the card: experimental rows and the VF override are warnings; a
 * VF count above what the card offers fails (prepare refuses it). Blackwell gets the driver note.
 */
static void entry_checks(struct tally *t, const struct gs_card *c, const struct gs_config *cfg)
{
	const struct gs_device *e = c->entry;
	char why[512];

	printf("  %s (%s, %s) -> vGPU target 10de:%04x %s\n", e->name, e->die, gs_generation_name(e->generation),
	       e->target, e->target_name);
	gs_vfs_describe(e, cfg, why, sizeof(why));
	printf("  VFs: %s\n", why);
	if (!e->same_die)
		check(t, "WARN", "%s: %s", c->pf.str, GS_NO_SAME_DIE_MSG);
	if (e->experimental)
		check(t, "WARN", "%s: experimental device ID 10de:%04x: not verified end to end", c->pf.str, e->device);
	switch (gs_vfs_check(e, cfg, why, sizeof(why))) {
	case -1:
		check(t, "FAIL", "%s: %s; prepare refuses it", c->pf.str, why);
		break;
	case 1:
		check(t, "WARN", "%s: %s", c->pf.str, why);
		break;
	}
	if (gs_expvfs_msg(e)) {
		if (cfg->experimental_vfs == GS_SW_ON)
			check(t, "WARN", "%s: experimental_vfs=on: geforce-sriov presents up to %u VFs. %s Off keeps 1",
			      c->pf.str, gs_vf_limit(e, 1, 1, gs_config_vfs(cfg)), gs_expvfs_msg(e));
		else
			check(t, "INFO", "%s: experimental_vfs=off: the card presents its hardware count, %u VF",
			      c->pf.str, e->totalvfs);
	}
	if (e->generation == GS_BLACKWELL)
		check(t, "INFO", "%s: " BLACKWELL_DRIVER_NOTE, c->pf.str);
	if (e->generation == GS_ADA)
		check(t, "INFO", "%s: " ADA_STACK_NOTE, c->pf.str);
	if (cfg->twin_id == GS_SW_ON && gs_has_twin_dwords(e))
		check(t, "WARN", "%s: twin_id=on: %s", c->pf.str, GS_TWIN_ID_MSG);
	else if (cfg->twin_id == GS_SW_ON)
		check(t, "INFO", "%s: twin_id=on: %s", c->pf.str, GS_TWIN_ID_ONLY_MSG);
}

/* 1 when a selected card is in the device table with twin dwords (twin_id acts on it). */
static int selection_has_twin(const char *sysroot, const struct gs_addr *sel, unsigned int n)
{
	for (unsigned int i = 0; i < n; i++) {
		struct gs_card c;

		gs_card_inspect(sysroot, &sel[i], &c);
		if (c.present && c.entry && gs_has_twin_dwords(c.entry))
			return 1;
	}
	return 0;
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
	check(t, "WARN", "%s: IOMMU group %s also holds %s: VFs may share the group, are then not isolated from those devices or each other, and VFIO may refuse them (an ACS override adds no real isolation)",
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
		check(t, "WARN", "identity: cannot confirm the mapping: %s is unreadable or invalid (or more than %u NVIDIA functions); the hook reads the same entries and may leave the selected GPUs unchanged",
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
		check(t, "OK", "identity: selected %s (10de:%04x) is matched by bus address and presented to nvidia-vgpud as 10de:%04x %s (the driver's address queries are checked when nvidia-vgpud runs)",
		      c.pf.str, c.device, c.entry->target, c.entry->target_name);
	}
	/* The device-ID fallback, per mapped card: refused while another, unmapped PF has the same ID. */
	for (i = 0; i < nmapped; i++) {
		for (j = 0; j < nfns; j++) {
			int selected = addr_listed(sel, nsel, fns[j].a.str);

			if (fns[j].device != mdev[i] || addr_listed(mapped, nmapped, fns[j].a.str))
				continue;
			if (!selected)
				check(t, "INFO", "identity: unselected %s has the same device ID; matched by bus address it stays unchanged and %s is rewritten alone",
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

int gs_cmd_doctor(const struct gs_diag_opts *o)
{
	const char *sysroot = o->sysroot ? o->sysroot : GS_LIVE_SYSFS;
	int live = is_live_root(sysroot);
	struct gs_addr devs[GS_CFG_MAX_BDFS];
	struct module_state m;
	struct gs_config cfg;
	struct tally t = { 0, 0 };
	const char *cfg_path, *source, *isrc;
	const struct gs_addr *isel;
	char err[512], p[GS_PATH_MAX];
	unsigned int n, i, nisel;
	int id_on, id_force;

	printf("geforce-sriov %s doctor (sysfs root %s%s)\n", GS_VERSION, sysroot,
	       live ? "" : ", diagnostic fixture: host checks skipped");
	if (diag_config(o, &cfg, &cfg_path, err, sizeof(err))) {
		check(&t, "FAIL", "config: %s", err);
		gs_config_defaults(&cfg);
	} else if (!cfg_path) {
		check(&t, "INFO", "config: not read (fixture root; pass --config)");
	} else if (!cfg.present) {
		check(&t, "INFO", "config: %s absent; defaults apply and no device is selected", cfg_path);
	} else {
		check(&t, "OK", "config: %s: %u device(s) selected; ari=%s msix=%s experimental_vfs=%s twin_id=%s vfs=%u%s identity=%s",
		      cfg_path, cfg.nbdfs, sw(cfg.ari), sw(cfg.msix), sw(cfg.experimental_vfs), sw(cfg.twin_id),
		      gs_config_vfs(&cfg), cfg.vfs ? "" : " (default)", cfg.identity == GS_SW_ON ? "on" : "off");
		if (live && config_file_safe(cfg_path, err, sizeof(err)))
			check(&t, "FAIL", "config: %s; prepare and enable refuse it", err);
		if (cfg.nrelease) {
			char rel[GS_BDF_LIST_STR_SIZE], consent[GS_CFG_MAX_BDFS * 40];

			gs_addr_list_str(cfg.boot_display_release, cfg.nrelease, rel, sizeof(rel));
			consent_args(&cfg, consent, sizeof(consent));
			check(&t, "INFO", "boot_display_release=%s: the boot unit defers and prepares none of the selected GPUs.\n"
			      "       " CONSENT_NEXT, rel, consent);
			print_indented("       ", GS_CONSOLE_RELEASE_NOTE);
		}
	}
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
		      "module %s loaded: ari=%s msix=%s experimental_vfs=%s twin_id=%s vfs=%s bdfs=%s; livepatch %s",
		      GS_MODULE_NAME, m.ari, m.msix, m.expvfs, m.twinid, m.vfs[0] ? m.vfs : "(none)",
		      m.bdfs[0] ? m.bdfs : "(none)",
		      !m.lp_present ? "directory missing" : m.lp_ok ? "enabled, transition complete" :
		      "not enabled or still in transition");
	if (m.loaded && module_vfs(&m) != gs_config_vfs(&cfg))
		check(&t, "WARN", "module %s was loaded with vfs=%u, the config has vfs=%u: the count is fixed at module load, so the new count takes effect after the next boot",
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
		/* What the later steps need: a live install (dkms, systemctl). */
		if (dkms && sysctl)
			check(&t, "OK", "dkms and systemctl found (a live install needs both)");
		else
			check(&t, "WARN", "%s not found: a live `geforce-sriov install` (also `install --device BDF`) refuses without dkms and systemctl%s",
			      !dkms && !sysctl ? "dkms and systemctl" : !dkms ? "dkms" : "systemctl",
			      mod ? " (the module is already installed for this kernel)" : "");
		if (gs_exists(GS_ATTEMPT_MARKER)) {
			check(&t, "FAIL", "guard %s is set: a preparation attempt did not finish.", GS_ATTEMPT_MARKER);
			print_indented("       ", recovery_msg);
		} else {
			check(&t, "OK", "no unfinished preparation attempt");
		}
		if (cfg_path && cfg.nbdfs && strcmp(cfg_path, GS_CONFIG_PATH) == 0) {
			if (!gs_exists(GS_UNIT_WANTS))
				check(&t, "WARN", "%s is not enabled: the next boot does not prepare the selected GPUs (`geforce-sriov install --device BDF` enables it)",
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
			check(&t, "WARN", "%s loads an nvidia module at boot; the PF may be bound before preparation (prepare then refuses)",
			      where);
			break;
		case 2:
			check(&t, "WARN", "%s puts nvidia into the initramfs configuration; the PF may be bound before preparation (prepare then refuses)",
			      where);
			break;
		default:
			check(&t, "INFO", "no nvidia module in modules-load.d, /etc/modules or initramfs-tools/dracut configuration (built initramfs images are not inspected)");
		}
		if (gs_find_program(cfg.sriov_manage, p, sizeof(p)) == 0)
			check(&t, "OK", "sriov-manage found at %s", cfg.sriov_manage);
		else
			check(&t, "WARN", "%s not found: `enable` needs NVIDIA's vGPU host driver package", cfg.sriov_manage);
		{
			int vgpud = unit_file_present("nvidia-vgpud.service");
			int mgr = unit_file_present("nvidia-vgpu-mgr.service");

			if (vgpud && mgr)
				check(&t, "OK", "nvidia-vgpud.service and nvidia-vgpu-mgr.service unit files found");
			else
				check(&t, "WARN", "%s not found in systemd's unit directories: without NVIDIA's vGPU daemons no vGPU types appear.\n"
				      "       NVIDIA's host package ships the unit files (for example in /usr/lib/nvidia/systemd/): copy them\n"
				      "       unchanged to /etc/systemd/system and run `systemctl daemon-reload`",
				      !vgpud && !mgr ? "nvidia-vgpud.service and nvidia-vgpu-mgr.service" :
				      !vgpud ? "nvidia-vgpud.service" : "nvidia-vgpu-mgr.service");
		}
		/* `enable`: sriov-manage needs lspci and setpci, found through the PATH gs_run gives it. */
		for (size_t k = 0; k < sizeof(deps) / sizeof(deps[0]); k++) {
			if (gs_find_program(deps[k], p, sizeof(p)) == 0)
				continue;
			deps_ok = 0;
			check(&t, "WARN", "%s (pciutils) not found: `enable` refuses without it (sriov-manage needs lspci and setpci)",
			      deps[k]);
		}
		if (deps_ok)
			check(&t, "OK", "lspci and setpci found (sriov-manage needs them for `enable`)");
		nvidia_version(GS_LIVE_PROC, ver, sizeof(ver));
		if (ver[0]) {
			int known = 0;

			for (i = 0; i < GS_HOOK_DRIVER_VERSION_COUNT; i++)
				known |= strcmp(ver, gs_hook_driver_versions[i]) == 0;
			check(&t, known || !id_on ? "INFO" : "WARN",
			      "NVIDIA driver %s loaded%s", ver,
			      known ? "; the identity hook supports it" :
			      "; the identity hook passes this version through unchanged");
		}
	}

	n = selected_devices(o, &cfg, sysroot, devs, &source);
	if (n == 0)
		check(&t, "WARN", "no NVIDIA display function found (%s)", source);
	for (i = 0; i < n; i++) {
		struct gs_card c;
		struct gs_plan plan;
		struct gs_display disp;
		const struct gs_display *d;
		int ours, prepared = 0;

		gs_card_inspect(sysroot, &devs[i], &c);
		ours = module_selects(&m, &c.pf);
		if (live && !gs_pathf(p, sizeof(p), "%s/%s", GS_PREPARED_DIR, c.pf.str))
			prepared = gs_exists(p);
		d = c.present ? granted_display(&cfg, sysroot, live ? GS_LIVE_PROC : NULL, &c, &disp) : NULL;
		printf("\n%s (%s)\n", devs[i].str, source);
		if (c.present) {
			printf("  id 10de:%04x subsystem %04x:%04x class 0x%06x driver %s boot_vga %d iommu_group %s\n",
			       c.device, c.subvendor, c.subdevice, c.class_code, c.driver[0] ? c.driver : "none",
			       c.boot_vga, c.iommu_group[0] ? c.iommu_group : "none");
			if (c.entry)
				entry_checks(&t, &c, &cfg);
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
				check(&t, "WARN", "%s: BAR0 is not assigned: the card is unusable this boot. Do not rescan again; " BAR_REMEDY,
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
			if (at_boot.entry && !gs_sriov_native(at_boot.entry) && ours)
				at_boot.has_sriov = 0;
			/* Firmware assigns BAR0 again at boot. */
			at_boot.bar0_assigned = -1;
			gs_plan_prepare(&at_boot, d, &boot_plan);
			if (boot_plan.verdict == GS_PLAN_READY && cfg.nrelease)
				check(&t, "WARN", "%s: bound to %s now, so prepare cannot act on it this boot; after the next (deferred) boot, run prepare by hand before anything loads nvidia",
				      c.pf.str, c.driver);
			else if (boot_plan.verdict == GS_PLAN_READY)
				check(&t, "WARN", "%s: bound to %s now, so prepare cannot act on it this boot; the boot unit prepares it before NVIDIA binds if nothing loads nvidia earlier",
				      c.pf.str, c.driver);
			else
				check(&t, "FAIL", "%s", boot_plan.reason);
		} else {
			check(&t, "FAIL", "%s", plan.reason);
		}
	}
	if (cfg.twin_id == GS_SW_ON && n && !selection_has_twin(sysroot, devs, n))
		check(&t, "FAIL", TWIN_ID_NONE "; prepare refuses it. %s", GS_TWIN_ID_ONLY_MSG);
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
		printf("  the boot display device: prepared only with a console-release grant (its Linux console then stays\n"
		       "  released for each prepared boot). If you accept that, next:\n"
		       "  `sudo geforce-sriov install --device %s --boot-display-release %s` (add --device for each other GPU)\n",
		       c->pf.str, c->pf.str);
		return;
	}
	if (!cfg_path || !addr_listed(cfg->bdfs, cfg->nbdfs, c->pf.str)) {
		printf("  not selected for boot-time preparation. Next: `geforce-sriov doctor --device %s`, then\n"
		       "  `sudo geforce-sriov install --device %s` (one --device per GPU to prepare) and reboot\n",
		       c->pf.str, c->pf.str);
		return;
	}
	if (live && module_selects(m, &c->pf)) {
		printf("  %s was loaded for it this boot, but the capability is not presented: see the output of\n"
		       "  `geforce-sriov prepare` (`journalctl -b -u %s` at boot) and `geforce-sriov doctor`\n",
		       GS_MODULE_NAME, GS_UNIT_PREPARE);
		return;
	}
	if (cfg->nrelease) {
		char consent[GS_CFG_MAX_BDFS * 40];

		consent_args(cfg, consent, sizeof(consent));
		printf("  deferred: boot_display_release in %s makes the boot unit prepare none of the selected GPUs.\n"
		       "  " CONSENT_NEXT "\n", cfg_path, consent);
		return;
	}
	printf("  expected until a prepared boot: %s selects it, and %s presents SR-IOV at boot,\n"
	       "  before NVIDIA binds the card. Next: `geforce-sriov doctor`, then reboot\n", cfg_path, GS_UNIT_PREPARE);
}

int gs_cmd_status(const struct gs_diag_opts *o)
{
	const char *sysroot = o->sysroot ? o->sysroot : GS_LIVE_SYSFS;
	int live = is_live_root(sysroot), all_ok = 1;
	unsigned int ready = 0;
	struct gs_addr devs[GS_CFG_MAX_BDFS];
	struct module_state m;
	struct gs_config cfg;
	const char *cfg_path, *source;
	char err[512];
	unsigned int n, i;

	if (diag_config(o, &cfg, &cfg_path, err, sizeof(err))) {
		fprintf(stderr, "geforce-sriov: config: %s\n", err);
		gs_config_defaults(&cfg);
		all_ok = 0;
	}
	read_module_state(sysroot, &m);
	printf("module %s: %s", GS_MODULE_NAME, m.loaded ? "loaded" : "not loaded");
	if (m.loaded)
		printf(" (ari=%s msix=%s experimental_vfs=%s twin_id=%s vfs=%s bdfs=%s; livepatch enabled=%u transition=%u)",
		       m.ari, m.msix, m.expvfs, m.twinid, m.vfs[0] ? m.vfs : "(none)", m.bdfs[0] ? m.bdfs : "(none)",
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
	n = selected_devices(o, &cfg, sysroot, devs, &source);
	if (n == 0) {
		printf("no NVIDIA display function selected or found (%s)\n"
		       "next: `geforce-sriov devices` lists the device IDs geforce-sriov acts on; select a GPU with `sudo geforce-sriov install --device BDF`\n",
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
		printf(": 10de:%04x %s driver %s\n", c.device, c.entry ? c.entry->name : "(not in device table)",
		       c.driver[0] ? c.driver : "none");
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
				printf("  presented without this boot's preparation (another kernel change?); `enable` acts only on GPUs prepared this boot\n");
				all_ok = 0;
			} else {
				ready++;
				if (c.bar0_assigned < 0 || c.vf_bars_assigned < 0)
					printf("  BAR assignment: not confirmed (resource file unreadable or incomplete)\n");
				if (live && c.numvfs == 0 && strcmp(c.driver, "nvidia") != 0)
					printf("  next: NVIDIA's vGPU host driver must bind the PF (it is bound to %s); then `sudo geforce-sriov enable`\n",
					       c.driver[0] ? c.driver : "nothing");
				else if (live && c.numvfs == 0)
					printf("  next: `sudo geforce-sriov enable` (enable also checks sriov-manage and pciutils)\n");
			}
			print_vfs(sysroot, &c);
		} else {
			printf("  SR-IOV: not presented\n");
			status_next(&c, &cfg, cfg_path, &m, live);
			all_ok = 0;
		}
	}
	if (ready < n)
		printf("\n%u of %u selected GPU(s) present SR-IOV as prepared (1..%u VFs, no BAR known to be unassigned%s); status exits 1\n"
		       "until all do (expected before the first prepared boot). Each card above names its next step\n",
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
		fprintf(stderr, "geforce-sriov: no device selected (bdfs= in %s); nothing is done without an explicit selection\n",
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
	GS_LIVE_SYSFS, GS_LIVE_PROC, GS_STATE_DIR, GS_RUN_DIR, gs_write_attr, gs_run,
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
			fprintf(stderr, "geforce-sriov: the unfinished attempt is from this boot; the guard can be cleared only after a cold power cycle.\n%s\n",
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
	printf("guard cleared. geforce-sriov sees a new boot ID but cannot tell whether that reboot was a cold power cycle. The next boot runs preparation again\n");
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
	char a[16], x[16], e[32], tw[16], v[16], b[256];
	int r;

	read_module_state(h->sys, &m);
	if (m.loaded) {
		if (strcmp(m.ari, yn(cfg->ari)) || strcmp(m.msix, yn(cfg->msix)) ||
		    strcmp(m.expvfs, yn(cfg->experimental_vfs)) || strcmp(m.twinid, yn(cfg->twin_id)) ||
		    module_vfs(&m) != gs_config_vfs(cfg) || strcmp(m.bdfs, bdfs)) {
			fprintf(stderr,
				"geforce-sriov: %s is already loaded with ari=%s msix=%s experimental_vfs=%s twin_id=%s vfs=%u bdfs=%s, not the requested ari=%s msix=%s experimental_vfs=%s twin_id=%s vfs=%u bdfs=%s; reboot to change them\n",
				GS_MODULE_NAME, m.ari, m.msix, m.expvfs, m.twinid, module_vfs(&m), m.bdfs, yn(cfg->ari),
				yn(cfg->msix), yn(cfg->experimental_vfs), yn(cfg->twin_id), gs_config_vfs(cfg), bdfs);
			return GS_EXIT_REFUSED;
		}
		printf("%s already loaded with the requested parameters\n", GS_MODULE_NAME);
	} else {
		/* vfs=N only when the config sets it; the module's default is GS_DEFAULT_VFS. */
		const char *argv[] = { "modprobe", GS_MODULE_NAME, a, x, e, tw, cfg->vfs ? v : b, cfg->vfs ? b : NULL,
				       NULL };

		snprintf(a, sizeof(a), "ari=%d", cfg->ari == GS_SW_OFF ? 0 : 1);
		snprintf(x, sizeof(x), "msix=%d", cfg->msix == GS_SW_OFF ? 0 : 1);
		snprintf(e, sizeof(e), "experimental_vfs=%d", cfg->experimental_vfs == GS_SW_OFF ? 0 : 1);
		snprintf(tw, sizeof(tw), "twin_id=%d", cfg->twin_id == GS_SW_OFF ? 0 : 1);
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
				"geforce-sriov: modprobe %s failed (%d); nothing was removed. The kernel log (`journalctl -k -b`)\n"
				"names the refused GPU and the reason\n",
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
	if (!gs_sriov_native(c.entry) && cfg->msix == GS_SW_ON)
		printf("%s: msix=on: the module applied the MSI-X unhide at load where this die has a known mechanism\n",
		       pf->str);

	/* Hold the upstream bridge awake for the whole remove/rescan/return sequence. */
	snprintf(pm_old, sizeof(pm_old), "%s", c.bridge_pm);
	gs_pathf(p, sizeof(p), "%s/power/control", c.bridge_dir);
	if ((r = h->write_attr(p, "on"))) {
		fprintf(stderr, "geforce-sriov: refused: %s: bridge %s power/control=on failed: %s\n", pf->str,
			c.bridge.str, strerror(-r));
		return CARD_REFUSED;
	}
	/* "unsupported" means runtime PM is disabled for the bridge: it is not runtime-suspended. */
	gs_pathf(p, sizeof(p), "%s/power/runtime_status", c.bridge_dir);
	if (wait_attr_eq(p, "unsupported", 0) && wait_attr_eq(p, "active", BRIDGE_WAKE_MS)) {
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
	restore_pm(h, &c, pm_old);
	/* Read-only resource check: a rescan can only resize the card's own bridge window. */
	if (c.bar0_assigned == 1 && after.bar0_assigned == 0) {
		fprintf(stderr, "geforce-sriov: %s came back without BAR0 (it was assigned before): the card is unusable this boot.\n"
			"Do not rescan again; " BAR_REMEDY ". Skip preparation with geforce_sriov.skip meanwhile\n",
			pf->str);
		return CARD_FAILED_BACK;
	}
	if (!after.has_sriov) {
		fprintf(stderr, "geforce-sriov: %s came back without sriov_totalvfs: the capability was not presented (see `geforce-sriov doctor` and the kernel log)\n",
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
	if (after.vf_bars_assigned == 0)
		printf("%s: warning: no VF BAR was assigned, so enabling VFs will fail with \"not enough MMIO resources for\n"
		       "SR-IOV\"; " BAR_REMEDY "\n", pf->str);
	if (c.entry->experimental)
		printf("%s: experimental device: SR-IOV presented; vGPU operation not verified end to end\n", pf->str);

	gs_boot_id(boot, sizeof(boot));
	r = snprintf(rec, sizeof(rec), "boot_id=%s\ntotalvfs=%u\nari_enabled=%d\n", boot, after.totalvfs,
		     after.ari_enabled);
	if (k)
		r += snprintf(rec + r, sizeof(rec) - (size_t)r, "console_release=%s\n", k->fw[0] ? k->fw : "none");
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

/* Once every selected card is prepared this boot, the boot unit's deferral is over. */
static void end_deferral(const struct gs_host *h, const struct gs_config *cfg)
{
	char p[GS_PATH_MAX];

	for (unsigned int i = 0; i < cfg->nbdfs; i++)
		if (!prepared_now(h, &cfg->bdfs[i]))
			return;
	if (gs_pathf(p, sizeof(p), "%s/" GS_DEFERRED_NAME, h->run_dir) || !gs_exists(p))
		return;
	if (unlink(p) == 0)
		printf("every selected GPU is prepared: the boot unit's deferral is over (%s removed).\n"
		       "Next: load NVIDIA's host driver, run `geforce-sriov enable`, then start nvidia-vgpud and nvidia-vgpu-mgr\n",
		       p);
	else
		fprintf(stderr, "geforce-sriov: cannot remove %s: %s; nvidia-vgpud stays blocked\n", p, strerror(errno));
}

/*
 * The boot unit with a console-release grant: change nothing on any card and record the deferral,
 * which also blocks nvidia-vgpud (its drop-in asserts the record is absent).
 */
static int defer_boot(const struct gs_host *h, const struct gs_config *cfg, const char *bdfs, const char *consent)
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
	       CONSENT_NEXT "\n", rel, bdfs, consent);
	return GS_EXIT_OK;
}

int gs_prepare(const struct gs_host *h, struct gs_config *cfg, const struct gs_prepare_opts *po)
{
	char bdfs[GS_BDF_LIST_STR_SIZE], consent[GS_CFG_MAX_BDFS * 40], p[GS_PATH_MAX];
	struct gs_addr todo[GS_CFG_MAX_BDFS];
	struct console *cons;
	unsigned int ntodo = 0, ncons = 0, i;
	int r, rc = GS_EXIT_OK;

	if (gs_pathf(p, sizeof(p), "%s/" GS_ATTEMPT_NAME, h->state_dir) || gs_exists(p)) {
		fprintf(stderr, "geforce-sriov: guard %s is set: an earlier preparation did not finish. Nothing done.\n%s\n",
			p, recovery_msg);
		return GS_EXIT_COLD_CYCLE;
	}
	gs_addr_list_str(cfg->bdfs, cfg->nbdfs, bdfs, sizeof(bdfs));
	consent_args(cfg, consent, sizeof(consent));
	/* Consent: the boot unit never gives it; by hand it must name exactly the config's grants. */
	if (cfg->nrelease && po->boot)
		return defer_boot(h, cfg, bdfs, consent);
	if (cfg->nrelease && !same_selection(po->release, po->nrelease, cfg->boot_display_release, cfg->nrelease)) {
		fprintf(stderr, "geforce-sriov: refused, nothing changed: the config grants a console release (boot_display_release),\n"
			"so prepare runs only with exactly that consent: `geforce-sriov prepare %s`\n%s\n", consent,
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
	if (cfg->twin_id == GS_SW_ON && !selection_has_twin(h->sys, cfg->bdfs, cfg->nbdfs)) {
		fprintf(stderr, "geforce-sriov: refused, nothing changed: " TWIN_ID_NONE ". %s\n", GS_TWIN_ID_ONLY_MSG);
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
		gs_card_inspect(h->sys, &cfg->bdfs[i], &c);
		d = granted_display(cfg, h->sys, h->proc, &c, &disp);
		gs_plan_prepare(&c, d, &plan);
		if (plan.verdict != GS_PLAN_READY) {
			fprintf(stderr, "geforce-sriov: refused, nothing changed: %s\n", plan.reason);
			free(cons);
			return GS_EXIT_REFUSED;
		}
		if (c.entry) {
			char why[512];
			int v = gs_vfs_check(c.entry, cfg, why, sizeof(why));

			if (v < 0) {
				fprintf(stderr, "geforce-sriov: refused, nothing changed: %s: %s\n", c.pf.str, why);
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
			free(cons);
			return GS_EXIT_REFUSED;
		}
	}
	if ((r = write_marker(h, bdfs, "load-module"))) {
		fprintf(stderr, "geforce-sriov: cannot write the guard: %s; nothing done\n", strerror(-r));
		rebind_consoles(h, cons, ncons);
		clear_marker(h);
		free(cons);
		return GS_EXIT_FAIL;
	}
	r = load_module(h, cfg, bdfs);
	if (r) {
		rebind_consoles(h, cons, ncons);
		clear_marker(h);
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
		if (res == CARD_REFUSED) {
			rc = GS_EXIT_REFUSED;
			break;
		}
		if (res == CARD_FAILED_BACK) {
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
	if (ov->experimental_vfs != GS_SW_UNSET)
		cfg.experimental_vfs = ov->experimental_vfs;
	if (ov->twin_id != GS_SW_UNSET)
		cfg.twin_id = ov->twin_id;
	return gs_prepare(&gs_host_live, &cfg, po);
}

/* ---- enable (live only) ---------------------------------------------------- */

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

static int vfs_bound(const struct gs_card *c, unsigned int want, char *bad, size_t len)
{
	char p[GS_PATH_MAX], target[GS_PATH_MAX];
	unsigned int i;

	for (i = 0; i < want; i++) {
		struct gs_addr a;
		struct gs_card vf;
		ssize_t n;
		const char *base;

		gs_pathf(p, sizeof(p), SYSDEV "/%s/virtfn%u", c->pf.str, i);
		n = readlink(p, target, sizeof(target) - 1);
		if (n <= 0)
			return snprintf(bad, len, "virtfn%u missing", i), -1;
		target[n] = '\0';
		base = strrchr(target, '/');
		if (gs_addr_parse(base ? base + 1 : target, &a))
			return snprintf(bad, len, "virtfn%u unparsable", i), -1;
		gs_card_inspect(GS_LIVE_SYSFS, &a, &vf);
		if (strcmp(vf.driver, "nvidia") != 0)
			return snprintf(bad, len, "VF %s bound to %s", a.str, vf.driver[0] ? vf.driver : "nothing"), -1;
	}
	return 0;
}

int gs_cmd_enable(void)
{
	struct gs_config cfg;
	char why[512], p[GS_PATH_MAX], boot[64] = "";
	unsigned int i;
	int r, rc = GS_EXIT_OK;

	if ((r = require_root("enable")))
		return r;
	if ((r = gs_lock()))
		return r;
	if (gs_exists(GS_ATTEMPT_MARKER)) {
		fprintf(stderr, "geforce-sriov: guard %s is set; no VF is enabled.\n%s\n", GS_ATTEMPT_MARKER,
			recovery_msg);
		return GS_EXIT_COLD_CYCLE;
	}
	if ((r = load_live_config(&cfg)))
		return r;
	if (sriov_manage_safe(cfg.sriov_manage, why, sizeof(why))) {
		fprintf(stderr, "geforce-sriov: refused: %s\n", why);
		return GS_EXIT_REFUSED;
	}
	/*
	 * sriov-manage needs lspci (address normalisation; without it it exits 1 silently) and
	 * setpci (class check), found through the PATH gs_run gives it.
	 */
	{
		static const char *const deps[] = { "lspci", "setpci" };

		for (size_t k = 0; k < sizeof(deps) / sizeof(deps[0]); k++) {
			if (gs_find_program(deps[k], p, sizeof(p))) {
				fprintf(stderr, "geforce-sriov: refused: %s needs %s (pciutils), not found in /usr/sbin:/usr/bin:/sbin:/bin\n",
					cfg.sriov_manage, deps[k]);
				return GS_EXIT_REFUSED;
			}
		}
	}
	printf("note: sriov-manage briefly stops nvidia-xid-logd, moves each GPU to pci-pf-stub to set sriov_numvfs,\n"
	       "binds it and the other functions on its bus to nvidia and runs `nvidia-smi -pm 1`; the result is\n"
	       "judged from sysfs, not from its exit status\n");
	gs_boot_id(boot, sizeof(boot));
	for (i = 0; i < cfg.nbdfs; i++) {
		const struct gs_addr *pf = &cfg.bdfs[i];
		struct gs_prepared rec;
		struct gs_card c;
		char bad[256];

		gs_card_inspect(GS_LIVE_SYSFS, pf, &c);
		gs_prepared_read(GS_RUN_DIR, pf, &rec);
		if (gs_check_enable(&c, &cfg, &rec, boot, why, sizeof(why))) {
			fprintf(stderr, "geforce-sriov: refused: %s\n", why);
			return GS_EXIT_REFUSED;
		}
		if (gs_check_presented(&c, &cfg, why, sizeof(why))) {
			fprintf(stderr, "geforce-sriov: refused: %s\n", why);
			return GS_EXIT_REFUSED;
		}
		if (c.numvfs) {
			if (c.numvfs == c.totalvfs && vfs_bound(&c, c.numvfs, bad, sizeof(bad)) == 0) {
				printf("%s: %u VFs already enabled\n", pf->str, c.numvfs);
				continue;
			}
			fprintf(stderr, "geforce-sriov: refused: %s has %u of %u VFs in an unexpected state\n", pf->str,
				c.numvfs, c.totalvfs);
			return GS_EXIT_REFUSED;
		}
		if (strcmp(c.driver, "nvidia") != 0) {
			fprintf(stderr, "geforce-sriov: refused: %s is bound to %s, not nvidia (load NVIDIA's vGPU host driver first)\n",
				pf->str, c.driver[0] ? c.driver : "nothing");
			return GS_EXIT_REFUSED;
		}
		{
			const char *argv[] = { cfg.sriov_manage, "-e", pf->str, NULL };

			printf("%s -e %s\n", cfg.sriov_manage, pf->str);
			r = gs_run(argv);
		}
		if (r != 0)
			fprintf(stderr, "geforce-sriov: %s exited with %d\n", cfg.sriov_manage, r);
		/* sriov-manage may exit 0 without doing anything; judge only by state. */
		for (unsigned int t = 0;; t += POLL_MS) {
			gs_card_inspect(GS_LIVE_SYSFS, pf, &c);
			if (c.numvfs == c.totalvfs && c.numvfs >= 1 && c.numvfs <= gs_config_vfs(&cfg) &&
			    vfs_bound(&c, c.numvfs, bad, sizeof(bad)) == 0 && strcmp(c.driver, "nvidia") == 0)
				break;
			if (t >= VF_BIND_WAIT_MS) {
				if (c.numvfs == 0)
					snprintf(bad, sizeof(bad), "no VF was enabled");
				else if (c.numvfs != c.totalvfs || c.numvfs > gs_config_vfs(&cfg))
					snprintf(bad, sizeof(bad), "VF count is not the presented 1..%u", gs_config_vfs(&cfg));
				else if (strcmp(c.driver, "nvidia") != 0)
					snprintf(bad, sizeof(bad), "PF bound to %s%s", c.driver[0] ? c.driver : "nothing",
						 strcmp(c.driver, "pci-pf-stub") == 0 ?
						 " (sriov-manage stopped mid-way; its driver_override may still name pci-pf-stub)" : "");
				else
					vfs_bound(&c, c.numvfs, bad, sizeof(bad));
				fprintf(stderr, "geforce-sriov: %s: enable did not complete: sriov_numvfs=%u of %u; %s\n",
					pf->str, c.numvfs, c.totalvfs, bad);
				rc = GS_EXIT_FAIL;
				break;
			}
			gs_sleep_ms(POLL_MS);
		}
		if (rc == GS_EXIT_OK)
			printf("%s: %u VFs enabled and bound to nvidia\n", pf->str, c.numvfs);
		else
			break;
	}
	if (rc == GS_EXIT_OK)
		for (i = 0; i < cfg.nbdfs; i++) {
			struct gs_card c;

			gs_card_inspect(GS_LIVE_SYSFS, &cfg.bdfs[i], &c);
			if (c.entry && c.entry->generation == GS_ADA)
				printf("%s (Ada): next `geforce-sriov activate`; it restarts nvidia-vgpud and nvidia-vgpu-mgr itself, in\n"
				       "  the measured order, then re-probes the GPU and powers its VFs\n",
				       cfg.bdfs[i].str);
		}
	return rc;
}

/* ---- activate and check: the Ada CPU-RM host stack ---------------------------------------- */

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

/* One selected Ada PF and its enabled VFs, as its virtfnN and their physfn links name them. */
struct ada_card {
	struct gs_addr pf;
	struct gs_addr vf[GS_VFS_MAX];
	unsigned int nvf;
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
 * The selected Ada GPUs, or those named by --device. A --device outside the selection or not an
 * Ada card, and a selected card that is missing, are refused; other selected cards are skipped.
 */
static int select_ada(const char *sys, const struct gs_config *cfg, const struct gs_activate_opts *o,
		      struct ada_card *out, unsigned int *n)
{
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
		int named = addr_listed(o->devices, o->ndevices, pf->str);
		struct gs_card c;

		if (o->ndevices && !named)
			continue;
		gs_card_inspect(sys, pf, &c);
		if (!c.present) {
			fprintf(stderr, "geforce-sriov: refused, nothing changed: %s is missing\n", pf->str);
			return GS_EXIT_REFUSED;
		}
		if (c.entry && c.entry->generation == GS_ADA) {
			out[(*n)++].pf = *pf;
			continue;
		}
		if (named) {
			fprintf(stderr, "geforce-sriov: refused, nothing changed: %s is not an Ada GPU (%s); this step is for Ada GPUs only\n",
				pf->str, c.entry ? gs_generation_name(c.entry->generation) : "not in the device table");
			return GS_EXIT_REFUSED;
		}
		printf("%s: %s, skipped: this step is for Ada GPUs only (no re-probe, type or power write for it)\n", pf->str,
		       c.entry ? gs_generation_name(c.entry->generation) : "not in the device table");
	}
	if (*n == 0) {
		fprintf(stderr, "geforce-sriov: refused, nothing changed: no selected GPU is an Ada GPU\n");
		return GS_EXIT_REFUSED;
	}
	return 0;
}

/*
 * An NVIDIA VF on the host whose PF is not selected in the config: 1 with which, 0 none, -1 when
 * the device list is unreadable. VFs of a selected GPU that activate does not act on (Ampere,
 * Blackwell) are named in a note: the daemon restart re-registers them, as their own flow does.
 */
static int foreign_vf(const char *sys, const struct gs_config *cfg, const struct ada_card *cards, unsigned int n,
		      char *which, size_t len)
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
		if (addr_listed(cfg->bdfs, cfg->nbdfs, base)) {
			printf("note: VF %s of %s (selected, not Ada): the daemon restart re-registers it, which is expected to\n"
			       "set its power/control back to auto (inferred, not measured); activate does not change it\n",
			       de->d_name, base);
			continue;
		}
		snprintf(which, len, "VF %.32s of %.32s", de->d_name, base);
		found = 1;
	}
	closedir(d);
	return found;
}

/*
 * A VF of an NVIDIA PF anywhere on the host that has a vGPU: current_vgpu_type not 0, or present
 * and unreadable. 1 with which, 0 none, -1 when the device list is unreadable.
 */
static int host_vgpu(const char *sys, char *which, size_t len)
{
	char p[GS_PATH_MAX], type[32];
	struct dirent *de;
	unsigned int vendor;
	int found = 0, r;
	DIR *d;

	if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices", sys) || !(d = opendir(p)))
		return snprintf(which, len, "%s/bus/pci/devices is unreadable", sys), -1;
	while (!found && (de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/physfn", sys, de->d_name) || !gs_exists(p))
			continue;
		if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/vendor", sys, de->d_name) || gs_read_hex(p, &vendor) ||
		    vendor != 0x10de)
			continue;
		r = read_dev_attr(sys, de->d_name, "nvidia/current_vgpu_type", type, sizeof(type));
		if (r == 0 && strcmp(type, "0") != 0) {
			snprintf(which, len, "VF %.32s has vGPU type %.31s", de->d_name, type);
			found = 1;
		} else if (r && r != -ENOENT) {
			snprintf(which, len, "VF %.32s: nvidia/current_vgpu_type is unreadable (%.64s)", de->d_name, strerror(-r));
			found = 1;
		}
	}
	closedir(d);
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
	snprintf(why, len, "%.300s has no EnableGpuFirmware line; the Ada host stack needs NVreg_EnableGpuFirmware=0", p);
	for (line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (strncmp(line, "EnableGpuFirmware:", 18) != 0)
			continue;
		for (v = line + 18; *v == ' ' || *v == '\t'; v++)
			;
		if (strcmp(v, "0") == 0)
			r = 0;
		else
			snprintf(why, len, "NVIDIA's driver is loaded with EnableGpuFirmware: %.32s; the Ada host stack needs it loaded with NVreg_EnableGpuFirmware=0 (GSP off, CPU-RM)",
				 v);
		break;
	}
	free(data);
	return r;
}

/* The plugin nvidia-vgpu-mgr loads must be the H5 copy of 580.159.01's: 0, or -1 with why. */
static int check_plugin(const char *path, char *why, size_t len)
{
	char sha[65], real[PATH_MAX];

	if (gs_sha256_file(path, sha))
		return snprintf(why, len, "cannot read %.300s, the vGPU plugin NVIDIA's host package installs", path), -1;
	if (!realpath(path, real))
		gs_copy(real, sizeof(real), path);
	if (strcmp(sha, ADA_PLUGIN_H5_SHA) == 0) {
		printf("vGPU plugin %s -> %s: the H5 copy\n", path, real);
		return 0;
	}
	if (strcmp(sha, ADA_PLUGIN_STOCK_SHA) == 0)
		return snprintf(why, len, "%.300s is NVIDIA's unchanged 580.159.01 plugin (%.300s); Ada needs its H5 copy (docs/ada.md)",
				path, real), -1;
	return snprintf(why, len, "%.300s (%.300s) has SHA-256 %s: neither NVIDIA's 580.159.01 plugin nor its H5 copy", path, real,
			sha), -1;
}

/* 1 when the VF's nvidia/creatable_vgpu_types lists type (lines like "911 : NVIDIA L4-2Q"). */
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
 * The measured PF re-probe (driver unbind, then bind; never remove or rescan), behind the guard
 * and NVIDIA's unbindLock, which the driver grants only while no client holds the GPU (the count
 * an unbind would otherwise wait on). 0; GS_EXIT_REFUSED: lock not granted, nothing unbound;
 * GS_EXIT_FAIL: still bound, nothing else changed; GS_EXIT_COLD_CYCLE: not back on nvidia, guard set.
 */
static int reprobe_pf(const struct gs_host *h, const struct gs_addr *pf)
{
	char lock[GS_PATH_MAX], unbind_path[GS_PATH_MAX], bind_path[GS_PATH_MAX], drv[GS_PATH_MAX], val[16] = "", now[64];
	int r;

	if (gs_pathf(lock, sizeof(lock), "%s/driver/nvidia/gpus/%s/unbindLock", h->proc, pf->str) ||
	    gs_pathf(unbind_path, sizeof(unbind_path), "%s/bus/pci/drivers/nvidia/unbind", h->sys) ||
	    gs_pathf(bind_path, sizeof(bind_path), "%s/bus/pci/drivers/nvidia/bind", h->sys) ||
	    gs_pathf(drv, sizeof(drv), "%s/bus/pci/devices/%s/driver", h->sys, pf->str))
		return GS_EXIT_FAIL;
	if ((r = write_marker(h, pf->str, "reprobe"))) {
		fprintf(stderr, "geforce-sriov: cannot write the guard: %s; %s was not re-probed\n", strerror(-r), pf->str);
		return GS_EXIT_FAIL;
	}
	r = h->write_attr(lock, "1");
	if (r || gs_read_attr(lock, val, sizeof(val)) || strcmp(val, "1") != 0) {
		h->write_attr(lock, "0");
		clear_marker(h);
		fprintf(stderr, "geforce-sriov: %s: NVIDIA's driver did not grant unbindLock (it reads '%s'%s%s): a client still\n"
			"holds the GPU, for example nvidia-smi, nvidia-persistenced or a CUDA program. It was not unbound\n",
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

		/* clear_marker also fails when the unlink worked and only the directory sync did not;
		 * only ENOENT proves the guard is gone, any other lstat error leaves it in place. */
		if (gs_pathf(g, sizeof(g), "%s/" GS_ATTEMPT_NAME, h->state_dir) == 0 && lstat(g, &st) < 0 &&
		    errno == ENOENT)
			fprintf(stderr, "geforce-sriov: %s is bound to nvidia again and the guard was removed, but syncing its\n"
				"directory failed: %s\n", pf->str, strerror(-r));
		else
			fprintf(stderr, "geforce-sriov: %s is bound to nvidia again, but removing the guard failed: %s. The guard stays\n"
				"set.\n%s\n", pf->str, strerror(-r), recovery_msg);
		return GS_EXIT_FAIL;
	}
	printf("%s: re-probed, bound to nvidia again\n", pf->str);
	return 0;
}

/* --type on one VF: current_vgpu_type must be 0 or type, and type listed as creatable. */
static int set_type_vf(const struct gs_host *h, const struct gs_addr *vf, unsigned int type)
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
		if (t >= VF_TYPES_WAIT_MS) {
			fprintf(stderr, "geforce-sriov: %s: type %s is not in nvidia/creatable_vgpu_types (`cat` it for the types this\n"
				"GPU offers); nothing written\n", vf->str, want);
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

/* After the last re-probe: the same VFs again, registered by the vGPU stack, then type and power. */
static int finish_card(const struct gs_host *h, const struct ada_card *k, const struct gs_activate_opts *o)
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
			if (t >= VF_TYPES_WAIT_MS) {
				fprintf(stderr, "geforce-sriov: %s: no nvidia/creatable_vgpu_types %u ms after the re-probe (bound to %s);\n"
					"nvidia-vgpu-mgr publishes it (`journalctl -u nvidia-vgpu-mgr -b`). Do not start a VM on it\n",
					vf->str, VF_TYPES_WAIT_MS, drv[0] ? drv : "nothing");
				return GS_EXIT_FAIL;
			}
			gs_sleep_ms(POLL_MS);
		}
		if (o->type && set_type_vf(h, vf, o->type))
			return GS_EXIT_FAIL;
		if (power_on_vf(h, vf))
			return GS_EXIT_FAIL;
	}
	return 0;
}

int gs_activate(const struct gs_host *h, const struct gs_config *cfg, const struct gs_activate_opts *o)
{
	static const char *const vgpud[] = { "systemctl", "restart", "nvidia-vgpud.service", NULL };
	static const char *const mgr[] = { "systemctl", "restart", "nvidia-vgpu-mgr.service", NULL };
	static const char *const mgr_active[] = { "systemctl", "is-active", "--quiet", "nvidia-vgpu-mgr.service", NULL };
	char why[1024], which[256], p[GS_PATH_MAX], g[GS_PATH_MAX], boot[64] = "", ver[32];
	struct ada_card *cards;
	unsigned int n = 0, i, j;
	int r, rc = GS_EXIT_OK;

	if (gs_pathf(p, sizeof(p), "%s/" GS_ATTEMPT_NAME, h->state_dir) || gs_exists(p)) {
		fprintf(stderr, "geforce-sriov: guard %s is set; nothing done.\n%s\n", p, recovery_msg);
		return GS_EXIT_COLD_CYCLE;
	}
	cards = calloc(GS_CFG_MAX_BDFS, sizeof(*cards));
	if (!cards)
		return GS_EXIT_FAIL;
	if ((r = select_ada(h->sys, cfg, o, cards, &n))) {
		free(cards);
		return r;
	}
	/* The daemon restart re-registers the VFs of every GPU, so every selected Ada GPU must be named. */
	if (o->ndevices) {
		struct ada_card *all = calloc(GS_CFG_MAX_BDFS, sizeof(*all));
		struct gs_activate_opts every = *o;
		unsigned int nall = 0;

		every.ndevices = 0;
		if (!all) {
			free(cards);
			return GS_EXIT_FAIL;
		}
		r = select_ada(h->sys, cfg, &every, all, &nall);
		free(all);
		if (r || nall != n) {
			snprintf(why, sizeof(why), "the restart of NVIDIA's vGPU daemons re-registers the VFs of every GPU; name every\n"
				 "selected Ada GPU with --device, or none");
			goto refuse;
		}
	}

	/* Preflight: every check passes before anything changes. */
	nvidia_version(h->proc, ver, sizeof(ver));
	if (strcmp(ver, ADA_DRIVER_VERSION) != 0) {
		snprintf(why, sizeof(why), "NVIDIA's driver %s%s; the Ada host stack is measured only with %s (proprietary, CPU-RM)",
			 ver[0] ? ver : "is not loaded", ver[0] ? " is loaded" : "", ADA_DRIVER_VERSION);
		goto refuse;
	}
	if (check_gsp_off(h->proc, why, sizeof(why)))
		goto refuse;
	/* The measured bring-up loaded it after the VF existed and before the daemons started. */
	if (gs_pathf(p, sizeof(p), "%s/module/nvidia_vgpu_vfio", h->sys) || !gs_exists(p)) {
		snprintf(why, sizeof(why), "nvidia_vgpu_vfio is not loaded: `modprobe nvidia_vgpu_vfio` first (NVIDIA's vGPU host package)");
		goto refuse;
	}
	if (o->plugin && check_plugin(o->plugin, why, sizeof(why)))
		goto refuse;
	gs_boot_id(boot, sizeof(boot));
	for (i = 0; i < n; i++) {
		struct ada_card *k = &cards[i];
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
			snprintf(why, sizeof(why), "%s has driver_override=%s: the re-probe's bind to nvidia would fail; `echo > /sys/bus/pci/devices/%s/driver_override`\n"
				 "first", k->pf.str, drv, k->pf.str);
			goto refuse;
		}
		if (c.numvfs < 1 || c.numvfs != c.totalvfs) {
			snprintf(why, sizeof(why), "%s has %u of %u VFs enabled: run `geforce-sriov enable` first", k->pf.str, c.numvfs,
				 c.totalvfs);
			goto refuse;
		}
		if (vf_list(h->sys, &k->pf, c.numvfs, k->vf, why, sizeof(why)))
			goto refuse;
		k->nvf = c.numvfs;
		if (o->type && k->nvf > 1) {
			snprintf(why, sizeof(why), "%s has %u VFs; --type is refused there because only one VF of an Ada card was\n"
				 "measured to start a vGPU. Run activate without --type and set each VF's type yourself", k->pf.str, k->nvf);
			goto refuse;
		}
		for (j = 0; j < k->nvf; j++) {
			dev_driver(h->sys, k->vf[j].str, drv, sizeof(drv));
			if (strcmp(drv, "nvidia") != 0) {
				snprintf(why, sizeof(why), "VF %s of %s is bound to %s, not nvidia", k->vf[j].str, k->pf.str,
					 drv[0] ? drv : "nothing");
				goto refuse;
			}
			if (gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/power/control", h->sys, k->vf[j].str) || !gs_exists(p)) {
				snprintf(why, sizeof(why), "VF %s has no power/control", k->vf[j].str);
				goto refuse;
			}
		}
		if (gs_pathf(p, sizeof(p), "%s/driver/nvidia/gpus/%s/unbindLock", h->proc, k->pf.str) || !gs_exists(p)) {
			snprintf(why, sizeof(why), "%.300s is missing: the re-probe needs NVIDIA's unbindLock for %s", p, k->pf.str);
			goto refuse;
		}
	}
	/* The daemons serve every GPU on the host: a restart would end any vGPU there. */
	if ((r = host_vgpu(h->sys, which, sizeof(which)))) {
		if (r < 0)
			snprintf(why, sizeof(why), "%s", which);
		else
			snprintf(why, sizeof(why), "%s: restarting NVIDIA's vGPU daemons would end it. Stop its VM and write 0 to its\n"
				 "nvidia/current_vgpu_type first", which);
		goto refuse;
	}
	/* The restart also re-registers the VFs of GPUs outside the selection: refused. */
	if ((r = foreign_vf(h->sys, cfg, cards, n, which, sizeof(which)))) {
		if (r < 0)
			snprintf(why, sizeof(why), "%s", which);
		else
			snprintf(why, sizeof(why), "%s: that GPU is not selected, and the restart of NVIDIA's vGPU daemons would\n"
				 "re-register it too. Disable its VFs first", which);
		goto refuse;
	}

	if (o->dry_run) {
		printf("dry run: every check passed; activate would run, in this order:\n"
		       "  systemctl restart nvidia-vgpud.service\n"
		       "  systemctl restart nvidia-vgpu-mgr.service, then wait until it is active\n");
		for (i = 0; i < n; i++)
			printf("  %s: unbindLock, then echo %s > /sys/bus/pci/drivers/nvidia/unbind and .../nvidia/bind\n",
			       cards[i].pf.str, cards[i].pf.str);
		for (i = 0; i < n; i++)
			for (j = 0; j < cards[i].nvf; j++) {
				if (o->type)
					printf("  %s: echo %u > nvidia/current_vgpu_type\n", cards[i].vf[j].str, o->type);
				printf("  %s: echo on > power/control, then read back on and active\n", cards[i].vf[j].str);
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
	/* The measured order: the VFs exist, then both daemons start, then the PF re-probe. */
	printf("systemctl restart nvidia-vgpud.service\n");
	if ((r = h->run(vgpud))) {
		fprintf(stderr, "geforce-sriov: systemctl restart nvidia-vgpud.service failed (%d); no GPU was re-probed\n"
			"(`journalctl -u nvidia-vgpud -b` says why)\n", r);
		rc = GS_EXIT_FAIL;
		goto stopped;
	}
	/* A VM may have started since the preflight: nvidia-vgpud restarted, nothing else changed yet. */
	if ((r = host_vgpu(h->sys, which, sizeof(which)))) {
		fprintf(stderr, "geforce-sriov: %s since the checks; nvidia-vgpud was restarted, nvidia-vgpu-mgr was not and no GPU\n"
			"was re-probed\n", which);
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
			finish_card(h, &cards[j], o);
		rc = GS_EXIT_FAIL;
		goto stopped;
	}
	/* Every re-probe has run: the VFs' power/control is written last, after their final registration. */
	for (i = 0; i < n; i++)
		if (finish_card(h, &cards[i], o))
			rc = GS_EXIT_FAIL;
	if (rc == GS_EXIT_OK && unlink(p) < 0 && errno != ENOENT) {
		fprintf(stderr, "geforce-sriov: cannot remove %s: %s; check stays NOT READY\n", p, strerror(errno));
		rc = GS_EXIT_FAIL;
	}
	if (rc == GS_EXIT_OK) {
		char arg[32] = "";

		if (o->type)
			snprintf(arg, sizeof(arg), " --type %u", o->type);
		printf("ready. Run `geforce-sriov check%s` before every VM start: a later PF re-probe or restart of\n"
		       "nvidia-vgpu-mgr is expected to set power/control back to auto (inferred, not measured; stop the\n"
		       "VMs, then run activate again)\n",
		       arg);
		free(cards);
		return rc;
	}
stopped:
	/* The guard is set (exit 5, or its removal failed): the next activate refuses until it is cleared. */
	if (rc == GS_EXIT_COLD_CYCLE ||
	    (gs_pathf(g, sizeof(g), "%s/" GS_ATTEMPT_NAME, h->state_dir) == 0 && gs_exists(g)))
		fprintf(stderr, "geforce-sriov: %s and the guard stay, so `geforce-sriov check` reports NOT READY and\n"
			"`geforce-sriov activate` refuses (exit 5) until a cold power cycle and `geforce-sriov prepare --clear-guard`\n",
			p);
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
	/* Where NVIDIA's host package puts the plugin link; a missing one is refused by its check. */
	static const char *const plugins[] = { "/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so", "/usr/lib64/libnvidia-vgpu.so",
					       "/usr/lib/libnvidia-vgpu.so" };
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
	o.plugin = plugins[0];
	for (size_t k = 0; k < sizeof(plugins) / sizeof(plugins[0]); k++) {
		if (gs_exists(plugins[k])) {
			o.plugin = plugins[k];
			break;
		}
	}
	o.settle_ms = MGR_SETTLE_MS;
	return gs_activate(&gs_host_live, &cfg, &o);
}

int gs_check_vfs(const struct gs_host *h, const struct gs_config *cfg, const struct gs_activate_opts *o)
{
	const char *sys = h->sys;
	struct ada_card *cards;
	unsigned int n = 0, i, j;
	char want[16] = "";
	int r, ready = 1;

	cards = calloc(GS_CFG_MAX_BDFS, sizeof(*cards));
	if (!cards)
		return GS_EXIT_FAIL;
	if ((r = select_ada(sys, cfg, o, cards, &n))) {
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
	if (o->type)
		snprintf(want, sizeof(want), "%u", o->type);
	for (i = 0; i < n; i++) {
		struct ada_card *k = &cards[i];
		char why[512];
		struct gs_card c;

		gs_card_inspect(sys, &k->pf, &c);
		if (strcmp(c.driver, "nvidia") != 0) {
			printf("NOT READY %s: bound to %s, not nvidia\n", k->pf.str, c.driver[0] ? c.driver : "nothing");
			ready = 0;
			continue;
		}
		if (c.numvfs < 1) {
			printf("NOT READY %s: no VF is enabled (`geforce-sriov enable`, then `geforce-sriov activate`)\n", k->pf.str);
			ready = 0;
			continue;
		}
		if (vf_list(sys, &k->pf, c.numvfs, k->vf, why, sizeof(why))) {
			printf("NOT READY %s\n", why);
			ready = 0;
			continue;
		}
		for (j = 0; j < c.numvfs; j++) {
			char drv[64], ctl[32], st[32], type[32];
			int ok;

			/* experimental_vfs=on: the measurement got a vGPU on virtfn0 only (the others: NV_ERR_OUT_OF_RANGE). */
			if (j > 0) {
				printf("NOT READY %s (VF of %s): virtfn%u; only the first VF of an Ada card was measured to reach a vGPU,\n"
				       "  the others failed with NV_ERR_OUT_OF_RANGE; start no VM on it\n", k->vf[j].str, k->pf.str, j);
				ready = 0;
				continue;
			}
			dev_driver(sys, k->vf[j].str, drv, sizeof(drv));
			read_dev_attr(sys, k->vf[j].str, "power/control", ctl, sizeof(ctl));
			read_dev_attr(sys, k->vf[j].str, "power/runtime_status", st, sizeof(st));
			ok = read_dev_attr(sys, k->vf[j].str, "nvidia/current_vgpu_type", type, sizeof(type)) == 0 &&
			     strcmp(drv, "nvidia") == 0 && strcmp(ctl, "on") == 0 && strcmp(st, "active") == 0 &&
			     (!o->type || strcmp(type, want) == 0);
			printf("%s %s (VF of %s): driver=%s type=%s%s%s power/control=%s runtime_status=%s\n", ok ? "OK" : "NOT READY",
			       k->vf[j].str, k->pf.str, drv[0] ? drv : "none", type[0] ? type : "unreadable", o->type ? " expected=" : "",
			       want, ctl[0] ? ctl : "unreadable", st[0] ? st : "unreadable");
			if (!ok)
				ready = 0;
			else if (!o->type && strcmp(type, "0") == 0)
				printf("  note: %s has no vGPU type yet; whatever starts the VM must set one\n", k->vf[j].str);
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
