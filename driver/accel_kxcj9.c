/* Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* KXCJ9 gsensor module for Chrome EC */

#include "accelerometer.h"
#include "common.h"
#include "console.h"
#include "driver/accel_kxcj9.h"
#include "gpio.h"
#include "i2c.h"
#include "timer.h"
#include "util.h"

#define CPUTS(outstr) cputs(CC_ACCEL, outstr)
#define CPRINTF(format, args...) cprintf(CC_ACCEL, format, ## args)

/* Range of the accelerometers: 2G, 4G, or 8G. */
static int sensor_range[ACCEL_COUNT] = {KXCJ9_GSEL_2G, KXCJ9_GSEL_2G};

/* Resolution: KXCJ9_RES_12BIT or KXCJ9_RES_12BIT. */
static int sensor_resolution[ACCEL_COUNT] = {KXCJ9_RES_12BIT, KXCJ9_RES_12BIT};

/* Output data rate: KXCJ9_OSA_* ranges from 0.781Hz to 1600Hz. */
static int sensor_datarate[ACCEL_COUNT] = {KXCJ9_OSA_100_0HZ,
						KXCJ9_OSA_100_0HZ};

/**
 * Read register from accelerometer.
 */
static int raw_read8(const int addr, const int reg, int *data_ptr)
{
	return i2c_read8(I2C_PORT_ACCEL, addr, reg, data_ptr);
}

/**
 * Write register from accelerometer.
 */
static int raw_write8(const int addr, const int reg, int data)
{
	return i2c_write8(I2C_PORT_ACCEL, addr, reg, data);
}


int accel_write_range(const enum accel_id id, const int range)
{
	int ret;

	/* Check for valid id. */
	if (id < 0 || id >= ACCEL_COUNT)
		return EC_ERROR_INVAL;

	/*
	 * Verify that the input range is valid. Note that we currently
	 * don't support the 8G with 14-bit resolution mode.
	 */
	if (range != KXCJ9_GSEL_2G && range != KXCJ9_GSEL_4G &&
		range != KXCJ9_GSEL_8G)
		return EC_ERROR_INVAL;

	ret = raw_write8(accel_addr[id],  KXCJ9_CTRL1,
			KXCJ9_CTRL1_PC1 | sensor_resolution[id] | range);

	/* If successfully written, then save the range. */
	if (ret == EC_SUCCESS)
		sensor_range[id] = range;

	return ret;
}

int accel_write_resolution(const enum accel_id id, const int res)
{
	int ret;

	/* Check for valid id. */
	if (id < 0 || id >= ACCEL_COUNT)
		return EC_ERROR_INVAL;

	/* Check that resolution input is valid. */
	if (res != KXCJ9_RES_12BIT && res != KXCJ9_RES_8BIT)
		return EC_ERROR_INVAL;

	ret = raw_write8(accel_addr[id],  KXCJ9_CTRL1,
			KXCJ9_CTRL1_PC1 | res | sensor_range[id]);

	/* If successfully written, then save the range. */
	if (ret == EC_SUCCESS)
		sensor_resolution[id] = res;

	return ret;
}

int accel_write_datarate(const enum accel_id id, const int rate)
{
	int ret;

	/* Check for valid id. */
	if (id < 0 || id >= ACCEL_COUNT)
		return EC_ERROR_INVAL;

	/* Check that rate input is valid. */
	if (rate < KXCJ9_OSA_12_50HZ || rate > KXCJ9_OSA_6_250HZ)
		return EC_ERROR_INVAL;

	/* Set output data rate. */
	ret = raw_write8(accel_addr[id],  KXCJ9_DATA_CTRL, rate);

	/* If successfully written, then save the range. */
	if (ret == EC_SUCCESS)
		sensor_datarate[id] = rate;

	return ret;
}

#ifdef CONFIG_ACCEL_INTERRUPTS
int accel_set_interrupt(const enum accel_id id, unsigned int threshold)
{
	int ctrl1, tmp, ret;

	/*
	 * TODO(crosbug.com/p/26884): This driver currently assumes only one
	 * task can call this function. If this isn't true anymore, need to
	 * protect with a mutex.
	 */

	/*
	 * Read the current status of the control register and disable the
	 * sensor to allow for changing of critical parameters.
	 */
	ret = raw_read8(accel_addr[id], KXCJ9_CTRL1, &ctrl1);
	if (ret != EC_SUCCESS)
		return ret;
	ret = raw_write8(accel_addr[id], KXCJ9_CTRL1, ctrl1 & ~KXCJ9_CTRL1_PC1);
	if (ret != EC_SUCCESS)
		return ret;

	/* Set interrupt timer to 1 so it wakes up immediately. */
	ret = raw_write8(accel_addr[id], KXCJ9_WAKEUP_TIMER, 1);
	if (ret != EC_SUCCESS)
		goto error_enable_sensor;

	/*
	 * Set threshold, note threshold register is in units of 16 counts, so
	 * first we need to divide by 16 to get the value to send.
	 */
	threshold >>= 4;
	ret = raw_write8(accel_addr[id], KXCJ9_WAKEUP_THRESHOLD, threshold);
	if (ret != EC_SUCCESS)
		goto error_enable_sensor;

	/*
	 * Set interrupt enable register on sensor. Note that once this
	 * function is called once, the interrupt stays enabled and it is
	 * only necessary to clear KXCJ9_INT_REL to allow the next interrupt.
	 */
	ret = raw_read8(accel_addr[id], KXCJ9_INT_CTRL1, &tmp);
	if (ret != EC_SUCCESS)
		goto error_enable_sensor;
	if (!(tmp & KXCJ9_INT_CTRL1_IEN)) {
		ret = raw_write8(accel_addr[id], KXCJ9_INT_CTRL1,
				tmp | KXCJ9_INT_CTRL1_IEN);
		if (ret != EC_SUCCESS)
			goto error_enable_sensor;
	}

	/*
	 * Clear any pending interrupt on sensor by reading INT_REL register.
	 * Note: this register latches motion detected above threshold. Once
	 * latched, no interrupt can occur until this register is cleared.
	 */
	ret = raw_read8(accel_addr[id], KXCJ9_INT_REL, &tmp);

error_enable_sensor:
	/* Re-enable accelerometer. */
	if (raw_write8(accel_addr[id], KXCJ9_CTRL1, ctrl1 | KXCJ9_CTRL1_PC1) !=
			EC_SUCCESS) {
		/* Cannot re-enable accel, print warning and return an error. */
		CPRINTF("[%T Error trying to enable accelerometer %d]\n", id);
		return EC_ERROR_UNKNOWN;
	}

	return ret;
}
#endif

int accel_read(enum accel_id id, int *x_acc, int *y_acc, int *z_acc)
{
	uint8_t acc[6];
	uint8_t reg = KXCJ9_XOUT_L;
	int ret, multiplier;

	/* Check for valid id. */
	if (id < 0 || id >= ACCEL_COUNT)
		return EC_ERROR_INVAL;

	/* Read 6 bytes starting at KXCJ9_XOUT_L. */
	i2c_lock(I2C_PORT_ACCEL, 1);
	ret = i2c_xfer(I2C_PORT_ACCEL, accel_addr[id], &reg, 1, acc, 6,
			I2C_XFER_SINGLE);
	i2c_lock(I2C_PORT_ACCEL, 0);

	if (ret != EC_SUCCESS)
		return ret;

	/* Determine multiplier based on stored range. */
	switch (sensor_range[id]) {
	case KXCJ9_GSEL_2G:
		multiplier = 1;
		break;
	case KXCJ9_GSEL_4G:
		multiplier = 2;
		break;
	case KXCJ9_GSEL_8G:
		multiplier = 4;
		break;
	default:
		return EC_ERROR_UNKNOWN;
	}

	/*
	 * Convert acceleration to a signed 12-bit number. Note, based on
	 * the order of the registers:
	 *
	 * acc[0] = KXCJ9_XOUT_L
	 * acc[1] = KXCJ9_XOUT_H
	 * acc[2] = KXCJ9_YOUT_L
	 * acc[3] = KXCJ9_YOUT_H
	 * acc[4] = KXCJ9_ZOUT_L
	 * acc[5] = KXCJ9_ZOUT_H
	 */
	*x_acc = multiplier * (((int8_t)acc[1]) << 4) | (acc[0] >> 4);
	*y_acc = multiplier * (((int8_t)acc[3]) << 4) | (acc[2] >> 4);
	*z_acc = multiplier * (((int8_t)acc[5]) << 4) | (acc[4] >> 4);

	return EC_SUCCESS;
}

int accel_init(enum accel_id id)
{
	int ret = EC_SUCCESS;
	int cnt = 0, ctrl1, ctrl2;

	/* Check for valid id. */
	if (id < 0 || id >= ACCEL_COUNT)
		return EC_ERROR_INVAL;

	/*
	 * This sensor can be powered through an EC reboot, so the state of
	 * the sensor is unknown here. Initiate software reset to restore
	 * sensor to default.
	 */
	ret = raw_write8(accel_addr[id], KXCJ9_CTRL2, KXCJ9_CTRL2_SRST);
	if (ret != EC_SUCCESS)
		return ret;

	/* Wait until software reset is complete or timeout. */
	while (1) {
		ret = raw_read8(accel_addr[id], KXCJ9_CTRL2, &ctrl2);

		/* Reset complete. */
		if (ret == EC_SUCCESS && !(ctrl2 & KXCJ9_CTRL2_SRST))
			break;

		/* Check for timeout. */
		if (cnt++ > 5)
			return EC_ERROR_TIMEOUT;

		/* Give more time for reset action to complete. */
		msleep(10);
	}

#ifdef CONFIG_ACCEL_INTERRUPTS
	/* Set interrupt polarity to rising edge and keep interrupt disabled. */
	ret |= raw_write8(accel_addr[id], KXCJ9_INT_CTRL1, KXCJ9_INT_CTRL1_IEA);

	/* Set output data rate for wake-up interrupt function. */
	ret |= raw_write8(accel_addr[id], KXCJ9_CTRL2, KXCJ9_OWUF_100_0HZ);

	/* Set interrupt to trigger on motion on any axis. */
	ret |= raw_write8(accel_addr[id], KXCJ9_INT_CTRL2,
			KXCJ9_INT_SRC2_XNWU | KXCJ9_INT_SRC2_XPWU |
			KXCJ9_INT_SRC2_YNWU | KXCJ9_INT_SRC2_YPWU |
			KXCJ9_INT_SRC2_ZNWU | KXCJ9_INT_SRC2_ZPWU);

	/*
	 * Enable accel interrupts. Note: accels will not initiate an interrupt
	 * until interrupt enable bit in KXCJ9_INT_CTRL1 is set on the device.
	 */
	gpio_enable_interrupt(GPIO_ACCEL_INT_LID);
	gpio_enable_interrupt(GPIO_ACCEL_INT_BASE);
#endif

	/* Set output data rate. */
	ret |= raw_write8(accel_addr[id], KXCJ9_DATA_CTRL, sensor_datarate[id]);

	/* Set resolution and range and enable sensor. */
	ctrl1 = sensor_resolution[id] | sensor_range[id]  | KXCJ9_CTRL1_PC1;
#ifdef CONFIG_ACCEL_INTERRUPTS
	/* Enable wake up (motion detect) functionality. */
	ctrl1 |= KXCJ9_CTRL1_WUFE;
#endif
	ret = raw_write8(accel_addr[id], KXCJ9_CTRL1, ctrl1);

	return ret;
}



/*****************************************************************************/
/* Console commands */
#ifdef CONFIG_CMD_ACCELS
static int command_read_accelerometer(int argc, char **argv)
{
	char *e;
	int addr, reg, data;

	if (argc != 3)
		return EC_ERROR_PARAM_COUNT;

	/* First argument is address. */
	addr = strtoi(argv[1], &e, 0);
	if (*e)
		return EC_ERROR_PARAM1;

	/* Second argument is register offset. */
	reg = strtoi(argv[2], &e, 0);
	if (*e)
		return EC_ERROR_PARAM2;

	raw_read8(addr, reg, &data);

	ccprintf("0x%02x\n", data);

	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(accelread, command_read_accelerometer,
	"addr reg",
	"Read from accelerometer at slave address addr", NULL);

static int command_write_accelerometer(int argc, char **argv)
{
	char *e;
	int addr, reg, data;

	if (argc != 4)
		return EC_ERROR_PARAM_COUNT;

	/* First argument is address. */
	addr = strtoi(argv[1], &e, 0);
	if (*e)
		return EC_ERROR_PARAM1;

	/* Second argument is register offset. */
	reg = strtoi(argv[2], &e, 0);
	if (*e)
		return EC_ERROR_PARAM2;

	/* Third argument is data. */
	data = strtoi(argv[3], &e, 0);
	if (*e)
		return EC_ERROR_PARAM3;

	raw_write8(addr, reg, data);

	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(accelwrite, command_write_accelerometer,
	"addr reg data",
	"Write to accelerometer at slave address addr", NULL);

#ifdef CONFIG_ACCEL_INTERRUPTS
static int command_accelerometer_interrupt(int argc, char **argv)
{
	char *e;
	int id, thresh;

	if (argc != 3)
		return EC_ERROR_PARAM_COUNT;

	/* First argument is id. */
	id = strtoi(argv[1], &e, 0);
	if (*e || id < 0 || id >= ACCEL_COUNT)
		return EC_ERROR_PARAM1;

	/* Second argument is interrupt threshold. */
	thresh = strtoi(argv[2], &e, 0);
	if (*e)
		return EC_ERROR_PARAM2;

	accel_set_interrupt(id, thresh);

	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(accelint, command_accelerometer_interrupt,
	"id threshold",
	"Write interrupt threshold", NULL);
#endif /* CONFIG_ACCEL_INTERRUPTS */

#endif /* CONFIG_CMD_ACCELS */
