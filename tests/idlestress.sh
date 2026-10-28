#!/bin/sh
# 15 min: load bursts on 1-4 cores, idle gaps, 10 ms wakeups, one module build
out=$HOME/idlestress.log
: > $out
( while :; do sleep 0.01; done ) &
tick=$!
end=$(( $(date +%s) + 900 ))
n=1
while [ $(date +%s) -lt $end ]; do
	pids=""; for c in $(seq 1 $n); do timeout 10 sh -c "while :; do :; done" & pids="$pids $!"; done
	wait $pids
	sleep 10
	u0=$(cat /sys/devices/system/cpu/cpu0/cpuidle/state1/usage)
	u2=$(cat /sys/devices/system/cpu/cpu2/cpuidle/state1/usage)
	t=$(sort -n /sys/class/thermal/thermal_zone*/temp | tail -1)
	echo "$(date +%T) n=$n cpu0-sleep=$u0 cpu2-sleep=$u2 maxtemp=$t" >> $out
	n=$(( n % 4 + 1 ))
	if [ $n -eq 1 ] && [ ! -e $HOME/idlestress.built ]; then
		(cd $HOME/sm5705fg && make LLVM=1 -C $HOME/linux O=$HOME/kb M=$HOME/sm5705fg KBUILD_MODPOST_WARN=1 -B -j2 modules > $HOME/idlestress-build.log 2>&1; echo "build exit $?" >> $out)
		touch $HOME/idlestress.built
	fi
done
kill $tick
echo "$(date +%T) done" >> $out
