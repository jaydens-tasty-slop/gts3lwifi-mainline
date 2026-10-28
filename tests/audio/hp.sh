#!/bin/sh
# Headphone playback (db820c UCM): MultiMedia2 -> SLIMBUS_6_RX -> AIF4_PB -> RX5/RX6 -> INT1/INT2 -> HPHL/HPHR
a() { amixer -q -c0 cset "name=$1" "$2" || echo "cset failed: $1 = $2"; }
for i in 0 1 2 3 4 7; do a "SLIM RX$i MUX" ZERO; done
a 'SLIM RX5 MUX' AIF4_PB
a 'SLIM RX6 MUX' AIF4_PB
a 'RX INT1_2 MUX' RX5
a 'RX INT2_2 MUX' RX6
a 'RX5 Digital Volume' ${VOL:-68}
a 'RX6 Digital Volume' ${VOL:-68}
a 'SLIMBUS_6_RX Audio Mixer MultiMedia2' 1
a 'RX INT1 DEM MUX' CLSH_DSM_OUT
a 'RX INT2 DEM MUX' CLSH_DSM_OUT
