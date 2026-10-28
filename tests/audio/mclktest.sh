#!/bin/sh
# Capture main/sub mic with and without a playback stream running (MCLK hypothesis).
lv() { for c in 1 2; do sox $1 -n remix $c stat 2>&1 | awk -F: '/RMS     amplitude/ {gsub(/ +/,"",$2); printf "%s ", $2}'; done; echo; }
arecord -q -D hw:0,2 -f S16_LE -r 48000 -c 2 -d 2 /tmp/c0.wav; printf "no playback:   "; lv /tmp/c0.wav
aplay -q -D hw:0,1 -f S16_LE -r 48000 -c 2 -d 6 /dev/zero & P=$!
sleep 1
arecord -q -D hw:0,2 -f S16_LE -r 48000 -c 2 -d 2 /tmp/c1.wav; printf "with playback: "; lv /tmp/c1.wav
wait $P
