#!/bin/sh
# Stereo capture: TX0 <- DEC0 <- AMIC$1, TX1 <- DEC1 <- AMIC$2 (backend is fixed at 2 channels)
a() { amixer -q -c0 cset "name=$1" "$2" || echo "cset failed: $1 = $2"; }
a 'MultiMedia3 Mixer SLIMBUS_0_TX' 1
a 'AIF1_CAP Mixer SLIM TX0' 1
a 'AIF1_CAP Mixer SLIM TX1' 1
a 'SLIM TX0 MUX' DEC0
a 'SLIM TX1 MUX' DEC1
a 'ADC MUX0' AMIC
a 'ADC MUX1' AMIC
a 'AMIC MUX0' ADC${1:-3}
a 'AMIC MUX1' ADC${2:-4}
a "ADC${1:-3} Volume" 12
a "ADC${2:-4} Volume" 12
rm -f /tmp/st.wav
arecord -q -D hw:0,2 -f S16_LE -r 48000 -c 2 -d ${3:-3} /tmp/st.wav || exit 1
for c in 1 2; do printf "ch%s: " $c; sox /tmp/st.wav -n remix $c stat 2>&1 | awk -F: '/RMS     amplitude|Maximum amplitude/ {gsub(/ +/," ",$2); printf "%s=%s ", $1, $2} END {print ""}'; done
