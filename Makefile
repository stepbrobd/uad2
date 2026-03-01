PWD  := $(CURDIR)
KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: modules modules_install clean
modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: default install
default: modules
install: modules_install
