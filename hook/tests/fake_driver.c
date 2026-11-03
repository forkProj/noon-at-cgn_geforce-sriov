// SPDX-License-Identifier: MIT
/*
 * Stand-in for the NVIDIA driver. Preloaded after the hook, so the hook's
 * dlsym(RTLD_NEXT, "ioctl") lands here. NV_ESC_RM_CONTROL never reaches the kernel:
 *  - the daemon's own controls (GET_PCI_INFO, GET_VIRTUALIZATION_MODE, FIX_OTHER_CMD): the
 *    caller has already put "the driver's reply" into its params; this only sets the RM status
 *    from the hObject word or fails the call (fixture.h);
 *  - the hook's four attribution queries: answered from the object table (fixture.h), with
 *    the injected faults, after checking every rule a query must keep. Each query and each
 *    broken rule is recorded per thread for gs_fake_take_queries().
 * Any other request goes to libc.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include "fixture.h"

#define EXPORT __attribute__((visibility("default")))
#define MAX_ENTRIES 64

enum fault_kind { F_NONE, F_STATUS, F_EIO, F_COUNT, F_ZERO, F_BUS, F_SLOT, F_ECHO };

struct entry {
	char type; /* 's' sub, 'd' dev, 'g' gpu, 'f' fault */
	uint32_t key; /* hSubDevice, hDevice, gpuId; fault: hObject or gpuId */
	uint32_t a, b, c; /* sub: gpuId; dev: num, hSubDevice; gpu: domain, bus, slot */
	uint32_t cmd; /* fault: the query */
	enum fault_kind kind;
	uint32_t value;
};

/* Replaced only between the daemon's operations, never while its threads run. */
static struct entry table[MAX_ENTRIES];
static size_t n_table;

/* The calling thread's last daemon control and the chain of queries that followed it. */
static __thread struct {
	bool have_call, have_gpu_id, have_found;
	int fd;
	uint32_t client, object, gpu_id, found_sub;
	char trace[FAKE_TRACE_MAX];
	size_t len;
	unsigned int queries, bad;
} t;

static uint32_t num(const char *s, bool *ok)
{
	char *end;
	unsigned long v;

	if (!s) {
		*ok = false;
		return 0;
	}
	errno = 0;
	v = strtoul(s, &end, 0);
	if (errno || *end || v > UINT32_MAX)
		*ok = false;
	return (uint32_t)v;
}

static uint32_t query_cmd(const char *name)
{
	if (strcmp(name, "id") == 0)
		return GS_NV2080_CTRL_CMD_GPU_GET_ID;
	if (strcmp(name, "pci") == 0)
		return GS_NV0000_CTRL_CMD_GPU_GET_PCI_INFO;
	if (strcmp(name, "num") == 0)
		return GS_NV0080_CTRL_CMD_GPU_GET_NUM_SUBDEVICES;
	if (strcmp(name, "find") == 0)
		return GS_NV0080_CTRL_CMD_GPU_FIND_SUBDEVICE_HANDLE;
	return 0;
}

static enum fault_kind fault_kind(const char *name)
{
	static const char *const names[] = { "", "status", "eio", "count", "zero", "bus", "slot",
					     "echo" };

	for (size_t i = 1; name && i < sizeof(names) / sizeof(names[0]); i++)
		if (strcmp(name, names[i]) == 0)
			return (enum fault_kind)i;
	return F_NONE;
}

EXPORT int gs_fake_load_table(const char *path)
{
	FILE *f = fopen(path, "re");
	char line[256];
	bool ok = true;

	if (!f)
		return -1;
	n_table = 0;
	while (ok && fgets(line, sizeof(line), f)) {
		char *save = NULL, *w[6] = { 0 };
		struct entry *e = &table[n_table];

		for (size_t i = 0; i < 6; i++)
			w[i] = strtok_r(i ? NULL : line, " \t\n", &save);
		if (!w[0])
			continue;
		if (n_table == MAX_ENTRIES) {
			ok = false;
			break;
		}
		memset(e, 0, sizeof(*e));
		e->type = w[0][0];
		if (strcmp(w[0], "sub") == 0) {
			e->key = num(w[1], &ok);
			e->a = num(w[2], &ok);
		} else if (strcmp(w[0], "dev") == 0) {
			e->key = num(w[1], &ok);
			e->a = num(w[2], &ok);
			e->b = num(w[3], &ok);
		} else if (strcmp(w[0], "gpu") == 0) {
			e->key = num(w[1], &ok);
			e->a = num(w[2], &ok);
			e->b = num(w[3], &ok);
			e->c = num(w[4], &ok);
		} else if (strcmp(w[0], "fault") == 0) {
			e->cmd = w[1] ? query_cmd(w[1]) : 0;
			e->key = num(w[2], &ok);
			e->kind = fault_kind(w[3]);
			e->value = num(w[4], &ok);
			ok = ok && e->cmd && e->kind != F_NONE;
		} else {
			ok = false;
		}
		n_table++;
	}
	fclose(f);
	if (!ok)
		n_table = 0;
	return ok ? 0 : -1;
}

__attribute__((constructor)) static void load_initial_table(void)
{
	const char *path = getenv(FAKE_ENV_TABLE);

	if (path && *path && gs_fake_load_table(path) != 0) {
		fprintf(stderr, "fake driver: bad table %s\n", path);
		exit(2);
	}
}

EXPORT void gs_fake_take_queries(char *buf, size_t len, unsigned int *queries, unsigned int *bad)
{
	snprintf(buf, len, "%s", t.len ? t.trace : "-");
	*queries = t.queries;
	*bad = t.bad;
	t.len = 0;
	t.trace[0] = '\0';
	t.queries = 0;
	t.bad = 0;
}

__attribute__((format(printf, 1, 2))) static void trace(const char *fmt, ...)
{
	va_list ap;
	int n;

	if (t.len + 1 >= sizeof(t.trace))
		return;
	if (t.len)
		t.trace[t.len++] = ',';
	va_start(ap, fmt);
	n = vsnprintf(t.trace + t.len, sizeof(t.trace) - t.len, fmt, ap);
	va_end(ap);
	if (n > 0)
		t.len += (size_t)n;
	if (t.len >= sizeof(t.trace))
		t.len = sizeof(t.trace) - 1;
}

static void broke(const char *rule)
{
	t.bad++;
	trace("BAD:%s", rule);
}

static const struct entry *find(char type, uint32_t key)
{
	for (size_t i = 0; i < n_table; i++)
		if (table[i].type == type && table[i].key == key)
			return &table[i];
	return NULL;
}

static const struct entry *fault_for(uint32_t cmd, uint32_t key)
{
	for (size_t i = 0; i < n_table; i++)
		if (table[i].type == 'f' && table[i].cmd == cmd && table[i].key == key)
			return &table[i];
	return NULL;
}

static uint32_t query_size(uint32_t cmd)
{
	switch (cmd) {
	case GS_NV2080_CTRL_CMD_GPU_GET_ID:
		return sizeof(struct gs_gpu_id);
	case GS_NV0000_CTRL_CMD_GPU_GET_PCI_INFO:
		return sizeof(struct gs_gpu_pci_loc);
	case GS_NV0080_CTRL_CMD_GPU_GET_NUM_SUBDEVICES:
		return sizeof(struct gs_num_subdevices);
	case GS_NV0080_CTRL_CMD_GPU_FIND_SUBDEVICE_HANDLE:
		return sizeof(struct gs_find_subdevice);
	}
	return 0;
}

static const char *query_name(uint32_t cmd)
{
	switch (cmd) {
	case GS_NV2080_CTRL_CMD_GPU_GET_ID:
		return "id";
	case GS_NV0000_CTRL_CMD_GPU_GET_PCI_INFO:
		return "pci";
	case GS_NV0080_CTRL_CMD_GPU_GET_NUM_SUBDEVICES:
		return "num";
	}
	return "find";
}

static int answer(int status)
{
	errno = FAKE_QUERY_ERRNO;
	return status;
}

/* One attribution query from the hook. */
static int query(int fd, struct gs_nvos54 *p)
{
	const uint32_t cmd = p->cmd;
	void *params = (void *)(uintptr_t)p->params;
	bool size_ok = p->params_size == query_size(cmd) && params;
	uint32_t key = p->h_object;
	const struct entry *e, *fault;

	t.queries++;
	trace("%s:%x", query_name(cmd), p->h_object);
	if (!t.have_call)
		broke("nocall");
	if (fd != t.fd)
		broke("fd");
	if (p->h_client != t.client)
		broke("client");
	if (p->flags != 0)
		broke("flags");
	if (!size_ok)
		broke("size");
	if (p->status == GS_NV_OK)
		broke("status");

	switch (cmd) {
	case GS_NV2080_CTRL_CMD_GPU_GET_ID:
		if (p->h_object != t.object && !(t.have_found && p->h_object == t.found_sub))
			broke("object");
		break;
	case GS_NV0000_CTRL_CMD_GPU_GET_PCI_INFO:
		if (p->h_object != p->h_client)
			broke("object");
		if (size_ok) {
			key = ((struct gs_gpu_pci_loc *)params)->gpu_id;
			if (!t.have_gpu_id || key != t.gpu_id)
				broke("gpuid");
		}
		break;
	case GS_NV0080_CTRL_CMD_GPU_GET_NUM_SUBDEVICES:
		if (p->h_object != t.object)
			broke("object");
		break;
	case GS_NV0080_CTRL_CMD_GPU_FIND_SUBDEVICE_HANDLE:
		if (p->h_object != t.object)
			broke("object");
		if (size_ok && ((struct gs_find_subdevice *)params)->sub_device_inst != 0)
			broke("inst");
		break;
	}
	if (!size_ok) {
		p->status = FAKE_NV_ERR_ARG; /* the driver checks paramsSize exactly */
		return answer(0);
	}

	fault = fault_for(cmd, key);
	if (fault && fault->kind == F_EIO) {
		errno = EIO;
		return -1;
	}
	if (fault && fault->kind == F_STATUS) {
		p->status = fault->value;
		return answer(0);
	}

	switch (cmd) {
	case GS_NV2080_CTRL_CMD_GPU_GET_ID: {
		struct gs_gpu_id *id = params;

		e = find('s', p->h_object);
		if (!e) {
			p->status = FAKE_NV_ERR_OBJECT;
			break;
		}
		id->gpu_id = e->a;
		t.gpu_id = e->a;
		t.have_gpu_id = true;
		p->status = GS_NV_OK;
		break;
	}
	case GS_NV0000_CTRL_CMD_GPU_GET_PCI_INFO: {
		struct gs_gpu_pci_loc *loc = params;

		e = find('g', loc->gpu_id);
		if (!e) {
			p->status = FAKE_NV_ERR_ARG;
			break;
		}
		loc->domain = e->a;
		loc->bus = (uint16_t)e->b;
		loc->slot = (uint16_t)e->c;
		if (fault && fault->kind == F_BUS)
			loc->bus = (uint16_t)fault->value;
		if (fault && fault->kind == F_SLOT)
			loc->slot = (uint16_t)fault->value;
		if (fault && fault->kind == F_ECHO)
			loc->gpu_id ^= 1u;
		p->status = GS_NV_OK;
		break;
	}
	case GS_NV0080_CTRL_CMD_GPU_GET_NUM_SUBDEVICES: {
		struct gs_num_subdevices *n = params;

		e = find('d', p->h_object);
		if (!e) {
			p->status = FAKE_NV_ERR_OBJECT;
			break;
		}
		n->num_subdevices = fault && fault->kind == F_COUNT ? fault->value : e->a;
		p->status = GS_NV_OK;
		break;
	}
	case GS_NV0080_CTRL_CMD_GPU_FIND_SUBDEVICE_HANDLE: {
		struct gs_find_subdevice *fs = params;

		e = find('d', p->h_object);
		if (!e || fs->sub_device_inst != 0) {
			p->status = e ? FAKE_NV_ERR_ARG : FAKE_NV_ERR_OBJECT;
			break;
		}
		if (fault && fault->kind == F_ZERO) {
			fs->h_sub_device = 0;
		} else if (!e->b) {
			p->status = FAKE_NV_ERR_OBJECT; /* no subdevice allocated under it */
			break;
		} else {
			fs->h_sub_device = e->b;
			t.found_sub = e->b;
			t.have_found = true;
		}
		p->status = GS_NV_OK;
		break;
	}
	}
	return answer(0);
}

static bool daemon_cmd(uint32_t cmd)
{
	return cmd == GS_NV2080_CTRL_CMD_BUS_GET_PCI_INFO ||
	       cmd == GS_NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE || cmd == FIX_OTHER_CMD;
}

EXPORT int ioctl(int fd, unsigned long request, ...)
{
	int (*real)(int, unsigned long, ...);
	va_list ap;
	void *argp;

	va_start(ap, request);
	argp = va_arg(ap, void *);
	va_end(ap);

	if (request == GS_NV_ESC_RM_CONTROL) {
		struct gs_nvos54 *p = argp;

		if (query_size(p->cmd))
			return query(fd, p);
		if (!daemon_cmd(p->cmd)) { /* nothing but the daemon and the four queries */
			t.queries++;
			trace("cmd:%x", p->cmd);
			broke("cmd");
			p->status = FAKE_NV_ERR_ARG;
			return answer(0);
		}
		t.have_call = true;
		t.have_gpu_id = false;
		t.have_found = false;
		t.fd = fd;
		t.client = p->h_client;
		t.object = p->h_object;
		if (p->h_object & FAKE_FAIL_EIO) {
			errno = EIO;
			return -1;
		}
		p->status = p->h_object & FAKE_STATUS_MASK;
		errno = FAKE_SUCCESS_ERRNO;
		return 0;
	}
	real = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
	if (!real) {
		errno = ENOSYS;
		return -1;
	}
	return real(fd, request, argp);
}
