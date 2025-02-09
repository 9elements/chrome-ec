/* Copyright 2019 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "common.h"
#include "ec_commands.h"
#include "fpsensor.h"
#include "fpsensor_private.h"
#include "fpsensor_state.h"
#include "host_command.h"
#include "system.h"
#include "task.h"
#include "util.h"

/* Last acquired frame (aligned as it is used by arbitrary binary libraries) */
uint8_t fp_buffer[FP_SENSOR_IMAGE_SIZE] FP_FRAME_SECTION __aligned(4);
/* Fingers templates for the current user */
uint8_t fp_template[FP_MAX_FINGER_COUNT][FP_ALGORITHM_TEMPLATE_SIZE]
	FP_TEMPLATE_SECTION;
/* Encryption/decryption buffer */
/* TODO: On-the-fly encryption/decryption without a dedicated buffer */
/*
 * Store the encryption metadata at the beginning of the buffer containing the
 * ciphered data.
 */
uint8_t fp_enc_buffer[FP_ALGORITHM_ENCRYPTED_TEMPLATE_SIZE]
	FP_TEMPLATE_SECTION;
/* Number of used templates */
uint32_t templ_valid;
/* Bitmap of the templates with local modifications */
uint32_t templ_dirty;
/* Current user ID */
uint32_t user_id[FP_CONTEXT_USERID_WORDS];
/* Part of the IKM used to derive encryption keys received from the TPM. */
uint8_t tpm_seed[FP_CONTEXT_TPM_BYTES];
/* Status of the FP encryption engine. */
static uint32_t fp_encryption_status;

uint32_t fp_events;

uint32_t sensor_mode;

void fp_task_simulate(void)
{
	int timeout_us = -1;

	while (1)
		task_wait_event(timeout_us);
}

void fp_clear_finger_context(int idx)
{
	memset(fp_template[idx], 0, sizeof(fp_template[0]));
}

void fp_clear_context(void)
{
	int idx;

	templ_valid = 0;
	templ_dirty = 0;
	memset(fp_buffer, 0, sizeof(fp_buffer));
	memset(fp_enc_buffer, 0, sizeof(fp_enc_buffer));
	memset(user_id, 0, sizeof(user_id));
	for (idx = 0; idx < FP_MAX_FINGER_COUNT; idx++)
		fp_clear_finger_context(idx);
	/* TODO maybe shutdown and re-init the private libraries ? */
}

int fp_get_next_event(uint8_t *out)
{
	uint32_t event_out = atomic_read_clear(&fp_events);

	memcpy(out, &event_out, sizeof(event_out));

	return sizeof(event_out);
}
DECLARE_EVENT_SOURCE(EC_MKBP_EVENT_FINGERPRINT, fp_get_next_event);

static int fp_command_tpm_seed(struct host_cmd_handler_args *args)
{
	const struct ec_params_fp_seed *params = args->params;

	if (params->struct_version != FP_TEMPLATE_FORMAT_VERSION) {
		CPRINTS("Invalid seed format %d", params->struct_version);
		return EC_RES_INVALID_PARAM;
	}

	if (fp_encryption_status & FP_ENC_STATUS_SEED_SET) {
		CPRINTS("Seed has already been set.");
		return EC_RES_ACCESS_DENIED;
	}
	memcpy(tpm_seed, params->seed, sizeof(tpm_seed));
	fp_encryption_status |= FP_ENC_STATUS_SEED_SET;

	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_FP_SEED, fp_command_tpm_seed, EC_VER_MASK(0));

int fp_tpm_seed_is_set(void)
{
	return fp_encryption_status & FP_ENC_STATUS_SEED_SET;
}

static int fp_command_encryption_status(struct host_cmd_handler_args *args)
{
	struct ec_response_fp_encryption_status *r = args->response;

	r->valid_flags = FP_ENC_STATUS_SEED_SET;
	r->status = fp_encryption_status;
	args->response_size = sizeof(*r);

	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_FP_ENC_STATUS, fp_command_encryption_status,
		     EC_VER_MASK(0));

static int validate_fp_mode(const uint32_t mode)
{
	uint32_t capture_type = FP_CAPTURE_TYPE(mode);
	uint32_t algo_mode = mode & ~FP_MODE_CAPTURE_TYPE_MASK;
	uint32_t cur_mode = sensor_mode;

	if (capture_type >= FP_CAPTURE_TYPE_MAX)
		return EC_ERROR_INVAL;

	if (algo_mode & ~FP_VALID_MODES)
		return EC_ERROR_INVAL;

	/* Don't allow sensor reset if any other mode is
	 * set (including FP_MODE_RESET_SENSOR itself).
	 */
	if (mode & FP_MODE_RESET_SENSOR) {
		if (cur_mode & FP_VALID_MODES)
			return EC_ERROR_INVAL;
	}

	return EC_SUCCESS;
}

static int fp_command_mode(struct host_cmd_handler_args *args)
{
	const struct ec_params_fp_mode *p = args->params;
	struct ec_response_fp_mode *r = args->response;
	int ret;

	ret = validate_fp_mode(p->mode);
	if (ret != EC_SUCCESS) {
		CPRINTS("Invalid FP mode 0x%x", p->mode);
		return EC_RES_INVALID_PARAM;
	}

	if (!(p->mode & FP_MODE_DONT_CHANGE)) {
		sensor_mode = p->mode;
		task_set_event(TASK_ID_FPSENSOR, TASK_EVENT_UPDATE_CONFIG, 0);
	}

	r->mode = sensor_mode;
	args->response_size = sizeof(*r);
	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_FP_MODE, fp_command_mode, EC_VER_MASK(0));

static int fp_command_context(struct host_cmd_handler_args *args)
{
	const struct ec_params_fp_context *params = args->params;

	fp_clear_context();

	memcpy(user_id, params->userid, sizeof(user_id));

	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_FP_CONTEXT, fp_command_context, EC_VER_MASK(0));
