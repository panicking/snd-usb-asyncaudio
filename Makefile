snd-usb-hiface-objs += chip.o pcm.o
obj-m += snd-usb-hiface.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
default:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) modules

clean:

	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) clean
