#!/bin/sh
# PipeWire stress: short plays and records (each a full open/prepare/stop/close
# or suspend/resume cycle), a WirePlumber restart, then more cycles.
sox -n -r 48000 -c 2 -b 16 /tmp/blip.wav synth 0.4 sine 660 vol 0.2
ok=0; bad=0
cycle() {
	for i in $(seq 1 ${1:-10}); do
		pw-play /tmp/blip.wav || bad=$((bad+1))
		rm -f /tmp/s.wav; timeout -s INT 1 pw-record /tmp/s.wav
		d=$(soxi -D /tmp/s.wav 2>/dev/null); case "$d" in 0*|"") bad=$((bad+1));; *) ok=$((ok+1));; esac
		sleep ${2:-0.3}
	done
}
cycle 10 0.3
cycle 3 7          # let the nodes suspend (5 s timeout) between uses
systemctl --user restart wireplumber; sleep 6
cycle 5 0.3
echo "records ok=$ok bad=$bad"
