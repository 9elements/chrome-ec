/* Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "console.h"
#include "hooks.h"
#include "registers.h"

#define CPRINTS(format, args...) cprints(CC_SYSTEM, format, ## args)
#define CPRINTF(format, args...) cprintf(CC_SYSTEM, format, ## args)

#define AP_TX_TERM	(1 << 0)
#define SPS_TERM	(1 << 1)

static uint8_t term_enabled;

static void update_term_state(int term, int enable)
{
	if (enable)
		term_enabled |= term;
	else
		term_enabled &= ~term;
}

int board_s3_term_is_enabled(void)
{
	return term_enabled;
}

static void ap_tx_term_enable(int term_enable)
{
	/* Add a pulldown to AP TX Cr50 RX */
	GWRITE_FIELD(PINMUX, DIOA3_CTL, PD, term_enable);
	update_term_state(AP_TX_TERM, term_enable);
}

static void sps_enable_pd(int term_enable)
{
	GWRITE_FIELD(PINMUX, DIOA2_CTL, PD, term_enable);	/* SPS_MOSI */
	GWRITE_FIELD(PINMUX, DIOA6_CTL, PD, term_enable);	/* SPS_CLK */
	GWRITE_FIELD(PINMUX, DIOA10_CTL, PD, term_enable);	/* SPS_MISO */
	GWRITE_FIELD(PINMUX, DIOA12_CTL, PD, term_enable);	/* SPS_CS_L */
}

static void sps_enable_inputs(int input_enable)
{
	GWRITE_FIELD(PINMUX, DIOA2_CTL, IE, input_enable);	/* SPS_MOSI */
	GWRITE_FIELD(PINMUX, DIOA6_CTL, IE, input_enable);	/* SPS_CLK */
	GWRITE_FIELD(PINMUX, DIOA10_CTL, IE, 0);		/* SPS_MISO */
	GWRITE_FIELD(PINMUX, DIOA12_CTL, IE, input_enable);	/* SPS_CS_L */
}

static void sps_term_enable(int term_enable)
{
	/* Disable the sps inputs before enabling the pulldowns */
	if (!term_enable)
		sps_enable_inputs(!term_enable);

	/* Control the pulldowns on the SPS signals */
	sps_enable_pd(term_enable);

	/* Reenable the sps inputs after enabling the pulldowns */
	if (term_enable)
		sps_enable_inputs(!term_enable);
	update_term_state(SPS_TERM, term_enable);
}

static void s3_term(int term_enable)
{
	/* If the board doesn't use s3_term, return before doing anything */
	if (!board_needs_s3_term())
		return;
	CPRINTS("%sable S3 signal terminations", term_enable ? "En" : "Dis");

	ap_tx_term_enable(term_enable);

	if (!board_tpm_uses_i2c())
		sps_term_enable(term_enable);
}

/*
 * Disable all terminations after cr50 reset. CCD state will re-enable them if
 * needed. We just want to make sure any terminations enabled from the previous
 * boot don't interfere with any other peripheral initialization. The pins
 * s3_term controls may not be covered by the standard gpio init, so they won't
 * be reset unless s3_term resets them during init.
 */
static void s3_term_init(void)
{
	s3_term(0);
}
DECLARE_HOOK(HOOK_INIT, s3_term_init, HOOK_PRIO_FIRST);

void board_s3_term(int term_enable)
{
	/* Only update the terminations if something has changed */
	if (!term_enable == !term_enabled)
		return;
	s3_term(term_enable);
}

static int command_s3term(int argc, char **argv)
{
	ccprintf("Terminations:%s%s%s\n",
		term_enabled ? "" : "None",
		term_enabled & AP_TX_TERM ? " AP" : "",
		term_enabled & SPS_TERM ? " SPS" : "");
	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(s3term, command_s3term, "",
			"Get the state of the S3 termination signals");
