#!/bin/sh
# Venus decode check: each clip through the GStreamer V4L2 stateful decoder and
# through ffmpeg in software, both as packed I420; reports frame counts and
# whether the outputs are byte-identical. Clips: see docs/venus.md.
run() { # name file demux parse dec w h
	n=$1; f=$2; dmx=$3; prs=$4; dec=$5; w=$6; h=$7
	rm -f hw-$n.yuv sw-$n.yuv
	t0=$(date +%s.%N)
	timeout 90 gst-launch-1.0 -q filesrc location=$f ! $dmx ! $prs $dec ! videoconvert ! \
		video/x-raw,format=I420 ! filesink location=hw-$n.yuv >gst-$n.log 2>&1
	rc=$?; t1=$(date +%s.%N)
	ffmpeg -hide_banner -loglevel error -i $f -f rawvideo -pix_fmt yuv420p sw-$n.yuv
	fs=$((w * h * 3 / 2))
	hwf=$(( $(stat -c %s hw-$n.yuv 2>/dev/null || echo 0) / fs )); swf=$(( $(stat -c %s sw-$n.yuv) / fs ))
	if cmp -s hw-$n.yuv sw-$n.yuv; then r=IDENTICAL; else r="DIFFER ($(cmp hw-$n.yuv sw-$n.yuv 2>&1 | head -1))"; fi
	echo "$n: gst rc=$rc hw_frames=$hwf sw_frames=$swf $r time=$(awk "BEGIN{printf \"%.1f\", $t1-$t0}")s"
	[ $rc -ne 0 ] && tail -3 gst-$n.log
}
run h264 h264-720p.mp4 qtdemux "h264parse !" v4l2h264dec 1280 720
run hevc hevc-1080p.mp4 qtdemux "h265parse !" v4l2h265dec 1920 1080
run vp8 vp8-720p.webm matroskademux "" v4l2vp8dec 1280 720
run vp9 vp9-720p.webm matroskademux "vp9parse !" v4l2vp9dec 1280 720
