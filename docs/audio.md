# Audio: ADSP + WCD9335 (headphones, microphones)

## Status (2026-10-28): stage 1 works

With `dts/test/test-audio.dts` (merged into the main DTS on 2026-10-28), kernel patches 0010-0011 and the device
package's UCM profile and WirePlumber config:

- **Headphones:** clean playback. Checked by ear: a test tone, then speech
  through an echo loop.
- **Microphones:** the main mic (AMIC3) and sub mic (AMIC4) record cleanly,
  also checked by ear through the echo loop.
- **Both work through plain ALSA and through PipeWire.** WirePlumber picks the
  UCM "HiFi" profile by itself and offers a "Headphones" sink and a
  "Built-in microphones (main, sub)" source. Repeated play/record cycles, node
  suspend/resume and WirePlumber restarts run without DSP errors.

Not done yet:

- **Speakers.** All four sit behind Richtek RT5508 smart amplifiers on I2S
  (`spkr_i2s` pins). There is no mainline driver; Samsung's `rt5508.c` would
  need porting.
- Headset mic (AMIC2), jack detection (WCD9335 MBHC; `apq8096.c` sets up no
  jack), and mic gain tuning.

## Hardware and firmware

| | |
|---|---|
| ADSP firmware | Samsung-signed `image/adsp.mdt` + `.b00`-`.b09` from the **modem** partition (`ADSP.8996.2.7.1.C1-00013`; the `dsp` partition only holds ADSP modules). Copy to `/lib/firmware/qcom/apq8096/samsung/gts3lwifi/`; not in this repo |
| ADSP memory | the image spans 0x2100000 bytes, more than msm8996.dtsi's 27 MB `adsp_mem`. The test DT grows it to 0x8ea00000-0x90f00000 and moves the unused `slpi_mem` to 0x91700000, inside the stock `peripheral_region` |
| Codec | WCD9335 v2.0 on SLIMbus (NGD at 0x91c0000), PGD `1,0`, IFD `0,0` |
| Codec reset | TLMM 74 (stock `wcd_gpio_ctrl`). The Xiaomi boards use 64 |
| Codec interrupt | TLMM 79, one line (`intr1`). The Xiaomi boards use 54/53 |
| MCLK | 9.6 MHz, PM8994 GPIO 15 function 1 (RPM `div_clk1`), as on db820c |
| Supplies | PM8994 S4 1.8 V for buck, sido, tx, rx and io |
| Mics (stock routing) | AMIC1 <- MIC BIAS4 (third), AMIC2 <- MIC BIAS2 (headset), AMIC3 <- MIC BIAS1 (main), AMIC4 <- MIC BIAS3 (sub) |

Routing in the UCM profile (`pmaports/device-samsung-gts3lwifi/ucm2-*`):

- playback: MultiMedia2 -> SLIMBUS_6_RX -> AIF4_PB -> SLIM RX5/RX6 ->
  RX INT1/INT2 mix -> HPHL/HPHR;
- capture: MultiMedia3 <- SLIMBUS_0_TX <- AIF1_CAP <- SLIM TX0/TX1 <-
  DEC0/DEC1 <- AMIC3/AMIC4.

The headphone settings follow Samsung's `mixer_paths_tasha.xml` ("headset"
path): compander on (`COMP1/COMP2 Switch`), `RX HPH Mode` class-H low power,
`HPHL/HPHR Volume` 20, `RX1/RX2 Mix Digital Volume` 74.

The db820c UCM sets `RX5/RX6 Digital Volume`, which is not on this path. With
the mix volume left at 0 dB and the compander off, the tone was distorted.

## What had to be fixed

1. **Capture recorded digital silence** unless something was playing. In the
   wcd9335 driver only the RX DACs (through `RX_BIAS`) pull in the `MCLK`
   supply. The test DT routes `MIC BIAS1-4` to `MCLK` as well.
2. **The ADSP refused to start the SLIMbus port** for PipeWire
   (`AFE enable for port 0x4001 failed`). `apq8096_be_hw_params_fixup()` fixed
   rate and channels but passed S24_LE through. Patch 0010 pins the back end
   to S16_LE, like sdm845.c and sm8250.c do.
3. **Re-prepare left a stream open in the DSP.** `q6asm_dai_prepare()` only
   closed RUNNING streams. After a STOP the next prepare opened the stream a
   second time (DSP error 9, EALREADY), then freed the audio client, and audio
   stayed dead until reboot. It also never reset `queue_ptr`. Patch 0011 fixes
   both. PipeWire hits this constantly, `aplay`/`arecord` never.
4. **Capture periods.** q6asm-dai allows at most 4096-byte capture periods
   (1024 frames of S16 stereo). postmarketOS' generic `soc-qcom` 51-qcom.conf
   asks for 4096-frame periods, so every capture open fails with EINVAL. The
   device package ships its own `51-gts3lwifi-audio.conf` (playback 4096 x 6,
   capture 1024 x 4, S16LE, 48 kHz). The same probably affects other q6asm
   devices that use 51-qcom.conf.
5. **UCM verb disable sequence.** PipeWire opens each PCM through its own UCM
   context. Tearing the shared ADSP routes down when one context closed cut
   the other direction off mid-stream (an intermittent beep), so the verb has
   no disable sequence.

Also seen: unbinding and rebinding the sound card fails because wcd9335 does
not free its SLIM slave interrupt (`request_irq ... -EBUSY`). Reboot instead
of reloading the machine driver.

## Testing headless

`tests/audio/` has the scripts that were used:

- `hp.sh` sets up the headphone routing;
- `mic2.sh` records two mics;
- `mclktest.sh` captures with and without playback;
- `echo.sh` and `pwecho.sh` record 5 s and play it back (ALSA, PipeWire);
- `pwstress.sh` runs play/record cycles with suspend and a WirePlumber
  restart.

Notes:

- Stop `pw-record` with SIGINT (`timeout -s INT`), or the WAV keeps only its
  header.
- Background jobs over ssh: use `systemd-run` (or `systemd-run --user` with
  lingering enabled); `sudo` jobs die with the session.
