# a32diag: first diagnostic for the Galaxy A32 4G

Not HakiOS yet: a minimal program that runs **before** Android's kernel, inside the `boot`
partition, to check three things on the real phone:

1. that Samsung's bootloader (LK) accepts and runs the project's `chainboot`, the same trick
   the Infinix Hot 20 port uses;
2. that the A32's framebuffer address and pitch are what the device tree says
   (`0x7C720000`, pitch 4352, 1080×2400);
3. that `ramoops` is readable from Android at `/sys/fs/pstore/console-ramoops-0`.

After 10 seconds the program copies Android's kernel to `0x40080000` and jumps to it, so the
phone comes back to Android. **It writes no partition.**

## Files

- `a32diag.c`: the program.
- `fbdiag_start.S`: entry point and the stub that starts Android's kernel.
- `mtkdbg.h`: output to the framebuffer and to `ramoops` (A32 constants).
- `build.sh`: builds and packs with `../tools/chainboot.py`.
- `a32diag.bin` / `a32diag.img`: generated; git ignores both.

## Build

```sh
ANDROID_BOOT=/path/to/boot.img ./build.sh
```

`ANDROID_BOOT` is the stock (or Magisk-patched) Android boot image to chain to; it is not
part of this repository.

## Try it (TWRP)

1. Push the image to the phone:
   ```sh
   adb push a32diag.img /sdcard/ios/a32diag.img
   ```
2. Reboot into **TWRP**.
3. TWRP → **Install** → **Install Image** → `a32diag.img` → partition **Boot**.
4. Reboot to system. It should pass the Samsung/Android logo and come back to Android in
   10–20 seconds.
5. Read the log:
   ```sh
   adb shell "su -c 'cat /sys/fs/pstore/console-ramoops-0'" > a32diag-pstore.txt
   ```
   Look for `A32DIAG`, `fb=`, `pitch=`, `OVL0 L0_ADDR=`, `EL=`, `CNTFRQ=`.

## If something goes wrong

- The phone does not boot: hold **Volume Down + Power** to restart, enter TWRP
  (Volume Up + Power + USB) and flash your saved stock `boot.img` back into **Boot**.
- Never flash `seccfg`, `nvram`, `proinfo`, `efs`, `nvdata` or any other partition.
- This diagnostic touches neither the system, the data nor the modem.

## What the result means

- `A32DIAG` in the log: the bootloader accepts the chainboot, and the path is open for
  `mtkloader`/XNU.
- Bars on the screen too: framebuffer and OVL0 are fine; if not, the command-mode panel needs
  the loader's DSI refresh kick (the next step after this diagnostic).
- The phone returns to Android: the return stub works and we can iterate without Odin.
