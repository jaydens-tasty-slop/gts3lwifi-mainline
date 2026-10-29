/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _WACOM_W9000_H
#define _WACOM_W9000_H

#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/input-event-codes.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#define WACOM_W9000_QUERY_MIN_LEN	9
#define WACOM_W9000_REPORT_MIN_LEN	7
#define WACOM_W9000_DEFAULT_QUERY_RETRIES	8
#define WACOM_W9000_QUERY_COMMAND	0x2a

struct wacom_w9000_features {
	u16 max_x;
	u16 max_y;
	u16 max_pressure;
	u16 fw_version;
};

struct wacom_w9000_report_format {
	u8 length;
	u8 special_packet_byte;
	bool has_distance;
};

struct wacom_w9000_report {
	u16 x;
	u16 y;
	u16 pressure;
	u8 distance;
	bool proximity;
	bool touch;
	bool rubber;
	bool side_button;
};

struct wacom_w9000_query_policy {
	u8 response_len;
	u8 retries;
	u16 response_delay_ms;
	u16 retry_delay_ms;
	bool split_transfer;
	bool require_nonzero_ranges;
};

struct wacom_w9000_query_ops {
	int (*transfer)(void *context, u8 command, u8 *data, size_t len);
	int (*send)(void *context, u8 command);
	int (*receive)(void *context, u8 *data, size_t len);
	void (*delay)(void *context, unsigned int delay_ms);
};

struct wacom_w9000_input_state {
	bool pen_proximity;
};

struct wacom_w9000_input_ops {
	void (*report_key)(void *context, unsigned int code, int value);
	void (*report_abs)(void *context, unsigned int code, int value);
	void (*report_pos)(void *context, u16 x, u16 y);
	void (*sync)(void *context);
};

struct wacom_w9000_power_state {
	bool supply_enabled;
	bool irq_enabled;
};

struct wacom_w9000_power_ops {
	int (*supply_enable)(void *context);
	int (*supply_disable)(void *context);
	void (*reset)(void *context, bool asserted);
	void (*irq_enable)(void *context);
	void (*irq_disable)(void *context);
	void (*release_input)(void *context);
	void (*startup_delay)(void *context);
	void (*shutdown_delay)(void *context);
};

static inline int wacom_w9000_xfer_result(int result, int expected)
{
	if (result == expected)
		return 0;

	return result < 0 ? result : -EIO;
}

static inline int
wacom_w9000_parse_query(const u8 *data, size_t len,
			struct wacom_w9000_features *features)
{
	if (len < WACOM_W9000_QUERY_MIN_LEN || data[0] != 0x0f)
		return -EINVAL;

	features->max_x = get_unaligned_be16(&data[1]);
	features->max_y = get_unaligned_be16(&data[3]);
	features->max_pressure = get_unaligned_be16(&data[5]);
	features->fw_version = get_unaligned_be16(&data[7]);

	return 0;
}

static inline int
wacom_w9000_query_once(const struct wacom_w9000_query_policy *policy,
		       const struct wacom_w9000_query_ops *ops, void *context,
		       u8 *data)
{
	int result;

	if (!policy->split_transfer) {
		result = ops->transfer(context, WACOM_W9000_QUERY_COMMAND,
				       data, policy->response_len);
		return wacom_w9000_xfer_result(result, 2);
	}

	result = ops->send(context, WACOM_W9000_QUERY_COMMAND);
	result = wacom_w9000_xfer_result(result, 1);
	if (result)
		return result;

	ops->delay(context, policy->response_delay_ms);
	result = ops->receive(context, data, policy->response_len);

	return wacom_w9000_xfer_result(result, policy->response_len);
}

static inline int
wacom_w9000_run_query(const struct wacom_w9000_query_policy *policy,
		      const struct wacom_w9000_query_ops *ops, void *context,
		      u8 *data, struct wacom_w9000_features *features)
{
	unsigned int retries = policy->retries ? policy->retries :
			       WACOM_W9000_DEFAULT_QUERY_RETRIES;
	int error = -EIO;
	unsigned int attempt;

	for (attempt = 0; attempt < retries; attempt++) {
		error = wacom_w9000_query_once(policy, ops, context, data);
		if (!error)
			error = wacom_w9000_parse_query(data, policy->response_len, features);
		if (!error && policy->require_nonzero_ranges &&
		    (!features->max_x || !features->max_y ||
		     !features->max_pressure))
			error = -EINVAL;
		if (!error)
			return 0;

		if (attempt + 1 < retries && policy->retry_delay_ms)
			ops->delay(context, policy->retry_delay_ms);
	}

	return error;
}

static inline int
wacom_w9000_parse_report(const u8 *data, size_t len,
			 const struct wacom_w9000_report_format *format,
			 const struct wacom_w9000_features *features,
			 struct wacom_w9000_report *report)
{
	if (len != format->length || len < WACOM_W9000_REPORT_MIN_LEN ||
	    (format->has_distance && len <= WACOM_W9000_REPORT_MIN_LEN) ||
	    (format->special_packet_byte &&
	     format->special_packet_byte >= len))
		return -EINVAL;

	*report = (struct wacom_w9000_report) {};

	if (!(data[0] & BIT(7)) ||
	    (format->special_packet_byte &&
	     data[format->special_packet_byte] & BIT(7)))
		return 0;

	report->x = get_unaligned_be16(&data[1]);
	report->y = get_unaligned_be16(&data[3]);
	report->pressure = get_unaligned_be16(&data[5]);
	if (report->x > features->max_x || report->y > features->max_y ||
	    report->pressure > features->max_pressure)
		return -EINVAL;

	report->proximity = true;
	report->touch = data[0] & BIT(4);
	report->side_button = data[0] & BIT(5);
	report->rubber = data[0] & BIT(6);
	if (format->has_distance)
		report->distance = data[7];

	return 0;
}

static inline void
wacom_w9000_release_input(struct wacom_w9000_input_state *state,
			  bool has_distance,
			  const struct wacom_w9000_input_ops *ops,
			  void *context)
{
	if (!state->pen_proximity)
		return;

	ops->report_abs(context, ABS_PRESSURE, 0);
	if (has_distance)
		ops->report_abs(context, ABS_DISTANCE, 255);
	ops->report_key(context, BTN_STYLUS, 0);
	ops->report_key(context, BTN_TOUCH, 0);
	ops->report_key(context, BTN_TOOL_PEN, 0);
	ops->report_key(context, BTN_TOOL_RUBBER, 0);
	ops->sync(context);

	state->pen_proximity = false;
}

static inline void
wacom_w9000_report_input(struct wacom_w9000_input_state *state,
			 const struct wacom_w9000_report *report,
			 bool has_distance,
			 const struct wacom_w9000_input_ops *ops,
			 void *context)
{
	if (!report->proximity) {
		wacom_w9000_release_input(state, has_distance, ops, context);
		return;
	}

	state->pen_proximity = true;
	ops->report_pos(context, report->x, report->y);
	ops->report_abs(context, ABS_PRESSURE, report->pressure);
	if (has_distance)
		ops->report_abs(context, ABS_DISTANCE, report->distance);
	ops->report_key(context, BTN_STYLUS, report->side_button);
	ops->report_key(context, BTN_TOUCH, report->touch);
	ops->report_key(context, BTN_TOOL_PEN, !report->rubber);
	ops->report_key(context, BTN_TOOL_RUBBER, report->rubber);
	ops->sync(context);
}

static inline int
wacom_w9000_power_on_transition(struct wacom_w9000_power_state *state,
				const struct wacom_w9000_power_ops *ops,
				void *context)
{
	int error;

	if (!state->supply_enabled) {
		error = ops->supply_enable(context);
		if (error)
			return error;
		state->supply_enabled = true;
		ops->startup_delay(context);
	}

	ops->reset(context, false);
	if (!state->irq_enabled) {
		ops->irq_enable(context);
		state->irq_enabled = true;
	}

	return 0;
}

static inline int
wacom_w9000_supply_off(struct wacom_w9000_power_state *state,
		       const struct wacom_w9000_power_ops *ops, void *context)
{
	int error;

	if (!state->supply_enabled)
		return 0;

	error = ops->supply_disable(context);
	if (error)
		return error;

	state->supply_enabled = false;
	ops->shutdown_delay(context);

	return 0;
}

static inline int
wacom_w9000_power_off_transition(struct wacom_w9000_power_state *state,
				 const struct wacom_w9000_power_ops *ops,
				 void *context, bool restore_on_failure)
{
	int error;

	if (state->irq_enabled) {
		ops->irq_disable(context);
		state->irq_enabled = false;
	}
	ops->release_input(context);
	ops->reset(context, true);

	if (!state->supply_enabled)
		return 0;

	error = wacom_w9000_supply_off(state, ops, context);
	if (error) {
		if (restore_on_failure) {
			ops->reset(context, false);
			if (!state->irq_enabled) {
				ops->irq_enable(context);
				state->irq_enabled = true;
			}
		}
		return error;
	}

	return 0;
}

#endif
