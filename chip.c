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
static bool enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP; /* Enable this card */

#define CARD_NAME "HiFace"

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "Index value for " CARD_NAME " soundcard.");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string for " CARD_NAME " soundcard.");
module_param_array(enable, bool, NULL, 0444);
MODULE_PARM_DESC(enable, "Enable " CARD_NAME " soundcard.");

static struct hiface_chip *chips[SNDRV_CARDS] = SNDRV_DEFAULT_PTR;

static DEFINE_MUTEX(register_mutex);

struct snd_vendor_quirk {
	const char *driver_short_name;
	u8 extra_freq;
};

static void hiface_chip_abort(struct hiface_chip *chip)
{
	if (chip) {
		if (chip->pcm)
			hiface_pcm_abort(chip);

		if (chip->card) {
			snd_card_free_when_closed(chip->card);
			chip->card = NULL;
		}
	}
}

static int hiface_dev_free(struct snd_device *device)
{
	struct hiface_chip *chip = device->device_data;
	kfree(chip);
	return 0;
}

static int hiface_chip_create(struct usb_device *device, int idx,
			      const struct snd_vendor_quirk *quirk,
			      struct hiface_chip **rchip)
{
	struct snd_card *card = NULL;
	struct hiface_chip *chip;
	int ret;
	static struct snd_device_ops ops = {
		.dev_free =	hiface_dev_free,
	};

	*rchip = NULL;

	/* if we are here, card can be registered in alsa. */
	ret = snd_card_create(index[idx], id[idx], THIS_MODULE, 0, &card);
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

	chip = kzalloc(sizeof(*chip), GFP_KERNEL);
	if (!chip) {
		snd_card_free(card);
		return -ENOMEM;
	}

	chip->dev = device;
	chip->index = idx;
	chip->card = card;

	ret = snd_device_new(card, SNDRV_DEV_LOWLEVEL, chip, &ops);
	if (ret < 0) {
			kfree(chip);
			snd_card_free(card);
			return ret;
	}

	*rchip = chip;
	return 0;
}

static int hiface_chip_probe(struct usb_interface *intf,
			     const struct usb_device_id *usb_id)
{
	const struct snd_vendor_quirk *quirk = (struct snd_vendor_quirk *)usb_id->driver_info;
	int ret;
	int i;
	struct hiface_chip *chip;
	struct usb_device *device = interface_to_usbdev(intf);

	pr_info("Probe m2-tech driver.\n");

	ret = usb_set_interface(device, 0, 0);
	if (ret != 0) {
		snd_printk(KERN_ERR "can't set first interface.\n");
		return -EIO;
	}

	/* check whether the card is already registered */
	chip = NULL;
	mutex_lock(&register_mutex);
	for (i = 0; i < SNDRV_CARDS; i++) {
		if (chips[i] && chips[i]->dev == device) {
			if (chips[i]->shutdown) {
				snd_printk(KERN_ERR "HiFace device is in the shutdown state, cannot create a card instance\n");
				ret = -ENODEV;
				goto err;
			}
			chip = chips[i];
			break;
		}
	}
	if (!chip) {
		/* it's a fresh one.
		 * now look for an empty slot and create a new card instance
		 */
		for (i = 0; i < SNDRV_CARDS; i++)
			if (enable[i] && !chips[i]) {
				ret = hiface_chip_create(device, i, quirk,
							 &chip);
				if (ret < 0)
					goto err;

				snd_card_set_dev(chip->card, &intf->dev);
				break;
			}
		if (!chip) {
			snd_printk(KERN_ERR "no available HiFace audio device\n");
			ret = -ENODEV;
			goto err;
		}
	}

	ret = hiface_pcm_init(chip, chip->card->shortname,
			      quirk ? quirk->extra_freq : 0);
	if (ret < 0)
		goto err_chip_destroy;

	ret = snd_card_register(chip->card);
	if (ret < 0) {
		snd_printk(KERN_ERR "cannot register card\n");
		goto err_chip_destroy;
	}

	chips[chip->index] = chip;
	chip->intf_count++;

	mutex_unlock(&register_mutex);

	usb_set_intfdata(intf, chip);
	return 0;

err_chip_destroy:
	snd_card_free(chip->card);
err:
	mutex_unlock(&register_mutex);
	return ret;
}

static void hiface_chip_disconnect(struct usb_interface *intf)
{
	struct hiface_chip *chip;
	struct snd_card *card;

	pr_debug("%s: called.\n", __func__);

	chip = usb_get_intfdata(intf);
	if (!chip)
		return;

	card = chip->card;
	chip->intf_count--;
	if (chip->intf_count <= 0) {
		/* Make sure that the userspace cannot create new request */
		snd_card_disconnect(card);

		mutex_lock(&register_mutex);
		chips[chip->index] = NULL;
		mutex_unlock(&register_mutex);

		chip->shutdown = true;
		hiface_chip_abort(chip);
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
