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
- **Ada and Blackwell** GeForce cards report TotalVFs = 1. With the default `experimental_vfs=on`
  the kernel module shows the kernel up to 4 VFs (the configured `vfs`, at most 4) when the hardware
  value is exactly 1 (a value of 0 stays 0). This is not verified end to end, and whether the
  GSP firmware or the driver accepts more than one VF on these cards is unknown. The module refuses
  the load outright if the card's VF offset or stride would place a VF outside functions 1–7.
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
only for the builds in [driver-patch.md](driver-patch.md), and does not apply to GSP-based drivers,
NVIDIA's open kernel modules, Ada or Blackwell. Host layouts (switches, shared buses, IOMMU groups,
BAR space) are covered in [the technical reference](technical-reference.md#multiple-gpus-and-pci-topology).

## Rows

| Device | Die | GeForce product | vGPU target | Target subsystem | Same die | Generation |
|---|---|---|---|---|---|---|
| `2203` | GA102 | RTX 3090 Ti | `2230` RTX A6000 | any | yes | Ampere |
| `2204` | GA102 | RTX 3090 | `2230` RTX A6000 | any | yes | Ampere |
| **`2206`** | GA102 | RTX 3080 (measured: memory-modded 20GB, subsystem `10de:146d` only) | `2230` RTX A6000 | any | yes | Ampere |
| `2208` | GA102 | RTX 3080 Ti | `2230` RTX A6000 | any | yes | Ampere |
| `220a` | GA102 | RTX 3080 12GB | `2230` RTX A6000 | any | yes | Ampere |
| `2216` | GA102 | RTX 3080 LHR | `2230` RTX A6000 | any | yes | Ampere |
| `2482` | GA104 | RTX 3070 Ti | `25b6` A2 | `10de:157e` | **no** | Ampere |
| `2484` | GA104 | RTX 3070 | `25b6` A2 | `10de:157e` | **no** | Ampere |
| `2486` | GA104 | RTX 3060 Ti | `25b6` A2 | `10de:157e` | **no** | Ampere |
| `2487` | GA104 | RTX 3060 | `25b6` A2 | `10de:157e` | **no** | Ampere |
| `2488` | GA104 | RTX 3070 LHR | `25b6` A2 | `10de:157e` | **no** | Ampere |
| `2489` | GA104 | RTX 3060 Ti LHR | `25b6` A2 | `10de:157e` | **no** | Ampere |
| `2503` | GA106 | RTX 3060 | `25b6` A2 | `10de:157e` | **no** | Ampere |
| `2504` | GA106 | RTX 3060 LHR | `25b6` A2 | `10de:157e` | **no** | Ampere |
| `2507` | GA106 | RTX 3050 | `25b6` A2 | `10de:157e` | **no** | Ampere |
| `2544` | GA106 | RTX 3060 | `25b6` A2 | `10de:157e` | **no** | Ampere |
| `2582` | GA107 | RTX 3050 8GB | `25b6` A2 | `10de:157e` | yes | Ampere |
| `2584` | GA107 | RTX 3050 6GB | `25b6` A2 | `10de:157e` | yes | Ampere |
| `2684` | AD102 | RTX 4090 | `26b1` RTX 6000 Ada | any | yes | Ada |
| `2685` | AD102 | RTX 4090 D | `26b1` RTX 6000 Ada | any | yes | Ada |
| `2689` | AD102 | RTX 4070 Ti SUPER | `26b1` RTX 6000 Ada | any | yes | Ada |
| `2702` | AD103 | RTX 4080 SUPER | `27b8` L4 | any | **no** | Ada |
| `2704` | AD103 | RTX 4080 | `27b8` L4 | any | **no** | Ada |
| `2705` | AD103 | RTX 4070 Ti SUPER | `27b8` L4 | any | **no** | Ada |
| `2709` | AD103 | RTX 4070 | `27b8` L4 | any | **no** | Ada |
| `2782` | AD104 | RTX 4070 Ti | `27b8` L4 | any | yes | Ada |
| `2783` | AD104 | RTX 4070 SUPER | `27b8` L4 | any | yes | Ada |
| `2786` | AD104 | RTX 4070 | `27b8` L4 | any | yes | Ada |
| `2788` | AD104 | RTX 4060 Ti | `27b8` L4 | any | yes | Ada |
| `2803` | AD106 | RTX 4060 Ti | `27b8` L4 | any | **no** | Ada |
| `2805` | AD106 | RTX 4060 Ti 16GB | `27b8` L4 | any | **no** | Ada |
| `2882` | AD107 | RTX 4060 | `27b8` L4 | any | **no** | Ada |
| `2b85` | GB202 | RTX 5090 | `2bb5` RTX PRO 6000 Blackwell Server Edition | `10de:204e` | yes | Blackwell |
| `2c02` | GB203 | RTX 5080 | `2c3a` RTX PRO 4500 Blackwell Server Edition | `10de:21f4` | yes | Blackwell |
| `2c05` | GB203 | RTX 5070 Ti | `2c3a` RTX PRO 4500 Blackwell Server Edition | `10de:21f4` | yes | Blackwell |
| `2d04` | GB206 | RTX 5060 Ti | `2c3a` RTX PRO 4500 Blackwell Server Edition | `10de:21f4` | **no** | Blackwell |
| `2d05` | GB206 | RTX 5060 | `2c3a` RTX PRO 4500 Blackwell Server Edition | `10de:21f4` | **no** | Blackwell |
| `2f04` | GB205 | RTX 5070 | `2c3a` RTX PRO 4500 Blackwell Server Edition | `10de:21f4` | **no** | Blackwell |

"Target subsystem: any" means the target's stock `<pgpu>` entry has subsystem ID `0x0`, so the card's
real subsystem is kept.

## Capability positions per generation

| Generation | SR-IOV | ARI | MSI-X | MSI-X hide bit |
|---|---|---|---|---|
| Ampere | hidden at `0xBCC` (TotalVFs 16, first VF offset 4, stride 1) | hidden at `0xC14` | `0xC8` | `0x41C` bit 29 |
| Ada | hidden at `0xBCC` (TotalVFs 1) | hidden at `0xC14` | `0xC8` expected | unknown; never written |
| Blackwell | linked natively at `0x250` (TotalVFs 1) | kernel's own discovery | kernel's own discovery | none assumed |

The VF count is a normal SR-IOV setting: `install --vfs N` or `vfs=N` in the config chooses it, from
1 up to the card's hardware default, and `enable` enables every presented VF. The default is 4. The
reason: ARI is enabled in software (with `ari=on` the module presents the card's ARI capability and
turns on ARI forwarding in the bridge above it), but ARI decoding in the GPU hardware does not work
for now. The default may change if ARI decoding is made to work.
On every card the module checks the layout the card itself reports (first VF offset and stride):
it refuses to load, or the kernel sets up no SR-IOV on that card, when a VF would land on the PF's
own function, two VFs would share a function, or a VF would land beyond function 7 or on a function
another device already uses, and also when the card's NumVFs register does not read back the value
the kernel wrote during setup.

## Excluded

- **Turing:** NVIDIA does not support SR-IOV on Turing
  ([validated platforms](https://docs.nvidia.com/vgpu/20.0/grid-vgpu-release-notes-red-hat-el-kvm/validated-platforms.html)).
- **Laptop GPUs:** not listed; Blackwell laptop dies show no SR-IOV capability in public
  config-space dumps.
- **Professional cards:** they have NVIDIA's own vGPU support and are never modified.
