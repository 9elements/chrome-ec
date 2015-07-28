/* Copyright 2015 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* KX022 gsensor module for Chrome EC */

#include "accelgyro.h"
#include "common.h"
#include "console.h"
#include "driver/accel_kx022.h"
#include "gpio.h"
#include "i2c.h"
#include "task.h"
#include "timer.h"
#include "util.h"

#define CPUTS(outstr) cputs(CC_ACCEL, outstr)
#define CPRINTF(format, args...) cprintf(CC_ACCEL, format, ## args)

/* Number of times to attempt to enable sensor before giving up. */
#define SENSOR_ENABLE_ATTEMPTS 3

/*
 * Struct for pairing an engineering value with the register value for a
 * parameter.
 */
struct accel_param_pair {
	int val; /* Value in engineering units. */
	int reg; /* Corresponding register value. */
};

/* List of range values in +/-G's and their associated register values. */
static const struct accel_param_pair ranges[] = {
	{2, KX022_GSEL_2G},
	{4, KX022_GSEL_4G},
	{8, KX022_GSEL_8G}
};

/* List of resolution values in bits and their associated register values. */
static const struct accel_param_pair resolutions[] = {
	{8,  KX022_RES_8BIT},
	{12, KX022_RES_12BIT}
};

/* List of ODR values in mHz and their associated register values. */
static const struct accel_param_pair datarates[] = {
	{0,       KX022_OSA_0_781HZ},
	{781,     KX022_OSA_0_781HZ},
	{1563,    KX022_OSA_1_563HZ},
	{3125,    KX022_OSA_3_125HZ},
	{6250,    KX022_OSA_6_250HZ},
	{12500,   KX022_OSA_12_50HZ},
	{25000,   KX022_OSA_25_00HZ},
	{50000,   KX022_OSA_50_00HZ},
	{100000,  KX022_OSA_100_0HZ},
	{200000,  KX022_OSA_200_0HZ},
	{400000,  KX022_OSA_400_0HZ},
	{800000,  KX022_OSA_800_0HZ},
	{1600000, KX022_OSA_1600_HZ}
};

/**
 * Find index into a accel_param_pair that matches the given engineering value
 * passed in. The round_up flag is used to specify whether to round up or down.
 * Note, this function always returns a valid index. If the request is
 * outside the range of values, it returns the closest valid index.
 */
static int find_param_index(const int eng_val, const int round_up,
		const struct accel_param_pair *pairs, const int size)
{
	int i;

	/* Linear search for index to match. */
	for (i = 0; i < size - 1; i++) {
		if (eng_val <= pairs[i].val)
			return i;

		if (eng_val < pairs[i+1].val) {
			if (round_up)
				return i + 1;
			else
				return i;
		}
	}

	return i;
}

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

/**
 * Disable sensor by taking it out of operating mode. When disabled, the
 * acceleration data does not change.
 *
 * Note: This is intended to be called in a pair with enable_sensor().
 *
 * @data Pointer to motion sensor data
 * @ctrl1 Pointer to location to store KX022_CTRL1 register after disabling
 *
 * @return EC_SUCCESS if successful, EC_ERROR_* otherwise
 */
static int disable_sensor(const struct motion_sensor_t *s, int *ctrl1)
{
	int i, ret;

	/*
	 * Read the current state of the ctrl1 register
	 * so that we can restore it later.
	 */
	for (i = 0; i < SENSOR_ENABLE_ATTEMPTS; i++) {
		ret = raw_read8(s->i2c_addr, KX022_CTRL1, ctrl1);
		if (ret != EC_SUCCESS)
			continue;

		*ctrl1 &= ~KX022_CTRL1_PC1;

		ret = raw_write8(s->i2c_addr, KX022_CTRL1, *ctrl1);
		if (ret == EC_SUCCESS)
			return EC_SUCCESS;
	}
	CPRINTF("Error trying to disable accelerometer\n");
	return ret;
}

/**
 * Enable sensor by placing it in operating mode.
 *
 * Note: This is intended to be called in a pair with disable_sensor().
 *
 * @data Pointer to motion sensor data
 * @ctrl1 Value of KX022_CTRL1 register to write to sensor
 *
 * @return EC_SUCCESS if successful, EC_ERROR_* otherwise
 */
static int enable_sensor(const struct motion_sensor_t *s, int ctrl1)
{
	int i, ret;

	for (i = 0; i < SENSOR_ENABLE_ATTEMPTS; i++) {
		ret = raw_read8(s->i2c_addr, KX022_CTRL1, &ctrl1);
		if (ret != EC_SUCCESS)
			continue;

		/* Enable accelerometer based on ctrl1 value. */
		ret = raw_write8(s->i2c_addr, KX022_CTRL1,
				ctrl1 | KX022_CTRL1_PC1);

		/* On first success, we are done. */
		if (ret == EC_SUCCESS)
			return EC_SUCCESS;
	}

	/* Cannot enable accel, print warning and return an error. */
	CPRINTF("Error trying to enable accelerometer\n");
	return ret;
}

static int set_range(const struct motion_sensor_t *s,
				int range,
				int rnd)
{
	int ret, ctrl1, ctrl1_new, index;
	struct kx022_data *data = (struct kx022_data *)s->drv_data;

	/* Find index for interface pair matching the specified range. */
	index = find_param_index(range, rnd, ranges, ARRAY_SIZE(ranges));

	/* Disable the sensor to allow for changing of critical parameters. */
	mutex_lock(s->mutex);
	ret = disable_sensor(s, &ctrl1);
	if (ret != EC_SUCCESS) {
		mutex_unlock(s->mutex);
		return ret;
	}

	/* Determine new value of CTRL1 reg and attempt to write it. */
	ctrl1_new = (ctrl1 & ~KX022_GSEL_ALL) | ranges[index].reg;
	ret = raw_write8(s->i2c_addr,  KX022_CTRL1, ctrl1_new);

	/* If successfully written, then save the range. */
	if (ret == EC_SUCCESS) {
		data->sensor_range = index;
		ctrl1 = ctrl1_new;
	}

	/* Re-enable the sensor. */
	if (enable_sensor(s, ctrl1) != EC_SUCCESS)
		ret = EC_ERROR_UNKNOWN;

	mutex_unlock(s->mutex);

	return ret;
}

static int get_range(const struct motion_sensor_t *s,
				int * const range)
{
	struct kx022_data *data = (struct kx022_data *)s->drv_data;
	*range = ranges[data->sensor_range].val;
	return EC_SUCCESS;
}

static int set_resolution(const struct motion_sensor_t *s,
				int res,
				int rnd)
{
	int ret, ctrl1, ctrl1_new, index;
	struct kx022_data *data = (struct kx022_data *)s->drv_data;

	/* Find index for interface pair matching the specified resolution. */
	index = find_param_index(res, rnd, resolutions,
			ARRAY_SIZE(resolutions));

	/* Disable the sensor to allow for changing of critical parameters. */
	mutex_lock(s->mutex);
	ret = disable_sensor(s, &ctrl1);
	if (ret != EC_SUCCESS) {
		mutex_unlock(s->mutex);
		return ret;
	}

	/* Determine new value of CTRL1 reg and attempt to write it. */
	ctrl1_new = (ctrl1 & ~KX022_RES_12BIT) | resolutions[index].reg;
	ret = raw_write8(s->i2c_addr,  KX022_CTRL1, ctrl1_new);

	/* If successfully written, then save the range. */
	if (ret == EC_SUCCESS) {
		data->sensor_resolution = index;
		ctrl1 = ctrl1_new;
	}

	/* Re-enable the sensor. */
	if (enable_sensor(s, ctrl1) != EC_SUCCESS)
		ret = EC_ERROR_UNKNOWN;

	mutex_unlock(s->mutex);
	return ret;
}

static int get_resolution(const struct motion_sensor_t *s,
			int *res)
{
	struct kx022_data *data = (struct kx022_data *)s->drv_data;
	*res = resolutions[data->sensor_resolution].val;
	return EC_SUCCESS;
}

static int set_data_rate(const struct motion_sensor_t *s,
			int rate,
			int rnd)
{
	int ret, ctrl1, index;
	struct kx022_data *data = (struct kx022_data *)s->drv_data;

	/* Find index for interface pair matching the specified rate. */
	index = find_param_index(rate, rnd, datarates, ARRAY_SIZE(datarates));

	/* Disable the sensor to allow for changing of critical parameters. */
	mutex_lock(s->mutex);
	ret = disable_sensor(s, &ctrl1);
	if (ret != EC_SUCCESS) {
		mutex_unlock(s->mutex);
		return ret;
	}

	/* Set output data rate. */
	/* Enable Low-pass filter , frequence ODR/2 */
	ret = raw_write8(s->i2c_addr,  KX022_ODCNTL,
			datarates[index].reg | KX022_LPRO);

	/* If successfully written, then save the range. */
	if (ret == EC_SUCCESS)
		data->sensor_datarate = index;

	/* Re-enable the sensor. */
	if (enable_sensor(s, ctrl1) != EC_SUCCESS)
		ret = EC_ERROR_UNKNOWN;

	mutex_unlock(s->mutex);
	return ret;
}

static int get_data_rate(const struct motion_sensor_t *s,
				int *rate)
{
	struct kx022_data *data = (struct kx022_data *)s->drv_data;
	*rate = datarates[data->sensor_datarate].val;
	return EC_SUCCESS;
}


#ifdef CONFIG_ACCEL_INTERRUPTS
static int set_interrupt(const struct motion_sensor_t *s,
		unsigned int threshold)
{
	int ctrl1, tmp, ret;
	struct kx022_data *data = (struct kx022_data *)s->drv_data;

	/* Disable the sensor to allow for changing of critical parameters. */
	mutex_lock(s->mutex);
	ret = disable_sensor(s, &ctrl1);
	if (ret != EC_SUCCESS) {
		mutex_unlock(s->mutex);
		return ret;
	}

	/* Set interrupt timer to 1 so it wakes up immediately. */
	ret = raw_write8(s->i2c_addr, KX022_WAKEUP_TIMER, 1);
	if (ret != EC_SUCCESS)
		goto error_enable_sensor;

	/*
	 * Set threshold, note threshold register is in units of 16 counts, so
	 * first we need to divide by 16 to get the value to send.
	 */
	threshold >>= 4;
	ret = raw_write8(s->i2c_addr, KX022_WAKEUP_THRESHOLD, threshold);
	if (ret != EC_SUCCESS)
		goto error_enable_sensor;

	/*
	 * Set interrupt enable register on sensor. Note that once this
	 * function is called once, the interrupt stays enabled and it is
	 * only necessary to clear KX022_INT_REL to allow the next interrupt.
	 */
	ret = raw_read8(s->i2c_addr, KX022_INT_CTRL1, &tmp);
	if (ret != EC_SUCCESS)
		goto error_enable_sensor;
	if (!(tmp & KX022_INT_CTRL1_IEN)) {
		ret = raw_write8(s->i2c_addr, KX022_INT_CTRL1,
				tmp | KX022_INT_CTRL1_IEN);
		if (ret != EC_SUCCESS)
			goto error_enable_sensor;
	}

	/*
	 * Clear any pending interrupt on sensor by reading INT_REL register.
	 * Note: this register latches motion detected above threshold. Once
	 * latched, no interrupt can occur until this register is cleared.
	 */
	ret = raw_read8(s->i2c_addr, KX022_INT_REL, &tmp);

error_enable_sensor:
	/* Re-enable the sensor. */
	if (enable_sensor(s, ctrl1) != EC_SUCCESS)
		ret = EC_ERROR_UNKNOWN;
	mutex_unlock(s->mutex);
	return ret;
}
#endif

static int read(const struct motion_sensor_t *s, vector_3_t v)
{
	uint8_t acc[6];
	uint8_t reg = KX022_XOUT_L;
	int ret, multiplier;
	struct kx022_data *data = (struct kx022_data *)s->drv_data;

	/* Read 6 bytes starting at KX022_XOUT_L. */
	/* USE XHPL after low pass filter */
	mutex_lock(s->mutex);
	i2c_lock(I2C_PORT_ACCEL, 1);
	ret = i2c_xfer(I2C_PORT_ACCEL, s->i2c_addr, &reg, 1, acc, 6,
			I2C_XFER_SINGLE);
	i2c_lock(I2C_PORT_ACCEL, 0);
	mutex_unlock(s->mutex);

	if (ret != EC_SUCCESS)
		return ret;

	/* Determine multiplier based on stored range. */
	switch (ranges[data->sensor_range].reg) {
	case KX022_GSEL_2G:
		multiplier = 1;
		break;
	case KX022_GSEL_4G:
		multiplier = 2;
		break;
	case KX022_GSEL_8G:
		multiplier = 4;
		break;
	default:
		return EC_ERROR_UNKNOWN;
	}

	/*
	 * Convert acceleration to a signed 12-bit number. Note, based on
	 * the order of the registers:
	 *
	 * acc[0] = KX022_XOUT_L
	 * acc[1] = KX022_XOUT_H
	 * acc[2] = KX022_YOUT_L
	 * acc[3] = KX022_YOUT_H
	 * acc[4] = KX022_ZOUT_L
	 * acc[5] = KX022_ZOUT_H
	 */
	v[0] = multiplier * (((int8_t)acc[1]) << 4) | (acc[0] >> 4);
	v[1] = multiplier * (((int8_t)acc[3]) << 4) | (acc[2] >> 4);
	v[2] = multiplier * (((int8_t)acc[5]) << 4) | (acc[4] >> 4);

	return EC_SUCCESS;
}

#ifdef CONFIG_ACCEL_INTERRUPTS
static int config_interrupt(const struct motion_sensor_t *s)
{
	int ctrl1;
	mutex_lock(s->mutex);

	/* Disable the sensor to allow for changing of critical parameters. */
	ret = disable_sensor(s, &ctrl1);
	if (ret != EC_SUCCESS)
		goto cleanup_exit;

	/* Enable wake up (motion detect) functionality. */
	ret = raw_read8(s->i2c_addr, KX022_CTRL1, &tmp);
	tmp &= ~KX022_CTRL1_PC1;
	tmp |= KX022_CTRL1_WUFE;
	ret = raw_write8(s->i2c_addr, KX022_CTRL1, tmp);

	/* Set interrupt polarity to rising edge and keep interrupt disabled. */
	ret = raw_write8(s->i2c_addr,
			  KX022_INT_CTRL1,
			  KX022_INT_CTRL1_IEA);
	if (ret != EC_SUCCESS)
		goto cleanup_exit;

	/* Set output data rate for wake-up interrupt function. */
	ret = raw_write8(s->i2c_addr, KX022_CTRL2, KX022_OWUF_100_0HZ);
	if (ret != EC_SUCCESS)
		goto cleanup_exit;

	/* Set interrupt to trigger on motion on any axis. */
	ret = raw_write8(s->i2c_addr, KX022_INT_CTRL2,
			KX022_INT_SRC2_XNWU | KX022_INT_SRC2_XPWU |
			KX022_INT_SRC2_YNWU | KX022_INT_SRC2_YPWU |
			KX022_INT_SRC2_ZNWU | KX022_INT_SRC2_ZPWU);
	if (ret != EC_SUCCESS)
		goto cleanup_exit;

	/*
	 * Enable accel interrupts. Note: accels will not initiate an interrupt
	 * until interrupt enable bit in KX022_INT_CTRL1 is set on the device.
	 */
	gpio_enable_interrupt(GPIO_ACCEL_INT_LID);
	gpio_enable_interrupt(GPIO_ACCEL_INT_BASE);

	/* Enable the sensor. */
	ret = enable_sensor(s, ctrl1);
cleanup_exit:
	mutex_unlock(s->mutex);
	return ret;
}
#endif

static int init(const struct motion_sensor_t *s)
{
	int ret = EC_SUCCESS;
	int cnt = 0, tmp, range, rate;

	/*
	 * This sensor can be powered through an EC reboot, so the state of
	 * the sensor is unknown here. Initiate software reset to restore
	 * sensor to default.
	 */
	mutex_lock(s->mutex);
	ret = raw_write8(s->i2c_addr, KX022_CTRL2, KX022_CTRL2_SRST);
	mutex_unlock(s->mutex);
	if (ret != EC_SUCCESS)
		return ret;

	/* Wait until software reset is complete or timeout. */
	do {
		/* Added 1m delay after software reset */
		msleep(1);

		ret = raw_read8(s->i2c_addr, KX022_CTRL2, &tmp);

		/* Reset complete. */
		if (ret == EC_SUCCESS && !(tmp & KX022_CTRL2_SRST))
			break;

		/* Check for timeout. */
		if (cnt++ > 5) {
			ret = EC_ERROR_TIMEOUT;
			CPRINTF("%s: SRST Error.\n", s->name);
			return ret;
		}
	} while (1);

	ret = set_range(s, s->range, 1);
	if (ret != EC_SUCCESS)
		return ret;

	ret = set_resolution(s, 12, 1);
	if (ret != EC_SUCCESS)
		return ret;

	ret = set_data_rate(s, s->odr, 1);
	if (ret != EC_SUCCESS)
		return ret;

#ifdef CONFIG_ACCEL_INTERRUPTS
	config_interrupt(s);
#endif
	get_range(s, &range);
	get_data_rate(s, &rate);
	CPRINTF("[%T %s: Done Init type:0x%X range:%d rate:%d]\n",
		s->name, s->type, range, rate);

	return ret;
}

const struct accelgyro_drv kx022_drv = {
	.init = init,
	.read = read,
	.set_range = set_range,
	.get_range = get_range,
	.set_resolution = set_resolution,
	.get_resolution = get_resolution,
	.set_data_rate = set_data_rate,
	.get_data_rate = get_data_rate,
#ifdef CONFIG_ACCEL_INTERRUPTS
	.set_interrupt = set_interrupt,
#endif
};
