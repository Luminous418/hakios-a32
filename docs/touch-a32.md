# Touch on the A32: STM FTS5CU56A on I2C6

The chip is an **STMicroelectronics FTS5CU56A** on **I2C6 at address 0x49**, 15×33 nodes,
firmware `ST05120022`. Samsung's driver is `drivers/input/sec_input/stm/` in the Android
kernel (`stm_core.c`, `stm_i2c.c`, `stm_reg.h`), not the mainline `stmfts.c`: the commands and
the event layout below are its.

## The bus, by hand

There is no I2C framework in XNU, so the two pins are driven directly:

| | |
| --- | --- |
| SCL | GPIO89, function 0 (`GPIO_MODE11` at `0x10005000 + 0x3b0`, bits 7:4) |
| SDA | GPIO90, same register, bits 11:8 |
| pull-ups | `iocfg_0` at `0x10002000`: PU `+0x60` bits 23 and 25, PD `+0x50` the same bits |
| speed | about 100 kHz (4 µs per half period) |

Open drain by direction: a pin is pulled low by making it an output at 0 and let go by making
it an input again; the pull-up brings it back. Reads are one write of the command byte, a
repeated start, then the data — the shape `stm_ts_i2c_read()` asks the I2C core for.

## Power: two rails, and both are needed

| rail | what | how it is switched on |
| --- | --- | --- |
| 1.8 V I/O | `tsp_en_1p8_ldo` / `fixed_regulator0` | **GPIO4 high**, function 0, output |
| 3.0 V analog | `tsp_avdd_ldo` = MT6358 **VCAMA2** | pwrap at `0x1000d000`: `0x1c88` bit 0, VOSEL `0x1e0c` bits 11:8 = 12 |

This was found in two steps, and the log says it plainly:

```
fts: pins mode 11100002 pu 12800000 pd 20529a69; idle scl 1 sda 1
fts: no answer at 0x49 ... the address was not acknowledged      (before GPIO4)
fts: so far 0 events, 0 finger positions, 2 status events ...    (after GPIO4)
fts: vcama2 before: con0 0000 ana 0a00
fts: vcama2 after:  con0 0001 ana 0c00 (enabled 1, 3000 mV)      (then fingers arrive)
```

With only the I/O rail the chip ACKs and answers status commands, but the analog front end
sees nothing: zero coordinate events. Both rails on, the events appear.

## The protocol

- `0x60` read one event (16 bytes), `0x62` clear all, `0x10` sense on, `0x11` sense off.
- Event byte 0: bits 1:0 event id (0 coordinate, 1 status, 2 gesture, 3 vendor), bits 5:2
  finger, bits 7:6 action (1 press, 2 move, 3 release).
- `x = (buf[1] << 4) | (buf[3] >> 4)`, `y = (buf[2] << 4) | (buf[3] & 0xf)`, byte 7 bits 4:0
  how many more events are waiting.
- The panel's range comes from `0x23` (4095×4095 here); coordinates are scaled to the
  0..65535 that `hakios_touch_post()` takes.

The driver polls `0x60` every 8 ms (boot-arg `touch_poll_us=N`), drains the remaining events,
and posts `HAKIOS_TOUCH_DOWN/MOVE/UP`. `touch_swap=1`, `touch_flip_x=1` and `touch_flip_y=1`
turn the axes if needed.

## Result

`xnu-a32-emmc-test32-vcama2.img` (`touchscreen=3`, 4 cores) parses and posts real touches:

```
fts: so far 1603 events, 1578 finger positions, 1 status event in 16414 reads (0 failed)
```
