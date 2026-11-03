// SPDX-License-Identifier: MIT
/* geforce-sriov: command-line entry point. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gs_lifecycle.h"

static void usage(FILE *f)
{
	fprintf(f,
		"Usage: geforce-sriov COMMAND [OPTIONS]\n"
		"\n"
		"Read-only:\n"
		"  devices                    list the device table\n"
		"  doctor [--device BDF]... [--sysfs-root PATH] [--config FILE]\n"
		"                             preflight checks and what prepare would do\n"
		"  status [--device BDF]... [--sysfs-root PATH] [--config FILE]\n"
		"                             module, guard and SR-IOV state of the selected GPUs and the\n"
		"                             next step; exits 0 only when each was prepared this boot and\n"
		"                             passes the SR-IOV and BAR checks (exit 1 is normal before the\n"
		"                             first prepared boot)\n"
		"\n"
		"Changing the system (root; the live /sys only):\n"
		"  install [--prefix PATH] [--device BDF]... [--boot-display-release BDF]... [--vfs N]\n"
		"          [--destdir PATH] [--source DIR] [--ari=on|off] [--msix=on|off] [--experimental-vfs=on|off]\n"
		"                             install the built tree; with DESTDIR (env or --destdir)\n"
		"                             only stage files below it. --device selects the GPUs\n"
		"                             (physical function dddd:bb:ss.0) and turns identity on;\n"
		"                             --boot-display-release grants a console release for the\n"
		"                             boot display GPU among them (the boot unit then defers);\n"
		"                             --vfs records the VF count per GPU (vfs=N in the config)\n"
		"  prepare [--ari=on|off] [--msix=on|off] [--experimental-vfs=on|off]\n"
		"          [--release-console=BDF]... [--boot]\n"
		"                             load the geforce_sriov livepatch and remove+rescan the\n"
		"                             selected GPUs before NVIDIA binds them. --release-console\n"
		"                             consents, by hand after boot, to the console release the\n"
		"                             config grants (name every granted GPU); --boot is how the\n"
		"                             boot unit runs it: with a grant it defers and changes nothing\n"
		"  prepare --clear-guard      clear the guard of an unfinished attempt, after a power cycle\n"
		"  uninstall [--unload]       remove only files installed by geforce-sriov; --unload also\n"
		"                             unloads the module when no VF is active and no GPU was prepared\n"
		"\n"
		"  --vfs N                    VFs per selected GPU, 1 up to the card's hardware default\n"
		"                             (`devices` lists it); default %u. ARI decoding in the GPU does\n"
		"                             not work for now, so more than %u VFs are expected not to work\n"
		"                             (the module refuses to load if the card's VF layout check\n"
		"                             fails). Takes effect at the next boot; `enable` enables the\n"
		"                             count the card presents\n"
		"  --experimental-vfs=on|off  on (default): on Ada/Blackwell, where the card reports 1 VF,\n"
		"                             present up to %u (not verified end to end); off keeps 1\n"
		"\n"
		"  --version, --help\n"
		"\n"
		"Exit status: 0 ok, 1 failed (status: not every selected GPU passes, for example before the first\n"
		"prepared boot), 2 usage, 3 refused (nothing changed; doctor: prepare would refuse), 4 locked,\n"
		"5 guard set: a cold power cycle is needed.\n"
		"Config: %s (override with $%s).\n",
		GS_DEFAULT_VFS, GS_DEFAULT_VFS, GS_DEFAULT_VFS, GS_CONFIG_PATH, GS_CONFIG_ENV);
}

static int bad(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static int bad(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "geforce-sriov: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\nTry 'geforce-sriov --help'.\n");
	return GS_EXIT_USAGE;
}

/* "--vfs", "--num-vfs=8", "--max_vfs" ...: an option asking for a VF count. */
static int is_vf_count_opt(const char *a)
{
	size_t n = strcspn(a, "=");
	char name[32];

	if (strncmp(a, "--", 2) != 0 || n >= sizeof(name))
		return 0;
	memcpy(name, a, n);
	name[n] = '\0';
	return strcmp(name, "--experimental-vfs") != 0 && (strstr(name, "vf") || strstr(name, "VF"));
}

/* a is not an option of this command; say where the VF count is set when that was the intent. */
static int unknown_opt(const char *a, int install)
{
	if (strncmp(a, "--experimental_vfs", 18) == 0 && (a[18] == '\0' || a[18] == '='))
		return bad("unknown option '%s'; use --experimental-vfs=on|off", a);
	if (!is_vf_count_opt(a))
		return bad("unknown option '%s'", a);
	if (install)
		return bad("%s: the VF count option is --vfs N (1 up to the card's hardware default, default %u)", a,
			   GS_DEFAULT_VFS);
	return bad("%s: set the VF count with `geforce-sriov install --vfs N` (vfs=N in the config); it takes effect\n"
		   "at the next boot, and `enable` takes no count", a);
}

/* "--name=value" or "--name value"; returns the value or NULL when absent. */
static const char *opt_value(int argc, char **argv, int *i, const char *name)
{
	size_t n = strlen(name);
	const char *a = argv[*i];

	if (strncmp(a, name, n) != 0)
		return NULL;
	if (a[n] == '=')
		return a + n + 1;
	if (a[n] != '\0')
		return NULL;
	if (*i + 1 >= argc)
		return "";
	return argv[++*i];
}

static int parse_device(const char *opt, const char *v, struct gs_addr *list, unsigned int *n)
{
	struct gs_addr a;
	unsigned int i;

	if (gs_addr_parse(v, &a))
		return bad("%s '%s' is not a full PCI address dddd:bb:ss.f as /sys/bus/pci/devices names it (lowercase\n"
			   "hex; a domain longer than 4 digits has no leading zero; at most %x)", opt, v, GS_PCI_DOMAIN_MAX);
	for (i = 0; i < *n; i++)
		if (strcmp(list[i].str, a.str) == 0)
			return bad("%s %s given twice", opt, v);
	if (*n == GS_CFG_MAX_BDFS)
		return bad("at most 8 %s options (%s)", opt, v);
	list[(*n)++] = a;
	return 0;
}

/* A physical-function address for a console-release option. */
static int parse_release(const char *opt, const char *v, struct gs_addr *list, unsigned int *n)
{
	int r = parse_device(opt, v, list, n);

	if (r)
		return r;
	if (list[*n - 1].func != 0)
		return bad("%s %s: name the physical function .0", opt, v);
	return 0;
}

static int parse_switch_opt(int argc, char **argv, int *i, struct gs_overrides *ov, int *matched)
{
	static const char *const names[] = { "--ari", "--msix", "--experimental-vfs" };
	int *dst[] = { &ov->ari, &ov->msix, &ov->experimental_vfs };

	*matched = 0;
	for (size_t k = 0; k < 3; k++) {
		const char *v = opt_value(argc, argv, i, names[k]);

		if (!v)
			continue;
		*matched = 1;
		if (gs_parse_switch(v, dst[k]))
			return bad("%s takes on or off", names[k]);
		return 0;
	}
	return 0;
}

static int diag_main(int argc, char **argv, int status)
{
	struct gs_diag_opts o;
	struct stat st;
	int i, r;

	memset(&o, 0, sizeof(o));
	for (i = 2; i < argc; i++) {
		const char *v;

		if ((v = opt_value(argc, argv, &i, "--device"))) {
			if ((r = parse_device("--device", v, o.devices, &o.ndevices)))
				return r;
		} else if ((v = opt_value(argc, argv, &i, "--sysfs-root"))) {
			if (gs_validate_abs_path(v) || stat(v, &st) || !S_ISDIR(st.st_mode))
				return bad("--sysfs-root '%s' must be an existing normalized absolute directory", v);
			o.sysroot = v;
		} else if ((v = opt_value(argc, argv, &i, "--config"))) {
			if (v[0] == '\0')
				return bad("%s needs a file", "--config");
			o.config_path = v;
		} else {
			return unknown_opt(argv[i], 0);
		}
	}
	return status ? gs_cmd_status(&o) : gs_cmd_doctor(&o);
}

/* Mutating commands never take a diagnostic root or config: name what they act on instead. */
static int reject_diag_opts(int argc, char **argv)
{
	const char *env = getenv("DESTDIR");
	int install = strcmp(argv[1], "install") == 0, stage = install && env && env[0];
	int i;

	for (i = 2; i < argc && install; i++)
		if ((strncmp(argv[i], "--destdir=", 10) == 0 && argv[i][10]) ||
		    (strcmp(argv[i], "--destdir") == 0 && i + 1 < argc && argv[i + 1][0]))
			stage = 1;
	for (i = 2; i < argc; i++) {
		if (strncmp(argv[i], "--sysfs-root", 12) != 0 && strncmp(argv[i], "--config", 8) != 0)
			continue;
		if (stage)
			return bad("%s is for doctor/status only; staging writes the config from --device and the switches\n"
				   "and checks no devices (run `geforce-sriov doctor` on the target host)", argv[i]);
		return bad("%s is for doctor/status only; this command acts on the live system and its config",
			   argv[i]);
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *cmd;
	int i, r, matched;

	if (argc < 2) {
		usage(stderr);
		return GS_EXIT_USAGE;
	}
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			usage(stdout);
			return GS_EXIT_OK;
		}
	}
	cmd = argv[1];
	if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "version") == 0) {
		printf("geforce-sriov %s\n", GS_VERSION);
		return GS_EXIT_OK;
	}
	if (strcmp(cmd, "help") == 0) {
		usage(stdout);
		return GS_EXIT_OK;
	}
	if (strcmp(cmd, "devices") == 0) {
		if (argc > 2)
			return unknown_opt(argv[2], 0);
		return gs_cmd_devices();
	}
	if (strcmp(cmd, "doctor") == 0)
		return diag_main(argc, argv, 0);
	if (strcmp(cmd, "status") == 0)
		return diag_main(argc, argv, 1);

	if ((r = reject_diag_opts(argc, argv)))
		return r;

	if (strcmp(cmd, "install") == 0) {
		struct gs_install_opts o;
		const char *env = getenv("DESTDIR");

		memset(&o, 0, sizeof(o));
		o.ov.ari = o.ov.msix = o.ov.experimental_vfs = GS_SW_UNSET;
		o.destdir = env && env[0] ? env : NULL;
		for (i = 2; i < argc; i++) {
			const char *v;

			if ((r = parse_switch_opt(argc, argv, &i, &o.ov, &matched)))
				return r;
			if (matched)
				continue;
			if ((v = opt_value(argc, argv, &i, "--device"))) {
				if ((r = parse_device("--device", v, o.devices, &o.ndevices)))
					return r;
				if (o.devices[o.ndevices - 1].func != 0)
					return bad("--device %s: select the physical function .0", v);
			} else if ((v = opt_value(argc, argv, &i, "--boot-display-release"))) {
				if ((r = parse_release("--boot-display-release", v, o.release, &o.nrelease)))
					return r;
			} else if ((v = opt_value(argc, argv, &i, "--prefix"))) {
				o.prefix = v;
			} else if ((v = opt_value(argc, argv, &i, "--destdir"))) {
				if (!v[0])
					return bad("%s needs a path (omit it for a live install)", "--destdir");
				o.destdir = v;
			} else if ((v = opt_value(argc, argv, &i, "--source"))) {
				if (gs_validate_abs_path(v))
					return bad("--source '%s' must be a normalized absolute path", v);
				o.source = v;
			} else if ((v = opt_value(argc, argv, &i, "--vfs"))) {
				if (o.vfs)
					return bad("%s given twice", "--vfs");
				if (gs_parse_vfs(v, &o.vfs))
					return bad("--vfs '%s': give a number of VFs from 1 to %u (default %u)", v, GS_VFS_MAX,
						   GS_DEFAULT_VFS);
			} else {
				return unknown_opt(argv[i], 1);
			}
		}
		for (unsigned int k = 0; k < o.nrelease; k++) {
			unsigned int d;

			for (d = 0; d < o.ndevices && strcmp(o.devices[d].str, o.release[k].str); d++)
				;
			if (d == o.ndevices)
				return bad("--boot-display-release %s must also be given as --device %s", o.release[k].str,
					   o.release[k].str);
		}
		return gs_cmd_install(&o);
	}
	if (strcmp(cmd, "prepare") == 0) {
		struct gs_overrides ov = { GS_SW_UNSET, GS_SW_UNSET, GS_SW_UNSET };
		struct gs_prepare_opts po;
		int clear = 0;

		memset(&po, 0, sizeof(po));
		for (i = 2; i < argc; i++) {
			const char *v;

			if ((r = parse_switch_opt(argc, argv, &i, &ov, &matched)))
				return r;
			if (matched)
				continue;
			if (strcmp(argv[i], "--clear-guard") == 0) {
				clear = 1;
			} else if (strcmp(argv[i], "--boot") == 0) {
				po.boot = 1;
			} else if ((v = opt_value(argc, argv, &i, "--release-console"))) {
				if ((r = parse_release("--release-console", v, po.release, &po.nrelease)))
					return r;
			} else {
				return unknown_opt(argv[i], 0);
			}
		}
		if (clear && (ov.ari != GS_SW_UNSET || ov.msix != GS_SW_UNSET || ov.experimental_vfs != GS_SW_UNSET ||
			      po.boot || po.nrelease))
			return bad("%s takes no other option", "--clear-guard");
		if (po.boot && po.nrelease)
			return bad("--boot is the boot unit's mode and does not release a console; run --release-console by hand, without --boot");
		return gs_cmd_prepare(&ov, &po, clear);
	}
	if (strcmp(cmd, "enable") == 0) {
		if (argc > 2)
			return unknown_opt(argv[2], 0);
		return gs_cmd_enable();
	}
	if (strcmp(cmd, "uninstall") == 0) {
		int unload = 0;

		for (i = 2; i < argc; i++) {
			if (strcmp(argv[i], "--unload") == 0)
				unload = 1;
			else
				return unknown_opt(argv[i], 0);
		}
		return gs_cmd_uninstall(unload);
	}
	return bad("unknown command '%s'", cmd);
}
