# CPU idle (PSCI)

## Status (2026-10-28): stage 1 works

The postmarketOS msm8996 kernel config had `# CONFIG_ARM_PSCI_CPUIDLE is not
set`, so the cores never left WFI: `/sys/devices/system/cpu/cpu0/cpuidle`
did not exist. Nothing in pmaports history says why; the option has been off
since the config was created.

With `CONFIG_ARM_PSCI_CPUIDLE=y` (which also selects
`CPU_IDLE_MULTIPLE_DRIVERS`, `DT_IDLE_STATES`, `DT_IDLE_GENPD` and
`ARM_PSCI_CPUIDLE_DOMAIN`) the `psci_idle` driver uses msm8996.dtsi's one
state:

| State | PSCI param | Entry / exit / residency | |
|---|---|---|---|
| `cpu-sleep-0` ("standalone-power-collapse") | 0x00000004 | 130 / 80 / 300 us | CPU mode 4 without the power-down bit; Linux keeps its context |

Samsung's TrustZone accepts it. The boot log shows PSCI 1.0 with OSI mode
supported, and the firmware refuses to switch to platform-coordinated mode
(`psci: [Firmware Bug]: failed to set PC mode: -1`, also with the old
kernel).

Stability: 15 minutes of load bursts on 1-4 cores, a process waking every
10 ms and a module build (`tests/idlestress.sh`, logged every 20 s) ran
without warnings, lockups or resets. Each core entered `cpu-sleep-0`
140,000-275,000 times and spent 600-800 s of the 18 minutes in it; the
hottest zone reached 59 °C.

## Power

Measured with the charger in suspend mode (SM5705 CNTL 0x0c = 0x00), so
the battery carries the whole tablet and the fuel gauge current is the
system load. Idle, no display, Wi-Fi not connected, 4.30 V:

| | Battery current |
|---|---|
| without cpuidle (old kernel) | about 200 mA (0.87 W) |
| `cpu-sleep-0` disabled at runtime | 200-202 mA |
| `cpu-sleep-0` enabled | 172-176 mA |

Settled idle, 30 one-second samples per run, alternating on/off five times.
The floor values are given; background activity adds spikes of 270-300 mA
in both cases. `cpu-sleep-0` saves about 27 mA, 0.12 W or 13 %.

The bigger savings (L2 and cluster power collapse, then the RPM's system
low-power modes) need cluster states, which mainline does not describe for
msm8996, even on the db820c.

## Building

`CONFIG_ARM_PSCI_CPUIDLE` is a bool, so it needs a kernel build, not a
module. In the envkernel terminal, with the kernel tree's
`kernel/configs/gts3-cpuidle.config` containing `CONFIG_ARM_PSCI_CPUIDLE=y`:

```
make gts3-cpuidle.config
make -j4 Image.gz
```

`scripts/make-test-image.sh` takes `KERNEL=<Image.gz>` to put that kernel
into a RAM-boot image, with the initramfs and cmdline of the installed
boot.img. The config of the September envkernel build matched the running
kernel, the version string stayed `6.19.5` and `MODVERSIONS` is off, so the
installed modules load as before.

## Deeper states

Stock (`msm8996-pm.dtsi`, `lpm-levels`) has three CPU levels: WFI, "fpc-def"
(a Qualcomm hypervisor call, SMC 0xc4000021) and "fpc", PSCI CPU_SUSPEND
with the power-down bit, 0x40000004. Cluster levels (L2 retention, L2 power
collapse) add the cluster mode at bit 4 and the affinity level, which the
OS decides as the last CPU of a cluster goes down: OSI mode.

- Stage 2, `dts/test/test-idle2.dts`: a second CPU state 0x40000004 with
  `local-timer-stop`, like msm8998's power-collapse states. **Samsung's
  TrustZone refuses it.** The state never got used (thousands of
  `rejected` per core in a minute), and a short kretprobe on
  `psci_cpu_suspend_enter` showed why:

  ```
  313 state=0x40000004  ret=-95   (PSCI NOT_SUPPORTED)
   35 state=0x4         ret=0
  ```

  Stock sends the same call (64-bit CPU_SUSPEND, 0x40000004, entry point
  `cpu_resume`, context saved), so either stock's deepest CPU level fails
  the same way and its fallback hides it, or the firmware wants something
  stock sets up elsewhere. Stock's other power collapse level goes through
  a Qualcomm hypervisor call (SMC 0xc4000021) instead of PSCI. Do not use
  this DT: a refused state makes the governor fall back to shallower
  states after every attempt.
- Stage 3, not tried: cluster states with the hierarchical PSCI layout (CPU
  and cluster power domains, OSI mode), as on sdm845 and later. Stock's
  cluster levels are L2 GDHS (cluster mode 3) and L2 power collapse (mode 4
  with the power-down bit), at bit 4 of the StateID with the affinity level
  at bit 24. Given the answer to 0x40000004, the power-down variant is
  likely refused too; L2 GDHS (no power-down bit) is the one worth trying.
