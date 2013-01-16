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

struct control_runtime {
	int (*set_rate)(struct control_runtime *rt, int rate);

	struct hiface_chip *chip;

	bool usb_streaming;
	int stored_rate;
};

int __devinit hiface_control_init(struct hiface_chip *chip);
void hiface_control_destroy(struct hiface_chip *chip);
#endif /* HIFACE_CONTROL_H */
