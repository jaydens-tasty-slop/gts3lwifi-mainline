# Mainline Linux on the Samsung Galaxy Tab S3 Wi-Fi (SM-T820, gts3lwifi)

Early bring-up of mainline Linux / postmarketOS on the Galaxy Tab S3 Wi-Fi.

- SoC: Qualcomm APQ8096 (Snapdragon 820), PMICs PM8994 + PM8004
- Storage: eMMC (not UFS)
- Bootloader chain: Samsung S-Boot -> lk2nd -> Linux
- Kernel: [msm8996-mainline/linux](https://gitlab.com/msm8996-mainline/linux) `v6.19.5-msm8996`,
  packaged as postmarketOS `linux-postmarketos-qcom-msm8996`

## Variants

The work targets the Wi-Fi model (SM-T820, `gts3lwifi`). The LTE model (SM-T825, `gts3llte`) shares the board apart from the modem; a device tree (`dts/msm8996-samsung-gts3llte.dts`) and an lk2nd build for it exist but are untested so far (docs/TESTING-t825-round1.md).

## Status

| Feature | Status |
|---|---|
| Boots, 4 CPU cores | works |
| eMMC (HS400) | works |
| USB peripheral (NCM network, ACM serial, mass storage) | works |
| PM8994 regulators via RPM | works (only the rails needed so far) |
| postmarketOS initramfs + debug shell over USB | works |
| Full rootfs boot | works (ssh over USB, rootfs resized to the full eMMC) |
| Reboot | works: PS_HOLD through syscon-reboot, because PSCI SYSTEM_RESET hangs in Samsung's TrustZone (docs/reboot.md). Power-off untested |
| TLMM GPIOs | work, 8 TrustZone-owned pins reserved |
| Display | both DSI links stream in sync (MDP5 split display, patch 0003), geometry correct, console readable, panel answers register reads. Colours still wrong; link-isolation images and the DSI test pattern place the fault between the DSI controller and the panel controller, most likely the panel init being sent as short DSI packets where the vendor uses long ones (fix in test, docs/display.md). No brightness control yet |
| Touch | works (Synaptics S5807 RMI4, dts/test/test-touch.dts) |
| S-Pen | not started |
| GPU (Adreno 530) | works headless: freedreno OpenGL ES 3.1, Samsung zap shader accepted, test render correct (docs/gpu.md, main DTS) |
| Wi-Fi (QCA6174, PCIe) | works: ath10k with linux-firmware and Samsung's board data, scans on 2.4 and 5 GHz. Needs BT_EN low while the chip's 3.3 V rail comes up (docs/wifi.md, main DTS) |
| Bluetooth (QCA6174, UART) | works: hci_qca with linux-firmware, address from the efs partition at boot (docs/bluetooth.md, main DTS) |
| Video codec (Venus) | decoding works and is bit-exact (H.264, VP8, VP9) with Samsung's firmware; H.264 encoding works via v4l2-ctl. Kernel patches 0008-0009 fix EOS and H.264 encoding on HFI v3. HEVC decode and GStreamer encoding still fail (docs/venus.md, main DTS) |
| Thermal (TSENS) | works once enabled: the msm8996 kernel config had it off, so nothing throttled (docs/thermal.md) |
| CPU idle | works once enabled: the msm8996 kernel config had `ARM_PSCI_CPUIDLE` off, so the cores never left WFI. With the CPU state plus L2 GDHS per cluster and system retention (OSI hierarchy, main DTS), idle power drops by a third (about 200 -> 135 mA). The firmware is OSI from boot but refuses the mode switch; kernel patch 0018 accepts that. CPU and L2 power collapse are refused by the firmware (docs/cpuidle.md) |
| Audio | headphones and the main/sub mics work, through ALSA and PipeWire (ADSP + WCD9335 on SLIMbus; patches 0010-0011, UCM profile and WirePlumber config in the device package). Speakers not yet: 4 Richtek RT5508 amps, no mainline driver (docs/audio.md, main DTS) |
| Battery (SM5705 fuel gauge) | works: percentage, voltage, current and temperature in sysfs and UPower. New driver, patches 0012-0013 (docs/battery.md, main DTS) |
| Charger (SM5705) | works: USB power, charger type and charging state in sysfs and UPower, charge voltage set to Samsung's 4.30 V (without a driver the charger used 4.38 V), input current limit from the detected charger type (MUIC, BC1.2: PC port 500 mA, wall charger 1.8 A). New drivers, patches 0014-0017 (docs/battery.md, main DTS). Opt-in 9 V fast charging with Samsung AFC / QC 2.0 chargers on a USB-A to C cable (patches 0019-0020, tested with AFC). USB-C PD needs the S2MM005 driver; flash LED not yet |
| Type-C controller (S2MM005) | no driver |

Solved: Samsung's TrustZone owns the fingerprint SPI pins (TLMM 25-28) and the
secure-element SPI pins (TLMM 58-61). If Linux reads them, the firmware resets
the tablet about 9 s into boot. The board DTS lists them in
`gpio-reserved-ranges`; found by bisecting on hardware (`dts/test/`).

## Layout

- `dts/` - board device tree with everything that works on hardware, plus test variants that `#include` it (display and touch are still test-only; the merged tests are include-only stubs)
- `pmaports/` - postmarketOS device package and the kernel patch
- `lk2nd/` - lk2nd device entries for the Wi-Fi and LTE models
- `scripts/` - regenerate the kernel patch, build tester packages
- `tools/` - on-device debugging aids: MMIO and PMIC (SPMI) register access, manual PCIe link retrain
- `kernel/` - out-of-tree kernel additions (panel, fuel gauge, charger and MUIC drivers, bindings, Kconfig snippet)
- `docs/` - tester instructions, boot logs, display, GPU, Wi-Fi, Bluetooth, reboot, video, thermal, audio, battery, charger and CPU idle notes

## lk2nd

A working prebuilt image is in the GitHub releases.

Samsung's bootloader only accepts lk2nd when it is packed like a stock boot
image. What works, what does not, and a packing script are in
[docs/lk2nd.md](docs/lk2nd.md). A working, reproducible lk2nd image is
attached to the GitHub releases (`lk2nd-gts3lwifi.img`).

## Debugging without a display or UART

lk2nd can keep the kernel log in RAM across a reset:

    fastboot --cmdline "console=ttyMSM0,115200 lk2nd.pass-ramoops=zap" boot boot.img
    # after the reset, back in lk2nd:
    fastboot oem ramoops console
    fastboot get_staged console.txt

The AP debug UART is BLSP2 UART2 on TLMM GPIO 4 (TX) / 5 (RX), 1.8 V, 115200 8N1
(`dts/test/test-A-uart.dts`). Pad locations on the board are not known yet.

## Licence

Device tree files: BSD-3-Clause (see SPDX headers). The kernel patch applies to
the GPL-2.0 Linux kernel. Packaging files follow postmarketOS pmaports (MIT).
No firmware blobs, stock images or schematics are included in this repository.
