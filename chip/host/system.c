/* Copyright 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* System module for emulator */

#include "common.h"
#include "host_test.h"
#include "panic.h"
#include "persistence.h"
#include "reboot.h"
#include "system.h"
#include "timer.h"
#include "util.h"

// Forward declaration from <stdlib.h> to avoid declaration conflicts.
void exit(int);

#define SHARED_MEM_SIZE 0x2000 /* bytes */
#define RAM_DATA_SIZE (sizeof(struct panic_data) + 512) /* bytes */
uint8_t __shared_mem_buf[SHARED_MEM_SIZE + RAM_DATA_SIZE];

static char *__ram_data = __shared_mem_buf + SHARED_MEM_SIZE;

static enum system_image_copy_t __running_copy;

static void ramdata_set_persistent(void)
{
	FILE *f = get_persistent_storage("ramdata", "wb");
	int sz;

	ASSERT(f != NULL);

	sz = fwrite(__ram_data, RAM_DATA_SIZE, 1, f);
	ASSERT(sz == 1);

	release_persistent_storage(f);
}

static void ramdata_get_persistent(void)
{
	FILE *f = get_persistent_storage("ramdata", "rb");

	if (f == NULL) {
		fprintf(stderr,
			"No RAM data found. Initializing to 0x00.\n");
		memset(__ram_data, 0, RAM_DATA_SIZE);
		return;
	}

	fread(__ram_data, RAM_DATA_SIZE, 1, f);

	release_persistent_storage(f);

	/*
	 * Assumes RAM data doesn't preserve across reboot except for sysjump.
	 * Clear persistent data once it's read.
	 */
	remove_persistent_storage("ramdata");
}

static void set_image_copy(uint32_t copy)
{
	FILE *f = get_persistent_storage("image_copy", "wb");

	ASSERT(f != NULL);
	ASSERT(fwrite(&copy, sizeof(copy), 1, f) == 1);

	release_persistent_storage(f);
}

static uint32_t get_image_copy(void)
{
	FILE *f = get_persistent_storage("image_copy", "rb");
	uint32_t ret;

	if (f == NULL)
		return SYSTEM_IMAGE_UNKNOWN;
	fread(&ret, sizeof(ret), 1, f);
	release_persistent_storage(f);
	remove_persistent_storage("image_copy");

	return ret;
}

static void save_reset_flags(uint32_t flags)
{
	FILE *f = get_persistent_storage("reset_flags", "wb");

	ASSERT(f != NULL);
	ASSERT(fwrite(&flags, sizeof(flags), 1, f) == 1);

	release_persistent_storage(f);
}

static uint32_t load_reset_flags(void)
{
	FILE *f = get_persistent_storage("reset_flags", "rb");
	uint32_t ret;

	if (f == NULL)
		return RESET_FLAG_POWER_ON;
	fread(&ret, sizeof(ret), 1, f);
	release_persistent_storage(f);
	remove_persistent_storage("reset_flags");

	return ret;
}

static void save_time(timestamp_t t)
{
	FILE *f = get_persistent_storage("time", "wb");

	ASSERT(f != NULL);
	ASSERT(fwrite(&t, sizeof(t), 1, f) == 1);

	release_persistent_storage(f);
}

static int load_time(timestamp_t *t)
{
	FILE *f = get_persistent_storage("time", "rb");

	if (f == NULL)
		return 0;
	fread(t, sizeof(*t), 1, f);
	release_persistent_storage(f);
	remove_persistent_storage("time");

	return 1;
}

test_mockable struct panic_data *panic_get_data(void)
{
	return (struct panic_data *)
		(__ram_data + RAM_DATA_SIZE - sizeof(struct panic_data));
}

test_mockable void system_reset(int flags)
{
	uint32_t save_flags = 0;
	if (flags & SYSTEM_RESET_PRESERVE_FLAGS)
		save_flags = system_get_reset_flags() | RESET_FLAG_PRESERVED;
	if (flags & SYSTEM_RESET_LEAVE_AP_OFF)
		save_flags |= RESET_FLAG_AP_OFF;
	if (flags & SYSTEM_RESET_HARD)
		save_flags |= RESET_FLAG_HARD;
	if (save_flags)
		save_reset_flags(save_flags);
	emulator_reboot();
}

test_mockable void system_hibernate(uint32_t seconds, uint32_t microseconds)
{
	uint32_t i;

	if (board_hibernate)
		board_hibernate();

	save_reset_flags(RESET_FLAG_HIBERNATE);

	if (!seconds && !microseconds)
		exit(EXIT_CODE_HIBERNATE);

	for (i = 0; i < seconds; ++i)
		udelay(SECOND);
	udelay(microseconds);
	emulator_reboot();
}

test_mockable int system_is_locked(void)
{
	return 0;
}

#ifdef TEST_FUZZ
/* When fuzzing, do not allow sysjumps. */
int system_run_image_copy(enum system_image_copy_t copy)
{
	ccprints("Emulator would sysjump here. Fuzzing: doing nothing.");
	return EC_ERROR_UNKNOWN;
}
#endif

const char *system_get_chip_vendor(void)
{
	return "chromeos";
}

const char *system_get_chip_name(void)
{
	return "emu";
}

const char *system_get_chip_revision(void)
{
	return "";
}

int system_get_bbram(enum system_bbram_idx idx, uint8_t *value)
{
	return EC_ERROR_UNIMPLEMENTED;
}

int system_set_bbram(enum system_bbram_idx idx, uint8_t value)
{
	return EC_ERROR_UNIMPLEMENTED;
}

enum system_image_copy_t system_get_image_copy(void)
{
	return __running_copy;
}

int system_set_scratchpad(uint32_t value)
{
	FILE *f = get_persistent_storage("scratchpad", "w");

	fprintf(f, "%lu", value);
	release_persistent_storage(f);

	return EC_SUCCESS;
}

uint32_t system_get_scratchpad(void)
{
	FILE *f = get_persistent_storage("scratchpad", "r");
	uint32_t value;
	int success;

	if (f == NULL)
		return 0;

	success = fscanf(f, "%u", &value);
	release_persistent_storage(f);

	if (success)
		return value;
	else
		return 0;
}

static void __jump_resetvec(void)
{
	save_time(get_time());
	ramdata_set_persistent();
	emulator_reboot();
}

static void __ro_jump_resetvec(void)
{
	set_image_copy(SYSTEM_IMAGE_RO);
	__jump_resetvec();
}

static void __rw_jump_resetvec(void)
{
	set_image_copy(SYSTEM_IMAGE_RW);
	__jump_resetvec();
}

void system_pre_init(void)
{
	timestamp_t t;

	if (load_time(&t))
		force_time(t);

	ramdata_get_persistent();
	__running_copy = get_image_copy();
	if (__running_copy == SYSTEM_IMAGE_UNKNOWN) {
		__running_copy = SYSTEM_IMAGE_RO;
		system_set_reset_flags(load_reset_flags());
	}

	*(uintptr_t *)(__host_flash + CONFIG_RO_MEM_OFF + 4) =
		(uintptr_t)__ro_jump_resetvec;
	*(uintptr_t *)(__host_flash + CONFIG_RW_MEM_OFF + 4) =
		(uintptr_t)__rw_jump_resetvec;
}
