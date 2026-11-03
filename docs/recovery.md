# Recovery

Read this before you need it.

## The one rule

**If a card disappears from `lspci` after preparation, or the host cannot be reached, power the
machine fully off and on (a cold power cycle).** Do not rescan the PCI bus again and do not
warm-reboot in a loop. Rescanning a bus whose GPU has already left it can take the kernel down
with it, and a GPU that stops answering config cycles comes back only after a full power-off. Keep
SSH access, and netconsole or a serial console if available, so you can read the logs remotely.

## The guard

`geforce-sriov prepare` writes `/var/lib/geforce-sriov/prepare-attempt` before it changes anything,
and removes it only when every selected card is back in a known state. If preparation is
interrupted (for example by a reset or a power loss) or a card does not come back, the file stays.
While it exists:

- the boot unit and `geforce-sriov prepare` refuse to run and exit with status **5**;
- `geforce-sriov enable` refuses to enable VFs (status 5);
- `geforce-sriov doctor` and `status` report it.

Every later boot is therefore an ordinary boot without preparation. To clear it:

1. Cold power cycle.
2. Boot and run `geforce-sriov doctor`; check that every selected card is present.
3. `geforce-sriov prepare --clear-guard`. It refuses (status 5) when the unfinished attempt was made
   in the current boot, so it cannot be cleared without a reboot in between. It can only see that
   the boot ID changed, not whether the reboot was a real power-off; making it one is up to you.
4. Decide what to change (below) before the next boot runs preparation again.

If preparation is refused after it had already unbound the card's HDMI audio function, it rebinds
that function to `snd_hda_intel` when the function is still present, unbound and the same device;
otherwise it prints `residual state: …` and leaves it alone (no rescan, no reset).

## The released console of the boot display GPU

`prepare --release-console=BDF` writes the guard before it unbinds the boot display's firmware
framebuffer (step `release-console:<BDF>`), and marks the card's removal step
`remove-rescan:<BDF>:console-released`. If it refuses or fails before that card's removal was
attempted, it rebinds the framebuffer to its driver, but only while the PF is the same unbound
device with the same resources and the framebuffer is still its unbound child; otherwise it prints
`residual state: …`. After the removal was attempted (whatever the remove write returned), it never
rebinds: the BARs may have moved, and the console stays released until the next boot. Nothing else
brings the console back during that boot; do not rebind it by hand.

While the boot unit has deferred (`/run/geforce-sriov/deferred` exists), `nvidia-vgpud` and the units
that require it refuse to start. The record is removed once every selected GPU is prepared, and it
does not survive a reboot. To stop deferring, run `geforce-sriov install --device BDF` for the other
GPUs only (without `--boot-display-release`): the next boot prepares them automatically and leaves
the boot display and its console alone.

## Skip preparation for one boot

Add `geforce_sriov.skip` to the kernel command line for that boot (in GRUB: press `e` on the entry
and append it to the `linux` line). The boot unit has `ConditionKernelCommandLine=!geforce_sriov.skip`
and does not run; nothing else changes.

## Turn features off

Each switch lives in `/etc/geforce-sriov/config` and is read by the next `prepare`:

| Symptom or wish | Change |
|---|---|
| VF enable or `nvidia-vgpu-mgr` start does not complete on your host | `ari=off` turns off the revealed ARI capability; it is the first switch to try. On **Blackwell** the ARI capability is the card's own (not presented by this package), so `ari=off` does not turn it off; add `pci=noari` to the kernel command line instead |
| `pci=noari` is on the kernel command line | nothing: `prepare` notices it and loads the module with `ari=0` |
| The kernel scans every slot below the port (`pci=pcie_scan_all`, or an x86 platform quirk) | nothing: ARI stays off automatically; SR-IOV and the VF count are unaffected |
| PF interrupts misbehave, or after any PF reset / AER recovery | `msix=off`; a PF reset re-hides MSI-X and silently stops its interrupts until the next cold boot |
| Ada or Blackwell card misbehaves with more than 1 VF | `experimental_vfs=off` (the default) presents the hardware's own count (1) |
| RTX 5090 with `twin_id=on`: NVIDIA's driver cannot start the GPU firmware, or the module does not load (`twin_id refused: the GPU is behind a hotplug-capable port`, or `twin_id refused: nvidia (or another driver) is bound to the GPU`) | `twin_id=off` (the default), then a cold boot |
| Stop the identity rewrite | `identity=off`; every ioctl then passes through unchanged |
| A selected card already shows `sriov_totalvfs` (patched kernel, or another livepatch) | nothing to do: `prepare` refuses and does not stack on it. If that other change already gives the VFs you need, skip preparation for that boot |
| VF enable fails after a vendor livepatch service (for example Ubuntu Livepatch) applied a cumulative "replace" livepatch | Such a patch silently disables `geforce_sriov`; VF enable then fails without harming the card. Disable the vendor livepatch service and reboot |
| `doctor` or `prepare` reports BAR0 unassigned, or a card came back from the rescan without BAR0 | The bridge windows above the card are too small. Do not rescan again; cold power cycle. Enable *Above 4G decoding* in the firmware or boot with `pci=realloc`, and skip preparation (`geforce_sriov.skip`) until BAR0 is assigned again |
| VF enable fails with "not enough MMIO resources for SR-IOV", or `doctor`/`status` report no VF BAR assigned | Same cause for the VF BARs: enable *Above 4G decoding* or boot with `pci=realloc`. The package never resizes the windows of a switch or of bridges further up |
| Kernel log `geforce_sriov: first VF offset … ; refusing` (module not loaded) or `VF layout for … requested VFs … SR-IOV is not set up on this PF` | The card's VF layout does not fit its own slot, or its NumVFs register does not take the kernel's value. If you set `vfs` above the default of 4, set it back to 4 or lower, or remove the `vfs` line, and reboot. Otherwise no setting changes it; remove the card from `bdfs` |
| You want fewer VFs per card | `vfs=N` (1–4; or `geforce-sriov install --vfs N`), then a cold boot: the module's count is fixed when it loads. `enable` then enables that count |
| Kernel log `unexpected VF layout after SR-IOV setup` | The card reported another layout at VF enable than at setup. Disable the VFs, then cold power cycle; do not rely on that card's VFs |
| One of several selected cards is refused, so nothing is prepared | `prepare` checks every selected card first and changes nothing when one fails. Fix that card, or remove it from `bdfs` to prepare the others |
| `status` exits 1 and shows `SR-IOV: not presented` after install | Nothing is wrong: this is expected until the first boot that prepares the card. Follow the next step `status` prints (usually `doctor`, then reboot) |

The same four kernel switches can be given once on the command line as
`geforce-sriov prepare --ari=off --msix=off --experimental-vfs=off --twin-id=off`, or recorded at
install time with the same options on `geforce-sriov install` (which also takes `--vfs N`). The
module's parameters are fixed at load time; to change them, reboot.

## Remove the package

```sh
sudo geforce-sriov uninstall
```

Steps, in this order:

1. With `--unload`, the module is unloaded first: the livepatch is disabled, its transition awaited,
   then `rmmod geforce_sriov`. This is refused, and **nothing is removed**, while a GPU named in
   the module's `bdfs` or the config has active VFs, when the loaded module's `bdfs` parameter cannot
   be read or parsed (so active VFs on its GPUs cannot be ruled out), or when a GPU was prepared
   in this boot; reboot instead. Without `--unload` the livepatch stays loaded until the next boot, which is then an
   ordinary boot.
2. `geforce-sriov-prepare.service` is disabled and the DKMS module removed.
3. Only the files it installed whose SHA-256 is unchanged are deleted; it prints
   `kept … (changed since install)` for the others, including an edited `/etc/geforce-sriov/config`.
4. The guard `/var/lib/geforce-sriov/prepare-attempt`, if set, is **kept**: an unfinished
   preparation still needs a cold power cycle, and `geforce-sriov prepare --clear-guard` clears it
   afterwards. A later `install` warns while it is set.

## What not to do

- Do not unload `nvidia`, `nvidia_vgpu_vfio` or `geforce_sriov` while VFs exist.
- With `twin_id=on`, do not unload `geforce_sriov` while `nvidia` is bound to the card: after a
  reset the driver compares the card's ID with the one it read when it started the GPU.
- Do not write `sriov_numvfs` by hand: NVIDIA requires its `sriov-manage` script, and the VF count
  must stay within the presented count.
- Do not reset the PF (`/sys/bus/pci/devices/<PF>/reset`) with MSI-X in use.
- Do not unbind the boot display's framebuffer, remove the card or rescan its bus yourself. Without a
  console-release grant the package refuses the boot display GPU; with one, only
  `prepare --release-console=BDF` releases its console, after its own checks.
- Do not treat IOMMU groups split by an ACS override as isolation between guests: it changes the
  kernel's grouping, not what the hardware can reach.
