/* Copyright 2015 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* Cyan board configuration */

#ifndef __BOARD_H
#define __BOARD_H

/* Optional features */
#define CONFIG_SYSTEM_UNLOCKED  /* Allow dangerous commands */
#define CONFIG_WATCHDOG_HELP
#define CONFIG_CLOCK_CRYSTAL
#define CONFIG_CHIPSET_BRASWELL
#define CONFIG_KEYBOARD_PROTOCOL_8042
#define CONFIG_KEYBOARD_IRQ_GPIO GPIO_KBD_IRQ_L
#define CONFIG_KEYBOARD_COL2_INVERTED
#define CONFIG_SCI_GPIO GPIO_PCH_SCI_L
#define CONFIG_POWER_BUTTON
#define CONFIG_POWER_BUTTON_X86
#define CONFIG_LID_SWITCH
#define CONFIG_SWITCH
#define CONFIG_LED_COMMON
#define CONFIG_POWER_COMMON
#define CONFIG_EXTPOWER_GPIO
#define CONFIG_I2C
#define CONFIG_VBOOT_HASH

#define CONFIG_CHARGER
#define CONFIG_BATTERY_SMART
#define CONFIG_CHARGER_V2
#define CONFIG_CHARGER_BQ24770
#define CONFIG_CHARGER_ILIM_PIN_DISABLED
#define CONFIG_CHARGER_SENSE_RESISTOR 10
#define CONFIG_CHARGER_SENSE_RESISTOR_AC 10
#define CONFIG_CHARGER_INPUT_CURRENT 2240
#define CONFIG_CHARGER_DISCHARGE_ON_AC

#define CONFIG_SPI
#define CONFIG_SPI_PORT 1
#define CONFIG_SPI_CS_GPIO GPIO_PVT_CS0
#define CONFIG_SPI_FLASH
#define CONFIG_SPI_FLASH_SIZE 4194304
#define CONFIG_FLASH_BANK_SIZE      0x00000800  /* protect bank size */
#define CONFIG_FLASH_ERASE_SIZE     0x00001000  /* erase bank size */
#define CONFIG_FLASH_WRITE_SIZE     0x00000010  /* minimum write size */

/* I2C ports */
#define I2C_PORT0		0
#define I2C_PORT1		1

#define I2C_PORT_BATTERY	I2C_PORT0
#define I2C_PORT_CHARGER	I2C_PORT0
#define I2C_PORT_THERMAL	I2C_PORT0

#define I2C_PORT_ACCEL		I2C_PORT1

/* Motion */
#define CONFIG_SENSOR_BASE 0
#define CONFIG_SENSOR_LID 1
#define CONFIG_LID_ANGLE
#define CONFIG_ACCEL_KXCJ9

#define CONFIG_TEMP_SENSOR
#define CONFIG_TEMP_SENSOR_TMP432

/* Ideal flash write size fills the 32-entry flash write buffer */
#define CONFIG_FLASH_WRITE_IDEAL_SIZE (32 * 4)

/* Modules we want to exclude */
#undef CONFIG_EEPROM
#undef CONFIG_EOPTION
#undef CONFIG_PSTORE
#undef CONFIG_PECI
#undef CONFIG_PWM
#undef CONFIG_FANS
#undef CONFIG_ADC
#undef CONFIG_WAKE_PIN
#ifndef __ASSEMBLER__

#include "gpio_signal.h"

/* power signal definitions */
enum power_signal {
	X86_ALL_SYS_PWRGD = 0,
	X86_RSMRST_L_PWRGD,
	X86_SLP_S3_DEASSERTED,
	X86_SLP_S4_DEASSERTED,

	/* Number of X86 signals */
	POWER_SIGNAL_COUNT
};

enum temp_sensor_id {
	/* TMP432 local and remote sensors */
	TEMP_SENSOR_I2C_TMP432_LOCAL,
	TEMP_SENSOR_I2C_TMP432_REMOTE1,
	TEMP_SENSOR_I2C_TMP432_REMOTE2,

	/* Battery temperature sensor */
	TEMP_SENSOR_BATTERY,

	TEMP_SENSOR_COUNT
};

/* Discharge battery when on AC power for factory test. */
int board_discharge_on_ac(int enable);
#endif /* !__ASSEMBLER__ */

#endif /* __BOARD_H */
