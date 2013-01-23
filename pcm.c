/*
 * Linux driver for HiFace M2Tech
 *
 * PCM driver
 *
 * Author:	Michael Trimarchi <michael@amarulasolutions.com>
 * Created:	15 July 2012
 * Copyright:	(C) M2TECH S.r.l and Amarula Solutions B.V.
 *
 * The driver is based on the work done in TerraTec DMX 6Fire USB
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <sound/pcm.h>

#include "pcm.h"
#include "chip.h"
#include "control.h"

#define PCM_N_URBS		8
#define PCM_MAX_PACKET_SIZE	4096

struct pcm_urb {
	struct hiface_chip *chip;

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
	struct hiface_chip *chip;
	struct snd_pcm *instance;

	struct pcm_substream playback;
	bool panic; /* if set driver won't do anymore pcm on device */

	struct pcm_urb out_urbs[PCM_N_URBS];

	struct mutex stream_mutex;
	u8 stream_state; /* one of STREAM_XXX (pcm.c) */
	u8 rate; /* one of PCM_RATE_XXX */
	u8 extra_freq;
	wait_queue_head_t stream_wait_queue;
	bool stream_wait_cond;
};

static const int rates[] = { 44100, 48000, 88200, 96000, 176400, 192000,
			     352800, 384000 };
static const int rates_alsaid[] = {
	SNDRV_PCM_RATE_44100, SNDRV_PCM_RATE_48000,
	SNDRV_PCM_RATE_88200, SNDRV_PCM_RATE_96000,
	SNDRV_PCM_RATE_176400, SNDRV_PCM_RATE_192000,
	SNDRV_PCM_RATE_KNOT, SNDRV_PCM_RATE_KNOT };

#define OUT_EP		2
#define MAX_BUFSIZE	(2 * PCM_N_URBS * PCM_MAX_PACKET_SIZE)

enum { /* pcm streaming states */
	STREAM_DISABLED, /* no pcm streaming */
	STREAM_STARTING, /* pcm streaming requested, waiting to become ready */
	STREAM_RUNNING, /* pcm streaming running */
	STREAM_STOPPING
};

static inline void swap_word(u8 *dest, u8 *orig)
{
	u16 *src = (u16 *)orig, *dst = (u16 *)dest;
	*(dst + 1) = *src;
	*dst = *(src + 1);
}

static const struct snd_pcm_hardware pcm_hw = {
	.info = SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_PAUSE |
		SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_BATCH,

	.formats = SNDRV_PCM_FMTBIT_S32_LE,

	.rates = SNDRV_PCM_RATE_44100 |
		SNDRV_PCM_RATE_48000 |
		SNDRV_PCM_RATE_88200 |
		SNDRV_PCM_RATE_96000 |
		SNDRV_PCM_RATE_176400 |
		SNDRV_PCM_RATE_192000 |
		SNDRV_PCM_RATE_KNOT,

	.rate_min = 44100,
	.rate_max = 192000, /* changes in hiface_pcm_open to support extra rates */
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = MAX_BUFSIZE,
	.period_bytes_min = PCM_MAX_PACKET_SIZE,
	.period_bytes_max = MAX_BUFSIZE,
	.periods_min = 2,
	.periods_max = 1024
};

static int hiface_pcm_set_rate(struct pcm_runtime *rt)
{
	int ret;
	struct control_runtime *ctrl_rt = rt->chip->control;

	ret = ctrl_rt->set_rate(ctrl_rt, rt->rate);
	if (ret < 0) {
		snd_printk(KERN_ERR "Error setting samplerate %d.\n",
				rates[rt->rate]);
		return ret;
	}

	return 0;
}

static struct pcm_substream *hiface_pcm_get_substream(
		struct snd_pcm_substream *alsa_sub)
{
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);

	if (alsa_sub->stream == SNDRV_PCM_STREAM_PLAYBACK)
		return &rt->playback;

	pr_debug("Error getting pcm substream slot.\n");
	return NULL;
}

/* call with stream_mutex locked */
static void hiface_pcm_stream_stop(struct pcm_runtime *rt)
{
	int i, time;

	if (rt->stream_state != STREAM_DISABLED) {
		rt->stream_state = STREAM_STOPPING;

		for (i = 0; i < PCM_N_URBS; i++) {
			time = usb_wait_anchor_empty_timeout(
					&rt->out_urbs[i].submitted, 100);
			if (!time)
				usb_kill_anchored_urbs(
					&rt->out_urbs[i].submitted);
			usb_kill_urb(&rt->out_urbs[i].instance);
		}

		rt->stream_state = STREAM_DISABLED;
	}
}

/* call with stream_mutex locked */
static int hiface_pcm_stream_start(struct pcm_runtime *rt)
{
	int ret = 0, i;

	if (rt->stream_state == STREAM_DISABLED) {
		/* submit our out urbs zero init */
		rt->stream_state = STREAM_STARTING;
		for (i = 0; i < PCM_N_URBS; i++) {
			memset(&rt->out_urbs[i].buffer, PCM_MAX_PACKET_SIZE, 0);
			usb_anchor_urb(&rt->out_urbs[i].instance,
				       &rt->out_urbs[i].submitted);
			ret = usb_submit_urb(&rt->out_urbs[i].instance,
					GFP_ATOMIC);
			if (ret) {
				hiface_pcm_stream_stop(rt);
				return ret;
			}
		}

		/* wait for first out urb to return (sent in in urb handler) */
		wait_event_timeout(rt->stream_wait_queue, rt->stream_wait_cond,
				HZ);
		if (rt->stream_wait_cond) {
			pr_debug("%s: Stream is running wakeup event\n",
				 __func__);
			rt->stream_state = STREAM_RUNNING;
		} else {
			hiface_pcm_stream_stop(rt);
			return -EIO;
		}
	}
	return ret;
}


/* call with substream locked */
static int hiface_pcm_playback(struct pcm_substream *sub,
		struct pcm_urb *urb)
{
	struct snd_pcm_runtime *alsa_rt = sub->instance->runtime;
	u8 *dest, *source;
	unsigned int stride;
	int i;

	stride = alsa_rt->frame_bits / 8;

	if (alsa_rt->format == SNDRV_PCM_FORMAT_S32_LE)
		dest = (u8 *)urb->buffer;
	else {
		pr_err("Unknown sample format\n");
		return -EINVAL;
	}

	if (sub->dma_off + PCM_MAX_PACKET_SIZE <=
		(alsa_rt->buffer_size * stride)) {
		pr_debug("%s: (1) buffer_size %x dma_offset %x\n", __func__,
			 (unsigned int) alsa_rt->buffer_size * stride,
			 (unsigned int) sub->dma_off);

		source = (u8 *)(alsa_rt->dma_area + sub->dma_off);
		for (i = 0; i < PCM_MAX_PACKET_SIZE; i += 4)
			swap_word(dest + i, source + i);
	} else {
		/* wrap around at end of ring buffer */
		unsigned int len = alsa_rt->buffer_size * stride - sub->dma_off;
		source = (u8 *)(alsa_rt->dma_area + sub->dma_off);

		pr_debug("%s: (2) buffer_size %x dma_offset %x\n", __func__,
			 (unsigned int) alsa_rt->buffer_size * stride,
			 (unsigned int) sub->dma_off);

		for (i = 0; i < len; i += 4)
			swap_word(dest + i, source + i);

		source = (u8 *)alsa_rt->dma_area;

		for (i = 0; i < PCM_MAX_PACKET_SIZE - len; i += 4)
			swap_word(dest + len + i, source + i);
	}
	sub->dma_off += PCM_MAX_PACKET_SIZE;
	if (sub->dma_off >= (alsa_rt->buffer_size * stride))
		sub->dma_off -= (alsa_rt->buffer_size * stride);

	sub->period_off += PCM_MAX_PACKET_SIZE;

	return 0;
}

static void hiface_pcm_out_urb_handler(struct urb *usb_urb)
{
	struct pcm_urb *out_urb = usb_urb->context;
	struct pcm_runtime *rt = out_urb->chip->pcm;
	struct pcm_substream *sub;
	unsigned long flags;

	pr_debug("%s: func\n", __func__);

	if (usb_urb->status || rt->panic || rt->stream_state == STREAM_STOPPING)
		return;

	if (rt->stream_state == STREAM_STARTING) {
		rt->stream_wait_cond = true;
		wake_up(&rt->stream_wait_queue);
	}

	/* now send our playback data (if a free out urb was found) */
	sub = &rt->playback;
	spin_lock_irqsave(&sub->lock, flags);
	if (sub->active) {
		int ret = 0;
		ret = hiface_pcm_playback(sub, out_urb);
		if (ret) {
			spin_unlock_irqrestore(&sub->lock, flags);
			goto out_fail;
		}
		if (sub->period_off >= sub->instance->runtime->period_size) {
			sub->period_off %= sub->instance->runtime->period_size;
			spin_unlock_irqrestore(&sub->lock, flags);
			snd_pcm_period_elapsed(sub->instance);
		} else {
			spin_unlock_irqrestore(&sub->lock, flags);
		}
	} else {
		memset(out_urb->buffer, 0, PCM_MAX_PACKET_SIZE);
		spin_unlock_irqrestore(&sub->lock, flags);
	}
out_fail:
	usb_submit_urb(&out_urb->instance, GFP_ATOMIC);
}

static int hiface_pcm_open(struct snd_pcm_substream *alsa_sub)
{
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	struct pcm_substream *sub = NULL;
	struct snd_pcm_runtime *alsa_rt = alsa_sub->runtime;

	if (rt->panic)
		return -EPIPE;

	pr_debug("%s: func\n", __func__);

	mutex_lock(&rt->stream_mutex);
	alsa_rt->hw = pcm_hw;

	if (alsa_sub->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		if (rt->rate < ARRAY_SIZE(rates))
			alsa_rt->hw.rates = rates_alsaid[rt->rate];
		sub = &rt->playback;
	}

	if (!sub) {
		mutex_unlock(&rt->stream_mutex);
		pr_err("Invalid stream type\n");
		return -EINVAL;
	}

	if (rt->extra_freq)
		alsa_rt->hw.rate_max = 384000;

	sub->instance = alsa_sub;
	sub->active = false;
	mutex_unlock(&rt->stream_mutex);
	return 0;
}

static int hiface_pcm_close(struct snd_pcm_substream *alsa_sub)
{
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	struct pcm_substream *sub = hiface_pcm_get_substream(alsa_sub);
	unsigned long flags;

	if (rt->panic)
		return 0;

	pr_debug("%s: func\n", __func__);

	mutex_lock(&rt->stream_mutex);
	if (sub) {
		/* deactivate substream */
		spin_lock_irqsave(&sub->lock, flags);
		sub->instance = NULL;
		sub->active = false;
		spin_unlock_irqrestore(&sub->lock, flags);

		/* all substreams closed? if so, stop streaming */
		if (!rt->playback.instance) {
			hiface_pcm_stream_stop(rt);
			rt->rate = ARRAY_SIZE(rates);
		}
	}
	mutex_unlock(&rt->stream_mutex);
	return 0;
}

static int hiface_pcm_hw_params(struct snd_pcm_substream *alsa_sub,
		struct snd_pcm_hw_params *hw_params)
{
	pr_debug("%s: func\n", __func__);
	return snd_pcm_lib_malloc_pages(alsa_sub,
			params_buffer_bytes(hw_params));
}

static int hiface_pcm_hw_free(struct snd_pcm_substream *alsa_sub)
{
	pr_debug("%s: func\n", __func__);
	return snd_pcm_lib_free_pages(alsa_sub);
}

static int hiface_pcm_prepare(struct snd_pcm_substream *alsa_sub)
{
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	struct pcm_substream *sub = hiface_pcm_get_substream(alsa_sub);
	struct snd_pcm_runtime *alsa_rt = alsa_sub->runtime;
	int ret;

	pr_debug("%s: func\n", __func__);

	if (rt->panic)
		return -EPIPE;
	if (!sub)
		return -ENODEV;

	mutex_lock(&rt->stream_mutex);

	sub->dma_off = 0;
	sub->period_off = 0;

	if (rt->stream_state == STREAM_DISABLED) {
		for (rt->rate = 0; rt->rate < ARRAY_SIZE(rates); rt->rate++)
			if (alsa_rt->rate == rates[rt->rate])
				break;
		if (rt->rate == ARRAY_SIZE(rates)) {
			mutex_unlock(&rt->stream_mutex);
			pr_err("Invalid rate %d in prepare\n",
					alsa_rt->rate);
			return -EINVAL;
		}

		ret = hiface_pcm_set_rate(rt);
		if (ret) {
			mutex_unlock(&rt->stream_mutex);
			return ret;
		}
		ret = hiface_pcm_stream_start(rt);
		if (ret) {
			mutex_unlock(&rt->stream_mutex);
			return ret;
		}
	}
	mutex_unlock(&rt->stream_mutex);
	return 0;
}

static int hiface_pcm_trigger(struct snd_pcm_substream *alsa_sub, int cmd)
{
	struct pcm_substream *sub = hiface_pcm_get_substream(alsa_sub);
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	unsigned long flags;

	pr_debug("%s: func\n", __func__);

	if (rt->panic)
		return -EPIPE;
	if (!sub)
		return -ENODEV;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		spin_lock_irqsave(&sub->lock, flags);
		sub->active = true;
		spin_unlock_irqrestore(&sub->lock, flags);
		return 0;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		spin_lock_irqsave(&sub->lock, flags);
		sub->active = false;
		spin_unlock_irqrestore(&sub->lock, flags);
		return 0;

	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t hiface_pcm_pointer(
		struct snd_pcm_substream *alsa_sub)
{
	struct pcm_substream *sub = hiface_pcm_get_substream(alsa_sub);
	struct pcm_runtime *rt = snd_pcm_substream_chip(alsa_sub);
	unsigned long flags;
	snd_pcm_uframes_t ret;

	if (rt->panic || !sub)
		return SNDRV_PCM_STATE_XRUN;


	spin_lock_irqsave(&sub->lock, flags);
	ret = sub->dma_off;
	spin_unlock_irqrestore(&sub->lock, flags);
	return ret / (alsa_sub->runtime->frame_bits / 8);
}

static struct snd_pcm_ops pcm_ops = {
	.open = hiface_pcm_open,
	.close = hiface_pcm_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = hiface_pcm_hw_params,
	.hw_free = hiface_pcm_hw_free,
	.prepare = hiface_pcm_prepare,
	.trigger = hiface_pcm_trigger,
	.pointer = hiface_pcm_pointer,
};

static int hiface_pcm_init_urb(struct pcm_urb *urb,
			       struct hiface_chip *chip,
			       int ep,
			       void (*handler)(struct urb *))
{
	urb->chip = chip;
	usb_init_urb(&urb->instance);

	urb->buffer = kzalloc(PCM_MAX_PACKET_SIZE, GFP_KERNEL);
	if (!urb->buffer)
		return -ENOMEM;

	usb_fill_bulk_urb(&urb->instance, chip->dev,
			  usb_sndbulkpipe(chip->dev, ep), (void *)urb->buffer,
			  PCM_MAX_PACKET_SIZE, handler, urb);
	init_usb_anchor(&urb->submitted);

	return 0;
}

int hiface_pcm_init(struct hiface_chip *chip,
		    const char *pcm_stream_name,
		    u8 extra_freq)
{
	int i;
	int ret;
	struct snd_pcm *pcm;
	struct pcm_runtime *rt;

	rt = kzalloc(sizeof(struct pcm_runtime), GFP_KERNEL);
	if (!rt)
		return -ENOMEM;

	rt->chip = chip;
	rt->stream_state = STREAM_DISABLED;
	rt->rate = ARRAY_SIZE(rates);
	if (extra_freq)
		rt->extra_freq = 1;

	init_waitqueue_head(&rt->stream_wait_queue);
	mutex_init(&rt->stream_mutex);
	spin_lock_init(&rt->playback.lock);

	for (i = 0; i < PCM_N_URBS; i++)
		hiface_pcm_init_urb(&rt->out_urbs[i], chip, OUT_EP,
				hiface_pcm_out_urb_handler);

	if (pcm_stream_name)
		ret = snd_pcm_new(chip->card, pcm_stream_name, 0, 1, 0, &pcm);
	else
		ret = snd_pcm_new(chip->card, "HiFace M2Tech", 0, 1, 0, &pcm);
	if (ret < 0) {
		kfree(rt);
		pr_err("Cannot create pcm instance\n");
		return ret;
	}

	pcm->private_data = rt;
	strcpy(pcm->name, "AsyncAudio USB");
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &pcm_ops);

	snd_pcm_lib_preallocate_pages_for_all(pcm,
					SNDRV_DMA_TYPE_CONTINUOUS,
					snd_dma_continuous_data(GFP_KERNEL),
					MAX_BUFSIZE, MAX_BUFSIZE);
	rt->instance = pcm;

	chip->pcm = rt;
	return 0;
}

void hiface_pcm_abort(struct hiface_chip *chip)
{
	struct pcm_runtime *rt = chip->pcm;

	if (rt) {
		rt->panic = true;

		if (rt->playback.instance) {
			snd_pcm_stop(rt->playback.instance,
					SNDRV_PCM_STATE_XRUN);
		}
		mutex_lock(&rt->stream_mutex);
		hiface_pcm_stream_stop(rt);
		mutex_unlock(&rt->stream_mutex);
	}
}

void hiface_pcm_destroy(struct hiface_chip *chip)
{
	struct pcm_runtime *rt = chip->pcm;
	int i;

	for (i = 0; i < PCM_N_URBS; i++)
		kfree(rt->out_urbs[i].buffer);

	kfree(chip->pcm);
	chip->pcm = NULL;
}
