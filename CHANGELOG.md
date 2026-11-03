# Changelog

All notable changes to this project are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- GitHub issue forms: a compatibility report for results on your own hardware, working or not,
  and a short bug report for software bugs. How to collect and redact the details:
  [docs/compatibility.md](docs/compatibility.md).

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
