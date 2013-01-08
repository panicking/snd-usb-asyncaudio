/*
 * Linux driver for HiFace M2Tech
 *
 * PCM driver
 *
 * Author:      Michael Trimarchi <michael@amarulasolutions.com>
 * Created:     15 July 2012
 * Copyright:	(C) M2TECH S.r.l and Amarula Solutions B.V.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef HIFACE_CONTROL_H
#define HIFACE_CONTROL_H

#include "common.h"

enum {
	CONTROL_RATE_44KHZ,
	CONTROL_RATE_48KHZ,
	CONTROL_RATE_88KHZ,
	CONTROL_RATE_96KHZ,
	CONTROL_RATE_176KHZ,
	CONTROL_RATE_192KHZ,
	CONTROL_RATE_352KHZ,
	CONTROL_RATE_384KHZ,
	CONTROL_N_RATES
};

struct control_runtime {
	int (*set_rate)(struct control_runtime *rt, int rate);

	struct shiface_chip *chip;

	bool usb_streaming;
	int stored_rate;
};

int __devinit hiface_control_init(struct shiface_chip *chip);
void hiface_control_abort(struct shiface_chip *chip);
void hiface_control_destroy(struct shiface_chip *chip);
#endif /* HIFACE_CONTROL_H */
