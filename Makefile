obj-m := test_rmem_transfer.o

MFLAGS := M=${CURDIR} -C ${KDIR}

ifeq ($(wildcard ${KDIR}),)
   $(error Should specify kernel build directory to $${KDIR})
endif

modules modules_install clean:
	make ${MFLAGS} $@
