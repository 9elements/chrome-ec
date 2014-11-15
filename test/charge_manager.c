/* Copyright 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Test charge manager module.
 */

#include "charge_manager.h"
#include "test_util.h"
#include "timer.h"
#include "usb_pd_config.h"
#include "util.h"

#define CHARGE_MANAGER_SLEEP_MS 50

/* Charge supplier priority: lower number indicates higher priority. */
const int supplier_priority[] = {
	[CHARGE_SUPPLIER_TEST1] = 0,
	[CHARGE_SUPPLIER_TEST2] = 1,
	[CHARGE_SUPPLIER_TEST3] = 1,
	[CHARGE_SUPPLIER_TEST4] = 1,
	[CHARGE_SUPPLIER_TEST5] = 3,
	[CHARGE_SUPPLIER_TEST6] = 3,
	[CHARGE_SUPPLIER_TEST7] = 5,
	[CHARGE_SUPPLIER_TEST8] = 6,
	[CHARGE_SUPPLIER_TEST9] = 6,
};
BUILD_ASSERT(ARRAY_SIZE(supplier_priority) == CHARGE_SUPPLIER_COUNT);

static unsigned int active_charge_limit = CHARGE_SUPPLIER_NONE;
static unsigned int active_charge_port = CHARGE_PORT_NONE;
static int new_power_request[PD_PORT_COUNT];

/* Callback functions called by CM on state change */
void board_set_charge_limit(int charge_ma)
{
	active_charge_limit = charge_ma;
}

void board_set_active_charge_port(int charge_port)
{
	active_charge_port = charge_port;
}

void pd_set_new_power_request(int port)
{
	new_power_request[port] = 1;
}

void clear_new_power_requests(void)
{
	int i;
	for (i = 0; i < PD_PORT_COUNT; ++i)
		new_power_request[i] = 0;
}

static void wait_for_charge_manager_refresh(void)
{
	msleep(CHARGE_MANAGER_SLEEP_MS);
}

static void initialize_charge_table(int current, int voltage, int ceil)
{
	int i, j;
	struct charge_port_info charge;

	charge.current = current;
	charge.voltage = voltage;

	for (i = 0; i < PD_PORT_COUNT; ++i) {
		charge_manager_set_ceil(i, ceil);
		for (j = 0; j < CHARGE_SUPPLIER_COUNT; ++j)
			charge_manager_update(j, i, &charge);
	}
	wait_for_charge_manager_refresh();
}

static int test_initialization(void)
{
	int i, j;
	struct charge_port_info charge;

	/*
	 * No charge port should be selected until all ports + suppliers
	 * have reported in with an initial charge.
	 */
	TEST_ASSERT(active_charge_port == CHARGE_PORT_NONE);
	charge.current = 1000;
	charge.voltage = 5000;

	/* Initialize all supplier/port pairs, except for the last one */
	for (i = 0; i < CHARGE_SUPPLIER_COUNT; ++i)
		for (j = 0; j < PD_PORT_COUNT; ++j) {
			if (i == CHARGE_SUPPLIER_COUNT - 1 &&
			    j == PD_PORT_COUNT - 1)
				break;
			charge_manager_update(i, j, &charge);
		}

	/* Verify no active charge port, since all pairs haven't updated */
	wait_for_charge_manager_refresh();
	TEST_ASSERT(active_charge_port == CHARGE_PORT_NONE);

	/* Update last pair and verify a charge port has been selected */
	charge_manager_update(CHARGE_SUPPLIER_COUNT-1,
			      PD_PORT_COUNT-1,
			      &charge);
	wait_for_charge_manager_refresh();
	TEST_ASSERT(active_charge_port != CHARGE_PORT_NONE);

	return EC_SUCCESS;
}

static int test_priority(void)
{
	struct charge_port_info charge;

	/* Initialize table to no charge */
	initialize_charge_table(0, 5000, 5000);
	TEST_ASSERT(active_charge_port == CHARGE_PORT_NONE);

	/*
	 * Set a 1A charge via a high-priority supplier and a 2A charge via
	 * a low-priority supplier, and verify the HP supplier is chosen.
	 */
	charge.current = 2000;
	charge.voltage = 5000;
	charge_manager_update(CHARGE_SUPPLIER_TEST6, 0, &charge);
	charge.current = 1000;
	charge_manager_update(CHARGE_SUPPLIER_TEST2, 1, &charge);
	wait_for_charge_manager_refresh();
	TEST_ASSERT(active_charge_port == 1);
	TEST_ASSERT(active_charge_limit == 1000);

	/*
	 * Set a higher charge on a LP supplier and verify we still use the
	 * lower charge.
	 */
	charge.current = 1500;
	charge_manager_update(CHARGE_SUPPLIER_TEST7, 1, &charge);
	wait_for_charge_manager_refresh();
	TEST_ASSERT(active_charge_port == 1);
	TEST_ASSERT(active_charge_limit == 1000);

	/*
	 * Zero our HP charge and verify fallback to next highest priority,
	 * which happens to be a different port.
	 */
	charge.current = 0;
	charge_manager_update(CHARGE_SUPPLIER_TEST2, 1, &charge);
	wait_for_charge_manager_refresh();
	TEST_ASSERT(active_charge_port == 0);
	TEST_ASSERT(active_charge_limit == 2000);

	/* Add a charge at equal priority and verify highest charge selected */
	charge.current = 2500;
	charge_manager_update(CHARGE_SUPPLIER_TEST5, 0, &charge);
	wait_for_charge_manager_refresh();
	TEST_ASSERT(active_charge_port == 0);
	TEST_ASSERT(active_charge_limit == 2500);

	charge.current = 3000;
	charge_manager_update(CHARGE_SUPPLIER_TEST6, 1, &charge);
	wait_for_charge_manager_refresh();
	TEST_ASSERT(active_charge_port == 1);
	TEST_ASSERT(active_charge_limit == 3000);

	return EC_SUCCESS;
}

static int test_charge_ceil(void)
{
	int port;
	struct charge_port_info charge;

	/* Initialize table to 1A @ 5V, and verify port + limit */
	initialize_charge_table(1000, 5000, 1000);
	TEST_ASSERT(active_charge_port != CHARGE_PORT_NONE);
	TEST_ASSERT(active_charge_limit == 1000);

	/* Set a 500mA ceiling, verify port is unchanged */
	port = active_charge_port;
	charge_manager_set_ceil(port, 500);
	wait_for_charge_manager_refresh();
	TEST_ASSERT(port == active_charge_port);
	TEST_ASSERT(active_charge_limit == 500);

	/* Raise the ceiling to 2A, verify limit goes back to 1A */
	charge_manager_set_ceil(port, 2000);
	wait_for_charge_manager_refresh();
	TEST_ASSERT(port == active_charge_port);
	TEST_ASSERT(active_charge_limit == 1000);

	/* Verify that ceiling is ignored in determining active charge port */
	charge.current = 2000;
	charge.voltage = 5000;
	charge_manager_update(0, 0, &charge);
	charge.current = 2500;
	charge_manager_update(0, 1, &charge);
	charge_manager_set_ceil(1, 750);
	wait_for_charge_manager_refresh();
	TEST_ASSERT(active_charge_port == 1);
	TEST_ASSERT(active_charge_limit == 750);

	return EC_SUCCESS;
}

static int test_new_power_request(void)
{
	struct charge_port_info charge;

	/* Initialize table to no charge */
	initialize_charge_table(0, 5000, 5000);
	/* Clear power requests, and verify they are zero'd */
	clear_new_power_requests();
	wait_for_charge_manager_refresh();
	TEST_ASSERT(new_power_request[0] == 0);
	TEST_ASSERT(new_power_request[1] == 0);

	/* Charge from port 1 and verify NPR on port 1 only */
	charge.current = 1000;
	charge.voltage = 5000;
	charge_manager_update(CHARGE_SUPPLIER_TEST2, 1, &charge);
	wait_for_charge_manager_refresh();
	TEST_ASSERT(new_power_request[0] == 0);
	TEST_ASSERT(new_power_request[1] == 1);
	clear_new_power_requests();

	/* Reduce port 1 limit and verify NPR on port 1 only */
	charge_manager_set_ceil(1, 500);
	wait_for_charge_manager_refresh();
	TEST_ASSERT(new_power_request[0] == 0);
	TEST_ASSERT(new_power_request[1] == 1);
	clear_new_power_requests();

	/* Add low-priority source and verify no NPRs */
	charge_manager_update(CHARGE_SUPPLIER_TEST6, 0, &charge);
	wait_for_charge_manager_refresh();
	TEST_ASSERT(new_power_request[0] == 0);
	TEST_ASSERT(new_power_request[1] == 0);
	clear_new_power_requests();

	/*
	 * Add higher-priority source and verify NPR on both ports,
	 * since we're switching charge ports.
	 */
	charge_manager_update(CHARGE_SUPPLIER_TEST1, 0, &charge);
	wait_for_charge_manager_refresh();
	TEST_ASSERT(new_power_request[0] == 1);
	TEST_ASSERT(new_power_request[1] == 1);
	clear_new_power_requests();

	return EC_SUCCESS;
}

void run_test(void)
{
	test_reset();

	RUN_TEST(test_initialization);
	RUN_TEST(test_priority);
	RUN_TEST(test_charge_ceil);
	RUN_TEST(test_new_power_request);

	test_print_result();
}
