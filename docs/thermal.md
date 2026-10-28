# Thermal: TSENS was missing

The postmarketOS msm8996 kernel config had `# CONFIG_QCOM_TSENS is not set`,
and nothing in pmaports history explains why. Without it:

- no thermal zones exist; `/sys/class/thermal` only has cpufreq cooling
  devices;
- nothing throttles the CPUs (up to 2.15 GHz on the big cluster) or the GPU
  under sustained load;
- a 4-job kernel module build on the tablet ended in a hardware reset (PS_HOLD
  warm reset, nothing in the kernel log).

With TSENS loaded (built as a module and loaded live first, now
`CONFIG_QCOM_TSENS=m` in the kernel config), msm8996.dtsi provides 14 zones:
CPU cores and L2, GPU top and bottom, Venus/L3, memory, modem, camera and
Q6. CPU trip points are 85 and 90 °C passive and 110 °C critical; GPU and
Venus are 90 °C. Idle temperatures on the tablet, CPUs capped at 1.19 GHz:
50-56 °C.

The ANA38401 panel and the SM5705 charger have their own thermal concerns
once they are driven; not looked at yet.
