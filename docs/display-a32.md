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

## The size of the interface: points, not pixels (test93-96)

iOS lays the interface out in **points**, and a point is not the panel's pixel: it is
`drawable_width / fbscale`. The framebuffer this kernel reports defaults to the iPhone X's
1125 x 2436 at three pixels a point, so the home screen is 375 x 812 points, drawn at 1125 x 2436
and scaled to the 1080 x 2400 panel (bilinear, `fb_filter`).

`fbwidth=` and `fbheight=` change the drawable, and with it the number of points:

| drawable | points | where |
|---|---|---|
| 1125 x 2436 (default) | 375 x 812 | iPhone X's own size; icons and text look small on a 6.4" panel |
| 810 x 1800 | **270 x 600** | the A32's choice: everything bigger, status bar fits |
| 720 x 1600 | 240 x 533 | what a Galaxy A22 4G gets (its panel is 720 wide), and the point where the A32's status bar breaks |

Measured on the phone:

- **375 pt** is what the port had; correct but small for the panel.
- **240 pt** (720 x 1600) is comfortable but **the status bar disappears**: this iOS 16.5 image is
  an internal build, and with less room the left of the bar shows its test carrier capsule, an
  orange pill with a dot and `rdar:45025538` (a radar number set as the carrier string), which
  takes the whole bar and pushes the clock and the battery out. The same happens on the A22.
- **270 pt** (810 x 1800) fits: clock and battery normal, icons and widgets at the size the A22
  shows, and the drawable is **1.9 times fewer pixels** than 1125 x 2436, so the window server
  has less to draw and the presenter scales 810 x 1800 to the panel by 1.33 (slightly softer
  than native, sharper than the A22's 720).

`fbscale` is an unsigned integer (2 or 3 to a point); the size is chosen with `fbwidth`/`fbheight`.
The wallpaper's strips are sized from the view's bounds and follow the change without edits.

