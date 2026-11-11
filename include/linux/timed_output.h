/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Android timed_output class, as used by the stock vibrator HAL through
 * /sys/class/timed_output/<name>/enable.
 *
 * Copyright (C) 2008 Google, Inc.
 */

#ifndef _LINUX_TIMED_OUTPUT_H
#define _LINUX_TIMED_OUTPUT_H

struct device;

struct timed_output_dev {
	const char	*name;

	/*
	 * Enable the output and set the timer, @timeout in ms (0 or less
	 * turns it off). May be called from atomic context.
	 */
	void (*enable)(struct timed_output_dev *sdev, int timeout);

	/* Returns the number of milliseconds remaining on the timer */
	int (*get_time)(struct timed_output_dev *sdev);

	/* private data */
	struct device	*dev;
	int		index;
	int		state;
};

int timed_output_dev_register(struct timed_output_dev *dev);
void timed_output_dev_unregister(struct timed_output_dev *dev);

#endif
