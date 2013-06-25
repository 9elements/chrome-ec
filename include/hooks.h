/* Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* System hooks for Chrome EC */

#ifndef __CROS_EC_HOOKS_H
#define __CROS_EC_HOOKS_H

#include "common.h"

enum hook_priority {
	/* Generic values across all hooks */
	HOOK_PRIO_FIRST = 1,       /* Highest priority */
	HOOK_PRIO_DEFAULT = 5000,  /* Default priority */
	HOOK_PRIO_LAST = 9999,     /* Lowest priority */

	/* Specific hook vales for HOOK_INIT */
	/* LPC inits before modules which need memory-mapped I/O */
	HOOK_PRIO_INIT_LPC = HOOK_PRIO_FIRST + 1,
	/* Chipset inits before modules which need to know its initial state. */
	HOOK_PRIO_INIT_CHIPSET = HOOK_PRIO_FIRST + 2,
};

enum hook_type {
	HOOK_INIT = 0,         /* System init */
	HOOK_FREQ_CHANGE,      /* System clock changed frequency */
	HOOK_SYSJUMP,          /* About to jump to another image.  Modules
				* which need to preserve data across such a
				* jump should save it here and restore it in
				* HOOK_INIT.
				*
				* NOTE: This hook is called with interrupts
				* disabled! */
	HOOK_CHIPSET_PRE_INIT, /* Initialization for components such as PMU to
				* be done before host chipset/AP starts up. */
	HOOK_CHIPSET_STARTUP,  /* System is starting up.  All suspend rails are
				* now on. */
	HOOK_CHIPSET_RESUME,   /* System is resuming from suspend, or booting
				* and has reached the point where all voltage
				* rails are on */
	HOOK_CHIPSET_SUSPEND,  /* System is suspending, or shutting down; all
				* voltage rails are still on */
	HOOK_CHIPSET_SHUTDOWN, /* System is shutting down.  All suspend rails
				* are still on. */
	HOOK_AC_CHANGE,        /* AC power plugged in or removed */
	HOOK_LID_CHANGE,       /* Lid opened or closed.  Based on debounced lid
				* state, not raw lid GPIO input. */
	HOOK_TICK,             /* Periodic tick, every HOOK_TICK_INTERVAL */
	HOOK_SECOND,           /* Periodic tick, every second */
};

struct hook_data {
	/* Hook processing routine. */
	void (*routine)(void);
	/* Priority; low numbers = higher priority. */
	int priority;
};

/**
 * Initialize the hooks library.
 */
void hook_init(void);

/**
 * Call all the hook routines of a specified type.
 *
 * @param type		Type of hook routines to call.
 */
void hook_notify(enum hook_type type);

/**
 * Start a timer to call a deferred routine.
 *
 * The routine will be called after at least the specified delay, in the
 * context of the hook task.
 *
 * @param routine	Routine to call; must have been declared with
 *			DECLARE_DEFERRED().
 * @param us		Delay in microseconds until routine will be called.
 *			If the routine is already pending, subsequent calls
 *			will change the delay.  Pass us=0 to call as soon as
 *			possible, or -1 to cancel the deferred call.
 *
 * @return non-zero if error.
 */
int hook_call_deferred(void (*routine)(void), int us);

/**
 * Register a hook routine.
 *
 * @param hooktype	Type of hook for routine (enum hook_type)
 * @param routine	Hook routine, with prototype void routine(void)
 * @param priority      Priority for determining when routine is called vs.
 *			other hook routines; should be between HOOK_PRIO_FIRST
 *                      and HOOK_PRIO_LAST, and should be HOOK_PRIO_DEFAULT
 *			unless there's a compelling reason to care about the
 *			order in which hooks are called.
 */
#define DECLARE_HOOK(hooktype, routine, priority)			\
	const struct hook_data __hook_##hooktype##_##routine		\
	__attribute__((section(".rodata." #hooktype)))			\
	     = {routine, priority}


struct deferred_data {
	/* Deferred function pointer */
	void (*routine)(void);
};

/**
 * Register a deferred function call.
 *
 * Note that if you declare a bunch of these, you may need to override
 * DEFERRABLE_MAX_COUNT in your board.h.
 *
 * @param routine	Function pointer, with prototype void routine(void)
 */
#define DECLARE_DEFERRED(routine)					\
	const struct deferred_data __deferred_##routine			\
	__attribute__((section(".rodata.deferred")))			\
	     = {routine}

#endif  /* __CROS_EC_HOOKS_H */
