/* Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* Power button and lid switch module for Chrome EC */

#include "chipset.h"
#include "console.h"
#include "eoption.h"
#include "gpio.h"
#include "hooks.h"
#include "keyboard.h"
#include "keyboard_scan.h"
#include "lpc.h"
#include "ec_commands.h"
#include "power_button.h"
#include "pwm.h"
#include "system.h"
#include "task.h"
#include "timer.h"
#include "util.h"

/* Console output macros */
#define CPUTS(outstr) cputs(CC_POWERBTN, outstr)
#define CPRINTF(format, args...) cprintf(CC_POWERBTN, format, ## args)

/* When chipset is on, we stretch the power button signal to it so chipset
 * hard-reset is triggered at ~8 sec, not ~4 sec:
 *
 *   PWRBTN#   ---                      ----
 *     to EC     |______________________|
 *
 *
 *   PWRBTN#   ---  ---------           ----
 *    to PCH     |__|       |___________|
 *                t0    t1    held down
 *
 *   scan code   |                      |
 *    to host    v                      v
 *     @S0   make code             break code
 */
/* TODO: link to full power button / lid switch state machine description. */
#define PWRBTN_DEBOUNCE_US 30000  /* Debounce time for power button */
#define PWRBTN_DELAY_T0    32000  /* 32ms (PCH requires >16ms) */
#define PWRBTN_DELAY_T1    (4000000 - PWRBTN_DELAY_T0)  /* 4 secs - t0 */
#define PWRBTN_INITIAL_US  200000 /* Length of time to stretch initial power
				   * button press to give chipset a chance to
				   * wake up (~100ms) and react to the press
				   * (~16ms).  Also used as pulse length for
				   * simulated power button presses when the
				   * system is off. */

#define LID_DEBOUNCE_US    30000  /* Debounce time for lid switch */

enum power_button_state {
	PWRBTN_STATE_IDLE = 0,      /* Button up; state machine idle */
	PWRBTN_STATE_PRESSED,       /* Button pressed; debouncing done */
	PWRBTN_STATE_T0,            /* Button down, chipset on; sending
				     * initial short pulse */
	PWRBTN_STATE_T1,            /* Button down, chipset on; delaying
				     * until we should reassert signal */
	PWRBTN_STATE_HELD,          /* Button down, signal asserted to
				     * chipset */
	PWRBTN_STATE_LID_OPEN,      /* Force pulse due to lid-open event */
	PWRBTN_STATE_RELEASED,      /* Button released; debouncing done */
	PWRBTN_STATE_EAT_RELEASE,   /* Ignore next button release */
	PWRBTN_STATE_BOOT_RECOVERY, /* Forced pulse at EC boot */
	PWRBTN_STATE_WAS_OFF,       /* Chipset was off; stretching pulse */
};
static enum power_button_state pwrbtn_state = PWRBTN_STATE_IDLE;

static const char * const state_names[] = {
	"idle",
	"pressed",
	"t0",
	"t1",
	"held",
	"lid-open",
	"released",
	"eat-release",
	"recovery",
	"was-off",
};

/* Time for next state transition of power button state machine, or 0 if the
 * state doesn't have a timeout. */
static uint64_t tnext_state;

/* Debounce timeouts for power button and lid switch.  0 means the signal is
 * stable (not being debounced). */
static uint64_t tdebounce_lid;
static uint64_t tdebounce_pwr;

static uint8_t *memmap_switches;
static int debounced_lid_open;
static int debounced_power_pressed;
static int ac_changed;
static int simulate_power_pressed;


/* Update status of non-debounced switches */
static void update_other_switches(void)
{
	if (gpio_get_level(GPIO_WRITE_PROTECT) == 0)
		*memmap_switches |= EC_SWITCH_WRITE_PROTECT_DISABLED;
	else
		*memmap_switches &= ~EC_SWITCH_WRITE_PROTECT_DISABLED;

	if (keyboard_scan_recovery_pressed())
		*memmap_switches |= EC_SWITCH_KEYBOARD_RECOVERY;
	else
		*memmap_switches &= ~EC_SWITCH_KEYBOARD_RECOVERY;

	if (gpio_get_level(GPIO_RECOVERYn) == 0)
		*memmap_switches |= EC_SWITCH_DEDICATED_RECOVERY;
	else
		*memmap_switches &= ~EC_SWITCH_DEDICATED_RECOVERY;

	/* Was this a reboot requesting recovery? */
	/* TODO: should use a different flag, not the dedicated recovery
	 * switch flag! */
	if (system_get_recovery_required())
		*memmap_switches |= EC_SWITCH_DEDICATED_RECOVERY;

#ifdef CONFIG_FAKE_DEV_SWITCH
	if (eoption_get_bool(EOPTION_BOOL_FAKE_DEV))
		*memmap_switches |= EC_SWITCH_FAKE_DEVELOPER;
	else
		*memmap_switches &= ~EC_SWITCH_FAKE_DEVELOPER;
#endif
}


static void set_pwrbtn_to_pch(int high)
{
	CPRINTF("[%T PB PCH pwrbtn=%s]\n", high ? "HIGH" : "LOW");
	gpio_set_level(GPIO_PCH_PWRBTNn, high);
}


/* Get raw power button signal state; 1 if power button is pressed, 0 if not
 * pressed. */
static int get_power_button_pressed(void)
{
	if (simulate_power_pressed)
		return 1;

	return gpio_get_level(GPIO_POWER_BUTTONn) ? 0 : 1;
}


/* Get raw lid switch state; 1 if lid is open, 0 if closed. */
static int get_lid_open(void)
{
	return gpio_get_level(GPIO_LID_SWITCHn) ? 1 : 0;
}


static void update_backlight(void)
{
	/* Only enable the backlight if the lid is open */
	if (gpio_get_level(GPIO_PCH_BKLTEN) && debounced_lid_open)
		gpio_set_level(GPIO_ENABLE_BACKLIGHT, 1);
	else
		gpio_set_level(GPIO_ENABLE_BACKLIGHT, 0);

	/* Same with keyboard backlight */
	pwm_enable_keyboard_backlight(debounced_lid_open);
}


/* Handle debounced power button down */
static void power_button_pressed(uint64_t tnow)
{
	if (debounced_power_pressed == 1) {
		CPRINTF("[%T PB already pressed]\n");
		return;
	}

	CPRINTF("[%T PB pressed]\n");
	debounced_power_pressed = 1;
	pwrbtn_state = PWRBTN_STATE_PRESSED;
	tnext_state = tnow;
	*memmap_switches |= EC_SWITCH_POWER_BUTTON_PRESSED;
	keyboard_set_power_button(1);
	lpc_set_host_events(EC_HOST_EVENT_MASK(EC_HOST_EVENT_POWER_BUTTON));
}


/* Handle debounced power button up */
static void power_button_released(uint64_t tnow)
{
	if (debounced_power_pressed == 0) {
		CPRINTF("[%T PB already released]\n");
		return;
	}

	CPRINTF("[%T PB released]\n");
	debounced_power_pressed = 0;
	pwrbtn_state = PWRBTN_STATE_RELEASED;
	tnext_state = tnow;
	*memmap_switches &= ~EC_SWITCH_POWER_BUTTON_PRESSED;
	keyboard_set_power_button(0);
	keyboard_enable_scanning(1);
}


/* Handle lid open */
static void lid_switch_open(uint64_t tnow)
{
	if (debounced_lid_open) {
		CPRINTF("[%T PB lid already open]\n");
		return;
	}

	CPRINTF("[%T PB lid open]\n");
	debounced_lid_open = 1;
	*memmap_switches |= EC_SWITCH_LID_OPEN;
	hook_notify(HOOK_LID_CHANGE, 0);
	update_backlight();
	lpc_set_host_events(EC_HOST_EVENT_MASK(EC_HOST_EVENT_LID_OPEN));

	/* If the chipset is off, send a power button pulse to wake up the
	 * chipset. */
	if (chipset_in_state(CHIPSET_STATE_ANY_OFF)) {
		chipset_exit_hard_off();
		set_pwrbtn_to_pch(0);
		pwrbtn_state = PWRBTN_STATE_LID_OPEN;
		tnext_state = tnow + PWRBTN_INITIAL_US;
		task_wake(TASK_ID_POWERBTN);
	}
}


/* Handle lid close */
static void lid_switch_close(uint64_t tnow)
{
	if (!debounced_lid_open) {
		CPRINTF("[%T PB lid already closed]\n");
		return;
	}

	CPRINTF("[%T PB lid close]\n");
	debounced_lid_open = 0;
	*memmap_switches &= ~EC_SWITCH_LID_OPEN;
	hook_notify(HOOK_LID_CHANGE, 0);
	update_backlight();
	lpc_set_host_events(EC_HOST_EVENT_MASK(EC_HOST_EVENT_LID_CLOSED));
}


/* Handle debounced power button changing state */
static void power_button_changed(uint64_t tnow)
{
	if (pwrbtn_state == PWRBTN_STATE_BOOT_RECOVERY ||
	    pwrbtn_state == PWRBTN_STATE_LID_OPEN ||
	    pwrbtn_state == PWRBTN_STATE_WAS_OFF) {
		/* Ignore all power button changes during an initial pulse */
		CPRINTF("[%T PB ignoring change]\n");
		return;
	}

	if (get_power_button_pressed()) {
		/* Power button pressed */
		power_button_pressed(tnow);
	} else {
		/* Power button released */
		if (pwrbtn_state == PWRBTN_STATE_EAT_RELEASE) {
			/* Ignore the first power button release if we already
			 * told the PCH the power button was released. */
			CPRINTF("[%T PB ignoring release]\n");
			pwrbtn_state = PWRBTN_STATE_IDLE;
			return;
		}

		power_button_released(tnow);
	}
}


/* Handle debounced lid switch changing state */
static void lid_switch_changed(uint64_t tnow)
{
	if (get_lid_open())
		lid_switch_open(tnow);
	else
		lid_switch_close(tnow);
}


/* Set initial power button state */
static void set_initial_pwrbtn_state(void)
{
	debounced_power_pressed = get_power_button_pressed();

	if (system_get_reset_cause() == SYSTEM_RESET_RESET_PIN) {
		/* Reset triggered by keyboard-controlled reset, so override
		 * the power button signal to the PCH. */
		if (keyboard_scan_recovery_pressed()) {
			/* In recovery mode, so send a power button pulse to
			 * the PCH so it powers on. */
			CPRINTF("[%T PB init-recovery]\n");
			chipset_exit_hard_off();
			set_pwrbtn_to_pch(0);
			pwrbtn_state = PWRBTN_STATE_BOOT_RECOVERY;
			tnext_state = get_time().val + PWRBTN_INITIAL_US;
		} else {
			/* Keyboard-controlled reset, so don't let the PCH see
			 * that the power button was pressed.  Otherwise, it
			 * might power on. */
			CPRINTF("[%T PB init-reset]\n");
			set_pwrbtn_to_pch(1);
			if (get_power_button_pressed())
				pwrbtn_state = PWRBTN_STATE_EAT_RELEASE;
			else
				pwrbtn_state = PWRBTN_STATE_IDLE;
		}
	} else if (system_get_reset_cause() == SYSTEM_RESET_WAKE_PIN &&
		   debounced_lid_open == 1) {
		/* Reset triggered by wake pin and lid is open, so power on the
		 * system.   Note that on EVT+, if the system is off, lid is
		 * open, and you plug it in, it'll turn on due to AC detect. */
		CPRINTF("[%T PB init-hib-wake]\n");
		chipset_exit_hard_off();
		set_pwrbtn_to_pch(0);
		if (get_power_button_pressed())
			pwrbtn_state = PWRBTN_STATE_WAS_OFF;
		else
			pwrbtn_state = PWRBTN_STATE_LID_OPEN;
		tnext_state = get_time().val + PWRBTN_INITIAL_US;
	} else {
		/* Copy initial power button state to PCH and memory-mapped
		 * switch positions. */
		set_pwrbtn_to_pch(get_power_button_pressed() ? 0 : 1);
		if (get_power_button_pressed()) {
			/* Wake chipset if power button is pressed at boot */
			chipset_exit_hard_off();
			*memmap_switches |= EC_SWITCH_POWER_BUTTON_PRESSED;
		}
	}
}


int power_ac_present(void)
{
	return gpio_get_level(GPIO_AC_PRESENT);
}


int power_lid_open_debounced(void)
{
	return debounced_lid_open;
}

/*****************************************************************************/
/* Task / state machine */

/* Power button state machine.  Passed current time from usec counter. */
static void state_machine(uint64_t tnow)
{
	/* Not the time to move onto next state */
	if (tnow < tnext_state)
		return;

	/* States last forever unless otherwise specified */
	tnext_state = 0;

	switch (pwrbtn_state) {
	case PWRBTN_STATE_PRESSED:
		if (chipset_in_state(CHIPSET_STATE_ANY_OFF)) {
			/* Chipset is off, so wake the chipset and send it a
			 * long enough pulse to wake up.  After that we'll
			 * reflect the true power button state.  If we don't
			 * stretch the pulse here, the user may release the
			 * power button before the chipset finishes waking from
			 * hard off state. */
			chipset_exit_hard_off();
			tnext_state = tnow + PWRBTN_INITIAL_US;
			pwrbtn_state = PWRBTN_STATE_WAS_OFF;
		} else {
			/* Chipset is on, so send the chipset a pulse */
			tnext_state = tnow + PWRBTN_DELAY_T0;
			pwrbtn_state = PWRBTN_STATE_T0;
		}
		set_pwrbtn_to_pch(0);
		break;
	case PWRBTN_STATE_T0:
		tnext_state = tnow + PWRBTN_DELAY_T1;
		pwrbtn_state = PWRBTN_STATE_T1;
		set_pwrbtn_to_pch(1);
		break;
	case PWRBTN_STATE_T1:
		/* If the chipset is already off, don't tell it the power
		 * button is down; it'll just cause the chipset to turn on
		 * again. */
		if (chipset_in_state(CHIPSET_STATE_ANY_OFF))
			CPRINTF("[%T PB chipset already off]\n");
		else
			set_pwrbtn_to_pch(0);
		pwrbtn_state = PWRBTN_STATE_HELD;
		break;
	case PWRBTN_STATE_RELEASED:
	case PWRBTN_STATE_LID_OPEN:
		set_pwrbtn_to_pch(1);
		pwrbtn_state = PWRBTN_STATE_IDLE;
		break;
	case PWRBTN_STATE_BOOT_RECOVERY:
		/* Initial forced pulse is done.  Ignore the actual power
		 * button until it's released, so that holding down the
		 * recovery combination doesn't cause the chipset to shut back
		 * down. */
		set_pwrbtn_to_pch(1);
		if (get_power_button_pressed())
			pwrbtn_state = PWRBTN_STATE_EAT_RELEASE;
		else
			pwrbtn_state = PWRBTN_STATE_IDLE;
		break;
	case PWRBTN_STATE_WAS_OFF:
		/* Done stretching initial power button signal, so show the
		 * true power button state to the PCH. */
		if (get_power_button_pressed()) {
			/* User is still holding the power button */
			pwrbtn_state = PWRBTN_STATE_HELD;
		} else {
			/* Stop stretching the power button press */
			power_button_released(tnow);
		}
		break;
	case PWRBTN_STATE_IDLE:
	case PWRBTN_STATE_HELD:
	case PWRBTN_STATE_EAT_RELEASE:
		/* Do nothing */
		break;
	}
}


void power_button_task(void)
{
	uint64_t t;
	uint64_t tsleep;

	while (1) {
		t = get_time().val;

		/* Handle AC state changes */
		if (ac_changed) {
			ac_changed = 0;
			hook_notify(HOOK_AC_CHANGE, 0);
		}

		/* Handle debounce timeouts for power button and lid switch */
		if (tdebounce_pwr && t >= tdebounce_pwr) {
			tdebounce_pwr = 0;
			if (get_power_button_pressed() !=
			    debounced_power_pressed)
				power_button_changed(t);
		}
		if (tdebounce_lid && t >= tdebounce_lid) {
			tdebounce_lid = 0;
			if (get_lid_open() != debounced_lid_open)
				lid_switch_changed(t);
		}

		/* Handle non-debounced switches */
		update_other_switches();

		/* Update state machine */
		CPRINTF("[%T PB task %d = %s, sw 0x%02x]\n", pwrbtn_state,
			state_names[pwrbtn_state], *memmap_switches);

		state_machine(t);

		/* Sleep until our next timeout */
		tsleep = -1;
		if (tdebounce_pwr && tdebounce_pwr < tsleep)
			tsleep = tdebounce_pwr;
		if (tdebounce_lid && tdebounce_lid < tsleep)
			tsleep = tdebounce_lid;
		if (tnext_state && tnext_state < tsleep)
			tsleep = tnext_state;
		t = get_time().val;
		if (tsleep > t) {
			unsigned d = tsleep == -1 ? -1 : (unsigned)(tsleep - t);
			/* (Yes, the conversion from uint64_t to unsigned could
			 * theoretically overflow if we wanted to sleep for
			 * more than 2^32 us, but our timeouts are small enough
			 * that can't happen - and even if it did, we'd just go
			 * back to sleep after deciding that we woke up too
			 * early.) */
			CPRINTF("[%T PB task %d = %s, wait %d]\n", pwrbtn_state,
			state_names[pwrbtn_state], d);
			task_wait_event(d);
		}
	}
}

/*****************************************************************************/
/* Hooks */

static int power_button_init(void)
{
	/* Set up memory-mapped switch positions */
	memmap_switches = lpc_get_memmap_range() + EC_MEMMAP_SWITCHES;
	*memmap_switches = 0;
	if (get_lid_open()) {
		debounced_lid_open = 1;
		*memmap_switches |= EC_SWITCH_LID_OPEN;
	}
	update_other_switches();
	update_backlight();

	set_initial_pwrbtn_state();

	/* Enable interrupts, now that we've initialized */
	gpio_enable_interrupt(GPIO_AC_PRESENT);
	gpio_enable_interrupt(GPIO_LID_SWITCHn);
	gpio_enable_interrupt(GPIO_POWER_BUTTONn);
	gpio_enable_interrupt(GPIO_RECOVERYn);
	gpio_enable_interrupt(GPIO_WRITE_PROTECT);

	return EC_SUCCESS;
}
DECLARE_HOOK(HOOK_INIT, power_button_init, HOOK_PRIO_DEFAULT);


void power_button_interrupt(enum gpio_signal signal)
{
	/* Reset debounce time for the changed signal */
	switch (signal) {
	case GPIO_LID_SWITCHn:
		/* Reset lid debounce time */
		tdebounce_lid = get_time().val + LID_DEBOUNCE_US;
		break;
	case GPIO_POWER_BUTTONn:
		/* Reset power button debounce time */
		tdebounce_pwr = get_time().val + PWRBTN_DEBOUNCE_US;
		if (get_power_button_pressed()) {
			/* We want to disable the matrix scan as soon as
			 * possible to reduce the risk of false-reboot triggered
			 * by those keys on the same column with ESC key. */
			keyboard_enable_scanning(0);
		}
		break;
	case GPIO_PCH_BKLTEN:
		update_backlight();
		break;
	case GPIO_AC_PRESENT:
		ac_changed = 1;
		break;
	default:
		/* Non-debounced switches; we'll update their state
		 * automatically the next time through the task loop. */
		break;
	}

	/* We don't have a way to tell the task to wake up at the end of the
	 * debounce interval; wake it up now so it can go back to sleep for the
	 * remainder of the interval.  The alternative would be to have the task
	 * wake up _every_ debounce_us on its own; that's less desirable when
	 * the EC should be sleeping. */
	task_wake(TASK_ID_POWERBTN);
}

/*****************************************************************************/
/* Console commands */

static int command_powerbtn(int argc, char **argv)
{
	int ms = PWRBTN_INITIAL_US / 1000;  /* Press duration in ms */
	char *e;

	if (argc > 1) {
		ms = strtoi(argv[1], &e, 0);
		if (*e)
			return EC_ERROR_INVAL;
	}

	ccprintf("Simulating %d ms power button press.\n", ms);
	simulate_power_pressed = 1;
	tdebounce_pwr = get_time().val + PWRBTN_DEBOUNCE_US;
	task_wake(TASK_ID_POWERBTN);

	usleep(ms * 1000);

	ccprintf("Simulating power button release.\n");
	simulate_power_pressed = 0;
	tdebounce_pwr = get_time().val + PWRBTN_DEBOUNCE_US;
	task_wake(TASK_ID_POWERBTN);

	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(powerbtn, command_powerbtn);


static int command_lidopen(int argc, char **argv)
{
	lid_switch_open(get_time().val);
	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(lidopen, command_lidopen);


static int command_lidclose(int argc, char **argv)
{
	lid_switch_close(get_time().val);
	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(lidclose, command_lidclose);
