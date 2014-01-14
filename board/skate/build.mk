# Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# Board specific files build

# the IC is STmicro STM32F100RB
CHIP:=stm32
CHIP_VARIANT:=stm32f100

board-y=board.o usb_charging.o
