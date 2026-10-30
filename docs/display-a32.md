# Display on the A32: command-mode DSI, the watcher and the busy check

The panel is Samsung's `s6e3fc3_a32`, 1080×2400, in **command mode**: nothing is scanned out
until the host writes a frame and kicks the DSI. Linux does this from the panel's own driver
(`drivers/misc/mediatek/lcm/smcdsd_panel/panels/s6e3fc3_a32.c`, `DSI_WRITE(... DISPLAY_ON)`
and the `update` path).

LK leaves the display with OVL0 layer 0 pointing at its own framebuffer
(`0x7c720000`, pitch `0x1100` = 736 pixels) and the panel already initialised. XNU's console
writes `0x00rrggbb` and LK had per-pixel alpha on, which made those pixels transparent; the
loader turns alpha off and gives XNU's colour table `0xff` alpha besides.

## What was missing

Sending the frame once is not enough. The first kick showed the Apple logo and then froze:
the panel needs a new kick for every frame, and it must not be kicked while the previous
frame is still going out. The working arrangement:

- a **watcher thread on core 2** (four cores in total) writes, every 16 ms, but only if
  `0x1401400c` bit 31 (DSI busy) is clear:

  ```
  0x14001030 = 0x90380      MUTEX_MOD0
  0x14001020 = 1            MUTEX_EN
  0x1400102c = 0            MUTEX_SOF
  0x14014060 = 1            DSI_CMDQ_SIZE
  0x14014000 = 1            DSI_START
  ```

  with the command already in `0x14014000 + 0x...`'s queue (`DSI_CMDQ0 = 0x002c3909`,
  the `0x2C` "write memory start" short packet).
- the kernel's own per-frame kick is disabled: `mtk_kick=0` in
  `iokit/Kernel/MiniIOMobileFramebuffer.cpp`.

The DSI busy check was the piece that made the whole home screen appear; without it the
second frame was written over the first one's transfer and the panel stayed on the logo.

## Result

`xnu-a32-emmc-test28-busy.img` (four cores, `fbscale=3`) shows the full iOS 16.5 home screen:
clock, battery, widgets, icons, dock, centred and with correct colours.

## Cores

Six cores were tried and the phone ended in fastboot; eight leave no core for the watcher.
Four is the stable choice, and the watcher needs one of them.
