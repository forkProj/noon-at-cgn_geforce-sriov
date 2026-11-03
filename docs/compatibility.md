# Compatibility reports

geforce-sriov has been measured on one host
([What has been verified](../README.md#what-has-been-verified)). Reports from other machines are
how everyone learns which cards and hosts work. Whether it works or not on your hardware, please
submit a
[compatibility report](https://github.com/noon-at-cgn/geforce-sriov/issues/new?template=compatibility-report.yml).
A setup that does not work is as useful as one that works: choose *Does not work* or *Partially*
in the form and say where it stopped.

Reports are voluntary. Filling in the form takes a few minutes; only a few fields are required.

## What to collect

Run these on the host. Replace `0000:03:00.0` with your card's address; `lspci -nn | grep -i nvidia`
lists the NVIDIA devices. In a source checkout that is not installed yet, use `./geforce-sriov`
instead of `geforce-sriov`.

| What | Command | What to copy |
|---|---|---|
| PCI ID and subsystem ID | `lspci -nnv -s 0000:03:00.0` | The `[10de:....]` at the end of the first line, and the ID on the `Subsystem:` line |
| Device table row | `geforce-sriov devices` | Whether your ID is listed, and its status (`supported` or `experimental`) |
| PCIe path to the GPU | `lspci -tv` | Whether the card is attached to a CPU root port, the chipset or a PCIe switch |
| Host kernel | `uname -r` | The whole line |
| NVIDIA vGPU host driver | `modinfo -F version nvidia` | The whole line |
| geforce-sriov version | `geforce-sriov --version`, and `git rev-parse --short HEAD` in a checkout | Both |
| `doctor` output | `sudo geforce-sriov doctor` | The whole output |
| `status` output | `sudo geforce-sriov status` | The whole output |
| Kernel log lines | `journalctl -k -b --no-hostname \| grep -E 'geforce_sriov\|NVRM\|AER'` | Only these lines, not the whole log |

If the problem showed up in an earlier boot, use `-b -1` instead of `-b` (this needs a persistent
journal). Without systemd's journal, `sudo dmesg | grep -E 'geforce_sriov|NVRM|AER'` shows the
same lines for the current boot. The other logs are listed in
[Troubleshooting](troubleshooting.md#where-the-logs-are).

For the guests, note each guest OS, its vGPU type (for example `RTXA6000-4Q`) and the version of
the NVIDIA vGPU guest driver installed in it.

## What to remove before posting

Issues are public. Before you paste output, remove or replace:

- IP and MAC addresses
- serial numbers of the GPU, the board or any other part, and GPU UUIDs (`GPU-…`)
- host names and user names, for example in shell prompts or log lines
- everything about your NVIDIA licence: licence server addresses, client configuration tokens,
  licence files
- passwords, keys and any other secrets

PCI addresses such as `0000:03:00.0`, PCI IDs and version numbers are not private; keep them.

## Opening a report

Open the
[compatibility report form](https://github.com/noon-at-cgn/geforce-sriov/issues/new?template=compatibility-report.yml),
or on GitHub choose **Issues → New issue → Compatibility report**. Please file one report per card
model and host. If your setup broke and you are looking for a fix, read
[Troubleshooting](troubleshooting.md) as well.

A bug in geforce-sriov's own tools or documentation that does not depend on your hardware, for
example a wrong message or a documentation error, goes in a **Bug report** instead.

## What happens to a report

- A report stays public as an issue with the `compatibility` label, so other readers with the same
  card can find it.
- The status of each card is the `experimental` field in `include/gs_devices.h`, shown by
  `geforce-sriov devices` and copied into [Devices](devices.md#status-of-every-row). Only
  `10de:2206` is `supported`; every other row is `experimental`, which means that device ID is not
  verified end to end. A row changes from `experimental` only when someone with that
  card reports a working setup and the maintainers confirm it. What counts as confirmed is
  maintainer judgement.
- Reports of setups that do not work are kept as well. Whether one leads to a note in the
  documentation is maintainer judgement.
- There is no guarantee of support, of a fix or of a response time.
