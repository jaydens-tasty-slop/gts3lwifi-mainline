#!/bin/sh
# Round 9: msm.ko with the MDP5 split-display patch, for the round 8 boot image.
# Run after building the module in the envkernel shell:
#   cd ~/samsung/linux && make -j$(nproc) modules
set -eu
P=$(cd "$(dirname "$0")" && pwd)
KO=$HOME/samsung/linux/.output/drivers/gpu/drm/msm/msm.ko
SRC=$HOME/samsung/linux/drivers/gpu/drm/msm/disp/mdp5/mdp5_ctl.c
OUT=$P/tester-round17
[ -f "$KO" ] || { echo "error: $KO not found" >&2; exit 1; }
[ "$KO" -nt "$SRC" ] || { echo "error: msm.ko is older than the mdp5 sources; rebuild with 'make -j\$(nproc) modules' in envkernel" >&2; exit 1; }
strings -a "$KO" | grep -q "split_start_both" && strings -a "$KO" | grep -q "split display v5" && strings -a "$KO" | grep -q "rgb_swap" && strings -a "$KO" | grep -q "enabling DSI test pattern" || { echo "error: msm.ko does not contain the split display code" >&2; exit 1; }
strings -a "$KO" | grep -m1 "^vermagic=" | grep -q "^vermagic=6.19.5 " || { echo "error: unexpected vermagic" >&2; exit 1; }
STRIP=$(command -v llvm-strip || ls /usr/lib/llvm-*/bin/llvm-strip 2>/dev/null | head -1 || true)
PKO=$HOME/samsung/linux/.output/drivers/gpu/drm/panel/panel-samsung-ams968hh01.ko
[ "$PKO" -nt "$HOME/samsung/linux/drivers/gpu/drm/panel/panel-samsung-ams968hh01.c" ] || { echo "error: panel module is older than its source; rebuild with 'make -j\$(nproc) modules'" >&2; exit 1; }
strings -a "$PKO" | grep -q "late_mode" || { echo "error: panel module lacks the mdnie code" >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT/lib/modules/6.19.5/kernel/drivers/gpu/drm/msm" "$OUT/lib/modules/6.19.5/kernel/drivers/gpu/drm/panel"
"$STRIP" --strip-debug -o "$OUT/panel.ko" "$PKO" 2>/dev/null || cp "$PKO" "$OUT/panel.ko"
zstd -q -19 --rm "$OUT/panel.ko" -o "$OUT/lib/modules/6.19.5/kernel/drivers/gpu/drm/panel/panel-samsung-ams968hh01.ko.zst"
if [ -n "$STRIP" ]; then "$STRIP" --strip-debug -o "$OUT/msm.ko" "$KO"; else cp "$KO" "$OUT/msm.ko"; fi
zstd -q -19 --rm "$OUT/msm.ko" -o "$OUT/lib/modules/6.19.5/kernel/drivers/gpu/drm/msm/msm.ko.zst"
tar -C "$OUT" -czf "$OUT/msm-split-display-v12.tar.gz" lib
rm -rf "$OUT/lib"
cat > "$OUT/README-round17.txt" <<'TXT'
Round 17: read the panel's mode registers back. Modules v12 (msm + panel).
RAM boot only, same boot image as round 8. Two boots, one text file each.
No photos needed unless the screen looks different from before.

1) Install:
     scp msm-split-display-v12.tar.gz test@172.16.42.1:
     ssh test@172.16.42.1
     sudo tar -C / -xzf msm-split-display-v12.tar.gz && sudo depmod 6.19.5 && sync && sync
     sudo reboot

2) Boot A (normal):
     fastboot boot boot-display-touch.img
   after a minute, over ssh:
     sudo dmesg | grep -iE "panel|ams968|readback|mode registers|long write|split display" > display17a.txt; sync
     sudo reboot

3) Boot B (same registers written again after display-on):
     fastboot --cmdline "console=tty0 panel_samsung_ams968hh01.late_mode=1 pmos_boot_uuid=6a690fa9-50e7-4341-a1c4-a2da8bd45995 pmos_root_uuid=6c62b2bd-62ef-48c9-8231-73857f873a6a pmos_rootfsopts=defaults" boot boot-display-touch.img
   after a minute, over ssh:
     sudo dmesg | grep -iE "panel|ams968|readback|mode registers|long write|split display" > display17b.txt; sync
   If the screen looks any different in boot B (colours, text), take a photo.

Send display17a.txt and display17b.txt.
TXT
(cd "$OUT" && sha256sum msm-split-display-v12.tar.gz > SHA256SUMS && tar -xzOf msm-split-display-v12.tar.gz lib/modules/6.19.5/kernel/drivers/gpu/drm/msm/msm.ko.zst | sha256sum > MODULE-SHA256)
ZIP=$P/gts3lwifi-test-round17.zip; rm -f "$ZIP"
(cd "$OUT" && zip -q -9 "$ZIP" ./*)
ls -la "$OUT" "$ZIP"; echo "post this file: $ZIP"
