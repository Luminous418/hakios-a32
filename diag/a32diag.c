/*
 * a32diag: first bare-metal probe for the Galaxy A32 4G (SM-A325F, MT6769T).
 *
 * Packed in front of Android's kernel in the `boot` partition by tools/chainboot.py. LK
 * decompresses it to 0x40080000 and starts it with MMU/caches off. It:
 *
 *   - reads LK's framebuffer address/pitch from the OVL0 registers and writes a few bars and
 *     text there (the A32 panel is an S6E3FC3 in DSI command mode, so the splash may not update
 *     until a real refresh; the ramoops log below is the reliable result),
 *   - writes the same report into the ramoops console zone (0x4d09f000), which Android exposes
 *     as /sys/fs/pstore/console-ramoops-0 after the next boot,
 *   - copies Android's own kernel Image down to 0x40080000 and jumps to it, exactly as LK
 *     would have done, so the phone comes back to Android.
 *
 * No watchdog, no pattern probes, no writes outside LK's framebuffer and the ramoops console.
 */
#include <stdint.h>
#include "mtkdbg.h"

#define CHAIN_OFFSET  0x40000u
#define KERNEL_PA     0x40110000ULL
#define STUB_PA       0x4a000000ULL

extern char _start[];
extern char chain_stub[], chain_stub_end[];
static uint64_t chain_dtb;

static void __attribute__((noreturn))
chain_android(void)
{
	const volatile uint32_t *img = (const volatile uint32_t *)((uint64_t)_start + CHAIN_OFFSET);
	uint64_t image_size;

	if (img[14] != 0x644d5241u) { /* "ARM\x64" */
		dbg_puts("a32diag: no Linux Image to chain to; waiting forever\n");
		for (;;) {
			__asm__ volatile("wfe");
		}
	}

	const volatile uint32_t *hdr = (const volatile uint32_t *)_start;
	image_size = ((uint64_t)hdr[11] << 32 | hdr[10]);
	image_size = (image_size + 7) & ~7ULL;
	dbg_puts("a32diag: chaining to Android, ");
	dbg_dec(image_size);
	dbg_puts(" bytes\n");

	volatile uint32_t *dst = (volatile uint32_t *)STUB_PA;
	const volatile uint32_t *src = (const volatile uint32_t *)chain_stub;
	for (uint64_t i = 0; i < (uint64_t)(chain_stub_end - chain_stub) / 4; i++) {
		dst[i] = src[i];
	}
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	void (*stub)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) = (void *)STUB_PA;
	stub(chain_dtb, KERNEL_PA, (uint64_t)img, image_size, KERNEL_PA);
	__builtin_unreachable();
}

void
fbdiag_fault(uint64_t vec, uint64_t esr, uint64_t elr, uint64_t far)
{
	fbc.fg = 0xffff4040u;
	dbg_puts("\na32diag: FAULT vec=");
	dbg_hex(vec, 3);
	dbg_puts(" ESR=");
	dbg_hex(esr, 8);
	dbg_puts(" ELR=");
	dbg_hex(elr, 10);
	dbg_puts(" FAR=");
	dbg_hex(far, 10);
	dbg_puts("\n");
	dbg_delay_ms(3000);
	chain_android();
}

static void
bar(uint32_t y, uint32_t color, const char *label)
{
	fb_rect(0, y, PANEL_W, 48, color);
	uint32_t saved_y = fbc.y, saved_x = fbc.x, saved_bg = fbc.bg, saved_fg = fbc.fg;

	fbc.x = 8;
	fbc.y = y + 8;
	fbc.bg = 0xff000000u;
	fbc.fg = 0xffffffffu;
	while (*label) {
		fbcon_putc(*label++);
	}
	fbc.x = saved_x;
	fbc.y = saved_y;
	fbc.bg = saved_bg;
	fbc.fg = saved_fg;
}

void
fbdiag_main(uint64_t dtb)
{
	uint32_t l0_addr, l0_pitch, l0_con;
	uint64_t fb = LK_FB_BASE;
	uint32_t pitch = LK_FB_PITCH;

	chain_dtb = dtb;
	wdt_disable();
	plog_reset();

	l0_addr = rd32(OVL0_BASE + OVL_L0_ADDR);
	l0_pitch = rd32(OVL0_BASE + OVL_L0_PITCH) & 0xffff;
	l0_con = rd32(OVL0_BASE + OVL_L0_CON);
	if (l0_addr >= LK_FB_BASE && l0_addr < LK_FB_BASE + LK_FB_SIZE &&
	    l0_pitch >= PANEL_W * 4 && l0_pitch <= 8192) {
		fb = l0_addr;
		pitch = l0_pitch;
	}

	fbcon_init(fb, pitch, 0, 1);
	fbc.scale = 2;
	fbc.x = 0;
	fbc.y = 0;
	fbc.fg = 0xffffffffu;
	fbc.bg = 0xff000000u;

	fb_rect(0, 0, PANEL_W, 420, 0xff000000u);
	bar(0, 0xffff0000u, "A32DIAG: red");
	bar(56, 0xff00ff00u, "A32DIAG: green");
	bar(112, 0xff0000ffu, "A32DIAG: blue");
	bar(168, 0xffffffffu, "A32DIAG: white");

	fbc.scale = 2;
	fbc.x = 0;
	fbc.y = 240;
	fbc.fg = 0xff40ff40u;
	dbg_puts("A32DIAG: Galaxy A32 4G (SM-A325F, MT6769T)\n");
	dbg_puts("dtb=");
	dbg_hex(dtb, 10);
	dbg_puts(" fb=");
	dbg_hex(fb, 10);
	dbg_puts(" pitch=");
	dbg_dec(pitch);
	dbg_puts(" panel=1080x2400\n");
	dbg_puts("OVL0 L0_ADDR=");
	dbg_hex(l0_addr, 8);
	dbg_puts(" L0_PITCH=");
	dbg_hex(l0_pitch, 4);
	dbg_puts(" L0_CON=");
	dbg_hex(l0_con, 8);
	dbg_puts("\n");
	dbg_puts("EL=");
	uint64_t el;
	__asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
	dbg_dec((el >> 2) & 3);
	dbg_puts(" CNTFRQ=");
	dbg_dec(dbg_freq());
	dbg_puts(" TOPRGU_MODE=");
	dbg_hex(rd32(TOPRGU_MODE), 8);
	dbg_puts("\n");
	dbg_puts("Android in 10 s; log will be in pstore\n");

	for (int s = 10; s > 0; s--) {
		dbg_puts("a32diag: ");
		dbg_dec(s);
		dbg_puts("\n");
		dbg_delay_ms(1000);
	}
	chain_android();
}
