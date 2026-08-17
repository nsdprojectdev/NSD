obj-m += nsd.o

KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
PWD        := $(shell pwd)

all:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean

install:
	sudo insmod nsd.ko

remove:
	sudo rmmod nsd

test:
	@echo "NSD modulu yuklu mu?"
	@lsmod | grep nsd || echo "Yuklu degil"
	@echo "SYSFS arayuzu:"
	@cat /sys/kernel/nsd/stats 2>/dev/null || echo "Erisilemiyor"

bugcheck:
	@echo "NSD Bug Hunter (3 katmanli, deterministik)"
	@sudo sh -c "cd /root/nsd-bughunter && python3 main.py /home/nsd/Desktop/nsd-v1 --min-score 4"
