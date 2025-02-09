/* Copyright 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * TI bq24773 battery charger driver.
 */

#ifndef __CROS_EC_BQ24773_H
#define __CROS_EC_BQ24773_H

/* for i2c_read and i2c_write functions. */
#include "i2c.h"

/* I2C address */
#define BQ24770_ADDR (0x12)
#define BQ24773_ADDR (0x6a << 1)

/* Chip specific commands */
#define BQ24770_CHARGE_OPTION0          0x12
#define BQ24770_CHARGE_OPTION1          0x3B
#define BQ24770_CHARGE_OPTION2          0x38
#define BQ24770_PROCHOT_OPTION0         0x3C
#define BQ24770_PROCHOT_OPTION1         0x3D
#define BQ24770_CHARGE_CURRENT          0x14
#define BQ24770_MAX_CHARGE_VOLTAGE      0x15
#define BQ24770_MIN_SYSTEM_VOLTAGE      0x3E
#define BQ24770_INPUT_CURRENT           0x3F
#define BQ24770_MANUFACTURE_ID          0xFE
#define BQ24770_DEVICE_ADDRESS          0xFF

#define BQ24773_CHARGE_OPTION0          0x00
#define BQ24773_CHARGE_OPTION1          0x02
#define BQ24773_PROCHOT_OPTION0         0x04
#define BQ24773_PROCHOT_OPTION1         0x06
#define BQ24773_PROCHOT_STATUS          0x08
#define BQ24773_DEVICE_ADDRESS          0x09
#define BQ24773_CHARGE_CURRENT          0x0A
#define BQ24773_MAX_CHARGE_VOLTAGE      0x0C
#define BQ24773_MIN_SYSTEM_VOLTAGE      0x0E
#define BQ24773_INPUT_CURRENT           0x0F
#define BQ24773_CHARGE_OPTION2          0x10

/* Option bits */
#define OPTION0_CHARGE_INHIBIT          BIT(0)
#define OPTION0_LEARN_ENABLE            BIT(5)
#define OPTION0_SWITCHING_FREQ_MASK     (3 << 8)
#define OPTION0_SWITCHING_FREQ_600KHZ   (0 << 8)
#define OPTION0_SWITCHING_FREQ_800KHZ   BIT(8)
#define OPTION0_SWITCHING_FREQ_1000KHZ  (2 << 8)
#define OPTION0_SWITCHING_FREQ_1200KHZ  (3 << 8)

#define OPTION2_EN_EXTILIM              BIT(7)

/* Prochot Option bits */
#define PROCHOT_OPTION1_SELECTOR_MASK   0x7f /* [6:0] PROCHOT SELECTOR */

/* ChargeCurrent Register - 0x14 (mA) */
#define CHARGE_I_OFF                    0
#define CHARGE_I_MIN                    128
#define CHARGE_I_MAX                    8128
#define CHARGE_I_STEP                   64

/* MaxChargeVoltage Register - 0x15 (mV) */
#define CHARGE_V_MIN                    1024
#define CHARGE_V_MAX                    19200
#define CHARGE_V_STEP                   16

/* InputCurrent Register - 0x3f (mA) */
#define INPUT_I_MIN                    128
#define INPUT_I_MAX                    8128
#define INPUT_I_STEP                   64

#ifdef CONFIG_CHARGER_BQ24770
	#define CHARGER_NAME		"bq24770"
	#define I2C_ADDR_CHARGER	BQ24770_ADDR

	#define REG_CHARGE_OPTION0	BQ24770_CHARGE_OPTION0
	#define REG_CHARGE_OPTION1	BQ24770_CHARGE_OPTION1
	#define REG_CHARGE_OPTION2	BQ24770_CHARGE_OPTION2
	#define REG_PROCHOT_OPTION0	BQ24770_PROCHOT_OPTION0
	#define REG_PROCHOT_OPTION1	BQ24770_PROCHOT_OPTION1
	#define REG_CHARGE_CURRENT	BQ24770_CHARGE_CURRENT
	#define REG_MAX_CHARGE_VOLTAGE	BQ24770_MAX_CHARGE_VOLTAGE
	#define REG_MIN_SYSTEM_VOLTAGE	BQ24770_MIN_SYSTEM_VOLTAGE
	#define REG_INPUT_CURRENT	BQ24770_INPUT_CURRENT
	#define REG_MANUFACTURE_ID	BQ24770_MANUFACTURE_ID
	#define REG_DEVICE_ADDRESS	BQ24770_DEVICE_ADDRESS

#elif defined(CONFIG_CHARGER_BQ24773)
	#define CHARGER_NAME		"bq24773"
	#define I2C_ADDR_CHARGER	BQ24773_ADDR

	#define REG_CHARGE_OPTION0	BQ24773_CHARGE_OPTION0
	#define REG_CHARGE_OPTION1	BQ24773_CHARGE_OPTION1
	#define REG_CHARGE_OPTION2	BQ24773_CHARGE_OPTION2
	#define REG_PROCHOT_OPTION0	BQ24773_PROCHOT_OPTION0
	#define REG_PROCHOT_OPTION1	BQ24773_PROCHOT_OPTION1
	#define REG_CHARGE_CURRENT	BQ24773_CHARGE_CURRENT
	#define REG_MAX_CHARGE_VOLTAGE	BQ24773_MAX_CHARGE_VOLTAGE
	#define REG_MIN_SYSTEM_VOLTAGE	BQ24773_MIN_SYSTEM_VOLTAGE
	#define REG_INPUT_CURRENT	BQ24773_INPUT_CURRENT
	#define REG_DEVICE_ADDRESS	BQ24773_DEVICE_ADDRESS
#endif

#ifdef CONFIG_CHARGER_BQ24773
static inline int raw_read8(int offset, int *value)
{
	return i2c_read8(I2C_PORT_CHARGER, I2C_ADDR_CHARGER, offset, value);
}

static inline int raw_write8(int offset, int value)
{
	return i2c_write8(I2C_PORT_CHARGER, I2C_ADDR_CHARGER, offset, value);
}
#endif

static inline int raw_read16(int offset, int *value)
{
	return i2c_read16(I2C_PORT_CHARGER, I2C_ADDR_CHARGER, offset, value);
}

static inline int raw_write16(int offset, int value)
{
	return i2c_write16(I2C_PORT_CHARGER, I2C_ADDR_CHARGER, offset, value);
}

#endif /* __CROS_EC_BQ24773_H */
