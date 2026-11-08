// SPDX-License-Identifier: MIT
/*
 * CLI and lifecycle tests. Fixture sysfs trees are built in a temporary
 * directory; only read-only commands and DESTDIR staging run against them.
 * Environment: GS_CLI = built geforce-sriov binary, GS_SRC = source tree.
 */
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "gs_lifecycle.h"

static int failures, checks;
static char tmp[256];
static const char *cli, *srcdir;

#define CHECK(cond, ...) do { \
		checks++; \
		if (!(cond)) { \
			failures++; \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__); \
			fputc('\n', stderr); \
		} \
	} while (0)

static void die(const char *what)
{
	perror(what);
	exit(2);
}

static void writef(const char *path, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void writef(const char *path, const char *fmt, ...)
{
	char dir[GS_PATH_MAX], *slash;
	va_list ap;
	FILE *f;

	gs_pathf(dir, sizeof(dir), "%s", path);
	slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		if (gs_mkdir_p(dir, 0755))
			die(dir);
	}
	f = fopen(path, "w");
	if (!f)
		die(path);
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fclose(f);
}

static void link_to(const char *target, const char *path)
{
	unlink(path);
	if (symlink(target, path))
		die(path);
}

static char *slurp(const char *path)
{
	char *data;
	size_t len;

	if (gs_read_whole(path, &data, &len, 1 << 22))
		return NULL;
	return data;
}

/* Everything the child writes to fd until it exits; returns its exit status. */
static int collect(int fd, pid_t pid, char **out)
{
	size_t cap = 65536, len = 0;
	char *buf = malloc(cap);
	int status;

	if (!buf)
		die("malloc");
	for (;;) {
		ssize_t r;

		if (len + 1 >= cap) {
			cap *= 2;
			buf = realloc(buf, cap);
			if (!buf)
				die("realloc");
		}
		r = read(fd, buf + len, cap - len - 1);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		len += (size_t)r;
	}
	buf[len] = '\0';
	close(fd);
	waitpid(pid, &status, 0);
	*out = buf;
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

/* Run the CLI; returns exit status, output (stdout+stderr) in *out (caller frees). */
static int run_cli(char **out, const char *destdir, const char *const args[])
{
	const char *argv[32];
	int pipefd[2];
	size_t n;
	pid_t pid;

	argv[0] = cli;
	for (n = 0; args[n]; n++)
		argv[n + 1] = args[n];
	argv[n + 1] = NULL;
	if (pipe(pipefd))
		die("pipe");
	pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0) {
		dup2(pipefd[1], 1);
		dup2(pipefd[1], 2);
		close(pipefd[0]);
		unsetenv(GS_CONFIG_ENV);
		if (destdir)
			setenv("DESTDIR", destdir, 1);
		else
			unsetenv("DESTDIR");
		execv(cli, (char *const *)argv);
		_exit(127);
	}
	close(pipefd[1]);
	return collect(pipefd[0], pid, out);
}

/* ---- fixture ---------------------------------------------------------- */

struct fx_fn {
	const char *bdf;
	const char *device;
	const char *class_code;
	const char *driver; /* NULL: unbound */
};

/* Where a card sits: its immediate bridge (path below root/devices, ending in the bridge's name) and its PF. */
struct fx_topo {
	const char *bridge_path;
	const char *pf;
};

static const struct fx_topo topo_default = { "pci0000:00/0000:00:02.0", "0000:03:00.0" };

/* A GPU (+ functions) at k->pf behind the bridge k->bridge_path; several cards may share one root. */
static void make_card(const char *root, const struct fx_topo *k, const struct fx_fn *fns, int boot_vga,
		      const char *sriov)
{
	char bridge[GS_PATH_MAX], p[GS_PATH_MAX], q[GS_PATH_MAX];
	const char *bname = strrchr(k->bridge_path, '/');
	int buslen = (int)(strrchr(k->pf, ':') - k->pf);

	bname = bname ? bname + 1 : k->bridge_path;
	gs_pathf(bridge, sizeof(bridge), "%s/devices/%s", root, k->bridge_path);
	gs_pathf(p, sizeof(p), "%s/vendor", bridge);
	writef(p, "0x8086\n");
	gs_pathf(p, sizeof(p), "%s/device", bridge);
	writef(p, "0x1901\n");
	gs_pathf(p, sizeof(p), "%s/class", bridge);
	writef(p, "0x060400\n");
	gs_pathf(p, sizeof(p), "%s/power/control", bridge);
	writef(p, "auto\n");
	gs_pathf(p, sizeof(p), "%s/power/runtime_status", bridge);
	writef(p, "suspended\n");
	gs_pathf(p, sizeof(p), "%s/pci_bus/%.*s/rescan", bridge, buslen, k->pf);
	writef(p, "%s", "");
	gs_pathf(p, sizeof(p), "%s/bus/pci/devices", root);
	gs_mkdir_p(p, 0755);
	gs_pathf(p, sizeof(p), "%s/bus/pci/drivers/snd_hda_intel", root);
	gs_mkdir_p(p, 0755);
	gs_pathf(p, sizeof(p), "%s/bus/pci/drivers/nvidia", root);
	gs_mkdir_p(p, 0755);
	gs_pathf(p, sizeof(p), "%s/bus/pci/drivers/xhci_hcd", root);
	gs_mkdir_p(p, 0755);
	gs_pathf(p, sizeof(p), "%s/kernel/livepatch", root);
	gs_mkdir_p(p, 0755);
	gs_pathf(p, sizeof(p), "%s/kernel/iommu_groups/12", root);
	gs_mkdir_p(p, 0755);
	gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s", root, bname);
	gs_pathf(q, sizeof(q), "../../../devices/%s", k->bridge_path);
	link_to(q, p);

	for (; fns->bdf; fns++) {
		char dev[GS_PATH_MAX];
		int is_pf = strcmp(fns->bdf, k->pf) == 0;

		gs_pathf(dev, sizeof(dev), "%s/%s", bridge, fns->bdf);
		gs_pathf(p, sizeof(p), "%s/vendor", dev);
		writef(p, "0x%s\n", strncmp(fns->device, "8086", 4) == 0 ? "8086" : "10de");
		gs_pathf(p, sizeof(p), "%s/device", dev);
		writef(p, "0x%s\n", strncmp(fns->device, "8086:", 5) == 0 ? fns->device + 5 : fns->device);
		gs_pathf(p, sizeof(p), "%s/class", dev);
		writef(p, "0x%s\n", fns->class_code);
		gs_pathf(p, sizeof(p), "%s/subsystem_vendor", dev);
		writef(p, "0x10de\n");
		gs_pathf(p, sizeof(p), "%s/subsystem_device", dev);
		writef(p, "0x146d\n");
		if (is_pf) {
			gs_pathf(p, sizeof(p), "%s/boot_vga", dev);
			writef(p, "%d\n", boot_vga);
			if (sriov) {
				unsigned int total = 0, num = 0;

				sscanf(sriov, "%u/%u", &num, &total);
				gs_pathf(p, sizeof(p), "%s/sriov_totalvfs", dev);
				writef(p, "%u\n", total);
				gs_pathf(p, sizeof(p), "%s/sriov_numvfs", dev);
				writef(p, "%u\n", num);
			}
		}
		if (fns->driver) {
			gs_pathf(p, sizeof(p), "%s/driver", dev);
			gs_pathf(q, sizeof(q), "../../../../bus/pci/drivers/%s", fns->driver);
			link_to(q, p);
		}
		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s", root, fns->bdf);
		gs_pathf(q, sizeof(q), "../../../devices/%s/%s", k->bridge_path, fns->bdf);
		link_to(q, p);
	}
}

/* A GPU at 0000:03:00.0 (+ functions) behind bridge 0000:00:02.0. */
static void make_fixture(const char *root, const struct fx_fn *fns, int boot_vga, const char *sriov)
{
	make_card(root, &topo_default, fns, boot_vga, sriov);
}

static const struct fx_fn gpu_with_audio[] = {
	{ "0000:03:00.0", "2206", "030000", NULL },
	{ "0000:03:00.1", "1aef", "040300", "snd_hda_intel" },
	{ NULL, NULL, NULL, NULL },
};

static void fixture(const char *name, const struct fx_fn *fns, int boot_vga, const char *sriov, char *root,
		    size_t len)
{
	snprintf(root, len, "%s/%s", tmp, name);
	make_fixture(root, fns, boot_vga, sriov);
}

static void plan_for(const char *root, struct gs_plan *plan, struct gs_card *card)
{
	struct gs_addr pf;

	gs_addr_parse("0000:03:00.0", &pf);
	gs_card_inspect(root, &pf, card);
	gs_plan_prepare(card, NULL, plan);
}

/* ---- tests ------------------------------------------------------------- */

static void test_addr(void)
{
	struct gs_addr a;

	CHECK(gs_addr_parse("0000:03:00.0", &a) == 0 && a.bus == 3 && a.slot == 0 && a.func == 0, "canonical");
	CHECK(gs_addr_parse("000a:ff:1f.7", &a) == 0 && a.domain == 10 && a.slot == 0x1f && a.func == 7, "max");
	CHECK(gs_addr_parse("0000:03:00.0 ", &a) != 0, "trailing space");
	CHECK(gs_addr_parse("03:00.0", &a) != 0, "short form");
	CHECK(gs_addr_parse("0000:0A:00.0", &a) != 0, "uppercase");
	CHECK(gs_addr_parse("0000:03:20.0", &a) != 0, "slot > 0x1f");
	CHECK(gs_addr_parse("0000:03:00.8", &a) != 0, "function 8");
	CHECK(gs_addr_parse("0000-03:00.0", &a) != 0, "separator");
	/* Domains as sysfs prints them ("%04x"): 5..8 digits without a leading 0 (Intel VMD uses 10000+). */
	CHECK(gs_addr_parse("10000:e1:00.0", &a) == 0 && a.domain == 0x10000 && a.bus == 0xe1 &&
	      strcmp(a.str, "10000:e1:00.0") == 0, "5-digit domain");
	CHECK(gs_addr_parse("7fffffff:00:00.0", &a) == 0 && a.domain == 0x7fffffff, "largest domain");
	CHECK(gs_addr_parse("010000:e1:00.0", &a) != 0, "leading zero beyond 4 domain digits");
	CHECK(gs_addr_parse("80000000:00:00.0", &a) != 0, "domain above the kernel's int range");
	CHECK(gs_addr_parse("100000000:00:00.0", &a) != 0, "9 domain digits");
	CHECK(gs_addr_parse("000:03:00.0", &a) != 0, "3 domain digits");
}

static void test_config(void)
{
	struct gs_config c;
	char err[512], path[GS_PATH_MAX], text[4096], *data;
	int n;
	static const char *const bad[] = {
		"ari =on\n", "ari=on \n", "ari=on\r\n", "ari=\n", "ari=yes\n", "ari=on\nari=off\n", "vfs=0\n",
		"vfs=17\n", "vfs=04\n", "vfs=four\n", "vfs=-1\n", "vfs=2\nvfs=3\n", "num_vfs=2\n", "max_vfs=8\n",
		"ARI=on\n", "bdfs=0000:03:00.1\n", "bdfs=0000:03:00.0,0000:03:00.0\n", "bdfs=03:00.0\n",
		"bdfs=0000:03:00.0,\n", "noequals\n", " #comment\n", "sriov_manage=relative/path\n",
		"sriov_manage=/usr/../bin/x\n", "msix=yes\n", "msix=on\nmsix=off\n", "MSIX=on\n",
		"bdfs=0000:01:00.0,0000:02:00.0,0000:03:00.0,0000:04:00.0,0000:05:00.0,0000:06:00.0,0000:07:00.0,0000:08:00.0,0000:09:00.0\n",
		NULL,
	};

	CHECK(gs_config_parse("", 0, &c, err, sizeof(err)) == 0 && c.ari == GS_SW_ON && c.msix == GS_SW_ON &&
	      c.identity == GS_SW_OFF && c.nbdfs == 0,
	      "defaults");
	/* A config without vfs (every config written before the key existed) keeps 4. */
	CHECK(c.vfs == 0 && gs_config_vfs(&c) == GS_DEFAULT_VFS, "default VF count");
	/* experimental_vfs and twin_id are not keys: a config that has them is refused, the key named. */
	n = snprintf(text, sizeof(text), "bdfs=0000:03:00.0\nari=on\nmsix=on\nexperimental_vfs=off\ntwin_id=off\n");
	CHECK(gs_config_parse(text, (size_t)n, &c, err, sizeof(err)) != 0 &&
	      strcmp(err, "line 4: unknown key 'experimental_vfs'; delete this line") == 0,
	      "experimental_vfs refused: %s", err);
	CHECK(gs_config_parse("twin_id=on\n", 11, &c, err, sizeof(err)) != 0 &&
	      strcmp(err, "line 1: unknown key 'twin_id'; delete this line") == 0, "twin_id refused: %s", err);
	CHECK(gs_config_parse("vfs=2\n", 6, &c, err, sizeof(err)) == 0 && gs_config_vfs(&c) == 2, "vfs=2: %s", err);
	CHECK(gs_config_parse("vfs=16", 6, &c, err, sizeof(err)) == 0 && gs_config_vfs(&c) == 16, "vfs=16: %s", err);
	CHECK(gs_config_parse("num_vfs=2\n", 10, &c, err, sizeof(err)) != 0 && strstr(err, "vfs="),
	      "VF-like key points to vfs: %s", err);
	n = snprintf(text, sizeof(text), "# c\n\nbdfs=0000:03:00.0,0000:81:00.0\nari=off\nidentity=on\nmsix=off");
	CHECK(gs_config_parse(text, (size_t)n, &c, err, sizeof(err)) == 0, "valid config: %s", err);
	CHECK(c.nbdfs == 2 && strcmp(c.bdfs[1].str, "0000:81:00.0") == 0, "bdfs list");
	CHECK(c.ari == GS_SW_OFF && c.msix == GS_SW_OFF && c.identity == GS_SW_ON, "switches, last line without newline");
	for (int i = 0; bad[i]; i++)
		CHECK(gs_config_parse(bad[i], strlen(bad[i]), &c, err, sizeof(err)) != 0, "rejects %s", bad[i]);

	/* render -> parse keeps everything */
	gs_config_defaults(&c);
	gs_addr_parse("0000:03:00.0", &c.bdfs[0]);
	c.nbdfs = 1;
	c.identity = GS_SW_ON;
	c.msix = GS_SW_OFF;
	n = gs_config_render(&c, text, sizeof(text));
	CHECK(n > 0, "render");
	{
		struct gs_config back;

		CHECK(gs_config_parse(text, (size_t)n, &back, err, sizeof(err)) == 0 && back.nbdfs == 1 &&
		      back.identity == GS_SW_ON && back.msix == GS_SW_OFF &&
		      back.ari == GS_SW_ON && back.vfs == 0, "roundtrip: %s", err);
		CHECK(strstr(text, "\nari=on\n") && strstr(text, "\nmsix=off\n") && !strstr(text, "experimental_vfs") &&
		      !strstr(text, "twin_id"), "the switches are written: %s", text);
		CHECK(!strstr(text, "\nvfs="), "an unset VF count is not written: %s", text);
		c.vfs = 2;
		n = gs_config_render(&c, text, sizeof(text));
		CHECK(n > 0 && gs_config_parse(text, (size_t)n, &back, err, sizeof(err)) == 0 && back.vfs == 2,
		      "vfs roundtrip: %s", err);
	}

	/* type_ada and type_ampere: the type per generation; unset means the generation default. */
	CHECK(gs_config_parse("", 0, &c, err, sizeof(err)) == 0 && c.type_ada == 0 && c.type_ampere == 0 &&
	      gs_config_vgpu_type(&c, GS_ADA) == GS_VGPU_TYPE_ADA && gs_config_vgpu_type(&c, GS_AMPERE) == GS_VGPU_TYPE_AMPERE,
	      "no type keys: the defaults");
	n = snprintf(text, sizeof(text), "bdfs=0000:03:00.0\ntype_ada=914\ntype_ampere=533\n");
	CHECK(gs_config_parse(text, (size_t)n, &c, err, sizeof(err)) == 0 && gs_config_vgpu_type(&c, GS_ADA) == 914 &&
	      gs_config_vgpu_type(&c, GS_AMPERE) == 533, "type keys parsed: %s", err);
	{
		static const char *const bad_type[] = { "type_ada=0\n", "type_ada=0942\n", "type_ada=x\n", "type_ada=1000000\n",
							"type_ada=942\ntype_ada=943\n", "type_ampere=-527\n", NULL };
		struct gs_config back;

		for (int i = 0; bad_type[i]; i++)
			CHECK(gs_config_parse(bad_type[i], strlen(bad_type[i]), &back, err, sizeof(err)) != 0, "rejects %s", bad_type[i]);
		n = snprintf(text, sizeof(text), "bdfs=0000:03:00.0\ntype_ampere=five\n");
		CHECK(gs_config_parse(text, (size_t)n, &back, err, sizeof(err)) != 0 &&
		      strcmp(err, "line 2: type_ampere must be a vGPU type ID from 1 to 999999, as nvidia/creatable_vgpu_types "
				  "lists it") == 0, "bad type_ampere: line-numbered error: %s", err);
		gs_config_defaults(&c);
		n = gs_config_render(&c, text, sizeof(text));
		CHECK(n > 0 && strstr(text, "\n#type_ada=942\n") && strstr(text, "\n#type_ampere=525\n") &&
		      !strstr(text, "\ntype_ada=") && !strstr(text, "\ntype_ampere="),
		      "unset type keys are shown commented out only: %s", text);
		c.type_ada = 914;
		n = gs_config_render(&c, text, sizeof(text));
		CHECK(n > 0 && strstr(text, "\ntype_ada=914\n") && strstr(text, "\n#type_ampere=525\n") &&
		      gs_config_parse(text, (size_t)n, &back, err, sizeof(err)) == 0 && back.type_ada == 914 &&
		      back.type_ampere == 0, "type_ada roundtrip: %s\n%s", err, text);
	}

	/* the shipped example is valid and selects nothing */
	gs_pathf(path, sizeof(path), "%s/config/config.example", srcdir);
	CHECK(gs_config_load(path, &c, err, sizeof(err)) == 0 && c.present && c.nbdfs == 0, "example: %s", err);

	/* missing file = defaults; oversize file rejected */
	gs_pathf(path, sizeof(path), "%s/no-such-config", tmp);
	CHECK(gs_config_load(path, &c, err, sizeof(err)) == 0 && !c.present, "missing file");
	data = malloc(GS_CONFIG_MAX_BYTES + 2);
	memset(data, '#', GS_CONFIG_MAX_BYTES + 1);
	data[GS_CONFIG_MAX_BYTES + 1] = '\0';
	gs_pathf(path, sizeof(path), "%s/big-config", tmp);
	writef(path, "%s", data);
	free(data);
	CHECK(gs_config_load(path, &c, err, sizeof(err)) != 0, "oversize file");
}

static void test_plan(void)
{
	char root[GS_PATH_MAX];
	struct gs_plan plan;
	struct gs_card card;
	static const struct fx_fn pf_bound[] = {
		{ "0000:03:00.0", "2206", "030000", "nvidia" },
		{ NULL, NULL, NULL, NULL },
	};
	static const struct fx_fn shared[] = {
		{ "0000:03:00.0", "2206", "030000", NULL },
		{ "0000:03:01.0", "8086:15f3", "0c0330", NULL },
		{ NULL, NULL, NULL, NULL },
	};
	static const struct fx_fn turing[] = {
		{ "0000:03:00.0", "1e07", "030000", NULL },
		{ NULL, NULL, NULL, NULL },
	};
	static const struct fx_fn other_fn_bound[] = {
		{ "0000:03:00.0", "2206", "030000", NULL },
		{ "0000:03:00.2", "1ada", "0c0330", "xhci_hcd" },
		{ NULL, NULL, NULL, NULL },
	};

	fixture("ok", gpu_with_audio, 0, NULL, root, sizeof(root));
	plan_for(root, &plan, &card);
	CHECK(plan.verdict == GS_PLAN_READY, "ready: %s", plan.reason);
	CHECK(plan.nunbind == 1 && strcmp(plan.unbind[0].str, "0000:03:00.1") == 0, "unbinds only the card's audio");
	CHECK(card.bridge_is_pci && strcmp(card.bridge.str, "0000:00:02.0") == 0, "bridge found");
	CHECK(card.rescan_present && strstr(card.rescan_path, "/pci_bus/0000:03/rescan"), "bridge-scoped rescan");
	CHECK(strcmp(card.bridge_pm, "auto") == 0, "bridge power/control read");
	CHECK(card.entry && card.entry->device == 0x2206, "table entry");

	fixture("bootvga", gpu_with_audio, 1, NULL, root, sizeof(root));
	plan_for(root, &plan, &card);
	CHECK(plan.verdict == GS_PLAN_REFUSE && strstr(plan.reason, "boot display"), "boot VGA refused");

	fixture("bound", pf_bound, 0, NULL, root, sizeof(root));
	plan_for(root, &plan, &card);
	CHECK(plan.verdict == GS_PLAN_REFUSE && strstr(plan.reason, "'nvidia'"), "bound PF refused");

	fixture("shared", shared, 0, NULL, root, sizeof(root));
	plan_for(root, &plan, &card);
	CHECK(plan.verdict == GS_PLAN_REFUSE && strstr(plan.reason, "shared with 0000:03:01.0"), "shared bus refused");

	fixture("turing", turing, 0, NULL, root, sizeof(root));
	plan_for(root, &plan, &card);
	CHECK(plan.verdict == GS_PLAN_REFUSE, "Turing refused");

	fixture("otherfn", other_fn_bound, 0, NULL, root, sizeof(root));
	plan_for(root, &plan, &card);
	CHECK(plan.verdict == GS_PLAN_REFUSE && strstr(plan.reason, "xhci_hcd"), "non-audio bound function refused");

	fixture("activevfs", gpu_with_audio, 0, "2/4", root, sizeof(root));
	plan_for(root, &plan, &card);
	CHECK(plan.verdict == GS_PLAN_REFUSE && strstr(plan.reason, "virtual functions are active"), "active VFs refused");

	{
		struct gs_addr missing;

		gs_addr_parse("0000:05:00.0", &missing);
		gs_card_inspect(root, &missing, &card);
		gs_plan_prepare(&card, NULL, &plan);
		CHECK(plan.verdict == GS_PLAN_REFUSE && strstr(plan.reason, "no such PCI function"), "missing PF refused");
	}
}

static int contains(const char *hay, const char *needle)
{
	return hay && strstr(hay, needle) != NULL;
}

static unsigned int count_of(const char *hay, const char *needle)
{
	unsigned int n = 0;

	for (const char *p = hay; p && (p = strstr(p, needle)); p += strlen(needle))
		n++;
	return n;
}

static void test_cli_readonly(void)
{
	char root[GS_PATH_MAX], cfg[GS_PATH_MAX], *out;
	int rc;

	rc = run_cli(&out, NULL, (const char *const[]){ "--version", NULL });
	CHECK(rc == 0 && contains(out, "geforce-sriov " GS_VERSION), "--version: %d %s", rc, out);
	free(out);

	rc = run_cli(&out, NULL, (const char *const[]){ "devices", NULL });
	CHECK(rc == 0 && contains(out, "10de:2206") &&
	      contains(out, "\nEvery listed card is supported. nvidia-vgpud sees each generation as one card:\n"
			    "  Ampere  presented as RTX A6000 10de:2230, default vGPU type 525\n"
			    "  Ada     presented as RTX 6000 Ada 10de:26b1, default vGPU type 942\n"),
	      "devices names one target per generation: %d\n%s", rc, out);
	CHECK(contains(out, "\nDEVICE    GEN     DIE    NAME\n10de:2203 Ampere  GA102  GeForce RTX 3090 Ti\n") &&
	      contains(out, "\n10de:27b1 Ada     AD104  RTX 4500 Ada Generation\n") &&
	      contains(out, "\n10de:24b0 Ampere  GA104  RTX A4000\n"),
	      "devices rows: DEVICE, GEN, DIE, NAME:\n%s", out);
	CHECK(!contains(out, "10de:1e07") && !contains(out, "10de:2b85"), "devices excludes Turing and Blackwell");
	CHECK(!contains(out, "SAME DIE") && !contains(out, "VGPU TARGET") && !contains(out, "unsupported") &&
	      !contains(out, "Blackwell") && !contains(out, "experimental") && !contains(out, "twin_id") &&
	      !contains(out, "professional"),
	      "devices has no per-row target, die match or status column:\n%s", out);
	{
		unsigned int rows = 0;

		for (const char *row = strstr(out ? out : "", "\n10de:"); row; row = strstr(row + 1, "\n10de:"))
			rows++;
		CHECK(rows == GS_DEVICE_COUNT, "devices prints every row once: %u", rows);
	}
	CHECK(contains(out, "geforce-sriov does nothing for an ID not in this table."), "devices footer:\n%s", out);
	free(out);

	fixture("cli-ok", gpu_with_audio, 0, NULL, root, sizeof(root));
	gs_pathf(cfg, sizeof(cfg), "%s/cli-config", tmp);
	writef(cfg, "bdfs=0000:03:00.0\n");
	rc = run_cli(&out, NULL,
		     (const char *const[]){ "doctor", "--sysfs-root", root, "--config", cfg, NULL });
	CHECK(rc == 0 && contains(out, "prepare would remove and rescan") && contains(out, "HDMI audio"),
	      "doctor ok: %d\n%s", rc, out);
	free(out);

	/* A second, unselected GPU with the same device ID: the hook cannot tell them apart. */
	{
		char p[GS_PATH_MAX], twin[GS_PATH_MAX];

		gs_pathf(twin, sizeof(twin), "%s/devices/pci0000:00/0000:00:03.0/0000:04:00.0", root);
		gs_pathf(p, sizeof(p), "%s/vendor", twin);
		writef(p, "0x10de\n");
		gs_pathf(p, sizeof(p), "%s/device", twin);
		writef(p, "0x2206\n");
		gs_pathf(p, sizeof(p), "%s/class", twin);
		writef(p, "0x030000\n");
		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/0000:04:00.0", root);
		link_to("../../../devices/pci0000:00/0000:00:03.0/0000:04:00.0", p);
		writef(cfg, "bdfs=0000:03:00.0\nidentity=on\n");
		rc = run_cli(&out, NULL,
			     (const char *const[]){ "doctor", "--sysfs-root", root, "--config", cfg, NULL });
		CHECK(rc == 0 && contains(out, "unselected 0000:04:00.0 has the same device ID 10de:2206"),
		      "doctor reports same-ID identity conflict: %d\n%s", rc, out);
		free(out);
		writef(cfg, "bdfs=0000:03:00.0\n");
	}

	fixture("cli-bootvga", gpu_with_audio, 1, NULL, root, sizeof(root));
	rc = run_cli(&out, NULL,
		     (const char *const[]){ "doctor", "--sysfs-root", root, "--device", "0000:03:00.0", NULL });
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "boot display") && !contains(out, "planned by --boot-display-release"),
	      "doctor boot VGA: %d\n%s", rc, out);
	free(out);
	/* --boot-display-release: judged as `install --device B --boot-display-release B` leaves it, the release granted. */
	rc = run_cli(&out, NULL,
		     (const char *const[]){ "doctor", "--sysfs-root", root, "--device", "0000:03:00.0",
					    "--boot-display-release", "0000:03:00.0", NULL });
	CHECK(rc == 0 && contains(out, "[INFO] config: console release of 0000:03:00.0 planned by --boot-display-release\n") &&
	      !contains(out, "is the boot display device (boot_vga=1)") &&
	      contains(out, "[OK  ] 0000:03:00.0: boot display: `prepare --release-console=0000:03:00.0` would "),
	      "doctor boot VGA with --boot-display-release: the console-release plan: %d\n%s", rc, out);
	free(out);
	/* Without --device B: refused as install refuses it, with the same words. */
	for (int k = 0; k < 2; k++) {
		rc = run_cli(&out, NULL,
			     k ? (const char *const[]){ "install", "--boot-display-release", "0000:03:00.0", NULL } :
				 (const char *const[]){ "doctor", "--sysfs-root", root, "--boot-display-release", "0000:03:00.0",
							NULL });
		CHECK(rc == GS_EXIT_USAGE &&
		      contains(out, "geforce-sriov: --boot-display-release 0000:03:00.0 must also be given as --device 0000:03:00.0\n"),
		      "%s --boot-display-release without its --device: usage: %d\n%s", k ? "install" : "doctor", rc, out);
		free(out);
	}
	rc = run_cli(&out, NULL,
		     (const char *const[]){ "status", "--sysfs-root", root, "--device", "0000:03:00.0",
					    "--boot-display-release", "0000:03:00.0", NULL });
	CHECK(rc == GS_EXIT_USAGE && contains(out, "unknown option '--boot-display-release'"),
	      "status takes no --boot-display-release: %d\n%s", rc, out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "--help", NULL });
	CHECK(rc == 0 && contains(out, "  doctor [--device BDF]... [--boot-display-release BDF]...\n"),
	      "help lists doctor --boot-display-release:\n%s", out);
	free(out);

	fixture("cli-prepared", gpu_with_audio, 0, "0/4", root, sizeof(root));
	rc = run_cli(&out, NULL,
		     (const char *const[]){ "status", "--sysfs-root", root, "--config", cfg, NULL });
	CHECK(rc == 0 && contains(out, "sriov_totalvfs=4 sriov_numvfs=0"), "status presented: %d\n%s", rc, out);
	free(out);

	fixture("cli-raw16", gpu_with_audio, 0, "0/16", root, sizeof(root));
	rc = run_cli(&out, NULL,
		     (const char *const[]){ "status", "--sysfs-root", root, "--config", cfg, NULL });
	CHECK(rc == GS_EXIT_FAIL && contains(out, "outside 1..4"), "status flags >4: %d\n%s", rc, out);
	free(out);

	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", "relative", NULL });
	CHECK(rc == GS_EXIT_USAGE, "relative sysfs root rejected: %d", rc);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "status", "--device", "0000:03:00.0x", NULL });
	CHECK(rc == GS_EXIT_USAGE, "bad BDF rejected: %d", rc);
	free(out);
}

static void test_cli_mutation_guards(void)
{
	char *out;
	int rc;

	/* A diagnostic root never reaches a mutating command. */
	rc = run_cli(&out, NULL, (const char *const[]){ "prepare", "--sysfs-root", tmp, NULL });
	CHECK(rc == GS_EXIT_USAGE && contains(out, "doctor and status only"), "prepare --sysfs-root: %d", rc);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "activate", "--sysfs-root=/tmp", NULL });
	CHECK(rc == GS_EXIT_USAGE, "activate --sysfs-root: %d", rc);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "uninstall", "--config", "/tmp/x", NULL });
	CHECK(rc == GS_EXIT_USAGE, "uninstall --config: %d", rc);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "prepare", "--ari=maybe", NULL });
	CHECK(rc == GS_EXIT_USAGE, "switch value: %d", rc);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "prepare", "--vfs=8", NULL });
	CHECK(rc == GS_EXIT_USAGE, "prepare takes no VF count: %d", rc);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "install", "--help", NULL });
	CHECK(rc == 0 && contains(out, "Usage:"), "install --help: %d", rc);
	free(out);
	{
		static const char *const usage_cases[][4] = {
			{ "activate", "--sysfs-root", "/tmp", NULL },
			{ "activate", "--type", "0", NULL },
			{ "activate", "--type", "0942", NULL },
			{ "activate", "--type=1000000", NULL, NULL },
			{ "activate", "--device", "0000:03:00.0", NULL },
			{ "activate", "--device=0000:03:00.0", NULL, NULL },
			{ "status", "--nvidia-source", "/tmp", NULL },
			{ "check", "--dry-run", NULL, NULL },
			{ "check", "--type", "x", NULL },
		};

		for (size_t k = 0; k < sizeof(usage_cases) / sizeof(usage_cases[0]); k++) {
			rc = run_cli(&out, NULL, usage_cases[k]);
			CHECK(rc == GS_EXIT_USAGE, "%s %s: %d", usage_cases[k][0], usage_cases[k][1], rc);
			free(out);
		}
	}
	if (geteuid() != 0) {
		rc = run_cli(&out, NULL, (const char *const[]){ "prepare", NULL });
		CHECK(rc == GS_EXIT_REFUSED && contains(out, "must run as root"), "prepare non-root: %d", rc);
		free(out);
		rc = run_cli(&out, NULL, (const char *const[]){ "uninstall", NULL });
		CHECK(rc == GS_EXIT_REFUSED, "uninstall non-root: %d", rc);
		free(out);
		rc = run_cli(&out, NULL, (const char *const[]){ "install", "--source", srcdir, NULL });
		CHECK(rc == GS_EXIT_REFUSED && contains(out, "set DESTDIR to stage"), "live install non-root: %d\n%s",
		      rc, out);
		free(out);
		rc = run_cli(&out, NULL, (const char *const[]){ "activate", NULL });
		CHECK(rc == GS_EXIT_REFUSED && contains(out, "must run as root"), "activate non-root: %d", rc);
		free(out);
	}
}

static long pos_of(const char *hay, const char *needle);

static void test_install_staging(void)
{
	char stage[GS_PATH_MAX], p[GS_PATH_MAX], sha[65], line[512], *out, *text;
	int rc;

	gs_pathf(stage, sizeof(stage), "%s/stage", tmp);
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--prefix", "/usr", "--source", srcdir,
							 "--device", "0000:03:00.0", "--ari=off", NULL });
	CHECK(rc == 0 && contains(out, "nothing outside it changed"), "staged install: %d\n%s", rc, out);
	/* The vgpud drop-in and the config come last, after every other file. */
	CHECK(pos_of(out, "/etc/systemd/system/" GS_UNIT_ACTIVATE "\n") > 0 &&
	      pos_of(out, "/etc/systemd/system/" GS_UNIT_ACTIVATE "\n") < pos_of(out, "/nvidia-vgpud.service.d/geforce-sriov.conf\n") &&
	      pos_of(out, "/usr/bin/geforce-sriov-driver\n") < pos_of(out, "/nvidia-vgpud.service.d/geforce-sriov.conf\n") &&
	      pos_of(out, "/nvidia-vgpud.service.d/geforce-sriov.conf\n") < pos_of(out, "/etc/geforce-sriov/config\n"),
	      "staged install writes the drop-in and then the config last:\n%s", out);
	free(out);

	gs_pathf(p, sizeof(p), "%s/etc/geforce-sriov/config", stage);
	text = slurp(p);
	CHECK(contains(text, "bdfs=0000:03:00.0\n") && contains(text, "identity=on\n") &&
	      contains(text, "identity_force_host=on\n") && contains(text, "ari=off\n") && !contains(text, "\nvfs="),
	      "config written: %s", text ? text : "(missing)");
	free(text);
	gs_pathf(p, sizeof(p), "%s/etc/systemd/system/geforce-sriov-prepare.service", stage);
	text = slurp(p);
	CHECK(contains(text, "ExecStart=/usr/bin/geforce-sriov prepare") && !contains(text, "@BINDIR@"), "unit");
	free(text);
	gs_pathf(p, sizeof(p), "%s/etc/systemd/system/nvidia-vgpud.service.d/geforce-sriov.conf", stage);
	text = slurp(p);
	CHECK(contains(text, "LD_PRELOAD=/usr/lib/geforce-sriov/libgeforce_sriov.so"), "vgpud drop-in");
	CHECK(!contains(text, "vgpu-mgr.service]"), "no manager hook");
	free(text);
	gs_pathf(p, sizeof(p), "%s/usr/src/geforce-sriov-%s/include/gs_devices.h", stage, GS_VERSION);
	CHECK(gs_exists(p), "DKMS tree has the device header");
	gs_pathf(p, sizeof(p), "%s/usr/src/geforce-sriov-%s/dkms.conf", stage, GS_VERSION);
	CHECK(gs_exists(p), "DKMS tree has dkms.conf");
	gs_pathf(p, sizeof(p), "%s/usr/bin/geforce-sriov-patch", stage);
	CHECK(access(p, X_OK) == 0, "patcher installed");

	/* The manifest records every file with its hash. */
	gs_pathf(p, sizeof(p), "%s/usr/bin/geforce-sriov", stage);
	CHECK(gs_sha256_file(p, sha) == 0, "hash binary");
	snprintf(line, sizeof(line), "f %s /usr/bin/geforce-sriov\n", sha);
	gs_pathf(p, sizeof(p), "%s%s", stage, GS_MANIFEST);
	text = slurp(p);
	CHECK(contains(text, line), "manifest entry for the binary");
	CHECK(contains(text, " /etc/geforce-sriov/config\n"), "manifest entry for the config");
	free(text);

	/* A second staging run without options keeps the selection. */
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--prefix", "/usr", "--source", srcdir, NULL });
	CHECK(rc == 0 && contains(out, "(unchanged)"), "reinstall keeps config: %d\n%s", rc, out);
	free(out);
	/* --vfs records the count; a later run without it keeps it. */
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--prefix", "/usr", "--source", srcdir, "--vfs",
							 "2", NULL });
	CHECK(rc == 0 && contains(out, "next boot"), "install --vfs 2: %d\n%s", rc, out);
	free(out);
	gs_pathf(p, sizeof(p), "%s/etc/geforce-sriov/config", stage);
	text = slurp(p);
	CHECK(contains(text, "\nvfs=2\n") && contains(text, "bdfs=0000:03:00.0\n"), "vfs written: %s",
	      text ? text : "(missing)");
	free(text);
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--prefix", "/usr", "--source", srcdir, NULL });
	CHECK(rc == 0 && contains(out, "(unchanged)"), "reinstall keeps vfs: %d\n%s", rc, out);
	free(out);
	for (size_t k = 0; k < 4; k++) {
		static const char *const badv[] = { "0", "17", "x", "" };

		rc = run_cli(&out, stage, (const char *const[]){ "install", "--source", srcdir, "--vfs", badv[k], NULL });
		CHECK(rc == GS_EXIT_USAGE, "--vfs '%s' refused: %d", badv[k], rc);
		free(out);
	}
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--source", srcdir, "--vfs=2", "--vfs=3", NULL });
	CHECK(rc == GS_EXIT_USAGE, "--vfs twice: %d", rc);
	free(out);


	rc = run_cli(&out, stage, (const char *const[]){ "install", "--prefix", "usr", "--source", srcdir, NULL });
	CHECK(rc == GS_EXIT_USAGE, "relative prefix: %d", rc);
	free(out);
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--prefix", "/usr/../x", "--source", srcdir, NULL });
	CHECK(rc == GS_EXIT_USAGE, "dot-dot prefix: %d", rc);
	free(out);
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--device", "0000:03:00.1", "--source", srcdir, NULL });
	CHECK(rc == GS_EXIT_USAGE, "non-PF device: %d", rc);
	free(out);
}

/* Staging refuses anything that would write outside DESTDIR or take over foreign files. */
static void test_install_guards(void)
{
	char stage[GS_PATH_MAX], outside[GS_PATH_MAX], p[GS_PATH_MAX], *out, *text;
	int rc;

	rc = run_cli(&out, NULL, (const char *const[]){ "install", "--destdir=", "--source", srcdir, NULL });
	CHECK(rc == GS_EXIT_USAGE && contains(out, "--destdir needs a path"), "empty --destdir: %d\n%s", rc, out);
	free(out);
	gs_pathf(stage, sizeof(stage), "%s/guard-stage", tmp);
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--prefix", "/opt/%n", "--source", srcdir, NULL });
	CHECK(rc == GS_EXIT_USAGE, "systemd specifier in prefix: %d", rc);
	free(out);
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--prefix", "/opt/a b", "--source", srcdir, NULL });
	CHECK(rc == GS_EXIT_USAGE, "space in prefix: %d", rc);
	free(out);

	/* A link inside DESTDIR (etc -> outside) is never followed. */
	gs_pathf(outside, sizeof(outside), "%s/outside", tmp);
	gs_mkdir_p(outside, 0755);
	gs_mkdir_p(stage, 0755);
	gs_pathf(p, sizeof(p), "%s/etc", stage);
	link_to(outside, p);
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--prefix", "/usr", "--source", srcdir, NULL });
	CHECK(rc != 0 && contains(out, "symbolic links are not followed"), "link inside DESTDIR: %d\n%s", rc, out);
	free(out);
	gs_pathf(p, sizeof(p), "%s/systemd", outside);
	CHECK(!gs_exists(p), "nothing written through the link");
	gs_pathf(p, sizeof(p), "%s/geforce-sriov", outside);
	CHECK(!gs_exists(p), "no config written through the link");

	/* DESTDIR itself reached through a link. */
	gs_pathf(p, sizeof(p), "%s/stage-link", tmp);
	link_to(outside, p);
	gs_pathf(stage, sizeof(stage), "%s/stage-link/sub", tmp);
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--prefix", "/usr", "--source", srcdir, NULL });
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "is a symbolic link"), "linked DESTDIR: %d\n%s", rc, out);
	free(out);

	/* A config the user wrote is not taken over. */
	gs_pathf(stage, sizeof(stage), "%s/foreign-stage", tmp);
	gs_pathf(p, sizeof(p), "%s/etc/geforce-sriov/config", stage);
	writef(p, "# mine\nari=off\n");
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--prefix", "/usr", "--source", srcdir, "--device",
							 "0000:03:00.0", NULL });
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "was not written by geforce-sriov"), "foreign config: %d\n%s", rc, out);
	free(out);
	text = slurp(p);
	CHECK(text && strcmp(text, "# mine\nari=off\n") == 0, "foreign config untouched");
	free(text);

	/* Nor is any other file at an install target: refused before any write, exit 3, no manifest. */
	gs_pathf(stage, sizeof(stage), "%s/foreign-bin", tmp);
	gs_pathf(p, sizeof(p), "%s/usr/bin/geforce-sriov", stage);
	writef(p, "not ours\n");
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--prefix", "/usr", "--source", srcdir, NULL });
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "was not installed by geforce-sriov"), "foreign binary: %d\n%s", rc, out);
	free(out);
	text = slurp(p);
	CHECK(text && strcmp(text, "not ours\n") == 0, "foreign binary untouched");
	free(text);
	gs_pathf(p, sizeof(p), "%s%s", stage, GS_MANIFEST);
	CHECK(!gs_exists(p), "foreign binary: nothing written, no manifest");

	/* The same refusal for an unrecorded file identical to the one install would write: uninstall would remove it. */
	{
		char *data;
		size_t len;

		gs_pathf(stage, sizeof(stage), "%s/identical-bin", tmp);
		gs_pathf(p, sizeof(p), "%s/geforce-sriov", srcdir);
		if (gs_read_whole(p, &data, &len, 256u << 20))
			die(p);
		gs_pathf(p, sizeof(p), "%s/usr/bin/geforce-sriov", stage);
		writef(p, "%s", "");
		if (gs_write_file_atomic(p, data, len, 0755))
			die(p);
		free(data);
		rc = run_cli(&out, stage, (const char *const[]){ "install", "--prefix", "/usr", "--source", srcdir, NULL });
		CHECK(rc == GS_EXIT_REFUSED && contains(out, "was not installed by geforce-sriov") && !contains(out, "unchanged "),
		      "identical unrecorded binary: refused, exit 3: %d\n%s", rc, out);
		free(out);
		gs_pathf(p, sizeof(p), "%s%s", stage, GS_MANIFEST);
		CHECK(!gs_exists(p), "identical unrecorded binary: no manifest records it");
	}

	/* A foreign vgpud drop-in, written after NVIDIA's files on a live host, refuses before any write: exit 3. */
	gs_pathf(stage, sizeof(stage), "%s/foreign-dropin", tmp);
	gs_pathf(p, sizeof(p), "%s/etc/systemd/system/nvidia-vgpud.service.d/geforce-sriov.conf", stage);
	writef(p, "[Service]\n");
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--prefix", "/usr", "--source", srcdir, "--device",
							 "0000:03:00.0", NULL });
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "was not installed by geforce-sriov") && !contains(out, "installed /"),
	      "foreign drop-in: refused before any write, exit 3: %d\n%s", rc, out);
	free(out);
	gs_pathf(p, sizeof(p), "%s/usr/bin/geforce-sriov", stage);
	CHECK(!gs_exists(p), "foreign drop-in: nothing installed");
	gs_pathf(p, sizeof(p), "%s%s", stage, GS_MANIFEST);
	CHECK(!gs_exists(p), "foreign drop-in: no manifest");

	/* A refusal after the first write exits 1, and the config, written last, is not written. */
	gs_pathf(stage, sizeof(stage), "%s/foreign-unit", tmp);
	gs_pathf(p, sizeof(p), "%s/etc/systemd/system/" GS_UNIT_PREPARE, stage);
	writef(p, "[Unit]\n");
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--prefix", "/usr", "--source", srcdir, "--device",
							 "0000:03:00.0", NULL });
	CHECK(rc == GS_EXIT_FAIL && contains(out, "was not installed by geforce-sriov") &&
	      contains(out, "installed ") && contains(out, "/usr/bin/geforce-sriov\n"),
	      "foreign unit after other writes: exit 1: %d\n%s", rc, out);
	free(out);
	gs_pathf(p, sizeof(p), "%s/etc/geforce-sriov/config", stage);
	CHECK(!gs_exists(p), "foreign unit: the config is not written");
	gs_pathf(p, sizeof(p), "%s%s", stage, GS_MANIFEST);
	text = slurp(p);
	CHECK(contains(text, " /usr/bin/geforce-sriov\n") && !contains(text, "/etc/geforce-sriov/config"),
	      "foreign unit: the manifest lists what was written: %s", text ? text : "(missing)");
	free(text);
}

/* gs_uninstall_file in a child; returns 0 kept or absent, 1 removed, 3 failed, and its output. */
static int run_uninstall_file(const char *path, const char *want, char **out)
{
	int pipefd[2];
	pid_t pid;

	if (pipe(pipefd))
		die("pipe");
	pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0) {
		int rc;

		dup2(pipefd[1], 1);
		dup2(pipefd[1], 2);
		close(pipefd[0]);
		rc = gs_uninstall_file(path, want);
		fflush(stdout);
		fflush(stderr);
		_exit(rc < 0 ? 3 : rc);
	}
	close(pipefd[1]);
	return collect(pipefd[0], pid, out);
}

/*
 * The manifest hash never follows a link: a link put where an installed file was, pointing at a
 * file with identical content, is kept by uninstall and not taken over by install.
 */
static void test_install_link_kept(void)
{
	char stage[GS_PATH_MAX], bin[GS_PATH_MAX], copy[GS_PATH_MAX], sha[65], have[65], *data, *out;
	struct stat st;
	size_t len;
	int rc;

	gs_pathf(stage, sizeof(stage), "%s/link-stage", tmp);
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--prefix", "/usr", "--source", srcdir, "--device",
							 "0000:03:00.0", "--ari=off", NULL });
	CHECK(rc == 0, "staged install for the link case: %d\n%s", rc, out);
	free(out);
	gs_pathf(bin, sizeof(bin), "%s/usr/bin/geforce-sriov", stage);
	gs_pathf(copy, sizeof(copy), "%s/copy-of-geforce-sriov", tmp);
	if (gs_sha256_file(bin, sha))
		die(bin);
	if (gs_read_whole(bin, &data, &len, 256u << 20))
		die(bin);
	if (gs_write_file_atomic(copy, data, len, 0755))
		die(copy);
	free(data);
	link_to(copy, bin);
	CHECK(gs_sha256_file(copy, have) == 0 && strcmp(have, sha) == 0, "the link's target has the installed digest");

	/* Uninstall keeps the link and its target. */
	rc = run_uninstall_file(bin, sha, &out);
	CHECK(rc == 0 && contains(out, "kept ") && lstat(bin, &st) == 0 && S_ISLNK(st.st_mode) && gs_exists(copy),
	      "uninstall keeps a link to identical content: %d\n%s", rc, out);
	free(out);

	/* Install, finding the link where no manifest records a file, refuses rather than taking it over. */
	gs_pathf(stage, sizeof(stage), "%s/link-foreign", tmp);
	gs_pathf(bin, sizeof(bin), "%s/usr/bin/geforce-sriov", stage);
	writef(bin, "%s", ""); /* creates usr/bin; replaced by the link below */
	link_to(copy, bin);
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--prefix", "/usr", "--source", srcdir, "--device",
							 "0000:03:00.0", "--ari=off", NULL });
	CHECK(rc != 0 && contains(out, "was not installed by geforce-sriov"), "install refuses a link to identical content: %d\n%s",
	      rc, out);
	free(out);
	CHECK(lstat(bin, &st) == 0 && S_ISLNK(st.st_mode) && gs_sha256_file(copy, have) == 0 && strcmp(have, sha) == 0,
	      "the link and its target untouched by install");

	/* The control: the regular file with the recorded digest is removed. */
	rc = run_uninstall_file(copy, sha, &out);
	CHECK(rc == 1 && contains(out, "removed ") && !gs_exists(copy), "uninstall removes the unchanged file: %d\n%s", rc,
	      out);
	free(out);
}

static char dkms_log[GS_PATH_MAX], dkms_scratch[GS_PATH_MAX];
static const char *dkms_fail; /* the dkms build that fails: "trial", "build" or NULL */
static int dkms_systemctl_rc;

/*
 * gs_dkms_step's commands, one line each in dkms_log; a scratch tree that exists below dkms_scratch is
 * logged as <tree>. rm runs for real, so the test sees the tree go.
 */
static int dkms_run(const char *const argv[])
{
	char line[2 * GS_PATH_MAX] = "";
	int trial = 0;
	FILE *f;

	if (strcmp(argv[0], "rm") == 0)
		return gs_run(argv);
	for (size_t i = 0; argv[i]; i++) {
		int tree = i > 0 && strcmp(argv[i - 1], "--dkmstree") == 0;
		const char *shown = !tree ? argv[i] :
				    strncmp(argv[i], dkms_scratch, strlen(dkms_scratch)) == 0 && gs_exists(argv[i]) ? "<tree>" :
														   "<bad tree>";

		trial |= tree;
		snprintf(line + strlen(line), sizeof(line) - strlen(line), "%s%s", i ? " " : "", shown);
	}
	f = fopen(dkms_log, "a");
	if (!f)
		die(dkms_log);
	fprintf(f, "%s\n", line);
	fclose(f);
	if (strcmp(argv[0], "systemctl") == 0)
		return dkms_systemctl_rc;
	if (dkms_fail && strcmp(argv[1], "build") == 0 && trial == (strcmp(dkms_fail, "trial") == 0))
		return 10;
	return 0;
}

/* gs_dkms_step in a child with dkms_run; its exit status, output and command log. */
static int run_dkms_step(int registered, char **out, char **log)
{
	int pipefd[2], rc;
	pid_t pid;

	unlink(dkms_log);
	if (pipe(pipefd))
		die("pipe");
	pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0) {
		dup2(pipefd[1], 1);
		dup2(pipefd[1], 2);
		close(pipefd[0]);
		rc = gs_dkms_step(dkms_run, registered, dkms_scratch);
		fflush(stdout);
		fflush(stderr);
		_exit(rc);
	}
	close(pipefd[1]);
	rc = collect(pipefd[0], pid, out);
	*log = slurp(dkms_log);
	return rc;
}

/*
 * install's DKMS step. A reinstall of the same version builds the new tree in a scratch DKMS tree first:
 * when that fails, nothing is removed. Only after it succeeds is the registered module removed; a failure
 * after that disables the boot units and names what was removed and the way back.
 */
static void test_dkms_step(void)
{
#define DKMS_MV "-m " GS_DKMS_NAME " -v " GS_VERSION
	static const char real[] = "dkms add " DKMS_MV "\ndkms build " DKMS_MV "\ndkms install " DKMS_MV "\n";
	static const char trial[] = "dkms add " DKMS_MV " --dkmstree <tree>\ndkms build " DKMS_MV " --dkmstree <tree>\n";
	static const char removal[] = "dkms remove " GS_DKMS_NAME "/" GS_VERSION " --all\n";
	static const char disable[] = "systemctl disable " GS_UNIT_PREPARE " " GS_UNIT_ACTIVATE "\n";
	char want[1024], *out, *log;
	struct dirent *de;
	unsigned int left = 0;
	DIR *d;
	int rc;

	gs_pathf(dkms_scratch, sizeof(dkms_scratch), "%s/dkms-scratch", tmp);
	gs_pathf(dkms_log, sizeof(dkms_log), "%s/dkms.log", tmp);
	gs_mkdir_p(dkms_scratch, 0755);
	dkms_fail = NULL;
	dkms_systemctl_rc = 0;

	rc = run_dkms_step(0, &out, &log);
	CHECK(rc == 0 && log && strcmp(log, real) == 0, "first install: add, build, install, no trial: %d\n%s\n%s", rc, out,
	      log ? log : "");
	free(out);
	free(log);

	rc = run_dkms_step(1, &out, &log);
	snprintf(want, sizeof(want), "%s%s%s", trial, removal, real);
	CHECK(rc == 0 && log && strcmp(log, want) == 0,
	      "reinstall: the trial build in a scratch tree, then remove, add, build, install: %d\n%s\n%s", rc, out,
	      log ? log : "");
	free(out);
	free(log);
	d = opendir(dkms_scratch);
	while (d && (de = readdir(d)))
		left += de->d_name[0] != '.';
	if (d)
		closedir(d);
	CHECK(d && !left, "reinstall: the scratch tree is removed");

	dkms_fail = "trial";
	rc = run_dkms_step(1, &out, &log);
	CHECK(rc == GS_EXIT_FAIL && log && strcmp(log, trial) == 0 &&
	      contains(out, "geforce-sriov: " GS_DKMS_NAME "/" GS_VERSION " does not build for the running kernel (DKMS's "
			    "output is above). Nothing\nwas removed: the module installed earlier stays registered and loads "
			    "at the next boot, and the boot\nunits are unchanged. Fix the build, then run `geforce-sriov "
			    "install` again.\n"),
	      "reinstall, the trial fails: nothing removed, the units untouched: %d\n%s\n%s", rc, out, log ? log : "");
	free(out);
	free(log);

	dkms_fail = "build";
	rc = run_dkms_step(1, &out, &log);
	snprintf(want, sizeof(want), "%s%sdkms add " DKMS_MV "\ndkms build " DKMS_MV "\n%s", trial, removal, disable);
	CHECK(rc == GS_EXIT_FAIL && log && strcmp(log, want) == 0 &&
	      contains(out, "and `dkms remove " GS_DKMS_NAME "/" GS_VERSION " --all` had already removed the module installed "
			    "earlier.\nSo that the next boot does not try to load it, " GS_UNIT_PREPARE " and " GS_UNIT_ACTIVATE
			    " are disabled.\nFix the cause in DKMS's output above, then run `geforce-sriov install` again; it "
			    "enables them when GPUs are selected.\n"),
	      "reinstall, the real build fails after the removal: units disabled, the way back: %d\n%s\n%s", rc, out,
	      log ? log : "");
	free(out);
	free(log);
	dkms_systemctl_rc = 1;
	rc = run_dkms_step(1, &out, &log);
	CHECK(rc == GS_EXIT_FAIL && contains(log, disable) &&
	      contains(out, "Disabling " GS_UNIT_PREPARE " and " GS_UNIT_ACTIVATE " failed: the next boot's prepare fails to "
			    "load the module unless you run `systemctl disable " GS_UNIT_PREPARE " " GS_UNIT_ACTIVATE "`.\n"
			    "Fix the cause in DKMS's output above, then run `geforce-sriov install` again.\n"),
	      "reinstall, the real build fails and so does the disable: said so: %d\n%s", rc, out);
	free(out);
	free(log);
	dkms_systemctl_rc = 0;

	rc = run_dkms_step(0, &out, &log);
	CHECK(rc == GS_EXIT_FAIL && !contains(log, "systemctl") && !contains(log, "--dkmstree") &&
	      contains(out, "DKMS could not add/build/install " GS_DKMS_NAME "/" GS_VERSION " for the running kernel; files are "
			    "installed, `geforce-sriov uninstall` removes them\n"),
	      "first install, the build fails: nothing to disable: %d\n%s\n%s", rc, out, log ? log : "");
	free(out);
	free(log);
	dkms_fail = NULL;
#undef DKMS_MV
}

/* The "N failure(s), M warning(s)" tally that ends doctor's output; -1 when absent. */
static void tally_of(const char *out, int *fail, int *warn)
{
	const char *p = out ? strstr(out, " failure(s), ") : NULL;

	*fail = *warn = -1;
	if (!p)
		return;
	while (p > out && p[-1] >= '0' && p[-1] <= '9')
		p--;
	if (sscanf(p, "%d failure(s), %d warning(s)", fail, warn) != 2)
		*fail = *warn = -1;
}

/* A loaded geforce_sriov with a complete livepatch transition, acting on bdfs. */
static void fake_module(const char *root, const char *bdfs)
{
	static const char *const param[][2] = {
		{ "ari", "Y" }, { "msix", "Y" },
	};
	char p[GS_PATH_MAX];

	for (size_t i = 0; i < sizeof(param) / sizeof(param[0]); i++) {
		gs_pathf(p, sizeof(p), "%s/module/%s/parameters/%s", root, GS_MODULE_NAME, param[i][0]);
		writef(p, "%s\n", param[i][1]);
	}
	gs_pathf(p, sizeof(p), "%s/module/%s/parameters/bdfs", root, GS_MODULE_NAME);
	writef(p, "%s\n", bdfs);
	gs_pathf(p, sizeof(p), "%s/kernel/livepatch/%s/enabled", root, GS_MODULE_NAME);
	writef(p, "1\n");
	gs_pathf(p, sizeof(p), "%s/kernel/livepatch/%s/transition", root, GS_MODULE_NAME);
	writef(p, "0\n");
}

/* <pf>/resource of the default fixture card: BAR0 (line 0) and VF BAR 0 (line 7) assigned or not. */
static void write_resource(const char *root, int bar0_assigned, int vf_assigned)
{
	char p[GS_PATH_MAX], text[1024] = "";
	static const char *const zero = "0x0000000000000000 0x0000000000000000 0x0000000000000000\n";

	strcat(text, bar0_assigned ? "0x00000000f0000000 0x00000000f0ffffff 0x0000000000040200\n" : zero);
	for (int i = 1; i <= 12; i++)
		strcat(text, i == 7 && vf_assigned ? "0x0000004000000000 0x00000040007fffff 0x000000000014220c\n" : zero);
	gs_pathf(p, sizeof(p), "%s/devices/%s/%s/resource", root, topo_default.bridge_path, topo_default.pf);
	writef(p, "%s", text);
}

static const struct fx_fn turing_gpu[] = {
	{ "0000:03:00.0", "1e07", "030000", NULL },
	{ NULL, NULL, NULL, NULL },
};

/* AD103: another die than the target's, 1 hardware VF, presented as 4. */
static const struct fx_fn ada_cross_die_gpu[] = {
	{ "0000:03:00.0", "2702", "030000", NULL },
	{ NULL, NULL, NULL, NULL },
};

/* Workstation rows: RTX 4500 Ada (TotalVFs 1) and RTX 2000 Ada (TotalVFs 16). */
static const struct fx_fn rtx4500_ada_gpu[] = {
	{ "0000:03:00.0", "27b1", "030000", NULL },
	{ NULL, NULL, NULL, NULL },
};
static const struct fx_fn rtx2000_ada_gpu[] = {
	{ "0000:03:00.0", "28b0", "030000", NULL },
	{ NULL, NULL, NULL, NULL },
};

/* GB202 (RTX 5090): Blackwell, not in the device table. */
static const struct fx_fn gb202_gpu[] = {
	{ "0000:03:00.0", "2b85", "030000", NULL },
	{ NULL, NULL, NULL, NULL },
};

static const struct fx_fn pf_bound_gpu[] = {
	{ "0000:03:00.0", "2206", "030000", "nvidia" },
	{ NULL, NULL, NULL, NULL },
};

static void test_doctor_scope(void)
{
	char root[GS_PATH_MAX], cfg[GS_PATH_MAX], p[GS_PATH_MAX], *out;
	const char *note;
	int rc, f1, w1, f2, w2;
	static const struct fx_topo twin_topo = { "pci0000:00/0000:00:03.0", "0000:04:00.0" };
	static const struct fx_fn twin_bound[] = {
		{ "0000:04:00.0", "2206", "030000", "nvidia" },
		{ NULL, NULL, NULL, NULL },
	};
	static const struct fx_fn amp_04[] = {
		{ "0000:04:00.0", "2206", "030000", NULL },
		{ NULL, NULL, NULL, NULL },
	};

	gs_pathf(cfg, sizeof(cfg), "%s/ux-config", tmp);

	/* An ID outside the table is refused; table notes are counted warnings, not plain text. */
	fixture("ux-turing", turing_gpu, 0, NULL, root, sizeof(root));
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--device", "0000:03:00.0", NULL });
	tally_of(out, &f1, &w1);
	CHECK(rc == GS_EXIT_REFUSED && f1 >= 1, "unsupported ID refused: %d\n%s", rc, out);
	free(out);

	fixture("ux-ada", ada_cross_die_gpu, 0, NULL, root, sizeof(root));
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--device", "0000:03:00.0", NULL });
	tally_of(out, &f1, &w1);
	CHECK(rc == 0 && f1 == 0 && w1 == 0 && !contains(out, "another die") && !contains(out, "cross-die") &&
	      !contains(out, "same die") && !contains(out, "unsupported") &&
	      contains(out, "  GeForce RTX 4080 SUPER (AD103, Ada) -> vGPU target 10de:26b1 RTX 6000 Ada\n") &&
	      contains(out, "hardware default 1, geforce-sriov presents 4 (default)") && !contains(out, "selection mixes"),
	      "Ada on another die: ready, no warning, the generation's target, 4 VFs by default: %d\n%s", rc, out);
	/* Host prerequisites (dkms, pciutils) are live-system checks; a fixture never fails on them. */
	CHECK(!contains(out, "dkms") && !contains(out, "lspci"), "fixture doctor runs no host prerequisite check");
	free(out);
	/* A config that still has experimental_vfs or twin_id is refused, the key named. */
	writef(cfg, "bdfs=0000:03:00.0\nexperimental_vfs=off\n");
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--config", cfg, NULL });
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "line 2: unknown key 'experimental_vfs'; delete this line"),
	      "doctor: experimental_vfs in the config: %d\n%s", rc, out);
	free(out);
	writef(cfg, "twin_id=off\nbdfs=0000:03:00.0\n");
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--config", cfg, NULL });
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "line 1: unknown key 'twin_id'; delete this line"),
	      "doctor: twin_id in the config: %d\n%s", rc, out);
	free(out);

	/* The workstation rows: the Ada target, 4 VFs from a hardware 1 or 16. */
	fixture("ux-rtx4500", rtx4500_ada_gpu, 0, NULL, root, sizeof(root));
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--device", "0000:03:00.0", NULL });
	tally_of(out, &f1, &w1);
	CHECK(rc == 0 && f1 == 0 && w1 == 0 &&
	      contains(out, "  RTX 4500 Ada Generation (AD104, Ada) -> vGPU target 10de:26b1 RTX 6000 Ada\n") &&
	      contains(out, "hardware default 1, geforce-sriov presents 4 (default)"),
	      "RTX 4500 Ada: ready, 4 VFs: %d\n%s", rc, out);
	free(out);
	fixture("ux-rtx2000", rtx2000_ada_gpu, 0, NULL, root, sizeof(root));
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--device", "0000:03:00.0", NULL });
	tally_of(out, &f1, &w1);
	CHECK(rc == 0 && f1 == 0 && w1 == 0 &&
	      contains(out, "  RTX 2000 / 2000E Ada Generation (AD107, Ada) -> vGPU target 10de:26b1 RTX 6000 Ada\n") &&
	      contains(out, "hardware default 16, geforce-sriov presents 4 (default)"),
	      "RTX 2000 Ada: ready, 4 VFs: %d\n%s", rc, out);
	free(out);

	/* Ada with an Ampere card: one warning about the 4-VF host module, never a refusal. */
	fixture("ux-ada-mixed", ada_cross_die_gpu, 0, NULL, root, sizeof(root));
	make_card(root, &twin_topo, amp_04, 0, NULL);
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--device", "0000:03:00.0",
							 "--device", "0000:04:00.0", NULL });
	tally_of(out, &f2, &w2);
	note = out ? strstr(out, "selection mixes") : NULL;
	CHECK(rc == 0 && f2 == 0 && note && !strstr(note + 1, "selection mixes") &&
	      contains(out, "[WARN] selection mixes Ada (0000:03:00.0) with Ampere (0000:04:00.0): one host module drives "
			    "both generations, with 4 VFs per card, and one vGPU plugin copy serves both"),
	      "Ada + Ampere: one mixed-generation warning, exit 0: %d\n%s", rc, out);
	CHECK(contains(out, "and the patched plugin copy of libnvidia-vgpu.so, SHA-256 "
			    "aa4fdeb5288b5e56e7d4972466813d78e10cfb428fcf62fb53b84fec718cf07c (docs/driver-patch.md#revisions)") &&
	      !contains(out, "7ddfad458bbfdb0c4d6902a9e045bd15c84f63aea19314f3d3a60967224b73c6"),
	      "Ada note: names the patched plugin copy's digest only:\n%s", out);
	free(out);

	/* A Blackwell card is not in the device table: refused with that plain reason, alone or next to Ada. */
	fixture("ux-gb202", gb202_gpu, 0, NULL, root, sizeof(root));
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--device", "0000:03:00.0", NULL });
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "0000:03:00.0 (10de:2b85) is not in the device table") &&
	      !contains(out, "Blackwell"),
	      "GB202 alone: not in the device table: %d\n%s", rc, out);
	free(out);
	fixture("ux-ada-blackwell", gpu_with_audio, 0, NULL, root, sizeof(root));
	{
		/* The GB202 at the second card's address, next to the card of this fixture. */
		static const struct fx_topo disp = { "pci0000:00/0000:00:03.0", "0000:04:00.0" };
		static const struct fx_fn gb202_04[] = {
			{ "0000:04:00.0", "2b85", "030000", NULL },
			{ NULL, NULL, NULL, NULL },
		};

		make_card(root, &disp, gb202_04, 0, NULL);
	}
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--device", "0000:03:00.0",
							"--device", "0000:04:00.0", NULL });
	tally_of(out, &f1, &w1);
	CHECK(rc == GS_EXIT_REFUSED && f1 == 1 && contains(out, "0000:04:00.0 (10de:2b85) is not in the device table") &&
	      !contains(out, "Blackwell"),
	      "GB202 next to a listed card: one FAIL, not in the device table: %d\n%s", rc, out);
	free(out);

	/* Before install: --device alone is judged with the identity settings `install --device` writes. */
	fixture("ux-twin", gpu_with_audio, 0, NULL, root, sizeof(root));
	make_card(root, &twin_topo, twin_bound, 0, NULL);
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--device", "0000:03:00.0", NULL });
	tally_of(out, &f1, &w1);
	CHECK(rc == 0 && contains(out, "unselected 0000:04:00.0 has the same device ID"),
	      "planned identity reports the same-ID fallback risk: %d\n%s", rc, out);
	free(out);
	/* A config that already holds this selection is authoritative: identity=off has no identity scope. */
	writef(cfg, "bdfs=0000:03:00.0\nidentity=off\n");
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--config", cfg, "--device",
							 "0000:03:00.0", NULL });
	tally_of(out, &f2, &w2);
	CHECK(rc == 0 && w2 < w1 && !contains(out, "unselected 0000:04:00.0 has the same device ID"),
	      "config identity=off wins for its own selection: %d %d/%d\n%s", rc, w1, w2, out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--device", "0000:03:00.0",
							 "--device", "0000:04:00.0", NULL });
	CHECK(!contains(out, "unselected 0000:04:00.0"), "both twins selected: neither is unselected\n%s", out);
	free(out);
	/* An unreadable PCI entry: the hook reads it too, so doctor cannot confirm the mapping. */
	gs_pathf(p, sizeof(p), "%s/devices/pci0000:00/0000:00:1f.0/vendor", root);
	writef(p, "0x8086\n");
	gs_pathf(p, sizeof(p), "%s/bus/pci/devices/0000:00:1f.0", root);
	link_to("../../../devices/pci0000:00/0000:00:1f.0", p);
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--device", "0000:03:00.0", NULL });
	tally_of(out, &f2, &w2);
	CHECK(rc == 0 && w2 >= 1 && !contains(out, "matched by bus address") && contains(out, "cannot confirm"),
	      "unreadable entry: identity unconfirmed, not OK: %d\n%s", rc, out);
	free(out);

	/* A card this module prepared is fine; the same capability from another kernel change is refused. */
	fixture("ux-prepared", gpu_with_audio, 0, "0/4", root, sizeof(root));
	fake_module(root, "0000:03:00.0");
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--device", "0000:03:00.0", NULL });
	tally_of(out, &f1, &w1);
	CHECK(rc == 0 && f1 == 0 && !contains(out, "another kernel change"), "prepared card has no stale refusal: %d\n%s",
	      rc, out);
	free(out);
	fake_module(root, "0000:81:00.0");
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--device", "0000:03:00.0", NULL });
	tally_of(out, &f2, &w2);
	CHECK(rc == GS_EXIT_REFUSED && f2 >= 1, "SR-IOV from another kernel change stays refused: %d\n%s", rc, out);
	free(out);

	/* IOMMU grouping: own functions and the bridge alone are informational; an unrelated device warns. */
	fixture("ux-iommu", gpu_with_audio, 0, NULL, root, sizeof(root));
	gs_pathf(p, sizeof(p), "%s/devices/%s/%s/iommu_group", root, topo_default.bridge_path, topo_default.pf);
	link_to("../../../../kernel/iommu_groups/7", p);
	gs_pathf(p, sizeof(p), "%s/kernel/iommu_groups/7/devices/0000:03:00.0", root);
	writef(p, "%s", "");
	gs_pathf(p, sizeof(p), "%s/kernel/iommu_groups/7/devices/0000:03:00.1", root);
	writef(p, "%s", "");
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--device", "0000:03:00.0", NULL });
	tally_of(out, &f1, &w1);
	free(out);
	gs_pathf(p, sizeof(p), "%s/kernel/iommu_groups/7/devices/0000:00:02.0", root);
	writef(p, "%s", "");
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--device", "0000:03:00.0", NULL });
	tally_of(out, &f2, &w2);
	CHECK(rc == 0 && w1 >= 0 && w2 == w1, "bridge alone in the PF's IOMMU group does not warn: %d->%d\n%s", w1, w2, out);
	free(out);
	gs_pathf(p, sizeof(p), "%s/kernel/iommu_groups/7/devices/0000:00:14.0", root);
	writef(p, "%s", "");
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--device", "0000:03:00.0", NULL });
	tally_of(out, &f2, &w2);
	CHECK(rc == 0 && w2 == w1 + 1, "unrelated device in the PF's IOMMU group warns: %d->%d\n%s", w1, w2, out);
	free(out);

	/* After enable: a VF in the PF's own IOMMU group warns; a VF in its own group does not. */
	{
		static const struct fx_fn with_vfs[] = {
			{ "0000:03:00.0", "2206", "030000", NULL },
			{ "0000:03:00.1", "1aef", "040300", "snd_hda_intel" },
			{ "0000:03:00.4", "2206", "030200", "nvidia" },
			{ "0000:03:00.5", "2206", "030200", "nvidia" },
			{ NULL, NULL, NULL, NULL },
		};
		char pfdir[GS_PATH_MAX];

		fixture("ux-vfgroup", with_vfs, 0, "2/4", root, sizeof(root));
		fake_module(root, "0000:03:00.0");
		gs_pathf(pfdir, sizeof(pfdir), "%s/devices/%s/%s", root, topo_default.bridge_path, topo_default.pf);
		gs_pathf(p, sizeof(p), "%s/iommu_group", pfdir);
		link_to("../../../../kernel/iommu_groups/7", p);
		for (int k = 0; k < 2; k++) {
			gs_pathf(p, sizeof(p), "%s/virtfn%d", pfdir, k);
			link_to(k ? "../0000:03:00.5" : "../0000:03:00.4", p);
			gs_pathf(p, sizeof(p), "%s/devices/%s/0000:03:00.%d/physfn", root, topo_default.bridge_path, 4 + k);
			link_to("../0000:03:00.0", p);
			gs_pathf(p, sizeof(p), "%s/devices/%s/0000:03:00.%d/iommu_group", root, topo_default.bridge_path, 4 + k);
			link_to(k ? "../../../../kernel/iommu_groups/9" : "../../../../kernel/iommu_groups/8", p);
		}
		rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--device", "0000:03:00.0", NULL });
		tally_of(out, &f1, &w1);
		CHECK(rc == 0 && f1 == 0, "VFs in their own groups: %d\n%s", rc, out);
		free(out);
		gs_pathf(p, sizeof(p), "%s/devices/%s/0000:03:00.5/iommu_group", root, topo_default.bridge_path);
		link_to("../../../../kernel/iommu_groups/7", p);
		rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--device", "0000:03:00.0", NULL });
		tally_of(out, &f2, &w2);
		CHECK(rc == 0 && w1 >= 0 && w2 == w1 + 1, "a VF sharing the PF's IOMMU group warns: %d->%d\n%s", w1, w2, out);
		free(out);
	}
}

static int run_diag(int (*fn)(const struct gs_diag_opts *), const struct gs_diag_opts *o, char **out);

static void test_status_guidance(void)
{
	char root[GS_PATH_MAX], cfg[GS_PATH_MAX], *out;
	struct gs_card card;
	struct gs_plan plan;
	struct gs_addr pf;
	int rc, f1, w1;

	gs_pathf(cfg, sizeof(cfg), "%s/ux-status-config", tmp);
	writef(cfg, "bdfs=0000:03:00.0\n");
	fixture("ux-status", gpu_with_audio, 0, NULL, root, sizeof(root));
	/* Not prepared yet is the normal state before a prepared boot: exit 1 with the next step. */
	rc = run_cli(&out, NULL, (const char *const[]){ "status", "--sysfs-root", root, "--config", cfg, NULL });
	CHECK(rc == GS_EXIT_FAIL && contains(out, "geforce-sriov doctor") &&
	      contains(out, "Next: run `geforce-sriov doctor`, then power the machine off and on\n") && !contains(out, "reboot"),
	      "selected, unprepared: next step given: %d\n%s", rc, out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "status", "--sysfs-root", root, "--device", "0000:03:00.0", NULL });
	CHECK(rc == GS_EXIT_FAIL && contains(out, "install --device 0000:03:00.0") &&
	      contains(out, "(one --device per GPU), then power the machine off and on\n") && !contains(out, "reboot"),
	      "unselected, unprepared: install guidance: %d\n%s", rc, out);
	free(out);

	/* A presented capability without VF BAR space is not ready for enable. */
	fixture("ux-bars", gpu_with_audio, 0, "0/4", root, sizeof(root));
	write_resource(root, 1, 0);
	gs_addr_parse(topo_default.pf, &pf);
	gs_card_inspect(root, &pf, &card);
	CHECK(card.bar0_assigned == 1 && card.vf_bars_assigned == 0, "resource read: %d %d", card.bar0_assigned,
	      card.vf_bars_assigned);
	rc = run_cli(&out, NULL, (const char *const[]){ "status", "--sysfs-root", root, "--config", cfg, NULL });
	CHECK(rc == GS_EXIT_FAIL, "unassigned VF BARs: not ready: %d\n%s", rc, out);
	free(out);
	write_resource(root, 1, 1);
	gs_card_inspect(root, &pf, &card);
	CHECK(card.vf_bars_assigned == 1, "VF BAR assigned");
	rc = run_cli(&out, NULL, (const char *const[]){ "status", "--sysfs-root", root, "--config", cfg, NULL });
	CHECK(rc == 0, "assigned VF BARs: ready: %d\n%s", rc, out);
	free(out);
	/*
	 * activate's marker in this boot's run directory: doctor warns and status names it with its next
	 * step, exit 1. Without the marker neither says anything about it.
	 */
	{
		char run[GS_PATH_MAX], marker[GS_PATH_MAX], want[2 * GS_PATH_MAX];
		struct gs_diag_opts d;
		int f0, w0;

		gs_pathf(run, sizeof(run), "%s/ux-run", tmp);
		gs_pathf(marker, sizeof(marker), "%s/" GS_ACTIVATING_NAME, run);
		memset(&d, 0, sizeof(d));
		d.sysroot = root;
		d.config_path = cfg;
		d.run_dir = run;
		rc = run_diag(gs_cmd_status, &d, &out);
		CHECK(rc == 0 && !contains(out, "activate: ") && !contains(out, "next: "),
		      "no activate marker: status ready, nothing said: %d\n%s", rc, out);
		free(out);
		run_diag(gs_cmd_doctor, &d, &out);
		tally_of(out, &f0, &w0);
		CHECK(!contains(out, " exists: geforce-sriov activate"), "no activate marker: doctor says nothing\n%s", out);
		free(out);
		writef(marker, "boot_id=x\n");
		rc = run_diag(gs_cmd_status, &d, &out);
		snprintf(want, sizeof(want), "activate: %s exists (activate is running or did not finish)\n"
			 "next: stop every VM that uses a vGPU, then run `geforce-sriov activate`\n", marker);
		CHECK(rc == GS_EXIT_FAIL && contains(out, want), "activate marker: status names it, exit 1: %d\n%s", rc, out);
		free(out);
		run_diag(gs_cmd_doctor, &d, &out);
		tally_of(out, &f1, &w1);
		snprintf(want, sizeof(want), "[WARN] %s exists: geforce-sriov activate is running or did not finish; check "
			 "reports NOT READY until activate succeeds\n", marker);
		CHECK(contains(out, want) && w1 == w0 + 1 && f1 == f0, "activate marker: doctor WARN: %d/%d\n%s", w1, w0, out);
		free(out);
		unlink(marker);

		/* No config file: the selection comes from --device when one is given. */
		gs_pathf(run, sizeof(run), "%s/ux-absent-config", tmp);
		rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--config", run, "--device",
								"0000:03:00.0", NULL });
		snprintf(want, sizeof(want), "[INFO] config: %s absent; the selected devices come from --device\n", run);
		CHECK(contains(out, want) && !contains(out, "no device is selected"),
		      "no config, --device: the selection comes from --device: %d\n%s", rc, out);
		free(out);
		rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--config", run, NULL });
		snprintf(want, sizeof(want), "[INFO] config: %s absent; defaults apply and no device is selected\n", run);
		CHECK(contains(out, want), "no config, no --device: defaults apply: %d\n%s", rc, out);
		free(out);
	}
	/* A presented card without BAR0 is unusable: not ready. */
	write_resource(root, 0, 1);
	rc = run_cli(&out, NULL, (const char *const[]){ "status", "--sysfs-root", root, "--config", cfg, NULL });
	CHECK(rc == GS_EXIT_FAIL, "no BAR0: not ready: %d\n%s", rc, out);
	free(out);

	/* prepare never rescans a card whose BAR0 is unassigned now ... */
	fixture("ux-nobar0", gpu_with_audio, 0, NULL, root, sizeof(root));
	write_resource(root, 1, 0);
	gs_card_inspect(root, &pf, &card);
	gs_plan_prepare(&card, NULL, &plan);
	CHECK(plan.verdict == GS_PLAN_READY, "BAR0 assigned: ready: %s", plan.reason);
	write_resource(root, 0, 0);
	gs_card_inspect(root, &pf, &card);
	gs_plan_prepare(&card, NULL, &plan);
	CHECK(plan.verdict == GS_PLAN_REFUSE && card.bar0_assigned == 0, "BAR0 unassigned: refused");
	/* ... but a bound card is judged as the next boot sees it, where firmware assigns BAR0 again. */
	fixture("ux-nobar0-bound", pf_bound_gpu, 0, NULL, root, sizeof(root));
	write_resource(root, 0, 0);
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--device", "0000:03:00.0", NULL });
	CHECK(rc == 0, "bound card without BAR0 now: next boot not refused: %d\n%s", rc, out);
	free(out);
}

/* Several GPUs, several PCI domains, a PCIe switch, and a root-bus card. */
static void test_topologies(void)
{
	char root[GS_PATH_MAX], *out;
	struct gs_card card;
	struct gs_plan plan;
	struct gs_addr pf;
	int rc, f, w;
	static const struct fx_topo dom1 = { "pci0001:80/0001:80:01.0", "0001:03:00.0" };
	static const struct fx_fn dom1_gpu[] = {
		{ "0001:03:00.0", "2206", "030000", NULL },
		{ "0001:03:00.1", "1aef", "040300", "snd_hda_intel" },
		{ NULL, NULL, NULL, NULL },
	};
	static const struct fx_topo dsp_a = { "pci0000:00/0000:00:01.0/0000:01:00.0/0000:02:00.0", "0000:03:00.0" };
	static const struct fx_topo dsp_b = { "pci0000:00/0000:00:01.0/0000:01:00.0/0000:02:01.0", "0000:04:00.0" };
	static const struct fx_fn gpu_b[] = {
		{ "0000:04:00.0", "2206", "030000", NULL },
		{ NULL, NULL, NULL, NULL },
	};
	static const struct fx_topo rootbus = { "pci0000:00", "0000:00:05.0" };
	static const struct fx_fn gpu_rootbus[] = {
		{ "0000:00:05.0", "2206", "030000", NULL },
		{ NULL, NULL, NULL, NULL },
	};
	static const struct fx_topo vmd = { "pci10000:e0/10000:e0:01.0", "10000:e1:00.0" };
	static const struct fx_fn gpu_vmd[] = {
		{ "10000:e1:00.0", "2206", "030000", NULL },
		{ NULL, NULL, NULL, NULL },
	};

	/* The same bus:slot in domains 0000 and 0001 are two separate cards on two separate buses. */
	snprintf(root, sizeof(root), "%s/ux-domains", tmp);
	make_card(root, &topo_default, gpu_with_audio, 0, NULL);
	make_card(root, &dom1, dom1_gpu, 0, NULL);
	gs_addr_parse(dom1.pf, &pf);
	gs_card_inspect(root, &pf, &card);
	gs_plan_prepare(&card, NULL, &plan);
	CHECK(plan.verdict == GS_PLAN_READY && strcmp(card.bridge.str, "0001:80:01.0") == 0 && card.nothers == 0 &&
	      card.nfns == 1 && contains(card.rescan_path, "/0001:80:01.0/pci_bus/0001:03/rescan"),
	      "domain 0001 card: own bridge, bus and rescan: %s", plan.reason);
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--device", "0000:03:00.0",
							 "--device", "0001:03:00.0", NULL });
	tally_of(out, &f, &w);
	CHECK(rc == 0 && f == 0 && !contains(out, "unselected"), "two domains, both selected: %d\n%s", rc, out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--device", "0000:03:00.0", NULL });
	CHECK(rc == 0 && contains(out, "unselected 0001:03:00.0 has the same device ID"),
	      "the same-ID twin in another domain is reported: %d\n%s", rc, out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "status", "--sysfs-root", root, "--device", "0000:03:00.0",
							 "--device", "0001:03:00.0", NULL });
	CHECK(rc == GS_EXIT_FAIL && contains(out, "install --device 0000:03:00.0") &&
	      contains(out, "install --device 0001:03:00.0"), "status covers both domains: %d\n%s", rc, out);
	free(out);

	/* Two GPUs behind two downstream ports of one switch: each port is its card's bridge. */
	snprintf(root, sizeof(root), "%s/ux-switch", tmp);
	make_card(root, &dsp_a, gpu_with_audio, 0, NULL);
	make_card(root, &dsp_b, gpu_b, 0, NULL);
	gs_addr_parse(dsp_a.pf, &pf);
	gs_card_inspect(root, &pf, &card);
	gs_plan_prepare(&card, NULL, &plan);
	CHECK(plan.verdict == GS_PLAN_READY && strcmp(card.bridge.str, "0000:02:00.0") == 0 &&
	      contains(card.rescan_path, "/0000:02:00.0/pci_bus/0000:03/rescan"), "switch port A: %s", plan.reason);
	gs_addr_parse(dsp_b.pf, &pf);
	gs_card_inspect(root, &pf, &card);
	gs_plan_prepare(&card, NULL, &plan);
	CHECK(plan.verdict == GS_PLAN_READY && strcmp(card.bridge.str, "0000:02:01.0") == 0 &&
	      contains(card.rescan_path, "/0000:02:01.0/pci_bus/0000:04/rescan"), "switch port B: %s", plan.reason);

	/* A card on a root bus has no bridge to rescan below. */
	snprintf(root, sizeof(root), "%s/ux-rootbus", tmp);
	make_card(root, &rootbus, gpu_rootbus, 0, NULL);
	gs_addr_parse(rootbus.pf, &pf);
	gs_card_inspect(root, &pf, &card);
	gs_plan_prepare(&card, NULL, &plan);
	CHECK(plan.verdict == GS_PLAN_REFUSE && !card.bridge_is_pci, "root-bus card refused");

	/* A domain above ffff (as behind Intel VMD) is handled like any other. */
	snprintf(root, sizeof(root), "%s/ux-vmd", tmp);
	make_card(root, &vmd, gpu_vmd, 0, NULL);
	gs_addr_parse(vmd.pf, &pf);
	gs_card_inspect(root, &pf, &card);
	gs_plan_prepare(&card, NULL, &plan);
	CHECK(plan.verdict == GS_PLAN_READY && strcmp(card.bridge.str, "10000:e0:01.0") == 0 &&
	      contains(card.rescan_path, "/10000:e0:01.0/pci_bus/10000:e1/rescan"), "domain 10000 card: %s", plan.reason);
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--device", vmd.pf, NULL });
	CHECK(rc == 0, "doctor accepts a domain above ffff: %d\n%s", rc, out);
	free(out);
}

static void test_option_guidance(void)
{
	char stage[GS_PATH_MAX], p[GS_PATH_MAX], *out;
	struct stat before, after;
	int rc;

	/* A VF count request elsewhere points to install --vfs, not to a generic unknown option. */
	rc = run_cli(&out, NULL, (const char *const[]){ "prepare", "--vfs=8", NULL });
	CHECK(rc == GS_EXIT_USAGE && !contains(out, "unknown option") && contains(out, "install --vfs"),
	      "--vfs=8: %d\n%s", rc, out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "activate", "--num-vfs", "2", NULL });
	CHECK(rc == GS_EXIT_USAGE && !contains(out, "unknown option"), "--num-vfs: %d\n%s", rc, out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "install", "--num-vfs=2", NULL });
	CHECK(rc == GS_EXIT_USAGE && contains(out, "--vfs N"), "install --num-vfs: %d\n%s", rc, out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "prepare", "--bogus", NULL });
	CHECK(rc == GS_EXIT_USAGE && contains(out, "unknown option"), "--bogus: %d\n%s", rc, out);
	free(out);
	/* The Blackwell options do not exist: plain unknown options, never the VF count hint. */
	rc = run_cli(&out, NULL, (const char *const[]){ "install", "--twin-id=on", NULL });
	CHECK(rc == GS_EXIT_USAGE && contains(out, "unknown option '--twin-id=on'"), "--twin-id: %d\n%s", rc, out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "install", "--experimental-vfs=on", NULL });
	CHECK(rc == GS_EXIT_USAGE && contains(out, "unknown option '--experimental-vfs=on'") && !contains(out, "--vfs N"),
	      "--experimental-vfs: %d\n%s", rc, out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "prepare", "--experimental-vfs=off", NULL });
	CHECK(rc == GS_EXIT_USAGE && contains(out, "unknown option '--experimental-vfs=off'"),
	      "prepare --experimental-vfs: %d\n%s", rc, out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "prepare", "--clear-guard", "--msix=off", NULL });
	CHECK(rc == GS_EXIT_USAGE && contains(out, "--clear-guard takes no other option"),
	      "--clear-guard --msix: %d\n%s", rc, out);
	free(out);

	/* Staging records addresses unchecked and sends the user to doctor on the target host. */
	gs_pathf(stage, sizeof(stage), "%s/ux-stage", tmp);
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--prefix", "/usr", "--source", srcdir, "--device",
							 "0000:7f:00.0", NULL });
	CHECK(rc == 0 && contains(out, "geforce-sriov doctor"), "staging an address absent here: %d\n%s", rc, out);
	free(out);
	gs_pathf(p, sizeof(p), "%s/usr/bin/geforce-sriov", stage);
	CHECK(stat(p, &before) == 0, "staged binary");
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--prefix", "/usr", "--source", srcdir, "--device",
							 "0000:7f:00.0", NULL });
	CHECK(rc == 0 && stat(p, &after) == 0 && after.st_ino == before.st_ino, "identical restage rewrites nothing: %d\n%s",
	      rc, out);
	free(out);

	/* Diagnostic options on a staging install name staging, not the live system. */
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--config", "/tmp/x", "--source", srcdir, NULL });
	CHECK(rc == GS_EXIT_USAGE && !contains(out, "live system"), "DESTDIR + --config: %d\n%s", rc, out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "install", "--destdir", stage, "--sysfs-root", "/tmp", NULL });
	CHECK(rc == GS_EXIT_USAGE && !contains(out, "live system"), "--destdir + --sysfs-root: %d\n%s", rc, out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "install", "--config", "/tmp/x", NULL });
	CHECK(rc == GS_EXIT_USAGE && contains(out, "live system"), "live install + --config: %d\n%s", rc, out);
	free(out);
}

/* procfs files report size 0; they must still be read (pci=noari, driver version). */
static void test_procfs_read(void)
{
	char *data = NULL;
	size_t len = 0;

	CHECK(gs_read_whole("/proc/self/status", &data, &len, 1 << 16) == 0 && len > 0 && strstr(data, "Name:"),
	      "zero-size procfs file read");
	free(data);
}

/* A temporary file an earlier process left (its PID reused after a power loss) never blocks an atomic write. */
static void test_write_atomic(void)
{
	char dir[GS_PATH_MAX], path[GS_PATH_MAX], stale[GS_PATH_MAX], *text;
	struct dirent *de;
	struct stat st;
	unsigned int entries = 0;
	DIR *d;
	int r;

	gs_pathf(dir, sizeof(dir), "%s/atomic", tmp);
	gs_pathf(path, sizeof(path), "%s/guard", dir);
	gs_pathf(stale, sizeof(stale), "%s.gs-tmp.%ld", path, (long)getpid());
	writef(stale, "left by a process with this PID\n");
	r = gs_write_file_atomic(path, "x\n", 2, 0640);
	text = slurp(path);
	CHECK(r == 0 && text && strcmp(text, "x\n") == 0 && stat(path, &st) == 0 && (st.st_mode & 07777) == 0640,
	      "atomic write beside a stale temporary file: %d", r);
	free(text);
	d = opendir(dir);
	while (d && (de = readdir(d)))
		entries += de->d_name[0] != '.';
	if (d)
		closedir(d);
	CHECK(entries == 2 && gs_exists(stale), "atomic write: no temporary file of its own left, the stale one untouched");
}

static void test_sha256(void)
{
	char hex[65], want[65], file[GS_PATH_MAX], lnk[GS_PATH_MAX], real[GS_PATH_MAX], expect[PATH_MAX];
	int r;

	gs_sha256_buf("abc", 3, hex);
	CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0, "sha256 abc");
	gs_sha256_buf("", 0, hex);
	CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0, "sha256 empty");
	gs_sha256_buf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, hex);
	CHECK(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0, "sha256 two blocks");

	/*
	 * gs_sha256_file is strict (the install manifest): a link is refused with -ELOOP, never hashed.
	 * gs_sha256_file_resolved hashes through a link to a regular file (libnvidia-vgpu.so's
	 * documented layout) and returns the target's digest and its resolved path.
	 */
	gs_pathf(file, sizeof(file), "%s/sha/libnvidia-vgpu.so.580.159.01", tmp);
	writef(file, "abc");
	gs_pathf(lnk, sizeof(lnk), "%s/sha/libnvidia-vgpu.so", tmp);
	link_to("libnvidia-vgpu.so.580.159.01", lnk);
	r = gs_sha256_file(file, want);
	CHECK(r == 0 && strcmp(want, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0, "sha256 file: %d",
	      r);
	r = gs_sha256_file(lnk, hex);
	CHECK(r == -ELOOP, "strict sha256 refuses a link: %d", r);
	if (!realpath(file, expect))
		die(file);
	r = gs_sha256_file_resolved(lnk, hex, real);
	CHECK(r == 0 && strcmp(hex, want) == 0 && strcmp(real, expect) == 0,
	      "resolved sha256 through a link equals its target's and names it: %d %s", r, r ? "" : real);
	r = gs_sha256_file_resolved(file, hex, real);
	CHECK(r == 0 && strcmp(hex, want) == 0 && strcmp(real, expect) == 0, "resolved sha256 of the file itself: %d", r);

	/* A link whose target does not exist, a directory and a FIFO are each refused distinctly. */
	link_to("no-such-plugin", lnk);
	r = gs_sha256_file_resolved(lnk, hex, real);
	CHECK(r == -ENOENT, "resolved sha256 dangling link: %d", r);
	r = gs_sha256_file(lnk, hex);
	CHECK(r == -ELOOP, "strict sha256 dangling link: %d", r);
	gs_pathf(file, sizeof(file), "%s/sha/dir", tmp);
	if (gs_mkdir_p(file, 0755))
		die(file);
	link_to("dir", lnk);
	r = gs_sha256_file_resolved(lnk, hex, real);
	CHECK(r == -EISDIR, "resolved sha256 link to a directory: %d", r);
	r = gs_sha256_file(file, hex);
	CHECK(r == -EISDIR, "sha256 directory: %d", r);
	gs_pathf(file, sizeof(file), "%s/sha/fifo", tmp);
	if (mkfifo(file, 0644))
		die(file);
	link_to("fifo", lnk);
	r = gs_sha256_file_resolved(lnk, hex, real);
	CHECK(r == -EINVAL, "resolved sha256 link to a FIFO (no writer, must not block): %d", r);
	r = gs_sha256_file(file, hex);
	CHECK(r == -EINVAL, "sha256 FIFO (no writer, must not block): %d", r);
	r = gs_sha256_file(lnk, hex);
	CHECK(r == -ELOOP, "strict sha256 link to a FIFO: %d", r);
}

static void cleanup(const char *path)
{
	const char *argv[] = { "rm", "-rf", path, NULL };

	gs_run(argv);
}

/* ---- boot display: console release --------------------------------------- */

static int ends_with(const char *s, const char *suffix)
{
	size_t n = strlen(s), m = strlen(suffix);

	return n >= m && strcmp(s + n - m, suffix) == 0;
}

/* The boot display card of the two-card fixtures: 0000:04:00.0 behind bridge 0000:00:03.0. */
static const struct fx_topo topo_display = { "pci0000:00/0000:00:03.0", "0000:04:00.0" };
static const struct fx_fn display_gpu[] = {
	{ "0000:04:00.0", "2206", "030000", NULL },
	{ "0000:04:00.1", "1aef", "040300", "snd_hda_intel" },
	{ NULL, NULL, NULL, NULL },
};

/* <pf>/resource with BAR0 assigned and BAR1 at bar1. */
static void card_resource(const char *root, const struct fx_topo *k, unsigned long long bar1)
{
	static const char *const zero = "0x0000000000000000 0x0000000000000000 0x0000000000000000\n";
	char p[GS_PATH_MAX], text[1024], line[128];

	snprintf(text, sizeof(text), "0x00000000f8000000 0x00000000f8ffffff 0x0000000000040200\n");
	snprintf(line, sizeof(line), "0x%016llx 0x%016llx 0x000000000014220c\n", bar1, bar1 + 0xfffffffULL);
	strcat(text, line);
	for (int i = 2; i <= 12; i++)
		strcat(text, zero);
	gs_pathf(p, sizeof(p), "%s/devices/%s/%s/resource", root, k->bridge_path, k->pf);
	writef(p, "%s", text);
}

/*
 * The first len bytes of <pf>/config: capability list 0x60 PM -> 0x68 MSI -> 0x78 PCI Express,
 * then, with msix, -> 0xc8 MSI-X. Without msix the MSI-X capability stays at 0xc8, unlinked.
 */
static void card_config(const char *root, const struct fx_topo *k, int msix, size_t len)
{
	unsigned char cfg[256] = { 0xde, 0x10, 0x06, 0x22 };
	char p[GS_PATH_MAX];

	cfg[0x06] = 0x10; /* Status: Capabilities List */
	cfg[0x34] = 0x60;
	cfg[0x60] = 0x01, cfg[0x61] = 0x68;
	cfg[0x68] = 0x05, cfg[0x69] = 0x78;
	cfg[0x78] = 0x10, cfg[0x79] = msix ? 0xc8 : 0x00;
	cfg[0xc8] = 0x11;
	gs_pathf(p, sizeof(p), "%s/devices/%s/%s/config", root, k->bridge_path, k->pf);
	if (gs_write_file_atomic(p, cfg, len, 0644))
		die(p);
}

enum { FW_BOUND = 1, FW_PLATFORM = 2, FW_EFI = 4 };

/* The firmware framebuffer sysfb creates, its fb0/card0, and fbcon, as on a host booted on this card. */
static void make_console(const char *root, const struct fx_topo *k, int how)
{
	const char *name = how & FW_EFI ? "efi-framebuffer.0" : "simple-framebuffer.0";
	const char *drv = how & FW_EFI ? "efi-framebuffer" : "simple-framebuffer";
	char rel[GS_PATH_MAX], fw[GS_PATH_MAX], p[GS_PATH_MAX], q[GS_PATH_MAX];

	if (how & FW_PLATFORM)
		gs_pathf(rel, sizeof(rel), "platform/%s", name);
	else
		gs_pathf(rel, sizeof(rel), "%s/%s/%s", k->bridge_path, k->pf, name);
	gs_pathf(fw, sizeof(fw), "%s/devices/%s", root, rel);
	gs_pathf(p, sizeof(p), "%s/uevent", fw);
	writef(p, "DRIVER=%s\n", drv);
	gs_pathf(p, sizeof(p), "%s/bus/platform/drivers/%s/unbind", root, drv);
	writef(p, "%s", "");
	{
		static const char *const dirs[] = { "bus/platform/devices", "class/graphics", "class/drm" };

		for (size_t i = 0; i < 3; i++) {
			gs_pathf(p, sizeof(p), "%s/%s", root, dirs[i]);
			gs_mkdir_p(p, 0755);
		}
	}
	gs_pathf(p, sizeof(p), "%s/bus/platform/devices/%s", root, name);
	gs_pathf(q, sizeof(q), "../../../devices/%s", rel);
	link_to(q, p);
	if (how & FW_BOUND) {
		gs_pathf(p, sizeof(p), "%s/driver", fw);
		gs_pathf(q, sizeof(q), "%s/bus/platform/drivers/%s", root, drv);
		link_to(q, p);
	}
	gs_pathf(p, sizeof(p), "%s/graphics/fb0/dev", fw);
	writef(p, "29:0\n");
	gs_pathf(p, sizeof(p), "%s/graphics/fb0/device", fw);
	link_to("../..", p);
	gs_pathf(p, sizeof(p), "%s/class/graphics/fb0", root);
	gs_pathf(q, sizeof(q), "%s/graphics/fb0", fw);
	link_to(q, p);
	gs_pathf(p, sizeof(p), "%s/drm/card0/dev", fw);
	writef(p, "226:0\n");
	gs_pathf(p, sizeof(p), "%s/drm/card0/device", fw);
	link_to("../..", p);
	gs_pathf(p, sizeof(p), "%s/class/drm/card0", root);
	gs_pathf(q, sizeof(q), "%s/drm/card0", fw);
	link_to(q, p);
	gs_pathf(p, sizeof(p), "%s/class/vtconsole/vtcon0/name", root);
	writef(p, "(S) dummy device\n");
	gs_pathf(p, sizeof(p), "%s/class/vtconsole/vtcon0/bind", root);
	writef(p, "0\n");
	gs_pathf(p, sizeof(p), "%s/class/vtconsole/vtcon1/name", root);
	writef(p, "(M) frame buffer device\n");
	gs_pathf(p, sizeof(p), "%s/class/vtconsole/vtcon1/bind", root);
	writef(p, "1\n");
}

/* root/proc: iomem (as on the two-card host), consoles and cmdline. */
static void make_proc(const char *root, int bootfb)
{
	char p[GS_PATH_MAX];

	gs_pathf(p, sizeof(p), "%s/proc/iomem", root);
	writef(p, "00000000-00000fff : Reserved\n"
		  "80000000-fbffffff : PCI Bus 0000:00\n"
		  "  90000000-91ffffff : PCI Bus 0000:03\n"
		  "    90000000-90ffffff : 0000:03:00.0\n"
		  "    91180000-91183fff : 0000:03:00.1\n"
		  "      91180000-91183fff : ICH HD audio\n"
		  "  f8000000-f9ffffff : PCI Bus 0000:04\n"
		  "    f8000000-f8ffffff : 0000:04:00.0\n"
		  "    f9080000-f9083fff : 0000:04:00.1\n"
		  "      f9080000-f9083fff : ICH HD audio\n"
		  "380000000000-387fffffffff : PCI Bus 0000:00\n"
		  "  387fc0000000-387fd1ffffff : PCI Bus 0000:04\n"
		  "    387fc0000000-387fcfffffff : 0000:04:00.0\n"
		  "%s"
		  "    387fd0000000-387fd1ffffff : 0000:04:00.0\n",
	       bootfb ? "      387fc0000000-387fc02fffff : BOOTFB\n"
			"        387fc0000000-387fc02fffff : simpledrm\n" : "");
	gs_pathf(p, sizeof(p), "%s/proc/consoles", root);
	writef(p, "tty0                 -WU (EC p  )    4:7\nnetcon0              EW    (EC    )\n");
	gs_pathf(p, sizeof(p), "%s/proc/cmdline", root);
	writef(p, "BOOT_IMAGE=/vmlinuz root=ZFS=rpool/ROOT\n");
}

/*
 * A simulated kernel behind prepare's writes on a fixture: unbind/bind move driver links,
 * remove stashes a function, rescan brings every stashed function back (dropping the PF's
 * firmware framebuffer child, as kernfs does) and presents SR-IOV on PFs the module lists.
 * Every write and command is appended to root/sim.log.
 */
static struct {
	char root[GS_PATH_MAX];
	char proc[GS_PATH_MAX], state[GS_PATH_MAX], run[GS_PATH_MAX];
	const char *fail;      /* a write to a path ending in this fails with EIO (after the log) */
	const char *no_sriov;  /* rescan brings this PF back without SR-IOV */
	int modprobe_fail;
	int stuck_console;     /* the framebuffer unbind write succeeds but its driver stays bound */
	int lock_denied;       /* NVIDIA's unbindLock reads 0 after a write of 1 (a client holds the GPU) */
	int systemctl_rc;      /* exit status of every systemctl command */
	int state_not_dir;     /* the nvidia bind replaces the state directory with a file: the guard's lstat fails, ENOTDIR */
	const char *types;     /* every VF's creatable_vgpu_types after the re-probe; NULL: its generation's list */
} sim;

/*
 * NVIDIA's driver probing a PF with VFs: the vGPU manager publishes the VF's types (RTX 6000 Ada
 * types on Ada, RTX A6000 types on Ampere). Measured in one run on one Ada card: the manager restart set
 * power/control=auto and runtime_status=suspended, and the PF re-probe brought them back to on and
 * active. The sim deliberately leaves each VF at auto, suspended and type 0 at the re-probe instead,
 * a worst case, so the tests rely on activate's own write of on and its read-back, not on the re-probe.
 */
static void sim_register_vfs(const char *pf)
{
	char p[GS_PATH_MAX], real[PATH_MAX];
	const struct gs_device *e;
	unsigned int device = 0;

	gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/device", sim.root, pf);
	gs_read_hex(p, &device);
	e = gs_device_lookup(device);
	for (unsigned int i = 0;; i++) {
		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/virtfn%u", sim.root, pf, i);
		if (!realpath(p, real))
			return;
		gs_pathf(p, sizeof(p), "%s/power/control", real);
		writef(p, "auto\n");
		gs_pathf(p, sizeof(p), "%s/power/runtime_status", real);
		writef(p, "suspended\n");
		gs_pathf(p, sizeof(p), "%s/nvidia/creatable_vgpu_types", real);
		if (sim.types)
			writef(p, "ID    : vGPU Name\n%s", sim.types);
		else if (e && e->generation == GS_AMPERE)
			writef(p, "ID    : vGPU Name\n525   : NVIDIA RTXA6000-2Q\n");
		else
			writef(p, "ID    : vGPU Name\n941   : NVIDIA RTX6000-Ada-1Q\n942   : NVIDIA RTX6000-Ada-2Q\n");
		gs_pathf(p, sizeof(p), "%s/nvidia/current_vgpu_type", real);
		writef(p, "0\n");
	}
}

static void sim_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void sim_log(const char *fmt, ...)
{
	char p[GS_PATH_MAX];
	va_list ap;
	FILE *f;

	gs_pathf(p, sizeof(p), "%s/sim.log", sim.root);
	f = fopen(p, "a");
	if (!f)
		die(p);
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fclose(f);
}

static void sim_drop_consumers(const char *fwreal)
{
	static const char *const cls[] = { "graphics/fb0", "drm/card0" };
	char p[GS_PATH_MAX], real[PATH_MAX], *text;

	for (size_t i = 0; i < 2; i++) {
		gs_pathf(p, sizeof(p), "%s/class/%s/device", sim.root, cls[i]);
		if (realpath(p, real) && strcmp(real, fwreal) == 0) {
			gs_pathf(p, sizeof(p), "%s/class/%s", sim.root, cls[i]);
			unlink(p);
		}
	}
	gs_pathf(p, sizeof(p), "%s/iomem", sim.proc);
	text = slurp(p);
	if (text) {
		char *out = calloc(1, strlen(text) + 1), *line, *save = NULL;

		for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
			if (!strstr(line, ": simpledrm"))
				strcat(strcat(out, line), "\n");
		writef(p, "%s", out);
		free(out);
		free(text);
	}
}

static int sim_write(const char *path, const char *value)
{
	size_t n = strlen(sim.root);
	const char *r = strncmp(path, sim.root, n) == 0 && path[n] == '/' ? path + n + 1 : path;
	char p[GS_PATH_MAX], q[GS_PATH_MAX], real[PATH_MAX];

	sim_log("W %s %s\n", r, value);
	if (ends_with(r, "drivers/nvidia/unbind") || ends_with(r, "drivers/nvidia/bind")) {
		sigset_t now;

		/* activate's re-probe holds SIGTERM back from the unbind to the bind. */
		if (sigprocmask(SIG_SETMASK, NULL, &now) || !sigismember(&now, SIGTERM)) {
			gs_pathf(p, sizeof(p), "%s/reprobe-unmasked", tmp);
			writef(p, "%s\n", r);
		}
	}
	if (sim.fail && ends_with(r, sim.fail))
		return -EIO;
	if (ends_with(r, "/power/control")) {
		writef(path, "%s\n", value);
		if (strcmp(value, "on") == 0) {
			gs_pathf(p, sizeof(p), "%.*s/runtime_status", (int)(strlen(path) - 8), path);
			writef(p, "active\n");
		}
		return 0;
	}
	if (ends_with(r, "/unbindLock")) {
		writef(path, "%s\n", sim.lock_denied ? "0" : value);
		return 0;
	}
	if (strncmp(r, "bus/platform/devices/", 21) == 0 && ends_with(r, "/driver/unbind") && sim.stuck_console)
		return 0;
	if (strncmp(r, "bus/platform/devices/", 21) == 0 && ends_with(r, "/driver/unbind")) {
		gs_pathf(p, sizeof(p), "%s/bus/platform/devices/%s", sim.root, value);
		if (!realpath(p, real))
			return -ENODEV;
		gs_pathf(q, sizeof(q), "%s/driver", real);
		unlink(q);
		sim_drop_consumers(real);
		return 0;
	}
	if ((strncmp(r, "bus/platform/drivers/", 21) == 0 || strncmp(r, "bus/pci/drivers/", 16) == 0) &&
	    ends_with(r, "/bind")) {
		gs_pathf(p, sizeof(p), "%s/bus/%s/devices/%s", sim.root,
			 strncmp(r, "bus/platform/", 13) == 0 ? "platform" : "pci", value);
		if (!realpath(p, real))
			return -ENODEV;
		gs_pathf(q, sizeof(q), "%s/driver", real);
		gs_pathf(p, sizeof(p), "%.*s", (int)(strlen(path) - 5), path);
		if (symlink(p, q))
			return -errno;
		if (strcmp(r, "bus/pci/drivers/nvidia/bind") == 0) {
			sim_register_vfs(value);
			if (sim.state_not_dir) {
				gs_pathf(p, sizeof(p), "%s.moved", sim.state);
				if (rename(sim.state, p))
					return -errno;
				writef(sim.state, "not a directory\n");
			}
		}
		return 0;
	}
	if (strncmp(r, "bus/pci/drivers/", 16) == 0 && ends_with(r, "/unbind")) {
		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/driver", sim.root, value);
		return unlink(p) ? -errno : 0;
	}
	if (strncmp(r, "bus/pci/devices/", 16) == 0 && ends_with(r, "/driver/unbind")) {
		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/driver", sim.root, value);
		return unlink(p) ? -errno : 0;
	}
	if (strncmp(r, "bus/pci/devices/", 16) == 0 && ends_with(r, "/remove")) {
		char bdf[32];

		gs_copy(bdf, sizeof(bdf), r + 16);
		*strchr(bdf, '/') = '\0';
		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s", sim.root, bdf);
		if (!realpath(p, real))
			return -ENODEV;
		gs_pathf(q, sizeof(q), "%s/stash/%s.path", sim.root, bdf);
		writef(q, "%s", real);
		gs_pathf(q, sizeof(q), "%s/stash/%s", sim.root, bdf);
		if (rename(real, q))
			return -errno;
		unlink(p);
		return 0;
	}
	if (ends_with(r, "/rescan")) {
		char names[16][32], *bdfs;
		unsigned int k = 0;
		struct dirent *de;
		DIR *d;

		gs_pathf(p, sizeof(p), "%s/stash", sim.root);
		d = opendir(p);
		while (d && (de = readdir(d)) && k < 16)
			if (de->d_name[0] != '.' && !ends_with(de->d_name, ".path"))
				gs_copy(names[k++], sizeof(names[0]), de->d_name);
		if (d)
			closedir(d);
		gs_pathf(p, sizeof(p), "%s/module/%s/parameters/bdfs", sim.root, GS_MODULE_NAME);
		bdfs = slurp(p);
		for (unsigned int i = 0; i < k; i++) {
			char *orig;

			gs_pathf(p, sizeof(p), "%s/stash/%s.path", sim.root, names[i]);
			orig = slurp(p);
			unlink(p);
			gs_pathf(q, sizeof(q), "%s/stash/%s", sim.root, names[i]);
			if (!orig || rename(q, orig))
				die("sim rescan");
			gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s", sim.root, names[i]);
			link_to(orig, p);
			gs_pathf(p, sizeof(p), "%s/simple-framebuffer.0", orig);
			cleanup(p);
			if (ends_with(names[i], ".0") && contains(bdfs, names[i]) &&
			    !(sim.no_sriov && strcmp(sim.no_sriov, names[i]) == 0)) {
				char *vfs;

				/* The module presents its vfs parameter (the card's hardware has 16). */
				gs_pathf(p, sizeof(p), "%s/module/%s/parameters/vfs", sim.root, GS_MODULE_NAME);
				vfs = slurp(p);
				gs_pathf(p, sizeof(p), "%s/sriov_totalvfs", orig);
				writef(p, "%s", vfs ? vfs : "4\n");
				free(vfs);
				gs_pathf(p, sizeof(p), "%s/sriov_numvfs", orig);
				writef(p, "0\n");
			}
			free(orig);
		}
		free(bdfs);
		return 0;
	}
	return gs_write_attr(path, value);
}

static int sim_run(const char *const argv[])
{
	char line[1024] = "R", p[GS_PATH_MAX];

	for (size_t i = 0; argv[i]; i++)
		strcat(strcat(line, " "), argv[i]);
	sim_log("%s\n", line);
	if (strcmp(argv[0], "systemctl") == 0)
		return sim.systemctl_rc;
	/* nvidia_vgpu_vfio is a plain module load: it appears, and changes no geforce_sriov parameter. */
	if (strcmp(argv[0], "modprobe") == 0 && argv[1] && strcmp(argv[1], "nvidia_vgpu_vfio") == 0) {
		if (sim.modprobe_fail)
			return 1;
		gs_pathf(p, sizeof(p), "%s/module/nvidia_vgpu_vfio/refcnt", sim.root);
		writef(p, "1\n");
		return 0;
	}
	if (strcmp(argv[0], "modprobe") != 0 || sim.modprobe_fail)
		return 1;
	/* Like the module: vfs defaults to GS_DEFAULT_VFS when modprobe does not pass it. */
	gs_pathf(p, sizeof(p), "%s/module/%s/parameters/vfs", sim.root, GS_MODULE_NAME);
	writef(p, "%u\n", GS_DEFAULT_VFS);
	for (size_t i = 2; argv[i]; i++) {
		const char *eq = strchr(argv[i], '=');

		gs_pathf(p, sizeof(p), "%s/module/%s/parameters/%.*s", sim.root, GS_MODULE_NAME,
			 (int)(eq - argv[i]), argv[i]);
		if (strncmp(argv[i], "bdfs=", 5) == 0 || strncmp(argv[i], "vfs=", 4) == 0)
			writef(p, "%s\n", eq + 1);
		else
			writef(p, "%s\n", strcmp(eq + 1, "1") == 0 ? "Y" : "N");
	}
	gs_pathf(p, sizeof(p), "%s/kernel/livepatch/%s/enabled", sim.root, GS_MODULE_NAME);
	writef(p, "1\n");
	gs_pathf(p, sizeof(p), "%s/kernel/livepatch/%s/transition", sim.root, GS_MODULE_NAME);
	writef(p, "0\n");
	return 0;
}

/* nvidia-smi answers with the fixture's nvidia-smi.out; without it, nvidia-smi is not installed. */
static int sim_capture(const char *const argv[], char *out, size_t len)
{
	char line[1024] = "R", p[GS_PATH_MAX], *text;

	for (size_t i = 0; argv[i]; i++)
		strcat(strcat(line, " "), argv[i]);
	sim_log("%s\n", line);
	out[0] = '\0';
	gs_pathf(p, sizeof(p), "%s/nvidia-smi.out", sim.root);
	if (strcmp(argv[0], "nvidia-smi") != 0 || !(text = slurp(p)))
		return -1;
	gs_copy(out, len, text);
	free(text);
	return 0;
}

/*
 * Card 0000:03:00.0 (with audio) and, with with_04, the boot display card 0000:04:00.0 with
 * its console made as display says (-1: no firmware framebuffer at all).
 */
static void two_cards(const char *name, int with_04, int display, char *root, size_t len)
{
	snprintf(root, len, "%s/%s", tmp, name);
	make_card(root, &topo_default, gpu_with_audio, 0, NULL);
	card_resource(root, &topo_default, 0x380000000000ULL);
	if (with_04) {
		make_card(root, &topo_display, display_gpu, 1, NULL);
		card_resource(root, &topo_display, 0x387fc0000000ULL);
		if (display >= 0)
			make_console(root, &topo_display, display);
	}
	make_proc(root, with_04 && display >= 0 && !(display & (FW_PLATFORM | FW_EFI)));
	memset(&sim, 0, sizeof(sim));
	gs_copy(sim.root, sizeof(sim.root), root);
	gs_pathf(sim.proc, sizeof(sim.proc), "%s/proc", root);
	gs_pathf(sim.state, sizeof(sim.state), "%s/state", root);
	gs_pathf(sim.run, sizeof(sim.run), "%s/run", root);
}

static void cfg_of(struct gs_config *cfg, const char *text)
{
	char err[512];

	if (gs_config_parse(text, strlen(text), cfg, err, sizeof(err))) {
		fprintf(stderr, "fixture config invalid: %s\n", err);
		exit(2);
	}
}

/* gs_prepare against the simulated host in a child; returns its exit status and output. */
static int run_prepare(struct gs_config *cfg, int boot, const char *release, char **out)
{
	struct gs_host h = { sim.root, sim.proc, sim.state, sim.run, sim_write, sim_run, NULL };
	struct gs_prepare_opts po;
	int pipefd[2];
	pid_t pid;

	memset(&po, 0, sizeof(po));
	po.boot = boot;
	if (release) {
		gs_addr_parse(release, &po.release[0]);
		po.nrelease = 1;
	}
	if (pipe(pipefd))
		die("pipe");
	pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0) {
		int rc;

		dup2(pipefd[1], 1);
		dup2(pipefd[1], 2);
		close(pipefd[0]);
		rc = gs_prepare(&h, cfg, &po);
		fflush(stdout);
		fflush(stderr);
		_exit(rc);
	}
	close(pipefd[1]);
	return collect(pipefd[0], pid, out);
}

static int sim_exists(const char *rel)
{
	char p[GS_PATH_MAX];

	gs_pathf(p, sizeof(p), "%s/%s", sim.root, rel);
	return gs_exists(p);
}

static char *sim_text(const char *rel)
{
	char p[GS_PATH_MAX];

	gs_pathf(p, sizeof(p), "%s/%s", sim.root, rel);
	return slurp(p);
}

/* Position of needle in hay, or -1. */
static long pos_of(const char *hay, const char *needle)
{
	const char *p = hay ? strstr(hay, needle) : NULL;

	return p ? (long)(p - hay) : -1;
}

static void test_release_config(void)
{
	struct gs_config c, back;
	char err[512], text[4096];
	int n;
	static const char *const bad[] = {
		"bdfs=0000:03:00.0\nboot_display_release=0000:04:00.0\n",
		"boot_display_release=0000:04:00.0\n",
		"bdfs=0000:04:00.0\nboot_display_release=0000:04:00.1\n",
		"bdfs=0000:04:00.0\nboot_display_release=0000:04:00.0,0000:04:00.0\n",
		"bdfs=0000:04:00.0\nboot_display_release=0000:04:00.0\nboot_display_release=0000:04:00.0\n",
		"bdfs=0000:04:00.0\nboot_display_release=04:00.0\n",
		NULL,
	};

	for (int i = 0; bad[i]; i++)
		CHECK(gs_config_parse(bad[i], strlen(bad[i]), &c, err, sizeof(err)) != 0, "rejects %s", bad[i]);
	CHECK(gs_config_parse(bad[0], strlen(bad[0]), &c, err, sizeof(err)) != 0 && strstr(err, "not listed in bdfs"),
	      "a grant outside bdfs names why: %s", err);
	/* Key order does not matter; the grant is a subset of bdfs. */
	n = snprintf(text, sizeof(text), "boot_display_release=0000:04:00.0\nbdfs=0000:03:00.0,0000:04:00.0\n");
	CHECK(gs_config_parse(text, (size_t)n, &c, err, sizeof(err)) == 0 && c.nrelease == 1 &&
	      gs_config_grants_release(&c, "0000:04:00.0") && !gs_config_grants_release(&c, "0000:03:00.0"),
	      "grant parsed: %s", err);
	n = gs_config_render(&c, text, sizeof(text));
	CHECK(n > 0 && gs_config_parse(text, (size_t)n, &back, err, sizeof(err)) == 0 && back.nrelease == 1 &&
	      strcmp(back.boot_display_release[0].str, "0000:04:00.0") == 0, "render keeps the grant: %s", err);
	gs_config_defaults(&c);
	n = gs_config_render(&c, text, sizeof(text));
	CHECK(n > 0 && !strstr(text, "boot_display_release="), "no grant rendered by default");
}

/* gs_release_steps for a grant of bdf, each line after indent. */
#define RELEASE_STEPS(bdf, indent) \
	indent "geforce-sriov prepare --release-console=" bdf "\n" indent "modprobe --ignore-install nvidia\n" \
	indent "geforce-sriov activate\n" indent "geforce-sriov check\n"
#define RELEASE_STEPS_04(indent) RELEASE_STEPS("0000:04:00.0", indent)
#define RELEASE_HOLD_04 \
	"/etc/modprobe.d/nvidia-manual-load.conf containing\n" \
	"  blacklist nvidia\n" \
	"  install nvidia /bin/false\n" \
	"and keep nvidia-persistenced, nvidia-vgpu-mgr and similar services disabled.\n" \
	GS_RELEASE_STEPS_LEAD "\n" RELEASE_STEPS_04("  ") \
	"  or run geforce-sriov setup, which asks, then runs them\n" \
	"Details: docs/installation.md#boot-display-card\n"

static void test_release_cli(void)
{
	char stage[GS_PATH_MAX], p[GS_PATH_MAX], *out, *text;
	int rc;
	static const char *const usage_cases[][4] = {
		{ "prepare", "--release-console=0000:04:00", NULL, NULL },
		{ "prepare", "--release-console=0000:04:00.1", NULL, NULL },
		{ "prepare", "--boot", "--release-console=0000:04:00.0", NULL },
		{ "prepare", "--clear-guard", "--release-console=0000:04:00.0", NULL },
		{ "prepare", "--clear-guard", "--boot", NULL },
		{ "prepare", "--release-console=0000:04:00.0", "--release-console=0000:04:00.0", NULL },
	};

	for (size_t i = 0; i < sizeof(usage_cases) / sizeof(usage_cases[0]); i++) {
		rc = run_cli(&out, NULL, usage_cases[i]);
		CHECK(rc == GS_EXIT_USAGE, "%s %s %s: usage error: %d\n%s", usage_cases[i][0], usage_cases[i][1],
		      usage_cases[i][2] ? usage_cases[i][2] : "", rc, out);
		free(out);
	}
	if (geteuid() != 0) {
		rc = run_cli(&out, NULL, (const char *const[]){ "prepare", "--release-console=0000:04:00.0", NULL });
		CHECK(rc == GS_EXIT_REFUSED && contains(out, "must run as root"), "release non-root: %d\n%s", rc, out);
		free(out);
	}

	gs_pathf(stage, sizeof(stage), "%s/release-stage", tmp);
	gs_pathf(p, sizeof(p), "%s/etc/geforce-sriov/config", stage);
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--prefix", "/usr", "--source", srcdir,
							 "--boot-display-release", "0000:04:00.0", NULL });
	CHECK(rc == GS_EXIT_USAGE, "grant without --device: %d\n%s", rc, out);
	free(out);
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--prefix", "/usr", "--source", srcdir, "--device",
							 "0000:03:00.0", "--boot-display-release", "0000:04:00.0", NULL });
	CHECK(rc == GS_EXIT_USAGE, "grant for a card not selected: %d\n%s", rc, out);
	free(out);
	CHECK(!gs_exists(p), "a usage error stages nothing");
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--prefix", "/usr", "--source", srcdir, "--device",
							 "0000:03:00.0", "--device", "0000:04:00.0",
							 "--boot-display-release", "0000:04:00.0", NULL });
	CHECK(rc == 0 && count_of(out, RELEASE_HOLD_04) == 1 && pos_of(out, RELEASE_HOLD_04) > pos_of(out, "staged below ") &&
	      count_of(out, "Console release:") == 1 && pos_of(out, "Console release:") < pos_of(out, "staged below "),
	      "staged grant: the hold and the commands after each boot once, in the closing block; the console note once, "
	      "before it: %d\n%s", rc, out);
	/* The grant is recorded at install; the release, and what it stops, come after the power cycle. */
	CHECK(contains(out, "Console release: granted; nothing is released now. After the power cycle, the commands\n"
			    "below or `geforce-sriov setup`, which asks again first, release the console. That stops the\n"
			    "local display on that GPU until the following boot, and the monitor may keep the last frame\n") &&
	      !contains(out, "stays detached until the next boot"),
	      "staged grant: the note says nothing is released now:\n%s", out);
	free(out);
	/* After setup's question, which explained the grant, install does not repeat its note. */
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--prefix", "/usr", "--source", srcdir, "--device",
							 "0000:03:00.0", "--device", "0000:04:00.0",
							 "--boot-display-release", "0000:04:00.0", "--release-note-shown", NULL });
	CHECK(rc == 0 && count_of(out, RELEASE_HOLD_04) == 1 && !contains(out, "Console release:"),
	      "staged grant after setup's question: no console note: %d\n%s", rc, out);
	free(out);
	/* The staged tree already holds nvidia back: the closing block names its file, as on the target host, instead of the one to create. */
	{
		static const char want[] = "nvidia is already held back at boot by /etc/modprobe.d/nvidia-manual-load.conf.\n"
					   GS_RELEASE_STEPS_LEAD "\n" RELEASE_STEPS_04("  ");
		char hold[GS_PATH_MAX];

		gs_pathf(hold, sizeof(hold), "%s/etc/modprobe.d/nvidia-manual-load.conf", stage);
		writef(hold, "blacklist nvidia\ninstall nvidia /bin/false\n");
		rc = run_cli(&out, stage, (const char *const[]){ "install", "--prefix", "/usr", "--source", srcdir, "--device",
								 "0000:03:00.0", "--device", "0000:04:00.0",
								 "--boot-display-release", "0000:04:00.0", NULL });
		CHECK(rc == 0 && count_of(out, want) == 1 && !contains(out, hold) && !contains(out, "create\n") &&
		      !contains(out, "nvidia-persistenced"),
		      "staged grant with the hold in place: the file named once, the commands kept: %d\n%s", rc, out);
		free(out);
		unlink(hold);
	}
	text = slurp(p);
	CHECK(contains(text, "bdfs=0000:03:00.0,0000:04:00.0\n") && contains(text, "boot_display_release=0000:04:00.0\n"),
	      "grant written: %s", text ? text : "(missing)");
	free(text);
	gs_pathf(p, sizeof(p), "%s/etc/systemd/system/geforce-sriov-prepare.service", stage);
	text = slurp(p);
	CHECK(contains(text, "ExecStart=/usr/bin/geforce-sriov prepare --boot\n"), "boot unit runs prepare --boot");
	free(text);
	gs_pathf(p, sizeof(p), "%s/etc/systemd/system/nvidia-vgpud.service.d/geforce-sriov.conf", stage);
	text = slurp(p);
	CHECK(contains(text, "AssertPathExists=!" GS_DEFERRED "\n"), "nvidia-vgpud is held back while deferred");
	free(text);
	/* A later install without options keeps it; reselecting without the flag drops it. */
	gs_pathf(p, sizeof(p), "%s/etc/geforce-sriov/config", stage);
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--prefix", "/usr", "--source", srcdir, NULL });
	free(out);
	text = slurp(p);
	CHECK(rc == 0 && contains(text, "boot_display_release=0000:04:00.0\n"), "reinstall keeps the grant");
	free(text);
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--prefix", "/usr", "--source", srcdir, "--device",
							 "0000:03:00.0", "--device", "0000:04:00.0", NULL });
	free(out);
	text = slurp(p);
	CHECK(rc == 0 && contains(text, "bdfs=0000:03:00.0,0000:04:00.0\n") && !contains(text, "boot_display_release"),
	      "--device without the flag drops the grant: %s", text ? text : "(missing)");
	free(text);
}

static void test_display_inspect(void)
{
	char root[GS_PATH_MAX], p[GS_PATH_MAX], q[GS_PATH_MAX], why[256];
	struct gs_display d;
	struct gs_card c;
	struct gs_plan plan;
	struct gs_addr pf;

	gs_addr_parse(topo_display.pf, &pf);

	/* A host booted on this card: simple-framebuffer.0 under the PF, simpledrm bound, fb0/card0, fbcon. */
	two_cards("disp-ok", 1, FW_BOUND, root, sizeof(root));
	gs_card_inspect(root, &pf, &c);
	gs_display_inspect(root, sim.proc, &c, &d);
	CHECK(!d.refuse[0] && strcmp(d.fw, "simple-framebuffer.0") == 0 && gs_display_fw_state(&d) == GS_FW_BOUND &&
	      strcmp(d.fw_driver, "simple-framebuffer") == 0 && d.nconsumers == 2 && d.proc_checked,
	      "owned simplefb accepted: %s", d.refuse);
	gs_plan_prepare(&c, &d, &plan);
	CHECK(plan.verdict == GS_PLAN_READY && strcmp(plan.release, "simple-framebuffer.0") == 0 &&
	      strcmp(plan.release_driver, "simple-framebuffer") == 0 && plan.nunbind == 1, "granted: release planned: %s",
	      plan.reason);
	gs_plan_prepare(&c, NULL, &plan);
	CHECK(plan.verdict == GS_PLAN_REFUSE && strstr(plan.reason, "boot display"), "no grant: refused as before");
	/* The unbound child stays registered: released once its fb and DRM devices are gone. */
	gs_pathf(p, sizeof(p), "%s/devices/%s/%s/simple-framebuffer.0/driver", root, topo_display.bridge_path,
		 topo_display.pf);
	unlink(p);
	gs_display_inspect(root, NULL, &c, &d);
	CHECK(gs_display_fw_state(&d) == GS_FW_RELEASED && strstr(d.refuse, "remain"),
	      "unbound child with its fb left: refused: %s", d.refuse);
	gs_pathf(p, sizeof(p), "%s/class/graphics/fb0", root);
	unlink(p);
	gs_pathf(p, sizeof(p), "%s/class/drm/card0", root);
	unlink(p);
	gs_display_inspect(root, NULL, &c, &d);
	CHECK(gs_display_fw_state(&d) == GS_FW_RELEASED && !d.refuse[0] && !d.proc_checked,
	      "unbound child without consumers: released: %s", d.refuse);
	gs_display_inspect(root, sim.proc, &c, &d);
	CHECK(strstr(d.refuse, "simpledrm"), "iomem still held by the driver after the unbind: refused: %s", d.refuse);

	/* The grant on a card that is not the boot display now: nothing to release, refused. */
	gs_display_inspect(root, NULL, &c, &d);
	c.boot_vga = 0;
	gs_plan_prepare(&c, &d, &plan);
	CHECK(plan.verdict == GS_PLAN_REFUSE && strstr(plan.reason, "not the boot display"), "grant on boot_vga=0: %s",
	      plan.reason);

	/* Ownership: a framebuffer not parented to this PF, or not simple-framebuffer, refuses. */
	two_cards("disp-platform", 1, FW_BOUND | FW_PLATFORM, root, sizeof(root));
	gs_card_inspect(root, &pf, &c);
	gs_display_inspect(root, NULL, &c, &d);
	CHECK(strstr(d.refuse, "not to this card"), "framebuffer under the platform root refused: %s", d.refuse);
	two_cards("disp-efi", 1, FW_BOUND | FW_EFI, root, sizeof(root));
	gs_card_inspect(root, &pf, &c);
	gs_display_inspect(root, NULL, &c, &d);
	CHECK(strstr(d.refuse, "not a simple-framebuffer"), "efi-framebuffer refused: %s", d.refuse);
	/* Another fb on the PF itself; the legacy VGA text console; unreadable proc. */
	two_cards("disp-extra", 1, FW_BOUND, root, sizeof(root));
	gs_card_inspect(root, &pf, &c);
	gs_pathf(p, sizeof(p), "%s/devices/%s/%s/graphics/fb1", root, topo_display.bridge_path, topo_display.pf);
	gs_mkdir_p(p, 0755);
	gs_pathf(q, sizeof(q), "%s/device", p);
	link_to("../..", q);
	gs_pathf(q, sizeof(q), "%s/class/graphics/fb1", root);
	link_to(p, q);
	gs_display_inspect(root, NULL, &c, &d);
	CHECK(strstr(d.refuse, "graphics/fb1 is on the card itself"), "fb on the PF refused: %s", d.refuse);
	two_cards("disp-vga", 1, FW_BOUND, root, sizeof(root));
	gs_card_inspect(root, &pf, &c);
	gs_pathf(p, sizeof(p), "%s/class/vtconsole/vtcon0/name", root);
	writef(p, "(S) VGA+\n");
	gs_pathf(p, sizeof(p), "%s/class/vtconsole/vtcon0/bind", root);
	writef(p, "1\n");
	gs_display_inspect(root, NULL, &c, &d);
	CHECK(strstr(d.refuse, "VGA+"), "bound vgacon refused: %s", d.refuse);
	gs_display_inspect(root, "/nonexistent-proc", &c, &d);
	CHECK(d.refuse[0], "unreadable proc refused");

	/* /proc parsers. */
	{
		static const char *const iomem_ok =
			"  387fc0000000-387fd1ffffff : PCI Bus 0000:04\n"
			"    387fc0000000-387fcfffffff : 0000:04:00.0\n"
			"      387fc0000000-387fc02fffff : BOOTFB\n"
			"        387fc0000000-387fc02fffff : simpledrm\n"
			"    387fd0000000-387fd1ffffff : 0000:04:00.0\n"
			"    380000000000-38000fffffff : 0000:03:00.0\n"
			"      380000000000-38000fffffff : nvidia\n";

		CHECK(gs_iomem_check(iomem_ok, "0000:04:00.0", GS_FW_BOUND, why, sizeof(why)) == 0,
		      "BOOTFB with its driver while bound; another card's child ignored: %s", why);
		CHECK(gs_iomem_check(iomem_ok, "0000:04:00.0", GS_FW_RELEASED, why, sizeof(why)) != 0,
		      "driver region left after release refused");
		CHECK(gs_iomem_check(iomem_ok, "0000:04:00.0", GS_FW_NONE, why, sizeof(why)) != 0,
		      "BOOTFB without a firmware framebuffer refused");
		CHECK(gs_iomem_check("    f8000000-f8ffffff : 0000:04:00.0\n      f8000000-f800ffff : efifb\n",
				     "0000:04:00.0", GS_FW_BOUND, why, sizeof(why)) != 0 && strstr(why, "efifb"),
		      "another user inside the BAR refused: %s", why);
		CHECK(gs_iomem_check("    387fc0000000-387fcfffffff : 0000:04:00.0\n"
				     "      387fc0000000-387fc02fffff : BOOTFB\n"
				     "    387fd0000000-387fd1ffffff : 0000:04:00.0\n",
				     "0000:04:00.0", GS_FW_RELEASED, why, sizeof(why)) == 0, "released BOOTFB alone: %s", why);
		CHECK(gs_iomem_check("", "0000:04:00.0", GS_FW_BOUND, why, sizeof(why)) != 0, "empty iomem refused");
		/* At the kernel's indentation cap a BAR's children print level with it: nesting unknown. */
		CHECK(gs_iomem_check("          f8000000-f8ffffff : 0000:04:00.0\n"
				     "          f8000000-f800ffff : nvidia\n",
				     "0000:04:00.0", GS_FW_BOUND, why, sizeof(why)) != 0,
		      "BAR and a user at the indentation cap: refused");
		CHECK(gs_consoles_check("efifb0               -W- (E  p  )\n", why, sizeof(why)) != 0,
		      "early efifb console refused");
		CHECK(gs_consoles_check("tty0                 -WU (EC p  )    4:7\n", why, sizeof(why)) == 0, "tty0 fine");
	}
}

static void test_release_diag(void)
{
	char root[GS_PATH_MAX], cfg[GS_PATH_MAX], *out;
	int rc;

	two_cards("diag-release", 1, FW_BOUND, root, sizeof(root));
	gs_pathf(cfg, sizeof(cfg), "%s/diag-release-config", tmp);
	writef(cfg, "bdfs=0000:03:00.0,0000:04:00.0\n");
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--config", cfg, NULL });
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "boot display"), "doctor: boot display without grant: %d\n%s", rc, out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "status", "--sysfs-root", root, "--config", cfg, NULL });
	CHECK(rc == GS_EXIT_FAIL && contains(out, "--boot-display-release 0000:04:00.0"),
	      "status: how to grant: %d\n%s", rc, out);
	free(out);
	writef(cfg, "bdfs=0000:03:00.0,0000:04:00.0\nboot_display_release=0000:04:00.0\n");
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--config", cfg, NULL });
	CHECK(rc == 0 && contains(out, "would release its console") && contains(out, "boot_vga 1") &&
	      contains(out, "       " GS_RELEASE_STEPS_LEAD "\n" RELEASE_STEPS_04("         ")
			    "         or run geforce-sriov setup, which asks, then runs them\n"),
	      "doctor: granted: %d\n%s", rc, out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "status", "--sysfs-root", root, "--config", cfg, NULL });
	CHECK(rc == GS_EXIT_FAIL && contains(out, "deferred") &&
	      contains(out, "  " GS_RELEASE_STEPS_LEAD "\n" RELEASE_STEPS_04("    ")
			    "    or run geforce-sriov setup, which asks, then runs them\n"),
	      "status: deferred and the next commands: %d\n%s",
	      rc, out);
	free(out);
	two_cards("diag-release-bad", 1, FW_BOUND | FW_PLATFORM, root, sizeof(root));
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--config", cfg, NULL });
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "not safe"), "doctor: unattributed framebuffer: %d\n%s", rc, out);
	free(out);
}

/* Nothing was written, run or recorded on the simulated host. */
static void check_untouched(const char *what)
{
	char *log = sim_text("sim.log");

	CHECK(!log, "%s: no write and no command: %s", what, log ? log : "");
	CHECK(!sim_exists("state/" GS_ATTEMPT_NAME), "%s: no guard", what);
	CHECK(!sim_exists("run/" GS_PREPARED_NAME), "%s: no preparation record", what);
	free(log);
}

static void test_prepare_default_and_deferral(void)
{
	char root[GS_PATH_MAX], *out, *log, *rec;
	struct gs_config cfg;
	int rc;

	/* Without a grant the boot path is unchanged: 03 alone is prepared at boot. */
	two_cards("prep-default", 0, -1, root, sizeof(root));
	cfg_of(&cfg, "bdfs=0000:03:00.0\n");
	rc = run_prepare(&cfg, 1, NULL, &out);
	log = sim_text("sim.log");
	rec = sim_text("run/" GS_PREPARED_NAME "/0000:03:00.0");
	CHECK(rc == 0 && contains(log, "R modprobe geforce_sriov ari=1 msix=1 bdfs=0000:03:00.0") &&
	      contains(log, "W bus/pci/devices/0000:03:00.1/driver/unbind") &&
	      contains(log, "W bus/pci/devices/0000:03:00.0/remove 1") && contains(log, "/rescan 1") &&
	      !contains(log, "platform"), "default boot prepare: %d\n%s\n%s", rc, out, log);
	CHECK(contains(rec, "totalvfs=4") && !contains(rec, "console_release"), "default record: %s", rec ? rec : "");
	CHECK(!sim_exists("state/" GS_ATTEMPT_NAME) && !sim_exists("run/" GS_DEFERRED_NAME), "no guard, no deferral");
	free(out);
	free(log);
	free(rec);

	/* A module already loaded with another msix is refused before any removal; equal values are reused. */
	two_cards("prep-loaded", 0, -1, root, sizeof(root));
	fake_module(root, "0000:03:00.0");
	cfg_of(&cfg, "bdfs=0000:03:00.0\nmsix=off\n");
	rc = run_prepare(&cfg, 1, NULL, &out);
	log = sim_text("sim.log");
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "loaded with ari=Y msix=Y vfs=4") &&
	      contains(out, "requested ari=Y msix=N vfs=4") && contains(out, "; power the machine off and on to change them\n") &&
	      !contains(log, "R modprobe") &&
	      !contains(log, "/remove") && !sim_exists("state/" GS_ATTEMPT_NAME),
	      "loaded msix=Y, requested off: refused: %d\n%s\n%s", rc, out, log);
	free(out);
	free(log);
	cfg_of(&cfg, "bdfs=0000:03:00.0\n");
	rc = run_prepare(&cfg, 1, NULL, &out);
	CHECK(rc == 0 && contains(out, "already loaded with the requested parameters"), "loaded, same values: %d\n%s", rc,
	      out);
	free(out);

	/* Default refusal of the boot display: every card is checked first, nothing changes. */
	two_cards("prep-nogrant", 1, FW_BOUND, root, sizeof(root));
	cfg_of(&cfg, "bdfs=0000:03:00.0,0000:04:00.0\n");
	rc = run_prepare(&cfg, 1, NULL, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "boot display"), "boot display without grant: %d\n%s", rc, out);
	check_untouched("boot display without grant");
	free(out);
	rc = run_prepare(&cfg, 0, "0000:04:00.0", &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "grants no console release"), "consent without grant: %d\n%s", rc, out);
	check_untouched("consent without grant");
	free(out);

	/* With the grant the boot unit defers: nothing on any card, only the deferral record. */
	two_cards("prep-defer", 1, FW_BOUND, root, sizeof(root));
	cfg_of(&cfg, "bdfs=0000:03:00.0,0000:04:00.0\nboot_display_release=0000:04:00.0\n");
	rc = run_prepare(&cfg, 1, NULL, &out);
	CHECK(rc == 0 && contains(out, "deferred") &&
	      contains(out, GS_RELEASE_STEPS_LEAD "\n" RELEASE_STEPS_04("  ") "  or run geforce-sriov setup, which asks, then runs them\n"),
	      "boot unit defers: %d\n%s", rc, out);
	check_untouched("deferred boot");
	rec = sim_text("run/" GS_DEFERRED_NAME);
	CHECK(contains(rec, "boot_display_release=0000:04:00.0"), "deferral recorded: %s", rec ? rec : "");
	free(rec);
	free(out);
	/* By hand without (or with the wrong) consent: refused, nothing changed, still deferred. */
	rc = run_prepare(&cfg, 0, NULL, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "prepare --release-console=0000:04:00.0"),
	      "no consent: %d\n%s", rc, out);
	check_untouched("no consent");
	free(out);
	rc = run_prepare(&cfg, 0, "0000:03:00.0", &out);
	CHECK(rc == GS_EXIT_REFUSED, "consent for an ungranted card: %d\n%s", rc, out);
	check_untouched("wrong consent");
	CHECK(sim_exists("run/" GS_DEFERRED_NAME), "deferral kept");
	free(out);

	/* An unsafe console refuses before the first change, for the whole selection. */
	two_cards("prep-unsafe", 1, FW_BOUND | FW_PLATFORM, root, sizeof(root));
	rc = run_prepare(&cfg, 0, "0000:04:00.0", &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "not safe"), "unattributed framebuffer: %d\n%s", rc, out);
	check_untouched("unattributed framebuffer");
	free(out);
}

/* After the card comes back, prepare says whether MSI-X is linked, from the PF's config space. */
static void test_prepare_msix_message(void)
{
	static const char linked[] = "0000:03:00.0: msix=on: MSI-X is linked\n";
	static const char hidden[] = "0000:03:00.0: msix=on: MSI-X stays hidden; the kernel log says why: "
				     "journalctl -k -b | grep geforce_sriov\n";
	char root[GS_PATH_MAX], *out;
	struct gs_config cfg;
	int rc;

	cfg_of(&cfg, "bdfs=0000:03:00.0\n");
	two_cards("prep-msix-linked", 0, -1, root, sizeof(root));
	card_config(root, &topo_default, 1, 256);
	rc = run_prepare(&cfg, 1, NULL, &out);
	CHECK(rc == 0 && contains(out, linked) && !contains(out, "stays hidden"), "MSI-X linked: %d\n%s", rc, out);
	free(out);

	two_cards("prep-msix-hidden", 0, -1, root, sizeof(root));
	card_config(root, &topo_default, 0, 256);
	rc = run_prepare(&cfg, 1, NULL, &out);
	CHECK(rc == 0 && contains(out, hidden) && !contains(out, "is linked"), "MSI-X not linked: %d\n%s", rc, out);
	free(out);

	cfg_of(&cfg, "bdfs=0000:03:00.0\nmsix=off\n");
	two_cards("prep-msix-off", 0, -1, root, sizeof(root));
	card_config(root, &topo_default, 1, 256);
	rc = run_prepare(&cfg, 1, NULL, &out);
	CHECK(rc == 0 && !contains(out, "MSI-X"), "msix=off: no MSI-X line: %d\n%s", rc, out);
	free(out);

	/* Without a config file, or one that stops before the capability list is walked: no line. */
	cfg_of(&cfg, "bdfs=0000:03:00.0\n");
	two_cards("prep-msix-noconfig", 0, -1, root, sizeof(root));
	rc = run_prepare(&cfg, 1, NULL, &out);
	CHECK(rc == 0 && contains(out, "sriov_totalvfs=4") && !contains(out, "MSI-X"), "no config: no MSI-X line: %d\n%s",
	      rc, out);
	free(out);
	two_cards("prep-msix-short", 0, -1, root, sizeof(root));
	card_config(root, &topo_default, 1, 64);
	rc = run_prepare(&cfg, 1, NULL, &out);
	CHECK(rc == 0 && !contains(out, "MSI-X"), "64-byte config: no MSI-X line: %d\n%s", rc, out);
	free(out);
}

static void test_prepare_release(void)
{
	char root[GS_PATH_MAX], p[GS_PATH_MAX], *out, *log, *rec;
	struct gs_config cfg;
	long unbind, load;
	int rc;

	cfg_of(&cfg, "bdfs=0000:03:00.0,0000:04:00.0\nboot_display_release=0000:04:00.0\n");

	/* Consent: the console is released before the module loads; both cards in one pass. */
	two_cards("prep-release", 1, FW_BOUND, root, sizeof(root));
	gs_pathf(p, sizeof(p), "%s/" GS_DEFERRED_NAME, sim.run);
	writef(p, "boot_id=x\n");
	rc = run_prepare(&cfg, 0, "0000:04:00.0", &out);
	log = sim_text("sim.log");
	unbind = pos_of(log, "W bus/platform/devices/simple-framebuffer.0/driver/unbind simple-framebuffer.0");
	load = pos_of(log, "R modprobe geforce_sriov ari=1 msix=1 bdfs=0000:03:00.0,0000:04:00.0");
	CHECK(rc == 0 && unbind >= 0 && load > unbind && pos_of(log, "W bus/pci/devices/0000:04:00.0/remove") > load &&
	      !contains(log, "/bind "), "release, one load, both cards, no rebind: %d\n%s\n%s", rc, out, log);
	CHECK(contains(out, GS_CONSOLE_RELEASE_NOTE), "consent text shown");
	rec = sim_text("run/" GS_PREPARED_NAME "/0000:04:00.0");
	CHECK(contains(rec, "console_release=simple-framebuffer.0\n"), "console release recorded: %s", rec ? rec : "");
	free(rec);
	rec = sim_text("run/" GS_PREPARED_NAME "/0000:03:00.0");
	CHECK(rec && !contains(rec, "console_release"), "03 record has no console: %s", rec ? rec : "");
	free(rec);
	CHECK(!sim_exists("state/" GS_ATTEMPT_NAME) && !sim_exists("run/" GS_DEFERRED_NAME),
	      "guard cleared and deferral over");
	free(out);
	free(log);
	/* Prepared already: a second run changes nothing. */
	gs_pathf(p, sizeof(p), "%s/sim.log", sim.root);
	unlink(p);
	rc = run_prepare(&cfg, 0, "0000:04:00.0", &out);
	log = sim_text("sim.log");
	CHECK(rc == 0 && !log && contains(out, "already prepared"), "second run: %d\n%s", rc, out);
	free(out);
	free(log);

	/* The module fails to load: no removal was attempted, so the console is given back. */
	two_cards("prep-modfail", 1, FW_BOUND, root, sizeof(root));
	sim.modprobe_fail = 1;
	rc = run_prepare(&cfg, 0, "0000:04:00.0", &out);
	log = sim_text("sim.log");
	CHECK(rc == GS_EXIT_FAIL && pos_of(log, "W bus/platform/drivers/simple-framebuffer/bind simple-framebuffer.0") >
	      pos_of(log, "R modprobe") && !contains(log, "/remove") && !sim_exists("state/" GS_ATTEMPT_NAME) &&
	      contains(out, "console restored"), "module failure: console rebound: %d\n%s\n%s", rc, out, log);
	free(out);
	free(log);

	/*
	 * The console does not let go (the unbind write fails, or succeeds without effect): refused
	 * before the module loads, no removal, no bind write, the framebuffer still bound.
	 */
	for (int mode = 0; mode < 2; mode++) {
		char fwdrv[GS_PATH_MAX];

		two_cards(mode ? "prep-stuck" : "prep-unbind-error", 1, FW_BOUND, root, sizeof(root));
		if (mode)
			sim.stuck_console = 1;
		else
			sim.fail = "simple-framebuffer.0/driver/unbind";
		rc = run_prepare(&cfg, 0, "0000:04:00.0", &out);
		log = sim_text("sim.log");
		gs_pathf(fwdrv, sizeof(fwdrv), "%s/devices/%s/%s/simple-framebuffer.0/driver", root,
			 topo_display.bridge_path, topo_display.pf);
		CHECK(rc == GS_EXIT_REFUSED && contains(log, "driver/unbind simple-framebuffer.0") &&
		      !contains(log, "R modprobe") && !contains(log, "/remove") && !contains(log, "/bind ") &&
		      gs_exists(fwdrv) && !sim_exists("state/" GS_ATTEMPT_NAME) && !sim_exists("run/" GS_PREPARED_NAME),
		      "console never released (%s): refused before load, untouched otherwise: %d\n%s\n%s",
		      mode ? "no effect" : "write error", rc, out, log);
		free(out);
		free(log);
	}

	/* 03 fails before 04's removal: 04's console is given back. */
	two_cards("prep-03fails", 1, FW_BOUND, root, sizeof(root));
	sim.no_sriov = "0000:03:00.0";
	rc = run_prepare(&cfg, 0, "0000:04:00.0", &out);
	log = sim_text("sim.log");
	CHECK(rc == GS_EXIT_FAIL && !contains(log, "0000:04:00.0/remove") &&
	      contains(log, "W bus/platform/drivers/simple-framebuffer/bind simple-framebuffer.0"),
	      "other card failed first: console rebound: %d\n%s\n%s", rc, out, log);
	free(out);
	free(log);

	/* 04's remove write fails: its removal was attempted, so the console is never rebound. */
	two_cards("prep-04fails", 1, FW_BOUND, root, sizeof(root));
	sim.fail = "0000:04:00.0/remove";
	sim.no_sriov = "0000:04:00.0";
	rc = run_prepare(&cfg, 0, "0000:04:00.0", &out);
	log = sim_text("sim.log");
	CHECK(rc == GS_EXIT_FAIL && contains(log, "W bus/pci/devices/0000:04:00.0/remove 1") && !contains(log, "/bind ") &&
	      contains(out, "stays released"), "remove attempted: never rebound: %d\n%s\n%s", rc, out, log);
	free(out);
	free(log);

	/* 03 is prepared, then 04 refuses (its bridge power/control write fails): the module is loaded, so exit 1, not 3. */
	two_cards("prep-04refuses", 1, FW_BOUND, root, sizeof(root));
	sim.fail = "0000:00:03.0/power/control";
	rc = run_prepare(&cfg, 0, "0000:04:00.0", &out);
	log = sim_text("sim.log");
	CHECK(rc == GS_EXIT_FAIL && pos_of(log, "W bus/pci/devices/0000:03:00.0/remove 1") > pos_of(log, "R modprobe") &&
	      !contains(log, "0000:04:00.0/remove") && contains(out, "bridge 0000:00:03.0 power/control=on failed") &&
	      sim_exists("run/" GS_PREPARED_NAME "/0000:03:00.0"),
	      "refused after the load: exit 1: %d\n%s\n%s", rc, out, log);
	free(out);
	free(log);
}

static void test_enable_check(void)
{
	struct gs_config granted, plain;
	struct gs_prepared rec;
	struct gs_card c;
	char why[512];

	cfg_of(&granted, "bdfs=0000:03:00.0,0000:04:00.0\nboot_display_release=0000:04:00.0\n");
	cfg_of(&plain, "bdfs=0000:03:00.0,0000:04:00.0\n");
	memset(&c, 0, sizeof(c));
	gs_addr_parse("0000:04:00.0", &c.pf);
	c.present = 1;
	c.entry = gs_device_lookup(0x2206);
	c.boot_vga = 1;
	memset(&rec, 0, sizeof(rec));
	rec.present = 1;
	gs_copy(rec.boot_id, sizeof(rec.boot_id), "boot-a");
	gs_copy(rec.console_release, sizeof(rec.console_release), "simple-framebuffer.0");
	CHECK(gs_check_enable(&c, &granted, &rec, "boot-a", why, sizeof(why)) == 0, "released this boot, granted: %s", why);
	CHECK(gs_check_enable(&c, &plain, &rec, "boot-a", why, sizeof(why)) != 0, "no grant: refused");
	CHECK(gs_check_enable(&c, &granted, &rec, "boot-b", why, sizeof(why)) != 0, "another boot: refused");
	/* boot_vga can read 0 after the rescan: the record still requires the grant. */
	c.boot_vga = 0;
	CHECK(gs_check_enable(&c, &plain, &rec, "boot-a", why, sizeof(why)) != 0, "released console, grant removed: refused");
	rec.console_release[0] = '\0';
	CHECK(gs_check_enable(&c, &plain, &rec, "boot-a", why, sizeof(why)) == 0, "ordinary card: allowed");
	c.boot_vga = 1;
	CHECK(gs_check_enable(&c, &granted, &rec, "boot-a", why, sizeof(why)) != 0,
	      "boot display prepared without a release: refused");
	rec.present = 0;
	CHECK(gs_check_enable(&c, &granted, &rec, "boot-a", why, sizeof(why)) != 0, "not prepared: refused");
}

/* The configurable VF count at prepare, doctor, status and enable's presented-count check. */
static void test_vf_count(void)
{
	char root[GS_PATH_MAX], cfgp[GS_PATH_MAX], p[GS_PATH_MAX], why[512], *out, *log, *rec;
	struct gs_config cfg;
	struct gs_card c;
	int rc, f1, w1, f2, w2;

	/* vfs=2 reaches the module and is the count the card presents and prepare records. */
	two_cards("vf-two", 0, -1, root, sizeof(root));
	cfg_of(&cfg, "bdfs=0000:03:00.0\nvfs=2\n");
	rc = run_prepare(&cfg, 1, NULL, &out);
	log = sim_text("sim.log");
	rec = sim_text("run/" GS_PREPARED_NAME "/0000:03:00.0");
	CHECK(rc == 0 && contains(log, "R modprobe geforce_sriov ari=1 msix=1 vfs=2 bdfs=0000:03:00.0") &&
	      contains(rec, "totalvfs=2"), "vfs=2 prepare: %d\n%s\n%s", rc, out, log);
	free(out);
	free(log);
	free(rec);

	/* Above 4: warned before anything changes; the module refusing to load leaves the card untouched. */
	two_cards("vf-eight", 0, -1, root, sizeof(root));
	cfg_of(&cfg, "bdfs=0000:03:00.0\nvfs=8\n");
	sim.modprobe_fail = 1;
	rc = run_prepare(&cfg, 1, NULL, &out);
	log = sim_text("sim.log");
	CHECK(rc == GS_EXIT_FAIL && contains(out, "0000:03:00.0: warning: vfs=8") && contains(log, " vfs=8 ") &&
	      !contains(log, "/remove") && !sim_exists("state/" GS_ATTEMPT_NAME) && !sim_exists("run/" GS_PREPARED_NAME),
	      "vfs=8: warned, module refusal changes nothing: %d\n%s\n%s", rc, out, log);
	free(out);
	free(log);

	/* Above what the card offers (Ada reports 1 and presents 4): refused before the first change. */
	two_cards("vf-ada", 0, -1, root, sizeof(root));
	gs_pathf(p, sizeof(p), "%s/devices/%s/%s/device", root, topo_default.bridge_path, topo_default.pf);
	writef(p, "0x2702\n");
	cfg_of(&cfg, "bdfs=0000:03:00.0\nvfs=8\n");
	rc = run_prepare(&cfg, 1, NULL, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "vfs=8 is above the 4 VFs geforce-sriov presents on 10de:2702 (hardware default 1)"),
	      "vfs above Ada's 4 refused: %d\n%s", rc, out);
	check_untouched("vfs above Ada's 4");
	free(out);
	/* The same refusal through gs_vfs_check() and the description gs_vfs_describe() gives. */
	{
		const struct gs_device *ada = gs_device_lookup(0x2702), *amp = gs_device_lookup(0x2206);
		const struct gs_device *rtx2000 = gs_device_lookup(0x28b0);

		cfg_of(&cfg, "vfs=8\n");
		CHECK(ada && gs_vfs_check(ada, &cfg, why, sizeof(why)) < 0 &&
		      strcmp(why, "vfs=8 is above the 4 VFs geforce-sriov presents on 10de:2702 (hardware default 1)") == 0,
		      "Ada vfs=8: %s", why);
		CHECK(amp && gs_vfs_check(amp, &cfg, why, sizeof(why)) > 0, "Ampere vfs=8: a warning: %s", why);
		/* The RTX 2000 Ada reports 16: vfs=8 is within its hardware default, so only the warning. */
		CHECK(rtx2000 && gs_vfs_check(rtx2000, &cfg, why, sizeof(why)) > 0, "RTX 2000 Ada vfs=8: a warning: %s", why);
		/* Above the hardware default: the parser refuses 17, so set it directly. */
		cfg.vfs = 17;
		CHECK(amp && gs_vfs_check(amp, &cfg, why, sizeof(why)) < 0 &&
		      contains(why, "vfs=17 is above the hardware default of 10de:2206, 16 VFs"), "Ampere vfs=17: %s", why);
		cfg_of(&cfg, "");
		gs_vfs_describe(ada, &cfg, why, sizeof(why));
		CHECK(strcmp(why, "hardware default 1, geforce-sriov presents 4 (default)") == 0, "Ada describe: %s", why);
		gs_vfs_describe(rtx2000, &cfg, why, sizeof(why));
		CHECK(strcmp(why, "hardware default 16, geforce-sriov presents 4 (default)") == 0, "RTX 2000 Ada describe: %s",
		      why);
	}

	/* doctor and status show the hardware default next to the presented count. */
	gs_pathf(cfgp, sizeof(cfgp), "%s/vf-config", tmp);
	fixture("vf-doctor", gpu_with_audio, 0, NULL, root, sizeof(root));
	writef(cfgp, "bdfs=0000:03:00.0\n");
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--config", cfgp, NULL });
	CHECK(rc == 0 && contains(out, "hardware default 16, geforce-sriov presents 4 (default)"),
	      "doctor default count: %d\n%s", rc, out);
	free(out);
	writef(cfgp, "bdfs=0000:03:00.0\nvfs=2\n");
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--config", cfgp, NULL });
	tally_of(out, &f1, &w1);
	CHECK(rc == 0 && contains(out, "presents 2 (vfs=2)"), "doctor vfs=2: %d\n%s", rc, out);
	free(out);
	writef(cfgp, "bdfs=0000:03:00.0\nvfs=8\n");
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--config", cfgp, NULL });
	tally_of(out, &f2, &w2);
	CHECK(rc == 0 && f2 == f1 && w2 == w1 + 1 && contains(out, "[WARN] 0000:03:00.0: vfs=8"), "doctor vfs=8 warns: %d\n%s",
	      rc, out);
	free(out);
	fixture("vf-doctor-ada", ada_cross_die_gpu, 0, NULL, root, sizeof(root));
	writef(cfgp, "bdfs=0000:03:00.0\nvfs=2\n");
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--config", cfgp, NULL });
	CHECK(rc == 0 && contains(out, "hardware default 1, geforce-sriov presents 2 (vfs=2)"),
	      "doctor Ada vfs=2 accepted: %d\n%s", rc, out);
	free(out);
	writef(cfgp, "bdfs=0000:03:00.0\nvfs=8\n");
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--config", cfgp, NULL });
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "vfs=8 is above the 4 VFs geforce-sriov presents on 10de:2702"),
	      "doctor Ada vfs above 4: %d\n%s", rc, out);
	free(out);

	fixture("vf-status", gpu_with_audio, 0, "0/2", root, sizeof(root));
	writef(cfgp, "bdfs=0000:03:00.0\nvfs=2\n");
	rc = run_cli(&out, NULL, (const char *const[]){ "status", "--sysfs-root", root, "--config", cfgp, NULL });
	CHECK(rc == 0 && contains(out, "presents 2 (vfs=2)") && contains(out, "sriov_totalvfs=2"), "status vfs=2: %d\n%s",
	      rc, out);
	free(out);
	fixture("vf-status4", gpu_with_audio, 0, "0/4", root, sizeof(root));
	rc = run_cli(&out, NULL, (const char *const[]){ "status", "--sysfs-root", root, "--config", cfgp, NULL });
	CHECK(rc == GS_EXIT_FAIL && contains(out, "outside 1..2"), "status: presented above vfs: %d\n%s", rc, out);
	free(out);

	/* enable acts only on a presented count within 1..vfs, and then enables exactly that count. */
	memset(&c, 0, sizeof(c));
	gs_addr_parse("0000:03:00.0", &c.pf);
	c.present = 1;
	c.entry = gs_device_lookup(0x2206);
	c.has_sriov = 1;
	c.totalvfs = 4;
	cfg_of(&cfg, "bdfs=0000:03:00.0\n");
	CHECK(gs_check_presented(&c, &cfg, why, sizeof(why)) == 0, "4 presented, default: %s", why);
	cfg_of(&cfg, "bdfs=0000:03:00.0\nvfs=2\n");
	CHECK(gs_check_presented(&c, &cfg, why, sizeof(why)) != 0, "4 presented, vfs=2: refused");
	c.totalvfs = 2;
	CHECK(gs_check_presented(&c, &cfg, why, sizeof(why)) == 0, "2 presented, vfs=2: %s", why);
	c.totalvfs = 0;
	CHECK(gs_check_presented(&c, &cfg, why, sizeof(why)) != 0, "0 presented: refused");
	c.totalvfs = 2;
	c.has_sriov = 0;
	CHECK(gs_check_presented(&c, &cfg, why, sizeof(why)) != 0, "nothing presented: refused");
}

/* ---- activate and check (Ada and Ampere) --------------------------------- */

static const struct fx_fn ada_gpu[] = {
	{ "0000:03:00.0", "2783", "030000", "nvidia" },
	{ "0000:03:00.1", "22bc", "040300", "snd_hda_intel" },
	{ "0000:03:00.4", "2783", "030000", "nvidia" },
	{ NULL, NULL, NULL, NULL },
};

#define ADA_DEV "devices/pci0000:00/0000:00:02.0/"

static void sim_writef(const char *rel, const char *text)
{
	char p[GS_PATH_MAX];

	gs_pathf(p, sizeof(p), "%s/%s", sim.root, rel);
	writef(p, "%s", text);
}

/*
 * An Ada host after `enable`: AD104 PF 0000:03:00.0 and its one VF 0000:03:00.4 on
 * nvidia, prepared this boot, NVIDIA's 580.159.01 driver loaded with GSP off. The VF reads
 * on/active, as an early udev hook leaves it before the re-probe.
 */
static void ada_host(const char *name, char *root, size_t len)
{
	char p[GS_PATH_MAX], boot[64] = "";

	snprintf(root, len, "%s/%s", tmp, name);
	make_card(root, &topo_default, ada_gpu, 0, "1/1");
	memset(&sim, 0, sizeof(sim));
	gs_copy(sim.root, sizeof(sim.root), root);
	gs_pathf(sim.proc, sizeof(sim.proc), "%s/proc", root);
	gs_pathf(sim.state, sizeof(sim.state), "%s/state", root);
	gs_pathf(sim.run, sizeof(sim.run), "%s/run", root);
	gs_pathf(p, sizeof(p), "%s/" ADA_DEV "0000:03:00.4/physfn", root);
	link_to("../0000:03:00.0", p);
	gs_pathf(p, sizeof(p), "%s/" ADA_DEV "0000:03:00.0/virtfn0", root);
	link_to("../0000:03:00.4", p);
	sim_writef(ADA_DEV "0000:03:00.4/power/control", "on\n");
	sim_writef(ADA_DEV "0000:03:00.4/power/runtime_status", "active\n");
	sim_writef(ADA_DEV "0000:03:00.4/nvidia/current_vgpu_type", "0\n");
	sim_writef("proc/driver/nvidia/version",
		   "NVRM version: NVIDIA UNIX x86_64 Kernel Module  580.159.01  Release Build\n"
		   "GCC version:  gcc version 12.2.0 (Debian 12.2.0-14)\n");
	sim_writef("proc/driver/nvidia/params", "ResmanDebugLevel: 4294967295\nEnableGpuFirmware: 0\nRegistryDwords: \"\"\n");
	sim_writef("proc/driver/nvidia/gpus/0000:03:00.0/unbindLock", "0\n");
	sim_writef("module/nvidia_vgpu_vfio/refcnt", "0\n");
	gs_boot_id(boot, sizeof(boot));
	gs_pathf(p, sizeof(p), "%s/run/" GS_PREPARED_NAME "/0000:03:00.0", root);
	writef(p, "boot_id=%s\ntotalvfs=1\nari_enabled=1\n", boot);
}

static int child_activate(const struct gs_config *cfg, const struct gs_activate_opts *o)
{
	struct gs_host h = { sim.root, sim.proc, sim.state, sim.run, sim_write, sim_run, sim_capture };

	return gs_activate(&h, cfg, o);
}

static int child_check(const struct gs_config *cfg, const struct gs_activate_opts *o)
{
	struct gs_host h = { sim.root, sim.proc, sim.state, sim.run, sim_write, sim_run, NULL };

	return gs_check_vfs(&h, cfg, o);
}

/* fn against the simulated host in a child; returns its exit status and output. */
static int run_ada(int (*fn)(const struct gs_config *, const struct gs_activate_opts *), const struct gs_config *cfg,
		   const struct gs_activate_opts *o, char **out)
{
	int pipefd[2];
	pid_t pid;

	if (pipe(pipefd))
		die("pipe");
	pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0) {
		int rc;

		dup2(pipefd[1], 1);
		dup2(pipefd[1], 2);
		close(pipefd[0]);
		rc = fn(cfg, o);
		fflush(stdout);
		fflush(stderr);
		_exit(rc);
	}
	close(pipefd[1]);
	return collect(pipefd[0], pid, out);
}

static void check_ada_untouched(const char *what)
{
	char *log = sim_text("sim.log");

	CHECK(!log, "%s: no write and no command: %s", what, log ? log : "");
	CHECK(!sim_exists("state/" GS_ATTEMPT_NAME), "%s: no guard", what);
	free(log);
}

/*
 * check against the simulated host while rel appears mid-read: the VF's power/control is a FIFO
 * whose writer creates rel only once check has opened it in its per-VF loop, then answers "on".
 */
static int check_marker_mid_read(const struct gs_config *cfg, const struct gs_activate_opts *o, const char *rel, char **out)
{
	char fifo[GS_PATH_MAX], marker[GS_PATH_MAX];
	int rc, fd, status;
	pid_t pid;

	gs_pathf(fifo, sizeof(fifo), "%s/" ADA_DEV "0000:03:00.4/power/control", sim.root);
	gs_pathf(marker, sizeof(marker), "%s/%s", sim.root, rel);
	unlink(fifo);
	if (mkfifo(fifo, 0644))
		die("mkfifo");
	pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0) {
		fd = open(fifo, O_WRONLY | O_CLOEXEC);
		if (fd < 0)
			_exit(2);
		writef(marker, "boot_id=x\n");
		_exit(write(fd, "on\n", 3) == 3 ? 0 : 2);
	}
	rc = run_ada(child_check, cfg, o, out);
	/* A check that never opened the FIFO leaves the writer blocked in open(): release it. */
	if (waitpid(pid, &status, WNOHANG) == 0) {
		fd = open(fifo, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		waitpid(pid, &status, 0);
		if (fd >= 0)
			close(fd);
	}
	return rc;
}

static void test_activate(void)
{
	static const char *const order[] = {
		"R systemctl restart nvidia-vgpud.service\n",
		"R systemctl restart nvidia-vgpu-mgr.service\n",
		"R systemctl is-active --quiet nvidia-vgpu-mgr.service\n",
		"W proc/driver/nvidia/gpus/0000:03:00.0/unbindLock 1\n",
		"W bus/pci/drivers/nvidia/unbind 0000:03:00.0\n",
		"W bus/pci/drivers/nvidia/bind 0000:03:00.0\n",
		"W bus/pci/devices/0000:03:00.4/nvidia/current_vgpu_type 942\n",
		"W bus/pci/devices/0000:03:00.4/power/control on\n",
	};
	static const struct {
		const char *name, *rel, *text, *msg;
	} refusals[] = {
		/* The supported version with GSP on: still refused. */
		{ "ada-gsp", "proc/driver/nvidia/params", "EnableGpuFirmware: 18\n", "NVreg_EnableGpuFirmware=0" },
		{ "ada-nover", "proc/driver/nvidia/version", "", "NVIDIA's driver is not loaded" },
		{ "ada-vgpu", ADA_DEV "0000:03:00.4/nvidia/current_vgpu_type", "942\n", "would end it" },
		{ "ada-novf", ADA_DEV "0000:03:00.0/sriov_numvfs", "0\n",
		  "install NVIDIA's vGPU host driver" },
		{ "ada-unlisted", ADA_DEV "0000:03:00.0/device", "0x2b85\n",
		  "geforce-sriov: refused, nothing changed: 0000:03:00.0 (10de:2b85) is not in the device table\n" },
	};
	char root[GS_PATH_MAX], p[GS_PATH_MAX], q[GS_PATH_MAX], want[GS_PATH_MAX + 128], sha[65], *out, *log, *t;
	struct gs_activate_opts o;
	struct gs_config cfg;
	unsigned int v = 0;
	long prev = -1;
	int rc;

	CHECK(gs_parse_vgpu_type("942", &v) == 0 && v == 942, "type 942");
	CHECK(gs_parse_vgpu_type("0", &v) != 0 && gs_parse_vgpu_type("0942", &v) != 0 &&
	      gs_parse_vgpu_type("1000000", &v) != 0 && gs_parse_vgpu_type("9x", &v) != 0, "bad types refused");
	cfg_of(&cfg, "bdfs=0000:03:00.0\n");

	/* The bring-up order; the type and power/control come after the last re-probe. --type 942 is Ada's default. */
	ada_host("ada-ok", root, sizeof(root));
	memset(&o, 0, sizeof(o));
	o.type = 942;
	rc = run_ada(child_check, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_FAIL && contains(out, "NOT READY 0000:03:00.4"), "check before activate: %d\n%s", rc, out);
	free(out);
	rc = run_ada(child_activate, &cfg, &o, &out);
	log = sim_text("sim.log");
	CHECK(rc == GS_EXIT_OK && !contains(out, "not supported"), "activate: %d\n%s", rc, out);
	for (size_t k = 0; k < sizeof(order) / sizeof(order[0]); k++) {
		long at = pos_of(log, order[k]);

		CHECK(at > prev, "activate order: %s at %ld, after %ld\n%s", order[k], at, prev, log ? log : "");
		if (at >= 0)
			prev = at;
	}
	CHECK(!contains(log, "rescan") && !contains(log, "/remove"), "activate never removes or rescans: %s", log);
	CHECK(!sim_exists("state/" GS_ATTEMPT_NAME), "guard cleared after the re-probe");
	t = sim_text(ADA_DEV "0000:03:00.4/power/control");
	CHECK(t && strcmp(t, "on\n") == 0, "VF power/control on after the re-probe: %s", t ? t : "(none)");
	free(t);
	t = sim_text(ADA_DEV "0000:03:00.4/nvidia/current_vgpu_type");
	CHECK(t && strncmp(t, "942", 3) == 0 && (t[3] == '\0' || t[3] == '\n'), "VF type 942: %s", t ? t : "(none)");
	free(t);
	free(log);
	free(out);
	rc = run_ada(child_check, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_OK && contains(out, "OK 0000:03:00.4") && contains(out, "NVIDIA's driver 580.159.01 is supported\n"),
	      "check after activate: %d\n%s", rc, out);
	free(out);
	o.type = 943;
	rc = run_ada(child_check, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_FAIL, "check with another type: %d\n%s", rc, out);
	free(out);
	/*
	 * A simulated re-registration leaves each VF at power/control=auto, runtime_status=suspended and
	 * type 0: check fails closed.
	 */
	sim_register_vfs("0000:03:00.0");
	o.type = 0;
	rc = run_ada(child_check, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_FAIL && contains(out, "type=0 expected=942 power/control=auto"),
	      "check after a re-registration: %d\n%s", rc, out);
	free(out);

	/* Without --type the card gets its generation's default, 942 on Ada, and check expects it. */
	ada_host("ada-notype", root, sizeof(root));
	memset(&o, 0, sizeof(o));
	rc = run_ada(child_activate, &cfg, &o, &out);
	log = sim_text("sim.log");
	CHECK(rc == GS_EXIT_OK && contains(log, "W bus/pci/devices/0000:03:00.4/nvidia/current_vgpu_type 942\n") &&
	      contains(log, "0000:03:00.4/power/control on"), "activate without --type: Ada default 942: %d\n%s\n%s", rc, out,
	      log ? log : "");
	free(log);
	free(out);
	rc = run_ada(child_check, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_OK && contains(out, "OK 0000:03:00.4 (VF of 0000:03:00.0): driver=nvidia type=942 expected=942 "
					       "power/control=on runtime_status=active"),
	      "check without --type expects 942: %d\n%s", rc, out);
	free(out);

	/* A type the VF does not list (the driver keeps the types of its first registration this boot): failed, the
	 * cause and the cold boot named, no type written. */
	ada_host("ada-type-kept", root, sizeof(root));
	memset(&o, 0, sizeof(o));
	o.type = 943;
	o.types_wait_ms = 200;
	rc = run_ada(child_activate, &cfg, &o, &out);
	log = sim_text("sim.log");
	CHECK(rc == GS_EXIT_FAIL &&
	      contains(out, "geforce-sriov: 0000:03:00.4: type 943 is not in nvidia/creatable_vgpu_types (`cat` it for the "
			    "types this GPU\noffers); nothing written. The driver keeps the vGPU types it registered first after "
			    "a boot, so after a\nchange of identity or target, power the machine off and on\n") &&
	      !contains(log, "current_vgpu_type"), "activate, type not creatable: the cause and the cold boot named: %d\n%s",
	      rc, out);
	free(log);
	free(out);
	memset(&o, 0, sizeof(o));

	/* --dry-run: every check, then the steps, and no change. */
	ada_host("ada-dry", root, sizeof(root));
	o.dry_run = 1;
	rc = run_ada(child_activate, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_OK && contains(out, "dry run") && contains(out, "0000:03:00.4: echo 942 > nvidia/current_vgpu_type"),
	      "dry run: %d\n%s", rc, out);
	check_ada_untouched("dry run");
	free(out);
	o.dry_run = 0;

	/* The line doctor, check and activate print for the loaded driver version. */
	{
		char line[256];

		rc = gs_driver_version_line("580.159.01", line, sizeof(line));
		CHECK(rc == 0 && strcmp(line, "NVIDIA's driver 580.159.01 is supported") == 0, "version line, supported: %d %s",
		      rc, line);
		rc = gs_driver_version_line("595.91.04", line, sizeof(line));
		CHECK(rc == 1 && strcmp(line, "NVIDIA's driver 595.91.04 is not supported") == 0,
		      "version line, not supported: %d %s", rc, line);
	}

	/* Another driver version is not supported but not refused: activate says so and runs the normal order. */
	for (size_t g = 0; g < 2; g++) {
		ada_host(g ? "ada-ver-gsp" : "ada-ver", root, sizeof(root));
		sim_writef("proc/driver/nvidia/version",
			   "NVRM version: NVIDIA UNIX x86_64 Kernel Module  595.91.04  Release Build\n");
		/* On another driver version GSP on is a warning too. */
		if (g)
			sim_writef("proc/driver/nvidia/params", "EnableGpuFirmware: 18\n");
		rc = run_ada(child_activate, &cfg, &o, &out);
		log = sim_text("sim.log");
		CHECK(rc == GS_EXIT_OK && !contains(out, "refused") &&
		      contains(out, "geforce-sriov: NVIDIA's driver 595.91.04 is not supported; continuing\n"),
		      "other version %zu: activate continues: %d\n%s", g, rc, out);
		CHECK(!g || contains(out, "EnableGpuFirmware: 18; Ada and Ampere need "
					  "NVreg_EnableGpuFirmware=0 (GSP off, CPU-RM); continuing on this driver version\n"),
		      "other version with GSP on: warned: %s", out);
		prev = -1;
		for (size_t k = 0; k < sizeof(order) / sizeof(order[0]); k++) {
			long at = pos_of(log, order[k]);

			CHECK(at > prev, "other version %zu, order: %s at %ld, after %ld\n%s", g, order[k], at, prev,
			      log ? log : "");
			if (at >= 0)
				prev = at;
		}
		free(log);
		free(out);
		rc = run_ada(child_check, &cfg, &o, &out);
		CHECK(rc == GS_EXIT_OK && contains(out, "OK 0000:03:00.4") &&
		      contains(out, "NVIDIA's driver 595.91.04 is not supported\n"),
		      "other version %zu: check reports it, the VFs decide: %d\n%s", g, rc, out);
		free(out);
	}
	/* On another driver version the plugin digest is a warning too: that version ships its own plugin. */
	ada_host("ada-ver-plugin", root, sizeof(root));
	sim_writef("proc/driver/nvidia/version", "NVRM version: NVIDIA UNIX x86_64 Kernel Module  595.91.04  Release Build\n");
	gs_pathf(p, sizeof(p), "%s/libnvidia-vgpu.so", root);
	writef(p, "not NVIDIA's plugin\n");
	o.plugin = p;
	rc = run_ada(child_activate, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_OK && !contains(out, "refused") && contains(out, ": not the patched plugin copy of NVIDIA's 580.159.01") &&
	      contains(out, "; continuing on this driver version\n"),
	      "other version with another plugin: warned, continues: %d\n%s", rc, out);
	free(out);
	o.plugin = NULL;

	for (size_t k = 0; k < sizeof(refusals) / sizeof(refusals[0]); k++) {
		struct gs_config row;

		ada_host(refusals[k].name, root, sizeof(root));
		sim_writef(refusals[k].rel, refusals[k].text);
		/*
		 * A card with no VFs needs sriov-manage, which the build host may or may not have: pin the
		 * path so the refusal is the same everywhere.
		 */
		cfg_of(&row, "bdfs=0000:03:00.0\nsriov_manage=/nonexistent/geforce-sriov-test/sriov-manage\n");
		rc = run_ada(child_activate, &row, &o, &out);
		CHECK(rc == GS_EXIT_REFUSED && contains(out, refusals[k].msg), "%s: %d\n%s", refusals[k].name, rc, out);
		check_ada_untouched(refusals[k].name);
		free(out);
	}

	/* The VF must be the PF's own: its physfn link leads back to the PF. */
	ada_host("ada-physfn", root, sizeof(root));
	gs_pathf(p, sizeof(p), "%s/" ADA_DEV "0000:03:00.4/physfn", root);
	link_to("../0000:03:00.1", p);
	rc = run_ada(child_activate, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "does not lead back"), "physfn: %d\n%s", rc, out);
	check_ada_untouched("physfn");
	free(out);

	/* A VF handed to another driver is refused. */
	ada_host("ada-vfio", root, sizeof(root));
	gs_pathf(p, sizeof(p), "%s/" ADA_DEV "0000:03:00.4/driver", root);
	link_to("../../../../bus/pci/drivers/vfio-pci", p);
	rc = run_ada(child_activate, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "bound to vfio-pci, not nvidia"), "VF driver: %d\n%s", rc, out);
	check_ada_untouched("VF driver");
	free(out);

	/* Not prepared this boot. */
	ada_host("ada-unprepared", root, sizeof(root));
	gs_pathf(p, sizeof(p), "%s/run/" GS_PREPARED_NAME "/0000:03:00.0", root);
	unlink(p);
	rc = run_ada(child_activate, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "not prepared this boot"), "unprepared: %d\n%s", rc, out);
	check_ada_untouched("unprepared");
	free(out);

	/* --device outside the selection. */
	ada_host("ada-device", root, sizeof(root));
	o.ndevices = 1;
	gs_addr_parse("0000:05:00.0", &o.devices[0]);
	rc = run_ada(child_activate, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "is not selected"), "--device: %d\n%s", rc, out);
	check_ada_untouched("--device");
	free(out);
	o.ndevices = 0;

	/* The guard of an unfinished attempt stops everything. */
	ada_host("ada-guard", root, sizeof(root));
	sim_writef("state/" GS_ATTEMPT_NAME, "boot_id=x\nstep=reprobe\n");
	rc = run_ada(child_activate, &cfg, &o, &out);
	log = sim_text("sim.log");
	CHECK(rc == GS_EXIT_COLD_CYCLE && !log, "guard set: %d\n%s", rc, out);
	free(log);
	free(out);

	/* A client holds the GPU: the lock is released and nothing is unbound. */
	ada_host("ada-lock", root, sizeof(root));
	sim.lock_denied = 1;
	rc = run_ada(child_activate, &cfg, &o, &out);
	log = sim_text("sim.log");
	CHECK(rc == GS_EXIT_FAIL && contains(out, "did not grant unbindLock") && contains(out, "were restarted") &&
	      contains(log, "R systemctl restart nvidia-vgpu-mgr.service"), "lock denied after the daemon restart: %d\n%s", rc, out);
	CHECK(contains(log, "unbindLock 1\n") && contains(log, "unbindLock 0\n") && !contains(log, "drivers/nvidia/unbind") &&
	      !contains(log, "power/control"), "lock denied: released, nothing unbound: %s", log);
	CHECK(!sim_exists("state/" GS_ATTEMPT_NAME) && sim_exists(ADA_DEV "0000:03:00.0/driver"), "lock denied: no guard, PF bound");
	free(log);
	free(out);

	/* A PF driver_override other than nvidia would make the re-probe's bind fail: refused first. */
	ada_host("ada-override", root, sizeof(root));
	sim_writef(ADA_DEV "0000:03:00.0/driver_override", "pci-pf-stub\n");
	rc = run_ada(child_activate, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "driver_override=pci-pf-stub"), "driver_override: %d\n%s", rc, out);
	check_ada_untouched("driver_override");
	free(out);

	/* An unselected GPU with VFs: the daemon restart would re-register them unpowered. Refused. */
	{
		static const struct fx_fn other_04[] = {
			{ "0000:04:00.0", "2206", "030000", "nvidia" },
			{ "0000:04:00.4", "2206", "030000", "nvidia" },
			{ NULL, NULL, NULL, NULL },
		};

		ada_host("ada-foreign", root, sizeof(root));
		make_card(root, &topo_display, other_04, 0, "1/1");
		gs_pathf(p, sizeof(p), "%s/devices/%s/0000:04:00.4/physfn", root, topo_display.bridge_path);
		link_to("../0000:04:00.0", p);
		rc = run_ada(child_activate, &cfg, &o, &out);
		CHECK(rc == GS_EXIT_REFUSED && contains(out, "VF 0000:04:00.4 of 0000:04:00.0") &&
		      contains(out, "that GPU is not selected"), "foreign VFs: %d\n%s", rc, out);
		check_ada_untouched("foreign VFs");
		free(out);
	}

	/* nvidia-vgpud does not start: no GPU is touched. */
	ada_host("ada-vgpud", root, sizeof(root));
	sim.systemctl_rc = 1;
	rc = run_ada(child_activate, &cfg, &o, &out);
	log = sim_text("sim.log");
	CHECK(rc == GS_EXIT_FAIL && !contains(log, "unbindLock") && !contains(log, "nvidia-vgpu-mgr"), "vgpud fails: %d\n%s",
	      rc, log);
	free(log);
	free(out);

	/* The PF does not come back to nvidia: the guard stays, no VF is powered, nothing rescans. */
	ada_host("ada-lost", root, sizeof(root));
	sim.fail = "drivers/nvidia/bind";
	rc = run_ada(child_activate, &cfg, &o, &out);
	log = sim_text("sim.log");
	CHECK(rc == GS_EXIT_COLD_CYCLE && sim_exists("state/" GS_ATTEMPT_NAME), "PF lost: %d\n%s", rc, out);
	CHECK(!contains(log, "power/control") && !contains(log, "rescan"), "PF lost: no power write, no rescan: %s", log);
	CHECK(contains(out, "refuses (exit 5). Power the machine off and on, then run `geforce-sriov prepare --clear-guard`") &&
	      !contains(out, "until an activate") && sim_exists("run/" GS_ACTIVATING_NAME),
	      "PF lost: the stop message names the guard's recovery and the record stays: %s", out);
	free(log);
	free(out);

	/* The guard's state cannot be read after the re-probe: it counts as set, not as gone. */
	ada_host("ada-guard-unknown", root, sizeof(root));
	sim.state_not_dir = 1;
	rc = run_ada(child_activate, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_FAIL && contains(out, "cannot be told (Not a directory); treat it as set") &&
	      contains(out, "stays and the guard may be set (Not a directory), so `geforce-sriov check` reports NOT READY.\n"
			    "Treat the guard as set: power the machine off and on, then run `geforce-sriov prepare --clear-guard`") &&
	      !contains(out, "until an activate") && sim_exists("run/" GS_ACTIVATING_NAME),
	      "guard unknown: the stop message counts it as set and the record stays: %d\n%s", rc, out);
	free(out);

	/*
	 * nvidia_vgpu_vfio is not loaded and cannot be: activate tries modprobe, refuses when it fails,
	 * and changes nothing else (no daemon restart, no re-probe, no write).
	 */
	ada_host("ada-novfio", root, sizeof(root));
	gs_pathf(p, sizeof(p), "%s/module/nvidia_vgpu_vfio", root);
	cleanup(p);
	sim.modprobe_fail = 1;
	rc = run_ada(child_activate, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "modprobe nvidia_vgpu_vfio exited with"), "no vfio module: %d\n%s", rc, out);
	log = sim_text("sim.log");
	CHECK(!contains(log, "W "), "no vfio module: no write: %s", log ? log : "");
	CHECK(!sim_exists("state/" GS_ATTEMPT_NAME), "no vfio module: no guard");
	free(log);
	free(out);

	/* The same host with the module loadable: activate loads it and goes on. */
	ada_host("ada-loadvfio", root, sizeof(root));
	gs_pathf(p, sizeof(p), "%s/module/nvidia_vgpu_vfio", root);
	cleanup(p);
	rc = run_ada(child_activate, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_OK && sim_exists("module/nvidia_vgpu_vfio/refcnt") &&
	      contains(out, "modprobe nvidia_vgpu_vfio"),
	      "vfio loads: %d\n%s", rc, out);
	free(out);

	/* --boot waits for the PF to bind to nvidia, then lets the checks below decide. */
	{
		struct gs_activate_opts saved = o;

		ada_host("ada-bootlate", root, sizeof(root));
		gs_pathf(p, sizeof(p), "%s/" ADA_DEV "0000:03:00.0/driver", root);
		cleanup(p);
		memset(&o, 0, sizeof(o));
		o.boot = 1;
		o.boot_wait_ms = 50;
		rc = run_ada(child_activate, &cfg, &o, &out);
		CHECK(rc == GS_EXIT_REFUSED && contains(out, "still bound to nothing after 50 ms"),
		      "--boot wait: %d\n%s", rc, out);
		CHECK(contains(out, "is bound to nothing, not nvidia"), "--boot leaves the decision to the checks: %s", out);
		check_ada_untouched("boot wait");
		free(out);

		/* The case the wait exists for: the PF binds while activate is waiting. */
		ada_host("ada-bootbind", root, sizeof(root));
		gs_pathf(p, sizeof(p), "%s/" ADA_DEV "0000:03:00.0/driver", root);
		gs_pathf(q, sizeof(q), "%s/" ADA_DEV "0000:03:00.0/driver", root);
		cleanup(p);
		{
			pid_t pid;

			pid = fork();
			if (pid == 0) {
				gs_sleep_ms(30);
				link_to("../../../../bus/pci/drivers/nvidia", q);
				_exit(0);
			}
			o.boot = 1;
			o.boot_wait_ms = 2000;
			rc = run_ada(child_activate, &cfg, &o, &out);
			if (pid > 0)
				while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
					;
		}
		CHECK(rc == GS_EXIT_OK && !contains(out, "still bound"), "bind during the wait: %d\n%s", rc, out);
		free(out);
		o = saved;
	}

	/* The plugin gate fails closed: an unknown file, a missing one and a non-regular one are refused. */
	ada_host("ada-plugin", root, sizeof(root));
	gs_pathf(p, sizeof(p), "%s/libnvidia-vgpu.so", root);
	writef(p, "not NVIDIA's plugin\n");
	o.plugin = p;
	rc = run_ada(child_activate, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "not the patched plugin copy of NVIDIA's 580.159.01") &&
	      contains(out, "SHA-256 aa4fdeb5288b5e56e7d4972466813d78e10cfb428fcf62fb53b84fec718cf07c") &&
	      !contains(out, "7ddfad458bbfdb0c4d6902a9e045bd15c84f63aea19314f3d3a60967224b73c6"),
	      "unknown plugin: names the patched plugin copy's digest: %d\n%s", rc, out);
	check_ada_untouched("unknown plugin");
	free(out);
	/* The documented layout: libnvidia-vgpu.so links to the versioned copy, which is hashed through the link. */
	gs_pathf(q, sizeof(q), "%s/libnvidia-vgpu.so.580.159.01", root);
	writef(q, "not NVIDIA's plugin\n");
	link_to("libnvidia-vgpu.so.580.159.01", p);
	gs_sha256_buf("not NVIDIA's plugin\n", 20, sha);
	snprintf(want, sizeof(want), "libnvidia-vgpu.so.580.159.01) has SHA-256 %s: not the patched plugin copy", sha);
	rc = run_ada(child_activate, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, want) && !contains(out, "cannot read"),
	      "plugin link to the versioned copy: hashed through the link: %d\n%s", rc, out);
	check_ada_untouched("plugin link");
	free(out);
	/*
	 * Acceptance and the stock refusal need NVIDIA's real digests, which no test file has, so the two
	 * halves of check_plugin are covered separately: hashing through the documented link returns the
	 * target's digest and resolved path (test_sha256, and the link case just above), and
	 * gs_plugin_verdict accepts the patched plugin copy's digest and refuses the copy with plugin-mixed only (naming
	 * `install`), the stock, the earlier Ada-only and an unknown one.
	 * Their composition on the real plugin was exercised on hardware by the live run, not by this test.
	 */
	{
		const char *vlnk = "/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so";
		const char *vreal = "/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so.580.159.01";
		char vwhy[1024] = "";

		rc = gs_plugin_verdict(vlnk, vreal, "aa4fdeb5288b5e56e7d4972466813d78e10cfb428fcf62fb53b84fec718cf07c", vwhy,
				       sizeof(vwhy));
		CHECK(rc == 0 && vwhy[0] == '\0', "plugin verdict: the patched plugin copy's digest is accepted: %d %s", rc, vwhy);
		rc = gs_plugin_verdict(vlnk, vreal, "7ddfad458bbfdb0c4d6902a9e045bd15c84f63aea19314f3d3a60967224b73c6", vwhy,
				       sizeof(vwhy));
		CHECK(rc == -1 && contains(vwhy, "libnvidia-vgpu.so (/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so.580.159.01) is an "
						 "earlier copy without the Ada framebuffer change. Run\n`sudo geforce-sriov install` "
						 "again to replace it with the patched plugin copy, SHA-256 "
						 "aa4fdeb5288b5e56e7d4972466813d78e10cfb428fcf62fb53b84fec718cf07c"),
		      "plugin verdict: the copy with plugin-mixed only is refused, naming install: %d %s", rc, vwhy);
		rc = gs_plugin_verdict(vlnk, vreal, "7e52b76dc7ca8490e7ffbc1fd724f5db07f3791a3a2ec07f2d595bf05abdcaa7", vwhy,
				       sizeof(vwhy));
		CHECK(rc == -1 && contains(vwhy, "libnvidia-vgpu.so is NVIDIA's unchanged 580.159.01 plugin "
						 "(/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so.580.159.01); Ada and Ampere need its "
						 "patched plugin copy,\nSHA-256 aa4fdeb5"),
		      "plugin verdict: the stock digest is refused as stock, naming the resolved copy: %d %s", rc, vwhy);
		rc = gs_plugin_verdict(vlnk, vreal, "097f571588f3a232265103b73e38ba111ddb44ef329ac720562cff71e6170d37", vwhy,
				       sizeof(vwhy));
		CHECK(rc == -1 && contains(vwhy, "is the earlier Ada-only copy; replace it with the patched plugin copy,\nSHA-256 aa4fdeb5"),
		      "plugin verdict: the earlier Ada-only digest is refused: %d %s", rc, vwhy);
		snprintf(want, sizeof(want), "libnvidia-vgpu.so.580.159.01) has SHA-256 %s: not the patched plugin copy", sha);
		rc = gs_plugin_verdict(vlnk, vreal, sha, vwhy, sizeof(vwhy));
		CHECK(rc == -1 && contains(vwhy, want), "plugin verdict: an unknown digest is refused: %d %s", rc, vwhy);
	}
	/* A link whose target is missing, a directory or a FIFO: each refusal names the path and its reason. */
	link_to("libnvidia-vgpu.so.595.91.04", p);
	snprintf(want, sizeof(want), "cannot read the vGPU plugin %s (from NVIDIA's host package): No such file or directory", p);
	rc = run_ada(child_activate, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, want), "dangling plugin link: %d\n%s", rc, out);
	check_ada_untouched("dangling plugin link");
	free(out);
	gs_pathf(q, sizeof(q), "%s/plugin-dir", root);
	if (gs_mkdir_p(q, 0755))
		die(q);
	link_to("plugin-dir", p);
	snprintf(want, sizeof(want), "cannot read the vGPU plugin %s (from NVIDIA's host package): Is a directory", p);
	rc = run_ada(child_activate, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, want), "plugin link to a directory: %d\n%s", rc, out);
	check_ada_untouched("plugin link to a directory");
	free(out);
	gs_pathf(q, sizeof(q), "%s/plugin-fifo", root);
	if (mkfifo(q, 0644))
		die(q);
	link_to("plugin-fifo", p);
	snprintf(want, sizeof(want), "cannot read the vGPU plugin %s (from NVIDIA's host package): Invalid argument", p);
	rc = run_ada(child_activate, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, want), "plugin link to a FIFO: %d\n%s", rc, out);
	check_ada_untouched("plugin link to a FIFO");
	free(out);
	gs_pathf(p, sizeof(p), "%s/no-such-libnvidia-vgpu.so", root);
	snprintf(want, sizeof(want), "cannot read the vGPU plugin %s (from NVIDIA's host package): No such file or directory", p);
	rc = run_ada(child_activate, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, want), "missing plugin: %d\n%s", rc, out);
	check_ada_untouched("missing plugin");
	free(out);
	o.plugin = NULL;

	/* An activate that did not finish leaves its record: check fails closed. */
	ada_host("ada-record", root, sizeof(root));
	sim_writef("run/" GS_ACTIVATING_NAME, "boot_id=x\npfs=0000:03:00.0\n");
	rc = run_ada(child_check, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_FAIL && contains(out, "NOT READY: ") && contains(out, GS_ACTIVATING_NAME), "record: %d\n%s", rc, out);
	free(out);

	/* An activate or a preparation that starts while check reads the VF: the readings do not count. */
	{
		static const char *const marker[] = { "run/" GS_ACTIVATING_NAME, "state/" GS_ATTEMPT_NAME };

		for (size_t k = 0; k < sizeof(marker) / sizeof(marker[0]); k++) {
			ada_host(k ? "ada-attempt-mid" : "ada-record-mid", root, sizeof(root));
			sim_writef(ADA_DEV "0000:03:00.4/nvidia/current_vgpu_type", "942\n");
			gs_pathf(p, sizeof(p), "%s/%s", root, marker[k]);
			rc = check_marker_mid_read(&cfg, &o, marker[k], &out);
			CHECK(rc == GS_EXIT_FAIL && contains(out, "OK 0000:03:00.4") && contains(out, "appeared during the check") &&
			      !contains(out, "exists:") && contains(out, p) && sim_exists(marker[k]), "%s appears mid-read: %d\n%s",
			      marker[k], rc, out);
			free(out);
		}
	}

	/* Two VFs: every VF is checked by the same rules, before activate and after it. */
	{
		static const struct fx_fn vf1[] = {
			{ "0000:03:00.5", "2783", "030000", "nvidia" },
			{ NULL, NULL, NULL, NULL },
		};

		ada_host("ada-check-two-vfs", root, sizeof(root));
		make_card(root, &topo_default, vf1, 0, NULL);
		gs_pathf(p, sizeof(p), "%s/" ADA_DEV "0000:03:00.5/physfn", root);
		link_to("../0000:03:00.0", p);
		gs_pathf(p, sizeof(p), "%s/" ADA_DEV "0000:03:00.0/virtfn1", root);
		link_to("../0000:03:00.5", p);
		sim_writef(ADA_DEV "0000:03:00.0/sriov_totalvfs", "2\n");
		sim_writef(ADA_DEV "0000:03:00.0/sriov_numvfs", "2\n");
		sim_writef(ADA_DEV "0000:03:00.5/power/control", "on\n");
		sim_writef(ADA_DEV "0000:03:00.5/power/runtime_status", "active\n");
		sim_writef(ADA_DEV "0000:03:00.5/nvidia/current_vgpu_type", "0\n");
		memset(&o, 0, sizeof(o));
		rc = run_ada(child_check, &cfg, &o, &out);
		CHECK(rc == GS_EXIT_FAIL && contains(out, "NOT READY 0000:03:00.4 (VF of 0000:03:00.0): driver=nvidia type=0 expected=942") &&
		      contains(out, "NOT READY 0000:03:00.5 (VF of 0000:03:00.0): driver=nvidia type=0 expected=942"),
		      "check with two VFs before activate: both NOT READY on the type: %d\n%s", rc, out);
		check_ada_untouched("check with two VFs");
		free(out);

		/* --type on a card with two VFs: accepted, written to every VF, then each powered. */
		o.type = 942;
		rc = run_ada(child_activate, &cfg, &o, &out);
		log = sim_text("sim.log");
		CHECK(rc == GS_EXIT_OK && !contains(out, "refused") &&
		      contains(log, "W bus/pci/devices/0000:03:00.4/nvidia/current_vgpu_type 942\n") &&
		      contains(log, "W bus/pci/devices/0000:03:00.5/nvidia/current_vgpu_type 942\n") &&
		      contains(log, "W bus/pci/devices/0000:03:00.5/power/control on\n") &&
		      pos_of(log, "W bus/pci/devices/0000:03:00.5/nvidia/current_vgpu_type 942\n") <
		      pos_of(log, "W bus/pci/devices/0000:03:00.5/power/control on\n"),
		      "activate --type with two VFs: %d\n%s\n%s", rc, out, log ? log : "");
		{
			static const char *const two_vfs[] = { "0000:03:00.4", "0000:03:00.5" };

			for (size_t k = 0; k < sizeof(two_vfs) / sizeof(two_vfs[0]); k++) {
				gs_pathf(p, sizeof(p), ADA_DEV "%s/nvidia/current_vgpu_type", two_vfs[k]);
				t = sim_text(p);
				CHECK(t && strncmp(t, "942", 3) == 0 && (t[3] == '\0' || t[3] == '\n'), "%s type 942: %s",
				      two_vfs[k], t ? t : "(none)");
				free(t);
			}
		}
		free(log);
		free(out);
		rc = run_ada(child_check, &cfg, &o, &out);
		CHECK(rc == GS_EXIT_OK && contains(out, "OK 0000:03:00.4") && contains(out, "OK 0000:03:00.5"),
		      "check --type 942 after activate with two VFs: %d\n%s", rc, out);
		free(out);
		o.type = 0;
	}

	/* check names a card that is not in the device table (a Blackwell GB202): refused. */
	ada_host("ada-check-unlisted", root, sizeof(root));
	sim_writef(ADA_DEV "0000:03:00.0/device", "0x2b85\n");
	o.ndevices = 1;
	gs_addr_parse("0000:03:00.0", &o.devices[0]);
	rc = run_ada(child_check, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "0000:03:00.0 (10de:2b85) is not in the device table"),
	      "check on an unlisted card: %d\n%s", rc, out);
	free(out);
}

/* VFs 4 to 7 of the card at k on the simulated host: bound to nvidia, on/active, type 0, linked to and from the PF. */
static void sim_add_vfs(const struct fx_topo *k, const char *device)
{
	char bdf[GS_BDF_STR_SIZE], p[GS_PATH_MAX], q[GS_PATH_MAX];
	int stem = (int)strlen(k->pf) - 1;

	for (unsigned int i = 0; i < 4; i++) {
		snprintf(bdf, sizeof(bdf), "%.*s%u", stem, k->pf, 4 + i);
		{
			const struct fx_fn vf[] = { { bdf, device, "030000", "nvidia" }, { NULL, NULL, NULL, NULL } };

			make_card(sim.root, k, vf, 0, NULL);
		}
		gs_pathf(p, sizeof(p), "%s/devices/%s/%s/physfn", sim.root, k->bridge_path, bdf);
		gs_pathf(q, sizeof(q), "../%s", k->pf);
		link_to(q, p);
		gs_pathf(p, sizeof(p), "%s/devices/%s/%s/virtfn%u", sim.root, k->bridge_path, k->pf, i);
		gs_pathf(q, sizeof(q), "../%s", bdf);
		link_to(q, p);
		gs_pathf(p, sizeof(p), "devices/%s/%s/power/control", k->bridge_path, bdf);
		sim_writef(p, "on\n");
		gs_pathf(p, sizeof(p), "devices/%s/%s/power/runtime_status", k->bridge_path, bdf);
		sim_writef(p, "active\n");
		gs_pathf(p, sizeof(p), "devices/%s/%s/nvidia/current_vgpu_type", k->bridge_path, bdf);
		sim_writef(p, "0\n");
	}
	gs_pathf(p, sizeof(p), "devices/%s/%s/sriov_totalvfs", k->bridge_path, k->pf);
	sim_writef(p, "4\n");
	gs_pathf(p, sizeof(p), "devices/%s/%s/sriov_numvfs", k->bridge_path, k->pf);
	sim_writef(p, "4\n");
}

/*
 * One Ada and one Ampere card, 4 VFs each, on the one host stack: the Ada card of ada_host at
 * 0000:03:00.0 and an RTX 3080 at 0000:04:00.0 that is the boot display, prepared this boot with
 * `prepare --release-console=0000:04:00.0` under the config's grant.
 */
static void mixed_host(const char *name, char *root, size_t len, struct gs_config *cfg)
{
	static const struct fx_fn ampere_pf[] = {
		{ "0000:04:00.0", "2206", "030000", "nvidia" },
		{ NULL, NULL, NULL, NULL },
	};
	char p[GS_PATH_MAX], boot[64] = "";

	ada_host(name, root, len);
	sim_add_vfs(&topo_default, "2783");
	make_card(root, &topo_display, ampere_pf, 1, NULL);
	sim_add_vfs(&topo_display, "2206");
	sim_writef("proc/driver/nvidia/gpus/0000:04:00.0/unbindLock", "0\n");
	gs_boot_id(boot, sizeof(boot));
	gs_pathf(p, sizeof(p), "%s/run/" GS_PREPARED_NAME "/0000:04:00.0", root);
	writef(p, "boot_id=%s\ntotalvfs=4\nari_enabled=1\nconsole_release=simple-framebuffer.0\n", boot);
	cfg_of(cfg, "bdfs=0000:03:00.0,0000:04:00.0\nboot_display_release=0000:04:00.0\n");
}

static void test_activate_mixed(void)
{
	static const struct {
		const char *pf, *vf, *type, *other;
	} vfs[] = {
		{ "0000:03:00.0", "0000:03:00.4", "942", "525" }, { "0000:03:00.0", "0000:03:00.5", "942", "525" },
		{ "0000:03:00.0", "0000:03:00.6", "942", "525" }, { "0000:03:00.0", "0000:03:00.7", "942", "525" },
		{ "0000:04:00.0", "0000:04:00.4", "525", "942" }, { "0000:04:00.0", "0000:04:00.5", "525", "942" },
		{ "0000:04:00.0", "0000:04:00.6", "525", "942" }, { "0000:04:00.0", "0000:04:00.7", "525", "942" },
	};
	char root[GS_PATH_MAX], p[GS_PATH_MAX], want[256], *out, *log, *t;
	struct gs_activate_opts o;
	struct gs_config cfg;
	unsigned int powered = 0;
	int rc;

	/* --dry-run: each card's generation default. Nothing changes. */
	mixed_host("mixed-dry", root, sizeof(root), &cfg);
	memset(&o, 0, sizeof(o));
	o.dry_run = 1;
	rc = run_ada(child_activate, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_OK && contains(out, "0000:03:00.7: echo 942 > nvidia/current_vgpu_type") &&
	      contains(out, "0000:04:00.7: echo 525 > nvidia/current_vgpu_type"), "mixed dry run, per-generation types: %d\n%s",
	      rc, out);
	check_ada_untouched("mixed dry run");
	free(out);

	/* --type on Ada with Ampere: no type is creatable on both targets, so activate and check refuse it. */
	o.dry_run = 0;
	o.type = 942;
	rc = run_ada(child_activate, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_REFUSED &&
	      contains(out, "geforce-sriov: refused, nothing changed: --type 942 would set one vGPU type on Ada (0000:03:00.0) "
			    "and Ampere (0000:04:00.0)\ncards, and no type fits both (942 is not "
			    "creatable on the Ampere target, 525 not on the Ada target). Omit --type\nfor the per-generation "
			    "defaults (942 Ada, 525 Ampere), or, for check, name one generation's GPUs with --device.\n"
			    "Set type_ada= and type_ampere= in ") &&
	      contains(out, " to choose one per generation\n"),
	      "mixed activate --type 942: refused, the config keys named: %d\n%s", rc, out);
	check_ada_untouched("mixed activate --type");
	free(out);
	rc = run_ada(child_check, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "--type 942 would set one vGPU type on Ada") &&
	      !contains(out, "NOT READY"), "mixed check --type 942: refused: %d\n%s", rc, out);
	check_ada_untouched("mixed check --type");
	free(out);

	/* --device narrows check to the Ada card: --type 941 is checked, not refused (the VFs have no type yet). */
	o.type = 941;
	o.ndevices = 1;
	gs_addr_parse("0000:03:00.0", &o.devices[0]);
	rc = run_ada(child_check, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_FAIL && contains(out, "type=0 expected=941") && !contains(out, "0000:04:00.") &&
	      !contains(out, "sets one vGPU type"), "mixed check --device Ada --type 941: %d\n%s", rc, out);
	free(out);
	/* Only the Ada card selected (bdfs): --type 941 passes the dry run. */
	ada_host("ada-only-type", root, sizeof(root));
	cfg_of(&cfg, "bdfs=0000:03:00.0\n");
	o.ndevices = 0;
	o.dry_run = 1;
	rc = run_ada(child_activate, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_OK && contains(out, "0000:03:00.4: echo 941 > nvidia/current_vgpu_type") &&
	      !contains(out, "echo 942") && !contains(out, "refused"), "Ada only, --type 941 dry run: %d\n%s", rc,
	      out);
	check_ada_untouched("Ada only --type dry run");
	free(out);

	/* activate: both PFs re-probed, 942 on the Ada VFs, 525 on the Ampere VFs, all eight powered. */
	mixed_host("mixed-ok", root, sizeof(root), &cfg);
	memset(&o, 0, sizeof(o));
	rc = run_ada(child_activate, &cfg, &o, &out);
	log = sim_text("sim.log");
	CHECK(rc == GS_EXIT_OK && contains(log, "W bus/pci/drivers/nvidia/bind 0000:03:00.0\n") &&
	      contains(log, "W bus/pci/drivers/nvidia/bind 0000:04:00.0\n"), "mixed activate, both re-probed: %d\n%s\n%s", rc,
	      out, log ? log : "");
	/*
	 * Measured on a two-card host: re-probing one PF left the other card's VFs at auto/suspended. So no
	 * power/control write for either card's functions may come before the last PF unbind or bind of either.
	 */
	{
		static const char *const reprobe[] = {
			"W bus/pci/drivers/nvidia/unbind 0000:03:00.0\n", "W bus/pci/drivers/nvidia/bind 0000:03:00.0\n",
			"W bus/pci/drivers/nvidia/unbind 0000:04:00.0\n", "W bus/pci/drivers/nvidia/bind 0000:04:00.0\n",
		};
		long last_reprobe = -1, first_power = -1;
		int all_reprobed = 1;

		for (size_t k = 0; k < sizeof(reprobe) / sizeof(reprobe[0]); k++)
			all_reprobed &= contains(log, reprobe[k]);
		for (const char *line = log, *eol = NULL; line && *line; line = eol ? eol + 1 : NULL) {
			const char *pc = strstr(line, "/power/control "), *a = strstr(line, "0000:03:00."),
				   *b = strstr(line, "0000:04:00.");

			eol = strchr(line, '\n');
			for (size_t k = 0; k < sizeof(reprobe) / sizeof(reprobe[0]); k++)
				if (strncmp(line, reprobe[k], strlen(reprobe[k])) == 0)
					last_reprobe = line - log;
			if (first_power < 0 && strncmp(line, "W ", 2) == 0 && pc && (!eol || pc < eol) &&
			    ((a && a < pc) || (b && b < pc)))
				first_power = line - log;
		}
		CHECK(all_reprobed && first_power > last_reprobe,
		      "mixed activate: every PF unbind and bind before the first power/control write (last re-probe at %ld, "
		      "first power/control at %ld): %s", last_reprobe, first_power, log ? log : "");
	}
	for (size_t k = 0; k < sizeof(vfs) / sizeof(vfs[0]); k++) {
		char type_w[128], other_w[128], power_w[128];

		snprintf(type_w, sizeof(type_w), "W bus/pci/devices/%s/nvidia/current_vgpu_type %s\n", vfs[k].vf, vfs[k].type);
		snprintf(other_w, sizeof(other_w), "W bus/pci/devices/%s/nvidia/current_vgpu_type %s\n", vfs[k].vf, vfs[k].other);
		snprintf(power_w, sizeof(power_w), "W bus/pci/devices/%s/power/control on\n", vfs[k].vf);
		CHECK(contains(log, type_w) && !contains(log, other_w) && contains(log, power_w) &&
		      pos_of(log, type_w) < pos_of(log, power_w), "mixed activate: %s gets type %s, then power: %s", vfs[k].vf,
		      vfs[k].type, log ? log : "");
		gs_pathf(p, sizeof(p), "devices/%s/%s/nvidia/current_vgpu_type",
			 strcmp(vfs[k].pf, "0000:03:00.0") == 0 ? topo_default.bridge_path : topo_display.bridge_path, vfs[k].vf);
		t = sim_text(p);
		CHECK(t && strncmp(t, vfs[k].type, 3) == 0 && (t[3] == '\0' || t[3] == '\n'), "%s reads type %s: %s", vfs[k].vf,
		      vfs[k].type, t ? t : "(none)");
		free(t);
		gs_pathf(p, sizeof(p), "devices/%s/%s/power/control",
			 strcmp(vfs[k].pf, "0000:03:00.0") == 0 ? topo_default.bridge_path : topo_display.bridge_path, vfs[k].vf);
		t = sim_text(p);
		powered += t && strcmp(t, "on\n") == 0;
		free(t);
	}
	CHECK(powered == 8, "mixed activate powers all eight VFs: %u", powered);
	free(log);
	free(out);

	/* check over both cards: one OK line per VF, each with its card's type. */
	rc = run_ada(child_check, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_OK && !contains(out, "NOT READY"), "mixed check: %d\n%s", rc, out);
	for (size_t k = 0; k < sizeof(vfs) / sizeof(vfs[0]); k++) {
		snprintf(want, sizeof(want), "OK %s (VF of %s): driver=nvidia type=%s expected=%s power/control=on runtime_status=active\n",
			 vfs[k].vf, vfs[k].pf, vfs[k].type, vfs[k].type);
		CHECK(contains(out, want), "mixed check reports %s: %s", vfs[k].vf, out);
	}
	free(out);

	/* A selected card that is not in the device table (a Blackwell GB202) next to them: refused, nothing changes. */
	mixed_host("mixed-unlisted", root, sizeof(root), &cfg);
	sim_writef("devices/pci0000:00/0000:00:03.0/0000:04:00.0/device", "0x2b85\n");
	rc = run_ada(child_activate, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "0000:04:00.0 (10de:2b85) is not in the device table"),
	      "mixed host with a selected unlisted card: activate refused: %d\n%s", rc, out);
	check_ada_untouched("selected unlisted card");
	free(out);
	rc = run_ada(child_check, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "0000:04:00.0 (10de:2b85) is not in the device table"),
	      "mixed host with a selected unlisted card: check refused: %d\n%s", rc, out);
	free(out);

	/* Three VFs with a vGPU on two cards: each is named, and the command clears all three. */
	mixed_host("mixed-vgpu-three", root, sizeof(root), &cfg);
	sim_writef(ADA_DEV "0000:03:00.4/nvidia/current_vgpu_type", "942\n");
	sim_writef(ADA_DEV "0000:03:00.6/nvidia/current_vgpu_type", "942\n");
	gs_pathf(p, sizeof(p), "devices/%s/0000:04:00.5/nvidia/current_vgpu_type", topo_display.bridge_path);
	sim_writef(p, "527\n");
	rc = run_ada(child_activate, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_REFUSED &&
	      contains(out, "geforce-sriov: refused, nothing changed: each VF below has a vGPU, and restarting NVIDIA's vGPU "
			    "daemons would end it:\n"
			    "  VF 0000:03:00.4 has vGPU type 942\n"
			    "  VF 0000:03:00.6 has vGPU type 942\n"
			    "  VF 0000:04:00.5 has vGPU type 527\n"
			    "Stop the VMs that use them, then write 0 to each nvidia/current_vgpu_type, for example:\n"
			    "  for f in 0000:03:00.4 0000:03:00.6 0000:04:00.5; do echo 0 > "
			    "/sys/bus/pci/devices/$f/nvidia/current_vgpu_type; done\n") &&
	      !contains(out, "more\n"),
	      "three VFs with a vGPU: all three named, one command: %d\n%s", rc, out);
	check_ada_untouched("three VFs with a vGPU");
	free(out);

	/* Ten: the first eight listed, the other two counted, and the command names all ten. */
	{
		static const char *const extra[] = { "0000:03:01.0", "0000:03:01.1" };

		mixed_host("mixed-vgpu-ten", root, sizeof(root), &cfg);
		for (size_t k = 0; k < sizeof(vfs) / sizeof(vfs[0]); k++) {
			gs_pathf(p, sizeof(p), "devices/%s/%s/nvidia/current_vgpu_type",
				 strcmp(vfs[k].pf, "0000:03:00.0") == 0 ? topo_default.bridge_path : topo_display.bridge_path,
				 vfs[k].vf);
			snprintf(want, sizeof(want), "%s\n", vfs[k].type);
			sim_writef(p, want);
		}
		for (size_t k = 0; k < sizeof(extra) / sizeof(extra[0]); k++) {
			const struct fx_fn vf[] = { { extra[k], "2783", "030000", "nvidia" }, { NULL, NULL, NULL, NULL } };

			make_card(root, &topo_default, vf, 0, NULL);
			gs_pathf(p, sizeof(p), "%s/" ADA_DEV "%s/physfn", root, extra[k]);
			link_to("../0000:03:00.0", p);
			gs_pathf(p, sizeof(p), ADA_DEV "%s/nvidia/current_vgpu_type", extra[k]);
			sim_writef(p, "942\n");
		}
		rc = run_ada(child_activate, &cfg, &o, &out);
		CHECK(rc == GS_EXIT_REFUSED &&
		      contains(out, "would end it:\n"
				    "  VF 0000:03:00.4 has vGPU type 942\n"
				    "  VF 0000:03:00.5 has vGPU type 942\n"
				    "  VF 0000:03:00.6 has vGPU type 942\n"
				    "  VF 0000:03:00.7 has vGPU type 942\n"
				    "  VF 0000:03:01.0 has vGPU type 942\n"
				    "  VF 0000:03:01.1 has vGPU type 942\n"
				    "  VF 0000:04:00.4 has vGPU type 525\n"
				    "  VF 0000:04:00.5 has vGPU type 525\n"
				    "  and 2 more\n"
				    "Stop the VMs that use them, then write 0 to each nvidia/current_vgpu_type, for example:\n"
				    "  for f in 0000:03:00.4 0000:03:00.5 0000:03:00.6 0000:03:00.7 0000:03:01.0 0000:03:01.1 "
				    "0000:04:00.4 0000:04:00.5 0000:04:00.6 0000:04:00.7; do echo 0 > "
				    "/sys/bus/pci/devices/$f/nvidia/current_vgpu_type; done\n") &&
		      !contains(out, "VF 0000:04:00.6 has") && !contains(out, "VF 0000:04:00.7 has"),
		      "ten VFs with a vGPU: eight listed, two counted, all ten in the command: %d\n%s", rc, out);
		check_ada_untouched("ten VFs with a vGPU");
		free(out);
	}
}

/* NVIDIA's vgpuConfig.xml, cut to the types the slot tests use; 9140 must not be read as 914. */
static const char slot_xml[] =
	"<vgpuConfig>\n"
	"  <vgpuType id=\"942\" name=\"NVIDIA RTX6000-Ada-2Q\" class=\"Quadro\">\n"
	"    <profileSize>0x80000000</profileSize>\n  </vgpuType>\n"
	"  <vgpuType id=\"912\" name=\"NVIDIA L4-3Q\" class=\"Quadro\">\n"
	"    <profileSize>0xc0000000</profileSize>\n  </vgpuType>\n"
	"  <vgpuType id=\"9140\" name=\"decoy\" class=\"Quadro\">\n"
	"    <profileSize>0x40000000</profileSize>\n  </vgpuType>\n"
	"  <vgpuType id=\"914\" name=\"NVIDIA L4-6Q\" class=\"Quadro\">\n"
	"    <profileSize>0x180000000</profileSize>\n  </vgpuType>\n"
	"  <vgpuType id=\"533\" name=\"NVIDIA RTXA6000-48Q\" class=\"Quadro\">\n"
	"    <profileSize>0xc00000000</profileSize>\n  </vgpuType>\n"
	"</vgpuConfig>\n";
#define SLOT_TYPES "912   : NVIDIA L4-3Q\n914   : NVIDIA L4-6Q\n942   : NVIDIA RTX6000-Ada-2Q\n533   : NVIDIA RTXA6000-48Q\n"
#define SLOT_SMI "R nvidia-smi --query-gpu=pci.bus_id,memory.total --format=csv,noheader,nounits\n"

/*
 * The Ada card of ada_host with four VFs, 0000:03:00.4 to .7, and 12282 MiB as nvidia-smi reports it.
 * xml: write slot_xml to root/vgpuConfig.xml; smi: write nvidia-smi's answer.
 */
static void slot_host(const char *name, char *root, size_t len, int xml, int smi, char *xml_path, size_t xml_len)
{
	ada_host(name, root, len);
	sim_add_vfs(&topo_default, "2783");
	sim.types = SLOT_TYPES;
	gs_pathf(xml_path, xml_len, "%s/vgpuConfig.xml", root);
	if (xml)
		writef(xml_path, "%s", slot_xml);
	if (smi)
		sim_writef("nvidia-smi.out", "00000000:03:00.0, 12282\n00000000:04:00.0, 10240\n");
}

/*
 * The type of a card: --type, else type_ada= or type_ampere= of the config, else the generation
 * default; it holds for activate (also --boot), its dry run and check. After activate, the Ada slot
 * hint (12282 MiB card, threshold 9211 MiB): 942 ends at 8064, 912's last VF at 12160, 914's VFs 2 to 4
 * at 12224 and above.
 */
static void test_activate_types(void)
{
	static const char *const vfs[] = { "0000:03:00.4", "0000:03:00.5", "0000:03:00.6", "0000:03:00.7" };
	char root[GS_PATH_MAX], xml[GS_PATH_MAX], want[256], *out, *log;
	struct gs_activate_opts o;
	struct gs_config cfg;
	int rc, typed;

	/* type_ada=912 in the config: the dry run, the boot unit's activate --boot and plain check use it. */
	slot_host("types-912", root, sizeof(root), 1, 1, xml, sizeof(xml));
	cfg_of(&cfg, "bdfs=0000:03:00.0\ntype_ada=912\n");
	memset(&o, 0, sizeof(o));
	o.vgpu_config = xml;
	o.dry_run = 1;
	rc = run_ada(child_activate, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_OK && contains(out, "0000:03:00.7: echo 912 > nvidia/current_vgpu_type") &&
	      !contains(out, "echo 942") && !contains(out, "may not start"), "dry run with type_ada=912: %d\n%s", rc, out);
	check_ada_untouched("dry run with type_ada");
	free(out);
	o.dry_run = 0;
	o.boot = 1;
	o.boot_wait_ms = 200;
	rc = run_ada(child_activate, &cfg, &o, &out);
	log = sim_text("sim.log");
	typed = 0;
	for (size_t k = 0; k < sizeof(vfs) / sizeof(vfs[0]); k++) {
		snprintf(want, sizeof(want), "W bus/pci/devices/%s/nvidia/current_vgpu_type 912\n", vfs[k]);
		typed += contains(log, want);
	}
	CHECK(rc == GS_EXIT_OK && typed == 4 && !contains(log, "current_vgpu_type 942") && contains(log, SLOT_SMI) &&
	      contains(out, "0000:03:00.0: with type 912 the VF 0000:03:00.7 may not start: its framebuffer slot ends "
			    "near the top of the card's 12282 MiB. Put VMs on the lower VFs, or choose a smaller type.\n") &&
	      !contains(out, "0000:03:00.6 may not") && !contains(out, "0000:03:00.6,") &&
	      contains(out, "OK 0000:03:00.7 (VF of 0000:03:00.0): driver=nvidia type=912 expected=912 "),
	      "activate --boot with type_ada=912: every VF 912, the last VF warned about: %d\n%s\n%s", rc, out,
	      log ? log : "");
	free(log);
	free(out);
	memset(&o, 0, sizeof(o));
	rc = run_ada(child_check, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_OK && contains(out, "OK 0000:03:00.4 (VF of 0000:03:00.0): driver=nvidia type=912 expected=912 ") &&
	      contains(out, "OK 0000:03:00.7 (VF of 0000:03:00.0): driver=nvidia type=912 expected=912 "),
	      "check without --type expects type_ada: %d\n%s", rc, out);
	free(out);
	/* The generation default no longer applies: check expects 912, so 942 on a VF is NOT READY. */
	sim_writef(ADA_DEV "0000:03:00.4/nvidia/current_vgpu_type", "942\n");
	rc = run_ada(child_check, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_FAIL && contains(out, "NOT READY 0000:03:00.4 (VF of 0000:03:00.0): driver=nvidia type=942 expected=912 "),
	      "check expects type_ada, not the default: %d\n%s", rc, out);
	free(out);

	/* The default 942: every slot ends below the threshold, no hint. */
	slot_host("types-942", root, sizeof(root), 1, 1, xml, sizeof(xml));
	cfg_of(&cfg, "bdfs=0000:03:00.0\n");
	memset(&o, 0, sizeof(o));
	o.vgpu_config = xml;
	rc = run_ada(child_activate, &cfg, &o, &out);
	log = sim_text("sim.log");
	CHECK(rc == GS_EXIT_OK && contains(log, "0000:03:00.7/nvidia/current_vgpu_type 942\n") && contains(log, SLOT_SMI) &&
	      !contains(out, "may not start"), "type 942: no slot hint: %d\n%s", rc, out);
	free(log);
	free(out);

	/* --type 914 wins over type_ada=912; VFs 2 to 4 are named in one line. */
	slot_host("types-914", root, sizeof(root), 1, 1, xml, sizeof(xml));
	cfg_of(&cfg, "bdfs=0000:03:00.0\ntype_ada=912\n");
	memset(&o, 0, sizeof(o));
	o.vgpu_config = xml;
	o.type = 914;
	rc = run_ada(child_activate, &cfg, &o, &out);
	log = sim_text("sim.log");
	CHECK(rc == GS_EXIT_OK && contains(log, "0000:03:00.4/nvidia/current_vgpu_type 914\n") &&
	      !contains(log, "current_vgpu_type 912") &&
	      contains(out, "0000:03:00.0: with type 914 the VFs 0000:03:00.5, 0000:03:00.6 and 0000:03:00.7 may not start: "
			    "their framebuffer slots end near the top of the card's 12282 MiB. Put VMs on the lower VFs, or "
			    "choose a smaller type.\n") &&
	      !contains(out, "0000:03:00.4 may not") && !contains(out, "VFs 0000:03:00.4"),
	      "--type 914 over type_ada=912: VFs 2 to 4 warned about in one line: %d\n%s", rc, out);
	free(log);
	free(out);

	/* No vgpuConfig.xml: no hint, nvidia-smi not run, exit unchanged. */
	slot_host("types-noxml", root, sizeof(root), 0, 1, xml, sizeof(xml));
	cfg_of(&cfg, "bdfs=0000:03:00.0\ntype_ada=914\n");
	memset(&o, 0, sizeof(o));
	o.vgpu_config = xml;
	rc = run_ada(child_activate, &cfg, &o, &out);
	log = sim_text("sim.log");
	CHECK(rc == GS_EXIT_OK && contains(log, "current_vgpu_type 914\n") && !contains(log, "nvidia-smi") &&
	      !contains(out, "may not start"), "missing vgpuConfig.xml: silent: %d\n%s", rc, out);
	free(log);
	free(out);

	/* No nvidia-smi: no hint, exit unchanged. */
	slot_host("types-nosmi", root, sizeof(root), 1, 0, xml, sizeof(xml));
	rc = run_ada(child_activate, &cfg, &o, &out);
	log = sim_text("sim.log");
	CHECK(rc == GS_EXIT_OK && contains(log, "current_vgpu_type 914\n") && contains(log, SLOT_SMI) &&
	      !contains(out, "may not start"), "missing nvidia-smi: silent: %d\n%s", rc, out);
	free(log);
	free(out);

	/*
	 * A mixed host without --type: each card gets its generation's key (912 Ada, 533 Ampere), check expects
	 * both, and the Ampere card gets no slot hint even with a type larger than its memory.
	 */
	mixed_host("types-mixed", root, sizeof(root), &cfg);
	sim.types = SLOT_TYPES;
	gs_pathf(xml, sizeof(xml), "%s/vgpuConfig.xml", root);
	writef(xml, "%s", slot_xml);
	sim_writef("nvidia-smi.out", "00000000:03:00.0, 12282\n00000000:04:00.0, 10240\n");
	cfg_of(&cfg, "bdfs=0000:03:00.0,0000:04:00.0\nboot_display_release=0000:04:00.0\ntype_ada=912\ntype_ampere=533\n");
	memset(&o, 0, sizeof(o));
	o.vgpu_config = xml;
	rc = run_ada(child_activate, &cfg, &o, &out);
	log = sim_text("sim.log");
	CHECK(rc == GS_EXIT_OK && contains(log, "W bus/pci/devices/0000:03:00.7/nvidia/current_vgpu_type 912\n") &&
	      contains(log, "W bus/pci/devices/0000:04:00.7/nvidia/current_vgpu_type 533\n") &&
	      contains(out, "0000:03:00.0: with type 912 the VF 0000:03:00.7 may not start") &&
	      !contains(out, "0000:04:00.0: with type") && !contains(out, "refused"),
	      "mixed host, a type per generation from the config, no hint for Ampere: %d\n%s\n%s", rc, out, log ? log : "");
	free(log);
	free(out);
	rc = run_ada(child_check, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_OK && contains(out, "OK 0000:03:00.5 (VF of 0000:03:00.0): driver=nvidia type=912 expected=912 ") &&
	      contains(out, "OK 0000:04:00.5 (VF of 0000:04:00.0): driver=nvidia type=533 expected=533 "),
	      "mixed check, a type per generation from the config: %d\n%s", rc, out);
	free(out);
	/* --type on that mixed selection: refused, naming the types the config gives without --type. */
	o.type = 942;
	rc = run_ada(child_check, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "for the per-generation defaults (912 Ada, 533 Ampere)") &&
	      contains(out, "Set type_ada= and type_ampere= in "), "mixed --type refusal names the config's types: %d\n%s", rc,
	      out);
	free(out);
}

/* gs_run_capture keeps a program's output, and kills one that runs past GS_CAPTURE_TIMEOUT_MS. */
static void test_run_capture(void)
{
	struct timespec t0, t1;
	char out[64];
	long ms;
	int rc;

	rc = gs_run_capture((const char *const[]){ "printf", "a, %s\\n", "12282", NULL }, out, sizeof(out));
	CHECK(rc == 0 && strcmp(out, "a, 12282\n") == 0, "capture: %d '%s'", rc, out);
	rc = gs_run_capture((const char *const[]){ "no-such-program-gs", NULL }, out, sizeof(out));
	CHECK(rc == -1 && out[0] == '\0', "capture of a missing program: %d", rc);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	rc = gs_run_capture((const char *const[]){ "sleep", "60", NULL }, out, sizeof(out));
	clock_gettime(CLOCK_MONOTONIC, &t1);
	ms = (long)(t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
	CHECK(rc == -1 && out[0] == '\0' && ms >= (long)GS_CAPTURE_TIMEOUT_MS - 100 && ms < (long)GS_CAPTURE_TIMEOUT_MS + 2000,
	      "a program past the deadline is killed: %d after %ld ms", rc, ms);
}

/* ---- the driver step's record against the installed nvidia.ko ----------------- */

#define BUILD_REL "6.1.0-gs-test"
#define BUILD_ID "0123456789abcdef0123456789abcdef01234567"
#define OTHER_ID "fedcba9876543210fedcba9876543210fedcba98"

/* A minimal ELF64 relocatable of the host's byte order whose one SHT_NOTE section holds the 20-byte GNU build-id hex. */
static void write_module(const char *path, const char *hex)
{
	struct fx_module {
		Elf64_Ehdr eh;
		Elf64_Word nh[3];
		char name[4];
		unsigned char id[20];
		Elf64_Shdr sh[2];
	} m;
	char dir[GS_PATH_MAX];
	FILE *f;

	memset(&m, 0, sizeof(m));
	memcpy(m.eh.e_ident, ELFMAG, SELFMAG);
	m.eh.e_ident[EI_CLASS] = ELFCLASS64;
	m.eh.e_ident[EI_DATA] = __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ ? ELFDATA2LSB : ELFDATA2MSB;
	m.eh.e_ident[EI_VERSION] = EV_CURRENT;
	m.eh.e_type = ET_REL;
	m.eh.e_machine = EM_X86_64;
	m.eh.e_version = EV_CURRENT;
	m.eh.e_ehsize = sizeof(m.eh);
	m.eh.e_shentsize = sizeof(Elf64_Shdr);
	m.eh.e_shnum = 2;
	m.eh.e_shoff = offsetof(struct fx_module, sh);
	m.nh[0] = sizeof(m.name);
	m.nh[1] = sizeof(m.id);
	m.nh[2] = NT_GNU_BUILD_ID;
	memcpy(m.name, "GNU", 4);
	for (size_t i = 0; i < sizeof(m.id); i++)
		sscanf(hex + 2 * i, "%2hhx", &m.id[i]);
	m.sh[1].sh_type = SHT_NOTE;
	m.sh[1].sh_offset = offsetof(struct fx_module, nh);
	m.sh[1].sh_size = sizeof(m.nh) + sizeof(m.name) + sizeof(m.id);
	m.sh[1].sh_addralign = 4;
	gs_pathf(dir, sizeof(dir), "%s", path);
	*strrchr(dir, '/') = '\0';
	if (gs_mkdir_p(dir, 0755) || !(f = fopen(path, "wb")))
		die(path);
	if (fwrite(&m, sizeof(m), 1, f) != 1)
		die(path);
	fclose(f);
}

/*
 * The loaded nvidia module's build-id note under root/module/nvidia, as sysfs shows it: one GNU note of the host's
 * byte order with the 20-byte build-id hex. hex "": a directory in its place, so the note cannot be read.
 */
static void write_loaded_note(const char *root, const char *hex)
{
	struct {
		Elf64_Word nh[3];
		char name[4];
		unsigned char id[20];
	} n;
	char p[GS_PATH_MAX];
	FILE *f;

	gs_pathf(p, sizeof(p), "%s/module/nvidia/notes/.note.gnu.build-id", root);
	if (!hex[0]) {
		if (gs_mkdir_p(p, 0755))
			die(p);
		return;
	}
	memset(&n, 0, sizeof(n));
	n.nh[0] = sizeof(n.name);
	n.nh[1] = sizeof(n.id);
	n.nh[2] = NT_GNU_BUILD_ID;
	memcpy(n.name, "GNU", 4);
	for (size_t i = 0; i < sizeof(n.id); i++)
		sscanf(hex + 2 * i, "%2hhx", &n.id[i]);
	writef(p, "%s", "");
	if (!(f = fopen(p, "wb")) || fwrite(&n, sizeof(n), 1, f) != 1)
		die(p);
	fclose(f);
}

/* fn (doctor or status) on a fixture in a child; returns its exit status and output. */
static int run_diag(int (*fn)(const struct gs_diag_opts *), const struct gs_diag_opts *o, char **out)
{
	int pipefd[2];
	pid_t pid;

	if (pipe(pipefd))
		die("pipe");
	pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0) {
		int rc;

		dup2(pipefd[1], 1);
		dup2(pipefd[1], 2);
		close(pipefd[0]);
		rc = fn(o);
		fflush(stdout);
		fflush(stderr);
		_exit(rc);
	}
	close(pipefd[1]);
	return collect(pipefd[0], pid, out);
}

/* doctor, status, activate and check on one Ada host: their exit statuses and output, and doctor's warnings. */
struct build_run {
	char root[GS_PATH_MAX], module[GS_PATH_MAX], record[GS_PATH_MAX];
	int doctor_rc, status_rc, activate_rc, check_rc, warn;
	char *doctor, *status, *activate, *check, *log;
};

/*
 * The Ada host with the fixture module for BUILD_REL, the record text (NULL: no record), with stock the driver
 * step's stock directory, and loaded the build-id note of the loaded nvidia module (NULL: not loaded; "": unreadable).
 */
static void build_case(const char *name, const char *record, int stock, const char *loaded, struct build_run *r)
{
	struct gs_build_src src;
	struct gs_diag_opts d;
	struct gs_activate_opts o;
	struct gs_config cfg;
	int fail;

	ada_host(name, r->root, sizeof(r->root));
	gs_pathf(r->module, sizeof(r->module), "%s/lib/modules/" BUILD_REL "/updates/dkms/nvidia.ko", r->root);
	gs_pathf(r->record, sizeof(r->record), "%s/state/" GS_DRIVER_BUILD_NAME, r->root);
	write_module(r->module, BUILD_ID);
	if (loaded)
		write_loaded_note(r->root, loaded);
	if (record)
		writef(r->record, "%s", record);
	if (stock) {
		char p[GS_PATH_MAX];

		gs_pathf(p, sizeof(p), "%s/state/stock", r->root);
		if (gs_mkdir_p(p, 0755))
			die(p);
	}
	src.record = r->record;
	src.release = BUILD_REL;
	src.module = r->module;
	memset(&d, 0, sizeof(d));
	d.sysroot = r->root;
	d.build = &src;
	gs_addr_parse("0000:03:00.0", &d.devices[0]);
	d.ndevices = 1;
	r->doctor_rc = run_diag(gs_cmd_doctor, &d, &r->doctor);
	tally_of(r->doctor, &fail, &r->warn);
	r->status_rc = run_diag(gs_cmd_status, &d, &r->status);
	cfg_of(&cfg, "bdfs=0000:03:00.0\n");
	memset(&o, 0, sizeof(o));
	o.build = &src;
	r->activate_rc = run_ada(child_activate, &cfg, &o, &r->activate);
	r->log = sim_text("sim.log");
	r->check_rc = run_ada(child_check, &cfg, &o, &r->check);
}

static void build_free(struct build_run *r)
{
	free(r->doctor);
	free(r->status);
	free(r->activate);
	free(r->check);
	free(r->log);
}

/* activate ran its normal order: the daemon restarts, the re-probe, then the type and power of the VF. */
static int activate_order_kept(const char *log)
{
	static const char *const steps[] = {
		"R systemctl restart nvidia-vgpud.service\n",
		"R systemctl restart nvidia-vgpu-mgr.service\n",
		"W bus/pci/drivers/nvidia/unbind 0000:03:00.0\n",
		"W bus/pci/drivers/nvidia/bind 0000:03:00.0\n",
		"W bus/pci/devices/0000:03:00.4/nvidia/current_vgpu_type 942\n",
		"W bus/pci/devices/0000:03:00.4/power/control on\n",
	};
	long prev = -1;

	for (size_t k = 0; k < sizeof(steps) / sizeof(steps[0]); k++) {
		long at = pos_of(log, steps[k]);

		if (at <= prev)
			return 0;
		prev = at;
	}
	return 1;
}

/*
 * After a kernel update DKMS installs a stock nvidia.ko and every other check activate makes still
 * passes. The driver step's record names the module it installed; doctor, status, activate and check
 * compare it with the module the kernel would load. doctor and status report a mismatch; activate
 * refuses whenever a record exists and the module is not its build, and only warns without one.
 */
static void test_driver_build(void)
{
	char want[3 * GS_PATH_MAX], other[GS_PATH_MAX], sha[65], text[512];
	struct build_run ok, bad, none, old, fresh, same, moved, noread, stock_loaded, other_loaded;
	struct gs_diag_opts d;
	struct gs_build b;
	char *out;
	int rc;

	/* The match: doctor OK, the status line says so, activate runs without a warning, check reports it. */
	gs_pathf(other, sizeof(other), "%s/build-module/nvidia.ko", tmp);
	write_module(other, BUILD_ID);
	if (gs_sha256_file(other, sha))
		die(other);
	snprintf(text, sizeof(text), "kernel=" BUILD_REL "\nmodule_sha256=%s\nmodule_build_id=" BUILD_ID "\n", sha);
	build_case("build-ok", text, 0, NULL, &ok);

	/* The reader alone, over the files build_case wrote. */
	CHECK(gs_driver_build_read(ok.record, BUILD_REL, ok.module, &b) == GS_BUILD_MATCHES && b.state == GS_BUILD_MATCHES &&
	      b.record_read && strcmp(b.found_id, BUILD_ID) == 0 && strcmp(b.rec_id, BUILD_ID) == 0 &&
	      strcmp(b.rec_kernel, BUILD_REL) == 0 && strcmp(b.rec_sha256, sha) == 0 && strcmp(b.module, ok.module) == 0,
	      "reader: the record matches the module: %d found=%s rec=%s kernel=%s", b.state, b.found_id, b.rec_id, b.rec_kernel);
	CHECK(gs_driver_build_read(ok.record, "6.2.0-gs-next", ok.module, &b) == GS_BUILD_ABSENT && b.record_read &&
	      strcmp(b.rec_kernel, BUILD_REL) == 0 && strcmp(b.found_id, BUILD_ID) == 0,
	      "reader: a record for another kernel is absent for this one: %d %s", b.state, b.rec_kernel);
	CHECK(gs_driver_build_read(ok.record, BUILD_REL, NULL, &b) == GS_BUILD_PRESENT && !b.module[0] && !b.found_id[0],
	      "reader: no module found: present, not matching: %d", b.state);
	gs_pathf(other, sizeof(other), "%s/nvidia.ko.zst", ok.root);
	writef(other, "not an ELF file\n");
	CHECK(gs_driver_build_read(ok.record, BUILD_REL, other, &b) == GS_BUILD_PRESENT && !b.found_id[0],
	      "reader: a module without a readable build-id does not match: %d %s", b.state, b.found_id);
	gs_pathf(other, sizeof(other), "%s/state/absent", ok.root);
	CHECK(gs_driver_build_read(other, BUILD_REL, ok.module, &b) == GS_BUILD_ABSENT && !b.record_read &&
	      !b.rec_kernel[0] && strcmp(b.found_id, BUILD_ID) == 0,
	      "reader: no record file: %d", b.state);

	snprintf(want, sizeof(want), "[OK  ] nvidia.ko for " BUILD_REL " is the driver step's build: %s, build-id " BUILD_ID "\n",
		 ok.module);
	CHECK(contains(ok.doctor, want), "doctor, record matches: OK line\n%s", ok.doctor);
	CHECK(contains(ok.status, " driver nvidia; nvidia.ko for " BUILD_REL ": the driver step's build\n"),
	      "status, record matches: on the device line\n%s", ok.status);
	CHECK(ok.activate_rc == GS_EXIT_OK && !contains(ok.activate, "warning: nvidia.ko") && activate_order_kept(ok.log),
	      "activate, record matches: no warning, normal order: %d\n%s\n%s", ok.activate_rc, ok.activate,
	      ok.log ? ok.log : "");
	gs_pathf(other, sizeof(other), "%s/reprobe-unmasked", tmp);
	text[0] = '\0';
	if (gs_exists(other))
		gs_read_attr(other, text, sizeof(text));
	CHECK(!gs_exists(other), "activate: SIGTERM is held back from the unbind to the bind (unmasked: %s)", text);
	CHECK(ok.check_rc == GS_EXIT_OK && contains(ok.check, "nvidia.ko for " BUILD_REL " is the driver step's build: "),
	      "check, record matches: reported: %d\n%s", ok.check_rc, ok.check);
	CHECK(!contains(ok.doctor, "the running nvidia module") && !contains(ok.status, "the running nvidia module") &&
	      !contains(ok.check, "the running nvidia module"), "nvidia not loaded: no loaded-module sentence");

	/* The loaded nvidia module is the installed build: nothing more is said. */
	snprintf(text, sizeof(text), "kernel=" BUILD_REL "\nmodule_sha256=%s\nmodule_build_id=" BUILD_ID "\n", sha);
	build_case("build-loaded-same", text, 0, BUILD_ID, &same);
	CHECK(!contains(same.doctor, "the running nvidia module") && same.warn == ok.warn &&
	      !contains(same.status, "the running nvidia module") && !contains(same.check, "the running nvidia module"),
	      "loaded module is the installed build: no sentence\n%s", same.doctor);
	CHECK(ok.status_rc == GS_EXIT_OK && same.status_rc == GS_EXIT_OK,
	      "status, the installed build or nvidia not loaded: exit 0: %d/%d\n%s", same.status_rc, ok.status_rc, same.status);
	/* The loaded module is another build than the installed driver step's build (a reinstall without a
	 * power cycle): doctor WARNs, status and check say so, and status exits 1. */
	build_case("build-loaded-other", text, 0, OTHER_ID, &moved);
	CHECK(contains(moved.doctor, "[WARN] the running nvidia module (build-id fedcba987654) is not the installed one "
				     "(0123456789ab); power the machine off and on to load it\n") &&
	      moved.warn == ok.warn + 1 && moved.doctor_rc == ok.doctor_rc,
	      "loaded module is another build: doctor WARN: %d/%d\n%s", moved.warn, ok.warn, moved.doctor);
	CHECK(contains(moved.status, " driver nvidia; nvidia.ko for " BUILD_REL ": the driver step's build; the running nvidia "
				     "module (build-id fedcba987654) is not the installed one (0123456789ab); power the machine "
				     "off and on to load it\n") && moved.status_rc == GS_EXIT_FAIL,
	      "loaded module is another build: on the status nvidia line, exit 1: %d\n%s", moved.status_rc, moved.status);
	snprintf(want, sizeof(want), "nvidia.ko for " BUILD_REL " is the driver step's build: %s, build-id " BUILD_ID "; the "
		 "running nvidia module (build-id fedcba987654) is not the installed one (0123456789ab); power the machine "
		 "off and on to load it\n", moved.module);
	CHECK(contains(moved.check, want) && !contains(moved.check, "[WARN]") && moved.check_rc == ok.check_rc,
	      "loaded module is another build: on check's module line, exit unchanged: %d\n%s", moved.check_rc, moved.check);
	CHECK(contains(moved.activate, "geforce-sriov: warning: the running nvidia module (build-id fedcba987654) is not the "
					"installed one (0123456789ab); power the machine off and on to load it\n") &&
	      moved.activate_rc == ok.activate_rc && activate_order_kept(moved.log),
	      "loaded module is another build: activate warns and continues: %d\n%s", moved.activate_rc, moved.activate);
	CHECK(!contains(ok.activate, "the running nvidia module") && !contains(same.activate, "the running nvidia module"),
	      "activate: no loaded-module warning otherwise");
	/*
	 * status: the power cycle is the only next step; activate's marker is still named, its next step is
	 * not (activate would run on the loaded module).
	 */
	{
		struct gs_build_src src = { moved.record, BUILD_REL, moved.module };
		const char *cycle = "next: power the machine off and on (the running nvidia module is not the installed one)\n";
		char run[GS_PATH_MAX], marker[GS_PATH_MAX];

		CHECK(contains(moved.status, cycle) && !contains(ok.status, cycle) && !contains(same.status, cycle),
		      "loaded module is another build: status names the power cycle\n%s", moved.status);
		gs_pathf(run, sizeof(run), "%s/run-marker", moved.root);
		gs_pathf(marker, sizeof(marker), "%s/" GS_ACTIVATING_NAME, run);
		writef(marker, "boot_id=x\n");
		memset(&d, 0, sizeof(d));
		d.sysroot = moved.root;
		d.build = &src;
		d.run_dir = run;
		gs_addr_parse("0000:03:00.0", &d.devices[0]);
		d.ndevices = 1;
		rc = run_diag(gs_cmd_status, &d, &out);
		CHECK(count_of(out, "next: ") == 1 && contains(out, cycle) &&
		      contains(out, "activate: ") && contains(out, GS_ACTIVATING_NAME " exists (activate is running or did not finish)\n") &&
		      !contains(out, "`geforce-sriov activate`"),
		      "loaded module is another build: the power cycle is the one next step, the marker named: %d\n%s", rc, out);
		free(out);
	}
	/* A note that cannot be read: no sentence. */
	build_case("build-loaded-unreadable", text, 0, "", &noread);
	CHECK(!contains(noread.doctor, "the running nvidia module") && noread.warn == ok.warn &&
	      !contains(noread.status, "the running nvidia module") && !contains(noread.check, "the running nvidia module"),
	      "unreadable note of the loaded module: no sentence\n%s", noread.doctor);
	/*
	 * The loaded module differs, but the installed nvidia.ko is not the driver step's build (no record, as
	 * before the driver step ran, or a record of another build): a cold boot would not load the driver
	 * step's build either, so no sentence.
	 */
	build_case("build-loaded-norecord", NULL, 0, OTHER_ID, &stock_loaded);
	CHECK(contains(stock_loaded.doctor, "[INFO] the driver step has not run on this host; install runs it\n") &&
	      !contains(stock_loaded.doctor, "the running nvidia module") && stock_loaded.warn == ok.warn &&
	      !contains(stock_loaded.status, "the running nvidia module") && !contains(stock_loaded.check, "the running nvidia module"),
	      "loaded module differs, no record: no sentence\n%s\n%s\n%s", stock_loaded.doctor, stock_loaded.status,
	      stock_loaded.check);
	snprintf(text, sizeof(text), "kernel=" BUILD_REL "\nmodule_sha256=%s\nmodule_build_id=" OTHER_ID "\n", sha);
	build_case("build-loaded-otherrec", text, 0, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &other_loaded);
	CHECK(contains(other_loaded.doctor, "is not the driver step's build: ") &&
	      !contains(other_loaded.doctor, "the running nvidia module") &&
	      !contains(other_loaded.status, "the running nvidia module") && !contains(other_loaded.check, "the running nvidia module"),
	      "loaded module differs, record of another build: no sentence\n%s\n%s\n%s", other_loaded.doctor,
	      other_loaded.status, other_loaded.check);

	/* The record names another build-id (a kernel update brought the stock module): doctor WARNs with both
	 * identities in one line, and activate refuses before changing anything. */
	snprintf(text, sizeof(text), "kernel=" BUILD_REL "\nmodule_sha256=%s\nmodule_build_id=" OTHER_ID "\n", sha);
	build_case("build-other", text, 0, NULL, &bad);
	snprintf(want, sizeof(want), "nvidia.ko for " BUILD_REL " is not the driver step's build: %s has build-id " BUILD_ID
		 ", %s records build-id " OTHER_ID "; rerun the driver step for this kernel "
		 "(`geforce-sriov-driver --kernel " BUILD_REL "`)", bad.module, bad.record);
	CHECK(contains(bad.doctor, "[WARN] ") && contains(bad.doctor, want) && pos_of(bad.doctor, want) >= 7 &&
	      strncmp(bad.doctor + pos_of(bad.doctor, want) - 7, "[WARN] ", 7) == 0,
	      "doctor, another build-id: one WARN line naming both\n%s", bad.doctor);
	CHECK(bad.warn == ok.warn + 1 && bad.doctor_rc == ok.doctor_rc,
	      "doctor, another build-id: one more warning, same exit: %d/%d %d/%d", bad.warn, ok.warn, bad.doctor_rc,
	      ok.doctor_rc);
	CHECK(contains(bad.status, " driver nvidia; nvidia.ko for " BUILD_REL ": not the driver step's build (build-id " BUILD_ID
		       ", recorded " OTHER_ID "), rerun the driver step\n") && bad.status_rc == ok.status_rc,
	      "status, another build-id: on the device line, same exit: %d/%d\n%s", bad.status_rc, ok.status_rc, bad.status);
	CHECK(bad.activate_rc == GS_EXIT_REFUSED &&
	      contains(bad.activate, "refused, nothing changed: nvidia.ko for " BUILD_REL " is not the driver step's build") &&
	      contains(bad.activate, "(`geforce-sriov-driver --kernel " BUILD_REL "`), or run `sudo geforce-sriov install`\n") &&
	      !contains(bad.activate, "continuing\n") && (!bad.log || !contains(bad.log, "W ")),
	      "activate, another build-id: refused, nothing written: %d\n%s\n%s", bad.activate_rc, bad.activate,
	      bad.log ? bad.log : "");
	/* activate refused, so no VF was brought up: check is not ready on its own VF gate, and still reports the build. */
	CHECK(bad.check_rc == GS_EXIT_FAIL && contains(bad.check, want),
	      "check, another build-id: reported, not ready because activate refused: %d\n%s", bad.check_rc, bad.check);

	/* No record and no install manifest, with the driver step's stock directory (left by an earlier uninstall): doctor
	 * says install runs the driver step; activate warns, check's exit unchanged. */
	build_case("build-none", NULL, 1, NULL, &none);
	CHECK(contains(none.doctor, "[INFO] the driver step has not run on this host; install runs it\n") &&
	      !contains(none.doctor, "rerun the driver step for this kernel") && none.warn == ok.warn,
	      "doctor, no record, no manifest, a stock directory: install runs it\n%s", none.doctor);
	CHECK(contains(none.status, " driver nvidia; nvidia.ko for " BUILD_REL ": no driver step record for this kernel, rerun "
		       "the driver step\n"), "status, no record\n%s", none.status);
	CHECK(none.activate_rc == GS_EXIT_OK && contains(none.activate, "geforce-sriov: warning: nvidia.ko for " BUILD_REL
							 " is not known to be the driver step's build") &&
	      activate_order_kept(none.log), "activate, no record: warns and runs: %d\n%s", none.activate_rc, none.activate);
	CHECK(none.check_rc == ok.check_rc && contains(none.check, "is not known to be the driver step's build"),
	      "check, no record: reported, exit unchanged: %d\n%s", none.check_rc, none.check);

	/* A record for another kernel release (the kernel-update case): reported, and activate refuses before any change. */
	build_case("build-old", "kernel=6.0.0-gs-old\nmodule_build_id=" BUILD_ID "\n", 0, NULL, &old);
	snprintf(want, sizeof(want), "[WARN] nvidia.ko for " BUILD_REL " is not known to be the driver step's build: %s records "
		 "kernel 6.0.0-gs-old; rerun the driver step for this kernel (`geforce-sriov-driver --kernel " BUILD_REL
		 "`)\n", old.record);
	CHECK(contains(old.doctor, want), "doctor, record for another kernel: WARN\n%s", old.doctor);
	CHECK(contains(old.status, "no driver step record for this kernel"), "status, record for another kernel\n%s", old.status);
	CHECK(old.activate_rc == GS_EXIT_REFUSED &&
	      contains(old.activate, "refused, nothing changed: nvidia.ko for " BUILD_REL " is not known to be the driver "
				     "step's build") &&
	      contains(old.activate, "(`geforce-sriov-driver --kernel " BUILD_REL "`), or run `sudo geforce-sriov install`\n") &&
	      !contains(old.activate, "continuing\n") && (!old.log || !contains(old.log, "W ")),
	      "activate, record for another kernel: refused, nothing written: %d\n%s\n%s", old.activate_rc, old.activate,
	      old.log ? old.log : "");
	CHECK(old.check_rc == GS_EXIT_FAIL, "check, record for another kernel: not ready because activate refused: %d\n%s",
	      old.check_rc, old.check);

	/* No record, no manifest and no stock directory: the driver step never ran on this host, and install runs it. */
	build_case("build-fresh", NULL, 0, NULL, &fresh);
	CHECK(contains(fresh.doctor, "[INFO] the driver step has not run on this host; install runs it\n") &&
	      !contains(fresh.doctor, "rerun the driver step for this kernel") && !contains(fresh.doctor, "install --driver=skip"),
	      "doctor, driver step never ran: install runs it\n%s", fresh.doctor);
	/* An install manifest beside the record (install ran with --driver=skip): the usual line. */
	{
		struct gs_build_src src = { fresh.record, BUILD_REL, fresh.module };

		gs_pathf(other, sizeof(other), "%s/state/" GS_MANIFEST_NAME, fresh.root);
		writef(other, "\n");
		memset(&d, 0, sizeof(d));
		d.sysroot = fresh.root;
		d.build = &src;
		gs_addr_parse("0000:03:00.0", &d.devices[0]);
		d.ndevices = 1;
		rc = run_diag(gs_cmd_doctor, &d, &out);
		snprintf(want, sizeof(want), "[WARN] nvidia.ko for " BUILD_REL " is not known to be the driver step's build: %s is "
			 "absent; rerun the driver step for this kernel (`geforce-sriov-driver --kernel " BUILD_REL "`); "
			 "if the module already is that build, `install --driver=skip` keeps it\n", fresh.record);
		CHECK(contains(out, want) && !contains(out, "install runs it"),
		      "doctor, installed with --driver=skip: the usual line: %d\n%s", rc, out);
		free(out);
	}

	/* A fixture without sources is not judged: no line, as in every other fixture test. */
	memset(&d, 0, sizeof(d));
	d.sysroot = ok.root;
	gs_addr_parse("0000:03:00.0", &d.devices[0]);
	d.ndevices = 1;
	rc = run_diag(gs_cmd_doctor, &d, &out);
	CHECK(!contains(out, "nvidia.ko for"), "doctor on a fixture without sources: no line: %d\n%s", rc, out);
	free(out);

	build_free(&ok);
	build_free(&bad);
	build_free(&none);
	build_free(&old);
	build_free(&fresh);
	build_free(&same);
	build_free(&moved);
	build_free(&noread);
	build_free(&stock_loaded);
	build_free(&other_loaded);
}

/*
 * doctor reads modprobe.d as the driver step does: another file that sets NVreg_EnableGpuFirmware to a
 * value other than 0 fails, one that sets 0 is noted; with a console release granted, nvidia must be
 * held back at boot by a `blacklist nvidia` and an `install nvidia /bin/false` line, and with an installed
 * config that grants none, a hold left in place is warned about.
 */
static void test_doctor_modprobe(void)
{
	char root[GS_PATH_MAX], cfg[GS_PATH_MAX], mpd[GS_PATH_MAX], p[GS_PATH_MAX], hold[GS_PATH_MAX], want[3 * GS_PATH_MAX];
	struct gs_diag_opts d;
	char *out;
	int rc, f0, w0, f1, w1;

	two_cards("doctor-mp", 1, FW_BOUND, root, sizeof(root));
	gs_pathf(cfg, sizeof(cfg), "%s/doctor-mp-config", tmp);
	writef(cfg, "bdfs=0000:03:00.0,0000:04:00.0\nboot_display_release=0000:04:00.0\n");
	gs_pathf(mpd, sizeof(mpd), "%s/doctor-mp-modprobe.d", tmp);
	gs_pathf(p, sizeof(p), "%s/nvidia-gsp-off.conf", mpd);
	gs_pathf(hold, sizeof(hold), "%s/nvidia-manual-load.conf", mpd);
	writef(p, "options nvidia NVreg_X=1 NVreg_EnableGpuFirmware=0\n");
	gs_pathf(want, sizeof(want), "%s/geforce-sriov-nvidia.conf", mpd);
	writef(want, "options nvidia NVreg_EnableGpuFirmware=0\n");
	memset(&d, 0, sizeof(d));
	d.sysroot = root;
	d.config_path = cfg;
	d.modprobe_dir = mpd;

	/* Set to 0 elsewhere, the driver step's own file left from an earlier install, no hold: noted, and the hold is missing. */
	rc = run_diag(gs_cmd_doctor, &d, &out);
	snprintf(want, sizeof(want), "[INFO] %s already sets NVreg_EnableGpuFirmware=0; the driver step keeps it; its own file "
		 "stays until uninstall\n", p);
	CHECK(contains(out, want) && !contains(out, "[FAIL] ") && !contains(out, "geforce-sriov-nvidia.conf") &&
	      contains(out, "[WARN] nvidia is not held back at boot, so it would bind 0000:04:00.0, the console card, before "
			    "prepare runs; prepare cannot act on a bound card. Add the hold from "
			    "docs/installation.md#boot-display-card\n"),
	      "doctor, option 0 elsewhere, no hold: noted, hold missing: %d\n%s", rc, out);
	free(out);
	/* Without the driver step's own file: it writes none. */
	gs_pathf(want, sizeof(want), "%s/geforce-sriov-nvidia.conf", mpd);
	unlink(want);
	rc = run_diag(gs_cmd_doctor, &d, &out);
	snprintf(want, sizeof(want), "[INFO] %s already sets NVreg_EnableGpuFirmware=0; the driver step keeps it and writes no "
		 "file of its own\n", p);
	CHECK(contains(out, want), "doctor, option 0 elsewhere, no file of the driver step: it writes none: %d\n%s", rc, out);
	free(out);

	/* A blacklist alone is no hold. */
	writef(hold, "blacklist nvidia\n");
	rc = run_diag(gs_cmd_doctor, &d, &out);
	CHECK(contains(out, "[WARN] nvidia is not held back at boot"), "doctor, blacklist alone: hold missing: %d\n%s", rc, out);
	free(out);

	/* Both lines: held. Another value: FAIL naming the file and the value. */
	writef(hold, "# keep nvidia unloaded\nblacklist nvidia\ninstall nvidia /bin/false\n");
	writef(p, "options nvidia NVreg_EnableGpuFirmware=0\noptions nvidia NVreg_X=1 NVreg_EnableGpuFirmware=18\n");
	rc = run_diag(gs_cmd_doctor, &d, &out);
	snprintf(want, sizeof(want), "[FAIL] %s sets NVreg_EnableGpuFirmware=18; this stack needs 0\n", p);
	CHECK(rc != 0 && contains(out, want) && !contains(out, "already sets") && !contains(out, "not held back") &&
	      contains(out, "[OK  ] nvidia is held back at boot"),
	      "doctor, option 18, hold present: FAIL and OK: %d\n%s", rc, out);
	free(out);

	/* No console release granted and no hold: no hold line. */
	writef(cfg, "bdfs=0000:03:00.0\n");
	unlink(hold);
	rc = run_diag(gs_cmd_doctor, &d, &out);
	CHECK(!contains(out, "held back at boot"), "doctor, no console release: no hold line: %d\n%s", rc, out);
	tally_of(out, &f0, &w0);
	free(out);
	/* No console release granted, the hold left: nothing would load nvidia, so activate refuses; one more warning. */
	writef(hold, "blacklist nvidia\ninstall nvidia /bin/false\n");
	rc = run_diag(gs_cmd_doctor, &d, &out);
	tally_of(out, &f1, &w1);
	snprintf(want, sizeof(want), "[WARN] nvidia is held back at boot by %s, and no console release is granted. activate "
		 "will refuse. Remove %s (docs/installation.md#returning-to-automatic-preparation)\n", hold, hold);
	CHECK(contains(out, want) && w1 == w0 + 1 && f1 == f0, "doctor, hold left without a grant: WARN naming the file: %d\n%s",
	      rc, out);
	free(out);
	/* The lines in a file of other entries too: only those lines are to be removed, not the file. */
	unlink(hold);
	gs_pathf(hold, sizeof(hold), "%s/blacklist.conf", mpd);
	writef(hold, "blacklist nouveau\nblacklist nvidia\ninstall nvidia /bin/false\n");
	rc = run_diag(gs_cmd_doctor, &d, &out);
	snprintf(want, sizeof(want), "activate will refuse. Remove the `blacklist nvidia` and "
		 "`install nvidia /bin/false` lines from %s (docs/installation.md#returning-to-automatic-preparation)\n", hold);
	CHECK(contains(out, want), "doctor, hold in a shared file: names the lines to remove: %d\n%s", rc, out);
	free(out);
	/* Without an installed config (a first install still to come), the hold is not judged. */
	gs_pathf(want, sizeof(want), "%s/doctor-mp-no-config", tmp);
	d.config_path = want;
	rc = run_diag(gs_cmd_doctor, &d, &out);
	CHECK(!contains(out, "held back at boot"), "doctor, hold without a config: no hold line: %d\n%s", rc, out);
	free(out);
	/* A grant planned by --boot-display-release without a config, nothing in modprobe.d: the hold is missing. */
	gs_pathf(mpd, sizeof(mpd), "%s/doctor-mp-empty-modprobe.d", tmp);
	if (gs_mkdir_p(mpd, 0755))
		die(mpd);
	d.modprobe_dir = mpd;
	gs_addr_parse("0000:03:00.0", &d.devices[0]);
	gs_addr_parse("0000:04:00.0", &d.devices[1]);
	d.ndevices = 2;
	d.release[0] = d.devices[1];
	d.nrelease = 1;
	rc = run_diag(gs_cmd_doctor, &d, &out);
	CHECK(contains(out, "[INFO] config: console release of 0000:04:00.0 planned by --boot-display-release\n") &&
	      contains(out, "[WARN] nvidia is not held back at boot, so it would bind 0000:04:00.0, the console card, before "
			    "prepare runs; prepare cannot act on a bound card. Add the hold from "
			    "docs/installation.md#boot-display-card\n"),
	      "doctor, grant planned by --boot-display-release, empty modprobe.d: hold missing: %d\n%s", rc, out);
	free(out);
}

/*
 * doctor judges libnvidia-vgpu.so as activate does, through the link: anything but the patched plugin copy is a
 * warning naming that copy's digest once the driver step has run (its record or install's manifest exists); before
 * that the stock plugin is expected there, so the line is INFO. The digests of NVIDIA's files are covered by the
 * gs_plugin_verdict cases in test_activate. Without a plugin on a fixture root there is no plugin line.
 */
static void test_doctor_plugin(void)
{
	char root[GS_PATH_MAX], cfg[GS_PATH_MAX], lnk[GS_PATH_MAX], copy[GS_PATH_MAX], record[GS_PATH_MAX], module[GS_PATH_MAX];
	char sha[65], want[3 * GS_PATH_MAX];
	struct gs_build_src src;
	struct gs_diag_opts d;
	char *out;
	int rc;

	two_cards("doctor-plugin", 1, FW_BOUND, root, sizeof(root));
	gs_pathf(cfg, sizeof(cfg), "%s/doctor-plugin-config", tmp);
	writef(cfg, "bdfs=0000:03:00.0\n");
	gs_pathf(record, sizeof(record), "%s/doctor-plugin-state/driver-build", tmp);
	gs_pathf(module, sizeof(module), "%s/doctor-plugin-state/no-nvidia.ko", tmp);
	writef(record, "kernel=6.1.0-gs-doctor\n");
	src = (struct gs_build_src){ record, "6.1.0-gs-doctor", module };
	memset(&d, 0, sizeof(d));
	d.sysroot = root;
	d.config_path = cfg;
	d.build = &src;
	rc = run_diag(gs_cmd_doctor, &d, &out);
	CHECK(!contains(out, "cannot read the vGPU plugin") && !contains(out, "not the patched plugin copy") &&
	      !contains(out, ": the patched plugin copy\n"), "doctor, fixture, no plugin: no plugin line: %d\n%s", rc, out);
	free(out);

	gs_pathf(lnk, sizeof(lnk), "%s/libnvidia-vgpu.so", root);
	gs_pathf(copy, sizeof(copy), "%s/libnvidia-vgpu.so.580.159.01.geforce-sriov", root);
	writef(copy, "not NVIDIA's plugin\n");
	link_to("libnvidia-vgpu.so.580.159.01.geforce-sriov", lnk);
	gs_sha256_buf("not NVIDIA's plugin\n", 20, sha);
	d.plugin = lnk;
	rc = run_diag(gs_cmd_doctor, &d, &out);
	snprintf(want, sizeof(want), "[WARN] %s (%s) has SHA-256 %s: not the patched plugin copy of NVIDIA's 580.159.01\n"
		 "       plugin, SHA-256 aa4fdeb5288b5e56e7d4972466813d78e10cfb428fcf62fb53b84fec718cf07c\n", lnk, copy, sha);
	CHECK(contains(out, want) && !contains(out, "[FAIL] vGPU plugin") && !contains(out, "driver step has not run on this host; install makes"),
	      "doctor, driver step run, another plugin copy: a warning naming the patched plugin copy's digest: %d\n%s", rc, out);
	free(out);

	/* No record and no install manifest: install has not run, so a plugin that is not the copy is only noted. */
	unlink(record);
	rc = run_diag(gs_cmd_doctor, &d, &out);
	snprintf(want, sizeof(want), "[INFO] %s (%s) has SHA-256 %s: not the patched plugin copy of NVIDIA's 580.159.01\n"
		 "       plugin, SHA-256 aa4fdeb5288b5e56e7d4972466813d78e10cfb428fcf62fb53b84fec718cf07c\n"
		 "       the driver step has not run on this host; install makes the patched plugin copy\n", lnk, copy, sha);
	CHECK(contains(out, want), "doctor, before install, another plugin: noted only: %d\n%s", rc, out);
	free(out);
	writef(record, "kernel=6.1.0-gs-doctor\n");

	unlink(copy);
	rc = run_diag(gs_cmd_doctor, &d, &out);
	snprintf(want, sizeof(want), "[WARN] cannot read the vGPU plugin %s (from NVIDIA's host package): No such file or "
		 "directory\n", lnk);
	CHECK(contains(out, want), "doctor, dangling plugin link: a warning: %d\n%s", rc, out);
	free(out);
}

/*
 * doctor names each drop-in of nvidia-vgpud or nvidia-vgpu-mgr, other than the package's own, that sets LD_PRELOAD
 * and so replaces the identity hook, with the hook the package's drop-in preloads. A drop-in that sets no LD_PRELOAD,
 * or that systemd reads before the package's own, is not named.
 */
static void test_doctor_dropins(void)
{
	char root[GS_PATH_MAX], cfg[GS_PATH_MAX], units[GS_PATH_MAX], own[GS_PATH_MAX], other[GS_PATH_MAX];
	char mgr[GS_PATH_MAX], want[3 * GS_PATH_MAX];
	struct gs_diag_opts d;
	char *out;
	int rc;

	two_cards("doctor-dropins", 1, FW_BOUND, root, sizeof(root));
	gs_pathf(cfg, sizeof(cfg), "%s/doctor-dropins-config", tmp);
	writef(cfg, "bdfs=0000:03:00.0\n");
	gs_pathf(units, sizeof(units), "%s/doctor-dropins-units", tmp);
	gs_pathf(own, sizeof(own), "%s/nvidia-vgpud.service.d/geforce-sriov.conf", units);
	writef(own, "[Unit]\nAfter=geforce-sriov-prepare.service\n\n[Service]\nEnvironment=LD_PRELOAD=/usr/lib/geforce-sriov/"
		    "libgeforce_sriov.so\n");
	memset(&d, 0, sizeof(d));
	d.sysroot = root;
	d.config_path = cfg;
	d.unit_dir = units;

	rc = run_diag(gs_cmd_doctor, &d, &out);
	CHECK(!contains(out, "LD_PRELOAD"), "doctor, the package's drop-in only: no drop-in line: %d\n%s", rc, out);
	free(out);

	gs_pathf(other, sizeof(other), "%s/nvidia-vgpud.service.d/zz-research.conf", units);
	writef(other, "[Service]\nEnvironment=LD_PRELOAD=/opt/research/hook.so\n");
	rc = run_diag(gs_cmd_doctor, &d, &out);
	snprintf(want, sizeof(want), "[WARN] %s sets LD_PRELOAD for nvidia-vgpud and overrides the geforce-sriov identity "
		 "hook. Remove it or point it at /usr/lib/geforce-sriov/libgeforce_sriov.so.\n", other);
	CHECK(contains(out, want) && count_of(out, "LD_PRELOAD") == 1, "doctor, a foreign drop-in with LD_PRELOAD: named: %d\n%s",
	      rc, out);
	free(out);

	/* Without LD_PRELOAD (a comment does not count), or read before the package's own: not named. */
	writef(other, "[Service]\n# Environment=LD_PRELOAD=/opt/research/hook.so\nEnvironment=FOO_LD_PRELOAD=1 BAR=2\n");
	gs_pathf(want, sizeof(want), "%s/nvidia-vgpud.service.d/00-early.conf", units);
	writef(want, "[Service]\nEnvironment=LD_PRELOAD=/opt/early.so\n");
	rc = run_diag(gs_cmd_doctor, &d, &out);
	CHECK(!contains(out, "LD_PRELOAD"), "doctor, a foreign drop-in without LD_PRELOAD, one read first: no line: %d\n%s",
	      rc, out);
	free(out);
	/* A later LD_PRELOAD list that still holds the hook does not replace it: not named. */
	writef(other, "[Service]\nEnvironment=\"LD_PRELOAD=/opt/research/hook.so /usr/lib/geforce-sriov/libgeforce_sriov.so\"\n");
	rc = run_diag(gs_cmd_doctor, &d, &out);
	CHECK(!contains(out, "LD_PRELOAD"), "doctor, a foreign LD_PRELOAD list with the hook: no line: %d\n%s", rc, out);
	free(out);
	writef(other, "[Service]\nEnvironment=LD_PRELOAD=/usr/lib/geforce-sriov/libgeforce_sriov.so:/opt/x.so\n");
	rc = run_diag(gs_cmd_doctor, &d, &out);
	CHECK(!contains(out, "LD_PRELOAD"), "doctor, a colon list with the hook: no line: %d\n%s", rc, out);
	free(out);
	writef(other, "[Service]\nEnvironment=FOO=1\n");

	gs_pathf(mgr, sizeof(mgr), "%s/nvidia-vgpu-mgr.service.d/override.conf", units);
	writef(mgr, "[Service]\nEnvironment=\"FOO=1\" \"LD_PRELOAD=/opt/mgr.so\"\n");
	rc = run_diag(gs_cmd_doctor, &d, &out);
	snprintf(want, sizeof(want), "[WARN] %s sets LD_PRELOAD for nvidia-vgpu-mgr and overrides the geforce-sriov identity "
		 "hook. Remove it or point it at /usr/lib/geforce-sriov/libgeforce_sriov.so.\n", mgr);
	CHECK(contains(out, want) && count_of(out, "LD_PRELOAD") == 1,
	      "doctor, a drop-in of nvidia-vgpu-mgr with LD_PRELOAD: named: %d\n%s", rc, out);
	free(out);
}

/*
 * The prerequisites are named in one sentence; the version in a package file name. The activate unit has a
 * finite start timeout; doctor has no root-filesystem warning (the prepare unit's comment keeps the rule).
 */
static void test_prereq_and_package(void)
{
	char buf[GS_PATH_MAX + 256], *text, *data;
	size_t len;
	int r;

	CHECK(strcmp(GS_PREREQ_MSG, "A live install needs kernel headers for the running release, dkms, pciutils, a C "
				    "compiler and make; install them with your distribution's package manager.") == 0,
	      "prerequisite sentence: %s", GS_PREREQ_MSG);

	gs_pathf(buf, sizeof(buf), "%s/systemd/geforce-sriov-activate.service.in", srcdir);
	text = slurp(buf);
	CHECK(contains(text, "; it is not the sum of every wait.\nTimeoutStartSec=600\n") &&
	      !contains(text, "infinity"), "activate unit: start timeout 600 s:\n%s", text ? text : "");
	free(text);
	gs_pathf(buf, sizeof(buf), "%s/systemd/geforce-sriov-prepare.service.in", srcdir);
	text = slurp(buf);
	CHECK(contains(text, "this unit runs before udevd"), "prepare unit: the root filesystem rule stays in its comment");
	CHECK(contains(text, " for a card with an audio function).\nTimeoutStartSec=300\n") &&
	      !contains(text, "infinity"), "prepare unit: start timeout 300 s:\n%s", text ? text : "");
	free(text);
	if (gs_read_whole(cli, &data, &len, 64u << 20))
		die(cli);
	CHECK(!memmem(data, len, "is not on the root filesystem", 29), "doctor has no root-filesystem warning");
	free(data);

	r = gs_nvidia_package_version("/x/NVIDIA-Linux-x86_64-580.159.01-vgpu-kvm.run", buf, sizeof(buf));
	CHECK(r == 0 && strcmp(buf, "580.159.01") == 0, "package version: %d '%s'", r, buf);
	r = gs_nvidia_package_version("NVIDIA-Linux-x86_64-595.91.04.run", buf, sizeof(buf));
	CHECK(r == 0 && strcmp(buf, "595.91.04") == 0, "package version before .run: %d '%s'", r, buf);
	r = gs_nvidia_package_version("/opt/1.2.3-x/vgpu-host.run", buf, sizeof(buf));
	CHECK(r == -1, "no version in the file name (the directory does not count): %d '%s'", r, buf);
}

/*
 * gs_nvidia_package in a child, with base as the extraction root and root as the host; its exit status and
 * output, ending in "changed=0" or "changed=1".
 */
static int run_package(const char *file, const char *base, const char *root, int accept, char **out)
{
	char dir[GS_PATH_MAX];
	int pipefd[2];
	pid_t pid;

	if (pipe(pipefd))
		die("pipe");
	pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0) {
		int rc, changed = -1;

		dup2(pipefd[1], 1);
		dup2(pipefd[1], 2);
		close(pipefd[0]);
		rc = gs_nvidia_package(file, base, root, accept, dir, sizeof(dir), &changed);
		printf("changed=%d\n", changed);
		fflush(stdout);
		fflush(stderr);
		_exit(rc);
	}
	close(pipefd[1]);
	return collect(pipefd[0], pid, out);
}

/*
 * --driver-file: the option rules, and the extraction against fixture packages (shell scripts
 * that log their arguments), so no real package and no installer runs.
 */
static void test_driver_file(void)
{
	char pkg[GS_PATH_MAX], other[GS_PATH_MAX], plain[GS_PATH_MAX], bad[GS_PATH_MAX], log[GS_PATH_MAX];
	char base[GS_PATH_MAX], root[GS_PATH_MAX], p[GS_PATH_MAX], stage[GS_PATH_MAX], sha[65], *out, *text;
	static const char *const script =
		"#!/bin/sh\n"
		"echo \"$*\" >> '%s'\n"
		"if [ \"$1\" = --extract-only ]; then\n"
		"  mkdir -p \"$3/kernel/nvidia\" \"$3/init-scripts/systemd\" && : > \"$3/LICENSE\" && %s\n"
		"fi\n"
		"exit %d\n";
	int rc;

	gs_pathf(log, sizeof(log), "%s/pkg/log", tmp);
	gs_pathf(base, sizeof(base), "%s/pkg/state", tmp);
	gs_pathf(root, sizeof(root), "%s/pkg/root", tmp);
	gs_pathf(pkg, sizeof(pkg), "%s/pkg/NVIDIA-Linux-x86_64-580.159.01-vgpu-kvm.run", tmp);
	writef(pkg, script, log, ": > \"$3/kernel/nvidia/nv-kernel.o_binary\"", 0);

	/* Option rules: usage errors before anything runs. */
	rc = run_cli(&out, NULL, (const char *const[]){ "install", "--driver-file", pkg, "--nvidia-source", "/usr/src/x", NULL });
	CHECK(rc == GS_EXIT_USAGE && contains(out, "--driver-file and --nvidia-source"), "install --driver-file + --nvidia-source: %d\n%s",
	      rc, out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "setup", "--nvidia-source=/usr/src/x", "--driver-file=" , NULL });
	CHECK(rc == GS_EXIT_USAGE, "setup --driver-file without a file: %d\n%s", rc, out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "setup", "--nvidia-source", "/usr/src/x", "--driver-file", pkg, NULL });
	CHECK(rc == GS_EXIT_USAGE && contains(out, "--driver-file and --nvidia-source"), "setup --driver-file + --nvidia-source: %d\n%s",
	      rc, out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "install", "--accept-nvidia-license", NULL });
	CHECK(rc == GS_EXIT_USAGE && contains(out, "needs --driver-file"), "licence without a package: %d\n%s", rc, out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "install", "--driver-file", pkg, "--driver=skip", NULL });
	CHECK(rc == GS_EXIT_USAGE && contains(out, "--driver=skip"), "--driver-file + --driver=skip: %d\n%s", rc, out);
	free(out);
	gs_pathf(p, sizeof(p), "%s/pkg/absent.run", tmp);
	rc = run_cli(&out, NULL, (const char *const[]){ "install", "--driver-file", p, NULL });
	CHECK(rc == GS_EXIT_USAGE && contains(out, "not a regular file"), "absent package: %d\n%s", rc, out);
	free(out);
	gs_pathf(p, sizeof(p), "%s/pkg", tmp);
	rc = run_cli(&out, NULL, (const char *const[]){ "install", "--driver-file", p, NULL });
	CHECK(rc == GS_EXIT_USAGE && contains(out, "not a regular file"), "directory as package: %d\n%s", rc, out);
	free(out);
	gs_pathf(stage, sizeof(stage), "%s/pkg-stage", tmp);
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--source", srcdir, "--driver-file", pkg, NULL });
	CHECK(rc == GS_EXIT_USAGE && contains(out, "omit it when staging") && !gs_exists(stage),
	      "staging with --driver-file: %d\n%s", rc, out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "help", NULL });
	CHECK(rc == 0 && contains(out, "[--nvidia-source DIR] [--driver-file PATH] [--accept-nvidia-license]\n") &&
	      contains(out, "[--msix=on|off] [--driver=skip]\n"
			    "          [--nvidia-source DIR] [--driver-file PATH] [--accept-nvidia-license]\n") &&
	      contains(out, "  --driver-file PATH ") && contains(out, "  --accept-nvidia-license "),
	      "help names the package options on setup and install:\n%s", out);
	free(out);

	/* Without --accept-nvidia-license: extracted only; the userspace is named as still to install. The
	 * extraction was written, so the refusal exits 1, not 3. */
	rc = run_package(pkg, base, root, 0, &out);
	gs_pathf(p, sizeof(p), "%s/580.159.01/kernel/nvidia/nv-kernel.o_binary", base);
	CHECK(rc == GS_EXIT_FAIL && contains(out, "extracted only") && contains(out, "changed=1\n") &&
	      contains(out, "NVIDIA's vGPU userspace\n(nvidia-vgpud, nvidia-vgpu-mgr, libnvidia-vgpu.so and sriov-manage), "
			    "which is not installed") &&
	      contains(out, "--accept-nvidia-license") && !contains(out, "NVIDIA's licence") && gs_exists(p),
	      "extract only: %d\n%s", rc, out);
	free(out);
	text = slurp(log);
	CHECK(contains(text, "--extract-only --target ") && contains(text, "/580.159.01.partial\n") &&
	      !contains(text, "--silent"), "extract only runs the package with --extract-only alone:\n%s", text);
	free(text);
	gs_pathf(p, sizeof(p), "%s/580.159.01.partial", base);
	CHECK(!gs_exists(p), "the partial extraction was renamed into place");

	/* nouveau loaded: NVIDIA's installer is not run. */
	gs_pathf(p, sizeof(p), "%s/sys/module/nouveau/refcnt", root);
	writef(p, "1\n");
	rc = run_package(pkg, base, root, 1, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "nouveau is loaded") && contains(out, "blacklist nouveau"),
	      "nouveau loaded: %d\n%s", rc, out);
	free(out);
	text = slurp(log);
	CHECK(!contains(text, "--silent"), "nouveau loaded: installer not run:\n%s", text);
	free(text);
	gs_pathf(p, sizeof(p), "%s/sys", root);
	cleanup(p);

	/* With it: the earlier extraction is reused, the licence notice printed, the installer run without loading nvidia. */
	rc = run_package(pkg, base, root, 1, &out);
	CHECK(rc == 0 && contains(out, "reusing") && contains(out, "NVIDIA's licence") && contains(out, "/580.159.01/LICENSE") &&
	      contains(out, "--silent --dkms --skip-module-load") && !contains(out, "extracted only"),
	      "licence accepted: %d\n%s", rc, out);
	free(out);
	text = slurp(log);
	CHECK(contains(text, "--silent --dkms --skip-module-load\n"), "the installer ran with --silent --dkms --skip-module-load:\n%s",
	      text);
	free(text);

	/* That version's userspace installed: a rerun runs no installer, and extract-only goes on. */
	gs_pathf(p, sizeof(p), "%s/usr/lib64/libnvidia-vgpu.so.580.159.01", root);
	writef(p, "stock\n");
	unlink(log);
	rc = run_package(pkg, base, root, 1, &out);
	CHECK(rc == 0 && contains(out, "NVIDIA's vGPU userspace 580.159.01 is already installed; NVIDIA's installer not run") &&
	      !contains(out, "NVIDIA's licence") && !gs_exists(log), "installed: installer skipped: %d\n%s", rc, out);
	free(out);
	rc = run_package(pkg, base, root, 0, &out);
	CHECK(rc == 0 && contains(out, "already installed") && !contains(out, "extracted only") && contains(out, "changed=0\n"),
	      "installed: extract only proceeds: %d\n%s", rc, out);
	free(out);

	/* Another file under the same version is refused; an unversioned name uses its SHA-256 prefix. */
	gs_pathf(other, sizeof(other), "%s/pkg/other/NVIDIA-Linux-x86_64-580.159.01-vgpu-kvm.run", tmp);
	writef(other, script, log, "echo other > /dev/null", 0);
	rc = run_package(other, base, root, 0, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "holds another extraction"), "same version, other file: %d\n%s", rc, out);
	free(out);
	gs_pathf(plain, sizeof(plain), "%s/pkg/vgpu-host.run", tmp);
	writef(plain, script, log, ": > \"$3/kernel/nvidia/x\"", 0);
	rc = run_package(plain, base, root, 0, &out);
	CHECK(rc == 0 && gs_sha256_file(plain, sha) == 0, "unversioned package: %d\n%s", rc, out);
	free(out);
	sha[16] = '\0';
	gs_pathf(p, sizeof(p), "%s/%s/kernel", base, sha);
	CHECK(gs_exists(p), "unversioned package extracted below its SHA-256 prefix %s", sha);
	/* Its version is unknown, so with the licence accepted the installer runs. */
	rc = run_package(plain, base, root, 1, &out);
	text = slurp(log);
	CHECK(rc == 0 && contains(text, "--silent --dkms --skip-module-load\n"), "unversioned package: installer runs: %d\n%s",
	      rc, out);
	free(text);
	free(out);

	/* A failing extractor and a package without kernel/ leave no extraction behind. */
	gs_pathf(bad, sizeof(bad), "%s/pkg/NVIDIA-Linux-x86_64-1.2.3.run", tmp);
	writef(bad, script, log, ":", 1);
	rc = run_package(bad, base, root, 0, &out);
	gs_pathf(p, sizeof(p), "%s/1.2.3", base);
	CHECK(rc == GS_EXIT_FAIL && contains(out, "extracting") && !gs_exists(p), "failing extractor: %d\n%s", rc, out);
	free(out);
	gs_pathf(bad, sizeof(bad), "%s/pkg/NVIDIA-Linux-x86_64-1.2.4.run", tmp);
	writef(bad, "#!/bin/sh\nmkdir -p \"$3\"\nexit 0\n");
	rc = run_package(bad, base, root, 0, &out);
	gs_pathf(p, sizeof(p), "%s/1.2.4", base);
	CHECK(rc == GS_EXIT_FAIL && contains(out, "no kernel/ directory") && !gs_exists(p), "package without kernel/: %d\n%s",
	      rc, out);
	free(out);
}

/* ---- setup's questions --------------------------------------------------- */

static char setup_log[GS_PATH_MAX];
static int setup_doctor_rc, setup_check_rc;
static const char *setup_config; /* NULL: a host without a config; else the installed host's config */
static const char *setup_fail;   /* the command that exits setup_fail_rc ("modprobe" for the module load) */
static int setup_fail_rc;
static const struct gs_build_src *setup_build; /* NULL: no driver build check (a fixture host) */
static const char *setup_doctor_sysfs; /* NULL: doctor exits setup_doctor_rc; else the real doctor on this fixture root */

/*
 * setup's steps on the fixture: each logged as "R <command> <args>" (modprobe as "R modprobe <args>"), none
 * run; doctor exits setup_doctor_rc, check setup_check_rc, setup_fail setup_fail_rc. With setup_doctor_sysfs,
 * doctor runs instead: the geforce-sriov CLI with setup's arguments and --sysfs-root, its output in setup's.
 */
static int setup_run(const char *const argv[])
{
	size_t first = strcmp(argv[0], "modprobe") == 0 ? 0 : 1;
	char line[4096] = "R";
	FILE *f;

	for (size_t i = first; argv[i]; i++)
		strcat(strcat(line, " "), argv[i]);
	f = fopen(setup_log, "a");
	if (!f)
		die(setup_log);
	fprintf(f, "%s\n", line);
	fclose(f);
	if (setup_fail && strcmp(argv[first], setup_fail) == 0)
		return setup_fail_rc;
	if (strcmp(argv[first], "doctor") == 0 && setup_doctor_sysfs) {
		const char *args[31]; /* run_cli takes up to 30 */
		size_t n = 0;
		char *out;
		int rc;

		for (size_t i = first; argv[i] && n < 28; i++)
			args[n++] = argv[i];
		args[n++] = "--sysfs-root";
		args[n++] = setup_doctor_sysfs;
		args[n] = NULL;
		rc = run_cli(&out, NULL, args);
		fputs(out, stdout);
		fflush(stdout);
		free(out);
		return rc;
	}
	if (strcmp(argv[first], "doctor") == 0)
		return setup_doctor_rc;
	return strcmp(argv[first], "check") == 0 ? setup_check_rc : 0;
}

/*
 * gs_setup on a host without a config (or, with setup_config, an installed one), reading the cards of
 * root, with answers on its input (NULL: it asks nothing, as without a terminal); its exit status and
 * output, the steps in setup_log.
 */
static int run_setup(const char *root, const char *answers, const struct gs_install_opts *o, int driver_present,
		     char **out)
{
	struct gs_host h = { root, root, root, root, NULL, setup_run, NULL };
	struct gs_setup_host s = { &h, -1, driver_present, setup_config, setup_build };
	char cfg[GS_PATH_MAX];
	int in[2], pipefd[2];
	pid_t pid;

	gs_pathf(setup_log, sizeof(setup_log), "%s/setup.log", root);
	unlink(setup_log);
	gs_pathf(cfg, sizeof(cfg), "%s/no-config", root);
	if (answers) {
		if (pipe(in))
			die("pipe");
		if (write(in[1], answers, strlen(answers)) != (ssize_t)strlen(answers))
			die("write");
		close(in[1]);
		s.in = in[0];
	}
	if (pipe(pipefd))
		die("pipe");
	pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0) {
		int rc;

		dup2(pipefd[1], 1);
		dup2(pipefd[1], 2);
		close(pipefd[0]);
		setenv(GS_CONFIG_ENV, cfg, 1);
		unsetenv("DESTDIR");
		rc = gs_setup(&s, o);
		fflush(stdout);
		fflush(stderr);
		_exit(rc);
	}
	if (answers)
		close(in[0]);
	close(pipefd[1]);
	return collect(pipefd[0], pid, out);
}

static void setup_opts(struct gs_install_opts *o)
{
	memset(o, 0, sizeof(*o));
	o->ov.ari = o->ov.msix = GS_SW_UNSET;
}

#define DRIVER_Q "Path to NVIDIA's vGPU host driver package (.run)?"
#define GPUS_Q "Which GPUs? all, a number, or numbers separated by commas"
#define RELEASE_Q "Allow releasing the console of 0000:04:00.0 after the next boot? [y/N]"

/*
 * Three eligible cards: 1 = 0000:03:00.0 and 3 = 0000:05:00.0, and 2 = 0000:04:00.0, the boot display.
 * Nothing runs: the steps are only logged.
 */
static void test_setup_questions(void)
{
	static const struct fx_topo t4 = { "pci0000:00/0000:00:03.0", "0000:04:00.0" };
	static const struct fx_topo t5 = { "pci0000:00/0000:00:04.0", "0000:05:00.0" };
	static const struct fx_fn c3[] = { { "0000:03:00.0", "2206", "030000", NULL }, { NULL, NULL, NULL, NULL } };
	static const struct fx_fn c4[] = { { "0000:04:00.0", "2206", "030000", NULL }, { NULL, NULL, NULL, NULL } };
	static const struct fx_fn c5[] = { { "0000:05:00.0", "2783", "030000", NULL }, { NULL, NULL, NULL, NULL } };
	char root[GS_PATH_MAX], pkg[GS_PATH_MAX], real[PATH_MAX], answers[GS_PATH_MAX + 128], want[GS_PATH_MAX + 512];
	char *out, *log;
	const char *line;
	struct gs_install_opts o;
	struct timespec t0, t1;
	int rc;

	snprintf(root, sizeof(root), "%s/setup-host", tmp);
	make_card(root, &topo_default, c3, 0, NULL);
	make_card(root, &t4, c4, 1, NULL);
	make_card(root, &t5, c5, 0, NULL);
	gs_pathf(pkg, sizeof(pkg), "%s/setup-pkg/NVIDIA-Linux-x86_64-%s-vgpu-kvm.run", tmp, GS_NVIDIA_VERSION);
	writef(pkg, "#!/bin/sh\nexit 0\n");
	if (!realpath(pkg, real))
		die(pkg);
	setup_doctor_rc = 0;

	/*
	 * Every question answered on piped input: an empty, a relative and a missing path are asked again,
	 * so are 0 and 99; all selects the two cards that are not the boot display.
	 */
	setup_opts(&o);
	snprintf(answers, sizeof(answers), "\nrelative.run\n/nonexistent/x.run\n%s\ny\n0\n99\nall\n", pkg);
	rc = run_setup(root, answers, &o, 0, &out);
	log = slurp(setup_log);
	CHECK(rc == 0 && count_of(out, DRIVER_Q) == 4 && contains(out, "give the absolute path of the .run file") &&
	      contains(out, "relative.run is not an absolute path") && contains(out, "/nonexistent/x.run is not an existing file"),
	      "driver question asked again on empty, relative and missing paths: %d\n%s", rc, out);
	snprintf(want, sizeof(want), "= --driver-file %s\n", real);
	CHECK(contains(out, want) && contains(out, "= --accept-nvidia-license\n"), "each answer prints its flag:\n%s", out);
	line = strstr(out, "  2  0000:04:00.0  ");
	CHECK(line && contains(out, "  1  0000:03:00.0  ") && contains(out, "  3  0000:05:00.0  ") &&
	      strstr(line, "boot display") && strstr(line, "boot display") < strchr(line, '\n') &&
	      count_of(out, "  boot display\n") == 1, "numbered list marks only the boot display card:\n%s", out);
	CHECK(count_of(out, GPUS_Q) == 3 && count_of(out, "answer all, or numbers from 1 to 3") == 2 &&
	      contains(out, "= --device 0000:03:00.0 --device 0000:05:00.0\n"),
	      "0 and 99 asked again; all leaves out the boot display:\n%s", out);
	snprintf(want, sizeof(want),
		 "geforce-sriov setup --no-prompt --driver-file %s --accept-nvidia-license --device 0000:03:00.0 --device 0000:05:00.0\n",
		 real);
	CHECK(contains(out, "Setup runs with:") && contains(out, want) && pos_of(out, want) < pos_of(out, "== doctor"),
	      "summary with the equivalent flags before any step:\n%s", out);
	snprintf(want, sizeof(want), "R install --device 0000:03:00.0 --device 0000:05:00.0 --driver-file %s --accept-nvidia-license\n",
		 real);
	CHECK(contains(log, want) && contains(log, "R doctor --config ") && !contains(log, "0000:04:00.0"),
	      "the run reaches install with the answers as flags:\n%s", log ? log : "");
	free(log);
	free(out);

	/* An empty answer is never taken as a path; at the end of input nothing runs, and the run ends at once. */
	clock_gettime(CLOCK_MONOTONIC, &t0);
	rc = run_setup(root, "\n", &o, 0, &out);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	log = slurp(setup_log);
	CHECK(rc == GS_EXIT_REFUSED && count_of(out, DRIVER_Q) == 2 && contains(out, "Give --driver-file PATH instead") && !log,
	      "empty driver answer asked again, then refused at the end of input: %d\n%s", rc, out);
	CHECK(t1.tv_sec - t0.tv_sec + (t1.tv_nsec - t0.tv_nsec) / 1e9 < 1.0, "end of input before the driver: ended in under 1 s");
	free(log);
	free(out);

	/* The input ends at each later question: refused naming the flag that answers it, nothing run. */
	o.driver_file = real;
	rc = run_setup(root, "", &o, 0, &out);
	log = slurp(setup_log);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "Give --accept-nvidia-license instead") && !log,
	      "end of input at the licence: %d\n%s", rc, out);
	free(log);
	free(out);
	o.accept_nvidia_license = 1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	rc = run_setup(root, "\n0\n", &o, 0, &out);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	log = slurp(setup_log);
	CHECK(rc == GS_EXIT_REFUSED && count_of(out, GPUS_Q) == 3 && contains(out, "Give --device BDF instead") && !log,
	      "end of input at the selection: %d\n%s", rc, out);
	CHECK(t1.tv_sec - t0.tv_sec + (t1.tv_nsec - t0.tv_nsec) / 1e9 < 1.0, "end of input at the selection: ended in under 1 s");
	free(log);
	free(out);

	/* The boot display only by its number, with its console release confirmed; a no to the licence extracts only. */
	setup_opts(&o);
	o.driver_file = real;
	rc = run_setup(root, "n\n2, 3\ny\n", &o, 0, &out);
	log = slurp(setup_log);
	/* setup explained the grant at its question, so install is told not to repeat its note. */
	snprintf(want, sizeof(want),
		 "R install --device 0000:04:00.0 --device 0000:05:00.0 --boot-display-release 0000:04:00.0 --driver-file %s "
		 "--release-note-shown\n", real);
	CHECK(rc == 0 && !contains(out, DRIVER_Q) && contains(out, "extract only") &&
	      contains(out, RELEASE_Q "\n  Yes records the grant; nothing is released now. After the power cycle, setup asks\n"
			    "  again before it releases the console of 0000:04:00.0, which stops the local display\n"
			    "  on that card until the following boot. SSH keeps working. Default is no.\n") &&
	      !contains(out, "stops until the next boot") &&
	      pos_of(out, RELEASE_Q) < pos_of(out, "= --device 0000:04:00.0 --device 0000:05:00.0 --boot-display-release 0000:04:00.0\n") &&
	      !contains(out, "Console release:") && contains(log, want) && !contains(log, "--accept-nvidia-license"),
	      "boot display by number, release confirmed, licence declined: %d\n%s\n%s", rc, out, log ? log : "");
	{
		char dwant[2 * GS_PATH_MAX + 256];

		/* doctor judges the cards with the grant install gets, so it plans the boot display's console release. */
		snprintf(dwant, sizeof(dwant),
			 "R doctor --config %s/no-config --driver-file %s --device 0000:04:00.0 --device 0000:05:00.0 "
			 "--boot-display-release 0000:04:00.0\nR install ", root, real);
		CHECK(contains(log, dwant), "boot display by number, release confirmed: doctor gets the grant:\n%s",
		      log ? log : "");
	}
	free(log);
	free(out);

	/* No (or an empty answer) drops the boot display card and continues with the others. */
	for (int k = 0; k < 2; k++) {
		rc = run_setup(root, k ? "n\n2, 3\n\n" : "n\n2, 3\nn\n", &o, 0, &out);
		log = slurp(setup_log);
		snprintf(want, sizeof(want), "R install --device 0000:05:00.0 --driver-file %s\n", real);
		CHECK(rc == 0 && contains(out, "0000:04:00.0 is left out\n") && contains(out, "= --device 0000:05:00.0\n") &&
		      !contains(out, "Console release:") && contains(log, want) && !contains(log, "--boot-display-release"),
		      "release declined (%s): the boot display card dropped: %d\n%s\n%s", k ? "empty" : "no", rc, out,
		      log ? log : "");
		free(log);
		free(out);
	}
	/* No with only the boot display selected: refused, nothing run; the end of input at the question: refused. */
	rc = run_setup(root, "n\n2\nn\n", &o, 0, &out);
	log = slurp(setup_log);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "setup: no GPU left to select") && !log,
	      "release declined, nothing left: refused: %d\n%s", rc, out);
	free(log);
	free(out);
	rc = run_setup(root, "n\n2\n", &o, 0, &out);
	log = slurp(setup_log);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, RELEASE_Q) && contains(out, "Give --boot-display-release BDF instead") &&
	      !log, "end of input at the release question: refused: %d\n%s", rc, out);
	free(log);
	free(out);

	/* Every input given as a flag: nothing is asked, even with no answer to read. */
	setup_opts(&o);
	o.driver_file = real;
	o.accept_nvidia_license = 1;
	gs_addr_parse("0000:03:00.0", &o.devices[0]);
	o.ndevices = 1;
	rc = run_setup(root, "", &o, 0, &out);
	log = slurp(setup_log);
	CHECK(rc == 0 && !contains(out, "\n> ") && contains(out, "Setup runs with:") && contains(log, "R install --device 0000:03:00.0 "),
	      "flags answer every question: %d\n%s", rc, out);
	free(log);
	free(out);

	/* --no-prompt (no input to ask on): doctor stops the run and setup names the flags it lacks. */
	setup_opts(&o);
	setup_doctor_rc = GS_EXIT_REFUSED;
	rc = run_setup(root, NULL, &o, 0, &out);
	log = slurp(setup_log);
	CHECK(rc == GS_EXIT_REFUSED && !contains(out, "\n> ") && !contains(out, "Setup runs with:") &&
	      contains(out, "To continue, add: --driver-file PATH --device BDF") &&
	      contains(log, "R doctor") && !contains(log, "R install"),
	      "no questions: refused, naming --driver-file: %d\n%s", rc, out);
	free(log);
	free(out);
	/* Without questions and with doctor passing, several eligible GPUs and no --device: refused, nothing installed. */
	setup_doctor_rc = 0;
	rc = run_setup(root, NULL, &o, 1, &out);
	log = slurp(setup_log);
	CHECK(rc == GS_EXIT_REFUSED && !contains(out, "Setup runs with:") &&
	      contains(out, "setup: 3 GPUs are eligible. Give --device BDF for each GPU to use") &&
	      contains(log, "R doctor") && !contains(log, "R install"),
	      "no questions, several GPUs: refused naming --device: %d\n%s", rc, out);
	free(log);
	free(out);

	rc = run_cli(&out, NULL, (const char *const[]){ "setup", "--interactive", "--no-prompt", NULL });
	CHECK(rc == GS_EXIT_USAGE && contains(out, "give one"), "--interactive with --no-prompt: %d\n%s", rc, out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "--help", NULL });
	CHECK(rc == 0 && contains(out, "[--interactive | --no-prompt]"), "help names the prompt options:\n%s", out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "setup", "--ari=off", NULL });
	CHECK(rc == GS_EXIT_USAGE && contains(out, "unknown option '--ari=off'"), "setup takes no switches: %d\n%s", rc, out);
	free(out);
}

/*
 * Nine eligible cards: the list shows the eight lowest addresses, whatever the directory order, and counts
 * the one left out. The first eight are boot displays; without input setup does not take them for the
 * only card, because the ninth is not one.
 */
static void test_setup_many_cards(void)
{
	char root[GS_PATH_MAX], bridge[9][48], pf[9][16], *out, *log;
	struct fx_topo t[9];
	struct fx_fn fn[9][2];
	struct gs_install_opts o;
	int rc;

	snprintf(root, sizeof(root), "%s/setup-nine", tmp);
	/* Highest address first, so the order of creation is not the sorted order. */
	for (int k = 8; k >= 0; k--) {
		snprintf(bridge[k], sizeof(bridge[k]), "pci0000:00/0000:00:%02x.0", k + 1);
		snprintf(pf[k], sizeof(pf[k]), "0000:%02x:00.0", 0x10 + k);
		t[k] = (struct fx_topo){ bridge[k], pf[k] };
		fn[k][0] = (struct fx_fn){ pf[k], "2206", "030000", NULL };
		fn[k][1] = (struct fx_fn){ NULL, NULL, NULL, NULL };
		make_card(root, &t[k], fn[k], k < 8, NULL);
	}
	setup_opts(&o);
	setup_doctor_rc = 0;

	rc = run_setup(root, "", &o, 1, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "  1  0000:10:00.0  ") && contains(out, "  8  0000:17:00.0  ") &&
	      !contains(out, "0000:18:00.0") && contains(out, "  1 more not listed (at most 8); name them with --device BDF\n"),
	      "nine cards: the eight lowest listed, one counted: %d\n%s", rc, out);
	free(out);

	rc = run_setup(root, NULL, &o, 1, &out);
	log = slurp(setup_log);
	CHECK(rc == GS_EXIT_REFUSED && !contains(out, "the only NVIDIA GPU here is the boot display") &&
	      contains(out, "setup: 9 GPUs are eligible. Give --device BDF for each GPU to use") && contains(log, "R doctor"),
	      "nine cards, eight listed boot displays, no input: not taken for the only card: %d\n%s", rc, out);
	free(log);
	free(out);
}

/* The one eligible card is selected; an installed host honours its selection and stops when check refuses. */
static void test_setup_selection(void)
{
	static const struct fx_fn one[] = { { "0000:03:00.0", "2206", "030000", NULL }, { NULL, NULL, NULL, NULL } };
	char root[GS_PATH_MAX], boot_root[GS_PATH_MAX], cfg[GS_PATH_MAX], *out, *log;
	struct gs_install_opts o;
	int rc;

	fixture("setup-one", one, 0, NULL, root, sizeof(root));
	setup_opts(&o);
	setup_doctor_rc = setup_check_rc = 0;
	rc = run_setup(root, NULL, &o, 1, &out);
	log = slurp(setup_log);
	CHECK(rc == 0 && contains(out, "The one eligible GPU, 0000:03:00.0, is selected: --device 0000:03:00.0\n") &&
	      contains(log, " --device 0000:03:00.0\nR install --device 0000:03:00.0\n") && pos_of(log, "R doctor ") == 0,
	      "one eligible card, no --device: doctor and install get it as --device: %d\n%s\n%s", rc, out, log ? log : "");
	free(log);
	free(out);

	/* The only card is the boot display, no --device and no input to ask with: refused before any step. */
	fixture("setup-boot-only", one, 1, NULL, boot_root, sizeof(boot_root));
	rc = run_setup(boot_root, NULL, &o, 1, &out);
	log = slurp(setup_log);
	CHECK(rc == GS_EXIT_REFUSED &&
	      contains(out, "setup: the only NVIDIA GPU here is the boot display, which setup does not select by itself.\n"
			    "To use it, give --device BDF --boot-display-release BDF, or add another GPU. Nothing changed.\n") && !log,
	      "boot display only, no --device: refused before any step: %d\n%s\n%s", rc, out, log ? log : "");
	free(log);
	free(out);

	/* With input, setup asks the console-release question for that card; yes selects it with the grant. */
	setup_opts(&o);
	setup_doctor_rc = setup_check_rc = 0;
	rc = run_setup(boot_root, "y\n", &o, 1, &out);
	log = slurp(setup_log);
	CHECK(rc == 0 && contains(out, "\nThe only NVIDIA GPU here, 0000:03:00.0, is the boot display.\n") &&
	      contains(out, "Allow releasing the console of 0000:03:00.0 after the next boot? [y/N]\n"
			    "  Yes records the grant; nothing is released now. After the power cycle, setup asks\n") &&
	      contains(out, "  = --device 0000:03:00.0 --boot-display-release 0000:03:00.0\n") &&
	      contains(log, "R install --device 0000:03:00.0 --boot-display-release 0000:03:00.0 --release-note-shown\n") &&
	      contains(log, " --device 0000:03:00.0 --boot-display-release 0000:03:00.0\nR install ") &&
	      pos_of(log, "R doctor ") == 0 &&
	      !contains(out, "does not select by itself"),
	      "boot display only, yes: selected with its release: %d\n%s\n%s", rc, out, log ? log : "");
	free(log);
	free(out);

	/* The same selection as flags: doctor gets the grant as install does. */
	setup_opts(&o);
	gs_addr_parse("0000:03:00.0", &o.devices[0]);
	o.ndevices = 1;
	o.release[0] = o.devices[0];
	o.nrelease = 1;
	rc = run_setup(boot_root, NULL, &o, 1, &out);
	log = slurp(setup_log);
	CHECK(rc == 0 && pos_of(log, "R doctor ") == 0 &&
	      contains(log, " --device 0000:03:00.0 --boot-display-release 0000:03:00.0\n"
			    "R install --device 0000:03:00.0 --boot-display-release 0000:03:00.0\n"),
	      "boot display only, --device and --boot-display-release: doctor gets the grant: %d\n%s\n%s", rc, out,
	      log ? log : "");
	free(log);
	free(out);

	/*
	 * The real doctor on this host without a config: with the grant (flags, or yes to the question) it
	 * plans the console release and setup reaches install; without it, doctor refuses the boot display.
	 */
	setup_doctor_sysfs = boot_root;
	for (int k = 0; k < 2; k++) {
		if (!k) {
			rc = run_setup(boot_root, NULL, &o, 1, &out);
		} else {
			struct gs_install_opts q;

			setup_opts(&q);
			rc = run_setup(boot_root, "y\n", &q, 1, &out);
		}
		log = slurp(setup_log);
		CHECK(rc == 0 && contains(out, "[INFO] config: console release of 0000:03:00.0 planned by --boot-display-release\n") &&
		      !contains(out, "is the boot display device (boot_vga=1)") && !contains(out, "Stopped:") &&
		      contains(log, "\nR install --device 0000:03:00.0 --boot-display-release 0000:03:00.0"),
		      "boot display only, real doctor, grant by %s: install runs: %d\n%s\n%s", k ? "question" : "flags", rc,
		      out, log ? log : "");
		free(log);
		free(out);
	}
	o.nrelease = 0;
	rc = run_setup(boot_root, NULL, &o, 1, &out);
	log = slurp(setup_log);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "0000:03:00.0 is the boot display device (boot_vga=1)") &&
	      contains(out, "Stopped: fix what doctor names above") && !contains(log, "R install"),
	      "boot display only, real doctor, --device without the grant: doctor refuses, nothing installed: %d\n%s\n%s",
	      rc, out, log ? log : "");
	free(log);
	free(out);
	setup_doctor_sysfs = NULL;

	/* No, or an empty answer: nothing left to select, nothing run. The end of input refuses and names its flag. */
	for (int k = 0; k < 2; k++) {
		setup_opts(&o);
		rc = run_setup(boot_root, k ? "\n" : "n\n", &o, 1, &out);
		log = slurp(setup_log);
		CHECK(rc == GS_EXIT_REFUSED && contains(out, "setup: no GPU left to select. Nothing changed.\n") && !log,
		      "boot display only, %s: refused, nothing run: %d\n%s", k ? "empty answer" : "no", rc, out);
		free(log);
		free(out);
	}
	setup_opts(&o);
	rc = run_setup(boot_root, "", &o, 1, &out);
	log = slurp(setup_log);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "Allow releasing the console of 0000:03:00.0") &&
	      contains(out, "Give --boot-display-release BDF instead") && !log,
	      "boot display only, end of input: refused naming the flag: %d\n%s", rc, out);
	free(log);
	free(out);

	/* Installed: check refusing (exit 3) stops the run before prepare and activate. */
	gs_pathf(cfg, sizeof(cfg), "%s/installed-config", root);
	writef(cfg, "bdfs=0000:03:00.0\n");
	setup_config = cfg;
	setup_check_rc = GS_EXIT_REFUSED;
	rc = run_setup(root, NULL, &o, 1, &out);
	log = slurp(setup_log);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "Stopped: check refused (exit 3)") && contains(log, "R check\n") &&
	      !contains(log, "R prepare") && !contains(log, "R activate"),
	      "installed, check refuses: stopped: %d\n%s\n%s", rc, out, log ? log : "");
	free(log);
	free(out);
	/* Not ready: prepare when needed, activate (which enables), check; no --device is passed on. */
	setup_check_rc = GS_EXIT_FAIL;
	gs_addr_parse("0000:03:00.0", &o.devices[0]);
	o.ndevices = 1;
	rc = run_setup(root, NULL, &o, 1, &out);
	log = slurp(setup_log);
	CHECK(rc == GS_EXIT_FAIL && contains(log, "R check\nR prepare --boot\nR activate --boot\nR check\n") &&
	      !contains(log, "enable") && !contains(log, "--device"),
	      "installed, not ready: prepare, activate, check: %d\n%s\n%s", rc, out, log ? log : "");
	free(log);
	free(out);
	/*
	 * A type in the config: setup reads and keeps it, and passes no --type, so its activate and check steps
	 * take the type from the config (test_activate_types covers that side).
	 */
	writef(cfg, "bdfs=0000:03:00.0\ntype_ada=914\ntype_ampere=528\n");
	rc = run_setup(root, NULL, &o, 1, &out);
	log = slurp(setup_log);
	CHECK(rc == GS_EXIT_FAIL && contains(log, "R check\nR prepare --boot\nR activate --boot\nR check\n") &&
	      !contains(log, "--type") && !contains(out, "config:"),
	      "installed with type_ada and type_ampere: the steps run without --type: %d\n%s\n%s", rc, out, log ? log : "");
	free(log);
	free(out);
	writef(cfg, "bdfs=0000:03:00.0\n");
	/* A --device other than the configured GPUs is refused, naming it. */
	gs_addr_parse("0000:04:00.0", &o.devices[0]);
	rc = run_setup(root, NULL, &o, 1, &out);
	log = slurp(setup_log);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "setup: --device 0000:04:00.0 is not a configured GPU (bdfs=0000:03:00.0") &&
	      !log, "installed, another --device: refused before any step: %d\n%s", rc, out);
	free(log);
	free(out);
	/*
	 * The first install's command line again on the installed host, as its closing text says: each option
	 * only a first install takes gets one note, nothing of them reaches a step, and the run goes on.
	 */
	setup_opts(&o);
	o.driver_file = "/x/NVIDIA-Linux-x86_64-580.159.01-vgpu-kvm.run";
	o.accept_nvidia_license = 1;
	o.vfs = 2;
	gs_addr_parse("0000:03:00.0", &o.devices[0]);
	o.ndevices = 1;
	setup_config = NULL;
	setup_check_rc = 0;
	rc = run_setup(root, NULL, &o, 1, &out);
	log = slurp(setup_log);
	CHECK(rc == 0 && contains(log, "R install --device 0000:03:00.0 --vfs 2 --driver-file /x/") &&
	      contains(out, "\nGuest drivers:\n") &&
	      contains(out, "\n  Every guest driver needs RMSetVgpuGspPluginOffloadMode=0.\n"
			    "  Use these versions; other versions are not supported.\n"
			    "  Setting and steps: docs/installation.md#guests.\n"
			    "  Attaching a VF to a VM is your step: docs/installation.md#create-a-vm.\n") &&
	      !contains(out, "The same setup command works after the boot") &&
	      !contains(out, "reboot") && !contains(out, "Power the machine") && !contains(out, "geforce-sriov status"),
	      "first install with options: installed, setup adds no note of its own to install's closing: %d\n%s", rc, out);
	free(log);
	free(out);
	writef(cfg, "bdfs=0000:03:00.0\n");
	setup_config = cfg;
	rc = run_setup(root, NULL, &o, 1, &out);
	log = slurp(setup_log);
	CHECK(rc == 0 && contains(out, "install already ran, so --vfs is ignored: it applies only to a first install\n") &&
	      contains(out, "install already ran, so --driver-file is ignored: it applies only to a first install\n") &&
	      count_of(out, "install already ran, so ") == 2 && contains(out, "Every VF is ready.") &&
	      contains(log, "R check\n") && !contains(log, "--driver-file") && !contains(log, "--vfs"),
	      "installed, the same command again: notes, exit 0: %d\n%s\n%s", rc, out, log ? log : "");
	free(log);
	free(out);
	setup_opts(&o);
	o.skip_driver = 1;
	o.nvidia_source = "/usr/src/nvidia-580.159.01";
	gs_addr_parse("0000:03:00.0", &o.release[0]);
	o.nrelease = 1;
	rc = run_setup(root, NULL, &o, 1, &out);
	CHECK(rc == 0 && contains(out, "install already ran, so --driver=skip is ignored: it applies only to a first install\n") &&
	      contains(out, "install already ran, so --nvidia-source is ignored: it applies only to a first install\n") &&
	      contains(out, "install already ran, so --boot-display-release is ignored: it applies only to a first install\n"),
	      "installed, --driver=skip, --nvidia-source, --boot-display-release: notes, exit 0: %d\n%s", rc, out);
	free(out);
	setup_config = NULL;
	setup_check_rc = 0;
}

#define RELEASE_NOW_Q "Release the console of 0000:03:00.0 and bring the GPUs up now? [y/N]"
#define RELEASE_NOW_NO "setup: releasing the console needs a yes. Run setup on a terminal, or run these commands:\n" \
	RELEASE_STEPS("0000:03:00.0", "  ")

/*
 * An installed host whose config grants a console release, after a boot: check not ready and NVIDIA's
 * driver not loaded. A yes runs prepare --release-console, modprobe --ignore-install nvidia, activate and
 * check, stopping at the first failure; no names them and exits 0; no question or no answer refuses with
 * them, exit 3. With nvidia loaded setup stops before prepare, exit 3. Released already this boot: with
 * nvidia loaded, activate --boot as before; without (an earlier modprobe failed), modprobe, activate and
 * check, asking nothing.
 */
static void test_setup_release(void)
{
	static const struct fx_fn one[] = { { "0000:03:00.0", "2206", "030000", NULL }, { NULL, NULL, NULL, NULL } };
	char root[GS_PATH_MAX], cfg[GS_PATH_MAX], p[GS_PATH_MAX], q[GS_PATH_MAX], boot[64], *out, *log;
	struct gs_install_opts o;
	int rc;

	fixture("setup-release", one, 1, NULL, root, sizeof(root));
	gs_pathf(cfg, sizeof(cfg), "%s/installed-config", root);
	writef(cfg, "bdfs=0000:03:00.0\nboot_display_release=0000:03:00.0\n");
	setup_config = cfg;
	setup_opts(&o);
	setup_doctor_rc = 0;
	setup_check_rc = GS_EXIT_FAIL;

	rc = run_setup(root, "y\n", &o, 1, &out);
	log = slurp(setup_log);
	CHECK(rc == GS_EXIT_FAIL && contains(out, RELEASE_NOW_Q "\n  the local console on 0000:03:00.0 stops until the "
						  "next boot; SSH keeps working; default is no\n") &&
	      contains(log, "R check\nR prepare --release-console=0000:03:00.0\nR modprobe --ignore-install nvidia\n"
			    "R activate\nR check\n") && contains(out, "\n== modprobe --ignore-install nvidia\n") &&
	      contains(out, "Guest drivers:"),
	      "yes: prepare with the consent, modprobe, activate, check, the result: %d\n%s\n%s", rc, out, log ? log : "");
	free(log);
	free(out);

	setup_fail = "modprobe";
	setup_fail_rc = 1;
	rc = run_setup(root, "yes\n", &o, 1, &out);
	log = slurp(setup_log);
	CHECK(rc == GS_EXIT_FAIL && contains(out, "Stopped: modprobe --ignore-install nvidia exited with 1.\n") &&
	      contains(log, "R modprobe --ignore-install nvidia\n") && !contains(log, "R activate"),
	      "yes, modprobe fails: stopped with its exit status: %d\n%s\n%s", rc, out, log ? log : "");
	free(log);
	free(out);
	setup_fail = "prepare";
	setup_fail_rc = GS_EXIT_REFUSED;
	rc = run_setup(root, "y\n", &o, 1, &out);
	log = slurp(setup_log);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "Stopped: prepare exited with 3.\n") && !contains(log, "R modprobe"),
	      "yes, prepare refuses: stopped with its exit status: %d\n%s\n%s", rc, out, log ? log : "");
	free(log);
	free(out);
	setup_fail = NULL;

	rc = run_setup(root, "n\n", &o, 1, &out);
	log = slurp(setup_log);
	CHECK(rc == 0 && contains(out, "Nothing changed. To bring the GPUs up yourself, as root, in this order:\n"
				       RELEASE_STEPS("0000:03:00.0", "  ")) &&
	      !contains(out, "or run geforce-sriov setup") && !contains(log, "R prepare") && !contains(log, "R modprobe"),
	      "no: the commands, nothing run, exit 0: %d\n%s\n%s", rc, out, log ? log : "");
	free(log);
	free(out);

	rc = run_setup(root, NULL, &o, 1, &out);
	log = slurp(setup_log);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, RELEASE_NOW_NO) && !contains(out, RELEASE_NOW_Q) &&
	      contains(log, "R check\n") && !contains(log, "R prepare") && !contains(log, "R modprobe"),
	      "no prompt: refused with the commands, exit 3: %d\n%s\n%s", rc, out, log ? log : "");
	free(log);
	free(out);
	rc = run_setup(root, "", &o, 1, &out);
	log = slurp(setup_log);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, RELEASE_NOW_Q) && contains(out, RELEASE_NOW_NO) &&
	      !contains(out, "Give ") && !contains(log, "R prepare"),
	      "end of input at the question: as without a prompt: %d\n%s\n%s", rc, out, log ? log : "");
	free(log);
	free(out);

	/* nvidia loaded: the console card cannot be released this boot; stop before prepare, exit 3. */
	gs_pathf(p, sizeof(p), "%s/driver/nvidia/version", root);
	writef(p, "NVRM version: NVIDIA UNIX x86_64 Kernel Module  " GS_NVIDIA_VERSION "\n");
	rc = run_setup(root, NULL, &o, 1, &out);
	log = slurp(setup_log);
	CHECK(rc == GS_EXIT_REFUSED && !contains(out, "console needs a yes") &&
	      contains(out, "setup: NVIDIA's driver is loaded, so the console GPU cannot be released in this boot. Create the "
			    "hold (docs/installation.md#boot-display-card), power the machine off and on, then run "
			    "setup again.\n") &&
	      contains(log, "R check\n") && !contains(log, "R prepare") && !contains(log, "R activate"),
	      "nvidia loaded: stopped before prepare, exit 3: %d\n%s\n%s", rc, out, log ? log : "");
	free(log);
	free(out);
	unlink(p);
	/* Released this boot already, an earlier modprobe failed: modprobe, activate and check, no question. */
	gs_boot_id(boot, sizeof(boot));
	gs_pathf(p, sizeof(p), "%s/" GS_PREPARED_NAME "/0000:03:00.0", root);
	writef(p, "boot_id=%s\ntotalvfs=4\nari_enabled=1\nconsole_release=simple-framebuffer.0\n", boot);
	rc = run_setup(root, NULL, &o, 1, &out);
	log = slurp(setup_log);
	CHECK(rc == GS_EXIT_FAIL && !contains(out, "console needs a yes") && !contains(out, RELEASE_NOW_Q) &&
	      contains(out, "The console was released this boot and NVIDIA's driver is not loaded: loading it.\n") &&
	      contains(log, "R check\nR modprobe --ignore-install nvidia\nR activate\nR check\n") &&
	      !contains(log, "R prepare") && !contains(log, "--boot") && contains(out, "Guest drivers:"),
	      "released this boot, nvidia not loaded: modprobe, activate, check: %d\n%s\n%s", rc, out, log ? log : "");
	free(log);
	free(out);
	setup_fail = "modprobe";
	setup_fail_rc = 1;
	rc = run_setup(root, NULL, &o, 1, &out);
	log = slurp(setup_log);
	CHECK(rc == 1 && contains(out, "Stopped: modprobe --ignore-install nvidia exited with 1.\n") && !contains(log, "R activate"),
	      "released this boot, modprobe fails again: stopped with its exit status: %d\n%s\n%s", rc, out, log ? log : "");
	free(log);
	free(out);
	setup_fail = NULL;
	/* Released this boot and nvidia loaded: activate --boot as before. */
	gs_pathf(q, sizeof(q), "%s/driver/nvidia/version", root);
	writef(q, "NVRM version: NVIDIA UNIX x86_64 Kernel Module  " GS_NVIDIA_VERSION "\n");
	rc = run_setup(root, NULL, &o, 1, &out);
	log = slurp(setup_log);
	CHECK(rc == GS_EXIT_FAIL && !contains(out, "console needs a yes") && !contains(log, "R modprobe") &&
	      contains(log, "R check\nR activate --boot\nR check\n") && !contains(log, "R prepare"),
	      "released this boot, nvidia loaded: activate --boot as before: %d\n%s\n%s", rc, out, log ? log : "");
	free(log);
	free(out);
	unlink(q);
	setup_config = NULL;
	setup_check_rc = 0;
}

#define SETUP_STALE "setup: the running nvidia module is not the installed one. Power the machine off and on, " \
	"then run setup again.\n"

/*
 * An installed host whose nvidia.ko is the driver step's build, check not ready. A loaded nvidia module of
 * another build stops setup after check, before prepare and activate, exit 3; the loaded installed build,
 * or no loaded module, runs as before.
 */
static void test_setup_loaded_build(void)
{
	static const struct fx_fn one[] = { { "0000:03:00.0", "2206", "030000", NULL }, { NULL, NULL, NULL, NULL } };
	char root[GS_PATH_MAX], cfg[GS_PATH_MAX], module[GS_PATH_MAX], record[GS_PATH_MAX], prepared[GS_PATH_MAX];
	char note[GS_PATH_MAX], boot[64], *out, *log;
	struct gs_build_src src;
	struct gs_install_opts o;
	int rc;

	fixture("setup-loaded-build", one, 0, NULL, root, sizeof(root));
	gs_pathf(cfg, sizeof(cfg), "%s/installed-config", root);
	writef(cfg, "bdfs=0000:03:00.0\n");
	gs_pathf(module, sizeof(module), "%s/lib/modules/" BUILD_REL "/updates/dkms/nvidia.ko", root);
	gs_pathf(record, sizeof(record), "%s/state/" GS_DRIVER_BUILD_NAME, root);
	write_module(module, BUILD_ID);
	writef(record, "kernel=" BUILD_REL "\nmodule_build_id=" BUILD_ID "\n");
	gs_pathf(note, sizeof(note), "%s/module/nvidia/notes/.note.gnu.build-id", root);
	src.record = record;
	src.release = BUILD_REL;
	src.module = module;
	setup_build = &src;
	setup_config = cfg;
	setup_opts(&o);
	setup_doctor_rc = 0;
	setup_check_rc = GS_EXIT_FAIL;

	/* Prepared this boot, the loaded module another build: stopped after check, nothing run. */
	gs_boot_id(boot, sizeof(boot));
	gs_pathf(prepared, sizeof(prepared), "%s/" GS_PREPARED_NAME "/0000:03:00.0", root);
	writef(prepared, "boot_id=%s\ntotalvfs=4\nari_enabled=1\n", boot);
	write_loaded_note(root, OTHER_ID);
	rc = run_setup(root, NULL, &o, 1, &out);
	log = slurp(setup_log);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, SETUP_STALE) && pos_of(log, "R doctor ") == 0 &&
	      strcmp(strchr(log, '\n'), "\nR check\n") == 0,
	      "prepared, loaded module another build: refused after check, no prepare or activate: %d\n%s\n%s", rc, out,
	      log ? log : "");
	free(log);
	free(out);
	/* Not prepared this boot: refused the same way, before prepare. */
	unlink(prepared);
	rc = run_setup(root, NULL, &o, 1, &out);
	log = slurp(setup_log);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, SETUP_STALE) && pos_of(log, "R doctor ") == 0 &&
	      strcmp(strchr(log, '\n'), "\nR check\n") == 0,
	      "not prepared, loaded module another build: refused before prepare: %d\n%s\n%s", rc, out, log ? log : "");
	free(log);
	free(out);
	writef(prepared, "boot_id=%s\ntotalvfs=4\nari_enabled=1\n", boot);
	/* The loaded module is the installed build: activate --boot as before. */
	write_loaded_note(root, BUILD_ID);
	rc = run_setup(root, NULL, &o, 1, &out);
	log = slurp(setup_log);
	CHECK(rc == GS_EXIT_FAIL && !contains(out, "setup: the running nvidia module") && pos_of(log, "R doctor ") == 0 &&
	      strcmp(strchr(log, '\n'), "\nR check\nR activate --boot\nR check\n") == 0,
	      "prepared, loaded module the installed build: activate --boot as before: %d\n%s\n%s", rc, out, log ? log : "");
	free(log);
	free(out);
	/* No loaded module: as before. */
	unlink(note);
	rc = run_setup(root, NULL, &o, 1, &out);
	log = slurp(setup_log);
	CHECK(rc == GS_EXIT_FAIL && !contains(out, "setup: the running nvidia module") && pos_of(log, "R doctor ") == 0 &&
	      strcmp(strchr(log, '\n'), "\nR check\nR activate --boot\nR check\n") == 0,
	      "prepared, no loaded module: activate --boot as before: %d\n%s\n%s", rc, out, log ? log : "");
	free(log);
	free(out);
	setup_build = NULL;
	setup_config = NULL;
	setup_check_rc = 0;
}

/* ---- an NVIDIA driver already installed ---------------------------------- */

#define DRV_KERNEL "6.1.0-gs-test"
#define DRV_DONE "geforce-sriov-driver: NVIDIA's module and plugin are prepared\n"

static void drv_stub(const char *dir, const char *name, const char *text)
{
	char p[GS_PATH_MAX];

	gs_pathf(p, sizeof(p), "%s/bin/%s", dir, name);
	writef(p, "%s", text);
	if (chmod(p, 0755))
		die(p);
}

/* A .text image of zero bytes; the patched one differs at exactly the driver step's eight offsets. */
static void drv_text(const char *path, int patched)
{
	static const off_t at[] = { 0x14a7c4, 0x14a7c5, 0x14a7c6, 0x14a7c7, 0xa16708, 0xa16709, 0xd8966f, 0xd89670 };
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (fd < 0 || ftruncate(fd, 0xd89671))
		die(path);
	for (size_t i = 0; patched && i < sizeof(at) / sizeof(at[0]); i++)
		if (pwrite(fd, "\x90", 1, at[i]) != 1)
			die(path);
	close(fd);
}

/*
 * geforce-sriov-driver against a fixture host below dir/root, with the tools it runs on NVIDIA's
 * files stubbed in dir/bin ahead of the system tools. Files hold marker text: the sha256sum stub
 * answers the pinned digests for the vendor object, the patched object, the stock plugin, the plugin
 * after plugin-mixed and the patched plugin copy, and a
 * module is "text=" and "id=" lines that the objcopy and readelf stubs read; objcopy copies
 * dir/<text>.text. The build is .text AAAA (the patched image), build-id built-id; BBBB is the stock
 * image. The installed module has .text installed_text, build-id installed-id.
 */
static void drv_fixture(const char *dir, const char *installed_text, const char *license)
{
	char p[GS_PATH_MAX], text[3 * GS_PATH_MAX];

	gs_pathf(p, sizeof(p), "%s/root/lib/modules/" DRV_KERNEL "/updates/dkms/nvidia.ko", dir);
	writef(p, "text=%s\nid=installed-id\n", installed_text);
	gs_pathf(p, sizeof(p), "%s/root/lib/modules/" DRV_KERNEL "/build/Makefile", dir);
	writef(p, "\n");
	gs_pathf(p, sizeof(p), "%s/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so.580.159.01", dir);
	writef(p, "stock-plugin\n");
	gs_pathf(p, sizeof(p), "%s/src/nvidia/nv-kernel.o_binary", dir);
	writef(p, "stock-object\n");
	gs_pathf(p, sizeof(p), "%s/built.ko", dir);
	writef(p, "text=AAAA\nid=built-id\n");
	gs_pathf(p, sizeof(p), "%s/license", dir);
	writef(p, "%s\n", license);
	gs_pathf(p, sizeof(p), "%s/AAAA.text", dir);
	drv_text(p, 1);
	gs_pathf(p, sizeof(p), "%s/BBBB.text", dir);
	drv_text(p, 0);
	drv_stub(dir, "sha256sum",
		 "#!/bin/sh\n"
		 "case $(cat \"$1\") in\n"
		 "stock-object) d=4e3abe645b6873ed478adea6a06c6955352ff06186849edeea536c9630596449 ;;\n"
		 "patched-object) d=946b27b8bcf0f08285335f47904516860c65b726619e5a36d89785981c70ba29 ;;\n"
		 "stock-plugin) d=7e52b76dc7ca8490e7ffbc1fd724f5db07f3791a3a2ec07f2d595bf05abdcaa7 ;;\n"
		 "mixed-plugin) d=7ddfad458bbfdb0c4d6902a9e045bd15c84f63aea19314f3d3a60967224b73c6 ;;\n"
		 "patched-plugin) d=aa4fdeb5288b5e56e7d4972466813d78e10cfb428fcf62fb53b84fec718cf07c ;;\n"
		 "*) PATH=/usr/bin:/bin exec sha256sum \"$1\" ;;\n"
		 "esac\n"
		 "printf '%s  %s\\n' \"$d\" \"$1\"\n");
	drv_stub(dir, "geforce-sriov-patch",
		 "#!/bin/sh\n"
		 "for out; do :; done\n"
		 "eval \"in=\\${$(($# - 1))}\"\n"
		 "case $(cat \"$in\") in stock-plugin) echo mixed-plugin ;; mixed-plugin) echo patched-plugin ;;\n"
		 "*) echo patched-object ;; esac >\"$out\"\n");
	snprintf(text, sizeof(text), "#!/bin/sh\ncp \"%s/$(sed -n 's/^text=//p' \"$5\").text\" \"$6\"\n", dir);
	drv_stub(dir, "objcopy", text);
	drv_stub(dir, "readelf", "#!/bin/sh\nprintf '    Build ID: %s\\n' \"$(sed -n 's/^id=//p' \"$2\")\"\n");
	drv_stub(dir, "depmod", "#!/bin/sh\nexit 0\n");
	snprintf(text, sizeof(text), "#!/bin/sh\ncp '%s/built.ko' \"$2/nvidia.ko\"\n", dir);
	drv_stub(dir, "make", text);
	snprintf(text, sizeof(text),
		 "#!/bin/sh\n"
		 "case $3 in\n"
		 "-n) echo '%s/root/lib/modules/'\"$2\"'/updates/dkms/nvidia.ko' ;;\n"
		 "-F) case $4 in vermagic) echo \"$2 SMP mod_unload\" ;; license) cat '%s/license' ;; esac ;;\n"
		 "esac\n", dir, dir);
	drv_stub(dir, "modinfo", text);
}

/*
 * The driver step on the fixture at dir, with extra arguments; its exit status and output. script NULL: the
 * source tree's; path NULL: dir/bin (the stubs) ahead of the system tools. env_path, when not NULL, is the
 * child's only environment variable (as `env -i PATH=...`).
 */
static int drv_exec(const char *dir, const char *script_path, const char *env_path, const char *const extra[], char **out)
{
	char script[GS_PATH_MAX], src[GS_PATH_MAX], root[GS_PATH_MAX], path[GS_PATH_MAX + 64], envbuf[GS_PATH_MAX + 64];
	const char *argv[16] = { "sh", script, "--source", src, "--kernel", DRV_KERNEL, "--root", root };
	char *envp[] = { envbuf, NULL };
	unsigned int n = 8;
	int pipefd[2];
	pid_t pid;

	if (script_path)
		gs_pathf(script, sizeof(script), "%s", script_path);
	else
		gs_pathf(script, sizeof(script), "%s/scripts/geforce-sriov-driver.sh", srcdir);
	gs_pathf(src, sizeof(src), "%s/src", dir);
	gs_pathf(root, sizeof(root), "%s/root", dir);
	snprintf(path, sizeof(path), "%s/bin:/usr/sbin:/usr/bin:/sbin:/bin", dir);
	if (env_path)
		snprintf(envbuf, sizeof(envbuf), "PATH=%s", env_path);
	for (; *extra && n < 15; extra++)
		argv[n++] = *extra;
	argv[n] = NULL;
	if (pipe(pipefd))
		die("pipe");
	pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0) {
		dup2(pipefd[1], 1);
		dup2(pipefd[1], 2);
		close(pipefd[0]);
		if (env_path) {
			execve("/bin/sh", (char *const *)argv, envp);
			_exit(127);
		}
		setenv("PATH", path, 1);
		setenv("TMPDIR", dir, 1);
		execv("/bin/sh", (char *const *)argv);
		_exit(127);
	}
	close(pipefd[1]);
	return collect(pipefd[0], pid, out);
}

static int drv_run(const char *dir, const char *const extra[], char **out)
{
	return drv_exec(dir, NULL, NULL, extra, out);
}

/*
 * A module already this build with nothing recorded: refused, naming --driver=skip, nothing
 * written. The open flavour: a warning, and the run goes on.
 */
static void test_driver_step(void)
{
	char dir[GS_PATH_MAX], module[GS_PATH_MAX], backup[GS_PATH_MAX], record[GS_PATH_MAX], config[GS_PATH_MAX];
	char build[GS_PATH_MAX], lnk[GS_PATH_MAX], want[3 * GS_PATH_MAX], *out, *text;
	int rc;

	gs_pathf(dir, sizeof(dir), "%s/drv-built", tmp);
	drv_fixture(dir, "AAAA", "NVIDIA");
	gs_pathf(record, sizeof(record), "%s/root/var/lib/geforce-sriov/driver-build", dir);
	gs_pathf(config, sizeof(config), "%s/root/etc/modprobe.d/geforce-sriov-nvidia.conf", dir);
	gs_pathf(backup, sizeof(backup), "%s/root/var/lib/geforce-sriov/stock/" DRV_KERNEL "/nvidia.ko", dir);
	gs_pathf(build, sizeof(build), "%s/root/var/lib/geforce-sriov/build/" DRV_KERNEL, dir);
	gs_pathf(lnk, sizeof(lnk), "%s/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so", dir);
	rc = drv_run(dir, (const char *const[]){ NULL }, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "is already this build, but ") &&
	      contains(out, " is missing or wrong. Reinstall the stock module first, or install with --driver=skip to keep "
			    "this module as it is\n") &&
	      !gs_exists(record) && !gs_exists(config) && !gs_exists(backup) && !gs_exists(build) && !gs_exists(lnk),
	      "already this build, unrecorded: refused naming --driver=skip, nothing written: %d\n%s", rc, out);
	free(out);

	/* The open flavour: one warning, then the usual install with a backup. */
	gs_pathf(dir, sizeof(dir), "%s/drv-open", tmp);
	drv_fixture(dir, "BBBB", "Dual MIT/GPL");
	gs_pathf(module, sizeof(module), "%s/root/lib/modules/" DRV_KERNEL "/updates/dkms/nvidia.ko", dir);
	gs_pathf(backup, sizeof(backup), "%s/root/var/lib/geforce-sriov/stock/" DRV_KERNEL "/nvidia.ko", dir);
	gs_pathf(record, sizeof(record), "%s/root/var/lib/geforce-sriov/driver-build", dir);
	rc = drv_run(dir, (const char *const[]){ NULL }, &out);
	snprintf(want, sizeof(want), "geforce-sriov-driver: warning: %s is NVIDIA's open flavour (licence Dual MIT/GPL). This "
		 "stack needs the proprietary one: the module built here replaces it, and `uninstall` puts the open one "
		 "back\n", module);
	CHECK(rc == 0 && count_of(out, "open flavour") == 1 && contains(out, want) &&
	      contains(out, DRV_DONE),
	      "open flavour: warned, continued: %d\n%s", rc, out);
	/* The plugin copy: plugin-mixed on the stock plugin, then plugin-div4 on its result. */
	CHECK(pos_of(out, "' 'ada' 'apply' '") >= 0 && pos_of(out, "libnvidia-vgpu.so.580.159.01' '") >= 0 &&
	      pos_of(out, "libnvidia-vgpu.so.580.159.01' '") < pos_of(out, "/patched-plugin.mixed' '"),
	      "the step applies plugin-mixed and then plugin-div4:\n%s", out);
	/* The step names the copy it installed, with the start of its digest, before its last line. */
	snprintf(want, sizeof(want), "geforce-sriov-driver: patched plugin copy installed: %s/root/usr/lib/x86_64-linux-gnu/"
		 "libnvidia-vgpu.so.580.159.01.geforce-sriov (sha256 aa4fdeb5288b5e56)\n", dir);
	CHECK(contains(out, want) && pos_of(out, want) < pos_of(out, DRV_DONE),
	      "the patched plugin copy named with its digest, before the last line:\n%s", out);
	free(out);
	text = slurp(backup);
	CHECK(text && strcmp(text, "text=BBBB\nid=installed-id\n") == 0, "open flavour: the open module is the backup");
	free(text);
	text = slurp(record);
	CHECK(contains(text, "module_build_id=built-id\n"), "open flavour: the record names the build:\n%s", text);
	free(text);
	{
		char msg[3 * GS_PATH_MAX + 256], target[GS_PATH_MAX];
		struct stat st;
		ssize_t n;

		gs_pathf(want, sizeof(want), "%s/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so.580.159.01.geforce-sriov", dir);
		gs_pathf(lnk, sizeof(lnk), "%s/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so", dir);
		text = slurp(want);
		CHECK(stat(want, &st) == 0 && (st.st_mode & 07777) == 0755 && text && strcmp(text, "patched-plugin\n") == 0,
		      "the patched plugin copy, both patches, has mode 0755: %o %s", (unsigned int)(st.st_mode & 07777),
		      text ? text : "(none)");
		free(text);
		/* A copy an earlier install left 0644 gets 0755 on a rerun that otherwise changes nothing. */
		if (chmod(want, 0644))
			die(want);
		rc = drv_run(dir, (const char *const[]){ NULL }, &out);
		snprintf(msg, sizeof(msg), "geforce-sriov-driver: set mode 0755 on %s\n", want);
		CHECK(rc == 0 && contains(out, "already prepared for " DRV_KERNEL) && contains(out, "; nothing to do\n") &&
		      contains(out, msg) && stat(want, &st) == 0 && (st.st_mode & 07777) == 0755,
		      "rerun: the patched plugin copy back to mode 0755: %d %o\n%s", rc, (unsigned int)(st.st_mode & 07777), out);
		free(out);

		/* An earlier install's copy with plugin-mixed only: the rerun on a prepared host replaces it and relinks. */
		writef(want, "mixed-plugin\n");
		if (chmod(want, 0644))
			die(want);
		link_to("libnvidia-vgpu.so.580.159.01", lnk);
		rc = drv_run(dir, (const char *const[]){ NULL }, &out);
		snprintf(msg, sizeof(msg), "geforce-sriov-driver: installed the patched plugin copy %s (SHA-256 "
			 "aa4fdeb5288b5e56e7d4972466813d78e10cfb428fcf62fb53b84fec718cf07c)\n", want);
		text = slurp(want);
		n = readlink(lnk, target, sizeof(target) - 1);
		target[n > 0 ? n : 0] = '\0';
		CHECK(rc == 0 && contains(out, "already prepared for " DRV_KERNEL) &&
		      contains(out, "; replacing the plugin copy, which is not the patched one\n") &&
		      !contains(out, "nothing to do") && contains(out, msg) && contains(out, "/patched-plugin.mixed' '") &&
		      text && strcmp(text, "patched-plugin\n") == 0 && stat(want, &st) == 0 && (st.st_mode & 07777) == 0755 &&
		      strcmp(target, "libnvidia-vgpu.so.580.159.01.geforce-sriov") == 0,
		      "rerun, prepared host with the mixed-only copy: replaced with the patched plugin copy: %d %s %s\n%s", rc,
		      text ? text : "(none)", target, out);
		free(text);
		free(out);

		/* The same on the record rewrite: the record lost, the module already this build. */
		writef(want, "mixed-plugin\n");
		unlink(record);
		rc = drv_run(dir, (const char *const[]){ NULL }, &out);
		text = slurp(want);
		CHECK(rc == 0 && contains(out, "recorded the installed module for " DRV_KERNEL "\n") && contains(out, msg) &&
		      gs_exists(record) && text && strcmp(text, "patched-plugin\n") == 0,
		      "record rewrite with the mixed-only copy: recorded, copy replaced: %d %s\n%s", rc, text ? text : "(none)", out);
		free(text);
		free(out);

		/* Any other copy there is not this step's: the record rewrite refuses it as incomplete. */
		writef(want, "another-plugin\n");
		unlink(record);
		rc = drv_run(dir, (const char *const[]){ NULL }, &out);
		text = slurp(want);
		CHECK(rc == GS_EXIT_REFUSED && contains(out, "is already this build, but ") &&
		      !gs_exists(record) && text && strcmp(text, "another-plugin\n") == 0,
		      "record rewrite with another copy: refused, nothing written: %d\n%s", rc, out);
		free(text);
		free(out);
		writef(want, "patched-plugin\n");
		rc = drv_run(dir, (const char *const[]){ NULL }, &out);
		CHECK(rc == 0 && contains(out, "recorded the installed module for " DRV_KERNEL "\n") && !contains(out, msg),
		      "record rewrite with the patched plugin copy: recorded, copy kept: %d\n%s", rc, out);
		free(out);
	}
	/* A lost backup of a module this step installed is refused. */
	unlink(backup);
	rc = drv_run(dir, (const char *const[]){ "--restore", NULL }, &out);
	text = slurp(module);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "refused: backup missing: ") && gs_exists(record) && text &&
	      strcmp(text, "text=AAAA\nid=built-id\n") == 0, "--restore without a backup: refused: %d\n%s", rc, out);
	free(text);
	free(out);
}

/* --dry-run names the installed module where its plan copies it, or a placeholder when modinfo does not know it. */
static void test_driver_dry_run(void)
{
	char dir[GS_PATH_MAX], want[3 * GS_PATH_MAX], *out;
	int rc;

	gs_pathf(dir, sizeof(dir), "%s/drv-dry", tmp);
	drv_fixture(dir, "BBBB", "NVIDIA");
	rc = drv_run(dir, (const char *const[]){ "--dry-run", NULL }, &out);
	snprintf(want, sizeof(want),
		 "+ test -f '%s/root/lib/modules/" DRV_KERNEL "/updates/dkms/nvidia.ko'\n"
		 "+ test -x ", dir);
	CHECK(rc == 0 && contains(out, want) && !contains(out, "''"), "dry run: the module path from modinfo: %d\n%s", rc, out);
	snprintf(want, sizeof(want), "+ cp -p '%s/root/lib/modules/" DRV_KERNEL "/updates/dkms/nvidia.ko' '%s/root/var/lib/"
		 "geforce-sriov/stock/" DRV_KERNEL "/nvidia.ko'", dir, dir);
	CHECK(contains(out, want), "dry run: the backup copy names the module:\n%s", out);
	free(out);
	/* No modinfo stub ahead of the system tools, which do not know the fixture kernel. */
	rc = drv_exec(dir, NULL, "/usr/bin:/bin", (const char *const[]){ "--dry-run", NULL }, &out);
	CHECK(rc == 0 && contains(out, "+ test -f '<installed nvidia.ko for " DRV_KERNEL ">'\n") && !contains(out, "''"),
	      "dry run without the module: a named placeholder: %d\n%s", rc, out);
	free(out);
}

/* gs_driver_step on argv with log in a child; its exit status, and its output followed by "why=<reason>". */
static int run_driver_step(const char *const argv[], const char *log, char **out)
{
	int pipefd[2];
	pid_t pid;

	if (pipe(pipefd))
		die("pipe");
	pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0) {
		char why[256];
		int rc;

		dup2(pipefd[1], 1);
		dup2(pipefd[1], 2);
		close(pipefd[0]);
		rc = gs_driver_step(argv, log, why, sizeof(why));
		printf("why=%s\n", why);
		fflush(stdout);
		fflush(stderr);
		_exit(rc);
	}
	close(pipefd[1]);
	return collect(pipefd[0], pid, out);
}

/*
 * install's driver step: its whole output goes to the log, rewritten with mode 0600; the screen shows only
 * its own lines, and on a failure the log's last 40 lines and the log's path.
 */
static void test_driver_step_log(void)
{
	char script[GS_PATH_MAX], log[GS_PATH_MAX], want[2 * GS_PATH_MAX], *out, *text;
	const char *const argv[] = { script, NULL };
	struct stat st;
	int rc;

	gs_pathf(script, sizeof(script), "%s/driver-log/step", tmp);
	gs_pathf(log, sizeof(log), "%s/driver-log/driver-step.log", tmp);
	writef(log, "an earlier run\n");
	if (chmod(log, 0644))
		die(log);
	writef(script, "#!/bin/sh\n"
		       "echo '+ make -C tree modules'\n"
		       "i=0; while [ $i -lt 100 ]; do echo \"build line $i\" >&2; i=$((i + 1)); done\n"
		       "echo 'geforce-sriov-driver: patched plugin copy installed: /l/p.geforce-sriov (sha256 aa4fdeb5288b5e56)'\n"
		       "echo 'geforce-sriov-driver: kept /x.conf: it already sets NVreg_EnableGpuFirmware=0'\n"
		       "echo \"geforce-sriov-driver: NVIDIA's module and plugin are prepared\"\n");
	if (chmod(script, 0755))
		die(script);
	rc = run_driver_step(argv, log, &out);
	text = slurp(log);
	CHECK(rc == 0 && strcmp(out, "geforce-sriov-driver: patched plugin copy installed: /l/p.geforce-sriov (sha256 "
				     "aa4fdeb5288b5e56)\n"
				     "geforce-sriov-driver: kept /x.conf: it already sets NVreg_EnableGpuFirmware=0\n"
				     DRV_DONE "why=\n") == 0,
	      "success: only the step's own lines on the screen: %d\n%s", rc, out);
	CHECK(stat(log, &st) == 0 && (st.st_mode & 07777) == 0600 && contains(text, "+ make -C tree modules\n") &&
	      contains(text, "build line 99\n") && contains(text, DRV_DONE) && !contains(text, "an earlier run"),
	      "success: the whole output in the log, rewritten, mode 0600: %o\n%s", (unsigned int)(st.st_mode & 07777),
	      text ? text : "");
	free(text);
	free(out);

	writef(script, "#!/bin/sh\n"
		       "i=0; while [ $i -lt 100 ]; do echo \"build line $i\"; i=$((i + 1)); done\n"
		       "echo 'geforce-sriov-driver: refused: module .text differs' >&2\n"
		       "exit 3\n");
	rc = run_driver_step(argv, log, &out);
	snprintf(want, sizeof(want), "geforce-sriov-driver: refused: module .text differs\n"
		 "Last 40 lines of the driver step (full output: %s):\nbuild line 61\n", log);
	CHECK(rc == GS_EXIT_REFUSED && pos_of(out, want) == 0 && contains(out, "build line 99\n") &&
	      !contains(out, "build line 60\n") && count_of(out, "refused: module .text differs\n") == 2 &&
	      contains(out, "why=module .text differs\n"),
	      "failure: the reason, the log's last 40 lines and its path: %d\n%s", rc, out);
	free(out);
}

/* The driver step as install leaves it: <bindir>/geforce-sriov-driver with the patcher beside it, not on PATH. */
static void drv_install_copy(const char *dir, char *script, size_t len)
{
	char src[GS_PATH_MAX], p[GS_PATH_MAX], q[GS_PATH_MAX], *text;

	gs_pathf(src, sizeof(src), "%s/scripts/geforce-sriov-driver.sh", srcdir);
	text = slurp(src);
	if (!text)
		die(src);
	gs_pathf(script, len, "%s/inst/geforce-sriov-driver", dir);
	writef(script, "%s", text);
	free(text);
	if (chmod(script, 0755))
		die(script);
	gs_pathf(p, sizeof(p), "%s/bin/geforce-sriov-patch", dir);
	gs_pathf(q, sizeof(q), "%s/inst/geforce-sriov-patch", dir);
	if (rename(p, q))
		die(q);
}

/*
 * The patcher is found beside the script under install's fixed PATH; --restore undoes the build directory,
 * the plugin link and the modprobe file whether or not the module record matches, and verifies the backup
 * against the recorded stock digest; it names the link it points back and the plugin copy it removes, and
 * names a link to another file, which stays; a leftover build directory does not stop a reinstall.
 */
static void test_driver_step_restore(void)
{
	char dir[GS_PATH_MAX], script[GS_PATH_MAX], module[GS_PATH_MAX], backup[GS_PATH_MAX], record[GS_PATH_MAX];
	char config[GS_PATH_MAX], build[GS_PATH_MAX], lnk[GS_PATH_MAX], target[GS_PATH_MAX], p[GS_PATH_MAX], *out, *text;
	char other[GS_PATH_MAX], state[GS_PATH_MAX], want[GS_PATH_MAX + 64], copy[GS_PATH_MAX], msg[2 * GS_PATH_MAX + 128];
	ssize_t n;
	int rc;

	gs_pathf(dir, sizeof(dir), "%s/drv-inst", tmp);
	drv_fixture(dir, "BBBB", "NVIDIA");
	drv_install_copy(dir, script, sizeof(script));
	gs_pathf(module, sizeof(module), "%s/root/lib/modules/" DRV_KERNEL "/updates/dkms/nvidia.ko", dir);
	gs_pathf(backup, sizeof(backup), "%s/root/var/lib/geforce-sriov/stock/" DRV_KERNEL "/nvidia.ko", dir);
	gs_pathf(record, sizeof(record), "%s/root/var/lib/geforce-sriov/driver-build", dir);
	gs_pathf(config, sizeof(config), "%s/root/etc/modprobe.d/geforce-sriov-nvidia.conf", dir);
	gs_pathf(build, sizeof(build), "%s/root/var/lib/geforce-sriov/build/" DRV_KERNEL, dir);
	gs_pathf(lnk, sizeof(lnk), "%s/root/usr/lib/x86_64-linux-gnu/libnvidia-vgpu.so", dir);

	/* install's environment: no PATH entry holds the patcher, which sits beside the script. */
	rc = drv_exec(dir, script, "/usr/bin:/bin", (const char *const[]){ NULL }, &out);
	CHECK(rc == GS_EXIT_REFUSED && !contains(out, "geforce-sriov-patch is not installed") && contains(out, "refused: "),
	      "fixed PATH, patcher beside the script: found (the run refuses later, on the fixture): %d\n%s", rc, out);
	free(out);
	if (!gs_exists("/usr/bin/geforce-sriov-patch") && !gs_exists("/bin/geforce-sriov-patch")) {
		rc = drv_exec(dir, NULL, "/usr/bin:/bin", (const char *const[]){ NULL }, &out);
		CHECK(rc == GS_EXIT_REFUSED && contains(out, "geforce-sriov-patch is not installed next to "),
		      "no patcher beside the script or on PATH: refused: %d\n%s", rc, out);
		free(out);
	}
	/* The whole step through the installed copy, with the patcher only beside it. */
	rc = drv_exec(dir, script, NULL, (const char *const[]){ NULL }, &out);
	CHECK(rc == 0 && contains(out, DRV_DONE), "installed copy: complete: %d\n%s", rc, out);
	free(out);
	text = slurp(record);
	CHECK(contains(text, "stock_sha256="), "the record holds the stock digest:\n%s", text ? text : "");
	free(text);

	/* --restore: module, build directory, link, patched plugin copy and modprobe file; then the backup of this kernel and the
	 * empty build/ go, and another kernel's backup stays. */
	gs_pathf(other, sizeof(other), "%s/root/var/lib/geforce-sriov/stock/6.0.0-gs-other/nvidia.ko", dir);
	writef(other, "text=DDDD\nid=other-kernel\n");
	rc = drv_exec(dir, script, NULL, (const char *const[]){ "--restore", NULL }, &out);
	text = slurp(module);
	n = readlink(lnk, target, sizeof(target) - 1);
	target[n > 0 ? n : 0] = '\0';
	gs_pathf(p, sizeof(p), "%s.580.159.01.geforce-sriov", lnk);
	CHECK(rc == 0 && text && strcmp(text, "text=BBBB\nid=installed-id\n") == 0 && !gs_exists(build) &&
	      strcmp(target, "libnvidia-vgpu.so.580.159.01") == 0 && !gs_exists(p) && !gs_exists(config) &&
	      !gs_exists(record), "--restore: everything put back, build directory removed: %d\n%s", rc, out);
	free(text);
	gs_pathf(p, sizeof(p), "%s/root/var/lib/geforce-sriov/stock/" DRV_KERNEL, dir);
	snprintf(want, sizeof(want), "geforce-sriov-driver: removed %s\n", p);
	gs_pathf(state, sizeof(state), "%s/root/var/lib/geforce-sriov", dir);
	CHECK(!gs_exists(p) && contains(out, want) && gs_exists(other), "--restore: this kernel's backup removed, another "
	      "kernel's kept\n%s", out);
	gs_pathf(p, sizeof(p), "%s/build", state);
	snprintf(want, sizeof(want), "geforce-sriov-driver: removed %s\n", p);
	CHECK(!gs_exists(p) && contains(out, want), "--restore: the empty build directory removed\n%s", out);
	gs_pathf(p, sizeof(p), "%s/stock", state);
	CHECK(gs_exists(p), "--restore: stock/ stays while another kernel's backup is in it");
	/* The package's link points back at the stock plugin, and the copy it removes is named. */
	snprintf(msg, sizeof(msg), "geforce-sriov-driver: pointed %s back at the stock plugin libnvidia-vgpu.so.580.159.01\n",
		 lnk);
	gs_pathf(copy, sizeof(copy), "%s.580.159.01.geforce-sriov", lnk);
	snprintf(want, sizeof(want), "geforce-sriov-driver: removed %s\n", copy);
	CHECK(contains(out, msg) && contains(out, want) && pos_of(out, msg) < pos_of(out, want) && !contains(out, ": left "),
	      "--restore: the link pointed back and the copy removed, each named\n%s", out);
	free(out);

	/* With no other kernel's backup, stock/ goes as well. */
	unlink(other);
	gs_pathf(p, sizeof(p), "%s/stock/6.0.0-gs-other", state);
	rmdir(p);
	rc = drv_exec(dir, script, NULL, (const char *const[]){ NULL }, &out);
	CHECK(rc == 0 && contains(out, DRV_DONE), "install after --restore: complete: %d\n%s", rc, out);
	free(out);
	text = slurp(backup);
	CHECK(text && strcmp(text, "text=BBBB\nid=installed-id\n") == 0, "install after --restore: a fresh backup of the "
	      "stock module: %s", text ? text : "(none)");
	free(text);
	rc = drv_exec(dir, script, NULL, (const char *const[]){ "--restore", NULL }, &out);
	gs_pathf(p, sizeof(p), "%s/stock", state);
	snprintf(want, sizeof(want), "geforce-sriov-driver: removed %s\n", p);
	CHECK(rc == 0 && !gs_exists(p) && contains(out, want) && gs_exists(state),
	      "--restore with no other backup: stock/ removed: %d\n%s", rc, out);
	free(out);

	/* A link to a file this package did not install stays and is named; the package's copy goes and is named. */
	rc = drv_exec(dir, script, NULL, (const char *const[]){ NULL }, &out);
	CHECK(rc == 0 && contains(out, DRV_DONE), "install again: complete: %d\n%s", rc, out);
	free(out);
	gs_pathf(p, sizeof(p), "%s.research", lnk);
	writef(p, "research\n");
	link_to("libnvidia-vgpu.so.research", lnk);
	rc = drv_exec(dir, script, NULL, (const char *const[]){ "--restore", NULL }, &out);
	n = readlink(lnk, target, sizeof(target) - 1);
	target[n > 0 ? n : 0] = '\0';
	snprintf(msg, sizeof(msg), "geforce-sriov-driver: left %s: it points to libnvidia-vgpu.so.research, which this "
		 "package did not install\n", lnk);
	snprintf(want, sizeof(want), "geforce-sriov-driver: removed %s\n", copy);
	CHECK(rc == 0 && contains(out, msg) && contains(out, want) && !contains(out, "back at the stock plugin") &&
	      strcmp(target, "libnvidia-vgpu.so.research") == 0 && gs_exists(p) && !gs_exists(copy),
	      "--restore, the link points elsewhere: left and named, the copy removed and named: %d\n%s", rc, out);
	free(out);
	/* A link to the stock plugin needs no line. */
	link_to("libnvidia-vgpu.so.580.159.01", lnk);
	rc = drv_exec(dir, script, NULL, (const char *const[]){ "--restore", NULL }, &out);
	CHECK(rc == 0 && !contains(out, ": left ") && !contains(out, "back at the stock plugin") && !contains(out, copy),
	      "--restore, the link at the stock plugin: nothing said of it: %d\n%s", rc, out);
	free(out);
	unlink(p);

	/* A leftover build directory is replaced, not refused. */
	gs_pathf(p, sizeof(p), "%s/leftover", build);
	writef(p, "x\n");
	rc = drv_exec(dir, script, NULL, (const char *const[]){ NULL }, &out);
	gs_pathf(p, sizeof(p), "%s/nvidia.ko", build);
	CHECK(rc == 0 && contains(out, DRV_DONE) && gs_exists(p) && !contains(out, "already exists"),
	      "reinstall over a leftover build directory: %d\n%s", rc, out);
	free(out);

	/* A backup that is not the recorded stock module is refused before anything changes. */
	writef(backup, "text=CCCC\nid=other\n");
	rc = drv_exec(dir, script, NULL, (const char *const[]){ "--restore", NULL }, &out);
	text = slurp(module);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "refused: backup ") && contains(out, " records ") && gs_exists(build) &&
	      gs_exists(config) && gs_exists(backup) && text && strcmp(text, "text=AAAA\nid=built-id\n") == 0,
	      "--restore with a changed backup: refused, nothing changed, the backup kept: %d\n%s", rc, out);
	free(text);
	free(out);

	/* A record for another kernel: the module is kept; the link, the modprobe file and the build directory are restored. */
	writef(record, "kernel=6.0.0-gs-old\nmodule_sha256=x\nmodule_build_id=built-id\nstock_sha256=y\n");
	rc = drv_exec(dir, script, NULL, (const char *const[]){ "--restore", NULL }, &out);
	text = slurp(module);
	n = readlink(lnk, target, sizeof(target) - 1);
	target[n > 0 ? n : 0] = '\0';
	CHECK(rc == 0 && contains(out, "it is not the module this script installed") && text &&
	      strcmp(text, "text=AAAA\nid=built-id\n") == 0 && strcmp(target, "libnvidia-vgpu.so.580.159.01") == 0 &&
	      !gs_exists(config) && !gs_exists(build) && gs_exists(record) && gs_exists(backup),
	      "--restore, record for another kernel: link, modprobe file and build restored, module and backup kept: %d\n%s",
	      rc, out);
	free(text);
	free(out);
}

/*
 * Another modprobe file that sets NVreg_EnableGpuFirmware: 0, on any `options nvidia` line among other
 * options, is kept and the step writes no file of its own, and --restore leaves it; any other value
 * refuses, naming the file and the value; with no other file the step writes its own as before.
 */
static void test_driver_step_modprobe(void)
{
	static const char zero[] = "# GSP off\noptions nvidia NVreg_OpenRmEnableUnsupportedGpus=1\n"
				   "options nvidia NVreg_RegistryDwords=x NVreg_EnableGpuFirmware=0 NVreg_Other=2\n"
				   "options nvidia_vgpu_vfio NVreg_EnableGpuFirmware=5\n";
	char dir[GS_PATH_MAX], config[GS_PATH_MAX], other[GS_PATH_MAX], record[GS_PATH_MAX], want[3 * GS_PATH_MAX];
	char *out, *text;
	int rc;

	gs_pathf(dir, sizeof(dir), "%s/drv-mp-zero", tmp);
	drv_fixture(dir, "BBBB", "NVIDIA");
	gs_pathf(config, sizeof(config), "%s/root/etc/modprobe.d/geforce-sriov-nvidia.conf", dir);
	gs_pathf(other, sizeof(other), "%s/root/etc/modprobe.d/nvidia-gsp-off.conf", dir);
	writef(other, "%s", zero);
	rc = drv_run(dir, (const char *const[]){ NULL }, &out);
	snprintf(want, sizeof(want), "geforce-sriov-driver: kept %s: it already sets NVreg_EnableGpuFirmware=0\n", other);
	text = slurp(other);
	CHECK(rc == 0 && contains(out, want) && contains(out, DRV_DONE) && !gs_exists(config) &&
	      text && strcmp(text, zero) == 0, "another file sets 0: kept, no file of its own: %d\n%s", rc, out);
	free(text);
	free(out);
	/* The module already this build, the record lost: the kept file stands in for the step's own. */
	gs_pathf(record, sizeof(record), "%s/root/var/lib/geforce-sriov/driver-build", dir);
	unlink(record);
	rc = drv_run(dir, (const char *const[]){ NULL }, &out);
	CHECK(rc == 0 && contains(out, "recorded the installed module for " DRV_KERNEL "\n") && gs_exists(record) &&
	      !gs_exists(config), "already this build, another file sets 0: recorded: %d\n%s", rc, out);
	free(out);
	rc = drv_run(dir, (const char *const[]){ "--restore", NULL }, &out);
	text = slurp(other);
	CHECK(rc == 0 && contains(out, "restored the stock module") && text && strcmp(text, zero) == 0 && !gs_exists(config),
	      "another file sets 0: --restore leaves it: %d\n%s", rc, out);
	free(text);
	free(out);

	/* One line sets 0 and a later one 1: refused naming the file and the value, nothing written. */
	gs_pathf(dir, sizeof(dir), "%s/drv-mp-one", tmp);
	drv_fixture(dir, "BBBB", "NVIDIA");
	gs_pathf(config, sizeof(config), "%s/root/etc/modprobe.d/geforce-sriov-nvidia.conf", dir);
	gs_pathf(other, sizeof(other), "%s/root/etc/modprobe.d/nvidia.conf", dir);
	gs_pathf(record, sizeof(record), "%s/root/var/lib/geforce-sriov/driver-build", dir);
	writef(other, "options nvidia NVreg_EnableGpuFirmware=0\noptions nvidia NVreg_X=1 NVreg_EnableGpuFirmware=1\n");
	rc = drv_run(dir, (const char *const[]){ NULL }, &out);
	snprintf(want, sizeof(want), "geforce-sriov-driver: refused: %s sets NVreg_EnableGpuFirmware=1; this stack needs 0\n",
		 other);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, want) && !gs_exists(config) && !gs_exists(record),
	      "another file sets 1: refused naming the value, nothing written: %d\n%s", rc, out);
	free(out);
	/* The option without a value is not 0: refused. */
	writef(other, "options nvidia NVreg_EnableGpuFirmware\n");
	rc = drv_run(dir, (const char *const[]){ NULL }, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, " sets NVreg_EnableGpuFirmware=; this stack needs 0\n") &&
	      !gs_exists(config), "the option without a value: refused: %d\n%s", rc, out);
	free(out);

	/* No other file: the step writes its own. */
	unlink(other);
	rc = drv_run(dir, (const char *const[]){ NULL }, &out);
	text = slurp(config);
	CHECK(rc == 0 && !contains(out, "kept ") && text && strcmp(text, "options nvidia NVreg_EnableGpuFirmware=0\n") == 0,
	      "no other file: its own file written: %d\n%s", rc, out);
	free(text);
	free(out);

	/* The source-tree and vendor-object refusals name install's --nvidia-source. */
	snprintf(want, sizeof(want), "%s/no-such-tree", dir);
	rc = drv_run(dir, (const char *const[]){ "--source", want, NULL }, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "refused: source tree missing: ") &&
	      contains(out, "with install --nvidia-source DIR (this script: --source DIR)\n"),
	      "no source tree: refused naming --nvidia-source: %d\n%s", rc, out);
	free(out);
	rc = drv_run(dir, (const char *const[]){ "--source", dir, NULL }, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "refused: vendor object missing: ") &&
	      contains(out, "with install --nvidia-source DIR (this script: --source DIR)\n"),
	      "no vendor object: refused naming --nvidia-source: %d\n%s", rc, out);
	free(out);
}

#define DRV_KERNEL_B "6.2.0-gs-next"

/*
 * One record serves every kernel: installed on kernel A, then on B, then run again on A, the step records
 * the module it already installed on A (from A's verified backup), so activate's reader matches on A. Without
 * a backup for the kernel the record cannot be completed, and the step refuses naming --driver=skip.
 */
static void test_driver_step_kernels(void)
{
	char dir[GS_PATH_MAX], p[GS_PATH_MAX], module[GS_PATH_MAX], backup[GS_PATH_MAX], record[GS_PATH_MAX], *out, *text;
	struct gs_build b;
	FILE *f;
	int rc;

	gs_pathf(dir, sizeof(dir), "%s/drv-kernels", tmp);
	drv_fixture(dir, "BBBB", "NVIDIA");
	/* The build is an ELF with a GNU build-id, so the reader activate uses can match it; the stubs read its last lines. */
	gs_pathf(p, sizeof(p), "%s/built.ko", dir);
	write_module(p, BUILD_ID);
	if (!(f = fopen(p, "a")) || fprintf(f, "\ntext=AAAA\nid=" BUILD_ID "\n") < 0 || fclose(f))
		die(p);
	gs_pathf(p, sizeof(p), "%s/root/lib/modules/" DRV_KERNEL_B "/updates/dkms/nvidia.ko", dir);
	writef(p, "text=BBBB\nid=installed-id\n");
	gs_pathf(p, sizeof(p), "%s/root/lib/modules/" DRV_KERNEL_B "/build/Makefile", dir);
	writef(p, "\n");
	gs_pathf(module, sizeof(module), "%s/root/lib/modules/" DRV_KERNEL "/updates/dkms/nvidia.ko", dir);
	gs_pathf(backup, sizeof(backup), "%s/root/var/lib/geforce-sriov/stock/" DRV_KERNEL "/nvidia.ko", dir);
	gs_pathf(record, sizeof(record), "%s/root/var/lib/geforce-sriov/driver-build", dir);

	rc = drv_run(dir, (const char *const[]){ NULL }, &out);
	CHECK(rc == 0 && contains(out, DRV_DONE), "kernel A: installed: %d\n%s", rc, out);
	free(out);
	rc = drv_run(dir, (const char *const[]){ "--kernel", DRV_KERNEL_B, NULL }, &out);
	text = slurp(record);
	CHECK(rc == 0 && contains(out, DRV_DONE) && contains(text, "kernel=" DRV_KERNEL_B "\n"),
	      "kernel B: installed, the record names B: %d\n%s", rc, out);
	free(text);
	free(out);

	/* Back on A: the record is rewritten for A, and activate's reader matches. */
	rc = drv_run(dir, (const char *const[]){ NULL }, &out);
	text = slurp(record);
	CHECK(rc == 0 && contains(out, "geforce-sriov-driver: recorded the installed module for " DRV_KERNEL "\n") &&
	      contains(text, "kernel=" DRV_KERNEL "\n") && contains(text, "module_build_id=" BUILD_ID "\n") &&
	      contains(text, "stock_sha256="), "kernel A again: the record names A: %d\n%s\n%s", rc, out, text ? text : "");
	free(text);
	free(out);
	CHECK(gs_driver_build_read(record, DRV_KERNEL, module, &b) == GS_BUILD_MATCHES,
	      "kernel A again: activate's reader matches: %d found=%s rec=%s kernel=%s", b.state, b.found_id, b.rec_id,
	      b.rec_kernel);

	/* Back on B, the same; then A without its backup: refused, the record still names B. */
	rc = drv_run(dir, (const char *const[]){ "--kernel", DRV_KERNEL_B, NULL }, &out);
	CHECK(rc == 0 && contains(out, "recorded the installed module for " DRV_KERNEL_B "\n"),
	      "kernel B again: recorded: %d\n%s", rc, out);
	free(out);
	unlink(backup);
	rc = drv_run(dir, (const char *const[]){ NULL }, &out);
	text = slurp(record);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "is already this build, but ") &&
	      contains(out, " is missing or wrong. Reinstall the stock module first, or install with --driver=skip to keep "
			    "this module as it is\n") &&
	      contains(text, "kernel=" DRV_KERNEL_B "\n"), "kernel A without its backup: refused: %d\n%s", rc, out);
	free(text);
	free(out);
}

static const char *const *uninstall_keep; /* gs_uninstall_files' keep in run_uninstall_files */

/* gs_uninstall_files on the manifest at path, with the modprobe.d directory mpd, in a child; its exit status and output. */
static int run_uninstall_files(const char *path, const char *mpd, char **out)
{
	int pipefd[2];
	pid_t pid;

	if (pipe(pipefd))
		die("pipe");
	pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0) {
		int rc;

		dup2(pipefd[1], 1);
		dup2(pipefd[1], 2);
		close(pipefd[0]);
		rc = gs_uninstall_files(path, mpd, uninstall_keep);
		fflush(stdout);
		fflush(stderr);
		_exit(rc);
	}
	close(pipefd[1]);
	return collect(pipefd[0], pid, out);
}

/* bin/ with the CLI, the patcher and a driver step that runs script_body; the manifest listing them. */
static void uninstall_fixture(const char *dir, const char *script_body, char *manifest, size_t len)
{
	static const char *const names[] = { "geforce-sriov", "geforce-sriov-patch", "geforce-sriov-driver" };
	char p[GS_PATH_MAX], sha[65], text[4 * GS_PATH_MAX] = "";
	size_t off;

	gs_pathf(p, sizeof(p), "%s/bin", dir);
	off = (size_t)snprintf(text, sizeof(text), "d - %s\n", p);
	for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
		gs_pathf(p, sizeof(p), "%s/bin/%s", dir, names[k]);
		writef(p, "%s", k == 2 ? script_body : names[k]);
		if (chmod(p, 0755) || gs_sha256_file(p, sha))
			die(p);
		off += (size_t)snprintf(text + off, sizeof(text) - off, "f %s %s\n", sha, p);
	}
	gs_pathf(manifest, len, "%s/manifest", dir);
	writef(manifest, "%s", text);
}

/*
 * uninstall when the driver step's --restore fails: the script stays (and stays in the manifest), everything
 * else is removed, exit 1, and the way back is printed. When it succeeds, everything goes, and so does the
 * manifest's directory once it is empty; the backups of other kernels stay and are named with the command
 * that puts each back. A hold of nvidia in modprobe.d is named either way and stays.
 */
static void test_uninstall_restore_failed(void)
{
	char dir[GS_PATH_MAX], manifest[GS_PATH_MAX], driver[GS_PATH_MAX], tool[GS_PATH_MAX], bin[GS_PATH_MAX];
	char want[3 * GS_PATH_MAX], guard[GS_PATH_MAX], mpd[GS_PATH_MAX], hold[GS_PATH_MAX], *out, *text;
	struct utsname u;
	int rc;

	if (uname(&u))
		die("uname");
	gs_pathf(dir, sizeof(dir), "%s/uninstall-restore", tmp);
	gs_pathf(driver, sizeof(driver), "%s/bin/geforce-sriov-driver", dir);
	gs_pathf(tool, sizeof(tool), "%s/bin/geforce-sriov", dir);
	gs_pathf(bin, sizeof(bin), "%s/bin", dir);
	gs_pathf(mpd, sizeof(mpd), "%s/uninstall-modprobe.d", tmp);
	gs_pathf(hold, sizeof(hold), "%s/nvidia-manual-load.conf", mpd);
	writef(hold, "blacklist nvidia\ninstall nvidia /bin/false\n");
	uninstall_fixture(dir,
			  "#!/bin/sh\n"
			  "echo 'geforce-sriov-driver: refused: /var/lib/geforce-sriov/driver-build records no stock_sha256' >&2\n"
			  "exit 3\n",
			  manifest, sizeof(manifest));
	rc = run_uninstall_files(manifest, mpd, &out);
	snprintf(want, sizeof(want),
		 "NVIDIA's module was not restored: /var/lib/geforce-sriov/driver-build records no stock_sha256. Fix that, "
		 "then run %s --restore, or as root: cp /var/lib/geforce-sriov/stock/%s/nvidia.ko ", driver, u.release);
	text = slurp(manifest);
	CHECK(rc == GS_EXIT_FAIL && contains(out, want) && gs_exists(driver) && !gs_exists(tool) && gs_exists(bin) &&
	      gs_exists(dir) &&
	      contains(text, "/bin/geforce-sriov-driver\n") && !contains(text, "/bin/geforce-sriov\n"),
	      "--restore fails: the script and its manifest entry stay, the rest goes, exit 1: %d\n%s\n%s", rc, out,
	      text ? text : "");
	snprintf(want, sizeof(want), " && depmod -a %s. For the plugin link and the modprobe file, see "
		 "docs/driver-patch.md#reverting.\n", u.release);
	CHECK(contains(out, want), "--restore fails: the manual way back names depmod and the reverting section\n%s", out);
	snprintf(want, sizeof(want), "%s still holds nvidia back at boot; remove it if nothing else needs it\n", hold);
	CHECK(contains(out, want) && gs_exists(hold), "--restore fails: the hold is named and stays\n%s", out);
	free(text);
	free(out);

	/* Another file beside the manifest (a guard): the directory stays. Without a hold, no hold line. */
	unlink(hold);
	gs_pathf(guard, sizeof(guard), "%s/prepare-attempt", dir);
	writef(guard, "x\n");
	uninstall_fixture(dir, "#!/bin/sh\nexit 0\n", manifest, sizeof(manifest));
	rc = run_uninstall_files(manifest, mpd, &out);
	CHECK(rc == 0 && !contains(out, "not restored") && !gs_exists(driver) && !gs_exists(tool) && !gs_exists(bin) &&
	      !gs_exists(manifest) && gs_exists(guard) && !contains(out, "holds nvidia back") &&
	      !contains(out, "backups stay"), "--restore succeeds: everything removed, the guard kept: %d\n%s", rc, out);
	free(out);
	unlink(guard);

	/* Two other kernels' backups: the directory stays, each is named with the command that puts it back. */
	{
		static const char *const kernels[] = { "6.0.0-gs-a", "6.0.0-gs-b" };
		char p[GS_PATH_MAX];

		for (size_t k = 0; k < 2; k++) {
			gs_pathf(p, sizeof(p), "%s/stock/%s/nvidia.ko", dir, kernels[k]);
			writef(p, "stock\n");
		}
		writef(hold, "blacklist nvidia\ninstall nvidia /bin/false\n");
		uninstall_fixture(dir, "#!/bin/sh\nexit 0\n", manifest, sizeof(manifest));
		rc = run_uninstall_files(manifest, mpd, &out);
		CHECK(rc == 0 && !gs_exists(manifest) && gs_exists(dir) &&
		      contains(out, "NVIDIA's module backups stay for these kernels; to put one back, as root:\n"),
		      "--restore succeeds, other kernels' backups: named: %d\n%s", rc, out);
		for (size_t k = 0; k < 2; k++) {
			snprintf(want, sizeof(want), "restore %s: cp %s/stock/%s/nvidia.ko /lib/modules/%s/updates/dkms/nvidia.ko "
				 "&& depmod -a %s\n", kernels[k], dir, kernels[k], kernels[k], kernels[k]);
			CHECK(contains(out, want), "other kernel %s: its restore line\n%s", kernels[k], out);
			gs_pathf(p, sizeof(p), "%s/stock/%s/nvidia.ko", dir, kernels[k]);
			CHECK(gs_exists(p), "other kernel %s: its backup stays", kernels[k]);
		}
		snprintf(want, sizeof(want), "%s still holds nvidia back at boot; remove it if nothing else needs it\n", hold);
		CHECK(contains(out, want) && gs_exists(hold), "--restore succeeds: the hold is named and stays\n%s", out);
		free(out);
		unlink(hold);
		gs_pathf(p, sizeof(p), "%s/stock", dir);
		cleanup(p);
	}

	uninstall_fixture(dir, "#!/bin/sh\nexit 0\n", manifest, sizeof(manifest));
	rc = run_uninstall_files(manifest, mpd, &out);
	snprintf(want, sizeof(want), "removed %s/\n", dir);
	CHECK(rc == 0 && !contains(out, "not restored") && !gs_exists(driver) && !gs_exists(tool) && !gs_exists(bin) &&
	      !gs_exists(manifest) && !gs_exists(dir) && contains(out, want),
	      "--restore succeeds: everything removed, the empty directory too: %d\n%s", rc, out);
	free(out);
}

/*
 * Files uninstall keeps (changed since install, or named in keep) stay in the manifest with their
 * directory, so a later uninstall removes them once they match again; with nothing kept the manifest goes.
 */
static void test_uninstall_kept(void)
{
	char dir[GS_PATH_MAX], manifest[GS_PATH_MAX], bin[GS_PATH_MAX], tool[GS_PATH_MAX], patch[GS_PATH_MAX];
	char driver[GS_PATH_MAX], mpd[GS_PATH_MAX], want[2 * GS_PATH_MAX], line[3][GS_PATH_MAX + 8], *out, *text;
	const char *const keep[] = { tool, NULL };
	int rc;

	gs_pathf(dir, sizeof(dir), "%s/uninstall-kept", tmp);
	gs_pathf(mpd, sizeof(mpd), "%s/uninstall-kept-modprobe.d", tmp);
	gs_pathf(bin, sizeof(bin), "%s/bin", dir);
	gs_pathf(tool, sizeof(tool), "%s/bin/geforce-sriov", dir);
	gs_pathf(patch, sizeof(patch), "%s/bin/geforce-sriov-patch", dir);
	gs_pathf(driver, sizeof(driver), "%s/bin/geforce-sriov-driver", dir);
	snprintf(line[0], sizeof(line[0]), " %s\n", patch);
	snprintf(line[1], sizeof(line[1]), "d - %s\n", bin);
	snprintf(line[2], sizeof(line[2]), " %s\n", tool);
	uninstall_fixture(dir, "#!/bin/sh\nexit 0\n", manifest, sizeof(manifest));
	writef(patch, "changed\n");
	rc = run_uninstall_files(manifest, mpd, &out);
	text = slurp(manifest);
	snprintf(want, sizeof(want), "kept %s (changed since install)\n", patch);
	CHECK(rc == 0 && contains(out, want) && gs_exists(patch) && !gs_exists(tool) && !gs_exists(driver),
	      "a changed file is kept, the rest removed: %d\n%s", rc, out);
	snprintf(want, sizeof(want), "kept %s: it lists the files kept above, so a later install or uninstall still owns them\n",
		 manifest);
	CHECK(contains(out, want) && text && count_of(text, "\n") == 2 && contains(text, line[0]) && contains(text, line[1]),
	      "the manifest keeps the changed file and its directory, nothing else:\n%s\n%s", out, text ? text : "(missing)");
	free(text);
	free(out);
	/* Put back as installed: the next uninstall removes it, the manifest and the emptied directories. */
	writef(patch, "geforce-sriov-patch");
	rc = run_uninstall_files(manifest, mpd, &out);
	CHECK(rc == 0 && !gs_exists(patch) && !gs_exists(bin) && !gs_exists(manifest) && !gs_exists(dir),
	      "unchanged again: removed with the manifest and its directory: %d\n%s", rc, out);
	free(out);

	/* A file the caller keeps (a unit whose disable failed) stays, listed, without a word from here. */
	uninstall_fixture(dir, "#!/bin/sh\nexit 0\n", manifest, sizeof(manifest));
	uninstall_keep = keep;
	rc = run_uninstall_files(manifest, mpd, &out);
	uninstall_keep = NULL;
	text = slurp(manifest);
	snprintf(want, sizeof(want), "kept %s ", tool);
	CHECK(rc == 0 && gs_exists(tool) && !gs_exists(patch) && !gs_exists(driver) && contains(text, line[2]) &&
	      !contains(out, want), "a file in keep stays in place and in the manifest: %d\n%s\n%s", rc, out,
	      text ? text : "(missing)");
	free(text);
	free(out);
	rc = run_uninstall_files(manifest, mpd, &out);
	CHECK(rc == 0 && !gs_exists(tool) && !gs_exists(manifest) && !gs_exists(dir),
	      "uninstall again: the kept file goes: %d\n%s", rc, out);
	free(out);
}

/* Another NVIDIA driver: refused before anything is extracted or run, naming what removes it. */
static void test_foreign_driver(void)
{
	char base[GS_PATH_MAX], root[GS_PATH_MAX], pkg[GS_PATH_MAX], log[GS_PATH_MAX], p[GS_PATH_MAX], *out;
	int rc;

	gs_pathf(base, sizeof(base), "%s/foreign/state", tmp);
	gs_pathf(root, sizeof(root), "%s/foreign/root", tmp);
	gs_pathf(log, sizeof(log), "%s/foreign/log", tmp);
	gs_pathf(pkg, sizeof(pkg), "%s/foreign/NVIDIA-Linux-x86_64-580.159.01-vgpu-kvm.run", tmp);
	writef(pkg, "#!/bin/sh\necho \"$*\" >> '%s'\nif [ \"$1\" = --extract-only ]; then mkdir -p \"$3/kernel\"; fi\nexit 0\n", log);
	gs_pathf(p, sizeof(p), "%s/usr/sbin/modinfo", root);
	writef(p, "#!/bin/sh\necho 550.120\n");
	chmod(p, 0755);
	gs_pathf(p, sizeof(p), "%s/usr/bin/dpkg-query", root);
	writef(p, "#!/bin/sh\nprintf '%%s\\n' 'nvidia-driver-550 install ok installed' 'nvidia-utils-550 install ok installed' "
		  "'nvidia-common deinstall ok config-files'\n");
	chmod(p, 0755);
	rc = run_package(pkg, base, root, 1, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "refused, nothing changed: NVIDIA's driver 550.120 is installed") &&
	      contains(out, "not the vGPU host driver: no libnvidia-vgpu.so* is in /usr/lib/x86_64-linux-gnu, /usr/lib64 "
			    "or /usr/lib; distribution packages own it (nvidia-driver-550, nvidia-utils-550).") &&
	      contains(out, "Remove it with `apt-get remove --purge 'nvidia-*'`, then rerun.\n") &&
	      !contains(out, "nvidia-common") && !gs_exists(base) && !gs_exists(log),
	      "Debian packages: refused before any write: %d\n%s", rc, out);
	free(out);
	unlink(p);
	gs_pathf(p, sizeof(p), "%s/usr/bin/rpm", root);
	writef(p, "#!/bin/sh\necho nvidia-driver-550.120-1.x86_64\n");
	chmod(p, 0755);
	rc = run_package(pkg, base, root, 1, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "(nvidia-driver-550.120-1.x86_64)") &&
	      contains(out, "Remove it with `dnf remove 'nvidia*'`") && !gs_exists(base), "RPM packages: %d\n%s", rc, out);
	free(out);
	unlink(p);
	/* No package, and the licence not accepted: still refused, NVIDIA's own uninstaller named. */
	rc = run_package(pkg, base, root, 0, &out);
	CHECK(rc == GS_EXIT_REFUSED && !contains(out, "distribution packages") && contains(out, "Remove it with `nvidia-uninstall`") &&
	      !gs_exists(base), "driver from a .run package: %d\n%s", rc, out);
	free(out);
	/* A vGPU userspace of another version and no package: not this refusal. */
	gs_pathf(p, sizeof(p), "%s/usr/lib64/libnvidia-vgpu.so.595.91.04", root);
	writef(p, "x\n");
	rc = run_package(pkg, base, root, 0, &out);
	CHECK(rc == GS_EXIT_FAIL && !contains(out, "is not the vGPU host driver") && contains(out, "extracted only"),
	      "vGPU userspace present: extraction goes on: %d\n%s", rc, out);
	free(out);
}

#define BOUND_LINE "NVIDIA's driver is bound to 0000:03:00.0, so prepare cannot act on it before the next boot. Setup\n" \
	"continues; the GPU is prepared after you power the machine off and on.\n"

/* setup on a host whose card runs NVIDIA's driver. */
static void test_setup_existing_driver(void)
{
	static const struct fx_fn bound[] = { { "0000:03:00.0", "2206", "030000", "nvidia" }, { NULL, NULL, NULL, NULL } };
	char root[GS_PATH_MAX], *out;
	struct gs_install_opts o;
	int rc;

	fixture("setup-bound", bound, 0, NULL, root, sizeof(root));
	setup_opts(&o);
	setup_doctor_rc = 0;
	rc = run_setup(root, NULL, &o, 1, &out);
	CHECK(rc == 0 && contains(out, BOUND_LINE) && pos_of(out, BOUND_LINE) < pos_of(out, "== doctor"),
	      "bound card: named before the first step: %d\n%s", rc, out);
	free(out);

	/* Two selected cards bound to nvidia and one that is not: one note listing the two. */
	{
		static const struct fx_topo t4 = { "pci0000:00/0000:00:03.0", "0000:04:00.0" };
		static const struct fx_topo t5 = { "pci0000:00/0000:00:04.0", "0000:05:00.0" };
		static const struct fx_fn b4[] = { { "0000:04:00.0", "2206", "030000", "nvidia" }, { NULL, NULL, NULL, NULL } };
		static const struct fx_fn c5[] = { { "0000:05:00.0", "2206", "030000", NULL }, { NULL, NULL, NULL, NULL } };
		static const char *const note = "NVIDIA's driver is bound to 0000:03:00.0, 0000:04:00.0, so prepare cannot act "
						"on them before the next boot. Setup\ncontinues; the GPUs are prepared after "
						"you power the machine off and on.\n";

		snprintf(root, sizeof(root), "%s/setup-bound-two", tmp);
		make_card(root, &topo_default, bound, 0, NULL);
		make_card(root, &t4, b4, 0, NULL);
		make_card(root, &t5, c5, 0, NULL);
		gs_addr_parse("0000:03:00.0", &o.devices[0]);
		gs_addr_parse("0000:04:00.0", &o.devices[1]);
		gs_addr_parse("0000:05:00.0", &o.devices[2]);
		o.ndevices = 3;
		rc = run_setup(root, NULL, &o, 1, &out);
		CHECK(rc == 0 && count_of(out, note) == 1 && count_of(out, "after you power the machine off and on") == 1 &&
		      !contains(out, "cannot act on it before") && pos_of(out, note) < pos_of(out, "== doctor"),
		      "two bound cards: one note listing both: %d\n%s", rc, out);
		free(out);
	}

	rc = run_cli(&out, NULL, (const char *const[]){ "--help", NULL });
	CHECK(rc == 0 && !contains(out, "  enable ") && contains(out, "  activate [--type N] [--dry-run] [--boot]\n") &&
	      !contains(out, "untested") && !contains(out, "EXPERIMENTAL") && !contains(out, "verified guest"),
	      "help: no enable, activate takes no --device, no test history:\n%s", out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "enable", NULL });
	CHECK(rc == GS_EXIT_USAGE && contains(out, "unknown command 'enable'"), "enable is gone: %d\n%s", rc, out);
	free(out);
}

/* gs_install_next for cfg, with the modprobe.d directory modprobe_dir, in a child; its output. */
static char *install_next_text(const struct gs_config *cfg, const char *modprobe_dir)
{
	int pipefd[2];
	pid_t pid;
	char *out;

	if (pipe(pipefd))
		die("pipe");
	pid = fork();
	if (pid < 0)
		die("fork");
	if (pid == 0) {
		dup2(pipefd[1], 1);
		close(pipefd[0]);
		gs_install_next("/usr/local/bin", cfg, modprobe_dir);
		fflush(stdout);
		_exit(0);
	}
	close(pipefd[1]);
	collect(pipefd[0], pid, &out);
	return out;
}

/*
 * The end of a live install says one thing: check doctor before the power cycle, power the machine off
 * and on, then setup (or, with a console release granted, the commands it lists, or setup).
 */
static void test_install_next(void)
{
	static const char *const cycle =
		"\nNext: run `/usr/local/bin/geforce-sriov doctor`, then power the machine off and on. A restart is not\n"
		"enough: the card must come up with the new module.\n";
	struct gs_config cfg;
	char mpd[GS_PATH_MAX], hold[GS_PATH_MAX], want[2 * GS_PATH_MAX], *out;

	gs_pathf(mpd, sizeof(mpd), "%s/install-next-modprobe.d", tmp);
	gs_pathf(hold, sizeof(hold), "%s/nvidia-manual-load.conf", mpd);
	cfg_of(&cfg, "bdfs=0000:03:00.0\n");
	out = install_next_text(&cfg, mpd);
	CHECK(contains(out, "geforce-sriov-prepare.service and geforce-sriov-activate.service are enabled for the next "
			    "boot, not started.\n") &&
	      contains(out, cycle) &&
	      contains(out, "enough: the card must come up with the new module.\n"
			    "After the boot, run `sudo geforce-sriov setup` again; it continues from there.\n") &&
	      count_of(out, "geforce-sriov setup") == 1 && !contains(out, "reboot") && !contains(out, "geforce-sriov status") &&
	      contains(out, "To skip preparation for one boot, add geforce_sriov.skip to the kernel command line") &&
	      !contains(out, GS_RELEASE_STEPS_LEAD), "install's closing, ordinary host: one next step\n%s", out ? out : "");
	free(out);

	cfg_of(&cfg, "bdfs=0000:03:00.0,0000:04:00.0\nboot_display_release=0000:04:00.0\n");
	out = install_next_text(&cfg, mpd);
	CHECK(contains(out, GS_RELEASE_STEPS_LEAD "\n" RELEASE_STEPS_04("  ")) && contains(out, cycle) &&
	      pos_of(out, GS_RELEASE_STEPS_LEAD) < pos_of(out, cycle) &&
	      contains(out, "enough: the card must come up with the new module.\n"
			    "After the boot, run the commands above, or `sudo geforce-sriov setup`, which asks first, then\n"
			    "runs them.\n") &&
	      !contains(out, "setup` again") && !contains(out, "reboot") && !contains(out, "geforce-sriov status") &&
	      !contains(out, "geforce_sriov.skip") && contains(out, "create\n" RELEASE_HOLD_04) && !contains(out, "already held"),
	      "install's closing, console release granted: the listed commands or setup, no skip line\n%s", out ? out : "");
	free(out);

	/* The hold already in place: its file named instead of the one to create, the commands after each boot unchanged. */
	writef(hold, "blacklist nvidia\ninstall nvidia /bin/false\n");
	out = install_next_text(&cfg, mpd);
	snprintf(want, sizeof(want), "\nnvidia is already held back at boot by %s.\n" GS_RELEASE_STEPS_LEAD "\n"
		 RELEASE_STEPS_04("  "), hold);
	CHECK(contains(out, want) && !contains(out, "create\n") && !contains(out, "nvidia-persistenced") && contains(out, cycle),
	      "install's closing, hold in place: the file named, the commands kept\n%s", out ? out : "");
	free(out);
}

int main(void)
{
	cli = getenv("GS_CLI");
	srcdir = getenv("GS_SRC");
	if (!cli || !srcdir) {
		fprintf(stderr, "set GS_CLI and GS_SRC (make test does)\n");
		return 2;
	}
	snprintf(tmp, sizeof(tmp), "/tmp/gs-lifecycle-test.XXXXXX");
	if (!mkdtemp(tmp))
		die("mkdtemp");

	test_addr();
	test_config();
	test_write_atomic();
	test_sha256();
	test_plan();
	test_cli_readonly();
	test_cli_mutation_guards();
	test_install_staging();
	test_install_guards();
	test_install_link_kept();
	test_dkms_step();
	test_doctor_scope();
	test_status_guidance();
	test_topologies();
	test_option_guidance();
	test_procfs_read();
	test_release_config();
	test_release_cli();
	test_display_inspect();
	test_release_diag();
	test_prepare_default_and_deferral();
	test_prepare_msix_message();
	test_prepare_release();
	test_vf_count();
	test_activate();
	test_activate_mixed();
	test_activate_types();
	test_run_capture();
	test_driver_build();
	test_doctor_modprobe();
	test_doctor_plugin();
	test_doctor_dropins();
	test_enable_check();
	test_prereq_and_package();
	test_driver_file();
	test_setup_questions();
	test_setup_many_cards();
	test_setup_selection();
	test_setup_release();
	test_setup_loaded_build();
	test_driver_step();
	test_driver_dry_run();
	test_driver_step_log();
	test_driver_step_restore();
	test_driver_step_modprobe();
	test_driver_step_kernels();
	test_uninstall_restore_failed();
	test_uninstall_kept();
	test_foreign_driver();
	test_setup_existing_driver();
	test_install_next();

	cleanup(tmp);
	printf("test_lifecycle: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
