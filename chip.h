/*
 * Linux driver for M2Tech HiFace
 *
 * Author:      Michael Trimarchi <michael@amarulasolutions.com>
 * Created:     June 01, 2012
 * Copyright:	(C) M2TECH S.r.l and Amarula Solutions B.V.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef HIFACE_CHIP_H
#define HIFACE_CHIP_H

#include "common.h"

struct hiface_chip {
	struct usb_device *dev;
	struct snd_card *card;
	int intf_count; /* number of registered interfaces */
	int regidx; /* index in module parameter arrays */
	bool shutdown;

	struct pcm_runtime *pcm;
	struct control_runtime *control;
};
#endif /* HIFACE_CHIP_H */
