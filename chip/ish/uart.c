/* Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* UART module for ISH */
#include "common.h"
#include "console.h"
#include "uart_defs.h"
#include "atomic.h"
#include "task.h"
#include "registers.h"
#include "uart.h"
#include "uart_defs.h"
#include "interrupts.h"
#include "system.h"

#define CPUTS(outstr) cputs(CC_LPC, outstr)
#define CPRINTS(format, args...) cprints(CC_LPC, format, ## args)
#define CPRINTF(format, args...) cprintf(CC_LPC, format, ## args)

static const uint32_t baud_conf[][BAUD_TABLE_MAX] = {
	{B9600, 9600},
	{B57600, 57600},
	{B115200, 115200},
	{B921600, 921600},
	{B2000000, 2000000},
	{B3000000, 3000000},
	{B3250000, 3250000},
	{B3500000, 3500000},
	{B4000000, 4000000},
	{B19200, 19200},
};

static struct uart_ctx uart_ctx[UART_DEVICES] = {
	{
		.id = 0,
		.base = UART0_BASE,
		.input_freq = UART_ISH_INPUT_FREQ,
		.addr_interval = UART_ISH_ADDR_INTERVAL,
		.uart_state = UART_STATE_CG,
	},
	{
		.id = 1,
		.base = UART1_BASE,
		.input_freq = UART_ISH_INPUT_FREQ,
		.addr_interval = UART_ISH_ADDR_INTERVAL,
		.uart_state = UART_STATE_CG,
	},
	{
		.id = 2,
		.base = UART2_BASE,
		.input_freq = UART_ISH_INPUT_FREQ,
		.addr_interval = UART_ISH_ADDR_INTERVAL,
		.uart_state = UART_STATE_CG,
	}
};

static int init_done;

int uart_init_done(void)
{
	return init_done;
}

void uart_tx_start(void)
{
	if (!IS_ENABLED(CONFIG_POLLING_UART)) {
		if (IER(ISH_DEBUG_UART) & IER_TDRQ)
			return;

		/* Do not allow deep sleep while transmit in progress */
		disable_sleep(SLEEP_MASK_UART);

		IER(ISH_DEBUG_UART) |= IER_TDRQ;
	}
}

void uart_tx_stop(void)
{
	if (!IS_ENABLED(CONFIG_POLLING_UART)) {
		/* Re-allow deep sleep */
		enable_sleep(SLEEP_MASK_UART);

		IER(ISH_DEBUG_UART) &= ~IER_TDRQ;
	}
}

void uart_tx_flush(void)
{
	if (!IS_ENABLED(CONFIG_POLLING_UART)) {
		while (!(LSR(ISH_DEBUG_UART) & LSR_TEMT))
			continue;
	}
}

int uart_tx_ready(void)
{
	return LSR(ISH_DEBUG_UART) & LSR_TDRQ;
}

int uart_rx_available(void)
{
	if (IS_ENABLED(CONFIG_POLLING_UART))
		return 0;

	return LSR(ISH_DEBUG_UART) & LSR_DR;
}

void uart_write_char(char c)
{
	/* Wait till receiver is ready */
	while (!uart_tx_ready())
		continue;

	THR(ISH_DEBUG_UART) = c;
}

int uart_read_char(void)
{
	return RBR(ISH_DEBUG_UART);
}

void uart_ec_interrupt(void)
{
	/* Read input FIFO until empty, then fill output FIFO */
	uart_process_input();
	uart_process_output();
}
#ifndef CONFIG_POLLING_UART
DECLARE_IRQ(ISH_DEBUG_UART_IRQ, uart_ec_interrupt);
#endif

static int uart_return_baud_rate_by_id(int baud_rate_id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(baud_conf); i++) {
		if (baud_conf[i][BAUD_IDX] == baud_rate_id)
			return baud_conf[i][BAUD_SPEED];
	}

	return -1;
}

static void uart_hw_init(enum UART_PORT id)
{
	uint32_t divisor;	/* baud rate divisor */
	uint8_t mcr = 0;
	uint8_t fcr = 0;
	struct uart_ctx *ctx = &uart_ctx[id];

	/* Calculate baud rate divisor */
	divisor = (ctx->input_freq / ctx->baud_rate) >> 4;

	MUL(ctx->id) = (divisor * ctx->baud_rate);
	DIV(ctx->id) = (ctx->input_freq / 16);
	PS(ctx->id) = 16;

	/* Set the DLAB to access the baud rate divisor registers */
	LCR(ctx->id) = LCR_DLAB;
	DLL(ctx->id) = (divisor & 0xff);
	DLH(ctx->id) = ((divisor >> 8) & 0xff);

	/* 8 data bits, 1 stop bit, no parity, clear DLAB */
	LCR(ctx->id) = LCR_8BIT_CHR;

	if (ctx->client_flags & UART_CONFIG_HW_FLOW_CONTROL)
		mcr = MCR_AUTO_FLOW_EN;

	/* needs to be set regardless of flow control */
	mcr |= MCR_INTR_ENABLE;

	mcr |= (MCR_RTS | MCR_DTR);
	MCR(ctx->id) = mcr;

	fcr = FCR_FIFO_SIZE_64 | FCR_ITL_FIFO_64_BYTES_1;

	/* configure FIFOs */
	FCR(ctx->id) = (fcr | FCR_FIFO_ENABLE
			| FCR_RESET_RX | FCR_RESET_TX);

	/* enable UART unit */
	ABR(ctx->id) = ABR_UUE;

	/* clear the port */
	RBR(ctx->id);

	if (IS_ENABLED(CONFIG_POLLING_UART))
		IER(ctx->id) = 0x00;
	else
		IER(ctx->id) = IER_RECV;
}

static void uart_stop_hw(enum UART_PORT id)
{
	int i;
	uint32_t fifo_len;

	/* Manually clearing the fifo from possible noise.
	 * Entering D0i3 when fifo is not cleared may result in a hang.
	 */
	fifo_len = (FOR(id) & FOR_OCCUPANCY_MASK) >> FOR_OCCUPANCY_OFFS;

	for (i = 0; i < fifo_len; i++)
		(void)RBR(id);

	/* No interrupts are enabled */
	IER(id) = 0;
	MCR(id) = 0;

	/* Clear and disable FIFOs */
	FCR(id) = (FCR_RESET_RX | FCR_RESET_TX);

	/* Disable uart unit */
	ABR(id) = 0;
}

static int uart_client_init(enum UART_PORT id, uint32_t baud_rate_id, int flags)
{
	if ((uart_ctx[id].base == 0) || (id >= UART_DEVICES))
		return UART_ERROR;

	if (!bool_compare_and_swap_u32(&uart_ctx[id].is_open, 0, 1))
		return UART_BUSY;

	uart_ctx[id].baud_rate = uart_return_baud_rate_by_id(baud_rate_id);

	if ((uart_ctx[id].baud_rate == -1) || (uart_ctx[id].baud_rate == 0))
		uart_ctx[id].baud_rate = UART_DEFAULT_BAUD_RATE;

	uart_ctx[id].client_flags = flags;

	atomic_and(&uart_ctx[id].uart_state, ~UART_STATE_CG);
	uart_hw_init(id);

	return EC_SUCCESS;
}

static void uart_drv_init(void)
{
	int i;

	/* Disable UART */
	for (i = 0; i < UART_DEVICES; i++)
		uart_stop_hw(i);

	/* Enable HSU global interrupts (DMA/U0/U1) and set PMEN bit
	 * to allow PMU to clock gate ISH
	 */
	HSU_REG_GIEN = (GIEN_DMA_EN | GIEN_UART0_EN
			| GIEN_UART1_EN | GIEN_PWR_MGMT);

	task_enable_irq(ISH_DEBUG_UART_IRQ);
}

void uart_init(void)
{
	uart_drv_init();
	uart_client_init(ISH_DEBUG_UART, B115200, 0);
	init_done = 1;
}
