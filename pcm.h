/*
 * Linux driver for HiFace M2Tech
 *
 * PCM driver
 *
 * Author:	Michael Trimarchi <michael@amarulasolutions.com>
 * Created:	15 July 2012
 * Copyright:	(C) Amarula Solutions B.V.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef USB6FIRE_PCM_H
#define USB6FIRE_PCM_H

#include <sound/pcm.h>
#include <linux/mutex.h>

#include "common.h"

enum /* settings for pcm */
{
	PCM_N_URBS = 8, PCM_MAX_PACKET_SIZE = 4096
};

struct pcm_urb {
	struct shiface_chip *chip;

	struct urb instance;
	struct usb_anchor submitted;
	void *buffer;
};

struct pcm_substream {
	spinlock_t lock;
	struct snd_pcm_substream *instance;

	bool active;
	snd_pcm_uframes_t dma_off;	/* current position in alsa dma_area */
        snd_pcm_uframes_t period_off;	/* current position in current period */
};

struct pcm_runtime {
	struct shiface_chip *chip;
	struct snd_pcm *instance;

	struct pcm_substream playback;
	bool panic; /* if set driver won't do anymore pcm on device */

	struct pcm_urb out_urbs[PCM_N_URBS];

	struct mutex stream_mutex;
	u8 stream_state; /* one of STREAM_XXX (pcm.c) */
	u8 rate; /* one of PCM_RATE_XXX */
	wait_queue_head_t stream_wait_queue;
	bool stream_wait_cond;
};

int __devinit hiface_pcm_init(struct shiface_chip *chip);
void hiface_pcm_abort(struct shiface_chip *chip);
void hiface_pcm_destroy(struct shiface_chip *chip);
#endif /* USB6FIRE_PCM_H */
