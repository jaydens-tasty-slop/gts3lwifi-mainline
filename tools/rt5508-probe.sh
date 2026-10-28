#!/bin/sh
# Show what each RT5508 speaker amplifier receives, without listening.
#
# The amplifier's report selector (SPKRPTSEL, 0xa2) 3 makes the 24-bit
# report register (SPKRPT, 0xa3) return the sample it is playing (after its
# channel and slot selection and DAC volume). Run during playback: the
# largest of a few readings is ~0 for an amplifier that gets silence.
# An amplifier that is powered down reads 0x5d5d5d.
#
# usage: rt5508-probe.sh [bus] [addr...]   (default: bus 0, 0x34-0x37)
# needs i2c-tools; restores the selector to its default (2)
bus=${1:-0}
[ $# -gt 0 ] && shift
addrs=${*:-"34 35 36 37"}

for a in $addrs; do
	i2cset -f -y "$bus" "0x$a" 0xa2 3
	max=0
	for k in 1 2 3 4 5 6 7 8; do
		v=$(i2ctransfer -f -y "$bus" "w1@0x$a" 0xa3 r3 | tr -d ' ' | sed 's/0x//g')
		v=$((0x$v))
		[ "$v" -ge 8388608 ] && v=$((16777216 - v))
		[ "$v" -gt "$max" ] && max=$v
	done
	i2cset -f -y "$bus" "0x$a" 0xa2 2
	printf '0x%s: %d\n' "$a" "$max"
done
