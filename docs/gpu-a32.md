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

## The job dump (test52)

The service was given a dump of the first four submissions (32 words each). The draw's jobs are
valid job headers:

```
job 1 at 0x90a0300: ... 00010015 ...   (vertex/tiler, runs)
job 2 at 0x90a0340: ... 00010013 ...   (fragment, runs)
job 3 at 0x90a0340: 3f800000 00000000 090a04c0 ...   (the blit: the draw's DATA, not a header)
```

The blit's `jc` points 0x40 into the pool, at the previous job's data, so the GPU rejects it
with `JS_STATUS 0x58 DATA_INVALID_FAULT`. The service receives the address from Mesa; the
suspect is how panfrost 25 computes/reuses the job-pool address (a 0x40 offset), not the GPU or
the ioctls. The next comparison would be with the Infinix's working Mesa/panfrost, which is not
published.

## The blit works (test54)

Two fixes from the public `y2sios` port made panfrost's blit run:

- `pan_kmod_bo_mmap` must map the **whole** BO and add `bo_offset` to the host address; ours
  ignored it, so the job descriptor was written 0x40 before its GPU address (the
  `DATA_INVALID_FAULT` of test52);
- `panfrost_bo_munmap` must not unmap at reference count 0: the BO cache keeps the mapping and
  the unmap goes in `panfrost_bo_free` (`panfrost-keepmap`), through `hakios_drm_unmap`.

With both, `gl_test` draws and reads the triangle back:

```
gl_test: reading 256 x 256 back
gl_test: 17086 of 65536 pixels coloured   (26%, the triangle's geometry)
RedwoodMali: a client goes after 4 job chains, 0 of them unfinished
```

The IOSurface import for CoreAnimation adds the surface's size in `pan_kmod.c` (the "file
descriptor" is the IOSurface's ID) and `osmesa_surface_resource` built for panfrost.

## CoreAnimation on the Mali (test55)

`GL=1` image, booted with `mali=1 gl=1`. The window server loads `libhakios.gl` and Mesa picks
the Mali:

```
hakios-gl: an OpenGL ES 3 context for CoreAnimation, by Mesa, off-screen (OSMesa):
           OpenGL ES 3.1 Mesa 25.0.7; Mali-G52 r1 (Panfrost); shaders OpenGL ES GLSL ES 3.10
RedwoodMali: request 0x7e, 40 bytes          (the 1125x2436 IOSurface imported as a buffer)
mali: step 6 of 6; 1 clients, 729 buffers, 21729 job chains (0 failed)
```

The display swaps the surfaces the GPU drew (swap #1275 at 143 s), the UI is visibly more
responsive, and the frame rate peaks at 21–31 a second (the software renderer was 10–14). No
backboardd crash.

What is still missing: the **wallpaper** (CoreAnimation's GL renderer does not draw it; the
Metal driver is what draws it on the Infinix), and the GPU-side polish of the panfrost BO cache
(`panfrost-retain`/`lazy-heap` from the public `y2sios` port).

The fixes that made it work, from that port: `pan_kmod_bo_mmap` adds `bo_offset` (test52's
0x40), cached BOs keep their CPU mapping (`panfrost-keepmap`), and the IOSurface import gets
its size from the service (`pan_kmod.c`) with `osmesa_surface_resource` built for panfrost.
