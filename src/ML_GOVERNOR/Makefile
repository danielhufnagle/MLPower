obj-m += ml_gov.o

CFLAGS_REMOVE_ml_gov.o := -mgeneral-regs-only

KBUILD_EXTRA_SYMBOLS := $(CURDIR)/../pmic_driver/Module.symvers \
                        $(CURDIR)/../data_collection/Module.symvers

all: 
	make -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) modules

clean: 
	make -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) clean 
