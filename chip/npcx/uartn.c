/*
 * Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* UART module for Chrome EC */

#include <clock.h>
#include "common.h"
#include <gpio.h>
#include <gpio_chip.h>
#include "registers.h"
#include "system.h"
#include "task.h"
#include "util.h"

#ifdef NPCX_UART_FIFO_SUPPORT
/* Enable UART Tx FIFO empty interrupt */
#define NPCX_UART_TX_EMPTY_INT_EN(n)      \
		(SET_BIT(NPCX_UFTCTL(n), NPCX_UFTCTL_TEMPTY_EN))
/* True if UART Tx FIFO empty interrupt is enabled */
#define NPCX_UART_TX_EMPTY_INT_IS_EN(n)   \
		(IS_BIT_SET(NPCX_UFTCTL(n), NPCX_UFTCTL_TEMPTY_EN))
/* Disable UART Tx FIFO empty interrupt */
#define NPCX_UART_TX_EMPTY_INT_DIS(n)     \
		(CLEAR_BIT(NPCX_UFTCTL(n), NPCX_UFTCTL_TEMPTY_EN))
/* True if the Tx FIFO is not completely full */
#define NPCX_UART_TX_IS_READY(n)          \
		(!(GET_FIELD(NPCX_UFTSTS(n), NPCX_UFTSTS_TEMPTY_LVL) == 0))
/*
 * True if Tx is in progress
 * (i.e. FIFO is not empty or last byte in TSFT (Transmit Shift register)
 * is not sent)
 */
#define NPCX_UART_TX_IN_XMIT(n)           \
		(!IS_BIT_SET(NPCX_UFTSTS(n), NPCX_UFTSTS_NXMIP))

/*
 * Enable to generate interrupt when there is at least one byte
 * in the receive FIFO
 */
#define NPCX_UART_RX_INT_EN(n)            \
		(SET_BIT(NPCX_UFRCTL(n), NPCX_UFRCTL_RNEMPTY_EN))
/* True if at least one byte is in the receive FIFO */
#define NPCX_UART_RX_IS_AVAILABLE(n)      \
		(IS_BIT_SET(NPCX_UFRSTS(n), NPCX_UFRSTS_RFIFO_NEMPTY_STS))
#else
/* Enable UART Tx buffer empty interrupt */
#define NPCX_UART_TX_EMPTY_INT_EN(n)      (NPCX_UICTRL(n) |= 0x20)
/* True if UART Tx buffer empty interrupt is enabled */
#define NPCX_UART_TX_EMPTY_INT_IS_EN(n)   (NPCX_UICTRL(n) & 0x20)
/* Disable UART Tx buffer empty interrupt */
#define NPCX_UART_TX_EMPTY_INT_DIS(n)     (NPCX_UICTRL(n) &= ~0x20)
/* True if 1-byte Tx buffer is empty */
#define NPCX_UART_TX_IS_READY(n)          (NPCX_UICTRL(n) & 0x01)
/*
 * True if Tx is in progress
 * (i.e. Tx buffer is not empty or last byte in TSFT (Transmit Shift register)
 * is not sent)
 */
#define NPCX_UART_TX_IN_XMIT(n)           (NPCX_USTAT(n) & 0x40)
 /* Enable to generate interrupt when there is data in the receive buffer */
#define NPCX_UART_RX_INT_EN(n)            (NPCX_UICTRL(n) = 0x40)
/* True if there is data in the 1-byte Receive buffer */
#define NPCX_UART_RX_IS_AVAILABLE(n)      (NPCX_UICTRL(n) & 0x02)
#endif

struct uart_configs {
	uint32_t irq;
	uint32_t clk_en_offset;
	uint32_t clk_en_msk;
};
static const struct uart_configs uart_cfg[] = {
	{NPCX_IRQ_UART, CGC_OFFSET_UART, CGC_UART_MASK},
#ifdef NPCX_SECOND_UART
	{NPCX_IRQ_UART2, CGC_OFFSET_UART2, CGC_UART2_MASK},
#endif
};
BUILD_ASSERT(ARRAY_SIZE(uart_cfg) == UART_MODULE_COUNT);

#ifdef CONFIG_LOW_POWER_IDLE
static const struct npcx_wui uart_wui[] = {
	WUI(1, NPCX_UART_WK_GROUP, NPCX_UART_WK_BIT),
#ifdef NPCX_SECOND_UART
	WUI(0, NPCX_UART2_WK_GROUP, NPCX_UART2_WK_BIT),
#endif
};
BUILD_ASSERT(ARRAY_SIZE(uart_wui) == UART_MODULE_COUNT);

void uartn_wui_en(uint8_t uart_num)
{
	struct npcx_wui wui;

	wui = uart_wui[uart_num];
	/* Clear pending bit before enable uart wake-up */
	SET_BIT(NPCX_WKPCL(wui.table, wui.group), wui.bit);
	/* Enable UART1 wake-up and interrupt request */
	SET_BIT(NPCX_WKEN(wui.table, wui.group), wui.bit);
}
#endif

void uartn_rx_int_en(uint8_t uart_num)
{
	NPCX_UART_RX_INT_EN(uart_num);
}

void uartn_tx_start(uint8_t uart_num)
{
	/* If interrupt is already enabled, nothing to do */
	if (NPCX_UART_TX_EMPTY_INT_IS_EN(uart_num))
		return;

	/* Do not allow deep sleep while transmit in progress */
	disable_sleep(SLEEP_MASK_UART);

	/*
	 * Re-enable the transmit interrupt, then forcibly trigger the
	 * interrupt.  This works around a hardware problem with the
	 * UART where the FIFO only triggers the interrupt when its
	 * threshold is _crossed_, not just met.
	 */
	NPCX_UART_TX_EMPTY_INT_EN(uart_num);

	task_trigger_irq(uart_cfg[uart_num].irq);
}

void uartn_tx_stop(uint8_t uart_num, uint8_t sleep_ena)
{
	/* Disable TX interrupt */
	NPCX_UART_TX_EMPTY_INT_DIS(uart_num);
	/*
	 * Re-allow deep sleep when transmiting on the default pad (deep sleep
	 * is always disabled when alternate pad is selected).
	 */
	if (sleep_ena == 1)
		enable_sleep(SLEEP_MASK_UART);
}

void uartn_tx_flush(uint8_t uart_num)
{
	/* Wait for transmit FIFO empty and last byte is sent */
	while (NPCX_UART_TX_IN_XMIT(uart_num))
		;
}

int uartn_tx_ready(uint8_t uart_num)
{
	return NPCX_UART_TX_IS_READY(uart_num);
}

int uartn_tx_in_progress(uint8_t uart_num)
{
	return NPCX_UART_TX_IN_XMIT(uart_num);
}

int uartn_rx_available(uint8_t uart_num)
{
	return NPCX_UART_RX_IS_AVAILABLE(uart_num);
}

void uartn_write_char(uint8_t uart_num, char c)
{
	/* Wait for space in transmit FIFO. */
	while (!uartn_tx_ready(uart_num))
		;

	NPCX_UTBUF(uart_num) = c;
}

int uartn_read_char(uint8_t uart_num)
{
	return NPCX_URBUF(uart_num);
}

void uartn_clear_rx_fifo(int channel)
{
	int scratch __attribute__ ((unused));

	/* If '1',  that means there is RX data on the FIFO register */
	while (NPCX_UART_RX_IS_AVAILABLE(channel))
		scratch = NPCX_URBUF(channel);
}

#ifdef NPCX_UART_FIFO_SUPPORT
static void uartn_set_fifo_mode(uint8_t uart_num)
{
	/* Enable the UART FIFO mode */
	SET_BIT(NPCX_UMDSL(uart_num), NPCX_UMDSL_FIFO_MD);
	/* Disable all Tx interrupts */
	NPCX_UFTCTL(uart_num) &= ~(BIT(NPCX_UFTCTL_TEMPTY_LVL_EN) |
					BIT(NPCX_UFTCTL_TEMPTY_EN) |
					BIT(NPCX_UFTCTL_NXIMPEN));
}

#endif

static void uartn_config(uint8_t uart_num)
{
#ifdef CONFIG_LOW_POWER_IDLE
	struct npcx_wui wui;
#endif

	/* Configure pins from GPIOs to CR_UART */
	gpio_config_module(MODULE_UART, 1);

#ifdef CONFIG_LOW_POWER_IDLE
	/*
	 * Configure the UART wake-up event triggered from a falling edge
	 * on CR_SIN pin.
	 */
	wui = uart_wui[uart_num];
	SET_BIT(NPCX_WKEDG(wui.table, wui.group), wui.bit);
#endif
	/*
	 * If apb2's clock is not 15MHz, we need to find the other optimized
	 * values of UPSR and UBAUD for baud rate 115200.
	 */
#if (NPCX_APB_CLOCK(2) != 15000000)
#error "Unsupported apb2 clock for UART!"
#endif

	/*
	 * Fix baud rate to 115200. If this value is modified, please also
	 * modify the delay in uart_set_pad and uart_reset_default_pad_panic.
	 */
	NPCX_UPSR(uart_num) = 0x38;
	NPCX_UBAUD(uart_num) = 0x01;

	/*
	 * 8-N-1, FIFO enabled.  Must be done after setting
	 * the divisor for the new divisor to take effect.
	 */
	NPCX_UFRS(uart_num) = 0x00;
#ifdef NPCX_UART_FIFO_SUPPORT
	uartn_set_fifo_mode(uart_num);
#endif
	NPCX_UART_RX_INT_EN(uart_num);
}

void uartn_init(uint8_t uart_num)
{
	uint32_t offset, mask;

	offset = uart_cfg[uart_num].clk_en_offset;
	mask = uart_cfg[uart_num].clk_en_msk;
	clock_enable_peripheral(offset, mask, CGC_MODE_ALL);

	if (uart_num == NPCX_UART_PORT0)
		npcx_gpio2uart();

	/* Configure UARTs (identically) */
	uartn_config(uart_num);

	/*
	 * Enable interrupts for UART0 only. Host UART will have to wait
	 * until the LPC bus is initialized.
	 */
	uartn_clear_rx_fifo(uart_num);
	task_enable_irq(uart_cfg[uart_num].irq);
}
