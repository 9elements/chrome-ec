/* Copyright 2015 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* oak board configuration */

#ifndef __BOARD_H
#define __BOARD_H

/* Add for AC adaptor, charger, battery */
#define CONFIG_BATTERY_CUT_OFF
#define CONFIG_BATTERY_SMART
#define CONFIG_CHARGER
#define CONFIG_CHARGER_BQ24773
#define CONFIG_CHARGER_DISCHARGE_ON_AC
#define CONFIG_CHARGER_INPUT_CURRENT 2150
#define CONFIG_CHARGER_SENSE_RESISTOR 10
#define CONFIG_CHARGER_SENSE_RESISTOR_AC 10
#define CONFIG_CHARGER_V2
#define CONFIG_CHIPSET_MEDIATEK
#define CONFIG_FORCE_CONSOLE_RESUME
#undef  CONFIG_HIBERNATE
#define CONFIG_HOST_COMMAND_STATUS
#define CONFIG_HOSTCMD_PD
#define CONFIG_I2C
#define CONFIG_KEYBOARD_COL2_INVERTED
#define CONFIG_KEYBOARD_PROTOCOL_MKBP
#define CONFIG_LED_COMMON
#define CONFIG_LOW_POWER_IDLE
#define CONFIG_FORCE_CONSOLE_RESUME
#define CONFIG_PMIC_FW_LONG_PRESS_TIMER
#define CONFIG_POWER_BUTTON
#define CONFIG_POWER_COMMON
#define CONFIG_USB_POWER_DELIVERY
#define CONFIG_USB_PD_ALT_MODE
#define CONFIG_USB_PD_ALT_MODE_DFP
#define CONFIG_USB_PD_CUSTOM_VDM
#define CONFIG_USB_PD_DUAL_ROLE
#define CONFIG_USB_PD_PORT_COUNT 1
#define CONFIG_USB_PD_TCPM_TCPCI
#define CONFIG_SPI
#define CONFIG_STM_HWTIMER32
#define CONFIG_VBOOT_HASH
#undef  CONFIG_WATCHDOG_HELP
#define CONFIG_LID_SWITCH
#define CONFIG_SWITCH
#define CONFIG_BOARD_VERSION
#undef  CONFIG_UART_CONSOLE
#define CONFIG_UART_CONSOLE 1

/* UART DMA */
#undef CONFIG_UART_TX_DMA
#undef CONFIG_UART_RX_DMA

#undef  DEFERRABLE_MAX_COUNT
#define DEFERRABLE_MAX_COUNT 9

/*
 * Allow dangerous commands.
 * TODO: Remove this config before production.
 */
#define CONFIG_SYSTEM_UNLOCKED

/* Optional features */
#define CONFIG_CMD_HOSTCMD

/* Drivers */
#define CONFIG_USB_SWITCH_PI3USB30532
/*
 * 8-bit USB type-C switch I2C addresses:
 *   port 0: 0x54 << 1
 *   port 1: 0x55 << 1
 */
#define CONFIG_USB_SWITCH_I2C_ADDRS {0x54 << 1, 0x55 << 1}

#ifndef __ASSEMBLER__

/* 48 MHz SYSCLK clock frequency */
#define CPU_CLOCK 48000000

/* Keyboard output port list */
#define KB_OUT_PORT_LIST GPIO_A, GPIO_B, GPIO_C, GPIO_D

/* 2 I2C master ports, connect to battery, charger, pd and USB switches */
#define I2C_PORT_BATTERY 0
#define I2C_PORT_CHARGER 0
#define I2C_PORT_PD_MCU 1
#define I2C_PORT_USB_SWITCH 1
#define I2C_PORT_TCPC 1

/* Timer selection */
#define TIM_CLOCK32 2
#define TIM_WATCHDOG 4

#include "gpio_signal.h"

enum power_signal {
	MTK_POWER_GOOD = 0,
	MTK_SUSPEND_ASSERTED,
	/* Number of power signals */
	POWER_SIGNAL_COUNT
};

enum pwm_channel {
	PWM_CH_POWER_LED = 0,
	/* Number of PWM channels */
	PWM_CH_COUNT
};

enum adc_channel {
	ADC_CH_COUNT
};

/* start as a sink in case we have no other power supply/battery */
#define PD_DEFAULT_STATE PD_STATE_SNK_DISCONNECTED

/* TODO: determine the following board specific type-C power constants */
/*
 * delay to turn on the power supply max is ~16ms.
 * delay to turn off the power supply max is about ~180ms.
 */
#define PD_POWER_SUPPLY_TURN_ON_DELAY  30000  /* us */
#define PD_POWER_SUPPLY_TURN_OFF_DELAY 250000 /* us */

/* Define typical operating power and max power */
#define PD_OPERATING_POWER_MW 15000
#define PD_MAX_POWER_MW       60000
#define PD_MAX_CURRENT_MA     3000
#define PD_MAX_VOLTAGE_MV     20000

/* Discharge battery when on AC power for factory test. */
int board_discharge_on_ac(int enable);

/* Reset PD MCU */
void board_reset_pd_mcu(void);

#endif  /* !__ASSEMBLER__ */

#endif  /* __BOARD_H */
