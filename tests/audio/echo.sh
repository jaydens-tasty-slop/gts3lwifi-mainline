#!/bin/sh
# Echo test: record 5 s (left = main mic AMIC3, right = sub mic AMIC4), play it back on the
# headphones, repeat for about 3 minutes.
pkill speaker-test
a() { amixer -q -c0 cset "name=$1" "$2" || echo "cset failed: $1 = $2"; }
sh "$(dirname "$0")"/hp.sh
a 'RX HPH Mode' 'Class-H Low Power'; a 'COMP1 Switch' 1; a 'COMP2 Switch' 1
a 'HPHL Volume' 20; a 'HPHR Volume' 20
a 'RX1 Mix Digital Volume' ${PBVOL:-74}; a 'RX2 Mix Digital Volume' ${PBVOL:-74}
a 'MultiMedia3 Mixer SLIMBUS_0_TX' 1
a 'AIF1_CAP Mixer SLIM TX0' 1; a 'AIF1_CAP Mixer SLIM TX1' 1
a 'SLIM TX0 MUX' DEC0; a 'SLIM TX1 MUX' DEC1
a 'ADC MUX0' AMIC; a 'ADC MUX1' AMIC
a 'AMIC MUX0' ADC3; a 'AMIC MUX1' ADC4
a 'ADC3 Volume' ${MICVOL:-16}; a 'ADC4 Volume' ${MICVOL:-16}
end=$(($(date +%s) + ${SECS:-180}))
n=0
while [ $(date +%s) -lt $end ]; do
	n=$((n + 1))
	arecord -q -D hw:0,2 -f S16_LE -r 48000 -c 2 -d 5 /tmp/echo.wav || break
	cp /tmp/echo.wav /tmp/echo-$n.wav
	aplay -q -D hw:0,1 /tmp/echo.wav || break
done
