/* Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
/* ryu sensor hub configuration */

#include "common.h"
#include "console.h"
#include "driver/accelgyro_lsm6ds0.h"
#include "gpio.h"
#include "hooks.h"
#include "i2c.h"
#include "motion_sense.h"
#include "power.h"
#include "registers.h"
#include "task.h"
#include "util.h"

#include "gpio_list.h"

/* I2C ports */
const struct i2c_port_t i2c_ports[] = {
	{"slave",  I2C_PORT_SLAVE, 100,
		GPIO_SLAVE_I2C_SCL, GPIO_SLAVE_I2C_SDA},
};
const unsigned int i2c_ports_used = ARRAY_SIZE(i2c_ports);

void board_config_pre_init(void)
{
	/*
	 *  enable SYSCFG clock:
	 *  otherwise the SYSCFG peripheral is not clocked during the pre-init
	 *  and the register write as no effect.
	 */
	STM32_RCC_APB2ENR |= 1 << 0;
	/*
	 * Remap USART DMA to match the USART driver
	 * the DMA mapping is :
	 *  Chan 4 : USART1_TX
	 *  Chan 5 : USART1_RX
	 */
	STM32_SYSCFG_CFGR1 |= (1 << 9) | (1 << 10);/* Remap USART1 RX/TX DMA */
}
