# The A32's pieces of XNU

These files sit at the paths they have in the common kernel tree (`xnu-8796.121.2`); copy
them over it and rebuild. Everything here is APSL 2.0, except nothing: all of it is built
into XNU.

| file | state | what |
| --- | --- | --- |
| `bsd/dev/hakios.fts.c` | new | the touch screen: STM FTS5CU56A on I2C6 at 0x49, I2C bit-banged on GPIO89/90, GPIO4 and PMIC VCAMA2 switched on, 16-byte events read every 8 ms and posted to `/dev/hakios-touch` (`touchscreen=3`) |
| `bsd/dev/hakios.mtkkeys.c` | new | the buttons: volume up from the MTK keypad (`KP_MEM1` bit 1), volume down from the MT6358 homekey (`TOPSTATUS` bit 3), power from `TOPSTATUS` bit 1, debounced, posted as `HAKIOS_BUTTON_*` (`buttons=2`) |
| `bsd/dev/hakios.mali.c` | new | the Mali-G52 MC2 brought up from the kernel (topckgen mux, SPM power domains, mfgcfg clock gate, reset, L2/tiler/shaders, two job tests) (`mali=1`); ported from the Infinix Hot 20's driver |
| `bsd/dev/hakios.mtkcpufreq.c` | new | the two CPU clusters: ARMPLL2/ARMPLL1 and the MT6358's Vproc11/Vproc12, `mtk_cpufreq=1` read only, `=2` the A75s to 2000 MHz, `=3` the A55s to 1700 MHz too |
| `bsd/dev/hakios.mtkbattery.c` | new | the battery: MT6358 AUXADC channel 0 (`RQST0` bit 0, `ADC0`, 3:1 divider) for the voltage and `TOPSTATUS` bit 2 for the charger, reported to `hakios_battery_report()` (`battery=2`) |
| `bsd/dev/hakios.input.c` | changed | calls `hakios_fts_init()`, `hakios_mtkkeys_init()` and `hakios_mali_init()` from `hakios_input_init()` |
| `bsd/sys/hakios.input.h` | changed | prototypes for the three |
| `bsd/dev/hakios.goodix.c` | changed | only `touchscreen=1` starts the Goodix/FocalTech driver; `touchscreen=3` is this phone's STM |
| `bsd/dev/hakios.battery.c` | changed | only `battery=1` uses the Qualcomm UEFI cache; `battery=2` is the A32's MT6358 driver |
| `bsd/dev/hakios.buttons.c` | changed | only `buttons=1` starts the Qualcomm SPMI button driver; `buttons=2` is `hakios.mtkkeys.c` |
| `bsd/conf/files` | changed | lists `hakios.fts.c`, `hakios.mtkkeys.c` and `hakios.mali.c` |
| `iokit/Kernel/MiniIOMobileFramebuffer.cpp` | changed | `mtk_kick` defaults to 0: the A32's DSI panel is kicked by the loader's watcher (with the DSI busy check), not by every frame from the kernel |

## Boot arguments

```
touchscreen=3          the STM FTS on I2C6
buttons=2              the A32's keypad and PMIC keys
mali=1                 bring the Mali up and run its two test jobs
battery=2              the A32's MT6358 battery ADC and charger detect
cpus=4                 four cores; six ended in fastboot
fbscale=3              iOS at scale 3 on the 1080x2400 panel
touch_swap=1, touch_flip_x=1, touch_flip_y=1   if the touch axes need turning
touch_poll_us=N        how often the touch chip is asked (default 8000)
keylog=1, keyraw=1     a line for every button change / every raw keypad value
```

## What is deliberately not here

The panfrost IOKit service, the Mesa DRM shim and the Metal driver: the public repositories
have the Adreno/msm side only. `hakios.mali.c` stops at proving the GPU (jobs through
identity and through its page tables); see the main README.
