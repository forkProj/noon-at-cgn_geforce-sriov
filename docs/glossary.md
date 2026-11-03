# Glossary

| Term | Meaning |
|---|---|
| **PF** (physical function) | The card itself as the PCI bus sees it, for example `0000:03:00.0` |
| **BDF** | A PCI address in the form `domain:bus:device.function`, as in `/sys/bus/pci/devices` |
| **SR-IOV** | A PCIe feature that lets one device appear as several lightweight devices |
| **VF** (virtual function) | One of those lightweight devices; each can back one vGPU. geforce-sriov presents 4 per card by default (`vfs`), because ARI decoding in the GPU hardware does not work yet |
| **ARI** | A PCIe addressing extension that allows more functions per device. The module enables it in software: it reveals the card's hidden ARI and turns on ARI forwarding in the bridge above it (`ari=on`). ARI decoding in the GPU hardware does not work for now |
| **MSI-X** | A way for a device to send interrupts. The module reveals the card's hidden MSI-X on Ampere (`msix=on`) |
| **vGPU** | NVIDIA's software that gives each VM a slice of one GPU, with a fixed memory size per type |
| **mdev** (mediated device) | Legacy vGPU, on GPUs without SR-IOV, creates each vGPU as a Linux mediated device on the physical function. With geforce-sriov each VM gets a VF instead; the Proxmox VE `hostpci` line names the vGPU type on that VF with `mdev=<type>`, for example `mdev=nvidia-527` ([SR-IOV note](../README.md#how-it-works)) |
| **IOMMU** | The hardware that keeps each VM's device access separate; must be enabled |
| **Livepatch** | A kernel feature that changes a running kernel's functions without a reboot; used here only for the listed cards |
| **DKMS** | Rebuilds out-of-tree kernel modules automatically for each new kernel |
| **Boot display** | The card the firmware uses for the boot screen (`boot_vga` is `1` in sysfs); see [The boot display GPU](boot-display.md) |
