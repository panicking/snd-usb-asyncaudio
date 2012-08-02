/*
 * Linux driver for M2Tech HiFace
 *
 * Author:	Michael Trimarchi <michael@amarulasolutions.com>
 * Created:	June 01, 2012
 * Copyright:	(C) Amarula Solutions
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
static struct shiface_chip *chips[SNDRV_CARDS] = SNDRV_DEFAULT_PTR;
static struct usb_device *devices[SNDRV_CARDS] = SNDRV_DEFAULT_PTR;

static DEFINE_MUTEX(register_mutex);

static void hiface_chip_abort(struct shiface_chip *chip)
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

static void hiface_chip_destroy(struct shiface_chip *chip)
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
	int ret;
	int i;
	struct shiface_chip *chip = NULL;
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
		pr_err("too many cards registered.\n");
		return -ENODEV;
	}
	devices[regidx] = device;
	mutex_unlock(&register_mutex);

	/* if we are here, card can be registered in alsa. */
	if (usb_set_interface(device, 0, 0) != 0) {
		pr_err("can't set first interface.\n");
		return -EIO;
	}
	ret = snd_card_create(index[regidx], id[regidx], THIS_MODULE,
			sizeof(struct shiface_chip), &card);
	if (ret < 0) {
		pr_err("cannot create alsa card.\n");
		return ret;
	}
	strcpy(card->driver, "M2Tech-Hiface");
	strcpy(card->shortname, "M2Tech HIFACE");
	sprintf(card->longname, "%s at %d:%d", card->shortname,
			device->bus->busnum, device->devnum);
	snd_card_set_dev(card, &intf->dev);

	chip = card->private_data;
	chips[regidx] = chip;
	chip->dev = device;
	chip->regidx = regidx;
	chip->intf_count = 1;
	chip->card = card;

	ret = hiface_pcm_init(chip);
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
		pr_err("cannot register card\n");
		hiface_chip_destroy(chip);
		return ret;
	}
	usb_set_intfdata(intf, chip);
	return 0;
}

static void hiface_chip_disconnect(struct usb_interface *intf)
{
	struct shiface_chip *chip;
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
		.match_flags = USB_DEVICE_ID_MATCH_DEVICE,
		.idVendor = 0x04b4,
		.idProduct = 0x930b
	},
	{
		.match_flags = USB_DEVICE_ID_MATCH_DEVICE,
		.idVendor = 0x04b4,
		.idProduct = 0x0384

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
