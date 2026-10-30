/*
 * The battery of the Samsung Galaxy A32 4G, from the MT6358's own ADC. Only with boot-arg
 * battery=2: the addresses belong to this phone (battery=1 is the Qualcomm driver beside this
 * one, which reads a cache the UEFI loader leaves).
 *
 * The PMIC measures the battery on AUXADC channel 0 (BATADC): request it in RQST0 bit 0, wait
 * for bit 15 of ADC0, and the 15 bits below are the voltage over a 3:1 divider with a 1.8 V
 * full scale, so
 *
 *     mV = raw * 3 * 1800 / 32768
 *
 * (the device tree says resistance-ratio 3 1 and 128 averages; Linux's
 * drivers/iio/adc/mt635x-auxadc.c is the same arithmetic). There is no fuel gauge here: the
 * percentage is interpolated from that voltage, and while the processors run at 2 GHz the
 * voltage sags a little under load, so the reading is smoothed and the result is only as good
 * as a voltage estimate can be.
 *
 * Charging is the PMIC's CHRDET bit (TOPSTATUS bit 2, clear while no charger is on the port);
 * the battery is always present. Temperature and current are not read yet: 25 C and 0 mA are
 * placeholders.
 *
 * The values go to iOS the same way the other phone's do: hakios_battery_report()
 * (bsd/dev/hakios.battery.c) keeps them for the sysctls and hands them to HakiosBattery
 * (iokit/Kernel/hakios.battery.cpp), the IOPMPowerSource powerd reads.
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

#define PWRAP_PHYS              0x1000d000
#define PWRAP_WACS2_CMD         0xc20
#define PWRAP_WACS2_RDATA       0xc24
#define PWRAP_WACS2_VLDCLR      0xc28
#define PWRAP_FSM(r)            (((r) >> 16) & 7u)
#define PWRAP_FSM_IDLE          0
#define PWRAP_FSM_WFVLDCLR      6

#define MT6358_TOPSTATUS        0x28
#define CHRDET                  (1u << 2)       /* clear while no charger is on the port */

#define MT6358_AUXADC_RQST0     0x1108
#define MT6358_AUXADC_ADC0      0x1088
#define AUXADC_RDY              (1u << 15)
#define AUXADC_VALUE_MASK       0x7fffu

#define BATADC_RATIO            3               /* resistance-ratio in the device tree: 3 1 */
#define BATADC_FULL_MV          1800            /* the ADC's full scale */
#define BATADC_RESOLUTION       15

#define BATTERY_DESIGN_UAH      5000000         /* the A32's battery is 5000 mAh */

static vm_offset_t pwrap;

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

static bool
pmic_write(uint32_t reg, uint32_t value)
{
	if (!pwrap_wait(PWRAP_FSM_IDLE)) {
		return false;
	}
	wr(pwrap, PWRAP_WACS2_CMD, (1u << 31) | ((reg >> 1) << 16) | (value & 0xffff));
	return pwrap_wait(PWRAP_FSM_IDLE);
}

/* One BATADC conversion: the request, 128 averages of 10 us, then the ready bit. */
static bool
batadc_raw(uint32_t *raw)
{
	uint32_t rqst = 0, adc = 0;

	if (!pmic_read(MT6358_AUXADC_RQST0, &rqst)) {
		return false;
	}
	if (!pmic_write(MT6358_AUXADC_RQST0, rqst | 1u)) {
		return false;
	}
	delay(2000);            /* 128 * 10 us, with room */
	for (unsigned i = 0; i < 50; i++) {
		if (!pmic_read(MT6358_AUXADC_ADC0, &adc)) {
			return false;
		}
		if (adc & AUXADC_RDY) {
			*raw = adc & AUXADC_VALUE_MASK;
			return true;
		}
		delay(1000);
	}
	return false;
}

/* Li-ion, at rest; 4.086 V is Android's 82 % on this phone (27.9 C, USB in). */
static const struct {
	uint16_t mv;
	uint8_t percent;
} curve[] = {
	{ 3300, 0 }, { 3450, 3 }, { 3550, 7 }, { 3650, 14 }, { 3700, 22 },
	{ 3750, 32 }, { 3800, 42 }, { 3850, 53 }, { 3900, 62 }, { 3950, 70 },
	{ 4000, 76 }, { 4050, 81 }, { 4100, 87 }, { 4150, 93 }, { 4200, 100 },
};

static uint32_t
percent_from_mv(uint32_t mv)
{
	for (unsigned i = 1; i < sizeof(curve) / sizeof(curve[0]); i++) {
		if (mv <= curve[i].mv) {
			uint32_t lo_mv = curve[i - 1].mv, hi_mv = curve[i].mv;
			uint32_t lo_p = curve[i - 1].percent, hi_p = curve[i].percent;

			if (hi_mv == lo_mv) {
				return hi_p;
			}
			return lo_p + (mv - lo_mv) * (hi_p - lo_p) / (hi_mv - lo_mv);
		}
	}
	if (mv < curve[0].mv) {
		return 0;
	}
	return 100;
}

static void
battery_poll(uint32_t *last_mv, uint32_t *last_percent, bool *last_charger)
{
	uint32_t sum = 0, good = 0, top = 0, mv, percent;
	bool charger;
	struct hakios_battery_state state;

	for (unsigned i = 0; i < 4; i++) {
		uint32_t raw = 0;

		if (batadc_raw(&raw)) {
			sum += raw;
			good++;
		}
	}
	if (good == 0) {
		printf("battery: the PMIC's BATADC did not answer\n");
		return;
	}
	mv = (uint32_t)(((uint64_t)(sum / good) * BATADC_RATIO * BATADC_FULL_MV) >> BATADC_RESOLUTION);
	percent = percent_from_mv(mv);
	(void)pmic_read(MT6358_TOPSTATUS, &top);
	charger = (top & CHRDET) != 0;

	memset(&state, 0, sizeof(state));
	state.size = sizeof(state);
	state.percent_hundredths = (int32_t)percent * 100;
	state.voltage_uv = (int32_t)mv * 1000;
	state.current_ua = 0;
	state.temperature_hundredths = 2500;    /* not read yet */
	state.present = 1;
	state.external = charger ? 1 : 0;
	state.status = charger ? (percent >= 100 ? 4 : 1) : 2;
	state.charge_uah = (int32_t)(percent * (BATTERY_DESIGN_UAH / 100));
	state.full_uah = BATTERY_DESIGN_UAH;
	state.design_uah = BATTERY_DESIGN_UAH;
	state.cycles = 0;
	state.usb_type = charger ? 1 : 0;
	state.usb_voltage_uv = charger ? 5000000 : 0;
	state.usb_current_ua = charger ? 500000 : 0;
	hakios_battery_report(&state);

	*last_mv = mv;
	*last_percent = percent;
	*last_charger = charger;
}

static void
mtkbattery_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t mv = 0, percent = 0;
	bool charger = false;

	pwrap = ml_io_map(PWRAP_PHYS, 0x1000);
	if (pwrap == 0) {
		printf("battery: cannot map the PMIC wrapper\n");
		thread_terminate(current_thread());
		return;
	}
	printf("battery: MT6358 BATADC and CHRDET, read every 10 s\n");

	for (unsigned tick = 1;; tick++) {
		battery_poll(&mv, &percent, &charger);
		if (tick % 6 == 0) {
			printf("battery: %u mV, %u%%, charger %s\n", mv, percent, charger ? "on" : "off");
		}
		delay_for_interval(10000, NSEC_PER_MSEC);
	}
}

void
hakios_mtkbattery_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("battery", &enabled, sizeof(enabled)) || enabled != 2) {
		return;
	}
	if (kernel_thread_start(mtkbattery_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
