/* Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* Kalista baseboard configuration */

#include "adc.h"
#include "adc_chip.h"
#include "baseboard.h"
#include "battery.h"
#include "bd99992gw.h"
#include "board_config.h"
#include "button.h"
#include "chipset.h"
#include "console.h"
#include "cros_board_info.h"
#include "driver/pmic_tps650x30.h"
#include "driver/temp_sensor/tmp432.h"
#include "driver/tcpm/ps8xxx.h"
#include "driver/tcpm/tcpci.h"
#include "driver/tcpm/tcpm.h"
#include "espi.h"
#include "extpower.h"
#include "espi.h"
#include "fan.h"
#include "fan_chip.h"
#include "gpio.h"
#include "hooks.h"
#include "host_command.h"
#include "i2c.h"
#include "math_util.h"
#include "oz554.h"
#include "pi3usb9281.h"
#include "power.h"
#include "power_button.h"
#include "pwm.h"
#include "pwm_chip.h"
#include "spi.h"
#include "switch.h"
#include "system.h"
#include "task.h"
#include "temp_sensor.h"
#include "timer.h"
#include "uart.h"
#include "usb_charge.h"
#include "usb_mux.h"
#include "usb_pd.h"
#include "usb_pd_tcpm.h"
#include "util.h"

#define CPRINTS(format, args...) cprints(CC_USBCHARGE, format, ## args)
#define CPRINTF(format, args...) cprintf(CC_USBCHARGE, format, ## args)

static uint8_t board_version;
static uint32_t oem;
static uint32_t sku;

enum bj_adapter {
	BJ_90W_19V,
	BJ_135W_19V,
};

/*
 * Bit masks to map SKU ID to BJ adapter wattage. 1:135W 0:90W
 * KBL-R i7 8550U	4	135
 * KBL-R i5 8250U	5	135
 * KBL-R i3 8130U	6	135
 * KBL-U i7 7600	3	135
 * KBL-U i5 7500	2	135
 * KBL-U i3 7100	1	90
 * KBL-U Celeron 3965	7	90
 * KBL-U Celeron 3865	0	90
 */
#define BJ_ADAPTER_135W_MASK (1 << 4 | 1 << 5 | 1 << 6 | 1 << 3 | 1 << 2)

static void tcpc_alert_event(enum gpio_signal signal)
{
	if (!gpio_get_level(GPIO_USB_C0_PD_RST_ODL))
		return;
#ifdef HAS_TASK_PDCMD
	/* Exchange status with TCPCs */
	host_command_pd_send_status(PD_CHARGE_NO_CHANGE);
#endif
}

void vbus0_evt(enum gpio_signal signal)
{
	task_wake(TASK_ID_PD_C0);
}

#include "gpio_list.h"

/* Hibernate wake configuration */
const enum gpio_signal hibernate_wake_pins[] = {
	GPIO_POWER_BUTTON_L,
};
const int hibernate_wake_pins_used = ARRAY_SIZE(hibernate_wake_pins);

/* ADC channels */
const struct adc_t adc_channels[] = {
	/* Vbus sensing (1/10 voltage divider). */
	[ADC_VBUS] = {"VBUS", NPCX_ADC_CH2, ADC_MAX_VOLT*10, ADC_READ_MAX+1, 0},
};
BUILD_ASSERT(ARRAY_SIZE(adc_channels) == ADC_CH_COUNT);

/* TODO: Verify fan control and mft */
const struct fan_conf fan_conf_0 = {
	.flags = FAN_USE_RPM_MODE,
	.ch = MFT_CH_0,	/* Use MFT id to control fan */
	.pgood_gpio = -1,
	.enable_gpio = GPIO_FAN_PWR_EN,
};

const struct fan_rpm fan_rpm_0 = {
	.rpm_min = 2180,
	.rpm_start = 2180,
	.rpm_max = 4900,
};

struct fan_t fans[] = {
	[FAN_CH_0] = { .conf = &fan_conf_0, .rpm = &fan_rpm_0, },
};
BUILD_ASSERT(ARRAY_SIZE(fans) == FAN_CH_COUNT);

const struct mft_t mft_channels[] = {
	[MFT_CH_0] = {NPCX_MFT_MODULE_2, TCKC_LFCLK, PWM_CH_FAN},
};
BUILD_ASSERT(ARRAY_SIZE(mft_channels) == MFT_CH_COUNT);

const struct i2c_port_t i2c_ports[]  = {
	{"tcpc", I2C_PORT_TCPC0, 400, GPIO_I2C0_0_SCL, GPIO_I2C0_0_SDA},
	{"eeprom", I2C_PORT_EEPROM, 400, GPIO_I2C0_1_SCL, GPIO_I2C0_1_SDA},
	{"backlight", I2C_PORT_BACKLIGHT, 100, GPIO_I2C1_SCL, GPIO_I2C1_SDA},
	{"pmic", I2C_PORT_PMIC, 400, GPIO_I2C2_SCL, GPIO_I2C2_SDA},
	{"thermal", I2C_PORT_THERMAL, 400, GPIO_I2C3_SCL, GPIO_I2C3_SDA},
};
const unsigned int i2c_ports_used = ARRAY_SIZE(i2c_ports);

/* TCPC mux configuration */
const struct tcpc_config_t tcpc_config[CONFIG_USB_PD_PORT_COUNT] = {
	/* Alert is active-low, push-pull */
	{
		.bus_type = EC_BUS_TYPE_I2C,
		.i2c_info = {
			.port = I2C_PORT_TCPC0,
			.addr = I2C_ADDR_TCPC0,
		},
		.drv = &ps8xxx_tcpm_drv,
	},
};

static int ps8751_tune_mux(int port)
{
	/* 0x98 sets lower EQ of DP port (4.5db) */
	mux_write(port, PS8XXX_REG_MUX_DP_EQ_CONFIGURATION, 0x98);
	return EC_SUCCESS;
}

struct usb_mux usb_muxes[CONFIG_USB_PD_PORT_COUNT] = {
	{
		.driver = &tcpci_tcpm_usb_mux_driver,
		.hpd_update = &ps8xxx_tcpc_update_hpd_status,
		.board_init = &ps8751_tune_mux,
	}
};

const int usb_port_enable[USB_PORT_COUNT] = {
	GPIO_USB1_ENABLE,
	GPIO_USB2_ENABLE,
	GPIO_USB3_ENABLE,
	GPIO_USB4_ENABLE,
};

void board_reset_pd_mcu(void)
{
	gpio_set_level(GPIO_USB_C0_PD_RST_ODL, 0);
	msleep(1);
	gpio_set_level(GPIO_USB_C0_PD_RST_ODL, 1);
}

void board_tcpc_init(void)
{
	int port, reg;

	/* This needs to be executed only once per boot. It could be run by RO
	 * if we boot in recovery mode. It could be run by RW if we boot in
	 * normal or dev mode. Note EFS makes RO jump to RW before HOOK_INIT. */
	board_reset_pd_mcu();

	/*
	 * Wake up PS8751. If PS8751 remains in low power mode after sysjump,
	 * TCPM_INIT will fail due to not able to access PS8751.
	 * Note PS8751 A3 will wake on any I2C access.
	 */
	i2c_read8(I2C_PORT_TCPC0, I2C_ADDR_TCPC0, 0xA0, &reg);

	/* Enable TCPC interrupts */
	gpio_enable_interrupt(GPIO_USB_C0_PD_INT_ODL);

	/*
	 * Initialize HPD to low; after sysjump SOC needs to see
	 * HPD pulse to enable video path
	 */
	for (port = 0; port < CONFIG_USB_PD_PORT_COUNT; port++) {
		const struct usb_mux *mux = &usb_muxes[port];
		mux->hpd_update(port, 0, 0);
	}
}
DECLARE_HOOK(HOOK_INIT, board_tcpc_init, HOOK_PRIO_INIT_I2C+1);

uint16_t tcpc_get_alert_status(void)
{
	if (!gpio_get_level(GPIO_USB_C0_PD_INT_ODL) &&
			gpio_get_level(GPIO_USB_C0_PD_RST_ODL))
		return PD_STATUS_TCPC_ALERT_0;
	return 0;
}

/*
 * TMP431 has one local and one remote sensor.
 *
 * Temperature sensors data; must be in same order as enum temp_sensor_id.
 * Sensor index and name must match those present in coreboot:
 *     src/mainboard/google/${board}/acpi/dptf.asl
 */
const struct temp_sensor_t temp_sensors[] = {
	{"TMP431_Internal", TEMP_SENSOR_TYPE_BOARD, tmp432_get_val,
			TMP432_IDX_LOCAL, 4},
	{"TMP431_Sensor_1", TEMP_SENSOR_TYPE_BOARD, tmp432_get_val,
			TMP432_IDX_REMOTE1, 4},
};
BUILD_ASSERT(ARRAY_SIZE(temp_sensors) == TEMP_SENSOR_COUNT);

/*
 * Thermal limits for each temp sensor.  All temps are in degrees K.  Must be in
 * same order as enum temp_sensor_id.  To always ignore any temp, use 0.
 */
struct ec_thermal_config thermal_params[] = {
	/* {Twarn, Thigh, Thalt}, <on>
	 * {Twarn, Thigh, X    }, <off>
	 * fan_off, fan_max
	 */
	{{0, C_TO_K(80), C_TO_K(81)}, {0, C_TO_K(78), 0},
		C_TO_K(4), C_TO_K(76)},	/* TMP431_Internal */
	{{0, 0, 0}, {0, 0, 0}, 0, 0},	/* TMP431_Sensor_1 */
};
BUILD_ASSERT(ARRAY_SIZE(thermal_params) == TEMP_SENSOR_COUNT);

/* Initialize PMIC */
#define I2C_PMIC_READ(reg, data) \
		i2c_read8(I2C_PORT_PMIC, TPS650X30_I2C_ADDR1, (reg), (data))

#define I2C_PMIC_WRITE(reg, data) \
		i2c_write8(I2C_PORT_PMIC, TPS650X30_I2C_ADDR1, (reg), (data))

static void board_pmic_init(void)
{
	int err;
	int error_count = 0;
	static uint8_t pmic_initialized = 0;

	if (pmic_initialized)
		return;

	/* Read vendor ID */
	while (1) {
		int data;
		err = I2C_PMIC_READ(TPS650X30_REG_VENDORID, &data);
		if (!err && data == TPS650X30_VENDOR_ID)
			break;
		else if (error_count > 5)
			goto pmic_error;
		error_count++;
	}

	/*
	 * VCCIOCNT register setting
	 * [6] : CSDECAYEN
	 * otherbits: default
	 */
	err = I2C_PMIC_WRITE(TPS650X30_REG_VCCIOCNT, 0x4A);
	if (err)
		goto pmic_error;

	/*
	 * VRMODECTRL:
	 * [4] : VCCIOLPM clear
	 * otherbits: default
	 */
	err = I2C_PMIC_WRITE(TPS650X30_REG_VRMODECTRL, 0x2F);
	if (err)
		goto pmic_error;

	/*
	 * PGMASK1 : Exclude VCCIO from Power Good Tree
	 * [7] : MVCCIOPG clear
	 * otherbits: default
	 */
	err = I2C_PMIC_WRITE(TPS650X30_REG_PGMASK1, 0x80);
	if (err)
		goto pmic_error;

	/*
	 * PWFAULT_MASK1 Register settings
	 * [7] : 1b V4 Power Fault Masked
	 * [4] : 1b V7 Power Fault Masked
	 * [2] : 1b V9 Power Fault Masked
	 * [0] : 1b V13 Power Fault Masked
	 */
	err = I2C_PMIC_WRITE(TPS650X30_REG_PWFAULT_MASK1, 0x95);
	if (err)
		goto pmic_error;

	/*
	 * Discharge control 4 register configuration
	 * [7:6] : 00b Reserved
	 * [5:4] : 01b V3.3S discharge resistance (V6S), 100 Ohm
	 * [3:2] : 01b V18S discharge resistance (V8S), 100 Ohm
	 * [1:0] : 01b V100S discharge resistance (V11S), 100 Ohm
	 */
	err = I2C_PMIC_WRITE(TPS650X30_REG_DISCHCNT4, 0x15);
	if (err)
		goto pmic_error;

	/*
	 * Discharge control 3 register configuration
	 * [7:6] : 01b V1.8U_2.5U discharge resistance (V9), 100 Ohm
	 * [5:4] : 01b V1.2U discharge resistance (V10), 100 Ohm
	 * [3:2] : 01b V100A discharge resistance (V11), 100 Ohm
	 * [1:0] : 01b V085A discharge resistance (V12), 100 Ohm
	 */
	err = I2C_PMIC_WRITE(TPS650X30_REG_DISCHCNT3, 0x55);
	if (err)
		goto pmic_error;

	/*
	 * Discharge control 2 register configuration
	 * [7:6] : 01b V5ADS3 discharge resistance (V5), 100 Ohm
	 * [5:4] : 01b V33A_DSW discharge resistance (V6), 100 Ohm
	 * [3:2] : 01b V33PCH discharge resistance (V7), 100 Ohm
	 * [1:0] : 01b V18A discharge resistance (V8), 100 Ohm
	 */
	err = I2C_PMIC_WRITE(TPS650X30_REG_DISCHCNT2, 0x55);
	if (err)
		goto pmic_error;

	/*
	 * Discharge control 1 register configuration
	 * [7:2] : 00b Reserved
	 * [1:0] : 01b VCCIO discharge resistance (V4), 100 Ohm
	 */
	err = I2C_PMIC_WRITE(TPS650X30_REG_DISCHCNT1, 0x01);
	if (err)
		goto pmic_error;

	/*
	 * Increase Voltage
	 *  [7:0] : 0x2a default
	 *  [5:4] : 10b default
	 *  [5:4] : 01b 5.1V (0x1a)
	 */
	err = I2C_PMIC_WRITE(TPS650X30_REG_V5ADS3CNT, 0x1a);
	if (err)
		goto pmic_error;

	/*
	 * PBCONFIG Register configuration
	 *   [7] :      1b Power button debounce, 0ms (no debounce)
	 *   [6] :      0b Power button reset timer logic, no action (default)
	 * [5:0] : 011111b Force an Emergency reset time, 31s (default)
	 */
	err = I2C_PMIC_WRITE(TPS650X30_REG_PBCONFIG, 0x9F);
	if (err)
		goto pmic_error;

	/*
	 * V3.3A_DSW (VR3) control. Default: 0x2A.
	 * [7:6] : 00b Disabled
	 * [5:4] : 00b Vnom + 3%. (default: 10b 0%)
	 */
	err = I2C_PMIC_WRITE(TPS650X30_REG_V33ADSWCNT, 0x0A);
	if (err)
		goto pmic_error;

	CPRINTS("PMIC init done");
	pmic_initialized = 1;
	return;

pmic_error:
	CPRINTS("PMIC init failed");
}

void chipset_pre_init_callback(void)
{
	board_pmic_init();
}

/**
 * Notify PCH of the AC presence.
 */
static void board_extpower(void)
{
	gpio_set_level(GPIO_PCH_ACPRESENT, extpower_is_present());
}
DECLARE_HOOK(HOOK_AC_CHANGE, board_extpower, HOOK_PRIO_DEFAULT);

enum battery_present battery_is_present(void)
{
	return BP_NO;
}

int64_t get_time_dsw_pwrok(void)
{
	/* DSW_PWROK is turned on before EC was powered. */
	return -20 * MSEC;
}

const struct pwm_t pwm_channels[] = {
	[PWM_CH_LED_RED]  = { 3, PWM_CONFIG_DSLEEP, 100 },
	[PWM_CH_LED_BLUE] = { 5, PWM_CONFIG_DSLEEP, 100 },
	[PWM_CH_FAN] = {4, PWM_CONFIG_OPEN_DRAIN, 25000},
};
BUILD_ASSERT(ARRAY_SIZE(pwm_channels) == PWM_CH_COUNT);

struct fan_step {
	int on;
	int off;
	int rpm;
};

/* Note: Do not make the fan on/off point equal to 0 or 100 */
static const struct fan_step fan_table0[] = {
	{.on =  0, .off =  5, .rpm = 0},
	{.on = 30, .off =  5, .rpm = 2180},
	{.on = 49, .off = 46, .rpm = 2680},
	{.on = 53, .off = 50, .rpm = 3300},
	{.on = 58, .off = 54, .rpm = 3760},
	{.on = 63, .off = 59, .rpm = 4220},
	{.on = 68, .off = 64, .rpm = 4660},
	{.on = 75, .off = 70, .rpm = 4900},
};
/* All fan tables must have the same number of levels */
#define NUM_FAN_LEVELS ARRAY_SIZE(fan_table0)

static const struct fan_step *fan_table = fan_table0;


static void cbi_init(void)
{
	uint32_t val;
	if (cbi_get_board_version(&val) == EC_SUCCESS && val <= UINT8_MAX)
		board_version = val;
	CPRINTS("Board Version: 0x%02x", board_version);

	if (cbi_get_oem_id(&val) == EC_SUCCESS && val < OEM_COUNT)
		oem = val;
	CPRINTS("OEM: %d", oem);

	if (cbi_get_sku_id(&val) == EC_SUCCESS)
		sku = val;
	CPRINTS("SKU: 0x%08x", sku);
}
DECLARE_HOOK(HOOK_INIT, cbi_init, HOOK_PRIO_INIT_I2C + 1);

static void setup_bj(void)
{
	enum bj_adapter bj = (BJ_ADAPTER_135W_MASK & (1 << sku)) ?
			BJ_135W_19V : BJ_90W_19V;
	gpio_set_level(GPIO_U22_90W, bj == BJ_90W_19V);
}

static void board_init(void)
{
	setup_bj();

	board_extpower();

	gpio_enable_interrupt(GPIO_USB_C0_VBUS_WAKE_L);
}
DECLARE_HOOK(HOOK_INIT, board_init, HOOK_PRIO_DEFAULT);

int fan_percent_to_rpm(int fan, int pct)
{
	static int current_level;
	static int previous_pct;
	int i;

	/*
	 * Compare the pct and previous pct, we have the three paths :
	 *  1. decreasing path. (check the off point)
	 *  2. increasing path. (check the on point)
	 *  3. invariant path. (return the current RPM)
	 */
	if (pct < previous_pct) {
		for (i = current_level; i >= 0; i--) {
			if (pct <= fan_table[i].off)
				current_level = i - 1;
			else
				break;
		}
	} else if (pct > previous_pct) {
		for (i = current_level + 1; i < NUM_FAN_LEVELS; i++) {
			if (pct >= fan_table[i].on)
				current_level = i;
			else
				break;
		}
	}

	if (current_level < 0)
		current_level = 0;

	previous_pct = pct;

	if (fan_table[current_level].rpm !=
		fan_get_rpm_target(FAN_CH(fan)))
		cprintf(CC_THERMAL, "[%T Setting fan RPM to %d]\n",
			fan_table[current_level].rpm);

	return fan_table[current_level].rpm;
}

void board_rtc_reset(void)
{
	CPRINTS("Asserting RTCRST# to PCH");
	gpio_set_level(GPIO_PCH_RTCRST, 1);
	udelay(100);
	gpio_set_level(GPIO_PCH_RTCRST, 0);
}
