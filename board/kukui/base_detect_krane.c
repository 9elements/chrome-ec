/* Copyright 2019 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "adc.h"
#include "board.h"
#include "charge_manager.h"
#include "console.h"
#include "gpio.h"
#include "hooks.h"
#include "timer.h"
#include "usb_pd.h"
#include "util.h"

#define CPRINTS(format, args...) cprints(CC_USB, format, ## args)

/* Krane base detection code */

/* Base detection and debouncing */
#define BASE_DETECT_DEBOUNCE_US (20 * MSEC)

/*
 * If the base status is unclear (i.e. not within expected ranges, read
 * the ADC value again every 500ms.
 */
#define BASE_DETECT_RETRY_US (500 * MSEC)

enum kukui_pogo_device_type {
	DEVICE_TYPE_ERROR = -2,
	DEVICE_TYPE_UNKNOWN = -1,
	DEVICE_TYPE_DETACHED = 0,
	DEVICE_TYPE_DOCK = 1,
	DEVICE_TYPE_KEYBOARD = 2,
	DEVICE_TYPE_COUNT,
};

struct {
	int mv_low, mv_high;
} static const pogo_detect_table[] = {
	[DEVICE_TYPE_DETACHED] = {2700, 3500}, /* 10K, NC, around 3.3V */
	[DEVICE_TYPE_DOCK] = {141, 173},       /* 10K, 0.5K ohm */
	[DEVICE_TYPE_KEYBOARD] = {270, 400},   /* 10K, 1K ohm */
};
BUILD_ASSERT(ARRAY_SIZE(pogo_detect_table) == DEVICE_TYPE_COUNT);

static uint64_t base_detect_debounce_time;

static enum kukui_pogo_device_type get_device_type(int mv)
{
	int i;

	if (mv == ADC_READ_ERROR)
		return DEVICE_TYPE_ERROR;

	for (i = 0; i < DEVICE_TYPE_COUNT; i++) {
		if (pogo_detect_table[i].mv_low <= mv &&
				mv <= pogo_detect_table[i].mv_high)
			return i;
	}

	return DEVICE_TYPE_UNKNOWN;
}

static void enable_charge(int enable)
{
	if (enable) {
		struct charge_port_info info = {
			.voltage = 5000, .current = 1500};
		/*
		 * Set supplier type to PD to have same priority as type c
		 * port.
		 */
		charge_manager_update_charge(
			CHARGE_SUPPLIER_DEDICATED, CHARGE_PORT_POGO, &info);
	} else {
		charge_manager_update_charge(
			CHARGE_SUPPLIER_DEDICATED, CHARGE_PORT_POGO, NULL);
	}
	pd_send_host_event(PD_EVENT_POWER_CHANGE);
}

static void enable_power_supply(int enable)
{
	gpio_set_level(GPIO_EN_PP3300_POGO, enable);
}

static void base_detect_deferred(void);
DECLARE_DEFERRED(base_detect_deferred);

static void base_detect_deferred(void)
{
	uint64_t time_now = get_time().val;
	int mv;
	int device_type;

	if (base_detect_debounce_time > time_now) {
		hook_call_deferred(&base_detect_deferred_data,
				   base_detect_debounce_time - time_now);
		return;
	}

	/*
	 * Disable interrupt first to prevent it triggered by value
	 * changed from 1 to disabled state(=0).
	 */
	gpio_disable_interrupt(GPIO_POGO_ADC_INT_L);
	gpio_set_flags(GPIO_POGO_ADC_INT_L, GPIO_ANALOG);
	mv = adc_read_channel(ADC_POGO_ADC_INT_L);
	/* restore the pin function */
	gpio_set_flags(GPIO_POGO_ADC_INT_L, GPIO_INT_BOTH);
	gpio_enable_interrupt(GPIO_POGO_ADC_INT_L);

	device_type = get_device_type(mv);
	CPRINTS("POGO: adc=%d, device_type=%d", mv, device_type);

	switch (device_type) {
	case DEVICE_TYPE_ERROR:
	case DEVICE_TYPE_UNKNOWN:
		hook_call_deferred(&base_detect_deferred_data,
				BASE_DETECT_RETRY_US);
		break;

	case DEVICE_TYPE_DETACHED:
		enable_power_supply(0);
		enable_charge(0);
		break;

	case DEVICE_TYPE_DOCK:
		enable_power_supply(0);
		enable_charge(1);
		break;

	case DEVICE_TYPE_KEYBOARD:
		enable_charge(0);
		enable_power_supply(1);
		break;

	case DEVICE_TYPE_COUNT:
		/* should not happen */
		break;
	}
}

void pogo_adc_interrupt(enum gpio_signal signal)
{
	uint64_t time_now = get_time().val;

	if (base_detect_debounce_time <= time_now) {
		hook_call_deferred(&base_detect_deferred_data,
				   BASE_DETECT_DEBOUNCE_US);
	}

	base_detect_debounce_time = time_now + BASE_DETECT_DEBOUNCE_US;
}

static void base_init(void)
{
	hook_call_deferred(&base_detect_deferred_data, 0);
}
DECLARE_HOOK(HOOK_INIT, base_init, HOOK_PRIO_INIT_ADC + 1);
