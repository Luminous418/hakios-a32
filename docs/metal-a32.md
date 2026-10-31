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

## The first dispatch (test61)

`rc.jobs.metalcompute` repeats the sweep's setup and then runs one Apple compute kernel,
`MetalFaceMask_InitMinMax` from `/System/Library/CoreImage/PortraitFilters.cifilter/default.metallib`
— the one the `y2sios` port tried first: one 24-byte device buffer, no textures, no samplers,
and it writes its buffer unconditionally, so an all-zero result means the dispatch did not
happen rather than that the kernel had nothing to say.

```
drvtest: before anything, MTLCreateSystemDefaultDevice() = RedwoodMetalDevice
drvtest: it calls itself "Mali-G77 MP11 (product 0x7402, Redwood)"
drvtest: newBufferWithLength:options: gave RedwoodMetalBuffer
drvtest:   length 4096, contents 0x100b40000 ... wrote 4096 bytes and read back the same bytes
drvtest: .../default.metallib, 213028 bytes
drvtest: newLibraryWithData: gave _MTLLibrary with 36 functions
m2m-nodev: I PSO compute pipeline made: MetalFaceMask_InitMinMax (1 buffers, 0 textures, 0 samplers)
drvtest: compute MetalFaceMask_InitMinMax compiled: 256 threads per group, width 8, 0 bytes of threadgroup memory
drvtest: of 1 functions, 1 compiled and 0 were refused
drvtest: newCommandQueue gave RedwoodMetalCommandQueue
RedwoodMali: request 0x42, 24 bytes        (CREATE_BO)
RedwoodMali: request 0x43, 16 bytes        (MMAP_BO)
RedwoodMali: job 1 at ... (+0x1c0, buffer 38), fragment 0, 5 handles
drvtest: commandBuffer RedwoodMetalCommandBuffer, computeCommandEncoder RedwoodMetalComputeEncoder
drvtest: dispatched MetalFaceMask_InitMinMax: status 4
drvtest: buffer 0: 15 non-zero bytes of 4096, first words 7fffffff 7fffffff 7fffffff 80000000
RedwoodMali: a client goes after 1 job chain, 0 of them unfinished
say_run: /redwood/bin/metal_drv_test exited with status 0
```

`status 4` is Metal's completed, the first words are `atomicMinMax`'s answer, and the job is
one chain with 0 unfinished: the AIR front end compiled the function to Bifrost and the GPU
ran it.

**Stability, open**: backboardd restarted once 80 s later (its GL client closed after 13,430
jobs, 0 unfinished; memory was 1.2 GB available and no job faulted). The same Gl-only port has
resprung before, so it may not be Metal's; a control boot with the same kernel and `metal=1`
but no properties set is the next measurement before drawing.

## The first triangle through Metal (test72)

`rc.jobs.metaldraw` draws four vertices as a strip with the generated vertex stage and
`clearFragment20` from `AppleMetalGLRenderer.bundle/default.metallib`, into three 4×4
`RGBA32Uint` attachments, and compares the first pixel of attachment 2 against the four words
softpipe produced for the same shader (2458616957 2930221209 3401825461 3873429713).

The run:

```
drvtest: newRenderPipelineStateWithDescriptor: gave RedwoodMetalRenderPipeline
drvtest: draw: commandBuffer RedwoodMetalCommandBuffer, computeCommandEncoder ...
drvtest: draw: status 4
drvtest: draw: attachment 2 at (3, 3) = ...
drvtest: draw: the pixel at (0, 0) of attachment 2 is 2458616957 2930221209 3401825461 3873429713
drvtest: draw: THE SAME -- a Metal fragment shader drew on the Adreno
RedwoodMali: a client goes after 3 job chains, 0 of them unfinished
mali: step 6 of 6 ... 19 job chains (0 failed)
```

(The verdict string says "Adreno" because it comes from the `y2sios` port; the GPU is the G52.)
No GPU fault, no respring, and the same boot's window server kept drawing with GL.

Getting there took two fixes in the AIR front end (`tools/mesa/air-integer-ubo.patch`, the two
hunks against `y2sios`'s `air_metal.c`), both found by measurement rather than guessed:

- **signed integer outputs.** `clearFragment20` writes four words ≥ 2^31, which as `int` are
  negative; the target is `uint`. `retype_integer_outputs` changed the output *variable* to
  `uvec4`, but `nir_lower_io` takes the store's source type from the **dereference**, which was
  still typed `ivec4`, so Bifrost's blend unit saturated every word to 0. The pass now retypes
  the variable's dereferences as well; the NIR says `uvec4` and `src_type=uint32`, and the bits
  land untouched.
- **constant buffers.** The shader does not write a constant: it stores what it loads from UBO 14
  at offset 16. On the GPU every word read 0, on softpipe the same code read the right bytes.
  A Metal buffer is written by the program through its own mapping; the ARM cores here are not
  coherent with the device, and a clean from the core that submits does not clean the writer's.
  UBOs of 4 KB or less are now bound as **user buffers**, so Panfrost copies them on the
  processor into the batch pool — the memory every descriptor already goes through — and the
  shader reads them correctly. Larger blocks keep the GPU descriptor and its mapping.

The kernel side that the path also needed: user buffers are mapped **cache-inhibited** (not
write-combined), and `submit` cleans and barriers the buffers a job names before queuing it
(`redwood_mali_to_memory`, `redwood_mali.cpp`), the same arrangement as `y2sios`'s.

Open for later: the clear action of an integer attachment packs the float 1.0 as `0x3f800000`
instead of the word 1 (a clear-only pass reads `1065353216`), which matters only to integer
render targets; floats — what CoreAnimation draws — are unaffected.

## CoreAnimation on Metal (test75)

`metal=1 metal_plugin=1` (a boot-arg that publishes `MetalPluginName` and
`MetalPluginClassName` on `IOAcceleratorES` **at registration**, before launchd exists) is what
makes backboardd, SpringBoard and QuartzCore find the driver on their first device request,
instead of the userland switch a test process uses.

What was in the way, in order, each measured on the phone:

1. **QuartzCore's own Metal library.** CA asserts `metallib_url != nil` (`ogl-metal.mm:3308`)
   because `select-system.py` drops every `.metallib` (`SKIP_EXT`) and the framework's
   `default.metallib` is one of them. The file (1 MB, from the 20F66 IPSW) travels in this
   image and is shown over `QuartzCore.framework` with a **union bind** (`src/init.c`).
   Its `default.pipelinelib/pipelines.<device>` is still missing, and CA logs
   `Metal failed to load pipeline library: Could not open pipeline cache file ...` and carries
   on, exactly as the runtime's `-targetDeviceInfo` answer was designed for.
2. **Siri's orb.** The first SpringBoard abort after that was `_MTLReportFailure <-
   MPSGetImageType <- MPSImage <- -[SUICOrbView _updateMetalTextureRasterSize:] <-
   -[SBAssistantController _init] <- ... <- applicationDidFinishLaunching`: MPS refuses the
   orb's image on this driver. Siri is outside this port, so SpringBoard's
   `+[SBAssistantController sharedInstance]` is answered `nil` (`inject_sb.c`); the assistant
   is not started and the rest of SpringBoard does not need the singleton to be an object.

The result: SpringBoard finishes launching and **CoreAnimation draws the home screen through
Metal** — icons, labels, the dock, the search pill, page transitions — with **no crash and no
respring** in the 150 s run. The runtime's counters: ~50 command buffers a second, ~550 draws
in 5 s, commit 0.6 ms, GPU wait 4–10 ms, 0 refused pipelines, 0 surface copies.

What is still wrong, measured:

- **10–26 frames a second.** The draws are the cost on the processor: ~100 CA draws a frame at
  0.3–1.2 ms of Gallium each, so a swipe runs at ~26 fps (the GL renderer was 16–21).
- **No wallpaper.** It is not drawn at all: no poster/wallpaper process appears in the log
  (`PosterBoard` is a LaunchAngel app, `RunAtLoad=false`, started by RunningBoard on demand),
  and SpringBoard shows black behind the icons.
- **Widgets white, status bar glyphs clipped.** Their content comes from other processes;
  icons (decoded in-process) are correct.

## A wallpaper of our own (test76)

With no poster configured, the home screen is black. PosterBoard is the process that owns iOS
16's wallpaper, and it is a LaunchAngel app started by RunningBoard on demand; none of that runs
here. So SpringBoard draws one of ours, behind the icons by construction: a **CAGradientLayer**
at the bottom of `SBHomeScreenViewController`'s and `SBLockScreenViewController`'s view layers
(`inject_sb.c`, boot-arg `wallpaper=1`). The first attempt inserted a UIView at index 0 of every
window's root view and covered the UI; a window is not the home screen, and its root view is above
whatever the springboard draws. A layer on the home screen's own view is below every icon and
overlay.

On the phone: the blue gradient behind the icons and the dock, the labels and the search pill
readable, and the lock screen has it too. `a wallpaper gradient behind N view(s)` in the log says
which views were found. It is a placeholder, not Apple's poster (no video, no depth, no
ProceduralWallpaper), but it is drawn by CoreAnimation on the Mali like everything else.

## Tearing and the swap's sequence (test77)

The first frames of CoreAnimation on Metal tore: a Settings sheet over the home screen came out in
horizontal bands of the previous frame. The display's log named the cause:

```
MiniIOMobileFramebuffer: swap 1402: the GPU is not done with surface 3 after 500 ms; shown as it is
```

The display waits, per surface, for the job that drew it before copying it to the panel. It read
that job from `surface->maliSeq` **at the moment of showing**, and `maliSeq` is the last job that
named the surface -- including jobs that only READ it, such as the blur of the screen behind a
sheet. A later blur moved the sequence on, the display waited for work that had nothing to do with
the frame, gave up after 500 ms and copied a surface still being drawn.

The swap now takes the sequence **when it is queued** (`present()` copies `maliSeq` into
`fSwapSeq`), and `show()` waits for that snapshot. Measured on the phone: the `GPU is not done`
lines went from two or more a run to **zero**, and the heavy tearing of that picture is gone.
Some glitches remain (a band of a second gradient from a nested controller was one of them, now
inserted only in the controller that owns the whole screen; partial redraws of moving icons are
still visible), which is the next thing to chase.

## The framebuffer race behind the rest of the tearing (test76-78)

After the swap-sequence fix the heavy tearing was gone, but the home screen still tore: a page
swipe showed half of one frame and half of the next. The reason was not the GPU and not the
panel: the **watcher in `mtkloader`** started the DSI transfer every 16 ms, whenever it liked,
while XNU's presenter was copying the next frame into the same framebuffer. The DSI read the
memory for 11 ms, so a frame that arrived during the transfer went out in pieces.

The display is now serialized by one side:

- `mtkloader` is built with `-DWATCHER_NO_KICK=1`: its watcher still keeps the return snapshot,
  but does not start transfers;
- XNU's presenter waits for `DSI_BUSY` (0x1401400c bit 31) to clear **before drawing** the next
  frame into the framebuffer, draws, and kicks (`mtk_kick=1`) after. No write overlaps a
  transfer.

Measured on the phone: "una diferencia notoria, considerablemente menos" tearing, and the frame
rate stays at ~26 fps. The few app cases left are in blurred regions (a backdrop filter reading
the screen) and in the panel's own scanout.

**TE**: the DTBO gives the panel's tearing-effect signal on GPIO 44 (`lcm_dsi_te`, pinmux
0x2c01, the `dsi_te` node's interrupt). The option is in the driver (`te_sync=1`): start the
transfer on a TE edge, with a debounce and a fallback. Measured (test78): **no edges were seen**
on the pin and the 20 ms timeout per frame cost the rate (19 fps), so it is off by default.

## What is next

- The per-draw cost of panfrost's command submission, which is what sets the frame rate
  (10–26 fps; CA asks for ~100 draws a frame at 0.3–1.2 ms each).
- The white widgets and the clipped status-bar glyphs: their content comes from other processes.
- A real poster: PosterBoard, or a still image from the system's wallpaper assets, instead of the
  gradient.
- The runtime still calls the GPU "Mali-G77 MP11" in its name (`-name`, informative only); the
  compiler and the kernel side are this phone's G52.

Build the image with, from the common tree:

```sh
cd userland
GL=1 METAL=1 METAL_TREE=$HOME/metal-port FLASHBOOT=1 RC_JOBS=rc.jobs.metalprops ./build.sh
```

then pack `root.apfs` with `xnuloader/mkpayload.py` and flash only `boot` (`tools/chainboot.py`).
