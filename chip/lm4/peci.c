/* Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* PECI interface for Chrome EC */

#include "board.h"
#include "clock.h"
#include "console.h"
#include "gpio.h"
#include "hooks.h"
#include "peci.h"
#include "registers.h"
#include "temp_sensor.h"
#include "util.h"

/* Max junction temperature for processor in degrees C */
/* TODO: read TjMax from processor via PECI */
#define PECI_TJMAX 105

/* Initial PECI baud rate */
#define PECI_BAUD_RATE 100000

/* Polling interval for PECI, in ms */
#define PECI_POLL_INTERVAL_MS 250

/* Internal and external path delays, in ns */
#define PECI_TD_FET_NS 60  /* Guess; TODO: what is real delay */
#define PECI_TD_INT_NS 80

/* Number of controller retries. Should be between 0 and 7. */
#define PECI_RETRY_COUNT 4

/* Timing negotiation error bypass. 1 = on. 0 = off. */
#define PECI_ERROR_BYPASS 1

#define TEMP_AVG_LENGTH 4  /* Should be power of 2 */
static int temp_vals[TEMP_AVG_LENGTH];
static int temp_idx = 0;

/* Configures the GPIOs for the PECI module. */
static void configure_gpios(void)
{
	/* PJ6 alternate function 1 = PECI Tx */
	gpio_set_alternate_function(LM4_GPIO_J, 0x40, 1);

	/* PJ7 analog input = PECI Rx (comparator) */
	LM4_GPIO_DEN(LM4_GPIO_J) &= ~0x80;
}


int peci_get_cpu_temp(void)
{
	int v = LM4_PECI_M0D0 & 0xffff;

	if (v >= 0x8000 && v <= 0x8fff)
		return -1;

	return v >> 6;
}


int peci_temp_sensor_poll(void)
{
	temp_vals[temp_idx] = peci_get_cpu_temp();
	temp_idx = (temp_idx + 1) & (TEMP_AVG_LENGTH - 1);
	return EC_SUCCESS;
}


int peci_temp_sensor_get_val(int idx)
{
	int sum = 0;
	int success_cnt = 0;
	int i;

	for (i = 0; i < TEMP_AVG_LENGTH; ++i) {
		if (temp_vals[i] >= 0) {
			success_cnt++;
			sum += temp_vals[i];
		}
	}

	if (success_cnt)
		return sum / success_cnt;
	else
		return -1;
}


static int peci_freq_changed(void)
{
	int freq = clock_get_freq();
	int baud;

	/* Disable polling while reconfiguring */
	LM4_PECI_CTL = 0;

	/* Calculate baud setting from desired rate, compensating for internal
	 * and external delays. */
	baud = freq / (4 * PECI_BAUD_RATE) - 2;
	baud -= (freq / 1000000) * (PECI_TD_FET_NS + PECI_TD_INT_NS) / 1000;

	/* Set baud rate and polling rate */
	LM4_PECI_DIV = (baud << 16) |
		(PECI_POLL_INTERVAL_MS * (freq / 1000 / 4096));

	/* Set up temperature monitoring to report in degrees K */
	LM4_PECI_CTL = ((PECI_TJMAX + 273) << 22) | 0x0001 |
		       (PECI_RETRY_COUNT << 12) |
		       (PECI_ERROR_BYPASS << 11);

	return EC_SUCCESS;
}
DECLARE_HOOK(HOOK_FREQ_CHANGE, peci_freq_changed, HOOK_PRIO_DEFAULT - 1);

/*****************************************************************************/
/* Console commands */

static int command_peci_temp(int argc, char **argv)
{
	int t = peci_get_cpu_temp();
	if (t == -1) {
		ccprintf("PECI error 0x%04x\n", LM4_PECI_M0D0 & 0xffff);
		return EC_ERROR_UNKNOWN;
	}
	ccprintf("CPU temp = %d K = %d C\n", t, t - 273);
	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(pecitemp, command_peci_temp,
			NULL,
			"Print CPU temperature",
			NULL);

/*****************************************************************************/
/* Initialization */

static int peci_init(void)
{
	volatile uint32_t scratch  __attribute__((unused));
	int i;

	/* Enable the PECI module and delay a few clocks */
	LM4_SYSTEM_RCGCPECI = 1;
	scratch = LM4_SYSTEM_RCGCPECI;

	/* Configure GPIOs */
	configure_gpios();

	/* Set initial clock frequency */
	peci_freq_changed();

	/* Initialize temperature reading buffer to a sane value. */
	for (i = 0; i < TEMP_AVG_LENGTH; ++i)
		temp_vals[i] = 300; /* 27 C */

	return EC_SUCCESS;
}
DECLARE_HOOK(HOOK_INIT, peci_init, HOOK_PRIO_DEFAULT);
