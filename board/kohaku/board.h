/* Copyright 2019 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* Kohaku board configuration */

#ifndef __CROS_EC_BOARD_H
#define __CROS_EC_BOARD_H

/* Baseboard features */
#include "baseboard.h"

/* Optional features */
#define CONFIG_SYSTEM_UNLOCKED /* Allow dangerous commands while in dev. */

#define CONFIG_POWER_BUTTON
#define CONFIG_KEYBOARD_BOARD_CONFIG
#define CONFIG_KEYBOARD_PROTOCOL_8042
#define CONFIG_LED_COMMON
#define CONFIG_LOW_POWER_IDLE

#define CONFIG_HOSTCMD_ESPI
/* #define CONFIG_HOSTCMD_ESPI_VW_SIGNALS */

#undef CONFIG_UART_TX_BUF_SIZE
#define CONFIG_UART_TX_BUF_SIZE 4096

/* Keyboard features */
#define CONFIG_PWM_KBLIGHT

/* Sensors */
/* BMI160 Base accel/gyro */
#define CONFIG_ACCEL_INTERRUPTS
#define CONFIG_ACCELGYRO_BMI160
#define CONFIG_ACCELGYRO_BMI160_INT_EVENT \
	TASK_EVENT_MOTION_SENSOR_INTERRUPT(BASE_ACCEL)
#define CONFIG_ACCELGYRO_BMI160_INT2_OUTPUT
/* BMA253 Lid accel */
#define CONFIG_ACCEL_BMA255
#define CONFIG_ACCEL_FORCE_MODE_MASK (BIT(LID_ACCEL) | BIT(LID_ALS))
#define CONFIG_LID_ANGLE
#define CONFIG_LID_ANGLE_SENSOR_BASE BASE_ACCEL
#define CONFIG_LID_ANGLE_SENSOR_LID LID_ACCEL
#define CONFIG_LID_ANGLE_UPDATE
/* OPT3001 ALS */
#define CONFIG_ALS
#define ALS_COUNT 1
#define CONFIG_ALS_BH1730
/* Parameter to calculate LUX on Kohaku */
/*
 * TODO (b/130835790): These values are from Caroline. Do they need to be
 * changed for Kohaku?
 */
#define CONFIG_ALS_BH1730_LUXTH_PARAMS
#define BH1730_LUXTH1_1K                270
#define BH1730_LUXTH1_D0_1K             4000
#define BH1730_LUXTH1_D1_1K             6364
#define BH1730_LUXTH2_1K                740
#define BH1730_LUXTH2_D0_1K             3088
#define BH1730_LUXTH2_D1_1K             3010
#define BH1730_LUXTH3_1K                1030
#define BH1730_LUXTH3_D0_1K             2056
#define BH1730_LUXTH3_D1_1K             1608
#define BH1730_LUXTH4_1K                3670
#define BH1730_LUXTH4_D0_1K             550
#define BH1730_LUXTH4_D1_1K             149

/* USB Type C and USB PD defines */
#define CONFIG_USB_PD_TCPM_PS8751
#define BOARD_TCPC_C0_RESET_HOLD_DELAY PS8XXX_RESET_DELAY_MS
#define BOARD_TCPC_C0_RESET_POST_DELAY 0
#define BOARD_TCPC_C1_RESET_HOLD_DELAY PS8XXX_RESET_DELAY_MS
#define BOARD_TCPC_C1_RESET_POST_DELAY 0
#define GPIO_USB_C0_TCPC_RST GPIO_USB_C0_TCPC_RST_ODL
#define GPIO_USB_C1_TCPC_RST GPIO_USB_C1_TCPC_RST_ODL

/* BC 1.2 */
#define CONFIG_BC12_DETECT_MAX14637

/* Charger features */
/*
 * The IDCHG current limit is set in 512 mA steps. The value set here is
 * somewhat specific to the battery pack being currently used. The limit here
 * was set via experimentation by finding how high it can be set and still boot
 * the AP successfully, then backing off to provide margin.
 *
 * TODO(b/133444665): Revisit this threshold once peak power consumption tuning
 * for the AP is completed.
 */
#define CONFIG_CHARGER_BQ25710_IDCHG_LIMIT_MA 8192

/* Volume Button feature */
#define CONFIG_VOLUME_BUTTONS
#define GPIO_VOLUME_UP_L GPIO_EC_VOLUP_BTN_ODL
#define GPIO_VOLUME_DOWN_L GPIO_EC_VOLDN_BTN_ODL

/* Thermal features */
#define CONFIG_TEMP_SENSOR_POWER_GPIO GPIO_EN_A_RAILS
#define CONFIG_THERMISTOR
#define CONFIG_THROTTLE_AP
#define CONFIG_STEINHART_HART_3V3_51K1_47K_4050B

/*
 * Macros for GPIO signals used in common code that don't match the
 * schematic names. Signal names in gpio.inc match the schematic and are
 * then redefined here to so it's more clear which signal is being used for
 * which purpose.
 */
#define GPIO_PCH_RSMRST_L GPIO_EC_PCH_RSMRST_L
#define GPIO_PCH_SLP_S0_L GPIO_SLP_S0_L
#define GPIO_CPU_PROCHOT GPIO_EC_PROCHOT_ODL
#define GPIO_AC_PRESENT GPIO_ACOK_OD
#define GPIO_RSMRST_L_PGOOD GPIO_PG_EC_RSMRST_L
#define GPIO_PCH_SLP_S3_L GPIO_SLP_S3_L
#define GPIO_PCH_SLP_S4_L GPIO_SLP_S4_L
#define GPIO_EN_PP5000 GPIO_EN_PP5000_A

#ifndef __ASSEMBLER__

#include "gpio_signal.h"
#include "registers.h"

/* GPIO signals updated base on board version. */
extern enum gpio_signal gpio_en_pp5000_a;

enum adc_channel {
	ADC_TEMP_SENSOR_1,	/* ADC0 */
	ADC_TEMP_SENSOR_2,	/* ADC1 */
	ADC_CH_COUNT
};

enum sensor_id {
	LID_ACCEL = 0,
	BASE_ACCEL,
	BASE_GYRO,
	LID_ALS,
	SENSOR_COUNT,
};

enum pwm_channel {
	PWM_CH_KBLIGHT,
	PWM_CH_COUNT
};

enum temp_sensor_id {
	TEMP_SENSOR_1,
	TEMP_SENSOR_2,
	TEMP_SENSOR_COUNT
};

/* List of possible batteries */
enum battery_type {
	BATTERY_DYNA,
	BATTERY_TYPE_COUNT,
};

#endif /* !__ASSEMBLER__ */

#endif /* __CROS_EC_BOARD_H */
