# Video codec: Venus under mainline

## Status (2026-10-28)

With `dts/test/test-venus.dts` and kernel patches 0008 and 0009:

- **Decoding works and is bit-exact.** H.264 (High, with B-frames), VP8 and
  VP9 at 720p decode to the last frame, byte-identical to ffmpeg's software
  decoders. Tested through GStreamer's V4L2 decoders (`v4l2h264dec`,
  `v4l2vp8dec`, `v4l2vp9dec`) and `tests/venus-dectest.sh`.
- **The decoder offers** H.264, HEVC, VP8, VP9, MPEG-2, MPEG-4, H.263, VC-1
  and Xvid. **The encoder offers** H.264, HEVC, VP8, MPEG-4 and H.263.
- **H.264 encoding works through `v4l2-ctl`**: 60 frames, valid High profile
  stream, clean stop.

Open:

- **HEVC decoding:** the firmware never reports the stream resolution, so
  GStreamer fails to negotiate ("Video device returned invalid dimensions").
  Seen with libx265 Main profile streams at 720p and 1080p.
- **Encoding through GStreamer (`v4l2h264enc`):** the firmware stops with
  `Err_Fatal ... venus\utils\src\vbuffer.c:1219` after about 5 frames, and the
  first frames are already wrong (19.6 dB). The I/O mode and padding make no
  difference. The driver recovers the firmware, but repeated crashes ended in
  a TrustZone reset. `v4l2-ctl` does not trigger it, so compare the two V4L2
  ioctl traces next.
- With `v4l2-ctl` defaults the encoder writes level 1.0 and no SPS cropping
  (1280x736 for 720p). Userspace normally sets the level; check the cropping.
- `ffmpeg -c:v h264_v4l2m2m` aborts in ffmpeg itself (`Assertion pkt failed`).
  Use GStreamer to test.

## Firmware and DT

Samsung's signed firmware is on the tablet's `apnhlos` partition
(`image/venus.mdt`, `.b00`-`.b04`, version `VIDEO.VE.4.4-00060`). Copy it to
`/lib/firmware/qcom/apq8096/samsung/gts3lwifi/`; it is not in this repo.
TrustZone authenticates it (PAS id 9).

The segments are relocatable and span 5 MB, so they load into msm8996.dtsi's
`venus_mem` at 0x91000000. That region lies inside the stock
`peripheral_region` (0x8f400000, 49 MB).

```dts
&venus {
	firmware-name = "qcom/apq8096/samsung/gts3lwifi/venus.mdt";
	status = "okay";
};
```

## What was broken (patches 0008, 0009)

1. **End of stream killed the session.** The driver signals EOS with an empty
   input buffer at the dummy address 0xdeadb000. This firmware checks every
   input buffer against its content-protection map. The firmware's own log
   (dynamic debug, `VenusFW`) says:

       IsBufferProtected: CP_UNKNOWN, buffer straddles CP & non-CP regions
       vDec_EmptyBuffer: Invalidate buffer in ETB

   The session dies (event 1001), every decode loses its last 7-12 frames,
   and the encoder crashes the firmware (SMMU faults, exception).

   A NULL address passes the check. But then the firmware returns the EOS on a
   NULL capture buffer ahead of frames still waiting for one, and the stream
   ends early whenever the consumer is slow. The vendor stack always uses a
   real buffer, so patch 0009 allocates a 4 KB DMA buffer on HFI v3 and
   signals EOS on it.

2. **LAST flag lost.** When the firmware reports EOS on a buffer tag the
   driver no longer holds, `vdec_buf_done()` dropped the flag, and userspace
   waited forever. Patch 0009 completes the drain anyway.

3. **H.264 encoding failed at REQBUFS.** The encoder always sets
   `HFI_PROPERTY_PARAM_VENC_H264_TRANSFORM_8X8`, but only the HFI v4 packet
   builder knew it. On v3 (msm8996) it returned -EINVAL. Patch 0008 adds it to
   the v3 builder. The downstream msm8996 driver sends the same property, so
   the firmware supports it. This affects every msm8996 device on 6.19, not
   only this tablet.

## How it was tested without rebuilding the kernel

The modules were rebuilt on the tablet itself, against the running kernel's
`/proc/config.gz` and with Alpine's clang 23 (the kernel uses kCFI, so the
compiler has to match). The host only sends a trimmed source tree: Kbuild
files, headers, arm64, `kernel/`, `lib/vdso` and the driver. The modules go
to `/lib/modules/$(uname -r)/updates/`, followed by `depmod`.

Run `sync` after installing modules. A reset shortly after an unsynced copy
corrupted both the `.ko` and the `.o` files.

To get the log from a boot that ended in a reset: boot with
`lk2nd.pass-ramoops=zap`. After the reset, in lk2nd, run
`fastboot oem ramoops console` and `fastboot get_staged console.txt`.

See also docs/thermal.md: without TSENS, long builds on the tablet overheated
it.
