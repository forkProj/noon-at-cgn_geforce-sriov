/* SPDX-License-Identifier: MIT */
/*
 * Shared by the hook's behavioural fixtures: fake_driver.c (stand-in for the NVIDIA driver,
 * preloaded after the hook), vgpud.c (stand-in daemon, built under the name nvidia-vgpud) and
 * run_hook_tests.c (the runner). No GPU, driver or device node is involved.
 */
#ifndef GS_HOOK_FIXTURE_H
#define GS_HOOK_FIXTURE_H

#include <stddef.h>
#include <stdint.h>

#include "gs_hook.h"

/*
 * Test convention only, for the daemon's own RM controls: the fake driver answers them from
 * the caller's hObject word. The low 16 bits are the RM status it writes; FAKE_FAIL_EIO fails
 * the ioctl itself. On success it sets errno to FAKE_SUCCESS_ERRNO, so a hook that clobbers
 * errno shows up. Handles of the fake driver's object table have the low 17 bits clear.
 */
#define FAKE_STATUS_MASK 0xffffu
#define FAKE_FAIL_EIO 0x10000u
#define FAKE_SUCCESS_ERRNO 114 /* EALREADY */

/*
 * The hook's attribution queries (GPU_GET_ID, root GPU_GET_PCI_INFO, GET_NUM_SUBDEVICES,
 * FIND_SUBDEVICE_HANDLE) are answered from an object table file named by FAKE_ENV_TABLE:
 *
 *   sub HSUBDEVICE GPUID                 GPU_GET_ID on HSUBDEVICE answers GPUID
 *   dev HDEVICE NUMSUBDEVICES HSUBDEVICE NUM_SUBDEVICES / FIND_SUBDEVICE_HANDLE{0} on HDEVICE
 *                                        (HSUBDEVICE 0: no subdevice, FIND fails)
 *   gpu GPUID DOMAIN BUS SLOT            root GPU_GET_PCI_INFO for GPUID
 *   fault id|pci|num|find KEY KIND VALUE fault on that query; KEY is the hObject, or the gpuId
 *                                        for pci. KIND: status (RM status VALUE), eio (the
 *                                        ioctl fails), count (numSubDevices VALUE), zero
 *                                        (hSubDevice 0, NV_OK), bus / slot (that field VALUE),
 *                                        echo (gpuId changed in the answer)
 *
 * A successful query sets errno to FAKE_QUERY_ERRNO, a different value from the daemon's.
 */
#define FAKE_ENV_TABLE "GEFORCE_SRIOV_TEST_FAKE_TABLE"
#define FAKE_QUERY_ERRNO 75     /* EOVERFLOW */
#define FAKE_NV_ERR_OBJECT 0x57u /* test values, not NVIDIA's: unknown handle */
#define FAKE_NV_ERR_ARG 0x1fu    /* unknown gpuId, wrong size, subDeviceInst != 0 */

/*
 * Exported by fake_driver.so and looked up by vgpud.c with dlsym:
 *   int gs_fake_load_table(const char *path)    replace the object table (0 or -1)
 *   void gs_fake_take_queries(char *buf, size_t len, unsigned int *queries, unsigned int *bad)
 *     the calling thread's queries since its last take, as "id:HOBJ,pci:HCLIENT,..." ("-" for
 *     none), each violated rule appended as a "BAD:<rule>" token; then cleared.
 */
#define FAKE_SYM_LOAD_TABLE "gs_fake_load_table"
#define FAKE_SYM_TAKE_QUERIES "gs_fake_take_queries"
typedef int (*fake_load_table_fn)(const char *path);
typedef void (*fake_take_queries_fn)(char *buf, size_t len, unsigned int *queries,
				     unsigned int *bad);
#define FAKE_TRACE_MAX 512

/* The params the stand-in daemon starts from ("the driver's reply"). */
#define FIX_CLIENT 0xc1d00001u
#define FIX_REVISION 0xa1u
#define FIX_STATUS_UNSET 0xdeadbeefu
#define FIX_OTHER_CMD 0x20800110u /* some other subdevice control */

#define FIX_PCI_LINE "pci dev=%08x sub=%08x rev=%08x ext=%08x rc=%d errno=%d status=%x q=%s\n"
#define FIX_VIRT_LINE "virt mode=%u grid=%u rc=%d errno=%d status=%x q=%s\n"
#define FIX_OTHER_LINE "other dev=%08x sub=%08x rc=%d q=%s\n"
#define FIX_TTY_LINE "tty rc=%d errno=%d q=%s\n"
#define FIX_THREADS_LINE \
	"threads even=%08x/%08x odd=%08x/%08x calls=%u mismatches=%u queries=%u bad=%u\n"
#define FIX_TABLE_LINE "table rc=%d\n"
#define FIX_CLIENT_LINE "client %x\n"
#define FIX_DUPFD_LINE "dupfd moved=%d\n"
#define FIX_RENAME_LINE "rename rc=%d\n"

#endif /* GS_HOOK_FIXTURE_H */
