# Identity hook (`libgeforce_sriov.so`)

An `LD_PRELOAD` library for NVIDIA's `nvidia-vgpud`. The stock `vgpuConfig.xml` lists no GeForce
device ID, so the daemon refuses a GeForce card ("GPU not supported by vGPU"). The hook presents
each selected card to the daemon under the vGPU target ID from the shared device table
(`include/gs_devices.h`). NVIDIA does not support this configuration.

**Status: measured on one setup.** This library ran inside `nvidia-vgpud` through the package's
systemd drop-in on host driver 580.159.01, kernel `7.0.14-16-pve`, with two memory-modded
RTX 3080 20GB cards (`10de:2206`, subsystem `10de:146d`). It presented both selected cards as
`10de:2230` with 23 vGPU types each, and `nvidia-vgpu-mgr` ran without it. With only one of the
two cards selected, the hook logged the other as unselected and left it unchanged, without the
device-ID fallback. It is also tested offline against a stand-in daemon and a fake driver (see
[Build and tests](#build-and-tests)). No other driver build or board has been run; a table entry
with `experimental = 0` (`2206`) does not mean any other `2206` board has been validated. Every
other entry is experimental.

## What it changes

The hook looks only at `NV_ESC_RM_CONTROL` (`ioctl` request `0xc020462a`). It acts after the
driver has answered, and only when the `ioctl` returned `>= 0`, the RM status is `NV_OK`, the
params pointer is set, the params size is exactly the header size, and the call is not
FINN-serialized (`NVOS54_FLAGS_FINN_SERIALIZED`):

| Command | Size | Action |
|---|---|---|
| `NV2080_CTRL_CMD_BUS_GET_PCI_INFO` `0x20801801` | 16 | When the driver attributes the reply to a selected card (see [Attribution](#attribution)) and the device word is that card's `10de:<device>`, the hook writes `10de:<target>`. When the table gives the target a fixed subsystem (`target_subvendor`/`target_subdevice` both non-zero), the subsystem word is set to it. Otherwise the board's own subsystem is kept, because the target's `<pgpu>` entry uses the `subsystemId="0x0"` wildcard. Revision and `pciExtDeviceId` are left unchanged. |
| `NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE` `0x00800289` | 8 | Changed only with `identity_force_host=on`, and only when the driver attributes the reply to a selected card: `virtualizationMode` is set to HOST (3) and `isGridBuild` is left unchanged. A reply from any other GPU, or one the driver cannot attribute, is left unchanged. |

Everything else reaches the driver and comes back unchanged. That includes other devices, other
sizes, failed calls and every other `ioctl`. The caller always gets the driver's return value
and `errno`.

The hook does **not** touch `SET_TYPES_INFO`, SR-IOV caps, type info, licensing, or
`nvidia-vgpu-mgr`. It does not redirect or edit `vgpuConfig.xml`. vGPU types and profile memory
are the target's own entries from the stock file. The hook neither changes nor filters them, so
a profile can be larger than the card's memory.

## Gates

The library makes its decision once per process, on its first call or at load. Any failure
leaves it inactive, and every `ioctl` then passes through:

1. **Process.** `/proc/self/comm` must be `nvidia-vgpud`. In any other process the hook is
   silent and inactive.
2. **Config.** The config is `/etc/geforce-sriov/config`, or the path in `GEFORCE_SRIOV_CONFIG`.
   - If the file is absent, the hook stays inactive.
   - The hook also stays inactive if the file is not a regular file, is writable by group or
     others, is owned by anyone other than root or the process's user, is over 65536 bytes,
     or is invalid.
   - The config must contain `identity=on`.
3. **Driver build.** The version on the `NVRM version:` line of `/proc/driver/nvidia/version`
   must exactly match one of the following builds (`gs_hook_driver_versions[]`):

   | Build | Status |
   |---|---|
   | `580.159.01` | measured: the daemon accepted both selected GA102 boards as `10de:2230` and reported 23 vGPU types each |
   | `580.178.05` | static only. Open headers of tag 580.178.04 (no 580.178.05 tag exists) are byte-identical to 580.159.03 for `nvos.h`, `ctrl2080bus.h`, `ctrl0080gpu.h`, `nv_escape.h` and `nv-ioctl-numbers.h`. No daemon has run under a hook on this build |
   | `595.91.04` | static only. No 595.91.04 source tag exists; the headers used are 595.91.07. No daemon has run under this hook on this build |

   Only 580.159.01 has run with this hook.
4. **Cards.** `bdfs` must be present. For each listed PF `<bdf>` (function 0), the hook reads
   `/sys/bus/pci/devices/<bdf>/{vendor,device,class}`. A card is mapped only when all of these
   hold:
   - the vendor is `10de` and there is no `physfn` link (it is not a VF);
   - the PCI base class is `0x03` (display);
   - the device ID is in the table;
   - no other NVIDIA display-class physical function exists in the same domain, bus and slot
     (functions `.1`–`.7`). VFs (`physfn`) and the audio function (class `0x04`) do not count.
     A second display function makes the slot ambiguous and the card is skipped, because the
     driver names a GPU by domain, bus and slot only. An unreadable function in the slot also
     skips the card.

   Cards that fail these checks are skipped, and at least one card must remain. Identical
   cards may be installed and selected in any combination: the same device ID on an
   unselected GPU does not refuse a card.

### Attribution

`GET_PCI_INFO` and `GET_VIRTUALIZATION_MODE` carry no bus address. For each reply it may
change, the hook therefore asks the driver which GPU the daemon's handle belongs to. It sends
standard read-only RM controls on the daemon's own fd and `hClient`, through the next `ioctl`
(never back into itself), with `flags = 0`, the exact params size and a non-zero preset status:

| Reply | Queries (in order) | Sent on |
|---|---|---|
| `GET_PCI_INFO`, device word `10de:<device>` of a mapped card | `NV2080_CTRL_CMD_GPU_GET_ID` `0x20800142` (4 bytes) → `NV0000_CTRL_CMD_GPU_GET_PCI_INFO` `0x21b` (12 bytes) | the reply's subdevice handle, then the root object (`hObject = hClient`) |
| `GET_VIRTUALIZATION_MODE`, `identity_force_host=on`, mode not already HOST | `NV0080_CTRL_CMD_GPU_GET_NUM_SUBDEVICES` `0x800280` (4 bytes, must be 1) → `NV0080_CTRL_CMD_GPU_FIND_SUBDEVICE_HANDLE` `0x800293` (8 bytes, instance 0, an existing subdevice, nothing allocated) → `GET_ID` → `GET_PCI_INFO` | the reply's device handle, then the subdevice found, then the root object |

A query succeeds only if the `ioctl` returns 0 and the status is `NV_OK`. The answer must also
repeat the `gpuId` that was asked for, with `bus <= 0xff` and `slot <= 0x1f`. The answer's
full 32-bit domain, bus and slot are compared exactly with each mapped card. Domains above
`ffff` are supported, as behind Intel VMD; the config accepts them as the kernel prints them, for
example `10000:81:00.0`. No other reply sends a query: other devices, other commands, failed
calls, wrong sizes, FINN-serialized calls and inactive processes send none.

- **Attributed to a mapped card:** the `GET_PCI_INFO` reply is rewritten only if its device
  word is that card's own `10de:<device>`. A different word is left unchanged and logged once.
  `GET_VIRTUALIZATION_MODE` is forced to HOST.
- **Attributed to any other GPU:** left unchanged. This includes an unselected identical card,
  the boot display, a selected card that was skipped, and a GPU in another domain or behind
  another switch port.
- **Not attributable** (any query fails or answers out of range):
  - `GET_VIRTUALIZATION_MODE` is left unchanged. There is no fallback for HOST.
  - `GET_PCI_INFO` falls back to the device ID only if two scans both find that every NVIDIA
    physical function with this device ID is a mapped card. The first scan runs at startup;
    the second runs again for each fallback, so it catches cards that were hot-plugged or
    removed. The second scan also checks that each such card still reads as `10de:<device>`.
  - If either scan finds another GPU with that ID, a missing or changed card, or an
    unreadable entry, the reply is left unchanged and the reason is logged once.

The hook keeps no cache of handles or answers. Handles can be freed and reused for another
GPU, so the hook asks again for every reply. That costs 2 extra controls per matching
`GET_PCI_INFO` reply and 4 per non-HOST `GET_VIRTUALIZATION_MODE` reply. Shared state is limited
to the card table built at startup and log-once flags, so concurrent daemon threads need no
lock.

The card table is built from sysfs once, when the daemon starts. After a PCI rescan, a hot-plug
or bus renumbering, restart `nvidia-vgpud`.

**Scope.** These four controls and their sizes come from NVIDIA's open kernel module sources (see
[Provenance](#provenance-and-credits)). On 580.159.01 the hook ran as described in the status
above. Not checked:
- whether the other proprietary vGPU host builds in the table above export them the same way;
- which handles `nvidia-vgpud` uses for these replies, and in particular whether a subdevice
  always exists under the device when it asks for the virtualization mode. If none exists,
  `FIND_SUBDEVICE_HANDLE` fails and HOST is not forced.

## Config

The config uses the same file and format as the CLI, which owns it:

- Lines are separated by `\n`.
- Empty lines, and lines whose first byte is `#`, are ignored.
- Every other line is `key=value`: the key matches `[a-z0-9_]+` and the value is non-empty.
- No whitespace or `\r` may appear anywhere on a line, and a key may not repeat.
- The hook ignores keys it does not use.

Any violation makes the whole file invalid, and the hook stays inactive.

| Key | Default | Meaning to the hook |
|---|---|---|
| `identity` | `off` | `on` enables the rewrite |
| `identity_force_host` | `off` | `on` forces the virtualization-mode reply to HOST, only for a reply the driver attributes to a mapped card (see [Attribution](#attribution)). The measured configuration forces HOST; whether it is needed has not been determined |
| `bdfs` | none | 1–8 distinct canonical PFs `dddd:bb:ss.0` (domain 4–8 hex digits, as the kernel prints it), comma-separated; parsed by `gs_parse_pf_bdf_list()` |

## Logs

Log lines go to syslog (`LOG_DAEMON`) and are prefixed `geforce-sriov-hook:`. To read them:
`journalctl -t nvidia-vgpud`. When the hook becomes active, it logs:

- each card mapping (`card <bdf>: ... presented as ...`), with the subsystem decision;
- the line `No vGPU target on the same die; support is experimental.` for every cross-die card;
- why a card was skipped (`card <bdf>: ... skipped`, including `ambiguous slot`);
- `card <bdf>: no device-ID fallback: <gpu> has device ID <id> but is unselected or skipped`
  when another GPU has its device ID, or `card <bdf>: cannot scan the PCI devices; no device-ID
  fallback` when the scan failed;
- the first rewrite and the first HOST force for each card (`GET_PCI_INFO: card <bdf> ...`,
  `GET_VIRTUALIZATION_MODE: card <bdf> reply forced to HOST (3)`).

Each of the following is logged once per process:
- size mismatches and FINN-serialized calls;
- device words of no card;
- `reply from GPU <dddd:bb:ss>, not a mapped card`;
- `has device word ... unchanged`;
- `cannot attribute the reply to a GPU (query <cmd>: <status | ioctl errno | reason>)`, with
  its outcome: `unchanged`, `by device ID`, or `device-ID fallback refused: <reason>`.

## Install

The CLI installs the library as `$(PREFIX)/lib/geforce-sriov/libgeforce_sriov.so` and adds a
drop-in for `nvidia-vgpud.service`:

```ini
[Unit]
After=geforce-sriov-prepare.service
AssertPathExists=!/run/geforce-sriov/deferred

[Service]
Environment=LD_PRELOAD=/usr/lib/geforce-sriov/libgeforce_sriov.so
```

The assertion keeps `nvidia-vgpud` from starting while the boot unit has deferred preparation for a
boot display GPU (see [The boot display GPU](../docs/boot-display.md)). The hook ignores the
`boot_display_release` key.

Do not preload it system-wide or into `nvidia-vgpu-mgr`. Even if you do, it stays inactive in
any process other than `nvidia-vgpud`.

## Build and tests

```sh
make -C hook          # hook/libgeforce_sriov.so  (needs a C11 compiler, libc, libdl)
make -C hook check    # behavioural tests, no GPU/driver/root needed
```

`check` builds the following into `hook/build/`:

- a test build of the same `gs_hook.c` with `-DGS_HOOK_TEST_ENV`. This is the only build that
  honours `GEFORCE_SRIOV_TEST_DRIVER_VERSION_FILE` and `GEFORCE_SRIOV_TEST_SYSFS_ROOT`, and it
  logs to stderr instead of syslog;
- a fake driver, `tests/fake_driver.c`;
- a stand-in daemon, `tests/vgpud.c`, built as `nvidia-vgpud` and copied as `nvidia-vgpu-mgr`.

The runner, `tests/run_hook_tests.c`, then runs the stand-in daemon under
`LD_PRELOAD="<hook> <fake driver>"`. Each scenario uses a temporary config, version file and
sysfs tree, plus an object table for the fake driver: one subdevice handle, device handle,
gpuId and address per registered GPU, with injected faults. The fake driver answers the four
queries from that table. It also checks each query and reports any rule the query breaks:
- fd and `hClient` equal to the daemon's call;
- the right object: the daemon's handle, the subdevice found, or `hClient` for the root query;
- an allowed command, exact size, `flags = 0` and a non-zero preset status;
- `subDeviceInst = 0`;
- the `gpuId` that `GET_ID` returned.

The runner compares every reply, return value, `errno` and status with the expected one. It also
compares the exact query sequence of each operation, with the GPU's own handles and the
current client.

The cards come from the device table: one target with a wildcard subsystem and one with a fixed
subsystem. The scenarios cover:

- **Topologies:**
  - an identical unselected boot GPU, on the lower or the higher bus;
  - mixed device IDs with selected subsets;
  - three identical cards with two or one of them selected;
  - the same bus in domains `0000`, `0001` and `10000`, with `0001` or `10000` selected;
  - three identical cards behind PCIe switch downstream ports;
  - identical cards in adjacent slots `ff:1e` and `ff:1f` of the highest bus;
  - the audio function and VFs in a card's slot;
  - an ambiguous slot, refused and as the only card;
  - a selected card that the driver does not have.
  
  In each, only the selected card is rewritten and forced.
- **Query failures:**
  - each of the four queries failing with an RM status or `EIO`;
  - out-of-range bus or slot, or a changed `gpuId`;
  - 0 or 2 subdevices, or a null subdevice handle.
  
  Each leaves the reply unchanged and keeps the daemon's return value, status and `errno`.
- **Fallback:**
  - rewrites `GET_PCI_INFO` only, never HOST;
  - works for two selected identical cards;
  - is off when a same-ID card was refused, or when sysfs cannot be read;
  - stops when an identical twin is hot-plugged, an unreadable entry appears or the selected
    card disappears, and resumes once a twin or missing card is undone;
  - is never used for a reply the driver attributes to an unselected GPU.
- **Attribution details:**
  - a device-ID mismatch is left unchanged;
  - handles reassigned to another GPU mid-run, which proves nothing is cached;
  - a client change and a new fd mid-run;
  - 8 threads × 500 calls on a selected card and an unselected identical card at once.
- **Unchanged replies, no queries:** a 12-byte reply, a failed status, `EIO` (with `errno`
  preserved), a FINN-serialized call, another vendor, a device word that is already a target,
  a GPU not in the table, mode already HOST, another command, and a non-NVIDIA `ioctl`.
- **Inactive configs:**
  - config absent, identity off or missing, or no `bdfs`;
  - a card not in the table, a VF, or a non-display function;
  - 15 malformed configs, a group-writable file, or a directory.
- **Inactive drivers:** an unlisted build, a four-part version, a garbage version file, or a
  missing version file.
- **Process gate:** the manager process stays silent and unchanged, and sends no queries.

There are 76 scenarios in all, including the no-hook baseline.

## Provenance and credits

The structures and command values in `include/gs_hook.h` come from NVIDIA's MIT-licensed
[open-gpu-kernel-modules](https://github.com/NVIDIA/open-gpu-kernel-modules) headers, compared at
tags 580.159.03, 580.178.04 and 595.91.07:

- `nvos.h`, for `NVOS54_PARAMETERS` (32 bytes; params at +16);
- `ctrl/ctrl2080/ctrl2080bus.h` (`BUS_GET_PCI_INFO`);
- `ctrl/ctrl0080/ctrl0080gpu.h` (`GET_VIRTUALIZATION_MODE`, `GET_NUM_SUBDEVICES`,
  `FIND_SUBDEVICE_HANDLE`);
- `ctrl/ctrl2080/ctrl2080gpu.h` (`GPU_GET_ID`);
- `ctrl/ctrl0000/ctrl0000gpu.h` (`GPU_GET_PCI_INFO`: `gpuId`, 32-bit `domain`, 16-bit `bus`
  and `slot`);
- `nv_escape.h` (`NV_ESC_RM_CONTROL` `0x2A`);
- `nv-ioctl-numbers.h` (`'F'`).

The sizes and offsets are pinned with `_Static_assert`. In the open RM sources, the four
attribution controls have export flags that include `NON_PRIVILEGED` and `API_LOCK_READONLY`,
and not `ROUTE_TO_PHYSICAL`. The driver checks `paramsSize` exactly (`control.c`). It refuses
to register two GPUs with the same `gpuId`, and it never registers a VF.

The design is adapted from the vgpu-toolkit hook (Rust), itself trimmed from a fork of
[vgpu_unlock-rs](https://github.com/mbilker/vgpu_unlock-rs) (MIT, © 2021 Jonathan Johansson,
© 2021 Matt Bilker), which builds on DualCoder's
[vgpu_unlock](https://github.com/DualCoder/vgpu_unlock). Their notices are kept in `LICENSE`.

Differences from the vgpu-toolkit hook:

- The C hook has one build for all listed drivers, because every branch has the same values.
- It reads the version from `/proc/driver/nvidia/version`.
- It scopes the rewrite with the CLI's `bdfs` allowlist and attributes each reply to a GPU by
  bus address through the driver, instead of matching the device ID alone.
- It takes its device table from `gs_devices.h` instead of explicit config pairs.
- It writes no result file.
- It does not record `SET_INFO`.
