/*
 * Linux driver for HiFace M2Tech
 *
 * Mixer control
 *
 * Author:      Michael Trimarchi <michael@amarulasolutions.com>
 * Created:     15 July 2012
 * Copyright:   (C) Amarula Solutions B.V.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/interrupt.h>
#include <sound/control.h>
#include <sound/tlv.h>

#include "control.h"
#include "chip.h"

static __u16 rate_value[] = { 0x43, 0x4b, 0x42, 0x4a, 0x40, 0x48 };

static int hiface_control_set_rate(struct control_runtime *rt, int rate)
{
	int ret;
	struct usb_device *device = rt->chip->dev;

	if (rate < 0 || rate >= CONTROL_N_RATES)
		return -EINVAL;

	printk(KERN_INFO "%s: set rate %d\n", __func__, rate);

	ret = usb_set_interface(device, 0, 0);
	if (ret < 0)
		return ret;

#if 0
	USBIO: Vendor 0xb0(wValue=0x0043, wIndex=0x0000)
		43 b0 43 00 00 00 00 00
	USBIO: Vendor 0xb0(wValue=0x004b, wIndex=0x0000)
		43 b0 4b 00 00 00 00 00
#endif
	ret = usb_control_msg(device, usb_sndctrlpipe(device, 0),
				0x43, 0xb0,
				rate_value[rate], 0, NULL, 0, 100);
	return 0;
}

static int hiface_control_streaming_update(struct control_runtime *rt)
{
	return 0;
}

int __devinit hiface_control_init(struct shiface_chip *chip)
{
	struct control_runtime *rt = kzalloc(sizeof(struct control_runtime),
			GFP_KERNEL);

	if (!rt)
		return -ENOMEM;

	rt->chip = chip;
	rt->update_streaming = hiface_control_streaming_update;
	rt->set_rate = hiface_control_set_rate;

	hiface_control_streaming_update(rt);

	chip->control = rt;
	return 0;
}

void hiface_control_abort(struct shiface_chip *chip)
{

}

void hiface_control_destroy(struct shiface_chip *chip)
{
	kfree(chip->control);
	chip->control = NULL;
}
