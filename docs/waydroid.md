# Waydroid (Android 13 in a container)

## Status (2026-10-30): runs, GPU-accelerated, network works

Tested on the headless SM-T820 (r5 kernel, pmOS edge, systemd) with Waydroid
1.6.3 and the LineageOS 20 VANILLA image (system 2026-09-27, vendor MAINLINE
2026-10-22), shown in a headless sway session over VNC.

| | |
|---|---|
| Kernel options | everything in pmbootstrap's `waydroid` kconfig list is already set (binder, hwbinder and vndbinder devices, PSI, squashfs, bridge, veth, nftables NAT, ...) |
| GPU | Android renders with Mesa freedreno: `GLES: freedreno, FD530, OpenGL ES 3.1 Mesa 26.0.8` (minigbm gbm_mesa) |
| Network | works once the firewall lets Waydroid through (below) |
| Battery | Android shows the real charge (from the SM5705 fuel gauge through the host) |
| Not available inside Android | sensors, camera, direct Wi-Fi/Bluetooth control (Android sees Ethernet; Bluetooth audio works through the host), hardware video decode |

## Package

`device-samsung-gts3lwifi-waydroid` (optional) installs `waydroid` and `cage`,
the firewall rule below as `/etc/nftables.d/52_waydroid.nft` (loaded right
away if the firewall is active), and an "Android (Waydroid)" login session
that runs Android full screen in cage (`gts3-waydroid-session`); powering
Android off ends the session. Then run `waydroid init` once (or use the local
images, below). If the rule was added by hand before, remove that file first.

## Fixes needed

**Firewall.** pmOS's nftables policy drops input and forwarding on unknown
interfaces, so Android on `waydroid0` gets no DHCP lease, no DNS and no
route out. pmaports has no rule for it. Add `/etc/nftables.d/52_waydroid.nft`:

    #!/usr/sbin/nft -f
    table inet filter {
    	chain input {
    		iifname "waydroid0" udp dport { 53, 67 } accept comment "Waydroid DHCP and DNS"
    		iifname "waydroid0" tcp dport 53 accept comment "Waydroid DNS over TCP"
    	}
    	chain forward {
    		iifname "waydroid0" accept comment "Waydroid outgoing traffic"
    		oifname "waydroid0" ct state { established, related } accept comment "Waydroid replies"
    	}
    }

and load it with `nft -f` (or restart nftables). Waydroid adds the NAT itself.

**Headless sway on the GPU.** With `WLR_BACKENDS=headless`, wlroots picks no
render node and composites with llvmpipe on the CPU (sway at 95% CPU, every
Android frame copied). Set `WLR_RENDERER=gles2` and
`WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128`; sway then idles at a few %.
A session on the real display does not need this.

## First boot

Known issue: right after a boot, a "System UI isn't responding" dialog can
appear while the tablet is still busy; the stack dump Android then takes is
what can kill `vold` (below).

Android's first boot compiles every app and takes about 5.5 minutes on this
SoC. If the tablet is busy with other work at the same time (another desktop
session, repeated `waydroid shell` calls, logcat capture), System UI and the
phone service hit ANRs; Android then dumps native stacks of its daemons, the
dump of `vold` times out, `vold` dies of SIGPIPE, and because it is marked
`reboot_on_failure` Android reboots (`reboot,vold-failed`) and Waydroid stops
the container. Each failed first boot also leaves the user data half
destroyed, so the next boot is a first boot again. Fix: keep the tablet idle
for the first boot; if it already looped, stop the session, remove
`~/.local/share/waydroid/data` and start again. Later boots are fast.

## Downloading the images

`waydroid init` downloads about 1 GB from SourceForge in one stream and does
not resume; on a slow mirror the stream broke at 210 MB and init failed
with a hash mismatch. `tools/sf-parallel-download.py` fetches 4 MiB chunks
from several SourceForge mirrors in parallel (about 6 MB/s instead of 40 KB/s
here) and checks the SHA-256 from the OTA JSON
(`https://ota.waydro.id/system/lineage/waydroid_arm64/VANILLA.json`,
`.../vendor/waydroid_arm64/MAINLINE.json`). Unzip, copy `system.img` and
`vendor.img` to `/etc/waydroid-extra/images/` on the tablet and run
`waydroid init -f`; it uses the local images.
