# Changelog

All notable changes to this project are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- GitHub issue forms: a compatibility report for results on your own hardware, working or not,
  and a short bug report for software bugs. How to collect and redact the details:
  [docs/compatibility.md](docs/compatibility.md).
- `twin_id` (module parameter, config key `twin_id`, `install --twin-id=on|off`,
  `prepare --twin-id=on|off`), off by default, experimental and untested: on an RTX 5090
  (`10de:2b85`) the kernel module shows NVIDIA's driver the RTX PRO 6000 Blackwell Server Edition
  ID (`10de:2bb5`, subsystem `10de:204e`) in its 32-bit reads of config dwords `0x00` and `0x2C`.
  It may make the GPU firmware fail to start. No effect on other cards; `doctor` fails and
  `prepare` refuses when no selected card is an RTX 5090. The module also refuses `twin_id` for a
  card behind a hotplug-capable port or with a driver already bound.
- [docs/devices.md](docs/devices.md): a status column for every row, and a section on Ada and
  Blackwell: VF count, the NVIDIA driver flavour each generation needs, Blackwell per-VF BAR sizes,
  `twin_id`, and third-party reports for the RTX 5090 (not verified by geforce-sriov).
- `geforce-sriov activate [--device BDF]... [--type N] [--dry-run]` (Ada only, root): after
  `enable`, restarts `nvidia-vgpud` and then `nvidia-vgpu-mgr`, re-probes each selected Ada card
  (guard, NVIDIA's `unbindLock`, driver unbind and bind; no remove or rescan), optionally sets
  vGPU type `N` on its VF, then writes `power/control=on` and requires the readback `on`/`active`.
  It refuses unless NVIDIA's driver is 580.159.01 loaded with `NVreg_EnableGpuFirmware=0`,
  `nvidia_vgpu_vfio` is loaded, `libnvidia-vgpu.so` resolves to the H5 copy of the plugin, no
  NVIDIA VF on the host has a vGPU type and no GPU outside `bdfs` has VFs enabled (the daemon
  restart is host-wide). From its first change until every VF reads back `on`/`active` it keeps
  `/run/geforce-sriov/activating`. Not run on hardware.
- `geforce-sriov check [--device BDF]... [--type N]` (Ada only, read-only): the gate before every
  VM start; exit 0 only when each VF is on `nvidia` with `power/control=on` and
  `runtime_status=active` (and type `N`), and neither `/run/geforce-sriov/activating` nor the
  guard exists. Not run on hardware.
- `doctor` prints, for every selected Ada card, the host stack it needs and that geforce-sriov
  builds and installs none of it; `enable` names `activate` as the next step for Ada cards.
- [docs/ada.md](docs/ada.md): the Ada host stack measured by hand on one RTX 4070 SUPER
  (`10de:2783`): proprietary 580.159.01 with GSP off and two `nvidia.ko` changes, the H5 plugin
  copy, one VF, the VF power fix, the guest drivers that ran, and recovery. It separates that
  measurement from the package steps above, which are not hardware-tested.
- `geforce-sriov-patch ada list|inspect|apply|revert` (offline): the Ada revisions on a copy of
  your own 580.159.01 host kernel object `nv-kernel.o_binary` or vGPU plugin
  `libnvidia-vgpu.so.580.159.01`, writing only `NEWOUTPUT`. Each revision admits a file only by its
  exact whole-file SHA-256. Sites: the Ada probe gate in the host object at `0xd654ef`
  (`75 7f` → `90 90`), accepted on the stock object and on the object that already carries the
  scheduler revision, and the H5 change in the plugin at `0x8fb0e` (`f8 06` → `00 5a`). Apply the
  scheduler revision (`geforce-sriov-patch apply`) first: the scheduler commands refuse an object
  that already carries the gate revision ([Ada revisions](docs/driver-patch.md#ada-revisions)).
  The accepted and revised digests were verified offline against copies of the measured files. No
  module is built or installed, and the plugin copy is not installed.

### Changed

- Ada and Blackwell cards present 1 VF, the card's own count, by default: `experimental_vfs` now
  defaults to `off` (module parameter `0`). NVIDIA's driver reads the VF count of these cards from
  the hardware itself.
- With `experimental_vfs=on`, the raised count on Blackwell now also reaches NVIDIA's driver: the
  kernel module answers its 32-bit read of InitialVFs/TotalVFs with the same count the kernel sees.
  Untested; the GPU firmware may still refuse more than 1 VF. On Ada only Linux sees the raised
  count.
- `doctor` notes for every Blackwell card that the proprietary 580.159.01 driver does not support
  it and that the open kernel module it needs is not supported by geforce-sriov yet.
- Documentation: Ada is no longer described as "untested on either flavour" or as needing GSP.
  The one measured card ran the proprietary 580.159.01 module with GSP off and changes made by
  hand; every Ada row, `2783` included, stays experimental.

### Fixed

- Documentation: one gate for more than one VF on Ada and Blackwell is known (NVIDIA's driver reads
  TotalVFs from the hardware), and Ada dumps do record config `0x41C` bit 29; only its meaning on
  Ada is unconfirmed.

### Known limitations

- `geforce-sriov-patch` makes the Ada host-object and plugin revisions only on copies; building
  and installing `nvidia.ko` and installing the plugin copy and its link remain your steps.

## [0.1.0]

Initial release.

### Added

- `geforce-sriov` CLI: read-only `devices`, `doctor` and `status`; `install`, `prepare`, `enable`
  and `uninstall`. Up to 8 selected GPUs, all checked before anything changes.
- `geforce_sriov` kernel module (livepatch, DKMS package `geforce-sriov/0.1.0`): for the listed
  cards only, presents the card's hidden SR-IOV capability, enables ARI in software and reveals
  MSI-X on Ampere. Parameters `bdfs`, `vfs`, `ari`, `msix` and `experimental_vfs`.
- Boot unit `geforce-sriov-prepare.service`: prepares the selected cards once per boot, before
  NVIDIA's driver binds them.
- Identity hook `libgeforce_sriov.so`, loaded only into `nvidia-vgpud` through a systemd drop-in:
  presents each selected card as its professional counterpart (for example RTX 3080 as RTX A6000)
  so that NVIDIA's daemon offers vGPU types for it.
- Driver patcher `geforce-sriov-patch` (`inspect`, `apply`, `revert`): a two-byte fix to the
  user's own copy of NVIDIA's `nv-kernel.o_binary` so that guest GPU work does not stall.
- Device table of 38 GeForce device IDs across Ampere, Ada and Blackwell.
- Boot display GPU by explicit grant: `install --boot-display-release BDF`, then
  `prepare --release-console=BDF` by hand after each boot.
- VF count setting: `install --vfs N` or `vfs=N` in `/etc/geforce-sriov/config`, default 4.
- Offline test suite (`make check`).

### Status

Experimental. Measured on one host: two memory-modded RTX 3080 20GB cards (`10de:2206`,
subsystem `10de:146d`), kernel `7.0.14-16-pve`, NVIDIA vGPU host driver 580.159.01 with
`nvidia.ko` rebuilt from `geforce-sriov-patch` output, `ari=on` and `msix=on`. The identity hook
presented both cards as `10de:2230` (RTX A6000) with 23 vGPU types each, and `enable` created
4 VFs per card. Eight guests (per card 2 Linux and 2 Windows 11, each with a 4 GiB `RTXA6000-4Q`
vGPU) ran one 30-minute concurrent CUDA and D3D11 workload with no failures recorded; one Linux and
one Windows guest were restarted while the others kept running.

### Known limitations

- Only `10de:2206` with subsystem `10de:146d` has been measured. Every other card in the device
  table, and every cross-die mapping, is experimental.
- The VF count defaults to 4: ARI is enabled in software, but ARI decoding in the GPU hardware
  does not work for now.
- vGPU types are not filtered by the card's memory; choose types that fit together.
- NVIDIA's vGPU host driver and the guests' vGPU licence setup are not included; you supply both.
