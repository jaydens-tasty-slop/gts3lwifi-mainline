Galaxy Tab S3 LTE (SM-T825) - postmarketOS test, round 1

Goal: install lk2nd and check that mainline Linux boots and USB works.
Only lk2nd gets written to the tablet (boot partition); everything else runs
from RAM. Undo = flash the stock firmware (or just its boot.img) with Odin.

Needs: SM-T825 with OEM unlock enabled, USB cable, a Linux PC (Windows works
with Odin + platform-tools but USB networking may need a driver).
Files: boot.img, lk2nd-gts3llte.img, lk2nd-gts3llte-odin.tar, SHA256SUMS

1) Install lk2nd (pick one)
   - TWRP: Install > Install Image > lk2nd-gts3llte.img > partition "Boot".
   - Odin: Download mode (Power + Volume Down + Home), lk2nd-gts3llte-odin.tar
     in the AP slot.
   - Linux: heimdall flash --BOOT lk2nd-gts3llte.img
   Only the boot partition changes; TWRP in recovery stays. To undo, flash
   your stock or LineageOS boot.img back the same way. Do NOT flash the
   boot.img from this package, it is only booted from RAM in step 3.
   The tablet reboots. After the Samsung logo it should stay in the lk2nd
   menu / fastboot. If it only shows the Samsung logo or reboots in a loop,
   stop and tell us (and which firmware you had: build number).

2) Send the lk2nd log
     fastboot devices
     fastboot oem log
     fastboot get_staged lk2nd.log
     fastboot getvar all 2> getvar.txt

3) Boot Linux from RAM (writes nothing)
     fastboot boot boot.img
   Wait 2 minutes. On the PC:
     lsusb
     ip -br addr
   Expected: a USB network interface with IP 172.16.42.2. Then:
     telnet 172.16.42.1
     dmesg > /tmp/d.txt; cat /tmp/d.txt
   "Could not find root partition" inside the tablet is expected (no
   postmarketOS installed yet). The screen may stay black or show text in
   odd colours; both are fine for this round.
   If nothing appears after 2 minutes, hold Power + Volume Down ~10 s to
   reboot. Nothing was written. Report "no USB".

Send: lk2nd.log, getvar.txt, the tablet dmesg (and lsusb output if USB
did not come up).
