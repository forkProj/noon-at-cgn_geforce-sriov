// SPDX-License-Identifier: MIT
/*
 * install and uninstall.
 *
 * install copies the built tree into DESTDIR+prefix (staging: nothing else),
 * or into the live system, where it also registers the module source with
 * DKMS and enables the boot unit when a device is selected. It never starts the
 * unit or touches a device. Every file it writes is recorded with its SHA-256
 * in GS_MANIFEST; uninstall removes only recorded files whose hash still
 * matches. NVIDIA's own driver is never installed, rebuilt or modified here.
 */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gs_lifecycle.h"

#define DKMS_SRC "/usr/src/" GS_DKMS_NAME "-" GS_VERSION
#define DKMS_ID GS_DKMS_NAME "/" GS_VERSION
#define UNIT_DIR GS_UNIT_DIR
#define VGPUD_DROPIN_DIR UNIT_DIR "/nvidia-vgpud.service.d"
#define MAX_ENTRIES 128

struct entry {
	char type;            /* 'f' file, 'd' directory created by install */
	char sha[65];
	char path[GS_PATH_MAX];
};

struct manifest {
	struct entry e[MAX_ENTRIES];
	unsigned int n;
};

static struct entry *manifest_find(struct manifest *m, char type, const char *path)
{
	for (unsigned int i = 0; i < m->n; i++)
		if (m->e[i].type == type && strcmp(m->e[i].path, path) == 0)
			return &m->e[i];
	return NULL;
}

static int manifest_add(struct manifest *m, char type, const char *sha, const char *path)
{
	struct entry *e = manifest_find(m, type, path);

	if (!e) {
		if (m->n == MAX_ENTRIES)
			return -ENOSPC;
		e = &m->e[m->n++];
	}
	e->type = type;
	snprintf(e->sha, sizeof(e->sha), "%s", sha);
	snprintf(e->path, sizeof(e->path), "%s", path);
	return 0;
}

/* Lines "f <sha256> <path>" and "d - <path>". A missing file is an empty manifest. */
static int manifest_load(const char *file, struct manifest *m)
{
	char *data, *line, *save = NULL;
	size_t len;
	int r = gs_read_whole(file, &data, &len, 1 << 20);

	m->n = 0;
	if (r == -ENOENT)
		return 0;
	if (r)
		return r;
	for (line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char type, sha[65], path[GS_PATH_MAX];

		if (sscanf(line, "%c %64s %4095s", &type, sha, path) != 3 || (type != 'f' && type != 'd') ||
		    gs_validate_abs_path(path) || manifest_add(m, type, sha, path)) {
			free(data);
			return -EINVAL;
		}
	}
	free(data);
	return 0;
}

static int manifest_save(const char *file, const struct manifest *m)
{
	size_t cap = (size_t)m->n * (GS_PATH_MAX + 80) + 1, off = 0;
	char *buf = malloc(cap);
	int r;

	if (!buf)
		return -ENOMEM;
	for (unsigned int i = 0; i < m->n; i++) {
		int n = snprintf(buf + off, cap - off, "%c %s %s\n", m->e[i].type, m->e[i].sha, m->e[i].path);

		if (n < 0 || (size_t)n >= cap - off) {
			free(buf);
			return -EOVERFLOW;
		}
		off += (size_t)n;
	}
	r = gs_write_file_atomic(file, buf, off, 0644);
	free(buf);
	return r;
}

/* ---- install ------------------------------------------------------------ */

/* Only characters that systemd passes through literally in ExecStart=/Environment=. */
static int unit_safe(const char *s)
{
	for (; *s; s++) {
		char c = *s;

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		      c == '/' || c == '.' || c == '_' || c == '+' || c == '-'))
			return 0;
	}
	return 1;
}

/* 1 and the offending prefix in out when an existing component of path is a symlink. */
static int symlink_component(const char *path, char *out, size_t len)
{
	char buf[GS_PATH_MAX];
	struct stat st;
	size_t i, n;

	if (gs_pathf(buf, sizeof(buf), "%s", path))
		return 0;
	n = strlen(buf);
	for (i = 1; i <= n; i++) {
		if (buf[i] != '/' && buf[i] != '\0')
			continue;
		buf[i] = '\0';
		if (lstat(buf, &st) == 0 && S_ISLNK(st.st_mode)) {
			gs_copy(out, len, buf);
			return 1;
		}
		if (i < n)
			buf[i] = '/';
	}
	return 0;
}

struct ctx {
	const char *destdir; /* "" for live */
	struct manifest m;
};

/* mkdir -p below destdir, recording every directory this run creates. */
static int make_dirs(struct ctx *x, const char *target_dir)
{
	char buf[GS_PATH_MAX], full[GS_PATH_MAX];
	size_t i, len;

	if (gs_pathf(buf, sizeof(buf), "%s", target_dir))
		return -ENAMETOOLONG;
	len = strlen(buf);
	for (i = 1; i <= len; i++) {
		if (buf[i] != '/' && buf[i] != '\0')
			continue;
		buf[i] = '\0';
		if (gs_pathf(full, sizeof(full), "%s%s", x->destdir, buf))
			return -ENAMETOOLONG;
		if (mkdir(full, 0755) == 0) {
			if (manifest_add(&x->m, 'd', "-", buf))
				return -ENOSPC;
		} else if (errno != EEXIST) {
			return -errno;
		}
		/* Staging never follows a link out of DESTDIR (e.g. etc -> /etc). */
		if (x->destdir[0]) {
			struct stat st;

			if (lstat(full, &st) < 0)
				return -errno;
			if (!S_ISDIR(st.st_mode)) {
				fprintf(stderr, "geforce-sriov: %s is not a directory (symbolic links are not followed when staging)\n",
					full);
				return -ELOOP;
			}
		}
		if (i < len)
			buf[i] = '/';
	}
	return 0;
}

static int put(struct ctx *x, const char *target, const void *data, size_t len, mode_t mode)
{
	char full[GS_PATH_MAX], dir[GS_PATH_MAX], sha[65];
	char *slash;
	int r;

	if (gs_pathf(dir, sizeof(dir), "%s", target))
		return -ENAMETOOLONG;
	slash = strrchr(dir, '/');
	if (slash && slash != dir) {
		*slash = '\0';
		if ((r = make_dirs(x, dir)))
			return r;
	}
	if (gs_pathf(full, sizeof(full), "%s%s", x->destdir, target))
		return -ENAMETOOLONG;
	gs_sha256_buf(data, len, sha);
	if (gs_exists(full)) {
		struct stat st;
		char have[65];
		int same = gs_sha256_file(full, have) == 0 && strcmp(have, sha) == 0;

		/* An existing file that a previous install did not write is not taken over. */
		if (!same && !manifest_find(&x->m, 'f', target)) {
			fprintf(stderr, "geforce-sriov: %s exists and was not installed by geforce-sriov; move it away first\n",
				full);
			return -EEXIST;
		}
		/* Already in place with the same content, mode and owner: leave it alone. */
		if (same && lstat(full, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 07777) == mode &&
		    st.st_uid == geteuid()) {
			printf("unchanged %s%s\n", x->destdir, target);
			return manifest_add(&x->m, 'f', sha, target);
		}
	}
	if ((r = gs_write_file_atomic(full, data, len, mode)))
		return r;
	printf("installed %s%s\n", x->destdir, target);
	return manifest_add(&x->m, 'f', sha, target);
}

static int copy_file(struct ctx *x, const char *src, const char *target, mode_t mode)
{
	char *data;
	size_t len;
	int r = gs_read_whole(src, &data, &len, 256u << 20);

	if (r)
		return r;
	r = put(x, target, data, len, mode);
	free(data);
	return r;
}

/* Copy a template, replacing every @NAME@ from the table. */
static int put_template(struct ctx *x, const char *src, const char *target, const char *const *subst)
{
	char *data, *out;
	size_t len, cap, off = 0;
	int r = gs_read_whole(src, &data, &len, 1 << 20);

	if (r)
		return r;
	cap = len + 8 * GS_PATH_MAX;
	out = malloc(cap);
	if (!out) {
		free(data);
		return -ENOMEM;
	}
	for (size_t i = 0; i < len;) {
		size_t k;

		for (k = 0; subst[k]; k += 2) {
			size_t n = strlen(subst[k]);

			if (len - i >= n && memcmp(data + i, subst[k], n) == 0)
				break;
		}
		if (subst[k]) {
			size_t vn = strlen(subst[k + 1]);

			if (off + vn >= cap)
				goto overflow;
			memcpy(out + off, subst[k + 1], vn);
			off += vn;
			i += strlen(subst[k]);
		} else {
			if (off + 1 >= cap)
				goto overflow;
			out[off++] = data[i++];
		}
	}
	free(data);
	/* An unreplaced @NAME@ placeholder would ship a broken unit. */
	for (size_t i = 0; i + 1 < off; i++) {
		if (out[i] == '@' && out[i + 1] >= 'A' && out[i + 1] <= 'Z') {
			free(out);
			return -EINVAL;
		}
	}
	r = put(x, target, out, off, 0644);
	free(out);
	return r;
overflow:
	free(data);
	free(out);
	return -EOVERFLOW;
}

static int self_dir(char *out, size_t len)
{
	char exe[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	char *slash;

	if (n <= 0)
		return -errno;
	exe[n] = '\0';
	slash = strrchr(exe, '/');
	if (!slash)
		return -EINVAL;
	*slash = '\0';
	return gs_pathf(out, len, "%s", exe[0] ? exe : "/");
}

static const char *const kernel_required[] = { "geforce_sriov.c", "Kbuild", "Makefile", "dkms.conf", NULL };

static int is_kernel_source(const char *name)
{
	size_t n = strlen(name);

	if (name[0] == '.')
		return 0;
	if (strcmp(name, "Kbuild") == 0 || strcmp(name, "Makefile") == 0 || strcmp(name, "dkms.conf") == 0)
		return 1;
	if (strstr(name, ".mod."))
		return 0;
	return n > 2 && name[n - 2] == '.' && (name[n - 1] == 'c' || name[n - 1] == 'h');
}

static void print_live_steps(const char *bindir, int enable_unit)
{
	if (enable_unit)
		printf("  %s/geforce-sriov doctor   (checks the selected GPUs on this host; staging did not)\n", bindir);
	printf("  dkms add -m %s -v %s && dkms build -m %s -v %s && dkms install -m %s -v %s\n", GS_DKMS_NAME,
	       GS_VERSION, GS_DKMS_NAME, GS_VERSION, GS_DKMS_NAME, GS_VERSION);
	printf("  systemctl daemon-reload\n");
	if (enable_unit)
		printf("  systemctl enable %s   (runs at the next boot)\n", GS_UNIT_PREPARE);
	else
		printf("  select the GPUs with `%s/geforce-sriov install --device BDF` (one per GPU; this sets bdfs=,\n"
		       "  identity=on and identity_force_host=on and enables %s), or set those keys in %s,\n"
		       "  then `systemctl enable %s`\n",
		       bindir, GS_UNIT_PREPARE, GS_CONFIG_PATH, GS_UNIT_PREPARE);
}

int gs_cmd_install(const struct gs_install_opts *o)
{
	const char *prefix = o->prefix ? o->prefix : GS_DEFAULT_PREFIX;
	const char *destdir = o->destdir ? o->destdir : "";
	int live = destdir[0] == '\0';
	char src[GS_PATH_MAX], p[GS_PATH_MAX], q[GS_PATH_MAX], bindir[GS_PATH_MAX], libdir[GS_PATH_MAX];
	char hook[GS_PATH_MAX], sharedir[GS_PATH_MAX], cfgfile[GS_PATH_MAX], manifest[GS_PATH_MAX];
	char err[512], prog[GS_PATH_MAX];
	struct gs_config cfg;
	int config_changed = 0, cfg_exists, r;
	struct ctx *x;
	DIR *d;
	struct dirent *de;
	static const char *const artifacts[] = {
		"geforce-sriov", "driver/geforce-sriov-patch", "hook/libgeforce_sriov.so",
		"include/gs_devices.h", "systemd/geforce-sriov-prepare.service.in",
		"systemd/nvidia-vgpud.service.d/geforce-sriov.conf.in", "config/config.example", NULL,
	};

	if (gs_validate_abs_path(prefix) || !unit_safe(prefix)) {
		fprintf(stderr, "geforce-sriov: --prefix must be a normalized absolute path of letters, digits and / . _ + - only (it is written into systemd units)\n");
		return GS_EXIT_USAGE;
	}
	if (!live && (gs_validate_abs_path(destdir) || strcmp(destdir, "/") == 0)) {
		fprintf(stderr, "geforce-sriov: DESTDIR must be a normalized absolute path other than / (omit it for a live install)\n");
		return GS_EXIT_USAGE;
	}
	if (!live && symlink_component(destdir, p, sizeof(p))) {
		fprintf(stderr, "geforce-sriov: DESTDIR component %s is a symbolic link; staging does not follow links\n", p);
		return GS_EXIT_REFUSED;
	}
	if (o->source)
		r = gs_pathf(src, sizeof(src), "%s", o->source);
	else
		r = self_dir(src, sizeof(src));
	if (r) {
		fprintf(stderr, "geforce-sriov: cannot locate the built tree\n");
		return GS_EXIT_FAIL;
	}
	for (size_t i = 0; artifacts[i]; i++) {
		if (gs_pathf(p, sizeof(p), "%s/%s", src, artifacts[i]) || !gs_exists(p)) {
			fprintf(stderr, "geforce-sriov: %s/%s is missing; build first (make)\n", src, artifacts[i]);
			return GS_EXIT_REFUSED;
		}
	}
	for (size_t i = 0; kernel_required[i]; i++) {
		if (gs_pathf(p, sizeof(p), "%s/kernel/%s", src, kernel_required[i]) || !gs_exists(p)) {
			fprintf(stderr, "geforce-sriov: %s is missing from the source tree\n", p);
			return GS_EXIT_REFUSED;
		}
	}
	if (live) {
		if (geteuid() != 0) {
			fprintf(stderr, "geforce-sriov: a live install must run as root (set DESTDIR to stage)\n");
			return GS_EXIT_REFUSED;
		}
		if ((r = gs_lock()))
			return r;
		if (gs_find_program("dkms", prog, sizeof(prog)) || gs_find_program("systemctl", prog, sizeof(prog))) {
			fprintf(stderr, "geforce-sriov: a live install needs dkms and systemctl\n");
			return GS_EXIT_REFUSED;
		}
		if (gs_exists(GS_ATTEMPT_MARKER))
			printf("warning: guard %s is set from an unfinished preparation; the boot unit refuses to run until a cold power cycle and `geforce-sriov prepare --clear-guard`\n",
			       GS_ATTEMPT_MARKER);
	}

	if (!live && (r = gs_mkdir_p(destdir, 0755))) {
		fprintf(stderr, "geforce-sriov: cannot create DESTDIR %s: %s\n", destdir, strerror(-r));
		return GS_EXIT_FAIL;
	}

	gs_pathf(bindir, sizeof(bindir), "%s/bin", strcmp(prefix, "/") ? prefix : "");
	gs_pathf(libdir, sizeof(libdir), "%s/lib/geforce-sriov", strcmp(prefix, "/") ? prefix : "");
	gs_pathf(sharedir, sizeof(sharedir), "%s/share/geforce-sriov", strcmp(prefix, "/") ? prefix : "");
	gs_pathf(hook, sizeof(hook), "%s/libgeforce_sriov.so", libdir);
	gs_pathf(cfgfile, sizeof(cfgfile), "%s%s", destdir, GS_CONFIG_PATH);
	gs_pathf(manifest, sizeof(manifest), "%s%s", destdir, GS_MANIFEST);

	x = calloc(1, sizeof(*x));
	if (!x)
		return GS_EXIT_FAIL;
	x->destdir = destdir;
	if ((r = manifest_load(manifest, &x->m))) {
		fprintf(stderr, "geforce-sriov: %s is unreadable or corrupt (%s)\n", manifest, strerror(-r));
		free(x);
		return GS_EXIT_FAIL;
	}

	/* Configuration: keep the existing file unless the command line changes it. */
	cfg_exists = gs_exists(cfgfile);
	if (gs_config_load(cfgfile, &cfg, err, sizeof(err))) {
		fprintf(stderr, "geforce-sriov: existing config is invalid, fix or remove it: %s\n", err);
		free(x);
		return GS_EXIT_REFUSED;
	}
	if (o->ndevices) {
		for (unsigned int i = 0; i < o->ndevices; i++)
			cfg.bdfs[i] = o->devices[i];
		cfg.nbdfs = o->ndevices;
		/* --device replaces the selection, and with it every console-release grant not given again. */
		for (unsigned int i = 0; i < o->nrelease; i++)
			cfg.boot_display_release[i] = o->release[i];
		cfg.nrelease = o->nrelease;
		/*
		 * A user-selected card gets the identity configuration the method was demonstrated
		 * with: the PCI info rewritten and the virtualization mode forced to HOST.
		 */
		cfg.identity = GS_SW_ON;
		cfg.identity_force_host = GS_SW_ON;
		config_changed = 1;
	}
	if (o->ov.ari != GS_SW_UNSET && o->ov.ari != cfg.ari)
		cfg.ari = o->ov.ari, config_changed = 1;
	if (o->ov.msix != GS_SW_UNSET && o->ov.msix != cfg.msix)
		cfg.msix = o->ov.msix, config_changed = 1;
	if (o->ov.experimental_vfs != GS_SW_UNSET && o->ov.experimental_vfs != cfg.experimental_vfs)
		cfg.experimental_vfs = o->ov.experimental_vfs, config_changed = 1;
	if (o->vfs && o->vfs != cfg.vfs)
		cfg.vfs = o->vfs, config_changed = 1;
	if (!cfg_exists)
		config_changed = 1;
	/* Never take over a config this tool did not write. */
	if (cfg_exists && config_changed && !manifest_find(&x->m, 'f', GS_CONFIG_PATH)) {
		fprintf(stderr, "geforce-sriov: %s exists and was not written by geforce-sriov; edit it yourself (see config.example) or remove it, then rerun\n",
			cfgfile);
		free(x);
		return GS_EXIT_REFUSED;
	}

	if (o->ndevices && live) {
		for (unsigned int i = 0; i < o->ndevices; i++) {
			struct gs_card c;

			gs_card_inspect(GS_LIVE_SYSFS, &o->devices[i], &c);
			if (gs_check_selectable(&c, gs_config_grants_release(&cfg, c.pf.str), err, sizeof(err))) {
				fprintf(stderr, "geforce-sriov: refused --device: %s\n", err);
				free(x);
				return GS_EXIT_REFUSED;
			}
			if (!c.entry->same_die)
				printf("%s: warning: %s\n", c.pf.str, GS_NO_SAME_DIE_MSG);
			if (c.entry->experimental)
				printf("%s: warning: experimental device ID: not verified end to end\n", c.pf.str);
			if ((c.entry->generation == GS_ADA || c.entry->generation == GS_BLACKWELL) &&
			    cfg.experimental_vfs != GS_SW_OFF)
				printf("%s: warning: experimental_vfs=on (default): on Ada/Blackwell, where the card reports 1 VF, present up to %u (not verified end to end); off keeps 1 (--experimental-vfs=off)\n",
				       c.pf.str, GS_DEFAULT_VFS);
			if (gs_config_grants_release(&cfg, c.pf.str) && c.boot_vga != 1)
				printf("%s: warning: --boot-display-release names it, but it is not the boot display device now (boot_vga=%d); prepare refuses this card while boot_display_release grants it\n",
				       c.pf.str, c.boot_vga);
		}
	} else if (o->ndevices) {
		printf("staging: the --device addresses are recorded unchecked (this host's PCI devices are not the target's);\n"
		       "run `geforce-sriov doctor` on the target host before enabling the boot unit\n");
	}
	/* The VF count against every selected card, the existing selection included. */
	for (unsigned int i = 0; live && i < cfg.nbdfs; i++) {
		struct gs_card c;
		char vfs[128];
		int v;

		gs_card_inspect(GS_LIVE_SYSFS, &cfg.bdfs[i], &c);
		if (!c.present || !c.entry)
			continue;
		gs_vfs_describe(c.entry, &cfg, vfs, sizeof(vfs));
		printf("%s: VFs: %s\n", c.pf.str, vfs);
		v = gs_vfs_check(c.entry, &cfg, err, sizeof(err));
		if (v < 0) {
			fprintf(stderr, "geforce-sriov: refused --vfs: %s: %s\n", c.pf.str, err);
			free(x);
			return GS_EXIT_REFUSED;
		}
		if (v > 0)
			printf("%s: warning: %s\n", c.pf.str, err);
	}
	if (o->vfs)
		printf("vfs=%u takes effect at the next boot (the count is fixed when the module loads); power the machine\n"
		       "fully off and on, as for the first prepared boot%s\n", o->vfs,
		       live ? "" : ". Staging checks no card: run `geforce-sriov doctor` on the target host");
	if (o->nrelease) {
		char rel[GS_BDF_LIST_STR_SIZE];

		gs_addr_list_str(cfg.boot_display_release, cfg.nrelease, rel, sizeof(rel));
		printf("boot_display_release=%s: the boot unit will defer and prepare none of the selected GPUs. After each\n"
		       "boot, prepare them by hand with `geforce-sriov prepare --release-console=...` (see `geforce-sriov status`).\n"
		       "Until then, do not load nvidia (also not automatically) or start NVIDIA's services; nvidia-vgpud is held back.\n%s\n",
		       rel, GS_CONSOLE_RELEASE_NOTE);
	}

	{
		const char *const subst[] = { "@BINDIR@", bindir, "@HOOK@", hook, "@VERSION@", GS_VERSION, NULL };

#define TRY(expr, what) do { \
		if ((r = (expr))) { \
			fprintf(stderr, "geforce-sriov: installing %s failed: %s\n", (what), strerror(-r)); \
			goto fail; \
		} \
	} while (0)
		gs_pathf(p, sizeof(p), "%s/geforce-sriov", src);
		gs_pathf(q, sizeof(q), "%s/geforce-sriov", bindir);
		TRY(copy_file(x, p, q, 0755), q);
		gs_pathf(p, sizeof(p), "%s/driver/geforce-sriov-patch", src);
		gs_pathf(q, sizeof(q), "%s/geforce-sriov-patch", bindir);
		TRY(copy_file(x, p, q, 0755), q);
		gs_pathf(p, sizeof(p), "%s/hook/libgeforce_sriov.so", src);
		TRY(copy_file(x, p, hook, 0755), hook);
		gs_pathf(p, sizeof(p), "%s/config/config.example", src);
		gs_pathf(q, sizeof(q), "%s/config.example", sharedir);
		TRY(copy_file(x, p, q, 0644), q);

		gs_pathf(p, sizeof(p), "%s/kernel", src);
		d = opendir(p);
		if (!d) {
			r = -errno;
			fprintf(stderr, "geforce-sriov: cannot read %s\n", p);
			goto fail;
		}
		while ((de = readdir(d))) {
			char sp[GS_PATH_MAX], tp[GS_PATH_MAX];
			struct stat st;

			if (!is_kernel_source(de->d_name) ||
			    gs_pathf(sp, sizeof(sp), "%s/kernel/%s", src, de->d_name) ||
			    stat(sp, &st) || !S_ISREG(st.st_mode) ||
			    gs_pathf(tp, sizeof(tp), "%s/%s", DKMS_SRC, de->d_name))
				continue;
			if ((r = copy_file(x, sp, tp, 0644))) {
				closedir(d);
				fprintf(stderr, "geforce-sriov: installing %s failed: %s\n", tp, strerror(-r));
				goto fail;
			}
		}
		closedir(d);
		gs_pathf(p, sizeof(p), "%s/include/gs_devices.h", src);
		TRY(copy_file(x, p, DKMS_SRC "/include/gs_devices.h", 0644), "gs_devices.h");

		gs_pathf(p, sizeof(p), "%s/systemd/geforce-sriov-prepare.service.in", src);
		TRY(put_template(x, p, UNIT_DIR "/" GS_UNIT_PREPARE, subst), GS_UNIT_PREPARE);
		gs_pathf(p, sizeof(p), "%s/systemd/nvidia-vgpud.service.d/geforce-sriov.conf.in", src);
		TRY(put_template(x, p, VGPUD_DROPIN_DIR "/geforce-sriov.conf", subst), "nvidia-vgpud drop-in");

		if (config_changed) {
			char text[GS_PATH_MAX + 1024];
			int n = gs_config_render(&cfg, text, sizeof(text));

			if (n < 0) {
				r = -EOVERFLOW;
				goto fail;
			}
			TRY(put(x, GS_CONFIG_PATH, text, (size_t)n, 0644), GS_CONFIG_PATH);
		} else {
			printf("kept %s (unchanged)\n", cfgfile);
		}
		TRY(make_dirs(x, GS_STATE_DIR), GS_STATE_DIR);
#undef TRY
	}
	if ((r = manifest_save(manifest, &x->m))) {
		fprintf(stderr, "geforce-sriov: writing %s failed: %s\n", manifest, strerror(-r));
		free(x);
		return GS_EXIT_FAIL;
	}
	free(x);

	if (!live) {
		printf("\nstaged below %s; nothing outside it was changed. On the target host, as root:\n", destdir);
		print_live_steps(bindir, cfg.nbdfs > 0);
		return GS_EXIT_OK;
	}

	{
		const char *rm_argv[] = { "dkms", "remove", DKMS_ID, "--all", NULL };
		const char *add_argv[] = { "dkms", "add", "-m", GS_DKMS_NAME, "-v", GS_VERSION, NULL };
		const char *build_argv[] = { "dkms", "build", "-m", GS_DKMS_NAME, "-v", GS_VERSION, NULL };
		const char *inst_argv[] = { "dkms", "install", "-m", GS_DKMS_NAME, "-v", GS_VERSION, NULL };
		const char *reload_argv[] = { "systemctl", "daemon-reload", NULL };
		const char *enable_argv[] = { "systemctl", "enable", GS_UNIT_PREPARE, NULL };
		const char *disable_argv[] = { "systemctl", "disable", GS_UNIT_PREPARE, NULL };

		if (gs_exists("/var/lib/dkms/" GS_DKMS_NAME "/" GS_VERSION) && gs_run(rm_argv) != 0) {
			fprintf(stderr, "geforce-sriov: dkms remove %s failed\n", DKMS_ID);
			return GS_EXIT_FAIL;
		}
		if (gs_run(add_argv) != 0 || gs_run(build_argv) != 0 || gs_run(inst_argv) != 0) {
			fprintf(stderr, "geforce-sriov: DKMS could not add/build/install %s for the running kernel; files are installed, `geforce-sriov uninstall` removes them\n",
				DKMS_ID);
			return GS_EXIT_FAIL;
		}
		if (gs_run(reload_argv) != 0) {
			fprintf(stderr, "geforce-sriov: systemctl daemon-reload failed\n");
			return GS_EXIT_FAIL;
		}
		if (cfg.nbdfs) {
			if (gs_run(enable_argv) != 0) {
				fprintf(stderr, "geforce-sriov: systemctl enable %s failed\n", GS_UNIT_PREPARE);
				return GS_EXIT_FAIL;
			}
			printf("\n%s enabled for the next boot (not started now). Check `%s/geforce-sriov doctor` before rebooting.\n"
			       "To skip preparation for one boot, add geforce_sriov.skip to the kernel command line (docs/recovery.md).\n"
			       "A patched NVIDIA module, if your driver needs it, is a separate manual step (docs/driver-patch.md).\n"
			       "After the reboot, `geforce-sriov status` names the next step.\n",
			       GS_UNIT_PREPARE, bindir);
		} else {
			gs_run(disable_argv);
			printf("\nno device selected: %s is installed but not enabled. Select GPUs with\n"
			       "`geforce-sriov install --device BDF` (one per GPU); this also sets identity=on and identity_force_host=on\n"
			       "and enables the unit.\n",
			       GS_UNIT_PREPARE);
		}
	}
	return GS_EXIT_OK;
fail:
	/* Keep a manifest of what was written so uninstall can remove it. */
	manifest_save(manifest, &x->m);
	free(x);
	return GS_EXIT_FAIL;
}

/* ---- uninstall ------------------------------------------------------------ */

/*
 * Active VFs on a GPU the module acts on. Its bdfs parameter is the authoritative list
 * (the config only adds to it). When that list cannot be read or parsed, safety cannot be
 * proven and the caller refuses. Other SR-IOV devices (network cards) never count.
 */
static int any_active_vfs(char *which, size_t len)
{
	struct gs_addr list[2 * GS_CFG_MAX_BDFS];
	unsigned int n = 0, k;
	struct gs_config cfg;
	char param[256], err[512];

	if (gs_read_attr(GS_LIVE_SYSFS "/module/" GS_MODULE_NAME "/parameters/bdfs", param, sizeof(param)) ||
	    gs_parse_addr_list("bdfs", param, list, &n, err, sizeof(err))) {
		snprintf(which, len, "the loaded module's bdfs parameter cannot be read or parsed, so active VFs on its GPUs cannot be ruled out");
		return 1;
	}
	if (gs_config_load(gs_config_path(), &cfg, err, sizeof(err)) == 0)
		for (k = 0; k < cfg.nbdfs && n < 2 * GS_CFG_MAX_BDFS; k++)
			list[n++] = cfg.bdfs[k];
	for (k = 0; k < n; k++) {
		char p[GS_PATH_MAX];
		unsigned int v = 0;

		gs_pathf(p, sizeof(p), GS_LIVE_SYSFS "/bus/pci/devices/%s/sriov_numvfs", list[k].str);
		if (gs_read_uint(p, &v) == 0 && v > 0) {
			snprintf(which, len, "%s has %u VFs", list[k].str, v);
			return 1;
		}
	}
	return 0;
}

static int any_prepared(void)
{
	DIR *d = opendir(GS_PREPARED_DIR);
	struct dirent *de;
	int found = 0;

	if (!d)
		return 0;
	while ((de = readdir(d)))
		if (de->d_name[0] != '.')
			found = 1;
	closedir(d);
	return found;
}

static int unload_module(void)
{
	char p[GS_PATH_MAX], why[256];
	/* rmmod needs no modules index, so it works whatever state DKMS is in. */
	const char *argv[] = { "rmmod", GS_MODULE_NAME, NULL };

	if (!gs_exists(GS_LIVE_SYSFS "/module/" GS_MODULE_NAME)) {
		printf("%s is not loaded\n", GS_MODULE_NAME);
		return 0;
	}
	if (any_active_vfs(why, sizeof(why))) {
		fprintf(stderr, "geforce-sriov: refused --unload: %s; disable VFs or reboot instead\n", why);
		return GS_EXIT_REFUSED;
	}
	if (any_prepared()) {
		fprintf(stderr, "geforce-sriov: refused --unload: a GPU was prepared this boot and its enumeration depends on the livepatch; reboot instead\n");
		return GS_EXIT_REFUSED;
	}
	gs_pathf(p, sizeof(p), GS_LIVE_SYSFS "/kernel/livepatch/%s/enabled", GS_MODULE_NAME);
	if (gs_exists(p) && gs_write_attr(p, "0")) {
		fprintf(stderr, "geforce-sriov: disabling livepatch %s failed\n", GS_MODULE_NAME);
		return GS_EXIT_FAIL;
	}
	gs_pathf(p, sizeof(p), GS_LIVE_SYSFS "/kernel/livepatch/%s", GS_MODULE_NAME);
	for (unsigned int ms = 0; gs_exists(p); ms += 100) {
		char tp[GS_PATH_MAX];
		unsigned int t = 1, e = 1;

		gs_pathf(tp, sizeof(tp), "%s/transition", p);
		if (gs_read_uint(tp, &t) == 0 && t == 0) {
			gs_pathf(tp, sizeof(tp), "%s/enabled", p);
			if (gs_read_uint(tp, &e) == 0 && e == 0)
				break;
		}
		if (ms >= 15000) {
			fprintf(stderr, "geforce-sriov: livepatch %s did not finish unpatching; module left loaded\n",
				GS_MODULE_NAME);
			return GS_EXIT_FAIL;
		}
		gs_sleep_ms(100);
	}
	if (gs_run(argv) != 0) {
		fprintf(stderr, "geforce-sriov: rmmod %s failed\n", GS_MODULE_NAME);
		return GS_EXIT_FAIL;
	}
	printf("%s unloaded\n", GS_MODULE_NAME);
	return 0;
}

int gs_cmd_uninstall(int unload)
{
	struct manifest *m;
	char prog[GS_PATH_MAX], sha[65];
	int r, rc = GS_EXIT_OK;

	if (geteuid() != 0) {
		fprintf(stderr, "geforce-sriov: uninstall must run as root\n");
		return GS_EXIT_REFUSED;
	}
	if ((r = gs_lock()))
		return r;
	/*
	 * Unload first: after `dkms remove` the module is gone from the modprobe index and
	 * `modprobe -r` can no longer find it. unload_module() refuses with active VFs or a
	 * GPU prepared this boot, and then nothing is removed.
	 */
	if (unload && (r = unload_module())) {
		fprintf(stderr, "geforce-sriov: nothing was removed\n");
		return r;
	}
	m = calloc(1, sizeof(*m));
	if (!m)
		return GS_EXIT_FAIL;
	if ((r = manifest_load(GS_MANIFEST, m))) {
		fprintf(stderr, "geforce-sriov: %s is corrupt (%s); nothing removed\n", GS_MANIFEST, strerror(-r));
		free(m);
		return GS_EXIT_FAIL;
	}
	if (m->n == 0) {
		printf("nothing installed by geforce-sriov (%s absent)\n", GS_MANIFEST);
		free(m);
		return GS_EXIT_OK;
	}

	if (gs_find_program("systemctl", prog, sizeof(prog)) == 0 && gs_exists(UNIT_DIR "/" GS_UNIT_PREPARE)) {
		const char *argv[] = { "systemctl", "disable", GS_UNIT_PREPARE, NULL };

		if (gs_run(argv) != 0) {
			fprintf(stderr, "geforce-sriov: systemctl disable %s failed\n", GS_UNIT_PREPARE);
			rc = GS_EXIT_FAIL;
		}
	}
	if (gs_exists("/var/lib/dkms/" GS_DKMS_NAME)) {
		const char *argv[] = { "dkms", "remove", DKMS_ID, "--all", NULL };

		if (gs_run(argv) != 0) {
			fprintf(stderr, "geforce-sriov: dkms remove %s failed\n", DKMS_ID);
			rc = GS_EXIT_FAIL;
		}
	}
	for (unsigned int i = 0; i < m->n; i++) {
		const struct entry *e = &m->e[i];

		if (e->type != 'f')
			continue;
		if (gs_sha256_file(e->path, sha)) {
			if (gs_exists(e->path))
				printf("kept %s (unreadable)\n", e->path);
			continue;
		}
		if (strcmp(sha, e->sha) != 0) {
			printf("kept %s (changed since install)\n", e->path);
			continue;
		}
		if (unlink(e->path) < 0) {
			fprintf(stderr, "geforce-sriov: removing %s failed: %s\n", e->path, strerror(errno));
			rc = GS_EXIT_FAIL;
		} else {
			printf("removed %s\n", e->path);
		}
	}
	/* The preparation guard is not an installed file: a reinstall must still see it. */
	if (gs_exists(GS_ATTEMPT_MARKER))
		printf("kept guard %s: an unfinished preparation needs a cold power cycle, then `geforce-sriov prepare --clear-guard`\n",
		       GS_ATTEMPT_MARKER);
	if (unlink(GS_MANIFEST) < 0 && errno != ENOENT)
		rc = GS_EXIT_FAIL;
	for (unsigned int i = m->n; i-- > 0;)
		if (m->e[i].type == 'd' && rmdir(m->e[i].path) == 0)
			printf("removed %s/\n", m->e[i].path);
	free(m);
	if (gs_find_program("systemctl", prog, sizeof(prog)) == 0) {
		const char *argv[] = { "systemctl", "daemon-reload", NULL };

		gs_run(argv);
	}
	if (!unload && gs_exists(GS_LIVE_SYSFS "/module/" GS_MODULE_NAME))
		printf("%s stays loaded until the next boot, which is an ordinary boot without preparation\n",
		       GS_MODULE_NAME);
	return rc;
}
