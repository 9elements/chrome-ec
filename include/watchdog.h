/* Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* Watchdog driver */

#ifndef __CROS_EC_WATCHDOG_H
#define __CROS_EC_WATCHDOG_H

/* Watchdog period in ms; must be at least twice HOOK_TICK_INTERVAL */
#define WATCHDOG_PERIOD_MS 1100

/*
 * Fire auxiliary timer 50ms before watchdog timer expires. This leaves
 * some time for debug trace to be printed.
 */
#define AUX_TIMER_PERIOD_MS (WATCHDOG_PERIOD_MS - 50)

/**
 * Initialize the watchdog.
 *
 * This will cause the CPU to reboot if it has been more than 2 watchdog
 * periods since watchdog_reload() has been called.
 */
int watchdog_init(void);

/**
 * Display a trace with information about an expired watchdog timer
 *
 * This shows the location in the code where the expiration happened.
 * Usually this helps locate a loop which is blocking execution of the
 * watchdog task.
 *
 * @param excep_lr	Value of lr to indicate caller return
 * @param excep_sp	Value of sp to indicate caller task id
 */
void watchdog_trace(uint32_t excep_lr, uint32_t excep_sp);

/**
 * Watchdog has not been tickled recently warning. This function should be
 * called when the watchdog is close to firing.
 */
void watchdog_warning_irq(void);

/* Reload the watchdog counter */
#ifdef CONFIG_WATCHDOG
void watchdog_reload(void);
#else
static inline void watchdog_reload(void) { }
#endif

#endif /* __CROS_EC_WATCHDOG_H */
