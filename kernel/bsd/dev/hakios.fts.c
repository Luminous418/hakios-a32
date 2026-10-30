/*
 * The touch screen of the Samsung Galaxy A32 4G (SM-A325F): an STM FTS5CU56A on I2C6 at
 * address 0x49, as a source of touches for /dev/hakios-touch (sys/hakios.input.h). Only with
 * boot-arg touchscreen=3: every address below belongs to this phone.
 *
 * There is no I2C framework in this kernel, and the MediaTek I2C6 controller is not needed:
 * the two bus pins are driven by hand, open drain, with the controller's internal pull-ups
 * on (the board's own may be there too). SCL is GPIO89, SDA GPIO90, both at function 0 in
 * GPIO_MODE11 (0x3b0); their pull registers are in the iocfg_0 block at 0x10002000 (PU at
 * +0x60 bits 23 and 25, PD at +0x50). The chip is at 0x49: write address 0x92, read 0x93.
 *
 * The controller keeps its firmware in its own flash; Linux's driver reads the version and
 * only updates it when the file is newer (drivers/input/sec_input/stm/stm_fw.c), so by the
 * time this runs the chip is already up and answering. What it answers with are 16-byte
 * events, one per read of 0x60 (READ_ONE_EVENT; 0x61 reads all of them, 0x62 clears):
 *
 *   byte 0   bits 1:0 event id (0 coordinate, 1 status, 2 gesture, 3 vendor)
 *            bits 5:2 finger (0..9)
 *            bits 7:6 action (1 press, 2 move, 3 release)
 *   byte 1   x bits 11:4
 *   byte 2   y bits 11:4
 *   byte 3   y bits 3:0 in the low nibble, x bits 3:0 in the high nibble
 *   byte 7   bits 4:0 how many more events are waiting
 *
 * The chip reports in its own coordinates (0..4095 here, the panel's 4096 maximum; the exact
 * range comes from READ_PANEL_INFO 0x23), which are scaled to the 0..65535 that
 * hakios_touch_post takes. Boot-arg touch_swap=1 swaps the axes, touch_flip_x=1 and
 * touch_flip_y=1 mirror them; touch_poll_us=N reads every N microseconds instead of 8000.
 *
 * No interrupt is used: the chip's IRQ line (GPIO10, low while an event waits) could be
 * looked at, but reading 0x60 with nothing to say costs one short transfer and returns
 * zeroes, so a thread simply asks 125 times a second, like the Goodix driver beside this one.
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
extern void hakios_tl(const char *format, ...) __printflike(1, 2);

#define GPIO_PHYS       0x10005000
#define GPIO_SIZE       0x1000
#define IOCFG_PHYS      0x10002000      /* iocfg_0: the electrical settings of GPIO89/90 */
#define IOCFG_SIZE      0x1000

/* The PMIC wrapper, and the MT6358's VCAMA2 LDO: the touch screen's 2.8 V (here 3.0 V) rail. */
#define PWRAP_PHYS      0x1000d000
#define PWRAP_SIZE      0x1000
#define PWRAP_WACS2_CMD     0xc20
#define PWRAP_WACS2_RDATA   0xc24
#define PWRAP_WACS2_VLDCLR  0xc28
#define PWRAP_FSM(r)        (((r) >> 16) & 7u)
#define PWRAP_FSM_IDLE      0
#define PWRAP_FSM_WFVLDCLR  6

#define MT6358_VCAMA2_CON0      0x1c88  /* PMIC_RG_LDO_VCAMA2_EN_ADDR, bit 0 enables */
#define MT6358_VCAMA2_OP_EN     0x1c8a  /* PMIC_RG_LDO_VCAMA2_SW_OP_EN, bit 0 */
#define MT6358_VCAMA2_ANA_CON0  0x1e0c  /* PMIC_RG_VCAMA2_VOSEL_ADDR, bits 11:8 */
#define VCAMA2_VOSEL_3000MV     12      /* drivers/regulator/mt6358-regulator.c's table */

#define PIN_SCL         89
#define PIN_SDA         90
#define PIN_TSP_EN      4               /* high: the touch screen's 1.8 V I/O rail (fixed_regulator0 / tsp_en_1p8_ldo) */

#define PIN_BANK(pin)   ((pin) / 32)
#define PIN_BIT(pin)    (1u << ((pin) % 32))

/*
 * The GPIO block keeps three sets of registers per 32 pins: direction at 0x004, output data
 * at 0x104 and input data at 0x200, each set moving on by 0x10 per bank. Pins 89 and 90 are
 * in bank 2, so bit 25 and bit 26 of 0x024/0x028, 0x124/0x128 and 0x220.
 */
#define DIR_SET(bank)   (0x004 + (bank) * 0x10)
#define DIR_CLR(bank)   (0x008 + (bank) * 0x10)
#define DOUT_SET(bank)  (0x104 + (bank) * 0x10)
#define DOUT_CLR(bank)  (0x108 + (bank) * 0x10)
#define DIN(bank)       (0x200 + (bank) * 0x10)

#define MODE_REG(pin)   (0x300 + ((pin) / 8) * 0x10)    /* four bits per pin */

#define IOCFG_PD        0x50
#define IOCFG_PU        0x60
#define IOCFG_BIT_SCL   23                              /* pin 89's pull-up/down bit */
#define IOCFG_BIT_SDA   25                              /* pin 90's */

/* STM commands (drivers/input/sec_input/stm/stm_reg.h) */
#define FTS_CMD_SENSE_ON        0x10
#define FTS_CMD_SENSE_OFF       0x11
#define FTS_READ_DEVICE_ID      0x22
#define FTS_READ_PANEL_INFO     0x23
#define FTS_READ_FW_VERSION     0x24
#define FTS_READ_ONE_EVENT      0x60
#define FTS_CLEAR_ALL_EVENT     0x62

#define FTS_ADDRESS             0x49
#define FTS_WRITE_ADDRESS       ((FTS_ADDRESS) << 1)
#define FTS_READ_ADDRESS        (((FTS_ADDRESS) << 1) | 1)

#define FTS_EVENT_SIZE          16
#define FTS_MAX_FINGERS         10

#define FTS_COORDINATE_EVENT    0
#define FTS_STATUS_EVENT        1

#define FTS_ACTION_PRESS        1
#define FTS_ACTION_MOVE         2
#define FTS_ACTION_RELEASE      3

static vm_offset_t gpio, iocfg, pwrap;

static uint32_t poll_us = 8000;
static uint32_t swap_axes, flip_x, flip_y;
static uint32_t max_x = 4095, max_y = 4095;

static bool finger_down[FTS_MAX_FINGERS];
static uint16_t last_x[FTS_MAX_FINGERS], last_y[FTS_MAX_FINGERS];

/* Kept to say again every minute: the start of the log is the first thing pstore loses. */
static uint8_t device_id[5], firmware[9], panel[11];
static bool have_device_id, have_firmware, have_panel;
static uint8_t last_status[8];
static bool have_status;

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

static inline void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

/* ---- the pins, open drain ---------------------------------------------------------------- */

/*
 * A pin is pulled low by making it an output at 0 and let go by making it an input again;
 * the pull-up (the board's, or the one enabled below) brings the line back up. The output
 * level is written before the direction changes so that the pin never drives the wrong one.
 */
static inline void
pin_low(unsigned pin)
{
	uint32_t bit = PIN_BIT(pin);

	wr(gpio, DOUT_CLR(PIN_BANK(pin)), bit);
	wr(gpio, DIR_SET(PIN_BANK(pin)), bit);
}

static inline void
pin_release(unsigned pin)
{
	uint32_t bit = PIN_BIT(pin);

	wr(gpio, DOUT_SET(PIN_BANK(pin)), bit);
	wr(gpio, DIR_CLR(PIN_BANK(pin)), bit);
}

static inline bool
pin_value(unsigned pin)
{
	return (rd(gpio, DIN(PIN_BANK(pin))) & PIN_BIT(pin)) != 0;
}

/* ---- the PMIC wrapper, for VCAMA2 --------------------------------------------------------- */

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

/*
 * The touch screen's analog rail is VCAMA2: `tsp_avdd_ldo` in the device tree, on at boot for
 * Linux, but this kernel has to ask for it. GPIO4 (above) is its 1.8 V I/O rail; with only that
 * one on the chip answers on I2C but nothing reaches the analog front end, so no finger is
 * ever seen. Set the voltage to the 3.0 V the device tree asks for and enable it.
 */
static void
touch_power_on(void)
{
	uint32_t con0 = 0, ana = 0, op = 0;

	if (pwrap == 0) {
		printf("fts: no pwrap, cannot enable vcama2\n");
		return;
	}
	if (!pmic_read(MT6358_VCAMA2_CON0, &con0) || !pmic_read(MT6358_VCAMA2_ANA_CON0, &ana)) {
		printf("fts: the pmic did not answer; vcama2 may be off\n");
		return;
	}
	printf("fts: vcama2 before: con0 %04x ana %04x\n", con0, ana);

	(void)pmic_write(MT6358_VCAMA2_ANA_CON0, (ana & ~0xf00u) | ((uint32_t)VCAMA2_VOSEL_3000MV << 8));
	(void)pmic_read(MT6358_VCAMA2_OP_EN, &op);
	(void)pmic_write(MT6358_VCAMA2_OP_EN, op | 1u);
	(void)pmic_write(MT6358_VCAMA2_CON0, con0 | 1u);
	sleep_us(2000);

	con0 = ana = 0;
	(void)pmic_read(MT6358_VCAMA2_CON0, &con0);
	(void)pmic_read(MT6358_VCAMA2_ANA_CON0, &ana);
	printf("fts: vcama2 after: con0 %04x ana %04x (enabled %u, %u mV)\n",
	    con0, ana, con0 & 1u, ((ana >> 8) & 0xf) == VCAMA2_VOSEL_3000MV ? 3000 : 0);
}

static void
pins_to_gpio(void)
{
	uint32_t mode, pu, pd;

	/* The 1.8 V I/O rail is a fixed regulator on GPIO4, high to enable. */
	wr(gpio, DOUT_SET(PIN_BANK(PIN_TSP_EN)), PIN_BIT(PIN_TSP_EN));
	wr(gpio, DIR_SET(PIN_BANK(PIN_TSP_EN)), PIN_BIT(PIN_TSP_EN));
	mode = rd(gpio, MODE_REG(PIN_TSP_EN));
	mode &= ~(0xfu << ((PIN_TSP_EN % 8) * 4));
	wr(gpio, MODE_REG(PIN_TSP_EN), mode);

	mode = rd(gpio, MODE_REG(PIN_SCL));
	mode &= ~(0xfu << ((PIN_SCL % 8) * 4));
	mode &= ~(0xfu << ((PIN_SDA % 8) * 4));
	wr(gpio, MODE_REG(PIN_SCL), mode);

	/* The bus is open drain: no pull-down, pull-up on both lines. */
	pd = rd(iocfg, IOCFG_PD);
	pd &= ~((1u << IOCFG_BIT_SCL) | (1u << IOCFG_BIT_SDA));
	wr(iocfg, IOCFG_PD, pd);
	pu = rd(iocfg, IOCFG_PU);
	pu |= (1u << IOCFG_BIT_SCL) | (1u << IOCFG_BIT_SDA);
	wr(iocfg, IOCFG_PU, pu);

	__asm__ volatile ("dsb sy");
	pin_release(PIN_SCL);
	pin_release(PIN_SDA);
}

/* ---- I2C, by hand ------------------------------------------------------------------------ */

/* Quarter of a period at about 100 kHz, with room for the register writes to settle. */
static inline void
i2c_half(void)
{
	sleep_us(4);
}

static void
i2c_start(void)
{
	pin_release(PIN_SDA);
	pin_release(PIN_SCL);
	i2c_half();
	pin_low(PIN_SDA);
	i2c_half();
	pin_low(PIN_SCL);
	i2c_half();
}

static void
i2c_stop(void)
{
	pin_low(PIN_SDA);
	i2c_half();
	pin_release(PIN_SCL);
	i2c_half();
	pin_release(PIN_SDA);
	i2c_half();
}

/* Nine clocks with the data line let go: enough to walk a chip out of a half-finished byte. */
static void
i2c_recover(void)
{
	pin_release(PIN_SDA);
	for (unsigned i = 0; i < 9; i++) {
		pin_release(PIN_SCL);
		i2c_half();
		pin_low(PIN_SCL);
		i2c_half();
	}
	i2c_stop();
}

static bool
i2c_write_byte(uint8_t value)
{
	bool ack;

	for (unsigned i = 0; i < 8; i++) {
		if (value & 0x80) {
			pin_release(PIN_SDA);
		} else {
			pin_low(PIN_SDA);
		}
		i2c_half();
		pin_release(PIN_SCL);
		i2c_half();
		pin_low(PIN_SCL);
		i2c_half();
		value <<= 1;
	}

	pin_release(PIN_SDA);
	i2c_half();
	pin_release(PIN_SCL);
	i2c_half();
	ack = !pin_value(PIN_SDA);
	pin_low(PIN_SCL);
	i2c_half();
	return ack;
}

static uint8_t
i2c_read_byte(bool acknowledge)
{
	uint8_t value = 0;

	pin_release(PIN_SDA);
	for (unsigned i = 0; i < 8; i++) {
		i2c_half();
		pin_release(PIN_SCL);
		i2c_half();
		value = (uint8_t)((value << 1) | (pin_value(PIN_SDA) ? 1 : 0));
		pin_low(PIN_SCL);
		i2c_half();
	}

	if (acknowledge) {
		pin_low(PIN_SDA);
	} else {
		pin_release(PIN_SDA);
	}
	i2c_half();
	pin_release(PIN_SCL);
	i2c_half();
	pin_low(PIN_SCL);
	i2c_half();
	pin_release(PIN_SDA);
	return value;
}

/*
 * A register read: the command byte, a repeated start, then the data. This is the shape
 * Linux's stm_ts_i2c_read asks the I2C core for, with two messages in one transfer.
 */
static bool
fts_read_once(uint8_t command, uint8_t *data, unsigned length)
{
	unsigned i;

	i2c_start();
	if (!i2c_write_byte(FTS_WRITE_ADDRESS) || !i2c_write_byte(command)) {
		i2c_stop();
		return false;
	}
	i2c_start();
	if (!i2c_write_byte(FTS_READ_ADDRESS)) {
		i2c_stop();
		return false;
	}
	for (i = 0; i < length; i++) {
		data[i] = i2c_read_byte(i + 1 < length);
	}
	i2c_stop();
	return true;
}

static bool
fts_write_once(uint8_t command, const uint8_t *data, unsigned length)
{
	unsigned i;

	i2c_start();
	if (!i2c_write_byte(FTS_WRITE_ADDRESS) || !i2c_write_byte(command)) {
		i2c_stop();
		return false;
	}
	for (i = 0; i < length; i++) {
		if (!i2c_write_byte(data[i])) {
			i2c_stop();
			return false;
		}
	}
	i2c_stop();
	return true;
}

static bool
fts_read(uint8_t command, uint8_t *data, unsigned length)
{
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		if (fts_read_once(command, data, length)) {
			return true;
		}
		i2c_recover();
	}
	return false;
}

static bool
fts_write(uint8_t command, const uint8_t *data, unsigned length)
{
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		if (fts_write_once(command, data, length)) {
			return true;
		}
		i2c_recover();
	}
	return false;
}

/* ---- what the chip says ------------------------------------------------------------------ */

static uint32_t
scale(uint32_t value, uint32_t maximum)
{
	if (value > maximum) {
		value = maximum;
	}
	if (maximum == 0) {
		return 0;
	}
	return (uint32_t)(((uint64_t)value * 65535u + maximum / 2) / maximum);
}

static void
coordinate_event(const uint8_t *event, uint32_t *events, uint32_t *touches)
{
	uint32_t id = (event[0] >> 2) & 0xf;
	uint32_t action = (event[0] >> 6) & 3;
	uint32_t x = ((uint32_t)event[1] << 4) | (event[3] >> 4);
	uint32_t y = ((uint32_t)event[2] << 4) | (event[3] & 0xf);

	if (id >= FTS_MAX_FINGERS || action == 0) {
		return;
	}

	/* A release sometimes comes with no coordinates: keep the last ones. */
	if (action == FTS_ACTION_RELEASE && x == 0 && y == 0 && finger_down[id]) {
		x = last_x[id];
		y = last_y[id];
	}
	last_x[id] = (uint16_t)x;
	last_y[id] = (uint16_t)y;

	if (swap_axes) {
		uint32_t t = x;

		x = y;
		y = t;
	}
	if (flip_x) {
		x = max_x - (x > max_x ? max_x : x);
	}
	if (flip_y) {
		y = max_y - (y > max_y ? max_y : y);
	}

	x = scale(x, max_x);
	y = scale(y, max_y);

	switch (action) {
	case FTS_ACTION_PRESS:
		finger_down[id] = true;
		hakios_touch_post(HAKIOS_TOUCH_DOWN, id, x, y);
		(*touches)++;
		break;
	case FTS_ACTION_MOVE:
		finger_down[id] = true;
		hakios_touch_post(HAKIOS_TOUCH_MOVE, id, x, y);
		(*touches)++;
		break;
	case FTS_ACTION_RELEASE:
		finger_down[id] = false;
		hakios_touch_post(HAKIOS_TOUCH_UP, id, x, y);
		break;
	}
	(*events)++;
}

static void
fts_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint8_t data[16];
	uint32_t events = 0, touches = 0, reads = 0, errors = 0, status_events = 0;
	uint32_t latency = 0;
	uint64_t period;

	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	PE_parse_boot_argn("touch_swap", &swap_axes, sizeof(swap_axes));
	PE_parse_boot_argn("touch_flip_x", &flip_x, sizeof(flip_x));
	PE_parse_boot_argn("touch_flip_y", &flip_y, sizeof(flip_y));
	PE_parse_boot_argn("touch_lat", &latency, sizeof(latency));
	if (poll_us < 1000 || poll_us > 100000) {
		poll_us = 8000;
	}

	gpio = ml_io_map(GPIO_PHYS, GPIO_SIZE);
	iocfg = ml_io_map(IOCFG_PHYS, IOCFG_SIZE);
	if (gpio == 0 || iocfg == 0) {
		printf("fts: cannot map the pins\n");
		thread_terminate(current_thread());
		return;
	}

	pwrap = ml_io_map(PWRAP_PHYS, PWRAP_SIZE);
	pins_to_gpio();
	sleep_us(2000);
	touch_power_on();
	sleep_us(30000);                /* the rails' ramp delay from the device tree is 270 us; be generous */

	/* Before touching the bus: are the two lines even high? Both low means no pull-ups. */
	printf("fts: pins mode %08x pu %08x pd %08x; idle scl %d sda %d\n",
	    rd(gpio, MODE_REG(PIN_SCL)), rd(iocfg, IOCFG_PU), rd(iocfg, IOCFG_PD),
	    pin_value(PIN_SCL) ? 1 : 0, pin_value(PIN_SDA) ? 1 : 0);

	i2c_recover();

	{
		uint64_t before = mach_absolute_time(), after;
		uint64_t ns;

		memset(data, 0, sizeof(data));
		if (!fts_read(FTS_READ_DEVICE_ID, data, 5)) {
			printf("fts: no answer at 0x%02x on i2c6 (gpio%u/gpio%u): the address was not acknowledged\n",
			    FTS_ADDRESS, PIN_SCL, PIN_SDA);
			thread_terminate(current_thread());
			return;
		}
		after = mach_absolute_time();
		absolutetime_to_nanoseconds(after - before, &ns);
		memcpy(device_id, data, sizeof(device_id));
		have_device_id = true;
		printf("fts: device id %02x %02x %02x %02x %02x, a read takes %llu us%s\n",
		    data[0], data[1], data[2], data[3], data[4], ns / 1000,
		    (data[0] | data[1] | data[2] | data[3] | data[4]) ? "" : " (all zero: the bus answers but nothing drives it)");
	}

	memset(data, 0, sizeof(data));
	if (fts_read(FTS_READ_FW_VERSION, data, 9)) {
		memcpy(firmware, data, sizeof(firmware));
		have_firmware = true;
		printf("fts: firmware %02x%02x config %02x%02x main %02x%02x project %02x ic %02x module %02x\n",
		    data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8]);
	}

	memset(data, 0, sizeof(data));
	if (fts_read(FTS_READ_PANEL_INFO, data, 11)) {
		uint32_t rx = ((uint32_t)data[0] << 8) | data[1];
		uint32_t ry = ((uint32_t)data[2] << 8) | data[3];

		memcpy(panel, data, sizeof(panel));
		have_panel = true;
		printf("fts: panel %ux%u, %u tx and %u rx nodes\n", rx, ry, data[8], data[9]);
		if (rx > 0 && rx < 0xffff) {
			max_x = rx;
		}
		if (ry > 0 && ry < 0xffff) {
			max_y = ry;
		}
	}

	/* Sense on, then drop whatever the chip queued while it was waking. */
	if (!fts_write(FTS_CMD_SENSE_ON, NULL, 0)) {
		printf("fts: sense on was not acknowledged\n");
	}
	sleep_us(30000);
	(void)fts_write(FTS_CLEAR_ALL_EVENT, NULL, 0);

	printf("fts: touch screen ready; asked every %u us, %ux%u scaled to 65535\n", poll_us, max_x + 1, max_y + 1);

	period = (uint64_t)60 * 1000000 / poll_us;
	for (unsigned tick = 1;; tick++) {
		unsigned got = 0;

		for (unsigned i = 0; i < FTS_MAX_FINGERS; i++) {
			if (!fts_read(FTS_READ_ONE_EVENT, data, FTS_EVENT_SIZE)) {
				errors++;
				break;
			}
			reads++;
			if (data[0] == 0) {
				break;          /* nothing waiting */
			}
			if ((data[0] & 3) == FTS_COORDINATE_EVENT) {
				coordinate_event(data, &events, &touches);
			} else if ((data[0] & 3) == FTS_STATUS_EVENT) {
				memcpy(last_status, data, sizeof(last_status));
				have_status = true;
				if (status_events < 3) {
					printf("fts: status event %02x %02x %02x %02x %02x %02x %02x %02x\n",
					    data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
				}
				status_events++;
			}
			got++;
			if ((data[7] & 0x1f) == 0) {
				break;
			}
		}

		if (latency && got) {
			uint64_t ns;

			absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
			hakios_tl("tl: f %u events at %llu\n", got, ns / 1000);
		}

		if (tick % period == 0) {
			printf("fts: so far %u events, %u finger positions, %u status events in %u reads (%u failed); "
			    "last at %u, %u; the bus %s\n",
			    events, touches, status_events, reads, errors,
			    last_x[0], last_y[0], errors > reads / 2 ? "mostly silent" : "answers");
			if (have_device_id) {
				printf("fts: id %02x %02x %02x %02x %02x", device_id[0], device_id[1],
				    device_id[2], device_id[3], device_id[4]);
			}
			if (have_firmware) {
				printf(", firmware %02x%02x config %02x%02x main %02x%02x ic %02x",
				    firmware[0], firmware[1], firmware[2], firmware[3],
				    firmware[4], firmware[5], firmware[7]);
			}
			if (have_panel) {
				printf(", panel %ux%u", ((uint32_t)panel[0] << 8) | panel[1],
				    ((uint32_t)panel[2] << 8) | panel[3]);
			}
			if (have_status) {
				printf(", last status %02x %02x %02x %02x", last_status[0], last_status[1],
				    last_status[2], last_status[3]);
			}
			printf("\n");
			if (events == 0) {
				/* Nothing at all yet: ask again, in case the first sense on came before the rails. */
				(void)fts_write(FTS_CMD_SENSE_OFF, NULL, 0);
				sleep_us(10000);
				(void)fts_write(FTS_CMD_SENSE_ON, NULL, 0);
				sleep_us(30000);
				(void)fts_write(FTS_CLEAR_ALL_EVENT, NULL, 0);
				printf("fts: asked the chip to sense again\n");
			}
		}

		delay_for_interval(poll_us, NSEC_PER_USEC);
	}
}

void
hakios_fts_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || enabled != 3) {
		return;
	}
	if (kernel_thread_start(fts_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
