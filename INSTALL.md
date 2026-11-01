# Installing HakiOS on the Galaxy A32 4G

This is the procedure used on a **Samsung Galaxy A32 4G (SM-A325F)**: MediaTek Helio G80, Mali-G52
MC2, eMMC, legacy non-A/B (the partition is `boot`, there is no slot B). It assumes the one-time
preparation of the phone has already been done: the iOS system image lives in its own 4 GiB
partition, `xnusys`, carved from the end of `userdata`, and the loader reads it from the eMMC as
the kernel asks for it. **Creating `xnusys` is destructive and is done once**; everything below
only ever writes `boot`.

Nothing is replaced for good: `boot` holds Android's kernel by default, and the loader starts XNU
for one run only when Android asks for it. After the run the phone comes back to Android with the
run's log.

## What you need

- a Galaxy A32 4G (SM-A325F) with the bootloader unlocked;
- Android 13, rooted (Magisk), with **TWRP** available, and **your stock `boot` image saved**;
- a Linux host with `adb`, `python3`, and the HakiOS common tree built: the cross toolchain
  (`xnu-deps`), the kernel, the user land, and the iOS system image made from **your own IPSW**
  (`sysimage/fetch.sh`);
- this repository, and the common tree's `devices/samsung-a32` pieces (the loader and its tools).

Nothing of iOS, no firmware and no image of the phone is in the repository; you bring those.

## 1. Build the kernel

```sh
cd ~/hakios
./xnu-build.sh -j"$(nproc)"
```

The kernel comes out as
`xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`.

## 2. Build user land

```sh
cd ~/hakios/userland
GL=1 CHRONO=1 FLASHBOOT=1 ./build.sh
```

`GL=1` builds Mesa (panfrost) and the OpenGL ES library CoreAnimation uses on the GPU;
`CHRONO=1` adds WidgetKit's daemon; `FLASHBOOT=1` makes the small image (`root.apfs`) for a boot
whose root is the iOS volume on the phone's flash.

Two resources are dropped from the system image when it is extracted and have to travel in
`root.apfs` instead, taken from **your own IPSW**:

```sh
7z e -o/tmp/quartzcore  rootfs.dmg 'System/Library/Frameworks/QuartzCore.framework/default.metallib'
7z e -o/tmp/renderbox   rootfs.dmg 'System/Library/PrivateFrameworks/RenderBox.framework/default.metallib'
```

and given to the build:

```sh
GL=1 CHRONO=1 FLASHBOOT=1 \
METAL_QUARTZCORE=/tmp/quartzcore/default.metallib \
METAL_RENDERBOX=/tmp/renderbox/default.metallib \
./build.sh
```

Without them CoreAnimation asserts on its missing library ("metallib_url != nil") and SwiftUI's
renderer (RenderBox, and with it the widgets) aborts with "unable to find default.metallib".

A wallpaper of your own is optional:

```sh
magick picture.jpg -crop 1080x400 +repage -strip PNG24:/tmp/strips/wallpaper-%d.png
GL=1 CHRONO=1 FLASHBOOT=1 \
WALLPAPER_FILE=picture.jpg WALLPAPER_STRIPS=/tmp/strips \
METAL_QUARTZCORE=/tmp/quartzcore/default.metallib \
METAL_RENDERBOX=/tmp/renderbox/default.metallib \
./build.sh
```

## 3. Pack the payload

```sh
cd ~/hakios/userland
python3 ~/hakios/xnuloader/mkpayload.py -o /tmp/payload.bin \
  kernel=~/hakios/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 \
  ramdisk=root.apfs
```

## 4. Build the loader and pack `boot`

```sh
cd ~/hakios/devices/samsung-a32/mtkloader
./build.sh -DWATCHER_NO_KICK=1 -DXNU_CORES=6 -DXNU_WDT_SECONDS=0 \
  -DXNU_WATCH_SECONDS=180 -DXNU_RETURN_AFTER='"0"' \
  -DXNU_EXTRA_ARGS='"cpus=6 fbwidth=810 fbheight=1800 fbscale=3 fb_filter=0 fb_threads=4 touchscreen=3 buttons=2 mtk_cpufreq=3 battery=2 mali=1 gl=1 wallpaper=1 mtk_kick=1 te_sync=1"'

python3 ../tools/chainboot.py ~/ios/backup/boot-stock.img \
  mtkloader.bin ~/ios/boot-a32.img /tmp/payload.bin
```

What the boot arguments do:

| argument | meaning |
| --- | --- |
| `cpus=6` | six of the eight cores (one is left to the loader's watcher); the A75s are used first |
| `fbwidth=810 fbheight=1800 fbscale=3` | the interface's size: 270 x 600 points, scaled by 1.33 to the 1080 x 2400 panel (the A22's interface size; 1125 x 2436 would be the iPhone X's 375 points, and 720 x 1600 breaks the status bar) |
| `fb_filter=0` | nearest-neighbour to the panel instead of bilinear: faster, and text stays sharp at this ratio |
| `fb_threads=4` | processors that copy a frame into the panel together |
| `touchscreen=3 buttons=2` | the touch screen and the buttons |
| `mtk_cpufreq=3 battery=2` | both CPU clusters at full clock; the battery gauge |
| `mali=1 gl=1` | the GPU, and CoreAnimation on OpenGL ES through panfrost |
| `wallpaper=1` | the picture from `WALLPAPER_STRIPS`, or a gradient when there is none |
| `mtk_kick=1 te_sync=1` | XNU starts each frame itself, on the panel's TE edge, after the DSI is idle |

The Metal layer (the private `hakios-a32-metal` tree) adds `metal=1 metal_plugin=1 metal_profile=1`
and its own build steps.

## 5. Flash `boot`

```sh
adb reboot recovery                      # TWRP
adb push ~/ios/boot-a32.img /tmp/boot.img
adb shell 'dd if=/tmp/boot.img of=/dev/block/by-name/boot bs=1048576; sync; rm -f /tmp/boot.img'
adb reboot
```

`boot` starts Android as before.

## 6. One run of iOS

From Android:

```sh
adb shell 'printf DARWIN-REDWOOD:BOOT-XNU > /dev/pmsg0'
adb reboot
```

XNU runs once. The loader's watchdog (180 s here) takes the phone back to Android; **five fingers
held for 1.5 s** return earlier. Do not reboot by hand while XNU runs -- that loses the log.

The run's kernel log is left in `/sys/fs/pstore/console-ramoops-2`, readable without root:

```sh
adb shell 'cat /sys/fs/pstore/console-ramoops-2' > run.log
```

## 7. Going back

Flash your stock `boot` image again (same `dd` as above); `xnusys` and the rest are untouched.
Android's own kernel was never replaced, only the small loader in front of it.

## Notes

- Keep a copy of the stock `boot` image before the first flash; it is the way back.
- `boot` is the only partition this procedure writes.
- The loader is built with `-DWATCHER_NO_KICK=1` because XNU starts the frames itself
  (`mtk_kick=1`); a loader that also kicks races the framebuffer and the screen tears.
