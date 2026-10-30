/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _STMPE_KEYPAD_H
#define _STMPE_KEYPAD_H

#include <linux/types.h>

#define STMPE_KEYPAD_MAX_DATA 5

struct stmpe_keypad_event {
	unsigned int row;
	unsigned int col;
	unsigned int scan_code;
	bool pressed;
};

struct stmpe_keypad_event_format {
	u8 row_mask;
	u8 row_shift;
	u8 col_mask;
	u8 col_shift;
	u8 no_key_mask;
};

struct stmpe_keypad_fifo_ops {
	int (*read)(void *context, u8 *data);
	void (*report)(void *context, const u8 *data);
};

struct stmpe_keypad_lifecycle_ops {
	int (*set_scan)(void *context, bool enable);
	void (*disable)(void *context);
	void (*report_stop_error)(void *context, int error);
};

#if IS_ENABLED(CONFIG_KUNIT)
int stmpe_keypad_decode_event(u8 data,
			      const struct stmpe_keypad_event_format *format,
			      unsigned int rows, unsigned int cols,
			      unsigned int scan_code_row_shift,
			      struct stmpe_keypad_event *event);

int stmpe_keypad_drain_fifo(const struct stmpe_keypad_fifo_ops *ops,
			    void *context, unsigned int fifo_depth,
			    bool stop_on_idle);

int stmpe_keypad_start(const struct stmpe_keypad_lifecycle_ops *ops,
		       void *context);

void stmpe_keypad_stop(const struct stmpe_keypad_lifecycle_ops *ops,
		       void *context);
#endif

#endif
