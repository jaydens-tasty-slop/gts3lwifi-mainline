# Widevine DRM

## Status (2026-10-30)

| Level | Where | State |
|---|---|---|
| L1 (hardware) | stock Android, LineageOS (also with an unlocked bootloader and a tripped Knox, as group members report on LineageOS 24) | not on mainline (below) |
| L3 (software) | Waydroid (Android 13) | works: Bitmovin's Widevine demo plays in Android's browser |
| L3 (software) | Flatpak Firefox on pmOS | works: Bitmovin's Widevine demo plays |

**Nothing from Google is in this repository.** These steps fetch Google's
files from Google's own servers, under Google's licence, for use on your own
tablet. The same rule as for Samsung's firmware: never commit or mirror them.

## Why not L1 on mainline

L1 keeps decrypted video out of Linux' reach end to end. On this tablet that
is Samsung's stack: the Widevine trusted app (`widevine.mdt` on `apnhlos`),
Qualcomm's QSEECOM driver to load and call it, secure memory heaps, Venus in
secure mode, a secure display path, and Samsung's Android 9 OEMCrypto
libraries. Mainline has none of the kernel pieces for msm8996 (its QSEECOM
only finds already running apps on newer laptop SoCs and cannot load one),
Waydroid's Android does not use Venus at all, and the vendor libraries are
built for the downstream kernel's interfaces. The keys themselves survive
(L1 works on LineageOS with an unlocked bootloader), so this is a kernel
problem, not a lost certificate.

## L3 in Waydroid (Android 13, arm64)

waydroid_script's Widevine has arm64 files only for Android 11, and takes them
from a third-party mirror. Instead, take Google's own Widevine L3 HAL from the
official Android 13 arm64 emulator image, which ships it in its vendor
partition:

1. Download `arm64-v8a-33_r09.zip` (1.6 GB, SHA-1
   `97b81c9eb787667e53d5ae9de428284e4f230259`) from the Android SDK repository,
   `https://dl.google.com/android/repository/sys-img/google_apis_playstore/`
   (listed in `sys-img2-3.xml` there; Android SDK licence).
2. `unzip arm64-v8a-33_r09.zip arm64-v8a/vendor.img`. It is a GPT disk image;
   partition 1 (`vendor`, ext4) starts at sector 2048:
   `dd if=arm64-v8a/vendor.img of=vendor-part.img bs=512 skip=2048 count=188416`.
3. Copy out four files without mounting:
   `debugfs -R "dump /<path> <out>" vendor-part.img` for
   - `bin/hw/android.hardware.drm-service.widevine`
   - `lib64/libwvaidl.so`
   - `etc/init/android.hardware.drm-service.widevine.rc`
   - `etc/vintf/manifest/manifest_android.hardware.drm-service.widevine.xml`
4. Install them into Waydroid's overlay, which is layered over Android's
   vendor partition: `/var/lib/waydroid/overlay/vendor/<same path>`, the
   binary mode 755, the rest 644, owned by root.
5. Restart Waydroid. `waydroid shell -- service list | grep -i drm` shows
   `android.hardware.drm.IDrmFactory/widevine`.

The rest of what it needs is already in Waydroid's LineageOS 20 image:
`libprotobuf-cpp-lite-3.9.1.so` in vendor and `android.hardware.drm-V1-ndk.so`
in the VNDK APEX. The device certificate is fetched online on first use.

## L3 in Firefox (Flatpak)

Firefox ships no Widevine for Linux on ARM64, so its "Play DRM-controlled
content" switch has nothing to download there. Asahi Linux's
[widevine-installer](https://github.com/AsahiLinux/widevine-installer) takes
the CDM from Google's ChromeOS browser image and patches it for normal glibc
systems. The CDM needs glibc 2.36 or newer, so it cannot load in pmOS's own
(musl) browsers, but it does in a Flatpak browser, which brings its own glibc:

1. `flatpak install --user flathub org.mozilla.firefox`
2. As the installer does: download
   `chromeos-lacros-arm64-squash-zstd-120.0.6098.0` (147 MB) from
   `https://commondatastorage.googleapis.com/chromeos-localmirror/distfiles/`,
   `unsquashfs lacros.squashfs 'WidevineCdm/*'`, then
   `python3 widevine_fixup.py squashfs-root/WidevineCdm/_platform_specific/cros_arm64/libwidevinecdm.so libwidevinecdm.so`
   (Widevine 4.10.2662.3; it supports 4K pages, so no memory permissions are
   weakened).
3. Put `libwidevinecdm.so` and `squashfs-root/WidevineCdm/manifest.json` into
   `~/.var/app/org.mozilla.firefox/widevine/gmp-widevinecdm/system-installed/`
   (a path the sandbox can see).
4. `flatpak override --user --env=MOZ_GMP_PATH=$HOME/.var/app/org.mozilla.firefox/widevine/gmp-widevinecdm/system-installed org.mozilla.firefox`
5. In the profile Firefox actually uses (`installs.ini`, usually
   `*.default-release` under `~/.var/app/org.mozilla.firefox/config/mozilla/firefox/`),
   add a `user.js` with the installer's preferences:

       user_pref("media.gmp-widevinecdm.version", "system-installed");
       user_pref("media.gmp-widevinecdm.visible", true);
       user_pref("media.gmp-widevinecdm.enabled", true);
       user_pref("media.gmp-widevinecdm.autoupdate", false);
       user_pref("media.eme.enabled", true);
       user_pref("media.eme.encrypted-media-encryption-scheme.enabled", true);

6. Restart Firefox. Widevine then appears under Add-ons > Plugins.
