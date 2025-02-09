/* Copyright 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Event handling in MKBP keyboard protocol
 */

#include "atomic.h"
#include "chipset.h"
#include "gpio.h"
#include "host_command.h"
#include "host_command_heci.h"
#include "hwtimer.h"
#include "timer.h"
#include "link_defs.h"
#include "mkbp_event.h"
#include "power.h"
#include "util.h"

#define CPUTS(outstr) cputs(CC_COMMAND, outstr)
#define CPRINTS(format, args...) cprints(CC_COMMAND, format, ## args)
#define CPRINTF(format, args...) cprintf(CC_COMMAND, format, ## args)

/*
 * Tracks the current state of the MKBP interrupt send from the EC to the AP.
 *
 * The inactive state is only valid when there are no events to set to the AP.
 * If the AP is asleep, then some events are not worth waking the AP up, so the
 * interrupt could remain in an inactive in that case.
 *
 * The transition state (INTERRUPT_INACTIVE_TO_ACTIVE) is used to track the
 * sometimes lock transition for a "rising edge" for platforms that send the
 * rising edge interrupt through a host communication layer
 *
 * The active state represents that a rising edge interrupt has already been
 * sent to the AP, and the EC is waiting for the AP to call get next event
 * host command to consume all of the events (at which point the state will
 * move to inactive).
 *
 * The transition from ACTIVE -> INACTIVE is considerer to be simple meaning
 * the operation can be performed within a blocking mutex (e.g. no-op or setting
 * a gpio).
 */
enum interrupt_state {
	INTERRUPT_INACTIVE,
	INTERRUPT_INACTIVE_TO_ACTIVE, /* Transitioning */
	INTERRUPT_ACTIVE,
};

struct mkbp_state {
	struct mutex lock;
	uint32_t events;
	enum interrupt_state interrupt;
	/*
	 * Tracks unique transitions to INTERRUPT_INACTIVE_TO_ACTIVE allowing
	 * only the most recent transition to finish the transition to a final
	 * state -- either active or inactive depending on the result of the
	 * operation.
	 */
	uint8_t interrupt_id;
	/*
	 * Tracks the number of consecutive failed attempts for the AP to poll
	 * get_next_events in order to limit the retry logic.
	 */
	uint8_t failed_attempts;
};

static struct mkbp_state state;
uint32_t mkbp_last_event_time;

#ifdef CONFIG_MKBP_USE_GPIO
static int mkbp_set_host_active_via_gpio(int active, uint32_t *timestamp)
{
	/*
	 * If we want to take a timestamp, then disable interrupts temporarily
	 * to ensure that the timestamp is as close as possible to the setting
	 * of the GPIO pin in hardware (i.e. we aren't interrupted between
	 * taking the timestamp and setting the gpio)
	 */
	if (timestamp) {
		interrupt_disable();
		*timestamp = __hw_clock_source_read();
	}

	gpio_set_level(GPIO_EC_INT_L, !active);

	if (timestamp)
		interrupt_enable();

	return EC_SUCCESS;
}
#endif

#ifdef CONFIG_MKBP_USE_HOST_EVENT
static int mkbp_set_host_active_via_event(int active, uint32_t *timestamp)
{
	/* This should be moved into host_set_single_event for more accuracy */
	if (timestamp)
		*timestamp = __hw_clock_source_read();
	if (active)
		host_set_single_event(EC_HOST_EVENT_MKBP);
	return EC_SUCCESS;
}
#endif

#ifdef CONFIG_MKBP_USE_HECI
static int mkbp_set_host_active_via_heci(int active, uint32_t *timestamp)
{
	if (active)
		return heci_send_mkbp_event(timestamp);
	return EC_SUCCESS;
}
#endif

/*
 * This communicates to the AP whether an MKBP event is currently available
 * for processing.
 *
 * NOTE: When active is 0 this function CANNOT de-schedule. It must be very
 * simple like toggling a GPIO or no-op
 *
 * @param active  1 if there is an event, 0 otherwise
 * @param timestamp, if non-null this variable will be written as close to the
 *			hardware interrupt from EC->AP as possible.
 */
static int mkbp_set_host_active(int active, uint32_t *timestamp)
{
#if defined(CONFIG_MKBP_USE_CUSTOM)
	return mkbp_set_host_active_via_custom(active, timestamp);
#elif defined(CONFIG_MKBP_USE_HOST_EVENT)
	return mkbp_set_host_active_via_event(active, timestamp);
#elif defined(CONFIG_MKBP_USE_GPIO)
	return mkbp_set_host_active_via_gpio(active, timestamp);
#elif defined(CONFIG_MKBP_USE_HECI)
	return mkbp_set_host_active_via_heci(active, timestamp);
#endif
}

#ifdef CONFIG_MKBP_WAKEUP_MASK
/**
 * Check if the host is sleeping. Check our power state in addition to the
 * self-reported sleep state of host (CONFIG_POWER_TRACK_HOST_SLEEP_STATE).
 */
static inline int host_is_sleeping(void)
{
	int is_sleeping = !chipset_in_state(CHIPSET_STATE_ON);

#ifdef CONFIG_POWER_TRACK_HOST_SLEEP_STATE
	enum host_sleep_event sleep_state = power_get_host_sleep_state();
	is_sleeping |=
		(sleep_state == HOST_SLEEP_EVENT_S3_SUSPEND ||
		 sleep_state == HOST_SLEEP_EVENT_S3_WAKEABLE_SUSPEND);
#endif
	return is_sleeping;
}
#endif /* CONFIG_MKBP_WAKEUP_MASK */

/*
 * This is the deferred function that ensures that we attempt to set the MKBP
 * interrupt again if there was a failure in the system (EC or AP) and the AP
 * never called get_next_event.
 */
static void force_mkbp_if_events(void);
DECLARE_DEFERRED(force_mkbp_if_events);

static void activate_mkbp_with_events(uint32_t events_to_add)
{
	int interrupt_id = -1;
	int skip_interrupt = 0;
	int rv, schedule_deferred = 0;

#ifdef CONFIG_MKBP_WAKEUP_MASK
	/* Only assert interrupt for wake events if host is sleeping */
	skip_interrupt = host_is_sleeping() &&
			 !(host_get_events() & CONFIG_MKBP_WAKEUP_MASK);
#endif

	mutex_lock(&state.lock);
	state.events |= events_to_add;

	/* To skip the interrupt, we cannot have the EC_MKBP_EVENT_KEY_MATRIX */
	skip_interrupt = skip_interrupt &&
			 !(state.events & BIT(EC_MKBP_EVENT_KEY_MATRIX));

	if (state.events && state.interrupt == INTERRUPT_INACTIVE &&
	    !skip_interrupt) {
		state.interrupt = INTERRUPT_INACTIVE_TO_ACTIVE;
		interrupt_id = ++state.interrupt_id;
	}
	mutex_unlock(&state.lock);

	/* If we don't need to send an interrupt we are done */
	if (interrupt_id < 0)
		return;

	/* Send a rising edge MKBP interrupt */
	rv = mkbp_set_host_active(1, &mkbp_last_event_time);

	/*
	 * If this was the last interrupt to the AP, update state;
	 * otherwise the latest interrupt should update state.
	 */
	mutex_lock(&state.lock);
	if (state.interrupt == INTERRUPT_INACTIVE_TO_ACTIVE &&
	    interrupt_id == state.interrupt_id) {
		schedule_deferred = 1;
		state.interrupt = rv == EC_SUCCESS ? INTERRUPT_ACTIVE
						   : INTERRUPT_INACTIVE;
	}
	mutex_unlock(&state.lock);

	if (schedule_deferred) {
		hook_call_deferred(&force_mkbp_if_events_data, SECOND);
		if (rv != EC_SUCCESS)
			CPRINTS("Could not activate MKBP (%d). Deferring", rv);
	}
}

/*
 * This is the deferred function that ensures that we attempt to set the MKBP
 * interrupt again if there was a failure in the system (EC or AP) and the AP
 * never called get_next_event.
 */
static void force_mkbp_if_events(void)
{
	int toggled = 0;

	mutex_lock(&state.lock);
	if (state.interrupt == INTERRUPT_ACTIVE) {
		if (++state.failed_attempts < 3) {
			state.interrupt = INTERRUPT_INACTIVE;
			toggled = 1;
		}
	}
	mutex_unlock(&state.lock);

	if (toggled)
		CPRINTS("MKBP not cleared within threshold, toggling.");

	activate_mkbp_with_events(0);
}

int mkbp_send_event(uint8_t event_type)
{
	activate_mkbp_with_events(BIT(event_type));

	return 1;
}

static int set_inactive_if_no_events(void)
{
	int interrupt_cleared;

	mutex_lock(&state.lock);
	interrupt_cleared = !state.events;
	if (interrupt_cleared) {
		state.interrupt = INTERRUPT_INACTIVE;
		state.failed_attempts = 0;
		/* Only simple tasks (i.e. gpio set or no-op) allowed here */
		mkbp_set_host_active(0, NULL);
	}
	mutex_unlock(&state.lock);

	/* Cancel our safety net since the events were cleared. */
	if (interrupt_cleared)
		hook_call_deferred(&force_mkbp_if_events_data, -1);

	return interrupt_cleared;
}

/* This can only be called when the state.lock mutex is held */
static int take_event_if_set(uint8_t event_type)
{
	int taken;

	taken = state.events & BIT(event_type);
	state.events &= ~BIT(event_type);

	return taken;
}

static int mkbp_get_next_event(struct host_cmd_handler_args *args)
{
	static int last;
	int i, data_size, evt;
	uint8_t *resp = args->response;
	const struct mkbp_event_source *src;

	do {
		/*
		 * Find the next event to service.  We do this in a round-robin
		 * way to make sure no event gets starved.
		 */
		mutex_lock(&state.lock);
		for (i = 0; i < EC_MKBP_EVENT_COUNT; ++i)
			if (take_event_if_set((last + i) % EC_MKBP_EVENT_COUNT))
				break;
		mutex_unlock(&state.lock);

		if (i == EC_MKBP_EVENT_COUNT) {
			if (set_inactive_if_no_events())
				return EC_RES_UNAVAILABLE;
			/* An event was set just now, restart loop. */
			continue;
		}

		evt = (i + last) % EC_MKBP_EVENT_COUNT;
		last = evt + 1;

		for (src = __mkbp_evt_srcs; src < __mkbp_evt_srcs_end; ++src)
			if (src->event_type == evt)
				break;

		if (src == __mkbp_evt_srcs_end)
			return EC_RES_ERROR;

		resp[0] = evt; /* Event type */

		/*
		 * get_data() can return -EC_ERROR_BUSY which indicates that the
		 * next element in the keyboard FIFO does not match what we were
		 * called with.  For example, get_data is expecting a keyboard
		 * matrix, however the next element in the FIFO is a button
		 * event instead.  Therefore, we have to service that button
		 * event first.
		 */
		data_size = src->get_data(resp + 1);
		if (data_size == -EC_ERROR_BUSY) {
			mutex_lock(&state.lock);
			state.events |= BIT(evt);
			mutex_unlock(&state.lock);
		}
	} while (data_size == -EC_ERROR_BUSY);

	/* If there are no more events and we support the "more" flag, set it */
	if (!set_inactive_if_no_events() && args->version >= 2)
		resp[0] |= EC_MKBP_HAS_MORE_EVENTS;

	if (data_size < 0)
		return EC_RES_ERROR;
	args->response_size = 1 + data_size;

	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_GET_NEXT_EVENT,
		     mkbp_get_next_event,
		     EC_VER_MASK(0) | EC_VER_MASK(1) | EC_VER_MASK(2));

#ifdef CONFIG_MKBP_WAKEUP_MASK
static int mkbp_get_wake_mask(struct host_cmd_handler_args *args)
{
	struct ec_response_host_event_mask *r = args->response;

	r->mask = CONFIG_MKBP_WAKEUP_MASK;
	args->response_size = sizeof(*r);

	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_HOST_EVENT_GET_WAKE_MASK,
		     mkbp_get_wake_mask,
		     EC_VER_MASK(0));
#endif
