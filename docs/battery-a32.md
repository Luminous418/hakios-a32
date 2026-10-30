# Battery on the A32: the MT6358's own ADC

The phone's battery is measured by the PMIC, an **MT6358**, on **AUXADC channel 0 (BATADC)**.
There is no fuel gauge in this driver yet: the percentage is interpolated from the voltage.

## Registers (through pwrap at `0x1000d000`)

| | |
| --- | --- |
| request | `RQST0` (`0x1108`) bit 0 |
| result | `ADC0` (`0x1088`): bit 15 ready, bits 14:0 the value |
| charger | `TOPSTATUS` (`0x28`) bit 2, CHRDET (clear with no charger) |

The device tree's `batadc` node says `resistance-ratio = <3 1>` and 128 averages; with a 1.8 V
full scale and 15 bits:

```
mV = raw * 3 * 1800 / 32768
```

Linux's `drivers/iio/adc/mt635x-auxadc.c` does the same arithmetic.

## The percentage

A Li-ion curve, interpolated:

```
3300 mV 0 %    3750 32 %   3950 70 %
3450  3 %      3800 42 %   4000 76 %
3550  7 %      3850 53 %   4050 81 %
3650 14 %      3900 62 %   4100 87 %
3700 22 %                  4200 100 %
```

Calibrated against this phone's Android: **4086 mV = 82 %** at 27.9 C with USB in. Android's own
reading uses coulomb counting, so the two differ by a few percent as the load moves the voltage;
the value is averaged over four conversions and read every 10 s.

## Result

`xnu-a32-emmc-test40-battery.img` (`battery=2`) at 82 % in Android:

```
battery: MT6358 BATADC and CHRDET, read every 10 s
battery: 4050 mV, 81%, charger on
battery: 4002 mV, 76%, charger on
```

iOS shows the real level and the charging bolt. Temperature (25 C) and current (0 mA) are
placeholders for now.
