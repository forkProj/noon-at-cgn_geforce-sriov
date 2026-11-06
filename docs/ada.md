# Ada host stack

This page describes the NVIDIA host stack for Ada (AD10x) GeForce cards: what was measured, what
this package automates, and what is still up to you. It covers the **host only**. It does not
create VMs and does not cover licensing.

> [!WARNING]
> Three different levels of evidence apply on this page. Keep them apart:
>
> | Level | What it covers |
> |---|---|
> | **Measured, by hand** | One RTX 4070 SUPER (AD104, `10de:2783`, ASUS subsystem `1043:8974`) on one Proxmox VE host, kernel `7.0.14-16-pve`, brought up with hand-written lab scripts. A pre-release geforce-sriov build presented SR-IOV (kernel module from DKMS `geforce-sriov/0.1.0`, config `experimental_vfs=on`), and an identity hook built from a pre-release commit of this package was configured in `nvidia-vgpud`. One VF was used: Linux showed 4, of which only the first reached a vGPU |
> | **Package integration, not hardware-tested** | `geforce-sriov activate` and `geforce-sriov check` of this release. They follow the measured order but have not been run on a GPU, and they do not build or install any NVIDIA part |
> | **Experimental** | Every other Ada device ID in the [device table](devices.md#rows), on which nothing has been run; and, on the measured card, NVIDIA's open flavour with GSP, which ran Linux guests only with lab-only workarounds and is not packaged |
>
> A listed device ID means the tools will act on that card, not that it works.

## The measured stack (RTX 4070 SUPER, by hand)

This is the configuration that ran end to end on the one measured card. Every NVIDIA file in it is
the user's own copy; the patched copies are made beside the stock files, which stay unchanged.

| Part | Measured setting |
|---|---|
| Card | RTX 4070 SUPER, AD104, `10de:2783`, presented as `10de:27b8` (NVIDIA L4). The identity hook was configured in `nvidia-vgpud` for that card; the archived evidence does not show which component performed the rewrite. On the measured host the PF was `0000:03:00.0` and its one VF `0000:03:00.4`; the addresses on your host differ |
| Host driver | NVIDIA vGPU-KVM **580.159.01**, proprietary flavour, loaded with `NVreg_EnableGpuFirmware=0`: the driver runs on the CPU, GSP firmware is not used (`nvidia-smi -q` shows `GSP Firmware Version: N/A`) |
| `nvidia.ko` | Rebuilt from the user's `nv-kernel.o_binary` with **two** two-byte changes: the Ada probe gate and the scheduler workaround ([below](#the-two-nvidiako-changes)) |
| vGPU plugin | A patched copy of `libnvidia-vgpu.so.580.159.01` (the H5 change, [below](#the-plugin-change)); the symlink `libnvidia-vgpu.so` points at the copy |
| Virtual functions | **One** VF, type `911` (`NVIDIA L4-2Q`, 2048 MiB) |
| VF power | `power/control=on`, `runtime_status=active` on the VF, set after the last PF re-probe and checked before every VM start ([VF power gate](#the-vf-power-gate)) |
| Guest drivers | vGPU 16.9: Linux 535.230.02 and Windows 539.19 ([Guest driver prerequisite](#guest-driver-prerequisite)) |

Measured with that stack, on that one host:

- three clean stop and re-attach cycles of a Linux guest, a 30-minute Windows 539.19 run, and a
  Linux guest again afterwards; no guest initialisation failure and no Xid or AER error;
- a cold boot with the unchanged bring-up script: the VF came up `on`/`active` without a manual
  write;
- a host `systemctl reboot` with `nvidia` loaded and the VF present reached the reboot target.

Not measured: more than one VF in use, more than one VM, a multi-day run, other kernels or
distributions, any other Ada board, a host with an Ampere and an Ada card under the same Ada
module, this release's build with its default settings, and the package steps of this release on
a GPU.

The lab scripts differed from the package in ways that matter if you compare them:

- They kept the host's second GPU (an Ampere card) driverless while `nvidia` loaded. The package
  does not do that; every GPU on the host is driven by the same Ada module.
- They loaded `nvidia` by hand (`insmod … NVreg_EnableGpuFirmware=0`) after boot and created the
  VF by writing `sriov_numvfs` with the PF on `pci-pf-stub`. The package leaves loading to your
  module configuration, for example `options nvidia NVreg_EnableGpuFirmware=0` in a file in
  `/etc/modprobe.d/` (unmeasured: the measured host loaded the module directly; DKMS can also
  replace the module you installed, see the [rebuild warning](driver-patch.md#install-the-patched-module)),
  and creates the VF with NVIDIA's `sriov-manage` (`geforce-sriov enable`).
- They re-probed the PF with a plain driver unbind and bind. `activate` takes NVIDIA's
  `unbindLock` first; whether the driver grants it while `nvidia-vgpu-mgr` runs on this CPU-RM
  stack has not been measured.
- The geforce-sriov config had `experimental_vfs=on`, so Linux showed 4 VFs, of which only the
  first was used. The package default (`experimental_vfs=off`) presents the one VF the card has.
  With `experimental_vfs=on` and `vfs` above 1 (the default is 4), `check` reports every VF after
  the first `NOT READY` and so always exits 1 ([The VF power gate](#the-vf-power-gate)).

### Why each change is needed

- **CPU-RM instead of GSP.** NVIDIA's proprietary 580.159.01 host module refuses Ada on a vGPU
  host: `rm_is_supported_device` returns `NV_ERR_NOT_SUPPORTED` (`0x56`) for an architecture above
  `0x17`, and Ada is `0x19`. With that check bypassed, `NVreg_EnableGpuFirmware=0` selects the
  driver's CPU-side mode. The open flavour, which always uses GSP, ran Linux guests on the measured
  card only with further lab-only workarounds and still showed scheduler stalls; it is not
  supported.
- **Scheduler workaround.** The same `jne` → `NOP NOP` change that
  [driver-patch.md](driver-patch.md#why-it-exists) applies for Ampere, where guest CUDA work
  stalled without it. On Ada it was present in every measured run; a run without it was not
  measured.
- **Plugin change.** Without it, `nvidia-vgpu-mgr` aborts the vGPU configuration (`0x1f`) and the
  `L4-2Q` instance does not start.
- **One VF.** The card reports TotalVFs 1, and NVIDIA's driver reads that count from the hardware.
  With `experimental_vfs=on` Linux showed 4 VFs on the measured card, but only the first one reached
  a vGPU; the others failed with `NV_ERR_OUT_OF_RANGE` (`0x5B`).
- **VF power on.** With `power/control=auto` the VF runtime-suspends between attaches; on the next
  attach its bus-master bit was clear during guest start, its MSI-X interrupts could not reach the
  host, and the guest driver failed with `RmInitAdapter failed! (0x11:0x45)`
  (`NV_ERR_IRQ_NOT_FIRING`). `auto` failed 8 of 8 re-attaches; `on` passed every one measured.
- **`enable-migration=off`.** On this stack a plain VM stop without it makes `nvidia-vgpu-vfio`
  return an error to QEMU's migration handshake, and QEMU resets the device. The stop still
  completes; with `enable-migration=off` on the VF's `vfio-pci` device it completes without those
  messages.

### The two `nvidia.ko` changes

Both are in NVIDIA's closed object `nvidia/nv-kernel.o_binary` of the 580.159.01 vGPU-KVM host
package. Offsets are file offsets in that object.

| Change | File offset | Bytes | Effect |
|---|---|---|---|
| Ada probe gate | `0xd654ef` | `75 7f` → `90 90` | the vGPU host no longer refuses architecture `0x19` at probe |
| Scheduler workaround (`_nv052453rm`) | `0x9f2588` | `75 76` → `90 90` | the PMU callback no longer disables the runlist scheduler ([driver-patch.md](driver-patch.md)) |

Both revisions are made by `geforce-sriov-patch` on a copy of your stock `nv-kernel.o_binary`. It
writes only `NEWOUTPUT`; the installed file is never touched. Each step admits only inputs it knows
by digest, so the order matters: first the scheduler revision on the stock object, then the gate
revision on that output. `ada apply` also accepts the stock object itself, but its gate-only output
is not the measured stack. Verified offline: `geforce-sriov-patch inspect` and `apply` both refuse
the gate-only object (`stock-normalized SHA-256 mismatch: unknown version/flavour or other
modifications`) and create no output.

```sh
geforce-sriov-patch apply INPUT NEWOUTPUT         # scheduler revision, on the stock object
geforce-sriov-patch ada apply INPUT NEWOUTPUT     # gate revision, on the output of the step above
```

Confirm each step with `sha256sum`:

| Object (580.159.01 host package) | SHA-256 |
|---|---|
| Stock `nv-kernel.o_binary` | `4e3abe645b6873ed478adea6a06c6955352ff06186849edeea536c9630596449` |
| After `geforce-sriov-patch apply` (scheduler revision) | `ce8a485bfaeebca3a9609f9415a6df81c160a0a53509aec093bc3361a6d12701` |
| After `geforce-sriov-patch ada apply` (both revisions) | `f5b788cdb3e06aed18860cc6cf950dcc13143bd831d90d8e9c4bf9e744426f12` |
| After `geforce-sriov-patch ada apply` on the stock object (gate only; **not** the measured stack) | `ed3efee68582f4fa9f120cf6c28138d8642db93ff0e6ba07ab0d6d7bf1b9823b` |

Revert in the opposite order: `geforce-sriov-patch ada revert PATCHED NEWOUTPUT` takes the object
with both revisions back to the scheduler-only object, then `geforce-sriov-patch revert PATCHED
STOCK NEWOUTPUT` takes that back to stock ([Usage](driver-patch.md#usage)). Restoring the stock
object or module from your own backup also works.

The tool never builds, installs or loads a module: rebuild `nvidia.ko` from its output
([Rebuilding NVIDIA's module](driver-patch.md#rebuilding-nvidias-module)) and install it yourself.
The measured module was built by hand (for kernel `7.0.14-16-pve`, SHA-256
`457abacb843d8b1b7a66798a86d1084eb3470b746104ea3819091316a8f9afe1`; a build for another kernel
differs).

### The plugin change

`libnvidia-vgpu.so.580.159.01`, stock SHA-256
`7e52b76dc7ca8490e7ffbc1fd724f5db07f3791a3a2ec07f2d595bf05abdcaa7`: at file offset `0x8fb0e`,
`f8 06` → `00 5a`. Make the revision with `geforce-sriov-patch ada apply INPUT NEWOUTPUT` on a copy
of the stock plugin. The revised copy has SHA-256
`097f571588f3a232265103b73e38ba111ddb44ef329ac720562cff71e6170d37`. The stock file is never
modified; only the symlink `libnvidia-vgpu.so` is pointed at the copy. geforce-sriov does not
install the copy or change the link; `activate` only checks that `libnvidia-vgpu.so` resolves to a
file with that SHA-256.

## Guest driver prerequisite

geforce-sriov installs nothing in a guest. The guest drivers that ran on this host stack are an
exception to the [Ampere rule](installation.md#1-prerequisites-checklist) of taking the guest
driver from the same release as the host driver. The table is validation only: it lists the drivers
that ran, not a supported matrix.

| Guest | Driver that ran | Required guest setting |
|---|---|---|
| Linux | 535.230.02 (vGPU 16.9) | `options nvidia NVreg_RegistryDwords="RMSetVgpuGspPluginOffloadMode=0"` in `/etc/modprobe.d/`. Without it the guest driver loops `RmInitAdapter failed! (0x22:0x65:897)` |
| Windows | 539.19 (vGPU 16.9) | DWORD `RMSetVgpuGspPluginOffloadMode` = `0` in the adapter's class key and in `HKLM\SYSTEM\CurrentControlSet\Services\nvlddmkm`. Without it the device shows Code 43 |

Guest licensing was the operator's own setup and is out of scope.

Of the guest drivers of the host's own 580 release, only the open 580.178.04 guest driver was run on
this CPU-RM host, and it timed out (`0x65`); the proprietary 580.159.03 guest driver was present but
was not run on this host stack. The 16.9 branch is outside NVIDIA's supported matrix for a 580 host
and receives no further fixes.

## What geforce-sriov does for Ada

This release adds two commands for Ada, `activate` and `check`; `doctor` and `enable` print Ada
hints. Nothing changes in `install`, the boot unit, the config file or `uninstall`. On Ampere and
Blackwell cards both new commands do no re-probe, type or power write (and refuse a non-Ada card
named with `--device`). The daemon restart in `activate` is host-wide, though: it re-registers the
VFs of every GPU, so `activate` refuses while a GPU that is not selected in `bdfs` has VFs enabled,
and prints a note for the VFs of a selected non-Ada card ([below](#bring-up-order)). The Ampere
steps stay as in the [installation guide](installation.md).

| Part | Who provides it |
|---|---|
| SR-IOV on the card, 1 VF by default, the boot unit, the identity hook | geforce-sriov (`install`, as for Ampere) |
| `nvidia.ko` with both changes, loaded with `NVreg_EnableGpuFirmware=0` | `geforce-sriov-patch` makes both revisions on a copy of your `nv-kernel.o_binary`; building and installing the module remain **your** step |
| The H5 copy of the plugin and the `libnvidia-vgpu.so` link to it | `geforce-sriov-patch ada apply` makes the copy; placing it and pointing the link at it is **your** step |
| VF creation | geforce-sriov `enable` (NVIDIA's `sriov-manage`) |
| Daemon restart, PF re-probe, optional type, VF power on and its check | geforce-sriov `activate` |
| Gate before every VM start | geforce-sriov `check` |
| Guest drivers, licensing, VM configuration (including `enable-migration=off`) | **you** |

> [!IMPORTANT]
> `geforce-sriov-patch` makes both `nvidia.ko` revisions and the plugin revision, on copies. The
> package still neither builds nor installs the module, so you supply the built module and the
> plugin copy. Without the gate revision NVIDIA's 580.159.01 module refuses the card; until you
> have that module and the plugin copy, `activate` refuses and the Ada stack cannot be completed
> with this package.

`geforce-sriov doctor` prints one `[INFO]` line per selected Ada card that lists these
prerequisites. Every Ada card stays `experimental` in `geforce-sriov devices`, and `install`,
`doctor` and `prepare` still warn about it.

### Bring-up order

The order is the measured one: the VF exists, then both daemons start, then the PF is re-probed,
then the VF is powered on. Each boot, after the boot unit has prepared the card and NVIDIA's driver
has bound it:

```sh
sudo geforce-sriov enable                     # creates the VF; for Ada it ends with "next `geforce-sriov activate`"
sudo geforce-sriov activate --dry-run         # preflight checks only, then prints the steps; changes nothing
sudo geforce-sriov activate                   # or: activate --type 911, to also set the vGPU type
geforce-sriov check                           # before every VM start; add --type N to check the type too
```

> [!WARNING]
> Run `activate` only while no VM start is pending or can begin, for example before your VMs
> autostart or with VM starts held back. Restarting `nvidia-vgpu-mgr` ends any vGPU on the host.
> `activate` checks for vGPUs before and between the two daemon restarts, but a VM that sets its
> vGPU type just before the manager restart still loses it; only keeping VM starts and `activate`
> apart closes that window. The lab scripts relied on the same kind of serialisation.

Do not restart `nvidia-vgpud` or `nvidia-vgpu-mgr` by hand on Ada: `activate` restarts them itself,
in order. It runs only as root, takes the package lock, and does this:

1. Refuses with exit 5 before any other check if the guard is set. Then checks, and refuses with
   nothing changed (exit 3) unless all hold: an Ada card is selected, and `--device`, if given,
   names every selected Ada card (the daemon restart re-registers the VFs of every GPU); NVIDIA's
   driver is 580.159.01 and `/proc/driver/nvidia/params` shows `EnableGpuFirmware: 0`;
   `nvidia_vgpu_vfio` is loaded; `libnvidia-vgpu.so` resolves to the
   H5 copy; each Ada card was prepared this boot, is bound to `nvidia`, has all its VFs enabled and
   has no `driver_override` other than empty or `nvidia`; every VF is bound to `nvidia` and has
   `power/control`; NVIDIA's `unbindLock` file exists; **no NVIDIA VF on the host has a vGPU type
   set**, because restarting the daemons would end that vGPU; and **no GPU outside `bdfs` has VFs
   enabled**, because the restart would re-register them too. VFs of a selected non-Ada card (for
   example an Ampere card after `enable`) are allowed: `activate` prints a note that the daemon
   restart is expected to set their `power/control` back to `auto` (inferred, not measured) and
   leaves them so. Selecting an Ampere and an Ada card
   together has not been run on hardware. `--dry-run` stops here and prints the steps. It runs the
   preflight checks only: it cannot know whether the driver grants `unbindLock`, whether a vGPU
   appears after the `nvidia-vgpud` restart, or whether the PF returns to `nvidia`.
   Right before its first change, `activate` writes `/run/geforce-sriov/activating`. It removes the
   file only after every VF has read back `on`/`active`, and `check` reports `NOT READY` while it
   exists. After exit 1 it stays until a later `activate` succeeds; if `activate` reported that the
   guard stays set, recover as for exit 5. After exit 5 the guard is still set, so the next
   `activate` refuses with exit 5 until the host is powered off and on and
   `geforce-sriov prepare --clear-guard` has run. In either case the host reboot removes the file.
2. `systemctl restart nvidia-vgpud.service`; checks again that no vGPU appeared meanwhile (if one
   did, it stops with exit 1 before restarting the manager or re-probing anything); then
   `systemctl restart nvidia-vgpu-mgr.service`, waits up to 10 s until the manager is active, then
   3 s more (the measured pause).
3. For each Ada card: writes the guard, takes NVIDIA's `unbindLock`, writes the PF to
   `/sys/bus/pci/drivers/nvidia/unbind`, then to `…/bind`, waits up to 10 s for the PF to be on
   `nvidia` again, and clears the guard. It never removes a device or rescans a bus. The
   `unbindLock` step is added by the package; the measured re-probe was a plain unbind and bind.
4. For each VF: waits up to 15 s for `nvidia/creatable_vgpu_types`; with `--type N` writes `N` to
   `nvidia/current_vgpu_type` (only when the type reads `0` or `N` and `N` is listed) and reads it
   back; writes `on` to `power/control`; reads back `power/control=on` and
   `runtime_status=active`.

`--type` has no default and no config key; without it `activate` writes no type, and whatever
starts the VM must set one. `--type` is refused on a card with more than one VF
(`experimental_vfs=on`), since only one Ada VF was measured to start a vGPU. The measured type was
`911` (`L4-2Q`); the type numbers depend on your driver, so read `nvidia/creatable_vgpu_types` of the
VF.

Exit codes are the package's usual ones ([configuration.md](configuration.md#commands)):

| Exit | `activate` | `check` |
|---|---|---|
| 0 | every VF on and active (`--dry-run`: the preflight checks passed; the later steps can still fail) | every VF ready |
| 1 | a step failed after the checks: a daemon did not restart (then no GPU was re-probed), a vGPU appeared after the `nvidia-vgpud` restart, `unbindLock` was not granted (a client such as `nvidia-smi`, `nvidia-persistenced` or a CUDA program still holds the GPU) or another re-probe step stopped, the types did not appear, or the type or power readback is wrong. When it stops at one card, the cards before it were re-probed and powered, but `/run/geforce-sriov/activating` stays, so `check` reports `NOT READY` for every card until `activate` succeeds. Start no VM until then | a VF or PF is `NOT READY`, a card has a VF after the first (`experimental_vfs=on`), `/run/geforce-sriov/activating` exists, or the guard is set |
| 2 | usage error | usage error |
| 3 | refused, nothing changed | an invalid or empty config, no selected Ada card, a `--device` that is not selected or not an Ada card, or a selected card that is missing |
| 4 | another `geforce-sriov` command is running | |
| 5 | the guard is set, or the PF did not come back to `nvidia` after the re-probe; see [Recovery](#recovery) | |

## The VF power gate

`geforce-sriov check [--device BDF]... [--type N]` reads sysfs only and needs no root. It reports
`NOT READY` and exits 1 while `/run/geforce-sriov/activating` exists (`activate` is running or did
not finish), while the guard `/var/lib/geforce-sriov/prepare-attempt` is set, or when it cannot tell
whether either file exists (for example a user without access to their directory: `NOT READY:
cannot tell whether <path> exists: …`). Every VF after the first (`virtfn1` and up, only present
with `experimental_vfs=on` and `vfs` above 1) is `NOT READY`, since only the first VF reached a
vGPU on the measured card; with more than one VF `check` therefore always exits 1. For each VF of
the selected Ada cards it prints one line:

```text
OK 0000:03:00.4 (VF of 0000:03:00.0): driver=nvidia type=911 expected=911 power/control=on runtime_status=active
```

Run it before **every** VM start and start no VM on a VF marked `NOT READY`. `check` is a
point-in-time read and takes no lock: nothing stops a PF re-probe or a daemon restart between
`check` and the VM start, so run it immediately before the start and do not re-probe or restart
NVIDIA's daemons in between. Any later PF re-probe or restart of `nvidia-vgpu-mgr` sets
`power/control` back to `auto` (an inference, not measured); then stop the VMs that use a vGPU,
set their VFs' type back to `0`, and run `activate` again. A udev rule or a systemd
`ExecStartPost` that writes `on` is not a replacement: on the measured host both were reported to
have run before the re-probe, which undid them (reported, not measured). That the re-probe's
registration of the VF resets `power/control` is an inference, not measured.

`check` reads only the current state of the VFs; it does not confirm that `activate` ran in this
boot. `check --type N` is the stronger check: a vGPU type can only be set on a VF after
`nvidia-vgpud` has registered the types.

## On the VM side

geforce-sriov does not create or configure VMs. For comparison only: on the measured host each VM
received the VF as a plain QEMU device, `-device vfio-pci,host=0000:03:00.4,enable-migration=off`,
with the vGPU type already set on the VF; no mdev was created. Whether Proxmox VE's
`hostpci …,mdev=` route ([Create a VM](installation.md#8-create-a-vm)) works on Ada has not been
tried.

## Recovery

The [one rule](recovery.md#the-one-rule) applies: if the card disappears or the host cannot be
reached, power the machine fully off and on; do not rescan.

| Situation | What to do |
|---|---|
| `activate` exits 5 because the PF is not back on `nvidia` | The guard stays set and none of that card's VFs is powered. Cold power cycle, `geforce-sriov doctor`, then `geforce-sriov prepare --clear-guard` ([The guard](recovery.md#the-guard)) |
| `activate` refuses because a VF has a vGPU type | Stop that VM, `echo 0 > /sys/bus/pci/devices/<VF>/nvidia/current_vgpu_type`, run `activate` again |
| `activate` refuses because a GPU that is not selected has VFs enabled | Stop the VMs on that GPU and disable its VFs with NVIDIA's `sriov-manage -d <PF>`, then run `activate` again |
| `check` reports that `/run/geforce-sriov/activating` exists | `activate` is running or did not finish; after a failure `activate` says so in its last line. Start no VM. Fix what it reported and run `activate` again (a leftover file does not block it, and a successful run removes it), or reboot |
| `activate` refuses because of `driver_override` on the PF | `echo > /sys/bus/pci/devices/<PF>/driver_override`, then run `activate` again |
| `activate` exits 1 with `unbindLock` not granted (`activate stopped at <PF> …`) | Stop the program that holds the GPU (`nvidia-smi` loops, `nvidia-persistenced`, CUDA programs). The cards before it were re-probed and powered, but `check` reports `NOT READY` for every card until `activate` succeeds. Before running `activate` again, write `0` to the type of every VF it set with `--type`, or it refuses because a vGPU type is set |
| `check` shows `power/control=auto` or `runtime_status=suspended` | Do not start the VM. Stop the VMs on that card, set their types to `0`, run `activate` again |
| Undo the power setting | `echo auto > /sys/bus/pci/devices/<VF>/power/control`; no reboot needed. Re-attaching a VM after a stop is then expected to fail again in the guest with `RmInitAdapter failed! (0x11:0x45)` |
| Undo the type | With the VM stopped: `echo 0 > /sys/bus/pci/devices/<VF>/nvidia/current_vgpu_type` |
| Back to NVIDIA's stock files | Point `libnvidia-vgpu.so` back at `libnvidia-vgpu.so.580.159.01` and put back your stock `nvidia.ko` ([Reverting](driver-patch.md#reverting)), then reboot. The stock module refuses the Ada card on a vGPU host |

`activate` and `check` install nothing, so `uninstall` has nothing extra to remove. Do not unload
`nvidia` while VFs exist ([What not to do](recovery.md#what-not-to-do)).

## Known limits

- Measured on one board, one host, one kernel and one driver build (580.159.01). Other Ada device
  IDs, kernels, distributions and hypervisors are untested.
- One VF per card is the measured and default configuration.
- `activate`, `check` and the `sriov-manage` VF creation on Ada have not been run on hardware.
- Whether NVIDIA's driver grants `unbindLock` while `nvidia-vgpu-mgr` runs on this CPU-RM build
  is unmeasured. If it does not, `activate` stops at the first card every time (exit 1, lock
  released, nothing unbound), and this package cannot complete the bring-up.
- The open-flavour/GSP route is not supported.
- geforce-sriov ships no NVIDIA file. `geforce-sriov-patch` makes the two `nvidia.ko` revisions and
  the plugin revision on copies; building, installing and loading the module and placing the plugin
  copy remain your steps.
- Licensing and VM creation are outside this project.
