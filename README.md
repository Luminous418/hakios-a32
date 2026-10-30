# hakios-a32

HakiOS for the **Samsung Galaxy A32 4G (SM-A325F)**: MediaTek Helio G80 (**MT6769T**), GPU
**Mali-G52 MC2**, eMMC. iOS 16.5's own user space (launchd → backboardd → SpringBoard → apps)
on Apple's open XNU kernel (xnu-8796.121.2), with this phone's own loader and drivers.

The A32 is a **legacy non-A/B** phone: its boot partition is `boot` (there is no `boot_a`
and no slot B).

Nothing on the phone is replaced for good: `mtkloader` sits in `boot` in front of Android's
own kernel, starts XNU once when Android asks for it, and the phone comes back to Android
after every run. The iOS system image lives in a partition of its own (`xnusys`, carved from
the end of `userdata`) and the kernel reads it off the eMMC as it needs it.

## What works

| | | |
| --- | --- | --- |
| boot | works | `mtkloader/`: MMU off, eMMC through MSDC0, device tree and boot arguments for XNU; 4 cores |
| storage | works, read only | the system image read off the eMMC as needed (`emmc_root=1`): 4 GiB `xnusys`, ~2.5 GB of iOS |
| display | works | 1080×2400 command-mode DSI, iOS at scale 3; the loader's watcher kicks the panel and checks DSI busy (`kernel/iokit/Kernel/MiniIOMobileFramebuffer.cpp`, `mtk_kick=0`) |
| touch | works | STM FTS5CU56A on I2C6 at 0x49, bit-banged on GPIO89/90 (`kernel/bsd/dev/hakios.fts.c`); its two rails (GPIO4 and PMIC VCAMA2) are switched on from the kernel |
| buttons | works | power and volume up/down (`kernel/bsd/dev/hakios.mtkkeys.c`) |
| GPU | hardware works | Mali-G52 r1p0 brought up from the kernel (clocks, SPM domains, reset, L2/tiler/shaders, two real jobs: `mali=1`); CoreAnimation still draws in software, see below |
| CPU | works | 4 cores; A75 at 1999 MHz and A55 at 1699 MHz with `mtk_cpufreq=3` (`kernel/bsd/dev/hakios.mtkcpufreq.c`); a ten-minute run stayed there (`docs/stability-a32.md`) |
| battery | works | MT6358 `BATADC` (voltage, 3:1 divider) and `CHRDET`, `battery=2`; temperature and current are still placeholders |
| sensors | partial | the SCP sensor hub runs and the accelerometer streams (`scp_step=6`, `kernel/bsd/dev/hakios.scp.c`); turning the samples into screen rotation is still user-space work |
| Wi-Fi / Bluetooth, audio, camera, cellular | missing | |

### GPU: what is and what is not

`kernel/bsd/dev/hakios.mali.c` (boot-arg `mali=1`) is the Infinix Hot 20's Mali-G52 bring-up
ported to this phone. It gets all the way to step 6 of 6:

```
mali: GPU_ID 0x74021000 (Mali-G52 r1p0), shader cores 0x3, tiler 0x1, L2 0x1: all powered
mali: first job, identity mode ... the word 0xc0ffee01 -- the GPU ran it
mali: second job, through the GPU's page tables ... -- the GPU ran it
mali: came up to step 6 of 6
```

The GPU itself works. What is missing for acceleration is the layer above: the panfrost IOKit
service, a panfrost DRM shim for Mesa, and (for Metal) a Mali Metal driver. The public
`hakios-xnu-kernel` and `hakios-metal` repositories only carry the Adreno/msm side; the
Infinix's Mali panfrost/Metal work is not published. Leaving `mali=1` on changes nothing
else: the GPU stays powered and tested.

## What is here

- `mtkloader/` — the bare-metal loader (MIT): chains in front of Android's kernel in
  `boot`, relocates itself and XNU to `0x40080000`, reads the eMMC, builds XNU's device
  tree and boot arguments, and brings Android back with XNU's log in `pstore`.
- `kernel/` — this phone's drivers and changes in XNU (APSL 2.0, see `kernel/README.md`).
- `tools/` — `chainboot.py` (pack a bare-metal program in front of Android's kernel) and
  `mktouchfw.py` (kept for the project's other phones; the A32's touch needs no firmware).
- `diag/` — `a32diag`, the first program that proved the chainboot and the framebuffer on
  this phone.
- `docs/` — how the display, the touch screen, the buttons and the GPU were found, register
  by register.
- `device.conf` — the phone and the state of each component.

## Not here, by design

No file of iOS, no firmware of the phone, no boot/partition image, no screenshot, no
`xnuloader/identity.h` (made from your own IPSW), no log with a device's data. `LEGAL.md`
says it in full. Build outputs (`*.o`, `*.bin`, `*.img`, generated headers) are ignored by
`.gitignore`.

## Running

What it needs:
- a Galaxy A32 4G (SM-A325F) with an unlocked bootloader, Android 13 rooted (Magisk), and
  its stock `boot` image saved;
- a Mac with the common HakiOS tree (kernel build, user space, the iOS system image made
  from your own IPSW).

The loader and the kernel are built from the common tree; this repository holds the phone's
pieces:

```sh
export HAKIOS_SERIAL=...                 # your phone, as `adb devices` shows it
# build mtkloader and the kernel with the common tree, then pack boot:
python3 tools/chainboot.py stock-boot.img mtkloader.bin boot-a32.img xnu-payload.bin
```

One run, with the A32 pieces enabled:

```
cpus=4 fbscale=3 touchscreen=3 buttons=2 mali=1
```

`boot` boots Android by default. To ask for one XNU run, from Android:

```sh
adb shell 'printf DARWIN-REDWOOD:BOOT-XNU > /dev/pmsg0'
adb reboot
```

XNU runs for the loader's watchdog (300 s is the value used here), returns to the loader,
and the loader starts Android with the run's log in
`/sys/fs/pstore/console-ramoops-2` (readable without root on this phone). Do not reboot by
hand while XNU runs: that loses the log. Five fingers held on the screen for 1.5 s also
return to the loader and keep it.

## Licenses

`kernel/` is APSL 2.0 (`LICENSE.APSL`), as code built into XNU; everything else is MIT
(`LICENSE`). See `NOTICE` and `LEGAL.md`.
