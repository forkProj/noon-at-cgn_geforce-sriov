// SPDX-License-Identifier: MIT
/* geforce-sriov: command-line entry point. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>

#include "gs_lifecycle.h"

static void usage(FILE *f)
{
	fprintf(f,
		"Usage: geforce-sriov COMMAND [OPTIONS]\n"
		"\n"
		"Read-only:\n"
		"  devices\n"
		"      List the device table.\n"
		"  doctor [--device BDF]... [--boot-display-release BDF]...\n"
		"         [--sysfs-root PATH] [--config FILE]\n"
		"         [--nvidia-source DIR] [--driver-file PATH]\n"
		"      Check the host and show what prepare would do. --nvidia-source,\n"
		"      --driver-file and --boot-display-release judge the host as install\n"
		"      will leave it.\n"
		"  status [--device BDF]... [--sysfs-root PATH] [--config FILE]\n"
		"      Show the module, guard and SR-IOV state of each selected GPU and the\n"
		"      next step. Exit 0 only when each one was prepared this boot and passes\n"
		"      the SR-IOV and BAR checks; exit 1 is normal before the first prepared\n"
		"      boot.\n"
		"  check [--device BDF]... [--type N]\n"
		"      Run before every VM start. Each VF of every selected GPU must be on\n"
		"      nvidia with power/control on, runtime_status active and its vGPU type:\n"
		"      type_ada= or type_ampere= in the config, else %u on Ada and %u on\n"
		"      Ampere. --type N expects N on every selected GPU, so use it with one\n"
		"      generation. Exit 0 ready, 1 not ready, 3 refused. It reads the state\n"
		"      once: a later re-probe or daemon restart can change it.\n"
		"\n"
		"Changing the system (root, live /sys only):\n"
		"  setup [--device BDF]... [--boot-display-release BDF]... [--vfs N]\n"
		"        [--driver=skip] [--interactive | --no-prompt]\n"
		"        [--nvidia-source DIR] [--driver-file PATH] [--accept-nvidia-license]\n"
		"      The guided path from a clean host to a ready VF: it runs the next steps\n"
		"      for the host's state, tells you when to power the machine off and on,\n"
		"      and names the guest drivers. It creates no VM. On a terminal or with\n"
		"      --interactive, it asks for what the options did not give and prints\n"
		"      the command that repeats the run; --no-prompt never asks. A single\n"
		"      eligible GPU that is not the boot display is selected by itself. Run\n"
		"      the same command again after the boot; first-install options are then\n"
		"      ignored.\n"
		"  install [--prefix PATH] [--destdir PATH] [--source DIR] [--vfs N]\n"
		"          [--device BDF]... [--boot-display-release BDF]...\n"
		"          [--ari=on|off] [--msix=on|off] [--driver=skip]\n"
		"          [--nvidia-source DIR] [--driver-file PATH] [--accept-nvidia-license]\n"
		"      Install the built tree. With DESTDIR (environment or --destdir), only\n"
		"      stage the files below it.\n"
		"  prepare [--ari=on|off] [--msix=on|off] [--release-console=BDF]... [--boot]\n"
		"      Load the geforce_sriov livepatch and remove and rescan the selected\n"
		"      GPUs before NVIDIA binds them. --release-console=BDF consents, by hand\n"
		"      after the boot, to a console release the config grants (name every\n"
		"      granted GPU). --boot is the boot unit's mode: with a grant it defers\n"
		"      and changes nothing.\n"
		"  prepare --clear-guard\n"
		"      Clear the guard of an unfinished attempt. Power the machine off and on\n"
		"      first.\n"
		"  activate [--type N] [--dry-run] [--boot]\n"
		"      Bring up the VFs of every selected GPU, set their vGPU type as check\n"
		"      expects it, then print what check prints. It refuses while any vGPU\n"
		"      exists, or when nvidia.ko is not the build install recorded (a kernel\n"
		"      update brings back the stock one). Run it only while your hypervisor\n"
		"      starts no VM. --boot (the boot unit) first waits up to 30 s for each\n"
		"      GPU to bind to nvidia; --dry-run checks and prints the steps.\n"
		"  uninstall [--unload]\n"
		"      Remove only the files geforce-sriov installed. --unload also unloads\n"
		"      the module when no VF is active and no GPU was prepared.\n"
		"  help, --help, -h; version, --version\n"
		"\n"
		"Options of setup and install\n"
		"(details: docs/installation.md#one-command-from-a-clean-host):\n"
		"  --device BDF               a GPU to use (physical function dddd:bb:ss.0)\n"
		"  --boot-display-release BDF grant a console release to the boot display GPU\n"
		"  --vfs N                    VFs per GPU (default %u; doctor shows the maximum)\n"
		"  --driver=skip              leave NVIDIA's files untouched\n"
		"  --nvidia-source DIR        kernel tree, default " GS_NVIDIA_SRC "\n"
		"  --driver-file PATH         NVIDIA's vGPU host .run package to install from\n"
		"  --accept-nvidia-license    accept NVIDIA's licence and run its installer\n"
		"\n"
		"Exit status: 0 ok, 1 failed (status: a selected GPU does not pass), 2 usage,\n"
		"3 refused, nothing changed (doctor: prepare would refuse), 4 locked, 5 guard\n"
		"set: power the machine off and on first.\n"
		"Config: " GS_CONFIG_PATH ". doctor, status, check, prepare, activate\n"
		"and uninstall read $" GS_CONFIG_ENV " instead when it is set; install\n"
		"always uses " GS_CONFIG_PATH ". setup reads $" GS_CONFIG_ENV " to\n"
		"tell whether the host is installed, and refuses it when it names an existing\n"
		"file other than " GS_CONFIG_PATH ".\n"
		"Supported: Ampere and Ada GPUs, NVIDIA host driver " GS_NVIDIA_VERSION " (other versions\n"
		"are not supported). Reference: docs/configuration.md#commands\n"
		"\n"
		"Example, from a clean host without questions (add one --device BDF per GPU\n"
		"when more than one is eligible, or for the boot display card):\n"
		"  geforce-sriov setup --driver-file PATH --accept-nvidia-license\n",
		GS_VGPU_TYPE_ADA, GS_VGPU_TYPE_AMPERE, GS_DEFAULT_VFS);
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
	return strstr(name, "vf") || strstr(name, "VF");
}

/* a is not an option of this command; say where the VF count is set when that was the intent. */
static int unknown_opt(const char *a, int install)
{
	/* --experimental-vfs is not a VF count. */
	if (strncmp(a, "--experimental", 14) == 0 || !is_vf_count_opt(a))
		return bad("unknown option '%s'", a);
	if (install)
		return bad("%s: the VF count option is --vfs N (1 up to the card's hardware default, default %u)", a,
			   GS_DEFAULT_VFS);
	return bad("%s: set the VF count with `geforce-sriov install --vfs N` (vfs=N in the config). It takes effect\n"
		   "at the next boot", a);
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

/* --driver-file PATH: an existing regular file, resolved once so every step reads the same one. */
static int parse_driver_file(const char *v, const char **out)
{
	static char real[PATH_MAX];
	struct stat st;

	if (*out)
		return bad("%s given twice", "--driver-file");
	if (!v[0] || !realpath(v, real) || stat(real, &st) || !S_ISREG(st.st_mode))
		return bad("--driver-file '%s' is not a regular file; give NVIDIA's vGPU host .run package", v);
	*out = real;
	return 0;
}

/* --driver-file and --accept-nvidia-license of install and setup. */
static int parse_package_opt(int argc, char **argv, int *i, struct gs_install_opts *o, int *matched)
{
	const char *v;

	*matched = 1;
	if ((v = opt_value(argc, argv, i, "--driver-file")))
		return parse_driver_file(v, &o->driver_file);
	if (strcmp(argv[*i], "--accept-nvidia-license") == 0) {
		o->accept_nvidia_license = 1;
		return 0;
	}
	*matched = 0;
	return 0;
}

/* Each --boot-display-release BDF of install, setup and doctor must also be given as --device BDF. */
static int check_release(const struct gs_addr *devices, unsigned int ndevices, const struct gs_addr *release,
			 unsigned int nrelease)
{
	for (unsigned int k = 0; k < nrelease; k++) {
		unsigned int d;

		for (d = 0; d < ndevices && strcmp(devices[d].str, release[k].str); d++)
			;
		if (d == ndevices)
			return bad("--boot-display-release %s must also be given as --device %s", release[k].str,
				   release[k].str);
	}
	return 0;
}

/* What install and setup check once every option is read. */
static int check_install_opts(const struct gs_install_opts *o)
{
	int r = check_release(o->devices, o->ndevices, o->release, o->nrelease);

	if (r)
		return r;
	if (o->driver_file && o->nvidia_source)
		return bad("--driver-file and --nvidia-source both name NVIDIA's kernel tree (--driver-file extracts it\n"
			   "from the package); give one");
	if (o->driver_file && o->skip_driver)
		return bad("--driver-file installs NVIDIA's files and --driver=skip leaves them untouched; give one");
	if (o->accept_nvidia_license && !o->driver_file)
		return bad("--accept-nvidia-license needs --driver-file PATH, NVIDIA's vGPU host .run package");
	return 0;
}

static int parse_switch_opt(int argc, char **argv, int *i, struct gs_overrides *ov, int *matched)
{
	static const char *const names[] = { "--ari", "--msix" };
	int *dst[] = { &ov->ari, &ov->msix };

	*matched = 0;
	for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
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

/* --type of activate and check; --device too when check, --dry-run and --boot when activate. */
static int parse_vgpu_opts(int argc, char **argv, struct gs_activate_opts *o, int activate)
{
	int i, r;

	memset(o, 0, sizeof(*o));
	for (i = 2; i < argc; i++) {
		const char *v;

		if (activate && strncmp(argv[i], "--device", 8) == 0 && (argv[i][8] == '\0' || argv[i][8] == '=')) {
			return bad("activate acts on every selected GPU (bdfs= in the config) and takes no --device");
		} else if (!activate && (v = opt_value(argc, argv, &i, "--device"))) {
			if ((r = parse_device("--device", v, o->devices, &o->ndevices)))
				return r;
			if (o->devices[o->ndevices - 1].func != 0)
				return bad("--device %s: name the physical function .0", v);
		} else if ((v = opt_value(argc, argv, &i, "--type"))) {
			if (o->type)
				return bad("%s given twice", "--type");
			if (gs_parse_vgpu_type(v, &o->type))
				return bad("--type '%s': give a vGPU type ID, 1 to %u, as nvidia/creatable_vgpu_types lists it", v,
					   GS_VGPU_TYPE_MAX);
		} else if (activate && strcmp(argv[i], "--dry-run") == 0) {
			o->dry_run = 1;
		} else if (activate && strcmp(argv[i], "--boot") == 0) {
			o->boot = 1;
		} else {
			return unknown_opt(argv[i], 0);
		}
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
		} else if (!status && (v = opt_value(argc, argv, &i, "--boot-display-release"))) {
			if ((r = parse_release("--boot-display-release", v, o.release, &o.nrelease)))
				return r;
		} else if ((v = opt_value(argc, argv, &i, "--sysfs-root"))) {
			if (gs_validate_abs_path(v) || stat(v, &st) || !S_ISDIR(st.st_mode))
				return bad("--sysfs-root '%s' must be an existing normalized absolute directory", v);
			o.sysroot = v;
		} else if ((v = opt_value(argc, argv, &i, "--config"))) {
			if (v[0] == '\0')
				return bad("%s needs a file", "--config");
			o.config_path = v;
		} else if (!status && (v = opt_value(argc, argv, &i, "--nvidia-source"))) {
			if (gs_validate_abs_path(v))
				return bad("--nvidia-source '%s' must be a normalized absolute path", v);
			o.nvidia_source = v;
		} else if (!status && (v = opt_value(argc, argv, &i, "--driver-file"))) {
			if ((r = parse_driver_file(v, &o.driver_file)))
				return r;
		} else {
			return unknown_opt(argv[i], 0);
		}
	}
	if (!status && (r = check_release(o.devices, o.ndevices, o.release, o.nrelease)))
		return r;
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
			return bad("%s is for doctor and status only. Staging writes the config from --device and the switches\n"
				   "and checks no device; run `geforce-sriov doctor` on the target host", argv[i]);
		return bad("%s is for doctor and status only; this command acts on the live system and its config",
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
	if (strcmp(cmd, "check") == 0) {
		struct gs_activate_opts o;

		if ((r = parse_vgpu_opts(argc, argv, &o, 0)))
			return r;
		return gs_cmd_check(&o);
	}

	if ((r = reject_diag_opts(argc, argv)))
		return r;

	if (strcmp(cmd, "install") == 0) {
		struct gs_install_opts o;
		const char *env = getenv("DESTDIR");

		memset(&o, 0, sizeof(o));
		o.ov.ari = o.ov.msix = GS_SW_UNSET;
		o.destdir = env && env[0] ? env : NULL;
		for (i = 2; i < argc; i++) {
			const char *v;

			if ((r = parse_switch_opt(argc, argv, &i, &o.ov, &matched)))
				return r;
			if (matched)
				continue;
			if ((r = parse_package_opt(argc, argv, &i, &o, &matched)))
				return r;
			if (matched)
				continue;
			if (strcmp(argv[i], "--release-note-shown") == 0) {
				/* setup passes it after its console-release question, which explained the grant */
				o.release_note_shown = 1;
			} else if ((v = opt_value(argc, argv, &i, "--device"))) {
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
			} else if ((v = opt_value(argc, argv, &i, "--driver"))) {
				if (strcmp(v, "skip") != 0)
					return bad("--driver '%s': the only value is skip (leave NVIDIA's files untouched)", v);
				o.skip_driver = 1;
			} else if ((v = opt_value(argc, argv, &i, "--nvidia-source"))) {
				if (gs_validate_abs_path(v))
					return bad("--nvidia-source '%s' must be a normalized absolute path", v);
				o.nvidia_source = v;
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
		if ((r = check_install_opts(&o)))
			return r;
		return gs_cmd_install(&o);
	}
	if (strcmp(cmd, "prepare") == 0) {
		struct gs_overrides ov = { GS_SW_UNSET, GS_SW_UNSET };
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
		if (clear && (ov.ari != GS_SW_UNSET || ov.msix != GS_SW_UNSET || po.boot || po.nrelease))
			return bad("%s takes no other option", "--clear-guard");
		if (po.boot && po.nrelease)
			return bad("--boot is the boot unit's mode and does not release a console; run --release-console by hand, without --boot");
		return gs_cmd_prepare(&ov, &po, clear);
	}
	if (strcmp(cmd, "activate") == 0) {
		struct gs_activate_opts o;

		if ((r = parse_vgpu_opts(argc, argv, &o, 1)))
			return r;
		return gs_cmd_activate(&o);
	}
	if (strcmp(cmd, "setup") == 0) {
		struct gs_install_opts o;
		enum gs_prompt prompt = GS_PROMPT_AUTO;

		memset(&o, 0, sizeof(o));
		o.ov.ari = o.ov.msix = GS_SW_UNSET;
		for (i = 2; i < argc; i++) {
			const char *v;

			if ((r = parse_package_opt(argc, argv, &i, &o, &matched)))
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
			} else if ((v = opt_value(argc, argv, &i, "--vfs"))) {
				if (o.vfs)
					return bad("%s given twice", "--vfs");
				if (gs_parse_vfs(v, &o.vfs))
					return bad("--vfs '%s': give a number of VFs from 1 to %u (default %u)", v, GS_VFS_MAX,
						   GS_DEFAULT_VFS);
			} else if ((v = opt_value(argc, argv, &i, "--driver"))) {
				if (strcmp(v, "skip") != 0)
					return bad("--driver '%s': the only value is skip (leave NVIDIA's files untouched)", v);
				o.skip_driver = 1;
			} else if ((v = opt_value(argc, argv, &i, "--nvidia-source"))) {
				if (gs_validate_abs_path(v))
					return bad("--nvidia-source '%s' must be a normalized absolute path", v);
				o.nvidia_source = v;
			} else if (strcmp(argv[i], "--interactive") == 0 || strcmp(argv[i], "--no-prompt") == 0) {
				enum gs_prompt want = argv[i][2] == 'i' ? GS_PROMPT_ON : GS_PROMPT_OFF;

				if (prompt != GS_PROMPT_AUTO && prompt != want)
					return bad("--interactive asks and --no-prompt never does; give one");
				prompt = want;
			} else {
				return unknown_opt(argv[i], 1);
			}
		}
		if ((r = check_install_opts(&o)))
			return r;
		return gs_cmd_setup(&o, prompt);
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
