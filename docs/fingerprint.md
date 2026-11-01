# Fingerprint sensor (Home key)

## Status (2026-11-01): blocked on both models (desk research, nothing tried on hardware)

The sensor is an Egis (EgisTec) ET510 on BLSP1 QUP6 SPI. Samsung's TrustZone
owns that bus. On the SM-T820 Linux can't even read its pins: psigod's boot
tests showed that a read resets the tablet. Stock Linux never talks to the
sensor. It switches the sensor's power, reset line and SPI clocks, and
forwards the finger interrupt. Everything else runs inside `dualfp`, a
Samsung-signed QSEE trustlet (TrustZone app) loaded from the `apnhlos`
partition. So there are two possible routes, and neither is open:

| Route | What blocks it | Can it go upstream? |
|---|---|---|
| Linux drives the SPI bus itself | TrustZone protects TLMM 25-28 (and probably the QUP and its BAM pipes) from boot. lk2nd and the DT can't change that. One TrustZone call that might hand the bus over has not been tried (see "The one untested lever") | Only if that call works. Even then the sensor init comes from the trustlet, and libfprint image matching on a small sensor may not be usable |
| Linux uses the trustlet | Mainline QSEECOM can't load apps, has no listener support, only runs on allowlisted laptops and has no user-space interface. The trustlet's command protocol is proprietary and undocumented | No |

The README's line "its pins are owned by Samsung's TrustZone" is correct, and
for the SM-T820 it is proven on hardware. The fuller statement: the pins, the
SPI controller and the whole sensor protocol belong to the secure world.

## The sensor and its wiring

Both models use the same DT include (`msm8996-sec-gts3llte_fingerprint-sensor_00.dtsi`,
pulled in at line 24 of every `msm8996-sec-gts3lwifi-r0x.dtsi` and
`msm8996-sec-gts3llte-r0x.dtsi`), the same defconfig values and the same PMIC
pins (the r05 `BTP_*` blocks differ only in whitespace).

| Signal | Connection | Source |
|---|---|---|
| Sensor | Egis ET510, `compatible = "etspi,et5xx"`, `etspi-chipid = "ET510"` | fingerprint dtsi:72-82; stock log `etspi_parse_dt: chipid: ET510`; HAL test library built from `sensor_test_et510/` |
| SPI controller | BLSP1 QUP6, `spi@757a000`, GIC SPI 100, alias `spi6` | fingerprint dtsi:45-53; `msm8996-sec-gts3lwifi-r05.dts:51` |
| BAM | BLSP1 BAM `0x07544000`, pipes 22/23 (stock disables BAM on this QUP) | fingerprint dtsi:51,61; `spi_qsd.c:2897-2904` |
| Clocks | `gcc_blsp1_ahb_clk`, `gcc_blsp1_qup6_spi_apps_clk`; bus master 86 (BLSP1) | fingerprint dtsi:56,68-70 |
| SPI pins | TLMM 25-28 (MOSI, MISO, CS_N, CLK), function `blsp_spi6`, 6 mA, no pull; sleep: GPIO, pull-down | `msm8996-pinctrl-sec-gts3lwifi-r05.dtsi:801-817` (LTE: `-gts3llte-r05.dtsi:822-838`) |
| SPI mode | mode 0, 8 bit, CS 0. The sensor allows up to 16 MHz, the controller 19.2 MHz. The HAL asks for 12.5 MHz (and 4 MHz at times) | fingerprint dtsi:55,75; `et5xx-spi.c:1144-1147`; stock log `FP_SET_SPI_CLOCK, clock = 12500000 / 4000000` |
| Interrupt (DRDY) | TLMM 131, pull-up when active, pull-down in suspend; set up as a wake IRQ | fingerprint dtsi:25-42,77; `et5xx-spi.c:119-127` |
| Reset (`BTP_RST_N`) | PM8994 MPP 2, digital out, 1.8 V (S4); the driver calls it `sleepPin` | fingerprint dtsi:76; `msm8996-sec-gts3lwifi-r05.dtsi:1197-1207` |
| Power (`BTP_LDO_EN`) | PM8994 MPP 8, digital out, 1.8 V; enables a board LDO | fingerprint dtsi:78; `msm8996-sec-gts3lwifi-r05.dtsi:1248-1258` |
| ID (`BTP_ID`) | PM8994 GPIO 12, digital in, 1.8 V, no pull (picks the sensor vendor) | fingerprint dtsi:13-22 |

Power-on order in the stock driver: LDO enable, wait 50-100 us, reset high,
pins to "idle", wait 10 ms (`et5xx-spi.c:191-211`). A reset is a 1 ms low
pulse on `BTP_RST_N` (`et5xx-spi.c:181-189`).

The reset, power, ID and interrupt pins all belong to Linux. Mainline has
drivers for PM8994 MPPs/GPIOs and for TLMM 131, and nothing in our DTs uses
these pins. Only the four SPI pins and the QUP belong to TrustZone.

File references in this document point to Samsung's GPL kernel as published in
awesometic/android_kernel_samsung_msm8996 (import of T825XXU3CTD1, DTs from
T820XXU3CSH9; line numbers at 53683713fd1f). "Stock log" means boot logs of the
LineageOS 3.18 kernel captured on this SM-T820 on 2026-10-06 and 2026-10-27.

## How stock drives it

Three layers:

1. **Kernel, `drivers/fingerprint/et5xx-spi.c`**: a char device
   `/dev/esfp0` (major 153). Samsung builds it in "secure" mode: the top-level
   `Makefile:837-843` defines `ENABLE_SENSORS_FPRINT_SECURE` for every build
   that doesn't set `SEC_BUILD_CONF_USE_FINGERPRINT_TZ=false`. (The
   `CONFIG_SEC_FACTORY` test there compares a Kconfig bool against `true`,
   so it never excludes anything.) In that mode:
   - the probe skips `spi_setup()` and the sensor ID check and sets
     `tz_mode = true` (`et5xx-spi.c:1148-1174`);
   - all 17 SPI transfer helpers in `et5xx-spi_data_transfer.c` are
     `#ifdef ENABLE_SENSORS_FPRINT_SECURE return 0;`, so the register, frame,
     NVM and VDM ioctls never reach the bus;
   - what is left: `FP_POWER_CONTROL` (MPP 8 and MPP 2), `FP_SENSOR_RESET`,
     `FP_SET_SPI_CLOCK` / `FP_DISABLE_SPI_CLOCK` (sets the QUP6 core clock
     rate and turns the QUP6 core and AHB clocks on or off for TrustZone),
     `INT_TRIGGER_*` plus `poll()` for the DRDY interrupt, and sysfs under
     `/sys/class/fingerprint/fingerprint/` (`et5xx-spi.c:464-580`).

   The stock log shows this mode on the tablet:
   `etspi_work_func_debug ldo: 0, sleep: 0, tz: 1, type: unknown`.

2. **SPI controller, `drivers/spi/spi_qsd.c`**: for bus 6
   (`CONFIG_SENSORS_FP_SPI_NUMBER=6`) the controller is registered but never
   used. `msm_spi_setup()` returns 0 at once (1767-1770). Probe doesn't read
   the QUP version register (2548-2562), doesn't write `SPI_IO_CONTROL` and
   doesn't reset the QUP (2594-2606). It drops BAM (2897-2904) and creates no
   debugfs (2951-2953). Only `fp_spi_clock_*()` (2336-2437) touch it, and only
   its clocks. `fp_spi_request_gpios()` (2439) would mux the pins, but nothing
   calls it. Stock log: `msm_spi_probe: disable bam for BLSP tzspi, spi num(6)`.

   `drivers/pinctrl/qcom/pinctrl-msm.c` keeps Linux away from the TLMM
   registers of GPIO 25-28 (`CONFIG_SENSORS_FP_SPI_GPIO_START/END` = 25/28):
   no config read (564-568), no value read (607-611) and nothing in debugfs
   (685-689).

3. **User space and TrustZone**: the HAL (`libbauthserver.so`, wrapped by
   LineageOS' `android.hardware.biometrics.fingerprint-service.samsung`) opens
   `/dev/esfp0` and, through `libbauthtzcommon.so` and `libQSEEComAPI.so`,
   starts the QSEE app `dualfp` from `/vendor/firmware_mnt/image` (the
   `apnhlos` partition). Stock log of one HAL start:

   ```
   etspi_ioctl FP_POWER_CONTROL, status = 1
   etspi_ioctl FP_SET_SPI_CLOCK, clock = 12500000
   fp_spi_clock_enable sucess
   bauth_TLC_Communicator: QSApp SB Size is 2008576
   ueventd: firmware: loading 'cmnlib64.mdt' ... 'cmnlib64.b05'
   QSEECOM: qseecom_load_app: App (dualfp) does'nt exist, loading apps for first time
   QSEECOM: qseecom_load_app: App with id 7 (dualfp) now loaded
   bauth_TLC_Communicator: Call FP cmd 0x10
   ...
   QSEECOM: qseecom_unload_app: App id 7 now unloaded
   etspi_ioctl FP_DISABLE_SPI_CLOCK
   ```

   Facts about `dualfp`, taken from its ELF headers and strings only:
   - It is a 64-bit ELF (`dualfp.mdt` plus `b00`-`b06`), signed by "Samsung
     AttestationCA" chaining to "Samsung Root CA", with OEM_ID 0x0020 and
     SW_ID 0x0C. TrustZone checks that signature, so the app can't be
     modified.
   - "Copyright of Egistec Ltd.", `ET300`...`ET516` names, `et510_init_sensor`,
     `et510_calibrate_detect_mode`: the Egis capture, calibration and matching
     library is linked in.
   - `sec_tzspi_open`, `qsee_tlmm_get_gpio_id: BLSP_CLK/CS/MISO/MOSI`,
     `blsp_spi_clk[6]`...`blsp_spi_mosi[6]`: the app claims the four pins
     and opens QUP6 through TrustZone's SPI service. On close it sets the
     pins to GPIO output-low and releases them
     (`qsee_tlmm_select_gpio_id_mode to generic IO`, `qsee_tlmm_gpio_id_out
     output-LOW`, `qsee_tlmm_release_gpio_id`).
   - `BAUTH TZ App cmd handler`, `TZ_CMD_ENROLL_DO`, `TZ_CMD_IDENTIFY_INIT`,
     `encode_each_templ`/`decode_each_templ` (HMAC), `CreateAuthToken`:
     enrolment and matching happen in TrustZone. Templates leave TrustZone
     only encrypted and HMAC-protected, and a match produces an Android
     hardware auth token for keymaster (the `authhat` app).

   The same partition also holds `fingerpr`, Qualcomm's "Qualcomm Fingerprint
   Framework" for its own QBT ultrasonic sensors ("Ontario"). This tablet
   doesn't use it.

The stock QSEE reports `qseecom.qsee_version = 0x1001000` (TZ.BF.4.0 family),
`qsee_reentrancy_support = 0` and `no-clock-support=0x1`. On Android,
`qseecomd` serves nine listener services (RPMB 8192, SSD 12288, file system
10, time 11, GP file system 28672 and others) that apps can call back into.

## Can Linux own the bus?

**SM-T820: no, proven on hardware.** psigod's RAM-boot tests found that the
tablet resets about 9 s into boot when Linux touches unreserved TLMM pins:
everything reserved boots, nothing reserved resets (`docs/README-round4.txt`,
commit 98a7bf3). Reserving 25-28 plus 58-61 boots (commit 2585c75). The audio
work later used 58-61 with only 25-28 reserved, and the tablet stayed up
(`docs/audio.md`, "What had to be found", item 6). So 25-28 is the protected
group. Linux reads every pin's config register when the GPIO chip registers
(direction readback), so a read alone is enough to trigger the reset. Why
the reset comes seconds later and not at the access is not known.

**SM-T825: assumed, not tested.** Its DT keeps both 25-28 and 58-61 reserved
(`dts/msm8996-samsung-gts3llte.dts:38-39`). The kernel and DT side is
identical, but its TrustZone build is a different Samsung firmware, so its
pin ownership hasn't been measured separately.

What is still open on both models:

- **The QUP6 registers (`0x0757a000`) and BAM pipes 22/23.** Probably also
  protected: Samsung's driver goes out of its way not to read even the QUP
  version register (see above). Untested.
- **Can lk2nd or the DT change this? No.** The xPU setup is part of
  TrustZone's own initialisation, before aboot runs. lk2nd runs in the same
  non-secure world as Linux, with the same rights. A DT describes hardware;
  it can't grant access. TrustZone is Samsung-signed and checked by secure
  boot, so it can't be replaced.
- **Another route to the sensor? No.** The sensor is wired only to TLMM
  25-28. The only full SPI function on those pins is `blsp_spi6`
  (`pinctrl-msm8996.c`: 25/26 also carry `blsp2_spi`, 27/28 `blsp1_spi`,
  which are extra chip selects). Bit-banging needs the same protected pins.

### The one untested lever

Qualcomm's TrustZone of this generation can hand a BLSP QUP between the
secure world and Linux at run time. The call is SIP service 4 (`SCM_SVC_TZ`),
function 3, `TZ_BLSP_MODIFY_OWNERSHIP_ID`, with two arguments: the QUP index
and the new owner (TZ = 1, APSS = 3). Samsung's secure-touch drivers make
this call on sibling msm8996 phones (`drivers/input/touchscreen/stm/fts_ts.c:72,154`,
Galaxy S7 edge `hero2qlte`; `sec_ts/sec_ts.c:37-42,129-150`), and so does
Qualcomm's `drivers/soc/qcom/qbt1000.c:48-51,636-655`. This tablet's touch
driver (Synaptics DSX2) doesn't use it.

Whether this TrustZone allows the call for BLSP1 QUP6 (index 5 under
`sec_ts`'s numbering), and whether the hand-over also opens the TLMM pins,
is unknown. It changes secure-world state until the next reboot, so it is
not a read-only test (see "Hardware session").

### If the bus ever becomes Linux's

The kernel side would be small:
- a `blsp1_spi6` node (`qcom,spi-qup-v2.2.1`, `0x0757a000`, GIC SPI 100,
  QUP6 SPI and BLSP1 AHB clocks, BAM 22/23, `blsp_spi6` pins);
- a sensor node with reset (PM8994 MPP 2), power (MPP 8, as a GPIO-enabled
  fixed regulator) and interrupt (TLMM 131);
- either spidev with a new `egis,et510` compatible (which needs a binding),
  or a small kernel driver;
- and, only if the lever works, a `qcom_scm` helper for the ownership call
  plus a consumer that makes it.

User space would need a new libfprint driver. libfprint (master, 2026-09)
has SPI support (`fpi-spi-transfer`, used by `elanspi`), but its Egis drivers
are all USB (`egis0570`, `egismoc`, `etes603`), and it finds spidev devices
by matching an ACPI ID string in the sysfs path (`fp-context.c`), so a DT
device needs a matching extension. What we know and don't:

- **Known, from GPL source**: framing and opcodes (`et5xx.h:57-70`: register
  read 0x20, write 0x24, burst variants 0x22/0x23/0x26/0x27, image read
  0x50, NVM 0x40-0x48, VDM 0x60/0x62), the status register 0x01 with bit 0
  "frame ready", and the ID check (`et5xx-spi.c:872-928`: register 0x00
  reads 0xAA when ready; 0xFD-0xFF read 03 0A 05 on ET510D, 00 10 05 on
  ET516A; ET510C answers 00 66 00 33 at 0x20/0x21/0x23/0x24).
- **Unknown**: the init, calibration and finger-detect register settings,
  frame size and timing. These live in the Egis library inside `dualfp`.
  The project rules (facts and init tables only, no transcribed code) would
  allow pulling the tables out with Ghidra.
- **Risk**: libfprint matches images with NBIS minutiae, which does poorly
  on small touch sensors. A working capture path could still give an
  unusable match rate.
- **Hazard**: never send the NVM write opcodes (0x42, 0x46). They write the
  sensor's factory calibration.

## Can Linux use the trustlet?

On the 3.18 kernel this works today: Lineage runs `dualfp` through Qualcomm's
downstream `drivers/misc/qseecom.c`. On mainline, almost every piece is
missing:

| Needed | Mainline (v6.19) |
|---|---|
| Load an app: `cmnlib`/`cmnlib64` first, then the app, from the partition into contiguous memory (downstream `qseecom.c:2266-2400`, `TZ_OS_APP_START_ID` = QSEE_OS/APP_MGR/1; stock reserves a 20 MiB `qseecom_region`) | Missing. `qcom_qseecom` only looks up apps that firmware already loaded (`qcom_qseecom.c`: "assuming the app has already been loaded (usually by firmware bootloaders)") |
| Send commands (QSEE_OS/APP_MGR lookup 3, TZ_APPS send) | Present: `qcom_scm_qseecom_app_get_id()`, `qcom_scm_qseecom_app_send()`. The SMC IDs match downstream's (`qseecomi.h:470-471,543-545`) |
| Run on this machine | Blocked by `qcom_scm_qseecom_allowlist` (laptops only). Its comment says re-entrant calls are not supported yet |
| Listener services (RPMB, file system, time) if `dualfp` calls back | Missing: `qcom_scm_qseecom_call()` warns on `BLOCKED_ON_LISTENER`. Whether `dualfp` needs a listener is unknown; it keeps templates in normal-world files, which points to no |
| Access from user space | Missing on purpose: each app gets an in-kernel client driver (`uefisecapp` is the only one) |
| QTEE / SMC-Invoke (`drivers/tee/qcomtee`) | Unknown on this TrustZone. `qcom_scm_qtee_init()` probes for it on every boot and creates a `qcomtee` platform device only if TrustZone answers; `CONFIG_QCOMTEE` (the driver) is off in our config. Stock disables `CONFIG_QCOM_SMCINVOKE` (`gts3lwifi_defconfig:4129`), and `dualfp` is a legacy command-buffer app |
| QUP6 clocks on, at the HAL's rate, while the app runs | Possible (`GCC_BLSP1_QUP6_SPI_APPS_CLK` exists), but needs a driver with an interface for it |
| Power, reset, interrupt | Possible (PM8994 MPP, TLMM 131) |
| The `dualfp` command protocol (FUNC IDs 0x1-0x20, FP cmds such as 0x10, buffer layouts, template blobs, the auth-token challenge) | Undocumented; only Samsung's `libbauthserver.so` speaks it |

Upstream won't take a generic user-space path into proprietary trustlets
(that is why mainline QSEECOM has none). A per-app kernel client for a
Samsung biometric app, talking a protocol learned by reverse engineering,
has no chance either. libfprint has no TrustZone device class. Loading the
trustlet at run time from the tablet's own partition is the same as other
firmware here (never shipped). Learning its protocol from blobs would have
to stay facts-only.

What could work outside upstream, and only for Android: Qualcomm's GPL
`qseecom` (as in the 3.18 tree, or the later vendor-module version) carried
as an out-of-tree module, Samsung's HAL as is, and a small driver re-creating
`/dev/esfp0`. That belongs to an Android-on-mainline build. For
postmarketOS it would also need libhybris to run a bionic HAL. Months of
work, and none of it upstreamable.

## Verdict

| Model | Verdict | Effort | Risk to the tablet |
|---|---|---|---|
| SM-T820 (`gts3lwifi`, APQ8096) | **Blocked.** The SPI pins are TrustZone-owned (hardware-proven) and the sensor protocol lives in a Samsung-signed trustlet. One untested lever: the BLSP ownership call | Lever test: one short session plus a small test module. If it works: about 1-2 days for SPI bring-up and the ID read, 1-3 weeks to pull the init tables from the trustlet, 2-4 weeks for a libfprint driver, with an uncertain match rate. Trustlet route: months, never upstream | Reading TLMM 25-28: reset after about 9 s (seen, harmless). Unclocked QUP read: possible bus hang (needs a long power-key press). Ownership call: secure-world state until reboot; worst case a reset. Sensor NVM writes: permanent, never do them |
| SM-T825 (`gts3llte`, MSM8996) | **Blocked**, same design and wiring. Pin ownership is inherited from the SM-T820 result, not measured, and its TrustZone build is different | Same, and every hardware result has to be repeated on an SM-T825 | Same |

Nothing here keeps hardware powered, so sleep is unaffected.

## Hardware session (when the owner is back)

Read-only first. Each step stands on its own.

On the LineageOS 3.18 ROM (`adb root`):

1. `dmesg | grep -E 'etspi|tzspi|fp_spi|QSEECOM'` after boot and again after
   unlocking with a finger once. Expected: `disable bam for BLSP tzspi, spi
   num(6)`, `tz: 1`, `dualfp` loaded, `FP_SET_SPI_CLOCK` values.
2. `cat /sys/class/fingerprint/fingerprint/{vendor,name,adm,bfs_values,type_check,intcnt}`.
   Expected: `EGISTEC`, `ET510`, `1`, `"FP_SPICLK":"12500000"`, then `intcnt`
   counting finger touches (DRDY on TLMM 131 works).
3. `logcat -s bauth_FPBAuthService bauth_TLC_Communicator bauth_FPDLBAuthSensorControl`
   during enrolment: the sensor variant (ET510C or D), the commands used and
   their order.
4. Under `/sys/kernel/debug/clk/`, the rate and enable count of
   `gcc_blsp1_qup6_spi_apps_clk` during an unlock: confirms TrustZone
   depends on Linux's clock votes.

On mainline (the installed postmarketOS, or a RAM boot):

5. On the installed system, no new kernel needed:
   `ls /sys/bus/platform/devices | grep -i qcomtee`. `qcom_scm` already makes
   the SMC-Invoke probe on every boot; the device exists only if TrustZone
   answered.
6. A test kernel with `CONFIG_QCOM_QSEECOM=y`. Its version query is
   read-only: expect `qseecom: found qseecom with version 0x...` followed by
   `untested machine, skipping`.
7. `tools/spmi-reg` reads of PM8994 (SID 0) MPP 2 / MPP 8 (`0xa100` /
   `0xa700` blocks) and GPIO 12 (`0xcb00`). Expected: outputs low, ID input level
   recorded.

Changes secure-world state, needs the owner's OK, plugged in, not during a
sleep study:

8. A test module makes the BLSP ownership call (QUP index 5, owner APSS = 3)
   and logs the return value. If it succeeds, read TLMM 25's config register
   (`tools/mmio-rd 1029000`) and wait 15 s. A reset means the pins were
   not handed over. Then hand QUP6 back (owner TZ = 1) or reboot.
9. Only if 8 opens both the pins and the QUP: a test DT with `blsp1_spi6` +
   spidev. Power the sensor (MPP 8 high, MPP 2 high, 10 ms), then read
   register 0x00 (expect 0xAA) and 0xFD-0xFF (`OP_REG_R`, 0x20). Reads only,
   no NVM opcodes.

If step 8 fails on both models, fingerprint stays blocked for good on
mainline Linux, and the README line can say so.
