# -*- makefile -*-
# Copyright 2018 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# SCP specific files build
#

CORE:=cortex-m
CFLAGS_CPU+=-march=armv7e-m -mcpu=cortex-m4

# Required chip modules
chip-y=clock.o gpio.o memmap.o system.o uart.o

ifeq ($(CONFIG_IPI),y)
$(out)/RO/chip/$(CHIP)/ipi_table.o: $(out)/ipi_table_gen.inc
$(out)/RW/chip/$(CHIP)/ipi_table.o: $(out)/ipi_table_gen.inc
endif

# Optional chip modules
chip-$(CONFIG_COMMON_TIMER)+=hrtimer.o
chip-$(CONFIG_I2C)+=i2c.o
chip-$(CONFIG_IPI)+=ipi.o ipi_table.o
chip-$(CONFIG_SPI)+=spi.o
chip-$(CONFIG_WATCHDOG)+=watchdog.o
