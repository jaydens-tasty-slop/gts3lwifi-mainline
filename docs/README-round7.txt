Galaxy Tab S3 Wi-Fi - round 7: real display driver, first try
RAM boot only, nothing flashed. Needs the pmOS rootfs already on the tablet.

1) Boot the normal image (any that gives you ssh), then copy the new
   kernel modules onto the tablet and install them:
     scp modules-r2.tar.gz test@172.16.42.1:
     ssh test@172.16.42.1
     sudo tar -C / -xzf modules-r2.tar.gz && sudo depmod 6.19.5 && sync
   Then power off / reboot to lk2nd fastboot.

2) Boot the display test image:
     fastboot boot boot-display.img
   Watch the screen for ~1 minute. Any change at all (black, flicker,
   text, garbage) is worth noting. Video welcome.

3) After 2 minutes, over ssh:
     sudo dmesg | grep -iE "msm|mdss|dsi|panel|ams968|drm|iommu|tcon" > display.txt
     ls /dev/dri /sys/class/drm
   Send display.txt, the ls output, and what the screen did.
   If ssh does not come up: back in fastboot,
     fastboot oem ramoops console ; fastboot get_staged display-console.txt
   (the image has no ramoops option, so this only works if you add
   lk2nd.pass-ramoops=zap via --cmdline; ask if needed)
