# CPU idle (PSCI)

## Status (2026-10-28): CPU, cluster and system idle work

| Stage | What | Idle current at 4.3 V |
|---|---|---|
| none | WFI only (the old config) | about 200 mA |
| 1 | CPU state 0x4 (msm8996.dtsi) | 172-176 mA |
| 3 | + L2 GDHS per cluster, + system retention (OSI hierarchy, kernel patch 0018) | 133-136 mA |

Stage 3 takes idle power down by about a third, 0.28 W. Stage 2 (CPU power
collapse) is refused by the firmware.

## Stage 1: CPU state

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

## Stage 3: cluster and system idle (OSI)

Stock composes its StateIDs itself, the OSI way: power-down bit 30,
affinity level at bit 24, system mode at bit 8, cluster mode at bit 4
(1 L2 WFI, 3 L2 GDHS, 4 L2 power collapse), CPU mode at bit 0. Mainline does
the same with the hierarchical layout: each CPU gets a PSCI power domain,
parented to a cluster domain, parented to a system domain, and the last
CPU going down requests the parameter of the highest domain that powers
off.

**The firmware is OSI from boot but refuses the mode switch.** It
advertises OSI and answers NOT_SUPPORTED to SET_SUSPEND_MODE for both
modes (`failed to set PC mode: -1` at boot, `failed to set OSI mode: -1`
from the domain driver). Mainline then removes the CPU power domains, and
cpuidle-psci cannot attach the CPUs: **no idle states at all**
(`dts/test/test-cluster1.dts` on an unpatched kernel).

Probing first, with the composite StateIDs as plain per-CPU states and
the kretprobe on the return codes (`dts/test/test-cluster2.dts`,
`test-cluster3.dts`; not for normal use):

| StateID | Meaning | Answer |
|---|---|---|
| 0x01000034 | affinity 1, L2 GDHS, CPU mode 4 | granted when the other CPU of the cluster is down, DENIED otherwise |
| 0x02002334 | affinity 2, system retention (0x23), L2 GDHS, CPU mode 4 | granted now and then (all four CPUs down), DENIED otherwise |
| 0x41000044 | L2 power collapse, power-down bit | always DENIED: it needs the CPUs in a power-down state, which is refused |

DENIED (not NOT_SUPPORTED) for the cluster IDs shows the firmware does
the OSI last-man check itself.

Kernel patch 0018 (`cpuidle: psci-domain: accept OSI-only firmware on
MSM8996`) takes NOT_SUPPORTED from the OSI switch to mean "already OSI" on
`qcom,apq8096`/`qcom,msm8996` machines. Other msm8996 devices are not
affected unless their DT uses the hierarchical layout. With it and
`dts/test/test-cluster1.dts` (CPU 0x4 -> cluster GDHS 0x01000034 -> system
retention 0x02002334):

```
CPUidle PSCI: firmware is in OSI mode already
CPUidle PSCI: Initialized CPU PM domain topology using OSI mode
```

In the first 80 s after boot the clusters spent 54-58 s in GDHS (3000-5300
entries each) and the system 50 s in retention (2053 entries); the
firmware refused a few hundred requests that raced with a CPU waking up.

`tests/idlestress.sh` (15 minutes of load bursts, 10 ms wakeups and a
module build) ran clean on this setup too. Over 20 minutes of uptime the
clusters spent 568 and 721 s in GDHS (137,000 and 191,000 entries) and
the system 408 s in retention (57,000 entries). One temperature sample
during a four-core burst read 72 °C; all others stayed at or below 58 °C,
as in the stage 1 run.

Power, same method as stage 1 (cpuidle state 1 disabled at runtime turns
the whole hierarchy off, as the domain states are entered through it):

| | floor | average of 30 s |
|---|---|---|
| hierarchy on | 133-136 mA | 141-147 mA |
| WFI only | 198-200 mA | 205-207 mA |

Latencies are conservative (GDHS 200/300/1000 us, system retention
400/700/3000 us; stock lists 90 and 350 us), so there may be a little
more to gain. L2 power collapse and the system's deeper modes (stock
"system-fpc", which also notifies the RPM) need the CPU power-down state
that the firmware refuses.
