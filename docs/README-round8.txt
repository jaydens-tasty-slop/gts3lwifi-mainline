Round 8: display + touchscreen. RAM boot only. Modules from round 7 must be installed.
  fastboot boot boot-display-touch.img
Then over ssh:
  ls /dev/input/ ; cat /proc/bus/input/devices | grep -A4 -i synaptics
  sudo dmesg | grep -iE "rmi|synaptics|i2c|touch" > touch.txt
Then touch the screen a few times and run for ~10 s:
  sudo cat /dev/input/event2   (try event0..event3; garbage on screen while touching = works)
Send touch.txt and whether touches produced output.
