/* Copyright 2015 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* USB Power delivery port controller */

#ifndef __CROS_EC_USB_PD_TCPC_H
#define __CROS_EC_USB_PD_TCPC_H

#include <stdint.h>
#include "usb_pd_tcpm.h"

/* If we are a TCPC but do not a TCPM, then we implement the slave TCPCI */
#if defined(CONFIG_USB_PD_TCPC) && !defined(CONFIG_USB_PD_TCPM_STUB)
#define TCPCI_I2C_SLAVE
#endif

#ifdef TCPCI_I2C_SLAVE
/* Convert TCPC address to type-C port number */
#define TCPC_ADDR_TO_PORT(addr) (((addr) - CONFIG_TCPC_I2C_BASE_ADDR) >> 1)
/* Check if the i2c address belongs to TCPC */
#define ADDR_IS_TCPC(addr)      (((addr) & 0xfc) == CONFIG_TCPC_I2C_BASE_ADDR)
#endif

/**
 * Process incoming TCPCI I2C command
 *
 * @param read This is a read request. If 0, this is a write request.
 * @param len Length of incoming payload
 * @param payload Pointer to incoming and outgoing data
 * @param send_response Function to call to send response if necessary
 */
void tcpc_i2c_process(int read, int port, int len, uint8_t *payload,
		      void (*send_response)(int));

/**
 * Handle VBUS wake interrupts
 *
 * @param signal The VBUS wake interrupt signal
 */
void pd_vbus_evt_p0(enum gpio_signal signal);
void pd_vbus_evt_p1(enum gpio_signal signal);

/* Methods for TCPCI slaves (e.g. zinger) to get/set their internal state */
int tcpc_alert_status(int port, int *alert);
int tcpc_alert_status_clear(int port, uint16_t mask);
int tcpc_alert_mask_set(int port, uint16_t mask);
int tcpc_get_cc(int port, int *cc1, int *cc2);
int tcpc_select_rp_value(int port, int rp);
int tcpc_set_cc(int port, int pull);
int tcpc_set_polarity(int port, int polarity);
int tcpc_set_power_status_mask(int port, uint8_t mask);
int tcpc_set_vconn(int port, int enable);
int tcpc_set_msg_header(int port, int power_role, int data_role);
int tcpc_set_rx_enable(int port, int enable);
int tcpc_get_message(int port, uint32_t *payload, int *head);
int tcpc_transmit(int port, enum tcpm_transmit_type type, uint16_t header,
		  const uint32_t *data);
int rx_buf_is_empty(int port);
void rx_buf_clear(int port);

/**
 * Invalidate last message received at the port when the port gets disconnected
 * or reset(soft/hard). This is used to identify and handle the duplicate
 * messages.
 *
 * @param port USB PD TCPC port number
 */
void invalidate_last_message_id(int port);

/**
 * Identify and drop any duplicate messages received at the port.
 *
 * @param port USB PD TCPC port number
 * @param msg_header Message Header containing the RX message ID
 * @return 1 if the received message is a duplicate one, 0 otherwise.
 */
int consume_repeat_message(int port, uint16_t msg_header);

#endif /* __CROS_EC_USB_PD_TCPC_H */
