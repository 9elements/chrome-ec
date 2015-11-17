/* Copyright 2015 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Power/Battery LED control for Edgar
 */

#include "charge_state.h"
#include "chipset.h"
#include "console.h"
#include "extpower.h"
#include "gpio.h"
#include "hooks.h"
#include "led_common.h"
#include "pwm.h"
#include "registers.h"
#include "util.h"

#define CPRINTF(format, args...) cprintf(CC_PWM, format, ## args)
#define CPRINTS(format, args...) cprints(CC_PWM, format, ## args)

#define LED_TOTAL_SECS 4
#define LED_ON_SECS 1

static int led_debug;

const enum ec_led_id supported_led_ids[] = {
	EC_LED_ID_POWER_LED, EC_LED_ID_BATTERY_LED};
const int supported_led_ids_count = ARRAY_SIZE(supported_led_ids);

enum led_color {
	LED_OFF = 0,
	LED_BLUE,
	LED_ORANGE,

	/* Number of colors, not a color itself */
	LED_COLOR_COUNT
};

static int bat_led_set_color(enum led_color color)
{
	switch (color) {
	case LED_OFF:
		gpio_set_level(GPIO_BAT_LED_BLUE, 1);
		gpio_set_level(GPIO_BAT_LED_ORANGE, 1);
		break;
	case LED_BLUE:
		gpio_set_level(GPIO_BAT_LED_BLUE, 0);
		gpio_set_level(GPIO_BAT_LED_ORANGE, 1);
		break;
	case LED_ORANGE:
		gpio_set_level(GPIO_BAT_LED_BLUE, 1);
		gpio_set_level(GPIO_BAT_LED_ORANGE, 0);
		break;
	default:
		return EC_ERROR_UNKNOWN;
	}
	return EC_SUCCESS;
}

static int pwr_led_set_color(enum led_color color)
{
	switch (color) {
	case LED_OFF:
		gpio_set_level(GPIO_PWR_LED_BLUE, 1);
		gpio_set_level(GPIO_PWR_LED_ORANGE, 1);
		break;
	case LED_BLUE:
		gpio_set_level(GPIO_PWR_LED_BLUE, 0);
		gpio_set_level(GPIO_PWR_LED_ORANGE, 1);
		break;
	case LED_ORANGE:
		gpio_set_level(GPIO_PWR_LED_BLUE, 1);
		gpio_set_level(GPIO_PWR_LED_ORANGE, 0);
		break;
	default:
		return EC_ERROR_UNKNOWN;
	}
	return EC_SUCCESS;
}

void led_get_brightness_range(enum ec_led_id led_id, uint8_t *brightness_range)
{
	brightness_range[EC_LED_COLOR_BLUE] = 1;
}

int led_set_brightness(enum ec_led_id led_id, const uint8_t *brightness)
{
	gpio_set_level(GPIO_PWR_LED_BLUE, brightness[EC_LED_COLOR_BLUE]);
	return EC_SUCCESS;
}

static void edgar_led_set_power(void)
{
	static int power_secs;

	power_secs++;

	/* PWR LED behavior:
	 * Power on: Blue
	 * Suspend: Blue in breeze mode (1 sec on/ 3 sec off)
	 * Power off: OFF
	 */
	if (chipset_in_state(CHIPSET_STATE_ANY_OFF))
		pwr_led_set_color(LED_OFF);
	else if (chipset_in_state(CHIPSET_STATE_ON))
		pwr_led_set_color(LED_BLUE);
	else if (chipset_in_state(CHIPSET_STATE_SUSPEND))
		pwr_led_set_color((power_secs % LED_TOTAL_SECS) < LED_ON_SECS
			? LED_ORANGE : LED_OFF);
}

static void edgar_led_set_battery(void)
{
	static int battery_secs;

	battery_secs++;

	/* BAT LED behavior:
	 * Fully charged / idle: Blue
	 * Force idle (for factory): 2 secs of blue, 2 secs of orange
	 * Under charging: Orange
	 * Battery low (10%): Orange in breeze mode (1 sec on, 3 sec off)
	 * Battery critical low (less than 3%) or abnormal battery
	 *     situation: Orange in blinking mode (1 sec on, 1 sec off)
	 * Using battery or not connected to AC power: OFF
	 */
	switch (charge_get_state()) {
	case PWR_STATE_CHARGE:
		bat_led_set_color(LED_ORANGE);
		break;
	case PWR_STATE_DISCHARGE:
		if (charge_get_percent() < 3)
			bat_led_set_color(
				(battery_secs % 2) < LED_ON_SECS
				? LED_ORANGE : LED_OFF);
		else if (charge_get_percent() < 10)
			bat_led_set_color(
				(battery_secs % LED_TOTAL_SECS) < LED_ON_SECS
				? LED_ORANGE : LED_OFF);
		else
			bat_led_set_color(LED_OFF);
		break;
	case PWR_STATE_ERROR:
		bat_led_set_color(
			(battery_secs % 2) < LED_ON_SECS
			? LED_ORANGE : LED_OFF);
		break;
	case PWR_STATE_CHARGE_NEAR_FULL:
		bat_led_set_color(LED_BLUE);
		break;
	case PWR_STATE_IDLE: /* External power connected in IDLE. */
		if (charge_get_flags() & CHARGE_FLAG_FORCE_IDLE)
			bat_led_set_color(
				(battery_secs % LED_TOTAL_SECS) < 2
				? LED_BLUE : LED_ORANGE);
		else
			bat_led_set_color(LED_BLUE);
		break;
	default:
		/* Other states don't alter LED behavior */
		break;
	}
}

static void led_init(void)
{
	bat_led_set_color(LED_OFF);
	pwr_led_set_color(LED_OFF);
}
DECLARE_HOOK(HOOK_INIT, led_init, HOOK_PRIO_DEFAULT);

/**
 * Called by hook task every 1s
 */
static void led_sec(void)
{
	if (led_debug)
		return;

	if (led_auto_control_is_enabled(EC_LED_ID_BATTERY_LED))
		edgar_led_set_battery();
	else
		bat_led_set_color(LED_OFF);

	if (led_auto_control_is_enabled(EC_LED_ID_POWER_LED))
		edgar_led_set_power();
	else
		pwr_led_set_color(LED_OFF);
}
DECLARE_HOOK(HOOK_SECOND, led_sec, HOOK_PRIO_DEFAULT);

static void dump_pwm_channels(void)
{
	int ch;
	for (ch = 0; ch < 4; ch++) {
		CPRINTF("channel = %d\n", ch);
		CPRINTF("0x%04X 0x%04X 0x%04X\n",
			MEC1322_PWM_CFG(ch),
			MEC1322_PWM_ON(ch),
			MEC1322_PWM_OFF(ch));
	}
}
/******************************************************************/
/* Console commands */
static int command_led_color(int argc, char **argv)
{
	if (argc > 1) {
		if (!strcasecmp(argv[1], "debug")) {
			led_debug ^= 1;
			CPRINTF("led_debug = %d\n", led_debug);
		} else if (!strcasecmp(argv[1], "bat_off")) {
			bat_led_set_color(LED_OFF);
		} else if (!strcasecmp(argv[1], "bat_blue")) {
			bat_led_set_color(LED_BLUE);
		} else if (!strcasecmp(argv[1], "bat_orange")) {
			bat_led_set_color(LED_ORANGE);
		} else if (!strcasecmp(argv[1], "pwr_off")) {
			pwr_led_set_color(LED_OFF);
		} else if (!strcasecmp(argv[1], "pwr_blue")) {
			pwr_led_set_color(LED_BLUE);
		} else if (!strcasecmp(argv[1], "pwr_orange")) {
			pwr_led_set_color(LED_ORANGE);
		} else {
			/* maybe handle charger_discharge_on_ac() too? */
			return EC_ERROR_PARAM1;
		}
	}

	if (led_debug == 1)
		dump_pwm_channels();
	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(ledcolor, command_led_color,
			"[debug|bat_off|bat_blue|bat_orange|pwr_off|pwr_blue|pwr_orange]",
			"Change LED color",
			NULL);

