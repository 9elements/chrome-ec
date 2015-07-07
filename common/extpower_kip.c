/* Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/*
 * The limits vary depending on each adapter's power rating, so we
 * need to watch for changes and adjust the limits and high-current thresholds
 * accordingly. If we go over, the AP needs to throttle itself. The EC's
 * charging state logic isn't affected, just the AP's P-State. We try to save
 * PROCHOT as a last resort.
 */

#include <limits.h>				/* part of the compiler */

#include "adc.h"
#include "battery_smart.h"
#include "charge_state.h"
#include "charger.h"
#include "chipset.h"
#include "common.h"
#include "console.h"
#include "driver/charger/bq24715.h"
#include "extpower.h"
#include "extpower_kip.h"
#include "hooks.h"
#include "host_command.h"
#include "system.h"
#include "throttle_ap.h"
#include "util.h"

/* Console output macros */
#define CPUTS(outstr) cputs(CC_CHARGER, outstr)
#define CPRINTF(format, args...) cprintf(CC_CHARGER, format, ## args)

/* Values for our supported adapters */
static const char * const ad_name[] = {
	"unknown",
	"45W",
	"65W",
	"90W"
};
BUILD_ASSERT(ARRAY_SIZE(ad_name) == NUM_ADAPTER_TYPES);

test_export_static
struct adapter_id_vals ad_id_vals[] = {
	/* mV low, mV high */
	{INT_MIN, INT_MAX},			/* anything = ADAPTER_UNKNOWN */
	{434,     554},				/* ADAPTER_45W */
	{561,     717},				/* ADAPTER_65W */
	{725,     925}				/* ADAPTER_90W */
};
BUILD_ASSERT(ARRAY_SIZE(ad_id_vals) == NUM_ADAPTER_TYPES);

test_export_static
int ad_input_current[] = {
	/*
	 * Current limits in mA for each adapter.
	 * Values are in hex to avoid roundoff, because the BQ24715 Input
	 * Current Register masks off bits 6-0.
	 *
	 * Note that this is very specific to the combinations of adapters and
	 * BQ24715 charger chip on Kip.
	 */

	0x0800,			/* ADAPTER_UNKNOWN ~ 2.0 A */
	0x0800,			/* ADAPTER_45W ~ 2.0 A */
	0x0c00,			/* ADAPTER_65W ~ 3.0 A */
	0x1100			/* ADAPTER_90W ~ 4.3 A */
};
BUILD_ASSERT(ARRAY_SIZE(ad_input_current) == NUM_ADAPTER_TYPES);

test_export_static
struct adapter_limits ad_limits[][NUM_AC_THRESHOLDS] = {
	/* ADAPTER_UNKNOWN - treat as 45W */
	{
		{ 2310, 1960, 16, 80, },
		{ 2560, 2210, 1, 80, }
	},
	/* ADAPTER_45W */
	{
		{ 2310, 1960, 16, 80, },
		{ 2560, 2210, 1, 80, }
	},
	/* ADAPTER_65W */
	{
		{ 3330, 2980, 16, 80, },
		{ 3590, 3240, 1, 80, }
	},
	/* ADAPTER_90W */
	{
		{ 4620, 4270, 16, 80, },
		{ 4870, 4520, 1, 80, }
	}
};
BUILD_ASSERT(ARRAY_SIZE(ad_limits) == NUM_ADAPTER_TYPES);

/* The battery current limits are independent of adapter rating.
 * hi_val and lo_val are DISCHARGE current in mA.
 */
test_export_static
struct adapter_limits batt_limits[][NUM_BATT_THRESHOLDS] = {
	{
		{ 5500, 5000, 16, 50, },
		{ 6000, 5500, 1, 50, },
	},
	/* The battery discharge OCP table for Kip14 */
	{
		{ 4000, 3500, 16, 50, },
		{ 5000, 4500, 1, 50, },
	},
};
BUILD_ASSERT(ARRAY_SIZE(batt_limits) == NUM_BATT_THRESHOLDS);

static int last_mv;
static enum adapter_type identify_adapter(void)
{
	int i;
	last_mv = adc_read_channel(ADC_AC_ADAPTER_ID_VOLTAGE);

	/* ADAPTER_UNKNOWN matches everything, so search backwards */
	for (i = NUM_ADAPTER_TYPES - 1; i >= 0; i--)
		if (last_mv >= ad_id_vals[i].lo && last_mv <= ad_id_vals[i].hi)
			return i;

	return ADAPTER_UNKNOWN;			/* should never get here */
}

test_export_static enum adapter_type ac_adapter;
static void ac_change_callback(void)
{
	if (extpower_is_present()) {
		ac_adapter = identify_adapter();
		CPRINTF("[%T AC Adapter is %s (%dmv)]\n",
			ad_name[ac_adapter], last_mv);
	} else {
		ac_adapter = ADAPTER_UNKNOWN;
		CPRINTF("[%T AC Adapter is not present]\n");
		/* Charger unavailable. Clear local flags */
	}
}
DECLARE_HOOK(HOOK_AC_CHANGE, ac_change_callback, HOOK_PRIO_DEFAULT);

static void set_ad_input_current(void)
{
	int r;

	/* Set allowed Io based on adapter. The charger will sometimes change
	 * this setting all by itself due to inrush current limiting, so we
	 * can't assume it stays where we put it. */
	r = charger_set_input_current(ad_input_current[ac_adapter]);
	if (r != EC_SUCCESS)
		goto bad;

	return;
bad:
	CPRINTF("[%T ERROR: can't talk to charger: %d]\n", r);
}


/* We need to OR all the possible reasons to throttle in order to decide
 * whether it should happen or not. Use one bit per reason.
 */
#define BATT_REASON_OFFSET 0
#define AC_REASON_OFFSET NUM_BATT_THRESHOLDS
BUILD_ASSERT(NUM_BATT_THRESHOLDS + NUM_AC_THRESHOLDS < 32);

test_export_static uint32_t ap_is_throttled;
static void set_throttle(int on, int whosays)
{
	if (on)
		ap_is_throttled |= (1 << whosays);
	else
		ap_is_throttled &= ~(1 << whosays);

	throttle_ap(ap_is_throttled ? THROTTLE_ON : THROTTLE_OFF,
		    THROTTLE_HARD, THROTTLE_SRC_POWER);
}

test_export_static
void check_threshold(int current, struct adapter_limits *lim, int whoami)
{
	if (lim->triggered) {
		/* watching for current to drop */
		if (current < lim->lo_val) {
			if (++lim->count >= lim->lo_cnt) {
				set_throttle(0, whoami);
				lim->count = 0;
				lim->triggered = 0;
			}
		} else {
			lim->count = 0;
		}
	} else {
		/* watching for current to rise */
		if (current > lim->hi_val) {
			if (++lim->count >= lim->hi_cnt) {
				set_throttle(1, whoami);
				lim->count = 0;
				lim->triggered = 1;
			}
		} else {
			lim->count = 0;
		}
	}
}


test_export_static
void watch_battery_closely(struct charge_state_context *ctx)
{
	int i;
	int v = ((system_get_board_version() & 0x4) == 0x4 ? 1 : 0);
	int current = ctx->curr.batt.current;

	/* NB: The values in batt_limits[] indicate DISCHARGE current (mA).
	 * However, the value returned from battery_current() is CHARGE
	 * current: postive for charging and negative for discharging.
	 *
	 * Turbo mode can discharge the battery even while connected to the
	 * charger. The spec says not to turn throttling off until the battery
	 * drain has been below the threshold for 5 seconds. That means we
	 * still need to check while on AC, or else just plugging the adapter
	 * in and out would mess up that 5-second timeout. Since the threshold
	 * logic uses signed numbers to compare the limits, everything Just
	 * Works.
	 */

	/* Check limits against DISCHARGE current, not CHARGE current! */
	for (i = 0; i < NUM_BATT_THRESHOLDS; i++)
		check_threshold(-current, &batt_limits[v][i],
				/* invert sign! */
				i + BATT_REASON_OFFSET);
}

void watch_adapter_closely(struct charge_state_context *ctx)
{
	int current, i;

	/* We always watch the battery current drain, even when on AC. */
	watch_battery_closely(ctx);

	/* If AC present, we can set adpter input current for each adpater. */
	if (extpower_is_present())
		set_ad_input_current();

	/* If the AP is off, we won't need to throttle it. */
	if (chipset_in_state(CHIPSET_STATE_ANY_OFF |
			     CHIPSET_STATE_SUSPEND))
		return;

	/* Check all the thresholds. */
	current = adc_read_channel(ADC_CH_CHARGER_CURRENT);
	for (i = 0; i < NUM_AC_THRESHOLDS; i++)
		check_threshold(current, &ad_limits[ac_adapter][i],
			i + AC_REASON_OFFSET);
}

static int command_adapter(int argc, char **argv)
{
	enum adapter_type v = identify_adapter();
	ccprintf("Adapter %s (%dmv), ap_is_throttled 0x%08x\n",
		 ad_name[v], last_mv, ap_is_throttled);
	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(adapter, command_adapter,
			NULL,
			"Display AC adapter information",
			NULL);
