/* Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* System hooks for Chrome EC */

#include "hooks.h"
#include "link_defs.h"
#include "timer.h"
#include "util.h"

#ifdef HOOK_DEBUG
#define CPUTS(outstr) cputs(CC_HOOK, outstr)
#define CPRINTF(format, args...) cprintf(CC_HOOK, format, ## args)
#else
#define CPUTS(outstr)
#define CPRINTF(format, args...)
#endif

/* Maximum number of deferrable functions */
#ifndef DEFERRABLE_MAX_COUNT
#define DEFERRABLE_MAX_COUNT 8
#endif

struct hook_ptrs {
	const struct hook_data *start;
	const struct hook_data *end;
};

/*
 * Hook data start and end pointers for each type of hook.  Must be in same
 * order as enum hook_type.
 */
static const struct hook_ptrs hook_list[] = {
	{__hooks_init, __hooks_init_end},
	{__hooks_freq_change, __hooks_freq_change_end},
	{__hooks_sysjump, __hooks_sysjump_end},
	{__hooks_chipset_pre_init, __hooks_chipset_pre_init_end},
	{__hooks_chipset_startup, __hooks_chipset_startup_end},
	{__hooks_chipset_resume, __hooks_chipset_resume_end},
	{__hooks_chipset_suspend, __hooks_chipset_suspend_end},
	{__hooks_chipset_shutdown, __hooks_chipset_shutdown_end},
	{__hooks_ac_change, __hooks_ac_change_end},
	{__hooks_lid_change, __hooks_lid_change_end},
	{__hooks_tick, __hooks_tick_end},
	{__hooks_second, __hooks_second_end},
};

/* Times for deferrable functions */
static uint64_t defer_until[DEFERRABLE_MAX_COUNT];
static int defer_count;

void hook_notify(enum hook_type type)
{
	const struct hook_data *start, *end, *p;
	int count, called = 0;
	int last_prio = HOOK_PRIO_FIRST - 1, prio;

	CPRINTF("[%T hook notify %d]\n", type);

	start = hook_list[type].start;
	end = hook_list[type].end;
	count = ((uint32_t)end - (uint32_t)start) / sizeof(struct hook_data);

	/* Call all the hooks in priority order */
	while (called < count) {
		/* Find the lowest remaining priority */
		for (p = start, prio = HOOK_PRIO_LAST + 1; p < end; p++) {
			if (p->priority < prio && p->priority > last_prio)
				prio = p->priority;
		}
		last_prio = prio;

		/* Call all the hooks with that priority */
		for (p = start; p < end; p++) {
			if (p->priority == prio) {
				called++;
				p->routine();
			}
		}
	}
}

void hook_init(void)
{
	defer_count = __deferred_funcs_end - __deferred_funcs;
	ASSERT(defer_count <= DEFERRABLE_MAX_COUNT);

	hook_notify(HOOK_INIT);
}

int hook_call_deferred(void (*routine)(void), int us)
{
	const struct deferred_data *p;
	int i;

	/* Find the index of the routine */
	for (p = __deferred_funcs; p < __deferred_funcs_end; p++) {
		if (p->routine == routine)
			break;
	}
	if (p >= __deferred_funcs_end)
		return EC_ERROR_INVAL;  /* Routine not registered */

	/* Convert to index */
	i = p - __deferred_funcs;
	if (i >= DEFERRABLE_MAX_COUNT)
		return EC_ERROR_UNKNOWN;  /* No space to hold time */

	if (us == -1) {
		/* Cancel */
		defer_until[i] = 0;
	} else {
		/*
		 * Set alarm, and wake task so it can re-sleep for the
		 * proper time.
		 */
		defer_until[i] = get_time().val + us;
		task_wake(TASK_ID_TICK);
	}

	return EC_SUCCESS;
}

void hook_task(void)
{
	/* Per-second hook will be called first time through the loop */
	static uint64_t last_second = -SECOND;
	static uint64_t last_tick = -HOOK_TICK_INTERVAL;

	while (1) {
		uint64_t t = get_time().val;
		int next = 0;
		int i;

		/* Handle deferred routines */
		for (i = 0; i < defer_count; i++) {
			if (defer_until[i] && defer_until[i] < t) {
				CPRINTF("[%T hook call deferred 0x%p]\n",
					__deferred_funcs[i].routine);
				/*
				 * Call deferred function.  Clear timer first,
				 * so it can request itself be called later.
				 */
				defer_until[i] = 0;
				__deferred_funcs[i].routine();
			}
		}

		if (t - last_tick >= HOOK_TICK_INTERVAL) {
			hook_notify(HOOK_TICK);
			last_tick = t;
		}

		if (t - last_second >= SECOND) {
			hook_notify(HOOK_SECOND);
			last_second = t;
		}

		/* Calculate when next tick needs to occur */
		t = get_time().val;
		if (last_tick + HOOK_TICK_INTERVAL > t)
			next = last_tick + HOOK_TICK_INTERVAL - t;

		/* Wake earlier if needed by a deferred routine */
		for (i = 0; i < defer_count && next > 0; i++) {
			if (!defer_until[i])
				continue;

			if (defer_until[i] < t)
				next = 0;
			else if (defer_until[i] - t < next)
				next = defer_until[i] - t;
		}

		/* Sleep until the next event */
		if (next > 0)
			task_wait_event(next);
	}
}
