obj-m += hmm.o 

# Specify the paths to the kernel headers
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)


# Define the build target
default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	gcc -o user user.c

# Define the clean target
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f user