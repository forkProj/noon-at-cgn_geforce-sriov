# Technical reference

The details behind the [README](../README.md): what each component does exactly, the rules the
CLI enforces, and how PCI layouts other than the measured one are handled. The measured setup and
its scope are listed in [What has been verified](../README.md#what-has-been-verified); this page
marks every other case as untested where it applies.

## How it works, step by step

The short version, with diagrams, the SR-IOV note and the reason for the default of 4 VFs, is in
the README's [How it works](../README.md#how-it-works). In plain words, five things happen.

**1. The consumer GeForce card hides SR-IOV; the kernel module reveals it.** GeForce cards of
these generations contain SR-IOV, the PCIe feature that lets one card appear as several smaller
devices. The card's configuration space leaves it out of the list the kernel reads, so Linux never
sees it. The `geforce_sriov` module is a *livepatch*: a small change to the running kernel, built
by DKMS, that applies **only to the cards you list**. It makes the kernel find SR-IOV at its hidden
position and reports 4 virtual functions by default on Ampere, and the card's own 1 on Ada and
Blackwell (the count is configurable: [configuration.md](configuration.md#configuration-file)).
With the defaults it also reveals two related features, ARI and (on Ampere) MSI-X; see the
[Glossary](glossary.md).

**2. The card is re-read before NVIDIA's driver binds it.** The kernel reads every PCI device
once at boot, before any module loads. So after loading the module, the boot unit removes the
card from the PCI tree and asks the bridge directly above it to rescan **only that card's own
bus**. The kernel then reads the card again through the patched lookup and sets up SR-IOV. While
it does this it keeps the bridge awake and first unbinds the card's own HDMI audio function. All
of this runs early in boot, before udev and NVIDIA's services start.

**3. NVIDIA's own script creates the virtual functions.** `geforce-sriov enable` runs NVIDIA's
`sriov-manage` (the only method NVIDIA supports for this) and checks that all virtual functions
came up bound to NVIDIA's driver.

**4. The identity hook lets NVIDIA's daemon accept the card.** `nvidia-vgpud` only offers vGPU
types for cards in NVIDIA's supported list, which has no GeForce cards. The hook is a tiny library
loaded **only** into `nvidia-vgpud` (through a systemd drop-in). For the cards you selected, it
changes the device ID in the driver's answer so the daemon sees the professional card of the same
chip (an RTX 3080 is presented as an RTX A6000) and registers that card's vGPU types. It does not
touch the XML file, the vGPU manager or licensing, and it is inactive for every other GPU.

**5. A two-byte driver fix keeps guest work from stalling.** With host driver 580.159.01, guest
CUDA work can stall because a routine in NVIDIA's closed driver object switches off the GPU's
scheduler. `geforce-sriov-patch` reads **your own** stock `nv-kernel.o_binary`, checks its SHA-256
and structure, and writes a new file that differs in exactly two bytes. You rebuild NVIDIA's
kernel module from it. Your input file is never modified and no NVIDIA file is distributed here.
Details: [driver-patch.md](driver-patch.md).

The sections below give the exact details of each part.

## Components

| Path | What it is | License |
|---|---|---|
| `geforce-sriov` (`src/`) | CLI: inspection, install, preparation, VF enable, uninstall | MIT |
| `kernel/geforce_sriov.c` | Livepatch module (DKMS `geforce-sriov/0.1.0`) | GPL-2.0-only OR MIT |
| `hook/libgeforce_sriov.so` | `LD_PRELOAD` identity hook for `nvidia-vgpud` only | MIT |
| `driver/geforce-sriov-patch` | Patcher for the user's own `nv-kernel.o_binary` | MIT |
| `systemd/` | `geforce-sriov-prepare.service` and an `nvidia-vgpud` drop-in | MIT |

## The kernel module

What the module does, for the PCI functions listed in its required `bdfs` parameter only
(switch parameters take `0`/`1`; the CLI sets them from the config's `on`/`off` values, and passes
`vfs=N` when the config sets a VF count):

- returns the hidden SR-IOV capability (Ampere, Ada: config offset `0xBCC`) from
  `pci_find_ext_capability()`; Blackwell's natively linked capability at `0x250` is used as is;
- reports TotalVFs/InitialVFs to the kernel as the `vfs` parameter (1–16, default **4**; see the ARI
  item below), never more than the card's hardware TotalVFs. On Ada and Blackwell the hardware
  value, 1, is kept by default; with `experimental_vfs=1` a hardware value of exactly 1 is shown as
  `vfs` (at most 4). These are 16-bit reads by the kernel's SR-IOV code; NVIDIA's driver does not
  use them (next item);
- replaces `pci_read_config_dword()` as well, only when `twin_id=1` or when `experimental_vfs=1`
  lists a Blackwell card. NVIDIA's driver reads TotalVFs, the device ID and the subsystem of
  Blackwell cards with 32-bit config reads. With `experimental_vfs=1`, the InitialVFs/TotalVFs dword
  of a listed Blackwell card (SR-IOV position + `0x0C`, so `0x25C`) returns the same counts as the
  16-bit reads. With `twin_id=1`, dword `0x00` of a listed RTX 5090 returns `10de:2bb5` while the
  card reads its own ID, and dword `0x2C` returns subsystem `10de:204e`
  ([twin_id](devices.md#twin_id-rtx-5090-only)); every in-kernel reader of those two dwords sees
  the twin ID, user space keeps the real one. Every other device, card and offset, the other
  SR-IOV dwords included, gets the hardware value. Ada is not covered: its driver reads these
  values through a BAR0 mirror that no config accessor sees, so on Ada only Linux sees the raised
  count;
- with `ari=1` (default), enables ARI in software: presents the hidden ARI capability (`0xC14`, only
  if its Next Function Number field is non-zero) and turns on ARI Forwarding in the GPU's immediate
  upstream bridge when that bridge supports it. ARI decoding in the GPU hardware does not work for
  now, which is why `vfs` defaults to 4, with or without ARI. The default may change if ARI
  decoding is made to work. The module never turns on ARI Forwarding when the kernel scans every
  slot below a port (`pci=pcie_scan_all` or a platform quirk), because the card's 3-bit function
  decode would then answer device numbers ≥ 1 as alias devices;
- with `msix=1` (default), on Ampere clears config `0x41C` bit 29 so the PF's MSI-X capability is
  linked; it leaves a natively linked MSI-X alone and skips Ada, whose mechanism is unknown. The
  bit is volatile: any PF reset (function-level reset, GPU reset, AER recovery) hides MSI-X again
  and stops PF interrupt delivery until the next cold boot. Use `msix=off` if you need PF resets;
- checks where the VFs would land: every VF must fall on a free function 1–7 of the PF's own slot
  (first VF offset not 0, stride not 0, no function beyond 7, no function another device already
  uses). At load this is checked with the values the card reports then. When the kernel sets up
  SR-IOV during the rescan it tries every VF count from `vfs` down to 1; a bad layout at any
  count, or a NumVFs register that does not read back what the kernel wrote, makes the kernel set
  up no SR-IOV on that card (kernel log: `VF layout for … requested VFs`). After setup the values
  are passed through unchanged and a different layout is only logged
  (`unexpected VF layout after SR-IOV setup`). A passing check does not prove that the card
  provides that many working VFs;
- validates every listed card before patching anything and refuses to load (nothing patched or
  written) when a listed address is missing or not a table card, the SR-IOV header does not match,
  the hardware TotalVFs is 0, or the VF layout check above fails (kernel log:
  `first VF offset … ; refusing`). With `twin_id=1` it also refuses when none of the listed cards
  is an RTX 5090 (on the others `twin_id` has no effect), and when a listed RTX 5090 is behind a
  hotplug-capable port or has a driver bound (`twin_id refused: …`). It never removes, rescans,
  resets or binds a device, and has no device table that could autoload it.

The kernel enumerates PCI devices before any module loads, so the CLI removes and rescans each
selected card after the livepatch is active, holding the upstream bridge awake throughout (a bridge
that runtime-suspends during the rescan loses the card). The kernel's own SR-IOV handling is
described in [PCI Express I/O Virtualization Howto](https://docs.kernel.org/PCI/pci-iov-howto.html)
and its livepatch mechanism in [Livepatch](https://docs.kernel.org/livepatch/livepatch.html). The
step-by-step sequence is in [manual-method.md](manual-method.md).

## Requirements in detail

- x86_64 Linux 6.8 or later with `CONFIG_LIVEPATCH` and `CONFIG_PCI_IOV`, built without Clang
  LTO (`CONFIG_LTO_CLANG`, full or thin: the module refuses to build, because the kernel's SR-IOV
  code may then use inlined copies of the config accessors it replaces), and
  `pci_dev_is_disconnected()` exported in `<linux/pci.h>` (6.9, or a stable backport); kernel
  headers, `dkms`, `systemd`, `pciutils` (`lspci`, `setpci`, used by NVIDIA's `sriov-manage`),
  a C compiler and `make`. `doctor` checks for `dkms`, `systemctl`, `lspci` and `setpci`; a live
  `install --device` refuses without `dkms` and `systemctl`, and `enable` refuses without `lspci`
  and `setpci`.
- IOMMU enabled (for example `intel_iommu=on iommu=pt`).
- NVIDIA vGPU-KVM host driver, proprietary flavour, installed by you, supporting every selected
  card. The identity hook acts only on 580.159.01, 580.178.05 and 595.91.04; only 580.159.01 has
  been measured. Its systemd units `nvidia-vgpud.service` and `nvidia-vgpu-mgr.service` must be
  installed; `doctor` warns when either unit file is missing from systemd's unit directories.
  Blackwell needs the open flavour, which geforce-sriov does not support yet. Ada needs a modified
  proprietary 580.159.01 module run with GSP off, which this package does not build
  ([Ada host stack](ada.md); [NVIDIA driver flavour](devices.md#nvidia-driver-flavour)).
- A selected GPU must be function 0 of its slot, must have a PCI bridge directly above it, must not
  share its PCI bus with another device, must not already present SR-IOV (another livepatch or a
  patched kernel; `prepare` does not stack on it), and must not be bound to a display driver when
  preparation runs. The boot display device is refused unless you grant its console release
  ([The boot display GPU](#the-boot-display-gpu)); it is then prepared by hand after boot, never by
  the boot unit. If `nvidia` is loaded from `modules-load.d`, `/etc/modules` or the initramfs it may
  bind the card first; `doctor` warns when it finds `nvidia` there or in the initramfs-tools/dracut
  configuration (it does not inspect built initramfs images), and `prepare` then refuses.
- Enough bridge window space for BAR0 and the VF BARs of every selected card (see
  [PCI resources](#pci-resources)).
- `/var/lib` and the install prefix must be on the root filesystem or on a volume the initramfs
  activates. The boot unit runs before `systemd-udevd` starts, so a filesystem that needs udev to
  appear would make it wait for a device that is never processed.

## Install details

A live install copies the binaries to `--prefix` (default `/usr/local`), the hook to
`$PREFIX/lib/geforce-sriov/`, the module source to `/usr/src/geforce-sriov-0.1.0/`, and runs
`dkms add/build/install`; it installs `geforce-sriov-prepare.service` and the `nvidia-vgpud`
drop-in, writes `/etc/geforce-sriov/config` with `bdfs=`, `identity=on` and
`identity_force_host=on` (and `vfs=N` with `--vfs N`), and **enables** the unit for the next boot.
It never starts the unit or touches a device. It refuses a `vfs` above a selected card's hardware
default, and prints a `warning:` for every cross-die or experimental card, for a `vfs` above 4, for
Ada/Blackwell cards with `experimental_vfs=on` and for RTX 5090 cards with `twin_id=on`. Every
installed file is recorded with its SHA-256 in `/var/lib/geforce-sriov/installed-files`; running
`install` again leaves a file that is already in place with the same content, mode and owner alone
and prints `unchanged <path>`.

Install refuses (exit 3) an existing `/etc/geforce-sriov/config` that it did not write (edit that
file yourself or remove it; see `config.example`) and a `DESTDIR` with a symlink in its path; it
stops (exit 1) rather than overwrite any other existing file it does not own with different
content. `--prefix` may contain only letters, digits and `/._+-`. A guard left by an unfinished
preparation makes install print a warning. Without `--device`, nothing is selected and the unit is
not enabled; the printed next steps say how to select GPUs.

To stage into a directory instead (nothing outside it is changed; the steps to run on the target are
printed, starting with `geforce-sriov doctor` there):

```sh
DESTDIR=/tmp/stage ./geforce-sriov install --prefix /usr --device 0000:03:00.0
```

Staging records `--device` addresses unchecked and checks no devices; `--config` and
`--sysfs-root` are rejected (exit 2).

## Commands

| Command | Effect |
|---|---|
| `devices` | Print the device table. |
| `doctor [--device BDF]... [--sysfs-root PATH] [--config FILE]` | Preflight checks and what `prepare` would do. Read-only. Exit 3 when `prepare` would refuse. |
| `status [--device BDF]... [--sysfs-root PATH] [--config FILE]` | Module, guard and SR-IOV state of the selected GPUs and the next step. Read-only. Exit 1 while a selected GPU is not ready for `enable` (expected before the first prepared boot). |
| `install [--prefix PATH] [--device BDF]... [--boot-display-release BDF]... [--vfs N] [--destdir PATH] [--source DIR] [--ari=on\|off] [--msix=on\|off] [--experimental-vfs=on\|off] [--twin-id=on\|off]` | Install or stage (see above). |
| `prepare [--ari=on\|off] [--msix=on\|off] [--experimental-vfs=on\|off] [--twin-id=on\|off] [--release-console=BDF]...` | Prepare the selected GPUs. Root. `--release-console` names every GPU the config grants a console release ([The boot display GPU](#the-boot-display-gpu)). |
| `prepare --boot [--ari=on\|off] [--msix=on\|off] [--experimental-vfs=on\|off] [--twin-id=on\|off]` | What the boot unit runs: as `prepare`, but with a console-release grant in the config it defers, changes nothing and exits 0. Root. |
| `prepare --clear-guard` | Clear the guard of an unfinished attempt, only after a power cycle. |
| `enable` | Enable the VFs of every prepared, selected GPU with `sriov-manage`. Root. |
| `activate [--device BDF]... [--type N] [--dry-run]` | Ada only, after `enable`. Root, takes the lock. Refuses (exit 3, nothing changed) unless NVIDIA's driver is 580.159.01 with `EnableGpuFirmware: 0`, `nvidia_vgpu_vfio` is loaded, `libnvidia-vgpu.so` resolves to the H5 copy, every VF of the Ada cards is on `nvidia`, no Ada PF has a `driver_override` other than empty or `nvidia`, `--device` (if given) names every selected Ada card, no NVIDIA VF on the host has a vGPU type, and no GPU outside `bdfs` has VFs enabled (the daemon restart is host-wide; VFs of a selected non-Ada card are allowed with a note). Then writes `/run/geforce-sriov/activating`, restarts `nvidia-vgpud`, checks again for vGPUs, restarts `nvidia-vgpu-mgr`, re-probes each Ada PF (guard, `unbindLock`, driver unbind and bind; never remove or rescan), and per VF sets type `N` (with `--type`) and `power/control=on`, gated on the readback `on`/`active`; only then removes `activating`. Exit 1 when a step after the checks fails, an `unbindLock` that is not granted included; `activating` then stays, so `check` passes no card until `activate` succeeds. Exit 5 when a PF does not come back to `nvidia` (guard stays set). Non-Ada cards get no re-probe, type or power write and are refused when named. `--type` is refused on a card with more than one VF. `--dry-run` checks and prints the steps. See [Ada host stack](ada.md#bring-up-order). |
| `check [--device BDF]... [--type N]` | Ada only. Read-only, no root, no lock: a point-in-time read that does not stop a later re-probe or daemon restart. One `OK` or `NOT READY` line per VF; exit 0 when every VF is on `nvidia` with `power/control=on`, `runtime_status=active` (and type `N` with `--type`), 1 otherwise or while `/run/geforce-sriov/activating` or the guard exists, 3 for an invalid or empty config, no selected Ada card, a `--device` that is not selected or not an Ada card, or a selected card that is missing. Run immediately before every VM start. |
| `uninstall [--unload]` | Remove only the files the package installed (see [recovery.md](recovery.md)). |
| `--version`, `--help` | |

`BDF` is a physical function `dddd:bb:ss.0` exactly as named in `/sys/bus/pci/devices`. The
domain has 4–8 lowercase hex digits (more than 4 only without leading zeros, at most `7fffffff`).
`--sysfs-root` points `doctor`/`status` at a copy of a sysfs tree; the commands that change the
system reject it. Exit status: 0 ok, 1 failed, 2 usage, 3 refused (nothing changed), 4 another
`geforce-sriov` holds the lock, 5 guard set (cold power cycle needed).

`doctor` prints one line per check, marked `OK`, `INFO`, `WARN` or `FAIL`. `FAIL` means `prepare`
would refuse (exit 3); `WARN` means a degraded setup or that a later step will refuse (for example a
missing `lspci`, an experimental or cross-die card, an unassigned BAR, a shared IOMMU group). For a
`--device` that the config does not select yet, its identity section shows what the hook will do
with the settings `install --device` writes. Checks of the host itself (tools, guard, boot unit,
module autoload) are skipped with `--sysfs-root`.

`status` exits 0 only when every selected GPU was prepared this boot and presents 1 to `vfs` VFs with no
BAR known to be unassigned; when the `resource` file cannot be read completely it prints
`BAR assignment: not confirmed`, so exit 0 does not prove that the BARs are assigned.

## Configuration rules

`/etc/geforce-sriov/config` (or `$GEFORCE_SRIOV_CONFIG`), shared by the CLI and the hook. One
`key=value` per line, `#` comments, no whitespace; a repeated key makes the file invalid, and the CLI
also rejects unknown keys. The hook uses the file only if it is not group- or world-writable and is
owned by root; otherwise it passes everything through. `prepare` and `enable` additionally require
`$GEFORCE_SRIOV_CONFIG`, when set, to be an absolute path, refuse a config that is a symlink, and
require the file and every parent directory to be root-owned and not group- or world-writable; a
read that fails partway is refused, never parsed in part. The keys are listed in the
[configuration reference](configuration.md#configuration-file) and annotated in
[config/config.example](../config/config.example). Absent file: no GPU is selected, the boot unit
does not run, and the hook passes every call through. A file without `vfs` (every file written
before the key existed) keeps the default of 4. `doctor` and `prepare` refuse a `vfs` above a
selected card's hardware default and warn above 4; `doctor` also warns when the loaded module's
count differs from the file's, which takes effect after the next boot. With `twin_id=on`, `doctor`
fails and `prepare` refuses (exit 3, nothing changed) when no selected card is an RTX 5090, because
the module would refuse to load.

## Multiple GPUs and PCI topology

Up to 8 physical functions can be selected. What the package does in each layout:

| Layout | Behaviour | Status |
|---|---|---|
| One selected GPU with a PCI bridge directly above it | The basic layout | Measured with each GPU directly below its own CPU root port (bus `03` below `0000:00:02.0`, bus `04` below `0000:00:03.0`); other bridges directly above the GPU are untested |
| Several selected GPUs | `prepare` checks every selected card before changing anything; one card that fails a check refuses the whole run (exit 3, nothing changed). Cards are then prepared one at a time; a card that fails stops the run. `enable` handles them in config order and stops at the first card that is refused or fails; cards enabled before it keep their VFs | Measured with two identical cards |
| Some GPUs selected, others not | Unselected GPUs are never prepared, never rewritten by the identity hook and never forced to HOST | Measured with two identical cards, one selected |
| Identical cards, only some selected (for example a twin of the boot display GPU) | The identity hook tells them apart by PCI address (see [The identity hook](#the-identity-hook)). If the driver does not answer its address queries, it falls back to the device ID only when **every** NVIDIA GPU with that ID is selected; otherwise that ID stays unchanged for all of them and a log line says why | Measured on 580.159.01: the unselected twin was logged as unselected and left unchanged |
| Different models in one host | Each card uses its own device-table row. All of them run under the **one** installed NVIDIA host driver: that build must support every selected card (driver branch, proprietary or open flavour, GSP). `prepare` and `enable` do not check the driver; `doctor` notes for each Blackwell card that it needs NVIDIA's open kernel module, which geforce-sriov does not support yet ([NVIDIA driver flavour](devices.md#nvidia-driver-flavour)), and for each Ada card the modified host stack it needs ([Ada host stack](ada.md)). `activate` refuses while a GPU outside `bdfs` has VFs enabled; VFs of a selected Ampere card are allowed, but an Ampere and an Ada card under the one Ada module have not been run. The two-byte driver patch exists only for the builds listed in [driver-patch.md](driver-patch.md) | Untested |
| GPU on a dedicated PCIe switch downstream port | The remove/rescan, the held-awake bridge and ARI Forwarding all use the GPU's **immediate** parent bridge, whichever it is; no root-port assumption is made. The switch's upstream link is shared by every GPU below it, so throughput can differ; the VF count does not | Untested |
| GPU on a bus shared with an unrelated device | Refused by `install --device`, `doctor` and `prepare`: removing the bus would touch the other device | Refused |
| The boot display GPU (`boot_vga` 1) | Refused unless the config grants its console release (`--boot-display-release`). With the grant the boot unit defers, and `prepare --release-console=BDF` by hand prepares the whole selection in one pass; see [The boot display GPU](#the-boot-display-gpu) | Measured (manual path) |
| GPU on a root bus with no PCI bridge above it | Refused: the bridge-scoped rescan is not possible | Refused |
| Other functions in the GPU's own slot | The kernel module refuses to load when a planned VF function (1–7) is already used by another device; the identity hook skips a card whose slot holds a second NVIDIA display function ("ambiguous slot"). The card's own audio function is expected | Refused |
| One RM device object spanning several GPUs (grouped or SLI-style) | The identity hook reports HOST only when the device object has exactly one subdevice; otherwise the reply stays unchanged | Not supported |
| PCI domain above `ffff` (for example behind Intel VMD) | Accepted in `--device`, `bdfs` and the module parameter as sysfs prints it, 4–8 hex digits up to `7fffffff` | Untested |
| Native ARI (Blackwell) | The card's own ARI capability is used as is; only `pci=noari` turns it off | Untested |

### PCI resources

Each GPU needs its BAR0 and, once SR-IOV is presented, its VF BARs assigned inside the bridge
windows above it. The package never writes resources: a rescan can only resize the card's own
bridge window, not the windows of a switch or of bridges further up. During the rescan the kernel
may log `VF BAR … can't assign` messages before it settles; what counts is the final state.
`doctor` and `status` read `/sys/bus/pci/devices/<PF>/resource` and warn when BAR0 or every VF BAR
is unassigned; `prepare` refuses a card whose BAR0 is unassigned, and stops when a card comes back
from the rescan without the BAR0 it had. Enabling *Above 4G decoding* in the firmware, or booting
with [`pci=realloc`](https://docs.kernel.org/admin-guide/kernel-parameters.html), may give the
windows enough room. These checks look only at whether a BAR is assigned, not at its size, so the
larger VF BARs of Blackwell (256 KiB, 256 MiB and 32 MiB per VF in public reports) are not an
error; they need more window space, and *Resizable BAR* settings can change how much is left
([Per-VF BARs on Blackwell](devices.md#per-vf-bars-on-blackwell)).

### IOMMU groups and ACS

VFs can only be assigned to VMs separately when the IOMMU places them in separate groups, and that
depends on ACS support in the GPU, the switch ports and the root ports. `doctor` reports each
card's IOMMU group: only its own functions (and its immediate bridge) is reported as information;
other devices in the group, or a VF that shares its PF's group, are warned about. This is the
kernel's grouping, **not** a statement of isolation between tenants; check the actual VF groups and
driver binding before assigning VFs. An ACS override patch changes the grouping, not the hardware's
isolation (see the kernel's [VFIO documentation](https://docs.kernel.org/driver-api/vfio.html)).

## The boot display GPU

The GPU the firmware booted on (`boot_vga` 1 in sysfs) carries the Linux console: the kernel's
`simple-framebuffer` platform device, driven by `simpledrm` or `simplefb`, scans out of one of its
BARs. A remove and rescan can move those BARs, so a console still on the card would be left on memory
the card no longer decodes. By default every command therefore refuses the boot display GPU. You can
instead **grant** a console release for it.

What a release means, as every command that grants or performs one prints it:

> Console release: the Linux console on that GPU stays detached until the next boot (the kernel
> switches to a dummy console; the monitor may keep the last frame or lose signal). Logs continue
> in the journal and over SSH; starting and stopping guests does not touch the display.

**Grant it** together with the other GPUs to prepare:

```sh
sudo ./geforce-sriov install --device 0000:03:00.0 --device 0000:04:00.0 --boot-display-release 0000:04:00.0
# or: sudo make install DEVICES="0000:03:00.0 0000:04:00.0" BOOT_DISPLAY_RELEASE="0000:04:00.0"
```

`--boot-display-release` must name a GPU also given with `--device`. Running `install --device …`
again without it removes the grant. The config then holds `boot_display_release=`.

**At boot nothing is prepared.** With a grant, the boot unit's `prepare --boot` prepares **none** of
the selected GPUs and changes nothing: the module's GPU list is fixed when it loads, and loading it
changes every listed card's configuration, so an early load would touch the boot display while its
console is live. It records the deferral in `/run/geforce-sriov/deferred` and exits 0. The
`nvidia-vgpud` drop-in has `AssertPathExists=!/run/geforce-sriov/deferred`: while deferred, starting
`nvidia-vgpud` fails, and so does starting a unit that requires it, such as `nvidia-vgpu-mgr`.
`status` exits 1 and prints `deferred` with the command to run.

**Before the manual step** (you arrange these; the package never changes module blacklists, services
or the kernel command line):

- Do not load `nvidia` before `prepare` has run. Udev and NVIDIA's services run by then, so keep
  the `nvidia` module from loading automatically (for example `blacklist nvidia` and
  `install nvidia /bin/false` in a modprobe.d file; `modprobe --ignore-install nvidia` loads it
  afterwards) and keep `nvidia-persistenced`, `nvidia-vgpu-mgr` and similar services from starting.
  If something binds a selected GPU first, `prepare` refuses that card.
- Reach the host over SSH or a serial console, and be able to cold power cycle it.

**Then, as root after each boot:**

```sh
geforce-sriov doctor
geforce-sriov prepare --release-console=0000:04:00.0
geforce-sriov status
```

`--release-console` is the consent for this run. It must name exactly the GPUs that
`boot_display_release` lists; without it, or with any other address, `prepare` refuses (exit 3) and
changes nothing. `prepare` then:

1. Checks every selected GPU before changing anything, including the console of the boot display. It
   releases a console only when the card's own `simple-framebuffer.N` (a child of the PF in sysfs,
   bound to `simple-framebuffer`) is its only user. It refuses (exit 3, nothing changed) when a firmware
   framebuffer cannot be attributed to the PF or is of another kind (`efi-framebuffer`,
   `vesa-framebuffer`, …), another fb or DRM device uses the card, the legacy VGA text console is
   bound, an early `efifb` console is registered, or anything other than the framebuffer's `BOOTFB`
   region sits inside one of the card's BARs in `/proc/iomem`. It also refuses a boot display whose
   BARs `/proc/iomem` lists five or more levels deep (for example behind two PCIe switches): the
   kernel stops indenting there, so what uses the BAR cannot be told. This limit applies only to the
   console release; other GPUs are prepared without reading `/proc/iomem`.
2. Writes the guard, then unbinds the framebuffer from its driver and confirms that no fb or DRM
   device of the card remains and that `BOOTFB` is no longer used. The unbound platform device stays
   registered; that is expected.
3. Loads `geforce_sriov` once for every selected GPU and prepares them card by card, as at boot.
4. Records the release in `/run/geforce-sriov/prepared/<BDF>` (`console_release=`) and, once every
   selected GPU is prepared, removes `/run/geforce-sriov/deferred`. Then load the vGPU host driver,
   run `enable`, and only then start `nvidia-vgpud` and `nvidia-vgpu-mgr`, as in the
   [installation guide](installation.md#6-enable-the-virtual-functions).

If `prepare` refuses or fails **before** the card's removal was attempted, it gives the console back,
but only while the PF is the same unbound device with the same resources and the framebuffer is still
its unbound child. Once the removal of the card has been attempted, even if the remove write
reported an error, the console stays released for the rest of the boot: the BARs may have moved,
and the firmware's framebuffer address is stale. A card that does not come back follows the
[recovery](recovery.md) rule: cold power cycle, do not rescan again.

`enable` accepts the boot display GPU only with this boot's record of its release and the grant
still in the config. To return to automatic preparation at boot, run `install --device` with the other
GPUs only; the next boot is an ordinary one again, and the console stays on the boot display.

## The identity hook

`nvidia-vgpud` only accepts GPUs listed in NVIDIA's `vgpuConfig.xml`, which has no GeForce IDs. The
hook is loaded into `nvidia-vgpud` only (never `nvidia-vgpu-mgr`). It stays passive unless the
process is `nvidia-vgpud`, `identity=on`, the driver version is exactly one it supports, and at
least one selected card can be mapped: an NVIDIA physical function of display class, listed in the
device table, and the only NVIDIA display function in its slot (otherwise it is skipped as an
"ambiguous slot").

It matches replies to cards **by PCI address**, asking the driver each time and keeping no cache:

- After a successful `NV2080_CTRL_CMD_BUS_GET_PCI_INFO` reply whose device ID is that of a mapped
  card, it asks the driver which GPU the reply came from (`NV2080_CTRL_CMD_GPU_GET_ID`, then
  `NV0000_CTRL_CMD_GPU_GET_PCI_INFO` for its domain, bus and slot) on the daemon's own file
  descriptor and client. Only a reply from a selected card's exact address, carrying that card's own
  device ID, gets the table's target device ID (and subsystem, for targets whose `<pgpu>` entry
  fixes one). A reply from any other GPU is left unchanged, even if it has the same device ID.
- With `identity_force_host=on`, a virtualization-mode reply that is not already HOST is attributed
  the same way through its device object (exactly one subdevice, its existing subdevice handle, its
  GPU ID and address) and set to HOST only for a selected card. If the reply cannot be attributed,
  it stays unchanged.
- If the driver does not answer the address queries for a PCI-info reply, the hook falls back to the
  device ID, but only when every NVIDIA GPU with that ID was a mapped card at startup and still is
  in a fresh scan of sysfs; otherwise the reply stays unchanged and a log line says why. There is no
  such fallback for HOST.

It changes nothing else: no vGPU type lists, no manager, no license, no XML. The hook adds two
driver queries to each matching PCI-info reply and four to each virtualization-mode reply it
considers. Details, gates and log lines: [hook/README.md](../hook/README.md).

## The driver patch

On 580.159.01, guest CUDA work can stall because NVIDIA's closed `nv-kernel.o_binary` disables the
GPU's runlist scheduler; a two-byte change skips that call. `geforce-sriov-patch` makes that change
to a copy of your own file; you rebuild NVIDIA's kernel module yourself, and again after every
kernel or NVIDIA driver update. See [driver-patch.md](driver-patch.md).
