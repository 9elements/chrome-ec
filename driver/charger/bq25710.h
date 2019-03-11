/* Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * TI bq25710 battery charger driver.
 */

#ifndef __CROS_EC_BQ25710_H
#define __CROS_EC_BQ25710_H

/* SMBUS Interface */
#define BQ25710_SMBUS_ADDR1 0x12

#define BQ25710_BC12_MIN_VOLTAGE_MV	1408

/* Registers */
#define BQ25710_REG_CHARGE_OPTION_0		0x12
#define BQ25710_REG_CHARGE_CURRENT		0x14
#define BQ25710_REG_MAX_CHARGE_VOLTAGE		0x15
#define BQ25710_REG_CHARGE_OPTION_1		0x30
#define BQ25710_REG_CHARGE_OPTION_2		0x31
#define BQ25710_REG_CHARGE_OPTION_3		0x32
#define BQ25710_REG_PROCHOT_OPTION_0		0x33
#define BQ25710_REG_PROCHOT_OPTION_1		0x34
#define BQ25710_REG_ADC_OPTION			0x35
#define BQ25710_REG_CHARGER_STATUS		0x20
#define BQ25710_REG_PROCHOT_STATUS		0x21
#define BQ25710_REG_IIN_DPM			0x22
#define BQ25710_REG_ADC_VBUS_PSYS		0x23
#define BQ25710_REG_ADC_IBAT			0x24
#define BQ25710_REG_ADC_CMPIN_IIN		0x25
#define BQ25710_REG_ADC_VSYS_VBAT		0x2C
#define BQ25710_REG_PROCHOT_OPTION_1		0x34
#define BQ25710_REG_OTG_VOLTAGE			0x3B
#define BQ25710_REG_OTG_CURRENT			0x3C
#define BQ25710_REG_INPUT_VOLTAGE		0x3D
#define BQ25710_REG_MIN_SYSTEM_VOLTAGE		0x3E
#define BQ25710_REG_IIN_HOST			0x3F
#define BQ25710_REG_MANUFACTURER_ID		0xFE
#define BQ25710_REG_DEVICE_ADDRESS		0xFF

/* ChargeOption0 Register */
#define BQ25710_CHARGE_OPTION_0_LOW_POWER_MODE	BIT(15)
#define BQ25710_CHARGE_OPTION_0_EN_LEARN	BIT(5)
#define BQ25710_CHARGE_OPTION_0_CHRG_INHIBIT	BIT(0)

/* ChargeOption2 Register */
#define BQ25710_CHARGE_OPTION_2_EN_EXTILIM	BIT(7)

/* ChargeOption3 Register */
#define BQ25710_CHARGE_OPTION_3_EN_ICO_MODE	BIT(11)

/* ChargeStatus Register */
#define BQ25710_CHARGE_STATUS_ICO_DONE		BIT(14)

/* IIN_DPM Register */
#define BQ25710_CHARGE_IIN_BIT_0FFSET		8
#define BQ25710_CHARGE_MA_PER_STEP		50

/* ADCOption Register */
#define BQ25710_ADC_OPTION_ADC_START		BIT(14)
#define BQ25710_ADC_OPTION_EN_ADC_VBUS		BIT(6)
#define BQ25710_ADC_OPTION_EN_ADC_IIN		BIT(4)
#define BQ25710_ADC_OPTION_EN_ADC_ALL		0xFF

/* ADCVBUS/PSYS Register */
#define BQ25710_ADC_VBUS_STEP_MV		64
#define BQ25710_ADC_VBUS_BASE_MV		3200
#define BQ25710_ADC_VBUS_STEP_BIT_OFFSET	8

/* ADCIIN Register */
#define BQ25710_ADC_IIN_STEP_MA			50
#define BQ25710_ADC_IIN_STEP_BIT_OFFSET		8

/* ProchotOption1 Register */
#define BQ25710_PROCHOT_PROFILE_VDPM		BIT(7)

#endif /* __CROS_EC_BQ25710_H */
