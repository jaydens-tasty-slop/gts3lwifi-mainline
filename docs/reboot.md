# Reboot: PSCI hangs in Samsung's TrustZone, PS_HOLD works

## Status (2026-10-28): fixed in the board DTS

Rebooting from Linux used to hang. That included `systemctl reboot`, busybox
`reboot` and even sysrq-b, which skips all driver shutdown hooks. The
tablet needed a forced restart (Power + Volume Down, about 10 s).

With the `syscon-reboot` node in `dts/apq8096-samsung-gts3lwifi.dts`:

- `systemctl reboot` resets 4 s after the command and is back in lk2nd
  fastboot 6 s later. The PMIC records a hard reset caused by PS_HOLD
  (PON_REASON1 0x41, POFF_REASON1 0x02), the same kind the vendor kernel
  does;
- the emergency restart (sysrq-b) is back in lk2nd in about 8 s (warm reset).

## What was wrong

Mainline restarts arm64 through PSCI `SYSTEM_RESET` (restart handler
priority 129). The msm8996 DTS has no PS_HOLD restart node, so PSCI is the
only handler. On this tablet the call never returns:

- the host's USB log shows the gadget link staying up but silent for about
  60 s after the restart. So Linux froze inside the call and nothing reset;
- after those 60 s the USB link drops and comes back as a device that never
  enumerates (most likely a secure watchdog sending the SoC into a dump
  state). Only a forced restart recovers it.

Samsung's own kernel never uses PSCI for this. `do_msm_restart()` writes a
restart reason to the PMIC, picks hard or warm reset, and then drops PS_HOLD
(SCM call or a direct write to MPM2 PS_HOLD at 0x004ab000).

Tested from userspace with `tools/mmio-wr` (sync and remount read-only
first):

| Reset | Result |
|---|---|
| clear PS_HOLD (0x004ab000 bit 0) from Linux | the register is writable from the non-secure side; PMIC warm reset; lk2nd fastboot after 6 s |
| a TrustZone-protected register read (TCSR 0x007b3000) | TrustZone resets the tablet itself; PON WARM_RESET_REASON1 = 0x02 (PS_HOLD); lk2nd after 5 s |
| PSCI SYSTEM_RESET (sysrq-b, old DT) | hang, as above |

## The fix

```dts
/ {
	reboot {
		compatible = "syscon-reboot";
		regmap = <&pshold>;
		offset = <0x0>;
		mask = <0x1>;
		value = <0x0>;
	};
};

&soc {
	pshold: syscon@4ab000 {
		compatible = "syscon";
		reg = <0x004ab000 0x4>;
	};
};

&pm8994_pon {
	mode-normal = <0x14>;
};
```

- `syscon-reboot` runs at its default priority 192, before the PSCI handler,
  and clears PS_HOLD. No kernel change is needed; `POWER_RESET_SYSCON` is
  already built in.
- `pm8941-pwrkey` (already bound) sets the PMIC PS_HOLD reset type on a
  normal reboot (hard reset). Emergency restarts keep the bootloader's
  setting (warm reset), which also works.
- `mode-normal = <0x14>` makes `qcom-pon` store Samsung's
  `PON_RESTART_REASON_NORMALBOOT` in the PON spare register, as the vendor
  kernel does on a plain reboot. `mode-bootloader` (0x02) and
  `mode-recovery` (0x01) in pm8994.dtsi already match Samsung's codes. Note
  that on Samsung "bootloader" means Download mode, not lk2nd's fastboot.

Power-off still goes through PSCI `SYSTEM_OFF` (`syscon-poweroff` would run
after it) and has not been tested.

## Debugging without a display

`tools/` has what was used here:

- `mmio-rd` and `mmio-wr` access registers through `/dev/mem`
  (`STRICT_DEVMEM` allows MMIO);
- `spmi-reg` reads and writes PMIC registers through the SPMI arbiter, for
  peripherals Linux owns;
- PON registers worth knowing (PM8994, SID 0): 0x808 PON_REASON1,
  0x80a WARM_RESET_REASON1, 0x80c POFF_REASON1, 0x85a/0x85b PS_HOLD reset
  type and enable, 0x88f restart reason (bits 7:2).

Reading TCSR 0x007b3000 (download-mode cookie) from Linux is an XPU
violation and resets the tablet at once. Don't do it by accident.
