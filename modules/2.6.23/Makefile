KSRC=/lib/modules/`uname -r`/build

obj-m += scubed2mtd.o

modules:
	@make modules -C $(KSRC) M=`pwd`

modules_install:
	@make modules_install -C $(KSRC) M=`pwd`

clean:
	@make clean -C $(KSRC) M=`pwd`
