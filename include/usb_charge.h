/* Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* USB charging control module for Chrome EC */

#ifndef __CROS_EC_USB_CHARGE_H
#define __CROS_EC_USB_CHARGE_H

#include "board.h"

enum usb_charge_mode {
	/* Disable USB port. */
	USB_CHARGE_MODE_DISABLED,
	/* Set USB port to Standard Downstream Port, USB 2.0 mode. */
	USB_CHARGE_MODE_SDP2,
	/* Set USB port to Charging Downstream Port, BC 1.2. */
	USB_CHARGE_MODE_CDP,
	/* Set USB port to Dedicated Charging Port, BC 1.2. */
	USB_CHARGE_MODE_DCP_SHORT,

	USB_CHARGE_MODE_COUNT
};

int usb_charge_set_mode(int usb_port_id, enum usb_charge_mode);

#endif  /* __CROS_EC_USB_CHARGE_H */
