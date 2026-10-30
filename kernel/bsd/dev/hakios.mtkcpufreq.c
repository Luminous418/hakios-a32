/*
 * The processors' clock on the Samsung Galaxy A32 4G (MT6769T), ported from the Infinix Hot 20's
 * driver (MT6768, the same two clusters and the same MT6358 PMIC). Only with boot-arg
 * mtk_cpufreq=N: every address below belongs to this phone.
 *
 * Android's driver for this SoC (drivers/misc/mediatek/base/power/cpufreq_v1, mach/mt6768) has
 * the table: the Cortex-A55s (ARMPLL1, Vproc12) to 1700 MHz at 0.9625 V, the Cortex-A75s
 * (ARMPLL2, Vproc11) to 2000 MHz at 1.0875 V, from the MT6358's bucks, 0.5 V + 6.25 mV a step.
 * The order is its own: the voltage up first (VSRAM before Vproc, so that it never falls under
 * it), then the PLL -- its new DDS with the CHG bit, then the post divider. Nothing is touched
 * unless every register reads what the boot loader is known to leave there.
 *
 *   mtk_cpufreq=1   read only: say what the boot loader left
 *   mtk_cpufreq=2   the A75s to 2000 MHz
 *   mtk_cpufreq=3   the A75s and then the A55s (1700 MHz)
 *
 * From the Infinix's bsd/dev/redwood_mtkkeys.c (where it lives beside its buttons).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

#define APMIXED_PHYS            0x1000c000
#define ARMPLL_LL_CON1          0x21c   /* ARMPLL1: the A55s */
#define ARMPLL_L_CON1           0x20c   /* ARMPLL2: the A75s */

#define PWRAP_PHYS              0x1000d000
#define PWRAP_WACS2_CMD         0xc20
#define PWRAP_WACS2_RDATA       0xc24
#define PWRAP_WACS2_VLDCLR      0xc28
#define PWRAP_FSM(r)            (((r) >> 16) & 7u)
#define PWRAP_FSM_IDLE          0
#define PWRAP_FSM_WFVLDCLR      6

#define MT6358_VPROC11_DBG0     0x139e  /* what the buck is set to (7 bits) */
#define MT6358_VPROC11_ELR0     0x13a6  /* where it is set */
#define MT6358_VPROC12_DBG0     0x141e
#define MT6358_VPROC12_ELR0     0x1426
#define MT6358_VSRAM11_TRACK    0x1b64
#define MT6358_VSRAM12_TRACK    0x1b6c
#define MT6358_VSRAM11_DBG0     0x1b60  /* bits 14:8 */
#define MT6358_VSRAM12_DBG0     0x1ba2
#define MT6358_VSRAM_CON0       0x1bee  /* VSRAM_PROC11's setting */
#define MT6358_VSRAM_CON1       0x1bf0  /* VSRAM_PROC12's */
#define VOLT_STEP_MAX           99      /* 1.11875 V: Android's MAX_VPROC_VOLT, 1.12 V */

static vm_offset_t apmixed, pwrap;

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

/* The PMIC wrapper's own channel: idle, command, wait for the answer, take it. */
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

static bool
pmic_write(uint32_t reg, uint32_t value)
{
	if (!pwrap_wait(PWRAP_FSM_IDLE)) {
		return false;
	}
	wr(pwrap, PWRAP_WACS2_CMD, (1u << 31) | ((reg >> 1) << 16) | (value & 0xffff));
	return pwrap_wait(PWRAP_FSM_IDLE);
}

static uint32_t
pll_mhz(uint32_t con1)
{
	uint32_t posdiv = 1u << ((con1 >> 24) & 7);

	return (uint32_t)((uint64_t)(con1 & 0x3fffff) * 26 / (1u << 14) / posdiv);
}

static uint32_t
pll_dds(uint32_t vco_mhz)
{
	return ((vco_mhz << 14) / 26) & 0x3fffff;
}

static void
report_cluster(const char *name, uint32_t con1_at, uint32_t dbg, uint32_t vsram_dbg)
{
	uint32_t con1 = rd(apmixed, con1_at), vproc = 0, vsram = 0;

	(void)pmic_read(dbg, &vproc);
	(void)pmic_read(vsram_dbg, &vsram);
	printf("cpufreq: %s at %u MHz (CON1 %#x), Vproc step %u (%u mV), VSRAM step %u (%u mV)\n",
	    name, pll_mhz(con1), con1, vproc & 0x7f, 500 + (vproc & 0x7f) * 25 / 4,
	    (vsram >> 8) & 0x7f, 500 + ((vsram >> 8) & 0x7f) * 25 / 4);
}

static bool
cluster_raise(const char *name, uint32_t con1_at, uint32_t elr, uint32_t dbg,
    uint32_t vsram_con, uint32_t vsram_dbg, uint32_t track, uint32_t mhz, uint32_t posdiv_sel, uint32_t step)
{
	uint32_t con1 = rd(apmixed, con1_at), vproc = 0, vsram = 0, tracking = 0, elr_was = 0;

	if (!pmic_read(dbg, &vproc) || !pmic_read(vsram_dbg, &vsram) || !pmic_read(track, &tracking) ||
	    !pmic_read(elr, &elr_was)) {
		printf("cpufreq: %s: the PMIC does not answer; left as it is\n", name);
		return false;
	}
	vproc &= 0x7f;
	vsram = (vsram >> 8) & 0x7f;
	printf("cpufreq: %s at %u MHz (CON1 %#x), Vproc step %u (%u mV), VSRAM step %u (%u mV), tracking %#x\n",
	    name, pll_mhz(con1), con1, vproc, 500 + vproc * 25 / 4, vsram, 500 + vsram * 25 / 4, tracking);
	if (step == 0) {
		return true;
	}
	/* what the boot loader leaves: 1175 MHz from a VCO of 2350 halved, 0.8 V */
	if ((con1 >> 24 & 7) != 1 || pll_mhz(con1) < 1150 || pll_mhz(con1) > 1200 || vproc < 44 || vproc > 52) {
		printf("cpufreq: %s is not as the boot loader leaves it; left as it is\n", name);
		return false;
	}
	uint32_t want_sram = step + 16 > VOLT_STEP_MAX ? VOLT_STEP_MAX : step + 16;

	if (!(tracking & 1)) {
		uint32_t con = 0;

		if (!pmic_read(vsram_con, &con) || !pmic_write(vsram_con, (con & ~0x7fu) | want_sram)) {
			printf("cpufreq: %s: VSRAM could not be raised; left as it is\n", name);
			return false;
		}
	}
	if (!pmic_write(elr, (elr_was & ~0x7fu) | step)) {
		printf("cpufreq: %s: Vproc could not be raised; left as it is\n", name);
		return false;
	}
	delay(1000);    /* 10 mV a microsecond up, and the PMIC's own delay: far more than enough */
	if (!pmic_read(dbg, &vproc) || (vproc & 0x7f) != step) {
		printf("cpufreq: %s: Vproc reads step %u, not %u; the clock left as it is\n", name, vproc & 0x7f, step);
		return false;
	}
	/* the VCO for the new clock under the old post divider, then the divider */
	uint32_t posdiv = 1u << posdiv_sel;

	wr(apmixed, con1_at, (rd(apmixed, con1_at) & ~0x3fffffu) | pll_dds(mhz * posdiv) | (1u << 31));
	delay(20);
	wr(apmixed, con1_at, (rd(apmixed, con1_at) & ~(7u << 24)) | posdiv_sel << 24);
	delay(1);
	con1 = rd(apmixed, con1_at);
	(void)pmic_read(vsram_dbg, &vsram);
	printf("cpufreq: %s now at %u MHz (CON1 %#x), Vproc step %u, VSRAM step %u\n", name, pll_mhz(con1), con1,
	    step, (vsram >> 8) & 0x7f);
	return true;
}

static void
cpufreq_set(uint32_t how)
{
	/* the A75s first: the window server and SpringBoard run there */
	if (cluster_raise("A75s", ARMPLL_L_CON1, MT6358_VPROC11_ELR0, MT6358_VPROC11_DBG0, MT6358_VSRAM_CON0,
	    MT6358_VSRAM11_DBG0, MT6358_VSRAM11_TRACK, 2000, 0, how >= 2 ? 94 + 2 : 0)) {
		(void)cluster_raise("A55s", ARMPLL_LL_CON1, MT6358_VPROC12_ELR0, MT6358_VPROC12_DBG0,
		    MT6358_VSRAM_CON1, MT6358_VSRAM12_DBG0, MT6358_VSRAM12_TRACK, 1700, 1, how >= 3 ? 74 + 2 : 0);
	}
}

static void
cpufreq_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t how = 0;

	(void)PE_parse_boot_argn("mtk_cpufreq", &how, sizeof(how));
	apmixed = ml_io_map(APMIXED_PHYS, 0x1000);
	pwrap = ml_io_map(PWRAP_PHYS, 0x1000);
	if (apmixed == 0 || pwrap == 0) {
		printf("cpufreq: cannot map the PLL or the PMIC\n");
		thread_terminate(current_thread());
		return;
	}
	cpufreq_set(how);
	for (;;) {
		delay(60 * 1000 * 1000);
		report_cluster("A75s", ARMPLL_L_CON1, MT6358_VPROC11_DBG0, MT6358_VSRAM11_DBG0);
		report_cluster("A55s", ARMPLL_LL_CON1, MT6358_VPROC12_DBG0, MT6358_VSRAM12_DBG0);
	}
}

void
hakios_mtkcpufreq_init(void)
{
	uint32_t how = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("mtk_cpufreq", &how, sizeof(how)) || how == 0) {
		return;
	}
	if (kernel_thread_start(cpufreq_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
