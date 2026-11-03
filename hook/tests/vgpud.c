// SPDX-License-Identifier: MIT
/*
 * Stand-in for nvidia-vgpud. Built under that exact name (and copied as nvidia-vgpu-mgr) so the
 * hook's /proc/self/comm gate sees it. Issues the RM controls named on its command line to the
 * fake driver and prints one line per operation with what came back (fixture.h formats),
 * including the attribution queries the fake driver saw the hook send for that operation:
 *
 *   pci DEVWORD SUBWORD SIZE HOBJECT FLAGS   NV2080_CTRL_CMD_BUS_GET_PCI_INFO
 *   virt MODE SIZE HOBJECT                   NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE
 *   other DEVWORD                            another control with a PCI-info-shaped buffer
 *   tty                                      TCGETS on /dev/null (not an NVIDIA request)
 *   threads DEVWORD SUBWORD THREADS CALLS HEVEN HODD
 *                                            concurrent GET_PCI_INFO calls; even-numbered
 *                                            threads on HEVEN, odd-numbered ones on HODD
 *   table PATH                               the fake driver's object table becomes PATH
 *   client HCLIENT                           later controls use this hClient
 *   dupfd                                    later controls use a new fd for the same file
 *   rename FROM TO                           rename(2): a PCI function appears in or leaves
 *                                            the fake sysfs tree while the daemon runs
 *
 * Numbers are parsed with strtoul base 0.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "fixture.h"

static int dev_fd;
static uint32_t client = FIX_CLIENT;
static fake_take_queries_fn take_queries;

static uint32_t num(const char *s)
{
	char *end;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, 0);
	if (errno || *end || v > UINT32_MAX) {
		fprintf(stderr, "vgpud: bad number %s\n", s);
		exit(2);
	}
	return (uint32_t)v;
}

static int rm_control(uint32_t cmd, uint32_t h_object, uint32_t flags, void *params, uint32_t size,
		      uint32_t *status, int *err)
{
	struct gs_nvos54 p = {
		.h_client = client,
		.h_object = h_object,
		.cmd = cmd,
		.flags = flags,
		.params = (uint64_t)(uintptr_t)params,
		.params_size = size,
		.status = FIX_STATUS_UNSET,
	};
	int rc;

	errno = 0;
	rc = ioctl(dev_fd, GS_NV_ESC_RM_CONTROL, &p);
	*err = errno;
	*status = p.status;
	return rc;
}

struct thread_arg {
	uint32_t dev, sub, calls, h_object;
	struct gs_pci_info first;
	unsigned int mismatches, queries, bad;
};

static void *thread_main(void *opaque)
{
	struct thread_arg *a = opaque;

	for (uint32_t i = 0; i < a->calls; i++) {
		struct gs_pci_info info = { a->dev, a->sub, FIX_REVISION, a->dev };
		char q[FAKE_TRACE_MAX];
		unsigned int nq, nb;
		uint32_t status;
		int err, rc;

		rc = rm_control(GS_NV2080_CTRL_CMD_BUS_GET_PCI_INFO, a->h_object, 0, &info,
				sizeof(info), &status, &err);
		take_queries(q, sizeof(q), &nq, &nb);
		a->queries += nq;
		a->bad += nb;
		if (i == 0)
			a->first = info;
		if (rc != 0 || err != FAKE_SUCCESS_ERRNO || status != 0 ||
		    memcmp(&info, &a->first, sizeof(info)) != 0)
			a->mismatches++;
	}
	return NULL;
}

static void op_threads(uint32_t dev, uint32_t sub, uint32_t n, uint32_t calls, uint32_t h_even,
		       uint32_t h_odd)
{
	struct thread_arg args[64];
	pthread_t tids[64];
	unsigned int mismatches = 0, queries = 0, bad = 0;

	if (n < 2 || n > 64) {
		fprintf(stderr, "vgpud: 2..64 threads\n");
		exit(2);
	}
	for (uint32_t i = 0; i < n; i++) {
		args[i] = (struct thread_arg){ .dev = dev, .sub = sub, .calls = calls,
					       .h_object = i % 2 ? h_odd : h_even };
		if (pthread_create(&tids[i], NULL, thread_main, &args[i]) != 0) {
			fprintf(stderr, "vgpud: pthread_create failed\n");
			exit(2);
		}
	}
	for (uint32_t i = 0; i < n; i++) {
		pthread_join(tids[i], NULL);
		mismatches += args[i].mismatches;
		queries += args[i].queries;
		bad += args[i].bad;
		if (memcmp(&args[i].first, &args[i % 2].first, sizeof(args[0].first)) != 0)
			mismatches++;
	}
	printf(FIX_THREADS_LINE, args[0].first.pci_device_id, args[0].first.pci_sub_system_id,
	       args[1].first.pci_device_id, args[1].first.pci_sub_system_id, n * calls, mismatches,
	       queries, bad);
}

int main(int argc, char **argv)
{
	fake_load_table_fn load_table;
	int i = 1;

	take_queries = (fake_take_queries_fn)dlsym(RTLD_DEFAULT, FAKE_SYM_TAKE_QUERIES);
	load_table = (fake_load_table_fn)dlsym(RTLD_DEFAULT, FAKE_SYM_LOAD_TABLE);
	if (!take_queries || !load_table) {
		fprintf(stderr, "vgpud: the fake driver is not preloaded\n");
		return 2;
	}
	dev_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	if (dev_fd < 0) {
		perror("vgpud: open /dev/null");
		return 2;
	}
	while (i < argc) {
		const char *op = argv[i];
		char q[FAKE_TRACE_MAX];
		unsigned int nq, nb;
		uint32_t status;
		int err, rc;

		if (strcmp(op, "pci") == 0 && i + 5 < argc) {
			struct gs_pci_info info = { num(argv[i + 1]), num(argv[i + 2]), FIX_REVISION,
						    num(argv[i + 1]) };

			rc = rm_control(GS_NV2080_CTRL_CMD_BUS_GET_PCI_INFO, num(argv[i + 4]),
					num(argv[i + 5]), &info, num(argv[i + 3]), &status, &err);
			take_queries(q, sizeof(q), &nq, &nb);
			printf(FIX_PCI_LINE, info.pci_device_id, info.pci_sub_system_id,
			       info.pci_revision_id, info.pci_ext_device_id, rc, err, status, q);
			i += 6;
		} else if (strcmp(op, "virt") == 0 && i + 3 < argc) {
			struct gs_virt_mode vm = { .virtualization_mode = num(argv[i + 1]),
						   .is_grid_build = 1 };

			rc = rm_control(GS_NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE, num(argv[i + 3]),
					0, &vm, num(argv[i + 2]), &status, &err);
			take_queries(q, sizeof(q), &nq, &nb);
			printf(FIX_VIRT_LINE, vm.virtualization_mode, vm.is_grid_build, rc, err, status,
			       q);
			i += 4;
		} else if (strcmp(op, "other") == 0 && i + 1 < argc) {
			struct gs_pci_info info = { num(argv[i + 1]), 0x000010deu, FIX_REVISION,
						    num(argv[i + 1]) };

			rc = rm_control(FIX_OTHER_CMD, 0, 0, &info, sizeof(info), &status, &err);
			take_queries(q, sizeof(q), &nq, &nb);
			printf(FIX_OTHER_LINE, info.pci_device_id, info.pci_sub_system_id, rc, q);
			i += 2;
		} else if (strcmp(op, "tty") == 0) {
			struct termios tio;

			errno = 0;
			rc = ioctl(dev_fd, TCGETS, &tio);
			err = errno;
			take_queries(q, sizeof(q), &nq, &nb);
			printf(FIX_TTY_LINE, rc, err, q);
			i += 1;
		} else if (strcmp(op, "threads") == 0 && i + 6 < argc) {
			op_threads(num(argv[i + 1]), num(argv[i + 2]), num(argv[i + 3]),
				   num(argv[i + 4]), num(argv[i + 5]), num(argv[i + 6]));
			i += 7;
		} else if (strcmp(op, "table") == 0 && i + 1 < argc) {
			printf(FIX_TABLE_LINE, load_table(argv[i + 1]));
			i += 2;
		} else if (strcmp(op, "client") == 0 && i + 1 < argc) {
			client = num(argv[i + 1]);
			printf(FIX_CLIENT_LINE, client);
			i += 2;
		} else if (strcmp(op, "dupfd") == 0) {
			int fd = fcntl(dev_fd, F_DUPFD_CLOEXEC, dev_fd + 1);

			if (fd < 0) {
				perror("vgpud: dup");
				return 2;
			}
			printf(FIX_DUPFD_LINE, fd != dev_fd);
			dev_fd = fd; /* the old fd stays open: its number is not reused */
			i += 1;
		} else if (strcmp(op, "rename") == 0 && i + 2 < argc) {
			printf(FIX_RENAME_LINE, rename(argv[i + 1], argv[i + 2]));
			i += 3;
		} else {
			fprintf(stderr, "vgpud: bad operation at %s\n", op);
			return 2;
		}
	}
	close(dev_fd);
	return 0;
}
