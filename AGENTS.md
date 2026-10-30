# AGENTS.md

Notes for AI coding agents (and people) working in this repository: what it is, how it is
laid out, and the rules that are not obvious from the code. Read it before changing anything.

## What this is

HakiOS for the **Samsung Galaxy A32 4G (SM-A325F)**: MediaTek Helio G80 (MT6769T), Mali-G52
MC2, eMMC. Apple's open XNU kernel (xnu-8796.121.2) and iOS 16.5's unmodified user space,
started by this phone's own loader. This is the device repository; the shared kernel, user
space and GPU stack live in the organisation's common repositories (`hakios-xnu-kernel`,
`hakios-metal`).

The phone is a Samsung/MediaTek one: the boot chain is Samsung's LK, the PMIC is an MT6358
through the MediaTek PMIC wrapper (pwrap), and the display is a command-mode DSI panel. Do
not carry Qualcomm assumptions (UEFI, UFS, Adreno, SMMU, QMI) over to this phone.

## Layout

| path | what | license |
| --- | --- | --- |
| `mtkloader/` | bare-metal loader, runs with the MMU off before XNU | MIT |
| `kernel/` | this phone's XNU drivers and the changes to Apple's files | APSL 2.0 |
| `tools/` | `chainboot.py`, `mktouchfw.py` | MIT |
| `diag/` | `a32diag`: the first chainboot/framebuffer diagnostic | MIT |
| `docs/` | how each part was found, register by register | MIT |
| `device.conf` | the phone and the state of each component | |

How a boot goes: `mtkloader` is chained in front of Android's kernel in `boot_a`
(`tools/chainboot.py`). Android boots normally; `printf DARWIN-REDWOOD:BOOT-XNU > /dev/pmsg0`
asks the loader for one XNU boot. The loader relocates itself and XNU to `0x40080000` (LK
loads the image at a random address), brings up what XNU cannot (display kick, PMIC, clocks),
builds XNU's device tree and boot arguments, and starts the kernel. The kernel's root is the
iOS system image in the `xnusys` partition, read off the eMMC as needed (`emmc_root=1`). A
run ends after the loader's watchdog (300 s here), which returns the phone to Android, or
earlier when five fingers are held on the screen.

Every hardware step is behind a boot-arg and does nothing without it (`touchscreen=3`,
`buttons=2`, `mali=1`). New hardware work follows the same pattern: numbered steps, reading
only first, each step's result said in the log.

## Rules

1. **Nothing of Apple's in the tree**: no iOS binary, image, cache, shader library, font,
   picture or screenshot, and no header derived from iOS firmware.
2. **Nothing of the vendor's**: no firmware, no partition image, no boot image, no device tree
   taken off a phone. Nothing here is required to build the loader and the kernel except the
   common tree.
3. **No personal data**: no device serial numbers, no home-directory paths, no tokens, no
   Wi-Fi names or passwords. Logs and backups stay out of the repository (`.gitignore`).
4. **Licenses**: `kernel/` is APSL 2.0; everything else is MIT. Do not relabel XNU-derived
   code as MIT. MediaTek's and Linux's GPL sources may be read for register addresses and
   hardware order and named where used, never copied.
5. **Names**: new code in the common repositories uses `hakios_*`, `kern.hakios.*`,
   `/dev/hakios-*`. Some files here still carry the project's former `redwood_*` prefix
   (`RedwoodMaliStart`, `redwood_mali_*`); do not rename them piecemeal.

## Working on the phone: safety

- Flash **only `boot_a`**. Never write slot B (`boot_b`, `system_b`, ...) or any partition
  other than `boot_a` and `xnusys`; `xnusys` is written only when the system image changes.
- Always pass the serial (`adb -s`, `fastboot -s`); a second phone may be connected.
- Keep the user's stock `boot_a` image; it is how the phone is restored. Magisk is not
  needed after the first setup: the chainboot image is flashed over `boot_a`.
- Runs are short (200–600 s) and one at a time. Do not raise CPU or GPU voltages.
- The eMMC driver is read only by construction; keep it so unless the owner asks otherwise.
- The touch screen's two rails are GPIO4 (1.8 V I/O) and PMIC VCAMA2 (3.0 V analog). The
  chip answers on I2C with only GPIO4 on but reports no fingers; both are needed.

## Building and running

What it needs:
- a Mac with the common HakiOS tree: kernel build, user space, `xnuloader/identity.h` made
  from the user's own IPSW;
- clang, plus the zig `ld.lld` that the common tree provides;
- `adb` and `fastboot`;
- an SM-A325F with an unlocked bootloader and its stock `boot_a` saved.

```sh
export HAKIOS_SERIAL=...      # adb devices
# from the common tree, with this repository's pieces applied:
python3 tools/chainboot.py stock-boot.img mtkloader.bin boot-a32.img xnu-payload.bin
```

Boot arguments used for the working image:

```
cpus=4 fbscale=3 touchscreen=3 buttons=2 mali=1
```

Four cores are stable; six were tried and ended in fastboot. XNU is given the 1.73 GiB run
between `0xd1600000` and `0x13ffefff`.

## Reading a run's log

The loader saves XNU's log into `pstore`; on this phone Android exposes it as
`/sys/fs/pstore/console-ramoops-2`, readable without root (mode 444). The start of a long log
is dropped; the drivers repeat their state once a minute for that reason.

## State, briefly

What works and what does not is in `device.conf` and `README.md`. The open work:
- GPU acceleration: panfrost service + Mesa DRM shim + a Metal driver for Mali (the hardware
  bring-up is done, `mali=1`);
- battery, sensors, Wi-Fi/Bluetooth, audio, camera, cellular.
