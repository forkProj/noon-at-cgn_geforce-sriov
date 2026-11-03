// SPDX-License-Identifier: MIT
/* PCI addresses and /etc/geforce-sriov/config (format shared with hook/gs_hook.c). */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gs_lifecycle.h"

int gs_addr_parse(const char *s, struct gs_addr *out)
{
	struct gs_bdf b;
	size_t n = s ? strlen(s) : 0;

	if (n == 0 || n >= GS_BDF_STR_SIZE || !gs_parse_bdf(s, &b))
		return -1;
	out->domain = b.domain;
	out->bus = b.bus;
	out->slot = b.devfn >> 3;
	out->func = b.devfn & 7;
	memcpy(out->str, s, n + 1);
	return 0;
}

int gs_parse_switch(const char *value, int *out)
{
	if (strcmp(value, "on") == 0)
		*out = GS_SW_ON;
	else if (strcmp(value, "off") == 0)
		*out = GS_SW_OFF;
	else
		return -1;
	return 0;
}

int gs_parse_addr_list(const char *key, const char *value, struct gs_addr *out, unsigned int *n, char *err,
		       size_t errlen)
{
	const char *p = value;
	unsigned int count = 0, i;

	for (;;) {
		const char *comma = strchr(p, ',');
		size_t len = comma ? (size_t)(comma - p) : strlen(p);
		char item[GS_BDF_STR_SIZE + 1];
		struct gs_addr b;

		if (len == 0 || len >= sizeof(item)) {
			snprintf(err, errlen, "%s: empty or malformed entry", key);
			return -1;
		}
		memcpy(item, p, len);
		item[len] = '\0';
		if (gs_addr_parse(item, &b)) {
			snprintf(err, errlen, "%s: '%s' is not a full PCI address dddd:bb:ss.f as /sys/bus/pci/devices names it (lowercase hex; a domain longer than 4 digits has no leading zero; at most %x)",
				 key, item, GS_PCI_DOMAIN_MAX);
			return -1;
		}
		if (b.func != 0) {
			snprintf(err, errlen, "%s: %s is not function 0; select the GPU's physical function", key, item);
			return -1;
		}
		for (i = 0; i < count; i++) {
			if (strcmp(out[i].str, b.str) == 0) {
				snprintf(err, errlen, "%s: %s listed twice", key, item);
				return -1;
			}
		}
		if (count == GS_CFG_MAX_BDFS) {
			snprintf(err, errlen, "%s: more than %u devices", key, GS_CFG_MAX_BDFS);
			return -1;
		}
		out[count++] = b;
		if (!comma)
			break;
		p = comma + 1;
	}
	*n = count;
	return 0;
}

void gs_addr_list_str(const struct gs_addr *list, unsigned int n, char *buf, size_t len)
{
	size_t off = 0;

	if (len)
		buf[0] = '\0';
	for (unsigned int i = 0; i < n && off < len; i++) {
		int k = snprintf(buf + off, len - off, "%s%s", i ? "," : "", list[i].str);

		if (k < 0)
			break;
		off += (size_t)k;
	}
}

int gs_config_grants_release(const struct gs_config *cfg, const char *pf)
{
	for (unsigned int i = 0; i < cfg->nrelease; i++)
		if (strcmp(cfg->boot_display_release[i].str, pf) == 0)
			return 1;
	return 0;
}

void gs_config_defaults(struct gs_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->ari = GS_SW_ON;
	cfg->msix = GS_SW_ON;
	cfg->experimental_vfs = GS_SW_OFF;
	cfg->twin_id = GS_SW_OFF;
	cfg->identity = GS_SW_OFF;
	cfg->identity_force_host = GS_SW_OFF;
	snprintf(cfg->sriov_manage, sizeof(cfg->sriov_manage), "%s", GS_SRIOV_MANAGE_DEFAULT);
}

unsigned int gs_config_vfs(const struct gs_config *cfg)
{
	return cfg->vfs ? cfg->vfs : GS_DEFAULT_VFS;
}

int gs_parse_vfs(const char *value, unsigned int *out)
{
	unsigned int v = 0;
	size_t i;

	if (!value[0] || value[0] == '0' || strlen(value) > 2)
		return -1;
	for (i = 0; value[i]; i++) {
		if (value[i] < '0' || value[i] > '9')
			return -1;
		v = v * 10 + (unsigned int)(value[i] - '0');
	}
	if (v < 1 || v > GS_VFS_MAX)
		return -1;
	*out = v;
	return 0;
}

int gs_vfs_check(const struct gs_device *e, const struct gs_config *cfg, char *why, size_t len)
{
	unsigned int ceiling = gs_vf_ceiling(e, e->totalvfs, cfg->experimental_vfs == GS_SW_ON);
	unsigned int vfs = gs_config_vfs(cfg);

	if (cfg->vfs && cfg->vfs > ceiling) {
		if (ceiling == e->totalvfs)
			snprintf(why, len, "vfs=%u is above the hardware default of 10de:%04x, %u VFs", cfg->vfs,
				 e->device, e->totalvfs);
		else
			snprintf(why, len, "vfs=%u is above the %u VFs experimental_vfs=on presents on 10de:%04x (hardware default %u)",
				 cfg->vfs, ceiling, e->device, e->totalvfs);
		return -1;
	}
	if ((vfs < ceiling ? vfs : ceiling) > GS_DEFAULT_VFS) {
		snprintf(why, len, "vfs=%u is above the default of %u: ARI decoding in the GPU does not work for now, so more than %u VFs are expected not to work (the module refuses to load if the card's VF layout check fails)",
			 vfs, GS_DEFAULT_VFS, GS_DEFAULT_VFS);
		return 1;
	}
	return 0;
}

void gs_vfs_describe(const struct gs_device *e, const struct gs_config *cfg, char *buf, size_t len)
{
	int exp = cfg->experimental_vfs == GS_SW_ON;
	unsigned int ceiling = gs_vf_ceiling(e, e->totalvfs, exp);
	char how[32];

	if (cfg->vfs)
		snprintf(how, sizeof(how), "vfs=%u", cfg->vfs);
	else
		snprintf(how, sizeof(how), "default");
	snprintf(buf, len, "hardware default %u, geforce-sriov presents %u (%s%s)", e->totalvfs,
		 gs_vf_limit(e, e->totalvfs, exp, gs_config_vfs(cfg)), how,
		 ceiling > e->totalvfs ? ", experimental_vfs=on" : "");
}

static int valid_key(const char *k, size_t n)
{
	size_t i;

	if (n == 0)
		return 0;
	for (i = 0; i < n; i++) {
		char c = k[i];

		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
			return 0;
	}
	return 1;
}

enum { K_BDFS, K_RELEASE, K_ARI, K_MSIX, K_EXPVFS, K_TWIN_ID, K_VFS, K_IDENTITY, K_FORCE_HOST, K_SRIOV_MANAGE, K_COUNT };
static const char *const keys[K_COUNT] = {
	"bdfs", "boot_display_release", "ari", "msix", "experimental_vfs", "twin_id", "vfs", "identity",
	"identity_force_host", "sriov_manage",
};

int gs_config_parse(const char *text, size_t len, struct gs_config *cfg, char *err, size_t errlen)
{
	int seen[K_COUNT] = { 0 };
	size_t pos = 0;
	unsigned int lineno = 0, release_line = 0;

	gs_config_defaults(cfg);
	if (len > GS_CONFIG_MAX_BYTES) {
		snprintf(err, errlen, "larger than %d bytes", GS_CONFIG_MAX_BYTES);
		return -1;
	}
	if (memchr(text, '\0', len)) {
		snprintf(err, errlen, "contains a NUL byte");
		return -1;
	}
	while (pos < len) {
		const char *line = text + pos;
		const char *nl = memchr(line, '\n', len - pos);
		size_t n = nl ? (size_t)(nl - line) : len - pos;
		const char *eq;
		char value[GS_PATH_MAX];
		size_t klen, vlen, i;
		int k;

		pos += n + (nl ? 1 : 0);
		lineno++;
		if (n == 0 || line[0] == '#')
			continue;
		for (i = 0; i < n; i++) {
			if (line[i] == ' ' || line[i] == '\t' || line[i] == '\r' || line[i] == '\v' ||
			    line[i] == '\f') {
				snprintf(err, errlen, "line %u: whitespace is not allowed", lineno);
				return -1;
			}
		}
		eq = memchr(line, '=', n);
		if (!eq) {
			snprintf(err, errlen, "line %u: expected key=value", lineno);
			return -1;
		}
		klen = (size_t)(eq - line);
		vlen = n - klen - 1;
		if (!valid_key(line, klen)) {
			snprintf(err, errlen, "line %u: key must match [a-z0-9_]+", lineno);
			return -1;
		}
		if (vlen == 0 || vlen >= sizeof(value)) {
			snprintf(err, errlen, "line %u: empty or overlong value", lineno);
			return -1;
		}
		memcpy(value, eq + 1, vlen);
		value[vlen] = '\0';
		for (k = 0; k < K_COUNT; k++) {
			if (strlen(keys[k]) == klen && memcmp(keys[k], line, klen) == 0)
				break;
		}
		if (k == K_COUNT) {
			if (memmem(line, klen, "vf", 2))
				snprintf(err, errlen, "line %u: unknown key '%.*s'; the VF count key is vfs (vfs=1..%u, default %u)",
					 lineno, (int)klen, line, GS_VFS_MAX, GS_DEFAULT_VFS);
			else
				snprintf(err, errlen, "line %u: unknown key '%.*s'", lineno, (int)klen, line);
			return -1;
		}
		if (seen[k]++) {
			snprintf(err, errlen, "line %u: duplicate key '%s'", lineno, keys[k]);
			return -1;
		}
		switch (k) {
		case K_BDFS:
		case K_RELEASE: {
			char sub[512];
			int r = k == K_BDFS ?
				gs_parse_addr_list(keys[k], value, cfg->bdfs, &cfg->nbdfs, sub, sizeof(sub)) :
				gs_parse_addr_list(keys[k], value, cfg->boot_display_release, &cfg->nrelease, sub,
						   sizeof(sub));

			if (r) {
				snprintf(err, errlen, "line %u: %s", lineno, sub);
				return -1;
			}
			if (k == K_RELEASE)
				release_line = lineno;
			break;
		}
		case K_ARI:
		case K_MSIX:
		case K_EXPVFS:
		case K_TWIN_ID:
		case K_IDENTITY:
		case K_FORCE_HOST: {
			int *dst = k == K_ARI ? &cfg->ari : k == K_MSIX ? &cfg->msix :
				   k == K_EXPVFS ? &cfg->experimental_vfs : k == K_TWIN_ID ? &cfg->twin_id :
				   k == K_IDENTITY ? &cfg->identity : &cfg->identity_force_host;

			if (gs_parse_switch(value, dst)) {
				snprintf(err, errlen, "line %u: %s must be on or off", lineno, keys[k]);
				return -1;
			}
			break;
		}
		case K_VFS:
			if (gs_parse_vfs(value, &cfg->vfs)) {
				snprintf(err, errlen, "line %u: vfs must be a number of VFs from 1 to %u", lineno, GS_VFS_MAX);
				return -1;
			}
			break;
		case K_SRIOV_MANAGE:
			if (gs_validate_abs_path(value)) {
				snprintf(err, errlen, "line %u: sriov_manage must be a normalized absolute path",
					 lineno);
				return -1;
			}
			snprintf(cfg->sriov_manage, sizeof(cfg->sriov_manage), "%s", value);
			break;
		}
	}
	/* A console release is granted only for a selected GPU. */
	for (unsigned int i = 0; i < cfg->nrelease; i++) {
		unsigned int j;

		for (j = 0; j < cfg->nbdfs && strcmp(cfg->bdfs[j].str, cfg->boot_display_release[i].str); j++)
			;
		if (j == cfg->nbdfs) {
			snprintf(err, errlen, "line %u: boot_display_release: %s is not listed in bdfs", release_line,
				 cfg->boot_display_release[i].str);
			return -1;
		}
	}
	cfg->present = 1;
	return 0;
}

int gs_config_load(const char *path, struct gs_config *cfg, char *err, size_t errlen)
{
	char *data;
	size_t len;
	int r = gs_read_whole(path, &data, &len, GS_CONFIG_MAX_BYTES);

	if (r == -ENOENT) {
		gs_config_defaults(cfg);
		return 0;
	}
	if (r) {
		snprintf(err, errlen, "%s: %s", path,
			 r == -EFBIG ? "larger than 65536 bytes" :
			 r == -EINVAL ? "not a regular file" : strerror(-r));
		return -1;
	}
	if (gs_config_parse(data, len, cfg, err, errlen)) {
		char sub[512];

		snprintf(sub, sizeof(sub), "%s", err);
		snprintf(err, errlen, "%s: %s", path, sub);
		free(data);
		return -1;
	}
	free(data);
	return 0;
}

const char *gs_config_path(void)
{
	const char *env = getenv(GS_CONFIG_ENV);

	return env && env[0] ? env : GS_CONFIG_PATH;
}

int gs_config_render(const struct gs_config *cfg, char *buf, size_t len)
{
	size_t off = 0;
	unsigned int i;
	int n;

#define EMIT(...) do { \
		n = snprintf(buf + off, len - off, __VA_ARGS__); \
		if (n < 0 || (size_t)n >= len - off) \
			return -1; \
		off += (size_t)n; \
	} while (0)

	EMIT("# geforce-sriov configuration: key=value, no spaces; see docs.\n");
	EMIT("# bdfs: the GPUs (physical function .0) that prepare/enable may act on.\n");
	if (cfg->nbdfs) {
		EMIT("bdfs=");
		for (i = 0; i < cfg->nbdfs; i++)
			EMIT("%s%s", i ? "," : "", cfg->bdfs[i].str);
		EMIT("\n");
	} else {
		EMIT("# no device selected: boot-time preparation does nothing\n");
	}
	if (cfg->nrelease) {
		EMIT("# boot_display_release: boot display GPUs whose console `prepare --release-console` may release;\n"
		     "# the boot unit then prepares nothing and prepare is run by hand after boot.\n");
		EMIT("boot_display_release=");
		for (i = 0; i < cfg->nrelease; i++)
			EMIT("%s%s", i ? "," : "", cfg->boot_display_release[i].str);
		EMIT("\n");
	}
	EMIT("ari=%s\n", cfg->ari == GS_SW_OFF ? "off" : "on");
	EMIT("msix=%s\n", cfg->msix == GS_SW_OFF ? "off" : "on");
	EMIT("experimental_vfs=%s\n", cfg->experimental_vfs == GS_SW_ON ? "on" : "off");
	EMIT("twin_id=%s\n", cfg->twin_id == GS_SW_ON ? "on" : "off");
	if (cfg->vfs)
		EMIT("vfs=%u\n", cfg->vfs);
	EMIT("identity=%s\n", cfg->identity == GS_SW_ON ? "on" : "off");
	EMIT("identity_force_host=%s\n", cfg->identity_force_host == GS_SW_ON ? "on" : "off");
	if (strcmp(cfg->sriov_manage, GS_SRIOV_MANAGE_DEFAULT) != 0)
		EMIT("sriov_manage=%s\n", cfg->sriov_manage);
#undef EMIT
	return (int)off;
}
