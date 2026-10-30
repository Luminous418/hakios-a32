# The A32's two CPU clusters

The Galaxy A32 4G's MT6769T has two Cortex-A75 cores and six Cortex-A55 cores, clocked by
MediaTek's ARMPLL2 and ARMPLL1 and fed by the MT6358's Vproc11 and Vproc12 bucks. The boot
loader (LK) leaves both clusters at about 1175 MHz and 0.8 V; Android's driver then takes them
to 2000 MHz (A75) and 1700 MHz (A55).

`kernel/bsd/dev/hakios.mtkcpufreq.c` is the Infinix Hot 20's `mtk_cpufreq` ported to this
phone (same clusters, same PMIC). Boot-arg `mtk_cpufreq=N`:

| N | what |
| --- | --- |
| 1 | read only: say what the boot loader left, and once a minute after |
| 2 | the A75s to 2000 MHz |
| 3 | the A75s, then the A55s to 1700 MHz |

## Registers

| | |
| --- | --- |
| ARMPLL_L_CON1 (A75) | `0x1000c000 + 0x20c` |
| ARMPLL_LL_CON1 (A55) | `0x1000c000 + 0x21c` |
| Vproc11 set/read | MT6358 `0x13a6` / `0x139e` |
| Vproc12 set/read | MT6358 `0x1426` / `0x141e` |
| VSRAM11 set/read | `0x1bee` / `0x1b60` (bits 14:8) |
| VSRAM12 set/read | `0x1bf0` / `0x1ba2` |
| PMIC | pwrap at `0x1000d000` |

The voltage step is 6.25 mV from 0.5 V: 0.9625 V is step 74, 1.0875 V is step 94. VSRAM is
kept about 100 mV above Vproc (`step + 16`, capped at 99).

## The order

1. read Vproc, VSRAM, tracking and the PLL; refuse to touch anything that does not look like
   the boot loader's state (1175 MHz, post divider 1, Vproc 0.8 V);
2. raise VSRAM (unless the PMIC says it tracks Vproc);
3. raise Vproc and read it back;
4. write the PLL's new DDS with the CHG bit, then the post divider;
5. read everything back and say it.

The A75s go first: the window server and SpringBoard run there.

## What the A32's boot loader leaves (measured)

```
cpufreq: A75s at 1175 MHz (CON1 0x1169d89), Vproc step 48 (800 mV), VSRAM step 64 (900 mV)
cpufreq: A55s at 1174 MHz (CON1 0x8116989d), Vproc step 48 (800 mV), VSRAM step 64 (900 mV)
```

That is exactly what the guard expects (1175 MHz, post divider 1, Vproc 0.8 V), so the raise
may proceed. The read-only run is the check before any voltage is written.
