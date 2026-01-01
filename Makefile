obj-m += sbull.o
sbull-objs := src/sbull.o

KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

clean:
$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean
rm -f *.o *.ko *.mod.c Module.symvers modules.order

install:
sudo insmod sbull.ko

uninstall:
sudo rmmod sbull.ko

test:
sudo dmesg | tail -20
