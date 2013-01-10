/*
 * Linux driver for M2Tech HiFace
 *
 * Author:	Michael Trimarchi <michael@amarulasolutions.com>
 * Created:	June 01, 2012
 * Copyright:	(C) M2TECH S.r.l and Amarula Solutions B.V.
 *
 * The driver is based on the work done in TerraTec DMX 6Fire USB
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "chip.h"
#include "pcm.h"
#include "control.h"

#include <linux/moduleparam.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/gfp.h>
#include <sound/initval.h>

MODULE_AUTHOR("Michael Trimarchi <michael@amarulasolutions.com>");
MODULE_DESCRIPTION("M2Tech HiFace USB audio driver");
MODULE_LICENSE("GPL v2");
MODULE_SUPPORTED_DEVICE("{{HiFace, Evo}}");

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX; /* Index 0-max */
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR; /* Id for card */
static struct hiface_chip *chips[SNDRV_CARDS] = SNDRV_DEFAULT_PTR;
static struct usb_device *devices[SNDRV_CARDS] = SNDRV_DEFAULT_PTR;

static DEFINE_MUTEX(register_mutex);

struct snd_vendor_quirk {
	const char *driver_short_name;
	u8 extra_freq;
};

static void hiface_chip_abort(struct hiface_chip *chip)
{
	if (chip) {
		/* Make sure that the userspace cannot create new request */
		if (chip->card)
			snd_card_disconnect(chip->card);

		if (chip->pcm)
			hiface_pcm_abort(chip);

		if (chip->control)
			hiface_control_abort(chip);

		if (chip->card) {
			snd_card_free_when_closed(chip->card);
			chip->card = NULL;
		}
	}
}

static void hiface_chip_destroy(struct hiface_chip *chip)
{
	if (chip) {
		if (chip->pcm)
			hiface_pcm_destroy(chip);
		if (chip->control)
			hiface_control_destroy(chip);
		if (chip->card)
			snd_card_free(chip->card);
	}
}

static int __devinit hiface_chip_probe(struct usb_interface *intf,
		const struct usb_device_id *usb_id)
{
	const struct snd_vendor_quirk *quirk = (struct snd_vendor_quirk *)usb_id->driver_info;
	int ret;
	int i;
	struct hiface_chip *chip = NULL;
	struct usb_device *device = interface_to_usbdev(intf);
	int regidx = -1; /* index in module parameter array */
	struct snd_card *card = NULL;

	pr_info("Probe m2-tech driver.\n");

	/* look if we already serve this card and return if so */
	mutex_lock(&register_mutex);
	for (i = 0; i < SNDRV_CARDS; i++) {
		if (devices[i] == device) {
			if (chips[i])
				chips[i]->intf_count++;
			usb_set_intfdata(intf, chips[i]);
			mutex_unlock(&register_mutex);
			return 0;
		} else if (regidx < 0)
			regidx = i;
	}
	if (regidx < 0) {
		mutex_unlock(&register_mutex);
		snd_printk(KERN_ERR "too many cards registered.\n");
		return -ENODEV;
	}
	devices[regidx] = device;
	mutex_unlock(&register_mutex);

	/* if we are here, card can be registered in alsa. */
	if (usb_set_interface(device, 0, 0) != 0) {
		snd_printk(KERN_ERR "can't set first interface.\n");
		return -EIO;
	}
	ret = snd_card_create(index[regidx], id[regidx], THIS_MODULE,
			sizeof(struct hiface_chip), &card);
	if (ret < 0) {
		snd_printk(KERN_ERR "cannot create alsa card.\n");
		return ret;
	}

	strcpy(card->driver, "snd-async-audio");

	if (quirk && quirk->driver_short_name) {
		strcpy(card->shortname, quirk->driver_short_name);
	} else {
		strcpy(card->shortname, "M2Tech generic audio");
	}

	sprintf(card->longname, "%s at %d:%d", card->shortname,
			device->bus->busnum, device->devnum);
	snd_card_set_dev(card, &intf->dev);

	chip = card->private_data;
	chips[regidx] = chip;
	chip->dev = device;
	chip->regidx = regidx;
	chip->intf_count = 1;
	chip->card = card;

	ret = hiface_pcm_init(chip, card->shortname,
			      quirk ? quirk->extra_freq : 0);
	if (ret < 0) {
		hiface_chip_destroy(chip);
		return ret;
	}

	ret = hiface_control_init(chip);
	if (ret < 0) {
		hiface_chip_destroy(chip);
		return ret;
	}

	ret = snd_card_register(card);
	if (ret < 0) {
		snd_printk(KERN_ERR "cannot register card\n");
		hiface_chip_destroy(chip);
		return ret;
	}
	usb_set_intfdata(intf, chip);
	return 0;
}

static void hiface_chip_disconnect(struct usb_interface *intf)
{
	struct hiface_chip *chip;
	struct snd_card *card;

	chip = usb_get_intfdata(intf);
	if (chip) { /* if !chip, fw upload has been performed */
		card = chip->card;
		chip->intf_count--;
		if (!chip->intf_count) {
			mutex_lock(&register_mutex);
			devices[chip->regidx] = NULL;
			chips[chip->regidx] = NULL;
			mutex_unlock(&register_mutex);

			chip->shutdown = true;
			hiface_chip_abort(chip);
			hiface_chip_destroy(chip);
		}
	}
}

static struct usb_device_id device_table[] = {
	{
		USB_DEVICE(0x04b4, 0x930b),
		.driver_info = (unsigned long)&(const struct snd_vendor_quirk) {
			.driver_short_name = "hiFace",
		}
	},
	{
		USB_DEVICE(0x04b4, 0x0384),
		.driver_info = (unsigned long)&(const struct snd_vendor_quirk) {
			.driver_short_name = "Young",
			.extra_freq = 1,
		}
	},
	{
		USB_DEVICE(0x04b4, 0x931E),
		.driver_info = (unsigned long)&(const struct snd_vendor_quirk) {
			.driver_short_name = "AUDIA",
		}
	},
	{
		USB_DEVICE(0x04b4, 0x931D),
		.driver_info = (unsigned long)&(const struct snd_vendor_quirk) {
			.driver_short_name = "Corrson",
		}
	},
	{
		USB_DEVICE(0x04b4, 0x9320),
		.driver_info = (unsigned long)&(const struct snd_vendor_quirk) {
			.driver_short_name = "Empirical",
		}
	},
	{
		USB_DEVICE(0x04b4, 0x931B),
		.driver_info = (unsigned long)&(const struct snd_vendor_quirk) {
			.driver_short_name = "North Star",
		}
	},
	{
		USB_DEVICE(0x04b4, 0x9321),
		.driver_info = (unsigned long)&(const struct snd_vendor_quirk) {
			.driver_short_name = "Rockna",
		}
	},
	{
		USB_DEVICE(0x04b4, 0x931F),
		.driver_info = (unsigned long)&(const struct snd_vendor_quirk) {
			.driver_short_name = "SL Audio",
		}
	},
	{
		USB_DEVICE(0x04b4, 0x931C),
		.driver_info = (unsigned long)&(const struct snd_vendor_quirk) {
			.driver_short_name = "W4S Young",
		}
	},
	{
		USB_DEVICE(0x245F, 0x931C),
		.driver_info = (unsigned long)&(const struct snd_vendor_quirk) {
			.driver_short_name = "CHORD",
		}
	},
	{
		USB_DEVICE(0x25C6, 0x9002),
		.driver_info = (unsigned long)&(const struct snd_vendor_quirk) {
			.driver_short_name = "Vitus",
		}
	},
	{
		USB_DEVICE(0x249C, 0x9006),
		.driver_info = (unsigned long)&(const struct snd_vendor_quirk) {
			.driver_short_name = "CAD",
		}
	},
	{
		USB_DEVICE(0x249C, 0x932C),
		.driver_info = (unsigned long)&(const struct snd_vendor_quirk) {
			.driver_short_name = "Eeaudio",
		}
	},
	{
		USB_DEVICE(0x249C, 0x9002),
		.driver_info = (unsigned long)&(const struct snd_vendor_quirk) {
			.driver_short_name = "Metronome",
		}
	},
	{
		USB_DEVICE(0x249C, 0x9001),
		.driver_info = (unsigned long)&(const struct snd_vendor_quirk) {
			.driver_short_name = "Pathos",
		}
	},
	{
		USB_DEVICE(0x249C, 0x931C),
		.driver_info = (unsigned long)&(const struct snd_vendor_quirk) {
			.driver_short_name = "Rotel",
		}
	},
	{
		USB_DEVICE(0x249C, 0x9008),
		.driver_info = (unsigned long)&(const struct snd_vendor_quirk) {
			.driver_short_name = "Audio Esclusive",
		}
	},
	{}
};

MODULE_DEVICE_TABLE(usb, device_table);

static struct usb_driver snd_usb_driver = {
	.name = "snd-usb-hiface",
	.probe = hiface_chip_probe,
	.disconnect = hiface_chip_disconnect,
	.id_table = device_table,
};
#if 0
module_usb_driver(snd_usb_driver);
#else
static int __init snd_module_init(void)
{
	return usb_register(&snd_usb_driver);
}

static void __exit snd_module_exit(void)
{
	usb_deregister(&snd_usb_driver);
}

module_init(snd_module_init)
module_exit(snd_module_exit)
#endif
