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

**Why not 270 pt at 1080?** That is a scale of 4, and iOS only carries artwork for 2x and 3x.
Measured (test97): with `fbscale=4`, `fbwidth=1080`, `fbheight=2400` the layout breaks -- the icons
draw at their 3x size on a 4x grid, rows overlap their labels, the wallpaper is cropped and the
status bar shows its test carrier capsule again. So 270 pt means drawing at 810x1800 and letting
the presenter scale by 1.33; the 1:1 path is only available at 375 pt or at 360 pt
(`fbwidth=1080 fbheight=2400 fbscale=3`).

**The filter matters more than it looked (test100).** With the drawable at 810x1800 the presenter
enlarges by 1.33 to the panel. Bilinear (`fb_filter=1`, the default) blends four source pixels per
panel pixel: measured 10 fps in a window of the home screen with the widgets up, 17-20 ms a frame
in the driver. With nearest neighbour (`fb_filter=0`) the same window reads **20 fps and 9 ms a
frame** -- the blending was the cost -- and the text is *sharper*, not softer: at a ratio above 1
each source pixel is written whole instead of being smeared into its neighbour. Measured on the
phone: "se nota mucho más suave, apenas hay stutters, y se ven mejor los textos". The widget page
still drops to 4 fps while it loads (RenderBox and the material passes), but the rest of the
system doubled.

So the A32's boot-args carry `fbwidth=810 fbheight=1800 fbscale=3 fb_filter=0`: 270 points, the
A22's interface size, one-to-one-and-a-third to the panel, nearest neighbour.

## The TE signal, measured (test102-103)

The tearing that is left is not a 90 Hz panel being written too slowly: it is a 60 Hz panel
being written at almost exactly the speed it scans.

- **Frame rate**: the TE pin's intervals are 11,040 us and 5,622 us, alternating: **16.66 ms, 60 Hz**.
- **Pulse**: the TE level is high for 5.6 ms of each frame, so there are **two edges per frame**
  and the old `te_sync` wait (any edge, 5 ms debounce) could start a frame up to 5.6 ms late --
  which is the intermittent tearing.
- **Writing time**: the DSI link runs at the vendor's 1660 Mbps per lane, four lanes (830 MB/s);
  a 1080x2400 frame is 7.8 MB, so **~9.4 ms** of transfer (11 measured). The panel's active scan
  of the same frame is about 11 ms of the 16.7 ms period. The write and the scan are therefore
  level, and the bottom of the panel is where the scan overtakes the write.
- **Syncing to the rising edge only** (`te_edge=1`) removes most of the tearing and makes the
  frame **much less responsive**: the wait is then up to a full 16.7 ms when the edge has just
  passed, and the phone reads as "everything takes much longer, changing pages gets stuck".
  Measured on the phone and rejected.

So the choice is the old one: any TE edge (lowest latency, small tearing) or a stricter sync
(more latency, less tearing). The physical fix would need the write to be faster than the active
scan (a higher DSI rate than the panel allows) or a double-buffered GRAM, which this panel's
vendor driver does not expose (command mode, one GRAM, no swap command).

## Brightness (test106-108)

The panel is a Samsung s6e3fc3 in command mode; brightness is DCS **0x51** with two bytes (10 bits,
high two first), and it only takes effect in the panel's normal mode:

- **0x53 0x20** -- write_control_display_brightness, normal (the vendor's other values are 0xE8 for
  HBM, 0x28 HBM off, 0x20/0xE0 "dimming off");
- **0x7A 0x00** -- self-mask off. The vendor's init writes an image into the self-mask and enables
  it, and with it on the 0x51 value is ignored (measured: 0x51 alone changed nothing, twice);
- **0x51 hi lo** -- the level, 0..1023.

The packets are written exactly as the vendor's `mtk_dsi_cmdq` writes them: a long packet (0x39)
has the byte count in bits 16-23 of the CMDQ header word, the payload follows in the next word;
a short write with one parameter is 0x15 with the command and parameter in bytes 2 and 3. Both
forms were proven on this panel first with **0x28 (display off)**, which turned the screen black
as a short write and again as a long write.

In this port:

- boot-arg `brightness=N` (0..1023) sets it once, at the first frame;
- `kern.hakios.brightness` (sysctl, writable by anybody) is applied by the presenter on its next
  frame, after the DSI is idle: that is the channel a user-space control would use;
- the vendor's brightness *tables* (dim/HBM/ACL, per level and per panel revision) are not ported;
  0x53 0x20 + 0x51 is what works, measured at 20/1023 (the screen went almost black).

**The iOS slider does not reach it (test108).** Settings and Control Center move the slider, but
nothing arrives: no `com.apple.backboard.displaybrightness` notification from Preferences (its
binary contains that name), and SpringBoard's `-[SBDisplayBrightnessController
_setBrightnessLevel:animated:]` is not called either (the hooks were in place). The notification
*chain* itself works: a self test that posts the notification with state 0 from SpringBoard dims
the panel to black -- through backboardd's listener, `kern.hakios.brightness`, and the presenter.
backboardd must register *after* notifyd is up (it starts long before it), so the registration is
retried every two seconds.

Left as it boots: no brightness boot-arg, the panel keeps the bootloader's level, and the sysctl
is there for a control that can reach it later.
