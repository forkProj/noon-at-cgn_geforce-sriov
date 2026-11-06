# Configuration and commands

Reference for the configuration file, the commands, the systemd units and the kernel module's
parameters. The exact parsing and validation rules are in
[technical-reference.md](technical-reference.md#configuration-rules), and the complete option list
of every command in [technical-reference.md](technical-reference.md#commands).

## Configuration file

`/etc/geforce-sriov/config` (or the path in `$GEFORCE_SRIOV_CONFIG`). One `key=value` per line,
`#` comments, no spaces. It must be owned by root and not writable by group or others. An annotated
copy is installed as `/usr/local/share/geforce-sriov/config.example`
([config/config.example](../config/config.example)).

| Key | Values | Default | Meaning |
|---|---|---|---|
| `bdfs` | 1–8 card addresses, comma-separated | none | The cards that `prepare`, `enable` and the hook act on |
| `boot_display_release` | addresses, each also in `bdfs` | none | Boot display cards whose console `prepare --release-console` may release; the boot unit then defers ([The boot display GPU](boot-display.md)) |
| `ari` | `on`/`off` | `on` | Enable ARI in software: reveal the card's hidden ARI capability and turn on ARI forwarding in the bridge above it. ARI decoding in the GPU hardware does not work for now, which is why `vfs` defaults to 4 either way. `pci=noari` on the kernel command line forces it off |
| `msix` | `on`/`off` | `on` | Reveal the card's MSI-X interrupts (Ampere) |
| `experimental_vfs` | `on`/`off` | `off` | Ada/Blackwell only, for experiments: where the card reports 1 VF, present up to `vfs` (at most 4); `off` presents the card's own 1 VF. On Blackwell the raised count also reaches NVIDIA's driver, and the GPU firmware may still refuse more than 1 VF (untested); on Ada the driver still sees 1, and on the one measured Ada card only the first VF reached a vGPU ([Ada and Blackwell](devices.md#ada-and-blackwell)) |
| `twin_id` | `on`/`off` | `off` | RTX 5090 (`10de:2b85`) only, experimental and untested: show NVIDIA's driver the RTX PRO 6000 Blackwell Server Edition ID when it starts the GPU. It may make the GPU firmware fail to start ([twin_id](devices.md#twin_id-rtx-5090-only)). `doctor` fails and `prepare` refuses when no selected card is an RTX 5090; on other cards it has no effect |
| `vfs` | 1 up to the card's hardware default (`geforce-sriov devices` lists it) | `4` | VFs presented per selected card; `install --vfs N` writes it. The default is 4 because ARI decoding in the GPU hardware does not work for now. Ada and Blackwell present 1 unless `experimental_vfs=on` |
| `identity` | `on`/`off` | `off` (install sets `on`) | Let the hook present the selected cards as their professional counterpart |
| `identity_force_host` | `on`/`off` | `off` (install sets `on`) | Hook also reports vGPU host mode for the selected cards |
| `sriov_manage` | absolute path | `/usr/lib/nvidia/sriov-manage` | NVIDIA's VF tool used by `enable` |

`vfs` applies to every selected card. A value above a selected card's hardware default (on
Ada/Blackwell with `experimental_vfs=on`, 4) is refused by `install`, `doctor` and `prepare`, and
they warn about a value above 4. The module's switches and VF count are fixed when it loads; to
change them, edit the file (or run `install` again) and reboot. As for the first prepared boot,
power the machine fully off and on.

Which switch to turn off for which symptom is listed in
[recovery.md](recovery.md#turn-features-off).

## Commands

| Command | Root | What it does |
|---|---|---|
| `geforce-sriov devices` | no | Print the device table |
| `geforce-sriov doctor [--device BDF]...` | no | Checks the host and cards and shows what `prepare` would do; changes nothing |
| `geforce-sriov status [--device BDF]...` | no | State of each selected card and the next step; exit 1 until all are ready |
| `geforce-sriov install --device BDF... [--boot-display-release BDF] [--vfs N] [--prefix PATH] [--ari=on\|off] [--msix=on\|off] [--experimental-vfs=on\|off] [--twin-id=on\|off]` | yes | Install, select cards, set the VF count, enable the boot unit |
| `geforce-sriov prepare [--release-console=BDF]...` | yes | Prepare the selected cards now (normally the boot unit does this) |
| `geforce-sriov prepare --clear-guard` | yes | Clear the guard of an unfinished preparation, only after a power cycle |
| `geforce-sriov enable` | yes | Create the VFs with `sriov-manage` |
| `geforce-sriov activate [--device BDF]... [--type N] [--dry-run]` | yes | Ada only, after `enable`: restart NVIDIA's two vGPU daemons, re-probe each Ada card, optionally set vGPU type `N`, power its VFs on and read that back ([Ada host stack](ada.md#bring-up-order)) |
| `geforce-sriov check [--device BDF]... [--type N]` | no | Ada only, before every VM start: exit 0 when each VF is on `nvidia` with `power/control=on`, `runtime_status=active` (and type `N`), 1 otherwise or while an `activate` is running or did not finish, 3 for an invalid or empty config, no selected Ada card, a `--device` that is not selected or not an Ada card, or a selected card that is missing. Only the first VF of each card (`virtfn0`) can be ready; every further VF is `NOT READY`, so with `experimental_vfs=on` and `vfs` above 1 (the default is 4) it always exits 1 ([The VF power gate](ada.md#the-vf-power-gate)) |
| `geforce-sriov uninstall [--unload]` | yes | Remove what `install` put there |
| `geforce-sriov-patch inspect\|apply\|revert …`, `geforce-sriov-patch ada list\|inspect\|apply\|revert …` | no | Driver patcher ([driver-patch.md](driver-patch.md), [Ada revisions](driver-patch.md#ada-revisions)) |

Exit codes: 0 ok, 1 failed, 2 usage error, 3 refused (nothing changed), 4 another
`geforce-sriov` is running, 5 guard set (cold power cycle needed). The complete option list
is in [technical-reference.md](technical-reference.md#commands).

## Systemd units

| Unit | When it runs | What it does |
|---|---|---|
| `geforce-sriov-prepare.service` | Once per boot, before `systemd-udevd`, module autoloading and NVIDIA's services | Runs `geforce-sriov prepare --boot`. Skipped when the config is missing or the kernel command line has `geforce_sriov.skip`. With a boot-display grant it changes nothing, writes `/run/geforce-sriov/deferred` and exits 0 |
| `nvidia-vgpud.service.d/geforce-sriov.conf` (drop-in) | Whenever `nvidia-vgpud` starts | Loads the hook into `nvidia-vgpud` only (`LD_PRELOAD`), orders it after the boot unit, and refuses to start while `/run/geforce-sriov/deferred` exists |

`nvidia-vgpud.service` and `nvidia-vgpu-mgr.service` themselves are NVIDIA's units, not part of
this package ([NVIDIA's vGPU daemon units](installation.md#nvidias-vgpu-daemon-units)).

## Module parameters

The `geforce_sriov` kernel module takes these parameters. You do not normally set them yourself:
`prepare` loads the module with `bdfs` and the switches from the configuration file (as `0`/`1`),
and with `vfs=N` when the file sets a VF count. They are read-only once the module is loaded
(`/sys/module/geforce_sriov/parameters/`), and `geforce-sriov status` prints them.

| Parameter | Type | Default | Meaning |
|---|---|---|---|
| `bdfs` | comma-separated PFs `dddd:bb:ss.0` | none (required) | The cards that get any change, at most 8, as named in `/sys/bus/pci/devices`. Empty refuses to load. Set from the config key `bdfs` |
| `vfs` | 1–16 | `4` | VFs presented per listed card, at most the card's hardware TotalVFs. ARI decoding in the GPU does not work for now, so more than 4 VFs are expected not to work (the module refuses to load if the card's VF layout check fails). Set from the config key `vfs` |
| `ari` | `0`/`1` | `1` | Present the card's hidden ARI capability and enable ARI forwarding on its upstream bridge. The kernel's `pci=noari` is not visible to the module; `prepare` passes `ari=0` with it. Set from the config key `ari` |
| `msix` | `0`/`1` | `1` | At load, relink the card's hidden MSI-X capability where its hide register is known (Ampere). Set from the config key `msix` |
| `experimental_vfs` | `0`/`1` | `0` | Experimental: on Ada/Blackwell, where the card reports 1 VF, present up to `vfs` (at most 4) instead of 1; 0 stays 0. On Blackwell NVIDIA's driver also sees the raised count, and the GPU firmware may still refuse more than 1 VF (untested); on Ada only Linux sees it, and on the one measured Ada card only the first VF reached a vGPU. Set from the config key `experimental_vfs` |
| `twin_id` | `0`/`1` | `0` | Experimental, untested: answer in-kernel 32-bit reads of config dwords `0x00` and `0x2C` of the listed RTX 5090 cards with the vGPU target's ID and subsystem. It may make the GPU firmware fail to start. Refused when none of the listed cards is an RTX 5090, or when a listed RTX 5090 is behind a hotplug-capable port or has a driver bound; on other listed cards it has no effect. Set from the config key `twin_id` |

What the module does with each parameter, and when it refuses to load, is described in
[technical-reference.md](technical-reference.md#the-kernel-module).
