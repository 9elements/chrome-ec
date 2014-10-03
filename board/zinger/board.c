/* Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
/* Tiny charger configuration */

#include "common.h"
#include "debug.h"
#include "registers.h"
#include "rsa.h"
#include "sha1.h"
#include "sha256.h"
#include "task.h"
#include "usb_pd.h"
#include "util.h"

/* Insert the RSA public key definition */
const struct rsa_public_key pkey __attribute__((section(".rsa_pubkey"))) =
#include "gen_pub_key.h"
/* The RSA signature is stored at the end of the RW firmware */
static const void *rw_sig = (void *)CONFIG_FLASH_BASE + CONFIG_FW_RW_OFF
				 + CONFIG_FW_RW_SIZE - RSANUMBYTES;
/* Large 768-Byte buffer for RSA computation : could be re-use afterwards... */
static uint32_t rsa_workbuf[3 * RSANUMWORDS];

extern void pd_rx_handler(void);

/* RW firmware reset vector */
static uint32_t * const rw_rst =
	(uint32_t *)(CONFIG_FLASH_BASE+CONFIG_FW_RW_OFF+4);

/* External interrupt EXTINT7 for external comparator on PA7 */
void pd_rx_interrupt(void)
{
	/* trigger reception handling */
	pd_rx_handler();
}
DECLARE_IRQ(STM32_IRQ_EXTI4_15, pd_rx_interrupt, 1);

static void jump_to_rw(void)
{
	void (*jump_rw_rst)(void) = (void *)*rw_rst;

	debug_printf("Jump to RW\n");
	/* Disable interrupts */
	asm volatile("cpsid i");
	/* Call RW firmware reset vector */
	jump_rw_rst();
}

int is_ro_mode(void)
{
	return (uint32_t)&jump_to_rw < (uint32_t)rw_rst;
}

static int check_rw_valid(void)
{
	int good;
	uint8_t *hash;

	/* Check if we have a RW firmware flashed */
	if (*rw_rst == 0xffffffff)
		return 0;

	hash = flash_hash_rw();
	good = rsa_verify(&pkey, (void *)rw_sig, (void *)hash, rsa_workbuf);
	if (!good) {
		debug_printf("RSA verify FAILED\n");
		return 0;
	}

	return 1;
}

extern void pd_task(void);

int main(void)
{
	hardware_init();
	debug_printf("Power supply started ... %s\n",
		is_ro_mode() ? "RO" : "RW");

	/* Verify RW firmware and use it if valid */
	if (is_ro_mode() && check_rw_valid())
		jump_to_rw();

	/* background loop for PD events */
	pd_task();

	debug_printf("background loop exited !\n");
	/* we should never reach that point */
	cpu_reset();
	return 0;
}
