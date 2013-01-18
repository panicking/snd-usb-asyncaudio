/*
 * Linux driver for HiFace M2Tech
 *
 * PCM driver
 *
 * Author:	Michael Trimarchi <michael@amarulasolutions.com>
 * Created:	15 July 2012
 * Copyright:	(C) M2TECH S.r.l and Amarula Solutions B.V.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef HIFACE_PCM_H
#define HIFACE_PCM_H

#include "common.h"

int __devinit hiface_pcm_init(struct hiface_chip *chip,
			      const char *pcm_stream_name,
			      u8 extra_freq);
void hiface_pcm_abort(struct hiface_chip *chip);
void hiface_pcm_destroy(struct hiface_chip *chip);
#endif /* HIFACE_PCM_H */
