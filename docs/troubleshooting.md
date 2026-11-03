# Troubleshooting

Start with `geforce-sriov status` and `geforce-sriov doctor`: each card line names its next step.
If a card has disappeared or the host cannot be reached, read [recovery.md](recovery.md) first.

## Symptoms

| Symptom | Likely cause | What to do |
|---|---|---|
| `prepare` or the boot unit exits with status **5**: `guard … is set` | An earlier preparation did not finish (interrupted, or a card did not come back) | Cold power cycle, run `geforce-sriov doctor`, then `sudo geforce-sriov prepare --clear-guard` ([recovery.md](recovery.md#the-guard)) |
| `refused: … already presents SR-IOV (sriov_totalvfs=…) from another kernel change (patched kernel or other livepatch)` | Another kernel change (a patched kernel or another livepatch) already presents SR-IOV on the card; `prepare` does not stack on it | Boot a kernel without that change (remove the other livepatch or patched kernel), then run `doctor` |
| `refused: … is the boot display device (boot_vga=1)` | The card shows the boot console | Use another display, or opt in: [The boot display GPU](boot-display.md) |
| `refused: … bound to driver 'nvidia'` at boot, or `doctor` warns that `nvidia` loads from `modules-load.d`, `/etc/modules` or the initramfs | Something loads `nvidia` before the boot unit | Remove `nvidia` from those lists and rebuild the initramfs, then reboot |
| `doctor`: `NVIDIA driver … the identity hook passes this version through unchanged` | The host driver version is not one the hook knows | Use a supported build (measured: 580.159.01). On other builds no vGPU types appear for the card |
| `status` shows `SR-IOV: not presented` | Not yet prepared this boot, or preparation failed | Follow the next step `status` prints; read `journalctl -b -u geforce-sriov-prepare.service` |
| `enable`: `not enough MMIO resources for SR-IOV`, or `status` says no VF BAR is assigned | The PCI bridge window above the card is too small | Enable *Above 4G decoding* in firmware or boot with `pci=realloc` |
| `enable`: `bound to nothing, not nvidia` | NVIDIA's host driver is not loaded | `modprobe nvidia` (or reboot), wait, run `enable` again |
| `modprobe nvidia` fails or loads nothing | A modprobe.d rule `install nvidia /bin/false` blocks it (for example the one from [The boot display GPU](boot-display.md#setting-it-up)) | `modprobe --ignore-install nvidia` |
| `Unit nvidia-vgpud.service not found` (or `nvidia-vgpu-mgr.service`) | NVIDIA's unit files are not installed | Install them: [NVIDIA's vGPU daemon units](installation.md#nvidias-vgpu-daemon-units) |
| VFs exist but no vGPU types appear | `nvidia-vgpud` started before the VFs, or the hook is off | Restart `nvidia-vgpud`, then `nvidia-vgpu-mgr`; check `identity=on` and the hook log lines |
| `nvidia-vgpud` fails with an assertion on `/run/geforce-sriov/deferred` | Boot-display grant: the cards have not been prepared yet this boot | Run `geforce-sriov prepare --release-console=…` first, then load `nvidia` and run `enable` |
| VM does not start, or start fails with an error about the vGPU type | Type does not fit the free memory, or VFs/daemons not ready | Check `geforce-sriov status`, pick a smaller type, restart the daemons in order |
| Windows guest shows **Code 43** | The guest has a bare-metal or Data Center driver, not the vGPU guest build | Install NVIDIA's vGPU guest driver from the same release as the host driver |
| Guest GPU work stalls | Stock `nvidia.ko` is loaded (DKMS rebuilt it after an update) | Re-apply the [driver patch](driver-patch.md) for this kernel |

More symptoms, each with the switch that addresses it, are in
[recovery.md](recovery.md#turn-features-off).

## Expected messages

These messages look like errors but appeared in the measured setup while everything worked. The
wording and line numbers of NVIDIA's and QEMU's messages can differ on other versions.

| When | Message | What it means |
|---|---|---|
| `doctor` before `install` | `[INFO] config: … absent` and `[WARN] kernel module for the running kernel not found …` | Nothing is installed yet |
| `doctor` before `install`, boot-display card | `[FAIL] … is the boot display device (boot_vga=1) …` | Expected until `install --boot-display-release` records the grant |
| `install` | DKMS `Sign command:`, `Signing key:` and `Public certificate (MOK):` lines | DKMS signs the module with its own key |
| Boot, boot-display path | The boot unit logs only `deferred: …` | The cards are prepared by hand after boot |
| `prepare` (kernel log) | `VF BAR … can't assign` | Transient during the rescan; `status` reports the final state |
| `enable` | `/usr/lib/nvidia/sriov-manage: line 148: echo: write error: No such device`, once per card | Printed by NVIDIA's script; `enable` judges the result from sysfs and still reports `4 VFs enabled and bound to nvidia` |
| `enable` (kernel log) | `pci-pf-stub 0000:03:00.0: driver left SR-IOV enabled after remove` | `sriov-manage` moves the card through `pci-pf-stub` on its way to `nvidia` |
| `enable` (kernel log) | `snd_hda_intel 0000:03:00.1: GPU sound probed, but not operational: please add a quirk to driver_denylist` | The card's HDMI audio function is left without a driver afterwards; vGPU does not use it |
| `enable` | `/dev/nvidia0`, `/dev/nvidiactl` … appear | `sriov-manage` runs `nvidia-smi -pm 1` (persistence mode) |
| [Step 7](installation.md#7-register-the-vgpu-types) | `nvidia-vgpud` shows `inactive (dead)` with `status=0/SUCCESS`; the hook lines appear twice | NVIDIA's unit is one-shot; starting `nvidia-vgpu-mgr` runs `nvidia-vgpud` again |
| `qm start` | `kvm: -device vfio-pci,host=0000:03:00.4,…: warning: vfio 0000:03:00.4: Could not enable error recovery for the device` | Printed by QEMU for each VF |
| `qm start` | `kvm: -device vfio-pci,host=0000:03:00.4,…: warning: VFIO dma-buf not supported in kernel: PCI BAR IOMMU mappings may fail` | Printed by QEMU for each VF |
| A guest driver loads | `nvidia-vgpu-mgr` journal: a few `error: vmiop_log: …` lines ending in `Failed to set guest license state in host` | Logged by NVIDIA's manager; licensing is configured separately and is outside this project |

## Where the logs are

- Boot unit: `journalctl -b -u geforce-sriov-prepare.service` (also printed on the console)
- Kernel module: `journalctl -k -b | grep geforce_sriov`
- Identity hook: `journalctl -b -u nvidia-vgpud.service | grep geforce-sriov-hook`
- NVIDIA daemons: `journalctl -b -u nvidia-vgpud.service -u nvidia-vgpu-mgr.service`
- Current state: `geforce-sriov status` and `geforce-sriov doctor`
