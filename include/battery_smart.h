/* Copyright 2012 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Smart battery v1.0
 * Smart battery charger v1.1
 */
#ifndef __CROS_EC_BATTERY_SMART_H
#define __CROS_EC_BATTERY_SMART_H

#include "common.h"

/* Smart battery and charger I2C address */
#define BATTERY_ADDR 0x16
#define CHARGER_ADDR 0x12

/* Charger functions */
#define SB_CHARGER_SPEC_INFO            0x11
#define SB_CHARGE_MODE                  0x12
#define SB_CHARGER_STATUS               0x13
#define SB_CHARGING_CURRENT             0x14
#define SB_CHARGING_VOLTAGE             0x15
#define SB_ALARM_WARNING                0x16

/* Battery functions */
#define SB_MANUFACTURER_ACCESS          0x00
#define SB_REMAINING_CAPACITY_ALARM     0x01
#define SB_REMAINING_TIME_ALARM         0x02
#define SB_BATTERY_MODE                 0x03
#define SB_AT_RATE                      0x04
#define SB_AT_RATE_TIME_TO_FULL         0x05
#define SB_AT_RATE_TIME_TO_EMPTY        0x06
#define SB_AT_RATE_OK                   0x07
#define SB_TEMPERATURE                  0x08
#define SB_VOLTAGE                      0x09
#define SB_CURRENT                      0x0a
#define SB_AVERAGE_CURRENT              0x0b
#define SB_MAX_ERROR                    0x0c
#define SB_RELATIVE_STATE_OF_CHARGE     0x0d
#define SB_ABSOLUTE_STATE_OF_CHARGE     0x0e
#define SB_REMAINING_CAPACITY           0x0f
#define SB_FULL_CHARGE_CAPACITY         0x10
#define SB_RUN_TIME_TO_EMPTY            0x11
#define SB_AVERAGE_TIME_TO_EMPTY        0x12
#define SB_AVERAGE_TIME_TO_FULL         0x13
#define SB_CHARGING_CURRENT             0x14
#define SB_CHARGING_VOLTAGE             0x15
#define SB_BATTERY_STATUS               0x16
#define SB_CYCLE_COUNT                  0x17
#define SB_DESIGN_CAPACITY              0x18
#define SB_DESIGN_VOLTAGE               0x19
#define SB_SPECIFICATION_INFO           0x1a
#define SB_MANUFACTURER_DATE            0x1b
#define SB_SERIAL_NUMBER                0x1c
#define SB_MANUFACTURER_NAME            0x20
#define SB_DEVICE_NAME                  0x21
#define SB_DEVICE_CHEMISTRY             0x22
#define SB_MANUFACTURER_DATA            0x23
/* Extension of smart battery spec, may not be supported on all platforms */
#define SB_PACK_STATUS                  0x43
#define SB_ALT_MANUFACTURER_ACCESS      0x44

/* Battery mode */
#define MODE_INTERNAL_CHARGE_CONTROLLER BIT(0)
#define MODE_PRIMARY_BATTERY_SUPPORT    BIT(1)
#define MODE_CONDITION_CYCLE            BIT(7)
#define MODE_CHARGE_CONTROLLER_ENABLED  BIT(8)
#define MODE_PRIMARY_BATTERY            BIT(9)
#define MODE_ALARM                      BIT(13)
#define MODE_CHARGER                    BIT(14)
#define MODE_CAPACITY                   BIT(15)

/* Battery status */
#define STATUS_ERR_CODE_MASK            0xf
#define STATUS_CODE_OK                  0
#define STATUS_CODE_BUSY                1
#define STATUS_CODE_RESERVED            2
#define STATUS_CODE_UNSUPPORTED         3
#define STATUS_CODE_ACCESS_DENIED       4
#define STATUS_CODE_OVERUNDERFLOW       5
#define STATUS_CODE_BADSIZE             6
#define STATUS_CODE_UNKNOWN_ERROR       7
#define STATUS_FULLY_DISCHARGED         BIT(4)
#define STATUS_FULLY_CHARGED            BIT(5)
#define STATUS_DISCHARGING              BIT(6)
#define STATUS_INITIALIZED              BIT(7)
#define STATUS_REMAINING_TIME_ALARM     BIT(8)
#define STATUS_REMAINING_CAPACITY_ALARM BIT(9)
#define STATUS_TERMINATE_DISCHARGE_ALARM BIT(11)
#define STATUS_OVERTEMP_ALARM           BIT(12)
#define STATUS_TERMINATE_CHARGE_ALARM   BIT(14)
#define STATUS_OVERCHARGED_ALARM        BIT(15)

/* Charger alarm warning */
#define ALARM_OVER_CHARGED              0x8000
#define ALARM_TERMINATE_CHARGE          0x4000
#define ALARM_RESERVED_2000             0x2000
#define ALARM_OVER_TEMP                 0x1000
#define ALARM_TERMINATE_DISCHARGE       0x0800
#define ALARM_RESERVED_0400             0x0400
#define ALARM_REMAINING_CAPACITY        0x0200
#define ALARM_REMAINING_TIME            0x0100
#define ALARM_STATUS_INITIALIZE         0x0080
#define ALARM_STATUS_DISCHARGING        0x0040
#define ALARM_STATUS_FULLY_CHARGED      0x0020
#define ALARM_STATUS_FULLY_DISCHARGED   0x0010
/* Charge mode */
#define CHARGE_FLAG_INHIBIT_CHARGE      BIT(0)
#define CHARGE_FLAG_ENABLE_POLLING      BIT(1)
#define CHARGE_FLAG_POR_RESET           BIT(2)
#define CHARGE_FLAG_RESET_TO_ZERO       BIT(3)
/* Charger status */
#define CHARGER_CHARGE_INHIBITED        BIT(0)
#define CHARGER_POLLING_ENABLED         BIT(1)
#define CHARGER_VOLTAGE_NOTREG          BIT(2)
#define CHARGER_CURRENT_NOTREG          BIT(3)
#define CHARGER_LEVEL_2                 BIT(4)
#define CHARGER_LEVEL_3                 BIT(5)
#define CHARGER_CURRENT_OR              BIT(6)
#define CHARGER_VOLTAGE_OR              BIT(7)
#define CHARGER_RES_OR                  BIT(8)
#define CHARGER_RES_COLD                BIT(9)
#define CHARGER_RES_HOT                 BIT(10)
#define CHARGER_RES_UR                  BIT(11)
#define CHARGER_ALARM_INHIBITED         BIT(12)
#define CHARGER_POWER_FAIL              BIT(13)
#define CHARGER_BATTERY_PRESENT         BIT(14)
#define CHARGER_AC_PRESENT              BIT(15)
/* Charger specification info */
#define INFO_CHARGER_SPEC(INFO)         ((INFO) & 0xf)
#define INFO_SELECTOR_SUPPORT(INFO)     (((INFO) >> 4) & 1)

/* Manufacturer Access parameters */
#define PARAM_SAFETY_STATUS             0x51
#define PARAM_OPERATION_STATUS          0x54
/* Operation status masks -- 6 byte reply */
/* reply[3] */
#define BATTERY_DISCHARGING_DISABLED    0x20
#define BATTERY_CHARGING_DISABLED       0x40

/* Read from battery */
int sb_read(int cmd, int *param);

/* Read sequence from battery */
int sb_read_string(int offset, uint8_t *data, int len);

/* Write to battery */
int sb_write(int cmd, int param);

/**
 * Write block to do battery cutoff
 *
 * @param reg		Battery cutoff register
 * @param val		Battery cutoff data value
 * @param len		Param val data length
 * @return          non-zero if error
 */
int sb_write_block(int reg, const uint8_t *val, int len);

/* Read manufactures access data from the battery */
int sb_read_mfgacc(int cmd, int block, uint8_t *data, int len);

#endif /* __CROS_EC_BATTERY_SMART_H */

