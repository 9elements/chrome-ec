/* Copyright 2015 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
/* Setzer board-specific configuration */

#include "adc.h"
#include "adc_chip.h"
#include "als.h"
#include "button.h"
#include "console.h"
#include "charger.h"
#include "charge_state.h"
#include "driver/accel_kxcj9.h"
#include "driver/als_isl29035.h"
#include "driver/gyro_l3gd20h.h"
#include "driver/temp_sensor/tmp432.h"
#include "driver/charger/bq24773.h"
#include "extpower.h"
#include "gpio.h"
#include "hooks.h"
#include "host_command.h"
#include "i2c.h"
#include "lid_switch.h"
#include "math_util.h"
#include "motion_lid.h"
#include "motion_sense.h"
#include "power.h"
#include "power_button.h"
#include "pwm.h"
#include "pwm_chip.h"
#include "registers.h"
#include "temp_sensor.h"
#include "temp_sensor_chip.h"
#include "thermal.h"
#include "uart.h"
#include "util.h"

#define GPIO_KB_INPUT (GPIO_INPUT | GPIO_PULL_UP)
#define GPIO_KB_OUTPUT (GPIO_ODR_HIGH)
#define GPIO_KB_OUTPUT_COL2 (GPIO_OUT_LOW)
#define CHARGER_TIMEOUT_SEC 36000

#include "gpio_list.h"

static int prev_stats;
static int charge_timeout_secs;
int state_charger_timeout;

/* Console output macros */
#define CPRINTS(format, args...) cprints(CC_CHARGER, format, ## args)

/* power signal list.  Must match order of enum power_signal. */
const struct power_signal_info power_signal_list[] = {
	{GPIO_ALL_SYS_PGOOD,     1, "ALL_SYS_PWRGD"},
	{GPIO_RSMRST_L_PGOOD,    1, "RSMRST_N_PWRGD"},
	{GPIO_PCH_SLP_S3_L,      1, "SLP_S3#_DEASSERTED"},
	{GPIO_PCH_SLP_S4_L,      1, "SLP_S4#_DEASSERTED"},
};
BUILD_ASSERT(ARRAY_SIZE(power_signal_list) == POWER_SIGNAL_COUNT);

const struct i2c_port_t i2c_ports[]  = {
	{"batt_chg", MEC1322_I2C0_0, 100,
		GPIO_I2C_PORT0_0_SCL, GPIO_I2C_PORT0_0_SDA},
	{"muxes", MEC1322_I2C0_1, 100,
		GPIO_I2C_PORT0_1_SCL, GPIO_I2C_PORT0_1_SDA},
	{"pd_mcu", MEC1322_I2C1, 1000,
		GPIO_I2C_PORT1_SCL, GPIO_I2C_PORT1_SDA},
	{"sensors", MEC1322_I2C2, 100,
		GPIO_I2C_PORT2_SCL, GPIO_I2C_PORT2_SDA},
	{"thermal", MEC1322_I2C3, 100,
		GPIO_I2C_PORT3_SCL, GPIO_I2C_PORT3_SDA}
};
const unsigned int i2c_ports_used = ARRAY_SIZE(i2c_ports);

const enum gpio_signal hibernate_wake_pins[] = {
	GPIO_POWER_BUTTON_L,
};

const int hibernate_wake_pins_used = ARRAY_SIZE(hibernate_wake_pins);

/*
 * Temperature sensors data; must be in same order as enum temp_sensor_id.
 * Sensor index and name must match those present in coreboot:
 *     src/mainboard/google/${board}/acpi/dptf.asl
 */
const struct temp_sensor_t temp_sensors[] = {
	{"TMP432_Internal", TEMP_SENSOR_TYPE_BOARD, tmp432_get_val,
		TMP432_IDX_LOCAL, 4},
	{"TMP432_Sensor_1", TEMP_SENSOR_TYPE_BOARD, tmp432_get_val,
		TMP432_IDX_REMOTE1, 4},
	{"TMP432_Sensor_2", TEMP_SENSOR_TYPE_BOARD, tmp432_get_val,
		TMP432_IDX_REMOTE2, 4},
	{"Battery", TEMP_SENSOR_TYPE_BATTERY, charge_temp_sensor_get_val,
		0, 4},
};
BUILD_ASSERT(ARRAY_SIZE(temp_sensors) == TEMP_SENSOR_COUNT);

/* ALS instances. Must be in same order as enum als_id. */
struct als_t als[] = {
	{"ISL", isl29035_read_lux, 5},
};
BUILD_ASSERT(ARRAY_SIZE(als) == ALS_COUNT);

/* Thermal limits for each temp sensor. All temps are in degrees K. Must be in
 * same order as enum temp_sensor_id. To always ignore any temp, use 0.
 */
struct ec_thermal_config thermal_params[] = {
	{{0, 0, 0}, 0, 0}, /* TMP432_Internal */
	{{0, 0, 0}, 0, 0}, /* TMP432_Sensor_1 */
	{{0, 0, 0}, 0, 0}, /* TMP432_Sensor_2 */
	{{0, 326, 332}, 0, 0}, /* Battery Sensor */
};
BUILD_ASSERT(ARRAY_SIZE(thermal_params) == TEMP_SENSOR_COUNT);

/* Four Motion sensors */
/* kxcj9 mutex and local/private data*/
static struct mutex g_kxcj9_mutex[2];
struct kxcj9_data g_kxcj9_data[2];

#ifdef CONFIG_GYRO_L3GD20H
/* Gyro sensor */
/* l3gd20h mutex and local/private data*/
static struct mutex g_l3gd20h_mutex;
struct l3gd20_data g_l3gd20h_data;
#endif

/* Matrix to rotate accelrator into standard reference frame */
const matrix_3x3_t base_standard_ref = {
	{ 0,  FLOAT_TO_FP(1),  0},
	{FLOAT_TO_FP(-1),  0,  0},
	{ 0,  0,  FLOAT_TO_FP(1)}
};

const matrix_3x3_t lid_standard_ref = {
	{FLOAT_TO_FP(1),  0,  0},
	{ 0, FLOAT_TO_FP(1),  0},
	{ 0,  0, FLOAT_TO_FP(1)}
};

struct motion_sensor_t motion_sensors[] = {
	{.name = "Base Accel",
	 .active_mask = SENSOR_ACTIVE_S0,
	 .chip = MOTIONSENSE_CHIP_KXCJ9,
	 .type = MOTIONSENSE_TYPE_ACCEL,
	 .location = MOTIONSENSE_LOC_BASE,
	 .drv = &kxcj9_drv,
	 .mutex = &g_kxcj9_mutex[0],
	 .drv_data = &g_kxcj9_data[0],
	 .i2c_addr = KXCJ9_ADDR1,
	 .rot_standard_ref = &base_standard_ref,
	 .default_config = {
		 .odr = 100000,
		 .range = 2,
		 .ec_rate = SUSPEND_SAMPLING_INTERVAL,
	 }
	},
	{.name = "Lid Accel",
	 .active_mask = SENSOR_ACTIVE_S0,
	 .chip = MOTIONSENSE_CHIP_KXCJ9,
	 .type = MOTIONSENSE_TYPE_ACCEL,
	 .location = MOTIONSENSE_LOC_LID,
	 .drv = &kxcj9_drv,
	 .mutex = &g_kxcj9_mutex[1],
	 .drv_data = &g_kxcj9_data[1],
	 .i2c_addr = KXCJ9_ADDR0,
	 .rot_standard_ref = &lid_standard_ref,
	 .default_config = {
		 .odr = 100000,
		 .range = 2,
		 .ec_rate = SUSPEND_SAMPLING_INTERVAL,
	 }
	},
#ifdef CONFIG_GYRO_L3GD20H
	{.name = "Lid Gyro",
	 .active_mask = SENSOR_ACTIVE_S0,
	 .chip = MOTIONSENSE_CHIP_L3GD20H,
	 .type = MOTIONSENSE_TYPE_GYRO,
	 .location = MOTIONSENSE_LOC_LID,
	 .drv = &l3gd20h_drv,
	 .mutex = &g_l3gd20h_mutex,
	 .drv_data = &g_l3gd20h_data,
	 .i2c_addr = L3GD20_ADDR1,
	 .rot_standard_ref = NULL,
	 .default_config = {
		 .odr = 190000,
		 .range = 2000,
		 .ec_rate = SUSPEND_SAMPLING_INTERVAL,
	 }
	},
#endif
};
const unsigned int motion_sensor_count = ARRAY_SIZE(motion_sensors);

/* Define the accelerometer orientation matrices. */
const struct accel_orientation acc_orient = {
	/* Hinge aligns with x axis. */
	.rot_hinge_90 = {
		{ FLOAT_TO_FP(1),  0,  0},
		{ 0,  0,  FLOAT_TO_FP(1)},
		{ 0, FLOAT_TO_FP(-1),  0}
	},
	.rot_hinge_180 = {
		{ FLOAT_TO_FP(1),  0,  0},
		{ 0, FLOAT_TO_FP(-1),  0},
		{ 0,  0, FLOAT_TO_FP(-1)}
	},
	.hinge_axis = {1, 0, 0},
};

/*
 * In S3, power rail for sensors (+V3p3S) goes down asynchronous to EC. We need
 * to execute this routine first and set the sensor state to "Not Initialized".
 * This prevents the motion_sense_suspend hook routine from communicating with
 * the sensor.
 */
static void motion_sensors_pre_init(void)
{
	struct motion_sensor_t *sensor;
	int i;

	for (i = 0; i < motion_sensor_count; ++i) {
		sensor = &motion_sensors[i];
		sensor->state = SENSOR_NOT_INITIALIZED;

		sensor->runtime_config.odr = sensor->default_config.odr;
		sensor->runtime_config.range = sensor->default_config.range;
	}
}
DECLARE_HOOK(HOOK_CHIPSET_SUSPEND, motion_sensors_pre_init,
	MOTION_SENSE_HOOK_PRIO - 1);

/* init ADC ports to avoid floating state due to thermistors */
static void adc_pre_init(void)
{
       /* Configure GPIOs */
	gpio_config_module(MODULE_ADC, 1);
}
DECLARE_HOOK(HOOK_INIT, adc_pre_init, HOOK_PRIO_INIT_ADC - 1);

/* ADC channels */
const struct adc_t adc_channels[] = {
	/*
	 * We have 0.01-ohm resistors, and IOUT is 40X the differential
	 * voltage, so 1000mA ==> 400mV.
	 * ADC returns 0x000-0xFFF, which maps to 0.0-3.0V (as configured).
	 * mA = 1000 * ADC_VALUE / ADC_READ_MAX * 3000 / 400
	*/
	[ADC_CH_CHARGER_CURRENT] = {"ChargerCurrent", 3000 * 10,
	 ADC_READ_MAX * 4, 0, MEC1322_ADC_CH(2)},
	[ADC_AC_ADAPTER_ID_VOLTAGE] = {"AdapterIDVoltage", 3000,
	 ADC_READ_MAX, 0, MEC1322_ADC_CH(3)},
};
BUILD_ASSERT(ARRAY_SIZE(adc_channels) == ADC_CH_COUNT);

int i2c_port_is_smbus(int port)
{
	return (port == MEC1322_I2C0_0 || port == MEC1322_I2C0_1) ? 1 : 0;
}

int board_charger_post_init(void)
{
	int ret, option0, option1;

	ret = raw_read16(REG_CHARGE_OPTION0, &option0);
	if (ret)
		return ret;

	option0 |= OPTION0_AUDIO_FREQ_40KHZ_LIMIT;
	ret = raw_write16(REG_CHARGE_OPTION0, option0);
	if (ret)
		return ret;

	ret = raw_read16(REG_CHARGE_OPTION1, &option1);
	if (ret)
		return ret;

	option1 |= OPTION1_PMON_ENABLE;
	return raw_write16(REG_CHARGE_OPTION1, option1);
}

static void touch_screen_set_control_mode(void)
{
	/*
	* If lid is closed; hold touchscreen in reset to cut power
	* usage.  If lid is open, take touchscreen out of reset so it
	* can wake the processor.
	*/
	gpio_set_level(GPIO_TOUCHSCREEN_RESET_L, lid_is_open());
}
DECLARE_HOOK(HOOK_LID_CHANGE, touch_screen_set_control_mode, HOOK_PRIO_DEFAULT);
DECLARE_HOOK(HOOK_CHIPSET_RESUME, touch_screen_set_control_mode,
	HOOK_PRIO_DEFAULT);

static void touch_screen_reset(void)
{
	/* Hold touchscreen in reset */
	gpio_set_level(GPIO_TOUCHSCREEN_RESET_L, 0);
}
DECLARE_HOOK(HOOK_CHIPSET_SUSPEND, touch_screen_reset, HOOK_PRIO_DEFAULT);

/* Called by hook task every 1 sec  */
void check_charger_timeout_second(void)
{
	if (charge_get_state() != prev_stats) {
		if (charge_get_state() == PWR_STATE_CHARGE)
			charge_timeout_secs = CHARGER_TIMEOUT_SEC;
		prev_stats = charge_get_state();
	}
	if (charge_get_state() == PWR_STATE_CHARGE) {
		charge_timeout_secs--;
		if (charge_timeout_secs == 0) {
			state_charger_timeout = 1;
			charge_set_input_current_limit(128);
			CPRINTS("Charge timed out after %d hours",
				(CHARGER_TIMEOUT_SEC / 3600));
		}
	} else {
		state_charger_timeout = 0;
	}
}
DECLARE_HOOK(HOOK_SECOND, check_charger_timeout_second, HOOK_PRIO_DEFAULT);

void board_rtc_reset(void)
{
	gpio_set_level(GPIO_PCH_SYS_PWROK, 0);
	gpio_set_level(GPIO_PCH_RSMRST_L, 0);
	/*
	 * Assert RTCRST# to the PCH long enough for it to latch the
	 * assertion and reset the internal RTC backed state.
	 */
	CPRINTS("Asserting RTCRST# to PCH");
	gpio_set_level(GPIO_PCH_RTCRST, 1);
	usleep(3 * SECOND);
	gpio_set_level(GPIO_PCH_RTCRST, 0);
	udelay(10 * MSEC);
}
