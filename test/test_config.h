/* Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* Per-test config flags */

#ifndef __CROS_EC_TEST_CONFIG_H
#define __CROS_EC_TEST_CONFIG_H

/* Test config flags only apply for test builds */
#ifdef TEST_BUILD

/* Don't compile vboot hash support unless specifically testing for it */
#undef CONFIG_VBOOT_HASH

#ifdef TEST_ADAPTER
#define CONFIG_CHIPSET_CAN_THROTTLE
#define CONFIG_EXTPOWER_FALCO
#endif

#ifdef TEST_BKLIGHT_LID
#define CONFIG_BACKLIGHT_LID
#endif

#ifdef TEST_BKLIGHT_PASSTHRU
#define CONFIG_BACKLIGHT_LID
#define CONFIG_BACKLIGHT_REQ_GPIO GPIO_PCH_BKLTEN
#endif

#ifdef TEST_KB_8042
#define CONFIG_KEYBOARD_PROTOCOL_8042
#endif

#ifdef TEST_KB_MKBP
#define CONFIG_KEYBOARD_PROTOCOL_MKBP
#endif

#ifdef TEST_KB_SCAN
#define CONFIG_KEYBOARD_PROTOCOL_MKBP
#endif

#ifdef TEST_LED_SPRING
#define CONFIG_BATTERY_MOCK
#define CONFIG_BATTERY_SMART
#define CONFIG_CHARGER_INPUT_CURRENT 4032
#define CONFIG_LED_DRIVER_LP5562
#define I2C_PORT_MASTER 1
#define I2C_PORT_BATTERY 1
#define I2C_PORT_CHARGER 1
#endif

#ifdef TEST_SBS_CHARGING
#define CONFIG_BATTERY_MOCK
#define CONFIG_BATTERY_SMART
#define CONFIG_CHARGER
#define CONFIG_CHARGER_INPUT_CURRENT 4032
#define CONFIG_CHARGER_DISCHARGE_ON_AC
int board_discharge_on_ac(int enabled);
#define I2C_PORT_MASTER 1
#define I2C_PORT_BATTERY 1
#define I2C_PORT_CHARGER 1
#endif

#ifdef TEST_THERMAL
#define CONFIG_CHIPSET_CAN_THROTTLE
#define CONFIG_FANS 1
#define CONFIG_TEMP_SENSOR
#endif

#ifdef TEST_THERMAL_FALCO
#define CONFIG_BATTERY_MOCK
#define CONFIG_BATTERY_SMART
#define CONFIG_CHARGER
#define CONFIG_CHARGER_INPUT_CURRENT 4032
#define CONFIG_CHIPSET_CAN_THROTTLE
#define CONFIG_EXTPOWER_FALCO
#define CONFIG_FANS 1
#define CONFIG_TEMP_SENSOR
#define I2C_PORT_BATTERY 1
#define I2C_PORT_CHARGER 1
#define I2C_PORT_MASTER 1
#endif

#endif  /* TEST_BUILD */
#endif  /* __CROS_EC_TEST_CONFIG_H */
