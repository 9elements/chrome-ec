/* Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "charge_manager.h"
#include "console.h"
#include "hooks.h"
#include "host_command.h"
#include "usb_pd.h"
#include "usb_pd_config.h"
#include "util.h"

#define CPRINTS(format, args...) cprints(CC_USBCHARGE, format, ## args)

/* Keep track of available charge for each charge port. */
static struct charge_port_info available_charge[CHARGE_SUPPLIER_COUNT]
					       [PD_PORT_COUNT];

/* Store current state of port enable / charge current. */
static int charge_port = CHARGE_PORT_NONE;
static int charge_current = CHARGE_CURRENT_UNINITIALIZED;
static int charge_supplier = CHARGE_SUPPLIER_NONE;

/**
 * Initialize available charge. Run before board init, so board init can
 * initialize data, if needed.
 */
static void charge_manager_init(void)
{
	int i, j;

	for (i = 0; i < CHARGE_SUPPLIER_COUNT; ++i)
		for (j = 0; j < PD_PORT_COUNT; ++j) {
			available_charge[i][j].current =
				CHARGE_CURRENT_UNINITIALIZED;
			available_charge[i][j].voltage =
				CHARGE_VOLTAGE_UNINITIALIZED;
		}
}
DECLARE_HOOK(HOOK_INIT, charge_manager_init, HOOK_PRIO_DEFAULT-1);

/**
 * Returns 1 if all ports + suppliers have reported in with some initial charge,
 * 0 otherwise.
 */
static int charge_manager_is_seeded(void)
{
	/* Once we're seeded, we don't need to check again. */
	static int is_seeded;
	int i, j;

	if (is_seeded)
		return 1;

	for (i = 0; i < CHARGE_SUPPLIER_COUNT; ++i)
		for (j = 0; j < PD_PORT_COUNT; ++j)
			if (available_charge[i][j].current ==
			    CHARGE_CURRENT_UNINITIALIZED ||
			    available_charge[i][j].voltage ==
			    CHARGE_VOLTAGE_UNINITIALIZED)
				return 0;

	is_seeded = 1;
	return 1;
}

/**
 * Charge manager refresh -- responsible for selecting the active charge port
 * and charge power. Called as a deferred task.
 */
static void charge_manager_refresh(void)
{
	int new_supplier = CHARGE_SUPPLIER_NONE;
	int new_port = CHARGE_PORT_NONE;
	int new_charge_current, new_charge_voltage, i, j;

	/*
	 * Charge supplier selection logic:
	 * 1. Prefer higher priority supply.
	 * 2. Prefer higher power over lower in case priority is tied.
	 * available_charge can be changed at any time by other tasks,
	 * so make no assumptions about its consistency.
	 */
	for (i = 0; i < CHARGE_SUPPLIER_COUNT; ++i)
		for (j = 0; j < PD_PORT_COUNT; ++j)
			if (available_charge[i][j].current > 0 &&
			    available_charge[i][j].voltage > 0 &&
			    (new_supplier == CHARGE_SUPPLIER_NONE ||
			     supplier_priority[i] <
			     supplier_priority[new_supplier] ||
			    (supplier_priority[i] ==
			     supplier_priority[new_supplier] &&
			     POWER(available_charge[i][j]) >
			     POWER(available_charge[new_supplier]
						   [new_port])))) {
				new_supplier = i;
				new_port = j;
			}

	if (new_supplier == CHARGE_SUPPLIER_NONE)
		new_charge_current = new_charge_voltage = 0;
	else {
		new_charge_current =
			available_charge[new_supplier][new_port].current;
		new_charge_voltage =
			available_charge[new_supplier][new_port].voltage;
	}

	/* Change the charge limit + charge port if changed. */
	if (new_port != charge_port || new_charge_current != charge_current) {
		CPRINTS("New charge limit: supplier %d port %d current %d "
			"voltage %d", new_supplier, new_port,
			new_charge_current, new_charge_voltage);
		board_set_charge_limit(new_charge_current);
		board_set_active_charge_port(new_port);

		charge_current = new_charge_current;
		charge_supplier = new_supplier;
		charge_port = new_port;
	}
}
DECLARE_DEFERRED(charge_manager_refresh);

/**
 * Update available charge for a given port / supplier.
 *
 * @param supplier		Charge supplier to update.
 * @param charge_port		Charge port to update.
 * @param charge		Charge port current / voltage.
 */
void charge_manager_update(int supplier,
			   int charge_port,
			   struct charge_port_info *charge)
{
	if (supplier < 0 || supplier >= CHARGE_SUPPLIER_COUNT) {
		CPRINTS("Invalid charge supplier: %d", supplier);
		return;
	}

	/* Update charge table if needed. */
	if (available_charge[supplier][charge_port].current !=
		charge->current ||
		available_charge[supplier][charge_port].voltage !=
		charge->voltage) {
		available_charge[supplier][charge_port].current =
			charge->current;
		available_charge[supplier][charge_port].voltage =
			charge->voltage;

		/*
		 * Don't call charge_manager_refresh unless all ports +
		 * suppliers have reported in. We don't want to make changes
		 * to our charge port until we are certain we know what is
		 * attached.
		 */
		if (charge_manager_is_seeded())
			hook_call_deferred(charge_manager_refresh, 0);
	}
}

static int hc_pd_power_info(struct host_cmd_handler_args *args)
{
	const struct ec_params_usb_pd_power_info *p = args->params;
	struct ec_response_usb_pd_power_info *r = args->response;
	int port = p->port;
	int sup = CHARGE_SUPPLIER_NONE;
	int i;

	/* If host is asking for the charging port, set port appropriately */
	if (port == PD_POWER_CHARGING_PORT)
		port = charge_port;

	/* Determine supplier information to show */
	if (port == charge_port) {
		sup = charge_supplier;
	} else {
		/* Find highest priority supplier */
		for (i = 0; i < CHARGE_SUPPLIER_COUNT; ++i) {
			if (available_charge[i][port].current > 0 &&
			    available_charge[i][port].voltage > 0 &&
			    (sup == CHARGE_SUPPLIER_NONE ||
			     supplier_priority[i] <
			     supplier_priority[sup] ||
			    (supplier_priority[i] ==
			     supplier_priority[sup] &&
			     POWER(available_charge[i][port]) >
			     POWER(available_charge[sup]
						   [port]))))
				sup = i;
		}
	}

	/* Fill in power role */
	if (charge_port == port)
		r->role = USB_PD_PORT_POWER_SINK;
	else if (sup != CHARGE_SUPPLIER_NONE)
		r->role = USB_PD_PORT_POWER_SINK_NOT_CHARGING;
	else if (pd_is_connected(port) && pd_get_role(port) == PD_ROLE_SOURCE)
		r->role = USB_PD_PORT_POWER_SOURCE;
	else
		r->role = USB_PD_PORT_POWER_DISCONNECTED;

	/* Is port partner dual-role capable */
	r->dualrole = pd_get_partner_dualrole_capable(port);

	if (sup == CHARGE_SUPPLIER_NONE) {
		r->type = USB_CHG_TYPE_NONE;
		r->voltage_max = 0;
		r->voltage_now = 0;
		r->current_max = 0;
		r->max_power = 0;
	} else {
		switch (sup) {
		case CHARGE_SUPPLIER_PD:
			r->type = USB_CHG_TYPE_PD;
			break;
		case CHARGE_SUPPLIER_TYPEC:
			r->type = USB_CHG_TYPE_C;
			break;
		case CHARGE_SUPPLIER_PROPRIETARY:
			r->type = USB_CHG_TYPE_PROPRIETARY;
			break;
		case CHARGE_SUPPLIER_BC12_DCP:
			r->type = USB_CHG_TYPE_BC12_DCP;
			break;
		case CHARGE_SUPPLIER_BC12_CDP:
			r->type = USB_CHG_TYPE_BC12_CDP;
			break;
		case CHARGE_SUPPLIER_BC12_SDP:
			r->type = USB_CHG_TYPE_BC12_SDP;
			break;
		default:
			r->type = USB_CHG_TYPE_OTHER;
		}
		r->voltage_max = available_charge[sup][port].voltage;
		r->current_max = available_charge[sup][port].current;
		r->max_power = POWER(available_charge[sup][port]);

		/*
		 * If we are sourcing power, or sinking but not charging, then
		 * VBUS must be 5V. If we are charging, then read VBUS ADC.
		 */
		if (r->role == USB_PD_PORT_POWER_SOURCE ||
		    r->role == USB_PD_PORT_POWER_SINK_NOT_CHARGING)
			r->voltage_now = 5000;
		else
			r->voltage_now = adc_read_channel(ADC_BOOSTIN);
	}

	args->response_size = sizeof(*r);
	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_USB_PD_POWER_INFO,
		     hc_pd_power_info,
		     EC_VER_MASK(0));
