/*
 * The buttons of the Samsung Galaxy A32 4G (SM-A325F), as sources of HAKIOS_BUTTON_* samples for
 * /dev/hakios-touch (sys/hakios.input.h). Only with boot-arg buttons=2: the addresses belong to
 * this phone, and buttons=1 is the Qualcomm driver beside this one.
 *
 * Where they are is in the device tree's keypad node (kp@10010000):
 *
 *   volume up    the SoC's keypad controller, hardware key 1, which its map (kpd-hw-init-map,
 *                0x73 at index 1) makes KEY_VOLUMEUP
 *   volume down  the PMIC's homekey: Android's keypad driver reports the homekey interrupt as
 *                KEY_VOLUMEDOWN (kpd-sw-rstkey 0x72; see mt6358/v1/pmic_irq.c and
 *                hal_kpd.c's kpd_pmic_rstkey_hal), bit 3 of TOPSTATUS
 *   power        the PMIC, an MT6358, reached through the PMIC wrapper: bit 1 of TOPSTATUS
 *
 * The keypad keeps a bit per key in KP_MEM1..5, clear while the key is down; the PMIC's bit is
 * the same way round (Linux drivers/input/keyboard/mediatek/kpd.c and mtk-pmic-keys.c). Nothing
 * interrupts: a thread looks every 20 ms, and a key has to read the same twice in a row before
 * it counts.
 *
 * Boot-arg keylog=1: a line for every change, to check the wiring against.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define KPD_PHYS        0x10010000
#define KP_MEM1         0x0004          /* keys 0..15, a bit clear while the key is down */
#define KP_MEM2         0x0008          /* keys 16..31 */

#define PWRAP_PHYS      0x1000d000
#define PWRAP_WACS2_CMD     0xc20
#define PWRAP_WACS2_RDATA   0xc24
#define PWRAP_WACS2_VLDCLR  0xc28
#define PWRAP_FSM(r)        (((r) >> 16) & 7u)
#define PWRAP_FSM_IDLE      0
#define PWRAP_FSM_WFVLDCLR  6

#define MT6358_TOPSTATUS    0x28
#define PWRKEY_DEB          (1u << 1)   /* clear while the power key is down */
#define HOMEKEY_DEB         (1u << 3)   /* clear while the volume down key is down */

#define KEY_VOLUME_UP_HW    1           /* hardware key 1 */

static vm_offset_t kpd, pwrap;

static inline uint32_t
rd(vm_offset_t base, uint32_t at)
{
	return *(volatile uint32_t *)(base + at);
}

static inline void
wr(vm_offset_t base, uint32_t at, uint32_t value)
{
	*(volatile uint32_t *)(base + at) = value;
}

static bool
pwrap_wait(uint32_t state)
{
	for (uint32_t spin = 0; spin < 100000; spin++) {
		if (PWRAP_FSM(rd(pwrap, PWRAP_WACS2_RDATA)) == state) {
			return true;
		}
	}
	return false;
}

static bool
pmic_read(uint32_t reg, uint32_t *value)
{
	if (!pwrap_wait(PWRAP_FSM_IDLE)) {
		return false;
	}
	wr(pwrap, PWRAP_WACS2_CMD, (reg >> 1) << 16);
	if (!pwrap_wait(PWRAP_FSM_WFVLDCLR)) {
		return false;
	}
	*value = rd(pwrap, PWRAP_WACS2_RDATA) & 0xffff;
	wr(pwrap, PWRAP_WACS2_VLDCLR, 1);
	return true;
}

struct key {
	const char *name;
	uint32_t button;                /* HAKIOS_BUTTON_* */
	bool down, seen;                /* what was reported, and what the last look found */
};

static struct key keys[3] = {
	{ "volume up", HAKIOS_BUTTON_VOLUME_UP, false, false },
	{ "volume down", HAKIOS_BUTTON_VOLUME_DOWN, false, false },
	{ "power", HAKIOS_BUTTON_LOCK, false, false },
};

static void
mtkkeys_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t log = 0, presses = 0, raw = 0, last_top = ~0u, last_raw[5] = { 0, 0, 0, 0, 0 };

	PE_parse_boot_argn("keylog", &log, sizeof(log));
	PE_parse_boot_argn("keyraw", &raw, sizeof(raw));
	kpd = ml_io_map(KPD_PHYS, 0x1000);
	pwrap = ml_io_map(PWRAP_PHYS, 0x1000);
	printf("mtkkeys: the buttons are looked at every 20 ms; keypad %#x %#x, PMIC status %s\n",
	    rd(kpd, KP_MEM1), rd(kpd, KP_MEM2), pwrap_wait(PWRAP_FSM_IDLE) ? "reachable" : "NOT REACHABLE");

	for (unsigned tick = 1;; tick++) {
		uint32_t mem1 = rd(kpd, KP_MEM1), mem2 = rd(kpd, KP_MEM2), top = PWRKEY_DEB;
		uint32_t mem[5];
		bool now[3];

		for (unsigned i = 0; i < 5; i++) {
			mem[i] = rd(kpd, KP_MEM1 + i * 4);
		}
		(void)pmic_read(MT6358_TOPSTATUS, &top);
		if (raw && (top != last_top || memcmp(mem, last_raw, sizeof(mem)) != 0)) {
			printf("mtkkeys: raw keypad %#x %#x %#x %#x %#x, PMIC %#x\n",
			    mem[0], mem[1], mem[2], mem[3], mem[4], top);
		}
		last_top = top;
		memcpy(last_raw, mem, sizeof(last_raw));
		now[0] = !(mem1 & (1u << KEY_VOLUME_UP_HW));
		now[1] = !(top & HOMEKEY_DEB);
		now[2] = !(top & PWRKEY_DEB);

		for (unsigned i = 0; i < 3; i++) {
			struct key *k = &keys[i];

			/* the same twice in a row, or it is a bounce */
			if (now[i] == k->seen && now[i] != k->down) {
				k->down = now[i];
				hakios_touch_post(k->down ? HAKIOS_BUTTON_DOWN : HAKIOS_BUTTON_UP, k->button, 0, 0);
				presses += k->down;
				if (log) {
					printf("mtkkeys: %s %s (keypad %#x %#x, PMIC %#x)\n", k->name,
					    k->down ? "down" : "up", mem1, mem2, top);
				}
			}
			k->seen = now[i];
		}

		if (tick % 3000 == 0) {         /* once a minute */
			printf("mtkkeys: %u presses so far; keypad %#x %#x, PMIC %#x\n", presses, mem1, mem2, top);
		}
		delay_for_interval(20, NSEC_PER_MSEC);
	}
}

void
hakios_mtkkeys_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("buttons", &enabled, sizeof(enabled)) || enabled != 2) {
		return;
	}
	if (kernel_thread_start(mtkkeys_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
