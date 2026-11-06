// SPDX-License-Identifier: MIT
/*
 * CLI and lifecycle tests. Fixture sysfs trees are built in a temporary
 * directory; only read-only commands and DESTDIR staging run against them.
 * Environment: GS_CLI = built geforce-sriov binary, GS_SRC = source tree.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
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
		"sriov_manage=/usr/../bin/x\n", "twin_id=yes\n", "twin_id=on\ntwin_id=off\n", "TWIN_ID=on\n",
		"bdfs=0000:01:00.0,0000:02:00.0,0000:03:00.0,0000:04:00.0,0000:05:00.0,0000:06:00.0,0000:07:00.0,0000:08:00.0,0000:09:00.0\n",
		NULL,
	};

	CHECK(gs_config_parse("", 0, &c, err, sizeof(err)) == 0 && c.ari == GS_SW_ON && c.msix == GS_SW_ON &&
	      c.experimental_vfs == GS_SW_OFF && c.twin_id == GS_SW_OFF && c.identity == GS_SW_OFF && c.nbdfs == 0,
	      "defaults");
	/* A config without vfs (every config written before the key existed) keeps 4. */
	CHECK(c.vfs == 0 && gs_config_vfs(&c) == GS_DEFAULT_VFS, "default VF count");
	CHECK(gs_config_parse("twin_id=on\nexperimental_vfs=on\n", 31, &c, err, sizeof(err)) == 0 &&
	      c.twin_id == GS_SW_ON && c.experimental_vfs == GS_SW_ON, "twin_id=on, experimental_vfs=on: %s", err);
	CHECK(gs_config_parse("twin_id=maybe\n", 14, &c, err, sizeof(err)) != 0 && strstr(err, "twin_id must be on or off"),
	      "twin_id value named: %s", err);
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
	c.experimental_vfs = GS_SW_OFF;
	c.twin_id = GS_SW_ON;
	n = gs_config_render(&c, text, sizeof(text));
	CHECK(n > 0, "render");
	{
		struct gs_config back;

		CHECK(gs_config_parse(text, (size_t)n, &back, err, sizeof(err)) == 0 && back.nbdfs == 1 &&
		      back.identity == GS_SW_ON && back.experimental_vfs == GS_SW_OFF && back.twin_id == GS_SW_ON &&
		      back.ari == GS_SW_ON && back.vfs == 0, "roundtrip: %s", err);
		CHECK(strstr(text, "\nexperimental_vfs=off\n") && strstr(text, "\ntwin_id=on\n"),
		      "both switches are written: %s", text);
		CHECK(!strstr(text, "\nvfs="), "an unset VF count is not written: %s", text);
		c.vfs = 2;
		n = gs_config_render(&c, text, sizeof(text));
		CHECK(n > 0 && gs_config_parse(text, (size_t)n, &back, err, sizeof(err)) == 0 && back.vfs == 2,
		      "vfs roundtrip: %s", err);
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
		CHECK(plan.verdict == GS_PLAN_REFUSE && strstr(plan.reason, "missing PF"), "missing PF refused");
	}
}

static int contains(const char *hay, const char *needle)
{
	return hay && strstr(hay, needle) != NULL;
}

static void test_cli_readonly(void)
{
	char root[GS_PATH_MAX], cfg[GS_PATH_MAX], *out;
	int rc;

	rc = run_cli(&out, NULL, (const char *const[]){ "--version", NULL });
	CHECK(rc == 0 && contains(out, "geforce-sriov " GS_VERSION), "--version: %d %s", rc, out);
	free(out);

	rc = run_cli(&out, NULL, (const char *const[]){ "devices", NULL });
	CHECK(rc == 0 && contains(out, "10de:2206") && contains(out, GS_NO_SAME_DIE_MSG), "devices: %d", rc);
	{
		const char *row = strstr(out, "10de:2206");
		const char *eol = row ? strchr(row, '\n') : NULL;
		char line[256] = "";

		if (row && eol && (size_t)(eol - row) < sizeof(line))
			memcpy(line, row, (size_t)(eol - row));
		CHECK(contains(out, "HW VFS") && contains(line, " 16 "), "devices lists the hardware default: %s", line);
	}
	CHECK(!contains(out, "10de:1e07"), "devices excludes Turing");
	CHECK(contains(out, GS_EXPVFS_OPT_MSG) && contains(out, GS_TWIN_ID_OPT_MSG) &&
	      !contains(out, "experimental_vfs=on (default)"), "devices describes experimental_vfs and twin_id:\n%s", out);
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
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "boot display"), "doctor boot VGA: %d\n%s", rc, out);
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
	CHECK(rc == GS_EXIT_USAGE && contains(out, "doctor/status only"), "prepare --sysfs-root: %d", rc);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "enable", "--sysfs-root=/tmp", NULL });
	CHECK(rc == GS_EXIT_USAGE, "enable --sysfs-root: %d", rc);
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
			{ "activate", "--type", "0911", NULL },
			{ "activate", "--type=1000000", NULL, NULL },
			{ "activate", "--device", "0000:03:00.4", NULL },
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
		rc = run_cli(&out, NULL, (const char *const[]){ "enable", NULL });
		CHECK(rc == GS_EXIT_REFUSED, "enable non-root: %d", rc);
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

static void test_install_staging(void)
{
	char stage[GS_PATH_MAX], p[GS_PATH_MAX], sha[65], line[512], *out, *text;
	int rc;

	gs_pathf(stage, sizeof(stage), "%s/stage", tmp);
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--prefix", "/usr", "--source", srcdir,
							 "--device", "0000:03:00.0", "--ari=off", NULL });
	CHECK(rc == 0 && contains(out, "nothing outside it was changed"), "staged install: %d\n%s", rc, out);
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

	/* Nor is any other file at an install target. */
	gs_pathf(stage, sizeof(stage), "%s/foreign-bin", tmp);
	gs_pathf(p, sizeof(p), "%s/usr/bin/geforce-sriov", stage);
	writef(p, "not ours\n");
	rc = run_cli(&out, stage, (const char *const[]){ "install", "--prefix", "/usr", "--source", srcdir, NULL });
	CHECK(rc != 0 && contains(out, "was not installed by geforce-sriov"), "foreign binary: %d\n%s", rc, out);
	free(out);
	text = slurp(p);
	CHECK(text && strcmp(text, "not ours\n") == 0, "foreign binary untouched");
	free(text);
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
		{ "ari", "Y" }, { "msix", "Y" }, { "experimental_vfs", "N" }, { "twin_id", "N" },
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

/*
 * <pf>/resource of a default-fixture Blackwell card with 1 VF: VF BAR 0 of 256 KiB (line 7) and the
 * 64-bit VF BARs 1 of 256 MiB (line 8) and 3 of 32 MiB (line 10).
 */
static void write_blackwell_resource(const char *root)
{
	static const char *const line[13] = {
		[0] = "0x00000000f0000000 0x00000000f3ffffff 0x0000000000040200\n",
		[7] = "0x00000000f4000000 0x00000000f403ffff 0x0000000000040200\n",
		[8] = "0x0000004000000000 0x000000400fffffff 0x000000000014220c\n",
		[10] = "0x0000004010000000 0x0000004011ffffff 0x000000000014220c\n",
	};
	char p[GS_PATH_MAX], text[1024] = "";

	for (int i = 0; i <= 12; i++)
		strcat(text, line[i] ? line[i] : "0x0000000000000000 0x0000000000000000 0x0000000000000000\n");
	gs_pathf(p, sizeof(p), "%s/devices/%s/%s/resource", root, topo_default.bridge_path, topo_default.pf);
	writef(p, "%s", text);
}

static const struct fx_fn turing_gpu[] = {
	{ "0000:03:00.0", "1e07", "030000", NULL },
	{ NULL, NULL, NULL, NULL },
};

/* AD103: experimental, cross-die, 1 hardware VF (4 with experimental_vfs=on). */
static const struct fx_fn ada_cross_die_gpu[] = {
	{ "0000:03:00.0", "2702", "030000", NULL },
	{ NULL, NULL, NULL, NULL },
};

/* GB202 (RTX 5090, the twin_id card) and GB203 (RTX 5080): native SR-IOV, 1 hardware VF. */
static const struct fx_fn gb202_gpu[] = {
	{ "0000:03:00.0", "2b85", "030000", NULL },
	{ NULL, NULL, NULL, NULL },
};
static const struct fx_fn gb203_gpu[] = {
	{ "0000:03:00.0", "2c02", "030000", NULL },
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

	gs_pathf(cfg, sizeof(cfg), "%s/ux-config", tmp);

	/* An ID outside the table is refused; experimental rows are counted warnings, not plain text. */
	fixture("ux-turing", turing_gpu, 0, NULL, root, sizeof(root));
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--device", "0000:03:00.0", NULL });
	tally_of(out, &f1, &w1);
	CHECK(rc == GS_EXIT_REFUSED && f1 >= 1, "unsupported ID refused: %d\n%s", rc, out);
	free(out);

	fixture("ux-ada", ada_cross_die_gpu, 0, NULL, root, sizeof(root));
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--device", "0000:03:00.0", NULL });
	tally_of(out, &f1, &w1);
	CHECK(rc == 0 && f1 == 0 && w1 >= 2 && contains(out, "hardware default 1, geforce-sriov presents 1 (default)") &&
	      contains(out, "[INFO] 0000:03:00.0: experimental_vfs=off: the card presents its hardware count, 1 VF") &&
	      !contains(out, "open kernel module"),
	      "experimental cross-die Ada: ready, warned, 1 VF by default, no Blackwell note: %d\n%s", rc, out);
	free(out);
	writef(cfg, "experimental_vfs=on\n");
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--config", cfg, "--device",
							 "0000:03:00.0", NULL });
	tally_of(out, &f2, &w2);
	CHECK(rc == 0 && w2 == w1 + 1 && contains(out, "presents 4 (default, experimental_vfs=on)") &&
	      contains(out, "[WARN] 0000:03:00.0: experimental_vfs=on: geforce-sriov presents up to 4 VFs. " GS_EXPVFS_LINUX_MSG),
	      "experimental_vfs=on adds only the VF-override warning: %d->%d\n%s", w1, w2, out);
	/* Host prerequisites (dkms, pciutils) are live-system checks; a fixture never fails on them. */
	CHECK(!contains(out, "dkms") && !contains(out, "lspci"), "fixture doctor runs no host prerequisite check");
	free(out);

	/* Blackwell: 1 VF by default and the driver note once; twin_id warns on GB202 only. */
	fixture("ux-gb202", gb202_gpu, 0, "0/1", root, sizeof(root));
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--device", "0000:03:00.0", NULL });
	tally_of(out, &f1, &w1);
	note = out ? strstr(out, "open kernel module") : NULL;
	CHECK(rc == 0 && f1 == 0 && note && !strstr(note + 1, "open kernel module") &&
	      contains(out, "[INFO] 0000:03:00.0: the closed 580.159.01 driver does not support this GPU; ") &&
	      contains(out, "hardware default 1, geforce-sriov presents 1 (default)") && !contains(out, "twin_id=on"),
	      "GB202 default: 1 VF, one driver note, no twin_id line: %d\n%s", rc, out);
	free(out);
	writef(cfg, "twin_id=on\n");
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--config", cfg, "--device",
							 "0000:03:00.0", NULL });
	tally_of(out, &f2, &w2);
	CHECK(rc == 0 && w2 == w1 + 1 && contains(out, "[WARN] 0000:03:00.0: twin_id=on: " GS_TWIN_ID_MSG) &&
	      contains(out, "experimental_vfs=off twin_id=on vfs=4 (default)"),
	      "GB202 twin_id=on: one more warning, config line names it: %d->%d\n%s", w1, w2, out);
	free(out);
	fixture("ux-gb203", gb203_gpu, 0, "0/1", root, sizeof(root));
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--device", "0000:03:00.0", NULL });
	tally_of(out, &f1, &w1);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--config", cfg, "--device",
							 "0000:03:00.0", NULL });
	tally_of(out, &f2, &w2);
	CHECK(rc == GS_EXIT_REFUSED && f2 == f1 + 1 && w2 == w1 &&
	      contains(out, "[INFO] 0000:03:00.0: twin_id=on: " GS_TWIN_ID_ONLY_MSG) &&
	      contains(out, "[FAIL] twin_id=on, but no selected GPU is a GB202, so the module refuses to load; prepare refuses it") &&
	      contains(out, "open kernel module"), "GB203 alone with twin_id=on: refused: %d->%d\n%s", f1, f2, out);
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
	CHECK(rc == GS_EXIT_FAIL && contains(out, "geforce-sriov doctor") && contains(out, "reboot"),
	      "selected, unprepared: next step given: %d\n%s", rc, out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "status", "--sysfs-root", root, "--device", "0000:03:00.0", NULL });
	CHECK(rc == GS_EXIT_FAIL && contains(out, "install --device 0000:03:00.0"),
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
	/* A presented card without BAR0 is unusable: not ready. */
	write_resource(root, 0, 1);
	rc = run_cli(&out, NULL, (const char *const[]){ "status", "--sysfs-root", root, "--config", cfg, NULL });
	CHECK(rc == GS_EXIT_FAIL, "no BAR0: not ready: %d\n%s", rc, out);
	free(out);
	/* Blackwell's larger VF BARs (256 KiB, 256 MiB, 32 MiB per VF) are an assigned layout, not an error. */
	fixture("ux-gb202-bars", gb202_gpu, 0, "0/1", root, sizeof(root));
	write_blackwell_resource(root);
	fake_module(root, "0000:03:00.0");
	gs_card_inspect(root, &pf, &card);
	CHECK(card.bar0_assigned == 1 && card.vf_bars_assigned == 1, "Blackwell resource read: %d %d", card.bar0_assigned,
	      card.vf_bars_assigned);
	rc = run_cli(&out, NULL, (const char *const[]){ "status", "--sysfs-root", root, "--config", cfg, NULL });
	CHECK(rc == 0 && contains(out, "experimental_vfs=N twin_id=N vfs=(none)") &&
	      contains(out, "hardware default 1, geforce-sriov presents 1 (default)") &&
	      contains(out, "sriov_totalvfs=1 sriov_numvfs=0") && !contains(out, "BAR"),
	      "Blackwell VF BARs: ready, module twin_id shown: %d\n%s", rc, out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--config", cfg, NULL });
	tally_of(out, &f1, &w1);
	CHECK(rc == 0 && f1 == 0 && !contains(out, "BAR0") && !contains(out, "VF BAR") &&
	      contains(out, "experimental_vfs=N twin_id=N"), "Blackwell VF BARs: doctor accepts them: %d\n%s", rc, out);
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
	rc = run_cli(&out, NULL, (const char *const[]){ "enable", "--num-vfs", "2", NULL });
	CHECK(rc == GS_EXIT_USAGE && !contains(out, "unknown option"), "--num-vfs: %d\n%s", rc, out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "install", "--num-vfs=2", NULL });
	CHECK(rc == GS_EXIT_USAGE && contains(out, "--vfs N"), "install --num-vfs: %d\n%s", rc, out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "prepare", "--bogus", NULL });
	CHECK(rc == GS_EXIT_USAGE && contains(out, "unknown option"), "--bogus: %d\n%s", rc, out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "install", "--twin_id=on", NULL });
	CHECK(rc == GS_EXIT_USAGE && contains(out, "--twin-id=on|off"), "--twin_id: %d\n%s", rc, out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "prepare", "--clear-guard", "--twin-id=off", NULL });
	CHECK(rc == GS_EXIT_USAGE && contains(out, "--clear-guard takes no other option"),
	      "--clear-guard --twin-id: %d\n%s", rc, out);
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

static void test_sha256(void)
{
	char hex[65];

	gs_sha256_buf("abc", 3, hex);
	CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0, "sha256 abc");
	gs_sha256_buf("", 0, hex);
	CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0, "sha256 empty");
	gs_sha256_buf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, hex);
	CHECK(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0, "sha256 two blocks");
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
} sim;

/*
 * NVIDIA's driver probing a PF with VFs: each VF's vfio device registers again, which sets
 * its power/control back to auto, and the vGPU manager publishes the VF's types.
 */
static void sim_register_vfs(const char *pf)
{
	char p[GS_PATH_MAX], real[PATH_MAX];

	for (unsigned int i = 0;; i++) {
		gs_pathf(p, sizeof(p), "%s/bus/pci/devices/%s/virtfn%u", sim.root, pf, i);
		if (!realpath(p, real))
			return;
		gs_pathf(p, sizeof(p), "%s/power/control", real);
		writef(p, "auto\n");
		gs_pathf(p, sizeof(p), "%s/power/runtime_status", real);
		writef(p, "suspended\n");
		gs_pathf(p, sizeof(p), "%s/nvidia/creatable_vgpu_types", real);
		writef(p, "ID    : vGPU Name\n908   : NVIDIA L4-1Q\n911   : NVIDIA L4-2Q\n");
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
		if (strcmp(r, "bus/pci/drivers/nvidia/bind") == 0)
			sim_register_vfs(value);
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
	struct gs_host h = { sim.root, sim.proc, sim.state, sim.run, sim_write, sim_run };
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
	CHECK(rc == 0 && contains(out, "--release-console"), "staged grant: %d\n%s", rc, out);
	free(out);
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

	/* The measured host: simple-framebuffer.0 under the PF, simpledrm bound, fb0/card0, fbcon. */
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
	      contains(out, "prepare --release-console=0000:04:00.0"), "doctor: granted: %d\n%s", rc, out);
	free(out);
	rc = run_cli(&out, NULL, (const char *const[]){ "status", "--sysfs-root", root, "--config", cfg, NULL });
	CHECK(rc == GS_EXIT_FAIL && contains(out, "deferred") &&
	      contains(out, "prepare --release-console=0000:04:00.0"), "status: deferred and the next command: %d\n%s",
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
	char root[GS_PATH_MAX], p[GS_PATH_MAX], *out, *log, *rec;
	struct gs_config cfg;
	int rc;

	/* Without a grant the boot path is unchanged: 03 alone is prepared at boot. */
	two_cards("prep-default", 0, -1, root, sizeof(root));
	cfg_of(&cfg, "bdfs=0000:03:00.0\n");
	rc = run_prepare(&cfg, 1, NULL, &out);
	log = sim_text("sim.log");
	rec = sim_text("run/" GS_PREPARED_NAME "/0000:03:00.0");
	CHECK(rc == 0 && contains(log, "R modprobe geforce_sriov ari=1 msix=1 experimental_vfs=0 twin_id=0 bdfs=0000:03:00.0") &&
	      contains(log, "W bus/pci/devices/0000:03:00.1/driver/unbind") &&
	      contains(log, "W bus/pci/devices/0000:03:00.0/remove 1") && contains(log, "/rescan 1") &&
	      !contains(log, "platform"), "default boot prepare: %d\n%s\n%s", rc, out, log);
	CHECK(contains(rec, "totalvfs=4") && !contains(rec, "console_release"), "default record: %s", rec ? rec : "");
	CHECK(!sim_exists("state/" GS_ATTEMPT_NAME) && !sim_exists("run/" GS_DEFERRED_NAME), "no guard, no deferral");
	free(out);
	free(log);
	free(rec);

	/* experimental_vfs and twin_id reach the module when a selected GPU is a GB202. */
	two_cards("prep-twin", 0, -1, root, sizeof(root));
	gs_pathf(p, sizeof(p), "%s/devices/%s/%s/device", root, topo_default.bridge_path, topo_default.pf);
	writef(p, "0x2b85\n");
	cfg_of(&cfg, "bdfs=0000:03:00.0\nexperimental_vfs=on\ntwin_id=on\n");
	rc = run_prepare(&cfg, 1, NULL, &out);
	log = sim_text("sim.log");
	CHECK(rc == 0 && contains(log, "R modprobe geforce_sriov ari=1 msix=1 experimental_vfs=1 twin_id=1 bdfs=0000:03:00.0"),
	      "twin_id=on prepare: %d\n%s\n%s", rc, out, log);
	free(out);
	free(log);

	/* twin_id=on without a GB202: the module would refuse to load, so prepare refuses first. */
	two_cards("prep-twin-none", 0, -1, root, sizeof(root));
	cfg_of(&cfg, "bdfs=0000:03:00.0\ntwin_id=on\n");
	rc = run_prepare(&cfg, 1, NULL, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "no selected GPU is a GB202") && contains(out, GS_TWIN_ID_ONLY_MSG),
	      "twin_id=on without a GB202: refused: %d\n%s", rc, out);
	check_untouched("twin_id=on without a GB202");
	free(out);

	/* A module already loaded with another twin_id is refused before any removal; equal values are reused. */
	two_cards("prep-loaded", 0, -1, root, sizeof(root));
	gs_pathf(p, sizeof(p), "%s/devices/%s/%s/device", root, topo_default.bridge_path, topo_default.pf);
	writef(p, "0x2b85\n");
	fake_module(root, "0000:03:00.0");
	cfg_of(&cfg, "bdfs=0000:03:00.0\ntwin_id=on\n");
	rc = run_prepare(&cfg, 1, NULL, &out);
	log = sim_text("sim.log");
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "loaded with ari=Y msix=Y experimental_vfs=N twin_id=N vfs=4") &&
	      contains(out, "requested ari=Y msix=Y experimental_vfs=N twin_id=Y vfs=4") && !contains(log, "R modprobe") &&
	      !contains(log, "/remove") && !sim_exists("state/" GS_ATTEMPT_NAME),
	      "loaded twin_id=N, requested on: refused: %d\n%s\n%s", rc, out, log);
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
	CHECK(rc == 0 && contains(out, "deferred") && contains(out, "prepare --release-console=0000:04:00.0"),
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
	load = pos_of(log, "R modprobe geforce_sriov ari=1 msix=1 experimental_vfs=0 twin_id=0 bdfs=0000:03:00.0,0000:04:00.0");
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
	CHECK(rc == 0 && contains(log, "R modprobe geforce_sriov ari=1 msix=1 experimental_vfs=0 twin_id=0 vfs=2 bdfs=0000:03:00.0") &&
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

	/* Above what the card offers (Ada reports 1, kept by default): refused before the first change. */
	two_cards("vf-ada", 0, -1, root, sizeof(root));
	gs_pathf(p, sizeof(p), "%s/devices/%s/%s/device", root, topo_default.bridge_path, topo_default.pf);
	writef(p, "0x2702\n");
	cfg_of(&cfg, "bdfs=0000:03:00.0\nvfs=2\n");
	rc = run_prepare(&cfg, 1, NULL, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "vfs=2 is above the hardware default"),
	      "vfs above the hardware refused: %d\n%s", rc, out);
	check_untouched("vfs above the hardware");
	free(out);
	cfg_of(&cfg, "bdfs=0000:03:00.0\nexperimental_vfs=on\nvfs=8\n");
	rc = run_prepare(&cfg, 1, NULL, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "experimental_vfs=on presents"),
	      "vfs above the experimental 4 refused: %d\n%s", rc, out);
	check_untouched("vfs above the experimental count");
	free(out);

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
	writef(cfgp, "bdfs=0000:03:00.0\nexperimental_vfs=off\nvfs=2\n");
	rc = run_cli(&out, NULL, (const char *const[]){ "doctor", "--sysfs-root", root, "--config", cfgp, NULL });
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "hardware default 1"), "doctor vfs above the hardware: %d\n%s", rc,
	      out);
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

/* ---- activate and check (Ada) ----------------------------------------- */

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
 * The measured Ada host after `enable`: AD104 PF 0000:03:00.0 and its one VF 0000:03:00.4 on
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
	struct gs_host h = { sim.root, sim.proc, sim.state, sim.run, sim_write, sim_run };

	return gs_activate(&h, cfg, o);
}

static int child_check(const struct gs_config *cfg, const struct gs_activate_opts *o)
{
	struct gs_host h = { sim.root, sim.proc, sim.state, sim.run, sim_write, sim_run };

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
		"W bus/pci/devices/0000:03:00.4/nvidia/current_vgpu_type 911\n",
		"W bus/pci/devices/0000:03:00.4/power/control on\n",
	};
	static const struct {
		const char *name, *rel, *text, *msg;
	} refusals[] = {
		{ "ada-gsp", "proc/driver/nvidia/params", "EnableGpuFirmware: 18\n", "NVreg_EnableGpuFirmware=0" },
		{ "ada-ver", "proc/driver/nvidia/version", "NVRM version: NVIDIA UNIX x86_64 Kernel Module  595.91.04  Release Build\n",
		  "measured only with 580.159.01" },
		{ "ada-vgpu", ADA_DEV "0000:03:00.4/nvidia/current_vgpu_type", "911\n", "would end it" },
		{ "ada-novf", ADA_DEV "0000:03:00.0/sriov_numvfs", "0\n", "run `geforce-sriov enable` first" },
		{ "ada-ampere", ADA_DEV "0000:03:00.0/device", "0x2206\n", "no selected GPU is an Ada GPU" },
	};
	char root[GS_PATH_MAX], p[GS_PATH_MAX], *out, *log, *t;
	struct gs_activate_opts o;
	struct gs_config cfg;
	unsigned int v = 0;
	long prev = -1;
	int rc;

	CHECK(gs_parse_vgpu_type("911", &v) == 0 && v == 911, "type 911");
	CHECK(gs_parse_vgpu_type("0", &v) != 0 && gs_parse_vgpu_type("0911", &v) != 0 &&
	      gs_parse_vgpu_type("1000000", &v) != 0 && gs_parse_vgpu_type("9x", &v) != 0, "bad types refused");
	cfg_of(&cfg, "bdfs=0000:03:00.0\n");

	/* The measured order; the type and power/control come after the last re-probe. */
	ada_host("ada-ok", root, sizeof(root));
	memset(&o, 0, sizeof(o));
	o.type = 911;
	rc = run_ada(child_check, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_FAIL && contains(out, "NOT READY 0000:03:00.4"), "check before activate: %d\n%s", rc, out);
	free(out);
	rc = run_ada(child_activate, &cfg, &o, &out);
	log = sim_text("sim.log");
	CHECK(rc == GS_EXIT_OK, "activate: %d\n%s", rc, out);
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
	CHECK(t && strncmp(t, "911", 3) == 0 && (t[3] == '\0' || t[3] == '\n'), "VF type 911: %s", t ? t : "(none)");
	free(t);
	free(log);
	free(out);
	rc = run_ada(child_check, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_OK && contains(out, "OK 0000:03:00.4"), "check after activate: %d\n%s", rc, out);
	free(out);
	o.type = 912;
	rc = run_ada(child_check, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_FAIL, "check with another type: %d\n%s", rc, out);
	free(out);
	/* A later re-probe (or manager restart) sets power/control back to auto: check fails closed. */
	sim_register_vfs("0000:03:00.0");
	o.type = 0;
	rc = run_ada(child_check, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_FAIL && contains(out, "power/control=auto"), "check after a re-registration: %d\n%s", rc, out);
	free(out);

	/* Without --type the type is the operator's: never written. */
	ada_host("ada-notype", root, sizeof(root));
	memset(&o, 0, sizeof(o));
	rc = run_ada(child_activate, &cfg, &o, &out);
	log = sim_text("sim.log");
	CHECK(rc == GS_EXIT_OK && !contains(log, "current_vgpu_type") && contains(log, "0000:03:00.4/power/control on"),
	      "activate without --type: %d\n%s\n%s", rc, out, log);
	free(log);
	free(out);
	rc = run_ada(child_check, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_OK && contains(out, "no vGPU type yet"), "check without --type: %d\n%s", rc, out);
	free(out);

	/* --dry-run: every check, then the steps, and no change. */
	ada_host("ada-dry", root, sizeof(root));
	o.dry_run = 1;
	rc = run_ada(child_activate, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_OK && contains(out, "dry run"), "dry run: %d\n%s", rc, out);
	check_ada_untouched("dry run");
	free(out);
	o.dry_run = 0;

	for (size_t k = 0; k < sizeof(refusals) / sizeof(refusals[0]); k++) {
		ada_host(refusals[k].name, root, sizeof(root));
		sim_writef(refusals[k].rel, refusals[k].text);
		rc = run_ada(child_activate, &cfg, &o, &out);
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

	/* A selected Ampere GPU with VFs: activate goes ahead, names it and writes nothing for it. */
	{
		static const struct fx_fn ampere_04[] = {
			{ "0000:04:00.0", "2206", "030000", "nvidia" },
			{ "0000:04:00.4", "2206", "030000", "nvidia" },
			{ NULL, NULL, NULL, NULL },
		};
		struct gs_config mixed;

		ada_host("ada-mixed", root, sizeof(root));
		make_card(root, &topo_display, ampere_04, 0, "1/1");
		gs_pathf(p, sizeof(p), "%s/devices/%s/0000:04:00.4/physfn", root, topo_display.bridge_path);
		link_to("../0000:04:00.0", p);
		cfg_of(&mixed, "bdfs=0000:03:00.0,0000:04:00.0\n");
		rc = run_ada(child_activate, &mixed, &o, &out);
		log = sim_text("sim.log");
		CHECK(rc == GS_EXIT_OK && contains(out, "note: VF 0000:04:00.4 of 0000:04:00.0") && !contains(log, "0000:04:00"),
		      "mixed Ampere + Ada: %d\n%s\n%s", rc, out, log);
		free(log);
		free(out);
	}

	/* Two selected Ada GPUs: --device naming one is refused, the daemon restart covers both. */
	{
		static const struct fx_fn ada_04[] = {
			{ "0000:04:00.0", "2783", "030000", "nvidia" },
			{ NULL, NULL, NULL, NULL },
		};
		struct gs_config two;

		ada_host("ada-two", root, sizeof(root));
		make_card(root, &topo_display, ada_04, 0, "1/1");
		cfg_of(&two, "bdfs=0000:03:00.0,0000:04:00.0\n");
		o.ndevices = 1;
		gs_addr_parse("0000:03:00.0", &o.devices[0]);
		rc = run_ada(child_activate, &two, &o, &out);
		CHECK(rc == GS_EXIT_REFUSED && contains(out, "name every\nselected Ada GPU"), "--device subset: %d\n%s", rc, out);
		check_ada_untouched("--device subset");
		free(out);
		o.ndevices = 0;
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
	CHECK(contains(out, "refuses (exit 5) until a cold power cycle and `geforce-sriov prepare --clear-guard`") &&
	      !contains(out, "until an activate") && sim_exists("run/" GS_ACTIVATING_NAME),
	      "PF lost: the stop message names the guard's recovery and the record stays: %s", out);
	free(log);
	free(out);

	/* nvidia_vgpu_vfio is not loaded: refused. */
	ada_host("ada-novfio", root, sizeof(root));
	gs_pathf(p, sizeof(p), "%s/module/nvidia_vgpu_vfio", root);
	cleanup(p);
	rc = run_ada(child_activate, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "nvidia_vgpu_vfio is not loaded"), "no vfio module: %d\n%s", rc, out);
	check_ada_untouched("no vfio module");
	free(out);

	/* The plugin gate fails closed: an unknown file and a missing one are both refused. */
	ada_host("ada-plugin", root, sizeof(root));
	gs_pathf(p, sizeof(p), "%s/libnvidia-vgpu.so", root);
	writef(p, "not NVIDIA's plugin\n");
	o.plugin = p;
	rc = run_ada(child_activate, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "neither NVIDIA's 580.159.01 plugin nor its H5 copy"), "unknown plugin: %d\n%s",
	      rc, out);
	check_ada_untouched("unknown plugin");
	free(out);
	gs_pathf(p, sizeof(p), "%s/no-such-libnvidia-vgpu.so", root);
	rc = run_ada(child_activate, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "cannot read"), "missing plugin: %d\n%s", rc, out);
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
			gs_pathf(p, sizeof(p), "%s/%s", root, marker[k]);
			rc = check_marker_mid_read(&cfg, &o, marker[k], &out);
			CHECK(rc == GS_EXIT_FAIL && contains(out, "OK 0000:03:00.4") && contains(out, "appeared during the check") &&
			      !contains(out, "exists:") && contains(out, p) && sim_exists(marker[k]), "%s appears mid-read: %d\n%s",
			      marker[k], rc, out);
			free(out);
		}
	}

	/* check names an Ampere card: refused. */
	ada_host("ada-check-ampere", root, sizeof(root));
	sim_writef(ADA_DEV "0000:03:00.0/device", "0x2206\n");
	o.ndevices = 1;
	gs_addr_parse("0000:03:00.0", &o.devices[0]);
	rc = run_ada(child_check, &cfg, &o, &out);
	CHECK(rc == GS_EXIT_REFUSED && contains(out, "not an Ada GPU"), "check on Ampere: %d\n%s", rc, out);
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
	test_sha256();
	test_plan();
	test_cli_readonly();
	test_cli_mutation_guards();
	test_install_staging();
	test_install_guards();
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
	test_prepare_release();
	test_vf_count();
	test_activate();
	test_enable_check();

	cleanup(tmp);
	printf("test_lifecycle: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
