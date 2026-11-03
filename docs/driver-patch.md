# The two-byte driver patch

## Why it exists

With NVIDIA vGPU-KVM host driver **580.159.01**, guest CUDA work can stall: a PMU-service callback
in NVIDIA's closed `nv-kernel.o_binary` calls a routine that sets the GPU's runlist
scheduler-disable register. Replacing the two-byte conditional jump (`jne`, bytes `75 76`) in front
of that call with two `NOP`s (`90 90`) makes the callback skip it. This is the driver change used
in the measured setup on the GA102 boards listed in [devices.md](devices.md).

It is a workaround: why the PMU path disables the runlist is unknown, and a clean run does not
prove the stall cannot return. It does not apply to GSP-based drivers, to NVIDIA's open kernel
modules, to guest drivers, or to Ada and Blackwell, and it changes no GSP firmware.

This repository contains no NVIDIA file. `geforce-sriov-patch` changes a copy of **your own**
`nv-kernel.o_binary`; you rebuild NVIDIA's kernel module from NVIDIA's source tree yourself. The tool
never builds, installs or loads a module.

## Supported inputs

| Driver | `nv-kernel.o_binary` of the vGPU-KVM host package | Status |
|---|---|---|
| 580.159.01 | accepted | Measured: `nvidia.ko` rebuilt from this tool's output ran the tested setup (GA102 `10de:2206`, subsystem `10de:146d`, kernel `7.0.14-16-pve`) |
| 595.91.04 | accepted | Untested candidate: a patched 595.91.04 driver has not been run |
| anything else | refused | |

The tool has been tested offline on the stock 580.159.01 and 595.91.04 host objects: `apply`
changes exactly the two bytes and `revert` restores the stock file. For 580.159.01, an `nvidia.ko`
rebuilt from its output was built against kernel `7.0.14-16-pve` and ran on hardware in the
measured setup ([What has been verified](../README.md#what-has-been-verified)); its `.text` is
identical to that of a module carrying the same two-byte change made by other means, differing only
in build ID and debug path data. No other kernel or driver version has been run.

An input is accepted only when all of these hold: it is a well-formed little-endian x86-64 ELF
relocatable object; the named callback, scheduler-writer and installer functions for that driver
version exist exactly once in `.text` with the expected sizes; exactly one copy of the instruction
signature exists in the whole `.text` and lies inside the callback; the surrounding branch, call and
relocation anchors match; and the SHA-256 of the file, with the two patch bytes taken as stock,
equals the recorded hash for that version. Anything else is refused with the reason: another
version, the open or GSP flavour, a guest driver object, a truncated or otherwise modified file.

## Usage

```
geforce-sriov-patch inspect INPUT
geforce-sriov-patch apply INPUT NEWOUTPUT
geforce-sriov-patch revert PATCHED STOCK NEWOUTPUT
geforce-sriov-patch --help | --version
```

After `make`, the tool is `driver/geforce-sriov-patch` in this repository. It is not on your `PATH`
until `geforce-sriov install` copies it to `/usr/local/bin`, which in the
[installation guide](installation.md) comes after this
step, so the commands below use the built copy.

- `inspect` prints the driver version, whether the file is `stock` or `already-patched`, whether the
  patch is the measured one (`stall workaround`) or an `untested` one, the function name and the
  file offset of the two bytes. It writes nothing.
- `apply` writes a new file that differs from `INPUT` in exactly those two bytes. It refuses an
  already-patched input.
- `revert` checks that `PATCHED` and `STOCK` are both accepted inputs of the same version, that
  `PATCHED` is patched and `STOCK` is not, and that they are identical except for the two bytes; it
  then writes `STOCK` to `NEWOUTPUT`.
- `NEWOUTPUT` must not exist: it is created exclusively (`O_EXCL`, never following a symlink), with
  mode `0644` reduced by the umask, written completely and `fsync`ed, and its directory is
  `fsync`ed. Inputs are never modified.
- If writing `NEWOUTPUT` fails, the tool tries to remove the partial file and prints what happened
  to it; when that removal itself fails (or the name now refers to another file), the error says so
  and the file may remain. If the directory cannot be `fsync`ed, the output is complete but the tool
  warns that the new name may not survive a power loss.
- If `apply` or `revert` cannot write its own messages to standard output, it exits 1 and says on
  standard error that the output file was already created: that file is complete.
- Exit status: 0 success, 1 refused or failed (the reason is printed on standard error), 2 usage.

## Rebuilding NVIDIA's module

The NVIDIA vGPU-KVM host package installed with DKMS places its kernel source in
`/usr/src/nvidia-<version>` (for example `/usr/src/nvidia-580.159.01`), with the closed object at
`nvidia/nv-kernel.o_binary`. Build the patched module from a copy of that tree; adapt the paths,
and keep the stock tree untouched.

```sh
GS=$HOME/geforce-sriov       # this repository, after `make`; adapt
VER=580.159.01
cp -a /usr/src/nvidia-$VER /var/tmp/nvidia-$VER-patched
cd /var/tmp/nvidia-$VER-patched

"$GS"/driver/geforce-sriov-patch inspect nvidia/nv-kernel.o_binary
"$GS"/driver/geforce-sriov-patch apply nvidia/nv-kernel.o_binary /var/tmp/nv-kernel.o_binary.patched
cp /var/tmp/nv-kernel.o_binary.patched nvidia/nv-kernel.o_binary
"$GS"/driver/geforce-sriov-patch inspect nvidia/nv-kernel.o_binary   # now: already-patched

make -j"$(nproc)" NV_EXCLUDE_BUILD_MODULES= KERNEL_UNAME="$(uname -r)" modules
```

On the measured driver the first `inspect` prints:

```text
580.159.01: stock; stall workaround; function _nv052453rm; file offset 0x9f2588
```

The build writes `nvidia.ko` and `nvidia-vgpu-vfio.ko` to the top of the copied tree, for example
`/var/tmp/nvidia-580.159.01-patched/nvidia.ko`. Only `nvidia.ko` contains the patched object;
leave the installed `nvidia-vgpu-vfio.ko` as it is. The build also prints a list of `objtool`
warnings such as `nvidia.o: warning: objtool: _nv000847rm+0x9: call without frame pointer
save/setup`. They concern functions in NVIDIA's closed object, not the two bytes, and appeared in
the measured build, whose module ran the tested setup.

### Compare with a stock build

Build an unpatched copy of the same tree the same way, then compare the code (`.text`) of the two
modules:

```sh
cp -a /usr/src/nvidia-$VER /var/tmp/nvidia-$VER-stock
cd /var/tmp/nvidia-$VER-stock
make -j"$(nproc)" NV_EXCLUDE_BUILD_MODULES= KERNEL_UNAME="$(uname -r)" modules
objcopy -O binary -j .text /var/tmp/nvidia-$VER-stock/nvidia.ko /var/tmp/stock.text
objcopy -O binary -j .text /var/tmp/nvidia-$VER-patched/nvidia.ko /var/tmp/patched.text
cmp -l /var/tmp/stock.text /var/tmp/patched.text
```

**Success:** `cmp -l` prints exactly two lines, for two neighbouring offsets, and exits 1 because
the files differ. It prints bytes in octal: `165 220` and `166 220` mean `75` → `90` and `76` →
`90`. The measured build showed (your offsets may differ):

```text
10577673 165 220
10577674 166 220
```

Any other output means the build is not the expected one; do not install it.

### Install the patched module

Do this while `nvidia` is not in use (before the reboot of
[installation step 5](installation.md#5-reboot-and-verify)). Back up the installed
stock module first:

```sh
M=$(modinfo -n nvidia)        # usually /lib/modules/<kernel>/updates/dkms/nvidia.ko
cp -a "$M" /root/nvidia.ko.stock-"$(uname -r)"
cp /var/tmp/nvidia-$VER-patched/nvidia.ko "$M"
sha256sum "$M" /var/tmp/nvidia-$VER-patched/nvidia.ko   # both sums must be equal
```

- Keep the backup **outside** `/lib/modules`, so `depmod` and `modprobe` never pick it up.
- Replacing the file under the same name needs no `depmod`. If `modinfo -n` names a compressed
  file (`nvidia.ko.xz`, `nvidia.ko.zst`), do not run the two `cp` lines as written: move that file
  to your backup location, copy the patched module into the same directory as `nvidia.ko`, and run
  `depmod <kernel>`.
- `nvidia` is not in the initramfs (an [installation prerequisite](installation.md#1-prerequisites-checklist)),
  so the initramfs needs no rebuild.
- `dkms status` now reports `Differences between built and installed modules` for `nvidia`. That is
  expected: the installed file is no longer the one DKMS built.
- **DKMS rebuilds the stock module** after a kernel update or a DKMS rebuild and can silently
  overwrite the patched file. Check the installed module's SHA-256 (or the loaded module's build ID
  in `/sys/module/nvidia/notes/.note.gnu.build-id`) after every kernel or DKMS change.
- The patched module is built for one kernel; rebuild it for each kernel you boot.
- One driver serves every GPU in the host: the patched module applies to all of them.
- With Secure Boot on, the rebuilt module must be signed with an enrolled key; this guide does not
  cover that (the measured host ran with Secure Boot off).

## After a kernel or NVIDIA driver update

The patch is never applied automatically. After any update:

1. **New kernel, same driver.** DKMS builds a stock `nvidia.ko` for the new kernel. Repeat the
   rebuild above with `KERNEL_UNAME` set to the new kernel and install the result for that kernel
   before you boot it.
2. **New NVIDIA driver version.** Reinstalling or upgrading the host package installs a stock
   module. Run `geforce-sriov-patch inspect` on the new `nv-kernel.o_binary`:
   - if it is accepted, repeat the rebuild from the new `/usr/src/nvidia-<version>` tree;
   - if it is refused, there is no patch for that build: keep the stock module (the stall can
     return) or stay on a supported build. Do not patch it by hand.

   Once the new driver is loaded, `geforce-sriov doctor` also prints whether the identity hook
   supports that build; on any other build the hook passes every call through.
3. **Replace the module only while no VF exists.** The simplest safe order is to install the
   patched file and reboot: the boot prepares the cards before `nvidia` loads, and `enable` then
   runs on the patched module.

## Reverting

Reinstall your backup of the stock `nvidia.ko` (copy it back to the path `modinfo -n nvidia`
printed), or rebuild from the untouched `/usr/src/nvidia-<version>` tree. To restore a stock
object from a patched one when you also have the stock file,
`geforce-sriov-patch revert PATCHED STOCK NEWOUTPUT` verifies the pair first.
