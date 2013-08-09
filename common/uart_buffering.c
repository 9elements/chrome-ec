/* Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* Common code to do UART buffering and printing */

#include <stdarg.h>

#include "common.h"
#include "console.h"
#include "host_command.h"
#include "printf.h"
#include "system.h"
#include "task.h"
#include "uart.h"
#include "util.h"

/* Macros to advance in the circular buffers */
#define TX_BUF_NEXT(i) (((i) + 1) & (CONFIG_UART_TX_BUF_SIZE - 1))
#define RX_BUF_NEXT(i) (((i) + 1) & (CONFIG_UART_RX_BUF_SIZE - 1))
#define RX_BUF_PREV(i) (((i) - 1) & (CONFIG_UART_RX_BUF_SIZE - 1))

/* Macros to calculate difference of pointers in the circular buffers. */
#define TX_BUF_DIFF(i, j) (((i) - (j)) & (CONFIG_UART_TX_BUF_SIZE - 1))
#define RX_BUF_DIFF(i, j) (((i) - (j)) & (CONFIG_UART_RX_BUF_SIZE - 1))

/* ASCII control character; for example, CTRL('C') = ^C */
#define CTRL(c) ((c) - '@')

/* Transmit and receive buffers */
static volatile char tx_buf[CONFIG_UART_TX_BUF_SIZE];
static volatile int tx_buf_head;
static volatile int tx_buf_tail;
static volatile char rx_buf[CONFIG_UART_RX_BUF_SIZE];
static volatile int rx_buf_head;
static volatile int rx_buf_tail;
static int tx_snapshot_head;
static int tx_snapshot_tail;
static int uart_suspended;

/**
 * Put a single character into the transmit buffer.
 *
 * Does not enable the transmit interrupt; assumes that happens elsewhere.
 *
 * @param context	Context; ignored.
 * @param c		Character to write.
 * @return 0 if the character was transmitted, 1 if it was dropped.
 */
static int __tx_char(void *context, int c)
{
	int tx_buf_next;

	/* Do newline to CRLF translation */
	if (c == '\n' && __tx_char(NULL, '\r'))
		return 1;

	tx_buf_next = TX_BUF_NEXT(tx_buf_head);
	if (tx_buf_next == tx_buf_tail)
		return 1;

	tx_buf[tx_buf_head] = c;
	tx_buf_head = tx_buf_next;
	return 0;
}

/**
 * Copy output from buffer until TX fifo full or output buffer empty.
 *
 * May be called from interrupt context.
 */
static void fill_tx_fifo(void)
{
	while (uart_tx_ready() && (tx_buf_head != tx_buf_tail)) {
		uart_write_char(tx_buf[tx_buf_tail]);
		tx_buf_tail = TX_BUF_NEXT(tx_buf_tail);
	}
}

/**
 * Helper for UART processing.
 */
void uart_process(void)
{
	int got_input = 0;

	/* Copy input from buffer until RX fifo empty */
	while (uart_rx_available()) {
		int c = uart_read_char();
		int rx_buf_next = RX_BUF_NEXT(rx_buf_head);

		if (c == CTRL('Q')) {
			/* Software flow control - XOFF */
			uart_suspended = 1;
			uart_tx_stop();
		} else if (c == CTRL('S')) {
			/* Software flow control - XON */
			uart_suspended = 0;
			if (uart_tx_stopped())
				uart_tx_start();
		} else if (rx_buf_next != rx_buf_tail) {
			/* Buffer all other input */
			rx_buf[rx_buf_head] = c;
			rx_buf_head = rx_buf_next;
		}

		got_input = 1;
	}

	if (got_input)
		console_has_input();

	if (uart_suspended)
		return;

	/* Copy output from buffer until TX fifo full or output buffer empty */
	fill_tx_fifo();

	/* If output buffer is empty, disable transmit interrupt */
	if (tx_buf_tail == tx_buf_head)
		uart_tx_stop();
}

int uart_putc(int c)
{
	int rv = __tx_char(NULL, c);

	if (!uart_suspended && uart_tx_stopped())
		uart_tx_start();

	return rv ? EC_ERROR_OVERFLOW : EC_SUCCESS;
}

int uart_puts(const char *outstr)
{
	/* Put all characters in the output buffer */
	while (*outstr) {
		if (__tx_char(NULL, *outstr++) != 0)
			break;
	}

	if (!uart_suspended && uart_tx_stopped())
		uart_tx_start();

	/* Successful if we consumed all output */
	return *outstr ? EC_ERROR_OVERFLOW : EC_SUCCESS;
}

int uart_vprintf(const char *format, va_list args)
{
	int rv = vfnprintf(__tx_char, NULL, format, args);

	if (!uart_suspended && uart_tx_stopped())
		uart_tx_start();

	return rv;
}

int uart_printf(const char *format, ...)
{
	int rv;
	va_list args;

	va_start(args, format);
	rv = uart_vprintf(format, args);
	va_end(args);
	return rv;
}

void uart_flush_output(void)
{
	/* If UART is suspended, ignore flush request. */
	if (uart_suspended)
		return;

	/*
	 * If we're in interrupt context, copy output explicitly, since the
	 * UART interrupt may not be able to preempt this one.
	 */
	if (in_interrupt_context()) {
		do {
			/* Copy until TX fifo full or output buffer empty */
			fill_tx_fifo();

			/* Wait for transmit FIFO empty */
			uart_tx_flush();
		} while (tx_buf_head != tx_buf_tail);
		return;
	}

	/* Wait for buffer to empty */
	while (tx_buf_head != tx_buf_tail) {
		/*
		 * It's possible we're in some other interrupt, and the
		 * previous context was doing a printf() or puts() but hadn't
		 * enabled the UART interrupt.  Check if the interrupt is
		 * disabled, and if so, re-enable and trigger it.  Note that
		 * this check is inside the while loop, so we'll be safe even
		 * if the context switches away from us to another partial
		 * printf() and back.
		 */
		if (uart_tx_stopped())
			uart_tx_start();
	}

	/* Wait for transmit FIFO empty */
	uart_tx_flush();
}

void uart_flush_input(void)
{
	/* Disable interrupts */
	uart_disable_interrupt();

	/* Empty the hardware FIFO */
	uart_process();

	/* Clear the input buffer */
	rx_buf_tail = rx_buf_head;

	/* Re-enable interrupts */
	uart_enable_interrupt();
}

int uart_getc(void)
{
	int c;

	/* Disable interrupts */
	uart_disable_interrupt();

	/* Call interrupt handler to empty the hardware FIFO */
	uart_process();

	if (rx_buf_tail == rx_buf_head) {
		c = -1;  /* No pending input */
	} else {
		c = rx_buf[rx_buf_tail];
		rx_buf_tail = RX_BUF_NEXT(rx_buf_tail);
	}

	/* Re-enable interrupts */
	uart_enable_interrupt();

	return c;
}

int uart_gets(char *dest, int size)
{
	int got = 0;
	int c;

	/* Read characters */
	while (got < size - 1) {
		c = uart_getc();

		/* Stop on input buffer empty */
		if (c == -1)
			break;

		dest[got++] = c;

		/* Stop after newline */
		if (c == '\n')
			break;
	}

	/* Null-terminate */
	dest[got] = '\0';

	/* Return the length we got */
	return got;
}

/*****************************************************************************/
/* Host commands */

static int host_command_console_snapshot(struct host_cmd_handler_args *args)
{
	/*
	 * Only allowed on unlocked system, since console output contains
	 * keystroke data.
	 */
	if (system_is_locked())
		return EC_ERROR_ACCESS_DENIED;

	/* Assume the whole circular buffer is full */
	tx_snapshot_head = tx_buf_head;
	tx_snapshot_tail = TX_BUF_NEXT(tx_snapshot_head);

	/*
	 * Immediately skip any unused bytes.  This doesn't always work,
	 * because a higher-priority task or interrupt handler can write to the
	 * buffer while we're scanning it.  This is acceptable because this
	 * command is only for debugging, and the failure mode is a bit of
	 * garbage at the beginning of the saved output.  The saved buffer
	 * could also be overwritten by the head coming completely back around
	 * before we finish.  The alternative would be to make a full copy of
	 * the transmit buffer, but that requires a lot of RAM.
	 */
	while (tx_snapshot_tail != tx_snapshot_head) {
		if (tx_buf[tx_snapshot_tail])
			break;
		tx_snapshot_tail = TX_BUF_NEXT(tx_snapshot_tail);
	}

	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_CONSOLE_SNAPSHOT,
		     host_command_console_snapshot,
		     EC_VER_MASK(0));

static int host_command_console_read(struct host_cmd_handler_args *args)
{
	char *dest = (char *)args->response;

	/*
	 * Only allowed on unlocked system, since console output contains
	 * keystroke data.
	 */
	if (system_is_locked())
		return EC_ERROR_ACCESS_DENIED;

	/* If no snapshot data, return empty response */
	if (tx_snapshot_head == tx_snapshot_tail)
		return EC_RES_SUCCESS;

	/* Copy data to response */
	while (tx_snapshot_tail != tx_snapshot_head &&
	       args->response_size < args->response_max - 1) {

		/*
		 * Copy only non-zero bytes, so that we don't copy unused
		 * bytes if the buffer hasn't completely rolled at boot.
		 */
		if (tx_buf[tx_snapshot_tail]) {
			*(dest++) = tx_buf[tx_snapshot_tail];
			args->response_size++;
		}

		tx_snapshot_tail = TX_BUF_NEXT(tx_snapshot_tail);
	}

	/* Null-terminate */
	*(dest++) = '\0';
	args->response_size++;

	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_CONSOLE_READ,
		     host_command_console_read,
		     EC_VER_MASK(0));
