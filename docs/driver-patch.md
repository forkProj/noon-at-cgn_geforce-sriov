# The two-byte driver patch

## Why it exists

With NVIDIA vGPU-KVM host driver **580.159.01**, guest CUDA work can stall: a PMU-service callback
in NVIDIA's closed `nv-kernel.o_binary` calls a routine that sets the GPU's runlist
scheduler-disable register. Replacing the two-byte conditional jump (`jne`, bytes `75 76`) in front
of that call with two `NOP`s (`90 90`) makes the callback skip it. This is the driver change used
in the measured setup on the GA102 boards listed in [devices.md](devices.md).

It is a workaround: why the PMU path disables the runlist is unknown, and a clean run does not
prove the stall cannot return. It does not apply to GSP-based drivers, to NVIDIA's open kernel
modules, to guest drivers, or to Blackwell, and it changes no GSP firmware. On Ada the same two
bytes are one of two changes in the measured host module; the other, the Ada probe gate, is made
by the tool's `ada` commands ([Ada revisions](#ada-revisions)), and an Ada card needs both
([Ada host stack](ada.md#the-two-nvidiako-changes)).

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

## Ada revisions

The same tool has a second command family for the measured Ada stack ([Ada host stack](ada.md)).
Like the commands above, it reads a copy of **your own** file and writes only `NEWOUTPUT`; the
installed file is never touched.

```
geforce-sriov-patch ada list
geforce-sriov-patch ada inspect INPUT
geforce-sriov-patch ada apply INPUT NEWOUTPUT
geforce-sriov-patch ada revert PATCHED NEWOUTPUT
```

| Revision | Artifact (driver 580.159.01) | File offset | Bytes |
|---|---|---|---|
| Ada probe gate | host kernel object `nvidia/nv-kernel.o_binary` | `0xd654ef` | `75 7f` → `90 90` |
| Plugin change | vGPU plugin `libnvidia-vgpu.so.580.159.01` | `0x8fb0e` | `f8 06` → `00 5a` |

- `list` prints each revision: name, driver version, file offset, old and new bytes, and the input
  and output SHA-256. It reads no file.
- `inspect` names the revision whose input or output digest the file has, and ends its line with
  `file at input state` or `file at output state`. It writes nothing.
- `apply` writes the output state and refuses a file already at the output state. `revert` writes
  the input state from a file at the output state; it needs no stock copy.
- Each revision admits only the exact whole-file SHA-256 digests listed for it by `ada list`. Any
  other file is refused.
- `NEWOUTPUT` and the exit status follow the rules in [Usage](#usage).

### Order on the host object

The Ada host object carries both two-byte changes. Make them in this order:

1. `geforce-sriov-patch apply` on the stock object (the scheduler workaround).
2. `geforce-sriov-patch ada apply` on that output (the Ada probe gate).

Revert in the opposite order: `ada revert` first, then
`geforce-sriov-patch revert PATCHED STOCK NEWOUTPUT`. The order is fixed by the scheduler commands:
`inspect` and `apply` hash the whole object except their own two bytes, and `revert` requires the
pair to differ only in those two bytes, so all three refuse an object that carries the Ada probe
gate. `ada apply` also accepts the stock object and then writes a gate-only object (SHA-256
`ed3efee68582f4fa9f120cf6c28138d8642db93ff0e6ba07ab0d6d7bf1b9823b`); the scheduler commands refuse
that file, so undo it with `ada revert` and start again in the order above. Restoring the stock
module from your own backup also works ([Reverting](#reverting)).

### Digests

Check each step with `sha256sum FILE`:

| File | State | SHA-256 |
|---|---|---|
| Host kernel object | stock | `4e3abe645b6873ed478adea6a06c6955352ff06186849edeea536c9630596449` |
| Host kernel object | after `geforce-sriov-patch apply` | `ce8a485bfaeebca3a9609f9415a6df81c160a0a53509aec093bc3361a6d12701` |
| Host kernel object | after `ada apply` (both changes) | `f5b788cdb3e06aed18860cc6cf950dcc13143bd831d90d8e9c4bf9e744426f12` |
| vGPU plugin | stock | `7e52b76dc7ca8490e7ffbc1fd724f5db07f3791a3a2ec07f2d595bf05abdcaa7` |
| vGPU plugin | after `ada apply` | `097f571588f3a232265103b73e38ba111ddb44ef329ac720562cff71e6170d37` |

These are the digests the tool checks (`ada list`); a round trip in the order above on the user's
own copies produced exactly these digests. How the plugin copy is used is in
[The plugin change](ada.md#the-plugin-change).

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

For an Ada card, replace the `cp` line and the second `inspect` line above with these, before
`make` ([Order on the host object](#order-on-the-host-object)):

```sh
"$GS"/driver/geforce-sriov-patch ada apply /var/tmp/nv-kernel.o_binary.patched /var/tmp/nv-kernel.o_binary.ada
cp /var/tmp/nv-kernel.o_binary.ada nvidia/nv-kernel.o_binary
"$GS"/driver/geforce-sriov-patch ada inspect nvidia/nv-kernel.o_binary   # now: file at output state
```

The plain `inspect` refuses that file, because it carries both changes. For what the comparison
below shows on an Ada build, see the end of [Compare with a stock build](#compare-with-a-stock-build).

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

**Success (scheduler change only):** `cmp -l` prints exactly two lines, for two neighbouring
offsets, and exits 1 because the files differ. It prints bytes in octal: `165 220` and `166 220`
mean `75` → `90` and `76` → `90`. The measured build showed (your offsets may differ):

```text
10577673 165 220
10577674 166 220
```

For a module with the scheduler change alone, any other output means the build is not the expected
one; do not install it.

For an Ada module with both changes, `cmp -l` of the `.text` sections is expected to print four
lines: the two above, and two neighbouring lines `165 220` and `177 220` (`75 7f` → `90 90`) for
the Ada probe gate. Any other output means the build is not the expected one; do not install it.

Measured on whole module files (`cmp -l` of a stock 580.159.01 `nvidia.ko` and the Ada `nvidia.ko`,
without extracting `.text`): exactly four bytes differ. `cmp` positions are 1-based; the file
offsets are 0-based.

```text
10588529 165 220
10588530 166 220
14205144 165 220
14205145 177 220
```

| Change | File offsets in `nvidia.ko` | Bytes |
|---|---|---|
| Scheduler workaround | `0xa19170`, `0xa19171` | `75 76` → `90 90` |
| Ada probe gate | `0xd8c0d7`, `0xd8c0d8` | `75 7f` → `90 90` |

These are offsets in that module file, not in `.text` or in `nv-kernel.o_binary`; yours may differ.

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
   - on Ada, the `ada` revisions cover only the 580.159.01 files listed by `ada list`; any other
     build has no Ada probe-gate change.

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
For an object with both Ada changes, run `geforce-sriov-patch ada revert PATCHED NEWOUTPUT` first,
then `revert` on its output ([Order on the host object](#order-on-the-host-object)).
