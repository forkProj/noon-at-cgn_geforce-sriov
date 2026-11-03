/* SPDX-License-Identifier: MIT */
/*
 * geforce-sriov identity hook: the driver interface it touches and the inputs it reads.
 *
 * Shared by hook/gs_hook.c (the LD_PRELOAD library for nvidia-vgpud) and its behavioural test
 * fixtures under hook/tests/. See hook/README.md.
 *
 * ABI provenance. Every value below is from NVIDIA's MIT-licensed open-gpu-kernel-modules
 * headers, compared at tags 580.159.03, 580.178.04 and 595.91.07:
 *   src/common/sdk/nvidia/inc/nvos.h                      NVOS54_PARAMETERS (32 bytes)
 *   src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080bus.h  NV2080_CTRL_CMD_BUS_GET_PCI_INFO
 *   src/common/sdk/nvidia/inc/ctrl/ctrl0080/ctrl0080gpu.h  NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE,
 *                                                          NV0080_CTRL_CMD_GPU_GET_NUM_SUBDEVICES,
 *                                                          NV0080_CTRL_CMD_GPU_FIND_SUBDEVICE_HANDLE
 *   src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080gpu.h  NV2080_CTRL_CMD_GPU_GET_ID
 *   src/common/sdk/nvidia/inc/ctrl/ctrl0000/ctrl0000gpu.h  NV0000_CTRL_CMD_GPU_GET_PCI_INFO
 *   src/nvidia/arch/nvalloc/unix/include/nv_escape.h       NV_ESC_RM_CONTROL 0x2A
 *   kernel-open/common/inc/nv-ioctl-numbers.h              NV_IOCTL_MAGIC 'F'
 * nvos.h, ctrl2080bus.h, ctrl0080gpu.h, ctrl2080gpu.h and ctrl0000gpu.h are byte-identical at
 * 580.159.03 and 580.178.04; at 595.91.07 ctrl0080gpu.h is byte-identical, and nvos.h,
 * ctrl2080bus.h, ctrl2080gpu.h and ctrl0000gpu.h differ in bytes but declare the same
 * structures and command values used here.
 *
 * The four attribution queries (GET_ID, root GET_PCI_INFO, GET_NUM_SUBDEVICES,
 * FIND_SUBDEVICE_HANDLE) are exported in the open RM sources of 580.159.03 and 595.91.07 with
 * flags that include NON_PRIVILEGED and API_LOCK_READONLY and not ROUTE_TO_PHYSICAL
 * (g_subdevice_nvoc.c, g_client_resource_nvoc.c, g_device_nvoc.c). That the proprietary vGPU
 * host builds listed below export them identically is NOT verified.
 */
#ifndef GS_HOOK_H
#define GS_HOOK_H

#include <stddef.h>
#include <stdint.h>

/* The only process the hook acts in, compared with /proc/self/comm. */
#define GS_HOOK_TARGET_PROCESS "nvidia-vgpud"

/* Shared key=value config (format owned by the CLI, see README "Config"). */
#define GS_HOOK_DEFAULT_CONFIG "/etc/geforce-sriov/config"
#define GS_HOOK_ENV_CONFIG "GEFORCE_SRIOV_CONFIG"
#define GS_HOOK_CONFIG_MAX_BYTES 65536

/* Running driver version and the PCI tree the bdfs key is resolved against. */
#define GS_HOOK_DRIVER_VERSION_PATH "/proc/driver/nvidia/version"
#define GS_HOOK_SYSFS_ROOT "/sys"

/*
 * Test-harness-only overrides. They are honoured only by a library compiled with
 * -DGS_HOOK_TEST_ENV (hook/tests); the shipped hook/libgeforce_sriov.so ignores them.
 */
#define GS_HOOK_TEST_ENV_DRIVER_VERSION "GEFORCE_SRIOV_TEST_DRIVER_VERSION_FILE"
#define GS_HOOK_TEST_ENV_SYSFS_ROOT "GEFORCE_SRIOV_TEST_SYSFS_ROOT"

/*
 * Driver builds the hook may act on, matched exactly against the version token of the
 * "NVRM version:" line. Each is a vGPU host package release:
 *   580.159.01  an identity hook with the same commands and sizes has presented a GA102
 *               board as its vGPU target on this build; open headers of tag 580.159.03.
 *   580.178.05  static only: open headers of tag 580.178.04 are byte-identical to 580.159.03;
 *               no daemon run.
 *   595.91.04   an identity hook with the same commands and sizes has presented a GA102
 *               board on this build; open headers of tag 595.91.07.
 * This C hook itself has not run against any real driver.
 */
static const char *const gs_hook_driver_versions[] = {
	"580.159.01",
	"580.178.05",
	"595.91.04",
};
#define GS_HOOK_DRIVER_VERSION_COUNT \
	(sizeof(gs_hook_driver_versions) / sizeof(gs_hook_driver_versions[0]))

#define GS_NV_OK 0u
#define GS_NVOS54_FLAGS_FINN_SERIALIZED 0x00000004u
#define GS_NV2080_CTRL_CMD_BUS_GET_PCI_INFO 0x20801801u
#define GS_NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE 0x00800289u
#define GS_NV0080_CTRL_GPU_VIRTUALIZATION_MODE_HOST 0x00000003u

/* Read-only queries the hook sends to attribute a reply to a GPU (see hook/README.md). */
#define GS_NV2080_CTRL_CMD_GPU_GET_ID 0x20800142u
#define GS_NV0000_CTRL_CMD_GPU_GET_PCI_INFO 0x0000021bu
#define GS_NV0080_CTRL_CMD_GPU_GET_NUM_SUBDEVICES 0x00800280u
#define GS_NV0080_CTRL_CMD_GPU_FIND_SUBDEVICE_HANDLE 0x00800293u

/* NVOS54_PARAMETERS: the argument of NV_ESC_RM_CONTROL. */
struct gs_nvos54 {
	uint32_t h_client;
	uint32_t h_object;
	uint32_t cmd;
	uint32_t flags;
	uint64_t params; /* NvP64, 8-byte aligned */
	uint32_t params_size;
	uint32_t status;
};

/* NV2080_CTRL_BUS_GET_PCI_INFO_PARAMS. Each ID word is (device << 16) | vendor. */
struct gs_pci_info {
	uint32_t pci_device_id;
	uint32_t pci_sub_system_id;
	uint32_t pci_revision_id;
	uint32_t pci_ext_device_id;
};

/* NV0080_CTRL_GPU_GET_VIRTUALIZATION_MODE_PARAMS (NvU32 + NvBool, padded to 8). */
struct gs_virt_mode {
	uint32_t virtualization_mode;
	uint8_t is_grid_build;
};

/* NV2080_CTRL_GPU_GET_ID_PARAMS (ctrl2080gpu.h), sent on a subdevice handle. */
struct gs_gpu_id {
	uint32_t gpu_id;
};

/*
 * NV0000_CTRL_GPU_GET_PCI_INFO_PARAMS (ctrl0000gpu.h), sent on the root object (hObject =
 * hClient). gpuId in; the GPU's full 32-bit PCI domain, bus and device (slot) number out.
 * There is no function number: the driver registers PFs only, never VFs.
 */
struct gs_gpu_pci_loc {
	uint32_t gpu_id;
	uint32_t domain;
	uint16_t bus;
	uint16_t slot;
};

/* NV0080_CTRL_GPU_GET_NUM_SUBDEVICES_PARAMS (ctrl0080gpu.h), sent on a device handle. */
struct gs_num_subdevices {
	uint32_t num_subdevices;
};

/*
 * NV0080_CTRL_GPU_FIND_SUBDEVICE_HANDLE_PARAM (ctrl0080gpu.h), sent on a device handle: the
 * handle of an existing subdevice of that instance under it (a lookup, nothing is allocated).
 * NvHandle is NvU32.
 */
struct gs_find_subdevice {
	uint32_t sub_device_inst; /* in */
	uint32_t h_sub_device;    /* out */
};

/* _IOWR(NV_IOCTL_MAGIC 'F', NV_ESC_RM_CONTROL 0x2A, NVOS54_PARAMETERS) */
#define GS_NV_ESC_RM_CONTROL                                                     \
	((3UL << 30) | ((unsigned long)sizeof(struct gs_nvos54) << 16) |         \
	 ((unsigned long)'F' << 8) | 0x2aUL)

_Static_assert(sizeof(struct gs_nvos54) == 32, "NVOS54_PARAMETERS is 32 bytes");
_Static_assert(offsetof(struct gs_nvos54, cmd) == 8, "NVOS54 cmd at +8");
_Static_assert(offsetof(struct gs_nvos54, flags) == 12, "NVOS54 flags at +12");
_Static_assert(offsetof(struct gs_nvos54, params) == 16, "NVOS54 params at +16");
_Static_assert(offsetof(struct gs_nvos54, params_size) == 24, "NVOS54 paramsSize at +24");
_Static_assert(offsetof(struct gs_nvos54, status) == 28, "NVOS54 status at +28");
_Static_assert(sizeof(struct gs_pci_info) == 16, "GET_PCI_INFO params are 16 bytes");
_Static_assert(sizeof(struct gs_virt_mode) == 8, "GET_VIRTUALIZATION_MODE params are 8 bytes");
_Static_assert(sizeof(struct gs_gpu_id) == 4, "GPU_GET_ID params are 4 bytes");
_Static_assert(sizeof(struct gs_gpu_pci_loc) == 12, "root GPU_GET_PCI_INFO params are 12 bytes");
_Static_assert(offsetof(struct gs_gpu_pci_loc, domain) == 4, "GPU_GET_PCI_INFO domain at +4");
_Static_assert(offsetof(struct gs_gpu_pci_loc, bus) == 8, "GPU_GET_PCI_INFO bus at +8");
_Static_assert(offsetof(struct gs_gpu_pci_loc, slot) == 10, "GPU_GET_PCI_INFO slot at +10");
_Static_assert(sizeof(struct gs_num_subdevices) == 4, "GET_NUM_SUBDEVICES params are 4 bytes");
_Static_assert(sizeof(struct gs_find_subdevice) == 8, "FIND_SUBDEVICE_HANDLE params are 8 bytes");
_Static_assert(offsetof(struct gs_find_subdevice, h_sub_device) == 4,
	       "FIND_SUBDEVICE_HANDLE hSubDevice at +4");
_Static_assert(GS_NV_ESC_RM_CONTROL == 0xc020462aUL, "NV_ESC_RM_CONTROL request");

#endif /* GS_HOOK_H */
