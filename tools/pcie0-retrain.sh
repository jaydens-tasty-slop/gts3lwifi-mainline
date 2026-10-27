#!/bin/sh
# Re-run PCIe0 link training by hand on MSM8996 and sample the LTSSM state.
# LTSSM off (PARF_LTSSM 0x006001b0 bit 8), PERST# (TLMM 35, io reg 0x01033004
# bit 1) low then high, wait $1 seconds (default 0.1), LTSSM on.
# LTSSM codes (DWC PORT_DEBUG0 0x0c000728 bits 5:0): 0x00 Detect.Quiet,
# 0x03 Polling.Compliance, 0x11 L0, 0x12 L0s.
# Needs mmio-rd / mmio-wr from this directory, as root.
D=$(dirname "$0"); R=${R:-$D/mmio-rd}; W=${W:-$D/mmio-wr}
val() { echo $(( 0x$($R $1 | cut -d' ' -f2) )); }
ltssm() { echo "LTSSM 0x$(printf %02x $(( $(val 0c000728) & 63 )))"; }
$W 006001b0 $(printf %x $(( $(val 006001b0) & ~0x100 )))
$W 01033004 0
sleep 0.2
$W 01033004 2
sleep ${1:-0.1}
$W 006001b0 $(printf %x $(( $(val 006001b0) | 0x100 )))
for i in 1 2 3 4 5 6 7 8 9 10; do ltssm; sleep 0.05; done | sort | uniq -c
$R 0c000080   # Link Control/Status: bits 16-19 speed, 20-25 width, bit 29 DLL active
