# Egis ET510 protocol facts

Facts for a future Linux/libfprint driver, should the SPI bus ever become
the application processor's (see `fingerprint.md`). Sources:

- **GPL**: Samsung's msm-3.18 kernel (awesometic/android_kernel_samsung_msm8996,
  at 53683713fd1f), `drivers/fingerprint/et5xx.h` and
  `drivers/fingerprint/et5xx-spi{,_data_transfer}.c`. This is the vendor's
  own GPL-2.0 driver, so the wire format below is authoritative, not
  reverse-engineered.
- **Observed**: stock boot/HAL logs from this SM-T820.
- **Blob (facts only)**: the `dualfp` trustlet and `libbauthserver.so`,
  inspected for structure and strings only. No code was copied. Blobs live
  under `out/pmos/fingerprint/` (gitignored), never in git.

The register *values* for init and calibration, the detect-mode
configuration and the exact image geometry are **not** in the GPL driver:
in Samsung's shipping build every SPI transfer helper is compiled out
(`#ifdef ENABLE_SENSORS_FPRINT_SECURE return 0;`), and the real sensor I/O
runs inside the `dualfp` trustlet. Those items are marked "TA-resident
(decompiler needed)" below.

## Transport

- SPI mode 0 (CPOL 0, CPHA 0), 8 bits/word, MSB first, CS active low
  (`et5xx-spi.c:1144-1146`: `spi->mode = SPI_MODE_0; bits_per_word = 8`).
- Clock: 12.5 MHz nominal (`SLOW_BAUD_RATE`, `et5xx.h:73`); the sensor node
  allows up to 16 MHz, the controller up to 19.2 MHz. The stock HAL drives
  12.5 MHz for register work and briefly 4 MHz (observed
  `FP_SET_SPI_CLOCK, clock = 12500000 / 4000000`).
- Every transaction is opcode byte first, then payload, full-duplex; read
  data comes back in the same `spi_sync` buffer, offset by the opcode (and
  address) bytes already clocked out.

## Opcode map

From `et5xx.h:57-70` (the `OP_*` constants) and the framing in
`et5xx-spi_data_transfer.c`:

| Opcode | Name | Direction | Wire format |
|---|---|---|---|
| 0x20 | `OP_REG_R` | read 1 reg | tx `[0x20, addr, 0x00]`; value is rx byte 2 (`etspi_io_read_register`) |
| 0x24 | `OP_REG_W` | write 1 reg | tx `[0x24, addr, val]` (`etspi_io_write_register`) |
| 0x22 | `OP_REG_R_C` | burst read | tx `[0x22, addr]` + N dummy bytes; data at rx+2 (`etspi_io_burst_read_register`) |
| 0x26 | `OP_REG_W_C` | burst write | tx `[0x26, val0, val1, ...]`; auto-incrementing from a start addr (`etspi_io_burst_write_register`) |
| 0x23 | `OP_REG_R_C_BW` | burst read, addr decrementing | "backward" variant |
| 0x27 | `OP_REG_W_C_BW` | burst write, addr decrementing | "backward" variant |
| 0x50 | `OP_IMG_R` | read frame | tx `[0x50]` + `size` clocked bytes; frame at rx+1, 8-bit/pixel (`etspi_io_get_frame`) |
| 0x40 | `OP_NVM_ON_R` | NVM read-enable | NVM access |
| 0x42 | `OP_NVM_ON_W` | **NVM write-enable** | **never send** |
| 0x44 | `OP_NVM_RE` | NVM read | NVM access |
| 0x46 | `OP_NVM_WE` | **NVM write** | **never send** |
| 0x48 | `OP_NVM_OFF` | NVM disable | NVM access |
| 0x60 | `OP_VDM_R` | VDM read | vendor data memory |
| 0x62 | `OP_VDM_W` | VDM write | vendor data memory |

**Hazard.** Opcodes 0x42 and 0x46 write the sensor's NVM, which holds its
factory calibration. A driver must never issue them. For bring-up, read-only
opcodes (0x20, 0x22, 0x50, 0x40/0x44/0x48 read side) are enough.

The frame read pads its length up to a multiple of 4 when it is 64 bytes or
more (`LARGE_SPI_TRANSFER_BUFFER=64`, `DIVISION_OF_IMAGE=4`,
`et5xx-spi.h:126-128`), a DMA alignment detail of the controller, not the
sensor.

## Known registers

From `et5xx.h:122-125` and the ID check in `et5xx-spi.c:872-928`:

| Reg | Name | Meaning |
|---|---|---|
| 0x00 | `FDATA_ET5XX_ADDR` | status/data; reads `0xAA` when the sensor is ready |
| 0x01 | `FSTATUS_ET5XX_ADDR` | bit 0 (`FRAME_READY_MASK`) = frame ready |
| 0xFD, 0xFE, 0xFF | chip-ID registers | identify the ET510 variant |
| 0x20, 0x21, 0x23, 0x24 | ID registers (ET510C) | alternate ID path |

Variant detection (`et5xx-spi.c:872-928`), after power-on and a 20 ms wait,
reading reg 0x00 first (must be 0xAA):

| Variant | 0xFD / 0xFE / 0xFF (or 0x20/0x21/0x23/0x24) |
|---|---|
| ET510C | 0x20=0x00, 0x21=0x66, 0x23=0x00, 0x24=0x33 |
| ET510D | 0xFD=0x03, 0xFE=0x0A, 0xFF=0x05 |
| ET516A | 0xFD=0x00, 0xFE=0x10, 0xFF=0x05 |

This tablet's node declares `etspi-chipid = "ET510"`; the live variant
(C or D) should be read in a hardware session (`fingerprint.md`, step 1c).

Interrupt/detect constants (`et5xx.h:76-77`): `ET5XX_INT_DETECTION_PERIOD`
10, `ET5XX_DETECTION_THRESHOLD` 10. Finger presence raises DRDY (TLMM 131),
handled as a wake IRQ (`et5xx-spi.c:119-127`).

## Power, reset, interrupt (GPL, Linux already owns these pins)

- Power on (`et5xx-spi.c:191-211`): LDO enable (PM8994 MPP 8) high, wait
  50-100 us, reset (`BTP_RST_N`, PM8994 MPP 2) high, select the active
  pinctrl, wait 10 ms.
- Reset pulse (`et5xx-spi.c:181-189`): `BTP_RST_N` low for ~1 ms, then high.
- Power off: reset low, then LDO low.
- DRDY: TLMM 131, pull-up active / pull-down in suspend; `enable_irq_wake`.

## Capture pipeline (architecture; TA-resident internals)

`dualfp` strings show the normal-world-visible stages: `[TIME_ANA] image
capture`, `[TIME_ANA] image quality check`, `average_10_frames`,
`fp_calibrate_image`, `get_calibrate_image_session`, `OutputImageType`. So a
capture averages ~10 frames and runs a quality/calibration pass before
matching. Enrolment and matching (`TZ_CMD_ENROLL_DO`, `TZ_CMD_IDENTIFY_INIT`)
and template storage (HMAC-wrapped) stay inside TrustZone and are out of
scope for a Linux capture driver.

The sensor is the front **home-button** sensor, i.e. a press/area capacitive
sensor (not a swipe sensor). A libfprint driver would use the press/image
path, with NBIS minutiae matching in the host.

## TA-resident, decompiler needed

The following are inside `dualfp` (and its linked Egis library) and are not
recoverable from GPL source or strings. They need a Ghidra decompile pass.
This pass was **not** done in this round: the only running Ghidra instance
was another session's `yp3` project (the rules forbid it), and the MCP
bridge connects only to running instances, so no clean instance was
available. A reconstructed, analysis-ready ELF is staged at
`out/pmos/fingerprint/blobs/dualfp_full.elf` (segments placed at their
program-header offsets; AArch64, code at vaddr 0, data at 0x8b000) to make
that pass quick when an instance is free.

To extract, facts-only (register addresses, values, tables, sizes — never
transcribed functions):

1. **Init / calibration register tables** — the `addr,value` sequences
   written at `et510_init_sensor` / `et510_calibrate_detect_mode` (names seen
   in `dualfp` strings). Start from the `sensor_test`/`NS`/`SNR` FPLOG format
   strings in the data segment and x-ref back to the writers.
2. **Detect-mode configuration** — the register writes that arm finger
   detection (threshold/period live here, the GPL `ET5XX_DETECTION_*`
   constants are only the driver-side poll parameters).
3. **Exact image geometry** — frame width x height and bits/pixel. The GPL
   frame read (`OP_IMG_R`) clocks a `size` the TA chooses; recover it from
   the capture buffer allocation / `average_10_frames` accumulator size. The
   TA comms shared buffer is 2008576 bytes (observed `QSApp SB Size`), an
   upper bound only.
4. **Any pre-image command sequence** — the order of register writes before
   `OP_IMG_R` for a single press capture.

Until that pass is done, a Linux capture path can be brought up far enough to
read the chip ID and poll `FSTATUS`, but not to pull a usable image.
