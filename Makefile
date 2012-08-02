snd-usb-asyncaudio-objs += chip.o control.o pcm.o
obj-m += snd-usb-asyncaudio.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

clean:

	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) clean
