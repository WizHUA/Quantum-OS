obj-m += quantum_os.o

quantum_os-objs := \
    quantum_main.o \
    quantum_interface.o \
    quantum_preproc.o \
    quantum_alloc.o \
    quantum_batch.o \
    quantum_sched.o \
    quantum_postproc.o \
    quantum_calib.o \
    quantum_result_store.o