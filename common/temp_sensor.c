/* Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* Temperature sensor module for Chrome EC */

#include "chip_temp_sensor.h"
#include "chipset.h"
#include "common.h"
#include "console.h"
#include "gpio.h"
#include "i2c.h"
#include "host_command.h"
#include "peci.h"
#include "task.h"
#include "temp_sensor.h"
#include "thermal.h"
#include "timer.h"
#include "tmp006.h"
#include "util.h"

/* Defined in board_temp_sensor.c. Must be in the same order as
 * in enum temp_sensor_id.
 */
extern const struct temp_sensor_t temp_sensors[TEMP_SENSOR_COUNT];

int temp_sensor_read(enum temp_sensor_id id, int *temp_ptr)
{
	const struct temp_sensor_t *sensor;

	if (id < 0 || id >= TEMP_SENSOR_COUNT)
		return EC_ERROR_INVAL;
	sensor = temp_sensors + id;

	return sensor->read(sensor->idx, temp_ptr);
}

void poll_slow_sensors(void)
{
	/* Poll every second */
#ifdef CONFIG_TMP006
	tmp006_poll();
#endif
#ifdef CHIP_lm4
	chip_temp_sensor_poll();
#endif
}

static void poll_fast_sensors(void)
{
	/* Poll every 1/4 second */
#ifdef CONFIG_PECI
	peci_temp_sensor_poll();
#endif
}

static void update_mapped_memory(void)
{
	int i, t;
	uint8_t *mptr = host_get_memmap(EC_MEMMAP_TEMP_SENSOR);

	for (i = 0; i < TEMP_SENSOR_COUNT; i++, mptr++) {
		/*
		 * Switch to second range if first one is full, or stop if
		 * second range is also full.
		 */
		if (i == EC_TEMP_SENSOR_ENTRIES)
			mptr = host_get_memmap(EC_MEMMAP_TEMP_SENSOR_B);
		else if (i >= EC_TEMP_SENSOR_ENTRIES +
			 EC_TEMP_SENSOR_B_ENTRIES)
			break;

		switch (temp_sensor_read(i, &t)) {
		case EC_ERROR_NOT_POWERED:
			*mptr = EC_TEMP_SENSOR_NOT_POWERED;
			break;
		case EC_SUCCESS:
			*mptr = t - EC_TEMP_SENSOR_OFFSET;
			break;
		default:
			*mptr = EC_TEMP_SENSOR_ERROR;
		}
	}
}

void temp_sensor_task(void)
{
	int i;
	uint8_t *base, *base_b;

	/*
	 * Initialize memory-mapped data. We initialize valid sensors to 23 C
	 * so that if a temperature value is read before we actually poll the
	 * sensors, we don't end up with an insane value.
	 */
	base = host_get_memmap(EC_MEMMAP_TEMP_SENSOR);
	base_b = host_get_memmap(EC_MEMMAP_TEMP_SENSOR_B);
	for (i = 0; i < TEMP_SENSOR_COUNT; ++i) {
		if (i < EC_TEMP_SENSOR_ENTRIES)
			base[i] = 0x60; /* 23 C */
		else
			base_b[i - EC_TEMP_SENSOR_ENTRIES] = 0x60; /* 23 C */
	}

	/* Set the rest of memory region to SENSOR_NOT_PRESENT */
	for (; i < EC_TEMP_SENSOR_ENTRIES + EC_TEMP_SENSOR_B_ENTRIES; ++i) {
		if (i < EC_TEMP_SENSOR_ENTRIES)
			base[i] = EC_TEMP_SENSOR_NOT_PRESENT;
		else
			base_b[i - EC_TEMP_SENSOR_ENTRIES] =
				EC_TEMP_SENSOR_NOT_PRESENT;
	}

	/* Temp sensor data is present, with B range supported. */
	*host_get_memmap(EC_MEMMAP_THERMAL_VERSION) = 2;

	while (1) {
		for (i = 0; i < 4; ++i) {
			usleep(250000);
			poll_fast_sensors();
		}
		poll_slow_sensors();
		update_mapped_memory();
	}
}

/*****************************************************************************/
/* Console commands */

static int command_temps(int argc, char **argv)
{
	int t, i;
	int rv, rv1 = EC_SUCCESS;

	for (i = 0; i < TEMP_SENSOR_COUNT; ++i) {
		ccprintf("  %-20s: ", temp_sensors[i].name);
		rv = temp_sensor_read(i, &t);
		if (rv) {
			ccprintf("Error %d\n", rv);
			rv1 = rv;
		} else
			ccprintf("%d K = %d C\n", t, t - 273);
	}

	return rv1;
}
DECLARE_CONSOLE_COMMAND(temps, command_temps,
			NULL,
			"Print temp sensors",
			NULL);
