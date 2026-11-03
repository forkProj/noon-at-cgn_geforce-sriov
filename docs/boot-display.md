# The boot display GPU

The card that shows the firmware boot screen and the Linux text console is the *boot display*.
Check a card with `cat /sys/bus/pci/devices/0000:03:00.0/boot_vga` (`1` means boot display).
On a server whose board has its own BMC graphics, the GPU is usually not the boot display. The
[README](../README.md#which-path-do-i-need) shows which path applies to your host.

This page is the boot-display path. Everything not mentioned here follows the
[installation guide](installation.md).

## Why it is refused by default

NVIDIA documents display rules for the professional cards its vGPU software supports. The
[vGPU 19.0 user guide](https://docs.nvidia.com/vgpu/19.0/grid-vgpu-user-guide/grid-vgpu-introduction.html#primary-display-adapter-requirements)
states: "The GPU that is set as the primary display adapter cannot be used for NVIDIA vGPU
deployments". The
[Display Mode Selector guide](https://developer.download.nvidia.com/display-mode-selector-tool/NVIDIA-Display-Mode-Selector-Tool-User-Guide_v08_Feb24.pdf)
describes switching workstation boards such as the RTX A6000 to a mode with physical display ports
disabled for vGPU. GeForce cards are not covered by either document. Separately, removing and
rescanning the card moves the memory the firmware console draws into. So by default **every
command refuses the boot display card**.

## What the opt-in changes

You can opt in. Then, after each boot, the Linux text console on that card is detached before the
card is prepared:

> Console release: the Linux console on that GPU stays detached until the next boot (the kernel
> switches to a dummy console; the monitor may keep the last frame or lose signal). Logs continue
> in the journal and over SSH; starting and stopping guests does not touch the display.

SSH, the Proxmox web UI and your VMs are unaffected.

### No host desktop on the same GPU

**A desktop on the host using the same GPU is not supported**: the card must stay free of display
drivers for preparation, and the vGPU host package has no desktop graphics stack to give it one
afterwards. For a local console, use an integrated GPU or a cheap second card.

## Setting it up

**1. Grant the release at install time**, together with any other cards:

```sh
sudo ./geforce-sriov install --device 0000:04:00.0 --boot-display-release 0000:04:00.0
# with a second card: --device 0000:03:00.0 --device 0000:04:00.0 --boot-display-release 0000:04:00.0
```

**2. Keep `nvidia` from loading at boot.** The boot unit now *defers*: it prepares **none** of the
selected cards and holds `nvidia-vgpud` back. You must make sure nothing binds the cards first:
keep the `nvidia` module from autoloading and keep `nvidia-persistenced`, `nvidia-vgpu-mgr` and
similar services from starting (leave NVIDIA's vGPU units disabled). The package does not change
module blacklists or services for you. One way is a file such as
`/etc/modprobe.d/nvidia-manual-load.conf`:

```text
blacklist nvidia
install nvidia /bin/false
```

`blacklist` stops udev from loading the module; the `install` line makes every plain
`modprobe nvidia` run `/bin/false` instead, so nothing else loads it either.

**3. After each boot, over SSH, as root,** in this order:

```sh
geforce-sriov doctor
geforce-sriov prepare --release-console=0000:04:00.0
modprobe --ignore-install nvidia    # plain `modprobe nvidia` without the rule above
geforce-sriov status                # each card: driver nvidia, next: `sudo geforce-sriov enable`
```

`--release-console` must name exactly the cards granted in the config. `prepare` checks every card
first and refuses (changing nothing) if the console is not the card's own simple framebuffer or
anything else still uses the card.

`modprobe --ignore-install` skips the `install` rule for this one command. Then continue with
[step 6](installation.md#6-enable-the-virtual-functions) (`enable`),
[step 7](installation.md#7-register-the-vgpu-types) (daemons) and
[step 8](installation.md#8-create-a-vm) of the installation guide, in that order.

## Expected output

**`doctor` before `install`.** A boot-display card shows
`[FAIL] 0000:04:00.0: is the boot display device (boot_vga=1) …`. It goes away once `install`
records the console-release grant.

### What the boot unit logs

The boot unit prepares nothing on this path. `systemctl status geforce-sriov-prepare.service` still
shows `active (exited)`, but `journalctl -b -u geforce-sriov-prepare.service` shows only this (with
your addresses):

```text
deferred: boot_display_release=0000:04:00.0 grants a console release, so the boot unit prepares none of
the selected GPUs (0000:04:00.0) and changes nothing; nvidia-vgpud is held back until they are prepared.
Next, after boot, as root: `geforce-sriov doctor`, then `geforce-sriov prepare --release-console=0000:04:00.0`
```

`status` then prints `boot unit: deferred this boot …` and `SR-IOV: not presented`, and exits 1.

### What `prepare` prints

The lines the boot unit logs on the standard path
([step 5](installation.md#5-reboot-and-verify)) appear in the output of `prepare` instead. It ends
with:

```text
every selected GPU is prepared: the boot unit's deferral is over (/run/geforce-sriov/deferred removed).
Next: load NVIDIA's host driver, run `geforce-sriov enable`, then start nvidia-vgpud and nvidia-vgpu-mgr
```

Until NVIDIA's driver is loaded, `status` shows the cards prepared with `driver none` and
`next: NVIDIA's vGPU host driver must bind the PF`; in the measured run the cards were bound as
soon as `modprobe` returned.

## Returning to automatic preparation

To return to automatic preparation, run `install --device …` again with only the non-display
cards. The next boot prepares them automatically and leaves the boot display and its console alone.

## If something goes wrong

- `prepare` refuses or fails: what it gives back and what it leaves released is described in
  [recovery.md](recovery.md#the-released-console-of-the-boot-display-gpu). Do not rebind the
  console or rescan the card by hand.
- A card does not come back, or the host cannot be reached: follow
  [the one rule](recovery.md#the-one-rule) (cold power cycle).
- `nvidia-vgpud` refuses to start, or `modprobe nvidia` loads nothing: see
  [Troubleshooting](troubleshooting.md).

Full details of the console release (framebuffer checks, the deferral record, `enable` rules):
[technical-reference.md](technical-reference.md#the-boot-display-gpu).
