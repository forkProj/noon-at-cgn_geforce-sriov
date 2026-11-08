// SPDX-License-Identifier: MIT
/*
 * geforce-sriov identity hook: an LD_PRELOAD ioctl wrapper for nvidia-vgpud.
 *
 * After the driver has answered, a successful NV2080_CTRL_CMD_BUS_GET_PCI_INFO reply is
 * presented under the vGPU target ID of the shared device table (include/gs_devices.h) when
 * the driver attributes it to a selected card, and, only when configured, a successful
 * NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE reply the driver attributes to a selected card is
 * presented as HOST. The attribution asks the driver, per reply, which GPU (PCI domain, bus,
 * slot) the daemon's handle belongs to, with read-only RM controls on the daemon's own fd and
 * client; no handle or answer is cached. Nothing else is changed. Fail closed: unless the
 * process is nvidia-vgpud, the config says identity=on, the running driver is an exact listed
 * build and a selected card resolves to a table entry, every ioctl passes through unchanged.
 * See hook/README.md.
 *
 * Adapted from earlier unpublished code by the same copyright holder, which was itself trimmed
 * from a fork of vgpu_unlock-rs 2.5.0 (https://github.com/mbilker/vgpu_unlock-rs, MIT,
 * Copyright (c) 2021 Jonathan Johansson, Copyright (c) 2021 Matt Bilker), which builds on
 * DualCoder's vgpu_unlock (https://github.com/DualCoder/vgpu_unlock). Licence and notices:
 * hook/LICENSE.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h> /* checks the interposer against libc's ioctl prototype */
#include <sys/syscall.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include "gs_devices.h"
#include "gs_hook.h"

#define GS_HOOK_EXPORT __attribute__((visibility("default")))
#define MAX_KEYS 64
/* Any non-zero value: a query whose status the driver did not write is never NV_OK. */
#define QUERY_STATUS_UNSET 0xffffffffu
/* id_holders() reasons: a whole sysfs entry name (up to NAME_MAX) plus the text around it. */
#define REASON_LEN (NAME_MAX + 64)
/*
 * Test build only (read through test_env_or()): the symbol looked up as the next ioctl. A name
 * no library defines stands for a failed lookup (hook/tests/run_hook_tests.c sets it).
 */
#define GS_HOOK_TEST_ENV_NEXT_IOCTL "GEFORCE_SRIOV_TEST_NEXT_IOCTL"

typedef int (*ioctl_fn)(int, unsigned long, ...);

/* One mapped card: a selected PF (function 0) and its presented identity. */
struct card {
	uint32_t domain;
	uint8_t bus;
	uint8_t slot;
	unsigned short device;
	unsigned short target;
	/*
	 * At startup every NVIDIA physical function with this device ID was a mapped card, so a
	 * reply whose GPU the driver cannot name may be rewritten by its device ID, once a fresh
	 * scan of the PCI devices still shows the same (id_holders()).
	 */
	bool id_unique;
	char name[GS_BDF_STR_SIZE];
	atomic_bool logged_rewrite;
	atomic_bool logged_forced;
};

struct config {
	bool identity;
	bool force_host;
	size_t n_bdfs;
	struct gs_bdf bdfs[GS_MAX_PFS];
};

/* Written once under hook_once, read-only afterwards (apart from the log-once flags). */
static struct {
	ioctl_fn next;
	bool active;
	bool force_host;
	size_t n_cards;
	struct card cards[GS_MAX_PFS];
	char devices[PATH_MAX]; /* <sysfs root>/bus/pci/devices */
} hook;

static pthread_once_t hook_once = PTHREAD_ONCE_INIT;
static atomic_bool logged_pci_size, logged_virt_size, logged_finn, logged_unmapped,
	logged_pci_other, logged_pci_mismatch, logged_pci_fallback, logged_pci_unattributed,
	logged_pci_fallback_refused, logged_virt_other, logged_virt_unattributed;

__attribute__((format(printf, 2, 3))) static void hook_log(int prio, const char *fmt, ...)
{
	char line[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
#ifdef GS_HOOK_TEST_ENV
	(void)prio; /* only the syslog branch uses the priority */
	fprintf(stderr, "geforce-sriov-hook: %s\n", line); /* tests: keep the host's syslog clean */
#else
	syslog(LOG_DAEMON | prio, "geforce-sriov-hook: %s", line);
#endif
}

static const char *test_env_or(const char *env, const char *dflt)
{
#ifdef GS_HOOK_TEST_ENV
	const char *v = secure_getenv(env);

	if (v && *v)
		return v;
#else
	(void)env;
#endif
	return dflt;
}

/*
 * Read at most cap bytes of an open file into buf (NUL-terminated, buf has cap + 1 bytes).
 * Returns the length, or -1 with errno set (EFBIG when the file holds more than cap bytes).
 */
static ssize_t read_fd(int fd, char *buf, size_t cap)
{
	size_t len = 0;

	for (;;) {
		char extra;
		ssize_t n = len < cap ? read(fd, buf + len, cap - len) : read(fd, &extra, 1);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			break;
		if (len == cap) {
			errno = EFBIG;
			return -1;
		}
		len += (size_t)n;
	}
	buf[len] = '\0';
	return (ssize_t)len;
}

static ssize_t read_path(const char *path, char *buf, size_t cap)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
	ssize_t n;
	int err;

	if (fd < 0)
		return -1;
	n = read_fd(fd, buf, cap);
	err = errno;
	close(fd);
	errno = err;
	return n;
}

static bool parse_on_off(const char *v, size_t n, bool *out)
{
	if (n == 2 && memcmp(v, "on", 2) == 0) {
		*out = true;
		return true;
	}
	if (n == 3 && memcmp(v, "off", 3) == 0) {
		*out = false;
		return true;
	}
	return false;
}

/* The shared PF list parser (gs_devices.h): canonical dddd:bb:ss.0, distinct, 1..GS_MAX_PFS. */
static bool parse_bdfs(const char *v, size_t n, struct config *cfg)
{
	char text[GS_BDF_LIST_STR_SIZE];
	int count;

	if (n >= sizeof(text))
		return false;
	memcpy(text, v, n);
	text[n] = '\0';
	count = gs_parse_pf_bdf_list(text, cfg->bdfs, GS_MAX_PFS);
	if (count <= 0)
		return false;
	cfg->n_bdfs = (size_t)count;
	return true;
}

/*
 * The shared key=value format (agreed with the CLI): '\n'-separated lines, empty lines and
 * lines whose first byte is '#' ignored, otherwise key=value with key [a-z0-9_]+, a non-empty
 * value, no whitespace or '\r' anywhere, no duplicate key. Keys the hook does not use are
 * ignored. Any violation makes the whole file invalid.
 */
static bool parse_config(const char *text, size_t len, struct config *cfg, char *why,
			 size_t why_len)
{
	struct { const char *p; size_t n; } keys[MAX_KEYS];
	size_t n_keys = 0, line_no = 0, pos = 0;

	memset(cfg, 0, sizeof(*cfg));
	while (pos < len) {
		const char *line = text + pos;
		const char *nl = memchr(line, '\n', len - pos);
		size_t n = nl ? (size_t)(nl - line) : len - pos;
		const char *eq;
		size_t key_n, val_n;
		const char *val;

		pos += n + 1;
		line_no++;
		if (n == 0 || line[0] == '#')
			continue;
		for (size_t i = 0; i < n; i++) {
			unsigned char c = (unsigned char)line[i];

			if (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f' || c == '\0') {
				snprintf(why, why_len, "line %zu: whitespace or NUL", line_no);
				return false;
			}
		}
		eq = memchr(line, '=', n);
		if (!eq || eq == line || eq == line + n - 1) {
			snprintf(why, why_len, "line %zu: not key=value", line_no);
			return false;
		}
		key_n = (size_t)(eq - line);
		val = eq + 1;
		val_n = n - key_n - 1;
		for (size_t i = 0; i < key_n; i++) {
			char c = line[i];

			if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
				snprintf(why, why_len, "line %zu: key is not [a-z0-9_]+", line_no);
				return false;
			}
		}
		for (size_t i = 0; i < n_keys; i++) {
			if (keys[i].n == key_n && memcmp(keys[i].p, line, key_n) == 0) {
				snprintf(why, why_len, "line %zu: duplicate key", line_no);
				return false;
			}
		}
		if (n_keys == MAX_KEYS) {
			snprintf(why, why_len, "more than %d keys", MAX_KEYS);
			return false;
		}
		keys[n_keys].p = line;
		keys[n_keys++].n = key_n;

#define KEY_IS(lit) (key_n == sizeof(lit) - 1 && memcmp(line, lit, key_n) == 0)
		if (KEY_IS("identity")) {
			if (!parse_on_off(val, val_n, &cfg->identity)) {
				snprintf(why, why_len, "line %zu: identity must be on or off", line_no);
				return false;
			}
		} else if (KEY_IS("identity_force_host")) {
			if (!parse_on_off(val, val_n, &cfg->force_host)) {
				snprintf(why, why_len,
					 "line %zu: identity_force_host must be on or off", line_no);
				return false;
			}
		} else if (KEY_IS("bdfs")) {
			if (!parse_bdfs(val, val_n, cfg)) {
				snprintf(why, why_len,
					 "line %zu: bdfs must be 1-%u distinct dddd:bb:ss.0, comma-separated",
					 line_no, GS_MAX_PFS);
				return false;
			}
		}
#undef KEY_IS
	}
	return true;
}

enum config_state { CONFIG_OK, CONFIG_ABSENT, CONFIG_BAD };

static enum config_state load_config(const char *path, struct config *cfg, char *why,
				     size_t why_len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
	struct stat st;
	char *text;
	ssize_t n;
	bool ok;

	if (fd < 0) {
		if (errno == ENOENT)
			return CONFIG_ABSENT;
		snprintf(why, why_len, "cannot open: %s", strerror(errno));
		return CONFIG_BAD;
	}
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		snprintf(why, why_len, "not a regular file");
		close(fd);
		return CONFIG_BAD;
	}
	/* Enables a rewrite in a root daemon: only its owner (root in production) may write it. */
	if ((st.st_mode & (S_IWGRP | S_IWOTH)) || (st.st_uid != 0 && st.st_uid != geteuid())) {
		snprintf(why, why_len, "writable by group/others or not owned by root");
		close(fd);
		return CONFIG_BAD;
	}
	text = malloc(GS_HOOK_CONFIG_MAX_BYTES + 1);
	if (!text) {
		snprintf(why, why_len, "out of memory");
		close(fd);
		return CONFIG_BAD;
	}
	n = read_fd(fd, text, GS_HOOK_CONFIG_MAX_BYTES);
	close(fd);
	if (n < 0) {
		snprintf(why, why_len, "%s", errno == EFBIG ? "larger than 65536 bytes"
							      : strerror(errno));
		free(text);
		return CONFIG_BAD;
	}
	ok = parse_config(text, (size_t)n, cfg, why, why_len);
	free(text);
	return ok ? CONFIG_OK : CONFIG_BAD;
}

/* The version token of the "NVRM version:" first line, e.g. "580.159.01". */
static bool driver_version(char *out, size_t out_len, char *why, size_t why_len)
{
	static const char prefix[] = "NVRM version:";
	const char *path = test_env_or(GS_HOOK_TEST_ENV_DRIVER_VERSION, GS_HOOK_DRIVER_VERSION_PATH);
	char text[4096];
	char *p, *end;

	if (read_path(path, text, sizeof(text) - 1) < 0) {
		snprintf(why, why_len, "cannot read %s: %s", path, strerror(errno));
		return false;
	}
	end = strchr(text, '\n');
	if (end)
		*end = '\0';
	if (strncmp(text, prefix, sizeof(prefix) - 1) != 0) {
		snprintf(why, why_len, "%s does not start with \"%s\"", path, prefix);
		return false;
	}
	for (p = text + sizeof(prefix) - 1; *p; p = end) {
		size_t n, dots = 0;
		bool shape = true;

		while (*p == ' ' || *p == '\t')
			p++;
		end = p + strcspn(p, " \t");
		n = (size_t)(end - p);
		if (n == 0)
			break;
		for (size_t i = 0; i < n && shape; i++) {
			if (p[i] == '.')
				shape = i > 0 && p[i - 1] != '.' && ++dots <= 2;
			else
				shape = p[i] >= '0' && p[i] <= '9';
		}
		if (shape && dots >= 1 && p[n - 1] != '.') {
			if (n >= out_len)
				break;
			memcpy(out, p, n);
			out[n] = '\0';
			return true;
		}
	}
	snprintf(why, why_len, "no version number in %s", path);
	return false;
}

/* A sysfs ID file such as "0x10de\n". */
static bool read_id(const char *dir, const char *name, unsigned int *out)
{
	char path[PATH_MAX], text[16];
	const char *p;
	ssize_t n;

	if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, name) >= sizeof(path))
		return false;
	n = read_path(path, text, sizeof(text) - 1);
	if (n == 7 && text[6] == '\n')
		text[--n] = '\0';
	if (n != 6 || text[0] != '0' || text[1] != 'x')
		return false;
	p = text + 2;
	return gs_hex_n(&p, 4, out) == 1;
}

/* The PCI base class from a sysfs class file such as "0x030000\n" (0x03: display). */
static bool read_base_class(const char *dir, unsigned int *out)
{
	char path[PATH_MAX], text[16];
	const char *p;
	unsigned int class;
	ssize_t n;

	if ((size_t)snprintf(path, sizeof(path), "%s/class", dir) >= sizeof(path))
		return false;
	n = read_path(path, text, sizeof(text) - 1);
	if (n == 9 && text[8] == '\n')
		text[--n] = '\0';
	if (n != 8 || text[0] != '0' || text[1] != 'x')
		return false;
	p = text + 2;
	if (!gs_hex_n(&p, 6, &class))
		return false;
	*out = class >> 16;
	return true;
}

/* A sysfs PCI function, as far as the card rules care. */
enum fn_kind { FN_ABSENT, FN_UNREADABLE, FN_OTHER_VENDOR, FN_NVIDIA_VF, FN_NVIDIA_PF };

struct fn_info {
	unsigned int vendor;
	unsigned int device;
	unsigned int base; /* read only when asked for */
};

static enum fn_kind pci_fn(const char *devices, const char *name, bool want_class,
			   struct fn_info *f)
{
	char dir[PATH_MAX], physfn[PATH_MAX];
	struct stat st;

	if ((size_t)snprintf(dir, sizeof(dir), "%s/%s", devices, name) >= sizeof(dir) ||
	    (size_t)snprintf(physfn, sizeof(physfn), "%s/physfn", dir) >= sizeof(physfn))
		return FN_UNREADABLE;
	if (lstat(dir, &st) != 0)
		return errno == ENOENT ? FN_ABSENT : FN_UNREADABLE;
	if (!read_id(dir, "vendor", &f->vendor) || !read_id(dir, "device", &f->device))
		return FN_UNREADABLE;
	if (f->vendor != GS_VENDOR_NVIDIA)
		return FN_OTHER_VENDOR;
	if (lstat(physfn, &st) == 0)
		return FN_NVIDIA_VF;
	if (want_class && !read_base_class(dir, &f->base))
		return FN_UNREADABLE;
	return FN_NVIDIA_PF;
}

enum scan { SCAN_NONE, SCAN_FOUND, SCAN_FAILED };

/*
 * The driver names a GPU by domain, bus and slot only. Another NVIDIA display-class physical
 * function in the card's slot (functions 1-7; VFs and the audio function do not count) would
 * make that name ambiguous. The sysfs name of the function looked at last goes to found.
 */
static enum scan slot_scan(const char *devices, const struct gs_bdf *b, char *found)
{
	for (unsigned int fn = 1; fn < GS_SLOT_FNS; fn++) {
		struct fn_info f = { 0 };

		snprintf(found, GS_BDF_STR_SIZE, "%04x:%02x:%02x.%x", b->domain, b->bus & 0xffu,
			 (b->devfn >> 3) & 0x1fu, fn);
		switch (pci_fn(devices, found, true, &f)) {
		case FN_ABSENT:
		case FN_OTHER_VENDOR:
		case FN_NVIDIA_VF:
			continue;
		case FN_UNREADABLE:
			return SCAN_FAILED;
		case FN_NVIDIA_PF:
			if (f.base == 0x03)
				return SCAN_FOUND;
			continue;
		}
	}
	return SCAN_NONE;
}

static bool is_card(const char *name)
{
	for (size_t i = 0; i < hook.n_cards; i++)
		if (strcmp(hook.cards[i].name, name) == 0)
			return true;
	return false;
}

/*
 * Is `device` held only by mapped cards that still read as NVIDIA physical functions with that
 * ID? SCAN_NONE: yes. SCAN_FOUND: no, and why goes to `why`. SCAN_FAILED: an entry cannot be
 * read. Run at startup and again before every use of the device-ID fallback, never on the
 * attributed path.
 */
static enum scan id_holders(unsigned short device, char *why, size_t why_len)
{
	enum scan result = SCAN_NONE;
	struct dirent *e;
	DIR *d;

	for (size_t i = 0; i < hook.n_cards; i++) {
		const struct card *c = &hook.cards[i];
		struct fn_info f = { 0 };
		enum fn_kind kind;

		if (c->device != device)
			continue;
		kind = pci_fn(hook.devices, c->name, false, &f);
		if (kind == FN_UNREADABLE)
			return SCAN_FAILED;
		if (kind != FN_NVIDIA_PF || f.device != device) {
			snprintf(why, why_len, "card %s no longer reads as 10de:%04x", c->name, device);
			return SCAN_FOUND;
		}
	}
	d = opendir(hook.devices);
	if (!d)
		return SCAN_FAILED;
	while (result == SCAN_NONE && (e = readdir(d))) {
		struct fn_info f = { 0 };

		if (e->d_name[0] == '.')
			continue;
		switch (pci_fn(hook.devices, e->d_name, false, &f)) {
		case FN_ABSENT:
		case FN_UNREADABLE:
			result = SCAN_FAILED;
			continue;
		case FN_OTHER_VENDOR:
		case FN_NVIDIA_VF:
			continue;
		case FN_NVIDIA_PF:
			break;
		}
		if (f.device == device && !is_card(e->d_name)) {
			snprintf(why, why_len, "%s has device ID %04x but is unselected or skipped",
				 e->d_name, device);
			result = SCAN_FOUND;
		}
	}
	closedir(d);
	return result;
}

/* Which cards may use the device-ID fallback, as far as the PCI devices show at startup. */
static void mark_shared_ids(void)
{
	for (size_t i = 0; i < hook.n_cards; i++) {
		struct card *c = &hook.cards[i];
		bool seen = false;
		char why[REASON_LEN];

		for (size_t j = 0; j < i && !seen; j++) {
			if (hook.cards[j].device == c->device) {
				c->id_unique = hook.cards[j].id_unique;
				seen = true;
			}
		}
		if (seen)
			continue;
		switch (id_holders(c->device, why, sizeof(why))) {
		case SCAN_NONE:
			c->id_unique = true;
			break;
		case SCAN_FOUND:
			hook_log(LOG_INFO, "card %s: no device-ID fallback: %s", c->name, why);
			break;
		case SCAN_FAILED:
			hook_log(LOG_WARNING, "card %s: cannot scan the PCI devices; no device-ID "
				 "fallback", c->name);
			break;
		}
	}
}

/*
 * Resolve the selected PFs to cards. A card must be an NVIDIA display-class physical function
 * whose device ID is in the table, alone among NVIDIA display functions in its slot. Cards may
 * share a device ID: each reply is attributed to one card by its bus address.
 */
static size_t build_cards(const struct config *cfg)
{
	const char *devices = hook.devices;

	for (size_t i = 0; i < cfg->n_bdfs; i++) {
		const struct gs_bdf *b = &cfg->bdfs[i];
		const struct gs_device *gd;
		char name[GS_BDF_STR_SIZE], other[GS_BDF_STR_SIZE];
		struct fn_info f = { 0 };
		enum fn_kind kind;

		snprintf(name, sizeof(name), "%04x:%02x:%02x.%x", b->domain, b->bus & 0xffu,
			 (b->devfn >> 3) & 0x1fu, b->devfn & 7u);
		kind = pci_fn(devices, name, true, &f);
		if (kind == FN_ABSENT || kind == FN_UNREADABLE) {
			hook_log(LOG_WARNING, "card %s: cannot read %s/%s/{vendor,device,class}; "
				 "skipped", name, devices, name);
			continue;
		}
		if (kind == FN_NVIDIA_VF) {
			hook_log(LOG_WARNING, "card %s: is a virtual function; skipped", name);
			continue;
		}
		gd = kind == FN_NVIDIA_PF ? gs_device_lookup(f.device) : NULL;
		if (!gd) {
			hook_log(LOG_WARNING, "card %s: %04x:%04x is not in the device table; skipped",
				 name, f.vendor, f.device);
			continue;
		}
		if (f.base != 0x03) {
			hook_log(LOG_WARNING, "card %s: PCI base class %02x is not display; skipped",
				 name, f.base);
			continue;
		}
		switch (slot_scan(devices, b, other)) {
		case SCAN_NONE:
			break;
		case SCAN_FOUND:
			hook_log(LOG_ERR, "card %s: ambiguous slot, %s is another NVIDIA display "
				 "function; skipped", name, other);
			continue;
		case SCAN_FAILED:
			hook_log(LOG_ERR, "card %s: cannot read %s in the same slot; skipped", name,
				 other);
			continue;
		}

		struct card *c = &hook.cards[hook.n_cards++];
		const struct gs_target *t = gs_target_of(gd);

		c->domain = b->domain;
		c->bus = (uint8_t)b->bus;
		c->slot = (uint8_t)(b->devfn >> 3);
		c->device = gd->device;
		c->target = t->device;
		c->id_unique = false; /* until mark_shared_ids() */
		memcpy(c->name, name, sizeof(name));
		hook_log(LOG_INFO, "card %s: %s (10de:%04x, %s) presented as %s (10de:%04x), subsystem kept",
			 name, gd->name, gd->device, gd->die, t->name, t->device);
	}
	if (hook.n_cards)
		mark_shared_ids();
	return hook.n_cards;
}

/* Gates after the process check, in order; on any failure the hook stays inactive. */
static void decide(void)
{
	const char *path = secure_getenv(GS_HOOK_ENV_CONFIG);
	const char *root = test_env_or(GS_HOOK_TEST_ENV_SYSFS_ROOT, GS_HOOK_SYSFS_ROOT);
	char why[256], version[32];
	struct config cfg;
	bool supported = false;

	if (!path || !*path)
		path = GS_HOOK_DEFAULT_CONFIG;
	switch (load_config(path, &cfg, why, sizeof(why))) {
	case CONFIG_ABSENT:
		hook_log(LOG_INFO, "no config %s: identity off, every ioctl passes through", path);
		return;
	case CONFIG_BAD:
		hook_log(LOG_ERR, "config %s invalid (%s): every ioctl passes through", path, why);
		return;
	case CONFIG_OK:
		break;
	}
	if (!cfg.identity) {
		hook_log(LOG_INFO, "identity is not on in %s: every ioctl passes through", path);
		return;
	}
	if (!driver_version(version, sizeof(version), why, sizeof(why))) {
		hook_log(LOG_ERR, "driver version unknown (%s): every ioctl passes through", why);
		return;
	}
	for (size_t i = 0; i < GS_HOOK_DRIVER_VERSION_COUNT; i++)
		supported |= strcmp(version, gs_hook_driver_versions[i]) == 0;
	if (!supported) {
		hook_log(LOG_ERR, "driver %s is not supported: every ioctl passes through",
			 version);
		return;
	}
	if (cfg.n_bdfs == 0) {
		hook_log(LOG_ERR, "identity=on without bdfs in %s: every ioctl passes through", path);
		return;
	}
	if ((size_t)snprintf(hook.devices, sizeof(hook.devices), "%s/bus/pci/devices", root) >=
		    sizeof(hook.devices) ||
	    build_cards(&cfg) == 0) {
		hook_log(LOG_ERR, "no selected card can be mapped: every ioctl passes through");
		return;
	}
	hook.force_host = cfg.force_host;
	hook.active = true;
	hook_log(LOG_INFO,
		 "active for driver %s; replies are matched to cards by bus address; "
		 "virtualization mode %s. vGPU types and profile memory are the target's from the "
		 "stock vgpuConfig.xml and are not changed or filtered",
		 version, hook.force_host ? "forced to HOST for mapped cards only" : "unchanged");
}

/* The kernel's ioctl, used when dlsym() finds no next ioctl: -1 with errno set, as libc's wrapper returns. */
static int sys_ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	void *argp;

	va_start(ap, request);
	argp = va_arg(ap, void *);
	va_end(ap);
	return (int)syscall(SYS_ioctl, fd, request, argp);
}

static void hook_init(void)
{
	char comm[64];
	ssize_t n;

	hook.next = (ioctl_fn)dlsym(RTLD_NEXT, test_env_or(GS_HOOK_TEST_ENV_NEXT_IOCTL, "ioctl"));
	if (!hook.next) {
		const char *why = dlerror();

		hook_log(LOG_ERR, "dlsym(RTLD_NEXT, \"ioctl\") failed (%s): every ioctl goes straight to the kernel",
			 why ? why : "no error text");
		hook.next = sys_ioctl;
		return;
	}
	n = read_path("/proc/self/comm", comm, sizeof(comm) - 1);
	if (n > 0 && comm[n - 1] == '\n')
		comm[--n] = '\0';
	if (n <= 0 || strcmp(comm, GS_HOOK_TARGET_PROCESS) != 0)
		return; /* any other process: pass through, silently */
	decide();
}

__attribute__((constructor)) static void hook_ctor(void)
{
	int err = errno;

	pthread_once(&hook_once, hook_init);
	errno = err;
}

static void log_size_once(atomic_bool *flag, const char *what, uint32_t size, size_t want)
{
	if (!atomic_exchange(flag, true))
		hook_log(LOG_WARNING, "%s params are %u bytes, not %zu; replies left unchanged", what,
			 size, want);
}

/* Why an attribution failed, for one log line. */
struct query_error {
	uint32_t cmd;
	int err;         /* the ioctl failed with this errno */
	uint32_t status; /* else the RM status, when not NV_OK */
	const char *bad; /* else what was wrong with an NV_OK answer */
};

static void query_error_text(const struct query_error *e, char *buf, size_t len)
{
	if (e->bad)
		snprintf(buf, len, "query %08x: %s", e->cmd, e->bad);
	else if (e->err)
		snprintf(buf, len, "query %08x: ioctl errno %d", e->cmd, e->err);
	else
		snprintf(buf, len, "query %08x: status %#x", e->cmd, e->status);
}

/*
 * One read-only RM control on the daemon's fd and client, straight to the next ioctl (never
 * back into this wrapper). ioctl() restores the daemon's errno afterwards.
 */
static bool rm_query(int fd, uint32_t h_client, uint32_t h_object, uint32_t cmd, void *params,
		     uint32_t size, struct query_error *e)
{
	struct gs_nvos54 q = {
		.h_client = h_client,
		.h_object = h_object,
		.cmd = cmd,
		.flags = 0,
		.params = (uint64_t)(uintptr_t)params,
		.params_size = size,
		.status = QUERY_STATUS_UNSET,
	};
	int ret;

	errno = 0;
	ret = hook.next(fd, GS_NV_ESC_RM_CONTROL, &q);
	*e = (struct query_error){ .cmd = cmd, .status = q.status };
	if (ret != 0) {
		e->err = errno ? errno : EIO;
		return false;
	}
	return q.status == GS_NV_OK;
}

struct gpu_addr {
	uint32_t domain;
	uint16_t bus;
	uint16_t slot;
};

/* Subdevice handle -> gpuId (GPU_GET_ID) -> the GPU's domain, bus, slot (root GET_PCI_INFO). */
static bool subdevice_addr(int fd, uint32_t h_client, uint32_t h_subdevice, struct gpu_addr *a,
			   struct query_error *e)
{
	struct gs_gpu_id id = { 0 };
	struct gs_gpu_pci_loc loc;

	if (!rm_query(fd, h_client, h_subdevice, GS_NV2080_CTRL_CMD_GPU_GET_ID, &id, sizeof(id),
		      e))
		return false;
	loc = (struct gs_gpu_pci_loc){ .gpu_id = id.gpu_id };
	if (!rm_query(fd, h_client, h_client, GS_NV0000_CTRL_CMD_GPU_GET_PCI_INFO, &loc,
		      sizeof(loc), e))
		return false;
	if (loc.gpu_id != id.gpu_id || loc.bus > 0xff || loc.slot > 0x1f) {
		e->bad = "malformed PCI location";
		return false;
	}
	*a = (struct gpu_addr){ loc.domain, loc.bus, loc.slot };
	return true;
}

/* Device handle -> its only subdevice (an existing one, found, not allocated) -> address. */
static bool device_addr(int fd, uint32_t h_client, uint32_t h_device, struct gpu_addr *a,
			struct query_error *e)
{
	struct gs_num_subdevices num = { 0 };
	struct gs_find_subdevice find = { .sub_device_inst = 0, .h_sub_device = 0 };

	if (!rm_query(fd, h_client, h_device, GS_NV0080_CTRL_CMD_GPU_GET_NUM_SUBDEVICES, &num,
		      sizeof(num), e))
		return false;
	if (num.num_subdevices != 1) {
		e->bad = "not exactly one subdevice";
		return false;
	}
	if (!rm_query(fd, h_client, h_device, GS_NV0080_CTRL_CMD_GPU_FIND_SUBDEVICE_HANDLE, &find,
		      sizeof(find), e))
		return false;
	if (!find.h_sub_device) {
		e->bad = "null subdevice handle";
		return false;
	}
	return subdevice_addr(fd, h_client, find.h_sub_device, a, e);
}

static struct card *card_at(const struct gpu_addr *a)
{
	for (size_t i = 0; i < hook.n_cards; i++) {
		struct card *c = &hook.cards[i];

		if (c->domain == a->domain && c->bus == a->bus && c->slot == a->slot)
			return c;
	}
	return NULL;
}

static uint32_t nvidia_word(unsigned short device)
{
	return ((uint32_t)device << 16) | GS_VENDOR_NVIDIA;
}

static void present(void *params, struct gs_pci_info *info, const struct card *c)
{
	info->pci_device_id = nvidia_word(c->target);
	memcpy(params, info, sizeof(*info)); /* revision and pciExtDeviceId kept */
}

static void on_pci_info(int fd, const struct gs_nvos54 *io)
{
	void *params = (void *)(uintptr_t)io->params;
	const struct card *by_id = NULL;
	struct gs_pci_info info;
	struct query_error e = { 0 };
	struct gpu_addr a = { 0 };
	char why[96];

	memcpy(&info, params, sizeof(info));
	for (size_t i = 0; i < hook.n_cards && !by_id; i++)
		if (info.pci_device_id == nvidia_word(hook.cards[i].device))
			by_id = &hook.cards[i];
	if (!by_id) { /* no card has this device ID: nothing to ask the driver */
		if (!atomic_exchange(&logged_unmapped, true))
			hook_log(LOG_INFO, "GET_PCI_INFO: device word %08x is not a selected card; "
				 "unchanged", info.pci_device_id);
		return;
	}
	if (subdevice_addr(fd, io->h_client, io->h_object, &a, &e)) {
		struct card *c = card_at(&a);

		if (!c) {
			if (!atomic_exchange(&logged_pci_other, true))
				hook_log(LOG_INFO, "GET_PCI_INFO: reply from GPU %04x:%02x:%02x, not "
					 "a mapped card; unchanged", a.domain, a.bus, a.slot);
			return;
		}
		if (info.pci_device_id != nvidia_word(c->device)) {
			if (!atomic_exchange(&logged_pci_mismatch, true))
				hook_log(LOG_WARNING, "GET_PCI_INFO: reply from card %s has device "
					 "word %08x, not 10de:%04x; unchanged", c->name,
					 info.pci_device_id, c->device);
			return;
		}
		present(params, &info, c);
		if (!atomic_exchange(&c->logged_rewrite, true))
			hook_log(LOG_INFO, "GET_PCI_INFO: card %s 10de:%04x presented as 10de:%04x, "
				 "subsystem %08x kept", c->name, c->device, c->target, info.pci_sub_system_id);
		return;
	}
	query_error_text(&e, why, sizeof(why));
	if (by_id->id_unique) { /* only mapped cards had this ID, all with the same target */
		char changed[REASON_LEN];
		enum scan now = id_holders(by_id->device, changed, sizeof(changed));

		if (now == SCAN_NONE) {
			present(params, &info, by_id);
			if (!atomic_exchange(&logged_pci_fallback, true))
				hook_log(LOG_WARNING, "GET_PCI_INFO: cannot attribute the reply to a "
					 "GPU (%s); 10de:%04x presented as 10de:%04x by device ID, "
					 "which only mapped cards have", why, by_id->device,
					 by_id->target);
			return;
		}
		if (!atomic_exchange(&logged_pci_fallback_refused, true))
			hook_log(LOG_WARNING, "GET_PCI_INFO: cannot attribute the reply to a GPU "
				 "(%s); unchanged, device-ID fallback refused: %s", why,
				 now == SCAN_FAILED ? "cannot scan the PCI devices" : changed);
		return;
	}
	if (!atomic_exchange(&logged_pci_unattributed, true))
		hook_log(LOG_WARNING, "GET_PCI_INFO: cannot attribute the reply to a GPU (%s); "
			 "unchanged", why);
}

static void on_virt_mode(int fd, const struct gs_nvos54 *io)
{
	void *params = (void *)(uintptr_t)io->params;
	struct query_error e = { 0 };
	struct gpu_addr a = { 0 };
	struct card *c;
	uint32_t mode;
	char why[96];

	memcpy(&mode, params, sizeof(mode));
	if (mode == GS_NV0080_CTRL_GPU_VIRTUALIZATION_MODE_HOST)
		return;
	if (!device_addr(fd, io->h_client, io->h_object, &a, &e)) {
		query_error_text(&e, why, sizeof(why));
		if (!atomic_exchange(&logged_virt_unattributed, true))
			hook_log(LOG_WARNING, "GET_VIRTUALIZATION_MODE: cannot attribute the reply to "
				 "a GPU (%s); unchanged", why);
		return;
	}
	c = card_at(&a);
	if (!c) {
		if (!atomic_exchange(&logged_virt_other, true))
			hook_log(LOG_INFO, "GET_VIRTUALIZATION_MODE: reply from GPU %04x:%02x:%02x, "
				 "not a mapped card; unchanged", a.domain, a.bus, a.slot);
		return;
	}
	mode = GS_NV0080_CTRL_GPU_VIRTUALIZATION_MODE_HOST;
	memcpy(params, &mode, sizeof(mode)); /* virtualizationMode only; isGridBuild kept */
	if (!atomic_exchange(&c->logged_forced, true))
		hook_log(LOG_INFO, "GET_VIRTUALIZATION_MODE: card %s reply forced to HOST (3)",
			 c->name);
}

/* p is the daemon's own NVOS54_PARAMETERS of an RM control the driver just completed on fd. */
static void after_rm_control(int fd, const struct gs_nvos54 *p)
{
	struct gs_nvos54 io;
	bool pci;

	memcpy(&io, p, sizeof(io));
	if (io.status != GS_NV_OK)
		return;
	if (io.cmd == GS_NV2080_CTRL_CMD_BUS_GET_PCI_INFO)
		pci = true;
	else if (io.cmd == GS_NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE && hook.force_host)
		pci = false;
	else
		return;
	if (pci && io.params_size != sizeof(struct gs_pci_info)) {
		log_size_once(&logged_pci_size, "GET_PCI_INFO", io.params_size,
			      sizeof(struct gs_pci_info));
		return;
	}
	if (!pci && io.params_size != sizeof(struct gs_virt_mode)) {
		log_size_once(&logged_virt_size, "GET_VIRTUALIZATION_MODE", io.params_size,
			      sizeof(struct gs_virt_mode));
		return;
	}
	if (io.flags & GS_NVOS54_FLAGS_FINN_SERIALIZED) {
		if (!atomic_exchange(&logged_finn, true))
			hook_log(LOG_WARNING, "FINN-serialized RM control; replies left unchanged");
		return;
	}
	if (!io.params)
		return;
	if (pci)
		on_pci_info(fd, &io);
	else
		on_virt_mode(fd, &io);
}

/*
 * libc's variadic ioctl. The third argument is read as one void * and forwarded unchanged,
 * exactly as glibc's own wrapper does (sysdeps/unix/sysv/linux/ioctl.c: va_arg(args, void *)
 * then the syscall), so requests that take an integer or no argument reach the next ioctl
 * with the same register value glibc would have passed to the kernel.
 */
GS_HOOK_EXPORT int ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	void *argp;
	int err = errno;
	int ret;

	va_start(ap, request);
	argp = va_arg(ap, void *);
	va_end(ap);

	pthread_once(&hook_once, hook_init);
	errno = err;
	ret = hook.next(fd, request, argp);
	if (ret < 0 || !hook.active || request != GS_NV_ESC_RM_CONTROL || !argp)
		return ret;

	/* The caller sees the driver's errno and return value, whatever happens below. */
	err = errno;
	after_rm_control(fd, argp);
	errno = err;
	return ret;
}
