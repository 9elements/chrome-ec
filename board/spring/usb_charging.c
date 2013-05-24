/* Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* USB charging control for spring board */

#include "adc.h"
#include "board.h"
#include "chipset.h"
#include "clock.h"
#include "console.h"
#include "gpio.h"
#include "hooks.h"
#include "host_command.h"
#include "keyboard_scan.h"
#include "lp5562.h"
#include "pmu_tpschrome.h"
#include "registers.h"
#include "smart_battery.h"
#include "stm32_adc.h"
#include "system.h"
#include "task.h"
#include "timer.h"
#include "tsu6721.h"
#include "util.h"

#define PWM_FREQUENCY 32000 /* Hz */

/* Console output macros */
#define CPUTS(outstr) cputs(CC_USBCHARGE, outstr)
#define CPRINTF(format, args...) cprintf(CC_USBCHARGE, format, ## args)

/* Devices that need VBUS power */
#define POWERED_5000_DEVICE_TYPE (TSU6721_TYPE_OTG)
#define POWERED_3300_DEVICE_TYPE (TSU6721_TYPE_JIG_UART_ON)

/* Toad cable */
#define TOAD_DEVICE_TYPE (TSU6721_TYPE_UART | TSU6721_TYPE_AUDIO3)

/* Voltage threshold of D+ for video */
#define VIDEO_ID_THRESHOLD	1335

/* PWM controlled current limit */
#define I_LIMIT_500MA   90
#define I_LIMIT_1000MA  75
#define I_LIMIT_1500MA  60
#define I_LIMIT_2000MA  50
#define I_LIMIT_2400MA  35
#define I_LIMIT_3000MA  0

/* PWM control loop parameters */
#define PWM_CTRL_MAX_DUTY	96 /* Minimum current for dead battery */
#define PWM_CTRL_BEGIN_OFFSET	90
#define PWM_CTRL_OC_MARGIN	15
#define PWM_CTRL_OC_DETECT_TIME	(800 * MSEC)
#define PWM_CTRL_OC_BACK_OFF	3
#define PWM_CTRL_OC_RETRY	2
#define PWM_CTRL_STEP_DOWN	3
#define PWM_CTRL_STEP_UP	5
#define PWM_CTRL_STEP_FAST_DOWN 15
#define PWM_CTRL_VBUS_HARD_LOW	4400
#define PWM_CTRL_VBUS_LOW	4500
#define PWM_CTRL_VBUS_HIGH	4700 /* Must be higher than 4.5V */
#define PWM_CTRL_VBUS_HIGH_500MA 4550

/* Delay for signals to settle */
#define DELAY_POWER_MS		20
#define DELAY_USB_DP_DN_MS	20
#define DELAY_ID_MUX_MS		30

/*
 * Mapping from PWM duty to current:
 *   Current = A + B * PWM_Duty
 */
#define PWM_MAPPING_A 3012
#define PWM_MAPPING_B (-29)

static int current_dev_type = TSU6721_TYPE_NONE;
static int nominal_pwm_duty;
static int current_pwm_duty;
static int user_pwm_duty = -1;
static int pwm_fast_mode;

static int pending_tsu6721_reset;

static enum {
	LIMIT_NORMAL,
	LIMIT_AGGRESSIVE,
} current_limit_mode = LIMIT_AGGRESSIVE;

static enum {
	ADC_WATCH_NONE,
	ADC_WATCH_TOAD,
} current_watchdog = ADC_WATCH_NONE;

struct {
	int type;
	const char *name;
} const known_dev_types[] = {
	{TSU6721_TYPE_OTG, "OTG"},
	{TSU6721_TYPE_USB_HOST, "USB"},
	{TSU6721_TYPE_CHG12, "Type-1/2-Chg"},
	{TSU6721_TYPE_NON_STD_CHG, "Non-Std-Chg"},
	{TSU6721_TYPE_DCP, "DCP"},
	{TSU6721_TYPE_CDP, "CDP"},
	{TSU6721_TYPE_U200_CHG, "U200-Chg"},
	{TSU6721_TYPE_APPLE_CHG, "Apple-Chg"},
	{TSU6721_TYPE_JIG_UART_ON, "Video"},
	{TSU6721_TYPE_AUDIO3, "Audio-3"},
	{TSU6721_TYPE_UART, "UART"},
	{TSU6721_TYPE_VBUS_DEBOUNCED, "Power"} };

/*
 * Last time we see a power source removed. Also records the power source
 * type and PWM duty cycle at that moment.
 * Index: 0 = Unknown power source.
 *        1 = Recognized power source.
 */
static timestamp_t power_removed_time[2];
static uint32_t power_removed_type[2];
static int power_removed_pwm_duty[2];
static int oc_detect_retry[2] = {PWM_CTRL_OC_RETRY, PWM_CTRL_OC_RETRY};

/* PWM duty cycle limit based on over current event */
static int over_current_pwm_duty;

static enum ilim_config current_ilim_config = ILIM_CONFIG_MANUAL_OFF;

static const int apple_charger_type[4] = {I_LIMIT_500MA,
					  I_LIMIT_1000MA,
					  I_LIMIT_2000MA,
					  I_LIMIT_2400MA};

static int video_power_enabled;

static int get_video_power(void)
{
	return video_power_enabled;
}

static void set_video_power(int enabled)
{
	/* TODO(victoryang): Drop VFET2. See crosbug.com/p/18186 */
	pmu_enable_fet(FET_VIDEO, enabled, NULL);
	pmu_enable_fet(FET_VIDEO2, enabled, NULL);
	video_power_enabled = enabled;
}

static void board_ilim_use_gpio(void)
{
	/* Disable counter */
	STM32_TIM_CR1(3) &= ~0x1;

	/* Disable TIM3 clock */
	STM32_RCC_APB1ENR &= ~0x2;

	/* Switch to GPIO */
	gpio_set_flags(GPIO_ILIM, GPIO_OUTPUT);
}

static void board_ilim_use_pwm(void)
{
	uint32_t val;

	/* Config alt. function (TIM3/PWM) */
	val = STM32_GPIO_CRL_OFF(GPIO_B) & ~0x000f0000;
	val |= 0x00090000;
	STM32_GPIO_CRL_OFF(GPIO_B) = val;

	/* Enable TIM3 clock */
	STM32_RCC_APB1ENR |= 0x2;

	/* Disable counter during setup */
	STM32_TIM_CR1(3) = 0x0000;

	/*
	 * CPU_CLOCK / (PSC + 1) determines how fast the counter operates.
	 * ARR determines the wave period, CCRn determines duty cycle.
	 * Thus, frequency = CPU_CLOCK / (PSC + 1) / ARR.
	 *
	 * Assuming 16MHz clock and ARR=100, PSC needed to achieve PWM_FREQUENCY
	 * is: PSC = CPU_CLOCK / PWM_FREQUENCY / ARR - 1
	 */
	STM32_TIM_PSC(3) = CPU_CLOCK / PWM_FREQUENCY / 100 - 1; /* pre-scaler */
	STM32_TIM_ARR(3) = 100;			/* auto-reload value */
	STM32_TIM_CCR1(3) = 100;		/* duty cycle */

	/* CC1 configured as output, PWM mode 1, preload enable */
	STM32_TIM_CCMR1(3) = (6 << 4) | (1 << 3);

	/* CC1 output enable, active high */
	STM32_TIM_CCER(3) = (1 << 0);

	/* Generate update event to force loading of shadow registers */
	STM32_TIM_EGR(3) |= 1;

	/* Enable auto-reload preload, start counting */
	STM32_TIM_CR1(3) |= (1 << 7) | (1 << 0);
}

void board_ilim_config(enum ilim_config config)
{
	if (config == current_ilim_config)
		return;
	current_ilim_config = config;

	switch (config) {
	case ILIM_CONFIG_MANUAL_OFF:
	case ILIM_CONFIG_MANUAL_ON:
		board_ilim_use_gpio();
		gpio_set_level(GPIO_ILIM,
			       config == ILIM_CONFIG_MANUAL_ON ? 1 : 0);
		break;
	case ILIM_CONFIG_PWM:
		board_ilim_use_pwm();
		break;
	default:
		break;
	}
}

/* Returns Apple charger current limit */
static int board_apple_charger_current(void)
{
	int vp, vn;
	int type = 0;
	int data[ADC_CH_COUNT];

	/* TODO(victoryang): Handle potential race condition. */
	tsu6721_disable_interrupts();
	tsu6721_mux(TSU6721_MUX_USB);
	/* Wait 20ms for signal to stablize */
	msleep(DELAY_USB_DP_DN_MS);
	adc_read_all_channels(data);
	vp = data[ADC_CH_USB_DP_SNS];
	vn = data[ADC_CH_USB_DN_SNS];
	tsu6721_mux(TSU6721_MUX_AUTO);
	tsu6721_enable_interrupts();
	if (vp > 1215)
		type |= 0x2;
	if (vn > 1215)
		type |= 0x1;

	return apple_charger_type[type];
}

static int hard_current_limit(int limit)
{
	if (current_limit_mode == LIMIT_AGGRESSIVE)
		return limit - PWM_CTRL_OC_MARGIN;
	else
		return limit;
}

static int video_dev_type(int device_type)
{
	return (device_type & ~TSU6721_TYPE_USB_HOST) |
	       TSU6721_TYPE_JIG_UART_ON;
}

static int board_probe_video(int device_type)
{
	tsu6721_disable_interrupts();
	gpio_set_level(GPIO_ID_MUX, 1);
	msleep(DELAY_ID_MUX_MS);

	if (adc_read_channel(ADC_CH_USB_DP_SNS) > VIDEO_ID_THRESHOLD) {
		/* Actually an USB host */
		gpio_set_level(GPIO_ID_MUX, 0);
		msleep(DELAY_ID_MUX_MS);
		tsu6721_enable_interrupts();
		return device_type;
	} else {
		/* Not USB host but video */
		device_type = video_dev_type(device_type);
		return device_type;
	}
}

int board_has_high_power_ac(void)
{
	return board_get_usb_dev_type() & TSU6721_TYPE_CHG12;
}

void board_pwm_duty_cycle(int percent)
{
	if (current_ilim_config != ILIM_CONFIG_PWM)
		board_ilim_config(ILIM_CONFIG_PWM);
	if (percent < 0)
		percent = 0;
	if (percent > 100)
		percent = 100;
	STM32_TIM_CCR1(3) = (percent * STM32_TIM_ARR(3)) / 100;
	current_pwm_duty = percent;
}

void board_pwm_init_limit(void)
{
	int dummy;

	/*
	 * Shut off power input if battery is good. Otherwise, leave
	 * 500mA to sustain the system.
	 */
	if (battery_current(&dummy))
		board_pwm_duty_cycle(I_LIMIT_500MA);
	else
		board_ilim_config(ILIM_CONFIG_MANUAL_ON);
}

/**
 * Returns next lower PWM duty cycle, or -1 for unchanged duty cycle.
 */
static int board_pwm_get_next_lower(void)
{
	int fast_next = current_pwm_duty - PWM_CTRL_STEP_FAST_DOWN;

	if (current_limit_mode == LIMIT_AGGRESSIVE) {
		if (pwm_fast_mode &&
		    fast_next >= nominal_pwm_duty - PWM_CTRL_OC_MARGIN &&
		    fast_next >= over_current_pwm_duty)
			return MAX(fast_next, 0);
		if (current_pwm_duty > nominal_pwm_duty -
				       PWM_CTRL_OC_MARGIN &&
		    current_pwm_duty > over_current_pwm_duty &&
		    current_pwm_duty > 0)
			return MAX(current_pwm_duty - PWM_CTRL_STEP_DOWN, 0);
		return -1;
	} else {
		if (current_pwm_duty > nominal_pwm_duty && current_pwm_duty > 0)
			return MAX(current_pwm_duty - PWM_CTRL_STEP_DOWN, 0);
		else
			return -1;
	}
}

static int board_pwm_check_vbus_high(int vbus)
{
	if (vbus > PWM_CTRL_VBUS_HIGH)
		return 1;
	if (vbus > PWM_CTRL_VBUS_HIGH_500MA && current_pwm_duty > I_LIMIT_500MA)
		return 1;
	return 0;
}

static int board_pwm_check_vbus_low(int vbus, int battery_current)
{
	if (battery_current >= 0)
		return vbus < PWM_CTRL_VBUS_LOW && current_pwm_duty < 100;
	else
		return vbus < PWM_CTRL_VBUS_HARD_LOW && current_pwm_duty < 100;
}

static void board_pwm_tweak(void)
{
	int vbus, current;
	int next;

	if (current_ilim_config != ILIM_CONFIG_PWM)
		return;

	vbus = adc_read_channel(ADC_CH_USB_VBUS_SNS);
	if (battery_current(&current))
		return;

	if (user_pwm_duty >= 0) {
		if (current_pwm_duty != user_pwm_duty)
			board_pwm_duty_cycle(user_pwm_duty);
		return;
	}

	/*
	 * If VBUS voltage is too low:
	 *   - If battery is discharging, throttling more is going to draw
	 *     more current from the battery, so do nothing unless VBUS is
	 *     about to be lower than AC good threshold.
	 *   - Otherwise, throttle input current to raise VBUS voltage.
	 * If VBUS voltage is high enough, allow more current until we hit
	 * current limit target.
	 */
	if (board_pwm_check_vbus_low(vbus, current)) {
		board_pwm_duty_cycle(current_pwm_duty + PWM_CTRL_STEP_UP);
		pwm_fast_mode = 0;
		CPRINTF("[%T PWM duty up %d%%]\n", current_pwm_duty);
	} else if (board_pwm_check_vbus_high(vbus)) {
		next = board_pwm_get_next_lower();
		if (next >= 0) {
			board_pwm_duty_cycle(next);
			CPRINTF("[%T PWM duty down %d%%]\n", current_pwm_duty);
		}
	}
}
DECLARE_HOOK(HOOK_SECOND, board_pwm_tweak, HOOK_PRIO_DEFAULT);

void board_pwm_nominal_duty_cycle(int percent)
{
	int dummy;

	if (battery_current(&dummy))
		board_pwm_duty_cycle(percent);
	else if (percent + PWM_CTRL_BEGIN_OFFSET > PWM_CTRL_MAX_DUTY)
		board_pwm_duty_cycle(PWM_CTRL_MAX_DUTY);
	else
		board_pwm_duty_cycle(percent + PWM_CTRL_BEGIN_OFFSET);
	nominal_pwm_duty = percent;
	pwm_fast_mode = 1;
}

void usb_charge_interrupt(enum gpio_signal signal)
{
	task_wake(TASK_ID_PMU_TPS65090_CHARGER);
}

static void board_adc_watch_toad(void)
{
	/* Watch VBUS and interrupt if voltage goes under 3V. */
	adc_enable_watchdog(STM32_AIN(5), 4095, 1800);
	task_clear_pending_irq(STM32_IRQ_ADC_1);
	task_enable_irq(STM32_IRQ_ADC_1);
	current_watchdog = ADC_WATCH_TOAD;
}

static void board_adc_watchdog_interrupt(void)
{
	if (current_watchdog == ADC_WATCH_TOAD) {
		pending_tsu6721_reset = 1;
		task_disable_irq(STM32_IRQ_ADC_1);
		task_wake(TASK_ID_PMU_TPS65090_CHARGER);
	}
}
DECLARE_IRQ(STM32_IRQ_ADC_1, board_adc_watchdog_interrupt, 2);

static int usb_has_power_input(int dev_type)
{
	if (dev_type & TSU6721_TYPE_JIG_UART_ON)
		return 1;
	return (dev_type & TSU6721_TYPE_VBUS_DEBOUNCED) &&
	       !(dev_type & POWERED_5000_DEVICE_TYPE);
}

static int usb_need_boost(int dev_type)
{
	if (dev_type & POWERED_5000_DEVICE_TYPE)
		return 0;
	if (chipset_in_state(CHIPSET_STATE_ON | CHIPSET_STATE_SUSPEND))
		return 1;
	return (dev_type != TSU6721_TYPE_NONE);
}

static void usb_boost_power_hook(int power_on)
{
	if (current_dev_type == TSU6721_TYPE_NONE)
		gpio_set_level(GPIO_BOOST_EN, power_on);
	else if (current_dev_type & TSU6721_TYPE_JIG_UART_ON)
		set_video_power(power_on);
}

static void usb_boost_pwr_on_hook(void) { usb_boost_power_hook(1); }
static void usb_boost_pwr_off_hook(void) { usb_boost_power_hook(0); }
DECLARE_HOOK(HOOK_CHIPSET_PRE_INIT, usb_boost_pwr_on_hook, HOOK_PRIO_DEFAULT);
DECLARE_HOOK(HOOK_CHIPSET_SHUTDOWN, usb_boost_pwr_off_hook, HOOK_PRIO_DEFAULT);

/*
 * When a power source is removed, record time, power source type,
 * and PWM duty cycle. Then when we see a power source, compare type
 * and calculate time difference to determine if we have just
 * encountered an over current event.
 */
static void usb_detect_overcurrent(int dev_type)
{
	if ((current_dev_type & TSU6721_TYPE_VBUS_DEBOUNCED) &&
	    (dev_type == TSU6721_TYPE_NONE)) {
		int idx = !(current_dev_type == TSU6721_TYPE_VBUS_DEBOUNCED);
		power_removed_time[idx] = get_time();
		power_removed_type[idx] = current_dev_type;
		/*
		 * TODO(victoryang): Record the maximum current seen during
		 * retry?
		 */
		power_removed_pwm_duty[idx] = current_pwm_duty;
	} else if (dev_type & TSU6721_TYPE_VBUS_DEBOUNCED) {
		int idx = !(dev_type == TSU6721_TYPE_VBUS_DEBOUNCED);
		timestamp_t now = get_time();
		now.val -= power_removed_time[idx].val;
		if (now.val >= PWM_CTRL_OC_DETECT_TIME) {
			oc_detect_retry[idx] = PWM_CTRL_OC_RETRY;
			return;
		}
		if (power_removed_type[idx] == dev_type) {
			if (oc_detect_retry[idx] > 0) {
				oc_detect_retry[idx]--;
				return;
			}
			over_current_pwm_duty = power_removed_pwm_duty[idx] +
						PWM_CTRL_OC_BACK_OFF;
		}
	}
}

/*
 * Supply 5V VBUS if needed. If we toggle power output, wait for a
 * moment, and then update device type. To avoid race condition, check
 * if power requirement changes during this time.
 */
static int usb_manage_boost(int dev_type)
{
	int need_boost;
	int retry_limit = 3;

	do {
		if (retry_limit-- <= 0)
			break;

		need_boost = usb_need_boost(dev_type);
		if (need_boost != gpio_get_level(GPIO_BOOST_EN)) {
			gpio_set_level(GPIO_BOOST_EN, need_boost);
			msleep(DELAY_POWER_MS);
			dev_type = tsu6721_get_device_type();
			if (gpio_get_level(GPIO_ID_MUX))
				dev_type = video_dev_type(dev_type);
		}
	} while (need_boost == !usb_need_boost(dev_type));

	return dev_type;
}

/* Updates ILIM current limit according to device type. */
static void usb_update_ilim(int dev_type)
{
	if (usb_has_power_input(dev_type)) {
		/* Limit USB port current. 500mA for not listed types. */
		int current_limit = I_LIMIT_500MA;
		if (dev_type & TSU6721_TYPE_CHG12)
			current_limit = I_LIMIT_3000MA;
		else if (dev_type & TSU6721_TYPE_APPLE_CHG)
			current_limit = board_apple_charger_current();
		else if (dev_type & TSU6721_TYPE_CDP)
			current_limit = I_LIMIT_1500MA;
		else if (dev_type & TSU6721_TYPE_DCP)
			current_limit = hard_current_limit(I_LIMIT_1500MA);
		else if (dev_type & TSU6721_TYPE_JIG_UART_ON)
			current_limit = hard_current_limit(I_LIMIT_2000MA);

		board_pwm_nominal_duty_cycle(current_limit);
	} else {
		board_ilim_config(ILIM_CONFIG_MANUAL_ON);
	}
}

static void usb_log_dev_type(int dev_type)
{
	int i = sizeof(known_dev_types) / sizeof(known_dev_types[0]);

	CPRINTF("[%T USB: 0x%06x", dev_type);
	for (--i; i >= 0; --i)
		if (dev_type & known_dev_types[i].type)
			CPRINTF(" %s", known_dev_types[i].name);
	CPRINTF("]\n");
}

static void usb_device_change(int dev_type)
{

	if (current_dev_type == dev_type)
		return;

	over_current_pwm_duty = 0;

	/*
	 * Video output is recognized incorrectly as USB host. When we see
	 * USB host, probe for video output.
	 */
	if (dev_type & TSU6721_TYPE_USB_HOST)
		dev_type = board_probe_video(dev_type);

	usb_detect_overcurrent(dev_type);

	dev_type = usb_manage_boost(dev_type);

	/* Supply 3.3V VBUS if needed. */
	if (dev_type & POWERED_3300_DEVICE_TYPE)
		set_video_power(1);

	usb_update_ilim(dev_type);

	if ((dev_type & TOAD_DEVICE_TYPE) &&
	    (dev_type & TSU6721_TYPE_VBUS_DEBOUNCED))
		board_adc_watch_toad();

	usb_log_dev_type(dev_type);

	keyboard_send_battery_key();

	current_dev_type = dev_type;
	if (dev_type)
		disable_sleep(SLEEP_MASK_USB_PWR);
	else
		enable_sleep(SLEEP_MASK_USB_PWR);
}

/*
 * TODO(victoryang): Get rid of polling loop when ADC watchdog is ready.
 *                   See crosbug.com/p/18171
 */
static void board_usb_monitor_detach(void)
{
	int vbus;

	if (!(current_dev_type & TSU6721_TYPE_JIG_UART_ON))
		return;

	if (adc_read_channel(ADC_CH_USB_DP_SNS) > VIDEO_ID_THRESHOLD) {
		set_video_power(0);
		gpio_set_level(GPIO_ID_MUX, 0);
		msleep(DELAY_ID_MUX_MS);
		tsu6721_enable_interrupts();
		usb_device_change(TSU6721_TYPE_NONE);
	}

	/* Check if there is external power */
	vbus = adc_read_channel(ADC_CH_USB_VBUS_SNS);
	if (get_video_power() && vbus > 4000) {
		set_video_power(0);
	} else if  (!get_video_power() && vbus <= 4000) {
		board_pwm_duty_cycle(100);
		set_video_power(1);
	}
}
DECLARE_HOOK(HOOK_SECOND, board_usb_monitor_detach, HOOK_PRIO_DEFAULT);

void board_usb_charge_update(int force_update)
{
	int int_val = 0;

	if (pending_tsu6721_reset) {
		current_watchdog = ADC_WATCH_NONE;
		adc_disable_watchdog();
		tsu6721_reset();
		force_update = 1;
		pending_tsu6721_reset = 0;
	} else
		int_val = tsu6721_get_interrupts();

	if (int_val & TSU6721_INT_DETACH)
		usb_device_change(TSU6721_TYPE_NONE);
	else if (int_val || force_update)
		usb_device_change(tsu6721_get_device_type());
}

int board_get_usb_dev_type(void)
{
	return current_dev_type;
}

int board_get_usb_current_limit(void)
{
	/* Approximate value by PWM duty cycle */
	return PWM_MAPPING_A + PWM_MAPPING_B * current_pwm_duty;
}

/*
 * Console commands for debugging.
 * TODO(victoryang): Remove after charging control is done.
 */
static int command_ilim(int argc, char **argv)
{
	char *e;
	int percent;

	if (argc >= 2) {
		if (strcasecmp(argv[1], "on") == 0)
			board_ilim_config(ILIM_CONFIG_MANUAL_ON);
		else if (strcasecmp(argv[1], "off") == 0)
			board_ilim_config(ILIM_CONFIG_MANUAL_OFF);
		else {
			percent = strtoi(argv[1], &e, 0);
			if (*e)
				return EC_ERROR_PARAM1;
			board_pwm_duty_cycle(percent);
		}
	}

	if (current_ilim_config == ILIM_CONFIG_MANUAL_ON)
		ccprintf("ILIM is GPIO high\n");
	else if (current_ilim_config == ILIM_CONFIG_MANUAL_OFF)
		ccprintf("ILIM is GPIO low\n");
	else
		ccprintf("ILIM is PWM duty cycle %d%%\n", STM32_TIM_CCR1(3));

	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(ilim, command_ilim,
		"[percent | on | off]",
		"Set or show ILIM duty cycle/GPIO value",
		NULL);

static int command_batdebug(int argc, char **argv)
{
	int val;
	ccprintf("VBUS = %d mV\n", adc_read_channel(ADC_CH_USB_VBUS_SNS));
	ccprintf("VAC = %d mV\n", pmu_adc_read(ADC_VAC, ADC_FLAG_KEEP_ON)
				  * 17000 / 1024);
	ccprintf("IAC = %d mA\n", pmu_adc_read(ADC_IAC, ADC_FLAG_KEEP_ON)
				  * (1000 / R_INPUT_MOHM) * 33 / 1024);
	ccprintf("VBAT = %d mV\n", pmu_adc_read(ADC_VBAT, ADC_FLAG_KEEP_ON)
				  * 17000 / 1024);
	ccprintf("IBAT = %d mA\n", pmu_adc_read(ADC_IBAT, 0)
				  * (1000 / R_BATTERY_MOHM) * 40 / 1024);
	ccprintf("PWM = %d%%\n", STM32_TIM_CCR1(3));
	battery_current(&val);
	ccprintf("Battery Current = %d mA\n", val);
	battery_voltage(&val);
	ccprintf("Battery Voltage= %d mV\n", val);
	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(batdebug, command_batdebug,
			NULL, NULL, NULL);

static int command_current_limit_mode(int argc, char **argv)
{
	if (1 == argc) {
		if (current_limit_mode == LIMIT_NORMAL)
			ccprintf("Normal mode\n");
		else
			ccprintf("Aggressive mode\n");
		return EC_SUCCESS;
	} else if (2 == argc) {
		if (!strcasecmp(argv[1], "normal"))
			current_limit_mode = LIMIT_NORMAL;
		else if (!strcasecmp(argv[1], "aggressive"))
			current_limit_mode = LIMIT_AGGRESSIVE;
		else
			return EC_ERROR_INVAL;
		return EC_SUCCESS;
	}
	return EC_ERROR_INVAL;
}
DECLARE_CONSOLE_COMMAND(limitmode, command_current_limit_mode,
			"[normal | aggressive]",
			"Set current limit mode",
			NULL);

/*****************************************************************************/
/* Host commands */

static int ext_power_command_current_limit(struct host_cmd_handler_args *args)
{
	const struct ec_params_ext_power_current_limit *p = args->params;

	if (system_is_locked())
		return EC_RES_ACCESS_DENIED;

	user_pwm_duty = ((int)(p->limit) - PWM_MAPPING_A) / PWM_MAPPING_B;

	return EC_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_EXT_POWER_CURRENT_LIMIT,
		     ext_power_command_current_limit,
		     EC_VER_MASK(0));
