# Display: Samsung AMS968HH01 (ANA38401 DDI) on the Galaxy Tab S3

Everything below comes from the downstream tree
(`drivers/video/msm/mdss/samsung/ANA38401_AMS968HH01/`, the board dtsi and
Samsung's `ss_regulator_common.c`). Nothing has been tried on hardware yet.

## Panel facts

| Item | Value |
|---|---|
| Panel / DDI | Samsung AMS968HH01 AMOLED, Anapass ANA38401 driver IC |
| Resolution | 2048 x 1536 (QXGA), 196 x 147 mm, 24 bpp |
| Interface | 2 x MIPI DSI, 4 lanes each, **command mode**, split: each link drives 1024 x 1536 |
| DSI clock | 893.2 MHz per lane (downstream `panel-clockrate`), continuous clock (`force-clock-lane-hs`) |
| Per-link timing | hbp 108, hfp 212, hpw 16, vbp 6, vfp 10, vpw 2, 60 Hz |
| Full mode | 2048x1536, htotal 2720, vtotal 1554, pixel clock 253.6 MHz (msm host halves h-timings per link) |
| Tearing | TE on TLMM GPIO 10 (`mdp_vsync`), tear scanline 1526 |
| Commands | DCS, sent on link 0 only (downstream `qcom,dcs-cmd-by-left`), init in LP mode |
| Brightness | via DCS gamma/AID/ELVSS tables (Samsung "smart dimming"): backlight device, 256 levels over the vendor's 65 steps from 2 to 360 cd/m2 (patch 0042) |

GPIOs (all TLMM): reset 143 (high = running), TCON_RDY 45 (input, DDI raises it
when ready), LCD_LDO_EN 46 (panel 3.3 V), TE 10.

## Power-on sequence (downstream, verified in code)

1. LCD_LDO_EN (GPIO 46) high, wait 10 ms (`ssreg-vdd`, post-on-sleep 10)
2. DSI clocks on, lanes in LP11 (`lp11-init`)
3. wait 75 ms (`ssreg-panelrst` pre-on-sleep, "requested by HW team")
4. reset GPIO 143 high (`panel-rst-seq = <1 0>`)
5. poll TCON_RDY (GPIO 45) until high, up to 1 s
6. wait 1 ms (`init-delay-us`), send the on-command list in LP mode
7. last register write (B0 D0 / B1 1D, "UPLL_F = 35") is followed by 215 ms
8. DCS 0x29 display on (+50 ms)

Power-off: DCS 0x28 (100 ms), reset low, 5 ms, LCD_LDO_EN low, 10 ms.

## On-command list (decoded)

`B0 xx` selects a register offset ("global parameter"), `B1`/`B2` write it.

| Command | Meaning (downstream comment) |
|---|---|
| B0 34 / B2 64 00 | check fail off |
| B0 75 / B2 28 | INTR setting |
| B0 51 / B1 06 | dual DSI |
| B0 D9 / B1 08 | Anapass compression mode |
| B0 7F / B2 15 | TSP HTE/VTE (touch sync) |
| 44 05 F6 | set tear scanline 1526 |
| B0 BC / B2 04 | gamma offset index |
| 83 + 33 bytes (0x80/0x00 pattern) | brightness condition set, 360 nit |
| 90 00 00 0E | AID, 360 nit |
| B0 67 / B2 0F | ELVSS, 360 nit |
| B0 4D / B2 04 | 16-frame average, ACL off |
| B0 36 / B2 10 | ACL off |
| B0 6D / B2 85 | CAPS on |
| B0 35 / B2 01 | gamma update key |
| B0 D0 / B1 1D, 215 ms | UPLL_F = 35 |
| 29 | display on |

The USA panel variant adds `B0 56 / B1 4C` (porch setting, Htotal 3224) and
uses a 895.4 MHz DSI clock. Panel revision is read from DCS DA/DB/DC.

## Status (2026-09-28)

First hardware test of the driver (kernel r2, `dts/test/test-display.dts`):
the panel powers up, both DSI links bind, `/dev/dri/card0` with a DSI
connector appears and the framebuffer console is visible on the panel.
The picture was wrong: text squeezed/stretched into vertical streaks, colours
shifted (red shown as green, green as magenta, blue as yellow), one
`pp done time out` in dmesg. Touch (Synaptics S5807, `test-touch.dts`) works.

**Root cause (found 2026-09-28, in the MDP5 driver, not the panel):** mainline
`drivers/gpu/drm/msm/disp/mdp5` has no split-display support for a bonded
DSI pair. The slave DSI (dsi1) gets an encoder but no connector, and the
CRTC only uses a second layer mixer when the mode is wider than one mixer
(2560 px on msm8996). So the whole 2048-wide frame went through one mixer
into INTF1/dsi0 only, while the dsi0 host was configured for a 1024-wide
command-mode packet: every panel line got half a frame line, and the odd
packet length shifted the RGB bytes. dsi1 never received pixels.

Fix: `pmaports/linux-postmarketos-qcom-msm8996/0003-drm-msm-mdp5-split-display-bonded-dsi.patch`
(sources mirrored in `kernel/drivers/gpu/drm/msm/disp/mdp5/`). It links the
slave encoder to the master, forces a right mixer for bonded DSI, programs
the slave CTL/pingpong from the master path and enables the SPLIT_DPL
single flush + SW trigger mux (what Samsung's bootloader also uses: CTL0 and
CTL1 with SPLIT_DPL_EN=1). Module parameter `msm.split_start_both=1` writes
START on both CTLs, as a fallback experiment.

Round 9 (2026-09-29): the split path activates ("bonded DSI: split display,
master DSI0") but the screen stays black: every frame ends in `pp done time
out, lm=2` and the DCS display-on command times out (-110) once the pixel
stream is started. Two differences from downstream were found and fixed for
round 9b: the mixer allocator picked LM2/LM5 instead of LM0/LM1 (it preferred
the *last* pair-able mixer), and downstream command mode does not use the
single-flush bit at all: it writes FLUSH to both CTLs itself, with the same
value in the SPLIT_DPL upper and lower registers, and START on the master
only.

Round 9b: much better log (LM0, display-on succeeds, only the usual first-frame
`pp done` timeout, no errors) but the screen is still black. Reading: DSI0 now
sends exactly its 1024-wide half and the DDI waits for the other half from
DSI1, which apparently never arrives (in round 7 the picture appeared only
because DSI0 pushed twice the data and the DDI's frame wrapped). lk2nd's
known-good dual-DSI command path (split regs upper=lower=BIT(1)|BIT(8),
CTL1 op = INTF2|CMD, LM1 via CTL1, flush both CTLs, START on CTL0, tear
check on PP0+PP1) matches what the patch now does, so round 9c adds a
register dump (INTF1/INTF2 frame counters, PP counters, CTL/split regs) to
see whether INTF2 produces frames at all.

Round 9c dump: `intf2 frame 0`, and the right mixer was **LM5 / PP3**, not
LM1 / PP1. The mixer allocator stored the pair of every pairable candidate it
looked at, so LM0 ended up with LM2's partner. INTF2 is fed by PP1, so it
never saw a frame. Fixed for round 9d (pair assigned together with the
chosen left mixer).

Round 9d: log identical to 9c (still LM5/PP3) although the module was built
with the fix. Suspect the tester's tablet rolled back to the previous module
(overwrite + power cut without sync). Round 9e prints a version tag and the
mixer choice, and the README has the tester verify the module checksum.

**Second route, DPU (round 10):** this kernel already carries the upstream
MDP5-to-DPU migration for msm8996 (`disp/dpu1/catalog/dpu_1_7_msm8996.h`,
selected with `msm.prefer_mdp5=0`). DPU has real bonded-DSI support (two
h-tiles, split display CTLs). The msm8996 catalog lacked the
`DPU_CTL_SPLIT_DISPLAY` flag on its CTLs, which the resource manager needs
for a two-interface topology; patch 0004 adds it to CTL_0/CTL_2 like
msm8998. Round 10 tests both drivers from one module.

**Round 9d, real result (2026-10-08):** the tester had re-run the old module
before; with the real v4 module the split display works: LM0+LM1, PP0+PP1
counting frames in lockstep (~53 fps), no timeouts, and the panel shows the
colour fills. Colours are still mapped wrong (red shows green, green magenta,
blue yellow, white white), the same as round 7, so this is independent of
the geometry. Explanation found in the downstream driver: the ANA38401 has
an mDNIe colour processing block (ASCR colour matrix) that downstream
programs at every panel-on; mainline never touched it. Patch 0005 sends the
vendor "bypass" (identity) tables after init.

Round 10: MDP5 v5 confirmed (LM0/LM1, lockstep frames, no errors), colours
unchanged with the mDNIe tables sent at init time in LP mode. Downstream
sends them *after* display-on, in HS mode, so v7 does the same and logs it.
DPU route: binds, fb0 appears, no errors, but black screen; parked while
MDP5 works.

Round 11: "mDNIe bypass programmed (v7)" confirmed in dmesg, colours still
green/purple/yellow, so the panel's colour block is not the cause. The
console photo from round 10 proves the geometry (text readable, lines along
the long side, first rows at one edge). Round 12 makes the remaining
candidates selectable at boot: DSI component order (`msm.rgb_swap=0..5`,
patch 0006), DCS COLMOD 0x77 (`panel_samsung_ams968hh01.colmod=0x77`), and
MDP5 now logs and clears the DSPP op-mode on both mixers.

Round 12 results: DSPP op-mode was only 0x20000 (histogram auto-clear), so
the post-processor is innocent. The component-order sweep is the key
result: whatever colour is put in the first byte of a pixel shows green,
the second shows purple, the third yellow (A: G/P/Y, B=BGR: Y/P/G, C=GBR:
Y/G/P, D=BRG: P/Y/G), so the DDI applies a fixed mix to each input
channel and no reordering helps. COLMOD 0x77 changes nothing.

Remaining suspect: the 33-byte "brightness condition" (gamma) command.
The vendor smart-dimming code gives its layout (V255 R/G/B as 9-bit pairs,
then V203..V3 and VT per channel); the DTS power-on placeholder we copied
puts 0x80 in the V255 high bytes (out of range, possibly control bits) and
zeros in scattered channels. Downstream overwrites it with a computed
table before showing anything. Round 13 sends the vendor reference table
(0x0100 / 0x80 / 0x00) instead, reads back the panel ID, the mDNIe
register and the gamma register to prove register access, and ships
colour-bar and gradient test images for photos.

Round 13 results (2026-10-19): reference gamma changes nothing. Readback
works for the panel ID (DA/DB/DC = 42 40 16); B9/83 are not readable that
way. The photos are the real information:
- colour bars: only **four** bands where eight were sent, matching the
  first four bars each twice as wide: both links contribute to every
  displayed pixel, the panel does not place the links side by side.
- per-channel gradients: smooth ramps show as **hue stripes** with a short
  period, which only a bit-level reinterpretation of the pixel stream can
  produce (no channel swap or colour matrix can).
A brute-force simulation (`scripts/pixel-stream-sim.py`) over stream
decodings finds that taking 12 bits of every pixel from each link
reproduces the fills (R teal, G purple, B yellow, W white) and the
striped ramps. Hypothesis: the ANA38401's "dual DSI" mode treats the two
links as one wide link (each link carries part of every pixel), not as
left/right halves. Round 14 sends link-isolation images (bars on one link
only, red/blue halves, ramps and stripes on one link) to confirm the
exact assembly before changing how MDP5 feeds the links.

Round 15 adds a decisive split test: `/sys/kernel/debug/dri/0/dsi_tpg`
(patch 0007) switches both DSI hosts to their built-in test pattern
generator, bypassing MDP entirely. A clean pattern means DSI + panel are
fine and the scramble is in the MDP-to-DSI pixel hand-off; a scrambled
pattern means the DDI's dual-link mode is not what we feed it. The round
also collects `/sys/kernel/debug/dri/0/state` and `kms`.

Rounds 14 + 15 results (2026-10-20):
- bars sent on **one** link only (other link black) show as clean, evenly
  spaced bars across the **whole** screen, colour-mapped exactly like the
  fills (W white, R green, G purple, B yellow, Y lavender, C salmon,
  M pale green). So each link spans the full width and the colour scramble
  is inside one link's own byte stream.
- DSI TPG (display engine bypassed): clean rectangle grid, scrambled
  colours (green/red/olive/lavender). The problem is between the DSI
  controller and the DDI.
- drm state: LM0 + LM1, one DMA0 pipe 2048 wide, XR24, as intended.
Conclusion: the DDI is not in the mode the vendor init puts it in. The
one difference left in how we talk to it: the vendor sends every B1/B2
register write as a DCS **long** packet (type 0x39) even for 1-byte
payloads; the mainline helpers use short packets (0x15) for <=2 bytes.
Round 16 sends the init with the vendor packet types
(`panel_samsung_ams968hh01.long_writes`, default on), which would
explain "dual DSI setting"/"compression mode" never taking effect.

**Independent confirmation from the Galaxy Tab S2 9.7 (2026-10-21).** The
Tab S2 uses the same panel and DDI (ANA38401 + AMS968HH01, 2048x1536):
- Exynos 5433 model (pmaports `linux-samsung-gts210lte`, kernel
  `universal5433`, `drivers/video/exynos/panels/ana38401_*`): the panel is
  driven over **one** 4-lane DSI at 896 Mbps/lane with Samsung **MIC**
  (Mobile Image Compression, fixed 2:1, i.e. 12 bits per pixel) enabled
  (`mic = <1>`). Its init sends INTR, TSP, tear scanline, gamma offset
  index, brightness and UPLL only; it never writes "compression mode"
  (B0 D9/B1 08) or "dual DSI" (B0 51/B1 06).
- Snapdragon msm8976 "VE" model (`linux-samsung-gts210vewifi`) and the
  Tab S3: dual DSI, uncompressed, with those two writes added at the top
  of the init.
So the DDI's power-on default is "MIC-compressed, single stream, 12 bpp",
and the two Qualcomm-only writes switch it to uncompressed dual-link.
Everything we observed (clean geometry, 3072 bytes per line per link
consumed as 2048 x 12 bits, both links in every pixel, value-dependent hue
stripes) is the DDI still in its default mode: our B1 writes are not being
applied. Round 16 (vendor long-packet types) is the direct test.

Round 16 (2026-10-23): no change with long packets (caveat: the panel module's version tag was not bumped for v10/v11, so the log cannot prove the v11 panel module ran; round 17 tags v12). The DSI host
configuration was then cross-checked register by register against the
downstream driver and the round 15 register dump (CMD_CFG0 = RGB888, no
colour select, TRIG_CTRL TE + SW DMA trigger, EoT append on, burst mode
on): identical. So whether the two mode writes reach the DDI is the only
open question. Round 17 reads them back through the DDI's own offset-read
mechanism (B0 offset, then a DCS read, as the vendor reads MTP data) and
adds `dual_dsi=`, `cmode=` and `late_mode=1` (rewrite after display-on in
HS) as boot-time knobs.

**Stock register reference (2026-10-24).** Jayden Milne captured the live
MDSS/DSI state of a running SM-T820 under the vendor 3.18 kernel:
https://gist.github.com/jaydens-tasty-slop/8829d71d3dc54a8ab7e42e80b0f79d42
(read-only capture, plus a write-up of the 3.18 debugfs traps). Diffed
against our round 15 dump: MDP top, split-display registers (EN 1,
upper = lower = 0x102), CTL_0/1 op modes, LM out sizes, tear check and the
DSI command-mode registers all match. Real differences:
- **link rate**: stock 893.2 Mbit/s per lane (pclk 148.867 MHz, byte clock
  111.65 MHz per link, both links on the DSI0 PLL); ours was 760.8. The
  DDI's UPLL value (B0 D0 / B1 1D) is set for the stock rate. Round 17 now
  uses a mode that reproduces the stock rate (htotal 3194, clock 297733,
  vrefresh still 60); `slow_link=1` restores the old one.
- minor: stock programs MDP stream 1 like stream 0, EoT RX-ignore set,
  CLKOUT timing 0x0d30 vs our computed 0x0c2b, HS timer values.

Round 17 (2026-10-25):
- Boot A (stock 893.2 Mbit/s link): no visible change, colours as before.
  Readback with DCS reads (0x06) of B1/B2 returned 0x16 for every offset,
  i.e. the last ID byte again: the DDI does not answer DCS reads of the
  write ports, so these values mean nothing.
- Boot B (`late_mode=1`: B0 51/B1 06 and B0 D9/B1 08 rewritten after
  display-on, in HS): the console area blinks on and off, and afterwards
  the DDI answers no reads at all (even the ID). So writing those two
  registers while the panel runs changes its state, which suggests the
  same writes at init time had not taken effect (identical values should
  be a no-op otherwise). Candidate cause: init sent before the T-CON has
  finished booting (Galaxy Tab S6 port: ~330 ms after reset, commands
  before that are silently dropped).
Round 18 logs TCON_RDY before reset release and how long it takes to go
high, reads registers the vendor way (B0 offset, then GENERIC read with two
parameters, type 0x24; D8@5A MTP as a sanity check), right after init and
after display-on, and boot B adds `init_delay_ms=500` before init.

Round 18 (2026-10-27):
- TCON_RDY is a real handshake: low before reset release, high 61 ms later.
  So the init is not sent before the T-CON says it is ready.
- The vendor read method works: B0 5A + generic read(D8, 8) returns real
  MTP data (1f 47 4e 3d 34 51 5b 36) after display-on. The B1/B2 write
  ports cannot be read back by any method tried, so the two mode
  registers cannot be checked directly.
- **Every read during prepare fails, even the panel ID**, and works after
  display-on. 500 ms of extra delay changes nothing. So during prepare(),
  where the whole init sequence is sent, the DDI is not receiving or not
  answering our commands, while the same link works later. Combined with
  round 17 boot B (rewriting the mode registers after display-on visibly
  changes the panel), the most likely story is that the init sequence sent
  in prepare() never takes effect and the panel runs on power-on defaults
  (MIC 12 bpp) until something is written later.
- Difference from the vendor sequence noted for later: stock holds all
  lanes in LP-11 during reset and only then forces the clock lane to HS;
  mainline forces continuous HS clock at host power-on, before the panel
  comes out of reset.
Why the DDI might ignore everything during prepare(): the vendor brings
the link up as LP-11 on all lanes through the panel reset, and only then
forces the clock lane into continuous HS before the LP init commands.
Mainline forces continuous HS at host power-on, i.e. *before* our
prepare() releases the panel reset, so the DDI comes out of reset with the
clock already running and never sees a clock-lane HS entry sequence. If
its command interface runs from the DSI clock (it derives its UPLL from
it), it would drop LP commands and reads until something re-syncs it,
which fits every observation so far.

Round 19 tests this directly: boot B uses `msm.defer_hs_clk=1` (patch
0006: the clock lane stays LP-11 until the first command after the panel
reset, then goes HS, like the vendor), boot C sends the init from enable()
(`init_in_enable=1`). `late_init=1` (init again after display-on) is also
available. Readbacks now also run just before display-on, and the dmesg
filter includes DSI host messages.

Round 19 (2026-10-28/29), run on a second SM-T820 with a webcam on the
screen, scored against webcam photos of the stock kernel (brightness 128).
Same patch stack on v6.19.5-msm8996 plus a local watchdog patch unrelated
to the display; `dts/test/test-touch.dts` with the GPU and Wi-Fi PCIe off:
- Boot A: as before. Boot C (`init_in_enable=1`): the panel lights, but
  with a scrambled frame.
- **Boot B (`msm.defer_hs_clk=1`) confirms the clock-lane theory.** The DDI
  now answers reads in prepare(), before the init (ID 42 40 16, MTP from
  D8@5A), and the init sequence takes effect. `late_init=1` alone gives the
  same picture as boot B. Other differences from the vendor sequence,
  tried one at a time with options from a local bring-up patch (a VDD
  power cycle at the first prepare, the vendor's manufacture_read_pre_tx
  block, display-on at the end of the init list plus tear-on, a
  non-continuous clock), each left the screen black without
  `msm.defer_hs_clk=1`.
- With the init taking effect, blue gave no light at all and every level
  below about 190/255 was black. That was the gamma table (register 0x83):
  the DDI takes 11 bytes per channel, R then G then B, each V11, V23, V35,
  V51, V87, V151, V203, V255 bits 7:0, V3, V255 bit 8 (as 0x80) and VT.
  The reference table we sent was in the vendor smart dimming code's
  internal order; the vendor converts its tables to register order in
  `gamma_command_sorting_post()` before sending. The vendor DT
  `gamma_tx_cmds` that we took for a placeholder is the nominal table in
  register order. For its 360 cd/m2 table the vendor adds the panel's 33
  MTP bytes (D8 from 0x5a, already in register order). Kernel patches
  0038-0040 send nominal + MTP and the vendor's 360 cd/m2 ELVSS (0x19).
- The MTP differs per panel (this unit: `41 56 5c 49 3f 56 5d 25 02 00 00
  ...`, the round 18 tester's: `1f 47 4e 3d 34 51 5b 36`), so the driver
  reads it. The table it sends matches, byte for byte, the one the stock
  kernel computes for the same panel. The stock kernel prints all its
  tables and the MTP offsets when `/sys/devices/virtual/lcd/panel/aid_log`
  is read (root, no unlock needed).
- **Result with 0038-0040 and `msm.defer_hs_clk=1`: correct picture.**
  Geometry and colours right, smooth red, green, blue and white ramps.
  Webcam against stock: colour bars 8/8, halves 2/2, colour grid 12/16;
  the four misses (grey, dark green, dark blue, orange's green half) have
  the right hue at about half the stock level. That is a tone-curve
  difference: stock ran Samsung's UI mDNIe mode at brightness 128, this
  runs mDNIe bypass at 360 cd/m2.
- With the init taking effect but the old gamma table, the DSI test
  pattern showed only red, green and black cells: it also passes through
  the DDI gamma.
- **Second power-on (patch 0041).** Blank then unblank left the panel
  dark: the DDI answered reads only after display-on, the boot A
  signature. `dsi_ctrl_enable()` builds `DSI_LANE_CTRL` from the
  register's current value, and nothing clears `CLKLN_HS_FORCE_REQUEST`
  (bit 28) at power-off, so with `msm.defer_hs_clk=1` the clock lane was
  in HS again through the panel reset. The vendor writes 0 to the
  register at every controller init (`mdss_dsi_host_init()`). Patch 0041
  clears the bit when the start is deferred; it only changes anything
  with `msm.defer_hs_clk=1`, so it can be folded into 0006. Tested with
  four blank/unblank cycles in one boot: the same picture and scores
  after each, and the DSI controller, PHY and PLL registers
  (`/sys/kernel/debug/dri/*/kms`) identical before and after.
- **Brightness (patch 0042).** The panel registers a backlight device
  (`/sys/class/backlight/994000.dsi.0`, 0-255, default 255) and sends
  the vendor brightness update: AID per level, then ELVSS, CAPS and the
  gamma table of the level's luminance step (65 steps, 2 to 360 cd/m2),
  then the update key, in HS. The tables below 360 cd/m2 are computed
  from the MTP the way the vendor smart dimming code does; the 65 tables
  the driver computes on this panel equal the stock kernel's `aid_log`
  tables byte for byte. White gets dimmer at every lower level; a
  blank/unblank keeps the level. At level 174, the level of the stock
  reference photos (234 cd/m2), the colour grid scores 15/16 against
  stock (bars 8/8, halves 2/2): grey, dark green and dark blue now match,
  so the 12/16 above was the 360 cd/m2 tone curve. The one miss (pink
  read as orange) is the webcam clipping red; the stock photo shows the
  same. Level 255 sends the same bytes as before.
- **ACL (patch 0043).** ACL (automatic current limiting) makes the DDI
  lower the emission current when the average picture level is high,
  which caps the panel's total current. The vendor kernel has it on by
  default at 15%: its on-list writes ACL off and the brightness update
  after display-on turns it on (B0 4D/B2 05, 32-frame average; B0 49/B2
  7F, start setting; B0 36/B2 12) and takes ELVSS from its ACL table
  (0x1b at 360 cd/m2 instead of 0x19; the two tables differ on the six
  steps from 265 to 360 cd/m2). Patch 0043 does the same at init. Tested:
  small white squares (3% of the screen) read the same as with ACL off,
  while full-screen white at the same spots reads about 13% lower on the
  webcam (white-to-squares ratio 0.61, against 0.68 to 0.70 in three
  ACL-off boots); full-screen white is lower at every brightness level;
  the colour scores against stock are unchanged.
- Still open after round 19: `msm.defer_hs_clk=1` on the command line
  (patch 0006 keeps it off by default); HBM; `pp done time out, lm=0`
  at each blank and unblank (a 50 ms wait each, not the 2 s between the
  two log lines, which was the test script's pause). Every boot logs
  `pclk0/1_clk_src: rcg didn't update its configuration` warnings at the
  first modeset, with or without these patches.

Round 20 (2026-10-29, same tablet and webcam):

- **No command-line option (patches 0050-0052).** Patch 0050 adds a
  mode flag, `MIPI_DSI_CLOCK_DEFERRED_HS`, with which a panel asks the
  DSI host to keep the clock lane in LP-11 until the panel has been
  reset and has sent its first command (the vendor's
  `qcom,mdss-dsi-lp11-init` with `force-clock-lane-hs`). Patch 0051 makes
  the MSM DSI host honour it in place of the `msm.defer_hs_clk`
  parameter of patch 0006 (the clock lane goes to HS at the first
  transfer after the panel's prepare, or at bridge enable if the panel
  sent nothing; the stale request bit of patch 0041 is still cleared),
  and patch 0052 sets it in the panel driver. With patches 0001-0054
  (plus a test-only patch of bring-up parameters, all at their defaults)
  and no `msm.defer_hs_clk` on the command line, the panel lights at the
  first power-on and through five more blank/unblank cycles, the DDI
  answers reads in prepare before the init list every time, and the
  webcam scores are as in round 19 (colour grid 14/16 at level 255,
  15/16 at level 174 with the stock camera profile, bars 8/8, halves
  2/2). The negative control, the same kernel with the panel's flag
  turned off by a test-only parameter, stays black and the DDI answers
  only after display-on, as in round 19's boot A. Patches 0006 and 0041
  stay applied; 0051 removes the parameter they use. With 0006 and 0041
  folded into one patch, 0051 is the clean form of both.

The lk2nd framebuffer (simplefb) route was abandoned: Samsung's bootloader
sets the display engine up in a way lk2nd's autorefresh cannot take over
(see docs/lk2nd.md).

## Mainline plan

- **DSI supplies** (from `drivers/gpu/drm/msm/dsi/dsi_cfg.c`, msm8996):
  vdda = PM8994 L2 1.25 V, vcca = PM8994 L28 0.925 V, vddio = PM8994 L14 1.8 V.
  PHY (`qcom,dsi-phy-14nm`): vcca = L28.
- **Dual link**: `qcom,dual-dsi-mode` on both controllers, `qcom,master-dsi` on
  dsi0, dsi1 clocked from the dsi0 PHY (as on sdm845-mtp). The panel node lives
  under dsi0 with two input ports.
- **Panel driver**: `kernel/drivers/gpu/drm/panel/panel-samsung-ams968hh01.c`,
  modelled on `panel-truly-nt35597.c` (two DSI devices) with the `_multi`
  helpers. Commands go to dsi[0] only. `prepare_prev_first` makes the DSI host
  power the lanes (LP11) before `prepare()`, which matches step 2.
- **Device tree**: `dts/test/test-display.dts` (compiles; not yet in the main
  DTS because the driver is untested).
- **Build**: the panel driver must be built into the kernel package
  (`pmbootstrap build --force linux-postmarketos-qcom-msm8996` with the extra
  patch); DRM_MSM is a module, so afterwards `msm.ko` can be iterated over ssh.

## First test, before any of this

`fastboot --cmdline "console=tty0 lk2nd.pass-simplefb=autorefresh,xrgb8888 clk_ignore_unused pd_ignore_unused" boot boot.img`
uses lk2nd's framebuffer. If kernel text shows up, the panel keeps running
under Linux and only the driver work remains.

## Open questions

- Whether commands really must go to link 0 only under mainline (the DDI is
  one chip; downstream never talks to link 1). If init fails, try dual writes.
- `qcom,sync-dual-dsi`: not set downstream (`cmd-sync-wait-broadcast` is
  commented out); start without it.
- Brightness: HBM is not implemented; ELVSS uses the vendor's
  normal-temperature table only.
