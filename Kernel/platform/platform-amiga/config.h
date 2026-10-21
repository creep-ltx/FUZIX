/*
 *	Commodore Amiga (68000-68030, OCS/ECS/AGA), flat memory, no MMU.
 *	First target: a stock A1200 (68EC020, 2MB chip RAM, Gayle IDE).
 */

/* Enable to make ^Z dump the inode table for debug */
#undef CONFIG_IDUMP
/* Enable to make ^A drop back into the monitor */
#undef CONFIG_MONITOR
/* Profil syscall support (not yet complete) */
#undef CONFIG_PROFIL

#define CONFIG_32BIT
#define CONFIG_LEVEL_2

#define CONFIG_MULTI
#define CONFIG_FLAT
#define CONFIG_SPLIT_ID
#define CONFIG_PARENT_FIRST
/* It's not that meaningful but we currently chunk to 512 bytes */
#define CONFIG_BANKS 	(65536/512)

#define CONFIG_LARGE_IO_DIRECT(x)	1

#define CONFIG_SPLIT_UDATA
#define UDATA_SIZE	1024
#define UDATA_BLKS	2

/* Vertical blank: 50Hz on PAL, 60Hz on NTSC (we assume PAL) */
#define TICKSPERSEC 50

/* Video terminal on tty1, Paula serial on tty2 */
#define CONFIG_VT
#define CONFIG_FONT8X8
#define VT_WIDTH	80
#define VT_HEIGHT	32
#define VT_RIGHT	79
#define VT_BOTTOM	31

#define BOOT_TTY (512 + 1)	/* tty1: the screen */

#define CMDLINE	NULL
/* Root file system: the boot floppy's Fuzix partition if it has one,
   else the first partition of the IDE disk (set by device_init) */
#ifndef __ASSEMBLER__
extern unsigned short amiga_root_dev;
#endif
#define BOOTDEVICE	amiga_root_dev

/* Device parameters */
#define NUM_DEV_TTY 2
#define TTYDEV   BOOT_TTY /* Device used by kernel for messages, panics */

#define NBUFS    16       /* Number of block buffers */
#define NMOUNTS	 8	  /* Number of mounts at a time */

#define MAX_BLKDEV 3

#define CONFIG_IDE
/* LBA, or cylinder/head/sector for drives without it */
#define CONFIG_IDE_AUTO
/* Amiga partition tables (RDB) as well as PC ones */
#define CONFIG_RDB

/* Battery-backed clock at $DC0000 (MSM6242 or RP5C01), if there is one:
   /dev/rtc, read by setdate at boot */
#define CONFIG_RTC
#define CONFIG_RTC_FULL
#define CONFIG_RTC_INTERVAL	255

#define plt_copyright()

#define PTABSIZE	64
#define UFTSIZE		16
#define OFTSIZE		160
#define ITABSIZE	176

#define BOOTDEVICENAMES "hd#"

#define TTY_INIT_BAUD	B9600
