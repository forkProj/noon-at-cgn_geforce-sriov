# Device table

The device table is `include/gs_devices.h`; the kernel module, the CLI and the identity hook all
read it. `geforce-sriov devices` prints it from the installed binary. This page is a readable copy
with the status of each row; if the two ever disagree, the header wins. A listed ID means the tools
will act on that card, not that it works.

Every entry is one exact PCI device ID (vendor `10de`). There are no ranges. A card whose ID is not
listed (Turing, laptop parts, professional cards, anything unreleased) is left alone by every
component.

## Status of every row

- **Supported** (status `supported` in `geforce-sriov devices`): only device `10de:2206`, the only
  row with `experimental = 0`. It has been measured with this package on two memory-modded
  RTX 3080 20GB cards with subsystem `10de:146d` (scope: [What has been verified](../README.md#what-has-been-verified));
  a stock `2206` RTX 3080 has 10 GB. Other boards that share the ID `2206` are untested.
- **Experimental** (status `experimental`): every other row. The capability offsets come from
  public raw config-space dumps of cards with that ID; they show where the capability is, not that
  SR-IOV or vGPU works there. The kernel module re-reads and validates each header on the running
  card and does nothing when it does not match.
- **Cross-die rows** (`same die = no`) have no professional SKU of their own die in NVIDIA's
  `vgpuConfig.xml`. They are mapped by a heuristic to the same-generation professional
  target whose largest vGPU profile is nearest a mid-range GeForce framebuffer; the choice is
  unverified. Every tool prints: `No vGPU target on the same die; support is experimental.`
  Whether NVIDIA's daemon and driver accept another die's profile table is unknown.
- **Ada and Blackwell** GeForce cards report TotalVFs = 1, and NVIDIA's driver reads that count
  from the hardware itself. geforce-sriov therefore presents the card's own count, 1 VF, by
  default. `experimental_vfs=on` is an opt-in for experiments only: where the hardware value is
  exactly 1 it presents up to 4 (the configured `vfs`, at most 4); a value of 0 stays 0. On
  Blackwell this also raises the count NVIDIA's driver sees; on Ada the driver still sees 1, so
  only Linux shows more VFs. On Blackwell this is untested, and the GPU firmware may still refuse
  more than 1 VF; on the one measured Ada card only the first VF reached a vGPU
  ([Ada host stack](ada.md)). The module refuses the load outright if the card's VF offset or
  stride would place a VF outside functions 1–7. Details: [Ada and Blackwell](#ada-and-blackwell).
- **Profile memory is not filtered.** The target's vGPU profile table is presented unchanged; it
  lists the target's profiles, not your card's capacity, so a card can be offered profiles larger
  than its own memory (for example a 10–12 GB GA102 card presented as the 48 GB RTX A6000). No
  automatic VRAM check exists. Choose profiles yourself so that all active vGPUs together, plus the
  driver's own overhead, fit the card's actual memory, not just each profile on its own.

Exposing SR-IOV is not the same as a working vGPU.

Have a card from this table? Whether it works or not, please submit a
[compatibility report](https://github.com/noon-at-cgn/geforce-sriov/issues/new?template=compatibility-report.yml).
An `experimental` row changes status only when someone with that card reports a working setup and
the maintainers confirm it ([what happens to a report](compatibility.md#what-happens-to-a-report)).

## Different cards in one host

Every selected card is looked up by its own device ID, so different rows can be selected together
(up to 8 cards), and identical cards can be selected in part. The identity hook presents each
selected card under its own row's target and leaves every other GPU as it is; how it tells
identical cards apart is described in [the technical reference](technical-reference.md#the-identity-hook).
Only two identical `2206` cards, together and with one of them selected, have been measured.

All selected cards share the one installed NVIDIA vGPU-KVM host driver. A row in this table does not
mean that a given driver build supports that card, or supports it together with the others: each
card's vGPU target must be supported by the same driver build and flavour. The driver patch exists
only for the builds in [driver-patch.md](driver-patch.md), and does not apply to GSP-based drivers
or NVIDIA's open kernel modules; on its own it does not make a driver that runs Ada
([Ada host stack](ada.md#the-two-nvidiako-changes)) or Blackwell. Blackwell needs the open
flavour and Ampere was tested with the proprietary one
([NVIDIA driver flavour](#nvidia-driver-flavour)), so an Ampere and a Blackwell card cannot share
one host with geforce-sriov today. `geforce-sriov activate` restarts NVIDIA's daemons for the whole
host, so it refuses while a GPU outside `bdfs` has VFs enabled; VFs of a selected Ampere card are
allowed, but an Ampere and an Ada card under the one modified Ada module have not been run. Host
layouts (switches, shared buses, IOMMU groups, BAR space) are covered in
[the technical reference](technical-reference.md#multiple-gpus-and-pci-topology).

## Rows

| Device | Die | GeForce product | vGPU target | Target subsystem | Same die | Generation | Status |
|---|---|---|---|---|---|---|---|
| `2203` | GA102 | RTX 3090 Ti | `2230` RTX A6000 | any | yes | Ampere | experimental |
| `2204` | GA102 | RTX 3090 | `2230` RTX A6000 | any | yes | Ampere | experimental |
| **`2206`** | GA102 | RTX 3080 (measured: memory-modded 20GB, subsystem `10de:146d` only) | `2230` RTX A6000 | any | yes | Ampere | supported |
| `2208` | GA102 | RTX 3080 Ti | `2230` RTX A6000 | any | yes | Ampere | experimental |
| `220a` | GA102 | RTX 3080 12GB | `2230` RTX A6000 | any | yes | Ampere | experimental |
| `2216` | GA102 | RTX 3080 LHR | `2230` RTX A6000 | any | yes | Ampere | experimental |
| `2482` | GA104 | RTX 3070 Ti | `25b6` A2 | `10de:157e` | **no** | Ampere | experimental |
| `2484` | GA104 | RTX 3070 | `25b6` A2 | `10de:157e` | **no** | Ampere | experimental |
| `2486` | GA104 | RTX 3060 Ti | `25b6` A2 | `10de:157e` | **no** | Ampere | experimental |
| `2487` | GA104 | RTX 3060 | `25b6` A2 | `10de:157e` | **no** | Ampere | experimental |
| `2488` | GA104 | RTX 3070 LHR | `25b6` A2 | `10de:157e` | **no** | Ampere | experimental |
| `2489` | GA104 | RTX 3060 Ti LHR | `25b6` A2 | `10de:157e` | **no** | Ampere | experimental |
| `2503` | GA106 | RTX 3060 | `25b6` A2 | `10de:157e` | **no** | Ampere | experimental |
| `2504` | GA106 | RTX 3060 LHR | `25b6` A2 | `10de:157e` | **no** | Ampere | experimental |
| `2507` | GA106 | RTX 3050 | `25b6` A2 | `10de:157e` | **no** | Ampere | experimental |
| `2544` | GA106 | RTX 3060 | `25b6` A2 | `10de:157e` | **no** | Ampere | experimental |
| `2582` | GA107 | RTX 3050 8GB | `25b6` A2 | `10de:157e` | yes | Ampere | experimental |
| `2584` | GA107 | RTX 3050 6GB | `25b6` A2 | `10de:157e` | yes | Ampere | experimental |
| `2684` | AD102 | RTX 4090 | `26b1` RTX 6000 Ada | any | yes | Ada | experimental; untested; 1 VF |
| `2685` | AD102 | RTX 4090 D | `26b1` RTX 6000 Ada | any | yes | Ada | experimental; untested; 1 VF |
| `2689` | AD102 | RTX 4070 Ti SUPER | `26b1` RTX 6000 Ada | any | yes | Ada | experimental; untested; 1 VF |
| `2702` | AD103 | RTX 4080 SUPER | `27b8` L4 | any | **no** | Ada | experimental; untested; 1 VF |
| `2704` | AD103 | RTX 4080 | `27b8` L4 | any | **no** | Ada | experimental; untested; 1 VF |
| `2705` | AD103 | RTX 4070 Ti SUPER | `27b8` L4 | any | **no** | Ada | experimental; untested; 1 VF |
| `2709` | AD103 | RTX 4070 | `27b8` L4 | any | **no** | Ada | experimental; untested; 1 VF |
| `2782` | AD104 | RTX 4070 Ti | `27b8` L4 | any | yes | Ada | experimental; untested; 1 VF |
| `2783` | AD104 | RTX 4070 SUPER | `27b8` L4 | any | yes | Ada | experimental; one board measured by hand with a host stack this package does not fully provide ([Ada host stack](ada.md)); 1 VF |
| `2786` | AD104 | RTX 4070 | `27b8` L4 | any | yes | Ada | experimental; untested; 1 VF |
| `2788` | AD104 | RTX 4060 Ti | `27b8` L4 | any | yes | Ada | experimental; untested; 1 VF |
| `2803` | AD106 | RTX 4060 Ti | `27b8` L4 | any | **no** | Ada | experimental; untested; 1 VF |
| `2805` | AD106 | RTX 4060 Ti 16GB | `27b8` L4 | any | **no** | Ada | experimental; untested; 1 VF |
| `2882` | AD107 | RTX 4060 | `27b8` L4 | any | **no** | Ada | experimental; untested; 1 VF |
| `2b85` | GB202 | RTX 5090 | `2bb5` RTX PRO 6000 Blackwell Server Edition | `10de:204e` | yes | Blackwell | experimental; third-party reports mixed, not verified by geforce-sriov; 1 VF by default; needs the open flavour |
| `2c02` | GB203 | RTX 5080 | `2c3a` RTX PRO 4500 Blackwell Server Edition | `10de:21f4` | yes | Blackwell | experimental; untested; 1 VF by default; needs the open flavour |
| `2c05` | GB203 | RTX 5070 Ti | `2c3a` RTX PRO 4500 Blackwell Server Edition | `10de:21f4` | yes | Blackwell | experimental; untested; 1 VF by default; needs the open flavour |
| `2d04` | GB206 | RTX 5060 Ti | `2c3a` RTX PRO 4500 Blackwell Server Edition | `10de:21f4` | **no** | Blackwell | experimental; untested; 1 VF by default; needs the open flavour |
| `2d05` | GB206 | RTX 5060 | `2c3a` RTX PRO 4500 Blackwell Server Edition | `10de:21f4` | **no** | Blackwell | experimental; untested; 1 VF by default; needs the open flavour |
| `2f04` | GB205 | RTX 5070 | `2c3a` RTX PRO 4500 Blackwell Server Edition | `10de:21f4` | **no** | Blackwell | experimental; untested; 1 VF by default; needs the open flavour |

"Target subsystem: any" means the target's stock `<pgpu>` entry has subsystem ID `0x0`, so the card's
real subsystem is kept. The third-party reports for `2b85` are listed under
[Third-party reports](#third-party-reports); the status of Ada and Blackwell rows is explained
under [Ada and Blackwell](#ada-and-blackwell).

## Capability positions per generation

| Generation | SR-IOV | ARI | MSI-X | MSI-X hide bit |
|---|---|---|---|---|
| Ampere | hidden at `0xBCC` (TotalVFs 16, first VF offset 4, stride 1) | hidden at `0xC14` | `0xC8` | `0x41C` bit 29 |
| Ada | hidden at `0xBCC` (TotalVFs 1, first VF offset 4, stride 1) | hidden at `0xC14` | `0xC8` expected | `0x41C` bit 29 is set in Ada dumps, but its meaning there is unconfirmed; not written |
| Blackwell | linked natively at `0x250` (TotalVFs 1, first VF offset 2, stride 1) | linked natively at `0x248`; kernel's own discovery | kernel's own discovery | none assumed |

The VF count is a normal SR-IOV setting: `install --vfs N` or `vfs=N` in the config chooses it, from
1 up to the card's hardware default, and `enable` enables every presented VF. The default is 4 on
Ampere. The reason: ARI is enabled in software (with `ari=on` the module presents the card's ARI
capability and turns on ARI forwarding in the bridge above it), but ARI decoding in the GPU hardware
does not work for now. The default may change if ARI decoding is made to work. On Ada and Blackwell
the default is the card's own count, 1 ([Ada and Blackwell](#ada-and-blackwell)).
On every card the module checks the layout the card itself reports (first VF offset and stride):
it refuses to load, or the kernel sets up no SR-IOV on that card, when a VF would land on the PF's
own function, two VFs would share a function, or a VF would land beyond function 7 or on a function
another device already uses, and also when the card's NumVFs register does not read back the value
the kernel wrote during setup.

## Ada and Blackwell

Only Ampere is confirmed to work with this package. NVIDIA's stock vGPU host drivers run Ada and
Blackwell on GSP firmware. One Ada card, an RTX 4070 SUPER (`10de:2783`), ran vGPU guests on one
host with a modified, CPU-only (GSP off) NVIDIA host stack that this package does not fully provide;
what was measured and what is missing is in [Ada host stack](ada.md). Everything else below comes
from NVIDIA's public driver source, public config-space dumps and the third-party reports below.

| | Ada (AD10x) | Blackwell (GB20x) |
|---|---|---|
| VFs by default | 1, the card's own count | 1, the card's own count |
| With `experimental_vfs=on` | Linux shows up to 4 VFs, but NVIDIA's driver still sees 1 (it reads the count through a BAR0 mirror that the module cannot change). On the measured RTX 4070 SUPER only the first VF reached a vGPU; the others failed with `NV_ERR_OUT_OF_RANGE` | Linux and NVIDIA's driver see up to 4 VFs. Untested: the GPU firmware may still refuse more than 1 VF |
| `twin_id` | not available | RTX 5090 (`2b85`) only ([below](#twin_id-rtx-5090-only)) |
| NVIDIA driver flavour | proprietary 580.159.01 with GSP off and a modified module, measured by hand on one card ([Ada host stack](ada.md)); the open flavour is not supported | open flavour needed, which geforce-sriov does not support yet ([below](#nvidia-driver-flavour)) |
| Status | experimental; `2783` measured by hand on one board, every other Ada card untested | experimental; RTX 5090 third-party reports mixed, not verified by geforce-sriov; other cards untested |

**Why 1 VF by default.** NVIDIA's driver does not use the count Linux sees. On these cards it reads
TotalVFs from the hardware itself and accepts vGPUs only up to that count. On Blackwell it reads it
with a 32-bit config read, which the module changes when `experimental_vfs=on`; on Ada it reads it
through a BAR0 mirror, which no config hook sees.

### NVIDIA driver flavour

NVIDIA's vGPU host package can install its kernel module in two flavours, proprietary and open.
geforce-sriov's driver patch and install path support only the proprietary flavour for now.

- **Ampere:** the proprietary flavour is the tested one. The open flavour always starts the GSP
  firmware, and on Ampere it was measured to fail at GPU start (`RmInitAdapter failed!
  (0x62:0x56:…)`, with the open flavour of 580.178.05 and 595.91.04).
- **Blackwell:** needs the open flavour. The proprietary 580.159.01 object does not support these
  GPUs on a vGPU host; NVIDIA's message for it reads `… installed in this vGPU host system is not
  supported by proprietary nvidia.ko`. Blackwell therefore cannot be completed with this package yet.
- **Ada:** NVIDIA's proprietary 580.159.01 host module refuses Ada at probe. With that check
  bypassed by a two-byte change and `NVreg_EnableGpuFirmware=0`, it ran on the CPU without GSP on
  the one measured RTX 4070 SUPER ([Ada host stack](ada.md)). geforce-sriov's patcher does not make
  that change. The open flavour, which always uses GSP, ran Linux guests on that card only with
  further lab-only workarounds, still showed scheduler stalls, and is not supported.

This is what NVIDIA's driver source and the measured Ampere and Ada results lead to expect;
NVIDIA's host documentation does not state it.

### Per-VF BARs on Blackwell

Public bug reports of an RTX 5090 and an RTX 5080 show three 64-bit prefetchable BARs per VF:
256 KiB, 256 MiB and 32 MiB. Four VFs need about 1.1 GiB of bridge window on top of the card's own
BARs; one VF about 290 MiB. The card's BAR1 is resizable (up to 32 GiB on an RTX 5090), and the
VFs' 256 MiB BAR has its own resizable BAR capability, so the firmware's *Above 4G decoding* and
*Resizable BAR* settings change how much window space is left. A third-party report needed
`pci=realloc` even for one VF. `doctor`, `status` and `prepare` check only that the BARs are
assigned, not their sizes, so the larger Blackwell VF BARs are not reported as an error.

### twin_id (RTX 5090 only)

> [!WARNING]
> `twin_id` is experimental and untested. It may make the GPU firmware fail to start. It is off by
> default.

With `twin_id=on`, the kernel module answers in-kernel 32-bit reads of config dwords `0x00` and
`0x2C` of each listed RTX 5090 (`10de:2b85`) with the RTX PRO 6000 Blackwell Server Edition ID:
device `10de:2bb5`, subsystem `10de:204e`. NVIDIA's driver reads these dwords when it first opens
the GPU, before it starts the GPU firmware. For that ID it reserves a larger firmware heap
(1370 MiB instead of 581 MiB) and 48 VF partitions instead of 32.

- It applies only to the listed RTX 5090 cards and only to those two dwords. Every in-kernel reader
  of them sees the twin ID: NVIDIA's driver, and also the kernel's PCI core and its hotplug
  driver. User space (`lspci`, sysfs) still shows `2b85`. Every other read, device and offset is
  unchanged.
- The module refuses to load with `twin_id=1`:
  - when none of the listed cards is an RTX 5090 (on the other listed cards `twin_id` has no
    effect);
  - when a listed RTX 5090 is behind a hotplug-capable port (`twin_id refused: the GPU is behind a
    hotplug-capable port`): the kernel's hotplug driver compares these dwords with the ID it
    found at boot and would treat the card as replaced;
  - when a driver is bound to a listed RTX 5090 (`twin_id refused: nvidia (or another driver) is
    bound to the GPU`): NVIDIA's driver keeps the ID it read at first open and compares it after a
    reset. `prepare` already refuses a card bound to `nvidia`, so on the normal path the module
    loads before NVIDIA's driver binds the card.
- It does not change the VF count (`experimental_vfs` does), the vGPU types offered, or the
  placement problem of the 96 GB type table on a 32 GB card ([Third-party reports](#third-party-reports)).
- It does not exist for Ada (the driver reads the ID through a BAR0 mirror) or for the other
  Blackwell cards (the ID changes nothing when their firmware starts).
- The module's parameters are fixed while it is loaded. Do not disable or unload it while
  NVIDIA's driver is bound to the card: after a reset the driver compares the ID it read at start
  with a new read.

### Third-party reports

Two public projects report results on an RTX 5090. Neither is verified by geforce-sriov.

- [olealgoritme/blackwell-vgpu-unlock](https://github.com/olealgoritme/blackwell-vgpu-unlock)
  reports one VF on an RTX 5090 with vGPU host driver 580.159.01 in the open flavour, and compute
  (CUDA) in a Linux guest only. Graphics, Windows guests and more than one VF are listed as not
  working. It uses the card's own single VF, presents the card as `10de:2bb5` subsystem `10de:204e`
  (the same mapping as geforce-sriov's identity hook), needs `pci=realloc`, and patches NVIDIA's
  open driver object so that the 96 GB type table still gets placement IDs on the 32 GB card.
  geforce-sriov has no such patch.
- [bird/vgpu-unlock-blackwell](https://github.com/bird/vgpu-unlock-blackwell) reports an
  RTX 5090 attempt on driver 595.58.03 that stopped with a GSP RPC timeout (Xid 119), and explains
  it with "fused" registers on consumer silicon. NVIDIA's public source does not support that
  explanation: the quoted register `0x111424` is the GSP core's own error-information register,
  which the driver reads in its timeout report, and the quoted code means a timeout, not a fused-off
  target. The flow also differs from NVIDIA's: no VF is enabled, the vGPU types are self-made and
  several driver checks are patched out.

Results on your own card are welcome as a
[compatibility report](https://github.com/noon-at-cgn/geforce-sriov/issues/new?template=compatibility-report.yml).

## Excluded

- **Turing:** NVIDIA does not support SR-IOV on Turing
  ([validated platforms](https://docs.nvidia.com/vgpu/20.0/grid-vgpu-release-notes-red-hat-el-kvm/validated-platforms.html)).
- **Laptop GPUs:** not listed; Blackwell laptop dies show no SR-IOV capability in public
  config-space dumps.
- **Professional cards:** they have NVIDIA's own vGPU support and are never modified.
