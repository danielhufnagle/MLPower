

KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

MODULE_NAME ?= pmic_driver
obj-m := $(MODULE_NAME).o

all:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean

load: all
	sudo insmod $(MODULE_NAME).ko

unload:
	sudo rmmod $(MODULE_NAME)

reload: unload load

dmesg:
	sudo dmesg | tail -20

info:
	modinfo $(MODULE_NAME).ko

.PHONY: all clean load unload reload dmesg info
