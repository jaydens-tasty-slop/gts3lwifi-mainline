# GPU: Adreno 530 under mainline

## Status (2026-10-28): working headless with freedreno

On a Tab S3 with no panel attached, booted from RAM with
`dts/test/test-gpu.dts` (merged into the main DTS on 2026-10-28) and
`msm.separate_gpu_kms=1`:

- nothing loads `msm` by itself: the module only has aliases for the
  display subsystem, which is off. The device package loads it at boot
  (`modules-load.conf`) and puts `msm.separate_gpu_kms=1` on the cmdline;
- the GPU probes as its own DRM device (`/dev/dri/renderD128`), Mesa reports
  **FD530, OpenGL ES 3.1 / OpenGL 3.1 (Mesa 26.2.5)**;
- Samsung's signed zap shader is accepted by TrustZone, firmware loads
  (`a530_pm4.fw`, `a530_pfp.fw`, `a530v3_gpmu.fw2`), no hangs or faults;
- `tests/gltri.c` (surfaceless EGL, triangle into an FBO, read back) renders
  correctly: green inside the triangle, the red clear colour outside;
- devfreq offers 133 to 624 MHz.

## What it takes

- **Power:** VDD_GFX is PM8004 S2 (downstream qpnp regulator at 0x1700,
  400 to 1015 mV, GFX CPR ceiling 1015 mV at the top corner). Mainline has no
  GFX CPR for MSM8996, so the test DTS pins it at 1015 mV, always on, like the
  Sony MSM8996 boards. `mmcc` gets `vdd-gfx-supply` for the GPU GX GDSC.
- **Firmware** (not in this repo; copy from the tablet itself, read-only):
  - `/system/vendor/firmware/a530_pm4.fw`, `a530_pfp.fw`, `a530v3_gpmu.fw2`
    to `/lib/firmware/qcom/`
  - Samsung's signed zap shader from the `apnhlos` partition,
    `image/a530_zap.mdt` + `.b00` to `.b02`, to
    `/lib/firmware/qcom/apq8096/samsung/gts3lwifi/` (the DTS sets that path).
    The db820c zap in linux-firmware would not be accepted by Samsung's TZ.
- **DRM device:** `msm.separate_gpu_kms=1` while MDSS is not in the main DTS,
  so the GPU binds on its own instead of waiting for the display.
- `msm` is a module; load it with `modprobe msm` (nothing autoloads it
  without the display).

## Next

- Move the GPU nodes into the main DTS once the regulator choice is settled
  (check whether a lower pinned voltage with capped OPPs is stable).
- Run the db820c dEQP-GLES31 baseline from Mesa CI on the tablet.
- Vulkan: see docs/vulkan-a5xx.md (this was milestone M0 there).

Known issue found on the way: rebooting from Linux hung (PSCI
`SYSTEM_RESET` never returns from Samsung's TrustZone). Fixed in the board
DTS, see docs/reboot.md.
