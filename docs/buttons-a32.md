# Buttons on the A32: one in the keypad, two in the PMIC

The three buttons are not all on the same controller, and the device tree says why.

| button | where | register |
| --- | --- | --- |
| volume up | MTK keypad at `0x10010000` | `KP_MEM1` (`+0x004`) bit 1, clear while down; `kpd-hw-init-map` index 1 = `0x73` (`KEY_VOLUMEUP`) |
| volume down | MT6358 PMIC **homekey** | `TOPSTATUS` (`0x28`) bit 3, clear while down |
| power | MT6358 PMIC pwrkey | `TOPSTATUS` bit 1, clear while down |

The keypad map in the A32's device tree has only index 1 filled (`0x73`). Volume down is the
PMIC's homekey: Android's keypad driver reports the homekey interrupt as `KEY_VOLUMEDOWN`
(`kpd-sw-rstkey` `0x72` in `mediatek,kp`, see `mt6358/v1/pmic_irq.c` and
`hal_kpd.c`'s `kpd_pmic_rstkey_hal`). Power was already known from the Infinix port.

The PMIC is reached through the **pwrap** at `0x1000d000` (WACS2 command/read data/VLDCLR),
the same code the touch driver uses for VCAMA2.

## Reading it

A thread looks every 20 ms; a key has to read the same twice in a row before it counts.
Pressed reads are:

```
volume up    keypad 0xfbfd 0xeff7   (MEM1 bit 1, plus matrix ghosts in MEM2)
volume down  PMIC   0x6             (TOPSTATUS was 0xe; bit 3 clears)
power        PMIC   0xc             (bit 1 clears)
```

`keylog=1` prints every change, `keyraw=1` prints all five `KP_MEM` registers and `TOPSTATUS`
whenever any of them changes — the raw mode is what identified volume down.

## Result

`xnu-a32-emmc-test34-keys2.img` (`buttons=2 keylog=1 keyraw=1`): all three buttons work on the
iOS home screen, short power presses lock and unlock it.
