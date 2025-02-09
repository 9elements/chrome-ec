/* Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Battery pack vendor provided charging profile
 */

#include "battery.h"
#include "battery_smart.h"
#include "charge_state.h"
#include "console.h"
#include "driver/charger/rt946x.h"
#include "driver/tcpm/mt6370.h"
#include "ec_commands.h"
#include "extpower.h"
#include "gpio.h"
#include "hooks.h"
#include "power.h"
#include "usb_pd.h"
#include "util.h"

#if defined(CONFIG_BATTERY_MAX17055)
#include "driver/battery/max17055.h"
#elif defined(CONFIG_BATTERY_MM8013)
#include "driver/battery/mm8013.h"
#endif

#define TEMP_OUT_OF_RANGE TEMP_ZONE_COUNT

#if defined(BOARD_KRANE)
#define BATT_ID 1
#else
#define BATT_ID 0
#endif

#define BAT_LEVEL_PD_LIMIT 85
#define IBAT_PD_LIMIT 1000

#define BATTERY_SIMPLO_CHARGE_MIN_TEMP 0
#define BATTERY_SIMPLO_CHARGE_MAX_TEMP 60

#define CPRINTS(format, args...) cprints(CC_CHARGER, format, ## args)

enum battery_type {
	BATTERY_SIMPLO = 0,
	BATTERY_SCUD,
	BATTERY_COUNT
};

static const struct battery_info info[] = {
	[BATTERY_SIMPLO] = {
		.voltage_max		= 4400,
		.voltage_normal		= 3860,
		.voltage_min		= 3000,
		.precharge_current	= 256,
		.start_charging_min_c	= 0,
		.start_charging_max_c	= 45,
		.charging_min_c		= 0,
		.charging_max_c		= 60,
		.discharging_min_c	= -20,
		.discharging_max_c	= 60,
	},
	[BATTERY_SCUD] = {
		.voltage_max		= 4400,
		.voltage_normal		= 3850,
		.voltage_min		= 3400,
		.precharge_current	= 256,
		.start_charging_min_c	= 0,
		.start_charging_max_c	= 45,
		.charging_min_c		= 0,
		.charging_max_c		= 50,
		.discharging_min_c	= -20,
		.discharging_max_c	= 60,
	},
};

#ifdef CONFIG_BATTERY_MAX17055

#if BATT_ID == 1
#error "Battery profile for Mitsumi battery not available"
#endif

static const struct max17055_batt_profile batt_profile[] = {
	[BATTERY_SIMPLO] = {
		.is_ez_config		= 1,
		.design_cap		= MAX17055_DESIGNCAP_REG(6910),
		.ichg_term		= MAX17055_ICHGTERM_REG(235),
		.v_empty_detect		= MAX17055_VEMPTY_REG(3000, 3600),
	},
};

static const struct max17055_alert_profile alert_profile[] = {
	[BATTERY_SIMPLO] = {
		.v_alert_mxmn = VALRT_DISABLE,
		.t_alert_mxmn = MAX17055_TALRTTH_REG(
			BATTERY_SIMPLO_CHARGE_MAX_TEMP,
			BATTERY_SIMPLO_CHARGE_MIN_TEMP),
		.s_alert_mxmn = SALRT_DISABLE,
		.i_alert_mxmn = IALRT_DISABLE,
	},
};

const struct max17055_batt_profile *max17055_get_batt_profile(void)
{
	return &batt_profile[BATT_ID];
}

const struct max17055_alert_profile *max17055_get_alert_profile(void)
{
	return &alert_profile[BATT_ID];
}
#endif  /* CONFIG_BATTERY_MAX17055 */

const struct battery_info *battery_get_info(void)
{
	return &info[BATT_ID];
}

int board_cut_off_battery(void)
{
	/* The cut-off procedure is recommended by Richtek. b/116682788 */
	rt946x_por_reset();
	mt6370_vconn_discharge(0);
	rt946x_cutoff_battery();

	return EC_SUCCESS;
}

enum battery_disconnect_state battery_get_disconnect_state(void)
{
	if (battery_is_present() == BP_YES)
		return BATTERY_NOT_DISCONNECTED;
	return BATTERY_DISCONNECTED;
}

int charger_profile_override(struct charge_state_data *curr)
{
	static int previous_chg_limit_mv;
	int chg_limit_mv;
#ifdef CONFIG_BATTERY_MAX17055
	/* battery temp in 0.1 deg C */
	int bat_temp_c = curr->batt.temperature - 2731;

	/*
	 * Keep track of battery temperature range:
	 *
	 *        ZONE_0   ZONE_1     ZONE_2
	 * -----+--------+--------+------------+----- Temperature (C)
	 *      t0       t1       t2           t3
	 */
	enum {
		TEMP_ZONE_0, /* t0 < bat_temp_c <= t1 */
		TEMP_ZONE_1, /* t1 < bat_temp_c <= t2 */
		TEMP_ZONE_2, /* t2 < bat_temp_c <= t3 */
		TEMP_ZONE_COUNT
	} temp_zone;

	static struct {
		int temp_min; /* 0.1 deg C */
		int temp_max; /* 0.1 deg C */
		int desired_current; /* mA */
		int desired_voltage; /* mV */
	} temp_zones[BATTERY_COUNT][TEMP_ZONE_COUNT] = {
		[BATTERY_SIMPLO] = {
			/* TEMP_ZONE_0 */
			{BATTERY_SIMPLO_CHARGE_MIN_TEMP * 10, 150, 1772, 4376},
			/* TEMP_ZONE_1 */
			{150, 450, 4020, 4376},
			/* TEMP_ZONE_2 */
			{450, BATTERY_SIMPLO_CHARGE_MAX_TEMP * 10, 3350, 4300},
		},
		[BATTERY_SCUD] = {
			/* unused */
		},
	};
	BUILD_ASSERT(ARRAY_SIZE(temp_zones[0]) == TEMP_ZONE_COUNT);
	BUILD_ASSERT(ARRAY_SIZE(temp_zones) == BATTERY_COUNT);

	if ((curr->batt.flags & BATT_FLAG_BAD_TEMPERATURE) ||
	    (bat_temp_c < temp_zones[BATT_ID][0].temp_min) ||
	    (bat_temp_c >= temp_zones[BATT_ID][TEMP_ZONE_COUNT - 1].temp_max))
		temp_zone = TEMP_OUT_OF_RANGE;
	else {
		for (temp_zone = 0; temp_zone < TEMP_ZONE_COUNT; temp_zone++) {
			if (bat_temp_c <
				temp_zones[BATT_ID][temp_zone].temp_max)
				break;
		}
	}

	if (curr->state != ST_CHARGE)
		return 0;

	switch (temp_zone) {
	case TEMP_ZONE_0:
	case TEMP_ZONE_1:
	case TEMP_ZONE_2:
		curr->requested_current =
			temp_zones[BATT_ID][temp_zone].desired_current;
		curr->requested_voltage =
			temp_zones[BATT_ID][temp_zone].desired_voltage;
		break;
	case TEMP_OUT_OF_RANGE:
		curr->requested_current = curr->requested_voltage = 0;
		curr->batt.flags &= ~BATT_FLAG_WANT_CHARGE;
		curr->state = ST_IDLE;
		break;
	}
#endif  /* CONFIG_BATTERY_MAX17055 */
	/* TODO(b:131284131): Add battery configs for krane. */

	/* Limit input (=VBUS) to 5V when soc > 85% and charge current < 1A. */
	if (!(curr->batt.flags & BATT_FLAG_BAD_CURRENT) &&
			charge_get_percent() > BAT_LEVEL_PD_LIMIT &&
			curr->batt.current < 1000) {
		chg_limit_mv = 5500;
	} else if (IS_ENABLED(BOARD_KRANE) &&
			board_get_version() == 3 &&
			power_get_state() == POWER_S0) {
		/*
		 * TODO(b:134227872): limit power to 5V/2A in S0 to prevent
		 * overheat
		 */
		chg_limit_mv = 5500;
		curr->requested_current = 2000;
	} else {
		chg_limit_mv = PD_MAX_VOLTAGE_MV;
	}

	if (chg_limit_mv != previous_chg_limit_mv)
		CPRINTS("VBUS limited to %dmV", chg_limit_mv);
	previous_chg_limit_mv = chg_limit_mv;

	/* Pull down VBUS */
	if (pd_get_max_voltage() != chg_limit_mv)
		pd_set_external_voltage_limit(0, chg_limit_mv);

	/*
	 * When the charger says it's done charging, even if fuel gauge says
	 * SOC < BATTERY_LEVEL_NEAR_FULL, we'll overwrite SOC with
	 * BATTERY_LEVEL_NEAR_FULL. So we can ensure both Chrome OS UI
	 * and battery LED indicate full charge.
	 */
	if (rt946x_is_charge_done()) {
		curr->batt.state_of_charge = MAX(BATTERY_LEVEL_NEAR_FULL,
						 curr->batt.state_of_charge);
	}

	return 0;
}

static void board_charge_termination(void)
{
	static uint8_t te;
	/* Enable charge termination when we are sure battery is present. */
	if (!te && battery_is_present() == BP_YES) {
		if (!rt946x_enable_charge_termination(1))
			te = 1;
	}
}
DECLARE_HOOK(HOOK_BATTERY_SOC_CHANGE,
	     board_charge_termination,
	     HOOK_PRIO_DEFAULT);

/* Customs options controllable by host command. */
#define PARAM_FASTCHARGE (CS_PARAM_CUSTOM_PROFILE_MIN + 0)

enum ec_status charger_profile_override_get_param(uint32_t param,
						  uint32_t *value)
{
	return EC_RES_INVALID_PARAM;
}

enum ec_status charger_profile_override_set_param(uint32_t param,
						  uint32_t value)
{
	return EC_RES_INVALID_PARAM;
}
