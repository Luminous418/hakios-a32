/*
 * mtkloader entry. Samsung's LK decompresses this where it likes and jumps to the first byte;
 * entry_relocate copies the whole blob to the address the loader was linked for (0x40080000).
 * MMU and caches are off, x0 = LK's device tree. All memory is device memory until XNU turns its
 * MMU on, so the C code is built without SIMD and with strict alignment (build.sh).
 */
.global _start
.global jump_to_kernel
.global chain_stub
.global chain_stub_end
.global xnu_return_stub
.global psci_system_reset
.global psci_cpu_on
.global watcher_entry
.global mmu_on
.global dcache_clean_range
.global irq_wait
.section .text.boot

_start:
    /* Linux ARM64 Image header (64 bytes); tools/chainboot.py fills image_size, res2, res3 */
    b       entry_relocate      /* code0 */
    .word   0                   /* code1 */
    .quad   0x80000             /* text_offset */
    .quad   0x02000000          /* image_size */
    .quad   0x0a                /* flags: little-endian, 4K pages, anywhere */
    .quad   0                   /* res2: unused */
    .quad   0                   /* res3: length of Android's Image (at CHAIN_OFFSET) */
    .quad   0                   /* res4: offset of the XNU payload ("XNUP") from _start */
    .ascii  "ARM\x64"
    .word   0

/*
 * Samsung's LK does not use the address the Image was linked for: it decompresses the kernel
 * wherever its allocator says and jumps to the first byte of the Image. So the loader must
 * move itself to the address its symbols were linked for (0x40080000) before any C runs.
 *
 * LK still hands us its device tree in x0. Everything here is PC-relative until the jump.
 */
#define RELOC_STUB_PA 0x4a000000

entry_relocate:
    msr     daifset, #0xf
    mov     x19, x0                 /* LK's device tree */

    /* Put the copy loop where the copy cannot overwrite it. */
    adr     x0, reloc_stub
    ldr     x1, =RELOC_STUB_PA
    ldr     x2, =reloc_stub_end - reloc_stub
1:  ldr     x6, [x0], #8
    str     x6, [x1], #8
    subs    x2, x2, #8
    b.hi    1b
    dsb     sy
    ic      iallu
    dsb     sy
    isb

    /*
     * How much to move: the whole decompressed blob (loader + padding + Android's Image,
     * plus the XNU payload when there is one). With no payload the exact blob is
     * CHAIN_OFFSET + res2; with one, image_size covers it and is rounded up to 1 MiB.
     */
    adr     x20, _start             /* where LK actually put us */
    ldr     x2, [x20, #0x10]        /* image_size */
    ldr     x3, [x20, #0x28]        /* length of Android's Image */
    add     x3, x3, #0x40000        /* CHAIN_OFFSET */
    add     x3, x3, #7
    and     x3, x3, #~7
    ldr     x6, [x20, #0x30]        /* offset of the XNU payload, 0 when none */
    cbnz    x6, 4f
    mov     x2, x3
    b       5f
4:  cmp     x2, x3
    csel    x2, x2, x3, hi
5:

    mov     x0, x20                 /* src */
    ldr     x1, =0x40080000         /* dst: the address the loader was linked for */
    ldr     x3, =primary_entry      /* entry once it is there */
    mov     x4, x19                 /* LK's dtb */
    ldr     x5, =RELOC_STUB_PA
    br      x5

/* Runs from RELOC_STUB_PA: no stack, no absolute symbol references. */
.balign 16
reloc_stub:
    /* A 35 MiB copy with the MMU off takes long enough to trip LK's watchdog. */
    ldr     x9, =0x10007000
    ldr     w10, =0x1971
    str     w10, [x9, #0x08]
    ldr     w10, =0x22000000
    str     w10, [x9, #0x00]
    dsb     sy

    cbz     x2, 3f
    cmp     x0, x1
    b.eq    3f
    b.lo    2f                      /* src < dst: copy backwards */
1:  ldr     x6, [x0], #8
    str     x6, [x1], #8
    subs    x2, x2, #8
    b.hi    1b
    b       3f
2:  add     x0, x0, x2
    add     x1, x1, x2
1:  ldr     x6, [x0, #-8]!
    str     x6, [x1, #-8]!
    subs    x2, x2, #8
    b.hi    1b
3:  dsb     sy
    ic      iallu
    dsb     sy
    isb
    mov     x0, x4                  /* LK's dtb */
    br      x3
    .ltorg
.balign 8
reloc_stub_end:

primary_entry:
    msr     daifset, #0xf
    mov     x19, x0

    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    adrp    x2, mtk_vectors
    add     x2, x2, :lo12:mtk_vectors
    cmp     x1, #2
    b.ne    10f
    msr     vbar_el2, x2
    b       20f
10: msr     vbar_el1, x2
20:
    /* Stack inside our own BSS: LK may have loaded us anywhere. */
    ldr     x0, =__stack_top
    mov     sp, x0
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
1:  cmp     x0, x1
    b.ge    2f
    str     xzr, [x0], #8
    b       1b
2:
    mov     x0, x19
    bl      mtkloader_main
3:  wfe
    b       3b

/* jump_to_kernel(entry_pa, boot_args_pa): enter XNU at EL1h, MMU and caches off */
jump_to_kernel:
    msr     daifset, #0xf
    mov     x16, x0
    mov     x0, x1
    mrs     x2, CurrentEL
    lsr     x2, x2, #2
    cmp     x2, #2
    b.ne    1f
    mov     x2, #(1 << 31)      /* HCR_EL2.RW: EL1 is AArch64 */
    msr     hcr_el2, x2
    mov     x2, #0x3c5          /* EL1h, DAIF masked */
    msr     spsr_el2, x2
    msr     elr_el2, x16
    eret
1:  mrs     x2, sctlr_el1
    bic     x2, x2, #0x1
    bic     x2, x2, #0x4
    bic     x2, x2, #0x1000
    msr     sctlr_el1, x2
    isb
    ic      iallu
    dsb     sy
    isb
    br      x16

/*
 * chain_stub(dtb, dst, src, len, entry): copy Android's Image down over this program and enter
 * it as LK would have. C copies the stub out of the way first. Position independent.
 */
chain_stub:
1:  ldr     x6, [x2], #8
    str     x6, [x1], #8
    subs    x3, x3, #8
    b.hi    1b
    dsb     sy
    ic      iallu
    dsb     sy
    isb
    mov     x1, xzr
    mov     x2, xzr
    mov     x3, xzr
    br      x4
chain_stub_end:

/*
 * XNU comes back here (loader_stub_slot[1], see xnu/files/osfmk/arm64/loader_return.c): after a
 * panic or an orderly end with the MMU on and this page mapped 1:1, or from its early exception
 * vector with the MMU off. x0 = log PA, x1 = log length, x2 = reason (0 end, 1 early exception,
 * 2 checkpoint), x3 = code (ESR for an exception), x4 = ELR, x5 = FAR.
 */
xnu_return_stub:
    msr     daifset, #0xf
    mov     x19, x0
    mov     x20, x1
    mov     x21, x2
    mov     x22, x3
    mov     x23, x4
    mov     x24, x5
    mov     x25, x29
    mov     x26, x30
    /* What XNU has only in its caches (its stack, its globals) goes to RAM before they go off */
    bl      dcache_clean_all
    mrs     x9, sctlr_el1
    bic     x9, x9, #0x1
    bic     x9, x9, #0x4
    bic     x9, x9, #0x1000
    msr     sctlr_el1, x9
    isb
    ic      iallu
    tlbi    vmalle1
    dsb     sy
    isb
    adrp    x9, mtk_vectors
    add     x9, x9, :lo12:mtk_vectors
    msr     vbar_el1, x9
    adrp    x9, xnu_fp
    add     x9, x9, :lo12:xnu_fp
    str     x25, [x9]            /* XNU's frame pointer and lr: mtkloader.c walks its stack */
    str     x26, [x9, #8]
    mrs     x10, tcr_el1
    str     x10, [x9, #16]
    mrs     x10, ttbr1_el1
    str     x10, [x9, #24]
    ldr     x9, =__stack_top
    mov     sp, x9
    mov     x0, x19
    mov     x1, x20
    mov     x2, x21
    mov     x3, x22
    mov     x4, x23
    mov     x5, x24
    bl      xnu_returned
1:  wfe
    b       1b

/* Clean and invalidate every data/unified cache level up to the point of coherency, by set/way. */
dcache_clean_all:
    mrs     x0, clidr_el1
    and     w3, w0, #0x07000000
    lsr     w3, w3, #23          /* LoC * 2 */
    cbz     w3, 5f
    mov     w10, #0              /* level * 2 */
1:  add     w2, w10, w10, lsr #1 /* level * 3 */
    lsr     w1, w0, w2
    and     w1, w1, #7
    cmp     w1, #2
    b.lt    4f                   /* no data cache at this level */
    msr     csselr_el1, x10
    isb
    mrs     x1, ccsidr_el1
    and     w2, w1, #7
    add     w2, w2, #4           /* log2(line size) */
    ubfx    w4, w1, #3, #10      /* ways - 1 */
    clz     w5, w4
    ubfx    w7, w1, #13, #15     /* sets - 1 */
2:  mov     w9, w4
3:  lsl     w6, w9, w5
    orr     w11, w10, w6
    lsl     w6, w7, w2
    orr     w11, w11, w6
    dc      cisw, x11
    subs    w9, w9, #1
    b.ge    3b
    subs    w7, w7, #1
    b.ge    2b
4:  add     w10, w10, #2
    cmp     w3, w10
    b.gt    1b
5:  dsb     sy
    isb
    ret

/* PSCI SYSTEM_RESET through ATF, which keeps DRAM refreshed (unlike a bare watchdog reset). */
psci_system_reset:
    ldr     x0, =0x84000009
    smc     #0
    ret

/* psci_cpu_on(mpidr, entry_pa, context): 0 on success */
psci_cpu_on:
    mov     x3, x2
    mov     x2, x1
    mov     x1, x0
    ldr     x0, =0xc4000003
    smc     #0
    ret

/* Second core, started by PSCI CPU_ON while XNU runs on the first: MMU and caches off. */
watcher_entry:
    msr     daifset, #0xf
    adrp    x9, mtk_vectors
    add     x9, x9, :lo12:mtk_vectors
    msr     vbar_el1, x9
    ldr     x9, =__watch_stack_top
    mov     sp, x9
    bl      watcher_main
1:  wfe
    b       1b

/*
 * mmu_on(l1_table_pa): 4K granule, 39-bit addresses from one level-1 table of 1 GB blocks,
 * MAIR attr 0 device nGnRnE, attr 1 normal write-back. Inner shareable, so this core's
 * cached reads see what the other cores have in their caches.
 */
mmu_on:
    ldr     x1, =0xff00
    msr     mair_el1, x1
    ldr     x1, =0x200803519     /* IPS 40 bit, EPD1, SH0 inner, ORGN0/IRGN0 WB, T0SZ 25 */
    msr     tcr_el1, x1
    msr     ttbr0_el1, x0
    isb
    tlbi    vmalle1
    dsb     sy
    isb
    mrs     x1, sctlr_el1
    orr     x1, x1, #0x1
    orr     x1, x1, #0x4
    orr     x1, x1, #0x1000
    msr     sctlr_el1, x1
    isb
    ret

/* dcache_clean_range(va, len): clean and invalidate to the point of coherency */
dcache_clean_range:
    add     x1, x0, x1
    bic     x0, x0, #63
1:  dc      civac, x0
    add     x0, x0, #64
    cmp     x0, x1
    b.lo    1b
    dsb     sy
    ret

/*
 * irq_wait(ticks): unmask IRQs and wait, so a timer interrupt can be taken; returns the number
 * of interrupts the handler saw. irq_count and irq_intid are C globals (mtkloader.c).
 */
irq_wait:
    mov     x19, x0
    msr     daifclr, #0x3        /* IRQ and FIQ on */
    isb
1:  isb
    mrs     x0, cntpct_el0
    cmp     x0, x19
    b.lo    1b
    msr     daifset, #0xf
    isb
    adrp    x0, irq_count
    add     x0, x0, :lo12:irq_count
    ldr     x0, [x0]
    ret

/* IRQ at EL1: take the interrupt, remember its number, silence both timers, end it. */
irq_entry:
    sub     sp, sp, #0x60
    stp     x0, x1, [sp, #0x00]
    stp     x2, x3, [sp, #0x10]
    stp     x4, x5, [sp, #0x20]
    mrs     x0, ICC_IAR1_EL1
    adrp    x1, irq_count
    add     x1, x1, :lo12:irq_count
    ldr     x2, [x1]
    add     x2, x2, #1
    str     x2, [x1]
    adrp    x1, irq_intid
    add     x1, x1, :lo12:irq_intid
    str     x0, [x1]
    mov     x3, #2               /* IMASK: stop the timer asking */
    msr     cntv_ctl_el0, x3
    msr     cntp_ctl_el0, x3
    isb
    cmp     x0, #1020
    b.hs    1f                   /* 1023 = spurious: nothing to end */
    msr     ICC_EOIR1_EL1, x0
    isb
1:  ldp     x0, x1, [sp, #0x00]
    ldp     x2, x3, [sp, #0x10]
    ldp     x4, x5, [sp, #0x20]
    add     sp, sp, #0x60
    eret

/* Any exception in the loader: ESR/ELR/FAR and the vector offset to C (loader_fault). */
.macro vec off
    .balign 0x80
    mov     x0, #\off
    b       fault_common
.endm

fault_common:
    mrs     x4, CurrentEL
    lsr     x4, x4, #2
    cmp     x4, #2
    b.ne    1f
    mrs     x1, esr_el2
    mrs     x2, elr_el2
    mrs     x3, far_el2
    b       2f
1:  mrs     x1, esr_el1
    mrs     x2, elr_el1
    mrs     x3, far_el1
2:  ldr     x5, =__fault_stack_top
    mov     sp, x5
    bl      loader_fault
3:  wfe
    b       3b

    .balign 0x800
mtk_vectors:
    vec 0x000
    .balign 0x80
    b       irq_entry
    vec 0x100
    vec 0x180
    vec 0x200
    .balign 0x80
    b       irq_entry
    vec 0x300
    vec 0x380
    vec 0x400
    vec 0x480
    vec 0x500
    vec 0x580
    vec 0x600
    vec 0x680
    vec 0x700
    vec 0x780
