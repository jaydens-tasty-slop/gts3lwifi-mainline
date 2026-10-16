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
| Brightness | via DCS gamma/AID/ELVSS tables (Samsung "smart dimming"). Not done yet; init leaves the panel at 360 nit |

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
- Brightness control: needs the gamma/AID/ELVSS tables from the downstream DTS
  (`samsung,*_tx_cmds_revA`, `candela_map_table`). A fixed 360 nit is fine for
  bring-up but is bright and warm for an AMOLED; add a simple backlight later.
