obj-m += quantum_os.o

quantum_os-objs := \
	quantum_main.o          \
	quantum_alloc.o         \
	quantum_preproc.o       \
	quantum_batch.o         \
	quantum_sched.o         \
	quantum_postproc.o      \
	quantum_result_store.o  \
	quantum_calib.o         \
	quantum_interface.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean