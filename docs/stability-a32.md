# A ten-minute run (test41)

`xnu-a32-emmc-test41-stability.img`, boot arguments:

```
cpus=4 fbscale=3 touchscreen=3 buttons=2 mtk_cpufreq=3 battery=2 mali=1
```

The phone was used for ten minutes: Settings, Photos, Maps, Safari, switching apps,
dark mode, lock/unlock, volume. The loader's 600 s watcher ended the run; Android came back
with the log.

## What the log says

- no panic: the loader's summary says `(no panic string)`;
- SpringBoard stayed pid 8 for the whole run (no respring);
- no memory kills or jetsam events;
- the CPU read 1999 MHz (A75) and 1699 MHz (A55) every minute, voltages unchanged
  (96 / 76 steps);
- the battery read 73–80 % while charging (3978–4046 mV), the charger always on;
- frame rate moved between 1 and 24 a second with what the screen was doing, as expected
  with CoreAnimation drawn by the processor.

One daemon, `activityawardsd` (Fitness), aborted once at 167 s; the corpse was collected and
nothing else noticed. It is a user-space daemon failure, not a kernel one.
