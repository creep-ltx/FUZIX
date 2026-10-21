
CSRCS = devtty.c video.c devfd.c rtc.c
CSRCS += devices.c main.c libc.c

ASRCS = p68000.S crt0.S
ASRCS += tricks.S

DSRCS = ../../dev/devide.c ../../dev/mbr.c ../../dev/rdb.c ../../dev/blkdev.c ../../dev/devide_discard.c
DOBJS = $(patsubst ../../dev/%.c,%.o, $(DSRCS))

LSRCS = ../../lib/68000exception.c ../../lib/68000usercopy.c ../../lib/68000relocate.c
LOBJS = $(patsubst ../../lib/%.c,%.o, $(LSRCS))

COBJS = $(CSRCS:.c=$(BINEXT))
AOBJS = $(ASRCS:.S=.o)
OBJS  = $(COBJS) $(AOBJS) $(DOBJS) $(LOBJS)

CROSS_CCOPTS += -I../../dev/

all:	$(OBJS)

$(COBJS): %.o: %.c
	$(CROSS_CC) $(CROSS_CCOPTS) -c $<

$(AOBJS): %.o: %.S
	$(CROSS_AS) $(ASOPTS) $< -o $*.o

$(DOBJS): %.o: ../../dev/%.c
	$(CROSS_CC) $(CROSS_CCOPTS) -c $<

$(LOBJS): %.o: ../../lib/%.c
	$(CROSS_CC) $(CROSS_CCOPTS) -c $<

tricks.S: ../../lib/68000flat.S

clean:
	rm -f *.o fuzix.elf bootblock.elf bootblock.bin core *~

IMAGES = $(FUZIX_ROOT)/Images/$(TARGET)

image:
	$(CROSS_LD) -M -o fuzix.elf -T fuzix.ld \
	p68000.o ../../start.o ../../version.o ../../cpu-68000/lowlevel-68000.o \
	tricks.o main.o ../../timer.o ../../kdata.o 68000exception.o devices.o \
	../../devio.o ../../filesys.o ../../process.o ../../inode.o ../../syscall_fs.o \
	../../syscall_proc.o ../../syscall_other.o ../../mm.o ../../flat.o \
	../../blk512.o ../../tty.o ../../devsys.o ../../usermem.o ../../syscall_fs2.o \
	../../syscall_fs3.o ../../syscall_exec32.o ../../syscall_exec.o \
	blkdev.o devide.o devide_discard.o mbr.o rdb.o 68000relocate.o \
	68000usercopy.o ../../cpu-68000/usermem_std-68000.o devtty.o video.o devfd.o rtc.o libc.o \
	../../malloc.o ../../vt.o ../../font8x8.o \
	../../level2.o ../../syscall_level2.o ../../select.o > ../../fuzix.map
	$(CROSS_COMPILE)objcopy fuzix.elf -O binary ../../fuzix.bin
	$(CROSS_CC) -m68030 -c bootblock.S -o bootblock.o
	$(CROSS_LD) -Ttext=0 --oformat=binary -o bootblock.bin bootblock.o
	$(CROSS_CC) -m68030 -c hdboot.S -o hdboot.o
	$(CROSS_LD) -Ttext=0 --oformat=binary -o hdboot.bin hdboot.o
	mkdir -p $(IMAGES)
	python3 mkadf.py bootblock.bin ../../fuzix.bin $(IMAGES)/fuzix.adf
	python3 mkhunk.py hdboot.bin $(IMAGES)/hdboot.hunk

diskimage:
	# A 40MB disk Kickstart boots on its own: an Amiga partition table
	# with the loader in it, the kernel and file system in a FUZIX
	# partition, the rest free (as setup leaves an empty disk)
	# (its file system leaves room for the kernel in a 32 MB partition)
	cd $(FUZIX_ROOT)/Standalone/filesystem-src && ./build-filesystem-ng -X -y \
		-f $(IMAGES)/hdfs.img -g 256 65248 -p platform-amiga >/dev/null
	python3 mkrdb.py $(IMAGES)/disk.img 40 $(IMAGES)/hdfs.img \
		../../fuzix.bin $(IMAGES)/hdboot.hunk
	# The whole system on one floppy: 1560 blocks after the kernel
	cd $(FUZIX_ROOT)/Standalone/filesystem-src && ./build-filesystem-ng -X -y \
		-f $(IMAGES)/floppyfs.img -g 64 1560 -p amiga-floppy >/dev/null
	cp $(FUZIX_ROOT)/Standalone/filesystem-src/ucp-tmp.txt $(IMAGES)/floppy-ucp.txt
	python3 mkadf.py bootblock.bin ../../fuzix.bin $(IMAGES)/fuzix-floppy.adf $(IMAGES)/floppyfs.img
	# The Extras disks: the rest of the full system, as tar archives
	cd $(FUZIX_ROOT)/Standalone/filesystem-src && ./build-filesystem-ng -X -y \
		-f $(IMAGES)/fullfs.tmp -g 256 65535 -p platform-amiga >/dev/null
	cp $(FUZIX_ROOT)/Standalone/filesystem-src/ucp-tmp.txt $(IMAGES)/full-ucp.txt
	rm -f $(IMAGES)/fullfs.tmp $(IMAGES)/fuzix-extras*.adf
	python3 mkextras.py $(IMAGES)/full-ucp.txt $(IMAGES)/floppy-ucp.txt \
		$(FUZIX_ROOT)/Standalone/filesystem-src $(IMAGES)
