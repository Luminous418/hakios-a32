# The SCP sensor hub on the A32

The Galaxy A32 has the same MediaTek "tinysys" Cortex-M co-processor as the Infinix Hot 20:
`scp@10500000`, 512 KB of TCM, its configuration block at `0x105c0000`, clock control at
`0x105c4000`, and its own DRAM in the `SCP-reserved` region (`0x9f900000`, 6 MB). The
accelerometer, gyroscope, light and proximity sensors are behind it; the application processor
talks to the firmware through IPI (shared memory, the SCP mailbox, and nanohub messages).

The driver is `kernel/bsd/dev/hakios.scp.c`, the Infinix's `redwood_scp.c` ported to this
phone (same addresses). Boot-arg `scp_step=N`:

| N | what |
| --- | --- |
| 1 | read only: is the SCP out of reset, is LK's firmware in its TCM, what does its region info say |
| 2 | and started (one write to `cfg+0`, as Android's kernel does) |
| 3 | and its messages read |
| 4 | and an accelerometer request sent |
| 5 | and the accelerometer enabled through nanohub |
| 6 | and the hub given a DRAM FIFO first, then the accelerometer, and the samples read |
| 7 | the same in a thread that keeps taking the hub's messages (`kern.hakios.accel`) |

The DRAM FIFO is 256 KB at `0x4e000000`, inside the range the device tree gives the SCP's
shared memory (`0x40000000`–`0x90000000`) and clear of XNU, ATF, ramoops and the loader.

## Result (test44)

LK does load the firmware: the region info says loader `0x9f900000+0x438`, firmware
`0x9f900800+0x39c4c` (the `scp1` image is `tinysys-loader-CM4_A` + `tinysys-scp-CM4_A`,
~231 KB). What LK leaves unset is the DRAM size (`0xffffffff`); the driver writes
`0x9f980000+0x580000` (up to the end of the 6 MB `SCP-reserved` region) before starting it.

With that:

```
scp: step 1: DRAM size was unset; wrote 0x9f980000+0x580000
scp: step 2: topckgen CLK_CFG_0 0x20001 -> 0x20001 (scp_sel parent 0, on)
scp: step 2: 5 s after the start: PC 0x6890 ..., to host 0x1
scp: step 3: message id 12, 4 bytes: 0x80000        (ready, 512 KB of TCM)
scp: step 5: nanohub config sensor 1 enable 1 rate 51200: taken 1
scp: step 5: after the config: id 8, 33 bytes: 12000005 ...   (a stream of samples)
scp: step 6: 1 s: FIFO rp 0 wp 0 size 262108
```

The firmware runs, the accelerometer is enabled and this handset's firmware pushes its samples
through **IPI messages** (id 8, 33 bytes, a timestamp and a counter) rather than the DRAM FIFO:
the FIFO stays empty. The next step for rotation is therefore to parse those messages and hand
the samples to iOS, not to read the FIFO.

## The scp1 image (checked)

```
0x0000  0x58881688  0x438   "tinysys-loader-CM4_A"
0x14b0  0x58881688  0x39c4c "tinysys-scp-CM4_A"
```

so LK's copy is the whole sensor-hub firmware, not a stub.

## What is still missing for rotation

The Infinix's user-space orientation feed (`inject.c`, `orientation_start`) is not in the
public tree, and this repository does not have it either. With the kernel samples in
`kern.hakios.accel`, the next step is a service in `backboardd`/SpringBoard that turns them
into an orientation, so that the screen rotates. That is a user-space piece, not a kernel one.
