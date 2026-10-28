#!/bin/sh
# Through PipeWire: a 3 s 440 Hz tone, then an echo loop (record 5 s, play it back).
sox -n -r 48000 -c 2 -b 16 /tmp/tone.wav synth 3 sine 440 vol 0.3
pw-play /tmp/tone.wav
end=$(($(date +%s) + ${SECS:-240}))
n=0
while [ $(date +%s) -lt $end ]; do
	n=$((n + 1))
	rm -f /tmp/pwecho.wav
	timeout -s INT 5 pw-record /tmp/pwecho.wav
	cp /tmp/pwecho.wav /tmp/pwecho-$n.wav
	pw-play /tmp/pwecho.wav
done
