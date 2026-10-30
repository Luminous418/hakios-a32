# The Mali-G52 on the A32: hardware brought up, software not yet

The GPU is a **Mali-G52 MC2** (Bifrost), the same block as the Infinix Hot 20's (MT6768). It
needs no firmware; bringing it up is power, clocks and a reset, in MediaTek's order. The
driver is `kernel/bsd/dev/hakios.mali.c`, ported from the Infinix's `redwood_mali.c`.

| | |
| --- | --- |
| topckgen | `0x10000000` (the `mfg_sel` mux, main PLL's third, 364 MHz) |
| SPM | `0x10006000` (MFG_ASYNC, MFG, MFG_CORE0, MFG_CORE1 and their SRAMs; bus protection once MFG is up) |
| mfgcfg | `0x13000000` (BG3D clock gate) |
| Mali | `0x13040000` (GPU_ID, reset, L2, tiler, shader cores, address space 0, job slots) |

## The six steps

1. the `mfg_sel` mux in topckgen;
2. the four power domains in the SPM, and the bus protection released;
3. the block's own clock gate (`BG3D` in mfgcfg);
4. the GPU: which one it is, a soft reset, L2/tiler/two shader cores powered;
5. one small job (write a word) through address space 0 in identity mode, read back;
6. the same job again through the GPU's own page tables.

Boot-arg `mali=1`. The A32's run reached all six:

```
mali: GPU_ID 0x74021000 (Mali-G52 r1p0), shader cores 0x3, tiler 0x1, L2 0x1: all powered
mali: first job, identity mode ... the word 0xc0ffee01 -- the GPU ran it
mali: second job, through the GPU's page tables ... -- the GPU ran it
mali: came up to step 6 of 6
```

`kern.hakios.mali` reports the step; the GPU stays powered afterwards.

## What acceleration needs

CoreAnimation still draws in software because the layer above the hardware is not here:

- **panfrost IOKit service** — the Infinix's `RedwoodMali.cpp` implements Linux's panfrost
  DRM interface for programs; it is available in the Infinix device repository, not in the
  public common one;
- **Mesa panfrost + a DRM shim** — `tools/mesa/hakios-drm` in the public tree speaks Adreno's
  msm interface only;
- **Metal for Mali** — the public `hakios-metal` driver is Adreno/PM4; the Infinix's
  AIR→NIR→panfrost Metal driver is not published.

## The panfrost service (test45)

`kernel/iokit/Kernel/hakios.mali.cpp` is the Infinix's `RedwoodMali.cpp` ported: an IOKit
service that speaks Linux's panfrost DRM interface (parameters, buffers, GPU addresses,
job chains, waits) and hands surfaces to/from MiniIOSurface. `MiniIOSurface` gained a
`maliSeq` and `MiniIOSurfaceDrawn()` waits on it, so the display never shows a surface the
Mali is still drawing.

With `mali=1` the kernel brings the GPU up and then starts it (test45):

```
mali: came up to step 6 of 6
RedwoodMali: the GPU is there for programs to draw with
```

So the path program → IOKit service → Mali is in place. What is still missing for
CoreAnimation acceleration is the client side: Mesa's panfrost needs a DRM shim
(`libdrm`-like) that talks to this service, and the public `hakios-metal` driver is
Adreno/PM4, not Mali. The realistic path remains Mesa panfrost plus CoreAnimation's
OpenGL ES backend (the Infinix's `camtl=0` fallback).

## The user-space test

`tools/mali_job_test.c` opens the `RedwoodMali` service, asks for the GPU ID, makes a 4 KB
buffer, maps it, writes the same "store `0xc0ffee01`" job the kernel's bring-up uses, submits
it, waits and reads the word back. Its result goes to `kern.hakios.say`, which the kernel
repeats in its log once a minute.

Build it with the common tree's cross compiler and put it in the small image:

```sh
xnu-clang -arch arm64 -isysroot $THEOS/sdks/iPhoneOS16.5.sdk -O1 \
    -framework IOKit -framework CoreFoundation -o mali_job_test tools/mali_job_test.c
cd <common-tree>/userland
FLASHBOOT=1 EXTRA=/path/to/mali_job_test RC_JOBS=rc.jobs.malitest ./build.sh
```

Then pack the payload with the new `root.apfs` and flash only `boot` (the small image travels
there; `xnusys` is not touched). `tools/rc.jobs.malitest` is the job list.

Run on the phone (test46), with Android back afterwards:

```
RedwoodMali: a client opened the GPU
RedwoodMali: request 0x44, 16 bytes      (GET_PARAM)
RedwoodMali: request 0x42, 24 bytes      (CREATE_BO)
RedwoodMali: request 0x40, 40 bytes      (SUBMIT)
mali_job_test: GPU 7402:1000, gpu address 0x1000000, job 1, wait 0, word c0ffee01 -- THE GPU RAN IT
```

So the whole path program → IOKit service → Mali works, with the same job the kernel's
bring-up runs.

## CoreAnimation with GL=1 (test51)

The image built with `GL=1` and booted with `mali=1 gl=1` loads `libhakios.gl.dylib` and Mesa
panfrost into backboardd, and the service sees the screen and buffer requests. But backboardd
crashes (`Corpse allowed` at 206 s and again at 218 s), so the window server never draws and the
panel keeps the loader's last frame ("Jumping to XNU"). It is the same panfrost job/blit fault
that makes `glReadPixels` fail, now in CoreAnimation's path.

So the stable image stays the one without GL (`xnu-a32-emmc-test41-stability.img`): display,
touch, buttons, CPU, battery and the Mali service, with CoreAnimation drawn by the processor.
