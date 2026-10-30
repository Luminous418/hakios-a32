# Metal on the A32: the driver found (stage 3)

The port already draws with CoreAnimation on the Mali-G52 through Mesa's panfrost and OpenGL ES
(`GL=1 gl=1`, `docs/gpu-a32.md`). Metal is the next layer: the same GPU, but through Apple's own
Metal framework, which is what CoreAnimation uses for the wallpaper and for the parts its GL
renderer leaves black.

The public `y2sios` port (Exynos 990, Mali-G77) has a working Metal implementation: the runtime
(`redwoodmetal.m`, `redwooddrawable.m`) talks to `libredwoodair`, which is built on Mesa's
panfrost and links **`libpanfrost_bifrost`**. The encoder is therefore not Valhall-only; the
runtime is generic and the compiler has the Bifrost backend this GPU needs. This is why the port
was worth attempting: the hard part (a Metal command encoder for Mali) is public and the GPU
family is supported.

## What was built (no flashing)

Kernel (`kernel/`):

- `iokit/Kernel/hakios.mali.cpp`: the syncobjs Mesa waits with (`0x7b/0x7c` set/get,
  `0xbf/0xc0/0xc3/0xc4/0xc5` create/destroy/wait/reset/signal), `submit_ex` (the job's `seqno`
  written back after the 48-byte request, `out_sync` signalled), `SetLabelBo`, and the port's
  counters at `0x7d` (chosen by the request's size so the GL shim's own wait at the same number
  still works).
- `bsd/dev/hakios.mali.c`: `redwood_mali_done()`/`redwood_mali_last()` for those waits, and the
  four `GET_PARAM`s Mesa 25.0.7 asks for (`SYSTEM_TIMESTAMP`, its frequency, priorities,
  coherency).
- `iokit/Kernel/MiniIOMobileFramebuffer.cpp`: `IOAcceleratorES::setProperties`, gated on
  boot-arg `metal=1`. This is the only thing that kept Metal from finding the driver: it reads
  `MetalPluginName` off the service, opens `/System/Library/Extensions/<name>.bundle` and takes
  `MetalPluginClassName` from it.

User land (the common tree's `userland/build.sh`, `METAL=1`):

- `lib/libredwoodair.dylib` (9.3 MB, 43 `rwa_*` functions), built with
  `METAL_TREE/…/mk-mesa.sh AIR=1 DRIVERS=softpipe,panfrost`;
- `lib/libredwoodmetal.dylib`, `lib/RedwoodMetal.bundle` (`RedwoodMetalDevice`),
  `bin/metal_props`, `bin/metal_drv_test`;
- `/Extensions/RedwoodMetal.bundle` → `/redwood/lib/RedwoodMetal.bundle`, and a **union** bind of
  that directory over `/System/Library/Extensions` (Apple's `AGXMetalA11` and
  `AppleMetalGLRenderer` stay visible);
- `bin/say_run`: a job's output ends at `/dev/console` and is kept nowhere, so this runs a
  program with its output on a pipe and sends every line to `kern.hakios.say` (the kernel log).
  That is how the test's report survives the reset.

The first image built this way found no device: the bind lived only in `flash_root()`
(`root_apfs`), and the A32 boots with `emmc_root=1`, where the root is the iOS image and this
small image is on `/redwood`. The bind now also runs in that mode, guarded by the directory
being there, so a RAM boot without `/Extensions` is unchanged.

## The run (test60, 300 s, back to Android by itself)

Boot arguments: `mali=1 gl=1 metal=1`. `rc.jobs.metalprops` waits 40 s (SpringBoard and
backboardd already drawing with GL), then turns the properties on, asks Metal, and turns them
off.

```
[ ok ] our Metal plug-in beside Apple's
metal_props: MetalPluginName (none)
metal_props: MetalPluginClassName (none)
IOAcceleratorES: MetalPluginName = RedwoodMetal, set by metal_props
IOAcceleratorES: MetalPluginClassName = RedwoodMetalDevice, set by metal_props
metal_props: Metal would open /System/Library/Extensions/RedwoodMetal.bundle: it is there
drvtest: sweep: MTLCreateSystemDefaultDevice() = RedwoodMetalDevice
drvtest: sweep: 11 classes, counted the way 10.17 counted the device: unanswered means absent
                 from the chain, and an entry whose body refuses is answered
drvtest: sweep: of 420 methods RedwoodMetalDevice requires, 0 are unanswered and 0 are
                 answered by an abstract stub of Apple's
drvtest: sweep: of 201 methods RedwoodMetalCommandQueue requires, 0 are unanswered ...
drvtest: sweep: of 51 methods RedwoodMetalCommandBuffer requires, 0 are unanswered ...
drvtest: sweep: of 143 methods RedwoodMetalRenderEncoder requires, 0 are unanswered ...
drvtest: sweep: of 47 methods RedwoodMetalComputeEncoder requires, 0 are unanswered ...
drvtest: sweep: of 46 methods RedwoodMetalRenderPipeline requires, 0 are unanswered ...
drvtest: sweep: of 19 methods RedwoodMetalComputePipeline requires, 0 are unanswered ...
drvtest: sweep: of 67 methods RedwoodMetalTexture requires, 0 are unanswered ...
drvtest: sweep: of 31 methods RedwoodMetalBuffer requires, 0 are unanswered ...
drvtest: sweep: of 4 methods RedwoodMetalSampler requires, 0 are unanswered ...
drvtest: sweep: of 6 methods RedwoodMetalDepthStencilState requires, 0 are unanswered ...
say_run: /redwood/bin/metal_drv_test exited with status 0
IOAcceleratorES: MetalPluginName removed by metal_props
```

The driver's own records (on stdout, now in the kernel log) show the service was used as the
Metal runtime expects:

```
RedwoodMali: a client opened the GPU
RedwoodMali: request 0x44, 16 bytes      (GET_PARAM, the GPU's parameters)
RedwoodMali: request 0x42, 24 bytes      (CREATE_BO)
RedwoodMali: request 0x43, 16 bytes      (MMAP_BO)
RedwoodMali: request 0xbf, 8 bytes       (SYNCOBJ_CREATE)
m2m-nodev: I DEV the compiler library Mali-G52 r1 (Panfrost)
```

No crash, no respring; the phone was back in Android 355 s after the boot. Metal discovery, the
driver's device object, its command model and the panfrost/Bifrost compiler library all load on
the A32.

## What is next

- `metal_drv_test` creating the device itself (`initWithAcceleratorPort:` + `MTLAddDevice`) and
  compiling a system `.metallib` (`/System/Library/VideoDecoders/AV1SW.metallib`) to Bifrost code.
- `--draw`: the first triangle through Metal, and then CoreAnimation on Metal (`--picture` at
  the panel's size), which is the road to the wallpaper.
- The runtime still calls the GPU "Mali-G77 MP11" in its name (`-name`, informative only); the
  compiler and the kernel side are this phone's G52.

Build the image with, from the common tree:

```sh
cd userland
GL=1 METAL=1 METAL_TREE=$HOME/metal-port FLASHBOOT=1 RC_JOBS=rc.jobs.metalprops ./build.sh
```

then pack `root.apfs` with `xnuloader/mkpayload.py` and flash only `boot` (`tools/chainboot.py`).
