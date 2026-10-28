#!/bin/sh
# Speaker listening test: one MultiMedia1 stream on both TDM buses, a 1 kHz
# tone at -20 dBFS on the left channel (top pair), the right channel
# (bottom pair), then both, at three amplifier volumes. Keep the ssh session
# open while it runs (a job left behind is killed at logout).
#
# usage: spk.sh [seconds]   (default 180; needs sox and alsa-utils)
set -e
dir=${TMPDIR:-/tmp}/spk
mkdir -p "$dir"
cd "$dir"
[ -f left.wav ] || {
	sox -n -r 48000 -b 16 -c 1 tone.wav synth 1.5 sine 1000 vol -20dB
	sox tone.wav -c 2 left.wav remix 1 0
	sox tone.wav -c 2 right.wav remix 0 1
	sox tone.wav -c 2 both.wav remix 1 1
	sox -n -r 48000 -b 16 -c 2 gap.wav trim 0 0.7
}

amps="Top 1|Top 2|Bottom 1|Bottom 2"
setvol() {
	echo "$amps" | tr '|' '\n' | while read -r p; do
		amixer -q -c 0 cset name="$p DAC Volume" "$1"
	done
}

amixer -q -c 0 cset name="PRIMARY_TDM_RX_0 Audio Mixer MultiMedia1" 1
amixer -q -c 0 cset name="QUAT_TDM_RX_0 Audio Mixer MultiMedia1" 1

end=$(( $(date +%s) + ${1:-180} ))
while [ "$(date +%s)" -lt "$end" ]; do
	for v in 175 195 215; do	# -40, -30, -20 dB
		setvol "$v"
		echo "$(date +%T) DAC volume $v: left, right, both"
		aplay -q -D hw:0,0 left.wav gap.wav right.wav gap.wav both.wav gap.wav gap.wav
	done
done
setvol 175
